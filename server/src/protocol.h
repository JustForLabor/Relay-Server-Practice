/* protocol.h - protocol.md 3–5장의 번호와 상한을 한곳에 모은다.
 * 값이 문서와 다르면 문서가 맞다. */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#define PROTO_VERSION 1
#define FRAME_HEADER_LEN 4
#define FRAME_MAX_PAYLOAD 512

/* 4.1 클라이언트 → 서버 */
enum {
    MSG_LIST_ROOMS = 0x10,
    MSG_CREATE_ROOM = 0x11,
    MSG_QUICK_JOIN = 0x12,
    MSG_JOIN_ROOM = 0x13,
    MSG_JOIN_BY_CODE = 0x14,
    MSG_LEAVE_ROOM = 0x15,
    MSG_PING = 0x16,
    MSG_REMATCH = 0x17,
    MSG_GAME_IN = 0x40,
};

/* 4.2 서버 → 클라이언트 */
enum {
    MSG_WELCOME = 0x80,
    MSG_ROOM_LIST = 0x81,
    MSG_ROOM_JOINED = 0x82,
    MSG_PLAYER_JOINED = 0x83,
    MSG_PLAYER_LEFT = 0x84,
    MSG_MATCH_START = 0x85,
    MSG_MATCH_END = 0x86,
    MSG_PONG = 0x87,
    MSG_REMATCH_VOTED = 0x88,
    MSG_ERROR = 0x8F,
    MSG_GAME_OUT = 0xC0,
};

/* 5.4 오류 코드 */
enum {
    ERR_BAD_STATE = 1,
    ERR_UNKNOWN_GAME = 2,
    ERR_ROOM_NOT_FOUND = 3,
    ERR_ROOM_FULL = 4,
    ERR_BAD_CODE = 5,
    ERR_TOO_MANY_CODE_TRIES = 6,
    ERR_BAD_GAME_INPUT = 7,
    ERR_SERVER_FULL = 8,
    ERR_BAD_SETTINGS = 9,
};

/* MATCH_END reason */
enum { MATCH_END_BY_GAME = 0, MATCH_END_TOO_FEW = 1 };

#define ROOM_VISIBILITY_PUBLIC 0
#define ROOM_VISIBILITY_PRIVATE 1
#define ROOM_CODE_LEN 6
#define ROOM_LIST_MAX 32
#define CODE_TRIES_MAX 5
#define ERROR_REASON_MAX 64

#endif /* PROTOCOL_H */
