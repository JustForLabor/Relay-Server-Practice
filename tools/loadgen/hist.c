/* hist.c - 고정 칸 히스토그램 (hist.h). */
#include "hist.h"

void hist_add(hist *h, uint64_t ns)
{
    uint64_t i = ns / HIST_BIN_NS;
    if (i < HIST_BINS)
        h->bins[i]++;
    else
        h->overflow++;
    h->count++;
    if (ns > h->max_ns)
        h->max_ns = ns;
    h->sum_ms += (double)ns / 1e6;
}

double hist_quantile_ms(const hist *h, double q)
{
    if (h->count == 0)
        return 0.0;
    /* 순위는 올림: count × q번째 표본이 들어 있는 칸. p100이 max와 같은 칸을 가리킨다. */
    uint64_t rank = (uint64_t)((double)h->count * q);
    if ((double)rank < (double)h->count * q)
        rank++;
    if (rank == 0)
        rank = 1;
    uint64_t seen = 0;
    for (uint32_t i = 0; i < HIST_BINS; i++) {
        seen += h->bins[i];
        if (seen >= rank) {
            double upper = (double)((i + 1) * HIST_BIN_NS) / 1e6;
            double max = (double)h->max_ns / 1e6;
            return upper < max ? upper : max;
        }
    }
    return (double)h->max_ns / 1e6;
}

void hist_write_json(const hist *h, FILE *f)
{
    fprintf(f,
            "{\"count\":%llu,\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f,\"mean\":%.3f,"
            "\"overflow\":%llu,\"bin_ms\":%.1f,\"bins\":[",
            (unsigned long long)h->count, hist_quantile_ms(h, 0.50), hist_quantile_ms(h, 0.95),
            hist_quantile_ms(h, 0.99), (double)h->max_ns / 1e6,
            h->count ? h->sum_ms / (double)h->count : 0.0, (unsigned long long)h->overflow,
            (double)HIST_BIN_NS / 1e6);
    int first = 1;
    for (uint32_t i = 0; i < HIST_BINS; i++) {
        if (h->bins[i] == 0)
            continue;
        fprintf(f, "%s[%u,%llu]", first ? "" : ",", i, (unsigned long long)h->bins[i]);
        first = 0;
    }
    fputs("]}", f);
}
