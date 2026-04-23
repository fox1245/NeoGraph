# Changelog

All notable changes to NeoGraph are documented in this file.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased] — Stage 4

Stage 4 closes the last `run_sync` hop on the async path. `run_async`
now stays on the caller's executor end-to-end: three 50 ms agents
on one `io_context` thread drop from ~150 ms (serial) to ~50 ms
(overlapping) in `examples/27_async_concurrent_runs`.

### Breaking

- **`GraphNode::execute_full_async` default flipped to async-first.**
  It now wraps `co_await execute_async(state)` into a `NodeResult`
  instead of calling sync `execute_full(state)`. Any subclass that
  emits `Command`/`Send` only from a sync `execute_full` override
  MUST add a one-line `execute_full_async` bridge:
  ```cpp
  asio::awaitable<NodeResult>
  execute_full_async(const GraphState& state) override {
      co_return execute_full(state);
  }
  ```
  Without the bridge, `Command`/`Send` are silently dropped on the
  async path — the 2.0 latent dispatch bug that 3.0 fixed by routing
  through sync at the cost of an `io_context` spawn per super-step.
  All in-tree subclasses (`deep_research_graph`, examples 10/14/21,
  tests 5 sites) now carry the bridge.

### Performance

- Example 27 wall time: **152 ms → 53 ms** (3 agents × 50 ms timer
  step on one `io_context` thread, full overlap).
- No measurable regression on single-run benchmarks; `run()` still
  drives the same coroutine through a fresh single-threaded
  `io_context` via `run_sync`.

### Tests

- 341/341 ctest green
- 295/295 ASan+UBSan green
- Valgrind clean on coroutine-heavy subset (20 tests, 2.4 s)

### Post-release validation (same day)

- **All 30 examples re-run:** 26/29 PASS, 0 FAIL, 3 environment-gated
  (clay_chatbot → raylib, postgres_react_hitl → docker compose,
  deep_research full loop → crawl4ai service). `21_mcp_fanout`
  measured at 3 MCP calls / 8 ms wall — Stage 4 overlap holds under
  real network I/O.

- **ARM64 compatibility (docker buildx --platform linux/arm64):**
  `Dockerfile.arm64-smoke` at repo root. ubuntu:24.04-arm64 +
  core+llm+async+sqlite+tests build under QEMU emulation completes
  in ~15 min; **306/306 ctest green** on ARM64. Stripped binary sizes
  0.81-0.88 MB (nearly identical to x86_64). example 27 runs in
  65 ms under emulation (native x86_64: 53 ms). Confirms Linux/ARM64
  as a supported target alongside macOS beta (Apple Silicon).

- **Cache locality (Ryzen 5800X / Zen 3, Valgrind cachegrind,
  32 KB L1i/d 8-way, 32 MB L3 16-way):**
  `bench_concurrent_neograph` sweep N=1 → 10,000.

  | N | I refs | LLi misses | LLi miss% | Native p50 |
  |---:|---:|---:|---:|---:|
  | 1 | 5.3 M | 4,313 | 0.08% | 17 µs |
  | 100 | 11.8 M | 4,320 | 0.04% | 6 µs |
  | 10,000 | 648 M | 4,329 | 0.00% | 5 µs |

  Last-level instruction misses stay flat at ~4,320 across 4 orders
  of magnitude of N. Unique hot code working set ≈ 277 KB (0.85% of
  L3). 648 M instructions at N=10,000 incur only 4,329 LL misses —
  roughly 1 miss per 150,000 instructions. Native p50 drops from
  17 µs to 5 µs purely from I-cache warming. First measured evidence
  for the "burst concurrency robustness" positioning.

---

## [3.0.0] — 2026-04-22

3.0 removes the Taskflow dependency and unifies sync and async
super-step execution on a single asio coroutine path. Graph-definition
JSON, node ABI, checkpoint schema, and public entry points (`run`,
`run_async`, `run_stream`, `resume`) are source-compatible with 2.0;
the break is confined to `GraphNode` subclasses that emit
`Command`/`Send` from the **sync** `execute_full` override only.

### Breaking

- **`deps/taskflow/` and the Taskflow INTERFACE target are gone.**
  The sync super-step loop, `run_one`, `run_parallel`, `run_sends`,
  and the process-wide `tf::Executor` static are deleted. Downstream
  consumers that `#include <taskflow/...>` via NeoGraph's include
  path must vendor Taskflow separately.
- **`GraphNode::execute_full_async` default now bridges to the sync
  `execute_full` via direct call (no `co_await execute_async`).**
  This preserves `Command`/`Send` emitted from a sync-only override
  — the common 2.0 pattern — through the async path that all entry
  points now share. Async-native nodes that need non-blocking I/O
  AND `Command`/`Send` must override `execute_full_async` directly;
  the docstring has said this since 2.0, but 2.0 never exercised it
  because sync `run()` bypassed the coroutine path entirely.
- **`NodeExecutor::run_one` / `run_parallel` / `run_sends` sync
  methods removed.** Use the `_async` peers.
- **CPU parallel fan-out is opt-in.** Previously Taskflow provided a
  process-wide thread pool by default. In 3.0 `run_parallel_async`
  and the multi-Send branch of `run_sends_async` dispatch branches
  on whichever executor drives the coroutine — the single-threaded
  io_context spun up by sync `run()`, or the caller's own executor
  for `run_async()`. I/O-bound fan-out still overlaps (co_await
  suspension on a single thread); CPU-bound fan-out serializes
  unless the caller uses a multi-threaded executor for `run_async()`
  or opts into an engine-owned pool via `engine->set_worker_count(N)`.

### Added

- `neograph::async::run_sync_pool(awaitable, n_threads)` — N-worker
  sync↔async bridge alongside the existing single-threaded
  `run_sync`. Spins a fresh `asio::thread_pool` for the call so
  inner `make_parallel_group` branches execute on separate workers.
- `GraphEngine::set_worker_count(n)` — opt-in engine-owned
  thread_pool used by `NodeExecutor` for parallel fan-out dispatch.
  Rebuilds the executor; must be called before any concurrent run.

### Changed

- `GraphEngine::execute_graph` (sync) is gone. All entry points
  (`run`, `run_stream`, `resume`) route through
  `execute_graph_async` via `neograph::async::run_sync`, so the
  super-step loop, retry backoff, checkpoint I/O, and parallel
  fan-out now live on one coroutine path end-to-end.
- `benchmarks/concurrent/bench_concurrent_neograph.cpp` switched
  from `tf::Executor` / `tf::Taskflow` to `asio::thread_pool` +
  `asio::post` for the caller-side driver.

### Perf (bench_neograph Release -O3 -DNDEBUG on reference Linux, 10-run median)

- `seq` engine overhead (3-node chain, counter): **~5.0 µs** per call.
- `par` engine overhead (5-worker fan-out + summarizer): **~11.8 µs**
  per call.
- Peak RSS of the whole bench process (warm-up + seq + par iters):
  **4.8 MB**.
- vs LangGraph 1.1.9 on the same workload: **131× faster seq, 199×
  faster par** per iteration; RSS ~12× lighter.

Prior drafts of this CHANGELOG listed "~46 µs seq / ~114 µs par"
as a 3.0 regression. Those numbers came from a build tree where
`CMAKE_BUILD_TYPE` was unset, so the bench binary was compiled
without `-O3 -DNDEBUG`. On a proper Release build the async-peer
collapse is a **win** vs 2.0's Taskflow sync path (which the 2.0
README advertised at 20.65 µs seq / 150.7 µs par on the same
host). The corrected chart is at
[`docs/images/bench-engine-overhead.png`](docs/images/bench-engine-overhead.png).

### Migration

- No action needed if your nodes override `execute()` / `execute_async()`
  and don't emit `Command` / `Send`.
- If you override sync `execute_full` to emit `Command` / `Send`:
  no change required — the 3.0 async-path default now calls your
  sync override directly. `Command.goto_node` routing works via
  sync and async entry points alike.
- If you override `execute_async` (async-native I/O) AND want
  `Command` / `Send`: override `execute_full_async` directly and
  assemble `NodeResult` there. Overriding only `execute_async`
  silently drops `Command` / `Send` because the default
  `execute_full_async` now routes through sync `execute_full`, not
  async `execute_async`.
- If you relied on Taskflow's process-wide pool for CPU parallel
  fan-out via `engine->run()`: call `engine->set_worker_count(N)`
  once after compile(), or drive the engine via `run_async()` on
  your own multi-threaded `asio::thread_pool` / io_context.

---

## [2.0.0] — 2026-04-22

First public release with the Stage 3 async API. This is a breaking
release; the changes below affect compilation (C++ standard) and
ABI (abstract base classes gained async peers). Sync call sites are
preserved bit-for-bit, so **application code that doesn't override
`Provider` / `CheckpointStore` / `GraphNode` / `Tool` continues to
work unchanged**.

### Breaking

- **C++20 required.** The public API exposes `asio::awaitable<T>`
  return types that need `std::coroutine` support. Consumers must
  compile with `-std=c++20` (or higher). GCC 13+, Clang 15+ tested;
  see `docs/ASYNC_GUIDE.md` §4.1 for GCC 13 coroutine workarounds.
- **libpqxx dependency dropped.** `neograph::postgres` now links
  libpq directly. Ubuntu 24.04 users no longer hit the
  `pqxx::argument_error::argument_error(..., std::source_location)`
  link error introduced by libpqxx-7.8t64's C++17/C++20 ABI split.
  CMake find now targets `PostgreSQL::PostgreSQL` (CMake-bundled
  FindPostgreSQL). Consumers who installed only `libpqxx-dev`
  must now also install / retain `libpq-dev`.
- **`Provider`, `CheckpointStore`, `GraphNode`, `MCPClient` ABIs
  extended.** Each grew async peer virtual functions
  (`complete_async`, `save_async`, `execute_async`, `rpc_call_async`
  and their variants). Downstream subclasses recompile against the
  2.0 headers; source is unchanged unless the subclass wants to
  provide a native async override (recommended for any implementor
  that does real I/O).
- **`CheckpointStore::save` / `load_latest` / `load_by_id` / `list`
  / `delete_thread` are no longer pure virtual.** They now have
  default implementations that bridge to the matching `_async`
  peer via `neograph::async::run_sync`. Subclasses that override
  the sync side keep working; subclasses that didn't provide any
  override (which would have been a compile error before) now
  infinitely recurse — contract: override at least one of each
  sync/async pair.

### Added

- **Async API** across all I/O layers
  (`docs/ASYNC_GUIDE.md` for full reference):
  - `Provider::complete_async` on the base class and all built-in
    providers (OpenAI, Schema, RateLimited).
  - `MCPClient::rpc_call_async` for both HTTP and stdio
    transports. stdio uses `asio::posix::stream_descriptor`.
  - `CheckpointStore::*_async` for all eight sync methods.
  - `GraphNode::execute_async` + stream / full / full_stream
    variants, with async-native crossover defaults.
  - `GraphEngine::run_async` / `run_stream_async` / `resume_async`
    driving `execute_graph_async` — an end-to-end coroutine super-
    step loop including parallel fan-out via
    `asio::experimental::make_parallel_group`.
  - `neograph::AsyncTool` adapter for user tools that want a
    coroutine body while preserving the sync `Tool` interface.
- **`neograph::async` namespace** — HTTP client, connection pool,
  SSE parser, run_sync bridge, URL endpoint splitter. See
  `include/neograph/async/*.h`.
- **New examples**:
  - `examples/27_async_concurrent_runs.cpp` — multiple agents on
    one `io_context`.
  - `examples/05_parallel_fanout.cpp` (rewritten) — async fan-out
    within a single graph run using `run_parallel_async`.
- **CI bench regression gate** (`.github/workflows/ci.yml`) —
  PR checks enforce floors on `bench_async_http` / `bench_async_fanout`
  / `bench_neograph`.

### Performance

Measured on the feat/async-api branch against Stage 2 sync baselines:

- `bench_async_http --mode async_pool --concur 1000`:
  6064 ops/s → **17834 ops/s** (2.9×).
- `bench_async_fanout --concur 50000`:
  thread-per-agent unachievable → **541K ops/s / 67 MB RSS**.
- `examples/27_async_concurrent_runs` (3 × 50ms async work):
  150ms (sync) → **50ms** (1 io_context thread).
- `examples/05_parallel_fanout` (3 × 100-150ms async work):
  370ms (sequential) → **150ms** (1 io_context thread).
- `bench_neograph` engine overhead: unchanged (~30 µs seq /
  ~205 µs par). Coroutine machinery does not regress the hot path.

### Not yet in 2.0.0

- **Taskflow dependency** remains. The sync `engine.run()` path
  still uses it for fan-out; Sem 4.5 revisits whether sync paths
  can be replaced by `run_sync(*_async)` so the dependency can
  drop entirely.

### Cross-platform

Three platforms are supported in 2.0.0 at different stability tiers.
The tier reflects how much real-world validation the platform has
seen before release — not feature coverage (the codebase is single-
sourced with `#ifdef _WIN32` splits; features are equivalent across
platforms once tests pass).

#### Linux — **GA** (production-ready)

* Ubuntu 24.04, GCC 13.
* Full 332/332 ctest green locally (Postgres via docker
  `postgres:16-alpine`) plus all benches inside committed CI floors.
* MCP stdio on fork/pipe/execvp + `asio::posix::stream_descriptor`.
* Postgres async peers on libpq nonblocking + `asio::posix::stream_
  descriptor` wrapping `PQsocket`.
* Reference platform for every performance number quoted above.

#### macOS — **beta**

* macos-latest (Apple Silicon), Clang via Xcode.
* CI builds + runs non-Postgres tests; Postgres integration cases
  self-skip without a service container. POSIX paths (same fork/
  pipe + asio::posix code) are exercised.
* `CoreFoundation` + `Security` frameworks linked through httplib
  for system cert loading on TLS.
* Treat as beta until 2-4 weeks of CI runs and user reports
  confirm no runtime-behaviour differences (coroutine scheduling,
  SIGPIPE / EPIPE shape, pipe buffer sizing). Targeted promotion
  to GA once those roll in without incident.

#### Windows — **alpha**

* windows-latest, MSVC 19.44 (VS 2022), x64.
* CI scope: **core + async + MCP + LLM only**. Postgres and
  SQLite backends are disabled on the Windows CI job because
  vcpkg would compile OpenSSL / libpq / zlib / lz4 from source
  on every run (~20 min, no working binary cache backend upstream
  since `x-gha` was removed). Windows users compile these
  locally via their own vcpkg / choco setup.
* OpenSSL via the runner's preinstalled choco package
  (`C:/Program Files/OpenSSL-Win64/`). TLS paths in httplib +
  asio::ssl compile and link.
* MCP stdio: `CreateProcess` + named-pipe (FILE_FLAG_OVERLAPPED) +
  `asio::windows::stream_handle`. The overlapped-pipe path was
  written against MSDN spec without local Windows validation;
  expect first-users to surface edge cases (ERROR_IO_PENDING
  handling, pipe buffer boundary on large JSON responses).
* Postgres async peers (when enabled locally): `asio::ip::tcp::
  socket::assign` wrapping the SOCKET returned by `PQsocket`
  (cast through `native_handle_type` to preserve 64-bit SOCKET
  values). Not exercised by Windows CI — local only.
* Coroutine machinery lives in MSVC's `<coroutine>`; behaviour
  expected to match GCC/Clang by spec but `examples/27` cross-run
  overlap measurements haven't been confirmed on Windows yet.
* Treat as **alpha** through 2.0.0. Promote to beta once one
  production user runs a multi-agent workload for a week without
  hitting stdio/pipe or coroutine-scheduler issues, AND Postgres
  async peers get locally validated by a user willing to run
  vcpkg's full libpq build.

> **Pattern**: CI green is a floor, not a ceiling. Layer 3 runtime
> behaviour differences (coroutine scheduling timing, pipe buffer
> boundaries, socket takeover semantics) only surface under real
> workloads. The tier language above gives users the right
> expectation for each platform rather than pretending all three
> are interchangeable on day one.

### Fixed post-bump

- **`async::HttpResponse` headers map** — the response surface now
  exposes a `headers` vector of `(name, value)` pairs preserving wire
  order and original casing, plus `get_header(name)` as a
  case-insensitive accessor. Retry-After and Location remain as
  dedicated fields for backward compatibility. Unblocks the MCP
  session tracking fix below.
- **MCP `Mcp-Session-Id` header tracking** — the Sem 2.6
  httplib→async_post migration silently dropped this. Every post-
  initialize RPC now echoes the server-assigned session id back
  via the new headers accessor, so the server's session state
  stays routable.
- **MCP stdio awaitable mutex** — `StdioSession::rpc_call_async`
  used `std::mutex`, which deadlocked when two coroutines on the
  same single-threaded io_context called the same session (the
  second's `lock_guard` blocked the worker the first needed).
  Replaced with an `asio::experimental::channel<void(error_code)>`
  capacity-1 semaphore so the second acquirer suspends
  cooperatively.
- **`PostgresCheckpointStore` async peers** — all eight
  CheckpointStore async methods (`save_async`, `load_latest_async`,
  `load_by_id_async`, `list_async`, `delete_thread_async`,
  `put_writes_async`, `get_writes_async`, `clear_writes_async`)
  are now true-async. Internals: `PQsetnonblocking(1)` +
  `PQsendQueryParams` + `asio::posix::stream_descriptor` on
  `PQsocket()` + `co_await sock.async_wait(wait_read/wait_write)`.
  Four concurrent `save_async` calls on a pool of 4 slots now
  commit-fsync in parallel at the wire level rather than
  serialising through `run_sync`.

---

## [0.1.0] — pre-2026-04

Pre-release development. No public API stability guarantees.
