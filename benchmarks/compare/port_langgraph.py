"""LangGraph port of the two workloads.

Workloads (identical for every framework): `seq` is a 3-node chain a -> b -> c where each node writes counter+1
(the answer is 3); `par` is a fan-out of five workers that each append their index to `results`, joined by a
summarizer that writes `count = len(results)` (the answer is five indices and 5). The graph is compiled once,
outside the timed region.

Variants:

  ainvoke_async   `await graph.ainvoke(...)` with `async def` nodes. The coroutine path: every node runs on the
                  caller's event loop in the calling thread. Matched regime, single thread.
  invoke_sync     `graph.invoke(...)` with plain `def` nodes: the form the LangGraph quick start shows first. A
                  step with one ready node runs inline; a step with several ready nodes is submitted to a thread
                  pool that the sync loop creates for every `invoke` (langgraph/pregel/_runner.py,
                  langchain_core/runnables/config.py get_executor_for_config). Idiomatic default, and the matched
                  thread-pool regime for `par`.
"""

PACKAGES = ["langgraph", "langchain-core", "langgraph-checkpoint", "langgraph-prebuilt"]

VARIANTS = {
    "ainvoke_async": {"kind": "async", "gate": True},
    "invoke_sync": {"kind": "sync", "gate": False},
}

_ns: dict = {}


def load() -> None:
    if _ns:
        return
    from operator import add
    from typing import Annotated, TypedDict

    from langgraph.graph import END, START, StateGraph

    class SeqState(TypedDict):
        counter: int

    class ParState(TypedDict):
        results: Annotated[list, add]
        count: int

    _ns.update(add=add, Annotated=Annotated, TypedDict=TypedDict, END=END, START=START, StateGraph=StateGraph,
               SeqState=SeqState, ParState=ParState)


def _build(variant: str, workload: str, gate=None):
    load()
    StateGraph, START, END = _ns["StateGraph"], _ns["START"], _ns["END"]
    is_async = VARIANTS[variant]["kind"] == "async"
    if workload == "seq":
        g = StateGraph(_ns["SeqState"])
        if is_async:
            async def inc(state):
                return {"counter": state["counter"] + 1}

            async def inc_gated(state):
                if gate is not None:
                    await gate.wait()
                return {"counter": state["counter"] + 1}
            nodes = {"a": inc, "b": inc_gated, "c": inc}
        else:
            def inc(state):
                return {"counter": state["counter"] + 1}
            nodes = {"a": inc, "b": inc, "c": inc}
        for name, fn in nodes.items():
            g.add_node(name, fn)
        g.add_edge(START, "a")
        g.add_edge("a", "b")
        g.add_edge("b", "c")
        g.add_edge("c", END)
        return g.compile()

    g = StateGraph(_ns["ParState"])

    def make_worker(idx: int):
        if is_async:
            async def worker(state):
                return {"results": [idx]}
        else:
            def worker(state):
                return {"results": [idx]}
        return worker

    if is_async:
        async def summarizer(state):
            if gate is not None:
                await gate.wait()  # parked after the fan-out has been collected
            return {"count": len(state["results"])}
    else:
        def summarizer(state):
            return {"count": len(state["results"])}

    for i in range(1, 6):
        g.add_node(f"w{i}", make_worker(i))
    g.add_node("summarizer", summarizer)
    for i in range(1, 6):
        g.add_edge(START, f"w{i}")
        g.add_edge(f"w{i}", "summarizer")
    g.add_edge("summarizer", END)
    return g.compile()


def build(variant: str, workload: str):
    """Compile the graph once and return the callable that does one run."""
    graph = _build(variant, workload)
    if VARIANTS[variant]["kind"] == "async":
        if workload == "seq":
            async def call():
                return await graph.ainvoke({"counter": 0})
        else:
            async def call():
                return await graph.ainvoke({"results": [], "count": 0})
    else:
        if workload == "seq":
            def call():
                return graph.invoke({"counter": 0})
        else:
            def call():
                return graph.invoke({"results": [], "count": 0})
    return call


def make_inflight(variant: str, workload: str, gate):
    """A factory of calls for the in-flight memory cell: the compiled graph is shared by every run."""
    graph = _build(variant, workload, gate)
    if workload == "seq":
        async def call():
            return await graph.ainvoke({"counter": 0})
    else:
        async def call():
            return await graph.ainvoke({"results": [], "count": 0})
    return lambda: call


def check(workload: str, result) -> None:
    if workload == "seq":
        assert result == {"counter": 3}, f"seq: expected {{'counter': 3}}, got {result!r}"
    else:
        assert result.get("count") == 5 and sorted(result.get("results", [])) == [1, 2, 3, 4, 5], (
            f"par: expected count 5 and results 1..5, got {result!r}")
