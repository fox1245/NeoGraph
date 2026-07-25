<!-- neograph-i18n: source=docs/migration-v0.4-to-v1.0.md locale=zh-CN source_sha256=5ec95b54abb4ec639a30a3eb4da6cf300fffa1e34c0afc2679b9ecf7272c4709 -->
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
| `execute_async(state)` | 原生异步，如 `co_return co_await provider->complete_async(...);` |
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

**旧：**
```cpp
class TalkNode : public GraphNode {
    std::shared_ptr<Provider> prov_;
public:
    asio::awaitable<std::vector<ChannelWrite>>
    execute_async(const GraphState& state) override {
        auto reply = co_await prov_->complete_async({
            .messages = state.get_messages(),
            .model    = "gpt-mock",
        });
        co_return std::vector<ChannelWrite>{
            {"reply", json(reply.message.content)}
        };
    }
    std::string get_name() const override { return "talk"; }
};
```

**新：**
```cpp
class TalkNode : public GraphNode {
    std::shared_ptr<Provider> prov_;
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto reply = co_await prov_->complete_async({
            .messages = in.state.get_messages(),
            .model    = "gpt-mock",
        });
        NodeOutput out;
        out.writes.push_back({"reply", json(reply.message.content)});
        co_return out;
    }
    std::string get_name() const override { return "talk"; }
};
```

### 案例 3 — 流式节点（迁移 `execute_stream`）

**旧：**
```cpp
std::vector<ChannelWrite>
execute_stream(const GraphState& state, const GraphStreamCallback& cb) override {
    auto reply = prov_->complete_stream(params, [&](const std::string& chunk) {
        cb({GraphEvent::Type::LLM_TOKEN, "talk", json(chunk)});
    });
    return {ChannelWrite{"reply", json(reply.message.content)}};
}
```

**新：**
```cpp
asio::awaitable<NodeOutput> run(NodeInput in) override {
    // in.stream_cb is a pointer — null means the caller does not want streaming.
    auto on_chunk = [&](const std::string& chunk) {
        if (in.stream_cb) {
            (*in.stream_cb)({GraphEvent::Type::LLM_TOKEN, "talk", json(chunk)});
        }
    };
    auto reply = prov_->complete_stream(params, on_chunk);
    NodeOutput out;
    out.writes.push_back({"reply", json(reply.message.content)});
    co_return out;
}
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

节点可用的 `in.ctx` 字段包括：`cancel_token`、`usage`、
`thread_id`、`step`、`stream_mode`、`store` 和 `resume_value`。
`deadline` 和 `trace_id` 是为未来 `RunConfig` 扩展保留的字段；当前
引擎不填充它们，也不向 Python 暴露。

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

# 迁移 2：`Provider` 兼容性策略及新的显式请求 API（v0.9+）

现有的 `Provider::complete*` 四个方法和基于回调的 `invoke()` 是稳定 API，
无移除计划。现有实现和调用者无需迁移。但是，新的 `Provider` 实现应仅重写
`CompletionProvider::do_invoke()`，新的直接调用应使用
`invoke_request(CompletionRequest)`。

## 现有调用模式与推荐的新的调用模式

| 稳定兼容 API | 直接使用 `CompletionProvider` 时推荐的 API |
|---|---|
| `complete(params)` | `run_sync(invoke_request(CompletionRequest::collect(params)))` |
| `complete_async(params)` | `co_await invoke_request(CompletionRequest::collect(params))` |
| `complete_stream(params, on_chunk)` | `run_sync(invoke_request(CompletionRequest::stream(params, on_chunk)))` |
| `complete_stream_async(params, on_chunk)` | `co_await invoke_request(CompletionRequest::stream(params, on_chunk))` |

`CompletionRequest` 将回调的存在与传输模式分离。因此，即使没有回调，
`CompletionRequest::stream(params)` 也明确请求流式传输。仅持有
`Provider&` 或 `Provider*` 的代码可以不变地使用现有 `complete*` 方法。

## 逐例转换

### 调用 Provider 的用户代码

```cpp
// Stable compatible API — continues to be supported
auto completion = co_await provider->complete_async(params);
```

```cpp
// New code using `CompletionProvider` directly — mode is explicit
auto completion = co_await provider.invoke_request(
    CompletionRequest::collect(params));
```

### 自定义 Provider 子类

现有 `Provider` 子类继续工作。新实现应继承 `CompletionProvider` 并仅实现
`do_invoke()`。现有 `Provider` 虚函数表和 Python `complete()` 约定保持不变。

```cpp
// Old pattern — async-native provider
class MyProvider : public neograph::Provider {
public:
    asio::awaitable<ChatCompletion>
    complete_async(const CompletionParams& params) override {
        // ... HTTP call ...
        co_return result;
    }

    ChatCompletion complete_stream(const CompletionParams& params,
                                   const StreamCallback& on_chunk) override {
        // ... SSE call ...
        return result;
    }

    std::string get_name() const override { return "my"; }
};
```

```cpp
// New pattern — transport mode is decoupled from callback presence
class MyProvider : public neograph::CompletionProvider {
public:
    asio::awaitable<ChatCompletion>
    do_invoke(CompletionRequest request) override {
        if (request.streaming()) {
            // Uses SSE/WS transport even without callback.
            // With callback, invokes request.on_chunk()(token) per token.
        } else {
            // ... HTTP call ...
        }
        co_return result;
    }

    std::string get_name() const override { return "my"; }
};
```

新调用者显式指定模式。

```cpp
auto full = co_await provider.invoke_request(
    CompletionRequest::collect(params));

auto streamed = co_await provider.invoke_request(
    CompletionRequest::stream(params, on_chunk));

// Can explicitly request streaming transport even without observing tokens.
auto streamed_without_observer = co_await provider.invoke_request(
    CompletionRequest::stream(params));
```

旧的四个虚函数和 `invoke(params, on_chunk)` 继续受支持。
`CompletionProvider` 最终适配器一次性将所有旧入口点连接到 `do_invoke()`，
防止相互递归。

## 自动取消传播

如果需要取消，请指定 `CompletionParams::cancel_token`。内部引擎节点将
`RunContext` 令牌传递给 params 中的 provider。线程局部隐式传播路径不再使用。

```cpp
// Node body inside engine — both cancel identically
co_await provider->invoke(params, nullptr);                    // OK
neograph::async::run_sync(provider->invoke(params, nullptr));  // OK
```

图外的直接调用者（例如 `Agent` 用户代码）遵循相同模式——没有显式的
cancel_token 则无法接收取消（与之前相同）。

## 如果不迁移会发生什么

无需采取任何操作：
- 旧的虚函数重写和直接调用继续工作。
- Provider 相关的 `-Wdeprecated-declarations` 警告不再出现。
- 兼容性和安全性修复同样适用于旧 API。

没有移除旧 API 的计划。但是，新功能可能仅添加到显式请求约定中，因此
使用 `CompletionProvider` 实现新功能更为可取。

## 能否自动转换？

调用点遵循简单模式：

```bash
# Review with dry-run
grep -rnE '->complete(_async|_stream|_stream_async)?\(' your/code

# Then manually edit case-by-case (refer to the mapping table above).
```

将旧的 `Provider` 子类合并到单个 `CompletionProvider::do_invoke()` 中
是可选的，由于交织的逻辑，需要手动编辑。

## 相关文档 / Issue

- [`include/neograph/graph/node.h`](../include/neograph/graph/node.h) —
  新的 `run(NodeInput)` 虚函数的内联 docstring（含示例）
- [ROADMAP_v1.md](../ROADMAP_v1.md) — 候选 1 的详细设计说明
  （GraphNode 8 虚函数展平）
- [troubleshooting.md](troubleshooting.md) — 实际迁移过程中遇到的编译
  错误和运行时差异
- [Issue #5](https://github.com/fox1245/NeoGraph/issues/5) —
  Provider 方法实现路径和永久兼容性策略的记录决策

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
