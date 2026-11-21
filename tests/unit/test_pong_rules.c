/* test_pong_rules.c - Pong 규칙 값 · 한 번 진행 · 메시지 바이트 단위 테스트 (protocol.md 6.1절).
 *
 * host 없이 순수 함수(pong_settings_*, pong_step, pong_write_*)만 확인한다.
 * host를 거치는 입력 큐 · 진행 · 경기 끝은 test_pong.c가 맡는다. 실패가 있으면 1로 끝난다.
 */
#include <stdio.h>
#include <string.h>

#include "../../server/src/games/pong.h"

static int g_checks, g_fails, g_tests;

#define CHECK(c)                                                                \
    do {                                                                        \
        g_checks++;                                                             \
        if (!(c)) {                                                             \
            g_fails++;                                                          \
            fprintf(stderr, "%s:%d: CHECK(%s) 실패\n", __FILE__, __LINE__, #c); \
        }                                                                       \
    } while (0)

#define CHECK_EQ(a, b)                                                          \
    do {                                                                        \
        long long va_ = (long long)(a), vb_ = (long long)(b);                   \
        g_checks++;                                                             \
        if (va_ != vb_) {                                                       \
            g_fails++;                                                          \
            fprintf(stderr, "%s:%d: %s == %s 실패 (%lld != %lld)\n", __FILE__, \
                    __LINE__, #a, #b, va_, vb_);                                \
        }                                                                       \
    } while (0)

static unsigned rd16(const uint8_t *p)
{
    return ((unsigned)p[0] << 8) | p[1];
}

/* 서브 대기 없이 바로 진행 중인 상태를 만든다. */
static void play_state(pong_state *st, const pong_settings *s, int32_t x, int32_t y,
                       int32_t vx, int32_t vy)
{
    pong_state_init(st, s);
    st->phase = PONG_PHASE_PLAY;
    st->ball_x = x;
    st->ball_y = y;
    st->vx = vx;
    st->vy = vy;
}

/* ---- 규칙 값 ---- */

static const uint8_t default_bytes[PONG_SETTINGS_LEN] = {
    0x03, 0x20, 0x02, 0x58, 0x00, 0x0A, 0x00, 0x64, 0x00, 0x14, 0x00, 0x0C, 0x00,
    0x0A, 0x00, 0x06, 0x00, 0x03, 0x00, 0x0E, 0x00, 0x08, 0x00, 0x1E, 0x05};

static int resolve_ok(const pong_settings *s)
{
    uint8_t in[PONG_SETTINGS_LEN], out[GAME_MAX_SETTINGS];
    size_t out_len = 0;
    pong_settings_write(s, in);
    if (pong_game.settings_resolve(in, sizeof in, out, &out_len) != 0)
        return 0;
    /* 받아 준 값은 같은 25바이트 그대로 방에 고정된다 */
    return out_len == PONG_SETTINGS_LEN && memcmp(in, out, sizeof in) == 0;
}

static void test_def(void)
{
    g_tests++;
    CHECK_EQ(pong_game.id, 1);
    CHECK(strcmp(pong_game.name, "Pong") == 0);
    CHECK_EQ(pong_game.min_players, 2);
    CHECK_EQ(pong_game.max_players, 2);
    CHECK_EQ(pong_game.tick_hz, 30);
    CHECK_EQ(pong_game.max_input_len, 5);
    CHECK(pong_game.on_join == NULL && pong_game.on_leave == NULL);
    CHECK(pong_game.settings_resolve && pong_game.match_create && pong_game.match_destroy);
    CHECK(pong_game.on_input && pong_game.on_tick);
}

static void test_settings_default_and_length(void)
{
    g_tests++;
    uint8_t out[GAME_MAX_SETTINGS];
    size_t out_len = 0;
    CHECK_EQ(pong_game.settings_resolve(NULL, 0, out, &out_len), 0);
    CHECK_EQ(out_len, 25);
    CHECK(memcmp(out, default_bytes, sizeof default_bytes) == 0);

    pong_settings s, d;
    pong_settings_default(&d);
    CHECK_EQ(pong_settings_parse(out, out_len, &s), 0);
    CHECK(memcmp(&s, &d, sizeof s) == 0);

    uint8_t big[26] = {0};
    memcpy(big, default_bytes, sizeof default_bytes);
    size_t bad_lens[] = {1, 24, 26};
    for (size_t i = 0; i < sizeof bad_lens / sizeof bad_lens[0]; i++)
        CHECK(pong_game.settings_resolve(big, bad_lens[i], out, &out_len) < 0);
    CHECK_EQ(pong_game.settings_resolve(big, 25, out, &out_len), 0);
}

/* base에서 필드 하나만 바꿔 받아 주는지 본다. */
#define TRY(base, field, value, expect)      \
    do {                                     \
        pong_settings t_ = (base);           \
        t_.field = (value);                  \
        CHECK_EQ(resolve_ok(&t_), (expect)); \
    } while (0)

static void test_settings_ranges(void)
{
    g_tests++;
    /* 의존 범위에 걸리지 않게 속도를 낮춘 기준값: 각 필드 자체의 범위만 본다 */
    pong_settings b;
    pong_settings_default(&b);
    b.max_vx = 4;
    b.serve_vx = 4;
    CHECK_EQ(resolve_ok(&b), 1);

    TRY(b, field_w, 200, 1);     TRY(b, field_w, 199, 0);
    TRY(b, field_w, 4000, 1);    TRY(b, field_w, 4001, 0);
    TRY(b, field_h, 200, 1);     TRY(b, field_h, 149, 0);
    TRY(b, field_h, 4000, 1);    TRY(b, field_h, 4001, 0);
    TRY(b, paddle_w, 2, 1);      TRY(b, paddle_w, 1, 0);      TRY(b, paddle_w, 101, 0);
    TRY(b, paddle_h, 10, 1);     TRY(b, paddle_h, 9, 0);
    TRY(b, paddle_speed, 1, 1);  TRY(b, paddle_speed, 0, 0);
    TRY(b, paddle_speed, 100, 1); TRY(b, paddle_speed, 101, 0);
    TRY(b, ball_size, 2, 1);     TRY(b, ball_size, 1, 0);
    TRY(b, ball_size, 100, 1);   TRY(b, ball_size, 101, 0);
    TRY(b, serve_vx, 1, 1);      TRY(b, serve_vx, 0, 0);
    TRY(b, max_vx, 0, 0);
    TRY(b, max_vy, 100, 1);      TRY(b, max_vy, 101, 0);
    TRY(b, serve_wait_ticks, 0, 1); TRY(b, serve_wait_ticks, 300, 1);
    TRY(b, serve_wait_ticks, 301, 0);
    TRY(b, win_score, 1, 1);     TRY(b, win_score, 0, 0);
    TRY(b, win_score, 21, 1);    TRY(b, win_score, 22, 0);

    pong_settings t = b;
    t.serve_vy = 0;
    t.max_vy = 0;
    CHECK_EQ(resolve_ok(&t), 1);
}

static void test_settings_dependent(void)
{
    g_tests++;
    pong_settings d;
    pong_settings_default(&d);
    /* paddle_h: 10 .. field_h/2 */
    TRY(d, paddle_h, 300, 1);
    TRY(d, paddle_h, 301, 0);
    pong_settings t = d;
    t.field_h = 151; /* 151/2 = 75 (0 쪽으로 자름) */
    t.paddle_h = 75;
    CHECK_EQ(resolve_ok(&t), 1);
    t.paddle_h = 76;
    CHECK_EQ(resolve_ok(&t), 0);
    /* paddle_inset: paddle_w .. field_w/4 */
    TRY(d, paddle_inset, 10, 1);
    TRY(d, paddle_inset, 9, 0);
    TRY(d, paddle_inset, 200, 1);
    TRY(d, paddle_inset, 201, 0);
    /* max_vx: 1 .. ball_size + paddle_w (= 20) */
    TRY(d, max_vx, 20, 1);
    TRY(d, max_vx, 21, 0);
    TRY(d, ball_size, 3, 0); /* 3 + 10 < max_vx 14 */
    /* serve_vx: 1 .. max_vx (= 14) */
    TRY(d, serve_vx, 14, 1);
    TRY(d, serve_vx, 15, 0);
    /* serve_vy: 0 .. max_vy (= 8) */
    TRY(d, serve_vy, 8, 1);
    TRY(d, serve_vy, 9, 0);
    TRY(d, serve_vy, 0, 1);
    TRY(d, max_vy, 2, 0); /* serve_vy 3 > 2 */
}

/* ---- 한 번 진행 ---- */

static void test_paddle_bounds(void)
{
    g_tests++;
    pong_settings s;
    pong_settings_default(&s);
    pong_state st;
    pong_state_init(&st, &s);
    CHECK_EQ(st.paddle_y[0], 300);
    CHECK_EQ(st.paddle_y[1], 300);
    for (int i = 0; i < 100; i++)
        pong_step(&st, &s, PONG_AXIS_UP, PONG_AXIS_DOWN);
    CHECK_EQ(st.paddle_y[0], 50);  /* paddle_h/2 */
    CHECK_EQ(st.paddle_y[1], 550); /* field_h - paddle_h/2 */
    pong_step(&st, &s, PONG_AXIS_DOWN, PONG_AXIS_STOP);
    CHECK_EQ(st.paddle_y[0], 62);
    CHECK_EQ(st.paddle_y[1], 550);
}

static void test_walls(void)
{
    g_tests++;
    pong_settings s;
    pong_settings_default(&s);
    pong_state st;
    play_state(&st, &s, 400, 8, 1, -5);
    CHECK_EQ(pong_step(&st, &s, 0, 0), -1);
    CHECK_EQ(st.ball_y, 5); /* ball_size/2 */
    CHECK_EQ(st.vy, 5);
    play_state(&st, &s, 400, 592, 1, 5);
    pong_step(&st, &s, 0, 0);
    CHECK_EQ(st.ball_y, 595);
    CHECK_EQ(st.vy, -5);
    /* 경계에 딱 닿으면 그대로 */
    play_state(&st, &s, 400, 10, 1, -5);
    pong_step(&st, &s, 0, 0);
    CHECK_EQ(st.ball_y, 5);
    CHECK_EQ(st.vy, -5);
}

static void test_paddle_hits(void)
{
    g_tests++;
    pong_settings s;
    pong_settings_default(&s);
    pong_state st;

    /* 왼쪽 패들(x 20): 40 → 30, 2×10 <= 10+10 이라 맞는다. vx 크기 1 증가 */
    play_state(&st, &s, 40, 335, -10, 0);
    pong_step(&st, &s, 0, 0);
    CHECK_EQ(st.vx, 11);
    CHECK_EQ(st.vy, 3); /* (335 - 300) / 10 */
    CHECK_EQ(st.ball_x, 30); /* 맞은 자리 그대로 */
    /* 오른쪽 패들(x 780), 음수 vy는 0 쪽으로 자른다 */
    play_state(&st, &s, 760, 265, 10, 0);
    pong_step(&st, &s, 0, 0);
    CHECK_EQ(st.vx, -11);
    CHECK_EQ(st.vy, -3); /* -35 / 10 */
    /* vx 상한 */
    play_state(&st, &s, 44, 300, -14, 0);
    pong_step(&st, &s, 0, 0);
    CHECK_EQ(st.vx, 14);
    CHECK_EQ(st.vy, 0);
    /* x 겹침 경계: 2×11 > 20 은 빗나감 */
    play_state(&st, &s, 41, 300, -10, 0);
    pong_step(&st, &s, 0, 0);
    CHECK_EQ(st.vx, -10);
    /* y: 2×56 > ball_size + paddle_h 는 빗나감, 2×55 는 맞음 */
    play_state(&st, &s, 40, 356, -10, 0);
    pong_step(&st, &s, 0, 0);
    CHECK_EQ(st.vx, -10);
    play_state(&st, &s, 40, 355, -10, 0);
    pong_step(&st, &s, 0, 0);
    CHECK_EQ(st.vx, 11);
    CHECK_EQ(st.vy, 5);
    /* 패들 뒤(이동 전 x < 패들 x)로 빠진 공은 맞지 않는다 */
    play_state(&st, &s, 15, 300, -3, 0);
    pong_step(&st, &s, 0, 0);
    CHECK_EQ(st.vx, -3);
    /* 멀어지는 공은 겹쳐 있어도 맞지 않는다 */
    play_state(&st, &s, 25, 300, 3, 0);
    pong_step(&st, &s, 0, 0);
    CHECK_EQ(st.vx, 3);

    /* 여러 번 맞기 방지: 큰 공(40)으로 반사 뒤에도 겹친 채 남게 한다 */
    pong_settings b = s;
    b.ball_size = 40;
    play_state(&st, &b, 31, 300, -1, 0);
    pong_step(&st, &b, 0, 0);
    CHECK_EQ(st.vx, 2);
    pong_step(&st, &b, 0, 0); /* x 32, 아직 겹침 */
    CHECK_EQ(st.vx, 2);
    CHECK_EQ(st.ball_x, 32);

    /* vy 제한 */
    pong_settings v = s;
    v.max_vy = 2;
    play_state(&st, &v, 40, 349, -10, 0);
    pong_step(&st, &v, 0, 0);
    CHECK_EQ(st.vy, 2);
    play_state(&st, &v, 40, 251, -10, 0);
    pong_step(&st, &v, 0, 0);
    CHECK_EQ(st.vy, -2);

    /* 패들이 같은 진행에 먼저 움직인 뒤 충돌을 본다 (패들 300 → 312) */
    play_state(&st, &s, 40, 370, -10, 0);
    pong_step(&st, &s, PONG_AXIS_DOWN, 0); /* 2×58 = 116 > 110 빗나감 */
    CHECK_EQ(st.vx, -10);
    play_state(&st, &s, 40, 360, -10, 0);
    pong_step(&st, &s, PONG_AXIS_DOWN, 0); /* 2×48 <= 110 */
    CHECK_EQ(st.vx, 11);
    CHECK_EQ(st.vy, 4);
}

static void test_goal_and_serve(void)
{
    g_tests++;
    pong_settings s;
    pong_settings_default(&s);
    s.serve_wait_ticks = 0;
    pong_state st;
    pong_state_init(&st, &s);

    /* 첫 서브: slot 0 쪽, vy 양수, 같은 진행에 바로 움직인다 */
    CHECK_EQ(pong_step(&st, &s, 0, 0), -1);
    CHECK_EQ(st.vx, -6);
    CHECK_EQ(st.vy, 3);
    CHECK_EQ(st.ball_x, 394);
    CHECK_EQ(st.phase, PONG_PHASE_PLAY);

    /* x < 0 → slot 1 득점, 공은 가운데로, 서브 대기 */
    st.ball_x = 2;
    st.ball_y = 500;
    CHECK_EQ(pong_step(&st, &s, 0, 0), 1);
    CHECK_EQ(st.score[0], 0);
    CHECK_EQ(st.score[1], 1);
    CHECK_EQ(st.ball_x, 400);
    CHECK_EQ(st.ball_y, 300);
    CHECK_EQ(st.phase, PONG_PHASE_SERVE);
    /* 잃은 slot 0 쪽으로, vy는 번갈아 음수 */
    pong_step(&st, &s, 0, 0);
    CHECK_EQ(st.vx, -6);
    CHECK_EQ(st.vy, -3);

    /* x > field_w → slot 0 득점, 다음 서브는 slot 1 쪽, vy 다시 양수 */
    st.ball_x = 798;
    st.ball_y = 100;
    st.vx = 6;
    CHECK_EQ(pong_step(&st, &s, 0, 0), 0);
    CHECK_EQ(st.score[0], 1);
    pong_step(&st, &s, 0, 0);
    CHECK_EQ(st.vx, 6);
    CHECK_EQ(st.vy, 3);
    CHECK_EQ(st.ball_x, 406);

    /* x == 0 과 x == field_w 는 아직 득점이 아니다 */
    play_state(&st, &s, 4, 500, -4, 0);
    CHECK_EQ(pong_step(&st, &s, 0, 0), -1);
    play_state(&st, &s, 796, 500, 4, 0);
    CHECK_EQ(pong_step(&st, &s, 0, 0), -1);

    /* 득점이 패들 충돌보다 먼저: 패들과 겹친 채 경기장 밖이면 득점 (공 100, 패들 폭 2, inset 2) */
    pong_settings w = s;
    w.paddle_w = 2;
    w.paddle_inset = 2;
    w.ball_size = 100;
    play_state(&st, &w, 10, 300, -20, 0);
    CHECK_EQ(pong_step(&st, &w, 0, 0), 1);
}

static void test_serve_wait(void)
{
    g_tests++;
    pong_settings s;
    pong_settings_default(&s); /* serve_wait_ticks 30 */
    pong_state st;
    pong_state_init(&st, &s);
    for (int i = 0; i < 30; i++) {
        pong_step(&st, &s, PONG_AXIS_UP, 0);
        CHECK_EQ(st.phase, PONG_PHASE_SERVE);
        CHECK_EQ(st.ball_x, 400);
    }
    CHECK_EQ(st.paddle_y[0], 50); /* 대기 중에도 패들은 움직인다 */
    pong_step(&st, &s, 0, 0);
    CHECK_EQ(st.phase, PONG_PHASE_PLAY);
    CHECK_EQ(st.ball_x, 394);

    /* 득점 뒤에도 30번 멈춘다 */
    st.ball_x = 2;
    st.ball_y = 500;
    CHECK_EQ(pong_step(&st, &s, 0, 0), 1);
    for (int i = 0; i < 30; i++)
        pong_step(&st, &s, 0, 0);
    CHECK_EQ(st.ball_x, 400);
    CHECK_EQ(st.phase, PONG_PHASE_SERVE);
    pong_step(&st, &s, 0, 0);
    CHECK_EQ(st.ball_x, 394);
}

/* 코어 통합 테스트 시나리오: 경기장 200×150, paddle_h 10, 서브 4 · 3, 대기 0, win 1.
 * 패들이 가운데에 가만히 있어도 공이 빗나가 slot 1이 득점해야 한다. */
static void test_integration_scenario(void)
{
    g_tests++;
    pong_settings s;
    pong_settings_default(&s);
    s.field_w = 200;
    s.field_h = 150;
    s.paddle_h = 10;
    s.serve_vx = 4;
    s.serve_vy = 3;
    s.serve_wait_ticks = 0;
    s.win_score = 1;
    CHECK_EQ(resolve_ok(&s), 1);
    pong_state st;
    pong_state_init(&st, &s);
    int scorer = -1, ticks = 0;
    while (scorer < 0 && ticks < 1000) {
        scorer = pong_step(&st, &s, 0, 0);
        ticks++;
        CHECK(st.vx <= 0); /* 한 번도 맞지 않는다 */
    }
    CHECK_EQ(scorer, 1);
    CHECK_EQ(ticks, 26);
}

/* ---- 메시지 ---- */

static void test_message_bytes(void)
{
    g_tests++;
    pong_settings s;
    pong_settings_default(&s);
    pong_state st;
    play_state(&st, &s, 400, 300, 0, 0);
    st.paddle_y[0] = 300;
    st.paddle_y[1] = 250;
    st.score[0] = 1;
    /* protocol.md 6.1절 바이트 예시의 GAME_OUT 페이로드 */
    static const uint8_t want[PONG_STATE_LEN] = {0x01, 0x00, 0x00, 0x00, 0x78, 0x00, 0x00,
                                                 0x00, 0x29, 0x01, 0x90, 0x01, 0x2C, 0x01,
                                                 0x2C, 0x00, 0xFA, 0x01, 0x00, 0x01};
    uint8_t out[PONG_STATE_LEN];
    CHECK_EQ(pong_write_state(out, 120, 41, &st), 20);
    CHECK(memcmp(out, want, sizeof want) == 0);

    st.ball_x = -7; /* 0 아래는 0 */
    pong_write_state(out, 120, 41, &st);
    CHECK_EQ(rd16(out + 9), 0);

    uint8_t g[PONG_GOAL_LEN];
    CHECK_EQ(pong_write_goal(g, 0, &st), 4);
    CHECK(g[0] == 0x02 && g[1] == 0 && g[2] == 1 && g[3] == 0);
}

int main(void)
{
    test_def();
    test_settings_default_and_length();
    test_settings_ranges();
    test_settings_dependent();
    test_paddle_bounds();
    test_walls();
    test_paddle_hits();
    test_goal_and_serve();
    test_serve_wait();
    test_integration_scenario();
    test_message_bytes();
    printf("test_pong_rules: 테스트 %d개, 검사 %d개, 실패 %d개\n", g_tests, g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
