# JARVIS Graph — Node-by-Node Details

What each node in `config/jarvis_graph.json` does and why it's in that position.
Best read alongside the diagram in README.md.

## One Turn's Lifespan

```
T0  Microphone active, Tony utterance start detected
T1  Utterance end (VAD detects 200ms silence)
T2  STT complete — text + detected language code
T3  Memory lookup complete — last 6 turns + preferences + last topic
T4  Router decision complete — {mode, tool_calls, delegate_to, skip_synthesis}
T5  4-way branch complete — one of self-response(chat) / direct tool / delegation / parallel done
T6  Response synthesis complete (or skip branch bypassed)
T7  Memory commit complete
T8  TTS first chunk playback starts ← point where Tony "starts hearing" the response
T9  TTS last chunk playback complete, microphone reactivated waiting
```

T0→T8 is JARVIS's perceived response time. Target distribution:
- Short answer (direct + skip_synthesis): T0→T8 ≈ 1.0-1.5s
- Normal (direct + synth): T0→T8 ≈ 2.0-3.0s
- Delegation (delegate): T0→T8 ≈ 3.0-8.0s (depends on expert work time)
- Comprehensive (parallel + synth): T0→T8 ≈ 2.5-4.0s

## Node-by-Node Details

### mic_capture (`voice_in`) — Live Microphone Implemented
- Default is stdin mode (text / `wav:/path`). **`use_microphone:true` or env
  `JARVIS_MIC=1`** activates live microphone capture.
- `miniaudio` capture device streams 16kHz mono f32 to callback → mutex buffer
- VAD worker thread runs `Silero VAD`(ONNX) inference on 512-sample (32ms) window → speech prob
- If `vad_threshold`(0.5) exceeded, recording starts (200ms pre-roll to avoid cutoff),
  500ms continuous silence ends utterance → PCM pushed to utterance queue → run() gets voice_in
- Noise <250ms ignored, forced end if `max_utterance_seconds` exceeded
- **Automatic stdin fallback on device initialization failure(WSL2 audio bridge missing, etc.)** —
  no crash. Works live on WSL2 if WSLg/PulseAudio source available.

### stt (`whisper_stt` or `moonshine_stt`)
- Reuses single whisper.cpp model for node lifetime (no reload cost ×)
- `language="auto"` auto-detects from first 30s (or entire utterance)
- Output: `user_text` (single string), `user_lang` (ISO code)
- If recognition confidence too low, empty string — turn skipped at router stage

**GPU acceleration (whisper.cpp ROCm/HIP)**: Bundled whisper.cpp is CPU-only so
whisper-large-v3-turbo takes ~32s on CPU (jfk 11s) — unsuitable for live. AMD GPU
(gfx1201=R9700, ROCm≥7.2) run `bash scripts/build_whisper_hip.sh` for GGML_HIP
build → replace whisper_install → **~7s (4.5×)**. run_jarvis.sh automatically loads ROCm
runtime and WSL dxg bridge (HSA_ENABLE_DXG_DETECTION). With GPU, config can stay large for real-time;
without, switch to whisper-small (CPU ~8s).

**Alternative option `moonshine_stt`** (Moonshine-tiny ONNX): 27M ultra-lightweight, raw 16kHz
waveform input (not mel), seq2seq(encoder + 2-model separated decoder + KV cache). Shares
ONNX Runtime with supertonic TTS. Language-specific flavor model so `user_lang` is config fixed
(tiny-ko = "ko"). Tokenizer decodes directly from SentencePiece BPE in tokenizer.json
(▁→space + ByteFallback + Fuse). Swapping via config stt.type works, and
Python optimum reference verified 55-token character-level parity. int8(~28MB) requires full ORT
build (bundled reduced build excludes ConvInteger) → default fp32(~183MB).

### text_or_voice (`channel_merge`)
- Selects living path between voice_in path(STT passed) and text_in path(external A2A)
- Empty turn if both empty — graph passes through one cycle
- External A2A call must also include `user_lang` (assumes "en" if missing)

### memory_lookup (`memory_lookup`)
- Reads from NeoGraph `Store` `jarvis.tony` namespace
- Combines last N turns + prefs + last_topic into single `memory_context` push
- Cost ×, always runs at graph start

### router (`intent_classifier`)
- Small LLM (gpt-4o-mini, ~200-400ms)
- System prompt = persona.txt [router] + MCP catalog text + agent registry text
- Output JSON validation: parse failure → fallback (mode=chat). Tool/agent names
  verified against catalog·registry; if not real, demoted to chat (prevents LLM-invented
  `delegate_to:"null"` from flowing downstream).
- Chat mode goes directly to response_synth without tools/delegation — for greetings,
  self-introduction, conversation recall

### direct_branch (`tool_dispatch`)
- Dispatches `route_decision.tool_calls[0]` once
- Appends result to `tool_results` channel
- If skip_synthesis=true, bypasses next node to TTS directly

### parallel_branch (`parallel_tool_fanout`)
- Executes all `route_decision.tool_calls` simultaneously (`make_parallel_group`)
- Upper bound via `max_concurrent` (default 4)
- Appends all results to `tool_results` in order → reducer uses for synthesizer

### delegate_branch (`a2a_delegate`)
- Sends user_text to A2A endpoint pointed by `route_decision.delegate_to`
- Error response if `timeout_seconds` exceeded (JARVIS says "Expert not responding" via voice)
- Extracts `[SUMMARY]` line first from response → saves to `delegated_reply`

### response_synth (`llm_call`)
- Large LLM (gpt-4o, ~800-1500ms)
- System prompt = persona.txt [synth] (+ language instruction + session boundary comment)
- Conversation history(memory_context.recent_turns) is **passed as messages array user/assistant
  role turns** — previously, inline JSON in user message caused model to treat past answers
  as content verbatim (memory parrot).
- Current turn user message = user_text + tool_results / delegated_reply attached
- Verbatim guard: If output exactly matches past answer after trim, regenerate once
- Output = `final_text` (string for TTS to read)
- Bypassed in skip_synthesis=true path (synth_skip occupies this spot)

### synth_skip (`passthrough`)
- Copies last item in tool_results (usually tool's raw response) directly as final_text
- Example: Time tool returns "3:30 PM" → voice directly
- JARVIS response speed secret weapon — saves entire ~1s large LLM call

### memory_commit (`memory_commit`)
- Appends this turn's user_text + final_text + used tool names to Store turns
- Next turn memory_lookup pulls these for router context
- Can process asynchronously (parallel with TTS) — currently serial

### tts (`supertonic_tts`)
- supertonic inference with final_text + user_lang → 44.1kHz PCM
- Starts miniaudio speaker playback → first chunk ~100-300ms later
- Cancels with cancel token if voice_in activation detected during playback (barge-in)
  - Initial skeleton does not support barge-in; to be added in v2

## Outside Graph — Background Triggers / A2A Server

JARVIS main graph is simple single utterance single response cycle, but main.cpp
starts two additional things that complete JARVIS feel:

### Background Trigger Graph
- Separate `GraphEngine` (or just std::thread)
- Monitors timer / external events
- Injects message to JARVIS main's `text_in` channel when event occurs
- JARVIS speaks before Tony asks ("Sir, meeting in 10 minutes.")

### A2A Server (Exposing JARVIS externally)
- Based on `self` section in `agent_registry.json`
- Exposes same engine wrapped with `GraphAgentAdapter` (example 38 pattern)
- External text input skips STT stage and goes `text_in` → router directly
- Response can be sent back as text, or simultaneously played via local TTS

## Known Limitations / Next Version

- **Barge-in not supported** — Microphone input ignored during TTS playback. Add cancel token in v2.
- **Multi-speaker not supported** — Assumes one person. Speaker separation needs separate node (e.g., pyannote).
- **Long-memory compression** — Turns grow infinitely as conversation lengthens. Need #56 history_compaction pattern.
- **Catalog hot-reload** — JSON change detection is manual (SIGHUP, etc.). Automatic reload via inotify in v2.
