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

These additional modules expose public headers under `include/neograph/{a2a,acp,async}/`. Dedicated narrative coverage is deferred; consult the headers for their exact public contracts. Their presence is not a current integrated qualification claim.

**Convenience header:** `#include <neograph/neograph.h>` includes the full core + graph engine API.

SchemaProvider is now a required external C++ dependency even when `NEOGRAPH_BUILD_LLM=OFF`: Core exports its owned typed provider contracts. Install the SDK runtime package and set `SCHEMAPROVIDER_PREFIX` to that install prefix; the configure commands below use `-DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX"`. Alternatively supply an explicit checkout with `-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=../SchemaProvider`. Neither a guessed sibling checkout nor the old bundled interpreter is selected automatically. The SDK runtime/archive currently supports Linux/POSIX; there is no dependency-free, no-OpenSSL, native Windows/macOS or WASM runtime promise for this cutover.

The SDK imported target supplies its `include/SchemaProvider` include root; public examples use `<descriptor/descriptor.h>`, `<runtime/client.h>` and `<neograph/llm/schema_provider.h>` directly, without recipe-only helpers.

```cmake
find_package(SchemaProvider CONFIG REQUIRED COMPONENTS runtime)
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
- [3. Tool Interface](#3-tool-interface)
  - [Tool](#tool)
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
  - [json_path Utilities](#json_path-utilities)
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
    std::vector<std::string> image_urls; // base64 data URLs or HTTP URLs for Vision
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
};
```

| Field | Type | Description |
|-------|------|-------------|
| `name` | `std::string` | Unique tool name |
| `description` | `std::string` | Description shown to the LLM to explain the tool's purpose |
| `parameters` | `json` | JSON Schema object describing accepted parameters |

### Owned Outcome

A provider call returns `sp::runtime::Result`: an immutable, owned `std::shared_ptr<const sp::Outcome>`, containing `sp::Completion` or `sp::Failure`. Retain the whole outcome, not only display text. Ordered messages/parts, native continuation, complete wire envelopes, ordered raw observations, stop evidence and genuine attempt metadata survive the call and client destruction. Usage counters are nullable `uint64_t` values with evidence, stage and quality: missing is unknown, never zero. A failure retains its original partial outcome. `ProviderFailure::outcome()` and `ProviderObserverError::outcome()` preserve that result; the latter also preserves the observer exception in `cause()`.

### Portable projections


If post-effect accounting or terminal-receipt persistence fails after a real result exists, `ProviderDispatchOutcomePersistenceError` retains the original immutable result in `outcome()` and the original persistence exception in `cause()`. If delivery also failed, `delivery_error()` retains the original observer exception. Successful persistence followed by observer failure rethrows that original observer exception unchanged; an unknown/no-result transport failure does not fabricate an outcome.
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

All fields use `value()` with empty-string defaults, making deserialization tolerant
of missing fields.

---

## 2. Provider Interface

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

`provider_failure_proves_not_sent(Failure)` requires complete, contradiction-free NotSent evidence. A status code, missing usage or observer/persistence exception alone never proves zero cost or renews a budget.

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


This is a source and binary break: recompile every C++ consumer and custom provider with matching new headers/libraries. `CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, the descriptor interpreter and Responses WebSocket path are removed, with no aliases or compatibility bridges. The SDK is unstable `0.0.0`, interface revision 3 / shared ABI 3, with out-of-line capability checks; that is not a stable release claim. Current runtime/archive support is Linux/POSIX; no Windows, macOS or WASM runtime qualification is implied. Python provider bindings/wrappers are deferred and not ported by this C++ change.

Fresh installed find_package Program C++/C ABI/dualQuickJS consumers and the NeoGraph/SchemaProvider typed two-request lifetime/native/raw/mismatch consumer passed. Interface/ABI declarations alone remain distinct from this exercised package result; broader platforms and stable release are not claimed.

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
    json        value;                             // Current value
    uint64_t    version = 0;                       // Write counter
};
```

### ChannelWrite

A single write operation targeting a named channel. Nodes return vectors of these.

```cpp
struct ChannelWrite {
    std::string channel;  // Target channel name
    json        value;    // Value to write (merged via the channel's reducer)
};
```

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
    std::string               model;      // Model override (empty = provider default)
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
| `NODE_END` | Channel writes produced by the node |
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
                      const json& initial_value = json());

    json get(const std::string& channel) const;
    std::vector<ChatMessage> get_messages() const;

    void write(const std::string& channel, const json& value);
    void apply_writes(const std::vector<ChannelWrite>& writes);

    uint64_t channel_version(const std::string& channel) const;
    uint64_t global_version() const;

    json serialize() const;
    void restore(const json& data);

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
| `channel_version(channel)` | Returns the write counter for a specific channel |
| `global_version()` | Returns the global version counter (incremented on every write to any channel) |
| `serialize()` | Serializes all channel values and versions to JSON (for checkpointing) |
| `restore(data)` | Restores channel values and versions from serialized JSON |
| `channel_names()` | Returns the names of all initialized channels |

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
| `in.ctx.cancel_token` | Pass to `provider.invoke(std::move(request))` so an LLM HTTP socket aborts on cancel, or poll `ctx.cancel_token->is_cancelled()` for your own loops |
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
legacy engines. Use scoped Python registries for deterministic callbacks.

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

### RunContext (v0.4 PR 1, exposed to nodes via `NodeInput.ctx`)

Per-run dispatch metadata threaded by the engine. Constructed from `RunConfig`
(with a new usage accumulator when none was supplied), `RunMetadata`, the
effective Store, and an optional resume value. Nodes consume it inside a
`run(NodeInput) -> NodeOutput` override via `in.ctx`.

```cpp
struct RunContext {
    std::shared_ptr<CancelToken>  cancel_token;
    std::shared_ptr<UsageAccumulator> usage;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::string                   trace_id;
    std::string                   thread_id;
    int                           step;
    StreamMode                    stream_mode;
    std::optional<json>           resume_value;
    std::shared_ptr<Store>        store;
    ToolGate                      tool_gate;
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
| `fork()` | **v0.4 PR 3.** Returns a child shared_ptr. Parent.cancel() cascades; if the parent is already cancelled at fork() time the child is constructed pre-cancelled (no emit-vs-bind race) |
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
    RunStatus status() const noexcept;            // Completed, Interrupted, or StepLimit

    template <typename T> T channel(const std::string& name) const;
    template <typename T> T channel(const ChannelKey<T>& key) const;
    template <typename T>
    std::optional<T> try_channel(const ChannelKey<T>& key) const;
};
```

`RunResult::usage` is the nullable provider report, not the spending bank. `native_messages` retains genuine typed history and `provider_outcomes` retains every owned Completion/Failure. JSON `output` is only a portable projection. For full history input use `RunConfig::provider_messages`; observe typed events with `on_provider_event`. Durable native checkpoint/receipt custody must use `native_history_archive`; in-memory sidecars do not require one.
| Field | Type | Description |
|-------|------|-------------|
| `output` | `json` | Serialized final state of all channels |
| `interrupted` | `bool` | `true` if execution was paused by an interrupt (HITL) |
| `interrupt_node` | `std::string` | Name of the node that triggered the interrupt |
| `interrupt_value` | `json` | Reason or payload from the interrupt |
| `checkpoint_id` | `std::string` | UUID of the last saved checkpoint |
| `execution_trace` | `std::vector<std::string>` | Ordered list of node names in execution order |

`max_steps_exhausted()` returns `true` only when the step ceiling stopped the
run while runnable work remained. A graph that reaches `__end__` exactly on its
last permitted step returns `false`.

`status()` returns `RunStatus::Completed`, `RunStatus::Interrupted`, or
`RunStatus::StepLimit` without changing the public `RunResult` data layout.
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
    // while a run is in flight (Round 3 guard — `active_runs_`
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

Returns the checkpoint history for a thread, ordered by timestamp (newest first).

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
- `run_sends_async`: single Send runs on the shared state with retry;
  multi Send gives each target an isolated state copy (init + restore
  + apply input) without retry — preserves pre-3.0 semantics.
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
    json        channel_versions;  // Per-channel version counters
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
| `channel_versions` | `json` | Version counter for each channel |
| `parent_id` | `std::string` | ID of the preceding checkpoint (forms a linked list for time-travel) |
| `current_node` | `std::string` | Node that was executing when the checkpoint was taken |
| `next_nodes` | `std::vector<std::string>` | All nodes scheduled for the next super-step (used by `resume()`). Under signal dispatch a super-step can leave several nodes simultaneously ready (parallel fan-out, conditional branches activating together), and every one of them must be persisted — storing a single node would silently drop siblings across a crash |
| `interrupt_phase` | `CheckpointPhase` | Enum: `Before` (interrupt_before fired), `After` (interrupt_after fired), `Completed` (normal super-step cadence), `NodeInterrupt` (node threw `NodeInterrupt` mid-execution), `Updated` (external `update_state()` injection). `to_string()` and `parse_checkpoint_phase()` give a stable wire/log encoding |
| `barrier_state` | `map<string, set<string>>` | Per-barrier accumulator of upstreams that have signaled so far. Entries only exist for barriers that are in-flight (not yet fired) — the Scheduler clears an entry when its barrier fires. Shape matches `BarrierState` from `scheduler.h`. Present since schema v2; v1 blobs deserialize with an empty map, which matches their pre-v2 behavior |
| `metadata` | `json` | Arbitrary user-defined data |
| `step` | `int64_t` | Super-step counter |
| `timestamp` | `int64_t` | Creation time in Unix epoch milliseconds |
| `schema_version` | `std::uint32_t` | On-wire layout version (see `CHECKPOINT_SCHEMA_VERSION`, currently `3`). Round 5 widened this from `int` to fixed-width unsigned — schema versions are non-negative and a platform-variable `int` width was wrong for a value persisted to disk and round-tripped through JSON. Persistent `CheckpointStore` implementations should serialize it and treat `0` on a deserialized blob as "pre-versioned" (e.g. the field was absent — migration is the caller's responsibility) |

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

> Historical Python provider/wrapper examples below are not ported to the typed C++ contract and are not current provider guidance. The C++ change does not implement or qualify Python bindings. C++ observers export only established public text/scalars and nullable counts, never raw native state.
**Module:** `neograph_engine.tracing` (OTel-shape) +
`neograph_engine.openinference` (LLM-shape)
**Since:** OTel layer in v0.3.x; OpenInference layer in **v0.6.0**.

NeoGraph emits its `GraphEvent` stream through the same callback the
streaming API uses. Two helpers ride on top:

  - **`otel_tracer(tracer)`** — vendor-neutral OpenTelemetry spans.
    Root span per run + child span per node + status / error / interrupt
    mapping. Spans flow to any OTel backend (Jaeger, Tempo, Honeycomb,
    Datadog, …). Useful when you already run an APM that just needs
    spans-shaped data.
  - **`openinference_tracer(tracer)` + `OpenInferenceProvider`** —
    LLM-shape attribute layer on top. Same OTel mechanics, but each
    span carries `openinference.span.kind` (`"CHAIN"` / `"LLM"`) plus
    LLM-specific keys (`llm.model_name`, `llm.input_messages.{i}.…`,
    `llm.token_count.{prompt,completion,total}`, etc.) so a backend
    that recognises the OpenInference convention — Phoenix, Arize,
    Langfuse — renders the trace as a chat-bubble + DAG hierarchy +
    per-call token cost UI (the "LangSmith UX").

### `otel_tracer` — OTel-shape spans

```python
from contextlib import contextmanager
from typing import Any, Callable, Iterator, Optional

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

| Knob | Default | Purpose |
|---|---|---|
| `root_name` | `"graph.run"` | Span name for the per-run root span |
| `node_span_prefix` | `"node."` | Prefix concatenated with each node name |
| `attribute_prefix` | `"neograph"` | Prefix for engine-specific attributes (`neograph.node`, `neograph.next_nodes`, etc.) |
| `on_event` | `None` | Optional secondary callback receiving every raw `GraphEvent` — useful for chaining with logging / metrics |

Events handled: `NODE_START` opens a child span, `NODE_END` closes
it (with `Status.OK`), `ERROR` records the exception and ends the
span with `Status.ERROR`, `INTERRUPT` tags
`{attribute_prefix}.interrupted = true` and ends.

Concurrent fan-out (multi-Send): each node-name keeps a stack of
open spans; `NODE_END` pops the most recent. Always-end-on-exit:
the context-manager's `finally` block force-closes any spans still
open if the run raises.

```python
from opentelemetry import trace
from neograph_engine.tracing import otel_tracer

tracer = trace.get_tracer("my-service")
with otel_tracer(tracer) as cb:
    engine.run_stream(cfg, cb)
```

### `openinference_tracer` — adds LLM-shape attributes

Same shape, plus each span tagged
`openinference.span.kind = "CHAIN"` and node payload encoded as
`input.value` / `output.value` JSON blobs. Phoenix / Arize / Langfuse
treat the trace as an LLM chain in their UI.

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

The tracer also attaches each node span as the OTel *current
context* (via `otel_context.attach`) so a `Provider.complete()`
call inside the node body opens its `llm.complete` span as a child
of that node — the trace is a single connected tree, not 3+ orphan
trace-IDs (the v0.6.0 contextvar-propagation fix).

### `OpenInferenceProvider` — wraps any `Provider`

> **Historical Python-only example.** The Python Provider/OpenInference wrappers below are not ported or qualified by the current C++ typed cutover. They are not a compatible bridge to the new `ProviderRequest`/owned-outcome contract.

```python
class OpenInferenceProvider(Provider):
    def __init__(self, inner: Provider, tracer: Any,
                 *, span_name: str = "llm.complete"):
        ...
```

On every `complete(params)` call it opens an LLM-kind child span
under the current OTel context (so it nests under whichever node
span is active), captures the OpenInference attributes, delegates
to `inner.complete()`, then closes the span. Tracing failures are
swallowed — observability never breaks the LLM call. Inner-provider
exceptions are re-raised after the span is marked ERROR.

Captured attributes per LLM span:

| Attribute | Source |
|---|---|
| `openinference.span.kind` | constant `"LLM"` |
| `llm.model_name` | `params.model` |
| `llm.invocation_parameters` | JSON blob of `temperature`, `max_tokens`, `top_p`, `frequency_penalty`, `presence_penalty` (when set) |
| `llm.input_messages.{i}.message.role` | `params.messages[i].role` |
| `llm.input_messages.{i}.message.content` | `params.messages[i].content` |
| `input.value` / `input.mime_type` | `params.messages` JSON / `application/json` (Langfuse-compatible blob) |
| `llm.output_messages.0.message.role` | `result.message.role` |
| `llm.output_messages.0.message.content` | `result.message.content` |
| `output.value` / `output.mime_type` | `result.message.content` / `text/plain` |
| `llm.token_count.prompt` | `result.usage.prompt_tokens` |
| `llm.token_count.completion` | `result.usage.completion_tokens` |
| `llm.token_count.total` | `result.usage.total_tokens` |

### End-to-end: NeoGraph + Phoenix in one block

```bash
docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix:latest
pip install neograph-engine opentelemetry-exporter-otlp
```

```python
from opentelemetry import trace
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import BatchSpanProcessor
from opentelemetry.exporter.otlp.proto.grpc.trace_exporter import OTLPSpanExporter
from neograph_engine.openinference import OpenInferenceProvider, openinference_tracer
from neograph_engine.llm import OpenAIProvider
import os
import neograph_engine as ng

provider = TracerProvider()
provider.add_span_processor(
    BatchSpanProcessor(OTLPSpanExporter(endpoint="http://localhost:4317", insecure=True)))
trace.set_tracer_provider(provider)
tracer = trace.get_tracer("my-app")

inner = OpenAIProvider(api_key=os.environ["OPENAI_API_KEY"])
wrapped = OpenInferenceProvider(inner, tracer)
ctx = ng.NodeContext(provider=wrapped)
engine = ng.GraphEngine.compile(graph_def, ctx)

with openinference_tracer(tracer) as cb:
    engine.run_stream(ng.RunConfig(input={"messages": [...]}), cb)

# Open http://localhost:6006 — the trace renders as a chain with
# each LLM call expanded into prompt / response / token counts.
```

Endpoint URL is the only thing you change to point this at Langfuse
self-host instead of Phoenix — both honour OpenInference and OTLP.

### Notes

- **Opt-in dependency.** `opentelemetry-api` is not pulled by the
  base wheel. Importing `neograph_engine.tracing` /
  `.openinference` raises `ImportError` on first use only when the
  package is missing — install with
  `pip install opentelemetry-api opentelemetry-sdk opentelemetry-exporter-otlp`.
- **OTel contextvars across pybind.** The `otel_tracer` in v0.3.x
  documented that `trace.use_span(...).__enter__()` without
  `__exit__()` leaks the contextvar AND doesn't reliably propagate
  through the C++ → Python callback boundary. Both tracers now use
  explicit `otel_context.attach` + `detach` token pairs to control
  current-span activation deterministically.
- **`otel_tracer` vs `openinference_tracer`.** Use the OTel one
  when your backend is APM-shape (Jaeger, Datadog) and you want
  generic spans. Use the OpenInference one when your backend is
  Phoenix / Langfuse / Arize and you want LLM-shape rendering. The
  two can't be combined on the same run — they're alternative
  callbacks for the engine's event stream.

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
| `model` | `std::string` | Model override (empty uses provider default) |

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
| `model` | `std::string` | Model override (empty uses provider default) |
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

### json_path Utilities

**Header:** `<neograph/llm/json_path.h>`
**Namespace:** `neograph::llm::json_path`

Utility functions for navigating and manipulating JSON values using dot-separated
path strings. Available for general JSON use; not the typed SDK request/response codec.

```cpp
namespace json_path {
    std::vector<std::string> split_path(const std::string& path);
    const json* at_path(const json& root, const std::string& path);
    json* at_path_mut(json& root, const std::string& path);
    bool has_path(const json& root, const std::string& path);

    template<typename T>
    T get_path(const json& root, const std::string& path, const T& default_val);

    void set_path(json& root, const std::string& path, const json& value);
}
```

| Function | Description |
|----------|-------------|
| `split_path(path)` | Splits a dot-path string into segments. Example: `"choices.0.message"` becomes `["choices", "0", "message"]` |
| `at_path(root, path)` | Navigates into a JSON value by dot-path. Numeric segments index into arrays. Returns `nullptr` if the path does not exist |
| `at_path_mut(root, path)` | Mutable version of `at_path` |
| `has_path(root, path)` | Returns `true` if the dot-path exists in the JSON value |
| `get_path<T>(root, path, default_val)` | Returns the value at the path converted to type `T`, or `default_val` if the path does not exist or conversion fails |
| `set_path(root, path, value)` | Sets a value at a dot-path, creating intermediate objects as needed |

**Examples:**

```cpp
using namespace neograph::llm::json_path;

json data = json::parse(R"({"choices": [{"message": {"content": "Hello"}}]})");

// Navigate
const json* msg = at_path(data, "choices.0.message.content");
// *msg == "Hello"

// Check existence
bool exists = has_path(data, "choices.0.message.role");
// exists == false

// Get with default
std::string role = get_path<std::string>(data, "choices.0.message.role", "assistant");
// role == "assistant"

// Set value (creates intermediates)
set_path(data, "metadata.version", 2);
```

---

## 13. MCP Module

**Header:** `<neograph/mcp/client.h>`
**Namespace:** `neograph::mcp`

Model Context Protocol (MCP) client implementation. Connects to MCP servers, discovers
available tools, and wraps them as NeoGraph `Tool` instances.

Two transports are available:

- **HTTP** — `MCPClient("http://host:port")`. Discovered tools retain the
  originating Streamable HTTP session, including `Mcp-Session-Id`, negotiated
  protocol version, timeout, and custom headers.
- **stdio** — `MCPClient({"python", "server.py"})`. The client resolves `PATH`
  before `fork`, executes the subprocess with `execve`, wires bidirectional
  pipes, and exchanges newline-delimited JSON-RPC
  over the child's stdin/stdout. The subprocess lives as long as the
  `MCPClient` *or any `MCPTool`* it produced; destruction sends SIGTERM and
  reaps via `waitpid` (SIGKILL fallback after ~500 ms).

### MCPTool

Wraps a single MCP server tool as a local `Tool` implementation. Discovered
tools retain their originating protocol session, regardless of transport.

```cpp
class MCPTool : public AsyncTool {
public:
    // Legacy direct-construction mode. Discovered tools reuse their client session.
    MCPTool(const std::string& server_url,
            const std::string& name,
            const std::string& description,
            const json& input_schema);

    const ToolDefinition& get_mcp_definition() const noexcept;
    CallToolResult execute_result(const json& arguments);
    asio::awaitable<CallToolResult> execute_result_async(const json& arguments);
    ChatTool get_definition() const override;
    asio::awaitable<std::string> execute_async(const json& arguments) override;
    std::string get_name() const override;
};
```

Usually you do not construct `MCPTool` directly — `MCPClient::get_tools()`
discovers and wraps them.

### MCPClient

Client that connects to an MCP server, performs the initialization handshake, and
provides methods to discover and invoke tools.

> `MCPClient` is not designed to be subclassed — you use it as-is.
> `rpc_call_async()` is the real implementation and `rpc_call()` is
> a thin sync facade. See [`ASYNC_GUIDE.md` §9.5](ASYNC_GUIDE.md#95-mcpclient).

```cpp
class MCPClient {
public:
    // HTTP transport.
    explicit MCPClient(const std::string& server_url);
    MCPClient(const std::string& server_url, MCPClientConfig config);

    // stdio transport — fork+exec the subprocess.
    explicit MCPClient(std::vector<std::string> argv);

    bool initialize(const std::string& client_name = "neograph");
    bool is_initialized() const noexcept;
    InitializeResult get_initialize_result() const;
    std::vector<std::unique_ptr<Tool>> get_tools();
    ListToolsPage list_tools(std::optional<std::string> cursor = std::nullopt);
    std::vector<ToolDefinition> get_tool_definitions();
    json call_tool(const std::string& name, const json& arguments);
    CallToolResult call_tool_result(const std::string& name,
                                    const json& arguments);

    // Low-level async dispatch retained for source compatibility.
    asio::awaitable<json>
    rpc_call_async(const std::string& method, const json& params);
};
```

**Wire protocol:** NeoGraph's MCP client speaks
`protocolVersion = "2025-11-25"`. HTTP transport sends the
`MCP-Protocol-Version` header on every JSON-RPC request (Round 1 +
Round 3 spec alignment); stdio transport carries the same version
in the `initialize` payload. Servers running older protocol
versions may reject these requests — pin server-side or upgrade.

**Session and transport ownership:** Both constructors create the same protocol
session. It owns JSON-RPC ids, response validation, initialization (including
the initialized notification), pagination, and tool-result adaptation. The HTTP
transport alone owns endpoint URLs, headers, negotiated `Mcp-Session-Id`, and
request timeout; the stdio transport alone owns the subprocess, pipes, reader,
and response-id demultiplexer. Both advertise concurrent requests, cancellation,
and deadlines to the session. A discovered tool retains its protocol session,
so it remains usable after the `MCPClient` is destroyed; the subprocess is
terminated and reaped when the last client/tool reference is released.

`rpc_call_async(method, params, deadline, cancel_token)` supports an optional
absolute steady-clock deadline and `graph::CancelToken`. Cancellation and
deadlines stop waiting for that call without closing the shared stdio process or
interfering with sibling responses. HTTP additionally applies
`MCPClientConfig::request_timeout` per request. Non-HTTP transport or wire
failures throw `MCPTransportError`, a `std::runtime_error` subclass exposing
`failure()` (`connection`, `timeout`, `cancelled`, `shutdown`, `http_status`, or
`protocol`). HTTP status failures expose `http_status()` (zero otherwise).
Valid JSON-RPC error responses remain `MCPError` with the server's `code()`
and `data()`. Neither class identifies a tool-level `isError` result; use
`call_tool_result()` to inspect that outcome.

| Method | Description |
|--------|-------------|
| `MCPClient(url)` | Construct an HTTP-mode client |
| `MCPClient(argv)` | Spawn a subprocess and construct a stdio-mode client. `argv[0]` is resolved through `PATH` before fork; failed exec surfaces as a connection error on first RPC. Refuses Windows `.bat`/`.cmd` for safety (Round 3 hardening) |
| `initialize(client_name)` | Perform the MCP initialization handshake once. Repeated calls are idempotent; protocol/transport failures throw |
| `get_initialize_result()` | Return negotiated protocol, capabilities, server info, instructions, and raw result |
| `list_tools(cursor)` | Fetch one page while treating the cursor as opaque |
| `get_tool_definitions()` | Follow all pages and preserve complete tool metadata |
| `get_tools()` | Discover all pages and return session-preserving `MCPTool` instances |
| `call_tool(name, arguments)` | Invokes a tool by name with the given arguments. Returns the raw JSON response |
| `call_tool_result(name, arguments)` | Typed result preserving content, structured content, `isError`, and `_meta` |
| `rpc_call_async(method, params)` | Coroutine version. The "real" implementation — `rpc_call` is a thin sync wrapper. |

**HTTP usage:**

```cpp
neograph::mcp::MCPClient client("http://localhost:8000");
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
    std::string model) {
    auto provider = std::make_shared<neograph::llm::SchemaProvider>(
        std::move(descriptor), std::move(options));

    std::vector<std::unique_ptr<neograph::Tool>> tools;
    tools.push_back(std::make_unique<SearchTool>());
    tools.push_back(std::make_unique<CalculatorTool>());

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
             {"routes", {{"yes", "tools"}, {"no", "__end__"}}}}
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
                {{{"channel", "priority"}, {"value", "high"}}} // state updates
            };
        } else {
            result.command = Command{
                "normal_handler",
                {{{"channel", "priority"}, {"value", "normal"}}}
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
JSON-RPC 2.0 over Streamable HTTP. `A2AClient` calls a remote
agent (`message/send`, `tasks/get`, `tasks/cancel`, AgentCard
discovery, `message/stream` SSE); the server side adapts a
NeoGraph `GraphEngine` into an A2A endpoint via
`GraphAgentAdapter`. Dual `v0.3` / `v1` method-name dispatch —
see commit `bc675a1`. Streaming uses `SseFrameSplitter` (client)
and httplib chunked (server). Caller node embeds an A2A call as
a graph node.

**Public headers:** [`include/neograph/a2a/`](../include/neograph/a2a/).

### `neograph::acp` — Agent Client Protocol

**Header:** `<neograph/acp/{server,types}.h>`
Editor↔agent JSON-RPC over newline-delimited JSON on stdio (Zed,
Gemini CLI, Neovim CodeCompanion). Bidirectional: client→agent
(`initialize`, `session/{new,prompt,cancel}`) and agent→client
(`fs/{read,write}_text_file`, `session/request_permission`) via
late-bound `ACPClient`. `ACPServer::handle_message` async-dispatches
prompts on a worker thread, capped at `max_inflight_prompts=32`
with per-session single-flight + `-32000` backpressure.

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

- Optional `ProviderControls` are caller choices, not mandatory defaults or silently clamped caps. Unsupported family controls fail before dispatch. Bounded calls require genuine admitted model input/output facts; missing facts fail with `LimitUnknown`. A reservation is conservative spending authority, not reported usage, a forecast or an invoice. Unknown/partial/delivery-unknown outcomes retain their hold; genuine final reports settle it, including oversized usage. Retry is one explicit layer, off by default, with a bounded window and unknown-prior hold; no hidden resend.
- **`neograph::AsyncTool`** — `Tool` peer that exposes
  `execute_async(json)` for tools whose work is naturally
  coroutine-shaped (HTTP fetch, MCP call). Sync `execute()` is
  `final`-routed through `run_sync`.
- **`neograph::graph::NodeCache`** — per-node memoization, opt-in
  at construction via `EngineConfig::cached_nodes` (the setter remains a
  compatibility surface).
- **`neograph::graph::create_deep_research_graph`** —
  open_deep_research-style supervisor + sub-researcher fan-out,
  used by `examples/25_deep_research.cpp`. Round 2 audit added
  `BriefNode` LLM rewrite, `FinalReportNode` token-limit retry,
  `ClarifyNode` HITL gate.

If the type you need isn't in this tour, check `include/neograph/`
directly. Every public header carries its reference documentation.
