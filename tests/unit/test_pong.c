/* test_pong.c - Pong 입력 큐와 game_def 콜백 단위 테스트 (protocol.md 6.1절).
 *
 * 가짜 host가 send · broadcast · match_over를 부른 순서대로 기록한다. 네트워크 없이
 * 입력 큐 규칙, 진행마다 보내는 STATE, 득점 · 경기 끝 순서, 결정성을 확인한다.
 * 규칙 값 · 한 번 진행 · 바이트 배치는 test_pong_rules.c가 맡는다. 실패가 있으면 1로 끝난다.
 */
#include <stdio.h>
#include <stdlib.h>
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

/* ---- 가짜 host ---- */

enum { EV_SEND, EV_BROADCAST, EV_OVER };

typedef struct event {
    int kind, slot, mode; /* slot: send는 받는 사람, broadcast는 except_slot */
    size_t len;
    uint8_t data[32];
} event;

#define MAX_EVENTS 4096

typedef struct fake {
    event ev[MAX_EVENTS];
    size_t n;
    int overflow, overs;
} fake;

static void record(fake *f, int kind, int slot, const uint8_t *data, size_t len, int mode)
{
    if (f->n == MAX_EVENTS || len > sizeof f->ev[0].data) {
        f->overflow = 1;
        return;
    }
    event *e = &f->ev[f->n++];
    e->kind = kind;
    e->slot = slot;
    e->mode = mode;
    e->len = len;
    if (len > 0)
        memcpy(e->data, data, len);
}

static void fake_send(void *ctx, int slot, const uint8_t *d, size_t len, enum game_send_mode m)
{
    record(ctx, EV_SEND, slot, d, len, (int)m);
}

static void fake_broadcast(void *ctx, int except, const uint8_t *d, size_t len,
                           enum game_send_mode m)
{
    record(ctx, EV_BROADCAST, except, d, len, (int)m);
}

static void fake_over(void *ctx)
{
    fake *f = ctx;
    f->overs++;
    record(f, EV_OVER, -1, NULL, 0, -1);
}

/* 기록과 host를 함께 만든다. 기록 배열(4096개)이 커서 스택 대신 힙에 둔다. */
static fake *fake_new(game_host *h)
{
    fake *f = calloc(1, sizeof *f);
    if (f == NULL) {
        perror("calloc");
        exit(2);
    }
    h->ctx = f;
    h->send = fake_send;
    h->broadcast = fake_broadcast;
    h->match_over = fake_over;
    return f;
}

static const uint8_t two_slots[GAME_MAX_SLOTS] = {1, 1};

static void *match_with(const game_host *h, const pong_settings *s)
{
    uint8_t buf[PONG_SETTINGS_LEN];
    pong_settings_write(s, buf);
    void *m = pong_game.match_create(h, two_slots, buf, sizeof buf);
    if (m == NULL) {
        fprintf(stderr, "match_create 실패\n");
        exit(2);
    }
    return m;
}

static int send_input(void *m, int slot, uint32_t seq, uint8_t axis)
{
    uint8_t b[PONG_INPUT_LEN] = {(uint8_t)(seq >> 24), (uint8_t)(seq >> 16),
                                 (uint8_t)(seq >> 8), (uint8_t)seq, axis};
    return pong_game.on_input(m, slot, b, sizeof b);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static unsigned rd16(const uint8_t *p)
{
    return ((unsigned)p[0] << 8) | p[1];
}

/* slot에게 간 가장 최근 STATE */
static const event *last_state(const fake *f, int slot)
{
    for (size_t i = f->n; i-- > 0;)
        if (f->ev[i].kind == EV_SEND && f->ev[i].slot == slot)
            return &f->ev[i];
    return NULL;
}

/* ---- 입력 큐 ---- */

static void test_queue_rules(void)
{
    g_tests++;
    pong_input_queue q;
    pong_queue_init(&q);
    /* 아무 입력도 없으면 정지, 쓴 번호 0 */
    CHECK_EQ(pong_queue_take(&q), PONG_AXIS_STOP);
    CHECK_EQ(q.last_used_seq, 0);

    /* 중복: 마지막으로 받은 번호보다 크지 않으면 버린다 */
    CHECK_EQ(pong_queue_push(&q, 5, PONG_AXIS_UP), 1);
    CHECK_EQ(pong_queue_push(&q, 5, PONG_AXIS_DOWN), 0);
    CHECK_EQ(pong_queue_push(&q, 3, PONG_AXIS_DOWN), 0);
    CHECK_EQ(pong_queue_push(&q, 0, PONG_AXIS_DOWN), 0);
    CHECK_EQ(q.count, 1);

    /* 넘침: 4개를 넘으면 가장 오래된 것을 버린다 (5를 버리고 6 7 8 9) */
    for (uint32_t seq = 6; seq <= 9; seq++)
        pong_queue_push(&q, seq, seq % 2 ? PONG_AXIS_UP : PONG_AXIS_DOWN);
    CHECK_EQ(q.count, 4);

    /* 진행마다 하나씩 */
    CHECK_EQ(pong_queue_take(&q), PONG_AXIS_DOWN);
    CHECK_EQ(q.last_used_seq, 6);
    CHECK_EQ(q.count, 3);
    CHECK_EQ(pong_queue_take(&q), PONG_AXIS_UP);
    CHECK_EQ(q.last_used_seq, 7);
    pong_queue_take(&q);
    CHECK_EQ(pong_queue_take(&q), PONG_AXIS_UP);
    CHECK_EQ(q.last_used_seq, 9);

    /* 비면 직전 입력과 그 번호를 다시 쓴다 */
    CHECK_EQ(pong_queue_take(&q), PONG_AXIS_UP);
    CHECK_EQ(q.last_used_seq, 9);
    CHECK_EQ(q.count, 0);
}

static void test_input_through_match(void)
{
    g_tests++;
    game_host h;
    fake *f = fake_new(&h);
    pong_settings s;
    pong_settings_default(&s);
    void *m = match_with(&h, &s);

    /* 형식: 길이 5, axis 0 .. 2. 틀리면 음수(코어가 BAD_GAME_INPUT으로 끊는다) */
    CHECK(send_input(m, 0, 1, 3) < 0);
    CHECK(send_input(m, 0, 1, 255) < 0);
    uint8_t short_in[4] = {0, 0, 0, 1};
    CHECK(pong_game.on_input(m, 0, short_in, sizeof short_in) < 0);

    /* 거부된 입력은 번호를 남기지 않아 같은 번호 1을 다시 받을 수 있다 */
    CHECK_EQ(send_input(m, 0, 1, PONG_AXIS_UP), 0);
    CHECK_EQ(send_input(m, 0, 2, PONG_AXIS_UP), 0);
    CHECK_EQ(send_input(m, 0, 2, PONG_AXIS_DOWN), 0); /* 중복은 조용히 버린다 */

    pong_game.on_tick(m, 0);
    const event *e0 = last_state(f, 0), *e1 = last_state(f, 1);
    CHECK(e0 != NULL && e1 != NULL);
    if (e0 != NULL && e1 != NULL) {
        CHECK_EQ(rd32(e0->data + 5), 1); /* slot 0은 입력 1을 썼다 */
        CHECK_EQ(rd32(e1->data + 5), 0); /* slot 1은 입력이 없다 */
        CHECK_EQ(rd16(e0->data + 13), 300 - 12);
    }
    pong_game.on_tick(m, 1);
    e0 = last_state(f, 0);
    if (e0 != NULL) {
        CHECK_EQ(rd32(e0->data + 5), 2);
        CHECK_EQ(rd16(e0->data + 13), 300 - 24); /* 중복(아래)이 버려져 위로 한 번 더 */
    }
    pong_game.on_tick(m, 2);
    e0 = last_state(f, 0);
    if (e0 != NULL) {
        CHECK_EQ(rd32(e0->data + 5), 2); /* 큐가 비어 직전 입력(2, 위)을 다시 쓴다 */
        CHECK_EQ(rd16(e0->data + 13), 300 - 36);
    }

    /* 넘침: 진행 사이에 3 .. 8 여섯 개가 오면 가장 오래된 3 · 4가 버려져 5부터 쓴다 */
    for (uint32_t seq = 3; seq <= 8; seq++)
        send_input(m, 1, seq, PONG_AXIS_DOWN);
    pong_game.on_tick(m, 3);
    e1 = last_state(f, 1);
    if (e1 != NULL)
        CHECK_EQ(rd32(e1->data + 5), 5);
    CHECK(!f->overflow);
    pong_game.match_destroy(m);
    free(f);
}

/* ---- 진행 ---- */

static void test_state_per_tick(void)
{
    g_tests++;
    game_host h;
    fake *f = fake_new(&h);
    pong_settings s;
    pong_settings_default(&s);
    void *m = match_with(&h, &s);
    for (uint32_t t = 0; t < 3; t++)
        pong_game.on_tick(m, t);
    CHECK_EQ(f->n, 6);
    for (size_t i = 0; i < f->n && i < 6; i++) {
        const event *e = &f->ev[i];
        CHECK_EQ(e->kind, EV_SEND);
        CHECK_EQ(e->slot, (int)(i % 2));
        CHECK_EQ(e->mode, GAME_SEND_UNRELIABLE);
        CHECK_EQ(e->len, 20);
        CHECK_EQ(e->data[0], PONG_OUT_STATE);
        CHECK_EQ(rd32(e->data + 1), i / 2);
        CHECK_EQ(e->data[19], PONG_PHASE_SERVE); /* 첫 서브 전 대기 */
    }
    pong_game.match_destroy(m);
    free(f);
}

static void test_win(void)
{
    g_tests++;
    game_host h;
    fake *f = fake_new(&h);
    pong_settings s;
    pong_settings_default(&s);
    s.win_score = 2;
    s.serve_wait_ticks = 0;
    void *m = match_with(&h, &s);
    /* slot 0 패들을 맨 위에 두면 slot 0 쪽 서브 두 번을 다 놓친다 */
    CHECK_EQ(send_input(m, 0, 1, PONG_AXIS_UP), 0);
    uint32_t t = 0;
    while (f->overs == 0 && t < 2000 && !f->overflow)
        pong_game.on_tick(m, t++);
    CHECK_EQ(f->overs, 1);
    CHECK(f->n >= 4);

    /* 마지막 진행의 기록: GOAL(slot 1, 0:2) → slot 0 · 1 STATE → 경기 끝 */
    size_t goals = 0, last_goal = 0;
    for (size_t i = 0; i < f->n; i++)
        if (f->ev[i].kind == EV_BROADCAST) {
            goals++;
            last_goal = i;
            CHECK_EQ(f->ev[i].mode, GAME_SEND_RELIABLE);
            CHECK_EQ(f->ev[i].slot, GAME_SLOT_NONE);
            CHECK_EQ(f->ev[i].len, 4);
        }
    CHECK_EQ(goals, 2);
    if (f->n >= 4) {
        CHECK_EQ(last_goal, f->n - 4);
        const uint8_t *g = f->ev[last_goal].data;
        CHECK(g[0] == 0x02 && g[1] == 1 && g[2] == 0 && g[3] == 2);
        CHECK_EQ(f->ev[f->n - 3].kind, EV_SEND);
        CHECK_EQ(f->ev[f->n - 2].kind, EV_SEND);
        CHECK_EQ(f->ev[f->n - 2].data[18], 2); /* 마지막 STATE에도 새 점수 */
        CHECK_EQ(f->ev[f->n - 1].kind, EV_OVER);
    }

    /* 끝난 경기에 진행이 더 와도 아무것도 보내지 않고 다시 끝내지 않는다 */
    size_t n = f->n;
    pong_game.on_tick(m, t++);
    CHECK_EQ(f->n, n);
    CHECK_EQ(f->overs, 1);
    pong_game.match_destroy(m);
    free(f);
}

/* ---- 결정성 ---- */

/* 같은 씨앗이면 같은 입력 열(진행 사이 0 .. 2개 도착, 방향 무작위)을 만든다. */
static void run_seeded(fake *f, uint32_t seed)
{
    game_host h = {f, fake_send, fake_broadcast, fake_over};
    pong_settings s;
    pong_settings_default(&s);
    s.serve_wait_ticks = 3;
    s.win_score = 21;
    void *m = match_with(&h, &s);
    uint32_t x = seed, seq[2] = {0, 0};
    for (uint32_t t = 0; t < 1300 && f->overs == 0; t++) {
        for (int slot = 0; slot < 2; slot++) {
            x = x * 1103515245u + 12345u;
            unsigned n = (x >> 16) % 3;
            for (unsigned k = 0; k < n; k++) {
                x = x * 1103515245u + 12345u;
                send_input(m, slot, ++seq[slot], (uint8_t)((x >> 16) % 3));
            }
        }
        pong_game.on_tick(m, t);
    }
    pong_game.match_destroy(m);
}

static int same_event(const event *a, const event *b)
{
    return a->kind == b->kind && a->slot == b->slot && a->mode == b->mode && a->len == b->len
        && memcmp(a->data, b->data, a->len) == 0;
}

static void test_determinism(void)
{
    g_tests++;
    fake *a = calloc(1, sizeof *a), *b = calloc(1, sizeof *b);
    if (a == NULL || b == NULL) {
        perror("calloc");
        exit(2);
    }
    run_seeded(a, 7);
    run_seeded(b, 7);
    CHECK(!a->overflow && !b->overflow);
    CHECK(a->n > 100);
    CHECK_EQ(a->n, b->n);
    size_t diff = 0, goals = 0;
    for (size_t i = 0; i < a->n && i < b->n; i++) {
        diff += !same_event(&a->ev[i], &b->ev[i]);
        goals += a->ev[i].kind == EV_BROADCAST;
    }
    CHECK_EQ(diff, 0);
    CHECK(goals > 0); /* 득점 경로까지 지나갔는지 */
    free(a);
    free(b);
}

int main(void)
{
    test_queue_rules();
    test_input_through_match();
    test_state_per_tick();
    test_win();
    test_determinism();
    printf("test_pong: 테스트 %d개, 검사 %d개, 실패 %d개\n", g_tests, g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
