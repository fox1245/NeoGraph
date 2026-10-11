"""Scenario 11: attempt timeouts and deadline-aware backoff (#364, WP12).

`retryPolicy(node, {attempt_timeout_ms, max_retries, on_failure})` gives each
attempt of a node a time limit. A timed-out attempt is cancelled through its
own cancel token and fails with NG.Timeout, which is retried and can be routed
along the node's failure edge. Whatever the cancelled attempt returns later is
discarded: it never reaches the state, the result or the trace. The retry
starts only once the cancelled attempt has stopped; one that does not stop by
its stop deadline fails the run with NG.EffectUncertain. A backoff that would
cross the run's wall-time budget stops retrying with kind "deadline" instead of
sleeping until the budget ends the run.
"""

import time

import neograph_engine as ng

from .support import (JOURNAL, budget, channel, define_graph, describe, run_program, slow,
                      trace_channel)

_FAST = "initial_delay_ms: 10, backoff_multiplier: 1, max_delay_ms: 10"


def work_graph(work_config, policy, routed=True):
    """work (entry, exit) with `policy`; with `routed`, its failures go to handler."""
    route = ', on_failure: "handler", error_channel: "failures"' if routed else ""
    return define_graph(f"""
    g.channel("failures", {{reducer: "append", initial: []}});
    g.node("work", Object.assign({{type: "e2e.step"}}, {work_config}));
    g.node("handler", {{type: "e2e.step"}});
    g.entry("work");
    g.exit("work");
    g.exit("handler");
    g.retryPolicy("work", {{{policy}{route}}});
""", name="timed")


def run_timed(text, run_budget=None):
    started = time.monotonic()
    _host, _version, result = run_program(text, run_budget=run_budget)
    return result, time.monotonic() - started


# ---------------------------------------------------------------------------
# A timed-out attempt is cancelled and retried
# ---------------------------------------------------------------------------


def test_a_slow_python_node_times_out_and_recovers_on_retry():
    # The first call would take 5 s; it polls its cancel token, which the
    # 300 ms timeout cancels.
    text = work_graph("{slow_first_ms: 5000, cooperative: true}",
                      f"attempt_timeout_ms: 300, max_retries: 1, {_FAST}")
    result, elapsed = run_timed(text)

    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert elapsed < 3.0, f"took {elapsed:.2f}s"
    assert trace_channel(result) == ["work"]
    assert result.execution_trace == ["work"]
    assert channel(result, "failures") == []
    assert JOURNAL.names("started").count("work") == 2
    assert JOURNAL.names("cancelled") == ["work"], "the first attempt saw its token cancelled"
    assert "handler" not in JOURNAL.names("started")


def test_a_node_that_always_times_out_routes_to_its_handler():
    text = work_graph("{delay_ms: 5000}", f"attempt_timeout_ms: 200, max_retries: 1, {_FAST}")
    result, elapsed = run_timed(text)

    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert elapsed < 3.0, f"took {elapsed:.2f}s"
    assert channel(result, "failures") == [{
        "node": "work", "kind": "exhausted", "code": "NG.Timeout", "attempts": 2,
        "retryable": True, "step": 0, "routed_to": "handler", "effects": []}]
    # The timed-out attempts wrote nothing; only the handler did.
    assert trace_channel(result) == ["handler"]
    assert result.execution_trace == ["work", "handler"]
    assert JOURNAL.names("cancelled") == ["work", "work"]


def test_without_a_failure_edge_timeout_exhaustion_fails_the_run_with_ng_timeout():
    text = work_graph("{delay_ms: 5000}", f"attempt_timeout_ms: 200, max_retries: 1, {_FAST}",
                      routed=False)
    result, _elapsed = run_timed(text)

    assert result.status == ng.ProgramTerminalStatus.Failed, describe(result)
    assert result.failure.code == "NG.Timeout"
    assert result.failure.witness["node_failure"] == {
        "node": "work", "code": "NG.Timeout", "kind": "exhausted", "attempts": 2,
        "retryable": True, "routable": True}


# ---------------------------------------------------------------------------
# The late completion
# ---------------------------------------------------------------------------


def test_a_late_completion_never_shows_up_in_the_result_or_the_trace():
    # The first call ignores cancellation and returns its writes 1.2 s in,
    # well after its 300 ms timeout but before its stop deadline (1.3 s).
    text = work_graph("{slow_first_ms: 1200}", f"attempt_timeout_ms: 300, max_retries: 1, {_FAST}")
    result, elapsed = run_timed(text)

    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    # Both calls ran to the end ...
    assert JOURNAL.names("ran").count("work") == 2
    # ... but only the retry's writes and trace entry count.
    assert trace_channel(result) == ["work"]
    assert result.execution_trace == ["work"]
    assert channel(result, "failures") == []
    # The retry started only after the late call returned.
    assert elapsed >= 1.2


@slow
def test_an_attempt_that_ignores_cancellation_past_its_stop_deadline_fails_the_run():
    # Timeout 150 ms, stop deadline a second later; the call blocks 2.5 s. Its
    # effects are uncertain: no retry, no route, the run fails.
    text = work_graph("{blocking_sleep_ms: 2500}", f"attempt_timeout_ms: 150, max_retries: 2, {_FAST}")
    result, elapsed = run_timed(text)

    assert result.status == ng.ProgramTerminalStatus.Failed, describe(result)
    assert result.failure.code == "NG.EffectUncertain"
    node_failure = result.failure.witness["node_failure"]
    assert node_failure["routable"] is False and node_failure["retryable"] is False
    assert node_failure["attempts"] == 1
    assert elapsed < 2.3, f"the run waited {elapsed:.2f}s for the stuck call"
    assert JOURNAL.names("started") == ["work"]
    assert channel(result, "failures") in (None, [])


# ---------------------------------------------------------------------------
# Deadline-aware backoff
# ---------------------------------------------------------------------------

_SLOW_BACKOFF = "max_retries: 3, initial_delay_ms: 5000, max_delay_ms: 5000"


def test_a_backoff_that_would_cross_the_wall_time_budget_stops_with_kind_deadline():
    text = work_graph("{boom: true}", _SLOW_BACKOFF)
    result, elapsed = run_timed(text, budget(wall_time_ms=1500))

    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert elapsed < 1.5, f"took {elapsed:.2f}s: the backoff slept"
    assert channel(result, "failures") == [{
        "node": "work", "kind": "deadline", "code": "NG.TaskFailed", "attempts": 1,
        "retryable": True, "step": 0, "routed_to": "handler", "effects": []}]
    assert JOURNAL.names("started").count("work") == 1
    assert trace_channel(result) == ["handler"]


def test_without_a_failure_edge_a_deadline_stop_fails_instead_of_timing_out():
    text = work_graph("{boom: true}", _SLOW_BACKOFF, routed=False)
    result, elapsed = run_timed(text, budget(wall_time_ms=1500))

    assert result.status == ng.ProgramTerminalStatus.Failed, describe(result)
    assert result.failure.code == "NG.TaskFailed"
    assert result.failure.witness["node_failure"]["kind"] == "deadline"
    assert elapsed < 1.5, f"took {elapsed:.2f}s"


def test_a_backoff_within_the_budget_still_retries():
    text = work_graph("{boom_first: true}", "max_retries: 2, initial_delay_ms: 100")
    result, _elapsed = run_timed(text, budget(wall_time_ms=5000))
    assert result.status == ng.ProgramTerminalStatus.Completed, describe(result)
    assert trace_channel(result) == ["work"]
    assert JOURNAL.names("started").count("work") == 2
    assert channel(result, "failures") == []
