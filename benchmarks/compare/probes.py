#!/usr/bin/env python3
"""Functional probes that settle questions about the frameworks' APIs; each prints one JSON object.

    probes.py llamaindex_reuse      can one LlamaIndex Workflow instance run again, and concurrently?
    probes.py threads <framework>   which threads do the five workers of `par` run on, per variant?
    probes.py pydantic_async        is pydantic-graph async-only, and is its fan-out real?

The timing questions (the LangGraph thread-pool share, reuse against a fresh instance, the hardening delta) are
answered by the cells of the cohort itself; benchmarks/compare/probe_pydantic_legacy.py explains the
pydantic-graph difference between the two older harnesses.
"""

import asyncio
import json
import os
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def llamaindex_reuse() -> dict:
    import port_llamaindex as port

    result = port.probe_reuse()
    result["versions"] = _versions(port.PACKAGES)
    return result


def _versions(packages):
    from importlib import metadata

    return {p: metadata.version(p) for p in packages}


def _thread_summary(idents, main_ident):
    return {"distinct_threads": len(set(idents)), "workers": len(idents),
            "main_thread_used": main_ident in idents,
            "worker_thread_count_excluding_main": len(set(idents) - {main_ident})}


def threads_langgraph() -> dict:
    from operator import add
    from typing import Annotated, TypedDict

    from langgraph.graph import END, START, StateGraph

    class State(TypedDict):
        results: Annotated[list, add]
        count: int

    out = {}
    for variant in ("invoke_sync", "ainvoke_async"):
        seen: list[int] = []
        g = StateGraph(State)
        for i in range(1, 6):
            if variant == "invoke_sync":
                def worker(state, i=i):
                    seen.append(threading.get_ident())
                    return {"results": [i]}
            else:
                async def worker(state, i=i):
                    seen.append(threading.get_ident())
                    return {"results": [i]}
            g.add_node(f"w{i}", worker)
            g.add_edge(START, f"w{i}")
            g.add_edge(f"w{i}", "summarizer")

        def summarizer(state):
            return {"count": len(state["results"])}
        g.add_node("summarizer", summarizer)
        g.add_edge("summarizer", END)
        graph = g.compile()
        main_ident = threading.get_ident()
        if variant == "invoke_sync":
            graph.invoke({"results": [], "count": 0})
        else:
            asyncio.run(graph.ainvoke({"results": [], "count": 0}))
        out[variant] = _thread_summary(seen, main_ident)
    return out


def threads_haystack() -> dict:
    from haystack import Pipeline, component

    out = {}
    for variant in ("run_sync", "run_async_coro", "run_async_pool"):
        seen: list[int] = []

        @component
        class Worker:
            def __init__(self, idx):
                self.idx = idx

            @component.output_types(result=list)
            def run(self, trigger: int):
                seen.append(threading.get_ident())
                return {"result": [self.idx]}

        @component
        class WorkerAsync:
            def __init__(self, idx):
                self.idx = idx

            @component.output_types(result=list)
            def run(self, trigger: int):
                return {"result": [self.idx]}

            @component.output_types(result=list)
            async def run_async(self, trigger: int):
                seen.append(threading.get_ident())
                return {"result": [self.idx]}

        @component
        class Summ:
            @component.output_types(count=int)
            def run(self, r1: list, r2: list, r3: list, r4: list, r5: list):
                return {"count": 5}

        p = Pipeline()
        cls = WorkerAsync if variant == "run_async_coro" else Worker
        for i in range(1, 6):
            p.add_component(f"w{i}", cls(i))
        p.add_component("s", Summ())
        for i in range(1, 6):
            p.connect(f"w{i}.result", f"s.r{i}")
        inputs = {f"w{i}": {"trigger": 0} for i in range(1, 6)}
        main_ident = threading.get_ident()
        if variant == "run_sync":
            p.run(inputs)
        else:
            asyncio.run(p.run_async(inputs))
        out[variant] = _thread_summary(seen, main_ident)
    return out


def pydantic_async() -> dict:
    from importlib import metadata
    from dataclasses import dataclass

    from pydantic_graph import GraphBuilder, StepContext, reduce_list_append

    @dataclass
    class S:
        pass

    out = {"version": metadata.version("pydantic-graph")}
    # 1. Graph.run is a coroutine function; run_sync only drives it on the loop.
    g = GraphBuilder(state_type=S, output_type=int)

    @g.step
    async def one(ctx: StepContext[S, None, None]) -> int:
        return 1

    g.add(g.edge_from(g.start_node).to(one), g.edge_from(one).to(g.end_node))
    graph = g.build()
    out["run_is_coroutine_function"] = asyncio.iscoroutinefunction(graph.run)
    out["run_sync_result"] = graph.run_sync(state=S())
    # 2. A step that is a plain function: accepted or refused?
    try:
        g2 = GraphBuilder(state_type=S, output_type=int)

        @g2.step
        def sync_step(ctx: StepContext[S, None, None]) -> int:  # type: ignore[misc]
            return 1

        g2.add(g2.edge_from(g2.start_node).to(sync_step), g2.edge_from(sync_step).to(g2.end_node))
        res = asyncio.run(g2.build().run(state=S()))
        out["sync_step_function"] = {"accepted": True, "result": res}
    except Exception as error:  # noqa: BLE001
        out["sync_step_function"] = {"accepted": False, "error": f"{type(error).__name__}: {str(error)[:200]}"}
    # 3. Is the fan-out real? Five workers that each wait 50 ms: concurrent is about 50 ms, serial about 250 ms.
    idents: list[int] = []
    g3 = GraphBuilder(state_type=S, output_type=list)
    workers = []
    for i in range(1, 6):
        def make(k):
            @g3.step(node_id=f"w{k}")
            async def worker(ctx: StepContext[S, None, None]) -> int:
                idents.append(threading.get_ident())
                await asyncio.sleep(0.05)
                return k
            return worker
        workers.append(make(i))
    collect = g3.join(reduce_list_append, initial_factory=list[int])
    g3.add(g3.edge_from(g3.start_node).to(*workers), g3.edge_from(*workers).to(collect),
           g3.edge_from(collect).to(g3.end_node))
    graph3 = g3.build()
    started = time.perf_counter()
    res3 = asyncio.run(graph3.run(state=S()))
    elapsed = time.perf_counter() - started
    out["fan_out"] = {"workers": 5, "each_waits_s": 0.05, "elapsed_s": round(elapsed, 4),
                      "result": sorted(res3), "concurrent": elapsed < 0.15,
                      "distinct_threads": len(set(idents)), "main_thread_used": threading.get_ident() in idents}
    return out


def main() -> int:
    which = sys.argv[1] if len(sys.argv) > 1 else ""
    if which == "llamaindex_reuse":
        result = llamaindex_reuse()
    elif which == "threads":
        fw = sys.argv[2]
        result = {"langgraph": threads_langgraph, "haystack": threads_haystack}[fw]()
    elif which == "pydantic_async":
        result = pydantic_async()
    else:
        print(__doc__, file=sys.stderr)
        return 2
    print("record\t" + json.dumps({"probe": which, "result": result}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
