# Concurrent-load benchmark: historical NeoGraph and Python results

**Languages:** [English](CONCURRENT.md) | [한국어](CONCURRENT.ko.md) | [日本語](CONCURRENT.ja.md) | [简体中文](CONCURRENT.zh-CN.md)

This page preserves the April 2026 counter-chain comparison, reduced to what its recorded data supports. It has not been rerun with the current SchemaProvider SDK or Python bindings, and the harnesses in this directory have since been corrected (see [Withdrawn and corrected](#withdrawn-and-corrected)): a re-measurement with the corrected harness is pending. “NeoGraph 3.0” is the recorded cohort label, not the current package version.

## Workload and timing

Three nodes increment an overwrite counter channel (`a → b → c`), with no model calls, sleep, network I/O, or checkpoint store. The matrix submits N ∈ {10, 100, 1000, 10000} under Docker profiles of 1 CPU / 512 MB and 2 CPU / 1 GB, with swap capped at the memory limit.

NeoGraph uses a caller-side `asio::thread_pool` sized to `max(hardware_concurrency(), 1)`. The workload is a **closed, single burst**: all N runs are submitted at once and the burst is drained; nothing models arrivals over time. `Wall` is the time from the first submission to the last completion. The old harness recorded it in whole milliseconds, so the NeoGraph points below 20 ms are not used for any chart or ratio. The per-request columns of the old data are withdrawn.

The historical Python field used LangGraph 1.1.9, Haystack 2.27.0, pydantic-graph 1.84.1, LlamaIndex Workflow 0.14.20, and AutoGen GraphFlow 0.7.5 in asyncio and multiprocessing modes. These versions describe the old cohort, not current Docker dependency resolution.

## Historical results: 1 CPU / 512 MB

The table and the charts below are generated from the archived [`results.jsonl`](results.jsonl) by `scripts/render_bench_report.py` and keep only the wall-time column and the single-process memory column. NeoGraph results were reported on 2026-04-22 and Python results on 2026-04-19. The N=10,000 table is engine-only, not provider transport or inference.

![Burst throughput: requests completed per second of wall time, closed single burst](../../docs/images/bench-concurrent-throughput.svg)

![Peak resident memory of one process, closed single burst](../../docs/images/bench-concurrent-rss.svg)

<!-- bench-record:begin concurrent_wall -->
| N | Engine and mode | Wall time of the closed burst | Peak RSS, one process (MiB) | OK / Err |
| ---: | :--- | ---: | ---: | ---: |
| 10,000 | NeoGraph threadpool | 52 ms | 5.5 | 10000 / 0 |
| 10,000 | pydantic-graph asyncio | 886 ms | 42.6 | 10000 / 0 |
| 10,000 | pydantic-graph mp-pool-7 | 2.8 s | withdrawn: the process tree is not counted | 10000 / 0 |
| 10,000 | Haystack mp-pool-7 | 2.9 s | withdrawn: the process tree is not counted | 10000 / 0 |
| 10,000 | Haystack asyncio | 3.1 s | 130.7 | 10000 / 0 |
| 10,000 | LangGraph mp-pool-7 | 8.0 s | withdrawn: the process tree is not counted | 10000 / 0 |
| 10,000 | LangGraph asyncio | 23.4 s | 416.2 | 10000 / 0 |
| 10,000 | AutoGen mp-pool-7 | 46.8 s | withdrawn: the process tree is not counted | 10000 / 0 |
| 10,000 | LlamaIndex asyncio | OOM killed | — | — |
| 10,000 | AutoGen asyncio | OOM killed | — | — |
<!-- bench-record:end concurrent_wall -->

The full matrix is archived in [`results.jsonl`](results.jsonl). The recorded LlamaIndex and AutoGen asyncio cells were classified as OOM kills: ten thousand simultaneous per-call instances did not fit in 512 MB. Those observations do not establish universal framework limits. The LangGraph asyncio row calls `ainvoke` with a plain `def` node, which LangGraph runs on a thread pool; with `async def` nodes it is a different regime (see the [engine-overhead guide](../README.md)).

## Withdrawn and corrected

Withdrawn from this page, with the reason for each:

* **The P50 and P99 columns and the latency chart.** They put the in-worker service time of NeoGraph, of the multiprocessing rows and of pydantic-graph (whose runs never suspend) next to the time in the system, from task creation, of the other asyncio rows, so the gaps measured how often a framework yields, not how fast it is.
* **The LlamaIndex multiprocessing rows.** Every call failed (0 of 10,000 completed) because the harness created the workflow handler outside a running event loop; the 6.6 s and the memory figure belonged to the exception.
* **The multiprocessing memory figures.** The harness counted the parent process only, not the worker processes.
* **The NeoGraph throughput points at N=10 and N=100.** The wall time was truncated to whole milliseconds and the renderer clamped it to 1 ms, so those points were not measured.

The harnesses in this directory now record, for every cell, the completions, the failures and a check of each result; a cell with a failure has status `error`, no percentiles and a non-zero exit. They record the service time and the response time from one common submit instant, wall time in microseconds, and a process-tree memory peak for multiprocessing. The Docker matrix has not been re-run with them: re-measurement is pending. The Docker images now pin the latest releases of [`requirements-compare.txt`](../requirements-compare.txt); the earlier LangGraph pin, 1.1.7, has been yanked from PyPI.

## Interpretation boundaries

GIL-enabled CPython serializes Python bytecode, but event-loop scheduling, framework work, process serialization, and worker count also affect throughput. This benchmark does not isolate one cause, establish a universal asyncio ceiling, or predict free-threaded Python performance.

Docker CPU quota need not change the cores visible to `hardware_concurrency()`. Record caller pool size and host topology. Peak RSS comes from Linux `/proc/self/status` of one process; zero on an unsupported platform means unavailable measurement. NeoGraph holds at most one run per pool thread while the Python asyncio rows hold all N runs in flight, so the memory columns are not like-for-like. No 256 MB experiment, bare-metal projection, persistence comparison, or remote-LLM capacity guarantee follows from this table.

## Current dependencies and reproduction status

Core links external `SchemaProvider::runtime` even with `NEOGRAPH_BUILD_LLM=OFF` and `NEOGRAPH_USE_LIBCURL=OFF`. Supply an installed SDK using `CMAKE_PREFIX_PATH` or an explicit checkout using `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`; see the [build guide](../../README.md). SDK source builds require C++20, Python for configuration generation, libcurl ≥7.88, and OpenSSL Crypto. The SDK has been tested on Linux/POSIX only; the old Docker results do not apply to other platforms.
NeoGraph `0.16.0` recipes require alpha SDK `0.4.0`, interface revision/shared
generation 7, with matching headers/libraries.

The maintained NeoGraph Docker image installs curl/OpenSSL development dependencies and builds a private CMake consumer linked to `neograph::core`, inheriting its SDK dependency closure. SDK acquisition follows root CMake policy; it may need network access when no package or source checkout is supplied. Optional NeoGraph network modules are off, but SDK dependencies remain. The matrix truncates its output before building, so choose a new path. `status=ok` only means JSON was extracted; inspect `ok`, `err`, and exit status too.

```bash
# From the repository root. Docker builds use the root SDK acquisition policy.
docker build -t ng-concurrent -f benchmarks/concurrent/Dockerfile.neograph .
docker run --rm --cpus=1 --memory=512m --memory-swap=512m ng-concurrent 10000

# Full matrix; a NEW path preserves the archived results.jsonl.
bash benchmarks/concurrent/run_matrix.sh benchmarks/concurrent/results-new.jsonl

# Render the tables and charts from the archived results.jsonl, not from the new output.
python3 scripts/render_bench_report.py
```

The following JSON illustrates field shape only, not an additional measured result.

```json
{"engine":"neograph","mode":"threadpool","concurrency":10000,
 "total_wall_ms":6,"p50_us":2,"p95_us":3,"p99_us":6,
 "ok":10000,"err":0,"peak_rss_kb":7808}
```
