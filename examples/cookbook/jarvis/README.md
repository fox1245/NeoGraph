# JARVIS — Voice-Driven Meta-Orchestrator

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

> Cloud-zero dependency, runs on a single Raspberry Pi.
> Microphone is Tony, NeoGraph is JARVIS, tools/experts are JARVIS's subordinates.

This cookbook is **not** a "voice TTS example". It's a demonstration of NeoGraph's
multi-agent primitives — MCP tools, bidirectional A2A, async parallel, Store memory,
ReAct subgraph — **woven together with a single line of voice**.

## Why This Is JARVIS

JARVIS in the movies isn't just a chatbot with voice TTS. JARVIS does five things simultaneously:

1. Grabs intent before Tony finishes speaking — **fast intent classification**
2. Answers directly if possible, otherwise delegates to subordinates — **4-way routing**
3. Gathers multiple pieces of information at once — **parallel fan-out**
4. Remembers yesterday's conversation — **long-term memory**
5. Can be called by other JARVIS/systems — **bidirectional A2A**

So the core of this cookbook is the **graph shape**, not voice. Voice is just the
input/output shell; what creates the "JARVIS feel" is NeoGraph's orchestration engine.

## Full Graph

```
                          ┌────────────────────────┐
                          │ Background triggers    │
                          │ (timer / external events)│ ── A2A server for
                          └───────────┬────────────┘     JARVIS calls go here
                                      │
 [Microphone]──[VAD]──[whisper.cpp STT]──[memory_lookup]──[intent_router]
    miniaudio                          ▲                   │
                                       │ Store             │
                                       │ (conversation accumulation)│
                                       │                   │ Router makes 4-way decision
                                       │                   │ (chat goes directly to synthesizer)
                                       │                   │
                           ┌───────────┴───────────────────┴───────────────┐
                           │                                                │
                   [direct_branch]        [delegate_branch]        [parallel_branch]
                        │                       │                       │
               MCP tool single call        Delegate to expert entirely       Send / fan-out
               (time, weather, memo, etc.)    (coder, researcher, ...)     to multiple tools simultaneously
                        │                       │                       │
                        └───────────────────────┼───────────────────────┘
                                                │
                                        [response_synth]
                                        (synthesize natural response with large LLM)
                                                │
                                                ↓
                                   [supertonic TTS] ──→ [Speaker]
                                   (in detected language)     miniaudio
```

## Two Catalog JSON Files — JARVIS's "What I Can Do"

When JARVIS starts, it reads two files and builds its capability list.
**This means you can add/remove capabilities without recompiling code.**

### `config/mcp_catalog.json` — Tools

A list of function-type tools JARVIS can call directly.
Each entry corresponds to one MCP server (HTTP or stdio).

```json
{
  "tools": [
    {
      "name": "time_weather",
      "transport": "http",
      "url": "http://127.0.0.1:8000",
      "description": "Short, immediate-answer information like current time, weather, exchange rates",
      "enabled": true
    },
    {
      "name": "personal_memo",
      "transport": "stdio",
      "command": ["python3", "examples/demo_mcp_stdio_server.py"],
      "description": "Tony's personal memo storage/retrieval",
      "enabled": true
    }
  ]
}
```

On startup, it calls `get_tools()` on each MCP server → merges tool definitions
and injects them into the router's system prompt as "available tools".

### `config/agent_registry.json` — Experts (A2A)

Sub-agents JARVIS can delegate entire tasks to. Each runs as a separate process/machine
as an A2A endpoint.

```json
{
  "agents": [
    {
      "name": "coder",
      "url": "http://127.0.0.1:8210",
      "expertise": "Code writing, review, debugging",
      "fetch_card_on_start": true
    },
    {
      "name": "researcher",
      "url": "http://127.0.0.1:8211",
      "expertise": "Web search + summarization, academic paper organization",
      "fetch_card_on_start": true
    }
  ]
}
```

On startup, it requests `AgentCard` from each URL → activates only those that respond.
**Key trick**: Any external agent that follows the A2A standard — whether it's a Python
A2A bot someone else made, another NeoGraph instance, etc. — can become JARVIS's
subordinate by just adding its URL to this JSON.

## Router (Intent Classification) — JARVIS's Brain

A single call to a small/fast LLM (e.g., `gpt-4o-mini` or local `llama-3.2-1b`) returns:

```json
{
  "mode": "chat" | "direct" | "delegate" | "parallel",
  "tool_calls": [{"tool": "time_weather.now", "args": {}}],
  "delegate_to": null,
  "skip_synthesis": false
}
```

- `chat` — No tools or delegation. Synthesizer answers directly using its own knowledge + conversation memory.
  Greetings, self-introduction, small talk, "what did I say earlier?" style conversation recall. If the router
  invents tools/agents not in the catalog, the validation stage demotes to this mode.
- `direct` — Single tool call. If the result is simple (`"3:30 PM"`), skip synthesis with `skip_synthesis=true`
  and go straight to TTS. **Fast.**
- `delegate` — Delegate entirely to the A2A endpoint pointed by `delegate_to`.
  After getting the result, synthesize only a one-line summary for voice.
- `parallel` — Multiple `tool_calls`. Execute simultaneously using NeoGraph's `make_parallel_group`,
  reducer combines results for the synthesizer.

### Why Separate Router and Synthesizer

Running everything through one large LLM with ReAct would take 1-3 seconds per turn, killing the JARVIS feel.
- Router: small model, ~200ms, single JSON
- Synthesizer: large model, ~800-1500ms, single natural language response
- If tool provides immediate answer, skip synthesizer → response starts in ~500ms

The quick response timing in movie JARVIS comes from this separation.

## Memory (`Store`)

At the start of each turn, the `memory_lookup` node pulls the last N turns + user preferences
(`tony.prefers.language=ko`, `tony.last_topic=...`) from NeoGraph `Store`.

At the end of each turn, JARVIS pushes the response + Tony's utterance + used tools to Store.
Next turn's router can resolve references like "that thing I mentioned earlier". `JsonFileStore`
persists to file — remembers across restarts. Empty turns (STT failure / noise) are excluded
from commits to prevent memory pollution. `prefs.native_lang` maintains the estimated native language
(language consistency).

## Bidirectional A2A — JARVIS Calls and Is Called

- **Calling**: Delegate to experts via `A2AClient` from `agent_registry.json`.
- **Being called**: JARVIS itself exposes an `A2AServer` (port 8200).
  - External systems can send text messages to JARVIS via `POST /v1/messages`.
  - Mobile apps, other NeoGraph instances, even another JARVIS can call it.
  - Text input skips the microphone/STT stage and goes directly to the router.

**JARVIS-to-JARVIS communication demo**: Home JARVIS (8200) ↔ Office JARVIS (8201).
"Get today's meeting minutes from office JARVIS" → Home JARVIS calls office JARVIS via A2A
→ Response delivered to Tony via voice.

## Background Triggers (Proactive)

A separate async graph runs in the background:
- Timer (check calendar every 5 minutes)
- External events (home sensors, email receipt)
- External A2A calls

When an event occurs, it injects a message into JARVIS's main graph → JARVIS speaks
before Tony asks. ("Sir, meeting in 10 minutes.")

Uses NeoGraph's `27_async_concurrent_runs.cpp` pattern exactly.

## Directory Structure

```
jarvis/
├── README.md                      ← This document
├── CMakeLists.txt                 External dependencies (whisper/onnxruntime/miniaudio) gated
├── config/                        Default config (graph · catalog · registry · persona)
├── config-demo/                   Execution preset (real-tools / mock)
├── config-bench*/                 Benchmark config
├── src/
│   ├── main.cpp                   Entry point (node registration · graph compilation · main loop)
│   ├── audio/                     miniaudio capture (+Silero VAD) · playback, supertonic TTS
│   ├── stt/                       whisper_node (multi-language · language consistency) + moonshine_node (edge)
│   ├── orchestrator/              Router, MCP catalog loader, A2A dispatcher
│   └── memory/                    Store-based conversation memory (JsonFileStore persistence)
├── specialists/                   coder / researcher (separate A2A servers)
├── bench/                         NeoGraph vs LangGraph benchmark (twin · driver · Docker)
├── assets/download.sh             Download whisper/supertonic/moonshine/silero models
├── scripts/
│   ├── run_jarvis.sh              Execution wrapper (LD_LIBRARY_PATH · ROCm · dxg auto)
│   ├── jarvis_repl.py             Korean readline REPL (text/wav input)
│   ├── build_whisper_hip.sh       Build whisper.cpp ROCm/HIP GPU
│   └── demo_mcp_server.py         Demo MCP server (time/weather/calc)
└── docs/architecture.md          Detailed node-by-node graph explanation
```

## Build / Run

```bash
# 1. Download models (whisper-large-v3-turbo ~1.6GB + supertonic + silero VAD)
#    Lightweight: JARVIS_WHISPER=small bash assets/download.sh  (Raspberry Pi / CPU)
bash examples/cookbook/jarvis/assets/download.sh

# 2. Build — onnxruntime, whisper.cpp, miniaudio found on system (or mock if missing)
cmake -B build-jarvis -DNEOGRAPH_BUILD_COOKBOOK_JARVIS=ON
cmake --build build-jarvis --target cookbook_jarvis -j

# 3a. Run — text/wav input (Korean line-edit REPL recommended)
cd examples/cookbook/jarvis
python3 scripts/jarvis_repl.py                 # Automatically loads OPENAI_API_KEY from .env
#   Tony ▸ Hello?                                # Text
#   Tony ▸ wav:/path/to/audio.wav                # Audio file → STT

# 3b. Run — live microphone (miniaudio capture + Silero VAD)
JARVIS_MIC=1 bash scripts/run_jarvis.sh config-demo/real-tools
#   "Online" appears → speak → voice end detection → STT → response → TTS

# (Demo MCP server for tools — separate terminal)
python3 scripts/demo_mcp_server.py 8888        # Time/weather/calc
```

LLM provider selected by `OPENAI_API_KEY` in `.env` (direct OpenAI) or
`OPENAI_BASE_URL`+`JARVIS_ROUTER_MODEL`/`JARVIS_SYNTH_MODEL` (Groq/Cerebras/etc.
OpenAI-compatible). Without it, runs offline with MockProvider (echo).

## Voice Stack Details

### Live Microphone (miniaudio + Silero VAD)
`JARVIS_MIC=1` or config `use_microphone:true`. Capture worker thread runs Silero VAD
on 512-sample window to detect speech start/end (200ms pre-roll, 500ms silence end).
**Backpressure**: Discards capture during inference to block TTS echo, stale utterances,
and start noise. Device failure (WSL2 microphone disconnected, etc.) falls back to stdin automatically.
Tuning: `JARVIS_VAD_THRESHOLD` (default 0.5), observe: `JARVIS_MIC_DEBUG=1`.

### STT — Two Options (swap via config `stt.type`)
- **`whisper_stt`** (default): whisper.cpp. `language:"auto"` detects 99 languages automatically
  → **Answers and TTS in speaker's language**. **Language consistency**: maintains native language
  in store.prefs so short utterances misidentified as foreign don't suddenly switch (requires
  consistent misidentification to switch).
- **`moonshine_stt`**: Moonshine-tiny ONNX (27M, shares ORT with supertonic).
  Edge, low-latency, Korean flavor. Language-specific model, so lang is fixed.

### GPU Acceleration (whisper.cpp ROCm/HIP)
Bundled whisper.cpp is CPU-only — large takes ~32s on CPU (11s clip). AMD GPU
(gfx1201=R9700, ROCm≥7.2) run `bash scripts/build_whisper_hip.sh` for GGML_HIP
build → **~7s (4.5×)**. run_jarvis.sh automatically loads ROCm runtime and WSL dxg.

## Benchmark — NeoGraph vs LangGraph (`bench/`)

Mirrors identical topology (mic→stt→merge→memory→router→4-way→synth/skip→commit→tts)
in LangGraph (Python twin `langgraph_twin.py`), measures in identical constraints
(`--cpus=2 --memory=2g`) container.

```bash
GROQ_API_KEY=... bash bench/run_bench.sh     # mock 200 turns + groq 20 turns × both
```

## Implementation Status

**Fully functional** — Verified live voice single-turn runs on real hardware (real LLM Groq).
Mic→VAD→STT→router→4-way→synth→TTS full chain + memory persistence + A2A self-server.

Known limitations / next version:
- **Barge-in not supported** — Utterances during TTS playback are discarded via backpressure
  (will add cancel token in v2).
- **Streaming STT not applied** — Batch transcription after utterance completion. Moonshine v2
  ergodic encoder chunk-by-chunk streaming is the next candidate.
- **Multi-speaker · long-memory compression** — Single-speaker assumption, 24-turn limit.
- **Background triggers (proactive)** — Designed but not implemented.

## License / External Dependencies

| Library | License | Role |
|---|---|---|
| [supertonic](https://github.com/supertone-inc/supertonic) | MIT | TTS (99M, ONNX, 31 languages) |
| [whisper.cpp](https://github.com/ggerganov/whisper.cpp) | MIT | STT (99 languages auto-detect, CPU/ROCm) |
| [Moonshine](https://github.com/moonshine-ai/moonshine) | MIT | Edge STT option (27M ONNX) |
| [miniaudio](https://github.com/mackron/miniaudio) | MIT-0 / public domain | Microphone capture + speaker playback |
| [Silero VAD](https://github.com/snakers4/silero-vad) | MIT | Speech start/end detection (ONNX) |
| ONNX Runtime | MIT | supertonic·moonshine·VAD inference |
