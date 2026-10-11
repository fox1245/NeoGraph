# JARVIS Orchestration Benchmark — NeoGraph vs LangGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

## Typed provider migration — source status

The C++ router, synthesizer and specialist fixtures use typed `ProviderRequest`, `sp::Message` and `sp::Event`; provider results are full immutable `sp::Outcome` values (`sp::runtime::Result`), not legacy response strings. `src/provider_support.h` implements local Jarvis/coder/researcher mock profiles: deterministic router JSON, user-text echo, and an explicitly synthetic researcher reply. These fixtures require neither credentials nor a network provider, but do not supply real research or expert reasoning. Existing configuration/profile paths below are not repaired or created by this documentation update.

Local voice is optional and needs the selected whisper/Moonshine models, ONNX Runtime/Supertonic assets, miniaudio and usable microphone/speaker devices; text/mock operation is not proof of working voice. Cloud-free applies only to local/mock operation. Live requests require an authorized `OPENROUTER_API_KEY`, network access and provider capacity, and send prompts, conversation memory and attached tool/delegation results to OpenRouter. The live model is pinned; the native request sets ZDR, not a residency guarantee. Keep keys out of logs and version control. Provider token usage is nullable accounting, not a monetary bill: cost requires current endpoint/model pricing and billable usage.

`[jarvis:ttft]` is emitted on the first nonempty `sp::PartDelta` with `PartKind::Text` and `DeltaChannel::Content`, not on usage, reasoning, headers or other events. It measures first synthesis text, not first audible TTS playback. Python REPL drivers remain protocol clients; pybind benchmarks use the migrated typed bindings and need separate execution evidence. The current [Jarvis CLI runtime evidence](../README.md) covers a greeting, a persisted synthetic memory turn and graceful EOF, not these benchmark rounds. All benchmark timings and execution statements below remain historical; microphone, ASR, TTS, pybind benchmarks and vendor inference are not qualified by that CLI run.

Mirrors identical topology (mic→stt→merge→memory→router→4-way→synth/skip→commit→tts)
in NeoGraph (C++ mock build) and LangGraph (Python twin `langgraph_twin.py`),
measures in identical constraints (`--cpus=2 --memory=2g`) container.

```bash
OPENROUTER_API_KEY=... bash bench/run_bench.sh     # mock 200 turns + OpenRouter 20 turns × both
```

For finite native-only diagnostics, run `bench/driver.py --max-turns 3` against
the actual tracked `turns_openrouter.txt`; keep the output and a fresh memory
file in an owner-private directory. The driver rejects empty inputs/TTS payloads
and failed/incomplete runs. `NG_EXAMPLE_MAX_TOKENS` fixes the C++ call's output cap
including reasoning, and `NG_EXAMPLE_EMPTY_REASKS=0` disables empty-cap re-asks.
An explicit cap is never increased; thinking and original broker/budget identities
remain intact. With empty `config-bench` catalogs, three turns require at most nine
model calls (including optional verbatim regeneration), not a guarantee of charge
or invoice. See [the Jarvis controls](../README.md) and [finite example commands](../../../README.md).
Run protocol/input regressions from the repository root with
`python -m unittest discover -s examples/cookbook/jarvis/bench -p test_driver.py`.
This reduced native diagnostic is not the historical paired/container/proxy cohort.

## Historical Results (2026-07-05, pre-OpenRouter migration; Groq)

| Metric | NeoGraph | LangGraph | Delta |
|---|---|---|---|
| Pure graph overhead/turn (mock 0ms LLM, 200 turns) | **0.38ms** | 3.07ms | +2.7ms (8.1×) |
| Groq real inference/turn (8b router+70b synth, 20 turns) | 684ms | 706ms | +22ms (~3%) |
| Groq p99 | 775ms | 870ms | +95ms (n=20, noise margin) |
| Cold start | 7.9ms | 716ms | ~90× |
| RSS (mock) | 7.5MB | 68MB | ~9× |

Interpretation:
- Graph engine itself is cheap compared to LLM on both sides (0.4ms vs 3ms). Groq delta +22ms of
  ~19ms is HTTP client stack difference (langchain-openai httpx+pydantic vs asio).
- Turn-to-turn gap is **growth-type** — gets larger as inference gets faster — 10%+ for
  200ms-turn (Cerebras-level / single-call path), 20-30% for local small models (~50ms/call).
- The historical startup/RSS ratios were about 90×/9× for this container setup.
  They do not establish memory capacity for 100 production JARVIS instances.

## E2E Round — Including Real MCP Tool Round-Trip (2026-07-05)

```bash
OPENROUTER_API_KEY=... bash bench/run_bench_e2e.sh
```

Shared demo MCP server container (time/calc/weather) + 24-turn mixed set (direct tool call ·
parallel fan-out · chat · memory recall), ABBA order interleaving for 2 rounds each:

| Round (execution order) | mean | p50 | max | Notes |
|---|---|---|---|---|
| neograph r1 | 810ms | 791 | 1052 | |
| langgraph r1 | 673ms | 667 | 934 | |
| langgraph r2 | 1442ms | 1025 | 3830 | Last 7 turns 2.4~3.8s — Groq throttle window |
| neograph r2 | 689ms | 665 | 983 | Stable despite running right after LG r2 |

**Conclusion: Under these conditions (Korea→Groq WAN, ~700ms/turn), provider-side
dispersion (±130~770ms between rounds) completely swallows the framework delta
(mock measured ~3ms + HTTP stack ~19ms).** Switching order flipped the winner —
e2e turn latency cannot determine framework superiority, only controlled mock
rounds measure fixed overhead and startup/memory. E2E verified: both harnesses
work correctly with real tools (routing mode match 21/24, direct/parallel real
round-trip), startup 74ms vs 1944~2483ms, RSS 14MB vs 122MB confirmed.

Implication: Framework difference becomes meaningful only with **low dispersion +
low absolute latency** (local inference, same-datacenter inference) — not just
"fast inference". Cloud inference across WAN makes network dominant regardless
of framework.

## Boundary Measurement Round — Eliminating Provider Dispersion (2026-07-05)

```bash
OPENROUTER_API_KEY=... bash bench/run_bench_proxy.sh
```

Jarvis sends the API key only over TLS, so the runner terminates TLS in the nginx
proxy (`nginx-openrouter.conf`, port 8443) and generates a throw-away CA and server
certificate for each run (requires `openssl` on the host; the CA and key live in a
temporary directory removed on exit). Both clients call
`https://jarvis-openrouter-proxy:8443/openrouter/v1` and use that CA bundle: Jarvis
through `OPENROUTER_CA_FILE`, the LangGraph twin through `SSL_CERT_FILE`. The proxy
logs `$msec $request_time $upstream_connect_time $upstream_header_time
$upstream_response_time $status`, the positional fields `analyze_proxy.py` and
`analyze_ttft.py` parse. The driver exits nonzero if turns are incomplete or the
child process fails; proxy log-rotation failures also stop the runner.

Solve E2E's "dispersion swallows delta" problem with proxy boundary measurement:
Place nginx in front of Groq to **log per-call upstream (WAN+Groq) time** and
compare only the residual (graph + HTTP client serialization + local MCP + pipe)
after subtraction from turn round-trip. Not statistical workaround (increase
ABBA/retry count) but measuring and subtracting noise source itself — results
don't wobble even if rounds hit different Groq windows.

| | Avg/turn upstream | **Residual p50** | Residual p90 | Residual min~max |
|---|---|---|---|---|
| NeoGraph | 1613ms | **3.5ms** | 19.1ms | 1.9~80.5 |
| LangGraph | 1417ms | **14.7ms** | 25.1ms | 10.8~33.3 |

- Raw wall-clock shows "LG is 189ms faster" this time (Groq gave NG round worse
  window — upstream avg +196ms). Residual shows **NG is p50 −11.1ms** — clear
  demonstration that method restores signal regardless of noise direction.
- Residual p50 matches mock round prediction (graph 0.4 vs 3.1ms + HTTP stack diff) —
  payload cross-validation success.
- Call↔turn mapping is **order-based** (verify call count = 2×turn count, log order = turn order).
  Time-window mapping has historical wall-clock step (measured -0.8s reversal during run) as
  fallback-only. Driver timestamps also derived from monotonic anchor.
- Trap note: Groq(Cloudflare) blocks `Python-urllib` UA with 403 — easy to mistake
  for proxy issue. Real smoke tests use curl/httpx-family UA.

## Streaming TTFT Round (2026-07-05)

This historical round switched both synthesis calls to streaming
(the historical C++ streaming provider path and LangGraph `SYNTH_LLM.stream()`);
the migrated C++ path now uses `ProviderMode::Stream` / `sp::Event`.
driver measures **turn-send → first synth token** time with `[jarvis:ttft]` marker.
nginx passes SSE through with `proxy_buffering off` so `$upstream_header_time` is
the real first byte. Separate logs per round (mv + `nginx -s reopen`) to eliminate
round-splitting guesswork.

| | Perceived TTFT p50 | Completion time p50 | Avg/turn upstream |
|---|---|---|---|
| NeoGraph | **631ms** | 744ms | 726ms |
| LangGraph | **629ms** | 723ms | 753ms |

- **Perceived TTFT effectively tied (delta −2ms).** NeoGraph's apparently slower TTFT
  earlier (800 vs 603) was pure provider dispersion — this time Groq gave both
  fair window (upstream 726 vs 753) eliminating gap. Confirmed "NG round only bad luck"
  suspicion with reproduction.
- **Completion-time residual in that historical round**: NeoGraph 4.1ms vs LangGraph
  14.6ms (matches previous proxy round 3.5 vs 14.7). The framework overhead
  residual includes client serialization, local MCP and pipe overhead; it is not
  a direct measurement of graph computation alone.
- **TTFT-residual is 0 within ±tens ms noise** (negative even appears). Compared to
  perceived TTFT 625ms vs upstream sum 673ms, resolution (±50ms) of subtracting two
  independent clocks (client monotonic vs nginx wall-clock) is larger than framework
  contribution (ms). I.e., **framework difference is below observation limit in TTFT path**
  — signal emerges above noise only in total residual/mock.
- **Streaming benefit**: First synthesis text (631ms) precedes completion (744ms).
  This marker does not measure first audible playback. The historical phrase “user starts hearing”
  at 0.6s described the text marker, not an audio measurement. Non-streaming waits
  for completion.

The historical streaming text-marker TTFT was tied. These measurements do not
qualify the current SDK transport, audio latency or production tenant capacity.

## Fairness Conditions

- Prompt (persona.txt shared) · decision validation (chat downgrade) · memory format (JsonFileStore) ·
  verbatim guard · stdout marker identical. Only framework and language differ.
- LangGraph side uses idiomatic stack (langgraph + langchain-openai).
- Measurement is container-internal `driver.py` (stdin injection → `[jarvis:tts]` marker round-trip).
- Initial output caps: both sides use 300 (router) / 220 (synthesis) nominal reply
  tokens plus 1024 tokens of reasoning headroom, with low reasoning effort.
  Only the C++ side re-asks a completed empty `MaxTokens` reply once at twice the
  cap. Extra provider calls invalidate the analyzers' two-calls-per-turn residual
  assumption; their mismatch warning must not be treated as comparable timing.

## Files

- `langgraph_twin.py` — LangGraph twin (identical topology·protocol, real tool call via
  official mcp SDK persistent session when MCP_URL set)
- `driver.py` / `analyze.py` — Measurement · comparison table
- `Dockerfile.neograph` / `Dockerfile.langgraph` / `Dockerfile.mcp` / `Dockerfile.proxy` — Benchmark images
- `run_bench.sh`(core) / `run_bench_e2e.sh`(real tool E2E) / `run_bench_proxy.sh`(TLS proxy boundary
  measurement) — Runners
- `nginx-openrouter.conf` — Proxy configuration; `analyze_proxy.py` / `analyze_ttft.py` — its log analyzers
- `turns_mock.txt`(200) / `turns_openrouter.txt`(20) / `turns_e2e.txt`(24) — Turn sets;
  `turns_openrouter.txt` is a chat-only selection of lines taken verbatim from the other two
- `../config-bench/` — Empty catalog (chat path fixed) /
  `../config-bench-e2e/` — Shared MCP server catalog
