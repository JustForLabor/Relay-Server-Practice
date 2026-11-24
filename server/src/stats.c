/* stats.c - 통계 집계와 JSON 한 줄 보고. 설명은 stats.h. */
#include "stats.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "conn.h"
#include "room.h"
#include "tick.h"

stats_counters g_stats;
bool g_quiet_logs;

/* 로그-선형 히스토그램(µs). 0–63µs는 1µs 칸, 그 위는 2의 거듭제곱 구간마다 32칸이라 칸 폭이
 * 값의 1/32 이하다(분위수 상대 오차 약 3% 이하). 표본을 쌓아 정렬하지 않으므로 기록이 O(1)이고
 * 표본 수가 늘어도 메모리가 그대로다. 2^40µs(약 12일)를 넘는 값은 마지막 구간에 넣는다. */
#define HIST_LINEAR 64
#define HIST_SUB 32
#define HIST_MIN_EXP 6
#define HIST_MAX_EXP 40
#define HIST_BUCKETS (HIST_LINEAR + (HIST_MAX_EXP - HIST_MIN_EXP + 1) * HIST_SUB)

typedef struct hist {
    uint32_t bucket[HIST_BUCKETS];
    uint64_t n;
    uint64_t max;
} hist;

static unsigned g_interval_sec;
static uint64_t g_interval_ns;
static uint64_t g_period_start_ns; /* 이번 구간 시작 */
static uint64_t g_next_report_ns;
static int g_statm_fd = -1;
static long g_page_kb;
static uint64_t g_cpu_user_us, g_cpu_sys_us; /* 직전 보고 때 누적 CPU 시간 */
static hist g_tick_hist, g_loop_hist;

static unsigned hist_index(uint64_t us)
{
    if (us < HIST_LINEAR)
        return (unsigned)us;
    unsigned k = 63u - (unsigned)__builtin_clzll(us);
    if (k > HIST_MAX_EXP) {
        k = HIST_MAX_EXP;
        us = (UINT64_C(1) << (HIST_MAX_EXP + 1)) - 1;
    }
    unsigned sub = (unsigned)(us >> (k - 5)) & (HIST_SUB - 1);
    return HIST_LINEAR + (k - HIST_MIN_EXP) * HIST_SUB + sub;
}

/* 칸의 대표값: 칸 가운데. 실제 최댓값보다 크게 말하지 않도록 max로 자른다. */
static uint64_t hist_value(unsigned idx, uint64_t max)
{
    uint64_t v;
    if (idx < HIST_LINEAR) {
        v = idx;
    } else {
        unsigned j = idx - HIST_LINEAR;
        unsigned k = j / HIST_SUB + HIST_MIN_EXP;
        uint64_t lo = (uint64_t)(HIST_SUB + j % HIST_SUB) << (k - 5);
        v = lo + ((UINT64_C(1) << (k - 5)) >> 1);
    }
    return v < max ? v : max;
}

static void hist_add(hist *h, uint64_t ns)
{
    uint64_t us = ns / 1000u;
    h->bucket[hist_index(us)]++;
    h->n++;
    if (us > h->max)
        h->max = us;
}

/* permille번째 분위수(500 = p50, 990 = p99). 표본이 없으면 0. */
static uint64_t hist_quantile(const hist *h, unsigned permille)
{
    if (h->n == 0)
        return 0;
    uint64_t target = (h->n * permille + 999u) / 1000u;
    if (target == 0)
        target = 1;
    uint64_t seen = 0;
    for (unsigned i = 0; i < HIST_BUCKETS; i++) {
        seen += h->bucket[i];
        if (seen >= target)
            return hist_value(i, h->max);
    }
    return h->max;
}

static void cpu_now_us(uint64_t *user, uint64_t *sys)
{
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) {
        *user = *sys = 0;
        return;
    }
    *user = (uint64_t)ru.ru_utime.tv_sec * 1000000u + (uint64_t)ru.ru_utime.tv_usec;
    *sys = (uint64_t)ru.ru_stime.tv_sec * 1000000u + (uint64_t)ru.ru_stime.tv_usec;
}

void stats_init(unsigned interval_sec, uint64_t now)
{
    g_interval_sec = interval_sec;
    if (interval_sec == 0)
        return;
    g_interval_ns = (uint64_t)interval_sec * NS_PER_SEC;
    g_period_start_ns = now;
    g_next_report_ns = now + g_interval_ns;
    /* 보고 때마다 열면 fd가 바닥난 순간 RSS를 못 읽는다. 처음에 열어 두고 pread로 다시 읽는다. */
    g_statm_fd = open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
    long page = sysconf(_SC_PAGESIZE);
    g_page_kb = page > 0 ? page / 1024 : 4;
    cpu_now_us(&g_cpu_user_us, &g_cpu_sys_us);
}

void stats_shutdown(void)
{
    if (g_statm_fd != -1)
        close(g_statm_fd);
    g_statm_fd = -1;
    g_interval_sec = 0;
}

bool stats_enabled(void)
{
    return g_interval_sec != 0;
}

void stats_record_tick_ns(uint64_t ns)
{
    hist_add(&g_tick_hist, ns);
}

void stats_record_loop_ns(uint64_t ns)
{
    hist_add(&g_loop_hist, ns);
}

int stats_timeout_ms(uint64_t now)
{
    return g_interval_sec == 0 ? -1 : tick_ms_until(g_next_report_ns, now);
}

/* statm 두 번째 값(상주 페이지 수) → KB. 못 읽으면 0. */
static uint64_t rss_kb(void)
{
    if (g_statm_fd == -1)
        return 0;
    char buf[128];
    ssize_t n = pread(g_statm_fd, buf, sizeof buf - 1, 0);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    unsigned long long size, resident;
    if (sscanf(buf, "%llu %llu", &size, &resident) != 2)
        return 0;
    return (uint64_t)resident * (uint64_t)g_page_kb;
}

static uint64_t wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void report(uint64_t now)
{
    uint64_t user, sys;
    cpu_now_us(&user, &sys);
    const stats_counters *s = &g_stats;
    printf("{\"ts_ms\":%" PRIu64 ",\"interval_ms\":%" PRIu64 ",\"conns\":%zu,\"rooms\":%zu,"
           "\"matches\":%zu,\"udp_bound\":%zu,\"ticks\":%" PRIu64 ",\"ticks_dropped\":%" PRIu64
           ",\"tick_samples\":%" PRIu64 ",\"tick_us_p50\":%" PRIu64 ",\"tick_us_p99\":%" PRIu64
           ",\"tick_us_max\":%" PRIu64 ",\"loop_samples\":%" PRIu64 ",\"loop_us_p99\":%" PRIu64
           ",\"tcp_rx_bytes\":%" PRIu64 ",\"tcp_tx_bytes\":%" PRIu64 ",\"udp_rx_bytes\":%" PRIu64
           ",\"udp_tx_bytes\":%" PRIu64 ",\"tcp_rx_frames\":%" PRIu64 ",\"tcp_tx_frames\":%" PRIu64
           ",\"udp_rx_datagrams\":%" PRIu64 ",\"udp_tx_datagrams\":%" PRIu64
           ",\"udp_tx_dropped\":%" PRIu64 ",\"conns_rejected\":%" PRIu64 ",\"rss_kb\":%" PRIu64
           ",\"cpu_user_ms\":%" PRIu64 ",\"cpu_sys_ms\":%" PRIu64 "}\n",
           wall_ms(), (now - g_period_start_ns) / NS_PER_MS, conn_count(), rooms_count(),
           rooms_match_count(), conn_udp_bound_count(), s->ticks, s->ticks_dropped,
           g_tick_hist.n, hist_quantile(&g_tick_hist, 500), hist_quantile(&g_tick_hist, 990),
           g_tick_hist.max, g_loop_hist.n, hist_quantile(&g_loop_hist, 990), s->tcp_rx_bytes,
           s->tcp_tx_bytes, s->udp_rx_bytes, s->udp_tx_bytes, s->tcp_rx_frames, s->tcp_tx_frames,
           s->udp_rx_datagrams, s->udp_tx_datagrams, s->udp_tx_dropped, s->conns_rejected,
           rss_kb(), (user - g_cpu_user_us) / 1000u, (sys - g_cpu_sys_us) / 1000u);
    /* 파이프로 읽는 측정 도구가 줄을 바로 받게 한다(파이프면 stdout은 줄 버퍼가 아니다). */
    fflush(stdout);
    g_cpu_user_us = user;
    g_cpu_sys_us = sys;
}

void stats_maybe_report(uint64_t now)
{
    if (g_interval_sec == 0 || now < g_next_report_ns)
        return;
    report(now);
    memset(&g_stats, 0, sizeof g_stats);
    memset(&g_tick_hist, 0, sizeof g_tick_hist);
    memset(&g_loop_hist, 0, sizeof g_loop_hist);
    g_period_start_ns = now;
    /* 보고 시각은 시작 + n × 간격으로 둔다. 서버가 한 간격 넘게 멈췄으면 지난 칸은 건너뛴다. */
    do
        g_next_report_ns += g_interval_ns;
    while (g_next_report_ns <= now);
}
