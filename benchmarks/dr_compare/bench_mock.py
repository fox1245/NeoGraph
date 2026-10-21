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
import json
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


# Every failure of this process, as (side, message); the first one goes into the --json output.
_ERRORS: list = []


def _time_one(label, run_fn, query):
    tid = f"mb-{uuid.uuid4().hex[:10]}"
    t0 = time.perf_counter()
    try:
        out = run_fn(query, tid)
        if not out or not out.strip():
            raise RuntimeError("Runner returned no visible report")
    except Exception as exc:
        print(f"    [{label}] FAILED: {exc}")
        _ERRORS.append((label, f"{type(exc).__name__}: {exc}"))
        return None, 0
    return time.perf_counter() - t0, len(out or "")


def _widths(environ):
    """Pool width of each side as the runners read it from the environment.

    NeoGraph: NG_WORKER_COUNT (default 4). LangGraph: LG_MAX_CONCURRENCY, which defaults to the NeoGraph value so
    that the two sides are matched unless a run says otherwise."""
    ng = int(environ.get("NG_WORKER_COUNT", "4"))
    lg = int(environ.get("LG_MAX_CONCURRENCY", environ.get("NG_WORKER_COUNT", "4")))
    return ng, lg


_KNOBS = ("LLM_MOCK_MS", "FANOUT", "MOCK_SEARCH", "USE_INMEMORY_CP", "NG_WORKER_COUNT", "LG_MAX_CONCURRENCY")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--query", default="사과에 대해서 조사해줘")
    ap.add_argument("--warmup", type=int, default=5)
    ap.add_argument("--iters",  type=int, default=50)
    ap.add_argument("--only",   choices=["neograph", "langgraph"], default=None)
    ap.add_argument("--allow-unmatched-widths", action="store_true",
                    help="run even when NeoGraph's worker count and LangGraph's max_concurrency differ")
    ap.add_argument("--json", metavar="PATH", default=None,
                    help="write the raw per-iteration samples, failures, widths and env knobs as JSON")
    args = ap.parse_args()
    _ERRORS.clear()
    if args.warmup < 0 or args.iters < 1:
        ap.error("--warmup must be nonnegative and --iters must be positive")
    # This entry point must never select a hosted provider, even when a key or
    # a real-mode environment has been inherited from another benchmark.
    try:
        mock_ms = int(os.environ.get("LLM_MOCK_MS", "0"))
    except ValueError:
        ap.error("LLM_MOCK_MS must be a nonnegative integer")
    if mock_ms < 0:
        ap.error("bench_mock.py forbids real-provider mode; use bench.py explicitly")
    os.environ["LLM_MOCK_MS"] = str(mock_ms)
    os.environ["MOCK_SEARCH"] = "1"
    os.environ["USE_INMEMORY_CP"] = "1"
    try:
        ng_width, lg_width = _widths(os.environ)
    except ValueError:
        ap.error("NG_WORKER_COUNT and LG_MAX_CONCURRENCY must be positive integers")
    if ng_width < 1 or lg_width < 1:
        ap.error("NG_WORKER_COUNT and LG_MAX_CONCURRENCY must be positive integers")
    if args.only is None and ng_width != lg_width and not args.allow_unmatched_widths:
        ap.error(f"pool widths differ: NeoGraph {ng_width} workers, LangGraph max_concurrency {lg_width}; "
                 "set both to the same value or pass --allow-unmatched-widths")
    samples = {}
    failures = 0

    def finish(code):
        if args.json:
            report = {
                "schema": 1,
                "kind": "dr_compare_mock",
                "query": args.query,
                "warmup": args.warmup,
                "iters": args.iters,
                "only": args.only,
                "widths": {"neograph": ng_width, "langgraph": lg_width},
                "widths_matched": ng_width == lg_width,
                "env": {k: os.environ.get(k) for k in _KNOBS},
                "samples_s": samples,
                "failures": max(failures, len(_ERRORS)),
                "errors": [{"side": side, "message": message[:400]} for side, message in _ERRORS],
                "exit": code,
            }
            with open(args.json, "w", encoding="utf-8") as handle:
                json.dump(report, handle, indent=1)
                handle.write("\n")
        return code

    print("=" * 78)
    print("Engine-throughput bench (mocked LLM + search)")
    print(f"  query:        {args.query!r}")
    print(f"  warmup:       {args.warmup} per side")
    print(f"  iters:        {args.iters} per side")
    print(f"  LLM_MOCK_MS:  {os.environ.get('LLM_MOCK_MS', '0')}")
    print(f"  MOCK_SEARCH:  {os.environ.get('MOCK_SEARCH', '0')}")
    print(f"  FANOUT:       {os.environ.get('FANOUT', '5')}")
    print(f"  USE_INMEMORY: {os.environ.get('USE_INMEMORY_CP', '0')}")
    print(f"  pool width:   NeoGraph worker_count={ng_width}, "
          f"LangGraph max_concurrency={lg_width}"
          f"{'' if ng_width == lg_width else '  (UNMATCHED)'}")
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
        if dr_neograph.LLM_MOCK_MS < 0 or not dr_neograph.MOCK_SEARCH:
            ap.error("NeoGraph runner was imported in non-mock mode; start a fresh process")
        runners["neograph"] = dr_neograph.run_query
    if args.only != "neograph":
        import dr_langgraph
        if dr_langgraph.LLM_MOCK_MS < 0 or not dr_langgraph.MOCK_SEARCH:
            ap.error("LangGraph runner was imported in non-mock mode; start a fresh process")
        runners["langgraph"] = dr_langgraph.run_query
    # The modules read their widths at import; they must agree with what was checked above.
    for module, attr, expected in (("dr_neograph", "NG_WORKER_COUNT", ng_width),
                                   ("dr_langgraph", "LG_MAX_CONCURRENCY", lg_width)):
        loaded = sys.modules.get(module)
        actual = getattr(loaded, attr, expected)
        if loaded is not None and actual != expected:
            ap.error(f"{module}.{attr} is {actual}, but the environment says {expected}; start a fresh process")
    print(f"  done in {(time.perf_counter()-t0)*1000:.1f}ms")

    if not runners:
        print("Nothing to run."); return finish(1)

    # Warmup — same single process, alternating order.
    print(f"\n[warmup] {args.warmup} per side")
    for i in range(args.warmup):
        for label, fn in runners.items():
            t, _ = _time_one(label, fn, args.query)
            if t is None:
                print(f"  [{label}] warmup {i+1}: FAILED — abort")
                failures += 1
                return finish(1)
    # Don't print warmup details (they're noisy); we just need the
    # first-call paths warm (compile caches, checkpoint pool, ...).

    # Measure.
    print(f"\n[measure] {args.iters} per side, alternating order")
    samples = {label: [] for label in runners}
    failures = 0
    for i in range(args.iters):
        for label, fn in runners.items():
            t, _ = _time_one(label, fn, args.query)
            failures += t is None
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
    print(f"\n  Failed runs: {failures}")
    return finish(1 if failures else 0)


if __name__ == "__main__":
    raise SystemExit(main())
