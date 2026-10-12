<!-- neograph-i18n: source=docs/reference-en.md locale=zh-CN source_sha256=f8bfe8568eee9c7f3c95490bb005e472614caa09c7472d8724b68e1c6ad60432 -->
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

附加模块在 `include/neograph/{a2a,acp,async}/` 提供公开头文件。本导览不涵盖这些模块，请查阅头文件中的精确契约。

**便利头文件：** `#include <neograph/neograph.h>` 包含完整的 core + graph engine API。

需要 CMake 3.20 或更高版本。Core 公开拥有所有权的 typed provider 契约，所以 `NEOGRAPH_BUILD_LLM=OFF` 时 SchemaProvider runtime 仍必需。显式 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` 优先；否则先查找已安装的 `SchemaProvider` runtime package。若没有且 `NEOGRAPH_FETCH_SCHEMAPROVIDER=ON`（默认），就下载 `cmake/NeoGraphSchemaProvider.cmake` 固定的不可变 GitHub archive。Offline build 需安装 SDK、将 `CMAKE_PREFIX_PATH` 指向其 prefix，并传入 `-DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`。CMake 不猜测 sibling checkout，也不选择已删除的 bundled interpreter。禁用 NeoGraph 可选 HTTP module 时，SDK runtime 的 transport 依赖仍必需。已验证的平台是 Linux/POSIX。Windows NTFS 与 macOS 实现已存在，但尚未在该 SDK 上验证；WASM provider runtime 支持尚未确立。

SDK imported target 提供 `include/SchemaProvider` include root；公开示例直接使用 `<descriptor/descriptor.h>`、`<runtime/client.h>`、`<neograph/llm/schema_provider.h>`，不依赖 recipe 专用 helper。

已安装 SDK package 须恰为 `0.3.0`，并匹配 interface revision 6 header 和 shared-library generation 6。版本匹配本身不批准旧 interface/ABI binary。

```cmake
find_package(SchemaProvider 0.3.0 EXACT CONFIG REQUIRED COMPONENTS runtime transport)
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
  - [计价证据与价格目录](#pricing-evidence-and-the-price-catalog)
- [3. Tool 接口](#3-tool-interface)
  - [Tool](#tool)
  - [工具失败契约](#tool-failure-contract)
  - [节点失败契约](#node-failure-contract)
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
    std::string tool_status;
    bool tool_retryable = false;
    bool tool_effect_uncertain = false;
    std::vector<std::string> image_urls; // base64 data URLs or HTTP URLs for Vision
    std::string reasoning;
    json reasoning_details = json::array(); // portable data, not native authority
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
    std::optional<bool> strict; // OpenAI Responses strict mode; unset keeps the SDK default
};
```

| 字段 | 类型 | 描述 |
|-------|------|-------------|
| `name` | `std::string` | 唯一的工具名称 |
| `description` | `std::string` | 显示给 LLM 以解释工具用途的描述 |
| `parameters` | `json` | 描述可接受参数的 JSON Schema 对象 |
| `strict` | `std::optional<bool>` | 此 function 工具的 OpenAI Responses strict 模式。未设置时沿用 SDK policy 默认值，该默认值只应用于 strict 兼容的 schema（每个 object 都设置 `"additionalProperties": false` 并在 `required` 中列出每个 property），其他 schema 以 `"strict": false` 发送。`false` 按原样发送；对不兼容的 schema 设置 `true` 会在 dispatch 前以 `InvalidRequest` 失败。其他 family 拒绝 `true`。Python：`ChatTool(..., strict=None)` |

### Owned Outcome

提供方调用返回 `sp::runtime::Result`，即持有 `sp::Completion` 或 `sp::Failure` 的不可变、拥有所有权的 `std::shared_ptr<const sp::Outcome>`。请保留完整结果，而非仅显示文本。顺序消息/part、native continuation、存在的 wire envelope、顺序 raw 观测、停止依据及真实尝试元数据在调用与客户端销毁后仍然保留。`input_total`、`output_total` 和 `total` 等用量计数器为 `std::optional<sp::Count>`；存在的 count 有 `uint64_t value` 和 `Evidence`。`Usage` 还记录 stage、quality 和 conflict。缺失表示未知，不应伪造零值。失败保留原始部分结果。`ProviderFailure::outcome()` 与 `ProviderObserverError::outcome()` 保留真实结果，后者的 `cause()` 也保留观察者异常。

`sp::Completion::wire_envelope` 和 `sp::PartialCompletion::wire_envelope` 可为空，且随提供商家族而异；不存在时 Python 视图返回 `None`。当前缓冲式 Chat 将此字段保持为空，并将完整响应文档保留在 `raw_events` 的 `sp::RawWire{type="chat.completion", payload=document}` 中（Python 为 `ProviderRawWire`）。请检查是否存在，并查看实际的类型化 raw 证据；不会通过回退合成封装，也不会把部分失败变为成功。拥有所有权的线协议证据，包括提供商的私有字段，在提供商销毁后仍保存在留存的结果中。原生追踪排除 raw 封装/事件和原生重放/推理；用于检查的 JSON 副本不授予原生权限或财务权限。

调用方应使用完整的类型化消息/部分、逻辑角色和文本，并在继续执行需要时保留真实原生所有权。检查点与 Chat 请求的文本 content 可以采用文本字符串或有效的类型化文本部分数组；偶然采用的任一种序列化形状都不是通用契约。

重放真实 `NativeContext` 时，须保留原始 `request.messages` 的完整前缀，再按顺序追加直接返回的 `outcome.messages`，并保留其原生所有权。直接结果包含新返回的消息，不包含原始请求历史。仅重放其中的 assistant 消息会在任何线协议 I/O 之前被 `ReplayIneligible` 拒绝。从 `NativeArchive` 恢复的 assistant 仍须提供原始完整前缀；存档保管权限不能替代历史谱系检查。图的 `RunResult.native_messages` 已包含完整历史，不要再次把原始输入加到开头。

`UsageAccumulator::snapshot()` 返回累计报告。`total_tokens_wide()` 返回已计费 token 与未解决预留之和，不能把它显示为报告用量。结算要求具有 input/output count 的 final、consistent 报告，并计入有依据的最大 total，不截断超额用量。任何累计报告缺少 counter，汇总该 counter 也为未知。预留、本地计费和 vendor 发票是不同的记录。

实际结果存在后，若 post-effect 结算或 terminal receipt 持久化失败，`ProviderDispatchOutcomePersistenceError::outcome()` 保留原始不可变结果，`cause()` 保留原始持久化异常。若 delivery 也失败，`delivery_error()` 保留原始观察者异常。持久化成功后的观察者失败原样重新抛出原异常；未知/无结果 transport 失败不会伪造 outcome。

### Portable projections

`ChatMessage` / `ChatTool` 和 JSON 只是 portable projection，不是 native 权限。Portable 格式仍为 [`provider-message-v2`](../schemas/provider-message-v2.schema.json)、[`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json)。真实 C++ checkpoint sidecar 保留内存 native seal。持久 native 历史需要 host-owned `sp::NativeArchive`：closed v3 / `spna3` 使用独立密钥提供经认证的 owner-private custody；archive v2 被拒绝，不升级或解释。认证绑定全部 semantic descriptor 选择（origin/path/header、policy、请求 field mapping、usage path、stop mapping）、owner 和精确 custody binding。这不是加密或 vendor-issuer 认证；不得公开 archive 正文、密钥、native blob 或 raw wire 观测。Archive 是证据存储，不是资金 grant 或 spending lease。Program/external bank 仍由独立 journal 拥有，复制 snapshot 不能创建 credit。

`RuntimeHistoryRecord` 的无参数 `serialize_canonical()` 支持可移植记录。持久原生历史须使用 `serialize_canonical(archive, owner_id)`，并提供所有者范围与该 ID 一致的存档。Python 暴露同一重载以及 `RuntimeHistoryRecord.parse(stored_bytes, archive=None, owner_id="")`；面向可移植记录的默认参数不会在缺少匹配存档时授权原生恢复。Python 的解析和存档感知序列化会在原生工作期间释放 GIL。JSON 观测数据仍与存档认证的保管权限不同。

Python 通过 `ContextStore.hydrate_records(range)` 与 `history_record_by_message_id(feed, message_id)` 保持类型化保管权限，后者返回记录或 `None`。`SQLiteContextStore(database_path, archive=None)` 接受真实存档，`LocalProgramHost` 将最后一个可选参数 `native_history_archive=None` 传入 `RuntimeConfig`。这些参数不会伪造授权或添加持久 Program 存储后端。返回的 `RuntimeHistoryRecord.message` 是保留真实共享原生所有权的独立类型化副本，修改它不能改写不可变 RAW 标识。存储构造与类型化检索会释放 GIL；当必须提供保管权限时，无存档存储会拒绝原生历史。

包含 `Assistant` 消息的 RAW `RuntimeHistoryRecord` 必须使用 `RuntimeTrustClass.ModelOutput`；`RuntimeTrustClass.UntrustedInput` 仅接受 `User` 消息。这些是实际的 Python enum 名称。trust class 标明记录的角色与来源边界；选择它不会授予原生重放保管权限或支出、执行权限。

**独立预算 bank。** 独立 bank 要求单调 trusted-store namespace obligation，以及真实不可变 original owner/thread/graph scope、ceiling、deadline/clock identity、generation。只有对全部 checkpoint commitment/revision 的精确 durable head CAS 才可发放 host-owned opaque lease。精确 pending effect window 必须在 provider I/O 前持久化；结算必须采用真实 SDK outcome 及实际 charge、nullable report、hold、dedup identity。Checkpoint 与 next head 在同一 owned actor/revision 下原子 publish。删除 bank metadata、prune checkpoint、replay old authenticated snapshot、覆盖同一 ID 或失去 actor 都不会授予 credit。已有 65 hold 时将 ceiling 130 降至 129，不能再批准另一个 65；已证明 no-effect 的失败可 release unchanged head，使 authentic 130 恢复仍可进行。Crash/unknown/lost-lease window 保持 hold，不 refund/retry/fallback。Plain archive 配置不授予 money 或 native spending lease，当前 `config.usage` 不能替换既有独立 obligation。Program/external bank 仍由各自的 journal 所有。

**Managed budget lease 声明。** `<neograph/graph/checkpoint.h>` 声明 `ManagedBudgetLeaseScope`，包含 `owner_scope`、logical `thread_id`、private backend `storage_thread_id`、`graph_identity`、`original_ceiling`、`original_deadline_ticks`、`deadline_clock_identity`。`OwnedManagedBudgetLease` 暴露 read-only `scope()`、`actor_id()`、不可变 `bank_generation()`、`revision()`、`head_checkpoint_id()`、`head_commitment()`，没有公开 authority-import constructor。`ManagedBudgetEffectReceipt` 暴露 `active()`、`effect_id()`、`claim_amount()`、`request_digest()`；default receipt 不授予权限。`CheckpointStore` 声明 `acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`、`begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`、`settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`、`publish_managed_budget_checkpoint(lease, checkpoint)`、`release_managed_budget_lease(lease)` 及 `_async` counterpart。Sync `CheckpointStoreCore` 与 `AsyncCheckpointStore` 暴露各自 variant。`managed_budget_checkpoint_commitment(checkpoint)` 绑定完整持久 checkpoint，而非仅 bank JSON。

**Shared-bank fork（InMemory）。** 原始真实 C++ fork 使用 ONE original financial journal 和 trusted current branch head，不复制 grant。`publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)` 及 `_async` 要求 authentic current source/full commitment 与实际 same-bank native C++ pointer；durable standalone fork 仍明确 unsupported。`OwnedManagedBudgetLease::scope()` 及 original owner/thread/graph、ceiling、deadline/clock、generation 保持不可变。Read-only store-issued `execution_thread_id()` / `execution_storage_thread_id()` 单独选择 execution branch；`GraphState::budget_original_thread_id()` 标识原始 financial bank。精确 selected-branch head CAS 和 global actor/revision 将所有 branch 对 canonical current counter、pending effect、burned identity 串行化。Original/fork branch 保持可用但不补充额度。Stale snapshot、checkpoint copy、imported JSON 不能发放 alias 或回退 head。保存的 original ceiling 与 effective fork ceiling 相互独立；放宽它或仅用 JSON 恢复都会被拒绝。Unbounded reported observation 是事实 data，不是 finite grant。只有已证明 zero-effect 的 lease 能 release unchanged head；unknown/pending effect 保留 obligation。

**Lease release 错误。** `<neograph/graph/engine.h>` 中 `graph::ManagedBudgetLeaseReleaseError` 继承 `ProviderOutcomeError`。`cause()` 保留原始 execution exception，`release_error()` 暴露次要 durable lease-disposition 失败。`outcome()` 在存在真实 SDK 证据时保留它，若没有 SDK outcome 则为 null；release 失败不能伪造结果或授权重新 dispatch。Closed `_neograph_managed_budget_scope` metadata 描述原始 logical scope/cap/deadline clock/generation，但只是 data，不是 backend CAS 权限。

**Archive owner 与 retention。** 只有 finite standalone root 或 authenticated finite source 才从真实配置的 `sp::NativeArchive::owner_scope()` 继承省略的 original owner；unbounded/plain owner metadata 语义不变。显式冲突的 archive owner 在 lease acquire 前拒绝。`CheckpointStore::retains_native_checkpoint() const noexcept` 及对应 Core/Async storage capability 默认 false；真实 InMemory backend override 为 true，wrapper 必须委托真实 retention。此 read-only 描述允许合法 unleased/plain/unbounded C++ native checkpoint custody，但不授予 spending credit 或 native replay authority。Leased custody 使用真实 store-issued receipt，而非 JSON flag 或猜测的 store type。

**Native-custody gate。** Managed effect begin 在任何 pending-effect/slot/held-window 修改前要求真实绑定的 NativeArchive 或实际 local store-issued private C++ retention capability。Private capability 不从 JSON import，也不经 wire 传输。C++ sidecar 无法跨越边界，因此即使 remote backend 是 InMemory，gRPC 仍要求真实 client/server archive。Archive 未提供 finite source owner 时，原始 anonymous owner scope 保持空值；真实 archive binding 必须匹配 original scope。Financial head/lease 证据本身不证明 native-custody readiness。

诊断 JSON 保留原始 raw byte，包括语法有效的 duplicate-key 文档；可执行请求/config admission 仍拒绝重复键。原始 non-2xx 响应 JSON 保留在 `http.error` 证据中，不进行第二次有损 parse。named SSE error 优先于之后的正常 stream close。诊断/provider metadata 按获准 source extent 限制，而非无关的小 error-text cap。

`ProviderRequest::observer_limits` 仅供宿主使用。显式 `max_events`、`max_bytes` 必须为正且只能降低获准 SDK 交付上限。`provider-request/v3` digest 绑定实际 limit、mode、encoded body、retry policy 及全部 semantic descriptor binding。Bridge 在 queued/draining batch 中同时计入真实 PMR vector/map capacity 与拥有的 event/document byte，并在 queue mutex 之外请求 cancellation。名为 `messages` 的 Generic channel 不会被强制转成 chat。将 native `history` channel mapping 到 `messages` 会保留 C++ sidecar，而不是从 JSON 制造 native 权限。

`ProviderOutcomeError` 是保留结果的共同 host-error base；`ProviderObserverError` 和 `ProviderDispatchOutcomePersistenceError` 保留完整 drain 后的 SDK 结果及原始 `cause()`，后者还通过 `delivery_error()` 保留次要 observer 失败。`ProviderFailure::outcome()` 保留 SDK 失败本身。这些证据不授权 Node/Program 重新 dispatch。SDK 是 provider retry 的唯一所有者，调用方选择的 `max_output_tokens` 不会被暗中 clamp。

`ProgramFailure` 保留 live `provider_outcome`、`provider_cause`。Canonical factual SDK witness 将真实 archive custody 绑定到 owner/run/version/bundle/operation/attempt；Runtime 在暴露恢复后的失败前立即恢复配置的 custody。公开 data-only `ProgramResult::create()` 不能用预填 witness 绕过；未解析完的 parsed seal 不是可执行结果。进程重启后原始 exception pointer 不可用（`provider_cause == nullptr`），不会从 text 重建。没有 `native_history_archive` 的宿主无法保存 provider 的 native replay 状态，因此会发布去掉该状态的失败：消息、usage、stop reason、error 和 raw event 保持不变，只去掉每条消息的 native seal，并在 witness 中写入 `provider_outcome_native_retained: false`。结果的 `provider_outcome` 就是这份副本，发起调用的进程里原始 outcome 仍留在 `provider_cause`。其他原因造成的无法持久化（例如 archive 写入失败）的失败，不能 serialize/publish/replay。

Python `LocalProgramHost` 析构时，会在 `ProgramRuntime` 取消、排空并 join 调度器工作期间释放调用者的 GIL，使正在执行的 Python 节点能够完成。销毁其他 host 成员及其持有的 Python 回调/对象之前，会重新获取 GIL。此变更仅涉及结束处理，不暴露额外的 capability binder 或执行权限。

`RecordedBindingSet` 是 source-bound move-only data，不是调用方提供的 dispatcher。可信 Catalog `recorded_capability_binder` 独立读取真实持久 source event，materialize captured-only capability。`ProgramRuntime::replay_recorded()` 检查原始 selected-source permission，再通过 durable CAS 转移真实剩余 bank；inherited spend 不是新的 model grant。旧 `start_recorded` 续期 API 已删除。InMemory/File/SQLite/PostgreSQL Program store 在整个执行期间保留精确不可变 owned lease，不因 expiry 续期。Controlled JavaScript 仍验证 underlying capability manifest，消费精确 completed command 结果，不重新 dispatch external effect。

**Recorded replay 记账。** Captured command replay 在执行前仅为新的 CPU wall-time/Core work 建立 durable reservation，再通过 result CAS publish 测量 work 与新产生的 Core checkpoint。不消耗新的 model、money、Program-operation allowance，也不重新 dispatch captured external effect。未结算 reservation 保持 debit。Await channel receive、timer wait/cancel、handoff wait 的开始/release 在所属 executor/strand 上串行化。

```cpp
#include <neograph/types.h>

neograph::json observe_result(const sp::runtime::Result& result) {
    if (!result) throw std::invalid_argument("Missing provider outcome");
    return neograph::outcome_projection_json(*result);
}
```

<a id="adl-serialization"></a>
### ADL 序列化

用于 nlohmann/json 集成的参数依赖查找（ADL）序列化函数。这些允许直接使用
`json j = my_tool_call;` 和 `my_tool_call = j.get<ToolCall>()`。

```cpp
void to_json(json& j, const ToolCall& tc);
void from_json(const json& j, ToolCall& tc);

void to_json(json& j, const ChatMessage& msg);
void from_json(const json& j, ChatMessage& msg);
```

ADL serialization 保留 portable message/tool field 及其声明默认值，不从 JSON 重建 native SDK continuation 权限。

---

<a id="2-provider-interface"></a>
## 2. Provider 接口

公开契约是拥有所有权的 typed 准备/dispatch，而非同步/异步 virtual completion 对。`ProviderRequest.payload` 是 Chat、Messages、Responses、Gemini、Interactions 的 SDK 请求 variant。`ProviderMode::Collect` / `Stream` 独立于观察者是否存在来选择传输。`on_event` 接收借用的 typed `sp::Event` view；只复制回调后仍需要的数据。不允许 raw JSON override 或通过 portable projection 导入 native 权限。

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

`make_provider_request(provider, model, messages, tools, controls, mode)` 选择一个封闭 family。错误 family 控制引发 `std::invalid_argument`；SDK prepare 在 I/O 前检查 enum 值、获准 origin、模型规则、tool 声明和 native binding。`ProviderControls` 没有 dictionary 逃生通道。optional 区分未指定与显式 `false`、空选择。

| Family | `ProviderControls` 字段及 SDK 映射 |
|---|---|
| `openai.chat` | `max_output_tokens`, `temperature`, `top_p`, `reasoning_effort`, `service_tier`, `provider`, `response_format`; `chat_reasoning` → `sp::chat::Request::reasoning`, `include_reasoning`, `usage_include` → `usage.include`, `models` |
| `openai.responses` | `max_output_tokens`, `max_tool_calls`, `temperature`, `top_p`, `reasoning_effort`/`reasoning_summary` → `reasoning`, `service_tier`, `required_tool`, `provider`, `response_format`, `store`, `system` → `instructions`, `account_scope`; `previous_response_id`, `previous_response_history`, `parallel_tool_calls`, `verbosity` → `text.verbosity`, `truncation`, `responses_include` → `include` |
| `anthropic.messages` | `max_output_tokens` → `max_tokens`, `temperature`, `top_p`, `thinking_budget`, `system`, `account_scope`, `provider`; `thinking_mode`, `output_effort` → `output_config.effort`, `cache_control`, `messages_tool_choice` → `tool_choice` |
| `google.generate` | `max_output_tokens`, `temperature`, `thinking_budget`, `include_thoughts`, `required_tool`, `system`, `account_scope`; `gemini_history_mode` → `history_mode`, `gemini_thinking_level` → `thinking_level`, `safety_settings`, `gemini_tool_choice` → `tool_choice` |
| `google.interactions` | `max_output_tokens`, `thinking_level`（optional string）, `thinking_summaries`, `service_tier`, `required_tool`, `system`, `account_scope`; Generate 的 enum `gemini_thinking_level` 不适用 |

Chat 的 `sp::chat::ReasoningOptions` 包含 optional `effort`, `max_tokens`, `exclude`, `enabled`。其 nested reasoning object、`include_reasoning`、`usage_include`、备用 `models` 需要 policy 声明的 OpenRouter origin。`sp::OpenRouterRouting` 也仅适用于 Chat/Responses/Messages 的声明 OpenRouter origin。gateway 形式的模型名不会批准其他 origin。SDK payload 还提供 family typed tool 声明、Responses `hosted_tools`、strict/deferred tool 选项；使用实际 payload variant，不添加 raw JSON。

Responses SSE 要求终止事件 `response.completed` 或 `response.incomplete`。`response.done` 仅在作为携带相同完整终止 Responses envelope（completed 或 incomplete）的别名时被接受；非终止或格式错误的 `response.done` 会以协议错误失败，`response.content_part.delta` 及其他未列出的事件为 `Unsupported`。在已声明的 OpenRouter origin 上，Responses 与 Messages 还接受网关针对 `~vendor/model-latest` 等路由别名报告的实际模型名；服务模型在同一响应内不得变化，replay 仍绑定到请求的模型。

| SDK 类型 | 封闭值或成员 |
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
| `sp::gemini::ToolChoice` / `ToolChoiceMode` | `mode`: `Auto`, `Any`, `None`, `Validated`; `allowed_function_names`: 已声明函数名 vector |

Messages 要求正 output cap。无 mode 的 thinking budget 选择 `Manual`，须达到获准 minimum 且严格小于 cap。`Adaptive`/`Disabled` 拒绝显式 budget。Manual/adaptive 省略其他方面有效的 temperature 并检查 thinking `top_p` minimum，但不覆盖模型 temperature 禁令。强制 `Any`/`Tool` 要求 tools 和 disabled thinking；`Tool` 指定已声明 client tool，其他 mode 拒绝 `name`，`None` 拒绝 `disable_parallel_tool_use`。Generate 的 thinking budget/level 与 `required_tool`/`gemini_tool_choice` 分别互斥；allowed-function 列表须引用已声明函数。sampling 遵守 family/policy 范围。Generate 没有 `top_p`，Interactions 不接受两个 sampling 字段。

`FamilyPolicy.temperature_forbidden_model_prefixes` 按 ASCII 大小写不敏感方式匹配完整模型名及最后 `/` 后的 suffix。内置 Chat/Responses 前缀为 `gpt-5`, `gpt-6`, `o1`, `o3`, `o4`；Messages 为 `claude-opus-4-7`, `claude-opus-4-8`, `claude-opus-5`, `claude-sonnet-5`, `claude-fable-`。显式禁用 temperature 在 I/O 前拒绝，包括 gateway 前缀模型。

#### Responses provider-held continuation

`previous_response_id` 选择 provider 保存状态。请求 `messages` 为 NEW INPUT ONLY，不重发过去对话。`previous_response_history` 是不发送的本地所有权证据 `std::vector<sp::Message>`。cursor 单独可允许新 text/image 输入，但不能授权 client tool result。初次 captured response 须提供 authentic original prefix 加上 ID 等于 cursor 的 terminal assistant response。后续 authentic in-process cursor-produced terminal response 可在不重建全 prefix 的情况下持有 private completed tool ownership。content/origin/model/route/config 不匹配拒绝；任意 ID 或 projection JSON 不能提供 ownership。

server cursor 和 private completed ownership 不授予 `NativeReplay` 或 native archive 权限。未指定 `responses_include` 保留 `reasoning.encrypted_content`；显式空 vector 发送 `[]`，其他显式选择也按指定处理。后续操作要求 native evidence 时，证据缺失会如实失败。

#### Explicit portable Gemini history

Generate 默认为 `NativeOnly`。显式 `PortableForeign` 仅允许不含 native seal、`wire_output`、signature/native metadata 的 caller-created assistant `Text`/`ToolCall`。只有第一个 foreign `functionCall` 带 Google `skip_thought_signature_validator`；text-only turn 不生成 signature。authentic native group 仍检查原 provenance/content/binding。损坏或不匹配 native group 不会降级为 portable，导入 foreign history 不获得 native replay 权限。

#### Output caps and native continuation

output generation cap 是每次调用获准的 resource，仅这个 cap 从 native replay config digest 排除。其他 binding 均保留，包括 content/prefix、origin/route、policy identity、tools、reasoning controls（文档规定的 per-turn tool selection/cursor 行为除外）。encoded/prepared request 仍保存 effective cap；request digest、journal slot、原 shared bank、绝对 deadline 仍有约束。增大 cap 的 semantic call 需要同一 grant 下的新 call ordinal 与 admission，不允许 seal repair、更新 deadline 或 replay 旧 effect。native archive 仍为 `spna3`/v3，portable JSON 仍为 v2。

Python 公开 `ChatReasoningOptions`, `ResponsesVerbosity`, `ResponsesTruncation`, `ResponsesInclude`, `MessagesThinkingMode`, `MessagesOutputEffort`, `MessagesCacheControl`, `MessagesCacheTtl`, `MessagesToolChoice`, `MessagesToolChoiceMode`, `GeminiHistoryMode`, `GeminiThinkingLevel`, `GeminiSafetySetting`, `GeminiSafetyCategory`, `GeminiSafetyThreshold`, `GeminiToolChoice`, `GeminiToolChoiceMode`。enum 值相同，唯 C++ `None` 使用 Python `None_`。optional class control 与 message/safety/history vector 返回 detached snapshot，修改后重新赋值。`ProviderControls.previous_response_history` 是 authentic `ProviderMessage` 列表，不是临时单个 response。

#### Deployment header preprocessing

普通 `sp::descriptor::load(source[, policy])` 将包括 `${VAR}` 的值按 literal 处理。显式 `load_with_environment_headers(source, overrides = {}, policy = {})` 读取 Messages 的 optional `ANTHROPIC_WORKSPACE_ID`/`ANTHROPIC_BETA`，unset/empty 省略。deterministic `load_with_deployment_headers(source, overrides, DeploymentHeaderEnvironment, policy)` 使用传入的 optional `anthropic_workspace_id`/`anthropic_beta`。两者返回 `LoadResult`，在 descriptor admission 前预处理。大小写不敏感优先级为 environment < descriptor literal headers < explicit overrides。重复 override、invalid/reserved name、换行在 admission 拒绝；批准后不再评估环境或修改 header。

Python 名为 `ProviderDeploymentHeaderEnvironment`, `load_provider_descriptor_with_environment_headers(source, overrides=[], policy=None)`, `load_provider_descriptor_with_deployment_headers(source, overrides, environment, policy=None)`。loader 返回 `ValidatedDescriptor`，admission 失败则抛异常。credential 保留在 runtime options。

#### Additional admission and event rules

Chat nested reasoning 必须非空，不能与 effective scalar `reasoning_effort` 并用。`effort`/`max_tokens` 互斥；budget 必须为正、在 signed 64-bit wire 范围内且不超过 effective output cap。`enabled=false` 禁止 effort/budget，`exclude=true` 与 `include_reasoning=true` 冲突。备用模型名须 nonempty、unique 且在获准 count limit 内；每个模型都检查 effective choices，包括 temperature 禁令。

policy identity 仍参与 native binding：公开内置 policy revision 4 拒绝旧 policy-3 seal/archive，不自动修复。Generate `safety_settings` 要求有效 category 不重复；nonempty allowed-function 列表仅适用于 `Any`/`Validated`。模型须为匹配两个获准 Generate descriptor path 的 literal name。portable tool result 须匹配获准 call identity；foreign assistant call 须为 client-executed，且没有 `wire_type`/`wire_metadata`。

semantic `Stop` 边界上 valid/invalid client-call intent 均把 `EndTurn` 改为 `ToolUse`，observer event 与 final outcome 一致。具体 `MaxTokens`, `ContentFilter`, `Unknown` 证据保留；已完成 server-executed hosted tool 本身不代表 client `ToolUse`。streaming OpenRouter reasoning fragment 按 index 合并、保持首次到达顺序；encrypted blob 仍为独立项。owned raw frame 与 native continuation 保留原内容及篡改检查。

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


这是源码和二进制破坏性变更；所有 C++ 使用者和自定义提供方都须使用匹配的新头文件/库重新编译。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、descriptor interpreter、Responses WebSocket 已删除，没有 alias 或兼容 bridge。SDK 为 alpha `0.3.0`、interface revision 6 / shared-library generation 6，采用 out-of-line capability check，不表示稳定发布。已验证的平台是 Linux/POSIX。Windows NTFS 与 macOS 实现已存在，但尚未在该 SDK 上验证；WASM provider runtime 支持尚未确立。

Python 公开与 C++ 相同的所有权 request/outcome 边界：`make_provider_request`、`Provider.prepare`、`dispatch` 和 `invoke`。Provider 历史使用带 typed part 的 `ProviderMessage`；`ChatMessage` 仍是图的便利 projection。SDK 失败可通过 `ProviderOutcome.failure` 读取，host observer/settlement 异常保留 `outcome` 和 `cause`。构造器及 GIL/回调行为参见 [Python binding 指南](python-binding.md)。

Python SDK 的 vector/map getter 返回独立值：`ProviderMessage.parts`、`ProviderRequest.messages`、`RunConfig.provider_messages`、completion/partial 的 `messages` 和 `raw_events`、`RunResult.native_messages`、`ProviderLoopEntry.messages`、usage 的 `extra`/`conflicts`。修改快照后，将它重新赋给具有 setter 的属性；向 getter 结果 append 不会更新所属对象。可选的 `ProviderControls.provider`/`response_format`、`SchemaProviderDefaults.provider`、`OpenRouterRouting.max_price` 和 `ProviderToolResult.host` 也遵循读取、修改、重新赋值的规则。只读 outcome 证据保持不变。这些是 Python 绑定规则，不是所有 C++ getter 都返回副本的保证。

原生代码调用 Python provider 的 `prepare` 重写时，返回 `None` 会在消费 handle 前引发 `TypeError`。`ProviderDescriptorPolicy.identity` 是包含 raw SHA-256 digest 的 `bytes`；`.hex()` 仅用于显示转换。反复检查已保存的 Python provider/graph cause 会保留原始异常值与 traceback，不消费已保存异常的 restore 状态；此规则也适用于嵌套原生异常转换。

<a id="pricing-evidence-and-the-price-catalog"></a>
### 计价证据与价格目录

Program 运行的 `monetary_microunits` 预算按 micro-USD 计数（1 microunit = 1e-6 USD）。当主机把内置 `llm_call` 节点的 provider 调用交给计价 broker 时，调用才会被计价：进程内使用 `bind_priced_provider_call(context)`，需要崩溃安全的重放时使用 `SQLiteProgramProviderCallJournal::bind_priced(context, deployment)`。运行时通过 `ProgramCoreProviderCallContext::monetary_account` 把 attempt 的金额账户交给每个 broker。Python 的 `LocalProgramHost(node_context=..., binding_label=...)` 总是为其调用计价（[Python 指南](python-binding.md#model-calling-nodes)）。与所有 `RunBudget` 字段一样，`monetary_microunits` 是授予运行的数量，`0` 表示什么也不授予：子运行与 fork 的预算从父运行的数量中划出，使用了模型 token 却以 `model_tokens = 0` 完成的运行会报告为 `BudgetExhausted`。`monetary_microunits = 0` 的运行会在发送前拒绝每一个被计价的调用。计价代码是内部的（`src/program/pricing.h`、`monetary.h`），不承诺 ABI。

分发之前，broker 为已准备的请求计价。origin 是 URL 与 descriptor base URL 匹配的目录 origin；若 SDK 策略把该 base URL 声明为 OpenRouter origin，则为 `openrouter`。hold 是请求的输入上限与输出上限对应的目录预留，乘以一次分发可能计费的模型调用次数（`Provider::invocation_upper_bound`）。输入上限是该模型已知的最小输入限制：SDK 策略的限制、目录的 `max_input_tokens` 或其上下文窗口。每个输入 token 按最贵的输入档计价，每个输出 token 按最贵的输出费率计价，aggregator 上使用 route ceiling。无法确定上限的调用，或 hold 超出剩余预算的调用，会在产生任何请求或 journal marker 之前被拒绝。节点以表示未发送的合成 provider 失败而失败，运行的 `ProgramFailure` 写明原因；`failure.witness["monetary"]` 包含 origin、model、上限与目录：

| 代码 | 状态 | 拒绝原因 |
|---|---|---|
| `P_MONEY_UNKNOWN_ORIGIN` | `Failed` | provider endpoint 不是目录 origin |
| `P_MONEY_UNKNOWN_MODEL` | `Failed` | 目录中没有该 origin 与 model 的条目 |
| `P_MONEY_UNMODELED` | `Failed` | 请求使用了目录不建模的内容：非标准 service tier、OpenRouter 模型回退（`models`）、Responses hosted tool 或 Anthropic 服务器工具 |
| `P_MONEY_NO_OUTPUT_CAP` | `Failed` | 请求没有输出 token 上限 |
| `P_MONEY_NO_INPUT_BOUND` | `Failed` | 不知道该模型的输入限制 |
| `P_MONEY_NO_UPPER_BOUND` | `Failed` | aggregator 条目没有 route ceiling，例如 router alias，或其 ceiling 无法作为 `provider.max_price` 固定 |
| `P_MONEY_BUDGET` | `BudgetExhausted` | hold 超出剩余的 `monetary_microunits` |

得到结果后，hold 恰好结算一次，attempt 的 `ProgramResult::monetary_records()`（Python 为 `result.monetary_records`）为每次调用增加一条 `ProgramMonetaryRecord`，字段为 `call_id`（journal 的逻辑调用 slot）、`operation_id`、`node`、`origin`、`model`、`evidence`、`microunits`、`reserved_microunits`、`basis`、`catalog_version`、`catalog_digest`、`conservative`、`may_undercount`、`replayed` 与 `reason`。`ProgramUsage::monetary_microunits` 是该 attempt 的收费与仍未结束的 hold 之和：

| 证据 | 条件 | 金额 |
|---|---|---|
| `reported` | 响应带有 provider 自己的费用：OpenRouter 的 `usage.cost`，由 SDK 解析为以 nano-USD 计的 `Usage::provider_cost`。BYOK 时加上报告的 upstream 费用。 | 报告金额向上取整到整数 microunit。 |
| `estimated` | 没有报告费用的最终且一致的 usage。 | 目录费率乘以报告的 token 档，每次调用向上取整一次。记录写明目录的 version 与 digest。 |
| `held` | 结果未知（transport 异常、取消、超时）、请求在失败前可能已发出、SDK 重发了 transport 尝试，或 usage 不完整或不一致。 | hold。 |

证明什么也没有发送的失败会释放 hold 且不留记录。超过 hold 的费用（当报告的费用包含固定上限不限制的部分时可能出现，例如 BYOK 的 upstream 费用）按报告收费，费用超出预算的运行以 `BudgetExhausted` 结束。并发调用（`ng.all`、`ng.race`、`ng.quorum`、并行节点）在同一个账户上预留，放不下的成员会被拒绝。节点重试在自己的 effect namespace（`task#a<k>`）下调用，因此每次尝试的调用都是新调用，会再次收费。

`bind_priced` 把调用的记录写入其 journal 行：随 dispatched marker 写入 held 记录，随结果写入结算后的记录。同一 Program attempt 在进程停止前记录的调用，会在恢复后的 attempt 绑定该 operation 时收费一次并标记为 `replayed`，无论 Core 是否再次调用它；这也包括通过 execution lease 过期而恢复的仅 define() 运行。较早 attempt 记录的调用已由该 attempt 的结果结算进剩余预算，后续 attempt 重放其结果时不会再次收费。recorded replay 从不花钱。

在 OpenRouter origin 上，broker 会在把请求准备为待发送之前，将 route ceiling 固定到请求上。OpenRouter 的 `provider.max_price` 以每百万 token 的精确十进制 USD 写入 ceiling 中最贵的输入费率作为 `prompt`、最贵的输出费率作为 `completion`；对于 `deepseek/deepseek-v4-flash-0731`，请求体带有 `"provider":{"max_price":{"prompt":0.44,"completion":1.32}}`。OpenRouter 只会路由到标价不高于这些价格的 endpoint，没有时请求失败，因此预留限定了 endpoint 的 token 价格。宿主已通过 `ProviderControls::provider` 或 `SchemaProvider::Defaults::provider` 发送的 `max_price`（请求自身的值取代默认值）在更低的字段上保留，目录不计价的金额（`image`、`audio`、`request`）保持宿主的值。被固定的是 Chat Completions、Responses 和 Messages 请求；其他 origin 不会带 `max_price`。计价使用把对话替换为一条用户消息的请求 probe，probe 不会发送。请求以固定后的形式只准备一次，因此 provider journal 记录的是实际发送请求的 digest，重放与之一致。在固定值下捕获了 native history 的后续轮次也带同样的固定值。如果 SDK policy 不允许在该 base URL 上使用 OpenRouter routing，或费率超过 `max_price` 允许的 2^53 nanoUSD，导致 route ceiling 无法固定，调用会以 `P_MONEY_NO_UPPER_BOUND` 被拒绝。

目录 [`data/pricing/catalog.json`](../data/pricing/catalog.json) 在构建时编译进 `neograph::program`，运行时只用这些字节计价。键为 origin 和 model。经由 Google AI Studio 的 Gemini 与经由 Vertex AI global endpoint 的 Gemini 是不同条目，OpenAI、Anthropic 和 OpenRouter 也同样分开。每个条目记录 `source_url`（官方价格页面或 OpenRouter models API）与 `retrieved` 日期。费率是以每百万 token 美元计的十进制字符串，涵盖输入、输出、缓存读取、缓存写入（Anthropic 的 5 分钟与 1 小时 TTL）和图像输出各档，并可选按提示长度分 tier。条目按标准 service tier 计价；batch、flex、priority tier，Anthropic 仅限美国的 inference_geo 与 fast mode，服务器工具费用和按请求收费均未建模。OpenRouter 把一个模型路由到价格不同的多个 endpoint，因此其条目带有 `route_ceiling`，即编写条目时最贵的 endpoint，预留使用它。像 `~deepseek/deepseek-v4-flash-latest` 这样的 router alias 不列出 endpoint，因此没有 ceiling，无法预留。没有条目的模型没有价格，对它的计价调用会被拒绝。

NeoGraph 运行时从不获取价格。[`scripts/check_price_drift.py`](../scripts/check_price_drift.py) 把目录与 OpenRouter 的 models 和 endpoints 列表、LiteLLM 社区价格文件（只是需要核对的信号，并非事实）以及在设置 `GCP_BILLING_API_KEY` 时 Cloud Billing Catalog API 中的 Vertex AI SKU 进行比较。它输出表格（`--json` 输出 JSON），漂移超过 `--tolerance` 时以 1 退出，且从不修改目录。每周的 `Price drift` 工作流运行它；OpenRouter 与 LiteLLM 不需要 secret。

组织费用报告（OpenAI 的 organization costs endpoint、Anthropic 的 cost report）需要管理员密钥，按时间段汇总且到达较晚，因此运行时不使用它们。主机可以事后用它们核对 `reported` 与 `estimated` 记录。

---

<a id="3-tool-interface"></a>
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

<a id="tool-failure-contract"></a>
### 工具失败契约

所有执行模型工具调用的路径都应用相同的规则：处理单个调用或 batch 的 `dispatch_tool_calls`、`tool_dispatch` 节点（包括 Program 的 Core `tool_dispatch`）、`llm::Agent::run` 与 `run_stream`、plan-and-execute 的 executor 以及 deep-research 的 researcher，C++、MCP 与 Python 工具均是如此。

| 失败 | 结果 |
|------|------|
| `HookBoundaryBlocked`：强制 Hook 失败、超时或无法运行，或工具代码抛出了它 | 运行以该异常停止，其 invocation id 与 state 保持不变；图运行也像 `llm::Agent` 一样直接抛出它，而不包装为 `NodeExecutionError`。节点重试策略不会重试它。 |
| 运行的 `CancelToken` 被取消（包括节点尝试的 token） | 运行以 `graph::CancelledException` 停止。进行中的 MCP 调用或 `GrpcRemoteTool` RPC 会立即停止，迟到的响应会被丢弃。 |
| 来自 `ToolGate` 或工具代码的 `NodeInterrupt` | 运行暂停；`llm::Agent` 将其抛给调用方。 |
| 未知工具，或 gate 的 `Deny` | 状态为 `rejected` 的工具结果。 |
| 调用将在运行截止时间（`RunContext::deadline`、`ToolExecutionContext::deadline`）当时或之后开始 | 状态为 `expired`、错误为 `run deadline elapsed before the tool started` 的工具结果。工具及其 Hook 都不会运行。 |
| 工具代码或其查找抛出的异常（C++、Python、MCP 的 `isError` 结果或 JSON-RPC 错误）、无法解析的参数，或超过策略 `output_limit_bytes` 的输出 | 状态为 `failed` 的工具结果；若调用已开始且其执行策略不能排除已产生效果（既非 `ReadOnly`、`ExternalRead`，也非 `Idempotent`），则为 `reconciliation_required`。 |
| 带类型的控制器状态，如 `timed_out`、`expired`、`killed` 或 `input_required` | 带有该状态的工具结果。因运行截止时间或请求超时而停止的 MCP 调用或 `GrpcRemoteTool` RPC 为 `timed_out`。 |
| 在 `ToolEffectBroker` 下效果不确定的工具结果 | 运行以 payload `kind` 为 `tool_effect_reconciliation` 的 `NodeInterrupt` 暂停。 |

前三行永远不会到达模型。工具结果会设置 `tool_status`，失败的结果无论是单个调用还是 batch 都带有 `{"error", "status", "retryable", "effect_uncertain"}`。在 batch 中，其他调用会先执行完毕；当多个调用都使运行停止时，取消优先于 Hook 阻断，Hook 阻断优先于中断。工具代码抛出的 `NodeInterrupt` 不会阻止其兄弟调用运行，恢复时整个 batch 会再次运行；若要在任何调用运行之前询问，请使用 `ToolGate`。运行截止时间（`RunMetadata::deadline`）约束 Hook、Provider 调用以及每个工具调用的开始，包括其在准入队列中的等待；工具自身的限制是其策略的 `execution_timeout` 与 `queue_timeout`。工具代码在开始之后抛出的系统错误是工具自身的失败（`failed` 或 `reconciliation_required`），而不是准入判定；主机准入与资源仲裁器一样，以 `rejected`（队列已满、没有容量）或 `expired`（队列等待耗尽）拒绝。从未开始的调用（`cancelled_before_start`、`expired`、`rejected`：`tool_call_not_started()`）没有产生任何效果：每个 `ToolEffectBroker`（包括持久的 `SQLiteToolEffectBroker`）都按原状态将其作为终态且效果确定的结果返回，并释放其槽位，因此它既不会为对账而暂停运行，也不会阻挡同一槽位之后的调用。

**工具 Hook。** 每条路径都在调用开始前发出一个 `BeforeToolExecution` 事件，在调用落定后发出一个 `AfterToolExecution` 事件，数据相同（`tool_call_id`、`tool_name`、`thread_id`、`request_id`，之前为 `arguments`，之后为 `status` 与 `result`），归属于运行的 owner scope 与 run id。`HookRuntime` 在直接分发时是 `ToolExecutionContext` 的；在 Agent 中是 `llm::Agent::set_hook_runtime` 或传给 `run` 的上下文的；在图运行中是引擎的（`EngineConfig::hook_runtime`、`GraphEngine::set_hook_runtime`），除非 `RunResources::hook_runtime` 为该次运行替换了它。当父运行与子图自身的引擎都有 `HookRuntime` 时，子图的工具调用会经过两者，先运行父运行的 Hook（`HookRuntime::compose`），任一方阻断都会阻断该调用；Program Core 的工具调用使用 Program 主机的（`RuntimeConfig::hook_runtime`）。当调用有任务槽位（`ToolExecutionContext::effect_task_id`，每次图运行都会设置）时，事件还携带 `task_id` 与 `call_ordinal`：这是调用的持久身份，与其效果身份相同（运行、线程、包含节点尝试的任务，如 `s3:act#a1`，以及调用在其 batch 中的序号），`request_id` 也由它派生。恢复或重放再次运行该 batch 时，已为该身份交付过的事件不会再次交付（`HookRuntime::emit_once_async`，基于 Hook 日志自身的记录）；强制 Hook 未通过（失败、超时或被取消，从而停止了运行）的那次发出不算交付，因此下一次发出会再次运行 Hook。被拒绝、未知或已过期的调用不启动任何东西，也不发出 Hook。最后一次尝试失败的强制 Hook 报告 `failed`；只有在截止时间已过或其目标超时时才报告 `timed_out`。超时的 Hook 目标是最终状态：即使还有剩余的 Hook 尝试次数或目标策略的 `retry_max_attempts`，运行器和工具执行控制器都不会再启动它，因为超时的目标可能仍在运行，第二次尝试可能与之并行运行。完全无法运行的 Hook 边界（没有可归属事件的 owner scope、无法使用的日志）对工具、检查点、Provider 与 Program 事件一律以 state `failed` 关闭式失败。Hook 运行期间若运行被取消，运行以取消停止。

**文本。** 跨越边界的文本都是格式正确的 UTF-8：每个格式错误的最大子序列都变为 U+FFFD，这是 Unicode 与 WHATWG 的替换做法。该规则适用于工具结果与失败消息、Hook 回执、`json::dump`（含有错误字符串的文档过去会被序列化为空字符串）、MCP 与 gRPC 响应，以及 Python 绑定。在 Python 中，从 C++ 读取的文本也以同样方式解码；`str` 转为 UTF-8 时，其中经过 surrogate-escape 的字节（U+DC80 到 U+DCFF，即 `errors="surrogateescape"` 产生的）还原为它们所代表的字节，其他孤立代理项变为 U+FFFD。

**抛出异常的门面。** `ToolExecutionController::execute_async` 成功时返回输出，对 `cancelled_before_start` 与 `cancellation_requested` 抛出 `graph::CancelledException`，对其他所有状态抛出携带完整 `ToolExecutionResult` 的 `ToolExecutionError`。节点失败分类器会读取它：效果不确定为 `NG.EffectUncertain`，超时或过期的调用为 `NG.Timeout`，其余为 Ordinary。

<a id="protocol-failure-mapping"></a>
### 协议失败映射

当 NeoGraph 通过 MCP、gRPC、A2A 或 ACP 提供工具、图或 agent 时，每个失败类别在传输上都保持各自不同且有文档说明的形式，并以节点失败分类器给出的稳定代码（`graph::classify_failure`）命名。运行控制信号永远不会成为模型可读的工具结果。只有公开消息会跨越传输：失败的 `public_message`（来自 `NodeFailureError`），或该类别的固定描述，绝不是可能携带路径、payload 或机密的异常文本。

| 类别（代码） | MCP `tools/call`（`MCPServer`） | gRPC `GraphService` | gRPC `ToolService.CallTool` | A2A 任务（`A2AServer`） | ACP `session/prompt`（`ACPServer`） |
|------|------|------|------|------|------|
| 客户端取消（`NG.Cancelled`）：`notifications/cancelled`、gRPC 取消 | 任何传输上都不应答：stdio（`run`）不发送任何内容；HTTP（`MCPHttpServer`）以空的 `text/event-stream` 结束 POST；读取应答 sink 的传输改为通过其 `NoResponseSink` 收到该 id | `CANCELLED` | `CANCELLED` | `CancelTask`：`CANCELED`；stream 也以此状态关闭 | `session/cancel`：`stopReason: "cancelled"` |
| 服务器取消（`NG.Cancelled`）：`stop()`，或处理器自身的工作 | JSON-RPC 错误 `-32000`（`stop()`）或 `-32603`，`data.code` | `CANCELLED` | `CANCELLED` | `CANCELED` | JSON-RPC 错误 `-32800`，`data` `{code, retryable}` |
| Hook 阻断（`NG.HookBlocked`） | JSON-RPC 错误 `-32010`，`data` `{code, invocation_id, state}` | `FAILED_PRECONDITION`，Hook 超时则为 `DEADLINE_EXCEEDED`；ErrorInfo metadata `invocation_id`、`state` | 与 `GraphService` 相同 | 没有 node 完成时为 `REJECTED`，否则为 `FAILED`；`metadata["neograph/failure"]` `{code, retryable}` | JSON-RPC 错误 `-32603`，`data` `{code, retryable}` |
| 中断（`NG.Interrupted`） | JSON-RPC 错误 `-32011`，`data` `{code, reason, value}` | 已暂停的运行：`OK`、`interrupted`、`interrupt_node`、`interrupt_value_json`（流的 FINAL payload：`interrupt_value`） | `FAILED_PRECONDITION`，ErrorInfo metadata `reason`、`value` | `INPUT_REQUIRED`：原因作为 text，payload 作为 data part，`metadata["neograph/interrupt"]` `{node, checkpointId}`；后续 message 恢复运行 | 同一轮次内的 `session/request_permission`，其回答恢复运行；适配器或客户端无法回答时为带 `NG.Interrupted` 的 JSON-RPC 错误 `-32603` |
| 普通（`NG.TaskFailed`、作者代码） | 带有 `isError: true` 与 `_meta["io.neograph/failure"]` `{code, retryable}` 的 `result` | `UNKNOWN` | 带有 `error` 与 `error_code` 的 `OK` | `FAILED`，公开消息作为 status message，`metadata["neograph/failure"]` `{code, retryable}` | JSON-RPC 错误 `-32603`，公开消息作为 message，`data` `{code, retryable}` |
| 超时（`NG.Timeout`） | 与普通相同，带有各自的代码 | `DEADLINE_EXCEEDED` | 与普通相同，带有各自的代码 | 与普通相同，带有各自的代码 | 与普通相同，带有各自的代码 |
| Provider 失败（`NG.ProviderFailed`） | 与普通相同，带有各自的代码 | `UNAVAILABLE` | 与普通相同，带有各自的代码 | 与普通相同，带有各自的代码 | 与普通相同，带有各自的代码 |
| 预算（`NG.BudgetExhausted`） | 与普通相同，带有各自的代码 | `RESOURCE_EXHAUSTED` | 与普通相同，带有各自的代码 | 与普通相同，带有各自的代码 | 超出运行的模型 token 预算时为 `stopReason: "max_tokens"`，否则与普通相同 |
| 安全或配置（`NG.SafetyBlocked`、`NG.ConfigError`） | 与普通相同，带有各自的代码 | `FAILED_PRECONDITION` | 与普通相同，带有各自的代码 | 与普通相同，带有各自的代码 | `NG.SafetyBlocked` 为 `stopReason: "refusal"`，`NG.ConfigError` 与普通相同 |
| 逻辑（`NG.LogicError`、`NG.BarrierStalled`） | 与普通相同，带有各自的代码 | `INTERNAL` | 与普通相同，带有各自的代码 | 与普通相同，带有各自的代码 | 与普通相同，带有各自的代码 |
| 已提交的效果（`NG.EffectCommitted`） | 与普通相同，带有各自的代码 | `ABORTED` | 与普通相同，带有各自的代码 | 与普通相同，带有各自的代码 | 与普通相同，带有各自的代码 |
| 不确定的效果、未知、失败出口（`NG.EffectUncertain`、`NG.UnknownFailure`、出口的代码） | 与普通相同，带有各自的代码 | `UNKNOWN` | 与普通相同，带有各自的代码 | 与普通相同，带有各自的代码 | 与普通相同，带有各自的代码 |
| 步数上限（`NG.StepLimit`）：`RunConfig::max_steps` | 无 | `OK`，`max_steps_exhausted` | 无 | `FAILED`，代码 `NG.StepLimit` | `stopReason: "max_turn_requests"` |
| 无效请求（`NG.InvalidRequest`） | JSON-RPC 错误 `-32602` | `INVALID_ARGUMENT`；没有可运行的图时为 `FAILED_PRECONDITION`（`NG.NoGraph`） | `INVALID_ARGUMENT` | 带 `google.rpc.BadRequest` field violation 的 JSON-RPC 错误 `-32602`；格式错误的 envelope 或 batch 为 `-32600` | JSON-RPC 错误 `-32602` |
| 未知工具（`NG.ToolNotFound`） | JSON-RPC 错误 `-32602`，`data.code` | 无 | `NOT_FOUND` | 无 | 无 |
| 容量已满（`NG.Capacity`） | JSON-RPC 错误 `-32000`，`data.capacity` | `RESOURCE_EXHAUSTED` | 无 | JSON-RPC 错误 `-32603`，ErrorInfo reason `RESOURCE_EXHAUSTED`，domain `neograph` | JSON-RPC 错误 `-32603`，`data.reason` `overloaded` |

`retryable` 在每个协议上都是 `FailureVerdict::retryable`：再次发送同一请求是否可能成功。它是节点的重试判定（[节点失败契约](#node-failure-contract)），对 provider 失败（`NG.ProviderFailed`）也为 `true`：节点重试循环把它交给 provider runtime，但新的请求可能会成功。A2A 在两个版本中都把代码和 `retryable` 放在 task 的 `metadata["neograph/failure"]` 中，ACP 放在错误的 `data` 中；只有 A2A 才有的请求错误见 [A2A 错误映射](#a2a-error-mapping)。

每个非 OK 的 gRPC 状态都在 `grpc-status-details-bin` trailer 中携带一个 `google.rpc.Status`，其中包含一个 `google.rpc.ErrorInfo`：其 `reason` 遵循 AIP-193，是由代码派生的不超过 63 个字符的 UPPER_SNAKE 常量（`NG.HookBlocked` 为 `HOOK_BLOCKED`，此外还有 `TIMEOUT`、`TOOL_NOT_FOUND`、`INVALID_REQUEST`、`NO_GRAPH`、`CAPACITY`、`BACKEND_UNAVAILABLE`、`BARRIER_STALLED`；作者代码取其类别的 reason，如 `TASK_FAILED`）；`domain` 为 `neograph`；`metadata` 包含 `code`（稳定代码本身）、`retryable` 以及上表中的字段。`RunGraphResponse.error` 已弃用，仅携带公开消息并保留一个版本；非 OK 的一元状态不携带响应体。`RunGraphStream` 以 payload 为 `{error, code}` 的 `DEBUG` 事件报告失败，然后以与一元调用相同的状态结束。`CheckpointService` 对后端失败返回 `UNAVAILABLE`（`NG.BackendUnavailable`），而不是“没有检查点”、空列表或成功的删除；对格式错误的 `SaveCheckpoint` 返回 `INVALID_ARGUMENT`。

NeoGraph 自己的客户端会约束其调用：MCP 客户端对因取消或超时而不再等待的请求发送 `notifications/cancelled`（`initialize` 除外），并丢弃迟到的响应；`GrpcRemoteTool`（`set_call_timeout`，默认 60 秒）与 `GrpcCheckpointStore`（`set_rpc_timeout`，默认 30 秒）为每次调用设置截止时间，运行的 cancel token 会取消远程工具的 RPC。这些客户端不会把远程代码还原为本地控制信号。远程 Hook 阻断（MCP `-32010`；`ErrorInfo` 代码为 `NG.HookBlocked` 的 gRPC 状态）与远程中断（`-32011`；`NG.Interrupted`）属于远程运行：`MCPTool` 与 `GrpcRemoteTool` 抛出 `graph::RemoteControlFailure`，这是一个携带该代码与远程公开消息、不会重试的普通失败，因此本地运行会带着失败的工具结果继续。来自远程服务器的其他 MCP JSON-RPC 错误、gRPC 工具错误或状态对调用方而言是普通工具失败。TODO（后续阶段）：将远程中断作为由人回答的原生暂停传入本地运行。

<a id="node-failure-contract"></a>
### 节点失败契约

节点抛出异常时，由同一个分类器决定其重试策略能否再次运行它，以及该失败能否路由到失败处理节点。分类器会遍历整条原因链（`NodeExecutionError` 的原因、`std::nested_exception` 以及该节点所运行子图的失败），在链上任意位置找到的最严重分类即为结果。上面的工具失败契约使用同一个分类器：工具原因链中任意位置出现运行控制信号都会停止运行，例如工具所运行的图的失败中包含的 Hook 阻断。

| 分类（由轻到重） | 代码 | 来源 | 重试 | 可路由 |
|------|------|------|------|------|
| Ordinary | `NG.TaskFailed`、`NG.SubgraphFailed` 或作者代码 | `std::runtime_error` 及其子类、`NodeFailureError`、Python 异常、根因为 Ordinary 的子图失败 | 是，作者标记为不可重试时除外 | 是 |
| Timeout | `NG.Timeout` | 超过 `attempt_timeout_ms`、被取消并已停止的尝试（`NodeAttemptTimeout`）；超时的操作（`timed_out` 的 asio 或 `std::system_error`）；超时或过期的工具调用（`ToolExecutionError`） | 是 | 是 |
| ProviderFailed | `NG.ProviderFailed` | 暂时性的 `ProviderFailure`：传输、响应被截断、截止时间、过载、限流，或 HTTP 5xx 状态的远程失败 | 否，Provider 重试由 Provider 运行时负责 | 是 |
| EffectCommitted | `NG.EffectCommitted` | 该尝试提交了非幂等的 Tool 效果 | 从不 | 是，处理节点可以进行补偿 |
| LogicError | `NG.LogicError` | `std::logic_error`，包括完整性检查 | 否 | 否 |
| ConfigError | `NG.ConfigError` | Provider 的配置、请求、认证、权限、not-found 或误用失败；同时继承 `ConfigurationFailure` 的异常，例如 Program 诊断（C++ 或 Python 的 `ProgramDiagnosticError`、编译或准入错误） | 否 | 否 |
| SafetyBlocked | `NG.SafetyBlocked` | 因内容过滤或拒绝而停止的 Provider 失败 | 否 | 否 |
| BudgetExhausted | `NG.BudgetExhausted` | 预算或配额耗尽 | 否 | 否 |
| EffectUncertain | `NG.EffectUncertain` | `ProviderOutcomeError`（包括结果未知的 Provider 分发）、结果未知的 Tool 效果（包括 `ToolExecutionError`），或在停止截止时间前未停止的超时尝试 | 否 | 否 |
| UnknownFailure | `NG.UnknownFailure` | `std::bad_alloc` 或其他任何异常类型 | 否 | 否 |
| 运行控制 | `NG.Interrupted`、`NG.HookBlocked`、`NG.Cancelled` | `NodeInterrupt`、`HookBoundaryBlocked`、取消 | 否 | 否，运行暂停或停止。无论位于原因链何处，Hook 阻断或取消都会像在每条工具路径上一样，不产生 `ERROR` 事件，以 `HookBoundaryBlocked` 或 `CancelledException` 本身离开图运行 |

`graph::classify_failure(error, run_token)` 对任意异常返回这一判定（对带有 `NodeFailure` 记录的 `NodeExecutionError` 则返回该记录的判定）：其类别、代码、`public_message`、能否重试或路由，以及对运行控制类别而言的信号本身（`FailureVerdict::signal`）。协议主机会在传输上报告该代码（[协议失败映射](#protocol-failure-mapping)）。

`NG.` 前缀为保留前缀。作者代码须匹配 `[a-z][a-z0-9_]*(\.[a-z0-9_]+)+`。C++ 节点抛出 `NodeFailureError(code, public_message, retryable)`。Python 节点抛出任意带有字符串 `code` 属性的异常，并可附带布尔值 `retryable` 与字符串 `public_message`；调用方收到的仍是原始异常。执行器放弃一个节点时，它抛出的 `NodeExecutionError` 带有一条 `NodeFailure` 记录，可通过 `node_failure_of()` 取得。记录包含代码、分类、停止重试的原因（`exhausted`、`non_retryable`、`effect_committed`，达到运行重试上限时为 `ceiling`，下一次尝试之前的退避会越过运行截止时间时为 `deadline`，节点激活超过 `max_visits` 时为 `visit_limit`）、尝试次数、可重试与可路由标志、`public_message`，以及最后一次尝试的效果（形如 `{tool, ordinal, status}`）。它从不复制 `what()`。节点 `non_retryable` 中列出的代码不会重试。

**可路由的失败可以被处理，而不是使运行失败。** 当节点有 failure edge（`failure_edges`，见 [Program 图功能](program-graphs.md#failure-routing)）时，执行器不抛出任何异常：失败的尝试不写入任何内容，一条结构化记录进入该边的 `error_channel`，节点只路由到该边的目标。记录恰好包含 `node`、`kind`、`code`、`attempts`、`retryable`、`step`、`routed_to` 和 `effects`；只有当节点通过 `error_text` 启用、且宿主的 `EngineConfig::failure_text_redactor` 从 `public_message` 中保留了文本时，才会加上 `detail.untrusted_text`。该节点在执行轨迹和 `interruptAfter` 中计为已运行，其待写记录保存已处理的结果，因此恢复会重放路由而不会再次运行该节点；其 `ERROR` 事件带有 `handled: true` 和 `routed_to`。静态节点、扇出分支、`Send` 目标和子图节点，无论经由同步还是异步入口，都走这条路径。不可路由的失败，或没有 failure edge 的节点的失败，仍按上文抛出。

**每次尝试都有自己的效果命名空间。** 尝试 0 使用任务 id（`s3:act`），因此现有日志的槽位保持不变。尝试 k 使用 `s3:act#a<k>`，调用序号在其中重新开始。Tool 效果、Provider 调用和子图线程都跟随尝试，因此重试绝不会重放或冲突于先前尝试的效果；`ProviderCallBroker` 在每次重试时看到新的调用槽位。尝试 k 开始之前，执行器会以该尝试的命名空间为键记录一条空的待写记录；检查点存储无需更改模式。崩溃或中断之后的恢复会在其命名空间中继续最近一次已开始的尝试，因此 `ToolEffectBroker` 会重放该尝试已提交的内容，剩余的重试次数也得以保留。在 `ToolEffectBroker` 下，提交了非幂等效果（以 `ReadOnly`、`ExternalRead` 以外且非 `Idempotent` 的策略成功的调用）的尝试绝不会被重试；结果未知的效果会以 `tool_effect_reconciliation` 中断暂停运行，即使节点捕获了该中断并抛出了其他错误也是如此。节点所运行子图的效果计入该节点的尝试。

**尝试超时与运行的截止时间。** 节点级策略中有 `attempt_timeout_ms` 时（或有只能缩短定义值的宿主超时 `GraphEngine::set_node_attempt_timeout_ms` 时），节点的每次尝试都在驱动运行的协程之外运行，超时一到就通过它自己的子 `CancelToken` 被取消。第一个终止事件获胜：在超时时刻或之后的完成就是超时。超时尝试的迟到完成会被丢弃：没有写入、没有待写记录、超时后没有事件，它发起的经中介的 Tool 调用会被拒绝。尝试在停止之后以 `NG.Timeout` 失败，下一次尝试此时才开始。在停止截止时间（再一个超时时长，至少一秒）内未停止的尝试以 `NG.EffectUncertain` 失败，因此运行失败；它在后台继续并保持其节点存活，`wait_for_node_attempts()` 会等待它。当下一次尝试之前的退避会越过 `RunContext::deadline`（Program 的 wall-time 预算）时，节点以 kind `deadline` 停止重试并保留失败的代码。在 Program 运行中，所有节点主体都以这种方式在 Program 调度器之外运行。参见 [Program 图功能](program-graphs.md#attempt-timeouts-and-the-deadline)。

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
    ChannelLifecyclePolicy lifecycle;
    json        value;                             // Current value
    uint64_t    version = 0;                       // Write counter
};
```

### ChannelWrite

针对命名通道的单次写入操作。节点返回这些的向量。

```cpp
struct ChannelWrite {
    enum class Mode { Reduce, Overwrite };
    std::string channel;
    json value;
    Mode mode = Mode::Reduce;
    std::shared_ptr<const std::vector<sp::Message>> native_messages;
};
```

`ChannelLifecyclePolicy` 区分 retention（`Unbounded`、`Latest`、`Bounded` 及 `retention_limit`）和 persistence（`Checkpoint`、`Ephemeral`）。`ChannelWrite::Mode::Overwrite` 绕过 reducer 后应用 retention。保留真实 SDK 历史应使用 `provider_messages_write(messages_or_outcome)`；JSON-only 写入不能创建 native replay 权限。Resume guard 与合并顺序参见 [channel 生命周期](concepts.md#channel-lifecycle-and-checkpoint-contract)。

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

哪些失败会被重试，以及各次尝试的效果如何彼此隔离，见节点失败契约（第 3 节）。

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
    ToolSet                  tools;      // Owned fixed collection of available tools
    std::string               model;      // Explicit model name; no provider default
    std::string               instructions; // System prompt / instructions
    json                      extra_config; // Additional configuration (node-type-specific)
};
```

将 `ToolSet(std::move(tools))` 赋给 `NodeContext::tools`，或者在上下文工具为空时
通过 `EngineResources::tools` 提供。编译结果及引擎共同持有同一批工具；
重新赋值上下文不会改变已有引擎。工厂可用 `ctx.tools.view()` 临时查找指针。
Python 和 MCP 工具遵守相同的编译期所有权规则。

Python 的 `NodeContext(provider=...)` 及其 `provider` setter 将原始 Python
provider 对象保存在每个上下文的生命周期所有者 lease 中，并将 lease 连接到真实的
原生共享指针。编译后的节点和引擎持有的原生上下文副本会保留同一个 Python
override 所有者，即使上下文被重新赋值或 Python wrapper 被回收。
重新赋值只释放可变上下文的 lease；已有编译快照仍保留各自的副本。
Lease 的最终 deleter 会取得 GIL。单独的借用 C++ 引用或 raw pointer
不会保留 Python override 所有者。

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

| 方法 | 描述 |
|--------|-------------|
| `init_channel(name, type, reducer, initial_value)` | 注册一个通道及其 reducer 和可选的初始值。必须在对该通道进行任何读/写之前调用 |
| `get(channel)` | 读取通道的当前值。线程安全（共享锁） |
| `get_messages()` | 便利方法：读取 `"messages"` 通道并将其反序列化为 `std::vector<ChatMessage>` |
| `write(channel, value)` | 通过其 reducer 向单个通道写入值。线程安全（独占锁） |
| `apply_writes(writes)` | 原子地应用批量 `ChannelWrite` 操作。所有写入在单个独占锁下应用 |
| `channel_version(channel)` | 返回特定通道的写入计数 |
| `global_version()` | 返回全局版本计数（每次向任何通道写入时递增） |
| `serialize()` | 将 checkpoint-persistent 通道值与 version 序列化为 JSON |
| `restore(data)` | 从序列化的 JSON 恢复通道值和版本 |
| `channel_names()` | 返回所有已初始化通道的名称 |

`serialize()` 只包含 checkpoint-persistent channel 的值和 version，省略 ephemeral 值。`restore_checkpoint` 要求匹配的 guard，已写入 ephemeral 状态丢失时会拒绝恢复。`serialize_runtime` / `restore_runtime` 保留同 process 副本的 live ephemeral 值，不用于持久存储。`checkpoint_snapshot()` 将 portable snapshot 与真实 C++ native sidecar 配对。`get_messages()` 是便利 projection；完整 SDK 历史使用 `get_provider_messages()`，不应把任意 JSON 解释为 chat 时使用 `captured_provider_messages()`。

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
| `in.ctx.cancel_token` | 在 `provider.invoke(std::move(request))` 前赋值 `request.cancel_token = in.ctx.cancel_token`。Provider 取消在支持的边界协作处理。自己的循环应检查非 null 的 `ctx.cancel_token`，调用 `ctx.cancel_token->is_cancelled()` |
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
                 std::map<std::string, std::string> output_map = {},
                 SubgraphPersistence persistence = SubgraphPersistence::Legacy);
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
| `persistence` | `SubgraphPersistence` | `Legacy`（兼容）、`PerInvocation`、`PerThread` 或 `Stateless`；JSON 拓扑节点使用小写的 `persistence` 字符串 |

如果映射为空，通道按名称映射（恒等映射）。

输入映射会把父图当前的通道值复制到子图输入。输出映射有意采用不同语义：它不会把
子图最终序列化状态当作新的 reducer 输入，而是按生成顺序转发子图的
`ChannelWrite` delta，并保留每个 write 的 `Mode`。因此，继承的 append/custom
值不会被重复应用。输出映射不会推断 snapshot replacement；若要替换映射后的父值，
子图必须显式发出 `ChannelWrite::Mode::Overwrite`。

#### 子图持久化与检查

| 模式 | 子图 checkpoint namespace | 开始/resume | Store 优先级 |
|------|----------------------------|--------------|------------------|
| `Legacy` (默认) | 长度分隔的 parent thread、node、parent step、task ID (`subgraph/...`) | 新 parent 启动新 child；parent resume 加载对应 snapshot | 若 parent run 有 checkpoint backend 则使用，否则使用 child 配置 |
| `PerInvocation` | `subgraph/run/` + parent thread、node、持久 parent graph-invocation UUID、step、task ID | 每次新 parent run 使用新 namespace；resume 恢复 UUID 及 child write journal | Parent，然后 child |
| `PerThread` | `subgraph/thread/` + parent thread、node | 新调用以旧 checkpoint seed child state 后应用新 input；parent resume 使用对应 snapshot；同 compiled node/namespace 的重叠调用报错 | Parent，然后 child |
| `Stateless` | 无 | 禁用 child checkpoint；拒绝 interrupt/resume，但传播 Store、取消和 ToolGate | 无 checkpoint backend；parent Store，然后 child Store |

显式 stateful 模式要求非空 parent thread ID。`Legacy` 保留 #238 之前的 namespace、checkpoint wire format 及 empty-thread 行为。`PerInvocation` 在 parent metadata 中记录 `_neograph.subgraph_invocation_id`；不含该值的旧 checkpoint 在切换策略后不能 resume。`PerThread` 共享 namespace。不同 engine/process 使用同一 backend 时，host 也必须协调 admission；node-local guard 仅保护一个 compiled node。不要在没有显式 migration 时改变现有 thread 的策略。

使用 `GraphEngine::inspect_nested_checkpoint(root_thread, path[, run_store])` 检查子图和孙图 checkpoint。每个 `SubgraphPathStep` 指定 child node name、parent super-step、stable Core task ID（`s0:child` 或 Send task ID）及可选的 exact parent checkpoint ID。结果包含 `graph_path`、child `thread_id` 和完整 `Checkpoint`（含 channel 值及 checkpoint ID）。其他 thread 的 checkpoint ID 或 stateless path 会被拒绝。若 `RunResources` 覆盖了 backend，需传入同一 run-scoped store。`SubgraphNode::checkpoint_thread_id()` 重建 namespace 的一段。

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

| 字段 | 类型 | 默认值 | 描述 |
|-------|------|---------|-------------|
| `thread_id` | `std::string` | `""` | 标识对话/会话以用于检查点 |
| `input` | `json` | `{}` | 在执行开始前写入通道的初始值。通常是 `{"messages": [...]}` |
| `max_steps` | `int` | `50` | 强制终止前的最大超级步骤数（防止无限循环） |
| `stream_mode` | `StreamMode` | `ALL` | 控制在流式执行期间发出哪些事件类型的位标记 |
| `cancel_token` | `std::shared_ptr<CancelToken>` | `nullptr` | 协作式取消句柄。引擎将其包装到 `RunContext` 中，并通过 `in.ctx.cancel_token` 传递给每个节点的 `run(NodeInput)` 调用 |
| `usage` | `std::shared_ptr<UsageAccumulator>` | `nullptr` | 可选的 token 累加器。当省略时引擎创建一个，并将活跃累加器暴露为 `in.ctx.usage` |
| `resume_if_exists` | `bool` | `false` | 如果为 `true` 且 `thread_id` 存在检查点，在应用 `input` 之前从该检查点播种（多轮聊天形态） |

### RunContext（v0.4，通过 `NodeInput.ctx` 暴露给节点）

引擎传递的每次运行分发元数据。最初由 `RunConfig`（未提供 usage 累加器时
创建一个）、`RunMetadata`、有效 Store 和可选 resume value 构造。节点在
`run(NodeInput) -> NodeOutput` 重写中通过 `in.ctx` 使用它。

这里的创建仅描述初始构造。检查点恢复可以用真实的原始 bank 及其先前报告替换该累加器。因此，resume 或 continuation 不保证返回全新的用量报告，也不保证先前报告的用量变为 `None`。

Python `RunMetadata(timeout_ms=None, ...)` 默认没有 deadline。构造器和 `set_timeout_ms(timeout)` 接受剩余 steady-clock 范围内的非负整数毫秒，在 signed 转换或加法前检查范围。负数或超出范围的值会引发 `OverflowError` 或 `ValueError`；setter 失败时保留原有绝对 deadline。0 立即过期；`clear_deadline()` 移除 deadline。

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
| `fork()` | **v0.4。** 返回子 shared_ptr。Parent.cancel() 级联；如果父令牌在 fork() 时已取消，子令牌被构造为预取消状态（无 emit-vs-bind 竞态） |
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
    bool barrier_stalled() const noexcept;        // A barrier can no longer fire
    bool failure_exited() const noexcept;         // A failure-exit node completed (#364)
    RunStatus status() const noexcept;            // Completed, Interrupted, StepLimit, SafePoint, BarrierStalled or FailureExit

    template <typename T> T channel(const std::string& name) const;
    template <typename T> T channel(const ChannelKey<T>& key) const;
    template <typename T>
    std::optional<T> try_channel(const ChannelKey<T>& key) const;
};
```

`RunResult::usage` 是 nullable provider 报告，不是支出 bank。`native_messages` 保留真实 typed 历史，`provider_outcomes` 保留每个拥有所有权的 Completion/Failure。JSON `output` 仅是 portable projection。完整历史输入用 `RunConfig::provider_messages`，typed event 观察用 `on_provider_event`。持久 native checkpoint/receipt custody 必须使用 `native_history_archive`，内存 sidecar 无需 archive。

resume 或 continuation 时，`provider_outcomes` 保留原始结果，再依次追加新产生的结果，形成有序列表。`usage` 也可能保留恢复 bank 中的先前报告。调用方必须维持已完成 provider effect 不被重新 dispatch、不被重复计费的不变量；结果列表为空或重置、`usage=None` 均不是 resume 不变量。

`provider_messages` 提供完整 typed 历史，仅替换 messages channel；`on_provider_event` 观察 typed SDK event。`provider_outcomes`、`provider_loop_history` 在 inner turn 间保留结果及 task-local continuation。`native_history_archive` 绑定持久 native custody，不授予模型预算。可选 `model_token_budget` 上限及 `budget_exhausted` 信号用于预算感知 dispatch。Python 与 C++ 都以 `RunResult.native_messages` 返回完整历史，以 `provider_outcomes` 返回拥有所有权的结果。输入仍为 `RunConfig.provider_messages`；没有 `RunResult.provider_messages` 或 `provider_history` alias。
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

`status()` 返回 `RunStatus::Completed`、`RunStatus::Interrupted`、
`RunStatus::StepLimit`、`RunStatus::SafePoint`、`RunStatus::BarrierStalled` 或 `RunStatus::FailureExit`，而不更改公开 `RunResult` 的数据布局。
`BarrierStalled` 表示没有可运行的节点、最后一个 super-step 没有显式路由到 `__end__`，而 barrier 仍持有信号，因此 join 再也无法触发。此时 `barrier_stalled()` 为 true，`output["_neograph"]["barrier_stalled"]` 以 `{node, wait_for, received, missing}` 列出每个停滞的 join。
`FailureExit`（#364）表示 failure exit 节点已完成，运行以失败结束：`failure_exited()` 为 true，`output["_neograph"]["failure_exit"]` 为 `{node, code}`。`RunInvocation` 将其报告为 `Error`。
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

返回线程的检查点历史，最后保存的排在最前；第一项就是 `get_state` 读取的检查点。

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

Fork 复制状态及选定 checkpoint 的 pending continuation，不创建新 turn。resume 已完成 `__end__` continuation 不执行任何 node，因此只编辑问题不会产生回答。继续 pending 工作需选择具有真实 `next_nodes` 的 exact paused checkpoint ID，fork 后修改 portable state，再 resume。不要将所有历史 empty-`next_nodes` latest-resume snapshot 都等同于 terminal sentinel。example 08 保留 terminal fork 后的单独 new-turn 流程。

authentic native state 保留原 shared-bank scope。Fork 不复制 spending grant；managed-bank custody/source commitment/原 ceiling/deadline 继续适用。durable standalone fork 不许可 import 或更新 native 权限。

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
- `run_sends_async`：无论一个还是多个 Send，都为每个目标提供隔离的状态副本
  （init + restore + apply input）并带重试运行，因此 payload 只对其自身的
  Send 可见（#389）。
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
| `schema_version` | `std::uint32_t` | 线路布局版本（见 `CHECKPOINT_SCHEMA_VERSION`，当前为 `3`）。它是定宽无符号类型，因为 schema version 非负，并且要持久化到磁盘、通过 JSON 往返。持久的 `CheckpointStore` 实现应序列化它，并将反序列化 blob 上的 `0` 视为"pre-versioned"（例如字段缺失 — 迁移是调用者的责任） |

### CheckpointStore

保留 ABI 兼容性的旧持久化接口。新的仅同步后端继承 `CheckpointStoreCore`
（五个纯同步操作），并调用 `adapt_checkpoint_store()`；原生异步后端继承
`AsyncCheckpointStore`（五个纯协程操作），并调用 `adapt_async_checkpoint_store()`。
两个适配器都向现有 GraphEngine、协议宿主、gRPC 检查点用户及 Python 绑定入口
公开 `CheckpointStore`。异步引擎操作调用规范的异步接口；仅同步后端的操作
卸载到有界线程池，而原生异步操作在调用方的执行器上运行。旧同步默认实现
在缺少能力时抛出异常，不会递归调用。持久化 pending write 是独立的可选
`PendingWritesCheckpointStore` 能力；没有它时，恢复会重放整个超步。
持久化 schema 不变。参见 [`ASYNC_GUIDE.md` §9.4](ASYNC_GUIDE.md#94-checkpointstore)。

Python 的 `CheckpointStore.requires_managed_budget(thread_id) -> bool` 是读取
已持久化 managed-bank 拒绝义务的同步虚方法。引擎的原生
`requires_managed_budget_async(thread_id)` facade 将同步调用卸载到 worker，
绑定取得 GIL 后调用 Python override。没有 override 时，原生实现会明确
报告 backend 不支持的错误，而不会返回 `False`。Reader 报告受信任的
namespace/thread 是否曾持有活动的 standalone managed bank；保存移除 bank 后的
状态或删除 checkpoint 都不得清除该义务。实现必须如实报告此义务。
此 reader 不授予支出、恢复、bank 或 lease 权限；有限预算执行仍需要
实际支持的原生 managed-budget lease。

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

Python 图 tracing 位于 `neograph_engine.tracing`、`neograph_engine.openinference`。`otel_tracer` 从 graph event 生成 run/node span；`openinference_tracer` 记录 `CHAIN` 标签及 node payload projection。`neograph_engine.openinference.OpenInferenceProvider` 用 native C++ dispatch observer 包装现有 typed provider，并向 Python OpenTelemetry tracer 发送每次调用的 `LLM` span。C++ 使用 `<neograph/observability/openinference.h>`。

### `otel_tracer` — OTel 形态 span

以下 signature 是参考声明。默认值为 `root_name=graph.run`、`node_span_prefix=node.`、`attribute_prefix=neograph`；`on_event` 可将 graph event 转发给其他 consumer。

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

`NODE_START` 打开 span；`NODE_END` 成功关闭；`ERROR` 记录错误；`INTERRUPT` 标记 pause。每个 node name 有重叠 event 的 stack；run 退出时 context manager 关闭剩余 span。Trace 不能证明 exactly-once node 执行或唯一关联每个并发 task。

```python
from opentelemetry import trace
from neograph_engine.tracing import otel_tracer

tracer = trace.get_tracer("my-service")
with otel_tracer(tracer) as cb:
    engine.run_stream(cfg, cb)
```

### `openinference_tracer` — LLM 形态属性

Graph span 带 `openinference.span.kind = "CHAIN"`；node input/output payload 成为 JSON `input.value` / `output.value` projection。仅 Python graph tracing 不创建每次 provider 调用的 `LLM` span，也不归因 vendor charge。Context attachment 限于原始 Python context；跨 thread/task 的 parent 传播需要 tracing integration 保留 context。

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

### `OpenInferenceProvider` — Python 与 C++ typed dispatch 观察者

Python 使用 `OpenInferenceProvider(inner, tracer, *, span_name="llm.complete")` 构造。它继承 `Provider` 的 `prepare(request)`、一次性 `dispatch(prepared)` 和 `invoke(request)`，不添加 completion API。Native wrapper 只委托一次准备，并在 dispatch 时观察同一 owned handle。无效或弃用的准备不打开 span。正常 tracing 下，每次已准入 dispatch 打开一个 LLM span；原 outcome、mode、deadline、取消、event 及 provider identity 原样传递。Tracing 失败不替换 provider outcome 或异常。

以下函数展示两种调用路径。`inner` 是已配置的 `SchemaProvider` 等现有 typed provider；`model` 必须明确。一次模型调用选择其中一个函数。

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

Python `invoke`/`dispatch` 释放 GIL；tracer adapter 在 Python 调用与引用销毁时重新获取 GIL。Prepared operation 保留 adapter 和 tracer，即使 dispatch 前 wrapper 已被回收也仍有效。Parent 取自 dispatch 时而非 prepare 时的 active OpenTelemetry context；跨 thread/task 移动任务时需传播 context。构造 wrapper 前安装 `opentelemetry-api`。

C++ session overload 在 session teardown 时仍安全连接 parent；raw parent lookup 要求调用者保持 parent 存活。Host 所有的 tracer 必须比全部 operation 活得更久。

```cpp
#include <neograph/observability/openinference.h>

// tracer is a host-owned neograph::observability::Tracer adapter.
// Its lifetime must cover the session and every provider operation.
auto session = neograph::observability::openinference_tracer(tracer);
auto observed = std::make_shared<neograph::observability::OpenInferenceProvider>(
    inner_provider, tracer, session);
// Use observed in NodeContext before compiling the graph.
```

Native LLM 属性只含公开 role/text projection、已声明 scalar 和已知 count。Native replay block、reasoning、raw wire envelope/event 及 `PreparedProviderRequest.encoded_body` 均不进入 trace payload；真实 custody 留在 request 和 outcome。已知零值会记录，未知值被省略。超过 signed span 范围的 count 编码为 decimal string。公开 text delta 生成 `llm.token` event，Python OTel 属性为 `{"chunk": text}`。Completion 设为 OK；failure 使用安全的 provider message 设为 ERROR。Dispatch 异常保留原 product error，同时在 span 中记录错误。公开 prompt、output 和异常消息仍可能含 application secret，应控制 exporter 接收的数据。

| 属性 | Native 来源 |
|---|---|
| `openinference.span.kind` | `"LLM"` |
| `llm.model_name` | 已准入 prepared model |
| `llm.invocation_parameters` | 可用的已声明 temperature 与 output cap |
| `llm.input_messages.{i}.message.role` | 公开 role projection |
| `llm.input_messages.{i}.message.content` | 公开 text part |
| `input.value` / `input.mime_type` | 公开 message JSON / `application/json` |
| `llm.output_messages.{i}.message.role` | 每个返回 message 的 role |
| `llm.output_messages.{i}.message.content` | 公开 text part |
| `output.value` / `output.mime_type` | 拼接公开 text / `text/plain` |
| `llm.token_count.prompt` | 存在的 `usage.input_total.value` |
| `llm.token_count.completion` | 存在的 `usage.output_total.value` |
| `llm.token_count.total` | 存在的 `usage.total.value` |

Token 属性报告 provider usage，包括失败时可用的 partial usage。它们不能证明 vendor charge 或恢复 budget authority。Charged/reserved accounting 使用 `UsageAccumulator.authority_snapshot()` / Program 的 `provider_budget_authority`，与 nullable usage report 分开处理。

### 端到端：NeoGraph + Phoenix 在一个代码块中

启动 Phoenix，再将 graph specification、现有 typed provider、明确的 model 和 `RunConfig` 传给 `trace_graph`。Helper 在 graph compile 前安装 wrapper，并让 graph/node `CHAIN` span 和 provider `LLM` span 使用同一 tracer。只有已执行的 provider 调用生成 LLM span；trace count 仍是 usage report，而非 charged accounting。

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

`ParentContextTracer` 明确把调用者的 run context 传入 graph worker 上的 provider dispatch。LLM span 的 parent 是 run root，不保证每个并发 task 的 node 级 ancestry。[OpenInference convention](https://github.com/Arize-ai/openinference/blob/main/spec/semantic_conventions.md) 定义 `CHAIN` 和 `LLM`；NeoGraph 导出上面的 subset。选择或 redact 公开 text 与 graph payload 时遵循 [OpenTelemetry 敏感数据指引](https://opentelemetry.io/docs/security/handling-sensitive-data/)。

### 注意

OpenTelemetry 为 opt-in；base wheel 不要求它。另行安装 API/SDK/exporter。同一 run 的 graph callback 只用 `otel_tracer` 或 `openinference_tracer` 之一。OTLP endpoint 和 credential 必须匹配 backend 配置；只换 URL 并不保证普遍 backend 兼容。

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
| `model` | `std::string` | 显式模型名称；typed provider 不选择默认模型 |

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
| `model` | `std::string` | 显式模型名称；typed provider 不选择默认模型 |
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

<a id="13-mcp-module"></a>
## 13. MCP 模块

**头文件：** `<neograph/mcp/client.h>`
**命名空间:** `neograph::mcp`

Model Context Protocol（MCP）客户端实现。它连接 MCP 服务器、发现
可用工具，并将其包装为 NeoGraph 的 `Tool` 实例。

提供两种传输方式：

- **HTTP** — `MCPClient("http://host:port/mcp")`。URL 是服务器的 MCP 端点，按原样使用，
  包括路径和查询，不附加任何内容（旧版客户端会附加 `/mcp`）。已发现的工具会保留其来源
  Streamable HTTP 会话，包括 `Mcp-Session-Id`、协商的协议版本、超时设置和自定义头。
- **stdio** — `MCPClient({"python", "server.py"})`。客户端在 `fork` 前解析 `PATH`，用
  `execve` 执行子进程，连接双向管道，并在子进程的 stdin/stdout 上交换按行分隔的 JSON-RPC。
  子进程在创建它的 `MCPClient` *或任何 `MCPTool`* 存活期间一直存在。关闭时先关闭其 stdin，
  最多等待一秒让它退出，然后向其进程组发送 SIGTERM，再最多等待一秒，之后发送 SIGKILL。

### MCPTool

将单个 MCP 服务器工具包装为本地 `Tool` 实现。已发现的工具无论使用何种传输，
都保留其来源协议会话。

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


通常不直接构造 `MCPTool` —— `MCPClient::get_tools()` 会发现并包装工具。用 URL 构造函数
创建的工具在第一次调用时打开一个 MCP 会话，之后的每次调用都复用它，并在工具销毁时用
`DELETE` 结束该会话。

所有调度器都调用带上下文的 `execute_async`，因此运行的取消令牌（在 attempt 超时下为节点
attempt 的令牌）和截止时间会在两种传输上约束调用。取消会立即停止等待并抛出
`graph::CancelledException`，工具失败契约将其视为取消；截止时间或请求超时抛出
`asio::system_error(asio::error::timed_out)`，即 `timed_out` 调用。无论哪种情况，都会用
`notifications/cancelled` 通知服务器，运行取消等待该调用的协程时也是如此。之后到达的应答被
丢弃：stdio 按 JSON-RPC id 路由应答，而该调用的 id 已不再等待；HTTP 则放弃该请求。
`isError` 结果抛出普通工具失败 `std::runtime_error`。在 Python 中，`mcp.MCPError` 保留
JSON-RPC 错误的 `code`、`data` 和 `http_status`。

### MCPClient

连接 MCP 服务器、执行初始化握手，并提供发现和调用工具的方法的客户端。

> `MCPClient` 不是为子类化设计的 —— 直接使用即可。
> `rpc_call_async()` 是唯一的传输层调用；同步方法驱动同一个会话。参见
> [`ASYNC_GUIDE.md` §9.5](ASYNC_GUIDE.md#95-mcpclient)。

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


**线路协议：** NeoGraph 的 MCP 客户端提出 `protocolVersion = "2025-11-25"`，并像官方 SDK
客户端一样接受答复 `2025-11-25`、`2025-06-18`、`2025-03-26` 或 `2024-11-05` 的服务器
（`MCP_SUPPORTED_PROTOCOL_VERSIONS`）。连接 `2024-11-05` 服务器使用 stdio 或 Streamable HTTP，
不支持更早的 HTTP+SSE 传输。其他答复会使 `initialize` 以同时指明两个版本的协议错误失败，并按 MCP 版本协商的要求断开
连接。HTTP 在之后每个请求的 `MCP-Protocol-Version` 头中发送协商的版本。尚不支持 MCP
`2026-07-28`（没有 `initialize`，也没有会话）：只提供该修订版的服务器以 `-32022` 拒绝握手，
调用方收到的是 `MCPError`。

**会话与传输的归属：** 两个构造函数创建同一种协议会话。会话拥有 JSON-RPC id、应答校验、
初始化（含 initialized 通知）、分页和工具结果的转换。在尚未握手的客户端上通过
`rpc_call_async()` 发送的请求会先执行握手。端点 URL、头、协商的 `Mcp-Session-Id` 和请求超时
只归 HTTP 传输所有；子进程、管道、读取器和应答 id 多路分解器只归 stdio 传输所有。已发现的
工具保留其协议会话，因此在 `MCPClient` 销毁后仍可使用；会话在对客户端或工具的最后一个引用
释放时结束。

**遵循传输规则：** 两种传输都会应答服务器的请求：`ping` 得到空结果，其他所有请求得到
`-32601`，因为客户端不声明任何客户端能力（roots、sampling、elicitation）。只有没有
`method` 的消息才算应答，因此复用待处理调用 id 的服务器请求不会完成该调用。服务器发来的
JSON-RPC 批处理（MCP `2025-03-26`）逐条处理，其中请求的应答以一个数组发回。在 HTTP 上，
客户端读取 JSON 和 Server-Sent Event 应答（按 WHATWG 规则：CRLF、LF 或 CR 行尾，多行
`data`、`id`、`retry`），对在调用的事件流上到达的服务器请求，在保持该流打开的同时用单独的
POST 应答，并在应答到达后停止读取该流。服务器在应答之前关闭的流，会在流的 `retry` 间隔之后
用 `GET` 和 `Last-Event-ID` 续接。`initialize` 之后，客户端像官方客户端一样，为不属于任何
调用的消息打开会话的可选 `GET` 事件流；没有该流的服务器返回 405，客户端将其视为没有该流
而不是失败：不再请求，会话及其调用照常继续。如果服务器对携带会话 id
的请求返回 404，说明会话已结束：客户端初始化一个新会话，并重新发送该请求一次。
`shutdown()` 和最后一个引用的释放会用最多两秒的 `DELETE` 结束会话。

**通知：** 服务器发送 `notifications/tools/list_changed` 时，会调用通过
`set_tool_list_changed_handler()` 注册的处理函数；此时请重新读取 `get_tools()` 或
`get_tool_definitions()`。在那之前，缓存的模式可能已过时，因此跳过 `outputSchema` 检查。
处理函数在客户端的传输线程上运行：必须尽快返回，且不得调用该客户端。

**加固的 stdio（`StdioClientConfig`）：** `argv`、`cwd` 和 `environment` 是显式的。使用
`replace_environment` 时，配置的条目就是服务器的全部环境；不使用时，服务器继承本进程的
环境，配置的条目添加到其中或覆盖同名条目（`PATH` 条目也决定在哪里查找 `argv[0]`）。会话
持续读取服务器的 stderr，并保留最后 `max_stderr_bytes`（64 KiB）；stderr 输出不会结束会话。
`startup_timeout` 约束从启动到 initialized 通知的过程，`request_timeout` 约束每个请求。被取消
或超时的请求会结束加固会话：客户端发送 `notifications/cancelled`，等待 `cancel_grace`
（500 ms）让服务器停止，然后终止进程树；之后的请求以 `MCPFailure::shutdown` 失败。

**约束与失败：** `rpc_call_async(method, params, deadline, cancel_token)` 和
`call_tool_result_async()` 接受绝对的 steady-clock 截止时间和 `graph::CancelToken`。取消和
截止时间会停止对该调用的等待，用 `notifications/cancelled` 通知服务器（对 `initialize` 不
发送），且不影响共享的 stdio 进程和其他应答。`initialize(name, deadline, cancel_token)` 以同样
方式约束握手，但不通知服务器，因为 MCP 从不取消 `initialize`；此时 stdio 服务器可能拒绝第二次
握手。HTTP 还会对每个请求应用 `MCPClientConfig::request_timeout`。传输和线路失败抛出
`MCPTransportError`，这是公开 `failure()`（`connection`、`timeout`、`cancelled`、`shutdown`、
`http_status` 或 `protocol`）和 `http_status()`（非 HTTP 失败时为 0）的 `std::runtime_error`
子类。JSON-RPC 错误应答是带有服务器 `code()` 和 `data()` 的 `MCPError`，由 HTTP 错误状态携带
时也是如此（此时 `http_status()` 指明该状态）。两个类都不表示工具层面的 `isError` 结果；
请用 `call_tool_result()` 检查该结果。

| 方法 | 说明 |
|--------|-------------|
| `MCPClient(url)` | 构造按原样使用 MCP 端点 `url` 的 HTTP 模式客户端 |
| `MCPClient(argv)` | 启动子进程并构造 stdio 模式客户端。`argv[0]` 在 fork 前通过 `PATH` 解析；exec 失败会在第一次 RPC 时表现为连接错误。出于安全考虑拒绝 Windows `.bat`/`.cmd` |
| `MCPClient(StdioClientConfig)` | 带显式环境、有界 stderr 捕获、截止时间和取消宽限期的加固 stdio 启动 |
| `initialize(client_name)` | 执行一次 MCP 初始化握手。重复调用是幂等的；协议/传输失败会抛出 |
| `initialize(client_name, deadline, cancel_token)` | 由截止时间和取消令牌约束的握手 |
| `get_initialize_result()` | 返回协商的协议、能力、服务器信息、instructions 和原始结果 |
| `list_tools(cursor)` | 将游标视为不透明值，获取一页 |
| `get_tool_definitions()` | 遍历所有页面并保留完整的工具元数据 |
| `get_tools()` | 发现所有页面，返回保留会话的 `MCPTool` 实例 |
| `call_tool(name, arguments)` | 以给定参数按名称调用工具。返回原始 JSON 应答 |
| `call_tool_result(name, arguments)` | 保留 content、structured content、`isError` 和 `_meta` 的类型化结果 |
| `call_tool_result_async(name, arguments, deadline, cancel_token)` | 与 `rpc_call_async` 一样，由绝对 steady-clock 截止时间和取消令牌约束的类型化调用 |
| `set_tool_list_changed_handler(handler)` | 在 `notifications/tools/list_changed` 时调用 `handler`；空处理函数停止调用 |
| `shutdown()` | 永久结束客户端及其创建的所有工具：HTTP 会话用 `DELETE`，stdio 服务器有序停止 |
| `rpc_call_async(method, params, deadline, cancel_token)` | 其他所有方法使用的传输层 JSON-RPC 调用 |

**提供工具。** `MCPServer`（`<neograph/mcp/server.h>`）用注册的处理函数应答 `tools/call`；
处理函数的失败如何到达客户端见 [Protocol failure mapping](#protocol-failure-mapping)。对请求
`2025-11-25`、`2025-06-18`、`2025-03-26` 或 `2024-11-05` 的客户端，它用该修订版及其定义的
字段应答（`2025-06-18` 没有 `icons` 和 `execution`；`2025-03-26` 还没有 `title`、`outputSchema`、
`_meta`、`structuredContent` 和 `resource_link` 内容，只有结构化数据的结果以文本携带这些数据；
`2024-11-05` 还没有 `annotations` 和音频内容；该修订版没有的内容块以携带其 JSON 的文本块发送），
对其他请求用 `2025-11-25` 应答；`protocol_version()` 报告协商结果。协商了 `2025-03-26` 的客户端
可以按该修订版对接收方的要求发送 JSON-RPC 批处理，`2024-11-05` 客户端也一样：批处理在 stdio 和
HTTP 上以一个数组应答。之后的修订版去掉了批处理，initialize 也不能放进批处理，因此这样的批处理
是一个 `-32600` 错误（HTTP 400）。`handle_message(message, reply)` 把每条消息的结果（应答、批处理
的数组或无）交给调用方。注册时会拒绝 `inputSchema` 或
`outputSchema` 未声明 `"type": "object"` 的工具（旧默认值空对象按 `{"type": "object"}` 读取）、
不是验证器支持的 JSON Schema 2020-12 的模式，以及超出 SEP-986 的 1 到 128 个
`A-Za-z0-9_.-` 字符的名称。参数用 JSON Schema 2020-12 验证器校验（`$ref`、`anyOf`、
`minimum`、作为整数的整值 `1.0`，以及 core、applicator 和 validation 词汇的其余部分）。
未知工具是 JSON-RPC 错误 `-32602`；错误应答在能读取请求 id 时携带它，否则不带 id，绝不使用
`null`。`MCPHttpServer`（`<neograph/mcp/http_server.h>`）通过 Streamable HTTP 提供同样的
功能：会话在以下情况下结束：收到 `DELETE`；`session_idle_timeout`（30 分钟）内没有请求；或
在 `max_sessions`（64）时有新客户端需要空位，而它是没有进行中请求、最久未使用的会话；之后
该 id 返回 404。工具调用在返回前占用一个 HTTP 线程，因此线程按需启动，最多到
`max_http_threads`（64）；超过 `response_timeout` 的请求得到 504，其工具调用被取消；`DELETE`
会关闭其连接；设置 `protected_resource_metadata` 时，服务器发布 RFC 9728 元数据，并在每个
401 质询中指明其位置。

**HTTP 用法：**

```cpp
neograph::mcp::MCPClient client("http://localhost:8000/mcp");
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
`A2AServer` 通过基于 HTTP 和 SSE 的 A2A JSON-RPC binding 公开一个 graph（或已准入的 `ProgramVersion`），`A2AClient` 调用远程 agent。A2A 1.0（v1.0.1）是主版本，0.3 是兼容 dialect（`WireDialect::{V1_0,V0_3}`）。官方 SDK（a2a-sdk 1.2.2、@a2a-js/sdk 1.3.0 和 0.3.14）在 `bindings/python/tests/interop/a2a_suite` 中测试两个方向。

#### A2A 版本策略

- **请求。** server 按 header 或 `A2A-Version` query parameter 中的 `A2A-Version` 所指定的 dialect 应答每个请求。没有版本即为 0.3（spec §3.6.2）。只支持 Major.Minor `1.0` 和 `0.3`，忽略 patch；`0.2`、`1.5`、`2.0` 等其他值得到列出所支持版本的 VersionNotSupportedError（`-32009`）。两种 method 写法（`SendMessage`、`message/send`）在任一版本下都接受。
- **Card。** card 按版本提供。`A2A-Version: 1.0` 得到纯 1.0 `AgentCard`；不带版本的请求得到 0.3 card，0.3 card 仍在 `supportedInterfaces` 中首先列出 1.0 interface。响应带有 `ETag`、`Cache-Control: public, max-age=300` 和 `Vary: A2A-Version`，`If-None-Match` 得到 304。未配置 `AgentCard::url` 时，card 公布 client 所用的地址。每个 skill 至少有一个 tag。card 从不声明未实现的 push notification、extended card 以及 HTTP+JSON 和 gRPC binding。
- **Client。** client 在第一次调用前获取 card，选择第一个版本为 `1.x` 或 `0.x` 的 JSONRPC interface，并优先选择 URL 与 client 的 `base_url` 相同的 interface。只有无法获取 card 时才探测：先发 1.0，遇到 `-32601`、`-32009` 或 HTTP 400 后改用 0.3，并记住结果。所选 interface URL 与 `base_url` 同源时，RPC 发往该 URL，因此 endpoint 为 `/a2a/v1` 的 agent 也能工作。除非通过 `set_allow_cross_origin_interfaces(true)` 允许，card 无法把 client 引向其他源，此时 RPC 发往 `base_url`，client 会向 stderr 写一条警告，指出它没有采用的源。`endpoint_url()` 返回正在使用的 endpoint，message id 是随机 UUID。
- **0.3。** 0.3 作为语义相同的兼容 dialect 保留。新功能以 1.0 为目标。

#### Task 与 method

每个 run 都在自己的 worker thread 上执行；stream 或 blocking send 只是观察它，因此 task 的生命周期长于断开的连接。`SendMessage` 会阻塞到 task 进入 terminal 或 interrupted 状态，除非 `returnImmediately`（0.3 为 `blocking: false`）要求立即返回 SUBMITTED 的 task。`SendStreamingMessage` 和 `SubscribeToTask`（0.3 为 `tasks/resubscribe`）先发送 Task，再发送 status 和 artifact update，并在 terminal 或 interrupted 状态时关闭；0.3 stream 以 `final: true` 结束。服务器重启后恢复的 Program task 没有自己的 channel，因为它的 run 不是这个进程启动的：对它调用 `SubscribeToTask` 时，先发送 Task，再跟随持久 run，直到 run 产生结果或 cancel 生效，然后像其他 run 一样发送结果 artifact 和最终 status（暂停的 run 以 `TASK_STATE_INPUT_REQUIRED` 结束）。恢复之后已经结束的 run 会先被读出，并作为 terminal 以 `-32004` 回答。无法跟随 run 时，stream 以 `-32603` 错误事件（ErrorInfo reason `RUN_UNAVAILABLE`）结束；task 在本服务器上没有 run 时，调用以 `-32603` 回答，不会空着关闭。静默 5 秒的 stream 会写出 SSE 注释 `: keep-alive`，这样对空闲连接设置超时的客户端或代理就不会切断仍在工作的 run。`GetTask` 和 `ListTasks` 遵循 `historyLength`（0 表示省略 history）。`ListTasks` 按 `contextId`、`status` 和 `statusTimestampAfter` 过滤，用 `pageSize`（1 到 100，默认 50）和 `pageToken` 按最新优先分页，未设置 `includeArtifacts` 时省略 artifact。每个 status 都带有 timestamp。

task id 由 server 生成。`taskId` 未知的 message 以 `-32001` 失败，terminal task 以 `-32004` 失败，与 task 不同的 `contextId` 以 `-32602` 失败，仍在运行的 task 以 `-32603`（ErrorInfo reason `TASK_BUSY`）失败。发给 INPUT_REQUIRED task 的 message 会从 interrupt 保存的 checkpoint 恢复其 run；`GraphAgentAdapter::build_resume_value` 将 message 转为 resume value，没有 checkpoint store 的 engine 则以该 message 为 input 重新运行 graph。基于 Program 的 task 目前还不能通过 A2A 接收该回答：message 得到 `-32004`，task 继续等待。NeoGraph collaboration envelope 保留发送方指定的幂等 task id（[A2A collaboration 扩展](#a2a-collaboration-extension)）。请求按 dialect 校验，违规得到带 `google.rpc.BadRequest` field violation 的 `-32602`，每个 error 都带有 `google.rpc.ErrorInfo` detail。配置 `CollaborationAuthenticator` 时，card 声明 Bearer security scheme，若请求必须认证还会声明全局 requirement；缺少 credential 得到带 `WWW-Authenticate` 的 HTTP 401，已认证但无权使用 collaboration link 的 peer 得到 HTTP 403。

<a id="a2a-error-mapping"></a>
#### A2A 错误映射

run 的失败 class 按 [协议失败映射](#protocol-failure-mapping) 到达 peer：task 状态、作为 status message 的 public message，以及 `metadata["neograph/failure"] = {code, retryable}`，异常文本永远不会到达。完成的 run 为 `COMPLETED`，以 agent 回复作为 status message 并带有 adapter artifact；step limit 为 code `NG.StepLimit` 的 `FAILED`；failure exit 为带 exit 声明的 code 的 `FAILED`。server 拒绝的请求：

| 请求 | A2A 结果 |
|---|---|
| 指向未知 task 的 message | `-32001` TaskNotFoundError |
| 发给 terminal task 的 message | `-32004` UnsupportedOperationError；collaboration 重新投递返回该 task |
| `contextId` 与 task 不同的 message | 带 field violation 的 `-32602` |
| 发给仍在运行的 task 的 message | `-32603`，ErrorInfo reason `TASK_BUSY`，domain `neograph` |
| 发给等待 input 的 Program task 的 message | `-32004`；task 继续等待（尚不支持通过 A2A 回答 Program 的 input 请求） |
| 达到 in-flight 上限 | `-32603`，ErrorInfo reason `RESOURCE_EXHAUSTED`，domain `neograph`，metadata `retryAfterSeconds` |
| 缺少 agent 要求的 credential 或 credential 无效（collaboration message，或 strict 认证下的所有请求） | 带 `WWW-Authenticate` 的 HTTP 401；body 为 ErrorInfo reason `UNAUTHENTICATED`、domain `neograph` 的 JSON-RPC `-32600` |
| collaboration link 不允许的已认证 peer | HTTP 403；body 为 ErrorInfo reason `PERMISSION_DENIED`、domain `neograph` 的 JSON-RPC `-32600` |
| 取消 terminal task | `-32002` TaskNotCancelableError；task 不变 |
| push notification 配置；extended card；card 为 `streaming = false` 时的 streaming method | `-32003`；`-32004`；`-32004` |
| 既非 1.0 也非 0.3 的 `A2A-Version` | `-32009` VersionNotSupportedError |

每个 error 都带有一个 `google.rpc.ErrorInfo`；除非该行注明 `neograph`，其 domain 为 `a2a-protocol.org`。

<a id="a2a-collaboration-extension"></a>
#### A2A collaboration 扩展

带有 `CollaborationMailbox` 的 Program agent 接受 NeoGraph collaboration envelope，并在两个版本 card 的 `capabilities.extensions` 中声明它：`uri` 为 `a2a::COLLABORATION_EXTENSION_URI`（即本节），`required` 为 false，`params` 为 `{"format": "neograph-a2a-collaboration-v1", "metadataKey": "neograph_collaboration"}`。这样的 message 带有 data part `{format, envelope}` 和 `metadata.neograph_collaboration`，只能由已链接 collaboration 中已认证的 peer 发送。发送方预先指定接收方的 task id：首次投递以该 id 创建 task，重新投递返回已有 task 而不会启动新的 run。普通 A2A client 不需要它。

#### Client stream 与 Python

SSE 接受 LF/CRLF/CR、comment、多行 data 以及最后未终止的 data。opening task snapshot、status update（包括其 metadata）以及 artifact append/replace update 会累积成返回的 task。观察到 event 后，即使没有外部 callback 也禁止 dialect 重发。非 SSE 的 RPC error 通过 `A2ARpcError::code()` 保留 code；非 2xx 的 HTTP 不会变成成功的 task。caller node 依次选择 final/interrupted agent status text、第一个 artifact text、最后一个 agent history text；progress status 或 user history 不能覆盖答案。通用 `async_post_stream` 对非空的固定 `Content-Length` body 也连同 status 交付一次；长度为 0 时不产生 chunk。body limit、提前 EOF 和已缓冲的多余数据会被拒绝。redirect/chunked/close-delimited 规则不变。

Python 的 `neograph_engine.a2a` 导出 `WireDialect`、`AgentInterface`、`AgentCard.supported_interfaces`/`raw`、`Part.media_type`、`MessageSendConfiguration`、`MessageSendParams`、`StreamEvent` 以及 status/artifact record。`A2AClient.wire_dialect()` 返回 enum 或 `None`；`set_authorization_header()`、`set_allow_cross_origin_interfaces()` 和 `endpoint_url()` 与 native client 一致；`send_message(params)` 是 multipart overload。`send_message_stream(text, on_event, task_id="", context_id="")` 或 `(params, on_event)` 返回累积的 `Task`，并把 owned event snapshot 传给返回 bool 的 callback。blocking call 会释放 GIL。`a2a.A2ARpcError.code` 保留远程整数 code。

**公开头文件：** [`include/neograph/a2a/`](../include/neograph/a2a/)。

### `neograph::acp` — Agent 客户端协议

**头文件：** `<neograph/acp/{server,types}.h>`
编辑器↔agent 通过 stdio 上的换行分隔 JSON 进行 JSON-RPC 2.0 通信（Zed、Gemini CLI、Neovim CodeCompanion）。协议为 ACP 版本 1，已用官方 schema v1.25.0 以及官方 Python（0.12、1.0）和 TypeScript SDK 验证。client→agent：`initialize`、`session/{new,resume,prompt,cancel}`。agent→client：`session/update`、`session/request_permission`；`fs/*`、`terminal/*` 和 `elicitation/*` 只在客户端声明了相应能力时，才通过延迟绑定的 `ACPClient` 发送。`ACPServer::handle_message` 在工作线程上运行每个提示轮次，同时最多 `max_inflight_prompts`（32）个，每个会话一个。被拒绝的轮次返回 JSON-RPC `-32603`，`data.reason` 为 `"overloaded"` 或 `"session_busy"`；`-32000` 是 ACP 的 "Authentication required"，因此不使用。`initialize` 之前的调用返回 `-32600`，未知会话返回 `-32002`；对 agent 已知但没有检查点的会话执行 `session/resume` 会成功。

提示接受 text 和 `resource_link` 块（基础内容；默认适配器把链接以 `[name](uri)` 形式写入输入），image、audio 和嵌入资源只在 `capabilities().prompt` 声明后才接受。格式错误或未声明的块返回 `-32602`。默认适配器会写入 `_acp_session_id`、`_acp_cwd`、`_acp_additional_directories` 和 `_acp_mcp_tools`；如需修改，重写 `build_session_state`（或旧的 `build_initial_state`）。`agentInfo` 始终是 ACP `Implementation`，缺少的 name 或 version 会被补齐。

轮次的结束方式：

| NeoGraph 结果 | ACP 结果 |
|---|---|
| 完成 | 一条 `agent_message_chunk`（文本为空时不发送）之后返回 `stopReason: "end_turn"` |
| 被 `session/cancel` 取消 | `stopReason: "cancelled"` |
| 步数上限（`RunConfig::max_steps`） | `stopReason: "max_turn_requests"` |
| 超出模型 token 预算 | `stopReason: "max_tokens"` |
| provider 拒绝或内容过滤（`NG.SafetyBlocked`） | `stopReason: "refusal"` |
| `NodeInterrupt` | 同一轮次内的 `session/request_permission`；回答后恢复运行 |
| Hook 阻止（`NG.HookBlocked`）、节点或工具失败（`NG.TaskFailed`、作者代码）、超时（`NG.Timeout`）、provider 失败（`NG.ProviderFailed`）、金额或配额预算（`NG.BudgetExhausted`）、配置、逻辑或未知失败 | JSON-RPC 错误 `-32603`，message = 公开消息，`data: {code, retryable}`（[协议失败映射](#protocol-failure-mapping)） |
| 并非由客户端引起的取消（节点中止、关闭） | JSON-RPC 错误 `-32800` |

异常文本（`what()`）不会发送给客户端：message 是带类型的公开消息（`NodeFailureError::public_message`）或该失败类别的通用文本，细节只保留在宿主日志中。中断会变成一个工具调用：标题为中断原因，`rawInput` 为载荷，带有 `_meta["neograph/interrupt"]` 以及选项 "approve"（`allow_once`）和 "reject"（`reject_once`）；被选中的 option id 就是恢复值。如需修改，重写 `interrupt_permission` 和 `interrupt_resume_value`。回答 `cancelled` 会以取消结束轮次；适配器返回 `std::nullopt` 或客户端无法回答时，轮次以 `data.code: "NG.Interrupted"` 失败，会话不会停留在暂停状态。由 `session/resume` 从检查点恢复的暂停（例如 agent 重启之后）由下一个提示的文本回答。

`session/cancel` 只取消正在进行的轮次；没有轮次运行时收到的取消会被忽略（会重排消息的传输可以用 `set_pre_armed_cancel(true)` 保留以前的预先取消）。取消后，等待中的 agent→client 请求有 500 ms 的回答时间，之后服务器发送 `$/cancel_request` 并结束轮次。权限请求默认没有超时，除非用 `ACPClient::set_permission_timeout` 设置。stdin 到达 EOF 时，`run()` 会让进行中的轮次在 `set_eof_grace`（1 秒）内完成，然后取消它们。

`session/new` 和 `session/resume` 中的 stdio `mcpServers` 通过 NeoGraph 的 MCP 客户端（以 `NEOGRAPH_BUILD_MCP_CLIENT` 构建）在会话的 `cwd` 中启动，环境是最小的：agent 的 `PATH`（Windows 上还有 `SystemRoot`）加上客户端的 `env` 项，可能含有 provider API 密钥的其余 agent 环境不会传递；节点通过 `ACPClient::session_tools(session_id)` 使用这些工具。HTTP 和 SSE MCP 服务器不会被声明，并以 `-32602` 拒绝。无法启动的服务器会使请求以 `-32603` 和 `data.reason: "mcp_server_unavailable"` 失败。

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

- **`neograph::AsyncTool`** — `Tool` 的对应类型，为天然适合协程的工具（HTTP 获取、MCP 调用）
  提供 `execute_async(json)`。同步 `execute()` 通过 `run_sync` 以 `final` 方式路由。
- **`neograph::graph::NodeCache`** — 每节点记忆化缓存，通过构造时的 `EngineConfig::cached_nodes`
  选择性启用（setter 仍作为兼容接口保留）。
- **`neograph::graph::create_deep_research_graph`** —
  open_deep_research 风格的 supervisor + 子研究者扇出，由
  `examples/25_deep_research.cpp` 使用。它包含
  `BriefNode` LLM 重写、`FinalReportNode` token 限制重试以及
  `ClarifyNode` HITL 闸门。

如果本导览中没有所需类型，请直接检查 `include/neograph/`；
每个公共头文件都包含其参考文档。
