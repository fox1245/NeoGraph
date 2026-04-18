#!/usr/bin/env python3
"""Concurrent-load bench for Haystack — two modes.

Haystack's Pipeline.run() is sync-only. For the `async` mode we wrap it
with asyncio.to_thread so the event loop can schedule it the way it
would for a sync tool call wired into a LangGraph-style app — this is
how users actually deploy Haystack behind an async server.

Schema matches bench_concurrent_langgraph.py so the runner / renderer
can concatenate results.
"""

from __future__ import annotations

import asyncio
import json
import multiprocessing as mp
import os
import resource
import sys
import time

from haystack import Pipeline, component


@component
class Inc:
    @component.output_types(counter=int)
    def run(self, counter: int):
        return {"counter": counter + 1}


def build_seq() -> Pipeline:
    p = Pipeline()
    p.add_component("a", Inc())
    p.add_component("b", Inc())
    p.add_component("c", Inc())
    p.connect("a.counter", "b.counter")
    p.connect("b.counter", "c.counter")
    return p


def peak_rss_kb() -> int:
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


def pct(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    values = sorted(values)
    idx = min(len(values) - 1, int(len(values) * p))
    return values[idx]


# ── async mode ─────────────────────────────────────────────────────────

async def _run_async(graph: Pipeline, concurrency: int):
    async def one_call() -> float:
        t0 = time.perf_counter()
        try:
            await asyncio.to_thread(graph.run, {"a": {"counter": 0}})
            return (time.perf_counter() - t0) * 1_000_000.0
        except Exception:
            return -1.0

    tasks = [asyncio.create_task(one_call()) for _ in range(concurrency)]
    results = await asyncio.gather(*tasks, return_exceptions=False)
    ok = [v for v in results if v >= 0]
    return ok, len(ok), concurrency - len(ok)


def run_async(concurrency: int) -> dict:
    graph = build_seq()
    for _ in range(10):
        graph.run({"a": {"counter": 0}})

    t_start = time.perf_counter()
    ok_latencies, ok_count, err_count = asyncio.run(_run_async(graph, concurrency))
    total_wall_ms = int((time.perf_counter() - t_start) * 1000)

    return {
        "engine": "haystack",
        "mode": "asyncio",
        "concurrency": concurrency,
        "total_wall_ms": total_wall_ms,
        "p50_us": int(pct(ok_latencies, 0.50)),
        "p95_us": int(pct(ok_latencies, 0.95)),
        "p99_us": int(pct(ok_latencies, 0.99)),
        "ok": ok_count,
        "err": err_count,
        "peak_rss_kb": peak_rss_kb(),
    }


# ── multiprocessing mode ───────────────────────────────────────────────

_worker_graph = None


def _mp_init():
    global _worker_graph
    _worker_graph = build_seq()


def _mp_invoke(_: int) -> float:
    assert _worker_graph is not None
    t0 = time.perf_counter()
    try:
        _worker_graph.run({"a": {"counter": 0}})
        return (time.perf_counter() - t0) * 1_000_000.0
    except Exception:
        return -1.0


def run_mp(concurrency: int) -> dict:
    pool_size = os.cpu_count() or 1

    t_start = time.perf_counter()
    with mp.Pool(pool_size, initializer=_mp_init) as pool:
        latencies = pool.map(_mp_invoke, range(concurrency))
    total_wall_ms = int((time.perf_counter() - t_start) * 1000)

    ok_latencies = [v for v in latencies if v >= 0]
    return {
        "engine": "haystack",
        "mode": f"mp-pool-{pool_size}",
        "concurrency": concurrency,
        "total_wall_ms": total_wall_ms,
        "p50_us": int(pct(ok_latencies, 0.50)),
        "p95_us": int(pct(ok_latencies, 0.95)),
        "p99_us": int(pct(ok_latencies, 0.99)),
        "ok": len(ok_latencies),
        "err": concurrency - len(ok_latencies),
        "peak_rss_kb": peak_rss_kb(),
    }


def main() -> None:
    if len(sys.argv) < 3:
        print("usage: bench_concurrent_haystack.py {async|mp} <concurrency>",
              file=sys.stderr)
        sys.exit(1)
    mode, concurrency = sys.argv[1], int(sys.argv[2])
    if mode == "async":
        result = run_async(concurrency)
    elif mode == "mp":
        result = run_mp(concurrency)
    else:
        print(f"unknown mode: {mode}", file=sys.stderr)
        sys.exit(1)
    print(json.dumps(result))
    sys.exit(2 if result["err"] > 0 else 0)


if __name__ == "__main__":
    main()
