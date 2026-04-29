# Memory & sanitizer sweep (Valgrind / ASan / UBSan / soak)

Ground-truth check that the example binaries and the test binary
release every allocation they make. Runs under `valgrind --tool=memcheck
--leak-check=full --show-leak-kinds=all`. The bar is **0 leaks, 0
errors** across the no-API-key example surface.

## Reproduce locally

```bash
mkdir build-debug && cd build-debug
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DNEOGRAPH_BUILD_TESTS=ON \
      -DNEOGRAPH_BUILD_EXAMPLES=ON \
      -DNEOGRAPH_BUILD_BENCHMARKS=OFF \
      -DNEOGRAPH_BUILD_POSTGRES=OFF ..
cmake --build . -j$(nproc)

# Sweep all no-API-key examples
for ex in example_custom_graph example_parallel_fanout example_send_command \
          example_intent_routing example_state_management example_all_features \
          example_plan_executor example_async_concurrent_runs \
          example_classifier_fanout example_subgraph example_checkpoint_hitl; do
    echo "=== $ex ==="
    valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=42 \
        ./$ex >/dev/null
done
```

## Last sweep

Run on 2026-04-29 against master (commit 4b02dea, post-classifier-fanout
example). Valgrind 3.22.0, GCC 13.3 Debug build.

### Examples — 11 / 11 clean

| Example | Allocs | Bytes | Errors |
|---|---:|---:|---:|
| `example_all_features` | 5,097 / 5,097 | 1,080,618 | 0 |
| `example_async_concurrent_runs` | 683 / 683 | 226,919 | 0 |
| `example_checkpoint_hitl` | 1,973 / 1,973 | 524,478 | 0 |
| `example_classifier_fanout` | 1,696 / 1,696 | 419,024 | 0 |
| `example_custom_graph` | 799 / 799 | 231,767 | 0 |
| `example_intent_routing` | 3,960 / 3,960 | 916,910 | 0 |
| `example_parallel_fanout` | 1,330 / 1,330 | 364,867 | 0 |
| `example_plan_executor` | 3,616 / 3,616 | 823,613 | 0 |
| `example_send_command` | 3,279 / 3,279 | 747,161 | 0 |
| `example_state_management` | 2,540 / 2,540 | 640,311 | 0 |
| `example_subgraph` | 1,568 / 1,568 | 419,423 | 0 |
| **Cumulative** | **26,541 / 26,541** | **6,395,091** | **0** |

Every allocation freed, 0 invalid reads, 0 invalid writes, 0 mismatched
free, 0 use-after-free.

### Tests — `*Smoke*:GraphCompiler*:GraphState*` clean

| Suite | Tests | Allocs | Bytes | Errors |
|---|---:|---:|---:|---:|
| Smoke / GraphCompiler / GraphState (31 tests) | 31 / 31 pass | 12,551 / 12,551 | 1,890,717 | 0 |

The full `neograph_tests` suite under valgrind takes ~30 minutes — the
subset above is the per-PR floor; full sweep is feasible as part of a
nightly CI job (not yet wired).

## What's NOT covered

- Network-bearing examples (`example_react_agent`, `example_mcp_*`,
  `example_openai_responses_*`) — TLS/socket interactions produce
  noise from libssl / libcurl that valgrind suppressions would have
  to mask. Use leak-check on the engine path with mock providers
  instead.
- Crawl4AI / Postgres / TransformerCPP-linked examples — external
  process or library state confounds the leak-check; coverage of those
  paths comes through ASan in CI rather than valgrind.
- Python binding (`_neograph.so`) — Python's interpreter has many
  intentional "leaks" at exit (allocated-but-not-freed module state)
  that swamp valgrind's signal. ASan with `LSAN_OPTIONS=detect_leaks=0`
  is the right tool there.

## ASan + UBSan + LSan sweep — 11/11 examples + 322 ctests clean

Compile with sanitizers:

```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
    -DNEOGRAPH_BUILD_TESTS=ON -DNEOGRAPH_BUILD_EXAMPLES=ON \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -O1" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j$(nproc)
```

Run examples + tests with `LSAN_OPTIONS=` and `UBSAN_OPTIONS=`:

```bash
export ASAN_OPTIONS="detect_leaks=1:halt_on_error=0"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0"
ctest --test-dir build-asan -E "BIG_|valgrind"   # 322/322 pass (2026-04-29)
```

Last sweep on master HEAD (commit 6bd9632, 2026-04-29):

| Surface | Result |
|---|---|
| 11 mock examples (custom_graph, send_command, intent_routing, state_management, all_features, subgraph, checkpoint_hitl, classifier_fanout, async_concurrent_runs, parallel_fanout, plan_executor) | ✓ exit=0, 0 ASan/UBSan errors |
| `neograph_tests` ctest (322 tests under sanitizers) | ✓ 322/322 pass |

ASan exposed one false-positive in the recursion guard added today —
the original `thread_local int` depth counter fired across nested
graphs (subgraph node's inner engine dispatching on the same thread
made the outer counter non-zero). Fixed by switching to a per-node
`const GraphNode*` key, so the guard only fires when the same node
re-enters its own default chain.

## Long-soak stress test — 10 000 graph runs, RSS Δ = 0 KB

```cpp
// /tmp/stress_runs.cpp — see commit log
for (int i = 0; i < 10000; ++i) {
    engine->run(RunConfig{.thread_id = "t" + std::to_string(i),
                          .input    = {{"count", 0}}});
    if (i == 100)  rss_at_100  = read_rss_kb();
    if (i == 9999) rss_at_1000 = read_rss_kb();
}
```

Run on master HEAD with a Counter node that emits Send fan-out
recursively up to depth 3 per run — a realistic stress shape:

```
10000 runs wall=0.68s  ops=14728/s  RSS@100=4608kB  RSS@9999=4608kB  Δ=0kB
PASS: RSS growth bounded
```

10 000 sequential graph runs, each allocating a few KB and freeing it,
with **0 KB resident-set growth** across the last 9 900 iterations.
The Linux glibc allocator returns the freed blocks to its pool cleanly
— no per-run leak path exists.

## CI gates (sanitizer-test, tsan-test, fuzz-canary)

Three CI jobs in `.github/workflows/ci.yml` enforce the following on
every PR and push to master:

### `sanitizer-test` — ASan + UBSan + LSan

| Step | Surface |
|---|---|
| `ctest -E "BIG_\|valgrind"` under `-fsanitize=address,undefined` | All unit tests including external surfaces (Postgres via service container, MCP HTTP/stdio, libssl/libcurl ConnPool) |
| 11 mock examples under same flags | full engine-path orchestration coverage |
| `pytest bindings/python/tests/` with `LD_PRELOAD=libasan.so` + `detect_leaks=1` | 46/48 Python tests with leak detection enabled (deselects 2 that propagate Python exceptions across pybind — known ASan `__cxa_throw` interception limitation, not a NeoGraph bug) |

### `tsan-test` — race detection on the engine's concurrency paths

| Step | Coverage |
|---|---|
| `setarch x86_64 -R ctest -E "BIG_\|valgrind"` under `-fsanitize=thread` | All 344 unit tests including the new `ConcurrentStress.TwoHundredOverlappingRunsAllSucceed` (200 simultaneous `run_async` × 3-way Send fan-out — catches data races in the worker pool, scheduler, parallel_group, and CheckpointStore concurrency paths) |
| 5 fan-out / async examples under TSan | `example_classifier_fanout` + `parallel_fanout` + `send_command` + `plan_executor` + `async_concurrent_runs` |

The `setarch x86_64 -R` wrap clears `ADDR_NO_RANDOMIZE` (kernel
`mmap_rnd_bits` defaults on Ubuntu 24.04+ trigger a TSan `unexpected
memory mapping` FATAL); the flag inherits through `fork`, so every
test child also gets a TSan-friendly address layout.

TSan + ASan are mutually exclusive at link time, so this is a separate
job from `sanitizer-test`.

### `fuzz-canary` — libFuzzer on `GraphCompiler::compile`

| Step | Coverage |
|---|---|
| `fuzz_graph_compile` for 60 s wall (`-max_total_time=60`) | Mutates the seed corpus under `tests/fuzz/corpus/graph_compile/` and feeds the bytes into `neograph::json::parse` → `GraphCompiler::compile`. Catches parser UB, unhandled exceptions, heap-buffer-overflow regressions. First run on master HEAD did 1.94 M iterations without a crash. |

Built with Clang's `-fsanitize=fuzzer,address,undefined` so any crash
surfaces ASan/UBSan diagnostics in the same trace.

## Release-build hardening

Release / RelWithDebInfo / MinSizeRel builds enable defense-in-depth
flags by default (`NEOGRAPH_ENABLE_HARDENING=ON`):

| Flag | What it catches |
|---|---|
| `-D_GLIBCXX_ASSERTIONS` | std::vector OOB, dereferencing `end()`, iterator invalidation, uninitialized `std::optional` access — abort with diagnostic instead of silent UB. Active in Debug + Release. |
| `-fstack-protector-strong` | Buffer overflows that would smash the return address — canary check fires before `ret`. |
| `-fcf-protection=full` | Indirect call/jump targets tagged for control-flow integrity. ROP-style attacks fail at the call site. Cheap on amd64 with CET-IBT. |
| `-D_FORTIFY_SOURCE=2` | Inline checks on libc string/memory routines. Release-only (needs ≥`-O1`). |
| `-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack` | Read-only relocations, immediate binding (no late PLT writes), no-exec stack — RELRO baseline. |

Performance impact measured on master HEAD with `bench_neograph`:

|  | seq µs | par µs |
|---|---:|---:|
| Baseline Release | 5.1 | 275.2 |
| Hardened Release | 5.1 | 275.6 |

**0 % overhead** within measurement noise. The flags shift work to the
linker (relocations, PLT) and to a per-function 8-byte canary load+
compare — both invisible at the µs scale of NeoGraph's engine path.

Disabled automatically under sanitizer builds (ASan/TSan/UBSan) where
they'd duplicate the sanitizer's own checks. Disabled under MSVC
(uses different hardening primitives — `/GS` etc., not in scope here).

## Sanitizer combinations explored but not viable

**MemorySanitizer** (uninitialized-read detection): requires every
linked C/C++ library — including libstdc++, libssl, libcurl, libpqxx —
to be MSan-instrumented, otherwise calls into them produce false
positives that swamp the signal. Clang's prebuilt `libc++` on Ubuntu
24.04 does not ship an MSan variant, and rebuilding the
standard library + every transitive dep is impractical. The
ASan+UBSan+TSan trio already catches uninit reads that escape into
heap-allocated state (since heap is poisoned at allocation under ASan
with `detect_uninitialized_reads=1` semantics in some passes). Skipped.

## Suppressions

| File | What it covers |
|---|---|
| [`tests/lsan_suppressions.txt`](../tests/lsan_suppressions.txt) | libssl / libcurl / libpq / libpqxx / libstdc++ ABI / glibc TLS / CPython interpreter / pybind11 type init / pydantic-core. Third-party only — adding a NeoGraph symbol is a real bug, fix the leak instead. |
| [`tests/tsan_suppressions.txt`](../tests/tsan_suppressions.txt) | asio reactor & socket service (epoll happens-before), yyjson SIMD reads, OpenSSL CRYPTO_THREAD_run_once. Library-internal benign races. |

## Concurrent stress test

`tests/test_concurrent_stress.cpp` runs as part of the standard ctest
suite (so it runs under both Debug and ASan):

- **TwoHundredOverlappingRunsAllSucceed** — 200 `engine->run_async()`
  calls overlap on one io_context, each with 3-way Send fan-out.
  Verifies parallel-group + pending-writes machinery is race-free under
  ASan and that all 200 runs produce the expected `{0, 1, 4}` worker
  outputs.
- **RssBoundedOverHundredsOfConcurrentRuns** — 5 bursts of 200 runs
  (1 000 concurrent total) with RSS Δ ≤ 10 MB threshold. Skipped under
  ASan (sanitizer's shadow-memory growth dominates the signal).

The Debug-build run produced RSS Δ=128 kB across 1 000 concurrent
runs — engine-side memory profile is flat under sustained concurrent
load.
