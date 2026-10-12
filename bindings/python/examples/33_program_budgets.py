"""33 - Budgets: a model-token budget ends the run, and a budget that differs from the admitted one is refused.

A Program is admitted together with the budget it will run under, and `host.run` must pass
exactly that budget. Here two `llm_call` nodes run in a row. The scripted model reports 600
tokens for each reply and the run is allowed 1,000. The second call is sent, because its
worst-case reservation fits the 400 tokens left; its reply then takes the run over, so the
Program ends BudgetExhausted, with `failure.code` NG.BudgetExhausted: the same status and code as a
run whose next call is refused before it is sent. A run started with a different budget never
starts: it raises P_START_BUDGET and names the field that differs.

Run:
    python 33_program_budgets.py

smoke-expect: status: BudgetExhausted
smoke-expect: failure: NG.BudgetExhausted
smoke-expect: model tokens: 1200 used of 1000
smoke-expect: model calls: 2
smoke-expect: refused: P_START_BUDGET
smoke-forbid: [neograph] lint
"""

import neograph_engine as ng
from _program import admit, budget, make_model_host, model_url
from _scripted import ScriptedModel, say

PROGRAM = r"""
export function define() {
  return ng.graph("draft_then_refine")
    .channel("messages", {reducer: "append", initial: [{role: "user", content: "Name a river."}]})
    .node("draft", {type: "llm_call"})
    .node("refine", {type: "llm_call"})
    .entry("draft").edge("draft", "refine").exit("refine");
}
"""

# 500 tokens in and 100 out per reply. The same script serves both calls.
script = [say("The Seine.").with_usage(500, 100), say("The Seine, in Paris.").with_usage(500, 100)]

with ScriptedModel(script) as model:
    host = make_model_host(model_url(model))
    run_budget = budget(model_tokens=1_000, monetary_microunits=1_000_000)
    version = admit(host, PROGRAM, run_budget, "rivers.js")

    result = host.run(version, {}, run_budget)
    print("status:", result.status.name)
    print("failure:", result.failure.code)
    print(f"model tokens: {result.usage.model_tokens} used of {run_budget.model_tokens}")
    print("model calls:", model.call_count)
    print("trace:", result.execution_trace)

    # The same version under another budget is refused before anything runs.
    other = budget(model_tokens=5_000, monetary_microunits=1_000_000)
    try:
        host.run(version, {}, other)
    except ng.ProgramDiagnosticError as error:
        refused = error.diagnostics[0]
        print("refused:", refused["code"])
        print(" ", refused["message"][:100])
    print("model calls after the refused run:", model.call_count)
    assert model.call_count == 2

assert result.status == ng.ProgramTerminalStatus.BudgetExhausted
assert result.failure.code == "NG.BudgetExhausted"
assert result.usage.model_tokens == 1_200 and result.remaining_budget.model_tokens == 0
assert refused["code"] == "P_START_BUDGET"
