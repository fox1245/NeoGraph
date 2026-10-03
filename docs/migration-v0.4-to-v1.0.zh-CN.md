<!-- neograph-i18n: source=docs/migration-v0.4-to-v1.0.md locale=zh-CN source_sha256=47da3b0da6a1a0469315657c9eb8079b216d0c51eafce0bed090d91fc5c3c7b5 -->
# 迁移指南：旧的 8 个虚函数 → `run(NodeInput)`（v0.4.x → v0.9+）

**Languages:** [English](migration-v0.4-to-v1.0.md) | [한국어](migration-v0.4-to-v1.0.ko.md) | [日本語](migration-v0.4-to-v1.0.ja.md) | [简体中文](migration-v0.4-to-v1.0.zh-CN.md)

NeoGraph v0.4 将节点入口点统一为单个 `run(NodeInput) -> awaitable<NodeOutput>`。
旧的 8 个虚函数（`execute` / `execute_async` / `execute_stream` /
`execute_stream_async` 以及它们的 `_full` 对应版本）在 v0.4.x 中被标记为
弃用，并在 v0.9.0（v1 预备版）中被移除。本文档概述将旧节点迁移到当前
API 的步骤。

> 从 v0.9.0 起，未实现 `run(NodeInput)` 的 C++ 子类将作为抽象类编译失败。
> Python 子类也必须实现 `run(self, input)`。

## 为什么需要迁移

旧模式——`(同步/异步) × (写入/完整) × (流式/非流式)` = 8 个虚函数笛卡尔积。
重写其中任何一个会导致其他 7 个回退到默认链。某些组合是安全的，但存在
运行时陷阱（例如，同步 `execute_full` + 异步分发 → 嵌套 `run_sync` 竞态），
使得用户不清楚应该重写哪个函数。

新模式——单个 `run(NodeInput) -> awaitable<NodeOutput>`。只需重写一个方法。
同步与异步的区别由调用者处理（用户可以在协程内部自由使用 `co_await` 或
纯同步代码）。Command / Send 包含在 `NodeOutput` 中，因此不需要额外的
虚函数。流式回调通过 `NodeInput::stream_cb`（可为空的指针）传入。

## 8 个虚函数 → 新的 `run()` 映射

| 旧虚函数 | 迁移后的形式 |
|---|---|
| `execute(state)` | `NodeOutput out; out.writes = {...}; co_return out;`（同步主体） |
| `execute_async(state)` | 原生异步，如 `co_await provider->invoke_async(std::move(request));` |
| `execute_stream(state, cb)` | `if (in.stream_cb) (*in.stream_cb)(event); co_return NodeOutput{...};` |
| `execute_stream_async(state, cb)` | 上述 + 原生异步（`co_await ...`） |
| `execute_full(state)` | `NodeOutput out; out.writes=...; out.command=...; co_return out;` |
| `execute_full_async(state)` | 上述 + 原生异步 |
| `execute_full_stream(state, cb)` | `execute_full` + 使用 `in.stream_cb` |
| `execute_full_stream_async(state, cb)` | 上述 + 原生异步 |

关键：**8 个变体可以表达为以下组合：填充哪个 `NodeOutput` 字段 + 是否使用
`in.stream_cb` + 是否使用 `co_await`**。仅剩下一个虚函数。

### 最常见的 Python 迁移

**旧代码：**

```python
class CounterNode(ng.GraphNode):
    def execute(self, state):
        current = state.get("count") or 0
        return [ng.ChannelWrite("count", current + 1)]
```

**当前代码：**

```python
class CounterNode(ng.GraphNode):
    def run(self, input):
        current = input.state.get("count") or 0
        return [ng.ChannelWrite("count", current + 1)]
```

Python 的 `run` 是普通的 `def`，而非 `async def`。在流式执行中，
`input.stream_cb` 是接收事件的函数；在普通执行中，它为 `None`。

## 逐例转换示例

### 案例 1 — 最简单的同步节点

**旧：**
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

**新：**
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

差异：
- `state` → `in.state`
- 返回值包装在 `NodeOutput` 中（`writes` 字段）
- 函数为 `asio::awaitable<NodeOutput>` 并以 `co_return` 结尾

### 案例 2 — 异步 LLM 节点（迁移 `execute_async`）

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

### 案例 3 — 流式节点（迁移 `execute_stream`）

公开契约是拥有所有权的 typed 准备/dispatch，而非同步/异步 virtual completion 对。`ProviderRequest.payload` 是 Chat、Messages、Responses、Gemini、Interactions 的 SDK 请求 variant。`ProviderMode::Collect` / `Stream` 独立于观察者是否存在来选择传输。`on_event` 接收借用的 typed `sp::Event` view；只复制回调后仍需要的数据。不允许 raw JSON override 或通过 portable projection 导入 native 权限。

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

### 案例 4 — 使用 Command / Send 的节点（迁移 `execute_full`）

**旧：**
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

**新：**
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

`NodeOutput` 是 `NodeResult` 的别名——旧的 `NodeResult` 代码仍然可以编译。

## 常见错误

### `NodeInput in` 按值传递

```cpp
// ❌ Wrong — coroutine ref-param UAF, SEGV in pybind async path
asio::awaitable<NodeOutput> run(const NodeInput& in) override { ... }

// ✅ Correct
asio::awaitable<NodeOutput> run(NodeInput in) override { ... }
```

原因：协程框架必须为其安全性复制参数。按引用接收会在调用者的栈帧消失后
使 `in.state` 成为悬垂引用。这是在 PR 2 工作中实际发生的错误。

### cancel / store / stream_cb 都来自 `in.ctx`

旧节点通过像 `state.run_cancel_token_` 这样的暗通通道接收取消令牌，
但 v0.4 引入了 `RunContext` 作为正式的数据通道：

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

C++ 节点可用的 `in.ctx` 字段包括：`cancel_token`、`usage`、`thread_id`、
`step`、`stream_mode`、`store`、`resume_value`、`deadline` 和 `trace_id`。
后两个字段由 `RunMetadata` 设置，且引擎会在嵌套 subgraph 中保留它们。检查点
路由是引擎内部实现，不是公开的 `RunContext` 字段。Python 已公开 `trace_id`、
`run_id`、`model_token_budget`、`has_deadline` 和 `deadline_remaining_ms`；原始的
C++ steady-clock截止时间则有意保持不透明。

### 迁移 `_full` 虚函数 — 在一行内以 `co_return out;` 结束

对于旧的 `execute_full` 用户最常见的困惑：
"`NodeResult` 是旧类型，但我必须返回 `NodeOutput` 吗？"
→ 它们是同一类型的别名。只需 `NodeOutput out;
out.writes=...; out.command=...; out.sends=...; co_return out;`。

## 如果不迁移会发生什么

从 v0.9.0 起，旧的 8 个虚函数已被移除。

- C++ 旧的 `override` 会产生编译错误，如 `'execute' marked
  override but does not override`。
- 仅实现 `execute()` 的 Python 节点会引发 `NotImplementedError`，
  要求实现 `run(input)`。

不要使用保留旧方法名的过渡模式。引擎只调用 `run(NodeInput)`，因此旧
方法体永远不会执行。

## 有没有批量迁移脚本？

没有——虚函数签名在 8 种形式中各不相同，使得基于正则表达式的转换不切实际。
用户应阅读逐例示例（以上 4 个示例）并手动迁移。

对于最常见的模式（仅重写 `execute(state)`），以下 sed/awk 一行命令可能
对初始遍历有帮助——需要人工审查：

```bash
# Very rough initial pass — nodes with single-line execute override only.
# Always dry-run without -i first.
grep -lE 'execute\(const GraphState' src/**/*.cpp
# Manually edit each resulting file to the new pattern.
```

复杂节点（`execute_full`、`execute_stream_async` 等）必须手动编辑。
没有捷径。

---

# 迁移 2：typed lossless Provider 切换（必须重新编译）

这是源码和二进制破坏性变更；所有 C++ 使用者与自定义提供方都必须使用匹配的新头文件/库重新编译。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、descriptor interpreter 和 Responses WebSocket 已删除，没有 alias 或兼容 bridge。SDK 为不稳定 `0.0.0`、interface revision 3 / shared ABI 3，使用 out-of-line capability check，不表示稳定发布。当前 runtime/archive 为 Linux/POSIX，不代表 Windows、macOS、WASM runtime 已获验证。Python provider binding/wrapper 已延期，不由本 C++ 变更完成移植。

Fresh installed find_package Program C++/C ABI/dualQuickJS consumer 与 NeoGraph/SchemaProvider typed2-request lifetime/native/raw/mismatch consumer pass。Interface/ABI 声明本身不同于实际 package 结果；不声称更广 platform 或稳定 release。

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


提供方调用返回 `sp::runtime::Result`，即持有 `sp::Completion` 或 `sp::Failure` 的不可变、拥有所有权的 `std::shared_ptr<const sp::Outcome>`。请保留完整结果，而非仅显示文本。顺序消息/part、native continuation、完整 wire envelope、顺序 raw 观测、停止依据及真实尝试元数据在调用与客户端销毁后仍然保留。使用量是带依据、阶段、质量的 nullable `uint64_t`；缺失表示未知，绝不是零。失败保留原始部分结果。`ProviderFailure::outcome()` 与 `ProviderObserverError::outcome()` 保留真实结果，后者的 `cause()` 也保留观察者异常。

实际结果存在后，若 post-effect 结算或 terminal receipt 持久化失败，`ProviderDispatchOutcomePersistenceError::outcome()` 保留原始不可变结果，`cause()` 保留原始持久化异常。若 delivery 也失败，`delivery_error()` 保留原始观察者异常。持久化成功后的观察者失败原样重新抛出原异常；未知/无结果 transport 失败不会伪造 outcome。
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

### Native 历史 / 预算

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

Stage 3（2026-04）设计及当时实测测试数量作为历史保留。provider 兼容/crossover 决策已由下方 typed lossless 切换取代；旧设计记录不是当前 provider API。

- [ABI_POLICY.md](ABI_POLICY.md)
- [ASYNC_GUIDE.md](ASYNC_GUIDE.md)
- [Issue #5](https://github.com/fox1245/NeoGraph/issues/5) — historical decision; superseded provider compatibility policy.

---

# 迁移 3：`compile()` 工作池默认为 1（v0.1.4 回归恢复）

## 变更了什么

`GraphEngine::compile(def, ctx)` 默认工作线程数从 v0.1.4（`b59444f`）起
为 `std::thread::hardware_concurrency()`，但在 v1.0 中恢复为
**`1`（= 无引擎持有的 thread_pool）**。

## 为什么

`hardware_concurrency` 默认值对所有扇出节点施加跨线程提交开销
（~6–7 µs/任务）——bench par 测量（5 个工作器 + summarizer）从 11.6 µs
退化为 44 µs，减慢 4 倍。在我们的测量环境中二分法精确定位到 v0.1.4 的
`b59444f` 为罪魁祸首。

真实生产工作负载（毫秒至秒级的 LLM 调用）可以忽略提交开销，但是：
- **简单图（无扇出）** 仍然支付池开销——无意义
- **非线程安全的节点状态** 默认暴露给多工作器——这是一颗真正的陷阱地雷

因此，默认值安全地设为 1，用户必须显式加入才能实现真正的扇出并行化。

## 迁移

需要扇出并行化的图（例如多个 `Send` 分发、`parallel_group`、deep_research
的 5 研究者扇出）必须在 `compile()` 之后显式调用：

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

简单图（无扇出）或轻型扇出图（LLM 调用占主导）保持默认——零池开销。

## 如果不迁移会发生什么

- 有扇出的用户图将在单线程上串行执行（一致性保证）
- 实际的挂钟恢复无法实现——需要显式的 `set_worker_count_auto()`

## 受影响的 NeoGraph 内部示例

与此变更一同添加的扇出可见性补丁——添加了显式调用来保持意图。如果您的
用户代码匹配，请应用相同模式：

- `examples/10_send_command.cpp` — 同步 `sleep_for` ResearcherNode 通过
  Send 扇出，添加了 `engine->set_worker_count_auto()`
- `examples/14_plan_executor.cpp` — 5 个子主题 Send 扇出（同步 sleep_for），
  同样添加
- `examples/21_mcp_fanout.cpp` — 3 个 MCP 工具调用同时触发，同样添加
- `examples/36_classifier_fanout.cpp` — 已有 `set_worker_count(5)` 显式
  调用。修正了声明错误默认值的注释（当前默认值为 hardware_concurrency）
- `src/core/deep_research_graph.cpp` `create_deep_research_graph()` 构建器 —
  在 `compile()` 之后立即调用 `set_worker_count_auto()`，使 supervisor 的
  N 个研究者真正并发运行

`examples/05_parallel_fanout.cpp` 在 `io_context` 上使用协程定时器重叠
（无同步 sleep），因此工作池没有效果——保持不变。

如果用户代码中存在相同模式：

```cpp
auto engine = GraphEngine::compile(def, ctx);
engine->set_worker_count_auto();   // ← add this line
```

详见 ROADMAP_v1.md 性能部分的测量数据（单独添加）。

> Stage 3（2026-04）设计及当时实测测试数量作为历史保留。provider 兼容/crossover 决策已由下方 typed lossless 切换取代；旧设计记录不是当前 provider API。

---

# 迁移 4：C++ ABI 与强制重新构建

NeoGraph 现在为所有公开二进制库设置项目 `VERSION` 和主版本
`SOVERSION`。v1 之前都使用 ABI 代次 0，但不保证各 `0.x` 版本之间的
二进制兼容性。changelog 公布边界时，必须重新构建所有 C++ 使用者。特别是
从 `0.11.1` 或更早版本升级到包含 bounded `NodeCache` 的版本时，
`NodeCache` 和 `EngineConfig` 对象布局已经改变，因此必须重新构建。

上面的 Provider 迁移不会改变现有 `Provider` vtable。未来的
`CheckpointStore` 异步迁移也必须遵守同一策略；v1 之后应优先增加独立能力
接口和适配器，而不是修改稳定布局。

平台库名称、已知边界和 CI 验证方法请参阅
[二进制兼容性策略](ABI_POLICY.md)。
