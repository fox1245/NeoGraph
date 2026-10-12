"""37 - A model inside a Program: the host binds the provider, and the usage lands in the result.

The built-in `llm_call` node asks a model. A Program cannot name a provider or hold a key:
its registry declares that `llm_call` requires a provider called "openrouter", and the host
supplies the object when it is built, with `node_context=` and a non-secret `binding_label`.
Here the provider is the scripted model. The reply goes into the `messages` channel, the
reported tokens are charged to the run (`usage.model_tokens`, `remaining_budget`), and each
call leaves a monetary record priced from the catalog. A host without the binding refuses to
admit the Program at all (P_ADMIT_PROVIDER).

Run:
    python 37_program_model_node.py

smoke-expect: reply: The Seine.
smoke-expect: usage.model_tokens: 42
smoke-expect: remaining model tokens: 958
smoke-expect: money: estimated
smoke-expect: unbound host refuses: P_ADMIT_PROVIDER
smoke-forbid: [neograph] lint
"""

import neograph_engine as ng
from _program import OWNER, admit, budget, ceiling, model_context, model_url, registry
from _scripted import ScriptedModel, say

PROGRAM = r"""
export function define() {
  return ng.graph("chat")
    .channel("messages", {reducer: "append", initial: []})
    .node("ask", {type: "llm_call"})
    .entry("ask").exit("ask");
}
export function* main(input) {
  return yield ng.callCore("chat", input, "chat:main");
}
"""

with ScriptedModel([say("The Seine.").with_usage(30, 12)]) as model:      # 30 in, 12 out
    # The registry says what is needed; the host says what answers.
    snapshot = registry(model_provider="openrouter")
    limit = ceiling(model_tokens=10_000_000, monetary_microunits=10_000_000)
    host = ng.LocalProgramHost(snapshot, OWNER, limit, node_context=model_context(model_url(model)),
                               binding_label="example-scripted")

    run_budget = budget(model_tokens=1_000, monetary_microunits=1_000_000)
    version = admit(host, PROGRAM, run_budget, "chat.js")
    question = {"messages": [{"role": "user", "content": "Name a river."}]}
    result = host.run(version, question, run_budget)

    reply = result.output["messages"][-1]
    print("reply:", reply["content"])
    print("usage.model_tokens:", result.usage.model_tokens)
    print("remaining model tokens:", result.remaining_budget.model_tokens)
    record = result.monetary_records[0]
    print(f"money: {record.evidence}, {record.microunits} microunits for {record.model}")

    # Without node_context the same registry cannot be admitted: no provider, no Program.
    unbound = ng.LocalProgramHost(snapshot, OWNER, limit)
    try:
        admit(unbound, PROGRAM, run_budget, "chat.js")
    except ng.ProgramAdmissionError as error:
        refusal = error.diagnostics[0]["code"]
    print("unbound host refuses:", refusal)

assert result.status == ng.ProgramTerminalStatus.Completed and reply["role"] == "assistant"
assert result.usage.model_tokens == 42 and result.remaining_budget.model_tokens == 958
assert record.evidence == "estimated" and record.microunits > 0 and model.call_count == 1
assert refusal == "P_ADMIT_PROVIDER"
