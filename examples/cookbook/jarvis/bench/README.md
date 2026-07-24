# JARVIS Orchestration Benchmark — NeoGraph vs LangGraph

Mirrors identical topology (mic→stt→merge→memory→router→4-way→synth/skip→commit→tts)
in NeoGraph (C++ mock build) and LangGraph (Python twin `langgraph_twin.py`),
measures in identical constraints (`--cpus=2 --memory=2g`) container.

```bash
GROQ_API_KEY=... bash bench/run_bench.sh     # mock 200 turns + groq 20 turns × both
```

## Measured Results (2026-07-05, WSL2, --cpus=2 --memory=2g)

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
- 90× startup · 9× RSS are **fixed gaps** unrelated to inference speed — immediately
  relevant for edge always-on · cold-start · multi-tenant (100 JARVIS = <1GB).

## E2E Round — Including Real MCP Tool Round-Trip (2026-07-05)

```bash
GROQ_API_KEY=... bash bench/run_bench_e2e.sh
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
GROQ_API_KEY=... bash bench/run_bench_proxy.sh
```

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
  Time-window mapping has WSL2 wall-clock step (measured -0.8s reversal during run) as
  fallback-only. Driver timestamps also derived from monotonic anchor.
- Trap note: Groq(Cloudflare) blocks `Python-urllib` UA with 403 — easy to mistake
  for proxy issue. Real smoke tests use curl/httpx-family UA.

## Streaming TTFT Round (2026-07-05)

Modern LLM services all stream, so benchmark matches: Both synth calls changed to streaming
(C++ `invoke(p, on_chunk)`, LangGraph `SYNTH_LLM.stream()`),
driver measures **turn-send → first synth token** time with `[jarvis:ttft]` marker.
nginx passes SSE through with `proxy_buffering off` so `$upstream_header_time` is
the real first byte. Separate logs per round (mv + `nginx -s reopen`) to eliminate
round-splitting guesswork.

| | Perceived TTFT p50 | Completion time p50 | Avg/turn upstream |
|---|---|---|---|
| NeoGraph | **631ms** | 744ms | 726ms |
| LangGraph | **629ms** | 723ms | 753ms |

- **Perceived TTFT effectively tied (delta −2ms).** NeoGraph TTFT appeared slower
  earlier (800 vs 603) was pure provider dispersion — this time Groq gave both
  fair window (upstream 726 vs 753) eliminating gap. Confirmed "NG round only bad luck"
  suspicion with reproduction.
- **Completion time residual (pure framework) reproduced**: NeoGraph 4.1ms vs LangGraph
  14.6ms (matches previous proxy round 3.5 vs 14.7). Framework's own overhead
  conclusion solid.
- **TTFT-residual is 0 within ±tens ms noise** (negative even appears). Compared to
  perceived TTFT 625ms vs upstream sum 673ms, resolution (±50ms) of subtracting two
  independent clocks (client monotonic vs nginx wall-clock) is larger than framework
  contribution (ms). I.e., **framework difference is below observation limit in TTFT path**
  — signal emerges above noise only in total residual/mock.
- **Streaming benefit**: Perceived TTFT (631) ≪ completion time (744) — user starts hearing
  answer in 0.6s. Confirmed perceived speed improvement over non-streaming which waits
  for completion.

Summary: Framework pure performance favors NeoGraph (total residual·mock, reproducible),
but **perceived TTFT is tied in streaming and provider dispersion dominates**. Edge/multi-tenant
(90× startup · 9× RSS) remains NeoGraph's real battlefield.

## Fairness Conditions

- Prompt (persona.txt shared) · decision validation (chat downgrade) · memory format (JsonFileStore) ·
  verbatim guard · stdout marker identical. Only framework and language differ.
- LangGraph side uses idiomatic stack (langgraph + langchain-openai).
- Measurement is container-internal `driver.py` (stdin injection → `[jarvis:tts]` marker round-trip).

## Files

- `langgraph_twin.py` — LangGraph twin (identical topology·protocol, real tool call via
  official mcp SDK persistent session when MCP_URL set)
- `driver.py` / `analyze.py` — Measurement · comparison table
- `Dockerfile.neograph` / `Dockerfile.langgraph` / `Dockerfile.mcp` — Benchmark images
- `run_bench.sh`(core) / `run_bench_e2e.sh`(real tool E2E) — Runners
- `turns_mock.txt`(200) / `turns_groq.txt`(20) / `turns_e2e.txt`(24) — Turn sets
- `../config-bench/` — Empty catalog (chat path fixed) /
  `../config-bench-e2e/` — Shared MCP server catalog
