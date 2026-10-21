<!-- neograph-i18n: source=bindings/python/examples/README.md locale=ko source_sha256=cdc4300edb845368b427f2b90eb0051e74fae14610f7039ae415a5bf6ed69281 -->
# Python API 예제

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

이 스크립트 47개는 그래프 상태, 라우팅, 도구, provider 요청, Program API, 프로토콜 호스팅을 보여 줍니다. 스크립트마다 한 가지를 보여 줍니다. 대부분은 오프라인으로 실행합니다. 스크립트 모델이 provider 역할을 하므로 API 키도 네트워크도 필요하지 않습니다. 실제 모델도 호출할 수 있는 예제는 `--live`를 받습니다.

**Offline** 열은 각 행이 필요로 하는 것을 알려 줍니다. `yes`: 키와 네트워크 없이 실행되며 `bindings/python/tests/test_examples_offline.py`가 자동으로 테스트합니다. `key`: provider API 키가 필요합니다. `service`: 다른 프로세스, 서버 또는 추가 패키지가 필요합니다. `paired`: 행에 적힌 예제와 함께 오프라인으로 실행됩니다.

## 설정

```bash
pip install neograph-engine
python 01_minimal.py
```

이 디렉터리에서 명령을 실행하세요. 오프라인 예제에는 그 밖에 필요한 것이 없습니다. wheel 대신 소스 빌드로 실행하려면 `-DNEOGRAPH_BUILD_PYBIND=ON`으로 구성하고 `PYTHONPATH`를 빌드 디렉터리와 `bindings/python`으로 설정하세요. 빌드 트리는 패키지의 Python 의존성을 설치하지 않으므로 먼저 `pip install 'pydantic>=2' certifi`를 실행하세요. `import neograph_engine`에는 pydantic이 필요하며, certifi가 없으면 TLS는 호스트의 신뢰 저장소를 사용합니다.

## 여기서 시작하세요

순서대로 읽으세요. 예제마다 개념이 하나씩 더해집니다. 모두 오프라인으로 실행됩니다.

| 단계 | 예제 | Offline | 배우는 것 |
|---|---|---|---|
| 1 | [`01_minimal.py`](01_minimal.py) | yes | 첫 그래프: `@ng.node`로 등록한 함수가 채널 하나를 읽어 다른 채널에 쓰고, 21이 42가 된다. |
| 2 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | yes | Python `Tool`과 내장 `tool_dispatch` 노드: 스크립트에 적은 도구 호출이 `42`로 돌아온다. |
| 3 | [`03_send_fanout.py`](03_send_fanout.py) | yes | `Send`가 실행 중에 항목마다 분기 하나를 만들고 그 분기를 합친다: 제곱값 여덟 개가 순서와 상관없이 모인다. |
| 4 | [`05_openai_provider.py`](05_openai_provider.py) | yes | 내장 `llm_call` 노드로 하는 모델 호출 한 번. 스크립트 모델이 `hello world`라고 답한다. `--live`는 호스팅 모델을 호출한다. |
| 5 | [`06_react_agent.py`](06_react_agent.py) | yes | ReAct 루프: `llm_call`, `tool_dispatch`, `has_tool_calls` 조건부 엣지. `calc`가 `4053`을 반환하고 모델이 답한다. |
| 6 | [`77_dynamic_interrupt.py`](77_dynamic_interrupt.py) | yes | Human-in-the-Loop: 노드가 `NodeInterrupt`를 발생시키고, 체크포인트가 실행을 보관하며, `resume`이 사람의 답을 노드에 돌려준다. |
| 7 | [`29_program_hello.py`](29_program_hello.py) | yes | Program: 짧은 JavaScript Program이 Python 노드가 있는 그래프 하나를 정의하고 `LocalProgramHost`에서 실행한다. |

7단계를 마친 뒤에는 다음 표에서 목표별로 고르거나 영역별 색인을 둘러보세요.

## 하고 싶은 일로 찾기

목표를 고르고 그에 맞는 예제를 읽으세요. 표는 기초에서 프로토콜 순서로 이어집니다.

| 하고 싶은 일 | 읽을 예제 | Offline |
|---|---|---|
| 첫 그래프를 실행하기 | [`01_minimal.py`](01_minimal.py) | yes |
| 그래프에 Python 도구 추가하기 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | yes |
| Send로 분기 fan-out하기 | [`03_send_fanout.py`](03_send_fanout.py) | yes |
| asyncio로 그래프 여러 개를 동시에 실행하기 | [`04_async_concurrent.py`](04_async_concurrent.py) | yes |
| 그래프에서 모델 호출하기 | [`05_openai_provider.py`](05_openai_provider.py) | yes |
| 도구를 쓰는 에이전트 루프 만들기 | [`06_react_agent.py`](06_react_agent.py) | yes |
| 의도에 따라 라우팅하기 | [`08_intent_routing.py`](08_intent_routing.py) | yes |
| 다음 노드를 고르면서 한 번에 상태 쓰기 | [`10_command_routing.py`](10_command_routing.py) | yes |
| 채널이 쓰기를 합치는 방식 고르기(덮어쓰기 또는 추가) | [`79_channels.py`](79_channels.py) | yes |
| 내 리듀서나 엣지 조건 등록하기 | [`80_custom_registries.py`](80_custom_registries.py) | yes |
| 실행을 진행되는 대로 지켜보기 | [`75_stream_modes.py`](75_stream_modes.py) | yes |
| 실패하는 노드를 백오프로 재시도하기 | [`76_retry_policy.py`](76_retry_policy.py) | yes |
| 노드가 계속 진행하기 전에 사람에게 묻게 하기 | [`77_dynamic_interrupt.py`](77_dynamic_interrupt.py) | yes |
| 단계 한도에서 루프 멈추기 | [`78_max_steps.py`](78_max_steps.py) | yes |
| 승인을 기다리며 멈췄다가 재개하기 | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | yes |
| 도구 호출을 허용, 거부하거나 인터럽트하기 | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) | yes |
| 저장된 상태를 읽고 포크하기 | [`09_state_management.py`](09_state_management.py) | yes |
| 그래프를 JSON으로 저장하고 불러오기 | [`14_graph_to_json.py`](14_graph_to_json.py) | yes |
| 토큰을 LangChain 형태의 청크로 스트리밍하기 | [`19_streaming_messages.py`](19_streaming_messages.py) | yes |
| OpenTelemetry로 실행 추적하기 | [`20_otel_tracing.py`](20_otel_tracing.py) | yes |
| I/O 중심 도구를 동시에 실행하기 | [`25_async_tools.py`](25_async_tools.py) | yes |
| JavaScript Program 실행하기 | [`29_program_hello.py`](29_program_hello.py) | yes |
| 병렬 분기를 join하고, join이 멈추는 이유 확인하기 | [`30_program_barrier_join.py`](30_program_barrier_join.py) | yes |
| 중복된 두 소스 중 먼저 오는 쪽만 기다리기 | [`31_program_any_of_join.py`](31_program_any_of_join.py) | yes |
| 실패한 Program 노드를 운영자에게 넘기기 | [`32_program_failure_routing.py`](32_program_failure_routing.py) | yes |
| Program이 쓸 수 있는 한도 정하기 | [`33_program_budgets.py`](33_program_budgets.py) | yes |
| Program 실행을 멈췄다가 재개하기 | [`34_program_pause_resume.py`](34_program_pause_resume.py) | yes |
| 모델 호출을 되풀이하지 않고 Program 실행 포크하기 | [`35_program_fork.py`](35_program_fork.py) | yes |
| 크래시 뒤 Program 실행 복구하기 | [`36_program_crash_recovery.py`](36_program_crash_recovery.py) | yes |
| Program 노드에 모델을 연결하고 사용량 읽기 | [`37_program_model_node.py`](37_program_model_node.py) | yes |
| 컴파일러 진단을 보고 Program 고치기 | [`38_program_diagnostics.py`](38_program_diagnostics.py) | yes |
| 실행 두 개를 경주시키고 느린 쪽 취소하기 | [`39_program_main_race.py`](39_program_main_race.py) | yes |
| 느린 노드 시도를 타임아웃시키고 재시도하기 | [`40_program_attempt_timeout.py`](40_program_attempt_timeout.py) | yes |
| MCP 서버의 도구 사용하기 | [`26_mcp_tools.py`](26_mcp_tools.py) | yes |
| 내 에이전트를 A2A로 호스팅하기 | [`27_a2a_server.py`](27_a2a_server.py) | yes |
| Python에서 A2A 에이전트 호출하기 | [`45_a2a_client.py`](45_a2a_client.py) | yes |
| 내 에이전트를 ACP로 호스팅하기 | [`28_acp_agent.py`](28_acp_agent.py) | yes |
| 에이전트가 자기 그래프를 고쳐 쓰게 하기 | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | yes |
| Reflexion, Self-Ask, 토론 시험하기 | [`11_reflexion.py`](11_reflexion.py) | key |

## 영역별 색인

### Core 엔진

노드, 채널, 라우팅, 스트리밍, 재시도, 인터럽트를 다루며 모델은 쓰지 않습니다. 예제 14가 스크립트 옆에 `my_graph.json`을 쓰고 15가 읽으므로, 이 순서대로 실행하세요.

| # | 예제 | Offline | 보여 주는 것 |
|---|---|---|---|
| 01 | [`01_minimal.py`](01_minimal.py) | yes | `@ng.node`로 등록한 함수가 채널을 읽어 두 배로 만들고 다시 쓴다. 21이 42가 된다. |
| 02 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | yes | Python `Tool`과 내장 `tool_dispatch` 노드: 스크립트에 적은 도구 호출이 `42`로 돌아온다. |
| 03 | [`03_send_fanout.py`](03_send_fanout.py) | yes | `Send` 분기 여덟 개가 제곱값 `[0, 1, 4, 9, 16, 25, 36, 49]`를 합친다. append 순서는 가정하지 않는다. |
| 04 | [`04_async_concurrent.py`](04_async_concurrent.py) | yes | `asyncio.gather`로 비동기 실행 여덟 개를 돌리면 `[0, 2, 4, 6, 8, 10, 12, 14]`가 나오고, 스트리밍 실행은 노드 이벤트를 낸다. |
| 10 | [`10_command_routing.py`](10_command_routing.py) | yes | `Command`가 다음 노드를 고르고 상태를 쓴다. 입력 200, 50, -10이 각각 accept, manual, reject로 간다. |
| 14 | [`14_graph_to_json.py`](14_graph_to_json.py) | yes | 그래프 정의는 평범한 JSON이다. dict로 실행한 뒤 `my_graph.json`으로 저장한다. |
| 15 | [`15_graph_from_json.py`](15_graph_from_json.py) | yes | 예제 14의 `my_graph.json`을 불러와 5를 10으로, 100을 200으로 두 배로 만든다. 14를 먼저 실행해야 한다. 사용자 정의 노드 타입은 코드에서 등록한다. |
| 19 | [`19_streaming_messages.py`](19_streaming_messages.py) | yes | `message_stream`이 토큰 이벤트를 LangChain 형태의 청크로 바꾼다. 스크립트에 적은 토큰 다섯 개가 `Octopuses have three hearts.`를 이룬다. |
| 75 | [`75_stream_modes.py`](75_stream_modes.py) | yes | 스트림 모드: 같은 실행을 VALUES, UPDATES, EVENTS, DEBUG 모드로 관찰하고, 모드마다 전달되는 이벤트를 센다. |
| 76 | [`76_retry_policy.py`](76_retry_policy.py) | yes | `RetryPolicy`: 노드가 두 번 실패하면 엔진이 지수 백오프로 재시도한다. 재시도 횟수를 줄이면 재시도가 바닥났을 때 호출한 쪽이 받는 결과를 볼 수 있다. |
| 77 | [`77_dynamic_interrupt.py`](77_dynamic_interrupt.py) | yes | `NodeInterrupt`: 노드가 이를 발생시키면 페이로드가 호출한 쪽에 물어볼 내용을 알려 주고, `resume`이 답(승인 또는 거절)을 노드에 돌려준다. |
| 78 | [`78_max_steps.py`](78_max_steps.py) | yes | `max_steps`: 루프가 단계 한도에서 멈추고, 결과가 그 사실을 알려 준다(`max_steps_exhausted`, `RunStatus.StepLimit`). 마지막 단계에서 스스로 끝난 실행에는 표시가 붙지 않는다. |
| 79 | [`79_channels.py`](79_channels.py) | yes | 채널과 리듀서: `overwrite`와 `append`의 차이, `initial` 값, 그리고 쓰기 카운터 하나를 모든 채널이 공유하는 채널 버전. |
| 80 | [`80_custom_registries.py`](80_custom_registries.py) | yes | 직접 만든 리듀서와 조건: `ReducerRegistry.register_reducer`로 심사 세 명의 점수 중 가장 높은 값을 순서와 상관없이 남기고, `ConditionRegistry.register_condition`으로 그 값에 따라 경로를 고른다. |

### 에이전트 패턴

그래프 안의 모델(도구가 있거나 없거나)입니다. `key` 행은 호스팅 모델을 호출합니다.

| # | 예제 | Offline | 보여 주는 것 |
|---|---|---|---|
| 05 | [`05_openai_provider.py`](05_openai_provider.py) | yes | `llm_call`로 하는 모델 호출 한 번. 스크립트 모델이 `hello world`라고 답하고, `--live`는 호스팅 모델을 호출한다. |
| 06 | [`06_react_agent.py`](06_react_agent.py) | yes | Responses 프로토콜 위의 ReAct: 모델이 낸 `calc` 호출이 `4053`을 반환하고 모델이 답한다. |
| 08 | [`08_intent_routing.py`](08_intent_routing.py) | yes | 질문 세 개가 `route_channel`을 거쳐 math, translate, general 전문가로 가고, 전문가마다 답을 쓴다. |
| 11 | [`11_reflexion.py`](11_reflexion.py) | key | Reflexion: actor와 critic 호출이 반성을 다음 시도로 넘긴다. 범위를 제한한 교육용 버전이다. |
| 12 | [`12_self_ask.py`](12_self_ask.py) | key | Self-Ask: 중간 질문과 답이 스크래치패드에 쌓인 뒤 최종 답이 나온다. 검색 서비스는 연결하지 않았다. |
| 13 | [`13_multi_agent_debate.py`](13_multi_agent_debate.py) | key | `Send` 분기 두 개가 반대 입장에서 논쟁하고, 심판 하나가 합쳐진 논거를 읽는다. |
| 16 | [`16_deep_research_chat.py`](16_deep_research_chat.py) | key | 연구 모드가 있는 Gradio 채팅: 웹 검색이 아니라 모델 지식을 쓰는 병렬 researcher가 질문 세 개짜리 보고서를 만든다. `gradio`가 필요하다. |
| 17 | [`17_deep_research_crawl4ai.py`](17_deep_research_crawl4ai.py) | key | 같은 연구 그래프에 Crawl4AI로 하는 실제 웹 검색과 선택적 PostgreSQL 체크포인트를 더했다. `gradio`, `requests`, Crawl4AI 서버가 필요하다. |
| 22 | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | yes | 그래프가 자기 출력을 점수화하고, 실패하면 모델에 고친 정의를 요청한다. 등록되지 않은 노드 타입을 쓴 재작성은 이유와 함께 거부되고, 승인된 재작성은 컴파일되어 두 번째 반복에서 목표를 이룬다. `--live`는 호스팅 모델을 호출한다. |
| 23 | [`23_evolving_chat_agent.py`](23_evolving_chat_agent.py) | yes | 채팅 몇 턴 뒤에 evolver 모델이 새 에이전트 정의를 제안한다. `validate_agent`가 이를 승인하고, 새 엔진이 체크포인트 저장소를 공유하므로 앞선 메시지 네 개가 그대로 남으며, `__graph_meta__`에 버전과 해시가 기록된다(애플리케이션 메타데이터이며 재생(replay) 증거가 아니다). |

### Program API

제한된 JavaScript Program API는 Core 그래프 위에서 동작합니다. 구성 요소는 `ProgramRegistryBuilder`, `ProgramCompiler`, `LocalProgramHost`입니다. 설명은 [Program and QuickJS](../../../docs/python-binding.md#program-and-quickjs)에 있고, join, 실패 라우팅, 예산, 일시 중지와 재개, fork, 크래시 복구는 [`bindings/python/tests/program_e2e/`](../tests/program_e2e/README.md)의 엔드투엔드 시나리오로 확인합니다. 아래 예제는 이 중 하나씩을 `LocalProgramHost`에서 보여 줍니다. 모두 오프라인으로 실행되며, 노드가 모델을 호출하는 곳에서는 스크립트 모델이 답합니다. 공통 설정은 `_program.py`에 있고, 예제 29가 그 설정을 한 번 풀어서 보여 줍니다.

| # | 예제 | Offline | 보여 주는 것 |
|---|---|---|---|
| 29 | [`29_program_hello.py`](29_program_hello.py) | yes | JavaScript Program이 Python 노드가 있는 그래프 하나를 정의하고 `LocalProgramHost`에서 실행한다. `status: Completed`, `value: 1`, `trace: ['work']`를 출력한다. |
| 30 | [`30_program_barrier_join.py`](30_program_barrier_join.py) | yes | 검토자 세 명이 병렬로 실행되고 `barrier`가 모두를 기다린다. 한 명이 다른 곳으로 라우팅하면 Program은 `Failed`로 끝나며, `P_CORE_BARRIER_STALLED`와 빠진 노드를 알려 주는 witness가 남는다. |
| 31 | [`31_program_any_of_join.py`](31_program_any_of_join.py) | yes | 일반 멤버 둘을 기다리는 join은 한쪽을 대신하는 보정 노드가 끼어들면 멈춘다. 컴파일러가 직접 제안한 `{any_of: [...]}` 슬롯을 소스에 적용하면 고친 Program이 완료된다. |
| 32 | [`32_program_failure_routing.py`](32_program_failure_routing.py) | yes | TEST가 실패해 두 번 재시도된 뒤, 마지막 실패가 `interruptBefore`가 걸린 처리 노드로 라우팅된다. 운영자가 `retry`로 재개하면 Program이 완료되고, `abort`로 재개하면 실패 종료 노드가 `plan.user_abort`와 함께 Program을 `Failed`로 끝낸다. |
| 33 | [`33_program_budgets.py`](33_program_budgets.py) | yes | 바인딩된 `llm_call` 노드 둘이 각각 600토큰을 보고하고 예산은 1,000토큰이라 실행은 `BudgetExhausted`로 끝난다. 승인된 예산과 다른 예산으로 시작한 실행은 `P_START_BUDGET`으로 거부된다. |
| 34 | [`34_program_pause_resume.py`](34_program_pause_resume.py) | yes | `interruptBefore`가 b 노드 앞에서 실행을 멈추고 `host.resume(run_id, value)`가 이어 간다. 멈춘 시점의 trace는 `['a']`, 재개한 뒤의 trace는 `['b', 'c']`이며 `main()`은 운영자의 답으로 끝난다. |
| 35 | [`35_program_fork.py`](35_program_fork.py) | yes | 끝난 모델 호출 옆에서 멈춘 실행을 다른 답으로 fork한다. 끝난 호출은 체크포인트에서 재생(replay)하므로 스크립트 모델이 받는 요청은 통틀어 한 번이고, fork에는 토큰이 청구되지 않는다. |
| 36 | [`36_program_crash_recovery.py`](36_program_crash_recovery.py) | yes | 프로세스 A가 질문까지 Program을 실행하고, 답을 받아 모델을 호출한 뒤 종료된다. 프로세스 B가 같은 `sqlite_directory`를 열어 `reconnect`를 호출하면 실행이 끝나고, 저널에 기록된 응답은 재생(replay)되며 호출 비용은 한 번만 청구된다. Linux와 macOS 전용이다. |
| 37 | [`37_program_model_node.py`](37_program_model_node.py) | yes | Program의 `llm_call` 노드를 호스트가 `node_context`로 provider에 바인딩한다. 스크립트 모델이 보고한 30 + 12토큰이 `usage.model_tokens: 42`, `remaining_budget`, 가격이 매겨진 금액 기록에 반영된다. 바인딩이 없는 호스트는 `P_ADMIT_PROVIDER`로 Program을 거부한다. |
| 38 | [`38_program_diagnostics.py`](38_program_diagnostics.py) | yes | 스크립트 모델이 엣지에서 노드 이름을 잘못 적는다. 컴파일은 `P_CORE_E3`, 수정 제안, 후보 목록과 함께 실패한다. 수정 루프가 제안을 적용하고 `soundness_check=True`로 다시 컴파일한 뒤 Program을 승인하며, 모델 호출은 한 번뿐이다. |
| 39 | [`39_program_main_race.py`](39_program_main_race.py) | yes | `main()`이 `ng.race`, `ng.quorum`, `ng.await`로 Core 실행 여러 개를 시작한다. 먼저 끝난 실행이 이기고, 느린 실행은 취소 토큰이 바뀌는 것을 보며, 기한을 넘긴 `ng.await`는 Program을 `P_AWAIT_TIMEOUT`과 함께 `TimedOut`으로 끝낸다. |
| 40 | [`40_program_attempt_timeout.py`](40_program_attempt_timeout.py) | yes | 5초 걸리는 노드가 `attempt_timeout_ms`에 걸려 `NG.Timeout`으로 실패하고 재시도에서 성공하며, `usage.node_retries`는 1이다. 늘 느린 노드는 Program을 `NG.Timeout`과 함께 `Failed`로 끝낸다. |

### 프로토콜: MCP, A2A, ACP

여기서 MCP와 A2A는 클라이언트(`ng.mcp`, `ng.a2a`)입니다. A2A와 ACP 에이전트는 공식 Python SDK와 `ProtocolHostAdapter`로 호스팅합니다. 이 영역의 예제는 모두 오프라인으로 실행됩니다. 빈 포트에 자체 서버를 띄우거나 에이전트를 자식 프로세스로 실행합니다. SDK를 쓰는 예제는 `neograph-engine[a2a]` 또는 `neograph-engine[acp]`가 필요하며, 없으면 건너뜁니다. 서버는 `bindings/python/tests/test_protocol_sdk_e2e.py`가 더 깊이 검증합니다.

| # | 예제 | Offline | 보여 주는 것 |
|---|---|---|---|
| 26 | [`26_mcp_tools.py`](26_mcp_tools.py) | yes | 로컬 MCP 서버를 띄워 `fetch`를 찾고, 재진입 정책을 명시해 호출 세 번을 실행한다. MCP가 켜진 빌드가 필요하며 스모크 테스트는 Linux에서 한다. |
| 27 | [`27_a2a_server.py`](27_a2a_server.py) | yes | 공식 A2A SDK로 에이전트를 호스팅한다. `127.0.0.1:9999`의 에이전트 카드와 JSON-RPC, 스트리밍 아티팩트를 제공한다. `--self-check`는 빈 포트에서 서버를 띄우고 SDK의 클라이언트로 두 턴을 진행한다. `neograph-engine[a2a]`가 필요하다. |
| 28 | [`28_acp_agent.py`](28_acp_agent.py) | yes | 공식 ACP SDK로 stdio 위에서 에이전트를 호스팅한다. initialize, 세션 생성, prompt, 토큰 업데이트, `end_turn`을 거친다. `--self-check`는 에이전트를 자식 프로세스로 실행하고 SDK의 클라이언트로 두 턴을 진행한다. `neograph-engine[acp]`가 필요하다. |
| 45 | [`45_a2a_client.py`](45_a2a_client.py) | yes | A2A 클라이언트: `ng.a2a.A2AClient`가 에이전트 카드를 가져오고 메시지를 보내며, 예제 27의 SDK 에이전트가 내보내는 스트림을 읽는다. 그 에이전트는 이 스크립트가 빈 포트에서 스레드로 띄운다. `neograph-engine[a2a]`가 필요하다. |

### 영속성과 운영

체크포인트, 상태, 캐시, 트레이싱, 전송, 도구 동시성을 다룹니다.

| # | 예제 | Offline | 보여 주는 것 |
|---|---|---|---|
| 07 | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | yes | 체크포인트가 결제 디스패치 전에 멈추고, 승인하면 모의 결제 한 건만 재개된다. |
| 09 | [`09_state_management.py`](09_state_management.py) | yes | `get_state`와 `fork`: alpha가 count 11에 이르고, beta가 그 값을 포크하며, 알 수 없는 스레드에는 체크포인트가 없다. |
| 18 | [`18_node_cache.py`](18_node_cache.py) | key | 주제 두 개에 걸친 다섯 번의 실행에서 provider를 쓰는 노드는 두 번만 실행되고, 반복된 입력은 캐시된 쓰기를 재생(replay)한다. |
| 20 | [`20_otel_tracing.py`](20_otel_tracing.py) | yes | 콘솔에 실행과 노드 세 개의 OpenTelemetry span이 출력되고, 최종 경로는 `['A', 'B', 'C']`이다. `opentelemetry-api`와 `opentelemetry-sdk`가 필요하다. |
| 21 | [`21_http2_transport.py`](21_http2_transport.py) | key | 같은 SDK로 HTTP/1.1과 HTTP/2를 비교한다. TLS Chat 엔드포인트와 HTTP/2를 지원하는 libcurl이 필요하다. 결과는 이 엔드포인트의 특성일 뿐 순위가 아니다. |
| 24 | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) | yes | 도구 게이트가 형제 도구 어느 쪽이든 실행되기 전에 멈춘다. 거부하면 `list_files`만, 승인하면 둘 다 한 번씩 실행된다. 셸 동작은 모의이다. |
| 25 | [`25_async_tools.py`](25_async_tools.py) | yes | I/O 중심 작업에서 동기 `Tool`과 겹쳐 실행되는 `AsyncTool`을 비교하고, 이어서 CPU 중심 작업에서 GIL 경계를 보여 준다. 시간 비율은 관찰값이며 스모크 테스트는 Linux에서 한다. |

## 오프라인, 스크립트 모델, 라이브

모델로 구동되는 예제는 `_common.py`의 `example_provider(script)`로 provider를 만듭니다. `--live`를 주지 않으면 `_scripted.ScriptedModel`이 응답합니다. 이 클래스는 `127.0.0.1`에서 동작하는 작은 서버로, 스크립트(텍스트, 도구 호출, 스트리밍, 사용량, HTTP 실패)에 따라 OpenAI Chat 또는 Responses 프로토콜로 응답합니다. 실제 `SchemaProvider`의 요청 구성, SSE 파싱, outcome 처리가 이 서버를 상대로 실행됩니다. 모델이 아니라서 무슨 말을 할지는 스크립트가 정합니다.

```python
from _common import example_provider
from _scripted import call_tool, say

script = [call_tool("calc", expression="2 + 3"), say("5")]   # call 1 asks for a tool, call 2 answers
with example_provider(script) as chosen:
    provider = chosen.provider        # a SchemaProvider; chosen.scripted is None with --live
```

스크립트는 `turn.last_user_text`, `turn.system_text`, `turn.last_tool_result`를 읽는 함수 `(turn) -> Reply`일 수도 있습니다. `chosen.scripted.call_count`로 모델 호출이 몇 번 있었는지 단언할 수 있습니다. 규칙은 `_scripted.py`의 docstring에 있습니다.

`--live`, `NG_EXAMPLE_LIVE=1`, `OPENAI_API_BASE`, `NG_PROVIDER_DESCRIPTOR` 중 하나를 쓰면 설정된 엔드포인트를 대신 선택합니다. 호스팅 엔드포인트에는 `OPENAI_API_KEY`가 필요하고 선택적으로 `OPENAI_MODEL`(기본값 `gpt-6-luna`)을 쓰며, 둘 다 프로세스 환경에서만 읽습니다. `_common.py`는 `.env` 파일을 읽지 않으며, 키가 없으면 성공한 실행이 아니라 오류입니다. `_common.py`는 닫힌 OpenAI Chat 또는 Responses descriptor를 승인(admission)한 뒤 `SchemaProvider`를 만듭니다. 타입 SDK는 libcurl HTTP를 쓰며, 제거된 provider 클래스, completion 파라미터, WebSocket 경로, 전송 백엔드 선택기는 사용할 수 없습니다. `OPENAI_API_BASE`는 origin 또는 게이트웨이 접두사이며 전체 `/v1/chat/completions` 경로가 아닙니다. 시스템 신뢰 저장소에 CA가 없는 HTTPS peer는 `NG_EXAMPLE_CA_FILE`도 필요합니다. 대신 `NG_PROVIDER_DESCRIPTOR`로 승인된 완전한 descriptor JSON 파일을 지정할 수도 있습니다. 타입 요청과 outcome 계약은 [Python 바인딩 가이드](../../../docs/python-binding.md)를 참고하세요.

## 상태와 스케줄링

노드는 현재 슈퍼스텝의 상태를 읽고 채널 쓰기를 반환합니다. 엔진은 다음 슈퍼스텝 전에 이 쓰기를 선언된 리듀서로 병합합니다. `Send`는 분기별 입력을 제공하고, `Command`는 상태를 갱신하며 다음 노드를 선택합니다. 워커 풀이 있어도 CPython GIL 아래에서는 CPU 중심 Python 콜백이 병렬로 실행되지 않습니다.

`GraphEngine.compile()`은 Python으로 작성한 딕셔너리뿐 아니라 JSON에서 불러온 딕셔너리도 받습니다. 정의에는 연결 구조가 저장될 뿐 실행 가능한 Python 클래스는 저장되지 않으므로, 저장된 정의를 컴파일하기 전에 사용자 정의 노드 팩터리를 등록하세요. 예제 14와 15는 스크립트 옆에 `my_graph.json`을 쓰고 읽습니다. 이 두 예제를 검증할 때는 버려도 되는 체크아웃이나 이 디렉터리의 복사본을 사용하세요.

## 대화형 및 프로토콜 시나리오

16번과 17번을 실행하려면 `gradio`를 설치하고, 스크립트를 실행한 뒤 출력된 로컬 URL을 여세요. `hello`를 보내고, 이어서 `research apples`를 보내세요. 17번에서는 실제 Crawl4AI `/md` 서비스, 또는 이를 충실하게 흉내 내는 로컬 peer가 `{"success": true, "markdown": "..."}`를 반환해야 합니다. 실행 중인 빌드가 Postgres를 지원하고 데이터베이스에 연결할 수 있을 때만 Postgres를 설정하세요. 엔진 상태는 UI 기록과 따로 확인하세요.

예제 27과 28은 와이어 처리에 공식 Python SDK를, 체크포인트를 인식하는 엔진 실행에 `ProtocolHostAdapter`를 씁니다. A2A를 확인하려면 SDK 클라이언트가 에이전트 카드를 가져오고, 메시지를 보내고, 스트림을 끝까지 읽어야 합니다. ACP를 확인하려면 클라이언트가 서브프로세스로 연결해 `initialize`, `session/new`, `session/prompt`를 호출해야 하며, stdout은 프로토콜 메시지 전용입니다. 같은 컨텍스트/세션에서 두 번째 프롬프트를 보내고 `(turn 2)`가 나오는지 확인하세요. 취소하면 진행 중인 요청을 멈춰야 하며, 성공처럼 보이는 대체 결과를 만들어서는 안 됩니다.

영속 ACP 세션을 쓰려면 `NEOGRAPH_ACP_POSTGRES_URL`과 `NEOGRAPH_ACP_SQLITE_PATH` 중 정확히 하나를 설정하세요. 첫 프롬프트가 끝나면 `session/load`에 필요한 체크포인트가 만들어집니다. 세션마다 활성 에이전트 프로세스를 하나만 두세요. 체크포인트 저장소는 프로세스 사이의 동시 쓰기를 직렬화하지 않습니다. 이 예제는 풍부한 콘텐츠 블록을 그대로 돌려주지만, 이미지/오디오 콘텐츠를 해석하거나 편집기의 파일 시스템/터미널 콜백을 제공하지는 않습니다.

## 배포 이름과 import 이름

```python
import neograph_engine as ng
from neograph_engine.llm import SchemaProvider
```

배포 이름은 `neograph-engine`입니다. PyPI의 `neograph`는 관련 없는 프로젝트입니다.

## 예제 추가하기

번호는 재사용하지 않으므로, 현재 가장 큰 번호보다 하나 큰 번호를 쓰세요. 새 예제는 파일 하나와 표의 행 하나로 이루어집니다.

1. 파일 이름을 `NN_short_name.py`로 짓고 모듈 docstring으로 시작합니다. 몇 줄로 보여 주는 내용, `Run:` 명령, 그다음 테스트 주석을 적습니다.
2. 그 docstring에 테스트 방법을 적습니다. 오프라인 예제는 반드시 출력해야 하는 줄을 적고, 키나 서비스가 필요한 예제는 오프라인으로 실행할 수 없는 이유를 적습니다(주석 문법은 `examples/run_example_smoke.py` 맨 위에 있습니다).
3. 모델로 구동되는 provider는 `example_provider(script)`로 만들어, 기본은 오프라인이고 `--live`를 받게 합니다.
4. 그 예제가 속한 영역의 표에 Offline 값과 함께 행을 추가합니다.
5. `python -m pytest bindings/python/tests/test_examples_offline.py`와 `python scripts/check_examples.py`를 실행합니다. 두 번째는 주석이 없는 예제, 중복된 번호, 주석과 어긋나는 표가 있으면 실패합니다.

```python
"""NN — one line saying what this shows.

Run:
    python NN_short_name.py           # scripted model, no key
    python NN_short_name.py --live    # hosted model; OPENAI_API_KEY in the environment

smoke-expect: provider: scripted
smoke-expect: final answer: 5
"""
```

예제를 오프라인으로 실행할 수 없으면 `smoke-expect` 줄 대신 `smoke-skip: <reason>`을 쓰세요. `smoke-requires: module1, module2`는 모듈이 없을 때 테스트를 건너뜁니다.
