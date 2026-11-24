/* scenarios_udp.c - UDP 시나리오 (j), (k)와 UDP 섞인 경기 도우미 (protocol.md 7장). */
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include "scenarios.h"
#include "wire.h"

#define GAME_PONG 1
#define INPUTS_PER_SEC 30
#define STATE_LEN 20
#define GOAL_LEN 4

int udp_pair(client *a, client *b, int bind_a, int bind_b, const uint8_t *settings,
             uint8_t settings_len)
{
    room_info ra, rb;
    msg m;
    if (client_connect(a, 0) != 0 || client_connect(b, 0) != 0)
        return 0;
    if ((bind_a && !client_udp_bind(a)) || (bind_b && !client_udp_bind(b)))
        return 0;
    send_create(a, GAME_PONG, ROOM_VISIBILITY_PRIVATE, settings, settings_len);
    if (!client_expect_joined(a, &ra) || ra.slot != 0)
        return 0;
    client_send(b, MSG_JOIN_BY_CODE, ra.code, ROOM_CODE_LEN);
    if (!client_expect_joined(b, &rb) || rb.room_id != ra.room_id || rb.slot != 1)
        return 0;
    if (!client_expect_u8(a, MSG_PLAYER_JOINED, 1))
        return 0;
    return client_wait_for(a, MSG_MATCH_START, &m, T_LONG) &&
           client_wait_for(b, MSG_MATCH_START, &m, T_LONG);
}

static void note(ulog *l, const msg *m, int via_udp)
{
    if (m->type != MSG_GAME_OUT) {
        l->bad_udp += via_udp;
        return;
    }
    if (m->len == STATE_LEN && m->p[0] == 0x01) {
        *(via_udp ? &l->udp_states : &l->tcp_states) += 1;
        uint32_t tick = wire_get_u32(m->p + 1), seq = wire_get_u32(m->p + 5);
        /* UDP는 순서가 바뀔 수 있어 7장대로 tick이 더 큰 STATE만 최신으로 본다. */
        if (!l->any_state || tick > l->last_tick) {
            if (l->any_state && seq < l->last_input_seq)
                l->seq_went_back++;
            l->last_tick = tick;
            l->last_input_seq = seq;
            l->any_state = 1;
        }
    } else if (m->len == GOAL_LEN && m->p[0] == 0x02) {
        *(via_udp ? &l->udp_goals : &l->tcp_goals) += 1;
    }
}

/* 지금 와 있는 것을 모두 읽는다. TCP가 끊겼으면 0. */
static int drain(client *c, ulog *l, uint8_t stop_type)
{
    msg m;
    int r;
    while ((r = client_recv(c, &m, 0)) == 1) {
        if (m.type == MSG_GAME_OUT) {
            note(l, &m, 0);
        } else if (m.type == stop_type && !l->stopped) {
            l->stopped = 1;
            l->stop = m;
        }
    }
    if (r == -1)
        return 0;
    if (c->ufd != -1) {
        while ((r = udp_recv(c->ufd, &m, 0)) != 0) {
            if (r == 1)
                note(l, &m, 1);
            else
                l->bad_udp++;
        }
    }
    return 1;
}

int udp_pump(client *cs[], ulog logs[], int n, int duration_ms, uint8_t stop_type)
{
    memset(logs, 0, sizeof *logs * (size_t)n);
    uint64_t start = itest_now_ms();
    uint64_t end = start + (uint64_t)duration_ms;
    uint64_t sent = 0, next_send = start;
    for (;;) {
        uint64_t now = itest_now_ms();
        if (now >= end)
            return 1;
        int all = stop_type != 0;
        for (int i = 0; i < n; i++)
            all = all && logs[i].stopped;
        if (all)
            return 1;
        if (now >= next_send) {
            /* 6.1절: 진행 간격과 같은 1/30초마다, n번째 = 시작 + n × 1000/30ms */
            for (int i = 0; i < n; i++) {
                if (logs[i].stopped)
                    continue;
                if (cs[i]->ufd != -1)
                    send_pong_batch_udp(cs[i], 0);
                else
                    send_pong_input(cs[i], 0);
            }
            sent++;
            next_send = start + sent * 1000u / INPUTS_PER_SEC;
            continue;
        }
        struct pollfd pfd[16];
        int np = 0;
        for (int i = 0; i < n; i++) {
            pfd[np++] = (struct pollfd){.fd = cs[i]->fd, .events = POLLIN};
            if (cs[i]->ufd != -1)
                pfd[np++] = (struct pollfd){.fd = cs[i]->ufd, .events = POLLIN};
        }
        uint64_t wake = next_send < end ? next_send : end;
        poll(pfd, (nfds_t)np, (int)(wake - now));
        for (int i = 0; i < n; i++) {
            if (!drain(cs[i], &logs[i], stop_type))
                return 0;
        }
    }
}

/* 다른 type은 건너뛰고 type이 올 때까지 기다린다. */
static int wait_skipping(client *c, uint8_t type, msg *m)
{
    uint64_t deadline = itest_now_ms() + T_LONG;
    for (uint64_t now = itest_now_ms(); now < deadline; now = itest_now_ms()) {
        if (client_recv(c, m, (int)(deadline - now)) != 1)
            return 0;
        if (m->type == type)
            return 1;
    }
    return 0;
}

/* T_SHORT 동안 fd로 온 데이터그램 중 GAME_OUT이 아닌 것(UDP_BOUND 등)의 수. 하나도 없어야 할 때 쓴다. */
static int udp_non_game_out(int fd)
{
    int count = 0;
    msg m;
    int r;
    while ((r = udp_recv(fd, &m, T_SHORT)) != 0)
        count += r == -1 || m.type != MSG_GAME_OUT;
    return count;
}

/* 데이터그램이 하나라도 오면 1(이미 와 있던 것은 먼저 비운 뒤에 쓴다). */
static int udp_got_any(int fd, int timeout_ms)
{
    msg m;
    return udp_recv(fd, &m, timeout_ms) != 0;
}

static void udp_flush(int fd)
{
    msg m;
    while (udp_recv(fd, &m, 0) != 0) {
    }
}

void scenario_j_udp_bind(void)
{
    /* 둘 다 UDP: STATE는 UDP로만, GOAL(신뢰)은 TCP로만, MATCH_END도 TCP */
    client a, b;
    uint8_t s[25];
    fast_settings(s);
    REQUIRE(udp_pair(&a, &b, 1, 1, s, sizeof s));
    client *cs[2] = {&a, &b};
    ulog L[2];
    REQUIRE(udp_pump(cs, L, 2, 10000, MSG_MATCH_END));
    for (int i = 0; i < 2; i++) {
        REQUIRE(L[i].stopped && L[i].stop.len == 1 && L[i].stop.p[0] == MATCH_END_BY_GAME);
        REQUIRE(L[i].udp_states >= 10 && L[i].tcp_states == 0);
        REQUIRE(L[i].tcp_goals == 1 && L[i].udp_goals == 0);
        /* 묶음 GAME_IN이 UDP로 들어가 진행에 쓰였다 */
        REQUIRE(L[i].last_input_seq >= 10 && L[i].seq_went_back == 0);
        REQUIRE(L[i].bad_udp == 0);
    }
    client_quit(&a);
    client_quit(&b);

    /* 섞인 방: c는 UDP, d는 TCP만. 서로 다른 경로로 같은 경기를 받는다 */
    client c, d;
    REQUIRE(udp_pair(&c, &d, 1, 0, NULL, 0));
    cs[0] = &c;
    cs[1] = &d;
    REQUIRE(udp_pump(cs, L, 2, 1200, 0));
    REQUIRE(L[0].udp_states >= 20 && L[0].tcp_states == 0 && L[0].bad_udp == 0);
    REQUIRE(L[1].tcp_states >= 20 && L[1].udp_states == 0);
    REQUIRE(L[0].last_input_seq > 0 && L[1].last_input_seq > 0);
    REQUIRE(L[0].seq_went_back == 0 && L[1].seq_went_back == 0);
    client_quit(&c);
    client_quit(&d);
}

/* 토큰 + 날 바이트 */
static void send_tok_bytes(int fd, uint32_t token, const uint8_t *bytes, size_t n)
{
    uint8_t d[700];
    wire_put_u32(d, token);
    memcpy(d + 4, bytes, n);
    udp_send_bytes(fd, d, 4 + n);
}

/* 규칙에 어긋난 데이터그램을 토큰 tok으로 fd에서 보낸다. 서버는 모두 조용히 버려야 한다. */
static void send_garbage(int fd, uint32_t tok)
{
    static const uint8_t big[600];
    static uint8_t over[FRAME_HEADER_LEN + 513];
    uint8_t g[64];
    memset(g, 0, sizeof g);
    udp_send_bytes(fd, "\x01\x18\x00", 3);                        /* 토큰도 다 안 됨 */
    udp_send_bytes(fd, g, 0);                                     /* 빈 데이터그램 */
    send_tok_bytes(fd, tok, (const uint8_t *)"\x01\x18\x00", 3);   /* 헤더가 모자람 */
    send_tok_bytes(fd, tok, (const uint8_t *)"\x02\x18\x00\x00", 4); /* 버전 2 */
    send_tok_bytes(fd, tok, (const uint8_t *)"\x01\x18\x00\x00\x01\x18\x00\x00", 8); /* 프레임 둘 */
    send_tok_bytes(fd, tok, (const uint8_t *)"\x01\x40\x00\x05\x00\x00\x00", 7); /* len 5, 3바이트 */
    send_tok_bytes(fd, tok, (const uint8_t *)"\x01\x00\x00\x00", 4); /* type 0 */
    send_tok_bytes(fd, tok, (const uint8_t *)"\x01\x89\x00\x00", 4); /* 서버 → 클라이언트 type */
    send_tok_bytes(fd, tok, (const uint8_t *)"\x01\x18\x00\x01\x00", 5); /* UDP_BIND 길이 1 */
    send_tok_bytes(fd, tok, (const uint8_t *)"\x01\x16\x00\x04\x00\x00\x00\x07", 8); /* PING은 TCP만 */
    send_tok_bytes(fd, tok, (const uint8_t *)"\x01\x40\x00\x00", 4); /* GAME_IN 길이 0 */
    udp_send_frame_tok(fd, tok, MSG_GAME_IN, g, 22); /* 게임 상한 21 초과 */
    over[0] = PROTO_VERSION; /* 프레임 상한 512 초과(len 513, 바이트도 그만큼) */
    over[1] = MSG_GAME_IN;
    wire_put_u16(over + 2, 513);
    send_tok_bytes(fd, tok, over, sizeof over);
    send_tok_bytes(fd, tok, big, sizeof big); /* 버전 0 */
    g[0] = 3; /* n = 3인데 길이 11(n = 2 자리) */
    udp_send_frame_tok(fd, tok, MSG_GAME_IN, g, 11);
    g[0] = 1; /* axis 9 */
    wire_put_u32(g + 1, 900000);
    g[5] = 9;
    udp_send_frame_tok(fd, tok, MSG_GAME_IN, g, 6);
}

void scenario_k_udp_ignored(void)
{
    client a, b, e;
    msg m;
    REQUIRE(udp_pair(&a, &b, 1, 0, NULL, 0));
    int rogue = udp_socket_open();
    REQUIRE(rogue != -1);

    /* 모르는 토큰의 UDP_BIND에는 답하지 않는다 */
    uint32_t unknown = a.udp_token ^ 0x5A5A5A5Au;
    if (unknown == b.udp_token)
        unknown ^= 1u;
    udp_send_frame_tok(rogue, unknown, MSG_UDP_BIND, NULL, 0);
    REQUIRE(!udp_got_any(rogue, T_SHORT));

    /* 묶인 주소가 아닌 곳에서 a의 토큰으로 보낸 GAME_IN은 받지 않는다: 받았다면 100000이 소비되고
     * 그 뒤 작은 번호는 모두 중복으로 버려진다 */
    uint8_t forged[6] = {1, 0, 0x01, 0x86, 0xA0, 1}; /* n 1, seq 100000, 위 */
    udp_send_frame_tok(rogue, a.udp_token, MSG_GAME_IN, forged, sizeof forged);
    udp_send_frame_tok(rogue, a.udp_token, MSG_GAME_IN, forged + 1, 5);
    send_garbage(a.ufd, a.udp_token);
    send_garbage(rogue, a.udp_token);
    client *cs[2] = {&a, &b};
    ulog L[2];
    REQUIRE(udp_pump(cs, L, 2, 700, 0));
    REQUIRE(L[0].udp_states >= 10 && L[0].tcp_states == 0 && L[0].bad_udp == 0);
    REQUIRE(L[0].last_input_seq > 0 && L[0].last_input_seq < 100000);
    REQUIRE(L[1].tcp_states >= 10);
    /* 어느 것에도 TCP로 ERROR를 보내지 않았고 연결도 그대로다 */
    send_u32(&a, MSG_PING, 4242);
    REQUIRE(client_wait_for(&a, MSG_PONG, &m, T_LONG) && wire_get_u32(m.p) == 4242);
    REQUIRE(!udp_got_any(rogue, 0));

    /* 다시 묶기: 같은 토큰으로 다른 주소(공유기가 포트를 바꾼 경우)에서 묶으면 그쪽으로 바뀐다 */
    int old = a.ufd;
    udp_flush(old);
    a.ufd = rogue;
    REQUIRE(client_udp_bind(&a));
    udp_flush(old); /* 묶기 전에 옛 주소로 이미 보낸 STATE */
    uint32_t before = L[0].last_input_seq;
    uint8_t forged2[6] = {1, 0, 0x03, 0x0D, 0x40, 2}; /* n 1, seq 200000: 옛 주소에서 */
    udp_send_frame_tok(old, a.udp_token, MSG_GAME_IN, forged2, sizeof forged2);
    REQUIRE(udp_pump(cs, L, 2, 700, 0));
    REQUIRE(L[0].udp_states >= 10 && L[0].tcp_states == 0 && L[0].bad_udp == 0);
    REQUIRE(L[0].last_input_seq > before && L[0].last_input_seq < 200000);
    REQUIRE(!udp_got_any(old, 0)); /* 옛 주소로는 더 오지 않는다 */
    close(old);

    /* 로비에 있는 묶인 연결의 UDP GAME_IN: BAD_STATE로 답하지 않고 버린다 */
    REQUIRE(client_connect(&e, 0) == 0 && client_udp_bind(&e));
    send_pong_batch_udp(&e, 1);
    send_garbage(e.ufd, e.udp_token);
    send_u32(&e, MSG_PING, 4343);
    REQUIRE(client_wait_for(&e, MSG_PONG, &m, T_LONG) && wire_get_u32(m.p) == 4343);
    REQUIRE(udp_non_game_out(e.ufd) == 0);
    client_quit(&e);

    /* TCP가 끊기면 토큰도 사라진다. b가 MATCH_END(1)을 받았으면 서버가 a를 정리한 뒤다
     * (같은 정리 단계에서 방을 나가고 표에서 지운다). */
    close(a.fd);
    a.fd = -1;
    REQUIRE(wait_skipping(&b, MSG_MATCH_END, &m) && m.len == 1 && m.p[0] == MATCH_END_TOO_FEW);
    udp_flush(a.ufd);
    udp_send_frame_tok(a.ufd, a.udp_token, MSG_UDP_BIND, NULL, 0);
    REQUIRE(!udp_got_any(a.ufd, T_SHORT));
    client_close(&a);
    client_quit(&b);
}
