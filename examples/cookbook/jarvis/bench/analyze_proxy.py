#!/usr/bin/env python3
"""경계 계측 잔차 분석 — 프록시 로그의 콜별 상류(WAN+Groq) 시간을 턴에서 차감.

  잔차 = 턴 왕복시간 − Σ(그 턴 시간창 안에 완료된 콜의 upstream_response_time)
       = 클라이언트측 순수 시간 (그래프 + HTTP 클라이언트 직렬화 + 로컬 MCP + 파이프)

콜↔턴 매핑: driver.py 가 기록한 턴별 절대시각 창 [t0, t1] 에 프록시 로그의
완료시각($msec)이 들어가는 콜을 그 턴의 것으로 귀속. 여러 런이 한 로그를
공유해도 시간창이 겹치지 않으므로 자동 분리된다.

사용:
  python3 analyze_proxy.py <groq.log> <label1>:<jsonl1> [<label2>:<jsonl2> ...]
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
            # 재시도 시 "0.1, 0.2" 형태 — 합산
            up = sum(float(x) for x in upstream.replace(",", " ").split()
                     if x not in ("-",))
            calls.append((ts, up, parts[5]))
        except ValueError:
            continue
    return calls


def main():
    # 라운드마다 독립 로그(label:jsonl:log) — 라운드 분할·크로스컨테이너
    # 시간창 매칭 불요. 콜이 정확히 2×턴수면 순서 기반, 아니면 residual 생략.
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
