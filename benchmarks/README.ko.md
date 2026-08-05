<!-- neograph-i18n: source=benchmarks/README.md locale=ko source_sha256=d0a5deba1ae20473d11d5dd6385d0966e7d08ab563f333f5730751dfeb994466 -->
**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

# NeoGraph 대 Python graph/pipeline 프레임워크 — 엔진 오버헤드 벤치마크


선두에 대해 NeoGraph의 호출별 오버헤드를 측정합니다.
**no가 포함된 동일한 모양의 그래프에 대한 Python 오케스트레이션 프레임워크
I/O, 절전 모드 없음, LLM 호출 없음**. 숫자는 엔진의 성능을 반영합니다.
자체 비용(노드 디스패치, 상태 채널 쓰기, 리듀서 호출) - 그렇지 않음
시뮬레이션된 작업의 대기 시간.

비교된 프레임워크:

|뼈대|버전|추출|
|-----------|---------|-------------|
|네오그래프|3.0 (`feat/taskflow-removal`)|상태 채널 그래프, C++20 코루틴 + asio|
|랭그래프| 1.1.9 |상태-채널 그래프(Python)|
|커다란 건초 더미| 2.27 |유형이 지정된 소켓이 있는 구성요소의 파이프라인|
|피단틱 그래프| 1.84 |단일 다음 노드 상태 머신|
|LlamaIndex 작업 흐름| 0.14 |이벤트 기반 비동기 워크플로|
|AutoGen 그래프 흐름| 0.7.5 |메시지 전달 다중 에이전트 그래프|

## 워크로드

6개의 구현 모두 정확히 동일한 두 개의 그래프를 정의하고 한 번 컴파일됩니다.
(해당되는 경우) 핫 루프에서 호출합니다.

|ID|모양|상태|
|----|-------|-------|
|`seq`|3노드 체인 `a → b → c`|단일 `counter` 채널, 각 노드는 `counter+1`를 씁니다(감소기 덮어쓰기).|
|`par`|5명의 작업자를 팬아웃한 후 `summarizer`에 합류합니다.|`results: list`(리듀서 추가) + `count: int`; 각 작업자는 인덱스를 추가하고 요약자는 `len(results)`를 작성합니다.|

체크포인트는 모든 프레임워크에서 비활성화됩니다.

프레임워크별 워크로드 형태 변환이 필요한 포트 2개:

* **Haystack**에는 추가 감소기가 없습니다. 각 작업자는 자체적으로 방출합니다.
소켓을 입력하면 요약자가 목록 길이를 합산합니다. 같은 수
실행당 구성요소가 발송됩니다.
* **pydantic-graph**는 단일 다음 노드 상태 머신이며 다음을 수행할 수 없습니다.
팬 아웃. `par` 워크로드는 6노드 직렬 체인으로 에뮬레이션됩니다.
(`w1 → w2 → w3 → w4 → w5 → summ`). 결과에 표시됨 —
사과 대 사과 병렬 팬아웃 측정.
* **AutoGen**은 상태 채널이 아닌 메시지 전달입니다. 카운터는
문자 메시지 콘텐츠로 인코딩됩니다. 요약자는 수신 횟수를 계산합니다.
작업자 메시지. 동일한 그래프 모양, 다른 상태 모델.

## 결과

아래 **참조 실행**은 2026년 4월 22일에 측정되었습니다.
x86_64 Linux의 NeoGraph v3.0.0, g++ 13 릴리스 `-O3 -DNDEBUG`,
CPython3.12.3. NeoGraph: `bench_neograph`의 10회 중앙값.
Python 필드: 프레임워크당 3회 실행 중앙값. 버전: neograph v3.0.0,
langgraph 1.1.9, haystack-ai 2.28.0, pydantic-graph 1.85.1,
라마-인덱스-코어 0.14.21, 자동 생성-에이전트 채팅 0.7.5.

당시 마스터(2026-04-29)에 대한 재측정 내용은 아래와 같습니다.
참조 실행. `par` 행은 지원되는 실행을 모두 보고합니다.
명시적으로 정권: 현재 `worker_count=1` 기본값과
`set_worker_count_auto()`에 의해 활성화된 엔진 소유 풀. *참고* 참조
no-I/O 마이크로벤치마크가 이를 유지해야 하는 이유에 대한 표 뒤에
숫자는 따로.

![Engine-overhead benchmark: per-iteration latency and peak RSS](../docs/images/bench-engine-overhead.png)

### 반복당 오버헤드(μs, 낮을수록 좋음)

|뼈대|`seq`(3노드 체인)|`par`(팬아웃 5 + 조인)|`seq` 대 NeoGraph|`par` 대 NeoGraph|
|-----------|---------------------:|-------------------------:|-------------------:|-------------------:|
|**NeoGraph v3.0.0** *(참고, 2026-04-22)*| **5.0** | **11.8** | 1× | 1× |
|**NeoGraph 마스터** *(2026-04-29, 기본 `worker_count=1`)*| **5.25** | **14.4** | 1× | 1× |
|**네오그래프 마스터** *(2026-04-29, `set_worker_count_auto()`)*| **5.25** | **278** | 1× | 1× |
|헤이스택 2.28.0| 139.85 | 278.48 | 28.0× / 26.6× / 26.6× | 23.6× / 19.3× / 1.0× |
|피단틱 그래프 1.87.0| 227.14 | 280.26¹ | 45.4× / 43.3× / 43.3× | 23.7×¹ / 19.5×¹ / 1.0×¹ |
|랭그래프 1.1.10| 642.62 | 2,261.55 | 128.5× / 122.4× / 122.4× | 191.7× / 157.1× / 8.1× |
|LlamaIndex 워크플로 0.14.21| 1,564.54 | 4,373.76 | 312.9× / 298.0× / 298.0× | 370.7× / 303.7× / 15.7× |
|AutoGen GraphFlow 0.7.5| 3,126.86 | 7,281.08 | 625.4× / 595.6× / 595.6× | 617.0× / 505.6× / 26.2× |

가장 오른쪽 두 열에는 v3.0.0 참조/마스터 대비 세 가지 비율이 표시됩니다.
작업자=1 기본값 / 마스터 자동 작업자 모드 비교.

2026-04-29 재측정(위)은 행당 ±10% 내에서 2026-04-22 참조를 재현합니다(동일 머신, 동일한 툴체인, 동일한 워크로드). README의 헤드라인 주장(`seq`의 130× LangGraph, 600× AutoGen)은 마스터 HEAD를 유지합니다.

¹ pydantic-graph `par`는 직렬 6노드 에뮬레이션입니다.
팬아웃을 지원합니다. 병렬 워크로드가 아닙니다. 완전성을 위해 포함되었습니다.

### `par` 행에 대한 참고 사항(`seq`는 변경되지 않음)

`par` 14.4 → 278 µs 격차는
실제 작업을 수행하지 않는 5개 노드에 대한 엔진 소유 스레드 풀:

* **현재 `build()` / `compile()` 기본값: `worker_count=1`.** 아니요
엔진 소유 풀이 설치되었으므로 팬아웃이 호출자에게 디스패치됩니다.
집행자. 이것은 14.4 µs 행이며 다음에 대한 올바른 회귀 신호입니다.
노드가 스레드로부터 안전하지 않은 상태를 유지하는 CPU-작은 노드 또는 그래프입니다.
* **병렬 팬아웃은 명시적입니다.** `set_worker_count_auto()`는
풀을 `hardware_concurrency`로; `set_worker_count(N)`는 고정을 선택합니다.
천장. 이것은 278 µs 행입니다. 조정 비용은 여기에 표시됩니다.
각 작업자는 하나의 정수만 추가하지만 옆에는 무시할 수 있기 때문입니다.
100ms LLM 호출을 수행하고 독립 노드가 중첩되도록 허용합니다.

벤치마크 소스를 변경하지 않고 두 체제를 모두 실행합니다.

```bash
./build/bench_neograph 10000 5000 1
./build/bench_neograph 10000 5000 auto
```

세 번째 인수는 `par` 엔진에 적용되며 `1`(기본값)를 허용합니다.
`auto` 또는 양수 작업자 수. 출력에는 다음이 포함됩니다.
`config\tpar_workers\t...` 행은 저장된 결과가 실행 모드를 유지하도록 합니다.

헤드라인은 여전히 ​​유효합니다. NeoGraph는 테이블의 모든 행에서 승리했습니다.
프레임워크 및 구성에 따라 8×–600×. "199× 더 빠르다
`par`의 LangGraph보다 "참조는 2026-04-29에서 163×였습니다.
작업자=1 측정; 자동 작업자 모드는 마이크로벤치마크 오버헤드를 거래합니다.
차단 또는 CPU 바인딩 노드의 실제 동시성을 위해.

### 엔드투엔드 프로세스 지표

워밍업 + 두 워크로드를 포함한 전체 binary/script 런타임.
`seq` = 10,000 반복, `par` = 5,000 반복. 측정
`/usr/bin/time -f "%e s, %M KB"`.

|뼈대|총 경과 시간|피크 RSS|집행자|
|-----------|--------------:|---------:|---------:|
|**네오그래프 3.0**|**0.11초**|**4.5MB**|기본적으로 단일 스레드 io_context|
|피단틱 그래프|3.98초|35.1MB|단일 스레드 비동기(GIL)|
|커다란 건초 더미|3.85초|80.3MB|단일 스레드 비동기(GIL)|
|랭그래프|18.95초|60.1MB|단일 스레드 비동기(GIL)|
|LlamaIndex 작업 흐름|39.49초|101.4MB|단일 스레드 비동기(GIL)|
|AutoGen 그래프 흐름|63.29초|52.3MB|단일 스레드 비동기(GIL)|

NeoGraph 3.0의 기본 슈퍼스텝 루프는 코루틴을 실행합니다.
`run_sync`를 통한 단일 스레드 io_context; CPU 병렬 팬아웃은
`engine->set_worker_count(N)`를 통해 선택하세요. I/O-bound 노드의 경우
co_await 정지를 통해 단일 스레드가 여전히 중복되는 작업 부하.

## Linux ARM64 기준: Neoverse-N1

이는 별도의 기본 ARM64 플랫폼 기준선입니다.
회귀가 아닌 [#165](https://github.com/fox1245/NeoGraph/issues/165)
위의 x86_64 테이블과 비교합니다. 버전 `d7a6477`에 고정되어 있습니다.
자체 종속성이 설정되어 이후 측정이 계속 귀속될 수 있습니다.

|목|값|
|---|---|
|날짜| 2026-07-22 |
|운영체제|우분투 24.04, 리눅스 `6.17.0-1018-oracle`|
|건축학|`aarch64`|
|CPU|vCPU 4개, ARM Neoverse-N1|
|메모리|23GiB, 스왑 없음|
|컴파일러/CMake|GCC 13.3.0 / CMake 3.28.3|
|파이썬|CPython 3.12.3|
|네오그래프 개정판|`d7a6477`|

작업량 및 반복 횟수는 기본 벤치마크와 일치합니다: 10,000
각 그래프를 컴파일한 후 `seq` 반복 및 5,000번의 `par` 반복
한 번. NeoGraph는 10번 측정되었으며 각 Python 구현은 3번 측정되었습니다.
타임스; 표는 중앙값을 보고합니다. 체크포인트, 네트워크 I/O, 모델 호출,
수면이 비활성화되었습니다. 전체 프로세스 피크 RSS는 다음에서 유래되었습니다.
`/usr/bin/time -f "%e s, %M KB"`.

|뼈대|`seq` (µs/iter)|`par` (µs/iter)|`seq` 대 NeoGraph|`par` 대 NeoGraph|피크 RSS|
|---|---:|---:|---:|---:|---:|
|**네오그래프**| **9.50** | **21.80** | 1× | 1× |**4.35MB**|
|헤이스택 3.0.0| 153.44 | 329.67 | 16.2× | 15.1× |73.6MB|
|피단틱 그래프 1.87.0| 342.60 | 405.87¹ | 36.1× | 18.6×¹ |32.1MB|
|랭그래프 1.2.9| 1,037.55 | 3,289.22 | 109.2× | 150.9× |63.7MB|
|라마인덱스 0.14.23| 2,765.04 | 7,824.85 | 291.1× | 358.9× |96.9MB|
|자동 생성 0.7.5| 4,166.39 | 9,571.11 | 438.6× | 439.0× |47.8MB|

NeoGraph는 작업자=1 기본값을 사용했으므로 `par` 행은 토폴로지를 측정합니다.
엔진 소유 스레드 풀이 아닌 감속기 및 직렬 디스패치 오버헤드
실행. 별도의 병렬 실행을 위해 명시적인 `auto` 벤치마크 모드를 사용하세요.
팬아웃 측정; 두 가지 모드를 하나의 헤드라인으로 결합하지 마세요.

¹ pydantic-graph는 이 팬아웃 토폴로지를 모델링할 수 없습니다. `par` 행은
위에서 설명한 것과 동일한 직렬 6노드 에뮬레이션입니다. 버전 1.87.0이 고정되었습니다.
당시 2.15.0 API는 더 이상 벤치마크를 지원하지 않기 때문입니다.
`Graph(...)` 생성자. 프로세스는 CPU 고정 또는 cgroup 제한이 아닙니다.

## 숫자의 의미

1. **실행당 엔진 오버헤드는 Python 전체에서 ~29× ~ ~642×에 이릅니다.
field.** Haystack은 가장 적은 경쟁업체입니다(입력된 DAG).
소켓, 최소 런타임); 그럼에도 불구하고 시퀀스당 28.8배 더 많은 비용이 듭니다.
NeoGraph보다 더 좋습니다. 다른 쪽 끝에 AutoGen은 642×에 위치합니다.
실행당 다중 에이전트 상태 설정으로 인해 NeoGraph 비용이 발생합니다.
2. **NeoGraph 3.0은 두 축 모두에서 2.0보다 뛰어납니다.** 동기화 축소
하나의 코루틴 경로에 대한 비동기가 엔진을 회귀하지 않았습니다.
오버헤드 — 전체 코루틴 기계(`run_sync` + io_context
호출당)은 릴리스 빌드에서 5μs 미만인 반면, 2.0은
동기화 작업 흐름 경로에서 20.65μs를 알렸습니다.
3. **메모리 공간은 규모에 따라 NeoGraph를 선호합니다.
more.** 4.8MB(NeoGraph) 대 Python 필드 전체의 35~101MB.
SBC 클래스 대상(Raspberry Pi 클래스 RAM)에서는 다음과 같습니다.
내하중 측정법 - "편안하게 실행됨"과 "편안하게 실행됨"의 차이
"신중하게 실행하세요".
4. **병렬 팬아웃은 3.0에서 선택됩니다.** NeoGraph 2.x 출시
Taskflow의 작업 도용 풀이 기본값입니다. 3.0은
기본적으로 코루틴 경로(단일 스레드 디스패치, 저렴함) 및
다중 스레드 풀을 다음을 통해 옵트인으로 노출합니다.
`engine->set_worker_count(N)` — 에이전트에 대한 올바른 기본값
I/O-bound(LLM 대기 시간이 지배적)이고
그렇지 않으면 속도 향상 없이 스레드 생성 오버헤드를 지불합니다.

## 주의 사항 — 이 벤치가 NOT에서 측정하는 내용

* **실제 에이전트 워크로드.** LLM가 지배하는 파이프라인에 병목 현상이 발생함
공급자 대기 시간(호출당 100ms~10초)별. 엔진 오버헤드가 사라짐
그 규모로. 정신 모델: NeoGraph 3.0의 비용은 ~5 µs/call,
건초 더미 ~144 µs, LangGraph ~657 µs, LlamaIndex/AutoGen ~2–7 ms —
500ms API 왕복 이동 옆에는 모두 보이지 않습니다. 이 벤치가 중요하다
LLM가 아닌 노드, 밀도가 높은 에이전트 오케스트레이션 및 시작이 많은 노드의 경우
배포.
* **프레임워크에 적합한 워크로드.** AutoGen, LlamaIndex 및
pydantic-graph는 각각 패러다임에 맞게 최적화됩니다(다중 에이전트 채팅,
이벤트 기반 장기 실행 워크플로, 상태 머신 제어 흐름)
이 벤치는 운동을 하지 않는다고요. NeoGraph에서 측정합니다.
집 잔디.
* **체크포인트 처리량.** 각 프레임워크에서 지속성 활성화
직렬화 비용이 지배적이게 됩니다. 그것은 다른 벤치 마크입니다.
* **콜드 스타트.** 각 구현에는 10리터의 예열 루프가 포함됩니다.
측정 전. 전체 프로세스 번호에는 Python 인터프리터가 포함됩니다.
부팅(~200ms) 및 프레임워크 가져오기 시간은 매우 다양합니다(LlamaIndex
AutoGen은 상당한 나무를 가져옵니다).
* **공정성.** NeoGraph는 CMake `-DCMAKE_BUILD_TYPE=Release`로 구축되었습니다.
이는 GCC의 `-O3 -DNDEBUG`로 확인됩니다. 모든 Python 프레임워크는
현재 pip가 설치된 버전의 CPython 3.12 재고 —
일반적인 프로덕션 배포, 사용자 지정 조정 없음. 역사적 참고 사항:
이 README의 3.0 이전 버전은 `-O2`를 광고했습니다.
독립형 벤치 명령이 사용한 것; CMake 빌드는 항상
`Release`를 `-O3`로 해결했습니다.

## 낳다

```bash
# Build NeoGraph (Release — MUST set BUILD_TYPE explicitly; the
# empty default configures the build without -O3):
cmake -B build -DCMAKE_BUILD_TYPE=Release -DNEOGRAPH_BUILD_BENCHMARKS=ON
cmake --build build --target bench_neograph -j

./build/bench_neograph                   # defaults: seq=10000, par=5000

# Shared Python venv for every Python framework:
python3 -m venv /tmp/bench_venv
/tmp/bench_venv/bin/pip install \
    langgraph \
    haystack-ai \
    pydantic-graph \
    llama-index-core \
    "autogen-agentchat" "autogen-core" "autogen-ext"

# Run each bench (10k seq + 5k par matches the C++ side):
/tmp/bench_venv/bin/python benchmarks/bench_langgraph.py      10000 5000
/tmp/bench_venv/bin/python benchmarks/bench_haystack.py       10000 5000
/tmp/bench_venv/bin/python benchmarks/bench_pydantic_graph.py 10000 5000
/tmp/bench_venv/bin/python benchmarks/bench_llamaindex.py     10000 5000
/tmp/bench_venv/bin/python benchmarks/bench_autogen.py        10000 5000

# Peak RSS + wall time:
/usr/bin/time -f "%e s, %M KB" ./build/bench_neograph
```

출력 형식은 `workload<TAB>iters<TAB>total_ms<TAB>per_iter_us`입니다.
모든면에서 차이점을 찾는 것은 사소한 일입니다.

## 2026-04-19 번호에 사용된 환경

```
OS:        Linux 6.6.87.2-microsoft-standard-WSL2 (Ubuntu 24.04 userland)
CPU:       host CPU (8 logical cores exposed to WSL)
Compiler:  g++ 13.x, -std=c++20 -O2 -DNDEBUG
Python:    3.12.3 (system)
Versions:  langgraph 1.1.7, haystack-ai 2.27.0, pydantic-graph 1.84.1,
           llama-index-core 0.14.20, autogen-agentchat 0.7.5
```

숫자는 하드웨어에 따라 다르지만 비율은 안정적이어야 합니다.
~20% 이내.
