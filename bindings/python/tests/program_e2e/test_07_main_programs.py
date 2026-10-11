"""Scenario 7: main() generator programs (callCore, all, race, quorum, await, cancel).

`main()` drives a define() graph through `ng.callCore(<graph name>, input)`.
A sealed-v1 module must still export define(): a main()-only module fails
with P_JS_DEFINE_MISSING, so "main()" here always means define() + main().
"""

import time

import pytest

import neograph_engine as ng

from .support import (JOURNAL, admit, budget, ceiling, define_graph, describe, known_issue,
                      make_host, slow, source, with_main)

WORKER = define_graph("""
    g.node("worker", {type: "e2e.step", delay_from_state: true});
    g.entry("worker");
    g.exit("worker");
""", name="work")


def program(main_body):
    return with_main(WORKER, main_body)


def timed_run(text, input=None, run_budget=None, scheduler_threads=None):
    host = make_host(scheduler_threads=scheduler_threads)
    run_budget = run_budget or budget()
    version = admit(host, text, run_budget)
    started = time.monotonic()
    result = host.run(version, input or {}, run_budget)
    return result, time.monotonic() - started


def test_main_only_module_is_rejected_with_a_clear_code():
    with pytest.raises(ng.ProgramCompileError) as raised:
        make_host().compile(source("export function* main(input) { return {echo: input}; }"), budget())
    assert [d["code"] for d in raised.value.diagnostics] == ["P_JS_DEFINE_MISSING"]


def test_call_core_passes_input_and_returns_channel_aliases():
    result, _ = timed_run(program("""
        const first = yield ng.callCore("work", {topic: input.topic}, "first");
        yield ng.checkpoint({seen: first.topic}, "after-first");
        const second = yield ng.callCore("work", {topic: first.topic + "!"}, "second");
        return {first: first.topic, second: second.topic, trace: second.trace,
                canonical: second.channels.topic.value};
    """), {"topic": "refund"})

    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert result.output == {"first": "refund", "second": "refund!", "trace": ["worker"],
                             "canonical": "refund!"}
    assert result.execution_trace == ["worker", "worker"]
    assert result.usage.program_operations >= 3  # two callCore + one checkpoint


def test_all_returns_member_results_in_declaration_order():
    result, _ = timed_run(program("""
        const results = yield ng.all([
          ng.callCore("work", {topic: "a", delay: 40}, "a"),
          ng.callCore("work", {topic: "b"}, "b"),
          ng.emit({topic: "c"}, "c")
        ], {max_in_flight: 3}, "fan");
        return results.map((r) => r.topic);
    """))
    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert result.output == ["a", "b", "c"]


def test_join_validates_every_member_before_dispatch():
    result, _ = timed_run(program("""
        yield ng.all([
          ng.callCore("work", {}, "valid"),
          ng.callCore("not-a-graph", {}, "typo")
        ], {max_in_flight: 2}, "join");
        return {unreachable: true};
    """))
    assert result.status == ng.ProgramTerminalStatus.Failed, describe(result)
    assert result.failure.code == "P_JS_CONTROL_COMMAND"
    assert JOURNAL.names("started") == []


def test_race_between_ready_members_picks_the_first_declared():
    result, _ = timed_run(program("""
        return yield ng.race([ng.emit({id: 1}, "one"), ng.emit({id: 2}, "two")], "race");
    """))
    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert result.output == {"id": 1}


def test_quorum_of_ready_members():
    result, _ = timed_run(program("""
        return yield ng.quorum([ng.emit({id: 1}, "q1"), ng.emit({id: 2}, "q2"),
                                ng.emit({id: 3}, "q3")], 2, "quorum");
    """))
    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert [item["id"] for item in result.output][:2] == [1, 2]


THREADS = pytest.mark.parametrize("scheduler_threads", [None, 4], ids=["default-threads", "4-threads"])

_SERIAL_JOIN = ("join members share one strand and a Python node's run() holds it, so "
                "all/race/quorum members execute one after another while usage.peak_concurrency "
                "still reports 2")


@slow
@THREADS
@known_issue("#367", _SERIAL_JOIN + "; all() of two 800 ms Python members takes ~1.6 s")
def test_all_runs_python_members_concurrently(scheduler_threads):
    result, elapsed = timed_run(program("""
        return (yield ng.all([
          ng.callCore("work", {delay: 800}, "a"),
          ng.callCore("work", {delay: 800}, "b")
        ], {max_in_flight: 2}, "fan")).length;
    """), scheduler_threads=scheduler_threads)
    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert result.usage.peak_concurrency == 2
    assert elapsed < 1.3, f"two 800 ms members took {elapsed:.2f}s"


@slow
@THREADS
@known_issue("#367", _SERIAL_JOIN + "; race() returns the slow first-declared member after it "
             "finishes instead of the fast one")
def test_race_returns_the_first_member_to_finish(scheduler_threads):
    result, elapsed = timed_run(program("""
        const winner = yield ng.race([
          ng.callCore("work", {topic: "slow", delay: 1500}, "slow"),
          ng.callCore("work", {topic: "fast", delay: 20}, "fast")
        ], {max_in_flight: 2}, "race");
        return winner.topic;
    """), scheduler_threads=scheduler_threads)
    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert result.output == "fast"
    assert elapsed < 1.0, f"race took {elapsed:.2f}s"


@slow
@THREADS
@known_issue("#367", _SERIAL_JOIN + "; quorum() waits for the slow member and includes it")
def test_quorum_does_not_wait_for_a_slow_member(scheduler_threads):
    result, elapsed = timed_run(program("""
        const ok = yield ng.quorum([
          ng.callCore("work", {topic: "slow", delay: 1500}, "slow"),
          ng.callCore("work", {topic: "f1", delay: 10}, "f1"),
          ng.callCore("work", {topic: "f2", delay: 10}, "f2")
        ], {required_successes: 2, max_in_flight: 3}, "quorum");
        return ok.map((r) => r.topic);
    """), scheduler_threads=scheduler_threads)
    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert sorted(result.output) == ["f1", "f2"]
    assert elapsed < 1.0, f"quorum took {elapsed:.2f}s"


AWAIT_SLOW = program("""
    const reply = yield ng.await(ng.callCore("work", {delay: 3000}, "slow"), 150, "deadline");
    return {late: reply.trace};
""")


def test_await_timeout_cancels_a_slow_child_with_scheduler_threads():
    result, elapsed = timed_run(AWAIT_SLOW, scheduler_threads=4)
    assert result.status == ng.ProgramTerminalStatus.TimedOut, describe(result)
    assert result.failure.code == "P_AWAIT_TIMEOUT"
    assert elapsed < 1.5, f"await(…, 150) returned after {elapsed:.2f}s"
    assert JOURNAL.names("cancelled") == ["worker"]
    assert JOURNAL.names("ran") == []


@slow
@known_issue("#368", "with the default LocalProgramHost (scheduler_threads=1) the await() timer "
             "cannot fire while a Python node holds the only thread, and a completion that "
             "arrives after the deadline is accepted: the run returns Completed with the late "
             "result after 3 s instead of TimedOut")
def test_await_timeout_cancels_a_slow_child_with_the_default_host():
    result, elapsed = timed_run(AWAIT_SLOW)
    assert result.status == ng.ProgramTerminalStatus.TimedOut, describe(result)
    assert result.failure.code == "P_AWAIT_TIMEOUT"
    assert elapsed < 1.5


def test_cancel_scope_cancels_the_run():
    result, _ = timed_run(program("""
        yield ng.callCore("work", {}, "before");
        yield ng.cancelScope("current", "operator stop", "stop");
        yield ng.callCore("work", {}, "after");
        return {unreachable: true};
    """))
    assert result.status == ng.ProgramTerminalStatus.Cancelled, describe(result)
    assert result.failure.code == "P_RUNTIME_CANCELLED"
    assert result.failure.message == "operator stop"
    assert JOURNAL.names() == ["worker"]


def test_external_cancel_through_the_handle():
    host = make_host()
    run_budget = budget()
    version = admit(host, program('return yield ng.callCore("work", {delay: 5000}, "slow");'), run_budget)
    handle = host.start(version, {}, run_budget)
    deadline = time.monotonic() + 3
    while "worker" not in JOURNAL.names("started") and time.monotonic() < deadline:
        time.sleep(0.01)
    assert handle.try_result() is None
    started = time.monotonic()
    assert handle.cancel() is True
    result = handle.wait()
    assert time.monotonic() - started < 1.5
    assert result.status == ng.ProgramTerminalStatus.Cancelled, describe(result)
    assert result.failure.code == "P_RUNTIME_CANCELLED"
    assert JOURNAL.names("cancelled") == ["worker"]


def test_spawn_has_no_child_binding_in_the_python_host():
    """`ng.spawn` is part of the sealed-v1 vocabulary, but LocalProgramHost
    offers no way to link a child Program, so every spawn fails closed."""
    host = make_host(budget_ceiling=ceiling(max_child_depth=1, max_total_children=1))
    run_budget = budget(max_child_depth=1, max_total_children=1)
    version = admit(host, program("""
        return yield ng.await(ng.spawn("reviewer", {draft: "x"}, "spawn"), 1000, "await");
    """), run_budget)
    result = host.run(version, {}, run_budget)
    assert result.status == ng.ProgramTerminalStatus.Failed, describe(result)
    assert result.failure.code == "P_CHILD_BINDING"
