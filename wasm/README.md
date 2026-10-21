# NeoGraph on WebAssembly

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

This directory holds the WebAssembly programs that CI builds and runs.

- `module.cpp` and `neograph.mjs` are the JavaScript module: compile a graph from JSON and run it, with the model calls
  sent through the host's `fetch`. `examples/` has a Node program and a Cloudflare Workers-style handler that use it,
  and `test/` has the offline tests that CI runs (a local HTTP server stands in for the model).
- `smoke.cpp` compiles a graph with one `DoubleNode`, writes `doubled = seed * 2`, and runs it with
  `InMemoryCheckpointStore`; with `seed = 21` it prints `doubled = 42` and `trace = d`.
- `checks.cpp` runs the checks that differ on a 32-bit host without threads (below).

The plan is in [#535](https://github.com/fox1245/NeoGraph/issues/535), and this is not a browser SDK.

## Status

| What | State |
|---|---|
| Toolchain | Emscripten 6.0.13, pinned in CI to the emsdk release commit. |
| Build modes | Single-thread (the default on WebAssembly) and pthread, both built and run in CI. The JavaScript module is single-thread. |
| Runtime | Node.js 22 and 24, in CI. The Workers example runs in Node, called with a `Request`; it has not run on workerd. Browsers, Deno and WASI are not verified. |
| SDK | SchemaProvider 0.4.0, interface revision 7, at the commit that `cmake/NeoGraphSchemaProvider.cmake` pins. It builds without libcurl and without threads, and its JavaScript host (`sp_js_host`) is the executor and the `fetch` transport of the module. |
| Components | Core, LLM (model calls through the host), and optionally Program (the runtime on the host's loop, with in-memory stores). |

## Build

The build needs Emscripten 6.0.13 (C++20 coroutines, `<stop_token>` and `std::bit_cast` are in its libc++), CMake 3.20
or newer and Ninja. ASYNC, MCP client, A2A, gRPC and the SQLite and PostgreSQL stores need sockets, TLS or system
libraries that a WebAssembly host does not provide, so CMake refuses them there and they are switched off below. LLM is
built for the host (below).

Single-thread, the default on WebAssembly:

```bash
source /path/to/emsdk/emsdk_env.sh

emcmake cmake -S . -B build-wasm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_WASM=ON -DNEOGRAPH_BUILD_WASM_JS=ON \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_ASYNC=OFF -DNEOGRAPH_BUILD_LLM=ON -DNEOGRAPH_BUILD_MCP=OFF \
  -DNEOGRAPH_BUILD_MCP_CLIENT=OFF -DNEOGRAPH_BUILD_MCP_SERVER=OFF -DNEOGRAPH_BUILD_MCP_HTTP_SERVER=OFF \
  -DNEOGRAPH_BUILD_A2A=OFF -DNEOGRAPH_BUILD_ACP=OFF -DNEOGRAPH_BUILD_GRPC=OFF \
  -DNEOGRAPH_BUILD_POSTGRES=OFF -DNEOGRAPH_BUILD_SQLITE=OFF -DNEOGRAPH_BUILD_UTIL=OFF \
  -DNEOGRAPH_BUILD_EXAMPLES=OFF -DNEOGRAPH_BUILD_TESTS=OFF -DNEOGRAPH_BUILD_BENCHMARKS=OFF \
  -DNEOGRAPH_USE_LIBCURL=OFF
cmake --build build-wasm --target neograph_wasm_smoke neograph_wasm_checks neograph_js
node build-wasm/wasm/smoke.js
node build-wasm/wasm/checks.js
node wasm/test/llm_host_test.mjs build-wasm/wasm
node wasm/test/workers_test.mjs build-wasm/wasm
```

`NEOGRAPH_BUILD_WASM_JS=ON` builds the JavaScript module into `build-wasm/wasm/`: `neograph_module.wasm`,
`neograph_module.mjs` (the Emscripten loader) and `neograph.mjs` (the API, copied from this directory). Copy those three
files into a project to use it. `NEOGRAPH_BUILD_PROGRAM=ON` is only needed for the native-plugin check; leave it off to
build the graph smoke and the module alone. Pthread: add `-DNEOGRAPH_THREADS=ON` to the configure line, leave
`NEOGRAPH_BUILD_WASM_JS` and `NEOGRAPH_BUILD_LLM` off, and use another build directory.

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

## The JavaScript module

```js
import { load } from './neograph.mjs';

const neograph = await load();                      // a Worker passes its imported module: load({ wasmModule })
const result = await neograph.run({
  graph: {                                          // a graph definition, as JSON text or an object
    channels: { messages: { reducer: 'append' } },
    nodes: { llm: { type: 'llm_call' } },
    edges: [{ from: '__start__', to: 'llm' }, { from: 'llm', to: '__end__' }],
  },
  provider: {                                       // given at run time; nothing of it is in the module
    baseUrl: 'https://api.example.com',             // the origin of an OpenAI-compatible Chat Completions endpoint
    path: '/v1/chat/completions',
    apiKey: env.API_KEY,
    model: 'a-model',
  },
  input: { messages: [{ role: 'user', content: 'Hello' }] },
  onEvent: (event) => console.log(event),           // optional: streams the run
});
console.log(result.text);
```

- **`load(options)`** instantiates the module once. In Node and a browser it finds `neograph_module.wasm` beside
  `neograph_module.mjs`. A Worker cannot compile bytes at run time, so it passes the `WebAssembly.Module` that its
  bundler imports as `wasmModule`. `wasmBinary` and `locateFile` are the Emscripten options.
- **`neograph.compile(graph, provider)`** returns a `Graph` that runs any number of times (`graph.run(input, run)`) until
  `graph.release()`. **`neograph.run({ graph, provider, input, ...run })`** compiles, runs once and releases.
- **`provider`** is `{ baseUrl, path, apiKey, model }` for a Chat Completions endpoint. `family` names another wire
  family and `paths: { buffered, streaming }` another route, and `descriptor` takes a complete SchemaProvider
  descriptor. `instructions`, `maxOutputTokens` and `temperature` are optional. The key is sent as the descriptor's
  family sends a key.
- **`run`** options: `onEvent` (streams the run: the model is called in streaming mode), `signal` (an `AbortSignal`
  that cancels the run and the model call) and `timeoutMs`.
- **The result** is `{ status, text, output, trace, usage }`: `status` is `completed`, `interrupted`, `step_limit` and so
  on; `text` is the last assistant message; `output` is the final state (`output.channels.<name>.value`); `trace` lists
  the nodes that ran; `usage` has `input_tokens`, `output_tokens` and `total_tokens` when the provider reports them.
- **Events** are `{ type, node, data }` with `type` one of `node_start`, `node_end`, `llm_token` (`data` is the text
  of the delta), `channel_write`, `interrupt` and `error`, in the order the engine produced them.
- **Errors** are a `NeoGraphError`: `type` is `provider` (the model call failed: `kind`, `reason` and `httpStatus` say
  how), `cancelled` or `error` (an invalid graph or provider, or a failing node). A graph or provider that is invalid
  throws from `compile`.

Graphs use the built-in node types that need no tool: `llm_call`, `intent_classifier` and `subgraph`. Tools and nodes
written in JavaScript, checkpoint stores and Programs are not part of this milestone.

## How it works

### The two event loops

NeoGraph's graph engine is a set of coroutines on an Asio `io_context`. A WebAssembly host owns the thread and its
event loop, and nothing in the module may block it: no Asyncify, no JSPI, no loop of the module's own. SchemaProvider
has the same problem and solves it with a host seam: an `Executor` that the host runs and a `HostTransport` that sends
HTTP, which `sp_js_host` implements over `queueMicrotask`, `setTimeout` and `fetch`. The module joins the engine to that
loop in four steps.

- **Who pumps the `io_context`.** `async::HostLoop` owns it and never calls `run()`. A turn is a task posted to a
  SchemaProvider executor of the loop's own (a Client shuts its executor down when it ends, so they cannot share one);
  it calls `poll_one()` until no handler is ready, at most 256 a turn, and then yields to the host and posts another turn
  if work is left. A turn is scheduled when work is executed on the loop's executor: `HostLoop::executor()` wraps the
  `io_context` executor so that executing a handler on it posts a turn, one at a time and never from inside a turn.
  Starting a run, cancelling it, and an SDK completion all do that. An idle loop schedules nothing.
- **How an SDK completion reaches the graph's coroutines.** `llm_call` starts the SDK operation and suspends on a
  wake channel. The SDK delivers a fetch chunk or the outcome in a task of its own executor, a JavaScript microtask. The
  callback keeps the event and signals the channel, which posts the coroutine's continuation to the loop's executor. That
  schedules a turn, and in it the coroutine resumes, hands the events to the run's observers in order and, on the
  outcome, finishes the node. Nothing runs inline in an SDK callback, and no thread or lock is involved.
- **How `run_async` is driven without blocking.** `HostLoop::spawn(awaitable, done)` is `co_spawn` plus a turn: it
  returns at once, and `done(exception, result)` is called from a later turn. The module's `ng_run` returns, the host
  event loop goes on, and the JavaScript `Promise` is resolved from `ng_js_done` when the run ends.
- **Asio timers.** A retry backoff or a resource-wait poll is an Asio timer, and the host's loop cannot see Asio's timer
  queue. The engine waits on `async::HostTimer` (`src/core/host_timer.h`), which arms the Asio timer and tells the loop
  its deadline (`async::note_deadline`) in one call, and the loop sets one host timer for the earliest. That timer can
  run before Asio dispatches the timer that expires then (Asio's Windows reactor learns from a thread of its own that a
  timer is due, and a host's clock can run ahead of Asio's), so the loop keeps a deadline until Asio has dispatched the
  timers up to it and looks again a millisecond later while it has not. A raw Asio timer in `src/` or `include/` would
  never fire on a host, so the `host_timer_guard` test refuses it; `scripts/host_timer_allowlist.txt` lists the code
  that never runs on a host loop. Code of your own that waits on an Asio timer under a `HostLoop` calls
  `async::note_deadline` itself.

The synchronous entry points keep their symbols and refuse. `GraphEngine::run`, `resume`, `Provider::dispatch` and
`invoke`, and the synchronous endpoint calls drive a private `io_context` until the call is done, which would wait for
a host loop that only the waiting thread can run. When a provider on a host-pumped executor (`Provider::host_pumped()`,
true for `SchemaProvider` built with an executor that says so) is reached from such a call, the call fails with the SDK's
`runtime.blocking.host_pumped` reason (`ErrorKind::Misuse`, retry safety `NotSent`) before anything is sent. The message
names the `_async` form to use. A run that never reaches a host-pumped provider still works through `run()`, as in the
smoke program.

### TLS and OpenSSL

TLS belongs to the host: `fetch` does it. Where the SDK has no libcurl transport (WebAssembly, or a build with
`NEOGRAPH_THREADS=OFF`), `neograph::llm` is built host-only: CMake does not look for OpenSSL, `neograph::llm` links
neither `neograph::async` nor `SchemaProvider::transport`, and `NEOGRAPH_BUILD_ASYNC` (the `asio::ssl` HTTP client
that LLM, MCP client and A2A use on native hosts) stays refused. The libcurl constructor of `SchemaProvider` then
throws the SDK's `ConfigError` at `/transport`. Native builds are unchanged: the same libraries, the same link lines, the
same constructors.

### Building a provider over the host seam

`SchemaProvider` has a second constructor, `SchemaProvider(descriptor, options, executor, transport, defaults)`, and a
matching `create`. It forwards the SDK's public `Client(descriptor, options, executor, transport)`. It needs no libcurl
and no OpenSSL, and takes the `host_pumped()` declaration of the executor. The module makes one executor and one fetch
transport for each compiled graph:

```cpp
auto executor  = sp::runtime::js::make_executor();
auto transport = sp::runtime::js::make_fetch_transport(executor);
auto provider  = neograph::llm::SchemaProvider::create(descriptor, sp::runtime::Options::with_api_key(key),
                                                       executor, transport);
```

### The JavaScript surface

The module exports a C API and `neograph.mjs` wraps it. A C API with JSON strings needs no binding library; Embind
would add one and a registration of every type, for a surface of five functions: `ng_compile`, `ng_run`, `ng_cancel`,
`ng_release` and `ng_error`. Results come back through two callbacks, `Module.ngOnEvent` and `Module.ngOnDone`, called
from a later turn of the host's event loop. The key is part of the provider description that `compile` is given and is
held by the SDK client of that graph; it is not in the binary, not in any result, and not in an error message.

## Programs on the host loop

`program::ProgramRuntime` takes the loop's executor, and runs everything on it: a Program's attempts, its deadline timers
and the waiters of `ProgramHandle::wait_async`. It starts no thread. This is the whole of the Program support on these
hosts at this point: the stores are the in-memory ones.

```cpp
async::HostLoop loop(host_executor);                  // the sp::runtime::Executor the host runs
program::RuntimeConfig config{catalog, checkpoints, /*state_store=*/{}, transitions, /*scheduler_threads=*/1};
config.executor = loop.executor();                    // required here: a build without threads has no pool to fall back to
program::ProgramRuntime runtime(std::move(config));

loop.spawn(runtime.run_async(owner, version, invocation),
           [](std::exception_ptr error, program::ProgramResult result) { /* from a later turn */ });
```

- **Required.** Without `RuntimeConfig::executor`, a build without threads throws `ProgramExecutorRequired`, a
  `graph::ConfigurationFailure`, from the constructor. On a native build the runtime's own pools are used when it is
  unset, as they always were.
- **Awaitable forms.** Every operation has one: `start_async`, `reconnect_async`, `resume_handle_async`, `fork_async`,
  `replace_async`, `migrate_graph_async` and the rest. They run on the loop and complete on the awaiting coroutine's
  executor. `run_async` and `resume_async` wait for the result.
- **The synchronous calls refuse.** `start`, `resume`, `fork`, `run` and the other synchronous calls, and
  `ProgramHandle::wait` while the result is not in, would block the thread that has to run the Program. They throw
  `program::ProgramBlockingCallRefused` (a `graph::ConfigurationFailure`) before they do anything. `operation()` names the
  call and `alternative()` the awaitable form. A `hook_runtime` is refused by the constructor for the same reason.
- **Timers.** The runtime waits on `async::HostTimer` only, which tells the loop each deadline and takes it back when the
  timer is cancelled, re-armed or destroyed, so a finished run leaves no host timer armed.
- **Node bodies.** With an executor, a node attempt that needs its own worker runs on a strand of it instead of the
  32-thread process-wide pool, so a node body must not block its thread.
- **Locks.** A critical section that reaches a store holds an `OperationLock`, which a coroutine waits for without
  blocking; `std::mutex` guards memory only.
- **Destroying the runtime** cancels its runs and does not wait for them. The attempts keep the runtime's state alive until
  they finish, so keep running the loop until the handles are terminal.

## Examples

- `examples/node/chat.mjs`: one call from Node,
  `API_KEY=... node wasm/examples/node/chat.mjs <base url> <path> <model> "<prompt>" [--stream]`.
- `examples/workers/worker.mjs` and `wrangler.toml`: a request handler that answers `POST {"message": "..."}` with the
  model's reply, as JSON or as server-sent events. The template holds no secret: the key is a Worker secret
  (`wrangler secret put API_KEY`), and `neograph_module.wasm` is imported as a `WebAssembly.Module`.

## Tests

CI builds the module and runs, under Node 22 and 24, with no network and no key:

- `test/llm_host_test.mjs`: a local HTTP server answers like a Chat Completions endpoint, buffered and as server-sent
  events with a character cut across two chunks. The test runs the one-node graph through the module and checks the
  final state, the order of the streamed events, the request that went out (the key and model given at run time), a
  provider refusal, cancellation by `AbortSignal`, a timeout, concurrent runs, and that the host's timers keep running
  while a call is in flight.
- `test/workers_test.mjs`: the Workers example's exported `fetch` handler is called with a `Request` against the same
  server, buffered and streamed. It shows the handler's logic and its use of the platform APIs; it is not a run on
  workerd.
- `checks.cpp` (below) runs a synchronous run against a host-pumped provider and checks that it is refused and that
  nothing was sent.
- `checks.cpp` also builds a `ProgramRuntime` on a `HostLoop`: a runtime without an executor and a synchronous `start` are
  refused, and a one-node Program with in-memory stores runs to its result in the host's turns.
- `tests/test_program_runtime_executor.cpp` runs Programs natively on a single-thread `io_context` and on a `HostLoop` over a
  manual executor, so a native build covers the same paths.
- `tests/test_host_loop.cpp` runs the loop and the host-seam provider natively against a manual executor, so a native
  build covers the same paths.

## Measured sizes

Release, `-O3 -flto`, Emscripten 6.0.13. The graph smoke contains the Core engine and the SDK runtime; the module adds
the LLM component, the SDK's JavaScript host and the C API.

| Build | `.wasm` | `.wasm` gzip -9 | `.js` |
|---|---:|---:|---:|
| Graph smoke, single-thread | 5,509,570 B (5.3 MiB) | 1,608,986 B (1.5 MiB) | 83,223 B |
| Graph smoke, pthread | 5,448,684 B (5.2 MiB) | 1,660,246 B (1.6 MiB) | 103,691 B |
| Checks with Program and LLM, single-thread | 6,039,737 B (5.8 MiB) | 1,842,525 B (1.8 MiB) | 84,263 B |
| JavaScript module, single-thread | 6,270,525 B (6.0 MiB) | 1,835,562 B (1.8 MiB) | 87,762 B |

CI fails the single-thread smoke above 6,000,000 bytes of `.wasm` or 1,750,000 bytes gzipped (the measured size plus
about 9%), and the module above 6,850,000 bytes of `.wasm` or 2,010,000 bytes gzipped, and reports the sizes in the job summary. A Worker's script has a size
limit that depends on its plan; compare the module's compressed size with the limit of yours. The 712 KB figure in
earlier versions of this page was measured before the typed SDK and no longer applies.

## What is unavailable on these hosts

Calls into these fail with a typed error where NeoGraph owns the call, and are not built otherwise.

- **Model calls other than through the host.** ASYNC, MCP client and A2A need sockets and TLS. Model calls go through
  the host's `fetch` with the module above; a build for another WebAssembly host supplies its own executor and
  transport to `SchemaProvider`.
- **Blocking entry points on a host-pumped provider.** `run()`, `Provider::dispatch` and the like refuse (above).
- **Program stores on a host database.** The Program runtime runs on the host loop (above), but its stores are the
  in-memory ones: the SQLite and PostgreSQL stores need system libraries, and a store that reaches a host database
  waits for the store interfaces to become awaitable ([#556](https://github.com/fox1245/NeoGraph/issues/556)). The
  synchronous Program calls refuse on a host loop; use the `_async` forms.
- **Native Program controls.** The C v1 plugin ABI is a 64-bit layout, so `NativeControlBinding::create` refuses on
  wasm32 with `NativeControlUnavailable`.
- **Synchronous checkpoint stores.** A store that is not asynchronous runs each operation on a pool of worker threads.
  A single-thread build has none, so the operation is refused with `SynchronousCheckpointStoreUnavailable` instead of
  waiting for a thread; implement `AsyncCheckpointStore`. The built-in in-memory store runs its operations, managed
  budgets included, on the calling thread, so a run with `model_token_budget` works on it.
- **A managed budget with a deadline.** The deadline is bound to a clock origin that a restart can recover, and a
  WebAssembly host has none. The run is refused with `ManagedBudgetDeadlineClockUnavailable`, before the store is
  asked.
- **The native archive.** There is no file custody backend; the SDK refuses with an `Unsupported` error.
- **Subprocess delegation** (Harness CLIs, stdio MCP servers), **listening sockets**, **gRPC**, **file-path
  uploads** and **Python**.

## Browser

The browser harness is not in the tree: `serve_smoke.py` expects a `smoke.html` that does not exist, and no browser
run has been recorded (#435). A Node.js run is not browser evidence. The module is built for the `web`, `worker` and
`node` environments, but only Node is run.

## Milestones

The plan after the toolchain review is in [#535](https://github.com/fox1245/NeoGraph/issues/535):

- **M0 and M1.** The wasm32 fixes, the single-thread build, and the Node smoke with a size gate in CI.
- **M2 (this page).** Model calls through the host's `fetch`: the host loop, the host-seam provider, the JavaScript
  module, and the Node and Workers examples.
- **M3.** Programs on a host store ([#557](https://github.com/fox1245/NeoGraph/issues/557),
  [#556](https://github.com/fox1245/NeoGraph/issues/556)). The runtime half is in: `ProgramRuntime` runs on
  `HostLoop::executor()` ("Programs on the host loop" above). The store half follows: awaitable store interfaces.
- **M4.** Realtime sessions, WASI, a custody backend for native archives, and remote Harness.
