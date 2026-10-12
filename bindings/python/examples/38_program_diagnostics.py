"""38 - Diagnostics a program can act on: the compiler names the mistake and the fix.

A model writes a Program and misspells a node in an edge. Compilation fails with
ProgramCompileError. Each diagnostic is a dict with a code, a JSON pointer, a source span,
a `witness` of facts, and a `help` entry with `suggestions` (edits, with an applicability)
and `candidates` (declared names close to the unknown one). A repair loop can use that
instead of asking the model again: here it applies the suggestion to the source, compiles
once more with `soundness_check=True`, and admits the result. The scripted model is asked
exactly once.

Run:
    python 38_program_diagnostics.py

smoke-expect: first compile: P_CORE_E3 at line 7, unknown 'sumarize'
smoke-expect: candidates: ['summarize']
smoke-expect: suggestion: Replace 'sumarize' with 'summarize'
smoke-expect: second compile: ok, soundness: Soundness check explored every run
smoke-expect: model calls: 1
smoke-forbid: [neograph] lint
"""

import neograph_engine as ng
from _common import ask_text
from _program import admit, budget, make_host, node, registry
from _scripted import ScriptedModel, say


@node("pipeline.step")
def step(name, input, config):
    return [ng.ChannelWrite("trace", [name])]


# What the scripted model writes: one node name is misspelled in the second edge.
DRAFT = r"""
export function define() {
  return ng.graph("pipeline")
    .channel("trace", {reducer: "append", initial: []})
    .node("fetch", {type: "pipeline.step"}).node("summarize", {type: "pipeline.step"})
    .node("publish", {type: "pipeline.step"})
    .entry("fetch").edge("fetch", "sumarize").edge("summarize", "publish").exit("publish");
}
export function* main(input) {
  return yield ng.callCore("pipeline", input, "pipeline:main");
}
"""

with ScriptedModel([say(DRAFT)]) as model:
    text = ask_text(model.provider(), [{"role": "user", "content": "Write the pipeline Program."}])
    compiler = ng.ProgramCompiler(registry(["pipeline.step"]), soundness_check=True)

    for attempt in (1, 2, 3):
        try:
            bundle = compiler.compile(ng.ProgramSource.from_javascript("pipeline.js", text))
            break
        except ng.ProgramCompileError as error:
            problem = next(d for d in error.diagnostics if d["severity"] == "error")
            print(f"first compile: {problem['code']} at line {problem['span']['line_begin']}, "
                  f"unknown {problem['witness']['unknown']!r}")
            print("candidates:", problem["help"]["candidates"])
            suggestion = problem["help"]["suggestions"][0]
            print("suggestion:", suggestion["message"], f"({suggestion['applicability']})")
            # The edit is a JSON-pointer replace on the Core definition. In the source text it
            # is the quoted name that has to change.
            text = text.replace(f'"{problem["witness"]["unknown"]}"',
                                f'"{suggestion["edits"][0]["value"]}"')
    else:
        raise SystemExit("the repair loop did not converge")

    summary = next(d for d in bundle.compile_diagnostics if d["code"] == "P_CORE_SOUNDNESS")
    print("second compile: ok, soundness:", summary["message"])

    host = make_host(["pipeline.step"])
    run_budget = budget()
    result = host.run(admit(host, text, run_budget, "pipeline.js"), {}, run_budget)
    print("run:", result.status.name, result.execution_trace)
    print("model calls:", model.call_count)

assert attempt == 2 and model.call_count == 1
assert result.status == ng.ProgramTerminalStatus.Completed
assert result.execution_trace == ["fetch", "summarize", "publish"]
