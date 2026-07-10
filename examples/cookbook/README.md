# NeoGraph Cookbooks

End-to-end recipes that compose multiple NeoGraph features into a
real working scenario. Each one is self-contained: copy the folder,
follow its README, and run.

| Cookbook | What it shows |
|---|---|
| [`the-beast/`](the-beast/) | **A self-evolving agent: generate · evolve · roll back.** The Beast (1) authors a NeoGraph topology in the DSL surface and proves it coherent through the three gates (elaborate → strict compile + translation validation → static validate), (2) evolves it with real mutation operators via `evolve()` using the compiler as the fitness gate, and (3) spawns the survivor with a checkpointer and rewinds its run to any prior super-step (`load_by_id` time-travel). The category only NeoGraph makes safe: the harness is data, and the DSL compiler proves it coherent before a single node runs. Offline stub author, a **live variant** where DeepSeek v4 pro (OpenRouter) actually writes the harness with a compiler-diagnostics self-repair loop, and an **apex variant** where the model devours a tool catalog and authors a ReAct agent that then calls the bound tools autonomously. |
| [`ai-assembly/`](ai-assembly/) | Multi-persona A2A: 4 국회의원 (each its own A2A endpoint) + a Speaker that broadcasts a bill in parallel and tallies votes. Cross-language: C++ member servers + Python or C++ Speaker. |
| [`byo-openai/`](byo-openai/) | Bring your own `openai.OpenAI()` client: subclass NeoGraph's `Provider` to delegate every LLM call into the SDK, keeping all your retries / Azure / observability config. Also: tool calling via the agentic-provider pattern. |
| [`jarvis/`](jarvis/) | **Voice-driven meta-orchestrator (skeleton).** Mic → whisper.cpp (auto-detect language) → router (direct / delegate / parallel 3-way) → MCP tools or A2A specialists → supertonic on-device TTS, in the user's detected language. JSON-driven tool + agent catalogs, A2A bidirectional (JARVIS is itself reachable). On-device, zero cloud required. |
| [`minimal-mcp/`](minimal-mcp/) | MCP client round-trip with **no LLM, no API key, no fastmcp**: a ~60-line stdlib stdio server + a C++ harness that does `initialize` → `tools/list` → `tools/call`. Shows NeoGraph's MCP client only needs a process that speaks the wire protocol — the peer can be anything. |
| [`ollama-provider/`](ollama-provider/) | Local LLM via Ollama. Two paths: built-in `OpenAIProvider` against Ollama's compat endpoint (zero new code), or a custom `Provider` against the native `/api/chat`. Full agent stack with no external API keys. |

Each cookbook also documents the friction it surfaced — useful for
finding the rough edges of the public API.
