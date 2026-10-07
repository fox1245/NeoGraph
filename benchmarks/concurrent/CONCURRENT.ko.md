<!-- neograph-i18n: source=benchmarks/concurrent/CONCURRENT.md locale=ko source_sha256=dbe7cdde4e740c018239a2ca99a8ee319dd9ff01974ea002629f03d5595e2199 -->
# 동시 부하 벤치마크: NeoGraph와 Python의 과거 결과

**Languages:** [English](CONCURRENT.md) | [한국어](CONCURRENT.ko.md) | [日本語](CONCURRENT.ja.md) | [简体中文](CONCURRENT.zh-CN.md)

2026년 4월 카운터 체인 비교를 보존합니다. 현재 SchemaProvider SDK나 Python 바인딩으로 재실행한 결과가 아닙니다. “NeoGraph 3.0”은 당시 기록의 표기이며 현재 패키지 버전이 아닙니다.

## 워크로드와 측정 구간

세 노드가 overwrite 카운터 채널을 증가시킵니다(`a → b → c`). 모델 호출, 대기, 네트워크 I/O, 체크포인트 저장소는 없습니다. Docker의 1 CPU / 512 MB와 2 CPU / 1 GB 설정에서 N ∈ {10, 100, 1000, 10000} 실행을 제출하고 스왑 한도를 메모리 한도와 같게 둡니다.

NeoGraph는 `max(hardware_concurrency(), 1)` 크기의 호출자 `asio::thread_pool`을 사용합니다. 요청 타이머는 작업자 내부에서 시작하므로 P50/P99에는 호출자 큐 대기 시간이 없습니다. `total_wall_ms`는 제출부터 전체 완료까지 포함합니다. 마이크로초 P99를 서버의 종단 간 SLO와 직접 비교하지 마세요.

과거 Python 대상은 LangGraph 1.1.9, Haystack 2.27.0, pydantic-graph 1.84.1, LlamaIndex Workflow 0.14.20, AutoGen GraphFlow 0.7.5의 asyncio 및 multiprocessing 모드였습니다. 이 버전은 과거 기록이지 현재 Docker 의존성 해석 결과가 아닙니다.

## 과거 결과: 1 CPU / 512 MB

차트와 표는 NeoGraph 2026-04-22, Python 2026-04-19 기록입니다. N=10,000 표는 엔진만 측정했으며 공급자 전송이나 추론 결과가 아닙니다. 누락된 값은 그대로 둡니다.

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

전체 행렬은 [`results.jsonl`](results.jsonl)에 있습니다. 당시 LlamaIndex와 AutoGen asyncio 셀은 OOM 종료로 분류됐고 이 행의 LlamaIndex multiprocessing 호출은 모두 실패했습니다. 보편적인 프레임워크 한계나 모든 실패 원인을 입증하지는 않습니다.

## 해석 범위

GIL이 활성화된 CPython은 Python 바이트코드를 직렬 실행하지만 이벤트 루프, 프레임워크 작업, 프로세스 직렬화와 작업자 수도 처리량에 영향을 줍니다. 한 가지 원인, 보편적인 asyncio 한계, free-threaded Python 성능을 입증하지 않습니다.

Docker CPU 할당량이 `hardware_concurrency()`의 코어 수를 바꾸지는 않을 수 있습니다. 호출자 풀과 호스트 구성을 기록하세요. 최대 RSS는 Linux `/proc/self/status`에서 읽으며 지원되지 않는 플랫폼의 0은 측정 불가입니다. multiprocessing 메모리는 각 실행자의 집계 범위도 확인하세요. 이 표로 256 MB 실험, 베어메탈 예측, 영속성 비교, 원격 LLM 용량을 보장할 수 없습니다.

## 현재 의존성과 재현 상태

Core는 `NEOGRAPH_BUILD_LLM=OFF`, `NEOGRAPH_USE_LIBCURL=OFF`에서도 외부 `SchemaProvider::runtime`을 링크합니다. 설치 SDK는 `CMAKE_PREFIX_PATH`, 명시적 소스는 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`로 지정합니다. [빌드 안내](../../README.md)를 참고하세요. SDK 소스 빌드에는 C++20, 설정 생성용 Python, libcurl ≥7.88, OpenSSL Crypto가 필요합니다. 현재 SDK 검증 범위는 Linux/POSIX이며 과거 Docker 결과로 다른 플랫폼을 검증하지 않습니다.
NeoGraph `0.13.1` recipe에는 alpha SDK `0.1.1`, interface revision/shared generation 4의 일치하는 header/library가 필요합니다. 현재 통합 검증은 대기 중입니다. 이전 Linux/POSIX 검증은 SDK4나 새 Docker 통과 기록이 아닙니다.

현재 NeoGraph Docker 이미지는 curl/OpenSSL 개발 의존성을 설치하고 `neograph::core`에 링크하는 전용 CMake 소비자를 빌드해 SDK 의존성을 상속합니다. SDK 취득은 루트 CMake 정책을 따르며 패키지나 소스를 제공하지 않으면 네트워크가 필요할 수 있습니다. 선택적 NG 네트워크 모듈을 꺼도 SDK는 필요합니다. 새 Docker 측정 결과를 주장하지 않습니다. 행렬은 빌드 전에 출력 파일을 비우므로 새 경로를 지정하세요. `status=ok`는 JSON 추출 성공일 뿐이며 `ok`, `err`, 종료 코드도 확인하세요.

```bash
# From the repository root. Docker builds use the root SDK acquisition policy.
docker build -t ng-concurrent -f benchmarks/concurrent/Dockerfile.neograph .
docker run --rm --cpus=1 --memory=512m --memory-swap=512m ng-concurrent 10000

# Full matrix; a NEW path preserves the archived results.jsonl.
bash benchmarks/concurrent/run_matrix.sh benchmarks/concurrent/results-new.jsonl

# Render the archived default results.jsonl, not the new output.
node benchmarks/render_concurrent.js
```

아래 JSON은 필드 형식 예시이며 추가 측정 결과가 아닙니다.

```json
{"engine":"neograph","mode":"threadpool","concurrency":10000,
 "total_wall_ms":6,"p50_us":2,"p95_us":3,"p99_us":6,
 "ok":10000,"err":0,"peak_rss_kb":7808}
```
