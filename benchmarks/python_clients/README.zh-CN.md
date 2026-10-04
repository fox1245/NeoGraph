<!-- neograph-i18n: source=benchmarks/python_clients/README.md locale=zh-CN source_sha256=6919a37bc310f6105ba5ef408675a5d982489430bc04ac6c565fdb7c66a4e6a1 -->
# Python 客户端开销：当前运行器与历史结果

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

本目录通过本地进程内协议服务器比较 NeoGraph Python 绑定和 Python SDK。时间包含客户端处理、服务器调度和 HTTP 交换，服务器开销不是已证明的常量。这里不运行真实模型。表格保留 x86_64 Ubuntu 24.04（WSL2）、Python 3.12.3 上 2026-04-29 的历史测量，不是更新后的 SDK cutover 结果。

## 历史顺序开销：K=1

`bench_a2a_clients.py` 的本地固定 A2A 响应记录，中位数比值为 1.93×。

| Client (2026-04-29) | Median | P95 | Throughput |
|---|---:|---:|---:|
| `neograph_engine.a2a.A2AClient` | 1,137 µs | 1,381 µs | 860 req/s |
| `a2a-sdk` 1.0.2 | 2,196 µs | 2,746 µs | 444 req/s |

`bench_openai_clients.py` 的本地固定 Chat 响应记录，中位数比值为 1.54×。保留旧供应商名是为了标识历史实现，不是当前导入方式。

| Client (2026-04-29, legacy provider) | Median | P95 | Throughput |
|---|---:|---:|---:|
| `neograph_engine.llm.OpenAIProvider` (removed) | 1,252 µs | 1,423 µs | 789 req/s |
| `openai` 2.33 | 1,927 µs | 2,393 µs | 509 req/s |

## 历史并发吞吐量

`bench_concurrent.py` 的本地 A2A 服务器，K ∈ {1, 4, 16, 64}，每行 500 请求。

| K (2026-04-29, A2A) | NeoGraph req/s | a2a-sdk req/s | Ratio |
|----:|---------------:|--------------:|--------:|
| 1 | 881 | 448 | 1.97× |
| 4 | 1,461 | 446 | 3.28× |
| 16 | 403 | 390 | 1.03× |
| 64 | 343 | 275 | 1.25× |

K=16/64 的下降包含 `ThreadingHTTPServer` 与客户端的交互，不能隔离标准库上限或证明通用 asyncio 限制。K=4 也不证明近线性扩展，只报告一个配置。

## 当前 API 与依赖

当前 OpenAI 比较从已准入 HTTP Chat descriptor 和运行时选项构造 `SchemaProvider`，使用 `make_provider_request` 和 `invoke`。它消费真实 typed 所有权 outcome，并检查失败/完成和可见文本。`OpenAIProvider`、`CompletionParams`、`complete()` 已移除。模型在请求中显式指定；未提供 controls 时使用 typed factory 默认值，不是旧供应商构造器默认值。

双方使用相同私有 HTTP loopback 服务器和 Chat 路径，固定响应文本为 `ok`。服务器检查路径、模型和提示。不需要远程凭据、自定义 CA 或付费调用。这是 HTTP/1.0 本地协议负载，不是 TLS/HTTP2 验证或 native replay 基准。固定 token usage 是供应商报告 fixture 数据，不是实际测量 token 或预算计费量。

按[Python 绑定指南](../../docs/python-binding.md)安装与当前源码匹配的 wheel，再安装以下比较 SDK。Core 源码也依赖外部 `SchemaProvider::runtime`，参见[构建指南](../../README.md)。Python 包装层无法给旧 wheel 添加 typed native API。本页不声称新 wheel 已验证或新基准已通过。
NeoGraph `0.13.0` 的 wheel 和 native consumer 必须匹配 alpha SDK `0.1.0`，interface revision/shared generation 4。当前集成验证尚未完成。

## 运行新批次

从仓库根目录运行，记录 wheel/源码/SDK 修订、Python 构建与 GIL 模式、比较包版本、平台、服务器协议、预热、迭代和失败数。默认 500 请求，测量前预热；并发运行器扫描 K=1/4/16/64。

```bash
# First install a wheel matching this source via docs/python-binding.md.
python -m pip install a2a-sdk openai httpx
python benchmarks/python_clients/bench_a2a_clients.py 500
python benchmarks/python_clients/bench_openai_clients.py 500
python benchmarks/python_clients/bench_concurrent.py 500
```

将历史表格与新输出分开。不能把各层比值相乘得到 OpenAI-inside-A2A 端到端加速，因为这里未测量组合负载。当前实现没有通用 2–3× 保证或 ±5% 可复现性声明。
