<!-- neograph-i18n: source=docs/VALGRIND.md locale=ja source_sha256=843b39a1b1a8b8f59fd537d6eeed53a258ad133623776cf2c22396c72872dce0 -->
# memory と sanitizer 検査

**Languages:** [English](VALGRIND.md) | [한국어](VALGRIND.ko.md) | [日本語](VALGRIND.ja.md) | [简体中文](VALGRIND.zh-CN.md)

## 範囲と現在の前提

Memcheck は不正 memory access と allocation leak、ASan は address error、UBSan は対象となる undefined behavior、TSan は data race を検査する。一つの pass は別の tool の性質を証明しない。以下は日付付きの歴史的結果で、新しい typed-provider 検証ではない。

現在の Core build は LLM node/PostgreSQL 無効時も外部 `SchemaProvider::runtime` を要する。`SDK_PREFIX` を対応する installed SDK に設定するか、`NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` と SDK 開発依存を指定する。SDK 内部の sanitizer 検査では SDK も同じ計装で build する（`SP_SANITIZE=address` または `thread`）。記録された interface-3 runtime/archive 検証は Linux/POSIX の範囲で、interface 4 の資格検証ではない。この Linux command は Windows/macOS/WASM の検証ではない。

NeoGraph は CMake 3.20+ を要する。明示 SDK source directory、installed package、既定 fetch の revision-pinned 公開 archive の順に選択する。installed SDK の offline 利用では `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` にする。source/fetched build は NeoGraph の Asio/yyjson を使うが system libcurl/OpenSSL 開発 file は必要。

## 現在の Memcheck scenario

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug \
  -DNEOGRAPH_BUILD_TESTS=ON -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_POSTGRES=OFF -DCMAKE_PREFIX_PATH="$SDK_PREFIX"
cmake --build build-debug --target example_custom_graph -j
valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all \
  --error-exitcode=42 ./build-debug/example_custom_graph
```

rebuild artifact で関連 no-network example と provider lifetime/cancel scenario を実行する。network/Python 経路にも限定した診断と検討した third-party suppression が必要で、検査免除ではない。mock-provider pass は SDK transport や本物の native replay custody を検証しない。

## 歴史的 Memcheck sweep、2026-04-29

記録は master `4b02dea`、GCC 13.3 Debug、Valgrind 3.22.0 を使用した。key 不要の十一 example で invalid access、mismatched free、leak はゼロだった。

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

`Smoke`/`GraphCompiler`/`GraphState` subset は 31/31 test、12551 allocation/free、1890717 byte、zero error を記録した。数はその revision のもので、full suite はこの Memcheck 結果の範囲ではない。

## 現在の ASan と UBSan 手順

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

Python は extension import 前に対応 ASan runtime を preload し、leak detection を一律無効にせず `tests/lsan_suppressions.txt` を使う。実際の interpreter 設定は CI workflow にある。除外を結果とともに記し、旧 Python test 数を新実行へコピーしない。

## 歴史的 sanitizer と soak 根拠

2026-04-29 master `6bd9632` の記録は十一 mock example と 322/322 CTest case を対象に ASan/UBSan reported error ゼロだった。旧 default-chain recursion guard 修正はその歴史に属する。現在の GraphNode は単一 `run(NodeInput)` override を使う。

歴史的 Counter/Send 10000-run soak は `wall=0.68s`、`ops=14728/s`、run 100/9999 の RSS 4608 KB（delta 0 KB）を記録した。別の debug 1000 concurrent run は delta 128 KB。観測 RSS の上限は未実行 code の leak 経路不存在を証明せず、allocator reuse は短期間の増加を隠せる。

歴史的 hardening 測定は baseline seq 5.1 µs/par 275.2 µs、hardened seq 5.1 µs/par 275.6 µs。その測定で差を検出できなかったのであり、全 workload の費用ゼロではない。

## CI と hardening 契約

[拡張 CI workflow](../.github/workflows/ci-extended.yml) は、push ごとではなく毎晩および手動で実行される `sanitizer-test`、`tsan-test`、`fuzz-canary` を定義する。C++ test/example と Python の ASan/LSan、TSan の concurrent 経路、libFuzzer の compiler input が対象。workflow 定義は意図する gate で、現在の依存設定の pass 証拠ではない。

TSan と ASan は別 build が必要。Linux `setarch x86_64 -R` は `ADDR_NO_RANDOMIZE` を設定し address randomization を無効にする。権限/kernel layout が TSan を阻む場合は残る。address sanitization は race freedom を証明しない。

`NEOGRAPH_ENABLE_HARDENING=ON` は compiler/platform に対応する assertion、stack-protector、FORTIFY、linker flag を有効にする。Linux x86 は設定した `-fcf-protection` を支持するが、他 platform に拡張できない。CMake は sanitizer flag がある場合と MSVC でこれらを省く。CET tagging は全 control-flow attack の失敗保証ではない。

## uninitialized read と suppression

MemorySanitizer は C++ standard library/runtime library を含む計装 dependency を要する。ASan/UBSan/TSan は一般的 uninitialized-read detection の代替ではなく、ASan に一般的 `detect_uninitialized_reads` option はない。[MemorySanitizer 文書](https://clang.llvm.org/docs/MemorySanitizer.html)を参照。

各 report に対し [LSan suppression](../tests/lsan_suppressions.txt) と [TSan suppression](../tests/tsan_suppressions.txt) を確認する。third-party pattern に歴史的 libpqxx entry があるが、現在の PostgreSQL は libpq を使う。NeoGraph 所有 leak/race を隠さない。library frame を通る allocation も suppression され得るので、suppressed report は全 ownership の正しさの証拠ではない。
