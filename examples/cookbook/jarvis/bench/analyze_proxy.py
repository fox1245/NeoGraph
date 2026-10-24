#!/usr/bin/env python3
"""Boundary-instrumented residual analysis — subtracts per-call upstream (OpenRouter)
latency in the proxy log from each turn's round-trip time.

  residual = turn round-trip − sum of upstream_response_time for that turn's two calls

Each round receives its own proxy log. Calls sorted by completion timestamp are
paired two per turn, so residuals are computed only when the call count equals
exactly twice the turn count. Runs with retries or extra calls should verify this assumption.

Usage:
  python3 analyze_proxy.py <label1>:<jsonl1>:<log1> [<label2>:<jsonl2>:<log2> ...]
"""
import json
import statistics as st
import sys


def parse_log(path):
    calls = []
    for line in open(path, encoding="utf-8"):
        parts = line.split()
        if len(parts) < 6:
            continue
        try:
            ts = float(parts[0])
            upstream = parts[4]
            # Sum comma-separated values on upstream retries (e.g. "0.1, 0.2")
            up = sum(float(x) for x in upstream.replace(",", " ").split()
                     if x not in ("-",))
            calls.append((ts, up, parts[5]))
        except ValueError:
            continue
    return calls


def main():
    # Each round has an independent log (label:jsonl:log) — no round splitting or
    # cross-container time-window matching needed. Pair in order when calls == 2 * turns.
    specs = [s.split(":") for s in sys.argv[1:]]

    rows = []
    for label, jsonl, log in specs:
        cluster = sorted(parse_log(log))
        turns = []
        for line in open(jsonl, encoding="utf-8"):
            o = json.loads(line)
            if "summary" in o or o.get("ms") is None or "t0" not in o:
                continue
            turns.append(o)

        residuals, upstream_sums, call_counts, unmatched = [], [], [], 0
        if len(cluster) == 2 * len(turns):
            for k, t in enumerate(turns):
                mine = cluster[2 * k: 2 * k + 2]
                up_ms = sum(up for _, up, _ in mine) * 1000.0
                residuals.append(t["ms"] - up_ms)
                upstream_sums.append(up_ms)
                call_counts.append(len(mine))
        else:
            print(f"[{label}] 콜 {len(cluster)} ≠ 2×{len(turns)}턴 "
                  "— residual 생략")
            residuals = [0.0]
            upstream_sums = [0.0]

        def pct(v, p):
            s = sorted(v)
            return s[min(len(s) - 1, int(round(p / 100 * (len(s) - 1))))]

        rows.append((label, len(turns), sum(call_counts),
                     st.mean(upstream_sums), st.mean(residuals),
                     st.median(residuals), pct(residuals, 90),
                     min(residuals), max(residuals), unmatched))

    hdr = (f"{'label':<18} {'턴':>3} {'콜':>4} {'상류평균':>9} "
           f"{'잔차mean':>9} {'잔차p50':>8} {'잔차p90':>8} "
           f"{'잔차min':>8} {'잔차max':>8} {'미매칭':>6}")
    print(hdr)
    for r in rows:
        print(f"{r[0]:<18} {r[1]:>3} {r[2]:>4} {r[3]:>8.0f}ms "
              f"{r[4]:>8.1f}ms {r[5]:>7.1f}ms {r[6]:>7.1f}ms "
              f"{r[7]:>7.1f}ms {r[8]:>7.1f}ms {r[9]:>6}")

    if len(rows) == 2:
        a, b = rows
        print(f"\n잔차 delta ({b[0]} − {a[0]}): "
              f"mean {b[4] - a[4]:+.1f}ms, p50 {b[5] - a[5]:+.1f}ms "
              f"— 이것이 공급자 분산을 소거한 클라이언트측 순수 격차")


if __name__ == "__main__":
    main()
