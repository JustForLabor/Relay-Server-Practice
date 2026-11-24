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
void scenario_j_udp_bind(void);
void scenario_k_udp_ignored(void);
void scenario_l_max_conns(void);
void scenario_m_stats(void);

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

/* Pong 규칙 값 25바이트: 공이 첫 서브에 패들을 빗나가 바로 득점하고 win_score 1로 끝난다. */
void fast_settings(uint8_t s[25]);

/* ---- UDP 섞인 경기 (scenarios_udp.c) ---- */

/* 경로별로 센 Pong GAME_OUT. STATE는 tick이 가장 큰 것을 "최신"으로 본다. */
typedef struct ulog {
    int udp_states, tcp_states, udp_goals, tcp_goals;
    int any_state;
    uint32_t last_tick, last_input_seq; /* 최신 STATE의 값 */
    int seq_went_back; /* tick은 커졌는데 last_input_seq가 작아진 횟수 */
    int bad_udp;       /* 프레임이 아니거나 GAME_OUT이 아닌 데이터그램 */
    int stopped;
    msg stop;
} ulog;

/* a가 비공개 Pong 방을 만들고 b가 코드로 들어가 둘 다 MATCH_START까지. bind_a · bind_b면 방에
 * 들어가기 전에 UDP로 묶는다. 성공 1. */
int udp_pair(client *a, client *b, int bind_a, int bind_b, const uint8_t *settings,
             uint8_t settings_len);

/* pump_pong과 같지만 ufd가 열린 클라이언트는 묶음 GAME_IN을 UDP로 보내고 UDP도 받는다.
 * TCP가 끊기면 0. */
int udp_pump(client *cs[], ulog logs[], int n, int duration_ms, uint8_t stop_type);

/* 서버 로그 파일 이름 앞부분(통합 테스트 실행 파일 경로). 따로 띄우는 서버의 로그가 쓴다. */
extern const char *g_itest_log_prefix;

#endif /* ITEST_SCENARIOS_H */
