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
Hooks on built-in consumers. It does not step the context between provider
calls; see [Host-stepped contract](#host-stepped-contract).

## Host-stepped contract

The strict path builds every provider request from the active epoch: its fixed
RAW range and its selected artifacts. It does not read the conversation that a
graph or an agent holds. Besides the epoch, only the host-instruction and trusted
supplemental slots that built-in nodes fill reach the prompt. NeoGraph therefore
cannot see a model reply or a tool result until the host has recorded it. The
contract is:

**The host appends RAW records and activates a new epoch for every provider
call.**

Between two calls the host appends the previous turn (the model's reply, then
the tool results) to the `ContextStore`, builds an epoch whose RAW range covers
it, and activates that epoch with `StrictRuntimeProfile::activate`. The first
epoch is built the same way from the user's messages. The code below is one
step. `head` is `store.history_head(feed)`, and `epoch_sequence` and
`previous_epoch_id` come from the last epoch.

```cpp
for (auto& [message, trust] : turn) {  // the reply (ModelOutput), then the tool results (ToolOutput)
    RuntimeHistoryRecordData record;
    record.feed_id = feed.feed_id;
    record.sequence = ++head.sequence;
    record.message_id = "message-" + std::to_string(record.sequence);
    record.trust = trust;
    record.message = std::move(message);
    record.predecessor_id = head.record_id;
    const auto created = RuntimeHistoryRecord::create(std::move(record));
    if (store.append_history(feed, created, head.record_id) != ContextStoreAppendResult::Appended)
        throw std::runtime_error("the context feed moved");
    head.record_id = created.id();
}
ContextEpochData next;
next.run_id = run_id;
next.sequence = ++epoch_sequence;
next.predecessor_id = previous_epoch_id;
next.feed_id = feed.feed_id;
next.raw_from_sequence = 1;
next.raw_through_sequence = head.sequence;
next.raw_window_digest = store.snapshot_history(feed, 1, head.sequence).digest;
next.guarantee_profile = RuntimeGuaranteeProfile::Strict;
auto epoch = ContextEpoch::create(std::move(next));
previous_epoch_id = epoch.id();
profile.activate(owner_id, std::move(epoch));
```

NeoGraph has no helper that steps the epoch inside a run yet.

**A loop inside one run.** Attaching the profile to a plain `GraphEngine` does
not step the context. In a tool loop inside one `engine.run()` (LLM call, tool
dispatch, LLM call, as `create_react_graph` builds it), the second LLM call
would run under the same epoch. The graph state holds the tool results, but the
epoch does not. The provider would receive the first turn's context again, the
model would never see the tool results, and it could ask for the same tool until
the step limit or a budget stops the run.

NeoGraph refuses that call instead. When a provider call arrives for an epoch
whose turn the graph has already advanced, it throws `std::logic_error`. A turn
counts as advanced when the messages the caller presents hold assistant or tool
messages that were added since the epoch's first dispatch and that the call does
not also carry as host instructions or trusted supplemental input. The error
names the contract:

```text
Strict provider dispatch is host-stepped: the host appends RAW records and activates a new epoch for every provider call. This call presents assistant or tool messages added since the active epoch's first dispatch, so the graph or agent loop advanced inside one run and the provider would not receive them. Append those messages to the context feed and activate a new epoch before this call.
```

In a graph, the failing node reports it as a `NodeExecutionError`. A logic
error is not retried, so `attempts()` is 1. `llm::Agent` with a controller set
through `set_runtime_interposition` fails the same way on its second provider
call.

The check does not refuse:

- A retry of the same turn. A node retry, or a repeated call after a provider
  failure, presents the same messages again.
- A conversation that already held assistant or tool messages at the epoch's
  first dispatch. Those messages are the baseline.
- Growth by user messages alone. The check looks for assistant and tool messages.
- A built-in node that passes its growing conversation as trusted supplemental
  input, such as the plan executor. Its replies and tool results reach the
  provider, and the assembly receipt binds them.

Activating an epoch, or `clear()`, forgets what was presented. The controller
keeps the first presentation of each call site (the node name) for a bounded
number of distinct presentations per epoch. A presentation past that limit is not
tracked, so the check is best effort there.

Serialize the runs of one conversation yourself; the profile holds one active
epoch and does not look at `thread_id`. See
[Runs of one conversation](concurrency.md#runs-of-one-conversation).

## Choosing how to manage context

NeoGraph has four ways to manage the context of a model call. They differ in
what builds the prompt and in what is guaranteed.

| Path | The prompt comes from | What it guarantees |
| --- | --- | --- |
| `messages` channel | The `messages` channel of the graph state, as the nodes leave it. `LLMCallNode` sends it and appends the reply, `ToolDispatchNode` appends the tool results, so a tool loop inside one run works. | The request is built from the graph state, and a checkpoint store can save that state. No receipt per call, no required artifact, no context ceiling. |
| `history::compact_history` | Not a path of its own. It is a helper that returns a shorter message list: when `estimate_tokens` is over the limit, it replaces an old text-only prefix with a model-written summary and keeps the recent tail. You use the result as the input of another path. The overload that takes a `RuntimeInterpositionController` sends the summary call through it. | A status that says why the history was kept or replaced, and history that holds native replay seals is returned unchanged. The summary is model output: nothing proves that a fact or a constraint survived it. The estimate is a heuristic. No epoch, no receipt. |
| Recorded profile | The active epoch (`RuntimeGuaranteeProfile::Recorded`, the default) through a `RuntimeInterpositionController`: its RAW range from the `ContextStore` plus its artifacts. Host-stepped. | The request is assembled from exactly that epoch and bound to an assembly receipt, and every dispatch has a receipt. The stores need not be durable and the input ceiling may be zero, so how long the evidence lasts, and whether a ceiling applies, is the host's choice. |
| Strict profile | The same as the recorded profile, through `StrictRuntimeProfile`. Host-stepped. | Everything in the recorded row, with durable context and dispatch stores, mandatory Hooks around each call, required artifacts that must be in the epoch, and a non-zero input ceiling that blocks an over-budget turn before dispatch. It does not guarantee that the model attended to or obeyed the context, and it does not step the epoch for you. |

Moving from the `messages` channel to an epoch changes who owns the context: the
graph state stops being the source of the prompt, and the host keeps the RAW
history.

## Context budget

The strict path checks every assembled request against the input ceiling
(`max_input_tokens`) and blocks an over-budget turn before dispatch with
`ContextBudgetBlocked`. The assembly receipt records the estimate it used
(`estimated_input_tokens`) and the share that is required artifact text
(`mandatory_input_tokens`).

The estimate is `RuntimeTurnAssembler::estimate_input_tokens`: the length in
bytes of the encoded request body, divided by three and rounded up. It is a
heuristic, not a proven upper bound. It is not derived from any provider's
tokenizer, so it can be below or above the number a provider counts. It counts
the encoded request, not only the visible text: tool schemas, field names and
provider framing add bytes too. Treat `max_input_tokens` as a guard against
oversized requests, not as a promise about the count a provider reports or bills.
`mandatory_input_tokens` uses the same rate on the text of the required
artifacts only.

The dispatch token reservation is a separate resource contract. When a run
has a model-token budget, each dispatch reserves the model's input limit plus the
output allowance for every attempt it may make, and does not use this estimate.

## Provider outcome lifecycle

The provider boundary records two separate immutable values:

1. `ProviderDispatchReceipt` is written before dispatch.
2. `ProviderDispatchOutcomeReceipt` records `Succeeded`, `Failed`, or
   `ReconciliationRequired` after the attempt.

A successful result binds a digest of the full SDK outcome observation.
A typed Failure proven not accepted (no request was sent, or a completed HTTP 4xx
answer declined it) records `Failed`; uncertain delivery records
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
Also set `RuntimeConfig::core_provider_calls_journaled = true`. A fork replays
provider calls from this journal, and the flag keeps a fork on this host from
reporting `P_FORK_PROVIDER_CALLS_NOT_JOURNALED` in `ProgramHandle::notices()`.
A fork that finds the source's call at the same position but can neither adopt it
nor show that the call differs is refused, unless
`RuntimeConfig::fork_may_resend_provider_calls` is true (see the reference).

The journal keys each call by owner, immutable Program version, run, operation,
Core thread/task/node and built-in call ordinal, **not** Program attempt. It
records the call's request content digest and the descriptor policy identity beside
the key and matches a call by its content (#543), so the same request under another
SchemaProvider descriptor policy identity is the same call. It commits the marker
with SQLite FULL synchronization before transport. The marker means transport *may* have occurred, not that the provider
received it or that the effect happened exactly once. Full immutable SDK
Completion/Failure outcomes are stored with encoding version 2 for exact-bound
replay. A Failure proven not accepted (not sent, or declined by a completed HTTP
4xx answer) records `Failed`; uncertain delivery, an
exception, or a crash before settlement requires reconciliation and never
silently re-dispatches. Use
`inspect(owner, logical_call_id(context, core_identity))` to check status, then
`reconcile_success` only with independently verified provider-side evidence and
a full Completion outcome. Replayed streaming calls return the captured outcome
without synthesizing stream events.

A larger output cap is a new semantic call, not a transport retry or replay of the same journal slot. Native replay configuration excludes that cap; the prepared-request digest and conservative resource claim still include it. Give each admitted call a distinct deterministic ordinal, retain every attempt's outcome/accounting and the original deadline, and obtain admission from the same resource bank. A changed content digest in an existing slot rejects; neither native history nor a cursor renews credit or authorizes another send after uncertain delivery, observer or settlement failure.

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
can use `StdioJsonRpcTransport` or `HttpJsonRpcTransport`. Both run over an
`MCPClient` session, so the endpoint is an MCP server that also serves
`hooks/invoke`. The method is a NeoGraph extension, not part of MCP; an MCP
server without it answers `-32601`.

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

## Artifact placement

An artifact's `placement` decides where its text goes among the RAW messages.
The assembler builds the prompt in this order. Within one placement, artifacts
are ordered by higher `priority` first, then by `producer_id` and `id`.

```text
host instructions
BeforeHistory artifacts
RAW messages before the latest user message
BeforeLatestUser artifacts
latest user message
AfterLatestUser artifacts
RAW messages after the latest user message
AfterHistory artifacts
trusted supplemental input
```

Put artifacts that stay the same from turn to turn at `BeforeHistory`. The
default placement is `BeforeLatestUser`, so an artifact created without a
placement moves on every turn:

- `BeforeLatestUser` and `AfterLatestUser` are anchored to the latest user
  message. When a new user message arrives, the artifact moves with it, and the
  prompt differs from the previous one from the artifact's old position onward.
  These two placements need a user message in the RAW window; assembly fails
  without one.
- `AfterHistory` is always last. The history in front of it grows, but it does
  not change the part of the prompt in front of it.
- `BeforeHistory` comes right after the host instructions and does not depend on
  the history. Its text and position stay the same while the artifact set and
  priorities stay the same.

A change of the artifact set, or a RAW window that drops its oldest records,
changes the prompt from that point on. A provider that caches the start of a
prompt can reuse only the part that is identical to the previous request from
its first byte. NeoGraph has not measured the effect of placement on cache hits.

`RequiredSkill` and `HardConstraint` artifacts render as System messages; every
other kind renders as a User message. What a System message does depends on the
provider family:

- On `anthropic.messages`, `google.generate` and `google.interactions`, the
  request builder moves every System message into the top-level system field
  (separated by a blank line, in assembled order), whatever its placement. For a
  Skill or a constraint, the placement then decides only its order among the
  system texts, not its position in the conversation.
- On `openai.chat` and `openai.responses`, a System message stays where its
  placement puts it.

Keep the text of System-role artifacts unchanged between turns when you want a
stable request start. This is the legacy layout, which an epoch uses unless it
asks for another one (see "The stable-first layout").

## Stability class, order and prefix claim

Whether a provider's prompt cache keeps the start of a request depends on which artifacts stay the same between calls. The data model records that, and the stable-first layout (next section) uses it.

- An artifact can declare a `StabilityClass`: `Stable` (the same bytes for the life of the run), `Slow` (changes only at a compaction or another declared prefix break) or `Volatile` (may change on every call). An artifact that declares none gets a class when it is read: `RequiredSkill` and `HardConstraint` are `Stable`, a `DerivedContext` whose producer is `COMPACTION_PRODUCER_ID` is `Slow`, and everything else is `Volatile`. Creating a `Volatile` artifact of a kind that renders as a System message is refused with `ContextStabilityError`.
- An artifact can declare an `AnchorMode`: `Replace` (the default) or `KeepInPlace`. Nothing acts on it yet.
- An epoch can record `ordered_artifact_ids`, the artifacts in a fixed order, and a `prefix_claim`: `Extends` (the request start is the predecessor's and only grows at the end) or `Breaks` with a `break_reason`. Activating an epoch that `Extends` checks the claim against the active epoch, or against the predecessor the host passes to `activate`: the same `raw_from_sequence`, a larger `raw_through_sequence`, the same `Stable` and `Slow` artifacts in the same order, and `KeepInPlace` artifacts that are still selected and inside the RAW window. An epoch that records no order, such as one stored before the field existed, cannot be extended. What `Extends` promises depends on the layout. Under `Legacy` it says that the `Stable` and `Slow` artifacts and their recorded order are unchanged and the window only grew; the legacy assembler orders by placement, priority and producer and does not read the recorded order, so it does not promise the start of the request. Under `StableFirst` the recorded order is the order of the request, the claim is also refused when the epoch's artifacts are not a layout the assembler accepts, and the start of the request up to the end of the `Slow` artifacts is the predecessor's (host instructions and tools aside, which the epoch does not own).
- A receipt written by the assembler records the artifacts in the order of the prompt, their classes (each marked as declared by the artifact or derived, with the version of the derivation rule), the encoder family, a digest of the tools and the epoch's break reason. A body that is not JSON is an error and not a request without tools. A stored receipt is checked strictly where a class was declared, and by deriving the class again only under the rule version in force, so a later change of the rule leaves stored receipts readable.

## The stable-first layout

An epoch chooses how its artifacts are laid out with `assembly_layout`. `Legacy`, the default and the only layout an epoch stored before the field existed can have, is the assembly under "Artifact placement". `StableFirst` is opt-in, needs `ordered_artifact_ids`, and builds the prompt in this order:

```text
host instructions
Stable artifacts, in the recorded order
Slow artifacts, in the recorded order
RAW messages
Volatile artifacts, in the recorded order
trusted supplemental input
```

- **The epoch's list is the order.** `order_context_artifacts` builds it from the artifacts: stability class, then higher `priority`, then `producer_id`, then a key the host gives. No hash of the content takes part, so a sibling whose text changes keeps its place, and two artifacts that agree on all four are refused as ambiguous.
- **Placement follows the class.** `Stable` and `Slow` artifacts are placed at `BeforeHistory`, `Volatile` ones at `AfterHistory`, and the list names the `Stable` ones, then the `Slow` ones, then the `Volatile` ones. Anything else, such as an artifact created with the default `BeforeLatestUser` placement, is refused with a `ContextStabilityError` and not moved.
- **System text stays in front.** A System message may be a host instruction or a `Stable` or `Slow` artifact, and nothing else: a RAW or supplemental System message would be lifted into the system field by `anthropic.messages`, `google.generate` and `google.interactions` and would move the start of the request. The lifted text is ordered `Stable`, then `Slow`.
- **A layout change is a break.** Two consecutive epochs may differ in layout only if the later one declares `Breaks` with the reason `LayoutChange`; `verify_context_epoch_claim` refuses it otherwise, and an `Extends` claim across a layout change as well.
- **Two boundaries, read from the body.** The receipt records where the `Stable` part (host instructions and `Stable` artifacts) ends and where the append-only part (everything in front of the `Volatile` artifacts) ends, and a digest of the request up to the second. A boundary is the first message and part of the encoded body that is not in the prefix, so `{0, 0}` on a family that lifts System text is a prefix of the tools and the system field alone. The positions are those of the body after System text is lifted and tool results are regrouped. The assembler finds them by encoding the request a second and a third time with only the messages of the prefix and a marker message after them, and it refuses the epoch with `ContextStabilityError` if the encoded bodies do not agree. When a `Slow` System text shares the system field with the `Stable` text, the field is one block and no `Stable` boundary is recorded. The digest covers every member of the body except the stream flag and the output cap, with the messages cut at the boundary. A legacy epoch has no boundaries.

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
