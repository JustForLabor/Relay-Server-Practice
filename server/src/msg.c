/* msg.c - 공통 메시지 인코더. */
#include "msg.h"

#include <string.h>

#include "games.h"
#include "wire.h"

void msg_welcome(conn *c)
{
    uint8_t p[FRAME_MAX_PAYLOAD];
    size_t n = 0;
    wire_put_u32(p + n, c->id);
    n += 4;
    wire_put_u32(p + n, c->udp_token);
    n += 4;
    size_t count_at = n++;
    uint8_t count = 0;
    for (size_t i = 0; i < games_count(); i++) {
        const game_def *g = games_at(i);
        size_t name_len = strlen(g->name);
        /* 게임 목록이 한 프레임(512)을 넘으면 넣지 못한 게임은 빠진다. 등록 목록이 작아
         * 실제로는 일어나지 않지만, 넘쳐서 프레임을 깨뜨리는 것보다 낫다. */
        if (name_len > 255 || n + 7 + name_len > sizeof p)
            break;
        wire_put_u16(p + n, g->id);
        p[n + 2] = g->min_players;
        p[n + 3] = g->max_players;
        wire_put_u16(p + n + 4, g->tick_hz);
        p[n + 6] = (uint8_t)name_len;
        memcpy(p + n + 7, g->name, name_len);
        n += 7 + name_len;
        count++;
    }
    p[count_at] = count;
    conn_send_frame(c, MSG_WELCOME, p, n);
}

void msg_error(conn *c, uint16_t code, const char *reason)
{
    uint8_t p[2 + ERROR_REASON_MAX];
    wire_put_u16(p, code);
    size_t len = strlen(reason);
    if (len > ERROR_REASON_MAX)
        len = ERROR_REASON_MAX;
    memcpy(p + 2, reason, len);
    conn_send_frame(c, MSG_ERROR, p, 2 + len);
}

void msg_room_joined(conn *c, const room *r)
{
    uint8_t p[17 + GAME_MAX_SETTINGS];
    wire_put_u32(p, r->id);
    wire_put_u16(p + 4, r->game->id);
    p[6] = (uint8_t)c->slot;
    p[7] = r->game->max_players;
    p[8] = r->visibility;
    if (r->visibility == ROOM_VISIBILITY_PRIVATE)
        memcpy(p + 9, r->code, ROOM_CODE_LEN);
    else
        memset(p + 9, 0, ROOM_CODE_LEN);
    uint8_t mask = 0;
    for (int i = 0; i < GAME_MAX_SLOTS; i++) {
        if (r->members[i] != NULL)
            mask |= (uint8_t)(1u << i);
    }
    p[15] = mask;
    p[16] = (uint8_t)r->settings_len;
    memcpy(p + 17, r->settings, r->settings_len);
    conn_send_frame(c, MSG_ROOM_JOINED, p, 17 + r->settings_len);
}

void msg_match_start(conn *c, uint32_t start_tick)
{
    uint8_t p[4];
    wire_put_u32(p, start_tick);
    conn_send_frame(c, MSG_MATCH_START, p, sizeof p);
}

void msg_pong(conn *c, uint32_t client_time_ms, uint32_t server_time_ms)
{
    uint8_t p[8];
    wire_put_u32(p, client_time_ms);
    wire_put_u32(p + 4, server_time_ms);
    conn_send_frame(c, MSG_PONG, p, sizeof p);
}

void msg_u8(conn *c, uint8_t type, uint8_t value)
{
    conn_send_frame(c, type, &value, 1);
}
