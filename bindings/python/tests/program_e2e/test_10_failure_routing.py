"""Scenario 10: failure routing, per-node retry and the retry ceiling (#364).

The design's program: TEST fails, is retried twice, and its failure is routed
to ASK_USER (interruptBefore) instead of failing the run. The human answers
"retry" (after a fix) or "abort"; ABORT is a failure exit, so an abort ends the
run Failed with the declared code, never Completed.
"""

import json

import neograph_engine as ng

from .support import (FAILURE_CONDITIONS, FAILURE_NODE_TYPES, JOURNAL, budget, build_registry,
                      channel, define_graph, describe, last_json_line, make_host, run_python,
                      slow, source)

_FAILURE_CHANNELS = """
    g.channel("failures", {reducer: "append", initial: []});
    g.channel("decision", {reducer: "overwrite", initial: ""});
    g.channel("fixed", {reducer: "overwrite", initial: false});
"""

_FAST = "initial_delay_ms: 10, backoff_multiplier: 1, max_delay_ms: 10"


def fix_loop(test_config="{}", policy=""):
    """IMPLEMENT -> TEST (exit); TEST fails to ASK_USER; ASK_USER -> IMPLEMENT or ABORT."""
    extra = f", {policy}" if policy else ""
    return define_graph(_FAILURE_CHANNELS + f"""
    g.node("IMPLEMENT", {{type: "e2e.step"}});
    g.node("TEST", Object.assign({{type: "e2e.fix_test"}}, {test_config}));
    g.node("ASK_USER", {{type: "e2e.review"}});
    g.node("ABORT", {{type: "e2e.step"}});
    g.entry("IMPLEMENT");
    g.edge("IMPLEMENT", "TEST");
    g.exit("TEST");
    g.conditionalEdge("ASK_USER", "e2e.decision", {{retry: "IMPLEMENT", abort: "ABORT"}});
    g.retryPolicy("TEST", {{max_retries: 2, {_FAST}, non_retryable: ["app.compile_error"],
                          max_visits: 3, on_failure: "ASK_USER", error_channel: "failures"{extra}}});
    g.interruptBefore("ASK_USER");
    g.failureExit("ABORT", {{code: "plan.user_abort"}});
""", name="fix")


FIX_LOOP = fix_loop()


def failure_registry():
    return build_registry(extra_node_types=FAILURE_NODE_TYPES,
                          extra_conditions=tuple(FAILURE_CONDITIONS))


def failure_host(**kwargs):
    return make_host(registry=failure_registry(), **kwargs)


def start(text, run_budget=None, host=None):
    host = host or failure_host()
    run_budget = run_budget or budget()
    version = host.compile_admit(source(text), run_budget)
    return host, host.run(version, {}, run_budget)


def failure_record(result):
    """The record a failure route carried into the ASK_USER interrupt."""
    assert result.status == ng.ProgramTerminalStatus.Interrupted, describe(result)
    assert result.interrupt.core_node == "ASK_USER"
    value = result.interrupt.pending_input.core_interrupt_value
    assert value == result.interrupt.value
    assert value["reason"] == "failure_route", value
    return value["failure"]


def started(name):
    return JOURNAL.names("started").count(name)


# ---------------------------------------------------------------------------
# The design's scenario
# ---------------------------------------------------------------------------


def test_three_failures_then_interrupted_at_ask_user_with_the_record():
    _host, result = start(FIX_LOOP)

    record = failure_record(result)
    assert record == {"node": "TEST", "kind": "exhausted", "code": "NG.TaskFailed",
                      "attempts": 3, "retryable": True, "step": 1, "routed_to": "ASK_USER",
                      "effects": []}
    assert started("TEST") == 3
    assert "ASK_USER" not in JOURNAL.names(), "the run waits before ASK_USER"
    # A handled failure is not a failure of the run.
    assert result.failure is None


def test_resume_along_the_retry_route_completes_after_a_fix():
    host, result = start(FIX_LOOP)
    record = failure_record(result)

    resumed = host.resume(result.run_id, {"decision": "retry", "fix": True}).wait()

    assert resumed.status == ng.ProgramTerminalStatus.Completed, describe(resumed)
    assert channel(resumed, "failures") == [record]
    assert channel(resumed, "trace") == ["IMPLEMENT", "ASK_USER", "IMPLEMENT", "TEST"]
    assert started("TEST") == 4
    assert started("ABORT") == 0


def test_resume_along_the_abort_route_fails_with_the_declared_code():
    host, result = start(FIX_LOOP)
    failure_record(result)

    resumed = host.resume(result.run_id, {"decision": "abort"}).wait()

    assert resumed.status == ng.ProgramTerminalStatus.Failed, describe(resumed)
    assert resumed.failure.code == "plan.user_abort"
    assert resumed.failure.core_node == "ABORT"
    assert resumed.failure.witness["failure_exit"] == {"node": "ABORT", "code": "plan.user_abort"}
    assert "ABORT" in JOURNAL.names()
    parsed = ng.ProgramResult.parse(resumed.serialize_canonical())
    assert parsed.failure.code == "plan.user_abort"


def test_an_author_code_from_a_python_exception_lands_in_the_record():
    _host, result = start(fix_loop('{code: "app.flaky_test"}'))
    record = failure_record(result)
    assert record["code"] == "app.flaky_test"
    assert record["kind"] == "exhausted"
    assert started("TEST") == 3

    # A code the policy lists as non_retryable runs once.
    JOURNAL.clear()
    _host, result = start(fix_loop('{code: "app.compile_error"}'))
    record = failure_record(result)
    assert record["code"] == "app.compile_error"
    assert record["kind"] == "non_retryable"
    assert record["attempts"] == 1
    assert record["retryable"] is False
    assert started("TEST") == 1


# ---------------------------------------------------------------------------
# Free text: opt-in, from public_message only, through the host's redactor
# ---------------------------------------------------------------------------

_SECRET = '{public_message: "token=hunter2 failed"}'


def test_error_text_without_a_redactor_writes_no_text():
    _host, result = start(fix_loop(_SECRET, "error_text: true"))
    assert "detail" not in failure_record(result)


def test_error_text_with_a_redactor_writes_what_it_keeps():
    seen = []

    def redact(failure):
        seen.append(failure)
        return failure["public_message"].replace("hunter2", "[redacted]")

    _host, result = start(fix_loop(_SECRET, "error_text: true"),
                          host=failure_host(failure_text_redactor=redact))
    record = failure_record(result)
    assert record["detail"] == {"untrusted_text": "token=[redacted] failed"}
    assert len(seen) == 1
    assert seen[0]["public_message"] == "token=hunter2 failed"
    assert seen[0]["code"] == "NG.TaskFailed" and seen[0]["kind"] == "exhausted"


def test_a_redactor_alone_does_not_opt_in():
    host = failure_host(failure_text_redactor=lambda failure: failure["public_message"])
    _host, result = start(fix_loop(_SECRET), host=host)
    assert "detail" not in failure_record(result)


# ---------------------------------------------------------------------------
# Loop guard and the run-level retry ceiling
# ---------------------------------------------------------------------------

LOOP = define_graph(_FAILURE_CHANNELS + """
    g.node("IMPLEMENT", {type: "e2e.step"});
    g.node("TEST", {type: "e2e.step"});
    g.node("DONE", {type: "e2e.step"});
    g.node("ASK_USER", {type: "e2e.review"});
    g.entry("IMPLEMENT");
    g.edge("IMPLEMENT", "TEST");
    g.conditionalEdge("TEST", "e2e.forever", {again: "IMPLEMENT", done: "DONE"});
    g.exit("DONE");
    g.edge("ASK_USER", "DONE");
    g.retryPolicy("TEST", {max_visits: 2, on_failure: "ASK_USER", error_channel: "failures"});
    g.interruptBefore("ASK_USER");
""", name="loop")


def test_max_visits_guards_a_loop():
    host = failure_host()
    version = host.compile_admit(source(LOOP), budget())
    result = host.run(version, {}, budget())

    record = failure_record(result)
    assert record == {"node": "TEST", "kind": "visit_limit", "code": "NG.VisitLimit",
                      "attempts": 0, "retryable": False, "step": 5, "routed_to": "ASK_USER",
                      "effects": []}
    assert JOURNAL.names().count("TEST") == 2

    resumed = host.resume(result.run_id, {"decision": "stop"}).wait()
    assert resumed.status == ng.ProgramTerminalStatus.Completed, describe(resumed)
    assert channel(resumed, "trace")[-2:] == ["ASK_USER", "DONE"]


def test_max_node_retries_is_a_run_level_ceiling():
    run_budget = budget()
    run_budget.max_node_retries = 1
    _host, result = start(FIX_LOOP, run_budget)

    record = failure_record(result)
    assert record["kind"] == "ceiling"
    assert record["attempts"] == 2
    assert started("TEST") == 2
    assert result.usage.node_retries == 1
    assert result.remaining_budget.max_node_retries == 0

    # Absent means none: the node policy alone decides, and nothing is counted.
    JOURNAL.clear()
    _host, unbounded = start(FIX_LOOP)
    assert failure_record(unbounded)["attempts"] == 3
    assert unbounded.usage.node_retries == 0
    assert unbounded.remaining_budget.max_node_retries is None
    assert "node_retries" not in unbounded.serialize_canonical()


# ---------------------------------------------------------------------------
# Unrouted failures
# ---------------------------------------------------------------------------


def test_a_program_diagnostic_inside_a_node_is_never_retried_or_routed():
    host = failure_host()
    text = fix_loop("{diagnostic: true}")
    version = host.compile_admit(source(text), budget())
    result = host.run(version, {}, budget())

    assert result.status == ng.ProgramTerminalStatus.Failed, describe(result)
    assert result.failure.code == "NG.ConfigError"
    assert result.failure.witness["node_failure"]["routable"] is False
    assert started("TEST") == 1
    assert "ASK_USER" not in JOURNAL.names()


# ---------------------------------------------------------------------------
# Crash and resume in a new process over SQLite: the Program host, then the
# Core engine
# ---------------------------------------------------------------------------

_HOST_PROCESS_A = r'''
import json, os, sys
from program_e2e import support as s
from program_e2e.test_10_failure_routing import FIX_LOOP, failure_registry

host = s.make_host(registry=failure_registry(), sqlite_directory=sys.argv[1])
version = s.admit(host, FIX_LOOP, s.budget())
result = host.run(version, {}, s.budget())
value = result.interrupt.pending_input.core_interrupt_value
print(json.dumps({"status": str(result.status), "run_id": result.run_id,
                  "call_id": result.interrupt.pending_input.call_id, "version_id": version.id,
                  "value": value, "started": s.JOURNAL.names("started")}), flush=True)
os._exit(0)  # no orderly shutdown: the process is simply gone
'''

_HOST_PROCESS_B = r'''
import json, sys
from program_e2e import support as s
from program_e2e.test_10_failure_routing import FIX_LOOP, failure_registry

run_id, call_id = sys.argv[2], sys.argv[3]
host = s.make_host(registry=failure_registry(), sqlite_directory=sys.argv[1])
version = s.admit(host, FIX_LOOP, s.budget())
result = host.resume(run_id, {"decision": "retry", "fix": True}, call_id).wait()
print(json.dumps({"status": str(result.status), "version_id": version.id,
                  "trace": s.trace_channel(result), "failures": s.channel(result, "failures"),
                  "started": s.JOURNAL.names("started"), "failure": s.describe(result)}),
      flush=True)
'''


@slow
def test_crash_resume_of_the_interrupted_run_in_a_new_process(tmp_path):
    store = str(tmp_path / "runs")
    first = run_python(_HOST_PROCESS_A, store)
    assert first.returncode == 0, first.stderr[-2000:]
    a = last_json_line(first.stdout)
    assert a["status"] == "ProgramTerminalStatus.Interrupted"
    assert a["value"]["reason"] == "failure_route"
    assert a["started"].count("TEST") == 3

    second = run_python(_HOST_PROCESS_B, store, a["run_id"], a["call_id"])
    assert second.returncode == 0, second.stderr[-2000:]
    b = last_json_line(second.stdout)
    assert b["version_id"] == a["version_id"]
    assert b["status"] == "ProgramTerminalStatus.Completed", b["failure"]
    # The handled TEST is never run again; the route continues at ASK_USER.
    assert b["started"] == ["IMPLEMENT", "TEST"], b
    assert b["trace"] == ["IMPLEMENT", "ASK_USER", "IMPLEMENT", "TEST"], b
    assert b["failures"] == [a["value"]["failure"]]


_CORE_PROCESS_A = r'''
import json, os, sys
import neograph_engine as ng
from program_e2e import support as s
from program_e2e.test_10_failure_routing import FIX_LOOP, failure_registry

bundle = ng.ProgramCompiler(failure_registry()).compile(s.source(FIX_LOOP), s.budget())
definition = bundle.sealed_core_definitions[0]["definition"]
engine = ng.GraphEngine.compile(definition, ng.NodeContext(), ng.SqliteCheckpointStore(sys.argv[1]))
result = engine.run(ng.RunConfig(thread_id="fix-thread"))
print(json.dumps({"interrupted": result.interrupted, "node": result.interrupt_node,
                  "value": result.interrupt_value, "started": s.JOURNAL.names("started")}))
sys.stdout.flush()
os._exit(0)  # no orderly shutdown: the process is simply gone
'''

_CORE_PROCESS_B = r'''
import json, sys
import neograph_engine as ng
from program_e2e import support as s
from program_e2e.test_10_failure_routing import FIX_LOOP, failure_registry

bundle = ng.ProgramCompiler(failure_registry()).compile(s.source(FIX_LOOP), s.budget())
definition = bundle.sealed_core_definitions[0]["definition"]
engine = ng.GraphEngine.compile(definition, ng.NodeContext(), ng.SqliteCheckpointStore(sys.argv[1]))
result = engine.resume("fix-thread", {"decision": "retry", "fix": True})
print(json.dumps({"status": str(result.status), "trace": result.execution_trace,
                  "failures": result.output["channels"]["failures"]["value"],
                  "started": s.JOURNAL.names("started")}))
'''


@slow
def test_core_engine_resume_of_the_interrupted_run_in_a_new_process(tmp_path):
    database = str(tmp_path / "checkpoints.sqlite")
    first = run_python(_CORE_PROCESS_A, database)
    assert first.returncode == 0, first.stderr[-2000:]
    a = last_json_line(first.stdout)
    assert a["interrupted"] is True and a["node"] == "ASK_USER"
    assert a["value"]["reason"] == "failure_route"
    assert a["started"].count("TEST") == 3

    second = run_python(_CORE_PROCESS_B, database)
    assert second.returncode == 0, second.stderr[-2000:]
    b = last_json_line(second.stdout)
    assert b["status"] == "RunStatus.Completed", b
    # The handled TEST is never run again; the route continues at ASK_USER.
    assert b["trace"] == ["ASK_USER", "IMPLEMENT", "TEST"], b
    assert b["started"] == ["IMPLEMENT", "TEST"], b
    assert b["failures"] == [a["value"]["failure"]]
