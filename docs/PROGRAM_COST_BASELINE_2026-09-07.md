# Program cost baseline — 2026-09-07

The small-payload baseline points to Program bookkeeping and durable-store work as the first optimization targets. It does not establish JavaScript interpretation as the dominant cost, and it does not measure a JIT speedup.

## Environment and method

- Source base: `319f8748c986a2f345ae6de88abb42d89eef9837` plus the measurement-only additions recorded by the source/binary hashes in `metadata.json`.
- AMD Ryzen 7 5800X, WSL2 Ubuntu 24.04, 16 logical CPUs available; GCC 13.3, Release `-O3 -DNDEBUG`, repository hardening enabled.
- Binaries and SQLite databases on the WSL ext4 filesystem. PostgreSQL 16.15 runs in native WSL Docker with a local Docker volume, reached over localhost TCP. `fsync`, `synchronous_commit` and `full_page_writes` are on; WAL sync method is `fdatasync`.
- Seven fresh processes per case, ten warmup and forty measured invocations per lifecycle/direct process. One Runtime scheduler thread; no concurrent test jobs or model calls. The desktop/VM is not an isolated bare-metal benchmark host.
- 308 successful process samples, 44 cases. Every real invocation checked counter and payload output. Case order alternates forward/reverse by repetition.
- Headline values are medians of seven process medians. p95 below is the median of seven within-process nearest-rank p95s, not a statistically independent pooled p95.
- Raw metadata, cases, samples and summaries are retained locally in `artifacts/program-costs-20260907/`. SQLite databases remain under `/tmp/neograph-program-costs-20260907/` in WSL; database files are not committed.

See [measurement definitions and reproduction](PROGRAM_COST_MEASUREMENT.md). This diagnostic matrix does not replace the existing QuickJS acceptance gate.

## One Core call, empty payload

| Path | Median (ms) | Within-process p95 (ms) | Across-process MAD of medians (ms) |
| --- | --- | --- | --- |
| Core directly, no checkpoints | 0.0074 | 0.0076 | 0.0000 |
| C++ Program / memory | 2.0031 | 2.2607 | 0.0355 |
| JavaScript Program / memory | 3.3192 | 3.4898 | 0.0196 |
| C++ Program / SQLite | 8.5885 | 13.5360 | 0.1155 |
| JavaScript Program / SQLite | 16.4288 | 20.9731 | 0.0253 |
| C++ Program / PostgreSQL | 49.0355 | 51.2952 | 0.5896 |
| JavaScript Program / PostgreSQL | 80.1248 | 82.8138 | 0.5005 |

The direct Core row excludes Program lifecycle and checkpoint semantics. Its ratio to Program is a total-envelope comparison, not an equivalent-durability engine comparison or evidence of a regression. The C++ Program also has real journal/checkpoint work. The JS–C++ difference includes additional command journaling and conversion, not only the interpreter.

## Payload scaling

| Path | 0 bytes (ms) | 4 KiB (ms) | 64 KiB (ms) |
| --- | --- | --- | --- |
| Core directly | 0.0074 | 0.0079 | 0.1308 |
| C++ Program / memory | 2.0031 | 2.8567 | 12.2975 |
| JavaScript Program / memory | 3.3192 | 5.2922 | 30.8116 |
| JavaScript Program / SQLite | 16.4288 | 23.9060 | 130.6772 |
| JavaScript Program / PostgreSQL | 80.1248 | 93.5064 | 252.1259 |

Payload is carried through the real channel state and checked unchanged. It is not model output or simulated network delay.

## Generator and bridge costs

| Payload | Open (µs) | First command (µs) | Warm command round trip (µs) | Terminal next (µs) | Close (µs) |
| --- | --- | --- | --- | --- | --- |
| 0 | 330.34 | 45.65 | 26.66 | 5.73 | 35.85 |
| 4096 | 349.72 | 171.95 | 153.32 | 30.99 | 39.99 |
| 65536 | 811.60 | 2038.30 | 2088.19 | 423.18 | 41.01 |

These isolated calls use synthetic Core responses. Warm round-trip time includes native JSON serialization/parsing, host command creation and JS execution. It is not pure bytecode execution time and is not directly subtractable from a lifecycle row. Large payloads expose a material data-conversion cost.

## Cold preparation

| Mode/backend | Store open (ms) | Compile (ms) | Admit (ms) | Runtime create (ms) | First run (ms) |
| --- | --- | --- | --- | --- | --- |
| memory-cpp-0 | 0.0054 | 0.6768 | 1.0492 | 0.1405 | 4.4991 |
| memory-javascript-0 | 0.0054 | 1.2498 | 1.2180 | 0.1420 | 5.9273 |
| sqlite-javascript-0 | 20.0656 | 1.3064 | 2.9385 | 0.1705 | 25.2293 |
| postgres-javascript-0 | 169.2527 | 1.2941 | 5.9809 | 0.1714 | 98.6726 |

Compilation/admission occur once per process outside warm invocation timing. Generator open occurs again for each newly started JS Program. These cold fields omit some fixture setup and do not time live replacement or migration.

## Event-marker partition

For this table only, each cell is the mean across all measured invocations, so the four marker intervals add to the mean total (rounding aside). The span between first and last Core event includes checkpoint work; it is not pure Core CPU time.

| Backend (JavaScript, 0 bytes) | Before first Core event (ms) | Core event span (ms) | After last Core event (ms) | Terminal to wait return (ms) |
| --- | --- | --- | --- | --- |
| memory | 1.1931 | 0.4062 | 1.7080 | 0.0340 |
| sqlite | 7.0816 | 1.2735 | 8.7646 | 0.0353 |
| postgres | 29.8013 | 9.1486 | 41.4887 | 0.0357 |

## Several Core calls in one generator

| Backend | 1 call/run (ms) | 4 calls/run (ms) | 16 calls/run (ms) |
| --- | --- | --- | --- |
| memory | 3.319 | 9.041 | 31.596 |
| sqlite | 16.429 | 55.527 | 331.089 |
| postgres | 80.125 | 244.105 | 1034.104 |

These are sequential calls with real journals and checkpoints. Dividing by call count amortizes run startup/termination, but it does not isolate dispatch cost.

## Suspended generator memory and replay

| Suspended generators | Median RSS increase (MiB) |
| --- | --- |
| 1 | 1.285 |
| 32 | 6.309 |
| 128 | 21.934 |

| Synthetic recorded commands replayed | Fresh open + replay (ms) |
| --- | --- |
| 0 | 0.1276 |
| 10 | 0.5526 |
| 100 | 3.4148 |
| 1000 | 33.4788 |

Memory above is for independent suspended QuickJS runtimes, including bootstrap/allocator effects. It excludes complete agents, catalogs, Core engines and durable histories. Replay excludes ProgramRuntime scheduling and journal I/O; it is not process-loss recovery latency.

## Supplementary database API profile

After the baseline, 72 additional runs compared profiling on/off, one versus 21 invocations, three repetitions, and both control modes across all backends. The table uses `(21-run process total − 1-run process total) / 20`, then takes the median across repetitions. Call-count differences were identical in all three repetitions; timing differences remain estimates affected by cold-process variation.

| Path | SQLite step calls/run | SQLite exec calls/run | SQLite commits/run | Synchronous libpq calls/run |
| --- | --- | --- | --- | --- |
| memory-cpp | 0 | 0 | 0 | 0 |
| memory-javascript | 0 | 0 | 0 | 0 |
| sqlite-cpp | 73 | 14 | 7 | 0 |
| sqlite-javascript | 117 | 18 | 9 | 0 |
| postgres-cpp | 0 | 0 | 0 | 64 |
| postgres-javascript | 0 | 0 | 0 | 108 |

SQLite steps can repeat for rows and are not unique SQL statements. `exec` includes transaction control; nested steps are excluded. libpq counts/times cover only synchronous `PQexec`/`PQexecParams`, excluding the async checkpoint path and connection establishment. Client CPU excludes the PostgreSQL server. API wall intervals and client CPU overlap and must not be added together or treated as a full partition of baseline latency.

The memory controls recorded zero database calls. In the JS SQLite case, nine commits and 117 external step calls occur per invocation; CPU work outside those timed database calls still deserves profiling. The JS PostgreSQL case adds 108 synchronous calls per invocation, making round-trip count a concrete target before a JIT experiment.

Supplementary evidence is in `artifacts/program-costs-20260907/profile/`. The first profiler omitted SQLite `exec` transaction calls; its preliminary output was not used. The corrected profile was rerun after file copying stopped. However, later profiled **and unprofiled** runs were materially slower than the main baseline, including the memory controls. The cause of this environment drift was not isolated. Consequently only the stable call counts are qualified here; raw API and CPU times are retained but are **not used to attribute the main baseline latency**. Main baseline data predates the copy issue and retains all seven repetitions and their dispersion.

The existing `bench_program` also gave 2.21–2.31 ms for its C++ Program path in three fresh processes (Core: 6.41–6.65 µs). That fixture has one channel and different bounds, so it is a cross-check of the millisecond envelope, not a matched replacement for the main matrix.

## Next measurements and optimization order

1. Profile Program startup/termination and publication serialization. Small-payload costs remain material with the in-memory stores and with C++ control, so replacing the JS engine alone cannot address the whole envelope.
2. Use the measured SQLite/libpq call counts to investigate redundant reads and round trips where the same journal, owner, authority, budget and atomicity guarantees can be preserved. Do not obtain a better number by disabling durable commits.
3. Investigate repeated canonical JSON conversion and copying using the payload sweep. Benchmark compiled-source reuse separately from warm generator execution.
4. Compare a JIT backend only after isolating genuine JS CPU work, including cold-start cost and memory at realistic agent counts.

Full CPU attribution, concurrent multi-tenant throughput/fairness, recursive spawn, live replacement and an actual JIT comparison remain unmeasured. No production runtime optimization was made for this baseline.
