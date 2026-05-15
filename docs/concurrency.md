# Concurrency & Async

NeoGraph supports two concurrency models out of the box — pick the
one that fits your hosting pattern:

* **Thread-per-agent (sync)** — `run()` / `run_stream()` / `resume()`
  dispatched onto any executor you already use. Safe up to roughly a
  thousand concurrent agents; ~5 µs engine overhead per call on a
  Release `-O3 -DNDEBUG` build (the super-step loop routes through
  `run_sync(execute_graph_async)` so both entry points share one
  coroutine path).
* **Coroutine-based async** — `run_async()` / `run_stream_async()` /
  `resume_async()` returning `asio::awaitable<RunResult>`. One
  `asio::io_context` hosts thousands of concurrent agents without a
  thread per run; all Provider / MCP / checkpoint I/O points are
  non-blocking `co_await` under the hood. Full migration guide in
  [`ASYNC_GUIDE.md`](ASYNC_GUIDE.md).

## Async (Stage 3)

```cpp
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

asio::io_context io;
for (const auto& user : users) {
    asio::co_spawn(
        io,
        [&, user]() -> asio::awaitable<void> {
            RunConfig cfg;
            cfg.thread_id = user.session_id;
            cfg.input     = {{"messages", user.history}};
            auto result = co_await engine->run_async(cfg);
            handle(result);
        },
        asio::detached);
}
io.run();  // drives all agents on this thread
```

`engine->run_async()` stays on the caller's executor end-to-end —
every super-step suspension point (node dispatch, checkpoint I/O,
parallel fan-out, retry backoff) is a real `co_await`. The three
50 ms steps above therefore overlap on one io_context thread and the
wall time lands at ~50 ms, not 3 × 50 ms. One thread, N concurrent
agents. For CPU-bound fan-out across cores, switch the driver to a
shared `asio::thread_pool` — that's the pattern in
[`benchmarks/concurrent/CONCURRENT.md`](../benchmarks/concurrent/CONCURRENT.md)
where N = 10,000 finishes in 52 ms. Within a single run, the
`make_parallel_group` fan-out overlaps too: three parallel-fanout
researchers collapse from 370 ms sequential to 150 ms.

Custom nodes join the async path by returning an `asio::awaitable`
from the unified `run(NodeInput)` entry point (the one canonical
override since v0.4.0; legacy 8-virtual chain removed in v1.0):

```cpp
class FetchNode : public GraphNode {
  public:
    asio::awaitable<NodeOutput>
    run(NodeInput in) override {
        auto ex = co_await asio::this_coro::executor;
        auto res = co_await neograph::async::async_post(ex, /*...*/);
        // in.ctx.cancel_token, in.state, in.stream_cb available.
        co_return NodeOutput{ {ChannelWrite{"out", res}} };
    }
    std::string get_name() const override { return "fetch"; }
};
```

Async-shaped tools derive from `AsyncTool`:

```cpp
class FetchTool : public neograph::AsyncTool {
  public:
    asio::awaitable<std::string>
    execute_async(const json& args) override { /* co_await HTTP */ }
    // sync execute() is final, routes through run_sync automatically.
};
```

See `examples/27_async_concurrent_runs.cpp` for the multi-agent
pattern and `examples/05_parallel_fanout.cpp` for fan-out within
one run.

## Sync (thread-per-agent)

NeoGraph does not ship its own async runtime — it exposes synchronous
`run()` / `run_stream()` / `resume()` and lets you pick the executor.
A single compiled `GraphEngine` is safe to share across threads that
invoke `run()` concurrently with **distinct `thread_id`s**, so hosting
multi-tenant agent workloads is a matter of dispatching onto whatever
executor you already use.

```cpp
// One engine, many concurrent sessions — no external runtime required.
auto engine = GraphEngine::compile(def, ctx, std::make_shared<InMemoryCheckpointStore>());

std::vector<std::future<RunResult>> sessions;
for (const auto& user : users) {
    sessions.push_back(std::async(std::launch::async, [&engine, user]() {
        RunConfig cfg;
        cfg.thread_id = user.session_id;
        cfg.input = {{"messages", user.history}};
        return engine->run(cfg);
    }));
}
for (auto& f : sessions) handle(f.get());
```

Works the same way with an `asio::thread_pool`, a `std::async`-backed
task system, or your web framework's worker pool — NeoGraph stays out
of the executor decision. If you need CPU-parallel fan-out *inside*
a single sync `run()` call (rather than N sync `run()`s on N threads),
call `engine->set_worker_count(N)` once after `compile()` to install
an engine-owned `asio::thread_pool` that `run_parallel_async` and the
multi-Send branch dispatch onto.

## Using the bundled `RequestQueue`

For multi-tenant servers that want a fixed worker pool with
backpressure (rejecting new sessions when the queue is saturated
instead of unbounded memory growth), link `neograph::util` and use
the built-in lock-free queue — no external executor needed:

```cpp
#include <neograph/util/request_queue.h>
using namespace neograph::util;

RequestQueue pool(16, 1000);           // 16 workers, max 1000 pending sessions
auto engine = GraphEngine::compile(def, ctx,
                                   std::make_shared<InMemoryCheckpointStore>());

std::vector<RunResult>          results(users.size());
std::vector<std::future<void>>  futs;

for (size_t i = 0; i < users.size(); ++i) {
    auto [accepted, fut] = pool.submit([&, i]() {
        RunConfig cfg;
        cfg.thread_id = users[i].session_id;
        cfg.input     = {{"messages", users[i].history}};
        results[i]    = engine->run(cfg);
    });
    if (!accepted) {
        // Backpressure: queue is full — shed load, return 503, retry later, …
        reject(users[i]);
        continue;
    }
    futs.push_back(std::move(fut));
}

for (auto& f : futs) f.get();           // propagates exceptions from run()

auto s = pool.stats();
log("pending={} active={} completed={} rejected={}",
    s.pending, s.active, s.completed, s.rejected);
```

`submit()` returns `{accepted, std::future<void>}`: capture the
`RunResult` via a shared output slot (as above) or a per-task
`std::promise<RunResult>`. The queue is backed by
`moodycamel::ConcurrentQueue` (lock-free) and workers park on a
condvar when idle — no busy-spin.

## Rules for safe concurrent use

- Configuration mutators (`set_retry_policy`, `set_checkpoint_store`,
  `set_store`, `own_tools`, …) must be called **before** any concurrent
  `run()`. Treat the engine as frozen after the first dispatch.
- Concurrent `run()` calls sharing the **same** `thread_id` do not crash
  but produce unspecified checkpoint interleaving. Serialize per-session
  access yourself if you need deterministic history.
- Custom `GraphNode` subclasses must be **stateless or self-synchronized**.
  Node instances are owned by the engine and reused across every run on
  every thread — per-run scratch data belongs in graph channels, not in
  node member variables.
- User-supplied `CheckpointStore`, `Store`, `Provider`, and `Tool`
  implementations must be thread-safe. The bundled `InMemoryCheckpointStore`
  and `InMemoryStore` already are.

## Persistent checkpointing with PostgreSQL

For multi-process deployments or when checkpoints must survive a restart,
link `neograph::postgres` and swap `InMemoryCheckpointStore` for
`PostgresCheckpointStore`:

```cpp
#include <neograph/graph/postgres_checkpoint.h>

auto store = std::make_shared<PostgresCheckpointStore>(
    "postgresql://user:pass@host:5432/dbname");
auto engine = GraphEngine::compile(def, ctx, store);
```

The schema mirrors LangGraph's `PostgresSaver` (three tables prefixed
`neograph_*` to coexist with LangGraph state in the same database) and
deduplicates channel values by `(thread_id, channel, version)`. A
1000-step session that touches one channel per super-step costs roughly
`O(steps + channels)` blob rows instead of `O(steps × channels)`.

**Build flag**: `-DNEOGRAPH_BUILD_POSTGRES=ON` (default). Requires
`libpq-dev` (apt) / `libpq-devel` (rpm). Set the flag `OFF` to skip
the dependency entirely.

**Running the integration tests**: spin up a throwaway local PG and
point the test binary at it:

```bash
docker run -d --rm --name neograph-pg-test \
    -e POSTGRES_PASSWORD=test -e POSTGRES_DB=neograph_test \
    -p 55432:5432 postgres:16-alpine

NEOGRAPH_TEST_POSTGRES_URL='postgresql://postgres:test@localhost:55432/neograph_test' \
    ctest --test-dir build -R PostgresCheckpoint --output-on-failure
```

Without the env var the PG tests are `GTEST_SKIP`'d so the rest of
the suite stays green on machines without a Postgres handy.

Coverage: `tests/test_graph_engine.cpp` contains
`ConcurrentRunDifferentThreadIds` (16 threads × 25 runs = 400 parallel
executions, validates per-session output + checkpoint isolation) and
`ConcurrentRunSameThreadIdNoCrash` (8 threads × 50 runs on one shared
`thread_id`, validates crash-free behavior).
