# Concurrent-load benchmark — NeoGraph vs. Python graph frameworks

Under a **burst** load — N requests submitted simultaneously, then the
test waits for all to complete — how do these engines scale, and at
what point does each approach stop being viable?

This bench answers that in a reproducible Docker sandbox with CPU and
memory limits matching an SBC-class target, across six frameworks.

## Setup

- **Workload**: 3-node sequential counter chain (`a → b → c`), each
  node increments a single state channel. No I/O, no sleep, no LLM.
- **Burst pattern**: N tasks submitted at t=0; the runner waits for all
  to finish; per-request latency is captured in-process.
- **Sandbox**: Docker with `--cpus` and `--memory` (+ `--memory-swap`
  matched so OOM fires instead of swapping). Matrix:
  - Profile **1 CPU / 512 MB** — the "tight SBC" target (primary)
  - Profile **2 CPU / 1 GB** — the "comfortable SBC" target
- **Concurrencies**: N ∈ {10, 100, 1000, 10000}
- **Engines tested**:
  - `neograph` (3.0) — caller-side `asio::thread_pool` with `hardware_concurrency()` workers dispatching `engine->run()`. The engine itself drives the super-step loop on a single-threaded `run_sync` by default; each invocation uses its own io_context, so scheduling is bounded by the caller pool, not the engine.
  - `langgraph-asyncio` / `langgraph-mp` — LangGraph 1.1.9 under `asyncio.gather` / `multiprocessing.Pool`.
  - `haystack-asyncio` / `haystack-mp` — Haystack 2.27.0. Pipeline.run() is sync; asyncio mode wraps with `asyncio.to_thread`.
  - `pydantic-asyncio` / `pydantic-mp` — pydantic-graph 1.84.1, async-native.
  - `llamaindex-asyncio` / `llamaindex-mp` — LlamaIndex Workflow 0.14.20, one fresh Workflow per run (per-run event bus).
  - `autogen-asyncio` / `autogen-mp` — AutoGen GraphFlow 0.7.5, one fresh flow per run (flow state isn't concurrent-safe).

## Results — 1 CPU / 512 MB profile (asyncio mode)

![Throughput — requests per second](../../docs/images/bench-concurrent-throughput.png)

![Tail latency — P99 per request](../../docs/images/bench-concurrent-latency.png)

![Peak resident memory](../../docs/images/bench-concurrent-rss.png)

The charts track asyncio-mode results for all six engines. The mp
(multiprocessing) rows are in the raw-numbers table below — mp
bypasses the GIL across N worker processes but saturates at pool
size, and the pattern is the same across every Python framework.

### Raw numbers (1 CPU / 512 MB, NeoGraph 2026-04-22 on 3.0, Python field 2026-04-19)

Full matrix including the 2 CPU / 1 GB profile is in
[`results.jsonl`](results.jsonl). N=10,000 is the row that tells the
story most sharply:

| N | Engine + mode | Wall | P50 | P99 | Peak RSS | OK / Err |
|---|---------------|------|-----|-----|----------|---------|
| **10,000** | **NeoGraph 3.0** | **52 ms** | **4 µs** | **7 µs** | **5.5 MB** | 10000 / 0 |
| 10,000 | LangGraph asyncio | 23.4 s | 20.2 s | **23.0 s** | 416.2 MB | 10000 / 0 |
| 10,000 | LangGraph mp-pool-7 | 8.0 s | 737 µs | 88.4 ms | 60.3 MB | 10000 / 0 |
| 10,000 | Haystack asyncio | 3.1 s | 1.7 s | 2.9 s | 130.7 MB | 10000 / 0 |
| 10,000 | Haystack mp-pool-7 | 2.9 s | 167 µs | 84.7 ms | 68.1 MB | 10000 / 0 |
| 10,000 | pydantic-graph asyncio | 886 ms | 71 µs | **158 µs** | 42.6 MB | 10000 / 0 |
| 10,000 | pydantic-graph mp-pool-7 | 2.8 s | 253 µs | 83.8 ms | 36.7 MB | 10000 / 0 |
| 10,000 | **LlamaIndex asyncio** | **OOM killed** | — | — | — | — |
| 10,000 | LlamaIndex mp-pool-7 | 6.6 s | — | — | 102.5 MB | **0 / 10000** |
| 10,000 | **AutoGen asyncio** | **OOM killed** | — | — | — | — |
| 10,000 | AutoGen mp-pool-7 | 46.8 s | 4.6 ms | 97.1 ms | 49.1 MB | 10000 / 0 |

Two engines exit the 512 MB sandbox non-gracefully at N=10,000:

* **LlamaIndex asyncio** — OOM killed. Each in-flight workflow holds a
  per-run event bus + channel runtime; 10k of them overshoots the
  cgroup before wall-clock completes.
* **AutoGen asyncio** — OOM killed. 10k concurrent GraphFlow instances
  with their participant state trip the same ceiling.
* **LlamaIndex mp-pool** — all 10k worker invocations failed. Workflow
  instances are not pickle-safe across worker-process forks; fails
  regardless of N.

The full raw matrix for both profiles is
[`results.jsonl`](results.jsonl) (one JSON line per cell).

## Interpretation

### Throughput: NeoGraph scales, every Python asyncio runtime plateaus

NeoGraph's green curve stays in the 22-25K req/s range across the
sweep while every Python asyncio curve degrades. The caller-side
`asio::thread_pool` dispatches `engine->run()` calls across all
available cores; each call then drives its own single-threaded
super-step loop through `run_sync` — the cgroup's CPU quota bounds
wall time but not thread count, so short tasks interleave cleanly
across the caller pool.

**Every Python asyncio curve plateaus or degrades.** The root cause is
the same across LangGraph, Haystack, pydantic-graph, LlamaIndex, and
AutoGen: one event loop in one process, and the GIL serializes the
CPU work every coroutine has to do. N coroutines → serialized
execution → throughput that doesn't scale with N.

The mp-pool mode for each framework bypasses the GIL across
`os.cpu_count()` worker processes — but saturates at that pool size
and pays fork + pickle overhead per task. Beyond ~N=1000 the pool is
saturated and throughput plateaus regardless of framework.

### Tail latency: the universal GIL ceiling

At N=10,000, NeoGraph's P99 stays in microseconds. Every Python
asyncio P99 climbs linearly with N — because the *last* coroutine in
GIL queue waits for the full run to complete before its slot.

This is not a LangGraph-specific problem. The exact same shape shows
up for Haystack (sync pipeline wrapped with `to_thread`), LlamaIndex
(async event-driven workflow), pydantic-graph (async state machine),
and AutoGen (async multi-agent runtime). If you have a Python orchestration
framework behind a single process, the GIL is the ceiling.

For any realistic server with P99 SLO expectations (say, "under 1
second for 99% of requests"), every asyncio-backed engine breaks at
some N. The exact breakpoint varies by framework — lighter runtimes
(pydantic-graph, LangGraph) break later, heavier ones (LlamaIndex,
AutoGen) break much earlier — but all of them break.

### Memory: asyncio's RSS grows with held coroutine stacks

- **NeoGraph 3.0** stays between 4.2–5.5 MB across the whole sweep.
  Tasks return immediately; only the caller-side `asio::thread_pool`
  and one io_context per in-flight `run()` are resident.
- **mp-pool modes** stay near 60–80 MB across frameworks — the worker
  pool size dominates; tasks don't accumulate because they're
  dispatched and returned one-by-one.
- **asyncio modes** grow linearly with N. Every pending coroutine
  holds a Python stack frame, closure state, and framework per-run
  state. At 10,000 in-flight coroutines this adds up to hundreds of
  MB for the heavier runtimes.

At our 512 MB memory budget, some asyncio runs get squeezed near the
cgroup ceiling at N=10,000. At a tighter 256 MB cgroup the heavier
frameworks would get OOM-killed somewhere between N=1,000 and
N=10,000. NeoGraph still has ~500 MB of headroom in that budget.

## What this bench does NOT say

- **Doesn't prove any framework "crashes" at scale.** The story is
  graceful degradation into unusable latency, not process death. At a
  tighter cgroup or higher N, OOM kills do become the exit mode — but
  we didn't document that here, and it'd require a follow-up.
- **Doesn't model LLM I/O.** A real agent workload has 100–1,000 ms
  per LLM call. That latency dwarfs the engine gap in absolute terms —
  but NOT in capacity terms: if your engine can only push 1,000 req/s
  through its runtime, no amount of concurrent LLM I/O helps.
- **Doesn't cover persistence.** Checkpointing was disabled on every
  framework. Enabling it would shift the comparison toward the store
  implementation, which is a different benchmark.
- **Workload-shape bias.** The counter chain is NeoGraph-native in
  state semantics; Haystack wraps its sync pipeline in `to_thread`,
  AutoGen encodes counter as message content, pydantic-graph has no
  fan-out (not used in this bench, but relevant for burst workloads
  with branching). Each framework is being asked to do its job, not
  its best-case job.
- **Docker on WSL2.** `--cpus` enforces CPU quota but not visible
  core count, which is why NeoGraph's `hardware_concurrency()` still
  returns the host count. Results on bare metal should be
  directionally the same but tighter at the NeoGraph end (fewer
  threads, less context-switch noise).

## Reproduce

```bash
# From the repo root.

# Build images once:
docker build -t ng-concurrent -f benchmarks/concurrent/Dockerfile.neograph .
docker build -t lg-concurrent -f benchmarks/concurrent/Dockerfile.langgraph .
docker build -t hs-concurrent -f benchmarks/concurrent/Dockerfile.haystack .
docker build -t pg-concurrent -f benchmarks/concurrent/Dockerfile.pydantic_graph .
docker build -t li-concurrent -f benchmarks/concurrent/Dockerfile.llamaindex .
docker build -t ag-concurrent -f benchmarks/concurrent/Dockerfile.autogen .

# Full matrix (88 cells across 6 engines × 2 modes × 4 concurrencies × 2 profiles,
# excluding neograph which has no mode split):
bash benchmarks/concurrent/run_matrix.sh

# Re-render charts from the results:
node benchmarks/render_concurrent.js
```

Single-cell debug run:

```bash
docker run --rm --cpus=1 --memory=512m --memory-swap=512m \
    ng-concurrent 10000

docker run --rm --cpus=1 --memory=512m --memory-swap=512m \
    li-concurrent async 10000
```

Each container prints a single JSON line of the form:

```json
{"engine":"neograph","mode":"threadpool","concurrency":10000,
 "total_wall_ms":6,"p50_us":2,"p95_us":3,"p99_us":6,
 "ok":10000,"err":0,"peak_rss_kb":7808}
```
