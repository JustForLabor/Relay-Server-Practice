/* client.h - 통합 테스트용 C 클라이언트. protocol.md의 바이트를 서버와 독립적으로 만들고 읽는다.
 *
 * 서버의 frame 파서는 서버 → 클라이언트 type을 거부하므로 쓰지 않고, 여기서 따로 자른다.
 * 모든 기다림에는 시간 제한이 있다: 서버가 답하지 않을 때 테스트가 영원히 멈추지 않게 한다.
 */
#ifndef ITEST_CLIENT_H
#define ITEST_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

#define T_SHORT 300   /* "오지 않아야 함"을 확인할 때 기다리는 ms */
#define T_LONG 5000   /* 와야 하는 메시지를 기다리는 최대 ms(ASan으로 느려져도 넉넉히) */

typedef struct msg {
    uint8_t type;
    uint16_t len;
    uint8_t p[FRAME_MAX_PAYLOAD];
} msg;

typedef struct client {
    int fd;
    int ufd;           /* UDP 소켓(서버 주소로 connect). 묶지 않았으면 -1 */
    uint8_t buf[8192];
    size_t len;
    uint32_t conn_id;
    uint32_t udp_token;
    uint32_t seq;      /* Pong GAME_IN input_seq */
    uint32_t ping_seq; /* client_quit이 쓰는 PING 값 */
    msg welcome;
} client;

/* 실패를 기록하고 현재 시나리오 함수에서 빠져나온다. */
void itest_fail(const char *file, int line, const char *what);
extern int g_itest_failures;
extern uint16_t g_itest_port;

#define REQUIRE(c)                                    \
    do {                                              \
        if (!(c)) {                                   \
            itest_fail(__FILE__, __LINE__, #c);       \
            return;                                   \
        }                                             \
    } while (0)

/* 접속하고 WELCOME을 받아 둔다. rcvbuf > 0이면 connect 전에 SO_RCVBUF를 줄인다. 실패하면 -1. */
int client_connect(client *c, int rcvbuf);
void client_close(client *c);

void client_send(client *c, uint8_t type, const void *payload, size_t len);
void client_send_raw(client *c, const void *bytes, size_t n);

/* 메시지 하나를 받는다. 1 받음, 0 시간 초과, -1 연결 끊김. */
int client_recv(client *c, msg *m, int timeout_ms);

/* type이 올 때까지 GAME_OUT은 건너뛰고 기다린다. 다른 type이 먼저 오면 실패(0). */
int client_wait_for(client *c, uint8_t type, msg *m, int timeout_ms);

/* 다음 메시지가 정확히 type이고 페이로드가 u8 value 하나인지. */
int client_expect_u8(client *c, uint8_t type, uint8_t value);

/* 다음 메시지가 ERROR(code)인지. */
int client_expect_error(client *c, uint16_t code);

/* 서버가 연결을 닫을 때까지 받는다. 닫히면 1, 시간 초과면 0. 닫히기 전에 받은 메시지 수를
 * *frames에 넣는다(NULL이면 무시). */
int client_wait_closed(client *c, int timeout_ms, int *frames);

/* 방에 있으면 나가고(LEAVE_ROOM), PING · PONG으로 서버가 처리했음을 확인한 뒤 닫는다.
 * 다음 시나리오가 이전 방을 보지 않게 한다. */
void client_quit(client *c);

/* 자주 쓰는 메시지 */
void send_u16(client *c, uint8_t type, uint16_t v);
void send_u32(client *c, uint8_t type, uint32_t v);
void send_create(client *c, uint16_t game_id, uint8_t visibility, const uint8_t *settings,
                 uint8_t settings_len);
void send_pong_input(client *c, uint8_t axis);

typedef struct room_info {
    uint32_t room_id;
    uint16_t game_id;
    uint8_t slot, max_players, visibility, mask, settings_len;
    char code[ROOM_CODE_LEN];
    uint8_t settings[64];
} room_info;

/* 다음 메시지가 ROOM_JOINED이면 풀어서 1. */
int client_expect_joined(client *c, room_info *ri);

uint64_t itest_now_ms(void);

/* ---- UDP (protocol.md 7장, udp_client.c) ---- */

/* 서버 UDP 포트로 connect한 새 UDP 소켓. 소켓마다 출처 포트가 달라 "다른 주소"를 흉내 낼 수 있다. */
int udp_socket_open(void);

/* u32 token + 프레임 하나를 데이터그램 하나로 보낸다. */
void udp_send_frame_tok(int fd, uint32_t token, uint8_t type, const void *payload, size_t len);
void udp_send_bytes(int fd, const void *bytes, size_t n);

/* 데이터그램 하나 = 프레임 하나(토큰 없음)를 받는다. 1 받음, 0 시간 초과, -1 프레임이 아님. */
int udp_recv(int fd, msg *m, int timeout_ms);

/* c->ufd를 열고(없으면) UDP_BIND를 0.2초마다 보내 2초 안에 UDP_BOUND를 받으면 1, 아니면 0. */
int client_udp_bind(client *c);

/* Pong 묶음 GAME_IN(6.1절)을 UDP로: 새 입력(++seq)과 그 앞 최대 3개, 오래된 것부터. */
void send_pong_batch_udp(client *c, uint8_t axis);

#endif /* ITEST_CLIENT_H */
