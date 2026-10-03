# NeoGraph Async Guide

**Languages:** [English](ASYNC_GUIDE.md) | [한국어](ASYNC_GUIDE.ko.md) | [日本語](ASYNC_GUIDE.ja.md) | [简体中文](ASYNC_GUIDE.zh-CN.md)

> Historical Stage 3 (2026-04) design and its measured test counts are preserved as history. Its provider compatibility/crossover decisions are superseded by the typed lossless cutover below; the design ledger is not the current provider API.
Stage 3 / 2026-04 release. Target audience: users migrating existing
NeoGraph code to the async API, or writing new code against it.

This guide covers **what** changed, **why** the shape is what it is,
and **how** to migrate incrementally. For the design rationale behind
individual semesters see [`ASYNC_STAGE3_DESIGN.md`](ASYNC_STAGE3_DESIGN.md);
for the minute-level commit ledger see the git log of the
`feat/async-api` branch.

---

## 1. What's new

Every synchronous I/O point in the engine now has an awaitable peer:

| Layer | Sync | Async peer |
|---|---|---|
| Provider | `invoke` / `dispatch` | `invoke_async` / `dispatch_async` |
| CheckpointStore | `save` / `load_latest` / `load_by_id` / `list` / `delete_thread` / `put_writes` / `get_writes` / `clear_writes` | `*_async` for each |
| GraphNode | — | `run(NodeInput) -> asio::awaitable<NodeOutput>` is the single canonical override |
| GraphEngine | `run` / `run_stream` / `resume` | `run_async` / `run_stream_async` / `resume_async` |
| MCPClient | `rpc_call` | `rpc_call_async` |
| Tool | `execute` (user interface — frozen) | wrap with `AsyncTool` adapter |

The async peers return `asio::awaitable<T>`. Drive them on any
`asio::io_context` (or strand, or thread pool with `any_io_executor`).
One `io_context` can host thousands of concurrent `run_async`
invocations without dedicating an OS thread per run — the concurrency
model that motivated the whole refactor.

The historical Stage 3 report recorded 276+ pre-existing test cases passing its then-current sync path; this is not a current cutover result.

---

<a id="2-the-crossover-default-pattern"></a>
## 2. Prepared provider dispatch (crossover removed)

The public contract is owned typed preparation and dispatch, not paired virtual completion methods. `ProviderRequest.payload` is the SDK variant of Chat, Messages, Responses, Gemini or Interactions requests. `ProviderMode::Collect` / `Stream` selects transport independently of an observer. `on_event` receives borrowed typed `sp::Event` views; copy only data needed after the callback. No raw JSON overrides or native-state import through portable projections are admitted.

`prepare()` validates and encodes exactly once, producing a move-only `PreparedProviderRequest` with the original deadline and cancellation state. Durable callers bind its `Provider::request_digest()` to their assembly, reserve an admitted budget claim, write the dispatch receipt, then consume that same handle through `ControlledProvider::dispatch_prepared(_async)`. They never rebuild a request after the gate. Duplicate receipts never redispatch. Custom providers implement `get_name()`, `family()` and `prepare()` using `prepare_runtime()` or `prepare_local()`; local callbacks capture owned shared state, not `this`.

This is a source and binary break: recompile every C++ consumer and custom provider with matching new headers/libraries. `CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, the descriptor interpreter and Responses WebSocket path are removed, with no aliases or compatibility bridges. The SDK is unstable `0.0.0`, interface revision 3 / shared ABI 3, with out-of-line capability checks; that is not a stable release claim. Current runtime/archive support is Linux/POSIX; no Windows, macOS or WASM runtime qualification is implied. Python provider bindings/wrappers are deferred and not ported by this C++ change.

---

## 3. Migration recipes

### 3.1 Sync caller migrating to async

**Before:**

```cpp
auto result = engine->run_stream(config, event_cb);
```

**After:**

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

The `io.run()` returns when the coroutine completes. For many
concurrent runs, co_spawn each onto the same `io_context` before
calling `io.run()` — see `examples/27_async_concurrent_runs.cpp`.

### 3.2 Writing a new async provider

`prepare()` validates and encodes exactly once, producing a move-only `PreparedProviderRequest` with the original deadline and cancellation state. Durable callers bind its `Provider::request_digest()` to their assembly, reserve an admitted budget claim, write the dispatch receipt, then consume that same handle through `ControlledProvider::dispatch_prepared(_async)`. They never rebuild a request after the gate. Duplicate receipts never redispatch. Custom providers implement `get_name()`, `family()` and `prepare()` using `prepare_runtime()` or `prepare_local()`; local callbacks capture owned shared state, not `this`.

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

### 3.3 Writing an async Tool

The `Tool` interface is sync by design (Stage 3 freezes it to keep
migration cost on existing user tools near zero). Use `AsyncTool`
when you need coroutine-shaped work inside:

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

`AsyncTool::execute` is `final` — it's the sync facade that spins up
a private `io_context` to drive `execute_async`. Overriding both
halves is a contract violation.

### 3.4 Writing a graph node that uses an async provider

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

## 4. Caveats and footguns

### 4.1 GCC 13 coroutine ICEs

Two specific C++20 coroutine shapes trigger GCC 13's
`build_special_member_call` ICE (as of GCC 13.3):

**Shape 1 — `co_await` inside a `catch` block:**

```cpp
try { ... }
catch (const MyError& e) {
    co_await something();  // ICE
}
```

**Workaround — capture the error outside, handle after:**

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

**Shape 2 — nested brace-init inside a coroutine body:**

```cpp
co_await fn(std::vector<std::string>{name},    // ICE
            json{{"key", "value"}});
```

**Workaround — build outside, reference in:**

```cpp
std::vector<std::string> v;
v.push_back(name);
json j;
j["key"] = "value";
co_await fn(v, j);
```

Both shapes surfaced multiple times during Stage 3 and the
workarounds are stable. Clang 18+ and GCC 14+ compile the "natural"
forms without issue, but NeoGraph targets GCC 13 as baseline.

### 4.2 `run_sync` lifetime hazard

`neograph::async::run_sync<T>(asio::awaitable<T>)` creates a fresh
single-threaded `io_context` per call. Any long-lived asio handle
bound to that executor — a socket in a pool, a timer, a file
descriptor — will dangle when `run_sync` returns. This bit the
early ConnPool work and the current architecture sidesteps it by
deliberately NOT pooling anything through the sync facade.

Rule: for resources that must outlive a single call (connection
pools, long-running stream descriptors), only bind them to
executors you own for the process lifetime. The sync facade path
creates fresh connections per request.

### 4.3 `co_return co_await x`, not `return x`

A coroutine function returning `asio::awaitable<T>` must use
`co_return` (or `co_await`) somewhere in its body. Plain `return
other_awaitable()` appears to compile but default-constructs the
wrapped `T` at runtime. Always chain via `co_return co_await`:

```cpp
asio::awaitable<RunResult>
GraphEngine::run_async(const RunConfig& config) {
    co_return co_await execute_graph_async(config, nullptr);
}
```

### 4.4 Streaming from a custom node

`GraphNode::run(NodeInput)` executes once per dispatch. Emit events only when
`in.stream_cb` is non-null, and return the same `NodeOutput` regardless of
whether the caller used a streaming engine entry point. The legacy
The obsolete double-execution fallback no longer exists.

### 4.5 MCP stdio single-session concurrency

One stdio transport owns its `io_context`, subprocess pipes, write semaphore,
and response-id reader. Sibling `rpc_call_async` calls on the **same** session
share only the frame-write lock; reads overlap and the reader routes each
response to its waiting request by JSON-RPC id. They can originate from
different caller executors, including successive `run_sync` graph runs.
Cancelling or timing out one request removes its waiter without closing the
transport or misrouting a late response to another request.

---

## 5. Performance notes

The async wire doesn't make a single agent faster — `bench_neograph`
reports the same seq (~30 µs) and par (~205 µs) numbers as before
Stage 3. The value axis is **concurrency robustness**, not engine
latency.

Measured improvements on real-shape benchmarks:

* `bench_async_http --mode async_pool --concur 1000` — 17834 ops/s,
  vs. Stage 2 async (8401 ops/s) and sync (6064 ops/s).
* `bench_async_fanout --concur 50000` — 541K ops/s, 67 MB RSS. The
  thread-per-agent baseline couldn't scale past ~1000 concurrent
  agents; 50K is now an afternoon's work.
* `examples/27_async_concurrent_runs` — 3 agents × 50ms work on one
  io_context: 50ms total (vs. 150ms sequential).
* `examples/05_parallel_fanout` — 3 parallel researchers on one
  io_context: 150ms total (vs. 370ms sequential).

### When to keep using the sync API

If your workload is ≤ 1000 concurrent agents and each agent runs in
a dedicated OS thread, the sync API remains a perfectly reasonable
choice. Threads are cheap enough at that scale, and the sync code
is simpler to reason about. The async API exists for the workloads
the sync shape can't address — hundreds of long-lived agents
sharing a process, hosting many users from a single event loop,
and so on.

---

## 6. Checklist for a clean migration

- [ ] Identify the agent host pattern: single agent process, pool,
      or shared event loop?
- [ ] If shared event loop → migrate call sites to `run_async` /
      `run_stream_async`.
- [ ] Custom nodes implement `run(NodeInput)` and `co_await` real I/O directly.
- [ ] If your tools do real I/O → derive from `AsyncTool`, override
      `execute_async`.
- [ ] If you use the Postgres checkpoint store → use its `*_async`
      methods on shared event loops. They use libpq's nonblocking wire
      protocol and a coroutine-friendly connection pool.
- [ ] Measure. The value axis is concurrency; if your workload
      isn't concurrency-bound, don't migrate.

---

## 7. What's not covered yet

* **Postgres pipeline mode** — async checkpoint methods already use
  nonblocking libpq I/O, but they do not yet batch multiple commands
  through libpq pipeline mode.
* **`async::HttpResponse` headers map** — the response surface only
  exposes status / body / retry_after / location. Arbitrary header
  access (e.g. MCP session ID header tracking) is a Sem 1
  follow-up.

---

## 8. What changed in 3.0

3.0 (`feat/taskflow-removal`) collapsed sync and async onto one
coroutine runtime by removing Taskflow and routing sync entry points
through `run_sync(execute_graph_async)`. The 2.0 async API shape is
unchanged — the differences are in the defaults and the new opt-ins.

### 8.1 `GraphNode::run(NodeInput)` replaces the legacy override chain

The v0.9.0 v1-preparation release removed the eight `execute*` virtuals.
A custom node now has one override for sync and async engine entry points,
streaming, and control flow:

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

Code migrating from earlier releases must move its state reads to
`in.state`, run metadata to `in.ctx`, streaming sink to `in.stream_cb`,
and writes/`Command`/`Send` values into the returned `NodeOutput`.

### 8.2 `GraphEngine::set_worker_count(N)` — opt-in CPU parallel fan-out

Default: `run_parallel_async` and the multi-Send branch of
`run_sends_async` dispatch branches on whichever executor drives the
current coroutine. For sync `run()` that's a single-threaded
io_context — I/O-bound branches still overlap via co_await suspension,
but CPU-bound branches serialize.

```cpp
EngineConfig engine_config;
engine_config.node_context = ctx;
engine_config.checkpoint_store = store;
engine_config.worker_count = std::thread::hardware_concurrency();
auto engine = GraphEngine::build(def, std::move(engine_config));
// Now run_parallel_async dispatches branches to an engine-owned
// asio::thread_pool of that size.
```

Set this before construction when possible. The compatibility setter must be
called before any concurrent `run()`; rebuilding the pool across in-flight
runs is not safe. `run_async` callers who drive a
multi-threaded `asio::thread_pool` themselves don't need this — their
caller-side executor already parallelizes the branches.

### 8.3 `neograph::async::run_sync_pool(aw, n_threads)` — N-worker sync bridge

```cpp
#include <neograph/async/run_sync.h>

int result = neograph::async::run_sync_pool(
    my_coroutine_that_uses_make_parallel_group(), /*n_threads=*/4);
```

Companion to the existing single-threaded `run_sync`. Spins a fresh
`asio::thread_pool` for the call so inner `make_parallel_group`
branches execute on separate workers. Per-call pool construction
spawns one `std::thread` per worker — cost is non-trivial for hot
paths, so this is for occasional sync-at-the-boundary bridges, not
per-request code.

### 8.4 Removed surfaces

- `NodeExecutor::run_one` / `run_parallel` / `run_sends` (sync) — use
  the `_async` peers.
- `GraphEngine::execute_graph` (sync) — deleted; `run()` /
  `run_stream()` / `resume()` route through the async peer via
  `run_sync`.
- `tf::Executor`, `tf::Taskflow`, the `deps/taskflow/` directory —
  gone. Benchmarks that used Taskflow as a caller-side driver
  (`bench_concurrent_neograph.cpp`) switched to `asio::thread_pool` +
  `asio::post`.

---

## 9. Override decision guide

The public contract is owned typed preparation and dispatch, not paired virtual completion methods. `ProviderRequest.payload` is the SDK variant of Chat, Messages, Responses, Gemini or Interactions requests. `ProviderMode::Collect` / `Stream` selects transport independently of an observer. `on_event` receives borrowed typed `sp::Event` views; copy only data needed after the callback. No raw JSON overrides or native-state import through portable projections are admitted.

### 9.1 Two-minute version

| You write a… | Override | Inherit as-is |
|---|---|---|
| Any custom `GraphNode` | `run(NodeInput)` | `get_name()` is the only other required virtual |
| Provider | `get_name()`, `family()`, `prepare(ProviderRequest)` | `invoke(_async)`, `dispatch(_async)` |
| Native async checkpoint backend | derive `AsyncCheckpointStore`, implement five mandatory async operations; call `adapt_async_checkpoint_store()` | explicit `run_sync` admin facade; no legacy sync override |
| Sync-only checkpoint backend | derive `CheckpointStoreCore`, implement five mandatory sync operations; call `adapt_checkpoint_store()` | async calls offload to bounded workers |
| Custom sync `Tool` | inherit `Tool`, override `execute()` | — |
| Custom async `Tool` | inherit `AsyncTool`, override `execute_async()` | sync `execute()` is `final`, bridges |

### 9.2 `GraphNode`

Always override `run(NodeInput)`. CPU-only work can execute directly before
`co_return`; real asynchronous I/O should be `co_await`ed. The engine invokes
the same method from `run`, `run_async`, streaming, resume, and Send fan-out,
so there is no override-selection matrix and no sync/async fallback recursion.

Do not block a shared single-thread `io_context` for long periods. Move blocking
work to an executor or use coroutine-friendly I/O. `EngineConfig::worker_count`
controls the engine-owned pool used by sync callers that need parallel fan-out.

### 9.3 `Provider`

The public contract is owned typed preparation and dispatch, not paired virtual completion methods. `ProviderRequest.payload` is the SDK variant of Chat, Messages, Responses, Gemini or Interactions requests. `ProviderMode::Collect` / `Stream` selects transport independently of an observer. `on_event` receives borrowed typed `sp::Event` views; copy only data needed after the callback. No raw JSON overrides or native-state import through portable projections are admitted.

`prepare()` validates and encodes exactly once, producing a move-only `PreparedProviderRequest` with the original deadline and cancellation state. Durable callers bind its `Provider::request_digest()` to their assembly, reserve an admitted budget claim, write the dispatch receipt, then consume that same handle through `ControlledProvider::dispatch_prepared(_async)`. They never rebuild a request after the gate. Duplicate receipts never redispatch. Custom providers implement `get_name()`, `family()` and `prepare()` using `prepare_runtime()` or `prepare_local()`; local callbacks capture owned shared state, not `this`.

Optional `ProviderControls` are caller choices, not mandatory defaults or silently clamped caps. Unsupported family controls fail before dispatch. Bounded calls require genuine admitted model input/output facts; missing facts fail with `LimitUnknown`. A reservation is conservative spending authority, not reported usage, a forecast or an invoice. Unknown/partial/delivery-unknown outcomes retain their hold; genuine final reports settle it, including oversized usage. Retry is one explicit layer, off by default, with a bounded window and unknown-prior hold; no hidden resend.


A provider call returns `sp::runtime::Result`: an immutable, owned `std::shared_ptr<const sp::Outcome>`, containing `sp::Completion` or `sp::Failure`. Retain the whole outcome, not only display text. Ordered messages/parts, native continuation, complete wire envelopes, ordered raw observations, stop evidence and genuine attempt metadata survive the call and client destruction. Usage counters are nullable `uint64_t` values with evidence, stage and quality: missing is unknown, never zero. A failure retains its original partial outcome. `ProviderFailure::outcome()` and `ProviderObserverError::outcome()` preserve that result; the latter also preserves the observer exception in `cause()`.

If post-effect accounting or terminal-receipt persistence fails after a real result exists, `ProviderDispatchOutcomePersistenceError` retains the original immutable result in `outcome()` and the original persistence exception in `cause()`. If delivery also failed, `delivery_error()` retains the original observer exception. Successful persistence followed by observer failure rethrows that original observer exception unchanged; an unknown/no-result transport failure does not fabricate an outcome.
### 9.4 `CheckpointStore`

Five mandatory save/load/list/delete operations form the engine's async
checkpoint contract. `AsyncCheckpointStore` requires all five async
overrides; `CheckpointStoreCore` requires the synchronous equivalents.
`adapt_async_checkpoint_store()` supplies synchronous administrative methods
for a native async backend, and `adapt_checkpoint_store()` offloads a
sync-only backend to the bounded blocking pool. In-memory storage executes
in-process mutex-protected operations on the caller; SQLite uses blocking
workers; PostgreSQL provides native async operations.

The pre-v1 `CheckpointStore` vtable remains for existing binary consumers.
Its synchronous defaults now throw `std::logic_error` instead of calling
their async peer; its async defaults offload synchronous overrides. Subclasses
must override every required synchronous operation, or migrate to an explicit
adapter. An async-only legacy subclass must implement `AsyncCheckpointStore`
and call `adapt_async_checkpoint_store()`; a synchronous call on the old
subclass no longer silently drives a coroutine. Missing mandatory operations
fail at compilation for new capability backends and explicitly at runtime for
legacy subclasses; neither can enter mutual recursion.

Pending-write durability is separate: derive `PendingWritesCheckpointStore`
only if `put_writes` is durable on return and can be replayed and cleared.
Without it, the adapter deliberately falls back to full super-step replay.
Do not mistake a no-op pending-write method for an effect-deduplication
guarantee. Python checkpoint subclasses implement the legacy synchronous
methods; wrap external async-native backends on the C++ side. gRPC checkpoints
and protocol hosts continue to use the engine's async methods and the same
store adapters; no wire-format or persisted-schema migration occurs.

### 9.5 `MCPClient`

`rpc_call_async()` is the real implementation; `rpc_call()` is a
thin `run_sync(rpc_call_async(...))` facade. **Not user-extensible**
— `MCPClient` is not designed to be subclassed, you use it as-is.
If you need a custom MCP transport, write a new class; don't
inherit.

HTTP requests overlap normally. stdio writes complete JSON lines under a
short write lock, then a single reader correlates out-of-order replies by
JSON-RPC id. Therefore stdio calls also overlap when the subprocess processes
requests concurrently; a serial subprocess remains the throughput floor.

### 9.6 `Tool` vs `AsyncTool`

Asymmetric by design. Pick one at class declaration time:

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

Do **not** try to inherit from both or override both surfaces of
one class — the `AsyncTool::execute` is `final` precisely to
prevent that.
