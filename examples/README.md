# C++ API examples

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

## Typed C++ cutover status

The current C++ recipes use owned `ProviderRequest` payloads from the five typed
SDK families, ordered `sp::Event` observations and immutable
`std::shared_ptr<const sp::Outcome>` terminals (`Completion` or `Failure`).
`ChatMessage` / `ChatTool` are portable projections, not native replay authority.
Prepare exactly once; durable callers bind their claim/receipt to
`Provider::request_digest(prepared)` and dispatch that same prepared handle.
Do not reconstruct history from printed JSON or flatten failures to final text.

Canonical persistence uses `provider-message-v2` and `runtime-history-record-v2`;
portable summaries never replace native records. Optional controls are caller-chosen,
not silently clamped. Bounded calls require genuine model facts; missing facts are
`LimitUnknown`. Reservation/charged/held amounts are distinct from nullable provider
usage and do not renew a budget or represent a price, forecast or invoice.

The header-only `examples/provider_example_support.h` helper uses the real SDK
runtime; it is not a replacement transport. Every Core build, including
`NEOGRAPH_BUILD_LLM=OFF`, requires `SchemaProvider::runtime`. With CMake 3.20+,
SDK resolution uses an explicit `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` first,
then an installed runtime package, then the pinned public GitHub source archive.
The download fallback is enabled by default; set `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`
for offline builds with an installed package or explicit source checkout.
Installed SDK include root is `include/SchemaProvider`. Interface/capability
checks are enforced; the SDK package is alpha `0.1.1` (interface/shared ABI 4).

The preserved interface-3 model-free C++ E2E run verified 39 numbered targets: 29 finite offline
targets plus actual MCP/ACP/A2A/Harness and gRPC graph/checkpoint/tool paths.
The gRPC-vs-JSON-RPC measurement example also ran, but its unchecked return values
are not a behavioral E2E pass. Twenty-two live/external-model scopes and the
disabled Clay GUI remain unqualified; no public vendor request or new grant was used.
Historical timings below are not new-cutover qualification. Live runs incur
provider charges and require explicit keys/network/model access. Keep keys,
prompts and artifacts private; envelope/native inspection demos print sensitive
payloads and must not feed public logs. A native archive provides authenticated,
owner-private custody, not encryption or vendor-issuer authentication.

Current interface-4 evidence is separate: Linux x86_64 runs exercised native
research recovery, ToT/Forge/rewrite helpers and A2A 0.3/1.0 peers, plus 11 installed
Python applications across 48 credential-free localhost requests in split cohorts.
The tracked evolution file mode and Plan resume were also executed as described
below. These are scoped local runs, not a repeat of the preserved full C++ suite.
See the [SDK interface-4 execution record](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/CONFORMANCE.md#interface-4-execution-record).
No new hosted-vendor, Windows/macOS/ARM64, HTTP/3 or sanitizer qualification is
claimed; remote CI and publication remain pending.



The numbered examples cover the NeoGraph engine surface, with one focused Core
quickstart and one focused Program quickstart.
Each is a single file in this directory (with one Docker-Compose
exception, [`26_postgres_react_hitl/`](26_postgres_react_hitl/)) — copy
one into your own project, link against `neograph::core` +
`neograph::llm`, and you have a starting point.

## Build

The default CMake configuration builds the examples supported by the enabled
components. The Program quickstart and Program-backed examples require
`-DNEOGRAPH_BUILD_PROGRAM=ON`; gRPC and Python bindings are opt-in, and
examples behind those components are omitted unless their options are enabled.

```bash
cmake -S . -B build -DNEOGRAPH_BUILD_EXAMPLES=ON
cmake --build build -j$(nproc)
```

To include Program-backed and A2A examples, also enable these components:

```bash
cmake -S . -B build \
  -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_A2A=ON
cmake --build build -j$(nproc)
```

Pass `-DNEOGRAPH_BUILD_EXAMPLES=OFF` to skip examples. Examples that need
extra deps (Crawl4AI Docker, Postgres, MCP servers, Clay+Raylib) are gated
by an explicit CMake option or a runtime probe — see the
"Setup" column below.

## Setup

Examples that hit a real LLM auto-load `.env` from the cwd (or any
parent) via cppdotenv. The live examples use one key:

```
OPENROUTER_API_KEY=sk-or-...
```

Examples without a "Setup" entry below need no API key — they use the
in-process `MockProvider` or pure mock nodes.

## Start here

If this is your first time:

| First | What you learn |
|---|---|
| [`62_core_quickstart.cpp`](62_core_quickstart.cpp) | **Core quickstart** — the installed `neograph::core` target, one strict graph, one typed channel. No optional component or API key. |
| [`63_program_quickstart.cpp`](63_program_quickstart.cpp) | **Program quickstart** — compile, admit, and run one `call_core` Program through the installed `neograph::program` target. Requires `-DNEOGRAPH_BUILD_PROGRAM=ON`. |
| [`51_minimal.cpp`](51_minimal.cpp) | The smallest working program — build, run, read `result.channel<T>("name")`. No API key. |
| [`02_custom_graph.cpp`](02_custom_graph.cpp) | Build a JSON graph definition, run it. No API key. |
| [`05_parallel_fanout.cpp`](05_parallel_fanout.cpp) | Async fan-out with `make_parallel_group`. No API key. |
| [`10_send_command.cpp`](10_send_command.cpp) | `Send` (dynamic fan-out) + `Command` (routing override). No API key. |
| [`01_react_agent.cpp`](01_react_agent.cpp) | ReAct loop with a real LLM + a calculator tool. **Needs `OPENROUTER_API_KEY`.** |
| [`14_plan_executor.cpp`](14_plan_executor.cpp) | Plan → parallel sub-tasks → solver, with crash-recovery via the checkpoint store. No API key. |

Once those make sense, the rest below is grouped by what they
demonstrate, not by file number.

## Index

### Core engine — graph, state, routing

| # | File | Setup | What it shows |
|---|------|-------|---------------|
| 02 | [`02_custom_graph.cpp`](02_custom_graph.cpp) | offline | Build a JSON graph + run it. The shortest useful program in this repo. |
| 05 | [`05_parallel_fanout.cpp`](05_parallel_fanout.cpp) | offline | Async fan-out — three "researcher" nodes co-run on one io_context, summarizer fans them in. |
| 06 | [`06_subgraph.cpp`](06_subgraph.cpp) | offline | Hierarchical composition — outer supervisor graph delegates to an inner ReAct subgraph. |
| 07 | [`07_intent_routing.cpp`](07_intent_routing.cpp) | offline | Classifier → conditional edge → math / translate / general expert. |
| 08 | [`08_state_management.cpp`](08_state_management.cpp) | offline | `get_state` / `update_state` / `fork` — LangGraph's Checkpointer API mapped to C++. |
| 09 | [`09_all_features.cpp`](09_all_features.cpp) | offline | Six features in one demo — `NodeInterrupt`, `RetryPolicy`, `StreamMode`, `Send`, `Command`, `Store`. |
| 10 | [`10_send_command.cpp`](10_send_command.cpp) | offline | Planner→Send→researcher→Command(loop|finish) — the canonical Send+Command pattern. |
| 42 | [`42_custom_reducer_condition.cpp`](42_custom_reducer_condition.cpp) | offline | Register custom channel reducers and edge conditions from C++ — extend the JSON vocabulary without touching the engine. |
| 43 | [`43_store_personalization.cpp`](43_store_personalization.cpp) | offline | Cross-thread `Store` reached from inside a node via `in.ctx.store` — per-user node behaviour from shared namespaced memory. |
| 51 | [`51_minimal.cpp`](51_minimal.cpp) | offline | The shortest working program — build, run, `result.channel<T>("name")`. The fresh-user template. |
| 52 | [`52_export_schema.cpp`](52_export_schema.cpp) | offline | `NodeFactory::export_schema()` → topology JSON Schema dump. The version-locked source of truth a codeless visual editor builds its palette from. |
| 56 | [`56_history_compaction.cpp`](56_history_compaction.cpp) | offline (optional OpenRouter) | Bounded message window — when history exceeds the budget the dropped prefix is replaced by an LLM-written summary. Mock provider by default. |

### Real LLM — providers, tools, ReAct

| # | File | Setup | What it shows |
|---|------|-------|---------------|
| 01 | [`01_react_agent.cpp`](01_react_agent.cpp) | OpenRouter | The ReAct loop: `llm_call` ↔ `tool_dispatch` with `has_tool_calls` conditional. Calculator tool. |
| 12 | [`12_rag_agent.cpp`](12_rag_agent.cpp) | OpenRouter | RAG with real OpenRouter-compatible embeddings + in-memory cosine search. |
| 13 | [`13_openrouter_responses_sse.cpp`](13_openrouter_responses_sse.cpp) | OpenRouter | Typed Responses SSE request, ordered `sp::Event` observations and owned `sp::Outcome`. |
| 34 | [`34_openrouter_responses_tools_sse.cpp`](34_openrouter_responses_tools_sse.cpp) | OpenRouter | All seven typed hosted-tool sections over SSE; full Outcomes and ordered wire observations. |
| 29 | [`29_responses_envelope.cpp`](29_responses_envelope.cpp) | OpenRouter | Debug aid: dump the raw `/api/v1/responses` JSON envelope for one tool-call request. |
| 30 | [`30_reasoning_effort.cpp`](30_reasoning_effort.cpp) | OpenRouter | Sweep the pinned DeepSeek reasoning-effort values on one prompt — see latency / reasoning-token tradeoffs. |

### Reasoning patterns

| # | File | Setup | Pattern |
|---|------|-------|---------|
| 15 | [`15_reflexion.cpp`](15_reflexion.cpp) | OpenRouter | Reflexion — generator ↔ critic loop until the critic says ACCEPT (Shinn et al. 2023). |
| 16 | [`16_tree_of_thoughts.cpp`](16_tree_of_thoughts.cpp) | OpenRouter | Tree of Thoughts — generate, score, retain, and expand candidate thoughts. |
| 17 | [`17_self_ask.cpp`](17_self_ask.cpp) | OpenRouter | Self-Ask — explicit follow-up decomposition for multi-hop reasoning (Press et al. 2022). |
| 18 | [`18_multi_agent_debate.cpp`](18_multi_agent_debate.cpp) | OpenRouter | Researcher / Skeptic / Judge — three system prompts and a shared transcript. |
| 19 | [`19_rewoo.cpp`](19_rewoo.cpp) | OpenRouter | REWOO — planner placeholders, parallel tool workers, and solver synthesis. |

### Persistence & HITL

| # | File | Setup | What it shows |
|---|------|-------|---------------|
| 04 | [`04_checkpoint_hitl.cpp`](04_checkpoint_hitl.cpp) | offline | `interrupt_before` a payment node, persist checkpoint, resume after operator approval. Mock provider. |
| 14 | [`14_plan_executor.cpp`](14_plan_executor.cpp) | offline | Plan-and-Executor with simulated mid-fan-out failure — checkpoint replay only re-runs the failed sibling. Pending-writes machinery in action. |
| 26 | [`26_postgres_react_hitl/`](26_postgres_react_hitl/) | OpenRouter + Postgres + Crawl4AI | Process-discontinuous deep-research HITL — PG-backed checkpoints survive `exit` between report and resume. Docker-Compose driven. |
| 41 | [`41_resume_if_exists_chat.cpp`](41_resume_if_exists_chat.cpp) | offline | LangGraph-style multi-turn chat — `resume_if_exists` reloads the prior checkpoint and appends the new turn. Mock provider. |
| 48 | [`48_sqlite_checkpoint.cpp`](48_sqlite_checkpoint.cpp) | offline | SQLite `:memory:` checkpoint/resume and thread isolation; no file/process-restart durability claim. |

### MCP (Model Context Protocol)

| # | File | Setup | What it shows |
|---|------|-------|---------------|
| 03 | [`03_mcp_agent.cpp`](03_mcp_agent.cpp) | OpenRouter + MCP HTTP server | Discover tools from a streamable-http MCP server, drive a ReAct loop. |
| 22 | [`22_mcp_stdio.cpp`](22_mcp_stdio.cpp) | OpenRouter + Python stdio script | Same as 03 but the MCP server is a child subprocess over stdin/stdout — no network stack. |
| 23 | [`23_mcp_multi.cpp`](23_mcp_multi.cpp) | OpenRouter + 2 servers | One agent, two MCP servers (HTTP + stdio), tools merged into one list — LLM picks across both transparently. |
| 21 | [`21_mcp_fanout.cpp`](21_mcp_fanout.cpp) | MCP HTTP server (no LLM) | A fixed planner emits one Send per MCP call; `make_parallel_group` runs them concurrently. No model call; the MCP server must still be reachable. |
| 20 | [`20_mcp_hitl.cpp`](20_mcp_hitl.cpp) | OpenRouter + MCP HTTP server | `interrupt_before` any MCP tool call — operator sees the pending tool name + args, approves, resumes. |
| 24 | [`24_mcp_feedback.cpp`](24_mcp_feedback.cpp) | OpenRouter + MCP HTTP server | Operator reads the agent's draft answer and types feedback; the second run incorporates that feedback as new conversational context. |

### Async, concurrency, performance

| # | File | Setup | What it shows |
|---|------|-------|---------------|
| 27 | [`27_async_concurrent_runs.cpp`](27_async_concurrent_runs.cpp) | offline | Three agent runs interleave on one `io_context` thread via `engine->run_async()` — wall ≈ 50 ms instead of 3×50 ms. Stage-4 async-end-to-end. |
| 40 | [`40_react_async_streaming.cpp`](40_react_async_streaming.cpp) | OpenRouter | Async ReAct with typed provider events; text deltas are display projections, not native history. |
| 44 | [`44_request_queue_backpressure.cpp`](44_request_queue_backpressure.cpp) | offline | Fixed-worker pool with backpressure (`neograph::util::RequestQueue`) — bounded in-flight work, no unbounded growth under load. |
| 46 | [`46_cancel_token.cpp`](46_cancel_token.cpp) | offline | Cooperative cancellation — `CancelToken::fork()` per child, parent `cancel()` cascades to all in-flight children. |
| 47 | [`47_node_cache.cpp`](47_node_cache.cpp) | offline | Per-node result cache keyed on node + input — skip recompute on identical inputs across runs. |
| 50 | [`50_async_tool.cpp`](50_async_tool.cpp) | offline | `AsyncTool` — coroutine-shaped tool execution adapter, so a tool can `co_await` without blocking the io_context. |

### Agent interop — A2A & ACP

| # | File | Setup | What it shows |
|---|------|-------|---------------|
| 38 | [`38_a2a_server.cpp`](38_a2a_server.cpp) | offline | Expose a compiled NeoGraph as an Agent-to-Agent endpoint (HTTP, streaming SSE). Run this first. |
| 37 | [`37_a2a_client.cpp`](37_a2a_client.cpp) | offline (needs example 38 running) | Drive a *remote* A2A agent — `A2ACallerNode` makes a remote agent look like a local node. |
| 39 | [`39_acp_server.cpp`](39_acp_server.cpp) | offline | Expose a NeoGraph over the Agent Client Protocol — bidirectional JSON-RPC over stdio, the shape an editor (Zed-style) drives. |

### Distributed — gRPC service & remote checkpoint/tool

Built only with `-DNEOGRAPH_BUILD_GRPC=ON` (needs `grpc++` / `protoc`).

| # | File | Setup | What it shows |
|---|------|-------|---------------|
| 52 | [`52_grpc_server.cpp`](52_grpc_server.cpp) | offline (grpc++) | Expose a `GraphEngine` over gRPC — per-distinct-graph engines compiled lazily and cached. |
| 53 | [`53_grpc_client.cpp`](53_grpc_client.cpp) | offline (grpc++) | Call a NeoGraph gRPC `GraphService` from a C++ client. |
| 54 | [`54_grpc_checkpoint.cpp`](54_grpc_checkpoint.cpp) | offline (grpc++) | `GrpcCheckpointStore` — a remote `CheckpointStore` across a network boundary, with an honest latency measurement. |
| 55 | [`55_grpc_vs_jsonrpc_toolcall.cpp`](55_grpc_vs_jsonrpc_toolcall.cpp) | offline (grpc++) | Head-to-head: tool-calling over JSON-RPC vs gRPC — the microbench behind "70× was a Nagle artifact". |
| 57 | [`57_grpc_remote_tool.cpp`](57_grpc_remote_tool.cpp) | offline (grpc++) | A tool living in another process exposed as a local `neograph::Tool`. |

### Observability

| # | File | Setup | What it shows |
|---|------|-------|---------------|
| 49 | [`49_openinference.cpp`](49_openinference.cpp) | offline | OpenInference tracer adapter — `graph.run > node.* > llm.complete` lands as one trace tree (12 attributes). Phoenix-verified. Mock provider. |

### Deep research / RAG variants

| # | File | Setup | What it shows |
|---|------|-------|---------------|
| 25 | [`25_deep_research.cpp`](25_deep_research.cpp) | OpenRouter DeepSeek + Crawl4AI Docker | C++ port of `langchain-ai/open_deep_research`. Supervisor plans, fans out parallel sub-researchers (each its own ReAct loop), synthesizes a markdown report. |
| 28 | [`28_corrective_rag.cpp`](28_corrective_rag.cpp) | OpenRouter | CRAG (Yan et al. 2024). Retrieve → grade → route to refine(KB) / refine+web / web-only depending on relevance. Web search via `/api/v1/responses` built-in tool. |

### Local / hybrid LLM backends

| # | File | Setup | What it shows |
|---|------|-------|---------------|
| 31 | [`31_local_transformer.cpp`](31_local_transformer.cpp) | llama.cpp / vLLM | Typed Chat client at `http://localhost:8090`; model weights stay outside the agent process. |

### Showcase

| # | File | Setup | What it shows |
|---|------|-------|---------------|
| 11 | [`11_clay_chatbot.cpp`](11_clay_chatbot.cpp) | Clay + Raylib (`-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON`) | Multi-turn chat with a Clay/Raylib UI. Pure-C++ desktop app, NeoGraph backend. Mock or `--live`. |
| 35 | [`35_re_agent.cpp`](35_re_agent.cpp) | OpenRouter + Ghidra + ghidra-mcp | Reverse-engineering agent — recovers function names + summaries from a stripped binary via Ghidra. Historical end-to-end result: matched_score 0.92 on a 6-function crackme. The full pipeline is maintained separately in the private `fox1245/re-agent` repository. |
| 36 | [`36_classifier_fanout.cpp`](36_classifier_fanout.cpp) | offline | Five small "classifiers" (sentiment / toxicity / language / topic / intent) fan out via Send and run in parallel. Wall time ≈ max(per-classifier), not sum — the small-model edge story. Mock 5 ms latency stand-in for a DistilBERT/MiniLM pass; inline `[ONNX SWAP-IN]` block shows the 30-line replacement using `Ort::Session`. No inference runtime dependency. |

Example 35 requires `GHIDRA_MCP_BRIDGE` to name your `bridge_mcp_ghidra.py` script. `GHIDRA_MCP_PYTHON` selects its interpreter (default `python3`); `GHIDRA_SERVER_URL` selects the plugin endpoint (default `http://127.0.0.1:18080/`). Start Ghidra and its MCP plugin before running the example. The showcase also requires `OPENROUTER_API_KEY` and makes paid model calls; the historical score above is not a new run or a general accuracy guarantee.

## Retained example contracts

These source contracts use SDK interface 4. The recorded C++ runs above remain
historical evidence; they are not interface-4 qualification or new live calls.
Family-specific controls are closed typed fields on `ProviderControls`; unsupported
family/origin/model combinations reject before I/O. See the
[provider reference](../docs/reference-en.md) for reasoning/sampling/tool controls,
Responses provider-held cursors, deployment headers and explicit portable Gemini
history. A cursor or portable history does not grant native replay authority.

### Research and slow reasoning routes

Deep Research (25 / 26) permits at most two additional semantic calls for each
supervisor, researcher, compression or final-report request only after a completed
empty `MaxTokens` outcome with no visible text and no valid or invalid client tool
call. The output cap doubles, never above 16,384. The research-brief call is outside
this ladder. Each additional call has a new ordinal, passes the original bank's
admission and retains its outcome and usage; no grant, hold or deadline is renewed.
Without an explicit deadline, one effect-free preparation discovers the configured
deadline and is released before mediated invocation; that deadline stays pinned.
An explicit deadline skips discovery. Failure, observer/settlement errors and
already-delivered streaming parts do not trigger another call.

An empty final report is an error. Nonempty `MaxTokens` report text is published
with an `Incomplete` annotation only in the public projection; the immutable outcome
is unchanged and the partial text is not retried. Exhausted empty compression yields
a diagnostic, not a fabricated successful provider result.

Example 16 keeps an 8,192-token cap across at most three completed-empty calls and
pins one 300-second deadline per ask; it does not double the cap or retry failures.
Example 28's rewrite requests low effort and 512 output tokens, returns the original
question unchanged when the reply is empty/whitespace, and uses a 180-second provider
timeout. These path-specific settings leave the shared factory default unchanged.

### Checkpoints and evolution

Example 08 retains its terminal-checkpoint fork followed by a new user turn; it
does not demonstrate resuming a paused reviewer. Example 14 computes saved executor
calls from the actual count: five initial calls, one failed-sibling rerun, four saved.

Example 54 registers `pnoop` before selecting either smoke or file mode. From the
repository root, use the tracked seed/task pair:

```bash
./build/example_evolution --smoke
./build/example_evolution examples/54_evolution_seed.json examples/54_evolution_task.json
```

Inspect the actual JSON fields `best.compiled`, `best.validated`, `best.executed`
and `best.correct`; `compile_passed` alone does not establish correct execution.
Built-in node types and this demo's `pnoop` are available in file mode; custom types
still require host registration. The current Linux x86_64 file-mode run using this
seed/task pair returned all four `best` flags true; an unregistered node type
failed without a successful compile/execute/correct result.

### A2A dialects and task snapshots

Example 37 reports the card's interfaces and the dialect selected on its first
RPC. The client selects compatible JSON-RPC 0.x or 1.0 card interfaces; card URLs
do not redirect the configured RPC endpoint. Without a fetched card, a numeric
`-32601` permits the initial dialect probe, never a replay after delivered SSE.
Example 38 advertises both dialects and labels opening/updated task snapshots;
an opening task is not a completed answer. The `A2A-Version` header selects server
response encoding, independently of method spelling. In 1.0, requests use PascalCase
methods, flat parts and `returnImmediately`; streams accumulate status/artifact
updates. Caller answer precedence is final/interrupted agent status text, then first
artifact text, then the last agent history text.

## Mental model — three layers, JSON in the middle

Each example is one of three setups:

1. **Built-in nodes only** (02, 04, 07, 14): `llm_call` / `tool_dispatch`
   / mock-provider node — graph wired entirely from JSON, no
   subclassing. Closest to what `create_react_graph()` produces.
2. **Custom `GraphNode` subclass** (05, 09, 10, 25): you control the
   exact `run(NodeInput)` body — emit `ChannelWrite`, `Send`, or
   `Command` through `NodeOutput`. This is where Send fan-out and
   Command routing overrides live.
3. **Typed provider requests and Outcomes** (13, 15, 16, 17): validated SDK admission, ordered events and immutable Outcomes; no descriptor interpreter or WebSocket adapter.

The graph definition is JSON-shaped (`std::map<std::string, json>`).
The [Python examples](../bindings/python/examples/) use the same topology format;
their provider requests and Outcomes are separate typed objects, not topology JSON.

## API key economy

| Provider | Examples |
|---|---|
| `OPENROUTER_API_KEY` | 01, 03, 12, 13, 15, 16, 17, 18, 19, 20, 22, 23, 24, 25, 28, 29, 30, 34, 35, 40 |
| local server (no key) | 31 |
| **none** | 02, 04, 05, 06, 07, 08, 09, 10, 14, 21, 27, 36, 37, 38, 39, 41, 42, 43, 44, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57 |

Examples 25 and 26 also use local Crawl4AI. Current secure Crawl4AI Docker
images require a non-empty `CRAWL4AI_API_TOKEN`; see example 26's `.env.example`.

Thirty-one examples run with no API key — that is the "kick the tyres"
floor. Examples 21 (MCP fanout, deterministic planner) and 27 (async
concurrency, `steady_timer` stand-in for LLM latency) in particular
demonstrate engine plumbing without spending a token. The gRPC suite
(52–55, 57) is also key-free but needs `-DNEOGRAPH_BUILD_GRPC=ON`
(`grpc++` / `protoc`); 56 (`history_compaction`) defaults to a mock
provider and only touches OpenRouter if a key is present.

## Re-running after CMake config

Built binaries land in the build directory's root, named
`example_<short_name>` (e.g. `example_react_agent`,
`example_custom_graph`). The exact name is in each `.cpp`'s top
comment under `Usage:`.

## Responses inspection contracts (13 / 29 / 30 / 34)

13 submits a typed Responses streaming request; events are not a string-only
callback. 29 retains both successful and failed Outcomes: the full response
`wire_envelope`, every ordered `wire_output` item and typed part, function
arguments, citations, reasoning, opaque hosted output and artifacts. Its raw
inspection output is sensitive, not a sanitized telemetry/export format.
30 retains each full Outcome while sweeping `none`, `low`, `medium`, `high`;
reasoning/input/output/total/provider-reported-total and extra counts are nullable
64-bit evidence with stage, quality and conflicts. Missing is not zero; visible
answer text does not replace the Outcome or turn a reservation into usage.
34 keeps all seven sections: function (calculator), web search, image generation,
file search, tool search, skills mounted in `shell.environment`, and shell
(`container_auto`). `OPENROUTER_VECTOR_STORE_ID` gates file search;
`OPENROUTER_SKILL_ID` overrides the curated `openai-spreadsheets` skill.
Function tools are advertised, not locally executed by this inspection demo.
All ordered typed/raw events and full terminals are retained. Hosted tools may
be unsupported by the chosen route or incur additional charges; typed admission
is not a live compatibility guarantee. No WebSocket/primitive runnable example
is retained. Images/Veo/Decisions are separate typed NeoGraph clients and do not
inherit a chat-run spending grant.
