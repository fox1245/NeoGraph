<!-- neograph-i18n: source=benchmarks/dr_compare/README.md locale=zh-CN source_sha256=54863904c664b90c0e13360fef870ad45a396a3150352de4af85f1084ffff9e6 -->
# dr_compare — NeoGraph 对比 LangGraph 深度研究基准测试

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

同一个 deep-research workflow 的两个实现（router → plan → 通过 Send fan-out 5 个 researchers → synthesize），每个引擎一个。相同 prompts、相同 model、相同 Crawl4AI search、相同 Postgres checkpoint backend（或 in-memory）。差异被隔离到引擎及其 HTTP transport。

## 文件

- `dr_neograph.py` — NeoGraph runner。由 env 驱动的 knobs（见下文）。
- `dr_langgraph.py` — LangGraph 等价实现。Sync `def` nodes + sync `app.invoke()`，以便与 `dr_neograph.py` 对齐。
- `bench.py` — real-LLM harness。Warmup + 交替测量 + percentiles。
- `bench_mock.py` — 使用 mocked LLM 的 engine-throughput harness。Modules 预加载一次，iters 复用已编译 engines。
- `mem_probe.py` — worker scaling 和 concurrent fan-out RSS 比较。
- `mem_prod_stack.py` — production-stack memory 比较。
- `sweep.sh` — 在 `(FANOUT, LLM_MOCK_MS)` variants 上运行 `bench_mock.py`。
- `_run_single.py` — one-shot runner。用于 wire/strace probes。

memory probes 需要 [psutil](https://github.com/giampaolo/psutil)，按项目文档安装：

```sh
python -m pip install psutil
```

## 环境变量 knobs

| 变量 | 默认值 | 用途 |
|---|---|---|
| `LLM_MOCK_MS` | -1 (real) | 用 `time.sleep(MS)` 替换 LLM。>=0 启用 mock。 |
| `MOCK_SEARCH` | "0" | 跳过 Crawl4AI；返回 canned evidence。 |
| `FANOUT` | 5 | researcher Sends 的数量。 |
| `USE_INMEMORY_CP` | "0" | 使用 in-memory checkpoint（忽略 PG_DSN）。 |
| `NG_TRANSPORT` | `ws-responses` | 仅 NG：`ws-responses`（WebSocket Responses）或 `http-chat`（`/v1/chat/completions`）。 |
| `NG_WORKER_COUNT` | "4" | 仅 NG：Send fan-out parallelism 使用的 thread pool。 |

## 发现（2026-04-26）

1. **纯引擎吞吐量（mocked LLM, FANOUT=5）** — NeoGraph 中位数 1.0ms，LangGraph 5.9ms。LLM 成本为零时，NG **快 5.9×**。
2. **真实 LLM 基准测试** — 第一轮 NG p50 23.90s（sd 5.90），LG 21.95s（sd 1.23）。LG 看起来快约 10%。
3. **Wire 诊断** — WSL2 上的 pcap 说了谎（BPF 经 HyperV vswitch 丢弃大多数 packet）。`strace -e trace=connect` 显示 NG 在每次 7 个 LLM 调用的运行中执行 21 个 connect() syscalls — 每次都是新的 TCP+TLS。
4. **根本原因** — `SchemaProvider::complete_async` 使用 free `async::async_post()`（每次调用关闭 socket），而不是已经存在的 `async::ConnPool`（HTTP/1.1 keep-alive）。`run_sync` 每次调用都会丢弃 io_context，使"把 pool 放在 provider 内部"的显然接线不安全 — 但由 provider 拥有的长生命周期后台 io_context 可行。
5. **修复（commit 6da4810 / bc2ab4f）** — SchemaProvider + OpenAIProvider 现在持有自己的 io_context + worker thread + ConnPool。修复后，NG p90 从 35.34s 降到 25.28s（-10s），sd 从 5.90 降到 1.28（稳定性提高 4.6×）。Median 基本不变，因为并行 Send fan-out 在 HTTP/1.1 上仍需要 N 条 TCP conns。
6. **剩余差距** — LG 的 httpx 支持 HTTP/2，可在单个 TCP 上 multiplex N 条 parallel streams。要缩小这个差距，NG 需要添加 HTTP/2 client support（httplib 仅 HTTP/1.1）。
7. **Worker pool 上限** — `set_worker_count(N)` 限制 Python-node fan-out concurrency。Bench code 的 `set_worker_count(4)` 是真实上限；`NG_WORKER_COUNT=50` 会让 NG sync 在 LG asyncio 前面（FANOUT=50, LLM=100ms 时 307ms vs 711ms）。

完整叙述请见 claude memory 中的 `feedback_schema_provider_no_pool.md` 和 `feedback_pybind_worker_ceiling.md`。

## 复现

真实 LLM 基准测试：
```sh
set -a && source ../../.env && set +a
export NEOGRAPH_PG_DSN="postgresql://postgres:test@localhost:5433/neograph"
export CRAWL4AI_URL="http://localhost:11235"
export NG_TRANSPORT=http-chat   # apples-to-apples vs LG (both HTTP)
python bench.py --warmup 2 --iters 5
```

引擎吞吐量 sweep：
```sh
./sweep.sh   # writes /tmp/sweep.log
```

Wire 诊断（当你怀疑 pcap 时，strace 才是 ground truth）：
```sh
strace -f -e trace=connect -o /tmp/ng.log \
    python _run_single.py neograph
grep "connect(" /tmp/ng.log | grep -oE 'sin_addr=inet_addr\("[^"]+"\)' \
    | sort | uniq -c
```
