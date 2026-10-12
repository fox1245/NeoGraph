"""Scenario 20: a fork of a JavaScript main() continues its paused callCore (#395).

main() calls the Core twice. The first callCore runs a bound llm_call and
finishes; the second runs it again and then a node asks. host.fork() of the
paused run used to start main() over: both model calls were sent and charged
again, the node asked its question again and the fork's answer was lost. The
fork now continues like a resume of the source: the finished callCore replays
from the command journal, the paused one resumes from its checkpoint with the
fork's answer, and no request is sent again, in memory and with the SQLite
journal.
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


class Gate(ng.GraphNode):
    """Asks for approval when the call's phase is "ask"; records the answer."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        if input.state.get("phase") != "ask":
            JOURNAL.add("ran", "passed")
            return [ng.ChannelWrite("answer", "passed")]
        answer = input.ctx.resume_value
        if answer is None:
            JOURNAL.add("asked", self._name)
            raise ng.NodeInterrupt({"asks": self._name}, reason="approve the reply")
        JOURNAL.add("ran", self._name)
        return [ng.ChannelWrite("answer", answer)]


EXTRA_TYPES = ("e2e.fork_js_gate",)
ng.NodeFactory.register_type("e2e.fork_js_gate", lambda name, _c, _x: Gate(name))

TEXT = with_main(define_graph("""
    g.channel("messages", {reducer: "append", initial: []});
    g.channel("phase", {reducer: "overwrite", initial: ""});
    g.node("llm", {type: "llm_call"});
    g.node("gate", {type: "e2e.fork_js_gate"});
    g.entry("llm");
    g.edge("llm", "gate");
    g.exit("gate");
""", name="chat"), """
    const warm = yield ng.callCore("chat", {messages: input.messages, phase: "warm"}, "warm");
    const asked = yield ng.callCore("chat", {messages: input.messages, phase: "ask"}, "ask");
    return {warm: warm.answer, asked: asked.answer};
""")


@pytest.fixture
def peer():
    with ChatPeer() as running:
        yield running


@pytest.mark.parametrize("durable", [False, True], ids=["in-memory", "sqlite-journal"])
def test_a_fork_continues_the_paused_call_core(peer, tmp_path, durable):
    if durable and sys.platform == "win32":
        pytest.skip("NativeArchive custody needs an owner-only protected directory")
    options = dict(extra_node_types=EXTRA_TYPES)
    if durable:
        store = str(tmp_path / "runs")
        archive = support.provider_archive(store, peer.base_url, provision=True)
        options.update(sqlite_directory=store, native_history_archive=archive)
    host = make_bound_host(peer.base_url, **options)
    run_budget = priced_budget(max_program_operations=8)
    version = admit(host, TEXT, run_budget, "fork_main.js")

    paused = host.run(version, {"messages": QUESTION}, run_budget)
    assert paused.status == ng.ProgramTerminalStatus.Interrupted, describe(paused)
    assert paused.interrupt.core_node == "gate"
    assert peer.request_count() == 2
    assert JOURNAL.names("asked") == ["gate"]

    share = support.fork_share(paused.remaining_budget)
    forked = host.fork(paused.run_id, "approved", version, share).wait()
    assert forked.status == ng.ProgramTerminalStatus.Completed, \
        "the fork started main() over: " + describe(forked)
    assert forked.run_id != paused.run_id
    assert forked.output == {"warm": "passed", "asked": "approved"}
    assert peer.request_count() == 2, "neither model call is sent again"
    assert forked.usage.model_tokens == 0
    assert forked.usage.monetary_microunits == 0
    assert JOURNAL.names("asked") == ["gate"], "the fork's answer reaches the paused call"
    assert JOURNAL.names() == ["passed", "gate"], "the finished callCore replays"
