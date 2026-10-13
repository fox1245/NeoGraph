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

## Runs of one conversation

A conversation can be stored in two layers, and they treat concurrent writers differently.

- **Context feeds** fail closed. `ContextStore::append_history` takes the head the writer expects. An append whose expected head, sequence or predecessor is not the feed's current one returns `Conflict` and stores nothing, so two writers cannot both extend the same head. Appending the exact record that is already stored returns `AlreadyPresent`, also after the head has moved.
- **Checkpoint stores** are last-writer-wins per `thread_id`. Each call is atomic, but a sequence of calls is not. When two runs of one `thread_id` overlap, their `save_checkpoint` calls interleave in no defined order, the last save wins, and `load_latest` returns the last saver's checkpoint. Neither the engine nor a store rejects the second run.

Serializing the runs and resumes of one `thread_id`, and of one feed, is the host's job. That holds across processes too. The strict profile does not do it: it holds one active epoch for all its callers and does not look at `thread_id`. Overlapping requests for one conversation are usually accidents, such as a retried webhook or a double click, so queue or reject them at the host before they reach the engine.

An in-process guard is planned: it would reject a second run or resume of the same `thread_id` while one is in flight. It would cover one process only. Until it exists, nothing in NeoGraph stops two runs of one `thread_id`. See also [Host-stepped contract](STRICT_RUNTIME_INTERPOSITION.md#host-stepped-contract).

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

## Runtime dependency

Core always links external `SchemaProvider::runtime`, including `NEOGRAPH_BUILD_LLM=OFF`. Disabling PostgreSQL, LLM nodes or NeoGraph's optional CurlH2Pool does not remove that runtime's libcurl requirement. Supply the matching installed SDK or `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`.

Source resolution prefers an explicit SDK source directory, then an installed package, then the revision-pinned public archive fallback (`NEOGRAPH_FETCH_SCHEMAPROVIDER=ON` by default). Configure an offline installed-SDK build with that flag `OFF` and its prefix in `CMAKE_PREFIX_PATH`. NeoGraph and SDK source configuration require CMake 3.20+.

WASM runtime integration with the SDK is not established; a portable executor API alone does not provide it.
