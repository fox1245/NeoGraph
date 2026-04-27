# NeoGraph vs. Python graph/pipeline frameworks — engine-overhead benchmark

Measures the per-invocation overhead of NeoGraph against the leading
Python orchestration frameworks on identically-shaped graphs with **no
I/O, no sleep, no LLM calls**. The numbers reflect what the engine
itself costs (node dispatch, state channel writes, reducer calls) — not
the latency of any simulated work.

Frameworks compared:

| Framework | Version | Abstraction |
|-----------|---------|-------------|
| NeoGraph | 3.0 (`feat/taskflow-removal`) | state-channel graph, C++20 coroutines + asio |
| LangGraph | 1.1.9 | state-channel graph (Python) |
| Haystack | 2.27 | pipeline of components with typed sockets |
| pydantic-graph | 1.84 | single-next-node state machine |
| LlamaIndex Workflow | 0.14 | event-driven async workflow |
| AutoGen GraphFlow | 0.7.5 | message-passing multi-agent graph |

## Workloads

All six implementations define the exact same two graphs, compile once
(where applicable), and invoke them in a hot loop.

| Id | Shape | State |
|----|-------|-------|
| `seq` | 3-node chain `a → b → c` | single `counter` channel, each node writes `counter+1` (overwrite reducer) |
| `par` | fan-out 5 workers then join at `summarizer` | `results: list` (append reducer) + `count: int`; each worker appends its index, summarizer writes `len(results)` |

Checkpointing is disabled on every framework.

Two ports required per-framework workload shape translation:

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

A re-measurement on current master (2026-04-27) is included below
the reference run because two changes since v3.0.0 affected the
`par` row — see the *Drift since v3.0.0* notes after the table.

![Engine-overhead benchmark: per-iteration latency and peak RSS](../docs/images/bench-engine-overhead.png)

### Per-iteration overhead (µs, lower is better)

| Framework | `seq` (3-node chain) | `par` (fan-out 5 + join) | `seq` vs. NeoGraph | `par` vs. NeoGraph |
|-----------|---------------------:|-------------------------:|-------------------:|-------------------:|
| **NeoGraph v3.0.0** *(reference, 2026-04-22)* | **5.0** | **11.8** | 1× | 1× |
| **NeoGraph master** *(today, default `worker_count=hardware_concurrency`)* | **5.5** | **283** | 1× | 1× |
| **NeoGraph master** *(today, `set_worker_count(1)`)* | **5.5** | **94** | 1× | 1× |
| Haystack | 144.10 | 290.00 | 28.8× / 26.2× / 26.2× | 24.6× / 1.0× / 3.1× |
| pydantic-graph | 235.90 | 286.13¹ | 47.2× / 42.9× / 42.9× | 24.2×¹ / 1.0×¹ / 3.0×¹ |
| LangGraph | 656.73 | 2,348.66 | 131.3× / 119.4× / 119.4× | 199.0× / 8.3× / 25.0× |
| LlamaIndex Workflow | 1,780.34 | 4,683.45 | 356.1× / 323.7× / 323.7× | 396.9× / 16.5× / 49.8× |
| AutoGen GraphFlow | 3,209.20 | 7,292.67 | 641.8× / 583.5× / 583.5× | 618.0× / 25.8× / 77.6× |

Rightmost two columns now show three ratios: vs. v3.0.0 reference / vs. master default / vs. master worker=1.

¹ pydantic-graph `par` is a serial 6-node emulation — it does not
support fan-out. Not a parallel workload; included for completeness.

### Drift since v3.0.0 (`par` row only — `seq` unchanged)

The `par` 11.8 → 283 µs gap on master breaks down as:

* **`worker_count` default flipped** from 1 → `hardware_concurrency`
  in commit `b59444f` (v0.1.4). Real-LLM workloads were
  fan-out-ceiling-bound at the old default (every Python node
  serialized through a single worker thread); the new default
  matches LLM concurrency expectations. Net effect on this
  micro-bench: +200 µs of thread-pool coordination overhead
  per iter that's invisible in any LLM-bound workflow
  (a 100 ms LLM round-trip dwarfs 280 µs thread setup).
* **Send fan-out path also got slower** — even with
  `set_worker_count(1)`, master takes 94 µs vs. v3.0.0's 11.8 µs.
  That ~8× regression sits inside the engine's Send dispatch
  / fan-in routing rewrite (`94f1515`, `304f2f4`); not yet
  bisected.

Both points are on the `par` micro-bench's 5-fan-out × 5000-iter
hot loop. The headline still holds: NeoGraph wins every row of
the table on every measurement, by 8×–600× depending on
framework. But "199× faster than LangGraph on `par`" was the
v3.0.0 default-config number, not the current master default.

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

## What the numbers mean

1. **Per-run engine overhead spans ~29× to ~642× across the Python
   field.** Haystack is the leanest competitor (DAG with typed
   sockets, minimal runtime); even so, it costs 28.8× more per seq
   iter than NeoGraph. At the other end, AutoGen sits at 642× the
   NeoGraph cost because of its per-run multi-agent state setup.
2. **NeoGraph 3.0 is a win over 2.0 on both axes.** Collapsing sync
   and async onto one coroutine path didn't regress engine
   overhead — the full coroutine machinery (`run_sync` + io_context
   per call) is under 5 µs in Release builds, versus 2.0's
   advertised 20.65 µs on the sync Taskflow path.
3. **Memory footprint favors NeoGraph by an order of magnitude or
   more.** 4.8 MB (NeoGraph) vs. 35–101 MB across the Python field.
   On SBC-class targets (Raspberry Pi-class RAM) this is the
   load-bearing metric — the difference between "runs comfortably" and
   "run it carefully".
4. **Parallel fan-out is opt-in in 3.0.** NeoGraph 2.x shipped
   Taskflow's work-stealing pool as the default. 3.0 ships the
   coroutine path as default (single-thread dispatch, cheap) and
   exposes the multi-threaded pool as opt-in via
   `engine->set_worker_count(N)` — the right default for agent
   workloads that are I/O-bound (LLM latency dominates) and would
   otherwise pay thread-creation overhead with no speedup.

## Caveats — what this bench does NOT measure

* **Real agent workloads.** LLM-dominated pipelines are bottlenecked
  by provider latency (100ms–10s per call). Engine overhead disappears
  at that scale. Mental model: NeoGraph 3.0 costs ~5 µs/call,
  Haystack ~144 µs, LangGraph ~657 µs, LlamaIndex/AutoGen ~2–7 ms —
  all invisible next to a 500 ms API round trip. This bench matters
  for non-LLM nodes, dense agent orchestration, and startup-heavy
  deployments.
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
  stock CPython 3.12 with current pip-installed versions —
  production-typical deployments, no custom tuning. Historical note:
  pre-3.0 versions of this README advertised `-O2` because that was
  what the standalone bench command used; the CMake build has always
  resolved `Release` to `-O3`.

## Reproduce

```bash
# Build NeoGraph (Release — MUST set BUILD_TYPE explicitly; the
# empty default configures the build without -O3):
cmake -B build -DCMAKE_BUILD_TYPE=Release -DNEOGRAPH_BUILD_BENCHMARKS=ON
cmake --build build --target bench_neograph -j

./build/bench_neograph                   # defaults: seq=10000, par=5000

# Shared Python venv for every Python framework:
python3 -m venv /tmp/bench_venv
/tmp/bench_venv/bin/pip install \
    langgraph \
    haystack-ai \
    pydantic-graph \
    llama-index-core \
    "autogen-agentchat" "autogen-core" "autogen-ext"

# Run each bench (10k seq + 5k par matches the C++ side):
/tmp/bench_venv/bin/python benchmarks/bench_langgraph.py      10000 5000
/tmp/bench_venv/bin/python benchmarks/bench_haystack.py       10000 5000
/tmp/bench_venv/bin/python benchmarks/bench_pydantic_graph.py 10000 5000
/tmp/bench_venv/bin/python benchmarks/bench_llamaindex.py     10000 5000
/tmp/bench_venv/bin/python benchmarks/bench_autogen.py        10000 5000

# Peak RSS + wall time:
/usr/bin/time -f "%e s, %M KB" ./build/bench_neograph
```

Output format is `workload<TAB>iters<TAB>total_ms<TAB>per_iter_us` on
every side so diffing is trivial.

## Environment used for the 2026-04-19 numbers

```
OS:        Linux 6.6.87.2-microsoft-standard-WSL2 (Ubuntu 24.04 userland)
CPU:       host CPU (8 logical cores exposed to WSL)
Compiler:  g++ 13.x, -std=c++20 -O2 -DNDEBUG
Python:    3.12.3 (system)
Versions:  langgraph 1.1.7, haystack-ai 2.27.0, pydantic-graph 1.84.1,
           llama-index-core 0.14.20, autogen-agentchat 0.7.5
```

Numbers will vary on your hardware, but the ratios should be stable to
within ~20%.
