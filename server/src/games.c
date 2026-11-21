/* games.c - 등록 목록. 순서가 WELCOME에 실리는 순서다. */
#include "games.h"

#include "games/pong.h"
#include "games/relay.h"

static const game_def *const g_games[] = {
    &pong_game,
    &relay_game,
};

#define GAME_COUNT (sizeof g_games / sizeof g_games[0])

size_t games_count(void)
{
    return GAME_COUNT;
}

const game_def *games_at(size_t i)
{
    return i < GAME_COUNT ? g_games[i] : NULL;
}

const game_def *games_find(uint16_t id, size_t *index)
{
    for (size_t i = 0; i < GAME_COUNT; i++) {
        if (g_games[i]->id == id) {
            if (index != NULL)
                *index = i;
            return g_games[i];
        }
    }
    return NULL;
}
