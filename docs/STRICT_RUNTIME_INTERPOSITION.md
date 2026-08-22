# Strict Runtime Interposition

**Languages:** [English](STRICT_RUNTIME_INTERPOSITION.md) | [한국어](STRICT_RUNTIME_INTERPOSITION.ko.md) | [日本語](STRICT_RUNTIME_INTERPOSITION.ja.md) | [简体中文](STRICT_RUNTIME_INTERPOSITION.zh-CN.md)

NeoGraph's strict runtime path moves mandatory context, lifecycle Hooks, and
provider dispatch evidence out of model discretion. It is additive: legacy
direct provider calls still exist for trusted embedding, while a
`StrictRuntimeProfile` assembles the dependencies required for the strict path.

## Guarantee boundary

```text
durable RAW history + admitted artifacts + required Skills/constraints
  -> immutable ContextEpoch
  -> RuntimeTurnAssembler
  -> ContextAssemblyReceipt
  -> mandatory BeforeProviderRequest Hooks
  -> durable ProviderDispatchReceipt
  -> provider
  -> ProviderDispatchOutcomeReceipt
  -> mandatory AfterProviderResponse Hooks
```

The guarantee covers exact context construction, mandatory-artifact presence,
request identity, dispatch admission, and known/reconciliation-required provider
outcomes. It does not claim that an LLM attended to or obeyed every token.

Host-authored custom native nodes remain trusted code. Giving such a node a raw
`Provider` deliberately leaves the strict profile; generated topology receives
only registered nodes and cannot manufacture that authority.

## Strict profile

`StrictRuntimeProfileConfig` requires:

- a provider;
- a `DurableContextStore`;
- a `DurableProviderDispatchReceiptStore` with terminal outcome support;
- a `HookRuntime`;
- a content-addressed provider binding identity;
- a non-zero input token ceiling; and
- optional exact required context and Skill artifact identities.

Only a `RuntimeGuaranteeProfile::Strict` epoch may be activated. Attaching the
profile to `GraphEngine` installs both provider interposition and lifecycle
Hooks on built-in consumers.

## Provider outcome lifecycle

The provider boundary now records two separate immutable values:

1. `ProviderDispatchReceipt` is written before dispatch.
2. `ProviderDispatchOutcomeReceipt` records `Succeeded`, `Failed`, or
   `ReconciliationRequired` after the attempt.

A successful result binds a digest of the normalized completion. An exception
after dispatch cannot prove whether a remote provider acted, so the controller
records `ReconciliationRequired` rather than retrying. SQLite schema v3 stores
outcomes separately and validates that each outcome still binds the exact
admitted dispatch receipt after restart.

## Mandatory Hooks over native, stdio, or HTTP

`MandatoryHookRunner` accepts either the existing native adapter or a
transport-neutral `HookExecutionBackend`. `RpcHookExecutionAdapter` binds
`HookRpcExecutor` to that backend. The same fixed `hooks/invoke` JSON-RPC method
can use `StdioJsonRpcTransport` or `HttpJsonRpcTransport`.

RPC Hook artifacts are evidence, never authority. A
`ContextStoreHookArtifactPublisher` accepts only artifacts whose:

- kind is `HookOutput`;
- `source_digest` equals the exact Hook invocation ID; and
- runtime event matches the invocation.

Publication is owner-scoped and idempotent. If an external effect succeeded but
its artifact cannot be published, the Hook settles to
`ReconciliationRequired`; it is not reported as clean success.

## Required context and transformation

`RuntimeContextRequirements` separates all required artifact IDs from the
subset that must be `RequiredSkill` artifacts. `HardConstraint` is a dedicated
required artifact kind. Every required artifact must be selected by the active
epoch, must retain `required=true`, and contributes to the mandatory token
count.

`ContextTransformReceipt` is deliberately conservative in v1. A transformer
may replace or compress optional evidence, but every required input artifact ID
must appear byte-identically in the output set. A paraphrase is not accepted as
a proof of constraint preservation.

## Runtime developer instructions

`RuntimeDeveloperInstruction` is immutable developer input, not authority.
`RuntimeInstructionController::submit_and_plan` performs this order:

```text
append Developer-trust history record
  -> load the exact active Program lineage/generation
  -> call the host planner
  -> validate decision against the current lineage head
  -> require an exact already-admitted target for transition decisions
  -> persist the required decision artifact
```

The closed decisions are:

- `SatisfiedInPlace`;
- `Rejected`;
- `ReplaceAtHandoff`; and
- `MigrateGraph`.

Applying a transition rechecks the lineage head immediately before delegating
to the existing `ProgramRuntime::replace` or `migrate_graph` path. A stale
decision cannot become authority.

## Bounded Program synthesis

`ProgramSynthesisGateway` provides the host-owned generated successor path:

```text
immutable ProgramSynthesisProposal
  -> durable host reservation receipt
  -> bounded QuickJS compilation
  -> proposal capability/effect closure check
  -> host-owned semantic contract validation
  -> ordinary ProgramCatalog admission
  -> immutable ProgramSynthesisReceipt
```

The reservation must show an exact decrement of one nonrenewable
`max_dynamic_compiles` unit and may not increase any other budget. Reservation
happens before compilation, so rejected source does not receive its compile unit
back. Semantic validation is mandatory and runs after compilation but before the
admission resolver. Its immutable receipt binds the proposal, reservation,
compiled bundle, validator identity, semantic contract identity, verdict, and
evidence digest. A rejected verdict exposes typed evidence and cannot publish a
`ProgramVersion`. The gateway never activates, binds, migrates, or spawns its
result. Those remain separate host decisions through existing Program APIs.

A runtime instruction planner may invoke the gateway and then return the exact
admitted version in a replacement or migration decision. This preserves:

```text
proposal -> reserve -> compile -> semantic validate -> admit -> decide -> migrate/spawn
```

without exposing compiler, Catalog, credentials, or activation authority to
generated JavaScript.
