"""Scenario 17: an answer reaches only the question it answers (#393).

A bound llm_call runs, then ``first`` asks, then ``second`` asks in the next
Core super-step. The resume that answers ``first`` used to leave its value
visible to every later node of the resumed run, so ``second`` took
``first``'s answer as its own approval and never asked. Now ``second`` asks
its own question, and the next resume answers it. The finished llm_call is
never resent and its tokens and money are charged once, in memory and with
the SQLite journal.
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


EXTRA_TYPES = ("e2e.scope_ask",)
ng.NodeFactory.register_type("e2e.scope_ask", lambda name, _c, _x: Ask(name))

TWO_STEPS = define_graph("""
    g.channel("messages", {reducer: "append", initial: []});
    g.channel("results", {reducer: "append", initial: []});
    g.node("start", {type: "e2e.step"});
    g.node("llm", {type: "llm_call"});
    g.node("first", {type: "e2e.scope_ask"});
    g.node("second", {type: "e2e.scope_ask"});
    g.entry("start");
    g.edge("start", "llm");
    g.edge("llm", "first");
    g.edge("first", "second");
    g.exit("second");
""", name="two_steps")


@pytest.fixture
def peer():
    with ChatPeer() as running:
        yield running


@pytest.mark.parametrize("durable", [False, True], ids=["in-memory", "sqlite-journal"])
def test_a_later_step_asks_its_own_question(peer, tmp_path, durable):
    if durable and sys.platform == "win32":
        pytest.skip("NativeArchive custody needs an owner-only protected directory")
    options = dict(extra_node_types=EXTRA_TYPES)
    if durable:
        store = str(tmp_path / "runs")
        archive = support.provider_archive(store, peer.base_url, provision=True)
        options.update(sqlite_directory=store, native_history_archive=archive)
    host = make_bound_host(peer.base_url, **options)
    run_budget = priced_budget()
    version = admit(host, TWO_STEPS, run_budget, "two_steps.js")

    first = host.run(version, {"messages": QUESTION}, run_budget)
    assert first.status == ng.ProgramTerminalStatus.Interrupted, describe(first)
    assert first.interrupt.core_node == "first"
    assert peer.request_count() == 1
    assert first.usage.model_tokens == 24
    assert first.usage.monetary_microunits > 0

    second = host.resume(first.run_id, "F").wait()
    assert second.status == ng.ProgramTerminalStatus.Interrupted, \
        "second took first's answer as its own: " + describe(second)
    assert second.interrupt.core_node == "second"
    assert second.interrupt.value["value"] == {"asks": "second"}
    assert peer.request_count() == 1
    assert second.usage.model_tokens == 0
    assert second.usage.monetary_microunits == 0

    done = host.resume(second.run_id, "S").wait()
    assert done.status == ng.ProgramTerminalStatus.Completed, describe(done)
    assert channel(done, "results") == ["first=F", "second=S"]
    assert "Paris" in str(channel(done, "messages"))
    assert peer.request_count() == 1, "the finished llm_call replays; nothing is resent"
    assert done.usage.model_tokens == 0
    assert done.usage.monetary_microunits == 0
    assert done.remaining_budget.model_tokens == 1_000_000 - 24
    assert JOURNAL.names("asked") == ["first", "second"], "each asked exactly once"
    assert JOURNAL.names() == ["start", "first", "second"], \
        "start ran once, and each asking node once with its own answer"
