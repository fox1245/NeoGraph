<!-- neograph-i18n: source=docs/migration-v0.4-to-v1.0.md locale=zh-CN source_sha256=9cf72b62901f60584777cbb139275d7fd8c1449f797cd9b7d68b2e1b05be1e56 -->
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

这是一次源码和二进制层面的不兼容变更：每个 C++ 使用方和自定义 provider，都要用匹配的新头文件和库重新编译。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、描述符解释器和 Responses WebSocket 路径都已移除，没有别名，也没有兼容桥。SDK 是 alpha `0.1.1`，接口修订版 4 / 共享库 ABI 4，并带有非内联的 capability 检查；这不表示它已是稳定版本。记录在案的接口 3 的 SDK 运行时和 archive 资格认证覆盖 Linux/POSIX，并不覆盖接口 4。Windows 的 NTFS 实现和 macOS 实现已经存在，但要在新平台上获得资格认证，还需要运行时证据；WASM 上的 provider 运行时资格认证尚未确立。

请把对已移除的 `Provider::complete`、`complete_async`、`complete_stream` 和 `complete_stream_async` 的调用，替换成显式指定模式的请求，加上 `invoke` / `dispatch`（或它们对应的 C++ 异步版本）。`Agent::complete` 仍是一个独立的单轮 API，返回持有所有权的 outcome。

需要 CMake 3.20 或更新版本。即使设置了 `NEOGRAPH_BUILD_LLM=OFF`，SchemaProvider 运行时仍然是必需的，因为 Core 导出了持有所有权的带类型 provider 契约。显式指定的 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` 优先；否则 CMake 优先使用已安装的 `SchemaProvider` 运行时包，再其次，在 `NEOGRAPH_FETCH_SCHEMAPROVIDER=ON`（默认）时，下载由 `cmake/NeoGraphSchemaProvider.cmake` 固定的、不可变的 GitHub 归档。离线构建时，先安装 SDK，把 `CMAKE_PREFIX_PATH` 设为它的前缀，并传入 `-DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`。CMake 不会去猜同级目录里的检出，也不会选用已被移除的内置解释器。即使 NeoGraph 可选的 HTTP 模块被禁用，SDK 运行时仍然需要它自己的传输层依赖。记录在案的 SDK 运行时和 archive 资格认证覆盖 Linux/POSIX。Windows 的 NTFS 实现和 macOS 实现已经存在，但要在新平台上获得资格认证，还需要运行时证据；WASM 上的 provider 运行时资格认证尚未确立。

Python 暴露的所有权式 request/outcome 边界与 C++ 相同：`make_provider_request`、`Provider.prepare`、`dispatch` 和 `invoke`。provider 的历史请使用带类型化部分（typed parts）的 `ProviderMessage`；`ChatMessage` 仍只是方便图使用的投影。SDK 返回的失败可以通过 `ProviderOutcome.failure` 读取，而宿主的 observer 或结算（settlement）抛出的异常会保留 `outcome` 和 `cause`。构造函数以及 GIL 和回调的行为，请看 [Python 绑定指南](python-binding.md)。

在接口 4 之前，已安装的 find_package Program C++/C ABI/双 QuickJS 使用方，以及 NeoGraph/SchemaProvider 的带类型双请求生命周期/native/raw/不匹配（mismatch）使用方，都通过了测试。这些是历史上的包结果，不是接口 4 的通过声明；更广的平台和稳定版本都不在声明之列。

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

接口 4 通过带类型的字段，而不是 `extra_fields` 字典，保留了每次调用所需的控制项：

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

### Native 历史与预算

`ChatMessage` / `ChatTool` 和 JSON 只是可移植的投影（portable projection），并不具备 native 权限。可移植格式仍然是 [`provider-message-v2`](../schemas/provider-message-v2.schema.json) 和 [`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json)。真正的 C++ 检查点 sidecar 把 native seal 保存在内存里。要持久保存 native 历史，需要由宿主持有的 `sp::NativeArchive`：封闭的 v3 / `spna3` 格式，带经过认证的、归 owner 私有的托管（custody），并使用独立的密钥。archive v2 会被拒绝，既不会升级，也不会解释。认证绑定的内容包括：描述符里每一项语义选择（origin、路径、header、policy、请求字段映射、usage 路径和 stop 映射）、owner，以及确切的托管绑定。它既不是加密，也不是对 vendor 签发方的认证；不要公开 archive 的正文、密钥、native blob 或原始的 wire 观测数据。archive 只是证据存储，不是资金 grant，也不是消费租约（spending lease）。Program 和外部的 bank 仍由各自独立的 journal 持有；复制快照不能凭空产生额度（credit）。

接口 4 只把输出生成上限从 native replay 的配置摘要中去掉了。内容、origin、路由、policy 标识、前缀、工具和 reasoning 控制项仍然保持绑定。实际的上限仍然会进入编码后的请求和已准备请求的摘要。每一次上限更大的语义调用，都需要重新通过资源 bank 的准入，拥有自己的确定性调用序号/副作用标识，并沿用原来的截止时间；它不能复用已经结清的调用槽位，不能续期额度，不能修复 seal，也不能重发状态不确定的副作用。archive v3 和可移植 JSON v2 都不变；旧 policy 下的 native archive 仍然绑定在那个旧 policy 上，在 policy 不匹配时会被拒绝，而不会被迁移。

走同一条路由的 native 续接，与显式的外来投影，遵循不同的契约。Gemini 默认使用 `sp::gemini::HistoryMode::NativeOnly`；`PortableForeign` 接受调用方自己构造的、没有 native seal、wire 输出和签名的 assistant Text/ToolCall 历史。每个外来 assistant 轮次中，只有第一个 function call 会带上 `skip_thought_signature_validator`；纯文本的历史不带签名。真实的 native 分组仍然要经过严格校验，校验失败或不匹配的 seal，绝不会剥离，也不会降级成可移植历史。Responses 的 `previous_response_id` 选定由 provider 保存的状态：在 `messages` 里只发送新的输入。`previous_response_history` 是本地的所有权证据，从不会发送出去；当客户端工具的所有权需要它时，请提供完整的原始前缀，以及 ID 等于该游标的、真实的终止 assistant。之后在进程内产生的游标结果，保留私有的已完成所有权，而不具备完整的 `NativeReplay` 或 archive 权限。游标不是 native archive，也不是可移植导入的授权。

**独立 bank journal 的修正：当前契约已修订，实际运行的证据见下文。** 经 owner 批准的协议要求：有一个单调的、受信存储命名空间义务（obligation），以及真实且不可变的原始 owner/thread/graph 作用域、上限（ceiling）、截止时间/时钟标识和 generation。只有对完整检查点承诺（commitment）和 revision 做精确的持久 head CAS，才能签发由宿主持有的不透明租约（lease）。精确的 pending effect 窗口必须在 provider I/O 之前持久化；结算必须使用真实的 SDK outcome 和实际计费，以及可空的 report、hold 和去重标识。检查点和下一个 head 必须在同一个 owned actor/revision 下原子发布。删除 bank 元数据、裁剪（prune）检查点、重放旧的已认证快照、覆盖同一个 ID 或丢失 actor，都不能产生额度（credit）。已有 65 的 hold 时，把上限 130 收紧到 129，不能再准入另一个 65；已证明没有副作用的失败，可以释放保持不变的 head，使真实的 130 恢复仍能进行。崩溃、未知、租约丢失的窗口一直处于 hold 状态，不退款、不重试、不降级（fallback）。普通/原始（plain/pristine）的 archive 配置不授予资金或 native 消费租约，当前的 `config.usage` 也不能替换已有的独立义务；Program 和外部 bank 的 journal 所有权不变。这是必须满足的契约；实际的币种/托管证据和测量手段的局限见下文，它不是稳定发布的 API 保证。

**当前声明；集成运行的证据见下文：** `<neograph/graph/checkpoint.h>` 声明了 `ManagedBudgetLeaseScope`，包含 `owner_scope`、逻辑 `thread_id`、私有的后端 `storage_thread_id`、`graph_identity`、`original_ceiling`、`original_deadline_ticks` 和 `deadline_clock_identity`。`OwnedManagedBudgetLease` 提供只读的 `scope()`、`actor_id()`、不可变的 `bank_generation()`、`revision()`、`head_checkpoint_id()` 和 `head_commitment()`；它没有可导入权限的公开构造函数。`ManagedBudgetEffectReceipt` 提供 `active()`、`effect_id()`、`claim_amount()` 和 `request_digest()`；默认构造的 receipt 不授予任何东西。`CheckpointStore` 声明了 `acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`、`begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`、`settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`、`publish_managed_budget_checkpoint(lease, checkpoint)` 和 `release_managed_budget_lease(lease)`，以及对应的 `_async` 版本。同步的 `CheckpointStoreCore` 和 `AsyncCheckpointStore` 各自提供相应的变体。`managed_budget_checkpoint_commitment(checkpoint)` 覆盖完整的持久检查点，而不只是 bank 的 JSON。这些声明本身并不能证明后端的 CAS、币种安全、已安装 ABI 的兼容性，或某条运行路径已被成功演练。

**真实的 InMemory shared-bank fork 已保留并经过验证。** 最初那个真实的 C++ fork 使用唯一一份原始的 financial journal 和受信的当前分支 head，而不是复制出来的 grant。`publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)`（及 `_async`）要求提供真实的当前源及其完整 commitment，以及指向同一个 bank 的实际 native C++ 指针；持久化的独立 fork 仍明确不支持。`OwnedManagedBudgetLease::scope()` 以及原始 owner/thread/graph、上限、截止时间/时钟和 generation 保持不可变。只读的、由存储签发的 `execution_thread_id()` / `execution_storage_thread_id()` 单独指定执行分支；`GraphState::budget_original_thread_id()` 标识原始的 financial bank。对所选分支 head 做精确 CAS，再加全局的 actor/revision，使所有分支针对规范的当前计数器、pending effect 和已作废（burned）的标识串行化。原分支和 fork 分支都保持可用，无需补充额度；过期的快照、复制的检查点和导入的 JSON 都不能产生别名（alias）或回退 head。原有的 root30 → charge3 → original continuation6 → fork lower20 → continuation9 同一 bank 的验证，在未改动的 test_graph_engine.cpp:810–913 中通过（PASSED）；保存的原始上限 30 与 fork 的有效上限 20 是两回事；放宽到 31 以及仅用 JSON 恢复，都必须被拒绝。上报的无上限观测值是事实数据，不是有限的 grant。只有已证明零副作用的租约才能释放保持不变的 head；未知或 pending 的副作用会保留其义务。

**当前的释放错误（release-error）契约；实际演练的测试集/探针见下文。** `<neograph/graph/engine.h>` 中的 `graph::ManagedBudgetLeaseReleaseError` 派生自 `ProviderOutcomeError`。`cause()` 保留原始的执行异常，`release_error()` 暴露次要的、持久化租约处置阶段的失败。`outcome()` 在有真实 SDK 证据时保留该证据，没有 SDK outcome 时为 null；释放失败既不能凭空构造 outcome，也不允许重新分发。封闭的 `_neograph_managed_budget_scope` 元数据描述原始的逻辑作用域、上限、截止时间时钟和 generation，但它只是数据，不具备后端 CAS 的权威。

**Archive owner 与留存（retention）契约；实际演练的测试集/探针见下文。** 只有有限的独立根（finite standalone root）或经认证的有限来源，才会从真实配置的 `sp::NativeArchive::owner_scope()` 继承被省略的原始 owner；无限（unbounded）或普通（plain）的 owner 元数据语义不变。显式冲突的 archive owner 会在获取租约之前被拒绝。`CheckpointStore::retains_native_checkpoint() const noexcept` 以及 Core/Async 对应的存储能力，默认返回 false；真实的 InMemory 后端重写为 true，包装器必须把真实的留存能力转发下去。这个只读的描述，允许合法的、未加租约/普通/无限的 C++ native 检查点托管，但它既不授予消费额度（credit），也不授予 native replay 权限。加了租约的托管使用存储实际签发的 receipt，而不是 JSON 标志或猜测出来的存储类型。

**native 托管的 I/O 前闸门（pre-I/O gate）；实际演练的测试集/探针见下文。** 开始一个受管的副作用（managed effect）之前，在修改任何 pending effect、slot 或 held 窗口之前，必须有真正绑定的 NativeArchive，或者存储实际签发的本地私有 C++ 留存能力。这个私有能力不会从 JSON 导入，也不会通过 wire 传输。即使远端后端是 InMemory，gRPC 也要求客户端和服务端各有真实的 archive，因为 C++ sidecar 无法跨越这条边界。没有 archive 提供有限的来源 owner 时，原始的匿名 owner 作用域保持为空；真实的 archive 绑定必须与原始作用域一致。只有 financial head/租约的证据，并不能证明 native 托管已经就绪。

诊断用的 JSON 保留原始字节，包括语法上合法但含有重复键的文档；而对可执行的请求/配置做准入时，仍然会拒绝重复键。原始的非 2xx 响应 JSON 保留在 `http.error` 证据里，不做第二次有损解析。具名的 SSE 错误优先于之后正常的流关闭。诊断/provider 元数据的上限，由其已准入的来源范围决定，而不是某个无关的、很小的错误文本上限。

`ProviderRequest::observer_limits` 只属于宿主：显式给出的 `max_events` 和 `max_bytes` 必须为正数，而且只能调低已准入的 SDK 交付上限。`provider-request/v3` 对生效的限制、模式、编码后的请求体、重试策略以及所有语义描述符绑定求摘要。bridge 把实际的 PMR vector/map 容量，加上持有的事件/文档字节数，在排队和排空两批中一并计入；取消是在它的队列互斥锁之外发起的。名为 `messages` 的普通通道不会强行当成 chat。把 native 的 `history` 通道映射到 `messages` 时，会保留它的 C++ sidecar，而不是凭 JSON 凭空造出 native 权限。

`ProviderOutcomeError` 是保留 outcome 的宿主错误的公共基类；`ProviderObserverError` 和 `ProviderDispatchOutcomePersistenceError` 保留已完全排空的 SDK 结果和原始的 `cause()`。持久化错误还会在 `delivery_error()` 中保留次要的 observer 失败。`ProviderFailure::outcome()` 保留 SDK 失败本身。这些都是证据，不意味着 Node 或 Program 可以重新分发：传输层的重试完全由 SDK 负责，调用方选定的 `max_output_tokens` 也绝不会悄悄截断。

`ProgramFailure` 保留存活的 `provider_outcome` 和 `provider_cause`。它规范的、事实性的 SDK witness，把真实的 archive 托管绑定到 owner/run/version/bundle/operation/attempt；Runtime 在暴露恢复出来的失败之前，会先主动恢复配置好的托管。公开的、纯数据的 `ProgramResult::create()` 不能用预先填好的 witness 绕过这一步，未解析完成的 parsed seal 也不是可执行的结果。进程重启之后，原来的异常指针不再可用（`provider_cause == nullptr`），也不会根据文本重建。无法持久化的失败，也就无法序列化、发布或重放。

`RecordedBindingSet` 是绑定到来源（source-bound）、只能移动的数据，不是调用方提供的 dispatcher。受信的 Catalog `recorded_capability_binder` 根据真实持久化的来源事件，独立地物化出只含已捕获内容的能力（captured-only capabilities）。`ProgramRuntime::replay_recorded()` 先检查原来所选来源的权限，再通过持久的 CAS 转移实际剩余的 bank；继承下来的花费不是新的模型 grant。旧的 `start_recorded` 续期 API 已经移除。InMemory、File、SQLite 和 PostgreSQL 的 Program 存储，在整个执行期间保留那份确切的、不可变的、归自己所有的租约；过期不会使它续期。受控的 JavaScript 仍会校验底层的 capability manifest，并消费确切的已完成命令结果，不会重新分发外部副作用。

**Recorded-control 因果修复已在完整测试集中验证。** 重放已捕获的命令时，执行之前只为新增的 CPU 挂钟时间和 Core 工作做持久的预留，然后通过结果 CAS 发布测得的工作量，以及新产生的 Core 检查点。它不消耗新的模型、资金或 Program 操作额度，也不会重新分发已捕获的外部副作用。未对账的预留仍然记为已扣减。这笔预留会选择经认证的结算转换，而不是曾经拒绝首个新 Core 检查点的普通 Running→Running 转换。await 通道的接收、定时器的等待/取消、以及 handoff 等待的发起/释放，都在各自所属的 executor/strand 上串行化；现有的 Recorded CPU/Memory await/handoff 场景在完整测试集中通过；远端 TSan 覆盖的局限仍在下文明确列出。

下面这些观测记录于本次文档校对之前。它们是历史证据，不是新的测试运行结果，也不保证适用于所有平台、传输方式或安全属性。

**已完成的付费观测；并非通用的资格认证（qualification）。** 最初的 `SPQUAL1` 基线为 630/1000000 microUSD，保持不变；同一份原始账本（ledger）里，由一个哈希链接的 `A` 接纳了获批的扩展额度 480/3000000，合计 1110/4000000，调用数、已花费额度、hold 和结算都是累计的，没有新的 grant ID、header，也没有重置。确切的声明字节/文件标识，以及原始的授权/基线/catalog/激活/账本前缀的哈希和总额都保持固定；删除、替换或改动都按“失败时关闭”处理。最终的规范账本为：调用 1110 次、已花费 437958、hold 1287828 microUSD，eventA1，上限 1110/4000000；已花费加 hold 共计 1.725786 美元，这是本地 catalogue 的计量，不是发票。文档记录的五个 family、每个 60 对的基线共完成 600 次请求：Chat 60/60、Responses 60/60、Messages 60/60、Generate 56/60（4 次 vision 判断错误，均为 SSE）、Interactions 57/60（1 次 buffered、2 次 SSE 的 vision 判断错误）；合计 293/300 对，不是 300/300。其他旧的 600 次相关财务记录被保留，但并不构成完整的行为证明。更早的 M5/media 一次性 cohort 保持不变。更早的三轮 Google 前置条件检查，仍保留 2 次 invalid-tool 和 1 次 unreadable-positive 失败。不再授权任何付费调用。最终的 SDK 证据和 native-axis 的局限，与基线的成功是两回事。更早的激活/重新打开（reopen）冒烟测试，仍记录为两次 reopen 后调用 610、已花费 219159、hold 751233，SDK 的 meter/canary/vision 4 个测试在 19.38 秒内通过；这些都是特定范围内的历史检查点，不是最终的账本总数。更早验证过的 Chat 60 对 cohort 仍保留 120 次实际尝试、120 次 UpperBound 计费，没有 UnknownHold。

**native-axis 观测：不是密码学验证，也不代表 native 消费或等价。** Generate 接受了篡改（mutation）、省略（omission）和重复（duplication）。Interactions 接受了孤立的真实来源/正向对照、单个 owner 的签名篡改、省略 thought-carrier、省略 call-carrier，以及重复。删除全部 thought 和签名，返回 generic 400；删除全部签名字段、但保留 THOUGHT 项，同样返回 generic 400。最后一次采集（capture）只有本地的、编码后原件的留存对照，没有同一次采集中的服务端正向结果；更早的正向 cohort 仍然是真实的。这些观测只确立了“整体 carrier 缺失”的边界，不能证明 issuer/签名校验或 vendor 的消费。实际报告：SDK 的 `config/qualification-extension-results.json`、`qualification-final-summary.json`、`qualification-native-axis-results.json`、`qualification-combined-omission-results.json` 和 `qualification-signature-presence-results.json`；前置条件失败、未运行、负向结果不确定这几种状态，如实保留。只要还剩另一个 carrier，仅省略 thought 或仅省略 carrier 的请求就被接受；这并不能加强 issuer 校验或 native 消费方面的任何说法。

**实际的集成验证与遗留局限。** 最近一次 Core 完整运行：2242 个测试，0 失败，16 个跳过（14 个 RAM 进程丢失用例不适用，2 个需要真实凭据的闸门），耗时 130.17 秒。`PgNestedJsonRoundTrips` 精确保留了重复的键、顺序、null 元数据、blob 和 residual，用时 0.18 秒。未改动的原始 shared-bank fork 和现有的 Recorded CPU/Memory await/handoff 场景均通过。针对真实包装的 Memory/SQLite/PostgreSQL/gRPC，执行了 finite130/hold65/lower129/strip/old-head/pruning/no-archive/import 这组探针，普通构建和 ASan+UBSan 构建下均通过。本地 Memory/SQLite/PostgreSQL 的 TSan 范围：7 项通过，0 个警告。含系统 Abseil/Protobuf 的完整混合 gRPC TSan 以退出码 66 结束，在依赖库和生成的 RPC 栈里有 402 条 race 警告：这是插桩/覆盖范围的局限，不是已证实的误报；我们不声称远端 TSan 无 race，也没有屏蔽任何警告。已安装的 find_package Program C++/C ABI/双 QuickJS 这 3 个使用方均通过。全新安装的 NeoGraph/SchemaProvider 类型化使用方通过了：2 次真实的 HTTP 请求、协程启动前销毁 provider、native/工具重放、refusal、已知为零/原始值的保留，以及真实的 LinkedMismatch 拒绝。浏览器中 Alice/Bob 的隔离和 generation 2 的替换，已经目视验证；PostgreSQL Program Chat 的 6 个黑盒测试在 18.989 秒内通过。最近一次 SDK：26/26 通过，0 失败，74.07 秒。最终的 ReleaseGraph：16 种配置 ×3 次全新进程重复，共 48 条记录，用时 38.29 秒完成，0 失败，所有实际的协议/owned-outcome 检查均通过。NeoGraph 的 `benchmarks/provider-cutover-final-results.json` 和 `benchmarks/provider-cutover-final-summary.json` 保存了这个独立的最终 cohort。测量期间没有运行编译器，也没有调用付费模型；历史 cohort 保持不变，也不声称语义或资源上等价。不稳定的 SDK/ABI3 既不是稳定发布，也不是对更多平台的资格认证。

**最小化的付费证据（2026-10-03），不是大范围的资格认证。** 三次分别批准的一次性调用产生了如下结果：Images——一张 JPEG，1024×1024，360685 字节，input/output/total token 为 19/1408/1427，已目视检查；Veo——一个 MP4，1280×720，4 秒，437737 字节，一次生成加三次状态 GET，用量可为空，已在 Chromium 中解码并目视检查；Decisions——`typesafe/jev-1.13`，概率 0.93，input/output token 为 283/21，total 未知，API 上报的成本为 0.000011886 美元。图像的 0.0336 美元基础费用加文本/thinking 费用，以及 Veo 的 0.20 美元，都是价格目录中的预期值，不是发票；最小化的图像冒烟测试没有记录价格档位的明细。没有任何结果会续期一次性调用的授权，也不授权重新运行。

已完成的 chat 对话对，并不能证明下游 vendor 消费了 native 续接状态。

- [ABI_POLICY.md](ABI_POLICY.md)
- [ASYNC_GUIDE.md](ASYNC_GUIDE.md)
- [Issue #5](https://github.com/fox1245/NeoGraph/issues/5)：历史决定；其中的 provider 兼容性策略已被取代。

---

# 迁移 3：`compile()` 的 worker 线程池默认值为 1（恢复 v0.1.4 引入的回归）

## 变更内容

`GraphEngine::compile(def, ctx)` 的默认 worker 数量，自 v0.1.4（`b59444f`）起是 `std::thread::hardware_concurrency()`，在当前 v1 之前的 API 中恢复为 **`1`（即没有引擎自己的 thread_pool）**。

## 原因

默认值取 `hardware_concurrency` 时，所有 fan-out 节点都要付出跨线程提交的开销（约 6-7 µs/任务）。bench 的并行测量（5 个 worker 加一个 summarizer）从 11.6 µs 退化到 44 µs，慢了 4 倍。在测量环境里做二分查找，定位到罪魁祸首是 v0.1.4 的 `b59444f`。

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

## 受影响的 NeoGraph 内部示例

与这次变更同时加入的 fan-out 可见性补丁，加入了显式调用，以保持原有意图。如果用户代码符合下面的情形，请采用同样的写法：

- `examples/10_send_command.cpp`：同步 `sleep_for` 的 ResearcherNode 通过
  Send 做 fan-out，已加上 `engine->set_worker_count_auto()`
- `examples/14_plan_executor.cpp`：5 个子主题的 Send fan-out（同步 sleep_for），
  同样加了这一行
- `examples/21_mcp_fanout.cpp`：3 个 MCP 工具调用并发发出，同上
- `examples/36_classifier_fanout.cpp`：本来就显式写了 `set_worker_count(5)`。
  修正了一处写着错误默认值的注释（当前默认值是
  1，即没有引擎自己的线程池）
- `src/core/deep_research_graph.cpp` 中的 `create_deep_research_graph()` 构建器：
  在 `compile()` 之后立即调用 `set_worker_count_auto()`，使 supervisor 的
  N 个 researcher 真正并发运行

`examples/05_parallel_fanout.cpp` 靠 `io_context` 上协程定时器的重叠实现
（没有同步 sleep），所以 worker 线程池没有作用，保持不变。

如果用户代码里有同样的写法：

```cpp
auto engine = GraphEngine::compile(def, ctx);
engine->set_worker_count_auto();   // ← add this line
```

---

# 迁移 4：C++ ABI 与必须重新构建的边界

NeoGraph 现在给每个已编译的公共库设置项目的 `VERSION` 和主版本号 `SOVERSION`。v1 之前的所有发布都使用 ABI 第 0 代，但 `0.x` 的二进制兼容性不受保证。变更日志宣布某个边界时，必须重新构建每个 C++ 使用方。特别是，从 `0.11.1` 或更早的版本升级到包含有界 `NodeCache` 的版本时，由于 `NodeCache` 和 `EngineConfig` 的对象布局发生了变化，必须重新构建。

带类型的 Provider 迁移，把它的虚函数契约改成了 `get_name`、`family` 和 `prepare`，并移除了旧的 completion 虚函数。已有的 Provider 二进制不再兼容；请用匹配的头文件和库，重新构建自定义 provider 和每个依赖它们的 C++ 使用方。Core 也暴露了 SchemaProvider 的类型，所以即使禁用了 LLM，构建仍然需要 SDK 运行时。将来稳定的接口，应当优先采用独立的 capability 接口加适配器，而不是修改已稳定的内存布局；当前 v1 之前的接口不承诺二进制兼容。

平台上的库名称、所有已知的必须重新构建的边界，以及 CI 验证，见[二进制兼容性策略](ABI_POLICY.md)。
