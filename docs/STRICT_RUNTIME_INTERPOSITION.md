# Strict Runtime Interposition

**Languages:** [English](STRICT_RUNTIME_INTERPOSITION.md) | [한국어](STRICT_RUNTIME_INTERPOSITION.ko.md) | [日本語](STRICT_RUNTIME_INTERPOSITION.ja.md) | [简体中文](STRICT_RUNTIME_INTERPOSITION.zh-CN.md)

NeoGraph's strict runtime path moves mandatory context, lifecycle Hooks, and
provider dispatch evidence out of model discretion. Direct typed provider calls
remain available for trusted embedding; `StrictRuntimeProfile` assembles the
dependencies required for the strict path.

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

A successful result binds a digest of the full SDK outcome observation.
A typed Failure proven not sent records `Failed`; uncertain delivery records
`ReconciliationRequired`. An exception after dispatch does not establish whether
the remote provider acted, so the controller does not silently retry.
SQLite schema v3 stores terminal receipts separately and checks their exact
admitted dispatch binding after restart. A receipt digest is evidence, not
native continuation custody or an executable stored outcome.

The controller accepts `ProviderRequest` and returns immutable owned
`sp::runtime::Result`, retaining ordered messages/parts and partial failure
evidence. If settlement or receipt persistence fails after a real result,
`ProviderDispatchOutcomePersistenceError` preserves that result and the original
cause; a secondary observer failure remains in `delivery_error()`.
Token charges/reservations remain separate from nullable provider usage reports.

## Program Core provider calls (separate from standalone Strict Runtime)

When Program uses built-in Core LLM nodes, the host can set
`RuntimeConfig::core_provider_call_resolver` and
`require_core_provider_call_broker = true`. For each exact
`ProgramCoreProviderCallContext`, return
`SQLiteProgramProviderCallJournal::bind(context, deployment_identity)` from
`<neograph/program/sqlite_provider_call_broker.h>`; link `neograph::program_sqlite`.
The deployment identity must be a host-owned SHA-256 identity of the actual
provider route, model deployment and authority (including credential version);
the broker does not discover these from a `Provider` object. Rebind the same
durable database on restart/reconnect.

The journal keys each call by owner, immutable Program version, run, operation,
Core thread/task/node and built-in call ordinal, **not** request content or
Program attempt. It commits the marker with SQLite FULL synchronization before
transport. The marker means transport *may* have occurred, not that the provider
received it or that the effect happened exactly once. Full immutable SDK
Completion/Failure outcomes are stored with encoding version 2 for exact-bound
replay. A Failure proven not sent records `Failed`; uncertain delivery, an
exception, or a crash before settlement requires reconciliation and never
silently re-dispatches. Use
`inspect(owner, logical_call_id(context, core_identity))` to check status, then
`reconcile_success` only with independently verified provider-side evidence and
a full Completion outcome. Replayed streaming calls return the captured outcome
without synthesizing stream events.

A larger output cap is a new semantic call, not a transport retry or replay of the same journal slot. Interface 4 excludes that cap only from native replay configuration; the prepared-request digest and conservative resource claim still include it. Give each admitted call a distinct deterministic ordinal, retain every attempt's outcome/accounting and the original deadline, and obtain admission from the same resource bank. A changed digest in an existing slot rejects; neither native history nor a cursor renews credit or authorizes another send after uncertain delivery, observer or settlement failure.

Ordered message parts, raw observations, nullable usage, attempt metadata, and
native continuation remain part of the stored result. Native outcomes require
the host's `sp::NativeArchive` supplied to
`SQLiteProgramProviderCallJournal(database_path, native_archive)`; a portable
JSON projection cannot recreate that authority. Legacy lossy receipts are
rejected rather than upgraded or silently dispatched again. The journal retains
conservative claim/committed token amounts separately from provider reports.
Use a durable filesystem database path: empty paths, `:memory:`, and `file:` URI
paths are rejected.
This broker operates on Core's existing ReAct message state, not an assembled
`ContextEpoch`, and cannot be combined with engine Strict Runtime interposition
on the same built-in call. Host-authored native Provider calls are outside it.

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
