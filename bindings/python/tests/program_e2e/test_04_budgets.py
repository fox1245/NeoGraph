"""Scenario 4: compile-time and run-time budgets."""

import time

import pytest

import neograph_engine as ng

from .support import (APPROVAL_GATE, JOURNAL, admit, budget, build_registry, ceiling, channel,
                      define_graph, describe, fork_share, make_host, run_program, slow, source,
                      trace_channel, with_main)

SPIN_DEFINE = "export function define() {\n  let i = 0;\n  while (true) { i++; }\n}\n"


def _timeout_diagnostic(error):
    assert isinstance(error, ng.ProgramCompileError)
    codes = [d["code"] for d in error.diagnostics]
    assert codes == ["P_JS_TIMEOUT"], codes
    return error.diagnostics[0]


@pytest.mark.parametrize("path", ["ProgramCompiler.compile", "LocalProgramHost.compile_admit"])
def test_nonterminating_define_fails_fast_with_js_timeout(path):
    spin = source(SPIN_DEFINE, "spin.js")
    started = time.monotonic()
    with pytest.raises(ng.ProgramCompileError) as raised:
        if path == "ProgramCompiler.compile":
            ng.ProgramCompiler(build_registry()).compile(spin, budget(wall_time_ms=300))
        else:
            make_host().compile_admit(spin, budget(wall_time_ms=300))
    elapsed = time.monotonic() - started

    assert elapsed < 3.0, f"compile took {elapsed:.2f}s for a 300 ms budget"
    diagnostic = _timeout_diagnostic(raised.value)
    assert diagnostic["source_id"] == "spin.js"
    assert diagnostic["witness"]["interrupt_reason"] == "wall_time_ms"
    assert diagnostic["witness"]["max_wall_time_ms"] == 300
    assert "P_JS_TIMEOUT" in str(raised.value)


def test_host_compile_limits_bound_define_without_a_budget():
    limits = ng.ProgramJavaScriptCompileLimits()
    limits.max_wall_time_ms = 300
    compiler = ng.ProgramCompiler(build_registry(), javascript_limits=limits)
    started = time.monotonic()
    with pytest.raises(ng.ProgramCompileError) as raised:
        compiler.compile(source(SPIN_DEFINE, "spin.js"))
    assert time.monotonic() - started < 3.0
    _timeout_diagnostic(raised.value)


def assert_wall_time_witness(result, granted):
    """The timeout's witness says how far past its wall-time grant the run was when it ended."""
    wall = result.failure.witness["wall_time_ms"]
    assert set(wall) == {"granted", "used", "over_by"}, wall
    assert wall["granted"] == granted
    assert wall["used"] >= granted
    assert wall["over_by"] == wall["used"] - granted
    assert wall["used"] == result.usage.wall_time_ms
    return wall


def test_nonterminating_main_times_out_at_run_time():
    text = with_main(define_graph('g.node("w", {type: "e2e.step"}); g.entry("w"); g.exit("w");'),
                     "let i = 0;\nwhile (true) { i++; }")
    run_budget = budget(wall_time_ms=300)
    started = time.monotonic()
    _host, _version, result = run_program(text, run_budget=run_budget)
    elapsed = time.monotonic() - started

    assert result.status == ng.ProgramTerminalStatus.TimedOut, describe(result)
    assert result.failure.code == "P_RUNTIME_TIMEOUT"
    assert_wall_time_witness(result, granted=300)
    assert elapsed < 3.0
    assert result.output is None


FOREVER = define_graph("""
    g.node("think", {type: "e2e.step"});
    g.node("act", {type: "e2e.step"});
    g.entry("think");
    g.edge("think", "act");
    g.conditionalEdge("act", "e2e.forever", {again: "think", done: "__end__"});
""", name="agent_loop")


def test_runaway_loop_exhausts_the_core_step_budget():
    _host, _version, result = run_program(FOREVER, run_budget=budget(max_core_steps=7))

    assert result.status == ng.ProgramTerminalStatus.BudgetExhausted, describe(result)
    assert result.failure.code == "P_CORE_STEP_BUDGET"
    # The engine stopped the run at its grant: it used all of it, and is over by nothing (#443).
    assert result.failure.witness["core_steps"] == {"granted": 7, "used": 7, "over_by": 0}
    assert result.usage.core_steps == 7
    assert result.remaining_budget.max_core_steps == 0
    assert len(result.execution_trace) == 7
    assert result.execution_trace[:4] == ["think", "act", "think", "act"]


BOUNDED = define_graph("""
    g.node("inc", {type: "e2e.step", incr: true});
    g.node("finish", {type: "e2e.step"});
    g.entry("inc");
    g.conditionalEdge("inc", "e2e.until_target", {again: "inc", done: "finish"});
    g.exit("finish");
""", name="bounded_loop")


def test_bounded_loop_completes_within_a_generous_budget():
    generous = budget(max_core_steps=500, wall_time_ms=60_000)
    _host, _version, result = run_program(BOUNDED, {"target": 5}, run_budget=generous)

    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert channel(result, "counter") == 5
    assert trace_channel(result) == ["inc"] * 5 + ["finish"]
    assert result.usage.core_steps == 6
    assert result.remaining_budget.max_core_steps == 500 - 6


def test_same_bounded_loop_is_cut_off_by_a_tight_budget():
    _host, _version, result = run_program(BOUNDED, {"target": 5}, run_budget=budget(max_core_steps=4))
    assert result.status == ng.ProgramTerminalStatus.BudgetExhausted, describe(result)
    assert "finish" not in JOURNAL.names()


def test_program_operation_budget_rejects_a_join_before_any_dispatch():
    text = with_main(define_graph('g.node("w", {type: "e2e.step"}); g.entry("w"); g.exit("w");'),
                     """
                     const results = yield ng.all([
                       ng.callCore("work", {}, "a"),
                       ng.callCore("work", {}, "b"),
                       ng.callCore("work", {}, "c")
                     ], {max_in_flight: 2}, "fan");
                     return results.length;
                     """)
    _host, _version, result = run_program(text, run_budget=budget(max_program_operations=2))
    assert result.status == ng.ProgramTerminalStatus.BudgetExhausted, describe(result)
    assert result.failure.code == "P_PROGRAM_OPERATION_BUDGET"
    assert JOURNAL.names("started") == []


def test_run_wall_time_cancels_a_cooperative_node():
    text = define_graph("""
        g.node("slow", {type: "e2e.step", delay_ms: 5000});
        g.node("after", {type: "e2e.step"});
        g.entry("slow");
        g.edge("slow", "after");
        g.exit("after");
    """)
    started = time.monotonic()
    _host, _version, result = run_program(text, run_budget=budget(wall_time_ms=300))
    elapsed = time.monotonic() - started

    assert result.status == ng.ProgramTerminalStatus.TimedOut, describe(result)
    assert result.failure.code == "P_RUNTIME_TIMEOUT"
    assert elapsed < 2.0, f"run took {elapsed:.2f}s for a 300 ms wall budget"
    assert JOURNAL.names("cancelled") == ["slow"]
    assert "after" not in JOURNAL.names("started")


@slow
def test_run_wall_time_with_a_node_that_ignores_cancellation():
    """A Python node cannot be preempted: the result waits for it, but is still TimedOut."""
    text = define_graph("""
        g.node("stuck", {type: "e2e.step", blocking_sleep_ms: 1200});
        g.node("after", {type: "e2e.step"});
        g.entry("stuck");
        g.edge("stuck", "after");
        g.exit("after");
    """)
    _host, _version, result = run_program(text, run_budget=budget(wall_time_ms=300))
    assert result.status == ng.ProgramTerminalStatus.TimedOut, describe(result)
    assert result.failure.code == "P_RUNTIME_TIMEOUT"
    assert "after" not in JOURNAL.names("started")


# ---------------------------------------------------------------------------
# A run that completes over a budget ends with the record of that budget (#443)
#
# Usage is settled once the plan has completed, so a run can finish over its
# grant: wall time, when its last node returns after the deadline, and Core
# steps, when Core runs that were started together each began with the steps
# that were left. Each ends with a failure record whose witness has granted,
# used and over_by under the budget's key, as a model-token overshoot does.
# ---------------------------------------------------------------------------

# The Core steps of a run include the inner steps of a subgraph node. The outer
# graph has three nodes and runs in three super-steps; the review subgraph adds
# two more, so the run takes five. Core stops a graph at the steps left when it
# starts, and the outer graph needs only three of them, so a grant of three or
# four lets the run reach its end and leaves it over its grant.
REVIEWED_DRAFT = """
export function define() {
  const inner = {
    schema_version: 1,
    name: "review",
    channels: {trace: {reducer: "append"}},
    nodes: {critique: {type: "e2e.step"}, revise: {type: "e2e.step"}},
    edges: [
      {from: "__start__", to: "critique"},
      {from: "critique", to: "revise"},
      {from: "revise", to: "__end__"}
    ]
  };
  return ng.graph("outer")
    .channel("trace", {reducer: "append", initial: []})
    .node("draft", {type: "e2e.step"})
    .node("review", {type: "subgraph", definition: inner,
                     input_map: {trace: "trace"}, output_map: {trace: "trace"}})
    .node("publish", {type: "e2e.step"})
    .entry("draft").edge("draft", "review").edge("review", "publish").exit("publish");
}
"""


def run_reviewed_draft(steps):
    host = make_host(registry=build_registry(extra_node_types=("subgraph",)))
    return run_program(REVIEWED_DRAFT, host=host, run_budget=budget(max_core_steps=steps))


@pytest.mark.parametrize("granted", [3, 4])
def test_a_run_that_completes_over_its_core_step_grant_names_the_budget(granted):
    _host, _version, result = run_reviewed_draft(granted)

    # Every node ran: the plan completed, and the grant was passed on the way.
    assert result.execution_trace == ["draft", "review", "publish"]
    assert JOURNAL.names("ran") == ["draft", "critique", "revise", "publish"]
    assert result.status == ng.ProgramTerminalStatus.BudgetExhausted, describe(result)
    assert result.failure.code == "P_CORE_STEP_BUDGET", describe(result)
    assert result.failure.witness["core_steps"] == {
        "granted": granted, "used": 5, "over_by": 5 - granted}
    assert result.usage.core_steps == 5
    assert "5" in result.failure.message and str(granted) in result.failure.message
    assert result.remaining_budget.max_core_steps == 0


def test_the_same_run_completes_within_a_grant_that_covers_its_steps():
    _host, _version, result = run_reviewed_draft(5)

    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert result.failure is None
    assert result.usage.core_steps == 5


def test_a_run_that_completes_after_its_wall_time_grant_ends_timed_out_with_the_witness():
    """The last node returns at about 900 ms, past the 300 ms granted; it cannot be preempted."""
    text = define_graph("""
        g.node("stuck", {type: "e2e.step", blocking_sleep_ms: 900});
        g.entry("stuck");
        g.exit("stuck");
    """)
    _host, _version, result = run_program(text, run_budget=budget(wall_time_ms=300))

    # The deadline ends the run TimedOut, and the record says how far over it went.
    assert result.status == ng.ProgramTerminalStatus.TimedOut, describe(result)
    assert result.failure.code == "P_RUNTIME_TIMEOUT"
    assert result.failure.message == "Program wall-time budget (300 ms) expired"
    wall = assert_wall_time_witness(result, granted=300)
    assert wall["used"] >= 900, wall


def test_run_budget_must_equal_the_admitted_budget():
    """compile_admit(source, budget) declares that exact budget (minimum == maximum).

    docs/python-binding.md passes the same `run_budget` to compile_admit() and
    run() but never says that any other run budget, larger or smaller, is
    rejected.
    """
    host = make_host()
    admitted = budget(max_core_steps=20)
    version = admit(host, BOUNDED, admitted)

    for steps in (10, 30):
        with pytest.raises(RuntimeError, match="outside its admitted bounds"):
            host.run(version, {"target": 1}, budget(max_core_steps=steps))
    result = host.run(version, {"target": 1}, admitted)
    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)


def test_budget_rejection_names_the_offending_resource():
    host = make_host()
    version = admit(host, BOUNDED, budget(max_core_steps=20))
    with pytest.raises(RuntimeError) as raised:
        host.run(version, {"target": 1}, budget(max_core_steps=10))
    message = str(raised.value)
    assert "max_core_steps" in message and "P_START_BUDGET" in message, message


def test_zero_model_token_ceiling_is_accepted_or_names_the_field():
    try:
        make_host(budget_ceiling=ceiling(model_tokens=0, monetary_microunits=0))
    except ValueError as error:
        assert "model_tokens" in str(error), str(error)


# ---------------------------------------------------------------------------
# A fork's budget and the paused run it forks (#414)
#
# Amounts are split: the fork's share is taken out of the paused run's
# remainder. max_concurrency is a limit: the fork carries its own and the
# source keeps its own. A fork that would leave the source without the wall
# time or the core steps every resume needs is refused at fork(), with
# P_FORK_SOURCE_BUDGET, and changes nothing.
# ---------------------------------------------------------------------------

GATE_SHAPES = {
    "define": APPROVAL_GATE,
    "main": with_main(APPROVAL_GATE, 'return yield ng.callCore("release", input, "run");'),
}


def paused_gate(shape):
    host = make_host()
    run_budget = budget(max_concurrency=4)
    version = admit(host, GATE_SHAPES[shape], run_budget)
    paused = host.run(version, {}, run_budget)
    assert paused.status == ng.ProgramTerminalStatus.Interrupted, describe(paused)
    return host, version, paused


@pytest.mark.parametrize("shape", sorted(GATE_SHAPES))
def test_a_fork_takes_its_share_and_the_source_keeps_the_rest(shape):
    host, version, paused = paused_gate(shape)
    left = paused.remaining_budget
    share = fork_share(left)

    forked = host.fork(paused.run_id, {"approved": False}, version, share).wait()
    assert forked.status == ng.ProgramTerminalStatus.Completed, describe(forked)

    resumed = host.resume(paused.run_id, {"approved": True}).wait()
    assert resumed.status == ng.ProgramTerminalStatus.Completed, describe(resumed)
    # What the source had left, less the fork's share, is all it could use.
    assert resumed.usage.core_steps + resumed.remaining_budget.max_core_steps == (
        left.max_core_steps - share.max_core_steps)
    assert resumed.usage.program_operations + resumed.remaining_budget.max_program_operations == (
        left.max_program_operations - share.max_program_operations)
    assert resumed.remaining_budget.wall_time_ms <= left.wall_time_ms - share.wall_time_ms


@pytest.mark.parametrize("shape", sorted(GATE_SHAPES))
def test_a_fork_does_not_consume_the_sources_concurrency(shape):
    host, version, paused = paused_gate(shape)
    left = paused.remaining_budget
    assert left.max_concurrency == 4
    share = fork_share(left)
    assert share.max_concurrency == left.max_concurrency

    forked = host.fork(paused.run_id, {"approved": False}, version, share).wait()
    assert forked.status == ng.ProgramTerminalStatus.Completed, describe(forked)

    resumed = host.resume(paused.run_id, {"approved": True}).wait()
    assert resumed.status == ng.ProgramTerminalStatus.Completed, describe(resumed)
    assert resumed.remaining_budget.max_concurrency == left.max_concurrency


@pytest.mark.parametrize("field", ["wall_time_ms", "max_core_steps"])
@pytest.mark.parametrize("shape", sorted(GATE_SHAPES))
def test_a_fork_that_would_leave_the_source_without_a_resume_resource_is_refused(shape, field):
    host, version, paused = paused_gate(shape)
    left = paused.remaining_budget
    whole = fork_share(left, **{field: getattr(left, field)})

    with pytest.raises(ng.ProgramDiagnosticError) as raised:
        host.fork(paused.run_id, {"approved": False}, version, whole)
    [diagnostic] = raised.value.diagnostics
    assert diagnostic["code"] == "P_FORK_SOURCE_BUDGET", diagnostic
    assert field in diagnostic["message"], diagnostic

    # The refusal changed nothing: the source resumes with all it had.
    resumed = host.resume(paused.run_id, {"approved": True}).wait()
    assert resumed.status == ng.ProgramTerminalStatus.Completed, describe(resumed)
    assert resumed.usage.core_steps + resumed.remaining_budget.max_core_steps == left.max_core_steps


@pytest.mark.parametrize("shape", sorted(GATE_SHAPES))
def test_a_fork_larger_than_the_source_remainder_is_still_refused(shape):
    host, version, paused = paused_gate(shape)
    left = paused.remaining_budget
    too_much = fork_share(left, wall_time_ms=left.wall_time_ms + 1)

    with pytest.raises(ng.ProgramDiagnosticError) as raised:
        host.fork(paused.run_id, {"approved": False}, version, too_much)
    assert [d["code"] for d in raised.value.diagnostics] == ["P_FORK_BUDGET"]
