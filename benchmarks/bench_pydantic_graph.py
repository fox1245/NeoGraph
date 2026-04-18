#!/usr/bin/env python3
"""Engine-overhead benchmark for pydantic-graph.

Mirrors bench_neograph.cpp / bench_langgraph.py:

  * seq  — 3-node state-machine chain, each increments a counter.
  * par  — pydantic-graph is a single-next-node state machine and does
           NOT support fan-out. We emulate the 5-worker + summarizer
           workload as a serial 6-node chain (w1 → ... → w5 → summ).
           This is flagged clearly in the results — pydantic-graph
           cannot do actual parallel fan-out, so this measures a
           different workload shape.

Graph is built once; Graph.run() is the hot loop. Columns:
workload, iters, total_ms, per_iter_us.
"""

from __future__ import annotations

import asyncio
import sys
import time
from dataclasses import dataclass, field

from pydantic_graph import BaseNode, End, Graph, GraphRunContext


# ── Sequential ─────────────────────────────────────────────────────────

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


# ── "Parallel" fan-out (serial emulation) ──────────────────────────────

@dataclass
class ParState:
    results: list = field(default_factory=list)
    count: int = 0


@dataclass
class Summ(BaseNode[ParState, None, int]):
    async def run(self, ctx: GraphRunContext[ParState]) -> End[int]:
        ctx.state.count = len(ctx.state.results)
        return End(ctx.state.count)


@dataclass
class W5(BaseNode[ParState]):
    async def run(self, ctx: GraphRunContext[ParState]) -> Summ:
        ctx.state.results.append(5)
        return Summ()


@dataclass
class W4(BaseNode[ParState]):
    async def run(self, ctx: GraphRunContext[ParState]) -> W5:
        ctx.state.results.append(4)
        return W5()


@dataclass
class W3(BaseNode[ParState]):
    async def run(self, ctx: GraphRunContext[ParState]) -> W4:
        ctx.state.results.append(3)
        return W4()


@dataclass
class W2(BaseNode[ParState]):
    async def run(self, ctx: GraphRunContext[ParState]) -> W3:
        ctx.state.results.append(2)
        return W3()


@dataclass
class W1(BaseNode[ParState]):
    async def run(self, ctx: GraphRunContext[ParState]) -> W2:
        ctx.state.results.append(1)
        return W2()


PAR_GRAPH = Graph(nodes=[W1, W2, W3, W4, W5, Summ], state_type=ParState)


# ── Bench harness ──────────────────────────────────────────────────────

async def bench_seq(iters: int) -> tuple[int, float]:
    t0 = time.perf_counter()
    for _ in range(iters):
        await SEQ_GRAPH.run(SA(), state=SeqState())
    total_s = time.perf_counter() - t0
    return int(total_s * 1000), (total_s * 1_000_000) / iters


async def bench_par(iters: int) -> tuple[int, float]:
    t0 = time.perf_counter()
    for _ in range(iters):
        await PAR_GRAPH.run(W1(), state=ParState())
    total_s = time.perf_counter() - t0
    return int(total_s * 1000), (total_s * 1_000_000) / iters


async def main() -> None:
    seq_iters = int(sys.argv[1]) if len(sys.argv) > 1 else 10000
    par_iters = int(sys.argv[2]) if len(sys.argv) > 2 else 5000

    # Warm-up
    for _ in range(10):
        await SEQ_GRAPH.run(SA(), state=SeqState())

    seq_total, seq_per = await bench_seq(seq_iters)
    print(f"seq\t{seq_iters}\t{seq_total}\t{seq_per:.2f}")

    par_total, par_per = await bench_par(par_iters)
    print(f"par\t{par_iters}\t{par_total}\t{par_per:.2f}")


if __name__ == "__main__":
    asyncio.run(main())
