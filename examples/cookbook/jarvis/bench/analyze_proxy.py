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


def split_rounds(calls, n_rounds):
    """콜 스트림을 라운드 수만큼 분할 — 가장 큰 (n-1)개 시간 공백을 경계로.

    라운드 간 공백(컨테이너 교체 수 초)이 턴 간 공백(delay+라우터)보다 항상
    크다는 성질만 사용 — 절대 임계값 불요, 벽시계 스텝에도 안전."""
    calls = sorted(calls)
    if n_rounds == 1:
        return [calls]
    gaps = sorted(range(len(calls) - 1),
                  key=lambda i: calls[i + 1][0] - calls[i][0],
                  reverse=True)[: n_rounds - 1]
    clusters, prev = [], 0
    for b in sorted(gaps):
        clusters.append(calls[prev: b + 1])
        prev = b + 1
    clusters.append(calls[prev:])
    return clusters


def main():
    log_path = sys.argv[1]
    calls = parse_log(log_path)
    print(f"프록시 로그 콜 수: {len(calls)}\n")

    # 순서 기반 매칭 — 턴은 순차 실행이고 콜은 턴 안에서 완료되므로
    # 로그 순서 = 턴 순서. 벽시계 스텝(WSL2/NTP)에 면역.
    # 각 라운드 콜 수가 정확히 2×턴수일 때만 사용, 아니면 시간창 폴백.
    specs = [s.split(":", 1) for s in sys.argv[2:]]
    clusters = split_rounds(calls, len(specs))

    rows = []
    for (label, jsonl), cluster in zip(specs, clusters):
        turns = []
        for line in open(jsonl, encoding="utf-8"):
            o = json.loads(line)
            if "summary" in o or o.get("ms") is None or "t0" not in o:
                continue
            turns.append(o)

        residuals, upstream_sums, call_counts, unmatched = [], [], [], 0
        if len(cluster) == 2 * len(turns):
            print(f"[{label}] 순서 기반 매칭 (콜 {len(cluster)} = 2×{len(turns)}턴)")
            for k, t in enumerate(turns):
                mine = cluster[2 * k: 2 * k + 2]
                up_ms = sum(up for _, up, _ in mine) * 1000.0
                residuals.append(t["ms"] - up_ms)
                upstream_sums.append(up_ms)
                call_counts.append(len(mine))
        else:
            print(f"[{label}] 콜 수 불일치({len(cluster)} vs 2×{len(turns)}) "
                  "— 시간창 매칭 폴백 (벽시계 스텝 시 부정확)")
            for t in turns:
                mine = [(ts, up) for ts, up, status in cluster
                        if t["t0"] - 0.05 <= ts <= t["t1"] + 0.05]
                up_ms = sum(up for _, up in mine) * 1000.0
                residuals.append(t["ms"] - up_ms)
                upstream_sums.append(up_ms)
                call_counts.append(len(mine))
                if not mine:
                    unmatched += 1

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
