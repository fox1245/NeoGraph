# C++ API examples

Thirty-six runnable C++ programs covering the NeoGraph engine surface.
Each is a single file in this directory (with one Docker-Compose
exception, [`26_postgres_react_hitl/`](26_postgres_react_hitl/)) — copy
one into your own project, link against `neograph::core` +
`neograph::llm`, and you have a starting point.

## Build

The default CMake build builds every example:

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

Pass `-DNEOGRAPH_BUILD_EXAMPLES=OFF` to skip them. Examples that need
extra deps (Crawl4AI Docker, Postgres, MCP servers, Clay+Raylib) are
gated by an explicit CMake option or a runtime probe — see the
"Setup" column below.

## Setup

Examples that hit a real LLM auto-load `.env` from the cwd (or any
parent) via cppdotenv. The two recognised keys are:

```
OPENAI_API_KEY=sk-...
ANTHROPIC_API_KEY=sk-ant-...
```

Examples without a "Setup" entry below need no API key — they use the
in-process `MockProvider` or pure mock nodes.

## Start here

If this is your first time:

| First | What you learn |
|---|---|
| [`02_custom_graph.cpp`](02_custom_graph.cpp) | Compile a JSON graph definition, run it. No API key. |
| [`05_parallel_fanout.cpp`](05_parallel_fanout.cpp) | Async fan-out with `make_parallel_group`. No API key. |
| [`10_send_command.cpp`](10_send_command.cpp) | `Send` (dynamic fan-out) + `Command` (routing override). No API key. |
| [`01_react_agent.cpp`](01_react_agent.cpp) | ReAct loop with a real LLM + a calculator tool. **Needs `OPENAI_API_KEY`.** |
| [`14_plan_executor.cpp`](14_plan_executor.cpp) | Plan → parallel sub-tasks → solver, with crash-recovery via the checkpoint store. No API key. |

Once those make sense, the rest below is grouped by what they
demonstrate, not by file number.

## Index

### Core engine — graph, state, routing

| # | File | Setup | What it shows |
|---|------|-------|---------------|
| 02 | [`02_custom_graph.cpp`](02_custom_graph.cpp) | offline | Compile a JSON graph + run it. The shortest useful program in this repo. |
| 05 | [`05_parallel_fanout.cpp`](05_parallel_fanout.cpp) | offline | Async fan-out — three "researcher" nodes co-run on one io_context, summarizer fans them in. |
| 06 | [`06_subgraph.cpp`](06_subgraph.cpp) | offline | Hierarchical composition — outer supervisor graph delegates to an inner ReAct subgraph. |
| 07 | [`07_intent_routing.cpp`](07_intent_routing.cpp) | offline | Classifier → conditional edge → math / translate / general expert. |
| 08 | [`08_state_management.cpp`](08_state_management.cpp) | offline | `get_state` / `update_state` / `fork` — LangGraph's Checkpointer API mapped to C++. |
| 09 | [`09_all_features.cpp`](09_all_features.cpp) | offline | Six features in one demo — `NodeInterrupt`, `RetryPolicy`, `StreamMode`, `Send`, `Command`, `Store`. |
| 10 | [`10_send_command.cpp`](10_send_command.cpp) | offline | Planner→Send→researcher→Command(loop|finish) — the canonical Send+Command pattern. |

### Real LLM — providers, tools, ReAct

| # | File | Setup | What it shows |
|---|------|-------|---------------|
| 01 | [`01_react_agent.cpp`](01_react_agent.cpp) | OpenAI | The ReAct loop: `llm_call` ↔ `tool_dispatch` with `has_tool_calls` conditional. Calculator tool. |
| 12 | [`12_rag_agent.cpp`](12_rag_agent.cpp) | OpenAI | RAG with real `text-embedding-3-small` embeddings + in-memory cosine search. |
| 13 | [`13_openai_responses.cpp`](13_openai_responses.cpp) | OpenAI | Same ReAct loop, but wired to `/v1/responses` via `SchemaProvider("openai_responses")`. One config flip, no provider subclass. |
| 33 | [`33_openai_responses_ws.cpp`](33_openai_responses_ws.cpp) | OpenAI | Responses API over WebSocket — `wss://api.openai.com/v1/responses`. ~40% lower latency on multi-tool agentic loops. |
| 34 | [`34_openai_responses_ws_tools.cpp`](34_openai_responses_ws_tools.cpp) | OpenAI | Tour every Responses-API built-in tool — web_search, image_generation, file_search, tool_search, skills, shell. Wire-level (no SchemaProvider). |
| 29 | [`29_responses_envelope.cpp`](29_responses_envelope.cpp) | OpenAI | Debug aid: dump the raw `/v1/responses` JSON envelope for one tool-call request. Bypasses SchemaProvider on purpose. |
| 30 | [`30_reasoning_effort.cpp`](30_reasoning_effort.cpp) | OpenAI | Sweep `reasoning_effort` in `{minimal, low, medium, high}` on one prompt — see latency / hidden CoT tokens / answer quality tradeoff. |

### Reasoning patterns

| # | File | Setup | Pattern |
|---|------|-------|---------|
| 15 | [`15_reflexion.cpp`](15_reflexion.cpp) | Anthropic | Reflexion — generator ↔ critic loop until the critic says ACCEPT (Shinn et al. 2023). Haiku constraint task. |
| 16 | [`16_tree_of_thoughts.cpp`](16_tree_of_thoughts.cpp) | Anthropic | Tree of Thoughts — at each depth, generate N candidate thoughts, score them, keep top-K, expand. Game of 24. |
| 17 | [`17_self_ask.cpp`](17_self_ask.cpp) | Anthropic | Self-Ask — explicit "Are follow-up questions needed?" decomposition for multi-hop reasoning (Press et al. 2022). |
| 18 | [`18_multi_agent_debate.cpp`](18_multi_agent_debate.cpp) | Anthropic | Researcher / Skeptic / Judge — three system prompts, shared transcript, judge adjudicates. |
| 19 | [`19_rewoo.cpp`](19_rewoo.cpp) | Anthropic | REWOO — planner commits a full plan with `#E1 / #E2` placeholders, worker fans out tools in parallel, solver synthesizes. |

### Persistence & HITL

| # | File | Setup | What it shows |
|---|------|-------|---------------|
| 04 | [`04_checkpoint_hitl.cpp`](04_checkpoint_hitl.cpp) | offline | `interrupt_before` a payment node, persist checkpoint, resume after operator approval. Mock provider. |
| 14 | [`14_plan_executor.cpp`](14_plan_executor.cpp) | offline | Plan-and-Executor with simulated mid-fan-out failure — checkpoint replay only re-runs the failed sibling. Pending-writes machinery in action. |
| 26 | [`26_postgres_react_hitl/`](26_postgres_react_hitl/) | OpenAI WS + Postgres + Crawl4AI | Process-discontinuous deep-research HITL — PG-backed checkpoints survive `exit` between report and resume. Docker-Compose driven. |

### MCP (Model Context Protocol)

| # | File | Setup | What it shows |
|---|------|-------|---------------|
| 03 | [`03_mcp_agent.cpp`](03_mcp_agent.cpp) | OpenAI + MCP HTTP server | Discover tools from a streamable-http MCP server, drive a ReAct loop. |
| 22 | [`22_mcp_stdio.cpp`](22_mcp_stdio.cpp) | OpenAI + Python stdio script | Same as 03 but the MCP server is a child subprocess over stdin/stdout — no network stack. |
| 23 | [`23_mcp_multi.cpp`](23_mcp_multi.cpp) | OpenAI + 2 servers | One agent, two MCP servers (HTTP + stdio), tools merged into one list — LLM picks across both transparently. |
| 21 | [`21_mcp_fanout.cpp`](21_mcp_fanout.cpp) | MCP HTTP server (no LLM) | Planner emits one Send per MCP call; `make_parallel_group` runs them concurrently. Deterministic — LLM hand-picks tools so the demo stays offline on the LLM axis. |
| 20 | [`20_mcp_hitl.cpp`](20_mcp_hitl.cpp) | OpenAI + MCP HTTP server | `interrupt_before` any MCP tool call — operator sees the pending tool name + args, approves, resumes. |
| 24 | [`24_mcp_feedback.cpp`](24_mcp_feedback.cpp) | OpenAI + MCP HTTP server | Operator reads the agent's draft answer and types feedback; the second run incorporates that feedback as new conversational context. |

### Async, concurrency, performance

| # | File | Setup | What it shows |
|---|------|-------|---------------|
| 27 | [`27_async_concurrent_runs.cpp`](27_async_concurrent_runs.cpp) | offline | Three agent runs interleave on one `io_context` thread via `engine->run_async()` — wall ≈ 50 ms instead of 3×50 ms. Stage-4 async-end-to-end. |

### Deep research / RAG variants

| # | File | Setup | What it shows |
|---|------|-------|---------------|
| 25 | [`25_deep_research.cpp`](25_deep_research.cpp) | Anthropic + Crawl4AI Docker | C++ port of `langchain-ai/open_deep_research`. Supervisor plans, fans out parallel sub-researchers (each its own ReAct loop), synthesizes a markdown report. |
| 28 | [`28_corrective_rag.cpp`](28_corrective_rag.cpp) | OpenAI | CRAG (Yan et al. 2024). Retrieve → grade → route to refine(KB) / refine+web / web-only depending on relevance. Web search via `/v1/responses` built-in tool. |

### Local / hybrid LLM backends

| # | File | Setup | What it shows |
|---|------|-------|---------------|
| 31 | [`31_local_transformer.cpp`](31_local_transformer.cpp) | local server (TransformerCPP / llama.cpp / vLLM) | Point `OpenAIProvider` at `http://localhost:8090`. Two-process split keeps model weights out of the agent's address space. |
| 32 | [`32_inproc_gemma.cpp`](32_inproc_gemma.cpp) | TransformerCPP linked, GGUF file | Fully-local: TransformerCPP linked into the NeoGraph process, no HTTP. Inline `Provider` adapter — ~60 lines. The "any C++ inference runtime can plug in" demo. |

### Showcase

| # | File | Setup | What it shows |
|---|------|-------|---------------|
| 11 | [`11_clay_chatbot.cpp`](11_clay_chatbot.cpp) | Clay + Raylib (`-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON`) | Multi-turn chat with a Clay/Raylib UI. Pure-C++ desktop app, NeoGraph backend. Mock or `--live`. |
| 35 | [`35_re_agent.cpp`](35_re_agent.cpp) | OpenAI + Ghidra + ghidra-mcp | Reverse-engineering agent — recovers function names + summaries from a stripped binary via Ghidra. End-to-end verified (matched_score 0.92, 6-fn crackme). Full pipeline in [`fox1245/re-agent`](https://github.com/fox1245/re-agent). |
| 36 | [`36_classifier_fanout.cpp`](36_classifier_fanout.cpp) | offline | Five small "classifiers" (sentiment / toxicity / language / topic / intent) fan out via Send and run in parallel. Wall time ≈ max(per-classifier), not sum — the small-model edge story. Mock 5 ms latency stand-in for a DistilBERT/MiniLM pass; inline `[ONNX SWAP-IN]` block shows the 30-line replacement using `Ort::Session`. No inference runtime dependency. |

## Mental model — three layers, JSON in the middle

Each example is one of three setups:

1. **Built-in nodes only** (02, 04, 07, 14): `llm_call` / `tool_dispatch`
   / mock-provider node — graph wired entirely from JSON, no
   subclassing. Closest to what `create_react_graph()` produces.
2. **Custom `GraphNode` subclass** (05, 09, 10, 25): you control the
   exact `execute_full(state)` body — emit `ChannelWrite`,  `Send`,
   `Command`. This is where Send fan-out and Command routing overrides
   live.
3. **`SchemaProvider` for non-OpenAI shapes** (13, 15, 16, 17, 33):
   one JSON schema describes the wire shape (Anthropic, Gemini, the
   OpenAI Responses API, raw WebSocket), so the same `Agent` /
   `llm_call` node hits a different endpoint without subclassing.

The graph definition is JSON-shaped (`std::map<std::string, json>`)
either way — examples 14 and 15 in the [Python examples](../bindings/python/examples/)
show how the same definition round-trips through `json.dumps` and back.

## API key economy

| Provider | Examples |
|---|---|
| `OPENAI_API_KEY` | 01, 03, 12, 13, 20, 22, 23, 24, 28, 29, 30, 33, 34, 35 |
| `ANTHROPIC_API_KEY` | 15, 16, 17, 18, 19, 25 |
| local server (no key) | 31, 32 |
| **none** | 02, 04, 05, 06, 07, 08, 09, 10, 14, 21, 27, 36 |

Twelve examples run with no API key — that is the "kick the tyres"
floor. Examples 21 (MCP fanout, deterministic planner) and 27 (async
concurrency, `steady_timer` stand-in for LLM latency) in particular
demonstrate engine plumbing without spending a token.

## Re-running after CMake config

Built binaries land in the build directory's root, named
`example_<short_name>` (e.g. `example_react_agent`,
`example_custom_graph`). The exact name is in each `.cpp`'s top
comment under `Usage:`.
