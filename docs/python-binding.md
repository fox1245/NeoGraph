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
| `interrupt_value` | `dict` | Diagnostic payload — `{"reason": ...}` or `{"message": ...}`. |
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

- **Engine surface** — `GraphEngine.compile / run / run_stream / run_async / run_stream_async / resume_async / get_state / update_state / fork`, `RunConfig`, `RunResult`, `set_worker_count`, `set_checkpoint_store`, `set_node_cache_enabled`.
- **Custom Python nodes** — subclass `neograph_engine.GraphNode`, register via `NodeFactory.register_type` or the `@neograph_engine.node` decorator. Engine dispatches under proper GIL handling, including from fan-out worker threads.
- **Custom Python tools** — subclass `neograph_engine.Tool`, pass into `NodeContext(tools=[...])`. Engine takes ownership at compile time.
- **Async** — every `*_async` binding returns an `asyncio.Future` bound to the calling thread's running loop. Stream callbacks are hopped to the loop thread via `loop.call_soon_threadsafe` so callbacks run where asyncio expects.
- **Checkpoints** — `InMemoryCheckpointStore` always; `PostgresCheckpointStore` when the binding is built from source with `-DNEOGRAPH_BUILD_POSTGRES=ON` (libpq bundling for the PyPI wheel is pending).
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
