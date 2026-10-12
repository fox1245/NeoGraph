"""35 - Fork: answer a paused run two ways without paying for its model call twice.

A run pauses while a human reviews a drafted reply: `draft` (a bound `llm_call`) has finished
and `check` is waiting for approval. `host.fork(run_id, answer, version, budget)` starts a
second run from that exact checkpoint with a different answer. The finished model call is
replayed from the checkpoint, not sent again and not charged again, so the scripted model
sees one request in total. The original run can still be resumed with its own answer. The
fork's budget is carved out of what the paused run has left, so give it a share, not all.

Run:
    python 35_program_fork.py

smoke-expect: paused: Interrupted after 1 model call
smoke-expect: fork answers approved: Completed, verdict approved
smoke-expect: original answers rejected: Completed, verdict rejected
smoke-expect: model calls: 1
smoke-forbid: [neograph] lint
"""

import neograph_engine as ng
from _program import admit, budget, make_model_host, model_url, node
from _scripted import ScriptedModel, say


@node("review.start")
def start(name, input, config):
    return [ng.ChannelWrite("trace", [name])]


@node("review.check")
def check(name, input, config):
    answer = input.ctx.resume_value
    if answer is None:                                  # first time: ask and pause
        raise ng.NodeInterrupt({"question": "publish the draft?"}, reason="needs approval")
    return [ng.ChannelWrite("verdict", answer)]


PROGRAM = r"""
export function define() {
  return ng.graph("review")
    .channel("messages", {reducer: "append", initial: [{role: "user", content: "Name a river."}]})
    .channel("trace", {reducer: "append", initial: []})
    .channel("verdict", {reducer: "overwrite", initial: null})
    .node("start", {type: "review.start"})
    .node("draft", {type: "llm_call"})
    .node("check", {type: "review.check"})
    .entry("start").edge("start", "draft").edge("start", "check")
    .exit("draft").exit("check");
}
export function* main(input) {
  const reply = yield ng.callCore("review", input, "review:main");
  return {verdict: reply.verdict, draft: reply.final_response};
}
"""

with ScriptedModel([say("The Seine.").with_usage(20, 10), say("never asked for")]) as model:
    host = make_model_host(model_url(model), ["review.start", "review.check"], worker_count=2)
    run_budget = budget(model_tokens=1_000, monetary_microunits=1_000_000)
    version = admit(host, PROGRAM, run_budget, "review.js")

    paused = host.run(version, {}, run_budget)
    print(f"paused: {paused.status.name} after {model.call_count} model call, "
          f"tokens charged {paused.usage.model_tokens}")

    # A share of what the paused run has left; the rest stays with the original.
    share = budget(wall_time_ms=4_000, max_concurrency=2, max_program_operations=8, max_core_steps=20)
    forked = host.fork(paused.run_id, "approved", version, share).wait()
    print(f"fork answers approved: {forked.status.name}, verdict {forked.output['verdict']}, "
          f"tokens charged {forked.usage.model_tokens}")

    original = host.resume(paused.run_id, "rejected").wait()
    print(f"original answers rejected: {original.status.name}, verdict {original.output['verdict']}")
    print("model calls:", model.call_count)

    assert paused.status == ng.ProgramTerminalStatus.Interrupted and paused.usage.model_tokens == 30
    assert forked.run_id != paused.run_id and forked.output["verdict"] == "approved"
    assert original.output["verdict"] == "rejected"
    assert forked.output["draft"] == original.output["draft"] == "The Seine."   # the replayed reply
    assert forked.usage.model_tokens == 0 and model.call_count == 1
