/* pong.h - Pong 게임 모듈 (protocol.md 6.1절, game_id 1).
 *
 * 코어는 pong_game만 쓴다. 나머지(규칙 값 해석, 입력 큐, 한 번 진행, 메시지 만들기)는
 * 네트워크 · host 없이 단위 테스트하려고 내놓는 순수 함수다.
 * 모든 계산은 정수이고 나눗셈은 0 쪽으로 자른다: C#에서 같은 식으로 같은 결과를 낸다.
 */
#ifndef PONG_H
#define PONG_H

#include <stddef.h>
#include <stdint.h>

#include "../game_api.h"

extern const game_def pong_game;

enum {
    PONG_SETTINGS_LEN = 25, /* u16 12개 + u8 1개 */
    PONG_INPUT_LEN = 5,     /* u32 input_seq + u8 axis */
    PONG_STATE_LEN = 20,    /* 종류 1 + 19 */
    PONG_GOAL_LEN = 4,      /* 종류 1 + 3 */
    PONG_QUEUE_MAX = 4,     /* 플레이어별 입력 큐 상한 */
};

enum { PONG_OUT_STATE = 0x01, PONG_OUT_GOAL = 0x02 };
enum { PONG_AXIS_STOP = 0, PONG_AXIS_UP = 1, PONG_AXIS_DOWN = 2 };
enum { PONG_PHASE_SERVE = 0, PONG_PHASE_PLAY = 1 };

/* 방 규칙 값. 필드 순서 · 이름은 protocol.md 6.1절 표와 같다. */
typedef struct pong_settings {
    uint16_t field_w, field_h;
    uint16_t paddle_w, paddle_h, paddle_inset, paddle_speed;
    uint16_t ball_size;
    uint16_t serve_vx, serve_vy, max_vx, max_vy;
    uint16_t serve_wait_ticks;
    uint8_t win_score;
} pong_settings;

/* 경기 상태. 좌표는 경기장 단위, 공 x는 잠깐 경기장 밖(음수 포함)일 수 있어 부호 있는 형이다. */
typedef struct pong_state {
    int32_t ball_x, ball_y;
    int32_t vx, vy;
    int32_t paddle_y[2];
    uint8_t score[2];
    uint8_t phase;         /* PONG_PHASE_* */
    uint16_t wait_left;    /* 서브 대기에서 남은 멈춤 진행 수 */
    int8_t serve_dir;      /* 다음 서브의 vx 부호: -1 왼쪽(slot 0 쪽), +1 오른쪽 */
    int8_t serve_vy_sign;  /* 다음 서브의 vy 부호. 서브마다 뒤집는다 */
} pong_state;

/* 플레이어 한 명의 입력 큐 (protocol.md 6.1절 GAME_IN 규칙). */
typedef struct pong_input_queue {
    uint32_t seq[PONG_QUEUE_MAX];
    uint8_t axis[PONG_QUEUE_MAX];
    uint8_t head, count;
    uint32_t last_received_seq; /* 받아 둔 입력 중 가장 큰 번호. 이하이면 중복으로 버린다 */
    uint32_t last_used_seq;     /* 지금까지 진행에 쓴 마지막 입력 번호. 없으면 0 */
    uint8_t current_axis;       /* 큐가 비었을 때 다시 쓰는 직전 입력 */
} pong_input_queue;

void pong_settings_default(pong_settings *s);

/* 25바이트를 읽어 범위 · 필드 사이 의존 범위를 검사한다. 맞으면 0, 아니면 -1. */
int pong_settings_parse(const uint8_t *in, size_t len, pong_settings *out);

/* 규칙 값을 25바이트 big-endian으로 쓴다. */
void pong_settings_write(const pong_settings *s, uint8_t out[PONG_SETTINGS_LEN]);

/* game_def.settings_resolve와 같은 약속. */
int pong_settings_resolve(const uint8_t *in, size_t in_len, uint8_t *out, size_t *out_len);

/* 경기 시작 상태: 공 가운데, 패들 가운데, 서브 대기, 첫 서브는 slot 0 쪽 · vy 양수. */
void pong_state_init(pong_state *st, const pong_settings *s);

/* 진행 한 번. axis0 · axis1은 이번 진행에 쓰는 두 플레이어의 입력(0 · 1 · 2).
 * 득점이 나면 득점한 slot(0 · 1), 아니면 -1. */
int pong_step(pong_state *st, const pong_settings *s, uint8_t axis0, uint8_t axis1);

void pong_queue_init(pong_input_queue *q);

/* 입력 하나를 넣는다. 중복 번호는 버리고 0, 넣었으면 1. 큐가 차 있으면 가장 오래된 것을 버린다. */
int pong_queue_push(pong_input_queue *q, uint32_t seq, uint8_t axis);

/* 이번 진행에 쓸 입력을 꺼낸다. 비어 있으면 직전 입력을 다시 돌려준다. */
uint8_t pong_queue_take(pong_input_queue *q);

/* GAME_OUT 페이로드 만들기. 쓴 바이트 수를 돌려준다. */
size_t pong_write_state(uint8_t out[PONG_STATE_LEN], uint32_t tick, uint32_t last_input_seq,
                        const pong_state *st);
size_t pong_write_goal(uint8_t out[PONG_GOAL_LEN], uint8_t scorer_slot, const pong_state *st);

#endif /* PONG_H */
