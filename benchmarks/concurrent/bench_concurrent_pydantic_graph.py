#!/usr/bin/env python3
"""Concurrent-load bench for pydantic-graph.

pydantic-graph is async-native — Graph.run() is a coroutine.

Two APIs exist. pydantic-graph 2.x only has the builder API (GraphBuilder,
steps and edges; the BaseNode state-machine constructor
Graph(nodes=[...], state_type=...) was removed). pydantic-graph 1.x has the
BaseNode state machine. The harness uses the builder API when it is exported
at the top level and the BaseNode state machine otherwise; "note" in the
record names the one used. Both run the same 3-node counter chain.

Record schema 2, see burst_common.py.
"""

from __future__ import annotations

import asyncio
import os
from dataclasses import dataclass

import burst_common as bc

try:
    from pydantic_graph import GraphBuilder, StepContext
    API = "builder"
except ImportError:  # pydantic-graph 1.x
    from pydantic_graph import BaseNode, End, Graph, GraphRunContext
    API = "basenode"

RSS_AFTER_IMPORT_KB = bc.vm_kb("VmRSS")

if API == "builder":
    def _build():
        builder = GraphBuilder(input_type=int, output_type=int)

        @builder.step
        async def a(ctx: StepContext[None, None, int]) -> int:
            bc.inject_failure("pydantic-graph")
            return ctx.inputs + 1

        @builder.step
        async def b(ctx: StepContext[None, None, int]) -> int:
            return ctx.inputs + 1

        @builder.step
        async def c(ctx: StepContext[None, None, int]) -> int:
            return ctx.inputs + 1

        builder.add(
            builder.edge_from(builder.start_node).to(a),
            builder.edge_from(a).to(b),
            builder.edge_from(b).to(c),
            builder.edge_from(c).to(builder.end_node),
        )
        return builder.build()

    SEQ_GRAPH = _build()

    def run_chain():
        return SEQ_GRAPH.run(inputs=0)
else:
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
            bc.inject_failure("pydantic-graph")
            ctx.state.counter += 1
            return SB()

    SEQ_GRAPH = Graph(nodes=[SA, SB, SC], state_type=SeqState)

    def run_chain():
        return SEQ_GRAPH.run(SA(), state=SeqState())


def check(result) -> None:
    # Builder API returns the output; the BaseNode API returns a GraphRunResult.
    value = result if API == "builder" else result.output
    bc.check_equal("pydantic-graph output", value)


# ── async mode ─────────────────────────────────────────────────────────

def run_async(concurrency: int) -> dict:
    async def _warm():
        for _ in range(10):
            check(await run_chain())
    failed = bc.guard_warmup("pydantic-graph", "asyncio", concurrency,
                             lambda: asyncio.run(_warm()), RSS_AFTER_IMPORT_KB)
    if failed:
        return failed
    rss_before = bc.vm_kb("VmRSS")

    collector, wall_ns = asyncio.run(bc.burst_async(concurrency, run_chain, check))
    return bc.build_record(
        "pydantic-graph", "asyncio", concurrency, collector, wall_ns,
        note=f"{API} API; the nodes never suspend, so each task runs to completion when first scheduled",
        rss_after_import_kb=RSS_AFTER_IMPORT_KB, rss_before_burst_kb=rss_before)


# ── multiprocessing mode ───────────────────────────────────────────────

def _mp_init():
    async def warm():
        return await run_chain()
    check(asyncio.run(warm()))  # first-call cost off the clock


def _mp_invoke(_: int) -> tuple:
    # A new event loop per call, as an unwrapped worker function would do.
    async def one():
        return await run_chain()
    return bc.timed_call(lambda: asyncio.run(one()), check)


def run_mp(concurrency: int) -> dict:
    pool_size = os.cpu_count() or 1
    rss_before = bc.vm_kb("VmRSS")
    collector, wall_ns, info = bc.burst_mp(concurrency, pool_size, _mp_invoke, _mp_init)
    return bc.build_record(
        "pydantic-graph", f"mp-pool-{pool_size}", concurrency, collector, wall_ns,
        note=f"{API} API; a new event loop per call in each pool worker; pool started before the clock",
        rss_after_import_kb=RSS_AFTER_IMPORT_KB, rss_before_burst_kb=rss_before, mp_info=info)


def main() -> None:
    mode, concurrency = bc.parse_args("bench_concurrent_pydantic_graph.py")
    bc.finish(run_async(concurrency) if mode == "async" else run_mp(concurrency))


if __name__ == "__main__":
    main()
