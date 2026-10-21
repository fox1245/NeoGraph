"""LlamaIndex Workflow port of the two workloads.

Workloads: see port_langgraph.py. `seq` is three steps, each turning an event into the next with counter+1. `par`
is a `dispatch` step that sends five worker events, a `worker` step (five at once) that turns each into a result
event, and a `summarizer` step that collects the five result events with `ctx.collect_events` and ends the run
with their count. That join idiom runs the `summarizer` step once per result event (four of the five calls return
`None` while they wait), so a run executes eleven step functions where the other frameworks execute six or seven
nodes; it is the framework's own way to wait for several events.

A `Workflow` instance holds configuration: `run()` creates a fresh `Context` per call, and the validation of the
step graph is cached on the instance after its first run. The workflow can therefore be reused across runs; the
`probe_reuse` helper checks that against the installed version and the compare driver records the answer.

Variants:

  reused_instance one `Workflow` instance, constructed and validated once outside the timed region, `await
                  wf.run(...)` per call. Matched regime, single thread (the compile-once rule of the other ports).
  fresh_instance  a new instance per call (`await Flow(timeout=60).run(...)`), the form the LlamaIndex
                  documentation shows first. The construction and the validation of the step graph are part of
                  the timed call. Idiomatic default; absolute times only.

Every step here is a coroutine (`async def`) on one event loop. The workflows runtime also accepts a plain `def` step
and runs it on the event loop's default executor (a thread-pool path, in llama-index-workflows 2.26.0); that path is
not part of this cohort, so there is no pool variant.
"""

PACKAGES = ["llama-index-core", "llama-index-workflows"]

VARIANTS = {
    "reused_instance": {"kind": "async", "gate": True},
    "fresh_instance": {"kind": "async", "gate": True},
}

_ns: dict = {}


def load() -> None:
    if _ns:
        return
    try:
        from workflows import Context, Workflow, step
        from workflows.events import Event, StartEvent, StopEvent
    except ImportError:  # older llama-index-core re-exports the same names
        from llama_index.core.workflow import Context, Event, StartEvent, StopEvent, Workflow, step

    _ns.update(Context=Context, Workflow=Workflow, step=step, Event=Event, StartEvent=StartEvent,
               StopEvent=StopEvent)


def _classes(gate=None):
    load()
    Context, Workflow, step = _ns["Context"], _ns["Workflow"], _ns["step"]
    Event, StartEvent, StopEvent = _ns["Event"], _ns["StartEvent"], _ns["StopEvent"]

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
            if gate is not None:
                await gate.wait()
            return BEv(counter=ev.counter + 1)

        @step
        async def c(self, ev: BEv) -> StopEvent:
            return StopEvent(result=ev.counter + 1)

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
            if gate is not None:
                await gate.wait()  # parked after the fan-out has been collected
            return StopEvent(result=(len(data), tuple(sorted(e.idx for e in data))))

    return SeqFlow, ParFlow


def _flow_class(workload: str, gate=None):
    seq, par = _classes(gate)
    return seq if workload == "seq" else par


def build(variant: str, workload: str):
    flow_class = _flow_class(workload)
    if variant == "reused_instance":
        flow = flow_class(timeout=60)

        if workload == "seq":
            async def call():
                return await flow.run(counter=0)
        else:
            async def call():
                return await flow.run()
    else:
        if workload == "seq":
            async def call():
                return await flow_class(timeout=60).run(counter=0)
        else:
            async def call():
                return await flow_class(timeout=60).run()
    return call


def make_inflight(variant: str, workload: str, gate):
    flow_class = _flow_class(workload, gate)
    if variant == "reused_instance":
        flow = flow_class(timeout=120)

        def make():
            if workload == "seq":
                async def call():
                    return await flow.run(counter=0)
            else:
                async def call():
                    return await flow.run()
            return call
        return make

    def make_fresh():
        if workload == "seq":
            async def call():
                return await flow_class(timeout=120).run(counter=0)
        else:
            async def call():
                return await flow_class(timeout=120).run()
        return call
    return make_fresh


def check(workload: str, result) -> None:
    if workload == "seq":
        assert result == 3, f"seq: expected 3, got {result!r}"
    else:
        assert result == (5, (1, 2, 3, 4, 5)), f"par: expected (5, (1, 2, 3, 4, 5)), got {result!r}"


def probe_reuse() -> dict:
    """Can one instance be run repeatedly and concurrently? Returns the evidence for the record."""
    import asyncio

    load()
    seq, _ = _classes()
    flow = seq(timeout=30)
    out = {}

    async def main():
        first = await flow.run(counter=0)
        second = await flow.run(counter=10)
        both = await asyncio.gather(flow.run(counter=0), flow.run(counter=100))
        return first, second, both

    first, second, both = asyncio.run(main())
    out["sequential_runs"] = [first, second]
    out["concurrent_runs"] = list(both)
    out["reusable"] = first == 3 and second == 13 and sorted(both) == [3, 103]
    return out
