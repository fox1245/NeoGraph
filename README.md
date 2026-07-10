<p align="center">
  <h1 align="center">NeoGraph</h1>
  <p align="center">
    <strong>The C++ graph agent engine — with Python bindings.</strong><br>
    LangGraph-level capabilities · 5&nbsp;µs engine overhead · one static binary that fits a Raspberry&nbsp;Pi.
  </p>
</p>

<p align="center">
  <a href="https://pypi.org/project/neograph-engine/"><img alt="PyPI" src="https://img.shields.io/pypi/v/neograph-engine?label=pip%20install%20neograph-engine&color=blue"></a>
  <a href="https://pypi.org/project/neograph-engine/"><img alt="Python versions" src="https://img.shields.io/pypi/pyversions/neograph-engine"></a>
  <a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-MIT-green.svg"></a>
</p>

<p align="center">
  <a href="#quick-start">Quick Start</a> &middot;
  <a href="#use-from-a-cmake-project">CMake</a> &middot;
  <a href="#python">Python</a> &middot;
  <a href="docs/concepts.md">Concepts</a> &middot;
  <a href="examples/README.md">Examples</a> &middot;
  <a href="docs/troubleshooting.md">Troubleshooting</a> &middot;
  <a href="docs/reference-en.md">API Reference</a> &middot;
  <a href="https://fox1245.github.io/NeoGraph/">Doxygen</a> &middot;
  <a href="#vs-langgraph">vs LangGraph</a>
</p>

---

<p align="center">
  <a href="docs/videos/neograph-promo.mp4">
    <img src="docs/images/neograph-promo.gif" alt="NeoGraph promo — 5µs engine overhead, 5.5MB RSS at 10K concurrent, 1.2MB static binary, fits Raspberry Pi" width="900">
  </a>
</p>

## What is NeoGraph?

NeoGraph is a **C++17/20 graph-based agent orchestration engine** that brings
LangGraph-level capabilities to C++. Define agent workflows as JSON, execute
them with parallel fan-out, checkpoint state for time-travel debugging and
human-in-the-loop, and plug in any LLM provider — all without Python.

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

The agent above is really just JSON the engine executes — swap the JSON, get a
different agent (see [`docs/concepts.md`](docs/concepts.md)):

```json
{
  "channels": { "messages": {"reducer": "append"}, "__route__": {"reducer": "overwrite"} },
  "nodes": {
    "planner":    {"type": "llm_call"},
    "researcher": {"type": "tool_dispatch"},
    "classifier": {"type": "intent_classifier", "routes": ["deep_dive", "summarize"]}
  },
  "edges": [
    {"from": "__start__", "to": "planner"},
    {"from": "planner", "condition": "has_tool_calls",
     "routes": {"true": "researcher", "false": "classifier"}},
    {"from": "researcher", "to": "planner"},
    {"from": "classifier", "condition": "route_channel",
     "routes": {"deep_dive": "__end__", "summarize": "__end__"}}
  ]
}
```

**NeoGraph is the only graph agent engine for C++.** If you're building agents
for robotics, embedded systems, games, high-frequency trading, or anywhere
Python isn't an option — this is it.

## The four axes

Each row is one command away — no setup, no API key except the live-LLM variants.

|   | Axis | Measured | Detail |
|---|---|---|---|
| ⚡ | **Performance** | 5 µs engine overhead · 10 K concurrent in 5.5 MB · p99 7 µs @ 10 K (1 CPU sandbox) | [performance deep-dive](docs/performance-deep-dive.md) |
| 🧬 | **Self-evolution** | LLM judge → `graph_def` hot-swap · 5 customer → 3 emergent topology clusters | [self_evolving_chatbot](examples/cookbook/self_evolving_chatbot/) |
| 🔌 | **Embedded-ready** | 1.2 MB stripped static binary · `libc.so.6` only · runs on RPi Zero 2W | [embedded / robotics](docs/performance-deep-dive.md#what-the-numbers-mean-for-embedded--robotics) |
| 🪶 | **Lightweight** | 2 direct wheel deps · 1 K-customer multi-tenant → 29 MB · t2.micro-friendly | [multi_tenant_chatbot](examples/cookbook/multi_tenant_chatbot/) |

### Benchmarks

Matched-topology, zero-I/O engine overhead — just node dispatch + state writes +
reducer calls (µs/iter, lower is better):

| Framework | `seq` (3-node) | `par` (fan-out 5) | vs. NeoGraph |
|---|--:|--:|--:|
| **NeoGraph master** | **5.0 µs** | **11.8 µs** | 1× |
| Haystack 2.28 | 144 µs | 290 µs | 29× |
| pydantic-graph 1.85 | 236 µs | 286 µs | 47× |
| LangGraph 1.1.9 | 657 µs | 2,349 µs | 131× |
| LlamaIndex 0.14 | 1,780 µs | 4,684 µs | 356× |
| AutoGen 0.7.5 | 3,209 µs | 7,293 µs | 642× |

At N=10,000 concurrent (1 CPU / 512 MB sandbox): NeoGraph 52 ms / 7 µs p99 /
5.5 MB · LangGraph 23.4 s / 416 MB · LlamaIndex & AutoGen OOM-killed.
Full matrix + methodology: [`docs/performance-deep-dive.md`](docs/performance-deep-dive.md)
· [`benchmarks/README.md`](benchmarks/README.md).

## Quick Start

**Requirements** — C++20 compiler (GCC 13.3 core-green; GCC 14.2+ / Clang 18+ /
MSVC 2022 for everything), CMake 3.16+, Python 3 (build-time codegen). With
default options the configure step also requires the OpenSSL, SQLite3, libpq,
and libcurl **development** packages (runtime `.so`s alone won't satisfy
`find_package`):

```bash
# Ubuntu / Debian
sudo apt install libssl-dev libsqlite3-dev libpq-dev libcurl4-openssl-dev
# macOS (SQLite ships with the system)
brew install openssl libpq curl
```

Don't need Postgres / SQLite checkpoints or the HTTP/2 backend? Skip the
packages and configure with `-DNEOGRAPH_BUILD_POSTGRES=OFF
-DNEOGRAPH_BUILD_SQLITE=OFF -DNEOGRAPH_USE_LIBCURL=OFF` instead.

**Platforms** — Linux x86_64 **GA** (reference, 429/429 ctest, sanitizer-clean);
macOS arm64, Linux ARM64, Windows MSVC 2022 **beta**. Per-platform rationale in
[`CHANGELOG.md`](CHANGELOG.md).

```bash
git clone https://github.com/fox1245/NeoGraph.git
cd NeoGraph
cmake -S . -B build
cmake --build build -j$(nproc)

# Run an example — no API key needed:
./build/example_custom_graph      # mock ReAct agent
./build/example_parallel_fanout   # parallel fan-out/fan-in
./build/example_send_command      # dynamic Send + Command routing
```

Run against a real LLM — every API-using example auto-loads `.env` from the cwd
(bundled `cppdotenv`):

```bash
echo "OPENAI_API_KEY=sk-..." > .env
./build/example_react_agent
```

## Use from a CMake project

`pip install` is Python-only (no C++ headers). For C++, `FetchContent` behaves
like `pip install` for CMake:

```cmake
include(FetchContent)
FetchContent_Declare(NeoGraph
    GIT_REPOSITORY https://github.com/fox1245/NeoGraph.git
    GIT_TAG        master)
# Optional: trim heavy components you don't need.
set(NEOGRAPH_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(NEOGRAPH_BUILD_PYBIND   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(NeoGraph)

add_executable(my_agent main.cpp)
target_link_libraries(my_agent PRIVATE neograph::core neograph::llm neograph::a2a)
```

That's the whole integration. New to it? The
[**5 common first-30-minutes traps**](docs/troubleshooting.md) (channel accessor
shape, the `neograph::graph::` sub-namespace, the `<httplib.h>` OpenSSL macro,
the GCC 13 coroutine ICE, …) will save you a debugging session. Full build
options and CMake targets: [`docs/reference-en.md`](docs/reference-en.md).

## Python

The same C++ engine, `pip`-installable and driven from a notebook, Gradio, or
FastAPI service:

```bash
pip install neograph-engine
```

```python
import neograph_engine as ng

definition = {
    "name": "demo",
    "channels": {"messages": {"reducer": "append"}},
    "nodes":    {"llm": {"type": "llm_call"}},
    "edges":    [{"from": ng.START_NODE, "to": "llm"},
                 {"from": "llm", "to": ng.END_NODE}],
}
engine = ng.GraphEngine.compile(definition, ng.NodeContext())
result = engine.run(ng.RunConfig(thread_id="t1", input={"messages": [...]}))
```

20 wheels + sdist per release (Linux x86_64/aarch64, macOS arm64, Windows x64 ·
Python 3.9–3.13). Full guide — ReAct with a real LLM, async, custom reducers,
the LangGraph-divergence list, observability, Docker-free deploy:
[`docs/python-binding.md`](docs/python-binding.md).

## Features

**Core engine (`neograph::core`)** — JSON-defined graphs (no recompile to change
a workflow) · Pregel super-step execution with cycles · parallel fan-out/fan-in ·
`Send` (dynamic fan-out) + `Command` (routing+state override) · checkpointing +
HITL (`interrupt_before/after`, `resume()`, `NodeInterrupt`) · `get_state` /
`update_state` / `fork` / time-travel · retry policies · stream modes · subgraphs ·
intent routing · cross-thread `Store` · custom nodes via `NodeFactory` ·
async-native (`run_async` / `run_stream_async`) · cooperative `CancelToken` ·
history compaction · per-node cache · `NodeFactory::export_schema()` (drives the
version-locked visual editor). Built-in **OpenInference tracer**, no extra link.

**LLM providers (`neograph::llm`)** — `OpenAIProvider` (OpenAI/Groq/Together/
vLLM/Ollama — any OpenAI-compatible API) · `SchemaProvider` (Claude, Gemini, or
any custom vendor via JSON schema) · ReAct `Agent` loop with streaming.

**Integrations** — MCP client (`neograph::mcp`, HTTP + stdio) · Agent-to-Agent
(`neograph::a2a`, server + client + caller node) · Agent Client Protocol
(`neograph::acp`, editor-driven) · gRPC service (`neograph::grpc`, opt-in) ·
async HTTP/HTTPS/WS + SSE (`neograph::async`).

**Durable state** — `PostgresCheckpointStore`, `SqliteCheckpointStore`, and
`InMemoryCheckpointStore` behind one `CheckpointStore` interface (all
Python-bound). Lock-free `RequestQueue` + `AsyncTool` in `neograph::util`.

Full capability list and the 55+ runnable examples:
[`examples/README.md`](examples/README.md).

## Architecture

`GraphEngine` is a thin super-step orchestrator delegating to four
purpose-built, independently unit-tested classes:

- **`GraphCompiler`** — pure `JSON → CompiledGraph` parser.
- **`Scheduler`** — signal-dispatch routing + barrier accumulation.
- **`NodeExecutor`** — retry loop, parallel fan-out (`asio::make_parallel_group`), `Send` dispatch.
- **`CheckpointCoordinator`** — save / resume / pending-writes behind a `(store, thread_id)` façade.

`neograph::core` has zero network dependencies (`yyjson` + header-only `asio`);
`httplib` stays PRIVATE to `llm`/`mcp` and is never exposed to your code. Two
concurrency models ship out of the box — thread-per-agent (sync) and
coroutine-async (thousands of agents on one `asio::io_context`). Details:
[`docs/reference-en.md` §7b](docs/reference-en.md#7b-engine-internals) ·
[`docs/concurrency.md`](docs/concurrency.md) · [`docs/ASYNC_GUIDE.md`](docs/ASYNC_GUIDE.md).

## vs LangGraph

| | LangGraph (Python) | NeoGraph (C++) |
|---|---|---|
| Engine | StateGraph | GraphEngine |
| Checkpointing / HITL / fork / time-travel | Yes | Yes (+ `NodeInterrupt`) |
| Parallel fan-out | Static | `make_parallel_group` (+ opt-in `asio::thread_pool`) |
| Send / Command | Yes | `NodeResult::sends` / `::command` |
| Multi-LLM | LangChain required | `SchemaProvider` built-in (3 vendors) |
| MCP | Separate impl | Built-in |
| Runtime / memory | Python GIL · ~300 MB+ | C++20 coroutines + asio · ~10 MB |
| Edge / embedded | Not possible | Raspberry Pi, Jetson, IoT |

Same multi-tenant shape LangGraph needs a *process per customer* for (StateGraph
is a Python object), NeoGraph serves from one process as graph-as-JSON — the
[multi-tenant](examples/cookbook/multi_tenant_chatbot/) and
[self-evolving](examples/cookbook/self_evolving_chatbot/) cookbooks show why.

## Acknowledgments

Inspired by [LangGraph](https://github.com/langchain-ai/langgraph),
[agent.cpp](https://github.com/mozilla-ai/agent.cpp),
[asio](https://think-async.com/Asio/) (the 3.0 engine runtime), and
[Clay](https://github.com/nicbarker/clay).

## License

MIT — see [LICENSE](LICENSE). Third-party: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
