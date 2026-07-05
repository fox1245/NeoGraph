#!/usr/bin/env python3
"""벤치 드라이버 — 대상 프로세스(자비스 C++ 또는 LangGraph 쌍둥이)를 파이프로
구동하며 기동 시간·턴당 왕복 시간·RSS 를 측정한다. 컨테이너 안에서 실행.

프로토콜 (양쪽 동일):
  기동 완료  : stdout 에 "온라인" 포함 라인
  턴 완료    : stdout 에 "[jarvis:tts]" 로 시작하는 라인 (응답 텍스트 포함)

사용:
  python3 bench/driver.py --cmd "exec bash scripts/run_jarvis.sh config-bench" \
      --turns bench/turns_mock.txt --out /out/neograph_mock.jsonl --label neograph-mock
"""
import argparse
import json
import os
import queue
import subprocess
import sys
import threading
import time


def read_rss_kb(pid: int) -> int:
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmd", required=True, help="셸 커맨드 (exec 권장 — RSS 측정 대상)")
    ap.add_argument("--turns", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--label", required=True)
    ap.add_argument("--delay", type=float, default=0.0, help="턴 간 대기(초) — API 레이트리밋용")
    ap.add_argument("--turn-timeout", type=float, default=120.0)
    ap.add_argument("--ready-timeout", type=float, default=300.0)
    args = ap.parse_args()

    with open(args.turns, encoding="utf-8") as f:
        turns = [ln.rstrip("\n") for ln in f if ln.strip()]

    stderr_log = open(args.out + ".stderr.log", "w")
    t_spawn = time.monotonic()
    proc = subprocess.Popen(args.cmd, shell=True,
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=stderr_log, text=True, bufsize=1)

    lines: "queue.Queue[str]" = queue.Queue()

    def pump() -> None:
        assert proc.stdout is not None
        for line in proc.stdout:
            lines.put(line)
        lines.put("")  # EOF 마커

    threading.Thread(target=pump, daemon=True).start()

    def wait_for(pred, timeout: float):
        deadline = time.monotonic() + timeout
        while True:
            remain = deadline - time.monotonic()
            if remain <= 0:
                return None
            try:
                line = lines.get(timeout=remain)
            except queue.Empty:
                return None
            if line == "":
                return None
            if pred(line):
                return line

    # ── 기동 대기 ──
    if wait_for(lambda l: "온라인" in l, args.ready_timeout) is None:
        print(f"[driver] FATAL: ready marker 미수신 ({args.label})", file=sys.stderr)
        proc.kill()
        sys.exit(1)
    startup_ms = (time.monotonic() - t_spawn) * 1000.0
    rss_ready = read_rss_kb(proc.pid)

    # ── 턴 루프 ──
    records = []
    max_rss = rss_ready
    for i, text in enumerate(turns):
        t0 = time.monotonic()
        assert proc.stdin is not None
        proc.stdin.write(text + "\n")
        proc.stdin.flush()
        line = wait_for(lambda l: l.startswith("[jarvis:tts]"), args.turn_timeout)
        ms = (time.monotonic() - t0) * 1000.0
        if line is None:
            print(f"[driver] turn {i} timeout", file=sys.stderr)
            records.append({"i": i, "ms": None, "reply": None})
            break
        records.append({"i": i, "ms": round(ms, 3),
                        "reply": line.rstrip("\n")[:120]})
        max_rss = max(max_rss, read_rss_kb(proc.pid))
        if args.delay:
            time.sleep(args.delay)

    try:
        proc.stdin.close()
    except OSError:
        pass
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()

    ok = sorted(r["ms"] for r in records if r["ms"] is not None)

    def pct(p: float) -> float:
        if not ok:
            return 0.0
        k = min(len(ok) - 1, max(0, int(round(p / 100.0 * (len(ok) - 1)))))
        return ok[k]

    summary = {
        "label": args.label, "turns_ok": len(ok), "turns_total": len(turns),
        "startup_ms": round(startup_ms, 1),
        "rss_ready_kb": rss_ready, "rss_max_kb": max_rss,
        "mean_ms": round(sum(ok) / len(ok), 3) if ok else 0.0,
        "p50_ms": round(pct(50), 3), "p90_ms": round(pct(90), 3),
        "p99_ms": round(pct(99), 3),
        "min_ms": round(ok[0], 3) if ok else 0.0,
        "max_ms": round(ok[-1], 3) if ok else 0.0,
    }
    with open(args.out, "w", encoding="utf-8") as f:
        for r in records:
            f.write(json.dumps({"label": args.label, **r},
                               ensure_ascii=False) + "\n")
        f.write(json.dumps({"summary": summary}, ensure_ascii=False) + "\n")
    print("SUMMARY " + json.dumps(summary, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
