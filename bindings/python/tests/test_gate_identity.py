"""Each interrupt_before gate of a step is answered on its own (#396).

Two gated nodes ready in the same super-step used to be passed by one
resume: the second human approval was never asked. Now a resume answers the
gate the run paused at, the run pauses again before the other gated node,
and each gated node receives the answer to its own gate.
"""

import neograph_engine as ng


class Record(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        seen = input.ctx.resume_value
        return [ng.ChannelWrite("results", [f"{self._name}={seen}"])]


ng.NodeFactory.register_type("gates.record", lambda name, _config, _ctx: Record(name))

DEFINITION = {
    "name": "gate_identity",
    "channels": {"results": {"reducer": "append", "initial": []}},
    "nodes": {"a": {"type": "gates.record"}, "b": {"type": "gates.record"}},
    "edges": [{"from": "__start__", "to": "a"}, {"from": "__start__", "to": "b"},
              {"from": "a", "to": "__end__"}, {"from": "b", "to": "__end__"}],
    "interrupt_before": ["a", "b"],
}


def test_each_gate_of_a_step_is_answered_on_its_own(monkeypatch):
    monkeypatch.setenv("NEOGRAPH_SUPPRESS_FANOUT_WARNING", "1")
    engine = ng.GraphEngine.compile(DEFINITION, ng.NodeContext(), ng.InMemoryCheckpointStore())

    paused = engine.run(ng.RunConfig(thread_id="gates"))
    assert paused.interrupted and paused.interrupt_node == "a"

    again = engine.resume("gates", "approve a")
    assert again.interrupted, "one resume passed both gates"
    assert again.interrupt_node == "b"
    assert again.output["channels"]["results"]["value"] == [], "nothing ran yet"

    done = engine.resume("gates", "approve b")
    assert done.status == ng.RunStatus.Completed
    assert sorted(done.output["channels"]["results"]["value"]) == ["a=approve a", "b=approve b"]
