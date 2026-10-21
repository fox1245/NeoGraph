<!-- neograph-i18n: source=benchmarks/README.md locale=ko source_sha256=bbfba1a87c6a9a4f4f7c7a732df64ce07c6ddacc41c9b8ac7024042f05a40fba -->
# NeoGraph와 Python 그래프/파이프라인 프레임워크 비교 — 엔진 오버헤드 벤치마크

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

I/O, sleep, 모델 호출, 내구성 체크포인트가 없는 두 개의 소규모 워크로드에서 NeoGraph와 Python 오케스트레이션
프레임워크 다섯 개의 실행당 엔진 오버헤드를 측정하고, 각각의 메모리, 시작 시간, 크기도 함께 측정합니다.
측정 범위에는 노드 디스패치, 상태 채널 쓰기, 리듀서 호출이 들어갑니다.

이 페이지의 모든 표와 차트는 체크인된 기록 하나([`records/CURRENT`](records/CURRENT)가 가리키는 파일)에서
`scripts/render_bench_report.py`가 생성합니다. 기록에는 모든 셀의 원본 샘플과 세션의 출처 정보(커밋, 컴파일러와
플래그, CPU, 커널, 패키지 버전, 부하 평균, 스크립트의 sha256)가 들어 있습니다. 표가 기록과 다르면 테스트가 실패합니다.
서로 다른 세션에서 버전을 섞어 측정한 이전 표는 현재 무효로 알려진 항목에 대한 설명과 함께
[2026-04 이력](history/engine-overhead-2026-04.md)에 보존되어 있으며, 이 페이지의 표와 비교할 수 없습니다.
현재 NeoGraph `0.16.0` 레시피는 alpha SDK `0.4.0`과, 인터페이스 리비전·공유 세대(generation) 7에 맞는
헤더와 라이브러리를 요구합니다.

Program 승인(admission), JavaScript 제어, SQLite/PostgreSQL 런타임 비용은
[Program 비용 측정](../docs/PROGRAM_COST_MEASUREMENT.md)을 참고하세요. 그 문서의 측정 매트릭스는
저널과 체크포인트 작업을 포함하므로, 아래의 체크포인트 없는 Core 비교 결과로 제시해서는 안 됩니다.

## 코호트

NeoGraph와 다섯 프레임워크는 한 대의 머신에서 한 세션 안에 설치, 빌드, 측정했습니다. **HEAD**는 프레임워크 기본
브랜치의 최신 커밋을 표시된 커밋으로 git에서 설치한 것이고, **release**는 당시의 최신 PyPI 릴리스입니다. release
빌드는 매칭 셀만 실행하므로, 독자는 HEAD와 릴리스가 다른지 확인할 수 있습니다. HEAD 빌드가 설치나 임포트에 실패했다면
릴리스로 대체하고 표에 그렇게 표시합니다. NeoGraph는 표시된 `origin/master` 커밋을 기본 Release 구성(하드닝
플래그 포함)으로 빌드한 것입니다([엔진 상한](#engine-ceiling-and-the-hardening-delta) 참조).

<!-- bench-record:begin cohort -->
| 프레임워크 | 최신 릴리스 | HEAD (커밋, 날짜, 버전) |
| :--- | :--- | :--- |
| LangGraph | langgraph 1.2.16, langchain-core 1.6.12 | langgraph 1.2.16, langchain-core 1.6.12; `langchain@e6d83ef` (2026-10-20), `langgraph@3cd57c0` (2026-10-21) |
| Haystack | haystack-ai 3.4.0 | haystack-ai 3.5.0-rc0; `haystack@276746c` (2026-10-21) |
| pydantic-graph | pydantic-graph 2.59.0 | pydantic-graph 2.59.1.dev4+e0ccafa8a; `pydantic-ai@e0ccafa` (2026-10-21) |
| LlamaIndex Workflow | llama-index-core 0.14.25, llama-index-workflows 2.26.0 | llama-index-core 0.14.25, llama-index-workflows 2.26.0; `llama_index@619d746` (2026-10-14), `workflows-py@f3528e8` (2026-10-21) |
| AutoGen GraphFlow | autogen-agentchat 0.7.5, autogen-core 0.7.5 | autogen-agentchat 0.7.5, autogen-core 0.7.5; `autogen@004749e` (2026-10-16) |

호스트: ARM Neoverse-N1, 코어 4개, 커널 6.17.0-1018-oracle, Python 3.12.3, NeoGraph 빌드 컴파일러 g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0; CPU 거버너: 호스트가 노출하지 않음.

세션: 2026-10-21T13:24:06Z ~ 2026-10-21T13:59:49Z UTC; 1분 부하 평균은 첫 단계 시작 시 0.26(유휴 상태를 기다린 뒤), 종료 시 0.97(세션 자신의 프로세스 포함); 교차 라운드 12회; 기록 `2026-10-21-compare-arm-neoverse-n1.json`.

NeoGraph 엔진: 커밋 `db0fc651e2c9`; 하니스 트리: 커밋 `10d6c77a7d42`. 하니스 커밋은 세션을 실행한 로컬 커밋이므로 이 저장소의 기록에 없을 수 있습니다. 기록은 수정하지 않으며, 하니스 스크립트마다 sha256이 들어 있어 파일을 식별합니다. 이 트리의 파일은 해시된 파일과 바이트 단위로 같습니다. 예외는 `scripts/render_bench_report.py`(이 표를 렌더링한 버전의 해시는 아래에 있음)와 `benchmarks/compare/`의 `port_llamaindex.py`, `port_autogen.py`(각각 세션 뒤에 주석을 하나 고침)입니다.

표는 `scripts/render_bench_report.py`(sha256 `db933a39bedf3733`…)가 렌더링했습니다.
<!-- bench-record:end cohort -->

## 워크로드

모든 프레임워크가 같은 두 워크로드를 포팅합니다. 그래프, 파이프라인, 워크플로는 측정 구간 밖에서 한 번 만들고
(프레임워크가 허용하면 인스턴스를 재사용), 측정 루프에서 호출합니다. 상태와 토폴로지 조정은 아래에 나열합니다.

| Id | 형태 | 상태 |
|----|-------|-------|
| `seq` | 3노드 체인 `a → b → c` | 단일 `counter` 채널, 각 노드는 `counter+1`을 기록 (overwrite 리듀서) |
| `par` | 워커 5개로 팬아웃한 뒤 `summarizer`에서 조인 | `results: list` (append 리듀서) + `count: int`; 각 워커는 자기 인덱스를 추가하고 summarizer는 `len(results)`를 기록 |

모든 실행은 검증합니다. 최종 상태가 기대한 값(counter 3, 인덱스 다섯 개와 count 5)이어야 합니다. 검증은 각 샘플의
마지막 실행과 시계가 멈춘 뒤 추가로 한 번 더 실행한 결과에 적용됩니다. 정답이 틀렸거나 실행이 실패했거나 완료된 실행이
없는 셀은 오류 셀이며, 그 셀의 시간은 공개하지 않고 기록에 오류로 남깁니다.

어떤 프레임워크에도 내구성 체크포인터를 설정하지 않았습니다. AutoGen은 실행마다 메시지 스레드를 누적하며, 이는
프레임워크의 실행당 비용입니다.

포팅 노트:

* **Haystack**에는 append 리듀서가 없어서 각 워커가 자신의 타입 소켓으로 출력하고 summarizer가 리스트 길이를 합산합니다.
  실행당 디스패치되는 컴포넌트 수는 같습니다. 벤치마크 프로세스에서는 텔레메트리를 끕니다.
* **pydantic-graph**의 API는 이제 `GraphBuilder` 하나입니다. 스텝은 `async def`이고, 여러 스텝으로 가는 엣지는 브로드캐스트하며,
  조인이 그들을 기다립니다. `par`는 실제 팬아웃입니다. 시작 노드에서 스텝 다섯 개로 보낸 뒤 `reduce_list_append`로 조인하고
  summarizer 스텝으로 넘깁니다. 문서에서 처음 보여 주는 `BaseNode` 클래스는 다음 노드를 정확히 하나만 반환하므로 그
  변형의 `par`는 6노드 직렬 체인이며, 관용 기본값 블록에만 나옵니다.
* **LlamaIndex**는 `ctx.collect_events`로 조인하므로 summarizer 스텝 함수가 결과 이벤트마다 한 번씩(다섯 번, 대기 중인
  네 번은 `None` 반환) 실행됩니다. `Workflow` 인스턴스는 여러 실행에 걸쳐, 그리고 동시에도 재사용할 수 있으므로(프로브 참조)
  매칭 행은 인스턴스 하나를 재사용하고, 관용 기본값 행은 실행마다 새 인스턴스를 만듭니다.
* **AutoGen**은 상태 채널이 아니라 메시지 전달 방식입니다. 카운터는 메시지 텍스트로 인코딩하고, summarizer는 받은 워커
  메시지를 보고하며, 새 작업 전에 팀을 리셋하는 AutoGen 문서의 방식대로 `reset()`을 매 실행 전에 호출합니다. 이는 매니저의
  메시지 스레드를 지웁니다. `reset()` 없이 반복 실행해도 올바른 답이 나오며(0.7.5: 그래프가 끝나면 스스로 실행 상태를
  초기화함), 그 경우는 여기서 측정하지 않았습니다.
* **LangGraph** 노드는 `ainvoke`에서는 `async def`, `invoke`에서는 일반 `def`이며 그래프는 그 외에는 동일합니다.

## 매칭 레짐: 비율이 있는 유일한 블록

두 가지 실행 레짐을 비교하며, 각각 자체 NeoGraph 행이 있습니다.

* **단일 스레드.** `worker_count=1`의 NeoGraph 대 각 프레임워크의 `async def` 노드 코루틴 경로: LangGraph `ainvoke`,
  코루틴 컴포넌트를 쓴 Haystack `run_async`, pydantic-graph(유일한 경로), `async def` 스텝을 쓴 LlamaIndex, AutoGen(유일한 경로).
  노드는 호출자의 스레드에서 실행됩니다.
* **스레드 풀.** `set_worker_count_auto()`의 NeoGraph 대 스레드 풀 경로가 있고 측정한 프레임워크의 해당 경로: LangGraph `invoke`
  (팬아웃 스텝을 langchain-core가 호출마다 새로 만드는 스레드 풀에서 실행)와 동기 전용 컴포넌트를 쓴 Haystack `run_async`
  (각 컴포넌트를 `asyncio.to_thread`로 실행). 그런 경로가 없거나 측정하지 않은 프레임워크는 이유와 함께 `해당 없음` 행으로
  표시하며, 에뮬레이션하지 않습니다. LlamaIndex Workflow는 후자입니다. 일반 `def` 스텝을 받아 이벤트 루프의 기본 실행기에서
  실행하지만, 그 경로는 이 코호트에 없습니다. 풀은 기본 너비를 그대로 씁니다(NeoGraph: 하드웨어 스레드당 하나, 여기서는 4개;
  Python 실행기: `min(32, 코어 수 + 4)`). [`dr_compare` 가이드](dr_compare/README.md)는 이를 맞춥니다. `seq`에서는 모든 스텝에
  준비된 노드가 하나뿐이어서 NeoGraph와 LangGraph `invoke` 모두 인라인으로 실행하므로 어느 쪽 풀도 쓰이지 않습니다. 그래도
  LangGraph 행은 해당 프레임워크의 기본 동기 경로입니다.

비율은 프레임워크의 중앙값을 같은 레짐과 워크로드의 NeoGraph 행 중앙값으로 나눈 값입니다. NeoGraph의 `par`는 워커 모드에 따라
달라지며 그 범위는 두 번째 표 아래에 적습니다. 하나의 비율로 요약하지 않습니다.

표 머리글 아래의 행은 같은 프레임워크의 HEAD 빌드와 최신 릴리스입니다.

### 단일 스레드

<!-- bench-record:begin matched_single -->
| 프레임워크 | 출처 | `seq` 실행당 µs | `par` 실행당 µs | `seq` 대 NeoGraph | `par` 대 NeoGraph |
| :--- | :--- | ---: | ---: | ---: | ---: |
| **NeoGraph `worker_count=1`** |  | **9.72 [9.68, 10.1]** | **21.5 [21.4, 23.6]** | 1× | 1× |
| Haystack — Pipeline.run_async, 코루틴 컴포넌트 | HEAD `276746c` (3.5.0-rc0) | 272 [266, 275] | 499 [497, 507] | 27.9× | 23.2× |
| Haystack — Pipeline.run_async, 코루틴 컴포넌트 | 릴리스 3.4.0 | 273 [266, 276] | 501 [499, 505] | 28.1× | 23.3× |
| pydantic-graph — GraphBuilder 스텝, 실제 팬아웃 | HEAD `e0ccafa` (2.59.1.dev4+e0ccafa8a) | 483 [476, 494] | 1,277 [1,252, 1,308] | 49.7× | 59.3× |
| pydantic-graph — GraphBuilder 스텝, 실제 팬아웃 | 릴리스 2.59.0 | 487 [480, 501] | 1,271 [1,256, 1,294] | 50.0× | 59.1× |
| LangGraph — ainvoke, async def 노드 | HEAD `3cd57c0` (1.2.16) | 1,248 [1,232, 1,273] | 1,777 [1,754, 1,808] | 128× | 82.6× |
| LangGraph — ainvoke, async def 노드 | 릴리스 1.2.16 | 1,258 [1,242, 1,280] | 1,773 [1,750, 1,792] | 129× | 82.4× |
| LlamaIndex Workflow — 워크플로 인스턴스 하나를 재사용 | HEAD `619d746` (0.14.25) | 2,272 [2,219, 2,297] | 7,069 [6,937, 7,136] | 234× | 329× |
| LlamaIndex Workflow — 워크플로 인스턴스 하나를 재사용 | 릴리스 0.14.25 | 2,364 [2,339, 2,396] | 7,393 [7,235, 7,458] | 243× | 344× |
| AutoGen GraphFlow — GraphFlow reset 후 run | HEAD `004749e` (0.7.5) | 4,192 [4,135, 4,286] | 9,543 [9,346, 9,734] | 431× | 444× |
| AutoGen GraphFlow — GraphFlow reset 후 run | 릴리스 0.7.5 | 4,224 [4,115, 4,274] | 9,521 [9,405, 9,645] | 434× | 443× |

수치는 12회 교차 라운드의 중앙값이며 괄호는 라운드 간 [최소, 최대]입니다. 각 라운드는 모든 셀을 무작위 순서로 한 번씩, 셀마다 새 프로세스로 실행합니다. 비율은 중앙값끼리 나눈 값입니다.
<!-- bench-record:end matched_single -->

![엔진 오버헤드 벤치마크, 매칭 단일 스레드 레짐: 실행당 마이크로초](../docs/images/bench-engine-overhead.svg)

### 스레드 풀

<!-- bench-record:begin matched_pool -->
| 프레임워크 | 출처 | `seq` 실행당 µs | `par` 실행당 µs | `seq` 대 NeoGraph | `par` 대 NeoGraph |
| :--- | :--- | ---: | ---: | ---: | ---: |
| **NeoGraph `set_worker_count_auto()`** |  | **10.2 [10.1, 10.5]** | **86.0 [82.8, 86.9]** | 1× | 1× |
| Haystack — Pipeline.run_async, 스레드에서 실행되는 동기 컴포넌트 | HEAD `276746c` (3.5.0-rc0) | 759 [745, 779] | 1,424 [1,389, 1,460] | 74.8× | 16.6× |
| Haystack — Pipeline.run_async, 스레드에서 실행되는 동기 컴포넌트 | 릴리스 3.4.0 | 757 [738, 780] | 1,426 [1,388, 1,459] | 74.5× | 16.6× |
| pydantic-graph |  | 해당 없음 — 스레드 풀 경로 없음: 스텝은 코루틴이며 일반 함수는 거부됨 |  |  |  |
| LangGraph — invoke, 일반 def 노드 | HEAD `3cd57c0` (1.2.16) | 1,063 [1,049, 1,079] | 3,184 [3,146, 3,224] | 105× | 37.0× |
| LangGraph — invoke, 일반 def 노드 | 릴리스 1.2.16 | 1,066 [1,053, 1,088] | 3,194 [3,163, 3,301] | 105× | 37.1× |
| LlamaIndex Workflow |  | 해당 없음 — 측정하지 않음: 일반 `def` 스텝을 받아 이벤트 루프의 기본 실행기에서 실행하며, 이 코호트는 그 스레드 풀 경로를 측정하지 않음 |  |  |  |
| AutoGen GraphFlow |  | 해당 없음 — 스레드 풀 경로 없음: 단일 스레드 에이전트 런타임 하나 |  |  |  |

NeoGraph `par`는 `worker_count=1`에서 21.5 µs, `set_worker_count_auto()`에서 86.0 µs입니다 (워커 스레드 4개).
<!-- bench-record:end matched_pool -->

## 관용 기본값: 절대 시간만, 비율 없음

각 프레임워크의 공식 문서가 가장 먼저 보여 주는 API입니다. 이 행들은 서로 다른 일을 하므로(실행마다 구성, 스레드 전환, 직렬 체인)
절대 시간만 제시하고 비율은 만들지 않습니다.

<!-- bench-record:begin idiomatic -->
| 프레임워크 | 측정한 API | `seq` 실행당 µs | `par` 실행당 µs |
| :--- | :--- | ---: | ---: |
| **NeoGraph** | **NeoGraph `run()` (기본 `worker_count=1`)** | **9.72 [9.68, 10.1]** | **21.5 [21.4, 23.6]** |
| Haystack | Pipeline.run, 동기 컴포넌트 | 143 [143, 144] | 321 [318, 325] |
| pydantic-graph | GraphBuilder 스텝, 실제 팬아웃 | 483 [476, 494] | 1,277 [1,252, 1,308] |
| pydantic-graph | BaseNode 클래스(par는 직렬 체인) | 535 [525, 547] | 818 [802, 823] |
| LangGraph | invoke, 일반 def 노드 | 1,063 [1,049, 1,079] | 3,184 [3,146, 3,224] |
| LlamaIndex Workflow | 실행마다 새 워크플로 인스턴스 | 2,615 [2,577, 2,656] | 7,536 [7,408, 7,729] |
| AutoGen GraphFlow | GraphFlow reset 후 run | 4,192 [4,135, 4,286] | 9,543 [9,346, 9,734] |
| AutoGen GraphFlow | 실행마다 새 GraphFlow | 5,274 [5,239, 5,372] | 11,280 [11,082, 11,549] |
<!-- bench-record:end idiomatic -->

<a id="engine-ceiling-and-the-hardening-delta"></a>
## 엔진 상한과 하드닝 차이

위의 NeoGraph 행은 기본 Release 빌드이며, `-O3 -DNDEBUG`에 `-D_GLIBCXX_ASSERTIONS`, `-fstack-protector-strong`,
`-D_FORTIFY_SOURCE=2`, 위치 독립 코드, RELRO가 추가됩니다. 이를 뺀 두 번째 빌드(`-DNEOGRAPH_ENABLE_HARDENING=OFF`에, 배포판
컴파일러가 기본으로 켜는 항목을 끄기 위한 `-U_FORTIFY_SOURCE -fno-stack-protector -fno-stack-clash-protection`)가 엔진 상한입니다.
두 빌드의 플래그와 각 바이너리가 컴파일될 때의 매크로는 표 아래에 출력했습니다.

<!-- bench-record:begin ceiling -->
| 빌드 | `seq`, `worker_count=1` | `par`, `worker_count=1` | `par`, `set_worker_count_auto()` |
| :--- | ---: | ---: | ---: |
| 기본 Release 빌드(하드닝) — 기준 수치 | 9.72 [9.68, 10.1] | 21.5 [21.4, 23.6] | 86.0 [82.8, 86.9] |
| 하드닝 없는 엔진 상한 | 9.88 [9.86, 10.6] | 20.7 [20.5, 22.2] | 84.6 [81.9, 86.5] |
| 하드닝 ÷ 비하드닝 | 0.98× | 1.04× | 1.02× |

| 빌드 | µs |
| :--- | ---: |
| `seq`, 모든 수명주기 이벤트를 만들어 콜백으로 전달 | 10.7 [10.7, 11.6] |

기본 빌드의 플래그와 컴파일러 매크로(바이너리와 빌드 트리에서 출력):

```text
[hardened] build type: Release; g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
[hardened] NEOGRAPH_ENABLE_HARDENING: ON
[hardened] compile: /usr/bin/c++ -DASIO_NO_DEPRECATED -DASIO_STANDALONE -DNEOGRAPH_BENCH_BUILD_TYPE=\"Release\" -DNEOGRAPH_STATIC_BUILD -I<work>/ng/include -I<work>/ng/deps/yyjson -I<work>/ng/deps/asio/include -I<work>/build-hardened/generated -I<work>/build-hardened/_deps/neograph_schemaprovider-src/src -I<work>/build-hardened/_deps/neograph_schemaprovider-build/generated -O3 -DNDEBUG -std=gnu++20 -fPIE -D_GLIBCXX_ASSERTIONS -fstack-protector-strong -D_FORTIFY_SOURCE=2 -o CMakeFiles/bench_neograph.dir/benchmarks/bench_neograph.cpp.o -c <work>/ng/benchmarks/bench_neograph.cpp
[hardened] macros seen by the binary: _GLIBCXX_ASSERTIONS=True, _FORTIFY_SOURCE=2, stack protector=strong, PIE=True
[unhardened] build type: Release; g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
[unhardened] CMAKE_CXX_FLAGS: -U_FORTIFY_SOURCE -fno-stack-protector -fno-stack-clash-protection
[unhardened] NEOGRAPH_ENABLE_HARDENING: OFF
[unhardened] compile: /usr/bin/c++ -DASIO_NO_DEPRECATED -DASIO_STANDALONE -DNEOGRAPH_BENCH_BUILD_TYPE=\"Release\" -DNEOGRAPH_STATIC_BUILD -I<work>/ng/include -I<work>/ng/deps/yyjson -I<work>/ng/deps/asio/include -I<work>/build-unhardened/generated -I<work>/build-unhardened/_deps/neograph_schemaprovider-src/src -I<work>/build-unhardened/_deps/neograph_schemaprovider-build/generated -U_FORTIFY_SOURCE -fno-stack-protector -fno-stack-clash-protection -O3 -DNDEBUG -std=gnu++20 -fPIE -o CMakeFiles/bench_neograph.dir/benchmarks/bench_neograph.cpp.o -c <work>/ng/benchmarks/bench_neograph.cpp
[unhardened] macros seen by the binary: _GLIBCXX_ASSERTIONS=False, _FORTIFY_SOURCE=0, stack protector=none, PIE=True
```
<!-- bench-record:end ceiling -->

## 메모리

모든 수치는 프로세스 전체 기준이며 Python은 **인터프리터와 임포트한 프레임워크를 포함**합니다(NeoGraph는 C++ 런타임과
SchemaProvider 런타임 포함). 배포된 애플리케이션의 메모리가 아닙니다.

* **유휴**는 프레임워크를 임포트한 뒤(NeoGraph는 엔진을 만든 뒤) 실행을 시작하기 전의 프로세스입니다. 처음 두 행은 빈 기준선으로,
  아무것도 임포트하지 않은 Python 인터프리터와 NeoGraph 바이너리입니다.
* **최대**는 매칭 셀의 측정 루프가 끝난 시점의 `VmHWM`이고, 증가량은 최대에서 유휴를 뺀 값입니다.
* **실행 중인 run당 한계 메모리**는 N개의 run을 하나의 노드 안에 멈춰 둔 상태(`seq`는 가운데 노드, `par`는 워커 다섯 개가 결과를 쓴 뒤의
  summarizer)로 단일 이벤트 루프(NeoGraph는 `io_context` 스레드 하나)에서 유지하고 그 순간의 프로세스 `VmRSS`와 `Pss`를 읽습니다.
  수치는 run 하나를 끝내고 할당자에게 빈 메모리 반환을 요청한 뒤, N=64와 N=1의 차이를 63으로 나눈 값입니다. AutoGen은 실행 중인 run마다
  팀이 하나 필요하여 측정 구간 안에서 만들고, 나머지 프레임워크는 컴파일된 그래프나 워크플로 인스턴스 하나를 공유합니다.

<!-- bench-record:begin memory -->
| 프레임워크 (HEAD) | 유휴: 임포트만, 실행 없음 (MiB) | `seq` 루프 중 최대 (MiB) | `par` 루프 중 최대 (MiB) | 유휴 대비 증가, `seq` / `par` (MiB) |
| :--- | ---: | ---: | ---: | ---: |
| Python 인터프리터만, 임포트 없음 | 10.9 | — | — | — |
| **NeoGraph**: 엔진을 만든 바이너리, 실행 없음 | 5.3 | 6.1 | 6.2 | 0.8 / 0.9 |
| Haystack | 63.5 | 70.2 | 70.2 | 6.7 / 6.7 |
| pydantic-graph | 25.1 | 26.2 | 26.2 | 1.1 / 1.2 |
| LangGraph | 62.8 | 63.0 | 63.1 | 0.2 / 0.3 |
| LlamaIndex Workflow | 35.5 | 36.7 | 36.7 | 1.1 / 1.2 |
| AutoGen GraphFlow | 42.2 | 42.7 | 42.8 | 0.5 / 0.5 |

**실행 중인 run당 한계 메모리**

| 프레임워크 (HEAD) | `seq` run당 KiB (RSS / PSS) | `par` run당 KiB (RSS / PSS) |
| :--- | ---: | ---: |
| **NeoGraph** | 33.9 / 33.8 | 49.9 / 50.0 |
| Haystack | 14.2 / 14.2 | 27.3 / 27.4 |
| pydantic-graph | 24.3 / 24.4 | 57.8 / 57.7 |
| LangGraph | 37.1 / 37.3 | 93.8 / 93.8 |
| LlamaIndex Workflow | 58.5 / 58.3 | 119.5 / 119.5 |
| AutoGen GraphFlow | 96.4 / 96.3 | 174.1 / 174.1 |
<!-- bench-record:end memory -->

## 시작 시간과 크기

**콜드 스타트**는 부모가 프로세스를 생성하는 순간부터 첫 `seq` 실행이 끝나고 검증되는 순간까지의 시간입니다. 부모는 프로세스를
생성하기 직전에 `CLOCK_MONOTONIC`을 읽고, 프로세스는 실행이 끝났을 때 `CLOCK_MONOTONIC`을 출력합니다. 로더 또는 인터프리터 시작, 임포트(Python)
또는 정적 초기화(NeoGraph), 그래프 빌드, 실행 한 번이 포함되며 페이지 캐시는 데워져 있고 바이트코드 캐시가 있는 상태입니다.
프레임워크마다 새 프로세스 20개를 프레임워크 사이에 교차하여 실행하고, 수치는 중앙값과 [최소, 최대]입니다.

**크기.** 네이티브 바이너리는 `strip` 후에 측정하고, `libneograph_core.a`는 빌드된 그대로의 오브젝트 아카이브이며, WebAssembly 모듈은
WebAssembly 워크플로의 크기 게이트에서 가져왔습니다. Python 환경은 프레임워크와 전체 의존성 묶음을 담은 새 가상 환경의 `site-packages`
디스크 크기에서 빈 가상 환경을 뺀 값이며 패키지 수를 함께 적습니다. 네이티브 실행 파일, WebAssembly 모듈, Python 환경은
**서로 다른 배포 단위**이므로 표에는 나란히 놓되 그 사이에 비율은 만들지 않습니다.

<!-- bench-record:begin footprint -->
| 프레임워크 (HEAD) | 프로세스 시작부터 첫 `seq` 결과까지, ms | 임포트 / 빌드 / 첫 실행 (중앙값 ms) |
| :--- | ---: | ---: |
| **NeoGraph** | **2.49 [2.40, 10.7]** | — / 0.55 / 0.09 |
| Haystack | 561 [550, 583] | 395 / 0.84 / 99.6 |
| pydantic-graph | 153 [149, 157] | 72.0 / 0.56 / 12.8 |
| LangGraph | 756 [735, 775] | 683 / 0.72 / 4.13 |
| LlamaIndex Workflow | 314 [309, 330] | 219 / 22.8 / 4.63 |
| AutoGen GraphFlow | 331 [326, 345] | 257 / 0.73 / 8.05 |

| 항목 | 배포 단위 | 크기 | 집계 대상 |
| :--- | :--- | ---: | :--- |
| NeoGraph `bench_footprint` (기본 하드닝 Release) | 네이티브 실행 파일 하나 | 7.15 MB | 최소 앱: 노드를 등록하고 `seq`를 컴파일해 한 번 실행; strip 후 |
| NeoGraph `bench_neograph` (기본 하드닝 Release) | 네이티브 실행 파일 하나 | 7.15 MB | 벤치마크 바이너리; strip 후 |
| NeoGraph `libneograph_core.a` (기본 하드닝 Release) | 라이브러리 파일 하나 | 25.84 MB | 빌드된 그대로의 정적 아카이브 |
| NeoGraph `bench_footprint` (MinSizeRel + LTO(`-Os`)) | 네이티브 실행 파일 하나 | 2.82 MB | 최소 앱: 노드를 등록하고 `seq`를 컴파일해 한 번 실행; strip 후 |
| NeoGraph `bench_footprint` (하드닝 없는 Release) | 네이티브 실행 파일 하나 | 6.49 MB | 최소 앱: 노드를 등록하고 `seq`를 컴파일해 한 번 실행; strip 후 |
| NeoGraph `bench_neograph` (하드닝 없는 Release) | 네이티브 실행 파일 하나 | 6.49 MB | 벤치마크 바이너리; strip 후 |
| NeoGraph `libneograph_core.a` (하드닝 없는 Release) | 라이브러리 파일 하나 | 23.86 MB | 빌드된 그대로의 정적 아카이브 |
| NeoGraph `bench_footprint` (공유 `libneograph_core.so`) | 네이티브 실행 파일 하나 | 0.26 MB | 실행 파일만; strip 후 |
| NeoGraph 최소 앱의 공유 라이브러리 배포 | 실행 파일과 그 공유 라이브러리 | 10.44 MB | 실행 파일과 NeoGraph·SDK 공유 라이브러리 7개; strip 후 |
| NeoGraph 그래프 스모크 `smoke.wasm` (Emscripten 6.0.13, -O3 -flto, Release) | WebAssembly 모듈 하나 | 5.53 MB / 1.61 MB gzip | CI의 WebAssembly 워크플로 크기 게이트, 단일 스레드 작업, 커밋 db0fc651 |
| NeoGraph JavaScript 모듈 `neograph_module.wasm` (Emscripten 6.0.13, -O3 -flto, Release) | WebAssembly 모듈 하나 | 6.29 MB / 1.84 MB gzip | CI의 WebAssembly 워크플로 크기 게이트, 단일 스레드 작업, 커밋 db0fc651 |
| Haystack (HEAD 설치) | Python 환경: 프레임워크와 전체 의존성 묶음 | 140.75 MB | 패키지 42개; site-packages에서 빈 venv를 뺀 값 |
| pydantic-graph (HEAD 설치) | Python 환경: 프레임워크와 전체 의존성 묶음 | 12.03 MB | 패키지 9개; site-packages에서 빈 venv를 뺀 값 |
| LangGraph (HEAD 설치) | Python 환경: 프레임워크와 전체 의존성 묶음 | 65.46 MB | 패키지 38개; site-packages에서 빈 venv를 뺀 값 |
| LlamaIndex Workflow (HEAD 설치) | Python 환경: 프레임워크와 전체 의존성 묶음 | 216.19 MB | 패키지 62개; site-packages에서 빈 venv를 뺀 값 |
| AutoGen GraphFlow (HEAD 설치) | Python 환경: 프레임워크와 전체 의존성 묶음 | 36.93 MB | 패키지 11개; site-packages에서 빈 venv를 뺀 값 |
<!-- bench-record:end footprint -->

## 프로브

행을 어떻게 읽어야 하는지를 좌우하는 질문에 같은 세션에서 답합니다. (1) LlamaIndex 워크플로 인스턴스를 여러 실행에 재사용할 수 있는가,
(2) LangGraph 팬아웃 비용 중 호출마다 만드는 스레드 풀의 몫은 얼마이고 Haystack 워커는 어느 스레드에서 실행되는가,
(3) pydantic-graph는 async 전용인가, 팬아웃은 실제인가, (4) 하드닝은 엔진에 얼마의 비용을 주는가,
(5) 기존의 두 하니스가 같은 pydantic-graph 호출을 왜 227 µs와 70~100 µs로 측정했는가.

<!-- bench-record:begin probes -->
| # | 결과 |
| :--- | :--- |
| 1 | LlamaIndex: `Workflow` 인스턴스 하나를 반복 실행하고 동시에도 실행할 수 있음 — 순차 실행 [3, 13], 동시 실행 [3, 103], llama-index-workflows 2.26.0. |
| 1 | LlamaIndex 측정: `seq`는 재사용 인스턴스 2,272 µs, 실행마다 새 인스턴스 2,615 µs; `par`는 7,069 µs와 7,536 µs. |
| 2 | LangGraph: `invoke`는 다섯 워커를 풀 스레드 1개에서 실행하며 호출 스레드는 쓰지 않음; `async def` 노드의 `ainvoke`는 호출 스레드에서 실행. 측정: `par`는 `invoke` 3,184 µs 대 `ainvoke` 1,777 µs (1.79배), `seq`는 1,063 µs 대 1,248 µs (0.85배). |
| 2 | Haystack: 동기 컴포넌트에서 `run_async`는 워커를 호출 스레드가 아닌 스레드 3개로 넘김; `run_async` 컴포넌트에서는 `run`처럼 호출 스레드에서 실행. |
| 3 | pydantic-graph 2.59.1.dev4+e0ccafa8a: `Graph.run`은 코루틴이고 `run_sync`는 이벤트 루프에서 이를 구동할 뿐; 일반 스텝 함수는 거부됨(TypeError: object int can't be used in 'await' expression); 각각 50 ms 대기하는 스텝 다섯 개가 스레드 1개에서 52 ms에 끝나므로 팬아웃은 실제이며 단일 스레드. |
| 4 | `bench_neograph`의 하드닝 차이: 기본 하드닝 빌드 대 비하드닝 빌드, `seq` 0.98배, `worker_count=1`의 `par` 1.04배, 워커 풀의 `par` 1.02배. |
| 5 | pydantic-graph 1.x, 레거시 `Graph(nodes=[...])` API, 이 호스트에서 연달아 측정: await 루프 79.3 µs/run, 같은 run을 gather한 태스크 87.2 µs, 하나의 태스크 안의 루프 79.2 µs, 순환 GC 끈 경우 79.2 µs. 하니스의 형태로는 기존의 227 µs 대 70~100 µs를 설명할 수 없으며, 두 수치는 서로 다른 세션과 환경에서 나왔음. |
| 5 | 같은 레거시 pydantic-graph 루프에 선택 패키지 `logfire`를 환경에 설치하면 run당 498 µs로, 깨끗한 환경 (79.3 µs)의 6.3배. 이 패키지가 없으면 계측 스팬은 아무 일도 하지 않고, 있으면 실제 스팬이 되므로 이를 포함한 공유 환경은 수치를 바꿈. |
<!-- bench-record:end probes -->

## 수치가 말해 주는 것

1. 비율은 해당 레짐 쌍, 워크로드, 호스트, 코호트 표의 버전에만 해당합니다. NeoGraph의 `par`는 두 워커 모드 사이에서 스레드 풀 표 아래에
   적은 만큼 달라지므로, 하나의 "N배 빠름" 수치로 엔진을 설명할 수 없습니다.
2. 각 행은 프레임워크와 워크로드의 측정값이며, 비용의 원인을 분리해서 설명하지 않습니다. 각 프레임워크가 기본으로 실행마다 하는 일
   (콜백, 스팬, 상태 스냅샷, 메시지 스레드)이 포함됩니다.
3. 워커 1 디스패치와 엔진이 소유한 스레드 풀은 서로 다른 실행 레짐입니다. 의도한 워크로드에 대해 둘 다 측정하세요. 작업이 없는 팬아웃
   벤치마크는 조율 비용을 드러내지만 모델이나 I/O 지연을 예측하지는 않습니다.
4. 메모리 수치에는 인터프리터와 임포트한 프레임워크가 들어 있습니다. 배포된 Python 서비스에서 엔진의 몫은 프로세스 전체 최대값이 아니라
   유휴 대비 증가량과 실행당 한계 수치입니다.

## 주의 사항 — 이 벤치마크가 측정하지 않는 것

* **실제 에이전트 워크로드.** 프레임워크 비교에는 모델 추론, 네트워크 요청, 도구 I/O가 없습니다. 이런 비용이 애플리케이션을
  지배할 수 있으므로 이 표들은 엔드 투 엔드 에이전트 속도 향상을 입증하지 않습니다.
* **프레임워크에 맞는 워크로드.** AutoGen, LlamaIndex, pydantic-graph는 각각 이 벤치마크가 다루지 않는 패러다임(멀티 에이전트 대화,
  이벤트 기반 장기 실행 워크플로, 상태 머신 제어 흐름)에 최적화되어 있습니다.
* **체크포인트 처리량.** 각 프레임워크에서 영속성을 켜면 직렬화 비용이 지배하게 됩니다. 그것은 다른 벤치마크입니다.
* **한 호스트, 한 세션.** 코호트는 4코어 Arm 머신 한 대에서 실행했습니다. 비율은 플랫폼과 프레임워크 버전에 따라 움직이며,
  HEAD와 릴리스 행은 버전만으로 얼마나 움직이는지 보여 줍니다.
* **공정성.** NeoGraph는 하드닝 플래그를 포함한 기본 Release 빌드이고(위에 출력), 하드닝 없는 행은 상한입니다. 모든 Python 프레임워크는
  각자의 가상 환경에서 기록에 남은 의존성 버전의 기본 CPython 3.12를 쓰며 별도 튜닝이 없습니다. 한 프레임워크를 위해 설치한 패키지가
  다른 프레임워크의 비용을 바꾸지 않도록 하기 위해서입니다.

## 재현 방법

현재 소스 빌드에는 Core 전용 벤치마크를 포함해 외부 `SchemaProvider::runtime` 패키지가 필요합니다. CMake 3.20 이상은
명시한 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`, 설치된 런타임, 고정된 공개 GitHub 소스 아카이브 순으로 선택합니다.
`NEOGRAPH_FETCH_SCHEMAPROVIDER`의 기본값은 ON이며, 설치된 패키지나 명시한 소스로 오프라인 빌드하려면 OFF로 설정하세요.
명시한 체크아웃이나 설치된 패키지는 인터페이스/공유 세대(generation) 7을 제공해야 합니다.
새 출력은 따로 저장하세요. `records/` 아래의 기록은 한 번 쓰고 나면 수정하지 않습니다.

```bash
# Build native Core + v1 Program benchmarks (Release is required for
# representative timings; the default CMake build type is not optimized).
cmake -B build-program-bench -DCMAKE_BUILD_TYPE=Release \
    -DNEOGRAPH_BUILD_BENCHMARKS=ON \
    -DNEOGRAPH_BUILD_PROGRAM=ON \
    -DNEOGRAPH_BUILD_ASYNC=ON
cmake --build build-program-bench --target \
    bench_neograph bench_program bench_program_dispatch \
    bench_program_serialization_poc bench_program_binary_poc -j

# Positional arguments are iterations, warmup runs, and measured samples.
# bench_neograph additionally accepts par_workers before warmup/samples.
# bench_program's optional fourth argument measures closed-batch outer-run concurrency.
# Its burst rows are throughput-equivalent time, not individual request latency.
./build-program-bench/bench_neograph 10000 5000 1 10 5
./build-program-bench/bench_neograph 10000 5000 auto 10 5
./build-program-bench/bench_program 1000 10 5
./build-program-bench/bench_program 1000 10 5 8
./build-program-bench/bench_program_dispatch 100000 10 5
./build-program-bench/bench_program_serialization_poc 25 100
./build-program-bench/bench_program_binary_poc 25 100

# Build the opt-in protobuf/Cap'n Proto transport-envelope experiment.
# This target alone requires protoc/libprotobuf and capnp/libcapnp.
cmake -S . -B build-program-codec-poc -DCMAKE_BUILD_TYPE=Release \
    -DNEOGRAPH_BUILD_BENCHMARKS=ON \
    -DNEOGRAPH_BUILD_PROGRAM=ON \
    -DNEOGRAPH_BUILD_PROGRAM_CODEC_POC=ON
cmake --build build-program-codec-poc --target bench_program_codec_poc -j
taskset -c 0 ./build-program-codec-poc/bench_program_codec_poc 25 100
```

네이티브 벤치마크는 각각 `config`, `runtime`, `header`, `result` 레코드를 출력합니다.
명시한 워밍업 이후에 측정한 샘플의 중앙값을 보고하고, 짧은 실행 한 번만으로 비교해서는 안 됩니다.
`bench_program`은 인메모리 저장소를 쓰며 provider나 네트워크 호출이 없습니다.
`bench_program_dispatch`는 변경할 수 없는 `ProgramPlan` 조회와 descriptor 순회만 측정하며, Core 실행은 측정하지 않습니다.
직렬화 POC는 오프라인 인메모리 측정입니다. 완료된 Program 실행에서 변경할 수 없는 publication을 가져와 사용합니다.
serialization POC는 정규(canonical) 바이트를 재사용하는 비용을 측정하고, binary POC는 현재의 정규 JSON 봉투(envelope)와
중첩된 레코드마다 정규 바이트를 보존하는 길이 접두 봉투를 비교합니다.
binary 결과는 하한(lower bound)을 확인하는 실험이며, 영속화 계약을 대체하려는 것이 아닙니다.
`bench_program_codec_poc`는 별도로 켜야 하는 항목이며, 같은 중첩 정규 바이트를 두고 protobuf와 Cap'n Proto의
**전송 봉투(transport envelope)**를 비교합니다.
`*_envelope_only_*`는 정규 바이트가 이미 준비된 상태에서 봉투를 만드는 비용만 측정합니다.
`*_transport_total_lower_bound_*`는 중첩 정규 바이트를 만드는 비용까지 포함하지만,
`ProgramTransitionPublication`의 바깥쪽 레코드 간 검증은 일부러 건너뜁니다.
영속화나 식별자 형식을 평가하는 벤치마크가 아닙니다.
복구(recovery) 지표는 파싱해서 소유권을 가진 Program 레코드로 되돌리는 시간까지 포함하며,
수신 측 소비자 전체를 모델링하는 유일한 코덱 수치입니다.
이 POC들은 SQLite/Postgres 트랜잭션 지연이나 `ProgramRuntime`의 엔드투엔드 지연을 측정하지 않습니다.

프레임워크 비교에는 프레임워크와 출처마다 가상 환경이 하나씩 필요하며, 한 프레임워크를 위해 설치한 것이 다른 프레임워크의
비용을 바꾸지 않도록 하기 위해서입니다. 버전은 [`requirements-compare.txt`](requirements-compare.txt)(최신 릴리스)와
[`requirements-compare-head.txt`](requirements-compare-head.txt)(git 커밋)에 고정되어 있습니다. `compare/driver.py`는 전체 세션을
교차 라운드로, 셀마다 새 프로세스로 실행하고 기록을 씁니다. 오류 셀이 하나라도 있으면 종료 상태가 0이 아닙니다.

```bash
# One virtual environment per framework and per source, from the pinned requirement files
# (creates $WORK/venvs/<framework>-release and $WORK/venvs/<framework>-head):
benchmarks/compare/setup_venvs.sh "$WORK"

# NeoGraph: the default hardened Release build and the unhardened engine ceiling.
cmake -S . -B "$WORK/build-hardened" -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEOGRAPH_BUILD_BENCHMARKS=ON \
    -DNEOGRAPH_BUILD_TESTS=OFF -DNEOGRAPH_BUILD_EXAMPLES=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "$WORK/build-hardened" --target bench_neograph bench_footprint
cmake -S . -B "$WORK/build-unhardened" -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEOGRAPH_ENABLE_HARDENING=OFF \
    -DNEOGRAPH_BUILD_BENCHMARKS=ON -DNEOGRAPH_BUILD_TESTS=OFF -DNEOGRAPH_BUILD_EXAMPLES=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    "-DCMAKE_CXX_FLAGS=-U_FORTIFY_SOURCE -fno-stack-protector -fno-stack-clash-protection" \
    "-DCMAKE_C_FLAGS=-U_FORTIFY_SOURCE -fno-stack-protector -fno-stack-clash-protection"
cmake --build "$WORK/build-unhardened" --target bench_neograph bench_footprint

# One cell by hand; the last line of standard output is the record:
"$WORK/venvs/langgraph-head/bin/python" benchmarks/compare/cell.py --framework langgraph \
    --variant ainvoke_async --workload seq --iters 600 --samples 3
"$WORK/build-hardened/bench_neograph" 80000 36000 1 10 5 --rows=seq --record

# The whole session on a quiet machine (it waits for a load average under 0.3), then the tables and the chart:
python3 benchmarks/compare/driver.py run --work "$WORK" --out record.json \
    --harness-commit "$(git rev-parse HEAD)" --engine-commit "$(git rev-parse HEAD)" \
    --harness-dirty "$([ -n "$(git status --porcelain)" ] && echo true || echo false)"
python3 scripts/render_bench_report.py --record record.json
```

서비스를 쓰는 체크포인트, HTTP, 동시성 Docker 벤치마크는 별도의 실험입니다.
위에서 설명한 결정적(deterministic) 네이티브 Core/Program 실행에는 필요하지 않습니다.

출력 형식은 탭으로 구분한 `config`, `runtime`, `header`, `result`, `metric` 레코드입니다. 네이티브 결과 행에는 중앙값 총 시간과 반복당 시간이
들어 있습니다. `--record`를 주면 `bench_neograph`와 `bench_footprint`는 `record<TAB>{json}` 한 줄도 출력하고,
`benchmarks/compare/cell.py`의 모든 셀은 이런 줄을 마지막 줄로 출력합니다. 여기에는 원본 샘플, 완료 수와 실패 수, 결과 검증, 프로세스
메모리가 들어 있습니다. 정답이 틀렸거나, 실행이 실패했거나, 완료한 것이 없는 셀은 `status`가 `error`이고 시간이 없으며 종료 상태가 0이 아닙니다.

## 이전 결과

2026-04(x86_64)의 표와 2026-07의 Arm64 기준 측정은 버전과 날짜, 그리고 현재 무효로 알려진 항목에 대한 설명과 함께
[이력](history/engine-overhead-2026-04.md)으로 옮겼습니다. 2026-04-19 측정 환경 블록도 거기에 있습니다. 아래의 typed provider 섹션은
NeoGraph만 측정한 것이며 그대로입니다.

## CI 성능 게이트

`CI extended` 워크플로의 두 작업이 벤치마크 결과를 검사합니다. 이 워크플로는 매일 밤, 수동 실행 시, 그리고 게이트 파일이 바뀔 때 실행됩니다.

* **`bench-ab`**는 테스트 대상 커밋과 기준(base) 커밋을 같은 러너에서 빌드하고, [`ci_ab_gates.json`](ci_ab_gates.json)에 적힌 벤치마크를 양쪽에서 실행해 케이스별로 중앙값을 비교합니다. 호스티드 러너 사이의 차이는 잡아낼 가치가 있는 회귀보다 크므로, 한 머신에서 두 빌드를 나란히 측정해야만 수십 퍼센트의 속도 저하를 볼 수 있습니다.
* **`bench-regression`**(`scripts/check_ci_benchmarks.py`)은 HTTP, 팬아웃, 코어 벤치마크를 한 번씩 실행해 절대 한도와 비교합니다. 호스티드 러너에서 이 한도는 여유가 크기 때문에 최후의 보루로만 남아 있습니다.

`bench-ab`가 실행하는 것:

| 워크로드 | 벤치마크 | 케이스 |
|---|---|---|
| 코어 실행 경로 | `bench_neograph` | 순차 체인, 같은 체인을 sync 노드로 구성한 경우, 호출자의 executor와 엔진의 워커 풀에서 실행하는 5갈래 팬아웃, 아무것도 받지 않는 스트림 콜백과 모든 이벤트를 받는 스트림 콜백을 붙인 체인 |
| 비동기 팬아웃 | `bench_async_fanout` | 타이머 코루틴 50,000개의 처리량과 최대 메모리 |
| HTTP | `bench_async_http` | 루프백 인터페이스의 프로세스 내 서버로 보내는 풀링된 호출의 처리량 |
| Program | `bench_program_dispatch`, `bench_program` | 플랜의 디스패치 디스크립터 순회, 인메모리 스토어에서 admission을 마친 Program의 `start()`와 전체 실행 |
| 체크포인트 스토어 | `bench_checkpoint_store` | 인메모리 스토어와 SQLite(프로세스 전용 임시 디렉터리)에 대한 초당 저장 횟수 |
| Canonical JSON과 identity | `bench_canonical_json`, `bench_sha256` | ASCII, 혼합, 이스케이프 64 KiB 문자열의 canonical 바이트, 64 KiB 레코드의 SHA-256 identity |
| 프로바이더 outcome | `bench_provider_outcome_codec` | 채팅 outcome과 도구 호출 턴의 인코딩과 디코딩 |

어느 것도 루프백 인터페이스를 넘어서는 네트워크나 키, 데이터베이스 서비스를 필요로 하지 않습니다.

### 비교가 진행되는 방식

1. **기준.** 브랜치나 풀 리퀘스트는 master와의 merge base와, master는 그 이전의 가장 최근 릴리스 태그와 비교합니다. 수동 실행에서는 다른 커밋이나 태그, 또는 빌드를 자기 자신과 비교하는 `head`(A/A 실행)를 지정할 수 있습니다. 기준은 자체 소스 트리에서 같은 옵션으로 빌드합니다.
2. **샘플.** 각 워크로드는 항목에 적힌 횟수만큼, 샘플마다 새 프로세스로 실행합니다. 샘플 인덱스에 따라 head와 base가 번갈아 먼저 실행되므로, 실행 중 머신 속도가 서서히 바뀌어도 양쪽에 똑같이 영향을 줍니다.
3. **판정.** 각 케이스는 head 샘플의 중앙값과 base 샘플의 중앙값을 비교합니다. 비율이 `tolerance`보다 크거나(낮을수록 좋은 경우) 작거나(높을수록 좋은 경우), head 중앙값이 `floor`의 절대 한도를 넘으면 실패입니다.
4. **재확인.** 실패한 케이스가 있는 워크로드는 새 샘플로 한 번 더 실행하고, 재실행 결과만 판정에 반영합니다. 실패했다가 통과한 케이스는 경고로 보고합니다.
5. **base 없음.** base 빌드에 해당 벤치마크나 출력이 없는 워크로드는 base 없음으로 보고하며 실패로 처리하지 않으므로, 새 벤치마크를 추가할 수 있습니다. 절대 한도는 그대로 적용됩니다.
6. **리포트.** 작업 로그와 스텝 요약에 비율 표가 남고, 작업은 모든 샘플, 비율, 머신, 각 바이너리의 SHA-256을 담은 JSON 파일 `bench-ab-report`를 업로드합니다.

### 허용 오차

[`ci_ab_gates.json`](ci_ab_gates.json)에는 게이트가 의존하는 모든 것이 들어 있습니다. 워크로드마다 벤치마크 타깃, 인자, 샘플 수가 있고, 케이스마다 출력에서 읽을 값, 방향, 허용 오차(tolerance), 선택 사항인 절대 한도, 그리고 허용 오차를 보정할 때 기준으로 삼은 노이즈(`calibrated_on`이 가리키는 실행에서 측정한 `a_a_noise`)가 있습니다. `a_a_noise`는 A/A 실행에서 중앙값 비율이 1에서 가장 멀리 벗어난 거리입니다. 러너는 허용 오차가 그 거리의 `minimum_noise_margin`(1.5)배보다 작은 파일, 그리고 원래 게이트의 절대 한도가 하나라도 빠진 파일을 거부합니다.

노이즈는 머신에 따라 다르므로, 게이트로 쓰는 러너에서 보정하세요. `bench_ab_base`를 `head`로 지정해 `CI extended`를 실행하면, 재실행 없이 빌드를 자기 자신과 다섯 번 비교하고 리포트에 각 비율을 나열합니다. 허용 오차는 1에 0.15와 노이즈의 2배 중 큰 값을 더한 값입니다(높을수록 좋은 경우에는 그 값을 뺀 값). 의도적으로 케이스를 허용 오차 밖으로 움직이는 변경은, 같은 풀 리퀘스트에서 허용 오차와 보정 값을 함께 고쳐 기록합니다.

### 로컬에서 실행하기

```bash
python3 scripts/verify_ci.py bench-ab --work-dir /tmp/bench-ab --jobs 4 --base v0.15.1
python3 scripts/verify_ci.py bench-ab --work-dir /tmp/bench-ab-aa --jobs 4 --base head --calibrate 5
# Two build directories that already exist:
python3 scripts/run_ci_ab_gates.py --head-dir build --base-dir build-base --output report.json
```

`verify_ci.py bench-ab`는 파일에 적힌 벤치마크 타깃만 빌드합니다. 머신이 컴파일도 하는 동안에는 측정값이 흔들리므로, 절대 시간 대신 비율을 보거나 조용한 머신을 쓰세요.

## Typed provider 전환: 실제 GraphEngine의 전후 비교

아래의 전환(cutover), 최종 검증, handoff 측정은 SDK 인터페이스·공유 세대 3으로 기록했습니다.
이 기록 안의 “current”와 “final”은 각 코호트가 만들어진 시점을 가리키며, 현재 릴리스를 뜻하지 않습니다.

이 절의 내용은 **로컬 TLS HTTP/SSE** 측정으로, 위의 체크포인트 없는 Python 프레임워크 벤치마크와는 별개이며
모델 추론도 아닙니다. 운영(production)용 `GraphEngine.llm_call/tool_dispatch`를 거쳐
정적 Release GCC 13.3 Linux x64 빌드에서 **16개 구성 × 새 프로세스 3회 = 48개 기록**을 측정했습니다.
공통 H1 워크로드 3개, 다섯 계열 전체의 버퍼링/SSE 네이티브 이어가기(continuation), H2 사례 3개를 포함합니다.
유료 API는 호출하지 않았습니다.

[원본 9개 기록](provider-cutover-legacy-results.json)은 수정하지 않은 `7b47ad43`에서 얻었습니다.
[현재 원시 기록](provider-cutover-current-results.json)과 [스칼라 비교](provider-cutover-summary.json)에는
실제 적용된 제어값, 분포, RSS와 스레드 수, 실제 peer 카운터, 소유 결과(owned outcome)가 남아 있습니다.

| 공통 그래프 워크로드 | 전 p50 ms | 후 p50 ms | 전 그래프 실행/s | 후 그래프 실행/s | 전 최대 RSS MiB | 후 최대 RSS MiB |
|---|---:|---:|---:|---:|---:|---:|
| H1 버퍼링 텍스트 | 0.707 | 1.320 | 1,375.58 | 733.65 | 12.617 | 15.465 |
| H1 버퍼링 도구 루프 | 29.337 | 30.510 | 800.30 | 702.64 | 17.832 | 20.219 |
| H1 SSE 도구 루프 | 600.540 | 37.593 | 51.43 | 615.38 | 14.414 | 20.367 |

값은 3회 반복의 중앙값이며, HTTP 요청이 아니라 **그래프 실행 한 번당** 값입니다.
텍스트는 동시성 1에 peer 지연이 없고, 도구 루프는 동시성 32에 요청당 5ms 지연을 두며 그래프 실행 한 번에
생성 요청이 두 번 들어갑니다. 모든 코호트는 10회 워밍업 후 100회를 측정하고,
도구/네이티브 코호트는 반복마다 취소도 8회 요청합니다.

현재 코호트 전체에서 측정 중 실패한 실행은 없었습니다.

절충점(tradeoff)이 있습니다. 소유하는 원시/네이티브/nullable 결과 전체와 authority 게이트가 추가되어 텍스트 지연과
메모리가 늘고, 버퍼링 도구 루프의 p50도 약간 늘어납니다. 반면 SSE는 더 이상 예전 버퍼링 경로의 지연을 겪지 않습니다.
예전 구현은 네이티브/nullable authority와 일부 worker 설정을 지원하지 않았으므로, 이 수치는 같은 워크로드에서 측정한
값일 뿐 의미나 자원 사용이 동등하다는 뜻도, 모델이 빨라진다는 약속도 아닙니다.
현재 p95/p99, 첫 의미 이벤트, 취소 정착(settle) 시간, 확장 계열 행은 JSON 요약에 있습니다.

```sh
cmake -S . -B build-provider-bench -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -DNEOGRAPH_ENABLE_NATIVE_OPTIMIZATION=OFF \
  -DNEOGRAPH_BUILD_TESTS=OFF -DNEOGRAPH_BUILD_EXAMPLES=OFF \
  -DNEOGRAPH_BUILD_PROGRAM=OFF -DNEOGRAPH_BUILD_BENCHMARKS=ON \
  -DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR="$SCHEMAPROVIDER_SOURCE_DIR" \
  -DSP_BUILD_TESTS=OFF -DSP_BUILD_BENCHMARKS=OFF
cmake --build build-provider-bench --target neograph_provider_cutover_benchmark
for config in benchmarks/provider_cutover_h1_*.json benchmarks/provider_cutover_extended_*.json; do
  build-provider-bench/neograph_provider_cutover_benchmark --config "$config" || exit "$?"
done
```

NeoGraph 루트에서 Node.js, OpenSSL CLI, SDK의 일반 의존성이 있는 환경에서 실행합니다.
peer는 임시 사설 CA를 쓰며, 시스템 신뢰 저장소나 의존성 라이브러리는 바꾸지 않습니다.

## 최종 검증 코호트: 새 typed provider GraphEngine 측정

같은 GraphEngine과 로컬 TLS HTTP/SSE 구성으로 나중에 다시 실행한 Release 코호트는
**16개 구성 × 새 프로세스 3회 반복 = 48개 기록을 실패 없이 38.29초에 마쳤습니다.**
유료 모델은 호출하지 않았습니다. 이 코호트는 위의 과거 표를 대체하지 않습니다.
[최종 스칼라 요약](provider-cutover-final-summary.json)과 [최종 원시 소유 합성 객체](provider-cutover-final-results.json)는
[레거시 9개 기록 코호트](provider-cutover-legacy-results.json),
[이전의 현재 48개 기록 코호트](provider-cutover-current-results.json), [이전 비교](provider-cutover-summary.json)와
별개의 파일입니다. 약 60MB인 원시 파일에는 실제 합성 결과 객체가 들어 있으며 provider 비밀 값은 없습니다.

아래 값은 요약에 실린, 독립 프로세스 3회 반복의 중앙값이며 모델 토큰이나 HTTP 요청이 아니라
**그래프 실행 한 번당** 값입니다. 자원·제어 방식과 네이티브/nullable authority의 차이는 그대로 남아 있습니다.
즉 **의미와 자원 사용이 동등하다는 주장은 성립하지 않습니다.**
이 값은 로컬에서 측정한 절충점일 뿐 모델이 빨라진다는 뜻은 아닙니다.

### 최종 16개 구성 측정값

| 계열 | HTTP | 워크로드 | p50 ms | p95 ms | p99 ms | 그래프 실행/s | 최대 RSS MiB | 최대 스레드 |
|---|---|---|---:|---:|---:|---:|---:|---:|
| `openai.chat` | H1 | 텍스트 버퍼링 | 1.296943 | 1.349401 | 1.397812 | 769.975584 | 15.898438 | 3 |
| `openai.chat` | H1 | 도구 버퍼링 | 30.672507 | 100.492667 | 112.663949 | 678.507993 | 20.585938 | 19 |
| `openai.chat` | H1 | 도구 SSE | 35.800113 | 105.431034 | 117.559931 | 627.047345 | 21.042969 | 19 |
| `openai.chat` | H1 | 네이티브 버퍼링 | 32.546693 | 102.680190 | 115.244483 | 683.637830 | 20.925781 | 19 |
| `openai.chat` | H1 | 네이티브 SSE | 38.135188 | 115.463732 | 126.668501 | 615.357297 | 21.902344 | 19 |
| `anthropic.messages` | H1 | 네이티브 버퍼링 | 30.612805 | 103.979300 | 123.110448 | 678.785191 | 20.324219 | 19 |
| `anthropic.messages` | H1 | 네이티브 SSE | 44.021986 | 71.548005 | 93.941146 | 604.744184 | 22.125000 | 19 |
| `openai.responses` | H1 | 네이티브 버퍼링 | 35.070430 | 100.615954 | 114.597387 | 647.751880 | 22.179688 | 19 |
| `openai.responses` | H1 | 네이티브 SSE | 46.670934 | 77.636952 | 89.664406 | 571.458359 | 25.511719 | 19 |
| `google.generate` | H1 | 네이티브 버퍼링 | 31.449930 | 99.319459 | 112.385528 | 698.783682 | 21.726562 | 19 |
| `google.generate` | H1 | 네이티브 SSE | 37.774704 | 105.703383 | 120.826886 | 627.040287 | 22.500000 | 19 |
| `google.interactions` | H1 | 네이티브 버퍼링 | 32.588486 | 101.365734 | 109.841457 | 690.967042 | 21.664062 | 19 |
| `google.interactions` | H1 | 네이티브 SSE | 46.786625 | 76.036675 | 92.590316 | 573.469300 | 23.058594 | 19 |
| `openai.chat` | H2 | 텍스트 버퍼링 | 1.311873 | 2.443117 | 2.604176 | 701.785481 | 16.148438 | 3 |
| `openai.chat` | H2 | 도구 SSE | 40.349311 | 54.813608 | 59.912728 | 714.064349 | 20.386719 | 19 |
| `google.interactions` | H2 | 네이티브 SSE | 44.561127 | 56.692743 | 64.665852 | 663.966743 | 22.750000 | 19 |

### 공통 H1 워크로드 3개: 수정하지 않은 레거시와 최종 코호트 비교

| H1 그래프 워크로드 | 전 p50 ms | 최종 p50 ms | 전 그래프 실행/s | 최종 그래프 실행/s |
|---|---:|---:|---:|---:|
| 텍스트 버퍼링 | 0.706631 | 1.296943 | 1375.578826 | 769.975584 |
| 도구 버퍼링 | 29.336983 | 30.672507 | 800.301406 | 678.507993 |
| 도구 SSE | 600.539987 | 35.800113 | 51.429767 | 627.047345 |

전(Before) 값은 원래의 7b47ad43 코호트 그대로입니다. 텍스트는 지연 시간이 늘고 처리량이 줄었으며,
버퍼링 도구 루프의 변화는 크지 않고, SSE는 예전 버퍼링 경로의 지연을 없앴습니다.
확장 계열/프로토콜, 첫 의미 이벤트, 취소, 네이티브 재생(replay), 보관된 결과는 최종 요약과 원시 기록에 있습니다.

## 이벤트 기반 Provider handoff: 동일 조건의 폴링과 병합(coalesced) 채널 비교

[측정 요약](provider-notification-summary.json): 코호트당 8개 구성 × 새 프로세스 3회 = 48개 기록,
측정한 그래프 실행 4800회, 실패 0건.
GCC13.3 Release 정적/하드닝 빌드에 네이티브 최적화는 끈 상태이고, 같은 로컬 TLS 오라클과 승인(admission)된 제어값을 썼으며,
프로세스마다 10회 워밍업 후 100회를 측정했습니다. 동시에 실행한 컴파일 작업이나 모델 추론은 없었습니다.
값은 프로세스별 통계의 중앙값이며 신뢰구간이나 모델 토큰 속도가 아닙니다.
아래 payload 크기는 픽스처 텍스트의 패딩이고, 0이어도 응답 봉투는 253B입니다.

| H1 워크로드 | 전 p50 ms | 후 p50 ms | 전 그래프 실행/s | 후 그래프 실행/s |
|---|---:|---:|---:|---:|
| text256, 동시성 1 | 1.302474 | 0.952992 | 738.901074 | 1022.958838 |
| text0, 동시성 1 | 1.286646 | 0.912571 | 761.940993 | 1070.657541 |
| text4KiB, 동시성 1 | 1.387636 | 1.128831 | 709.024147 | 870.631703 |
| text64KiB, 동시성 1 | 4.873767 | 4.144619 | 196.243991 | 240.268946 |
| text256, peer 지연 5ms | 6.601920 | 6.425313 | 146.906115 | 155.052016 |
| text256, 동시성 32 | 7.641085 | 6.939675 | 1645.559595 | 1803.087419 |
| 도구 버퍼링, 동시성 32 | 30.548201 | 31.348443 | 710.487696 | 692.440963 |
| 도구 SSE, 동시성 32 | 33.597342 | 38.607118 | 655.586834 | 611.740359 |

작은 텍스트의 p50은 26.83% 줄고 처리량은 38.44% 늘었으며, 도구/SSE의 처리량은 각각 2.54%/6.69% 줄었습니다.
‘후’ 쪽은 폴링 대신 용량 1짜리 동시성 채널로 provider 알림을 병합(coalescing)합니다.
계측을 넣은 180회 연산 실행을 따로 측정한 결과, SDK publication에서 drain까지의 p50은 전 383.7125µs에서 후 36.4165µs로 줄었고,
타이머 대기는 298회에서 0회가 되었으며 알림 대기는 180회였습니다.
이 프로브 수치는 운영 지연 시간 표에 포함하지 않았습니다.

자원 최고치는 1ms 간격 샘플링 값입니다. 이 측정은 Linux/POSIX에서만 수행했습니다.
