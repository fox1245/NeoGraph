# Changelog

All notable changes to NeoGraph are documented in this file.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [2.0.0] — 2026-04-21 (unreleased, `feat/async-api` branch)

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
