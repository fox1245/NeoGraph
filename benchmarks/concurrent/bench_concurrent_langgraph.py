#!/usr/bin/env python3
"""Concurrent-load bench for LangGraph — two modes.

Submits N simultaneous runs of the 3-node counter chain and reports the wall
time of the burst, service and response times, peak RSS and failure counts
(record schema 2, see burst_common.py).

Usage:
    python bench_concurrent_langgraph.py <mode> <concurrency>

Modes:
    async  — asyncio.gather(*[graph.ainvoke(state) for _ in range(N)])
             Single event loop, single process. The node is a plain `def`, so
             LangGraph hops each node to the loop's default thread pool.
    mp     — multiprocessing.Pool(P).map(worker, [state]*N)
             P = CPU count visible to the container. Real parallelism,
             but pay fork + pickle overhead per task. The pool is started and
             every worker has built its graph before the clock starts.
"""

from __future__ import annotations

import asyncio
import os
from typing import TypedDict

from langgraph.graph import StateGraph, START, END

import burst_common as bc

RSS_AFTER_IMPORT_KB = bc.vm_kb("VmRSS")


# ── 3-node sequential counter chain ────────────────────────────────────

class SeqState(TypedDict):
    counter: int


def seq_inc(state: SeqState) -> dict:
    bc.inject_failure("langgraph")
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


def check(result) -> None:
    bc.check_equal("langgraph counter", result["counter"])


# ── async mode ─────────────────────────────────────────────────────────

def run_async(concurrency: int) -> dict:
    graph = build_seq()

    # Warm-up so the event loop and the channel runtime are live before timing.
    async def _warm():
        for _ in range(10):
            check(await graph.ainvoke({"counter": 0}))
    failed = bc.guard_warmup("langgraph", "asyncio", concurrency,
                             lambda: asyncio.run(_warm()), RSS_AFTER_IMPORT_KB)
    if failed:
        return failed
    rss_before = bc.vm_kb("VmRSS")

    collector, wall_ns = asyncio.run(
        bc.burst_async(concurrency, lambda: graph.ainvoke({"counter": 0}), check))
    return bc.build_record(
        "langgraph", "asyncio", concurrency, collector, wall_ns,
        note="ainvoke of a sync def node: every node hops to the loop's default thread pool",
        rss_after_import_kb=RSS_AFTER_IMPORT_KB, rss_before_burst_kb=rss_before)


# ── multiprocessing mode ───────────────────────────────────────────────

# Initialized once per worker process.
_worker_graph = None


def _mp_init():
    global _worker_graph
    _worker_graph = build_seq()
    check(_worker_graph.invoke({"counter": 0}))  # first-call cost off the clock


def _mp_invoke(_: int) -> tuple:
    return bc.timed_call(lambda: _worker_graph.invoke({"counter": 0}), check)


def run_mp(concurrency: int) -> dict:
    pool_size = os.cpu_count() or 1
    rss_before = bc.vm_kb("VmRSS")
    collector, wall_ns, info = bc.burst_mp(concurrency, pool_size, _mp_invoke, _mp_init)
    return bc.build_record(
        "langgraph", f"mp-pool-{pool_size}", concurrency, collector, wall_ns,
        note="sync invoke in each pool worker; pool started and graph built before the clock",
        rss_after_import_kb=RSS_AFTER_IMPORT_KB, rss_before_burst_kb=rss_before, mp_info=info)


def main() -> None:
    mode, concurrency = bc.parse_args("bench_concurrent_langgraph.py")
    bc.finish(run_async(concurrency) if mode == "async" else run_mp(concurrency))


if __name__ == "__main__":
    main()
