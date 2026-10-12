<!-- neograph-i18n: source=docs/VALGRIND.md locale=zh-CN source_sha256=843b39a1b1a8b8f59fd537d6eeed53a258ad133623776cf2c22396c72872dce0 -->
# 内存与 sanitizer 检查

**Languages:** [English](VALGRIND.md) | [한국어](VALGRIND.ko.md) | [日本語](VALGRIND.ja.md) | [简体中文](VALGRIND.zh-CN.md)

## 范围与当前的前提条件

用 Memcheck 查找非法内存访问和分配泄漏，用 ASan 查地址类错误，用 UBSan 查它所覆盖的未定义行为，用 TSan 查数据竞争。通过其中一个工具，并不能证明另一个工具所检查的性质成立。下面的报告是带日期的历史结果，不是对新的带类型 provider 的资格认证。

当前的 Core 构建需要外部的 `SchemaProvider::runtime`，即使禁用了 LLM 节点和 PostgreSQL 也一样。把 `SDK_PREFIX` 设为匹配的、已安装的 SDK；或者提供 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` 及其开发依赖。要用 sanitizer 检查 SDK 内部，需要用相应的插桩来构建这个 SDK（`SP_SANITIZE=address` 或 `thread`）。记录在案的接口 3 的运行时和 archive 资格认证覆盖 Linux/POSIX，不覆盖接口 4；这些 Linux 命令也不能证明 Windows、macOS 或 WASM 合格。

NeoGraph 需要 CMake 3.20 及以上。选择 SDK 源码的顺序是：优先使用显式指定的 SDK 源码目录，其次是已安装的包，最后是默认下载的、固定了修订版本的公开归档。离线使用已安装的 SDK 时，设置 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`。从源码构建或下载构建时，使用 NeoGraph 自带的 Asio/yyjson；系统的 libcurl/OpenSSL 开发文件仍然是必需的。

## 运行当前的 Memcheck 场景

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug \
  -DNEOGRAPH_BUILD_TESTS=ON -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_POSTGRES=OFF -DCMAKE_PREFIX_PATH="$SDK_PREFIX"
cmake --build build-debug --target example_custom_graph -j
valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all \
  --error-exitcode=42 ./build-debug/example_custom_graph
```

在重新构建的产物上，运行相关的无网络示例，以及 provider 生命周期/取消的场景。网络和 Python 路径需要限定范围的诊断，以及经过审阅的第三方抑制规则；它们同样不能免于内存检查。用 mock provider 通过测试，不能证明 SDK 的传输层或真实的 native replay 托管合格。

## 历史上的 Memcheck 全面检查，2026-04-29

记录在案的这次检查使用 master 提交 `4b02dea`、GCC 13.3 的 Debug 构建和 Valgrind 3.22.0。十一个不需要密钥的示例，都没有记录到非法访问、不匹配的释放和泄漏：

| 4b02dea 上的示例 | 分配 / 释放次数 | 分配字节数 | 错误数 |
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
| 合计 | 26541 / 26541 | 6395091 | 0 |

`Smoke`/`GraphCompiler`/`GraphState` 子集记录到 31/31 个测试通过、12551 次分配和释放、1890717 字节、零错误。这些数字属于该修订版。这次结果的 Memcheck 范围不包括完整的测试集。

## 当前的 ASan 与 UBSan 做法

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

对于 Python，要在导入扩展之前预加载匹配的 ASan 运行时，并使用 `tests/lsan_suppressions.txt`，而不要一刀切地关闭泄漏检测。实际的解释器配置以 CI 工作流为准。有任何被排除的内容，都要连同结果一起报告；不要把旧的 Python 测试数量照搬到新的运行里。

## 历史上的 sanitizer 与长时间运行证据

在 2026-04-29 的 master `6bd9632` 上，记录在案的 sanitizer 全面检查覆盖了十一个 mock 示例和 322/322 个 CTest 用例，没有报告任何 ASan/UBSan 错误。旧的默认调用链递归保护的修复属于那段历史；当前的 GraphNode 契约只有一个需要重写的 `run(NodeInput)`。

历史上一次 10000 轮的 Counter/Send 长时间运行测试，报告 `wall=0.68s`、`ops=14728/s`，第 100 轮和第 9999 轮的 RSS 都是 4608 KB（差值 0 KB）。另一次 1000 个并发运行的 debug 检查，报告差值为 128 KB。观察到的 RSS 有界，并不能证明没有被测试到的代码里不存在泄漏路径；分配器的复用也可能在较短的区间内掩盖增长。

历史上的加固测量值是：基线 seq 5.1 µs / par 275.2 µs，加固后 seq 5.1 µs / par 275.6 µs。这只说明在那次测量里没有检测到差异，并不是说在所有工作负载上开销都为零。

## CI 与加固的约定

[扩展 CI 工作流](../.github/workflows/ci-extended.yml)定义了 `sanitizer-test`、`tsan-test` 和 `fuzz-canary`，它们每晚运行或按需运行，而不是每次 push 都运行：在 ASan/LSan 下运行 C++ 测试/示例和 Python，在 TSan 下运行并发路径，在 libFuzzer 下运行编译器输入。工作流定义的是预期的闸门，不能证明当前的依赖配置已经通过了它们。

TSan 和 ASan 需要分别构建。Linux 上的 `setarch x86_64 -R` 通过设置 `ADDR_NO_RANDOMIZE` 关闭地址随机化；权限和内核布局仍然可能让 TSan 无法运行。地址 sanitizer 通过，并不能证明没有数据竞争。

`NEOGRAPH_ENABLE_HARDENING=ON` 会启用经编译器/平台确认可用的断言、栈保护、FORTIFY 和链接器选项。Linux x86 支持配置的 `-fcf-protection` 选项；其他平台不能照搬这一点。当存在 sanitizer 选项时，以及在 MSVC 下，CMake 会跳过这些选项。CET 标记并不能保证所有控制流攻击都会失败。

## 未初始化读取与抑制规则

MemorySanitizer 要求依赖项（包括 C++ 标准库和运行时库）都经过插桩。ASan/UBSan/TSan 不能代替通用的未初始化读取检测，而且 ASan 没有通用的 `detect_uninitialized_reads` 选项。参见 [MemorySanitizer 文档](https://clang.llvm.org/docs/MemorySanitizer.html)。

请对照每一份报告，审阅 [LSan 抑制规则](../tests/lsan_suppressions.txt)和 [TSan 抑制规则](../tests/tsan_suppressions.txt)。其中是第三方的模式，包括历史上遗留的 libpqxx 条目；当前的 PostgreSQL 使用 libpq。不要抑制 NeoGraph 自己的泄漏或数据竞争。抑制规则也可能覆盖经过某个库栈帧的分配，所以被抑制的报告，并不能证明所有的所有权都正确。
