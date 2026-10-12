# Python API examples

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

These 47 scripts show graph state, routing, tools, provider requests, the Program API and protocol hosting. Each one shows one thing. Most run offline: a scripted model stands in for the provider, so they need no API key and no network. Examples that can also call a real model take `--live`.

The **Offline** column says what a row needs. `yes`: runs with no key and no network, and is tested automatically by `bindings/python/tests/test_examples_offline.py`. `key`: needs a provider API key. `service`: needs another process, server or extra package. `paired`: runs offline together with the example named in its row.

## Setup

```bash
pip install neograph-engine
python 01_minimal.py
```

Run commands from this directory. The offline examples need nothing else. To run against a source build instead of the wheel, configure with `-DNEOGRAPH_BUILD_PYBIND=ON` and set `PYTHONPATH` to the build directory and `bindings/python`. A build tree does not install the package's Python dependencies: run `pip install 'pydantic>=2' certifi` first. `import neograph_engine` requires pydantic; without certifi, TLS uses the host's trust store.

## Start here

Read these in order; each one adds a single idea. All of them run offline.

| Step | Example | Offline | You learn |
|---|---|---|---|
| 1 | [`01_minimal.py`](01_minimal.py) | yes | Your first graph: a function registered with `@ng.node` reads one channel and writes another, and 21 becomes 42. |
| 2 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | yes | A Python `Tool` and the built-in `tool_dispatch` node: a scripted tool call comes back as `42`. |
| 3 | [`03_send_fanout.py`](03_send_fanout.py) | yes | `Send` creates one branch per item at run time and the branches merge: eight squares, in any order. |
| 4 | [`05_openai_provider.py`](05_openai_provider.py) | yes | One model call through the built-in `llm_call` node. A scripted model answers `hello world`; `--live` calls a hosted one. |
| 5 | [`06_react_agent.py`](06_react_agent.py) | yes | A ReAct loop: `llm_call`, `tool_dispatch` and a `has_tool_calls` conditional edge. `calc` returns `4053`, then the model answers. |
| 6 | [`77_dynamic_interrupt.py`](77_dynamic_interrupt.py) | yes | Human in the loop: a node raises `NodeInterrupt`, a checkpoint keeps the run, and `resume` hands the person's answer back to the node. |
| 7 | [`29_program_hello.py`](29_program_hello.py) | yes | A Program: a short JavaScript Program defines one graph with a Python node and runs it on `LocalProgramHost`. |

After step 7, pick by goal in the next table, or browse the index by tier.

## I want to ...

Pick the goal, read the example. The rows run from the basics to the protocols.

| I want to ... | Read | Offline |
|---|---|---|
| run my first graph | [`01_minimal.py`](01_minimal.py) | yes |
| add a Python tool to a graph | [`02_tool_dispatch.py`](02_tool_dispatch.py) | yes |
| fan out branches with Send | [`03_send_fanout.py`](03_send_fanout.py) | yes |
| run many graphs at once with asyncio | [`04_async_concurrent.py`](04_async_concurrent.py) | yes |
| call a model from a graph | [`05_openai_provider.py`](05_openai_provider.py) | yes |
| build a tool-using agent loop | [`06_react_agent.py`](06_react_agent.py) | yes |
| route by intent | [`08_intent_routing.py`](08_intent_routing.py) | yes |
| choose the next node and write state in one step | [`10_command_routing.py`](10_command_routing.py) | yes |
| choose how a channel combines writes (overwrite or append) | [`79_channels.py`](79_channels.py) | yes |
| register my own reducer or edge condition | [`80_custom_registries.py`](80_custom_registries.py) | yes |
| watch a run as it happens | [`75_stream_modes.py`](75_stream_modes.py) | yes |
| retry a failing node with backoff | [`76_retry_policy.py`](76_retry_policy.py) | yes |
| let a node ask a person before it goes on | [`77_dynamic_interrupt.py`](77_dynamic_interrupt.py) | yes |
| stop a loop at a step limit | [`78_max_steps.py`](78_max_steps.py) | yes |
| pause for approval and resume | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | yes |
| allow, deny or interrupt tool calls | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) | yes |
| read and fork saved state | [`09_state_management.py`](09_state_management.py) | yes |
| save and load a graph as JSON | [`14_graph_to_json.py`](14_graph_to_json.py) | yes |
| stream tokens as LangChain-style chunks | [`19_streaming_messages.py`](19_streaming_messages.py) | yes |
| trace a run with OpenTelemetry | [`20_otel_tracing.py`](20_otel_tracing.py) | yes |
| run I/O-bound tools concurrently | [`25_async_tools.py`](25_async_tools.py) | yes |
| run a JavaScript Program | [`29_program_hello.py`](29_program_hello.py) | yes |
| join parallel branches, and see why a join stalls | [`30_program_barrier_join.py`](30_program_barrier_join.py) | yes |
| wait for the first of two redundant sources | [`31_program_any_of_join.py`](31_program_any_of_join.py) | yes |
| send a failing Program node to an operator | [`32_program_failure_routing.py`](32_program_failure_routing.py) | yes |
| bound what a Program may spend | [`33_program_budgets.py`](33_program_budgets.py) | yes |
| pause and resume a Program run | [`34_program_pause_resume.py`](34_program_pause_resume.py) | yes |
| fork a Program run without repeating a model call | [`35_program_fork.py`](35_program_fork.py) | yes |
| recover a Program run after a crash | [`36_program_crash_recovery.py`](36_program_crash_recovery.py) | yes |
| bind a model to a Program node and read its usage | [`37_program_model_node.py`](37_program_model_node.py) | yes |
| repair a Program from the compiler's diagnostics | [`38_program_diagnostics.py`](38_program_diagnostics.py) | yes |
| race two runs and cancel the slower one | [`39_program_main_race.py`](39_program_main_race.py) | yes |
| time out a slow node attempt and retry it | [`40_program_attempt_timeout.py`](40_program_attempt_timeout.py) | yes |
| use tools from an MCP server | [`26_mcp_tools.py`](26_mcp_tools.py) | yes |
| host my agent over A2A | [`27_a2a_server.py`](27_a2a_server.py) | yes |
| call an A2A agent from Python | [`45_a2a_client.py`](45_a2a_client.py) | yes |
| host my agent over ACP | [`28_acp_agent.py`](28_acp_agent.py) | yes |
| let an agent rewrite its own graph | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | yes |
| try Reflexion, Self-Ask or a debate | [`11_reflexion.py`](11_reflexion.py) | key |

## Index by tier

### Core engine

Nodes, channels, routing, streaming, retries and interrupts, with no model involved. Example 14 writes `my_graph.json` beside the script and 15 reads it, so run them in that order.

| # | Example | Offline | Shows |
|---|---|---|---|
| 01 | [`01_minimal.py`](01_minimal.py) | yes | A function registered with `@ng.node` reads a channel, doubles it and writes it back: 21 becomes 42. |
| 02 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | yes | A Python `Tool` and the built-in `tool_dispatch` node: a scripted tool call comes back as `42`. |
| 03 | [`03_send_fanout.py`](03_send_fanout.py) | yes | Eight `Send` branches merge squares `[0, 1, 4, 9, 16, 25, 36, 49]`; append order is not assumed. |
| 04 | [`04_async_concurrent.py`](04_async_concurrent.py) | yes | Eight async runs with `asyncio.gather` give `[0, 2, 4, 6, 8, 10, 12, 14]`; a streamed run emits node events. |
| 10 | [`10_command_routing.py`](10_command_routing.py) | yes | `Command` picks the next node and writes state: inputs 200, 50 and -10 go to accept, manual and reject. |
| 14 | [`14_graph_to_json.py`](14_graph_to_json.py) | yes | A graph definition is plain JSON: it runs from a dict and is saved as `my_graph.json`. |
| 15 | [`15_graph_from_json.py`](15_graph_from_json.py) | yes | Loads `my_graph.json` from example 14 and doubles 5 to 10 and 100 to 200. Run 14 first; custom node types are registered in code. |
| 19 | [`19_streaming_messages.py`](19_streaming_messages.py) | yes | `message_stream` turns token events into LangChain-style chunks; five scripted tokens form `Octopuses have three hearts.` |
| 75 | [`75_stream_modes.py`](75_stream_modes.py) | yes | Stream modes: the same run observed in VALUES, UPDATES, EVENTS and DEBUG mode, with the events each mode delivers counted. |
| 76 | [`76_retry_policy.py`](76_retry_policy.py) | yes | `RetryPolicy`: a node fails twice and the engine retries it with exponential backoff; a smaller budget shows what the caller gets when the retries run out. |
| 77 | [`77_dynamic_interrupt.py`](77_dynamic_interrupt.py) | yes | `NodeInterrupt`: a node raises it, the payload tells the caller what to ask, and `resume` hands the answer back to the node (approve or refuse). |
| 78 | [`78_max_steps.py`](78_max_steps.py) | yes | `max_steps`: a loop is stopped by the step limit and the result says so (`max_steps_exhausted`, `RunStatus.StepLimit`); a run that ends on the last step is not flagged. |
| 79 | [`79_channels.py`](79_channels.py) | yes | Channels and reducers: `overwrite` against `append`, `initial` values, and channel versions as one shared write counter. |
| 80 | [`80_custom_registries.py`](80_custom_registries.py) | yes | Your own reducer and condition: `ReducerRegistry.register_reducer` keeps the highest of three judges' scores in any order, and `ConditionRegistry.register_condition` routes on it. |

### Agent patterns

A model, with or without tools, inside a graph. The `key` rows call a hosted model.

| # | Example | Offline | Shows |
|---|---|---|---|
| 05 | [`05_openai_provider.py`](05_openai_provider.py) | yes | One model call through `llm_call`; a scripted model answers `hello world`, `--live` calls a hosted one. |
| 06 | [`06_react_agent.py`](06_react_agent.py) | yes | ReAct over the Responses protocol: a model-issued `calc` call returns `4053`, then the model answers. |
| 08 | [`08_intent_routing.py`](08_intent_routing.py) | yes | Three questions route to math, translate and general experts through `route_channel`; each expert writes its answer. |
| 11 | [`11_reflexion.py`](11_reflexion.py) | key | Reflexion: actor and critic calls carry a reflection into the next attempt; a bounded teaching version. |
| 12 | [`12_self_ask.py`](12_self_ask.py) | key | Self-Ask: intermediate questions and answers accumulate in a scratchpad before the final answer; no search service is attached. |
| 13 | [`13_multi_agent_debate.py`](13_multi_agent_debate.py) | key | Two `Send` branches argue opposite sides; one judge reads the merged arguments. |
| 16 | [`16_deep_research_chat.py`](16_deep_research_chat.py) | key | A Gradio chat with a research mode: a three-question report from parallel researchers that use model knowledge, not web search. Needs `gradio`. |
| 17 | [`17_deep_research_crawl4ai.py`](17_deep_research_crawl4ai.py) | key | The same research graph with real web search through Crawl4AI and optional PostgreSQL checkpoints. Needs `gradio`, `requests` and a Crawl4AI server. |
| 22 | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | yes | A graph scores its own output and asks a model for a revised definition after a failure. A rewrite with an unregistered node type is rejected with its reason; the admitted one is compiled and the goal is met on the second iteration. `--live` calls a hosted model. |
| 23 | [`23_evolving_chat_agent.py`](23_evolving_chat_agent.py) | yes | After a few chat turns an evolver model proposes a new agent definition. `validate_agent` admits it, a fresh engine shares the checkpoint store so the four earlier messages survive, and `__graph_meta__` records the version and hash (application metadata, not replay evidence). |

### Program API

The restricted JavaScript Program API runs on top of Core graphs: `ProgramRegistryBuilder`, `ProgramCompiler`, `LocalProgramHost`. It is described in [Program and QuickJS](../../../docs/python-binding.md#program-and-quickjs) and exercised by the end-to-end scenarios in [`bindings/python/tests/program_e2e/`](../tests/program_e2e/README.md): joins, failure routing, budgets, pause and resume, fork and crash recovery. The examples below each show one of those on `LocalProgramHost`. They run offline, and a scripted model answers where a node calls one. `_program.py` holds the setup they share; example 29 spells it out once.

| # | Example | Offline | Shows |
|---|---|---|---|
| 29 | [`29_program_hello.py`](29_program_hello.py) | yes | A JavaScript Program defines one graph with a Python node and runs it on `LocalProgramHost`; prints `status: Completed`, `value: 1`, `trace: ['work']`. |
| 30 | [`30_program_barrier_join.py`](30_program_barrier_join.py) | yes | Three reviewers run in parallel and a `barrier` waits for all of them. When one reviewer routes elsewhere, the Program ends `Failed` with `P_CORE_BARRIER_STALLED` and a witness naming the missing node. |
| 31 | [`31_program_any_of_join.py`](31_program_any_of_join.py) | yes | A join over two plain members stalls when a fix-up node takes over for one of them. The compiler's own suggestion, an `{any_of: [...]}` slot, is applied to the source and the repaired Program completes. |
| 32 | [`32_program_failure_routing.py`](32_program_failure_routing.py) | yes | TEST fails, is retried twice, and its final failure is routed to a handler with `interruptBefore`. The operator resumes with `retry` and the Program completes, or with `abort` and a failure exit ends it `Failed` with `plan.user_abort`. |
| 33 | [`33_program_budgets.py`](33_program_budgets.py) | yes | Two bound `llm_call` nodes report 600 tokens each under a 1,000-token budget, so the run ends `BudgetExhausted`. A run whose budget differs from the admitted one is refused with `P_START_BUDGET`. |
| 34 | [`34_program_pause_resume.py`](34_program_pause_resume.py) | yes | `interruptBefore` pauses a run before node b and `host.resume(run_id, value)` continues it. The paused trace is `['a']`, the resumed one `['b', 'c']`, and `main()` finishes with the operator's answer. |
| 35 | [`35_program_fork.py`](35_program_fork.py) | yes | A run paused next to a finished model call is forked with a different answer. The finished call is replayed from the checkpoint, so the scripted model sees one request in total and the fork is charged no tokens. |
| 36 | [`36_program_crash_recovery.py`](36_program_crash_recovery.py) | yes | Process A runs a Program to a question, is answered, makes its model call and dies. Process B opens the same `sqlite_directory` and calls `reconnect`: the run finishes, the journaled reply is replayed, and the call is charged once. Linux and macOS. |
| 37 | [`37_program_model_node.py`](37_program_model_node.py) | yes | A Program's `llm_call` node is bound to a provider by the host through `node_context`. The scripted model reports 30 + 12 tokens, which land in `usage.model_tokens: 42`, in `remaining_budget` and in a priced monetary record. A host without the binding refuses the Program with `P_ADMIT_PROVIDER`. |
| 38 | [`38_program_diagnostics.py`](38_program_diagnostics.py) | yes | A scripted model misspells a node in an edge. Compilation fails with `P_CORE_E3`, a suggestion and candidates. A repair loop applies the suggestion, recompiles with `soundness_check=True` and admits the Program; the model is called once. |
| 39 | [`39_program_main_race.py`](39_program_main_race.py) | yes | `main()` starts Core runs under `ng.race`, `ng.quorum` and `ng.await`. The first run to finish wins, the slow one sees its cancel token flip, and an `ng.await` that passes its deadline ends the Program `TimedOut` with `P_AWAIT_TIMEOUT`. |
| 40 | [`40_program_attempt_timeout.py`](40_program_attempt_timeout.py) | yes | A node that takes five seconds is stopped by `attempt_timeout_ms`, fails with `NG.Timeout` and succeeds on the retry; `usage.node_retries` is 1. A node that is always slow ends the Program `Failed` with `NG.Timeout`. |

### Protocols: MCP, A2A, ACP

MCP and A2A are clients here (`ng.mcp`, `ng.a2a`). A2A and ACP agents are hosted with the official Python SDKs and `ProtocolHostAdapter`. Each example in this tier runs offline: it starts its own server on a free port or runs the agent as a child process. The SDK ones need `neograph-engine[a2a]` or `neograph-engine[acp]` and are skipped when it is missing; `bindings/python/tests/test_protocol_sdk_e2e.py` tests the servers in more depth.

| # | Example | Offline | Shows |
|---|---|---|---|
| 26 | [`26_mcp_tools.py`](26_mcp_tools.py) | yes | Starts a local MCP server, discovers `fetch` and runs three calls with an explicit re-entrant policy; needs an MCP-enabled build and is smoke-tested on Linux. |
| 27 | [`27_a2a_server.py`](27_a2a_server.py) | yes | Hosts an agent through the official A2A SDK: an agent card and JSON-RPC on `127.0.0.1:9999`, streamed artifacts. `--self-check` serves on a free port and has two turns with the SDK's own client. Needs `neograph-engine[a2a]`. |
| 28 | [`28_acp_agent.py`](28_acp_agent.py) | yes | Hosts an agent through the official ACP SDK over stdio: initialize, create a session, prompt, token updates, `end_turn`. `--self-check` runs the agent as a child process and has two turns with the SDK's client. Needs `neograph-engine[acp]`. |
| 45 | [`45_a2a_client.py`](45_a2a_client.py) | yes | A2A client: `ng.a2a.A2AClient` fetches the agent card, sends a message and reads the stream from the SDK-hosted agent of example 27, which the script starts in a thread on a free port. Needs `neograph-engine[a2a]`. |

### Persistence and operations

Checkpoints, state, caching, tracing, transport and tool concurrency.

| # | Example | Offline | Shows |
|---|---|---|---|
| 07 | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | yes | A checkpoint pauses before payment dispatch; approval resumes exactly one simulated payment. |
| 09 | [`09_state_management.py`](09_state_management.py) | yes | `get_state` and `fork`: alpha reaches count 11, beta forks that value, an unknown thread has no checkpoint. |
| 18 | [`18_node_cache.py`](18_node_cache.py) | key | Five runs across two topics execute the provider-backed node twice; repeated inputs replay cached writes. |
| 20 | [`20_otel_tracing.py`](20_otel_tracing.py) | yes | OpenTelemetry spans for the run and three nodes on the console; the final trail is `['A', 'B', 'C']`. Needs `opentelemetry-api` and `opentelemetry-sdk`. |
| 21 | [`21_http2_transport.py`](21_http2_transport.py) | key | HTTP/1.1 against HTTP/2 through the same SDK; needs a TLS Chat endpoint and libcurl with HTTP/2. The result describes this endpoint, not a ranking. |
| 24 | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) | yes | A tool gate pauses before either sibling tool runs; refusal runs only `list_files`, approval runs both once. Shell actions are simulated. |
| 25 | [`25_async_tools.py`](25_async_tools.py) | yes | A sync `Tool` against an overlapping `AsyncTool` on I/O-bound work, then the GIL boundary on CPU-bound work; timing ratios are observations, and the smoke test runs on Linux. |

## Offline, scripted and live

Model-driven examples build their provider with `example_provider(script)` from `_common.py`. Unless `--live` is given, `_scripted.ScriptedModel` answers: a small server on `127.0.0.1` that speaks the OpenAI Chat or Responses protocol from a script (text, tool calls, streaming, usage, HTTP failures). The real `SchemaProvider` request building, SSE parsing and outcome handling run against it. It is not a model: the script decides what is said.

```python
from _common import example_provider
from _scripted import call_tool, say

script = [call_tool("calc", expression="2 + 3"), say("5")]   # call 1 asks for a tool, call 2 answers
with example_provider(script) as chosen:
    provider = chosen.provider        # a SchemaProvider; chosen.scripted is None with --live
```

A script can also be a function `(turn) -> Reply` that reads `turn.last_user_text`, `turn.system_text` and `turn.last_tool_result`. `chosen.scripted.call_count` lets an example assert how many model calls happened. The rules are in the docstring of `_scripted.py`.

`--live`, `NG_EXAMPLE_LIVE=1`, `OPENAI_API_BASE` or `NG_PROVIDER_DESCRIPTOR` select a configured endpoint instead. Hosted endpoints need `OPENAI_API_KEY` and optionally `OPENAI_MODEL` (default `gpt-4.1-mini`), read from the process environment only; `_common.py` loads no `.env` file, and a missing key is an error, not a successful run. `_common.py` admits a closed OpenAI Chat or Responses descriptor, then constructs `SchemaProvider`. The typed SDK uses libcurl HTTP; the removed provider classes, completion parameters, WebSocket path and transport-backend selector are not available. `OPENAI_API_BASE` is an origin or gateway prefix, not a full `/v1/chat/completions` route; an HTTPS peer also needs `NG_EXAMPLE_CA_FILE` when its CA is not in the system trust store. `NG_PROVIDER_DESCRIPTOR` can name a complete admitted descriptor JSON file instead. See [the Python binding guide](../../../docs/python-binding.md) for the typed request and outcome contract.

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

## Add an example

Numbers are never reused, so take one higher than every existing number. A new example is one file and one table row.

1. Name the file `NN_short_name.py` and start it with a module docstring: what it shows in a few lines, the `Run:` commands, then the test annotations.
2. Say how it is tested, in that docstring. An offline example names a line it must print; one that needs a key or a service says why it cannot run offline (the annotation grammar is at the top of `examples/run_example_smoke.py`).
3. Build any model-driven provider with `example_provider(script)` so the example is offline by default and takes `--live`.
4. Add a row, with its Offline value, to the table of its tier.
5. Run `python -m pytest bindings/python/tests/test_examples_offline.py` and `python scripts/check_examples.py`. The second fails when an example has no annotation, a number is used twice, or a table disagrees with the annotations.

```python
"""NN — one line saying what this shows.

Run:
    python NN_short_name.py           # scripted model, no key
    python NN_short_name.py --live    # hosted model; OPENAI_API_KEY in the environment

smoke-expect: provider: scripted
smoke-expect: final answer: 5
"""
```

Use `smoke-skip: <reason>` instead of the `smoke-expect` lines when the example cannot run offline; `smoke-requires: module1, module2` skips the test when a module is missing.
