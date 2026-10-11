"""Scenario 2: human-in-the-loop interrupt and resume."""

import json

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


def test_each_result_of_two_interrupting_call_cores_lists_the_nodes_it_ran():
    # Issue #374: the traces were one attempt late: ["draft"], ["draft"],
    # ["deploy", "deploy"].
    text = with_main(APPROVAL_GATE, """
        const first = yield ng.callCore("release", {}, "first");
        const second = yield ng.callCore("release", {}, "second");
        return {first: first.answer, second: second.answer};
    """)
    host, _version, result = _interrupted(text)
    traces = [result.execution_trace]
    for answer in ("one", "two"):
        assert result.status == ng.ProgramTerminalStatus.Interrupted, describe(result)
        result = host.resume(result.run_id, answer).wait()
        traces.append(result.execution_trace)

    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert result.output == {"first": "one", "second": "two"}
    assert traces == [["draft"], ["deploy", "draft"], ["deploy"]]
    # Every node ran exactly once, and the traces together list them in order.
    assert JOURNAL.names() == ["draft", "deploy", "draft", "deploy"]
    assert [node for trace in traces for node in trace] == JOURNAL.names()


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


# ---------------------------------------------------------------------------
# Crash during a resumed attempt: recovery continues it with the resume value
# (#377). The run input and the resume value differ, so recovery with the
# wrong one is visible in the "answer" channel.
# ---------------------------------------------------------------------------

_RESUMED_CRASH_SETUP = r'''
import json, os, sys
import neograph_engine as ng
from program_e2e import support as s

store, kind = sys.argv[1], sys.argv[2]


class DieOnce(ng.GraphNode):
    """Kills its process when it runs in the crashing process."""

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
GRAPH = s.define_graph("""
    g.node("draft", {type: "e2e.step"});
    g.node("deploy", {type: "e2e.step", record_resume: true});
    g.node("after", {type: "e2e.die_once"});
    g.entry("draft");
    g.edge("draft", "deploy");
    g.edge("deploy", "after");
    g.interruptBefore("deploy");
    g.exit("after");
""", name="release")
TEXT = GRAPH if kind == "define" else s.with_main(GRAPH, """
    const reply = yield ng.callCore("release", {topic: input.topic}, "release");
    return {answer: reply.answer, trace: reply.trace};
""")
# Child budgets make a define() run recover from its checkpoint after a crash.
CEILING = s.ceiling(max_child_depth=1, max_total_children=1)
BUDGET = s.budget(max_child_depth=1, max_total_children=1)
host = s.make_host("tenant:resumed-crash",
                   registry=s.build_registry(extra_node_types=("e2e.die_once",)),
                   budget_ceiling=CEILING, sqlite_directory=store)
version = s.admit(host, TEXT, BUDGET)
'''

_RESUMED_CRASH_FIRST = _RESUMED_CRASH_SETUP + r'''
paused = host.run(version, {"topic": "original input"}, BUDGET)
print(json.dumps({"status": str(paused.status), "run_id": paused.run_id}), flush=True)
os.environ["E2E_DIE"] = "1"
done = host.resume(paused.run_id, {"approved": True}).wait()
print(json.dumps({"unreachable": s.describe(done)}), flush=True)
'''

_RESUMED_CRASH_RECOVERY = _RESUMED_CRASH_SETUP + r'''
result = host.reconnect(sys.argv[3]).wait()
answer = (result.output or {}).get("answer") if kind == "main" else s.channel(result, "answer")
print(json.dumps({"status": str(result.status), "attempt": result.attempt, "answer": answer,
                  "failure": s.describe(result)}), flush=True)
'''


@slow
@pytest.mark.parametrize("kind", ["define", "main"])
def test_recovery_of_a_resumed_attempt_keeps_its_resume_value(kind, tmp_path):
    store = str(tmp_path / "runs")
    crashed = run_python(_RESUMED_CRASH_FIRST, store, kind)
    assert crashed.returncode == 9, (crashed.returncode, crashed.stdout, crashed.stderr[-2000:])
    lines = [json.loads(line) for line in crashed.stdout.splitlines() if line.startswith("{")]
    assert lines[0]["status"] == "ProgramTerminalStatus.Interrupted"
    assert lines[-1] == {"dying": True}

    recovered = run_python(_RESUMED_CRASH_RECOVERY, store, kind, lines[0]["run_id"])
    assert recovered.returncode == 0, recovered.stderr[-2000:]
    outcome = last_json_line(recovered.stdout)
    assert outcome["status"] == "ProgramTerminalStatus.Completed", outcome["failure"]
    assert outcome["attempt"] == 2
    # The recovered attempt resumed "deploy" with the resume value, as the
    # attempt that crashed did, not with the run input.
    assert outcome["answer"] == {"approved": True}, outcome


# ---------------------------------------------------------------------------
# A member's interrupt pauses its join (#380). The join launches no further
# member, lets running members finish and keeps what they reach. Unless that
# ends the join by its own rule, it is Interrupted on its first paused member;
# resume replays the settled members and continues the paused one.
# ---------------------------------------------------------------------------

ROUTED = define_graph("""
    g.node("route", {type: "e2e.step"});
    g.node("work", {type: "e2e.step", delay_from_state: true});
    g.node("confirm", {type: "e2e.step", ask: "ok?"});
    g.entry("route");
    g.conditionalEdge("route", "e2e.route_by_topic", {billing: "work", general: "confirm"});
    g.exit("work");
    g.exit("confirm");
""", name="chat")

WORK = '{topic: "billing"}'
ASK = '{topic: "general"}'
SLOW_WORK = '{topic: "billing", delay: 300}'


def _join_program(join, members, options):
    calls = ",\n          ".join(f'ng.callCore("chat", {member}, "member{index}")'
                                for index, member in enumerate(members))
    return with_main(ROUTED, f"""
        const replies = yield ng.{join}([
          {calls}
        ], {options}, "join");
        return Array.isArray(replies) ? replies.map((reply) => reply.trace) : replies.trace;
    """)


def _paused_join(text, host=None, members=2):
    host = host or make_host(scheduler_threads=members)
    run_budget = budget(max_concurrency=members, max_program_operations=8)
    version = admit(host, text, run_budget)
    return host, host.run(version, {}, run_budget)


@pytest.mark.parametrize("durable", [False, True], ids=["in-memory", "sqlite"])
def test_an_interrupted_member_pauses_all_and_resume_replays_the_completed_one(durable, tmp_path):
    # Before the fix, a SQLite host resumed the completed member's checkpoint
    # and asked again, and an in-memory host ran the completed member twice.
    options = {"sqlite_directory": str(tmp_path / "runs")} if durable else {}
    host, paused = _paused_join(_join_program("all", [WORK, ASK], "{max_in_flight: 2}"),
                                make_host(scheduler_threads=2, **options))

    assert paused.status == ng.ProgramTerminalStatus.Interrupted, describe(paused)
    assert paused.interrupt.core_node == "confirm"
    assert paused.output[1] is None
    assert paused.execution_trace == ["route", "work", "route"]

    done = host.resume(paused.run_id, {"ok": True}).wait()

    assert done.status == ng.ProgramTerminalStatus.Completed, describe(done)
    assert done.output == [["route", "work"], ["route", "confirm"]]
    assert done.execution_trace == ["confirm"]
    # The completed member was replayed: "work" ran once, and nothing asked twice.
    assert sorted(JOURNAL.names()) == ["confirm", "route", "route", "work"]
    assert JOURNAL.names("asked") == ["confirm"]


def test_a_race_is_won_by_the_member_that_finishes_while_another_is_paused():
    # A paused member has not finished: the member that does wins the race.
    _host, result = _paused_join(_join_program("race", [ASK, SLOW_WORK], "{max_in_flight: 2}"))

    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert result.output == ["route", "work"]
    assert JOURNAL.names("asked") == ["confirm"]


def test_a_race_of_paused_members_resumes_the_first_declared():
    host, paused = _paused_join(_join_program("race", [ASK, ASK], "{max_in_flight: 2}"))

    assert paused.status == ng.ProgramTerminalStatus.Interrupted, describe(paused)
    assert paused.interrupt.pending_input.operation_id == "root.javascript.1/member/0"

    done = host.resume(paused.run_id, {"ok": True}).wait()

    assert done.status == ng.ProgramTerminalStatus.Completed, describe(done)
    assert done.output == ["route", "confirm"]


def test_a_quorum_met_without_its_paused_member_completes():
    _host, result = _paused_join(
        _join_program("quorum", [ASK, WORK, SLOW_WORK],
                      "{required_successes: 2, max_in_flight: 3}"), members=3)

    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert result.output == [["route", "work"], ["route", "work"]]
    assert JOURNAL.names("asked") == ["confirm"]


def test_a_quorum_that_needs_its_paused_member_waits_for_it():
    host, paused = _paused_join(
        _join_program("quorum", [ASK, WORK], "{required_successes: 2, max_in_flight: 2}"))

    assert paused.status == ng.ProgramTerminalStatus.Interrupted, describe(paused)

    done = host.resume(paused.run_id, {"ok": True}).wait()

    assert done.status == ng.ProgramTerminalStatus.Completed, describe(done)
    assert done.output == [["route", "confirm"], ["route", "work"]]
    assert JOURNAL.names().count("work") == 1
