# Migration Guide: Legacy 8 virtuals → `run(NodeInput)` (v0.4.x → v0.9+)

NeoGraph v0.4 consolidated the node entry point into a single `run(NodeInput) ->
awaitable<NodeOutput>`. The legacy 8 virtuals (`execute` / `execute_async` /
`execute_stream` / `execute_stream_async` and their `_full` counterparts) were
deprecated in v0.4.x and removed in v0.9.0, the v1 preparation release. This
document outlines the procedure for migrating legacy nodes to the current API.

> In v0.9.0 and later, C++ subclasses that do not implement `run(NodeInput)` will
> fail to compile as abstract classes. Python subclasses must also implement
> `run(self, input)`.

## Why Migrate

Old pattern — `(sync/async) × (writes/full) × (stream/non-stream)` = 8 virtual
cross-product. Overriding any single one causes the other 7 to fall back to the
default chain. Some combinations are safe, but there are runtime pitfalls (e.g.,
sync `execute_full` + async dispatch → nested `run_sync` race), making it
unclear which function the user should override.

New pattern — a single `run(NodeInput) -> awaitable<NodeOutput>`. Override only
one. The sync vs async distinction is the caller's concern (users can use
`co_await` inside coroutines or plain synchronous code freely). Command / Send
are included in `NodeOutput`, so no additional virtuals are needed. Streaming
callbacks arrive via `NodeInput::stream_cb` (a nullable pointer).

## 8 virtuals → New `run()` Mapping

| Legacy Virtual | Migrated Form |
|---|---|
| `execute(state)` | `NodeOutput out; out.writes = {...}; co_return out;` (sync body) |
| `execute_async(state)` | Native async like `co_return co_await provider->complete_async(...);` |
| `execute_stream(state, cb)` | `if (in.stream_cb) (*in.stream_cb)(event); co_return NodeOutput{...};` |
| `execute_stream_async(state, cb)` | Above + native async (`co_await ...`) |
| `execute_full(state)` | `NodeOutput out; out.writes=...; out.command=...; co_return out;` |
| `execute_full_async(state)` | Above + native async |
| `execute_full_stream(state, cb)` | `execute_full` + `in.stream_cb` usage |
| `execute_full_stream_async(state, cb)` | Above + native async |

Key: **The 8 variants are expressible as combinations of which `NodeOutput` field
is populated + whether `in.stream_cb` is used + whether `co_await` is used**.
Only one virtual remains.

### Most Common Python Migration

**Old code:**

```python
class CounterNode(ng.GraphNode):
    def execute(self, state):
        current = state.get("count") or 0
        return [ng.ChannelWrite("count", current + 1)]
```

**Current code:**

```python
class CounterNode(ng.GraphNode):
    def run(self, input):
        current = input.state.get("count") or 0
        return [ng.ChannelWrite("count", current + 1)]
```

Python's `run` is a regular `def`, not `async def`. In streaming execution,
`input.stream_cb` is a function that receives events; in regular execution, it
is `None`.

## Case-by-Case Conversion Examples

### Case 1 — Simplest Sync Node

**Old:**
```cpp
class MyNode : public GraphNode {
public:
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        int n = state.get("counter").get<int>();
        return {ChannelWrite{"counter", json(n + 1)}};
    }
    std::string get_name() const override { return "my_node"; }
};
```

**New:**
```cpp
class MyNode : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        int n = in.state.get("counter").get<int>();
        NodeOutput out;
        out.writes.push_back({"counter", json(n + 1)});
        co_return out;
    }
    std::string get_name() const override { return "my_node"; }
};
```

Differences:
- `state` → `in.state`
- Return value wrapped in `NodeOutput` (`writes` field)
- Function is `asio::awaitable<NodeOutput>` and ends with `co_return`

### Case 2 — Async LLM Node (Migrating `execute_async`)

**Old:**
```cpp
class TalkNode : public GraphNode {
    std::shared_ptr<Provider> prov_;
public:
    asio::awaitable<std::vector<ChannelWrite>>
    execute_async(const GraphState& state) override {
        auto reply = co_await prov_->complete_async({
            .messages = state.get_messages(),
            .model    = "gpt-mock",
        });
        co_return std::vector<ChannelWrite>{
            {"reply", json(reply.message.content)}
        };
    }
    std::string get_name() const override { return "talk"; }
};
```

**New:**
```cpp
class TalkNode : public GraphNode {
    std::shared_ptr<Provider> prov_;
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto reply = co_await prov_->complete_async({
            .messages = in.state.get_messages(),
            .model    = "gpt-mock",
        });
        NodeOutput out;
        out.writes.push_back({"reply", json(reply.message.content)});
        co_return out;
    }
    std::string get_name() const override { return "talk"; }
};
```

### Case 3 — Streaming Node (Migrating `execute_stream`)

**Old:**
```cpp
std::vector<ChannelWrite>
execute_stream(const GraphState& state, const GraphStreamCallback& cb) override {
    auto reply = prov_->complete_stream(params, [&](const std::string& chunk) {
        cb({GraphEvent::Type::LLM_TOKEN, "talk", json(chunk)});
    });
    return {ChannelWrite{"reply", json(reply.message.content)}};
}
```

**New:**
```cpp
asio::awaitable<NodeOutput> run(NodeInput in) override {
    // in.stream_cb is a pointer — null means the caller does not want streaming.
    auto on_chunk = [&](const std::string& chunk) {
        if (in.stream_cb) {
            (*in.stream_cb)({GraphEvent::Type::LLM_TOKEN, "talk", json(chunk)});
        }
    };
    auto reply = prov_->complete_stream(params, on_chunk);
    NodeOutput out;
    out.writes.push_back({"reply", json(reply.message.content)});
    co_return out;
}
```

### Case 4 — Node Using Command / Send (Migrating `execute_full`)

**Old:**
```cpp
NodeResult execute_full(const GraphState& state) override {
    NodeResult r;
    r.writes.push_back({"step", json("dispatched")});
    Command command;
    command.goto_node = "next_router";
    r.command = command;   // Force routing
    return r;
}
```

**New:**
```cpp
asio::awaitable<NodeOutput> run(NodeInput in) override {
    NodeOutput out;   // NodeOutput == NodeResult — alias of the same type
    out.writes.push_back({"step", json("dispatched")});
    Command command;
    command.goto_node = "next_router";
    out.command = command;
    co_return out;
}
```

`NodeOutput` is an alias for `NodeResult` — legacy `NodeResult` code still
compiles.

## Common Mistakes

### `NodeInput in` is by-value

```cpp
// ❌ Wrong — coroutine ref-param UAF, SEGV in pybind async path
asio::awaitable<NodeOutput> run(const NodeInput& in) override { ... }

// ✅ Correct
asio::awaitable<NodeOutput> run(NodeInput in) override { ... }
```

Reason: The coroutine frame must take an argument copy for safety. Receiving by
reference leaves `in.state` dangling after the caller's stack frame disappears.
A real bug that occurred during PR 2 work.

### cancel / store / stream_cb all come from `in.ctx`

Legacy nodes received cancel tokens via smuggling channels like
`state.run_cancel_token_`, but v0.4 introduced `RunContext` as official plumbing:

```cpp
asio::awaitable<NodeOutput> run(NodeInput in) override {
    // Check cancellation signal
    if (in.ctx.cancel_token && in.ctx.cancel_token->is_cancelled()) {
        throw CancelledException("user cancelled");
    }

    // Store access (issue #27)
    if (in.ctx.store) {
        auto user_pref = in.ctx.store->get({"users", in.ctx.thread_id}, "lang");
        // ...
    }

    // Streaming sink (nullable)
    if (in.stream_cb) {
        (*in.stream_cb)({GraphEvent::Type::NODE_END, "my_node", json(...)});
    }

    co_return NodeOutput{};
}
```

Fields of `in.ctx` available to nodes are: `cancel_token`, `usage`,
`thread_id`, `step`, `stream_mode`, `store`, and `resume_value`. `deadline` and
`trace_id` are reserved slots for future `RunConfig` extensions; the current
engine does not populate them and does not expose them to Python.

### Migrating `_full` virtuals — finish with `co_return out;` in one line

The most common confusion for legacy `execute_full` users:
"`NodeResult` is the old type, but must I return `NodeOutput`?"
→ They are aliases of the same type. Just do `NodeOutput out;
out.writes=...; out.command=...; out.sends=...; co_return out;`.

## What Happens If You Don't Migrate

In v0.9.0 and later, the legacy 8 virtuals are gone.

- C++ legacy `override` generates compilation errors like `'execute' marked
  override but does not override`.
- Python nodes implementing only `execute()` raise `NotImplementedError`
  requiring `run(input)`.

Do not use a transition pattern that leaves the old method names in place. The
engine calls only `run(NodeInput)`, so old bodies never execute.

## Is There a Bulk Migration Script?

No — the virtual signatures vary across 8 forms, making regex-based conversion
impractical. Users should read the case-by-case examples (the 4 examples above)
and migrate manually.

For the most common pattern (override only `execute(state)`), the following
sed/awk one-liner may assist with the initial pass — human review is required:

```bash
# Very rough initial pass — nodes with single-line execute override only.
# Always dry-run without -i first.
grep -lE 'execute\(const GraphState' src/**/*.cpp
# Manually edit each resulting file to the new pattern.
```

Complex nodes (`execute_full`, `execute_stream_async`, etc.) must be edited
manually. There are no shortcuts.

---

# Migration 2: `Provider` Compatibility Policy and New Explicit Request API (v0.9+)

The existing `Provider::complete*` four methods and callback-based `invoke()` are
stable APIs with no removal plans. Existing implementations and callers need not
migrate. However, new `Provider` implementations should override only
`CompletionProvider::do_invoke()`, and new direct calls should use
`invoke_request(CompletionRequest)`.

## Existing vs Recommended New Call Patterns

| Stable Compatible API | Recommended API When Using `CompletionProvider` Directly |
|---|---|
| `complete(params)` | `run_sync(invoke_request(CompletionRequest::collect(params)))` |
| `complete_async(params)` | `co_await invoke_request(CompletionRequest::collect(params))` |
| `complete_stream(params, on_chunk)` | `run_sync(invoke_request(CompletionRequest::stream(params, on_chunk)))` |
| `complete_stream_async(params, on_chunk)` | `co_await invoke_request(CompletionRequest::stream(params, on_chunk))` |

`CompletionRequest` separates callback presence from transport mode. Thus, even
without a callback, `CompletionRequest::stream(params)` explicitly requests
streaming transport. Code that holds only `Provider&` or `Provider*` can use
existing `complete*` methods unchanged.

## Case-by-Case Conversion

### User Code Calling Provider

```cpp
// Stable compatible API — continues to be supported
auto completion = co_await provider->complete_async(params);
```

```cpp
// New code using `CompletionProvider` directly — mode is explicit
auto completion = co_await provider.invoke_request(
    CompletionRequest::collect(params));
```

### Custom Provider Subclass

Existing `Provider` subclasses continue to work. New implementations should
inherit from `CompletionProvider` and implement only `do_invoke()`. The
existing `Provider` vtable and Python `complete()` contract remain unchanged.

```cpp
// Old pattern — async-native provider
class MyProvider : public neograph::Provider {
public:
    asio::awaitable<ChatCompletion>
    complete_async(const CompletionParams& params) override {
        // ... HTTP call ...
        co_return result;
    }

    ChatCompletion complete_stream(const CompletionParams& params,
                                   const StreamCallback& on_chunk) override {
        // ... SSE call ...
        return result;
    }

    std::string get_name() const override { return "my"; }
};
```

```cpp
// New pattern — transport mode is decoupled from callback presence
class MyProvider : public neograph::CompletionProvider {
public:
    asio::awaitable<ChatCompletion>
    do_invoke(CompletionRequest request) override {
        if (request.streaming()) {
            // Uses SSE/WS transport even without callback.
            // With callback, invokes request.on_chunk()(token) per token.
        } else {
            // ... HTTP call ...
        }
        co_return result;
    }

    std::string get_name() const override { return "my"; }
};
```

New callers specify the mode explicitly.

```cpp
auto full = co_await provider.invoke_request(
    CompletionRequest::collect(params));

auto streamed = co_await provider.invoke_request(
    CompletionRequest::stream(params, on_chunk));

// Can explicitly request streaming transport even without observing tokens.
auto streamed_without_observer = co_await provider.invoke_request(
    CompletionRequest::stream(params));
```

The legacy four virtuals and `invoke(params, on_chunk)` remain supported. The
`CompletionProvider` final adapter connects all legacy entry points to
`do_invoke()` once, preventing mutual recursion.

## Automatic Cancellation Propagation

If cancellation is needed, specify `CompletionParams::cancel_token`. Internal
engine nodes pass the `RunContext` token to providers in params. The thread-local
implicit propagation path is no longer used.

```cpp
// Node body inside engine — both cancel identically
co_await provider->invoke(params, nullptr);                    // OK
neograph::async::run_sync(provider->invoke(params, nullptr));  // OK
```

Direct callers outside the graph (e.g., `Agent` user code) follow the same
pattern — without an explicit cancel_token, cancellation cannot be received
(same as before).

## What Happens If You Don't Migrate

No action is required:
- Legacy virtual overrides and direct calls continue to work.
- Provider-related `-Wdeprecated-declarations` warnings no longer occur.
- Compatibility and security fixes apply to legacy APIs as well.

There are no plans to remove legacy APIs. However, new features may only be
added to the explicit request contract, so implementing new features with
`CompletionProvider` is preferable.

## Can It Be Automatically Converted?

Call sites follow simple patterns:

```bash
# Review with dry-run
grep -rnE '->complete(_async|_stream|_stream_async)?\(' your/code

# Then manually edit case-by-case (refer to the mapping table above).
```

Merging legacy `Provider` subclasses into a single `CompletionProvider::do_invoke()`
is optional and requires manual editing due to intertwined logic.

## Related Documentation / Issues

- [`include/neograph/graph/node.h`](../include/neograph/graph/node.h) — inline
  docstring for the new `run(NodeInput)` virtual (includes examples)
- [ROADMAP_v1.md](../ROADMAP_v1.md) — Detailed design notes for Candidate 1
  (GraphNode 8-virtual flattening)
- [troubleshooting.md](troubleshooting.md) — Compilation errors and runtime
  differences encountered during actual migration
- [Issue #5](https://github.com/fox1245/NeoGraph/issues/5) — Record of decision
  on Provider method implementation path and permanent compatibility policy

---

# Migration 3: `compile()` worker pool default is 1 (v0.1.4 regression restoration)

## What Changed

`GraphEngine::compile(def, ctx)` default worker count was
`std::thread::hardware_concurrency()` from v0.1.4 (`b59444f`) but is restored to
**`1` (= no engine-owned thread_pool)** in v1.0.

## Why

The `hardware_concurrency` default imposes cross-thread submit overhead (~6-7
µs/task) on all fan-out nodes — bench par measurement (5 workers + summarizer)
regressed from 11.6 µs to 44 µs, a 4× slowdown. Bisecting our measurement
environment pinpointed v0.1.4's `b59444f` as the culprit.

Real production workloads (LLM calls in ms~s range) can ignore submit overhead,
but:
- **Simple graphs (no fan-out)** still pay pool overhead — meaningless
- **Non-thread-safe node state** is exposed to multi-worker by default — a real
  footgun

Thus, the default is safely set to 1, and users must explicitly opt-in for
actual fan-out parallelization.

## Migration

Graphs requiring fan-out parallelization (e.g., multiple `Send` dispatches,
`parallel_group`, deep_research's 5-researcher fan-out) must call explicitly
after `compile()`:

```cpp
auto engine = GraphEngine::compile(def, ctx);
engine->set_worker_count_auto();  // hardware_concurrency()
// or
engine->set_worker_count(4);  // specify exact N
```

```python
engine = ng.GraphEngine.compile(def, ctx)
engine.set_worker_count_auto()
```

Simple graphs (no fan-out) or light fan-out graphs (LLM call dominant) keep the
default — 0 pool overhead.

## What Happens If You Don't Migrate

- User graphs with fan-out execute serially on a single thread (consistency
  guaranteed)
- Actual wallclock recovery is not achieved — explicit
  `set_worker_count_auto()` is required

## NeoGraph Internal Examples Affected

The fan-out visibility patch added alongside this change — explicit calls added
to preserve intent. Apply the same pattern if your user code matches:

- `examples/10_send_command.cpp` — sync `sleep_for` ResearcherNode fans out via
  Send, `engine->set_worker_count_auto()` added
- `examples/14_plan_executor.cpp` — 5 sub-topic Send fan-out (sync sleep_for),
  same addition
- `examples/21_mcp_fanout.cpp` — 3 MCP tool calls fired concurrently, same
- `examples/36_classifier_fanout.cpp` — already had `set_worker_count(5)`
  explicit. Fixed comment stating false default (current default is
  hardware_concurrency)
- `src/core/deep_research_graph.cpp` `create_deep_research_graph()` builder —
  calls `set_worker_count_auto()` immediately after `compile()` so supervisor's
  N researchers truly run concurrently

`examples/05_parallel_fanout.cpp` uses coroutine timer overlap on `io_context`
(no sync sleep), so worker pool has no effect — left unchanged.

If the same pattern exists in user code:

```cpp
auto engine = GraphEngine::compile(def, ctx);
engine->set_worker_count_auto();   // ← add this line
```

See ROADMAP_v1.md perf section for detailed measurements (separate addition).
