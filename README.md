<p align="center">
  <h1 align="center">NeoGraph</h1>
  <p align="center">
    <strong>A fast C++ graph runtime with a durable programmable agent control plane.</strong><br>
    Static Core execution when latency matters. QuickJS Programs, sub-agents, Hooks, runtime context, and verified topology evolution when control matters.
  </p>
</p>

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

<p align="center">
  <a href="https://pypi.org/project/neograph-engine/"><img alt="PyPI" src="https://img.shields.io/pypi/v/neograph-engine?label=pip%20install%20neograph-engine&color=blue"></a>
  <a href="https://pypi.org/project/neograph-engine/"><img alt="Python versions" src="https://img.shields.io/pypi/pyversions/neograph-engine"></a>
  <a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-MIT-green.svg"></a>
</p>

<p align="center">
  <a href="#quick-start">Quick Start</a> &middot;
  <a href="#two-runtime-layers">Architecture</a> &middot;
  <a href="#python">Python</a> &middot;
  <a href="examples/README.md">Examples</a> &middot;
  <a href="docs/reference-en.md">C++ Reference</a> &middot;
  <a href="docs/python-binding.md">Python Reference</a>
</p>

---

<p align="center">
  <a href="docs/videos/neograph-promo-v3.mp4">
    <img src="docs/images/neograph-promo-v3.gif" alt="NeoGraph — generated Programs, semantic admission, runtime topology, Hooks, context and Python parity" width="900">
  </a>
</p>

## What NeoGraph is today

NeoGraph has two deliberately separate execution layers:

| Layer | Use it for | Contract |
|---|---|---|
| **GraphEngine / Core** | Fixed or host-selected graphs, low overhead, embedded deployment | Immutable compiled topology; C++ nodes execute through Pregel-style super-steps |
| **ProgramRuntime / QuickJS** | Runtime control, child Programs, structured concurrency, topology replacement and migration | Immutable Program generations; durable typed commands; journaled transitions and replay |

The model never receives compiler, catalog, credential, migration, or authority-granting access. Generated source follows:

```text
proposal → reserve → compile → semantic validate → admit → publish → migrate or spawn
```

A rejected proposal cannot publish a `ProgramVersion`, and its dynamic-compile budget is not restored. See [Strict Runtime Interposition](docs/STRICT_RUNTIME_INTERPOSITION.md) and [DSL capability evaluation](docs/DSL_CAPABILITY_EVAL.md).

## Quick Start

### C++ Core

```bash
git clone https://github.com/fox1245/NeoGraph.git
cd NeoGraph
cmake -S . -B build -DNEOGRAPH_BUILD_EXAMPLES=ON
cmake --build build --parallel
./build/example_core_quickstart
```

The complete source is [examples/62_core_quickstart.cpp](examples/62_core_quickstart.cpp). It registers one C++ node, compiles a strict graph, runs it, and reads a typed channel.

Enable the programmable control plane when needed:

```bash
cmake -S . -B build-program \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON \
  -DNEOGRAPH_BUILD_EXAMPLES=ON
cmake --build build-program --parallel
./build-program/example_program_quickstart
```

See [examples/63_program_quickstart.cpp](examples/63_program_quickstart.cpp) and the [QuickJS authoring boundary](docs/QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md).

## Two runtime layers

### GraphEngine / Core

- static and conditional edges, cycles, barriers, `Send` fan-out and `Command` routing;
- checkpoint/resume, exact-checkpoint resume, fork, state history, HITL and `NodeInterrupt`;
- synchronous and coroutine APIs, streaming, cancellation and token accounting;
- graph-wide and per-node retry policies, jitter and bounded reusable node caching;
- custom registries, providers, tools, MCP, A2A and ACP integration;
- safe-point capture and shape-preserving GraphEngine generation migration.

### ProgramRuntime / QuickJS

- standard JavaScript computation in bounded QuickJS `define()` and generator `main(input)`;
- sealed commands: `callCore`, `spawn`, `await`, `all`, `parallel`, `race`, `quorum`, `emit`, `checkpoint`, `cancelScope`, and admitted host capabilities;
- immutable Program bundles, versions, catalogs, admission profiles and policy snapshots;
- durable command journals, exact replay, child lineage, nonrenewable budgets and process recovery;
- checkpoint replacement and restricted live GraphEngine topology migration;
- host-owned semantic validation before admission of generated Programs.

The installed JavaScript surface is machine-readable through `javascript_authoring_capability_manifest()` and checked against the actual QuickJS bindings in CI.

## Runtime safety and context

NeoGraph moves important behavior outside model discretion:

- immutable RAW message history and `ContextEpoch` selection;
- derived context, required Skills and hard constraints;
- conservative transformation receipts that preserve required artifacts exactly;
- mandatory lifecycle Hooks over native, stdio, or HTTP execution backends;
- provider dispatch and terminal-outcome receipts;
- durable runtime developer instructions and admitted topology transitions.

NeoGraph guarantees construction, admission, dispatch, and evidence boundaries. It does not claim an LLM attended to every token.

## Python

The Python package uses the same C++ engine and now includes the Program, Hook, strict-context, runtime-policy, and SQLite durability surfaces:

```bash
pip install neograph-engine
```

### Five-second demo (no API key)

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

Python additionally exposes:

- `RetryPolicy`, per-node runtime overrides, `RunMetadata`, exact `resume_from`, and reusable cache scope;
- `ProgramSource`, `ProgramRegistryBuilder`, `ProgramCompiler`, `LocalProgramHost`, handles and results;
- mandatory `HookRuntime` callbacks and fail-closed lifecycle delivery;
- `RuntimeContextRequirements`, `ContextTransformReceipt`, SQLite durable context/dispatch stores, and `StrictRuntimeProfile`.

See [Python binding guide](docs/python-binding.md) and [Python examples](bindings/python/examples/README.md).

## Build configuration

Core-only users do not pay for Program or QuickJS:

```bash
cmake -S . -B build-core \
  -DNEOGRAPH_BUILD_PROGRAM=OFF \
  -DNEOGRAPH_BUILD_LLM=OFF \
  -DNEOGRAPH_BUILD_MCP=OFF
```

Important options:

| Option | Purpose |
|---|---|
| `NEOGRAPH_BUILD_PROGRAM` | Durable Program values, catalog, runtime, lineage and migration |
| `NEOGRAPH_BUILD_QUICKJS_CONTROL` | QuickJS Program authoring and generator commands |
| `NEOGRAPH_BUILD_PYBIND` | `neograph-engine` Python extension |
| `NEOGRAPH_BUILD_SQLITE` | SQLite checkpoint, context, Hook and provider-receipt stores |
| `NEOGRAPH_BUILD_POSTGRES` | PostgreSQL checkpoint and Program persistence components |
| `NEOGRAPH_BUILD_MCP_CLIENT` / `SERVER` | MCP client and server roles |
| `NEOGRAPH_BUILD_A2A` / `ACP` / `GRPC` | Optional protocol integrations |

Use the narrow CMake target matching your deployment: `neograph::core`, `neograph::llm`, `neograph::program`, `neograph::mcp`, `neograph::a2a`, or another enabled component.

## Verification

The repository runs deterministic C++ and Python suites, Program replay/migration probes, DSL capability fixtures, documentation/i18n checks, sanitizers, and optional live-model evaluations. Benchmark claims belong in [benchmarks](benchmarks/README.md) and the dated [performance report](docs/performance-deep-dive.md), not as timeless API guarantees.

## Documentation

- [Concepts](docs/concepts.md)
- [C++ reference](docs/reference-en.md)
- [Python binding](docs/python-binding.md)
- [Concurrency and cancellation](docs/concurrency.md)
- [Async guide](docs/ASYNC_GUIDE.md)
- [Harness MCP](docs/HARNESS_MCP.md)
- [QuickJS public authoring boundary](docs/QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)
- [Strict runtime interposition](docs/STRICT_RUNTIME_INTERPOSITION.md)
- [Troubleshooting](docs/troubleshooting.md)
- [Examples](examples/README.md)

## License

MIT — see [LICENSE](LICENSE). Third-party notices: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
