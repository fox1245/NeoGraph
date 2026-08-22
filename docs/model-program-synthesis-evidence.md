# Model-generated QuickJS Program synthesis evidence

**Languages:** [English](model-program-synthesis-evidence.md) | [한국어](model-program-synthesis-evidence.ko.md) | [日本語](model-program-synthesis-evidence.ja.md) | [简体中文](model-program-synthesis-evidence.zh-CN.md)

- Status: bounded PoC verified
- Observed: 2026-08-21
- Model: `deepseek/deepseek-v4-flash-0731` through OpenRouter
- Provider response: `gen-1787288110-o3PCpNZgsnE8eyQF1TzM`

## Verified chain

One model response produced this QuickJS Program source:

```javascript
export function define() {
    const graph = ng.graph("model-synthesized");
    graph.channel("value", { reducer: "probe.overwrite", initial: 0 });
    graph.channel("path", { reducer: "probe.overwrite", initial: "" });
    const pairs = [["seed", "probe.seed"], ["double", "probe.double"], ["finish", "probe.finish"]];
    for (const [name, type] of pairs) {
        graph.node(name, {type});
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

The checked-in fixture verifies the same host pipeline deterministically. Its
durable identities are:

| Evidence | Identity |
|---|---|
| Program source | `sha256:4e994637bfa31884f3a0090ffee7b0135f591656ee6217448d435d4a2b6384a3` |
| Proposal | `sha256:c51cd4737dc19939ee25a08799e9308a4dbf8943bffbcbd225e0cc9d7e361347` |
| Reservation | `sha256:531ed6ed5fd713f8d5eda5d3d26df1bedf36d59d44d35b2af8cbfa53ff1ec628` |
| Bundle | `sha256:3f21798666ddc5ad76c73ba9706e93db032064b76e68a4965f0bc49b2c89a375` |
| ProgramVersion | `sha256:71b0ea551fc37ccd92b89c740b58824752397046d5fef78e13a4c21adca84728` |
| Synthesis receipt | `sha256:350775f4c0bc9bb40937cec5c91fd5887f5678c7af349aa177406cec9c5e2f99` |

Catalog lookup found the exact admitted version. Program execution completed
with trace `seed -> double -> finish`; every node ran exactly once. The final
channels were `value = 12` and `path = "model-generated"`.

## Runtime topology replacement

The synthesis gateway now compiles a successor under host-owned
`ProgramBudgetBounds`. The maximum for every resource is the reservation's
`remaining_after_reservation`; it is therefore an authority ceiling derived
from the already-debited lineage, not from model output. The lower bound is
only the JavaScript runtime structural floor: one wall-time unit, one worker,
one Program operation, and one Core step. Consumable and child grants retain a
zero floor. Inverted host bounds are rejected before source evaluation.

This lets an exact replacement carry its smaller, wall-time-debited lineage
remainder without widening any nonrenewable budget. The ordinary exact-budget
compiler overload remains available for fixed standalone invocations.

A different source Program reached a durable top-level `ng.checkpoint`. The
host consumed that handoff and called `ProgramRuntime::replace` with the model
topology as the successor. The target was the synthesis gateway's exact
ProgramVersion above; no second bundle was compiled or admitted. The fixture
transition produced:

| Evidence | Value |
|---|---|
| Source ProgramVersion | `sha256:24d9f2b64ee55e212039d31fe8d9b59a619b0b285346730b06d712f10716c09f` |
| Source run | `run-e153d50d90cc8d222c5f363c99399569` |
| Target run | `sha256:590048ab76d42119e184f89eb88701cd8df32a84bc244286f415c0b53086a089` |
| Target ProgramVersion | `sha256:71b0ea551fc37ccd92b89c740b58824752397046d5fef78e13a4c21adca84728` |
| Replacement receipt | `sha256:79a334ee185bf2ac0115d7d79038c2adff7b4bb854f294c7a5585924d8376fc3` |
| Active generation | `2` |
| Target status | `completed` |

The stale source node ran zero times. The successor trace was
`seed -> double -> finish`, every successor node ran once, and the final output
again contained `value = 12` and `path = "model-generated"`.

The latest live DeepSeek run reported `replacement_uses_synthesis_version =
true`, target status `completed`, active generation `2`, and the same
zero-stale-node result.

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
different Program generation at a durable runtime checkpoint. It further
proves that the synthesis gateway's own admitted version can be that successor
after its dynamic-compile debit; the runtime neither reuses the proposal as
authority nor recompiles the source on the replacement path.

This Program-level replacement must not be confused with arbitrary
GraphEngine state/frontier migration. A migration plan from the source Core
topology to the model topology was correctly classified `blocked` because its
materialization and runtime contract differed.

NeoGraph now also has a deliberately narrow P1 GraphEngine path:
`GraphSemanticMigrationAdapter`. A host must prepare this immutable adapter
from the exact admitted source and target artifacts. It admits only
declaration-only (no runtime JavaScript control), single-root `call_core`
Programs with identical checkpointed channels/reducers,
node names, edges, routing, barriers, retry/interrupt shape, capability binding,
authority, and input/output contracts. It can therefore carry an identity-mapped
frontier and channel snapshot into a successor with a different sealed Core
definition and compiled-plan identity. The adapter is stored in the migration
receipt and revalidated during recovery.

QuickJS control, node/frontier renames, channel or reducer translation, changed
barrier membership, pending effects, children, and arbitrary topology edits are
still fail-closed. Those cases continue to require an explicit handoff/restart
until a later mapping class proves every affected state dimension. Automatic
child binding/spawn, crash recovery across every synthesis boundary, and an
in-Program `ng.proposeProgram` command surface also remain separate
qualification gates.

## Model-generated P1 GraphEngine migration

The P1 adapter was also exercised end-to-end with a live
`deepseek/deepseek-v4-flash-0731` OpenRouter response
`gen-1787291529-fCOHp8pry7EwHHHu1MUH`. The model generated a declaration-only
QuickJS `define()` source (SHA-256
`346329bf39790cc5557a9961a7faa5da0b35168f84257b12d6166565d594df08d`) whose
topology preserved the source graph's `work -> followup` frontier shape while
introducing a distinct target Core definition through `migration_epoch: 2`.

```text
model QuickJS define()
  -> ProgramSynthesisProposal
  -> dynamic-compile reservation
  -> ProgramCompiler + ProgramCatalog admission
  -> GraphSemanticMigrationAdapter preparation
  -> durable GraphEngine generation-2 migration
  -> recovery-proof validation
```

The ordinary migration plan remained `blocked`, as required for a changed
bundle/materialization. The host-created adapter then admitted the narrow
identity projection. The target completed at generation `2`; `work` ran once
on the source generation and `followup` ran once on the successor. The exact
adapter identity was persisted in the migration receipt.

Reproduce with the `program_model_synthesis_probe` target and:

```powershell
bun run scripts/run_model_program_synthesis_probe.ts `
  --probe build-agent-vs/tests/Release/program_model_synthesis_probe.exe `
  --model deepseek/deepseek-v4-flash-0731
```

For the GraphEngine P1 path, use `program_model_semantic_migration_probe` and:

```powershell
bun run scripts/run_model_semantic_migration_probe.ts `
  --probe build-agent-vs/tests/Release/program_model_semantic_migration_probe.exe `
  --model deepseek/deepseek-v4-flash-0731
```
