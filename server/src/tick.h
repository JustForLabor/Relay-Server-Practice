/* tick.h - 단조 시계와 고정 간격 진행 시계 (overview.md 6.1 tick, 6.2 3단계).
 *
 * 시각은 CLOCK_MONOTONIC 나노초 정수다. 1/30초는 정수 나노초로 나누어떨어지지 않으므로
 * "다음 시각 += 간격"을 되풀이하면 오차가 쌓인다. 대신 기준 시각 + n번째 진행 시각을
 * 곱한 뒤 나누어 매번 새로 계산한다.
 */
#ifndef TICK_H
#define TICK_H

#include <stdbool.h>
#include <stdint.h>

#define NS_PER_SEC UINT64_C(1000000000)
#define NS_PER_MS UINT64_C(1000000)

/* 서버가 한 반복에서 밀린 진행을 몰아서 하는 상한. 넘으면 남은 것을 버리고 지금부터 다시 센다.
 * 오래 멈췄다 깨어나 수백 번을 한꺼번에 돌리면 그동안 다른 방과 패킷 처리가 멈추기 때문이다. */
#define TICK_MAX_CATCHUP 5

uint64_t tick_now_ns(void);

typedef struct tick_clock {
    uint64_t base_ns; /* 0번째 진행의 기준 시각 */
    uint64_t done;    /* base 이후 한 진행 수 */
    uint16_t hz;
    uint64_t dropped; /* 상한 때문에 버린 진행 수(통계 · 로그용) */
} tick_clock;

/* 지금(now)부터 센다. 첫 진행은 한 간격 뒤다. */
void tick_clock_start(tick_clock *t, uint16_t hz, uint64_t now);

/* 다음 진행 시각. */
uint64_t tick_clock_deadline(const tick_clock *t);

/* 진행 하나를 했다고 기록한다. */
static inline void tick_clock_advance(tick_clock *t)
{
    t->done++;
}

/* 상한만큼 진행했는데도 아직 밀려 있으면 밀린 것을 버리고 now 기준으로 다시 센다. */
void tick_clock_drop_backlog(tick_clock *t, uint64_t now);

/* deadline까지 남은 ms를 올림해서 돌려준다(epoll_wait 타임아웃). 이미 지났으면 0. */
int tick_ms_until(uint64_t deadline, uint64_t now);

#endif /* TICK_H */
