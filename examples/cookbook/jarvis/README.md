# JARVIS — Voice-Driven Meta-Orchestrator

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

## Typed provider migration — scoped runtime status

The C++ router, synthesizer and specialist fixtures use typed `ProviderRequest`, `sp::Message` and `sp::Event`; provider results are full immutable `sp::Outcome` values (`sp::runtime::Result`), not legacy response strings. `src/provider_support.h` implements local Jarvis/coder/researcher mock profiles: deterministic router JSON, user-text echo, and an explicitly synthetic researcher reply. These fixtures require neither credentials nor a network provider, but do not supply real research or expert reasoning. Existing configuration/profile paths below are not repaired or created by this documentation update.

Local voice is optional and needs the selected whisper/Moonshine models, ONNX Runtime/Supertonic assets, miniaudio and usable microphone/speaker devices; text/mock operation is not proof of working voice. Cloud-free applies only to local/mock operation. Live requests require an authorized `OPENROUTER_API_KEY`, network access and provider capacity, and send prompts, conversation memory and attached tool/delegation results to OpenRouter. The live model is pinned; the native request sets ZDR, not a residency guarantee. Keep keys out of logs and version control. Provider token usage is nullable accounting, not a monetary bill: cost requires current endpoint/model pricing and billable usage.

Custom provider-calling nodes use the existing runtime-interposition/broker boundary and shared `record_usage` sink. They retain the real owned outcome before extracting response text, propagate cancellation/deadlines and both local and host observers, and preserve drained outcomes when an observer throws. The synthesizer's distinct regeneration call uses its own stable call ordinal. Bounded calls require admitted model-limit facts; missing facts fail before provider dispatch rather than inventing a token estimate. Default HTTPS ports are omitted from the admitted origin so OpenRouter routing matches the policy's canonical origin.

`[jarvis:ttft]` is emitted on the first nonempty `sp::PartDelta` with `PartKind::Text` and `DeltaChannel::Content`, not on usage, reasoning, headers or other events. It measures first synthesis text, not first audible TTS playback. Python REPL drivers remain protocol clients; pybind benchmarks use the migrated typed bindings and need separate execution evidence. Current runtime evidence covers the actual CLI greeting, a persisted synthetic memory turn and graceful EOF. It does not qualify microphone capture, ASR, TTS, pybind benchmarks or vendor inference. Timings and voice/live execution statements retained below remain historical, not current cutover qualification.

The CLI evidence above was recorded with interface 3 and has not been rerun.
[Current interface-4 local evidence](../../README.md#typed-c-cutover-status) covers
SDK controls and A2A 0.3/1.0 peers separately; it does not qualify Jarvis voice,
benchmarks or a new full CLI session.

> Local/mock operation needs no cloud provider; optional voice needs local assets and devices.
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

On startup, JARVIS fetches each configured AgentCard. Calls require a compatible
JSON-RPC 0.x/1.0 interface; responding discovery alone does not establish compatibility.
The client selects the card dialect without redirecting the configured endpoint.
Card-selected calls do not fallback to another dialect, and delivered SSE events
cannot be replayed. External Python agents and NeoGraph instances can use this
contract when their cards and wire behavior agree.

## Router (Intent Classification) — JARVIS's Brain

A single call to the pinned DeepSeek model (`~deepseek/deepseek-v4-flash-latest`)
returns:

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

This JSON file stores speech/conversation projections, not authenticated native
provider replay custody.

## Bidirectional A2A — JARVIS Calls and Is Called

- **Calling**: Delegate to experts via `A2AClient` from `agent_registry.json`.
- **Being called**: JARVIS itself exposes an `A2AServer` (port 8200).
  - External systems send JSON-RPC to `POST /`: `message/send` for 0.x or `SendMessage` with `A2A-Version: 1.0` for 1.0; the header selects the response dialect.
  - Mobile apps, other NeoGraph instances, even another JARVIS can call it.
  - Text input skips the microphone/STT stage and goes directly to the router.

**JARVIS-to-JARVIS communication demo**: Home JARVIS (8200) ↔ Office JARVIS (8201).
"Get today's meeting minutes from office JARVIS" → Home JARVIS calls office JARVIS via A2A
→ Response delivered to Tony via voice.

## Background Triggers (Proactive)

Background timer/event triggers are a design, not an implemented component.
A host could use the `27_async_concurrent_runs.cpp` pattern to enqueue text
from calendar or sensor events. The A2A server is implemented separately;
its presence does not qualify proactive triggering.

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

# 2. Build — install SchemaProvider and optional voice dependencies first
export SCHEMAPROVIDER_PREFIX="/absolute/path/to/installed/schemaprovider"
cmake -S . -B build-jarvis -DNEOGRAPH_BUILD_COOKBOOK_JARVIS=ON \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX"
cmake --build build-jarvis --target cookbook_jarvis -j

# 3a. Run — text/wav input (Korean line-edit REPL recommended)
cd examples/cookbook/jarvis
python3 scripts/jarvis_repl.py                 # Automatically loads OPENROUTER_API_KEY from .env
#   Tony ▸ Hello?                                # Text
#   Tony ▸ wav:/path/to/audio.wav                # Audio file → STT

# 3b. Run — live microphone (miniaudio capture + Silero VAD)
JARVIS_MIC=1 bash scripts/run_jarvis.sh config-demo/real-tools
#   "Online" appears → speak → voice end detection → STT → response → TTS

# (Demo MCP server for tools — separate terminal)
python3 scripts/demo_mcp_server.py 8888        # Time/weather/calc
```

The live provider is fixed to OpenRouter with the pinned DeepSeek model and
`OPENROUTER_API_KEY` in `.env`. Without the key, it runs offline with
MockProvider (echo).

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
OPENROUTER_API_KEY=... bash bench/run_bench.sh     # mock 200 turns + OpenRouter 20 turns × both
```

## Implementation Status

**Historical voice evidence** — Earlier live single-turn runs on real hardware
(OpenRouter DeepSeek). Mic→VAD→STT→router→4-way→synth→TTS full chain +

Known limitations / next version:
- **Barge-in not supported** — Utterances during TTS playback are discarded via backpressure
  (will add cancel token in v2).
- **Streaming STT not applied** — Batch transcription after utterance completion. Moonshine v2
  ergodic encoder chunk-by-chunk streaming is the next candidate.
- **Multi-speaker · long-memory compression** — Single-speaker assumption. Memory lookup defaults to six recent turns; commits retain the latest 24 turns without summarizing older history.
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
