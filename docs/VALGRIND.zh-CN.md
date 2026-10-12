<!-- neograph-i18n: source=docs/VALGRIND.md locale=zh-CN source_sha256=21a939b18bdcef34c5ead883275d4932d5f9eeec6c25401be1dfd8836025e75f -->
# 内存与 sanitizer 检查

**Languages:** [English](VALGRIND.md) | [한국어](VALGRIND.ko.md) | [日本語](VALGRIND.ja.md) | [简体中文](VALGRIND.zh-CN.md)

## 范围与前提条件

用 Memcheck 查找非法内存访问和分配泄漏，用 ASan 查地址类错误，用 UBSan 查它所覆盖的未定义行为，用 TSan 查数据竞争。通过其中一个工具，并不能证明另一个工具所检查的性质成立。

当前的 Core 构建需要外部的 `SchemaProvider::runtime`，即使禁用了 LLM 节点和 PostgreSQL 也一样。把 `SDK_PREFIX` 设为匹配的、已安装的 SDK；或者提供 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` 及其开发依赖。要用 sanitizer 检查 SDK 内部，需要用相应的插桩来构建这个 SDK（`SP_SANITIZE=address` 或 `thread`）。

NeoGraph 需要 CMake 3.20 及以上。选择 SDK 源码的顺序是：优先使用显式指定的 SDK 源码目录，其次是已安装的包，最后是默认下载的、固定了修订版本的公开归档。离线使用已安装的 SDK 时，设置 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`。从源码构建或下载构建时，使用 NeoGraph 自带的 Asio/yyjson；系统的 libcurl/OpenSSL 开发文件仍然是必需的。

## 运行 Memcheck 场景

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug \
  -DNEOGRAPH_BUILD_TESTS=ON -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_POSTGRES=OFF -DCMAKE_PREFIX_PATH="$SDK_PREFIX"
cmake --build build-debug --target example_custom_graph -j
valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all \
  --error-exitcode=42 ./build-debug/example_custom_graph
```

在重新构建的产物上，运行相关的无网络示例，以及 provider 生命周期/取消的场景。网络和 Python 路径需要限定范围的诊断，以及经过审阅的第三方抑制规则；它们同样不能免于内存检查。用 mock provider 运行，覆盖不到 SDK 的传输层或真实的 native replay 托管。

## ASan 与 UBSan 做法

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

对于 Python，要在导入扩展之前预加载匹配的 ASan 运行时，并使用 `tests/lsan_suppressions.txt`，而不要一刀切地关闭泄漏检测。实际的解释器配置以 CI 工作流为准。有任何被排除的内容，都要连同结果一起报告。

## CI 与加固的约定

[扩展 CI 工作流](../.github/workflows/ci-extended.yml)定义了 `sanitizer-test`、`tsan-test` 和 `fuzz-canary`，它们每晚运行或按需运行，而不是每次 push 都运行：在 ASan/LSan 下运行 C++ 测试/示例和 Python，在 TSan 下运行并发路径，在 libFuzzer 下运行编译器输入。

TSan 和 ASan 需要分别构建。Linux 上的 `setarch x86_64 -R` 通过设置 `ADDR_NO_RANDOMIZE` 关闭地址随机化；权限和内核布局仍然可能让 TSan 无法运行。地址 sanitizer 通过，并不能证明没有数据竞争。

`NEOGRAPH_ENABLE_HARDENING=ON` 会启用经编译器/平台确认可用的断言、栈保护、FORTIFY 和链接器选项。Linux x86 支持配置的 `-fcf-protection` 选项；其他平台不能照搬这一点。当存在 sanitizer 选项时，以及在 MSVC 下，CMake 会跳过这些选项。CET 标记并不能保证所有控制流攻击都会失败。

有一次对加固开销的测量，基线是 seq 5.1 µs / par 275.2 µs，加固后是 seq 5.1 µs / par 275.6 µs。这只说明在那次测量里没有检测到差异，并不是说在所有工作负载上开销都为零。

## 未初始化读取与抑制规则

MemorySanitizer 要求依赖项（包括 C++ 标准库和运行时库）都经过插桩。ASan/UBSan/TSan 不能代替通用的未初始化读取检测，而且 ASan 没有通用的 `detect_uninitialized_reads` 选项。参见 [MemorySanitizer 文档](https://clang.llvm.org/docs/MemorySanitizer.html)。

请对照每一份报告，审阅 [LSan 抑制规则](../tests/lsan_suppressions.txt)和 [TSan 抑制规则](../tests/tsan_suppressions.txt)。其中是第三方的模式，包括历史上遗留的 libpqxx 条目；当前的 PostgreSQL 使用 libpq。不要抑制 NeoGraph 自己的泄漏或数据竞争。抑制规则也可能覆盖经过某个库栈帧的分配，所以被抑制的报告，并不能证明所有的所有权都正确。
