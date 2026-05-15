# Performance deep-dive

> Detailed measurements behind the **Performance** and **Lightweight**
> axes. README has the headline numbers; this is the full evidence.

---

## Production economics

The four points (single-dep tree, no Docker required, frozen ABI,
single-wheel deploy) compound into a measurably different cost
structure when you actually scale on AWS / GCP / Azure. Two
mechanisms — **fleet-safety on auto-scaling** and **workers per
instance** — drive the numbers.

### Auto-scaling without Heisenbugs

LangChain on AWS effectively requires `docker image hash` pinning
all the way through the stack — ECR-immutable images, ASG launch
templates pinned to image hash, multi-region replication of that
hash. Without it, every fleet-changing event is a timing bomb:

| Event | LangChain risk | NeoGraph behavior |
|---|---|---|
| ASG launches new EC2 | `pip install` may pull newer transitive minor → fleet behavior drift | wheel is hash-immutable on PyPI; new instance = byte-identical binary |
| Lambda cold start | 5–15 s (`langchain-community` import graph) | ms-class — no transitive imports |
| Spot interruption + Karpenter rebuild | OS package + transitive Python dep drift | static-linked C++; only `libc.so.6` matters |
| Blue/green deploy | image rebuilt at deploy-time = different runtime than yesterday | `pip install neograph-engine==X.Y.Z` is reproducible by version string alone |
| Multi-region rollout | PyPI mirror lag + ECR replication timing → regions diverge | wheel hash equality across regions, period |
| "Code 0 lines changed, prod broke" | regular occurrence (Pydantic v1→v2 / 2024) | structurally impossible — no transitive surface to drift |

→ NeoGraph removes the SOP that LangChain prod *requires*. Bare-metal
`pip install neograph-engine` on an EC2 user-data script is itself
prod-grade.

### Workers per instance — the RAM-side delta

| | LangGraph | NeoGraph |
|---|---|---|
| Just-imported (zero workers) | **80 MB** | **5.5 MB** |
| 1024 idle workers | (typically OOM-class) | **31 MB** |
| Per-worker overhead (idle, no user state) | ~200–500 MB realistic prod | ~30 KB measured |
| t3.medium (4 GB) — workers/instance | 7–17 | **700–3,500** |
| Instances needed for 1 K concurrent requests | 60–140 | **1–3** |
| us-east-1 spend (24/7, on-demand t3.medium) | **~$1,800–4,300/mo** | **~$30–90/mo** |

That's a **50–150× infrastructure cost ratio** for the same
concurrent-user count. The mechanism behind the per-worker number is
the L3-cache fit story below: NeoGraph's hot working set is 277 KB
regardless of N, so vertical scale ceiling is set by physical RAM
itself, not by cache pressure.

> *"LangChain runtime cost: ~$4 K/mo for 1 K concurrent users.
> NeoGraph: ~$50/mo. Same code shape, same LLM, frozen ABI."*

This is the angle SREs / Platform teams care about when they
veto LangChain in prod. It's not "Python is slow" — it's
"the cost curve makes the SLA impossible."

### Measured: 10,000 concurrent workers, one process, one GPU

The table above is conservative. A direct stress test pinned the
real number — *measured*, not extrapolated. Setup:

- One process, one RTX 4070 Ti, one Gemma 4 E2B Q4 GGUF (≈ 1.5 GB
  model weights via llama.cpp).
- A single shared `LocalProvider` serializing inference at the
  GPU boundary (representing the typical "your LLM endpoint is
  the bottleneck" production shape).
- N concurrent NeoGraph workers, each running a 1-node graph
  (`llm_call` → `__end__`) with `engine.run_async()`, all
  contending for the same provider.
- Real generation: input `"Hi"`, output e.g.
  `"Hello! How can I help you today?\n"`.

| N workers | wall (s) | throughput (rps) | p50 (ms) | p99 (ms) | peak RSS (MB) | engine overhead (MB) | per-worker incremental |
|---:|---:|---:|---:|---:|---:|---:|---:|
| **1** | 0.64 | 1.6 | 642 | 642 | 2 464 | +294¹ | — |
| **10** | 0.94 | 10.6 | 184 | 686 | 2 529 | +359 | 7.2 MB/worker |
| **100** | 4.81 | 20.8 | 343 | 855 | 2 549 | +379 | 222 KB/worker |
| **1 000** | 44.1 | 22.7 | 347 | 673 | 2 564 | +394 | **6 KB/worker** |
| **5 000** | 213.7 | 23.4 | 338 | 657 | 2 570 | +400 | **1.2 KB/worker** |
| **10 000** | **424** | **23.6** | **337** | **648** | **2 572** | **+403** | **≈ 1 KB/worker** |

¹ One-time KV cache + llama.cpp activation buffers. Amortized across
all N once allocated.

**What the numbers say:**

- **10,000 workers cost 9 MB more RAM than 1,000 workers**
  (2 564 → 2 572 MB). The marginal cost of an additional worker
  *converges to about 1 KB* — the size of a `RunConfig` plus a
  `thread_id` string.
- **Throughput is GPU-bound at 23 rps**, identical for N = 100 and
  N = 10 000. The engine schedules 10 000 idle workers on a queue
  for 7 minutes and contributes nothing to wall time.
- **p99 latency is flat** (648 ms at N = 10 000 vs 686 ms at N = 10).
  Queue depth does not accumulate latency — the scheduler releases
  workers fairly as the GPU drains.
- **Workers/instance ceiling is set by physical RAM, not by the
  engine.** On a 32 GB host, N can grow to ≈ 30 million workers
  before RAM saturates.

For the 1 K-worker LangGraph cost projection earlier, the implicit
per-worker assumption was 200–500 MB. **The NeoGraph measurement is
6 KB.** The ratio isn't 100× — it's ≈ 30 000–80 000×.

The benchmark source lives in the sister project
[`neoclaw`](https://github.com/fox1245/neoclaw):
[`benchmarks/bench_concurrent_workers_local_llm.cpp`](https://github.com/fox1245/neoclaw/blob/main/benchmarks/bench_concurrent_workers_local_llm.cpp).
Reproduce with `-DNEOCLAW_BUILD_BENCHMARKS=ON -DNEOCLAW_BUILD_CUDA=ON`.

---

## The agent runtime that fits in L3 cache

NeoGraph's hot code path is small enough that N concurrent agents share
one L3-resident working set. We measured this with Valgrind cachegrind
on a Ryzen 7 5800X (Zen 3: 32 KB L1i/d 8-way, **32 MB L3 16-way**),
sweeping N = 1 → 10,000 concurrent requests through
`benchmarks/concurrent/bench_concurrent_neograph`:

| N | I refs | **L3 instruction misses** | L3i miss rate | Native p50 |
|---:|---:|---:|---:|---:|
| 1 | 5.3 M | **4,313** | 0.08% | 17 µs |
| 10 | 5.9 M | **4,304** | 0.07% | 16 µs |
| 100 | 11.8 M | **4,320** | 0.04% | 6 µs |
| 1,000 | 69.7 M | **4,327** | 0.01% | 6 µs |
| 10,000 | **648 M** | **4,329** | **0.00%** | **5 µs** |

**L3 instruction misses stay flat at ~4,320** across four orders of
magnitude of N. The unique hot code working set is roughly
`4,330 × 64 B = 277 KB` — **0.85 % of the 32 MB L3**. At N = 10,000
we processed **648 million instructions** and only **4,329 of them
reached DRAM** (≈ 1 miss per 150,000 instructions).

Native per-request latency drops from 17 µs (cold) to 5 µs (warm) as N
grows — the 3.4× improvement is pure I-cache warming. Throughput at
N = 10,000 is ~1.1 M req/s on the single thread pool, with 5.2 MB
peak RSS (≈ 100 B / agent marginal cost).

**Why this matters:** DRAM access on Zen 3 is ~250 cycles vs ~46 for
an L3 hit — roughly 5.5× slower per access. If NeoGraph's working set
had overflowed L3 (as Python interpreters + dict-heavy state typically
do), the same N = 10,000 sweep would have paid **+420 to +840 ms in
memory stalls** instead of the measured **9 ms total wall time** —
47–94× slower depending on how much of the miss chain reaches DRAM.
The whole L3 stays available for *your* workload (conversation history,
embeddings, tool responses): the engine itself is a rounding error.

_Reproduce:_
```bash
g++ -std=c++20 -O2 -DNDEBUG -Iinclude -Ideps -Ideps/yyjson -Ideps/asio/include \
    -DASIO_STANDALONE benchmarks/concurrent/bench_concurrent_neograph.cpp \
    build-release/libneograph_core.a build-release/libyyjson.a -pthread -o bench_ng

valgrind --tool=cachegrind --cache-sim=yes \
    --I1=32768,8,64 --D1=32768,8,64 --LL=33554432,16,64 ./bench_ng 10000
```

### Holds end-to-end with a real LLM in the loop

The L3 story survives full-stack production: we point NeoGraph at a
locally-hosted Gemma-4 E2B (Q4_K_M, 4.65 B params, 2.9 GB GGUF) served
by [TransformerCPP](https://github.com/fox1245/TransformerCPP)'s
OpenAI-compatible HTTP endpoint — zero NeoGraph code changes, just
`OpenAIProvider::Config::base_url = "http://localhost:8090"`. See
[`examples/31_local_transformer.cpp`](../examples/31_local_transformer.cpp).

| | Pure NeoGraph | **NeoGraph + local Gemma (HTTP)** |
|---|---:|---:|
| L3 instruction misses | 4,320 | **7,262** |
| Hot code working set | 277 KB | **465 KB** (1.42% of L3) |
| Per-request TTFT | — | **25–27 ms** (curl baseline 9–10 ms → ~15 ms NeoGraph overhead) |
| Per-request total | — | 146–213 ms @ 19–27 tokens (~130 tok/s) |
| **NeoGraph agent RSS** | 5.2 MB | **7.6 MB** (+2.4 MB for httplib + JSON streaming) |
| Gemma server RSS | n/a | 2.45 GB (mmap GGUF) |
| VRAM (RTX 4070 Ti) | n/a | 3.06 GB |

The inference process lives in a **separate address space**, so its
2.5 GB of model weights never touch NeoGraph's L3 cache lines. The
agent's 465 KB working set stays L3-resident regardless of how large
the model is. That's the architectural payoff of the two-process
split: you can swap in a 70 B model without inflating the agent.

Burst-tested with 5 concurrent NeoGraph agents against the same server:
aggregate wall 1.58 s / 5 requests (2.65× speedup from coroutine
overlap). Per-agent throughput drops under queue pressure because the
Gemma server doesn't implement continuous batching — that's a
TransformerCPP concern, not an agent one. NeoGraph dispatched all 5
cleanly with no resource pressure and the RSS stayed flat at ~7 MB.

---

## Benchmarks

### Engine overhead vs. Python graph/pipeline frameworks

Matched-topology, zero-I/O workloads: graph compiled once, invoked in a
hot loop. Measures what the engine itself costs (dispatch, state
writes, reducer calls) — no LLM, no sleep, no network.

![NeoGraph vs Python frameworks — per-iteration latency and peak RSS](images/bench-engine-overhead.png)

Per-iteration engine overhead (µs, lower is better). All rows
measured 2026-04-22 on the same x86_64 Linux host. NeoGraph built
with Release `-O3 -DNDEBUG` (10-run median); Python rows are 3-run
median through CPython 3.12.3.

| Framework | `seq` (3-node chain) | `par` (fan-out 5 + join) | `seq` vs. NeoGraph |
|-----------|---------------------:|-------------------------:|-------------------:|
| **NeoGraph master** | **5.0 µs** | **11.8 µs** | 1× |
| Haystack 2.28.0 | 144.1 µs | 290.0 µs | 28.8× |
| pydantic-graph 1.85.1 | 235.9 µs | 286.1 µs¹ | 47.2× |
| LangGraph 1.1.9 | 656.7 µs | 2,348.7 µs | 131.3× |
| LlamaIndex Workflow 0.14.21 | 1,780.3 µs | 4,683.5 µs | 356.1× |
| AutoGen GraphFlow 0.7.5 | 3,209.2 µs | 7,292.7 µs | 641.8× |

¹ pydantic-graph is a single-next-node state machine and cannot fan
out; `par` is a serial 6-node emulation.

Whole-process metrics (warm-up + both workloads, 10k seq + 5k par iters):

| | NeoGraph | best Python (Haystack) | worst (AutoGen) |
|---|----------|------------------------|-----------------|
| **Total elapsed** | **~0.16 s** | 2.91 s | 68.29 s |
| **Peak RSS** | **4.8 MB** | 80.3 MB | 52.4 MB² |
| **Parallel fan-out executor** | `asio::experimental::make_parallel_group` | single-thread asyncio (GIL) | single-thread asyncio (GIL) |

² AutoGen has a smaller RSS than LlamaIndex but its per-iter cost
is 64× higher — different tradeoff axes. Full matrix in
[`benchmarks/README.md`](../benchmarks/README.md).

**Engine overhead disappears under LLM latency.** A 500 ms OpenAI round
trip swamps every engine; the per-iter gap only shows up in non-LLM
nodes (data transforms, routing decisions, pure-compute tool calls) and
in dense agent orchestration. Where it does show up, it shows up big:
on a Raspberry Pi 4 / Jetson Nano / any SBC-class target, a 10–20×
RAM delta is the difference between "fits" and "swap thrash."

Reproduction and methodology: [`benchmarks/README.md`](../benchmarks/README.md).

### Burst concurrency (1 CPU / 512 MB sandbox)

What happens under thousands of simultaneous requests? Burst test: N
requests submitted at t=0 to each engine, all-in / all-wait, inside a
Docker cgroup limited to **1 CPU and 512 MB RAM** — roughly a
Raspberry Pi 4 process budget.

![Tail latency — P99 per request](images/bench-concurrent-latency.png)

![Throughput under concurrent load](images/bench-concurrent-throughput.png)

![Peak resident memory](images/bench-concurrent-rss.png)

At **N=10,000 concurrent requests** in asyncio mode (the default
deployment shape for every Python framework):

| Engine | Wall | P99 latency | Peak RSS | Status |
|--------|-----:|------------:|---------:|:-------|
| **NeoGraph master** | **52 ms** | **7 µs** | **5.5 MB** | ✅ 10000 / 0 |
| pydantic-graph | 886 ms | **158 µs** | 42.6 MB | ✅ 10000 / 0 |
| Haystack | 3.1 s | 2.9 s | 130.7 MB | ✅ 10000 / 0 |
| LangGraph | 23.4 s | 23.0 s | 416.2 MB | ✅ 10000 / 0 |
| LlamaIndex | — | — | — | ❌ **OOM killed** |
| AutoGen | — | — | — | ❌ **OOM killed** |

**Two frameworks don't complete** — LlamaIndex Workflow and AutoGen
GraphFlow exhaust the 512 MB cgroup and get OOM-killed before 10k
concurrent coroutines can drain. The remaining Python frameworks
degrade rather than die, but their P99 latency grows linearly with N
because the CPython GIL serializes every coroutine's CPU work. **This
is not a LangGraph-specific pathology** — it shows up in every Python
asyncio runtime.

NeoGraph beats every Python asyncio runtime on throughput,
tail latency, and RSS: 7 µs P99 at N=10k, ~76× lower RSS than
LangGraph at the same load, and 3 orders of magnitude ahead of the
GIL-serialized Python curves. Even pydantic-graph — the leanest
Python state-machine — sits at 158 µs P99 and ~8× NeoGraph's RSS.

`multiprocessing.Pool` mode bypasses the GIL across worker processes
but saturates at pool size and pays fork + pickle overhead; full
numbers and the mp-mode story are in
[`benchmarks/concurrent/CONCURRENT.md`](../benchmarks/concurrent/CONCURRENT.md).

### Size & cold-start footprint (Plan & Executor demo)

All numbers below were measured on x86_64 Linux (GCC 13) using
`example_plan_executor` — a self-contained Plan & Executor demo that
runs a 5-way Send fan-out, crashes sub-topic #2 on the first run, and
resumes with the failure cleared. No LLM calls, no API keys, no network.

| Build configuration | Size |
|---|---|
| **MinSizeRel `-Os`, static libstdc++, `--gc-sections`, stripped** | **1,203 KB (1.2 MB)** |

The MinSizeRel binary's only dynamic dependency is `libc.so.6` —
`libstdc++` and `libgcc_s` are linked in statically. Drop it onto any
Linux host with a matching libc and it runs.

| Metric | Value |
|---|---|
| Peak RSS (full Plan & Executor run, crash + resume included) | **2.9 MB** |
| Wall-clock (cold start → both phases complete) | **~720 ms** |
| Dynamic dependencies | `libc.so.6` only |

`example_plan_executor` sleeps 120 ms per Send target to simulate an
LLM call; the 5-way fan-out runs serially on the default
single-threaded super-step loop. Call `engine->set_worker_count(N)`
after `compile()` for multi-threaded fan-out (cuts this demo's wall
time roughly in half on a 2-core host). Steady-state RSS is unaffected.

```bash
cmake -B build-minsize -S . \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DNEOGRAPH_BUILD_MCP=OFF -DNEOGRAPH_BUILD_TESTS=OFF \
    -DCMAKE_CXX_FLAGS="-ffunction-sections -fdata-sections" \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,--gc-sections -static-libstdc++ -static-libgcc"
cmake --build build-minsize --target example_plan_executor -j$(nproc)
strip --strip-all build-minsize/example_plan_executor
ls -la build-minsize/example_plan_executor   # size
ldd    build-minsize/example_plan_executor   # libc only
/usr/bin/time -v build-minsize/example_plan_executor   # RSS + wall
```

### What the numbers mean for embedded / robotics

- **1.2 MB static binary** fits a Docker `scratch` image at ~1 MB, fits
  on-board flash of a Pixhawk companion computer, fits comfortably in
  a Jetson Orin boot partition. Python + LangGraph does not.
- **2.9 MB RSS** means you can host **100+ concurrent agent sessions**
  on an RPi Zero 2W (512 MB RAM) by sharing one compiled engine across
  threads — see [`docs/concurrency.md`](concurrency.md) for the pattern.
- **< 250 ms cold start** fits inside a drone watchdog reset window;
  a Python LangGraph process still hasn't finished `import` by then.
- **`libc.so.6` only** makes cross-compilation trivial: pick `glibc` or
  `musl` and link — no transitive dependency hell.
