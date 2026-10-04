<!-- neograph-i18n: source=docs/concurrency.md locale=zh-CN source_sha256=889743688862b981a4a8e8d8de0c0f3bc287693712453d430183d4dcdd7a030a -->
# 并发与异步

**Languages:** [English](concurrency.md) | [한국어](concurrency.ko.md) | [日本語](concurrency.ja.md) | [简体中文](concurrency.zh-CN.md)

## 选择 executor

sync `run`、`run_stream`、`resume` 驱动与 async API 相同的协程实现，并阻塞调用线程。host worker pool 可并发执行独立 session。async API 返回 `asio::awaitable<RunResult>`，在自行拥有的 executor 上驱动。awaitable 不会把任意 user code 变为 nonblocking。

`EngineConfig::worker_count = 1` 是默认值，不创建 engine 自有 fan-out pool。挂起的 I/O 分支可在单线程重叠推进，CPU 工作则串行执行。多线程 caller executor 或可选 engine pool 可在多核执行 CPU 分支。公开 engine 前配置 pool。

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

`27_async_concurrent_runs.cpp` 展示一个 io_context 的多个 session；`05_parallel_fanout.cpp` 展示一次 run 内的分支。历史吞吐量与内存测量见[性能详解](performance-deep-dive.md)，不保证安全的 session 数上限。

## shared engine 规则

- 独立 session 使用不同 `thread_id`。同 id 并发执行的 checkpoint 交错顺序未规定；需要有序 history 时在 host 串行化。
- 向 execution/admin 线程公开前完成 setter 和 tool binding。运行中 resize worker pool 是错误。
- 一个 engine 上管理与执行互斥。run/resume 期间 state/history read、update、fork 以 `std::logic_error` 拒绝；管理期间也拒绝执行。cancel/drain 并等待完成后再重试管理。
- 共享 store 的不同 engine 在此 admission 边界之外；由 host 协调。
- node instance 在各 run 重用。run 级 scratch state 放在 channel 中；custom node/provider/tool/store 须无状态或同步。内置 in-memory store 使用 mutex。

Provider 调用拥有 prepared request 和 runtime client。C++ event view 仅在 callback 中有效，需保留的 data 须复制。native replay 和 accounting 权限需要真实 custody，不能通过 portable JSON 重建获得。参见[异步指南](ASYNC_GUIDE.md)。

## 有界同步 admission

链接 `neograph::util` 使用 `RequestQueue`。它采用 `moodycamel::ConcurrentQueue`，idle worker 在 condition variable 等待。pending-slot 上限限制排队 session 数，不限制运行中 session 的 memory。

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

queue 满时返回 `accepted=false` 和 invalid future。内部 enqueue 失败返回 `false` 和含 `std::runtime_error` 的 valid future；须观察 future，不要把所有拒绝当作普通饱和。构造至少需要一个 worker。

`close()` 幂等：拒绝新 submit，允许 claimed work 完成，将 unclaimed future 以 `std::runtime_error("RequestQueue is closed")` 完成。worker 调用 close 会启动 shutdown，不等待自身。destructor 走同一路径，不会默默遗留 accepted future。

## checkpoint I/O 和 Python

in-memory checkpoint 在 caller 下使用 mutex；SQLite 与 sync custom backend 将 blocking work 移交 bounded worker。PostgreSQL 使用无 pipeline batching 的 nonblocking libpq I/O。`NEOGRAPH_BUILD_POSTGRES=ON` 启用需要 libpq 开发文件的可选 target；`OFF` 仅移除此可选依赖。

Python callback 在 GIL 下执行。CPU-bound Python node/reducer 不能仅靠增加 worker_count 获得并行；native call 仅在自身实现释放 GIL 时可重叠。typed provider invoke/dispatch 释放 GIL，可通过 `asyncio.to_thread` 调用。这不公开 native provider asyncio awaitable。

## runtime 依赖和平台验证

Core 始终链接外部 `SchemaProvider::runtime`，包括 `NEOGRAPH_BUILD_LLM=OFF`。禁用 PostgreSQL、LLM node 或 NeoGraph 可选 CurlH2Pool 不会移除 SDK runtime 的 libcurl 要求。提供匹配的 installed SDK 或 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`。

source resolution 依次采用显式 SDK source directory、installed package、revision-pinned 公开 archive fallback（默认 `NEOGRAPH_FETCH_SCHEMAPROVIDER=ON`）。installed SDK 离线构建时将 flag 设为 `OFF`，在 `CMAKE_PREFIX_PATH` 提供 prefix。NeoGraph 与 SDK source 配置要求 CMake 3.20+。

当前 SDK runtime/archive 验证范围是 Linux/POSIX。已有 Linux/macOS/Windows package metadata 不证明新依赖在所有平台可用。macOS、Windows、WASM 各自需要 runtime/build 验证；portable executor API 不能代替该验证。
