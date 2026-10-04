# Concurrency & Async

**Languages:** [English](concurrency.md) | [한국어](concurrency.ko.md) | [日本語](concurrency.ja.md) | [简体中文](concurrency.zh-CN.md)

## Choosing the executor

Synchronous `run`, `run_stream` and `resume` block the calling thread while driving the same coroutine implementation as their async peers. A host worker pool can run independent sessions concurrently. The async peers return `asio::awaitable<RunResult>`; drive them on an executor you own. An awaitable does not make arbitrary user code nonblocking.

`EngineConfig::worker_count = 1` is the default and creates no engine-owned fan-out pool. Suspended I/O branches can overlap on one thread; CPU work on that thread serializes. A multithreaded caller executor or an opt-in engine worker pool can run CPU branches on multiple cores. Configure the pool before publishing the engine.

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

Example `27_async_concurrent_runs.cpp` shows multiple sessions on one io_context; `05_parallel_fanout.cpp` shows branches within one run. Historical throughput and memory measurements are in [performance deep-dive](performance-deep-dive.md), not guarantees about a safe session-count ceiling.

## Shared-engine rules

- Use distinct `thread_id` values for independent sessions. Concurrent executions with the same id have unspecified checkpoint interleaving; serialize at the host when history order matters.
- Call configuration setters and bind tools before exposing the engine to execution or administration threads. Resizing the worker pool during execution is a hard error.
- Administration and execution are mutually exclusive on one engine. State/history reads, update and fork reject with `std::logic_error` while any run/resume is active; execution rejects while administration is active. Cancel/drain and await completion before retrying administration.
- Different engines sharing a store are outside that admission boundary; coordinate them at the host.
- Node instances are reused across runs. Keep per-run scratch state in channels; custom nodes, providers, tools and stores must be stateless or synchronized. The bundled in-memory stores use mutexes.

Provider calls own their prepared request and runtime client. Their C++ event views are callback-scoped; copy retained data. Native replay state and accounting authority require authentic custody, not a portable JSON reconstruction. See [the async guide](ASYNC_GUIDE.md).

## Bounded synchronous admission

Link `neograph::util` to use `RequestQueue`. It uses `moodycamel::ConcurrentQueue`, with idle workers waiting on a condition variable. The pending-slot limit bounds queued sessions, not memory consumed by a running session.

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

A full queue returns `accepted=false` and an invalid future. Internal enqueue failure returns `false` with a valid future carrying `std::runtime_error`; observe it rather than treating every rejection as ordinary saturation. Construction requires at least one worker.

`close()` is idempotent. It rejects new submissions, lets claimed work finish and completes unclaimed futures with `std::runtime_error("RequestQueue is closed")`. A worker calling close initiates shutdown without waiting for itself. The destructor uses the same path; accepted futures are not silently stranded.

## Checkpoint I/O and Python

In-memory checkpoint operations run on the caller under mutexes; SQLite and synchronous custom backends offload blocking work to bounded workers. PostgreSQL uses nonblocking libpq I/O, without pipeline batching. `NEOGRAPH_BUILD_POSTGRES=ON` enables that optional target and requires libpq development files; `OFF` removes that optional dependency only.

Python callbacks execute under the GIL. CPU-bound Python nodes/reducers cannot gain parallelism merely by increasing worker_count; native calls can overlap only when their own implementation releases the GIL. Typed provider invoke/dispatch release the GIL and can be called through `asyncio.to_thread`. This does not expose a native provider asyncio awaitable.

## Runtime dependency and platform qualification

Core always links external `SchemaProvider::runtime`, including `NEOGRAPH_BUILD_LLM=OFF`. Disabling PostgreSQL, LLM nodes or NeoGraph's optional CurlH2Pool does not remove that runtime's libcurl requirement. Supply the matching installed SDK or `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`.

Source resolution prefers an explicit SDK source directory, then an installed package, then the revision-pinned public archive fallback (`NEOGRAPH_FETCH_SCHEMAPROVIDER=ON` by default). Configure an offline installed-SDK build with that flag `OFF` and its prefix in `CMAKE_PREFIX_PATH`. NeoGraph and SDK source configuration require CMake 3.20+.

The current SDK runtime/archive is qualified on Linux/POSIX. NeoGraph's existing Linux/macOS/Windows package metadata is not evidence that the new dependency works on all those platforms. macOS, Windows and WASM require their own runtime/build qualification; a portable executor API alone does not establish it.
