#!/usr/bin/env python3
"""Concurrent-load bench for Haystack — two modes.

Haystack's Pipeline.run() is sync-only. For the `async` mode we wrap it
with asyncio.to_thread so the event loop can schedule it the way it
would for a sync tool call wired into an async server — this is how
users deploy Haystack behind an async server. Haystack also ships
AsyncPipeline; this harness does not use it, so the row measures the
sync Pipeline on the loop's default thread pool.

Record schema 2, see burst_common.py.
"""

from __future__ import annotations

import asyncio
import os

from haystack import Pipeline, component

import burst_common as bc

RSS_AFTER_IMPORT_KB = bc.vm_kb("VmRSS")


@component
class Inc:
    @component.output_types(counter=int)
    def run(self, counter: int):
        bc.inject_failure("haystack")
        return {"counter": counter + 1}


def build_seq() -> Pipeline:
    p = Pipeline()
    p.add_component("a", Inc())
    p.add_component("b", Inc())
    p.add_component("c", Inc())
    p.connect("a.counter", "b.counter")
    p.connect("b.counter", "c.counter")
    return p


def check(result) -> None:
    # run() returns the outputs of the leaf component only.
    bc.check_equal("haystack counter", result["c"]["counter"])


# ── async mode ─────────────────────────────────────────────────────────

def run_async(concurrency: int) -> dict:
    graph = build_seq()

    def warm():
        for _ in range(10):
            check(graph.run({"a": {"counter": 0}}))

    failed = bc.guard_warmup("haystack", "asyncio", concurrency, warm, RSS_AFTER_IMPORT_KB)
    if failed:
        return failed
    rss_before = bc.vm_kb("VmRSS")

    async def call():
        return await asyncio.to_thread(graph.run, {"a": {"counter": 0}})

    collector, wall_ns = asyncio.run(bc.burst_async(concurrency, call, check))
    return bc.build_record(
        "haystack", "asyncio", concurrency, collector, wall_ns,
        note="sync Pipeline.run through asyncio.to_thread on the default thread pool (AsyncPipeline not used)",
        rss_after_import_kb=RSS_AFTER_IMPORT_KB, rss_before_burst_kb=rss_before)


# ── multiprocessing mode ───────────────────────────────────────────────

_worker_graph = None


def _mp_init():
    global _worker_graph
    _worker_graph = build_seq()
    check(_worker_graph.run({"a": {"counter": 0}}))  # first-call cost off the clock


def _mp_invoke(_: int) -> tuple:
    return bc.timed_call(lambda: _worker_graph.run({"a": {"counter": 0}}), check)


def run_mp(concurrency: int) -> dict:
    pool_size = os.cpu_count() or 1
    rss_before = bc.vm_kb("VmRSS")
    collector, wall_ns, info = bc.burst_mp(concurrency, pool_size, _mp_invoke, _mp_init)
    return bc.build_record(
        "haystack", f"mp-pool-{pool_size}", concurrency, collector, wall_ns,
        note="sync Pipeline.run in each pool worker; pool started and pipeline built before the clock",
        rss_after_import_kb=RSS_AFTER_IMPORT_KB, rss_before_burst_kb=rss_before, mp_info=info)


def main() -> None:
    mode, concurrency = bc.parse_args("bench_concurrent_haystack.py")
    bc.finish(run_async(concurrency) if mode == "async" else run_mp(concurrency))


if __name__ == "__main__":
    main()
