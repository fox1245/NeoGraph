# NeoGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

NeoGraph is a C++20 runtime for stateful workflows described as graphs. A graph defines executable nodes, named state channels, rules for merging writes, and edges that determine what runs next. Nodes can perform ordinary computation, call tools, or request model output. The runtime schedules those nodes, applies their writes, and, when configured with a checkpoint store, saves progress for interruption and resumption. Python bindings expose the same C++ engine.

Consider a research workflow: retrieve documents, extract findings from several documents, combine the findings, and ask a reviewer whether another retrieval round is needed. The documents and findings belong in state channels; retrieval, extraction, and review are nodes; edges select the next stage or return to retrieval. A graph makes those transitions explicit instead of hiding them in a sequence of model prompts. See the [examples](examples/README.md) for research, tool use, human review, and multi-agent workflows.

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

## A worked Core example

The [complete C++ quickstart](examples/62_core_quickstart.cpp) registers an uppercase node and compiles this topology:

```text
__start__ -> upper -> __end__

Input channel:   text = "hello"
Node reads:      "hello"
Node returns:    ChannelWrite{"text", "HELLO"}
Reducer:         overwrite
Output channel:  text = "HELLO"
```

The node's computation is ordinary C++:

```cpp
class UpperNode final : public neograph::graph::GraphNode {
public:
    asio::awaitable<neograph::graph::NodeOutput> run(
        neograph::graph::NodeInput input) override {
        auto text = input.state.get(neograph::graph::ChannelKey<std::string>{"text"});
        for (auto& character : text)
            character = static_cast<char>(
                std::toupper(static_cast<unsigned char>(character)));
        co_return neograph::graph::NodeOutput{{
            neograph::graph::ChannelWrite{"text", neograph::json(std::move(text))}}};
    }
    std::string get_name() const override { return "upper"; }
};
```

The complete source includes the headers, node registration with declared reads/writes, topology, `GraphEngine::build_strict`, run input, and typed output access. No model call or API key is needed. The expected output is `HELLO`.

### Build and run

SchemaProvider is a required external SDK even when `NEOGRAPH_BUILD_LLM=OFF`, because Core exports its typed provider contracts. The commands below use an installed [SchemaProvider runtime package](https://github.com/fox1245/SchemaProvider): set `SCHEMAPROVIDER_PREFIX` to its install prefix. An explicit checkout supplied with `-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=../SchemaProvider` takes precedence; otherwise CMake prefers an installed package and, if none is found, fetches the pinned public SDK archive. Set `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` for an offline build with an installed package or explicit checkout. CMake does not guess a sibling checkout or use the removed bundled interpreter.

Prerequisites include a C++20 compiler, CMake 3.20 or newer, and the SDK's runtime dependencies, including OpenSSL and libcurl 7.88 or newer. Full builds with NeoGraph's HTTPS components require OpenSSL 3. The default build also enables SQLite and PostgreSQL integrations; the command below disables unnecessary NeoGraph components without removing SDK dependencies. Recorded SDK interface 4 checks cover Linux x86_64 and local protocol/state peers; the [SDK conformance record](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/CONFORMANCE.md#interface-4-execution-record) gives their exact scope. They do not establish new Windows, macOS, ARM64, HTTP/3, hosted-vendor or WASM qualification. See [troubleshooting](docs/troubleshooting.md) for platform and build constraints.

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

The [Program quickstart](examples/63_program_quickstart.cpp) compiles and admits a Program that calls an increment graph; its expected output is `1`. It uses in-memory stores and the C++ Program builder. For JavaScript authoring and durable execution, start with the [authoring boundary](docs/QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md), [recursive Programs](docs/PROGRAM_RECURSIVE_HARNESSES.md), and [strict runtime contracts](docs/STRICT_RUNTIME_INTERPOSITION.md).

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

After `first_call` returns, use `std::get_if<sp::Completion>(result.get())` to inspect a completion's `messages`, `stop`, and `usage`. Otherwise, `std::get<sp::Failure>(*result)` provides `error.kind`, `error.safe_message`, retry evidence, and the partial messages and usage in `partial`. For example, an absent `completion.usage.output_total` means the output-token count is unknown; a present counter whose `value` is `0` reports zero. Display text is only one view of the retained outcome.

`ChatMessage`, `ChatTool`, and JSON are portable projections. Authentic native history can stay in memory with its native checkpoint sidecar; durable native history requires a real `sp::NativeArchive` and protected owner-private custody. Portable JSON cannot recreate that authority. The archive authenticates custody with an independent key; it is neither encryption nor vendor-issuer authentication. Do not publish archive bodies, keys, native blobs, or raw wire observations. See the [provider reference](docs/reference-en.md) and [migration guide](docs/migration-v0.4-to-v1.0.md) for persistence failures, observers, managed budget banks, and replay boundaries.

The typed cutover removes `CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, the descriptor interpreter, and the Responses WebSocket path. Recompile C++ consumers and migrate custom providers; there are no compatibility aliases. The SDK package is `0.1.0` with an alpha interface, interface revision 4, and shared ABI 4; matching revisions are required, and these numbers do not declare a stable SDK interface.

## Python

```bash
pip install neograph-engine
```

The typed-provider API described here targets NeoGraph `0.13.0`; historical wheels expose the older interface. The [Python binding guide](docs/python-binding.md) documents the source API and build prerequisites.

This graph needs no API key:

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

The expected message content is `Hello, NeoGraph!`. Python also exposes Program compilation and execution, Hooks, runtime-context requirements, strict profiles, SQLite durability, and exact checkpoint resume. Typed providers use `ProviderMessage` and `make_provider_request`, then `prepare`/`dispatch` or `invoke`; outcomes retain native-owned completion or failure evidence. These messages differ from graph convenience `ChatMessage` values. Blocking provider calls release the GIL; `asyncio.to_thread` can move them off an event-loop thread. See the [Python examples](bindings/python/examples/README.md) for complete provider and Program inputs.

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
| `NEOGRAPH_BUILD_MCP_CLIENT` / `NEOGRAPH_BUILD_MCP_SERVER` | MCP client and server components. |
| `NEOGRAPH_BUILD_A2A` / `NEOGRAPH_BUILD_ACP` / `NEOGRAPH_BUILD_GRPC` | Protocol integrations; gRPC defaults off. |
| `NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION` | Non-portable host-specific tuning for optimized configurations; default off. |

Installed consumers link only the enabled components they need:

```cmake
find_package(SchemaProvider 0.1.0 CONFIG REQUIRED COMPONENTS runtime)
find_package(NeoGraph CONFIG REQUIRED)
target_link_libraries(app PRIVATE neograph::core neograph::llm SchemaProvider::runtime)
```

- [Concepts and graph semantics](docs/concepts.md)
- [C++ reference](docs/reference-en.md) and [Python binding guide](docs/python-binding.md)
- [Async guide](docs/ASYNC_GUIDE.md) and [concurrency/cancellation](docs/concurrency.md)
- [Runtime context and strict interposition](docs/STRICT_RUNTIME_INTERPOSITION.md)
- [Harness MCP](docs/HARNESS_MCP.md) and [QuickJS authoring](docs/QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)
- [Migration guide](docs/migration-v0.4-to-v1.0.md) and [troubleshooting](docs/troubleshooting.md)
- [C++ examples](examples/README.md), [Python examples](bindings/python/examples/README.md), and [benchmark methodology](benchmarks/README.md)

## License

MIT; see [LICENSE](LICENSE). Third-party notices are in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
