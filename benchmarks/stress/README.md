# NeoGraph stress harnesses

Operational-readiness gates that complement the engine-overhead and
single-shot concurrent benchmarks. Where `benchmarks/bench_neograph`
measures **per-call cost** and `benchmarks/concurrent/...` measures the
**single 10k burst**, this directory exercises NeoGraph **over time**.

## What's here

### `bench_sustained_concurrent`

Holds N graph runs in flight for M wall-clock seconds. Submits a new
run as soon as one completes, so inflight stays at the target. Samples
RSS and per-window latency every `--sample-s` seconds; exits 1 if RSS
drifts upward by more than `--rss-tolerance-pct` between the warm
baseline (after warmup) and the final sample.

Catches three failure modes the burst bench can't:

- **Steady-state leaks** — coroutines / pending writes / cached
  state that grow without bound. The drift gate is best-effort
  (Valgrind / LSan stays the authoritative tool, see ASan / TSan CI),
  but a 25% RSS climb over 60 s is a strong "look at this" signal.
- **Latency drift** — mean / max-per-window walks up after the
  pool warms. Often points at thread-pool starvation or scheduler
  back-pressure that doesn't show in t=0 burst tests.
- **Pool exhaustion under churn** — completions overlap with new
  submissions, so the worker pool sees a mixed inflight pattern,
  not the all-drain of a burst.

#### Usage

```bash
cmake -B build-stress -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DNEOGRAPH_BUILD_BENCHMARKS=ON \
    -DNEOGRAPH_BUILD_TESTS=OFF \
    -DNEOGRAPH_BUILD_EXAMPLES=OFF
cmake --build build-stress -j$(nproc) --target bench_sustained_concurrent

./build-stress/bench_sustained_concurrent \
    --concurrency        1000 \
    --duration-s         60   \
    --sample-s           5    \
    --warmup-s           5    \
    --rss-tolerance-pct  25
```

Smoke result (concurrency=100, duration=15s, on Ryzen 7 5800X):
- 15.3 M graph runs / 15 s ≈ **1.0 M runs/s** sustained
- mean latency per run: ~55 µs
- RSS warm: 9.3 MB → final: 7.4 MB (drift ‑20 %, exit 0)

#### Output shape

One JSON line per sample, one final summary line:

```json
{"sample":1,"elapsed_s":5,"window_ok":5012514,"err_total":0,"inflight":100,
 "mean_us":55.95,"max_us_window":189607,"rss_kb":9344,"peak_rss_kb":9472}
…
{"summary":true,"concurrency":100,"duration_s":15,"ok_total":15334628,
 "err_total":0,"rss_warm_kb":9344,"rss_final_kb":7448,"rss_peak_kb":9600,
 "rss_drift_pct":-20.29,"rss_tolerance_pct":25,"leak_suspect":false}
```

### `bench_sustained_concurrent` under `prlimit` (memory cap test)

Wrap the harness in a virtual-memory cap to prove NeoGraph handles
allocation pressure cleanly:

```bash
# Cap address space at 256 MB. Allocations beyond this fail with
# std::bad_alloc — NeoGraph's audit-Round-5 typed catch in
# graph_executor (commit ead703e) rethrows bad_alloc instead of
# silently retrying, so the workload should error out instead of
# crashing.
prlimit --as=$((256*1024*1024)) \
    ./build-stress/bench_sustained_concurrent \
        --concurrency 200 --duration-s 30
```

Pass criteria: process exits cleanly (return code 0 or 1; not SIGABRT
/ SIGSEGV), `err_total` may be non-zero (those are the bad_alloc
rethrows surfacing as failed runs).

## Not yet here

- **24-hour soak** — same harness, longer wall window. Run it on a
  dedicated host; watch `peak_rss_kb` for monotone-non-decreasing
  trend over hours.
- **cgroup-bounded run** — `systemd-run --scope -p MemoryMax=512M`
  for a stricter resource cap than `prlimit` (kernel-side
  enforcement, not just allocation-time check). WSL2 systemd
  support is limited; test on a real Linux host.
