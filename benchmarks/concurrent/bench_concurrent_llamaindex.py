#!/usr/bin/env python3
"""Concurrent-load bench for LlamaIndex Workflow.

LlamaIndex Workflow is async-native. Workflow.run() is a plain method that
returns a WorkflowHandler (an awaitable, not a coroutine) and schedules the
run with asyncio.create_task, so it must be called inside a running event
loop. Both modes therefore create and await the handler inside a coroutine.

Each concurrent call constructs a fresh SeqFlow. Whether one instance can be
reused across runs is measured separately by the engine-overhead comparison
(benchmarks/compare), not here.

Record schema 2, see burst_common.py.
"""

from __future__ import annotations

import asyncio
import os

from llama_index.core.workflow import (
    Event,
    StartEvent,
    StopEvent,
    Workflow,
    step,
)

import burst_common as bc

RSS_AFTER_IMPORT_KB = bc.vm_kb("VmRSS")


class AEv(Event):
    counter: int


class BEv(Event):
    counter: int


class SeqFlow(Workflow):
    @step
    async def a(self, ev: StartEvent) -> AEv:
        bc.inject_failure("llamaindex")
        return AEv(counter=ev.counter + 1)

    @step
    async def b(self, ev: AEv) -> BEv:
        return BEv(counter=ev.counter + 1)

    @step
    async def c(self, ev: BEv) -> StopEvent:
        return StopEvent(result=ev.counter + 1)


def check(result) -> None:
    bc.check_equal("llamaindex result", result)


async def run_chain():
    # The handler is created inside the running loop and awaited there.
    return await SeqFlow(timeout=60).run(counter=0)


# ── async mode ─────────────────────────────────────────────────────────

def run_async(concurrency: int) -> dict:
    async def _warm():
        for _ in range(10):
            check(await run_chain())
    failed = bc.guard_warmup("llamaindex", "asyncio", concurrency,
                             lambda: asyncio.run(_warm()), RSS_AFTER_IMPORT_KB)
    if failed:
        return failed
    rss_before = bc.vm_kb("VmRSS")

    collector, wall_ns = asyncio.run(bc.burst_async(concurrency, run_chain, check))
    return bc.build_record(
        "llamaindex", "asyncio", concurrency, collector, wall_ns,
        note="a fresh Workflow instance per call",
        rss_after_import_kb=RSS_AFTER_IMPORT_KB, rss_before_burst_kb=rss_before)


# ── multiprocessing mode ───────────────────────────────────────────────

def _mp_init():
    check(asyncio.run(run_chain()))  # first-call cost off the clock


def _mp_invoke(_: int) -> tuple:
    # A new event loop per call; the workflow is created and awaited inside it.
    return bc.timed_call(lambda: asyncio.run(run_chain()), check)


def run_mp(concurrency: int) -> dict:
    pool_size = os.cpu_count() or 1
    rss_before = bc.vm_kb("VmRSS")
    collector, wall_ns, info = bc.burst_mp(concurrency, pool_size, _mp_invoke, _mp_init)
    return bc.build_record(
        "llamaindex", f"mp-pool-{pool_size}", concurrency, collector, wall_ns,
        note="a fresh Workflow instance and a new event loop per call in each pool worker; pool started before the clock",
        rss_after_import_kb=RSS_AFTER_IMPORT_KB, rss_before_burst_kb=rss_before, mp_info=info)


def main() -> None:
    mode, concurrency = bc.parse_args("bench_concurrent_llamaindex.py")
    bc.finish(run_async(concurrency) if mode == "async" else run_mp(concurrency))


if __name__ == "__main__":
    main()
