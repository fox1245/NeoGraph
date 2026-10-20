# NeoGraph on WebAssembly

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

This directory holds the WebAssembly programs that CI builds and runs. `smoke.cpp` compiles a graph with one
`DoubleNode`, writes `doubled = seed * 2`, and runs it with `InMemoryCheckpointStore`; with `seed = 21` it prints
`doubled = 42` and `trace = d`. `checks.cpp` runs the checks that differ on a 32-bit host without threads (below).
Neither performs a network or model call. The graph engine runs on a WebAssembly host today; model calls, Programs
and stores do not yet. The plan is in [#535](https://github.com/fox1245/NeoGraph/issues/535), and this is not a
browser SDK.

## Status

| What | State |
|---|---|
| Toolchain | Emscripten 6.0.13, pinned in CI to the emsdk release commit. |
| Build modes | Single-thread (the default on WebAssembly) and pthread, both built and run in CI. |
| Runtime | Node.js 22 and 24, in CI. Browsers, Cloudflare Workers, Deno and WASI are not verified. |
| SDK | SchemaProvider 0.4.0, interface revision 7, at the commit that `cmake/NeoGraphSchemaProvider.cmake` pins. It builds without libcurl and without threads. |
| Components | Core and, optionally, Program. |

## Build

The build needs Emscripten 6.0.13 (C++20 coroutines, `<stop_token>` and `std::bit_cast` are in its libc++), CMake 3.20
or newer and Ninja. LLM, ASYNC, MCP client, A2A, gRPC and the SQLite and PostgreSQL stores need sockets, TLS or system
libraries that a WebAssembly host does not provide, so CMake refuses them there and they are switched off below.

Single-thread, the default on WebAssembly:

```bash
source /path/to/emsdk/emsdk_env.sh

emcmake cmake -S . -B build-wasm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_WASM=ON \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_ASYNC=OFF -DNEOGRAPH_BUILD_LLM=OFF -DNEOGRAPH_BUILD_MCP=OFF \
  -DNEOGRAPH_BUILD_MCP_CLIENT=OFF -DNEOGRAPH_BUILD_MCP_SERVER=OFF -DNEOGRAPH_BUILD_MCP_HTTP_SERVER=OFF \
  -DNEOGRAPH_BUILD_A2A=OFF -DNEOGRAPH_BUILD_ACP=OFF -DNEOGRAPH_BUILD_GRPC=OFF \
  -DNEOGRAPH_BUILD_POSTGRES=OFF -DNEOGRAPH_BUILD_SQLITE=OFF -DNEOGRAPH_BUILD_UTIL=OFF \
  -DNEOGRAPH_BUILD_EXAMPLES=OFF -DNEOGRAPH_BUILD_TESTS=OFF -DNEOGRAPH_BUILD_BENCHMARKS=OFF \
  -DNEOGRAPH_USE_LIBCURL=OFF
cmake --build build-wasm --target neograph_wasm_smoke neograph_wasm_checks
node build-wasm/wasm/smoke.js
node build-wasm/wasm/checks.js
```

Pthread: add `-DNEOGRAPH_THREADS=ON` to the configure line, and use another build directory.
`NEOGRAPH_BUILD_PROGRAM=ON` is only needed for the native-plugin check; leave it off to build the graph smoke alone.

`NEOGRAPH_THREADS` is the one switch between the two modes. It is `OFF` on Emscripten and WASI and `ON` elsewhere.

- **Single-thread (`OFF`).** No `-pthread` and no `Threads::Threads`. The `asio` target exports `ASIO_DISABLE_THREADS`,
  and the SDK is built the same way (`SP_THREADS=OFF`, no libcurl transport). Work runs on an executor the host
  supplies.
- **pthread (`ON`).** `-pthread` everywhere and a pool of four workers for the smoke programs. The SDK is built with
  `SP_THREADS=ON`; it still has no libcurl transport on WebAssembly.
- **Exceptions.** `-fexceptions` (JavaScript-based exceptions) is set for every NeoGraph target, C sources included,
  and the SDK takes the same flag. A configuration that already names `-fexceptions` or `-fwasm-exceptions` is kept,
  and configuration fails when exceptions are disabled.
- **One setting per program.** Asio is header-only, so two translation units that disagree about threads are a
  one-definition violation that nothing diagnoses. The generated `neograph/build_config.h` records the setting, every
  public header reaches it through `neograph/api.h`, and a translation unit that disagrees stops compiling. The SDK
  records its own setting the same way (`sp/build_config.h`), and configuration fails when the two differ.

## Measured sizes

Release, `-O3 -flto`, Emscripten 6.0.13. The graph smoke contains the Core engine and the SDK runtime.

| Build | `.wasm` | `.wasm` gzip -9 | `.js` |
|---|---:|---:|---:|
| Graph smoke, single-thread | 5,484,652 B (5.2 MiB) | 1,601,090 B (1.5 MiB) | 83,223 B |
| Graph smoke, pthread | 5,432,156 B (5.2 MiB) | 1,655,480 B (1.6 MiB) | 103,691 B |
| Checks with Program, single-thread | 5,017,721 B (4.8 MiB) | 1,554,991 B (1.5 MiB) | 83,031 B |

CI fails the single-thread smoke above 6,000,000 bytes of `.wasm` or 1,750,000 bytes gzipped (the measured size plus
about 9%), and reports the sizes in the job summary.
The 712 KB figure in earlier versions of this page was measured before the typed SDK and no longer applies.

## What is unavailable on these hosts

Calls into these fail with a typed error where NeoGraph owns the call, and are not built otherwise.

- **Model calls.** LLM, ASYNC, MCP client and A2A need sockets and TLS. Model calls through the host's `fetch`
  arrive with M2 of #535.
- **Programs.** The Program component compiles, but `ProgramRuntime` starts threads, and in a single-thread build
  its construction, a node attempt timeout and `set_worker_count(n)` with `n > 1` fail with `thread: Not supported`
  (#557). Program stores on a host are tracked in #556.
- **Native Program controls.** The C v1 plugin ABI is a 64-bit layout, so `NativeControlBinding::create` refuses on
  wasm32 with `NativeControlUnavailable`.
- **Synchronous checkpoint stores.** A store that is not asynchronous runs each operation on a pool of worker threads.
  A single-thread build has none, so the operation is refused with `SynchronousCheckpointStoreUnavailable` instead of
  waiting for a thread; implement `AsyncCheckpointStore`. The built-in in-memory store is asynchronous for checkpoints
  but not for managed-budget operations, so a run with `model_token_budget` is refused too, until the stores are
  awaitable (#556).
- **A managed budget with a deadline.** The deadline is bound to a clock origin that a restart can recover, and a
  WebAssembly host has none. The run is refused with `ManagedBudgetDeadlineClockUnavailable`, before the store is
  asked.
- **The native archive.** There is no file custody backend; the SDK refuses with an `Unsupported` error.
- **Subprocess delegation** (Harness CLIs, stdio MCP servers), **listening sockets**, **gRPC**, **file-path
  uploads** and **Python**.

## Browser

The browser harness is not in the tree: `serve_smoke.py` expects a `smoke.html` that does not exist, and no browser
run has been recorded (#435). A Node.js run is not browser evidence.

## Milestones

The plan after the toolchain review is in [#535](https://github.com/fox1245/NeoGraph/issues/535):

- **M0 and M1 (this page).** The wasm32 fixes, the single-thread build, and the Node smoke with a size gate in CI.
- **M2.** Model calls through the host's `fetch`, in a browser and in Workers.
- **M3.** Programs on a host store ([#557](https://github.com/fox1245/NeoGraph/issues/557),
  [#556](https://github.com/fox1245/NeoGraph/issues/556)).
- **M4.** Realtime sessions, WASI, a custody backend for native archives, and remote Harness.
