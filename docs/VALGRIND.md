# Memory and sanitizer checks

**Languages:** [English](VALGRIND.md) | [한국어](VALGRIND.ko.md) | [日本語](VALGRIND.ja.md) | [简体中文](VALGRIND.zh-CN.md)

## Scope and prerequisites

Use Memcheck to find invalid memory accesses and allocation leaks, ASan for address errors, UBSan for covered undefined behavior, and TSan for data races. Passing one tool does not prove the properties checked by another.

Current Core builds require external `SchemaProvider::runtime`, even when LLM nodes and PostgreSQL are disabled. Set `SDK_PREFIX` to a matching installed SDK; alternatively supply `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` and its development dependencies. To inspect SDK internals with sanitizers, build that SDK with matching instrumentation (`SP_SANITIZE=address` or `thread`).

NeoGraph requires CMake 3.20+. Source selection prefers an explicit SDK source directory, then an installed package, then a revision-pinned public archive fetched by default. Set `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` when using an installed SDK offline. Source/fetched builds use NeoGraph's Asio/yyjson; system libcurl/OpenSSL development files remain required.

## Run a Memcheck scenario

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug \
  -DNEOGRAPH_BUILD_TESTS=ON -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_POSTGRES=OFF -DCMAKE_PREFIX_PATH="$SDK_PREFIX"
cmake --build build-debug --target example_custom_graph -j
valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all \
  --error-exitcode=42 ./build-debug/example_custom_graph
```

Run the relevant no-network examples and provider lifetime/cancellation scenarios on the rebuilt artifacts. Network and Python paths need scoped diagnostics and reviewed third-party suppressions; they are not exempt from memory checks. A mock-provider run does not exercise the SDK transport or authentic native replay custody.

## ASan and UBSan recipe

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

For Python, preload the matching ASan runtime before importing the extension and use `tests/lsan_suppressions.txt` rather than disabling leak detection as a blanket workaround. The CI workflow defines the actual interpreter setup. Report any exclusions with the result.

## CI and hardening contracts

The [extended CI workflow](../.github/workflows/ci-extended.yml) defines `sanitizer-test`, `tsan-test` and `fuzz-canary`, which run nightly and on demand rather than on every push: C++ tests/examples and Python under ASan/LSan, concurrent paths under TSan, and compiler input under libFuzzer.

TSan and ASan need separate builds. Linux `setarch x86_64 -R` disables address randomization by setting `ADDR_NO_RANDOMIZE`; permissions and kernel layout can still block TSan. Address sanitization does not establish race freedom.

`NEOGRAPH_ENABLE_HARDENING=ON` enables the compiler/platform-qualified assertion, stack-protector, FORTIFY and linker flags. Linux x86 supports the configured `-fcf-protection` option; other platforms do not inherit that claim. The CMake path skips these flags when sanitizer flags are present and under MSVC. CET tagging is not a general guarantee that every control-flow attack fails.

One measurement of the hardening cost gave seq 5.1 µs/par 275.2 µs for the baseline and seq 5.1 µs/par 275.6 µs hardened. It shows no detectable difference in that measurement, not zero cost on every workload.

## Uninitialized reads and suppressions

MemorySanitizer requires instrumented dependencies, including the C++ standard library and runtime libraries. ASan/UBSan/TSan are not substitutes for general uninitialized-read detection, and ASan has no general `detect_uninitialized_reads` option. See the [MemorySanitizer documentation](https://clang.llvm.org/docs/MemorySanitizer.html).

Review [LSan suppressions](../tests/lsan_suppressions.txt) and [TSan suppressions](../tests/tsan_suppressions.txt) against each report. They contain third-party patterns, including historical libpqxx entries; current PostgreSQL uses libpq. Do not suppress a NeoGraph-owned leak or race. A suppression can also cover allocations through a library frame, so a suppressed report is not proof that all ownership is correct.
