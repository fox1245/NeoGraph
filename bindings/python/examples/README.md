# Python API examples

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

These 28 scripts show graph state, routing, tools, provider requests, and protocol hosting. Start with the offline examples; hosted model calls require explicit credentials and incur provider charges.

## Setup

```bash
pip install neograph-engine
python 01_minimal.py
```

Run commands from this directory. `python-dotenv` is optional: install it if you want `_common.py` to load the nearest example/repository `.env`. Exported environment variables take precedence. Missing hosted credentials are an error, not a successful verification run.

For hosted examples, set `OPENAI_API_KEY` and optionally `OPENAI_MODEL` (default `gpt-4.1-mini`). `_common.py` admits a closed OpenAI Chat or Responses descriptor, then constructs `SchemaProvider`. The typed SDK uses libcurl HTTP; the removed provider classes, completion parameters, WebSocket path, and transport-backend selector are not available.

For no-key protocol qualification, set `OPENAI_API_BASE` to a faithful loopback Chat/Responses peer. It must implement the route and response shape used by the example, not return a Python mock. Use `NG_EXAMPLE_CA_FILE` for a local TLS CA. `NG_PROVIDER_DESCRIPTOR` can instead name a complete admitted descriptor JSON file, including a gateway prefix, routes, and allowed headers. A base URL is not a full `/v1/chat/completions` route. See [the Python binding guide](../../../docs/python-binding.md) for the typed request/outcome contract.

## Index and expected behavior

Run each entry with `python <file>`. The expected state below is a verification target; it is not a claim that these scripts were run in this documentation update. Model-generated wording is not deterministic.

| # | File | Prerequisites | Expected behavior |
|---|------|---------------|-------------------|
| 01 | [`01_minimal.py`](01_minimal.py) | None | A custom node doubles 21 to 42. |
| 02 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | None | A scripted tool call goes through `tool_dispatch`; the calculator returns `42`. |
| 03 | [`03_send_fanout.py`](03_send_fanout.py) | None | Eight `Send` branches merge squares `[0, 1, 4, 9, 16, 25, 36, 49]`; append order is not assumed. |
| 04 | [`04_async_concurrent.py`](04_async_concurrent.py) | None | Eight async runs produce `[0, 2, 4, 6, 8, 10, 12, 14]`; a streamed run emits node events. |
| 05 | [`05_openai_provider.py`](05_openai_provider.py) | Chat peer or hosted key | `llm_call` writes an assistant message saying `hello world`. |
| 06 | [`06_react_agent.py`](06_react_agent.py) | Responses peer or hosted key | A model-issued `calc` call returns `4053`, then the model gives its final answer. |
| 07 | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | None | A checkpoint pauses before payment dispatch; approval resumes exactly one simulated payment. No money is charged. |
| 08 | [`08_intent_routing.py`](08_intent_routing.py) | Chat peer or hosted key | Three questions route to math, translate, and general experts; each expert writes its answer. |
| 09 | [`09_state_management.py`](09_state_management.py) | None | Alpha reaches count 11; beta forks that value; an unknown thread has no checkpoint. |
| 10 | [`10_command_routing.py`](10_command_routing.py) | None | Inputs 200, 50, and -10 choose accept, manual, and reject nodes via `Command`. |
| 11 | [`11_reflexion.py`](11_reflexion.py) | Chat peer or hosted key | Actor/critic calls carry reflection into the next attempt, stopping on `ok` or the superstep cap. This is a bounded teaching adaptation. |
| 12 | [`12_self_ask.py`](12_self_ask.py) | Chat peer or hosted key | Intermediate questions/answers accumulate in a scratchpad before a final answer. No search service is attached. |
| 13 | [`13_multi_agent_debate.py`](13_multi_agent_debate.py) | Chat peer or hosted key | Two `Send` branches produce opposing arguments; one judge reads the merged arguments. |
| 14 | [`14_graph_to_json.py`](14_graph_to_json.py) | None | A doubled result of 42 and a saved `my_graph.json` definition. |
| 15 | [`15_graph_from_json.py`](15_graph_from_json.py) | Run 14 first | The saved definition doubles 5 to 10 and 100 to 200; custom node types are registered separately. |
| 16 | [`16_deep_research_chat.py`](16_deep_research_chat.py) | Responses peer/key; `gradio` | Normal chat or a three-question research report. Researchers use model knowledge, not web search. |
| 17 | [`17_deep_research_crawl4ai.py`](17_deep_research_crawl4ai.py) | Responses peer/key; `gradio`, `requests`; optional Crawl4AI/Postgres | `CRAWL4AI_URL` enables real `/md` search; `NEOGRAPH_PG_DSN` enables durable engine checkpoints. Configured service failures are not silently replaced. Without either setting, the script explicitly reports model-only research/in-memory state. Gradio history is not restored automatically. |
| 18 | [`18_node_cache.py`](18_node_cache.py) | Chat peer or hosted key | Five runs across two topics execute the provider-backed node twice; repeated inputs replay cached writes. No fixed latency is promised. |
| 19 | [`19_streaming_messages.py`](19_streaming_messages.py) | None | Five scripted token events form `Octopuses have three hearts.` and message-stream chunks. |
| 20 | [`20_otel_tracing.py`](20_otel_tracing.py) | `opentelemetry-api`, `opentelemetry-sdk` | Console spans cover the run and three nodes; final trail is `['A', 'B', 'C']`. |
| 21 | [`21_http2_transport.py`](21_http2_transport.py) | TLS Chat peer/key; libcurl HTTP/2 support | Compare HTTP/1.1 and HTTP/2 requests through the same SDK. Confirm negotiated protocol separately; timings depend on the endpoint. |
| 22 | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | Chat peer or hosted key | Score profile JSON, request a revised graph after failure, recompile, retry. Only registered node types are admitted. A model may exhaust the iteration cap without success. |
| 23 | [`23_evolving_chat_agent.py`](23_evolving_chat_agent.py) | Chat peer or hosted key | Preserve conversation checkpoints across an admitted graph rewrite; record version/hash in `__graph_meta__`. This metadata is application-level, not authoritative replay evidence. |
| 24 | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) | None | Pause before either sibling tool runs; refusal executes only `list_files`, approval executes both once. Shell actions are simulated. |
| 25 | [`25_async_tools.py`](25_async_tools.py) | None | Compare serial `Tool` with overlapping I/O-bound `AsyncTool`, then show the CPython GIL boundary. Timing ratios are observations, not pass criteria. |
| 26 | [`26_mcp_tools.py`](26_mcp_tools.py) | MCP-enabled build | Start a real local JSON-RPC MCP peer, discover `fetch`, execute three calls with an explicit re-entrant policy. No external network or key. |
| 27 | [`27_a2a_server.py`](27_a2a_server.py) | Python 3.10+; `neograph-engine[a2a]` | Serve an agent card and A2A JSON-RPC on `127.0.0.1:9999`; streamed artifacts form `NeoGraph received: hello (turn 1)`. |
| 28 | [`28_acp_agent.py`](28_acp_agent.py) | Python 3.10+; `neograph-engine[acp]` | Serve ACP over stdio; initialize, create a session, prompt, receive token updates and `end_turn`. Durable `session/load` requires a configured Postgres or SQLite backend. |

## State and scheduling

A node reads the current superstep's state and returns channel writes. The engine merges those writes through the declared reducers before the next superstep. `Send` supplies branch-specific input; `Command` updates state and chooses the next node. A worker pool does not make CPU-bound Python callbacks execute in parallel under the CPython GIL.

`GraphEngine.compile()` accepts a dictionary loaded from JSON as well as one written in Python. Definitions store wiring, not executable Python classes: register custom node factories before compiling a saved definition. Examples 14 and 15 write/read `my_graph.json` beside the scripts; use a disposable checkout or copy of this directory when qualifying that pair.

## Interactive and protocol scenarios

Install `gradio` for 16/17, launch the script, and visit the printed local URL. Submit `hello`, then `research apples`. For 17, a real Crawl4AI `/md` service or faithful local peer must return `{"success": true, "markdown": "..."}`. Configure Postgres only when the running build supports it and the database is reachable. Check engine state separately from UI history.

Examples 27/28 use the official Python SDKs for wire handling and `ProtocolHostAdapter` for checkpoint-aware engine execution. A2A verification needs an SDK client to fetch the agent card, send a message, and consume the stream. ACP verification needs a client subprocess connection for `initialize`, `session/new`, and `session/prompt`; stdout is reserved for protocol messages. Send a second prompt in the same context/session and expect `(turn 2)`. Cancellation should stop an active request rather than create a success-shaped fallback.

For durable ACP sessions, set exactly one of `NEOGRAPH_ACP_POSTGRES_URL` or `NEOGRAPH_ACP_SQLITE_PATH`. A completed first prompt creates the checkpoint needed for `session/load`. Keep one active agent process per session; checkpoint stores do not serialize concurrent writers across processes. The example echoes rich content blocks but does not interpret image/audio content or provide editor filesystem/terminal callbacks.

## Distribution and import names

```python
import neograph_engine as ng
from neograph_engine.llm import SchemaProvider
```

The distribution is `neograph-engine`; `neograph` on PyPI is an unrelated project.
