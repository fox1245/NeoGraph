# NeoGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

[![CI](https://github.com/fox1245/NeoGraph/actions/workflows/ci.yml/badge.svg)](https://github.com/fox1245/NeoGraph/actions/workflows/ci.yml)
[![PyPI](https://img.shields.io/pypi/v/neograph-engine)](https://pypi.org/project/neograph-engine/)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

NeoGraph is a C++20 library for running workflows that you describe as graphs. Nodes do the work, named state channels hold the data, and edges decide what runs next. The runtime schedules the nodes, merges their writes into state, saves a checkpoint after each step when a checkpoint store is configured, and can pause a run for human input and resume it later.

The same engine is available from C++ and from Python (`pip install neograph-engine`). Optional components add model calls through the SchemaProvider SDK, MCP, A2A, ACP and gRPC servers and clients, SQLite and PostgreSQL checkpoint stores, and a Program layer in which restricted JavaScript programs coordinate several graphs under a budget.

## Who it is for

- Python and C++ developers who build agent or pipeline workflows with explicit state: loops, branches, parallel fan-out, retries, checkpoints and human approval steps.
- Teams that run such workflows inside a C++ service, or expose them to other agents and tools over MCP, A2A or ACP.
- Readers who know LangGraph: channels with reducers, `Send`, `Command` and checkpoints are the same ideas, so the [concepts guide](docs/concepts.md) should read quickly.

NeoGraph is alpha software (version 0.14.2). The C++ binary interface can change between 0.x releases; see the [ABI policy](docs/ABI_POLICY.md). It does not train models or replace a numerical library, and a model call is only one kind of node.

## Quickstart

Both examples run a graph with one node. They need no API key and make no network call at run time.

### Python

Python 3.9 or newer:

```bash
pip install neograph-engine
```

Save this as `hello.py`:

```python
import neograph_engine as ng

@ng.node("greet")
def greet(state):
    return [ng.ChannelWrite(
        "messages",
        [{"role": "assistant", "content": f"Hello, {state.get('name')}!"}],
    )]

definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "demo",
    "channels": {
        "name": {"reducer": "overwrite"},
        "messages": {"reducer": "append"},
    },
    "nodes": {"greet": {"type": "greet"}},
    "edges": [
        {"from": ng.START_NODE, "to": "greet"},
        {"from": "greet", "to": ng.END_NODE},
    ],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())
result = engine.run(ng.RunConfig(thread_id="t1", input={"name": "NeoGraph"}))
print(result.output["channels"]["messages"]["value"])
```

```bash
python hello.py
```

```text
[{'role': 'assistant', 'content': 'Hello, NeoGraph!'}]
```

The wheel on PyPI is built from release 0.14.2. Changes listed under *Unreleased* in the [changelog](CHANGELOG.md) are not in it; build from source to use them (see [Build configuration](#build-configuration-and-further-reading)).

### C++

You need a C++20 compiler, CMake 3.20 or newer, git, and the libcurl development files (libcurl 7.88 or newer; `libcurl4-openssl-dev` on Debian and Ubuntu). The first configure downloads the pinned SchemaProvider SDK, which Core requires even when no model call is made; for an offline machine, see [Build and run](#build-and-run) below.

NeoGraph has no tagged release that matches this documentation yet, so the project below fetches the `master` branch. Replace `master` with a commit hash to make the build reproducible.

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(hello_neograph CXX)

include(FetchContent)
# Build only Core: no model calls, protocols, databases, examples or tests.
set(NEOGRAPH_BUILD_EXAMPLES OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_LLM OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_ASYNC OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_MCP OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_A2A OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_ACP OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_POSTGRES OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_SQLITE OFF CACHE BOOL "")
FetchContent_Declare(NeoGraph
  GIT_REPOSITORY https://github.com/fox1245/NeoGraph.git
  GIT_TAG master)
FetchContent_MakeAvailable(NeoGraph)

add_executable(hello main.cpp)
target_compile_features(hello PRIVATE cxx_std_20)
target_link_libraries(hello PRIVATE neograph::core)
```

`main.cpp`:

```cpp
#include <neograph/neograph.h>

#include <cctype>
#include <iostream>
#include <memory>
#include <utility>

using namespace neograph;
using namespace neograph::graph;

namespace {

const ChannelKey<std::string> text_channel{"text"};

class UpperNode final : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        auto text = input.state.get(text_channel);
        for (auto& character : text)
            character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        co_return NodeOutput{{ChannelWrite{"text", json(std::move(text))}}};
    }

    std::string get_name() const override { return "upper"; }
};

}  // namespace

int main() {
    NodeFactory::instance().register_type(
        "quickstart.upper", [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<UpperNode>();
        },
        json::object(),
        json{{"reads", json::array({"text"})},
             {"writes", json::array({"text"})},
             {"exports", json::array({"text"})}});

    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "hello"},
        {"channels", {{"text", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"upper", {{"type", "quickstart.upper"}}}}},
        {"edges",
         {{{"from", "__start__"}, {"to", "upper"}},
          {{"from", "upper"}, {"to", "__end__"}}}},
    };

    auto engine = GraphEngine::build_strict(definition, EngineConfig{});
    RunConfig config;
    config.input = {{"text", "hello"}};
    const auto result = engine->run(config);
    std::cout << result.channel(text_channel) << '\n';
    return 0;
}
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/hello
```

```text
HELLO
```

The same graph is [examples/62_core_quickstart.cpp](examples/62_core_quickstart.cpp). To use a NeoGraph installed with `cmake --install` instead, call `find_package(NeoGraph CONFIG REQUIRED)` and link `neograph::core`; see [Build configuration](#build-configuration-and-further-reading).

### Next steps

- [Documentation index](docs/README.md): every guide, and who it is for.
- [Concepts](docs/concepts.md): channels, nodes, edges, `Send`, `Command`, checkpoints and streaming.
- [Python binding guide](docs/python-binding.md) and [C++ examples](examples/README.md).
- Servers and clients: [MCP](docs/mcp.md), [A2A](docs/a2a.md), [ACP](docs/acp.md).
- [Program guide](docs/program-guide.md): restricted JavaScript programs with fan-out, failure routing, approval and budgets.

## How a graph changes state

Suppose a channel named `count` holds `2`. An increment node reads `2` and returns a write proposing `3`. The runtime applies that write through the channel's reducer after the scheduled node batch finishes. A downstream node in the next batch reads `3`.

```text
Committed state       Node computation          Reduced state
count = 2       ->    read 2; propose 3     ->    count = 3
                                                  |
                                            next node reads 3
```

The terms in that trace describe the execution model:

| Term | Meaning in NeoGraph |
|---|---|
| Node | An executable registered with the host. It reads its input state and returns channel writes and optional routing commands. |
| State | The channel values visible to the current execution step, together with runtime-owned history and accounting where configured. |
| Channel | A named value with a reducer and optional retention and checkpoint-persistence policies. |
| Reducer | A function combining the current channel value with an incoming write. `overwrite` replaces a value; `append` accumulates array elements; a custom reducer defines another combination. |
| Edge | A scheduling rule between nodes. Edges may be unconditional, conditional, or a barrier waiting for several predecessors. Cycles permit repeated stages. |
| Superstep | A scheduled batch of ready node executions followed by applying their writes and advancing the schedule. |

During a normal batch, ready nodes read the pre-batch channel state. Returning a `ChannelWrite` does not immediately change what a sibling node reads. After successful completion, the executor applies the results, and later steps see the updated state. In a multi-branch `Send` batch, each branch receives its input in an isolated state copy and merges its output afterward. This is a Pregel-style organization of work; NeoGraph's channel/reducer rules are its own contract, not a claim that it implements every Pregel feature.

Concurrent execution does not make every reducer order-independent. If two nodes append text or overwrite the same channel, write order affects the result. Use independent channels or an order-independent reducer when the workflow requires that property. Channel retention also differs from combination: an append channel can keep only a bounded suffix. See [concepts](docs/concepts.md) and [concurrency](docs/concurrency.md) for scheduling, reducers, barriers, and cancellation.

## The Core example step by step

The [C++ quickstart](examples/62_core_quickstart.cpp) above registers an uppercase node and compiles this topology:

```text
__start__ -> upper -> __end__

Input channel:   text = "hello"
Node reads:      "hello"
Node returns:    ChannelWrite{"text", "HELLO"}
Reducer:         overwrite
Output channel:  text = "HELLO"
```

The source declares which channels the node reads and writes, so `GraphEngine::build_strict` can check the topology before the run. No model call or API key is needed. The expected output is `HELLO`.

### Build and run

SchemaProvider is a required external SDK even when `NEOGRAPH_BUILD_LLM=OFF`, because Core exports its typed provider contracts. The commands below use an installed [SchemaProvider runtime package](https://github.com/fox1245/SchemaProvider): set `SCHEMAPROVIDER_PREFIX` to its install prefix. An explicit checkout supplied with `-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=../SchemaProvider` takes precedence; otherwise CMake prefers an installed package and, if none is found, fetches the pinned public SDK archive. Set `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` for an offline build with an installed package or explicit checkout. CMake does not guess a sibling checkout or use the removed bundled interpreter.

Prerequisites include a C++20 compiler, CMake 3.20 or newer, and the SDK's runtime dependency libcurl 7.88 or newer; the SDK itself needs no OpenSSL, because its TLS comes from libcurl's backend. Full builds with NeoGraph's HTTPS components require OpenSSL 3. The default build also enables SQLite and PostgreSQL integrations; the command below disables unnecessary NeoGraph components without removing SDK dependencies. See [troubleshooting](docs/troubleshooting.md) for platform and build constraints.

```bash
git clone https://github.com/fox1245/NeoGraph.git
cd NeoGraph
cmake -S . -B build-core \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_PROGRAM=OFF \
  -DNEOGRAPH_BUILD_LLM=OFF \
  -DNEOGRAPH_BUILD_ASYNC=OFF \
  -DNEOGRAPH_BUILD_MCP=OFF \
  -DNEOGRAPH_BUILD_A2A=OFF \
  -DNEOGRAPH_BUILD_ACP=OFF \
  -DNEOGRAPH_BUILD_POSTGRES=OFF \
  -DNEOGRAPH_BUILD_SQLITE=OFF
cmake --build build-core --parallel --target example_core_quickstart
./build-core/example_core_quickstart
```

## Core and ProgramRuntime

`GraphEngine` executes a compiled graph. It owns node scheduling, state updates, routing, retries, streaming, cancellation, and graph checkpoint/resume. A compiled topology is immutable; supported generation migration occurs at controlled safe points rather than through arbitrary mutation while nodes run.

`ProgramRuntime` coordinates admitted Programs that can call Core graphs and manage child Programs. It adds immutable Program versions, catalogs and policy snapshots, command journals, child lineage, budgets, replay, and admitted replacement or migration. The host registers executable capabilities, compiles and admits a Program, and starts an invocation. Core remains the graph node executor.

For the research workflow, one Core graph can perform retrieval and review. A Program can call that graph, start child Programs for separate tasks, await their results, and record lifecycle transitions. Durable recovery requires the configured stores and the relevant custody contracts; an in-memory store does not survive process exit, and a journal does not by itself make an external tool effect exactly-once.

QuickJS is an optional Program authoring surface. Programs use bounded JavaScript computation and generator commands such as `callCore`, `spawn`, `await`, `all`, `parallel`, `race`, `quorum`, `emit`, `checkpoint`, and `cancelScope`. The host admits capabilities and validates generated source before publication. A model-generated proposal does not receive compiler, credential, catalog, or authority-granting access.

```bash
cmake -S . -B build-program \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON \
  -DNEOGRAPH_BUILD_EXAMPLES=ON
cmake --build build-program --parallel --target example_program_quickstart
./build-program/example_program_quickstart
```

The [Program quickstart](examples/63_program_quickstart.cpp) compiles and admits a Program that calls an increment graph; its expected output is `1`. It uses in-memory stores and the C++ Program builder. For JavaScript authoring and durable execution, start with the [Program guide](docs/program-guide.md), [recursive Programs](docs/PROGRAM_RECURSIVE_HARNESSES.md), and [strict runtime contracts](docs/STRICT_RUNTIME_INTERPOSITION.md).

## Typed provider calls

Model calls use a validated descriptor, runtime options, and a typed request. Descriptor admission accepts closed, versioned data; it does not execute a request/response interpreter. Credentials belong in runtime options, not public descriptor files. `SchemaProvider::Defaults` contains typed OpenRouter routing and Responses retention controls. Images, Veo, and Decisions use separate typed clients and authorization.

```cpp
#include <neograph/llm/schema_provider.h>
#include <neograph/types.h>

sp::runtime::Result first_call(
    sp::descriptor::ValidatedDescriptor descriptor,
    sp::runtime::Options options, std::string model) {
    neograph::llm::SchemaProvider provider(
        std::move(descriptor), std::move(options), {});
    std::vector<sp::Message> history{
        {.role = sp::Role::User, .parts = {sp::Text{"hi"}}}};
    auto request = neograph::make_provider_request(
        provider, std::move(model), std::move(history));
    auto prepared = provider.prepare(std::move(request));
    return provider.dispatch(std::move(prepared));
}
```

A prepared request is consumed once. `sp::runtime::Result` owns an immutable `sp::Outcome` containing a `Completion` or `Failure`. Keep that outcome when you need ordered messages and parts, native continuation, raw observations, stop evidence, attempt metadata, or failure partials. Usage counters are nullable: missing means unknown, while an observed zero remains zero. A usage report and a budget charge are separate records; a portable report cannot grant spending authority.

`usage.provider_cost` retains provider-reported nanoUSD amounts, per-field status, source, quality, BYOK evidence, and upward rounding from the parsed binary64 value. It is not an invoice, token count, estimate, or spending authority. Missing monetary metadata remains absent in canonical v2 JSON; a reported zero or `is_byok=false` is still evidence. Unknown currency and conflicting/malformed reports are retained, not converted to USD or tokens. Blocking provider calls still drive the single async SDK pipeline and reject SDK event-loop/callback-worker re-entry.

After `first_call` returns, use `std::get_if<sp::Completion>(result.get())` to inspect a completion's `messages`, `stop`, and `usage`. Otherwise, `std::get<sp::Failure>(*result)` provides `error.kind`, `error.safe_message`, retry evidence, and the partial messages and usage in `partial`. For example, an absent `completion.usage.output_total` means the output-token count is unknown; a present counter whose `value` is `0` reports zero. Display text is only one view of the retained outcome.

`ChatMessage`, `ChatTool`, and JSON are portable projections. Authentic native history can stay in memory with its native checkpoint sidecar; durable native history requires a real `sp::NativeArchive` and protected owner-private custody. Portable JSON cannot recreate that authority. The archive authenticates custody with an independent key; it is neither encryption nor vendor-issuer authentication. Do not publish archive bodies, keys, native blobs, or raw wire observations. See the [provider reference](docs/reference-en.md) and [migration guide](docs/migration-v0.4-to-v1.0.md) for persistence failures, observers, managed budget banks, and replay boundaries.

The typed cutover removes `CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, the descriptor interpreter, and the Responses WebSocket path. Recompile C++ consumers and migrate custom providers; there are no compatibility aliases. The SDK package is `0.3.2` alpha, pinned to public commit `91fb993e50e49e3b03d290b170c83cf57afbc518` on its `maint/interface6` branch, with interface revision 6 and shared ABI 6. Monetary usage metadata changes public layouts: rebuild SDK consumers, NeoGraph and Python extensions together. Native archive v3 and portable JSON v2 remain unchanged; the generation does not declare a stable SDK interface.

## Python

```bash
pip install neograph-engine
```

The typed-provider API described here targets NeoGraph `0.14.0`; historical wheels expose the older interface. The [Python binding guide](docs/python-binding.md) documents the source API and build prerequisites. The graph in the [Quickstart](#python) needs no API key.

Python also exposes Program compilation and execution, Hooks, runtime-context requirements, strict profiles, SQLite durability, and exact checkpoint resume. Typed providers use `ProviderMessage` and `make_provider_request`, then `prepare`/`dispatch` or `invoke`; outcomes retain native-owned completion or failure evidence. These messages differ from graph convenience `ChatMessage` values. Blocking provider calls release the GIL; `asyncio.to_thread` can move them off an event-loop thread. See the [Python examples](bindings/python/examples/README.md) for complete provider inputs, and the [Python binding guide](docs/python-binding.md#program-and-quickjs) for a runnable Program example and its rules.

## Workloads and limits

NeoGraph fits workflows with explicit state transitions, branches or loops, parallel tasks, checkpoints, and human review. It can also embed small fixed graphs in a C++ application. Node computation remains the application's responsibility: the graph runtime does not train a model or replace a numerical-computing library, and a model call still has the provider's latency, availability, and cost.

The runtime supports graph-wide and per-node retry policies, bounded reusable node caching, `Send` fan-out, `Command` routing, subgraphs, state history, forks, HITL, and `NodeInterrupt`. MCP, A2A, ACP, gRPC, and observability integrations are optional components. Runtime context and Hooks can require particular dispatch inputs and record delivery evidence; those checks do not prove a model attended to every token. Durable native recovery and bounded forks need their original shared accounting authority; copying a snapshot cannot renew a budget.

Measure a workload with its actual nodes, provider, stores, concurrency, and build profile. [Benchmarks](benchmarks/README.md) and the [performance guide](docs/performance-deep-dive.md) describe measured configurations and limitations, not universal speed claims. Single-config builds should specify `CMAKE_BUILD_TYPE=Release` when measuring optimized execution. `NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION=ON` adds host-specific tuning on supported compilers; keep it off for distributable binaries.

## Build configuration and further reading

| Option | Purpose |
|---|---|
| `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` | Explicit SDK checkout; takes precedence over installed-package discovery and fetching. |
| `NEOGRAPH_FETCH_SCHEMAPROVIDER` | Fetch the pinned public SDK archive when no package is installed; default on. Disable for offline builds. |
| `NEOGRAPH_BUILD_PROGRAM` | Program runtime, catalogs, lineage, and migration; default off. |
| `NEOGRAPH_BUILD_QUICKJS_CONTROL` | Embedded QuickJS Program authoring; default off. |
| `NEOGRAPH_BUILD_PYBIND` | Python extension; default off. |
| `NEOGRAPH_BUILD_LLM` | NeoGraph model-call adapters; disabling them does not remove the SDK dependency. |
| `NEOGRAPH_BUILD_SQLITE` / `NEOGRAPH_BUILD_POSTGRES` | Optional persistent stores; both default on. |
| `NEOGRAPH_BUILD_MCP_CLIENT` / `NEOGRAPH_BUILD_MCP_SERVER` | MCP client and stdio server components; both default on (`NEOGRAPH_BUILD_MCP` sets their default). |
| `NEOGRAPH_BUILD_MCP_HTTP_SERVER` | MCP Streamable HTTP server transport; default off. |
| `NEOGRAPH_BUILD_HARNESS_MCP_BINARY` | Build and install the `neograph-harness-mcp` executable; default off. |
| `NEOGRAPH_BUILD_A2A` / `NEOGRAPH_BUILD_ACP` / `NEOGRAPH_BUILD_GRPC` | Protocol integrations; A2A and ACP default on, gRPC defaults off. |
| `NEOGRAPH_BUILD_ASYNC` | HTTP, SSE and WebSocket helpers; default on. |
| `NEOGRAPH_BUILD_EXAMPLES` / `NEOGRAPH_BUILD_TESTS` | Example programs (default on) and unit tests (default off). |
| `NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION` | Non-portable host-specific tuning for optimized configurations; default off. |

To install Core and use it from another project instead of fetching it, build and install it once. When CMake fetched the SDK, `cmake --install` puts the SDK into the same prefix, so one `CMAKE_PREFIX_PATH` entry is enough:

```bash
git clone https://github.com/fox1245/NeoGraph.git
cmake -S NeoGraph -B NeoGraph/build-install -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_EXAMPLES=OFF -DNEOGRAPH_BUILD_LLM=OFF -DNEOGRAPH_BUILD_ASYNC=OFF \
  -DNEOGRAPH_BUILD_MCP=OFF -DNEOGRAPH_BUILD_A2A=OFF -DNEOGRAPH_BUILD_ACP=OFF \
  -DNEOGRAPH_BUILD_POSTGRES=OFF -DNEOGRAPH_BUILD_SQLITE=OFF
cmake --build NeoGraph/build-install --parallel
cmake --install NeoGraph/build-install --prefix "$HOME/neograph"
```

In the other project, replace the `FetchContent` lines of the Quickstart with `find_package(NeoGraph CONFIG REQUIRED)` and configure with `-DCMAKE_PREFIX_PATH="$HOME/neograph"`. Installed consumers link only the enabled components they need:

```cmake
find_package(SchemaProvider 0.3.2 EXACT CONFIG REQUIRED COMPONENTS runtime transport)
find_package(NeoGraph CONFIG REQUIRED)
target_link_libraries(app PRIVATE neograph::core neograph::llm SchemaProvider::runtime)
```

### Local CI verification (Windows and WSL)

Run from the source root with Python 3.10+, CMake/CTest, and the profile's native
toolchain and dependencies already provisioned. Use a VS2022 x64 developer
PowerShell for Windows; provide the vcpkg transport toolchain through
`CMAKE_TOOLCHAIN_FILE`, its private `VCPKG_INSTALLED_DIR`, and OpenSSL/curl runtime
tools on `PATH`. In WSL, provision a C++20 compiler, pkg-config, libpq, SQLite,
OpenSSL and HTTP/2 curl development packages. Linux `native-linux` and `asan`
also require `psql` and `NEOGRAPH_TEST_POSTGRES_URL` pointing to a reachable
**destructive-test-only** database. Provision Python pytest/pydantic/certifi;
`native-linux` additionally needs `a2a-sdk[http-server]>=1.1,<2`,
`agent-client-protocol==0.12.1`, uvicorn and httpx in the selected interpreter.

```powershell
# Windows: choose a new output path for each invocation.
python scripts/verify_ci.py native-windows --work-dir build/local-windows-01 --jobs 4
python scripts/verify_ci.py install --work-dir build/local-install-01 --jobs 4 --shared --program
```

```sh
# WSL/Linux: provision the test database and dependencies before running.
python scripts/verify_ci.py native-linux --work-dir build/local-linux-01 --jobs 4
python scripts/verify_ci.py quickjs-performance --work-dir build/local-quickjs-01 --jobs 4
```

`--work-dir` must be a fresh, nonexistent output path you own, not the source
root or its ancestor; `--jobs` must be a positive number or `auto` (the CPU count). Existing outputs are refused
and retained, never automatically cleaned. The runner does not install
dependencies or change host policy; existing pinned dependency fetches and
cibuildwheel's declared bootstrap/repair still apply.

| Profile | Retained purpose / prerequisite |
|---|---|
| `native-linux` | Full native PostgreSQL gate, then serial full Python/protocol suite and DB-free ACP durable rerun. |
| `native-posix` | Native Linux ARM/macOS suite; PostgreSQL build/link coverage without a test service. |
| `native-windows` | MSVC DB-free native/Program/QuickJS suite and separate C embedding ABI smoke; builds with Ninja in an x64 MSVC environment, or pass `--generator "Visual Studio 17 2022"`. |
| `asan` | Linux ASan/UBSan/LSan, eleven examples and complete Python suite; GCC libasan/libstdc++ required. |
| `tsan` | Separate Linux TSan suite and five examples; permitted process-local `setarch -R`, unchanged suppressions. |
| `msvc-asan` | Serial Windows Program/QuickJS canary; activated `cl >=19.50` (VS2026), not MSVC 19.44. |
| `grpc` | gRPC graph contract; provision gRPC/Protobuf compiler and libraries. |
| `benchmark` | Linux Release four-workload regression gate; original throughput, latency and target-RSS limits. |
| `quickjs-performance` | Linux matched enabled/disabled builds; actual source-root Git checkout required for immutable provenance. Ceilings are calibrated on CI's hosted x86-64 runners: on slower hardware `javascript_wrapped_core` can fail without a regression. `--base <ref>` also builds that commit and compares each case with it on the same machine. |
| `fuzz` | Linux Clang/libFuzzer 60-second canary; corpus copied into owned output. |
| `install` | Isolated exported-prefix ABI/symbol/C++/C11/collision/relocation consumers; no Git/Bash. Optional `--shared`, `--core-only` or `--program`; `--quickjs` requires `--program` and ELF/Mach-O inspection (Windows rows omit it). |
| `sdist` | Source archive and Twine; provision build/twine/scikit-build-core>=1.0/pybind11==2.13.6/ninja>=1.10; optional `--release-tag v0.14.2`. |
| `wheel` | Repaired installed wheels; provision cibuildwheel==2.23.0 and native/container provider; required `--arch x86_64\|aarch64\|arm64\|AMD64`; optional `--python cp312` builds one CPython instead of all. |
| `runtime-archive` | Shared SDK native-archive portability target/CTest; Ninja and platform runtime dependencies. |

Local Windows/WSL results do not replace native ARM/macOS or VS2026 sanitizer
rows. Every push and pull request runs `ci.yml`: the full native suite on Linux,
macOS and Windows plus two installed-consumer rows; documentation-only changes
skip it. `ci-extended.yml` runs nightly and on demand: sanitizers, the fuzz
canary, performance gates, native ARM64, gRPC, the Visual Studio generator and
the other eight installed-consumer rows. Run it on a release candidate before
tagging. `wheels.yml` builds one CPython 3.12 wheel per platform for packaging changes. On
release tags it builds wheels for CPython 3.9–3.13 on Linux (x86_64 and aarch64), macOS (arm64)
and Windows (AMD64), with glibc 2.34 and macOS 14 as the minimum platform versions.

- [Documentation index](docs/README.md)
- [Concepts and graph semantics](docs/concepts.md)
- [C++ reference](docs/reference-en.md) and [Python binding guide](docs/python-binding.md)
- [MCP](docs/mcp.md), [A2A](docs/a2a.md) and [ACP](docs/acp.md) guides, and the [Program guide](docs/program-guide.md)
- [Async guide](docs/ASYNC_GUIDE.md) and [concurrency/cancellation](docs/concurrency.md)
- [Runtime context and strict interposition](docs/STRICT_RUNTIME_INTERPOSITION.md)
- [Harness MCP](docs/HARNESS_MCP.md)
- [Migration guide](docs/migration-v0.4-to-v1.0.md) and [troubleshooting](docs/troubleshooting.md)
- [C++ examples](examples/README.md), [Python examples](bindings/python/examples/README.md), and [benchmark methodology](benchmarks/README.md)

## License

MIT; see [LICENSE](LICENSE). Third-party notices are in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
