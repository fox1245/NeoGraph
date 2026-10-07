# Concurrent-load benchmark: historical NeoGraph and Python results

**Languages:** [English](CONCURRENT.md) | [한국어](CONCURRENT.ko.md) | [日本語](CONCURRENT.ja.md) | [简体中文](CONCURRENT.zh-CN.md)

This page preserves the April 2026 counter-chain comparison. It has not been rerun with the current SchemaProvider SDK or Python bindings. “NeoGraph 3.0” is the recorded cohort label, not the current package version.

## Workload and timing

Three nodes increment an overwrite counter channel (`a → b → c`), with no model calls, sleep, network I/O, or checkpoint store. The matrix submits N ∈ {10, 100, 1000, 10000} under Docker profiles of 1 CPU / 512 MB and 2 CPU / 1 GB, with swap capped at the memory limit.

NeoGraph uses a caller-side `asio::thread_pool` sized to `max(hardware_concurrency(), 1)`. Per-request timing starts inside the worker: P50/P99 exclude caller-queue waiting. `total_wall_ms` includes submission and the full burst drain. Do not compare the microsecond P99 directly with an end-to-end server SLO.

The historical Python field used LangGraph 1.1.9, Haystack 2.27.0, pydantic-graph 1.84.1, LlamaIndex Workflow 0.14.20, and AutoGen GraphFlow 0.7.5 in asyncio and multiprocessing modes. These versions describe the old cohort, not current Docker dependency resolution.

## Historical results: 1 CPU / 512 MB

Charts and table preserve NeoGraph results reported on 2026-04-22 and Python results on 2026-04-19. The N=10,000 table is engine-only, not provider transport or inference. Missing metrics remain missing.

![Throughput — requests per second](../../docs/images/bench-concurrent-throughput.png)

![Tail latency — P99 per request](../../docs/images/bench-concurrent-latency.png)

![Peak resident memory](../../docs/images/bench-concurrent-rss.png)

| N | Engine + mode | Wall | P50 | P99 | Peak RSS | OK / Err |
|---|---------------|------|-----|-----|----------|---------|
| 10,000 | NeoGraph 3.0 (historical label) | 52 ms | 4 µs | 7 µs | 5.5 MB | 10000 / 0 |
| 10,000 | LangGraph asyncio | 23.4 s | 20.2 s | 23.0 s | 416.2 MB | 10000 / 0 |
| 10,000 | LangGraph mp-pool-7 | 8.0 s | 737 µs | 88.4 ms | 60.3 MB | 10000 / 0 |
| 10,000 | Haystack asyncio | 3.1 s | 1.7 s | 2.9 s | 130.7 MB | 10000 / 0 |
| 10,000 | Haystack mp-pool-7 | 2.9 s | 167 µs | 84.7 ms | 68.1 MB | 10000 / 0 |
| 10,000 | pydantic-graph asyncio | 886 ms | 71 µs | 158 µs | 42.6 MB | 10000 / 0 |
| 10,000 | pydantic-graph mp-pool-7 | 2.8 s | 253 µs | 83.8 ms | 36.7 MB | 10000 / 0 |
| 10,000 | LlamaIndex asyncio | OOM killed | — | — | — | — |
| 10,000 | LlamaIndex mp-pool-7 | 6.6 s | — | — | 102.5 MB | 0 / 10000 |
| 10,000 | AutoGen asyncio | OOM killed | — | — | — | — |
| 10,000 | AutoGen mp-pool-7 | 46.8 s | 4.6 ms | 97.1 ms | 49.1 MB | 10000 / 0 |

The full matrix is archived in [`results.jsonl`](results.jsonl). The recorded LlamaIndex and AutoGen asyncio cells were classified as OOM kills; all LlamaIndex multiprocessing invocations in this row failed. Those observations do not establish universal framework limits or diagnose every failure.

## Interpretation boundaries

GIL-enabled CPython serializes Python bytecode, but event-loop scheduling, framework work, process serialization, and worker count also affect throughput. This benchmark does not isolate one cause, establish a universal asyncio ceiling, or predict free-threaded Python performance.

Docker CPU quota need not change the cores visible to `hardware_concurrency()`. Record caller pool size and host topology. Peak RSS comes from Linux `/proc/self/status`; zero on an unsupported platform means unavailable measurement. Read multiprocessing memory numbers with each runner’s accounting scope. No 256 MB experiment, bare-metal projection, persistence comparison, or remote-LLM capacity guarantee follows from this table.

## Current dependencies and reproduction status

Core links external `SchemaProvider::runtime` even with `NEOGRAPH_BUILD_LLM=OFF` and `NEOGRAPH_USE_LIBCURL=OFF`. Supply an installed SDK using `CMAKE_PREFIX_PATH` or an explicit checkout using `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`; see the [build guide](../../README.md). SDK source builds require C++20, Python for configuration generation, libcurl ≥7.88, and OpenSSL Crypto. Current SDK qualification is Linux/POSIX; the old Docker results do not qualify other platforms.
NeoGraph `0.13.1` recipes require alpha SDK `0.1.1`, interface revision/shared
generation 4, with matching headers/libraries. Current integrated validation is pending;
the earlier Linux/POSIX qualification is not an SDK4 or new Docker pass.

The maintained NeoGraph Docker image installs curl/OpenSSL development dependencies and builds a private CMake consumer linked to `neograph::core`, inheriting its SDK dependency closure. SDK acquisition follows root CMake policy; it may need network access when no package or source checkout is supplied. Optional NeoGraph network modules are off, but SDK dependencies remain. No new Docker measurements are asserted here. The matrix truncates its output before building, so choose a new path. `status=ok` only means JSON was extracted; inspect `ok`, `err`, and exit status too.

```bash
# From the repository root. Docker builds use the root SDK acquisition policy.
docker build -t ng-concurrent -f benchmarks/concurrent/Dockerfile.neograph .
docker run --rm --cpus=1 --memory=512m --memory-swap=512m ng-concurrent 10000

# Full matrix; a NEW path preserves the archived results.jsonl.
bash benchmarks/concurrent/run_matrix.sh benchmarks/concurrent/results-new.jsonl

# Render the archived default results.jsonl, not the new output.
node benchmarks/render_concurrent.js
```

The following JSON illustrates field shape only, not an additional measured result.

```json
{"engine":"neograph","mode":"threadpool","concurrency":10000,
 "total_wall_ms":6,"p50_us":2,"p95_us":3,"p99_us":6,
 "ok":10000,"err":0,"peak_rss_kb":7808}
```
