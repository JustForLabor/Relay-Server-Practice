/* server.h - 메인 루프 (overview.md 6.2). */
#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>

/* port에서 듣고 SIGINT · SIGTERM이 올 때까지 돈다. 정상 종료면 0. */
int server_run(uint16_t port);

#endif /* SERVER_H */
