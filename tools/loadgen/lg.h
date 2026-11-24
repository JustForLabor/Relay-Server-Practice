/* lg.h - loadgen 공통 정의: 설정, 봇, 집계, 타이머.
 *
 * 한 프로세스 · 한 스레드가 epoll로 봇 N개를 돌린다. 봇 하나 = TCP 연결 하나
 * (udp 모드면 UDP 소켓 하나 더). 봇 둘이 짝이 되어 비공개 Pong 방 하나에서 경기한다. */
#ifndef LOADGEN_LG_H
#define LOADGEN_LG_H

#include <netinet/in.h>
#include <stdint.h>

#include "hist.h"

#define TICK_HZ 30
#define SEQ_RING 256           /* 입력 보낸 시각을 기억하는 개수(약 8.5초 분량) */
#define RBUF_CAP 8192
#define WBUF_CAP 4096
#define PING_INTERVAL_NS 1000000000ull
#define BIND_RETRY_NS 200000000ull  /* protocol.md 7장: 0.2초마다 다시 */
#define BIND_GIVEUP_NS 2000000000ull /* 2초 안에 UDP_BOUND가 없으면 TCP만 */
#define AIM_SPREAD 80          /* 공을 노리는 오차 ±. 패들 반높이 + 공 반변(55)보다 커서 가끔 놓친다 */

enum { MODE_TCP = 0, MODE_UDP = 1 };

typedef struct config {
    struct sockaddr_in addr;
    const char *host;
    uint16_t port;
    uint32_t bots;
    int mode;
    double duration_s, ramp_s, warmup_s;
    const char *out;
    uint32_t seed;
    uint8_t win_score;
} config;

enum bot_state {
    B_IDLE,       /* 아직 접속 시각이 안 됨 */
    B_CONNECTING, /* 비차단 connect 진행 중 */
    B_WELCOME,    /* 접속됨, WELCOME 기다림 */
    B_BINDING,    /* udp 모드: UDP_BOUND 기다림 */
    B_READY,      /* 로비: 방을 만들거나 코드를 기다림 */
    B_IN_ROOM,    /* 방 안 대기(짝 기다림 · 경기 끝) */
    B_PLAYING,
    B_DEAD,
};

/* 접속 실패 · 연결 끊김 이유. 결과 JSON의 이름표와 순서가 같다(report.c). */
enum fail_reason {
    F_SOCKET,         /* socket() 실패(EMFILE 등) */
    F_CONNECT,        /* connect가 오류로 끝남 */
    F_SERVER_FULL,    /* ERROR(SERVER_FULL) */
    F_CLOSED,         /* 서버가 닫음 */
    F_PROTOCOL,       /* 서버가 보낸 바이트가 명세와 다름 */
    F_TX_OVERFLOW,    /* 도구 송신 버퍼가 넘침(도구가 밀림) */
    F_NO_WELCOME,     /* 끝날 때까지 WELCOME 못 받음 */
    F_COUNT
};

typedef struct bot {
    int fd, ufd;
    uint32_t idx;
    uint8_t state;
    uint8_t slot;
    uint8_t udp_on;   /* UDP_BOUND 받음 */
    uint8_t welcomed;
    uint8_t has_code; /* 방장: ROOM_JOINED로 코드를 받음 */
    uint8_t want_out; /* EPOLLOUT 켜져 있음 */
    char code[6];
    uint32_t udp_token;
    uint32_t rng;

    uint64_t connect_at;  /* 0이면 예약 없음. 아래 *_at도 같은 뜻 */
    uint64_t bind_start, bind_at;
    uint64_t input_base;  /* MATCH_START 받은 시각 */
    uint64_t input_n;     /* input_base 뒤로 보낸 개수 */
    uint64_t input_at;
    uint64_t ping_at;
    uint32_t ping_val;
    uint64_t ping_sent;   /* 0이면 답을 기다리지 않음 */

    uint32_t seq;            /* 이번 경기에서 마지막으로 보낸 input_seq. 경기마다 1부터 */
    uint32_t last_reflected; /* STATE.last_input_seq로 본 가장 큰 값 */
    uint64_t sent_ns[SEQ_RING];
    uint8_t axis[SEQ_RING];

    int64_t last_tick;   /* 받아들인 마지막 STATE tick. 경기 시작 때 start_tick − 1 */
    uint64_t last_state; /* 받아들인 마지막 STATE 도착 시각, 0이면 아직 없음 */
    uint16_t ball_x, ball_y, my_y;
    uint8_t incoming; /* 공이 내 쪽으로 오는 중. 오기 시작할 때마다 노림 오차를 다시 뽑는다 */
    int16_t aim;

    size_t rlen, wlen;
    uint8_t rbuf[RBUF_CAP];
    uint8_t wbuf[WBUF_CAP];
} bot;

typedef struct stats {
    /* 실행 전체 */
    uint64_t conn_ok, conn_fail, fail[F_COUNT];
    uint64_t dropped, drop[F_COUNT]; /* WELCOME을 받은 뒤 끊긴 연결 */
    uint64_t udp_bound, udp_fallback;
    uint64_t rooms_created, joins_sent, matches_started, matches_ended, rematch_sent, goals;
    uint64_t errors_rx[16]; /* ERROR code별 (15 이상은 15) */
    uint64_t udp_tx_err;    /* UDP send 실패(커널 버퍼 가득 · ICMP 거부) */
    /* 입력 송신 밀림을 측정 구간 밖(접속을 여는 동안)까지 센다. 서버는 진행마다 큐에서 하나만
     * 꺼내므로, 도구가 한 진행 간격 넘게 멈췄다가 밀린 입력을 몰아 보내면 큐가 늘어난 채
     * 경기 끝까지 줄지 않아 측정 구간의 입력 지연이 진행 간격 단위로 커진다. 그 원인을 가른다. */
    uint64_t inputs_late_tick_all; /* 예정보다 한 진행 간격 이상 늦게 보낸 입력 */
    hist send_lag_all;
    /* 측정 구간만 */
    uint64_t inputs_sent, inputs_sent_udp, states_rx, states_rx_udp, states_ok, ticks_skipped,
        states_reordered, inputs_unreflected, latency_untracked, pings_sent, pongs_rx;
    hist lat, interval, rtt, send_lag;
} stats;

/* 타이머 종류. 봇의 *_at 값과 같지 않은 항목은 지난 예약으로 보고 건너뛴다. */
enum timer_kind { T_CONNECT, T_INPUT, T_PING, T_BIND };

typedef struct loadgen {
    config cfg;
    int epfd;
    bot *bots;
    stats st;
    int measuring;
    uint64_t t0, win_start, win_end;
    /* 측정 구간의 실제 시각(유닉스 ms). 서버 stats 줄(ts_ms)과 구간을 맞출 때 쓴다. */
    uint64_t win_unix_ms[2];
} loadgen;

extern loadgen g;

uint64_t now_ns(void);

/* timers.c - (시각, 봇, 종류) 최소 힙 */
int timer_push(uint64_t when, uint32_t idx, uint8_t kind);
int timer_peek(uint64_t *when);
void timer_pop(uint64_t *when, uint32_t *idx, uint8_t *kind);
void timers_free(void);

/* bot.c - 프로토콜 동작 */
void bot_on_msg(bot *b, uint8_t type, const uint8_t *p, uint16_t len, int via_udp);
void bot_on_timer(bot *b, uint8_t kind, uint64_t when);

/* botio.c - 소켓 입출력 */
void bot_connect(bot *b);
void bot_on_tcp(bot *b, uint32_t events);
void bot_on_udp(bot *b);
void bot_send_frame(bot *b, uint8_t type, const uint8_t *payload, uint16_t len);
void bot_send_udp(bot *b, uint8_t type, const uint8_t *payload, uint16_t len);
int bot_open_udp(bot *b);
void bot_close_udp(bot *b);
void bot_fail(bot *b, int reason);
void bot_close(bot *b);

/* report.c */
int report_write(const char *path, double window_s, double cpu_user_s, double cpu_sys_s,
                 long max_rss_kb, uint64_t loop_iters, uint64_t timers_fired);

#endif /* LOADGEN_LG_H */
