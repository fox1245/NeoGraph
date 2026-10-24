#!/usr/bin/env python3
"""Benchmark driver — drives a target process (JARVIS C++ or LangGraph twin) over
pipes and measures startup time, per-turn round-trip latency, and RSS inside a container.

Protocol (identical on both sides):
  Startup ready : stdout line containing the Korean "online" marker (see wait_for below)
  Turn complete : stdout line starting with "[jarvis:tts]" (includes reply text)

Usage:
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


def tts_payload(line: str) -> str:
    prefix = "[jarvis:tts]"
    if not line.startswith(prefix):
        return ""
    payload = line[len(prefix):].strip()
    if payload.startswith("[") and "]" in payload:
        payload = payload.split("]", 1)[1].strip()
    return payload


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmd", required=True, help="셸 커맨드 (exec 권장 — RSS 측정 대상)")
    ap.add_argument("--turns", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--label", required=True)
    ap.add_argument("--delay", type=float, default=0.0, help="턴 간 대기(초) — API 레이트리밋용")
    ap.add_argument("--turn-timeout", type=float, default=120.0)
    ap.add_argument("--ready-timeout", type=float, default=300.0)
    ap.add_argument("--max-turns", type=int, help="Use only this many actual input turns")
    args = ap.parse_args()
    if args.max_turns is not None and args.max_turns < 1:
        ap.error("--max-turns must be positive")

    with open(args.turns, encoding="utf-8") as f:
        turns = [ln.rstrip("\n") for ln in f if ln.strip()]
    if args.max_turns is not None:
        turns = turns[:args.max_turns]
    if not turns:
        ap.error("--turns contains no nonempty input turns")

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
        lines.put("")  # EOF marker

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

    # ── Wait for startup ──
    if wait_for(lambda l: "온라인" in l, args.ready_timeout) is None:
        print(f"[driver] FATAL: ready marker 미수신 ({args.label})", file=sys.stderr)
        proc.kill()
        sys.exit(1)
    startup_ms = (time.monotonic() - t_spawn) * 1000.0
    rss_ready = read_rss_kb(proc.pid)

    # ── Turn loop ──
    records = []
    max_rss = rss_ready
    # Derive absolute timestamps from a monotonic anchor — immune to WSL2/NTP wall-clock steps
    # (nginx $msec uses wall-clock time and can drift on a step, so the analyzer prefers
    #  order-based matching and uses time windows only as a fallback)
    wall_anchor = time.time() - time.monotonic()
    for i, text in enumerate(turns):
        t0 = time.monotonic()
        t0_epoch = wall_anchor + t0     # absolute timestamp for matching against proxy log ($msec)
        assert proc.stdin is not None
        proc.stdin.write(text + "\n")
        proc.stdin.flush()
        # Markers arrive in order: [jarvis:ttft] (first synth token) → [jarvis:tts] (turn complete).
        # Scan both in one pass to capture ttft_ms and total ms.
        ttft_ms = None

        def _seen(l):
            nonlocal ttft_ms
            if ttft_ms is None and l.startswith("[jarvis:ttft]"):
                ttft_ms = (time.monotonic() - t0) * 1000.0
            return l.startswith("[jarvis:tts]")

        line = wait_for(_seen, args.turn_timeout)
        ms = (time.monotonic() - t0) * 1000.0
        if line is None:
            print(f"[driver] turn {i} timeout", file=sys.stderr)
            records.append({"i": i, "ms": None, "reply": None})
            break
        if not tts_payload(line):
            print(f"[driver] turn {i} empty TTS result", file=sys.stderr)
            records.append({"i": i, "ms": None, "reply": None})
            break
        records.append({"i": i, "ms": round(ms, 3),
                        "ttft_ms": round(ttft_ms, 3) if ttft_ms is not None else None,
                        "t0": round(t0_epoch, 3),
                        "t1": round(wall_anchor + time.monotonic(), 3),
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
        proc.wait()
    stderr_log.close()

    ok = sorted(r["ms"] for r in records if r["ms"] is not None)

    def pct(p: float) -> float:
        if not ok:
            return 0.0
        k = min(len(ok) - 1, max(0, int(round(p / 100.0 * (len(ok) - 1)))))
        return ok[k]

    tt = sorted(r["ttft_ms"] for r in records if r.get("ttft_ms") is not None)

    def pct_of(v, p):
        if not v:
            return 0.0
        return v[min(len(v) - 1, max(0, int(round(p / 100.0 * (len(v) - 1)))))]

    summary = {
        "label": args.label, "turns_ok": len(ok), "turns_total": len(turns),
        "startup_ms": round(startup_ms, 1),
        "rss_ready_kb": rss_ready, "rss_max_kb": max_rss,
        "mean_ms": round(sum(ok) / len(ok), 3) if ok else 0.0,
        "p50_ms": round(pct(50), 3), "p90_ms": round(pct(90), 3),
        "p99_ms": round(pct(99), 3),
        "min_ms": round(ok[0], 3) if ok else 0.0,
        "max_ms": round(ok[-1], 3) if ok else 0.0,
        "ttft_n": len(tt),
        "ttft_mean_ms": round(sum(tt) / len(tt), 3) if tt else None,
        "ttft_p50_ms": round(pct_of(tt, 50), 3) if tt else None,
        "ttft_p90_ms": round(pct_of(tt, 90), 3) if tt else None,
    }
    with open(args.out, "w", encoding="utf-8") as f:
        for r in records:
            f.write(json.dumps({"label": args.label, **r},
                               ensure_ascii=False) + "\n")
        f.write(json.dumps({"summary": summary}, ensure_ascii=False) + "\n")
    print("SUMMARY " + json.dumps(summary, ensure_ascii=False), flush=True)
    if len(ok) != len(turns) or proc.returncode != 0:
        print(f"[driver] FATAL: incomplete or failed run ({args.label}): "
              f"{len(ok)}/{len(turns)} turns, child exit {proc.returncode}",
              file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
