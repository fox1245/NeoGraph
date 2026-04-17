# Concurrent-load benchmark — NeoGraph vs LangGraph

Under a **burst** load — N requests submitted simultaneously, then the
test waits for all to complete — how do the two engines scale, and at
what point does each approach stop being viable?

This bench answers that in a reproducible Docker sandbox with CPU and
memory limits matching an SBC-class target.

## Setup

- **Workload**: 3-node sequential counter chain (`a → b → c`), each
  node increments a single state channel. No I/O, no sleep.
- **Burst pattern**: N tasks submitted at t=0; the runner waits for all
  to finish; per-request latency is captured in-process.
- **Sandbox**: Docker with `--cpus` and `--memory` (+ `--memory-swap`
  matched so OOM fires instead of swapping). Matrix:
  - Profile **1 CPU / 512 MB** — the "tight SBC" target (primary)
  - Profile **2 CPU / 1 GB** — the "comfortable SBC" target
- **Engines tested**:
  - `neograph` — `tf::Executor` thread pool with `hardware_concurrency()` workers, each task invokes `engine->run()`.
  - `langgraph-asyncio` — `asyncio.gather(*[graph.ainvoke(state) for _ in range(N)])` in a single process.
  - `langgraph-mp` — `multiprocessing.Pool(P).map(worker, [state] * N)` with `P = os.cpu_count()`.
- **Concurrencies**: N ∈ {10, 100, 1000, 5000, 10000}
- **Versions**: LangGraph 1.1.7, CPython 3.12 (python:3.12-slim),
  g++ 13.3 (ubuntu:24.04), NeoGraph HEAD.

## Results — 1 CPU / 512 MB profile

![Throughput — requests per second](../../docs/images/bench-concurrent-throughput.png)

![Tail latency — P99 per request](../../docs/images/bench-concurrent-latency.png)

![Peak resident memory](../../docs/images/bench-concurrent-rss.png)

### Raw numbers (1 CPU / 512 MB)

| N | Engine | Wall (ms) | Throughput (req/s) | P50 | P99 | Peak RSS |
|---|--------|-----------|--------------------|-----|-----|----------|
| 10 | NeoGraph | <1 | ≥10,000 | 85 µs | **472 µs** | 4.4 MB |
| 10 | LG asyncio | 17 | 588 | 14.5 ms | 16.3 ms | 59.2 MB |
| 10 | LG mp | 100 | 100 | 4.6 ms | 62.7 ms | 61.7 MB |
| 100 | NeoGraph | <1 | ≥100,000 | 3 µs | **537 µs** | 4.5 MB |
| 100 | LG asyncio | 128 | 781 | 101.3 ms | 125.8 ms | 62.7 MB |
| 100 | LG mp | 212 | 472 | 784 µs | 77.2 ms | 62.2 MB |
| 1,000 | NeoGraph | <1 | ≥1,000,000 | 3 µs | **5 µs** | 4.7 MB |
| 1,000 | LG asyncio | 1,340 | 746 | 1.10 s | 1.28 s | 95.8 MB |
| 1,000 | LG mp | 928 | 1,078 | 789 µs | 89.3 ms | 61.8 MB |
| 5,000 | NeoGraph | 3 | ~1.67M | 3 µs | **8 µs** | 5.9 MB |
| 5,000 | LG asyncio | 9,784 | 511 | 8.27 s | 9.64 s | 242.7 MB |
| 5,000 | LG mp | 3,971 | 1,259 | 733 µs | 89.4 ms | 61.9 MB |
| **10,000** | **NeoGraph** | **6** | **~1.67M** | **2 µs** | **6 µs** | **7.8 MB** |
| **10,000** | **LG asyncio** | **20,348** | **491** | **17.18 s** | **20.02 s** | **425.6 MB** |
| **10,000** | **LG mp** | **8,046** | **1,243** | **750 µs** | **89.3 ms** | **61.9 MB** |

Every cell completed — no engine crashed, no OOM kill, all 30/30 cells
green. **But the scaling curves diverge by three orders of magnitude.**

## Interpretation

### Throughput: NeoGraph scales, LangGraph plateaus

The green curve climbs from 10k req/s to 1.67M req/s as concurrency
grows — Taskflow's work-stealing scheduler amortizes per-task setup
over the batch. Taskflow's default executor uses every logical CPU,
and the cgroup's 1-CPU quota only bounds wall time, not thread count,
so short tasks interleave cleanly.

The LangGraph curves plateau because neither asyncio nor a process
pool scales past its single bottleneck:

- **`asyncio.gather`** puts all N coroutines on one event loop in one
  process. Our workload is CPU-bound, so every coroutine still has to
  take the GIL to execute its `graph.ainvoke()`. N coroutines →
  serialised execution → flat ~500 req/s regardless of N.
- **`multiprocessing.Pool(7)`** bypasses the GIL across 7 processes,
  so it actually scales with concurrency up to the pool size. Past
  that, tasks queue. Beyond N=1000 the pool is saturated and
  throughput plateaus at ~1,250 req/s.

NeoGraph at N=10,000 delivers **~1,340× the LangGraph-mp throughput**
and **~3,400× the asyncio throughput**.

### Tail latency: asyncio's P99 grows linearly with N

The most dramatic chart. At N=10,000:

- **NeoGraph P99 = 6 µs.** A single request's worst case is 6 microseconds.
- **LangGraph mp P99 = 89 ms.** Tasks that get queued behind 10 ahead of them wait ~90ms.
- **LangGraph asyncio P99 = 20,022 ms (20 seconds).** The median (P50)
  coroutine waits over 17 seconds for its GIL slot. The P99 coroutine
  waits the full run's duration — essentially, it's last in line.

The 10,000× concurrency delta between N=10 and N=10,000 shows up in
the asyncio numbers almost linearly: 16ms → 20s is a 1,200× latency
growth. It's a textbook GIL-serialisation symptom.

For any realistic server with P99 SLO expectations (say, "under 1
second for 99% of requests"), asyncio breaks at ~N=500 and mp-pool
breaks at ~N=8 (because P99 is already 62ms at N=10, meaning even
low-traffic servers eat the full pool-wait cost).

### Memory: asyncio's RSS grows with held coroutine stacks

- **NeoGraph** stays between 4.4–7.8 MB across the whole sweep. Tasks
  return immediately; only the Taskflow pool is resident.
- **LangGraph mp-pool** stays near 62 MB — the pool of 7 worker
  processes is the floor, and tasks don't accumulate because they're
  dispatched and returned one-by-one.
- **LangGraph asyncio** starts at 58 MB (Python + langgraph baseline)
  and grows to 425 MB at N=10,000. Every pending coroutine holds a
  Python stack frame, closure state, and LangGraph runtime state. At
  10,000 in-flight coroutines, that's ~37 KB each on top of the
  baseline.

At our 512 MB memory budget, asyncio leaves ~80 MB of headroom at
N=10,000. At a tighter 256 MB cgroup the asyncio run would get
OOM-killed somewhere between N=5,000 and N=10,000. NeoGraph still has
504 MB of headroom in that budget.

## What this bench does NOT say

- **Doesn't prove LangGraph "crashes" at scale.** Every cell
  completed. The story is graceful degradation into unusable latency,
  not process death. At a tighter cgroup or higher N, OOM kills do
  become the exit mode — but we didn't document that here, and it'd
  require a follow-up.
- **Doesn't model LLM I/O.** A real agent workload has 100–1,000 ms
  per LLM call. That latency dwarfs the engine gap in absolute terms —
  but NOT in capacity terms: if your engine can only serialize 1,250
  req/s through its runtime, no amount of concurrent LLM I/O helps.
- **Doesn't cover persistence.** Checkpointing was disabled on both
  sides. Enabling it would shift the comparison toward the store
  implementation, which is a different benchmark.
- **Docker on WSL2.** `--cpus` enforces CPU quota but not visible core
  count, which is why NeoGraph's `hardware_concurrency()` still
  returns the host count. Results on bare metal should be directionally
  the same but tighter at the NeoGraph end (fewer threads, less
  context-switch noise).

## Reproduce

```bash
# From the repo root.
docker build -t ng-concurrent -f benchmarks/concurrent/Dockerfile.neograph .
docker build -t lg-concurrent -f benchmarks/concurrent/Dockerfile.langgraph .

# Full matrix (30 cells, ~5-10 minutes):
bash benchmarks/concurrent/run_matrix.sh

# Re-render charts from the results:
node benchmarks/render_concurrent.js
```

Single-cell debug run:

```bash
docker run --rm --cpus=1 --memory=512m --memory-swap=512m \
    ng-concurrent 10000

docker run --rm --cpus=1 --memory=512m --memory-swap=512m \
    lg-concurrent async 10000

docker run --rm --cpus=1 --memory=512m --memory-swap=512m \
    lg-concurrent mp 10000
```

Each container prints a single JSON line of the form:

```json
{"engine":"neograph","mode":"threadpool","concurrency":10000,
 "total_wall_ms":6,"p50_us":2,"p95_us":3,"p99_us":6,
 "ok":10000,"err":0,"peak_rss_kb":7808}
```
