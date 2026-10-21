<!-- neograph-i18n: source=docs/performance-deep-dive.md locale=zh-CN source_sha256=1441793955b212960f5bb907b65dda7705de92ccfbfa4877fb40810066ac2ea7 -->
# 性能深度剖析

**Languages:** [English](performance-deep-dive.md) | [한국어](performance-deep-dive.ko.md) | [日本語](performance-deep-dive.ja.md) | [简体中文](performance-deep-dive.zh-CN.md)

## 如何解读这些历史测量值

下面的表格保留的是切换（cutover）之前的测量值。它们不能证明当前带类型的 provider、它所依赖的外部运行时，或者重新构建的 Python wheel 合格。做任何新的对比时，都要记录修订版本、编译器、配置、拓扑、并发数、endpoint 和计时边界。当前如何复现，见[基准测试](../benchmarks/README.md)。

这些测量值不能用来保证云成本、worker 数量或容量。除了包的版本，还要固定 wheel 的哈希和原生依赖；仅凭一个版本号，不能让部署可复现。

## 仅引擎的工作负载

当前的跨框架表格（匹配与惯用默认值、内存、启动时间和体积）由一份已入库的记录生成，见[基准测试指南](../benchmarks/README.md)。本页过去重复的 2026 年 4 月表格是在多个会话中、混用版本测得的；它们连同对现已确认无效之处的说明，保留在[历史记录](../benchmarks/history/engine-overhead-2026-04.md)中。不含 I/O 的引擎对比无法预测模型服务的延迟。

## 突发并发

一万个请求同时提交（封闭的单次突发），运行在限制为 1 个 CPU 和 512 MB 内存的 Docker cgroup 中。记录数据所支持的墙钟时间一列，以及被撤回的列和行及其原因，见[并发基准测试](../benchmarks/concurrent/CONCURRENT.md)。这些结果只属于该工作负载和环境，并不能确立 Python/GIL 的普适扩展规律。

## 历史上的缓存模拟

Cachegrind 模拟了 Ryzen 7 5800X 的缓存层级：32 KB 的 L1 指令/数据缓存，8 路组相联；32 MB 的末级缓存，16 路组相联；缓存行 64 字节。历史上的并发基准测试报告了：

| N | 指令引用数 | 末级缓存指令未命中数 | 原生 p50（工作线程内的服务时间） |
|---|---:|---:|---:|
| 1 | 5.3 M | 4313 | 17 µs |
| 10 | 5.9 M | 4304 | 16 µs |
| 100 | 11.8 M | 4320 | 6 µs |
| 1000 | 69.7 M | 4327 | 6 µs |
| 10000 | 648 M | 4329 | 5 µs |

把约 4330 次未命中乘以 64 字节，得到按缓存行数估算的 277 KB。Cachegrind 模拟出的未命中数，本身并不能证明真实的常驻工作集、DRAM 停顿时间，或原生延迟变化的原因。参见 [Cachegrind 手册](https://valgrind.org/docs/manual/cg-manual.html)。

## 历史上的本地模型工作负载

另一个 neoclaw 实验使用一块 RTX 4070 Ti 和一个 Gemma 4 E2B Q4 GGUF 模型（权重约 1.5 GB），由共享的 LocalProvider 串行化推理，图只有一个 `llm_call` 节点。记录的总耗时包含把所有 worker 排空的时间；表中列出的请求分位数，并不能说明排队消失了。

| N | 总耗时 s | 吞吐量 rps | p50 ms | p99 ms | 峰值 RSS MB |
|---|---:|---:|---:|---:|---:|
| 1 | 0.64 | 1.6 | 642 | 642 | 2464 |
| 10 | 0.94 | 10.6 | 184 | 686 | 2529 |
| 100 | 4.81 | 20.8 | 343 | 855 | 2549 |
| 1000 | 44.1 | 22.7 | 347 | 673 | 2564 |
| 5000 | 213.7 | 23.4 | 338 | 657 | 2570 |
| 10000 | 424 | 23.6 | 337 | 648 | 2572 |

来源：[neoclaw 基准测试](https://github.com/fox1245/neoclaw/blob/main/benchmarks/bench_concurrent_workers_local_llm.cpp)。其中的 CUDA 基准配置属于那个仓库，不是 NeoGraph 当前 provider 构建的一部分。不要把观察到的边际内存外推到没有测过的会话数上限。

另一个历史实验，在 HTTP endpoint 后面使用了本地的 Gemma-4 E2B Q4_K_M 模型（4.65 B 参数，2.9 GB 的 GGUF）：

| 指标 | 仅引擎 | 历史上的本地 Gemma HTTP |
|---|---:|---:|
| 末级缓存指令未命中数 | 4320 | 7262 |
| 64 × 模拟的末级缓存指令未命中数（历史推算值） | 277 KB | 465 KB |
| Agent 的 RSS | 5.2 MB | 7.6 MB |
| 首 token 延迟（TTFT） | — | 25–27 ms |
| 请求总耗时 | — | 146–213 ms |
| 独立的模型服务进程 RSS | — | 2.45 GB |
| RTX 4070 Ti 显存 | — | 3.06 GB |

实验报告 5 个请求用了 1.58 秒，并发重叠带来 2.65 倍的加速。旧的 OpenAIProvider 配置和 httplib 传输层的统计，都属于那次实验。当前本地 endpoint 的配置方式，是使用经过校验的描述符和 SDK 运行时选项；参见[示例 31](../examples/31_local_transformer.cpp)。把推理放在独立进程里，可以让模型占用的内存留在 agent 进程之外，但并不保证缓存常驻。

## 历史上的体积与冷启动

x86_64 Linux、GCC 13 下的 Plan & Executor 演示，使用了 `-Os`、静态链接 libstdc++/libgcc、去除无用段和 strip。它用 120 ms 的 sleep 模拟模型的工作，启用了可选的 fan-out 线程池，让一个分支失败，然后恢复运行。

| 指标 | 历史上的 Plan & Executor 演示 |
|---|---:|
| strip 之后的 MinSizeRel 二进制文件 | 1203 KB |
| 峰值 RSS（包含崩溃和恢复） | 2.9 MB |
| 冷启动，跑完两个阶段 | ~720 ms |
| 记录到的动态依赖 | 只有 libc.so.6 |

这些是针对历史构建产物的测量值。它们不能证明当前可以只依赖 libc 部署、冷启动低于 250 ms、可以交叉编译到 musl，也不能证明在某块具体的嵌入式开发板上的容量。

## 复现当前的测量

即使设置了 `NEOGRAPH_BUILD_LLM=OFF`，仍然需要外部的 `SchemaProvider::runtime`。把 `SDK_PREFIX` 设为它的安装前缀，或者使用 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`，并准备好 SDK 的开发依赖。请通过 CMake 目标来传递链接依赖；旧的直接链接 core/yyjson 归档的命令，会漏掉这个运行时。下面测量的是当前的引擎基准测试，它并不能原样复现历史上的并发扫描。

NeoGraph 需要 CMake 3.20 及以上。没有显式指定 SDK 源码，也没有已安装的包时，配置阶段默认会下载固定了修订版本的公开 SDK 归档。在离线环境下用已安装的 SDK 测量，除了前缀，还要设置 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`。下载 SDK 并不能免去 libcurl/OpenSSL 的开发环境前提。

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_BENCHMARKS=ON -DCMAKE_PREFIX_PATH="$SDK_PREFIX"
cmake --build build-release --target bench_neograph -j
./build-release/bench_neograph
valgrind --tool=cachegrind --cache-sim=yes \
  --I1=32768,8,64 --D1=32768,8,64 --LL=33554432,16,64 \
  ./build-release/bench_neograph
```

请对生成的产物用 `ldd`/`otool` 或平台自带的加载器清单检查实际的依赖，而不要默认沿用旧的依赖列表。带类型的 Python 绑定的性能需要针对重新构建的 wheel 单独测量。
