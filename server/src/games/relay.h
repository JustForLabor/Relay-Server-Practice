/* relay.h - Relay 게임 모듈 (protocol.md 6.2절, game_id 2).
 * 서버는 계산하지 않고, 받은 GAME_IN 앞에 보낸 사람 slot을 붙여 방의 다른 사람에게 나른다. */
#ifndef RELAY_H
#define RELAY_H

#include "../game_api.h"

extern const game_def relay_game;

#endif /* RELAY_H */
