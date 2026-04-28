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

## Adding to CI

Wiring this as a GitHub Actions job: copy the loop above into a step
under `ci.yml`'s `build-and-test` job (after `make`), gate on
`runs-on: ubuntu-latest` (valgrind preinstalled), expect ~5–8 minutes
wall on CI hardware. Emits per-example pass/fail to the workflow log.

Not yet wired — the local sweep above is the current verification floor.
