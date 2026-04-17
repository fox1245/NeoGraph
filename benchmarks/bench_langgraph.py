#!/usr/bin/env python3
"""Engine-overhead benchmark for LangGraph.

Apples-to-apples counterpart of bench_neograph.cpp:

  * seq  — 3-node chain, each increments a counter field.
  * par  — fan-out 5 workers + summarizer. Each worker appends its
           index; summarizer counts.

Graph is compiled once; invoke() is the hot loop. Columns match the
C++ bench: workload, iters, total_ms, per_iter_us.
"""

from __future__ import annotations

import sys
import time
from operator import add
from typing import Annotated, TypedDict

from langgraph.graph import StateGraph, START, END


# ── Sequential ─────────────────────────────────────────────────────────

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


# ── Parallel fan-out ───────────────────────────────────────────────────

class ParState(TypedDict):
    results: Annotated[list, add]
    count: int


def _make_worker(idx: int):
    def _worker(state: ParState) -> dict:
        return {"results": [idx]}
    _worker.__name__ = f"w{idx}"
    return _worker


def summarizer(state: ParState) -> dict:
    return {"count": len(state["results"])}


def build_par():
    g = StateGraph(ParState)
    for i in range(1, 6):
        g.add_node(f"w{i}", _make_worker(i))
    g.add_node("summarizer", summarizer)
    for i in range(1, 6):
        g.add_edge(START, f"w{i}")
        g.add_edge(f"w{i}", "summarizer")
    g.add_edge("summarizer", END)
    return g.compile()


# ── Bench harness ──────────────────────────────────────────────────────

def bench(graph, initial: dict, iters: int) -> tuple[int, float]:
    t0 = time.perf_counter()
    for _ in range(iters):
        graph.invoke(initial)
    total_s = time.perf_counter() - t0
    total_ms = int(total_s * 1000)
    per_iter_us = (total_s * 1_000_000) / iters
    return total_ms, per_iter_us


def main() -> None:
    seq_iters = int(sys.argv[1]) if len(sys.argv) > 1 else 10000
    par_iters = int(sys.argv[2]) if len(sys.argv) > 2 else 5000

    # Warm-up: LangGraph lazy-initializes channels + runtime on first invoke.
    warm = build_seq()
    for _ in range(10):
        warm.invoke({"counter": 0})

    seq_graph = build_seq()
    seq_total, seq_per = bench(seq_graph, {"counter": 0}, seq_iters)
    print(f"seq\t{seq_iters}\t{seq_total}\t{seq_per:.2f}")

    par_graph = build_par()
    par_total, par_per = bench(par_graph, {"results": [], "count": 0}, par_iters)
    print(f"par\t{par_iters}\t{par_total}\t{par_per:.2f}")


if __name__ == "__main__":
    main()
