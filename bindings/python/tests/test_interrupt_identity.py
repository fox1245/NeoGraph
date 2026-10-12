"""Several NodeInterrupts in one super-step are answered one at a time (#388).

Two nodes of one step ask. The paused RunResult lists both in
``result.interrupts`` (each ``{"id", "node", "value"}``) and reports the first
through ``interrupt_node``, ``interrupt_value`` and ``interrupt_id``. A resume
answers the interrupt that ``interrupt_id=`` names, or the first one; the other
node runs without the value and asks its own question again. One resume used
to hand the same value to both.
"""

import asyncio

import pytest

import neograph_engine as ng


class Ask(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        if input.ctx.resume_value is None:
            raise ng.NodeInterrupt({"asks": self._name}, reason=f"approve {self._name}")
        return [ng.ChannelWrite("results", [f"{self._name}={input.ctx.resume_value}"])]


ng.NodeFactory.register_type("identity.ask", lambda name, _config, _ctx: Ask(name))

DEFINITION = {
    "name": "interrupt_identity",
    "channels": {"results": {"reducer": "append", "initial": []}},
    "nodes": {"left": {"type": "identity.ask"}, "right": {"type": "identity.ask"}},
    "edges": [{"from": "__start__", "to": "left"}, {"from": "__start__", "to": "right"},
              {"from": "left", "to": "__end__"}, {"from": "right", "to": "__end__"}],
}


@pytest.fixture
def engine(monkeypatch):
    monkeypatch.setenv("NEOGRAPH_SUPPRESS_FANOUT_WARNING", "1")
    return ng.GraphEngine.compile(DEFINITION, ng.NodeContext(), ng.InMemoryCheckpointStore())


def _results(result):
    return sorted(result.output["channels"]["results"]["value"])


def test_the_paused_result_lists_every_interrupt_of_the_step(engine):
    paused = engine.run(ng.RunConfig(thread_id="list"))
    assert paused.interrupted
    assert paused.interrupt_id == "s0:left"
    assert [entry["id"] for entry in paused.interrupts] == ["s0:left", "s0:right"]
    assert [entry["node"] for entry in paused.interrupts] == ["left", "right"]
    assert paused.interrupts[1]["value"]["value"] == {"asks": "right"}
    assert paused.interrupts[0]["value"] == paused.interrupt_value


def test_each_resume_answers_one_interrupt_and_the_other_asks_again(engine):
    paused = engine.run(ng.RunConfig(thread_id="turns"))
    again = engine.resume("turns", "L")
    assert again.interrupted
    assert again.interrupt_node == "right"
    assert again.interrupt_id == "s0:right"
    assert again.interrupt_value["value"] == {"asks": "right"}
    done = engine.resume("turns", "R")
    assert done.status == ng.RunStatus.Completed
    assert done.interrupts == []
    assert _results(done) == ["left=L", "right=R"]


def test_resume_answers_the_interrupt_it_names(engine):
    paused = engine.run(ng.RunConfig(thread_id="named"))
    again = engine.resume("named", "R", interrupt_id=paused.interrupts[1]["id"])
    assert again.interrupt_node == "left"
    config = ng.RunConfig(thread_id="named")
    done = engine.resume_from(config, again.checkpoint_id, "L", interrupt_id="s0:left")
    assert _results(done) == ["left=L", "right=R"]


def test_an_interrupt_that_is_not_pending_is_rejected(engine):
    engine.run(ng.RunConfig(thread_id="unknown"))
    with pytest.raises(ValueError, match="not pending"):
        engine.resume("unknown", "x", interrupt_id="s0:nobody")
    assert engine.resume("unknown", "x").interrupted, "the run is still resumable"


def test_resume_async_answers_the_interrupt_it_names(engine):
    async def scenario():
        await engine.run_async(ng.RunConfig(thread_id="async"))
        again = await engine.resume_async("async", "R", interrupt_id="s0:right")
        assert again.interrupt_node == "left"
        return await engine.resume_async("async", "L")

    done = asyncio.run(scenario())
    assert _results(done) == ["left=L", "right=R"]


def test_run_metadata_carries_the_interrupt_a_resume_answers():
    metadata = ng.RunMetadata()
    assert metadata.resume_interrupt_id == ""
    metadata.resume_interrupt_id = "s0:right"
    assert metadata.resume_interrupt_id == "s0:right"
