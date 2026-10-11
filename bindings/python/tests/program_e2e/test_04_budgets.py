"""Scenario 4: compile-time and run-time budgets."""

import time

import pytest

import neograph_engine as ng

from .support import (JOURNAL, admit, budget, build_registry, ceiling, channel, define_graph,
                      describe, make_host, run_program, slow, source, trace_channel,
                      with_main)

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


def test_nonterminating_main_times_out_at_run_time():
    text = with_main(define_graph('g.node("w", {type: "e2e.step"}); g.entry("w"); g.exit("w");'),
                     "let i = 0;\nwhile (true) { i++; }")
    run_budget = budget(wall_time_ms=300)
    started = time.monotonic()
    _host, _version, result = run_program(text, run_budget=run_budget)
    elapsed = time.monotonic() - started

    assert result.status == ng.ProgramTerminalStatus.TimedOut, describe(result)
    assert result.failure.code == "P_RUNTIME_TIMEOUT"
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
