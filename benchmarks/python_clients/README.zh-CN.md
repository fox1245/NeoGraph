<!-- neograph-i18n: source=benchmarks/python_clients/README.md locale=zh-CN source_sha256=6d8012ba0393efa7a76c1905fcab2a826c43a0cd2d8287940fcb28ec62dc25b4 -->
# Python 客户端开销 — NeoGraph bindings 对比标准 SDK

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

同一个 workload 分别通过 `neograph_engine`（经 pybind11 暴露的 C++ 引擎）和官方 Python SDKs 运行。C++ engine bench 显示在 engine-overhead 上有 100×-600× 优势（见 [`benchmarks/`](../README.md)）；本目录回答一个更窄的问题：当用户处在 Python 中时，这些优势是否能跨过 binding 边界留下来？

方法：进程内 Python mock server 在 <1 ms 内返回 canned responses，因此 server-side time 是平坦常数。delta 完全来自 client-side — JSON build、HTTP、parse。

## 顺序开销（K=1）

`bench_a2a_clients.py`：

| 客户端 | 中位数 | p95 | 吞吐量 |
|---|---:|---:|---:|
| `neograph_engine.a2a.A2AClient` | **1,137 µs** | 1,381 µs | **860 req/s** |
| `a2a-sdk` 1.0.2 | 2,196 µs | 2,746 µs | 444 req/s |

→ NeoGraph **快 1.93×**。

`bench_openai_clients.py`：

| 客户端 | 中位数 | p95 | 吞吐量 |
|---|---:|---:|---:|
| `neograph_engine.llm.OpenAIProvider` | **1,252 µs** | 1,423 µs | **789 req/s** |
| `openai` 2.33 | 1,927 µs | 2,393 µs | 509 req/s |

→ NeoGraph **快 1.54×**。

因此，OpenAI-call-inside-A2A 的组合往返通过 NeoGraph binding 比通过 SDK stack **快约 ~3×** — 由于每一层彼此独立，优势会叠加。

## 并发吞吐量

`bench_concurrent.py`，K = 1/4/16/64 个 in-flight requests，总数 500：

|   K | NeoGraph req/s | a2a-sdk req/s | speedup |
|----:|---------------:|--------------:|--------:|
|   1 |            881 |           448 |  1.97×  |
|   4 |        **1,461** |           446 |  **3.28×**  |
|  16 |            403 |           390 |  1.03×  |
|  64 |            343 |           275 |  1.25×  |

K=4 行是最干净的读数：NeoGraph 的 pybind11 wrapper 在 C++ HTTP exchange 期间释放 GIL，因此 `ThreadPoolExecutor` 几乎线性扩展。`a2a-sdk` 是 asyncio-native，使用一个 event loop — 增加更多 in-flight requests 并不能帮助它在 Python 内部支付的 per-request serialization cost。

（K=16+ 二者都会下降，因为 mock server 的 `http.server.ThreadingHTTPServer` 在并发线程约 ~400 r/s 处饱和 — 这是 Python stdlib 限制，不是 client 属性。根据 engine bench，NeoGraph A2A C++ server 可处理 200 个并发运行而毫不吃力，但上面的 client-side 数字本身已经成立。）

## 这意味着什么

- **Sub-µs engine win 无法跨过 binding 边界保留下来**，但 **2–3× client win 可以。** 每个请求的开销来自 GIL release、HTTP plumbing 和 JSON parsing — native client 都比 httpx + pydantic 更快。
- 对真实 LLM workloads（每次调用 300+ ms）来说，client overhead 在噪声中 — 但以高 RPS 调用 fast endpoint（mock test、internal services、agent fan-out、multi-shot routing）时，它会主导 wall time。
- **Concurrent threads 可以扩展。** `send_message` / `complete()` 上的 GIL release 允许你运行真实的 ThreadPoolExecutor，而不会撞上 Python parallelism wall。当一个 client fan out 到 N 个 agents 时很有用。

## 复现

```bash
pip install neograph-engine==0.2.2 a2a-sdk openai httpx
python bench_a2a_clients.py 500       # sequential A2A
python bench_openai_clients.py 500    # sequential OpenAI
python bench_concurrent.py 500        # concurrent A2A
```

上面的数字于 2026-04-29 在 x86_64 Ubuntu 24.04（WSL2）、Python 3.12.3 上测得，使用本目录中的进程内 mock servers。结果可在 ±5% 以内复现。
