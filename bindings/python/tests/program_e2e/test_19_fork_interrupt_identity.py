"""Scenario 19: a fork answers the Core interrupt its pending call names (#394).

A Core super-step runs a bound llm_call next to two nodes that both ask, so
the Program pauses with a pending call that names the first Core interrupt
(``core_interrupt_id``). ``host.fork()`` starts a new run from that
checkpoint and answers that call: exactly the interrupt it names gets the
answer, as a resume of the source would, and the other node asks its own
question again in the fork. The finished llm_call replays in the fork, so its
request is never resent and its tokens and money are not charged again, in
memory and with the SQLite journal.
"""

import sys

import pytest

import neograph_engine as ng

from . import support
from .support import (JOURNAL, ChatPeer, admit, channel, define_graph, describe, make_bound_host,
                      priced_budget)

pytestmark = pytest.mark.skipif(not getattr(ng, "_HAVE_LLM", False),
                                reason="neograph_engine was built without the LLM target")

QUESTION = [{"role": "user", "content": "What is the capital of France?"}]


class Ask(ng.GraphNode):
    """Asks its own question until answered; records the answer it got."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        answer = input.ctx.resume_value
        if answer is None:
            JOURNAL.add("asked", self._name)
            raise ng.NodeInterrupt({"asks": self._name}, reason=f"approve {self._name}")
        JOURNAL.add("ran", self._name)
        return [ng.ChannelWrite("results", [f"{self._name}={answer}"])]


EXTRA_TYPES = ("e2e.fork_identity_ask",)
ng.NodeFactory.register_type("e2e.fork_identity_ask", lambda name, _c, _x: Ask(name))

TWO_QUESTIONS = define_graph("""
    g.channel("messages", {reducer: "append", initial: []});
    g.channel("results", {reducer: "append", initial: []});
    g.node("start", {type: "e2e.step"});
    g.node("llm", {type: "llm_call"});
    g.node("left", {type: "e2e.fork_identity_ask"});
    g.node("right", {type: "e2e.fork_identity_ask"});
    g.entry("start");
    g.edge("start", "llm");
    g.edge("start", "left");
    g.edge("start", "right");
    g.exit("llm");
    g.exit("left");
    g.exit("right");
""", name="two_questions")


@pytest.fixture
def peer():
    with ChatPeer() as running:
        yield running


@pytest.mark.parametrize("durable", [False, True], ids=["in-memory", "sqlite-journal"])
def test_a_fork_answers_the_core_interrupt_its_pending_call_names(peer, tmp_path, durable):
    if durable and sys.platform == "win32":
        pytest.skip("NativeArchive custody needs an owner-only protected directory")
    options = dict(extra_node_types=EXTRA_TYPES, worker_count=2)
    if durable:
        store = str(tmp_path / "runs")
        archive = support.provider_archive(store, peer.base_url, provision=True)
        options.update(sqlite_directory=store, native_history_archive=archive)
    host = make_bound_host(peer.base_url, **options)
    run_budget = priced_budget()
    version = admit(host, TWO_QUESTIONS, run_budget, "two_questions.js")

    paused = host.run(version, {"messages": QUESTION}, run_budget)
    assert paused.status == ng.ProgramTerminalStatus.Interrupted, describe(paused)
    assert paused.interrupt.pending_input.core_interrupt_id == "s1:left"
    assert peer.request_count() == 1

    forked = host.fork(paused.run_id, "L", version, paused.remaining_budget).wait()
    assert forked.status == ng.ProgramTerminalStatus.Interrupted, describe(forked)
    assert forked.run_id != paused.run_id
    assert forked.interrupt.core_node == "right", "left got the fork's answer; right asks again"
    assert forked.interrupt.value["value"] == {"asks": "right"}
    assert forked.interrupt.pending_input.core_interrupt_id == "", "one pending interrupt"
    assert peer.request_count() == 1, "the finished llm_call replays in the fork"
    assert forked.usage.model_tokens == 0

    done = host.resume(forked.run_id, "R").wait()
    assert done.status == ng.ProgramTerminalStatus.Completed, describe(done)
    assert sorted(channel(done, "results")) == ["left=L", "right=R"]
    assert peer.request_count() == 1, "nothing is resent"
    assert done.usage.model_tokens == 0
    assert JOURNAL.names().count("start") == 1, "the step's first node does not run again"
