# Python API examples

Twenty-eight scripts covering the binding surface end-to-end.

## Setup

```bash
pip install neograph-engine python-dotenv
cp .env.example .env  # edit OPENAI_API_KEY for examples that hit a real LLM
```

`_common.py` auto-loads `.env` from this directory or any parent.
Examples that need an API key skip cleanly (without crashing) when
the key is missing.

## Index

| # | File | Network | Pattern |
|---|------|---------|---------|
| 01 | [`01_minimal.py`](01_minimal.py) | offline | `GraphNode` subclass + `engine.run()`. The smallest useful graph. |
| 02 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | offline | `Tool` subclass + built-in `tool_dispatch`. Hand-crafted tool_call (no real LLM). |
| 03 | [`03_send_fanout.py`](03_send_fanout.py) | offline | `run(input)` returning `NodeResult` with `Send` list + `set_worker_count(4)`. Map-reduce. |
| 04 | [`04_async_concurrent.py`](04_async_concurrent.py) | offline | `engine.run_async` + `asyncio.gather` of 8 concurrent runs + `run_stream_async`. |
| 05 | [`05_openai_provider.py`](05_openai_provider.py) | **OpenAI** | `OpenAIProvider` + built-in `llm_call` node. One-shot completion. |
| 06 | [`06_react_agent.py`](06_react_agent.py) | **OpenAI** | ReAct loop: `llm_call` ↔ `tool_dispatch` with `has_tool_calls` conditional. |
| 07 | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | offline | Two-stage propose/approve workflow with mock LLM emitter. |
| 08 | [`08_intent_routing.py`](08_intent_routing.py) | **OpenAI** | Classifier node + conditional edge → math / translate / general expert. |
| 09 | [`09_state_management.py`](09_state_management.py) | offline | `set_checkpoint_store(InMemoryCheckpointStore())` + `get_state` + `fork`. |
| 10 | [`10_command_routing.py`](10_command_routing.py) | offline | `run(input)` returning `Command(goto_node=…, updates=[…])`. |
| 11 | [`11_reflexion.py`](11_reflexion.py) | **OpenAI** | Actor + critic loop with reflection prompt (Shinn et al. 2023). |
| 12 | [`12_self_ask.py`](12_self_ask.py) | **OpenAI** | Self-Ask follow-up question decomposition (Press et al. 2022). |
| 13 | [`13_multi_agent_debate.py`](13_multi_agent_debate.py) | **OpenAI** | Two-debater + judge. Debaters fan out via `Send`. |
| 14 | [`14_graph_to_json.py`](14_graph_to_json.py) | offline | Serialize a graph definition to a `.json` file. |
| 15 | [`15_graph_from_json.py`](15_graph_from_json.py) | offline | Load a `.json` graph and run it (companion to 14). |
| 16 | [`16_deep_research_chat.py`](16_deep_research_chat.py) | **OpenAI WS** | Multi-turn Gradio chat that switches into a parallel deep-research subgraph on `조사해줘 / research / investigate`. Uses `SchemaProvider("openai_responses", use_websocket=True)`. Requires `pip install gradio`. |
| 17 | [`17_deep_research_crawl4ai.py`](17_deep_research_crawl4ai.py) | **OpenAI WS + Crawl4AI + Postgres** | Same chat shape as 16, but researchers actually search the web via a local Crawl4AI container (`docker run unclecode/crawl4ai`) and state is durable in Postgres (`PostgresCheckpointStore`). Both are optional via env vars; falls back gracefully when absent. Source-build with `-DNEOGRAPH_BUILD_POSTGRES=ON` for the Postgres path. |
| 18 | [`18_node_cache.py`](18_node_cache.py) | **OpenAI** | `engine.set_node_cache_enabled("ask", True)` — second run on the same input replays the cached `NodeResult` in 0 ms, no LLM call. Stats via `engine.node_cache_stats()`. |
| 19 | [`19_streaming_messages.py`](19_streaming_messages.py) | offline | `from neograph_engine import message_stream` — wraps a callback so `LLM_TOKEN` events arrive as LangChain-shape message dicts (`{role, content, content_so_far, node, metadata}`). |
| 20 | [`20_otel_tracing.py`](20_otel_tracing.py) | offline | `from neograph_engine.tracing import otel_tracer` — bridge engine events to OpenTelemetry spans. Ships ConsoleSpanExporter; swap for OTLP to send to Jaeger / Tempo / Honeycomb / Datadog. |
| 21 | [`21_http2_transport.py`](21_http2_transport.py) | **OpenAI** | `SchemaProvider(..., prefer_libcurl=True)` — opt-in HTTP/2 (libcurl) transport vs default ConnPool (HTTP/1.1 keep-alive). A/Bs both on a 5-way parallel burst and prints which is faster on YOUR endpoint. Default ConnPool is faster on api.openai.com; flip when you need CF-WAF compatibility, lower TCP fan-out through corporate proxies, or HTTP/3. |
| 22 | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | **OpenAI** | Goal-driven self-evolution: the agent runs, scores its output against a JSON-shape goal, and asks an LLM to propose a revised graph definition. Loop closes when score ≥ 1.0 or max_iters hit. Demonstrates JSON-as-program where the modifier's only output is a new graph spec. |
| 23 | [`23_evolving_chat_agent.py`](23_evolving_chat_agent.py) | offline (mock) / **OpenAI** | Per-thread evolving chat agent: persistent multi-turn conversation; between turns the agent's JSON definition is rewritten based on accumulated history. Demonstrates checkpoint-resume across evolution (prior messages survive), the `__graph_meta__` audit channel pattern, and a validator boundary (whitelist node types, required channels, edge connectivity). Runs end-to-end with no API key via a deterministic mock provider + heuristic mock evolver. |
| 24 | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) | offline | The tool gate (#89): `engine.set_tool_gate(...)` is consulted for every tool call **before any tool runs**, returning Allow / Allow-with-rewritten-args / Deny / Interrupt. Shows the canonical approval prompt — *"the agent wants to run `rm -rf build/`. Allow?"* — and, crucially, that the harmless sibling call has **not** run while the human is deciding, so a refusal really means nothing happened and an approval does not re-run it. |
| 25 | [`25_async_tools.py`](25_async_tools.py) | offline | Concurrent tools (#96): `ng.AsyncTool` instead of `ng.Tool` and three 300 ms tools take 0.30 s instead of 0.90 s. Also measures the boundary in the same run — three *CPU-bound* tools take 3.2x the time of one, because a Python function holds the GIL while it runs and no number of threads changes that. Concurrency is opt-in so an existing stateful tool cannot suddenly race itself. |
| 26 | [`26_mcp_tools.py`](26_mcp_tools.py) | offline | MCP (#95): `ng.mcp.MCPClient(url).get_tools()` pulls a remote tool catalogue and hands it straight to `NodeContext`. Starts its own MCP server, so it runs with no network. Measures the thing that matters — three 0.4 s MCP calls in 0.41 s over HTTP — and says out loud where it does *not* hold: stdio has one pipe, so those calls serialize. |
| 27 | [`27_a2a_server.py`](27_a2a_server.py) | localhost | A2A hosting (#120): the official `a2a-sdk` owns JSON-RPC, task state, the agent card, and cancellation. `ProtocolHostAdapter` maps A2A context/task IDs to NeoGraph checkpoint threads and active async runs. Requires Python 3.10+ and `pip install "neograph-engine[a2a]"`. |
| 28 | [`28_acp_agent.py`](28_acp_agent.py) | stdio | ACP hosting (#120): the official `agent-client-protocol` runtime owns stdio framing and session methods. `ProtocolHostAdapter` preserves multi-turn state while the demo process is running and turns ACP cancel notifications into real `run_async()` cancellation. Requires Python 3.10+ and `pip install "neograph-engine[acp]"`. |

## Why hosting uses the official SDKs

The C++ library has its own `A2AServer` and `ACPServer`, but exposing those
classes directly would give Python users a second protocol implementation with
weaker integration than Python's official SDKs. In particular, the official
SDKs already own current wire-format compatibility, server transports, task or
session lifecycle, and asyncio cancellation. NeoGraph only supplies the part
that those SDKs cannot: a checkpoint-aware call into the C++ graph engine.

| Item | Decision |
|------|----------|
| C++ feature that appears missing | `A2AServer`, `ACPServer`, and their lifecycle methods are not mirrored as Python classes. |
| Python alternative | Official `a2a-sdk` 1.x and `agent-client-protocol` 0.11.x server runtimes. |
| NeoGraph integration | `ProtocolHostAdapter` maps protocol conversation IDs to `RunConfig.thread_id`, enables `resume_if_exists`, calls `run_async()`, and cancels the active asyncio task. |
| Dependency policy | Both SDKs are optional because they require Python 3.10+, while `neograph-engine` supports Python 3.9. Install `neograph-engine[a2a]`, `neograph-engine[acp]`, or `neograph-engine[protocols]`. |
| Current limit | The examples return one final text artifact/message. Protocol-native streaming, rich media, ACP editor tool callbacks, and durable ACP `session/load` remain application code. The demo uses an in-memory checkpoint store. |
| Revisit direct bindings when | A user must embed the exact C++ server in Python, or the official SDK path cannot preserve a required NeoGraph cancellation, checkpoint, tracing, or tool-call behavior. |

Run any one with:

```bash
python 01_minimal.py
```

## Mental model

NeoGraph from Python looks like LangGraph from Python: a graph of
nodes, channels with reducers, dynamic fan-out via `Send`, routing
overrides via `Command`, conditional edges via named conditions
(`route_channel`, `has_tool_calls`, etc.). The same primitives, the
same JSON-shaped graph definition. The difference is what's running
it — a C++ engine that does the super-step loop, scheduling, and
checkpointing in microseconds per step instead of LangGraph's
~600 µs.

Three patterns appear across the examples:

1. **Python custom nodes** (01, 03, 04, 07, 09, 10, 11, 12, 13)
   subclass `neograph_engine.GraphNode` and override `execute(state)`
   or `execute_full(state)`. The engine dispatches into Python under
   GIL handling — fan-out workers each acquire the GIL re-entrantly
   so concurrent custom nodes don't deadlock.

2. **Python tools** (02, 06, 07) subclass `neograph_engine.Tool` and
   pass instances into `NodeContext(tools=[…])`. The engine takes
   ownership at compile time; Python references can drop afterwards.

3. **Async** (04) — every `*_async` binding returns an
   `asyncio.Future` bound to the calling thread's running loop.
   Stream callbacks are hopped onto the loop thread via
   `loop.call_soon_threadsafe`, so your `cb(ev)` runs where asyncio
   expects.

## Graph definition is JSON

`GraphEngine.compile(definition, ctx)` accepts either a Python
`dict` you build in code or a `dict` you `json.loads()` from a file
— same shape. Examples 14 + 15 show the round-trip. Custom node
*types* still need to be registered in code (Python classes can't
be encoded to JSON), but the wiring — channels, nodes by type,
edges, conditional edges — is data.

## Distribution name vs. import name

The PyPI package is **`neograph-engine`** (the bare `neograph` name
was already taken on PyPI by an unrelated project). The Python
import name is `neograph_engine`:

```python
import neograph_engine as ng
from neograph_engine.llm import OpenAIProvider, SchemaProvider
```
