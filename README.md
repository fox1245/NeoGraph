<p align="center">
  <h1 align="center">NeoGraph</h1>
  <p align="center">
    <strong>A C++ Graph Agent Engine — with Python bindings</strong><br>
    Microsecond tail latency under 10k concurrent requests on 512 MB.<br>
    LangGraph's semantics, without the Python runtime tax — and now reachable from Python too.
  </p>
</p>

<p align="center">
  <a href="https://pypi.org/project/neograph-engine/"><img alt="PyPI" src="https://img.shields.io/pypi/v/neograph-engine?label=pip%20install%20neograph-engine&color=blue"></a>
  <a href="https://pypi.org/project/neograph-engine/"><img alt="Python versions" src="https://img.shields.io/pypi/pyversions/neograph-engine"></a>
  <a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-MIT-green.svg"></a>
</p>

<p align="center">
  <a href="docs/concepts.md">Concepts</a> &middot;
  <a href="#quick-start">Quick Start</a> &middot;
  <a href="#python-binding">Python Binding</a> &middot;
  <a href="examples/README.md">C++ Examples</a> &middot;
  <a href="bindings/python/examples/README.md">Python Examples</a> &middot;
  <a href="examples/cookbook/">Cookbooks</a> &middot;
  <a href="docs/troubleshooting.md">Troubleshooting</a> &middot;
  <a href="docs/reference-en.md">API Reference</a> &middot;
  <a href="https://fox1245.github.io/NeoGraph/">Doxygen</a> &middot;
  <a href="#comparison-with-langgraph">vs LangGraph</a> &middot;
  <a href="#benchmarks">Benchmarks</a>
</p>

---

<p align="center">
  <a href="docs/videos/neograph-promo.mp4">
    <img src="docs/images/neograph-promo.gif" alt="NeoGraph 16-second promo — 5µs engine overhead, 5.5MB RSS at 10K concurrent, 1.2MB static binary, fits Raspberry Pi" width="900">
  </a>
  <br><sub><i>16s · the numbers · click for the 1080p MP4 (662 KB)</i></sub>
</p>

<p align="center">
  <a href="docs/videos/neograph-promo-v2.mp4">
    <img src="docs/images/neograph-promo-v2.gif" alt="NeoGraph 15-second promo v2 — graph engine in motion, 10-line Python, every LangGraph capability built in" width="900">
  </a>
  <br><sub><i>15s · what you actually ship · click for the 1080p MP4 (740 KB)</i></sub>
</p>

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

### Engine overhead vs. leading frameworks

Per-invocation overhead on identically-shaped graphs, no I/O / no LLM —
just node dispatch + state writes + reducer calls. Lower is better.
Reproduced 2026-04-29 against NeoGraph v0.2.3 (g++ 13 Release `-O3
-DNDEBUG`); Python framework rows from the 2026-04-22 reference run,
re-validated within ±10 % at the same date.

| Framework | `seq` (3-node chain) | `par` (fan-out 5 + join, worker=1 fast path)¹ | Slowdown vs. NeoGraph |
|-----------|---------------------:|---------------------------------------------:|-------------------:|
| **NeoGraph v0.2.3** (this repo) | **5.0 µs**  | **14.4 µs** | 1× |
| Haystack 2.28 | 140 µs   | 278 µs   | **28× / 19×** |
| pydantic-graph 1.87 | 227 µs | 280 µs²  | **45× / 19×**² |
| LangGraph 1.1.10 | 643 µs  | 2,262 µs | **128× / 157×** |
| LlamaIndex Workflow 0.14 | 1,565 µs | 4,374 µs | **313× / 304×** |
| AutoGen GraphFlow 0.7.5 | 3,127 µs | 7,281 µs | **625× / 505×** |

¹ NeoGraph's `par` row uses `engine->set_worker_count(1)` to compare
the scheduling cost, not the thread-pool spin-up cost. With the default
(hw\_concurrency) the engine pays \~280 µs of pool coordination — same
total as Haystack but parallelizes any actual node work, which is the
real LLM-workload payoff.
² pydantic-graph cannot fan out; emulated as a 6-node chain.

This is the cost of **one engine round-trip**. Real LLM graphs spend
most of their time in network I/O, but every super-step pays this
once — at 100k requests/day a 600 µs framework sheds an hour of CPU
that NeoGraph spends in 5 seconds. Reproducible end-to-end:
[`benchmarks/README.md`](benchmarks/README.md).

**NeoGraph is the only graph agent engine for C++.** If you're building agents in robotics, embedded systems, games, high-frequency trading, or anywhere Python isn't an option — this is it.

## Using NeoGraph from your CMake project

The `pip install` route is Python-only — the wheel doesn't ship C++
headers. For a C++ project, the simplest path is `FetchContent`,
which behaves like `pip install` for CMake:

```cmake
include(FetchContent)
FetchContent_Declare(
    NeoGraph
    GIT_REPOSITORY https://github.com/fox1245/NeoGraph.git
    GIT_TAG        v0.2.3
)
# Optional: turn off heavy components you don't need.
set(NEOGRAPH_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(NEOGRAPH_BUILD_PYBIND   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(NeoGraph)

add_executable(my_agent main.cpp)
target_link_libraries(my_agent PRIVATE
    neograph::core neograph::llm neograph::a2a)
```

That's the entire integration. See the [AI National Assembly
cookbook](examples/cookbook/ai-assembly/) for a 600-line demo built
this way (4 personas, A2A multi-process, OpenAI-backed) — including
a friction journal of what a fresh user trips over.

### A minimal LLM-only chatbot (no tools, no streaming)

The shortest C++ that runs a real OpenAI multi-turn chatbot —
useful as a template since the wider examples lean on `create_react_graph`
+ tools and obscure how the bare wiring looks:

```cpp
#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>

using namespace neograph::graph;        // GraphEngine, NodeContext, RunConfig,
                                         // InMemoryCheckpointStore live here.
                                         // (RunConfig stays under graph::; the
                                         //  README quickstack uses it directly.)

int main() {
    // OpenAIProvider exposes two factories:
    //   * `create(Config)`        → unique_ptr<OpenAIProvider> (transferable)
    //   * `create_shared(Config)` → shared_ptr<Provider>       (copyable, the
    //                               natural fit for NodeContext::provider
    //                               and for sharing across multiple nodes
    //                               or A2A servers)
    // For a chatbot the shared-ptr peer is what you want — drop straight
    // into NodeContext, no std::move dance.
    neograph::llm::OpenAIProvider::Config cfg;
    cfg.api_key       = std::getenv("OPENAI_API_KEY");
    cfg.default_model = "gpt-4o-mini";
    auto provider = neograph::llm::OpenAIProvider::create_shared(cfg);

    NodeContext ctx;
    ctx.provider     = provider;
    ctx.model        = "gpt-4o-mini";
    ctx.instructions = "Reply in one short sentence.";

    neograph::json definition = {
        {"name", "chatbot"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes",    {{"llm",       {{"type", "llm_call"}}}}},
        {"edges", neograph::json::array({
            {{"from", "__start__"}, {"to", "llm"}},
            {{"from", "llm"},       {"to", "__end__"}}
        })}
    };

    // C++ compile() takes (definition, ctx, store) directly — Python's
    // GraphEngine.compile takes the same trailing arg as a keyword
    // (or use engine.set_checkpoint_store afterwards; both equivalent).
    auto store  = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(definition, ctx, store);

    for (std::string line; std::getline(std::cin, line); ) {
        RunConfig cfg;
        cfg.thread_id        = "session-1";
        cfg.input            = {{"messages", neograph::json::array({
            {{"role", "user"}, {"content", line}}
        })}};
        cfg.resume_if_exists = true;     // multi-turn memory: load prior
                                          // checkpoint, append new turn

        auto r   = engine->run(cfg);
        auto msgs = r.output["channels"]["messages"]["value"];
        std::cout << "Bot: " << msgs.back()["content"].get<std::string>() << "\n";
    }
}
```

Four small things that are easy to miss:

  - **`neograph::graph::` sub-namespace** — `GraphEngine`, `RunConfig`,
    `NodeContext`, `InMemoryCheckpointStore`, `GraphState` all live
    under `neograph::graph::`. `using namespace neograph::graph` (or a
    handful of `using` declarations) keeps the call sites flat.
    `neograph::llm::` and `neograph::a2a::` stay separate on purpose
    so consumers can pick which sub-libraries they link against.
  - **Two factories on `OpenAIProvider`** —
    `create(Config)` → `unique_ptr<OpenAIProvider>` (transferable
    ownership), `create_shared(Config)` → `shared_ptr<Provider>`
    (copyable; drops straight into `NodeContext::provider`). For a
    chatbot or any multi-node graph the `shared_ptr` peer is the
    intended path; the unique flavour is for callers that want
    short-lived ownership before transferring elsewhere via
    `std::move`.
  - **`neograph::json` is a yyjson-backed nlohmann subset** —
    `json::array(...)`, `j["k"]`, `j.value(k, default)`, `j.contains(k)`
    work like nlohmann; element-wise iterators and `.front()` / `.back()`
    on objects do **not**. The full surface map is in
    [include/neograph/json.h](include/neograph/json.h)'s top docstring.
  - **`<cppdotenv/dotenv.hpp>` for `OPENAI_API_KEY` loading** is bundled
    at `deps/cppdotenv/dotenv.hpp`. The in-tree examples reach it via
    `target_include_directories(... PRIVATE ${CMAKE_SOURCE_DIR}/deps)`;
    consumers using `FetchContent` can add
    `target_include_directories(my_agent PRIVATE
    ${neograph_SOURCE_DIR}/deps)` and `#include <cppdotenv/dotenv.hpp>`.
    It's a header-only single file; not part of the public install.

## Python Binding

NeoGraph also ships as a `pip`-installable Python package, so the same
C++ engine can drive a LangGraph-style workflow from a Jupyter
notebook, a Gradio app, or a FastAPI service:

```bash
pip install neograph-engine
```

### Five-second demo (no API key)

The shortest thing that proves the install worked — one decorator-defined
node, run it, read the output:

```python
import neograph_engine as ng

@ng.node("greet")
def greet(state):
    return [ng.ChannelWrite("messages",
        [{"role": "assistant", "content": f"Hello, {state.get('name')}!"}])]

definition = {
    "name": "demo",
    "channels": {"name":     {"reducer": "overwrite"},
                 "messages": {"reducer": "append"}},
    "nodes":    {"greet": {"type": "greet"}},
    "edges":    [{"from": ng.START_NODE, "to": "greet"},
                 {"from": "greet",       "to": ng.END_NODE}],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())
result = engine.run(ng.RunConfig(thread_id="t1", input={"name": "NeoGraph"}))

print(result.output["channels"]["messages"]["value"])
# [{'role': 'assistant', 'content': 'Hello, NeoGraph!'}]
```

### ReAct agent with a real LLM

```python
import neograph_engine as ng
from neograph_engine.llm import OpenAIProvider

class CalcTool(ng.Tool):
    def get_name(self):       return "calc"
    def get_definition(self): return ng.ChatTool(name="calc", description="multiply by 2",
        parameters={"type":"object","properties":{"x":{"type":"number"}}})
    def execute(self, args):  return str(args["x"] * 2)

ctx = ng.NodeContext(
    provider=OpenAIProvider(api_key="sk-..."),
    tools=[CalcTool()],
    instructions="Use `calc` for arithmetic.",
)

definition = {
    "name": "react",
    "channels": {"messages": {"reducer": "append"}},
    "nodes":    {"llm": {"type": "llm_call"}, "dispatch": {"type": "tool_dispatch"}},
    "edges":    [{"from": ng.START_NODE, "to": "llm"}, {"from": "dispatch", "to": "llm"}],
    "conditional_edges": [{"from": "llm", "condition": "has_tool_calls",
                           "routes": {"true": "dispatch", "false": ng.END_NODE}}],
}
engine = ng.GraphEngine.compile(definition, ctx)
result = engine.run(ng.RunConfig(thread_id="t1",
    input={"messages": [{"role": "user", "content": "What is 21 * 2?"}]},
    max_steps=10))
```

### Reading the output

`engine.run(...)` returns a `RunResult` with these fields:

| Field | Type | Meaning |
|---|---|---|
| `output` | `dict` | Final state — `{"channels": {...}, "global_version": int}`. Use `output["channels"][name]["value"]` to read a channel. |
| `interrupted` | `bool` | `True` if the run paused at an `interrupt_before` / `interrupt_after` / `NodeInterrupt`. |
| `interrupt_node` | `str` | Name of the node that triggered the interrupt (when `interrupted`). |
| `interrupt_value` | `dict` | Diagnostic payload — `{"reason": ...}` or `{"message": ...}`. |
| `checkpoint_id` | `str` | ID of the latest checkpoint saved during the run. Pass to `engine.resume_async(checkpoint_id=...)` to continue. |
| `execution_trace` | `list[str]` | Node names in the order they executed — useful for debugging routing. |

`RunConfig` mirrors the LangGraph `RunnableConfig` idea:

| Field | Default | Meaning |
|---|---|---|
| `thread_id` | required | Conversation / session identifier — keeps checkpoint streams separate. |
| `input` | `{}` | Initial channel values — keys must match the graph's `channels` definition. |
| `max_steps` | 25 | Super-step ceiling; ReAct loops typically need 10+. |
| `stream_mode` | `StreamMode.OFF` | Bitmask: `EVENTS \| TOKENS \| DEBUG \| VALUES \| UPDATES \| ALL`. Only consulted by `run_stream` / `run_stream_async`. |
| `resume_if_exists` | `False` | When `True` and a checkpoint store is configured, the run loads the latest checkpoint for `thread_id` (if any) and applies `input` on top via the channel reducers — multi-turn chat without manually threading prior state through `input`. Default keeps fresh-start semantics for back-compat; for HITL resume from an interrupted run, use `engine.resume_async()` instead. |

### Built-in reducers

Channels need a reducer — how new writes combine with existing values.
Two built-ins ship today:

| Reducer | Behavior | Typical use |
|---|---|---|
| `"overwrite"` | New value replaces old. | Single-value channels: `name`, `current_question`, intermediate scratch. |
| `"append"` | New list concatenated to existing list. | Conversation history, intermediate results, anything you want to accumulate across nodes. |

Custom reducers register from Python (since v0.1.9):

```python
ng.ReducerRegistry.register_reducer("sum",
    lambda current, incoming: (current or 0) + incoming)

# Now `"reducer": "sum"` works in your channel definitions.
```

Same pattern for conditional routing — `ng.ConditionRegistry.register_condition("name", fn)`
where `fn(state) -> str` returns one of the route keys.

### What's covered by the binding

- **Engine surface** — `GraphEngine.compile / run / run_stream / run_async / run_stream_async / resume_async / get_state / update_state / fork`, `RunConfig`, `RunResult`, `set_worker_count`, `set_checkpoint_store`, `set_node_cache_enabled`.
- **Custom Python nodes** — subclass `neograph_engine.GraphNode`, register via `NodeFactory.register_type` or the `@neograph_engine.node` decorator. Engine dispatches under proper GIL handling, including from fan-out worker threads.
- **Custom Python tools** — subclass `neograph_engine.Tool`, pass into `NodeContext(tools=[...])`. Engine takes ownership at compile time.
- **Async** — every `*_async` binding returns an `asyncio.Future` bound to the calling thread's running loop. Stream callbacks are hopped to the loop thread via `loop.call_soon_threadsafe` so callbacks run where asyncio expects.
- **Checkpoints** — `InMemoryCheckpointStore` always; `PostgresCheckpointStore` when the binding is built from source with `-DNEOGRAPH_BUILD_POSTGRES=ON` (libpq bundling for the PyPI wheel is pending).
- **OpenAI Responses over WebSocket** — `SchemaProvider(schema="openai_responses", use_websocket=True)`.

Wheels: Linux x86_64 (manylinux_2_34), Linux aarch64 (manylinux_2_34),
macOS arm64 (14+), Windows x64 (MSVC), for Python 3.9 → 3.13. **20 wheels
+ sdist per release** via cibuildwheel.

See [`bindings/python/examples/`](bindings/python/examples/) for the
full example index — minimal graph, ReAct, HITL, intent routing, async,
multi-agent debate, JSON graph round-trip, and a Gradio chat with a
deep-research subgraph (Crawl4AI + Postgres optional).

### Differences from LangGraph (Python binding)

The pitch is "LangGraph for C++", but a few semantics diverge from
LangGraph Python — surfaced here so you don't hit them mid-port:

- **Multi-turn `thread_id` is opt-in** — `engine.run(cfg)` with the
  same `thread_id` does **not** auto-load the previous turn's
  checkpoint by default; every run starts fresh from `cfg.input`.
  Set `cfg.resume_if_exists = True` for the LangGraph-style "load
  latest, apply input on top" behaviour. Default is `False` so
  callers that already thread state through `input` themselves are
  unaffected. See the `RunConfig` table above.
- **`update_state` accepts dict OR list of `ChannelWrite`** —
  `update_state(thread_id, channel_writes, as_node='')` takes
  either of two shapes for `channel_writes`:
  - dict: `{"messages": [...]}` — the directly-keyed form, closest
    to LangGraph's `values={...}` (kwarg name differs).
  - list: `[ChannelWrite("messages", [...]), ...]` — symmetric with
    what every node body emits.

  Duplicate channels in the list form are last-write-wins; for
  multi-write-per-channel on an APPEND reducer (e.g. appending two
  messages in one call), bundle the values into the value list:
  `{"messages": [m1, m2]}`. Other types raise `TypeError` instead
  of silently no-op'ing (a pre-v0.3.2 trap closed by item #5).
- **`get_state(thread_id)` returns a nested dict — `get_state_view`
  is the flat helper** — `state["channels"]["messages"]["value"]`
  is the canonical raw shape (stable across versions). For
  ergonomic dot-access, use
  `view = engine.get_state_view(thread_id)` and read `view.messages`,
  `view.scratch`, etc. directly. `view.raw` exposes the unflattened
  dict for callers needing version / metadata. Subclass `StateView`
  with declared fields (Pydantic v2) for typed access:
  `class ChatState(ng.StateView): messages: list[dict] = []` then
  `engine.get_state_view(thread_id, model=ChatState)`.
- **Python `Provider` subclasses bind only `complete` (sync)** —
  `Provider.complete_async` is not bound on Python user-defined
  Provider subclasses, so a custom Python Provider always serves
  through the sync entry. For async-native provider integrations
  (HTTP/2 multiplexing, true overlap with other coroutines), stay
  in C++ and subclass `neograph::llm::Provider` there.
- **One-line token emit** — `from neograph_engine.streaming import
  emit_token`, then `emit_token(cb, self._name, token)` inside a
  streaming node. Replaces the 4-line `GraphEvent` construction
  ritual.
- **One node method** — `def run(self, input)` is the canonical
  override as of **v0.4.0**. Read state from `input.state`, the live
  cancel handle from `input.ctx.cancel_token`, the streaming sink
  (or `None`) from `input.stream_cb`. Return a `list[ChannelWrite]`,
  `list[Send]`, a `Command`, or a `NodeResult`. The legacy 8-virtual
  chain (`execute`, `execute_async`, `execute_full`,
  `execute_full_async`, `execute_stream`, `execute_stream_async`,
  `execute_full_stream`, `execute_full_stream_async`) is
  `[[deprecated]]` in v0.4.x and **removed in v1.0.0** — migrate now
  to silence the warnings.
- **Two Python deps, full stop** — `pip install neograph-engine`
  pulls `certifi` and `pydantic>=2.0` and that's the entire runtime
  dependency tree. The graph engine, schedulers, checkpoint stores,
  HTTP/WebSocket clients, MCP/A2A/ACP transports, OpenAI-compatible
  provider, and Postgres/SQLite checkpoint backends are all native
  C++ baked into the wheel.
  Compare LangGraph's transitive runtime: `langgraph` →
  `langchain-core` → `langchain` → `langchain-community` (each a
  fast-moving package), plus per-integration packages (`langchain-openai`,
  `langchain-anthropic`, `langchain-postgres`, `langchain-chroma`, …).
  This is why a working LangGraph script breaks 6 months later —
  Pydantic v1→v2 broke the world in 2024, and import paths drift across
  every minor release (`from langchain.chat_models import ChatOpenAI`
  → `from langchain_openai import ChatOpenAI` →
  `from langchain_community.chat_models import ChatOpenAI`, depending
  on which year you read the docs).
  NeoGraph's Python surface is a thin pybind11 layer over a frozen
  C++ ABI under semantic-versioning. **Code you write today against
  v0.4.0 will compile against v1.x** — the deprecation window is the
  *only* mechanism for breaking changes, and you get a `[[deprecated]]`
  warning at compile time before anything moves under you.
- **No Docker required for deployment** — a direct consequence of
  the single-dep tree above. Production LangChain deployments
  effectively *require* Docker + a fully-pinned `requirements.txt`
  (or `poetry.lock` / `uv.lock`); without it, a transitive package's
  silent minor bump on the next deploy can take the server down at
  runtime. NeoGraph's wheel ships its full native runtime baked in,
  so:

  - `pip install neograph-engine==0.4.0` on bare metal / VPS / a
    serverless function works — the host's other Python packages
    can't reach into NeoGraph's C++ engine.
  - Container images can be **alpine + musl + ~20 MB** (engine .so +
    Python interpreter + 2 deps), or static-linked C++ binary at
    **~1.2 MB** with `libc.so.6` as the only dynamic dep.
  - Cold start on serverless (Lambda, Cloud Run) is ms-class, not
    seconds — there's no LangChain import graph to walk.
  - Lock-file maintenance burden is near-zero. `pydantic>=2.0` is
    the only constraint that could ever drift, and you'd see it at
    install time, not 3 AM in production.

## The agent runtime that fits in L3 cache

NeoGraph's hot code path is small enough that N concurrent agents share
one L3-resident working set. We measured this with Valgrind cachegrind
on a Ryzen 7 5800X (Zen 3: 32 KB L1i/d 8-way, **32 MB L3 16-way**),
sweeping N = 1 → 10,000 concurrent requests through
`benchmarks/concurrent/bench_concurrent_neograph`:

| N | I refs | **L3 instruction misses** | L3i miss rate | Native p50 |
|---:|---:|---:|---:|---:|
| 1 | 5.3 M | **4,313** | 0.08% | 17 µs |
| 10 | 5.9 M | **4,304** | 0.07% | 16 µs |
| 100 | 11.8 M | **4,320** | 0.04% | 6 µs |
| 1,000 | 69.7 M | **4,327** | 0.01% | 6 µs |
| 10,000 | **648 M** | **4,329** | **0.00%** | **5 µs** |

**L3 instruction misses stay flat at ~4,320** across four orders of
magnitude of N. The unique hot code working set is roughly
`4,330 × 64 B = 277 KB` — **0.85 % of the 32 MB L3**. At N = 10,000
we processed **648 million instructions** and only **4,329 of them
reached DRAM** (≈ 1 miss per 150,000 instructions).

Native per-request latency drops from 17 µs (cold) to 5 µs (warm) as N
grows — the 3.4× improvement is pure I-cache warming. Throughput at
N = 10,000 is ~1.1 M req/s on the single thread pool, with 5.2 MB
peak RSS (≈ 100 B / agent marginal cost).

**Why this matters:** DRAM access on Zen 3 is ~250 cycles vs ~46 for
an L3 hit — roughly 5.5× slower per access. If NeoGraph's working set
had overflowed L3 (as Python interpreters + dict-heavy state typically
do), the same N = 10,000 sweep would have paid **+420 to +840 ms in
memory stalls** instead of the measured **9 ms total wall time** —
47–94× slower depending on how much of the miss chain reaches DRAM.
The whole L3 stays available for *your* workload (conversation history,
embeddings, tool responses): the engine itself is a rounding error.

_Reproduce:_
```bash
g++ -std=c++20 -O2 -DNDEBUG -Iinclude -Ideps -Ideps/yyjson -Ideps/asio/include \
    -DASIO_STANDALONE benchmarks/concurrent/bench_concurrent_neograph.cpp \
    build-release/libneograph_core.a build-release/libyyjson.a -pthread -o bench_ng

valgrind --tool=cachegrind --cache-sim=yes \
    --I1=32768,8,64 --D1=32768,8,64 --LL=33554432,16,64 ./bench_ng 10000
```

### Holds end-to-end with a real LLM in the loop

The L3 story survives full-stack production: we point NeoGraph at a
locally-hosted Gemma-4 E2B (Q4_K_M, 4.65 B params, 2.9 GB GGUF) served
by [TransformerCPP](https://github.com/fox1245/TransformerCPP)'s
OpenAI-compatible HTTP endpoint — zero NeoGraph code changes, just
`OpenAIProvider::Config::base_url = "http://localhost:8090"`. See
[`examples/31_local_transformer.cpp`](examples/31_local_transformer.cpp).

| | Pure NeoGraph | **NeoGraph + local Gemma (HTTP)** |
|---|---:|---:|
| L3 instruction misses | 4,320 | **7,262** |
| Hot code working set | 277 KB | **465 KB** (1.42% of L3) |
| Per-request TTFT | — | **25–27 ms** (curl baseline 9–10 ms → ~15 ms NeoGraph overhead) |
| Per-request total | — | 146–213 ms @ 19–27 tokens (~130 tok/s) |
| **NeoGraph agent RSS** | 5.2 MB | **7.6 MB** (+2.4 MB for httplib + JSON streaming) |
| Gemma server RSS | n/a | 2.45 GB (mmap GGUF) |
| VRAM (RTX 4070 Ti) | n/a | 3.06 GB |

The inference process lives in a **separate address space**, so its
2.5 GB of model weights never touch NeoGraph's L3 cache lines. The
agent's 465 KB working set stays L3-resident regardless of how large
the model is. That's the architectural payoff of the two-process
split: you can swap in a 70 B model without inflating the agent.

Burst-tested with 5 concurrent NeoGraph agents against the same server:
aggregate wall 1.58 s / 5 requests (2.65× speedup from coroutine
overlap). Per-agent throughput drops under queue pressure because the
Gemma server doesn't implement continuous batching — that's a
TransformerCPP concern, not an agent one. NeoGraph dispatched all 5
cleanly with no resource pressure and the RSS stayed flat at ~7 MB.

## Quick Start

### Requirements

- **C++20** compiler — coroutines are on the public API surface as
  of 2.0.0. Verified toolchains:
  - **GCC 13.3** — core + all tests green. The OpenAI Responses
    built-in-tools demo (`example_openai_responses_ws_tools`) is
    skipped because GCC 13 trips a coroutine-cleanup ICE
    (`build_special_member_call` at `cp/call.cc:11096`); the rest
    of the project is unaffected and the skip is automatic.
  - **GCC 14.2+** — everything including the tools demo.
  - **Clang 18+** — everything including the tools demo.
  - **MSVC 2022** — core builds + non-Postgres tests in CI; runtime
    not yet load-tested.
- CMake 3.16+.
- OpenSSL (HTTPS), libpq (optional, Postgres checkpoint),
  SQLite3 (optional, SQLite checkpoint).

### Platform support

| Platform | Tier | Notes |
|---|---|---|
| Linux x86_64 (Ubuntu 24.04, GCC 13) | **GA** | Reference — 429/429 ctest green, ASan/UBSan/LSan/TSan clean (CI gates), Valgrind clean (11/11 no-key examples 0 leak/error after stale-`.so` trap fix) |
| macOS (Apple Silicon, Clang) | **beta** | CI builds + non-Postgres tests; runtime differences (coroutine scheduling, SIGPIPE) not yet exercised in production |
| Linux ARM64 (Ubuntu 24.04, GCC 13) | **beta** | Native ARM64 CI gate via GitHub-hosted `ubuntu-24.04-arm` runner — full ctest green every push (no QEMU). Wheel CI uses the same native runner. Bare-metal ARM64 hardware (Raspberry Pi, Graviton) load testing still pending. Stripped binary ~1 MB. |
| Windows (MSVC 2022, x64) | **beta** | Native VS 2022 / MSVC 19.44 build verified — 382/382 ctest pass on Win11, sustained-burst stress 162.04 M graph runs / 5 min @ ~540 k rps with `bench_sustained_concurrent` (0 err, peak 73.6 MB, leak_suspect=false). MCP stdio + PG async socket wrap still need a real-traffic soak. |

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
| 30 | `reasoning_effort` | Same prompt at `reasoning.effort` ∈ {none, low, medium, high} on a reasoning model — compares wall, hidden-CoT tokens, and answer | Required (OpenAI, reasoning model) |

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

Stage 4 reality: `engine->run_async()` stays on the caller's
executor end-to-end — every super-step suspension point (node
dispatch, checkpoint I/O, parallel fan-out, retry backoff) is a real
`co_await`. The three 50 ms steps above therefore overlap on one
io_context thread and the wall time lands at ~50 ms, not 3 × 50 ms.
One thread, N concurrent agents. For CPU-bound fan-out across cores,
switch the driver to a shared `asio::thread_pool` — that's the
pattern in [`benchmarks/concurrent/CONCURRENT.md`](benchmarks/concurrent/CONCURRENT.md)
where N = 10,000 finishes in 52 ms. Within a single run, the
`make_parallel_group` fan-out overlaps too: three parallel-fanout
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
├── examples/                   # 30+ runnable C++ examples + cookbooks (multi-file scenarios)
└── scripts/
    └── embed_schemas.py        # Build-time schema embedding
```

## CMake Targets

| Target | Description | Dependencies |
|--------|-------------|--------------|
| `neograph::core` | Graph engine + types | yyjson (bundled), asio (header-only), Threads |
| `neograph::async` | asio HTTP/SSE helpers | core + OpenSSL |
| `neograph::llm` | LLM providers + Agent | core + OpenSSL (httplib PRIVATE) |
| `neograph::mcp` | MCP client | core + OpenSSL (httplib PRIVATE) |
| `neograph::a2a` | Agent-to-Agent client + server + caller node | core + async + OpenSSL (httplib PRIVATE) |
| `neograph::postgres` | PostgresCheckpointStore | core + libpq |
| `neograph::sqlite` | SqliteCheckpointStore | core + libsqlite3 |
| `neograph::util` | RequestQueue | core + concurrentqueue |

## Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `NEOGRAPH_BUILD_LLM` | ON | Build LLM provider module |
| `NEOGRAPH_BUILD_MCP` | ON | Build MCP client module |
| `NEOGRAPH_BUILD_A2A` | ON | Build Agent-to-Agent module (client + server + caller node) |
| `NEOGRAPH_BUILD_UTIL` | ON | Build utility module |
| `NEOGRAPH_BUILD_POSTGRES` | ON | Build PostgresCheckpointStore (libpq) |
| `NEOGRAPH_BUILD_SQLITE` | ON | Build SqliteCheckpointStore (libsqlite3) |
| `NEOGRAPH_BUILD_EXAMPLES` | ON | Build example programs |
| `NEOGRAPH_BUILD_CLAY_EXAMPLE` | OFF | Build Clay+Raylib chatbot (fetches Raylib) |
| `NEOGRAPH_BUILD_BENCHMARKS` | OFF | Build micro/load benchmark binaries |
| `NEOGRAPH_BUILD_TESTS` | OFF | Build unit tests (GoogleTest auto-fetched) |
| `NEOGRAPH_BUILD_PYBIND` | OFF | Build Python bindings (pybind11 auto-fetched) |
| `BUILD_SHARED_LIBS` | OFF | Build `neograph_*` as `.so`/`.dylib`/`.dll` instead of `.a`. Wired on every supported platform; native MSVC DLL load test still pending — see "Shared library mode" below. |

### Shared library mode

Pass `-DBUILD_SHARED_LIBS=ON` at configure time to ship `libneograph_core.so`,
`libneograph_llm.so`, `libneograph_mcp.so`, `libneograph_async.so`, and
`libneograph_sqlite.so` instead of static archives. Build-tree binaries
get an `$ORIGIN`-relative RPATH so they find the libraries beside
themselves with no `LD_LIBRARY_PATH` gymnastics.

Trade-offs (Linux, stripped, measured 2026-04-25):

| Configuration | Single agent binary | N agents on same host |
|---------------|--------------------:|----------------------:|
| Static (default) | ~2.2 MB per agent | N × 2.2 MB |
| Shared           | ~0.25 MB per agent | N × 0.25 MB + 13.1 MB shared `.so` set (one-time) |

Crossover at N≈7 agents. For deployments shipping multiple NeoGraph
agents on the same host (or for staged-rollout scenarios where one
subsystem like the LLM provider is patched independently of the rest)
shared mode is strictly better. For a single-agent embedded edge
deployment, static keeps everything in one self-contained binary.

Patch-update size example: replacing `libneograph_llm.so` (one
subsystem, ~4 MB) updates every agent on the host without rebuilding
or redeploying any of them.

Windows: `BUILD_SHARED_LIBS=ON` links cleanly — every public class /
free function with an out-of-line .cpp definition carries
`NEOGRAPH_API`, which expands to `__declspec(dllexport)` inside the
engine's TUs and `__declspec(dllimport)` for downstream consumers
(see `include/neograph/api.h`). Verified on Linux shared builds
(`libneograph_*.so` + 429/429 ctest green) and on native MSVC 19.44
(`98f43fd` — VS 2022 BuildTools, x64 Release, OpenSSL 3.0.17 from
PG17 bundle): 382/382 ctest pass and the
`bench_sustained_concurrent` harness held c=1000 for 5 minutes on
Win11 (162.04 M graph runs, 0 err, peak 73.6 MB working-set, no
leak signal). DLL load tests under continuous production traffic
still pending — file an issue if you hit LNK2019 on a public
symbol with the unresolved name.

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
