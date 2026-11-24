/* hist.h - 고정 칸 히스토그램. 0.1ms 칸 HIST_BINS개 + 넘침 칸.
 *
 * 봇 수 · 측정 시간과 상관없이 메모리가 일정해야 4000봇 60초에도 도구가 표본을 쌓느라
 * 느려지지 않는다. 0.1ms 해상도는 루프백 RTT(0.1ms 안팎)와 진행 간격(33.3ms)을 함께
 * 구분하기에 충분하다. 최대값은 칸과 따로 정확하게 둔다. */
#ifndef LOADGEN_HIST_H
#define LOADGEN_HIST_H

#include <stdint.h>
#include <stdio.h>

#define HIST_BIN_NS 100000ull /* 0.1ms */
#define HIST_BINS 50000u      /* 0–5000ms. 그 위는 넘침 칸 */

typedef struct hist {
    uint64_t bins[HIST_BINS];
    uint64_t overflow;
    uint64_t count;
    uint64_t max_ns;
    double sum_ms;
} hist;

void hist_add(hist *h, uint64_t ns);

/* q(0–1) 분위수 ms. 칸의 위쪽 끝을 돌려준다(실제 값보다 최대 0.1ms 크게 잡힌다).
 * 넘침 칸에 걸리면 max를 돌려준다. 표본이 없으면 0. */
double hist_quantile_ms(const hist *h, double q);

/* {"count":..,"p50":..,"p95":..,"p99":..,"max":..,"mean":..,"bins":[[칸,개수],..]}
 * bins는 0이 아닌 칸만. 여러 프로세스 결과를 합칠 때 다시 분위수를 구할 수 있게 남긴다. */
void hist_write_json(const hist *h, FILE *f);

#endif /* LOADGEN_HIST_H */
