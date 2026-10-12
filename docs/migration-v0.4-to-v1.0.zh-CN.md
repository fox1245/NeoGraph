<!-- neograph-i18n: source=docs/migration-v0.4-to-v1.0.md locale=zh-CN source_sha256=0a1f90cdd5b2c7cf3e672239c5237d10e7d011fbeb5bf6b002227a29889a0220 -->
# 迁移指南：旧的 8 个虚函数 → `run(NodeInput)`（v0.4.x → v0.9+）

**Languages:** [English](migration-v0.4-to-v1.0.md) | [한국어](migration-v0.4-to-v1.0.ko.md) | [日本語](migration-v0.4-to-v1.0.ja.md) | [简体中文](migration-v0.4-to-v1.0.zh-CN.md)

NeoGraph v0.4 把节点入口统一成了唯一的 `run(NodeInput) -> awaitable<NodeOutput>`。旧的 8 个虚函数（`execute`、`execute_async`、`execute_stream`、`execute_stream_async`，以及它们对应的 `_full` 版本）在 v0.4.x 中被弃用，并在 v1 的准备版本 v0.9.0 中移除。本文说明如何把旧节点迁移到当前的 API。

> 从 v0.9.0 起，没有实现 `run(NodeInput)` 的 C++ 子类，会因为是抽象类而无法编译。Python 子类同样必须实现 `run(self, input)`。

事件驱动的 Provider 分发另外增加了一个 v1 之前必须重新构建的边界：`CancelToken` 现在使用 `std::stop_source`，并提供 `stop_token()`，供与 executor 无关的原生订阅使用。现有的 `cancel()`、`is_cancelled()`、`fork()`、`bind_executor()` 和 `slot()` 调用点在源码层面仍然兼容，但旧的内联实现在二进制层面不再兼容。请用匹配的 NeoGraph 头文件和库，重新编译每一个 C++ 使用方和扩展；只替换共享库是不够的。Provider 的通知机制保留了 SDK 的 `join()`、先进先出的、持有所有权的 outcome、绝对截止时间，以及准入(admission)/权限预算。参见 [ABI 策略](ABI_POLICY.md)和[对应的测量数据](../benchmarks/provider-notification-summary.json)。

## 为什么要迁移

旧模式：`(sync/async) × (writes/full) × (stream/non-stream)`，一共是 8 个虚函数的组合。重写其中任意一个，其余 7 个都会回退到默认的调用链。有些组合是安全的，但运行时有陷阱（例如同步的 `execute_full` 加异步分发，会造成嵌套 `run_sync` 的竞态），用户也就搞不清到底该重写哪一个函数。

新模式：只有一个 `run(NodeInput) -> awaitable<NodeOutput>`，只需重写它。同步还是异步，是调用方要关心的事（用户可以在协程里随意使用 `co_await`，也可以写普通的同步代码）。Command 和 Send 都包含在 `NodeOutput` 里，不再需要额外的虚函数。流式回调通过 `NodeInput::stream_cb`（一个可以为空的指针）传入。

## 8 个虚函数与新 `run()` 的对应关系

| 旧的虚函数 | 迁移后的写法 |
|---|---|
| `execute(state)` | `NodeOutput out; out.writes = {...}; co_return out;`（同步函数体） |
| `execute_async(state)` | 原生异步，例如 `co_await provider->invoke_async(std::move(request));` |
| `execute_stream(state, cb)` | `if (in.stream_cb) (*in.stream_cb)(event); co_return NodeOutput{...};` |
| `execute_stream_async(state, cb)` | 上面的写法，再加原生异步（`co_await ...`） |
| `execute_full(state)` | `NodeOutput out; out.writes=...; out.command=...; co_return out;` |
| `execute_full_async(state)` | 上面的写法，再加原生异步 |
| `execute_full_stream(state, cb)` | `execute_full` 的写法，再用上 `in.stream_cb` |
| `execute_full_stream_async(state, cb)` | 上面的写法，再加原生异步 |

关键在于：**这 8 种变体，都可以用三件事的组合来表达：填充了 `NodeOutput` 的哪些字段、是否使用 `in.stream_cb`、是否使用 `co_await`**。最终只剩下一个虚函数。

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

Python 的 `run` 是普通的 `def`，不是 `async def`。流式执行时，`input.stream_cb` 是接收事件的函数；普通执行时，它是 `None`。

## 逐个场景的转换示例

### 场景 1：最简单的同步节点

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
- 返回值包装进 `NodeOutput`（`writes` 字段）
- 函数返回 `asio::awaitable<NodeOutput>`，并以 `co_return` 结束

### 场景 2：异步 LLM 节点（迁移 `execute_async`）

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

### 场景 3：流式节点（迁移 `execute_stream`）

公开的契约是持有所有权的带类型准备与分发，而不是成对的 completion 虚方法。`ProviderRequest.payload` 是 SDK 中 Chat、Messages、Responses、Gemini 或 Interactions 请求的 variant。`ProviderMode::Collect` / `Stream` 选择传输方式，与 observer 无关。`on_event` 收到的是借用的、带类型的 `sp::Event` 视图；只复制回调之后还需要的数据。不允许覆盖原始 JSON，也不允许通过可移植投影导入 native 状态。

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

### 场景 4：使用 Command / Send 的节点（迁移 `execute_full`）

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

`NodeOutput` 是 `NodeResult` 的别名，所以旧的 `NodeResult` 代码仍然可以编译。

## 常见错误

### `NodeInput in` 必须按值传递

```cpp
// ❌ Wrong — coroutine ref-param UAF, SEGV in pybind async path
asio::awaitable<NodeOutput> run(const NodeInput& in) override { ... }

// ✅ Correct
asio::awaitable<NodeOutput> run(NodeInput in) override { ... }
```

原因：为了安全，协程帧必须持有参数的副本。按引用接收的话，调用方的栈帧消失之后，`in.state` 就会悬空。这是 PR 2 开发期间真实出现过的 bug。

### cancel、store、stream_cb 都来自 `in.ctx`

旧节点是通过 `state.run_cancel_token_` 这类偷偷夹带的通道拿到取消令牌的，v0.4 则引入 `RunContext` 作为正式的传递途径：

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

C++ 节点可以使用的 `in.ctx` 字段有：`cancel_token`、`usage`、`thread_id`、`step`、`stream_mode`、`store`、`resume_value`、`deadline` 和 `trace_id`。后两者要在 `RunMetadata` 上设置；引擎会让它们穿过嵌套的子图保持不变。检查点的路由仍然是引擎内部的事，不是 `RunContext` 的公开字段。Python 提供 `trace_id`、`run_id`、`model_token_budget`、`has_deadline` 和 `deadline_remaining_ms`；C++ 的 steady-clock 原始截止时间有意保持不透明。

### 迁移 `_full` 虚函数：用一行 `co_return out;` 收尾

旧的 `execute_full` 用户最常见的困惑是：
“`NodeResult` 是旧类型，那我必须返回 `NodeOutput` 吗？”
答案是：它们是同一个类型的别名。直接写 `NodeOutput out;
out.writes=...; out.command=...; out.sends=...; co_return out;` 即可。

## 不迁移会怎样

从 v0.9.0 起，旧的 8 个虚函数已经不存在。

- C++ 中旧的 `override` 会产生 `'execute' marked
  override but does not override` 这样的编译错误。
- 只实现了 `execute()` 的 Python 节点，会抛出 `NotImplementedError`，
  要求实现 `run(input)`。

不要采用保留旧方法名的过渡写法。引擎只调用 `run(NodeInput)`，所以旧的函数体永远不会被执行。

## 有没有批量迁移脚本？

没有——这些虚函数的签名有 8 种形式，用正则表达式转换不现实。请阅读上面按场景给出的示例（4 个示例），手动迁移。

对于最常见的写法（只重写 `execute(state)`），下面这个 sed/awk 单行命令可以帮助完成第一遍转换，但必须人工复查：

```bash
# Very rough initial pass — nodes with single-line execute override only.
# Always dry-run without -i first.
grep -lE 'execute\(const GraphState' src/**/*.cpp
# Manually edit each resulting file to the new pattern.
```

复杂的节点（`execute_full`、`execute_stream_async` 等）必须手动修改，没有捷径。

---

# 迁移 2：带类型的无损 Provider 切换（必须重新编译）

这是一次源码和二进制层面的不兼容变更：每个 C++ 使用方和自定义 provider，都要用匹配的新头文件和库重新编译。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、描述符解释器和 Responses WebSocket 路径都已移除，没有别名，也没有兼容桥。SDK 是 alpha `0.3.0`，接口修订版 6 / 共享库 ABI 6，并带有非内联的 capability 检查；这不表示它已是稳定版本。

请把对已移除的 `Provider::complete`、`complete_async`、`complete_stream` 和 `complete_stream_async` 的调用，替换成显式指定模式的请求，加上 `invoke` / `dispatch`（或它们对应的 C++ 异步版本）。`Agent::complete` 仍是一个独立的单轮 API，返回持有所有权的 outcome。

需要 CMake 3.20 或更新版本。即使设置了 `NEOGRAPH_BUILD_LLM=OFF`，SchemaProvider 运行时仍然是必需的，因为 Core 导出了持有所有权的带类型 provider 契约。显式指定的 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` 优先；否则 CMake 优先使用已安装的 `SchemaProvider` 运行时包，再其次，在 `NEOGRAPH_FETCH_SCHEMAPROVIDER=ON`（默认）时，下载由 `cmake/NeoGraphSchemaProvider.cmake` 固定的、不可变的 GitHub 归档。离线构建时，先安装 SDK，把 `CMAKE_PREFIX_PATH` 设为它的前缀，并传入 `-DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`。CMake 不会去猜同级目录里的检出，也不会选用已被移除的内置解释器。即使 NeoGraph 可选的 HTTP 模块被禁用，SDK 运行时仍然需要它自己的传输层依赖。SDK 运行时无法用于 WebAssembly 构建。

Python 暴露的所有权式 request/outcome 边界与 C++ 相同：`make_provider_request`、`Provider.prepare`、`dispatch` 和 `invoke`。provider 的历史请使用带类型化部分（typed parts）的 `ProviderMessage`；`ChatMessage` 仍只是方便图使用的投影。SDK 返回的失败可以通过 `ProviderOutcome.failure` 读取，而宿主的 observer 或结算（settlement）抛出的异常会保留 `outcome` 和 `cause`。构造函数以及 GIL 和回调的行为，请看 [Python 绑定指南](python-binding.md)。

公开的契约是持有所有权的带类型准备与分发，而不是成对的 completion 虚方法。`ProviderRequest.payload` 是 SDK 中 Chat、Messages、Responses、Gemini 或 Interactions 请求的 variant。`ProviderMode::Collect` / `Stream` 选择传输方式，与 observer 无关。`on_event` 收到的是借用的、带类型的 `sp::Event` 视图；只复制回调之后还需要的数据。不允许覆盖原始 JSON，也不允许通过可移植投影导入 native 状态。

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

每次调用所需的控制项是带类型的字段，而不是 `extra_fields` 字典：

| 请求族 | 额外的 `ProviderControls` |
|---|---|
| Chat | `chat_reasoning`、`include_reasoning`、`usage_include`、`models`；这些要求声明了 OpenRouter origin。标量的 `reasoning_effort` 是另外一项。 |
| Responses | `previous_response_id`、`previous_response_history`、`parallel_tool_calls`、`verbosity`、`truncation`、`responses_include`。 |
| Messages | `thinking_mode`、`output_effort`、`cache_control`、`messages_tool_choice`，以及声明了 origin 的 OpenRouter `provider` 路由。 |
| Gemini Generate | `gemini_history_mode`、`gemini_thinking_level`、`temperature`、`safety_settings`、`gemini_tool_choice`。 |

Messages 的手动（manual）thinking，要求一个经策略准入、且低于输出上限的 budget；自适应（adaptive）和禁用（disabled）模式不允许设置 thinking budget。手动和自适应的 thinking 会省略合法的 temperature，但非法的值以及模型禁止的 temperature，会在省略之前就被拒绝。禁用 thinking 时，会带上已准入的 temperature。按模型名前缀施加的限制，属于已准入的策略事实，按 ASCII 不区分大小写，对照完整的模型名或其最后一个 `/` 之后的后缀进行匹配。Gemini 的 thinking level 与 thinking budget 互斥；它带类型的工具选择与 `required_tool` 也互斥。不支持的请求族/origin/取值组合，会在 I/O 之前被拒绝。

### PreparedProviderRequest / ProviderBudgetClaim
`prepare()` 只会校验并编码一次，产出一个只能移动的 `PreparedProviderRequest`，它带有原始的截止时间和取消状态。需要持久化的调用方，先把它的 `Provider::request_digest()` 绑定到自己的装配结果，预留一笔已准入的预算额度，写入分发回执，然后通过 `ControlledProvider::dispatch_prepared(_async)` 消耗同一个句柄。过了闸门之后，绝不重建请求。重复的回执不会重新分发。自定义 provider 实现 `get_name()`、`family()` 和 `prepare()`，其中使用 `prepare_runtime()` 或 `prepare_local()`；本地回调捕获的是持有所有权的共享状态，而不是 `this`。

可选的 `ProviderControls` 是调用方的选择，不是强制的默认值，也不是被悄悄截断的上限。请求族不支持的控制项，会在分发之前失败。有界的调用需要真实的、已准入的模型输入/输出事实；缺少这些事实时，会以 `LimitUnknown` 失败。预留是保守的消费额度，不是上报的用量、预测值或发票。未知、部分或送达状态未知的 outcome，会一直保留它们的冻结额度（hold）；真实的最终报告会结清它，超额的用量也一样。重试是一个显式的层，默认关闭，有时间窗口的上限，并带有“先前状态未知”的冻结；不会有隐藏的重发。


provider 调用返回 `sp::runtime::Result`：它是不可变的、持有所有权的 `std::shared_ptr<const sp::Outcome>`，其中包含 `sp::Completion` 或 `sp::Failure`。请保留完整的 outcome，而不只是用于显示的文本。有序的消息/parts、保留下来的 native 续接状态和请求族提供的 wire 证据、有序的原始观测、停止证据和真实的尝试元数据，在调用结束和客户端销毁之后依然存在。`input_total`、`output_total` 和 `total` 等用量计数器的类型是 `std::optional<sp::Count>`；有值的计数带有 `uint64_t value` 和 `Evidence`。`Usage` 还会记录 stage、quality 和 conflicts。缺失表示未知，绝不会当成 0。失败会保留它原来的部分 outcome。`ProviderFailure::outcome()` 和 `ProviderObserverError::outcome()` 保留这份结果；后者还会在 `cause()` 中保留 observer 的异常。

wire 证据由请求族提供，而且是可选的：`sp::Completion::wire_envelope` 可能为空（在 Python 中，`ProviderCompletion.wire_envelope` 为 `None`）。带缓冲的 Chat 目前把完整的响应 JSON 作为 `RawWire` 保存在 `raw_events` 里，其 `type == "chat.completion"`，文档内容在 `payload` 中；`wire_envelope` 则保持为空。请到请求族保留证据的地方去读取；不会伪造一个兜底的 envelope。native 续接状态和原始缓冲区仍然是受保护的证据，不会出现在 trace 的 payload 里。

`UsageAccumulator::snapshot()` 返回累计的报告。`total_tokens_wide()` 返回已计费的 token 数加上尚未结清的预留量，不能把它当作上报的用量显示出来。结算要求报告是最终的、前后一致的，且带有 input 和 output 计数；结算按有依据的最大总量计费，超出的用量也不做截断。只要累计的某份报告里缺少某个计数器，汇总后该计数器仍然是未知。预留、本地计费和 vendor 的发票是不同的记录。

如果在已有真实结果之后，副作用后的核算或终态回执的持久化失败了，`ProviderDispatchOutcomePersistenceError` 会在 `outcome()` 中保留原来的不可变结果，在 `cause()` 中保留原来的持久化异常。如果交付也失败了，`delivery_error()` 会保留原来的 observer 异常。持久化成功、随后 observer 失败时，会原样重新抛出那个原始的 observer 异常；状态未知、没有结果的传输失败，不会伪造 outcome。
### SchemaProvider

`SchemaProvider` 接受已准入的 `sp::descriptor::ValidatedDescriptor`、`sp::runtime::Options` 和可选的 `SchemaProvider::Defaults`。加载描述符是对封闭且带版本的数据做准入，不是请求/响应解释器，也不是任意原语的注册表。凭据应放在运行时选项里，而不是公开的描述符文件里。Defaults 只包含带类型的 OpenRouter 路由和 Responses 的留存（`responses_store`）；后者只对 Responses 有效。托管的 OpenRouter 路由、留存和 JSON 格式，仍然是需要声明的、带类型的控制项。Images、Veo 和 Decisions 使用各自独立的 NeoGraph 带类型客户端，并有各自独立的授权；它们不继承 SDK 的 chat 授权。

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

直接调用 `sp::descriptor::load` 时，header 按字面处理：`${VAR}` 不会展开。如果要在准入之前，由宿主一侧显式做预处理，请使用 `sp::descriptor::load_with_environment_headers(source, overrides, policy)`，或者配合 `DeploymentHeaderEnvironment` 使用结果确定的 `load_with_deployment_headers(source, overrides, environment, policy)`。环境变量辅助函数会为 Messages 读取可选的 `ANTHROPIC_WORKSPACE_ID` / `ANTHROPIC_BETA`；未设置或为空的值会被省略。描述符中写死的 header 优先于环境变量的值，显式的覆盖项又优先于前两者，且名称不区分大小写。重复的覆盖项、无效或保留的 header，会使最终的准入失败。没有任何编码器会求值模板，或修改已准入的描述符。

### Native 历史

检查点只会在生成它的进程内，以 C++ sidecar 的形式保存 provider 的 native 历史（推理内容、签名、有序的工具调用）。`ChatMessage`、`ChatTool` 和 JSON 是可移植的副本，不具备 native 权限。需要在重启后保留 native 历史的宿主，要配置一个 `sp::NativeArchive`。这些契约见 [typed provider reference](reference-en.md#portable-projections)。

另见：
- [ABI_POLICY.md](ABI_POLICY.md)
- [ASYNC_GUIDE.md](ASYNC_GUIDE.md)

---

# 迁移 3：`compile()` 的 worker 线程池默认值为 1（恢复 v0.1.4 引入的回归）

## 变更内容

`GraphEngine::compile(def, ctx)` 的默认 worker 数量，自 v0.1.4（`b59444f`）起是 `std::thread::hardware_concurrency()`，在当前 v1 之前的 API 中恢复为 **`1`（即没有引擎自己的 thread_pool）**。

## 原因

默认值取 `hardware_concurrency` 时，所有 fan-out 节点都要付出跨线程提交的开销（约 6-7 µs/任务）。bench 的并行测量（5 个 worker 加一个 summarizer）从 11.6 µs 退化到 44 µs，慢了 4 倍。

真实的生产负载（毫秒到秒量级的 LLM 调用）可以忽略提交开销，但是：
- **简单的图（没有 fan-out）** 也要为线程池付出开销，这毫无意义
- **非线程安全的节点状态** 默认就暴露给了多个 worker，这是一个真实的陷阱（footgun）

因此，默认值稳妥地设为 1，需要真正的 fan-out 并行时，用户必须显式选用。

## 迁移

需要 fan-out 并行的图（例如多个 `Send` 分发、`parallel_group`、deep_research 的 5 个 researcher 的 fan-out），必须在 `compile()` 之后显式调用：

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

简单的图（没有 fan-out），或者以 LLM 调用为主的轻量 fan-out 图，保持默认值即可，线程池开销为 0。

## 不迁移会怎样

没有选用 worker 线程池时，CPU 密集的 fan-out 在调用方的 executor 上运行；异步 I/O 仍然可以重叠。需要独立的执行线程时，使用 `set_worker_count_auto()` 或显式指定 worker 数量。光有 worker 数量，既不保证一致性，也不保证加速。

---

# 迁移 4：C++ ABI 与必须重新构建的边界

NeoGraph 现在给每个已编译的公共库设置项目的 `VERSION` 和主版本号 `SOVERSION`。v1 之前的所有发布都使用 ABI 第 0 代，但 `0.x` 的二进制兼容性不受保证。变更日志宣布某个边界时，必须重新构建每个 C++ 使用方。特别是，从 `0.11.1` 或更早的版本升级到包含有界 `NodeCache` 的版本时，由于 `NodeCache` 和 `EngineConfig` 的对象布局发生了变化，必须重新构建。

带类型的 Provider 迁移，把它的虚函数契约改成了 `get_name`、`family` 和 `prepare`，并移除了旧的 completion 虚函数。已有的 Provider 二进制不再兼容；请用匹配的头文件和库，重新构建自定义 provider 和每个依赖它们的 C++ 使用方。Core 也暴露了 SchemaProvider 的类型，所以即使禁用了 LLM，构建仍然需要 SDK 运行时。将来稳定的接口，应当优先采用独立的 capability 接口加适配器，而不是修改已稳定的内存布局；当前 v1 之前的接口不承诺二进制兼容。

平台上的库名称、所有已知的必须重新构建的边界，以及 CI 验证，见[二进制兼容性策略](ABI_POLICY.md)。
