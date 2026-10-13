"""Scenario 22: a fork says when its host cannot replay provider calls (#431).

A fork replays the provider calls of the paused invocation from a durable
provider-call journal (#405). An in-memory host keeps none, so a node that runs
again in the fork would send and charge its earlier calls again. The handle that
host.fork() returns carries a notice for that: `handle.notices` holds one
`P_FORK_PROVIDER_CALLS_NOT_JOURNALED` when the host has no journal and the paused
attempt recorded provider calls in the paused operation. The notice is not an
error and does not change what the fork sends or charges.

The Core step here runs a bound llm_call node next to a node that asks, so the
call finished before the pause and the fork replays it from the step's saved
writes (#390) on either host; the notice lists the recorded call and cannot say
whether the fork runs it again. A Python node cannot make a provider call and
ask in one node, which is the case a fork sends again; the C++ test
ProgramForkNotice.AForkWithoutAProviderJournalSendsTheCallAgainAndSaysSo covers it.
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
CODE = "P_FORK_PROVIDER_CALLS_NOT_JOURNALED"


class Approve(ng.GraphNode):
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
        return [ng.ChannelWrite("results", [f"x={answer}"])]


EXTRA_TYPES = ("e2e.notice_approve",)
ng.NodeFactory.register_type("e2e.notice_approve", lambda name, _c, _x: Approve(name))

# llm and approve are ready in one step after "start".
WITH_A_CALL = define_graph("""
    g.channel("messages", {reducer: "append", initial: []});
    g.channel("results", {reducer: "append", initial: []});
    g.node("start", {type: "e2e.step"});
    g.node("llm", {type: "llm_call"});
    g.node("approve", {type: "e2e.notice_approve"});
    g.entry("start");
    g.edge("start", "llm");
    g.edge("start", "approve");
    g.exit("llm");
    g.exit("approve");
""", name="with_a_call")

# Only the node that asks: the run makes no provider call.
WITHOUT_A_CALL = define_graph("""
    g.channel("results", {reducer: "append", initial: []});
    g.node("approve", {type: "e2e.notice_approve"});
    g.entry("approve");
    g.exit("approve");
""", name="without_a_call")


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


def _skip_unless_archive_custody_works(durable):
    if durable and sys.platform == "win32":
        pytest.skip("NativeArchive custody needs an owner-only protected directory")


def test_a_fork_on_an_in_memory_host_names_the_calls_it_cannot_replay(peer, tmp_path):
    host = _host(peer, tmp_path, durable=False)
    run_budget = priced_budget()
    version = admit(host, WITH_A_CALL, run_budget, "notice.js")
    paused = host.run(version, {"messages": QUESTION}, run_budget)
    assert paused.status == ng.ProgramTerminalStatus.Interrupted, describe(paused)
    assert peer.request_count() == 1

    handle = host.fork(paused.run_id, "approved", version, support.fork_share(paused.remaining_budget))
    forked = handle.wait()

    # The notice is information: the fork completes and sends and charges what
    # it would have without it.
    assert forked.status == ng.ProgramTerminalStatus.Completed, describe(forked)
    assert channel(forked, "results") == ["x=approved"]
    assert peer.request_count() == 1, "the call finished in the paused step, so the fork replays it"
    assert forked.usage.model_tokens == 0 and forked.usage.monetary_microunits == 0

    [notice] = handle.notices
    assert isinstance(notice, ng.ProgramNotice)
    assert notice.code == CODE
    assert "durable provider-call journal" in notice.message
    witness = notice.witness
    assert witness["source_run_id"] == paused.run_id
    assert witness["source_attempt"] == paused.attempt
    assert witness["operation_id"] == paused.interrupt.pending_input.operation_id
    [call] = witness["calls"]
    assert call["node"] == "llm"
    assert call["model"] == support.PRICED_MODEL
    assert call["microunits"] == paused.usage.monetary_microunits == 1235
    assert call["call_id"] == paused.monetary_records[0].call_id
    assert witness["microunits"] == 1235
    assert handle.notices == handle.notices, "reading the notices again gives the same list"


def test_a_fork_on_a_host_with_a_journal_has_no_notice_and_one_request_in_all(peer, tmp_path):
    _skip_unless_archive_custody_works(True)
    host = _host(peer, tmp_path, durable=True)
    run_budget = priced_budget()
    version = admit(host, WITH_A_CALL, run_budget, "notice.js")
    paused = host.run(version, {"messages": QUESTION}, run_budget)
    assert paused.status == ng.ProgramTerminalStatus.Interrupted, describe(paused)

    handle = host.fork(paused.run_id, "approved", version, support.fork_share(paused.remaining_budget))
    forked = handle.wait()

    assert forked.status == ng.ProgramTerminalStatus.Completed, describe(forked)
    assert channel(forked, "results") == ["x=approved"]
    assert peer.request_count() == 1
    assert forked.usage.model_tokens == 0 and forked.usage.monetary_microunits == 0
    assert handle.notices == []


@pytest.mark.parametrize("durable", [False, True], ids=["in-memory", "sqlite-journal"])
def test_a_fork_of_a_run_that_made_no_provider_call_has_no_notice(peer, tmp_path, durable):
    _skip_unless_archive_custody_works(durable)
    host = _host(peer, tmp_path, durable)
    run_budget = priced_budget()
    version = admit(host, WITHOUT_A_CALL, run_budget, "notice.js")
    paused = host.run(version, {}, run_budget)
    assert paused.status == ng.ProgramTerminalStatus.Interrupted, describe(paused)
    assert paused.monetary_records == []

    handle = host.fork(paused.run_id, "approved", version, support.fork_share(paused.remaining_budget))

    assert handle.wait().status == ng.ProgramTerminalStatus.Completed
    assert handle.notices == []
    assert peer.request_count() == 0


def test_only_fork_gives_a_notice(peer, tmp_path):
    host = _host(peer, tmp_path, durable=False)
    run_budget = priced_budget()
    version = admit(host, WITH_A_CALL, run_budget, "notice.js")

    started = host.start(version, {"messages": QUESTION}, run_budget)
    paused = started.wait()
    assert paused.status == ng.ProgramTerminalStatus.Interrupted, describe(paused)
    assert started.notices == []

    resumed = host.resume(paused.run_id, "approved")
    assert resumed.wait().status == ng.ProgramTerminalStatus.Completed
    assert resumed.notices == []
