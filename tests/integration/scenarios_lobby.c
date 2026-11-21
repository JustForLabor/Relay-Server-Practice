/* scenarios_lobby.c - 접속 · 로비 · 입장 코드 · 견고성 시나리오 (a), (c), (d), (h). */
#include <string.h>

#include "scenarios.h"
#include "wire.h"

#define GAME_PONG 1
#define GAME_RELAY 2

/* WELCOME 게임 항목 하나를 읽고 다음 위치를 돌려준다. */
static size_t check_game_entry(const msg *w, size_t at, uint16_t id, uint8_t min, uint8_t max,
                               uint16_t hz, const char *name, int *ok)
{
    size_t nl = strlen(name);
    if (at + 7 + nl > w->len || wire_get_u16(w->p + at) != id || w->p[at + 2] != min ||
        w->p[at + 3] != max || wire_get_u16(w->p + at + 4) != hz || w->p[at + 6] != nl ||
        memcmp(w->p + at + 7, name, nl) != 0)
        *ok = 0;
    return at + 7 + nl;
}

void scenario_a_welcome_ping(void)
{
    client a, b;
    REQUIRE(client_connect(&a, 0) == 0);
    REQUIRE(client_connect(&b, 0) == 0);
    REQUIRE(a.conn_id >= 1);
    REQUIRE(b.conn_id == a.conn_id + 1); /* conn_id는 접속 순서대로 1씩 는다 */
    REQUIRE(a.udp_token != b.udp_token);
    const msg *w = &a.welcome;
    REQUIRE(w->p[8] == 2); /* 등록 게임 2개: Pong, Relay */
    int ok = 1;
    size_t at = check_game_entry(w, 9, GAME_PONG, 2, 2, 30, "Pong", &ok);
    at = check_game_entry(w, at, GAME_RELAY, 2, 8, 0, "Relay", &ok);
    REQUIRE(ok);
    REQUIRE(at == w->len); /* 9 + (7 + 4) + (7 + 5) = 32 */

    msg m;
    send_u32(&a, MSG_PING, 0xDEADBEEFu);
    REQUIRE(client_wait_for(&a, MSG_PONG, &m, T_LONG));
    REQUIRE(m.len == 8 && wire_get_u32(m.p) == 0xDEADBEEFu);
    uint32_t t1 = wire_get_u32(m.p + 4);
    send_u32(&a, MSG_PING, 7);
    REQUIRE(client_wait_for(&a, MSG_PONG, &m, T_LONG));
    REQUIRE(wire_get_u32(m.p) == 7 && wire_get_u32(m.p + 4) - t1 < 10000u);

    /* 상태 · 값 오류는 ERROR로 답하고 연결을 유지한다(5.4절) */
    client_send(&a, MSG_LEAVE_ROOM, NULL, 0);
    REQUIRE(client_expect_error(&a, ERR_BAD_STATE));
    client_send(&a, MSG_REMATCH, NULL, 0);
    REQUIRE(client_expect_error(&a, ERR_BAD_STATE));
    uint8_t game_in = 1;
    client_send(&a, MSG_GAME_IN, &game_in, 1);
    REQUIRE(client_expect_error(&a, ERR_BAD_STATE));
    send_u16(&a, MSG_LIST_ROOMS, 99);
    REQUIRE(client_expect_error(&a, ERR_UNKNOWN_GAME));
    send_u16(&a, MSG_QUICK_JOIN, 0);
    REQUIRE(client_expect_error(&a, ERR_UNKNOWN_GAME));
    uint8_t one = 0;
    send_create(&a, GAME_RELAY, 0, &one, 1); /* Relay는 규칙 값이 없다 */
    REQUIRE(client_expect_error(&a, ERR_BAD_SETTINGS));
    uint8_t bad_pong[25] = {0}; /* field_w 0: 범위 밖 */
    send_create(&a, GAME_PONG, 0, bad_pong, sizeof bad_pong);
    REQUIRE(client_expect_error(&a, ERR_BAD_SETTINGS));
    send_create(&a, GAME_PONG, 0, bad_pong, 3); /* 길이가 0도 25도 아님 */
    REQUIRE(client_expect_error(&a, ERR_BAD_SETTINGS));
    send_u32(&a, MSG_JOIN_ROOM, 0x7FFFFFF0u);
    REQUIRE(client_expect_error(&a, ERR_ROOM_NOT_FOUND));
    send_u32(&a, MSG_PING, 8);
    REQUIRE(client_wait_for(&a, MSG_PONG, &m, T_LONG) && wire_get_u32(m.p) == 8);

    client_quit(&a);
    client_quit(&b);
}

static int code_ok(const char code[ROOM_CODE_LEN])
{
    for (int i = 0; i < ROOM_CODE_LEN; i++) {
        if (strchr("23456789ABCDEFGHJKLMNPQRSTUVWXYZ", code[i]) == NULL || code[i] == '\0')
            return 0;
    }
    return 1;
}

void scenario_c_private_code(void)
{
    client a, b, d, e, l;
    room_info ra, rb;
    msg m;
    REQUIRE(client_connect(&a, 0) == 0);
    send_create(&a, GAME_PONG, ROOM_VISIBILITY_PRIVATE, NULL, 0);
    REQUIRE(client_expect_joined(&a, &ra));
    REQUIRE(ra.visibility == 1 && ra.slot == 0 && ra.mask == 0x01);
    REQUIRE(code_ok(ra.code));

    /* 비공개 방은 목록에 나오지 않고 번호로도 들어갈 수 없다 */
    REQUIRE(client_connect(&l, 0) == 0);
    send_u16(&l, MSG_LIST_ROOMS, GAME_PONG);
    REQUIRE(client_wait_for(&l, MSG_ROOM_LIST, &m, T_LONG));
    REQUIRE(!room_list_has(&m, ra.room_id, NULL, NULL));
    send_u32(&l, MSG_JOIN_ROOM, ra.room_id);
    REQUIRE(client_expect_error(&l, ERR_ROOM_NOT_FOUND));

    REQUIRE(client_connect(&b, 0) == 0);
    client_send(&b, MSG_JOIN_BY_CODE, ra.code, ROOM_CODE_LEN);
    REQUIRE(client_expect_joined(&b, &rb));
    REQUIRE(rb.room_id == ra.room_id && rb.slot == 1 && rb.mask == 0x03);
    REQUIRE(memcmp(rb.code, ra.code, ROOM_CODE_LEN) == 0);
    REQUIRE(client_expect_u8(&a, MSG_PLAYER_JOINED, 1));
    REQUIRE(client_wait_for(&a, MSG_MATCH_START, &m, T_LONG));
    REQUIRE(client_wait_for(&b, MSG_MATCH_START, &m, T_LONG));

    /* 꽉 찬 방에 맞는 코드: ROOM_FULL(틀린 코드로 세지 않는다) */
    REQUIRE(client_connect(&e, 0) == 0);
    client_send(&e, MSG_JOIN_BY_CODE, ra.code, ROOM_CODE_LEN);
    REQUIRE(client_expect_error(&e, ERR_ROOM_FULL));

    /* 틀린 코드 4번은 BAD_CODE, 5번째에 TOO_MANY_CODE_TRIES 후 끊김.
     * 소문자 · 알파벳 밖 글자도 틀린 코드로 센다. */
    REQUIRE(client_connect(&d, 0) == 0);
    char wrong[5][ROOM_CODE_LEN];
    for (int i = 0; i < 5; i++) {
        memcpy(wrong[i], ra.code, ROOM_CODE_LEN);
        wrong[i][0] = ra.code[0] == '2' ? '3' : '2';
        wrong[i][5] = "23456"[i];
    }
    for (int k = 0; k < ROOM_CODE_LEN; k++)
        wrong[1][k] = (char)(ra.code[k] >= 'A' ? ra.code[k] + 32 : '0');
    wrong[2][3] = 'O';
    for (int i = 0; i < 4; i++) {
        client_send(&d, MSG_JOIN_BY_CODE, wrong[i], ROOM_CODE_LEN);
        REQUIRE(client_expect_error(&d, ERR_BAD_CODE));
    }
    client_send(&d, MSG_JOIN_BY_CODE, wrong[4], ROOM_CODE_LEN);
    REQUIRE(client_expect_error(&d, ERR_TOO_MANY_CODE_TRIES));
    REQUIRE(client_wait_closed(&d, T_LONG, NULL));
    client_close(&d);

    client_quit(&a);
    client_quit(&b);
    client_quit(&e);
    client_quit(&l);
}

void scenario_d_quick_join(void)
{
    client a, b;
    room_info ra, rb;
    msg m;
    REQUIRE(client_connect(&a, 0) == 0);
    REQUIRE(client_connect(&b, 0) == 0);
    send_u16(&a, MSG_QUICK_JOIN, GAME_PONG);
    REQUIRE(client_expect_joined(&a, &ra));
    REQUIRE(ra.game_id == GAME_PONG && ra.visibility == 0 && ra.slot == 0);
    REQUIRE(ra.settings_len == 25); /* 새로 만든 방은 서버 기본 규칙 값 */
    send_u16(&b, MSG_QUICK_JOIN, GAME_PONG);
    REQUIRE(client_expect_joined(&b, &rb));
    REQUIRE(rb.room_id == ra.room_id && rb.slot == 1 && rb.mask == 0x03);
    REQUIRE(memcmp(rb.settings, ra.settings, 25) == 0);
    REQUIRE(client_expect_u8(&a, MSG_PLAYER_JOINED, 1));
    REQUIRE(client_wait_for(&a, MSG_MATCH_START, &m, T_LONG) && wire_get_u32(m.p) == 0);
    REQUIRE(client_wait_for(&b, MSG_MATCH_START, &m, T_LONG) && wire_get_u32(m.p) == 0);
    client_quit(&a);
    client_quit(&b);
}

/* 위반 바이트를 보낸 연결은 ERROR 없이 끊기고, 다른 연결은 계속 응답을 받는다. */
static void expect_cut(client *ok, const uint8_t *bytes, size_t n, const char *what)
{
    client x;
    if (client_connect(&x, 0) != 0) {
        itest_fail(__FILE__, __LINE__, what);
        return;
    }
    client_send_raw(&x, bytes, n);
    int frames = -1;
    if (!client_wait_closed(&x, T_LONG, &frames) || frames != 0)
        itest_fail(__FILE__, __LINE__, what);
    client_close(&x);
    msg m;
    uint32_t mark = 0xA0000000u + (uint32_t)n;
    send_u32(ok, MSG_PING, mark);
    if (!client_wait_for(ok, MSG_PONG, &m, T_LONG) || wire_get_u32(m.p) != mark)
        itest_fail(__FILE__, __LINE__, "server stopped answering after a bad client");
}

void scenario_h_robustness(void)
{
    client ok;
    REQUIRE(client_connect(&ok, 0) == 0);
    const uint8_t bad_version[] = {2, MSG_PING, 0, 4, 0, 0, 0, 1};
    expect_cut(&ok, bad_version, sizeof bad_version, "version 2 not cut");
    const uint8_t undefined_type[] = {1, 0x20, 0, 0};
    expect_cut(&ok, undefined_type, sizeof undefined_type, "undefined type not cut");
    const uint8_t zero_type[] = {1, 0x00, 0, 0};
    expect_cut(&ok, zero_type, sizeof zero_type, "type 0 not cut");
    const uint8_t len513[] = {1, MSG_GAME_IN, 0x02, 0x01}; /* 헤더만: 페이로드를 기다리지 않아야 함 */
    expect_cut(&ok, len513, sizeof len513, "len 513 not cut");
    const uint8_t s2c[] = {1, MSG_WELCOME, 0, 0};
    expect_cut(&ok, s2c, sizeof s2c, "server-to-client type not cut");
    const uint8_t s2c_game[] = {1, MSG_GAME_OUT, 0, 1, 0};
    expect_cut(&ok, s2c_game, sizeof s2c_game, "GAME_OUT from client not cut");
    const uint8_t wrong_len[] = {1, MSG_LIST_ROOMS, 0, 3, 0, 1, 0};
    expect_cut(&ok, wrong_len, sizeof wrong_len, "wrong length not cut");
    const uint8_t create_mismatch[] = {1, MSG_CREATE_ROOM, 0, 5, 0, 1, 0, 0, 7};
    expect_cut(&ok, create_mismatch, sizeof create_mismatch, "settings_len mismatch not cut");
    client_quit(&ok);
}
