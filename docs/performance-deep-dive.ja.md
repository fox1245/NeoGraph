<!-- neograph-i18n: source=docs/performance-deep-dive.md locale=ja source_sha256=31ea664b884c612f5b01c1132fe748da0cb0115e2afe5bf203f33e958516c63b -->
# 性能詳細

**Languages:** [English](performance-deep-dive.md) | [한국어](performance-deep-dive.ko.md) | [日本語](performance-deep-dive.ja.md) | [简体中文](performance-deep-dive.zh-CN.md)

## 歴史的測定の読み方

以下の表は移行前測定を保存する。現在の typed provider、外部 runtime、再構築した Python wheel の検証ではない。新比較には revision、compiler、configuration、topology、concurrency、endpoint、計時境界を記録する。現在の再現手順は [benchmark](../benchmarks/README.md) にある。

旧文の cloud 費用、数百万 worker、恒久 ABI freeze は測定からの予測で、実測した deployment ではない。現在の容量・費用保証ではない。package version に加え wheel hash と native dependency を固定する。version 文字列だけでは再現性を得られない。

## engine-only workload、2026 年 4 月

同じ topology を一度 compile し、model/network/sleep なしで実行した。seq は三 node chain、par は五分岐と join。2026-04-22 の x86_64 Linux で、NeoGraph は GCC 13 Release `-O3 -DNDEBUG` の十回 median、Python は CPython 3.12.3 の三回 median。pydantic-graph の par は native fan-out でなく六 node の直列模倣である。

| Framework (2026-04-22) | seq µs | par µs |
|---|---:|---:|
| NeoGraph, then-current master | 5.0 | 11.8 |
| Haystack 2.28.0 | 144.1 | 290.0 |
| pydantic-graph 1.85.1 | 235.9 | 286.1 |
| LangGraph 1.1.9 | 656.7 | 2348.7 |
| LlamaIndex Workflow 0.14.21 | 1780.3 | 4683.5 |
| AutoGen GraphFlow 0.7.5 | 3209.2 | 7292.7 |

当時の warm-up と seq 10000/par 5000 回では NeoGraph ~0.16 s・4.8 MB、Haystack 2.91 s・80.3 MB、AutoGen 68.29 s・52.4 MB を記録した。zero-I/O engine 比較は model-service latency を予測しない。

## 歴史的 burst 並行実行

一 CPU、512 MB RAM の Docker cgroup に 10000 request を同時投入した。Python は asyncio。結果はその workload/environment に属し、普遍的 Python/GIL scaling 法則を示さない。

| Framework | Wall | P99 | Peak RSS | Result |
|---|---:|---:|---:|---|
| NeoGraph, then-current master | 52 ms | 7 µs | 5.5 MB | 10000 completed |
| pydantic-graph | 886 ms | 158 µs | 42.6 MB | 10000 completed |
| Haystack | 3.1 s | 2.9 s | 130.7 MB | 10000 completed |
| LangGraph | 23.4 s | 23.0 s | 416.2 MB | 10000 completed |
| LlamaIndex | — | — | — | OOM killed |
| AutoGen | — | — | — | OOM killed |

手法と process-pool 比較は [concurrent benchmark](../benchmarks/concurrent/CONCURRENT.md) にある。

## 歴史的 cache simulation

Cachegrind は Ryzen 7 5800X の cache 階層を模擬した。L1 instruction/data は各 32 KB・eight-way、last-level は 32 MB・sixteen-way、line は 64 byte。当時の concurrent benchmark は以下を記録した。

| N | Instruction references | Last-level instruction misses | Native p50 |
|---|---:|---:|---:|
| 1 | 5.3 M | 4313 | 17 µs |
| 10 | 5.9 M | 4304 | 16 µs |
| 100 | 11.8 M | 4320 | 6 µs |
| 1000 | 69.7 M | 4327 | 6 µs |
| 10000 | 648 M | 4329 | 5 µs |

約 4330 miss に 64 byte を掛けると 277 KB の line-count 推定になる。simulated miss だけで実際の resident working set、DRAM stall 時間、native latency 変化の原因を証明できない。[Cachegrind manual](https://valgrind.org/docs/manual/cg-manual.html)を参照。

## 歴史的 local model workload

別の neoclaw 実験は RTX 4070 Ti 一つ、Gemma 4 E2B Q4 GGUF 一つ（~1.5 GB weight）、inference を直列化する shared LocalProvider、単一 `llm_call` node を使った。wall time は全 worker の drain を含む。request percentile は queueing が消える証拠ではない。

| N | Wall s | Throughput rps | p50 ms | p99 ms | Peak RSS MB |
|---|---:|---:|---:|---:|---:|
| 1 | 0.64 | 1.6 | 642 | 642 | 2464 |
| 10 | 0.94 | 10.6 | 184 | 686 | 2529 |
| 100 | 4.81 | 20.8 | 343 | 855 | 2549 |
| 1000 | 44.1 | 22.7 | 347 | 673 | 2564 |
| 5000 | 213.7 | 23.4 | 338 | 657 | 2570 |
| 10000 | 424 | 23.6 | 337 | 648 | 2572 |

source は [neoclaw benchmark](https://github.com/fox1245/neoclaw/blob/main/benchmarks/bench_concurrent_workers_local_llm.cpp)。CUDA benchmark 設定はその repository のもので、現在の NeoGraph provider build ではない。観測した marginal memory から未測定 session 上限を外挿しない。

別の歴史的実験では local Gemma-4 E2B Q4_K_M（4.65 B parameter、2.9 GB GGUF）を HTTP endpoint の背後で使った。

| Metric | Engine-only | Historical local Gemma HTTP |
|---|---:|---:|
| Last-level instruction misses | 4320 | 7262 |
| 64 × simulated LL instruction miss（歴史的算出値） | 277 KB | 465 KB |
| Agent RSS | 5.2 MB | 7.6 MB |
| TTFT | — | 25–27 ms |
| Total request time | — | 146–213 ms |
| Separate model-server RSS | — | 2.45 GB |
| RTX 4070 Ti VRAM | — | 3.06 GB |

五 request で 1.58 s、overlap speedup 2.65×を記録した。旧 OpenAIProvider 設定と httplib transport accounting はその実験のものである。現在の local endpoint は validated descriptor と SDK runtime option を使う。[例 31](../examples/31_local_transformer.cpp)を参照。別 inference process は model allocation を agent process の外に置くが、cache residency を保証しない。

## 歴史的 size と cold start

x86_64 Linux GCC 13 の Plan & Executor demo は `-Os`、static libstdc++/libgcc、dead-section removal、stripping を使った。120 ms sleep で model 処理を模擬し、任意の fan-out pool を使い、一 branch を失敗させて resume した。

| Metric | Historical Plan & Executor demo |
|---|---:|
| Stripped MinSizeRel binary | 1203 KB |
| Peak RSS, crash and resume included | 2.9 MB |
| Cold start through both phases | ~720 ms |
| Recorded dynamic dependencies | libc.so.6 only |

当時の artifact 測定であり、現在の libc-only deployment、250 ms 未満 cold start、musl cross-compile、特定 embedded board の容量を証明しない。

## 現在の測定を再現する

外部 `SchemaProvider::runtime` は `NEOGRAPH_BUILD_LLM=OFF` でも必要。`SDK_PREFIX` を installed prefix に設定するか、SDK 開発依存と `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` を使う。transitive link は CMake target で扱う。旧 core/yyjson archive 直接 link は runtime を欠く。以下は現在の engine benchmark で、歴史的 concurrent sweep の厳密な再現ではない。

NeoGraph は CMake 3.20+ を要する。明示 SDK source/installed package がなければ、既定設定は revision-pinned 公開 SDK archive を取得する。installed SDK の offline 測定は prefix に加え `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` を設定する。fetch は libcurl/OpenSSL 開発依存を除かない。

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_BENCHMARKS=ON -DCMAKE_PREFIX_PATH="$SDK_PREFIX"
cmake --build build-release --target bench_neograph -j
./build-release/bench_neograph
valgrind --tool=cachegrind --cache-sim=yes \
  --I1=32768,8,64 --D1=32768,8,64 --LL=33554432,16,64 \
  ./build-release/bench_neograph
```

結果 artifact の `ldd`/`otool` または platform loader inventory を確認し、旧依存一覧を仮定しない。記録された interface-3 SDK runtime/archive 検証は Linux/POSIX の範囲で、interface 4 の資格検証ではない。既存 macOS/Windows metadata と WASM target は新依存の検証ではない。typed Python binding は移行に含まれ、延期ではない。性能は再構築 wheel で測定する必要がある。
