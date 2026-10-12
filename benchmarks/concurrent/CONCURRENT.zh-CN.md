<!-- neograph-i18n: source=benchmarks/concurrent/CONCURRENT.md locale=zh-CN source_sha256=acac6c40df2f3cdd8b1d1d39bd54572c68b5e97122c7b4c5ffc208dbf36a3c5b -->
# 并发负载基准测试：NeoGraph 与 Python 的历史结果

**Languages:** [English](CONCURRENT.md) | [한국어](CONCURRENT.ko.md) | [日本語](CONCURRENT.ja.md) | [简体中文](CONCURRENT.zh-CN.md)

本页保留 2026 年 4 月的计数器链对比。它没有用当前的 SchemaProvider SDK 或 Python 绑定重新运行过。“NeoGraph 3.0”是记录在案的 cohort 标签，不是当前的包版本。

## 工作负载与计时

三个节点依次对一个 overwrite 计数器通道做递增（`a → b → c`），没有模型调用、sleep、网络 I/O 或检查点存储。测试矩阵在 1 CPU / 512 MB 和 2 CPU / 1 GB 两种 Docker 配置下，提交 N ∈ {10, 100, 1000, 10000} 个请求，交换空间的上限与内存上限相同。

NeoGraph 在调用方一侧使用 `asio::thread_pool`，其大小为 `max(hardware_concurrency(), 1)`。每个请求的计时从 worker 内部开始：P50/P99 不包含在调用方队列中的等待时间。`total_wall_ms` 包含提交过程和整批请求的全部处理。不要把微秒级的 P99 直接与端到端的服务 SLO 比较。

历史上的 Python 一方，使用 LangGraph 1.1.9、Haystack 2.27.0、pydantic-graph 1.84.1、LlamaIndex Workflow 0.14.20 和 AutoGen GraphFlow 0.7.5，分别在 asyncio 和 multiprocessing 两种模式下运行。这些版本描述的是旧的 cohort，不是当前 Docker 依赖解析的结果。

## 历史结果：1 CPU / 512 MB

图表和表格保留的是：2026-04-22 报告的 NeoGraph 结果，以及 2026-04-19 报告的 Python 结果。N=10,000 的表格只涉及引擎本身，不含 provider 的传输和推理。缺失的指标仍然缺失。

![吞吐量：每秒请求数](../../docs/images/bench-concurrent-throughput.png)

![尾部延迟：每个请求的 P99](../../docs/images/bench-concurrent-latency.png)

![峰值常驻内存](../../docs/images/bench-concurrent-rss.png)

| N | 引擎 + 模式 | 总耗时 | P50 | P99 | 峰值 RSS | 成功 / 错误 |
|---|---------------|------|-----|-----|----------|---------|
| 10,000 | NeoGraph 3.0（历史标签） | 52 ms | 4 µs | 7 µs | 5.5 MB | 10000 / 0 |
| 10,000 | LangGraph asyncio | 23.4 s | 20.2 s | 23.0 s | 416.2 MB | 10000 / 0 |
| 10,000 | LangGraph mp-pool-7 | 8.0 s | 737 µs | 88.4 ms | 60.3 MB | 10000 / 0 |
| 10,000 | Haystack asyncio | 3.1 s | 1.7 s | 2.9 s | 130.7 MB | 10000 / 0 |
| 10,000 | Haystack mp-pool-7 | 2.9 s | 167 µs | 84.7 ms | 68.1 MB | 10000 / 0 |
| 10,000 | pydantic-graph asyncio | 886 ms | 71 µs | 158 µs | 42.6 MB | 10000 / 0 |
| 10,000 | pydantic-graph mp-pool-7 | 2.8 s | 253 µs | 83.8 ms | 36.7 MB | 10000 / 0 |
| 10,000 | LlamaIndex asyncio | 因 OOM 被杀 | — | — | — | — |
| 10,000 | LlamaIndex mp-pool-7 | 6.6 s | — | — | 102.5 MB | 0 / 10000 |
| 10,000 | AutoGen asyncio | 因 OOM 被杀 | — | — | — | — |
| 10,000 | AutoGen mp-pool-7 | 46.8 s | 4.6 ms | 97.1 ms | 49.1 MB | 10000 / 0 |

完整的矩阵归档在 [`results.jsonl`](results.jsonl) 中。记录下来的 LlamaIndex 和 AutoGen 的 asyncio 单元格，被归类为因 OOM 被杀；这一行里所有 LlamaIndex 的 multiprocessing 调用都失败了。这些观察结果并不能确立各框架普遍适用的极限，也不能诊断每一次失败的原因。

## 解读的边界

启用 GIL 的 CPython 会把 Python 字节码串行化，但事件循环的调度、框架自身的工作、进程间序列化和 worker 数量，同样会影响吞吐量。这个基准测试没有把其中某一个原因单独分离出来，不能确立适用于一切场景的 asyncio 上限，也无法预测无 GIL（free-threaded）Python 的性能。

Docker 的 CPU 配额不一定会改变 `hardware_concurrency()` 看到的核数。请记录调用方线程池的大小和宿主机的拓扑。峰值 RSS 取自 Linux 的 `/proc/self/status`；在不支持的平台上为零，表示没有测到。解读 multiprocessing 的内存数字时，要结合每个 runner 的统计口径。由这张表不能推出 256 MB 的实验结果、裸机上的推算、持久化的对比，或远程 LLM 的容量保证。

## 当前的依赖与复现状态

即使设置了 `NEOGRAPH_BUILD_LLM=OFF` 和 `NEOGRAPH_USE_LIBCURL=OFF`，Core 仍然链接外部的 `SchemaProvider::runtime`。可以通过 `CMAKE_PREFIX_PATH` 提供已安装的 SDK，或者通过 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` 指定一份显式的检出；参见[构建指南](../../README.md)。从源码构建 SDK 需要 C++20、用于生成配置的 Python、libcurl ≥7.88 和 OpenSSL Crypto。SDK 只在 Linux/POSIX 上测试过；旧的 Docker 结果不适用于其他平台。
NeoGraph `0.13.1` 当前的构建方法，需要 alpha 版 SDK `0.1.1`、接口修订版/共享库
代数 4，以及匹配的头文件和库。

仍在维护的 NeoGraph Docker 镜像，会安装 curl/OpenSSL 的开发依赖，并构建一个链接 `neograph::core` 的私有 CMake 使用方，它会继承 SDK 的整个依赖闭包。获取 SDK 的方式遵循根目录 CMake 的策略；既没有提供包、也没有提供源码检出时，可能需要联网。NeoGraph 可选的网络模块是关闭的，但 SDK 的依赖依然存在。测试矩阵会在构建之前清空它的输出文件，所以请选一个新的路径。`status=ok` 只表示 JSON 提取成功；还要检查 `ok`、`err` 和退出状态。

```bash
# From the repository root. Docker builds use the root SDK acquisition policy.
docker build -t ng-concurrent -f benchmarks/concurrent/Dockerfile.neograph .
docker run --rm --cpus=1 --memory=512m --memory-swap=512m ng-concurrent 10000

# Full matrix; a NEW path preserves the archived results.jsonl.
bash benchmarks/concurrent/run_matrix.sh benchmarks/concurrent/results-new.jsonl

# Render the archived default results.jsonl, not the new output.
node benchmarks/render_concurrent.js
```

下面的 JSON 只用来说明字段的形态，不是额外的实测结果。

```json
{"engine":"neograph","mode":"threadpool","concurrency":10000,
 "total_wall_ms":6,"p50_us":2,"p95_us":3,"p99_us":6,
 "ok":10000,"err":0,"peak_rss_kb":7808}
```
