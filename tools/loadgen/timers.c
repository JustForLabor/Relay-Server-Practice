/* timers.c - 봇 예약 시각의 최소 힙.
 *
 * 봇 4000개가 각자 1/30초마다 입력을 보내므로, 매 바퀴 모든 봇을 훑어 가장 이른 시각을
 * 찾으면 도구가 병목이 된다. 힙은 넣기 · 꺼내기가 log N이다. 예약을 취소하지 않고
 * 봇 쪽 *_at 값과 비교해 지난 항목을 건너뛴다(lg.h). */
#include <stdlib.h>

#include "lg.h"

typedef struct timer_ent {
    uint64_t when;
    uint32_t idx;
    uint8_t kind;
} timer_ent;

static timer_ent *heap;
static size_t heap_len, heap_cap;

int timer_push(uint64_t when, uint32_t idx, uint8_t kind)
{
    if (heap_len == heap_cap) {
        size_t cap = heap_cap ? heap_cap * 2 : 1024;
        timer_ent *n = realloc(heap, cap * sizeof *n);
        if (n == NULL)
            return -1;
        heap = n;
        heap_cap = cap;
    }
    size_t i = heap_len++;
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (heap[parent].when <= when)
            break;
        heap[i] = heap[parent];
        i = parent;
    }
    heap[i] = (timer_ent){.when = when, .idx = idx, .kind = kind};
    return 0;
}

int timer_peek(uint64_t *when)
{
    if (heap_len == 0)
        return 0;
    *when = heap[0].when;
    return 1;
}

void timer_pop(uint64_t *when, uint32_t *idx, uint8_t *kind)
{
    *when = heap[0].when;
    *idx = heap[0].idx;
    *kind = heap[0].kind;
    timer_ent last = heap[--heap_len];
    size_t i = 0;
    for (;;) {
        size_t l = 2 * i + 1;
        if (l >= heap_len)
            break;
        size_t m = (l + 1 < heap_len && heap[l + 1].when < heap[l].when) ? l + 1 : l;
        if (last.when <= heap[m].when)
            break;
        heap[i] = heap[m];
        i = m;
    }
    if (heap_len > 0)
        heap[i] = last;
}

void timers_free(void)
{
    free(heap);
    heap = NULL;
    heap_len = heap_cap = 0;
}
