<!-- neograph-i18n: source=examples/README.md locale=ko source_sha256=5a44b977003310ca797fa9891075e6f4c80b2e92a4a6e5bdd9707239c6d8e423 -->
# C++ API 예제

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

각 예제는 파일 하나로 한 가지를 보여 주며, 위에서 아래로 읽는 데 몇 분이면 충분합니다. 대부분 오프라인으로 실행합니다. 스크립트 모델이 provider 역할을 하므로 API 키도 네트워크도 필요하지 않습니다. 실제 모델도 호출할 수 있는 예제는 `--live`를 받습니다. 하나를 프로젝트에 복사하고 `neograph::core`(모델 기반 예제는 `neograph::llm`도)를 링크하면 동작하는 코드에서 시작할 수 있습니다.

**Offline** 열은 각 행이 필요로 하는 것을 알려 줍니다. `yes`: 키와 네트워크 없이 실행되며 자동 스모크 테스트(`ctest -L examples-offline`)가 있습니다. `key`: provider API 키가 필요합니다. `service`: 다른 프로세스, 서버, 데이터베이스, 디스플레이 또는 모델이 필요합니다. `paired`: 행에 적힌 예제와 함께 오프라인으로 실행됩니다.

## 빌드와 실행

```bash
cmake -S . -B build -DNEOGRAPH_BUILD_EXAMPLES=ON -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_TESTS=ON
cmake --build build -j$(nproc)
./build/example_react_agent            # scripted model: no key, no network
./build/example_react_agent --live     # real model: OPENROUTER_API_KEY must be set in the environment
ctest --test-dir build -L examples-offline --output-on-failure   # run every offline example and check its output
```

바이너리는 빌드 디렉터리에 `example_<name>`으로 만들어지며, 대부분의 소스 파일은 맨 위 `Usage:` 줄에 정확한 명령을 적어 둡니다. Program 기반 예제(63~69)는 `-DNEOGRAPH_BUILD_PROGRAM=ON`이 필요합니다. 그중 JavaScript 예제(65~69)는 `-DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON`도 필요하고, 69번은 SQLite도 필요합니다. 예제 76은 `-DNEOGRAPH_BUILD_MCP_HTTP_SERVER=ON`이 필요합니다. gRPC 예제(45, 53, 54, 55, 57)는 `-DNEOGRAPH_BUILD_GRPC=ON`과 `grpc++`/`protoc`이 필요합니다. 예제 11은 `-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON`이 필요합니다. 구성 요소를 꺼 둔 예제는 빌드에서 빠질 뿐 빌드를 실패시키지 않습니다.

## 여기서 시작하세요

순서대로 읽으세요. 예제마다 개념이 하나씩 더해집니다. 모두 오프라인으로 실행됩니다.

| 단계 | 예제 | Offline | 배우는 것 |
|---|---|---|---|
| 1 | [`62_core_quickstart.cpp`](62_core_quickstart.cpp) | yes | 설치된 `neograph::core` 타깃: 노드 하나, 엄격한 그래프 하나, 타입이 있는 채널 하나. `HELLO`를 출력한다. |
| 2 | [`51_minimal.cpp`](51_minimal.cpp) | yes | 채널, 그래프가 다루는 상태: 노드 하나가 채널 하나를 읽고 두 채널에 쓰며, 호출한 쪽이 타입이 있는 `ChannelKey`로 둘을 다시 읽는다. |
| 3 | [`105_conditional_edges.cpp`](105_conditional_edges.cpp) | yes | 조건부 엣지: `route_channel`이 레이블로 다음 노드를 고르고, `default` 경로가 맵에 없는 모든 레이블을 받는다. |
| 4 | [`05_parallel_fanout.cpp`](05_parallel_fanout.cpp) | yes | fan-out과 fan-in: 노드 세 개가 같은 슈퍼스텝에서 실행되고, `barrier`가 이들을 합쳐 뒤따르는 노드로 넘긴다. |
| 5 | [`01_react_agent.cpp`](01_react_agent.cpp) | yes | 모델 하나, 도구 하나, 루프 하나: `llm::Agent`가 스크립트 모델과 실제 계산기 도구로 ReAct 에이전트를 실행한다. `--live`는 실제 모델을 호출한다. |
| 6 | [`102_node_interrupt.cpp`](102_node_interrupt.cpp) | yes | Human-in-the-Loop: 노드가 실행을 멈추고, 체크포인트가 그 상태를 보관하며, `resume`이 사람의 답을 노드에 돌려준다. |
| 7 | [`65_program_javascript.cpp`](65_program_javascript.cpp) | yes | Program: JavaScript `define()`과 `main()`이 C++ 호스트에서 Core 그래프를 구동한다(`-DNEOGRAPH_BUILD_PROGRAM=ON`과 `-DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON` 필요). |

7단계를 마친 뒤에는 다음 표에서 목표별로 고르거나 영역별 색인을 둘러보세요.

## 하고 싶은 일로 찾기

목표를 고르고 그에 맞는 예제를 읽으세요. 표는 기초에서 프로토콜 순서로 이어집니다.

| 하고 싶은 일 | 읽을 예제 | Offline |
|---|---|---|
| 첫 그래프를 실행하기 | [`62_core_quickstart.cpp`](62_core_quickstart.cpp) | yes |
| 이름 있는 채널에 상태를 두고 타입으로 다시 읽기 | [`51_minimal.cpp`](51_minimal.cpp) | yes |
| 코루틴 없이 짧은 노드 작성하기 | [`61_sync_node.cpp`](61_sync_node.cpp) | yes |
| 레이블로 요청을 라우팅하고 대체 경로 두기 | [`105_conditional_edges.cpp`](105_conditional_edges.cpp) | yes |
| 요청을 알맞은 전문가에게 보내기 | [`07_intent_routing.cpp`](07_intent_routing.cpp) | yes |
| 단계를 병렬로 실행하고 결과를 합치기 | [`05_parallel_fanout.cpp`](05_parallel_fanout.cpp) | yes |
| 실행 중에 작업을 만들고 되돌아가기 | [`10_send_command.cpp`](10_send_command.cpp) | yes |
| 내 리듀서나 엣지 조건 등록하기 | [`42_custom_reducer_condition.cpp`](42_custom_reducer_condition.cpp) | yes |
| 채널이 쓰기를 합치는 방식 고르기(덮어쓰기 또는 추가) | [`104_channels_reducers.cpp`](104_channels_reducers.cpp) | yes |
| 그래프 안에 그래프 넣기 | [`06_subgraph.cpp`](06_subgraph.cpp) | yes |
| 모델에 도구를 주고 답할 때까지 반복하기 | [`01_react_agent.cpp`](01_react_agent.cpp) | yes |
| ReAct 그래프를 한 번의 호출로 만들기 | [`02_custom_graph.cpp`](02_custom_graph.cpp) | yes |
| 실행을 진행되는 대로 지켜보기 | [`100_stream_modes.cpp`](100_stream_modes.cpp) | yes |
| 실패하는 노드를 백오프로 재시도하기 | [`101_retry_policy.cpp`](101_retry_policy.cpp) | yes |
| 노드가 계속 진행하기 전에 사람에게 묻게 하기 | [`102_node_interrupt.cpp`](102_node_interrupt.cpp) | yes |
| 노드 앞에서 그래프를 멈췄다가 재개하기 | [`04_checkpoint_hitl.cpp`](04_checkpoint_hitl.cpp) | yes |
| 폭주하는 루프 멈추기 | [`103_max_steps.cpp`](103_max_steps.cpp) | yes |
| 저장된 상태를 읽고, 고치고, 포크하기 | [`08_state_management.cpp`](08_state_management.cpp) | yes |
| 크래시 뒤 끝난 작업을 다시 하지 않고 복구하기 | [`14_plan_executor.cpp`](14_plan_executor.cpp) | yes |
| 호출을 넘나들며 대화 이어 가기 | [`41_resume_if_exists_chat.cpp`](41_resume_if_exists_chat.cpp) | yes |
| 스레드 사이에서 메모리 공유하기 | [`43_store_personalization.cpp`](43_store_personalization.cpp) | yes |
| 긴 기록을 예산 안에 유지하기 | [`56_history_compaction.cpp`](56_history_compaction.cpp) | yes |
| 다른 스레드에서 실행 취소하기 | [`46_cancel_token.cpp`](46_cancel_token.cpp) | yes |
| 노드 재계산 건너뛰기 | [`47_node_cache.cpp`](47_node_cache.cpp) | yes |
| OpenInference 백엔드로 트레이스 보내기 | [`49_openinference.cpp`](49_openinference.cpp) | yes |
| Program을 컴파일하고 실행하기 | [`63_program_quickstart.cpp`](63_program_quickstart.cpp) | yes |
| JavaScript로 Program 작성하기 | [`65_program_javascript.cpp`](65_program_javascript.cpp) | yes |
| 실패한 Program 노드를 운영자에게 넘기기 | [`66_program_failure_routing.cpp`](66_program_failure_routing.cpp) | yes |
| Program이 쓸 수 있는 한도 정하기 | [`67_program_budgets.cpp`](67_program_budgets.cpp) | yes |
| Program 실행을 멈추고, 재개하고, 포크하기 | [`68_program_pause_resume_fork.cpp`](68_program_pause_resume_fork.cpp) | yes |
| 크래시 뒤 Program 실행 복구하기 | [`69_program_crash_recovery.cpp`](69_program_crash_recovery.cpp) | yes |
| 실행 중인 Program을 더 나은 토폴로지로 옮기기 | [`64_topology_retrieval.cpp`](64_topology_retrieval.cpp) | yes |
| 시드에서 harness를 진화시키기 | [`32_evolution.cpp`](32_evolution.cpp) | yes |
| MCP 서버로 도구 제공하기 | [`59_mcp_server.cpp`](59_mcp_server.cpp) | yes |
| 베어러 토큰을 쓰는 HTTP로 MCP 제공하기 | [`76_mcp_http_server.cpp`](76_mcp_http_server.cpp) | yes |
| ReAct 루프에서 MCP 도구 사용하기 | [`22_mcp_stdio.cpp`](22_mcp_stdio.cpp) | yes |
| 실패하는 MCP 도구 처리하기 | [`75_mcp_tool_errors.cpp`](75_mcp_tool_errors.cpp) | yes |
| MCP 도구 호출을 실행 전에 승인하기 | [`20_mcp_hitl.cpp`](20_mcp_hitl.cpp) | yes |
| A2A로 내 그래프를 다른 에이전트에 제공하기 | [`38_a2a_server.cpp`](38_a2a_server.cpp) | yes |
| 원격 A2A 에이전트 호출하기 | [`37_a2a_client.cpp`](37_a2a_client.cpp) | yes |
| 멈춘 그래프를 입력을 기다리는 A2A 태스크로 바꾸기 | [`78_a2a_input_required.cpp`](78_a2a_input_required.cpp) | yes |
| ACP로 내 그래프를 편집기에 제공하기 | [`39_acp_server.cpp`](39_acp_server.cpp) | yes |
| 위험한 단계 전에 편집기에 권한 요청하기 | [`80_acp_permission.cpp`](80_acp_permission.cpp) | yes |
| gRPC로 그래프 제공하기 | [`45_grpc_server.cpp`](45_grpc_server.cpp) | yes |
| gRPC로 다른 프로세스의 도구 호출하기 | [`57_grpc_remote_tool.cpp`](57_grpc_remote_tool.cpp) | yes |
| Reflexion 같은 추론 패턴 시험하기 | [`15_reflexion.cpp`](15_reflexion.cpp) | key |
| 로컬 모델 서버에 연결해 실행하기 | [`31_local_transformer.cpp`](31_local_transformer.cpp) | service |

## 영역별 색인

### Core 엔진

노드, 채널, 라우팅, fan-out, 스트리밍, 재시도, 인터럽트를 다룹니다. `neograph::core`만 링크하지만, 02, 06, 07은 스크립트 모델 때문에 `neograph::llm`도 링크합니다.

| # | 예제 | Offline | 보여 주는 것 |
|---|---|---|---|
| 62 | [`62_core_quickstart.cpp`](62_core_quickstart.cpp) | yes | 설치된 `neograph::core` 타깃: 노드 하나, 엄격한 그래프 하나, 타입이 있는 채널 하나. |
| 02 | [`02_custom_graph.cpp`](02_custom_graph.cpp) | yes | `create_react_graph()` 한 번으로 llm, tools, llm ReAct 그래프를 만들고, `run_stream`이 노드 이벤트를 출력한다. |
| 05 | [`05_parallel_fanout.cpp`](05_parallel_fanout.cpp) | yes | fan-out과 fan-in: researcher 노드 세 개가 타이머를 동시에 기다리고(카운터로 확인한다), barrier가 이들을 합쳐 summarizer로 넘긴다. |
| 06 | [`06_subgraph.cpp`](06_subgraph.cpp) | yes | 서브그래프: 바깥 supervisor 그래프가 안쪽 ReAct 그래프에 일을 맡긴다. |
| 07 | [`07_intent_routing.cpp`](07_intent_routing.cpp) | yes | 분류 노드와 조건부 엣지(`route_channel`)가 요청마다 math, translate, general 전문가 중 하나로 보낸다. |
| 09 | [`09_all_features.cpp`](09_all_features.cpp) | yes | 엔진 기능 다섯 가지(`NodeInterrupt`, `RetryPolicy`, `StreamMode`, `Command`, `Store`)를 각각 몇 줄로 훑어본다. 기능마다 자세히 다루는 예제를 함께 안내한다. |
| 10 | [`10_send_command.cpp`](10_send_command.cpp) | yes | `Send` fan-out과 `Command` 경로 재정의: 결과가 충분해질 때까지 planner, researcher, evaluator가 반복한다. |
| 42 | [`42_custom_reducer_condition.cpp`](42_custom_reducer_condition.cpp) | yes | C++에서 채널 리듀서와 엣지 조건을 직접 등록한다. |
| 51 | [`51_minimal.cpp`](51_minimal.cpp) | yes | 채널: 노드 하나가 채널 하나를 읽고 두 채널에 쓰며, 호출한 쪽이 타입이 있는 `ChannelKey`로 둘을 다시 읽는다(`result.channel(key)`, `try_channel`). |
| 52 | [`52_export_schema.cpp`](52_export_schema.cpp) | yes | `NodeFactory::export_schema()`가 비주얼 편집기가 팔레트를 만드는 데 쓰는 토폴로지 JSON Schema를 출력한다. |
| 61 | [`61_sync_node.cpp`](61_sync_node.cpp) | yes | `SyncGraphNode`: 코루틴 보일러플레이트 없는 짧은 노드 두 개(공백 제거, 대문자 변환). 블로킹 I/O는 여전히 `GraphNode::run()`에서 처리한다. |
| 100 | [`100_stream_modes.cpp`](100_stream_modes.cpp) | yes | `StreamMode`: 같은 실행을 VALUES, UPDATES, EVENTS, DEBUG 모드로 관찰하고, 모드마다 전달되는 이벤트를 센다. |
| 101 | [`101_retry_policy.cpp`](101_retry_policy.cpp) | yes | `RetryPolicy`: 노드가 두 번 실패하면 엔진이 지수 백오프로 재시도한다. 재시도 횟수를 줄이면 재시도가 바닥났을 때 호출한 쪽이 받는 결과를 볼 수 있다. |
| 102 | [`102_node_interrupt.cpp`](102_node_interrupt.cpp) | yes | `NodeInterrupt`: 노드가 스스로 실행을 멈추고, 페이로드가 호출한 쪽에 물어볼 내용을 알려 주며, `resume`이 답(승인 또는 거절)을 노드에 돌려준다. |
| 103 | [`103_max_steps.cpp`](103_max_steps.cpp) | yes | `max_steps`: 루프가 단계 한도에서 멈추고, 결과가 그 사실을 알려 준다(`max_steps_exhausted()`, `RunStatus::StepLimit`). 마지막 단계에서 스스로 끝난 실행에는 표시가 붙지 않는다. |
| 104 | [`104_channels_reducers.cpp`](104_channels_reducers.cpp) | yes | 채널과 리듀서: `overwrite`와 `append`의 차이, `initial` 값, 그리고 쓰기 카운터 하나를 모든 채널이 공유하는 채널 버전. |
| 105 | [`105_conditional_edges.cpp`](105_conditional_edges.cpp) | yes | 조건부 엣지: `route_channel`이 레이블로 다음 노드를 고르고, `default` 경로가 맵에 없는 모든 레이블을 받는다. |

### 에이전트 패턴

모델과 도구를 루프로 묶은 구성과, 그 위에 만든 공개 추론 패턴입니다. `key` 행은 실제 모델을 호출합니다.

| # | 예제 | Offline | 보여 주는 것 |
|---|---|---|---|
| 01 | [`01_react_agent.cpp`](01_react_agent.cpp) | yes | ReAct: Thought 한 줄, 도구 호출, Observation, 그리고 최종 답. `--live`는 실제 모델을 호출한다. |
| 36 | [`36_classifier_fanout.cpp`](36_classifier_fanout.cpp) | yes | 작은 분류기 다섯 개를 `Send`로 fan-out한다. 걸리는 시간은 합이 아니라 가장 느린 하나의 시간이다. 판정값은 미리 정해 둔 값이다. |
| 15 | [`15_reflexion.cpp`](15_reflexion.cpp) | key | Reflexion: actor와 critic이 critic이 수락할 때까지 반복한다(Shinn et al. 2023). |
| 16 | [`16_tree_of_thoughts.cpp`](16_tree_of_thoughts.cpp) | key | Tree of Thoughts: 후보 사고를 생성하고, 점수를 매기고, 남기고, 확장한다. |
| 17 | [`17_self_ask.cpp`](17_self_ask.cpp) | key | Self-Ask: 다단계 추론을 위한 명시적 후속 질문(Press et al. 2022). |
| 18 | [`18_multi_agent_debate.cpp`](18_multi_agent_debate.cpp) | key | researcher, skeptic, judge: 하나의 공유 대화 기록 위에서 시스템 프롬프트 세 개가 동작한다. |
| 19 | [`19_rewoo.cpp`](19_rewoo.cpp) | key | REWOO: planner가 자리 표시자를 쓰고, worker가 도구를 병렬로 실행하고, solver가 증거를 종합한다. |
| 12 | [`12_rag_agent.cpp`](12_rag_agent.cpp) | key | 실제 임베딩과 메모리 내 코사인 검색을 쓰는 검색 증강 생성. |
| 28 | [`28_corrective_rag.cpp`](28_corrective_rag.cpp) | key | Corrective RAG: 검색 결과를 평가한 뒤 다듬거나, 웹 검색을 더하거나, 웹 검색만 쓴다(Yan et al. 2024). |
| 25 | [`25_deep_research.cpp`](25_deep_research.cpp) | key | Deep research: supervisor가 각자 ReAct 루프를 도는 하위 researcher를 fan-out하고 보고서를 쓴다. `OPENROUTER_API_KEY`와 Crawl4AI 서버가 필요하다. |
| 40 | [`40_react_async_streaming.cpp`](40_react_async_streaming.cpp) | key | 비동기 ReAct: 바깥 코루틴이 `run_stream_async`를 기다리며 타입이 있는 텍스트 델타를 출력한다. |
| 13 | [`13_openrouter_responses_sse.cpp`](13_openrouter_responses_sse.cpp) | key | 순서가 있는 `sp::Event` 관찰값과 소유된 `sp::Outcome`을 갖는 타입 기반 Responses SSE 요청. |
| 29 | [`29_responses_envelope.cpp`](29_responses_envelope.cpp) | key | 디버그 도구: 도구 호출 요청 하나의 Responses 봉투 전체와 순서가 있는 출력 항목을 모두 덤프한다. |
| 30 | [`30_reasoning_effort.cpp`](30_reasoning_effort.cpp) | key | 프롬프트 하나에 `none`, `low`, `medium`, `high` 추론 강도를 차례로 적용하며 지연 시간과 추론 토큰을 비교한다. |
| 34 | [`34_openrouter_responses_tools_sse.cpp`](34_openrouter_responses_tools_sse.cpp) | key | SSE로 타입 기반 호스팅 도구 일곱 가지 섹션을 모두 실행하며, 전체 outcome과 순서가 있는 wire 관찰값을 남긴다. |
| 31 | [`31_local_transformer.cpp`](31_local_transformer.cpp) | service | `127.0.0.1:8090`의 로컬 OpenAI 호환 서버(llama.cpp, vLLM)를 가리키는 타입 기반 Chat 클라이언트. |

### Program API

Program은 Core 그래프를 수명 주기로 감쌉니다. 이 수명 주기에는 내구성 있는 식별자, 예산, 승인(admission)이 들어 있습니다. JavaScript Program API(join, 실패 라우팅, 예산, 일시 중지와 재개, fork, 크래시 복구)는 [`bindings/python/tests/program_e2e/`](../bindings/python/tests/program_e2e/README.md)의 Python 엔드투엔드 시나리오로 확인합니다. 예제 65~69는 C++ 호스트에서 JavaScript Program을 실행하며 Python 예제 29~36에 대응합니다. 이 예제들은 `-DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON`도 필요합니다. 레지스트리, 승인(admission), 런타임 설정은 `program_support.h`에 모아 두었으며, 예제 63은 C++ 빌더 JSON으로 쓴 Program에 이 설정을 씁니다.

| # | 예제 | Offline | 보여 주는 것 |
|---|---|---|---|
| 63 | [`63_program_quickstart.cpp`](63_program_quickstart.cpp) | yes | `call_core` 연산 하나를 가진 `ProgramSource`를 컴파일하고 승인(admission)한 뒤 `ProgramRuntime`으로 실행한다. |
| 64 | [`64_topology_retrieval.cpp`](64_topology_retrieval.cpp) | yes | 검색으로 얻은 토폴로지 후보를, 내구성 있는 안전 지점에서 실행 중인 lineage 하나의 검증된 마이그레이션으로 만든다. |
| 65 | [`65_program_javascript.cpp`](65_program_javascript.cpp) | yes | C++ 호스트가 `define()`과 `main()`이 있는 JavaScript Program을 컴파일해 실행한다. `main()`이 `ng.callCore`로 그래프를 두 번 호출해 21이 42가 되고, 다시 84가 된다. |
| 66 | [`66_program_failure_routing.cpp`](66_program_failure_routing.cpp) | yes | Python 32번의 C++ 짝이다. TEST가 작성자 코드가 붙은 `NodeFailureError`를 던지고, 두 번 재시도된 뒤 `interruptBefore`가 걸린 처리 노드로 라우팅된다. `retry`로 `resume`하면 Program이 완료되고, `abort`면 `plan.user_abort`와 함께 `failed`로 끝난다. |
| 67 | [`67_program_budgets.cpp`](67_program_budgets.cpp) | yes | C++의 예산 한도 세 가지. 승인(admission)은 호스트의 `BudgetLimits` 상한보다 큰 예산을 요구하는 Program을 거부하고(`P_ADMIT_POLICY`), 다른 `RunBudget`으로 시작한 실행은 시작할 때 거부되며(`P_START_BUDGET`), `max_core_steps`를 다 쓴 루프는 `budget_exhausted`로 끝난다. |
| 68 | [`68_program_pause_resume_fork.cpp`](68_program_pause_resume_fork.cpp) | yes | Python 34번과 35번의 C++ 짝이다. `interruptBefore`가 실행을 멈추고, `resume`이 운영자의 답으로 이어 가며, `fork`는 멈춘 실행의 체크포인트에서 같은 질문에 다른 답을 준다. 끝난 노드의 본문은 통틀어 한 번만 실행된다. |
| 69 | [`69_program_crash_recovery.cpp`](69_program_crash_recovery.cpp) | yes | SQLite 파일을 쓰는 Python 36번의 C++ 짝이다. 프로세스 A가 질문에 답하고 `charge`를 실행한 뒤 종료되며, 프로세스 B가 같은 디렉터리를 열어 `reconnect`를 호출한다. 실행은 두 번째 시도에서 완료된다. `charge`는 두 번 실행되므로 그 효과를 장부(ledger)로 보호한다. Linux와 macOS 전용이다. |
| 70 | [`70_program_plan_intake.cpp`](70_program_plan_intake.cpp) | yes | 스크립트된 계획자가 평평한 `step-plan/v1`로 답합니다. 입력 노드는 첫 답을 위반 세 가지와 함께 거부하고, 계획자는 그 위반을 받아 다시 답합니다. 두 번째 답은 작업 그래프로 변환되어 합 두 개를 구하고, 둘을 더하고, 단어 하나를 대문자로 바꿉니다. |
| 32 | [`32_evolution.cpp`](32_evolution.cpp) | yes | Harness 진화 루프: 엄격한 Core 시드를 과제에 맞춰 변이시키고 계보를 JSON으로 출력한다(`--smoke`는 내장 픽스처를 쓴다). |
| 58 | [`58_harness_bench.cpp`](58_harness_bench.cpp) | yes | 세 가지 피드백 모드로 harness 수렴을 재는 벤치마크. `--smoke`는 내장 과제를 실행한다. |
| 60 | [`60_harness_mcp_server.cpp`](60_harness_mcp_server.cpp) | yes | Harness MCP 서버: 도구 여섯 개로 범위가 제한된 다중 worker 워크플로를 컴파일하고 실행한다. `NEOGRAPH_HARNESS_SMOKE=1`은 오프라인 픽스처 provider로 바꾼다. |

### 프로토콜: MCP, A2A, ACP, gRPC

모두 오프라인으로 실행됩니다. MCP 클라이언트 예제는 표준 라이브러리만 쓰는 서버(`mcp_demo_server.py`, 설치할 것 없음)를 띄우고 스크립트 모델을 씁니다. A2A와 ACP 예제는 클라이언트와 서버를 한 프로세스에 두거나 동반 프로세스를 시작합니다. 선택 CMake 옵션이 필요한 예제는 그 행에 `-DNEOGRAPH_BUILD_GRPC=ON` 같은 옵션을 적어 두었습니다.

| # | 예제 | Offline | 보여 주는 것 |
|---|---|---|---|
| 59 | [`59_mcp_server.cpp`](59_mcp_server.cpp) | yes | stdio 기반 MCP 서버: `echo` 도구 하나, 입력·출력 스키마, 구조화된 콘텐츠. |
| 76 | [`76_mcp_http_server.cpp`](76_mcp_http_server.cpp) | yes | streamable HTTP 기반 MCP 서버: 빈 포트에서 베어러 토큰을 검사하고 NeoGraph 자체 클라이언트가 호출한다. 토큰 없는 호출은 HTTP 401을 받는다(`-DNEOGRAPH_BUILD_MCP_HTTP_SERVER=ON` 필요). |
| 22 | [`22_mcp_stdio.cpp`](22_mcp_stdio.cpp) | yes | stdio 기반 MCP: 서버는 네트워크 스택이 없는 자식 프로세스이고, ReAct 루프가 그 도구를 호출한다. 스크립트 모델과 표준 라이브러리 서버를 쓴다. |
| 03 | [`03_mcp_agent.cpp`](03_mcp_agent.cpp) | yes | streamable HTTP 기반 MCP: 서버의 도구를 찾아 ReAct 루프에 넘긴다. 데모 서버는 표준 라이브러리만 쓰는 Python이고 모델은 스크립트 모델이다. |
| 20 | [`20_mcp_hitl.cpp`](20_mcp_hitl.cpp) | yes | 사람의 승인을 받는 MCP: `interrupt_before`가 MCP 도구 호출 앞에서 그래프를 멈추고, 대기 중인 호출을 보여 준 뒤 `resume()`으로 모델을 다시 호출하지 않고 이어 간다. |
| 21 | [`21_mcp_fanout.cpp`](21_mcp_fanout.cpp) | yes | `Send`를 쓰는 MCP: planner가 도구마다 호출 하나를 만들고, 세 호출이 한 슈퍼스텝 안에서 겹쳐 실행된다. 모델은 쓰지 않는다. |
| 23 | [`23_mcp_multi.cpp`](23_mcp_multi.cpp) | yes | 에이전트 하나가 MCP 서버 두 개(HTTP와 stdio)의 도구를 하나의 목록으로 합쳐 쓰고, 두 호출은 서로 다른 서버로 간다. |
| 24 | [`24_mcp_feedback.cpp`](24_mcp_feedback.cpp) | yes | MCP 위의 운영자 피드백: 첫 답은 추측이고, 운영자의 답장이 대화 기록에 들어가며, 두 번째 라운드에서 도구를 호출한다. |
| 75 | [`75_mcp_tool_errors.cpp`](75_mcp_tool_errors.cpp) | yes | MCP 실패: `isError` 결과와 JSON-RPC 오류는 서로 다른 응답이다. 에이전트는 두 메시지를 읽고 스스로 바로잡는다. |
| 35 | [`35_re_agent.cpp`](35_re_agent.cpp) | yes | 리버스 엔지니어링 에이전트: stdio의 GhidraMCP로 스트립된 바이너리의 함수에 이름을 붙인다. 대역 브리지와 스크립트 모델로 오프라인 실행하고, `--live`는 Ghidra와 실제 모델을 쓴다. |
| 38 | [`38_a2a_server.cpp`](38_a2a_server.cpp) | yes | A2A 서버: 그래프를 SSE 스트리밍과 함께 A2A 0.3, 1.0으로 제공하고 자체 클라이언트로 호출한다. `serve`를 주면 계속 대기한다. |
| 77 | [`77_a2a_dialects.cpp`](77_a2a_dialects.cpp) | yes | A2A 방언: 에이전트 카드가 JSON-RPC 1.0과 0.3을 나열한다. 한 클라이언트는 카드에서 1.0을 고르고, 0.3만 적힌 카드를 받은 다른 클라이언트는 0.3을 고른다. |
| 78 | [`78_a2a_input_required.cpp`](78_a2a_input_required.cpp) | yes | A2A `input-required`: `interrupt_before`에서 멈춘 그래프가 입력을 기다리는 태스크가 되고, 클라이언트가 같은 태스크에 답하면 결과를 아티팩트로 받는다. |
| 79 | [`79_a2a_collaboration.cpp`](79_a2a_collaboration.cpp) | yes | A2A 협업: 두 에이전트가 링크를 맺고(제안 후 수락) 메일박스로 순번이 붙은 봉투를 주고받는다. 재전달된 봉투는 한 번만 적용된다. |
| 37 | [`37_a2a_client.cpp`](37_a2a_client.cpp) | yes | A2A 클라이언트: 에이전트 카드를 가져오고 메시지를 보내며, `A2ACallerNode`로 그래프에서 원격 에이전트를 호출한다. |
| 39 | [`39_acp_server.cpp`](39_acp_server.cpp) | yes | stdio 기반 ACP 에이전트로, 편집기가 구동하는 형태이다. `--self-test`는 initialize, session/new, session/prompt를 프로세스 안에서 실행한다. |
| 80 | [`80_acp_permission.cpp`](80_acp_permission.cpp) | yes | ACP 권한 요청: 노드가 삭제 전에 `session/request_permission`으로 편집기에 묻는다. 스크립트 편집기가 한 호출은 허용하고 다음 호출은 거부한다. |
| 81 | [`81_acp_elicitation.cpp`](81_acp_elicitation.cpp) | yes | ACP elicitation: 클라이언트가 그 capability를 선언했으면 노드가 `elicitation/create`로 사용자에게 폼 질문을 하고, 선언하지 않았으면 기본값으로 대체한다. |
| 45 | [`45_grpc_server.cpp`](45_grpc_server.cpp) | yes | gRPC: `GraphEngine`을 `GraphService`로 제공한다. `--self-test`는 서버와 클라이언트를 한 프로세스에서 빈 포트로 실행하며, 예제 53이 프로세스 간 클라이언트이다(`-DNEOGRAPH_BUILD_GRPC=ON` 필요). |
| 53 | [`53_grpc_client.cpp`](53_grpc_client.cpp) | yes | gRPC: 일반 클라이언트에서 `GraphService`(`Health`, `RunGraph`, 스트리밍)를 호출한다(`-DNEOGRAPH_BUILD_GRPC=ON` 필요). |
| 54 | [`54_grpc_checkpoint.cpp`](54_grpc_checkpoint.cpp) | yes | gRPC: 원격 `CheckpointStore`인 `GrpcCheckpointStore`와 측정한 지연 시간(`-DNEOGRAPH_BUILD_GRPC=ON` 필요). |
| 55 | [`55_grpc_vs_jsonrpc_toolcall.cpp`](55_grpc_vs_jsonrpc_toolcall.cpp) | yes | 도구 호출에서 gRPC와 JSON-RPC를 루프백으로 측정해 비교한다. 합격/불합격 검사가 아니라 측정이다(`-DNEOGRAPH_BUILD_GRPC=ON` 필요). |
| 57 | [`57_grpc_remote_tool.cpp`](57_grpc_remote_tool.cpp) | yes | gRPC: 다른 프로세스에 있는 도구를 로컬 `neograph::Tool`처럼 쓴다(`-DNEOGRAPH_BUILD_GRPC=ON` 필요). |

### 영속성과 운영

체크포인트, 상태, 메모리, 동시성, 관찰 가능성을 다룹니다.

| # | 예제 | Offline | 보여 주는 것 |
|---|---|---|---|
| 04 | [`04_checkpoint_hitl.cpp`](04_checkpoint_hitl.cpp) | yes | Human-in-the-Loop: 결제 노드 앞에서 `interrupt_before`로 멈추고, 체크포인트를 저장한 뒤 승인을 받아 재개한다. |
| 08 | [`08_state_management.cpp`](08_state_management.cpp) | yes | 저장된 체크포인트에서 쓰는 `get_state`, `update_state`, `fork`와 체크포인트 이력. |
| 14 | [`14_plan_executor.cpp`](14_plan_executor.cpp) | yes | fan-out 도중 실패를 흉내 낸 Plan and execute: 재개하면 실패한 형제 노드 하나만 다시 실행된다(대기 중인 쓰기, pending writes). |
| 26 | [`26_postgres_react_hitl/`](26_postgres_react_hitl/) | key | PostgreSQL을 쓰는 deep-research HITL로, 보고서와 재개 사이에 프로세스가 종료돼도 유지된다. 키, PostgreSQL, Crawl4AI(Docker Compose)가 필요하다. |
| 41 | [`41_resume_if_exists_chat.cpp`](41_resume_if_exists_chat.cpp) | yes | `resume_if_exists`를 쓰는 여러 턴 대화: 다음 턴이 저장된 체크포인트를 다시 불러와 거기에 이어 붙인다. |
| 43 | [`43_store_personalization.cpp`](43_store_personalization.cpp) | yes | 노드 안에서 읽고 쓰는 스레드 간 `Store`로 사용자별 응답을 개인화한다. |
| 48 | [`48_sqlite_checkpoint.cpp`](48_sqlite_checkpoint.cpp) | yes | `:memory:`의 `SqliteCheckpointStore`: 체크포인트, 재개, 스레드 격리(파일 내구성은 보여 주지 않는다). |
| 56 | [`56_history_compaction.cpp`](56_history_compaction.cpp) | yes | 대화 기록 압축: 요약이 깨끗할 때만 오래된 텍스트 턴을 요약으로 바꾸고, 네이티브 그룹은 통째로 유지한다. |
| 27 | [`27_async_concurrent_runs.cpp`](27_async_concurrent_runs.cpp) | yes | 에이전트 실행 세 개가 `engine->run_async()`로 하나의 `io_context` 스레드에서 번갈아 실행된다. |
| 44 | [`44_request_queue_backpressure.cpp`](44_request_queue_backpressure.cpp) | yes | 백프레셔가 있는 고정 worker 풀(`RequestQueue`): 처리 중인 작업 수를 제한하고 넘치는 요청은 거절한다. |
| 46 | [`46_cancel_token.cpp`](46_cancel_token.cpp) | yes | 협력적 취소: 자식마다 `CancelToken::fork()`를 쓰고, 부모가 `cancel()`하면 모두 멈춘다. |
| 47 | [`47_node_cache.cpp`](47_node_cache.cpp) | yes | 노드별 결과 캐시: 같은 입력으로 두 번째 실행하면 캐시에 적중하고, 새 입력은 적중하지 못한다. |
| 50 | [`50_async_tool.cpp`](50_async_tool.cpp) | yes | `AsyncTool`: 동기 `Tool::execute` 계약도 그대로 받아들이는 코루틴 형태의 도구. |
| 49 | [`49_openinference.cpp`](49_openinference.cpp) | yes | OpenInference 트레이싱: `graph.run > node.* > llm.complete`가 mock provider와 함께 span 트리 하나로 기록된다. |

### 쇼케이스와 쿡북

여러 기능을 조합한 더 큰 프로그램입니다. 쿡북 프로젝트마다 README가 있으며, [쿡북 색인](cookbook/README.md)을 참고하세요.

| # | 예제 | Offline | 보여 주는 것 |
|---|---|---|---|
| 11 | [`11_clay_chatbot.cpp`](11_clay_chatbot.cpp) | service | C++로 만든 데스크톱 채팅: Clay가 창을 배치하고 Raylib이 그리며, 에이전트의 응답이 스트리밍된다. 기본은 mock 응답이고 `--live`는 OpenRouter를 쓴다. `-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON`과 디스플레이가 필요하다. |

| 프로젝트 | Offline | 보여 주는 것 |
|---|---|---|
| [`cookbook/the-beast/`](cookbook/the-beast/) | yes | 엄격한 Core JSON을 쓰고, 컴파일러로 검증하고, 진화시키고, 체크포인트로 롤백하는 자기 진화 에이전트. 오프라인 바이너리는 스텁 작성자를 쓰고, live 변형은 키가 필요하다. |
| [`cookbook/ai-assembly/`](cookbook/ai-assembly/) | yes | A2A 의원 서버 네 개와, 법안을 일제히 전달하고 표를 집계하는 의장(Speaker). `scripts/run_session.sh --mock`으로 오프라인 실행하며, `NEOGRAPH_BUILD_DIR`에 빌드 디렉터리를 지정한다. |
| [`cookbook/jarvis/`](cookbook/jarvis/) | yes | 4갈래 라우터, MCP 도구, A2A 전문가, 저장되는 메모리를 갖춘 메타 오케스트레이터. mock 빌드: `-DNEOGRAPH_BUILD_COOKBOOK_JARVIS=ON -DNEOGRAPH_JARVIS_FORCE_MOCK=ON`. |
| [`cookbook/minimal-mcp/`](cookbook/minimal-mcp/) | yes | 표준 라이브러리만 쓴 60줄짜리 Python 서버를 상대로 하는 MCP 클라이언트 왕복. 모델은 없다. |
| [`cookbook/multi_tenant_chatbot/`](cookbook/multi_tenant_chatbot/) | yes | 컴파일 캐시를 거쳐 한 프로세스가 여러 고객의 토폴로지를 서비스한다. mock 바이너리는 아무것도 필요 없고, live 바이너리는 키가 필요하다. |
| [`cookbook/self_evolving_chatbot/`](cookbook/self_evolving_chatbot/) | key | 고객마다 토폴로지를 고르고 진화시키는 채팅 서버. ProgramChat에는 `--mock` 모드가 있고, Core 서버는 키가 필요하다. |
| [`cookbook/topology-retrieval/`](cookbook/topology-retrieval/) | yes | 예제 64로 이어지는 후보 선택(dense, BM25, 순위 결합, 재순위). `--mock`은 `pip install faiss-cpu numpy`가 필요하다. |
| [`cookbook/byo-openai/`](cookbook/byo-openai/)와 [`cookbook/openrouter-provider/`](cookbook/openrouter-provider/) | service | Python provider 레시피: 노드 안에서 자체 OpenAI 클라이언트를 쓰는 방법과 타입 기반 OpenRouter 요청. Chat 엔드포인트(키, 또는 `bindings/python/examples/_scripted.py`의 스크립트 모델 같은 로컬 peer)가 필요하다. |

이 표에서 `yes`는 프로젝트의 기본 실행에 키가 필요 없다는 뜻입니다. 스모크 테스트를 거치는 바이너리는 `ctest -L examples-offline`에 `cookbook.*`으로 나타납니다.

### 예제 11과 35 참고

예제 11은 Raylib을 소스에서 빌드하므로, Linux에서는 X11과 OpenGL 개발 헤더가 필요합니다(예: `libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev`). 텍스트는 fontconfig나 일반적인 설치 경로에서 찾은 시스템 TrueType/OpenType 글꼴을 씁니다. 파일은 `NEOGRAPH_CHAT_FONT`와, 필요하면 `NEOGRAPH_CHAT_BOLD_FONT`, `NEOGRAPH_CHAT_MONO_FONT`로 지정할 수 있으며, `font.ttc#N`은 글꼴 모음의 N번째 face를 고릅니다. 키 조작은 Enter가 전송, Esc가 입력 지우기, Ctrl+V가 붙여넣기, Ctrl+Q가 종료이며, 답변이 스트리밍되는 동안에는 Send 버튼이 Stop으로 바뀝니다.

예제 35는 `--bridge examples/ghidra_stand_in.py`로 GhidraMCP 브리지의 대역을 지정하면 스크립트 모델과 함께 오프라인으로 실행됩니다. `--live`로 실행하려면 `GHIDRA_MCP_BRIDGE`(또는 `--bridge`)에 `bridge_mcp_ghidra.py` 스크립트의 경로를 지정해야 합니다. `GHIDRA_MCP_PYTHON`은 인터프리터를 고르며 기본값은 `python3`입니다. `GHIDRA_SERVER_URL`은 GhidraMCP 백엔드의 엔드포인트를 고르며 기본값은 `http://127.0.0.1:18080/`입니다. 실제 실행 전에 GhidraMCP 백엔드를 켜고 프로그램을 연 채로 Ghidra를 시작해야 합니다. 예제는 모델을 호출하기 전에 도구를 찾고 `list_methods`를 한 번 호출하며, 백엔드가 응답하지 않으면 상태 2로 종료합니다. 일부 GhidraMCP 릴리스는 백엔드가 내려가 있어도 모든 도구를 나열하기 때문입니다. 실제 실행에는 그다음에 확인하는 비어 있지 않은 `OPENROUTER_API_KEY`도 필요하며, 유료 모델 호출이 발생합니다.

## 오프라인, 스크립트 모델, 라이브

모델로 구동되는 예제에는 모델이 필요합니다. 오프라인 예제는 `scripted_provider.h`를 씁니다. 응답을 예제 안에 직접 적는 `neograph::Provider`로, 텍스트, 도구 호출, 스트리밍 델타, 사용량, 타입이 있는 실패를 표현할 수 있습니다. 실제 provider와 같은 prepare와 dispatch 경로를 거치므로, 그래프·도구·체크포인트 코드는 실제로 배포할 코드 그대로입니다. 모델이 아니라서 무슨 말을 할지는 스크립트가 정합니다.

```cpp
#include "scripted_provider.h"

auto scripted = std::make_shared<examples::ScriptedProvider>(std::vector<examples::Reply>{
    examples::call_tool("calculator", {{"expression", "2 + 3"}}),   // call 1: ask for a tool
    examples::say("The answer is 5."),                              // call 2: the final answer
});
auto provider = examples::choose_provider(argc, argv, scripted);    // --live switches to OpenRouter
```

스크립트는 요청을 받는 함수일 수도 있습니다. `ScriptedProvider([](const examples::ScriptedTurn& turn) { ... })`는 기록, 마지막 사용자 텍스트, 도구 결과를 볼 수 있으며, 루프와 병렬 fan-out에는 이 형태가 필요합니다. 규칙은 `scripted_provider.h` 맨 위 주석에 있습니다.

`--live`(또는 `NG_EXAMPLE_LIVE=1`)는 `OPENROUTER_API_KEY`로 실제 provider를 만듭니다. 키는 프로세스 환경에서만 읽으며, 헬퍼는 어떤 파일도 읽지 않습니다. 라이브 호출은 provider가 과금합니다. 오래된 라이브 전용 예제는 아직 `cppdotenv`로 `.env` 파일을 불러오며, 다시 쓸 때 이 헬퍼로 옮깁니다. 예제 25와 26은 로컬 Crawl4AI 서버도 쓰며, 현재의 보안 Crawl4AI 이미지는 비어 있지 않은 `CRAWL4AI_API_TOKEN`이 필요합니다. 예제 26의 `.env.example`을 보세요.

## 예제 추가하기

번호는 재사용하지 않으므로, 현재 가장 큰 번호보다 하나 큰 번호를 쓰세요. 새 예제는 소스 파일 하나, CMake 한 줄, 표의 행 하나로 이루어집니다.

1. 파일 이름을 `NN_short_name.cpp`로 짓고 헤더 주석으로 시작합니다. 몇 줄로 보여 주는 내용, `Usage:` 줄, 그다음 테스트 주석을 적습니다.
2. 그 주석 블록에 테스트 방법을 적습니다. 오프라인 예제는 반드시 출력해야 하는 줄을 적고, 키나 서비스가 필요한 예제는 오프라인으로 실행할 수 없는 이유를 적습니다(주석 문법은 `examples/run_example_smoke.py` 맨 위에 있습니다).
3. `examples/cmake/` 아래 파일에 `neograph_add_example(short_name NN_short_name.cpp LINK neograph::core neograph::llm)`으로 타깃을 등록합니다. ctest 항목은 주석에서 만들어집니다.
4. 그 예제가 속한 영역의 표에 Offline 값과 함께 행을 추가합니다.
5. `ctest -L examples-offline`과 `python scripts/check_examples.py`를 실행합니다. 두 번째는 주석이 없는 예제, 중복된 번호, 주석과 어긋나는 표가 있으면 실패합니다.

```cpp
// smoke-expect: Final answer: 427
// smoke-skip: needs OPENROUTER_API_KEY (live model call)
```

위의 두 주석은 둘 중 하나입니다. 예제에는 `smoke-expect` 줄 또는 `smoke-skip` 줄 하나가 있어야 하며, 둘 다 쓰지는 않습니다.

## 타입 C++ 전환 상태

현재 C++ 레시피는 다섯 타입 SDK family의 소유된 `ProviderRequest`, 순서 있는
`sp::Event`, 불변 `std::shared_ptr<const sp::Outcome>` (`Completion`/`Failure`)을
사용합니다. `ChatMessage`/`ChatTool`은 portable 투영이며 native replay 권한이 아닙니다.
prepare는 정확히 한 번 수행합니다. durable 호출자는
`Provider::request_digest(prepared)`에 claim/receipt를 연결하고 같은 handle을 dispatch합니다.
출력 JSON으로 history를 재구성하거나 failure를 최종 텍스트로 축소하지 않습니다.

canonical persistence는 `provider-message-v2`/`runtime-history-record-v2`를 사용하며
portable 요약은 native record를 대체하지 않습니다. optional control은 호출자가 선택하고
조용히 clamp하지 않습니다. bounded call에는 진짜 model fact가 필요하며 누락은 `LimitUnknown`입니다.
reservation/charged/held 값은 nullable provider usage와 별개이며 budget 갱신, 가격, forecast, invoice가 아닙니다.

header-only `examples/provider_example_support.h`는 실제 SDK runtime을 사용합니다.
`NEOGRAPH_BUILD_LLM=OFF`를 포함한 모든 Core 빌드에 `SchemaProvider::runtime`이 필요합니다.
CMake 3.20+는 명시적 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`, 설치된 runtime package,
고정된 public GitHub source archive 순서로 SDK를 선택합니다. 다운로드 fallback은 기본 활성화이며,
설치 package나 명시 source를 쓰는 offline 빌드에서는 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`로 끄세요.
설치 include root는 `include/SchemaProvider`입니다. interface/capability 검사를
수행하며 SDK package는 alpha `0.3.1` (interface revision/shared ABI 6)입니다.

보존된 interface-3 model-free C++ E2E 실행에서는 번호가 있는 타깃 39개를 검증했습니다. finite offline 29개와
실제 MCP/ACP/A2A/Harness 및 gRPC graph/checkpoint/tool 경로입니다. gRPC-vs-JSON-RPC
측정 예제도 실행했지만 반환값을 검사하지 않아 행동 E2E 통과로 세지 않습니다.
live/외부 모델 경로 22개와 비활성 Clay GUI는 미검증이며 공개 vendor 요청이나 새 grant는 없었습니다.
아래 과거 측정은 새 전환의 qualification이 아닙니다. live에는 키/네트워크/모델 접근과 비용이 필요합니다.
키, prompt, artifact는 비공개로 유지하고 민감한 envelope/native 출력을 공개 log에 내보내지 마세요.
native archive는 owner-private 인증 custody이며 암호화나 vendor issuer 인증이 아닙니다.

이전 interface-4 증거는 별도입니다. Linux x86_64에서 native research 복구,
ToT/Forge/rewrite helper와 A2A 0.3/1.0 peer를 실행했고, 설치된 Python application 11개가
분리된 실행 묶음에서 credential-free localhost 요청 48개를 수행했습니다.
추적된 evolution file 모드와 Plan resume도 아래와 같이 실행했습니다.
보존된 전체 C++ suite를 재실행한 결과가 아닌 범위가 한정된 local 실행입니다.
[SDK interface-4 실행 기록](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/CONFORMANCE.md#interface-4-execution-record)을 참조하세요.
새 hosted vendor, Windows/macOS/ARM64, HTTP/3 또는 sanitizer 검증은 주장하지 않으며,
remote CI와 공개 배포는 아직 대기 중입니다.

## 보존된 예제 계약

이 소스 계약은 SDK `0.3.1`(interface 6)으로 빌드합니다. 위에 기록된 C++ 실행은
더 오래된 SDK interface로 수행했으며 과거 증거로 남습니다. 현재 SDK를 검증한 것도,
새로운 live 호출도 아닙니다. family별 제어는 `ProviderControls`의
닫힌 타입 필드이며 지원하지 않는 family/origin/model 조합은 I/O 전에 거부합니다.
reasoning/sampling/tool 제어, Responses 서버 보관 cursor, deployment header 및 명시적
portable Gemini history는 [provider reference](../docs/reference-en.md)를 참조하세요.
cursor와 portable history는 native replay 권한을 부여하지 않습니다.

### 연구 및 느린 추론 경로

Deep Research (25 / 26)는 supervisor, researcher, compression, final-report 요청별로
완료된 빈 `MaxTokens` outcome에 visible text와 유효하거나 무효인 client tool call이 없을 때만
최대 두 번의 추가 semantic call을 허용합니다. 출력 cap은 두 배로 늘되 16,384를 넘지 않습니다.
research-brief 호출은 이 ladder에 포함되지 않습니다. 추가 호출마다 새 ordinal로 원래 bank의
admission을 통과하고 outcome과 usage를 보존합니다. grant, hold, deadline은 갱신하지 않습니다.
명시적 deadline이 없으면 effect 없는 preparation 한 번으로 설정된 deadline을 알아내고
mediated invoke 전에 해제한 뒤 그 deadline을 고정합니다. 명시적 deadline은 이 단계를 건너뜁니다.
Failure, observer/settlement 오류, 이미 전달한 streaming part는 추가 호출을 유발하지 않습니다.

빈 최종 보고서는 오류입니다. 내용이 있는 `MaxTokens` 보고서는 public 투영에만 `Incomplete`를
표시하며 불변 outcome을 수정하거나 부분 텍스트를 재시도하지 않습니다. 빈 compression이
ladder를 소진하면 diagnostic을 반환하며 성공한 provider result를 꾸며내지 않습니다.

예제 16은 기본 cap 8,192와 ask당 deadline 300초를 유지하며 완료된 빈 응답에만 최대 세 번 호출합니다.
cap을 두 배로 늘리거나 failure를 재시도하지 않습니다. 빈 중간 응답/잘못된 evaluator 점수는
0점으로 바꾸지 않고 실패하며, 잘린 최종 답변은 불완전한 결과로 1로 종료합니다. 예제 28의 rewrite는 low effort와 출력
512 token을 요청하고 빈/공백 응답이면 원래 질문을 그대로 반환합니다. provider timeout은
180초입니다. 이 경로별 설정은 공유 factory의 기본값을 바꾸지 않습니다.

다중 고객 self-evolving chatbot은 judge 이력 접두사를 유효한 UTF-8 경계에서 200바이트
이내로 자르고 예외를 오류와 종료 코드 1로 보고합니다. The Beast의 `baldwin_llm --llm`은
키를 요구하며 오프라인 oracle로 대체하지 않습니다. 타입 요청은 low effort와 기본 cap 1,824를
사용하며 완료된 빈 `MaxTokens` 응답만 기본 cap 3,648로 한 번 재요청합니다. 원래 Outcome과
실제 호출별 usage는 보존합니다. Provider 실패, 끝내 빈 응답, 파싱 불가능한 연산 이름은
완료된 learner로 가장하지 않고 1로 종료합니다. 이 소스 수정은 라이브 provider 검증이나 퍼즐 성공 주장이 아닙니다.

### 이슈 #190 / #317의 유한 진단

`NG_EXAMPLE_MAX_TOKENS`는 ToT, Baldwin, Jarvis, `server_multi` 및 inspection 예제 29/30의 명시적인 양수 호출별 출력 상한입니다. 숨은 reasoning을 포함하며 명시적으로 지정하면 절대 두 배로 늘리지 않습니다. 설정하지 않으면 원래 레시피 기본값을 그대로 유지합니다. `NG_EXAMPLE_EMPTY_REASKS=0`은 기존의 완료된 빈 `MaxTokens` 재요청을 끕니다. 최대/기본값은 ToT 2, Baldwin/Jarvis 1입니다. 이는 새로운 의미적 호출이며 실패나 이미 전달된 tool-call 출력의 재시도가 아닙니다. Jarvis는 별도의 broker ordinal, 원래 admission bank/deadline, 모든 실제 Outcome/report를 유지합니다. 통과를 위해 thinking 설정을 끄지 않으며, 모델 한도 정보를 지어내지 않고, 금전 grant를 갱신하지 않습니다.

다음 제어는 원래 예제 작업량을 줄이기만 합니다.

| 레시피 | 제어와 원래 기본값 | 재요청을 끈 최대 호출 수 |
|---|---|---|
| ToT | `NG_TOT_DEPTH=3` (1..3), `NG_TOT_BRANCHING=3` (1..3), `NG_TOT_BEAM_WIDTH=5` (1..5) | 원래 37; 1/1/1에서 3 |
| Baldwin, 두 모드 | `NG_BALDWIN_POPULATION=6` (2/4/6), `NG_BALDWIN_GENERATIONS=4` (1..4) | 원래 48; 2/1에서 4 |
| 다중 고객 서버 | `NG_MULTI_CUSTOMERS=5`, `NG_MULTI_TURNS=5` (둘 다 1..5) | 원래 90; 고객 1명/4턴에서 14 |

**별도의 호스트 지출 예약** 후 저장소 root에서 실행합니다.

```sh
# Representative canaries, not substitutes for the unchanged original workload.
NG_EXAMPLE_MAX_TOKENS=8192 NG_EXAMPLE_EMPTY_REASKS=0 \
  NG_TOT_DEPTH=1 NG_TOT_BRANCHING=1 NG_TOT_BEAM_WIDTH=1 \
  ./build/example_tree_of_thoughts
NG_EXAMPLE_MAX_TOKENS=8192 NG_EXAMPLE_EMPTY_REASKS=0 \
  NG_BALDWIN_POPULATION=2 NG_BALDWIN_GENERATIONS=1 \
  ./build/cookbook_the_beast_baldwin_llm --llm
NG_EXAMPLE_MAX_TOKENS=8192 NG_MULTI_CUSTOMERS=1 NG_MULTI_TURNS=4 \
  ./build/cookbook_self_evolving_chatbot_multi
# These inspection commands retain sensitive native/provider payloads.
NG_EXAMPLE_MAX_TOKENS=8192 ./build/example_responses_envelope "Reply briefly to hello."
NG_EXAMPLE_MAX_TOKENS=8192 ./build/example_reasoning_effort
```

이 다섯 명령은 모델 호출 3 + 4 + 14 + 1 + 4 = 26회와 설정된 출력 토큰 212,992개로 제한되며 입력 토큰은 별도입니다. 달러 가격을 입증하거나 지출을 승인하지 않습니다. 비용을 써도 되는 모델과 경로를 사용하고 로그는 비공개로 유지하세요. 원래 작업량 qualification은 shape override만 빼고 명시적 cap/재요청 선택은 기록해 둡니다. 이 명시적 설정에서 전체 ToT + Baldwin + 서버는 최대 175회 호출 / 설정된 출력 토큰 1,433,600개입니다.

ToT는 비어 있지 않은 최종 `EXPR`/`CHECK`를 출력해야 합니다. 실제 식을 독립적으로 계산해 4 하나, 7 하나, 8 두 개를 정확히 사용하고 결과가 24인지 확인하세요. 모델의 점수는 산술 oracle이 아닙니다. Baldwin은 두 모드 모두 실제 pipeline fitness와 hit 수를 보여야 합니다. 특정 진화 trace를 강제하거나 live learner를 오프라인 oracle로 대체하지 마세요. 서버는 비어 있지 않은 모델/judge 출력과 직렬화/provider 실패 없이 선택한 네 번째 턴에 도달해야 합니다. 30은 네 가지 effort를 관찰하는 sweep입니다. reasoning 토큰, 실행 시간, 정확도의 단조성은 약속하지 않으며 빈 답변은 Outcome을 보존한 채 exit 2를 반환합니다. 29는 답변 텍스트 없이 tool call만 반환하는 것이 정상일 수 있습니다. 이는 envelope inspection이지 날씨 답변 benchmark가 아닙니다. Provider 실패는 계속 nonzero입니다.

### 체크포인트 및 진화

예제 08은 terminal checkpoint를 fork한 뒤 새 user turn을 시작하는 기존 흐름을 유지합니다.
일시 정지한 reviewer의 resume 예제가 아닙니다. 예제 14는 실제 executor 횟수로 절약량을 계산합니다.
최초 다섯 호출 뒤 실패한 sibling 하나만 다시 실행하면 네 호출을 절약합니다.

예제 32는 smoke/file 모드를 선택하기 전에 `pnoop`를 등록합니다. 저장소 root에서
추적된 seed/task 파일을 사용하세요:

```bash
./build/example_evolution --smoke
./build/example_evolution examples/32_evolution_seed.json examples/32_evolution_task.json
```

실제 JSON의 `best.compiled`, `best.validated`, `best.executed`, `best.correct`를 확인하세요.
`compile_passed`만으로 올바른 실행을 입증하지 못합니다. file 모드에서도 built-in node type과
이 demo의 `pnoop`를 쓸 수 있지만 custom type은 host 등록이 필요합니다.
현재 Linux x86_64에서 이 seed/task 쌍을 사용한 file 모드 실행은 네 `best` flag가 모두 true였습니다.
등록되지 않은 node type은 성공한 compile/execute/correct 결과 없이 실패했습니다.

### A2A dialect 및 task snapshot

예제 37은 card의 interface와 첫 RPC에서 선택한 dialect를 출력합니다. client는 호환되는
JSON-RPC 0.x/1.0 card interface를 선택하며 card URL로 설정된 RPC endpoint를 바꾸지 않습니다.
card를 fetch하지 않았다면 숫자 `-32601`일 때만 초기 dialect probe가 가능하며 SSE 전달 뒤에는
재전송하지 않습니다. 예제 38은 두 dialect를 광고하고 최초/갱신 task snapshot을 표시합니다.
최초 task는 완료된 답변이 아닙니다. server의 응답 encoding은 method 표기와 별개로
`A2A-Version` header가 선택합니다. 1.0은 PascalCase method, flat part,
`returnImmediately`를 쓰고 stream의 status/artifact update를 누적합니다. caller는 완료/중단된
agent status text, 첫 artifact text, 마지막 agent history text 순서로 답변을 선택합니다.

## 개념 모델: 세 층, 가운데는 JSON

각 예제는 다음 세 가지 구성 중 하나입니다.

1. **내장 노드만 사용** (02, 04, 07, 14): `llm_call` / `tool_dispatch` / mock provider 노드로 그래프 전체를 JSON으로 연결하며, 서브클래싱이 없습니다. `create_react_graph()`가 만드는 그래프와 가장 비슷합니다.
2. **직접 만든 `GraphNode` 서브클래스** (05, 09, 10, 25): `run(NodeInput)` 본문을 직접 제어합니다. `NodeOutput`으로 `ChannelWrite`, `Send`, `Command`를 내보냅니다. Send fan-out과 Command 경로 재정의는 이 구성에서 다룹니다.
3. **타입 provider 요청과 Outcome** (13, 15, 16, 17): 검증된 SDK 승인(admission), 순서 있는 이벤트, 불변 Outcome을 씁니다. descriptor interpreter와 WebSocket adapter는 없습니다.

그래프 정의는 JSON 형태(`std::map<std::string, json>`)입니다.
[Python 예제](../bindings/python/examples/)도 같은 토폴로지 형식을 씁니다.
Python 예제의 provider 요청과 Outcome은 토폴로지 JSON이 아니라 별도의 타입 객체입니다.

## Responses inspection 계약 (13 / 29 / 30 / 34)

13은 타입 Responses streaming 요청이며 이벤트는 문자열 callback이 아닙니다.
29는 성공/실패의 전체 Outcome, `wire_envelope`, 순서 있는 모든 `wire_output` item과
타입 part, function argument, citation, reasoning, opaque hosted output, artifact를 보존합니다.
raw inspection 출력은 민감하며 안전한 telemetry/export 형식이 아닙니다.
30은 `none`, `low`, `medium`, `high`를 sweep하면서 전체 Outcome을 보존합니다.
reasoning/input/output/total/provider-reported-total 및 extra count는 nullable 64-bit evidence이며
stage/quality/conflict를 포함합니다. 누락은 zero가 아니며 visible text나 예약량은 usage가 아닙니다.
34의 일곱 섹션은 function(calculator), web search, image generation, file search,
tool search, `shell.environment`의 skills, shell(`container_auto`)입니다.
`OPENROUTER_VECTOR_STORE_ID`는 file search 조건이며 `OPENROUTER_SKILL_ID`는 기본
`openai-spreadsheets` skill을 교체합니다. 이 inspection demo는 function tool을 알리지만 직접 실행하지 않습니다.
순서 있는 타입/raw event와 전체 terminal을 유지합니다. hosted tool은 route에 따라 지원되지 않거나
추가 비용이 발생할 수 있습니다. 타입 admission은 live 호환성 보장이 아닙니다.
WebSocket/primitive 실행 예제는 유지하지 않습니다. Images/Veo/Decisions는 별도 타입 NeoGraph client이며 chat spending grant를 상속하지 않습니다.

Decisions choice criteria는 문자열 배열이 아니라 choice를 키로 하는 객체입니다.
현재 별도 typed client는 raw `SchemaProvider::request_json()` body 없이 올바른 형태를 표현합니다.

```cpp
#include <neograph/llm/decisions_client.h>
neograph::llm::DecisionsChoiceQuestion question;
question.instructions = "Choose whether the supplied answer addresses the question.";
question.criteria = {{"accept", "The answer addresses the question."},
                     {"reject", "The answer does not address the question."}};
neograph::llm::DecisionsRequest request;
request.state = {{"question", "What is 2 + 2?"}, {"answer", "4"}};
request.questions.emplace("decision", std::move(question));
```

이는 요청 구성일 뿐 새 유료 요청이나 vendor 정확도의 증거가 아닙니다.
