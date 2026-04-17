# NeoGraph vs LangGraph — engine-overhead benchmark

Measures the per-invocation overhead of the two graph engines on
identically-shaped graphs with **no I/O, no sleep, no LLM calls**. The
numbers therefore reflect what the engine itself costs (node dispatch,
state channel writes, reducer calls) — not the latency of any simulated
work.

## Workloads

Both implementations define the exact same two graphs, compile once,
and invoke them in a hot loop.

| Id | Shape | State |
|----|-------|-------|
| `seq` | `__start__ → a → b → c → __end__` — 3-node chain | single `counter` channel, each node writes `counter+1` (overwrite reducer) |
| `par` | `__start__ → {w1..w5} → summarizer → __end__` — fan-out 5 then join | `results: list` (append reducer) + `count: int`; each worker appends its index, summarizer writes `len(results)` |

Checkpointing is disabled on both sides (NeoGraph: no `thread_id`;
LangGraph: no checkpointer).

## Results (2026-04-18)

```
workload    iters      neograph_ms   langgraph_ms   per-iter neo/lg   speedup
seq         20,000     413           12,906         20.65 / 645.30    31.2×
par         10,000     1,507         22,251         150.70 / 2225.12  14.8×
```

End-to-end process metrics (whole binary including warm-up + both runs):

| Metric | NeoGraph | LangGraph | Ratio |
|--------|----------|-----------|-------|
| Wall clock | **1.92 s** | 35.64 s | 18.6× |
| Peak RSS | **4.9 MB** | 58.9 MB | 12.0× |
| CPU utilization | 407% (Taskflow multi-core) | 100% (GIL-bound) | — |

## What the numbers mean

1. **Per-run engine overhead is ~20× lower.** For agent servers that
   spin up many short graph runs (think CLI tools, per-request agents,
   batch pipelines), this is the dominant cost. With an LLM call in the
   mix it's noise — LLM latency swamps everything — but for
   LLM-independent business logic inside a graph, it's the whole cost.
2. **Memory is ~12× lower.** On a Raspberry Pi 4 (1GB or 2GB RAM
   models) this is the difference between "runs comfortably" and "run
   it carefully". On SBC-class targets — the positioning NeoGraph is
   built for — this is the more load-bearing metric than wall-clock.
3. **Parallel fan-out is actually parallel.** NeoGraph's Taskflow
   backend hit 400% CPU on a 5-way fan-out; LangGraph stays at 100%
   because the Python interpreter's GIL serializes user nodes. For
   I/O-free workers this matters; for LLM-heavy workers where the I/O
   releases the GIL anyway, it matters less.

## Caveats — what this bench does NOT measure

* **Real agent workloads.** LLM-dominated pipelines are bottlenecked by
  provider latency (100ms–10s per call). Engine overhead disappears at
  that scale. The right mental model: "NeoGraph costs ~20µs/call,
  LangGraph ~650µs/call — both invisible next to a 500ms API round
  trip." This bench matters for non-LLM nodes and for dense agent
  orchestration.
* **Checkpoint throughput.** Enabling checkpointing on both sides would
  let serialization cost dominate; that's a different benchmark.
* **Cold start.** Both implementations include a small warm-up loop
  before measurement so first-invoke JIT / lazy-init costs don't skew
  per-iter numbers. Full-process numbers above include the Python
  interpreter boot (~200ms).
* **Fairness.** NeoGraph was built with `-O2 -DNDEBUG`. LangGraph is
  stock CPython 3.12 with LangGraph 1.1.7 — production-typical
  deployments.

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

# Set up LangGraph in a venv and run the Python bench:
python3 -m venv /tmp/bench_venv
/tmp/bench_venv/bin/pip install langgraph
/tmp/bench_venv/bin/python benchmarks/bench_langgraph.py 20000 10000

# Peak RSS: wrap either with `/usr/bin/time -v`.
```

Output format is `workload<TAB>iters<TAB>total_ms<TAB>per_iter_us` on
both sides so diffing them is trivial.

## Environment used for the 2026-04-18 numbers

```
OS: Linux 6.6.87.2-microsoft-standard-WSL2 (Ubuntu 24.04 userland)
CPU: host CPU (8 logical cores exposed to WSL)
Compiler: g++ 13.x, -std=c++20 -O2 -DNDEBUG
Python: 3.12.3 (system), langgraph 1.1.7
```

Numbers will vary on your hardware, but the ratios should be stable to
within ~20%.
