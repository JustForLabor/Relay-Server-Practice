/* scenarios.h - 통합 시나리오 목록. 각 함수는 실패하면 itest_fail을 남기고 돌아온다.
 * 시나리오는 끝날 때 자기 연결을 모두 정리해, 다음 시나리오가 남은 방을 보지 않게 한다. */
#ifndef ITEST_SCENARIOS_H
#define ITEST_SCENARIOS_H

#include "client.h"

void scenario_a_welcome_ping(void);
void scenario_b_public_room_pong(void);
void scenario_c_private_code(void);
void scenario_d_quick_join(void);
void scenario_e_rematch(void);
void scenario_f_disconnect_mid_match(void);
void scenario_g_relay_three(void);
void scenario_h_robustness(void);
void scenario_i_slow_reader(void);

/* Pong 경기 중 진행 간격(1/30초)마다 GAME_IN을 보내며 받은 STATE를 기록한다. */
#define PONG_LOG_CAP 512
typedef struct pong_log {
    int states;
    uint32_t tick[PONG_LOG_CAP];
    uint16_t ball_x[PONG_LOG_CAP], ball_y[PONG_LOG_CAP];
    uint32_t last_input_seq; /* 가장 최근 STATE의 값 */
    int goals;
    int stopped; /* stop_type 메시지를 받았나 */
    msg stop;
} pong_log;

/* duration_ms 동안(또는 모두가 stop_type을 받을 때까지, 0이면 시간만) 돌린다.
 * 클라이언트가 끊기면 0, 아니면 1. */
int pump_pong(client *cs[], pong_log logs[], int n, int duration_ms, uint8_t stop_type);

/* 두 기록에 같은 tick이 몇 개 있는지 세고, 그 tick마다 공 위치가 같은지 본다. 다르면 -1. */
int pong_logs_agree(const pong_log *a, const pong_log *b);

/* 목록 응답에서 room_id를 찾는다. 있으면 1(players · max도 돌려줌). */
int room_list_has(const msg *list, uint32_t room_id, uint8_t *players, uint8_t *max);

#endif /* ITEST_SCENARIOS_H */
