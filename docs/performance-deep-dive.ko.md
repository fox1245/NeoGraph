<!-- neograph-i18n: source=docs/performance-deep-dive.md locale=ko source_sha256=31ea664b884c612f5b01c1132fe748da0cb0115e2afe5bf203f33e958516c63b -->
# 성능 상세

**Languages:** [English](performance-deep-dive.md) | [한국어](performance-deep-dive.ko.md) | [日本語](performance-deep-dive.ja.md) | [简体中文](performance-deep-dive.zh-CN.md)

## 역사적 측정 읽기

아래 표는 전환 전 측정을 보존한다. 현재 typed provider, 외부 runtime, 재빌드된 Python wheel의 검증이 아니다. 새 비교에는 revision, compiler, configuration, topology, concurrency, endpoint, 시간 측정 경계를 기록한다. 현재 재현 지침은 [benchmark](../benchmarks/README.md)에 있다.

과거 글의 cloud 비용, 수백만 worker, 영구 ABI 동결은 이 측정에서 외삽한 전망이지 측정된 배포가 아니다. 현재 용량·비용 보장이 아니다. package version뿐 아니라 wheel hash와 native dependency도 고정한다. version 문자열만으로 배포 재현성을 얻을 수 없다.

## 엔진 전용 작업, 2026년 4월

같은 topology를 한 번 컴파일하고 model/network/sleep 없이 실행했다. seq는 세 node chain, par는 다섯 분기와 join이다. 2026-04-22 x86_64 Linux에서 NeoGraph는 GCC 13 Release `-O3 -DNDEBUG` 10회 중앙값, Python은 CPython 3.12.3 3회 중앙값을 썼다. pydantic-graph의 par는 native fan-out이 아닌 여섯 node 직렬 모방이다.

| Framework (2026-04-22) | seq µs | par µs |
|---|---:|---:|
| NeoGraph, then-current master | 5.0 | 11.8 |
| Haystack 2.28.0 | 144.1 | 290.0 |
| pydantic-graph 1.85.1 | 235.9 | 286.1 |
| LangGraph 1.1.9 | 656.7 | 2348.7 |
| LlamaIndex Workflow 0.14.21 | 1780.3 | 4683.5 |
| AutoGen GraphFlow 0.7.5 | 3209.2 | 7292.7 |

과거 warm-up 및 seq 10000/par 5000회 실행은 NeoGraph ~0.16 s·4.8 MB, Haystack 2.91 s·80.3 MB, AutoGen 68.29 s·52.4 MB였다. I/O 없는 engine 비교가 model-service latency를 예측하지 않는다.

## 역사적 burst 동시성

CPU 하나와 RAM 512 MB의 Docker cgroup에 요청 10000개를 동시에 제출했다. Python은 asyncio를 썼다. 결과는 그 환경·작업에 한정되며 보편적인 Python/GIL scaling 법칙이 아니다.

| Framework | Wall | P99 | Peak RSS | Result |
|---|---:|---:|---:|---|
| NeoGraph, then-current master | 52 ms | 7 µs | 5.5 MB | 10000 completed |
| pydantic-graph | 886 ms | 158 µs | 42.6 MB | 10000 completed |
| Haystack | 3.1 s | 2.9 s | 130.7 MB | 10000 completed |
| LangGraph | 23.4 s | 23.0 s | 416.2 MB | 10000 completed |
| LlamaIndex | — | — | — | OOM killed |
| AutoGen | — | — | — | OOM killed |

방법과 process-pool 비교는 [동시 benchmark](../benchmarks/concurrent/CONCURRENT.md)에 있다.

## 역사적 cache simulation

Cachegrind가 Ryzen 7 5800X cache 계층을 모사했다. L1 instruction/data 각각 32 KB·8-way, last-level 32 MB·16-way, line 64 byte였다. 당시 concurrent benchmark 결과:

| N | Instruction references | Last-level instruction misses | Native p50 |
|---|---:|---:|---:|
| 1 | 5.3 M | 4313 | 17 µs |
| 10 | 5.9 M | 4304 | 16 µs |
| 100 | 11.8 M | 4320 | 6 µs |
| 1000 | 69.7 M | 4327 | 6 µs |
| 10000 | 648 M | 4329 | 5 µs |

약 4330 miss에 64 byte를 곱한 값은 277 KB의 line-count 추정이다. simulated miss만으로 실제 resident working set, DRAM stall 시간, native latency 변화 원인을 증명할 수 없다. [Cachegrind manual](https://valgrind.org/docs/manual/cg-manual.html)을 참고한다.

## 역사적 local model 작업

별도 neoclaw 실험은 RTX 4070 Ti 하나, Gemma 4 E2B Q4 GGUF 하나(~1.5 GB weight), inference를 직렬화한 shared LocalProvider, `llm_call` node 하나를 썼다. wall time은 모든 worker를 drain하는 시간이다. request percentile이 대기 시간을 없앤다는 근거는 아니다.

| N | Wall s | Throughput rps | p50 ms | p99 ms | Peak RSS MB |
|---|---:|---:|---:|---:|---:|
| 1 | 0.64 | 1.6 | 642 | 642 | 2464 |
| 10 | 0.94 | 10.6 | 184 | 686 | 2529 |
| 100 | 4.81 | 20.8 | 343 | 855 | 2549 |
| 1000 | 44.1 | 22.7 | 347 | 673 | 2564 |
| 5000 | 213.7 | 23.4 | 338 | 657 | 2570 |
| 10000 | 424 | 23.6 | 337 | 648 | 2572 |

소스는 [neoclaw benchmark](https://github.com/fox1245/neoclaw/blob/main/benchmarks/bench_concurrent_workers_local_llm.cpp)다. CUDA benchmark 설정은 그 저장소의 것이며 현재 NeoGraph provider build 설정이 아니다. 관측된 marginal memory에서 미측정 session 상한을 외삽하지 않는다.

다른 역사적 실험은 HTTP endpoint 뒤의 local Gemma-4 E2B Q4_K_M(4.65 B parameter, 2.9 GB GGUF)을 썼다.

| Metric | Engine-only | Historical local Gemma HTTP |
|---|---:|---:|
| Last-level instruction misses | 4320 | 7262 |
| 64 × simulated LL instruction miss(역사적 계산값) | 277 KB | 465 KB |
| Agent RSS | 5.2 MB | 7.6 MB |
| TTFT | — | 25–27 ms |
| Total request time | — | 146–213 ms |
| Separate model-server RSS | — | 2.45 GB |
| RTX 4070 Ti VRAM | — | 3.06 GB |

요청 다섯 개에 1.58 s, overlap 속도 향상 2.65×를 기록했다. 옛 OpenAIProvider 설정과 httplib transport accounting은 그 실험의 것이다. 현재 local endpoint는 validated descriptor와 SDK runtime option으로 구성한다. [예제 31](../examples/31_local_transformer.cpp)을 참고한다. 별도 inference process는 model allocation을 agent process 밖에 두지만 cache residency를 보장하지 않는다.

## 역사적 크기와 cold start

x86_64 Linux GCC 13 Plan & Executor demo는 `-Os`, static libstdc++/libgcc, dead-section 제거, stripping을 썼다. 120 ms sleep으로 model 작업을 흉내 내고 선택적 fan-out pool을 켠 뒤 한 branch 실패 후 resume했다.

| Metric | Historical Plan & Executor demo |
|---|---:|
| Stripped MinSizeRel binary | 1203 KB |
| Peak RSS, crash and resume included | 2.9 MB |
| Cold start through both phases | ~720 ms |
| Recorded dynamic dependencies | libc.so.6 only |

이는 당시 artifact 측정이다. 현재 libc-only 배포, 250 ms 미만 cold start, musl cross-compile, 특정 embedded board 용량을 증명하지 않는다.

## 현재 측정 재현

외부 `SchemaProvider::runtime`은 `NEOGRAPH_BUILD_LLM=OFF`여도 필요하다. `SDK_PREFIX`를 installed prefix로 설정하거나 SDK 개발 의존성과 함께 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`를 쓴다. transitive link는 CMake target으로 처리한다. 옛 core/yyjson archive 직접 링크는 runtime을 빠뜨린다. 다음은 현재 engine benchmark 측정이며 과거 concurrent sweep의 정확한 재현은 아니다.

NeoGraph는 CMake 3.20+가 필요하다. 명시적 SDK source나 installed package가 없으면 기본 설정이 revision-pinned 공개 SDK archive를 가져온다. installed SDK offline 측정은 prefix와 함께 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`를 설정한다. fetch가 libcurl/OpenSSL 개발 의존성을 제거하지 않는다.

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_BENCHMARKS=ON -DCMAKE_PREFIX_PATH="$SDK_PREFIX"
cmake --build build-release --target bench_neograph -j
./build-release/bench_neograph
valgrind --tool=cachegrind --cache-sim=yes \
  --I1=32768,8,64 --D1=32768,8,64 --LL=33554432,16,64 \
  ./build-release/bench_neograph
```

결과 artifact에 `ldd`/`otool` 또는 해당 플랫폼 loader inventory를 적용하고 과거 의존성 목록을 가정하지 않는다. 기록된 interface-3 SDK runtime/archive 검증은 Linux/POSIX 범위이며 interface 4를 검증하지 않는다. 기존 macOS/Windows metadata와 WASM target은 새 의존성 검증이 아니다. typed Python binding은 전환에 포함되며 유예된 것이 아니다. 성능은 재빌드한 wheel로 측정해야 한다.
