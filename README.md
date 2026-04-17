<p align="center">
  <h1 align="center">NeoGraph</h1>
  <p align="center">
    <strong>A C++ Graph Agent Engine Library</strong><br>
    LangGraph for C++ — build, checkpoint, and orchestrate LLM agents with zero Python dependency.
  </p>
</p>

<p align="center">
  <a href="https://fox1245.github.io/NeoGraph/">API Reference</a> &middot;
  <a href="#quick-start">Quick Start</a> &middot;
  <a href="#features">Features</a> &middot;
  <a href="#examples">Examples</a> &middot;
  <a href="#architecture">Architecture</a> &middot;
  <a href="#comparison">vs LangGraph</a> &middot;
  <a href="#benchmarks">Benchmarks</a> &middot;
  <a href="#license">License</a>
</p>

---

## What is NeoGraph?

NeoGraph is a **C++17 graph-based agent orchestration engine** that brings LangGraph-level capabilities to C++. Define agent workflows as JSON, execute them with parallel fan-out, checkpoint state for time-travel debugging, and integrate any LLM provider — all without Python.

```cpp
#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/graph/react_graph.h>

auto provider = neograph::llm::OpenAIProvider::create({
    .api_key = "sk-...", .default_model = "gpt-4o-mini"
});
auto engine = neograph::graph::create_react_graph(provider, std::move(tools));

neograph::graph::RunConfig config;
config.input = {{"messages", json::array({{{"role","user"},{"content","Hello!"}}})}};
auto result = engine->run(config);
```

## Why NeoGraph?

| Python + LangGraph | C++ + NeoGraph (measured) |
|---|---|
| ~500 MB runtime (Python + deps) | **982 KB static binary** (stripped, `example_plan_executor`) |
| ~300 MB steady RSS | **3.1 MB peak RSS** (Plan & Executor run) |
| 2–8 s import / cold start | **< 250 ms** end-to-end (crash + resume cycle included) |
| GIL-limited parallelism | Taskflow work-stealing + lock-free RequestQueue |
| Cloud / server only | Raspberry Pi Zero 2W, Jetson, drones, IoT, edge |

All figures are from `example_plan_executor` on x86_64 Linux built with
`CMAKE_BUILD_TYPE=MinSizeRel`, `-ffunction-sections -fdata-sections`,
`-static-libstdc++ -static-libgcc -Wl,--gc-sections`, then stripped.
Only runtime dependency is `libc.so.6`. See the [Benchmarks](#benchmarks)
section below for the reproduction command.

**NeoGraph is the only graph agent engine for C++.** If you're building agents in robotics, embedded systems, games, high-frequency trading, or anywhere Python isn't an option — this is it.

## Quick Start

### Requirements

- C++17 compiler (GCC 9+, Clang 10+, MSVC 2019+)
- CMake 3.16+
- OpenSSL (for HTTPS)

### Build

```bash
git clone https://github.com/fox1245/NeoGraph.git
cd NeoGraph
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Run an example (no API key needed)

```bash
./example_custom_graph      # Mock ReAct agent
./example_parallel_fanout   # Parallel fan-out/fan-in (150ms vs 370ms sequential)
./example_send_command      # Dynamic Send + Command routing
```

### Integration

**FetchContent (recommended):**
```cmake
include(FetchContent)
FetchContent_Declare(neograph
  GIT_REPOSITORY https://github.com/fox1245/NeoGraph.git
  GIT_TAG main)
FetchContent_MakeAvailable(neograph)

target_link_libraries(my_app PRIVATE neograph::core neograph::llm)
```

**add_subdirectory:**
```cmake
add_subdirectory(deps/neograph)
target_link_libraries(my_app PRIVATE neograph::core neograph::llm)
```

## Features

### Core Engine (`neograph::core`)

- **JSON-defined graphs** — No recompilation to change agent workflows
- **Super-step execution** — Pregel BSP model with cycle support
- **Parallel fan-out/fan-in** — Taskflow work-stealing scheduler
- **Send (dynamic fan-out)** — Nodes spawn N parallel tasks at runtime
- **Command (routing override)** — Nodes control routing + state in one return
- **Checkpointing** — Full state snapshots at every super-step
- **HITL (Human-in-the-Loop)** — `interrupt_before` / `interrupt_after` + `resume()`
- **State management** — `get_state()`, `update_state()`, `fork()`, time-travel
- **Dynamic breakpoints** — `throw NodeInterrupt("reason")` from any node
- **Retry policies** — Per-node exponential backoff with configurable limits
- **Stream modes** — `EVENTS | TOKENS | VALUES | UPDATES | DEBUG` bitflags
- **Subgraphs** — Hierarchical composition via JSON (Supervisor pattern)
- **Intent routing** — LLM-based classification + dynamic routing
- **Cross-thread Store** — Namespace-based shared memory across threads
- **Custom nodes** — Register via `NodeFactory` with zero framework changes

### LLM Providers (`neograph::llm`)

- **OpenAIProvider** — OpenAI, Groq, Together, vLLM, Ollama (any OpenAI-compatible API)
- **SchemaProvider** — Claude, Gemini, and any custom provider via JSON schema
- **Built-in schemas** — `"openai"`, `"claude"`, `"gemini"` embedded at build time
- **Agent** — ReAct loop with streaming support

### MCP Client (`neograph::mcp`)

- **JSON-RPC 2.0** over HTTP (Streamable HTTP transport)
- **Tool discovery** — Auto-discover tools from any MCP server
- **stdio transport** — Launch MCP servers as subprocesses

### Utilities (`neograph::util`)

- **RequestQueue** — Lock-free worker pool with backpressure (moodycamel::ConcurrentQueue)

## Examples

| # | Example | Description | API Key |
|---|---------|-------------|---------|
| 01 | `react_agent` | Basic ReAct agent with calculator tool | Required |
| 02 | `custom_graph` | JSON-defined graph with mock provider | No |
| 03 | `mcp_agent` | Real MCP server tool integration | Required |
| 04 | `checkpoint_hitl` | Checkpointing + Human-in-the-Loop (interrupt/resume) | No |
| 05 | `parallel_fanout` | Taskflow parallel fan-out/fan-in (3 workers, 150ms) | No |
| 06 | `subgraph` | Hierarchical graph composition (Supervisor pattern) | No |
| 07 | `intent_routing` | Intent classification + expert routing | No |
| 08 | `state_management` | get_state / update_state / fork / time-travel | No |
| 09 | `all_features` | All 6 advanced features in one demo | No |
| 10 | `send_command` | Dynamic Send fan-out + Command routing override | No |
| 11 | `clay_chatbot` | Multi-turn chatbot UI (Clay + Raylib) | Optional |
| 12 | `rag_agent` | RAG agent with in-memory vector search (CLI) | Required |

### Run with a real LLM

```bash
# Set your API key
echo "OPENAI_API_KEY=sk-..." > .env

# ReAct agent with OpenAI
./example_react_agent

# MCP agent (start demo server first: python examples/demo_mcp_server.py)
./example_mcp_agent http://localhost:8000 "What time is it?"

# Visual chatbot
cmake .. -DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON && make example_clay_chatbot
./example_clay_chatbot --live
```

## Architecture

```
User Code
  │
  ▼
┌──────────────────────────────────────────┐
│  neograph::core                          │
│                                          │
│  GraphEngine::compile(json, context)     │
│    ├─ NodeFactory (llm_call, tool_dispatch, subgraph, intent_classifier, custom)
│    ├─ ReducerRegistry (overwrite, append, custom)
│    └─ ConditionRegistry (has_tool_calls, route_channel, custom)
│                                          │
│  GraphEngine::run(config)                │
│    ├─ Super-step loop (Pregel BSP)       │
│    ├─ Single node → direct call          │
│    ├─ Multiple nodes → Taskflow parallel │
│    ├─ Send → dynamic fan-out             │
│    ├─ Command → routing override         │
│    ├─ Checkpoint after each step         │
│    └─ interrupt → resume()               │
│                                          │
│  Dependencies: nlohmann/json, Taskflow   │
│  No network. No HTTP. Pure computation.  │
├──────────────────────────────────────────┤
│  neograph::llm           (PRIVATE httplib)│
│  OpenAIProvider, SchemaProvider, Agent    │
├──────────────────────────────────────────┤
│  neograph::mcp           (PRIVATE httplib)│
│  MCPClient, MCPTool (JSON-RPC 2.0)       │
├──────────────────────────────────────────┤
│  neograph::util                          │
│  RequestQueue (lock-free worker pool)    │
└──────────────────────────────────────────┘
```

### Dependency Isolation

```
Link target               What gets pulled in
─────────────────          ──────────────────
neograph::core             nlohmann/json (header-only)
neograph::core + llm       + OpenSSL (httplib stays PRIVATE)
neograph::core + mcp       + OpenSSL (httplib stays PRIVATE)
neograph::util             + concurrentqueue (header-only)
```

`httplib` is never exposed to your code. `core` has zero network dependencies.

## Concurrency & Async

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

Works the same way with a `boost::asio::thread_pool`, a `taskflow::Executor`,
or your web framework's worker pool — NeoGraph stays out of the executor
decision.

### Using the bundled `RequestQueue`

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

**Rules for safe concurrent use:**

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

Coverage: `tests/test_graph_engine.cpp` contains
`ConcurrentRunDifferentThreadIds` (16 threads × 25 runs = 400 parallel
executions, validates per-session output + checkpoint isolation) and
`ConcurrentRunSameThreadIdNoCrash` (8 threads × 50 runs on one shared
`thread_id`, validates crash-free behavior).

## JSON Graph Definition

```json
{
  "name": "research_agent",
  "channels": {
    "messages": {"reducer": "append"},
    "findings": {"reducer": "append"},
    "__route__": {"reducer": "overwrite"}
  },
  "nodes": {
    "planner":    {"type": "llm_call"},
    "researcher": {"type": "tool_dispatch"},
    "classifier": {
      "type": "intent_classifier",
      "routes": ["deep_dive", "summarize"]
    },
    "inner_agent": {
      "type": "subgraph",
      "definition": { "...nested graph..." }
    }
  },
  "edges": [
    {"from": "__start__", "to": "planner"},
    {"from": "planner", "condition": "has_tool_calls",
     "routes": {"true": "researcher", "false": "classifier"}},
    {"from": "researcher", "to": "planner"},
    {"from": "classifier", "condition": "route_channel",
     "routes": {"deep_dive": "inner_agent", "summarize": "__end__"}}
  ],
  "interrupt_before": ["researcher"]
}
```

## Comparison with LangGraph

| Feature | LangGraph (Python) | NeoGraph (C++) |
|---------|-------------------|----------------|
| Graph engine | StateGraph | GraphEngine |
| Checkpointing | MemorySaver + Postgres/SQLite/Redis | CheckpointStore (interface) + InMemory |
| HITL | interrupt_before/after | interrupt_before/after + NodeInterrupt |
| get_state / update_state | Yes | Yes |
| Fork | Yes | Yes |
| Time travel | get_state_history | get_state_history |
| Subgraphs | CompiledGraph as node | SubgraphNode (JSON inline) |
| Parallel fan-out | Static | Taskflow work-stealing |
| Send (dynamic fan-out) | Send() | NodeResult::sends → Taskflow parallel |
| Command (routing+state) | Command(goto, update) | NodeResult::command |
| Retry policy | RetryPolicy | RetryPolicy + exponential backoff |
| Stream modes | values/updates/messages | EVENTS/TOKENS/VALUES/UPDATES/DEBUG |
| Cross-thread Store | Store (Postgres) | Store (interface) + InMemory |
| Multi-LLM | LangChain required | SchemaProvider built-in (3 vendors) |
| MCP support | None (separate impl) | MCPClient built-in |
| Performance | Python (GIL) | C++ + Taskflow |
| Memory footprint | ~300MB+ | ~10MB |
| Edge/embedded | Not possible | Raspberry Pi, Jetson, IoT |

## Project Structure

```
NeoGraph/
├── include/neograph/
│   ├── neograph.h              # Convenience header
│   ├── types.h                 # ChatMessage, ToolCall, ChatCompletion
│   ├── provider.h              # Provider interface (abstract)
│   ├── tool.h                  # Tool interface (abstract)
│   ├── graph/
│   │   ├── types.h             # Channel, Edge, NodeContext, GraphEvent,
│   │   │                       # NodeInterrupt, Send, Command, RetryPolicy, StreamMode
│   │   ├── state.h             # GraphState (thread-safe channels)
│   │   ├── node.h              # GraphNode, LLMCallNode, ToolDispatchNode,
│   │   │                       # IntentClassifierNode, SubgraphNode
│   │   ├── engine.h            # GraphEngine, RunConfig, RunResult
│   │   ├── checkpoint.h        # CheckpointStore, InMemoryCheckpointStore
│   │   ├── store.h             # Store, InMemoryStore (cross-thread memory)
│   │   ├── loader.h            # NodeFactory, ReducerRegistry, ConditionRegistry
│   │   └── react_graph.h       # create_react_graph() convenience
│   ├── llm/
│   │   ├── openai_provider.h   # OpenAI-compatible provider
│   │   ├── schema_provider.h   # Multi-vendor LLM (JSON schema driven)
│   │   ├── agent.h             # ReAct agent loop
│   │   └── json_path.h         # JSON dot-path utilities
│   ├── mcp/
│   │   └── client.h            # MCP client + tool wrapper
│   └── util/
│       └── request_queue.h     # Lock-free worker pool
├── src/
│   ├── core/                   # 7 source files
│   ├── llm/                    # 3 source files
│   └── mcp/                    # 1 source file
├── schemas/                    # Built-in LLM provider schemas
│   ├── openai.json
│   ├── claude.json
│   └── gemini.json
├── deps/                       # Header-only dependencies (vendored)
│   ├── nlohmann/json.hpp
│   ├── taskflow/
│   ├── httplib.h
│   ├── concurrentqueue.h
│   └── clay.h
├── examples/                   # 11 examples + demo MCP server
└── scripts/
    └── embed_schemas.py        # Build-time schema embedding
```

## CMake Targets

| Target | Description | Dependencies |
|--------|-------------|--------------|
| `neograph::core` | Graph engine + types | nlohmann/json, Taskflow, Threads |
| `neograph::llm` | LLM providers + Agent | core + OpenSSL (httplib PRIVATE) |
| `neograph::mcp` | MCP client | core + OpenSSL (httplib PRIVATE) |
| `neograph::util` | RequestQueue | core + concurrentqueue |

## Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `NEOGRAPH_BUILD_LLM` | ON | Build LLM provider module |
| `NEOGRAPH_BUILD_MCP` | ON | Build MCP client module |
| `NEOGRAPH_BUILD_UTIL` | ON | Build utility module |
| `NEOGRAPH_BUILD_EXAMPLES` | ON | Build example programs |
| `NEOGRAPH_BUILD_CLAY_EXAMPLE` | OFF | Build Clay+Raylib chatbot (fetches Raylib) |

## Benchmarks

### Engine overhead vs LangGraph

Matched-topology, zero-I/O workloads: graph compiled once, invoked in a
hot loop. Measures what the engine itself costs (dispatch, state
writes, reducer calls) — no LLM, no sleep, no network.

![NeoGraph vs LangGraph — per-iteration latency and peak RSS](docs/images/bench-engine-overhead.png)

|                           | NeoGraph    | LangGraph    | Ratio |
|---------------------------|-------------|--------------|-------|
| Per-iter, 3-node chain    | **20.65 µs** | 645.30 µs   | **31.2× faster** |
| Per-iter, fan-out 5 + join| **150.7 µs** | 2,225 µs    | **14.8× faster** |
| Peak RSS (whole process)  | **4.9 MB**   | 58.9 MB     | **12× less RAM** |
| Total elapsed, full bench | 1.92 s       | 35.64 s     | 18.6× |
| CPU utilization on fan-out| 407% (Taskflow multi-core) | 100% (GIL-bound) | — |

**Engine overhead disappears under LLM latency.** A 500 ms OpenAI round
trip swamps both engines; the per-iter gap only shows up in non-LLM
nodes (data transforms, routing decisions, pure-compute tool calls) and
in dense agent orchestration. Where it does show up, it shows up big:
on a Raspberry Pi 4 / Jetson Nano / any SBC class target, a 12× RAM
delta is the difference between "fits" and "swap thrash."

Reproduction and methodology: [`benchmarks/README.md`](benchmarks/README.md).

### Size & cold-start footprint (Plan & Executor demo)

All numbers below were measured on x86_64 Linux (GCC 13) using
`example_plan_executor` — a self-contained Plan & Executor demo that
runs a 5-way Send fan-out, crashes sub-topic #2 on the first run, and
resumes with the failure cleared. No LLM calls, no API keys, no network.

### Binary size (MinSizeRel + static libstdc++ + strip)

| Build configuration | Size |
|---|---|
| Debug (dev default) | 12.2 MB |
| Release `-O3`, dynamic libstdc++, stripped | **653 KB** |
| **MinSizeRel `-Os`, static libstdc++, `--gc-sections`, stripped** | **982 KB** |

The MinSizeRel binary's only dynamic dependency is `libc.so.6` —
`libstdc++` and `libgcc_s` are linked in statically. Drop it onto any
Linux host with a matching libc and it runs.

Per-object contribution (Release, `.text` section):

| Object | Size | Role |
|---|---|---|
| `graph_engine.cpp.o` | 263 KB | Super-step loop, Taskflow fan-out, Send, pending writes |
| `graph_node.cpp.o` | 120 KB | Built-in node types (LLMCall, ToolDispatch, Subgraph, IntentClassifier) |
| `graph_loader.cpp.o` | 112 KB | JSON → graph compiler |
| `graph_checkpoint.cpp.o` | 67 KB | CheckpointStore + PendingWrite machinery |
| `graph_state.cpp.o` | 63 KB | Thread-safe channel store |
| `react_graph.cpp.o` | 39 KB | `create_react_graph()` convenience |
| `store.cpp.o` | 28 KB | Cross-thread Store |

### Runtime footprint

| Metric | Value |
|---|---|
| Peak RSS (full Plan & Executor run, crash + resume included) | **3.1 MB** |
| Wall-clock (cold start → both phases complete) | **~240 ms** |
| Dynamic dependencies | `libc.so.6` only |

### Reproduction

```bash
git clone https://github.com/fox1245/NeoGraph.git
cd NeoGraph

cmake -B build-minsize -S . \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DNEOGRAPH_BUILD_MCP=OFF \
    -DNEOGRAPH_BUILD_TESTS=OFF \
    -DCMAKE_CXX_FLAGS="-ffunction-sections -fdata-sections" \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,--gc-sections -static-libstdc++ -static-libgcc"

cmake --build build-minsize --target example_plan_executor -j$(nproc)

strip --strip-all build-minsize/example_plan_executor
ls -la    build-minsize/example_plan_executor        # binary size
ldd       build-minsize/example_plan_executor        # dynamic deps (libc only)
/usr/bin/time -v build-minsize/example_plan_executor  # peak RSS + wall time
```

### What the numbers mean for embedded / robotics

- **982 KB static binary** fits a Docker `scratch` image under 1 MB, fits
  on-board flash of a Pixhawk companion computer, fits in the first 1 MB
  of a Jetson Orin boot partition. Python + LangGraph does not.
- **3.1 MB RSS** means you can host **100+ concurrent agent sessions**
  on an RPi Zero 2W (512 MB RAM) by sharing one compiled engine across
  threads — the [Concurrency & Async](#concurrency--async) section covers
  the pattern.
- **< 250 ms cold start** fits inside a drone watchdog reset window;
  a Python LangGraph process still hasn't finished `import` by then.
- **`libc.so.6` only** makes cross-compilation trivial: pick `glibc` or
  `musl` and link — no transitive dependency hell.

## Acknowledgments

Inspired by:
- [LangGraph](https://github.com/langchain-ai/langgraph) — Graph agent orchestration for Python
- [agent.cpp](https://github.com/mozilla-ai/agent.cpp) — Local LLM agent framework for C++
- [Taskflow](https://github.com/taskflow/taskflow) — Parallel task programming in C++
- [Clay](https://github.com/nicbarker/clay) — High-performance UI layout library

## License

MIT License. See [LICENSE](LICENSE) for details.

Third-party licenses: [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES)
