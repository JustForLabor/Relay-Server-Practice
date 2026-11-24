/* report.c - 결과 JSON 쓰기. 키 이름은 bench.sh · summarize.py가 읽는다. */
#include <stdio.h>

#include "lg.h"

static const char *const FAIL_NAMES[F_COUNT] = {
    "socket", "connect", "server_full", "closed", "protocol", "tool_tx_overflow", "no_welcome",
};

static void reasons(FILE *f, const uint64_t *v)
{
    fputc('{', f);
    for (int i = 0; i < F_COUNT; i++)
        fprintf(f, "%s\"%s\":%llu", i ? "," : "", FAIL_NAMES[i], (unsigned long long)v[i]);
    fputc('}', f);
}

static void hist_field(FILE *f, const char *name, const hist *h)
{
    fprintf(f, ",\n  \"%s\":", name);
    hist_write_json(h, f);
}

int report_write(const char *path, double window_s, double cpu_user_s, double cpu_sys_s,
                 long max_rss_kb, uint64_t loop_iters, uint64_t timers_fired)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        perror(path);
        return 1;
    }
    const config *c = &g.cfg;
    const stats *s = &g.st;
    uint64_t expected = s->states_ok + s->ticks_skipped;
    fprintf(f,
            "{\n  \"config\":{\"host\":\"%s\",\"port\":%u,\"bots\":%u,\"mode\":\"%s\","
            "\"duration_s\":%.1f,\"ramp_s\":%.1f,\"warmup_s\":%.1f,\"seed\":%u,\"win_score\":%u,"
            "\"tick_hz\":%d},\n",
            c->host, (unsigned)c->port, c->bots, c->mode == MODE_UDP ? "udp" : "tcp",
            c->duration_s, c->ramp_s, c->warmup_s, c->seed, (unsigned)c->win_score, TICK_HZ);
    fprintf(f, "  \"window_s\":%.3f,\"window_unix_ms\":[%llu,%llu],\n", window_s,
            (unsigned long long)g.win_unix_ms[0], (unsigned long long)g.win_unix_ms[1]);
    fprintf(f, "  \"connect\":{\"ok\":%llu,\"fail\":%llu,\"fail_reasons\":",
            (unsigned long long)s->conn_ok, (unsigned long long)s->conn_fail);
    reasons(f, s->fail);
    fprintf(f, ",\"dropped_after_welcome\":%llu,\"drop_reasons\":", (unsigned long long)s->dropped);
    reasons(f, s->drop);
    fprintf(f, "},\n  \"udp\":{\"bound\":%llu,\"fallback_tcp\":%llu,\"tx_err\":%llu},\n",
            (unsigned long long)s->udp_bound, (unsigned long long)s->udp_fallback,
            (unsigned long long)s->udp_tx_err);
    fprintf(f,
            "  \"rooms_created\":%llu,\"joins_sent\":%llu,\"matches_started\":%llu,"
            "\"matches_ended\":%llu,\"rematch_sent\":%llu,\"goals\":%llu,\n",
            (unsigned long long)s->rooms_created, (unsigned long long)s->joins_sent,
            (unsigned long long)s->matches_started, (unsigned long long)s->matches_ended,
            (unsigned long long)s->rematch_sent, (unsigned long long)s->goals);
    fputs("  \"errors_rx\":{", f);
    int first = 1;
    for (int i = 0; i < 16; i++) {
        if (s->errors_rx[i] == 0)
            continue;
        fprintf(f, "%s\"%d\":%llu", first ? "" : ",", i, (unsigned long long)s->errors_rx[i]);
        first = 0;
    }
    fprintf(f, "},\n  \"inputs_late_tick_all\":%llu,\n",
            (unsigned long long)s->inputs_late_tick_all);
    fprintf(f,
            "  \"window\":{\"inputs_sent\":%llu,\"inputs_sent_udp\":%llu,\"states_rx\":%llu,"
            "\"states_rx_udp\":%llu,\"states_accepted\":%llu,\"ticks_skipped\":%llu,"
            "\"tick_skip_ratio\":%.6f,\"states_reordered\":%llu,\"inputs_unreflected\":%llu,"
            "\"latency_untracked\":%llu,\"pings_sent\":%llu,\"pongs_rx\":%llu}",
            (unsigned long long)s->inputs_sent, (unsigned long long)s->inputs_sent_udp,
            (unsigned long long)s->states_rx, (unsigned long long)s->states_rx_udp,
            (unsigned long long)s->states_ok, (unsigned long long)s->ticks_skipped,
            expected ? (double)s->ticks_skipped / (double)expected : 0.0,
            (unsigned long long)s->states_reordered, (unsigned long long)s->inputs_unreflected,
            (unsigned long long)s->latency_untracked, (unsigned long long)s->pings_sent,
            (unsigned long long)s->pongs_rx);
    hist_field(f, "input_latency_ms", &s->lat);
    hist_field(f, "state_interval_ms", &s->interval);
    hist_field(f, "rtt_ms", &s->rtt);
    hist_field(f, "input_send_lag_ms", &s->send_lag);
    hist_field(f, "input_send_lag_all_ms", &s->send_lag_all);
    fprintf(f,
            ",\n  \"tool\":{\"cpu_user_s\":%.3f,\"cpu_sys_s\":%.3f,\"cpu_util\":%.4f,"
            "\"max_rss_kb\":%ld,\"loop_iterations\":%llu,\"timers_fired\":%llu}\n}\n",
            cpu_user_s, cpu_sys_s, window_s > 0 ? (cpu_user_s + cpu_sys_s) / window_s : 0.0,
            max_rss_kb, (unsigned long long)loop_iters, (unsigned long long)timers_fired);
    return fclose(f) == 0 ? 0 : 1;
}
