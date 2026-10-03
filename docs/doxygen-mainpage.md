# NeoGraph C++ API Reference {#mainpage}

**Languages:** [English](doxygen-mainpage.md) | [한국어](doxygen-mainpage.ko.md) | [日本語](doxygen-mainpage.ja.md) | [简体中文](doxygen-mainpage.zh-CN.md)

A C++20 graph agent engine library — LangGraph for C++, with optional
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
| C++ examples (verification reported separately) | [examples/](https://github.com/fox1245/NeoGraph/tree/master/examples) |
| Python examples (provider port deferred) | [bindings/python/examples/](https://github.com/fox1245/NeoGraph/tree/master/bindings/python/examples) |
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
- `neograph::llm` — `SchemaProvider`, `Agent`; typed SDK runtime
- `neograph::mcp`      — Model Context Protocol client
- `neograph::async`    — coroutine + io_context infrastructure
- `neograph::util`     — concurrency primitives

## A first program

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

`SchemaProvider` accepts an admitted `sp::descriptor::ValidatedDescriptor`, `sp::runtime::Options` and optional `SchemaProvider::Defaults`. Descriptor loading is closed/versioned data admission, not a request/response interpreter or arbitrary primitive registry. Credentials belong in runtime options, not public descriptor files. Defaults contain only typed OpenRouter routing and Responses retention (`responses_store`); the latter is valid only for Responses. Hosted OpenRouter routing, retention and JSON formats remain declared typed controls. Images, Veo and Decisions use separate NeoGraph typed clients and separate authorization; they do not inherit an SDK chat grant.

A provider call returns `sp::runtime::Result`: an immutable, owned `std::shared_ptr<const sp::Outcome>`, containing `sp::Completion` or `sp::Failure`. Retain the whole outcome, not only display text. Ordered messages/parts, native continuation, complete wire envelopes, ordered raw observations, stop evidence and genuine attempt metadata survive the call and client destruction. Usage counters are nullable `uint64_t` values with evidence, stage and quality: missing is unknown, never zero. A failure retains its original partial outcome. `ProviderFailure::outcome()` and `ProviderObserverError::outcome()` preserve that result; the latter also preserves the observer exception in `cause()`.

This is a source and binary break: recompile every C++ consumer and custom provider with matching new headers/libraries. `CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, the descriptor interpreter and Responses WebSocket path are removed, with no aliases or compatibility bridges. The SDK is unstable `0.0.0`, interface revision 3 / shared ABI 3, with out-of-line capability checks; that is not a stable release claim. Current runtime/archive support is Linux/POSIX; no Windows, macOS or WASM runtime qualification is implied. Python provider bindings/wrappers are deferred and not ported by this C++ change.


If post-effect accounting or terminal-receipt persistence fails after a real result exists, `ProviderDispatchOutcomePersistenceError` retains the original immutable result in `outcome()` and the original persistence exception in `cause()`. If delivery also failed, `delivery_error()` retains the original observer exception. Successful persistence followed by observer failure rethrows that original observer exception unchanged; an unknown/no-result transport failure does not fabricate an outcome.
## Reference index

The class list, file list, and namespace list in the sidebar are
generated from headers under `include/neograph/`.
[Class list](annotated.html) is the most useful entry point.

## Source

Project home: <https://github.com/fox1245/NeoGraph>

License: MIT.
