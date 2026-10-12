"""31 - any_of join: the compiler's own suggestion repairs a join that stalls.

Node D joins two sources, B and C. When B's check fails, a fix-up node FIX takes over and
routes to D, but a plain barrier `["B", "C"]` counts only B itself, so D never fires and the
run ends Failed with P_CORE_BARRIER_STALLED. The compiler saw this before the run (P_CORE_E15)
and attaches a repair to the diagnostic: let FIX stand in for B with an `{any_of: [...]}`
slot, which is filled by whichever alternative arrives. The example applies that suggestion
to the source, admits the result and runs it again.

Run:
    python 31_program_any_of_join.py

smoke-expect: first run: Failed P_CORE_BARRIER_STALLED, missing ['B']
smoke-expect: compiler suggests: [{"any_of": ["B", "FIX"]}, "C"]
smoke-expect: repaired run: Completed, D ran once
smoke-forbid: [neograph] lint
"""

import json

import neograph_engine as ng
from _program import admit, budget, condition, make_host, node


@node("join.step")
def step(name, input, config):
    return [ng.ChannelWrite("trace", [name])]


@condition("join.check")
def check(state):
    return state.get("check")          # "pass" or "fail", given as the run input


TEMPLATE = r"""
export function define() {
  return ng.graph("sources")
    .channel("trace", {reducer: "append", initial: []})
    .channel("check", {reducer: "overwrite", initial: "pass"})
    .node("A", {type: "join.step"}).node("B", {type: "join.step"})
    .node("C", {type: "join.step"}).node("FIX", {type: "join.step"})
    .node("D", {type: "join.step"})
    .entry("A").edge("A", "B").edge("A", "C")
    .conditionalEdge("B", "join.check", {pass: "D", fail: "FIX"})
    .edge("FIX", "D").edge("C", "D")
    .barrier("D", WAIT_FOR)
    .exit("D");
}
export function* main(input) {
  return yield ng.callCore("sources", input, "sources:main");
}
"""

host = make_host(["join.step"], ["join.check"])
run_budget = budget()

# 1. Two plain members. B can fail over to FIX, which a plain list does not count.
program = TEMPLATE.replace("WAIT_FOR", json.dumps(["B", "C"]))
version = admit(host, program, run_budget, "sources.js")
first = host.run(version, {"check": "fail"}, run_budget)
stall = first.failure.witness["stalled_barriers"][0]
print("first run:", first.status.name, first.failure.code + ", missing", stall["missing"])

# 2. The compiler's warning carries the repair as data: an edit of the barrier's wait_for.
bundle = host.compile(ng.ProgramSource.from_javascript("sources.js", program), run_budget)
warning = next(d for d in bundle.compile_diagnostics if d["code"] == "P_CORE_E15")
suggestion = warning["help"]["suggestions"][0]
wait_for = suggestion["edits"][0]["value"]
print("compiler suggests:", json.dumps(wait_for))
print("  (" + suggestion["message"] + ")")

# 3. Apply it and run again. The same suggestion appears in the stalled run's witness.
assert stall["suggested_wait_for"] == wait_for
repaired = admit(host, TEMPLATE.replace("WAIT_FOR", json.dumps(wait_for)), run_budget,
                 "sources.js")
second = host.run(repaired, {"check": "fail"}, run_budget)
assert second.execution_trace.count("D") == 1
print("repaired run:", second.status.name + ", D ran once")
print("trace has FIX:", "FIX" in second.execution_trace)
# The passing path never needed the stand-in.
third = host.run(repaired, {"check": "pass"}, run_budget)
print("check passes:", third.status.name + ", FIX ran:", "FIX" in third.execution_trace)

assert first.status == ng.ProgramTerminalStatus.Failed and stall["missing"] == ["B"]
assert second.status == ng.ProgramTerminalStatus.Completed and "FIX" in second.execution_trace
assert third.status == ng.ProgramTerminalStatus.Completed and "FIX" not in third.execution_trace
