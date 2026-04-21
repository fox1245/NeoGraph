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
| Provider | `complete` / `complete_stream` | `complete_async` |
| CheckpointStore | `save` / `load_latest` / `load_by_id` / `list` / `delete_thread` / `put_writes` / `get_writes` / `clear_writes` | `*_async` for each |
| GraphNode | `execute` / `execute_stream` / `execute_full` / `execute_full_stream` | `*_async` for each |
| GraphEngine | `run` / `run_stream` / `resume` | `run_async` / `run_stream_async` / `resume_async` |
| MCPClient | `rpc_call` | `rpc_call_async` |
| Tool | `execute` (user interface — frozen) | wrap with `AsyncTool` adapter |

The async peers return `asio::awaitable<T>`. Drive them on any
`asio::io_context` (or strand, or thread pool with `any_io_executor`).
One `io_context` can host thousands of concurrent `run_async`
invocations without dedicating an OS thread per run — the concurrency
model that motivated the whole refactor.

Sync surfaces are preserved. Existing code that calls `engine->run(cfg)`
or `provider->complete(params)` keeps working bit-for-bit. The 276+
test cases that existed before Stage 3 still pass against the sync
path.

---

## 2. The crossover-default pattern

Every sync/async pair on the abstract base classes is connected by a
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
| Must emit `Command` or `Send` from a coroutine body (graph nodes) | override `execute_full_async` directly |
| Streaming-aware LLM node that produces writes through a token stream | both `execute_async` (for non-stream paths) and either `execute_stream_async` or `execute_full_stream_async` |

### Why not a single unified API?

Collapsing sync into async (making every call a coroutine) would
force every existing Tool, every user-authored GraphNode, every
CheckpointStore subclass to acknowledge the async machinery —
including cases where it buys nothing (a tool that adds two
numbers). The crossover pair is the zero-migration-cost path:
legacy implementors change nothing.

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

Implement only `complete_async`. The sync `complete` comes free
through the base-class bridge (fresh `io_context` per call), so
users that call your provider through the sync API still work.

```cpp
class MyProvider : public Provider {
  public:
    asio::awaitable<ChatCompletion>
    complete_async(const CompletionParams& params) override {
        auto ex = co_await asio::this_coro::executor;
        auto res = co_await neograph::async::async_post(
            ex, host, port, path, body, headers, /*tls=*/true);
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
    asio::awaitable<std::vector<ChannelWrite>>
    execute_async(const GraphState& state) override {
        CompletionParams params = build_params(state);
        auto completion = co_await provider_->complete_async(params);

        neograph::json msg;
        to_json(msg, completion.message);
        co_return std::vector<ChannelWrite>{
            ChannelWrite{"messages", json::array({msg})}};
    }

    std::string get_name() const override { return name_; }
  private:
    std::shared_ptr<Provider> provider_;
    std::string name_;
};
```

The node's sync `execute()` comes from the `GraphNode` crossover
default and routes through `run_sync`. For graph users who drive
everything through `engine->run_async()`, this node participates in
the io_context overlap without an OS thread per run.

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

### 4.4 `execute_full_stream_async` default re-runs the node

The default `execute_full_stream_async` in `GraphNode` runs
`execute_full_async` first, then if no `Command` or `Send` was
emitted, replaces the writes with a second call through
`execute_stream_async`. This is the correct behaviour for LLM nodes
where the streaming path is what produces the writes (token
assembly), but for nodes whose `execute_async` already did all the
work it's a redundant second run — visible as 2× wall-clock cost
in traces.

Nodes that don't stream should override `execute_full_stream_async`
directly to short-circuit:

```cpp
asio::awaitable<NodeResult>
execute_full_stream_async(const GraphState& state,
                          const GraphStreamCallback&) override {
    auto writes = co_await execute_async(state);
    co_return NodeResult{std::move(writes)};
}
```

See `examples/05_parallel_fanout.cpp` for the pattern in a
real node.

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
- [ ] If your custom nodes do real I/O → override `execute_async`.
- [ ] If your tools do real I/O → derive from `AsyncTool`, override
      `execute_async`.
- [ ] If you use the Postgres checkpoint store → still sync under
      the hood (Sem 3.3 pending); expect a future breaking change
      when libpq pipeline mode lands.
- [ ] Measure. The value axis is concurrency; if your workload
      isn't concurrency-bound, don't migrate.

---

## 7. What's not covered yet

* **Postgres checkpoint store** — still uses libpqxx sync; its
  `save_async` routes through `run_sync`. A full libpq-pipeline
  rewrite is tracked as Sem 3.3.
* **`async::HttpResponse` headers map** — the response surface only
  exposes status / body / retry_after / location. Arbitrary header
  access (e.g. MCP session ID header tracking) is a Sem 1
  follow-up.
* **2.0 version bump** — the `feat/async-api` branch hasn't been
  merged yet; the version in `CMakeLists.txt` still reads 1.x.
  A proper breaking-change ledger + release notes accompany the
  bump when Sem 3.3 closes.
