/* test_tick.c - 고정 간격 진행 시계 (overview.md 6.2 3단계).
 * 30Hz는 정수 나노초로 나누어떨어지지 않는다. 30번 진행한 뒤 정확히 1초 뒤여야 오차가 쌓이지 않는다. */
#include "tick.h"
#include "unit_check.h"

int main(void)
{
    tick_clock t;
    uint64_t base = 1000 * NS_PER_SEC;
    tick_clock_start(&t, 30, base);
    CHECK_EQ(tick_clock_deadline(&t), base + 33333333);
    for (int i = 0; i < 30; i++)
        tick_clock_advance(&t);
    CHECK_EQ(tick_clock_deadline(&t), base + NS_PER_SEC + 33333333);
    for (int i = 0; i < 29; i++)
        tick_clock_advance(&t);
    CHECK_EQ(tick_clock_deadline(&t), base + 2 * NS_PER_SEC); /* 60번째 진행 = 정확히 2초 */

    /* epoll 타임아웃은 올림: 1ns 남아도 1ms 잔다(0이면 바쁜 대기가 된다). */
    CHECK_EQ(tick_ms_until(base + 1, base), 1);
    CHECK_EQ(tick_ms_until(base + NS_PER_MS, base), 1);
    CHECK_EQ(tick_ms_until(base + NS_PER_MS + 1, base), 2);
    CHECK_EQ(tick_ms_until(base, base), 0);
    CHECK_EQ(tick_ms_until(base, base + 5), 0);

    /* 1초 멈췄다 깨어남: 밀린 약 30번을 버리고 지금부터 한 간격 뒤로 다시 센다. */
    tick_clock_start(&t, 30, base);
    uint64_t late = base + NS_PER_SEC;
    tick_clock_drop_backlog(&t, late);
    CHECK_EQ(t.dropped, 30);
    CHECK_EQ(tick_clock_deadline(&t), late + 33333333);

    /* 아직 밀리지 않았으면 버린 수는 0 */
    tick_clock_start(&t, 30, base);
    tick_clock_drop_backlog(&t, base + 1);
    CHECK_EQ(t.dropped, 0);

    CHECK(tick_now_ns() > 0);
    return UNIT_DONE("test_tick");
}
