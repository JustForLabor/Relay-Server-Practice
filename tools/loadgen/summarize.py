#!/usr/bin/env python3
"""summarize.py - docs/report-data/의 회차별 원자료를 summary.csv · summary_agg.csv로 모은다.

회차 폴더 하나 = meta.json(조건) + loadgen.json(도구 측정) + server_stats.jsonl(서버 1초 줄).
서버 줄은 loadgen의 측정 구간(window_unix_ms) 안에 끝나는 줄만 쓴다: 접속을 여는 동안과
끝난 뒤 정리하는 동안의 값이 섞이지 않게 한다.

  python3 summarize.py <report-data 폴더>
"""
import csv
import json
import os
import statistics
import sys

RUN_COLS = [
    "scenario", "run", "rep", "mode", "bots", "delay_ms", "loss_pct", "status",
    "loadavg_start", "loadavg_end",
    "conn_ok", "conn_fail", "dropped", "udp_bound", "udp_fallback", "matches_started",
    "window_s", "inputs_sent", "states_rx", "lat_n", "lat_p50", "lat_p95", "lat_p99", "lat_max",
    "int_p50", "int_p95", "int_p99", "int_max", "tick_skip_ratio", "states_reordered",
    "inputs_unreflected", "rtt_n", "rtt_p50", "rtt_p99", "send_lag_p99", "send_lag_max",
    "send_lag_all_max", "late_tick_all", "tool_cpu", "srv_lines", "srv_conns_max", "srv_ticks",
    "srv_ticks_dropped",
    "srv_tick_us_p99_med", "srv_tick_us_p99_max", "srv_tick_us_max", "srv_loop_us_p99_med",
    "srv_rss_kb_max", "srv_cpu", "srv_udp_tx_dropped",
]

# 회차 평균 · 최소 · 최대를 낼 열(같은 조건 3회의 흩어짐)
AGG_COLS = [
    "conn_ok", "matches_started", "lat_p50", "lat_p95", "lat_p99", "lat_max", "int_p50",
    "int_p99", "int_max", "tick_skip_ratio", "rtt_p50", "rtt_p99", "send_lag_p99",
    "send_lag_all_max", "late_tick_all", "inputs_unreflected", "tool_cpu",
    "srv_ticks_dropped", "srv_tick_us_p99_med", "srv_tick_us_p99_max", "srv_tick_us_max",
    "srv_rss_kb_max", "srv_cpu",
]
GROUP = ["scenario", "mode", "bots", "delay_ms", "loss_pct"]


def server_row(path, win):
    rows = []
    if not os.path.exists(path):
        return {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            # 구간 [ts - interval, ts]가 측정 구간 안에 들어간 줄만
            if win and d["ts_ms"] - d["interval_ms"] >= win[0] - 50 and d["ts_ms"] <= win[1] + 50:
                rows.append(d)
    if not rows:
        return {"srv_lines": 0}
    p99s = [r["tick_us_p99"] for r in rows if r["tick_samples"] > 0]
    loops = [r["loop_us_p99"] for r in rows if r["loop_samples"] > 0]
    ms = sum(r["interval_ms"] for r in rows)
    return {
        "srv_lines": len(rows),
        "srv_conns_max": max(r["conns"] for r in rows),
        "srv_ticks": sum(r["ticks"] for r in rows),
        "srv_ticks_dropped": sum(r["ticks_dropped"] for r in rows),
        "srv_tick_us_p99_med": statistics.median(p99s) if p99s else 0,
        "srv_tick_us_p99_max": max(p99s) if p99s else 0,
        "srv_tick_us_max": max(r["tick_us_max"] for r in rows),
        "srv_loop_us_p99_med": statistics.median(loops) if loops else 0,
        "srv_rss_kb_max": max(r["rss_kb"] for r in rows),
        "srv_cpu": round(sum(r["cpu_user_ms"] + r["cpu_sys_ms"] for r in rows) / ms, 4) if ms else 0,
        "srv_udp_tx_dropped": sum(r.get("udp_tx_dropped", 0) for r in rows),
    }


def run_row(run_dir):
    with open(os.path.join(run_dir, "meta.json"), encoding="utf-8") as f:
        meta = json.load(f)
    row = {k: meta.get(k, "") for k in ["scenario", "run", "rep", "mode", "bots", "delay_ms",
                                        "loss_pct", "status", "loadavg_start", "loadavg_end"]}
    lg_path = os.path.join(run_dir, "loadgen.json")
    if not os.path.exists(lg_path):
        return row
    with open(lg_path, encoding="utf-8") as f:
        lg = json.load(f)
    w = lg["window"]
    lat, iv, rtt, lag = (lg["input_latency_ms"], lg["state_interval_ms"], lg["rtt_ms"],
                         lg["input_send_lag_ms"])
    row.update({
        "conn_ok": lg["connect"]["ok"], "conn_fail": lg["connect"]["fail"],
        "dropped": lg["connect"]["dropped_after_welcome"],
        "udp_bound": lg["udp"]["bound"], "udp_fallback": lg["udp"]["fallback_tcp"],
        "matches_started": lg["matches_started"], "window_s": lg["window_s"],
        "inputs_sent": w["inputs_sent"], "states_rx": w["states_rx"],
        "lat_n": lat["count"], "lat_p50": lat["p50"], "lat_p95": lat["p95"],
        "lat_p99": lat["p99"], "lat_max": lat["max"],
        "int_p50": iv["p50"], "int_p95": iv["p95"], "int_p99": iv["p99"], "int_max": iv["max"],
        "tick_skip_ratio": w["tick_skip_ratio"], "states_reordered": w["states_reordered"],
        "inputs_unreflected": w["inputs_unreflected"],
        "rtt_n": rtt["count"], "rtt_p50": rtt["p50"], "rtt_p99": rtt["p99"],
        "send_lag_p99": lag["p99"], "send_lag_max": lag["max"],
        "send_lag_all_max": lg["input_send_lag_all_ms"]["max"],
        "late_tick_all": lg["inputs_late_tick_all"],
        "tool_cpu": lg["tool"]["cpu_util"],
    })
    row.update(server_row(os.path.join(run_dir, "server_stats.jsonl"), lg.get("window_unix_ms")))
    return row


def cell(rs, col, digits=1, scale=1.0):
    """회차 평균, 회차가 여럿이고 값이 다르면 괄호에 최소-최대."""
    vals = [float(r[col]) * scale for r in rs if r.get(col, "") != ""]
    if not vals:
        return "없음"
    mean, lo, hi = statistics.fmean(vals), min(vals), max(vals)
    if len(vals) == 1 or lo == hi:
        return f"{mean:.{digits}f}"
    return f"{mean:.{digits}f} ({lo:.{digits}f}-{hi:.{digits}f})"


def reps_cell(rs):
    return f"{sum(1 for r in rs if r.get('status') == 'ok')}/{len(rs)}"


def write_tables(root, groups):
    """README가 그대로 싣는 S1 · S2 · 자체 검증 표. 손으로 옮기다 숫자가 어긋나지 않게 여기서 만든다."""
    s1 = sorted(((k, rs) for k, rs in groups.items() if k[0] == "S1"), key=lambda kv: int(kv[0][2]))
    s2 = sorted(((k, rs) for k, rs in groups.items() if k[0] == "S2"),
                key=lambda kv: (float(kv[0][4]), kv[0][1]))
    out = ["# 측정 표 (summarize.py가 만듦, 손으로 고치지 않는다)", "",
           "칸 값은 같은 조건 회차의 평균이고, 괄호는 회차 최소-최대다.", ""]
    if s1:
        out += ["## S1 규모 (tcp, 손실 0)", "",
                "| 봇 | 정상 회차 | 접속 성공 | 접속 실패 | 접속 뒤 끊김 | 서버 진행 처리 p99 µs (초별 중앙값) "
                "| 초별 p99 최대 µs | 버린 진행 | 입력 지연 p50 ms | 입력 지연 p99 ms | 입력 지연 표본 "
                "| 반영 안 된 입력 "
                "| 서버 RSS MB | 서버 CPU % | 도구 CPU % | 도구 송신 밀림 p99 ms "
                "| 한 진행 넘게 늦게 보낸 입력 (실행 전체) |",
                "|" + "---|" * 17]
        for k, rs in s1:
            out.append(
                f"| {k[2]} | {reps_cell(rs)} | {cell(rs, 'conn_ok', 0)} | {cell(rs, 'conn_fail', 0)} "
                f"| {cell(rs, 'dropped', 0)} | {cell(rs, 'srv_tick_us_p99_med', 0)} "
                f"| {cell(rs, 'srv_tick_us_p99_max', 0)} | {cell(rs, 'srv_ticks_dropped', 0)} "
                f"| {cell(rs, 'lat_p50')} | {cell(rs, 'lat_p99')} | {cell(rs, 'lat_n', 0)} "
                f"| {cell(rs, 'inputs_unreflected', 0)} "
                f"| {cell(rs, 'srv_rss_kb_max', 1, 1 / 1024)} | {cell(rs, 'srv_cpu', 1, 100)} "
                f"| {cell(rs, 'tool_cpu', 1, 100)} | {cell(rs, 'send_lag_p99', 2)} "
                f"| {cell(rs, 'late_tick_all', 0)} |")
        out.append("")
    if s2:
        out += ["## S2 전송 비교 (봇 200, netem 지연 20ms)", "",
                "| 손실 % | 방식 | 정상 회차 | UDP 묶인 봇 | TCP로 돌아간 봇 | 입력 지연 p50 ms "
                "| 입력 지연 p95 ms | 입력 지연 p99 ms | 입력 지연 표본 | 반영 안 된 입력 "
                "| STATE 간격 p99 ms | tick 건너뜀 % | 순서 뒤바뀜 | RTT p50 ms "
                "| 한 진행 넘게 늦게 보낸 입력 (실행 전체) |",
                "|" + "---|" * 15]
        for k, rs in s2:
            out.append(
                f"| {k[4]} | {k[1]} | {reps_cell(rs)} | {cell(rs, 'udp_bound', 0)} "
                f"| {cell(rs, 'udp_fallback', 0)} | {cell(rs, 'lat_p50')} | {cell(rs, 'lat_p95')} "
                f"| {cell(rs, 'lat_p99')} | {cell(rs, 'lat_n', 0)} | {cell(rs, 'inputs_unreflected', 0)} "
                f"| {cell(rs, 'int_p99')} "
                f"| {cell(rs, 'tick_skip_ratio', 2, 100)} | {cell(rs, 'states_reordered', 0)} "
                f"| {cell(rs, 'rtt_p50')} | {cell(rs, 'late_tick_all', 0)} |")
        out.append("")
    sc = sorted(((k, rs) for k, rs in groups.items() if k[0].startswith("selfcheck")),
                key=lambda kv: (kv[0][0], kv[0][1], int(kv[0][3])))
    if sc:
        out += ["## 자체 검증 (봇 2개 = 한 경기)", "",
                "| 시나리오 | 방식 | netem 지연 ms | 정상 회차 | 경기 시작 | 입력 지연 p50 ms "
                "| 입력 지연 p99 ms | 입력 지연 표본 | 반영 안 된 입력 | RTT p50 ms | RTT p99 ms |",
                "|" + "---|" * 11]
        for k, rs in sc:
            out.append(
                f"| {k[0]} | {k[1]} | {k[3]} | {reps_cell(rs)} | {cell(rs, 'matches_started', 0)} "
                f"| {cell(rs, 'lat_p50')} | {cell(rs, 'lat_p99')} | {cell(rs, 'lat_n', 0)} "
                f"| {cell(rs, 'inputs_unreflected', 0)} | {cell(rs, 'rtt_p50', 2)} "
                f"| {cell(rs, 'rtt_p99', 2)} |")
        out.append("")
    bad = [r for _, rs in s1 + s2 + sc for r in rs if r.get("status") != "ok"]
    out += ["## 정상으로 끝나지 않은 회차", ""]
    out += [f"- {r['scenario']}/{r['run']}: {r['status']}" for r in bad] or ["- 없음"]
    with open(os.path.join(root, "tables.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "docs/report-data"
    rows = []
    for scen in sorted(os.listdir(root)):
        sdir = os.path.join(root, scen)
        if not os.path.isdir(sdir):
            continue
        for run in sorted(os.listdir(sdir)):
            rdir = os.path.join(sdir, run)
            if os.path.exists(os.path.join(rdir, "meta.json")):
                rows.append(run_row(rdir))
    with open(os.path.join(root, "summary.csv"), "w", newline="", encoding="utf-8") as f:
        wr = csv.DictWriter(f, fieldnames=RUN_COLS, extrasaction="ignore")
        wr.writeheader()
        wr.writerows(rows)

    groups = {}
    for r in rows:
        groups.setdefault(tuple(r.get(k, "") for k in GROUP), []).append(r)
    agg_cols = GROUP + ["reps", "reps_ok"]
    for c in AGG_COLS:
        agg_cols += [c + "_mean", c + "_min", c + "_max"]
    with open(os.path.join(root, "summary_agg.csv"), "w", newline="", encoding="utf-8") as f:
        wr = csv.DictWriter(f, fieldnames=agg_cols)
        wr.writeheader()
        for key, rs in groups.items():
            out = dict(zip(GROUP, key))
            out["reps"] = len(rs)
            out["reps_ok"] = sum(1 for r in rs if r.get("status") == "ok")
            for c in AGG_COLS:
                vals = [float(r[c]) for r in rs if r.get(c, "") != ""]
                if vals:
                    out[c + "_mean"] = round(statistics.fmean(vals), 4)
                    out[c + "_min"] = min(vals)
                    out[c + "_max"] = max(vals)
            wr.writerow(out)
    write_tables(root, groups)
    print(f"summarize: {len(rows)} runs, {len(groups)} groups -> {root}/summary.csv, "
          "summary_agg.csv, tables.md")


if __name__ == "__main__":
    main()
