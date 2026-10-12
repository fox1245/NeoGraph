"""40 - Attempt timeout: a slow node is stopped, fails with NG.Timeout and succeeds on the retry.

`retryPolicy("work", {attempt_timeout_ms: 400, max_retries: 1})` limits every attempt of
`work` to 400 ms. When the limit passes, the attempt is cancelled through its own cancel
token (`input.ctx.cancel_token`) and fails with NG.Timeout, which is retried like any other
failure. A Python body cannot be preempted, so a node that may block should poll the token.
Whatever a timed-out attempt returns later is discarded. Run 1 is slow once and then fast;
run 2 is slow every time, so the retries run out and the Program ends Failed with NG.Timeout.
`max_node_retries` caps retries across the run and `usage.node_retries` counts them.

Run:
    python 40_program_attempt_timeout.py

smoke-expect: run 1: Completed, attempts 2, node retries 1 of 5
smoke-expect: first attempt saw its cancel_token flip: True
smoke-expect: run 2: Failed NG.Timeout after 2 attempts
smoke-forbid: [neograph] lint
"""

import time

import neograph_engine as ng
from _program import admit, budget, channel, make_host, node

CALLS = []                                # one entry per attempt: did it see the cancellation?


@node("work.slow")
def work(name, input, config):
    CALLS.append(False)
    token = input.ctx.cancel_token
    if len(CALLS) == 1 or input.state.get("always_slow"):
        deadline = time.monotonic() + 5.0                 # would take five seconds
        while time.monotonic() < deadline:
            if token.is_cancelled():                      # the attempt timeout fired
                CALLS[-1] = True
                raise RuntimeError("cancelled by its timeout")
            time.sleep(0.005)
    return [ng.ChannelWrite("trace", [name])]


PROGRAM = r"""
export function define() {
  return ng.graph("timed")
    .channel("trace", {reducer: "append", initial: []})
    .channel("always_slow", {reducer: "overwrite", initial: false})
    .node("work", {type: "work.slow"})
    .entry("work").exit("work")
    .retryPolicy("work", {attempt_timeout_ms: 400, max_retries: 1, initial_delay_ms: 10,
                          backoff_multiplier: 1, max_delay_ms: 10});
}
"""

host = make_host(["work.slow"])
run_budget = budget(max_node_retries=5)
version = admit(host, PROGRAM, run_budget, "timed.js")

started = time.monotonic()
first = host.run(version, {}, run_budget)
print(f"run 1: {first.status.name}, attempts {len(CALLS)}, "
      f"node retries {first.usage.node_retries} of {run_budget.max_node_retries}")
print("first attempt saw its cancel_token flip:", CALLS[0], f"({time.monotonic() - started:.1f} s in all)")

CALLS.clear()
second = host.run(version, {"always_slow": True}, run_budget)
failure = second.failure.witness["node_failure"]
print(f"run 2: {second.status.name} {second.failure.code} after {failure['attempts']} attempts")

assert first.status == ng.ProgramTerminalStatus.Completed and channel(first, "trace") == ["work"]
assert first.usage.node_retries == 1 and time.monotonic() - started < 4
assert second.status == ng.ProgramTerminalStatus.Failed and second.failure.code == "NG.Timeout"
assert failure["attempts"] == 2 and failure["kind"] == "exhausted"
