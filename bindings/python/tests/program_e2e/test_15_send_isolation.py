"""Scenario 15: a Send payload is visible only to its own Send, one Send or many (#389).

A map node returns one ng.Send per item; each payload carries the item, and
the first also a "secret". A step with one Send used to apply its payload to
the shared run state: the node after the Sends read it and it stayed in the
final state, while a step with several Sends kept every payload to its own
Send. Both now behave the same, live and after a resume.
"""

import pytest

import neograph_engine as ng

from .support import JOURNAL, admit, budget, build_registry, channel, define_graph, describe, make_host


class Spread(ng.GraphNode):
    """One Send to "work" per item; the first payload also carries a secret."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        items = input.state.get("items") or []
        return [ng.Send("work", {"item": item, **({"secret": f"s-{item}"} if index == 0 else {})})
                for index, item in enumerate(items)]


class Work(ng.GraphNode):
    """Reports what it sees; the item named by "topic" asks until answered."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        item = input.state.get("item")
        if input.state.get("topic") == item and input.ctx.resume_value is None:
            raise ng.NodeInterrupt({"item": item}, reason=f"approve {item}")
        JOURNAL.add("ran", f"work:{item}")
        return [ng.ChannelWrite("seen", [f"{item}:secret={input.state.get('secret')}"])]


class After(ng.GraphNode):
    """Runs in the step after the Sends and reports the shared state it reads."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        return [ng.ChannelWrite(
            "after", [f"item={input.state.get('item')},secret={input.state.get('secret')}"])]


EXTRA_TYPES = ("e2e.spread", "e2e.work", "e2e.after_sends")
ng.NodeFactory.register_type("e2e.spread", lambda name, _c, _x: Spread(name))
ng.NodeFactory.register_type("e2e.work", lambda name, _c, _x: Work(name))
ng.NodeFactory.register_type("e2e.after_sends", lambda name, _c, _x: After(name))

SPREAD = define_graph("""
    g.channel("items", {reducer: "overwrite", initial: []});
    g.channel("item", {reducer: "overwrite", initial: ""});
    g.channel("secret", {reducer: "overwrite", initial: ""});
    g.channel("seen", {reducer: "append", initial: []});
    g.channel("after", {reducer: "append", initial: []});
    g.node("spread", {type: "e2e.spread"});
    g.node("work", {type: "e2e.work"});
    g.node("after", {type: "e2e.after_sends"});
    g.entry("spread");
    g.edge("work", "after");
    g.exit("after");
""", name="spread")


def _expect_isolated(result, items):
    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    expected = [f"{item}:secret={'s-' + item if index == 0 else ''}" for index, item in enumerate(items)]
    assert sorted(channel(result, "seen")) == sorted(expected), "each Send sees its own payload only"
    assert channel(result, "after") == ["item=,secret="], "the next step sees no Send payload"
    assert channel(result, "item") == "" and channel(result, "secret") == ""


@pytest.mark.parametrize("items", [["a"], ["a", "b", "c"]], ids=["one-send", "three-sends"])
@pytest.mark.parametrize("workers", [1, 4])
def test_a_payload_is_visible_only_to_its_own_send(items, workers):
    host = make_host(registry=build_registry(extra_node_types=EXTRA_TYPES), worker_count=workers)
    run_budget = budget()
    version = admit(host, SPREAD, run_budget)
    _expect_isolated(host.run(version, {"items": items}, run_budget), items)


@pytest.mark.parametrize("items", [["a"], ["a", "b", "c"]], ids=["one-send", "three-sends"])
def test_a_resumed_send_step_keeps_the_payloads_isolated(items, tmp_path):
    host = make_host(registry=build_registry(extra_node_types=EXTRA_TYPES),
                     sqlite_directory=str(tmp_path / "runs"))
    run_budget = budget()
    version = admit(host, SPREAD, run_budget)
    paused = host.run(version, {"items": items, "topic": items[-1]}, run_budget)
    assert paused.status == ng.ProgramTerminalStatus.Interrupted, describe(paused)

    done = host.resume(paused.run_id, "yes").wait()
    _expect_isolated(done, items)
    assert JOURNAL.names().count(f"work:{items[0]}") == 1
