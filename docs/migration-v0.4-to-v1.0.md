# Migration Guide: Legacy 8 virtuals → `run(NodeInput)` (v0.4.x → v0.9+)

**Languages:** [English](migration-v0.4-to-v1.0.md) | [한국어](migration-v0.4-to-v1.0.ko.md) | [日本語](migration-v0.4-to-v1.0.ja.md) | [简体中文](migration-v0.4-to-v1.0.zh-CN.md)

NeoGraph v0.4 consolidated the node entry point into a single `run(NodeInput) ->
awaitable<NodeOutput>`. The legacy 8 virtuals (`execute` / `execute_async` /
`execute_stream` / `execute_stream_async` and their `_full` counterparts) were
deprecated in v0.4.x and removed in v0.9.0, the v1 preparation release. This
document outlines the procedure for migrating legacy nodes to the current API.

> In v0.9.0 and later, C++ subclasses that do not implement `run(NodeInput)` will
> fail to compile as abstract classes. Python subclasses must also implement
> `run(self, input)`.

## Why Migrate

Old pattern — `(sync/async) × (writes/full) × (stream/non-stream)` = 8 virtual
cross-product. Overriding any single one causes the other 7 to fall back to the
default chain. Some combinations are safe, but there are runtime pitfalls (e.g.,
sync `execute_full` + async dispatch → nested `run_sync` race), making it
unclear which function the user should override.

New pattern — a single `run(NodeInput) -> awaitable<NodeOutput>`. Override only
one. The sync vs async distinction is the caller's concern (users can use
`co_await` inside coroutines or plain synchronous code freely). Command / Send
are included in `NodeOutput`, so no additional virtuals are needed. Streaming
callbacks arrive via `NodeInput::stream_cb` (a nullable pointer).

## 8 virtuals → New `run()` Mapping

| Legacy Virtual | Migrated Form |
|---|---|
| `execute(state)` | `NodeOutput out; out.writes = {...}; co_return out;` (sync body) |
| `execute_async(state)` | Native async like `co_await provider->invoke_async(std::move(request));` |
| `execute_stream(state, cb)` | `if (in.stream_cb) (*in.stream_cb)(event); co_return NodeOutput{...};` |
| `execute_stream_async(state, cb)` | Above + native async (`co_await ...`) |
| `execute_full(state)` | `NodeOutput out; out.writes=...; out.command=...; co_return out;` |
| `execute_full_async(state)` | Above + native async |
| `execute_full_stream(state, cb)` | `execute_full` + `in.stream_cb` usage |
| `execute_full_stream_async(state, cb)` | Above + native async |

Key: **The 8 variants are expressible as combinations of which `NodeOutput` field
is populated + whether `in.stream_cb` is used + whether `co_await` is used**.
Only one virtual remains.

### Most Common Python Migration

**Old code:**

```python
class CounterNode(ng.GraphNode):
    def execute(self, state):
        current = state.get("count") or 0
        return [ng.ChannelWrite("count", current + 1)]
```

**Current code:**

```python
class CounterNode(ng.GraphNode):
    def run(self, input):
        current = input.state.get("count") or 0
        return [ng.ChannelWrite("count", current + 1)]
```

Python's `run` is a regular `def`, not `async def`. In streaming execution,
`input.stream_cb` is a function that receives events; in regular execution, it
is `None`.

## Case-by-Case Conversion Examples

### Case 1 — Simplest Sync Node

**Old:**
```cpp
class MyNode : public GraphNode {
public:
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        int n = state.get("counter").get<int>();
        return {ChannelWrite{"counter", json(n + 1)}};
    }
    std::string get_name() const override { return "my_node"; }
};
```

**New:**
```cpp
class MyNode : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        int n = in.state.get("counter").get<int>();
        NodeOutput out;
        out.writes.push_back({"counter", json(n + 1)});
        co_return out;
    }
    std::string get_name() const override { return "my_node"; }
};
```

Differences:
- `state` → `in.state`
- Return value wrapped in `NodeOutput` (`writes` field)
- Function is `asio::awaitable<NodeOutput>` and ends with `co_return`

### Case 2 — Async LLM Node (Migrating `execute_async`)

```cpp
#include <neograph/graph/node.h>
#include <neograph/graph/run_context.h>
#include <neograph/provider.h>
#include <neograph/runtime_interposition_consumer.h>

class ChatNode : public neograph::graph::GraphNode,
                 public neograph::RuntimeInterpositionConsumer {
    std::shared_ptr<neograph::Provider> provider_;
    std::string model_;
public:
    ChatNode(std::shared_ptr<neograph::Provider> provider, std::string model)
        : provider_(std::move(provider)), model_(std::move(model)) {}
    asio::awaitable<neograph::graph::NodeOutput>
    run(neograph::graph::NodeInput in) override {
        auto request = neograph::make_provider_request(
            *provider_, model_, in.state.get_provider_messages());
        request.cancel_token = in.ctx.cancel_token;
        request.options.deadline = in.ctx.deadline;
        auto result = co_await neograph::graph::observe_provider_result(
            in.ctx, invoke_provider(provider_, std::move(request), {}, {},
                neograph::graph::provider_call_broker(in.ctx),
                neograph::graph::make_provider_call_identity(in.ctx, get_name())));
        neograph::graph::record_usage(in.ctx, result);
        neograph::outcome_or_throw(result);
        neograph::graph::NodeOutput out;
        out.writes.push_back(neograph::graph::provider_messages_write(result));
        co_return out;
    }
    std::string get_name() const override { return "chat"; }
};
```

### Case 3 — Streaming Node (Migrating `execute_stream`)

The public contract is owned typed preparation and dispatch, not paired virtual completion methods. `ProviderRequest.payload` is the SDK variant of Chat, Messages, Responses, Gemini or Interactions requests. `ProviderMode::Collect` / `Stream` selects transport independently of an observer. `on_event` receives borrowed typed `sp::Event` views; copy only data needed after the callback. No raw JSON overrides or native-state import through portable projections are admitted.

```cpp
#include <neograph/graph/node.h>
#include <neograph/graph/run_context.h>
#include <neograph/provider.h>
#include <neograph/runtime_interposition_consumer.h>

class StreamingChatNode : public neograph::graph::GraphNode,
                 public neograph::RuntimeInterpositionConsumer {
    std::shared_ptr<neograph::Provider> provider_;
    std::string model_;
public:
    StreamingChatNode(std::shared_ptr<neograph::Provider> provider, std::string model)
        : provider_(std::move(provider)), model_(std::move(model)) {}
    asio::awaitable<neograph::graph::NodeOutput>
    run(neograph::graph::NodeInput in) override {
        auto request = neograph::make_provider_request(
            *provider_, model_, in.state.get_provider_messages(), {}, {},
            neograph::ProviderMode::Stream);
        request.on_event = [sink = in.stream_cb](const sp::Event& event) {
            if (!sink) return;
            const auto* delta = std::get_if<sp::PartDelta>(&event);
            if (delta && delta->payload.kind == sp::PartKind::Text &&
                delta->payload.channel == sp::DeltaChannel::Content)
                (*sink)({neograph::graph::GraphEvent::Type::LLM_TOKEN,
                         "chat", neograph::json(std::string(delta->payload.bytes))});
        };
        request.cancel_token = in.ctx.cancel_token;
        request.options.deadline = in.ctx.deadline;
        auto result = co_await neograph::graph::observe_provider_result(
            in.ctx, invoke_provider(provider_, std::move(request), {}, {},
                neograph::graph::provider_call_broker(in.ctx),
                neograph::graph::make_provider_call_identity(in.ctx, get_name())));
        neograph::graph::record_usage(in.ctx, result);
        neograph::outcome_or_throw(result);
        neograph::graph::NodeOutput out;
        out.writes.push_back(neograph::graph::provider_messages_write(result));
        co_return out;
    }
    std::string get_name() const override { return "chat"; }
};
```

### Case 4 — Node Using Command / Send (Migrating `execute_full`)

**Old:**
```cpp
NodeResult execute_full(const GraphState& state) override {
    NodeResult r;
    r.writes.push_back({"step", json("dispatched")});
    Command command;
    command.goto_node = "next_router";
    r.command = command;   // Force routing
    return r;
}
```

**New:**
```cpp
asio::awaitable<NodeOutput> run(NodeInput in) override {
    NodeOutput out;   // NodeOutput == NodeResult — alias of the same type
    out.writes.push_back({"step", json("dispatched")});
    Command command;
    command.goto_node = "next_router";
    out.command = command;
    co_return out;
}
```

`NodeOutput` is an alias for `NodeResult` — legacy `NodeResult` code still
compiles.

## Common Mistakes

### `NodeInput in` is by-value

```cpp
// ❌ Wrong — coroutine ref-param UAF, SEGV in pybind async path
asio::awaitable<NodeOutput> run(const NodeInput& in) override { ... }

// ✅ Correct
asio::awaitable<NodeOutput> run(NodeInput in) override { ... }
```

Reason: The coroutine frame must take an argument copy for safety. Receiving by
reference leaves `in.state` dangling after the caller's stack frame disappears.
A real bug that occurred during PR 2 work.

### cancel / store / stream_cb all come from `in.ctx`

Legacy nodes received cancel tokens via smuggling channels like
`state.run_cancel_token_`, but v0.4 introduced `RunContext` as official plumbing:

```cpp
asio::awaitable<NodeOutput> run(NodeInput in) override {
    // Check cancellation signal
    if (in.ctx.cancel_token && in.ctx.cancel_token->is_cancelled()) {
        throw CancelledException("user cancelled");
    }

    // Store access (issue #27)
    if (in.ctx.store) {
        auto user_pref = in.ctx.store->get({"users", in.ctx.thread_id}, "lang");
        // ...
    }

    // Streaming sink (nullable)
    if (in.stream_cb) {
        (*in.stream_cb)({GraphEvent::Type::NODE_END, "my_node", json(...)});
    }

    co_return NodeOutput{};
}
```

Fields of `in.ctx` available to C++ nodes are: `cancel_token`, `usage`,
`thread_id`, `step`, `stream_mode`, `store`, `resume_value`, `deadline`, and
`trace_id`. Set the latter two on `RunMetadata`; the engine preserves them
through nested subgraphs. Checkpoint routing remains internal to the engine,
not a public `RunContext` field. Python exposes `trace_id`, `run_id`,
`model_token_budget`, `has_deadline`, and `deadline_remaining_ms`; the raw C++
steady-clock deadline remains intentionally opaque.

### Migrating `_full` virtuals — finish with `co_return out;` in one line

The most common confusion for legacy `execute_full` users:
"`NodeResult` is the old type, but must I return `NodeOutput`?"
→ They are aliases of the same type. Just do `NodeOutput out;
out.writes=...; out.command=...; out.sends=...; co_return out;`.

## What Happens If You Don't Migrate

In v0.9.0 and later, the legacy 8 virtuals are gone.

- C++ legacy `override` generates compilation errors like `'execute' marked
  override but does not override`.
- Python nodes implementing only `execute()` raise `NotImplementedError`
  requiring `run(input)`.

Do not use a transition pattern that leaves the old method names in place. The
engine calls only `run(NodeInput)`, so old bodies never execute.

## Is There a Bulk Migration Script?

No — the virtual signatures vary across 8 forms, making regex-based conversion
impractical. Users should read the case-by-case examples (the 4 examples above)
and migrate manually.

For the most common pattern (override only `execute(state)`), the following
sed/awk one-liner may assist with the initial pass — human review is required:

```bash
# Very rough initial pass — nodes with single-line execute override only.
# Always dry-run without -i first.
grep -lE 'execute\(const GraphState' src/**/*.cpp
# Manually edit each resulting file to the new pattern.
```

Complex nodes (`execute_full`, `execute_stream_async`, etc.) must be edited
manually. There are no shortcuts.

---

# Migration 2: Typed lossless Provider cutover (mandatory recompile)

This is a source and binary break: recompile every C++ consumer and custom provider with matching new headers/libraries. `CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, the descriptor interpreter and Responses WebSocket path are removed, with no aliases or compatibility bridges. The SDK is unstable `0.0.0`, interface revision 3 / shared ABI 3, with out-of-line capability checks; that is not a stable release claim. Current runtime/archive support is Linux/POSIX; no Windows, macOS or WASM runtime qualification is implied. Python provider bindings/wrappers are deferred and not ported by this C++ change.

Fresh installed find_package Program C++/C ABI/dualQuickJS consumers and the NeoGraph/SchemaProvider typed two-request lifetime/native/raw/mismatch consumer passed. Interface/ABI declarations alone remain distinct from this exercised package result; broader platforms and stable release are not claimed.

The public contract is owned typed preparation and dispatch, not paired virtual completion methods. `ProviderRequest.payload` is the SDK variant of Chat, Messages, Responses, Gemini or Interactions requests. `ProviderMode::Collect` / `Stream` selects transport independently of an observer. `on_event` receives borrowed typed `sp::Event` views; copy only data needed after the callback. No raw JSON overrides or native-state import through portable projections are admitted.

```cpp
#include <neograph/provider.h>
#include <neograph/runtime_interposition_consumer.h>
#include <neograph/controlled_provider.h>

// Public operation signatures (the only virtual operation is prepare).
// ProviderRequest owns the SDK request variant, mode, options and observer.
// invoke[_async](request) = prepare once, then dispatch the same handle.
// dispatch[_async](prepared) returns sp::runtime::Result.
```

### ProviderRequest / ProviderControls

```cpp
#include <neograph/llm/schema_provider.h>
#include <neograph/types.h>

sp::runtime::Result call_provider(
    neograph::Provider& provider, std::string model,
    std::vector<sp::Message> history,
    std::function<void(const sp::Event&)> observer) {
    neograph::ProviderControls controls;
    controls.max_output_tokens = 128;  // optional caller-selected wire cap
    auto request = neograph::make_provider_request(
        provider, std::move(model), std::move(history), {},
        std::move(controls), neograph::ProviderMode::Stream);
    request.on_event = std::move(observer);
    auto prepared = provider.prepare(std::move(request));
    return provider.dispatch(std::move(prepared));  // owns Completion or Failure
}
```

### PreparedProviderRequest / ProviderBudgetClaim
`prepare()` validates and encodes exactly once, producing a move-only `PreparedProviderRequest` with the original deadline and cancellation state. Durable callers bind its `Provider::request_digest()` to their assembly, reserve an admitted budget claim, write the dispatch receipt, then consume that same handle through `ControlledProvider::dispatch_prepared(_async)`. They never rebuild a request after the gate. Duplicate receipts never redispatch. Custom providers implement `get_name()`, `family()` and `prepare()` using `prepare_runtime()` or `prepare_local()`; local callbacks capture owned shared state, not `this`.

Optional `ProviderControls` are caller choices, not mandatory defaults or silently clamped caps. Unsupported family controls fail before dispatch. Bounded calls require genuine admitted model input/output facts; missing facts fail with `LimitUnknown`. A reservation is conservative spending authority, not reported usage, a forecast or an invoice. Unknown/partial/delivery-unknown outcomes retain their hold; genuine final reports settle it, including oversized usage. Retry is one explicit layer, off by default, with a bounded window and unknown-prior hold; no hidden resend.


A provider call returns `sp::runtime::Result`: an immutable, owned `std::shared_ptr<const sp::Outcome>`, containing `sp::Completion` or `sp::Failure`. Retain the whole outcome, not only display text. Ordered messages/parts, native continuation, complete wire envelopes, ordered raw observations, stop evidence and genuine attempt metadata survive the call and client destruction. Usage counters are nullable `uint64_t` values with evidence, stage and quality: missing is unknown, never zero. A failure retains its original partial outcome. `ProviderFailure::outcome()` and `ProviderObserverError::outcome()` preserve that result; the latter also preserves the observer exception in `cause()`.

If post-effect accounting or terminal-receipt persistence fails after a real result exists, `ProviderDispatchOutcomePersistenceError` retains the original immutable result in `outcome()` and the original persistence exception in `cause()`. If delivery also failed, `delivery_error()` retains the original observer exception. Successful persistence followed by observer failure rethrows that original observer exception unchanged; an unknown/no-result transport failure does not fabricate an outcome.
### SchemaProvider

`SchemaProvider` accepts an admitted `sp::descriptor::ValidatedDescriptor`, `sp::runtime::Options` and optional `SchemaProvider::Defaults`. Descriptor loading is closed/versioned data admission, not a request/response interpreter or arbitrary primitive registry. Credentials belong in runtime options, not public descriptor files. Defaults contain only typed OpenRouter routing and Responses retention (`responses_store`); the latter is valid only for Responses. Hosted OpenRouter routing, retention and JSON formats remain declared typed controls. Images, Veo and Decisions use separate NeoGraph typed clients and separate authorization; they do not inherit an SDK chat grant.

```cpp
#include <neograph/llm/schema_provider.h>
#include <descriptor/descriptor.h>
#include <stdexcept>
#include <variant>

std::shared_ptr<neograph::llm::SchemaProvider> admitted_provider(
    std::string_view descriptor_json, std::string api_key) {
    auto loaded = sp::descriptor::load(descriptor_json);
    if (const auto* error = std::get_if<sp::descriptor::ConfigError>(&loaded))
        throw std::invalid_argument(error->message);
    sp::runtime::Options options;
    options.api_key = std::move(api_key);
    neograph::llm::SchemaProvider::Defaults defaults;
    return std::make_shared<neograph::llm::SchemaProvider>(
        std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded)),
        std::move(options), std::move(defaults));
}
```

### Native history / budget

`ChatMessage` / `ChatTool` and JSON are portable projections, not native authority. Portable formats remain [`provider-message-v2`](../schemas/provider-message-v2.schema.json) and [`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json). Genuine C++ checkpoint sidecars retain native seals in memory. Durable native history requires host-owned `sp::NativeArchive`: closed v3 / `spna3`, with authenticated owner-private custody and an independent key. Archive v2 is rejected, not upgraded or interpreted. Authentication binds every semantic descriptor choice (origin/paths/headers, policy, request field mappings, usage path and stop mappings), owner and exact custody binding. It is neither encryption nor vendor-issuer authentication; never publish archive bodies, keys, native blobs or raw wire observations. An archive is evidence storage, not a money grant or a spending lease. Program/external banks remain independently journal-owned; snapshot copies cannot create credit.

**Standalone bank journal correction — current contract revised; exercised runtime evidence below.** The owner-approved protocol requires a monotonic trusted-store namespace obligation and a real immutable original owner/thread/graph scope, ceiling, deadline/clock identity and generation. Only exact durable head CAS over the full checkpoint commitment and revision may issue a host-owned opaque lease. Exact pending effect windows must persist before provider I/O; settlement must use genuine SDK outcomes and actual charges, nullable reports, holds and dedup identities. Checkpoint and next head must publish atomically under the same owned actor/revision. Removing bank metadata, pruning a checkpoint, replaying an old authenticated snapshot, overwriting the same ID or losing the actor must not grant credit. Tightening a 130 ceiling to 129 with an existing 65 hold cannot admit another 65; a proven no-effect failure may release the unchanged head so authentic 130 recovery can still proceed. Crash/unknown/lost-lease windows remain held without refund, retry or fallback. Plain/pristine archive configuration grants no money or native spending lease, and current `config.usage` cannot replace an existing standalone obligation; Program/external-bank journal ownership is unchanged. This is the required contract; actual currency/custody evidence and instrumentation limits are reported below, not a stable released API guarantee.

**Current declarations; integrated runtime evidence below:** `<neograph/graph/checkpoint.h>` declares `ManagedBudgetLeaseScope` with `owner_scope`, logical `thread_id`, private backend `storage_thread_id`, `graph_identity`, `original_ceiling`, `original_deadline_ticks` and `deadline_clock_identity`. `OwnedManagedBudgetLease` exposes read-only `scope()`, `actor_id()`, immutable `bank_generation()`, `revision()`, `head_checkpoint_id()` and `head_commitment()`; it has no public authority-import constructor. `ManagedBudgetEffectReceipt` exposes `active()`, `effect_id()`, `claim_amount()` and `request_digest()`; a default receipt grants nothing. `CheckpointStore` declares `acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`, `begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`, `settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`, `publish_managed_budget_checkpoint(lease, checkpoint)` and `release_managed_budget_lease(lease)`, with `_async` counterparts. Sync `CheckpointStoreCore` and `AsyncCheckpointStore` expose their respective variants. `managed_budget_checkpoint_commitment(checkpoint)` covers the full durable checkpoint, not just bank JSON. These declarations do not establish backend CAS, currency safety, installed ABI compatibility or a successfully exercised runtime path.

**Genuine InMemory shared-bank fork retained and exercised.** The original genuine C++ fork uses ONE original financial journal and trusted current branch heads, not cloned grants. `publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)` (and `_async`) requires the authentic current source/full commitment and actual same-bank native C++ pointer; durable standalone forks remain explicitly unsupported. `OwnedManagedBudgetLease::scope()` and original owner/thread/graph, ceiling, deadline/clock and generation remain immutable. Read-only store-issued `execution_thread_id()` / `execution_storage_thread_id()` select the execution branch separately; `GraphState::budget_original_thread_id()` identifies the original financial bank. Exact selected-branch head CAS and global actor/revision serialize all branches against canonical current counters, pending effects and burned identities. Original and fork branches remain usable without replenishment; stale snapshots, copied checkpoints and imported JSON cannot mint aliases or rewind heads. The original root30 → charge3 → original continuation6 → fork lower20 → continuation9 same-bank proof PASSED in the unchanged test_graph_engine.cpp:810–913; saved original ceiling30 is separate from effective fork ceiling20; widening31 and JSON-only restore must reject. Unbounded reported observations are factual data, not finite grants. Only a proven zero-effect lease can release an unchanged head; unknown/pending effects keep their obligations.

**Current release-error contract; exercised suite/probes below.** `graph::ManagedBudgetLeaseReleaseError` in `<neograph/graph/engine.h>` derives from `ProviderOutcomeError`. `cause()` preserves the original execution exception and `release_error()` exposes the secondary durable lease-disposition failure. `outcome()` retains genuine SDK evidence when available and is null when no SDK outcome exists; release failure cannot invent an outcome or permit redispatch. Closed `_neograph_managed_budget_scope` metadata describes original logical scope/cap/deadline clock/generation, but is data rather than backend CAS authority.

**Archive-owner/retention contract; exercised suite/probes below.** Only finite standalone roots or authenticated finite sources inherit an omitted original owner from the genuinely configured `sp::NativeArchive::owner_scope()`; unbounded/plain owner metadata semantics are unchanged. An explicitly conflicting archive owner is rejected before lease acquisition. `CheckpointStore::retains_native_checkpoint() const noexcept` and the corresponding Core/Async storage capability default to false; the real InMemory backend overrides true, and wrappers must delegate actual retention. This read-only description permits legitimate unleased/plain/unbounded C++ native checkpoint custody; it grants neither spending credit nor native replay authority. Leased custody uses the actual store-issued receipt rather than a JSON flag or guessed store type.

**Native-custody pre-I/O gate; exercised suite/probes below.** Beginning a managed effect requires a genuinely bound NativeArchive or the actual local store-issued private C++ retention capability before any pending-effect, slot or held-window mutation. The private capability is never imported from JSON or transferred over the wire. gRPC requires real client and server archives even when the remote backend is InMemory, because a C++ sidecar cannot cross that boundary. Original anonymous owner scope remains empty when no archive supplies a finite source owner; a real archive binding must match the original scope. Financial head/lease evidence alone does not prove native-custody readiness.

Diagnostic JSON preserves original raw bytes, including syntactically valid duplicate-key documents; executable request/configuration admission still rejects duplicates. Original non-2xx response JSON remains in `http.error` evidence, without a second lossy parse. A named SSE error takes precedence over a later normal stream close. Diagnostic/provider metadata is bounded by its admitted source extent, not an unrelated tiny error-text cap.

`ProviderRequest::observer_limits` is host-only: explicitly supplied `max_events` and `max_bytes` must be positive and may only lower admitted SDK delivery ceilings. `provider-request/v3` digests the effective limits, mode, encoded body, retry policy and all semantic descriptor bindings. The bridge charges actual PMR vector/map capacity plus owned event/document bytes across both queued and draining batches; cancellation is requested outside its queue mutex. Generic channels called `messages` are not coerced to chat. Mapping a native `history` channel into `messages` preserves its C++ sidecar, rather than manufacturing native authority from JSON.

`ProviderOutcomeError` is the common outcome-preserving host-error base; `ProviderObserverError` and `ProviderDispatchOutcomePersistenceError` retain the complete drained SDK result and original `cause()`. The persistence error also retains secondary observer failure in `delivery_error()`. `ProviderFailure::outcome()` retains the SDK failure itself. These are evidence, not permission for Node/Program to redispatch: the SDK is the sole owner of provider retries, and a caller-selected `max_output_tokens` is never silently clamped.

`ProgramFailure` retains live `provider_outcome` and `provider_cause`. Its canonical factual SDK witness binds genuine archive custody to owner/run/version/bundle/operation/attempt; Runtime eagerly restores configured custody before exposing a recovered failure. Public data-only `ProgramResult::create()` cannot bypass this with a prefilled witness, and an unresolved parsed seal is not an executable result. After process restart the original exception pointer is unavailable (`provider_cause == nullptr`), not recreated from text. A failure that cannot be persisted cannot be serialized, published or replayed.

`RecordedBindingSet` is source-bound, move-only data, never a caller-supplied dispatcher. The trusted Catalog `recorded_capability_binder` independently materializes captured-only capabilities from real persisted source events. `ProgramRuntime::replay_recorded()` checks original selected-source permissions, then transfers the actual remaining bank through durable CAS; inherited spend is not a new model grant. The old `start_recorded` renewal API is removed. InMemory, File, SQLite and PostgreSQL Program stores preserve the exact immutable owned lease throughout execution; expiry does not renew it. Controlled JavaScript still validates the underlying capability manifest and consumes exact completed command outcomes without redispatching external effects.

**Recorded-control causal fix exercised in the full suite.** Captured command replay durably reserves only new CPU wall-time/Core work before execution, then publishes measured work and any newly produced Core checkpoint through the result CAS. It consumes no new model, money or Program-operation allowance and does not redispatch captured external effects. An unreconciled reservation remains debited. The reservation selects the authenticated settlement transition rather than an ordinary Running→Running transition that rejected the first new Core checkpoint. Await channel receive, timer wait/cancel and handoff wait initiation/release are serialized on their owning executors/strands; the existing Recorded CPU/Memory await/handoff scenarios passed in the full suite; remote TSan coverage limits remain explicit below.

**Completed paid observations; not universal qualification.** Original `SPQUAL1` base630/1000000 microUSD is unchanged; ONE hash-chained `A` admits approved extension480/3000000 in the same original ledger, aggregate1110/4000000, with cumulative calls/spent/holds/settlements and no new grant ID/header/reset. Exact declaration bytes/file identity and original authorization/baseline/catalog/activation/ledger-prefix hashes/totals remain pinned; removal/replacement/change fails closed. The final canonical ledger is calls1110/spent437958/held1287828 microUSD, eventA1, limits1110/4000000; spent+held is US$1.725786 LOCAL catalogue meter, not an invoice. The documented five-family60-pair baseline completed600 requests: Chat60/60, Responses60/60, Messages60/60, Generate56/60 (four incorrect-vision SSE), Interactions57/60 (one buffered and two SSE incorrect-vision); aggregate293/300 pairs, not300/300. Other old600 financial records remain preserved, not full behavioral proof. Earlier M5/media one-shot cohorts are unchanged. The earlier three-round Google prerequisites retain two invalid-tool and one unreadable-positive failures. No further paid calls are authorized. Final SDK evidence and native-axis limits are separate from baseline success. Earlier activation/reopen smoke remains recorded at calls610/spent219159/held751233 after two reopens, with SDK meter/canary/vision four tests passed19.38seconds; these are scoped prior checkpoints, not final ledger totals. The earlier verified Chat60-pair cohort retains120 actual attempts,120 UpperBound charges and no UnknownHold.

**Native-axis observations, not cryptographic verification or native consumption/equivalence.** Generate accepted mutation, omission and duplication. Interactions accepted the isolated genuine source/positive control, one-owner signature mutation, thought-carrier omission, call-carrier omission and duplication. Removing all thoughts/signatures returned generic400; removing all signature fields while keeping THOUGHT items also returned generic400. The last capture had a local encoded-original retention control, not a same-capture server positive; the earlier positive cohort remains genuine. These observations establish an aggregate-carrier-absence boundary only, not issuer/signature validation or vendor consumption. Actual reports: SDK `config/qualification-extension-results.json`, `qualification-final-summary.json`, `qualification-native-axis-results.json`, `qualification-combined-omission-results.json`, `qualification-signature-presence-results.json`; prerequisite-failed/not-run/negative-inconclusive states remain factual. Thought-only/carrier-only omissions were accepted while another carrier remained; this does not strengthen issuer-validation or native-consumption claims.

**Actual integrated proof and remaining limits.** Latest Core full run:2242 tests, zero failures,16 skips (14 RAM process-loss cases not applicable; two live-credential gates),130.17seconds. `PgNestedJsonRoundTrips` preserved exact duplicate keys/order/null metadata, blob and residual in0.18seconds. The unchanged original shared-bank fork and existing Recorded CPU/Memory await/handoff scenarios passed. Real wrappedMemory/SQLite/PostgreSQL/gRPC finite130/hold65/lower129/strip/old-head/pruning/no-archive/import probes passed plain and ASan+UBSan. LOCAL Memory/SQLite/PostgreSQL TSan scopes:seven passed,zero warnings. Full mixed gRPC plus system Abseil/Protobuf TSan exited66 with402 race warnings in dependency/generated-RPC stacks: an instrumentation/coverage limit, not a proven false positive; remote TSan/race-freedom is NOT claimed and no warning is suppressed. Installed find_package Program C++/C ABI/dualQuickJS three consumers passed. Fresh installed NeoGraph/SchemaProvider typed consumer passed two real HTTP requests, provider destruction before coroutine start, native/tool replay, refusal,known-zero/raw retention and actual LinkedMismatch rejection. Browser Alice/Bob isolation and generation2 replacement were visually verified; PostgreSQL Program Chat six black-box tests passed18.989seconds. Latest SDK26/26 passed,zero failures,74.07seconds. Final ReleaseGraph16 configurations ×3 fresh process repetitions/48 records completed38.29seconds,zero failures,all actual protocol/owned-outcome checks passed. NeoGraph `benchmarks/provider-cutover-final-results.json` and `benchmarks/provider-cutover-final-summary.json` retain this separate final cohort. No compiler or paid model ran during measurement; historical cohorts stay unchanged and semantic/resource equivalence is not claimed. Unstable SDK/ABI3 is not a stable release or broader-platform qualification.

**Minimal paid evidence (2026-10-03), not broad qualification.** Three separately approved one-shot calls produced: Images—one JPEG, 1024×1024, 360685 bytes, input/output/total tokens 19/1408/1427, visually inspected; Veo—one MP4, 1280×720, 4 seconds, 437737 bytes, one generation plus three status GETs, nullable usage, decoded and visually inspected in Chromium; Decisions—`typesafe/jev-1.13`, probability 0.93, input/output tokens 283/21, total unknown, API-reported cost USD 0.000011886. Image USD 0.0336 base plus text/thinking and Veo USD 0.20 are catalog expectations, not invoices; the minimal image smoke did not capture a price-band breakdown. No result renews one-shot authority or authorizes reruns.

The completed chat pairs do not establish native-continuation consumption by a downstream vendor.

Historical Stage 3 (2026-04) design and its measured test counts are preserved as history. Its provider compatibility/crossover decisions are superseded by the typed lossless cutover below; the design ledger is not the current provider API.

- [ABI_POLICY.md](ABI_POLICY.md)
- [ASYNC_GUIDE.md](ASYNC_GUIDE.md)
- [Issue #5](https://github.com/fox1245/NeoGraph/issues/5) — historical decision; superseded provider compatibility policy.

---

# Migration 3: `compile()` worker pool default is 1 (v0.1.4 regression restoration)

## What Changed

`GraphEngine::compile(def, ctx)` default worker count was
`std::thread::hardware_concurrency()` from v0.1.4 (`b59444f`) but is restored to
**`1` (= no engine-owned thread_pool)** in v1.0.

## Why

The `hardware_concurrency` default imposes cross-thread submit overhead (~6-7
µs/task) on all fan-out nodes — bench par measurement (5 workers + summarizer)
regressed from 11.6 µs to 44 µs, a 4× slowdown. Bisecting our measurement
environment pinpointed v0.1.4's `b59444f` as the culprit.

Real production workloads (LLM calls in ms~s range) can ignore submit overhead,
but:
- **Simple graphs (no fan-out)** still pay pool overhead — meaningless
- **Non-thread-safe node state** is exposed to multi-worker by default — a real
  footgun

Thus, the default is safely set to 1, and users must explicitly opt-in for
actual fan-out parallelization.

## Migration

Graphs requiring fan-out parallelization (e.g., multiple `Send` dispatches,
`parallel_group`, deep_research's 5-researcher fan-out) must call explicitly
after `compile()`:

```cpp
auto engine = GraphEngine::compile(def, ctx);
engine->set_worker_count_auto();  // hardware_concurrency()
// or
engine->set_worker_count(4);  // specify exact N
```

```python
engine = ng.GraphEngine.compile(def, ctx)
engine.set_worker_count_auto()
```

Simple graphs (no fan-out) or light fan-out graphs (LLM call dominant) keep the
default — 0 pool overhead.

## What Happens If You Don't Migrate

- User graphs with fan-out execute serially on a single thread (consistency
  guaranteed)
- Actual wallclock recovery is not achieved — explicit
  `set_worker_count_auto()` is required

## NeoGraph Internal Examples Affected

The fan-out visibility patch added alongside this change — explicit calls added
to preserve intent. Apply the same pattern if your user code matches:

- `examples/10_send_command.cpp` — sync `sleep_for` ResearcherNode fans out via
  Send, `engine->set_worker_count_auto()` added
- `examples/14_plan_executor.cpp` — 5 sub-topic Send fan-out (sync sleep_for),
  same addition
- `examples/21_mcp_fanout.cpp` — 3 MCP tool calls fired concurrently, same
- `examples/36_classifier_fanout.cpp` — already had `set_worker_count(5)`
  explicit. Fixed comment stating false default (current default is
  hardware_concurrency)
- `src/core/deep_research_graph.cpp` `create_deep_research_graph()` builder —
  calls `set_worker_count_auto()` immediately after `compile()` so supervisor's
  N researchers truly run concurrently

`examples/05_parallel_fanout.cpp` uses coroutine timer overlap on `io_context`
(no sync sleep), so worker pool has no effect — left unchanged.

If the same pattern exists in user code:

```cpp
auto engine = GraphEngine::compile(def, ctx);
engine->set_worker_count_auto();   // ← add this line
```

See ROADMAP_v1.md perf section for detailed measurements (separate addition).

> Historical Stage 3 (2026-04) design and its measured test counts are preserved as history. Its provider compatibility/crossover decisions are superseded by the typed lossless cutover below; the design ledger is not the current provider API.

---

# Migration 4: C++ ABI and Mandatory Rebuilds

NeoGraph now assigns every compiled public library the project `VERSION` and
major `SOVERSION`. All pre-v1 releases use ABI generation 0, but `0.x` binary
compatibility is not guaranteed. Rebuild every C++ consumer when the changelog
announces a boundary. In particular, moving from `0.11.1` or earlier to the
release containing bounded `NodeCache` requires a rebuild because `NodeCache`
and `EngineConfig` object layouts changed.

The Provider migration above does **not** change the established `Provider`
vtable. Existing Provider binaries remain subject only to the release-wide
boundaries. The planned `CheckpointStore` async migration must follow the same
policy: any pre-v1 vtable change requires an announced rebuild, while v1 and
later should add separate capability interfaces and adapters instead of
changing the stable layout.

See [Binary Compatibility Policy](ABI_POLICY.md) for platform library names,
all known rebuild boundaries, and CI verification.
