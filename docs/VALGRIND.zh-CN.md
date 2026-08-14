<!-- neograph-i18n: source=docs/VALGRIND.md locale=zh-CN source_sha256=38c9d7fc8073d2557d9b5533dfb33ec2de6867a55ba394bc26d6a5cf6b2e6215 -->
# 内存与消毒器扫描（Valgrind / ASan / UBSan / 浸泡）

**Languages:** [English](VALGRIND.md) | [한국어](VALGRIND.ko.md) | [日本語](VALGRIND.ja.md) | [简体中文](VALGRIND.zh-CN.md)

基准真实性检查，验证示例二进制文件和测试二进制文件释放了其所做的每一次分配。
在 `valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all` 下运行。
标准是**零泄漏、零错误**覆盖无需 API 密钥的示例面。

## 本地复现

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

## 最近一次扫描

于 2026-04-29 对 master（提交 4b02dea，在分类器扇出示例之后）运行。
Valgrind 3.22.0，GCC 13.3 Debug 构建。

### 示例——11 / 11 干净

| 示例 | 分配 | 字节 | 错误 |
|---:|---:|---:|---:|
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
| **累计** | **26,541 / 26,541** | **6,395,091** | **0** |

每次分配均已释放，0 次无效读取，0 次无效写入，0 次不匹配释放，0 次释放后使用。

### 测试——`*Smoke*:GraphCompiler*:GraphState*` 干净

| 测试套件 | 测试 | 分配 | 字节 | 错误 |
|---:|---:|---:|---:|---:|
| Smoke / GraphCompiler / GraphState（31 个测试） | 31 / 31 通过 | 12,551 / 12,551 | 1,890,717 | 0 |

完整的 `neograph_tests` 套件在 valgrind 下运行约需 30 分钟——以上子集
是每个 PR 的底线；完整扫描作为夜间 CI 作业可行（尚未接入）。

## 未覆盖的部分

- 涉及网络的示例（`example_react_agent`、`example_mcp_*`、
  `example_*_responses_*`）——TLS/套接字交互会产生来自 libssl /
  libcurl 的噪音，需要 valgrind 压制规则来屏蔽。建议改用 mock Provider
  在引擎路径上进行泄漏检查。
- Crawl4AI / Postgres 示例——外部进程或库状态会干扰泄漏检查；这些路径
  的覆盖通过 CI 中的 ASan 而非 valgrind 实现。
- Python 绑定（`_neograph.so`）——Python 解释器在退出时有很多有意的
  "泄漏"（已分配但未释放的模块状态），会淹没 valgrind 的信号。
  `LSAN_OPTIONS=detect_leaks=0` 的 ASan 是这里正确的工具。

## ASan + UBSan + LSan 扫描——11/11 示例 + 322 ctests 干净

使用消毒器编译：

```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
    -DNEOGRAPH_BUILD_TESTS=ON -DNEOGRAPH_BUILD_EXAMPLES=ON \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -O1" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j$(nproc)
```

使用 `LSAN_OPTIONS=` 和 `UBSAN_OPTIONS=` 运行示例 + 测试：

```bash
export ASAN_OPTIONS="detect_leaks=1:halt_on_error=0"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0"
ctest --test-dir build-asan -E "BIG_|valgrind"   # 322/322 pass (2026-04-29)
```

最近一次对 master HEAD（提交 6bd9632，2026-04-29）的扫描：

| 覆盖范围 | 结果 |
|---|---|
| 11 个 mock 示例（custom_graph、send_command、intent_routing、state_management、all_features、subgraph、checkpoint_hitl、classifier_fanout、async_concurrent_runs、parallel_fanout、plan_executor） | ✓ exit=0，0 个 ASan/UBSan 错误 |
| `neograph_tests` ctest（消毒器下的 322 个测试） | ✓ 322/322 通过 |

ASan 暴露了当天新增的递归守卫中的一个误报——原始的 `thread_local int`
深度计数器在嵌套图中触发（子图节点的内部引擎在同一线程上分发导致外部
计数器为非零）。通过切换到每个节点的 `const GraphNode*` 键修复，因此
守卫仅在同一节点重新进入自己的默认链时触发。

## 长时间浸泡压力测试——10,000 次图运行，RSS Δ = 0 KB

```cpp
// /tmp/stress_runs.cpp — see commit log
for (int i = 0; i < 10000; ++i) {
    engine->run(RunConfig{.thread_id = "t" + std::to_string(i),
                          .input    = {{"count", 0}}});
    if (i == 100)  rss_at_100  = read_rss_kb();
    if (i == 9999) rss_at_1000 = read_rss_kb();
}
```

在 master HEAD 上使用一个 Counter 节点运行，该节点每次运行递归发送扇出
深度至多 3 层——一个现实的压力形态：

```
10000 runs wall=0.68s  ops=14728/s  RSS@100=4608kB  RSS@9999=4608kB  Δ=0kB
PASS: RSS growth bounded
```

10,000 次连续的图运行，每次分配几 KB 并释放，在最后 9,900 次迭代中
**驻留集增长为 0 KB**。Linux glibc 分配器将释放的块干净地归还给池——
不存在每次运行的泄漏路径。

## CI 门禁（sanitizer-test、tsan-test、fuzz-canary）

`.github/workflows/ci.yml` 中的三个 CI 作业在每次 PR 和推送到 master 时
强制执行以下内容：

### `sanitizer-test` — ASan + UBSan + LSan

| 步骤 | 覆盖范围 |
|---|---|
| `-fsanitize=address,undefined` 下的 `ctest -E "BIG_\|valgrind"` | 所有单元测试，包括外部接口（通过服务容器的 Postgres、MCP HTTP/stdio、libssl/libcurl ConnPool） |
| 相同标志下的 11 个 mock 示例 | 完整的引擎路径编排覆盖 |
| 带 `LD_PRELOAD=libasan.so` + `detect_leaks=1` 的 `pytest bindings/python/tests/` | 46/48 个启用泄漏检测的 Python 测试（排除 2 个在 pybind 之间传播 Python 异常的测试——已知的 ASan `__cxa_throw` 拦截限制，非 NeoGraph 错误） |

### `tsan-test` — 引擎并发路径的竞态检测

| 步骤 | 覆盖范围 |
|---|---|
| `-fsanitize=thread` 下的 `setarch x86_64 -R ctest -E "BIG_\|valgrind"` | 所有 344 个单元测试，包括新增的 `ConcurrentStress.TwoHundredOverlappingRunsAllSucceed`（200 个同时 `run_async` × 3 路 Send 扇出——捕获工作池、调度器、parallel_group 和 CheckpointStore 并发路径中的数据竞态） |
| TSan 下的 5 个扇出/异步示例 | `example_classifier_fanout` + `parallel_fanout` + `send_command` + `plan_executor` + `async_concurrent_runs` |

`setarch x86_64 -R` 包装禁用 `ADDR_NO_RANDOMIZE`（内核
`mmap_rnd_bits` 在 Ubuntu 24.04+ 上默认触发 TSan `unexpected
memory mapping` 致命错误）；该标志通过 `fork` 继承，因此每个测试子进程
同样获得 TSan 友好的地址布局。

TSan + ASan 在链接时互斥，因此这是一个独立于 `sanitizer-test` 的作业。

### `fuzz-canary` — 对 `GraphCompiler::compile` 的 libFuzzer

| 步骤 | 覆盖范围 |
|---|---|
| `fuzz_graph_compile` 运行 60 秒（`-max_total_time=60`） | 变异 `tests/fuzz/corpus/graph_compile/` 下的种子语料库，并将字节输入 `neograph::json::parse` → `GraphCompiler::compile`。捕获解析器未定义行为、未处理异常、堆缓冲区溢出回归。首次在 master HEAD 上运行完成了 194 万次迭代，无崩溃。 |

使用 Clang 的 `-fsanitize=fuzzer,address,undefined` 构建，因此任何崩溃
都会在同一跟踪中显示 ASan/UBSan 诊断信息。

## Release 构建加固

Release / RelWithDebInfo / MinSizeRel 构建默认启用纵深防御标志
（`NEOGRAPH_ENABLE_HARDENING=ON`）：

| 标志 | 捕获的内容 |
|---|---|
| `-D_GLIBCXX_ASSERTIONS` | std::vector 越界、解引用 `end()`、迭代器失效、未初始化 `std::optional` 访问——以诊断信息中止而非静默未定义行为。在 Debug + Release 中均激活。 |
| `-fstack-protector-strong` | 会破坏返回地址的缓冲区溢出——金丝雀检查在 `ret` 之前触发。 |
| `-fcf-protection=full` | 间接调用/跳转目标标记用于控制流完整性。ROP 风格攻击在调用点失败。在带 CET-IBT 的 amd64 上成本低廉。 |
| `-D_FORTIFY_SOURCE=2` | 对 libc 字符串/内存例程的内联检查。仅 Release（需要 ≥`-O1`）。 |
| `-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack` | 只读重定位、立即绑定（无延迟 PLT 写入）、不可执行栈——RELRO 基线。 |

在 master HEAD 上用 `bench_neograph` 测量的性能影响：

|  | seq µs | par µs |
|---:|---:|
| 基线 Release | 5.1 | 275.2 |
| 加固 Release | 5.1 | 275.6 |

在测量噪声范围内**0% 开销**。这些标志将工作转移到链接器（重定位、PLT）
和每个函数 8 字节金丝雀加载+比较——在 NeoGraph 引擎路径的 µs 尺度上
均不可见。

在消毒器构建（ASan/TSan/UBSan）下自动禁用，否则会与消毒器自身的检查
重复。在 MSVC 下禁用（使用不同的加固原语——`/GS` 等，不在本文范围内）。

## 已探索但不可行的消毒器组合

**MemorySanitizer**（未初始化读取检测）：要求每个链接的 C/C++ 库——
包括 libstdc++、libssl、libcurl、libpqxx——均经过 MSan 插桩，否则调用
它们会产生淹没信号的误报。Ubuntu 24.04 上 Clang 预构建的 `libc++`
未提供 MSan 变体，重建标准库 + 每个传递依赖不切实际。
ASan+UBSan+TSan 三者已能捕获泄漏到堆分配状态中的未初始化读取（因为在
某些通道中堆在 ASan 下以 `detect_uninitialized_reads=1` 语义在分配时
被下毒）。已跳过。

## 压制规则

| 文件 | 覆盖内容 |
|---|---|
| [`tests/lsan_suppressions.txt`](../tests/lsan_suppressions.txt) | libssl / libcurl / libpq / libpqxx / libstdc++ ABI / glibc TLS / CPython 解释器 / pybind11 类型初始化 / pydantic-core。仅第三方——添加 NeoGraph 符号是一个真正的错误，请修复泄漏而非压制。 |
| [`tests/tsan_suppressions.txt`](../tests/tsan_suppressions.txt) | asio reactor & socket service（epoll 发生先于）、yyjson SIMD 读取、OpenSSL CRYPTO_THREAD_run_once。库内部的良性竞态。 |

## 并发压力测试

`tests/test_concurrent_stress.cpp` 作为标准 ctest 套件的一部分运行
（因此在 Debug 和 ASan 下均运行）：

- **TwoHundredOverlappingRunsAllSucceed** —— 200 个 `engine->run_async()`
  调用在单个 io_context 上重叠，每个调用带有 3 路 Send 扇出。
  验证 parallel-group + pending-writes 机制在 ASan 下无竞态，且所有
  200 次运行产生预期的 `{0, 1, 4}` 工作器输出。
- **RssBoundedOverHundredsOfConcurrentRuns** —— 5 轮 200 次运行
  （共计 1,000 次并发），RSS Δ ≤ 10 MB 阈值。在 ASan 下跳过（消毒器
  的影子内存增长主导了信号）。

Debug 构建运行在 1,000 次并发运行中产生 RSS Δ=128 kB——引擎侧内存在
持续并发负载下保持平坦。
