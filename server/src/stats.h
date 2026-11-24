/* stats.h - 서버 통계 (overview.md 6.1 stats).
 *
 * --stats-interval SEC이면 SEC마다 stdout에 JSON 한 줄을 쓴다. 필드는 stats.c의 stats_report 참고.
 * 카운터는 늘 센다(정수 더하기 하나라 거의 공짜). 시간 측정(clock_gettime)과 히스토그램 기록은
 * 통계를 켰을 때만 한다. 히스토그램은 고정 크기 배열이라 표본이 많아도 메모리가 늘지 않는다.
 */
#ifndef STATS_H
#define STATS_H

#include <stdbool.h>
#include <stdint.h>

/* 구간 카운터. 보고할 때마다 0으로 돌린다. */
typedef struct stats_counters {
    uint64_t tcp_rx_bytes, tcp_tx_bytes;
    uint64_t udp_rx_bytes, udp_tx_bytes;
    uint64_t tcp_rx_frames, tcp_tx_frames;
    uint64_t udp_rx_datagrams, udp_tx_datagrams;
    uint64_t udp_tx_dropped; /* sendto가 EAGAIN 등으로 실패해 버린 데이터그램 */
    uint64_t conns_rejected; /* SERVER_FULL로 돌려보낸 접속 */
    uint64_t ticks;          /* 모든 방의 on_tick 호출 수 */
    uint64_t ticks_dropped;  /* 밀려서 버린 진행 수(protocol.md 5.2절 8번) */
} stats_counters;

extern stats_counters g_stats;

/* --quiet: 연결마다 · 방마다 생기는 로그(연결 종료, 진행 밀림)를 끈다. 부하 측정에서 로그가
 * 쏟아지면 stderr 쓰기가 서버 시간을 잡아먹고 측정을 흐린다. */
extern bool g_quiet_logs;

/* interval_sec이 0이면 끈다. 켜면 /proc/self/statm을 열어 둔다(실패하면 rss_kb는 0). */
void stats_init(unsigned interval_sec, uint64_t now);
void stats_shutdown(void);
bool stats_enabled(void);

/* 한 번의 진행 시각에 모든 방의 on_tick을 돈 시간, 메인 루프 한 바퀴(기다림 제외) 처리 시간. */
void stats_record_tick_ns(uint64_t ns);
void stats_record_loop_ns(uint64_t ns);

/* 다음 보고까지 남은 ms(epoll 타임아웃). 꺼져 있으면 -1. */
int stats_timeout_ms(uint64_t now);

/* 보고 시각이 지났으면 한 줄 쓰고 구간 값을 비운다. */
void stats_maybe_report(uint64_t now);

#endif /* STATS_H */
