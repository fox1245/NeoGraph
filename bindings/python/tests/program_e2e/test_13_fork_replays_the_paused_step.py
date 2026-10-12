"""Scenario 13: a fork replays what the paused step finished (#390).

A Core super-step runs a bound llm_call node next to a node that asks, so the
Program pauses after the model call has finished. host.fork() starts a new run
from that checkpoint and answers the question. The fork used to copy the
checkpoint without the step's pending writes, so the finished llm_call ran
again in the new run: its request was sent and charged a second time. The fork
now carries those writes and replays the finished work exactly as resume does.
"""

import sys

import pytest

import neograph_engine as ng

from . import support
from .support import (JOURNAL, ChatPeer, admit, channel, define_graph, describe,
                      make_bound_host, priced_budget)

pytestmark = pytest.mark.skipif(not getattr(ng, "_HAVE_LLM", False),
                                reason="neograph_engine was built without the LLM target")

QUESTION = [{"role": "user", "content": "What is the capital of France?"}]


class ForkFan(ng.GraphNode):
    """Map step: a Send to the bound llm_call node and one to "check"."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        JOURNAL.add("ran", self._name)
        return [ng.Send("llm", {"messages": QUESTION}), ng.Send("check", {"item": "x"})]


class ForkCheck(ng.GraphNode):
    """Asks until it is answered."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        answer = input.ctx.resume_value
        if answer is None:
            JOURNAL.add("asked", self._name)
            raise ng.NodeInterrupt({"item": "x"}, reason="approve x")
        JOURNAL.add("ran", self._name)
        return [ng.ChannelWrite("results", [f"x={answer}"])]


EXTRA_TYPES = ("e2e.fork_fan", "e2e.fork_check")
ng.NodeFactory.register_type("e2e.fork_fan", lambda name, _c, _x: ForkFan(name))
ng.NodeFactory.register_type("e2e.fork_check", lambda name, _c, _x: ForkCheck(name))

# llm and check are ready in one step after "start".
FAN_OUT = define_graph("""
    g.channel("messages", {reducer: "append", initial: []});
    g.channel("results", {reducer: "append", initial: []});
    g.node("start", {type: "e2e.step"});
    g.node("llm", {type: "llm_call"});
    g.node("check", {type: "e2e.fork_check"});
    g.entry("start");
    g.edge("start", "llm");
    g.edge("start", "check");
    g.exit("llm");
    g.exit("check");
""", name="fan_out")

# llm and check are Sends of the entry step.
SENDS = define_graph("""
    g.channel("messages", {reducer: "append", initial: []});
    g.channel("item", {reducer: "overwrite", initial: ""});
    g.channel("results", {reducer: "append", initial: []});
    g.node("fan", {type: "e2e.fork_fan"});
    g.node("llm", {type: "llm_call"});
    g.node("check", {type: "e2e.fork_check"});
    g.entry("fan");
    g.exit("llm");
    g.exit("check");
""", name="sends")


@pytest.fixture
def peer():
    with ChatPeer() as running:
        yield running


def _host(peer, tmp_path, durable):
    options = dict(extra_node_types=EXTRA_TYPES, worker_count=2)
    if durable:
        store = str(tmp_path / "runs")
        archive = support.provider_archive(store, peer.base_url, provision=True)
        options.update(sqlite_directory=store, native_history_archive=archive)
    return make_bound_host(peer.base_url, **options)


@pytest.mark.parametrize("durable", [False, True], ids=["in-memory", "sqlite-journal"])
@pytest.mark.parametrize("graph, first_node", [(FAN_OUT, "start"), (SENDS, "fan")],
                         ids=["fan-out", "sends"])
def test_a_fork_never_resends_or_recharges_the_call_its_step_finished(peer, tmp_path, durable,
                                                                     graph, first_node):
    if durable and sys.platform == "win32":
        pytest.skip("NativeArchive custody needs an owner-only protected directory")
    host = _host(peer, tmp_path, durable)
    run_budget = priced_budget()
    version = admit(host, graph, run_budget, "fork.js")

    paused = host.run(version, {"messages": QUESTION}, run_budget)

    assert paused.status == ng.ProgramTerminalStatus.Interrupted, describe(paused)
    assert paused.interrupt.core_node == "check"
    assert peer.request_count() == 1
    assert paused.usage.model_tokens == 24
    assert paused.usage.monetary_microunits == 1235

    forked = host.fork(paused.run_id, "approved", version, paused.remaining_budget).wait()

    assert forked.status == ng.ProgramTerminalStatus.Completed, describe(forked)
    assert forked.run_id != paused.run_id
    assert channel(forked, "results") == ["x=approved"]
    assert "Paris" in str(channel(forked, "messages")), "the fork keeps the finished reply"
    assert peer.request_count() == 1, "the finished call replays in the fork; nothing is resent"
    assert forked.usage.model_tokens == 0
    assert forked.usage.monetary_microunits == 0
    assert JOURNAL.names().count(first_node) == 1, "the step's first node does not run again"
    assert JOURNAL.names("asked") == ["check"]

