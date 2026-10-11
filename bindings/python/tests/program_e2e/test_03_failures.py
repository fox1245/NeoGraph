"""Scenario 3: a node raises; the run must fail visibly and keep useful evidence."""

import neograph_engine as ng

from .support import (JOURNAL, admit, budget, define_graph, describe, known_issue, last_json_line,
                      make_host, run_program, run_python, slow, with_main)

PIPELINE = define_graph("""
    g.node("fetch", {type: "e2e.step"});
    g.node("parse", {type: "e2e.step"});
    g.node("validate", {type: "e2e.step", boom: true});
    g.node("publish", {type: "e2e.step"});
    g.entry("fetch");
    g.edge("fetch", "parse");
    g.edge("parse", "validate");
    g.edge("validate", "publish");
    g.exit("publish");
""", name="pipeline")


def _assert_not_success(result):
    assert result.status != ng.ProgramTerminalStatus.Completed
    assert result.failure is not None
    # A failed define()-graph run reports no output at all; a failed main()
    # run reports an empty object. Neither may look like a finished result.
    assert result.output in (None, {}), result.output


def test_node_failure_mid_graph():
    _host, version, result = run_program(PIPELINE)

    assert result.status == ng.ProgramTerminalStatus.Failed, describe(result)
    _assert_not_success(result)
    failure = result.failure
    assert failure.code == "P_RUNTIME_CORE_FAILURE"
    assert failure.core_node == "validate"
    assert "boom from validate" in failure.message
    assert failure.attempts == 1
    assert failure.provider_outcome is None and failure.provider_cause is None
    assert result.execution_trace == ["fetch", "parse"]
    assert JOURNAL.names() == ["fetch", "parse"]
    assert "publish" not in JOURNAL.names("started")
    assert result.program_version_id == version.id

    parsed = ng.ProgramResult.parse(result.serialize_canonical())
    assert parsed.status == ng.ProgramTerminalStatus.Failed
    assert parsed.failure.code == failure.code
    assert parsed.failure.core_node == "validate"
    assert parsed.execution_trace == ["fetch", "parse"]


def test_node_failure_through_start_and_wait():
    host = make_host()
    version = admit(host, PIPELINE)
    handle = host.start(version, {}, budget())
    result = handle.wait()
    assert handle.run_id == result.run_id
    assert result.status == ng.ProgramTerminalStatus.Failed, describe(result)
    assert result.failure.core_node == "validate"
    assert handle.try_result() is not None


def test_failure_in_the_entry_node_has_an_empty_trace():
    text = define_graph("""
        g.node("first", {type: "e2e.step", boom: true});
        g.node("second", {type: "e2e.step"});
        g.entry("first");
        g.edge("first", "second");
        g.exit("second");
    """)
    _host, _version, result = run_program(text)
    assert result.status == ng.ProgramTerminalStatus.Failed, describe(result)
    assert result.failure.core_node == "first"
    assert result.execution_trace == []
    assert JOURNAL.names() == []


def test_failing_branch_of_a_fan_out_stops_the_join():
    text = define_graph("""
        g.node("split", {type: "e2e.step"});
        g.node("left", {type: "e2e.step"});
        g.node("right", {type: "e2e.step", boom: true});
        g.node("join", {type: "e2e.step"});
        g.entry("split");
        g.edge("split", "left");
        g.edge("split", "right");
        g.edge("left", "join");
        g.edge("right", "join");
        g.barrier("join", ["left", "right"]);
        g.exit("join");
    """)
    _host, _version, result = run_program(text)
    assert result.status == ng.ProgramTerminalStatus.Failed, describe(result)
    _assert_not_success(result)
    assert result.failure.core_node == "right"
    assert "join" not in JOURNAL.names("started")
    assert result.execution_trace[0] == "split"


def test_exception_thrown_by_main_fails_the_run():
    text = with_main(define_graph('g.node("w", {type: "e2e.step"}); g.entry("w"); g.exit("w");'),
                     'yield ng.checkpoint({phase: "start"}, "cp");\nthrow new Error("main exploded");')
    _host, _version, result = run_program(text)
    assert result.status == ng.ProgramTerminalStatus.Failed, describe(result)
    _assert_not_success(result)
    assert result.failure.code == "P_JS_EVALUATION"
    assert "main exploded" in result.failure.message


def test_failed_call_core_terminates_main_without_reaching_catch():
    """A Core failure ends the run; it is not thrown into the generator.

    docs/QUICKJS_CONTROL_ARCHITECTURE.md describes the yield result as
    'recorded result or failure -> generator.next(recorded outcome)', which a
    reader can take to mean the failure is catchable. Today it is not.
    """
    text = with_main(define_graph('g.node("w", {type: "e2e.step", boom: true}); g.entry("w"); g.exit("w");'),
                     """
                     try {
                       yield ng.callCore("work", {}, "risky");
                     } catch (error) {
                       return {recovered: String(error)};
                     }
                     return {unreachable: true};
                     """)
    _host, _version, result = run_program(text)
    assert result.status == ng.ProgramTerminalStatus.Failed, describe(result)
    _assert_not_success(result)
    assert result.failure.code == "P_RUNTIME_CORE_FAILURE"
    assert result.failure.core_node == "w"


_LLM_CALL_WITHOUT_PROVIDER = r'''
import json
import neograph_engine as ng
from program_e2e import support as s

text = """
export function define() {
  const g = ng.graph("chat");
  g.channel("messages", {reducer: "append", initial: []});
  g.node("llm", {type: "llm_call"});
  g.entry("llm");
  g.exit("llm");
  return g;
}
"""
try:
    registry = s.build_registry(extra_node_types=("llm_call",))
    host = s.make_host(registry=registry)
    version = s.admit(host, text)
    result = host.run(version, {"messages": [{"role": "user", "content": "hi"}]}, s.budget())
    print(json.dumps({"status": str(result.status),
                      "code": result.failure.code if result.failure else None,
                      "message": result.failure.message if result.failure else None}))
except Exception as error:
    codes = [d.get("code") for d in getattr(error, "diagnostics", None) or [] if isinstance(d, dict)]
    print(json.dumps({"raised": type(error).__name__, "message": str(error), "codes": codes}))
'''


@slow
@known_issue("#366", "an llm_call node with no bound Provider aborts the interpreter: "
             "LLMCallNode dereferences a null provider and LocalProgramHost has no capability "
             "binder, so every Python Program llm_call hits it")
def test_llm_call_node_without_a_provider_is_rejected_at_admission():
    # A fresh interpreter, because today the scenario kills the process.
    completed = run_python(_LLM_CALL_WITHOUT_PROVIDER, timeout=60)
    assert completed.returncode == 0, (
        f"interpreter died with exit code {completed.returncode}: {completed.stderr[-600:]}")
    outcome = last_json_line(completed.stdout)
    assert "raised" in outcome, outcome
    assert "P_ADMIT_PROVIDER" in outcome["codes"] or "P_ADMIT_PROVIDER" in outcome["message"], \
        outcome
