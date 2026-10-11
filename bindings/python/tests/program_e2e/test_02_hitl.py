"""Scenario 2: human-in-the-loop interrupt and resume."""

import pytest

import neograph_engine as ng

from .support import (APPROVAL_GATE, JOURNAL, admit, budget, channel, define_graph, describe,
                      last_json_line, make_host, run_python, slow, trace_channel,
                      with_main)

DYNAMIC_QUESTION = define_graph("""
    g.node("plan", {type: "e2e.step"});
    g.node("confirm", {type: "e2e.step", ask: "rm -rf build/ ?"});
    g.node("apply", {type: "e2e.step"});
    g.entry("plan");
    g.edge("plan", "confirm");
    g.edge("confirm", "apply");
    g.exit("apply");
""", name="dynamic")


def _interrupted(text, host=None):
    host = host or make_host()
    run_budget = budget()
    version = admit(host, text, run_budget)
    result = host.run(version, {}, run_budget)
    assert result.status == ng.ProgramTerminalStatus.Interrupted, describe(result)
    return host, version, result


def test_interrupt_before_then_resume_with_default_pending_id():
    host, _version, interrupted = _interrupted(APPROVAL_GATE)

    assert interrupted.failure is None
    assert interrupted.execution_trace == ["draft"]
    assert trace_channel(interrupted) == ["draft"]
    assert interrupted.checkpoint is not None and interrupted.checkpoint.checkpoint_id
    interrupt = interrupted.interrupt
    assert interrupt.core_node == "deploy"
    pending = interrupt.pending_input
    assert pending is not None and interrupt.pending_effect is None
    assert pending.kind == ng.ProgramPendingInputKind.Input
    assert pending.state == ng.ProgramPendingState.Awaiting
    assert pending.call_id.startswith(interrupted.run_id)

    resumed = host.resume(interrupted.run_id, {"approved": True}).wait()

    assert resumed.status == ng.ProgramTerminalStatus.Completed, describe(resumed)
    assert resumed.run_id == interrupted.run_id
    assert trace_channel(resumed) == ["draft", "deploy"]
    assert resumed.execution_trace[-1] == "deploy"
    # The node gated by interruptBefore receives the human's answer.
    assert channel(resumed, "answer") == {"approved": True}
    # Nothing ran twice.
    assert JOURNAL.names() == ["draft", "deploy"]


def test_dynamic_node_interrupt_payload_and_explicit_pending_id():
    host, _version, interrupted = _interrupted(DYNAMIC_QUESTION)

    assert interrupted.interrupt.core_node == "confirm"
    assert interrupted.interrupt.value["value"] == {"question": "rm -rf build/ ?"}
    assert interrupted.interrupt.value["reason"] == "needs approval"
    assert interrupted.interrupt.pending_input.payload == interrupted.interrupt.value
    call_id = interrupted.interrupt.pending_input.call_id

    resumed = host.resume(interrupted.run_id, {"approved": "yes"}, call_id).wait()

    assert resumed.status == ng.ProgramTerminalStatus.Completed, describe(resumed)
    assert channel(resumed, "answer") == {"approved": "yes"}
    assert trace_channel(resumed) == ["plan", "confirm", "apply"]
    assert JOURNAL.names("asked") == ["confirm"]
    assert JOURNAL.names() == ["plan", "confirm", "apply"]


def test_wrong_pending_id_is_rejected_and_run_stays_resumable():
    host, _version, interrupted = _interrupted(DYNAMIC_QUESTION)
    call_id = interrupted.interrupt.pending_input.call_id

    with pytest.raises(RuntimeError, match="pending call"):
        host.resume(interrupted.run_id, {"approved": True}, call_id + "-typo")

    # The rejected attempt consumed nothing.
    resumed = host.resume(interrupted.run_id, {"approved": True}).wait()
    assert resumed.status == ng.ProgramTerminalStatus.Completed, describe(resumed)


def test_resume_of_completed_run_is_rejected():
    host, _version, interrupted = _interrupted(APPROVAL_GATE)
    assert host.resume(interrupted.run_id, {"approved": True}).wait().status == \
        ng.ProgramTerminalStatus.Completed

    with pytest.raises(RuntimeError, match="Only an interrupted Program run may resume"):
        host.resume(interrupted.run_id, {"approved": True})


def test_resume_of_unknown_run_is_rejected(host):
    with pytest.raises(RuntimeError, match="not found"):
        host.resume("run-does-not-exist", {"approved": True})


def test_interrupt_inside_main_call_core_resumes_into_the_generator():
    text = with_main(
        define_graph("""
            g.node("confirm", {type: "e2e.step", ask: "ship it?"});
            g.entry("confirm");
            g.exit("confirm");
        """),
        """
        const reply = yield ng.callCore("work", {}, "approval");
        return {decision: reply.answer, trace: reply.trace};
        """,
    )
    host, _version, interrupted = _interrupted(text)
    assert interrupted.interrupt.core_node == "confirm"

    resumed = host.resume(interrupted.run_id, {"ship": True}).wait()

    assert resumed.status == ng.ProgramTerminalStatus.Completed, describe(resumed)
    assert resumed.output == {"decision": {"ship": True}, "trace": ["confirm"]}


# ---------------------------------------------------------------------------
# Restart: interrupt in process A, resume in process B.
# ---------------------------------------------------------------------------

_PROCESS_A = r'''
import json
from program_e2e import support as s
host = s.make_host("tenant:restart")
version = s.admit(host, s.APPROVAL_GATE, s.budget())
result = host.run(version, {}, s.budget())
print(json.dumps({"status": str(result.status), "run_id": result.run_id,
                  "call_id": result.interrupt.pending_input.call_id, "version_id": version.id}))
'''

_PROCESS_B = r'''
import json, sys
from program_e2e import support as s
run_id, call_id = sys.argv[1], sys.argv[2]
host = s.make_host("tenant:restart")
version = s.admit(host, s.APPROVAL_GATE, s.budget())
try:
    result = host.resume(run_id, {"approved": True}, call_id).wait()
    print(json.dumps({"version_id": version.id, "status": str(result.status),
                      "trace": s.trace_channel(result)}))
except RuntimeError as error:
    print(json.dumps({"version_id": version.id, "error": str(error)}))
'''


@slow
def test_resume_in_a_new_process_with_the_in_memory_host():
    """LocalProgramHost is documented as in-memory: a restart loses the run.

    This pins down what a user sees today: a clear 'not found', and the same
    version id in both processes.
    """
    first = run_python(_PROCESS_A)
    assert first.returncode == 0, first.stderr[-2000:]
    a = last_json_line(first.stdout)
    assert a["status"] == "ProgramTerminalStatus.Interrupted"

    second = run_python(_PROCESS_B, a["run_id"], a["call_id"])
    assert second.returncode == 0, second.stderr[-2000:]
    b = last_json_line(second.stdout)

    assert b["version_id"] == a["version_id"], "admission identity is deterministic across processes"
    assert "error" in b and "not found" in b["error"], b


# ---------------------------------------------------------------------------
# main() with two callCore steps, the second one interrupted.
# ---------------------------------------------------------------------------

_TWO_CALL_CORES = r'''
import json
import neograph_engine as ng
from program_e2e import support as s

text = s.with_main(s.APPROVAL_GATE, """
    const first = yield ng.callCore("release", {}, "first");
    const second = yield ng.callCore("release", {}, "second");
    return {first: first.answer, second: second.answer};
""")
host = s.make_host()
version = s.admit(host, text)
result = host.run(version, {}, s.budget())
statuses = [str(result.status)]
for answer in ("one", "two"):
    if result.status != ng.ProgramTerminalStatus.Interrupted:
        break
    result = host.resume(result.run_id, answer).wait()
    statuses.append(str(result.status))
print(json.dumps({"statuses": statuses, "output": result.output}))
'''


@slow
def test_second_interrupted_call_core_in_main_resumes():
    # A fresh interpreter: before the fix (#371) the scenario killed the process.
    completed = run_python(_TWO_CALL_CORES, timeout=60)
    assert completed.returncode == 0, (
        f"interpreter died with exit code {completed.returncode}: {completed.stderr[-600:]}")
    outcome = last_json_line(completed.stdout)
    assert outcome["statuses"] == ["ProgramTerminalStatus.Interrupted",
                                   "ProgramTerminalStatus.Interrupted",
                                   "ProgramTerminalStatus.Completed"], outcome
    assert outcome["output"] == {"first": "one", "second": "two"}, outcome
