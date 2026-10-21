<!-- neograph-i18n: source=benchmarks/python_clients/README.md locale=zh-CN source_sha256=9eb323da15539ee4bca6ce0c716a1012e6f981bec388df65b8b346e3014d145f -->
# Python 客户端开销：当前的 runner 与历史结果

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

这个目录对比 NeoGraph 的 Python 绑定和各 Python SDK 客户端，对端是本地进程内的协议服务。测得的挂钟时间，包含客户端的工作、服务端的调度和 HTTP 往返；服务端的开销并不是一个已证明的常数。这里不运行真实的模型。表格保留的是 2026-04-29 在 x86_64 Ubuntu 24.04（WSL2）、Python 3.12.3 上的测量值，不是更新后的 SDK 切换之后的测量。

## 历史上的串行开销：K=1

`bench_a2a_clients.py`，本地预置的 A2A 响应；记录到的中位数比值为 1.93×。

| 客户端（2026-04-29） | 中位数 | P95 | 吞吐量 |
|---|---:|---:|---:|
| `neograph_engine.a2a.A2AClient` | 1,137 µs | 1,381 µs | 860 req/s |
| `a2a-sdk` 1.0.2 | 2,196 µs | 2,746 µs | 444 req/s |

`bench_openai_clients.py`，本地预置的 Chat 响应；记录到的中位数比值为 1.54×。保留旧的 NeoGraph provider 标签，是为了标识旧的实现，而不是当前可用的 import。

| 客户端（2026-04-29，旧版 provider） | 中位数 | P95 | 吞吐量 |
|---|---:|---:|---:|
| `neograph_engine.llm.OpenAIProvider`（已移除） | 1,252 µs | 1,423 µs | 789 req/s |
| `openai` 2.33 | 1,927 µs | 2,393 µs | 509 req/s |

## 历史上的并发吞吐量

`bench_concurrent.py`，本地 A2A 对端，K ∈ {1, 4, 16, 64}，每行 500 个请求。

| K（2026-04-29，A2A） | NeoGraph req/s | a2a-sdk req/s | 比值 |
|----:|---------------:|--------------:|--------:|
| 1 | 881 | 448 | 1.97× |
| 4 | 1,461 | 446 | 3.28× |
| 16 | 403 | 390 | 1.03× |
| 64 | 343 | 275 | 1.25× |

K=16/64 时吞吐量的下降，包含了与 `ThreadingHTTPServer` 和各客户端之间相互作用的影响。它并没有把标准库的上限单独分离出来，也不能证明 asyncio 存在普遍的限制。K=4 的结果同样不能说明近似线性的扩展：它只报告了抽样的一种配置。

## 数据中包含的内容

* **模拟对端（mock peer）包含在每一个数值之中。** 两个客户端都与同一进程内的 Python `ThreadingHTTPServer` 通信。该服务器使用 HTTP/1.0（`http.server` 的默认值），因此每个请求都是一个新的 TCP 连接和一个新的服务器线程，并且服务器与客户端共享解释器和 GIL。这部分开销由两个客户端同样承担，没有单独测量，并会稀释所有比值：只比较客户端自身工作的比值会比上面的比值更大。K=16 和 K=64 的行描述的是模拟服务器与客户端合在一起的表现，而不是客户端的扩展性；这张表无法推出任何扩展规律。
* **a2a-sdk 各行使用的是其兼容传输层。** 模拟服务器的 agent card 声明协议版本为 0.3.0，因此 `a2a-sdk` 选择其向后兼容的 JSON-RPC 传输，它会在 1.0 类型与 0.3 类型之间转换每个请求和回复。其原生 1.0 传输没有被测到，NeoGraph 客户端对该 card 也使用 0.3 方言。若对端提供的是 1.0 的 card，就不会触发这种转换。
* **闭环，每个请求一个连接。** 每个客户端在上一个请求返回之后，通过新的连接发送下一个请求，因此这些数值是固定并发度下的服务时间，而不是 keep-alive HTTPS 连接的行为。

## 当前的 API 与依赖

当前的 OpenAI 对比，用已准入的 HTTP Chat 描述符和运行时选项构造 `SchemaProvider`，然后使用 `make_provider_request` 和 `invoke`。它消耗的是真实的、带类型且持有所有权的 outcome，并检查失败/完成状态和可见文本。`OpenAIProvider`、`CompletionParams` 和 `complete()` 都是已移除的 API。请求的模型必须显式给出；控制项使用带类型的工厂默认值，除非另行提供。这些默认值不是旧版 provider 构造函数的默认值。

两个客户端使用同一个私有的 HTTP 回环对端和同一条 Chat 路由，预置的响应文本是 `ok`。对端会检查路由、模型和提示词。不需要托管服务的凭据、自定义 CA 或付费调用。这是一个 HTTP/1.0 的本地协议工作负载，不是对 TLS/HTTP2 传输的资格认证，也不是 native 重放的基准测试。预置的 token 用量，是模拟 provider 上报的测试夹具数据，不是实测的 token，也不是预算的计费。

请按 [Python 绑定指南](../../docs/python-binding.md)安装与当前源码匹配的 wheel，再安装下面用于对比的各 SDK。从源码构建时，即使只用 Core，也需要外部的 `SchemaProvider::runtime`；请按[构建指南](../../README.md)操作。只靠 Python 封装层，无法给旧的 wheel 补上带类型的原生 API。
对于 NeoGraph `0.16.0`，wheel 和原生使用方必须匹配 alpha 版 SDK
`0.4.0`、接口修订版/共享库代数 7。

## 运行新的 cohort

在仓库根目录下运行。要记录 wheel、源码和 SDK 的修订版本、Python 构建（包括 GIL 模式）、对比用各包的版本、平台、对端协议、预热、迭代次数和失败情况。各 runner 默认发送 500 个请求，并在测量之前预热；并发 runner 会依次扫描 K=1/4/16/64。

```bash
# First install a wheel matching this source via docs/python-binding.md.
python -m pip install a2a-sdk openai httpx
python benchmarks/python_clients/bench_a2a_clients.py 500
python benchmarks/python_clients/bench_openai_clients.py 500
python benchmarks/python_clients/bench_concurrent.py 500
```

旧的表格要与新的输出分开保存。各层各自的比值相乘，并不能得到端到端的“A2A 内部使用 OpenAI”的加速比：这里并没有测量过这种组合的工作负载。对于当前实现，也不存在通用的 2–3× 客户端加速保证，或 ±5% 的可复现性承诺。
