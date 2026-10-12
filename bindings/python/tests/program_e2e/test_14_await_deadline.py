"""Scenario 14: an await keeps its absolute deadline across resume and recovery (#391).

`ng.await(command, timeout_ms)` in main() used to start its timer again on
every attempt, so each interrupt/resume cycle (and each crash recovery) gave the
awaited command another full `timeout_ms`. The await's start is now recorded in
the command journal when it first starts, and every later attempt has only the
time that remains: a resume after the deadline times out at once
(P_AWAIT_TIMEOUT) without running the answered node.
"""

import json
import time

import pytest

import neograph_engine as ng

from .support import (JOURNAL, admit, budget, ceiling, define_graph, describe, last_json_line,
                      make_host, run_python, slow, with_main)

ASK = define_graph("""
    g.node("check", {type: "e2e.step", ask: "approve?"});
    g.entry("check");
    g.exit("check");
""", name="approval")


def _awaiting(timeout_ms):
    return with_main(ASK, f"""
        const reply = yield ng.await(ng.callCore("approval", {{}}, "approval"), {timeout_ms}, "await");
        return {{answer: reply.answer}};
    """)


def _paused(host, timeout_ms):
    run_budget = budget()
    version = admit(host, _awaiting(timeout_ms), run_budget)
    paused = host.run(version, {}, run_budget)
    assert paused.status == ng.ProgramTerminalStatus.Interrupted, describe(paused)
    assert JOURNAL.names("asked") == ["check"]
    return paused


def test_a_resume_inside_the_deadline_completes():
    host = make_host()
    paused = _paused(host, 10_000)
    done = host.resume(paused.run_id, {"approved": True}).wait()
    assert done.status == ng.ProgramTerminalStatus.Completed, describe(done)
    assert done.output == {"answer": {"approved": True}}


@slow
@pytest.mark.parametrize("durable", [False, True], ids=["in-memory", "sqlite"])
def test_a_resume_after_the_deadline_times_out_at_once(durable, tmp_path):
    options = dict(sqlite_directory=str(tmp_path / "runs")) if durable else {}
    host = make_host(**options)
    paused = _paused(host, 800)

    time.sleep(1.0)
    started = time.monotonic()
    done = host.resume(paused.run_id, {"approved": True}).wait()

    assert done.status == ng.ProgramTerminalStatus.TimedOut, describe(done)
    assert done.failure.code == "P_AWAIT_TIMEOUT"
    assert time.monotonic() - started < 0.7, "no new timer was started"
    assert JOURNAL.names() == [], "the answered node does not run after the deadline"


@slow
def test_a_host_restarted_on_the_same_directory_keeps_the_deadline(tmp_path):
    directory = str(tmp_path / "runs")
    paused = _paused(make_host(sqlite_directory=directory), 800)

    time.sleep(1.0)
    restarted = make_host(sqlite_directory=directory)
    done = restarted.resume(paused.run_id, {"approved": True}).wait()

    assert done.status == ng.ProgramTerminalStatus.TimedOut, describe(done)
    assert done.failure.code == "P_AWAIT_TIMEOUT"


# ---------------------------------------------------------------------------
# Crash recovery: the resumed attempt dies inside the await, and reconnect()
# in a new process enforces the deadline the await started with.
# ---------------------------------------------------------------------------

_CRASH_SETUP = r'''
import json, os, sys, time
import neograph_engine as ng
from program_e2e import support as s

store = sys.argv[1]


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
    g.node("check", {type: "e2e.step", ask: "approve?"});
    g.node("after", {type: "e2e.die_once"});
    g.entry("check");
    g.edge("check", "after");
    g.exit("after");
""", name="approval")
TEXT = s.with_main(GRAPH, """
    const reply = yield ng.await(ng.callCore("approval", {}, "approval"), 2500, "await");
    return {answer: reply.answer};
""")
CEILING = s.ceiling(max_child_depth=1, max_total_children=1)
BUDGET = s.budget(max_child_depth=1, max_total_children=1)
host = s.make_host("tenant:await-crash",
                   registry=s.build_registry(extra_node_types=("e2e.die_once",)),
                   budget_ceiling=CEILING, sqlite_directory=store)
version = s.admit(host, TEXT, BUDGET)
'''

_CRASH_FIRST = _CRASH_SETUP + r'''
paused = host.run(version, {}, BUDGET)
print(json.dumps({"status": str(paused.status), "run_id": paused.run_id,
                  "paused_at": time.time()}), flush=True)
os.environ["E2E_DIE"] = "1"
done = host.resume(paused.run_id, {"approved": True}).wait()
print(json.dumps({"unreachable": s.describe(done)}), flush=True)
'''

_CRASH_RECOVERY = _CRASH_SETUP + r'''
deadline = float(sys.argv[3]) + 3.0
time.sleep(max(0.0, deadline - time.time()))
result = host.reconnect(sys.argv[2]).wait()
print(json.dumps({"status": str(result.status), "attempt": result.attempt,
                  "code": result.failure.code if result.failure else None,
                  "failure": s.describe(result)}), flush=True)
'''


@slow
def test_crash_recovery_of_a_resumed_await_keeps_its_deadline(tmp_path):
    store = str(tmp_path / "runs")
    crashed = run_python(_CRASH_FIRST, store)
    assert crashed.returncode == 9, (crashed.returncode, crashed.stdout, crashed.stderr[-2000:])
    lines = [json.loads(line) for line in crashed.stdout.splitlines() if line.startswith("{")]
    assert lines[0]["status"] == "ProgramTerminalStatus.Interrupted"
    assert lines[-1] == {"dying": True}

    recovered = run_python(_CRASH_RECOVERY, store, lines[0]["run_id"], str(lines[0]["paused_at"]))
    assert recovered.returncode == 0, recovered.stderr[-2000:]
    outcome = last_json_line(recovered.stdout)
    # The await started before the first pause; 2.5 s later its deadline has
    # passed, so recovery times out instead of granting a new 2.5 s.
    assert outcome["status"] == "ProgramTerminalStatus.TimedOut", outcome["failure"]
    assert outcome["code"] == "P_AWAIT_TIMEOUT"
    assert outcome["attempt"] == 2
