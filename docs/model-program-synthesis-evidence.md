# Model-generated QuickJS Program synthesis evidence

- Status: bounded PoC verified
- Observed: 2026-08-21
- Model: `deepseek/deepseek-v4-flash-0731` through OpenRouter
- Provider response: `gen-1787286237-aGf7D0XMoUz7JYlYqQty`
- Generated source SHA-256: `718a50ef0e88fd8581547230bea861e44fa816cc99c4dd470951d96356be5ecf`

## Verified chain

One model response produced this QuickJS Program source:

```javascript
export function define() {
    const graph = ng.graph("model-synthesized");
    graph.channel("value", { reducer: "probe.overwrite", initial: 0 });
    graph.channel("path", { reducer: "probe.overwrite", initial: "" });
    const nodes = [["seed", "probe.seed"], ["double", "probe.double"], ["finish", "probe.finish"]];
    for (const [name, type] of nodes) {
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
| Program source | `sha256:3d4e406dff870e594d2f14fd3becf471a7a19b03a1cbafd8a8df894b83442b22` |
| Proposal | `sha256:d2c8c6df03cadd80f97813b261c9fbb631a5d4622ab9bb178ec2733b3df69027` |
| Reservation | `sha256:75e7442130cfe1bd888e89acbb603e78bcf7e568f78205ac331b396ee70f6a8f` |
| Bundle | `sha256:b8eb660f571186223896495122fe547388d6040ae5325e3367f1142ecbedf7f8` |
| ProgramVersion | `sha256:9974fe3f07a457a8734aaefae5b032b546740e9568913b619f79b1ed2897b7a1` |
| Synthesis receipt | `sha256:46d6849ff8964cbefe5cae922df43895e3a30cc19cebfa22e4c016a714d62b34` |

Catalog lookup found the exact admitted version. Program execution completed
with trace `seed -> double -> finish`; every node ran exactly once. The final
channels were `value = 12` and `path = "model-generated"`.

## Runtime topology replacement

The same model source was also compiled with replacement-compatible budget
bounds and admitted as ProgramVersion
`sha256:5823b5541f071b438e86993001080778e94bdb10ee26f82437db3fd6de16aedd`.
Its source hash exactly matched the synthesis-gateway bundle.

A different source Program reached a durable top-level `ng.checkpoint`. The
host consumed that handoff and called `ProgramRuntime::replace` with the model
topology as the successor. The transition produced:

| Evidence | Value |
|---|---|
| Source ProgramVersion | `sha256:24d9f2b64ee55e212039d31fe8d9b59a619b0b285346730b06d712f10716c09f` |
| Source run | `run-be2ac2878d19a766b52b258487cf8d53` |
| Target run | `sha256:5d79d6363382a97c7db9575d2570fc459176f9cb469a98a3c1cdc062ce59836a` |
| Replacement receipt | `sha256:cc1531a4ae1dfee60638c85d37171c511fa70242ec09b266c611ca994229cd4b` |
| Active generation | `2` |
| Target status | `completed` |

The stale source node ran zero times. The successor trace was
`seed -> double -> finish`, every successor node ran once, and the final output
again contained `value = 12` and `path = "model-generated"`.

## Negative evidence and prompt contract

Earlier model outputs were rejected before execution:

- `P_JS_DEFINE_MISSING`: no synchronous exported `define()`;
- `P_JS_DEFINE_VALUE`: `define()` returned plain graph-shaped data rather than
  the opaque `ng.graph()` builder; and
- `P_JS_GRAPH_ARGUMENT`: channel/node builder arguments did not match the
  reviewed DSL schema; and
- `P_JS_EVALUATION`: an unquoted reducer identifier referenced ambient state
  that does not exist in the bounded QuickJS context.

The successful prompt therefore specified the exact trusted authoring surface:
`ng.graph`, `graph.channel` with `initial`, `graph.node(name, {type})`, entry,
edge, exit, and a sealed `ng.callCore` command. Invalid proposals produced no
ProgramVersion and ran no node.

## Scope boundary

This proves an external model can synthesize QuickJS topology source that is
then reserved, compiled, admitted, published, executed, and selected as a
different Program generation at a durable runtime checkpoint.

The exact synthesis-gateway ProgramVersion could not be used directly as the
replacement target because its requested budget is compiled as
minimum=maximum, while replacement must debit elapsed wall time and transfer the
smaller exact lineage remainder. The probe therefore admitted a second bundle
from the identical model source using the compiler's replacement-compatible
budget range. Closing this budget-contract mismatch remains integration work.

This Program-level replacement must not be confused with arbitrary
GraphEngine state/frontier migration. A migration plan from the source Core
topology to the model topology was correctly classified `blocked` because its
materialization and runtime contract differed. Automatic child binding/spawn,
crash recovery across every synthesis boundary, and an in-Program
`ng.proposeProgram` command surface also remain separate qualification gates.

Reproduce with the `program_model_synthesis_probe` target and:

```powershell
bun run scripts/run_model_program_synthesis_probe.ts `
  --probe build-agent-vs/tests/Release/program_model_synthesis_probe.exe `
  --model deepseek/deepseek-v4-flash-0731
```
