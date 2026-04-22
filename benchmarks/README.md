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

## Results (NeoGraph 2026-04-22 on 3.0, Python field 2026-04-19)

![Engine-overhead benchmark: per-iteration latency and peak RSS](../docs/images/bench-engine-overhead.png)

### Per-iteration overhead (µs, lower is better)

| Framework | `seq` (3-node chain) | `par` (fan-out 5 + join) | `seq` vs. NeoGraph | `par` vs. NeoGraph |
|-----------|---------------------:|-------------------------:|-------------------:|-------------------:|
| **NeoGraph 3.0** | **46.10** | **114.40** | 1× | 1× |
| Haystack | 150.70 | 293.60 | 3.3× | 2.6× |
| pydantic-graph | 240.34 | 308.32¹ | 5.2× | 2.7×¹ |
| LangGraph | 671.18 | 2,396.30 | 14.6× | 20.9× |
| LlamaIndex Workflow | 1,842.58 | 4,781.24 | 40.0× | 41.8× |
| AutoGen GraphFlow | 3,226.79 | 7,389.42 | 70.0× | 64.6× |

¹ pydantic-graph `par` is a serial 6-node emulation — it does not
support fan-out. Not a parallel workload; included for completeness.

### End-to-end process metrics

Whole binary/script runtime including warm-up + both workloads.
`seq` = 10,000 iters, `par` = 5,000 iters (3.0 bench parameters).
Python rows are carried over from the 2026-04-19 20k/10k run and
scale linearly in the engine overhead, so the absolute elapsed
numbers aren't directly comparable across rows — the per-iter
µs column above is the apples-to-apples axis.

| Framework | Total elapsed | Peak RSS | CPU utilization |
|-----------|--------------:|---------:|----------------:|
| **NeoGraph 3.0** | **~1.04 s** | **5.5 MB** | 100% (single-thread io_context by default) |
| Haystack | 6.72 s | 78.3 MB | 100% (GIL) |
| pydantic-graph | 8.05 s | 34.9 MB | 100% (GIL) |
| LangGraph | 35.64 s | 58.9 MB | 100% (GIL) |
| LlamaIndex Workflow | 85.77 s | 101.5 MB | 100% (GIL) |
| AutoGen GraphFlow | 138.79 s | 52.4 MB | 100% (GIL) |

NeoGraph 3.0's default super-step loop runs the coroutine on a
single-threaded io_context via `run_sync`; CPU-parallel fan-out is
opt-in via `engine->set_worker_count(N)`. For I/O-bound node
workloads the single thread still overlaps via co_await suspension —
the `par` iteration still beats every Python framework's apples-to-
apples number.

## What the numbers mean

1. **Per-run engine overhead spans ~3× to ~70× across the Python
   field.** Haystack is the leanest competitor (DAG with typed
   sockets, minimal runtime). At the other end, AutoGen and LlamaIndex
   are async/event-driven with per-run state setup that dominates when
   there's no real work to amortize over.
2. **3.0 narrowed the seq gap vs. 2.0** — NeoGraph traded some sync-
   path speed (21 µs → 46 µs on seq) for a unified coroutine
   architecture. In exchange, `par` actually got faster (150 µs → 114
   µs) because `make_parallel_group` on a single-thread io_context
   dispatches cheaper than Taskflow's scheduler for the fan-out-5
   workload. The CPU-parallel path is still available — opt in via
   `engine->set_worker_count(N)`.
3. **Memory footprint favors NeoGraph by an order of magnitude or
   more.** 5.5 MB (NeoGraph) vs. 35–101 MB across the Python field.
   On SBC-class targets (Raspberry Pi-class RAM) this is the
   load-bearing metric — the difference between "runs comfortably" and
   "run it carefully".
4. **Parallel fan-out is available but no longer default.** NeoGraph 2.x
   shipped Taskflow's work-stealing pool as the default, which hit 400%+
   CPU on 5-way fan-out. 3.0 ships the coroutine path as default
   (single-thread dispatch, cheap) and exposes the multi-threaded pool
   as opt-in — the right default for agent workloads that are
   I/O-bound (LLM latency dominates) and would otherwise pay
   thread-creation overhead with no speedup.

## Caveats — what this bench does NOT measure

* **Real agent workloads.** LLM-dominated pipelines are bottlenecked
  by provider latency (100ms–10s per call). Engine overhead disappears
  at that scale. Mental model: NeoGraph 3.0 costs ~46 µs/call, Haystack
  ~150µs, LangGraph ~670µs, LlamaIndex/AutoGen ~2–8ms — all invisible
  next to a 500ms API round trip. This bench matters for non-LLM
  nodes, dense agent orchestration, and startup-heavy deployments.
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
* **Fairness.** NeoGraph was built with `-O2 -DNDEBUG`. Every Python
  framework is stock CPython 3.12 with current pip-installed versions
  — production-typical deployments, no custom tuning.

## Reproduce

```bash
# Build NeoGraph (release) once:
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Build + run the C++ bench (built by CMake above):
./build/bench_neograph                   # defaults: seq=10000, par=5000

# Or rebuild standalone with the same flags CMake uses:
g++ -std=c++20 -O2 -DNDEBUG \
    -Iinclude -Ideps -Ideps/yyjson -Ideps/asio/include \
    -DASIO_STANDALONE \
    benchmarks/bench_neograph.cpp \
    build/libneograph_core.a build/libyyjson.a \
    -pthread -o /tmp/bench_neograph

# Shared Python venv for every Python framework:
python3 -m venv /tmp/bench_venv
/tmp/bench_venv/bin/pip install \
    langgraph \
    haystack-ai \
    pydantic-graph \
    llama-index-core \
    "autogen-agentchat" "autogen-core" "autogen-ext"

# Run each bench:
/tmp/bench_venv/bin/python benchmarks/bench_langgraph.py      20000 10000
/tmp/bench_venv/bin/python benchmarks/bench_haystack.py       20000 10000
/tmp/bench_venv/bin/python benchmarks/bench_pydantic_graph.py 20000 10000
/tmp/bench_venv/bin/python benchmarks/bench_llamaindex.py     20000 10000
/tmp/bench_venv/bin/python benchmarks/bench_autogen.py        20000 10000

# Peak RSS: wrap any of the above with `/usr/bin/time -v`.
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
