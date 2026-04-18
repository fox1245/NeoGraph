#!/usr/bin/env python3
"""Engine-overhead benchmark for Haystack Pipeline.

Mirrors bench_neograph.cpp / bench_langgraph.py:

  * seq  — 3-component chain, each increments a counter field.
  * par  — fan-out 5 workers + summarizer. Haystack has no append
           reducer, so each worker emits on a distinct socket and the
           summarizer sums the list lengths.

Pipeline is built once; Pipeline.run() is the hot loop. Columns:
workload, iters, total_ms, per_iter_us.
"""

from __future__ import annotations

import sys
import time

from haystack import Pipeline, component


# ── Sequential ─────────────────────────────────────────────────────────

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


# ── Parallel fan-out ───────────────────────────────────────────────────

@component
class Worker:
    def __init__(self, idx: int):
        self.idx = idx

    @component.output_types(result=list)
    def run(self, trigger: int):
        return {"result": [self.idx]}


@component
class Summarizer:
    @component.output_types(count=int)
    def run(self, r1: list, r2: list, r3: list, r4: list, r5: list):
        return {"count": len(r1) + len(r2) + len(r3) + len(r4) + len(r5)}


def build_par() -> Pipeline:
    p = Pipeline()
    for i in range(1, 6):
        p.add_component(f"w{i}", Worker(i))
    p.add_component("s", Summarizer())
    for i in range(1, 6):
        p.connect(f"w{i}.result", f"s.r{i}")
    return p


# ── Bench harness ──────────────────────────────────────────────────────

def bench(pipeline: Pipeline, initial: dict, iters: int) -> tuple[int, float]:
    t0 = time.perf_counter()
    for _ in range(iters):
        pipeline.run(initial)
    total_s = time.perf_counter() - t0
    return int(total_s * 1000), (total_s * 1_000_000) / iters


def main() -> None:
    seq_iters = int(sys.argv[1]) if len(sys.argv) > 1 else 10000
    par_iters = int(sys.argv[2]) if len(sys.argv) > 2 else 5000

    # Warm-up
    warm = build_seq()
    for _ in range(10):
        warm.run({"a": {"counter": 0}})

    seq_graph = build_seq()
    seq_total, seq_per = bench(seq_graph, {"a": {"counter": 0}}, seq_iters)
    print(f"seq\t{seq_iters}\t{seq_total}\t{seq_per:.2f}")

    par_graph = build_par()
    par_total, par_per = bench(
        par_graph, {f"w{i}": {"trigger": 0} for i in range(1, 6)}, par_iters
    )
    print(f"par\t{par_iters}\t{par_total}\t{par_per:.2f}")


if __name__ == "__main__":
    main()
