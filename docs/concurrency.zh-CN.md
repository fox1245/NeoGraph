<!-- neograph-i18n: source=docs/concurrency.md locale=zh-CN source_sha256=fe3657f31d0895edf67431f7c6135b418f6677d2d4b8cc90699617f5ab343df8 -->
# 并发与异步

**Languages:** [English](concurrency.md) | [한국어](concurrency.ko.md) | [日本語](concurrency.ja.md) | [简体中文](concurrency.zh-CN.md)



NeoGraph 支持两种开箱即用的并发模型 - 选择适合你的托管模式的一种：

* **每个代理线程（同步）** —`run()` / `run_stream()` / `resume()`
分派到你已经使用的任何执行程序。最多可安全容纳大约一千个并发代理；Release `-O3 -DNDEBUG` 构建中每次调用约 5 µs 引擎开销（超级步循环通过 `run_sync(execute_graph_async)` 路由，因此两个入口点共享一个协程路径）。
* **基于协程的异步** —`run_async()` / `run_stream_async()` /
`resume_async()`返回`asio::awaitable<RunResult>`。一`asio::io_context`托管数千个并发代理，每次运行无需线程；所有提供者 / MCP / 检查点 I/O 点是非阻塞的`co_await`在底层。完整的迁移指南在 [`ASYNC_GUIDE.md`](ASYNC_GUIDE.md)。

## 异步（第 3 阶段）
```cpp
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

asio::io_context io;
for (const auto& user : users) {
    asio::co_spawn(
        io,
        [&, user]() -> asio::awaitable<void> {
            RunConfig cfg;
            cfg.thread_id = user.session_id;
            cfg.input     = {{"messages", user.history}};
            auto result = co_await engine->run_async(cfg);
            handle(result);
        },
        asio::detached);
}
io.run();  // drives all agents on this thread
```

`engine->run_async()`端到端地停留在调用者的执行器上——每个超级步暂停点（节点调度、检查点 I/O、并行扇出、重试退避）都是真实的`co_await`。因此，上述三个 50 毫秒的步骤在一个 io_context 线程上重叠，并且壁钟时间约为 50 毫秒，而不是 3 × 50 毫秒。一个线程，N 个并发代理。对于跨核心的 CPU-bound 扇出，将驱动程序切换到共享`asio::thread_pool`——这就是[中的模式`benchmarks/concurrent/CONCURRENT.md`](../benchmarks/concurrent/CONCURRENT.md)其中 N = 10,000 在 52 毫秒内完成。在单次运行中，`make_parallel_group`扇出也重叠：三名并行扇出研究人员从顺序执行的 370 毫秒缩短到 150 毫秒。

自定义节点通过从统一的`run(NodeInput)`入口点返回 `asio::awaitable`（在 v0.4.0 中引入；旧的 8 虚拟链在 v0.9.0 中被删除）：
```cpp
class FetchNode : public GraphNode {
  public:
    asio::awaitable<NodeOutput>
    run(NodeInput in) override {
        auto ex = co_await asio::this_coro::executor;
        auto res = co_await neograph::async::async_post(ex, /*...*/);
        // in.ctx.cancel_token, in.state, in.stream_cb available.
        co_return NodeOutput{ {ChannelWrite{"out", res}} };
    }
    std::string get_name() const override { return "fetch"; }
};
```

异步形工具源自`AsyncTool`：
```cpp
class FetchTool : public neograph::AsyncTool {
  public:
    asio::awaitable<std::string>
    execute_async(const json& args) override { /* co_await HTTP */ }
    // sync execute() is final, routes through run_sync automatically.
};
```

参见 `examples/27_async_concurrent_runs.cpp`对于多代理模式和`examples/05_parallel_fanout.cpp`用于一次运行内的扇出。

## 同步（每个代理线程）

NeoGraph不提供自己的异步运行时——它公开同步`run()` / `run_stream()` / `resume()`并让你选择执行器。单个已编译的 `GraphEngine`可以安全地在调用的线程之间共享`run()`并发调用，只要使用**不同的 `thread_id`**，因此托管多租户 agent的任何工作负载只需将其分派到你已使用的执行程序上即可。
```cpp
// One engine, many concurrent sessions — no external runtime required.
EngineConfig engine_config;
engine_config.node_context = ctx;
engine_config.checkpoint_store = std::make_shared<InMemoryCheckpointStore>();
auto engine = GraphEngine::build(def, std::move(engine_config));

std::vector<std::future<RunResult>> sessions;
for (const auto& user : users) {
    sessions.push_back(std::async(std::launch::async, [&engine, user]() {
        RunConfig cfg;
        cfg.thread_id = user.session_id;
        cfg.input = {{"messages", user.history}};
        return engine->run(cfg);
    }));
}
for (auto& f : sessions) handle(f.get());
```

同样适用于 `asio::thread_pool`、基于 `std::async` 的任务系统，或者你的网络框架的工作池，NeoGraph不参与执行者的决定。如果你需要 CPU 并行扇出发生在*单个*同步`run()`调用（而不是在 N 个线程上执行 N 个同步 `run()`），在 `build()` 前设置 `EngineConfig::worker_count`安装引擎拥有的`asio::thread_pool`供 `run_parallel_async` 和多 Send 分支调度使用。

## 使用捆绑的`RequestQueue`

对于需要具有背压的固定工作池（当队列饱和而不是无限内存增长时拒绝新会话）的多租户服务器，链接`neograph::util`并使用内置的无锁队列——不需要外部执行器：
```cpp
#include <neograph/util/request_queue.h>
using namespace neograph::util;

RequestQueue pool(16, 1000);           // 16 workers, max 1000 pending sessions
EngineConfig engine_config;
engine_config.node_context = ctx;
engine_config.checkpoint_store = std::make_shared<InMemoryCheckpointStore>();
auto engine = GraphEngine::build(def, std::move(engine_config));

std::vector<RunResult>          results(users.size());
std::vector<std::future<void>>  futs;

for (size_t i = 0; i < users.size(); ++i) {
    auto [accepted, fut] = pool.submit([&, i]() {
        RunConfig cfg;
        cfg.thread_id = users[i].session_id;
        cfg.input     = {{"messages", users[i].history}};
        results[i]    = engine->run(cfg);
    });
    if (!accepted) {
        // Backpressure: queue is full — shed load, return 503, retry later, …
        reject(users[i]);
        continue;
    }
    futs.push_back(std::move(fut));
}

for (auto& f : futs) f.get();           // propagates exceptions from run()

auto s = pool.stats();
log("pending={} active={} completed={} rejected={}",
    s.pending, s.active, s.completed, s.rejected);
```

`submit()`返回`{accepted, std::future<void>}`: 捕获`RunResult`通过共享输出槽（如上所述）或每个任务`std::promise<RunResult>`。队列底层使用`moodycamel::ConcurrentQueue`（无锁）并且工作器在闲置时在 condvar 上休眠 - 无忙旋转。

## 安全并发使用规则

- 配置修改函数（`set_retry_policy`, `set_checkpoint_store`,
`set_store`, `own_tools`, ...) 必须在任何并发 `run()` 之前调用。第一次调度后应把引擎视为冻结。
- 共享**相同** `thread_id` 的并发 `run()` 调用不会崩溃
但会产生未指定的检查点交错。如果你需要确定性历史记录，请自行序列化每个会话的访问。
- 自定义 `GraphNode`子类必须是**无状态或自同步**。
节点实例由引擎拥有，并在每个线程的每次运行中重用 - 每次运行的临时数据属于图形通道，而不是节点成员变量。
- 用户提供`CheckpointStore`, `Store`, `Provider`， 和`Tool`
实现必须是线程安全的。捆绑的`InMemoryCheckpointStore`和`InMemoryStore`已经是了。

## PostgreSQL 的持久检查点

对于多进程部署或当检查点必须在重新启动后继续存在时，链接`neograph::postgres`并将 `InMemoryCheckpointStore` 换成`PostgresCheckpointStore`：
```cpp
#include <neograph/graph/postgres_checkpoint.h>

auto store = std::make_shared<PostgresCheckpointStore>(
    "postgresql://user:pass@host:5432/dbname");
EngineConfig engine_config;
engine_config.node_context = ctx;
engine_config.checkpoint_store = store;
auto engine = GraphEngine::build(def, std::move(engine_config));
```

schema 镜像 LangGraph 的`PostgresSaver`（三个表前缀`neograph_*`与同一数据库中的 LangGraph 状态共存）并通过以下方式对通道值去重`(thread_id, channel, version)`。每个超级步涉及一个通道的 1000 步会话的成本大约为`O(steps + channels)`blob 行而不是`O(steps × channels)`。

**构建标志**：`-DNEOGRAPH_BUILD_POSTGRES=ON`（默认）。需要`libpq-dev`（apt）/`libpq-devel`（rpm）。设置标志`OFF`完全跳过依赖关系。

**运行集成测试**：启动一个一次性的本地 PG 并将测试二进制文件指向它：
```bash
docker run -d --rm --name neograph-pg-test \
    -e POSTGRES_PASSWORD=test -e POSTGRES_DB=neograph_test \
    -p 55432:5432 postgres:16-alpine

NEOGRAPH_TEST_POSTGRES_URL='postgresql://postgres:test@localhost:55432/neograph_test' \
    ctest --test-dir build -R PostgresCheckpoint --output-on-failure
```

如果没有环境变量，PG 测试会被 `GTEST_SKIP`，因此套件的其余部分在没有 Postgres 的机器上保持绿色。

覆盖范围：`tests/test_graph_engine.cpp`包含`ConcurrentRunDifferentThreadIds`（16 个线程 × 25 次运行 = 400 次并行执行，验证每个会话输出 + 检查点隔离）和`ConcurrentRunSameThreadIdNoCrash`（8 线程 × 50 在一个共享线程上运行`thread_id`，验证无崩溃行为）。
