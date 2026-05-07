# NeoGraph core concepts — a narrative guide

Read this once before diving into the examples. It builds up the
mental model in the order you'd construct one yourself: graph →
channels → nodes → edges → fan-out → routing override →
checkpoints → streaming.

Code samples are Python-side because they're terser; everything maps
1:1 to the C++ API (see [`reference-en.md`](reference-en.md) for class
signatures or [Doxygen](https://fox1245.github.io/NeoGraph/) for the
generated reference).

> **If you've used LangGraph before:** the primitives are intentionally
> the same — channels with reducers, nodes that emit writes, conditional
> edges, `Send`, `Command`, checkpoints. The differences are described in
> [Comparison with LangGraph](../README.md#comparison-with-langgraph) on
> the README. The narrative below assumes nothing.

---

## Table of contents

(Section 8.5 added in v0.6.0 — `Tracing — OpenTelemetry + Phoenix / Langfuse`.
The numbered headings stay 1-9 to keep external docs links stable;
8.5 sits between Streaming and Common pitfalls.)


1. [The big picture](#1-the-big-picture)
2. [Channels & reducers](#2-channels--reducers)
3. [Nodes](#3-nodes)
4. [Edges & conditional routing](#4-edges--conditional-routing)
5. [Send — dynamic fan-out](#5-send--dynamic-fan-out)
6. [Command — routing override + state patch](#6-command--routing-override--state-patch)
7. [Checkpoints, interrupts, HITL](#7-checkpoints-interrupts-hitl)
8. [Streaming events](#8-streaming-events)
9. [Common pitfalls](#9-common-pitfalls)

---

## 1. The big picture

A NeoGraph **graph** is four things:

| Thing | What it is | Defined by |
|---|---|---|
| **Channels** | Named slots in the shared state. Each has a reducer that defines how new writes combine with existing values. | `definition["channels"]` |
| **Nodes** | Functions that read state, emit writes (and optionally `Send` / `Command`). | `definition["nodes"]` |
| **Edges** | Static next-node pointers. | `definition["edges"]` |
| **Conditional edges** | Predicate-driven routing — picks one of several next nodes based on state. | `definition["conditional_edges"]` |

Execution is a **super-step loop**:

```
1. ready_set = nodes routed from __start__
2. while ready_set is not empty:
   a. run all nodes in ready_set (in parallel if the executor allows)
   b. apply each node's writes to state
   c. collect their Send / Command / outgoing-edge signals
   d. plan_next_step → new ready_set
```

A super-step is the unit of parallelism, of checkpointing, and of
streaming events. Two nodes that can both run "now" are the same
super-step; they observe the same input state and their writes
combine via reducers when the step ends.

---

## 2. Channels & reducers

Every piece of state lives in a named channel. Channels persist across
nodes and across super-steps; nodes communicate by writing to them.

### Defining channels

```python
"channels": {
    "messages":  {"reducer": "append"},     # conversation history
    "counter":   {"reducer": "overwrite"},  # latest value wins
    "summary":   {"reducer": "overwrite"},
}
```

### Built-in reducers

| Reducer | New write semantics | Typical use |
|---|---|---|
| `"overwrite"` | New value replaces old. Last-writer-wins on parallel writes. | Single-value scratch (current node, current question, route hint). |
| `"append"` | New list (must be a list!) is concatenated to the existing list. Order: previous-step values first, this-step writes appended in node-execution order. | Conversation messages, search results, fan-out collection. |

> Both reducers are registered in `ReducerRegistry::ReducerRegistry()`
> at engine startup ([`src/core/graph_loader.cpp`](../src/core/graph_loader.cpp)).
> Custom reducers register from C++ via `ReducerRegistry::register_reducer(name, fn)`
> or from Python (since v0.1.9):
>
> ```python
> ng.ReducerRegistry.register_reducer("sum",
>     lambda current, incoming: (current or 0) + incoming)
> ```
>
> The Python callable runs under the GIL; concurrent Send fan-outs
> serialise on it the same way Python custom nodes do. Re-registering
> a name replaces the previous reducer.

### Writing to channels

A node returns a list of `ChannelWrite`s:

```python
return [
    ng.ChannelWrite("messages", [{"role": "assistant", "content": "Hi!"}]),
    ng.ChannelWrite("counter",  state.get("counter", 0) + 1),
]
```

The shape of the value must match the reducer:
- `"append"` → must be a list (will be concatenated).
- `"overwrite"` → any JSON-serializable value.

### Reading state from a node

```python
def execute(self, state):
    msgs    = state.get("messages") or []     # list of message dicts
    counter = state.get("counter") or 0
    ...
```

`state.get(channel)` returns the channel's current value, or `None` if
the channel exists but hasn't been written to yet. For typed access to
chat messages, `state.get_messages()` returns `list[ChatMessage]`
(parsed from the `messages` channel) — used internally by `llm_call`.

### Versions

Each channel carries a monotonic `version` number. The engine uses
this for checkpoint diffing and for the `state.channel_version(name)`
inspection API. You usually don't read it directly.

---

## 3. Nodes

Three ways to register a node type, in increasing order of control:

### 3.1 Built-in nodes

| `type` (in JSON) | What it does | Configuration |
|---|---|---|
| `llm_call` | Calls `provider->complete_async(messages, tools)` and appends the assistant message to `messages`. | Reads `provider`, `model`, `instructions`, `tools` from `NodeContext`. |
| `tool_dispatch` | Looks at the latest assistant message's `tool_calls`, executes each via `Tool::execute`, appends `{role: "tool", tool_call_id, content}` results. | Reads `tools` from `NodeContext`. |
| `intent_classifier` | LLM classifies user intent into one of N labels and writes the chosen label to `__route__`. Pair with `route_channel` conditional. | `extra_config: {labels, prompt_template}` |
| `subgraph` | Embeds another graph as a single node. Inner state is mapped through configured key remappings. | `extra_config: {graph_def, input_keys, output_keys}` |

### 3.2 The `@ng.node` decorator (Python only)

The shortest way to define a write-only node:

```python
@ng.node("greet")
def greet_node(state):
    name = state.get("name") or "world"
    return [ng.ChannelWrite("messages",
        [{"role": "assistant", "content": f"Hello, {name}!"}])]
```

The decorated function must return a `list[ChannelWrite]` (or `None`,
treated as `[]`). It cannot emit `Send` or `Command` — for those,
subclass `GraphNode`.

### 3.3 The full `GraphNode` subclass

Override `run(input)` for full control. As of v0.4.0 this is the
canonical entry point — one method, one signature:

```python
class Researcher(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        # input.state    — read channels via input.state.get(...)
        # input.ctx      — RunContext (cancel_token, deadline, trace_id, ...)
        # input.stream_cb — non-None when running in streaming mode
        topic = input.state.get("topic")
        result = await_llm(topic, cancel_token=input.ctx.cancel_token)
        return ng.NodeResult(
            writes=[ng.ChannelWrite("findings", [result])],
            command=ng.Command(goto_node="evaluator"),  # optional
            sends=[],                                    # optional
        )
```

You can also return a bare `list[ChannelWrite]` when you don't need
`Send` or `Command` — the binding lifts it into a `NodeResult`
automatically.

> **Migrating from v0.3.x:** the 8-virtual chain (`execute`,
> `execute_async`, `execute_full`, `execute_full_async`,
> `execute_stream`, `execute_stream_async`, `execute_full_stream`,
> `execute_full_stream_async`) still compiles in v0.4.x but is
> `[[deprecated]]` and removed in v1.0.0. Replace with a single
> `run(input)` override; read state from `input.state`, emit tokens
> via `input.stream_cb` when non-None, read the cancel token from
> `input.ctx.cancel_token`.

Register the type so the JSON loader can instantiate it:

```python
ng.NodeFactory.register_type(
    "researcher",
    lambda name, config, ctx: Researcher(name),
)
```

The factory sees `(name, per-node config, NodeContext)` so the same
class can be instantiated under multiple names with different configs.

### 3.4 Tools (separate concept, used by `tool_dispatch`)

`Tool` is not a node — it's something `tool_dispatch` invokes. Subclass
`ng.Tool`, override three methods, pass instances into
`NodeContext(tools=[…])`:

```python
class CalcTool(ng.Tool):
    def get_name(self):       return "calc"
    def get_definition(self): return ng.ChatTool(name="calc", ...)
    def execute(self, args):  return str(args["x"] * 2)
```

The engine takes ownership of the tool list at compile time — your
local references can drop afterwards.

---

## 4. Edges & conditional routing

### Static edges

```python
"edges": [
    {"from": ng.START_NODE, "to": "llm"},
    {"from": "dispatch",    "to": "llm"},
    {"from": "summarizer",  "to": ng.END_NODE},
]
```

Multiple edges from the same source node fan out (every successor goes
into the next super-step's ready set). Two edges to the same target
from one super-step deduplicate to one execution of the target.

### Conditional edges

A conditional edge runs a **named condition** and picks the next node
from the `routes` map:

```python
"conditional_edges": [
    {
        "from": "llm",
        "condition": "has_tool_calls",
        "routes": {"true": "dispatch", "false": ng.END_NODE},
    }
]
```

The condition name resolves to a `ConditionFn` registered in the
engine. Two ship as built-ins:

| Condition | Returns | When to use |
|---|---|---|
| `has_tool_calls` | `"true"` if the latest assistant message has non-empty `tool_calls`; `"false"` otherwise. | ReAct loops — keep dispatching tools until the LLM stops asking. |
| `route_channel` | Whatever string is in the `__route__` channel; falls back to `"default"`. | Pair with `intent_classifier` for explicit intent routing. |

Custom conditions register from C++ via `ConditionRegistry::register_condition(name, fn)`
or from Python (since v0.1.9):

```python
def is_long(state):
    msgs = state.get("messages") or []
    return "long" if len(msgs) > 10 else "short"

ng.ConditionRegistry.register_condition("is_long", is_long)
```

The callable receives the live `GraphState` (so `state.get(channel)` and
`state.get_messages()` work) and must return a string matching one of
the conditional edge's `routes` keys.

### Two equivalent forms — both work since v0.1.8

Conditional edges may live either inside the `edges` array (with a
`condition` field) **or** in a separate `conditional_edges` block.
Both forms are accepted; pick whichever is clearer:

```python
# Form A — top-level (LangGraph parity, recommended for Python)
"edges":             [{"from": "__start__", "to": "llm"}, ...],
"conditional_edges": [{"from": "llm", "condition": "...", "routes": {...}}]

# Form B — inline (used by every C++ example)
"edges": [
    {"from": "__start__", "to": "llm"},
    {"from": "llm", "condition": "...", "routes": {...}},
]
```

> **History:** form A was silently dropped by the graph compiler before
> v0.1.8 — the README and every Python example used it, so ReAct loops
> degenerated to a single LLM call. Fixed in commit `e23a523`. If you
> see this on a wheel ≤ 0.1.7, upgrade.

---

## 5. Send — dynamic fan-out

`Send` is for cases where the number of next-step nodes depends on
state. Classic use: split a list of search topics into N parallel
researcher invocations.

```python
class Planner(ng.GraphNode):
    def execute_full(self, state):
        topics = decide_topics(state)                  # e.g. 5 strings
        return ng.NodeResult(
            writes=[],
            sends=[ng.Send("researcher", {"topic": t}) for t in topics],
        )
```

The engine's `run_sends_async` instantiates `researcher` once per
`Send`, each with its own `state.get("topic")`, and runs them in
parallel via `asio::experimental::make_parallel_group`.

### Mental model

A `Send(target, payload)` is "instantiate `target` with this state
patch and add it to the ready set". The payload is applied as a
state write before the target sees `state`.

After the parallel group finishes, the next super-step's routing comes
from each Send-spawned task's outgoing edges (or its `Command.goto`,
if it emitted one).

### Common shape: fan-out 5, fan-in to summarizer

```
planner ─┬─ Send("researcher", {topic: "A"})  ─┐
         ├─ Send("researcher", {topic: "B"})  ─┤
         ├─ Send("researcher", {topic: "C"})  ─┼─→ summarizer
         ├─ Send("researcher", {topic: "D"})  ─┤
         └─ Send("researcher", {topic: "E"})  ─┘
```

`researcher`'s outgoing edge is just `{"from": "researcher", "to": "summarizer"}`
— same dedup rule as static edges, so summarizer runs once.

### Worker-count tuning

For real parallelism, set the worker count to at least the fan-out
width:

```python
engine.set_worker_count(5)   # match Send count
```

The default is `hardware_concurrency()`. Setting it to 1 puts the
engine on a no-allocate fast path (single super-step, no thread pool)
— useful for benchmarks.

---

## 6. Command — routing override + state patch

`Command` lets a node decide where to go next AND mutate state in the
same return value. It bypasses the regular outgoing edges.

```python
class Evaluator(ng.GraphNode):
    def execute_full(self, state):
        if score(state) >= 0.8:
            return ng.NodeResult(
                writes=[],
                command=ng.Command(
                    goto_node="summarizer",
                    updates=[ng.ChannelWrite("verdict", "accepted")],
                ),
            )
        else:
            return ng.NodeResult(
                writes=[],
                command=ng.Command(
                    goto_node="planner",                  # loop back
                    updates=[ng.ChannelWrite("retries",  state.get("retries", 0) + 1)],
                ),
            )
```

### When to use Command vs conditional edge

- **Conditional edge**: routing depends on a state predicate that
  doesn't need node logic. Cleaner, declarative.
- **Command**: routing depends on logic that's most natural to write
  inside a node — multi-criteria scoring, content inspection, retry
  decisions. Also the only way to atomically update state AND choose
  the next node.

### Last-writer-wins under fan-in

If multiple Commands fire in the same super-step (rare — only
possible when multiple parallel-group siblings emit them), the last
one wins. The order is determined by parallel-group completion, which
is non-deterministic — design around this by ensuring at most one
sibling emits a `Command`.

---

## 7. Checkpoints, interrupts, HITL

### Setting up a checkpoint store

```python
engine.set_checkpoint_store(ng.InMemoryCheckpointStore())
# or: engine.set_checkpoint_store(ng.PostgresCheckpointStore(...))   # if built with PG
```

With a store attached, every super-step writes a checkpoint to the
store keyed on `(thread_id, checkpoint_id)`. The `RunResult.checkpoint_id`
field is the latest one.

### Static interrupt points

```python
"interrupt_before": ["payment"],   # pause before this node runs
"interrupt_after":  ["llm"],       # pause after, before routing
```

The engine returns a `RunResult` with `interrupted=True` and
`interrupt_node` set. To resume:

```python
result = await engine.resume_async(thread_id="t1",
                                   checkpoint_id=result.checkpoint_id,
                                   new_input={...})  # optional
```

### Dynamic interrupts via `NodeInterrupt`

Throw from inside a node body (Python: `raise ng.NodeInterrupt(reason)`,
C++: `throw NodeInterrupt(...)`). The engine catches, persists state,
returns a `RunResult` interrupted at the throwing node — same
resume API.

Useful when the decision to pause depends on intermediate node output
(e.g. "did the LLM produce something worth showing the human?").

### Time travel

`engine.fork(thread_id, from_checkpoint_id)` returns a new thread that
starts from a past checkpoint. Useful for "what if I had answered
differently" branching.

---

## 8. Streaming events

`run_stream` / `run_stream_async` invoke a callback as events fire.
Modes are an OR-able bitmask:

| Mode | Emits |
|---|---|
| `EVENTS` | `NODE_START`, `NODE_END`, `INTERRUPT` |
| `TOKENS` | `LLM_TOKEN` for every streamed token from a `Provider` |
| `DEBUG` | `__routing__` events showing the next-ready set |
| `VALUES` | `__state__` events with full state after every super-step |
| `UPDATES` | `CHANNEL_WRITE` events per `ChannelWrite` |
| `ALL` | All of the above |

```python
def cb(event):
    print(event.type, event.node_name, event.data)

engine.run_stream(
    ng.RunConfig(thread_id="t", input={...},
                 stream_mode=ng.StreamMode.EVENTS),
    cb)
```

> **Note:** `event.node_name` (not `event.node`). The C++ struct field
> is `node_name`; pybind preserves the original name.

For chat-shaped streaming (LangChain-compatible message dicts with
incremental `content_so_far`), use the helper:

```python
from neograph_engine import message_stream

engine.run_stream(
    ng.RunConfig(thread_id="t", input={...},
                 stream_mode=ng.StreamMode.TOKENS),
    message_stream(lambda chunk: print(chunk["content"], end="", flush=True)))
```

---

## 8.5. Tracing — OpenTelemetry + Phoenix / Langfuse

Same callback shape as streaming, different consumer. Pass an OTel
tracer-emitting callback into `engine.run_stream(cfg, cb)` and every
`NODE_START` / `NODE_END` / `ERROR` / `INTERRUPT` event becomes a
span.

Two layers ship in-tree:

  - `neograph_engine.tracing.otel_tracer` — vendor-neutral OTel
    spans. Spans flow to any OTel backend (Jaeger, Tempo, Honeycomb,
    Datadog).
  - `neograph_engine.openinference` — LLM-shape attribute layer
    that turns the same spans into a *LangSmith-style chat-bubble
    trace* in Phoenix / Arize / Langfuse:

```python
from opentelemetry import trace
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import BatchSpanProcessor
from opentelemetry.exporter.otlp.proto.grpc.trace_exporter import OTLPSpanExporter
from neograph_engine.openinference import OpenInferenceProvider, openinference_tracer

trace.set_tracer_provider(TracerProvider())
trace.get_tracer_provider().add_span_processor(
    BatchSpanProcessor(OTLPSpanExporter(endpoint="http://localhost:4317", insecure=True)))
tracer = trace.get_tracer("my-app")

# Wrap the provider — every Provider.complete() now emits an LLM-kind span.
wrapped = OpenInferenceProvider(real_provider, tracer)
ctx = ng.NodeContext(provider=wrapped)
engine = ng.GraphEngine.compile(graph_def, ctx)

with openinference_tracer(tracer) as cb:
    engine.run_stream(ng.RunConfig(input={"messages": [...]}), cb)
```

Spin up Phoenix once: `docker run -d -p 6006:6006 -p 4317:4317
arizephoenix/phoenix`. Open http://localhost:6006 — the trace
renders as a chain (`graph.run` → `node.X` → `llm.complete`) with
prompt / response / token counts visible in the LLM detail pane.
Same code, swap the OTLP endpoint URL for Langfuse self-host and
the trace shows up there with the same shape.

This is the answer to *"NeoGraph doesn't have LangSmith"* — you
get the LangSmith UX (chat bubbles, DAG hierarchy, token cost) by
running Phoenix or Langfuse locally with one Docker command. No
SaaS contract, no per-trace pricing.

See `docs/reference-en.md` §10.5 for the attribute-key schema and
the trade-off note between `otel_tracer` and `openinference_tracer`.

---

## 9. Common pitfalls

These have all been hit by real users; cross-referenced from
[`docs/troubleshooting.md`](troubleshooting.md).

### "My ReAct loop only runs once"

You're on wheel ≤ 0.1.7. The graph compiler dropped the
`conditional_edges` block silently. Upgrade to ≥ 0.1.8. Verify with
`result.execution_trace == ['llm', 'dispatch', 'llm']` (not just
`['llm']`).

### "Provider call hangs for 60 seconds and then errors"

You're on wheel ≤ 0.1.6. The bundled OpenSSL hardcodes RHEL CA paths
that don't exist on Ubuntu / Debian / macOS. Upgrade to ≥ 0.1.7
(auto-sets `SSL_CERT_FILE` to certifi's bundle on import) or set
`SSL_CERT_FILE` manually.

### "My fan-out is slower than I expected"

`set_worker_count(N)` where N matches your Send fan-out width. Default
is `hardware_concurrency()`, but Python custom nodes see GIL contention
on small fan-outs — bench with both 1 and N.

### "RunResult has no .status / .final_state attribute"

It doesn't. Use `result.output`, `result.interrupted`,
`result.execution_trace`. See the table in the README's
"Reading the output" section.

### "Unknown reducer: <name>"

Two reducers ship: `overwrite` and `append`. Custom reducers require
`ReducerRegistry::register_reducer` from C++ (no Python hook yet).

### "The condition is registered but my conditional edge doesn't fire"

Verify the form is one the loader accepts (form A or form B from
[§4](#4-edges--conditional-routing)) — both work since v0.1.8. On
older wheels, only form B works.

### "execution_trace shows only the start node"

Routing fell through to `__end__`. Most likely a missing edge from
your start node, or your conditional returned a value not in the
`routes` map (in which case the engine takes the
lexicographically-last route as a fallback — surprise factor).

---

## Where to next

- [Python examples](../bindings/python/examples/) — 21 self-contained
  scripts covering every concept above.
- [C++ examples](../examples/) — 36 programs with the same structure.
- [`reference-en.md`](reference-en.md) — exhaustive class-by-class API.
- [Doxygen](https://fox1245.github.io/NeoGraph/) — generated reference
  for the C++ headers.
- [`ASYNC_GUIDE.md`](ASYNC_GUIDE.md) — deep dive on the async / coroutine
  layer.
