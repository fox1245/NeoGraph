"""Scenario 9: built-in llm_call nodes bound to a provider and priced (#372).

LocalProgramHost(node_context=ng.NodeContext(provider=..., model=...),
binding_label=...) binds one provider to the llm_call nodes a registry
declares with add_registered_node("llm_call", ..., provider=name). Every call
is priced from the provider policy's model catalogue against the run's
monetary_microunits budget: reserved before it is sent, refused (fail closed)
when it cannot be bounded or does not fit, and settled once from the reply.

The peer is a loopback Chat Completions server declared as an OpenRouter
origin, so no key and no paid call are involved. A reply with usage.cost
settles as `reported`; without it the catalogue `estimated` amount applies.
"""

import json
import os
import sys
from decimal import Decimal

import pytest

import neograph_engine as ng

from . import support
from .support import (ChatPeer, PRICED_MODEL, admit, budget, ceiling,
                      define_graph, describe, digest, last_json_line,
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


def priced_entry():
    return next(e for e in support.price_layer()["models"] if e["id"] == PRICED_MODEL)


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
    assert (record.origin, record.model, record.node) == (support.origin_of(peer.base_url), PRICED_MODEL, "llm")
    # The record still names the catalogue and rows its hold was priced with.
    assert record.catalog_version == "sp-model-catalogue/1" and len(record.catalog_digest) == 64
    assert record.pricing["entry"] == PRICED_MODEL
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
    assert record.catalog_version == "sp-model-catalogue/1"
    assert len(record.catalog_digest) == 64  # the provider policy's catalogue
    assert record.pricing["entry"] == PRICED_MODEL
    assert record.pricing["units"]["ceiling_input"] == priced_entry()["pricing"]["ceiling_input"]
    assert record.pricing["assumptions"] == ["route ceiling"]
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


def run_refused(peer, status):
    """One call whose reply is an HTTP error `status`, and the budget it ran under."""
    peer.refusals = {f"answer-{status}": status}
    host = make_bound_host(peer.base_url)
    run_budget = priced_budget()
    version = admit(host, CHAT, run_budget, "chat.js")
    question = {"messages": [{"role": "user", "content": f"answer-{status}: please"}]}
    return host.run(version, question, run_budget), run_budget


@pytest.mark.parametrize("status", [400, 401, 404])
def test_a_completed_4xx_answer_releases_the_hold_and_charges_nothing(peer, status):
    result, run_budget = run_refused(peer, status)

    assert result.status == ng.ProgramTerminalStatus.Failed, describe(result)
    assert peer.request_count() == 1
    # The provider declined the request, so nothing ran and nothing is billed:
    # neither the money hold nor the model-token hold stays, and no call is
    # recorded as charged.
    assert result.monetary_records == []
    assert result.usage.monetary_microunits == 0
    assert result.usage.model_tokens == 0
    assert result.remaining_budget.monetary_microunits == run_budget.monetary_microunits
    assert result.remaining_budget.model_tokens == run_budget.model_tokens


@pytest.mark.parametrize("status", [429, 503])
def test_an_answer_that_can_follow_started_work_keeps_the_hold(peer, status):
    result, run_budget = run_refused(peer, status)

    assert result.status == ng.ProgramTerminalStatus.Failed, describe(result)
    assert peer.request_count() == 1
    [record] = result.monetary_records
    assert record.evidence == "held"
    assert record.microunits == HOLD
    assert result.usage.monetary_microunits == HOLD
    assert result.usage.model_tokens == support.PRICED_INPUT_LIMIT + support.OUTPUT_CAP
    assert result.remaining_budget.monetary_microunits == run_budget.monetary_microunits - HOLD


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


def test_the_request_pins_the_route_ceiling_as_max_price(peer):
    _host, _version, result = run_bound(peer)
    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    [sent] = peer.requests
    # OpenRouter may route only to endpoints priced at or below the ceiling the
    # call was reserved at: the dearest input and output rate of the entry's
    # route ceiling, in USD per million tokens, sent as exact decimals.
    pricing = priced_entry()["pricing"]  # decimal USD per token
    prompt = Decimal(pricing["ceiling_input"]) * 1_000_000
    completion = Decimal(pricing["ceiling_output"]) * 1_000_000
    provider = json.loads(sent["raw_body"], parse_float=Decimal)["provider"]
    assert provider["max_price"] == {"prompt": prompt, "completion": completion}, provider
    assert (prompt, completion) == (Decimal("0.44"), Decimal("1.32"))
    assert b'"max_price":{"prompt":0.44,"completion":1.32}' in sent["raw_body"]
    assert provider["zdr"] is True  # the provider's routing defaults still apply
    [record] = result.monetary_records
    assert record.reserved_microunits == HOLD


def test_a_host_max_price_below_the_ceiling_is_kept(peer):
    routing = ng.OpenRouterRouting()
    price = ng.OpenRouterMaxPrice()
    price.prompt_nano_usd = 300_000_000        # $0.30, below the $0.44 ceiling
    price.completion_nano_usd = 2_000_000_000  # $2, above the $1.32 ceiling
    routing.max_price = price
    controls = support.chat_controls()
    controls.provider = routing
    _host, _version, result = run_bound(peer, controls=controls)
    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    [sent] = peer.requests
    provider = json.loads(sent["raw_body"], parse_float=Decimal)["provider"]
    assert provider["max_price"] == {"prompt": Decimal("0.3"), "completion": Decimal("1.32")}
    assert provider["zdr"] is True


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


# A resume value that feeds the request: Core appends it to "messages" as a
# user turn, so recovery with any other value would build a different request
# (#377).
ANSWER = "Answer in one word."

_CRASHING_WITH_A_RESUME_VALUE = _RECOVERY_SETUP + r'''
version = s.admit(host, TEXT, BUDGET, "chat.js")
paused = host.run(version, {"topic": "run input"}, BUDGET)
print(json.dumps({"status": str(paused.status), "run_id": paused.run_id}), flush=True)
os.environ["E2E_DIE"] = "1"
done = host.resume(paused.run_id, sys.argv[4]).wait()
print(json.dumps({"unreachable": s.describe(done)}), flush=True)
'''


@slow
@pytest.mark.skipif(sys.platform == "win32",
                    reason="NativeArchive custody needs an owner-only protected directory")
def test_recovery_of_a_resumed_attempt_rebuilds_the_journaled_request(peer, tmp_path):
    store = str(tmp_path / "runs")
    crashed = run_python(_CRASHING_WITH_A_RESUME_VALUE, peer.base_url, store, "first", ANSWER,
                         timeout=120)
    assert crashed.returncode == 9, (crashed.returncode, crashed.stdout, crashed.stderr[-2000:])
    lines = [json.loads(line) for line in crashed.stdout.splitlines() if line.startswith("{")]
    assert lines[0]["status"] == "ProgramTerminalStatus.Interrupted"
    assert lines[-1] == {"dying": True}
    [sent] = peer.requests
    assert ANSWER in json.dumps(sent["body"]["messages"][-1])

    recovered = run_python(_RECOVERING_PROCESS, peer.base_url, store, "again", lines[0]["run_id"],
                           timeout=120)
    assert recovered.returncode == 0, recovered.stderr[-2000:]
    outcome = last_json_line(recovered.stdout)
    # Recovery resumes with the same value, so the journal recognises the
    # request and replays its reply: no binding conflict, nothing resent.
    assert outcome["status"] == "ProgramTerminalStatus.Completed", outcome["failure"]
    assert outcome["attempt"] == 2
    assert peer.request_count() == 1, "the peer saw exactly the requests of an uncrashed run"
    assert outcome["records"] == [{"evidence": "reported", "microunits": 1235, "replayed": True}]
    assert outcome["monetary"] == 1235


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
    # The resumed turn resends the first turn's native history, which binds the
    # routing it was sent with, so it carries the same max_price pin.
    first, second = (request["body"]["provider"] for request in peer.requests)
    assert first["max_price"] == second["max_price"] == {"prompt": 0.44, "completion": 1.32}


# ---------------------------------------------------------------------------
# A stream cut in the middle is a provider failure that publishes (#426)
# ---------------------------------------------------------------------------

CUT_QUESTION = {"messages": [{"role": "user", "content": "cut-stream: capital of France?"}]}


def assert_cut_stream_failure(result):
    """A cut stream ends the run Failed with the provider's own error."""
    assert result.status == ng.ProgramTerminalStatus.Failed, describe(result)
    witness = result.failure.witness
    assert witness["provider_error"]["message"] == "response ended abnormally", witness
    return witness["provider_outcome"]


def test_a_cut_stream_on_the_in_memory_host_publishes_its_terminal_state(peer, capfd):
    host = make_bound_host(peer.base_url)
    run_budget = priced_budget()
    version = admit(host, CHAT, run_budget, "chat.js")

    result = host.run(version, CUT_QUESTION, run_budget)

    outcome = assert_cut_stream_failure(result)
    assert result.failure.code == "NG.ProviderFailed"
    assert peer.request_count() == 1
    # The failure keeps what the stream delivered, as portable evidence. The
    # host holds no native archive, so the provider's native replay state is
    # not kept, and the failure says so.
    assert outcome["kind"] == "failure"
    assert outcome["native_archive_reference"] is None
    assert "Par" in json.dumps(outcome["messages"])
    assert result.failure.witness["provider_outcome_native_retained"] is False
    assert result.failure.provider_outcome is not None
    # No exception was swallowed on the way to publishing it.
    assert "terminal publication exception" not in capfd.readouterr().err

    # The terminal state is published: the run can be reconnected and read.
    again = host.reconnect(result.run_id).wait()
    assert again.status == ng.ProgramTerminalStatus.Failed, describe(again)
    assert again.id == result.id
    assert again.failure.code == result.failure.code
    assert again.usage.model_tokens == result.usage.model_tokens
    assert again.usage.monetary_microunits == result.usage.monetary_microunits
    assert ng.ProgramResult.parse(result.serialize_canonical()).id == result.id
    # Reconnecting never sends the call again.
    assert peer.request_count() == 1


def test_a_cut_stream_on_the_in_memory_host_inside_main_publishes_its_terminal_state(peer, capfd):
    text = with_main(CHAT, """
        return yield ng.callCore("chat", input, "chat");
    """)
    host = make_bound_host(peer.base_url)
    run_budget = priced_budget()
    version = admit(host, text, run_budget, "chat.js")

    result = host.run(version, CUT_QUESTION, run_budget)

    assert_cut_stream_failure(result)
    assert "terminal publication exception" not in capfd.readouterr().err
    again = host.reconnect(result.run_id).wait()
    assert again.id == result.id


def test_a_cut_stream_on_a_host_with_an_archive_keeps_its_native_evidence(peer, tmp_path):
    if sys.platform == "win32":
        pytest.skip("NativeArchive custody needs an owner-only protected directory")
    host = durable_bound_host(peer, tmp_path)
    run_budget = priced_budget()
    version = admit(host, CHAT, run_budget, "chat.js")

    result = host.run(version, CUT_QUESTION, run_budget)

    outcome = assert_cut_stream_failure(result)
    # With custody the native replay state is archived, not dropped.
    assert outcome["native_archive_reference"], outcome
    assert "provider_outcome_native_retained" not in result.failure.witness
    again = host.reconnect(result.run_id).wait()
    assert again.id == result.id
    assert again.failure.provider_outcome is not None


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
    support.assert_started_bodies_saw_cancel("wait")


# ---------------------------------------------------------------------------
# A paused join charges a settled member's call once (#380)
# ---------------------------------------------------------------------------

ASK_OR_CALL = define_graph("""
    g.channel("messages", {reducer: "append", initial: []});
    g.node("route", {type: "e2e.step"});
    g.node("llm", {type: "llm_call"});
    g.node("confirm", {type: "e2e.step", ask: "ok?"});
    g.entry("route");
    g.conditionalEdge("route", "e2e.route_by_topic", {billing: "llm", general: "confirm"});
    g.exit("llm");
    g.exit("confirm");
""", name="chat")


@pytest.mark.parametrize("durable", [False, True], ids=["in-memory", "sqlite-journal"])
def test_resume_of_a_paused_join_never_resends_or_recharges_a_settled_call(peer, tmp_path,
                                                                         durable):
    # Before the fix, an in-memory host abandoned the member's call when the
    # other member asked (holding its reservation), then sent it again on resume.
    if durable and sys.platform == "win32":
        pytest.skip("NativeArchive custody needs an owner-only protected directory")
    text = with_main(ASK_OR_CALL, """
        const replies = yield ng.all([
          ng.callCore("chat", {topic: "billing", messages: [{role: "user", content: "a"}]}, "call"),
          ng.callCore("chat", {topic: "general"}, "confirm")
        ], {max_in_flight: 2}, "fan");
        return {reply: replies[0].messages[1].content, answer: replies[1].answer};
    """)
    host = (durable_bound_host(peer, tmp_path, scheduler_threads=2) if durable
            else make_bound_host(peer.base_url, scheduler_threads=2))
    run_budget = priced_budget(max_concurrency=2, max_program_operations=8)
    version = admit(host, text, run_budget, "chat.js")

    paused = host.run(version, {}, run_budget)

    assert paused.status == ng.ProgramTerminalStatus.Interrupted, describe(paused)
    assert paused.interrupt.core_node == "confirm"
    assert peer.request_count() == 1
    assert paused.usage.model_tokens == 24
    assert paused.usage.monetary_microunits == 1235

    done = host.resume(paused.run_id, {"ok": True}).wait()

    assert done.status == ng.ProgramTerminalStatus.Completed, describe(done)
    assert done.output == {"reply": "Paris.", "answer": {"ok": True}}
    assert peer.request_count() == 1, "the settled member is replayed, its call not resent"
    assert support.JOURNAL.names().count("route") == 2, "and its nodes do not run again"
    assert done.usage.model_tokens == 0
    assert done.usage.monetary_microunits == 0
    assert done.remaining_budget.model_tokens == 1_000_000 - 24
    assert done.remaining_budget.monetary_microunits == 1_000_000 - 1235


# ---------------------------------------------------------------------------
# An expired execution lease charges a journaled call's tokens with its money
# (#381)
# ---------------------------------------------------------------------------

_LEASE_SETUP = r'''
import json, os, sys, threading
import neograph_engine as ng
from program_e2e import support as s

base_url, store, first = sys.argv[1], sys.argv[2], sys.argv[3] == "first"
STARTED = threading.Event()


class DieOnce(ng.GraphNode):
    """Kills its process, once the run id is printed, in the crashing process."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        if os.environ.get("E2E_DIE") == "1":
            STARTED.wait(30)
            print(json.dumps({"dying": True}), flush=True)
            os._exit(9)
        return [ng.ChannelWrite("trace", [self._name])]


ng.NodeFactory.register_type("e2e.die_once", lambda name, config, ctx: DieOnce(name))
TEXT = s.define_graph("""
    g.channel("messages", {reducer: "append",
                           initial: [{role: "user", content: "What is the capital of France?"}]});
    g.node("llm", {type: "llm_call"});
    g.node("after", {type: "e2e.die_once"});
    g.entry("llm");
    g.edge("llm", "after");
    g.exit("after");
""", name="chat")
# Without child budgets the run holds a root execution lease for its wall time.
BUDGET = s.priced_budget(wall_time_ms=3_000)
archive = s.provider_archive(store, base_url, provision=first)
host = s.make_bound_host(base_url, sqlite_directory=store, extra_node_types=("e2e.die_once",),
                         native_history_archive=archive)
'''

_LEASE_CRASHING_PROCESS = _LEASE_SETUP + r'''
version = s.admit(host, TEXT, BUDGET, "chat.js")
os.environ["E2E_DIE"] = "1"
handle = host.start(version, {}, BUDGET)
print(json.dumps({"run_id": handle.run_id}), flush=True)
STARTED.set()
handle.wait()
print(json.dumps({"unreachable": True}), flush=True)
'''

_LEASE_EXPIRED_PROCESS = _LEASE_SETUP + r'''
import time
deadline = time.monotonic() + 30
while True:
    try:
        result = host.reconnect(sys.argv[4]).wait()
        break
    except RuntimeError as error:
        if "P_RUN_LEASE_HELD" not in str(error) or time.monotonic() > deadline:
            raise
        time.sleep(0.1)
print(json.dumps({
    "status": str(result.status), "code": result.failure.code if result.failure else None,
    "tokens": result.usage.model_tokens,
    "remaining_tokens": result.remaining_budget.model_tokens,
    "monetary": result.usage.monetary_microunits,
    "remaining_monetary": result.remaining_budget.monetary_microunits,
}), flush=True)
'''


@slow
@pytest.mark.skipif(sys.platform == "win32",
                    reason="NativeArchive custody needs an owner-only protected directory")
def test_an_expired_lease_charges_the_journaled_calls_tokens_with_its_money(peer, tmp_path):
    store = str(tmp_path / "runs")
    crashed = run_python(_LEASE_CRASHING_PROCESS, peer.base_url, store, "first", timeout=120)
    assert crashed.returncode == 9, (crashed.returncode, crashed.stdout, crashed.stderr[-2000:])
    lines = [json.loads(line) for line in crashed.stdout.splitlines() if line.startswith("{")]
    assert lines[-1] == {"dying": True}
    assert peer.request_count() == 1, "the model call happened before the crash"

    expired = run_python(_LEASE_EXPIRED_PROCESS, peer.base_url, store, "again",
                         lines[0]["run_id"], timeout=120)
    assert expired.returncode == 0, expired.stderr[-2000:]
    outcome = last_json_line(expired.stdout)
    assert outcome["status"] == "ProgramTerminalStatus.TimedOut", outcome
    assert outcome["code"] == "P_RUN_LEASE_EXPIRED"
    assert peer.request_count() == 1
    # Money was already charged on this path (#372); tokens are charged too.
    assert outcome["monetary"] == 1235
    assert outcome["remaining_monetary"] == 1_000_000 - 1235
    assert outcome["tokens"] == 24, outcome
    assert outcome["remaining_tokens"] == 1_000_000 - 24, outcome


# ---------------------------------------------------------------------------
# Exhausting the model-token budget ends a Program in one shape (#420)
#
# Either the reply of a call takes the run over its budget, or the next call
# cannot be reserved and is refused before it is sent. Both end the run
# BudgetExhausted, with a failure record that carries NG.BudgetExhausted.
# ---------------------------------------------------------------------------

TOKEN_BUDGET = 1_000
# A call reserves the model's input limit and the output cap against the token
# budget: 300 here, so a call is sent while 300 tokens are free.
TOKEN_INPUT_LIMIT = 100
TOKEN_OUTPUT_CAP = 200


def token_peer_usage(peer, total):
    peer.usage = {"prompt_tokens": total - 100, "completion_tokens": 100, "total_tokens": total,
                  "cost": 0.001}


def token_controls():
    controls = ng.ProviderControls()
    controls.max_output_tokens = TOKEN_OUTPUT_CAP
    return controls


def run_with_token_budget(peer, text):
    run_budget = priced_budget(model_tokens=TOKEN_BUDGET)
    return run_bound(peer, text, run_budget, controls=token_controls(),
                     input_limits={PRICED_MODEL: TOKEN_INPUT_LIMIT})


def llm_chain(count):
    """A define() graph that calls the model `count` times in a row."""
    names = [f"n{index}" for index in range(count)]
    nodes = "\n".join(f'g.node("{name}", {{type: "llm_call"}});' for name in names)
    edges = "\n".join(f'g.edge("{a}", "{b}");' for a, b in zip(names, names[1:]))
    return define_graph(f"""
        g.channel("messages", {{reducer: "append",
                                initial: [{{role: "user", content: "Name a river."}}]}});
        {nodes}
        g.entry("{names[0]}");
        {edges}
        g.exit("{names[-1]}");
    """, name="chain")


def llm_calls_from_main(count):
    """A main() that calls a one-node chat graph `count` times in a row."""
    calls = "\n".join(f'yield ng.callCore("chat", ask, "call{index}");' for index in range(count))
    return with_main(CHAT, f"""
        const ask = {{messages: [{{role: "user", content: "Name a river."}}]}};
        {calls}
        return "done";
    """)


TOKEN_SHAPES = {"define": llm_chain, "main": llm_calls_from_main}


@pytest.mark.parametrize("shape", sorted(TOKEN_SHAPES))
@pytest.mark.parametrize("replies, tokens, sent, used", [
    pytest.param(2, 600, 2, 1_200, id="the-last-reply-crosses-the-budget"),
    pytest.param(3, 400, 2, 800, id="the-next-call-is-refused-before-sending"),
])
def test_exhausting_the_model_token_budget_is_one_terminal_shape(peer, shape, replies, tokens,
                                                                 sent, used):
    token_peer_usage(peer, tokens)
    _host, _version, result = run_with_token_budget(peer, TOKEN_SHAPES[shape](replies))

    assert result.status == ng.ProgramTerminalStatus.BudgetExhausted, describe(result)
    assert result.failure is not None, "a budget stop carries its failure record"
    assert result.failure.code == "NG.BudgetExhausted", describe(result)
    assert result.failure.operation_id == "root" or result.failure.operation_id.startswith("root.")
    assert peer.request_count() == sent
    assert result.usage.model_tokens == used
    assert result.remaining_budget.model_tokens == max(TOKEN_BUDGET - used, 0)


def test_the_overshoot_record_says_how_far_the_last_reply_went_over(peer):
    token_peer_usage(peer, 600)
    _host, _version, result = run_with_token_budget(peer, llm_chain(2))

    assert result.status == ng.ProgramTerminalStatus.BudgetExhausted, describe(result)
    assert result.failure.witness["model_tokens"] == {
        "granted": TOKEN_BUDGET, "used": 1_200, "over_by": 200}
    assert "1200" in result.failure.message and str(TOKEN_BUDGET) in result.failure.message


# ---------------------------------------------------------------------------
# reconnect() of a run that holds an execution lease says why it is held (#421)
#
# A define()-only Program started without child budgets holds a root execution
# lease for its wall time, so that no other runtime continues a run whose owner
# may still be alive. The error names the lease and the two ways out: wait for
# it to expire (the run then ends TimedOut), or start the Program with child
# budgets, which holds no lease and recovers from the last checkpoint
# (test_a_call_journaled_before_a_crash_is_charged_once_after_recovery).
# ---------------------------------------------------------------------------

_LEASE_HELD_SETUP = _LEASE_SETUP.replace("wall_time_ms=3_000", "wall_time_ms=60_000")
_LEASE_HELD_CRASHING_PROCESS = _LEASE_HELD_SETUP + _LEASE_CRASHING_PROCESS[len(_LEASE_SETUP):]
_LEASE_HELD_RECONNECT = _LEASE_HELD_SETUP + r'''
try:
    host.reconnect(sys.argv[4])
except RuntimeError as error:
    print(json.dumps({"diagnostic": error.diagnostics[0]}, default=str), flush=True)
else:
    print(json.dumps({"reconnected": True}), flush=True)
'''


@pytest.mark.skipif(sys.platform == "win32",
                    reason="NativeArchive custody needs an owner-only protected directory")
def test_reconnect_of_a_leased_run_names_the_lease_and_the_child_budget_exit(peer, tmp_path):
    store = str(tmp_path / "runs")
    crashed = run_python(_LEASE_HELD_CRASHING_PROCESS, peer.base_url, store, "first", timeout=120)
    assert crashed.returncode == 9, (crashed.returncode, crashed.stdout, crashed.stderr[-2000:])
    lines = [json.loads(line) for line in crashed.stdout.splitlines() if line.startswith("{")]

    held = run_python(_LEASE_HELD_RECONNECT, peer.base_url, store, "again", lines[0]["run_id"],
                      timeout=120)
    assert held.returncode == 0, held.stderr[-2000:]
    diagnostic = last_json_line(held.stdout)["diagnostic"]
    assert diagnostic["code"] == "P_RUN_LEASE_HELD", diagnostic
    message = diagnostic["message"]
    assert "execution lease" in message, message
    assert "TimedOut" in message, message
    assert "max_child_depth" in message and "max_total_children" in message, message
    witness = diagnostic["witness"]
    assert witness["run_id"] == lines[0]["run_id"]
    assert 0 < witness["retry_after_ms"] <= 60_000, witness
    assert witness["expires_at_ms"] > 0
    assert peer.request_count() == 1, "the held run was not dispatched again"


# ---------------------------------------------------------------------------
# A run that finishes over its money grant ends with the record of that budget (#443)
#
# A reply can report a cost above the reservation made for its call (a BYOK
# upstream charge, a fee that max_price does not cap). The call was sent
# within the budget, so the run completes, and finishes over its grant. It ends
# BudgetExhausted with P_MONEY_BUDGET, the code of a call that does not fit,
# and failure.witness["monetary"] holds granted, used and over_by.
# ---------------------------------------------------------------------------

OVERSHOOT = 500


def reported_cost(microunits):
    """A reply cost in USD that settles as `microunits`: the half microunit rounds up."""
    return (microunits - 0.5) / 1_000_000


def test_a_reported_cost_above_the_money_grant_ends_the_run_with_the_record(peer):
    peer.usage = {"prompt_tokens": 21, "completion_tokens": 3, "total_tokens": 24,
                  "cost": reported_cost(HOLD + OVERSHOOT)}
    run_budget = priced_budget(monetary_microunits=HOLD)
    _host, _version, result = run_bound(peer, run_budget=run_budget)

    assert result.status == ng.ProgramTerminalStatus.BudgetExhausted, describe(result)
    assert result.failure is not None, "a money overshoot carries its failure record"
    assert result.failure.code == "P_MONEY_BUDGET"
    assert result.failure.witness["monetary"] == {
        "granted": HOLD, "used": HOLD + OVERSHOOT, "over_by": OVERSHOOT}
    assert "model_tokens" not in result.failure.witness
    assert str(HOLD + OVERSHOOT) in result.failure.message
    assert peer.request_count() == 1
    assert result.usage.monetary_microunits == HOLD + OVERSHOOT
    assert result.remaining_budget.monetary_microunits == 0
    [record] = result.monetary_records
    assert record.microunits == HOLD + OVERSHOOT


def test_a_reported_cost_within_the_money_grant_leaves_no_failure(peer):
    peer.usage = {"prompt_tokens": 21, "completion_tokens": 3, "total_tokens": 24,
                  "cost": reported_cost(HOLD)}
    _host, _version, result = run_bound(peer, run_budget=priced_budget(monetary_microunits=HOLD))

    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert result.failure is None
    assert result.usage.monetary_microunits == HOLD


@pytest.mark.parametrize("calls", [
    pytest.param(2, id="the-last-reply-takes-the-run-over"),
    pytest.param(3, id="the-run-stops-before-the-third-call"),
])
def test_a_run_over_its_tokens_and_its_money_names_both(peer, calls):
    """Each reply is 600 tokens and costs hold + OVERSHOOT microunits.

    The budget is 1,000 tokens and 2 * hold + OVERSHOOT microunits, where hold is the
    reservation of one call. A call is sent while its reservation fits, so the second
    call is sent, and its reply takes the run over both budgets. The code is the first
    budget's in the order of the budget fields (tokens before money), and the witness
    has an entry for each.
    """
    hold = route_ceiling_hold(input_tokens=TOKEN_INPUT_LIMIT, output_tokens=TOKEN_OUTPUT_CAP)
    cost = hold + OVERSHOOT
    peer.usage = {"prompt_tokens": 500, "completion_tokens": 100, "total_tokens": 600,
                  "cost": reported_cost(cost)}
    money = 2 * hold + OVERSHOOT
    run_budget = priced_budget(model_tokens=TOKEN_BUDGET, monetary_microunits=money)
    _host, _version, result = run_bound(
        peer, llm_chain(calls), run_budget, controls=token_controls(),
        input_limits={PRICED_MODEL: TOKEN_INPUT_LIMIT})

    assert result.status == ng.ProgramTerminalStatus.BudgetExhausted, describe(result)
    assert result.failure.code == "NG.BudgetExhausted", describe(result)
    assert peer.request_count() == 2
    assert result.failure.witness["model_tokens"] == {
        "granted": TOKEN_BUDGET, "used": 1_200, "over_by": 200}
    assert result.failure.witness["monetary"] == {
        "granted": money, "used": 2 * cost, "over_by": OVERSHOOT}
