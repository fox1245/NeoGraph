<!-- neograph-i18n: source=docs/VALGRIND.md locale=zh-CN source_sha256=77ef4aac4dcc1d2275fa67596cf241a8e86f7437291204b5eaab2f91233d1308 -->
# 内存与 sanitizer 检查

**Languages:** [English](VALGRIND.md) | [한국어](VALGRIND.ko.md) | [日本語](VALGRIND.ja.md) | [简体中文](VALGRIND.zh-CN.md)

## 范围和当前前提

Memcheck 查找非法 memory access 和 allocation leak，ASan 检查 address error，UBSan 检查覆盖范围内的 undefined behavior，TSan 检查 data race。一个工具通过不证明另一个工具检查的性质。下文是有日期的历史结果，不是新的 typed-provider 验证。

当前 Core build 即使禁用 LLM node/PostgreSQL 也需要外部 `SchemaProvider::runtime`。设置 `SDK_PREFIX` 为匹配的 installed SDK，或指定 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` 及 SDK 开发依赖。检查 SDK 内部时也用匹配 instrumentation 构建 SDK（`SP_SANITIZE=address` 或 `thread`）。已记录的 interface-3 runtime/archive 验证覆盖 Linux/POSIX，不验证 interface 4；这些 Linux command 不验证 Windows/macOS/WASM。

NeoGraph 要求 CMake 3.20+。依次选择显式 SDK source directory、installed package、默认 fetch 的 revision-pinned 公开 archive。离线使用 installed SDK 时设 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`。source/fetched build 使用 NeoGraph Asio/yyjson，仍需 system libcurl/OpenSSL 开发文件。

## 当前 Memcheck 场景

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug \
  -DNEOGRAPH_BUILD_TESTS=ON -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_POSTGRES=OFF -DCMAKE_PREFIX_PATH="$SDK_PREFIX"
cmake --build build-debug --target example_custom_graph -j
valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all \
  --error-exitcode=42 ./build-debug/example_custom_graph
```

在 rebuild artifact 上运行相关 no-network example 和 provider lifetime/cancel scenario。network/Python 路径需要限定诊断和审查后的 third-party suppression，不免于检查。mock-provider 通过不能验证 SDK transport 或真实 native replay custody。

## 历史 Memcheck sweep，2026-04-29

记录使用 master `4b02dea`、GCC 13.3 Debug、Valgrind 3.22.0。无需 key 的十一个 example 记录 invalid access、mismatched free、leak 均为零：

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

`Smoke`/`GraphCompiler`/`GraphState` subset 记录 31/31 test、12551 allocation/free、1890717 byte、zero error。数量属于该 revision；full suite 不在此 Memcheck 结果范围。

## 当前 ASan 与 UBSan 配方

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

Python 在 import extension 前 preload 匹配 ASan runtime，并用 `tests/lsan_suppressions.txt`，不要一概关闭 leak detection。CI workflow 定义实际 interpreter 设置。随结果报告排除项，不把旧 Python test 数复制到新运行。

## 历史 sanitizer 与 soak 证据

2026-04-29 master `6bd9632` 的记录覆盖十一个 mock example 和 322/322 CTest case，ASan/UBSan 报告 error 为零。旧 default-chain recursion guard 修复属于该历史；当前 GraphNode 只有一个 `run(NodeInput)` override。

历史 Counter/Send 10000-run soak 记录 `wall=0.68s`、`ops=14728/s`，run 100/9999 的 RSS 均为 4608 KB（delta 0 KB）。另一个 debug 1000 concurrent run 记录 delta 128 KB。观测 RSS 有界不证明未执行 code 没有 leak 路径；allocator reuse 可隐藏短期增长。

历史 hardening 测量为 baseline seq 5.1 µs/par 275.2 µs，hardened seq 5.1 µs/par 275.6 µs。仅表示该测量未检测到差异，不表示所有 workload 成本为零。

## CI 与 hardening 契约

[CI workflow](../.github/workflows/ci.yml) 定义 `sanitizer-test`、`tsan-test`、`fuzz-canary`：C++ test/example 和 Python 的 ASan/LSan、TSan concurrent 路径、libFuzzer compiler input。workflow 定义是预期 gate，不证明当前依赖配置已通过。

TSan 与 ASan 需要独立 build。Linux `setarch x86_64 -R` 设置 `ADDR_NO_RANDOMIZE` 以禁用 address randomization；权限/kernel layout 仍可阻碍 TSan。address sanitization 不证明无 race。

`NEOGRAPH_ENABLE_HARDENING=ON` 启用适配 compiler/platform 的 assertion、stack-protector、FORTIFY、linker flag。Linux x86 支持配置的 `-fcf-protection`，不能扩展为其他 platform 的声明。存在 sanitizer flag 或使用 MSVC 时 CMake 跳过这些 flag。CET tagging 不保证所有 control-flow attack 失败。

## 未初始化读取与 suppression

MemorySanitizer 需要已插桩 dependency，包括 C++ standard library 和 runtime library。ASan/UBSan/TSan 不替代一般 uninitialized-read detection，ASan 没有通用 `detect_uninitialized_reads` option。参见 [MemorySanitizer 文档](https://clang.llvm.org/docs/MemorySanitizer.html)。

按每份 report 审查 [LSan suppression](../tests/lsan_suppressions.txt) 和 [TSan suppression](../tests/tsan_suppressions.txt)。third-party pattern 包含历史 libpqxx entry；当前 PostgreSQL 使用 libpq。不要隐藏 NeoGraph 自有 leak/race。suppression 也可覆盖经过 library frame 的 allocation，因此 suppressed report 不证明所有 ownership 正确。
