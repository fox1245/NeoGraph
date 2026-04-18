# NeoGraph vs. Python graph/pipeline frameworks — engine-overhead benchmark

Measures the per-invocation overhead of NeoGraph against the leading
Python orchestration frameworks on identically-shaped graphs with **no
I/O, no sleep, no LLM calls**. The numbers reflect what the engine
itself costs (node dispatch, state channel writes, reducer calls) — not
the latency of any simulated work.

Frameworks compared:

| Framework | Version | Abstraction |
|-----------|---------|-------------|
| NeoGraph | master | state-channel graph, Taskflow executor (C++) |
| LangGraph | 1.1.7 | state-channel graph (Python) |
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

## Results (2026-04-19)

![Engine-overhead benchmark: per-iteration latency and peak RSS](../docs/images/bench-engine-overhead.png)

### Per-iteration overhead (µs, lower is better)

| Framework | `seq` (3-node chain) | `par` (fan-out 5 + join) | `seq` vs. NeoGraph | `par` vs. NeoGraph |
|-----------|---------------------:|-------------------------:|-------------------:|-------------------:|
| **NeoGraph** | **20.65** | **150.70** | 1× | 1× |
| Haystack | 150.70 | 293.60 | 7.3× | 1.9× |
| pydantic-graph | 240.34 | 308.32¹ | 11.6× | 2.0×¹ |
| LangGraph | 645.30 | 2,225.12 | 31.2× | 14.8× |
| LlamaIndex Workflow | 1,842.58 | 4,781.24 | 89.2× | 31.7× |
| AutoGen GraphFlow | 3,226.79 | 7,389.42 | 156.3× | 49.0× |

¹ pydantic-graph `par` is a serial 6-node emulation — it does not
support fan-out. Not a parallel workload; included for completeness.

### End-to-end process metrics

Whole binary/script runtime including warm-up + both workloads.
`seq` = 20,000 iters, `par` = 10,000 iters.

| Framework | Total elapsed | Peak RSS | CPU utilization |
|-----------|--------------:|---------:|----------------:|
| **NeoGraph** | **1.92 s** | **4.9 MB** | 407% (Taskflow multi-core) |
| Haystack | 6.72 s | 78.3 MB | 100% (GIL) |
| pydantic-graph | 8.05 s | 34.9 MB | 100% (GIL) |
| LangGraph | 35.64 s | 58.9 MB | 100% (GIL) |
| LlamaIndex Workflow | 85.77 s | 101.5 MB | 100% (GIL) |
| AutoGen GraphFlow | 138.79 s | 52.4 MB | 100% (GIL) |

## What the numbers mean

1. **Per-run engine overhead spans ~7× to ~150× across the Python
   field.** Haystack is the leanest competitor (DAG with typed
   sockets, minimal runtime). At the other end, AutoGen and LlamaIndex
   are async/event-driven with per-run state setup that dominates when
   there's no real work to amortize over.
2. **The gap narrows on `par`.** Every Python framework pays fixed
   setup cost per run; once the graph is richer, per-node overhead is
   a smaller share. NeoGraph's lead shrinks from 7–156× to 2–49×.
3. **Memory footprint favors NeoGraph by an order of magnitude or
   more.** 4.9 MB (NeoGraph) vs. 35–101 MB across the Python field.
   On SBC-class targets (Raspberry Pi-class RAM) this is the
   load-bearing metric — the difference between "runs comfortably" and
   "run it carefully".
4. **Fan-out is actually parallel.** NeoGraph's Taskflow backend hit
   400%+ CPU on a 5-way fan-out; every Python framework sits at 100%
   because the interpreter's GIL serializes user nodes. For I/O-free
   workers this matters; for LLM-heavy workers where I/O releases the
   GIL, it matters less.

## Caveats — what this bench does NOT measure

* **Real agent workloads.** LLM-dominated pipelines are bottlenecked
  by provider latency (100ms–10s per call). Engine overhead disappears
  at that scale. Mental model: NeoGraph costs ~20µs/call, Haystack
  ~150µs, LangGraph ~650µs, LlamaIndex/AutoGen ~2–8ms — all invisible
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

# Build + run the C++ bench:
g++ -std=c++20 -O2 -DNDEBUG \
    -Iinclude -Ideps -Ideps/yyjson \
    benchmarks/bench_neograph.cpp \
    build/libneograph_core.a build/libyyjson.a \
    -pthread -o /tmp/bench_neograph
/tmp/bench_neograph 20000 10000

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
