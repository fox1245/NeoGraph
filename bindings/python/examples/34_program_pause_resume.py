"""34 - Pause and resume: a Program stops before a node and continues when an operator answers.

`g.interruptBefore("b")` pauses the run before node b. The result comes back with status
Interrupted and an `interrupt` that says which node is waiting and carries the pending call
that `host.resume(run_id, value)` answers. The node receives the answer as
`input.ctx.resume_value`, and `main()` continues from the `yield ng.callCore(...)` that
paused. The resumed result lists only the Core nodes that ran after the pause.

Run:
    python 34_program_pause_resume.py

smoke-expect: paused: Interrupted before b, trace ['a']
smoke-expect: pending call: Input, Awaiting
smoke-expect: resumed: Completed, trace ['b', 'c']
smoke-expect: main() returned: {'answer': 'ship it', 'trace': ['a', 'b', 'c']}
smoke-forbid: [neograph] lint
"""

import neograph_engine as ng
from _program import admit, budget, make_host, node


@node("steps.run")
def run_step(name, input, config):
    writes = [ng.ChannelWrite("trace", [name])]
    if config.get("takes_answer"):
        writes.append(ng.ChannelWrite("answer", input.ctx.resume_value))
    return writes


PROGRAM = r"""
export function define() {
  return ng.graph("pipeline")
    .channel("trace", {reducer: "append", initial: []})
    .channel("answer", {reducer: "overwrite", initial: null})
    .node("a", {type: "steps.run"})
    .node("b", {type: "steps.run", takes_answer: true})
    .node("c", {type: "steps.run"})
    .entry("a").edge("a", "b").edge("b", "c").exit("c")
    .interruptBefore("b");
}
export function* main(input) {
  const reply = yield ng.callCore("pipeline", input, "pipeline:main");
  return {answer: reply.answer, trace: reply.trace};
}
"""

host = make_host(["steps.run"])
run_budget = budget()
version = admit(host, PROGRAM, run_budget, "pipeline.js")

paused = host.run(version, {}, run_budget)
pending = paused.interrupt.pending_input
print(f"paused: {paused.status.name} before {paused.interrupt.core_node}, trace {paused.execution_trace}")
print(f"pending call: {pending.kind.name}, {pending.state.name}")

# With one awaiting call, resume() needs no id. Pass pending_id=pending.call_id to name it.
resumed = host.resume(paused.run_id, "ship it", pending.call_id).wait()
print(f"resumed: {resumed.status.name}, trace {resumed.execution_trace}")
print("main() returned:", resumed.output)

assert resumed.run_id == paused.run_id                   # the same run continues
assert paused.status == ng.ProgramTerminalStatus.Interrupted and paused.execution_trace == ["a"]
assert resumed.status == ng.ProgramTerminalStatus.Completed and resumed.execution_trace == ["b", "c"]
assert resumed.output == {"answer": "ship it", "trace": ["a", "b", "c"]}
