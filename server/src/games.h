/* games.h - 등록된 게임 목록 (overview.md 6.1 games, protocol.md 1장 표).
 * 새 게임을 붙일 때 코어에서 고치는 곳은 games.c의 목록 한 줄뿐이다. */
#ifndef GAMES_H
#define GAMES_H

#include <stddef.h>
#include <stdint.h>

#include "game_api.h"

size_t games_count(void);

/* 등록 순서의 i번째 게임(WELCOME 순서). */
const game_def *games_at(size_t i);

/* game_id로 찾는다. 없으면 NULL, *index에는 등록 순서 번호. */
const game_def *games_find(uint16_t id, size_t *index);

#endif /* GAMES_H */
