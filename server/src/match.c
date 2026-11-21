/* match.c - 경기 수명과 진행. 설명은 match.h. */
#include "match.h"

#include <stdio.h>

#include "msg.h"

static void send_all_u8(room *r, uint8_t type, uint8_t v)
{
    room_broadcast(r, NULL, type, &v, 1);
}

static uint8_t member_mask(const room *r)
{
    uint8_t mask = 0;
    for (int i = 0; i < GAME_MAX_SLOTS; i++) {
        if (r->members[i] != NULL)
            mask |= (uint8_t)(1u << i);
    }
    return mask;
}

/* 중간에 들어온 사람에게 줄 start_tick: 다음 on_tick 번호다. 처음 시작할 때는 0과 같다.
 * 진행 간격이 없는 게임은 protocol.md 4.2절대로 0. */
static uint32_t current_start_tick(const room *r)
{
    return r->game->tick_hz > 0 ? r->next_tick : 0;
}

bool match_start(room *r)
{
    uint8_t slots[GAME_MAX_SLOTS];
    for (int i = 0; i < GAME_MAX_SLOTS; i++)
        slots[i] = r->members[i] != NULL ? 1 : 0;
    r->over_pending = false;
    r->just_ended = false;
    r->votes = 0;
    r->next_tick = 0;
    r->match = r->game->match_create(&r->host, slots, r->settings, r->settings_len);
    if (r->match == NULL) {
        /* game_api.h: 만들지 못하면 코어가 방을 닫는다. 방을 닫는다는 메시지가 프로토콜에
         * 없으므로 방 사람의 연결을 끊는다. 방은 마지막 사람이 정리될 때 사라진다. */
        fprintf(stderr, "room %u: match_create failed, closing room\n", r->id);
        for (int i = 0; i < GAME_MAX_SLOTS; i++) {
            if (r->members[i] != NULL)
                conn_kill(r->members[i], "match_create failed");
        }
        return false;
    }
    if (r->game->tick_hz > 0)
        tick_clock_start(&r->clock, r->game->tick_hz, tick_now_ns());
    uint32_t start = current_start_tick(r);
    for (int i = 0; i < GAME_MAX_SLOTS; i++) {
        if (r->members[i] != NULL)
            msg_match_start(r->members[i], start);
    }
    match_settle(r); /* match_create 안에서 match_over를 불렀을 수도 있다 */
    room_update_listing(r);
    return true;
}

void match_end(room *r, uint8_t reason)
{
    if (r->match == NULL)
        return;
    send_all_u8(r, MSG_MATCH_END, reason);
    r->game->match_destroy(r->match);
    r->match = NULL;
    r->over_pending = false;
    r->votes = 0;
    r->just_ended = reason == MATCH_END_BY_GAME;
    room_update_listing(r);
}

void match_settle(room *r)
{
    if (r->over_pending && r->match != NULL)
        match_end(r, MATCH_END_BY_GAME);
    r->over_pending = false;
}

void match_player_joined(room *r, int slot)
{
    msg_match_start(r->members[slot], current_start_tick(r));
    if (r->game->on_join != NULL) {
        r->game->on_join(r->match, slot);
        match_settle(r);
    }
}

void match_player_left(room *r, int slot)
{
    /* game_api.h: 최소 인원 아래로 떨어지는 퇴장은 on_leave 없이 match_destroy만 부른다. */
    if (r->count < r->game->min_players) {
        match_end(r, MATCH_END_TOO_FEW);
        return;
    }
    if (r->game->on_leave != NULL) {
        r->game->on_leave(r->match, slot);
        match_settle(r);
    }
}

void match_input(room *r, conn *c, const uint8_t *data, size_t len)
{
    if (len > r->game->max_input_len) {
        /* 5.4절: 길이가 그 종류의 허용 길이와 다르면 ERROR 없이 끊는다. */
        conn_kill(c, "game_in longer than game limit");
        return;
    }
    int rc = r->game->on_input(r->match, c->slot, data, len);
    match_settle(r);
    if (rc < 0) {
        msg_error(c, ERR_BAD_GAME_INPUT, "bad game input");
        conn_kill(c, "bad game input");
    }
}

void match_tick(room *r, uint64_t now)
{
    int steps = 0;
    while (r->match != NULL && steps < TICK_MAX_CATCHUP && tick_clock_deadline(&r->clock) <= now) {
        r->game->on_tick(r->match, r->next_tick);
        r->next_tick++;
        tick_clock_advance(&r->clock);
        steps++;
        match_settle(r); /* 경기가 끝났으면 match가 NULL이 되어 남은 진행을 하지 않는다 */
    }
    if (r->match != NULL && tick_clock_deadline(&r->clock) <= now) {
        tick_clock_drop_backlog(&r->clock, now);
        fprintf(stderr, "room %u: behind schedule, dropped ticks (total %llu)\n", r->id,
                (unsigned long long)r->clock.dropped);
    }
}

int match_rematch(room *r, conn *c)
{
    if (!r->just_ended)
        return ERR_BAD_STATE;
    uint8_t bit = (uint8_t)(1u << c->slot);
    if ((r->votes & bit) == 0) { /* 같은 사람이 두 번 보내도 한 번으로 친다 */
        r->votes |= bit;
        send_all_u8(r, MSG_REMATCH_VOTED, (uint8_t)c->slot);
    }
    if (r->votes == member_mask(r) && r->count >= r->game->min_players)
        match_start(r);
    return 0;
}
