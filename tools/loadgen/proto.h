/* proto.h - loadgen이 쓰는 protocol.md의 번호 · 형식.
 *
 * 서버 헤더를 끌어오지 않고 명세에서 직접 옮긴다: 측정 도구는 서버 구현과 독립적으로
 * 명세의 바이트를 만들어야, 서버가 명세와 어긋날 때 같이 틀리지 않는다. */
#ifndef LOADGEN_PROTO_H
#define LOADGEN_PROTO_H

#include <stdint.h>

#define P_VERSION 1
#define P_HDR 4
#define P_MAX_PAYLOAD 512

/* 4.1 클라이언트 → 서버, 7장 UDP_BIND */
enum {
    P_CREATE_ROOM = 0x11,
    P_JOIN_BY_CODE = 0x14,
    P_PING = 0x16,
    P_REMATCH = 0x17,
    P_UDP_BIND = 0x18,
    P_GAME_IN = 0x40,
};

/* 4.2 서버 → 클라이언트, 7장 UDP_BOUND */
enum {
    P_WELCOME = 0x80,
    P_ROOM_JOINED = 0x82,
    P_PLAYER_JOINED = 0x83,
    P_PLAYER_LEFT = 0x84,
    P_MATCH_START = 0x85,
    P_MATCH_END = 0x86,
    P_PONG = 0x87,
    P_REMATCH_VOTED = 0x88,
    P_UDP_BOUND = 0x89,
    P_ERROR = 0x8F,
    P_GAME_OUT = 0xC0,
};

#define P_ERR_SERVER_FULL 8
#define P_GAME_PONG 1
#define P_CODE_LEN 6

/* 6.1 Pong GAME_OUT 종류와 STATE 길이(종류 바이트 포함) */
#define PONG_STATE 0x01
#define PONG_GOAL 0x02
#define PONG_STATE_LEN 20
#define PONG_SETTINGS_LEN 25
#define PONG_BUNDLE_MAX 4

static inline uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* 프레임 헤더를 쓴다. 페이로드는 호출한 쪽이 dst + P_HDR에 채운다. */
static inline void put_hdr(uint8_t *dst, uint8_t type, uint16_t len)
{
    dst[0] = P_VERSION;
    dst[1] = type;
    put_u16(dst + 2, len);
}

#endif /* LOADGEN_PROTO_H */
