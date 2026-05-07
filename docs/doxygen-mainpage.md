# NeoGraph C++ API Reference {#mainpage}

A C++17 graph agent engine library — LangGraph for C++, with optional
Python bindings. This site is the **generated reference** for the
public C++ headers in `include/neograph/`.

## Where to start

If you're new to NeoGraph, **read the narrative docs first** — this
generated reference is for looking up class signatures once you know
what you're looking for.

| For | Go to |
|---|---|
| What NeoGraph is, why, benchmarks | [README](https://github.com/fox1245/NeoGraph#readme) |
| Mental model — channels, nodes, edges, Send, Command | [Core Concepts](https://github.com/fox1245/NeoGraph/blob/master/docs/concepts.md) |
| Symptom-first fixes for common issues | [Troubleshooting](https://github.com/fox1245/NeoGraph/blob/master/docs/troubleshooting.md) |
| 39 runnable C++ programs | [examples/](https://github.com/fox1245/NeoGraph/tree/master/examples) |
| 23 runnable Python programs | [bindings/python/examples/](https://github.com/fox1245/NeoGraph/tree/master/bindings/python/examples) |
| Async / coroutine internals | [ASYNC_GUIDE](https://github.com/fox1245/NeoGraph/blob/master/docs/ASYNC_GUIDE.md) |

## Top-level header

The convenience header pulls in the full core + graph engine API:

```cpp
#include <neograph/neograph.h>

using namespace neograph;
using namespace neograph::graph;
```

Sub-namespaces:

- `neograph`           — foundation types (`Provider`, `Tool`, `ChatMessage`)
- `neograph::graph`    — engine, nodes, state, checkpointing
- `neograph::llm`      — provider implementations (OpenAI, schema-driven, agent helper)
- `neograph::mcp`      — Model Context Protocol client
- `neograph::async`    — coroutine + io_context infrastructure
- `neograph::util`     — concurrency primitives

## A first program

```cpp
#include <neograph/neograph.h>
#include <neograph/llm/mock_provider.h>

using namespace neograph;
using namespace neograph::graph;

int main() {
    json definition = {
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes",    {{"echo",     {{"type", "llm_call"}}}}},
        {"edges",    json::array({
            {{"from", "__start__"}, {"to", "echo"}},
            {{"from", "echo"},      {"to", "__end__"}}})}
    };

    NodeContext ctx;
    ctx.provider = std::make_shared<llm::MockProvider>();
    auto engine = GraphEngine::compile(definition, ctx);

    RunConfig cfg;
    cfg.thread_id = "demo";
    cfg.input["messages"] = json::array({{{"role","user"},{"content","hi"}}});
    auto result = engine->run(cfg);
    return 0;
}
```

For real LLM use, swap `MockProvider` for `llm::OpenAIProvider` or
`llm::SchemaProvider`. The full `Provider` interface is at
`neograph::Provider`.

## Reference index

The class list, file list, and namespace list in the sidebar are
generated from headers under `include/neograph/`.
[Class list](annotated.html) is the most useful entry point.

## Source

Project home: <https://github.com/fox1245/NeoGraph>

License: MIT.
