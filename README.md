<p align="center">
  <h1 align="center">NeoGraph</h1>
  <p align="center">
    <strong>The C++ graph agent engine with Python bindings.</strong><br>
    Performance · Self-evolution · Embedded-ready · Lightweight — all four, one binary.<br>
    <i>Pick any three: many frameworks. Pick all four: just NeoGraph.</i>
  </p>
</p>

<p align="center">
  <a href="https://pypi.org/project/neograph-engine/"><img alt="PyPI" src="https://img.shields.io/pypi/v/neograph-engine?label=pip%20install%20neograph-engine&color=blue"></a>
  <a href="https://pypi.org/project/neograph-engine/"><img alt="Python versions" src="https://img.shields.io/pypi/pyversions/neograph-engine"></a>
  <a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-MIT-green.svg"></a>
</p>

<p align="center">
  <a href="#the-four-axes--measurable-each-independently-verifiable">Four Axes</a> &middot;
  <a href="#flagship-cookbooks--what-the-four-axes-enable">Cookbooks</a> &middot;
  <a href="#quick-start">Quick Start</a> &middot;
  <a href="#python-binding">Python Binding</a> &middot;
  <a href="docs/concepts.md">Concepts</a> &middot;
  <a href="examples/README.md">C++ Examples</a> &middot;
  <a href="bindings/python/examples/README.md">Python Examples</a> &middot;
  <a href="docs/troubleshooting.md">Troubleshooting</a> &middot;
  <a href="docs/reference-en.md">API Reference</a> &middot;
  <a href="https://fox1245.github.io/NeoGraph/">Doxygen</a> &middot;
  <a href="#comparison-with-langgraph">vs LangGraph</a>
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

## The four axes — measurable, each independently verifiable

|   | Axis | Measured value | Reproduce |
|---|---|---|---|
| ⚡ | **Performance** | 5 µs engine overhead · 10 K concurrent in 5.5 MB · p99 17 µs flat | [Engine overhead](#engine-overhead-vs-leading-frameworks) · [L3 cache fit](docs/performance-deep-dive.md#the-agent-runtime-that-fits-in-l3-cache) |
| 🧬 | **Self-evolution** | LLM judge → graph_def hot-swap · 5 customer → 3 emergent topology cluster | [self_evolving_chatbot cookbook](examples/cookbook/self_evolving_chatbot/) |
| 🔌 | **Embedded-ready** | 1.2 MB stripped binary · `libc.so.6` only · runs on RPi Zero 2W · MCU-class possible | [Embedded / robotics](docs/performance-deep-dive.md#what-the-numbers-mean-for-embedded--robotics) |
| 🪶 | **Lightweight** | 2 wheel deps (`certifi` + `pydantic`) · multi-tenant 1 K customer → 29 MB · t2.micro 1 K concurrent OK | [multi_tenant_chatbot cookbook](examples/cookbook/multi_tenant_chatbot/) |

Each row is a single command away — no setup, no API key needed except
the live-LLM cookbook variants.

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
NeoGraph row 는 2026-05-13 master HEAD 에서 재측정 (g++ 13 Release
`-O3 -DNDEBUG`, WSL2 + taskset -c 3 chrt -f 99 isolation). Python
framework rows 는 2026-04-22 reference run 그대로 — 2026-04-29 ±10%
재검증.

| Framework | `seq` (3-node chain) | `par` (fan-out 5 + join, v1.0 default)¹ | Slowdown vs. NeoGraph |
|-----------|---------------------:|----------------------------------------:|-------------------:|
| **NeoGraph master** (this repo) | **5.0 µs**  | **11.6 µs** | 1× |
| Haystack 2.28 | 140 µs   | 278 µs   | **28× / 24×** |
| pydantic-graph 1.87 | 227 µs | 280 µs²  | **45× / 24×**² |
| LangGraph 1.1.10 | 643 µs  | 2,262 µs | **128× / 195×** |
| LlamaIndex Workflow 0.14 | 1,565 µs | 4,374 µs | **313× / 377×** |
| AutoGen GraphFlow 0.7.5 | 3,127 µs | 7,281 µs | **625× / 627×** |

¹ NeoGraph 의 `par` 행은 v1.0 기본 워커 수 (= 1) 에서 측정. 시퀀셜
+ CPU-tiny fan-out 의 dispatch 비용만 잰다. 진짜 병렬 wallclock 이
필요한 그래프 (sleep-바운드 시뮬, sync HTTP 등) 는 `engine->
set_worker_count_auto()` 한 줄로 hardware_concurrency 만큼 워커를
풀어준다 — `examples/36_classifier_fanout.cpp` 의 5 분류기 fan-out
은 그 한 줄로 wall time 25.2 ms 순차 → 6.0 ms 병렬 (4.22× speedup).
NeoGraph row 는 master HEAD 에서 재측정 (WSL2 taskset+chrt
isolation); 다른 framework 행은 2026-04-29 reference run (당시
NeoGraph v0.2.3 = 5.0 / 14.4 µs, 두 번째 숫자만 v1.0 cycle 의
fan-out 회귀 fix `e5ecb08` 로 14.4 → 11.6 µs 개선).
² pydantic-graph cannot fan out; emulated as a 6-node chain.

This is the cost of **one engine round-trip**. Real LLM graphs spend
most of their time in network I/O, but every super-step pays this
once — at 100k requests/day a 600 µs framework sheds an hour of CPU
that NeoGraph spends in 5 seconds. Reproducible end-to-end:
[`benchmarks/README.md`](benchmarks/README.md).

**NeoGraph is the only graph agent engine for C++.** If you're building agents in robotics, embedded systems, games, high-frequency trading, or anywhere Python isn't an option — this is it.

## Flagship cookbooks — what the four axes enable

Two production-shaped cookbooks demonstrate categories of agent
infrastructure that other frameworks structurally can't reach.

### Multi-tenant chatbot — 1 K customer in one process

[`examples/cookbook/multi_tenant_chatbot/`](examples/cookbook/multi_tenant_chatbot/) —
JSON graph-as-data + compile cache → one process serves N customers
each with their own agent topology.

| Run | Wall | RSS peak | LLM calls | Errors |
|---|---:|---:|---:|---:|
| Mock provider · 1 000 concurrent · 3 distinct topology | 5 ms | 5.25 MB | 0 | **0** |
| **Real OpenAI gpt-4o-mini · 1 000 concurrent · 6 customer** | 50.2 s | **29 MB** | 2 330 | **0** |

LangGraph for the same multi-tenant shape needs *process per customer*
(StateGraph is a Python object — can't be safely round-tripped through
JSON). 1 000 customer × ~80 MB LG baseline = **~80 GB**. NeoGraph: one
process, 30 MB. **~2 700× memory ratio.**

A t2.micro ($0.01/hour, 1 GB RAM) instance running NeoGraph holds
~10 K concurrent in-flight LLM coroutines comfortably. LangGraph for
the same load needs m5.2xlarge ($0.38/hour) at minimum.

### Self-evolving chatbot — harness reshapes itself live

[`examples/cookbook/self_evolving_chatbot/`](examples/cookbook/self_evolving_chatbot/) —
the multi-tenant infrastructure above + an LLM judge that watches each
customer's conversation and rewrites their graph_def. Customer harness
**evolves to fit user behavior with zero deploy / zero restart**.

| Demo | Customers | Turns | Evolution observed | Distinct engines |
|---|---:|---:|---|---:|
| `server.cpp` (Alice solo) | 1 | 5 | `simple → fanout` at turn 3 | 2 |
| **`server_multi.cpp`** | 5 | 25 | 4 of 5 patterns matched hypothesis; eve oscillated | **3** |

Multi-customer run's distinct-engine count (3) ≈ behavior cluster
count, even with 5 customers — *emergent cluster discovery* falls out
for free. At 1 000 customer scale, distinct shapes typically converge
to ~10 → engine memory stays nearly constant.

LangGraph + StateGraph as a Python class **cannot do this** — runtime
reshape requires module reload + in-flight conversation state loss.
NeoGraph's graph-as-JSON model: evolution = one JSON transform.

> *"AI agent that builds itself" — a vision that's been around since
> AutoGPT (2023). NeoGraph is the first framework where it's a
> production-ready cookbook, not an academic prototype.*

---

## Using NeoGraph from your CMake project

The `pip install` route is Python-only — the wheel doesn't ship C++
headers. For a C++ project, the simplest path is `FetchContent`,
which behaves like `pip install` for CMake:

```cmake
include(FetchContent)
FetchContent_Declare(
    NeoGraph
    GIT_REPOSITORY https://github.com/fox1245/NeoGraph.git
    GIT_TAG        main
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

### 흔한 함정 5선 — 신참이 첫 30분에 부딪치는 것들

NeoGraph 처음 쓰는 사람이 가장 자주 막히는 5 가지. 미리 한 번 훑어두면 디버깅 30분 절약됩니다.

1. **결과는 `result.channel<T>("name")` 으로 꺼내세요** ([example 51](examples/51_minimal.cpp))
   `result.output["counter"]` 식 직접 접근은 `react_graph` 같은 일부 빌더에서만 통합니다. raw `GraphEngine::compile` 그래프는 `output["channels"]["counter"]["value"]` 한 겹 더 들어간 모양이라 직접 접근하면 깨집니다. 도우미 함수 `result.channel<T>("counter")` 는 두 모양 모두 처리.

2. **노드 안에서 `Store` 는 `in.ctx.store` 로** ([example 43](examples/43_store_personalization.cpp))
   `engine->set_store(...)` 로 `Store` 박으면 v0.7+ 부터는 노드 본문에서 `in.ctx.store->get(ns, key)` 한 줄로 닿습니다. 옛 패턴 (`NodeFactory` 람다에서 `shared_ptr<Store>` 캡처) 도 계속 동작하지만 새 코드는 `in.ctx.store` 가 깔끔.

3. **`neograph::graph::` 서브네임스페이스** — `GraphEngine`, `GraphNode`, `RunConfig`, `RunResult` 는 모두 `neograph::graph::` 안에 있습니다. 매번 풀 경로로 쓰기 싫으면 파일 위에 `using namespace neograph::graph;` 한 줄. `Provider`, `Tool`, `json` 같은 것들은 `neograph::` 바로 아래.

4. **`<httplib.h>` 직접 쓰는 TU 는 `CPPHTTPLIB_OPENSSL_SUPPORT` 정의 필수** ([issue #16](https://github.com/fox1245/NeoGraph/issues/16))
   여러분 코드에서 `httplib::Server` 같은 걸 직접 쓴다면, `<httplib.h>` 를 include 하기 전에 반드시 매크로를 정의하세요. 안 하면 NeoGraph 의 cpp-httplib 와 ABI 가 안 맞아서 (One Definition Rule 위반) 런타임에 `getaddrinfo` 안에서 SEGV. v0.8+ 부터는 컴파일 타임에 자동 검출됩니다 (`<neograph/api.h>` 의 `#error` 가드).

   ```cpp
   #define CPPHTTPLIB_OPENSSL_SUPPORT   // ← 반드시 먼저
   #include <httplib.h>
   ```

   또는 CMake 측에서:
   ```cmake
   target_compile_definitions(your_target PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
   ```

5. **GCC 13 코루틴 ICE** — Ubuntu 24.04 기본 GCC 13.3 에서 `co_await x.foo_async(...)` 형태가 `internal compiler error: in build_special_member_call, at cp/call.cc:11096` 으로 죽습니다. 컴파일러 버그라 코드는 멀쩡. 회피: GCC 14 업그레이드, 또는 `co_spawn` 대신 `neograph::async::run_sync(awaitable)` 로 sync-bridge. 자세한 안내는 [troubleshooting.md "Build errors"](docs/troubleshooting.md).

각 항목의 더 깊은 내용은 [troubleshooting.md](docs/troubleshooting.md) 와 링크된 예제 / 이슈에 있습니다.

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
  - **If you also `#include <httplib.h>` in your own code** (e.g. for
    your own `httplib::Server` SSE endpoint), every TU that does so
    MUST `#define CPPHTTPLIB_OPENSSL_SUPPORT` **before** the include —
    or set it via `target_compile_definitions(your_target PRIVATE
    CPPHTTPLIB_OPENSSL_SUPPORT)` globally. NeoGraph's SchemaProvider
    .cpp defines it; if your TUs don't match, the linker silently
    picks one `httplib::ClientImpl` layout and the mismatched TU
    reads members at wrong offsets → SEGV inside `getaddrinfo` on the
    first LLM call. Issue #16 documented the trap end-to-end; see
    [docs/troubleshooting.md → "C++ consumers — `httplib.h` macro
    consistency"](docs/troubleshooting.md#c-consumers--httplibh-macro-consistency-load-bearing-issue-16)
    for the audit recipe and the one-line fix.

## Python Binding

NeoGraph ships as a `pip`-installable package — the same C++ engine,
driven from a Jupyter notebook, Gradio app, or FastAPI service:

```bash
pip install neograph-engine
```

```python
import neograph_engine as ng

@ng.node("greet")
def greet(state):
    return [ng.ChannelWrite("messages",
        [{"role": "assistant", "content": f"Hello, {state.get('name')}!"}])]

definition = {
    "name": "demo",
    "channels": {"name": {"reducer": "overwrite"},
                 "messages": {"reducer": "append"}},
    "nodes":    {"greet": {"type": "greet"}},
    "edges":    [{"from": ng.START_NODE, "to": "greet"},
                 {"from": "greet", "to": ng.END_NODE}],
}
engine = ng.GraphEngine.compile(definition, ng.NodeContext())
result = engine.run(ng.RunConfig(thread_id="t1", input={"name": "NeoGraph"}))
print(result.output["channels"]["messages"]["value"])
# [{'role': 'assistant', 'content': 'Hello, NeoGraph!'}]
```

20 wheels + sdist per release (Linux x86_64 / aarch64, macOS arm64,
Windows x64 · Python 3.9–3.13). **Full guide — ReAct with a real LLM,
async, custom reducers, the LangGraph-divergence list, in-tree
observability, Docker-free deployment: [`docs/python-binding.md`](docs/python-binding.md).**

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

Two models out of the box: **thread-per-agent (sync)** — share one
compiled engine across threads with distinct `thread_id`s — and
**coroutine async** — one `asio::io_context` hosts thousands of
agents without a thread per run. Plus a bundled lock-free
`RequestQueue` for fixed-pool backpressure and
`PostgresCheckpointStore` for restart-surviving state.

**Full patterns, safe-concurrency rules, and the Postgres setup:
[`docs/concurrency.md`](docs/concurrency.md)** · async migration
guide: [`docs/ASYNC_GUIDE.md`](docs/ASYNC_GUIDE.md).

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

## Performance & production economics

Headline numbers are in [The four axes](#the-four-axes--measurable-each-independently-verifiable)
and [Why NeoGraph?](#why-neograph) above. The full evidence —
AWS/GCP cost modeling, the 10 K-worker one-GPU stress test, the
L3-cache-fit cachegrind sweep, and the local-LLM end-to-end hold —
lives in **[`docs/performance-deep-dive.md`](docs/performance-deep-dive.md)**.

> *"LangChain runtime cost: ~$4 K/mo for 1 K concurrent users.
> NeoGraph: ~$50/mo. Same code shape, same LLM, frozen ABI."*

## Benchmarks

Matched-topology, zero-I/O engine overhead (µs/iter, lower better):

| Framework | `seq` (3-node) | `par` (fan-out 5) | vs. NeoGraph |
|---|--:|--:|--:|
| **NeoGraph master** | **5.0 µs** | **11.8 µs** | 1× |
| Haystack 2.28 | 144 µs | 290 µs | 29× |
| pydantic-graph 1.85 | 236 µs | 286 µs | 47× |
| LangGraph 1.1.9 | 657 µs | 2,349 µs | 131× |
| LlamaIndex 0.14 | 1,780 µs | 4,684 µs | 356× |
| AutoGen 0.7.5 | 3,209 µs | 7,293 µs | 642× |

At N=10,000 concurrent (1 CPU / 512 MB sandbox): NeoGraph 52 ms /
7 µs p99 / 5.5 MB · LangGraph 23.4 s / 416 MB · LlamaIndex & AutoGen
OOM-killed. **Full matrix, burst-concurrency curves, size/cold-start,
methodology: [`docs/performance-deep-dive.md`](docs/performance-deep-dive.md)**
and [`benchmarks/README.md`](benchmarks/README.md).

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
