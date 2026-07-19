# NeoGraph WASM runtime

NeoGraph's current C++ graph engine compiles to WebAssembly and is exposed
through a small JSON ABI for Node.js and browsers. The original v0.1.6
feasibility spike remains as `smoke.cpp`; `bindings.cpp` is the production npm
surface consumed by `@neograph/wasm`.

## Result

| Metric | Value |
|---|---|
| Current core WASM binary (Release) | **1,045 KB** |
| Emscripten ES module              | **114 KB** |
| Compressed npm package            | **366 KB** |
| Engine source files               | **27** |
| Node + browser smoke output       | `NeoGraph 0.11.1: 42` |

For comparison: native NG totals 5.5 MB; the LangGraph stack
(langgraph + langchain + openai + httpx + pydantic + langsmith) is 31 MB
of pure-Python that does not even attempt to ship to a browser. NG
fits inside L3 cache twice over and is small enough that a typical SaaS
landing page already loads more JS than this engine takes.

## What runs today

- `GraphEngine::compile`, `run`, and `resume` through JSON strings.
- `InMemoryCheckpointStore` for multi-turn and HITL state.
- Synchronous JavaScript node callbacks registered by type.
- Channels, reducers, conditional edges, Send, Command, interrupts, tracing,
  validation, history, evolution, and the current C++20 coroutine engine.
- Emscripten 6.0.3 single-thread output for Node.js and browsers.

## What's deliberately not shipped yet

| Subsystem | Why deferred | Phase |
|---|---|---|
| `neograph_async` (HTTP/WebSocket via asio) | Browser uses TypeScript `fetch` / native WebSocket adapters | adapter |
| `neograph_llm` native providers | Async work returns an interrupt and resumes with the JS result | adapter |
| `neograph_postgres` | Browser irrelevant | — |
| `neograph_mcp` | Subprocess-based, browser irrelevant | — |
| Persistent SQLite | Starts in-memory; OPFS-backed storage is future work | future |

## Build

```bash
docker run --rm -v "$PWD:/src" -w /src \
  emscripten/emsdk:6.0.3 \
  emcmake cmake -S . -B build/wasm-release \
    -DCMAKE_BUILD_TYPE=Release \
    -DNEOGRAPH_BUILD_WASM=ON

docker run --rm -v "$PWD:/src" -w /src \
  emscripten/emsdk:6.0.3 \
  cmake --build build/wasm-release --target neograph_wasm --parallel

node wasm/smoke.mjs build/wasm-release/neograph.js
```

The runtime is single-threaded and intended to run inside a Web Worker, so a
normal static server is sufficient. A future pthread variant needs a separate
callback design because `emscripten::val` handles cannot cross worker threads;
merely enabling `-pthread` would add SharedArrayBuffer requirements without
making the exposed JavaScript nodes safely parallel.

## JavaScript boundary

`bindings.cpp` deliberately exposes JSON strings rather than mirroring every
C++ class through Embind. This keeps ownership explicit and isolates the npm
API from C++ ABI changes. JavaScript callbacks are synchronous. A callback that
needs Fetch, DOM, WebGPU, or another async browser API returns an interrupt;
TypeScript performs the operation and calls `resume` with its result.

The generated `neograph.js` and `neograph.wasm` are copied into
[NeoGraph-TypeScript](https://github.com/fox1245/NeoGraph-TypeScript) by
`scripts/sync-wasm.mjs`, which records the source revision, Emscripten image,
and SHA-256 hashes in the npm package. The sync script rejects a dirty NeoGraph
source tree for releases; `--allow-dirty` exists only for local development.
