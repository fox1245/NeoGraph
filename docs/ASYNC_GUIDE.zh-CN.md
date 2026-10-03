<!-- neograph-i18n: source=docs/ASYNC_GUIDE.md locale=zh-CN source_sha256=3d01320c4796b2b8fae399c9660352bcb3eaadefa460b54c1e15664cb04bd537 -->
# NeoGraph 异步指南


> Stage 3（2026-04）设计及当时实测测试数量作为历史保留。provider 兼容/crossover 决策已由下方 typed lossless 切换取代；旧设计记录不是当前 provider API。

第三阶段 / 2026-04 发布。目标受众：正在将现有 NeoGraph 代码迁移到异步
API，或正在编写针对异步 API 的新代码的用户。

本指南涵盖**变更了什么**、**为什么是这个形态**以及**如何增量迁移**。
关于各学期的设计原理，请参阅
[`ASYNC_STAGE3_DESIGN.md`](ASYNC_STAGE3_DESIGN.md)；关于分钟级别的提交
记录，请参阅 `feat/async-api` 分支的 git log。

---

## 1. 新增内容

引擎中的每个同步 I/O 点现在都有一个可等待的对应版本：

| 层 | 同步 | 异步 |
|---|---|---|
| Provider | `invoke` / `dispatch` | `invoke_async` / `dispatch_async` |
| CheckpointStore | `save` / `load_latest` / `load_by_id` / `list` / `delete_thread` / `put_writes` / `get_writes` / `clear_writes` | 每个都有 `*_async` 版本 |
| GraphNode | — | `run(NodeInput) -> asio::awaitable<NodeOutput>` 是唯一的正式重写入口 |
| GraphEngine | `run` / `run_stream` / `resume` | `run_async` / `run_stream_async` / `resume_async` |
| MCPClient | `rpc_call` | `rpc_call_async` |
| Tool | `execute`（用户接口——冻结） | 通过 `AsyncTool` 适配器包装 |

异步对应版本返回 `asio::awaitable<T>`。可在任何 `asio::io_context`
（或 strand，或通过 `any_io_executor` 的线程池）上驱动它们。一个
`io_context` 可以承载数千个并发的 `run_async` 调用，而不必为每次运行
分配一个 OS 线程——这正是推动整个重构的并发模型。

旧 Stage 3 报告记录了既有 276+ 测试通过当时的 sync 路径；这不是当前切换验证结果。

---

<a id="2-the-crossover-default-pattern"></a>
## 2. 已准备 provider dispatch（移除 crossover）

公开契约是拥有所有权的 typed 准备/dispatch，而非同步/异步 virtual completion 对。`ProviderRequest.payload` 是 Chat、Messages、Responses、Gemini、Interactions 的 SDK 请求 variant。`ProviderMode::Collect` / `Stream` 独立于观察者是否存在来选择传输。`on_event` 接收借用的 typed `sp::Event` view；只复制回调后仍需要的数据。不允许 raw JSON override 或通过 portable projection 导入 native 权限。

`prepare()` 恰好验证、编码一次，生成保持原始 deadline 与取消状态的仅可移动 `PreparedProviderRequest`。持久调用方将 `Provider::request_digest()` 绑定到 assembly，预留获准的 budget claim，写入 dispatch receipt，然后通过 `ControlledProvider::dispatch_prepared(_async)` 消费同一个 handle。gate 之后不重建请求。重复 receipt 绝不重新 dispatch。自定义实现提供 `get_name()`、`family()`、`prepare()` 并使用 `prepare_runtime()` 或 `prepare_local()`；local callback 捕获拥有所有权的 shared 状态，而非 `this`。

这是源码和二进制破坏性变更；所有 C++ 使用者与自定义提供方都必须使用匹配的新头文件/库重新编译。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、descriptor interpreter 和 Responses WebSocket 已删除，没有 alias 或兼容 bridge。SDK 为不稳定 `0.0.0`、interface revision 3 / shared ABI 3，使用 out-of-line capability check，不表示稳定发布。当前 runtime/archive 为 Linux/POSIX，不代表 Windows、macOS、WASM runtime 已获验证。Python provider binding/wrapper 已延期，不由本 C++ 变更完成移植。

---

## 3. 迁移方法

### 3.1 同步调用者迁移到异步

**之前：**

```cpp
auto result = engine->run_stream(config, event_cb);
```

**之后：**

```cpp
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

asio::io_context io;
RunResult result;
asio::co_spawn(
    io,
    [&]() -> asio::awaitable<void> {
        result = co_await engine->run_stream_async(config, event_cb);
    },
    asio::detached);
io.run();
```

`io.run()` 在协程完成时返回。对于多个并发运行，在调用 `io.run()` 之前
为每个运行 co_spawn 到同一个 `io_context` 上——参见
`examples/27_async_concurrent_runs.cpp`。

### 3.2 编写新的异步 provider

`prepare()` 恰好验证、编码一次，生成保持原始 deadline 与取消状态的仅可移动 `PreparedProviderRequest`。持久调用方将 `Provider::request_digest()` 绑定到 assembly，预留获准的 budget claim，写入 dispatch receipt，然后通过 `ControlledProvider::dispatch_prepared(_async)` 消费同一个 handle。gate 之后不重建请求。重复 receipt 绝不重新 dispatch。自定义实现提供 `get_name()`、`family()`、`prepare()` 并使用 `prepare_runtime()` 或 `prepare_local()`；local callback 捕获拥有所有权的 shared 状态，而非 `this`。

```cpp
#include <neograph/provider.h>
#include <neograph/runtime_interposition_consumer.h>
#include <runtime/client.h>

class MyProvider final : public neograph::Provider {
    std::string family_;
    std::shared_ptr<sp::runtime::Client> client_;
public:
    MyProvider(sp::descriptor::ValidatedDescriptor descriptor,
               sp::runtime::Options options)
        : family_(descriptor.family()),
          client_(std::make_shared<sp::runtime::Client>(
              std::move(descriptor), std::move(options))) {}
    std::string get_name() const override { return "my-provider"; }
    std::string_view family() const noexcept override { return family_; }
    neograph::PreparedProviderRequest
    prepare(neograph::ProviderRequest request) override {
        return prepare_runtime(client_, std::move(request));
    }
};
```

### 3.3 编写异步 Tool

`Tool` 接口按设计是同步的（第三阶段冻结它以将现有用户工具的迁移成本接近
零）。当你需要协程形态的工作时使用 `AsyncTool`：

```cpp
class FetchTool : public neograph::AsyncTool {
  public:
    ChatTool get_definition() const override { ... }
    std::string get_name() const override { return "fetch"; }

    asio::awaitable<std::string>
    execute_async(const json& args) override {
        auto ex = co_await asio::this_coro::executor;
        auto res = co_await neograph::async::async_post(
            ex, /*host*/, /*port*/, /*path*/, /*body*/);
        co_return res.body;
    }
};
```

`AsyncTool::execute` 是 `final` ——它是同步门面，会启动一个私有
`io_context` 来驱动 `execute_async`。同时重写两部分是违反约定的。

### 3.4 编写使用异步 provider 的图节点

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

## 4. 注意事项与陷阱

### 4.1 GCC 13 协程 ICE

两种特定的 C++20 协程形态会触发 GCC 13 的 `build_special_member_call`
ICE（截至 GCC 13.3）：

**形态 1 — `catch` 块内的 `co_await`：**

```cpp
try { ... }
catch (const MyError& e) {
    co_await something();  // ICE
}
```

**变通方法 — 在外部捕获错误，之后再处理：**

```cpp
std::optional<MyError> err;
std::optional<Result> ok;
try { ok.emplace(co_await op()); }
catch (const MyError& e) { err.emplace(e); }

if (err) {
    co_await recover();
    throw *err;
}
```

**形态 2 — 协程体内的嵌套花括号初始化：**

```cpp
co_await fn(std::vector<std::string>{name},    // ICE
            json{{"key", "value"}});
```

**变通方法 — 在外部构建，在内部引用：**

```cpp
std::vector<std::string> v;
v.push_back(name);
json j;
j["key"] = "value";
co_await fn(v, j);
```

这两种形态在第三阶段中多次出现，变通方法是稳定的。Clang 18+ 和 GCC 14+
可以无问题地编译"自然"形式，但 NeoGraph 以 GCC 13 为基线。

### 4.2 `run_sync` 生命周期风险

`neograph::async::run_sync<T>(asio::awaitable<T>)` 每次调用创建一个全新的
单线程 `io_context`。任何绑定到该执行器的长寿命 asio 句柄——池中的套接字、
定时器、文件描述符——在 `run_sync` 返回后将成为悬垂引用。这在早期的
ConnPool 工作中曾造成问题，当前架构通过刻意不通过同步门面池化任何内容
来规避。

规则：对于必须比单次调用寿命更长的资源（连接池、长寿命流描述符），
仅将它们绑定到您在进程生命周期内拥有的执行器。同步门面路径每次请求
创建全新连接。

### 4.3 `co_return co_await x`，而非 `return x`

返回 `asio::awaitable<T>` 的协程函数必须在函数体内某处使用 `co_return`
（或 `co_await`）。普通的 `return other_awaitable()` 看似可以编译，但在
运行时会默认构造包装的 `T`。始终通过 `co_return co_await` 链接：

```cpp
asio::awaitable<RunResult>
GraphEngine::run_async(const RunConfig& config) {
    co_return co_await execute_graph_async(config, nullptr);
}
```

### 4.4 从自定义节点发送流事件

`GraphNode::run(NodeInput)` 每次分发执行一次。仅在 `in.stream_cb` 非空时
发送事件，无论调用者是否使用流式引擎入口点，都返回相同的 `NodeOutput`。
旧的双重执行回退路径已不存在。

### 4.5 MCP stdio 单会话并发

一个 stdio 传输独立持有自己的 `io_context`、子进程管道、写信号量和
按响应 ID 分发的读取器。**同一**会话上的 `rpc_call_async` 调用只在写入
完整帧时互斥；读取可并发等待，读取器通过 JSON-RPC ID 将响应交给对应调用。
调用可以来自不同执行器，也可以跨连续的 `run_sync` 图运行。某个调用被
取消或超时不会关闭共享子进程，迟到的响应也不会错误地发给其他调用。

---

## 5. 性能说明

异步线路不会使单个 agent 更快——`bench_neograph` 报告与第三阶段之前相同
的 seq（~30 µs）和 par（~205 µs）数值。价值轴是**并发鲁棒性**，而非
引擎延迟。

在真实形态基准测试上测量的改善：

* `bench_async_http --mode async_pool --concur 1000` — 17834 ops/s，
  对比第二阶段异步（8401 ops/s）和同步（6064 ops/s）。
* `bench_async_fanout --concur 50000` — 541K ops/s，67 MB RSS。
  每个 agent 一个线程的基线无法扩展到超过 ~1000 个并发 agent；
  50K 现在只是一个下午的工作量。
* `examples/27_async_concurrent_runs` — 3 个 agent × 50ms 工作在单个
  io_context 上：总计 50ms（对比串行 150ms）。
* `examples/05_parallel_fanout` — 3 个并行研究者在一个 io_context 上：
  总计 150ms（对比串行 370ms）。

### 何时继续使用同步 API

如果你的工作负载是 ≤ 1000 个并发 agent 且每个 agent 在一个专用 OS 线程
中运行，同步 API 仍然是完全合理的选择。在此规模下线程足够便宜，且同步
代码更易于推理。异步 API 的存在是为了解决同步形态无法应对的工作负载——
数百个长寿命 agent 共享一个进程，从单个事件循环托管多个用户，等等。

---

## 6. 干净迁移清单

- [ ] 确定 agent 宿主模式：单 agent 进程、池，还是共享事件循环？
- [ ] 如果是共享事件循环 → 将调用点迁移到 `run_async` /
      `run_stream_async`。
- [ ] 自定义节点实现 `run(NodeInput)` 并直接 `co_await` 真实 I/O。
- [ ] 如果您的工具执行真实 I/O → 继承 `AsyncTool`，重写
      `execute_async`。
- [ ] 如果使用 Postgres 检查点存储 → 在共享事件循环上使用其 `*_async`
      方法。它们使用 libpq 的非阻塞线路协议和协程友好的连接池。
- [ ] 测量。价值轴是并发；如果您的工作负载不受并发限制，不要迁移。

---

## 7. 尚未覆盖的内容

* **Postgres 管线模式** — 异步检查点方法已使用非阻塞 libpq I/O，但尚未
  通过 libpq 管线模式批量处理多个命令。
* **`async::HttpResponse` headers map** — 响应接口仅暴露 status / body /
  retry_after / location。任意头访问（例如 MCP 会话 ID 头跟踪）是第一
  学期的后续工作。

---

## 8. 3.0 中的变更

3.0（`feat/taskflow-removal`）通过移除 Taskflow 并将同步入口点路由到
`run_sync(execute_graph_async)`，将同步和异步折叠到一个协程运行时。
2.0 异步 API 形态不变——差异在于默认值和新加入的可选项。

### 8.1 `GraphNode::run(NodeInput)` 取代旧的 override 链

v0.9.0 v1 预备版移除了八个 `execute*` 虚函数。现在自定义节点对同步和
异步引擎入口点、流式以及控制流仅有一个重写方法：

```cpp
asio::awaitable<NodeOutput> run(NodeInput in) override {
    NodeOutput out;
    out.writes.push_back({"answer", co_await fetch_answer(in)});
    Command command;
    command.goto_node = "review";
    out.command = command;
    if (in.stream_cb) {
        (*in.stream_cb)({GraphEvent::Type::LLM_TOKEN, get_name(), json("done")});
    }
    co_return out;
}
```

从更早版本迁移的代码必须将其状态读取移动到 `in.state`，运行元数据移动到
`in.ctx`，流式接收器移动到 `in.stream_cb`，并将写入/`Command`/`Send` 值
填充到返回的 `NodeOutput` 中。

### 8.2 `GraphEngine::set_worker_count(N)` — 可选加入的 CPU 并行扇出

默认值：`run_parallel_async` 和 `run_sends_async` 的多 Send 分支在驱动
当前协程的执行器上分发分支。对于同步 `run()`，那是单线程 io_context——
I/O 绑定的分支仍然通过 co_await 挂起重叠，但 CPU 绑定的分支串行执行。

```cpp
EngineConfig engine_config;
engine_config.node_context = ctx;
engine_config.checkpoint_store = store;
engine_config.worker_count = std::thread::hardware_concurrency();
auto engine = GraphEngine::build(def, std::move(engine_config));
// Now run_parallel_async dispatches branches to an engine-owned
// asio::thread_pool of that size.
```

尽可能在构造之前设置。兼容性 setter 必须在任何并发 `run()` 之前调用；
在运行中的 run 之间重建池是不安全的。自己驱动多线程
`asio::thread_pool` 的 `run_async` 调用者不需要这一步——他们的调用侧
执行器已经并行化了分支。

### 8.3 `neograph::async::run_sync_pool(aw, n_threads)` — N 工作器同步桥接

```cpp
#include <neograph/async/run_sync.h>

int result = neograph::async::run_sync_pool(
    my_coroutine_that_uses_make_parallel_group(), /*n_threads=*/4);
```

与现有单线程 `run_sync` 相伴。为调用启动一个全新的
`asio::thread_pool`，使得内部的 `make_parallel_group` 分支在不同
工作器上执行。每次调用的池构造会为每个工作器生成一个 `std::thread`——
成本对热路径不可忽略，因此这用于偶尔的边界同步桥接，而非每请求代码。

### 8.4 已移除的接口

- `NodeExecutor::run_one` / `run_parallel` / `run_sends`（同步）——使用
  `_async` 对应版本。
- `GraphEngine::execute_graph`（同步）——已删除；`run()` /
  `run_stream()` / `resume()` 通过 `run_sync` 路由到异步对应版本。
- `tf::Executor`、`tf::Taskflow`、`deps/taskflow/` 目录——已移除。使用
  Taskflow 作为调用侧驱动的基准测试（`bench_concurrent_neograph.cpp`）
  切换到 `asio::thread_pool` + `asio::post`。

---

## 9. 重写决策指南

公开契约是拥有所有权的 typed 准备/dispatch，而非同步/异步 virtual completion 对。`ProviderRequest.payload` 是 Chat、Messages、Responses、Gemini、Interactions 的 SDK 请求 variant。`ProviderMode::Collect` / `Stream` 独立于观察者是否存在来选择传输。`on_event` 接收借用的 typed `sp::Event` view；只复制回调后仍需要的数据。不允许 raw JSON override 或通过 portable projection 导入 native 权限。

### 9.1 两分钟版本

| 你写的是… | 重写 | 按原样继承 |
|---|---|---|
| 任何自定义 `GraphNode` | `run(NodeInput)` | `get_name()` 是唯一其他必需的虚函数 |
| Provider | `get_name()`, `family()`, `prepare(ProviderRequest)` | `invoke(_async)`, `dispatch(_async)` |
| 自定义 `CheckpointStore`，支持异步后端 | 全部八个 `*_async` 对应版本 | 同步对应版本通过 `run_sync` 桥接 |
| 自定义 `CheckpointStore`，仅同步后端 | 全部八个同步对应版本 | 异步对应版本通过 `run_sync` 桥接 |
| 自定义同步 `Tool` | 继承 `Tool`，重写 `execute()` | — |
| 自定义异步 `Tool` | 继承 `AsyncTool`，重写 `execute_async()` | 同步 `execute()` 是 `final`，会桥接 |

### 9.2 `GraphNode`

始终重写 `run(NodeInput)`。仅 CPU 的工作可以在 `co_return` 之前直接执行；
真正的异步 I/O 应当被 `co_await`。引擎从 `run`、`run_async`、流式、
resume 和 Send 扇出调用同一方法，因此没有重写选择矩阵，也没有同步/异步
回退递归。

不要在共享的单线程 `io_context` 上长时间阻塞。将阻塞工作移到执行器或使用
协程友好的 I/O。`EngineConfig::worker_count` 控制需要并行扇出的同步调用者
使用的引擎持有的池。

### 9.3 `Provider`

公开契约是拥有所有权的 typed 准备/dispatch，而非同步/异步 virtual completion 对。`ProviderRequest.payload` 是 Chat、Messages、Responses、Gemini、Interactions 的 SDK 请求 variant。`ProviderMode::Collect` / `Stream` 独立于观察者是否存在来选择传输。`on_event` 接收借用的 typed `sp::Event` view；只复制回调后仍需要的数据。不允许 raw JSON override 或通过 portable projection 导入 native 权限。

`prepare()` 恰好验证、编码一次，生成保持原始 deadline 与取消状态的仅可移动 `PreparedProviderRequest`。持久调用方将 `Provider::request_digest()` 绑定到 assembly，预留获准的 budget claim，写入 dispatch receipt，然后通过 `ControlledProvider::dispatch_prepared(_async)` 消费同一个 handle。gate 之后不重建请求。重复 receipt 绝不重新 dispatch。自定义实现提供 `get_name()`、`family()`、`prepare()` 并使用 `prepare_runtime()` 或 `prepare_local()`；local callback 捕获拥有所有权的 shared 状态，而非 `this`。

可选 `ProviderControls` 是调用方选择，不是强制默认值或暗中 clamp 的 cap。不支持的 family 控制在 dispatch 前拒绝。有界调用需要获准的真实模型 input/output 上限；缺失时为 `LimitUnknown`。预留是保守的支出权限，而非报告使用量、预测或账单。未知/部分/delivery-unknown 结果保留 hold，真实最终报告用于结算，超额报告也全额计入。retry 是显式单层，默认 off，具有有界 window 与 unknown-prior hold；没有隐藏重发。


提供方调用返回 `sp::runtime::Result`，即持有 `sp::Completion` 或 `sp::Failure` 的不可变、拥有所有权的 `std::shared_ptr<const sp::Outcome>`。请保留完整结果，而非仅显示文本。顺序消息/part、native continuation、完整 wire envelope、顺序 raw 观测、停止依据及真实尝试元数据在调用与客户端销毁后仍然保留。使用量是带依据、阶段、质量的 nullable `uint64_t`；缺失表示未知，绝不是零。失败保留原始部分结果。`ProviderFailure::outcome()` 与 `ProviderObserverError::outcome()` 保留真实结果，后者的 `cause()` 也保留观察者异常。

实际结果存在后，若 post-effect 结算或 terminal receipt 持久化失败，`ProviderDispatchOutcomePersistenceError::outcome()` 保留原始不可变结果，`cause()` 保留原始持久化异常。若 delivery 也失败，`delivery_error()` 保留原始观察者异常。持久化成功后的观察者失败原样重新抛出原异常；未知/无结果 transport 失败不会伪造 outcome。
### 9.4 `CheckpointStore`

八个同步方法，八个异步对应版本，1:1 匹配。已发布的存储
（`InMemoryCheckpointStore`、`SqliteCheckpointStore`、
`PostgresCheckpointStore`）均实现异步一侧，并通过基类默认实现让同步桥接。

- **支持异步的后端**（libpq 非阻塞、异步 MongoDB 驱动等）：重写全部八个
  `*_async` 对应版本。同步调用路径为每次调用支付一次 `run_sync`——对于
  `get_state` / `update_state` 管理调用没问题，但不适用于热循环（不过引擎
  从不调用同步检查点方法；仅用户工具调用）。
- **仅阻塞的后端**（旧文件 I/O、某些 ODBC 包装器）：重写八个同步方法。
  异步调用者通过 `run_sync` 在每次调用时阻塞协程线程，这通常可接受，因为
  检查点写入相对于节点分发是低频的。
- **不要混用**：如果你重写了 `save()` 但保持 `save_async()` 为默认值，
  异步对应版本通过基类默认实现桥接*回到*同步——正确，但失去了异步 I/O 的
  好处。每个接口应全同步或全异步。

### 9.5 `MCPClient`

`rpc_call_async()` 是真正的实现；`rpc_call()` 是轻量的
`run_sync(rpc_call_async(...))` 门面。**不可由用户扩展**——
`MCPClient` 并非设计为可被子类化，你应该按原样使用它。如果需要自定义
MCP 传输，编写新类；不要继承。

HTTP 请求正常重叠。stdio 写入在短暂的写入锁下完成 JSON 行，然后单个
读取器通过 JSON-RPC id 关联乱序回复。因此，当子进程并发处理请求时，
stdio 调用也会重叠；串行子进程仍然是吞吐量的下限。

### 9.6 `Tool` vs `AsyncTool`

设计上是不对称的。在类声明时选择其一：

```cpp
class MyCpuTool : public Tool {
  public:
    std::string execute(const json& args) override { /* sync */ }
    ChatTool get_definition() const override { /* ... */ }
    std::string get_name() const override { return "cpu-tool"; }
};

class MyHttpTool : public AsyncTool {
  public:
    asio::awaitable<std::string> execute_async(const json& args) override {
        auto ex = co_await asio::this_coro::executor;
        auto r = co_await neograph::async::async_post(ex, /* ... */);
        co_return r.body;
    }
    // sync execute() is final and routes through run_sync automatically.
    ChatTool get_definition() const override { /* ... */ }
    std::string get_name() const override { return "http-tool"; }
};
```

**不要**尝试从两者继承或重写一个类的两个表面——`AsyncTool::execute` 是
`final` 正是为了阻止这一点。
