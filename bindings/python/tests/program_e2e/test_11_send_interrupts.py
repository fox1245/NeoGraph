"""Scenario 11: a Send target that interrupts pauses the Program (#385).

A map node returns one ng.Send per item and the Send target raises
ng.NodeInterrupt for the item that needs an answer. The Core run used to raise
RuntimeError and leave no checkpoint, so the Program failed. It now pauses like
any other node: the Program stops Interrupted on the Send target, and resume
runs the Send that asked with the answer while the map node and every Send that
finished replay without running, or being charged, again.
"""

import sys

import pytest

import neograph_engine as ng

from . import support
from .support import (JOURNAL, ChatPeer, admit, budget, build_registry, channel, define_graph,
                      describe, make_bound_host, make_host, priced_budget)


class AskFan(ng.GraphNode):
    """Map step: one Send to "check" per entry of the "items" channel."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        JOURNAL.add("ran", self._name)
        return [ng.Send("check", {"item": item}) for item in (input.state.get("items") or [])]


class Check(ng.GraphNode):
    """Send target: the item named by the "topic" channel asks until answered."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        item = input.state.get("item")
        if input.state.get("topic") == item and input.ctx.resume_value is None:
            JOURNAL.add("asked", f"{self._name}:{item}")
            raise ng.NodeInterrupt({"item": item}, reason=f"approve {item}")
        JOURNAL.add("ran", f"{self._name}:{item}")
        answer = input.ctx.resume_value if input.state.get("topic") == item else None
        return [ng.ChannelWrite("results", [f"{item}={answer}" if answer else item])]


class CallAndAskFan(ng.GraphNode):
    """Map step: a Send to the bound llm_call node and one to "check"."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        JOURNAL.add("ran", self._name)
        question = [{"role": "user", "content": "What is the capital of France?"}]
        return [ng.Send("llm", {"messages": question}), ng.Send("check", {"item": "x"})]


EXTRA_TYPES = ("e2e.ask_fan", "e2e.check", "e2e.call_and_ask_fan")
ng.NodeFactory.register_type("e2e.ask_fan", lambda name, _c, _x: AskFan(name))
ng.NodeFactory.register_type("e2e.check", lambda name, _c, _x: Check(name))
ng.NodeFactory.register_type("e2e.call_and_ask_fan", lambda name, _c, _x: CallAndAskFan(name))

MAP = define_graph("""
    g.channel("items", {reducer: "overwrite", initial: []});
    g.channel("item", {reducer: "overwrite", initial: ""});
    g.channel("results", {reducer: "append", initial: []});
    g.node("fan", {type: "e2e.ask_fan"});
    g.node("check", {type: "e2e.check"});
    g.entry("fan");
    g.exit("check");
""", name="map")


def _paused(host, items, ask):
    run_budget = budget()
    version = admit(host, MAP, run_budget)
    paused = host.run(version, {"items": items, "topic": ask}, run_budget)
    assert paused.status == ng.ProgramTerminalStatus.Interrupted, describe(paused)
    return paused


def test_a_send_that_asks_pauses_the_program_and_resume_replays_the_rest():
    host = make_host(registry=build_registry(extra_node_types=EXTRA_TYPES))
    paused = _paused(host, ["a", "b", "c"], "b")

    assert paused.failure is None
    assert paused.interrupt.core_node == "check"
    assert paused.interrupt.value["value"] == {"item": "b"}
    assert paused.interrupt.value["reason"] == "approve b"
    assert paused.checkpoint is not None and paused.checkpoint.checkpoint_id
    assert JOURNAL.names("asked") == ["check:b"]
    assert sorted(JOURNAL.names()) == ["check:a", "check:c", "fan"]

    done = host.resume(paused.run_id, "yes").wait()

    assert done.status == ng.ProgramTerminalStatus.Completed, describe(done)
    assert channel(done, "results") == ["a", "b=yes", "c"]
    assert JOURNAL.names("asked") == ["check:b"], "the answered Send does not ask again"
    assert sorted(JOURNAL.names()) == ["check:a", "check:b", "check:c", "fan"], \
        "the map node and the Sends that finished replay; they do not run again"


def test_a_durable_host_resumes_a_paused_send_after_a_restart(tmp_path):
    directory = str(tmp_path / "runs")
    registry = build_registry(extra_node_types=EXTRA_TYPES)
    paused = _paused(make_host(registry=registry, sqlite_directory=directory), ["a", "b"], "a")
    assert JOURNAL.names("asked") == ["check:a"]

    restarted = make_host(registry=registry, sqlite_directory=directory)
    done = restarted.resume(paused.run_id, "ok").wait()

    assert done.status == ng.ProgramTerminalStatus.Completed, describe(done)
    assert channel(done, "results") == ["a=ok", "b"]
    assert sorted(JOURNAL.names()) == ["check:a", "check:b", "fan"]


# ---------------------------------------------------------------------------
# A bound provider: the Send that finished is charged once (#378, #372)
# ---------------------------------------------------------------------------

CALL_AND_ASK = define_graph("""
    g.channel("messages", {reducer: "append", initial: []});
    g.channel("item", {reducer: "overwrite", initial: ""});
    g.channel("results", {reducer: "append", initial: []});
    g.node("fan", {type: "e2e.call_and_ask_fan"});
    g.node("llm", {type: "llm_call"});
    g.node("check", {type: "e2e.check"});
    g.entry("fan");
    g.exit("llm");
    g.exit("check");
""", name="map")


@pytest.fixture
def peer():
    with ChatPeer() as running:
        yield running


@pytest.mark.skipif(not getattr(ng, "_HAVE_LLM", False),
                    reason="neograph_engine was built without the LLM target")
@pytest.mark.parametrize("durable", [False, True], ids=["in-memory", "sqlite-journal"])
def test_resume_never_resends_or_recharges_the_send_that_called_the_model(peer, tmp_path,
                                                                        durable):
    if durable and sys.platform == "win32":
        pytest.skip("NativeArchive custody needs an owner-only protected directory")
    options = dict(extra_node_types=EXTRA_TYPES)
    if durable:
        store = str(tmp_path / "runs")
        archive = support.provider_archive(store, peer.base_url, provision=True)
        options.update(sqlite_directory=store, native_history_archive=archive)
    host = make_bound_host(peer.base_url, **options)
    run_budget = priced_budget()
    version = admit(host, CALL_AND_ASK, run_budget, "map.js")

    paused = host.run(version, {"topic": "x"}, run_budget)

    assert paused.status == ng.ProgramTerminalStatus.Interrupted, describe(paused)
    assert paused.interrupt.core_node == "check"
    assert peer.request_count() == 1
    assert paused.usage.model_tokens == 24
    assert paused.usage.monetary_microunits == 1235

    done = host.resume(paused.run_id, "approved").wait()

    assert done.status == ng.ProgramTerminalStatus.Completed, describe(done)
    assert channel(done, "results") == ["x=approved"]
    assert "Paris" in str(channel(done, "messages"))
    assert peer.request_count() == 1, "the Send that called the model replays; nothing is resent"
    assert done.usage.model_tokens == 0
    assert done.usage.monetary_microunits == 0
    assert done.remaining_budget.model_tokens == 1_000_000 - 24
    assert done.remaining_budget.monetary_microunits == 1_000_000 - 1235
    assert JOURNAL.names().count("fan") == 1
