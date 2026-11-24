/* pong.c - Pong 게임 모듈 (protocol.md 6.1절).
 *
 * 서버가 30Hz로 경기를 계산하는 권위적 방식이다. 클라이언트(C#)가 같은 규칙 값으로 같은 계산을
 * 재현할 수 있도록 모든 계산을 정수로 하고, 진행 순서는 6.1절 규칙 표의 순서를 그대로 따른다.
 */
#include "pong.h"

#include <stdlib.h>

/* 바이트 순서 헬퍼. 코어의 wire 모듈에 기대지 않아야 이 파일만으로 테스트를 빌드할 수 있다. */
static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(((unsigned)p[0] << 8) | p[1]);
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int in_range(unsigned v, unsigned lo, unsigned hi)
{
    return v >= lo && v <= hi;
}

static int32_t iabs32(int32_t v)
{
    return v < 0 ? -v : v;
}

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ---- 규칙 값 ---- */

void pong_settings_default(pong_settings *s)
{
    s->field_w = 800;
    s->field_h = 600;
    s->paddle_w = 10;
    s->paddle_h = 100;
    s->paddle_inset = 20;
    s->paddle_speed = 12;
    s->ball_size = 10;
    s->serve_vx = 6;
    s->serve_vy = 3;
    s->max_vx = 14;
    s->max_vy = 8;
    s->serve_wait_ticks = 30;
    s->win_score = 5;
}

int pong_settings_parse(const uint8_t *in, size_t len, pong_settings *out)
{
    if (in == NULL || len != PONG_SETTINGS_LEN)
        return -1;

    pong_settings s;
    s.field_w = get_u16(in + 0);
    s.field_h = get_u16(in + 2);
    s.paddle_w = get_u16(in + 4);
    s.paddle_h = get_u16(in + 6);
    s.paddle_inset = get_u16(in + 8);
    s.paddle_speed = get_u16(in + 10);
    s.ball_size = get_u16(in + 12);
    s.serve_vx = get_u16(in + 14);
    s.serve_vy = get_u16(in + 16);
    s.max_vx = get_u16(in + 18);
    s.max_vy = get_u16(in + 20);
    s.serve_wait_ticks = get_u16(in + 22);
    s.win_score = in[24];

    /* 의존 범위는 앞에서 이미 범위를 통과한 필드로만 계산한다(6.1절 표의 "허용 범위"). */
    int ok = in_range(s.field_w, 200, 4000)
          && in_range(s.field_h, 150, 4000)
          && in_range(s.paddle_w, 2, 100)
          && in_range(s.paddle_h, 10, s.field_h / 2u)
          && in_range(s.paddle_inset, s.paddle_w, s.field_w / 4u)
          && in_range(s.paddle_speed, 1, 100)
          && in_range(s.ball_size, 2, 100)
          && in_range(s.max_vx, 1, (unsigned)s.ball_size + s.paddle_w)
          && in_range(s.max_vy, 0, 100)
          && in_range(s.serve_vx, 1, s.max_vx)
          && in_range(s.serve_vy, 0, s.max_vy)
          && in_range(s.serve_wait_ticks, 0, 300)
          && in_range(s.win_score, 1, 21);
    if (!ok)
        return -1;
    *out = s;
    return 0;
}

void pong_settings_write(const pong_settings *s, uint8_t out[PONG_SETTINGS_LEN])
{
    put_u16(out + 0, s->field_w);
    put_u16(out + 2, s->field_h);
    put_u16(out + 4, s->paddle_w);
    put_u16(out + 6, s->paddle_h);
    put_u16(out + 8, s->paddle_inset);
    put_u16(out + 10, s->paddle_speed);
    put_u16(out + 12, s->ball_size);
    put_u16(out + 14, s->serve_vx);
    put_u16(out + 16, s->serve_vy);
    put_u16(out + 18, s->max_vx);
    put_u16(out + 20, s->max_vy);
    put_u16(out + 22, s->serve_wait_ticks);
    out[24] = s->win_score;
}

int pong_settings_resolve(const uint8_t *in, size_t in_len, uint8_t *out, size_t *out_len)
{
    pong_settings s;
    if (in_len == 0)
        pong_settings_default(&s);
    else if (pong_settings_parse(in, in_len, &s) != 0)
        return -1;
    /* 방에는 항상 25바이트 전체를 고정한다: ROOM_JOINED로 받은 사람이 기본값을 따로 몰라도 된다. */
    pong_settings_write(&s, out);
    *out_len = PONG_SETTINGS_LEN;
    return 0;
}

/* ---- 한 번 진행 ---- */

void pong_state_init(pong_state *st, const pong_settings *s)
{
    st->ball_x = s->field_w / 2;
    st->ball_y = s->field_h / 2;
    st->vx = 0;
    st->vy = 0;
    st->paddle_y[0] = s->field_h / 2;
    st->paddle_y[1] = s->field_h / 2;
    st->score[0] = 0;
    st->score[1] = 0;
    /* 첫 서브도 득점 뒤와 같이 멈췄다가 나간다: 클라이언트가 MATCH_START 뒤 준비할 시간을 준다. */
    st->phase = PONG_PHASE_SERVE;
    st->wait_left = s->serve_wait_ticks;
    st->serve_dir = -1;
    st->serve_vy_sign = 1;
}

static void move_paddle(int32_t *y, uint8_t axis, const pong_settings *s)
{
    if (axis == PONG_AXIS_UP)
        *y -= s->paddle_speed;
    else if (axis == PONG_AXIS_DOWN)
        *y += s->paddle_speed;
    int32_t half = s->paddle_h / 2;
    *y = clamp32(*y, half, (int32_t)s->field_h - half);
}

/* side 0 = 왼쪽, 1 = 오른쪽 패들. 맞으면 1.
 * 패들 쪽으로 움직이는 공만 맞는다: 반사된 공은 vx 부호가 바뀌어 같은 패들에 다시 맞지 않는다.
 * 이동 전 공 중심이 패들 중심보다 경기장 안쪽이어야 한다: 패들 뒤로 빠진 공을 패들이 따라와
 * 뒤에서 쳐 내는 일을 막는다. 겹침은 "2 × 중심 거리 <= 두 크기의 합"으로 판정해 홀수 크기에서도
 * 반 픽셀이 없다. max_vx <= ball_size + paddle_w라서 이 겹침 구간을 한 번에 건너뛰지 못한다. */
static int paddle_hit(pong_state *st, const pong_settings *s, int side, int32_t prev_x)
{
    int32_t px = side == 0 ? (int32_t)s->paddle_inset
                           : (int32_t)s->field_w - (int32_t)s->paddle_inset;
    int32_t py = st->paddle_y[side];

    if (side == 0 ? (st->vx >= 0 || prev_x < px) : (st->vx <= 0 || prev_x > px))
        return 0;
    if (2 * iabs32(st->ball_x - px) > (int32_t)s->ball_size + s->paddle_w)
        return 0;
    if (2 * iabs32(st->ball_y - py) > (int32_t)s->ball_size + s->paddle_h)
        return 0;

    int32_t speed = iabs32(st->vx) + 1;
    if (speed > s->max_vx)
        speed = s->max_vx;
    st->vx = side == 0 ? speed : -speed;
    /* C와 C#의 '/'는 모두 0 쪽으로 자른다. */
    st->vy = clamp32((st->ball_y - py) / 10, -(int32_t)s->max_vy, s->max_vy);
    return 1;
}

static void reset_for_serve(pong_state *st, const pong_settings *s, int scorer)
{
    st->ball_x = s->field_w / 2;
    st->ball_y = s->field_h / 2;
    st->vx = 0;
    st->vy = 0;
    st->phase = PONG_PHASE_SERVE;
    st->wait_left = s->serve_wait_ticks;
    /* 점수를 잃은 쪽으로 서브한다: slot 1이 득점했으면 slot 0(왼쪽)으로. */
    st->serve_dir = scorer == 1 ? -1 : 1;
}

int pong_step(pong_state *st, const pong_settings *s, uint8_t axis0, uint8_t axis1)
{
    /* 1. 패들 먼저: 이번 진행의 입력이 이번 진행의 충돌에 반영된다. 서브 대기 중에도 움직인다. */
    move_paddle(&st->paddle_y[0], axis0, s);
    move_paddle(&st->paddle_y[1], axis1, s);

    /* 2. 서브 대기: serve_wait_ticks번 멈춘 뒤의 진행에서 속도를 주고 그 진행에 바로 움직인다. */
    if (st->phase == PONG_PHASE_SERVE) {
        if (st->wait_left > 0) {
            st->wait_left--;
            return -1;
        }
        st->vx = st->serve_dir * (int32_t)s->serve_vx;
        st->vy = st->serve_vy_sign * (int32_t)s->serve_vy;
        st->serve_vy_sign = (int8_t)-st->serve_vy_sign;
        st->phase = PONG_PHASE_PLAY;
    }

    /* 3. 공 이동 */
    int32_t prev_x = st->ball_x;
    st->ball_x += st->vx;
    st->ball_y += st->vy;

    /* 4. 위아래 벽: 공이 경기장 위아래로 걸치면 경계로 되돌리고 vy가 안쪽을 향하게 한다.
     * 위치를 거울처럼 접지 않고 경계에 붙이는 이유: 큰 vy에서 접은 위치가 반대 벽을 넘는 일이 없다. */
    int32_t lo = s->ball_size / 2;
    int32_t hi = (int32_t)s->field_h - lo;
    if (st->ball_y < lo) {
        st->ball_y = lo;
        if (st->vy < 0)
            st->vy = -st->vy;
    } else if (st->ball_y > hi) {
        st->ball_y = hi;
        if (st->vy > 0)
            st->vy = -st->vy;
    }

    /* 5. 득점을 패들 충돌보다 먼저 본다: 공 중심이 경기장 밖이면 패들과 겹쳐 있어도 득점이다.
     * 반대 순서면 경기장 밖에서 반사된 공이 다음 진행에 또 밖에 있어 판정이 꼬인다. */
    int scorer = -1;
    if (st->ball_x < 0)
        scorer = 1;
    else if (st->ball_x > (int32_t)s->field_w)
        scorer = 0;
    if (scorer >= 0) {
        st->score[scorer]++;
        reset_for_serve(st, s, scorer);
        return scorer;
    }

    /* 6. 패들 충돌. 공은 한 방향으로만 움직이므로 많아야 한쪽만 맞는다. */
    if (!paddle_hit(st, s, 0, prev_x))
        paddle_hit(st, s, 1, prev_x);
    return -1;
}

/* ---- 입력 큐 ---- */

void pong_queue_init(pong_input_queue *q)
{
    q->head = 0;
    q->count = 0;
    q->last_received_seq = 0;
    q->last_used_seq = 0;
    q->current_axis = PONG_AXIS_STOP;
}

int pong_queue_push(pong_input_queue *q, uint32_t seq, uint8_t axis)
{
    if (seq <= q->last_received_seq)
        return 0;
    q->last_received_seq = seq;
    if (q->count == PONG_QUEUE_MAX) {
        /* 밀린 입력은 오래된 것부터 버린다: 지연을 4진행 안으로 묶는다. */
        q->head = (uint8_t)((q->head + 1) % PONG_QUEUE_MAX);
        q->count--;
    }
    unsigned idx = (q->head + q->count) % PONG_QUEUE_MAX;
    q->seq[idx] = seq;
    q->axis[idx] = axis;
    q->count++;
    return 1;
}

uint8_t pong_queue_take(pong_input_queue *q)
{
    if (q->count > 0) {
        q->current_axis = q->axis[q->head];
        q->last_used_seq = q->seq[q->head];
        q->head = (uint8_t)((q->head + 1) % PONG_QUEUE_MAX);
        q->count--;
    }
    return q->current_axis;
}

/* ---- 메시지 ---- */

static uint16_t to_u16(int32_t v)
{
    return (uint16_t)clamp32(v, 0, UINT16_MAX);
}

size_t pong_write_state(uint8_t out[PONG_STATE_LEN], uint32_t tick, uint32_t last_input_seq,
                        const pong_state *st)
{
    out[0] = PONG_OUT_STATE;
    put_u32(out + 1, tick);
    put_u32(out + 5, last_input_seq);
    put_u16(out + 9, to_u16(st->ball_x)); /* 0 아래는 0으로 (6.1절) */
    put_u16(out + 11, to_u16(st->ball_y));
    put_u16(out + 13, to_u16(st->paddle_y[0]));
    put_u16(out + 15, to_u16(st->paddle_y[1]));
    out[17] = st->score[0];
    out[18] = st->score[1];
    out[19] = st->phase;
    return PONG_STATE_LEN;
}

size_t pong_write_goal(uint8_t out[PONG_GOAL_LEN], uint8_t scorer_slot, const pong_state *st)
{
    out[0] = PONG_OUT_GOAL;
    out[1] = scorer_slot;
    out[2] = st->score[0];
    out[3] = st->score[1];
    return PONG_GOAL_LEN;
}

/* ---- game_def ---- */

typedef struct pong_match {
    const game_host *host;
    pong_settings settings;
    pong_state state;
    pong_input_queue queue[2];
    int over; /* match_over를 부른 뒤에는 코어가 지울 때까지 아무것도 하지 않는다 */
} pong_match;

static void *pong_match_create(const game_host *host, const uint8_t slots[GAME_MAX_SLOTS],
                               const uint8_t *settings, size_t settings_len)
{
    (void)slots; /* 2명 고정이라 slot 0 · 1이 늘 차 있다 */
    pong_settings s;
    if (pong_settings_parse(settings, settings_len, &s) != 0)
        return NULL;
    pong_match *m = calloc(1, sizeof *m);
    if (m == NULL)
        return NULL;
    m->host = host;
    m->settings = s;
    pong_state_init(&m->state, &s);
    pong_queue_init(&m->queue[0]);
    pong_queue_init(&m->queue[1]);
    return m;
}

static void pong_match_destroy(void *match)
{
    free(match);
}

/* GAME_IN 두 형식(protocol.md 6.1절): 단일 5바이트, 또는 u8 n + 5바이트 n개(n = 1–4, 오래된 것부터).
 * 1 + 5n은 5가 될 수 없어 길이만으로 형식이 갈린다. 묶음은 모두 검사한 뒤에만 큐에 넣는다:
 * 중간에 틀린 항목이 있으면 앞 항목만 들어가 번호가 소비되는 일이 없게 한다. 같은 입력이 여러
 * 묶음에 겹쳐 와도(UDP 손실 메우기) 큐의 중복 규칙이 한 번만 쓰이게 한다. */
static int pong_on_input(void *match, int slot, const uint8_t *data, size_t len)
{
    pong_match *m = match;
    if (slot < 0 || slot > 1)
        return -1;
    const uint8_t *items = data;
    size_t n = 1;
    if (len != PONG_INPUT_LEN) {
        if (len < 1)
            return -1;
        n = data[0];
        if (n < 1 || n > PONG_BATCH_MAX || len != 1 + PONG_INPUT_LEN * n)
            return -1;
        items = data + 1;
    }
    for (size_t i = 0; i < n; i++) {
        if (items[i * PONG_INPUT_LEN + 4] > PONG_AXIS_DOWN)
            return -1;
    }
    for (size_t i = 0; i < n; i++) {
        const uint8_t *it = items + i * PONG_INPUT_LEN;
        pong_queue_push(&m->queue[slot], get_u32(it), it[4]);
    }
    return 0;
}

static void pong_on_tick(void *match, uint32_t tick)
{
    pong_match *m = match;
    if (m->over)
        return;
    const game_host *h = m->host;

    uint8_t a0 = pong_queue_take(&m->queue[0]);
    uint8_t a1 = pong_queue_take(&m->queue[1]);
    int scorer = pong_step(&m->state, &m->settings, a0, a1);

    if (scorer >= 0) {
        uint8_t goal[PONG_GOAL_LEN];
        size_t n = pong_write_goal(goal, (uint8_t)scorer, &m->state);
        h->broadcast(h->ctx, GAME_SLOT_NONE, goal, n, GAME_SEND_RELIABLE);
    }

    /* 플레이어마다 자기 입력 번호가 달라서 broadcast 대신 한 명씩 보낸다. */
    for (int slot = 0; slot < 2; slot++) {
        uint8_t buf[PONG_STATE_LEN];
        size_t n = pong_write_state(buf, tick, m->queue[slot].last_used_seq, &m->state);
        h->send(h->ctx, slot, buf, n, GAME_SEND_UNRELIABLE);
    }

    if (scorer >= 0 && m->state.score[scorer] >= m->settings.win_score) {
        m->over = 1;
        h->match_over(h->ctx);
    }
}

const game_def pong_game = {
    .id = 1,
    .name = "Pong",
    .min_players = 2,
    .max_players = 2,
    .tick_hz = 30,
    .max_input_len = PONG_INPUT_MAX_LEN,
    .settings_resolve = pong_settings_resolve,
    .match_create = pong_match_create,
    .match_destroy = pong_match_destroy,
    .on_join = NULL, /* 2명 고정: 인원이 바뀌면 코어가 경기를 지운다 */
    .on_leave = NULL,
    .on_input = pong_on_input,
    .on_tick = pong_on_tick,
};
