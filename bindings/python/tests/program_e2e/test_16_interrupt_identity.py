"""Scenario 16: two Core interrupts of one step are answered one at a time (#388).

A Core super-step runs a bound llm_call next to two nodes that both ask. The
Program pauses on the first question, and its pending input names that Core
interrupt (``core_interrupt_id``), because the step paused on two. One resume
used to answer both questions with the same value; now it answers the first,
the second asks its own question again, and the next resume answers it. The
finished llm_call replays throughout, so its request is never resent and its
tokens and money are charged once.
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


EXTRA_TYPES = ("e2e.identity_ask",)
ng.NodeFactory.register_type("e2e.identity_ask", lambda name, _c, _x: Ask(name))

TWO_QUESTIONS = define_graph("""
    g.channel("messages", {reducer: "append", initial: []});
    g.channel("results", {reducer: "append", initial: []});
    g.node("start", {type: "e2e.step"});
    g.node("llm", {type: "llm_call"});
    g.node("left", {type: "e2e.identity_ask"});
    g.node("right", {type: "e2e.identity_ask"});
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
def test_each_core_interrupt_gets_its_own_answer(peer, tmp_path, durable):
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

    first = host.run(version, {"messages": QUESTION}, run_budget)
    assert first.status == ng.ProgramTerminalStatus.Interrupted, describe(first)
    assert first.interrupt.core_node == "left"
    assert first.interrupt.pending_input.core_interrupt_id == "s1:left"
    assert sorted(JOURNAL.names("asked")) == ["left", "right"]
    assert peer.request_count() == 1
    assert first.usage.model_tokens == 24

    second = host.resume(first.run_id, "L").wait()
    assert second.status == ng.ProgramTerminalStatus.Interrupted, describe(second)
    assert second.interrupt.core_node == "right", "right asks its own question again"
    assert second.interrupt.value["value"] == {"asks": "right"}
    assert second.interrupt.pending_input.core_interrupt_id == "", "one pending interrupt"
    assert peer.request_count() == 1
    assert second.usage.model_tokens == 0

    done = host.resume(second.run_id, "R").wait()
    assert done.status == ng.ProgramTerminalStatus.Completed, describe(done)
    assert sorted(channel(done, "results")) == ["left=L", "right=R"]
    assert "Paris" in str(channel(done, "messages"))
    assert peer.request_count() == 1, "the finished llm_call replays; nothing is resent"
    assert done.usage.model_tokens == 0
    assert done.usage.monetary_microunits == 0
    assert done.remaining_budget.model_tokens == 1_000_000 - 24
    assert JOURNAL.names() == ["start", "left", "right"], \
        "start ran once, and each asking node once with its own answer"
