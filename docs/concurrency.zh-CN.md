<!-- neograph-i18n: source=docs/concurrency.md locale=zh-CN source_sha256=6a70472a787f8ffaece52d4630a02370df682588e64986fd104f49284ad21a2d -->
# 并发与异步

**Languages:** [English](concurrency.md) | [한국어](concurrency.ko.md) | [日本語](concurrency.ja.md) | [简体中文](concurrency.zh-CN.md)

## 选择 executor

同步的 `run`、`run_stream` 和 `resume` 会阻塞调用线程，底层驱动的是与异步版本相同的协程实现。宿主的 worker 线程池可以并发运行互相独立的会话。异步版本返回 `asio::awaitable<RunResult>`，需要在自己持有的 executor 上驱动它们。awaitable 并不会让任意的用户代码变成非阻塞。

`EngineConfig::worker_count = 1` 是默认值，此时不会创建引擎自己的 fan-out 线程池。挂起的 I/O 分支可以在同一个线程上重叠；该线程上的 CPU 计算则是串行的。调用方使用多线程的 executor，或者选择启用引擎的 worker 线程池，CPU 分支才能在多个核上运行。线程池要在发布引擎之前配置好。

```cpp
#include <neograph/async/run_sync.h>

EngineConfig options;
options.node_context = ctx;
options.checkpoint_store = std::make_shared<InMemoryCheckpointStore>();
options.worker_count = 4;
auto engine = GraphEngine::build(def, std::move(options));
RunConfig run;
run.thread_id = "session-1";
run.input = {{"count", 0}};
auto result = neograph::async::run_sync(engine->run_async(run));
```

示例 `27_async_concurrent_runs.cpp` 展示了在一个 io_context 上运行多个会话；`05_parallel_fanout.cpp` 展示了同一次运行内的各个分支。历史上的吞吐量和内存测量值见[性能深度剖析](performance-deep-dive.md)，它们不是对安全会话数上限的保证。

## 共享引擎的规则

- 不同的会话使用不同的 `thread_id`。相同 id 的并发执行，其检查点的交错顺序没有规定；历史顺序重要时，由宿主来串行化。
- 在把引擎暴露给执行线程或管理线程之前，先调用配置类的 setter 并绑定工具。执行期间调整 worker 线程池的大小属于硬错误。
- 同一个引擎上，管理操作和执行操作互斥。只要有任何 run/resume 正在进行，状态/历史的读取、update 和 fork 就会以 `std::logic_error` 拒绝；管理操作进行期间，执行也会被拒绝。重试管理操作之前，先取消或排空，并等待完成。
- 共用同一个存储的不同引擎，不在这条准入(admission)边界之内；需要由宿主来协调。
- 节点实例会在多次运行之间复用。每次运行的临时状态放在通道里；自定义节点、provider、工具和存储必须是无状态的，或者做好同步。内置的内存存储使用互斥锁。

Provider 调用自己持有准备好的请求和运行时客户端。它们的 C++ 事件视图只在回调期间有效；需要保留的数据要复制出来。native replay 状态和核算权限需要真实的托管，不能靠可移植 JSON 重建。参见[异步指南](ASYNC_GUIDE.md)。

## 一场对话的多次运行

一场对话可以存放在两层里，这两层对并发写入方的处理不同。

- **上下文 feed** 失败时关闭（fail closed）。`ContextStore::append_history` 接受写入方期望的 head。期望的 head、序号或前驱与 feed 当前的值不一致时，追加会返回 `Conflict` 且不存储任何内容，因此两个写入方不可能都接在同一个 head 之后。把已经存储的那条记录原样再追加一次，即使 head 已经前移，也返回 `AlreadyPresent`。
- **检查点存储**按 `thread_id` 采用最后写入者获胜（last-writer-wins）。单次调用是原子的，一连串调用则不是。同一个 `thread_id` 的两次运行重叠时，它们的 `save_checkpoint` 调用以没有规定的顺序交错，最后一次保存生效，`load_latest` 返回最后保存者的检查点。引擎和存储都不会拒绝第二次运行。

对同一个 `thread_id`（以及同一个 feed）的运行和恢复做串行化，是宿主的职责，跨进程时同样如此。严格 profile 不负责这件事：它为所有调用方只持有一个生效的 epoch，也不看 `thread_id`。同一场对话的请求重叠，通常出于意外，比如重试的 webhook 或者双击。请在请求到达引擎之前，由宿主排队或拒绝。

计划提供进程内的守卫：同一个 `thread_id` 的运行或恢复正在进行时，它会拒绝第二个。它只覆盖单个进程。在它出现之前，NeoGraph 不会阻止同一个 `thread_id` 的两次运行。另见[宿主逐步推进契约](STRICT_RUNTIME_INTERPOSITION.md#host-stepped-contract)。

## 有界的同步准入

使用 `RequestQueue` 需要链接 `neograph::util`。它基于 `moodycamel::ConcurrentQueue`，空闲的 worker 在条件变量上等待。待处理槽位的上限限制的是排队的会话数，而不是正在运行的会话所占用的内存。

```cpp
#include <neograph/util/request_queue.h>

neograph::util::RequestQueue queue(16, 1000);
auto [accepted, future] = queue.submit([engine, config] {
    auto result = engine->run(config);
    handle(result);
});
if (future.valid()) future.get();
if (!accepted) reject_request();
```

队列已满时，返回 `accepted=false` 和一个无效的 future。内部入队失败时，返回 `false`，同时给出一个带有 `std::runtime_error` 的有效 future；要去观察这个 future，不要把每次拒绝都当作普通的队列饱和。构造时至少需要一个 worker。

`close()` 是幂等的。它拒绝新的提交，让已被认领的工作做完，并让尚未认领的 future 以 `std::runtime_error("RequestQueue is closed")` 结束。worker 自己调用 close 时，只发起关闭，不会等待自己。析构函数走的是同一条路径；已接受的 future 不会悄悄丢下。

## 检查点 I/O 与 Python

内存中的检查点操作在调用方线程上、持互斥锁运行；SQLite 和同步的自定义后端，会把阻塞的工作交给数量有限的 worker 去做。PostgreSQL 使用非阻塞的 libpq I/O，不做 pipeline 批处理。`NEOGRAPH_BUILD_POSTGRES=ON` 会启用这个可选目标，并需要 libpq 的开发文件；`OFF` 只会去掉这个可选依赖。

Python 回调在 GIL 下执行。CPU 密集的 Python 节点和 reducer，光靠调大 worker_count 得不到并行；原生调用只有在自身实现释放了 GIL 时才能重叠。带类型的 provider invoke/dispatch 会释放 GIL，可以通过 `asyncio.to_thread` 调用。这并没有提供原生的 provider asyncio awaitable。

## 运行时依赖

Core 始终会链接外部的 `SchemaProvider::runtime`，`NEOGRAPH_BUILD_LLM=OFF` 时也是如此。关闭 PostgreSQL、LLM 节点或 NeoGraph 可选的 CurlH2Pool，都不会去掉该运行时对 libcurl 的需求。请提供匹配的已安装 SDK，或设置 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`。

查找 SDK 源码的顺序是：优先使用显式指定的 SDK 源码目录，其次是已安装的包，最后是固定了修订版本的公开归档（默认 `NEOGRAPH_FETCH_SCHEMAPROVIDER=ON`）。离线构建时，使用已安装的 SDK，把该开关设为 `OFF`，并在 `CMAKE_PREFIX_PATH` 中给出它的前缀。配置 NeoGraph 和 SDK 源码都需要 CMake 3.20 及以上。

与 SDK 的 WASM 运行时集成尚未确立，仅有可移植的 executor API 也无法提供它。
