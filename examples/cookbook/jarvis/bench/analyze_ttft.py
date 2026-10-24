#!/usr/bin/env python3
"""TTFT analysis — for streaming benchmark rounds.

Reports two metrics:
  1. Client-observed TTFT: driver-measured turn-send → first synthesized token.
     The moment the user starts hearing the answer. Independent of reply length
     (excludes generation duration), making raw comparisons fairer than total time.
  2. Framework-residual TTFT: client TTFT − (full router upstream + synth upstream first byte).
     = graph + client serialization + local hops, canceling provider prefill/queue variance.

Proxy log fields: $msec $request_time $connect $header_time $response_time $status
Call pair = [router (non-streaming: header≈response), synth (streaming: header=first byte)].

Usage: python3 analyze_ttft.py <label:jsonl:log> [<label:jsonl:log> ...]
       (separate log per round — no round splitting needed)
"""
import json
import statistics as st
import sys


def parse_log(path):
    out = []
    for line in open(path, encoding="utf-8"):
        p = line.split()
        if len(p) < 6:
            continue
        try:
            ts = float(p[0])
            hdr = sum(float(x) for x in p[3].replace(",", " ").split() if x != "-")
            rsp = sum(float(x) for x in p[4].replace(",", " ").split() if x != "-")
            out.append((ts, hdr, rsp))
        except ValueError:
            continue
    return sorted(out)


def pct(v, p):
    s = sorted(v)
    return s[min(len(s) - 1, max(0, int(round(p / 100 * (len(s) - 1)))))]


def main():
    specs = [s.split(":") for s in sys.argv[1:]]

    rows = []
    for label, jsonl, log in specs:
        cluster = parse_log(log)
        turns = [json.loads(l) for l in open(jsonl, encoding="utf-8")]
        turns = [t for t in turns
                 if "summary" not in t and t.get("ttft_ms") is not None]
        client_ttft = [t["ttft_ms"] for t in turns]

        resid = None
        if len(cluster) == 2 * len(turns):
            resid = []
            for k, t in enumerate(turns):
                pair = cluster[2 * k: 2 * k + 2]
                # Distinguish router vs. synth within the pair by response time
                # rather than completion timestamp (msec) — router (short JSON) <
                # synth (longer streamed reply). Safe even if log completion order flips.
                router, synth = sorted(pair, key=lambda c: c[2])
                # Upstream TTFT component = full router call (non-streaming) + synth first byte (header)
                resid.append(t["ttft_ms"] - (router[2] + synth[1]) * 1000.0)
        else:
            print(f"[{label}] 콜 {len(cluster)} ≠ 2×{len(turns)}턴 "
                  "— residual 생략(체감 TTFT 만)")
        rows.append((label, len(turns),
                     st.mean(client_ttft), pct(client_ttft, 50), pct(client_ttft, 90),
                     (st.mean(resid) if resid else None),
                     (pct(resid, 50) if resid else None)))

    print(f"{'label':<18} {'턴':>3} {'체감TTFT mean':>13} {'p50':>7} {'p90':>7} "
          f"{'FW잔차 mean':>11} {'p50':>7}")
    for r in rows:
        fw = f"{r[5]:>9.1f}ms {r[6]:>6.1f}ms" if r[5] is not None else f"{'n/a':>18}"
        print(f"{r[0]:<18} {r[1]:>3} {r[2]:>11.0f}ms {r[3]:>6.0f} {r[4]:>6.0f}  {fw}")

    if len(rows) == 2:
        a, b = rows
        print(f"\n체감 TTFT delta ({b[0]} − {a[0]}): "
              f"p50 {b[3] - a[3]:+.0f}ms (제품 체감 — 공급자 분산 포함)")
        if a[6] is not None and b[6] is not None:
            print(f"프레임워크-잔차 TTFT delta: p50 {b[6] - a[6]:+.1f}ms "
                  f"(분산 소거 — 순수 프레임워크)")


if __name__ == "__main__":
    main()
