<!-- neograph-i18n: source=benchmarks/concurrent/CONCURRENT.md locale=zh-CN source_sha256=c5618ac13a6472e43cc2a1c6dd605d9f9d85842c5b39a3d5f07d5592047cc3da -->
# 并发负载基准测试：NeoGraph 与 Python 的历史结果

**Languages:** [English](CONCURRENT.md) | [한국어](CONCURRENT.ko.md) | [日本語](CONCURRENT.ja.md) | [简体中文](CONCURRENT.zh-CN.md)

本页保留了 2026 年 4 月的计数器链对比，并缩减为所记录数据能够支持的部分。它没有用当前的 SchemaProvider SDK 或 Python 绑定重新运行，而且此目录中的测试工具此后已被修正（见[撤回与修正](#withdrawn-and-corrected)）：用修正后的工具重新测量尚未进行。“NeoGraph 3.0”是所记录批次的标签，不是当前的软件包版本。

## 工作负载与计时

三个节点依次对一个 overwrite 计数器通道做递增（`a → b → c`），没有模型调用、sleep、网络 I/O 或检查点存储。测试矩阵在 1 CPU / 512 MB 和 2 CPU / 1 GB 两种 Docker 配置下，提交 N ∈ {10, 100, 1000, 10000} 个请求，交换空间的上限与内存上限相同。

NeoGraph 在调用方使用大小为 `max(hardware_concurrency(), 1)` 的 `asio::thread_pool`。工作负载是**封闭的单次突发**：N 个运行一次性全部提交，然后把这一批处理完；不对随时间到达的请求建模。`Wall` 是从第一次提交到最后一次完成的时间。旧的测试工具以整毫秒记录它，因此低于 20 ms 的 NeoGraph 数据点不用于任何图表或比值。旧数据中按请求统计的各列已撤回。

历史上的 Python 一方，使用 LangGraph 1.1.9、Haystack 2.27.0、pydantic-graph 1.84.1、LlamaIndex Workflow 0.14.20 和 AutoGen GraphFlow 0.7.5，分别在 asyncio 和 multiprocessing 两种模式下运行。这些版本描述的是旧的 cohort，不是当前 Docker 依赖解析的结果。

## 历史结果：1 CPU / 512 MB

下面的表格和图表由 `scripts/render_bench_report.py` 根据存档的 [`results.jsonl`](results.jsonl) 生成，只保留墙钟时间一列和单进程内存一列。NeoGraph 的结果报告于 2026-04-22，Python 的结果报告于 2026-04-19。N=10,000 的表格只测量引擎，不涉及 provider 传输或推理。

![吞吐量：每秒墙钟时间内完成的请求数，封闭的单次突发](../../docs/images/bench-concurrent-throughput.svg)

![单进程的峰值常驻内存，封闭的单次突发](../../docs/images/bench-concurrent-rss.svg)

<!-- bench-record:begin concurrent_wall -->
| N | 引擎与模式 | 封闭突发的墙钟时间 | 峰值 RSS，单进程 (MiB) | 成功 / 错误 |
| ---: | :--- | ---: | ---: | ---: |
| 10,000 | NeoGraph threadpool | 52 ms | 5.5 | 10000 / 0 |
| 10,000 | pydantic-graph asyncio | 886 ms | 42.6 | 10000 / 0 |
| 10,000 | pydantic-graph mp-pool-7 | 2.8 s | 已撤回：未统计整个进程树 | 10000 / 0 |
| 10,000 | Haystack mp-pool-7 | 2.9 s | 已撤回：未统计整个进程树 | 10000 / 0 |
| 10,000 | Haystack asyncio | 3.1 s | 130.7 | 10000 / 0 |
| 10,000 | LangGraph mp-pool-7 | 8.0 s | 已撤回：未统计整个进程树 | 10000 / 0 |
| 10,000 | LangGraph asyncio | 23.4 s | 416.2 | 10000 / 0 |
| 10,000 | AutoGen mp-pool-7 | 46.8 s | 已撤回：未统计整个进程树 | 10000 / 0 |
| 10,000 | LlamaIndex asyncio | 因 OOM 被终止 | — | — |
| 10,000 | AutoGen asyncio | 因 OOM 被终止 | — | — |
<!-- bench-record:end concurrent_wall -->

完整的矩阵存档在 [`results.jsonl`](results.jsonl) 中。所记录的 LlamaIndex 和 AutoGen asyncio 单元被归类为被 OOM 终止：一万个同时存在的逐次调用实例放不进 512 MB。这些观察并不能确立适用于所有框架的通用限制。 LangGraph 的 asyncio 行用普通 `def` 节点调用 `ainvoke`，LangGraph 会在线程池上运行它；使用 `async def` 节点则是另一种运行方式（见[引擎开销指南](../README.md)）。

<a id="withdrawn-and-corrected"></a>
## 撤回与修正

本页撤回的内容及各自的原因：

* **P50、P99 两列和延迟图表。** 它们把 NeoGraph、多进程各行以及 pydantic-graph（其运行从不挂起）的工作线程内服务时间，与其他 asyncio 行从任务创建起算的系统内停留时间并排放在一起，因此差距衡量的是框架让出控制权的频繁程度，而不是它有多快。
* **LlamaIndex 多进程各行。** 每次调用都失败了（10,000 次中完成 0 次），因为测试工具在没有运行中的事件循环的地方创建了工作流 handler；6.6 s 和内存数值属于异常处理。
* **多进程的内存数值。** 测试工具只统计了父进程，没有统计工作进程。
* **N=10 和 N=100 时 NeoGraph 的吞吐量数据点。** 墙钟时间被截断为整毫秒记录，渲染器又把它钳制为 1 ms，因此这些点并非测得的值。

此目录中的测试工具现在会为每个单元记录完成数、失败数和对每个结果的检查；有失败的单元其 `status` 为 `error`，没有百分位数，退出状态非 0。它们还记录从同一个提交时刻起算的服务时间与响应时间、以微秒计的墙钟时间，以及多进程的进程树内存峰值。Docker 矩阵尚未用这些工具重新运行：重新测量尚未进行。 Docker 镜像现在固定在 [`requirements-compare.txt`](../requirements-compare.txt) 中的最新发布版；此前固定的 LangGraph 1.1.7 已从 PyPI 撤下（yank）。

## 解读的边界

启用 GIL 的 CPython 会把 Python 字节码串行化，但事件循环的调度、框架自身的工作、进程间序列化和 worker 数量，同样会影响吞吐量。这个基准测试没有把其中某一个原因单独分离出来，不能确立适用于一切场景的 asyncio 上限，也无法预测无 GIL（free-threaded）Python 的性能。

Docker 的 CPU 配额不一定会改变 `hardware_concurrency()` 所见的核数。请记录调用方线程池的大小和主机拓扑。峰值 RSS 取自单个进程的 Linux `/proc/self/status`；在不支持的平台上为 0 表示无法测量。NeoGraph 每个线程池线程最多同时持有一个运行，而 Python asyncio 各行把全部 N 个运行都保持在途，因此内存各列并非同条件的对比。从这张表无法推出 256 MB 实验、裸机推算、持久化对比或远程 LLM 的容量保证。

## 当前的依赖与复现状态

即使设置了 `NEOGRAPH_BUILD_LLM=OFF` 和 `NEOGRAPH_USE_LIBCURL=OFF`，Core 仍然链接外部的 `SchemaProvider::runtime`。可以通过 `CMAKE_PREFIX_PATH` 提供已安装的 SDK，或者通过 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` 指定一份显式的检出；参见[构建指南](../../README.md)。从源码构建 SDK 需要 C++20、用于生成配置的 Python、libcurl ≥7.88 和 OpenSSL Crypto。SDK 只在 Linux/POSIX 上测试过；旧的 Docker 结果不适用于其他平台。
NeoGraph `0.16.0` 当前的构建方法，需要 alpha 版 SDK `0.4.0`、接口修订版/共享库
代数 7，以及匹配的头文件和库。

仍在维护的 NeoGraph Docker 镜像，会安装 curl/OpenSSL 的开发依赖，并构建一个链接 `neograph::core` 的私有 CMake 使用方，它会继承 SDK 的整个依赖闭包。获取 SDK 的方式遵循根目录 CMake 的策略；既没有提供包、也没有提供源码检出时，可能需要联网。NeoGraph 可选的网络模块是关闭的，但 SDK 的依赖依然存在。测试矩阵会在构建之前清空它的输出文件，所以请选一个新的路径。`status=ok` 只表示 JSON 提取成功；还要检查 `ok`、`err` 和退出状态。

```bash
# From the repository root. Docker builds use the root SDK acquisition policy.
docker build -t ng-concurrent -f benchmarks/concurrent/Dockerfile.neograph .
docker run --rm --cpus=1 --memory=512m --memory-swap=512m ng-concurrent 10000

# Full matrix; a NEW path preserves the archived results.jsonl.
bash benchmarks/concurrent/run_matrix.sh benchmarks/concurrent/results-new.jsonl

# Render the tables and charts from the archived results.jsonl, not from the new output.
python3 scripts/render_bench_report.py
```

下面的 JSON 只用来说明字段的形态，不是额外的实测结果。

```json
{"engine":"neograph","mode":"threadpool","concurrency":10000,
 "total_wall_ms":6,"p50_us":2,"p95_us":3,"p99_us":6,
 "ok":10000,"err":0,"peak_rss_kb":7808}
```
