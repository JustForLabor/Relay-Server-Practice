/* server_proc.c - 서버 프로세스 띄우기 · 내리기. 설명은 server_proc.h. */
#include "server_proc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "client.h"

#define MAX_ARGS 16

const char *g_itest_server_path;

int server_start(server_proc *s, const char *path, const char *const extra[],
                 const char *log_path)
{
    memset(s, 0, sizeof *s);
    s->pid = -1;
    s->out_fd = -1;
    snprintf(s->log_path, sizeof s->log_path, "%s", log_path);
    const char *argv[MAX_ARGS + 4] = {path, "--port", "0"};
    size_t argc = 3;
    for (size_t i = 0; extra != NULL && extra[i] != NULL && i < MAX_ARGS; i++)
        argv[argc++] = extra[i];
    argv[argc] = NULL;

    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) == -1) {
        perror("pipe2");
        return -1;
    }
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        int logfd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logfd == -1 || dup2(pfd[1], STDOUT_FILENO) == -1 || dup2(logfd, STDERR_FILENO) == -1)
            _exit(126);
        /* execv의 argv는 char *const[]라 const를 떼야 한다. 서버는 인자를 고치지 않는다. */
        execv(path, (char *const *)(void *)argv);
        _exit(127);
    }
    close(pfd[1]);
    s->pid = pid;
    s->out_fd = pfd[0];
    fcntl(s->out_fd, F_SETFL, fcntl(s->out_fd, F_GETFL) | O_NONBLOCK);

    /* 서버가 줄을 쓰지 않고 죽거나 멈춰도 영원히 기다리지 않게 시간 제한을 둔다. */
    char line[128];
    unsigned port = 0;
    if (server_read_line(s, line, sizeof line, 15000) != 1 ||
        sscanf(line, "listening on port %u", &port) != 1 || port == 0 || port > 65535) {
        fprintf(stderr, "server did not report its port\n");
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(s->out_fd);
        s->out_fd = -1;
        s->pid = -1;
        return -1;
    }
    s->port = (uint16_t)port;
    return 0;
}

int server_read_line(server_proc *s, char *line, size_t cap, int timeout_ms)
{
    uint64_t deadline = itest_now_ms() + (uint64_t)timeout_ms;
    for (;;) {
        char *nl = memchr(s->out, '\n', s->out_len);
        if (nl != NULL) {
            size_t n = (size_t)(nl - s->out);
            size_t keep = n < cap - 1 ? n : cap - 1;
            memcpy(line, s->out, keep);
            line[keep] = '\0';
            memmove(s->out, nl + 1, s->out_len - n - 1);
            s->out_len -= n + 1;
            return 1;
        }
        if (s->out_len == sizeof s->out) /* 줄바꿈 없이 버퍼가 찼다: 출력이 깨졌다 */
            return -1;
        uint64_t now = itest_now_ms();
        int wait = now >= deadline ? 0 : (int)(deadline - now);
        struct pollfd p = {.fd = s->out_fd, .events = POLLIN};
        int r = poll(&p, 1, wait);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return r == 0 ? 0 : -1;
        ssize_t n = read(s->out_fd, s->out + s->out_len, sizeof s->out - s->out_len);
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (n <= 0)
            return -1;
        s->out_len += (size_t)n;
    }
}

int server_stop(server_proc *s)
{
    if (s->pid == -1)
        return 0;
    kill(s->pid, SIGTERM);
    /* LeakSanitizer 검사가 끝날 때까지 넉넉히 기다린다. */
    uint64_t deadline = itest_now_ms() + 20000;
    int status = 0;
    int ok = 1;
    for (;;) {
        pid_t r = waitpid(s->pid, &status, WNOHANG);
        if (r == s->pid)
            break;
        if (r == -1 && errno != EINTR) {
            perror("waitpid");
            ok = 0;
            break;
        }
        if (itest_now_ms() >= deadline) {
            fprintf(stderr, "server did not exit after SIGTERM\n");
            kill(s->pid, SIGKILL);
            waitpid(s->pid, NULL, 0);
            ok = 0;
            break;
        }
        struct timespec ts = {0, 20 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    if (ok && (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
        fprintf(stderr, "server exit status: %s %d\n", WIFEXITED(status) ? "code" : "signal",
                WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
        ok = 0;
    }
    s->pid = -1;
    if (s->out_fd != -1)
        close(s->out_fd);
    s->out_fd = -1;
    return ok;
}

int server_log_clean(const char *log_path)
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

int server_log_has(const char *log_path, const char *needle)
{
    FILE *f = fopen(log_path, "r");
    if (f == NULL)
        return 0;
    char line[1024];
    int found = 0;
    while (!found && fgets(line, sizeof line, f) != NULL)
        found = strstr(line, needle) != NULL;
    fclose(f);
    return found;
}
