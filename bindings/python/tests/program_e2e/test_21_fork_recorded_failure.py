"""Scenario 21: a fork replays a recorded provider failure under its own custody (#398).

main() runs a collecting ng.all of two callCores. The first member's bound
llm_call is refused by the peer, so it fails with the provider's outcome as
evidence; the second member's gate asks. The join waits for the paused
member, so the run pauses with the failed member recorded under the source
run's custody. A fork of the paused run replays that record (#395): it used
to restore the evidence under the fork's own custody, which the record does
not match, so the fork failed. It now re-binds the record to the fork: the
gate gets the fork's answer, the join reports the recorded failure with its
provider evidence, and neither call is sent again, in memory and with the
SQLite journal.
"""

import sys

import pytest

import neograph_engine as ng

from . import support
from .support import (JOURNAL, ChatPeer, admit, define_graph, describe, make_bound_host,
                      priced_budget, with_main)

pytestmark = pytest.mark.skipif(not getattr(ng, "_HAVE_LLM", False),
                                reason="neograph_engine was built without the LLM target")

QUESTION = [{"role": "user", "content": "What is the capital of France?"}]
REFUSED = [{"role": "user", "content": "fail-call: please refuse this one"}]


class Gate(ng.GraphNode):
    """Asks for approval when the call's phase is "ask"; records the answer."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        if input.state.get("phase") != "ask":
            return [ng.ChannelWrite("answer", "passed")]
        answer = input.ctx.resume_value
        if answer is None:
            JOURNAL.add("asked", self._name)
            raise ng.NodeInterrupt({"asks": self._name}, reason="approve the reply")
        JOURNAL.add("ran", self._name)
        return [ng.ChannelWrite("answer", answer)]


EXTRA_TYPES = ("e2e.fork_failure_gate",)
ng.NodeFactory.register_type("e2e.fork_failure_gate", lambda name, _c, _x: Gate(name))

TEXT = with_main(define_graph("""
    g.channel("messages", {reducer: "append", initial: []});
    g.channel("phase", {reducer: "overwrite", initial: ""});
    g.node("llm", {type: "llm_call"});
    g.node("gate", {type: "e2e.fork_failure_gate"});
    g.entry("llm");
    g.edge("llm", "gate");
    g.exit("gate");
""", name="chat"), """
    const results = yield ng.all([
      ng.callCore("chat", {messages: input.refused, phase: "warm"}, "refused"),
      ng.callCore("chat", {messages: input.messages, phase: "ask"}, "asks")
    ], {max_in_flight: 2, collect: true}, "collect");
    return results.map((r) => r && r.answer);
""")


@pytest.fixture
def peer():
    with ChatPeer() as running:
        yield running


@pytest.mark.parametrize("durable", [False, True], ids=["in-memory", "sqlite-journal"])
def test_a_fork_replays_a_recorded_provider_failure(peer, tmp_path, durable):
    if durable and sys.platform == "win32":
        pytest.skip("NativeArchive custody needs an owner-only protected directory")
    options = dict(extra_node_types=EXTRA_TYPES, scheduler_threads=2)
    if durable:
        store = str(tmp_path / "runs")
        archive = support.provider_archive(store, peer.base_url, provision=True)
        options.update(sqlite_directory=store, native_history_archive=archive)
    host = make_bound_host(peer.base_url, **options)
    run_budget = priced_budget(max_concurrency=2, max_program_operations=8)
    version = admit(host, TEXT, run_budget, "fork_failure.js")

    paused = host.run(version, {"messages": QUESTION, "refused": REFUSED}, run_budget)
    assert paused.status == ng.ProgramTerminalStatus.Interrupted, describe(paused)
    assert paused.interrupt.core_node == "gate"
    assert peer.request_count() == 2

    share = support.fork_share(paused.remaining_budget)
    forked = host.fork(paused.run_id, "approved", version, share).wait()
    assert forked.status == ng.ProgramTerminalStatus.Failed, describe(forked)
    assert "custody" not in forked.failure.message, \
        "the fork could not replay the recorded failure: " + describe(forked)
    assert forked.failure.code == "NG.ConfigError", describe(forked)
    assert forked.failure.provider_outcome is not None, "the recorded evidence is restored"
    assert JOURNAL.names() == ["gate"], "the gate gets the fork's answer"
    assert peer.request_count() == 2, "neither call is sent again"
