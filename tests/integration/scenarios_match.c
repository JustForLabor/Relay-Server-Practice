/* scenarios_match.c - 경기 시나리오 (b), (e), (f), (g), (i). */
#include <string.h>

#include "scenarios.h"
#include "wire.h"

#define GAME_PONG 1
#define GAME_RELAY 2

/* a가 game의 공개 방을 만들고 b가 목록에서 보고 JOIN_ROOM으로 들어간다. 둘 다 MATCH_START까지. */
static int make_pair(client *a, client *b, uint16_t game, const uint8_t *settings,
                     uint8_t settings_len, room_info *ra)
{
    room_info rb;
    msg m;
    if (client_connect(a, 0) != 0 || client_connect(b, 0) != 0)
        return 0;
    send_create(a, game, ROOM_VISIBILITY_PUBLIC, settings, settings_len);
    if (!client_expect_joined(a, ra) || ra->slot != 0 || ra->mask != 0x01 || ra->visibility != 0)
        return 0;
    for (int i = 0; i < ROOM_CODE_LEN; i++) {
        if (ra->code[i] != 0) /* 공개 방 코드는 0으로 채운다 */
            return 0;
    }
    uint8_t players = 0, max = 0;
    send_u16(b, MSG_LIST_ROOMS, game);
    if (!client_wait_for(b, MSG_ROOM_LIST, &m, T_LONG) || wire_get_u16(m.p) != game ||
        !room_list_has(&m, ra->room_id, &players, &max) || players != 1 ||
        max != ra->max_players)
        return 0;
    send_u32(b, MSG_JOIN_ROOM, ra->room_id);
    if (!client_expect_joined(b, &rb) || rb.room_id != ra->room_id || rb.slot != 1 ||
        rb.mask != 0x03 || rb.settings_len != ra->settings_len ||
        memcmp(rb.settings, ra->settings, ra->settings_len) != 0)
        return 0;
    if (!client_expect_u8(a, MSG_PLAYER_JOINED, 1))
        return 0;
    if (!client_wait_for(a, MSG_MATCH_START, &m, T_LONG) || wire_get_u32(m.p) != 0)
        return 0;
    if (!client_wait_for(b, MSG_MATCH_START, &m, T_LONG) || wire_get_u32(m.p) != 0)
        return 0;
    return 1;
}

void scenario_b_public_room_pong(void)
{
    client a, b, l;
    room_info ra;
    REQUIRE(make_pair(&a, &b, GAME_PONG, NULL, 0, &ra));
    REQUIRE(ra.settings_len == 25 && ra.max_players == 2);

    client *cs[2] = {&a, &b};
    pong_log logs[2];
    REQUIRE(pump_pong(cs, logs, 2, 1200, 0));
    /* 30Hz로 1.2초면 약 36번. ASan · 스케줄 흔들림을 감안해 20번 이상이면 된다. */
    REQUIRE(logs[0].states >= 20 && logs[1].states >= 20);
    REQUIRE(logs[0].tick[0] == 0 && logs[1].tick[0] == 0); /* 첫 STATE는 start_tick */
    for (int i = 1; i < logs[0].states && i < PONG_LOG_CAP; i++)
        REQUIRE(logs[0].tick[i] == logs[0].tick[i - 1] + 1);
    int common = pong_logs_agree(&logs[0], &logs[1]);
    REQUIRE(common >= 20); /* 같은 tick이면 두 사람의 공 위치가 같다 */
    REQUIRE(logs[0].last_input_seq > 0 && logs[1].last_input_seq > 0);

    /* 경기 중인 방은 목록에서 빠진다 */
    msg m;
    REQUIRE(client_connect(&l, 0) == 0);
    send_u16(&l, MSG_LIST_ROOMS, GAME_PONG);
    REQUIRE(client_wait_for(&l, MSG_ROOM_LIST, &m, T_LONG));
    REQUIRE(!room_list_has(&m, ra.room_id, NULL, NULL));
    client_quit(&l);
    client_quit(&a);
    client_quit(&b);
}

/* 빨리 끝나는 규칙: 공이 패들을 빗나가 첫 득점으로 끝난다(win_score 1). (j)도 쓴다. */
void fast_settings(uint8_t s[25])
{
    const uint16_t v[12] = {200, 150, 2, 10, 2, 1, 2, 4, 3, 4, 8, 0};
    for (int i = 0; i < 12; i++)
        wire_put_u16(s + 2 * i, v[i]);
    s[24] = 1;
}

void scenario_e_rematch(void)
{
    client a, b, l;
    room_info ra;
    uint8_t s[25];
    fast_settings(s);
    REQUIRE(make_pair(&a, &b, GAME_PONG, s, sizeof s, &ra));
    REQUIRE(ra.settings_len == 25 && memcmp(ra.settings, s, 25) == 0);

    client *cs[2] = {&a, &b};
    pong_log logs[2];
    REQUIRE(pump_pong(cs, logs, 2, 10000, MSG_MATCH_END));
    REQUIRE(logs[0].stopped && logs[1].stopped);
    REQUIRE(logs[0].stop.len == 1 && logs[0].stop.p[0] == 0);
    REQUIRE(logs[1].stop.len == 1 && logs[1].stop.p[0] == 0);
    REQUIRE(logs[0].goals == 1 && logs[1].goals == 1);

    /* 막 끝난 방은 목록에 나오지 않는다 */
    msg m;
    REQUIRE(client_connect(&l, 0) == 0);
    send_u16(&l, MSG_LIST_ROOMS, GAME_PONG);
    REQUIRE(client_wait_for(&l, MSG_ROOM_LIST, &m, T_LONG));
    REQUIRE(!room_list_has(&m, ra.room_id, NULL, NULL));

    /* 대기 중 GAME_IN은 조용히 버린다: 다음 답이 PONG이어야 한다 */
    send_pong_input(&a, 0);
    send_u32(&a, MSG_PING, 77);
    REQUIRE(client_wait_for(&a, MSG_PONG, &m, T_LONG) && wire_get_u32(m.p) == 77);

    /* 같은 사람이 두 번 보내도 REMATCH_VOTED는 한 번 */
    client_send(&a, MSG_REMATCH, NULL, 0);
    client_send(&a, MSG_REMATCH, NULL, 0);
    send_u32(&a, MSG_PING, 78);
    REQUIRE(client_expect_u8(&a, MSG_REMATCH_VOTED, 0));
    REQUIRE(client_wait_for(&a, MSG_PONG, &m, T_LONG) && wire_get_u32(m.p) == 78);
    REQUIRE(client_expect_u8(&b, MSG_REMATCH_VOTED, 0));
    client_send(&b, MSG_REMATCH, NULL, 0);
    REQUIRE(client_expect_u8(&a, MSG_REMATCH_VOTED, 1));
    REQUIRE(client_expect_u8(&b, MSG_REMATCH_VOTED, 1));
    REQUIRE(client_wait_for(&a, MSG_MATCH_START, &m, T_LONG) && wire_get_u32(m.p) == 0);
    REQUIRE(client_wait_for(&b, MSG_MATCH_START, &m, T_LONG) && wire_get_u32(m.p) == 0);

    /* 새 경기는 처음부터: 진행 번호 0, 점수 0에서 다시 시작해 다시 한 번 끝난다 */
    REQUIRE(pump_pong(cs, logs, 2, 10000, MSG_MATCH_END));
    REQUIRE(logs[0].states > 0 && logs[0].tick[0] == 0);
    REQUIRE(logs[0].stopped && logs[0].stop.p[0] == 0 && logs[0].goals == 1);

    /* 경기 중이 아닌 방에서 다시 REMATCH 없이 나가면 상대는 PLAYER_LEFT */
    client_quit(&b);
    REQUIRE(client_expect_u8(&a, MSG_PLAYER_LEFT, 1));
    client_quit(&a);
    client_quit(&l);
}

void scenario_f_disconnect_mid_match(void)
{
    client a, b, l;
    room_info ra;
    msg m;
    REQUIRE(make_pair(&a, &b, GAME_PONG, NULL, 0, &ra));
    client *cs[2] = {&a, &b};
    pong_log logs[2];
    REQUIRE(pump_pong(cs, logs, 2, 200, 0));
    client_close(&b); /* 인사 없이 끊는다 */
    REQUIRE(client_expect_u8(&a, MSG_PLAYER_LEFT, 1));
    REQUIRE(client_expect_u8(&a, MSG_MATCH_END, 1));

    /* 남은 사람의 방은 다시 대기 공개 방으로 목록에 나온다 */
    uint8_t players = 0;
    REQUIRE(client_connect(&l, 0) == 0);
    send_u16(&l, MSG_LIST_ROOMS, GAME_PONG);
    REQUIRE(client_wait_for(&l, MSG_ROOM_LIST, &m, T_LONG));
    REQUIRE(room_list_has(&m, ra.room_id, &players, NULL) && players == 1);
    client_quit(&l);
    client_quit(&a);
}

void scenario_g_relay_three(void)
{
    client a, b, c;
    room_info ra, rc;
    msg m;
    REQUIRE(make_pair(&a, &b, GAME_RELAY, NULL, 0, &ra));
    REQUIRE(ra.settings_len == 0 && ra.max_players == 8);
    /* 경기 중에도 빈자리가 있으면 들어온다: ROOM_JOINED 다음 바로 MATCH_START */
    REQUIRE(client_connect(&c, 0) == 0);
    send_u32(&c, MSG_JOIN_ROOM, ra.room_id);
    REQUIRE(client_expect_joined(&c, &rc));
    REQUIRE(rc.slot == 2 && rc.mask == 0x07);
    REQUIRE(client_recv(&c, &m, T_LONG) == 1 && m.type == MSG_MATCH_START &&
            wire_get_u32(m.p) == 0);
    REQUIRE(client_expect_u8(&a, MSG_PLAYER_JOINED, 2));
    REQUIRE(client_expect_u8(&b, MSG_PLAYER_JOINED, 2));

    const uint8_t hello[] = {'h', 'e', 'l', 'l', 'o'};
    client_send(&b, MSG_GAME_IN, hello, sizeof hello);
    client *others[2] = {&a, &c};
    for (int i = 0; i < 2; i++) {
        REQUIRE(client_recv(others[i], &m, T_LONG) == 1 && m.type == MSG_GAME_OUT);
        REQUIRE(m.len == 6 && m.p[0] == 1 && memcmp(m.p + 1, hello, 5) == 0);
    }
    /* 보낸 사람에게는 오지 않는다: GAME_OUT이 있었다면 PONG보다 먼저 와야 한다 */
    send_u32(&b, MSG_PING, 5);
    REQUIRE(client_recv(&b, &m, T_LONG) == 1 && m.type == MSG_PONG);

    uint8_t big[499];
    for (size_t i = 0; i < sizeof big; i++)
        big[i] = (uint8_t)(i * 13);
    client_send(&c, MSG_GAME_IN, big, sizeof big);
    client *to[2] = {&a, &b};
    for (int i = 0; i < 2; i++) {
        REQUIRE(client_recv(to[i], &m, T_LONG) == 1 && m.type == MSG_GAME_OUT);
        REQUIRE(m.len == 500 && m.p[0] == 2 && memcmp(m.p + 1, big, sizeof big) == 0);
    }

    /* 한 명이 나가도 최소 인원(2) 이상이면 경기는 계속된다 */
    client_quit(&c);
    REQUIRE(client_expect_u8(&a, MSG_PLAYER_LEFT, 2));
    client_send(&a, MSG_GAME_IN, hello, 1);
    REQUIRE(client_recv(&b, &m, T_LONG) == 1 && m.type == MSG_PLAYER_LEFT);
    REQUIRE(client_recv(&b, &m, T_LONG) == 1 && m.type == MSG_GAME_OUT && m.p[0] == 0);
    client_quit(&a);
    client_quit(&b);
}

/* 읽지 않는 클라이언트: 송신 버퍼가 64KB를 넘으면 그 연결만 끊긴다. */
void scenario_i_slow_reader(void)
{
    client a, slow;
    room_info ra, rs;
    msg m;
    REQUIRE(client_connect(&a, 0) == 0);
    REQUIRE(client_connect(&slow, 4096) == 0);
    send_create(&a, GAME_RELAY, ROOM_VISIBILITY_PUBLIC, NULL, 0);
    REQUIRE(client_expect_joined(&a, &ra));
    send_u32(&slow, MSG_JOIN_ROOM, ra.room_id);
    REQUIRE(client_expect_joined(&slow, &rs));
    REQUIRE(client_wait_for(&slow, MSG_MATCH_START, &m, T_LONG));
    REQUIRE(client_expect_u8(&a, MSG_PLAYER_JOINED, rs.slot));
    REQUIRE(client_wait_for(&a, MSG_MATCH_START, &m, T_LONG));

    /* slow는 이제 읽지 않는다. a가 499바이트 입력을 쏟아붓는다. 커널 버퍼(수 MB)가 찬 뒤
     * 서버 송신 버퍼가 64KB를 넘으면 slow가 끊기고 a에게 PLAYER_LEFT · MATCH_END(1)이 온다. */
    uint8_t batch[32 * (4 + 499)];
    for (int i = 0; i < 32; i++) {
        uint8_t *f = batch + i * (4 + 499);
        f[0] = 1;
        f[1] = MSG_GAME_IN;
        wire_put_u16(f + 2, 499);
        memset(f + 4, 'x', 499);
    }
    int left = 0;
    uint64_t deadline = itest_now_ms() + 20000;
    size_t sent = 0;
    while (!left && itest_now_ms() < deadline && sent < (size_t)256 << 20) {
        client_send_raw(&a, batch, sizeof batch);
        sent += sizeof batch;
        int r = 0;
        /* PLAYER_LEFT를 보면 바로 멈춰, 뒤따르는 MATCH_END(1)을 아래에서 순서대로 확인한다. */
        while (!left && (r = client_recv(&a, &m, 0)) == 1) {
            if (m.type == MSG_PLAYER_LEFT && m.len == 1 && m.p[0] == rs.slot)
                left = 1;
        }
        if (left)
            break;
        REQUIRE(r != -1);
    }
    REQUIRE(left);
    REQUIRE(client_expect_u8(&a, MSG_MATCH_END, 1));
    /* slow 쪽에서 보면: 쌓인 것을 다 읽고 나면 연결이 닫혀 있다 */
    REQUIRE(client_wait_closed(&slow, 20000, NULL));
    client_close(&slow);
    client_quit(&a);
}
