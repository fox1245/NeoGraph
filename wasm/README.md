# NeoGraph WASM — Phase 1 smoke build

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

This directory contains the Phase 1 WebAssembly smoke program. It verifies
that the core engine can compile and run a graph under Emscripten without a
WASM-specific fork. It is a Node.js smoke path, not a browser SDK.

## Historical Result

The following numbers came from an earlier local smoke run. They are kept as
reference points only; no generated `.wasm` or `.js` artifact is committed and
the repository does not currently publish a WASM size artifact in CI.

| Metric | Value |
|---|---|
| WASM binary (-O3 + LTO) | **712 KB** |
| Emscripten JS runtime    | 92 KB |
| Total JavaScript + WASM  | **~800 KB** |
| Engine source diff       | 0 lines |
| First run output         | `doubled = 42, trace = d` ✓ |

For comparison: the referenced native NG build totals 5.5 MB; the LangGraph stack
(langgraph + langchain + openai + httpx + pydantic + langsmith) is 31 MB
of pure-Python that does not even attempt to ship to a browser. NG
fits inside L3 cache twice over and is small enough that a typical SaaS
landing page already loads more JS than this engine takes.

## What runs today (Phase 1)

- `GraphEngine::compile(json)` — JSON definition → executable engine.
- `engine->run(cfg)` — synchronous run with InMemoryCheckpointStore.
- Custom nodes registered via `NodeFactory::register_type` — leaf
  semantics carry over from the C++ / Python paths.
- The phase-1 smoke covered: `set_worker_count`,
  `set_node_cache_enabled`, channels with reducers, conditional edges,
  Send fan-out, Command routing, interrupts.
- C++20 coroutines (asio's header-only `awaitable` pieces) work under
  Emscripten 5.0.

## What's deliberately not shipped yet

| Subsystem | Why deferred | Phase |
|---|---|---|
| `neograph_async` (HTTP/WebSocket via asio) | Browser uses `fetch` / native WebSocket, not raw sockets | 2 |
| `neograph_llm` (SchemaProvider, OpenAIProvider) | Depends on the async transport above | 2 |
| `neograph_postgres` | Browser irrelevant | — |
| `neograph_mcp` | Subprocess-based, browser irrelevant | — |
| Embind JS bindings | Lets JS define node implementations as callbacks | 2-A |

## Build And Run

```bash
source /opt/emsdk/emsdk_env.sh

emcmake cmake -S . -B build-wasm \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_WASM=ON \
  -DNEOGRAPH_BUILD_ASYNC=OFF \
  -DNEOGRAPH_BUILD_LLM=OFF \
  -DNEOGRAPH_BUILD_MCP=OFF \
  -DNEOGRAPH_BUILD_MCP_CLIENT=OFF \
  -DNEOGRAPH_BUILD_MCP_SERVER=OFF \
  -DNEOGRAPH_BUILD_MCP_HTTP_SERVER=OFF \
  -DNEOGRAPH_BUILD_A2A=OFF \
  -DNEOGRAPH_BUILD_ACP=OFF \
  -DNEOGRAPH_BUILD_GRPC=OFF \
  -DNEOGRAPH_BUILD_POSTGRES=OFF \
  -DNEOGRAPH_BUILD_SQLITE=OFF \
  -DNEOGRAPH_BUILD_UTIL=OFF \
  -DNEOGRAPH_BUILD_EXAMPLES=OFF \
  -DNEOGRAPH_BUILD_TESTS=OFF \
  -DNEOGRAPH_BUILD_BENCHMARKS=OFF \
  -DNEOGRAPH_USE_LIBCURL=OFF
cmake --build build-wasm --target neograph_wasm_smoke -j
node build-wasm/wasm/smoke.js
```

The target links `neograph_core` directly, so its source list is maintained by
the main CMake build rather than copied into this document. Expected output
includes `doubled = 42` and the node trace.

`compile()` defaults to `worker_count=1` and therefore creates no engine-owned
thread pool. This smoke command still enables a four-thread Emscripten pool so
callers can opt into parallel fan-out with `set_worker_count(N >= 2)`; the smoke
itself uses the single-worker default. For a single-thread build, pass
`-sPTHREAD_POOL_SIZE=0`; no `set_worker_count(1)` call is needed.

## Browser Status

There is no browser loader, npm package, or Embind API in this repository yet.
The current target is intentionally Node.js-only. A browser build that uses
Emscripten pthreads also needs a web server with cross-origin isolation headers
(`Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp`) plus the generated worker assets.

## Phase 2 sketch

1. **2-A — Embind JS bindings.** Expose `GraphEngine`, `RunConfig`,
   `ChannelWrite`, `Send`, `Command` to JS. A JS function can register
   itself as a node implementation; the engine calls back into JS for
   each node execution. Estimated 1-2 days.

2. **2-B — fetch-based HTTP transport.** Provide a transport
   interface that `SchemaProvider` consumes; the WASM build wires it
   to `fetch()`. Same provider code targets either backend. Estimated
   3-5 days.

3. **2-C — npm package.** Publish as `@neograph/wasm` so apps can
   `npm install` the engine + JS bindings without their own build.
   Estimated 1-2 days.

After Phase 2 the engine can run Originator-issued graphs entirely in
a browser tab — leaves call BYOK Anthropic / OpenAI / Bedrock keys via
`fetch()`, transformers.js / built-in AI for local inference, and
results flow back through channels into a Result Envelope. That's the
runtime side of the
[NeoProtocol](https://github.com/fox1245/NeoProtocol) Executor role.
