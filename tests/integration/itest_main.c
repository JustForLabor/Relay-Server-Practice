/* itest_main.c - 서버를 --port 0으로 띄우고 시나리오 (a)–(m)을 돌린 뒤 SIGTERM으로 끝낸다.
 *
 * (a)–(k)는 여기서 띄운 서버 하나에서 차례로 돈다. (l) · (m)은 옵션이 달라 자기 서버를 따로 띄운다.
 * 서버 stderr는 파일로 받아 마지막에 sanitizer 보고가 있는지 본다.
 *   사용: integration_test <relay-server 경로>
 */
#include <stdio.h>

#include "scenarios.h"
#include "server_proc.h"

typedef struct scenario {
    const char *name;
    void (*run)(void);
} scenario;

static const scenario k_scenarios[] = {
    {"(a) WELCOME, PING/PONG, lobby errors", scenario_a_welcome_ping},
    {"(d) QUICK_JOIN pairs two players", scenario_d_quick_join},
    {"(b) public room, LIST_ROOMS, JOIN_ROOM, same STATE per tick", scenario_b_public_room_pong},
    {"(c) private room code, 5 wrong codes cut", scenario_c_private_code},
    {"(e) win_score 1, MATCH_END(0), REMATCH", scenario_e_rematch},
    {"(f) disconnect mid match", scenario_f_disconnect_mid_match},
    {"(g) Relay room of three", scenario_g_relay_three},
    {"(h) protocol violations cut only that connection", scenario_h_robustness},
    {"(i) slow reader over 64KB is cut", scenario_i_slow_reader},
    {"(j) UDP bind: STATE over UDP, GOAL over TCP, mixed room", scenario_j_udp_bind},
    {"(k) bad datagrams, unknown token, other address ignored", scenario_k_udp_ignored},
    {"(l) --max-conns 3: 4th gets SERVER_FULL", scenario_l_max_conns},
    {"(m) --stats-interval 1 JSON line", scenario_m_stats},
};

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <relay-server>\n", argv[0]);
        return 2;
    }
    g_itest_server_path = argv[1];
    char log_path[4096];
    g_itest_log_prefix = argv[0];
    snprintf(log_path, sizeof log_path, "%s-server.log", argv[0]);
    server_proc srv;
    if (server_start(&srv, argv[1], NULL, log_path) == -1)
        return 1;
    g_itest_port = srv.port;
    printf("server on port %u, log %s\n", (unsigned)g_itest_port, log_path);

    int passed = 0;
    size_t count = sizeof k_scenarios / sizeof k_scenarios[0];
    for (size_t i = 0; i < count; i++) {
        int before = g_itest_failures;
        uint64_t t0 = itest_now_ms();
        k_scenarios[i].run();
        /* (l) · (m)은 자기 서버의 포트로 바꿨다가 끝나면 되돌린다. 실패로 빠져도 되돌린다. */
        g_itest_port = srv.port;
        int ok = g_itest_failures == before;
        passed += ok;
        printf("%s %s (%llu ms)\n", ok ? "PASS" : "FAIL", k_scenarios[i].name,
               (unsigned long long)(itest_now_ms() - t0));
        fflush(stdout);
    }

    int exited = server_stop(&srv);
    int clean = server_log_clean(log_path);
    printf("%s server exited 0 after SIGTERM\n", exited ? "PASS" : "FAIL");
    printf("%s no sanitizer reports in server log\n", clean ? "PASS" : "FAIL");
    printf("integration: %d/%zu scenarios passed\n", passed, count);
    return (passed == (int)count && exited && clean) ? 0 : 1;
}
