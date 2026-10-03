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

SchemaProvider is now a required external C++ dependency even when `NEOGRAPH_BUILD_LLM=OFF`: Core exports its owned typed provider contracts. Install the SDK runtime package and set `SCHEMAPROVIDER_PREFIX` to that install prefix; the configure commands below use `-DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX"`. Alternatively supply an explicit checkout with `-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=../SchemaProvider`. Neither a guessed sibling checkout nor the old bundled interpreter is selected automatically. The SDK runtime/archive currently supports Linux/POSIX; there is no dependency-free, no-OpenSSL, native Windows/macOS or WASM runtime promise for this cutover.

```bash
git clone https://github.com/fox1245/NeoGraph.git
cd NeoGraph
cmake -S . -B build -DNEOGRAPH_BUILD_EXAMPLES=ON -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX"
cmake --build build --parallel
./build/example_core_quickstart
```

The complete source is [examples/62_core_quickstart.cpp](examples/62_core_quickstart.cpp). It registers one C++ node, compiles a strict graph, runs it, and reads a typed channel.

Enable the programmable control plane when needed:

```bash
cmake -S . -B build-program \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON \
  -DNEOGRAPH_BUILD_EXAMPLES=ON
cmake --build build-program --parallel
./build-program/example_program_quickstart
```

See [examples/63_program_quickstart.cpp](examples/63_program_quickstart.cpp) and the [QuickJS authoring boundary](docs/QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md).

### Performance build

Single-config generators such as Ninja and Unix Makefiles do not select an
optimization level when `CMAKE_BUILD_TYPE` is empty. NeoGraph warns about that
configuration because GCC/Clang then compile QuickJS and NeoGraph without the
Release `-O3 -DNDEBUG` flags.

For a local, host-specific performance build on GCC or Clang:

```bash
cmake -S . -B build-performance -G Ninja \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_ENABLE_NATIVE_OPTIMIZATION=ON \
  -DNEOGRAPH_BUILD_BENCHMARKS=ON \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON
cmake --build build-performance --parallel
```

`NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION=ON` adds `-march=native -mtune=native`
to optimized configurations. It improves local throughput but makes the
artifacts non-portable; keep it off for distributable binaries. Release
hardening remains enabled by default.

On GCC/Clang, the resulting Release profile uses C11 for QuickJS and C++20
for NeoGraph, `-O3 -DNDEBUG`, and the default hardening flags
`-D_GLIBCXX_ASSERTIONS`, `-fstack-protector-strong`,
`-fcf-protection=full`, and Linux `-D_FORTIFY_SOURCE=2` plus RELRO/NOW
linking. LTO and host-specific tuning are not enabled by default.

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
## Typed C++ provider calls

`SchemaProvider` accepts an admitted `sp::descriptor::ValidatedDescriptor`, `sp::runtime::Options` and optional `SchemaProvider::Defaults`. Descriptor loading is closed/versioned data admission, not a request/response interpreter or arbitrary primitive registry. Credentials belong in runtime options, not public descriptor files. Defaults contain only typed OpenRouter routing and Responses retention (`responses_store`); the latter is valid only for Responses. Hosted OpenRouter routing, retention and JSON formats remain declared typed controls. Images, Veo and Decisions use separate NeoGraph typed clients and separate authorization; they do not inherit an SDK chat grant.

```cpp
#include <neograph/llm/schema_provider.h>
#include <neograph/types.h>

sp::runtime::Result first_call(
    sp::descriptor::ValidatedDescriptor descriptor, sp::runtime::Options options,
    std::string model) {
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

A provider call returns `sp::runtime::Result`: an immutable, owned `std::shared_ptr<const sp::Outcome>`, containing `sp::Completion` or `sp::Failure`. Retain the whole outcome, not only display text. Ordered messages/parts, native continuation, complete wire envelopes, ordered raw observations, stop evidence and genuine attempt metadata survive the call and client destruction. Usage counters are nullable `uint64_t` values with evidence, stage and quality: missing is unknown, never zero. A failure retains its original partial outcome. `ProviderFailure::outcome()` and `ProviderObserverError::outcome()` preserve that result; the latter also preserves the observer exception in `cause()`.

`ChatMessage` / `ChatTool` and JSON are portable projections, not native authority. Current portable formats are [`provider-message-v2`](schemas/provider-message-v2.schema.json) and [`runtime-history-record-v2`](schemas/runtime-history-record-v2.schema.json). Genuine C++ in-memory checkpoint sidecars retain native seals without an archive. Durable native history and bank references require a real `sp::NativeArchive`: closed v2 / `spna2`, authenticated owner-private protected custody with an independent key, not encryption and not vendor-issuer authentication. Never publish archive bodies, keys, native blobs or raw wire observations. Managed recovery/forks share the canonical charged/reserved/report/dedup bank without renewal. Generic bounded durable forks require an external host-shared bank and journal; copied snapshots cannot grant independent sp…


If post-effect accounting or terminal-receipt persistence fails after a real result exists, `ProviderDispatchOutcomePersistenceError` retains the original immutable result in `outcome()` and the original persistence exception in `cause()`. If delivery also failed, `delivery_error()` retains the original observer exception. Successful persistence followed by observer failure rethrows that original observer exception unchanged; an unknown/no-result transport failure does not fabricate an outcome.
This is a source and binary break: recompile every C++ consumer and custom provider with matching new headers/libraries. `CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, the descriptor interpreter and Responses WebSocket path are removed, with no aliases or compatibility bridges. The SDK is unstable `0.0.0`, interface revision 3 / shared ABI 3, with out-of-line capability checks; that is not a stable release claim. Current runtime/archive support is Linux/POSIX; no Windows, macOS or WASM runtime qualification is implied. Python provider bindings/wrappers are deferred and not ported by this C++ change.

## Python

> The Python material below describes existing bindings; provider bindings/wrappers are explicitly deferred and have not been ported or exercised for the typed lossless C++ cutover. Installing a historical wheel does not expose the new C++ provider API.
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

Core-only builds still omit Program and QuickJS, but no longer omit SchemaProvider runtime:

```bash
cmake -S . -B build-core \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DNEOGRAPH_BUILD_PROGRAM=OFF \
  -DNEOGRAPH_BUILD_LLM=OFF \
  -DNEOGRAPH_BUILD_MCP=OFF
```

Important options:

| Option | Purpose |
|---|---|
| `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` | Explicit SDK source checkout; otherwise an installed runtime package is required. |
| `NEOGRAPH_BUILD_PROGRAM` | Durable Program values, catalog, runtime, lineage and migration |
| `NEOGRAPH_BUILD_QUICKJS_CONTROL` | QuickJS Program authoring and generator commands |
| `NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION` | Opt into non-portable host-specific instruction tuning for optimized configurations |
| `NEOGRAPH_WARN_ON_UNOPTIMIZED_SINGLE_CONFIG` | Warn when a single-config build omits `CMAKE_BUILD_TYPE` and would miss Release optimization flags |
| `NEOGRAPH_BUILD_PYBIND` | `neograph-engine` Python extension |
| `NEOGRAPH_BUILD_SQLITE` | SQLite checkpoint, context, Hook and provider-receipt stores |
| `NEOGRAPH_BUILD_POSTGRES` | PostgreSQL checkpoint and Program persistence components |
| `NEOGRAPH_BUILD_MCP_CLIENT` / `SERVER` | MCP client and server roles |
| `NEOGRAPH_BUILD_A2A` / `ACP` / `GRPC` | Optional protocol integrations |

Use the narrow CMake target matching your deployment: `neograph::core`, `neograph::llm`, `neograph::program`, `neograph::mcp`, `neograph::a2a`, or another enabled component.

The SDK imported target supplies its `include/SchemaProvider` include root; public examples use `<descriptor/descriptor.h>`, `<runtime/client.h>` and `<neograph/llm/schema_provider.h>` directly, without recipe-only helpers.

```cmake
find_package(SchemaProvider CONFIG REQUIRED COMPONENTS runtime)
find_package(NeoGraph CONFIG REQUIRED)
target_link_libraries(app PRIVATE neograph::core neograph::llm SchemaProvider::runtime)
```

## Verification

`scripts/test_find_package.sh` describes installed-consumer checks; its existence is not a current pass claim. The current SDK ABI3 full rebuild/CTest passed 26/26, and the shared installed consumer exercised real local HTTP two-turn typed requests, tool/native/refusal/known-zero outcomes and mismatch rejection. These results do not qualify NeoGraph, Python, Windows, macOS, WASM or paid live-provider compatibility. NeoGraph integrated verification is reported separately.

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
