/* conn.h - 연결 테이블, 연결별 수신 · 송신 버퍼, 지연 정리 (overview.md 6.1 conn, 6.4).
 *
 * 연결은 끊을 일이 생겨도 바로 free하지 않고 종료 표시만 한다(conn_kill). 같은 반복 안에서
 * 방 · 게임 콜백이 그 포인터를 아직 쥐고 있을 수 있어서다. 실제 정리는 반복 끝의 conn_sweep.
 */
#ifndef CONN_H
#define CONN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "frame.h"

/* 송신 버퍼 상한. 읽지 않는 클라이언트 하나가 서버 메모리를 끝없이 먹지 못하게 한다. */
#define CONN_OUT_MAX (64u * 1024u)

struct room;

typedef struct conn {
    int fd;
    uint32_t id;
    uint32_t udp_token;
    bool closing;
    const char *close_reason;
    bool epollout_on;

    frame_reader in;

    uint8_t *out;    /* 아직 커널에 넘기지 못한 바이트: out[out_off .. out_len) */
    size_t out_off, out_len, out_cap;

    struct room *room; /* 로비면 NULL */
    int slot;
    uint8_t code_fails; /* 틀린 입장 코드 수(protocol.md 5.3절) */

    /* 이번 반복에 보낼 것이 생긴 연결 목록. 메시지마다 send하지 않고 반복 끝에 한 번에 내보낸다. */
    struct conn *flush_prev, *flush_next;
    bool in_flush;
    struct conn *close_next; /* 종료 표시된 연결 목록 */
} conn;

/* max_fds 크기의 fd → 연결 표를 만든다. 실패하면 -1. */
int conn_system_init(int epfd, size_t max_fds);

/* 남은 연결을 메시지 없이 모두 닫고 표를 해제한다(정상 종료). */
void conn_system_shutdown(void);

/* 받아 낸 fd로 연결을 만들고 epoll에 등록한다. 실패하면 NULL(fd는 호출한 쪽이 닫는다). */
conn *conn_open(int fd);

size_t conn_count(void);

/* 종료 표시. 이후 이 연결로 가는 송신은 버려지고, 수신 처리도 멈춘다. */
void conn_kill(conn *c, const char *why);

/* 헤더 + payload 프레임 하나를 송신 버퍼에 넣는다. 상한을 넘으면 연결을 종료 표시한다. */
void conn_send_frame(conn *c, uint8_t type, const uint8_t *payload, size_t len);

/* 송신 버퍼에 n바이트 자리를 잡아 돌려준다. 종료 중이거나 상한을 넘으면 NULL.
 * 호출한 쪽이 바로 n바이트를 다 채워야 한다. */
uint8_t *conn_reserve(conn *c, size_t n);

/* 이번 반복에 쌓인 송신 버퍼를 모두 커널에 넘긴다. 다 못 넘긴 연결은 EPOLLOUT을 켠다. */
void conn_flush_all(void);

/* EPOLLOUT 이벤트: 남은 바이트를 마저 보낸다. */
void conn_on_writable(conn *c);

/* 종료 표시된 연결을 정리한다. 닫기 전에 on_close(방에서 빼기 등)를 부른다.
 * on_close가 다른 연결을 또 종료 표시하면 그것까지 정리한다. 하나라도 닫았으면 true. */
bool conn_sweep(void (*on_close)(conn *c));

#endif /* CONN_H */
