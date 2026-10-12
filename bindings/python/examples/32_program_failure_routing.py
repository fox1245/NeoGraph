"""32 - Failure routing: a node's final failure goes to a handler that waits for an operator.

TEST fails with an author code (`app.test_failed`). `retryPolicy("TEST", {...})` retries it
twice and then, instead of failing the Program, routes the failure to ASK_USER, which has
`interruptBefore`: the run pauses with a structured failure record. The operator answers
through `host.resume`. "retry" runs IMPLEMENT and TEST again and the Program completes;
"abort" runs ABORT, a failure exit, so the Program ends Failed with the declared code and
never reports Completed. `max_node_retries` caps retries across the whole run.

Run:
    python 32_program_failure_routing.py

smoke-expect: paused at ASK_USER, reason: failure_route
smoke-expect: record: TEST app.test_failed, exhausted after 3 attempts
smoke-expect: operator retries: Completed, trace ['ASK_USER', 'IMPLEMENT', 'TEST']
smoke-expect: operator aborts: Failed plan.user_abort at ABORT
smoke-expect: node retries: 2 of 4
smoke-forbid: [neograph] lint
"""

import neograph_engine as ng
from _program import admit, budget, channel, condition, make_host, node


class ChecksFailed(Exception):
    """The binding reads `code` and `retryable` from the exception. str(exc) never reaches a record."""
    code = "app.test_failed"


@node("fix.implement")
def implement(name, input, config):
    return [ng.ChannelWrite("trace", [name])]


@node("fix.test")
def run_tests(name, input, config):
    if not input.state.get("fixed"):
        raise ChecksFailed("2 tests failed")
    return [ng.ChannelWrite("trace", [name])]


@node("fix.ask_user")
def ask_user(name, input, config):
    answer = input.ctx.resume_value or {}              # what the operator sent to host.resume
    return [ng.ChannelWrite("trace", [name]),
            ng.ChannelWrite("decision", answer.get("decision", "abort")),
            ng.ChannelWrite("fixed", bool(answer.get("fix")))]


@condition("fix.decision")
def decision(state):
    return state.get("decision")


PROGRAM = r"""
export function define() {
  return ng.graph("fix")
    .channel("trace", {reducer: "append", initial: []})
    .channel("failures", {reducer: "append", initial: []})
    .channel("decision", {reducer: "overwrite", initial: ""})
    .channel("fixed", {reducer: "overwrite", initial: false})
    .node("IMPLEMENT", {type: "fix.implement"})
    .node("TEST", {type: "fix.test"})
    .node("ASK_USER", {type: "fix.ask_user"})
    .node("ABORT", {type: "fix.implement"})
    .entry("IMPLEMENT").edge("IMPLEMENT", "TEST").exit("TEST")
    .conditionalEdge("ASK_USER", "fix.decision", {retry: "IMPLEMENT", abort: "ABORT"})
    .retryPolicy("TEST", {max_retries: 2, initial_delay_ms: 10, backoff_multiplier: 1,
                          max_delay_ms: 10, on_failure: "ASK_USER", error_channel: "failures"})
    .interruptBefore("ASK_USER")
    .failureExit("ABORT", {code: "plan.user_abort"});
}
"""

host = make_host(["fix.implement", "fix.test", "fix.ask_user"], ["fix.decision"])
run_budget = budget(max_node_retries=4)
version = admit(host, PROGRAM, run_budget, "fix.js")

paused = host.run(version, {}, run_budget)
value = paused.interrupt.pending_input.core_interrupt_value
record = value["failure"]
print("paused at", paused.interrupt.core_node + ", reason:", value["reason"])
print(f"record: {record['node']} {record['code']}, {record['kind']} after {record['attempts']} attempts")
print(f"node retries: {paused.usage.node_retries} of {run_budget.max_node_retries}")

retried = host.resume(paused.run_id, {"decision": "retry", "fix": True}).wait()
print("operator retries:", retried.status.name + ", trace", retried.execution_trace)

# A second run, same Program version, and this time the operator gives up.
paused_again = host.run(version, {}, run_budget)
aborted = host.resume(paused_again.run_id, {"decision": "abort"}).wait()
print("operator aborts:", aborted.status.name, aborted.failure.code, "at", aborted.failure.core_node)

assert paused.status == ng.ProgramTerminalStatus.Interrupted and paused.failure is None
assert (record["code"], record["attempts"], record["routed_to"]) == ("app.test_failed", 3, "ASK_USER")
assert channel(retried, "failures") == [record]        # the record stays in the error channel
assert retried.status == ng.ProgramTerminalStatus.Completed
assert aborted.status == ng.ProgramTerminalStatus.Failed
assert aborted.failure.code == "plan.user_abort" and aborted.failure.core_node == "ABORT"
