/* channel.c - game_host의 send · broadcast · match_over. */
#include "channel.h"

#include <stdio.h>

static bool valid_len(size_t len)
{
    /* 게임 모듈의 잘못을 그대로 내보내면 클라이언트가 프레임 위반으로 끊긴다. 버리고 기록한다. */
    if (len >= 1 && len <= GAME_MAX_PAYLOAD)
        return true;
    fprintf(stderr, "game sent GAME_OUT of bad length %zu, dropped\n", len);
    return false;
}

static void host_send(void *ctx, int slot, const uint8_t *data, size_t len,
                      enum game_send_mode mode)
{
    /* 1차는 신뢰 · 비신뢰 모두 같은 TCP 연결로 보낸다. */
    (void)mode;
    room *r = ctx;
    if (slot < 0 || slot >= GAME_MAX_SLOTS || r->members[slot] == NULL || !valid_len(len))
        return;
    conn_send_frame(r->members[slot], MSG_GAME_OUT, data, len);
}

static void host_broadcast(void *ctx, int except_slot, const uint8_t *data, size_t len,
                           enum game_send_mode mode)
{
    (void)mode;
    room *r = ctx;
    if (!valid_len(len))
        return;
    for (int i = 0; i < GAME_MAX_SLOTS; i++) {
        if (i != except_slot && r->members[i] != NULL)
            conn_send_frame(r->members[i], MSG_GAME_OUT, data, len);
    }
}

static void host_match_over(void *ctx)
{
    /* 표시만 한다. 실제 MATCH_END · match_destroy는 콜백이 돌아온 뒤 match_settle이 한다.
     * 두 번 불려도 표시는 하나라 한 번만 처리된다. */
    room *r = ctx;
    r->over_pending = true;
}

void channel_bind(room *r)
{
    r->host.ctx = r;
    r->host.send = host_send;
    r->host.broadcast = host_broadcast;
    r->host.match_over = host_match_over;
}
