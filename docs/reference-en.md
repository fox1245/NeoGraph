# NeoGraph API — Narrative Tour

**Languages:** [English](reference-en.md) | [한국어](reference-ko.md) | [日本語](reference-ja.md) | [简体中文](reference-zh-CN.md)

This document is a **guided narrative tour** of NeoGraph's public API,
not a complete reference. It walks through the modules in the order
you'll meet them when building a real agent: foundation types →
provider/tool interfaces → graph types → engine → checkpoint store →
multi-LLM → MCP. Provider sections document the typed cutover; public headers
are authoritative. Several modules
(`neograph::a2a`, `neograph::acp`, `neograph::async`,
`SqliteCheckpointStore`, `PostgresCheckpointStore`,
`NodeCache`, `AsyncTool`, `create_deep_research_graph`)
have **public API in the headers that this tour does not cover**.

> **For the complete, type-by-type API surface — including every
> module above — use the public headers under `include/neograph/`,
> linked below. This narrative tour is the recommended entry point;
> the headers are the canonical reference.**

The trade-off this split buys: the narrative stays small enough to
read end-to-end, while the detailed reference remains beside the
implementation in `include/neograph/`.

**Modules at a glance:**

| Module | Namespace | Description | Tour | Header |
|--------|-----------|-------------|------|---------|
| Core | `neograph` | Foundation types, Provider and Tool interfaces | [§1–§3](#1-foundation-types) | [Provider](../include/neograph/provider.h) |
| Graph | `neograph::graph` | Graph engine, nodes, state, checkpointing, store | [§4–§11](#4-graph-types) | [GraphEngine](../include/neograph/graph/engine.h) |
| LLM | `neograph::llm` | LLM provider implementations and Agent | [§12](#12-llm-module) | [Agent](../include/neograph/llm/agent.h) |
| MCP | `neograph::mcp` | Model Context Protocol client | [§13](#13-mcp-module) | [MCPClient](../include/neograph/mcp/client.h) |
| Util | `neograph::util` | Concurrency utilities | [§14](#14-util-module) | [RequestQueue](../include/neograph/util/request_queue.h) |
| **A2A** | `neograph::a2a` | Agent-to-Agent JSON-RPC bridge (client + server + streaming) | [Public headers](../include/neograph/) | [A2AClient](../include/neograph/a2a/client.h) |
| **ACP** | `neograph::acp` | Agent Client Protocol — editor↔agent bidirectional RPC over stdio | [Public headers](../include/neograph/) | [ACPServer](../include/neograph/acp/server.h) |
| **Async** | `neograph::async` | Asio HTTP/SSE/WS helpers, ConnPool, run_sync | [Public headers](../include/neograph/) | [WsClient](../include/neograph/async/ws_client.h) |

These additional modules expose public headers under `include/neograph/{a2a,acp,async}/`. This tour does not cover them; consult the headers for their exact public contracts.

**Convenience header:** `#include <neograph/neograph.h>` includes the full core + graph engine API.

CMake 3.20 or newer is required. SchemaProvider runtime remains mandatory when `NEOGRAPH_BUILD_LLM=OFF` because Core exports owned typed provider contracts. An explicit `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` wins; otherwise CMake prefers an installed `SchemaProvider` runtime package, then downloads the immutable GitHub archive pinned by `cmake/NeoGraphSchemaProvider.cmake` when `NEOGRAPH_FETCH_SCHEMAPROVIDER=ON` (default). For offline builds, install the SDK, set `CMAKE_PREFIX_PATH` to its prefix and pass `-DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`. CMake does not guess a sibling checkout or select the removed bundled interpreter. The SDK runtime requires its transport dependencies even when NeoGraph's optional HTTP module is disabled. Linux/POSIX is the tested platform. Windows NTFS and macOS implementations are present but not tested with this SDK, and WASM provider runtime support is not established.

The SDK imported target supplies its `include/SchemaProvider` include root; public examples use `<descriptor/descriptor.h>`, `<runtime/client.h>` and `<neograph/llm/schema_provider.h>` directly, without recipe-only helpers.

The installed SDK package must be exactly `0.3.3`, with matching interface revision 6 headers and shared-library generation 6. A version match alone does not admit older interface/ABI binaries.

```cmake
find_package(SchemaProvider 0.3.3 EXACT CONFIG REQUIRED COMPONENTS runtime transport)
find_package(NeoGraph CONFIG REQUIRED)
target_link_libraries(app PRIVATE neograph::core neograph::llm SchemaProvider::runtime)
```

---

## Table of Contents

- [1. Foundation Types](#1-foundation-types)
  - [ToolCall](#toolcall)
  - [ChatMessage](#chatmessage)
  - [ChatTool](#chattool)
  - [Owned Outcome](#owned-outcome)
  - [Portable projections](#portable-projections)
  - [ADL Serialization](#adl-serialization)
- [2. Provider Interface](#2-provider-interface)
  - [ProviderRequest / ProviderControls](#providerrequest--providercontrols)
  - [PreparedProviderRequest / ProviderBudgetClaim](#preparedproviderrequest--providerbudgetclaim)
  - [Pricing evidence and the price catalog](#pricing-evidence-and-the-price-catalog)
- [3. Tool Interface](#3-tool-interface)
  - [Tool](#tool)
  - [Tool failure contract](#tool-failure-contract)
  - [Node failure contract](#node-failure-contract)
- [4. Graph Types](#4-graph-types)
  - [ReducerType](#reducertype)
  - [ReducerFn](#reducerfn)
  - [Channel](#channel)
  - [ChannelWrite](#channelwrite)
  - [NodeInterrupt](#nodeinterrupt)
  - [Send](#send)
  - [Command](#command)
  - [RetryPolicy](#retrypolicy)
  - [StreamMode](#streammode)
  - [Edge](#edge)
  - [ConditionalEdge](#conditionaledge)
  - [NodeContext](#nodecontext)
  - [GraphEvent](#graphevent)
  - [GraphStreamCallback](#graphstreamcallback)
  - [NodeResult](#noderesult)
  - [ConditionFn](#conditionfn)
  - [Constants](#constants)
- [5. GraphState](#5-graphstate)
- [6. GraphNode](#6-graphnode)
  - [GraphNode (abstract)](#graphnode-abstract)
  - [LLMCallNode](#llmcallnode)
  - [ToolDispatchNode](#tooldispatchnode)
  - [IntentClassifierNode](#intentclassifiernode)
  - [SubgraphNode](#subgraphnode)
- [7. GraphEngine](#7-graphengine)
  - [EngineConfig and EngineResources](#engineconfig-and-engineresources)
  - [RunConfig](#runconfig)
  - [RunResult](#runresult)
  - [GraphEngine](#graphengine)
- [7b. Engine Internals](#7b-engine-internals)
  - [GraphCompiler](#graphcompiler)
  - [Scheduler](#scheduler)
  - [CheckpointCoordinator](#checkpointcoordinator)
  - [NodeExecutor](#nodeexecutor)
- [8. Checkpoint](#8-checkpoint)
  - [Checkpoint (struct)](#checkpoint-struct)
  - [CheckpointStore](#checkpointstore)
  - [InMemoryCheckpointStore](#inmemorycheckpointstore)
- [9. Store](#9-store)
  - [Namespace](#namespace)
  - [StoreItem](#storeitem)
  - [Store (abstract)](#store-abstract)
  - [InMemoryStore](#inmemorystore)
- [10. Loader](#10-loader)
  - [ReducerRegistry](#reducerregistry)
  - [ConditionRegistry](#conditionregistry)
  - [NodeFactory](#nodefactory)
  - [Built-in Registrations](#built-in-registrations)
- [11. React Graph](#11-react-graph)
- [12. LLM Module](#12-llm-module)
  - [SchemaProvider](#schemaprovider)
  - [Agent](#agent)
- [13. MCP Module](#13-mcp-module)
  - [MCPTool](#mcptool)
  - [MCPClient](#mcpclient)
- [14. Util Module](#14-util-module)
  - [RequestQueue](#requestqueue)
- [Usage Examples](#usage-examples)
  - [Minimal ReAct Agent](#minimal-react-agent)
  - [Custom Graph with Conditional Routing](#custom-graph-with-conditional-routing)
  - [Human-in-the-Loop with Checkpointing](#human-in-the-loop-with-checkpointing)
  - [Dynamic Fan-Out with Send](#dynamic-fan-out-with-send)
  - [Routing Override with Command](#routing-override-with-command)
  - [SchemaProvider Multi-LLM Support](#schemaprovider-multi-llm-support)
  - [MCP Tool Integration](#mcp-tool-integration)

---

## 1. Foundation Types

**Header:** `<neograph/types.h>`
**Namespace:** `neograph`

Core data types shared across all modules. These model the LLM chat protocol:
messages, tool calls, completions, and their JSON serialization.

### ToolCall

Represents a single tool invocation requested by the LLM.

```cpp
struct ToolCall {
    std::string id;         // Unique identifier assigned by the LLM
    std::string name;       // Name of the tool to call
    std::string arguments;  // JSON-encoded string of arguments
};
```

| Field | Type | Description |
|-------|------|-------------|
| `id` | `std::string` | Unique identifier for this tool call (assigned by the LLM) |
| `name` | `std::string` | Name of the tool function to invoke |
| `arguments` | `std::string` | JSON-encoded string containing the call arguments |

### ChatMessage

A single message in a conversation. Covers all roles: system, user, assistant, and tool.

```cpp
struct ChatMessage {
    std::string role;                    // "system", "user", "assistant", or "tool"
    std::string content;                 // Text content of the message
    std::vector<ToolCall> tool_calls;    // Tool calls (assistant messages only)
    std::string tool_call_id;           // ID of the tool call this responds to (tool messages)
    std::string tool_name;              // Name of the tool (tool messages)
    std::string tool_status;
    bool tool_retryable = false;
    bool tool_effect_uncertain = false;
    std::vector<std::string> image_urls; // base64 data URLs or HTTP URLs for Vision
    std::string reasoning;
    json reasoning_details = json::array(); // portable data, not native authority
};
```

| Field | Type | Description |
|-------|------|-------------|
| `role` | `std::string` | Message role: `"system"`, `"user"`, `"assistant"`, or `"tool"` |
| `content` | `std::string` | Text content of the message |
| `tool_calls` | `std::vector<ToolCall>` | Tool calls requested by the assistant (empty for non-assistant messages) |
| `tool_call_id` | `std::string` | ID linking this tool result to its originating tool call |
| `tool_name` | `std::string` | Name of the tool that produced this result |
| `image_urls` | `std::vector<std::string>` | Image URLs for multi-modal/vision messages. Accepts `data:image/...;base64,...` or `https://...` |

### ChatTool

Defines a tool available to the LLM.

```cpp
struct ChatTool {
    std::string name;        // Tool name (unique identifier)
    std::string description; // Human-readable description for the LLM
    json parameters;         // JSON Schema describing the tool's parameters
    std::optional<bool> strict; // OpenAI Responses strict mode; unset keeps the SDK default
};
```

| Field | Type | Description |
|-------|------|-------------|
| `name` | `std::string` | Unique tool name |
| `description` | `std::string` | Description shown to the LLM to explain the tool's purpose |
| `parameters` | `json` | JSON Schema object describing accepted parameters |
| `strict` | `std::optional<bool>` | OpenAI Responses strict mode for this function tool. Unset keeps the SDK policy default, which applies only to strict-compatible schemas (every object sets `"additionalProperties": false` and lists each property in `required`); other schemas are sent with `"strict": false`. `false` is sent as given; `true` on an incompatible schema fails before dispatch with `InvalidRequest`. Other families reject `true`. Python: `ChatTool(..., strict=None)` |

### Owned Outcome

A provider call returns `sp::runtime::Result`: an immutable, owned `std::shared_ptr<const sp::Outcome>`, containing `sp::Completion` or `sp::Failure`. Retain the whole outcome, not only display text. Ordered messages/parts, native continuation, wire envelopes when present, ordered raw observations, stop evidence and genuine attempt metadata survive the call and client destruction. Usage counters such as `input_total`, `output_total` and `total` are `std::optional<sp::Count>`; each present count has a `uint64_t value` and `Evidence`. `Usage` also records stage, quality and conflicts. Missing is unknown, never an invented zero. A failure retains its original partial outcome. `ProviderFailure::outcome()` and `ProviderObserverError::outcome()` preserve that result; the latter also preserves the observer exception in `cause()`.

`sp::Completion::wire_envelope` and `sp::PartialCompletion::wire_envelope` are nullable and family-specific; the Python views return `None` when absent. Current buffered Chat leaves this field empty and retains the full response document in `raw_events` as `sp::RawWire{type="chat.completion", payload=document}` (Python `ProviderRawWire`). Check availability and inspect actual typed raw evidence; no fallback synthesizes an envelope or turns a partial failure into success. Owned wire evidence, including provider-private fields, remains with the retained outcome after provider destruction. Native tracing excludes raw envelopes/events and native replay/reasoning; JSON inspection copies grant neither native nor financial authority.

Consumers should use full typed messages/parts, logical roles and text, and authentic native ownership where continuation requires it. Checkpoint and Chat request text content may use a text string or a valid typed text-part array; neither incidental serialized shape is a universal contract.

For genuine `NativeContext` replay, retain the full original `request.messages` prefix and append the directly returned `outcome.messages` in order, preserving their native owners. The direct outcome contains newly returned messages, not the original request history. Replaying only its assistant messages is rejected with `ReplayIneligible` before wire I/O. An assistant restored from `NativeArchive` still needs that full original prefix; archive custody does not replace the lineage check. Graph `RunResult.native_messages` already contains the full history, so do not prepend the original input again.

`UsageAccumulator::snapshot()` returns accumulated reports. `total_tokens_wide()` returns charged tokens plus unresolved reservations; it must not be displayed as reported usage. Settlement requires a final, consistent report with input and output counts and charges the largest supported total, without clamping oversized usage. A missing counter in any accumulated report remains unknown in the aggregate. A reservation, a local charge and a vendor invoice are different records.

If post-effect accounting or terminal-receipt persistence fails after a real result exists, `ProviderDispatchOutcomePersistenceError` retains the original immutable result in `outcome()` and the original persistence exception in `cause()`. If delivery also failed, `delivery_error()` retains the original observer exception. Successful persistence followed by observer failure rethrows that original observer exception unchanged; an unknown/no-result transport failure does not fabricate an outcome.

### Portable projections

`ChatMessage` / `ChatTool` and JSON are portable projections, not native authority. Portable formats remain [`provider-message-v2`](../schemas/provider-message-v2.schema.json) and [`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json). Genuine C++ checkpoint sidecars retain native seals in memory. Durable native history requires host-owned `sp::NativeArchive`: closed v3 / `spna3`, with authenticated owner-private custody and an independent key. Archive v2 is rejected, not upgraded or interpreted. Authentication binds every semantic descriptor choice (origin/paths/headers, policy, request field mappings, usage path and stop mappings), owner and exact custody binding. It is neither encryption nor vendor-issuer authentication; never publish archive bodies, keys, native blobs or raw wire observations. An archive is evidence storage, not a money grant or a spending lease. Program/external banks remain independently journal-owned; snapshot copies cannot create credit.

For `RuntimeHistoryRecord`, no-argument `serialize_canonical()` supports portable records. Durable native history uses `serialize_canonical(archive, owner_id)` and an archive whose owner scope matches that ID. Python exposes the same overload and `RuntimeHistoryRecord.parse(stored_bytes, archive=None, owner_id="")`; its portable defaults do not authorize native recovery without the matching archive. Python parsing and archive-aware serialization release the GIL around native work. JSON observations remain distinct from archive-authenticated custody.

Python retains typed custody through `ContextStore.hydrate_records(range)` and `history_record_by_message_id(feed, message_id)`; the latter returns a record or `None`. `SQLiteContextStore(database_path, archive=None)` accepts the actual archive, and `LocalProgramHost` forwards its optional final `native_history_archive=None` argument into `RuntimeConfig`. These parameters do not fabricate grants or add a durable Program-store backend. A returned `RuntimeHistoryRecord.message` is a detached typed copy with shared authentic native owners; mutating it cannot rewrite the immutable RAW identity. Store construction and typed retrieval release the GIL, and archive-free storage rejects native histories when custody is required.

A RAW `RuntimeHistoryRecord` containing an `Assistant` message requires `RuntimeTrustClass.ModelOutput`; `RuntimeTrustClass.UntrustedInput` accepts only `User` messages. These are the actual Python enum names. The trust class identifies the record's role/provenance boundary; selecting it grants neither native replay custody nor spending or execution authority.

**Standalone budget bank.** A standalone bank requires a monotonic trusted-store namespace obligation and a real immutable original owner/thread/graph scope, ceiling, deadline/clock identity and generation. Only an exact durable head CAS over the full checkpoint commitment and revision may issue a host-owned opaque lease. Exact pending effect windows must persist before provider I/O; settlement must use genuine SDK outcomes and actual charges, nullable reports, holds and dedup identities. A checkpoint and the next head publish atomically under the same owned actor and revision. Removing bank metadata, pruning a checkpoint, replaying an old authenticated snapshot, overwriting the same ID or losing the actor does not grant credit. Tightening a 130 ceiling to 129 with an existing 65 hold cannot admit another 65; a proven no-effect failure may release the unchanged head so that authentic 130 recovery can still proceed. Crash, unknown and lost-lease windows remain held, without refund, retry or fallback. A plain archive configuration grants no money and no native spending lease, and the current `config.usage` cannot replace an existing standalone obligation. Program and external banks remain owned by their own journals.

**Managed budget lease declarations.** `<neograph/graph/checkpoint.h>` declares `ManagedBudgetLeaseScope` with `owner_scope`, logical `thread_id`, private backend `storage_thread_id`, `graph_identity`, `original_ceiling`, `original_deadline_ticks` and `deadline_clock_identity`. `OwnedManagedBudgetLease` exposes read-only `scope()`, `actor_id()`, immutable `bank_generation()`, `revision()`, `head_checkpoint_id()` and `head_commitment()`; it has no public authority-import constructor. `ManagedBudgetEffectReceipt` exposes `active()`, `effect_id()`, `claim_amount()` and `request_digest()`; a default receipt grants nothing. `CheckpointStore` declares `acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`, `begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`, `settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`, `publish_managed_budget_checkpoint(lease, checkpoint)` and `release_managed_budget_lease(lease)`, with `_async` counterparts. Sync `CheckpointStoreCore` and `AsyncCheckpointStore` expose their respective variants. `managed_budget_checkpoint_commitment(checkpoint)` covers the full durable checkpoint, not just bank JSON.

**Shared-bank fork (InMemory).** The original genuine C++ fork uses ONE original financial journal and trusted current branch heads, not cloned grants. `publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)` (and `_async`) requires the authentic current source/full commitment and actual same-bank native C++ pointer; durable standalone forks remain explicitly unsupported. `OwnedManagedBudgetLease::scope()` and original owner/thread/graph, ceiling, deadline/clock and generation remain immutable. Read-only store-issued `execution_thread_id()` / `execution_storage_thread_id()` select the execution branch separately; `GraphState::budget_original_thread_id()` identifies the original financial bank. Exact selected-branch head CAS and global actor/revision serialize all branches against canonical current counters, pending effects and burned identities. Original and fork branches remain usable without replenishment; stale snapshots, copied checkpoints and imported JSON cannot mint aliases or rewind heads. The saved original ceiling stays separate from the effective fork ceiling; widening it, or restoring from JSON alone, is rejected. Unbounded reported observations are factual data, not finite grants. Only a proven zero-effect lease can release an unchanged head; unknown/pending effects keep their obligations.

**Lease release errors.** `graph::ManagedBudgetLeaseReleaseError` in `<neograph/graph/engine.h>` derives from `ProviderOutcomeError`. `cause()` preserves the original execution exception and `release_error()` exposes the secondary durable lease-disposition failure. `outcome()` retains genuine SDK evidence when available and is null when no SDK outcome exists; release failure cannot invent an outcome or permit redispatch. Closed `_neograph_managed_budget_scope` metadata describes original logical scope/cap/deadline clock/generation, but is data rather than backend CAS authority.

**Archive owner and retention.** Only finite standalone roots or authenticated finite sources inherit an omitted original owner from the genuinely configured `sp::NativeArchive::owner_scope()`; unbounded/plain owner metadata semantics are unchanged. An explicitly conflicting archive owner is rejected before lease acquisition. `CheckpointStore::retains_native_checkpoint() const noexcept` and the corresponding Core/Async storage capability default to false; the real InMemory backend overrides true, and wrappers must delegate actual retention. This read-only description permits legitimate unleased/plain/unbounded C++ native checkpoint custody; it grants neither spending credit nor native replay authority. Leased custody uses the actual store-issued receipt rather than a JSON flag or guessed store type.

**Native-custody gate.** Beginning a managed effect requires a genuinely bound NativeArchive or the actual local store-issued private C++ retention capability before any pending-effect, slot or held-window mutation. The private capability is never imported from JSON or transferred over the wire. gRPC requires real client and server archives even when the remote backend is InMemory, because a C++ sidecar cannot cross that boundary. Original anonymous owner scope remains empty when no archive supplies a finite source owner; a real archive binding must match the original scope. Financial head/lease evidence alone does not prove native-custody readiness.

Diagnostic JSON preserves original raw bytes, including syntactically valid duplicate-key documents; executable request/configuration admission still rejects duplicates. Original non-2xx response JSON remains in `http.error` evidence, without a second lossy parse. A named SSE error takes precedence over a later normal stream close. Diagnostic/provider metadata is bounded by its admitted source extent, not an unrelated tiny error-text cap.

`ProviderRequest::observer_limits` is host-only: explicitly supplied `max_events` and `max_bytes` must be positive and may only lower admitted SDK delivery ceilings. `provider-request/v3` digests the effective limits, mode, encoded body, retry policy and all semantic descriptor bindings. The bridge charges actual PMR vector/map capacity plus owned event/document bytes across both queued and draining batches; cancellation is requested outside its queue mutex. Generic channels called `messages` are not coerced to chat. Mapping a native `history` channel into `messages` preserves its C++ sidecar, rather than manufacturing native authority from JSON.

`ProviderOutcomeError` is the common outcome-preserving host-error base; `ProviderObserverError` and `ProviderDispatchOutcomePersistenceError` retain the complete drained SDK result and original `cause()`. The persistence error also retains secondary observer failure in `delivery_error()`. `ProviderFailure::outcome()` retains the SDK failure itself. These are evidence, not permission for Node/Program to redispatch: the SDK is the sole owner of provider retries, and a caller-selected `max_output_tokens` is never silently clamped.

`ProgramFailure` retains live `provider_outcome` and `provider_cause`. Its canonical factual SDK witness binds genuine archive custody to owner/run/version/bundle/operation/attempt; Runtime eagerly restores configured custody before exposing a recovered failure. Public data-only `ProgramResult::create()` cannot bypass this with a prefilled witness, and an unresolved parsed seal is not an executable result. After process restart the original exception pointer is unavailable (`provider_cause == nullptr`), not recreated from text. A host without a `native_history_archive` cannot keep a provider's native replay state, so it publishes a failure that carries native messages without that state: the same messages, usage, stop reason, error and raw events, each message without its native seal, with `provider_outcome_native_retained: false` in the witness. The result's `provider_outcome` is that portable copy, and the original outcome stays on `provider_cause` in the process that made the call. A failure that cannot be persisted otherwise, because the archive write failed, cannot be serialized, published or replayed.

Python `LocalProgramHost` destruction releases the caller's GIL while `ProgramRuntime` cancels, drains and joins scheduler work, so active Python nodes can finish. It reacquires the GIL before destroying the remaining host members and their Python callback/object owners. This changes teardown only; it exposes no additional capability binder or execution authority.

`RecordedBindingSet` is source-bound, move-only data, never a caller-supplied dispatcher. The trusted Catalog `recorded_capability_binder` independently materializes captured-only capabilities from real persisted source events. `ProgramRuntime::replay_recorded()` checks original selected-source permissions, then transfers the actual remaining bank through durable CAS; inherited spend is not a new model grant. The old `start_recorded` renewal API is removed. InMemory, File, SQLite and PostgreSQL Program stores preserve the exact immutable owned lease throughout execution; expiry does not renew it. Controlled JavaScript still validates the underlying capability manifest and consumes exact completed command outcomes without redispatching external effects.

**Recorded replay accounting.** Captured command replay durably reserves only new CPU wall-time and Core work before execution, then publishes measured work and any newly produced Core checkpoint through the result CAS. It consumes no new model, money or Program-operation allowance and does not redispatch captured external effects. An unreconciled reservation remains debited. Await channel receive, timer wait/cancel and handoff wait initiation and release are serialized on their owning executors or strands.

```cpp
#include <neograph/types.h>

neograph::json observe_result(const sp::runtime::Result& result) {
    if (!result) throw std::invalid_argument("Missing provider outcome");
    return neograph::outcome_projection_json(*result);
}
```

### ADL Serialization

Argument-Dependent Lookup (ADL) serialization functions for nlohmann/json integration.
These allow direct use with `json j = my_tool_call;` and `my_tool_call = j.get<ToolCall>()`.

```cpp
void to_json(json& j, const ToolCall& tc);
void from_json(const json& j, ToolCall& tc);

void to_json(json& j, const ChatMessage& msg);
void from_json(const json& j, ChatMessage& msg);
```

ADL serialization preserves portable message/tool fields and their declared defaults. It does not reconstruct native SDK continuation authority from JSON.

---

## 2. Provider Interface

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

#### Typed controls

`make_provider_request(provider, model, messages, tools, controls, mode)` selects one closed family. Wrong-family controls raise `std::invalid_argument`; SDK preparation then checks enum values, admitted origin, model rules, tool declarations and native bindings before I/O. `ProviderControls` has no dictionary escape hatch. Optional values preserve absent versus explicit `false` or empty selection.

| Family | `ProviderControls` fields and SDK mapping |
|---|---|
| `openai.chat` | `max_output_tokens`, `temperature`, `top_p`, `reasoning_effort`, `service_tier`, `provider`, `response_format`; `chat_reasoning` → `sp::chat::Request::reasoning`, `include_reasoning`, `usage_include` → `usage.include`, `models` |
| `openai.responses` | `max_output_tokens`, `max_tool_calls`, `temperature`, `top_p`, `reasoning_effort`/`reasoning_summary` → `reasoning`, `service_tier`, `required_tool`, `provider`, `response_format`, `store`, `system` → `instructions`, `account_scope`; `previous_response_id`, `previous_response_history`, `parallel_tool_calls`, `verbosity` → `text.verbosity`, `truncation`, `responses_include` → `include` |
| `anthropic.messages` | `max_output_tokens` → `max_tokens`, `temperature`, `top_p`, `thinking_budget`, `system`, `account_scope`, `provider`; `thinking_mode`, `output_effort` → `output_config.effort`, `cache_control`, `messages_tool_choice` → `tool_choice` |
| `google.generate` | `max_output_tokens`, `temperature`, `thinking_budget`, `include_thoughts`, `required_tool`, `system`, `account_scope`; `gemini_history_mode` → `history_mode`, `gemini_thinking_level` → `thinking_level`, `safety_settings`, `gemini_tool_choice` → `tool_choice` |
| `google.interactions` | `max_output_tokens`, `thinking_level` (optional string), `thinking_summaries`, `service_tier`, `required_tool`, `system`, `account_scope`; Generate's enum `gemini_thinking_level` does not apply |

Chat's `sp::chat::ReasoningOptions` contains optional `effort`, `max_tokens`, `exclude` and `enabled`. Its nested reasoning object, `include_reasoning`, `usage_include` and alternative `models` require a policy-declared OpenRouter origin. `sp::OpenRouterRouting` is supported only on declared OpenRouter origins for Chat, Responses and Messages; choosing a gateway-shaped model name does not admit a different origin. SDK requests also expose typed family tool definitions, Responses `hosted_tools` and strict/deferred tool options; use the actual payload variant for those fields rather than adding raw JSON.

Responses SSE requires a terminal `response.completed` or `response.incomplete` event. `response.done` is accepted only as an alias carrying the same full terminal Responses envelope (completed or incomplete); a nonterminal or malformed `response.done` fails as a protocol error, and `response.content_part.delta` or any other unlisted event is `Unsupported`. On a declared OpenRouter origin, Responses and Messages also accept the concrete model the gateway reports for a routing alias such as `~vendor/model-latest`; the served model must stay the same for the whole response, and replay stays bound to the requested model.

| SDK type | Closed values or members |
|---|---|
| `sp::responses::Verbosity` | `Low`, `Medium`, `High` |
| `sp::responses::Truncation` | `Disabled`, `Auto` |
| `sp::responses::Include` | `ReasoningEncryptedContent`, `WebSearchSources`, `FileSearchResults`, `MessageOutputTextLogprobs`, `ComputerCallOutputImageUrl`, `CodeInterpreterCallOutputs` |
| `sp::messages::ThinkingMode` | `Manual`, `Adaptive`, `Disabled` |
| `sp::messages::OutputEffort` | `Low`, `Medium`, `High`, `Max` |
| `sp::messages::CacheControl` / `CacheTtl` | optional `ttl`: `FiveMinutes`, `OneHour`; wire type `ephemeral`, optional TTL `5m`/`1h` |
| `sp::messages::ToolChoice` / `ToolChoiceMode` | `mode`: `Auto`, `Any`, `None`, `Tool`; `name`; optional `disable_parallel_tool_use` |
| `sp::gemini::HistoryMode` | `NativeOnly`, `PortableForeign` |
| `sp::gemini::ThinkingLevel` | `Minimal`, `Low`, `Medium`, `High` |
| `sp::gemini::SafetySetting` / `SafetyCategory` | `category`: `Harassment`, `HateSpeech`, `SexuallyExplicit`, `DangerousContent`, `CivicIntegrity`; `threshold`: `SafetyThreshold` |
| `sp::gemini::SafetyThreshold` | `BlockNone`, `BlockOnlyHigh`, `BlockMediumAndAbove`, `BlockLowAndAbove`, `Off` |
| `sp::gemini::ToolChoice` / `ToolChoiceMode` | `mode`: `Auto`, `Any`, `None`, `Validated`; `allowed_function_names`: vector of declared function names |

Messages requires a positive output cap. A supplied thinking budget selects `Manual` when no mode is set; manual thinking requires at least the admitted minimum and a budget strictly below the cap. `Adaptive` and `Disabled` reject a supplied budget. Manual/adaptive thinking omits otherwise valid temperature and checks the admitted thinking `top_p` minimum; it does not override model-specific temperature prohibitions. Forced `Any`/`Tool` choice requires tools and disabled thinking; `Tool` names a declared client tool, other modes reject `name`, and `None` rejects `disable_parallel_tool_use`. Generate rejects simultaneous thinking budget/level and simultaneous `required_tool`/`gemini_tool_choice`; its allowed-function list must name declared functions. Sampling remains family/policy-bounded; Generate has no `top_p`, and Interactions accepts neither sampling field.

`FamilyPolicy.temperature_forbidden_model_prefixes` matches ASCII case-insensitively against the full model and the suffix after the last `/`. Built-in Chat/Responses prefixes are `gpt-5`, `gpt-6`, `o1`, `o3`, `o4`; Messages prefixes are `claude-opus-4-7`, `claude-opus-4-8`, `claude-opus-5`, `claude-sonnet-5`, `claude-fable-`. Explicit prohibited temperature rejects before I/O, including gateway-prefixed models.

#### Responses provider-held continuation

`previous_response_id` selects provider-held state. Request `messages` contains NEW INPUT ONLY; do not resend the prior conversation. `previous_response_history` is `std::vector<sp::Message>` local ownership evidence and is never emitted. A cursor alone can admit new text/image input but cannot authorize client tool results. For the initial captured response, pass its authentic original prefix plus terminal assistant response whose ID equals the cursor. A later authentic in-process cursor-produced terminal response can carry private completed tool ownership without reconstructing that full prefix. Content, origin, model, route and configuration mismatches reject; an arbitrary ID or projected JSON cannot supply that ownership.

Server-held cursors and private completed ownership do not grant `NativeReplay` or native-archive authority. Absent `responses_include` preserves `reasoning.encrypted_content`; an explicit empty vector sends `[]`, and other explicit selections are respected. Missing native evidence still fails honestly when a subsequent operation requires it.

#### Explicit portable Gemini history

Generate defaults to `NativeOnly`. Explicit `PortableForeign` admits caller-created assistant `Text`/`ToolCall` parts only when they carry no native seal, `wire_output` or signatures/native metadata. Only the first foreign `functionCall` receives Google's `skip_thought_signature_validator`; foreign text-only turns receive no signature. Authentic native groups still require their original provenance, content and binding. A corrupt/mismatched native group is never demoted to portable input, and imported foreign history never acquires native replay authority.

#### Output caps and native continuation

The output generation cap is a per-call admitted resource, excluded only from the native replay configuration digest. Every other native binding remains, including content/prefix, origin/route, policy identity, tools and reasoning controls (apart from documented per-turn tool selection/cursor behavior). Each encoded/prepared request still retains its effective cap; request digests, journal slots, original shared bank and absolute deadline remain binding. A larger-cap semantic call needs a fresh call ordinal and fresh admission under the same grants, not a repaired seal, renewed deadline or replay of the old effect. Native archives remain `spna3`/v3 and portable JSON remains v2.

Python exposes `ChatReasoningOptions`, `ResponsesVerbosity`, `ResponsesTruncation`, `ResponsesInclude`, `MessagesThinkingMode`, `MessagesOutputEffort`, `MessagesCacheControl`, `MessagesCacheTtl`, `MessagesToolChoice`, `MessagesToolChoiceMode`, `GeminiHistoryMode`, `GeminiThinkingLevel`, `GeminiSafetySetting`, `GeminiSafetyCategory`, `GeminiSafetyThreshold`, `GeminiToolChoice`, `GeminiToolChoiceMode` with the same enum values except Python `None_` for C++ `None`. Optional class-valued controls and message/safety/history vectors return detached snapshots; reassign edits. `ProviderControls.previous_response_history` is a list of authentic `ProviderMessage` values, not a single provisional response.

#### Deployment header preprocessing

Plain `sp::descriptor::load(source[, policy])` treats header values literally, including `${VAR}`. Explicit `load_with_environment_headers(source, overrides = {}, policy = {})` reads optional `ANTHROPIC_WORKSPACE_ID`/`ANTHROPIC_BETA` for Messages; unset/empty values are omitted. Deterministic `load_with_deployment_headers(source, overrides, DeploymentHeaderEnvironment, policy)` uses the supplied optional `anthropic_workspace_id`/`anthropic_beta` instead. Both return `LoadResult` and preprocess before descriptor admission. Case-insensitive precedence is environment < descriptor literal headers < explicit overrides. Duplicate overrides, invalid/reserved names and line breaks reject during admission; no environment evaluation or header mutation occurs afterward.

Python names are `ProviderDeploymentHeaderEnvironment`, `load_provider_descriptor_with_environment_headers(source, overrides=[], policy=None)` and `load_provider_descriptor_with_deployment_headers(source, overrides, environment, policy=None)`; the loaders return `ValidatedDescriptor` or raise on failed admission. Credentials remain in runtime options.

#### Additional admission and event rules

Chat nested reasoning must be nonempty and cannot coexist with effective scalar `reasoning_effort`. Its `effort` and `max_tokens` are mutually exclusive; a budget must be positive, fit signed 64-bit wire range and not exceed the effective output cap. `enabled=false` forbids effort/budget, and `exclude=true` conflicts with `include_reasoning=true`. Alternative models must have unique nonempty names within the admitted count limit; each model is checked against the effective choices, including temperature prohibitions.

Policy identity remains part of native binding: the published built-in policy revision 4 rejects old policy-3 seals/archives rather than repairing them. Generate's `safety_settings` requires unique valid categories; a nonempty allowed-function list is valid only with `Any`/`Validated`. Its model must be a literal name matching both admitted Generate descriptor paths. Portable tool results must match the admitted call identity; foreign assistant calls must be client-executed and have no `wire_type` or `wire_metadata`.

At the semantic `Stop` boundary, valid or invalid client-call intent changes `EndTurn` to `ToolUse`; observer events and final outcomes agree. Specific `MaxTokens`, `ContentFilter` and `Unknown` evidence remains unchanged, and completed server-executed hosted tools do not by themselves imply client `ToolUse`. Streaming OpenRouter reasoning fragments merge by index in first-arrival order; encrypted blobs remain discrete. Owned raw frames and native continuation retain their original content and tamper checks.

### PreparedProviderRequest / ProviderBudgetClaim
`prepare()` validates and encodes exactly once, producing a move-only `PreparedProviderRequest` with the original deadline and cancellation state. Durable callers bind its `Provider::request_digest()` to their assembly, reserve an admitted budget claim, write the dispatch receipt, then consume that same handle through `ControlledProvider::dispatch_prepared(_async)`. They never rebuild a request after the gate. Duplicate receipts never redispatch. Custom providers implement `get_name()`, `family()` and `prepare()` using `prepare_runtime()` or `prepare_local()`; local callbacks capture owned shared state, not `this`.

Optional `ProviderControls` are caller choices, not mandatory defaults or silently clamped caps. Unsupported family controls fail before dispatch. Bounded calls require genuine admitted model input/output facts; missing facts fail with `LimitUnknown`. A reservation is conservative spending authority, not reported usage, a forecast or an invoice. Unknown/partial/delivery-unknown outcomes retain their hold; genuine final reports settle it, including oversized usage. A usage record with stage `Missing` is no evidence (not zero), and `Partial` is what was seen so far; neither settles a call, and the hold stays. A count the SDK derives (`Evidence::Derived`) settles like a reported one: Gemini's `output_total` and `total`, derived when the output budget is spent while thinking and `candidatesTokenCount` is omitted, are enough to charge the call what it used. Retry is one explicit layer, off by default, with a bounded window and unknown-prior hold; no hidden resend.

`provider_failure_proves_not_accepted(Failure)` is the one rule every settlement path uses to decide whether a failed call releases its hold or keeps it: the token reservation of the bank, of the managed journal and of the MCP harness provider, the Program's monetary hold, the SQLite provider journal and the controlled provider's dispatch receipt (`Failed` or `ReconciliationRequired`). It requires complete, contradiction-free evidence of one of two kinds:

- `NotSent`: the SDK proves that no request byte left (`provider_failure_proves_not_sent(Failure)` is this half alone).
- `RejectedBeforeOutput` on a completed HTTP 4xx answer other than 408, 425 and 429, such as a bad key (401), a malformed request (400) or a pinned `max_price` nobody meets (404). The server declined the request, so nothing ran and nothing is billed. The failure must also carry no output, no stop reason and no usage, and no earlier attempt that could have been billed.

An HTTP 408, 425, 429 or 5xx answer, a cut or timed-out call, a transport failure after the request left, and any failure with partial output or usage keep the hold. Missing usage, an observer or persistence exception, or a status code without the SDK's `RejectedBeforeOutput` label never proves zero cost or renews a budget.

```cpp
#include <neograph/controlled_provider.h>

sp::runtime::Result dispatch_admitted(
    neograph::ControlledProvider& gateway, std::string owner_scope,
    std::string dispatch_id, const neograph::ContextAssemblyReceipt& assembly,
    neograph::PreparedProviderRequest prepared,
    neograph::ProviderDispatchBudget budget) {
    auto claim = neograph::reserve_provider_dispatch(prepared, std::move(budget));
    return gateway.dispatch_prepared(
        std::move(owner_scope), std::move(dispatch_id), assembly,
        std::move(prepared), std::move(claim));
}
```


This is a source and binary break: recompile every C++ consumer and custom provider with matching new headers/libraries. `CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, the descriptor interpreter and Responses WebSocket path are removed, with no aliases or compatibility bridges. The SDK is alpha `0.3.3`, interface revision 6 / shared-library generation 6, with out-of-line capability checks; that is not a stable release claim. Linux/POSIX is the tested platform. Windows NTFS and macOS implementations are present but not tested with this SDK, and WASM provider runtime support is not established.

Python exposes the same owned request/outcome boundary as C++: `make_provider_request`, `Provider.prepare`, `dispatch` and `invoke`. Use `ProviderMessage` with typed parts for provider history; `ChatMessage` remains a graph convenience projection. A returned SDK failure is available through `ProviderOutcome.failure`, while host observer/settlement exceptions retain `outcome` and `cause`. See the [Python binding guide](python-binding.md) for constructors and GIL/callback behavior.

Python SDK vector/map getters return detached values: `ProviderMessage.parts`, `ProviderRequest.messages`, `RunConfig.provider_messages`, completion/partial `messages` and `raw_events`, `RunResult.native_messages`, `ProviderLoopEntry.messages`, and usage `extra`/`conflicts`. Modify a snapshot, then assign it back where a setter exists; appending to a getter result does not update its owner. Optional `ProviderControls.provider`/`response_format`, `SchemaProviderDefaults.provider`, `OpenRouterRouting.max_price` and `ProviderToolResult.host` follow the same read-modify-assign rule. Read-only outcome evidence remains unchanged. These are Python binding rules, not a blanket copy guarantee for C++ getters.

When native code calls a Python provider's `prepare` override, returning `None` raises `TypeError` before handle consumption. `ProviderDescriptorPolicy.identity` is `bytes` containing the raw SHA-256 digest; `.hex()` is a display conversion. Repeated inspection of stored Python provider/graph causes preserves the original exception value and traceback without consuming the stored exception's restore state, including nested native translation.

### Pricing evidence and the price catalog

A Program run's `monetary_microunits` budget counts micro-USD (one microunit is 1e-6 USD). A call is priced when the host routes the built-in `llm_call` node's provider calls through a pricing broker: `bind_priced_provider_call(context)` in process, or `SQLiteProgramProviderCallJournal::bind_priced(context, deployment)`, which adds crash-safe replay. The runtime hands each broker the attempt's monetary account in `ProgramCoreProviderCallContext::monetary_account`. The Python `LocalProgramHost(node_context=..., binding_label=...)` always prices its calls ([Python guide](python-binding.md#model-calling-nodes)). Like every `RunBudget` field, `monetary_microunits` is an amount granted to the run, and `0` grants nothing: child and fork budgets are carved out of their parent's amounts, and a completed run that used model tokens against `model_tokens = 0` is reported `BudgetExhausted`. A run with `monetary_microunits = 0` refuses every priced call before sending it. The pricing code is internal (`src/program/pricing.h`, `monetary.h`) and carries no ABI promise.

Before dispatch, the broker prices the prepared request. The origin is the catalog origin whose URL matches the descriptor's base URL, or `openrouter` when the SDK policy declares that base URL an OpenRouter origin. The hold is the catalog reservation for the request's input bound and output cap, times the model invocations one dispatch can bill (`Provider::invocation_upper_bound`). The input bound is the smallest input limit known for the model: the SDK policy's, the catalog's `max_input_tokens` or its context window. Every input token is priced at the dearest input band and every output token at the dearest output rate, on an aggregator at the route ceiling. A call that cannot be bounded, or whose hold does not fit the remaining budget, is refused before any request or journal marker exists. The node fails with a synthetic not-sent provider failure, and the run's `ProgramFailure` names the reason; `failure.witness["monetary"]` holds the origin, model, bounds and catalog:

| Code | Status | Refused because |
|---|---|---|
| `P_MONEY_UNKNOWN_ORIGIN` | `Failed` | the provider endpoint is not a catalog origin |
| `P_MONEY_UNKNOWN_MODEL` | `Failed` | the catalog has no entry for the origin and model |
| `P_MONEY_UNMODELED` | `Failed` | the request uses what the catalog does not model: a service tier other than standard, OpenRouter model fallbacks (`models`), Responses hosted tools or an Anthropic server tool |
| `P_MONEY_NO_OUTPUT_CAP` | `Failed` | the request has no output token cap |
| `P_MONEY_NO_INPUT_BOUND` | `Failed` | no input limit is known for the model |
| `P_MONEY_NO_UPPER_BOUND` | `Failed` | an aggregator entry has no route ceiling, such as a router alias, or its ceiling cannot be pinned as `provider.max_price` |
| `P_MONEY_BUDGET` | `BudgetExhausted` | the hold does not fit the remaining `monetary_microunits` (a run whose charges end over the budget has the same code, with `failure.witness["monetary"]` holding `granted`, `used` and `over_by`) |

After the outcome, the hold is settled exactly once, and the attempt's `ProgramResult::monetary_records()` (Python `result.monetary_records`) gets one `ProgramMonetaryRecord` per call: `call_id` (the journal's logical call slot), `operation_id`, `node`, `origin`, `model`, `evidence`, `microunits`, `reserved_microunits`, `basis`, `catalog_version`, `catalog_digest`, `conservative`, `may_undercount`, `replayed` and `reason`. `ProgramUsage::monetary_microunits` is the sum of the attempt's charges and of holds still open:

| Evidence | When | Amount |
|---|---|---|
| `reported` | The response carries the provider's own cost: OpenRouter `usage.cost`, which the SDK parses into `Usage::provider_cost` in nano-USD. With BYOK the reported upstream charge is added. | The reported amount, rounded up to whole microunits. |
| `estimated` | Final, consistent usage without a reported cost. | Catalog rates times the reported token bands, rounded up once per call. The record names the catalog version and digest. |
| `held` | The outcome is unknown (a transport exception, cancellation, a timeout), the call failed in a way that can follow started work (an HTTP 408, 425, 429 or 5xx answer, a cut stream, a transport failure after the request left), the SDK resent transport attempts, or the usage is partial or inconsistent. | The hold. |

A failure that proves the provider accepted nothing releases the hold and leaves no record: no request was sent, or the provider answered with a completed HTTP 4xx other than 408, 425 and 429, which declines the request. Neither the money hold nor the model-token reservation is charged. A charge above the hold, possible when the reported cost includes what the pin does not cap, such as a BYOK upstream charge, is charged as reported, and a run whose charges exceed its budget ends `BudgetExhausted` with `P_MONEY_BUDGET`. Concurrent calls (`ng.all`, `ng.race`, `ng.quorum`, parallel nodes) reserve against one account, so a member that does not fit is refused. A node retry calls under its own effect namespace (`task#a<k>`), so each attempt's call is a new call and is charged again.

`bind_priced` writes the call's record into its journal row: the held record with the dispatched marker, then the settled record with the outcome. A call the same Program attempt journaled before its process stopped is charged once, marked `replayed`, when the recovered attempt binds the operation, whether or not the Core calls it again; this includes a define()-only run that recovers by letting its execution lease expire. A call an earlier attempt journaled was settled into the remaining budget by that attempt's result and is never charged again when a later attempt replays its outcome. Recorded replay never spends money.

A fork replays the provider calls of the paused invocation only from a durable provider-call journal (`SQLiteProgramProviderCallJournal::bind` or `bind_priced`). The in-process broker keeps none, so a node that runs again in the fork sends and charges its earlier calls again. A host whose `core_provider_call_resolver` returns the journal's brokers sets `RuntimeConfig::core_provider_calls_journaled = true`; the Python `LocalProgramHost` does so when it has a `sqlite_directory`. When the flag is `false` and the paused attempt recorded priced provider calls in the paused operation (the source's paused `ProgramResult::monetary_records()`), `ProgramRuntime::fork` gives the handle it returns one `ProgramNotice`, read with `ProgramHandle::notices()` (Python `handle.notices`). Its `code` is `P_FORK_PROVIDER_CALLS_NOT_JOURNALED`, and its `witness` is `{source_run_id, source_attempt, operation_id, calls: [{call_id, node, model, microunits}], microunits}`. The notice is not an error: it changes neither what the fork sends nor what it charges. It cannot tell which listed calls the fork runs again, because a call of a node that finished in the paused step is replayed from the step's saved writes. It is not stored with the run, so a handle from `reconnect()`, `resume()` or a repeated `fork()` of the same run has none. The flag decides only whether the notice is given; it does not make a broker journal. A host that does not route its provider calls through a pricing broker records no priced call and gets no notice.

On an OpenRouter origin the broker pins the route ceiling on the request before it prepares the request to be sent. OpenRouter's `provider.max_price` gets the ceiling's dearest input rate as `prompt` and its dearest output rate as `completion`, in exact decimal USD per million tokens; for `deepseek/deepseek-v4-flash-0731` the body carries `"provider":{"max_price":{"prompt":0.44,"completion":1.32}}`. OpenRouter then routes only to endpoints listed at or below those prices and fails the request when there is none, so the reservation bounds the endpoint's token prices. A `max_price` the host already sends through `ProviderControls::provider` or `SchemaProvider::Defaults::provider` (a request's own replaces the default) is kept in each field where it is lower, and the amounts the catalog does not price (`image`, `audio`, `request`) stay the host's. Chat Completions, Responses and Messages requests are pinned; other origins get no `max_price`. The call is priced from a probe of the request whose conversation is one user message, which is never sent, and the request is prepared once, pinned, so the provider journal records the digest of the request that was sent and a replay matches it. A later turn whose native history was captured under the pin carries the same pin. A route ceiling that cannot be pinned, because the SDK policy admits no OpenRouter routing at that base URL or a rate exceeds the 2^53 nanoUSD that `max_price` admits, refuses the call with `P_MONEY_NO_UPPER_BOUND`.

The catalog, [`data/pricing/catalog.json`](../data/pricing/catalog.json), is compiled into `neograph::program` at build time; the runtime prices calls only from those bytes. It is keyed by origin and model. Gemini through Google AI Studio and through the Vertex AI global endpoint are separate entries, as are OpenAI, Anthropic and OpenRouter. Each entry records a `source_url` (the official pricing page, or the OpenRouter models API) and the `retrieved` date. Rates are decimal strings in USD per million tokens for input, output, cache reads, cache writes (Anthropic's 5-minute and 1-hour TTLs) and image output, with optional prompt-length tiers. Entries price the standard service tier; batch, flex and priority tiers, Anthropic's US-only inference_geo and fast mode, server-tool fees and per-request fees are not modeled. OpenRouter routes one model to endpoints with different prices, so its entries add a `route_ceiling`, the dearest endpoint when the entry was written, and reservations use it. A router alias such as `~deepseek/deepseek-v4-flash-latest` lists no endpoints, so it has no ceiling and cannot be reserved. A model without an entry has no price, and a priced call to it is refused.

NeoGraph never fetches prices at run time. [`scripts/check_price_drift.py`](../scripts/check_price_drift.py) compares the catalog with OpenRouter's models and endpoints listings, with LiteLLM's community price file (a signal to check, not the truth), and with Vertex AI SKUs from the Cloud Billing Catalog API when `GCP_BILLING_API_KEY` is set. It prints a table (`--json` prints JSON), exits 1 on drift beyond `--tolerance`, and never edits the catalog. The weekly `Price drift` workflow runs it; OpenRouter and LiteLLM need no secret.

Organization cost reports (OpenAI's organization costs endpoint, Anthropic's cost report) need admin keys, aggregate by time bucket and arrive late, so the runtime does not use them. A host can reconcile its `reported` and `estimated` records against them afterwards.

---

## 3. Tool Interface

**Header:** `<neograph/tool.h>`
**Namespace:** `neograph`

Abstract interface for tools that LLMs can call. Implement this to expose functions
to the agent.

> **Writing a custom Tool subclass?** See
> [`ASYNC_GUIDE.md` §9.6](ASYNC_GUIDE.md#96-tool-vs-asynctool) for
> when to inherit `Tool` (sync) vs `AsyncTool` (async). The two are
> mutually exclusive — pick one.

### Tool

```cpp
class Tool {
public:
    virtual ~Tool() = default;

    // Returns the tool's definition (name, description, parameter schema)
    virtual ChatTool get_definition() const = 0;

    // Executes the tool with the given arguments, returns result as string
    virtual std::string execute(const json& arguments) = 0;

    // Returns the tool's unique name
    virtual std::string get_name() const = 0;
};
```

| Method | Returns | Description |
|--------|---------|-------------|
| `get_definition()` | `ChatTool` | Returns the tool's metadata including JSON Schema for parameters |
| `execute(arguments)` | `std::string` | Runs the tool with parsed JSON arguments. Returns the result as a string that will be sent back to the LLM |
| `get_name()` | `std::string` | Unique identifier for this tool |

**Example implementation:**

```cpp
class WeatherTool : public neograph::Tool {
public:
    ChatTool get_definition() const override {
        return {"get_weather", "Get current weather for a city", json::parse(R"({
            "type": "object",
            "properties": {
                "city": {"type": "string", "description": "City name"}
            },
            "required": ["city"]
        })")};
    }

    std::string execute(const json& args) override {
        std::string city = args.at("city");
        return "Weather in " + city + ": 22C, sunny";
    }

    std::string get_name() const override { return "get_weather"; }
};
```

### Tool failure contract

Every path that runs a model's tool calls applies the same rules: `dispatch_tool_calls` for one call or a batch, the `tool_dispatch` node (Program's Core `tool_dispatch` included), `llm::Agent::run` and `run_stream`, the plan-and-execute executor and the deep-research researcher, for C++, MCP and Python tools alike.

| Failure | Outcome |
|---------|---------|
| `HookBoundaryBlocked`: a mandatory Hook failed, timed out or could not run, or tool code raised one | The run stops with that exception, its invocation id and state intact: a graph run throws it as `llm::Agent` does, not wrapped in a `NodeExecutionError`. A node retry policy does not retry it. |
| The run's `CancelToken` is cancelled (a node attempt's token included) | The run stops with `graph::CancelledException`. An MCP call or a `GrpcRemoteTool` RPC in flight stops at once; its late response is dropped. |
| `NodeInterrupt` from a `ToolGate` or from tool code | The run pauses; `llm::Agent` throws it to its caller. |
| Unknown tool, or a gate's `Deny` | A tool result with status `rejected`. |
| The call would start at or after the run's deadline (`RunContext::deadline`, `ToolExecutionContext::deadline`) | A tool result with status `expired` and error `run deadline elapsed before the tool started`. Neither the tool nor its Hooks run. |
| A request the tool's service refused before running it: an MCP JSON-RPC error `-32700`, `-32600`, `-32601` or `-32602`, an MCP `isError` result marked `NG.InvalidRequest`, a gRPC `ToolService` status `NOT_FOUND` or `INVALID_ARGUMENT`, or a `ToolRequestRejected` thrown by tool code | A tool result with status `failed`, `retryable` false and `effect_uncertain` false, whatever the execution policy says: nothing ran. |
| Any other exception from tool code or its lookup (C++, Python, any other MCP `isError` result or JSON-RPC error, such as `-32603`), unparseable arguments, or an output over the policy's `output_limit_bytes` | A tool result with status `failed`, or `reconciliation_required` when the call had started and its execution policy does not rule out an effect (it is neither `ReadOnly` nor `ExternalRead` nor `Idempotent`). |
| A typed controller status such as `timed_out`, `expired`, `killed` or `input_required` | A tool result with that status. An MCP call or `GrpcRemoteTool` RPC stopped by the run's deadline or by its request timeout is `timed_out`. |
| A tool result with an uncertain effect, under a `ToolEffectBroker` | The run pauses with a `NodeInterrupt` whose payload `kind` is `tool_effect_reconciliation`. |

The first three rows never reach the model. A tool result sets `tool_status`, and a failed one carries `{"error", "status", "retryable", "effect_uncertain"}`, in one call or a batch. In a batch the other calls finish first; when several calls stop the run, cancellation wins over a Hook block and a Hook block over an interrupt. A `NodeInterrupt` raised by tool code lets its siblings run, and resume runs the whole batch again; to ask before anything runs, use a `ToolGate`. The run deadline (`RunMetadata::deadline`) bounds Hooks, provider calls and the start of every tool call, including its wait in the admission queue; a tool's own limits are its policy's `execution_timeout` and `queue_timeout`. A system error that tool code raises after it started is the tool's failure (`failed` or `reconciliation_required`), not an admission verdict; host admission refuses with `rejected` (a full queue, no capacity) or `expired` (its queue wait ran out), like the resource arbiter. A call that never started (`cancelled_before_start`, `expired`, `rejected`: `tool_call_not_started()`) performed no effect: every `ToolEffectBroker`, the durable `SQLiteToolEffectBroker` included, returns it with that status, terminal and not effect-uncertain, and frees its slot, so it neither pauses the run for reconciliation nor blocks a later call of the same slot.

**A refused request is a known outcome.** `ToolRequestRejected` (`<neograph/tool_execution.h>`) is what a tool throws when it knows its call did not run; an exception type that has a base of its own, such as `MCPError`, implements the `ToolRequestRefusal` interface instead. `retryable()` says whether sending the same request again may succeed. `MCPTool` throws an `MCPError` that is also a refusal for the JSON-RPC errors that reject a request before the server runs it (`-32700`, `-32600`, `-32601`, `-32602`), so a caller that catches `MCPError` still gets the code and data. For an `isError` result whose `_meta["io.neograph/failure"].code` is `NG.InvalidRequest`, which is how `MCPServer` reports arguments that fail a tool's input schema, it throws `ToolRequestRejected`. `GrpcRemoteTool` throws `ToolRequestRejected` for the statuses `NOT_FOUND` and `INVALID_ARGUMENT`. The controller turns a refusal into `failed` with `retryable` false, because the same request is refused again and the model has to change it, and `effect_uncertain` false. Every other failure leaves the outcome unknown (the server may have run the tool and then failed: an ordinary `isError` result, `-32603`, a server error such as `-32000`), so it stays `reconciliation_required` unless the tool's policy rules out an effect. The node failure classifier reads a refusal as an ordinary failure that is not retried when `retryable()` is false.

**Tool Hooks.** Every path emits one `BeforeToolExecution` event before a call starts and one `AfterToolExecution` event after it settles, with the same data (`tool_call_id`, `tool_name`, `thread_id`, `request_id`, and `arguments` before or `status` and `result` after), under the run's owner scope and run id. The `HookRuntime` is the `ToolExecutionContext`'s for direct dispatch; `llm::Agent::set_hook_runtime`, or the context passed to `run`, for the Agent; and for a graph run the engine's (`EngineConfig::hook_runtime`, `GraphEngine::set_hook_runtime`) unless `RunResources::hook_runtime` replaces it for that run. A subgraph's tool calls go through both its parent run's `HookRuntime` and its own engine's when both have one, the parent's Hooks first (`HookRuntime::compose`), and a block from either blocks the call; a Program Core's tool calls use the Program host's (`RuntimeConfig::hook_runtime`). When the call has a task slot (`ToolExecutionContext::effect_task_id`, set in every graph run), its events also carry `task_id` and `call_ordinal`: the call's durable identity, the same as its effect identity (the run, the thread, the task with its node attempt, such as `s3:act#a1`, and the call's ordinal in its batch), from which `request_id` is then derived. An event already delivered for that identity is not delivered again when a resume or replay runs the batch again (`HookRuntime::emit_once_async`, on the Hook journal's own records); an emission whose mandatory Hook did not pass (it failed, timed out or was cancelled, which stopped the run) was no delivery, so the next one runs the Hooks again. A denied, unknown or expired call starts nothing and emits no Hook. A mandatory Hook whose last attempt failed reports `failed`; it reports `timed_out` only when its deadline elapsed or its target timed out. A Hook target that timed out is final: neither the runner nor the tool execution controller starts it again, even when Hook attempts or the target policy's `retry_max_attempts` remain, because the timed-out target may still be running, and a second attempt could run beside it. A Hook boundary that cannot run at all (no owner scope to attribute the event to, an unusable journal) fails closed with state `failed`, for tool, checkpoint, provider and Program events alike. When the run is cancelled while a Hook runs, the run stops as cancelled.

**Text.** Text that crosses a boundary is well-formed UTF-8: each maximal ill-formed subsequence becomes U+FFFD, the Unicode and WHATWG substitution practice. The rule covers tool results and failure messages, Hook receipts, `json::dump` (a document holding an ill-formed string used to serialize as an empty string), MCP and gRPC responses, and the Python binding. There, text read from C++ decodes the same way, and a `str` turns into UTF-8 with its surrogate-escaped bytes (U+DC80 to U+DCFF, as `errors="surrogateescape"` produces) turned back into the bytes they stand for, and any other lone surrogate turned into U+FFFD.

**The throwing facade.** `ToolExecutionController::execute_async` returns the output on success, throws `graph::CancelledException` for `cancelled_before_start` and `cancellation_requested`, and throws `ToolExecutionError`, which carries the whole `ToolExecutionResult`, for every other status. The node failure classifier reads it: an uncertain effect is `NG.EffectUncertain`, a call that timed out or expired is `NG.Timeout`, anything else is ordinary.

### Protocol failure mapping

When NeoGraph serves tools, graphs or agents over MCP, gRPC, A2A or ACP, every failure class keeps a distinct, documented form on the wire, named by the stable code the node failure classifier gives (`graph::classify_failure`). Run control signals never become a tool result a model could read. Only the public message crosses the wire: the failure's `public_message` (from `NodeFailureError`), or a fixed description of its class, never the exception text, which can carry paths, payloads or secrets.

| Class (code) | MCP `tools/call` (`MCPServer`) | gRPC `GraphService` | gRPC `ToolService.CallTool` | A2A task (`A2AServer`) | ACP `session/prompt` (`ACPServer`) |
|------|------|------|------|------|------|
| Cancelled by the client (`NG.Cancelled`): `notifications/cancelled`, a gRPC cancel | No response on any transport: stdio (`run`) sends nothing; HTTP (`MCPHttpServer`) ends the POST with an empty `text/event-stream`; a transport that reads the response sink gets the id through its `NoResponseSink` instead | `CANCELLED` | `CANCELLED` | `CancelTask`: `CANCELED`; the stream closes on it | `session/cancel`: `stopReason: "cancelled"` |
| Cancelled by the server (`NG.Cancelled`): `stop()`, or the handler's own work | JSON-RPC error `-32000` (`stop()`) or `-32603`, `data.code` | `CANCELLED` | `CANCELLED` | `CANCELED` | JSON-RPC error `-32800`, `data` `{code, retryable}` |
| Hook blocked (`NG.HookBlocked`) | JSON-RPC error `-32010`, `data` `{code, invocation_id, state}` | `FAILED_PRECONDITION`, or `DEADLINE_EXCEEDED` when the Hook timed out; ErrorInfo metadata `invocation_id`, `state` | as `GraphService` | `REJECTED` when no node had finished, otherwise `FAILED`; `metadata["neograph/failure"]` `{code, retryable}` | JSON-RPC error `-32603`, `data` `{code, retryable}` |
| Interrupted (`NG.Interrupted`) | JSON-RPC error `-32011`, `data` `{code, reason, value}` | a paused run: `OK`, `interrupted`, `interrupt_node`, `interrupt_value_json` (the stream's FINAL payload: `interrupt_value`) | `FAILED_PRECONDITION`, ErrorInfo metadata `reason`, `value` | `INPUT_REQUIRED`: the reason as text, the payload as a data part, `metadata["neograph/interrupt"]` `{node, checkpointId}`; a follow-up message resumes the run | `session/request_permission` in the same turn, whose answer resumes the run; JSON-RPC error `-32603` with `NG.Interrupted` when the adapter or the client cannot answer |
| Ordinary (`NG.TaskFailed`, an author code) | `result` with `isError: true` and `_meta["io.neograph/failure"]` `{code, retryable}` | `UNKNOWN` | `OK` with `error` and `error_code` | `FAILED`, the public message as status message, `metadata["neograph/failure"]` `{code, retryable}` | JSON-RPC error `-32603`, the public message as message, `data` `{code, retryable}` |
| Timeout (`NG.Timeout`) | as ordinary, with its code | `DEADLINE_EXCEEDED` | as ordinary, with its code | as ordinary, with its code | as ordinary, with its code |
| Provider failure (`NG.ProviderFailed`) | as ordinary, with its code | `UNAVAILABLE` | as ordinary, with its code | as ordinary, with its code | as ordinary, with its code |
| Budget (`NG.BudgetExhausted`) | as ordinary, with its code | `RESOURCE_EXHAUSTED` | as ordinary, with its code | as ordinary, with its code | `stopReason: "max_tokens"` when the run's model-token budget was crossed, otherwise as ordinary |
| Safety or configuration (`NG.SafetyBlocked`, `NG.ConfigError`) | as ordinary, with its code | `FAILED_PRECONDITION` | as ordinary, with its code | as ordinary, with its code | `stopReason: "refusal"` for `NG.SafetyBlocked`; `NG.ConfigError` as ordinary |
| Logic (`NG.LogicError`, `NG.BarrierStalled`) | as ordinary, with its code | `INTERNAL` | as ordinary, with its code | as ordinary, with its code | as ordinary, with its code |
| Effect committed (`NG.EffectCommitted`) | as ordinary, with its code | `ABORTED` | as ordinary, with its code | as ordinary, with its code | as ordinary, with its code |
| Effect uncertain, unknown, a failure exit (`NG.EffectUncertain`, `NG.UnknownFailure`, the exit's code) | as ordinary, with its code | `UNKNOWN` | as ordinary, with its code | as ordinary, with its code | as ordinary, with its code |
| Step limit (`NG.StepLimit`): `RunConfig::max_steps` | none | `OK`, `max_steps_exhausted` | none | `FAILED`, code `NG.StepLimit` | `stopReason: "max_turn_requests"` |
| Invalid request (`NG.InvalidRequest`) | JSON-RPC error `-32602`; for arguments that fail the tool's input schema, a `result` with `isError: true` and `_meta["io.neograph/failure"]` `{code: "NG.InvalidRequest", retryable: false}`, which tells a NeoGraph client that nothing ran | `INVALID_ARGUMENT`; `FAILED_PRECONDITION` (`NG.NoGraph`) when there is no graph to run | `INVALID_ARGUMENT` | JSON-RPC error `-32602` with `google.rpc.BadRequest` field violations; a malformed envelope or a batch: `-32600` | JSON-RPC error `-32602` |
| Unknown tool (`NG.ToolNotFound`) | JSON-RPC error `-32602`, `data.code` | none | `NOT_FOUND` | none | none |
| At capacity (`NG.Capacity`) | JSON-RPC error `-32000`, `data.capacity` | `RESOURCE_EXHAUSTED` | none | JSON-RPC error `-32603`, ErrorInfo reason `RESOURCE_EXHAUSTED`, domain `neograph` | JSON-RPC error `-32603`, `data.reason` `overloaded` |

`retryable` is `FailureVerdict::retryable` on every protocol: whether sending the same request again may succeed. It is the node's retry verdict ([Node failure contract](#node-failure-contract)), and also `true` for a provider failure (`NG.ProviderFailed`), which the node retry loop leaves to the provider runtime but a new request may get past. A2A carries the code and `retryable` in the task's `metadata["neograph/failure"]` in both versions, and ACP in the error's `data`; the request errors only A2A has are under [A2A error mapping](#a2a-error-mapping).

Every non-OK gRPC status carries a `google.rpc.Status` in the `grpc-status-details-bin` trailer with one `google.rpc.ErrorInfo`. Its `reason` follows AIP-193: an UPPER_SNAKE constant of at most 63 characters derived from the code (`HOOK_BLOCKED` for `NG.HookBlocked`, `TIMEOUT`, `TOOL_NOT_FOUND`, `INVALID_REQUEST`, `NO_GRAPH`, `CAPACITY`, `BACKEND_UNAVAILABLE`, `BARRIER_STALLED`; an author code gives its class's reason, such as `TASK_FAILED`). `domain` is `neograph`, and `metadata` holds `code` (the stable code itself), `retryable` and the fields above. `RunGraphResponse.error` is deprecated and kept for one release with the public message only; a non-OK unary status carries no response body. `RunGraphStream` reports a failure as a `DEBUG` event whose payload is `{error, code}` and then ends with the same status as a unary call. The `CheckpointService` answers a backend failure with `UNAVAILABLE` (`NG.BackendUnavailable`) instead of "no checkpoint", an empty list or a successful delete, and a malformed `SaveCheckpoint` with `INVALID_ARGUMENT`.

`RunGraphStream` follows `RunGraphRequest.stream_mode`, the bits of `StreamMode` (0 streams every event; unknown bits are ignored). A state snapshot is a `VALUES` event, a node's channel write an `UPDATES` event, and an interrupt or a node error a `DEBUG` event; a node error's `error` is `node failed`, never the exception text. `RunGraphRequest.thread_id` is required: an empty one is refused with `INVALID_ARGUMENT`. A `CheckpointService` written in another language must serve every RPC of `proto/neograph.proto`, `RequiresManagedBudget` included: the engine asks it for every new or resumed run and fails the run on `UNIMPLEMENTED`, and a server that manages no budget answers `required = false`. NeoGraph's `CheckpointService` stores a `Checkpoint`, not the JSON, so it refuses with `INVALID_ARGUMENT` a `checkpoint_json` it could not keep whole (a top-level member it does not know, or a `schema_version` newer than its own) instead of dropping part of it; `channel_values` and `metadata` are kept as sent.

`run_server`, `run_tool_server` and `run_checkpoint_server` answer the standard health service, `grpc.health.v1.Health`: `SERVING` for the server and for its service while it takes calls, `NOT_SERVING` once it stops. Their overloads that take `ServeOptions` (`<neograph/grpc/serve_options.h>`) stop when `ServeOptions::stop` is requested, after `shutdown_grace` for the calls in flight, and report the bound port to `on_serving` (so the address may name port 0). Server reflection is the host's choice: link `gRPC::grpc++_reflection` and call `grpc::reflection::InitProtoReflectionServerBuilderPlugin()` before the servers start, and gRPC adds it to every server of the process, the built-in ones included. For TLS, authentication or message size limits, register `make_graph_service()`, `make_tool_service()` or `make_checkpoint_service()` with your own `grpc::ServerBuilder`. The gRPC headers include gRPC's own, so code with `using namespace neograph;` writes `neograph::grpc::GrpcRemoteTool`: a bare `grpc::` is ambiguous there.

NeoGraph's own clients bound their calls: the MCP client sends `notifications/cancelled` for a request it stops waiting for, cancelled or timed out (never for `initialize`), and drops the late response; `GrpcRemoteTool` (`set_call_timeout`, 60 s by default) and `GrpcCheckpointStore` (`set_rpc_timeout`, 30 s by default) set a deadline on every call, and the run's cancel token cancels the remote tool's RPC. They do not turn a remote code back into a local control signal. A remote Hook block (MCP `-32010`; a gRPC status whose `ErrorInfo` code is `NG.HookBlocked`) and a remote interrupt (`-32011`; `NG.Interrupted`) belong to the remote run: `MCPTool` and `GrpcRemoteTool` raise `graph::RemoteControlFailure`, an ordinary failure that carries that code and the remote's public message and is not retried, so the local run goes on with a failed tool result. Every other MCP JSON-RPC error, gRPC tool error or status from a remote server is an ordinary tool failure for the caller, except a request the remote refused before running it (MCP `-32700`, `-32600`, `-32601`, `-32602`, a result marked `NG.InvalidRequest`; gRPC `NOT_FOUND`, `INVALID_ARGUMENT`), which is a failed tool result that is not effect-uncertain ([Tool failure contract](#tool-failure-contract)). TODO (later phase): carry a remote interrupt into the local run as a native pause that a human answers.

### Node failure contract

When a node throws, one classifier decides whether its retry policy may run it again and whether the failure may be routed to a failure handler. It walks the whole cause chain (`NodeExecutionError` causes, `std::nested_exception` and the failures of subgraphs the node ran), and the most severe class found anywhere in it wins. The tool failure contract above uses the same classifier: a run control signal anywhere in a tool's cause chain stops the run, for example a Hook block inside the failure of a graph the tool ran.

| Class (least to most severe) | Code | Raised by | Retried | Routable |
|------|------|------|------|------|
| Ordinary | `NG.TaskFailed`, `NG.SubgraphFailed` or an author code | `std::runtime_error` and its subclasses, `NodeFailureError`, a Python exception, a subgraph failure whose root is ordinary | yes, unless the author marked it not retryable | yes |
| Timeout | `NG.Timeout` | an attempt that ran past its `attempt_timeout_ms`, was cancelled and stopped (`NodeAttemptTimeout`); an operation that timed out (an asio or `std::system_error` with `timed_out`); a tool call that timed out or expired (`ToolExecutionError`) | yes | yes |
| ProviderFailed | `NG.ProviderFailed` | a transient `ProviderFailure`: transport, truncated response, deadline, overloaded, rate limited, or a remote failure with an HTTP 5xx status | no: the provider runtime owns provider retries | yes |
| EffectCommitted | `NG.EffectCommitted` | the attempt committed a non-idempotent Tool effect | never | yes, so a handler can compensate |
| LogicError | `NG.LogicError` | `std::logic_error`, including integrity checks | no | no |
| ConfigError | `NG.ConfigError` | provider configuration, request, authentication, permission, not-found or misuse failures; an exception that mixes in `ConfigurationFailure`, such as a Program diagnostic (`ProgramDiagnosticError`, a compile or admission error, in C++ or Python) | no | no |
| SafetyBlocked | `NG.SafetyBlocked` | a provider failure that stopped on a content filter or a refusal | no | no |
| BudgetExhausted | `NG.BudgetExhausted` | budget or quota exhaustion | no | no |
| EffectUncertain | `NG.EffectUncertain` | `ProviderOutcomeError`, including a provider dispatch whose outcome is unknown, a Tool effect whose outcome is unknown (a `ToolExecutionError` included), or a timed-out attempt that did not stop by its stop deadline | no | no |
| UnknownFailure | `NG.UnknownFailure` | `std::bad_alloc` or any other exception type | no | no |
| Run control | `NG.Interrupted`, `NG.HookBlocked`, `NG.Cancelled` | `NodeInterrupt`, `HookBoundaryBlocked`, cancellation | no | no: the run pauses or stops. A Hook block or a cancellation anywhere in the cause chain leaves the graph run as the `HookBoundaryBlocked` or `CancelledException` itself, with no `ERROR` event, as on every tool path |

`graph::classify_failure(error, run_token)` returns this verdict for any exception (for a `NodeExecutionError` that carries a `NodeFailure` record, the record's): its class, its code, `public_message`, whether it may be retried or routed, and for a run control class the signal itself (`FailureVerdict::signal`). Protocol hosts report the code on the wire ([Protocol failure mapping](#protocol-failure-mapping)).

The `NG.` prefix is reserved. An author code matches `[a-z][a-z0-9_]*(\.[a-z0-9_]+)+`. A C++ node throws `NodeFailureError(code, public_message, retryable)`. A Python node raises any exception with a string `code` attribute, and optionally a boolean `retryable` and a string `public_message`; the caller still receives the original exception. When the executor gives up on a node, the `NodeExecutionError` it throws carries a `NodeFailure` record, which `node_failure_of()` returns. The record holds the code, the class, why retrying stopped (`exhausted`, `non_retryable`, `effect_committed`, `ceiling` when the run's retry ceiling was reached, `deadline` when the backoff before the next attempt would have crossed the run's deadline, or `visit_limit` when the node was activated past its `max_visits`), the number of attempts, the retryable and routable flags, `public_message`, and the effects of the final attempt as `{tool, ordinal, status}`. It never copies `what()`. A code listed in the node's `non_retryable` is not retried.

**A routable failure can be handled instead of failing the run.** When the node has a failure edge (`failure_edges`; [Program graph features](program-graphs.md#failure-routing)), the executor throws nothing: the failed attempts write nothing, one structured record goes to the edge's `error_channel`, and the node routes to the edge's target only. The record has exactly `node`, `kind`, `code`, `attempts`, `retryable`, `step`, `routed_to` and `effects`, plus `detail.untrusted_text` only when the node opts in with `error_text` and the host's `EngineConfig::failure_text_redactor` keeps text from `public_message`. The node counts as run for the trace and `interruptAfter`, its pending write holds the handled outcome so a resume replays the route without running the node again, and its `ERROR` event says `handled: true` and `routed_to`. Static nodes, fan-out branches, `Send` targets and subgraph nodes all take this path, through both the sync and async entry points. A failure that is not routable, or of a node without a failure edge, is thrown as above.

**Every attempt has its own effect namespace.** Attempt 0 uses the task id (`s3:act`), so existing journals keep their slots. Attempt k uses `s3:act#a<k>`, and call ordinals start again inside it. Tool effects, provider calls and subgraph threads all follow the attempt, so a retry never replays or collides with an earlier attempt's effects; a `ProviderCallBroker` sees a fresh call slot for each retry. Before attempt k starts, the executor records an empty pending write keyed by the attempt's namespace; checkpoint stores need no schema change. A resume after a crash or an interrupt continues the latest attempt that started, in its namespace, so a `ToolEffectBroker` replays what that attempt committed, and the remaining retry budget is kept. Under a `ToolEffectBroker`, an attempt that committed a non-idempotent effect (a call that succeeded under a policy other than `ReadOnly` or `ExternalRead` and not `Idempotent`) is never retried, and an effect whose outcome is unknown pauses the run with the `tool_effect_reconciliation` interrupt even when the node caught that interrupt and raised another error. The effects of subgraphs a node runs count for that node's attempt.

**Attempt timeouts and the run's deadline.** With `attempt_timeout_ms` in its per-node policy (or a host timeout, `GraphEngine::set_node_attempt_timeout_ms`, which may only shorten the definition's), each attempt of a node runs off the coroutine that drives the run and is cancelled through its own child `CancelToken` when the timeout passes. The first terminal event wins: a completion at or after the timeout is the timeout. The timed-out attempt's late completion is discarded: no writes, no pending write, no event after the timeout, and a mediated Tool call it starts is refused. It fails with `NG.Timeout` once it has stopped, and the next attempt starts only then. One that does not stop within its stop deadline (the timeout again, at least one second) fails with `NG.EffectUncertain`, so the run fails; it goes on in the background and keeps its node alive, and `wait_for_node_attempts()` waits for it. When the backoff before the next attempt would cross `RunContext::deadline` (a Program's wall-time budget), the node stops retrying with kind `deadline` and keeps the failure's code. In a Program run every node body runs off the Program scheduler this way. See [Program graph features](program-graphs.md#attempt-timeouts-and-the-deadline).

---

## 4. Graph Types

**Header:** `<neograph/graph/types.h>`
**Namespace:** `neograph::graph`

Core types for the graph engine: channels, edges, events, and control-flow primitives.

### ReducerType

Determines how channel values are merged when written by multiple nodes.

```cpp
enum class ReducerType {
    OVERWRITE,  // New value replaces old value
    APPEND,     // New value is appended (for array channels)
    CUSTOM      // User-defined reducer function
};
```

### ReducerFn

Signature for custom reducer functions.

```cpp
using ReducerFn = std::function<json(const json& current, const json& incoming)>;
```

| Parameter | Description |
|-----------|-------------|
| `current` | The current channel value |
| `incoming` | The new value being written |

**Returns:** The merged result that becomes the new channel value.

### Channel

Internal representation of a named, versioned state channel with an associated reducer.

```cpp
struct Channel {
    std::string name;                              // Channel name
    ReducerType reducer_type = ReducerType::OVERWRITE; // Merge strategy
    ReducerFn   reducer;                           // Custom reducer (when type == CUSTOM)
    ChannelLifecyclePolicy lifecycle;
    json        value;                             // Current value
    uint64_t    version = 0;                       // Shared write counter at the last write
};
```

### ChannelWrite

A single write operation targeting a named channel. Nodes return vectors of these.

```cpp
struct ChannelWrite {
    enum class Mode { Reduce, Overwrite };
    std::string channel;
    json value;
    Mode mode = Mode::Reduce;
    std::shared_ptr<const std::vector<sp::Message>> native_messages;
};
```

`ChannelLifecyclePolicy` separates retention (`Unbounded`, `Latest`, `Bounded` plus `retention_limit`) from persistence (`Checkpoint`, `Ephemeral`). `ChannelWrite::Mode::Overwrite` bypasses the reducer, then applies retention. To retain authentic SDK history, use `provider_messages_write(messages_or_outcome)`; a JSON-only write cannot create native replay authority. See [channel lifecycle](concepts.md#channel-lifecycle-and-checkpoint-contract) for resume guards and ordered folds.

### NodeInterrupt

Exception type thrown from within a node to trigger a dynamic breakpoint (human-in-the-loop).
When thrown, execution pauses, a checkpoint is saved, and the interrupt can be resumed later.

```cpp
class NodeInterrupt : public std::runtime_error {
public:
    explicit NodeInterrupt(const std::string& reason);
    NodeInterrupt(const std::string& reason, json value);   // with a payload
    const std::string& reason() const;
    const json&        value()  const;   // null when no payload was attached
    const std::string& node()   const;   // stamped by the executor
};
```

| Method | Returns | Description |
|--------|---------|-------------|
| `reason()` | `const std::string&` | The reason string passed to the constructor |
| `value()` | `const json&` | The structured payload, or null if none was attached |
| `node()` | `const std::string&` | The node that threw. The executor stamps this — a node body does not know what the graph definition called it |

**The round trip.** An approval prompt needs information to travel in both
directions: the node says *what* needs approving, and the human's answer has to
come back to the node that asked.

```cpp
asio::awaitable<NodeResult> run(NodeInput in) override {
    // The human's answer. Empty until someone has actually answered — which is
    // how you tell "nobody has looked yet" from "the answer was no".
    const auto& verdict = in.ctx.resume_value;

    if (needs_approval(in.state) && !verdict) {
        throw NodeInterrupt("shell command needs approval",
                            json{{"tool", "shell"}, {"cmd", "rm -rf build/"}});
    }
    if (verdict && !verdict->value("approved", false)) {
        co_return refused();
    }
    co_return proceed();
}
```

The caller sees the pause as a normal `RunResult` — `NodeInterrupt` is not
re-thrown at them:

```cpp
auto r = engine->run(cfg);
if (r.interrupted) {
    r.interrupt_node;                          // "risky"  — which node paused
    r.interrupt_value["reason"];               // the sentence, for a human
    r.interrupt_value["value"];                // the payload, to branch on
                                               //   (key absent if none attached)
    engine->resume(cfg.thread_id, json{{"approved", true}});   // the answer
}
```

`resume_value` also arrives as a user turn on a `messages` channel when the
graph has one, which is how chat-shaped graphs have always received it.
`ctx.resume_value` is the general path — it works whatever the graph's channels
are called.

This is the *dynamic* form of interruption. The *static* form —
`interrupt_before` / `interrupt_after` in the graph definition — pauses at a
node chosen when the graph was written, which cannot express "pause only if the
model asked for something dangerous".

### Send

Represents a dynamic fan-out request. A node can return `Send` objects to dispatch
one or more nodes with different inputs, enabling map-reduce patterns.

```cpp
struct Send {
    std::string target_node;  // Node to dispatch
    json        input;        // Channel writes for that invocation
};
```

The engine executes each `Send` target with its own input, then continues the graph
after all sends complete. Multiple sends to the same node run in sequence.

### Command

Combined routing override and state update. A node returns a `Command` to simultaneously
write state updates AND redirect execution to a specific next node, bypassing normal
edge routing.

```cpp
struct Command {
    std::string               goto_node;  // Next node (overrides edge routing)
    std::vector<ChannelWrite> updates;    // State updates to apply
};
```

| Field | Type | Description |
|-------|------|-------------|
| `goto_node` | `std::string` | Name of the node to execute next. Overrides normal edge resolution |
| `updates` | `std::vector<ChannelWrite>` | Channel writes to apply before routing |

### RetryPolicy

Configures automatic retry behavior for node execution failures.

```cpp
struct RetryPolicy {
    int   max_retries        = 0;      // 0 = no retry
    int   initial_delay_ms   = 100;    // First retry delay in milliseconds
    float backoff_multiplier = 2.0f;   // Exponential backoff factor
    int   max_delay_ms       = 5000;   // Maximum delay cap in milliseconds
    float jitter_pct         = 0.0f;   // Per-retry jitter as a fraction of
                                       // the computed delay (0.25 = ±25%).
                                       // Default 0 = back-compat. Per-thread
                                       // RNG, no global state.
};
```

Delay for retry `n` is `min(initial_delay_ms * backoff_multiplier^n, max_delay_ms)`,
optionally multiplied by `1 + uniform(-jitter_pct, +jitter_pct)` when
`jitter_pct > 0`.

Which failures are retried, and how the effects of separate attempts are kept
apart, is described in the node failure contract (section 3).

### StreamMode

Bitfield flags controlling which events are emitted during streaming execution.

```cpp
enum class StreamMode : uint8_t {
    EVENTS  = 0x01,  // NODE_START, NODE_END, INTERRUPT, ERROR
    TOKENS  = 0x02,  // LLM_TOKEN (individual tokens from streaming LLM calls)
    VALUES  = 0x04,  // Full state snapshot after each step
    UPDATES = 0x08,  // Channel write deltas per node
    DEBUG   = 0x10,  // Internal debug info (retry attempts, routing decisions)
    ALL     = 0xFF   // All event types
};
```

Combine flags with bitwise OR:

```cpp
StreamMode mode = StreamMode::EVENTS | StreamMode::TOKENS;
```

**Operators:**

```cpp
StreamMode operator|(StreamMode a, StreamMode b);  // Combine flags
StreamMode operator&(StreamMode a, StreamMode b);  // Mask flags
bool has_mode(StreamMode flags, StreamMode test);   // Test if flag is set
```

### Edge

A static directed edge between two nodes.

```cpp
struct Edge {
    std::string from;  // Source node name
    std::string to;    // Target node name
};
```

Use the special constants `START_NODE` and `END_NODE` for graph entry and exit points.

### ConditionalEdge

A dynamic edge whose target is determined at runtime by a named condition function.

```cpp
struct ConditionalEdge {
    std::string from;                              // Source node name
    std::string condition;                         // Name in ConditionRegistry
    std::map<std::string, std::string> routes;     // condition_result -> target node name
};
```

At runtime, the engine calls the condition function (looked up by name in `ConditionRegistry`).
The function's return value is used as a key into the `routes` map to determine the next node.

### NodeContext

Dependency injection container passed to node constructors. Provides access to the
LLM provider, tools, and configuration.

```cpp
struct NodeContext {
    ProviderControls provider_controls;
    std::shared_ptr<Provider> provider;   // LLM provider
    ToolSet                  tools;      // Owned fixed collection of available tools
    std::string               model;      // Explicit model name; no provider default
    std::string               instructions; // System prompt / instructions
    json                      extra_config; // Additional configuration (node-type-specific)
};
```

Construct `ToolSet(std::move(unique_tools))` for a standalone Core context, or
pass it in `EngineResources::tools` when `NodeContext::tools` is empty. A
`ToolSet` can also adopt `std::vector<std::shared_ptr<Tool>>` for shared host
tools. Copies retain the same pointees: context reassignment cannot change a
previously compiled graph. `GraphCompiler::compile()` retains the collection
until `GraphEngine::link()` takes it; `GraphEngine::build()` does both steps.
Built-in `LLMCallNode` and `ToolDispatchNode` also retain this collection
when used without a `GraphEngine`. Factories may call `ctx.tools.view()` for
temporary raw lookup; only the owned collection survives dispatch. This has
no per-call ownership work. Supplying nonempty tools in both context and
resources is rejected.

Python `NodeContext(provider=...)` and its `provider` setter retain the original
Python provider object in a per-context lifetime-owner lease attached to the
real native shared pointer. Native context copies held by compiled nodes and engines retain
that same Python override owner after context reassignment or Python wrapper
collection. Reassignment releases only the mutable context's lease; existing
compiled snapshots keep their copies. The lease's final deleter acquires the
GIL. A borrowed C++ reference or raw pointer alone does not retain a Python
override owner.

### GraphEvent

Event emitted during streaming graph execution.

```cpp
struct GraphEvent {
    enum class Type {
        NODE_START,     // A node is about to execute
        NODE_END,       // A node has finished executing
        LLM_TOKEN,      // A single token from a streaming LLM call
        CHANNEL_WRITE,  // A channel value was updated
        INTERRUPT,      // Execution paused (NodeInterrupt or configured breakpoint)
        ERROR           // An error occurred during execution
    };

    Type        type;       // Event type
    std::string node_name;  // Name of the node that produced this event
    json        data;       // Event payload (varies by type)
};
```

**Event data payloads:**

| Type | `data` contents |
|------|-----------------|
| `NODE_START` | `{}` or node metadata |
| `NODE_END` | `null`, or an object with `command_goto` when the node returned a Command, `sends` (a count) when it returned Sends, and `send: true` when the execution was started by a Send |
| `LLM_TOKEN` | `{"token": "..."}` |
| `CHANNEL_WRITE` | `{"channel": "...", "value": ...}` |
| `INTERRUPT` | `{"reason": "...", "node": "..."}` |
| `ERROR` | `{"error": "...", "node": "..."}` |

### GraphStreamCallback

Type alias for the graph event callback used in streaming execution.

```cpp
using GraphStreamCallback = std::function<void(const GraphEvent&)>;
```

`GraphEvent` remains the stable callback and JSON-facing shape. Code that wants
typed payloads can adapt the same stream without changing the engine entry
point:

```cpp
using TypedGraphEvent = std::variant<NodeStartEvent, NodeEndEvent,
    LlmTokenEvent, ChannelWriteEvent, StateSnapshotEvent, RoutingEvent,
    SendDispatchEvent, InterruptEvent, ErrorEvent, RawGraphEvent>;

auto callback = adapt_typed_stream([](const TypedGraphEvent& event) {
    std::visit([](const auto& typed) {
        // Handle NodeStartEvent, LlmTokenEvent, and the other alternatives.
    }, event);
});
```

`to_typed_event()` performs the conversion directly. Malformed payloads and
payload shapes introduced by future versions become `RawGraphEvent` rather
than throwing from the streaming callback.

### NodeResult

Extended return type from node execution. Wraps channel writes with optional
`Command` and `Send` directives for advanced control flow.

```cpp
struct NodeResult {
    std::vector<ChannelWrite> writes;           // Channel updates
    std::optional<Command>    command;           // Routing override (if set)
    std::vector<Send>         sends;             // Dynamic fan-out targets

    NodeResult() = default;
    NodeResult(std::vector<ChannelWrite> w);     // Implicit from plain writes
};
```

When `command` is set, normal edge routing is bypassed and execution jumps to
`command->goto_node`. When `sends` is non-empty, the engine performs dynamic
fan-out to the specified targets.

### ConditionFn

Signature for condition functions used in conditional edges.

```cpp
using ConditionFn = std::function<std::string(const GraphState&)>;
```

The function inspects the current graph state and returns a string key. This key is
looked up in the `ConditionalEdge::routes` map to determine the next node.

### Constants

```cpp
constexpr const char* START_NODE = "__start__";  // Graph entry point
constexpr const char* END_NODE   = "__end__";    // Graph termination
```

These are used in edge definitions to mark graph entry and exit:

```cpp
Edge{START_NODE, "my_first_node"}
Edge{"my_last_node", END_NODE}
```

---

## 5. GraphState

**Header:** `<neograph/graph/state.h>`
**Namespace:** `neograph::graph`

Thread-safe, versioned key-value state container for the graph. Each entry is a
named channel with an associated reducer that controls how values are merged.

```cpp
class GraphState {
public:
    void init_channel(const std::string& name,
                      ReducerType type,
                      ReducerFn reducer,
                      const json& initial_value = json(),
                      ChannelLifecyclePolicy lifecycle = {});

    json get(const std::string& channel) const;
    std::vector<ChatMessage> get_messages() const;
    std::vector<sp::Message> get_provider_messages(
        const std::string& channel = "messages") const;
    std::optional<std::vector<sp::Message>> captured_provider_messages(
        const std::string& channel = "messages") const;

    void write(const std::string& channel, const json& value);
    void apply_writes(const std::vector<ChannelWrite>& writes);

    uint64_t channel_version(const std::string& channel) const;
    uint64_t global_version() const;

    json serialize() const;
    void restore(const json& data);
    json serialize_runtime() const;
    void restore_runtime(const json& data);
    json ephemeral_checkpoint_guard() const;
    void restore_checkpoint(const json& data, const json& guard,
                            std::shared_ptr<const NativeGraphCheckpoint> native = {});
    std::pair<json, std::shared_ptr<const NativeGraphCheckpoint>> checkpoint_snapshot() const;

    std::vector<std::string> channel_names() const;
};
```

| Method | Description |
|--------|-------------|
| `init_channel(name, type, reducer, initial_value)` | Register a channel with its reducer and optional initial value. Must be called before any read/write to that channel |
| `get(channel)` | Read the current value of a channel. Thread-safe (shared lock) |
| `get_messages()` | Convenience method: reads the `"messages"` channel and deserializes it as `std::vector<ChatMessage>` |
| `write(channel, value)` | Write a value to a single channel through its reducer. Thread-safe (exclusive lock) |
| `apply_writes(writes)` | Atomically apply a batch of `ChannelWrite` operations. All writes are applied under a single exclusive lock |
| `channel_version(channel)` | Returns the channel's version: the value of the shared write counter at the channel's last write, or 0 if it was never written |
| `global_version()` | Returns the shared write counter, which every write to any channel increments (the run input's writes included) |
| `serialize()` | Serializes checkpoint-persistent channel values and versions |
| `restore(data)` | Restores channel values and versions from serialized JSON |
| `channel_names()` | Returns the names of all initialized channels |

`serialize()` includes only checkpoint-persistent channel values and versions. It omits ephemeral values; `restore_checkpoint` requires their matching guard and rejects lost written ephemeral state. `serialize_runtime` / `restore_runtime` preserve live ephemeral values for same-process copies, not durable storage. `checkpoint_snapshot()` pairs the portable snapshot with its genuine C++ native sidecar. `get_messages()` is a convenience projection; use `get_provider_messages()` for full ordered SDK history and `captured_provider_messages()` when arbitrary JSON must not be interpreted as chat.

---

## 6. GraphNode

**Header:** `<neograph/graph/node.h>`
**Namespace:** `neograph::graph`

Nodes are the computational units of a graph. The library provides an
abstract base class and four built-in node types.

### GraphNode (abstract)

A subclass overrides ONE method: `run(NodeInput) -> awaitable<NodeOutput>`.
Read state, decide what to do, return writes (and optionally `Command` /
`Send`).

```cpp
class GraphNode {
public:
    virtual ~GraphNode() = default;

    // The only custom-node dispatch entry.
    virtual asio::awaitable<NodeOutput> run(NodeInput in) = 0;

    virtual std::string get_name() const = 0;
};

struct NodeInput {
    const GraphState&          state;       // channels visible to this node
    const RunContext&          ctx;         // cancel_token, step, thread_id, ...
    const GraphStreamCallback* stream_cb;   // null when not streaming
};

using NodeOutput = NodeResult;  // writes + optional Command + optional Sends
```

| Member | Description |
|--------|-------------|
| `in.state` | Read-only `GraphState`. Use `in.state.get(channel)` for reads |
| `in.ctx.cancel_token` | Assign `request.cancel_token = in.ctx.cancel_token` before `provider.invoke(std::move(request))`; provider cancellation is cooperative at supported boundaries. For your own loops, poll a non-null `ctx.cancel_token` with `ctx.cancel_token->is_cancelled()` |
| `in.ctx.step` | Current super-step index |
| `in.ctx.thread_id` | Mirrors `RunConfig::thread_id` |
| `in.stream_cb` | Streaming sink; if non-null, emit `LLM_TOKEN` events through it. Null on non-streaming runs |
| Return: `NodeOutput.writes` | Channel writes the engine merges via reducers |
| Return: `NodeOutput.command` | Optional routing override (`goto_node` + state updates) |
| Return: `NodeOutput.sends` | Optional dynamic fan-out — engine spawns one branch per `Send` |
| `get_name()` | Returns the node's unique name within the graph |

Minimal example:

```cpp
class CounterNode : public neograph::graph::GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto current = in.state.get("count");
        int n = current.is_number() ? current.get<int>() : 0;
        NodeOutput out;
        out.writes.push_back({"count", n + 1});
        co_return out;
    }
    std::string get_name() const override { return "counter"; }
};
```

Async-native LLM call:

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

> **Migration note.** `GraphNode` has one node entry point:
> `run(NodeInput)`. It preserves `Command` and `Send`, participates in async and
> streaming execution, and is the override subclasses must implement.

### LLMCallNode

Calls the LLM with the current conversation state. Reads from the
`"messages"` channel, sends a completion request to the provider, and
writes the assistant's response back. Streams `LLM_TOKEN` events when
the run was started via `run_stream` / `run_stream_async`.

```cpp
class LLMCallNode : public GraphNode {
public:
    LLMCallNode(const std::string& name, const NodeContext& ctx);
    asio::awaitable<NodeOutput> run(NodeInput in) override;
    std::string get_name() const override;
};
```

| Constructor Parameter | Description |
|-----------------------|-------------|
| `name` | Node name |
| `ctx` | Node context providing the LLM provider, tools, model, and instructions |

(LLMCallNode, `ToolDispatchNode`, `IntentClassifierNode`, and `SubgraphNode`
all implement the same `run(NodeInput)` contract.)

### ToolDispatchNode

Dispatches tool calls from the latest assistant message. Reads pending tool calls from
the `"messages"` channel, executes each tool, and writes tool result messages back.

```cpp
class ToolDispatchNode : public GraphNode {
public:
    ToolDispatchNode(const std::string& name, const NodeContext& ctx);

    asio::awaitable<NodeOutput> run(NodeInput in) override;
    std::string get_name() const override;
};
```

| Constructor Parameter | Description |
|-----------------------|-------------|
| `name` | Node name |
| `ctx` | Node context (uses `ctx.tools` to look up and execute tools) |

### IntentClassifierNode

Uses the LLM to classify user intent, then writes the classification result to the
`"__route__"` channel. Designed for use with the `"route_channel"` built-in condition
to enable dynamic intent-based routing.

```cpp
class IntentClassifierNode : public GraphNode {
public:
    IntentClassifierNode(const std::string& name, const NodeContext& ctx,
                         const std::string& prompt,
                         std::vector<std::string> valid_routes);

    asio::awaitable<NodeOutput> run(NodeInput in) override;
    std::string get_name() const override;
};
```

| Constructor Parameter | Type | Description |
|-----------------------|------|-------------|
| `name` | `std::string` | Node name |
| `ctx` | `NodeContext` | Provider and model for the classification LLM call |
| `prompt` | `std::string` | Classification prompt template |
| `valid_routes` | `std::vector<std::string>` | Allowed classification values. The LLM output is validated against these |

### SubgraphNode

Wraps a compiled `GraphEngine` as a single node, enabling hierarchical graph composition
(supervisor pattern, nested workflows). Channel mappings control data flow between
parent and child graphs.

```cpp
class SubgraphNode : public GraphNode {
public:
    SubgraphNode(const std::string& name,
                 std::shared_ptr<GraphEngine> subgraph,
                 std::map<std::string, std::string> input_map = {},
                 std::map<std::string, std::string> output_map = {},
                 SubgraphPersistence persistence = SubgraphPersistence::Legacy);
    asio::awaitable<NodeOutput> run(NodeInput in) override;
    std::string get_name() const override;
};
```

| Constructor Parameter | Type | Description |
|-----------------------|------|-------------|
| `name` | `std::string` | Node name in the parent graph |
| `subgraph` | `std::shared_ptr<GraphEngine>` | The compiled child graph engine |
| `input_map` | `std::map<std::string, std::string>` | `parent_channel -> child_channel` mapping. Read from parent, write to child input |
| `output_map` | `std::map<std::string, std::string>` | `child_channel -> parent_channel` mapping. Rename and forward child-produced write deltas to the parent |
| `persistence` | `SubgraphPersistence` | `Legacy` (compatible), `PerInvocation`, `PerThread`, or `Stateless`; JSON topology nodes use the lower-case `persistence` string |

If the maps are empty, channels are mapped by name (identity mapping).

Input mapping copies the parent's current channel values into the child's input.
Output mapping is deliberately different: it forwards the child's ordered
`ChannelWrite` deltas, preserving each write's `Mode`, rather than treating the
child's final serialized state as a new reducer input. Consequently inherited
append/custom values are not applied twice. Output mapping does not infer
snapshot replacement; a child must emit `ChannelWrite::Mode::Overwrite` when it
intends to replace the mapped parent value.
#### Child persistence and inspection

| Mode | Child checkpoint namespace | Start/resume | Store precedence |
|------|----------------------------|--------------|------------------|
| `Legacy` (default) | Length-framed parent thread, node, parent step, task ID (`subgraph/...`) | Historical behavior: a fresh parent starts a fresh child; parent resume loads the matching child snapshot | Parent run's checkpoint backend if present; otherwise child's configured backend |
| `PerInvocation` | `subgraph/run/` plus parent thread, node, persisted parent graph-invocation UUID, step, and task ID | A new parent run gets a fresh namespace; parent resume restores the UUID and the child write journal | Parent, then child |
| `PerThread` | `subgraph/thread/` plus parent thread and node | Fresh calls seed child state from the previous checkpoint before applying new input; parent resume resumes a matching child snapshot; overlapping calls on the same compiled node/namespace fail rather than race | Parent, then child |
| `Stateless` | None | Child checkpoints are disabled even if the child has a backend. Interrupt/resume is unsupported and rejected; Store, cancellation, and ToolGate still propagate | No checkpoint backend; parent Store then child Store |

Explicit stateful modes require a nonempty parent thread ID. `Legacy` retains
the exact pre-#238 namespace and checkpoint wire format, including the existing
empty-thread behavior. `PerInvocation` records `_neograph.subgraph_invocation_id`
in parent checkpoint metadata; old checkpoints without it cannot resume after
an opt-in policy change. `PerThread` is a shared namespace: a host sharing the
same durable backend between *different* engines/processes must also coordinate
their admission; the node-local guard only covers one compiled node. Avoid
switching an existing thread between policies without an explicit migration.

Use `GraphEngine::inspect_nested_checkpoint(root_thread, path[, run_store])`
to traverse child and grandchild checkpoints. Each `SubgraphPathStep` supplies
the child node name, parent super-step, stable Core task ID (`s0:child` or a
Send task ID), and optionally the exact parent checkpoint ID to pin an older
invocation. The result contains `graph_path`, child `thread_id`, and the
complete `Checkpoint` (including serialized channel values and checkpoint ID).
A checkpoint ID from a different thread or a stateless path is rejected.
Pass the same run-scoped checkpoint store used by `RunResources` when it
overrode the engine's backend. The public
`SubgraphNode::checkpoint_thread_id()` reconstructs one segment's namespace.


#### Runtime-context propagation

`SubgraphNode` derives a child execution context at the engine boundary. It
does not change the public `RunContext` layout.

| Context value | Child semantics |
|---------------|-----------------|
| `cancel_token` | A descendant operation token is created, so parent cancellation reaches every child and grandchild. |
| `usage`, `deadline`, `trace_id`, `stream_mode` | Inherited. `deadline` and `trace_id` originate from `RunMetadata`; a child cannot widen the parent's stream mode. |
| `thread_id` | When the parent thread ID is non-empty, deterministically derived from it, the subgraph node name, super-step, and invocation identity. Sibling `Send` invocations therefore receive distinct checkpoint identities. An empty parent thread ID keeps the child unscoped and checkpointing disabled. |
| `step` | Local to the child execution; it starts from the child checkpoint or zero. |
| `store` | The parent Store is inherited when present; otherwise the child engine retains its configured Store. |
| Tool policy | Parent `ToolGate` runs before the child's gate. A child may further restrict or rewrite an allowed call, but cannot bypass a parent deny or interrupt. |
| Checkpoint backend and resume value | The parent backend is inherited when present; otherwise the child keeps its own backend. A parent resume follows a child checkpoint only when the child's derived checkpoint identity exists, forwarding a non-null resume value. Checkpoint routing is internal, not a public `RunContext` field. |

---

## 7. GraphEngine

**Header:** `<neograph/graph/engine.h>`
**Namespace:** `neograph::graph`

The core execution engine. Compiles graph definitions, manages state transitions,
and orchestrates node execution through a super-step loop.

### EngineConfig and EngineResources

New code should assemble construction dependencies and policies before creating
the engine:

```cpp
struct EngineConfig {
    NodeContext node_context;
    std::shared_ptr<CheckpointStore> checkpoint_store;
    std::shared_ptr<Store> store;
    std::optional<RetryPolicy> retry_policy;
    std::map<std::string, RetryPolicy> node_retry_policies;
    ToolGate tool_gate;
    std::size_t worker_count = 1;
    std::set<std::string> cached_nodes;
    std::size_t node_cache_max_entries = 0;
    std::map<std::string, CacheKeyPolicy> node_cache_policies;
    std::shared_ptr<ToolExecutionController> tool_execution_controller;
    std::shared_ptr<::neograph::HookRuntime> hook_runtime;
    std::shared_ptr<::neograph::RuntimeInterpositionController> runtime_interposition;
    std::shared_ptr<sp::NativeArchive> native_history_archive;
};

struct EngineResources {
    ToolSet tools;
    std::shared_ptr<const GraphRegistry> registry;
};
```

`ToolSet` owns a fixed tool collection. A new `GraphRegistry` accepts
engine-specific reducer, condition, and node registrations and resolves built-ins
without setup. It **does not** inherit process-global custom registrations.
`GraphEngine::build` / `link` copy a synchronized registry snapshot before
compiling and running; registering again on the original changes only engines
built afterwards. The engine owns its snapshot, so the original registry may be
destroyed. Callables themselves must remain thread-safe when shared by
concurrent runs.

For migration, existing `GraphEngine::compile`, no-registry `build`, and
`NodeFactory` / `ReducerRegistry` / `ConditionRegistry` singleton registration
remain supported. To opt a particular registry into the legacy custom-name
fallback, construct `GraphRegistry{GraphRegistry::Fallback::GlobalFallback}`.
Its global entries are captured at engine construction; avoid this policy for
tenant or test isolation. `GraphCompiler::compile_local` and sealed Program
registries remain exact local-only resolvers (no built-in fallback).

Legacy Python singleton callbacks still look up their names in module-global
dictionaries at invocation, so replacing such a name can change existing
legacy engines. Use scoped Python registries to preserve callback identity.

### RunConfig

Configuration for a single graph execution run.

```cpp
struct RunConfig {
    std::string                 thread_id;
    json                        input;
    int                         max_steps    = 50;
    StreamMode                  stream_mode  = StreamMode::ALL;
    std::shared_ptr<CancelToken> cancel_token;          // v0.3+
    std::shared_ptr<UsageAccumulator> usage;             // optional accumulator
    std::optional<std::vector<sp::Message>> provider_messages;
    std::shared_ptr<sp::NativeArchive> native_history_archive;
    std::function<void(const sp::Event&)> on_provider_event;
    std::shared_ptr<ProviderOutcomes> provider_outcomes;
    std::shared_ptr<ProviderLoopHistory> provider_loop_history;
    std::uint64_t model_token_budget = 0;
    std::shared_ptr<std::atomic_bool> budget_exhausted;
    bool                        resume_if_exists = false; // v0.3.1+
};
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `thread_id` | `std::string` | `""` | Identifies the conversation/session for checkpointing |
| `input` | `json` | `{}` | Initial values written to channels before execution starts. Typically `{"messages": [...]}` |
| `max_steps` | `int` | `50` | Maximum number of super-steps before forced termination (prevents infinite loops) |
| `stream_mode` | `StreamMode` | `ALL` | Bitfield controlling which event types are emitted during streaming |
| `cancel_token` | `std::shared_ptr<CancelToken>` | `nullptr` | Cooperative cancel handle. Engine wraps this into a `RunContext` and threads it to every node's `run(NodeInput)` call as `in.ctx.cancel_token` |
| `usage` | `std::shared_ptr<UsageAccumulator>` | `nullptr` | Optional token accumulator. The engine creates one when omitted and exposes the active accumulator as `in.ctx.usage` |
| `resume_if_exists` | `bool` | `false` | If `true` and a checkpoint exists for `thread_id`, seed from it before applying `input` (multi-turn chat shape) |

**Known checkpoint limitation:** fresh non-resuming runs that reuse an existing
thread can collide with channel-version blob keys in `InMemoryCheckpointStore`
and subsequently restore stale values. This is a pre-existing fresh-run defect,
not fixed by the subgraph persistence changes. Use a new thread ID for a fresh
history, or `resume_if_exists=true` for an intentional continuing-thread turn.

### RunContext (v0.4, exposed to nodes via `NodeInput.ctx`)

Per-run dispatch metadata threaded by the engine. Initially constructed from
`RunConfig` (allocating a usage accumulator when none was supplied),
`RunMetadata`, the effective Store, and an optional resume value. Nodes consume
it inside a `run(NodeInput) -> NodeOutput` override via `in.ctx`.

That allocation describes initial construction only. Checkpoint restoration can replace it with the genuine original bank and its prior reports. Resume or continuation therefore does not promise a fresh returned usage report or `None` for previously reported usage.

Python `RunMetadata(timeout_ms=None, ...)` has no deadline by default. Its constructor and `set_timeout_ms(timeout)` accept nonnegative integer milliseconds within the remaining steady-clock range, checked before signed conversion or addition. Negative or overflowing values raise `OverflowError` or `ValueError`; a failed setter leaves the prior absolute deadline intact. Zero expires immediately; `clear_deadline()` removes the deadline.

```cpp
struct RunContext {
    std::shared_ptr<CancelToken>  cancel_token;
    std::shared_ptr<UsageAccumulator> usage;
    std::shared_ptr<ProviderOutcomes> provider_outcomes;
    std::shared_ptr<ProviderLoopHistory> provider_loop_history;
    std::function<void(const sp::Event&)> on_provider_event;
    std::shared_ptr<sp::NativeArchive> native_history_archive;
    std::uint64_t model_token_budget = 0;
    std::shared_ptr<std::atomic_bool> budget_exhausted;
    std::string run_id;
    std::shared_ptr<CancelToken> budget_cancel_token;
    std::shared_ptr<OwnedManagedBudgetLease> managed_budget_lease;
    std::shared_ptr<CheckpointStore> managed_budget_store;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::string                   trace_id;
    std::string                   thread_id;
    std::uint64_t                 cache_execution_id = 0;
    int                           step;
    StreamMode                    stream_mode;
    std::optional<json>           resume_value;
    std::shared_ptr<Store>        store;
    ToolGate                      tool_gate;
    std::shared_ptr<ToolExecutionController> tool_execution_controller;
    ToolExecutionIdentity tool_execution_identity;
};
```

| Field | Description |
|-------|-------------|
| `cancel_token` | The active token. Pass to `ProviderRequest::cancel_token` so an LLM HTTP socket aborts on cancel, or poll `is_cancelled()` for your own loops |
| `usage` | Shared token-accounting sink populated by the engine |
| `deadline` | Optional absolute deadline from C++ `RunMetadata` |
| `trace_id` | Optional trace correlator from C++ `RunMetadata` |
| `thread_id` | Mirror of `RunConfig.thread_id` |
| `step` | Current super-step index, updated each iteration |
| `stream_mode` | Mirror of `RunConfig.stream_mode` |
| `resume_value` | Value supplied to `GraphEngine::resume()`, or empty on a fresh run |
| `store` | Store installed on the engine, or `nullptr` when none is configured |
| `tool_gate` | Effective policy for this invocation, including any inherited parent policy |

### CancelToken

Cooperative cancel primitive shared between caller and engine. Construct
via `std::make_shared<CancelToken>()`, hand to `RunConfig.cancel_token`,
and call `cancel()` from any thread to abort the in-flight run —
including the LLM HTTP socket if a node is mid-`provider.invoke_async`.
Each engine run forks its own operation child, so one parent can safely cancel
multiple concurrent runs without sharing an asio cancellation slot.

Engine operation children retain themselves until their posted cancellation
emit executes. If application code calls `bind_executor()` directly on a token
it constructed itself, the application must keep that token alive until the
executor drains; the engine cannot supply ownership for an external object.
Because these methods are inline in the public header, existing C++ consumers
must be recompiled to receive the updated `fork()` lifetime behavior. The
`CancelToken` object layout remains binary-compatible with 0.11.x.

```cpp
class CancelToken {
public:
    void cancel() noexcept;                            // request cancellation
    bool is_cancelled() const noexcept;                // polling read

    std::shared_ptr<CancelToken> fork();                // v0.4: child token
    void bind_executor(asio::any_io_executor ex);
    asio::cancellation_slot slot() noexcept;
};
```

#### Hierarchical cancel (v0.4 `fork()`)

Each child token has its own `cancellation_signal`; the parent's
`cancel()` cascades to every live child. This is the structural
replacement for the v0.3.x `add_cancel_hook` list (deprecated, removed
in v1.0). Concurrent nested scopes — a multi-Send fan-out where every
worker calls `provider.invoke(std::move(request))` simultaneously — each
`fork()` once and never overwrite each other's slot.

```cpp
// Caller side: one parent token, fan it out across N concurrent runs.
auto parent = std::make_shared<neograph::graph::CancelToken>();

RunConfig cfg_a; cfg_a.thread_id = "user-1"; cfg_a.cancel_token = parent;
RunConfig cfg_b; cfg_b.thread_id = "user-2"; cfg_b.cancel_token = parent;

auto fut_a = std::async(std::launch::async, [&] { return engine->run(cfg_a); });
auto fut_b = std::async(std::launch::async, [&] { return engine->run(cfg_b); });

// User hits stop in the UI:
parent->cancel();   // cascades to every fork() child, every run aborts

// Inside a RuntimeInterpositionConsumer node, pass cancellation in the owned request.
// socket aborts on parent cancel without you doing any wiring:
asio::awaitable<NodeOutput> run(NodeInput in) override {
    auto request = neograph::make_provider_request(
        *provider_, model_, in.state.get_provider_messages());
    request.cancel_token = in.ctx.cancel_token;
    request.options.deadline = in.ctx.deadline;
    auto reply = co_await neograph::graph::observe_provider_result(
        in.ctx, invoke_provider(provider_, std::move(request), {}, {},
            neograph::graph::provider_call_broker(in.ctx),
            neograph::graph::make_provider_call_identity(in.ctx, get_name())));
    neograph::graph::record_usage(in.ctx, reply);
    neograph::outcome_or_throw(reply);
    NodeOutput out;
    out.writes.push_back(neograph::graph::provider_messages_write(reply));
    co_return out;
}
```

| Method | Description |
|--------|-------------|
| `cancel()` | Idempotent, thread-safe. Sets the polling flag and emits the asio cancellation_signal on the bound executor; cascades to all live children via `fork()` |
| `is_cancelled()` | Lock-free polling read |
| `fork()` | **v0.4.** Returns a child shared_ptr. Parent.cancel() cascades; if the parent is already cancelled at fork() time the child is constructed pre-cancelled (no emit-vs-bind race) |
| `bind_executor(ex)` | Engine-internal; binds the executor that handles signal emits |
| `slot()` | asio `cancellation_slot` for `bind_cancellation_slot` at `co_spawn` time |

### RunResult

Result returned after graph execution completes or is interrupted.

```cpp
struct RunResult {
    sp::Usage usage;
    std::vector<sp::Message> native_messages;
    std::vector<sp::runtime::Result> provider_outcomes;
    json        output;                          // Final serialized state
    bool        interrupted       = false;       // True if execution was paused (HITL)
    std::string interrupt_node;                  // Node that caused the interrupt
    json        interrupt_value;                 // Value associated with the interrupt
    std::string checkpoint_id;                   // ID of the last checkpoint saved
    std::vector<std::string> execution_trace;    // Ordered list of executed node names

    bool max_steps_exhausted() const noexcept;    // Limit stopped runnable work
    bool barrier_stalled() const noexcept;        // A barrier can no longer fire
    bool failure_exited() const noexcept;         // A failure-exit node completed (#364)
    RunStatus status() const noexcept;            // Completed, Interrupted, StepLimit, SafePoint, BarrierStalled or FailureExit

    template <typename T> T channel(const std::string& name) const;
    template <typename T> T channel(const ChannelKey<T>& key) const;
    template <typename T>
    std::optional<T> try_channel(const ChannelKey<T>& key) const;
};
```

`RunResult::usage` is the nullable provider report, not the spending bank. `native_messages` retains genuine typed history and `provider_outcomes` retains every owned Completion/Failure. JSON `output` is only a portable projection. For full history input use `RunConfig::provider_messages`; observe typed events with `on_provider_event`. Durable native checkpoint/receipt custody must use `native_history_archive`; in-memory sidecars do not require one.

In `output`, `channels.<name>.version` and `global_version` come from one write counter that all channels share. A channel's version is the counter's value at its last write, so versions order writes; a version is not the number of writes the channel received, and `global_version` is not a super-step number. See [Versions](concepts.md#versions).

On resume or continuation, `provider_outcomes` retains the original outcomes followed by newly produced outcomes in an ordered list. `usage` can retain prior reports from the restored bank. Consumers must preserve the no-redispatch and no-double-charge invariants for completed provider effects; an empty/reset outcome list or `usage=None` is not a resume invariant.
| Field | Type | Description |
|-------|------|-------------|
| `output` | `json` | Serialized final state of all channels |
| `interrupted` | `bool` | `true` if execution was paused by an interrupt (HITL) |
| `interrupt_node` | `std::string` | Name of the node that triggered the interrupt |
| `interrupt_value` | `json` | Reason or payload from the interrupt |
| `checkpoint_id` | `std::string` | UUID of the last saved checkpoint |
| `execution_trace` | `std::vector<std::string>` | One entry per completed node execution, in the order the engine applied the results: super-steps in order, and within a step the ready nodes, then the Sends in the order they were emitted, whatever order they finished in. A Send execution is listed as `<target>[send]`. See [Send](concepts.md#5-send--dynamic-fan-out) |

`provider_messages` supplies full typed history and replaces only the messages channel; `on_provider_event` observes typed SDK events. `provider_outcomes` and `provider_loop_history` retain outcomes and task-local continuations across inner turns. `native_history_archive` binds durable native custody, not a model budget. The optional `model_token_budget` ceiling and `budget_exhausted` signal belong to budget-aware dispatch. Python and C++ both return full history as `RunResult.native_messages`, with owned results in `provider_outcomes`. Input remains `RunConfig.provider_messages`; there is no `RunResult.provider_messages` or `provider_history` alias.

`max_steps_exhausted()` returns `true` only when the step ceiling stopped the
run while runnable work remained. A graph that reaches `__end__` exactly on its
last permitted step returns `false`.

`status()` returns `RunStatus::Completed`, `RunStatus::Interrupted`,
`RunStatus::StepLimit`, `RunStatus::SafePoint`, `RunStatus::BarrierStalled`, or `RunStatus::FailureExit` without changing the public `RunResult` data layout.
`BarrierStalled` means that nothing was ready, the final super-step did not route to `__end__` explicitly, and a barrier still held signals, so the join can no longer fire. `barrier_stalled()` is then true, and `output["_neograph"]["barrier_stalled"]` lists each stalled join as `{node, wait_for, received, missing}`.
`FailureExit` (#364) means that a failure-exit node completed and the run ended as failed: `failure_exited()` is true and `output["_neograph"]["failure_exit"]` is `{node, code}`. A `RunInvocation` reports it as `Error`.
`ChannelKey<T>` binds a reusable channel name to its expected C++ type:

```cpp
inline const ChannelKey<std::string> Answer{"answer"};

auto answer = result.channel(Answer);
if (auto optional = result.try_channel(Answer)) {
    std::cout << *optional << '\n';
}
```

### GraphEngine

The main engine class. New code should use `build_strict()` for a JSON definition;
it rejects invalid topology before any node is instantiated. Use `link()` with a
`ValidatedTopology` when parse, validation, inspection, or transformation must be
separate steps. The lenient `build()`, `CompiledGraph` link overloads, `compile()`,
and post-construction setters remain compatibility paths.

```cpp
class GraphEngine {
public:
    // ---- Construction ----

    static std::unique_ptr<GraphEngine> build(
        const json& definition, EngineConfig config);
    static std::unique_ptr<GraphEngine> build(
        const json& definition, EngineConfig config, EngineResources resources);

    static std::unique_ptr<GraphEngine> build_strict(
        const json& definition, EngineConfig config);
    static std::unique_ptr<GraphEngine> build_strict(
        const json& definition, EngineConfig config, EngineResources resources);

    static std::unique_ptr<GraphEngine> link(
        ValidatedTopology topology, EngineConfig config = {});
    static std::unique_ptr<GraphEngine> link(
        ValidatedTopology topology, EngineConfig config, EngineResources resources);

    static std::unique_ptr<GraphEngine> link(
        CompiledGraph graph, EngineConfig config = {});
    static std::unique_ptr<GraphEngine> link(
        CompiledGraph graph, EngineConfig config, EngineResources resources);

    static std::unique_ptr<GraphEngine> compile( // compatibility facade
        const json& definition, const NodeContext& default_context,
        std::shared_ptr<CheckpointStore> store = nullptr);

    // ---- Execution (sync) ----

    RunResult run(const RunConfig& config);

    RunResult run_stream(const RunConfig& config,
                         const GraphStreamCallback& cb);

    RunResult resume(const std::string& thread_id,
                     const json& resume_value = json(),
                     const GraphStreamCallback& cb = nullptr);

    // ---- Execution (async, 3.0) ----

    asio::awaitable<RunResult> run_async(const RunConfig& config);

    asio::awaitable<RunResult> run_stream_async(
        const RunConfig& config, const GraphStreamCallback& cb);

    asio::awaitable<RunResult> resume_async(
        const std::string& thread_id,
        const json& resume_value = json(),
        const GraphStreamCallback& cb = nullptr);

    // ---- State Inspection & Manipulation ----

    GraphAdmin admin(); // borrowed facade; must not outlive this engine

    std::optional<json> get_state(const std::string& thread_id) const;

    std::vector<Checkpoint> get_state_history(const std::string& thread_id,
                                              int limit = 100) const;

    void update_state(const std::string& thread_id,
                      const json& channel_writes,
                      const std::string& as_node = "");

    std::string fork(const std::string& source_thread_id,
                     const std::string& new_thread_id,
                     const std::string& checkpoint_id = "");

    // ---- Compatibility configuration (prefer EngineConfig/EngineResources) ----

    void set_checkpoint_store(std::shared_ptr<CheckpointStore> store);
    void set_store(std::shared_ptr<Store> store);
    std::shared_ptr<Store> get_store() const;
    void set_retry_policy(const RetryPolicy& policy);
    void set_node_retry_policy(const std::string& node_name, const RetryPolicy& policy);

    // Fan-out worker pool. n==1 keeps the engine on the caller's
    // executor (no engine-owned thread_pool); n>=2 installs an
    // owned `asio::thread_pool` of size n. build() defaults to
    // n==1 — prefer EngineConfig::worker_count to opt into
    // real parallel fan-out. Throws `std::logic_error` if called
    // while a run is in flight (an `active_runs_`
    // counter prevents tasks queued on the old pool from being
    // silently dropped on swap).
    void set_worker_count(std::size_t n);

    // Compatibility convenience: set_worker_count(hardware_concurrency()).
    void set_worker_count_auto();

    // Per-node result caching. Disabled by default; opt in per node.
    void set_node_cache_enabled(const std::string& node_name, bool enabled);
    void clear_node_cache();
    const NodeCache& node_cache() const;

    const std::string& get_graph_name() const;
};
```

#### `build` and `link`

```cpp
EngineConfig config;
config.node_context.provider = provider;
config.checkpoint_store = checkpoint_store;
config.store = store;
config.worker_count = 4;
config.cached_nodes.insert("retrieve");

std::vector<std::unique_ptr<Tool>> owned_tools;
owned_tools.push_back(std::make_unique<SearchTool>());
auto registry = std::make_shared<GraphRegistry>();
// Register engine-local reducers, conditions, or node types on registry.

EngineResources resources{
    .tools = ToolSet(std::move(owned_tools)),
    .registry = registry,
};

auto engine = GraphEngine::build(definition, std::move(config),
                                 std::move(resources));
```

`build()` compiles, verifies, links, and returns a fully configured engine.
`link()` consumes a `CompiledGraph` by move and applies runtime configuration;
callers that compile manually remain responsible for any source-to-IR
round-trip verification they require.

#### `compile` (compatibility)

```cpp
static std::unique_ptr<GraphEngine> compile(
    const json& definition,
    const NodeContext& default_context,
    std::shared_ptr<CheckpointStore> store = nullptr);
```

Compiles a graph from a JSON definition and returns an engine ready for execution.
This original signature is preserved and delegates to `build()`. Prefer
`EngineConfig` when new code needs stores, retry policy, worker configuration,
caching, or a tool gate.

| Parameter | Type | Description |
|-----------|------|-------------|
| `definition` | `const json&` | Graph definition in JSON format (see below) |
| `default_context` | `const NodeContext&` | Default context injected into all nodes |
| `store` | `std::shared_ptr<CheckpointStore>` | Optional checkpoint store for persistence |

**Graph definition JSON schema:**

```json
{
  "name": "my_graph",
  "channels": {
    "messages": {"reducer": "append"},
    "status": {"reducer": "overwrite", "initial": "idle"}
  },
  "nodes": {
    "llm": {"type": "llm_call"},
    "tools": {"type": "tool_dispatch"}
  },
  "edges": [
    {"from": "__start__", "to": "llm"},
    {"from": "tools", "to": "llm"}
  ],
  "conditional_edges": [
    {
      "from": "llm",
      "condition": "has_tool_calls",
      "routes": {"yes": "tools", "no": "__end__"}
    }
  ],
  "interrupt_before": [],
  "interrupt_after": ["tools"]
}
```

##### Barrier nodes (AND-join opt-in)

A node declaration may include a `barrier` field to opt into AND-join
semantics for that specific node. Under the default signal-dispatch
model, a node fires every super-step that any upstream routes to it
— which double-fires join nodes on asymmetric serial fan-in (paths
of different lengths). A barrier gates the node until **all** listed
upstreams have signaled at least once (across any number of
super-steps):

```json
"join": {
  "type": "my_join",
  "barrier": {"wait_for": ["a", "s2"]}
}
```

Fires once when both `a` and `s2` have signaled. State resets on
fire, so loops through the barrier collect fresh signals each round.

**Persistence:** since `CHECKPOINT_SCHEMA_VERSION = 2`, the barrier
accumulator is persisted on every checkpoint (`Checkpoint::barrier_state`,
a `map<string, set<string>>`) and restored on resume. Interrupts that
land mid-accumulation are therefore safe — the partial upstream set
survives the pause and the barrier fires as soon as the remaining
signals arrive. v1 blobs deserialize with an empty `barrier_state`,
matching pre-v2 behavior for those stored checkpoints.

#### `run`

```cpp
RunResult run(const RunConfig& config);
```

Executes the graph synchronously (blocking). Starts from `START_NODE`, follows edges
until `END_NODE` is reached or `max_steps` is exceeded.

#### `run_stream`

```cpp
RunResult run_stream(const RunConfig& config,
                     const GraphStreamCallback& cb);
```

Executes the graph with streaming events. The callback `cb` is invoked for each event
matching the `config.stream_mode` filter.

#### `resume`

```cpp
RunResult resume(const std::string& thread_id,
                 const json& resume_value = json(),
                 const GraphStreamCallback& cb = nullptr);
```

Resumes execution from a previously interrupted checkpoint (human-in-the-loop).

| Parameter | Type | Description |
|-----------|------|-------------|
| `thread_id` | `std::string` | Thread ID to resume |
| `resume_value` | `json` | Optional value to inject before resuming (e.g., human approval) |
| `cb` | `GraphStreamCallback` | Optional streaming callback. Pass `nullptr` for non-streaming resume |

#### `get_state`

```cpp
std::optional<json> get_state(const std::string& thread_id) const;
```

Returns the latest state for a thread, or `std::nullopt` if no checkpoint exists.

#### `get_state_history`

```cpp
std::vector<Checkpoint> get_state_history(const std::string& thread_id,
                                          int limit = 100) const;
```

Returns the checkpoint history for a thread, most recently saved first; the first entry is the checkpoint `get_state` reads.

#### `update_state`

```cpp
void update_state(const std::string& thread_id,
                  const json& channel_writes,
                  const std::string& as_node = "");

void update_state_writes(const std::string& thread_id,
                         const std::vector<ChannelWrite>& channel_writes,
                         const std::string& as_node = "");
```

Manually updates the state for a thread by applying channel writes. The JSON
object form applies reducer writes by channel name. The `ChannelWrite` vector
form preserves write order and explicit overwrite modes. Both create a new
checkpoint with the updated state.

| Parameter | Type | Description |
|-----------|------|-------------|
| `thread_id` | `std::string` | Target thread |
| `channel_writes` | `json` | Object of `{channel: value}` pairs to apply |
| `as_node` | `std::string` | Optional: record these writes as if from a specific node |

#### `fork`

```cpp
std::string fork(const std::string& source_thread_id,
                 const std::string& new_thread_id,
                 const std::string& checkpoint_id = "");
```

Creates a copy of a thread's state as a new thread. Useful for branching conversations
or creating what-if scenarios.

| Parameter | Type | Description |
|-----------|------|-------------|
| `source_thread_id` | `std::string` | Thread to copy from |
| `new_thread_id` | `std::string` | New thread identifier |
| `checkpoint_id` | `std::string` | Optional: fork from a specific checkpoint (default: latest) |

**Returns:** The checkpoint ID of the new forked state.

Fork copies the selected checkpoint's pending continuation as well as state; it does not invent a new turn. Resuming a completed `__end__` continuation executes no node, so editing a question on that fork does not by itself produce an answer. To continue pending work, select the exact paused checkpoint ID with real `next_nodes`, fork it, edit portable state and resume that fork. Do not generalize every historical empty-`next_nodes` latest-resume snapshot to the terminal sentinel. Example 08 retains its separate new-turn flow after a terminal fork.

Authentic native state remains under the original shared-bank scope. Fork never clones spending grants; managed-bank custody, source commitment, original ceiling and deadline still apply. A durable standalone fork is not permission to import or renew native authority.

Tool ownership is established before compilation via `NodeContext::tools` or
`EngineResources::tools`; there is no post-compilation ownership transfer.

#### `set_checkpoint_store`

```cpp
void set_checkpoint_store(std::shared_ptr<CheckpointStore> store);
```

Attaches a checkpoint store. Required for `resume()`, `get_state()`, `fork()`, and
all state inspection methods.

#### `set_store`

```cpp
void set_store(std::shared_ptr<Store> store);
```

Attaches a cross-thread shared memory store (see [Store](#9-store)).

#### `get_store`

```cpp
std::shared_ptr<Store> get_store() const;
```

Returns the attached shared memory store, or `nullptr` if none is set.

#### `set_retry_policy`

```cpp
void set_retry_policy(const RetryPolicy& policy);
```

Sets the default retry policy for all nodes. Nodes without a specific policy
will use this one.

#### `set_node_retry_policy`

```cpp
void set_node_retry_policy(const std::string& node_name, const RetryPolicy& policy);
```

Sets a retry policy for a specific node, overriding the default.

#### `get_graph_name`

```cpp
const std::string& get_graph_name() const;
```

Returns the name of the graph as specified in the definition.

---

## 7b. Engine Internals

`GraphEngine` is a thin orchestrator that delegates to four purpose-built
classes. Users typically never touch them directly — they are instantiated
inside `GraphEngine::build()` (or its `compile()` compatibility facade) and
driven from `execute_graph()` — but
they are public so advanced callers can build without JSON, drive custom
checkpoint flows, or stub pieces in tests.

| Class | Header | Responsibility |
|-------|--------|----------------|
| [`GraphCompiler`](#graphcompiler) | `<neograph/graph/compiler.h>` | Parses JSON → `CompiledGraph` |
| [`Scheduler`](#scheduler) | `<neograph/graph/scheduler.h>` | Routing decisions (signal dispatch + barriers) |
| [`CheckpointCoordinator`](#checkpointcoordinator) | `<neograph/graph/coordinator.h>` | Per-run checkpoint lifecycle |
| [`NodeExecutor`](#nodeexecutor) | `<neograph/graph/executor.h>` | Retry, parallel fan-out, Send dispatch |

### GraphCompiler

**Header:** `<neograph/graph/compiler.h>`

Pure JSON → value-type translation. No provider dispatch during compilation; the installed Core target still requires SchemaProvider runtime — the
resulting `CompiledGraph` is a movable bundle you can inspect or
construct by hand in tests.

```cpp
namespace neograph::graph {

struct ChannelDef {
    std::string  name;
    ReducerType  type = ReducerType::OVERWRITE;
    std::string  reducer_name = "overwrite";
    json         initial_value;
};

struct CompiledGraph {
    std::string name;
    std::vector<ChannelDef> channel_defs;
    std::map<std::string, std::unique_ptr<GraphNode>> nodes;
    std::vector<Edge> edges;
    std::vector<ConditionalEdge> conditional_edges;
    BarrierSpecs barrier_specs;
    std::set<std::string> interrupt_before;
    std::set<std::string> interrupt_after;
    std::optional<RetryPolicy> retry_policy;
};

class GraphCompiler {
public:
    static TopologySpec parse(const json& definition);
    static CompiledGraph link(TopologySpec topology,
                              const NodeContext& default_context);
    static CompiledGraph compile(const json& definition,
                                 const NodeContext& default_context);
};

} // namespace neograph::graph
```

`GraphCompiler::parse()` produces a `TopologySpec` without constructing nodes.
`GraphValidator::validate()` returns structured diagnostics, while
`GraphValidator::require_valid()` returns a `ValidatedTopology` or throws
`std::runtime_error`. Only `GraphCompiler::link()` resolves factories and
instantiates runtime nodes. `compile()` remains the compatibility composition of
parse and link, and `GraphEngine::build()` retains its lenient warning behavior.
New code can enforce the full boundary with `GraphEngine::build_strict()` or:

```cpp
auto spec = GraphCompiler::parse(definition);
auto validated = GraphValidator::require_valid(std::move(spec));
auto engine = GraphEngine::link(std::move(validated), config, resources);
```

### Scheduler

**Header:** `<neograph/graph/scheduler.h>`

Owns the graph topology and computes each super-step's ready set from
routing signals emitted by the previous step. No knowledge of
threading, checkpointing, retries, or HITL — those stay in the engine.

```cpp
namespace neograph::graph {

struct StepRouting {
    std::string node_name;
    std::optional<std::string> command_goto;
};

struct NextStepPlan {
    std::vector<std::string> ready;
    bool hit_end = false;
    std::optional<std::string> winning_command_goto;
};

using BarrierSpecs = std::map<std::string, std::set<std::string>>;
using BarrierState = std::map<std::string, std::set<std::string>>;

class Scheduler {
public:
    Scheduler(const std::vector<Edge>& edges,
              const std::vector<ConditionalEdge>& conditional_edges,
              BarrierSpecs barrier_specs = {});

    std::vector<std::string> plan_start_step() const;

    NextStepPlan plan_next_step(
        const std::vector<std::string>& just_ran,
        const std::vector<NodeResult>& results,
        const GraphState& state,
        BarrierState& barrier_state) const;

    std::vector<std::string> resolve_next_nodes(
        const std::string& current,
        const GraphState& state) const;

    const BarrierSpecs& barrier_specs() const;
};

} // namespace neograph::graph
```

**Semantics:**

- **Signal dispatch**: a node becomes ready in super-step S+1 iff some
  node in step S explicitly routed to it (regular edge, conditional
  edge branch, `Command::goto_node`, or Send). No static predecessor
  map — that would conflate XOR routing with AND fan-in.
- **Pairing invariant**: the caller must pass `just_ran` and `results`
  with `just_ran[i] ↔ results[i]`. Enforced by the two-argument
  overload's type signature so callers cannot desynchronize them.
- **Barriers**: nodes declared with `"barrier": {"wait_for": [...]}`
  gate on ALL listed upstreams having signaled, accumulated across
  super-steps via the mutable `BarrierState` map. Fires reset the
  entry so loops through the barrier work correctly.

### CheckpointCoordinator

**Header:** `<neograph/graph/coordinator.h>`

Per-run wrapper over `(CheckpointStore, thread_id)`. Every method is a
safe no-op when the store is null or thread_id is empty, so call sites
never need to guard.

```cpp
namespace neograph::graph {

struct ResumeContext {
    bool have_cp = false;
    std::string checkpoint_id;
    json channel_values;
    int start_step = 0;  // Phase-adjusted
    CheckpointPhase phase = CheckpointPhase::Completed;
    std::vector<std::string> next_nodes;
    std::unordered_map<std::string, NodeResult> replay_results;
    BarrierState barrier_state;
};

class CheckpointCoordinator {
public:
    CheckpointCoordinator(std::shared_ptr<CheckpointStore> store,
                          std::string thread_id);

    bool enabled() const noexcept;

    std::string save_super_step(
        const GraphState& state,
        const std::string& current_node,
        const std::vector<std::string>& next_nodes,
        CheckpointPhase phase,
        int step,
        const std::string& parent_id,
        const BarrierState& barrier_state) const;

    ResumeContext load_for_resume() const;

    void record_pending_write(
        const std::string& parent_cp_id,
        const std::string& task_id,
        const std::string& task_path,
        const std::string& node_name,
        const NodeResult& nr,
        int step) const;

    void clear_pending_writes(const std::string& parent_cp_id) const;
};

} // namespace neograph::graph
```

**Phase-aware step offset:** `load_for_resume()` reads the latest
checkpoint's `interrupt_phase` and sets `start_step` accordingly —
`Before` / `NodeInterrupt` re-enter at `cp.step`, `After` / `Completed` /
`Updated` advance by +1. The engine's resume path never repeats this
logic.

### NodeExecutor

**Header:** `<neograph/graph/executor.h>`

Owns per-super-step node invocation: retry loop, replay lookup,
pending-write recording, parallel fan-out via
`asio::experimental::make_parallel_group`, and Send dispatch. 3.0
removed the sync `run_one` / `run_parallel` / `run_sends` twins;
callers use the `_async` peers.

```cpp
namespace neograph::graph {

class NodeExecutor {
public:
    using RetryPolicyLookup = std::function<RetryPolicy(const std::string&)>;

    NodeExecutor(
        const std::map<std::string, std::unique_ptr<GraphNode>>& nodes,
        const std::vector<ChannelDef>& channel_defs,
        RetryPolicyLookup retry_policy_for,
        asio::thread_pool* fan_out_pool = nullptr);

    asio::awaitable<NodeResult> run_one_async(
        const std::string& node_name, int step,
        GraphState& state,
        const std::unordered_map<std::string, NodeResult>& replay,
        CheckpointCoordinator& coord,
        const std::string& parent_cp_id,
        const BarrierState& barrier_state,
        std::vector<std::string>& trace,
        const GraphStreamCallback& cb, StreamMode stream_mode);

    asio::awaitable<std::vector<NodeResult>> run_parallel_async(
        const std::vector<std::string>& ready, int step,
        GraphState& state,
        const std::unordered_map<std::string, NodeResult>& replay,
        CheckpointCoordinator& coord,
        const std::string& parent_cp_id,
        const BarrierState& barrier_state,
        std::vector<std::string>& trace,
        const GraphStreamCallback& cb, StreamMode stream_mode);

    asio::awaitable<void> run_sends_async(
        const std::vector<Send>& sends, int step,
        GraphState& state,
        const std::unordered_map<std::string, NodeResult>& replay,
        CheckpointCoordinator& coord,
        const std::string& parent_cp_id,
        std::vector<std::string>& trace,
        const GraphStreamCallback& cb, StreamMode stream_mode);

    asio::awaitable<NodeResult> execute_node_with_retry_async(
        const std::string& node_name,
        GraphState& state,
        const GraphStreamCallback& cb, StreamMode stream_mode);
};

} // namespace neograph::graph
```

**Invariants:**

- `run_one_async` and `run_parallel_async` both save a
  `phase=NodeInterrupt` checkpoint scoped to the interrupting node
  before rethrowing `NodeInterrupt`, so resume re-enters just that
  node (sibling writes are already in `pending_writes` and replay via
  the map).
- `run_parallel_async` applies writes + `Command.updates` in `ready`
  order so `ready[i] ↔ results[i]` pairing holds for the subsequent
  Scheduler call.
- `run_sends_async`: every Send, one or many, gives its target an
  isolated state copy (init + restore + apply input) and runs it with
  retry, so a payload is visible only to its own Send (#389).
- `fan_out_pool` (optional) determines where parallel branches
  dispatch. When null, branches run on `co_await asio::this_coro::
  executor` — fine for single-thread async callers, but CPU-bound
  fan-out serializes. When non-null, `run_parallel_async` and the
  multi-Send branch `co_spawn` onto `pool->get_executor()` for real
  thread parallelism. `GraphEngine::set_worker_count(N)` installs the
  pool for sync `run()` callers.
- `execute_node_with_retry_async` is the inner retry loop: backoff
  uses an `asio::steady_timer` so the executor isn't frozen during
  retry waits.

---

## 8. Checkpoint

**Header:** `<neograph/graph/checkpoint.h>`
**Namespace:** `neograph::graph`

Checkpointing enables persistence, time-travel debugging, and human-in-the-loop
workflows by saving and restoring graph execution state.

### Checkpoint (struct)

A serialized snapshot of graph execution state at a point in time.

```cpp
struct Checkpoint {
    std::string id;                // UUID v4
    std::string thread_id;         // Conversation/session identifier
    json        channel_values;    // Serialized channel data
    json        channel_versions;  // Not set by the engine (see below)
    std::string parent_id;         // Previous checkpoint ID (for time-travel chain)
    std::string current_node;      // Node that was active at checkpoint time
    std::vector<std::string> next_nodes;  // Nodes to execute on resume
    CheckpointPhase interrupt_phase;  // Before | After | Completed | NodeInterrupt | Updated
    std::map<std::string, std::set<std::string>> barrier_state;  // v2+: in-flight barrier accumulators
    json        metadata;          // User-defined metadata
    int64_t     step;              // Super-step number
    int64_t     timestamp;         // Unix epoch milliseconds
    std::uint32_t schema_version = CHECKPOINT_SCHEMA_VERSION;  // Layout version

    static std::string generate_id();  // Generate UUID v4
};

// Wire-stable schema version. Bump on layout-incompatible changes.
// v2 added `barrier_state`; v3 records pending-write mode support.
// Typed `uint32_t`: schema versions are non-negative wire values.
constexpr std::uint32_t CHECKPOINT_SCHEMA_VERSION = 3;
```

| Field | Type | Description |
|-------|------|-------------|
| `id` | `std::string` | Unique identifier (UUID v4) |
| `thread_id` | `std::string` | Groups checkpoints by conversation/session |
| `channel_values` | `json` | Serialized state of all channels |
| `channel_versions` | `json` | The engine does not set it. Its checkpoints hold each channel's version at `channel_values["channels"][name]["version"]` and the shared write counter at `channel_values["global_version"]` |
| `parent_id` | `std::string` | ID of the preceding checkpoint (forms a linked list for time-travel) |
| `current_node` | `std::string` | Node that was executing when the checkpoint was taken |
| `next_nodes` | `std::vector<std::string>` | All nodes scheduled for the next super-step (used by `resume()`). Under signal dispatch a super-step can leave several nodes simultaneously ready (parallel fan-out, conditional branches activating together), and every one of them must be persisted — storing a single node would silently drop siblings across a crash |
| `interrupt_phase` | `CheckpointPhase` | Enum: `Before` (interrupt_before fired), `After` (interrupt_after fired), `Completed` (normal super-step cadence), `NodeInterrupt` (node threw `NodeInterrupt` mid-execution), `Updated` (external `update_state()` injection). `to_string()` and `parse_checkpoint_phase()` give a stable wire/log encoding |
| `barrier_state` | `map<string, set<string>>` | Per-barrier accumulator of upstreams that have signaled so far. Entries only exist for barriers that are in-flight (not yet fired) — the Scheduler clears an entry when its barrier fires. Shape matches `BarrierState` from `scheduler.h`. Present since schema v2; v1 blobs deserialize with an empty map, which matches their pre-v2 behavior |
| `metadata` | `json` | Arbitrary user-defined data |
| `step` | `int64_t` | Super-step counter |
| `timestamp` | `int64_t` | Creation time in Unix epoch milliseconds |
| `schema_version` | `std::uint32_t` | On-wire layout version (see `CHECKPOINT_SCHEMA_VERSION`, currently `3`). It is a fixed-width unsigned type because a schema version is non-negative and is persisted to disk and round-tripped through JSON. Persistent `CheckpointStore` implementations should serialize it and treat `0` on a deserialized blob as "pre-versioned" (e.g. the field was absent — migration is the caller's responsibility) |

### CheckpointStore

The legacy ABI-compatible persistence facade. New sync-only backends derive
`CheckpointStoreCore` (five pure operations) and call
`adapt_checkpoint_store()`; native async backends derive
`AsyncCheckpointStore` (five pure coroutine operations) and call
`adapt_async_checkpoint_store()`. Both adapters expose `CheckpointStore`
to existing GraphEngine, protocol hosts, gRPC checkpoint users, and Python
binding entry points. Async engine operations call the canonical async peers;
sync-only backends are offloaded to a bounded pool, while native async
operations run on the caller's executor. The legacy synchronous defaults
throw on missing capabilities rather than recursing. Durable pending writes
are an independent optional `PendingWritesCheckpointStore` capability; without
it, resume replays the full super-step. The persisted schema is unchanged.
See [`ASYNC_GUIDE.md` §9.4](ASYNC_GUIDE.md#94-checkpointstore).

Python `CheckpointStore.requires_managed_budget(thread_id) -> bool` is a
synchronous virtual reader of persisted managed-bank denial obligations. The
engine's native `requires_managed_budget_async(thread_id)` facade offloads the
synchronous call; the binding forwards a Python override under the GIL. Without
an override, the native implementation raises an explicit unsupported-backend
error instead of returning `False`. The reader reports whether the trusted
namespace/thread has ever held an active standalone managed bank; saving
stripped state or deleting checkpoints must not clear that obligation.
Implementations must report it truthfully. This reader grants no spending,
restoration, bank, or lease authority; bounded execution still requires supported
native managed-budget leases.

```cpp
class CheckpointStore {
public:
    virtual ~CheckpointStore() = default;

    // ── Sync facade (5 virtuals; missing operation throws) ──────
    virtual void save(const Checkpoint& cp);
    virtual std::optional<Checkpoint> load_latest(const std::string& thread_id);
    virtual std::optional<Checkpoint> load_by_id(const std::string& id);
    virtual std::vector<Checkpoint>   list(const std::string& thread_id,
                                           int limit = 100);
    virtual void delete_thread(const std::string& thread_id);

    // ── Async peers (5 virtuals; sync-only operations offload) ──
    virtual asio::awaitable<void> save_async(const Checkpoint& cp);
    virtual asio::awaitable<std::optional<Checkpoint>>
        load_latest_async(const std::string& thread_id);
    virtual asio::awaitable<std::optional<Checkpoint>>
        load_by_id_async(const std::string& id);
    virtual asio::awaitable<std::vector<Checkpoint>>
        list_async(const std::string& thread_id, int limit = 100);
    virtual asio::awaitable<void>
        delete_thread_async(const std::string& thread_id);

    // ── Pending writes — fine-grained super-step progress log ──────
    //
    // Default no-ops: backends that don't support per-node durable
    // writes fall back to "full super-step replay" on resume.
    virtual void put_writes(const std::string& thread_id,
                            const std::string& parent_checkpoint_id,
                            const PendingWrite& write) {}
    virtual std::vector<PendingWrite> get_writes(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id) { return {}; }
    virtual void clear_writes(const std::string& thread_id,
                              const std::string& parent_checkpoint_id) {}

    // ── Async pending-writes peers (default-bridge to sync) ────────
    virtual asio::awaitable<void> put_writes_async(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id,
        const PendingWrite& write);
    virtual asio::awaitable<std::vector<PendingWrite>> get_writes_async(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id);
    virtual asio::awaitable<void> clear_writes_async(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id);
};
```

| Method | Description |
|--------|-------------|
| `save(cp)` / `save_async(cp)` | Persist a checkpoint. Engine writes one per super-step. |
| `load_latest(thread_id)` / `_async` | Load the most recent checkpoint for a thread. |
| `load_by_id(id)` / `_async` | Load a specific checkpoint by UUID (time-travel). |
| `list(thread_id, limit)` / `_async` | List checkpoints for a thread, newest first, up to `limit`. |
| `delete_thread(thread_id)` / `_async` | Delete all checkpoints for a thread. |
| `put_writes(thread_id, parent_cp, write)` / `_async` | Record a successful node execution mid-super-step. Engine calls this immediately after a node returns and *before* its writes apply to GraphState. Default no-op. |
| `get_writes(thread_id, parent_cp)` / `_async` | Load pending writes attached to a parent checkpoint. Engine calls this on resume to skip already-completed tasks. Default empty. |
| `clear_writes(thread_id, parent_cp)` / `_async` | Discard pending writes after the successor super-step's checkpoint has been durably saved. Default no-op. |

### InMemoryCheckpointStore

Thread-safe in-memory implementation suitable for testing and single-process applications.

```cpp
class InMemoryCheckpointStore : public CheckpointStore {
public:
    void save(const Checkpoint& cp) override;
    std::optional<Checkpoint> load_latest(const std::string& thread_id) override;
    std::optional<Checkpoint> load_by_id(const std::string& id) override;
    std::vector<Checkpoint> list(const std::string& thread_id,
                                  int limit = 100) override;
    void delete_thread(const std::string& thread_id) override;

    size_t size() const;  // Total number of stored checkpoints
};
```

---

## 9. Store

**Header:** `<neograph/graph/store.h>`
**Namespace:** `neograph::graph`

Cross-thread shared memory store. Provides namespaced key-value storage that persists
across threads and graph executions. Use cases include long-term user preferences,
shared knowledge bases, and agent memory.

### Namespace

A hierarchical path represented as a vector of strings.

```cpp
using Namespace = std::vector<std::string>;
```

Example: `{"users", "user123", "preferences"}` represents the path `users/user123/preferences`.

### StoreItem

A single item in the store.

```cpp
struct StoreItem {
    Namespace   ns;          // Namespace path
    std::string key;         // Item key within the namespace
    json        value;       // Stored value
    int64_t     created_at;  // Creation timestamp (Unix epoch millis)
    int64_t     updated_at;  // Last update timestamp (Unix epoch millis)
};
```

### Store (abstract)

Abstract interface for cross-thread shared memory.

```cpp
class Store {
public:
    virtual ~Store() = default;

    // Put a value (create or update)
    virtual void put(const Namespace& ns, const std::string& key,
                     const json& value) = 0;

    // Get a single item
    virtual std::optional<StoreItem> get(const Namespace& ns,
                                         const std::string& key) const = 0;

    // Search items under a namespace prefix
    virtual std::vector<StoreItem> search(const Namespace& ns_prefix,
                                           int limit = 100) const = 0;

    // Delete an item
    virtual void delete_item(const Namespace& ns, const std::string& key) = 0;

    // List namespaces under a prefix
    virtual std::vector<Namespace> list_namespaces(
        const Namespace& prefix = {}) const = 0;
};
```

| Method | Description |
|--------|-------------|
| `put(ns, key, value)` | Insert or update a value. Updates `updated_at` if the item already exists |
| `get(ns, key)` | Retrieve a single item. Returns `std::nullopt` if not found |
| `search(ns_prefix, limit)` | Find all items whose namespace starts with the given prefix |
| `delete_item(ns, key)` | Remove an item from the store |
| `list_namespaces(prefix)` | List all unique namespaces that start with the given prefix |

### InMemoryStore

Thread-safe in-memory implementation for testing and single-process use.

```cpp
class InMemoryStore : public Store {
public:
    void put(const Namespace& ns, const std::string& key,
             const json& value) override;
    std::optional<StoreItem> get(const Namespace& ns,
                                 const std::string& key) const override;
    std::vector<StoreItem> search(const Namespace& ns_prefix,
                                   int limit = 100) const override;
    void delete_item(const Namespace& ns, const std::string& key) override;
    std::vector<Namespace> list_namespaces(
        const Namespace& prefix = {}) const override;

    size_t size() const;  // Total number of stored items
};
```

---

## 10. Loader

**Header:** `<neograph/graph/loader.h>`
**Namespace:** `neograph::graph`

`ReducerRegistry`, `ConditionRegistry`, and `NodeFactory` are synchronized
legacy process-wide convenience APIs. New engines should register on
`GraphRegistry` and pass it through `EngineResources`: built-ins resolve by
default, but custom process-global names require explicit `GlobalFallback`.
Legacy no-registry calls still recognize the singleton registrations.

### ReducerRegistry

Singleton registry mapping string names to `ReducerFn` implementations.

```cpp
class ReducerRegistry {
public:
    static ReducerRegistry& instance();

    void register_reducer(const std::string& name, ReducerFn fn);
    ReducerFn get(const std::string& name) const;
    std::vector<std::string> names() const;
};
```

| Method | Description |
|--------|-------------|
| `instance()` | Returns the singleton instance |
| `register_reducer(name, fn)` | Registers a custom reducer function |
| `get(name)` | Looks up a reducer by name. Throws if not found |
| `names()` | Sorted list of all registered reducer names (introspection for external tooling) |

### ConditionRegistry

Singleton registry mapping string names to `ConditionFn` implementations.

```cpp
class ConditionRegistry {
public:
    static ConditionRegistry& instance();

    void register_condition(const std::string& name, ConditionFn fn);
    ConditionFn get(const std::string& name) const;
    std::vector<std::string> names() const;
};
```

| Method | Description |
|--------|-------------|
| `instance()` | Returns the singleton instance |
| `register_condition(name, fn)` | Registers a custom condition function |
| `get(name)` | Looks up a condition by name. Throws if not found |
| `names()` | Sorted list of all registered condition names (introspection for external tooling) |

### NodeFactory

Singleton factory for creating `GraphNode` instances from JSON configuration.

```cpp
using NodeFactoryFn = std::function<std::unique_ptr<GraphNode>(
    const std::string& name,
    const json& config,
    const NodeContext& ctx)>;

class NodeFactory {
public:
    static NodeFactory& instance();

    void register_type(const std::string& type, NodeFactoryFn fn);
    void register_type(const std::string& type, NodeFactoryFn fn,
                       json config_schema);
    std::unique_ptr<GraphNode> create(const std::string& type,
                                       const std::string& name,
                                       const json& config,
                                       const NodeContext& ctx) const;
    std::vector<std::string> registered_types() const;
    json export_schema() const;
};
```

| Method | Description |
|--------|-------------|
| `instance()` | Returns the singleton instance |
| `register_type(type, fn)` | Registers a node factory. Config schema defaults to a permissive `{"type":"object"}` |
| `register_type(type, fn, config_schema)` | As above, with a declared JSON Schema (Draft 2020-12) for the node's `config`. Additive — the 2-arg overload still works unchanged. Used only by `export_schema()`; the engine does not validate config against it |
| `create(type, name, config, ctx)` | Creates a node of the given type. Throws if the type is not registered |
| `registered_types()` | Sorted list of all registered node type names |
| `export_schema()` | Machine-readable description of the topology JSON this engine accepts (see [Topology Schema Export](#topology-schema-export-issue-56)) |

### Built-in Registrations

The library pre-registers the following components:

**Reducers:**

| Name | Behavior |
|------|----------|
| `"overwrite"` | Replaces the current value with the incoming value |
| `"append"` | Appends the incoming value to the current array. If the incoming value is an array, its elements are concatenated |

**Conditions:**

| Name | Behavior |
|------|----------|
| `"has_tool_calls"` | Inspects the last message in the `"messages"` channel. Returns `"yes"` if it contains tool calls, `"no"` otherwise |
| `"route_channel"` | Reads the `"__route__"` channel and returns its string value. Used with `IntentClassifierNode` |

**Node types:**

| Type | Class | Description |
|------|-------|-------------|
| `"llm_call"` | `LLMCallNode` | Calls the LLM with current conversation state |
| `"tool_dispatch"` | `ToolDispatchNode` | Dispatches tool calls from the latest assistant message |
| `"intent_classifier"` | `IntentClassifierNode` | LLM-based intent classification. Reads `prompt` and `valid_routes` from `config` |
| `"subgraph"` | `SubgraphNode` | Runs a compiled subgraph. Reads `input_map` and `output_map` from `config` |

### Topology Schema Export (issue #56)

NeoGraph runs a graph that is *described in JSON*; swap the JSON and the
same engine becomes a different harness. `NodeFactory::export_schema()`
emits a machine-readable description of exactly what topology JSON this
engine version accepts, so external tooling — notably a code-free
visual block editor (NeoGraph Studio, a private companion repo,
issue #56) — can generate its palette from the engine and never drift
out of sync.

**Export the registry used for compilation:**

| From | How |
|------|-----|
| C++ scoped | `registry.export_effective_schema()` (built-ins plus scoped names); `registry.export_schema()` remains exact local-only for sealed Program palettes |
| C++ legacy | `NodeFactory::instance().export_schema()` |
| CLI legacy | `./example_export_schema > schema.json` (`examples/52_export_schema.cpp`) |
| Python scoped | `ng.export_schema(registry)` or `registry.export_schema()` |
| Python legacy | `ng.export_schema()` |

Python: register on `ng.GraphRegistry()` with `register_type`,
`register_reducer`, or `register_condition`; pass it as
`ng.GraphEngine.compile(definition, context, registry=registry)`. Re-registering
the same symbolic name on a different registry never changes the first engine.

**Document shape:**

```jsonc
{
  "neograph_version": "0.9.0",
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "topology":   { /* JSON Schema for the top-level envelope:
                     name, channels, nodes (type + config + barrier),
                     edges, conditional_edges, interrupt_before,
                     interrupt_after, retry_policy */ },
  "node_types": { "<type>": { /* config JSON Schema */ }, ... },
  "reducers":   ["append", "overwrite", ...],
  "conditions": ["has_tool_calls", "route_channel", ...]
}
```

- **`neograph_version`** is stamped at compile time from
  `pyproject.toml` (single source of truth). A tool compares it to its
  cached schema and warns when its palette is older than the engine.
- **`node_types`** reflects whatever is registered in `NodeFactory` at
  call time, so an embedder's custom node types appear too — register
  them (and any custom reducers/conditions) *before* exporting, exactly
  as you would before `compile()`. A type registered via the 3-arg
  `register_type` carries its declared config schema; the 2-arg form
  yields a permissive `{"type":"object"}`.
- **Round-trip contract.** A tool that emits topology JSON should
  round-trip it through the loader and assert structure is preserved.
  In particular the top-level `conditional_edges` block was silently
  dropped by the compiler in v0.1.0–v0.1.7 (fixed v0.1.8); the engine
  test suite (`tests/test_schema_export.cpp`) guards this regression,
  and tooling should too.

```cpp
#include <neograph/graph/loader.h>
// register custom node types first if you want them in the palette …
auto schema = neograph::graph::NodeFactory::instance().export_schema();
std::cout << schema.dump(2) << "\n";
```

---

## 10.5. Observability — OpenTelemetry + OpenInference

Python graph tracing lives in `neograph_engine.tracing` and `neograph_engine.openinference`. `otel_tracer` emits a run span and node spans from graph events; `openinference_tracer` tags those spans as `CHAIN` and records node payload projections. `neograph_engine.openinference.OpenInferenceProvider` wraps an existing typed provider with the native C++ dispatch observer and sends per-call `LLM` spans to a Python OpenTelemetry tracer. C++ callers use `<neograph/observability/openinference.h>`.

### `otel_tracer` — OTel-shape spans

The signature below is a reference declaration. `root_name` defaults to `graph.run`, `node_span_prefix` to `node.`, and `attribute_prefix` to `neograph`. `on_event` optionally forwards graph events to another consumer.

```python
@contextmanager
def otel_tracer(
    tracer: Any,
    *,
    root_name: str = "graph.run",
    node_span_prefix: str = "node.",
    attribute_prefix: str = "neograph",
    on_event: Optional[Callable[[Any], None]] = None,
) -> Iterator[Callable[[Any], None]]:
    ...
```

`NODE_START` opens a span; `NODE_END` closes it successfully; `ERROR` records an error; `INTERRUPT` marks a pause. Each node name has a stack for overlapping events. Context-manager cleanup closes remaining spans when the run exits. A trace does not establish exactly-once node execution or correlate every concurrent task uniquely.

```python
from opentelemetry import trace
from neograph_engine.tracing import otel_tracer

tracer = trace.get_tracer("my-service")
with otel_tracer(tracer) as cb:
    engine.run_stream(cfg, cb)
```

### `openinference_tracer` — adds LLM-shape attributes

Graph spans carry `openinference.span.kind = "CHAIN"`; node input/output payloads become JSON `input.value` / `output.value` projections. Python graph tracing alone does not create per-provider `LLM` spans or attribute vendor charges. Context attachment is scoped to the originating Python context; cross-thread/task parent propagation needs the tracing integration to preserve that context.

```python
@contextmanager
def openinference_tracer(
    tracer: Any,
    *,
    root_name: str = "graph.run",
    node_span_prefix: str = "node.",
    on_event: Optional[Callable[[Any], None]] = None,
) -> Iterator[Callable[[Any], None]]:
    ...
```

### `OpenInferenceProvider` — Python and C++ typed dispatch observer

Construct the Python wrapper as `OpenInferenceProvider(inner, tracer, *, span_name="llm.complete")`. It inherits `prepare(request)`, one-shot `dispatch(prepared)` and `invoke(request)` from `Provider`; it does not add completion APIs. The native wrapper delegates preparation exactly once and observes the same owned handle at dispatch. Invalid or discarded preparation opens no span. Each admitted dispatch opens one LLM span under normal tracing operation; original outcomes, mode, deadline, cancellation, events and provider identity pass through. Tracing failures do not replace a provider outcome or exception.

The following functions show alternative call paths. `inner` is an existing typed provider, such as a configured `SchemaProvider`; `model` is explicit. Choose one function for one model call.

```python
from neograph_engine import ProviderMessage, ProviderRole, Text, make_provider_request
from neograph_engine.openinference import OpenInferenceProvider


def traced_dispatch(inner, model, tracer):
    observed = OpenInferenceProvider(inner, tracer)
    request = make_provider_request(observed, model, [
        ProviderMessage(ProviderRole.User, [Text("Say hello.")])])
    prepared = observed.prepare(request)
    return observed.dispatch(prepared)


def traced_invoke(inner, model, tracer):
    observed = OpenInferenceProvider(inner, tracer)
    request = make_provider_request(observed, model, [
        ProviderMessage(ProviderRole.User, [Text("Say hello.")])])
    return observed.invoke(request)
```

Python `invoke`/`dispatch` release the GIL; the tracer adapter reacquires it for Python calls and reference destruction. The prepared operation retains that adapter and its tracer even if the wrapper is collected before dispatch. Parentage follows the active OpenTelemetry context at dispatch, not preparation; propagate that context when moving work between threads/tasks. Install `opentelemetry-api` before constructing the wrapper.

In C++, the session overload safely attaches a parent across session teardown; a raw parent lookup requires the caller to keep that parent alive. The host-owned tracer must outlive all operations:

```cpp
#include <neograph/observability/openinference.h>

// tracer is a host-owned neograph::observability::Tracer adapter.
// Its lifetime must cover the session and every provider operation.
auto session = neograph::observability::openinference_tracer(tracer);
auto observed = std::make_shared<neograph::observability::OpenInferenceProvider>(
    inner_provider, tracer, session);
// Use observed in NodeContext before compiling the graph.
```

Native LLM attributes contain only public role/text projections, declared scalars and known counts. Native replay blocks, reasoning, raw wire envelopes/events and `PreparedProviderRequest.encoded_body` are excluded from trace payloads; authentic custody stays with the request and outcome. Known zero is recorded; unknown is omitted. Counts above the signed span range are encoded as decimal strings. Visible text deltas emit `llm.token` events with Python OTel attributes `{"chunk": text}`. Completion sets OK; failure sets ERROR with the safe provider message, and dispatch exceptions retain their original product error while the span records an error. Public prompts, outputs and exception messages can still contain application secrets; control what reaches the exporter.

| Attribute | Native source |
|---|---|
| `openinference.span.kind` | `"LLM"` |
| `llm.model_name` | Admitted prepared model |
| `llm.invocation_parameters` | Declared temperature and output cap when available |
| `llm.input_messages.{i}.message.role` | Public role projection |
| `llm.input_messages.{i}.message.content` | Public text parts |
| `input.value` / `input.mime_type` | Public message JSON / `application/json` |
| `llm.output_messages.{i}.message.role` | Every returned message's role |
| `llm.output_messages.{i}.message.content` | Public text parts |
| `output.value` / `output.mime_type` | Concatenated public text / `text/plain` |
| `llm.token_count.prompt` | Present `usage.input_total.value` |
| `llm.token_count.completion` | Present `usage.output_total.value` |
| `llm.token_count.total` | Present `usage.total.value` |

Token attributes report provider usage, including available partial usage on failure. They do not establish a vendor charge or restore budget authority. Use `UsageAccumulator.authority_snapshot()` / Program's `provider_budget_authority` for charged and reserved accounting; keep those separate from nullable usage reports.

### End-to-end: NeoGraph + Phoenix in one block

Run Phoenix, then pass a graph specification, an existing typed provider, an explicit model and a `RunConfig` to `trace_graph`. The helper installs the wrapper before compiling the graph and uses the same tracer for graph/node `CHAIN` spans and provider `LLM` spans. Only executed provider calls produce LLM spans; trace counts remain usage reports rather than charged accounting.

```bash
docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix:latest
pip install neograph-engine opentelemetry-api opentelemetry-sdk opentelemetry-exporter-otlp
```

```python
from opentelemetry import context as otel_context
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import BatchSpanProcessor
from opentelemetry.exporter.otlp.proto.grpc.trace_exporter import OTLPSpanExporter
from neograph_engine import GraphEngine, NodeContext
from neograph_engine.openinference import OpenInferenceProvider, openinference_tracer


class ParentContextTracer:
    def __init__(self, tracer, parent_context):
        self.tracer, self.parent_context = tracer, parent_context

    def start_span(self, name):
        return self.tracer.start_span(name, context=self.parent_context)


def trace_graph(graph_spec, inner_provider, model, cfg):
    provider = TracerProvider()
    provider.add_span_processor(BatchSpanProcessor(
        OTLPSpanExporter(endpoint="http://localhost:4317", insecure=True)))
    tracer = provider.get_tracer("my-app")
    try:
        with openinference_tracer(tracer) as cb:
            parent = ParentContextTracer(tracer, otel_context.get_current())
            observed = OpenInferenceProvider(inner_provider, parent)
            engine = GraphEngine.compile(
                graph_spec, NodeContext(provider=observed, model=model))
            return engine.run_stream(cfg, cb)
    finally:
        provider.shutdown()
```

`ParentContextTracer` explicitly carries the caller's run context into provider dispatch on graph workers. It parents LLM spans to the run root; it does not promise node-level ancestry for every concurrent task. The [OpenInference conventions](https://github.com/Arize-ai/openinference/blob/main/spec/semantic_conventions.md) define `CHAIN` and `LLM`; NeoGraph exports the subset above. Follow [OpenTelemetry's sensitive-data guidance](https://opentelemetry.io/docs/security/handling-sensitive-data/) when choosing or redacting public-text and graph payloads.

### Notes

OpenTelemetry is opt-in. Install its API/SDK/exporter separately; the base wheel does not require them. Use either `otel_tracer` or `openinference_tracer` as the graph callback, not both for the same run. An OTLP endpoint and credentials must match your backend configuration; swapping only a URL is not a universal backend-compatibility guarantee.

---

## 11. React Graph

**Header:** `<neograph/graph/react_graph.h>`
**Namespace:** `neograph::graph`

Convenience function that creates a standard ReAct (Reason + Act) agent as a two-node
graph: `llm_call -> tool_dispatch -> (loop back if tool calls, else end)`.

```cpp
std::unique_ptr<GraphEngine> create_react_graph(
    std::shared_ptr<Provider> provider,
    std::vector<std::unique_ptr<Tool>> tools,
    const std::string& instructions = "",
    const std::string& model = "");
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `provider` | `std::shared_ptr<Provider>` | LLM provider |
| `tools` | `std::vector<std::unique_ptr<Tool>>` | Tools available to the agent (ownership transferred) |
| `instructions` | `std::string` | System prompt / instructions |
| `model` | `std::string` | Explicit model name; the typed provider does not choose a default model |

**Returns:** A compiled `GraphEngine` ready to run.

This is functionally equivalent to using `Agent::run()` but as a graph engine, giving
you access to checkpointing, streaming events, state inspection, and all other graph
engine features.

---

## 11b. Plan-and-Execute Graph

**Header:** `<neograph/graph/plan_execute_graph.h>`
**Namespace:** `neograph::graph`

Convenience factory for the Plan-and-Execute pattern: a planner emits a JSON
array of steps, an executor consumes them one-by-one via an inner ReAct loop,
and a responder composes the final answer from `past_steps`.

```
__start__ → planner → [plan_empty? responder : executor]
                      executor → [plan_empty? responder : executor]
                      responder → __end__
```

```cpp
std::unique_ptr<GraphEngine> create_plan_execute_graph(
    std::shared_ptr<Provider> provider,
    std::vector<std::unique_ptr<Tool>> tools,
    const std::string& planner_prompt,
    const std::string& executor_prompt,
    const std::string& responder_prompt,
    const std::string& model = "",
    int max_step_iterations = 5);
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `provider` | `std::shared_ptr<Provider>` | LLM provider shared by every phase |
| `tools` | `std::vector<std::unique_ptr<Tool>>` | Tools the executor may invoke (ownership transferred) |
| `planner_prompt` | `std::string` | System prompt for the planner; must instruct the model to reply with a JSON array of steps (fenced ```json blocks and leading prose are tolerated) |
| `executor_prompt` | `std::string` | System prompt for the single-step executor (inner ReAct loop) |
| `responder_prompt` | `std::string` | System prompt for the final synthesis phase |
| `model` | `std::string` | Explicit model name; the typed provider does not choose a default model |
| `max_step_iterations` | `int` | Upper bound on tool-call iterations inside the executor per step |

**Channels populated:** `plan`, `past_steps`, `final_response`, `messages`.

**Returns:** A compiled `GraphEngine` ready to run. The factory registers its
three custom node types and the `plan_empty` condition on first call
(idempotent via `std::call_once`).

See `examples/14_plan_executor.cpp` for a Send-fan-out variant with crash /
resume via pending-writes.

---

## 12. LLM Module

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

### Agent

`Agent::run`, `run_stream` and `complete` return the full `sp::runtime::Result` and accept `std::vector<sp::Message>`. `run_stream` receives typed events on every actual turn; it does not discard and resend an answer for display. `outcomes()` retains individual results and `usage()` exposes the nullable report. A model is chosen explicitly by the caller.

```cpp
#include <neograph/llm/agent.h>
#include <neograph/llm/schema_provider.h>

sp::runtime::Result run_agent(
    sp::descriptor::ValidatedDescriptor descriptor, sp::runtime::Options options,
    std::string model, std::vector<std::unique_ptr<neograph::Tool>> tools,
    std::vector<sp::Message>& history) {
    auto provider = std::make_shared<neograph::llm::SchemaProvider>(
        std::move(descriptor), std::move(options));
    neograph::llm::Agent agent(provider, std::move(tools), "", model);
    return agent.run(history);
}
```

## 13. MCP Module

**Header:** `<neograph/mcp/client.h>`
**Namespace:** `neograph::mcp`

Model Context Protocol (MCP) client implementation. Connects to MCP servers, discovers
available tools, and wraps them as NeoGraph `Tool` instances.

Two transports are available:

- **HTTP** — `MCPClient("http://host:port/mcp")`. The URL is the server's MCP
  endpoint and is used as given, path and query included; nothing is appended
  (an older client appended `/mcp`). Discovered tools retain the originating
  Streamable HTTP session, including `Mcp-Session-Id`, negotiated protocol
  version, timeout, and custom headers.
- **stdio** — `MCPClient({"python", "server.py"})`. The client resolves `PATH`
  before `fork`, executes the subprocess with `execve`, wires bidirectional
  pipes, and exchanges newline-delimited JSON-RPC
  over the child's stdin/stdout. The subprocess lives as long as the
  `MCPClient` *or any `MCPTool`* it produced. Shutting it down closes its stdin
  first and waits up to one second for it to exit, then sends SIGTERM to its
  process group, waits up to another second, and then sends SIGKILL.

### MCPTool

Wraps a single MCP server tool as a local `Tool` implementation. Discovered
tools retain their originating protocol session, regardless of transport.

```cpp
class MCPTool : public AsyncTool, public ContextualAsyncTool {
public:
    // One known tool at an HTTP endpoint. Discovered tools reuse their client session.
    MCPTool(const std::string& server_url,
            const std::string& name,
            const std::string& description,
            const json& input_schema);

    const ToolDefinition& get_mcp_definition() const noexcept;
    CallToolResult execute_result(const json& arguments);
    asio::awaitable<CallToolResult> execute_result_async(const json& arguments);
    asio::awaitable<CallToolResult> execute_result_async(const json& arguments,
                                                         ToolExecutionContext context);
    ChatTool get_definition() const override;
    asio::awaitable<std::string> execute_async(const json& arguments) override;
    asio::awaitable<std::string> execute_async(const json& arguments,
                                               ToolExecutionContext context) override;
    std::string get_name() const override;
};
```

Usually you do not construct `MCPTool` directly — `MCPClient::get_tools()`
discovers and wraps them. A tool built with the URL constructor opens one MCP
session on its first call, reuses it for every later call, and ends it with
`DELETE` when the tool is destroyed.

Every dispatcher calls the context-aware `execute_async`, so the run's cancel
token (a node attempt's token under an attempt timeout) and deadline bound the
call on both transports. Cancelling stops waiting at once and throws
`graph::CancelledException`, which the tool failure contract treats as
cancellation; the deadline or the request timeout throws
`asio::system_error(asio::error::timed_out)`, a `timed_out` call. Either way the
server is told with `notifications/cancelled`, also when the run cancels the
coroutine that awaits the call. The response that arrives later is dropped:
stdio routes responses by JSON-RPC id and the call's id is no longer awaited,
and HTTP abandons the request. An `isError` result throws `std::runtime_error`,
an ordinary tool failure. In Python, `mcp.MCPError` keeps the JSON-RPC error's
`code`, `data` and `http_status`.

### MCPClient

Client that connects to an MCP server, performs the initialization handshake, and
provides methods to discover and invoke tools.

> `MCPClient` is not designed to be subclassed — you use it as-is.
> `rpc_call_async()` is the one transport-level call; the synchronous methods
> drive the same session. See [`ASYNC_GUIDE.md` §9.5](ASYNC_GUIDE.md#95-mcpclient).

```cpp
class MCPClient {
public:
    // HTTP transport: the server's MCP endpoint URL, used as given.
    explicit MCPClient(const std::string& server_url);
    MCPClient(const std::string& server_url, MCPClientConfig config);

    // stdio transport — fork+exec the subprocess.
    explicit MCPClient(std::vector<std::string> argv);
    // Hardened stdio: explicit environment, cwd, bounds, cancellation grace.
    explicit MCPClient(StdioClientConfig config);

    bool initialize(const std::string& client_name = "neograph");
    bool initialize(const std::string& client_name,
                    std::chrono::steady_clock::time_point deadline,
                    std::shared_ptr<graph::CancelToken> cancel_token);
    bool is_initialized() const noexcept;
    InitializeResult get_initialize_result() const;
    std::vector<std::unique_ptr<Tool>> get_tools();
    ListToolsPage list_tools(std::optional<std::string> cursor = std::nullopt);
    std::vector<ToolDefinition> get_tool_definitions();
    json call_tool(const std::string& name, const json& arguments);
    CallToolResult call_tool_result(const std::string& name,
                                    const json& arguments);
    asio::awaitable<CallToolResult> call_tool_result_async(
        const std::string& name, const json& arguments,
        std::chrono::steady_clock::time_point deadline,
        std::shared_ptr<graph::CancelToken> cancel_token);
    void set_tool_list_changed_handler(std::function<void()> handler);
    void shutdown() noexcept;

    // The transport-level JSON-RPC call every other method uses.
    asio::awaitable<json> rpc_call_async(
        const std::string& method, const json& params = json::object(),
        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::time_point::max(),
        std::shared_ptr<graph::CancelToken> cancel_token = {});
};
```

**Wire protocol:** NeoGraph's MCP client offers `protocolVersion =
"2025-11-25"` and, as the official SDK clients do, accepts a server that
answers `2025-11-25`, `2025-06-18`, `2025-03-26` or `2024-11-05`
(`MCP_SUPPORTED_PROTOCOL_VERSIONS`). A `2024-11-05` server is reached over stdio or
Streamable HTTP; the older HTTP+SSE transport is not spoken. Any other answer fails
`initialize` with a protocol error naming both and the client disconnects, as
MCP's version negotiation asks. HTTP sends the negotiated version in the
`MCP-Protocol-Version` header of every later request. MCP `2026-07-28` (no
`initialize`, no sessions) is not spoken yet: a server that serves only that
revision refuses the handshake with `-32022`, which reaches the caller as
`MCPError`.

**Session and transport ownership:** Both constructors create the same protocol
session. It owns JSON-RPC ids, response validation, initialization (including
the initialized notification), pagination, and tool-result adaptation. A
request sent with `rpc_call_async()` on a client that has not shaken hands yet
performs the handshake first. The HTTP transport alone owns the endpoint URL,
headers, the negotiated `Mcp-Session-Id`, and the request timeout; the stdio
transport alone owns the subprocess, pipes, reader, and response-id
demultiplexer. A discovered tool retains its protocol session, so it remains
usable after the `MCPClient` is destroyed; the session ends when the last
client or tool reference is released.

**Following the transports:** Both transports answer a server's requests: a
`ping` gets an empty result, and every other request gets `-32601`, because the
client declares no client capability (no roots, sampling or elicitation). A
message counts as a response only when it has no `method`, so a server request
that reuses the id of a pending call never completes that call. A JSON-RPC
batch from the server (MCP `2025-03-26`) is handled message by message, and the
replies to the requests in it go back as one array. Over HTTP the
client reads JSON and Server-Sent Event responses (by the WHATWG rules: CRLF,
LF or CR line ends, multi-line `data`, `id`, `retry`), answers a server request
that arrives on a call's event stream in its own POST while that stream stays
open, and stops reading a stream once its response arrived. A stream the server
closes before the response is resumed with `GET` and `Last-Event-ID` after the
stream's `retry` interval. After `initialize` the client opens the optional
`GET` event stream of its session for messages that belong to no call, as the
official clients do; a server without one answers 405, which the client takes
as no stream, not as a failure: it does not ask again, and the session and its
calls go on. When the server answers
404 for a request that carried the session id, the session has ended: the
client initializes a new one and sends the request again, once. `shutdown()`
and the release of the last reference end the session with `DELETE`, bounded
to two seconds.

**Notifications:** `set_tool_list_changed_handler()` is called when the server
sends `notifications/tools/list_changed`; read `get_tools()` or
`get_tool_definitions()` again then. Until that, `outputSchema` checks are
skipped, as the cached schemas may be stale. The handler runs on the client's
transport thread: it must return promptly and must not call the client.

**Hardened stdio (`StdioClientConfig`):** `argv`, `cwd` and `environment` are
explicit. With `replace_environment` the configured entries are the server's
whole environment; without it, the server inherits this process's environment
and the configured entries add to or override it (a `PATH` entry also decides
where `argv[0]` is looked up). The session drains the server's stderr and keeps
its last `max_stderr_bytes` (64 KiB); stderr output never ends the session.
`startup_timeout` bounds the launch through the initialized notification and
`request_timeout` every request. A request that is cancelled or times out ends
a hardened session: the client sends `notifications/cancelled`, waits
`cancel_grace` (500 ms) for the server to stop, and then terminates the process
tree; later requests fail with `MCPFailure::shutdown`.

**Bounds and failures:** `rpc_call_async(method, params, deadline,
cancel_token)` and `call_tool_result_async()` take an absolute steady-clock
deadline and a `graph::CancelToken`. Cancellation and deadlines stop waiting
for that call, tell the server with `notifications/cancelled` (never for
`initialize`), and leave the shared stdio process and sibling responses alone.
`initialize(name, deadline, cancel_token)` bounds the handshake the same way
without telling the server, since MCP never cancels `initialize`; a stdio server
may then refuse a second handshake. HTTP additionally applies
`MCPClientConfig::request_timeout` per request. Transport and wire failures
throw `MCPTransportError`, a `std::runtime_error` subclass exposing `failure()`
(`connection`, `timeout`, `cancelled`, `shutdown`, `http_status`, or
`protocol`) and `http_status()` (zero for non-HTTP failures). JSON-RPC error
responses are `MCPError` with the server's `code()` and `data()`, also when an
HTTP error status carried them (`http_status()` then names it). Neither class
identifies a tool-level `isError` result; use `call_tool_result()` to inspect
that outcome.

| Method | Description |
|--------|-------------|
| `MCPClient(url)` | Construct an HTTP-mode client for the MCP endpoint `url`, used as given |
| `MCPClient(argv)` | Spawn a subprocess and construct a stdio-mode client. `argv[0]` is resolved through `PATH` before fork; failed exec surfaces as a connection error on first RPC. Refuses Windows `.bat`/`.cmd` for safety |
| `MCPClient(StdioClientConfig)` | Hardened stdio launch with an explicit environment, bounded stderr capture, deadlines and the cancellation grace |
| `initialize(client_name)` | Perform the MCP initialization handshake once. Repeated calls are idempotent; protocol/transport failures throw |
| `initialize(client_name, deadline, cancel_token)` | The handshake, bounded by a deadline and a cancel token |
| `get_initialize_result()` | Return negotiated protocol, capabilities, server info, instructions, and raw result |
| `list_tools(cursor)` | Fetch one page while treating the cursor as opaque |
| `get_tool_definitions()` | Follow all pages and preserve complete tool metadata |
| `get_tools()` | Discover all pages and return session-preserving `MCPTool` instances |
| `call_tool(name, arguments)` | Invokes a tool by name with the given arguments. Returns the raw JSON response |
| `call_tool_result(name, arguments)` | Typed result preserving content, structured content, `isError`, and `_meta` |
| `call_tool_result_async(name, arguments, deadline, cancel_token)` | The typed call, bounded by an absolute steady-clock deadline and a cancel token as `rpc_call_async` is |
| `set_tool_list_changed_handler(handler)` | Call `handler` on `notifications/tools/list_changed`; an empty handler stops the calls |
| `shutdown()` | End the client and every tool it produced for good: `DELETE` for an HTTP session, an orderly stop for a stdio server |
| `rpc_call_async(method, params, deadline, cancel_token)` | The transport-level JSON-RPC call every other method uses |

**Serving tools.** `MCPServer` (`<neograph/mcp/server.h>`) answers `tools/call`
from registered handlers; how a handler's failure reaches the client is in
[Protocol failure mapping](#protocol-failure-mapping). It answers a client that
asks for `2025-11-25`, `2025-06-18`, `2025-03-26` or `2024-11-05` with that
revision and the fields it defines (no `icons` or `execution` for `2025-06-18`;
also no `title`, `outputSchema`, `_meta`, `structuredContent` or
`resource_link` content for `2025-03-26`, where a result with only structured
data carries it as text; also no `annotations` or audio content for
`2024-11-05`; a content block the revision lacks is sent as a text block holding
its JSON), and any other request with `2025-11-25`; `protocol_version()` reports
the result. A client that negotiated `2025-03-26` may send a JSON-RPC batch, as
that revision requires a receiver to accept, and so may a `2024-11-05` one: the
batch is answered with one array, on stdio and over HTTP. Later revisions
removed batches, and initialize is never part of one, so such a batch is one
`-32600` error (HTTP 400). `handle_message(message, reply)` hands each message's
outcome, a response, a batch's array or none, to its caller. Registration refuses a
tool whose `inputSchema` or `outputSchema` does not say `"type": "object"` (an
empty object, the old default, is read as `{"type": "object"}`), a schema that
is not JSON Schema 2020-12 the validator supports, and a name outside SEP-986's
1 to 128 characters of `A-Za-z0-9_.-`. Arguments are validated with a JSON
Schema 2020-12 validator (`$ref`, `anyOf`, `minimum`, an integral `1.0` as an
integer, and the rest of the core, applicator and validation vocabularies).
An unknown tool is JSON-RPC error `-32602`, and an error response carries the
request's id when it could be read and no id otherwise, never `null`.
`MCPHttpServer` (`<neograph/mcp/http_server.h>`) serves the same over
Streamable HTTP: a session ends with `DELETE`, after `session_idle_timeout` (30
minutes) without a request, or when a new client needs room at `max_sessions`
(64) and it is the least recently used session with no request in flight; its
id then answers 404. HTTP threads start on demand up to `max_http_threads` (64),
since a tool call holds one until it returns; a request that outlives
`response_timeout` gets 504 and its tool call is cancelled; a `DELETE` closes
its connection; and with `protected_resource_metadata` the server publishes
RFC 9728 metadata and names it in every 401 challenge. A request after
`initialize` may name any supported revision in `MCP-Protocol-Version`: the
session keeps the revision it negotiated, the first request of a session that
names another one is reported on stderr, and an unsupported value is 400.

**HTTP usage:**

```cpp
neograph::mcp::MCPClient client("http://localhost:8000/mcp");
client.initialize();
auto tools = client.get_tools();
```

**stdio usage:**

```cpp
// argv[0] is resolved through PATH before fork; inherited fds close before execve.
neograph::mcp::MCPClient client({"python", "/path/to/server.py"});
client.initialize();
auto tools = client.get_tools();   // Tools retain the protocol session/process.
```

---

## 14. Util Module

**Header:** `<neograph/util/request_queue.h>`
**Namespace:** `neograph::util`

### RequestQueue

Lock-free request queue with a worker thread pool and backpressure support.
Decouples HTTP connection acceptance from LLM call concurrency in server applications.

```cpp
class RequestQueue {
public:
    struct Stats {
        size_t pending;        // Tasks waiting in queue
        size_t active;         // Tasks currently executing
        size_t completed;      // Total settled tasks, including cancellation
        size_t rejected;       // Tasks rejected during admission
        size_t num_workers;    // Number of worker threads
        size_t max_queue_size; // Maximum queue capacity
    };

    // num_workers must be greater than zero.
    RequestQueue(size_t num_workers = 128, size_t max_queue_size = 10000);
    ~RequestQueue();

    // Non-copyable
    RequestQueue(const RequestQueue&) = delete;
    RequestQueue& operator=(const RequestQueue&) = delete;

    // Submit a task. Concurrent callers cannot exceed max_queue_size.
    // A full queue returns {false, invalid_future}; an internal enqueue
    // failure returns {false, valid_future}, which throws on get().
    template<typename F>
    std::pair<bool, std::future<void>> submit(F&& task);

    // Idempotently reject new work, cancel queued tasks, and wait for workers.
    void close();
    bool is_closed() const noexcept;

    // Get current queue statistics
    Stats stats() const;
};
```

| Constructor Parameter | Type | Default | Description |
|-----------------------|------|---------|-------------|
| `num_workers` | `size_t` | `128` | Number of worker threads in the pool; zero throws `std::invalid_argument` |
| `max_queue_size` | `size_t` | `10000` | Maximum number of pending tasks. Tasks beyond this limit are rejected |

| Method | Description |
|--------|-------------|
| `submit(task)` | Atomically reserves pending capacity, then enqueues a callable. Returns a pair: `first` is `true` if accepted. A full or closed queue returns `false` with an invalid future; an internal enqueue failure returns `false` with a valid future that propagates the error. Accepted futures resolve when the task completes or propagates task exceptions. |
| `close()` | Idempotently rejects new work. An external caller waits for all workers to finish shutdown, then unclaimed work completes with `std::runtime_error("RequestQueue is closed")`. A callable claimed by a worker before closure may finish. A callable may invoke `close()` to initiate shutdown, but cannot wait for itself. |
| `is_closed()` | Reports whether `close()` has begun rejecting new work. |
| `stats()` | Returns a snapshot of current queue statistics |

The queue uses `moodycamel::ConcurrentQueue` internally for lock-free enqueue/dequeue
and a condition variable to wake idle workers.

**Usage:**

```cpp
neograph::util::RequestQueue queue(4, 100);  // 4 workers, max 100 pending

auto [accepted, future] = queue.submit([&] {
    // Handle an incoming HTTP request
    auto result = engine->run(config);
    send_response(result);
});

if (!accepted) {
    send_503_service_unavailable();
}
```

---

## Usage Examples

### Minimal ReAct Agent



```cpp
#include <neograph/graph/react_graph.h>
#include <neograph/llm/schema_provider.h>

neograph::graph::RunResult run_react(
    sp::descriptor::ValidatedDescriptor descriptor, sp::runtime::Options options,
    std::string model, std::vector<std::unique_ptr<neograph::Tool>> tools,
    neograph::graph::RunConfig config) {
    auto provider = std::make_shared<neograph::llm::SchemaProvider>(
        std::move(descriptor), std::move(options));
    auto engine = neograph::graph::create_react_graph(
        provider, std::move(tools), "", model);
    return engine->run(config);
}
```


### Custom Graph with Conditional Routing

Building a graph with conditional edges:

```cpp
#include <neograph/neograph.h>
#include <neograph/llm/schema_provider.h>

using namespace neograph::graph;
using json = nlohmann::json;

void run_custom_graph(
    sp::descriptor::ValidatedDescriptor descriptor, sp::runtime::Options options,
    std::string model, std::vector<std::unique_ptr<neograph::Tool>> tools) {
    auto provider = std::make_shared<neograph::llm::SchemaProvider>(
        std::move(descriptor), std::move(options));

    json definition = {
        {"name", "assistant_graph"},
        {"channels", {
            {"messages", {{"reducer", "append"}}},
            {"status",   {{"reducer", "overwrite"}, {"initial", "idle"}}}
        }},
        {"nodes", {
            {"llm",   {{"type", "llm_call"}}},
            {"tools", {{"type", "tool_dispatch"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "llm"}},
            {{"from", "tools"},     {"to", "llm"}}
        })},
        {"conditional_edges", json::array({
            {{"from", "llm"},
             {"condition", "has_tool_calls"},
             {"routes", {{"true", "tools"}, {"false", "__end__"}}}}
        })}
    };

    auto store = std::make_shared<InMemoryCheckpointStore>();
    EngineConfig engine_config;
    engine_config.node_context.provider = provider;
    engine_config.node_context.model = model;
    engine_config.node_context.instructions = "You are a helpful assistant.";
    engine_config.checkpoint_store = store;
    EngineResources resources{.tools = ToolSet(std::move(tools))};
    auto engine = GraphEngine::build(definition, std::move(engine_config),
                                     std::move(resources));

    RunConfig config;
    config.thread_id = "session-1";
    config.input = {{"messages", json::array({
        {{"role", "user"}, {"content", "Search for NeoGraph C++ library"}}
    })}};

    auto result = engine->run(config);

    // Inspect execution trace
    for (const auto& node : result.execution_trace) {
        std::cout << "Executed: " << node << "\n";
    }
}
```

### Human-in-the-Loop with Checkpointing

Using interrupts for human approval:

```cpp
auto store = std::make_shared<InMemoryCheckpointStore>();
EngineConfig engine_config;
engine_config.node_context = ctx;
engine_config.checkpoint_store = store;
auto engine = GraphEngine::build(definition, std::move(engine_config));

// Configure interrupt after the "tools" node
// (set "interrupt_after": ["tools"] in the JSON definition)

RunConfig config;
config.thread_id = "approval-session";
config.input = {{"messages", json::array({
    {{"role", "user"}, {"content", "Delete all files in /tmp"}}
})}};

auto result = engine->run(config);

if (result.interrupted) {
    std::cout << "Interrupted at: " << result.interrupt_node << "\n";
    std::cout << "Reason: " << result.interrupt_value.dump() << "\n";

    // Get human input...
    std::string approval = get_human_approval();

    // Resume with the human's decision
    auto resumed = engine->resume(
        "approval-session",
        {{"approved", approval == "yes"}}
    );
}
```

### Dynamic Fan-Out with Send

Using `Send` for map-reduce patterns:

```cpp
class FanOutNode : public GraphNode {
public:
    std::string get_name() const override { return "fan_out"; }

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto items = in.state.get("items");
        NodeOutput result;
        for (const auto& item : items) {
            result.sends.push_back(Send{
                "process_item",       // target node
                {{"item", item}}      // input for that invocation
            });
        }
        co_return result;
    }
};
```

Each `Send` dispatches the `"process_item"` node with a different input. The engine
executes all sends, collecting their channel writes, before proceeding to the next
edge in the graph.

### Routing Override with Command

Using `Command` to simultaneously update state and control routing:

```cpp
class RouterNode : public GraphNode {
public:
    std::string get_name() const override { return "router"; }

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto messages = in.state.get_messages();
        auto last = messages.back().content;

        NodeOutput result;

        if (last.find("urgent") != std::string::npos) {
            result.command = Command{
                "urgent_handler",                          // goto node
                {{"priority", neograph::json("high")}} // state updates
            };
        } else {
            result.command = Command{
                "normal_handler",
                {{"priority", neograph::json("normal")}}
            };
        }

        co_return result;
    }
};
```

When `Command` is returned, its `updates` are applied to the state and execution
jumps directly to the specified `goto_node`, bypassing normal edge routing.

### SchemaProvider Multi-LLM Support

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


### MCP Tool Integration

`Agent::run`, `run_stream` and `complete` return the full `sp::runtime::Result` and accept `std::vector<sp::Message>`. `run_stream` receives typed events on every actual turn; it does not discard and resend an answer for display. `outcomes()` retains individual results and `usage()` exposes the nullable report. A model is chosen explicitly by the caller.

```cpp
#include <neograph/mcp/client.h>
#include <neograph/llm/agent.h>
#include <neograph/llm/schema_provider.h>

sp::runtime::Result run_mcp_agent(
    neograph::mcp::MCPClient& mcp,
    sp::descriptor::ValidatedDescriptor descriptor, sp::runtime::Options options,
    std::string model, std::vector<sp::Message>& history) {
    mcp.initialize("neograph-example");
    auto tools = mcp.get_tools();
    auto provider = std::make_shared<neograph::llm::SchemaProvider>(
        std::move(descriptor), std::move(options));
    neograph::llm::Agent agent(provider, std::move(tools), "", model);
    return agent.run(history);
}
```


## Beyond this tour

The headers under `include/neograph/` carry public surface that
isn't walked through above. Each block below is a one-paragraph
pointer to that canonical source-level reference.

### `neograph::a2a` — Agent-to-Agent protocol

**Header:** `<neograph/a2a/{client,server,types,a2a_caller_node}.h>`
`A2AServer` exposes a graph (or an admitted `ProgramVersion`) over the A2A JSON-RPC binding with HTTP and SSE; `A2AClient` calls remote agents. A2A 1.0 (v1.0.1) is the primary version and 0.3 the compatibility dialect (`WireDialect::{V1_0,V0_3}`). The official SDKs (a2a-sdk 1.2.2, @a2a-js/sdk 1.3.0 and 0.3.14) test both directions in `bindings/python/tests/interop/a2a_suite`.

#### A2A version policy

- **Requests.** The server answers each request in the dialect its `A2A-Version` names, from the header or the `A2A-Version` query parameter. No version means 0.3 (spec §3.6.2). Only Major.Minor `1.0` and `0.3` are spoken and a patch component is ignored; anything else, such as `0.2`, `1.5` or `2.0`, gets VersionNotSupportedError (`-32009`) naming the supported versions. Both method spellings (`SendMessage`, `message/send`) are accepted with either version.
- **Card.** The card is served per version. `A2A-Version: 1.0` receives a pure 1.0 `AgentCard`; a request without a version receives the 0.3 card, which still lists the 1.0 interface first in `supportedInterfaces`. Responses carry `ETag`, `Cache-Control: public, max-age=300` and `Vary: A2A-Version`, and `If-None-Match` gets 304. Without a configured `AgentCard::url` the card advertises the address the client used. Every skill has at least one tag. The card never claims push notifications, the extended card or the HTTP+JSON and gRPC bindings, none of which are implemented.
- **Client.** Before its first call the client fetches the card and selects the first JSONRPC interface with a `1.x` or `0.x` version, preferring the one whose URL is the client's `base_url`. Only when no card can be fetched does it probe: 1.0 first, then 0.3 after `-32601`, `-32009` or HTTP 400, remembering the outcome. RPCs go to the selected interface URL when it has `base_url`'s origin, so an agent whose endpoint is `/a2a/v1` works. A card cannot send the client to another origin unless `set_allow_cross_origin_interfaces(true)` opts in; otherwise the RPC goes to `base_url`, and the client writes a warning naming the origin it did not follow to stderr. `endpoint_url()` reports the endpoint in use, and message ids are random UUIDs.
- **0.3.** 0.3 stays as a compatibility dialect with the same semantics. New features target 1.0.

#### Tasks and methods

Every run executes on its own worker thread; a stream or a blocking send only observes it, so a task outlives a dropped connection. `SendMessage` blocks until the task is terminal or interrupted unless `returnImmediately` (0.3 `blocking: false`) asks for the SUBMITTED task at once. `SendStreamingMessage` and `SubscribeToTask` (0.3 `tasks/resubscribe`) stream the Task first, then status and artifact updates, and close on a terminal or interrupted state; the 0.3 stream ends on `final: true`. A Program task that the server recovered after a restart has no channel of its own, because this process did not start its run: `SubscribeToTask` on it streams the Task, follows the durable run until the run has a result or a cancel commits, then sends the result artifact and the final status as for any run (a run that paused ends on `TASK_STATE_INPUT_REQUIRED`). A run that settled since the recovery is read first and answered as terminal with `-32004`. When the run cannot be followed, the stream ends with a `-32603` error event (ErrorInfo reason `RUN_UNAVAILABLE`), or the call is answered with `-32603` when the task has no run here, and never closes empty. A stream that has been silent for 5 seconds writes the SSE comment `: keep-alive`, so a client or proxy that times out an idle connection does not cut a run that is still working. `GetTask` and `ListTasks` honour `historyLength` (0 omits the history). `ListTasks` filters by `contextId`, `status` and `statusTimestampAfter`, pages newest first with `pageSize` (1 to 100, default 50) and `pageToken`, and omits artifacts unless `includeArtifacts` is set. Every status carries a timestamp.

Task ids are generated by the server. A message whose `taskId` is unknown fails with `-32001`; a terminal task with `-32004`; a `contextId` that differs from the task's with `-32602`; a task that is still running with `-32603` (ErrorInfo reason `TASK_BUSY`). A message to an INPUT_REQUIRED task resumes its run from the checkpoint the interrupt saved; `GraphAgentAdapter::build_resume_value` turns the message into the resume value, and an engine without a checkpoint store runs the graph again with the message as input. A Program-backed task cannot take that answer over A2A yet: the message gets `-32004` and the task keeps waiting. NeoGraph collaboration envelopes keep their sender-named, idempotent task ids ([A2A collaboration extension](#a2a-collaboration-extension)). Requests are validated per dialect; violations get `-32602` with `google.rpc.BadRequest` field violations, and every error carries a `google.rpc.ErrorInfo` detail. With a `CollaborationAuthenticator` the card declares a Bearer security scheme, plus a global requirement when requests must be authenticated; missing credentials get HTTP 401 with `WWW-Authenticate`, and an authenticated peer that may not use a collaboration link gets HTTP 403.

#### A2A error mapping

A run's failure classes reach the peer as [Protocol failure mapping](#protocol-failure-mapping) shows: the task state, the public message as status message and `metadata["neograph/failure"] = {code, retryable}`, never exception text. A completed run is `COMPLETED` with the agent's reply as status message and the adapter's artifacts; a step limit is `FAILED` with code `NG.StepLimit`, and a failure exit `FAILED` with the exit's declared code. The requests the server refuses:

| Request | A2A result |
|---|---|
| A message naming an unknown task | `-32001` TaskNotFoundError |
| A message to a terminal task | `-32004` UnsupportedOperationError; a collaboration redelivery returns the task |
| A message whose `contextId` differs from the task's | `-32602` with a field violation |
| A message to a task that is still running | `-32603`, ErrorInfo reason `TASK_BUSY`, domain `neograph` |
| A message to a Program task waiting for input | `-32004`; the task keeps waiting (answering a Program's input request over A2A is not supported yet) |
| The in-flight limit is reached | `-32603`, ErrorInfo reason `RESOURCE_EXHAUSTED`, domain `neograph`, metadata `retryAfterSeconds` |
| Credentials the agent requires are missing or invalid (a collaboration message, or every request under strict authentication) | HTTP 401 with `WWW-Authenticate`; the body is JSON-RPC `-32600` with ErrorInfo reason `UNAUTHENTICATED`, domain `neograph` |
| An authenticated peer the collaboration link does not permit | HTTP 403; the body is JSON-RPC `-32600` with ErrorInfo reason `PERMISSION_DENIED`, domain `neograph` |
| Cancel of a terminal task | `-32002` TaskNotCancelableError; the task is unchanged |
| Push-notification configuration; extended card; a streaming method when the card has `streaming = false` | `-32003`; `-32004`; `-32004` |
| An `A2A-Version` other than 1.0 or 0.3 | `-32009` VersionNotSupportedError |

Every error carries one `google.rpc.ErrorInfo`; its domain is `a2a-protocol.org` unless the row says `neograph`.

#### A2A collaboration extension

A Program agent with a `CollaborationMailbox` accepts NeoGraph collaboration envelopes and declares them in its card's `capabilities.extensions`, in both versions: `uri` is `a2a::COLLABORATION_EXTENSION_URI` (this section), `required` is false, and `params` is `{"format": "neograph-a2a-collaboration-v1", "metadataKey": "neograph_collaboration"}`. Such a message carries a data part `{format, envelope}` and `metadata.neograph_collaboration`, and only an authenticated peer of a linked collaboration may send one. Its sender names the receiver task id up front: the first delivery creates the task under that id, and a redelivery returns the existing task instead of starting another run. Ordinary A2A clients never need it.

#### Client streams and Python

SSE accepts LF/CRLF/CR, comments, multiline data and final unterminated data. Opening task snapshots, status updates (including their metadata) and artifact append/replace updates accumulate into the returned task. An observed event forbids dialect redispatch even without an external callback. Non-SSE RPC errors retain their code through `A2ARpcError::code()`; non-2xx HTTP cannot become a successful task. The caller node chooses final/interrupted agent status text, then first artifact text, then last agent history text; progress status or user history cannot override the answer. Generic `async_post_stream` also delivers ordinary fixed-`Content-Length` bodies once when nonempty, preserving status; zero length emits no chunk. Body limit, early EOF and already-buffered surplus reject. Redirect/chunked/close-delimited rules remain unchanged.

Python's `neograph_engine.a2a` exports `WireDialect`, `AgentInterface`, `AgentCard.supported_interfaces`/`raw`, `Part.media_type`, `MessageSendConfiguration`, `MessageSendParams`, `StreamEvent` and status/artifact records. `A2AClient.wire_dialect()` returns an enum or `None`; `set_authorization_header()`, `set_allow_cross_origin_interfaces()` and `endpoint_url()` mirror the native client; `send_message(params)` is the multipart overload. `send_message_stream(text, on_event, task_id="", context_id="")` or `(params, on_event)` returns the accumulated `Task` and passes owned event snapshots to a bool-returning callback. Blocking calls release the GIL. `a2a.A2ARpcError.code` preserves the remote integer code.

**Public headers:** [`include/neograph/a2a/`](../include/neograph/a2a/).

### `neograph::acp` — Agent Client Protocol

**Header:** `<neograph/acp/{server,types}.h>`
Editor↔agent JSON-RPC 2.0 over newline-delimited JSON on stdio (Zed, Gemini CLI, Neovim CodeCompanion), ACP protocol version 1, checked against the official schema v1.25.0 and the official Python (0.12, 1.0) and TypeScript SDKs. Client→agent: `initialize`, `session/{new,resume,prompt,cancel}`. Agent→client: `session/update`, `session/request_permission`, and `fs/*`, `terminal/*` or `elicitation/*` only when the client advertised the capability, through the late-bound `ACPClient`. `ACPServer::handle_message` runs each prompt turn on a worker thread, at most `max_inflight_prompts` (32) turns and one per session. A refused turn is JSON-RPC `-32603` with `data.reason` `"overloaded"` or `"session_busy"`; `-32000` is ACP's "Authentication required" and is not used. A call before `initialize` is `-32600`, an unknown session `-32002`, and `session/resume` of a session the agent knows but never checkpointed succeeds.

Prompts accept text and `resource_link` blocks (baseline content; the default adapter renders a link as `[name](uri)` in the input) and image, audio and embedded resources only when `capabilities().prompt` advertises them. A malformed or unadvertised block is `-32602`. The default adapter seeds `_acp_session_id`, `_acp_cwd`, `_acp_additional_directories` and `_acp_mcp_tools`; override `build_session_state` (or the older `build_initial_state`). `agentInfo` is always an ACP `Implementation`: a missing name or version is filled in.

How a turn ends:

| NeoGraph outcome | ACP result |
|---|---|
| Completed | `stopReason: "end_turn"` after one `agent_message_chunk` (none for empty text) |
| Cancelled by `session/cancel` | `stopReason: "cancelled"` |
| Step limit (`RunConfig::max_steps`) | `stopReason: "max_turn_requests"` |
| Model-token budget crossed | `stopReason: "max_tokens"` |
| Provider refusal or content filter (`NG.SafetyBlocked`) | `stopReason: "refusal"` |
| `NodeInterrupt` | `session/request_permission` in the same turn; the answer resumes the run |
| Hook block (`NG.HookBlocked`), node or tool failure (`NG.TaskFailed`, an author code), timeout (`NG.Timeout`), provider failure (`NG.ProviderFailed`), money or quota budget (`NG.BudgetExhausted`), configuration, logic or unknown failure | JSON-RPC error `-32603`, message = the public message, `data: {code, retryable}` ([Protocol failure mapping](#protocol-failure-mapping)) |
| Cancelled by something other than the client (a node aborted, shutdown) | JSON-RPC error `-32800` |

Exception text (`what()`) never reaches the client: the message is a typed public message (`NodeFailureError::public_message`) or a generic text for the failure class, and the detail stays in the host's logs. An interrupt becomes a tool call titled with its reason, with the payload as `rawInput`, `_meta["neograph/interrupt"]` and the options "approve" (`allow_once`) and "reject" (`reject_once`); the selected option id is the resume value. Override `interrupt_permission` and `interrupt_resume_value` to change that. A `cancelled` answer ends the turn cancelled; an adapter that returns `std::nullopt`, or a client that cannot answer, fails the turn with `data.code: "NG.Interrupted"` and leaves the session unpaused. A pause restored by `session/resume` from a checkpoint, for example after an agent restart, is answered by the next prompt's text.

`session/cancel` cancels the ongoing turn only; a cancel with no turn running is ignored (`set_pre_armed_cancel(true)` keeps the earlier pre-cancel for transports that reorder messages). A `$/cancel_request` notification whose `requestId` is that of a running `session/prompt` cancels the turn in the same way, and the prompt still gets its one response (`stopReason: "cancelled"`); a `requestId` of any other request, unknown or finished, is ignored. A pending agent→client request gets 500 ms to be answered after a cancel; then the server sends `$/cancel_request` and the turn ends. Permission requests have no timeout unless `ACPClient::set_permission_timeout` sets one. When stdin reaches EOF, `run()` lets in-flight turns finish for `set_eof_grace` (one second) and then cancels them.

Stdio `mcpServers` from `session/new` and `session/resume` are started with NeoGraph's MCP client (built with `NEOGRAPH_BUILD_MCP_CLIENT`) in the session's `cwd`, with a minimal environment: the agent's `PATH` (on Windows also `SystemRoot`) plus the client's `env` entries. The rest of the agent's environment, which may hold provider API keys, is not passed on; nodes reach their tools through `ACPClient::session_tools(session_id)`. HTTP and SSE MCP servers are not advertised and are rejected with `-32602`. A server that does not start fails the request with `-32603` and `data.reason: "mcp_server_unavailable"`.

**Public headers:** [`include/neograph/acp/`](../include/neograph/acp/).

### `neograph::async` — HTTP/SSE/WS helpers

**Header:** `<neograph/async/{conn_pool,http_client,sse_parser,ws_client,curl_h2_pool,run_sync}.h>`
These general NeoGraph HTTP/SSE/WebSocket helpers remain available for non-provider integrations. They are not the SchemaProvider transport, codec or retry authority. Typed chat-family calls use the SDK runtime and `ProviderMode`; the old Responses WebSocket provider path and descriptor stream parser are removed.

**Public headers:** [`include/neograph/async/`](../include/neograph/async/).

### Persistent checkpoint backends

**Header:** `<neograph/graph/postgres_checkpoint.h>`,
`<neograph/graph/sqlite_checkpoint.h>`
`PostgresCheckpointStore` — libpq-based, 3-table schema (`neograph_*`)
with channel-blob deduplication keyed on
`(thread_id, channel, version)`; LangGraph `PostgresSaver` parity. Async
initial/replacement connections use one global deadline across all hosts:
positive `connect_timeout` written directly in the connection string (minimum
2s), otherwise a 30s safety default. Environment and service-file timeout
values are not available before initial connection setup and use that default.
Synchronous libpq connection timeout behavior is unchanged.
`SqliteCheckpointStore` — same shape, single-file backend, fits the
edge / single-host deployments.
**Public headers:**
[`PostgresCheckpointStore`](../include/neograph/graph/postgres_checkpoint.h) ·
[`SqliteCheckpointStore`](../include/neograph/graph/sqlite_checkpoint.h).

### Other public surface not in this tour

- **`neograph::AsyncTool`** — `Tool` peer that exposes
  `execute_async(json)` for tools whose work is naturally
  coroutine-shaped (HTTP fetch, MCP call). Sync `execute()` is
  `final`-routed through `run_sync`.
- **`neograph::graph::NodeCache`** — per-node memoization, opt-in
  at construction via `EngineConfig::cached_nodes` (the setter remains a
  compatibility surface).
- **`neograph::graph::create_deep_research_graph`** —
  open_deep_research-style supervisor + sub-researcher fan-out,
  used by `examples/25_deep_research.cpp`. It includes a
  `BriefNode` LLM rewrite, a `FinalReportNode` token-limit retry and
  a `ClarifyNode` HITL gate.

If the type you need isn't in this tour, check `include/neograph/`
directly. Every public header carries its reference documentation.
