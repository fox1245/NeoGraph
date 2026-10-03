<!-- neograph-i18n: source=docs/reference-en.md locale=zh-CN source_sha256=976c9b048dbd0cb5fcef306e22f4155d5e496ae10e5ed4b846c8d8ab575023b2 -->
# NeoGraph API — 叙述式导览

**Languages:** [English](reference-en.md) | [한국어](reference-ko.md) | [日本語](reference-ja.md) | [简体中文](reference-zh-CN.md)

本文档是 NeoGraph 公开 API 的**引导式叙述导览**，而非完整参考。它按构建
真实 agent 时会遇到的顺序遍历各个模块：基础类型 → provider/tool 接口 →
图类型 → 引擎 → 检查点存储 → 多 LLM → MCP。provider 章节描述 typed 切换，
公开头文件为准；另有部分模块
（`neograph::a2a`、`neograph::acp`、`neograph::async`、
`SqliteCheckpointStore`、`PostgresCheckpointStore`、
`NodeCache`、`AsyncTool`、
`create_deep_research_graph`）具有**本导览未涵盖的头文件中的公开 API**。

> **关于完整、逐类型的 API 表面 — 包括以上每个模块 — 请使用下方链接的
> `include/neograph/` 公共头文件。本叙述式导览是推荐的入口点；
> 头文件是权威参考。**

此拆分换取的好处：叙述保持小到可从头到尾读完，而详细参考与
`include/neograph/` 中的实现保持在一起。

**模块一览：**

| 模块 | 命名空间 | 描述 | 导览 | 头文件 |
|--------|-----------|-------------|------|---------|
| Core | `neograph` | 基础类型、Provider 和 Tool 接口 | [§1–§3](#1-foundation-types) | [Provider](../include/neograph/provider.h) |
| Graph | `neograph::graph` | 图引擎、节点、状态、检查点、存储 | [§4–§11](#4-graph-types) | [GraphEngine](../include/neograph/graph/engine.h) |
| LLM | `neograph::llm` | LLM Provider 实现和 Agent | [§12](#12-llm-module) | [Agent](../include/neograph/llm/agent.h) |
| MCP | `neograph::mcp` | 模型上下文协议客户端 | [§13](#13-mcp-module) | [MCPClient](../include/neograph/mcp/client.h) |
| Util | `neograph::util` | 并发工具 | [§14](#14-util-module) | [RequestQueue](../include/neograph/util/request_queue.h) |
| **A2A** | `neograph::a2a` | Agent 到 Agent JSON-RPC 桥接（客户端 + 服务器 + 流式） | [公开头文件](../include/neograph/) | [A2AClient](../include/neograph/a2a/client.h) |
| **ACP** | `neograph::acp` | Agent 客户端协议 — 编辑器↔agent 通过 stdio 的双向 RPC | [公开头文件](../include/neograph/) | [ACPServer](../include/neograph/acp/server.h) |
| **Async** | `neograph::async` | Asio HTTP/SSE/WS 辅助类、ConnPool、run_sync | [公开头文件](../include/neograph/) | [WsClient](../include/neograph/async/ws_client.h) |

附加模块在 `include/neograph/{a2a,acp,async}/` 提供公开头文件。单独叙述指南已延期，请查阅头文件中的精确契约。文件存在不表示当前集成已获验证。

**便利头文件：** `#include <neograph/neograph.h>` 包含完整的 core + graph engine API。

即使 `NEOGRAPH_BUILD_LLM=OFF`，SchemaProvider 也已是必需的外部 C++ 依赖，因为 Core 公开拥有所有权的 typed provider 契约。请安装 SDK runtime package，将安装 prefix 设为 `SCHEMAPROVIDER_PREFIX`；下方 configure 使用 `-DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX"`。也可用 `-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=../SchemaProvider` 明确指定 checkout。不会自动选择猜测的 sibling checkout 或旧 bundled interpreter。当前 SDK runtime/archive 支持 Linux/POSIX；不承诺无依赖、无需 OpenSSL、native Windows/macOS 或 WASM runtime。

SDK imported target 提供 `include/SchemaProvider` include root；公开示例直接使用 `<descriptor/descriptor.h>`、`<runtime/client.h>`、`<neograph/llm/schema_provider.h>`，不依赖 recipe 专用 helper。

```cmake
find_package(SchemaProvider CONFIG REQUIRED COMPONENTS runtime)
find_package(NeoGraph CONFIG REQUIRED)
target_link_libraries(app PRIVATE neograph::core neograph::llm SchemaProvider::runtime)
```

---

## 目录

- [1. 基础类型](#1-foundation-types)
  - [ToolCall](#toolcall)
  - [ChatMessage](#chatmessage)
  - [ChatTool](#chattool)
  - [Owned Outcome](#owned-outcome)
  - [Portable projections](#portable-projections)
  - [ADL 序列化](#adl-serialization)
- [2. Provider 接口](#2-provider-interface)
  - [ProviderRequest / ProviderControls](#providerrequest--providercontrols)
  - [PreparedProviderRequest / ProviderBudgetClaim](#preparedproviderrequest--providerbudgetclaim)
- [3. Tool 接口](#3-tool-interface)
  - [Tool](#tool)
- [4. 图类型](#4-graph-types)
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
  - [GraphNode（抽象）](#graphnode-abstract)
  - [LLMCallNode](#llmcallnode)
  - [ToolDispatchNode](#tooldispatchnode)
  - [IntentClassifierNode](#intentclassifiernode)
  - [SubgraphNode](#subgraphnode)
- [7. GraphEngine](#7-graphengine)
  - [EngineConfig 和 EngineResources](#engineconfig-and-engineresources)
  - [RunConfig](#runconfig)
  - [RunResult](#runresult)
  - [GraphEngine](#graphengine)
- [7b. 引擎内部](#7b-engine-internals)
  - [GraphCompiler](#graphcompiler)
  - [Scheduler](#scheduler)
  - [CheckpointCoordinator](#checkpointcoordinator)
  - [NodeExecutor](#nodeexecutor)
- [8. 检查点](#8-checkpoint)
  - [Checkpoint（结构体）](#checkpoint-struct)
  - [CheckpointStore](#checkpointstore)
  - [InMemoryCheckpointStore](#inmemorycheckpointstore)
- [9. Store](#9-store)
  - [Namespace](#namespace)
  - [StoreItem](#storeitem)
  - [Store（抽象）](#store-abstract)
  - [InMemoryStore](#inmemorystore)
- [10. Loader](#10-loader)
  - [ReducerRegistry](#reducerregistry)
  - [ConditionRegistry](#conditionregistry)
  - [NodeFactory](#nodefactory)
  - [内置注册](#built-in-registrations)
- [11. React Graph](#11-react-graph)
- [12. LLM 模块](#12-llm-module)
  - [SchemaProvider](#schemaprovider)
  - [Agent](#agent)
  - [json_path 工具](#json_path-utilities)
- [13. MCP 模块](#13-mcp-module)
  - [MCPTool](#mcptool)
  - [MCPClient](#mcpclient)
- [14. Util 模块](#14-util-module)
  - [RequestQueue](#requestqueue)
- [使用示例](#usage-examples)
  - [最简 ReAct Agent](#minimal-react-agent)
  - [带条件路由的自定义图](#custom-graph-with-conditional-routing)
  - [带检查点的人类参与](#human-in-the-loop-with-checkpointing)
  - [使用 Send 的动态扇出](#dynamic-fan-out-with-send)
  - [使用 Command 的路由覆盖](#routing-override-with-command)
  - [SchemaProvider 多 LLM 支持](#schemaprovider-multi-llm-support)
  - [MCP 工具集成](#mcp-tool-integration)

---

<a id="1-foundation-types"></a>
## 1. 基础类型

**头文件：** `<neograph/types.h>`
**命名空间：** `neograph`

跨所有模块共享的核心数据类型。这些模型化了 LLM 聊天协议：消息、工具调用、
补全及其 JSON 序列化。

### ToolCall

表示 LLM 请求的单次工具调用。

```cpp
struct ToolCall {
    std::string id;         // Unique identifier assigned by the LLM
    std::string name;       // Name of the tool to call
    std::string arguments;  // JSON-encoded string of arguments
};
```

| 字段 | 类型 | 描述 |
|-------|------|-------------|
| `id` | `std::string` | 本次工具调用的唯一标识符（LLM 分配） |
| `name` | `std::string` | 要调用的工具函数名称 |
| `arguments` | `std::string` | 包含调用参数的 JSON 编码字符串 |

### ChatMessage

对话中的单条消息。覆盖所有角色：system、user、assistant 和 tool。

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

| 字段 | 类型 | 描述 |
|-------|------|-------------|
| `role` | `std::string` | 消息角色：`"system"`、`"user"`、`"assistant"` 或 `"tool"` |
| `content` | `std::string` | 消息的文本内容 |
| `tool_calls` | `std::vector<ToolCall>` | assistant 请求的工具调用（非 assistant 消息为空） |
| `tool_call_id` | `std::string` | 将此工具结果链接到其原始工具调用的 ID |
| `tool_name` | `std::string` | 产生此结果的工具名称 |
| `image_urls` | `std::vector<std::string>` | 多模态/视觉消息的图像 URL。接受 `data:image/...;base64,...` 或 `https://...` |

### ChatTool

定义 LLM 可用的工具。

```cpp
struct ChatTool {
    std::string name;        // Tool name (unique identifier)
    std::string description; // Human-readable description for the LLM
    json parameters;         // JSON Schema describing the tool's parameters
};
```

| 字段 | 类型 | 描述 |
|-------|------|-------------|
| `name` | `std::string` | 唯一的工具名称 |
| `description` | `std::string` | 显示给 LLM 以解释工具用途的描述 |
| `parameters` | `json` | 描述可接受参数的 JSON Schema 对象 |

### Owned Outcome

提供方调用返回 `sp::runtime::Result`，即持有 `sp::Completion` 或 `sp::Failure` 的不可变、拥有所有权的 `std::shared_ptr<const sp::Outcome>`。请保留完整结果，而非仅显示文本。顺序消息/part、native continuation、完整 wire envelope、顺序 raw 观测、停止依据及真实尝试元数据在调用与客户端销毁后仍然保留。使用量是带依据、阶段、质量的 nullable `uint64_t`；缺失表示未知，绝不是零。失败保留原始部分结果。`ProviderFailure::outcome()` 与 `ProviderObserverError::outcome()` 保留真实结果，后者的 `cause()` 也保留观察者异常。

### Portable projections


实际结果存在后，若 post-effect 结算或 terminal receipt 持久化失败，`ProviderDispatchOutcomePersistenceError::outcome()` 保留原始不可变结果，`cause()` 保留原始持久化异常。若 delivery 也失败，`delivery_error()` 保留原始观察者异常。持久化成功后的观察者失败原样重新抛出原异常；未知/无结果 transport 失败不会伪造 outcome。
`ChatMessage` / `ChatTool` 和 JSON 只是 portable projection，不是 native 权限。Portable 格式仍为 [`provider-message-v2`](../schemas/provider-message-v2.schema.json)、[`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json)。真实 C++ checkpoint sidecar 保留内存 native seal。持久 native 历史需要 host-owned `sp::NativeArchive`：closed v3 / `spna3` 使用独立密钥提供经认证的 owner-private custody；archive v2 被拒绝，不升级或解释。认证绑定全部 semantic descriptor 选择（origin/path/header、policy、请求 field mapping、usage path、stop mapping）、owner 和精确 custody binding。这不是加密或 vendor-issuer 认证；不得公开 archive 正文、密钥、native blob 或 raw wire 观测。Archive 是证据存储，不是资金 grant 或 spending lease。Program/external bank 仍由独立 journal 拥有，复制 snapshot 不能创建 credit。

**Standalone bank journal 修正——当前契约已修订；实际 runtime 证据如下。** Owner-approved protocol 要求单调 trusted-store namespace obligation，以及真实不可变 original owner/thread/graph scope、ceiling、deadline/clock identity、generation。只有对全部 checkpoint commitment/revision 的精确 durable head CAS 才可发放 host-owned opaque lease。精确 pending effect window 必须在 provider I/O 前持久化；结算必须采用真实 SDK outcome 及实际 charge、nullable report、hold、dedup identity。Checkpoint 与 next head 必须在同一 owned actor/revision 下原子 publish。删除 bank metadata、prune checkpoint、replay old authenticated snapshot、覆盖同一 ID 或失去 actor 都不能授予 credit。已有 65 hold 时将 ceiling 130 降至 129，不能再批准另一个 65；已证明 no-effect 的失败可 release unchanged head，使 authentic 130 恢复仍可进行。Crash/unknown/lost-lease window 保持 hold，不 refund/retry/fallback。Plain/pristine archive 配置不授予 money/native spending lease；当前 `config.usage` 不能替换既有 standalone obligation，Program/external-bank journal 所有权不变。这是要求契约。实际 currency/custody 证据与 instrumentation 限制见下文，不是稳定 released API 保证。

**当前声明；集成 runtime 证据如下:** `<neograph/graph/checkpoint.h>` 声明 `ManagedBudgetLeaseScope`，包含 `owner_scope`、logical `thread_id`、private backend `storage_thread_id`、`graph_identity`、`original_ceiling`、`original_deadline_ticks`、`deadline_clock_identity`。`OwnedManagedBudgetLease` 暴露 read-only `scope()`、`actor_id()`、不可变 `bank_generation()`、`revision()`、`head_checkpoint_id()`、`head_commitment()`，没有公开 authority-import constructor。`ManagedBudgetEffectReceipt` 暴露 `active()`、`effect_id()`、`claim_amount()`、`request_digest()`；default receipt 不授予权限。`CheckpointStore` 声明 `acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`、`begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`、`settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`、`publish_managed_budget_checkpoint(lease, checkpoint)`、`release_managed_budget_lease(lease)` 及 `_async` counterpart。Sync `CheckpointStoreCore` 与 `AsyncCheckpointStore` 暴露各自 variant。`managed_budget_checkpoint_commitment(checkpoint)` 绑定完整持久 checkpoint，而非仅 bank JSON。这些声明不证明 backend CAS、currency 安全性、installed ABI 兼容性或实际成功的 runtime 路径。

**真实 InMemory shared-bank fork 已保留并实证。** 原始真实 C++ fork 使用 ONE original financial journal 和 trusted current branch head，不复制 grant。`publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)` 及 `_async` 要求 authentic current source/full commitment 与实际 same-bank native C++ pointer；durable standalone fork 仍明确 unsupported。`OwnedManagedBudgetLease::scope()` 及 original owner/thread/graph、ceiling、deadline/clock、generation 保持不可变。Read-only store-issued `execution_thread_id()` / `execution_storage_thread_id()` 单独选择 execution branch；`GraphState::budget_original_thread_id()` 标识原始 financial bank。精确 selected-branch head CAS 和 global actor/revision 将所有 branch 对 canonical current counter、pending effect、burned identity 串行化。Original/fork branch 保持可用但不补充额度。Stale snapshot、checkpoint copy、imported JSON 不能发放 alias 或回退 head。原始 root30 → charge3 → original continuation6 → fork lower20 → continuation9 same-bank 证明在未修改 test_graph_engine.cpp:810–913 中 PASSED；saved original ceiling30 不同于 effective fork ceiling20；widening31 和 JSON-only restore 必须拒绝。Unbounded reported observation 是事实 data，不是 finite grant。只有已证明 zero-effect 的 lease 能 release unchanged head；unknown/pending effect 保留 obligation。

**当前 release-error 契约；实际 suite/probe 如下。** `<neograph/graph/engine.h>` 中 `graph::ManagedBudgetLeaseReleaseError` 继承 `ProviderOutcomeError`。`cause()` 保留原始 execution exception，`release_error()` 暴露次要 durable lease-disposition 失败。`outcome()` 在存在真实 SDK 证据时保留它，若没有 SDK outcome 则为 null；release 失败不能伪造结果或授权重新 dispatch。Closed `_neograph_managed_budget_scope` metadata 描述原始 logical scope/cap/deadline clock/generation，但只是 data，不是 backend CAS 权限。

**Archive-owner/retention 契约；实际 suite/probe 如下。** 只有 finite standalone root 或 authenticated finite source 才从真实配置的 `sp::NativeArchive::owner_scope()` 继承省略的 original owner；unbounded/plain owner metadata 语义不变。显式冲突的 archive owner 在 lease acquire 前拒绝。`CheckpointStore::retains_native_checkpoint() const noexcept` 及对应 Core/Async storage capability 默认 false；真实 InMemory backend override 为 true，wrapper 必须委托真实 retention。此 read-only 描述允许合法 unleased/plain/unbounded C++ native checkpoint custody，但不授予 spending credit 或 native replay authority。Leased custody 使用真实 store-issued receipt，而非 JSON flag 或猜测的 store type。

**Native-custody pre-I/O gate；实际 suite/probe 如下。** Managed effect begin 在任何 pending-effect/slot/held-window 修改前要求真实绑定的 NativeArchive 或实际 local store-issued private C++ retention capability。Private capability 不从 JSON import，也不经 wire 传输。C++ sidecar 无法跨越边界，因此即使 remote backend 是 InMemory，gRPC 仍要求真实 client/server archive。Archive 未提供 finite source owner 时，原始 anonymous owner scope 保持空值；真实 archive binding 必须匹配 original scope。Financial head/lease 证据本身不证明 native-custody readiness。

诊断 JSON 保留原始 raw byte，包括语法有效的 duplicate-key 文档；可执行请求/config admission 仍拒绝重复键。原始 non-2xx 响应 JSON 保留在 `http.error` 证据中，不进行第二次有损 parse。named SSE error 优先于之后的正常 stream close。诊断/provider metadata 按获准 source extent 限制，而非无关的小 error-text cap。

`ProviderRequest::observer_limits` 仅供宿主使用。显式 `max_events`、`max_bytes` 必须为正且只能降低获准 SDK 交付上限。`provider-request/v3` digest 绑定实际 limit、mode、encoded body、retry policy 及全部 semantic descriptor binding。Bridge 在 queued/draining batch 中同时计入真实 PMR vector/map capacity 与拥有的 event/document byte，并在 queue mutex 之外请求 cancellation。名为 `messages` 的 Generic channel 不会被强制转成 chat。将 native `history` channel mapping 到 `messages` 会保留 C++ sidecar，而不是从 JSON 制造 native 权限。

`ProviderOutcomeError` 是保留结果的共同 host-error base；`ProviderObserverError` 和 `ProviderDispatchOutcomePersistenceError` 保留完整 drain 后的 SDK 结果及原始 `cause()`，后者还通过 `delivery_error()` 保留次要 observer 失败。`ProviderFailure::outcome()` 保留 SDK 失败本身。这些证据不授权 Node/Program 重新 dispatch。SDK 是 provider retry 的唯一所有者，调用方选择的 `max_output_tokens` 不会被暗中 clamp。

`ProgramFailure` 保留 live `provider_outcome`、`provider_cause`。Canonical factual SDK witness 将真实 archive custody 绑定到 owner/run/version/bundle/operation/attempt；Runtime 在暴露恢复后的失败前立即恢复配置的 custody。公开 data-only `ProgramResult::create()` 不能用预填 witness 绕过；未解析完的 parsed seal 不是可执行结果。进程重启后原始 exception pointer 不可用（`provider_cause == nullptr`），不会从 text 重建。无法持久化的失败不能 serialize/publish/replay。

`RecordedBindingSet` 是 source-bound move-only data，不是调用方提供的 dispatcher。可信 Catalog `recorded_capability_binder` 独立读取真实持久 source event，materialize captured-only capability。`ProgramRuntime::replay_recorded()` 检查原始 selected-source permission，再通过 durable CAS 转移真实剩余 bank；inherited spend 不是新的 model grant。旧 `start_recorded` 续期 API 已删除。InMemory/File/SQLite/PostgreSQL Program store 在整个执行期间保留精确不可变 owned lease，不因 expiry 续期。Controlled JavaScript 仍验证 underlying capability manifest，消费精确 completed command 结果，不重新 dispatch external effect。

**Recorded-control causal fix 已在 full suite 实证。** Captured command replay 在执行前仅为新的 CPU wall-time/Core work 建立 durable reservation，再通过 result CAS publish 测量 work 与新产生的 Core checkpoint。不消耗新的 model、money、Program-operation allowance，也不重新 dispatch captured external effect。未结算 reservation 保持 debit。Reservation 选择认证 settlement transition，而非曾拒绝首个新 Core checkpoint 的普通 Running→Running transition。Await channel receive、timer wait/cancel、handoff wait 的开始/release 在所属 executor/strand 上串行化；既有 Recorded CPU/Memory await/handoff scenario 在 full suite pass；remote TSan coverage 限制如下明确保留。

**付费观测已完成；不是普遍 qualification。** 原始 `SPQUAL1` base630/1000000 microUSD 不变；同一原始 ledger 中 ONE hash-chained `A` 接纳批准的 extension480/3000000，aggregate1110/4000000。Calls/spent/hold/settlement 累积，不产生新 grant ID/header/reset。精确 declaration byte/file identity 和 original authorization/baseline/catalog/activation/ledger-prefix hash/totals 仍固定；删除、替换、变更均 fail closed。最终 canonical ledger 为 calls1110/spent437958/held1287828 microUSD、eventA1、limits1110/4000000；spent+held US$1.725786 是 LOCAL catalogue meter，不是 invoice。记录的 five-family60-pair baseline 完成600 request：Chat60/60、Responses60/60、Messages60/60、Generate56/60（incorrect-vision SSE4次）、Interactions57/60（incorrect-vision buffered1次/SSE2次）；合计293/300 pair，不是300/300。其他 old600 financial record 保留，但不是完整 behavioral proof。此前 M5/media one-shot cohort 不变。此前 Google3-round prerequisite 保留 invalid-tool2次/unreadable-positive1次失败状态。不批准更多付费调用。最终 SDK 证据与 native-axis 限制不同于 baseline 成功。 此前 activation/reopen smoke 保留为两次 reopen 后 calls610/spent219159/held751233、SDK meter/canary/vision4-test19.38秒 pass；这是限定的历史 checkpoint，不是最终 ledger totals。此前验证的 Chat60-pair cohort 保留实际 attempt120、UpperBound charge120、无 UnknownHold。

**Native-axis 观测不是 cryptographic 验证或 native consumption/equivalence。** Generate 接纳 mutation/omission/duplication。Interactions 接纳 isolated genuine source/positive control、one-owner signature mutation、thought-carrier omission、call-carrier omission、duplication。删除全部 thought/signature 返回 generic400；保留 THOUGHT item 而删除全部 signature field 也返回 generic400。最后一次 capture 只有 local encoded-original retention control，没有 same-capture server positive；此前 positive cohort 仍是真实证据。这仅建立 aggregate-carrier-absence boundary，不证明 issuer/signature 验证或 vendor consumption。实际 report：SDK `config/qualification-extension-results.json`、`qualification-final-summary.json`、`qualification-native-axis-results.json`、`qualification-combined-omission-results.json`、`qualification-signature-presence-results.json`；prerequisite-failed/not-run/negative-inconclusive 状态保持为事实。 Thought-only/carrier-only omission 在仍有其他 carrier 时被接纳；这不加强 issuer-validation/native-consumption 声明。

**实际集成证明及剩余限制。** 最新 Core full run：2242 test、失败0、skip16（RAM process-loss 不适用14项/live-credential gate2项）、130.17秒。`PgNestedJsonRoundTrips` 精确保留 duplicate key/order/null metadata、blob、residual，0.18秒 pass。未修改的原始 shared-bank fork 和既有 Recorded CPU/Memory await/handoff scenario 均 pass。真实 wrappedMemory/SQLite/PostgreSQL/gRPC finite130/hold65/lower129/strip/old-head/pruning/no-archive/import probe 在 plain 和 ASan+UBSan pass。LOCAL Memory/SQLite/PostgreSQL TSan scope7项 pass、warning0。包含 system Abseil/Protobuf 的 full mixed gRPC TSan 为 exit66，dependency/generated-RPC stack 有 race warning402项。这是 instrumentation/coverage 限制，不是已证明的 false positive；不声称 remote TSan/race-free，不 suppress warning。Installed find_package Program C++/C ABI/dualQuickJS3个 consumer pass。Fresh installed NeoGraph/SchemaProvider typed consumer 实际2个 HTTP request、coroutine 开始前 provider 销毁、native/tool replay、refusal、known-zero/raw 保留、实际 LinkedMismatch 拒绝均 pass。Browser Alice/Bob isolation、generation2 replacement 已实际目视验证；PostgreSQL Program Chat black-box6项18.989秒 pass。最新 SDK26/26、失败0、74.07秒 pass。最终 ReleaseGraph16配置 ×fresh process3次/48记录以38.29秒、失败0、全部 actual protocol/owned-outcome check pass 完成。NeoGraph `benchmarks/provider-cutover-final-results.json` 和 `benchmarks/provider-cutover-final-summary.json` 保留独立最终 cohort。测量期间未执行 compiler/付费 model；历史 cohort 不变，不声明 semantic/resource equivalence。Unstable SDK/ABI3 不是稳定 release 或更广 platform qualification。

**最小付费证据（2026-10-03）不是广泛 qualification。** 分别批准的三次 one-shot 调用结果：Images—1 个 JPEG，1024×1024，360685 byte，input/output/total token 19/1408/1427，已实际目视检查；Veo—1 个 MP4，1280×720，4 秒，437737 byte，1 次 generation 加 3 次 status GET，usage nullable，在 Chromium 中 decode 并目视检查；Decisions—`typesafe/jev-1.13`，probability 0.93，input/output token 283/21，total 未知，API 报告费用 USD 0.000011886。Image USD 0.0336 base 加 text/thinking、Veo USD 0.20 是 catalog 预期，不是 invoice；最小 image smoke 未取得价格档位分解。结果不续期 one-shot 权限，也不授权重跑。

已完成的 chat pair 不证明 downstream vendor 对 native-continuation 的消费。

```cpp
#include <neograph/types.h>

neograph::json observe_result(const sp::runtime::Result& result) {
    if (!result) throw std::invalid_argument("Missing provider outcome");
    return neograph::outcome_projection_json(*result);
}
```

### ADL 序列化

用于 nlohmann/json 集成的参数依赖查找（ADL）序列化函数。这些允许直接使用
`json j = my_tool_call;` 和 `my_tool_call = j.get<ToolCall>()`。

```cpp
void to_json(json& j, const ToolCall& tc);
void from_json(const json& j, ToolCall& tc);

void to_json(json& j, const ChatMessage& msg);
void from_json(const json& j, ChatMessage& msg);
```

所有字段使用带空字符串默认值的 `value()`，使反序列化对缺失字段具有容错性。

---

<a id="2-provider-interface"></a>
## 2. Provider 接口

公开契约是拥有所有权的 typed 准备/dispatch，而非同步/异步 virtual completion 对。`ProviderRequest.payload` 是 Chat、Messages、Responses、Gemini、Interactions 的 SDK 请求 variant。`ProviderMode::Collect` / `Stream` 独立于观察者是否存在来选择传输。`on_event` 接收借用的 typed `sp::Event` view；只复制回调后仍需要的数据。不允许 raw JSON override 或通过 portable projection 导入 native 权限。

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
`prepare()` 恰好验证、编码一次，生成保持原始 deadline 与取消状态的仅可移动 `PreparedProviderRequest`。持久调用方将 `Provider::request_digest()` 绑定到 assembly，预留获准的 budget claim，写入 dispatch receipt，然后通过 `ControlledProvider::dispatch_prepared(_async)` 消费同一个 handle。gate 之后不重建请求。重复 receipt 绝不重新 dispatch。自定义实现提供 `get_name()`、`family()`、`prepare()` 并使用 `prepare_runtime()` 或 `prepare_local()`；local callback 捕获拥有所有权的 shared 状态，而非 `this`。

可选 `ProviderControls` 是调用方选择，不是强制默认值或暗中 clamp 的 cap。不支持的 family 控制在 dispatch 前拒绝。有界调用需要获准的真实模型 input/output 上限；缺失时为 `LimitUnknown`。预留是保守的支出权限，而非报告使用量、预测或账单。未知/部分/delivery-unknown 结果保留 hold，真实最终报告用于结算，超额报告也全额计入。retry 是显式单层，默认 off，具有有界 window 与 unknown-prior hold；没有隐藏重发。

`provider_failure_proves_not_sent(Failure)` 要求完整、无矛盾的 NotSent 依据。仅状态码、使用量缺失或观察者/持久化异常不能证明零成本或更新预算。

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


这是源码和二进制破坏性变更；所有 C++ 使用者与自定义提供方都必须使用匹配的新头文件/库重新编译。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、descriptor interpreter 和 Responses WebSocket 已删除，没有 alias 或兼容 bridge。SDK 为不稳定 `0.0.0`、interface revision 3 / shared ABI 3，使用 out-of-line capability check，不表示稳定发布。当前 runtime/archive 为 Linux/POSIX，不代表 Windows、macOS、WASM runtime 已获验证。Python provider binding/wrapper 已延期，不由本 C++ 变更完成移植。

Fresh installed find_package Program C++/C ABI/dualQuickJS consumer 与 NeoGraph/SchemaProvider typed2-request lifetime/native/raw/mismatch consumer pass。Interface/ABI 声明本身不同于实际 package 结果；不声称更广 platform 或稳定 release。

---

## 3. Tool 接口

**头文件：** `<neograph/tool.h>`
**命名空间：** `neograph`

LLM 可调用的工具的抽象接口。实现此接口以向 agent 暴露函数。

> **正在编写自定义 Tool 子类？** 关于何时继承 `Tool`（同步）与 `AsyncTool`
> （异步），参见 [`ASYNC_GUIDE.md` §9.6](ASYNC_GUIDE.md#96-tool-vs-asynctool)。
> 两者互斥 — 选择一个。

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

| 方法 | 返回 | 描述 |
|--------|---------|-------------|
| `get_definition()` | `ChatTool` | 返回包含参数 JSON Schema 的工具元数据 |
| `execute(arguments)` | `std::string` | 以解析后的 JSON 参数运行工具。以字符串形式返回结果，该结果将发回 LLM |
| `get_name()` | `std::string` | 此工具的唯一标识符 |

**示例实现：**

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

<a id="4-graph-types"></a>
## 4. 图类型

**头文件：** `<neograph/graph/types.h>`
**命名空间：** `neograph::graph`

图引擎的核心类型：通道、边、事件和控制流原语。

### ReducerType

确定通道值在被多个节点写入时如何合并。

```cpp
enum class ReducerType {
    OVERWRITE,  // New value replaces old value
    APPEND,     // New value is appended (for array channels)
    CUSTOM      // User-defined reducer function
};
```

### ReducerFn

自定义 reducer 函数的签名。

```cpp
using ReducerFn = std::function<json(const json& current, const json& incoming)>;
```

| 参数 | 描述 |
|-----------|-------------|
| `current` | 当前通道值 |
| `incoming` | 正在写入的新值 |

**返回：** 成为新通道值的合并结果。

### Channel

具有关联 reducer 的命名、带版本的状态通道的内部表示。

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

针对命名通道的单次写入操作。节点返回这些的向量。

```cpp
struct ChannelWrite {
    std::string channel;  // Target channel name
    json        value;    // Value to write (merged via the channel's reducer)
};
```

### NodeInterrupt

从节点内部抛出的异常类型，用于触发动态断点（人类参与）。当被抛出时，执行
暂停，保存检查点，中断可在以后恢复。

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

| 方法 | 返回 | 描述 |
|--------|---------|-------------|
| `reason()` | `const std::string&` | 传递给构造函数的 reason 字符串 |
| `value()` | `const json&` | 结构化 payload，如果未附加则为 null |
| `node()` | `const std::string&` | 抛出的节点。执行器打戳 — 节点体不知道图定义中是如何称呼它的 |

**往返过程。** 审批提示需要信息双向传输：节点说*什么*需要审批，人类的
答案必须返回到发起请求的节点。

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

调用者将暂停视为正常的 `RunResult` — `NodeInterrupt` 不会向他们重新抛出：

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

当图有 `messages` 通道时，`resume_value` 也作为一个用户轮次到达，这是聊天
形态图始终接收它的方式。`ctx.resume_value` 是通用路径 — 无论图的通道如何
命名，它都能工作。

这是*动态*形式的中断。*静态*形式 — 图定义中的 `interrupt_before` /
`interrupt_after` — 在编写图时选择的节点处暂停，无法表达"仅当模型请求
危险操作时才暂停"。

### Send

表示动态扇出请求。节点可以返回 `Send` 对象以用不同输入分发一个或多个节点，
实现 map-reduce 模式。

```cpp
struct Send {
    std::string target_node;  // Node to dispatch
    json        input;        // Channel writes for that invocation
};
```

引擎使用各自的输入执行每个 `Send` 目标，然后在所有 send 完成后继续图。
对同一节点的多个 send 按顺序运行。

### Command

组合的路由覆盖和状态更新。节点返回 `Command` 以同时写入状态更新并将执行
重定向到特定的下一个节点，绕过正常的边路由。

```cpp
struct Command {
    std::string               goto_node;  // Next node (overrides edge routing)
    std::vector<ChannelWrite> updates;    // State updates to apply
};
```

| 字段 | 类型 | 描述 |
|-------|------|-------------|
| `goto_node` | `std::string` | 下一个要执行的节点名称。覆盖正常的边解析 |
| `updates` | `std::vector<ChannelWrite>` | 在路由之前应用的通道写入 |

### RetryPolicy

为节点执行失败配置自动重试行为。

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

第 `n` 次重试的延迟为
`min(initial_delay_ms * backoff_multiplier^n, max_delay_ms)`，当
`jitter_pct > 0` 时可选地乘以 `1 + uniform(-jitter_pct, +jitter_pct)`。

### StreamMode

位标记，控制在流式执行期间发出哪些事件。

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

按位或组合标志：

```cpp
StreamMode mode = StreamMode::EVENTS | StreamMode::TOKENS;
```

**运算符：**

```cpp
StreamMode operator|(StreamMode a, StreamMode b);  // Combine flags
StreamMode operator&(StreamMode a, StreamMode b);  // Mask flags
bool has_mode(StreamMode flags, StreamMode test);   // Test if flag is set
```

### Edge

两个节点之间的静态有向边。

```cpp
struct Edge {
    std::string from;  // Source node name
    std::string to;    // Target node name
};
```

使用特殊常量 `START_NODE` 和 `END_NODE` 用于图的入口和出口点。

### ConditionalEdge

动态边，其目标在运行时由命名条件函数确定。

```cpp
struct ConditionalEdge {
    std::string from;                              // Source node name
    std::string condition;                         // Name in ConditionRegistry
    std::map<std::string, std::string> routes;     // condition_result -> target node name
};
```

运行时，引擎调用条件函数（通过名称在 `ConditionRegistry` 中查找）。函数的
返回值被用作 `routes` 映射中的键来确定下一个节点。

### NodeContext

传递给节点构造函数的依赖注入容器。提供对 LLM provider、工具和配置的访问。

```cpp
struct NodeContext {
    ProviderControls provider_controls;
    std::shared_ptr<Provider> provider;   // LLM provider
    ToolSet                  tools;      // 持有的固定工具集合
    std::string               model;      // Model override (empty = provider default)
    std::string               instructions; // System prompt / instructions
    json                      extra_config; // Additional configuration (node-type-specific)
};
```

将 `ToolSet(std::move(tools))` 赋给 `NodeContext::tools`，或者在上下文工具为空时
通过 `EngineResources::tools` 提供。编译结果及引擎共同持有同一批工具；
重新赋值上下文不会改变已有引擎。工厂可用 `ctx.tools.view()` 临时查找指针。
Python 和 MCP 工具遵守相同的编译期所有权规则。

### GraphEvent

在流式图执行期间发出的事件。

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

**事件数据 payload：**

| 类型 | `data` 内容 |
|------|-----------------|
| `NODE_START` | `{}` 或节点元数据 |
| `NODE_END` | 节点产生的通道写入 |
| `LLM_TOKEN` | `{"token": "..."}` |
| `CHANNEL_WRITE` | `{"channel": "...", "value": ...}` |
| `INTERRUPT` | `{"reason": "...", "node": "..."}` |
| `ERROR` | `{"error": "...", "node": "..."}` |

### GraphStreamCallback

用于流式执行的图事件回调的类型别名。

```cpp
using GraphStreamCallback = std::function<void(const GraphEvent&)>;
```

`GraphEvent` 仍然是稳定的回调和面向 JSON 的形态。想要类型化 payload 的
代码可以在不更改引擎入口点的情况下适配相同的流：

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

`to_typed_event()` 直接执行转换。畸形的 payload 和未来版本引入的 payload
形态成为 `RawGraphEvent`，而非从流式回调中抛出。

### NodeResult

节点执行的扩展返回类型。将通道写入与可选 `Command` 和 `Send` 指令包装在
一起，用于高级控制流。

```cpp
struct NodeResult {
    std::vector<ChannelWrite> writes;           // Channel updates
    std::optional<Command>    command;           // Routing override (if set)
    std::vector<Send>         sends;             // Dynamic fan-out targets

    NodeResult() = default;
    NodeResult(std::vector<ChannelWrite> w);     // Implicit from plain writes
};
```

当 `command` 被设置时，正常的边路由被绕过，执行跳转到 `command->goto_node`。
当 `sends` 非空时，引擎对指定目标执行动态扇出。

### ConditionFn

用于条件边中的条件函数的签名。

```cpp
using ConditionFn = std::function<std::string(const GraphState&)>;
```

该函数检查当前图状态并返回一个字符串键。该键在 `ConditionalEdge::routes`
映射中查找以确定下一个节点。

<a id="constants"></a>
### 常量

```cpp
constexpr const char* START_NODE = "__start__";  // Graph entry point
constexpr const char* END_NODE   = "__end__";    // Graph termination
```

这些用于边定义中以标记图入口和出口：

```cpp
Edge{START_NODE, "my_first_node"}
Edge{"my_last_node", END_NODE}
```

---

## 5. GraphState

**头文件：** `<neograph/graph/state.h>`
**命名空间：** `neograph::graph`

图的线程安全、带版本的键值状态容器。每个条目是一个命名通道，具有关联的
reducer，控制值如何合并。

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

| 方法 | 描述 |
|--------|-------------|
| `init_channel(name, type, reducer, initial_value)` | 注册一个通道及其 reducer 和可选的初始值。必须在对该通道进行任何读/写之前调用 |
| `get(channel)` | 读取通道的当前值。线程安全（共享锁） |
| `get_messages()` | 便利方法：读取 `"messages"` 通道并将其反序列化为 `std::vector<ChatMessage>` |
| `write(channel, value)` | 通过其 reducer 向单个通道写入值。线程安全（独占锁） |
| `apply_writes(writes)` | 原子地应用批量 `ChannelWrite` 操作。所有写入在单个独占锁下应用 |
| `channel_version(channel)` | 返回特定通道的写入计数 |
| `global_version()` | 返回全局版本计数（每次向任何通道写入时递增） |
| `serialize()` | 将所有通道值和版本序列化为 JSON（用于检查点） |
| `restore(data)` | 从序列化的 JSON 恢复通道值和版本 |
| `channel_names()` | 返回所有已初始化通道的名称 |

---

## 6. GraphNode

**头文件：** `<neograph/graph/node.h>`
**命名空间：** `neograph::graph`

节点是图的计算单元。库提供了一个抽象基类和四种内置节点类型。

<a id="graphnode-abstract"></a>
### GraphNode（抽象）

子类重写一个方法：`run(NodeInput) -> awaitable<NodeOutput>`。读取状态，
决定做什么，返回写入（以及可选的 `Command` / `Send`）。

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

| 成员 | 描述 |
|--------|-------------|
| `in.state` | 只读 `GraphState`。使用 `in.state.get(channel)` 读取 |
| `in.ctx.cancel_token` | 传递给 `provider.invoke(std::move(request))`，使 LLM HTTP 套接字在取消时中止，或轮询 `ctx.cancel_token->is_cancelled()` 用于自己的循环 |
| `in.ctx.step` | 当前超级步骤索引 |
| `in.ctx.thread_id` | 镜像 `RunConfig::thread_id` |
| `in.stream_cb` | 流式接收器；如果非空，通过它发出 `LLM_TOKEN` 事件。非流式运行时为 null |
| 返回：`NodeOutput.writes` | 引擎通过 reducer 合并的通道写入 |
| 返回：`NodeOutput.command` | 可选的路由覆盖（`goto_node` + 状态更新） |
| 返回：`NodeOutput.sends` | 可选的动态扇出 — 引擎为每个 `Send` 生成一个分支 |
| `get_name()` | 返回图中节点的唯一名称 |

最简示例：

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

异步原生 LLM 调用：

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

> **迁移说明。** `GraphNode` 只有一个节点入口点：`run(NodeInput)`。
> 它保留 `Command` 和 `Send`，参与异步和流式执行，并且是子类必须实现的
> 重写。

### LLMCallNode

以当前对话状态调用 LLM。从 `"messages"` 通道读取，向 provider 发送补全
请求，并将 assistant 的响应写回。当运行通过 `run_stream` /
`run_stream_async` 启动时，流式发送 `LLM_TOKEN` 事件。

```cpp
class LLMCallNode : public GraphNode {
public:
    LLMCallNode(const std::string& name, const NodeContext& ctx);
    asio::awaitable<NodeOutput> run(NodeInput in) override;
    std::string get_name() const override;
};
```

| 构造函数参数 | 描述 |
|-----------------------|-------------|
| `name` | 节点名称 |
| `ctx` | 提供 LLM provider、工具、模型和指令的 NodeContext |

（LLMCallNode、`ToolDispatchNode`、`IntentClassifierNode` 和 `SubgraphNode`
都实现相同的 `run(NodeInput)` 契约。）

### ToolDispatchNode

从最新的 assistant 消息分发工具调用。从 `"messages"` 通道读取待处理的
工具调用，执行每个工具，并将工具结果消息写回。

```cpp
class ToolDispatchNode : public GraphNode {
public:
    ToolDispatchNode(const std::string& name, const NodeContext& ctx);

    asio::awaitable<NodeOutput> run(NodeInput in) override;
    std::string get_name() const override;
};
```

| 构造函数参数 | 描述 |
|-----------------------|-------------|
| `name` | 节点名称 |
| `ctx` | NodeContext（使用 `ctx.tools` 查找和执行工具） |

### IntentClassifierNode

使用 LLM 分类用户意图，然后将分类结果写入 `"__route__"` 通道。设计用于
与 `"route_channel"` 内置条件配合，以启用动态基于意图的路由。

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

| 构造函数参数 | 类型 | 描述 |
|-----------------------|------|-------------|
| `name` | `std::string` | 节点名称 |
| `ctx` | `NodeContext` | 用于分类 LLM 调用的 provider 和模型 |
| `prompt` | `std::string` | 分类提示模板 |
| `valid_routes` | `std::vector<std::string>` | 允许的分类值。LLM 输出对照这些验证 |

### SubgraphNode

将编译后的 `GraphEngine` 作为单个节点包装，实现分层图组合（supervisor
模式、嵌套工作流）。通道映射控制父子图之间的数据流。

```cpp
class SubgraphNode : public GraphNode {
public:
    SubgraphNode(const std::string& name,
                 std::shared_ptr<GraphEngine> subgraph,
                 std::map<std::string, std::string> input_map = {},
                 std::map<std::string, std::string> output_map = {});
    asio::awaitable<NodeOutput> run(NodeInput in) override;
    std::string get_name() const override;
};
```

| 构造函数参数 | 类型 | 描述 |
|-----------------------|------|-------------|
| `name` | `std::string` | 父图中的节点名称 |
| `subgraph` | `std::shared_ptr<GraphEngine>` | 编译后的子图引擎 |
| `input_map` | `std::map<std::string, std::string>` | `parent_channel -> child_channel` 映射。从父读取，写入子输入 |
| `output_map` | `std::map<std::string, std::string>` | `child_channel -> parent_channel` 映射。重命名子图生成的 write delta 并转发到父图 |

如果映射为空，通道按名称映射（恒等映射）。

输入映射会把父图当前的通道值复制到子图输入。输出映射有意采用不同语义：它不会把
子图最终序列化状态当作新的 reducer 输入，而是按生成顺序转发子图的
`ChannelWrite` delta，并保留每个 write 的 `Mode`。因此，继承的 append/custom
值不会被重复应用。输出映射不会推断 snapshot replacement；若要替换映射后的父值，
子图必须显式发出 `ChannelWrite::Mode::Overwrite`。

#### 运行时上下文传播

`SubgraphNode` 在引擎边界派生子执行上下文，不改变公开 `RunContext` 的布局。

| 上下文值 | 子图语义 |
|---------------|-----------------|
| `cancel_token` | 创建子操作 token，因此父取消会到达所有子图和孙图。 |
| `usage`, `deadline`, `trace_id`, `stream_mode` | 继承。`deadline` 和 `trace_id` 来自 `RunMetadata`；子图不能扩大父图的 stream mode。 |
| `thread_id` | 父 thread ID 非空时，根据父 ID、subgraph 节点名、super-step 和 invocation identity 确定性派生。因此 sibling `Send` 调用获得不同的 checkpoint identity。空父 thread ID 会让子图保持无作用域并禁用 checkpointing。 |
| `step` | 子执行本地值，从子 checkpoint 或 0 开始。 |
| `store` | 存在父 Store 时继承，否则保留子引擎配置的 Store。 |
| Tool policy | 父 `ToolGate` 先于子 gate 运行。子图可以进一步限制或 rewrite 已允许的调用，但不能绕过父 deny/interrupt。 |
| Checkpoint backend and resume value | 存在父 backend 时继承，否则保留子 backend。只有派生的子 checkpoint identity 存在时，父 resume 才会 resume 对应子 checkpoint，并转发非 null resume value。Checkpoint routing 是内部实现，不是公开 `RunContext` 字段。 |

---

## 7. GraphEngine

**头文件：** `<neograph/graph/engine.h>`
**命名空间：** `neograph::graph`

核心执行引擎。编译图定义，管理状态转换，并通过超级步骤循环编排节点执行。

<a id="engineconfig-and-engineresources"></a>
### EngineConfig 和 EngineResources

新代码应在创建引擎之前组装构造依赖和策略：

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

`ToolSet` 是用于固定工具集合的仅移动所有者。`GraphRegistry` 是每引擎的
reducer、condition 和 node-factory 覆盖层；覆盖层中不存在的名称回退到现有
的进程全局注册表。在将它们传递给 `build()` 或 `link()` 之前配置两者。
运行时变更故意不是本地注册表约定的一部分。

### RunConfig

单次图执行运行的配置。

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

| 字段 | 类型 | 默认值 | 描述 |
|-------|------|---------|-------------|
| `thread_id` | `std::string` | `""` | 标识对话/会话以用于检查点 |
| `input` | `json` | `{}` | 在执行开始前写入通道的初始值。通常是 `{"messages": [...]}` |
| `max_steps` | `int` | `50` | 强制终止前的最大超级步骤数（防止无限循环） |
| `stream_mode` | `StreamMode` | `ALL` | 控制在流式执行期间发出哪些事件类型的位标记 |
| `cancel_token` | `std::shared_ptr<CancelToken>` | `nullptr` | 协作式取消句柄。引擎将其包装到 `RunContext` 中，并通过 `in.ctx.cancel_token` 传递给每个节点的 `run(NodeInput)` 调用 |
| `usage` | `std::shared_ptr<UsageAccumulator>` | `nullptr` | 可选的 token 累加器。当省略时引擎创建一个，并将活跃累加器暴露为 `in.ctx.usage` |
| `resume_if_exists` | `bool` | `false` | 如果为 `true` 且 `thread_id` 存在检查点，在应用 `input` 之前从该检查点播种（多轮聊天形态） |

### RunContext（v0.4 PR 1，通过 `NodeInput.ctx` 暴露给节点）

引擎传递的每次运行分发元数据。由 `RunConfig`（未提供时创建新的 usage
累加器）、`RunMetadata`、有效 Store 和可选 resume value 构造。节点在
`run(NodeInput) -> NodeOutput` 重写中通过 `in.ctx` 使用它。

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

| 字段 | 描述 |
|-------|-------------|
| `cancel_token` | 活跃 token。传递给 `ProviderRequest::cancel_token`，使 LLM HTTP 套接字在取消时中止，或轮询 `is_cancelled()` 用于自己的循环 |
| `usage` | 引擎填充的共享 token-记账接收器 |
| `deadline` | 来自 C++ `RunMetadata` 的可选绝对 deadline |
| `trace_id` | 来自 C++ `RunMetadata` 的可选 trace correlator |
| `thread_id` | `RunConfig.thread_id` 的镜像 |
| `step` | 当前超级步骤索引，每次迭代更新 |
| `stream_mode` | `RunConfig.stream_mode` 的镜像 |
| `resume_value` | 提供给 `GraphEngine::resume()` 的值，或在新运行时为空 |
| `store` | 安装在引擎上的 Store，或在未配置时为 `nullptr` |
| `tool_gate` | 此 invocation 的有效策略，包括继承的父策略 |

### CancelToken

调用者与引擎之间共享的协作式取消原语。通过
`std::make_shared<CancelToken>()`、传给 `RunConfig.cancel_token`，并从任何
线程调用 `cancel()` 来中止正在运行的运行 — 如果节点正在
`provider.invoke_async` 中间执行，则包括 LLM HTTP 套接字。每次引擎运行
分叉自己的操作子令牌，因此一个父令牌可以安全地取消多个并发运行，而无需
共享 asio 取消槽。

引擎操作子令牌保留自身直到其投递的取消发出执行完成。如果应用程序代码在
自己构造的令牌上直接调用 `bind_executor()`，应用程序必须保持该令牌存活
直到执行器排空；引擎无法为外部对象提供所有权。由于这些方法是公共头文件
中的内联函数，现有 C++ 消费者必须重新编译以接收更新的 `fork()` 生命周期
行为。`CancelToken` 对象布局与 0.11.x 保持二进制兼容。

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

#### 分层取消（v0.4 `fork()`）

每个子令牌有自己的 `cancellation_signal`；父令牌的 `cancel()` 级联到每个
活动子令牌。这是 v0.3.x `add_cancel_hook` 列表（已弃用，在 v1.0 中移除）
的结构性替代品。并发嵌套作用域 — 多 Send 扇出，其中每个工作器同时调用
`provider.invoke(std::move(request))` — 各自调用一次 `fork()`，互不覆盖对方的槽。

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

| 方法 | 描述 |
|--------|-------------|
| `cancel()` | 幂等、线程安全。设置轮询标志，并在绑定执行器上发出 asio cancellation_signal；通过 `fork()` 级联到所有活动子令牌 |
| `is_cancelled()` | 无锁轮询读取 |
| `fork()` | **v0.4 PR 3。** 返回子 shared_ptr。Parent.cancel() 级联；如果父令牌在 fork() 时已取消，子令牌被构造为预取消状态（无 emit-vs-bind 竞态） |
| `bind_executor(ex)` | 引擎内部；绑定处理信号发出的执行器 |
| `slot()` | asio `cancellation_slot`，用于在 `co_spawn` 时 `bind_cancellation_slot` |

### RunResult

图执行完成或中断后返回的结果。

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

`RunResult::usage` 是 nullable provider 报告，不是支出 bank。`native_messages` 保留真实 typed 历史，`provider_outcomes` 保留每个拥有所有权的 Completion/Failure。JSON `output` 仅是 portable projection。完整历史输入用 `RunConfig::provider_messages`，typed event 观察用 `on_provider_event`。持久 native checkpoint/receipt custody 必须使用 `native_history_archive`，内存 sidecar 无需 archive。
| 字段 | 类型 | 描述 |
|-------|------|-------------|
| `output` | `json` | 所有通道的序列化最终状态 |
| `interrupted` | `bool` | 如果执行被中断暂停则为 `true`（HITL） |
| `interrupt_node` | `std::string` | 触发中断的节点名称 |
| `interrupt_value` | `json` | 中断的原因或 payload |
| `checkpoint_id` | `std::string` | 最后保存的检查点的 UUID |
| `execution_trace` | `std::vector<std::string>` | 按执行顺序排列的节点名称有序列表 |

`max_steps_exhausted()` 仅在步骤上限停止运行且仍有可运行工作时返回
`true`。一个恰好在最后一步允许时到达 `__end__` 的图返回 `false`。

`status()` 返回 `RunStatus::Completed`、`RunStatus::Interrupted` 或
`RunStatus::StepLimit`，而不更改公开 `RunResult` 的数据布局。
`ChannelKey<T>` 将可复用的通道名称绑定到其预期的 C++ 类型：

```cpp
inline const ChannelKey<std::string> Answer{"answer"};

auto answer = result.channel(Answer);
if (auto optional = result.try_channel(Answer)) {
    std::cout << *optional << '\n';
}
```

### GraphEngine

主引擎类。新代码应使用 `build_strict()` 处理 JSON 定义；它在任何节点
实例化之前拒绝无效拓扑。使用 `link()` 与 `ValidatedTopology` 用于必须
拆分解析、验证、检查或转换的步骤。宽松的 `build()`、`CompiledGraph` 链接
重载、`compile()` 和构造后 setter 仍然是兼容路径。

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

    // 在编译前通过 NodeContext 或 EngineResources 绑定工具。
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

#### `build` 和 `link`

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

`build()` 编译、验证、链接并返回完全配置的引擎。`link()` 通过移动消费
`CompiledGraph` 并应用运行时配置；手动编译的调用者对其所需的任何源到 IR
往返验证负责。

#### `compile`（兼容性）

```cpp
static std::unique_ptr<GraphEngine> compile(
    const json& definition,
    const NodeContext& default_context,
    std::shared_ptr<CheckpointStore> store = nullptr);
```

从 JSON 定义编译图并返回一个准备执行的引擎。此原始签名被保留并委托给
`build()`。当新代码需要存储、重试策略、工作器配置、缓存或工具门时，
优先使用 `EngineConfig`。

| 参数 | 类型 | 描述 |
|-----------|------|-------------|
| `definition` | `const json&` | JSON 格式的图定义（见下文） |
| `default_context` | `const NodeContext&` | 注入所有节点的默认上下文 |
| `store` | `std::shared_ptr<CheckpointStore>` | 可选的持久化检查点存储 |

**图定义 JSON Schema：**

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

##### 屏障节点（AND-join 可选加入）

节点声明可以包含一个 `barrier` 字段，以选择对该特定节点启用 AND-join
语义。在默认的信号分发模型下，只要有任何上游在给定超级步骤中路由到该
节点，该节点就会触发——这在非对称串行扇入（不同长度的路径）上会导致
join 节点双重触发。屏障在该节点被**所有**列出的上游至少发出过一次信号
（跨任意数量的超级步骤）之前将其守门：

```json
"join": {
  "type": "my_join",
  "barrier": {"wait_for": ["a", "s2"]}
}
```

当 `a` 和 `s2` 都发出信号时触发一次。触发后状态重置，因此通过屏障的循环
每轮收集新的信号。

**持久性：** 自 `CHECKPOINT_SCHEMA_VERSION = 2` 起，屏障累加器在每次
检查点（`Checkpoint::barrier_state`，一个 `map<string, set<string>>`）上
持久化，并在恢复时恢复。因此，在累积过程中着陆的中断是安全的——部分上游
集合在暂停后存续，一旦剩余信号到达屏障就会触发。v1 blob 以空
`barrier_state` 反序列化，匹配这些存储检查点的 pre-v2 行为。

#### `run`

```cpp
RunResult run(const RunConfig& config);
```

同步（阻塞）执行图。从 `START_NODE` 开始，跟随边直到到达 `END_NODE` 或
超过 `max_steps`。

#### `run_stream`

```cpp
RunResult run_stream(const RunConfig& config,
                     const GraphStreamCallback& cb);
```

以流式事件执行图。对每个匹配 `config.stream_mode` 过滤器的事件调用回调
`cb`。

#### `resume`

```cpp
RunResult resume(const std::string& thread_id,
                 const json& resume_value = json(),
                 const GraphStreamCallback& cb = nullptr);
```

从先前中断的检查点恢复执行（人类参与）。

| 参数 | 类型 | 描述 |
|-----------|------|-------------|
| `thread_id` | `std::string` | 要恢复的线程 ID |
| `resume_value` | `json` | 在恢复前要注入的可选值（例如，人类批准） |
| `cb` | `GraphStreamCallback` | 可选的流式回调。对于非流式恢复传递 `nullptr` |

#### `get_state`

```cpp
std::optional<json> get_state(const std::string& thread_id) const;
```

返回线程的最新状态，或如果不存在检查点则为 `std::nullopt`。

#### `get_state_history`

```cpp
std::vector<Checkpoint> get_state_history(const std::string& thread_id,
                                          int limit = 100) const;
```

返回线程的检查点历史，按时间戳排序（最新优先）。

#### `update_state`

```cpp
void update_state(const std::string& thread_id,
                  const json& channel_writes,
                  const std::string& as_node = "");

void update_state_writes(const std::string& thread_id,
                         const std::vector<ChannelWrite>& channel_writes,
                         const std::string& as_node = "");
```

通过应用通道 write 手动更新线程状态。JSON object 形式按通道名应用 reducer write。
`ChannelWrite` vector 形式保留 write 顺序和显式 overwrite mode。两种形式都会使用
更新后的状态创建新 checkpoint。

| 参数 | 类型 | 描述 |
|-----------|------|-------------|
| `thread_id` | `std::string` | 目标线程 |
| `channel_writes` | `json` | 要应用的 `{channel: value}` 对对象 |
| `as_node` | `std::string` | 可选：将这些写入记录为好像来自特定节点 |

#### `fork`

```cpp
std::string fork(const std::string& source_thread_id,
                 const std::string& new_thread_id,
                 const std::string& checkpoint_id = "");
```

将线程状态的副本创建为新线程。适用于分支对话或创建假设场景。

| 参数 | 类型 | 描述 |
|-----------|------|-------------|
| `source_thread_id` | `std::string` | 要从中复制的线程 |
| `new_thread_id` | `std::string` | 新线程标识符 |
| `checkpoint_id` | `std::string` | 可选：从特定检查点分叉（默认：最新） |

**返回：** 新分叉状态的检查点 ID。

工具在编译前由 `NodeContext::tools` 或 `EngineResources::tools` 持有；
不再存在编译之后的所有权转移。

#### `set_checkpoint_store`

```cpp
void set_checkpoint_store(std::shared_ptr<CheckpointStore> store);
```

附加检查点存储。`resume()`、`get_state()`、`fork()` 和所有状态检查方法
都需要它。

#### `set_store`

```cpp
void set_store(std::shared_ptr<Store> store);
```

附加跨线程共享内存存储（见 [Store](#9-store)）。

#### `get_store`

```cpp
std::shared_ptr<Store> get_store() const;
```

返回附加的共享内存存储，如果未设置则为 `nullptr`。

#### `set_retry_policy`

```cpp
void set_retry_policy(const RetryPolicy& policy);
```

为所有节点设置默认重试策略。没有特定策略的节点将使用此策略。

#### `set_node_retry_policy`

```cpp
void set_node_retry_policy(const std::string& node_name, const RetryPolicy& policy);
```

为特定节点设置重试策略，覆盖默认值。

#### `get_graph_name`

```cpp
const std::string& get_graph_name() const;
```

返回定义中指定的图名称。

---

<a id="7b-engine-internals"></a>
## 7b. 引擎内部

`GraphEngine` 是一个轻量级的编排器，委托给四个专门构建的类。用户通常
永远不直接接触它们 — 它们在 `GraphEngine::build()`（或其 `compile()` 兼容
门面）内部实例化，并由 `execute_graph()` 驱动 — 但它们是公开的，以便高级
调用者可以在没有 JSON 的情况下构建、驱动自定义检查点流或在测试中对部分
进行存根。

| 类 | 头文件 | 职责 |
|-------|--------|----------------|
| [`GraphCompiler`](#graphcompiler) | `<neograph/graph/compiler.h>` | 解析 JSON → `CompiledGraph` |
| [`Scheduler`](#scheduler) | `<neograph/graph/scheduler.h>` | 路由决策（信号分发 + 屏障） |
| [`CheckpointCoordinator`](#checkpointcoordinator) | `<neograph/graph/coordinator.h>` | 每运行的检查点生命周期 |
| [`NodeExecutor`](#nodeexecutor) | `<neograph/graph/executor.h>` | 重试、并行扇出、Send 分发 |

### GraphCompiler

**头文件：** `<neograph/graph/compiler.h>`

纯 JSON → 值类型转换。无运行时依赖 — 生成的 `CompiledGraph` 是一个可移动
的包，你可以在测试中检查或手动构造。

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

`GraphCompiler::parse()` 产生一个 `TopologySpec` 而不构造节点。
`GraphValidator::validate()` 返回结构化诊断，而
`GraphValidator::require_valid()` 返回 `ValidatedTopology` 或抛出
`std::runtime_error`。只有 `GraphCompiler::link()` 解析工厂并实例化运行时
节点。`compile()` 仍然是解析和链接的兼容组合，且 `GraphEngine::build()`
保留其宽松警告行为。新代码可以使用 `GraphEngine::build_strict()` 强制
完整边界，或：

```cpp
auto spec = GraphCompiler::parse(definition);
auto validated = GraphValidator::require_valid(std::move(spec));
auto engine = GraphEngine::link(std::move(validated), config, resources);
```

### Scheduler

**头文件：** `<neograph/graph/scheduler.h>`

拥有图拓扑，并从前一步发出的路由信号计算每个超级步骤的就绪集。不了解
线程、检查点、重试或 HITL — 这些留在引擎中。

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

**语义：**

- **信号分发**：一个节点在超级步骤 S+1 中就绪，当且仅当步骤 S 中的某个
  节点显式路由到它（常规边、条件边分支、`Command::goto_node` 或 Send）。
  没有静态前驱映射 — 那会将 XOR 路由与 AND 扇入混淆。
- **配对不变式**：调用者必须传递 `just_ran` 和 `results`，其中
  `just_ran[i] ↔ results[i]`。由双参数重载的类型签名强制执行，因此调用者
  不能使其不同步。
- **屏障**：声明了 `"barrier": {"wait_for": [...]}` 的节点在**所有**列出的
  上游都发出过信号后守门，跨超级步骤通过可变 `BarrierState` 映射累积。
  触发重置条目，因此通过屏障的循环正确工作。

### CheckpointCoordinator

**头文件：** `<neograph/graph/coordinator.h>`

在 `(CheckpointStore, thread_id)` 上的每运行包装器。当存储为 null 或
thread_id 为空时，每个方法都是安全的无操作，因此调用点永远不需要守卫。

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

**Phase-aware 步骤偏移：** `load_for_resume()` 读取最新检查点的
`interrupt_phase` 并相应地设置 `start_step` — `Before` /
`NodeInterrupt` 在 `cp.step` 重新进入，`After` / `Completed` /
`Updated` 推进 +1。引擎的恢复路径从不重复此逻辑。

### NodeExecutor

**头文件：** `<neograph/graph/executor.h>`

拥有每超级步骤的节点调用：重试循环、重放查找、pending-write 记录、通过
`asio::experimental::make_parallel_group` 的并行扇出和 Send 分发。3.0
移除了同步 `run_one` / `run_parallel` / `run_sends` 孪生；调用者使用
`_async` 对应版本。

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

**不变式：**

- `run_one_async` 和 `run_parallel_async` 在重新抛出 `NodeInterrupt` 之前
  都保存一个限定于中断节点的 `phase=NodeInterrupt` 检查点，因此恢复恰好
  重新进入该节点（兄弟写入已在 `pending_writes` 中，并通过映射重放）。
- `run_parallel_async` 以 `ready` 顺序应用写入 + `Command.updates`，因此
  `ready[i] ↔ results[i]` 配对对后续 Scheduler 调用成立。
- `run_sends_async`：单个 Send 在共享状态上运行并重试；多 Send 为每个目标
  提供隔离的状态副本（init + restore + apply input）不重试 — 保留 3.0 前
  语义。
- `fan_out_pool`（可选）确定并行分支分发的目标。为 null 时，分支在
  `co_await asio::this_coro::executor` 上运行 — 对单线程异步调用者没问题，
  但 CPU 绑定的扇出串行执行。非 null 时，`run_parallel_async` 和多 Send
  分支 `co_spawn` 到 `pool->get_executor()` 上以实现真正的线程并行。
  `GraphEngine::set_worker_count(N)` 为同步 `run()` 调用者安装池。
- `execute_node_with_retry_async` 是内部重试循环：退避使用
  `asio::steady_timer`，因此在重试等待期间执行器不被冻结。

---

<a id="8-checkpoint"></a>
## 8. 检查点

**头文件：** `<neograph/graph/checkpoint.h>`
**命名空间：** `neograph::graph`

检查点通过保存和恢复图执行状态来实现持久性、时间旅行调试和人类参与
工作流。

<a id="checkpoint-struct"></a>
### Checkpoint（结构体）

图执行状态在某个时间点的序列化快照。

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

| 字段 | 类型 | 描述 |
|-------|------|-------------|
| `id` | `std::string` | 唯一标识符（UUID v4） |
| `thread_id` | `std::string` | 按对话/会话分组检查点 |
| `channel_values` | `json` | 所有通道的序列化状态 |
| `channel_versions` | `json` | 每个通道的版本计数 |
| `parent_id` | `std::string` | 前一个检查点的 ID（形成时间旅行的链表） |
| `current_node` | `std::string` | 检查点被拍摄时正在执行的节点 |
| `next_nodes` | `std::vector<std::string>` | 为下一个超级步骤调度的所有节点（由 `resume()` 使用）。在信号分发下，一个超级步骤可能同时使几个节点就绪（并行扇出、条件分支同时激活），必须持久化每个节点 — 存储单个节点会在崩溃时静默丢弃兄弟节点 |
| `interrupt_phase` | `CheckpointPhase` | 枚举：`Before`（interrupt_before 触发）、`After`（interrupt_after 触发）、`Completed`（正常超级步骤节奏）、`NodeInterrupt`（节点执行中途抛出 `NodeInterrupt`）、`Updated`（外部 `update_state()` 注入）。`to_string()` 和 `parse_checkpoint_phase()` 提供稳定的线路/日志编码 |
| `barrier_state` | `map<string, set<string>>` | 每屏障累加器，记录了到目前为止已发出信号的上游。条目仅为飞行中的屏障存在（尚未触发）— Scheduler 在屏障触发时清除条目。形态匹配 `scheduler.h` 中的 `BarrierState`。自 schema v2 起存在；v1 blob 以空映射反序列化，匹配其 pre-v2 行为 |
| `metadata` | `json` | 任意的用户定义数据 |
| `step` | `int64_t` | 超级步骤计数 |
| `timestamp` | `int64_t` | 创建时间，Unix 纪元毫秒 |
| `schema_version` | `std::uint32_t` | 线路布局版本（见 `CHECKPOINT_SCHEMA_VERSION`，当前为 `3`）。第五轮将其从 `int` 扩大到定宽无符号 — schema version 是非负的，平台可变的 `int` 宽度对于持久化到磁盘并通过 JSON 往返的值来说是不正确的。持久的 `CheckpointStore` 实现应序列化它，并将反序列化 blob 上的 `0` 视为"pre-versioned"（例如字段缺失 — 迁移是调用者的责任） |

### CheckpointStore

检查点持久化的抽象接口。实现此接口以在数据库、文件系统或任何其他后端
中存储检查点。

> **正在编写自定义存储？** 新实现应实现最小的适用能力：
> `CheckpointStoreCore`，可选地 `AsyncCheckpointStore` 和/或
> `PendingWritesCheckpointStore`，然后通过 `adapt_checkpoint_store()` 传递
> 它。现有 `CheckpointStore` 接口仍然是兼容性约定。其异步默认实现调用同步
> 方法；因此仅同步后端仍然有效，但其异步调用是阻塞的。参见
> [`ASYNC_GUIDE.md` §9.4](ASYNC_GUIDE.md#94-checkpointstore)。

```cpp
class CheckpointStore {
public:
    virtual ~CheckpointStore() = default;

    // ── Sync core (5 virtuals, non-pure with bridge defaults) ──────
    virtual void save(const Checkpoint& cp);
    virtual std::optional<Checkpoint> load_latest(const std::string& thread_id);
    virtual std::optional<Checkpoint> load_by_id(const std::string& id);
    virtual std::vector<Checkpoint>   list(const std::string& thread_id,
                                           int limit = 100);
    virtual void delete_thread(const std::string& thread_id);

    // ── Async peers (5 virtuals, default co_return the sync call) ──
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

| 方法 | 描述 |
|--------|-------------|
| `save(cp)` / `save_async(cp)` | 持久化一个检查点。引擎每超级步骤写入一个。 |
| `load_latest(thread_id)` / `_async` | 加载线程的最新检查点。 |
| `load_by_id(id)` / `_async` | 按 UUID 加载特定检查点（时间旅行）。 |
| `list(thread_id, limit)` / `_async` | 列出某线程的检查点，最新优先，最多 `limit`。 |
| `delete_thread(thread_id)` / `_async` | 删除某线程的所有检查点。 |
| `put_writes(thread_id, parent_cp, write)` / `_async` | 在超级步骤中途记录成功的节点执行。引擎在节点返回后且其写入应用到 GraphState **之前**立即调用。默认无操作。 |
| `get_writes(thread_id, parent_cp)` / `_async` | 加载附加到父检查点的待处理写入。引擎在恢复时调用此方法以跳过已完成的任务。默认空。 |
| `clear_writes(thread_id, parent_cp)` / `_async` | 在后继超级步骤的检查点被持久保存后丢弃待处理写入。默认无操作。 |

### InMemoryCheckpointStore

适合测试和单进程应用的线程安全内存实现。

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

**头文件：** `<neograph/graph/store.h>`
**命名空间：** `neograph::graph`

跨线程共享内存存储。提供跨线程和图执行持久化的命名空间键值存储。
用例包括长期用户偏好、共享知识库和 agent 记忆。

### Namespace

表示为字符串向量的分层路径。

```cpp
using Namespace = std::vector<std::string>;
```

示例：`{"users", "user123", "preferences"}` 表示路径 `users/user123/preferences`。

### StoreItem

存储中的单个项。

```cpp
struct StoreItem {
    Namespace   ns;          // Namespace path
    std::string key;         // Item key within the namespace
    json        value;       // Stored value
    int64_t     created_at;  // Creation timestamp (Unix epoch millis)
    int64_t     updated_at;  // Last update timestamp (Unix epoch millis)
};
```

<a id="store-abstract"></a>
### Store（抽象）

跨线程共享内存的抽象接口。

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

| 方法 | 描述 |
|--------|-------------|
| `put(ns, key, value)` | 插入或更新值。如果项已存在，更新 `updated_at` |
| `get(ns, key)` | 检索单个项。如果未找到，返回 `std::nullopt` |
| `search(ns_prefix, limit)` | 查找所有命名空间以给定前缀开头的项 |
| `delete_item(ns, key)` | 从存储中移除项 |
| `list_namespaces(prefix)` | 列出所有以给定前缀开头的唯一命名空间 |

### InMemoryStore

适用于测试和单进程使用的线程安全内存实现。

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

**头文件：** `<neograph/graph/loader.h>`
**命名空间：** `neograph::graph`

用于 reducer、条件和节点类型的旧单例注册表。这些仍然是由 JSON 驱动的图
构造的进程全局回退。新代码可以通过 `EngineResources` 传递 `GraphRegistry`；
其局部条目优先，而缺失的名称继续在此处解析。

### ReducerRegistry

将字符串名称映射到 `ReducerFn` 实现的单例注册表。

```cpp
class ReducerRegistry {
public:
    static ReducerRegistry& instance();

    void register_reducer(const std::string& name, ReducerFn fn);
    ReducerFn get(const std::string& name) const;
    std::vector<std::string> names() const;
};
```

| 方法 | 描述 |
|--------|-------------|
| `instance()` | 返回单例实例 |
| `register_reducer(name, fn)` | 注册自定义 reducer 函数 |
| `get(name)` | 按名称查找 reducer。如未找到则抛出 |
| `names()` | 所有已注册 reducer 名称的排序列表（外部工具的自省） |

### ConditionRegistry

将字符串名称映射到 `ConditionFn` 实现的单例注册表。

```cpp
class ConditionRegistry {
public:
    static ConditionRegistry& instance();

    void register_condition(const std::string& name, ConditionFn fn);
    ConditionFn get(const std::string& name) const;
    std::vector<std::string> names() const;
};
```

| 方法 | 描述 |
|--------|-------------|
| `instance()` | 返回单例实例 |
| `register_condition(name, fn)` | 注册自定义条件函数 |
| `get(name)` | 按名称查找条件。如未找到则抛出 |
| `names()` | 所有已注册条件名称的排序列表（外部工具的自省） |

### NodeFactory

从 JSON 配置创建 `GraphNode` 实例的单例工厂。

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

| 方法 | 描述 |
|--------|-------------|
| `instance()` | 返回单例实例 |
| `register_type(type, fn)` | 注册节点工厂。Config schema 默认为宽松的 `{"type":"object"}` |
| `register_type(type, fn, config_schema)` | 同上，带有为节点 `config` 声明的 JSON Schema（Draft 2020-12）。纯新增 — 2 参数重载不变地工作。仅由 `export_schema()` 使用；引擎不根据它验证 config |
| `create(type, name, config, ctx)` | 创建给定类型的节点。如果类型未注册则抛出 |
| `registered_types()` | 所有已注册节点类型名称的排序列表 |
| `export_schema()` | 此引擎版本接受的拓扑 JSON 的机器可读描述（见 [拓扑 Schema 导出](#topology-schema-export-issue-56)） |

<a id="built-in-registrations"></a>
### 内置注册

库预注册了以下组件：

**Reducer：**

| 名称 | 行为 |
|------|----------|
| `"overwrite"` | 用传入的值替换当前值 |
| `"append"` | 将传入的值追加到当前数组。如果传入的值是数组，其元素被连接 |

**条件：**

| 名称 | 行为 |
|------|----------|
| `"has_tool_calls"` | 检查 `"messages"` 通道中的最后一条消息。如果包含工具调用则返回 `"yes"`，否则返回 `"no"` |
| `"route_channel"` | 读取 `"__route__"` 通道并返回其字符串值。与 `IntentClassifierNode` 配合使用 |

**节点类型：**

| 类型 | 类 | 描述 |
|------|-------|-------------|
| `"llm_call"` | `LLMCallNode` | 以当前对话状态调用 LLM |
| `"tool_dispatch"` | `ToolDispatchNode` | 从最新的 assistant 消息分发工具调用 |
| `"intent_classifier"` | `IntentClassifierNode` | 基于 LLM 的意图分类。从 `config` 读取 `prompt` 和 `valid_routes` |
| `"subgraph"` | `SubgraphNode` | 运行已编译的子图。从 `config` 读取 `input_map` 和 `output_map` |

<a id="topology-schema-export-issue-56"></a>
### 拓扑 Schema 导出（issue #56）

NeoGraph 运行一个*用 JSON 描述*的图；换一个 JSON 同一个引擎就变成不同
的 harness。`NodeFactory::export_schema()` 发出此引擎版本接受的拓扑 JSON
的机器可读描述，使外部工具 — 特别是无代码可视化块编辑器（NeoGraph Studio，
一个私有的伴侣仓库，issue #56）— 可以从引擎生成其调色板，并且永不漂移
出同步状态。

**三种访问路径，一份文档：**

| 从 | 方式 |
|------|-----|
| C++ | `neograph::graph::NodeFactory::instance().export_schema()` → `json` |
| CLI | `./example_export_schema > schema.json`（`examples/52_export_schema.cpp`） |
| Python | `neograph_engine.export_schema()` → `dict` |

**文档形态：**

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

- **`neograph_version`** 在编译时从 `pyproject.toml`（单一真源）打戳。
  工具将其与缓存 schema 比较并在其调色板比引擎旧时发出警告。
- **`node_types`** 反映调用时刻 `NodeFactory` 中注册的任何内容，因此嵌入
  者的自定义节点类型也会出现 — 在导出之前注册它们（以及任何自定义
  reducer/condition），正如你在 `compile()` 之前所做的那样。通过 3 参数
  `register_type` 注册的类型携带其声明的 config schema；2 参数形式产生
  宽松的 `{"type":"object"}`。
- **往返约定。** 发出拓扑 JSON 的工具应将其通过 loader 往返并断言结构
  被保留。特别是顶级 `conditional_edges` 块在 v0.1.0–v0.1.7 中被编译器
  静默丢弃（在 v0.1.8 中修复）；引擎测试套件（`tests/test_schema_export.cpp`）
  守卫此回归，工具也应如此。

```cpp
#include <neograph/graph/loader.h>
// register custom node types first if you want them in the palette …
auto schema = neograph::graph::NodeFactory::instance().export_schema();
std::cout << schema.dump(2) << "\n";
```

---

## 10.5. 可观测性 — OpenTelemetry + OpenInference

> 下方 Python provider/wrapper 示例仅为历史资料，尚未移植到 typed C++ 契约，不是当前 provider 指南。C++ 变更不实现或验证 Python binding。C++ 观察者仅导出既有公开文本/scalar/nullable count，不导出 raw native 状态。
**模块：** `neograph_engine.tracing`（OTel 形态）+
`neograph_engine.openinference`（LLM 形态）
**始于：** OTel 层在 v0.3.x；OpenInference 层在 **v0.6.0**。

NeoGraph 通过流式 API 使用的相同回调发出其 `GraphEvent` 流。两个辅助类
位于其上：

  - **`otel_tracer(tracer)`** — 供应商中立的 OpenTelemetry span。每次运行
    的根 span + 每节点的子 span + status / error / interrupt 映射。Span
    流向任何 OTel 后端（Jaeger、Tempo、Honeycomb、Datadog、…）。在你已经
    运行只需要 span 形态数据的 APM 时很有用。
  - **`openinference_tracer(tracer)` + `OpenInferenceProvider`** —
    位于其上的 LLM 形态属性层。相同的 OTel 机制，但每个 span 携带
    `openinference.span.kind`（`"CHAIN"` / `"LLM"`）加上 LLM 特定键
    （`llm.model_name`、`llm.input_messages.{i}.…`、
    `llm.token_count.{prompt,completion,total}` 等），因此识别
    OpenInference 约定的后端 — Phoenix、Arize、Langfuse — 将追踪渲染为
    聊天气泡 + DAG 层次结构 + 每次调用 token 成本 UI（"LangSmith UX"）。

### `otel_tracer` — OTel 形态 span

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

| 配置项 | 默认值 | 用途 |
|---|---|---|
| `root_name` | `"graph.run"` | 每运行根 span 的 span 名称 |
| `node_span_prefix` | `"node."` | 与每个节点名称连接的前缀 |
| `attribute_prefix` | `"neograph"` | 引擎特定属性的前缀（`neograph.node`、`neograph.next_nodes` 等） |
| `on_event` | `None` | 可选辅助回调，接收每个原始 `GraphEvent` — 用于与日志/指标链式连接 |

处理的事件：`NODE_START` 打开子 span，`NODE_END` 关闭它（带有
`Status.OK`），`ERROR` 记录异常并以 `Status.ERROR` 结束 span，
`INTERRUPT` 标记 `{attribute_prefix}.interrupted = true` 并结束。

并发扇出（多 Send）：每个节点名称维护一个开放 span 的栈；`NODE_END`
弹出最近的。始终在退出时结束：上下文管理器的 `finally` 块强制关闭运行
引发时仍打开的任何 span。

```python
from opentelemetry import trace
from neograph_engine.tracing import otel_tracer

tracer = trace.get_tracer("my-service")
with otel_tracer(tracer) as cb:
    engine.run_stream(cfg, cb)
```

### `openinference_tracer` — 添加 LLM 形态属性

相同形态，加上每个 span 标记 `openinference.span.kind = "CHAIN"` 且节点
payload 编码为 `input.value` / `output.value` JSON blob。Phoenix /
Arize / Langfuse 在其 UI 中将追踪视为 LLM 链。

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

追踪器还将每个节点 span 附加为 OTel *current context*（通过
`otel_context.attach`），因此节点体内的 `Provider.complete()` 调用将其
`llm.complete` span 打开为该节点的子节点 — 追踪是单个连接的树，而不是
3+ 个孤立的 trace-ID（v0.6.0 contextvar 传播修复）。

### `OpenInferenceProvider` — 包装任何 `Provider`

> **历史 Python-only 示例。** 下方 Python Provider/OpenInference wrapper 未在当前 C++ typed cutover 中 port/qualification，不是新 `ProviderRequest`/owned-outcome 契约的兼容 bridge。

```python
class OpenInferenceProvider(Provider):
    def __init__(self, inner: Provider, tracer: Any,
                 *, span_name: str = "llm.complete"):
        ...
```

在每次 `complete(params)` 调用时，它在当前 OTel 上下文下打开一个 LLM 种类
的子 span（因此它嵌套在活跃的节点 span 下），捕获 OpenInference 属性，
委托给 `inner.complete()`，然后关闭 span。追踪失败被吞下 — 可观测性永远
不破坏 LLM 调用。内部 provider 异常在 span 被标记 ERROR 后重新抛出。

每个 LLM span 捕获的属性：

| 属性 | 来源 |
|---|---|
| `openinference.span.kind` | 常量 `"LLM"` |
| `llm.model_name` | `params.model` |
| `llm.invocation_parameters` | `temperature`、`max_tokens`、`top_p`、`frequency_penalty`、`presence_penalty` 的 JSON blob（设置时） |
| `llm.input_messages.{i}.message.role` | `params.messages[i].role` |
| `llm.input_messages.{i}.message.content` | `params.messages[i].content` |
| `input.value` / `input.mime_type` | `params.messages` JSON / `application/json`（Langfuse 兼容 blob） |
| `llm.output_messages.0.message.role` | `result.message.role` |
| `llm.output_messages.0.message.content` | `result.message.content` |
| `output.value` / `output.mime_type` | `result.message.content` / `text/plain` |
| `llm.token_count.prompt` | `result.usage.prompt_tokens` |
| `llm.token_count.completion` | `result.usage.completion_tokens` |
| `llm.token_count.total` | `result.usage.total_tokens` |

### 端到端：NeoGraph + Phoenix 在一个代码块中

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

端点 URL 是将其指向 Langfuse 自托管而非 Phoenix 时唯一需要更改的内容 —
两者都遵循 OpenInference 和 OTLP。

### 备注

- **可选加入依赖。** `opentelemetry-api` 不被基础 wheel 拉入。仅当包缺失时，
  导入 `neograph_engine.tracing` / `.openinference` 在首次使用时引发
  `ImportError` — 通过
  `pip install opentelemetry-api opentelemetry-sdk opentelemetry-exporter-otlp`
  安装。
- **OTel contextvars 跨 pybind。** v0.3.x 中的 `otel_tracer` 文档记录了
  不带 `__exit__()` 的 `trace.use_span(...).__enter__()` 会泄漏
  contextvar 且不会可靠地通过 C++ → Python 回调边界传播。两个追踪器现在
  都使用显式 `otel_context.attach` + `detach` token 对，以确定性地控制
  当前 span 的激活。
- **`otel_tracer` 与 `openinference_tracer`。** 当你的后端是 APM 形态
  （Jaeger、Datadog）且你想要通用 span 时，使用 OTel 版本。当你的后端是
  Phoenix / Langfuse / Arize 且你想要 LLM 形态渲染时，使用 OpenInference
  版本。两者不能在同一运行中组合 — 它们是引擎事件流的替代回调。

---

## 11. React Graph

**头文件：** `<neograph/graph/react_graph.h>`
**命名空间：** `neograph::graph`

便利函数，创建标准 ReAct（Reason + Act）agent 作为双节点图：
`llm_call -> tool_dispatch ->（如有工具调用则循环返回，否则结束）`。

```cpp
std::unique_ptr<GraphEngine> create_react_graph(
    std::shared_ptr<Provider> provider,
    std::vector<std::unique_ptr<Tool>> tools,
    const std::string& instructions = "",
    const std::string& model = "");
```

| 参数 | 类型 | 描述 |
|-----------|------|-------------|
| `provider` | `std::shared_ptr<Provider>` | LLM provider |
| `tools` | `std::vector<std::unique_ptr<Tool>>` | agent 可用的工具（所有权转移） |
| `instructions` | `std::string` | 系统提示 / 指令 |
| `model` | `std::string` | 模型覆盖（空表示使用 provider 默认值） |

**返回：** 一个准备好运行的编译后的 `GraphEngine`。

这在功能上等价于使用 `Agent::run()`，但作为图引擎，可让你访问检查点、
流式事件、状态检查以及所有其他图引擎特性。

---

## 11b. Plan-and-Execute 图

**头文件：** `<neograph/graph/plan_execute_graph.h>`
**命名空间：** `neograph::graph`

Plan-and-Execute 模式的便利工厂：planner 发出 JSON 步骤数组，executor
通过内部 ReAct 循环逐个消费它们，responder 从 `past_steps` 组成最终答案。

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

| 参数 | 类型 | 描述 |
|-----------|------|-------------|
| `provider` | `std::shared_ptr<Provider>` | 每个阶段共享的 LLM provider |
| `tools` | `std::vector<std::unique_ptr<Tool>>` | executor 可调用的工具（所有权转移） |
| `planner_prompt` | `std::string` | planner 的系统提示；必须指示模型以 JSON 步骤数组回复（容忍被围栏的 ```json 块和前置散文） |
| `executor_prompt` | `std::string` | 单步骤 executor（内部 ReAct 循环）的系统提示 |
| `responder_prompt` | `std::string` | 最终合成阶段的系统提示 |
| `model` | `std::string` | 模型覆盖（空表示使用 provider 默认值） |
| `max_step_iterations` | `int` | 每步骤的 executor 内部工具调用迭代上限 |

**填充的通道：** `plan`、`past_steps`、`final_response`、`messages`。

**返回：** 一个准备好运行的编译后的 `GraphEngine`。工厂将其自定义节点类型
注册到进程全局 `NodeFactory` — 这些名称出现在 `export_schema()` 中，以使
Studio 调色板在不获取额外依赖的情况下将它们显示为可用节点。

---

<a id="12-llm-module"></a>
## 12. LLM 模块

### SchemaProvider

`SchemaProvider` 接收获准的 `sp::descriptor::ValidatedDescriptor`、`sp::runtime::Options` 及可选 `SchemaProvider::Defaults`。descriptor 是 closed/versioned 数据 admission，不是请求/响应 interpreter 或任意 primitive registry。credential 应放在 runtime options，而非公开 descriptor。Defaults 仅包含 typed OpenRouter routing 和 Responses 保留 (`responses_store`)，后者仅适用于 Responses。Hosted OpenRouter routing、retention、JSON 格式仍是声明的 typed 控制。Images、Veo、Decisions 使用独立的 NeoGraph typed client 和独立授权，不继承 SDK chat grant。

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

`Agent::run`、`run_stream`、`complete` 返回完整 `sp::runtime::Result`，接收 `std::vector<sp::Message>`。`run_stream` 接收每个真实 turn 的 typed event，不会为显示而丢弃答案并重新请求。`outcomes()` 保留各个结果，`usage()` 提供 nullable 报告。调用方明确选择模型。

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

### json_path 工具

**头文件：** `<neograph/llm/json_path.h>`
**命名空间:** `neograph::llm::json_path`

使用以点分隔的路径字符串导航和修改 JSON 值的工具函数。
这些函数供 `SchemaProvider` 内部使用，也可用于一般场景。

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

| 函数 | 描述 |
|----------|-------------|
| `split_path(path)` | 将点路径字符串拆分为多个片段。例如：`"choices.0.message"` 变为 `["choices", "0", "message"]` |
| `at_path(root, path)` | 按点路径进入 JSON 值；数字片段用于索引数组。如果路径不存在则返回 `nullptr` |
| `at_path_mut(root, path)` | `at_path` 的可变版本 |
| `has_path(root, path)` | 如果 JSON 值中存在该点路径则返回 `true` |
| `get_path<T>(root, path, default_val)` | 返回路径处转换为 `T` 类型的值；路径不存在或转换失败时返回 `default_val` |
| `set_path(root, path, value)` | 在点路径处设置值，并按需创建中间对象 |

**示例：**

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

<a id="13-mcp-module"></a>
## 13. MCP 模块

**头文件：** `<neograph/mcp/client.h>`
**命名空间:** `neograph::mcp`

Model Context Protocol（MCP）客户端实现。它连接 MCP 服务器、发现
可用工具，并将其包装为 NeoGraph 的 `Tool` 实例。

提供两种传输方式：

- **HTTP** — `MCPClient("http://host:port")`. 已发现的工具会保留其来源 Streamable HTTP 会话，包括 `Mcp-Session-Id`、协商的
  协议版本、超时设置和自定义头。
- **stdio** — `MCPClient({"python", "server.py"})`。客户端在 `fork` 前解析 `PATH`，
  子进程通过 `execve` 启动，并连接双向管道以通过 stdin/stdout 交换换行分隔的
  JSON-RPC。只要 `MCPClient` 或其生成的任意 `MCPTool` 存在，子进程就会持续运行；
  析构时发送 SIGTERM，并通过 `waitpid` 回收（约 500 ms 后回退到 SIGKILL）。

### MCPTool

将单个 MCP 服务器工具包装为本地 `Tool` 实现。已发现的工具会保留其来源
协议会话，与传输方式无关。

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

通常不需要直接构造 `MCPTool`；`MCPClient::get_tools()`
会发现并完成包装。

### MCPClient

连接 MCP 服务器、执行初始化握手并
提供发现和调用工具的方法。

 > `MCPClient` 不应被继承，请直接使用。
> `rpc_call_async()` 是实际实现，`rpc_call()` 只是
> 薄同步门面。参见 [`ASYNC_GUIDE.md` §9.5](ASYNC_GUIDE.md#95-mcpclient).

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

**线路协议：** NeoGraph 的 MCP 客户端使用
`protocolVersion = "2025-11-25"`。HTTP 传输在每个 JSON-RPC 请求上发送
`MCP-Protocol-Version` 头（与 Round 1 和 Round 3 规范对齐）；stdio 传输在
`initialize` 负载中携带相同版本。运行旧协议版本的服务器可能
拒绝这些请求，请固定服务器版本或升级。

| 方法 | 描述 |
|--------|-------------|
| `MCPClient(url)` | 构造 HTTP 模式客户端 |
| `MCPClient(argv)` | 生成子进程并构造 stdio 模式客户端。`argv[0]` 在 fork 前通过 `PATH` 解析；exec 失败将在首次 RPC 中表现为连接错误。为安全起见拒绝 Windows `.bat`/`.cmd` |
| `initialize(client_name)` | 执行一次 MCP 初始化握手。重复调用幂等；协议或传输失败会抛出异常 |
| `get_initialize_result()` | 返回协商后的协议、能力、服务器信息、说明和原始结果 |
| `list_tools(cursor)` | 获取一页结果，并将游标视为不透明值 |
| `get_tool_definitions()` | 遍历所有页面并保留完整工具元数据 |
| `get_tools()` | 发现所有页面并返回保留会话的 `MCPTool` 实例 |
| `call_tool(name, arguments)` | 按名称和参数调用工具，返回原始 JSON 响应 |
| `call_tool_result(name, arguments)` | 保留 content、structured content、`isError` 和 `_meta` 的类型化结果 |
| `rpc_call_async(method, params)` | 协程版本。它是“实际”实现；`rpc_call` 是薄同步包装。 |

**HTTP 用法：**

```cpp
neograph::mcp::MCPClient client("http://localhost:8000");
client.initialize();
auto tools = client.get_tools();
```

**stdio 用法：**

```cpp
// argv[0] is resolved through PATH before fork; inherited fds close before execve.
neograph::mcp::MCPClient client({"python", "/path/to/server.py"});
client.initialize();
auto tools = client.get_tools();   // Tools retain the protocol session/process.
```

---

<a id="14-util-module"></a>
## 14. Util 模块

**头文件：** `<neograph/util/request_queue.h>`
**命名空间:** `neograph::util`

### RequestQueue

带工作线程池和背压支持的无锁请求队列。
在服务器应用中，将 HTTP 连接接收与 LLM 调用并发解耦。

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

| Constructor 参数 | 类型 | 默认值 | 描述 |
|-----------------------|------|---------|-------------|
| `num_workers` | `size_t` | `128` | 工作线程池中的线程数；0 会抛出 `std::invalid_argument` |
| `max_queue_size` | `size_t` | `10000` | 待处理任务的最大数量。超过该上限的任务会被拒绝 |

| 方法 | 描述 |
|--------|-------------|
| `submit(task)` | 原子预留 pending capacity 后将 callable 入队。接受时 `first` 为 `true`。已满或已关闭的队列返回 `false` 和 invalid future；内部 enqueue 失败返回 `false` 和可传播错误的 valid future。已接受的 future 在任务完成时就绪或传播任务异常。 |
| `close()` | 幂等地拒绝新工作。外部调用者等待所有 worker 退出，未领取的工作以 `std::runtime_error("RequestQueue is closed")` 完成。已领取 callable 可完成。callable 可以调用 `close()` 发起关闭，但不会等待自身。 |
| `is_closed()` | 报告 `close()` 是否已开始拒绝新工作。 |
| `stats()` | 返回当前队列统计信息的快照 |

队列内部使用 `moodycamel::ConcurrentQueue` 实现无锁入队/出队，
并使用条件变量唤醒空闲工作线程。

**用法：**

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

<a id="usage-examples"></a>
## 使用示例

<a id="minimal-react-agent"></a>
### 最简 ReAct Agent



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


<a id="custom-graph-with-conditional-routing"></a>
### 带条件路由的自定义图

构建带条件边的图：

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

<a id="human-in-the-loop-with-checkpointing"></a>
### 带检查点的人类参与

使用中断请求人工批准：

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

<a id="dynamic-fan-out-with-send"></a>
### 使用 Send 的动态扇出

使用 `Send` 实现 map-reduce 模式：

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

每个 `Send` 都会以不同输入调用 `"process_item"` 节点。引擎
会执行所有 send、收集其通道写入，然后再进入下一条
图中的边。

<a id="routing-override-with-command"></a>
### 使用 Command 的路由覆盖

使用 `Command` 同时更新状态并控制路由：

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

返回 `Command` 时，其 `updates` 会应用到状态，执行
直接跳转到指定的 `goto_node`，绕过正常的边路由。

<a id="schemaprovider-multi-llm-support"></a>
### SchemaProvider 多 LLM 支持

`SchemaProvider` 接收获准的 `sp::descriptor::ValidatedDescriptor`、`sp::runtime::Options` 及可选 `SchemaProvider::Defaults`。descriptor 是 closed/versioned 数据 admission，不是请求/响应 interpreter 或任意 primitive registry。credential 应放在 runtime options，而非公开 descriptor。Defaults 仅包含 typed OpenRouter routing 和 Responses 保留 (`responses_store`)，后者仅适用于 Responses。Hosted OpenRouter routing、retention、JSON 格式仍是声明的 typed 控制。Images、Veo、Decisions 使用独立的 NeoGraph typed client 和独立授权，不继承 SDK chat grant。

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


<a id="mcp-tool-integration"></a>
### MCP 工具集成

`Agent::run`、`run_stream`、`complete` 返回完整 `sp::runtime::Result`，接收 `std::vector<sp::Message>`。`run_stream` 接收每个真实 turn 的 typed event，不会为显示而丢弃答案并重新请求。`outcomes()` 保留各个结果，`usage()` 提供 nullable 报告。调用方明确选择模型。

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


## 超越本导览

`include/neograph/` 下的头文件包含本导览未展开的公共接口。
下面每个区块都是对该权威源码级参考的简短指引。

### `neograph::a2a` — Agent 到 Agent 协议

**头文件：** `<neograph/a2a/{client,server,types,a2a_caller_node}.h>`
基于 Streamable HTTP 的 JSON-RPC 2.0。`A2AClient` 调用远程
agent（`message/send`、`tasks/get`、`tasks/cancel`、AgentCard
发现、`message/stream` SSE）；服务器端通过
`GraphAgentAdapter` 将 NeoGraph `GraphEngine` 适配为 A2A 端点。支持
`v0.3` / `v1` 双版本方法名分发，参见提交 `bc675a1`。流式传输使用
`SseFrameSplitter`（客户端）和 httplib 分块传输（服务器）。调用者节点
将 A2A 调用嵌入为图节点。

**公共头文件：** [`include/neograph/a2a/`](../include/neograph/a2a/)。

### `neograph::acp` — Agent 客户端协议

**头文件：** `<neograph/acp/{server,types}.h>`
编辑器↔agent 通过 stdio 上的换行分隔 JSON 进行 JSON-RPC 通信（Zed、
Gemini CLI、Neovim CodeCompanion）。双向通信包括：client→agent
（`initialize`、`session/{new,prompt,cancel}`）以及通过延迟绑定的 `ACPClient`
实现的 agent→client（`fs/{read,write}_text_file`、`session/request_permission`）。
`ACPServer::handle_message` 在工作线程上异步分发提示，`max_inflight_prompts=32`
为上限，并按会话执行单飞控制 + `-32000` 背压。

**公共头文件：** [`include/neograph/acp/`](../include/neograph/acp/)。

### `neograph::async` — HTTP/SSE/WS 辅助工具

**头文件：** `<neograph/async/{conn_pool,http_client,sse_parser,ws_client,curl_h2_pool,run_sync}.h>`
通用 NeoGraph HTTP/SSE/WebSocket helper 可用于非 provider 集成，但不是 SchemaProvider 的 transport、codec 或 retry 权限。typed chat-family 调用使用 SDK runtime 与 `ProviderMode`；旧 Responses WebSocket provider 路径和 descriptor stream parser 已删除。

**公共头文件：** [`include/neograph/async/`](../include/neograph/async/)。

### 持久化检查点后端

**头文件：** `<neograph/graph/postgres_checkpoint.h>`,
`<neograph/graph/sqlite_checkpoint.h>`
`PostgresCheckpointStore` — 基于 libpq，使用三张表的 schema（`neograph_*`），
按 `(thread_id, channel, version)` 对通道 blob 去重；与 LangGraph
`PostgresSaver` 对等。异步初始/替换连接对所有主机使用一个全局截止时间：
正数 `connect_timeout` 直接写入连接字符串（最小 2 秒），否则使用 30 秒安全默认值。
初始连接建立前无法取得环境变量和 service 文件中的超时值，因此使用该默认值。
同步 libpq 连接的超时行为不变。
`SqliteCheckpointStore` — 形态相同的单文件后端，适合边缘设备/单主机部署。
**公共头文件：**
[`PostgresCheckpointStore`](../include/neograph/graph/postgres_checkpoint.h) ·
[`SqliteCheckpointStore`](../include/neograph/graph/sqlite_checkpoint.h)。

### 本导览未涵盖的其他公共接口

- 可选 `ProviderControls` 是调用方选择，不是强制默认值或暗中 clamp 的 cap。不支持的 family 控制在 dispatch 前拒绝。有界调用需要获准的真实模型 input/output 上限；缺失时为 `LimitUnknown`。预留是保守的支出权限，而非报告使用量、预测或账单。未知/部分/delivery-unknown 结果保留 hold，真实最终报告用于结算，超额报告也全额计入。retry 是显式单层，默认 off，具有有界 window 与 unknown-prior hold；没有隐藏重发。
- **`neograph::AsyncTool`** — `Tool` 的对应类型，为天然适合协程的工具（HTTP 获取、MCP 调用）
  提供 `execute_async(json)`。同步 `execute()` 通过 `run_sync` 以 `final` 方式路由。
- **`neograph::graph::NodeCache`** — 每节点记忆化缓存，通过构造时的 `EngineConfig::cached_nodes`
  选择性启用（setter 仍作为兼容接口保留）。
- **`neograph::graph::create_deep_research_graph`** —
  open_deep_research 风格的 supervisor + 子研究者扇出，由
  `examples/25_deep_research.cpp` 使用。Round 2 审计新增了
  `BriefNode` LLM 重写、`FinalReportNode` token 限制重试以及
  `ClarifyNode` HITL 闸门。

如果本导览中没有所需类型，请直接检查 `include/neograph/`；
每个公共头文件都包含其参考文档。
