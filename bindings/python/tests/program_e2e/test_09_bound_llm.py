"""Scenario 9: built-in llm_call nodes bound to a provider and priced (#372).

LocalProgramHost(node_context=ng.NodeContext(provider=..., model=...),
binding_label=...) binds one provider to the llm_call nodes a registry
declares with add_registered_node("llm_call", ..., provider=name). Every call
is priced from the repository's price catalog against the run's
monetary_microunits budget: reserved before it is sent, refused (fail closed)
when it cannot be bounded or does not fit, and settled once from the reply.

The peer is a loopback Chat Completions server declared as an OpenRouter
origin, so no key and no paid call are involved. A reply with usage.cost
settles as `reported`; without it the catalog `estimated` amount applies.
"""

import json
import os
import sys

import pytest

import neograph_engine as ng

from . import support
from .support import (ChatPeer, CATALOG_PATH, PRICED_MODEL, admit, budget, ceiling,
                      define_graph, describe, digest, known_issue, last_json_line,
                      make_bound_host, make_host, priced_budget, route_ceiling_hold,
                      run_python, slow, with_main)

pytestmark = pytest.mark.skipif(not getattr(ng, "_HAVE_LLM", False),
                                reason="neograph_engine was built without the LLM target")

CHAT = define_graph("""
    g.channel("messages", {reducer: "append", initial: []});
    g.node("llm", {type: "llm_call"});
    g.entry("llm");
    g.exit("llm");
""", name="chat")

QUESTION = {"messages": [{"role": "user", "content": "What is the capital of France?"}]}
HOLD = route_ceiling_hold()


@pytest.fixture
def peer():
    with ChatPeer() as running:
        yield running


def catalog():
    with open(CATALOG_PATH, encoding="utf-8") as handle:
        return json.load(handle)


def run_bound(peer, text=CHAT, run_budget=None, **host_options):
    host = make_bound_host(peer.base_url, **host_options)
    run_budget = run_budget or priced_budget()
    version = admit(host, text, run_budget, "chat.js")
    return host, version, host.run(version, QUESTION, run_budget)


def last_reply(result):
    messages = (result.output.get("channels", {}).get("messages") or {}).get("value") or []
    return messages[-1] if messages else None


def test_a_bound_llm_call_settles_the_reported_cost(peer):
    _host, _version, result = run_bound(peer)

    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert "Paris" in json.dumps(last_reply(result))
    assert result.usage.model_tokens == 24
    assert result.usage.monetary_microunits == 1235  # usage.cost 0.001234567 USD, rounded up
    assert result.remaining_budget.monetary_microunits == 1_000_000 - 1235
    [record] = result.monetary_records
    assert record.evidence == "reported"
    assert record.microunits == 1235
    assert record.reserved_microunits == HOLD
    assert record.basis == "provider_reported"
    assert (record.origin, record.model, record.node) == ("openrouter", PRICED_MODEL, "llm")
    assert record.catalog_version == "" and record.catalog_digest == ""
    assert not record.replayed
    [sent] = peer.requests
    body = sent["body"]
    assert body["model"] == PRICED_MODEL
    assert body["max_tokens"] == support.OUTPUT_CAP
    assert body.get("provider", {}).get("zdr") is True, body


def test_without_a_reported_cost_the_catalog_estimate_names_its_catalog(peer):
    peer.usage = {"prompt_tokens": 21, "completion_tokens": 3, "total_tokens": 24}
    _host, _version, result = run_bound(peer)

    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    [record] = result.monetary_records
    assert record.evidence == "estimated"
    # 21 input tokens at 0.44 and 3 output tokens at 1.32 USD per million
    # (the route ceiling), rounded up once: 13.2 -> 14 microunits.
    assert record.microunits == 14
    assert record.basis == "route_ceiling"
    assert record.catalog_version == catalog()["version"]
    assert record.catalog_digest.startswith("sha256:")
    assert result.usage.monetary_microunits == 14


def test_a_ceiling_too_small_for_the_reservation_is_refused_before_sending(peer):
    run_budget = priced_budget(monetary_microunits=HOLD - 1)
    _host, _version, result = run_bound(peer, run_budget=run_budget)

    assert result.status == ng.ProgramTerminalStatus.BudgetExhausted, describe(result)
    assert result.failure.code == "P_MONEY_BUDGET"
    assert result.failure.witness["monetary"]["reserved_microunits"] == HOLD
    assert peer.request_count() == 0
    assert result.usage.monetary_microunits == 0
    assert result.monetary_records == []


def test_zero_monetary_budget_grants_nothing(peer):
    # Like every RunBudget amount, monetary_microunits = 0 grants nothing.
    _host, _version, result = run_bound(peer, run_budget=priced_budget(monetary_microunits=0))

    assert result.status == ng.ProgramTerminalStatus.BudgetExhausted, describe(result)
    assert result.failure.code == "P_MONEY_BUDGET"
    assert peer.request_count() == 0


def test_an_unknown_model_fails_closed(peer):
    model = "vendor/unlisted-model"
    _host, _version, result = run_bound(peer, model=model,
                                        input_limits={model: support.PRICED_INPUT_LIMIT})

    assert result.status == ng.ProgramTerminalStatus.Failed, describe(result)
    assert result.failure.code == "P_MONEY_UNKNOWN_MODEL"
    assert result.failure.witness["monetary"]["model"] == model
    assert peer.request_count() == 0


def test_a_router_alias_without_a_route_ceiling_fails_closed(peer):
    model = "~deepseek/deepseek-v4-flash-latest"
    _host, _version, result = run_bound(peer, model=model,
                                        input_limits={model: support.PRICED_INPUT_LIMIT})

    assert result.status == ng.ProgramTerminalStatus.Failed, describe(result)
    assert result.failure.code == "P_MONEY_NO_UPPER_BOUND"
    assert peer.request_count() == 0


def test_a_program_without_a_provider_keeps_its_ids(peer):
    # The binding changes nothing for a Program whose graph calls no model.
    run_budget = budget()
    plain = make_host("tenant:ids", registry=support.build_priced_registry(),
                      budget_ceiling=support.PRICED_CEILING)
    bound = make_bound_host(peer.base_url, owner="tenant:ids")
    expected = admit(plain, support.TRIAGE, run_budget)
    version = admit(bound, support.TRIAGE, run_budget)
    assert version.bundle_id == expected.bundle_id
    assert version.id == expected.id
    # Registries without provider entries keep every pinned id: see
    # test_identity_golden.py, which this change leaves untouched.


def test_without_node_context_a_provider_program_is_rejected_at_admission(peer):
    host = make_host("tenant:unbound", registry=support.build_priced_registry(),
                     budget_ceiling=support.PRICED_CEILING)
    with pytest.raises(ng.ProgramAdmissionError) as raised:
        admit(host, CHAT, priced_budget())
    assert "P_ADMIT_PROVIDER" in [d["code"] for d in raised.value.diagnostics]


def test_all_reserves_every_member_against_one_budget(peer):
    text = with_main(CHAT, """
        const ask = (topic) => ({messages: [{role: "user", content: topic}]});
        const replies = yield ng.all([
          ng.callCore("chat", ask("a"), "a"),
          ng.callCore("chat", ask("b"), "b"),
          ng.callCore("chat", ask("c"), "c")
        ], {max_in_flight: 3}, "fan");
        return replies.length;
    """)
    run_budget = priced_budget(max_concurrency=3, max_program_operations=8)
    _host, _version, result = run_bound(peer, text, run_budget, scheduler_threads=3)

    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert result.output == 3
    assert peer.request_count() == 3
    assert len({r.operation_id for r in result.monetary_records}) == 3  # one per member
    assert result.usage.monetary_microunits == 3 * 1235
    assert result.remaining_budget.monetary_microunits == 1_000_000 - 3 * 1235


def test_all_refuses_the_member_that_does_not_fit(peer):
    text = with_main(CHAT, """
        const ask = (topic) => ({messages: [{role: "user", content: topic}]});
        return (yield ng.all([
          ng.callCore("chat", ask("a"), "a"),
          ng.callCore("chat", ask("b"), "b")
        ], {max_in_flight: 2}, "fan")).length;
    """)
    run_budget = priced_budget(max_concurrency=2, max_program_operations=8,
                               monetary_microunits=HOLD + HOLD // 2)
    _host, _version, result = run_bound(peer, text, run_budget, scheduler_threads=2)

    # The member that does not fit is refused before sending, and the join
    # reports that refusal, never the sibling it cancelled (#379).
    assert result.status == ng.ProgramTerminalStatus.BudgetExhausted, describe(result)
    assert result.failure.code == "P_MONEY_BUDGET"
    assert peer.request_count() <= 1
    assert result.usage.monetary_microunits <= run_budget.monetary_microunits


@slow
def test_quorum_charges_the_cancelled_member_its_hold(peer):
    text = with_main(CHAT, """
        const ask = (topic) => ({messages: [{role: "user", content: topic}]});
        const ok = yield ng.quorum([
          ng.callCore("chat", ask("slow"), "slow"),
          ng.callCore("chat", ask("f1"), "f1"),
          ng.callCore("chat", ask("f2"), "f2")
        ], {required_successes: 2, max_in_flight: 3}, "quorum");
        return ok.length;
    """)
    run_budget = priced_budget(max_concurrency=3, max_program_operations=8)
    _host, _version, result = run_bound(peer, text, run_budget, scheduler_threads=3)

    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert result.output == 2
    assert peer.request_count() == 3
    # The slow member's request was sent and then abandoned: it keeps its hold.
    assert result.usage.monetary_microunits == 2 * 1235 + HOLD
    evidence = sorted(r.evidence for r in result.monetary_records)
    assert evidence in (["reported", "reported"], ["held", "reported", "reported"]), evidence


@known_issue("#372", "the pinned SchemaProvider's OpenRouterRouting has no max_price, so the "
                     "binding cannot pin the route ceiling on the request yet")
def test_the_request_pins_the_route_ceiling_as_max_price(peer):
    _host, _version, result = run_bound(peer)
    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    [sent] = peer.requests
    max_price = sent["body"].get("provider", {}).get("max_price")
    assert max_price, sent["body"]
    assert float(max_price["prompt"]) == 0.44
    assert float(max_price["completion"]) == 1.32


# ---------------------------------------------------------------------------
# Crash and resume: the SQLite provider journal charges a call once
# ---------------------------------------------------------------------------

_RECOVERY_SETUP = r'''
import json, os, sys
import neograph_engine as ng
from program_e2e import support as s

base_url, store, first = sys.argv[1], sys.argv[2], sys.argv[3] == "first"


class DieOnce(ng.GraphNode):
    """Kills its process the first time it runs in the crashing process."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        if os.environ.get("E2E_DIE") == "1":
            print(json.dumps({"dying": True}), flush=True)
            os._exit(9)
        return [ng.ChannelWrite("trace", [self._name])]


ng.NodeFactory.register_type("e2e.die_once", lambda name, config, ctx: DieOnce(name))
# The question is the channel's initial value, and the run input and the
# resume value are empty, so the recovered attempt rebuilds the same request.
TEXT = s.define_graph("""
    g.channel("messages", {reducer: "append",
                           initial: [{role: "user", content: "What is the capital of France?"}]});
    g.node("llm", {type: "llm_call"});
    g.node("after", {type: "e2e.die_once"});
    g.entry("llm");
    g.edge("llm", "after");
    g.exit("after");
    g.interruptBefore("llm");
""", name="chat")
# Child budgets make the run recover from its checkpoint after a crash.
CEILING = s.ceiling(model_tokens=10_000_000, monetary_microunits=10_000_000,
                    max_child_depth=1, max_total_children=1)
BUDGET = s.priced_budget(max_child_depth=1, max_total_children=1)
# The journal keeps provider outcomes under the host's native custody.
archive = s.provider_archive(store, base_url, provision=first)
host = s.make_bound_host(base_url, sqlite_directory=store, budget_ceiling=CEILING,
                         extra_node_types=("e2e.die_once",), native_history_archive=archive)
'''

_CRASHING_PROCESS = _RECOVERY_SETUP + r'''
version = s.admit(host, TEXT, BUDGET, "chat.js")
paused = host.run(version, {}, BUDGET)
print(json.dumps({"status": str(paused.status), "run_id": paused.run_id}), flush=True)
os.environ["E2E_DIE"] = "1"
done = host.resume(paused.run_id, {}).wait()
print(json.dumps({"unreachable": s.describe(done)}), flush=True)
'''

_RECOVERING_PROCESS = _RECOVERY_SETUP + r'''
run_id = sys.argv[4]
result = host.reconnect(run_id).wait()
print(json.dumps({
    "status": str(result.status), "attempt": result.attempt,
    "monetary": result.usage.monetary_microunits,
    "remaining": result.remaining_budget.monetary_microunits,
    "records": [{"evidence": r.evidence, "microunits": r.microunits, "replayed": r.replayed}
                for r in result.monetary_records],
    "trace": result.execution_trace,
    "failure": s.describe(result),
}), flush=True)
'''


@slow
@pytest.mark.skipif(sys.platform == "win32",
                    reason="NativeArchive custody needs an owner-only protected directory")
def test_a_call_journaled_before_a_crash_is_charged_once_after_recovery(peer, tmp_path):
    store = str(tmp_path / "runs")
    crashed = run_python(_CRASHING_PROCESS, peer.base_url, store, "first", timeout=120)
    assert crashed.returncode == 9, (crashed.returncode, crashed.stdout, crashed.stderr[-2000:])
    lines = [json.loads(line) for line in crashed.stdout.splitlines() if line.startswith("{")]
    assert lines[0]["status"] == "ProgramTerminalStatus.Interrupted"
    assert lines[-1] == {"dying": True}
    assert peer.request_count() == 1, "the model call happened before the crash"

    recovered = run_python(_RECOVERING_PROCESS, peer.base_url, store, "again", lines[0]["run_id"],
                           timeout=120)
    assert recovered.returncode == 0, recovered.stderr[-2000:]
    outcome = last_json_line(recovered.stdout)
    assert outcome["status"] == "ProgramTerminalStatus.Completed", outcome["failure"]
    assert outcome["attempt"] == 2
    assert peer.request_count() == 1, "recovery replays the journaled reply, it never resends"
    assert outcome["records"] == [{"evidence": "reported", "microunits": 1235, "replayed": True}]
    assert outcome["monetary"] == 1235, "charged once, and not lost"
    assert outcome["remaining"] == 1_000_000 - 1235


# ---------------------------------------------------------------------------
# Interrupt and resume: each provider call's tokens are charged once (#378)
# ---------------------------------------------------------------------------

TWO_CALLS = define_graph("""
    g.channel("messages", {reducer: "append", initial: []});
    g.node("first", {type: "llm_call"});
    g.node("second", {type: "llm_call"});
    g.entry("first");
    g.edge("first", "second");
    g.interruptBefore("second");
    g.exit("second");
""", name="chat")


def durable_bound_host(peer, directory, **options):
    store = str(directory / "runs")
    archive = support.provider_archive(store, peer.base_url, provision=True)
    return make_bound_host(peer.base_url, sqlite_directory=store,
                           native_history_archive=archive, **options)


@pytest.mark.parametrize("durable", [False, True], ids=["in-memory", "sqlite-journal"])
def test_interrupt_and_resume_charge_each_call_once(peer, tmp_path, durable):
    if durable and sys.platform == "win32":
        pytest.skip("NativeArchive custody needs an owner-only protected directory")
    host = durable_bound_host(peer, tmp_path) if durable else make_bound_host(peer.base_url)
    run_budget = priced_budget()
    version = admit(host, TWO_CALLS, run_budget, "chat.js")

    paused = host.run(version, QUESTION, run_budget)
    assert paused.status == ng.ProgramTerminalStatus.Interrupted, describe(paused)
    assert paused.usage.model_tokens == 24
    assert paused.remaining_budget.model_tokens == 1_000_000 - 24

    done = host.resume(paused.run_id, "And of Italy?").wait()

    assert done.status == ng.ProgramTerminalStatus.Completed, describe(done)
    assert peer.request_count() == 2
    # The resumed attempt is charged for its own call only: the first call was
    # settled by the attempt that made it.
    assert done.usage.model_tokens == 24, describe(done)
    assert done.remaining_budget.model_tokens == 1_000_000 - 2 * 24
    assert done.usage.monetary_microunits == 1235
    assert done.remaining_budget.monetary_microunits == 1_000_000 - 2 * 1235


# ---------------------------------------------------------------------------
# A join member refused for money is the join's failure (#379)
# ---------------------------------------------------------------------------

ROUTED = define_graph("""
    g.channel("messages", {reducer: "append", initial: []});
    g.node("route", {type: "e2e.step"});
    g.node("llm", {type: "llm_call"});
    g.node("wait", {type: "e2e.step", delay_ms: 5000});
    g.entry("route");
    g.conditionalEdge("route", "e2e.route_by_topic", {billing: "llm", general: "wait"});
    g.exit("llm");
    g.exit("wait");
""", name="chat")


def test_a_refused_member_is_the_join_failure_not_the_sibling_it_cancelled(peer):
    text = with_main(ROUTED, """
        yield ng.all([
          ng.callCore("chat", {topic: "general"}, "wait"),
          ng.callCore("chat", {topic: "billing",
                               messages: [{role: "user", content: "a"}]}, "ask")
        ], {max_in_flight: 2}, "fan");
        return {unreachable: true};
    """)
    run_budget = priced_budget(max_concurrency=2, max_program_operations=8,
                               monetary_microunits=HOLD - 1)
    _host, _version, result = run_bound(peer, text, run_budget, scheduler_threads=2)

    assert result.status == ng.ProgramTerminalStatus.BudgetExhausted, describe(result)
    assert result.failure.code == "P_MONEY_BUDGET", describe(result)
    assert peer.request_count() == 0
    assert support.JOURNAL.names("cancelled") == ["wait"]
