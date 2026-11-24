/* main.c - loadgen: Pong 봇 N개를 한 프로세스 · 한 스레드 epoll로 돌려 지연을 잰다.
 *
 *   loadgen --bots N [--host H] [--port P] [--mode tcp|udp] [--duration S] [--ramp S]
 *           [--warmup S] [--out result.json] [--seed X] [--win-score W]
 *
 * 시간 구간: 시작 → ramp(접속을 고르게 나눠 엶) → warmup(마지막 짝이 경기에 들어감)
 * → duration(측정 구간). 지연 · 간격 · 개수는 측정 구간 것만 센다. 접속 · 경기 수는 전체. */
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "lg.h"

#define MAX_EVENTS 1024
#define S_TO_NS(s) ((uint64_t)((s) * 1e9))

loadgen g;
static volatile sig_atomic_t g_stop;

uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t unix_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(void)
{
    fprintf(stderr, "usage: loadgen --bots N [--host H] [--port P] [--mode tcp|udp] [--duration S]\n"
                    "               [--ramp S] [--warmup S] [--out FILE] [--seed X] [--win-score W]\n");
}

static int parse_num(const char *s, double lo, double hi, double *out)
{
    char *end;
    double v = strtod(s, &end);
    if (*s == '\0' || *end != '\0' || v < lo || v > hi)
        return -1;
    *out = v;
    return 0;
}

static int parse_args(int argc, char **argv, config *c)
{
    double v;
    c->host = "127.0.0.1";
    c->port = 7777;
    c->mode = MODE_TCP;
    c->duration_s = 60;
    c->ramp_s = 5;
    c->warmup_s = 2;
    c->out = "result.json";
    c->seed = 1;
    c->win_score = 21;
    for (int i = 1; i < argc; i++) {
        const char *k = argv[i];
        const char *a = i + 1 < argc ? argv[i + 1] : NULL;
        if (a == NULL)
            return -1;
        i++;
        if (strcmp(k, "--host") == 0) {
            c->host = a;
        } else if (strcmp(k, "--port") == 0 && parse_num(a, 1, 65535, &v) == 0) {
            c->port = (uint16_t)v;
        } else if (strcmp(k, "--bots") == 0 && parse_num(a, 1, 1000000, &v) == 0) {
            c->bots = (uint32_t)v;
        } else if (strcmp(k, "--mode") == 0 && (strcmp(a, "tcp") == 0 || strcmp(a, "udp") == 0)) {
            c->mode = strcmp(a, "udp") == 0 ? MODE_UDP : MODE_TCP;
        } else if (strcmp(k, "--duration") == 0 && parse_num(a, 0.1, 86400, &v) == 0) {
            c->duration_s = v;
        } else if (strcmp(k, "--ramp") == 0 && parse_num(a, 0, 3600, &v) == 0) {
            c->ramp_s = v;
        } else if (strcmp(k, "--warmup") == 0 && parse_num(a, 0, 3600, &v) == 0) {
            c->warmup_s = v;
        } else if (strcmp(k, "--out") == 0) {
            c->out = a;
        } else if (strcmp(k, "--seed") == 0 && parse_num(a, 0, 4294967295.0, &v) == 0) {
            c->seed = (uint32_t)v;
        } else if (strcmp(k, "--win-score") == 0 && parse_num(a, 1, 21, &v) == 0) {
            c->win_score = (uint8_t)v;
        } else {
            return -1;
        }
    }
    if (c->bots == 0)
        return -1;
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res;
    if (getaddrinfo(c->host, NULL, &hints, &res) != 0) {
        fprintf(stderr, "loadgen: cannot resolve %s\n", c->host);
        return -1;
    }
    memcpy(&c->addr, res->ai_addr, sizeof c->addr);
    c->addr.sin_port = htons(c->port);
    freeaddrinfo(res);
    return 0;
}

/* 봇마다 fd가 1개(udp면 2개) 필요하다. 소프트 상한을 하드 상한까지 올린다. */
static void raise_nofile(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
    uint64_t need = (uint64_t)g.cfg.bots * (g.cfg.mode == MODE_UDP ? 2 : 1) + 16;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < need)
        fprintf(stderr, "loadgen: nofile %llu < needed %llu; extra bots will fail (socket)\n",
                (unsigned long long)rl.rlim_cur, (unsigned long long)need);
}

static double cpu_s(const struct timeval *tv)
{
    return (double)tv->tv_sec + (double)tv->tv_usec / 1e6;
}

static void dispatch(const struct epoll_event *ev)
{
    bot *b = &g.bots[ev->data.u64 >> 1];
    if (b->state == B_DEAD)
        return;
    if (ev->data.u64 & 1)
        bot_on_udp(b);
    else
        bot_on_tcp(b, ev->events);
}

int main(int argc, char **argv)
{
    if (parse_args(argc, argv, &g.cfg) != 0) {
        usage();
        return 2;
    }
    raise_nofile();
    /* 입력을 정확히 1/30초마다 보내려면 깨어나는 시각이 늦지 않아야 한다. 기본 여유 50µs를 1µs로. */
    prctl(PR_SET_TIMERSLACK, 1000UL, 0, 0, 0);
    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    g.epfd = epoll_create1(EPOLL_CLOEXEC);
    g.bots = calloc(g.cfg.bots, sizeof *g.bots);
    if (g.epfd < 0 || g.bots == NULL) {
        perror("loadgen init");
        return 1;
    }
    g.t0 = now_ns();
    uint64_t ramp = S_TO_NS(g.cfg.ramp_s);
    for (uint32_t i = 0; i < g.cfg.bots; i++) {
        bot *b = &g.bots[i];
        b->fd = b->ufd = -1;
        b->idx = i;
        b->state = B_IDLE;
        /* xorshift는 0에서 멈추므로 0이 되지 않게 섞는다 */
        b->rng = (g.cfg.seed * 2654435761u) ^ ((i + 1) * 2246822519u);
        if (b->rng == 0)
            b->rng = 0x9E3779B9u;
        b->connect_at = g.t0 + ramp * i / g.cfg.bots;
        if (timer_push(b->connect_at, i, T_CONNECT) != 0) {
            perror("loadgen timers");
            return 1;
        }
    }
    g.win_start = g.t0 + ramp + S_TO_NS(g.cfg.warmup_s);
    g.win_end = g.win_start + S_TO_NS(g.cfg.duration_s);

    struct epoll_event evs[MAX_EVENTS];
    struct rusage ru0 = {0}, ru1;
    uint64_t win_real_start = 0, loops = 0, fired = 0;
    for (;;) {
        uint64_t now = now_ns();
        if (!g.measuring && now >= g.win_start) {
            g.measuring = 1;
            win_real_start = now;
            g.win_unix_ms[0] = unix_ms();
            getrusage(RUSAGE_SELF, &ru0);
        }
        if (now >= g.win_end || g_stop)
            break;
        uint64_t when;
        while (timer_peek(&when) && when <= now) {
            uint32_t idx;
            uint8_t kind;
            timer_pop(&when, &idx, &kind);
            bot_on_timer(&g.bots[idx], kind, when);
            fired++;
        }
        uint64_t next = g.measuring ? g.win_end : g.win_start;
        if (timer_peek(&when) && when < next)
            next = when;
        now = now_ns();
        uint64_t wait = next > now ? next - now : 0;
        struct timespec ts = {.tv_sec = (time_t)(wait / 1000000000ull),
                              .tv_nsec = (long)(wait % 1000000000ull)};
        int n = epoll_pwait2(g.epfd, evs, MAX_EVENTS, &ts, NULL);
        if (n < 0 && errno != EINTR) {
            perror("epoll_pwait2");
            break;
        }
        for (int i = 0; i < n; i++)
            dispatch(&evs[i]);
        loops++;
    }
    uint64_t end = now_ns();
    g.win_unix_ms[1] = unix_ms();
    getrusage(RUSAGE_SELF, &ru1);

    for (uint32_t i = 0; i < g.cfg.bots; i++) {
        bot *b = &g.bots[i];
        if (!b->welcomed && b->state != B_DEAD && b->state != B_IDLE) {
            g.st.conn_fail++;
            g.st.fail[F_NO_WELCOME]++;
        }
        bot_close(b);
    }
    double window = g.measuring ? (double)(end - win_real_start) / 1e9 : 0.0;
    double cu = g.measuring ? cpu_s(&ru1.ru_utime) - cpu_s(&ru0.ru_utime) : 0.0;
    double cs = g.measuring ? cpu_s(&ru1.ru_stime) - cpu_s(&ru0.ru_stime) : 0.0;
    int rc = report_write(g.cfg.out, window, cu, cs, ru1.ru_maxrss, loops, fired);
    fprintf(stderr,
            "loadgen: bots=%u ok=%llu fail=%llu matches=%llu states=%llu lat_p50=%.1fms "
            "lat_p99=%.1fms rtt_p50=%.2fms tool_cpu=%.0f%% -> %s\n",
            g.cfg.bots, (unsigned long long)g.st.conn_ok, (unsigned long long)g.st.conn_fail,
            (unsigned long long)g.st.matches_started, (unsigned long long)g.st.states_rx,
            hist_quantile_ms(&g.st.lat, 0.5), hist_quantile_ms(&g.st.lat, 0.99),
            hist_quantile_ms(&g.st.rtt, 0.5), window > 0 ? 100.0 * (cu + cs) / window : 0.0,
            g.cfg.out);
    free(g.bots);
    timers_free();
    close(g.epfd);
    return rc;
}
