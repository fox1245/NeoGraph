<!-- neograph-i18n: source=benchmarks/concurrent/CONCURRENT.md locale=zh-CN source_sha256=5331369d595d05bf67c0748516d7da5980b3dfa70262e2df9c01849f90564b58 -->
# 并发负载基准测试 — NeoGraph 对比 Python 图框架

**Languages:** [English](CONCURRENT.md) | [한국어](CONCURRENT.ko.md) | [日本語](CONCURRENT.ja.md) | [简体中文](CONCURRENT.zh-CN.md)

在 **burst** 负载下 — 同时提交 N 个请求，然后测试等待全部完成 — 这些引擎如何扩展？每种方法在什么点开始不再可用？

这个 bench 在可复现的 Docker sandbox 中回答这个问题，CPU 和内存限制匹配 SBC 级目标，并覆盖六个框架。

## 设置

- **Workload**：3 节点顺序 counter 链（`a → b → c`），每个节点递增单个状态通道。没有 I/O、没有 sleep、没有 LLM。
- **Burst pattern**：N 个任务在 t=0 提交；runner 等待全部完成；每个请求的延迟在进程内采集。
- **Sandbox**：Docker 使用 `--cpus` 和 `--memory`（并匹配 `--memory-swap`，让 OOM 发生而不是 swapping）。矩阵：
  - Profile **1 CPU / 512 MB** — "tight SBC" 目标（主要）
  - Profile **2 CPU / 1 GB** — "comfortable SBC" 目标
- **Concurrencies**：N ∈ {10, 100, 1000, 10000}
- **被测引擎**：
  - `neograph` (3.0) — 调用方侧的 `asio::thread_pool`，使用 `hardware_concurrency()` 个 worker 调度 `engine->run()`。引擎自身默认在单线程 `run_sync` 上驱动 super-step loop；每次调用使用自己的 io_context，因此调度受调用方池限制，而不是受引擎限制。
  - `langgraph-asyncio` / `langgraph-mp` — LangGraph 1.1.9，分别在 `asyncio.gather` / `multiprocessing.Pool` 下运行。
  - `haystack-asyncio` / `haystack-mp` — Haystack 2.27.0。Pipeline.run() 是 sync；asyncio 模式用 `asyncio.to_thread` 包装。
  - `pydantic-asyncio` / `pydantic-mp` — pydantic-graph 1.84.1，原生 async。
  - `llamaindex-asyncio` / `llamaindex-mp` — LlamaIndex Workflow 0.14.20，每次运行一个全新的 Workflow（每次运行一个 event bus）。
  - `autogen-asyncio` / `autogen-mp` — AutoGen GraphFlow 0.7.5，每次运行一个全新的 flow（flow state 不是并发安全的）。

## 结果 — 1 CPU / 512 MB profile（asyncio 模式）

![Throughput — requests per second](../../docs/images/bench-concurrent-throughput.png)

![Tail latency — P99 per request](../../docs/images/bench-concurrent-latency.png)

![Peak resident memory](../../docs/images/bench-concurrent-rss.png)

图表跟踪六个引擎的 asyncio-mode 结果。mp（multiprocessing）行在下面的 raw-numbers 表中 — mp 通过 N 个 worker 进程绕过 GIL，但在 pool size 处饱和，而且这个模式在每个 Python 框架中都相同。

### 原始数据（1 CPU / 512 MB, NeoGraph 2026-04-22 on 3.0, Python 端 2026-04-19）

包含 2 CPU / 1 GB profile 的完整矩阵在 [`results.jsonl`](results.jsonl)。N=10,000 是最能说明问题的一行：

| N | 引擎 + 模式 | 总耗时 | P50 | P99 | 峰值 RSS | 成功 / 错误 |
|---|---------------|------|-----|-----|----------|---------|
| **10,000** | **NeoGraph 3.0** | **52 ms** | **4 µs** | **7 µs** | **5.5 MB** | 10000 / 0 |
| 10,000 | LangGraph asyncio | 23.4 s | 20.2 s | **23.0 s** | 416.2 MB | 10000 / 0 |
| 10,000 | LangGraph mp-pool-7 | 8.0 s | 737 µs | 88.4 ms | 60.3 MB | 10000 / 0 |
| 10,000 | Haystack asyncio | 3.1 s | 1.7 s | 2.9 s | 130.7 MB | 10000 / 0 |
| 10,000 | Haystack mp-pool-7 | 2.9 s | 167 µs | 84.7 ms | 68.1 MB | 10000 / 0 |
| 10,000 | pydantic-graph asyncio | 886 ms | 71 µs | **158 µs** | 42.6 MB | 10000 / 0 |
| 10,000 | pydantic-graph mp-pool-7 | 2.8 s | 253 µs | 83.8 ms | 36.7 MB | 10000 / 0 |
| 10,000 | **LlamaIndex asyncio** | **因 OOM 终止** | — | — | — | — |
| 10,000 | LlamaIndex mp-pool-7 | 6.6 s | — | — | 102.5 MB | **0 / 10000** |
| 10,000 | **AutoGen asyncio** | **因 OOM 终止** | — | — | — | — |
| 10,000 | AutoGen mp-pool-7 | 46.8 s | 4.6 ms | 97.1 ms | 49.1 MB | 10000 / 0 |

两个引擎在 N=10,000 时非优雅地退出 512 MB sandbox：

* **LlamaIndex asyncio** — 因 OOM 终止。每个 in-flight workflow 持有一个每次运行的 event bus + channel runtime；1 万个超过了 wall-clock 完成前的 cgroup 上限。
* **AutoGen asyncio** — 因 OOM 终止。1 万个并发 GraphFlow 实例及其 participant state 触发了同样的上限。
* **LlamaIndex mp-pool** — 全部 1 万个 worker 调用失败。Workflow 实例在 worker-process fork 之间不是 pickle-safe；无论 N 为多少都会失败。

两个 profile 的完整 raw matrix 在 [`results.jsonl`](results.jsonl)（每个 cell 一行 JSON）。

## 解读

### 吞吐量：NeoGraph 可扩展，每个 Python asyncio runtime 都出现平台效应

NeoGraph 的绿色曲线在整个 sweep 中保持在 22-25K req/s 范围，而每条 Python asyncio 曲线都会退化。调用方侧的 `asio::thread_pool` 把 `engine->run()` 调用调度到所有可用核心；每个调用随后通过 `run_sync` 驱动自己的单线程 super-step loop — cgroup 的 CPU quota 限制 wall time，但不限制线程数，因此短任务能在调用方池中干净地交错执行。

**每条 Python asyncio 曲线都会 plateau 或退化。** 根因在 LangGraph、Haystack、pydantic-graph、LlamaIndex 和 AutoGen 中都相同：一个进程里的一个 event loop，GIL 会串行化每个 coroutine 必须做的 CPU 工作。N 个 coroutines → 串行执行 → throughput 不随 N 扩展。

每个框架的 mp-pool 模式通过 `os.cpu_count()` 个 worker 进程绕过 GIL — 但在该 pool size 处饱和，并且每个任务都支付 fork + pickle 开销。超过约 N=1000 后，pool 已饱和，throughput 无论框架如何都会 plateau。

### 尾部延迟：普遍的 GIL 上限

在 N=10,000 时，NeoGraph 的 P99 保持在微秒级。每个 Python asyncio 的 P99 都随 N 线性上升 — 因为 GIL 队列中的 *最后一个* coroutine 要等整个运行完成才轮到自己的 slot。

这不是 LangGraph 特有的问题。Haystack（用 `to_thread` 包装 sync pipeline）、LlamaIndex（async event-driven workflow）、pydantic-graph（async state machine）和 AutoGen（async multi-agent runtime）都出现完全相同的形状。如果你在单进程后面放一个 Python orchestration framework，GIL 就是上限。

对于任何带 P99 SLO 预期的现实服务器（例如"99% 请求低于 1 秒"），每个 asyncio-backed engine 都会在某个 N 处破裂。确切断点因框架而异 — 较轻的 runtime（pydantic-graph、LangGraph）晚些破裂，较重的（LlamaIndex、AutoGen）早得多 — 但都会破裂。

### 内存：asyncio 的 RSS 随持有的协程栈增长

- **NeoGraph 3.0** 在整个 sweep 中保持在 4.2–5.5 MB。任务立即返回；常驻的只有调用方侧 `asio::thread_pool` 和每个 in-flight `run()` 的一个 io_context。
- **mp-pool modes** 在各框架中保持在约 60–80 MB — worker pool size 占主导；任务不会积累，因为它们逐个被调度和返回。
- **asyncio modes** 随 N 线性增长。每个 pending coroutine 都持有 Python stack frame、closure state 和框架的 per-run state。1 万个 in-flight coroutines 对较重 runtime 来说会累计到数百 MB。

在我们的 512 MB memory budget 下，一些 asyncio 运行在 N=10,000 时被挤到接近 cgroup 上限。若 cgroup 更紧，为 256 MB，较重框架会在 N=1,000 和 N=10,000 之间的某处被 OOM-killed。NeoGraph 在该预算下仍有约 500 MB headroom。

## 此基准未说明的内容

- **不能证明任何框架在规模上"crashes"。** 这里的故事是优雅退化到不可用延迟，而不是进程死亡。在更紧的 cgroup 或更高 N 下，OOM kills 确实会成为退出模式 — 但这里没有记录它，需要后续实验。
- **不建模 LLM I/O。** 真实 agent workload 每次 LLM 调用有 100–1,000 ms 延迟。绝对时间上该延迟远大于引擎差距 — 但在容量维度上并非如此：如果你的引擎只能通过 runtime 推 1,000 req/s，再多并发 LLM I/O 也帮不上忙。
- **不覆盖 persistence。** 每个框架都关闭 checkpointing。启用后，比较会转向 store implementation，那是另一个 benchmark。
- **Workload-shape bias。** counter chain 在状态语义上是 NeoGraph-native；Haystack 用 `to_thread` 包装其 sync pipeline，AutoGen 把 counter 编码成消息内容，pydantic-graph 没有 fan-out（本 bench 未使用，但对带分支的 burst workloads 相关）。每个框架都被要求做自己的工作，而不是做自己的最佳场景。
- **Docker on WSL2。** `--cpus` 强制 CPU quota，但不限制可见 core count，因此 NeoGraph 的 `hardware_concurrency()` 仍返回 host count。裸机结果方向应相同，但 NeoGraph 一端会更紧（线程更少、context-switch noise 更少）。

## 复现

```bash
# From the repo root.

# Build images once:
docker build -t ng-concurrent -f benchmarks/concurrent/Dockerfile.neograph .
docker build -t lg-concurrent -f benchmarks/concurrent/Dockerfile.langgraph .
docker build -t hs-concurrent -f benchmarks/concurrent/Dockerfile.haystack .
docker build -t pg-concurrent -f benchmarks/concurrent/Dockerfile.pydantic_graph .
docker build -t li-concurrent -f benchmarks/concurrent/Dockerfile.llamaindex .
docker build -t ag-concurrent -f benchmarks/concurrent/Dockerfile.autogen .

# Full matrix (88 cells across 6 engines × 2 modes × 4 concurrencies × 2 profiles,
# excluding neograph which has no mode split):
bash benchmarks/concurrent/run_matrix.sh

# Re-render charts from the results:
node benchmarks/render_concurrent.js
```

单元格调试运行：

```bash
docker run --rm --cpus=1 --memory=512m --memory-swap=512m \
    ng-concurrent 10000

docker run --rm --cpus=1 --memory=512m --memory-swap=512m \
    li-concurrent async 10000
```

每个 container 打印一行 JSON，形状如下：

```json
{"engine":"neograph","mode":"threadpool","concurrency":10000,
 "total_wall_ms":6,"p50_us":2,"p95_us":3,"p99_us":6,
 "ok":10000,"err":0,"peak_rss_kb":7808}
```
