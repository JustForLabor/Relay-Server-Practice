/* msg.h - 서버 → 클라이언트 공통 메시지 만들기 (protocol.md 4.2절).
 * 필드 순서 · 길이는 이 파일과 protocol.md 표만 맞추면 되도록 한곳에 모은다. */
#ifndef MSG_H
#define MSG_H

#include <stdint.h>

#include "conn.h"
#include "room.h"

void msg_welcome(conn *c);
void msg_error(conn *c, uint16_t code, const char *reason);
void msg_room_joined(conn *c, const room *r);
void msg_match_start(conn *c, uint32_t start_tick);
void msg_pong(conn *c, uint32_t client_time_ms, uint32_t server_time_ms);

/* 페이로드가 u8 하나인 메시지(PLAYER_JOINED · PLAYER_LEFT · MATCH_END · REMATCH_VOTED). */
void msg_u8(conn *c, uint8_t type, uint8_t value);

#endif /* MSG_H */
