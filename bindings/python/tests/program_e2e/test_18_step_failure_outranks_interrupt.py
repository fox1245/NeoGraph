"""Scenario 18: a failure ends its step, whatever the index of a node that asks (#392).

One Core super-step runs a bound llm_call next to a node that asks and a node
that fails. The Program used to pause when the asking node came before the
failing one, and fail when it came after. It now fails either way, with the
failing node's classified failure, and no question is left pending. The model
call is sent and charged once, in memory and with the SQLite journal.
"""

import sys

import pytest

import neograph_engine as ng

from . import support
from .support import (JOURNAL, ChatPeer, admit, define_graph, describe, make_bound_host,
                      priced_budget)

pytestmark = pytest.mark.skipif(not getattr(ng, "_HAVE_LLM", False),
                                reason="neograph_engine was built without the LLM target")

QUESTION = [{"role": "user", "content": "What is the capital of France?"}]


def _graph(first, second):
    def spec(does):
        return '{type: "e2e.step", ask: "approve?"}' if does == "ask" else \
            '{type: "e2e.step", boom: true}'
    return define_graph(f"""
        g.channel("messages", {{reducer: "append", initial: []}});
        g.node("start", {{type: "e2e.step"}});
        g.node("llm", {{type: "llm_call"}});
        g.node("a_first", {spec(first)});
        g.node("b_second", {spec(second)});
        g.entry("start");
        g.edge("start", "llm");
        g.edge("start", "a_first");
        g.edge("start", "b_second");
        g.exit("llm");
        g.exit("a_first");
        g.exit("b_second");
    """, name="ask_and_fail")


@pytest.fixture
def peer():
    with ChatPeer() as running:
        yield running


@pytest.mark.parametrize("order", [("ask", "fail"), ("fail", "ask")],
                         ids=["ask-first", "fail-first"])
@pytest.mark.parametrize("durable", [False, True], ids=["in-memory", "sqlite-journal"])
def test_a_failure_fails_the_step_whatever_the_order(peer, tmp_path, durable, order):
    if durable and sys.platform == "win32":
        pytest.skip("NativeArchive custody needs an owner-only protected directory")
    options = dict(worker_count=2)
    if durable:
        store = str(tmp_path / "runs")
        archive = support.provider_archive(store, peer.base_url, provision=True)
        options.update(sqlite_directory=store, native_history_archive=archive)
    host = make_bound_host(peer.base_url, **options)
    run_budget = priced_budget()
    version = admit(host, _graph(*order), run_budget, "ask_and_fail.js")

    result = host.run(version, {"messages": QUESTION}, run_budget)

    failing = "a_first" if order[0] == "fail" else "b_second"
    assert result.status == ng.ProgramTerminalStatus.Failed, \
        "the step paused on the question and dropped the failure: " + describe(result)
    assert result.failure.code == "NG.TaskFailed"
    assert result.failure.core_node == failing
    assert result.interrupt is None, "no question is left pending"
    assert JOURNAL.names("raised") == [failing]
    assert peer.request_count() == 1, "the model call is sent once"
    assert result.usage.model_tokens == 24
