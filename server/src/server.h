/* server.h - 메인 루프 (overview.md 6.2). */
#ifndef SERVER_H
#define SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct server_config {
    uint16_t port;           /* 0이면 커널이 빈 포트를 고른다 */
    bool udp;                /* false면 UDP를 열지 않는다(--no-udp, protocol.md 7장) */
    size_t max_conns;        /* 동시 접속 상한. 0이면 RLIMIT_NOFILE에서 여유를 뺀 값 */
    unsigned stats_interval; /* 초. 0이면 통계를 쓰지 않는다 */
    bool quiet;              /* 연결 · 방마다 생기는 로그를 끈다 */
} server_config;

/* cfg대로 듣고 SIGINT · SIGTERM이 올 때까지 돈다. 정상 종료면 0. */
int server_run(const server_config *cfg);

#endif /* SERVER_H */
