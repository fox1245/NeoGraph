# NeoGraph sustained-concurrency stress benchmark

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

This runner repeats a three-node counter graph over a wall-clock window. It measures local engine churn, not provider calls, persistence, or production readiness.

## Measurement and exit status

`bench_sustained_concurrent` defaults to `--concurrency 1000`, `--duration-s 60`, `--sample-s 5`, `--warmup-s 5`, and `--rss-tolerance-pct 25`. It creates a caller pool with one thread per target run; completions enqueue replacements. Samples report mean and maximum latency, not P99. Timing starts inside a caller worker and excludes its queue wait. `ok_total` counts calls that returned without a thrown exception, not validation of the returned graph state.

Exit 1 means final current RSS exceeds the recorded warm baseline by the tolerance; exit 0 does not prove leak freedom or absence of run errors. Read `err_total` separately. The final RSS is read after stopping and joining the pool, so thread teardown affects drift. The warm baseline is only captured on the first sample if it has reached `warmup-s`; keep warmup no longer than the first sample interval. A missing baseline yields zero drift and cannot qualify a memory gate.

Windows uses process working-set counters; Linux uses `/proc/self/status`. Other platforms can report zero because those counters are unavailable. Peak RSS is a high-water mark and never decreases; inspect current RSS and baseline validity when investigating growth.

## Build and run

Install the external SchemaProvider SDK and set its prefix. Core requires `SchemaProvider::runtime` even when LLM and the optional NeoGraph libcurl backend are disabled. An explicit SDK source checkout can replace the prefix using `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`; its build needs C++20, Python, libcurl ≥7.88, and OpenSSL Crypto. See the [build guide](../../README.md) for dependency and platform limits. This recipe disables network fetching and unused NeoGraph integrations.
For NeoGraph `0.13.0`, use alpha SDK `0.1.0`, interface revision/shared
generation 4, and rebuild with matching headers/libraries. Current integrated validation is pending.

```bash
# Set SCHEMAPROVIDER_PREFIX to the installed SDK prefix.
cmake -B build-stress -S . \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_BENCHMARKS=ON \
  -DNEOGRAPH_BUILD_TESTS=OFF -DNEOGRAPH_BUILD_EXAMPLES=OFF \
  -DNEOGRAPH_BUILD_PROGRAM=OFF -DNEOGRAPH_BUILD_LLM=OFF \
  -DNEOGRAPH_BUILD_ASYNC=OFF -DNEOGRAPH_BUILD_MCP=OFF \
  -DNEOGRAPH_BUILD_A2A=OFF -DNEOGRAPH_BUILD_ACP=OFF \
  -DNEOGRAPH_BUILD_UTIL=OFF -DNEOGRAPH_BUILD_POSTGRES=OFF \
  -DNEOGRAPH_BUILD_SQLITE=OFF -DNEOGRAPH_USE_LIBCURL=OFF
cmake --build build-stress --parallel --target bench_sustained_concurrent

./build-stress/bench_sustained_concurrent \
  --concurrency 1000 --duration-s 60 --sample-s 5 \
  --warmup-s 5 --rss-tolerance-pct 25
```

## Preserved legacy observation

The previous README reported an undated Ryzen 7 5800X run with concurrency=100 and duration=15 s: 15.3 M runs (about 1.0 M runs/s), mean latency about 55 µs, warm RSS 9.3 MB and final RSS 7.4 MB (about −20%, exit 0). Its date and SDK revision were not recorded here. It is retained as legacy evidence, not a new cutover qualification or a throughput guarantee. The output excerpt below belongs to that observation; the ellipsis is not JSON.

```json
{"sample":1,"elapsed_s":5,"window_ok":5012514,"err_total":0,"inflight":100,
 "mean_us":55.95,"max_us_window":189607,"rss_kb":9344,"peak_rss_kb":9472}
…
{"summary":true,"concurrency":100,"duration_s":15,"ok_total":15334628,
 "err_total":0,"rss_warm_kb":9344,"rss_final_kb":7448,"rss_peak_kb":9600,
 "rss_drift_pct":-20.29,"rss_tolerance_pct":25,"leak_suspect":false}
```

## Allocation-pressure experiment

`prlimit` limits Linux virtual address space. With a thread per caller slot, stacks and pool construction can exhaust that limit before graph execution. The per-run catch records exceptions from `engine->run`, but pool creation is outside that catch. Clean process exit under allocation pressure is an acceptance criterion to measure, not a guarantee from this script.

```bash
# Linux: cap virtual address space, not resident memory.
prlimit --as=$((256*1024*1024)) \
  ./build-stress/bench_sustained_concurrent \
  --concurrency 200 --duration-s 30
```

## Additional experiments

A 24-hour run or a cgroup memory cap needs a separately recorded environment and outcome. Compare current RSS across steady-state windows, record `err_total` and termination signals, and distinguish cgroup resident-memory enforcement from `prlimit` address-space limits. This page does not report those runs.
