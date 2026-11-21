/* relay.c - Relay 게임. 내용을 저장하지 않으므로 경기 상태는 host 포인터 하나뿐이다. */
#include "relay.h"

#include <stdlib.h>
#include <string.h>

/* GAME_OUT = from_slot 1바이트 + 받은 바이트. 전체가 GAME_MAX_PAYLOAD 안에 들어가야 하므로
 * 받는 쪽 상한은 하나 작다(protocol.md 6.2절의 "1–500"은 GAME_OUT 쪽 상한 500과 함께
 * 지켜야 해서 GAME_IN을 499로 둔다). */
#define RELAY_MAX_INPUT (GAME_MAX_PAYLOAD - 1)

typedef struct relay_match {
    const game_host *host;
} relay_match;

static void *relay_create(const game_host *host, const uint8_t slots[GAME_MAX_SLOTS],
                          const uint8_t *settings, size_t settings_len)
{
    (void)slots;
    (void)settings;
    (void)settings_len;
    relay_match *m = malloc(sizeof *m);
    if (m == NULL)
        return NULL;
    m->host = host;
    return m;
}

static void relay_destroy(void *match)
{
    free(match);
}

static int relay_input(void *match, int slot, const uint8_t *data, size_t len)
{
    relay_match *m = match;
    uint8_t out[GAME_MAX_PAYLOAD];
    out[0] = (uint8_t)slot;
    memcpy(out + 1, data, len);
    /* 호스트 역할 클라이언트가 계산한 결과나 채팅이라 빠지면 안 된다: 신뢰 전송. */
    m->host->broadcast(m->host->ctx, slot, out, len + 1, GAME_SEND_RELIABLE);
    return 0;
}

const game_def relay_game = {
    .id = 2,
    .name = "Relay",
    .min_players = 2,
    .max_players = 8,
    .tick_hz = 0,
    .max_input_len = RELAY_MAX_INPUT,
    .settings_resolve = NULL,
    .match_create = relay_create,
    .match_destroy = relay_destroy,
    .on_join = NULL,
    .on_leave = NULL,
    .on_input = relay_input,
    .on_tick = NULL,
};
