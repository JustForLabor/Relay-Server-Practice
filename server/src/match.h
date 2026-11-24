/* match.h - 경기 만들기 · 끝내기 · 진행 · 다시 하기 (protocol.md 5.2절, 5.5절).
 *
 * 게임 콜백을 부른 뒤에는 반드시 match_settle을 불러, 콜백 안에서 요청된 match_over를
 * 콜백이 돌아온 다음에 처리한다(game_api.h의 약속). 콜백 도중에 경기를 지우면 게임이
 * 자기 상태를 쓰는 중에 메모리가 사라진다.
 */
#ifndef MATCH_H
#define MATCH_H

#include <stdbool.h>
#include <stdint.h>

#include "room.h"

/* 경기를 만들고 방 전원에게 MATCH_START. 게임이 만들지 못하면 방 사람을 모두 끊고 false. */
bool match_start(room *r);

/* 경기를 지우고 남은 사람에게 MATCH_END(reason). 게임이 끝낸 경우 방은 "막 끝난 방"이 된다. */
void match_end(room *r, uint8_t reason);

/* 콜백 뒤 처리: match_over가 불렸으면 MATCH_END(0)과 match_destroy. */
void match_settle(room *r);

/* 경기 중에 새로 들어온 사람(slot)에게 MATCH_START를 보내고 게임에 알린다. */
void match_player_joined(room *r, int slot);

/* slot이 방에서 빠진 뒤 부른다. 최소 인원 아래면 MATCH_END(1), 아니면 on_leave. */
void match_player_left(room *r, int slot);

/* match_input 결과. 무엇을 할지(끊기 · 버리기)는 받은 경로(TCP · UDP)에 따라 session이 정한다. */
enum { MATCH_INPUT_OK = 0, MATCH_INPUT_TOO_LONG = -1, MATCH_INPUT_REJECTED = -2 };

/* GAME_IN 하나를 게임에 넘긴다. 게임 길이 상한을 넘으면 넘기지 않고 TOO_LONG,
 * 게임이 형식 오류로 거부하면 REJECTED. */
int match_input(room *r, conn *c, const uint8_t *data, size_t len);

/* 진행 시각이 지난 만큼(한 번에 TICK_MAX_CATCHUP까지) on_tick을 부른다. */
void match_tick(room *r, uint64_t now);

/* REMATCH. 막 끝난 방이 아니면 ERR_BAD_STATE, 아니면 0. */
int match_rematch(room *r, conn *c);

#endif /* MATCH_H */
