"""pydantic-graph port of the two workloads.

Workloads: see port_langgraph.py. The graph is built once, outside the timed region.

pydantic-graph's current API is the `GraphBuilder` (steps are `async def`, `edge_from(a).to(b, c)` broadcasts to
several steps that run concurrently on the event loop, and a `join` with a reducer waits for all of them).
`Graph.run` is a coroutine; `run_sync` only drives that coroutine on the loop. The library has no thread-pool path
(sync step functions are not accepted), so there is no pool regime for it. The `Graph(nodes=[...])` constructor
that earlier versions of this benchmark used no longer exists.

Variants:

  builder_steps   `@g.step` functions on a `GraphBuilder`. `seq` is a chain of three steps that each add one to
                  the counter in the graph state. `par` broadcasts from the start node to five workers, joins
                  their results with `reduce_list_append` and passes the list to a summarizer step: a real
                  fan-out. Matched regime, single thread, and the form the step documentation shows first.
  basenode_chain  `BaseNode` subclasses registered with `g.node(...)`: the form the graph documentation shows
                  first. A `BaseNode` returns exactly one next node, so `par` is a serial chain of six nodes
                  (w1 .. w5, summarizer), not a fan-out. Idiomatic default only; no ratio is formed with it.
"""

PACKAGES = ["pydantic-graph", "pydantic"]

VARIANTS = {
    "builder_steps": {"kind": "async", "gate": True},
    "basenode_chain": {"kind": "async", "gate": True},
}

_ns: dict = {}


def load() -> None:
    if _ns:
        return
    from dataclasses import dataclass, field

    from pydantic_graph import BaseNode, End, GraphBuilder, GraphRunContext, StepContext, reduce_list_append

    @dataclass
    class SeqState:
        counter: int = 0

    @dataclass
    class ParState:
        results: list = field(default_factory=list)
        count: int = 0

    _ns.update(dataclass=dataclass, field=field, BaseNode=BaseNode, End=End, GraphBuilder=GraphBuilder,
               GraphRunContext=GraphRunContext, StepContext=StepContext, reduce_list_append=reduce_list_append,
               SeqState=SeqState, ParState=ParState)


def _build_steps(workload: str, gate=None):
    load()
    GraphBuilder, StepContext = _ns["GraphBuilder"], _ns["StepContext"]
    SeqState, ParState = _ns["SeqState"], _ns["ParState"]
    if workload == "seq":
        g = GraphBuilder(state_type=SeqState, output_type=int)

        @g.step
        async def a(ctx: StepContext[SeqState, None, None]) -> None:
            ctx.state.counter += 1

        @g.step
        async def b(ctx: StepContext[SeqState, None, None]) -> None:
            if gate is not None:
                await gate.wait()
            ctx.state.counter += 1

        @g.step
        async def c(ctx: StepContext[SeqState, None, None]) -> int:
            ctx.state.counter += 1
            return ctx.state.counter

        g.add(
            g.edge_from(g.start_node).to(a),
            g.edge_from(a).to(b),
            g.edge_from(b).to(c),
            g.edge_from(c).to(g.end_node),
        )
        return g.build()

    reduce_list_append = _ns["reduce_list_append"]
    g = GraphBuilder(state_type=ParState, output_type=tuple)
    workers = []
    for idx in range(1, 6):
        def make(i):
            @g.step(node_id=f"w{i}")
            async def worker(ctx: StepContext[ParState, None, None]) -> int:
                return i
            return worker
        workers.append(make(idx))

    collect = g.join(reduce_list_append, initial_factory=list[int])

    @g.step
    async def summarizer(ctx: StepContext[ParState, None, list[int]]) -> tuple:
        if gate is not None:
            await gate.wait()  # parked after the fan-out has been collected
        return (len(ctx.inputs), tuple(sorted(ctx.inputs)))

    g.add(
        g.edge_from(g.start_node).to(*workers),
        g.edge_from(*workers).to(collect),
        g.edge_from(collect).to(summarizer),
        g.edge_from(summarizer).to(g.end_node),
    )
    return g.build()


def _build_nodes(workload: str, gate=None):
    load()
    dataclass, BaseNode, End = _ns["dataclass"], _ns["BaseNode"], _ns["End"]
    GraphRunContext, GraphBuilder, StepContext = _ns["GraphRunContext"], _ns["GraphBuilder"], _ns["StepContext"]
    SeqState, ParState = _ns["SeqState"], _ns["ParState"]
    if workload == "seq":
        @dataclass
        class C(BaseNode[SeqState, object, int]):
            async def run(self, ctx: GraphRunContext[SeqState]) -> End[int]:
                ctx.state.counter += 1
                return End(ctx.state.counter)

        @dataclass
        class B(BaseNode[SeqState]):
            async def run(self, ctx: GraphRunContext[SeqState]) -> C:
                if gate is not None:
                    await gate.wait()
                ctx.state.counter += 1
                return C()

        @dataclass
        class A(BaseNode[SeqState]):
            async def run(self, ctx: GraphRunContext[SeqState]) -> B:
                ctx.state.counter += 1
                return B()

        g = GraphBuilder(state_type=SeqState, output_type=int)

        @g.step
        async def start(ctx: StepContext[SeqState, None, None]) -> A:
            return A()

        g.add(g.node(A), g.node(B), g.node(C), g.edge_from(g.start_node).to(start))
        return g.build()

    @dataclass
    class Summ(BaseNode[ParState, object, tuple]):
        async def run(self, ctx: GraphRunContext[ParState]) -> End[tuple]:
            if gate is not None:
                await gate.wait()  # parked in the last node
            ctx.state.count = len(ctx.state.results)
            return End((ctx.state.count, tuple(sorted(ctx.state.results))))

    def make(i, nxt):
        @dataclass
        class W(BaseNode[ParState]):
            async def run(self, ctx: GraphRunContext[ParState]) -> nxt:  # type: ignore[valid-type]
                ctx.state.results.append(i)
                return nxt()
        W.__name__ = f"W{i}"
        W.__qualname__ = f"W{i}"
        return W

    chain = Summ
    for i in range(5, 0, -1):
        chain = make(i, chain)
    first = chain
    g = GraphBuilder(state_type=ParState, output_type=tuple)

    @g.step
    async def start(ctx: StepContext[ParState, None, None]) -> first:  # type: ignore[valid-type]
        return first()

    # Register every node class of the chain so the builder knows the whole graph.
    nodes = []
    cls = first
    while True:
        nodes.append(cls)
        hint = cls.run.__annotations__.get("return")
        if cls is Summ:
            break
        cls = hint if not isinstance(hint, str) else None
        if cls is None:
            break
    g.add(*[g.node(n) for n in nodes], g.edge_from(g.start_node).to(start))
    return g.build()


def _graph(variant: str, workload: str, gate=None):
    return _build_steps(workload, gate) if variant == "builder_steps" else _build_nodes(workload, gate)


def _runner(variant: str, workload: str, graph):
    load()
    SeqState, ParState = _ns["SeqState"], _ns["ParState"]
    state_type = SeqState if workload == "seq" else ParState

    async def call():
        return await graph.run(state=state_type())
    return call


def build(variant: str, workload: str):
    return _runner(variant, workload, _graph(variant, workload))


def make_inflight(variant: str, workload: str, gate):
    call = _runner(variant, workload, _graph(variant, workload, gate))
    return lambda: call


def check(workload: str, result) -> None:
    if workload == "seq":
        assert result == 3, f"seq: expected 3, got {result!r}"
    else:
        assert result == (5, (1, 2, 3, 4, 5)), f"par: expected (5, (1, 2, 3, 4, 5)), got {result!r}"
