<!-- neograph-i18n: source=docs/performance-deep-dive.md locale=zh-CN source_sha256=31ea664b884c612f5b01c1132fe748da0cb0115e2afe5bf203f33e958516c63b -->
# 性能详解

**Languages:** [English](performance-deep-dive.md) | [한국어](performance-deep-dive.ko.md) | [日本語](performance-deep-dive.ja.md) | [简体中文](performance-deep-dive.zh-CN.md)

## 阅读历史测量

下表保留切换前测量，不验证当前 typed provider、外部 runtime 或重建的 Python wheel。新的比较须记录 revision、compiler、configuration、topology、concurrency、endpoint 和计时边界。当前复现指南见 [benchmark](../benchmarks/README.md)。

旧文的 cloud 账单、数百万 worker 和永久 ABI freeze 是从测量外推的预测，不是实测 deployment，也不是当前容量或成本保证。除 package version 外还须固定 wheel hash 和 native dependency；version 字符串本身不使 deployment 可复现。

## engine-only 工作负载，2026 年 4 月

相同 topology，compile 一次，无 model/network/sleep。seq 是三 node chain，par 是五分支加 join。2026-04-22 在 x86_64 Linux 测量；NeoGraph 用 GCC 13 Release `-O3 -DNDEBUG` 十次 median，Python 用 CPython 3.12.3 三次 median。pydantic-graph 的 par 是六 node 串行模拟，不是 native fan-out。

| Framework (2026-04-22) | seq µs | par µs |
|---|---:|---:|
| NeoGraph, then-current master | 5.0 | 11.8 |
| Haystack 2.28.0 | 144.1 | 290.0 |
| pydantic-graph 1.85.1 | 235.9 | 286.1 |
| LangGraph 1.1.9 | 656.7 | 2348.7 |
| LlamaIndex Workflow 0.14.21 | 1780.3 | 4683.5 |
| AutoGen GraphFlow 0.7.5 | 3209.2 | 7292.7 |

当时 warm-up 加 seq 10000/par 5000 次记录 NeoGraph ~0.16 s、4.8 MB，Haystack 2.91 s、80.3 MB，AutoGen 68.29 s、52.4 MB。zero-I/O engine 比较不预测 model-service latency。

## 历史 burst 并发

在一个 CPU、512 MB RAM 的 Docker cgroup 中同时提交 10000 request。Python 用 asyncio。结果属于该 workload/environment，不确立通用 Python/GIL scaling 规律。

| Framework | Wall | P99 | Peak RSS | Result |
|---|---:|---:|---:|---|
| NeoGraph, then-current master | 52 ms | 7 µs | 5.5 MB | 10000 completed |
| pydantic-graph | 886 ms | 158 µs | 42.6 MB | 10000 completed |
| Haystack | 3.1 s | 2.9 s | 130.7 MB | 10000 completed |
| LangGraph | 23.4 s | 23.0 s | 416.2 MB | 10000 completed |
| LlamaIndex | — | — | — | OOM killed |
| AutoGen | — | — | — | OOM killed |

方法和 process-pool 比较见 [concurrent benchmark](../benchmarks/concurrent/CONCURRENT.md)。

## 历史 cache simulation

Cachegrind 模拟 Ryzen 7 5800X cache 层级：L1 instruction/data 各 32 KB、eight-way，last-level 32 MB、sixteen-way，line 64 byte。当时 concurrent benchmark 记录：

| N | Instruction references | Last-level instruction misses | Native p50 |
|---|---:|---:|---:|
| 1 | 5.3 M | 4313 | 17 µs |
| 10 | 5.9 M | 4304 | 16 µs |
| 100 | 11.8 M | 4320 | 6 µs |
| 1000 | 69.7 M | 4327 | 6 µs |
| 10000 | 648 M | 4329 | 5 µs |

约 4330 miss 乘 64 byte 得到 277 KB line-count 估计。simulated miss 本身不证明实际 resident working set、DRAM stall 时间或 native latency 变化的原因。参见 [Cachegrind manual](https://valgrind.org/docs/manual/cg-manual.html)。

## 历史 local model 工作负载

独立 neoclaw 实验使用一个 RTX 4070 Ti、一个 Gemma 4 E2B Q4 GGUF（~1.5 GB weight）、串行 inference 的 shared LocalProvider 和一个 `llm_call` node。wall time 包含 drain 全部 worker；request percentile 不证明 queueing 消失。

| N | Wall s | Throughput rps | p50 ms | p99 ms | Peak RSS MB |
|---|---:|---:|---:|---:|---:|
| 1 | 0.64 | 1.6 | 642 | 642 | 2464 |
| 10 | 0.94 | 10.6 | 184 | 686 | 2529 |
| 100 | 4.81 | 20.8 | 343 | 855 | 2549 |
| 1000 | 44.1 | 22.7 | 347 | 673 | 2564 |
| 5000 | 213.7 | 23.4 | 338 | 657 | 2570 |
| 10000 | 424 | 23.6 | 337 | 648 | 2572 |

source 为 [neoclaw benchmark](https://github.com/fox1245/neoclaw/blob/main/benchmarks/bench_concurrent_workers_local_llm.cpp)。CUDA benchmark 配置属于该 repository，不是当前 NeoGraph provider build。不要从观测 marginal memory 外推未测量的 session 上限。

另一历史实验在 HTTP endpoint 后使用 local Gemma-4 E2B Q4_K_M（4.65 B parameter、2.9 GB GGUF）：

| Metric | Engine-only | Historical local Gemma HTTP |
|---|---:|---:|
| Last-level instruction misses | 4320 | 7262 |
| 64 × simulated LL instruction miss（历史推导值） | 277 KB | 465 KB |
| Agent RSS | 5.2 MB | 7.6 MB |
| TTFT | — | 25–27 ms |
| Total request time | — | 146–213 ms |
| Separate model-server RSS | — | 2.45 GB |
| RTX 4070 Ti VRAM | — | 3.06 GB |

记录五 request 为 1.58 s，overlap speedup 2.65×。旧 OpenAIProvider 配置和 httplib transport accounting 属于该实验。当前 local endpoint 用 validated descriptor 和 SDK runtime option；参见[例 31](../examples/31_local_transformer.cpp)。独立 inference process 将 model allocation 放在 agent process 外，但不保证 cache residency。

## 历史 size 与 cold start

x86_64 Linux GCC 13 Plan & Executor demo 使用 `-Os`、static libstdc++/libgcc、dead-section removal、stripping；用 120 ms sleep 模拟 model 工作，启用可选 fan-out pool，一个 branch 失败后 resume。

| Metric | Historical Plan & Executor demo |
|---|---:|
| Stripped MinSizeRel binary | 1203 KB |
| Peak RSS, crash and resume included | 2.9 MB |
| Cold start through both phases | ~720 ms |
| Recorded dynamic dependencies | libc.so.6 only |

这些是当时 artifact 测量，不证明当前 libc-only deployment、250 ms 以下 cold start、musl cross-compile 或某个 embedded board 的容量。

## 复现当前测量

外部 `SchemaProvider::runtime` 即使在 `NEOGRAPH_BUILD_LLM=OFF` 时仍必需。将 `SDK_PREFIX` 设为 installed prefix，或使用 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` 并提供 SDK 开发依赖。用 CMake target 完成 transitive link；旧 core/yyjson archive 直接 link 遗漏 runtime。以下测量当前 engine benchmark，不精确复现历史 concurrent sweep。

NeoGraph 要求 CMake 3.20+。没有显式 SDK source/installed package 时，配置默认获取 revision-pinned 公开 SDK archive。installed SDK 离线测量除 prefix 外还须设置 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`。fetch 不移除 libcurl/OpenSSL 开发依赖。

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_BENCHMARKS=ON -DCMAKE_PREFIX_PATH="$SDK_PREFIX"
cmake --build build-release --target bench_neograph -j
./build-release/bench_neograph
valgrind --tool=cachegrind --cache-sim=yes \
  --I1=32768,8,64 --D1=32768,8,64 --LL=33554432,16,64 \
  ./build-release/bench_neograph
```

在生成 artifact 上检查 `ldd`/`otool` 或平台 loader inventory，不假设旧 dependency list。已记录的 interface-3 SDK runtime/archive 验证覆盖 Linux/POSIX，不验证 interface 4；已有 macOS/Windows metadata 和 WASM target 不验证该依赖。typed Python binding 属于本次切换，不是延期；性能须针对重建 wheel 测量。
