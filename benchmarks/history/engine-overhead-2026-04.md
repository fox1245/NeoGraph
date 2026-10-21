# Engine-overhead benchmark, 2026-04 (x86_64) and 2026-07 (Arm64): superseded

**Languages:** [English](engine-overhead-2026-04.md) | [한국어](engine-overhead-2026-04.ko.md) | [日本語](engine-overhead-2026-04.ja.md) | [简体中文](engine-overhead-2026-04.zh-CN.md)

These tables were measured in 2026-04 (x86_64) and 2026-07 (Arm64), in separate sessions, and are kept unchanged for the record. The current cohort, measured in one session, is in [the benchmark guide](../README.md). Do not compare the figures below with it. The items below are now known to be invalid or unreliable.

1. The first table mixes sessions: the NeoGraph rows come from 2026-04-22 and 2026-04-29, the Python rows from 2026-04-29, and the ratios divide medians across sessions. The text above it, the framework list, the environment block and the chart name different versions.
2. LangGraph used the synchronous `invoke` API, which runs a fan-out on a thread pool created for every call. The `par` ratio of 157× therefore compares a thread pool with NeoGraph's inline dispatch (`worker_count=1`); the pool-to-pool ratio printed beside it, 8.1×, is the matched one. The Executor column called this path "single-thread asyncio", which it is not.
3. pydantic-graph used the legacy `Graph(nodes=[...])` API, which current releases no longer have, and its `par` row is a serial six-node chain. The statement that pydantic-graph cannot fan out is wrong: the `GraphBuilder` API broadcasts to steps that run concurrently.
4. The LlamaIndex rows constructed and validated a new workflow instance in every timed iteration, did not record the llama-index-workflows version, and used a join idiom that runs the summarizer step five times. A workflow instance can be reused.
5. The memory columns compare a native process with Python processes that include the interpreter and the imported framework. The command that was given to reproduce the elapsed-time column runs five rows of five samples each, many times the work of the binary that produced it.
6. No script checked the answer of a run, and no spread across repetitions was recorded. The build was described as `-O3 -DNDEBUG`; the default build also carries the hardening flags.
7. The pydantic-graph engine figure (about 227 µs per run) and the 70 to 100 µs of the burst harness cannot both describe the same call. They came from different sessions in environments that were not recorded; the current guide measures both shapes back to back, and shows what an installed `logfire` package does to the cost of a run.

| Framework | Version | Abstraction |
|-----------|---------|-------------|
| NeoGraph | 3.0 | state-channel graph, C++20 coroutines + asio |
| LangGraph | 1.1.9 | state-channel graph (Python) |
| Haystack | 2.27 | pipeline of components with typed sockets |
| pydantic-graph | 1.84 | single-next-node state machine |
| LlamaIndex Workflow | 0.14 | event-driven async workflow |
| AutoGen GraphFlow | 0.7.5 | message-passing multi-agent graph |

## Workloads

The six implementations port the same two workloads, compile once where
applicable, and invoke in a hot loop. State and topology adaptations are listed below.

| Id | Shape | State |
|----|-------|-------|
| `seq` | 3-node chain `a → b → c` | single `counter` channel, each node writes `counter+1` (overwrite reducer) |
| `par` | fan-out 5 workers then join at `summarizer` | `results: list` (append reducer) + `count: int`; each worker appends its index, summarizer writes `len(results)` |

Checkpointing is disabled on every framework.

Three ports required framework-specific workload adaptations:

* **Haystack** has no append reducer — each worker emits on its own
  typed socket and the summarizer sums the list lengths. Same number of
  components dispatched per run.
* **pydantic-graph** is a single-next-node state machine and cannot
  fan out. The `par` workload is emulated as a 6-node serial chain
  (`w1 → w2 → w3 → w4 → w5 → summ`). Flagged in the results — not an
  apples-to-apples parallel-fan-out measurement.
* **AutoGen** is message-passing, not state-channel. The counter is
  encoded as text message content. The summarizer counts incoming
  worker messages. Same graph shape, different state model.

## Results

The **reference run** below was measured 2026-04-22 against
NeoGraph v3.0.0 on x86_64 Linux, g++ 13 Release `-O3 -DNDEBUG`,
CPython 3.12.3. NeoGraph: 10-run median of `bench_neograph`.
Python field: 3-run median per framework. Versions: neograph v3.0.0,
langgraph 1.1.9, haystack-ai 2.28.0, pydantic-graph 1.85.1,
llama-index-core 0.14.21, autogen-agentchat 0.7.5.

A re-measurement on then-current master (2026-04-29) is included below
the reference run. The `par` row reports both supported execution
regimes explicitly: the current `worker_count=1` default and the
engine-owned pool enabled by `set_worker_count_auto()`. See *Notes*
after the table for why a no-I/O microbenchmark should keep these
numbers separate.

### Per-iteration overhead (µs, lower is better)

| Framework | `seq` (3-node chain) | `par` (fan-out 5 + join) | `seq` vs. NeoGraph | `par` vs. NeoGraph |
|-----------|---------------------:|-------------------------:|-------------------:|-------------------:|
| **NeoGraph v3.0.0** *(reference, 2026-04-22)* | **5.0** | **11.8** | 1× | 1× |
| **NeoGraph master** *(2026-04-29, default `worker_count=1`)* | **5.25** | **14.4** | 1× | 1× |
| **NeoGraph master** *(2026-04-29, `set_worker_count_auto()`)* | **5.25** | **278** | 1× | 1× |
| Haystack 2.28.0 | 139.85 | 278.48 | 28.0× / 26.6× / 26.6× | 23.6× / 19.3× / 1.0× |
| pydantic-graph 1.87.0 | 227.14 | 280.26¹ | 45.4× / 43.3× / 43.3× | 23.7×¹ / 19.5×¹ / 1.0×¹ |
| LangGraph 1.1.10 | 642.62 | 2,261.55 | 128.5× / 122.4× / 122.4× | 191.7× / 157.1× / 8.1× |
| LlamaIndex Workflow 0.14.21 | 1,564.54 | 4,373.76 | 312.9× / 298.0× / 298.0× | 370.7× / 303.7× / 15.7× |
| AutoGen GraphFlow 0.7.5 | 3,126.86 | 7,281.08 | 625.4× / 595.6× / 595.6× | 617.0× / 505.6× / 26.2× |

Rightmost two columns show three ratios: vs. v3.0.0 reference / vs. master
worker=1 default / vs. master auto-worker mode.

The dated `seq` measurements are close: NeoGraph changed from 5.0 to 5.25 µs.
The `par` measurements differ with execution mode (11.8, 14.4, and 278 µs);
they do not reproduce every reference row within ±10% or measure current HEAD.

¹ pydantic-graph `par` is a serial 6-node emulation — it does not
support fan-out. Not a parallel workload; included for completeness.

### Notes on the `par` row (`seq` is unchanged)

The `par` 14.4 → 278 µs gap is the measured cost of opting into an
engine-owned thread pool for five nodes that do no real work:

* **Current `build()` / `compile()` default: `worker_count=1`.** No
  engine-owned pool is installed, so fan-out dispatches on the caller's
  executor. This is the 14.4 µs row and the correct regression signal for
  CPU-tiny nodes or graphs whose nodes hold non-thread-safe state.
* **Parallel fan-out is explicit.** `set_worker_count_auto()` sizes the
  pool to `hardware_concurrency`; `set_worker_count(N)` chooses a fixed
  ceiling. This is the 278 µs row. The coordination cost is visible here
  because each worker only appends one integer, but is negligible beside a
  100 ms LLM call and allows independent nodes to overlap.

Run both regimes without changing the benchmark source:

```bash
./build/bench_neograph 10000 5000 1
./build/bench_neograph 10000 5000 auto
```

The third argument applies to the `par` engine and accepts `1` (default),
`auto`, or any positive worker count. Output includes a
`config\tpar_workers\t...` row so saved results retain their execution mode.

In the dated worker=1 `par` comparison, LangGraph's 2,261.55 µs is
157.1 times NeoGraph's 14.4 µs. With the engine-owned pool, the ratio is
8.1 times. Haystack and pydantic-graph are approximately tied with NeoGraph's
278 µs auto-worker row. These ratios describe this workload and configuration.

### End-to-end process metrics

Whole binary/script runtime including warm-up + both workloads.
`seq` = 10,000 iters, `par` = 5,000 iters. Measured with
`/usr/bin/time -f "%e s, %M KB"`.

| Framework | Total elapsed | Peak RSS | Executor |
|-----------|--------------:|---------:|---------:|
| **NeoGraph 3.0** | **0.11 s** | **4.5 MB** | single-thread io_context by default |
| pydantic-graph | 3.98 s | 35.1 MB | single-thread asyncio (GIL) |
| Haystack | 3.85 s | 80.3 MB | single-thread asyncio (GIL) |
| LangGraph | 18.95 s | 60.1 MB | single-thread asyncio (GIL) |
| LlamaIndex Workflow | 39.49 s | 101.4 MB | single-thread asyncio (GIL) |
| AutoGen GraphFlow | 63.29 s | 52.3 MB | single-thread asyncio (GIL) |

NeoGraph 3.0's default super-step loop runs the coroutine on a
single-threaded io_context via `run_sync`; CPU-parallel fan-out is
opt-in via `engine->set_worker_count(N)`. For I/O-bound node
workloads the single thread still overlaps via co_await suspension.

## Linux ARM64 baseline: Neoverse-N1

This is a separate native ARM64 platform baseline from
[#165](https://github.com/fox1245/NeoGraph/issues/165), not a regression
comparison with the x86_64 table above. It is pinned to revision `d7a6477`
and its own dependency set so later measurements remain attributable.

| Item | Value |
|---|---|
| Date | 2026-07-22 |
| OS | Ubuntu 24.04, Linux `6.17.0-1018-oracle` |
| Architecture | `aarch64` |
| CPU | 4 vCPU, ARM Neoverse-N1 |
| Memory | 23 GiB, no swap |
| Compiler / CMake | GCC 13.3.0 / CMake 3.28.3 |
| Python | CPython 3.12.3 |
| NeoGraph revision | `d7a6477` |

The workload and iteration counts match the primary benchmark: 10,000
`seq` iterations and 5,000 `par` iterations after compiling each graph
once. NeoGraph was measured 10 times and each Python implementation three
times; the table reports medians. Checkpointing, network I/O, model calls,
and sleeps were disabled. Whole-process peak RSS came from
`/usr/bin/time -f "%e s, %M KB"`.

| Framework | `seq` (µs/iter) | `par` (µs/iter) | `seq` vs. NeoGraph | `par` vs. NeoGraph | Peak RSS |
|---|---:|---:|---:|---:|---:|
| **NeoGraph** | **9.50** | **21.80** | 1× | 1× | **4.35 MB** |
| Haystack 3.0.0 | 153.44 | 329.67 | 16.2× | 15.1× | 73.6 MB |
| pydantic-graph 1.87.0 | 342.60 | 405.87¹ | 36.1× | 18.6×¹ | 32.1 MB |
| LangGraph 1.2.9 | 1,037.55 | 3,289.22 | 109.2× | 150.9× | 63.7 MB |
| LlamaIndex 0.14.23 | 2,765.04 | 7,824.85 | 291.1× | 358.9× | 96.9 MB |
| AutoGen 0.7.5 | 4,166.39 | 9,571.11 | 438.6× | 439.0× | 47.8 MB |

NeoGraph used the worker=1 default, so the `par` row measures topology,
reducer, and serial dispatch overhead rather than engine-owned thread-pool
execution. Use the explicit `auto` benchmark mode for a separate parallel
fan-out measurement; do not combine the two modes into one headline.

¹ pydantic-graph cannot model this fan-out topology; its `par` row is the
same serial six-node emulation described above. Version 1.87.0 was pinned
because the then-current 2.15.0 API no longer supported the benchmark's
`Graph(...)` constructor. Processes were not CPU-pinned or cgroup-constrained.

## What the numbers mean

1. The x86_64 reference `seq` rows range from 28.0 times NeoGraph's
   per-run overhead for Haystack to 625.4 times for AutoGen. These are
   framework/workload measurements, not isolated explanations of their costs.
2. The reference whole-process peak RSS is 4.5 MB for NeoGraph and
   35.1–101.4 MB for the Python implementations. These values include
   each process's runtime and imports, not a deployed application's memory.
3. Worker=1 dispatch and an engine-owned thread pool are different execution
   regimes. Measure both for the intended workload; a no-work fan-out benchmark
   exposes coordination costs but does not predict model or I/O latency.

## Caveats — what this bench does NOT measure

* **Real agent workloads.** No model inference, network request, or tool I/O
  occurs in the framework comparison. Those costs can dominate an application,
  so the table does not establish end-to-end agent speedups.
* **Framework-appropriate workloads.** AutoGen, LlamaIndex, and
  pydantic-graph each optimize for paradigms (multi-agent chat,
  event-driven long-running workflows, state-machine control flow)
  that this bench does not exercise. We measure them on NeoGraph's
  home turf.
* **Checkpoint throughput.** Enabling persistence on each framework
  would let serialization cost dominate; that's a different benchmark.
* **Cold start.** Each implementation includes a 10-iter warm-up loop
  before measurement. Full-process numbers include Python interpreter
  boot (~200ms) and framework import time, which varies widely (LlamaIndex
  and AutoGen import substantial trees).
* **Fairness.** NeoGraph was built with CMake `-DCMAKE_BUILD_TYPE=Release`
  which resolves to `-O3 -DNDEBUG` on GCC. Every Python framework is
  stock CPython 3.12 with the dependency versions recorded for each cohort,
  without custom tuning. The standalone bench command used `-O2`; the CMake build resolves
  `Release` to `-O3`.

## Environment used for the 2026-04-19 numbers

```
OS:        Linux 6.6.87.2-microsoft-standard-WSL2 (Ubuntu 24.04 userland)
CPU:       host CPU (8 logical cores exposed to WSL)
Compiler:  g++ 13.x, -std=c++20 -O2 -DNDEBUG
Python:    3.12.3 (system)
Versions:  langgraph 1.1.7, haystack-ai 2.27.0, pydantic-graph 1.84.1,
           llama-index-core 0.14.20, autogen-agentchat 0.7.5
```

Hardware, runtime versions, workload shape, and worker mode can change both
latencies and ratios; this document establishes no cross-platform tolerance.
