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

Event-driven Provider dispatch adds a separate mandatory pre-v1 rebuild boundary: `CancelToken` now uses `std::stop_source` and exposes `stop_token()` for executor-independent native subscriptions. Existing `cancel()`, `is_cancelled()`, `fork()`, `bind_executor()` and `slot()` callsites remain source-compatible, but old inline implementations are not binary-compatible. Recompile every C++ consumer/extension with matching NeoGraph headers/libraries; replacing only a shared library is insufficient. Provider notifications preserve SDK `join()`, FIFO owned outcomes, absolute deadlines and admission/authority budgets. See [ABI policy](ABI_POLICY.md) and [matched measurements](../benchmarks/provider-notification-summary.json).

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

This is a source and binary break: recompile every C++ consumer and custom provider with matching new headers/libraries. `CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, the descriptor interpreter and Responses WebSocket path are removed, with no aliases or compatibility bridges. The SDK is alpha `0.3.2`, interface revision 6 / shared ABI 6, with out-of-line capability checks; that is not a stable release claim.

Replace removed `Provider::complete`, `complete_async`, `complete_stream` and `complete_stream_async` callsites with explicit-mode requests and `invoke` / `dispatch` (or their C++ async peers). `Agent::complete` remains a separate one-turn API returning an owned outcome.

CMake 3.20 or newer is required. SchemaProvider runtime remains mandatory when `NEOGRAPH_BUILD_LLM=OFF` because Core exports owned typed provider contracts. An explicit `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` wins; otherwise CMake prefers an installed `SchemaProvider` runtime package, then downloads the immutable GitHub archive pinned by `cmake/NeoGraphSchemaProvider.cmake` when `NEOGRAPH_FETCH_SCHEMAPROVIDER=ON` (default). For offline builds, install the SDK, set `CMAKE_PREFIX_PATH` to its prefix and pass `-DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`. CMake does not guess a sibling checkout or select the removed bundled interpreter. The SDK runtime requires its transport dependencies even when NeoGraph's optional HTTP module is disabled. The SDK runtime is not available for WebAssembly builds.

Python exposes the same owned request/outcome boundary as C++: `make_provider_request`, `Provider.prepare`, `dispatch` and `invoke`. Use `ProviderMessage` with typed parts for provider history; `ChatMessage` remains a graph convenience projection. A returned SDK failure is available through `ProviderOutcome.failure`, while host observer/settlement exceptions retain `outcome` and `cause`. See the [Python binding guide](python-binding.md) for constructors and GIL/callback behavior.

The public contract is owned typed preparation and dispatch, not paired virtual completion methods. `ProviderRequest.payload` is the SDK variant of Chat, Messages, Responses, Gemini or Interactions requests. `ProviderMode::Collect` / `Stream` selects transport independently of an observer. `on_event` receives borrowed typed `sp::Event` views; copy only data needed after the callback. No raw JSON overrides or native-state import through portable projections are admitted.

```cpp
#include <neograph/provider.h>
#include <neograph/runtime_interposition_consumer.h>
#include <neograph/controlled_provider.h>

// Selected public declarations from neograph::Provider.
class Provider {
public:
    virtual ~Provider() = default;
    virtual std::string get_name() const = 0;
    virtual std::string_view family() const noexcept = 0;
    virtual PreparedProviderRequest prepare(ProviderRequest request) = 0;
    sp::runtime::Result dispatch(PreparedProviderRequest request);
    asio::awaitable<sp::runtime::Result> dispatch_async(PreparedProviderRequest request);
    sp::runtime::Result invoke(ProviderRequest request);
    asio::awaitable<sp::runtime::Result> invoke_async(ProviderRequest request);
    static std::string request_digest(const PreparedProviderRequest& request);
    static std::optional<std::uint64_t> conservative_token_upper_bound(
        const PreparedProviderRequest& request);
};
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

The retained per-call controls are typed fields, not an `extra_fields` dictionary:

| Family | Additional `ProviderControls` |
|---|---|
| Chat | `chat_reasoning`, `include_reasoning`, `usage_include`, `models`; these require a declared OpenRouter origin. Scalar `reasoning_effort` remains separate. |
| Responses | `previous_response_id`, `previous_response_history`, `parallel_tool_calls`, `verbosity`, `truncation`, `responses_include`. |
| Messages | `thinking_mode`, `output_effort`, `cache_control`, `messages_tool_choice`, declared-origin OpenRouter `provider` routing. |
| Gemini Generate | `gemini_history_mode`, `gemini_thinking_level`, `temperature`, `safety_settings`, `gemini_tool_choice`. |

Messages manual thinking requires a policy-admitted budget below the output cap; adaptive and disabled modes forbid a thinking budget. Manual/adaptive thinking omit valid temperature, but invalid values and model-prohibited temperature reject before omission. Disabled thinking emits admitted temperature. Model-prefix restrictions are admitted policy facts, matched ASCII case-insensitively against the full model or its last `/` suffix. Gemini thinking level and thinking budget are mutually exclusive; its typed tool choice and `required_tool` are mutually exclusive. Unsupported family/origin/value combinations reject before I/O.

### PreparedProviderRequest / ProviderBudgetClaim
`prepare()` validates and encodes exactly once, producing a move-only `PreparedProviderRequest` with the original deadline and cancellation state. Durable callers bind its `Provider::request_digest()` to their assembly, reserve an admitted budget claim, write the dispatch receipt, then consume that same handle through `ControlledProvider::dispatch_prepared(_async)`. They never rebuild a request after the gate. Duplicate receipts never redispatch. Custom providers implement `get_name()`, `family()` and `prepare()` using `prepare_runtime()` or `prepare_local()`; local callbacks capture owned shared state, not `this`.

Optional `ProviderControls` are caller choices, not mandatory defaults or silently clamped caps. Unsupported family controls fail before dispatch. Bounded calls require genuine admitted model input/output facts; missing facts fail with `LimitUnknown`. A reservation is conservative spending authority, not reported usage, a forecast or an invoice. Unknown/partial/delivery-unknown outcomes retain their hold; genuine final reports settle it, including oversized usage. Retry is one explicit layer, off by default, with a bounded window and unknown-prior hold; no hidden resend.


A provider call returns `sp::runtime::Result`: an immutable, owned `std::shared_ptr<const sp::Outcome>`, containing `sp::Completion` or `sp::Failure`. Retain the whole outcome, not only display text. Ordered messages/parts, retained native continuation and family-supplied wire evidence, ordered raw observations, stop evidence and genuine attempt metadata survive the call and client destruction. Usage counters such as `input_total`, `output_total` and `total` are `std::optional<sp::Count>`; each present count has a `uint64_t value` and `Evidence`. `Usage` also records stage, quality and conflicts. Missing is unknown, never an invented zero. A failure retains its original partial outcome. `ProviderFailure::outcome()` and `ProviderObserverError::outcome()` preserve that result; the latter also preserves the observer exception in `cause()`.

Wire evidence is family-supplied and optional: `sp::Completion::wire_envelope` may be null (`ProviderCompletion.wire_envelope` is `None` in Python). Buffered Chat currently retains the full response JSON in `raw_events` as a `RawWire` with `type == "chat.completion"` and the document in `payload`; it leaves `wire_envelope` null. Read the evidence where the family retains it; no fallback envelope is fabricated. Native continuation and raw buffers remain protected evidence, excluded from trace payloads.

`UsageAccumulator::snapshot()` returns accumulated reports. `total_tokens_wide()` returns charged tokens plus unresolved reservations; it must not be displayed as reported usage. Settlement requires a final, consistent report with input and output counts and charges the largest supported total, without clamping oversized usage. A missing counter in any accumulated report remains unknown in the aggregate. A reservation, a local charge and a vendor invoice are different records.

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

Plain `sp::descriptor::load` treats headers as literals: `${VAR}` is not expanded. For explicit host-side preprocessing before admission, use `sp::descriptor::load_with_environment_headers(source, overrides, policy)` or deterministic `load_with_deployment_headers(source, overrides, environment, policy)` with `DeploymentHeaderEnvironment`. The environment helper reads optional `ANTHROPIC_WORKSPACE_ID` / `ANTHROPIC_BETA` for Messages; unset or empty values are omitted. Literal descriptor headers override environment values, and explicit overrides win over both, case-insensitively. Duplicate overrides, invalid or reserved headers fail final admission. No encoder evaluates templates or mutates an admitted descriptor.

### Native history

Checkpoints keep a provider's native history (reasoning, signatures, ordered tool calls) only as C++ sidecars in the process that produced it. `ChatMessage`, `ChatTool` and JSON are portable copies and carry no native authority. A host that must keep native history across restarts configures an `sp::NativeArchive`. The [typed provider reference](reference-en.md#portable-projections) describes these contracts.

See also:
- [ABI_POLICY.md](ABI_POLICY.md)
- [ASYNC_GUIDE.md](ASYNC_GUIDE.md)

---

# Migration 3: `compile()` worker pool default is 1 (v0.1.4 regression restoration)

## What Changed

`GraphEngine::compile(def, ctx)` default worker count was
`std::thread::hardware_concurrency()` from v0.1.4 (`b59444f`) but is restored to
**`1` (= no engine-owned thread_pool)** in the current pre-v1 API.

## Why

The `hardware_concurrency` default imposes cross-thread submit overhead (~6-7
µs/task) on all fan-out nodes — bench par measurement (5 workers + summarizer)
regressed from 11.6 µs to 44 µs, a 4× slowdown.

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

Without an opted-in worker pool, CPU-bound fan-out runs on the caller's executor; asynchronous I/O can still overlap. Use `set_worker_count_auto()` or an explicit worker count when separate execution threads are required. Worker count alone does not guarantee consistency or speedup.

---

# Migration 4: C++ ABI and Mandatory Rebuilds

NeoGraph now assigns every compiled public library the project `VERSION` and
major `SOVERSION`. All pre-v1 releases use ABI generation 0, but `0.x` binary
compatibility is not guaranteed. Rebuild every C++ consumer when the changelog
announces a boundary. In particular, moving from `0.11.1` or earlier to the
release containing bounded `NodeCache` requires a rebuild because `NodeCache`
and `EngineConfig` object layouts changed.

The typed Provider migration changes its virtual contract to `get_name`, `family` and `prepare`, and removes the old completion virtuals. Existing Provider binaries are incompatible; rebuild custom providers and every dependent C++ consumer with matching headers and libraries. Core also exposes SchemaProvider types, so an LLM-disabled build still needs the SDK runtime. Future stable interfaces should prefer separate capability interfaces and adapters rather than mutate a stable layout; the current pre-v1 interfaces do not promise binary compatibility.

See [Binary Compatibility Policy](ABI_POLICY.md) for platform library names,
all known rebuild boundaries, and CI verification.
