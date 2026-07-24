<!-- neograph-i18n: source=benchmarks/concurrent/CONCURRENT.md locale=ko source_sha256=5331369d595d05bf67c0748516d7da5980b3dfa70262e2df9c01849f90564b58 -->
**Languages:** [English](CONCURRENT.md) | [한국어](CONCURRENT.ko.md) | [日本語](CONCURRENT.ja.md) | [简体中文](CONCURRENT.zh-CN.md)

# 동시 로드 벤치마크 — NeoGraph 대 Python 그래프 프레임워크


**버스트** 로드 시 — N개의 요청이 동시에 제출된 다음
테스트는 모든 것이 완료될 때까지 기다립니다. 이러한 엔진은 어떻게 확장되고,
각 접근 방식의 실행이 중단되는 지점은 무엇입니까?

이 벤치는 재현 가능한 Docker 샌드박스에서 CPU 및
6개 프레임워크에 걸쳐 SBC 클래스 대상과 일치하는 메모리 제한입니다.

## 설정

- **워크로드**: 3노드 순차 카운터 체인(`a → b → c`), 각각
노드는 단일 상태 채널을 증가시킵니다. I/O 없음, 절전 없음, LLM 없음.
- **버스트 패턴**: t=0에 N개의 작업이 제출되었습니다. 주자는 모두를 기다립니다
끝내다; 요청별 대기 시간은 프로세스 내에서 캡처됩니다.
- **샌드박스**: `--cpus` 및 `--memory`(+ `--memory-swap`)가 포함된 Docker
일치하므로 교체하는 대신 OOM가 실행됩니다.) 행렬:
  - 프로필 **1 CPU / 512MB** — "tight SBC" 대상(기본)
  - 프로필 **2 CPU / 1GB** — "편안한 SBC" 대상
- **동시성**: N ∈ {10, 100, 1000, 10000}
- **테스트된 엔진**:
  - `neograph`(3.0) — `engine->run()`를 파견하는 `hardware_concurrency()` 작업자가 있는 호출자 측 `asio::thread_pool`입니다. 엔진 자체는 기본적으로 단일 스레드 `run_sync`에서 슈퍼 스텝 루프를 구동합니다. 각 호출은 자체 io_context를 사용하므로 예약은 엔진이 아닌 호출자 풀에 의해 제한됩니다.
  - `langgraph-asyncio` / `langgraph-mp` — `asyncio.gather` / `multiprocessing.Pool` 아래의 LangGraph 1.1.9.
  - `haystack-asyncio` / `haystack-mp` — 건초 더미 2.27.0. Pipeline.run()는 동기화됩니다. asyncio 모드는 `asyncio.to_thread`로 래핑됩니다.
  - `pydantic-asyncio` / `pydantic-mp` — pydantic-graph 1.84.1, 비동기 네이티브.
  - `llamaindex-asyncio` / `llamaindex-mp` — LlamaIndex 워크플로 0.14.20, 실행당 하나의 새로운 워크플로(실행당 이벤트 버스).
  - `autogen-asyncio` / `autogen-mp` — AutoGen GraphFlow 0.7.5, 실행당 하나의 새로운 흐름(흐름 상태는 동시 안전이 아님)

## 결과 — 1 CPU / 512MB 프로필(asyncio 모드)

![Throughput — requests per second](../../docs/images/bench-concurrent-throughput.png)

![Tail latency — P99 per request](../../docs/images/bench-concurrent-latency.png)

![Peak resident memory](../../docs/images/bench-concurrent-rss.png)

차트는 6개 엔진 모두에 대한 비동기 모드 결과를 추적합니다. mp
(다중 처리) 행은 아래 원시 숫자 테이블에 있습니다. — mp
N 작업자 프로세스에서 GIL를 우회하지만 풀에서는 포화 상태입니다.
크기이며 패턴은 모든 Python 프레임워크에서 동일합니다.

### 원시 숫자(1 CPU / 512MB, 3.0의 NeoGraph 2026-04-22, Python 필드 2026-04-19)

2 CPU / 1GB 프로필을 포함한 전체 매트릭스는 다음과 같습니다.
[`results.jsonl`](results.jsonl). N=10,000은 다음을 알려주는 행입니다.
가장 날카로운 이야기:

|N|엔진 + 모드|벽|P50|P99|피크 RSS|알았어/오류|
|---|---------------|------|-----|-----|----------|---------|
| **10,000** |**네오그래프 3.0**|**52ms**|**4μs**|**7μs**|**5.5MB**| 10000 / 0 |
| 10,000 |LangGraph 비동기|23.4초|20.2초|**23.0초**|416.2MB| 10000 / 0 |
| 10,000 |LangGraph mp-풀-7|8.0초|737μs|88.4ms|60.3MB| 10000 / 0 |
| 10,000 |건초 더미 asyncio|3.1초|1.7초|2.9초|130.7MB| 10000 / 0 |
| 10,000 |건초 더미 mp-pool-7|2.9초|167μs|84.7ms|68.1MB| 10000 / 0 |
| 10,000 |pydantic 그래프 asyncio|886ms|71μs|**158μs**|42.6MB| 10000 / 0 |
| 10,000 |pydantic 그래프 mp-pool-7|2.8초|253μs|83.8ms|36.7MB| 10000 / 0 |
| 10,000 |**LlamaIndex 비동기**|**OOM가 사망했습니다**| — | — | — | — |
| 10,000 |라마인덱스 mp-pool-7|6.6초| — | — |102.5MB| **0 / 10000** |
| 10,000 |**자동 생성 비동기**|**OOM가 사망했습니다**| — | — | — | — |
| 10,000 |AutoGen mp-풀-7|46.8초|4.6ms|97.1ms|49.1MB| 10000 / 0 |

두 엔진이 N=10,000에서 512MB 샌드박스를 비정상적으로 종료합니다.

* **LlamaIndex asyncio** — OOM가 사망했습니다. 각 진행 중인 워크플로우에는
실행별 이벤트 버스 + 채널 런타임; 그 중 10,000개가
벽시계가 완료되기 전의 cgroup입니다.
* **AutoGen asyncio** — OOM가 종료되었습니다. 10,000개의 동시 GraphFlow 인스턴스
참가자 국가 여행의 한도는 동일합니다.
* **LlamaIndex mp-pool** — 10,000개의 작업자 호출이 모두 실패했습니다. 작업 흐름
인스턴스는 작업자 프로세스 포크에서 피클로부터 안전하지 않습니다. 실패하다
N과 상관없이

두 프로필의 전체 원시 행렬은 다음과 같습니다.
[`results.jsonl`](results.jsonl)(셀당 하나의 JSON 라인).

## 해석

### 처리량: NeoGraph 확장, 모든 Python asyncio 런타임 정체

NeoGraph의 녹색 곡선은 전체 22-25K req/s 범위에 유지됩니다.
모든 Python asyncio 곡선이 저하되는 동안 청소합니다. 발신자 측
`asio::thread_pool`는 전체에 걸쳐 `engine->run()` 호출을 전달합니다.
사용 가능한 코어; 그런 다음 각 호출은 자체 단일 스레드를 구동합니다.
`run_sync`를 통한 슈퍼 스텝 루프 — cgroup의 CPU 할당량 범위
벽 시간은 있지만 스레드 개수는 아니므로 짧은 작업이 깔끔하게 인터리브됩니다.
발신자 풀 건너편에 있습니다.

**모든 Python asyncio 곡선은 정체되거나 저하됩니다.** 근본 원인은 다음과 같습니다.
LangGraph, Haystack, pydantic-graph, LlamaIndex 및
AutoGen: 하나의 프로세스에 하나의 이벤트 루프가 있으며 GIL는
CPU는 모든 코루틴이 수행해야 하는 작업입니다. N 코루틴 → 직렬화됨
실행 → N으로 확장되지 않는 처리량

각 프레임워크의 mp-pool 모드는 전체에서 GIL를 우회합니다.
`os.cpu_count()` 작업자 프로세스 - 그러나 해당 풀 크기에서는 포화 상태입니다.
작업당 포크 + 피클 오버헤드를 지불합니다. ~N=1000 너머에는 풀이 있습니다.
프레임워크에 관계없이 포화 및 처리량 정체 상태입니다.

### 꼬리 대기 시간: 범용 GIL 한도

N=10,000에서 NeoGraph의 P99는 마이크로초 단위로 유지됩니다. 모든 파이썬
asyncio P99는 N에 따라 선형적으로 상승합니다. 왜냐하면 *마지막* 코루틴이
GIL 큐는 슬롯 이전에 전체 실행이 완료될 때까지 기다립니다.

이는 LangGraph에만 국한된 문제가 아닙니다. 똑같은 모양이 보여요
Haystack(`to_thread`로 래핑된 동기화 파이프라인), LlamaIndex
(비동기 이벤트 기반 워크플로), pydantic-graph (비동기 상태 머신),
및 AutoGen(비동기 다중 에이전트 런타임). Python 오케스트레이션이 있는 경우
단일 프로세스 뒤에 있는 프레임워크인 GIL가 한계입니다.

P99 SLO 기대치가 있는 현실적인 서버의 경우(예: "1 미만)
요청의 99%에 대해 두 번째"), 모든 asyncio 지원 엔진은
일부 N. 정확한 중단점은 프레임워크에 따라 다릅니다. 런타임이 더 가벼워집니다.
(pydantic-graph, LangGraph) 나중에 중단되고 더 무거운 것(LlamaIndex,
AutoGen)은 훨씬 일찍 중단되지만 모두 중단됩니다.

### 메모리: asyncio의 RSS는 보유된 코루틴 스택으로 성장합니다.

- **NeoGraph 3.0**은 전체 스윕에서 4.2~5.5MB 사이를 유지합니다.
작업은 즉시 반환됩니다. 발신자 측 `asio::thread_pool`만
기내 `run()`당 하나의 io_context가 상주합니다.
- **mp-pool 모드**는 프레임워크 전체에서 약 60~80MB를 유지합니다. — 작업자
수영장 크기가 지배적입니다. 작업은 누적되지 않습니다.
하나씩 발송하고 반송했습니다.
- **asyncio 모드**는 N에 따라 선형적으로 증가합니다. 보류 중인 모든 코루틴
Python 스택 프레임, 클로저 상태 및 실행별 프레임워크를 보유합니다.
상태. 비행 중인 코루틴이 10,000개이면 수백 개가 됩니다.
더 무거운 런타임의 경우 MB입니다.

512MB 메모리 예산으로 인해 일부 asyncio 실행이
N=10,000의 cgroup 한도. 256MB의 cgroup이 더 작을수록 더 무거워집니다.
프레임워크는 N=1,000과 N=1,000 사이 어딘가에서 OOM가 종료됩니다.
N=10,000. NeoGraph는 해당 예산에서 여전히 최대 500MB의 여유 공간을 갖고 있습니다.

## 이 벤치는 NOT가 말하는 것

- **규모에 따른 프레임워크 "충돌"을 증명하지 않습니다.** 이야기는 다음과 같습니다.
프로세스 종료가 아니라 사용할 수 없는 대기 시간으로의 점진적인 저하입니다. 에
더 엄격한 cgroup 이상 N, OOM 킬은 종료 모드가 됩니다.
여기에는 이를 문서화하지 않았으며 후속 조치가 필요합니다.
- **LLM I/O를 모델링하지 않습니다.** 실제 에이전트 워크로드는 100~1,000ms입니다.
LLM 호출당. 그 지연 시간은 절대적인 측면에서 엔진 격차를 축소시킵니다.
그러나 용량 측면에서 NOT: 엔진이 1,000 req/s만 푸시할 수 있는 경우
런타임을 통해 동시 LLM I/O가 도움이 되지 않습니다.
- **지속성은 다루지 않습니다.** 체크포인트는 다음에서 비활성화되었습니다.
뼈대. 이를 활성화하면 비교가 상점으로 이동됩니다.
구현은 다른 벤치마크입니다.
- **워크로드 형태 편향.** 카운터 체인은 NeoGraph 기반입니다.
상태 의미론; Haystack은 동기화 파이프라인을 `to_thread`로 래핑합니다.
AutoGen은 카운터를 메시지 콘텐츠로 인코딩하지만 pydantic-graph에는
팬아웃(이 벤치에서는 사용되지 않지만 버스트 워크로드와 관련됨)
분기 포함). 각 프레임워크는 해당 작업을 수행하도록 요청받는 것이지
최고의 사례입니다.
- **WSL2의 Docker.** `--cpus`는 CPU 할당량을 적용하지만 표시되지는 않습니다.
코어 수, 이것이 NeoGraph의 `hardware_concurrency()`가 여전히 이유입니다.
호스트 수를 반환합니다. 베어메탈에 대한 결과는 다음과 같아야 합니다.
방향은 동일하지만 NeoGraph 끝 부분이 더 단단합니다(더 적음).
스레드, 컨텍스트 전환 노이즈 감소).

## 낳다

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

단일 셀 디버그 실행:

```bash
docker run --rm --cpus=1 --memory=512m --memory-swap=512m \
    ng-concurrent 10000

docker run --rm --cpus=1 --memory=512m --memory-swap=512m \
    li-concurrent async 10000
```

각 컨테이너는 다음 형식의 단일 JSON 줄을 인쇄합니다.

```json
{"engine":"neograph","mode":"threadpool","concurrency":10000,
 "total_wall_ms":6,"p50_us":2,"p95_us":3,"p99_us":6,
 "ok":10000,"err":0,"peak_rss_kb":7808}
```
