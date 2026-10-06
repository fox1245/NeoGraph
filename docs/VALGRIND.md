# Memory and sanitizer checks

**Languages:** [English](VALGRIND.md) | [한국어](VALGRIND.ko.md) | [日本語](VALGRIND.ja.md) | [简体中文](VALGRIND.zh-CN.md)

## Scope and current prerequisites

Use Memcheck to find invalid memory accesses and allocation leaks, ASan for address errors, UBSan for covered undefined behavior, and TSan for data races. Passing one tool does not prove the properties checked by another. Reports below are dated historical results, not a new typed-provider qualification.

Current Core builds require external `SchemaProvider::runtime`, even when LLM nodes and PostgreSQL are disabled. Set `SDK_PREFIX` to a matching installed SDK; alternatively supply `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` and its development dependencies. To inspect SDK internals with sanitizers, build that SDK with matching instrumentation (`SP_SANITIZE=address` or `thread`). Recorded interface-3 runtime/archive qualification covers Linux/POSIX, not interface 4; these Linux commands do not qualify Windows, macOS or WASM.

NeoGraph requires CMake 3.20+. Source selection prefers an explicit SDK source directory, then an installed package, then a revision-pinned public archive fetched by default. Set `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` when using an installed SDK offline. Source/fetched builds use NeoGraph's Asio/yyjson; system libcurl/OpenSSL development files remain required.

## Run a current Memcheck scenario

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug \
  -DNEOGRAPH_BUILD_TESTS=ON -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_POSTGRES=OFF -DCMAKE_PREFIX_PATH="$SDK_PREFIX"
cmake --build build-debug --target example_custom_graph -j
valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all \
  --error-exitcode=42 ./build-debug/example_custom_graph
```

Run the relevant no-network examples and provider lifetime/cancellation scenarios on the rebuilt artifacts. Network and Python paths need scoped diagnostics and reviewed third-party suppressions; they are not exempt from memory checks. A mock-provider pass cannot qualify the SDK transport or authentic native replay custody.

## Historical Memcheck sweep, 2026-04-29

The recorded sweep used master commit `4b02dea`, GCC 13.3 Debug and Valgrind 3.22.0. Eleven no-key examples recorded zero invalid accesses, mismatched frees and leaks:

| Example at 4b02dea | Allocations / frees | Allocated bytes | Errors |
|---|---:|---:|---:|
| example_all_features | 5097 / 5097 | 1080618 | 0 |
| example_async_concurrent_runs | 683 / 683 | 226919 | 0 |
| example_checkpoint_hitl | 1973 / 1973 | 524478 | 0 |
| example_classifier_fanout | 1696 / 1696 | 419024 | 0 |
| example_custom_graph | 799 / 799 | 231767 | 0 |
| example_intent_routing | 3960 / 3960 | 916910 | 0 |
| example_parallel_fanout | 1330 / 1330 | 364867 | 0 |
| example_plan_executor | 3616 / 3616 | 823613 | 0 |
| example_send_command | 3279 / 3279 | 747161 | 0 |
| example_state_management | 2540 / 2540 | 640311 | 0 |
| example_subgraph | 1568 / 1568 | 419423 | 0 |
| Total | 26541 / 26541 | 6395091 | 0 |

The `Smoke`/`GraphCompiler`/`GraphState` subset recorded 31/31 tests, 12551 allocations/frees, 1890717 bytes and zero errors. These counts belong to that revision. The full suite was not the Memcheck scope of this result.

## Current ASan and UBSan recipe

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$SDK_PREFIX" \
  -DNEOGRAPH_BUILD_TESTS=ON -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
  LSAN_OPTIONS="suppressions=$PWD/tests/lsan_suppressions.txt" \
  ctest --test-dir build-asan -E 'BIG_|valgrind' --output-on-failure
```

For Python, preload the matching ASan runtime before importing the extension and use `tests/lsan_suppressions.txt` rather than disabling leak detection as a blanket workaround. The CI workflow defines the actual interpreter setup. Report any exclusions with the result; do not copy an old Python test count into a new run.

## Historical sanitizer and soak evidence

At master `6bd9632` on 2026-04-29, the recorded sanitizer sweep covered eleven mock examples and 322/322 CTest cases with zero reported ASan/UBSan errors. The old default-chain recursion guard fix belongs to that history; the current GraphNode contract has one `run(NodeInput)` override.

A historical 10000-run Counter/Send soak reported `wall=0.68s`, `ops=14728/s`, and RSS 4608 KB at runs 100 and 9999 (delta 0 KB). A separate 1000-concurrent-run debug check reported delta 128 KB. Bounded observed RSS does not prove that no leak path exists in unexercised code, and allocator reuse can hide growth over a short interval.

Historical hardening measurements were seq 5.1 µs/par 275.2 µs baseline and seq 5.1 µs/par 275.6 µs hardened. They establish no detectable difference in that measurement, not zero cost on every workload.

## CI and hardening contracts

The [extended CI workflow](../.github/workflows/ci-extended.yml) defines `sanitizer-test`, `tsan-test` and `fuzz-canary`, which run nightly and on demand rather than on every push: C++ tests/examples and Python under ASan/LSan, concurrent paths under TSan, and compiler input under libFuzzer. Workflow definitions are intended gates, not proof the current dependency configuration has passed them.

TSan and ASan need separate builds. Linux `setarch x86_64 -R` disables address randomization by setting `ADDR_NO_RANDOMIZE`; permissions and kernel layout can still block TSan. Address sanitization does not establish race freedom.

`NEOGRAPH_ENABLE_HARDENING=ON` enables the compiler/platform-qualified assertion, stack-protector, FORTIFY and linker flags. Linux x86 supports the configured `-fcf-protection` option; other platforms do not inherit that claim. The CMake path skips these flags when sanitizer flags are present and under MSVC. CET tagging is not a general guarantee that every control-flow attack fails.

## Uninitialized reads and suppressions

MemorySanitizer requires instrumented dependencies, including the C++ standard library and runtime libraries. ASan/UBSan/TSan are not substitutes for general uninitialized-read detection, and ASan has no general `detect_uninitialized_reads` option. See the [MemorySanitizer documentation](https://clang.llvm.org/docs/MemorySanitizer.html).

Review [LSan suppressions](../tests/lsan_suppressions.txt) and [TSan suppressions](../tests/tsan_suppressions.txt) against each report. They contain third-party patterns, including historical libpqxx entries; current PostgreSQL uses libpq. Do not suppress a NeoGraph-owned leak or race. A suppression can also cover allocations through a library frame, so a suppressed report is not proof that all ownership is correct.
