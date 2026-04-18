#!/usr/bin/env python3
"""Engine-overhead benchmark for LlamaIndex Workflow.

Mirrors bench_neograph.cpp / bench_langgraph.py:

  * seq  — 3-step chain, each increments a counter field.
  * par  — fan-out 5 workers + summarizer. Each worker emits its
           index; summarizer counts.

Workflow is instantiated once per iteration (LlamaIndex semantics —
Workflow.run() consumes the workflow's event bus, so re-use across runs
is not officially supported; we construct fresh each iteration which is
the idiomatic pattern). Columns match the other benches:
workload, iters, total_ms, per_iter_us.
"""

from __future__ import annotations

import asyncio
import sys
import time

from llama_index.core.workflow import (
    Context,
    Event,
    StartEvent,
    StopEvent,
    Workflow,
    step,
)


# ── Sequential ─────────────────────────────────────────────────────────

class AEv(Event):
    counter: int


class BEv(Event):
    counter: int


class SeqFlow(Workflow):
    @step
    async def a(self, ev: StartEvent) -> AEv:
        return AEv(counter=ev.counter + 1)

    @step
    async def b(self, ev: AEv) -> BEv:
        return BEv(counter=ev.counter + 1)

    @step
    async def c(self, ev: BEv) -> StopEvent:
        return StopEvent(result=ev.counter + 1)


# ── Parallel fan-out ───────────────────────────────────────────────────

class WorkerEv(Event):
    idx: int


class ResultEv(Event):
    idx: int


class ParFlow(Workflow):
    @step
    async def dispatch(self, ctx: Context, ev: StartEvent) -> WorkerEv:
        for i in range(1, 6):
            ctx.send_event(WorkerEv(idx=i))

    @step(num_workers=5)
    async def worker(self, ev: WorkerEv) -> ResultEv:
        return ResultEv(idx=ev.idx)

    @step
    async def summarizer(self, ctx: Context, ev: ResultEv) -> StopEvent:
        data = ctx.collect_events(ev, [ResultEv] * 5)
        if data is None:
            return None
        return StopEvent(result=len(data))


# ── Bench harness ──────────────────────────────────────────────────────

async def bench_seq(iters: int) -> tuple[int, float]:
    t0 = time.perf_counter()
    for _ in range(iters):
        await SeqFlow(timeout=60).run(counter=0)
    total_s = time.perf_counter() - t0
    return int(total_s * 1000), (total_s * 1_000_000) / iters


async def bench_par(iters: int) -> tuple[int, float]:
    t0 = time.perf_counter()
    for _ in range(iters):
        await ParFlow(timeout=60).run()
    total_s = time.perf_counter() - t0
    return int(total_s * 1000), (total_s * 1_000_000) / iters


async def main() -> None:
    seq_iters = int(sys.argv[1]) if len(sys.argv) > 1 else 10000
    par_iters = int(sys.argv[2]) if len(sys.argv) > 2 else 5000

    # Warm-up
    for _ in range(10):
        await SeqFlow(timeout=60).run(counter=0)

    seq_total, seq_per = await bench_seq(seq_iters)
    print(f"seq\t{seq_iters}\t{seq_total}\t{seq_per:.2f}")

    par_total, par_per = await bench_par(par_iters)
    print(f"par\t{par_iters}\t{par_total}\t{par_per:.2f}")


if __name__ == "__main__":
    asyncio.run(main())
