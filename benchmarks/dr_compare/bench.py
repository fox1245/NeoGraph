"""Bench harness: NeoGraph deep-research vs LangGraph equivalent.

Methodology:
  * Warmup N runs on each side (alternating) so both engines have
    primed: HTTP/WebSocket connections, PG checkpoint pool,
    Crawl4AI keep-alive sockets, Python interpreter caches,
    NeoGraph asio fan-out pool worker threads spun up.
  * Measure N runs on each side, alternating order so any drift
    in network latency / LLM provider load affects both equally.
  * Each iteration uses a fresh thread_id (no checkpoint reuse;
    no "second-call cheaper" advantage).
  * Per-iter: total wall-clock + (best-effort) sub-stage timings
    captured by wrapping the engine call.
  * Report median + p10 + p90 + mean + stdev.

What we're isolating:
  * Same model (DR_MODEL, default gpt-5.4-mini)
  * Same prompts (mirror of example 17)
  * Same Crawl4AI search client (CRAWL4AI_URL)
  * Same Postgres backend
What necessarily differs:
  * NeoGraph: C++ engine, asio thread-pool fan-out, WebSocket Responses
  * LangGraph: Python engine, asyncio fan-out, HTTP via httpx

Both differences are part of what each library ships with, so we
benchmark the libraries as they're meant to be used. Forcing
LangGraph onto WebSocket would be unrealistic; forcing NeoGraph
to HTTP would erase a documented design choice."""

from __future__ import annotations

import argparse
import os
import statistics
import sys
import time
import uuid
from pathlib import Path

# Make sibling modules importable.
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
        print(f"  {name:10s}  (no samples)")
        return
    p10 = _percentile(samples, 10)
    p50 = _percentile(samples, 50)
    p90 = _percentile(samples, 90)
    mean = statistics.fmean(samples)
    sd  = statistics.stdev(samples) if len(samples) > 1 else 0.0
    print(f"  {name:10s}  "
          f"p10 {p10:6.2f}s  p50 {p50:6.2f}s  p90 {p90:6.2f}s  "
          f"mean {mean:6.2f}s  sd {sd:5.2f}s  n={len(samples)}")


def _time_one(label, run_fn, query):
    tid = f"bench-{uuid.uuid4().hex[:10]}"
    t0 = time.perf_counter()
    try:
        out = run_fn(query, tid)
    except Exception as exc:
        elapsed = time.perf_counter() - t0
        print(f"    [{label}] FAILED after {elapsed:.2f}s: {exc}")
        return None, 0
    elapsed = time.perf_counter() - t0
    return elapsed, len(out or "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--query", default="사과에 대해서 조사해줘")
    ap.add_argument("--warmup", type=int, default=2)
    ap.add_argument("--iters",  type=int, default=6)
    ap.add_argument("--only",   choices=["neograph", "langgraph"], default=None)
    args = ap.parse_args()

    print("=" * 70)
    print(f"Deep-research bench: NeoGraph vs LangGraph")
    print(f"  query:  {args.query!r}")
    print(f"  warmup: {args.warmup} per side  ·  iters: {args.iters} per side")
    print(f"  model:  {os.environ.get('DR_MODEL', 'gpt-5.4-mini')}")
    print(f"  search: {os.environ.get('CRAWL4AI_URL', '(none)')}")
    print(f"  pg:     {os.environ.get('NEOGRAPH_PG_DSN', '(none)')[:60]}")
    print("=" * 70)

    # Lazy-import so each module's PROVIDER / pool init only happens
    # for the side we're actually running.
    runners = {}
    if args.only != "langgraph":
        import dr_neograph
        runners["neograph"] = dr_neograph.run_query
    if args.only != "neograph":
        import dr_langgraph
        runners["langgraph"] = dr_langgraph.run_query

    if len(runners) == 0:
        print("Nothing to run."); return 1

    # Warmup — alternating order so both have equally-cold/warm setups.
    print(f"\n[warmup] {args.warmup} per side")
    for i in range(args.warmup):
        for label, fn in runners.items():
            t, n = _time_one(label, fn, args.query)
            print(f"  [{label}] warmup {i+1}/{args.warmup}: "
                  f"{'fail' if t is None else f'{t:6.2f}s'}  ({n} chars)")

    # Measure — alternating order, fresh thread_id each iter.
    print(f"\n[measure] {args.iters} per side, alternating order")
    samples = {label: [] for label in runners}
    for i in range(args.iters):
        for label, fn in runners.items():
            t, n = _time_one(label, fn, args.query)
            if t is not None:
                samples[label].append(t)
            print(f"  [{label}] iter {i+1}/{args.iters}: "
                  f"{'fail' if t is None else f'{t:6.2f}s'}  ({n} chars)")

    # Stats.
    print("\n" + "=" * 70)
    print("Results")
    print("=" * 70)
    for label, xs in samples.items():
        _stats(label, xs)

    if len(samples) == 2 and all(samples.values()):
        ng_med = _percentile(samples["neograph"], 50)
        lg_med = _percentile(samples["langgraph"], 50)
        if ng_med < lg_med:
            print(f"\n  NeoGraph median is {(lg_med-ng_med):.2f}s "
                  f"({(lg_med/ng_med-1)*100:.1f}%) faster than LangGraph.")
        else:
            print(f"\n  LangGraph median is {(ng_med-lg_med):.2f}s "
                  f"({(ng_med/lg_med-1)*100:.1f}%) faster than NeoGraph.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
