"""Haystack port of the two workloads.

Workloads: see port_langgraph.py. The pipeline is built once, outside the timed region. Haystack has no append
reducer, so each worker emits on its own socket and the summarizer sums the list lengths; the same six components
run.

Variants:

  run_sync        `Pipeline.run(...)` with sync components: the form the Haystack quick start shows first. The
                  components run serially in the calling thread. Idiomatic default.
  run_async_coro  `await Pipeline.run_async(...)` with components that define a coroutine `run_async`: they are
                  awaited on the caller's event loop. Matched regime, single thread.
  run_async_pool  `await Pipeline.run_async(...)` with sync-only components: Haystack offloads each of them to a
                  thread with `asyncio.to_thread` (the loop's default executor). Matched thread-pool regime.

`concurrency_limit` keeps its default (4). Telemetry is switched off by the driver (HAYSTACK_TELEMETRY_ENABLED=False)
so that no benchmark process opens a network connection.
"""

PACKAGES = ["haystack-ai"]

VARIANTS = {
    "run_sync": {"kind": "sync", "gate": False},
    "run_async_coro": {"kind": "async", "gate": True},
    "run_async_pool": {"kind": "async", "gate": False},
}

_ns: dict = {}


def load() -> None:
    if _ns:
        return
    from haystack import Pipeline, component

    @component
    class Inc:
        def __init__(self, gate=None):
            self.gate = gate

        @component.output_types(counter=int)
        def run(self, counter: int):
            return {"counter": counter + 1}

        @component.output_types(counter=int)
        async def run_async(self, counter: int):
            if self.gate is not None:
                await self.gate.wait()
            return {"counter": counter + 1}

    @component
    class IncSyncOnly:
        @component.output_types(counter=int)
        def run(self, counter: int):
            return {"counter": counter + 1}

    @component
    class Worker:
        def __init__(self, idx: int, gate=None):
            self.idx = idx
            self.gate = gate

        @component.output_types(result=list)
        def run(self, trigger: int):
            return {"result": [self.idx]}

        @component.output_types(result=list)
        async def run_async(self, trigger: int):
            return {"result": [self.idx]}

    @component
    class WorkerSyncOnly:
        def __init__(self, idx: int):
            self.idx = idx

        @component.output_types(result=list)
        def run(self, trigger: int):
            return {"result": [self.idx]}

    @component
    class Summarizer:
        def __init__(self, gate=None):
            self.gate = gate

        @component.output_types(count=int)
        def run(self, r1: list, r2: list, r3: list, r4: list, r5: list):
            return {"count": len(r1) + len(r2) + len(r3) + len(r4) + len(r5)}

        @component.output_types(count=int)
        async def run_async(self, r1: list, r2: list, r3: list, r4: list, r5: list):
            if self.gate is not None:
                await self.gate.wait()  # parked after the fan-out has been collected
            return {"count": len(r1) + len(r2) + len(r3) + len(r4) + len(r5)}

    @component
    class SummarizerSyncOnly:
        @component.output_types(count=int)
        def run(self, r1: list, r2: list, r3: list, r4: list, r5: list):
            return {"count": len(r1) + len(r2) + len(r3) + len(r4) + len(r5)}

    _ns.update(Pipeline=Pipeline, Inc=Inc, IncSyncOnly=IncSyncOnly, Worker=Worker, WorkerSyncOnly=WorkerSyncOnly,
               Summarizer=Summarizer, SummarizerSyncOnly=SummarizerSyncOnly)


def _build(variant: str, workload: str, gate=None):
    load()
    Pipeline = _ns["Pipeline"]
    sync_only = variant == "run_async_pool"
    p = Pipeline()
    if workload == "seq":
        cls = _ns["IncSyncOnly"] if sync_only else _ns["Inc"]
        if sync_only:
            p.add_component("a", cls())
            p.add_component("b", cls())
            p.add_component("c", cls())
        else:
            p.add_component("a", cls())
            p.add_component("b", cls(gate))
            p.add_component("c", cls())
        p.connect("a.counter", "b.counter")
        p.connect("b.counter", "c.counter")
        return p
    for i in range(1, 6):
        if sync_only:
            p.add_component(f"w{i}", _ns["WorkerSyncOnly"](i))
        else:
            p.add_component(f"w{i}", _ns["Worker"](i, gate))
    p.add_component("s", _ns["SummarizerSyncOnly"]() if sync_only else _ns["Summarizer"](gate))
    for i in range(1, 6):
        p.connect(f"w{i}.result", f"s.r{i}")
    return p


def _inputs(workload: str):
    if workload == "seq":
        return {"a": {"counter": 0}}
    return {f"w{i}": {"trigger": 0} for i in range(1, 6)}


def build(variant: str, workload: str):
    pipeline = _build(variant, workload)
    if VARIANTS[variant]["kind"] == "async":
        async def call():
            return await pipeline.run_async(_inputs(workload))
    else:
        def call():
            return pipeline.run(_inputs(workload))
    return call


def make_inflight(variant: str, workload: str, gate):
    pipeline = _build(variant, workload, gate)

    async def call():
        return await pipeline.run_async(_inputs(workload))
    return lambda: call


def check(workload: str, result) -> None:
    if workload == "seq":
        assert result == {"c": {"counter": 3}}, f"seq: expected {{'c': {{'counter': 3}}}}, got {result!r}"
    else:
        assert result == {"s": {"count": 5}}, f"par: expected {{'s': {{'count': 5}}}}, got {result!r}"
