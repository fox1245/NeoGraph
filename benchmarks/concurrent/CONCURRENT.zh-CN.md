<!-- neograph-i18n: source=benchmarks/concurrent/CONCURRENT.md locale=zh-CN source_sha256=d1bb14dd5c317c6fc2c6785110f99b2a224b80909a7875d413374ec3bb854f27 -->
# 并发负载基准：NeoGraph 与 Python 的历史结果

**Languages:** [English](CONCURRENT.md) | [한국어](CONCURRENT.ko.md) | [日本語](CONCURRENT.ja.md) | [简体中文](CONCURRENT.zh-CN.md)

本页保留 2026 年 4 月的计数器链比较，未使用当前 SchemaProvider SDK 或 Python 绑定重新运行。“NeoGraph 3.0”是当时的记录标签，不是当前包版本。

## 工作负载与计时边界

三个节点递增 overwrite 计数器通道（`a → b → c`），没有模型调用、休眠、网络 I/O 或检查点存储。矩阵在 Docker 的 1 CPU / 512 MB 和 2 CPU / 1 GB 配置下提交 N ∈ {10, 100, 1000, 10000} 次运行，并使交换空间上限等于内存上限。

NeoGraph 使用大小为 `max(hardware_concurrency(), 1)` 的调用方 `asio::thread_pool`。请求计时从工作线程内部开始，P50/P99 不含调用方队列等待时间。`total_wall_ms` 包含提交到全部完成的时间。不能把微秒级 P99 直接与服务器端到端 SLO 比较。

历史 Python 对比使用 LangGraph 1.1.9、Haystack 2.27.0、pydantic-graph 1.84.1、LlamaIndex Workflow 0.14.20 和 AutoGen GraphFlow 0.7.5 的 asyncio 与 multiprocessing 模式。版本描述历史批次，不代表当前 Docker 依赖解析结果。

## 历史结果：1 CPU / 512 MB

图表保留 NeoGraph 2026-04-22 和 Python 2026-04-19 的记录。N=10,000 表只测量引擎，不测量供应商传输或推理；缺失指标仍保留为缺失。

![Throughput — requests per second](../../docs/images/bench-concurrent-throughput.png)

![Tail latency — P99 per request](../../docs/images/bench-concurrent-latency.png)

![Peak resident memory](../../docs/images/bench-concurrent-rss.png)

| N | Engine + mode | Wall | P50 | P99 | Peak RSS | OK / Err |
|---|---------------|------|-----|-----|----------|---------|
| 10,000 | NeoGraph 3.0 (historical label) | 52 ms | 4 µs | 7 µs | 5.5 MB | 10000 / 0 |
| 10,000 | LangGraph asyncio | 23.4 s | 20.2 s | 23.0 s | 416.2 MB | 10000 / 0 |
| 10,000 | LangGraph mp-pool-7 | 8.0 s | 737 µs | 88.4 ms | 60.3 MB | 10000 / 0 |
| 10,000 | Haystack asyncio | 3.1 s | 1.7 s | 2.9 s | 130.7 MB | 10000 / 0 |
| 10,000 | Haystack mp-pool-7 | 2.9 s | 167 µs | 84.7 ms | 68.1 MB | 10000 / 0 |
| 10,000 | pydantic-graph asyncio | 886 ms | 71 µs | 158 µs | 42.6 MB | 10000 / 0 |
| 10,000 | pydantic-graph mp-pool-7 | 2.8 s | 253 µs | 83.8 ms | 36.7 MB | 10000 / 0 |
| 10,000 | LlamaIndex asyncio | OOM killed | — | — | — | — |
| 10,000 | LlamaIndex mp-pool-7 | 6.6 s | — | — | 102.5 MB | 0 / 10000 |
| 10,000 | AutoGen asyncio | OOM killed | — | — | — | — |
| 10,000 | AutoGen mp-pool-7 | 46.8 s | 4.6 ms | 97.1 ms | 49.1 MB | 10000 / 0 |

完整矩阵保存在 [`results.jsonl`](results.jsonl)。当时 LlamaIndex 和 AutoGen 的 asyncio 单元被归类为 OOM 终止，该行的 LlamaIndex multiprocessing 调用全部失败。这不能确立普遍的框架上限或诊断所有失败原因。

## 解读范围

启用 GIL 的 CPython 会串行执行 Python 字节码，但事件循环、框架处理、进程序列化和工作线程数也影响吞吐量。本比较没有隔离单一原因，不能确立普遍的 asyncio 上限或预测 free-threaded Python 性能。

Docker CPU 配额未必改变 `hardware_concurrency()` 可见的核心数。请记录调用方线程池和主机配置。峰值 RSS 来自 Linux `/proc/self/status`；不支持的平台返回 0 表示无法测量。multiprocessing 内存也需结合各运行器的统计范围解读。本表不能保证 256 MB 实验、裸机表现、持久化比较或远程 LLM 容量。

## 当前依赖与复现状态

即使设置 `NEOGRAPH_BUILD_LLM=OFF` 和 `NEOGRAPH_USE_LIBCURL=OFF`，Core 也链接外部 `SchemaProvider::runtime`。安装 SDK 用 `CMAKE_PREFIX_PATH`，显式源码用 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` 指定；参见[构建指南](../../README.md)。SDK 源码需要 C++20、生成配置用的 Python、libcurl ≥7.88 和 OpenSSL Crypto。目前 SDK 验证范围是 Linux/POSIX，旧 Docker 结果不验证其他平台。
NeoGraph `0.13.0` recipe 需要匹配 alpha SDK `0.1.0`、interface revision/shared generation 4 的 header/library。当前集成验证尚未完成；此前 Linux/POSIX 验证不是 SDK4 或新 Docker 通过记录。

当前 NeoGraph Docker 镜像安装 curl/OpenSSL 开发依赖，构建链接 `neograph::core` 的专用 CMake 消费者，继承完整 SDK 依赖。SDK 获取遵循根 CMake 策略；没有包或显式源码时可能需要网络。可选 NG 网络模块关闭后，SDK 仍必需。本页不声称新 Docker 测量。矩阵在构建前清空输出，请指定新路径。`status=ok` 仅表示提取到 JSON；还需检查 `ok`、`err` 和退出状态。

```bash
# From the repository root. Docker builds use the root SDK acquisition policy.
docker build -t ng-concurrent -f benchmarks/concurrent/Dockerfile.neograph .
docker run --rm --cpus=1 --memory=512m --memory-swap=512m ng-concurrent 10000

# Full matrix; a NEW path preserves the archived results.jsonl.
bash benchmarks/concurrent/run_matrix.sh benchmarks/concurrent/results-new.jsonl

# Render the archived default results.jsonl, not the new output.
node benchmarks/render_concurrent.js
```

以下 JSON 仅展示字段格式，不是新增测量结果。

```json
{"engine":"neograph","mode":"threadpool","concurrency":10000,
 "total_wall_ms":6,"p50_us":2,"p95_us":3,"p99_us":6,
 "ok":10000,"err":0,"peak_rss_kb":7808}
```
