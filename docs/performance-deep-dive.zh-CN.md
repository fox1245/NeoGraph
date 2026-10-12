<!-- neograph-i18n: source=docs/performance-deep-dive.md locale=zh-CN source_sha256=2d15a0b62d2881a1880f92dc7b5d24e9e499ffc57b27fd02b0bb365c20ce9225 -->
# 性能深度剖析

**Languages:** [English](performance-deep-dive.md) | [한국어](performance-deep-dive.ko.md) | [日本語](performance-deep-dive.ja.md) | [简体中文](performance-deep-dive.zh-CN.md)

## 如何解读这些历史测量值

下面的表格保留的是切换（cutover）之前的测量值。它们不能证明当前带类型的 provider、它所依赖的外部运行时，或者重新构建的 Python wheel 合格。做任何新的对比时，都要记录修订版本、编译器、配置、拓扑、并发数、endpoint 和计时边界。当前如何复现，见[基准测试](../benchmarks/README.md)。

这些测量值不能用来保证云成本、worker 数量或容量。除了包的版本，还要固定 wheel 的哈希和原生依赖；仅凭一个版本号，不能让部署可复现。

## 仅引擎的工作负载，2026 年 4 月

拓扑一致，只编译一次，没有模型、网络或 sleep：seq 是三个节点的链；par 是五路 fan-out 加 join。这些数据是 2026-04-22 在 x86_64 Linux 上测得的。NeoGraph 使用 GCC 13 的 Release `-O3 -DNDEBUG`，取十次运行的中位数；Python 使用 CPython 3.12.3，取三次运行的中位数。pydantic-graph 的 par 一行是对六个节点的串行模拟，不是原生的 fan-out。

| 框架（2026-04-22） | seq µs | par µs |
|---|---:|---:|
| NeoGraph，当时的 master | 5.0 | 11.8 |
| Haystack 2.28.0 | 144.1 | 290.0 |
| pydantic-graph 1.85.1 | 235.9 | 286.1 |
| LangGraph 1.1.9 | 656.7 | 2348.7 |
| LlamaIndex Workflow 0.14.21 | 1780.3 | 4683.5 |
| AutoGen GraphFlow 0.7.5 | 3209.2 | 7292.7 |

历史上“预热加 10000 次 seq / 5000 次 par”的那次运行，记录的是：NeoGraph 约 0.16 秒、4.8 MB；Haystack 2.91 秒、80.3 MB；AutoGen 68.29 秒、52.4 MB。零 I/O 的引擎对比，不能用来预测模型服务的延迟。

## 历史上的突发并发测量

一次性同时提交 10000 个请求，运行在限制为 1 个 CPU 和 512 MB 内存的 Docker cgroup 里。Python 的各行使用 asyncio。这些结果只属于该工作负载和环境，并不能确立适用于一切场景的 Python/GIL 扩展规律。

| 框架 | 总耗时 | P99 | 峰值 RSS | 结果 |
|---|---:|---:|---:|---|
| NeoGraph，当时的 master | 52 ms | 7 µs | 5.5 MB | 10000 个全部完成 |
| pydantic-graph | 886 ms | 158 µs | 42.6 MB | 10000 个全部完成 |
| Haystack | 3.1 s | 2.9 s | 130.7 MB | 10000 个全部完成 |
| LangGraph | 23.4 s | 23.0 s | 416.2 MB | 10000 个全部完成 |
| LlamaIndex | — | — | — | 因 OOM 被杀 |
| AutoGen | — | — | — | 因 OOM 被杀 |

测量方法以及与进程池的对比，见[并发基准测试](../benchmarks/concurrent/CONCURRENT.md)。

## 历史上的缓存模拟

Cachegrind 模拟了 Ryzen 7 5800X 的缓存层级：32 KB 的 L1 指令/数据缓存，8 路组相联；32 MB 的末级缓存，16 路组相联；缓存行 64 字节。历史上的并发基准测试报告了：

| N | 指令引用数 | 末级缓存指令未命中数 | 原生 p50 |
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
