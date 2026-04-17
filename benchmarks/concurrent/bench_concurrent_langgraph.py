#!/usr/bin/env python3
"""Concurrent-load bench for LangGraph — two modes.

Submits N simultaneous graph invocations and reports throughput, latency
distribution (P50/P95/P99), peak RSS, and failure counts.

Usage:
    python bench_concurrent_langgraph.py <mode> <concurrency>

Modes:
    async  — asyncio.gather(*[graph.ainvoke(state) for _ in range(N)])
             Single event loop, single process. Cheap task switching but
             GIL-bound on CPU work.
    mp     — multiprocessing.Pool(P).map(worker, [state]*N)
             P = CPU count visible to the container. Real parallelism,
             but pay fork + pickle overhead per task.

Output is a single JSON line on stdout in the same schema as the
NeoGraph bench binary, so the runner script can concatenate results.
"""

from __future__ import annotations

import asyncio
import json
import multiprocessing as mp
import os
import resource
import sys
import time
from operator import add
from typing import Annotated, TypedDict

from langgraph.graph import StateGraph, START, END


# ── 3-node sequential counter chain ────────────────────────────────────

class SeqState(TypedDict):
    counter: int


def seq_inc(state: SeqState) -> dict:
    return {"counter": state["counter"] + 1}


def build_seq():
    g = StateGraph(SeqState)
    g.add_node("a", seq_inc)
    g.add_node("b", seq_inc)
    g.add_node("c", seq_inc)
    g.add_edge(START, "a")
    g.add_edge("a", "b")
    g.add_edge("b", "c")
    g.add_edge("c", END)
    return g.compile()


# ── Peak RSS (self-reported, matches NeoGraph side) ───────────────────

def peak_rss_kb() -> int:
    # ru_maxrss is kB on Linux, bytes on macOS. Container targets Linux.
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


# ── Percentile helper ──────────────────────────────────────────────────

def pct(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    values = sorted(values)
    idx = min(len(values) - 1, int(len(values) * p))
    return values[idx]


# ── async mode ─────────────────────────────────────────────────────────

async def _run_async(graph, concurrency: int) -> tuple[list[float], int, int]:
    async def one_call() -> float:
        t0 = time.perf_counter()
        try:
            await graph.ainvoke({"counter": 0})
            return (time.perf_counter() - t0) * 1_000_000.0
        except Exception:
            return -1.0

    tasks = [asyncio.create_task(one_call()) for _ in range(concurrency)]
    results = await asyncio.gather(*tasks, return_exceptions=False)
    ok = [v for v in results if v >= 0]
    return ok, len(ok), concurrency - len(ok)


def run_async(concurrency: int) -> dict:
    graph = build_seq()
    # Warm-up so event loop + channel runtime are live before timing.
    async def _warm():
        for _ in range(10):
            await graph.ainvoke({"counter": 0})
    asyncio.run(_warm())

    t_start = time.perf_counter()
    ok_latencies, ok_count, err_count = asyncio.run(_run_async(graph, concurrency))
    total_wall_ms = int((time.perf_counter() - t_start) * 1000)

    return {
        "engine": "langgraph",
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

# Initialized once per worker process.
_worker_graph = None


def _mp_init():
    global _worker_graph
    _worker_graph = build_seq()


def _mp_invoke(_: int) -> float:
    assert _worker_graph is not None
    t0 = time.perf_counter()
    try:
        _worker_graph.invoke({"counter": 0})
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
    ok_count = len(ok_latencies)
    err_count = concurrency - ok_count

    return {
        "engine": "langgraph",
        "mode": f"mp-pool-{pool_size}",
        "concurrency": concurrency,
        "total_wall_ms": total_wall_ms,
        "p50_us": int(pct(ok_latencies, 0.50)),
        "p95_us": int(pct(ok_latencies, 0.95)),
        "p99_us": int(pct(ok_latencies, 0.99)),
        "ok": ok_count,
        "err": err_count,
        "peak_rss_kb": peak_rss_kb(),
    }


def main() -> None:
    if len(sys.argv) < 3:
        print("usage: bench_concurrent_langgraph.py {async|mp} <concurrency>",
              file=sys.stderr)
        sys.exit(1)
    mode = sys.argv[1]
    concurrency = int(sys.argv[2])

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
