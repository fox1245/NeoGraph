<!-- neograph-i18n: source=docs/ASYNC_GUIDE.md locale=zh-CN source_sha256=6683a6d1c43ac965183efed84bcb41bd6eb07d6f3cd7aa66f84de0dd8b61dea3 -->
# NeoGraph 异步指南

**Languages:** [English](ASYNC_GUIDE.md) | [한국어](ASYNC_GUIDE.ko.md) | [日本語](ASYNC_GUIDE.ja.md) | [简体中文](ASYNC_GUIDE.zh-CN.md)

## 执行入口

`GraphEngine::run`、`run_stream` 和 `resume` 通过一个同步桥驱动与它们对应的协程版本。`run_async`、`run_stream_async` 和 `resume_async` 返回 `asio::awaitable<RunResult>`，并使用调用方的 executor。要一直驱动这个 executor，直到操作结束；回调还在用的资源，不要提前销毁。

`EngineConfig::worker_count` 默认为 `1`，没有引擎自己的 fan-out 线程池。I/O 密集的分支在挂起时可以重叠；单个 executor 线程上 CPU 密集的函数体则是串行的。如果这些函数体需要用到多个核，请在发布引擎之前配置 worker 线程池。参见[并发指南](concurrency.md)和 `examples/27_async_concurrent_runs.cpp`。

## 先准备再分发的 provider 调用

用封闭的 `sp::descriptor::ValidatedDescriptor`、`sp::runtime::Options`，以及可选的带类型 `SchemaProvider::Defaults` 创建 `SchemaProvider`。运行时使用 libcurl；旧的描述符解释器、`prefer_libcurl` 选择器和 Responses WebSocket 路径都已移除。

`make_provider_request` 构造出某个请求族专属的带类型 payload。`ProviderMode::Collect` 是默认值；流式要显式选择 `Stream`，与 `on_event` 无关。`prepare` 只校验并编码一次。`dispatch(_async)` 把只能移动的句柄消耗掉，只能用一次；`invoke(_async)` 把准备和分发合在一起。返回的 awaitable 自己持有请求和客户端的状态，所以原始的请求和 Provider 不必活到被调度的时候。

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

C++ 的 `on_event` 收到的是借用的 `sp::Event` 视图；回调结束后还要用的字节必须复制出来。事件不只有文本，还包括用量、reasoning、工具和原始的 wire 观测数据。不可变的 `sp::runtime::Result` 持有一个 `sp::Completion` 或 `sp::Failure`，其中包括失败时的部分输出证据。observer 失败时，该结果保留在 `ProviderObserverError::outcome()` 中，回调抛出的异常保留在 `cause()` 中。

分发过程等待的是一个容量为 1、会合并的通知；有界的 Bridge 保存事件和结果。通知本身不是事件队列。取消会把停止请求转发给 SDK；`operation.join()` 保证回调已经返回、准入(admission)槽位已经释放，observer 或资源出错时也是如此。截止时间限制整个操作，而调用方停止等待，并不能证明服务端从未收到过这个请求。

要做持久的分发，需要先把 `Provider::request_digest()` 绑定到装配结果，预留一笔已准入的额度，并持久化回执，然后才能通过 `ControlledProvider::dispatch_prepared(_async)` 消耗同一个句柄。重复的回执并不授权重新分发。预留是从已准入的消费额度中扣减或冻结，它不是 provider 上报的用量，也不是发票。缺失的计数器仍然是未知；送达状态未知或只有部分证据，不足以作为释放未决冻结额度的理由。

完整的消息在内存中保留真实的 native 续接状态。可移植的 JSON 投影携带的是观测数据，不具备 native replay 或资金方面的权限。native 续接状态只能通过已准入的 `sp::NativeArchive` 持久化；不要根据文本或原始 JSON 去重建它。参见[迁移指南](migration-v0.4-to-v1.0.md)。

## 自定义 provider 与节点

Provider 的子类只需实现 `get_name`、`family` 和 `prepare`；通用的 invoke/dispatch 方法不是可重写的 completion 钩子。

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

`prepare_local` 供持有真实本地分发状态的 C++ 适配器使用。捕获时要捕获持有所有权的共享状态，而不是借用的 `this`。Python 子类把准备工作委托给真实的 provider；它们无法伪造 outcome，也无法伪造本地分发的权限。

自定义图节点重写 `run(NodeInput) -> asio::awaitable<NodeOutput>`。从 `in.state` 和 `in.ctx` 读取，只通过非空的 `in.stream_cb` 发出流式内容，把写入、Command 和 Send 放进同一个 `NodeOutput` 返回。同步和异步的流式运行，引擎对每次分发都只调用该节点一次。完整的 provider 历史请用 `RunConfig::provider_messages`；`ChatMessage` 是方便图使用的类型，不是 SDK 的消息模型。

<a id="94-checkpointstore"></a>
## CheckpointStore 适配器

`CheckpointStoreCore` 要求实现五个同步操作：save、load_latest、load_by_id、list 和 delete_thread。`adapt_checkpoint_store` 提供引擎所需的契约，并把阻塞的工作交给数量有限的 worker。`AsyncCheckpointStore` 要求实现与之对应的五个异步操作；`adapt_async_checkpoint_store` 提供一个显式的同步管理门面。

旧版 `CheckpointStore` 的同步默认实现会以 `std::logic_error` 失败；异步默认实现则会把同步的重写交给 worker 去做。只支持异步的旧子类必须迁移到显式的异步能力或适配器。内存中的操作在调用方线程上使用受互斥锁保护的存储，SQLite 把阻塞的工作交给 worker，PostgreSQL 使用非阻塞的 libpq I/O，不做 pipeline 批处理。

待定写入（pending write）的持久化是单独的 `PendingWritesCheckpointStore` 能力。没有它时，恢复会把整个超步重放一遍；空操作的方法不会对外部副作用去重。Python 的检查点子类实现同步方法；原生异步的后端由 C++ 适配器承载。

<a id="95-mcpclient"></a>
## MCPClient

`rpc_call` 同步地驱动 `rpc_call_async`。MCPClient 不是用来自定义传输层的子类接口。HTTP 调用可以重叠；一个 stdio 会话把帧的写入串行化，并由一个读取方按 JSON-RPC id 把回复分发出去。取消某一个等待方，不会关闭共享的传输层。串行的子进程仍然会限制吞吐量。

<a id="96-tool-vs-asynctool"></a>
## Tool 与 AsyncTool

同步的 Tool 实现 `execute`、`get_definition` 和 `get_name`。AsyncTool 实现 `execute_async`、`get_definition` 和 `get_name`；它的同步 `execute` 是 final 的，在一个私有的 `run_sync` 上下文中驱动异步版本。不要两套接口都重写，也不要用耗时的同步工作去阻塞共享的事件循环线程。

## 通用的 HTTP 流式传输

NeoGraph 的 `async_post_stream` 仍是一个独立的通用 HTTP 工具，供保留下来的使用方使用；SDK 的 libcurl 分发并没有取代它。它支持分块（chunked）、有上限的 `Content-Length`，以及以连接关闭为结束标志的响应体。非空的定长响应体只会向回调交付一次；长度为零的响应体不会触发回调。返回的 `HttpStreamResponse.status` 会保留 200 和非 2xx 的状态码，因此调用方可以解读 JSON 错误响应体，而不会把非 SSE 的回复当成空的成功。

`RequestOptions` 默认的状态行/头部上限是 64 KiB，解码后响应体上限是 16 MiB，传输块上限是 1 MiB；设为零表示取消对应的上限。定长分支在分配内存之前就检查响应体上限。无效或有歧义的分帧、过早出现的 EOF，以及已经缓冲的多余字节，都会被拒绝；但这并不承诺能检测到返回之后才到达的字节。重定向的响应体单独处理。每一跳的默认超时为零，重定向默认关闭。`connect_timeout`（默认为零，即不限制）只约束 `async_post`、`async_get`、`async_post_stream` 和 `async_request_stream` 的域名解析、TCP 连接和 TLS 握手，连接池不读取它。这些通用选项不是 SDK 的策略，也不是模型消费额度的授权。

## Python 与生命周期的边界

Python 导出了 `prepare`、`dispatch` 和 `invoke`，并配有带类型的 `ProviderRequest`、`PreparedProviderRequest` 和不可变的 `ProviderOutcome`。invoke/dispatch 会释放 GIL；回调和 Python 对象的销毁会获取 GIL。在 asyncio 的边界上，使用 `asyncio.to_thread(provider.invoke, request)`。C++ provider 的 awaitable 不会自动转换成 asyncio 的 awaitable。参见 [Python 绑定](python-binding.md)。

`run_sync` 每次调用都会创建一个私有的单线程上下文；`run_sync_pool` 每次调用创建一个线程池。绑定到这些 executor 的句柄，不能逃出这次调用。要返回另一个 awaitable，可以把它写成普通函数直接返回，也可以在协程体里使用 `co_return co_await`；不要在协程体里混用普通的 `return`。

## 构建

`neograph::core` 需要外部的 `SchemaProvider::runtime`，`NEOGRAPH_BUILD_LLM=OFF` 时也一样。请使用已安装的 SDK 前缀，或设置 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`。构建 SDK 需要 CMake 3.20 及以上、C++20、Python、独立版 Asio、yyjson、libcurl 7.88 及以上版本和 OpenSSL Crypto。WASM 上的运行时集成尚未确立。

配置 NeoGraph 需要 CMake 3.20 及以上。依赖的查找顺序是：先用显式指定的 SDK 源码目录，没有的话用已安装的包，最后才用默认的、固定了修订版本的公开归档。离线配置时，使用已安装的 SDK，设置 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`，并把 `CMAKE_PREFIX_PATH` 设为该 SDK 的前缀。下载或从源码构建的 SDK 使用 NeoGraph 仓库中自带的 Asio 和 yyjson，但仍然需要系统的开发依赖。
