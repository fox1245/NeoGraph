# Performance deep-dive

**Languages:** [English](performance-deep-dive.md) | [한국어](performance-deep-dive.ko.md) | [日本語](performance-deep-dive.ja.md) | [简体中文](performance-deep-dive.zh-CN.md)

## Reading the historical measurements

The tables below preserve pre-cutover measurements. They do not qualify the current typed provider, its external runtime, or the rebuilt Python wheel. Record the revision, compiler, configuration, topology, concurrency, endpoint and timing boundary for any new comparison. Current reproduction guidance is in [benchmarks](../benchmarks/README.md).

These measurements give no cloud costs, worker counts or capacity guarantees. Pin wheel hashes and native dependencies as well as package versions; a version string alone does not make a deployment reproducible.

## Engine-only workloads

The current cross-framework tables (the matched and idiomatic regimes, memory, start-up and size) are generated from a checked-in record in [the benchmark guide](../benchmarks/README.md). The April 2026 tables that this page used to repeat were measured in several sessions with mixed versions; they are kept, with a note on what is known to be invalid, in [the history](../benchmarks/history/engine-overhead-2026-04.md). A zero-I/O engine comparison does not predict model-service latency.

## Burst concurrency

Ten thousand requests submitted together (a closed, single burst) inside a Docker cgroup with one CPU and 512 MB RAM. The wall-time column that the recorded data supports, and the columns and rows that were withdrawn with the reason for each, are in [the concurrent benchmark](../benchmarks/concurrent/CONCURRENT.md). These results belong to that workload and environment; they do not establish a universal Python/GIL scaling law.

## Historical cache simulation

Cachegrind simulated a Ryzen 7 5800X cache hierarchy: 32 KB L1 instruction/data caches, eight-way; 32 MB last-level cache, sixteen-way; 64-byte lines. The historical concurrent benchmark reported:

| N | Instruction references | Last-level instruction misses | Native p50 (service time in the worker) |
|---|---:|---:|---:|
| 1 | 5.3 M | 4313 | 17 µs |
| 10 | 5.9 M | 4304 | 16 µs |
| 100 | 11.8 M | 4320 | 6 µs |
| 1000 | 69.7 M | 4327 | 6 µs |
| 10000 | 648 M | 4329 | 5 µs |

Multiplying roughly 4330 misses by 64 bytes gives a 277 KB line-count estimate. Cachegrind's simulated misses do not by themselves prove a physical resident working set, DRAM stall time or the cause of a native latency change. See the [Cachegrind manual](https://valgrind.org/docs/manual/cg-manual.html).

## Historical local-model workloads

A separate neoclaw experiment used one RTX 4070 Ti and one Gemma 4 E2B Q4 GGUF (~1.5 GB weights), with a shared LocalProvider serializing inference and a one-node `llm_call` graph. The recorded wall time covers draining all workers; the listed request percentiles do not establish that queueing disappears.

| N | Wall s | Throughput rps | p50 ms | p99 ms | Peak RSS MB |
|---|---:|---:|---:|---:|---:|
| 1 | 0.64 | 1.6 | 642 | 642 | 2464 |
| 10 | 0.94 | 10.6 | 184 | 686 | 2529 |
| 100 | 4.81 | 20.8 | 343 | 855 | 2549 |
| 1000 | 44.1 | 22.7 | 347 | 673 | 2564 |
| 5000 | 213.7 | 23.4 | 338 | 657 | 2570 |
| 10000 | 424 | 23.6 | 337 | 648 | 2572 |

Source: [neoclaw benchmark](https://github.com/fox1245/neoclaw/blob/main/benchmarks/bench_concurrent_workers_local_llm.cpp). Its CUDA benchmark configuration belongs to that repository, not NeoGraph's current provider build. Do not extrapolate the observed marginal memory to an unmeasured session ceiling.

A different historical experiment used a local Gemma-4 E2B Q4_K_M model (4.65 B parameters, 2.9 GB GGUF) behind an HTTP endpoint:

| Metric | Engine-only | Historical local Gemma HTTP |
|---|---:|---:|
| Last-level instruction misses | 4320 | 7262 |
| 64 × simulated LL instruction misses (historical derived value) | 277 KB | 465 KB |
| Agent RSS | 5.2 MB | 7.6 MB |
| TTFT | — | 25–27 ms |
| Total request time | — | 146–213 ms |
| Separate model-server RSS | — | 2.45 GB |
| RTX 4070 Ti VRAM | — | 3.06 GB |

It reported five requests in 1.58 s, a 2.65× overlap speedup. The old OpenAIProvider configuration and httplib transport accounting belong to that experiment. Current local-endpoint setup uses a validated descriptor and SDK runtime options; see [example 31](../examples/31_local_transformer.cpp). A separate inference process keeps model allocations outside the agent process, but does not guarantee cache residency.

## Historical size and cold start

The x86_64 Linux GCC 13 Plan & Executor demo used `-Os`, static libstdc++/libgcc, dead-section removal and stripping. It simulated model work with 120 ms sleeps, used an opt-in fan-out pool, failed one branch and resumed.

| Metric | Historical Plan & Executor demo |
|---|---:|
| Stripped MinSizeRel binary | 1203 KB |
| Peak RSS, crash and resume included | 2.9 MB |
| Cold start through both phases | ~720 ms |
| Recorded dynamic dependencies | libc.so.6 only |

These are historical artifact measurements. They do not prove a current libc-only deployment, sub-250 ms cold start, cross-compilation to musl, or capacity on a specific embedded board.

## Reproducing a current measurement

External `SchemaProvider::runtime` is required even with `NEOGRAPH_BUILD_LLM=OFF`. Set `SDK_PREFIX` to its installed prefix, or use `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` with the SDK's development dependencies. Use CMake targets for transitive linking; the old direct core/yyjson archive command omits the runtime. The following measures the current engine benchmark; it does not reproduce the historical concurrent sweep exactly.

NeoGraph requires CMake 3.20+. Without an explicit SDK source or an installed package, configuration fetches the revision-pinned public SDK archive by default. For an offline installed-SDK measurement, set `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` as well as the prefix. Fetching does not remove libcurl/OpenSSL development prerequisites.

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_BENCHMARKS=ON -DCMAKE_PREFIX_PATH="$SDK_PREFIX"
cmake --build build-release --target bench_neograph -j
./build-release/bench_neograph
valgrind --tool=cachegrind --cache-sim=yes \
  --I1=32768,8,64 --D1=32768,8,64 --LL=33554432,16,64 \
  ./build-release/bench_neograph
```

Inspect `ldd`/`otool` or the platform loader inventory on the resulting artifact rather than assuming the old dependency list. The performance of the typed Python bindings needs its own measurement against the rebuilt wheel.
