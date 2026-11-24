/* room.h - 방 목록 · 입장 · 퇴장 · 공개 목록 (protocol.md 5장, overview.md 6.1 room).
 *
 * 방 하나는 게임 하나의 규칙 값과 자리(slot)들, 그리고 진행 중이면 경기 상태(match)를 가진다.
 * 경기 만들기 · 끝내기 · 진행은 match.c, 게임이 보내는 바이트의 전송은 channel.c가 맡는다.
 */
#ifndef ROOM_H
#define ROOM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "conn.h"
#include "game_api.h"
#include "protocol.h"
#include "tick.h"
#include "uthash.h"

typedef struct room {
    uint32_t id;
    const game_def *game;
    size_t game_index; /* games_at 순서. 게임별 공개 목록을 고른다 */
    uint8_t visibility;
    char code[ROOM_CODE_LEN]; /* 비공개 방만 */
    uint8_t settings[GAME_MAX_SETTINGS];
    size_t settings_len;

    conn *members[GAME_MAX_SLOTS];
    uint8_t count;

    void *match;        /* 경기 중이면 게임이 만든 상태, 아니면 NULL */
    game_host host;     /* ctx = 이 방. 방이 사는 동안 주소가 바뀌지 않는다 */
    bool over_pending;  /* 콜백 안에서 match_over가 불림. 콜백이 돌아온 뒤 처리한다 */
    bool just_ended;    /* "경기가 막 끝난 방"(protocol.md 5.5절) */
    uint8_t votes;      /* REMATCH에 동의한 slot 비트 */
    uint32_t next_tick; /* 다음 on_tick에 넘길 번호 */
    tick_clock clock;

    /* 게임별 공개 대기 목록(LIST_ROOMS · QUICK_JOIN). 목록에 나올 수 있게 된 순서로 이어 붙여서
     * 머리가 "가장 오래 기다린 방"이 된다. 전체 방을 훑거나 정렬하지 않아도 된다. */
    bool listed;
    struct room *list_prev, *list_next;

    UT_hash_handle hh;      /* id → 방 */
    UT_hash_handle hh_code; /* 비공개 코드 → 방 */
} room;

/* 게임 수만큼 공개 목록을 만든다. 실패하면 -1. */
int rooms_init(void);

/* 남은 방과 경기를 메시지 없이 모두 지운다(정상 종료). */
void rooms_shutdown(void);

/* 아래 함수는 성공하면 0, 아니면 protocol.md 5.4절 오류 코드를 돌려준다.
 * 오류 응답은 호출한 쪽(session)이 보낸다. c는 로비 상태여야 한다. */
int room_create(conn *c, uint16_t game_id, uint8_t visibility, const uint8_t *settings,
                size_t settings_len);
int room_quick_join(conn *c, uint16_t game_id);
int room_join_by_id(conn *c, uint32_t room_id);
int room_join_by_code(conn *c, const uint8_t code[ROOM_CODE_LEN]);

/* 방에서 뺀다(LEAVE_ROOM, 끊김). 남은 사람에게 알리고 인원이 모자라면 경기를 끝내며,
 * 아무도 없으면 방을 지운다. 이 호출 뒤 c->room의 옛 포인터를 쓰면 안 된다. */
void room_leave(conn *c);

/* 게임의 공개 대기 방 목록을 ROOM_LIST 페이로드로 c에 보낸다. 없는 게임이면 오류 코드. */
int room_send_list(conn *c, uint16_t game_id);

/* 방 상태가 바뀐 뒤 공개 목록에 넣거나 뺀다. */
void room_update_listing(room *r);

/* 방 전원(except 제외)에게 프레임 하나를 보낸다. */
void room_broadcast(room *r, const conn *except, uint8_t type, const uint8_t *payload,
                    size_t len);

/* 진행 시각이 된 방마다 게임을 진행한다. */
void rooms_tick(uint64_t now);

/* 가장 가까운 진행 시각까지 남은 ms(올림). 진행할 방이 없으면 -1. */
int rooms_timeout_ms(uint64_t now);

/* 지금 있는 방 수(테스트 · 로그용). */
size_t rooms_count(void);

/* 경기가 진행 중인 방 수(통계). */
size_t rooms_match_count(void);

#endif /* ROOM_H */
