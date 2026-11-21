/* tick.c - 시계 계산. 설명은 tick.h. */
#include "tick.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

uint64_t tick_now_ns(void)
{
    struct timespec ts;
    /* MONOTONIC은 NTP나 사람이 시계를 바꿔도 뒤로 가지 않는다. 진행 간격 계산에 필요하다. */
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        abort();
    }
    return (uint64_t)ts.tv_sec * NS_PER_SEC + (uint64_t)ts.tv_nsec;
}

void tick_clock_start(tick_clock *t, uint16_t hz, uint64_t now)
{
    t->base_ns = now;
    t->done = 0;
    t->hz = hz;
    t->dropped = 0;
}

uint64_t tick_clock_deadline(const tick_clock *t)
{
    return t->base_ns + (t->done + 1) * NS_PER_SEC / t->hz;
}

void tick_clock_drop_backlog(tick_clock *t, uint64_t now)
{
    /* 지금까지 시각이 된 진행 번호의 끝 m을 구해 아직 하지 않은 것(m - done)을 버린 수로 센다.
     * deadline 계산과 똑같이 내림 나눗셈을 쓰므로 경계에서 하나 차이가 나면 바로잡는다. */
    uint64_t elapsed = now - t->base_ns;
    uint64_t m = elapsed * t->hz / NS_PER_SEC;
    if ((m + 1) * NS_PER_SEC / t->hz <= elapsed)
        m++;
    if (m > t->done)
        t->dropped += m - t->done;
    t->base_ns = now;
    t->done = 0;
}

int tick_ms_until(uint64_t deadline, uint64_t now)
{
    if (deadline <= now)
        return 0;
    uint64_t ms = (deadline - now + NS_PER_MS - 1) / NS_PER_MS;
    return ms > INT_MAX ? INT_MAX : (int)ms;
}
