"""A step ends by one rule, whatever the index of the sibling that failed (#392).

In a fan-out step, or among the Sends of a step, a node that asks next to a
node that fails used to pause the run when the asking node came first and
fail it when the failing node did. The failure now fails the run either way:
the step's ordinary interrupts are not reported and nothing waits for an
answer. Only an uncertain effect (a reconciliation interrupt) still pauses
next to a failure, so that it can be reconciled.
"""

import pytest

import neograph_engine as ng


class Act(ng.GraphNode):
    def __init__(self, name, does):
        super().__init__()
        self._name = name
        self._does = does

    def get_name(self):
        return self._name

    def run(self, input):
        if self._does == "ask" and input.ctx.resume_value is None:
            raise ng.NodeInterrupt({"asks": self._name}, reason=f"approve {self._name}")
        if self._does == "fail":
            raise RuntimeError(f"boom {self._name}")
        return [ng.ChannelWrite("results", [self._name])]


class Fan(ng.GraphNode):
    def get_name(self):
        return "fan"

    def run(self, input):
        return [ng.Send("worker", {"item": item}) for item in input.state.get("items")]


class Worker(ng.GraphNode):
    def get_name(self):
        return "worker"

    def run(self, input):
        item = input.state.get("item")
        return Act(item, item.split(":")[1]).run(input)


ng.NodeFactory.register_type("outcome.act", lambda name, config, _ctx: Act(name, config["does"]))
ng.NodeFactory.register_type("outcome.fan", lambda _name, _config, _ctx: Fan())
ng.NodeFactory.register_type("outcome.worker", lambda _name, _config, _ctx: Worker())

CHANNELS = {"results": {"reducer": "append", "initial": []},
            "items": {"reducer": "overwrite", "initial": []},
            "item": {"reducer": "overwrite", "initial": ""}}


def _fan_out(first, second):
    return {"name": "outcome_fan_out", "channels": CHANNELS,
            "nodes": {"n1": {"type": "outcome.act", "does": first},
                      "n2": {"type": "outcome.act", "does": second}},
            "edges": [{"from": "__start__", "to": "n1"}, {"from": "__start__", "to": "n2"},
                      {"from": "n1", "to": "__end__"}, {"from": "n2", "to": "__end__"}]}


SENDS = {"name": "outcome_sends", "channels": CHANNELS,
         "nodes": {"fan": {"type": "outcome.fan"}, "worker": {"type": "outcome.worker"}},
         "edges": [{"from": "__start__", "to": "fan"}, {"from": "fan", "to": "__end__"},
                   {"from": "worker", "to": "__end__"}]}

ORDERS = [("ask", "fail"), ("fail", "ask")]


@pytest.fixture(autouse=True)
def _quiet(monkeypatch):
    monkeypatch.setenv("NEOGRAPH_SUPPRESS_FANOUT_WARNING", "1")


@pytest.mark.parametrize("order", ORDERS, ids=["ask-first", "fail-first"])
@pytest.mark.parametrize("workers", [1, 4])
def test_a_failing_branch_fails_the_step_whatever_its_index(order, workers):
    store = ng.InMemoryCheckpointStore()
    engine = ng.GraphEngine.compile(_fan_out(*order), ng.NodeContext(), store)
    engine.set_worker_count(workers)
    with pytest.raises(Exception, match="boom n"):
        engine.run(ng.RunConfig(thread_id="fan-out"))


@pytest.mark.parametrize("order", ORDERS, ids=["ask-first", "fail-first"])
@pytest.mark.parametrize("workers", [1, 4])
def test_a_failing_send_fails_the_step_whatever_its_index(order, workers):
    store = ng.InMemoryCheckpointStore()
    engine = ng.GraphEngine.compile(SENDS, ng.NodeContext(), store)
    engine.set_worker_count(workers)
    items = [f"{index + 1}:{does}" for index, does in enumerate(order)]
    with pytest.raises(Exception, match="boom"):
        engine.run(ng.RunConfig(thread_id="sends", input={"items": items}))
