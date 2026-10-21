#!/usr/bin/env python3
"""Why did two older harnesses measure the same pydantic-graph call so differently?

The engine-overhead harness (a loop of awaited `Graph.run` calls) gave about 227 microseconds per run; the burst
harness (all runs created as tasks and gathered) gave 70 to 100 microseconds. Both used the legacy
`Graph(nodes=[...])` API of pydantic-graph 1.x (the current 2.x API has no such constructor), with the same three
`BaseNode` classes and the same `Graph.run(start_node, state=...)` call. This probe runs both shapes, and a few
variations, in one process on one machine and prints microseconds per run for each, so that the difference can be
attributed to the harness shape or to something else.

    python probe_pydantic_legacy.py [runs]        # needs a venv with pydantic-graph 1.87.0 (legacy API)

Shapes (each repeated three times, interleaved in this order):

  loop            `for _ in range(n): await graph.run(...)` in the main coroutine: the engine-overhead harness
  burst           n tasks created up front and gathered: the burst harness (wall time / n)
  burst_chunks    the same tasks gathered in chunks of 100
  loop_in_task    the loop, inside one task
  loop_gc_off     the loop with the cyclic collector disabled
  burst_gc_off    the burst with the cyclic collector disabled
"""

from __future__ import annotations

import asyncio
import gc
import json
import sys
import time
from dataclasses import dataclass
from importlib import metadata

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


async def one() -> int:
    result = await SEQ_GRAPH.run(SA(), state=SeqState())
    assert result.output == 3, result
    return result.output


async def shape_loop(n: int) -> float:
    started = time.perf_counter()
    for _ in range(n):
        await one()
    return (time.perf_counter() - started) * 1e6 / n


async def shape_burst(n: int) -> float:
    started = time.perf_counter()
    await asyncio.gather(*[asyncio.create_task(one()) for _ in range(n)])
    return (time.perf_counter() - started) * 1e6 / n


async def shape_burst_chunks(n: int) -> float:
    started = time.perf_counter()
    for _ in range(n // 100):
        await asyncio.gather(*[asyncio.create_task(one()) for _ in range(100)])
    return (time.perf_counter() - started) * 1e6 / (n // 100 * 100)


async def shape_loop_in_task(n: int) -> float:
    return await asyncio.create_task(shape_loop(n))


async def shape_loop_gc_off(n: int) -> float:
    gc.disable()
    try:
        return await shape_loop(n)
    finally:
        gc.enable()


async def shape_burst_gc_off(n: int) -> float:
    gc.disable()
    try:
        return await shape_burst(n)
    finally:
        gc.enable()


SHAPES = [("loop", shape_loop), ("burst", shape_burst), ("burst_chunks", shape_burst_chunks),
          ("loop_in_task", shape_loop_in_task), ("loop_gc_off", shape_loop_gc_off), ("burst_gc_off", shape_burst_gc_off)]


async def main(n: int) -> dict:
    for _ in range(50):
        await one()
    samples: dict[str, list[float]] = {name: [] for name, _ in SHAPES}
    for _ in range(3):
        for name, shape in SHAPES:
            gc.collect()
            samples[name].append(await shape(n))
    return {
        "pydantic_graph": metadata.version("pydantic-graph"),
        "pydantic": metadata.version("pydantic"),
        "python": sys.version.split()[0],
        "runs_per_shape": n,
        "us_per_run": {name: [round(v, 2) for v in values] for name, values in samples.items()},
        "median_us_per_run": {name: round(sorted(values)[1], 2) for name, values in samples.items()},
    }


if __name__ == "__main__":
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 3000
    print("record\t" + json.dumps({"probe": "pydantic_legacy_shapes", "result": asyncio.run(main(count))}))
