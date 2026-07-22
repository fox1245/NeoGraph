# NeoGraph Async Guide

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

| Layer | Sync (unchanged) | Async peer |
|---|---|---|
| Provider | `complete` / `complete_stream` | `complete_async` / `complete_stream_async` |
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

Sync surfaces are preserved. Existing code that calls `engine->run(cfg)`
or any `provider->complete*` entry point remains supported. The 276+
test cases that existed before Stage 3 still pass against the sync
path.

---

## 2. The crossover-default pattern

Every remaining sync/async pair on Provider and persistence abstractions is connected by a
pair of default implementations that bridge each direction:

```cpp
class Provider {
  public:
    // Sync default: drive the async peer on a private io_context.
    virtual ChatCompletion complete(const CompletionParams& params);

    // Async default: co_return the sync peer (single-threaded on
    // the resuming coroutine).
    virtual asio::awaitable<ChatCompletion>
    complete_async(const CompletionParams& params);

    // ...
};
```

**Contract: override at least one of the two.** If you override
neither, calling either method infinitely recurses between the two
defaults until the stack overflows. Documented; no runtime guard
(would slow every call on every implementor).

### Which side to override

| Your code shape | Override |
|---|---|
| Issues real non-blocking I/O (HTTP, MCP, DB, timer) | **async peer** — inherit the sync facade |
| Pure CPU work, or blocks briefly on a sync library | **sync peer** — inherit the async bridge |
| Custom `GraphNode` | override `run(NodeInput)`; return writes, `Command`, and `Send` in one `NodeOutput` |

### Why not a single unified API?

Collapsing every public abstraction into async would
force every existing Tool and every
CheckpointStore subclass to acknowledge the async machinery —
including cases where it buys nothing (a tool that adds two
numbers). The crossover pair remains the zero-migration-cost path for
those abstractions. `GraphNode` was intentionally collapsed to one
coroutine override in v1.0.

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

Derive from `CompletionProvider` and implement only `do_invoke()`.
Its final adapters keep every existing `Provider` entry point working,
while `CompletionRequest` makes collect versus stream mode explicit.

```cpp
class MyProvider : public CompletionProvider {
  public:
    asio::awaitable<ChatCompletion>
    do_invoke(CompletionRequest request) override {
        auto ex = co_await asio::this_coro::executor;
        const auto& params = request.params();
        auto res = co_await neograph::async::async_post(
            ex, host, port, path, body, headers, /*tls=*/true);
        if (request.streaming() && request.on_chunk()) {
            // Deliver parsed chunks through request.on_chunk().
        }
        co_return parse_response(res);
    }

    std::string get_name() const override { return "my-provider"; }
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
class MyNode : public GraphNode {
  public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        CompletionParams params = build_params(in.state);
        params.cancel_token = in.ctx.cancel_token;
        auto completion = co_await provider_->complete_async(params);

        neograph::json msg;
        to_json(msg, completion.message);
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"messages", json::array({msg})});
        co_return out;
    }

    std::string get_name() const override { return name_; }
  private:
    std::shared_ptr<Provider> provider_;
    std::string name_;
};
```

The engine drives this same coroutine from sync and async entry points. With
`engine->run_async()`, the node participates in io_context overlap without an
OS thread per run.

---

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
`execute_full_stream_async` double-execution fallback no longer exists.

### 4.5 MCP stdio single-session concurrency

`StdioSession::rpc_call_async` serialises concurrent calls via
`std::mutex`. Two coroutines calling the **same** session on the
**same single-threaded** `io_context` will deadlock — the second
coroutine's `lock_guard` blocks the worker the first needs to
drive its I/O completions. Typical usage (one logical caller per
session, async fan-out across *different* sessions) is unaffected.
An awaitable-mutex version is tracked as future work.

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

v1.0 removed the eight `execute*` virtuals. A custom node now has one
override for sync and async engine entry points, streaming, and control flow:

```cpp
asio::awaitable<NodeOutput> run(NodeInput in) override {
    NodeOutput out;
    out.writes.push_back({"answer", co_await fetch_answer(in)});
    out.command = Command{.goto_node = "review"};
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

`GraphNode` has one canonical override. Provider and persistence
interfaces retain separate sync/async peers for compatibility.

### 9.1 Two-minute version

| You write a… | Override | Inherit as-is |
|---|---|---|
| Any custom `GraphNode` | `run(NodeInput)` | `get_name()` is the only other required virtual |
| New custom LLM backend | inherit `CompletionProvider`, override `do_invoke()` | all existing `Provider` entry points are final adapters |
| Custom `CheckpointStore`, async-capable backend | all eight `*_async` peers | sync peers bridge via `run_sync` |
| Custom `CheckpointStore`, sync-only backend | all eight sync peers | async peers bridge via `run_sync` |
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

Existing `Provider` subclasses may keep using the four sync/async collect/stream
virtuals. They are stable compatibility APIs with no removal planned and no
deprecation warnings. Each pair still requires at least one override:

| Override | Behaviour |
|---|---|
| `complete()` only | Sync works directly; async `complete_async` bridges via the base-class default `co_return complete()`. Fine for CPU-only mock providers. |
| `complete_async()` only | Async works directly; sync `complete` bridges via `run_sync(complete_async())`. |
| `complete_stream()` only | Sync streaming works directly; the async peer runs it on a worker thread and delivers callbacks on the awaiting executor. |
| `complete_stream_async()` only | Native async streaming works directly; implement a sync peer too if direct sync streaming calls must avoid the default collect fallback. |

For a **new** backend, do not choose among these pairs. Derive from
`CompletionProvider`, implement `do_invoke(CompletionRequest)`, and use
`request.streaming()` to select the transport. New direct callers should use
`invoke_request()` with `CompletionRequest::collect(...)` or
`CompletionRequest::stream(...)`. Compatibility and security fixes continue on
the old entry points, but new capabilities may be explicit-request-only.

### 9.4 `CheckpointStore`

Eight sync methods, eight async peers, matched 1:1. The shipping
stores (`InMemoryCheckpointStore`, `SqliteCheckpointStore`,
`PostgresCheckpointStore`) all implement the async side and let
sync bridge through the base-class default.

- **Async-capable backend** (libpq non-blocking, async MongoDB
  driver, etc.): override all eight `*_async` peers. The sync-call
  path pays one `run_sync` per invocation — fine for `get_state` /
  `update_state` admin calls, not on a hot loop (but the engine
  never calls sync checkpoint methods; only user tooling does).
- **Blocking-only backend** (old file I/O, some ODBC wrappers):
  override the eight sync methods. Async callers block the
  coroutine thread through `run_sync` on each call, which is
  usually acceptable because checkpoint writes are infrequent
  relative to node dispatch.
- **Don't mix**: if you override `save()` but leave `save_async()` at
  the default, the async peer bridges BACK to sync through the
  base-class default — correct, but loses the async I/O benefit. Go
  all-sync or all-async per interface.

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
