# dr_compare — NeoGraph vs LangGraph deep-research bench

Two implementations of the same deep-research workflow (router → plan →
fan-out 5 researchers via Send → synthesize), one per engine. Same
prompts, same model, same Crawl4AI search, same Postgres checkpoint
backend (or in-memory). Differences are isolated to the engines + their
HTTP transport.

## Files

- `dr_neograph.py` — NeoGraph runner. Env-driven knobs (see below).
- `dr_langgraph.py` — LangGraph equivalent. Sync `def` nodes + sync
  `app.invoke()` for parity with `dr_neograph.py`.
- `bench.py` — real-LLM harness. Warmup + alternating measure +
  percentiles.
- `bench_mock.py` — engine-throughput harness with mocked LLM. Modules
  pre-loaded once, iters reuse the compiled engines.
- `sweep.sh` — runs `bench_mock.py` across `(FANOUT, LLM_MOCK_MS)`
  variants.
- `_run_single.py` — one-shot runner. Used for wire/strace probes.

## Env knobs

| Var | Default | Purpose |
|---|---|---|
| `LLM_MOCK_MS` | -1 (real) | Replace LLM with `time.sleep(MS)`. >=0 enables mock. |
| `MOCK_SEARCH` | "0" | Skip Crawl4AI; return canned evidence. |
| `FANOUT` | 5 | Number of researcher Sends. |
| `USE_INMEMORY_CP` | "0" | Use in-memory checkpoint (ignore PG_DSN). |
| `NG_TRANSPORT` | `ws-responses` | NG only: `ws-responses` (WebSocket Responses) or `http-chat` (`/v1/chat/completions`). |
| `NG_WORKER_COUNT` | "4" | NG only: thread pool for Send fan-out parallelism. |

## Findings (2026-04-26)

1. **Pure engine throughput (mocked LLM, FANOUT=5)** — NeoGraph 1.0ms
   median vs LangGraph 5.9ms. NG is **5.9× faster** at zero LLM cost.
2. **Real LLM bench** — first round had NG p50 23.90s (sd 5.90), LG
   21.95s (sd 1.23). LG looked ~10% faster.
3. **Wire diagnosis** — pcap on WSL2 lied (BPF drops most packets via
   HyperV vswitch). `strace -e trace=connect` showed NG doing 21
   connect() syscalls per 7-LLM-call run — fresh TCP+TLS every time.
4. **Root cause** — `SchemaProvider::complete_async` used the free
   `async::async_post()` (closes socket per call) instead of the
   already-existing `async::ConnPool` (HTTP/1.1 keep-alive).
   `run_sync`'s per-call throw-away io_context made the obvious "pool
   inside the provider" wiring unsafe — but a long-lived background
   io_context owned by the provider works.
5. **Fix (commit 6da4810 / bc2ab4f)** — SchemaProvider + OpenAIProvider
   now hold their own io_context + worker thread + ConnPool. After
   fix, NG p90 dropped 35.34s → 25.28s (-10s), sd 5.90→1.28
   (4.6× more stable). Median ~unchanged because parallel Send fan-out
   still needs N TCP conns on HTTP/1.1.
6. **Remaining gap** — LG's httpx supports HTTP/2, multiplexing N
   parallel streams over a single TCP. Closing this gap requires NG
   to add HTTP/2 client support (httplib is HTTP/1.1 only).
7. **Worker pool ceiling** — `set_worker_count(N)` caps Python-node
   fan-out concurrency. Bench code's `set_worker_count(4)` was a real
   ceiling; `NG_WORKER_COUNT=50` flips NG sync ahead of LG asyncio
   (307ms vs 711ms at FANOUT=50, LLM=100ms).

See `feedback_schema_provider_no_pool.md` and
`feedback_pybind_worker_ceiling.md` in claude memory for the full
narrative.

## Reproducing

Real LLM bench:
```sh
set -a && source ../../.env && set +a
export NEOGRAPH_PG_DSN="postgresql://postgres:test@localhost:5433/neograph"
export CRAWL4AI_URL="http://localhost:11235"
export NG_TRANSPORT=http-chat   # apples-to-apples vs LG (both HTTP)
python bench.py --warmup 2 --iters 5
```

Engine-throughput sweep:
```sh
./sweep.sh   # writes /tmp/sweep.log
```

Wire diagnosis (when in doubt about pcap, strace is ground truth):
```sh
strace -f -e trace=connect -o /tmp/ng.log \
    python _run_single.py neograph
grep "connect(" /tmp/ng.log | grep -oE 'sin_addr=inet_addr\("[^"]+"\)' \
    | sort | uniq -c
```
