"""Engine-throughput bench: NeoGraph vs LangGraph with mocked LLM + mocked search.

Why: the live-LLM bench was dominated by ~30s of OpenAI latency per run
(LLM = 99% of wall-clock). Engine-overhead differences in the µs-ms
range can't surface. This bench replaces the LLM with a deterministic
sleep, removing the variance + the dominant cost so the engine
overhead becomes measurable.

Three knobs:
  LLM_MOCK_MS — sleep per LLM call (default 0 — pure engine).
  MOCK_SEARCH=1 — skip Crawl4AI (use canned evidence).
  FANOUT — number of researcher Sends per query (default 5).

Both impls are imported ONCE at process start (so module init cost is
paid once, not per-iteration). The compiled engine + checkpointer are
held in module-globals and reused across iterations. This isolates the
*runtime* engine overhead from one-time C++ shared-lib load and Python
package import — which the wire-level analysis identified as the
~4-5s startup penalty NeoGraph paid in single-shot bench runs."""

from __future__ import annotations

import argparse
import os
import statistics
import sys
import time
import uuid
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))


def _percentile(xs, p):
    if not xs: return float("nan")
    s = sorted(xs)
    k = (len(s) - 1) * (p / 100.0)
    f, c = int(k), min(int(k) + 1, len(s) - 1)
    return s[f] + (s[c] - s[f]) * (k - f)


def _stats(name, samples):
    if not samples:
        print(f"  {name:10s}  (no samples)"); return None
    p10 = _percentile(samples, 10)
    p50 = _percentile(samples, 50)
    p90 = _percentile(samples, 90)
    mean = statistics.fmean(samples)
    sd  = statistics.stdev(samples) if len(samples) > 1 else 0.0
    print(f"  {name:10s}  "
          f"p10 {p10*1000:7.1f}ms  p50 {p50*1000:7.1f}ms  "
          f"p90 {p90*1000:7.1f}ms  mean {mean*1000:7.1f}ms  "
          f"sd {sd*1000:5.1f}ms  n={len(samples)}")
    return p50


def _time_one(label, run_fn, query):
    tid = f"mb-{uuid.uuid4().hex[:10]}"
    t0 = time.perf_counter()
    try:
        out = run_fn(query, tid)
    except Exception as exc:
        print(f"    [{label}] FAILED: {exc}")
        return None, 0
    return time.perf_counter() - t0, len(out or "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--query", default="사과에 대해서 조사해줘")
    ap.add_argument("--warmup", type=int, default=5)
    ap.add_argument("--iters",  type=int, default=50)
    ap.add_argument("--only",   choices=["neograph", "langgraph"], default=None)
    args = ap.parse_args()

    print("=" * 78)
    print("Engine-throughput bench (mocked LLM + search)")
    print(f"  query:        {args.query!r}")
    print(f"  warmup:       {args.warmup} per side")
    print(f"  iters:        {args.iters} per side")
    print(f"  LLM_MOCK_MS:  {os.environ.get('LLM_MOCK_MS', '0')}")
    print(f"  MOCK_SEARCH:  {os.environ.get('MOCK_SEARCH', '0')}")
    print(f"  FANOUT:       {os.environ.get('FANOUT', '5')}")
    print(f"  USE_INMEMORY: {os.environ.get('USE_INMEMORY_CP', '0')}")
    print("=" * 78)

    # PRE-LOAD both modules — this triggers C++ shared-lib load,
    # graph compile, channel init, etc. Each ng/lg module exposes a
    # module-global compiled engine; subsequent run_query() calls
    # reuse it. So the next-50-iters loop measures pure runtime.
    print("\n[preload] importing modules + compiling graphs ...")
    t0 = time.perf_counter()
    runners = {}
    if args.only != "langgraph":
        import dr_neograph
        runners["neograph"] = dr_neograph.run_query
    if args.only != "neograph":
        import dr_langgraph
        runners["langgraph"] = dr_langgraph.run_query
    print(f"  done in {(time.perf_counter()-t0)*1000:.1f}ms")

    if not runners:
        print("Nothing to run."); return 1

    # Warmup — same single process, alternating order.
    print(f"\n[warmup] {args.warmup} per side")
    for i in range(args.warmup):
        for label, fn in runners.items():
            t, _ = _time_one(label, fn, args.query)
            if t is None:
                print(f"  [{label}] warmup {i+1}: FAILED — abort"); return 1
    # Don't print warmup details (they're noisy); we just need the
    # first-call paths warm (compile caches, checkpoint pool, ...).

    # Measure.
    print(f"\n[measure] {args.iters} per side, alternating order")
    samples = {label: [] for label in runners}
    for i in range(args.iters):
        for label, fn in runners.items():
            t, _ = _time_one(label, fn, args.query)
            if t is not None:
                samples[label].append(t)
        if (i + 1) % 10 == 0:
            print(f"  ... iter {i+1}/{args.iters}")

    # Stats.
    print("\n" + "=" * 78)
    print("Results (per-iteration wall-clock)")
    print("=" * 78)
    medians = {}
    for label, xs in samples.items():
        medians[label] = _stats(label, xs)

    if len(medians) == 2 and all(medians.values()):
        ng = medians["neograph"]; lg = medians["langgraph"]
        if ng < lg:
            print(f"\n  NeoGraph median {(lg-ng)*1000:.2f}ms "
                  f"({(lg/ng-1)*100:.1f}%) faster than LangGraph.")
        else:
            print(f"\n  LangGraph median {(ng-lg)*1000:.2f}ms "
                  f"({(ng/lg-1)*100:.1f}%) faster than NeoGraph.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
