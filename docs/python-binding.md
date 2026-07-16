# Python Binding

NeoGraph ships as a `pip`-installable Python package, so the same
C++ engine can drive a LangGraph-style workflow from a Jupyter
notebook, a Gradio app, or a FastAPI service:

```bash
pip install neograph-engine
```

## Five-second demo (no API key)

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

## ReAct agent with a real LLM

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

## Reading the output

`engine.run(...)` returns a `RunResult` with these fields:

| Field | Type | Meaning |
|---|---|---|
| `output` | `dict` | Final state — `{"channels": {...}, "global_version": int}`. Use `output["channels"][name]["value"]` to read a channel. |
| `interrupted` | `bool` | `True` if the run paused at an `interrupt_before` / `interrupt_after` / `NodeInterrupt`. |
| `interrupt_node` | `str` | Name of the node that triggered the interrupt (when `interrupted`). |
| `interrupt_value` | `dict` | `{"reason": str, "type": "NodeInterrupt", "value": ...}` for a dynamic interrupt (`"value"` present only when the node attached a payload), or `{"message": ...}` for a static `interrupt_before` / `interrupt_after`. |
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
| `cancel_token` | `None` | Optional `CancelToken` for cooperative cancellation. Assign one before `engine.run()`, then call `token.cancel()` from another Python thread. The engine stops at the next super-step boundary; nodes doing long work should poll `input.ctx.cancel_token`. |

```python
token = ng.CancelToken()
config = ng.RunConfig(thread_id="job-42", input={"query": "..."})
config.cancel_token = token

# Run engine.run(config) in a worker thread, then request cancellation from
# the caller thread when the request disconnects or the user presses Stop.
token.cancel()
```

## Pausing for a human, from a Python node

`interrupt_before` in the graph definition pauses at a node you picked when you
wrote the graph. That cannot express the case human-in-the-loop actually exists
for, because whether a step is dangerous depends on what the model just asked
for:

> *"The agent wants to run `rm -rf build/`. Allow?"*

For that, the node itself decides — it raises `NodeInterrupt`, attaching what
needs approving. The engine checkpoints and hands you back a normal `RunResult`
(nothing is raised at you), and your answer travels back to the node that asked:

```python
import neograph_engine as ng

class ApprovalNode(ng.GraphNode):
    def run(self, input):
        # The human's answer. None until someone has actually answered — which
        # is how you tell "nobody has looked yet" from "the answer was no".
        verdict = input.ctx.resume_value

        if verdict is None:
            raise ng.NodeInterrupt(
                {"tool": "shell", "cmd": "rm -rf build/"},
                reason="shell command needs approval")

        if not verdict.get("approved"):
            return [ng.ChannelWrite("result", "refused")]
        return [ng.ChannelWrite("result", "done")]

    def get_name(self):
        return "risky"
```

```python
result = engine.run(cfg)

if result.interrupted:
    print(result.interrupt_node)               # "risky"      — which node paused
    print(result.interrupt_value["reason"])    # for a human to read
    print(result.interrupt_value["value"])     # for your code to branch on

    result = engine.resume(cfg.thread_id, {"approved": True})   # the answer
```

`NodeInterrupt(reason)` with a plain string works too, and omits the `"value"`
key. Anything else you raise stays an error: a bug in a node fails the run
loudly rather than looking like a question for a human.

Requires a checkpoint store — there is nothing to resume from otherwise.

## Remembering the user across conversations — the Store

A checkpoint remembers **one conversation**. A Store remembers **the user**,
across all of them.

```python
store = ng.InMemoryStore()
engine.set_store(store)

class Greet(ng.GraphNode):
    def run(self, input):
        seen = input.ctx.store.get(["users", "u1"], "visits")
        n = (seen.value["n"] if seen else 0) + 1
        input.ctx.store.put(["users", "u1"], "visits", {"n": n})
        return [ng.ChannelWrite("greeting", f"visit #{n}")]
```

Namespaces are hierarchical lists, so `store.search(["users"])` finds everything
under every user, and `store.search(["users", "u1"])` finds one user's items.
`get()` returns `None` for a miss — absence is an answer, not an error.

Subclass `ng.Store` to put it in a database instead.

Custom checkpoint persistence works the same way: subclass
`ng.CheckpointStore` and implement `save`, `load_latest`, `load_by_id`, `list`,
and `delete_thread`. The optional `put_writes`, `get_writes`, and
`clear_writes` methods default to no-op/full-super-step replay behavior. Values
inside `StoreItem`, `Checkpoint`, and `PendingWrite` are ordinary Python JSON
shapes (`dict`, `list`, strings, numbers, booleans, and `None`).

## Backing off on a 429 — RateLimitedProvider

```python
from neograph_engine.llm import RateLimitedProvider, OpenAIProvider

provider = RateLimitedProvider(OpenAIProvider(...), max_retries=5)
engine = ng.GraphEngine.compile(definition, ng.NodeContext(provider=provider))
```

Without it you end up wrapping `engine.run()` in your own retry loop, which
retries **the whole graph** — re-running every node that already succeeded. This
retries the one HTTP request that failed.

It honours the upstream's `Retry-After` when there is one, falls back to
`default_wait_seconds` when there is not, caps a single sleep at
`max_wait_seconds`, and gives up once `max_total_wait_seconds` of sleeping has
accumulated (`0` = no total cap).

Your own provider opts in by raising the right exception:

```python
class MyProvider(ng.Provider):
    def complete(self, params):
        r = requests.post(...)
        if r.status_code == 429:
            raise ng.RateLimitError(
                "rate limited",
                retry_after_seconds=int(r.headers.get("Retry-After", -1)))
```

Anything else you raise stays an error.

## Checking a graph before you run it — `validate`

```python
report = ng.validate(definition)
if report.has_errors():
    print(report.summary())
    for d in report.errors():
        print(d.code, d.path, d.message)
```

Dangling edges, unreachable nodes, dead barriers — a report you can read, rather
than finding out when `compile()` throws.

One edge worth knowing: `validate()` compiles the definition first, so a node
type nobody registered surfaces as an **exception**, not a diagnostic. Register
your node types before validating, exactly as you would before compiling.

**Retries at the node level need no class.** Put `"retry_policy": {...}` in the
graph definition and the engine honours it — that has always worked from Python:

```python
definition["retry_policy"] = {"max_retries": 5, "initial_delay_ms": 100}
```

## MCP — using a remote tool server

```python
client = ng.mcp.MCPClient("http://localhost:8931")     # or ["python", "server.py"]
client.initialize()

engine = ng.GraphEngine.compile(
    definition, ng.NodeContext(tools=client.get_tools()))
```

That is the whole integration. `get_tools()` returns the server's catalogue as
tools the graph can dispatch, and you can mix them freely with your own Python
tools in the same `NodeContext`.

`client.call_tool(name, args)` calls one directly, outside any graph.

**They overlap — over HTTP.** MCP tools are network round-trips, which is the
case where concurrent dispatch pays, and `MCPTool` is a real C++ `AsyncTool`. But
the two transports are not the same, and it is worth knowing which one you have:

| transport | 3 calls × 0.4 s |
|---|---|
| HTTP | **0.41 s** — each call is its own request |
| stdio | 1.2 s — one subprocess, one pipe; the client takes a capacity-1 lock, so calls queue |

That stdio number is not a NeoGraph limitation to be optimised away; it is what
a single pipe means. If you need MCP calls to overlap, use an HTTP server.

The stdio subprocess is terminated when the last reference to the session is
dropped — the client, or any tool it produced.

Runnable, offline (it starts its own MCP server): [`examples/26_mcp_tools.py`](../bindings/python/examples/26_mcp_tools.py).

## Making tools run concurrently

When the model asks for several tools in one turn, NeoGraph dispatches them
together. Whether they actually *overlap* is the tool's choice:

```python
class Fetch(ng.AsyncTool):          # ng.Tool -> serial;  ng.AsyncTool -> overlaps
    def execute(self, arguments):
        return requests.get(arguments["url"]).text
    ...
```

Measured, twenty tools that each wait 300 ms:

| tool base class | wall clock |
|---|---|
| `ng.Tool` | 6.0 s |
| `ng.AsyncTool` | **0.30 s** (19.9×) |

**Why it is opt-in.** A sync `Tool` runs to completion before the next one
starts — so an existing tool that keeps state cannot suddenly find itself
racing a copy of itself. Concurrency is something you declare, not something
that happens to you. The flip side: two calls to the same `AsyncTool` can be in
flight at once (the model may ask for it twice in one turn), so keep per-call
state on the stack, not on `self`.

**The boundary, stated plainly.** A Python function holds the GIL while it runs.
Your tool overlaps with its siblings only while it is *not* holding it — which
is while it is blocked on I/O, because that is when CPython lets go. An HTTP
call, a socket read, a database query, `time.sleep`: all release it, all overlap.

A tool that burns CPU **in Python** holds the GIL for its whole body and will
not overlap, however many threads it is handed:

| 3 CPU-bound `AsyncTool`s | 3.1× the time of one |
|---|---|

Declaring such a tool `AsyncTool` buys nothing. (If the heavy work happens
inside numpy, a C extension, or a subprocess, the GIL is released there and it
does overlap.) This is pinned by a test, so the claim cannot quietly drift.

Concurrency is bounded by an internal worker pool — 32 threads by default, or
`NEOGRAPH_TOOL_THREADS`. They spend their time blocked on I/O, so a generous
pool costs little.

Runnable, offline: [`examples/25_async_tools.py`](../bindings/python/examples/25_async_tools.py).

## Gating tool calls — "the agent wants to run `rm -rf build/`. Allow?"

There is one hook between *the model asked for tool X* and *tool X runs*, and it
returns one of three verdicts:

```python
def gate(call, gctx):
    if call.name not in DANGEROUS:
        return ng.ToolDecision.allow()

    # None until a human has actually answered — which is how the gate tells
    # "nobody has been asked yet" from "the answer was no", and so avoids
    # asking the same question forever.
    if gctx.resume_value is None:
        return ng.ToolDecision.interrupt(
            f"{call.name} needs approval",
            {"tool": call.name, "arguments": call.arguments})

    if gctx.resume_value.get("approved"):
        return ng.ToolDecision.allow()
    return ng.ToolDecision.deny("the operator refused this command")

engine.set_tool_gate(gate)
```

```python
result = engine.run(cfg)
if result.interrupted:
    print(result.interrupt_value["reason"])   # "shell needs approval"
    print(result.interrupt_value["value"])    # {"tool": ..., "arguments": ...}
    result = engine.resume(cfg.thread_id, {"approved": True})
```

| Verdict | Effect |
|---|---|
| `ToolDecision.allow()` | Run it. |
| `ToolDecision.allow({...})` | Run it with these arguments instead — this is where ambient values (tenant, thread, credentials) get injected, rather than every tool knowing about them. |
| `ToolDecision.deny(reason)` | Do not run it. The reason goes back to the model as the tool's result, so it can adapt instead of asking for the same tool again next turn. |
| `ToolDecision.interrupt(reason, payload)` | Do not run it, and pause the whole run. The payload surfaces at `RunResult.interrupt_value["value"]`. |

Permission, audit, argument rewriting and the per-call interrupt are not four
features; they are one primitive wearing four hats.

**The gate sees every call before any tool runs, and that ordering is the
design.** Suppose the model asks for `list_files` and `shell` together, and only
`shell` needs approval. When the run pauses, `list_files` has **not** run either
— even though the gate allowed it.

That is not an oversight. `resume()` re-enters the node from the top, because a
node that interrupted recorded no writes. Had `list_files` already run, the
approval would run it a *second* time — swap it for `git commit` and the prompt
for `rm -rf` has just double-committed. And if the human says **no**, anything
already executed cannot be undone. A permission system in which "denied" does
not mean "nothing happened" is not a permission system.

Two practical notes:

- **A checkpoint store is required.** An interrupt has to be resumable; without
  a store there is nothing to resume from.
- **The gate lives on the engine, not on `RunConfig`.** `resume()` builds its own
  `RunConfig` internally, so a per-run gate would vanish the moment a human
  answered the very prompt it raised — and the dangerous tool would then run
  unchecked. Set it once on the engine and it holds for every run and resume.

Runnable end to end, no API key: [`examples/24_tool_approval_gate.py`](../bindings/python/examples/24_tool_approval_gate.py).

## Built-in reducers

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

## What's covered by the binding

- **Engine surface** — `GraphEngine.compile / run / run_stream / run_async / run_stream_async / resume / resume_async / get_state / update_state / fork`, `RunConfig`, `RunResult`, `set_worker_count`, `set_checkpoint_store`, `set_node_cache_enabled`.
- **Custom Python nodes** — subclass `neograph_engine.GraphNode`, register via `NodeFactory.register_type` or the `@neograph_engine.node` decorator. Engine dispatches under proper GIL handling, including from fan-out worker threads.
- **Custom Python tools** — subclass `neograph_engine.Tool`, pass into `NodeContext(tools=[...])`. Engine takes ownership at compile time.
- **Async** — every `*_async` binding returns an `asyncio.Future` bound to the calling thread's running loop. Stream callbacks are hopped to the loop thread via `loop.call_soon_threadsafe` so callbacks run where asyncio expects.
- **Checkpoints** — subclass `CheckpointStore` for a Python backend, use `InMemoryCheckpointStore` directly, or use `PostgresCheckpointStore` when the binding is built from source with `-DNEOGRAPH_BUILD_POSTGRES=ON` (libpq bundling for the PyPI wheel is pending).
- **OpenAI Responses over WebSocket** — `SchemaProvider(schema="openai_responses", use_websocket=True)`.

Wheels: Linux x86_64 (manylinux_2_34), Linux aarch64 (manylinux_2_34),
macOS arm64 (14+), Windows x64 (MSVC), for Python 3.9 → 3.13. **20 wheels
+ sdist per release** via cibuildwheel.

See [`bindings/python/examples/`](../bindings/python/examples/) for the
full example index — minimal graph, ReAct, HITL, intent routing, async,
multi-agent debate, JSON graph round-trip, and a Gradio chat with a
deep-research subgraph (Crawl4AI + Postgres optional).

## Differences from LangGraph (Python binding)

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
- **Observability ships in-tree, not as a separate SaaS** — pair
  `neograph_engine.tracing.otel_tracer` (vendor-neutral OTel spans)
  with `neograph_engine.openinference.OpenInferenceProvider` +
  `openinference_tracer` (LLM-shape attribute keys), point an
  OTLP exporter at any OpenInference-aware backend (Phoenix, Arize,
  Langfuse — all OSS, all self-hostable), and you get the LangSmith
  UX (chat-bubble per turn, DAG hierarchy, prompt/response capture,
  per-call token counts and cost) without a vendor SaaS contract.

  ```bash
  docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix
  pip install neograph-engine opentelemetry-exporter-otlp
  ```
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

  wrapped = OpenInferenceProvider(OpenAIProvider(api_key=...), tracer)
  ctx = ng.NodeContext(provider=wrapped)
  engine = ng.GraphEngine.compile(graph, ctx)
  with openinference_tracer(tracer) as cb:
      engine.run_stream(cfg, cb)
  # → http://localhost:6006 renders the trace as a LangSmith-style chain.
  ```

  LangGraph's hosted LangSmith is the typical observability path
  in that ecosystem; LangFuse / Phoenix are the OSS substitutes
  but require integration glue. NeoGraph's `OpenInferenceProvider`
  *is* the integration glue — drop in, every `Provider.complete()`
  becomes an LLM span automatically.
- **One node method** — `def run(self, input)` is the canonical
  override as of **v0.4.0**. Read state from `input.state`, the live
  cancel handle from `input.ctx.cancel_token`, the streaming sink
  (or `None`) from `input.stream_cb`. Return a `list[ChannelWrite]`,
  `list[Send]`, a `Command`, or a `NodeResult`. The legacy 8-virtual
  chain is removed in v1.0.0 — `def run(self, input)` is the only
  override.
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
  every minor release.
  NeoGraph's Python surface is a thin pybind11 layer over a frozen
  C++ ABI under semantic-versioning. **Code you write today against
  v0.4.0 will compile against v1.x** — the deprecation window is the
  *only* mechanism for breaking changes.
- **No Docker required for deployment** — a direct consequence of
  the single-dep tree above. Production LangChain deployments
  effectively *require* Docker + a fully-pinned `requirements.txt`;
  without it, a transitive package's silent minor bump on the next
  deploy can take the server down at runtime. NeoGraph's wheel ships
  its full native runtime baked in, so:

  - `pip install neograph-engine` on bare metal / VPS / a
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
