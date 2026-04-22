<p align="center">
  <h1 align="center">NeoGraph</h1>
  <p align="center">
    <strong>A C++ Graph Agent Engine Library</strong><br>
    Microsecond tail latency under 10k concurrent requests on 512 MB — LangGraph's semantics, without the Python runtime tax.
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
| ~500 MB runtime (Python + deps) | **1.1 MB static binary** (stripped, `example_plan_executor`) |
| ~300 MB steady RSS | **2.9 MB peak RSS** (Plan & Executor run) |
| 2–8 s import / cold start | **< 250 ms** end-to-end (crash + resume cycle included) |
| GIL-limited parallelism | `asio::thread_pool` fan-out + lock-free RequestQueue |
| Cloud / server only | Raspberry Pi Zero 2W, Jetson, drones, IoT, edge |

All figures are from `example_plan_executor` on x86_64 Linux built with
`CMAKE_BUILD_TYPE=MinSizeRel`, `-ffunction-sections -fdata-sections`,
`-static-libstdc++ -static-libgcc -Wl,--gc-sections`, then stripped.
Only runtime dependency is `libc.so.6`. See the [Benchmarks](#benchmarks)
section below for the reproduction command.

**NeoGraph is the only graph agent engine for C++.** If you're building agents in robotics, embedded systems, games, high-frequency trading, or anywhere Python isn't an option — this is it.

## Quick Start

### Requirements

- **C++20** compiler (GCC 13+, Clang 15+, MSVC 2022+) — coroutines
  are on the public API surface as of 2.0.0.
- CMake 3.16+.
- OpenSSL (HTTPS), libpq (optional, Postgres checkpoint),
  SQLite3 (optional, SQLite checkpoint).

### Platform support (3.0 draft, `feat/taskflow-removal`)

| Platform | Tier | Notes |
|---|---|---|
| Linux (Ubuntu 24.04, GCC 13) | **GA** | Reference — 338/338 ctest green (292 under Valgrind memcheck + ASan/UBSan clean), all paths validated locally |
| macOS (Apple Silicon, Clang) | **beta** | CI builds + non-Postgres tests; runtime differences (coroutine scheduling, SIGPIPE) not yet exercised in production |
| Windows (MSVC 2022, x64) | **alpha** | CI builds + non-Postgres tests; MCP stdio (named-pipe overlapped) + PG async socket wrap written against MSDN spec but unvalidated under load |

CI matrix (GitHub Actions): `build-and-test` (Ubuntu, full with PG
service), `build-macos`, `build-windows`, `bench-regression` (3
committed floors). See [`CHANGELOG.md`](CHANGELOG.md) for the full
stability rationale per platform.

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
./example_parallel_fanout   # Parallel fan-out/fan-in (3 researchers run concurrently)
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
- **Parallel fan-out/fan-in** — `asio::experimental::make_parallel_group` on the engine's executor; opt-in `asio::thread_pool` for CPU-bound branches via `set_worker_count(N)`
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

- **HTTP transport** — JSON-RPC 2.0 over Streamable HTTP, session-aware
- **stdio transport** — `MCPClient({"python", "server.py"})` spawns the
  MCP server as a child subprocess and exchanges newline-delimited
  JSON-RPC over its stdin / stdout; subprocess lifetime is tied to the
  last MCPTool that references it
- **Tool discovery** — `get_tools()` auto-discovers tools from either
  transport; returned `MCPTool`s plug straight into `Agent` / `GraphEngine`

### Utilities (`neograph::util`)

- **RequestQueue** — Lock-free worker pool with backpressure (moodycamel::ConcurrentQueue)

## Examples

| # | Example | Description | API Key |
|---|---------|-------------|---------|
| 01 | `react_agent` | Basic ReAct agent with calculator tool | Required |
| 02 | `custom_graph` | JSON-defined graph with mock provider | No |
| 03 | `mcp_agent` | Real MCP server tool integration | Required |
| 04 | `checkpoint_hitl` | Checkpointing + Human-in-the-Loop (interrupt/resume) | No |
| 05 | `parallel_fanout` | Parallel fan-out/fan-in via `make_parallel_group` (3 workers) | No |
| 06 | `subgraph` | Hierarchical graph composition (Supervisor pattern) | No |
| 07 | `intent_routing` | Intent classification + expert routing | No |
| 08 | `state_management` | get_state / update_state / fork / time-travel | No |
| 09 | `all_features` | All 6 advanced features in one demo | No |
| 10 | `send_command` | Dynamic Send fan-out + Command routing override | No |
| 11 | `clay_chatbot` | Multi-turn chatbot UI (Clay + Raylib) | Optional |
| 12 | `rag_agent` | RAG agent with in-memory vector search (CLI) | Required (OpenAI) |
| 13 | `openai_responses` | ReAct via OpenAI `/v1/responses` through SchemaProvider | Required (OpenAI) |
| 14 | `plan_executor` | Plan & Executor: 5-way Send + crash/resume via pending_writes | No |
| 15 | `reflexion` | Self-critique loop until acceptance (Anthropic) | Required (Anthropic) |
| 16 | `tree_of_thoughts` | BFS over LLM thought branches, top-k pruning | Required (Anthropic) |
| 17 | `self_ask` | Follow-up decomposition across multiple hops | Required (Anthropic) |
| 18 | `multi_agent_debate` | Proponent / opponent / judge pattern | Required (Anthropic) |
| 19 | `rewoo` | Reasoning WithOut Observation — plan once, fan out, synthesize | Required (Anthropic) |
| 20 | `mcp_hitl` | MCP + checkpoint HITL (`interrupt_before` tool dispatch, resume after approval) | Required (OpenAI) |
| 21 | `mcp_fanout` | Parallel MCP tool calls via Send fan-out inside one super-step | No |
| 22 | `mcp_stdio` | MCP over stdio transport — subprocess MCP server spawned by the client | Required (OpenAI) |
| 23 | `mcp_multi` | One agent routing tools across two MCP servers (HTTP + stdio) | Required (OpenAI) |
| 24 | `mcp_feedback` | Human-feedback loop — draft answer, operator pushes back, agent revises | Required (OpenAI) |
| 25 | `deep_research` | open_deep_research-style multi-step web research loop (Crawl4AI + Anthropic) | Required (Anthropic) |
| 26 | `postgres_react_hitl` | ReAct + Postgres-backed checkpoint HITL — survives process restart | Required (Anthropic + Postgres) |
| 27 | `async_concurrent_runs` | Hosting many concurrent agent runs on one shared `asio::io_context` | No |
| 28 | `corrective_rag` | Corrective RAG (arXiv:2401.15884) — retrieve → evaluator routes to refine / web / both → generate, all over `/v1/responses` | Required (OpenAI) |
| 29 | `responses_envelope` | Wire-level dump of `/v1/responses`'s `output[]` envelope — debug/pedagogy aid for understanding tool-calling shape before SchemaProvider flattens it | Required (OpenAI) |

Every API-using example above auto-loads `.env` from the cwd or any
parent directory via the bundled `cppdotenv`, so the recipe is just
`echo 'OPENAI_API_KEY=...' > .env && ./example_*`. Process-environment
values still take precedence if both are set.

### Run with a real LLM

```bash
# Set your API key (auto-loaded by every API-using example via cppdotenv)
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

![NeoGraph architecture — core / llm / mcp / util with internal class breakdown](docs/images/architecture.png)

`GraphEngine` is a thin super-step orchestrator that delegates to four
purpose-built classes extracted in the 0.1 refactor:

- **`GraphCompiler`** — pure `JSON → CompiledGraph` parser.
- **`Scheduler`** — signal-dispatch routing plus barrier accumulation.
- **`NodeExecutor`** — retry loop (async-native with timer-based backoff), parallel fan-out via `asio::experimental::make_parallel_group`, Send dispatch.
- **`CheckpointCoordinator`** — save / resume / pending-writes lifecycle
  behind a `(store, thread_id)` façade.

Each class has a dedicated unit-test suite so engine behaviour is
verifiable without spinning up a full run. See
[`docs/reference-en.md` §7b](docs/reference-en.md#7b-engine-internals)
for the full API surface.

### Dependency Isolation

| Link target               | What gets pulled in |
|---------------------------|---------------------|
| `neograph::core`          | `yyjson` (compiled, bundled), `asio` (header-only, standalone) |
| `neograph::core + llm`    | + OpenSSL (`httplib` stays PRIVATE) |
| `neograph::core + mcp`    | + OpenSSL (`httplib` stays PRIVATE) |
| `neograph::util`          | + `moodycamel::ConcurrentQueue` (header-only) |

`httplib` is never exposed to your code. `core` has zero network dependencies.
Taskflow was removed in 3.0 — parallel fan-out now runs on asio's
coroutine primitives (see [Features](#core-engine-neographcore)).

## Concurrency & Async

NeoGraph supports two concurrency models out of the box — pick the
one that fits your hosting pattern:

* **Thread-per-agent (sync)** — `run()` / `run_stream()` / `resume()`
  dispatched onto any executor you already use. Safe up to roughly a
  thousand concurrent agents; ~5 µs engine overhead per call on a
  Release `-O3 -DNDEBUG` build (the super-step loop routes through
  `run_sync(execute_graph_async)` so both entry points share one
  coroutine path). Detailed below.
* **Coroutine-based async** — `run_async()` / `run_stream_async()` /
  `resume_async()` returning `asio::awaitable<RunResult>`. One
  `asio::io_context` hosts thousands of concurrent agents without a
  thread per run; all Provider / MCP / checkpoint I/O points are
  non-blocking `co_await` under the hood. Short intro below; full
  migration guide in [`docs/ASYNC_GUIDE.md`](docs/ASYNC_GUIDE.md).

### Async (Stage 3)

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

3.0 reality: each `engine->run_async()` drives its own internal
io_context to completion, so the three runs above are still serial on
the caller thread (~150 ms for three 50 ms steps). The wiring above is
the call shape we want users locking in today; full inside-the-engine
coroutine-ification is the Stage 4 work that turns the same code into
true single-thread overlap (~50 ms). For burst concurrency *now*,
dispatch each `run()` onto a shared `asio::thread_pool` — that's the
pattern measured in [`benchmarks/concurrent/CONCURRENT.md`](benchmarks/concurrent/CONCURRENT.md)
where N=10,000 finishes in 52 ms. Within a single run, the
`make_parallel_group` fan-out *does* overlap: three parallel-fanout
researchers collapse from 370 ms sequential to 150 ms.

Custom nodes join the async path by overriding `execute_async`
instead of `execute`:

```cpp
class FetchNode : public GraphNode {
  public:
    asio::awaitable<std::vector<ChannelWrite>>
    execute_async(const GraphState& state) override {
        auto ex = co_await asio::this_coro::executor;
        auto res = co_await neograph::async::async_post(ex, /*...*/);
        co_return std::vector<ChannelWrite>{/*...*/};
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

### Sync (thread-per-agent)

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

### Persistent checkpointing with PostgreSQL

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
`libpqxx-dev` (apt) / `libpqxx-devel` (rpm). Set the flag `OFF` to skip
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

Without the env var the 19 PG tests are `GTEST_SKIP`'d so the rest of
the suite stays green on machines without a Postgres handy.

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
| Checkpointing | MemorySaver + Postgres/SQLite/Redis | CheckpointStore (interface) + InMemory + Postgres |
| HITL | interrupt_before/after | interrupt_before/after + NodeInterrupt |
| get_state / update_state | Yes | Yes |
| Fork | Yes | Yes |
| Time travel | get_state_history | get_state_history |
| Subgraphs | CompiledGraph as node | SubgraphNode (JSON inline) |
| Parallel fan-out | Static | `make_parallel_group` (+ opt-in `asio::thread_pool`) |
| Send (dynamic fan-out) | Send() | NodeResult::sends → parallel_group fan-out |
| Command (routing+state) | Command(goto, update) | NodeResult::command |
| Retry policy | RetryPolicy | RetryPolicy + exponential backoff |
| Stream modes | values/updates/messages | EVENTS/TOKENS/VALUES/UPDATES/DEBUG |
| Cross-thread Store | Store (Postgres) | Store (interface) + InMemory |
| Multi-LLM | LangChain required | SchemaProvider built-in (3 vendors) |
| MCP support | None (separate impl) | MCPClient built-in |
| Performance | Python (GIL) | C++20 coroutines + asio |
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
│   ├── core/                   # 13 source files (engine + compiler/scheduler/executor/coordinator split)
│   ├── llm/                    # 3 source files
│   └── mcp/                    # 1 source file
├── schemas/                    # Built-in LLM provider schemas
│   ├── openai.json
│   ├── claude.json
│   └── gemini.json
├── deps/                       # Vendored dependencies
│   ├── yyjson/                 # Compiled C JSON library (yyjson.c + yyjson.h)
│   ├── asio/                   # Standalone asio (header-only, C++20 coroutines)
│   ├── httplib.h               # cpp-httplib (PRIVATE to llm/mcp)
│   ├── concurrentqueue.h       # moodycamel lock-free queue
│   ├── cppdotenv/              # .env loader (example 13)
│   ├── clay.h                  # Clay UI layout
│   └── clay_renderer_raylib.c  # Clay + raylib renderer glue (example 11)
├── benchmarks/                 # NeoGraph vs LangGraph engine-overhead bench
├── examples/                   # 18 runnable examples + Clay chatbot
└── scripts/
    └── embed_schemas.py        # Build-time schema embedding
```

## CMake Targets

| Target | Description | Dependencies |
|--------|-------------|--------------|
| `neograph::core` | Graph engine + types | yyjson (bundled), asio (header-only), Threads |
| `neograph::llm` | LLM providers + Agent | core + OpenSSL (httplib PRIVATE) |
| `neograph::mcp` | MCP client | core + OpenSSL (httplib PRIVATE) |
| `neograph::util` | RequestQueue | core + concurrentqueue |
| `neograph::async` | asio HTTP/SSE helpers | core + OpenSSL |

## Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `NEOGRAPH_BUILD_LLM` | ON | Build LLM provider module |
| `NEOGRAPH_BUILD_MCP` | ON | Build MCP client module |
| `NEOGRAPH_BUILD_UTIL` | ON | Build utility module |
| `NEOGRAPH_BUILD_EXAMPLES` | ON | Build example programs |
| `NEOGRAPH_BUILD_CLAY_EXAMPLE` | OFF | Build Clay+Raylib chatbot (fetches Raylib) |

## Benchmarks

### Engine overhead vs. Python graph/pipeline frameworks

Matched-topology, zero-I/O workloads: graph compiled once, invoked in a
hot loop. Measures what the engine itself costs (dispatch, state
writes, reducer calls) — no LLM, no sleep, no network.

![NeoGraph vs Python frameworks — per-iteration latency and peak RSS](docs/images/bench-engine-overhead.png)

Per-iteration engine overhead (µs, lower is better). All rows
measured 2026-04-22 on the same x86_64 Linux host. NeoGraph built
with Release `-O3 -DNDEBUG` (10-run median); Python rows are 3-run
median through CPython 3.12.3.

| Framework | `seq` (3-node chain) | `par` (fan-out 5 + join) | `seq` vs. NeoGraph |
|-----------|---------------------:|-------------------------:|-------------------:|
| **NeoGraph 3.0** | **5.0 µs** | **11.8 µs** | 1× |
| Haystack 2.28.0 | 144.1 µs | 290.0 µs | 28.8× |
| pydantic-graph 1.85.1 | 235.9 µs | 286.1 µs¹ | 47.2× |
| LangGraph 1.1.9 | 656.7 µs | 2,348.7 µs | 131.3× |
| LlamaIndex Workflow 0.14.21 | 1,780.3 µs | 4,683.5 µs | 356.1× |
| AutoGen GraphFlow 0.7.5 | 3,209.2 µs | 7,292.7 µs | 641.8× |

¹ pydantic-graph is a single-next-node state machine and cannot fan
out; `par` is a serial 6-node emulation.

Whole-process metrics (warm-up + both workloads, 10k seq + 5k par iters):

| | NeoGraph 3.0 | best Python (Haystack) | worst (AutoGen) |
|---|----------|------------------------|-----------------|
| **Total elapsed** | **~0.16 s** | 2.91 s | 68.29 s |
| **Peak RSS** | **4.8 MB** | 80.3 MB | 52.4 MB² |
| **Parallel fan-out executor** | `asio::experimental::make_parallel_group` | single-thread asyncio (GIL) | single-thread asyncio (GIL) |

² AutoGen has a smaller RSS than LlamaIndex but its per-iter cost
is 64× higher — different tradeoff axes. Full matrix in
[`benchmarks/README.md`](benchmarks/README.md).

**Engine overhead disappears under LLM latency.** A 500 ms OpenAI round
trip swamps every engine; the per-iter gap only shows up in non-LLM
nodes (data transforms, routing decisions, pure-compute tool calls) and
in dense agent orchestration. Where it does show up, it shows up big:
on a Raspberry Pi 4 / Jetson Nano / any SBC-class target, a 10–20×
RAM delta is the difference between "fits" and "swap thrash."

Reproduction and methodology: [`benchmarks/README.md`](benchmarks/README.md).

### Burst concurrency (1 CPU / 512 MB sandbox)

What happens under thousands of simultaneous requests? Burst test: N
requests submitted at t=0 to each engine, all-in / all-wait, inside a
Docker cgroup limited to **1 CPU and 512 MB RAM** — roughly a
Raspberry Pi 4 process budget.

![Tail latency — P99 per request](docs/images/bench-concurrent-latency.png)

![Throughput under concurrent load](docs/images/bench-concurrent-throughput.png)

![Peak resident memory](docs/images/bench-concurrent-rss.png)

At **N=10,000 concurrent requests** in asyncio mode (the default
deployment shape for every Python framework):

| Engine | Wall | P99 latency | Peak RSS | Status |
|--------|-----:|------------:|---------:|:-------|
| **NeoGraph 3.0** | **52 ms** | **7 µs** | **5.5 MB** | ✅ 10000 / 0 |
| pydantic-graph | 886 ms | **158 µs** | 42.6 MB | ✅ 10000 / 0 |
| Haystack | 3.1 s | 2.9 s | 130.7 MB | ✅ 10000 / 0 |
| LangGraph | 23.4 s | 23.0 s | 416.2 MB | ✅ 10000 / 0 |
| LlamaIndex | — | — | — | ❌ **OOM killed** |
| AutoGen | — | — | — | ❌ **OOM killed** |

**Two frameworks don't complete** — LlamaIndex Workflow and AutoGen
GraphFlow exhaust the 512 MB cgroup and get OOM-killed before 10k
concurrent coroutines can drain. The remaining Python frameworks
degrade rather than die, but their P99 latency grows linearly with N
because the CPython GIL serializes every coroutine's CPU work. **This
is not a LangGraph-specific pathology** — it shows up in every Python
asyncio runtime.

NeoGraph 3.0 beats every Python asyncio runtime on throughput,
tail latency, and RSS: 7 µs P99 at N=10k, ~76× lower RSS than
LangGraph at the same load, and 3 orders of magnitude ahead of the
GIL-serialized Python curves. Even pydantic-graph — the leanest
Python state-machine — sits at 158 µs P99 and ~8× NeoGraph's RSS.

`multiprocessing.Pool` mode bypasses the GIL across worker processes
but saturates at pool size and pays fork + pickle overhead; full
numbers and the mp-mode story are in
[`benchmarks/concurrent/CONCURRENT.md`](benchmarks/concurrent/CONCURRENT.md).

### Size & cold-start footprint (Plan & Executor demo)

All numbers below were measured on x86_64 Linux (GCC 13) using
`example_plan_executor` — a self-contained Plan & Executor demo that
runs a 5-way Send fan-out, crashes sub-topic #2 on the first run, and
resumes with the failure cleared. No LLM calls, no API keys, no network.

### Binary size (MinSizeRel + static libstdc++ + strip)

| Build configuration | Size |
|---|---|
| **MinSizeRel `-Os`, static libstdc++, `--gc-sections`, stripped** | **1,203 KB (1.2 MB)** |

The MinSizeRel binary's only dynamic dependency is `libc.so.6` —
`libstdc++` and `libgcc_s` are linked in statically. Drop it onto any
Linux host with a matching libc and it runs. 3.0 is ~80 KB larger
than 2.0 because asio's coroutine machinery (steady_timer,
make_parallel_group, use_future) is pulled into the engine path;
Taskflow was header-only and `--gc-sections` stripped most of it
anyway, so its removal doesn't offset the coroutine growth.

### Runtime footprint

| Metric | Value |
|---|---|
| Peak RSS (full Plan & Executor run, crash + resume included) | **2.9 MB** |
| Wall-clock (cold start → both phases complete) | **~720 ms** |
| Dynamic dependencies | `libc.so.6` only |

`example_plan_executor` sleeps 120 ms per Send target to simulate an
LLM call; the 5-way fan-out runs serially on the default
single-threaded super-step loop (5 × 120 ms × 2 phases ≈ wall
time). Call `engine->set_worker_count(N)` after `compile()` to get
the 2.x-style multi-threaded fan-out (cuts this demo's wall time
roughly in half on a 2-core host). Steady-state footprint (RSS) is
unchanged between 2.0 and 3.0.

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

- **1.1 MB static binary** fits a Docker `scratch` image at ~1 MB, fits
  on-board flash of a Pixhawk companion computer, fits comfortably in
  a Jetson Orin boot partition. Python + LangGraph does not.
- **2.9 MB RSS** means you can host **100+ concurrent agent sessions**
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
- [asio](https://think-async.com/Asio/) — Cross-platform C++ networking and coroutine primitives (the 3.0 engine runtime)
- [Clay](https://github.com/nicbarker/clay) — High-performance UI layout library

Previously (2.x): also built on [Taskflow](https://github.com/taskflow/taskflow)
for parallel fan-out. 3.0 replaced that path with
`asio::experimental::make_parallel_group` to unify sync and async
execution on one coroutine runtime.

## License

MIT License. See [LICENSE](LICENSE) for details.

Third-party licenses: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)
