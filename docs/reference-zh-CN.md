<!-- neograph-i18n: source=docs/reference-en.md locale=zh-CN source_sha256=79727c2b30c2623180976aae1d3a09ad00a17554779a9b48e209c585311165bf -->
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

需要 CMake 3.20 或更高版本。Core 公开拥有所有权的 typed provider 契约，所以 `NEOGRAPH_BUILD_LLM=OFF` 时 SchemaProvider runtime 仍必需。显式 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` 优先；否则先查找已安装的 `SchemaProvider` runtime package。若没有且 `NEOGRAPH_FETCH_SCHEMAPROVIDER=ON`（默认），就下载 `cmake/NeoGraphSchemaProvider.cmake` 固定的不可变 GitHub archive。Offline build 需安装 SDK、将 `CMAKE_PREFIX_PATH` 指向其 prefix，并传入 `-DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`。CMake 不猜测 sibling checkout，也不选择已删除的 bundled interpreter。禁用 NeoGraph 可选 HTTP module 时，SDK runtime 的 transport 依赖仍必需。已记录的 SDK runtime/archive 验证覆盖 Linux/POSIX。Windows NTFS 与 macOS 实现已存在，但新 platform 验证需要 runtime 证据；尚未确立 WASM provider runtime 验证。

SDK imported target 提供 `include/SchemaProvider` include root；公开示例直接使用 `<descriptor/descriptor.h>`、`<runtime/client.h>`、`<neograph/llm/schema_provider.h>`，不依赖 recipe 专用 helper。

已安装 SDK package 至少为 `0.1.1`，须匹配 interface revision 4 header 和 shared-library generation 4。版本匹配本身不批准旧 interface/ABI binary。

```cmake
find_package(SchemaProvider 0.1.1 CONFIG REQUIRED COMPONENTS runtime)
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
};
```

| 字段 | 类型 | 描述 |
|-------|------|-------------|
| `name` | `std::string` | 唯一的工具名称 |
| `description` | `std::string` | 显示给 LLM 以解释工具用途的描述 |
| `parameters` | `json` | 描述可接受参数的 JSON Schema 对象 |

### Owned Outcome

提供方调用返回 `sp::runtime::Result`，即持有 `sp::Completion` 或 `sp::Failure` 的不可变、拥有所有权的 `std::shared_ptr<const sp::Outcome>`。请保留完整结果，而非仅显示文本。顺序消息/part、native continuation、存在的 wire envelope、顺序 raw 观测、停止依据及真实尝试元数据在调用与客户端销毁后仍然保留。`input_total`、`output_total` 和 `total` 等用量计数器为 `std::optional<sp::Count>`；存在的 count 有 `uint64_t value` 和 `Evidence`。`Usage` 还记录 stage、quality 和 conflict。缺失表示未知，不应伪造零值。失败保留原始部分结果。`ProviderFailure::outcome()` 与 `ProviderObserverError::outcome()` 保留真实结果，后者的 `cause()` 也保留观察者异常。

`sp::Completion::wire_envelope` 和 `sp::PartialCompletion::wire_envelope` 可为空，且随提供商家族而异；不存在时 Python 视图返回 `None`。当前缓冲式 Chat 将此字段保持为空，并将完整响应文档保留在 `raw_events` 的 `sp::RawWire{type="chat.completion", payload=document}` 中（Python 为 `ProviderRawWire`）。请检查是否存在，并查看实际的类型化 raw 证据；不会通过回退合成封装，也不会把部分失败变为成功。拥有所有权的线协议证据，包括提供商的私有字段，在提供商销毁后仍保存在留存的结果中。原生追踪排除 raw 封装/事件和原生重放/推理；用于检查的 JSON 副本不授予原生权限或财务权限。

调用方应使用完整的类型化消息/部分、逻辑角色和文本，并在继续执行需要时保留真实原生所有权。检查点与 Chat 请求的文本 content 可以采用文本字符串或有效的类型化文本部分数组；偶然采用的任一种序列化形状都不是通用契约。

重放真实 `NativeContext` 时，须保留原始 `request.messages` 的完整前缀，再按顺序追加直接返回的 `outcome.messages`，并保留其原生所有权。直接结果包含新返回的消息，不包含原始请求历史。仅重放其中的 assistant 消息会在任何线协议 I/O 之前被 `ReplayIneligible` 拒绝。从 `NativeArchive` 恢复的 assistant 仍须提供原始完整前缀；存档保管权限不能替代历史谱系检查。图的 `RunResult.native_messages` 已包含完整历史，不要再次把原始输入加到开头。

`UsageAccumulator::snapshot()` 返回累计报告。`total_tokens_wide()` 返回已计费 token 与未解决预留之和，不能把它显示为报告用量。结算要求具有 input/output count 的 final、consistent 报告，并计入有依据的最大 total，不截断超额用量。任何累计报告缺少 counter，汇总该 counter 也为未知。预留、本地计费和 vendor 发票是不同的记录。

### Portable projections


实际结果存在后，若 post-effect 结算或 terminal receipt 持久化失败，`ProviderDispatchOutcomePersistenceError::outcome()` 保留原始不可变结果，`cause()` 保留原始持久化异常。若 delivery 也失败，`delivery_error()` 保留原始观察者异常。持久化成功后的观察者失败原样重新抛出原异常；未知/无结果 transport 失败不会伪造 outcome。
`ChatMessage` / `ChatTool` 和 JSON 只是 portable projection，不是 native 权限。Portable 格式仍为 [`provider-message-v2`](../schemas/provider-message-v2.schema.json)、[`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json)。真实 C++ checkpoint sidecar 保留内存 native seal。持久 native 历史需要 host-owned `sp::NativeArchive`：closed v3 / `spna3` 使用独立密钥提供经认证的 owner-private custody；archive v2 被拒绝，不升级或解释。认证绑定全部 semantic descriptor 选择（origin/path/header、policy、请求 field mapping、usage path、stop mapping）、owner 和精确 custody binding。这不是加密或 vendor-issuer 认证；不得公开 archive 正文、密钥、native blob 或 raw wire 观测。Archive 是证据存储，不是资金 grant 或 spending lease。Program/external bank 仍由独立 journal 拥有，复制 snapshot 不能创建 credit。

`RuntimeHistoryRecord` 的无参数 `serialize_canonical()` 支持可移植记录。持久原生历史须使用 `serialize_canonical(archive, owner_id)`，并提供所有者范围与该 ID 一致的存档。Python 暴露同一重载以及 `RuntimeHistoryRecord.parse(stored_bytes, archive=None, owner_id="")`；面向可移植记录的默认参数不会在缺少匹配存档时授权原生恢复。Python 的解析和存档感知序列化会在原生工作期间释放 GIL。JSON 观测数据仍与存档认证的保管权限不同。

Python 通过 `ContextStore.hydrate_records(range)` 与 `history_record_by_message_id(feed, message_id)` 保持类型化保管权限，后者返回记录或 `None`。`SQLiteContextStore(database_path, archive=None)` 接受真实存档，`LocalProgramHost` 将最后一个可选参数 `native_history_archive=None` 传入 `RuntimeConfig`。这些参数不会伪造授权或添加持久 Program 存储后端。返回的 `RuntimeHistoryRecord.message` 是保留真实共享原生所有权的独立类型化副本，修改它不能改写不可变 RAW 标识。存储构造与类型化检索会释放 GIL；当必须提供保管权限时，无存档存储会拒绝原生历史。

包含 `Assistant` 消息的 RAW `RuntimeHistoryRecord` 必须使用 `RuntimeTrustClass.ModelOutput`；`RuntimeTrustClass.UntrustedInput` 仅接受 `User` 消息。这些是实际的 Python enum 名称。trust class 标明记录的角色与来源边界；选择它不会授予原生重放保管权限或支出、执行权限。

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

Python `LocalProgramHost` 析构时，会在 `ProgramRuntime` 取消、排空并 join 调度器工作期间释放调用者的 GIL，使正在执行的 Python 节点能够完成。销毁其他 host 成员及其持有的 Python 回调/对象之前，会重新获取 GIL。此变更仅涉及结束处理，不暴露额外的 capability binder 或执行权限。

`RecordedBindingSet` 是 source-bound move-only data，不是调用方提供的 dispatcher。可信 Catalog `recorded_capability_binder` 独立读取真实持久 source event，materialize captured-only capability。`ProgramRuntime::replay_recorded()` 检查原始 selected-source permission，再通过 durable CAS 转移真实剩余 bank；inherited spend 不是新的 model grant。旧 `start_recorded` 续期 API 已删除。InMemory/File/SQLite/PostgreSQL Program store 在整个执行期间保留精确不可变 owned lease，不因 expiry 续期。Controlled JavaScript 仍验证 underlying capability manifest，消费精确 completed command 结果，不重新 dispatch external effect。

**Recorded-control causal fix 已在 full suite 实证。** Captured command replay 在执行前仅为新的 CPU wall-time/Core work 建立 durable reservation，再通过 result CAS publish 测量 work 与新产生的 Core checkpoint。不消耗新的 model、money、Program-operation allowance，也不重新 dispatch captured external effect。未结算 reservation 保持 debit。Reservation 选择认证 settlement transition，而非曾拒绝首个新 Core checkpoint 的普通 Running→Running transition。Await channel receive、timer wait/cancel、handoff wait 的开始/release 在所属 executor/strand 上串行化；既有 Recorded CPU/Memory await/handoff scenario 在 full suite pass；remote TSan coverage 限制如下明确保留。

以下观察记录于本次文档整理之前。它们是历史证据，不是新测试运行，也不保证所有 platform、transport 或 security 属性。

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

#### Interface 4 typed controls

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


这是源码和二进制破坏性变更；所有 C++ 使用者和自定义提供方都须使用匹配的新头文件/库重新编译。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、descriptor interpreter、Responses WebSocket 已删除，没有 alias 或兼容 bridge。SDK 为 alpha `0.1.1`、interface revision 4 / shared-library generation 4，采用 out-of-line capability check，不表示稳定发布。已记录 interface-3 SDK runtime/archive 验证是 Linux/POSIX 的历史证据，不是 interface-4 pass。Windows NTFS/macOS 实现已存在，但新 platform 验证需要 runtime 证据；WASM provider runtime 尚未验证。

Python 公开与 C++ 相同的所有权 request/outcome 边界：`make_provider_request`、`Provider.prepare`、`dispatch` 和 `invoke`。Provider 历史使用带 typed part 的 `ProviderMessage`；`ChatMessage` 仍是图的便利 projection。SDK 失败可通过 `ProviderOutcome.failure` 读取，host observer/settlement 异常保留 `outcome` 和 `cause`。构造器及 GIL/回调行为参见 [Python binding 指南](python-binding.md)。

Python SDK 的 vector/map getter 返回独立值：`ProviderMessage.parts`、`ProviderRequest.messages`、`RunConfig.provider_messages`、completion/partial 的 `messages` 和 `raw_events`、`RunResult.native_messages`、`ProviderLoopEntry.messages`、usage 的 `extra`/`conflicts`。修改快照后，将它重新赋给具有 setter 的属性；向 getter 结果 append 不会更新所属对象。可选的 `ProviderControls.provider`/`response_format`、`SchemaProviderDefaults.provider` 和 `ProviderToolResult.host` 也遵循读取、修改、重新赋值的规则。只读 outcome 证据保持不变。这些是 Python 绑定规则，不是所有 C++ getter 都返回副本的保证。

原生代码调用 Python provider 的 `prepare` 重写时，返回 `None` 会在消费 handle 前引发 `TypeError`。`ProviderDescriptorPolicy.identity` 是包含 raw SHA-256 digest 的 `bytes`；`.hex()` 仅用于显示转换。反复检查已保存的 Python provider/graph cause 会保留原始异常值与 traceback，不消费已保存异常的 restore 状态；此规则也适用于嵌套原生异常转换。

此前记录的 installed find_package Program C++/C ABI/dualQuickJS consumer 和 NeoGraph/SchemaProvider typed2-request lifetime/native/raw/mismatch consumer 在当时 snapshot 上 pass。这不证明 interface-4 package 验证；仅声明匹配不证明新 runtime 结果或更广 platform 支持。

当前 SDK4 Linux x86_64 证据覆盖全部 27 个注册 case：首次 full run 中 25 个 pass；修正两个 obsolete assertion 后，`native_archive` 与 `stop_reasoning_preservation` 在 focused 2/2 run 中 pass。这不是第二次 full-suite 27/27 run。未改 buffered/SSE Stop probe 和已安装 SDK 的 exact README consumer 也 pass。后者每个 zero-usage、unknown-usage、HTTP-400-failure variant 执行一个无 credential 请求。这些 SDK 结果不证明新 NeoGraph native build/wheel pass 或 Windows/macOS/ARM64/HTTP3 验证。

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

### RunContext（v0.4 PR 1，通过 `NodeInput.ctx` 暴露给节点）

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
    RunStatus status() const noexcept;            // Completed, Interrupted, StepLimit, or SafePoint

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
`RunStatus::StepLimit` 或 `RunStatus::SafePoint`，而不更改公开 `RunResult` 的数据布局。
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
`A2AClient` 与 `GraphAgentAdapter` 实现 `WireDialect::{V0_3,V1_0}` JSON-RPC 2.0 HTTP/SSE。`AgentCard.supported_interfaces` 为从新旧 card 解析的有序 `AgentInterface{url, protocol_binding, protocol_version, tenant}` 观测，raw card 保留。延迟选择首个 compatible JSONRPC 0.x/1.x interface，优先尾 slash 规范化后匹配配置 base URL 的项。card URL 不重定向 RPC endpoint。tenant 传至 send/get/cancel/stream。`wire_dialect()` 在选择或成功 probe 前为空，force discovery 重置。已取得 incompatible card 时直接拒绝，不 probe 其他 dialect。

无 card 时 probe 0.3，仅数字 JSON-RPC `-32601` 才切换，并记住成功 dialect。切换同时修改 method/body/header。V1 用 PascalCase、`A2A-Version: 1.0`、`ROLE_*`/`TASK_STATE_*`、不带 `kind` 的 flat text/raw/url/data part，以及反转 `blocking` 的 `returnImmediately`。解析 task/message wrapper 与 bare get/cancel task。server 响应编码按 version header，而非 method spelling：无 header 为 0.3，不支持 major 返回 `-32009`；默认 card 广告两者。bound discovery URL 保留 restart 行为，不覆盖 explicit endpoint。

SSE 接受 LF/CRLF/CR、comment、multiline data、最后未终止 data。opening task、status、artifact append/replace 累积为返回 Task。V1 在 opening submitted task 与 artifact 后发送 terminal status，无需 legacy `kind`/`final`/trailing task。观察到 event 后，即使无 external callback 也禁止 dialect redispatch。non-SSE RPC error 保留 `A2ARpcError::code()`；non-2xx HTTP 不成为成功 task。

caller node 回答优先级为 final/interrupted agent status text、首个 artifact text、最后 agent history text。progress status 或 user history 不覆盖答案。通用 `async_post_stream` 对 nonempty fixed-`Content-Length` body 保留 status 并传递一次；zero length 不发 chunk。body limit/early EOF/已缓冲 surplus 拒绝，redirect/chunked/close-delimited 规则不变。

Python `neograph_engine.a2a` 公开 `WireDialect`, `AgentInterface`, `AgentCard.supported_interfaces`/`raw`, `Part.media_type`, `MessageSendConfiguration`, `MessageSendParams`, `StreamEvent` 和 status/artifact record。`A2AClient.wire_dialect()` 返回 enum 或 `None`；`set_authorization_header()` 为 native setter；`send_message(params)` 为 multipart overload。`send_message_stream(text, on_event, task_id="", context_id="")` 或 `(params, on_event)` 返回累积 `Task`，向 bool callback 传递 owned event snapshot。blocking call 释放 GIL，callback owner 安全重获 GIL。`a2a.A2ARpcError.code` 保留 remote 整数 code。vector/optional child 是 detached snapshot，`Task.status` 和 `MessageSendParams.message` 是 live inline field。JSON 观测不授予 provider native 权限。

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
