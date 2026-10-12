"""30 - Barrier join: three reviewers run in parallel and one node waits for all of them.

`g.barrier("merge", [...])` makes `merge` run only after every listed node has routed to it.
Run 1: all three reviewers approve, `merge` runs once and joins their notes.
Run 2: reviewer_c routes to `escalate` instead of `merge`. The join can never fire, so the
Program does not report Completed: it ends Failed with P_CORE_BARRIER_STALLED and
`failure.witness` names the node that never arrived. The compiler had already warned about
this shape (P_CORE_E15) before anything ran.

Run:
    python 30_program_barrier_join.py

smoke-expect: run 1: Completed, joined ['reviewer_a', 'reviewer_b', 'reviewer_c']
smoke-expect: run 2: Failed P_CORE_BARRIER_STALLED at merge
smoke-expect: missing: ['reviewer_c']
smoke-expect: compiler warned: P_CORE_E15
smoke-forbid: [neograph] lint
"""

import neograph_engine as ng
from _program import admit, budget, condition, make_host, node


@node("review.plan")
def plan(name, input, config):
    return [ng.ChannelWrite("reviews", [name])]


@node("review.reviewer")
def reviewer(name, input, config):
    return [ng.ChannelWrite("reviews", [name])]


@node("review.merge")
def merge(name, input, config):
    notes = [note for note in input.state.get("reviews") if note.startswith("reviewer_")]
    return [ng.ChannelWrite("joined", sorted(notes))]    # fan-out order is not defined


@condition("review.verdict")
def verdict(state):
    return "escalate" if state.get("escalate") else "approve"


PROGRAM = r"""
export function define() {
  return ng.graph("review")
    .channel("reviews", {reducer: "append", initial: []})
    .channel("joined", {reducer: "overwrite", initial: []})
    .channel("escalate", {reducer: "overwrite", initial: false})
    .node("plan", {type: "review.plan"})
    .node("reviewer_a", {type: "review.reviewer"})
    .node("reviewer_b", {type: "review.reviewer"})
    .node("reviewer_c", {type: "review.reviewer"})
    .node("escalate", {type: "review.reviewer"})
    .node("merge", {type: "review.merge"})
    .entry("plan")
    .edge("plan", "reviewer_a").edge("plan", "reviewer_b").edge("plan", "reviewer_c")
    .edge("reviewer_a", "merge").edge("reviewer_b", "merge")
    .conditionalEdge("reviewer_c", "review.verdict", {approve: "merge", escalate: "escalate"})
    .barrier("merge", ["reviewer_a", "reviewer_b", "reviewer_c"])
    .exit("merge");
}
export function* main(input) {
  return yield ng.callCore("review", input, "review:main");
}
"""

host = make_host(["review.plan", "review.reviewer", "review.merge"], ["review.verdict"])
run_budget = budget()
version = admit(host, PROGRAM, run_budget, "review.js")

# The compiler already sees that reviewer_c can leave the join. Warnings come with the bundle.
bundle = host.compile(ng.ProgramSource.from_javascript("review.js", PROGRAM), run_budget)
warning = next(d for d in bundle.compile_diagnostics if d["code"] == "P_CORE_E15")
print("compiler warned:", warning["code"], "-", warning["message"][:60] + "...")

ok = host.run(version, {}, run_budget)
print("run 1:", ok.status.name + ", joined", ok.output["joined"])

stalled = host.run(version, {"escalate": True}, run_budget)
join = stalled.failure.witness["stalled_barriers"][0]
print("run 2:", stalled.status.name, stalled.failure.code, "at", stalled.failure.core_node)
print("received:", sorted(join["received"]))
print("missing:", join["missing"])

assert ok.status == ng.ProgramTerminalStatus.Completed
assert ok.output["joined"] == ["reviewer_a", "reviewer_b", "reviewer_c"]
assert stalled.status == ng.ProgramTerminalStatus.Failed
assert stalled.failure.code == "P_CORE_BARRIER_STALLED" and stalled.failure.core_node == "merge"
assert join["missing"] == ["reviewer_c"] and "merge" not in stalled.execution_trace
