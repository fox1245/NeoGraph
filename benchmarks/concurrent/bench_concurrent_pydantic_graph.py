#!/usr/bin/env python3
"""Concurrent-load bench for pydantic-graph.

pydantic-graph is async-native — Graph.run() is a coroutine.
Schema matches bench_concurrent_langgraph.py.
"""

from __future__ import annotations

import asyncio
import json
import multiprocessing as mp
import os
import resource
import sys
import time
from dataclasses import dataclass

from pydantic_graph import BaseNode, End, Graph, GraphRunContext


@dataclass
class SeqState:
    counter: int = 0


@dataclass
class SC(BaseNode[SeqState, None, int]):
    async def run(self, ctx: GraphRunContext[SeqState]) -> End[int]:
        ctx.state.counter += 1
        return End(ctx.state.counter)


@dataclass
class SB(BaseNode[SeqState]):
    async def run(self, ctx: GraphRunContext[SeqState]) -> SC:
        ctx.state.counter += 1
        return SC()


@dataclass
class SA(BaseNode[SeqState]):
    async def run(self, ctx: GraphRunContext[SeqState]) -> SB:
        ctx.state.counter += 1
        return SB()


SEQ_GRAPH = Graph(nodes=[SA, SB, SC], state_type=SeqState)


def peak_rss_kb() -> int:
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


def pct(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    values = sorted(values)
    idx = min(len(values) - 1, int(len(values) * p))
    return values[idx]


# ── async mode ─────────────────────────────────────────────────────────

async def _run_async(concurrency: int):
    async def one_call() -> float:
        t0 = time.perf_counter()
        try:
            await SEQ_GRAPH.run(SA(), state=SeqState())
            return (time.perf_counter() - t0) * 1_000_000.0
        except Exception:
            return -1.0

    tasks = [asyncio.create_task(one_call()) for _ in range(concurrency)]
    results = await asyncio.gather(*tasks, return_exceptions=False)
    ok = [v for v in results if v >= 0]
    return ok, len(ok), concurrency - len(ok)


def run_async(concurrency: int) -> dict:
    async def _warm():
        for _ in range(10):
            await SEQ_GRAPH.run(SA(), state=SeqState())
    asyncio.run(_warm())

    t_start = time.perf_counter()
    ok_latencies, ok_count, err_count = asyncio.run(_run_async(concurrency))
    total_wall_ms = int((time.perf_counter() - t_start) * 1000)

    return {
        "engine": "pydantic-graph",
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

def _mp_invoke(_: int) -> float:
    t0 = time.perf_counter()
    try:
        asyncio.run(SEQ_GRAPH.run(SA(), state=SeqState()))
        return (time.perf_counter() - t0) * 1_000_000.0
    except Exception:
        return -1.0


def run_mp(concurrency: int) -> dict:
    pool_size = os.cpu_count() or 1

    t_start = time.perf_counter()
    with mp.Pool(pool_size) as pool:
        latencies = pool.map(_mp_invoke, range(concurrency))
    total_wall_ms = int((time.perf_counter() - t_start) * 1000)

    ok_latencies = [v for v in latencies if v >= 0]
    return {
        "engine": "pydantic-graph",
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
        print("usage: bench_concurrent_pydantic_graph.py {async|mp} <concurrency>",
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
