/* itest_main.c - 서버를 --port 0으로 띄우고 시나리오 (a)–(i)를 돌린 뒤 SIGTERM으로 끝낸다.
 *
 * 서버 stdout의 "listening on port N" 한 줄로 포트를 읽는다(커널이 고른 빈 포트라 여러 테스트가
 * 동시에 돌아도 부딪히지 않는다). 서버 stderr는 파일로 받아 마지막에 sanitizer 보고가 있는지 본다.
 *   사용: integration_test <relay-server 경로>
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "scenarios.h"

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
};

static pid_t start_server(const char *path, const char *log_path)
{
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) == -1) {
        perror("pipe2");
        return -1;
    }
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        int logfd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logfd == -1 || dup2(pfd[1], STDOUT_FILENO) == -1 || dup2(logfd, STDERR_FILENO) == -1)
            _exit(126);
        execl(path, path, "--port", "0", (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    /* 서버가 줄을 쓰지 않고 죽거나 멈춰도 테스트가 영원히 기다리지 않게 시간 제한을 둔다. */
    char line[128];
    size_t len = 0;
    uint64_t deadline = itest_now_ms() + 15000;
    while (len < sizeof line - 1 && memchr(line, '\n', len) == NULL) {
        uint64_t now = itest_now_ms();
        struct pollfd p = {.fd = pfd[0], .events = POLLIN};
        if (now >= deadline || poll(&p, 1, (int)(deadline - now)) <= 0)
            break;
        ssize_t n = read(pfd[0], line + len, sizeof line - 1 - len);
        if (n <= 0)
            break;
        len += (size_t)n;
    }
    line[len] = '\0';
    close(pfd[0]);
    unsigned port = 0;
    if (sscanf(line, "listening on port %u", &port) != 1 || port == 0 || port > 65535) {
        fprintf(stderr, "server did not report its port (got \"%s\")\n", line);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    g_itest_port = (uint16_t)port;
    return pid;
}

/* SIGTERM 뒤 정상 종료(코드 0)를 기다린다. LeakSanitizer 검사가 끝날 때까지 넉넉히 기다린다. */
static int stop_server(pid_t pid)
{
    kill(pid, SIGTERM);
    uint64_t deadline = itest_now_ms() + 20000;
    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            break;
        if (r == -1 && errno != EINTR) {
            perror("waitpid");
            return 0;
        }
        if (itest_now_ms() >= deadline) {
            fprintf(stderr, "server did not exit after SIGTERM\n");
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            return 0;
        }
        struct timespec ts = {0, 20 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "server exit status: %s %d\n", WIFEXITED(status) ? "code" : "signal",
                WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
        return 0;
    }
    return 1;
}

/* 서버 로그에 sanitizer 보고가 있으면 0. 로그는 실패했을 때 그대로 보여 준다. */
static int log_is_clean(const char *log_path)
{
    FILE *f = fopen(log_path, "r");
    if (f == NULL) {
        perror(log_path);
        return 0;
    }
    static const char *const bad[] = {"Sanitizer", "runtime error:", "LeakSanitizer"};
    char line[1024];
    int clean = 1;
    while (fgets(line, sizeof line, f) != NULL) {
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            if (strstr(line, bad[i]) != NULL) {
                clean = 0;
                fprintf(stderr, "server log: %s", line);
                break;
            }
        }
    }
    fclose(f);
    return clean;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <relay-server>\n", argv[0]);
        return 2;
    }
    char log_path[4096];
    snprintf(log_path, sizeof log_path, "%s-server.log", argv[0]);
    pid_t pid = start_server(argv[1], log_path);
    if (pid == -1)
        return 1;
    printf("server on port %u, log %s\n", (unsigned)g_itest_port, log_path);

    int passed = 0;
    size_t count = sizeof k_scenarios / sizeof k_scenarios[0];
    for (size_t i = 0; i < count; i++) {
        int before = g_itest_failures;
        uint64_t t0 = itest_now_ms();
        k_scenarios[i].run();
        int ok = g_itest_failures == before;
        passed += ok;
        printf("%s %s (%llu ms)\n", ok ? "PASS" : "FAIL", k_scenarios[i].name,
               (unsigned long long)(itest_now_ms() - t0));
        fflush(stdout);
    }

    int exited = stop_server(pid);
    int clean = log_is_clean(log_path);
    printf("%s server exited 0 after SIGTERM\n", exited ? "PASS" : "FAIL");
    printf("%s no sanitizer reports in server log\n", clean ? "PASS" : "FAIL");
    printf("integration: %d/%zu scenarios passed\n", passed, count);
    return (passed == (int)count && exited && clean) ? 0 : 1;
}
