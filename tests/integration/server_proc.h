/* server_proc.h - 테스트 · soak가 서버 프로세스를 띄우고 내리는 도우미.
 *
 * 서버는 늘 --port 0으로 띄워 stdout의 "listening on port N" 한 줄로 포트를 읽는다(커널이 고른 빈
 * 포트라 여러 서버를 동시에 띄워도 부딪히지 않는다). stdout 파이프는 열어 둬 --stats-interval의
 * JSON 줄을 읽을 수 있게 하고, stderr는 파일로 받아 sanitizer 보고를 찾는다.
 */
#ifndef ITEST_SERVER_PROC_H
#define ITEST_SERVER_PROC_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct server_proc {
    pid_t pid;
    int out_fd;      /* 서버 stdout 읽는 쪽(논블로킹) */
    uint16_t port;
    char out[8192];  /* 아직 줄로 꺼내지 않은 stdout 바이트 */
    size_t out_len;
    char log_path[4096];
} server_proc;

/* 통합 테스트 · soak에 넘겨받은 서버 실행 파일. 따로 서버를 띄우는 시나리오가 쓴다. */
extern const char *g_itest_server_path;

/* path를 "--port 0" + extra(NULL로 끝, NULL 자체도 됨)로 띄우고 포트를 읽는다. 성공 0, 실패 -1. */
int server_start(server_proc *s, const char *path, const char *const extra[],
                 const char *log_path);

/* stdout에서 한 줄(줄바꿈 뺌)을 timeout_ms까지 기다려 꺼낸다. 1 꺼냄, 0 시간 초과, -1 닫힘. */
int server_read_line(server_proc *s, char *line, size_t cap, int timeout_ms);

/* SIGTERM 뒤 정상 종료(코드 0)를 기다린다. 0이 아닌 종료 · 시간 초과면 0, 정상이면 1. */
int server_stop(server_proc *s);

/* 서버 로그에 sanitizer 보고가 없으면 1. 있으면 그 줄을 보여 주고 0. */
int server_log_clean(const char *log_path);

/* 서버 로그에 needle이 든 줄이 있으면 1. */
int server_log_has(const char *log_path, const char *needle);

#endif /* ITEST_SERVER_PROC_H */
