# Model-generated QuickJS Program synthesis evidence

- Status: bounded PoC verified
- Observed: 2026-08-21
- Model: `deepseek/deepseek-v4-flash-0731` through OpenRouter
- Provider response: `gen-1787284980-ylgst35zIZI7Tvpm1LsT`
- Generated source SHA-256: `1f934e4c5a3394435e2aacc520edebd215eb89b841685858a959724809626c28`

## Verified chain

One model response produced this QuickJS Program source:

```javascript
export function define() {
    const graph = ng.graph("model-synthesized");
    graph.channel("value", { reducer: "probe.overwrite", initial: 0 });
    graph.channel("path", { reducer: "probe.overwrite", initial: "" });
    const nodePairs = [["seed", "probe.seed"], ["double", "probe.double"], ["finish", "probe.finish"]];
    for (const [name, type] of nodePairs) {
        graph.node(name, { type });
    }
    graph.entry("seed");
    graph.edge("seed", "double");
    graph.edge("double", "finish");
    graph.exit("finish");
    return graph;
}

export function* main(input) {
    return yield ng.callCore("model-synthesized", input, "model-generated:1");
}
```

The host did not execute this proposal directly. The probe enforced:

```text
model source
  -> immutable ProgramSynthesisProposal
  -> nonrenewable dynamic-compile reservation
  -> bounded QuickJS ProgramCompiler
  -> independent ProgramCatalog admission
  -> immutable ProgramVersion publication
  -> ProgramRuntime execution
```

The durable identities emitted by the successful run were:

| Evidence | Identity |
|---|---|
| Program source | `sha256:e1492d0a699243e352cc80e854dd3eadf8ff2d8bb96447fe940755fa157989d9` |
| Proposal | `sha256:1b60aa153c48038ad975e309182108955b432ef40f70135d81e9c580c5e14071` |
| Reservation | `sha256:2be33948bfe1667f585a759cd3c120126b4d29b8552ec7851768dca2a0face38` |
| Bundle | `sha256:dd8ac3a6db392ec80371cc22c9ab9f0fe4f197a37286eb8e03a375636b09adb5` |
| ProgramVersion | `sha256:84fd77d5a623ff0d1d4f2fe4d2fe4ee90a96b7fa7cec9c8d483bbf83954016dc` |
| Synthesis receipt | `sha256:91d0ef8205c08ca33668185ef953529909fb120e7cd9a06a06506259fa6e9bde` |

Catalog lookup found the exact admitted version. Program execution completed
with trace `seed -> double -> finish`; every node ran exactly once. The final
channels were `value = 12` and `path = "model-generated"`.

## Negative evidence and prompt contract

Earlier model outputs were rejected before execution:

- `P_JS_DEFINE_MISSING`: no synchronous exported `define()`;
- `P_JS_DEFINE_VALUE`: `define()` returned plain graph-shaped data rather than
  the opaque `ng.graph()` builder; and
- `P_JS_GRAPH_ARGUMENT`: channel/node builder arguments did not match the
  reviewed DSL schema.

The successful prompt therefore specified the exact trusted authoring surface:
`ng.graph`, `graph.channel` with `initial`, `graph.node(name, {type})`, entry,
edge, exit, and a sealed `ng.callCore` command. Invalid proposals produced no
ProgramVersion and ran no node.

## Scope boundary

This proves an external model can synthesize QuickJS topology source that is
then reserved, compiled, admitted, published, and executed by NeoGraph. It does
not prove live migration of an already-running Program, automatic child binding
and spawn, crash recovery across every synthesis boundary, or an in-Program
`ng.proposeProgram` command surface. Those remain separate qualification gates.

Reproduce with the `program_model_synthesis_probe` target and:

```powershell
bun run scripts/run_model_program_synthesis_probe.ts `
  --probe build-agent-vs/tests/Release/program_model_synthesis_probe.exe `
  --model deepseek/deepseek-v4-flash-0731
```
