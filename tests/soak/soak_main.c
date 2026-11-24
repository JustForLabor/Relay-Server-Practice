/* soak_main.c - 접속 · 경기 · 끊기를 오래 되풀이해 서버 메모리(RSS)가 늘어나는지, 끝날 때 누수가
 * 있는지 본다(make soak). 통합 테스트(make test)에는 넣지 않는다: 몇 분씩 걸린다.
 *
 * 한 바퀴(약 0.1초): 8명 접속 → 2명 UDP 묶기 → Pong 두 쌍 QUICK_JOIN으로 경기 시작 → 입력 몇 개
 * (묶인 쪽은 UDP 묶음) → Relay 방 하나 → 절반은 LEAVE 후 정상 종료, 절반은 RST로 끊기.
 * 10초마다 서버 RSS(/proc/<pid>/statm)를 찍고, 끝에 시작 · 끝 · 최대와 실패 수를 낸다.
 * 서버는 SIGTERM으로 내리고 종료 코드 0, 로그에 sanitizer 보고(ASan 빌드면 누수 포함)가 없어야 한다.
 *   사용: soak <relay-server> <seconds> <label>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "client.h"
#include "server_proc.h"
#include "wire.h"

#define CLIENTS 8
#define ROUND_MS 100
#define SAMPLE_MS 10000
#define GAME_PONG 1
#define GAME_RELAY 2

static unsigned long long g_failures, g_conns, g_rounds;

static unsigned long long rss_kb(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/statm", (int)pid);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return 0;
    unsigned long long size = 0, resident = 0;
    int ok = fscanf(f, "%llu %llu", &size, &resident) == 2;
    fclose(f);
    long page = sysconf(_SC_PAGESIZE);
    return ok ? resident * (unsigned long long)(page > 0 ? page : 4096) / 1024u : 0;
}

/* 다른 type은 건너뛰고 type을 기다린다. 못 받으면 실패로 센다. */
static int wait_skip(client *c, uint8_t type)
{
    uint64_t deadline = itest_now_ms() + T_LONG;
    msg m;
    for (uint64_t now = itest_now_ms(); now < deadline; now = itest_now_ms()) {
        if (client_recv(c, &m, (int)(deadline - now)) != 1)
            break;
        if (m.type == type)
            return 1;
    }
    g_failures++;
    return 0;
}

/* 커널이 RST를 보내게 닫는다. 클라이언트 쪽 TIME_WAIT도 남지 않아 포트가 바닥나지 않는다. */
static void abort_close(client *c)
{
    struct linger lg = {.l_onoff = 1, .l_linger = 0};
    if (c->fd != -1)
        setsockopt(c->fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    client_close(c);
}

static void one_round(void)
{
    client cs[CLIENTS];
    int n = 0;
    for (; n < CLIENTS; n++) {
        if (client_connect(&cs[n], 0) != 0) {
            g_failures++;
            client_close(&cs[n]);
            break;
        }
        g_conns++;
    }
    if (n == CLIENTS) {
        if (!client_udp_bind(&cs[0]) || !client_udp_bind(&cs[2]))
            g_failures++;
        /* 0 · 1, 2 · 3이 차례로 QUICK_JOIN: 앞사람이 만든 방에 뒷사람이 들어가 경기가 시작된다 */
        for (int i = 0; i < 4; i++) {
            send_u16(&cs[i], MSG_QUICK_JOIN, GAME_PONG);
            wait_skip(&cs[i], MSG_ROOM_JOINED);
        }
        for (int i = 0; i < 4; i++)
            wait_skip(&cs[i], MSG_MATCH_START);
        for (int k = 0; k < 3; k++) {
            for (int i = 0; i < 4; i++) {
                if (cs[i].ufd != -1)
                    send_pong_batch_udp(&cs[i], (uint8_t)(k % 3));
                else
                    send_pong_input(&cs[i], (uint8_t)(k % 3));
            }
        }
        send_create(&cs[4], GAME_RELAY, ROOM_VISIBILITY_PUBLIC, NULL, 0);
        if (wait_skip(&cs[4], MSG_ROOM_JOINED)) {
            uint8_t hello[32] = "soak";
            client_send(&cs[4], MSG_GAME_IN, hello, sizeof hello);
        }
        send_u32(&cs[5], MSG_PING, 1);
        wait_skip(&cs[5], MSG_PONG);
    }
    for (int i = 0; i < n; i++) {
        if (i % 2 == 0)
            client_quit(&cs[i]);
        else
            abort_close(&cs[i]);
    }
}

static void sleep_until(uint64_t t)
{
    uint64_t now = itest_now_ms();
    if (now >= t)
        return;
    struct timespec ts = {(time_t)((t - now) / 1000u), (long)((t - now) % 1000u) * 1000000L};
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s <relay-server> <seconds> <label>\n", argv[0]);
        return 2;
    }
    int seconds = atoi(argv[2]);
    char log_path[4096];
    snprintf(log_path, sizeof log_path, "%s-%s-server.log", argv[0], argv[3]);
    static const char *const args[] = {"--quiet", NULL};
    server_proc s;
    if (seconds <= 0 || server_start(&s, argv[1], args, log_path) == -1)
        return 1;
    g_itest_port = s.port;
    printf("soak %s: %s for %d s, log %s\n", argv[3], argv[1], seconds, log_path);

    uint64_t start = itest_now_ms(), end = start + (uint64_t)seconds * 1000u;
    unsigned long long first = rss_kb(s.pid), last = first, max = first;
    printf("t=0s rss_kb=%llu\n", first);
    uint64_t next_sample = start + SAMPLE_MS;
    while (itest_now_ms() < end) {
        uint64_t round_start = itest_now_ms();
        one_round();
        g_rounds++;
        uint64_t now = itest_now_ms();
        if (now >= next_sample || now >= end) {
            last = rss_kb(s.pid);
            if (last > max)
                max = last;
            printf("t=%llus rss_kb=%llu conns=%llu rounds=%llu failures=%llu\n",
                   (unsigned long long)((now - start) / 1000u), last, g_conns, g_rounds,
                   g_failures);
            fflush(stdout);
            next_sample += SAMPLE_MS;
        }
        sleep_until(round_start + ROUND_MS);
    }
    int exited = server_stop(&s);
    int clean = server_log_clean(log_path);
    printf("soak %s: rss_kb start %llu end %llu max %llu; %llu connections in %llu rounds, "
           "%llu failures; server exit %s, sanitizer %s\n",
           argv[3], first, last, max, g_conns, g_rounds, g_failures, exited ? "0" : "BAD",
           clean ? "clean" : "REPORTED");
    return g_failures == 0 && exited && clean ? 0 : 1;
}
