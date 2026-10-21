"""AutoGen GraphFlow port of the two workloads.

Workloads: see port_langgraph.py. AutoGen is message-passing, not state-channel: the counter is carried as the
text content of the messages, and the summarizer counts the worker messages it received. The same graph shape and
node count run (`seq`: three agents; `par`: a dispatcher, five workers and a summarizer). AutoGen's runtime is
`SingleThreadedAgentRuntime` on one event loop; it has no thread-pool regime.

A `GraphFlow` is built once, outside the timed region, and reused. `await flow.reset()` runs before every run, as
AutoGen's team documentation does before a new task; it clears the manager's message thread, and it belongs to the
timed call. A repeated run without it also returns the right answer (autogen-agentchat 0.7.5: the graph resets its
own execution state when it completes), and is not timed here.

Variants:

  reset_run    one `GraphFlow`; `await flow.reset(); await flow.run(task=...)` per call. Matched regime, single
               thread, and the form the AutoGen documentation shows for repeated runs.
  fresh_flow   a new agents + `GraphFlow` per call: the construction is part of the timed call. Idiomatic
               default for a one-off run; absolute times only. A team cannot run two tasks at once, so the
               in-flight cells build one team per run.
"""

PACKAGES = ["autogen-agentchat", "autogen-core"]

VARIANTS = {
    "reset_run": {"kind": "async", "gate": True},
    "fresh_flow": {"kind": "async", "gate": True},
}

_ns: dict = {}


def load() -> None:
    if _ns:
        return
    from autogen_agentchat.agents import BaseChatAgent
    from autogen_agentchat.base import Response
    from autogen_agentchat.messages import TextMessage
    from autogen_agentchat.teams import DiGraphBuilder, GraphFlow

    _ns.update(BaseChatAgent=BaseChatAgent, Response=Response, TextMessage=TextMessage,
               DiGraphBuilder=DiGraphBuilder, GraphFlow=GraphFlow)


def _agent_classes(gate=None):
    load()
    BaseChatAgent, Response, TextMessage = _ns["BaseChatAgent"], _ns["Response"], _ns["TextMessage"]

    class IncAgent(BaseChatAgent):
        def __init__(self, name, description, gated=False):
            super().__init__(name=name, description=description)
            self.gated = gated

        @property
        def produced_message_types(self):
            return (TextMessage,)

        async def on_messages(self, messages, cancellation_token):
            if self.gated and gate is not None:
                await gate.wait()
            last = None
            for m in reversed(messages):
                if isinstance(m, TextMessage):
                    last = m.content
                    break
            n = int(last) + 1 if last and last.lstrip("-").isdigit() else 1
            return Response(chat_message=TextMessage(source=self.name, content=str(n)))

        async def on_reset(self, cancellation_token):
            pass

    class Dispatch(BaseChatAgent):
        @property
        def produced_message_types(self):
            return (TextMessage,)

        async def on_messages(self, messages, cancellation_token):
            return Response(chat_message=TextMessage(source=self.name, content="go"))

        async def on_reset(self, cancellation_token):
            pass

    class WorkerAgent(BaseChatAgent):
        def __init__(self, name, idx):
            super().__init__(name=name, description=f"worker{idx}")
            self.idx = idx

        @property
        def produced_message_types(self):
            return (TextMessage,)

        async def on_messages(self, messages, cancellation_token):
            return Response(chat_message=TextMessage(source=self.name, content=str(self.idx)))

        async def on_reset(self, cancellation_token):
            pass

    class SummAgent(BaseChatAgent):
        @property
        def produced_message_types(self):
            return (TextMessage,)

        async def on_messages(self, messages, cancellation_token):
            if gate is not None:
                await gate.wait()  # parked after the fan-out has been collected
            workers = sorted(int(m.content) for m in messages
                             if isinstance(m, TextMessage) and m.source.startswith("w"))
            return Response(chat_message=TextMessage(
                source=self.name, content=",".join(str(w) for w in workers)))

        async def on_reset(self, cancellation_token):
            pass

    return IncAgent, Dispatch, WorkerAgent, SummAgent


def _build_flow(workload: str, gate=None):
    IncAgent, Dispatch, WorkerAgent, SummAgent = _agent_classes(gate)
    DiGraphBuilder, GraphFlow = _ns["DiGraphBuilder"], _ns["GraphFlow"]
    builder = DiGraphBuilder()
    if workload == "seq":
        a = IncAgent(name="a", description="inc")
        b = IncAgent(name="b", description="inc", gated=True)
        c = IncAgent(name="c", description="inc")
        builder.add_node(a).add_node(b).add_node(c)
        builder.add_edge(a, b).add_edge(b, c)
        builder.set_entry_point(a)
        return GraphFlow(participants=[a, b, c], graph=builder.build())
    d = Dispatch(name="d", description="disp")
    workers = [WorkerAgent(name=f"w{i}", idx=i) for i in range(1, 6)]
    s = SummAgent(name="s", description="summ")
    builder.add_node(d).add_node(s)
    for w in workers:
        builder.add_node(w)
        builder.add_edge(d, w)
        builder.add_edge(w, s)
    builder.set_entry_point(d)
    return GraphFlow(participants=[d, *workers, s], graph=builder.build())


def _task(workload: str) -> str:
    return "0" if workload == "seq" else "start"


def _final(result) -> str:
    return result.messages[-1].content


def build(variant: str, workload: str):
    if variant == "reset_run":
        flow = _build_flow(workload)

        async def call():
            await flow.reset()
            return _final(await flow.run(task=_task(workload)))
    else:
        async def call():
            flow = _build_flow(workload)
            return _final(await flow.run(task=_task(workload)))
    return call


def make_inflight(variant: str, workload: str, gate):
    def make():
        flow = _build_flow(workload, gate)

        async def call():
            return _final(await flow.run(task=_task(workload)))
        return call
    return make


def check(workload: str, result) -> None:
    if workload == "seq":
        assert result == "3", f"seq: expected the text '3', got {result!r}"
    else:
        assert result == "1,2,3,4,5", f"par: expected the text '1,2,3,4,5', got {result!r}"
