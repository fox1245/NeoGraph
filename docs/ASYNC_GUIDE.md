# NeoGraph Async Guide

**Languages:** [English](ASYNC_GUIDE.md) | [한국어](ASYNC_GUIDE.ko.md) | [日本語](ASYNC_GUIDE.ja.md) | [简体中文](ASYNC_GUIDE.zh-CN.md)

## Execution entry points

`GraphEngine::run`, `run_stream` and `resume` drive their coroutine peers through a synchronous bridge. `run_async`, `run_stream_async` and `resume_async` return `asio::awaitable<RunResult>` and use the caller's executor. Drive that executor until the operations finish; do not destroy resources still used by callbacks.

The default `EngineConfig::worker_count` is `1`, with no engine-owned fan-out pool. I/O-bound branches can overlap when they suspend; CPU-bound bodies on a single executor thread serialize. Configure a worker pool before publishing the engine if those bodies need multiple cores. See [concurrency](concurrency.md) and `examples/27_async_concurrent_runs.cpp`.

## Prepared provider calls

Create `SchemaProvider` from a closed `sp::descriptor::ValidatedDescriptor`, `sp::runtime::Options` and optional typed `SchemaProvider::Defaults`. The runtime uses libcurl; the old descriptor interpreter, `prefer_libcurl` selector and Responses WebSocket path are removed.

`make_provider_request` builds a family-specific typed payload. `ProviderMode::Collect` is the default; select `Stream` explicitly, independently of `on_event`. `prepare` validates and encodes once. `dispatch(_async)` consumes the move-only handle once; `invoke(_async)` combines preparation and dispatch. The returned awaitable owns request/client state, so the original request and Provider need not survive scheduling.

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

C++ `on_event` receives borrowed `sp::Event` views; copy bytes needed after the callback. Events include usage, reasoning, tools and raw wire observations, not only text. An immutable `sp::runtime::Result` owns a `sp::Completion` or `sp::Failure`, including partial failure evidence. Observer failure retains that result in `ProviderObserverError::outcome()` and the callback exception in `cause()`.

Dispatch waits on a capacity-one coalesced notification; the bounded Bridge retains events and the result. Notification is not the event queue. Cancellation forwards stop to the SDK; `operation.join()` fences callback return and admission-slot release, including observer/resource failure. A deadline bounds the operation, and stopping the caller does not prove that the server never received the request.

For durable dispatch, bind `Provider::request_digest()` to the assembly, reserve an admitted claim and persist the receipt before consuming the same handle through `ControlledProvider::dispatch_prepared(_async)`. A duplicate receipt does not authorize redispatch. Reservations debit or hold already-admitted spending authority; they are not provider-reported usage or invoices. Missing counters remain unknown; delivery-unknown or partial evidence cannot justify releasing an unresolved hold.

Full messages retain authentic native continuation in memory. Portable JSON projections carry observations, not native replay or financial authority. Persist native continuation only through an admitted `sp::NativeArchive`; do not reconstruct it from text or raw JSON. See [the migration guide](migration-v0.4-to-v1.0.md).

## Custom providers and nodes

Provider subclasses implement only `get_name`, `family` and `prepare`; the common invoke/dispatch methods are not virtual completion hooks.

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

`prepare_local` is available to C++ adapters that own genuine local-dispatch state. Capture owned shared state, not a borrowed `this`. Python subclasses delegate preparation to an authentic provider; they cannot fabricate outcomes or local-dispatch authority.

Custom graph nodes override `run(NodeInput) -> asio::awaitable<NodeOutput>`. Read `in.state` and `in.ctx`, emit only through a non-null `in.stream_cb`, and return writes/Command/Send in one `NodeOutput`. The engine invokes the node once per dispatch for sync and async streaming alike. Use `RunConfig::provider_messages` for full provider history; `ChatMessage` is a graph convenience type, not the SDK message model.

<a id="94-checkpointstore"></a>
## CheckpointStore adapters

`CheckpointStoreCore` requires five synchronous operations: save, load_latest, load_by_id, list and delete_thread. `adapt_checkpoint_store` supplies the engine contract and offloads blocking work to bounded workers. `AsyncCheckpointStore` requires their five async counterparts; `adapt_async_checkpoint_store` provides an explicit synchronous administration facade.

The legacy `CheckpointStore` synchronous defaults fail with `std::logic_error`; async defaults offload synchronous overrides. An async-only legacy subclass must migrate to the explicit async capability/adapter. In-memory operations use mutex-protected storage on the caller, SQLite offloads blocking work, and PostgreSQL uses nonblocking libpq I/O without pipeline batching.

Pending-write durability is a separate `PendingWritesCheckpointStore` capability. Without it, resume replays the full super-step; no-op methods do not deduplicate external effects. Python checkpoint subclasses implement synchronous methods; C++ adapters host async-native backends.

<a id="95-mcpclient"></a>
## MCPClient

`rpc_call` drives `rpc_call_async` synchronously. MCPClient is not a custom-transport subclass interface. HTTP calls overlap; one stdio session serializes frame writes and uses one reader to route replies by JSON-RPC id. Cancelling one waiter does not close the shared transport. A serial subprocess still limits throughput.

<a id="96-tool-vs-asynctool"></a>
## Tool vs AsyncTool

A synchronous Tool implements `execute`, `get_definition` and `get_name`. An AsyncTool implements `execute_async`, `get_definition` and `get_name`; its synchronous `execute` is final and drives a private `run_sync` context. Do not override both surfaces or block a shared event-loop thread with long synchronous work.

## Generic HTTP streaming

NeoGraph's `async_post_stream` remains a separate generic HTTP utility used by retained consumers; SDK libcurl dispatch does not replace it. It supports chunked, bounded `Content-Length`, and close-delimited response bodies. A nonempty fixed-length body is delivered once to the callback; a zero-length body emits no callback. The returned `HttpStreamResponse.status` preserves 200 and non-2xx statuses, so callers can interpret a JSON error body instead of treating a non-SSE reply as an empty success.

`RequestOptions` defaults to a 64 KiB status/header limit, 16 MiB decoded-body limit and 1 MiB transfer-chunk limit; zero disables each corresponding limit. The fixed-length branch checks the body limit before allocation. Invalid or ambiguous framing, premature EOF, and already-buffered surplus bytes are rejected; this is not a promise to detect bytes arriving after return. Redirect bodies are handled separately. The default per-hop timeout is zero and redirects are disabled; these generic options are not SDK policy or a model spending grant.

## Python and lifetime boundaries

Python exports `prepare`, `dispatch` and `invoke` with typed `ProviderRequest`, `PreparedProviderRequest` and immutable `ProviderOutcome`. Invoke/dispatch release the GIL; callbacks and Python-object destruction acquire it. Use `asyncio.to_thread(provider.invoke, request)` at an asyncio boundary. There is no automatic conversion of a C++ provider awaitable to an asyncio awaitable. See [Python binding](python-binding.md).

`run_sync` creates a private single-thread context per call; `run_sync_pool` creates a per-call pool. Handles bound to those executors must not escape the call. A coroutine may return another awaitable directly as a regular function, or use `co_return co_await` in a coroutine body; do not mix ordinary `return` into that body.

## Build and historical evidence

External `SchemaProvider::runtime` is required by `neograph::core`, even with `NEOGRAPH_BUILD_LLM=OFF`. Use an installed SDK prefix or `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`. The SDK build requires CMake 3.20+, C++20, Python, standalone Asio, yyjson, libcurl 7.88+ and OpenSSL Crypto. Recorded pre-interface-4 runtime/archive qualification covers Linux/POSIX; it does not qualify interface 4. Existing macOS/Windows metadata does not qualify this dependency, and WASM runtime integration is not established.

NeoGraph configuration requires CMake 3.20+. Resolution uses an explicit SDK source directory first, otherwise an installed package, then the default revision-pinned public archive fallback. Set `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` with an installed SDK for offline configuration; set `CMAKE_PREFIX_PATH` to that SDK prefix. A fetched/source SDK uses NeoGraph's checked-in Asio and yyjson, but still needs its system development dependencies.

The [Stage 3 design](ASYNC_STAGE3_DESIGN.md) is an archived April 2026 proposal. Its 2.0/3.0 labels, completion crossover and test/benchmark counts are historical milestones, not current release numbers, supported signatures or new pass claims. Current release numbers come from `pyproject.toml`; historical measurements are in [performance deep-dive](performance-deep-dive.md).

The original Stage 3 guide recorded 276+ then-existing tests on its sync path, seq ~30 µs/par ~205 µs engine overhead, HTTP async_pool at 17834 ops/s versus Stage 2 async 8401/s and sync 6064/s, and 50000-timer fan-out at 541K ops/s with 67 MB RSS. It also recorded three 50 ms agents completing in 50 ms and three researchers completing in 150 ms versus 370 ms sequential. These are historical observations of those workloads, not results from the current provider cutover.
