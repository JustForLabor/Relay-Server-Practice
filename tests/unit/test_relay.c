/* test_relay.c - Relay 게임을 가짜 host 위에서 돌린다 (protocol.md 6.2절).
 * 가짜 host의 broadcast는 코어(channel.c)처럼 except_slot을 뺀 사람에게 나눠 주고 slot별로
 * 받은 바이트를 기록한다. 3명 방에서 slot 1이 보낸 것이 slot 0 · 2에만 from_slot과 함께 가야 한다. */
#include <string.h>

#include "games/relay.h"
#include "unit_check.h"

typedef struct inbox {
    int count;
    uint8_t last[GAME_MAX_PAYLOAD];
    size_t last_len;
    int last_mode;
} inbox;

typedef struct fake_host {
    uint8_t present[GAME_MAX_SLOTS];
    inbox box[GAME_MAX_SLOTS];
    int over_calls;
    int bad_len;
} fake_host;

static void deliver(fake_host *h, int slot, const uint8_t *data, size_t len, int mode)
{
    if (len < 1 || len > GAME_MAX_PAYLOAD) {
        h->bad_len++;
        return;
    }
    inbox *b = &h->box[slot];
    b->count++;
    memcpy(b->last, data, len);
    b->last_len = len;
    b->last_mode = mode;
}

static void fake_send(void *ctx, int slot, const uint8_t *data, size_t len,
                      enum game_send_mode mode)
{
    fake_host *h = ctx;
    if (slot >= 0 && slot < GAME_MAX_SLOTS && h->present[slot])
        deliver(h, slot, data, len, (int)mode);
}

static void fake_broadcast(void *ctx, int except_slot, const uint8_t *data, size_t len,
                           enum game_send_mode mode)
{
    fake_host *h = ctx;
    for (int i = 0; i < GAME_MAX_SLOTS; i++) {
        if (i != except_slot && h->present[i])
            deliver(h, i, data, len, (int)mode);
    }
}

static void fake_over(void *ctx)
{
    ((fake_host *)ctx)->over_calls++;
}

static void test_definition(void)
{
    CHECK_EQ(relay_game.id, 2);
    CHECK(strcmp(relay_game.name, "Relay") == 0);
    CHECK_EQ(relay_game.min_players, 2);
    CHECK_EQ(relay_game.max_players, 8);
    CHECK_EQ(relay_game.tick_hz, 0);
    CHECK(relay_game.settings_resolve == NULL); /* 규칙 값 없음: 코어가 settings_len 0만 받는다 */
    CHECK(relay_game.on_tick == NULL);
    CHECK(relay_game.max_input_len >= 1);
    /* from_slot 1바이트를 붙인 GAME_OUT이 상한 500을 넘지 않아야 한다 */
    CHECK(relay_game.max_input_len + 1 <= GAME_MAX_PAYLOAD);
}

static void test_three_players(void)
{
    fake_host fh;
    memset(&fh, 0, sizeof fh);
    fh.present[0] = fh.present[1] = fh.present[2] = 1;
    game_host host = {&fh, fake_send, fake_broadcast, fake_over};
    uint8_t slots[GAME_MAX_SLOTS] = {1, 1, 1, 0, 0, 0, 0, 0};
    void *m = relay_game.match_create(&host, slots, NULL, 0);
    CHECK(m != NULL);
    if (m == NULL)
        return;

    const uint8_t hello[] = {'h', 'e', 'l', 'l', 'o'};
    CHECK_EQ(relay_game.on_input(m, 1, hello, sizeof hello), 0);
    CHECK_EQ(fh.box[1].count, 0);
    for (int s = 0; s <= 2; s += 2) {
        CHECK_EQ(fh.box[s].count, 1);
        CHECK_EQ(fh.box[s].last_len, 1 + sizeof hello);
        CHECK_EQ(fh.box[s].last[0], 1);
        CHECK(memcmp(fh.box[s].last + 1, hello, sizeof hello) == 0);
        CHECK_EQ(fh.box[s].last_mode, GAME_SEND_RELIABLE);
    }

    /* 가장 긴 입력: 받은 바이트 그대로 + from_slot 하나 = 정확히 상한 */
    uint8_t big[GAME_MAX_PAYLOAD];
    for (size_t i = 0; i < sizeof big; i++)
        big[i] = (uint8_t)i;
    size_t n = relay_game.max_input_len;
    CHECK_EQ(relay_game.on_input(m, 2, big, n), 0);
    CHECK_EQ(fh.box[2].count, 1); /* 보낸 slot 2는 첫 메시지 하나만 받은 채로 그대로 */
    CHECK_EQ(fh.box[0].count, 2);
    CHECK_EQ(fh.box[1].count, 1);
    CHECK_EQ(fh.box[0].last_len, n + 1);
    CHECK_EQ(fh.box[1].last[0], 2);
    CHECK(memcmp(fh.box[1].last + 1, big, n) == 0);

    /* 내용을 해석하지 않으므로 아무 바이트 1개도 받아 준다 */
    const uint8_t zero = 0;
    CHECK_EQ(relay_game.on_input(m, 0, &zero, 1), 0);
    CHECK_EQ(fh.box[1].last_len, 2);
    CHECK_EQ(fh.box[1].last[0], 0);

    CHECK_EQ(fh.over_calls, 0); /* Relay는 스스로 경기를 끝내지 않는다 */
    CHECK_EQ(fh.bad_len, 0);
    relay_game.match_destroy(m);
}

int main(void)
{
    test_definition();
    test_three_players();
    return UNIT_DONE("test_relay");
}
