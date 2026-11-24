/* bot.c - 봇 하나의 프로토콜 동작 (소켓 입출력은 botio.c).
 *
 * 흐름: 접속 → WELCOME → (udp 모드면 UDP 묶기) → 짝수 봇은 비공개 방 만들기, 홀수 봇은
 * 짝의 코드로 입장 → MATCH_START부터 1/30초마다 입력 → MATCH_END면 REMATCH.
 * QUICK_JOIN을 쓰지 않는 까닭: 봇 수백 개가 동시에 빠른 입장을 하면 누가 누구와 짝이
 * 될지 정해지지 않아, 홀로 남는 방이 생기고 봇 수와 경기 수의 관계가 회차마다 달라진다. */
#include <string.h>

#include "lg.h"
#include "proto.h"

/* n번째 입력 시각 = 시작 + n × 1000/30ms. 반올림 간격을 더해 가면 오차가 쌓인다
 * (protocol.md 6.1절). */
#define INPUT_OFFSET_NS(n) ((n) * 1000000000ull / TICK_HZ)

/* 6.1절 기본값에 win_score만 바꾼 규칙: 순서대로 u16 12개 + u8 1개 */
static const uint16_t SETTINGS_U16[12] = {800, 600, 10, 100, 20, 12, 10, 6, 3, 14, 8, 30};

static uint32_t rng_next(bot *b)
{
    uint32_t x = b->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    b->rng = x;
    return x;
}

static int16_t roll_aim(bot *b)
{
    return (int16_t)((int)(rng_next(b) % (2 * AIM_SPREAD + 1)) - AIM_SPREAD);
}

static void send_join(bot *guest, const bot *host)
{
    bot_send_frame(guest, P_JOIN_BY_CODE, (const uint8_t *)host->code, P_CODE_LEN);
    g.st.joins_sent++;
}

/* 로비에 섰다: 방장은 방을 만들고, 짝은 방장의 코드가 있으면 바로 들어간다. */
static void become_ready(bot *b)
{
    b->state = B_READY;
    if (b->idx % 2 == 0) {
        uint8_t p[4 + PONG_SETTINGS_LEN];
        put_u16(p, P_GAME_PONG);
        p[2] = 1; /* 비공개 */
        p[3] = PONG_SETTINGS_LEN;
        for (int i = 0; i < 12; i++)
            put_u16(p + 4 + 2 * i, SETTINGS_U16[i]);
        p[4 + 24] = g.cfg.win_score;
        bot_send_frame(b, P_CREATE_ROOM, p, sizeof p);
    } else if (g.bots[b->idx - 1].has_code) {
        send_join(b, &g.bots[b->idx - 1]);
    }
}

static void start_bind(bot *b, uint64_t now)
{
    if (bot_open_udp(b) != 0) {
        g.st.udp_fallback++;
        become_ready(b);
        return;
    }
    b->state = B_BINDING;
    b->bind_start = now;
    b->bind_at = now + BIND_RETRY_NS;
    bot_send_udp(b, P_UDP_BIND, NULL, 0);
    timer_push(b->bind_at, b->idx, T_BIND);
}

static uint8_t choose_axis(const bot *b)
{
    int target = (int)b->ball_y + b->aim;
    if (target + 6 < (int)b->my_y)
        return 1; /* 위(y가 작아지는 쪽) */
    if (target > (int)b->my_y + 6)
        return 2;
    return 0;
}

/* 예정 시각이 지난 입력을 모두 보낸다. 도구가 밀려 여러 개가 밀렸으면 몰아 보내 개수를
 * 맞추고, 밀린 정도는 send_lag로 남겨 도구가 병목인지 보인다. */
static void send_inputs(bot *b, uint64_t now)
{
    while (b->state == B_PLAYING && b->input_at != 0 && b->input_at <= now) {
        uint64_t sched = b->input_at;
        uint8_t axis = choose_axis(b);
        uint32_t seq = ++b->seq;
        b->sent_ns[seq % SEQ_RING] = now;
        b->axis[seq % SEQ_RING] = axis;
        if (b->udp_on) {
            /* 묶음: 방금 입력과 그 앞 최대 3개, 오래된 것부터 (6.1절) */
            uint32_t first = seq > PONG_BUNDLE_MAX - 1 ? seq - (PONG_BUNDLE_MAX - 1) : 1;
            uint8_t p[1 + 5 * PONG_BUNDLE_MAX];
            uint8_t n = (uint8_t)(seq - first + 1);
            p[0] = n;
            for (uint8_t i = 0; i < n; i++) {
                put_u32(p + 1 + 5 * i, first + i);
                p[5 + 5 * i] = b->axis[(first + i) % SEQ_RING];
            }
            bot_send_udp(b, P_GAME_IN, p, (uint16_t)(1 + 5 * n));
        } else {
            uint8_t p[5];
            put_u32(p, seq);
            p[4] = axis;
            bot_send_frame(b, P_GAME_IN, p, sizeof p);
        }
        hist_add(&g.st.send_lag_all, now - sched);
        g.st.inputs_late_tick_all += now - sched >= INPUT_OFFSET_NS(1);
        if (g.measuring) {
            g.st.inputs_sent++;
            g.st.inputs_sent_udp += b->udp_on;
            hist_add(&g.st.send_lag, now - sched);
        }
        b->input_n++;
        b->input_at = b->input_base + INPUT_OFFSET_NS(b->input_n);
    }
    if (b->state == B_PLAYING)
        timer_push(b->input_at, b->idx, T_INPUT);
}

static void on_state(bot *b, const uint8_t *p, int via_udp, uint64_t now)
{
    if (b->state != B_PLAYING)
        return; /* 경기가 끝난 뒤 늦게 온 UDP 상태 */
    int m = g.measuring;
    g.st.states_rx += (uint64_t)m;
    g.st.states_rx_udp += (uint64_t)(m && via_udp);
    int64_t tick = get_u32(p + 1);
    uint32_t reflected = get_u32(p + 5);
    /* 7장: 이미 받은 것보다 tick이 작거나 같은 STATE는 버린다. 이번 경기에 아직 보내지 않은
     * 번호를 담은 STATE는 앞 경기에서 늦게 온 UDP 데이터그램이라 같이 버린다(seq는 경기마다 1부터). */
    if (tick <= b->last_tick || reflected > b->seq) {
        g.st.states_reordered += (uint64_t)m;
        return;
    }
    if (m) {
        g.st.states_ok++;
        g.st.ticks_skipped += (uint64_t)(tick - b->last_tick - 1);
        if (b->last_state != 0)
            hist_add(&g.st.interval, now - b->last_state);
    }
    b->last_tick = tick;
    b->last_state = now;
    /* 입력 반영 지연: 이 번호가 last_input_seq로 처음 나타난 STATE를 받은 시각 − 보낸 시각.
     * 건너뛴 번호(큐 넘침으로 버려짐, UDP에서 묶음으로도 못 메움)는 표본이 없고 따로 센다. */
    if (reflected > b->last_reflected) {
        if (m) {
            g.st.inputs_unreflected += reflected - b->last_reflected - 1;
            if (b->seq - reflected < SEQ_RING)
                hist_add(&g.st.lat, now - b->sent_ns[reflected % SEQ_RING]);
            else
                g.st.latency_untracked++;
        }
        b->last_reflected = reflected;
    }
    /* 공이 내 쪽으로 오기 시작하면 노림 오차를 새로 뽑는다. 한 번 뽑은 오차를 경기 내내 쓰면
     * 오차가 패들 범위 안인 봇은 영영 놓치지 않아 득점 · 경기 끝 · 다시 하기가 일어나지 않는다. */
    uint16_t ball_x = get_u16(p + 9);
    uint8_t incoming = b->slot ? ball_x > b->ball_x : ball_x < b->ball_x;
    if (incoming && !b->incoming)
        b->aim = roll_aim(b);
    b->incoming = incoming;
    b->ball_x = ball_x;
    b->ball_y = get_u16(p + 11);
    b->my_y = get_u16(p + (b->slot ? 15 : 13));
}

static void begin_match(bot *b, uint32_t start_tick, uint64_t now)
{
    if (b->idx % 2 == 0)
        g.st.matches_started++;
    b->state = B_PLAYING;
    b->last_tick = (int64_t)start_tick - 1;
    b->last_state = 0;
    /* input_seq는 경기마다 1부터(6.1절, Unity 클라이언트와 같음). 이어서 늘리면 UDP 묶음의
     * "직전 3개"에 앞 경기 입력이 실려 새 경기 큐에 한꺼번에 4개가 들어가고, 입력이 3진행 늦게
     * 반영되는 상태로 굳는다. */
    b->seq = 0;
    b->last_reflected = 0;
    b->aim = roll_aim(b);
    b->ball_x = SETTINGS_U16[0] / 2; /* 시작 상태: 공은 가운데(6.1절) */
    b->incoming = 0;
    b->input_base = now;
    b->input_n = 0;
    b->input_at = now;
    send_inputs(b, now);
}

void bot_on_msg(bot *b, uint8_t type, const uint8_t *p, uint16_t len, int via_udp)
{
    uint64_t now = now_ns();
    switch (type) {
    case P_WELCOME:
        if (len < 9 || b->welcomed)
            break;
        b->welcomed = 1;
        g.st.conn_ok++;
        b->udp_token = get_u32(p + 4);
        b->ping_at = now;
        timer_push(now, b->idx, T_PING);
        if (g.cfg.mode == MODE_UDP)
            start_bind(b, now);
        else
            become_ready(b);
        return;
    case P_ROOM_JOINED:
        if (len < 17)
            break;
        b->slot = p[6];
        b->state = B_IN_ROOM;
        if (b->idx % 2 == 0) {
            memcpy(b->code, p + 9, P_CODE_LEN);
            b->has_code = 1;
            g.st.rooms_created++;
            if (b->idx + 1 < g.cfg.bots && g.bots[b->idx + 1].state == B_READY)
                send_join(&g.bots[b->idx + 1], b);
        }
        return;
    case P_MATCH_START:
        if (len != 4)
            break;
        begin_match(b, get_u32(p), now);
        return;
    case P_MATCH_END:
        if (b->idx % 2 == 0)
            g.st.matches_ended++;
        b->state = B_IN_ROOM;
        b->input_at = 0;
        bot_send_frame(b, P_REMATCH, NULL, 0);
        g.st.rematch_sent++;
        return;
    case P_PONG:
        if (len == 8 && b->ping_sent != 0 && get_u32(p) == b->ping_val) {
            if (g.measuring) {
                g.st.pongs_rx++;
                hist_add(&g.st.rtt, now - b->ping_sent);
            }
            b->ping_sent = 0;
        }
        return;
    case P_ERROR: {
        uint16_t code = len >= 2 ? get_u16(p) : 0;
        g.st.errors_rx[code < 15 ? code : 15]++;
        if (code == P_ERR_SERVER_FULL)
            bot_fail(b, F_SERVER_FULL);
        return;
    }
    case P_UDP_BOUND:
        if (via_udp && b->state == B_BINDING) {
            b->udp_on = 1;
            b->bind_at = 0;
            g.st.udp_bound++;
            become_ready(b);
        }
        return;
    case P_GAME_OUT:
        if (len == PONG_STATE_LEN && p[0] == PONG_STATE) {
            on_state(b, p, via_udp, now);
        } else if (len == 4 && p[0] == PONG_GOAL) {
            g.st.goals += (b->idx % 2 == 0);
        }
        return;
    default:
        return; /* PLAYER_JOINED · PLAYER_LEFT · REMATCH_VOTED 등은 동작에 쓰지 않는다 */
    }
    bot_fail(b, F_PROTOCOL);
}

static void on_ping_timer(bot *b, uint64_t now)
{
    b->ping_val = (uint32_t)(now / 1000000u); /* 클라이언트 시계 ms 하위 32비트 */
    uint8_t p[4];
    put_u32(p, b->ping_val);
    b->ping_sent = now;
    g.st.pings_sent += (uint64_t)g.measuring;
    bot_send_frame(b, P_PING, p, sizeof p);
    if (b->state != B_DEAD) {
        b->ping_at += PING_INTERVAL_NS;
        timer_push(b->ping_at, b->idx, T_PING);
    }
}

static void on_bind_timer(bot *b, uint64_t now)
{
    if (now - b->bind_start >= BIND_GIVEUP_NS) {
        /* 7장 1번: 2초 안에 묶이지 않으면 UDP를 포기하고 TCP만 쓴다 */
        g.st.udp_fallback++;
        bot_close_udp(b);
        b->bind_at = 0;
        become_ready(b);
        return;
    }
    bot_send_udp(b, P_UDP_BIND, NULL, 0);
    b->bind_at += BIND_RETRY_NS;
    timer_push(b->bind_at, b->idx, T_BIND);
}

void bot_on_timer(bot *b, uint8_t kind, uint64_t when)
{
    uint64_t now = now_ns();
    if (kind == T_CONNECT && b->connect_at == when && b->state == B_IDLE)
        bot_connect(b);
    else if (kind == T_INPUT && b->input_at == when)
        send_inputs(b, now);
    else if (kind == T_PING && b->ping_at == when && b->state != B_DEAD)
        on_ping_timer(b, now);
    else if (kind == T_BIND && b->bind_at == when && b->state == B_BINDING)
        on_bind_timer(b, now);
}
