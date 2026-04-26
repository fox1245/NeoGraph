# NeoGraph WASM — feasibility spike

The graph engine compiled to WebAssembly. This directory is the
**phase-1 spike** — proof that the engine layer (compile, run, executor,
scheduler, coordinator, state, channels, NodeCache) builds and executes
unmodified under Emscripten.

## Result

| Metric | Value |
|---|---|
| WASM binary (-O3 + LTO) | **712 KB** |
| Emscripten JS runtime    | 92 KB |
| Total ship size          | **~800 KB** |
| Engine source diff       | 0 lines |
| First run output         | `doubled = 42, trace = d` ✓ |

For comparison: native NG totals 5.5 MB; the LangGraph stack
(langgraph + langchain + openai + httpx + pydantic + langsmith) is 31 MB
of pure-Python that does not even attempt to ship to a browser. NG
fits inside L3 cache twice over and is small enough that a typical SaaS
landing page already loads more JS than this engine takes.

## What runs today (Phase 1)

- `GraphEngine::compile(json)` — JSON definition → executable engine.
- `engine->run(cfg)` — synchronous run with InMemoryCheckpointStore.
- Custom nodes registered via `NodeFactory::register_type` — leaf
  semantics carry over from the C++ / Python paths.
- All the v0.1.6 features compile clean: `set_worker_count`,
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

## Build

```bash
source /opt/emsdk/emsdk_env.sh

em++ -std=c++20 -O3 -flto -fexceptions -pthread \
  -sALLOW_MEMORY_GROWTH=1 -sPTHREAD_POOL_SIZE=4 \
  -DASIO_STANDALONE -DASIO_NO_DEPRECATED \
  -I include -I deps/asio/include -I deps/yyjson \
  wasm/smoke.cpp \
  src/core/json.cpp deps/yyjson/yyjson.c \
  src/core/graph_engine.cpp src/core/graph_compiler.cpp \
  src/core/graph_coordinator.cpp src/core/graph_executor.cpp \
  src/core/scheduler.cpp src/core/graph_state.cpp \
  src/core/graph_node.cpp src/core/graph_loader.cpp \
  src/core/graph_checkpoint.cpp src/core/store.cpp \
  src/core/provider.cpp src/core/tool.cpp \
  src/core/react_graph.cpp src/core/plan_execute_graph.cpp \
  src/core/deep_research_graph.cpp src/core/node_cache.cpp \
  -o wasm/smoke.js
```

Run with `node wasm/smoke.js`. No browser flags needed.

`-pthread` is required because `compile()` provisions a default
thread_pool sized to `hardware_concurrency()`. Single-threaded WASM is
also possible — pass `-sPTHREAD_POOL_SIZE=0` and call
`engine->set_worker_count(1)` before `run()`.

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
