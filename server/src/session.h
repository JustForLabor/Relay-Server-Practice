/* session.h - 연결 상태(로비 · 대기 · 경기 중)와 공통 메시지 처리 (protocol.md 5.1절). */
#ifndef SESSION_H
#define SESSION_H

#include "conn.h"
#include "frame.h"

typedef enum {
    SESSION_LOBBY,   /* 방에 없음 */
    SESSION_WAITING, /* 방에 있고 경기가 없음 */
    SESSION_PLAYING, /* 방에 있고 경기 중 */
} session_state;

/* 상태는 따로 저장하지 않고 방 · 경기 유무에서 읽는다: 두 곳에 두면 어긋날 수 있다. */
session_state session_state_of(const conn *c);

/* 접속 직후: WELCOME을 보낸다. */
void session_open(conn *c);

/* 프레임 검사를 통과한 메시지 하나를 처리한다. 처리 중 c가 종료 표시될 수 있다. */
void session_handle(conn *c, const frame *f);

#endif /* SESSION_H */
