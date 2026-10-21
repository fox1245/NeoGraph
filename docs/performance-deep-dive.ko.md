<!-- neograph-i18n: source=docs/performance-deep-dive.md locale=ko source_sha256=1441793955b212960f5bb907b65dda7705de92ccfbfa4877fb40810066ac2ea7 -->
# 성능 심층 분석

**Languages:** [English](performance-deep-dive.md) | [한국어](performance-deep-dive.ko.md) | [日本語](performance-deep-dive.ja.md) | [简体中文](performance-deep-dive.zh-CN.md)

## 과거 측정값 읽는 법

아래 표는 cutover 이전에 측정한 값을 그대로 보존한 것입니다. 현재의 타입 기반 provider, 그 외부 런타임, 다시 빌드한 Python wheel을 검증한 결과가 아닙니다. 새로 비교할 때는 리비전, 컴파일러, 구성(configuration), 토폴로지, 동시성, 엔드포인트, 시간 측정 경계를 기록해 두어야 합니다. 현재의 재현 방법은 [benchmarks](../benchmarks/README.md)에 있습니다.

이 측정값으로는 클라우드 비용, 워커 수, 용량을 보장할 수 없습니다. 패키지 버전뿐 아니라 wheel 해시와 네이티브 의존성도 고정해야 합니다. 버전 문자열만으로는 배포를 재현할 수 없습니다.

## 엔진 단독 워크로드

현재의 프레임워크 간 비교 표(매칭 레짐과 관용 기본값, 메모리, 시작 시간, 크기)는 체크인된 기록에서 생성되며 [벤치마크 가이드](../benchmarks/README.md)에 있습니다. 이 페이지가 되풀이하던 2026년 4월의 표는 버전을 섞어 여러 세션에서 측정한 것이어서, 현재 무효로 알려진 항목에 대한 설명과 함께 [이력](../benchmarks/history/engine-overhead-2026-04.md)에 보존합니다. I/O가 전혀 없는 엔진 비교로는 모델 서비스의 지연 시간을 예측할 수 없습니다.

## 버스트 동시성

요청 1만 건을 한꺼번에 제출하고(닫힌 단일 버스트) CPU 1개와 RAM 512 MB로 제한한 Docker cgroup 안에서 실행했습니다. 기록된 데이터가 뒷받침하는 wall 시간 열과, 철회한 열과 행 및 그 이유는 [동시성 벤치마크](../benchmarks/concurrent/CONCURRENT.md)에 있습니다. 이 결과는 그 워크로드와 환경에 한정되며, Python/GIL의 보편적인 확장성 법칙을 보여 주지는 않습니다.

## 과거 캐시 시뮬레이션

Cachegrind로 Ryzen 7 5800X의 캐시 계층을 시뮬레이션했습니다. 32 KB L1 명령어/데이터 캐시(8-way), 32 MB 마지막 레벨 캐시(16-way), 64바이트 라인입니다. 과거 동시성 벤치마크는 다음 결과를 보고했습니다.

| N | 명령어 참조 수 | 마지막 레벨 명령어 미스 | 네이티브 p50(워커 안의 서비스 시간) |
|---|---:|---:|---:|
| 1 | 5.3 M | 4313 | 17 µs |
| 10 | 5.9 M | 4304 | 16 µs |
| 100 | 11.8 M | 4320 | 6 µs |
| 1000 | 69.7 M | 4327 | 6 µs |
| 10000 | 648 M | 4329 | 5 µs |

미스 약 4330회에 64바이트를 곱하면 라인 수 기준으로 277 KB라는 추정값이 나옵니다. 그러나 Cachegrind가 시뮬레이션한 미스만으로는 실제로 상주하는 워킹 셋, DRAM 대기 시간, 네이티브 지연 시간이 달라진 원인을 입증할 수 없습니다. [Cachegrind 매뉴얼](https://valgrind.org/docs/manual/cg-manual.html)을 참고하세요.

## 과거 로컬 모델 워크로드

별도의 neoclaw 실험은 RTX 4070 Ti 한 장과 Gemma 4 E2B Q4 GGUF 모델 하나(가중치 약 1.5 GB)를 사용했습니다. 공유 LocalProvider가 추론을 직렬화했고, 그래프는 `llm_call` 노드 하나로 구성했습니다. 기록된 wall time은 모든 워커가 끝날 때까지 걸린 시간입니다. 표에 나온 요청 백분위수만으로는 대기열 지연이 사라졌다고 볼 수 없습니다.

| N | Wall s | 처리량 rps | p50 ms | p99 ms | 최대 RSS MB |
|---|---:|---:|---:|---:|---:|
| 1 | 0.64 | 1.6 | 642 | 642 | 2464 |
| 10 | 0.94 | 10.6 | 184 | 686 | 2529 |
| 100 | 4.81 | 20.8 | 343 | 855 | 2549 |
| 1000 | 44.1 | 22.7 | 347 | 673 | 2564 |
| 5000 | 213.7 | 23.4 | 338 | 657 | 2570 |
| 10000 | 424 | 23.6 | 337 | 648 | 2572 |

출처: [neoclaw 벤치마크](https://github.com/fox1245/neoclaw/blob/main/benchmarks/bench_concurrent_workers_local_llm.cpp). CUDA 벤치마크 구성은 그 저장소의 것이며, NeoGraph의 현재 provider 빌드와는 무관합니다. 관측된 추가 메모리 증가분을 측정하지 않은 세션 상한까지 외삽해서는 안 됩니다.

또 다른 과거 실험은 HTTP 엔드포인트 뒤에 로컬 Gemma-4 E2B Q4_K_M 모델(파라미터 4.65 B, GGUF 2.9 GB)을 두고 진행했습니다.

| 항목 | 엔진 단독 | 과거 로컬 Gemma HTTP |
|---|---:|---:|
| 마지막 레벨 명령어 미스 | 4320 | 7262 |
| 64 × 시뮬레이션된 LL 명령어 미스 (과거에 계산한 값) | 277 KB | 465 KB |
| 에이전트 RSS | 5.2 MB | 7.6 MB |
| TTFT | — | 25–27 ms |
| 전체 요청 시간 | — | 146–213 ms |
| 별도 모델 서버 RSS | — | 2.45 GB |
| RTX 4070 Ti VRAM | — | 3.06 GB |

이 실험은 요청 5개를 1.58 s에 처리했고, 겹쳐 실행해서 얻은 속도 향상은 2.65×였습니다. 기존 OpenAIProvider 구성과 httplib 전송 계층 집계는 이 실험에만 해당합니다. 현재는 검증된 descriptor와 SDK 런타임 옵션으로 로컬 엔드포인트를 설정합니다. [예제 31](../examples/31_local_transformer.cpp)을 참고하세요. 추론 프로세스를 따로 두면 모델이 쓰는 메모리가 에이전트 프로세스 바깥에 잡히지만, 캐시에 계속 남아 있다는 보장은 없습니다.

## 과거 크기와 콜드 스타트

x86_64 Linux, GCC 13에서 빌드한 Plan & Executor 데모는 `-Os`, 정적 libstdc++/libgcc, 사용하지 않는 섹션 제거, strip을 적용했습니다. 모델 작업은 120 ms sleep으로 흉내 냈고, 선택적으로 켜는 fan-out 풀을 사용했으며, 분기 하나를 일부러 실패시킨 뒤 재개했습니다.

| 항목 | 과거 Plan & Executor 데모 |
|---|---:|
| strip한 MinSizeRel 바이너리 | 1203 KB |
| 최대 RSS (크래시와 재개 포함) | 2.9 MB |
| 두 단계를 모두 마칠 때까지의 콜드 스타트 | ~720 ms |
| 기록된 동적 의존성 | libc.so.6뿐 |

이 값은 과거 산출물에서 측정한 것입니다. 현재도 libc만으로 배포할 수 있다는 점, 콜드 스타트가 250 ms 미만이라는 점, musl로 크로스 컴파일할 수 있다는 점, 특정 임베디드 보드에서 감당할 수 있는 용량은 이 값으로 입증되지 않습니다.

## 현재 측정을 재현하는 방법

`NEOGRAPH_BUILD_LLM=OFF`로 빌드해도 외부 `SchemaProvider::runtime`이 필요합니다. `SDK_PREFIX`에 설치된 prefix를 지정하거나, SDK의 개발 의존성과 함께 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`을 사용합니다. 전이적 링크는 CMake 타깃으로 처리해야 합니다. 예전처럼 core와 yyjson 아카이브를 직접 지정하는 명령에는 런타임이 빠져 있습니다. 아래 절차는 현재의 엔진 벤치마크를 측정하며, 과거의 동시성 스윕을 그대로 재현하지는 않습니다.

NeoGraph에는 CMake 3.20 이상이 필요합니다. SDK 소스를 명시하지 않고 설치된 패키지도 없으면, 구성 단계에서 리비전이 고정된 공개 SDK 아카이브를 기본으로 내려받습니다. 설치된 SDK로 오프라인에서 측정하려면 prefix와 함께 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`도 설정합니다. 내려받아도 libcurl/OpenSSL 개발 패키지는 여전히 필요합니다.

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_BENCHMARKS=ON -DCMAKE_PREFIX_PATH="$SDK_PREFIX"
cmake --build build-release --target bench_neograph -j
./build-release/bench_neograph
valgrind --tool=cachegrind --cache-sim=yes \
  --I1=32768,8,64 --D1=32768,8,64 --LL=33554432,16,64 \
  ./build-release/bench_neograph
```

예전 의존성 목록을 그대로 믿지 말고, 만들어진 산출물에서 `ldd`/`otool`이나 플랫폼 로더의 목록을 직접 확인합니다. 타입 기반 Python 바인딩의 성능은 다시 빌드한 wheel로 따로 측정해야 합니다.
