/* scenarios_server.c - 옵션이 다른 서버를 따로 띄우는 시나리오 (l) --max-conns, (m) --stats-interval. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "scenarios.h"
#include "server_proc.h"
#include "wire.h"

#define GAME_PONG 1

const char *g_itest_log_prefix = "integration_test";

/* 시나리오 몸통(REQUIRE로 빠져나올 수 있음)을 돌리고, 빠져나왔든 아니든 서버를 내려 확인한다.
 * absent가 NULL이 아니면 서버를 내린 뒤 로그에 그 글자가 없어야 한다. */
static void with_server(const char *tag, const char *const args[], void (*body)(server_proc *),
                        const char *absent)
{
    char log_path[4096];
    snprintf(log_path, sizeof log_path, "%s-%s-server.log", g_itest_log_prefix, tag);
    server_proc s;
    REQUIRE(server_start(&s, g_itest_server_path, args, log_path) == 0);
    g_itest_port = s.port; /* itest_main이 시나리오가 끝나면 되돌린다 */
    body(&s);
    REQUIRE(server_stop(&s));
    REQUIRE(server_log_clean(log_path));
    REQUIRE(absent == NULL || !server_log_has(log_path, absent));
}

static void sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* ---- (l) ---- */

static void max_conns_body(server_proc *s)
{
    (void)s;
    client c[3], x, y;
    msg m;
    for (int i = 0; i < 3; i++)
        REQUIRE(client_connect(&c[i], 0) == 0);

    /* 4번째: accept는 되지만 WELCOME 대신 ERROR(SERVER_FULL)을 받고 닫힌다(5.4절) */
    REQUIRE(client_connect(&x, 0) == -1 && x.fd != -1);
    REQUIRE(x.welcome.type == MSG_ERROR && x.welcome.len >= 2);
    REQUIRE(wire_get_u16(x.welcome.p) == ERR_SERVER_FULL);
    REQUIRE(client_wait_closed(&x, T_LONG, NULL));
    client_close(&x);

    /* 있던 접속은 그대로 쓸 수 있다: PING, 두 사람이 QUICK_JOIN으로 경기까지 */
    for (int i = 0; i < 3; i++) {
        send_u32(&c[i], MSG_PING, 300u + (uint32_t)i);
        REQUIRE(client_wait_for(&c[i], MSG_PONG, &m, T_LONG) && wire_get_u32(m.p) == 300u + (uint32_t)i);
    }
    send_u16(&c[0], MSG_QUICK_JOIN, GAME_PONG);
    REQUIRE(client_wait_for(&c[0], MSG_ROOM_JOINED, &m, T_LONG));
    send_u16(&c[1], MSG_QUICK_JOIN, GAME_PONG);
    REQUIRE(client_wait_for(&c[1], MSG_ROOM_JOINED, &m, T_LONG));
    REQUIRE(client_wait_for(&c[0], MSG_PLAYER_JOINED, &m, T_LONG));
    REQUIRE(client_wait_for(&c[0], MSG_MATCH_START, &m, T_LONG));
    REQUIRE(client_wait_for(&c[1], MSG_MATCH_START, &m, T_LONG));

    /* --no-udp: UDP_BIND에 답이 없어(2초) TCP만 쓰고, STATE는 TCP로 온다 */
    REQUIRE(!client_udp_bind(&c[0]));
    client *cs[2] = {&c[0], &c[1]};
    pong_log logs[2];
    REQUIRE(pump_pong(cs, logs, 2, 500, 0));
    REQUIRE(logs[0].states >= 5 && logs[1].states >= 5);

    /* 하나가 나가면 자리가 난다. 서버가 끊김을 처리하기 전이면 SERVER_FULL이라 잠시 다시 해 본다. */
    client_quit(&c[2]);
    int ok = 0;
    for (uint64_t end = itest_now_ms() + T_LONG; !ok && itest_now_ms() < end;) {
        ok = client_connect(&y, 0) == 0;
        if (!ok) {
            client_close(&y);
            sleep_ms(50);
        }
    }
    REQUIRE(ok);
    client_quit(&y);
    client_quit(&c[0]);
    client_quit(&c[1]);
}

void scenario_l_max_conns(void)
{
    static const char *const args[] = {"--max-conns", "3", "--no-udp", NULL};
    with_server("l", args, max_conns_body, NULL);
}

/* ---- (m) ---- */

/* 서버가 쓰는 필드 전부(stats.c report). 빠진 것 · 모르는 것 모두 실패다. */
static const char *const k_fields[] = {
    "ts_ms", "interval_ms", "conns", "rooms", "matches", "udp_bound", "ticks", "ticks_dropped",
    "tick_samples", "tick_us_p50", "tick_us_p99", "tick_us_max", "loop_samples", "loop_us_p99",
    "tcp_rx_bytes", "tcp_tx_bytes", "udp_rx_bytes", "udp_tx_bytes", "tcp_rx_frames",
    "tcp_tx_frames", "udp_rx_datagrams", "udp_tx_datagrams", "udp_tx_dropped", "conns_rejected",
    "rss_kb", "cpu_user_ms", "cpu_sys_ms",
};
#define NFIELDS (sizeof k_fields / sizeof k_fields[0])

/* {"이름":부호 없는 정수,...} 꼴의 JSON 한 줄을 엄격히 읽어 k_fields 순서의 값으로 넣는다.
 * 모든 필드가 정확히 한 번 나오면 1. */
static int parse_stats(const char *s, unsigned long long v[NFIELDS])
{
    int seen[NFIELDS] = {0};
    if (*s++ != '{')
        return 0;
    for (;;) {
        if (*s++ != '"')
            return 0;
        const char *name = s;
        while (*s != '\0' && *s != '"')
            s++;
        size_t nlen = (size_t)(s - name);
        if (*s++ != '"' || *s++ != ':' || *s < '0' || *s > '9')
            return 0;
        char *end;
        unsigned long long x = strtoull(s, &end, 10);
        s = end;
        size_t k = 0;
        while (k < NFIELDS && (strlen(k_fields[k]) != nlen || strncmp(k_fields[k], name, nlen) != 0))
            k++;
        if (k == NFIELDS || seen[k]) {
            fprintf(stderr, "stats: unknown or repeated field %.*s\n", (int)nlen, name);
            return 0;
        }
        seen[k] = 1;
        v[k] = x;
        if (*s == ',') {
            s++;
            continue;
        }
        if (*s++ != '}' || *s != '\0')
            return 0;
        break;
    }
    for (size_t k = 0; k < NFIELDS; k++) {
        if (!seen[k]) {
            fprintf(stderr, "stats: missing field %s\n", k_fields[k]);
            return 0;
        }
    }
    return 1;
}

static unsigned long long field(const unsigned long long v[NFIELDS], const char *name)
{
    for (size_t k = 0; k < NFIELDS; k++) {
        if (strcmp(k_fields[k], name) == 0)
            return v[k];
    }
    return 0;
}

static void stats_body(server_proc *s)
{
    client a, b;
    REQUIRE(udp_pair(&a, &b, 1, 0, NULL, 0)); /* a는 UDP, b는 TCP만 */
    client *cs[2] = {&a, &b};
    ulog L[2];
    /* 보고는 서버 시작 + n초. 2.6초 돌리면 마지막 보고의 1초 구간은 통째로 경기 중이다. */
    REQUIRE(udp_pump(cs, L, 2, 2600, 0));
    char line[2048], last[2048] = "";
    int lines = 0;
    while (server_read_line(s, line, sizeof line, 0) == 1) {
        snprintf(last, sizeof last, "%s", line);
        lines++;
    }
    REQUIRE(lines >= 2);
    unsigned long long v[NFIELDS];
    if (!parse_stats(last, v))
        fprintf(stderr, "stats line: %s\n", last);
    REQUIRE(parse_stats(last, v));
#define F(name) field(v, name)
    REQUIRE(F("ts_ms") > 1600000000000ull);
    REQUIRE(F("interval_ms") >= 900 && F("interval_ms") <= 1500);
    REQUIRE(F("conns") == 2 && F("rooms") == 1 && F("matches") == 1 && F("udp_bound") == 1);
    /* 30Hz로 1초: 30번 언저리(ASan · 스케줄 흔들림 감안) */
    REQUIRE(F("ticks") >= 20 && F("ticks") <= 40);
    REQUIRE(F("tick_samples") >= 1 && F("tick_samples") <= F("ticks"));
    REQUIRE(F("tick_us_p50") <= F("tick_us_p99") && F("tick_us_p99") <= F("tick_us_max"));
    REQUIRE(F("tick_us_max") < 1000000ull);
    REQUIRE(F("loop_samples") >= F("tick_samples") && F("loop_us_p99") < 1000000ull);
    /* TCP: b의 GAME_IN(헤더 4 + 5)만 들어오고 b의 STATE(4 + 20)만 나간다 */
    REQUIRE(F("tcp_rx_frames") >= 20 && F("tcp_rx_bytes") == 9 * F("tcp_rx_frames"));
    REQUIRE(F("tcp_tx_frames") >= 20 && F("tcp_tx_bytes") == 24 * F("tcp_tx_frames"));
    /* UDP: a의 묶음 n = 4(토큰 4 + 헤더 4 + 21)가 들어오고 a의 STATE가 나간다 */
    REQUIRE(F("udp_rx_datagrams") >= 20 && F("udp_rx_bytes") == 29 * F("udp_rx_datagrams"));
    REQUIRE(F("udp_tx_datagrams") >= 20 && F("udp_tx_bytes") == 24 * F("udp_tx_datagrams"));
    REQUIRE(F("udp_tx_dropped") == 0 && F("conns_rejected") == 0);
    REQUIRE(F("rss_kb") > 0);
#undef F
    client_quit(&a);
    client_quit(&b);
    sleep_ms(200); /* 서버가 두 끊김을 처리한 뒤 내려야 --quiet 확인에 뜻이 있다 */
}

void scenario_m_stats(void)
{
    static const char *const args[] = {"--stats-interval", "1", "--quiet", NULL};
    /* --quiet: 서버를 내린 뒤 로그에 연결 종료 줄이 없다 */
    with_server("m", args, stats_body, "closed:");
}
