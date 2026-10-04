<!-- neograph-i18n: source=docs/ASYNC_GUIDE.md locale=zh-CN source_sha256=a48e529a151529d9711ee297aed8bc4b7e603c0198c596188608cb6a8565dc13 -->
# NeoGraph 异步指南

**Languages:** [English](ASYNC_GUIDE.md) | [한국어](ASYNC_GUIDE.ko.md) | [日本語](ASYNC_GUIDE.ja.md) | [简体中文](ASYNC_GUIDE.zh-CN.md)

## 执行入口

`GraphEngine::run`、`run_stream`、`resume` 通过同步桥接驱动协程实现。`run_async`、`run_stream_async`、`resume_async` 返回 `asio::awaitable<RunResult>`，使用调用方 executor。持续驱动 executor 直到操作结束，不要提前销毁 callback 使用的资源。

`EngineConfig::worker_count` 默认是 `1`，没有 engine 自有 fan-out pool。I/O 分支挂起时可以重叠推进，单个 executor 线程上的 CPU 工作则串行执行。需要多核时，在公开 engine 前配置 pool。参见[并发](concurrency.md)和 `examples/27_async_concurrent_runs.cpp`。

## 准备后的 provider 调用

`SchemaProvider` 从闭合的 `sp::descriptor::ValidatedDescriptor`、`sp::runtime::Options` 和可选 typed `SchemaProvider::Defaults` 构建。runtime 使用 libcurl；旧 descriptor interpreter、`prefer_libcurl` 选择器和 Responses WebSocket 路径已删除。

`make_provider_request` 构建各 family 的 typed payload。默认是 `ProviderMode::Collect`；独立于 `on_event` 显式选择 `Stream`。`prepare` 验证、编码一次，`dispatch(_async)` 消费仅可移动 handle 一次。`invoke(_async)` 合并两个阶段。返回的 awaitable 拥有 request/client 状态，因此原 request 和 Provider 无须存活到调度之后。

```cpp
#include <neograph/async/run_sync.h>
#include <neograph/provider.h>

// provider owns a validated descriptor and SDK runtime policy.
auto request = neograph::make_provider_request(
    *provider, model, messages, {}, {}, neograph::ProviderMode::Collect);
request.cancel_token = cancel_token;
request.options.deadline = deadline;
auto prepared = provider->prepare(std::move(request));
auto result = neograph::async::run_sync(
    provider->dispatch_async(std::move(prepared)));
```

C++ `on_event` 接收借用的 `sp::Event` view；复制 callback 后需要的 bytes。event 包含 usage、reasoning、tool 和 raw wire 观察，不仅是 text。不可变 `sp::runtime::Result` 拥有 `sp::Completion` 或带部分失败证据的 `sp::Failure`。observer 失败时，`ProviderObserverError::outcome()` 保留结果，`cause()` 保留 callback 异常。

dispatch 等待容量为一的合并通知；bounded Bridge 保存 event 和结果。通知本身不是 event queue。cancel 向 SDK 传递 stop，`operation.join()` 即使在 observer/resource 失败时也等待 callback 返回和 admission slot 释放。deadline 限制操作，但调用方停止不证明 server 未收到请求。

持久 dispatch 将 `Provider::request_digest()` 绑定到 assembly，预留获准 claim，保存 receipt，再通过 `ControlledProvider::dispatch_prepared(_async)` 消费同一 handle。重复 receipt 不授予重发权限。预留扣减或保留已获准的支出权限，不是 provider 报告 usage 或账单。缺失 counter 保持 unknown；部分/交付未知的证据不能解除未解决 hold。

完整 message 在内存中保留真实 native continuation。portable JSON projection 是观察值，不是 native replay 或财务权限。仅通过获准的 `sp::NativeArchive` 持久化 native continuation，不从 text 或 raw JSON 重建。参见[迁移指南](migration-v0.4-to-v1.0.md)。

## 自定义 provider 和 node

Provider 子类只实现 `get_name`、`family`、`prepare`；通用 invoke/dispatch 不是 virtual completion hook。

```cpp
#include <neograph/provider.h>

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
    neograph::PreparedProviderRequest prepare(neograph::ProviderRequest request) override {
        return prepare_runtime(client_, std::move(request));
    }
};
```

C++ adapter 拥有真实 local-dispatch 状态时可用 `prepare_local`。捕获拥有所有权的 shared state，不捕获借用的 `this`。Python 子类将准备委托给真实 provider，不能伪造 outcome 或 local-dispatch 权限。

自定义 graph node override `run(NodeInput) -> asio::awaitable<NodeOutput>`。读取 `in.state`、`in.ctx`，仅通过非空 `in.stream_cb` 发出 event，并在一个 `NodeOutput` 中返回 writes/Command/Send。sync 和 async streaming 每次 dispatch 都只调用一次。用 `RunConfig::provider_messages` 传入完整 provider history。`ChatMessage` 是 graph 便利类型，不是 SDK message model。

<a id="94-checkpointstore"></a>
## CheckpointStore adapter

`CheckpointStoreCore` 要求 save、load_latest、load_by_id、list、delete_thread 五个 sync 操作。`adapt_checkpoint_store` 提供 engine 契约，将 blocking 工作移到 bounded worker。`AsyncCheckpointStore` 要求五个 async 对应操作，`adapt_async_checkpoint_store` 提供显式 sync 管理 facade。

legacy `CheckpointStore` 的 sync 默认实现以 `std::logic_error` 失败，async 默认实现 offload sync override。async-only 子类须迁移到显式 async capability/adapter。in-memory 操作在调用方用 mutex，SQLite offload blocking 工作，PostgreSQL 用无 pipeline batching 的 nonblocking libpq I/O。

pending-write 持久性是单独的 `PendingWritesCheckpointStore` capability。没有时 resume 重放整个 super-step；no-op method 不会去重外部效果。Python checkpoint 子类实现 sync method，async-native backend 由 C++ adapter 承载。

<a id="95-mcpclient"></a>
## MCPClient

`rpc_call` 同步驱动 `rpc_call_async`。MCPClient 不是自定义 transport 的继承接口。HTTP 调用可重叠；stdio session 串行写 frame，单个 reader 按 JSON-RPC id 路由 reply。取消一个 waiter 不关闭 shared transport。串行 subprocess 仍限制吞吐量。

<a id="96-tool-vs-asynctool"></a>
## Tool 与 AsyncTool

sync Tool 实现 `execute`、`get_definition`、`get_name`。AsyncTool 实现 `execute_async`、`get_definition`、`get_name`；sync `execute` 为 final，驱动 private `run_sync` context。不要 override 两套执行接口，也不要用长 sync 工作阻塞 shared event-loop 线程。

## Generic HTTP streaming

NeoGraph 的 `async_post_stream` 仍是 retained consumer 使用的独立 generic HTTP utility；SDK libcurl dispatch 不替代它。支持 chunked、有界 `Content-Length` 和 close-delimited response body。非空 fixed-length body 向 callback 交付一次，零长度 body 不触发 callback。返回的 `HttpStreamResponse.status` 保留 200 与 non-2xx status，调用方可以解释 JSON error body，不把 non-SSE reply 当作空成功。

`RequestOptions` 默认限制为 status/header 64 KiB、decoded body 16 MiB、transfer chunk 1 MiB；各值为零时关闭对应限制。Fixed-length 分支在分配前检查 body limit。无效或歧义 framing、premature EOF 和已在 buffer 中的 surplus byte 均被拒绝；不保证检测返回后才到达的 byte。Redirect body 单独处理。默认 per-hop timeout 为零，redirect 禁用；这些 generic option 不是 SDK policy 或 model spending grant。

## Python 和生命周期边界

Python 公开使用 typed `ProviderRequest`、`PreparedProviderRequest`、不可变 `ProviderOutcome` 的 `prepare`、`dispatch`、`invoke`。invoke/dispatch 释放 GIL；callback 和 Python object 销毁获取 GIL。asyncio 边界使用 `asyncio.to_thread(provider.invoke, request)`；没有 C++ provider awaitable 到 asyncio awaitable 的自动转换。参见 [Python binding](python-binding.md)。

`run_sync` 每次调用创建 private 单线程 context，`run_sync_pool` 创建调用级 pool。绑定这些 executor 的 handle 不得逃出调用。普通函数可直接返回其他 awaitable；协程正文可用 `co_return co_await`，不要混入普通 `return`。

## 构建与历史证据

即使 `NEOGRAPH_BUILD_LLM=OFF`，`neograph::core` 也要求外部 `SchemaProvider::runtime`。使用已安装 SDK prefix 或 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`。SDK 构建需要 CMake 3.20+、C++20、Python、standalone Asio、yyjson、libcurl 7.88+、OpenSSL Crypto。已记录的 interface-4 之前 runtime/archive 验证覆盖 Linux/POSIX，不验证 interface 4；已有 macOS/Windows metadata 不验证新依赖，WASM 集成也未确立。

NeoGraph 配置要求 CMake 3.20+。优先显式 SDK source directory，否则使用 installed package，再使用默认 revision-pinned 公开 archive fallback。用 installed SDK 离线配置时设 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`，在 `CMAKE_PREFIX_PATH` 指定 SDK prefix。fetched/source SDK 使用 NeoGraph 已提交的 Asio/yyjson，但仍需 system 开发依赖。

[Stage 3 设计](ASYNC_STAGE3_DESIGN.md)是 2026 年 4 月提案的归档记录。2.0/3.0 名称、completion crossover、test/benchmark 数量是历史 milestone，不是当前版本、支持的签名或新通过声明。版本来自 `pyproject.toml`；历史测量见[性能详解](performance-deep-dive.md)。

原 Stage 3 guide 记录当时 sync 路径的已有 test 276+、engine seq ~30 µs/par ~205 µs、HTTP async_pool 17834 ops/s（Stage 2 async 8401/s、sync 6064/s）、50000 timer fan-out 541K ops/s 与 RSS 67 MB；还记录三个 50 ms agent 在 50 ms 完成，三个 researcher 在 150 ms 完成（串行 370 ms）。这些是当时 workload 的历史观察，不是当前 provider 切换结果。
