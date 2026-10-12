<!-- neograph-i18n: source=bindings/python/examples/README.md locale=ko source_sha256=a56e3bfe852d264dcadce3f7ea680e0c8b6f8dca978c16eb8d21270ed2254900 -->
# Python API 예제

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

이 스크립트 28개는 그래프 상태, 라우팅, 도구, 공급자 요청, 프로토콜 호스팅을 보여 줍니다. 스크립트마다 한 가지를 보여 줍니다. 대부분은 오프라인으로 실행됩니다. 스크립트된 모델이 공급자 역할을 하므로 API 키도 네트워크도 필요하지 않습니다. 실제 모델도 호출할 수 있는 예제는 `--live`를 받습니다.

**Offline** 열은 각 행이 필요로 하는 것을 알려 줍니다. `yes`: 키와 네트워크 없이 실행되며 `bindings/python/tests/test_examples_offline.py`가 자동으로 테스트합니다. `key`: 공급자 API 키가 필요합니다. `service`: 다른 프로세스, 서버 또는 추가 패키지가 필요합니다. `paired`: 행에 적힌 예제와 함께 오프라인으로 실행됩니다.

## 설정

```bash
pip install neograph-engine
python 01_minimal.py
```

이 디렉터리에서 명령을 실행하세요. 오프라인 예제에는 그 밖에 필요한 것이 없습니다. wheel 대신 소스 빌드로 실행하려면 `-DNEOGRAPH_BUILD_PYBIND=ON`으로 구성하고 `PYTHONPATH`를 빌드 디렉터리와 `bindings/python`으로 설정하세요. 빌드 트리는 패키지의 Python 의존성을 설치하지 않으므로 먼저 `pip install 'pydantic>=2' certifi`를 실행하세요. `import neograph_engine`에는 pydantic이 필요하며, certifi가 없으면 TLS는 호스트의 신뢰 저장소를 사용합니다.

## 여기서 시작하세요

순서대로 읽으세요. 하나를 읽을 때마다 아이디어가 하나씩 더해집니다.

| 단계 | 예제 | Offline | 배우는 것 |
|---|---|---|---|
| 1 | [`01_minimal.py`](01_minimal.py) | yes | 사용자 정의 노드가 채널을 읽어 두 배로 만들고 다시 씁니다. 21이 42가 됩니다. |
| 2 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | yes | Python `Tool`과 내장 `tool_dispatch` 노드: 스크립트로 정한 도구 호출이 `42`로 돌아옵니다. |
| 3 | [`05_openai_provider.py`](05_openai_provider.py) | yes | 내장 `llm_call` 노드를 통한 모델 호출 한 번. 스크립트 모델이 `hello world`라고 답합니다. |
| 4 | [`06_react_agent.py`](06_react_agent.py) | yes | ReAct 루프: `llm_call`, `tool_dispatch`, `has_tool_calls` 조건부 엣지. `calc`가 `4053`을 반환하고 모델이 답합니다. |
| 5 | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | yes | 체크포인트가 결제 디스패치 전에 멈추고, 승인하면 모의 결제가 정확히 한 번 재개됩니다. 실제 돈은 움직이지 않습니다. |
| 6 | [`14_graph_to_json.py`](14_graph_to_json.py) | yes | 그래프 정의는 평범한 JSON입니다. dict로 실행한 뒤 `my_graph.json`으로 저장합니다. 예제 15가 이를 불러옵니다. |

## 하고 싶은 일로 찾기

목표를 고르고 해당 예제를 읽으세요.

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
| 승인을 기다리며 멈췄다가 재개하기 | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | yes |
| 도구 호출을 허용, 거부, 중단하기 | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) | yes |
| 저장된 상태를 읽고 포크하기 | [`09_state_management.py`](09_state_management.py) | yes |
| 그래프를 JSON으로 저장하고 불러오기 | [`14_graph_to_json.py`](14_graph_to_json.py) | yes |
| 토큰을 LangChain 형태의 청크로 스트리밍하기 | [`19_streaming_messages.py`](19_streaming_messages.py) | yes |
| OpenTelemetry로 실행 추적하기 | [`20_otel_tracing.py`](20_otel_tracing.py) | yes |
| I/O 중심 도구를 동시에 실행하기 | [`25_async_tools.py`](25_async_tools.py) | yes |
| MCP 서버의 도구 사용하기 | [`26_mcp_tools.py`](26_mcp_tools.py) | yes |
| 내 에이전트를 A2A로 제공하기 | [`27_a2a_server.py`](27_a2a_server.py) | service |
| 내 에이전트를 ACP로 제공하기 | [`28_acp_agent.py`](28_acp_agent.py) | service |
| Reflexion, Self-Ask, 토론 시험하기 | [`11_reflexion.py`](11_reflexion.py) | key |
| 에이전트가 자기 그래프를 고쳐 쓰게 하기 | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | key |

## 단계별 색인

### 시작하기와 Core 엔진

모델이 필요 없는 나머지 Core 기능입니다. 예제 14는 스크립트 옆에 `my_graph.json`을 쓰고 15가 그것을 읽으므로 이 순서대로 실행하세요.

| # | 예제 | Offline | 보여 주는 것 |
|---|---|---|---|
| 03 | [`03_send_fanout.py`](03_send_fanout.py) | yes | `Send` 분기 여덟 개가 제곱값 `[0, 1, 4, 9, 16, 25, 36, 49]`를 합칩니다. append 순서는 가정하지 않습니다. |
| 04 | [`04_async_concurrent.py`](04_async_concurrent.py) | yes | `asyncio.gather`로 비동기 실행 여덟 개를 돌리면 `[0, 2, 4, 6, 8, 10, 12, 14]`가 나오고, 스트리밍 실행은 노드 이벤트를 냅니다. |
| 10 | [`10_command_routing.py`](10_command_routing.py) | yes | `Command`가 다음 노드를 고르고 상태를 씁니다. 입력 200, 50, -10이 각각 accept, manual, reject로 갑니다. |
| 14 | [`14_graph_to_json.py`](14_graph_to_json.py) | yes | 그래프 정의는 평범한 JSON입니다. dict로 실행한 뒤 `my_graph.json`으로 저장합니다. |
| 15 | [`15_graph_from_json.py`](15_graph_from_json.py) | yes | 예제 14의 `my_graph.json`을 불러와 5를 10으로, 100을 200으로 두 배로 만듭니다. 14를 먼저 실행하세요. 사용자 정의 노드 타입은 코드에서 등록합니다. |
| 19 | [`19_streaming_messages.py`](19_streaming_messages.py) | yes | `message_stream`이 토큰 이벤트를 LangChain 형태의 청크로 바꿉니다. 스크립트로 정한 토큰 다섯 개가 `Octopuses have three hearts.`가 됩니다. |

### 에이전트 패턴

그래프 안의 모델(도구가 있거나 없거나)입니다. `key` 행은 호스팅 모델을 호출합니다.

| # | 예제 | Offline | 보여 주는 것 |
|---|---|---|---|
| 05 | [`05_openai_provider.py`](05_openai_provider.py) | yes | `llm_call`을 통한 모델 호출 한 번. 스크립트 모델이 `hello world`라고 답하고, `--live`는 호스팅 모델을 호출합니다. |
| 06 | [`06_react_agent.py`](06_react_agent.py) | yes | Responses 프로토콜 위의 ReAct: 모델이 낸 `calc` 호출이 `4053`을 반환하고 모델이 답합니다. |
| 08 | [`08_intent_routing.py`](08_intent_routing.py) | yes | 질문 세 개가 `route_channel`을 거쳐 math, translate, general 전문가로 가고, 각 전문가가 답을 씁니다. |
| 11 | [`11_reflexion.py`](11_reflexion.py) | key | Reflexion: actor와 critic 호출이 반성을 다음 시도로 넘깁니다. 범위를 제한한 교육용 버전입니다. |
| 12 | [`12_self_ask.py`](12_self_ask.py) | key | Self-Ask: 중간 질문과 답이 스크래치패드에 쌓인 뒤 최종 답이 나옵니다. 검색 서비스는 연결되어 있지 않습니다. |
| 13 | [`13_multi_agent_debate.py`](13_multi_agent_debate.py) | key | `Send` 분기 두 개가 반대 입장을 주장하고, 심판 하나가 합쳐진 논거를 읽습니다. |
| 16 | [`16_deep_research_chat.py`](16_deep_research_chat.py) | key | 연구 모드가 있는 Gradio 채팅: 웹 검색이 아니라 모델 지식을 쓰는 병렬 researcher가 질문 세 개짜리 보고서를 만듭니다. `gradio`가 필요합니다. |
| 17 | [`17_deep_research_crawl4ai.py`](17_deep_research_crawl4ai.py) | key | 같은 연구 그래프에 Crawl4AI를 통한 실제 웹 검색과 선택적 PostgreSQL 체크포인트를 더했습니다. `gradio`, `requests`, Crawl4AI 서버가 필요합니다. |
| 22 | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | key | 그래프가 자기 출력을 채점하고, 실패하면 모델에게 수정된 정의를 요청해 다시 컴파일하고 재시도합니다. 등록된 노드 타입만 허용됩니다. |
| 23 | [`23_evolving_chat_agent.py`](23_evolving_chat_agent.py) | key | 허용된 그래프 재작성 뒤에도 대화 체크포인트가 유지됩니다. 버전과 해시는 `__graph_meta__`에 기록됩니다(애플리케이션 수준 메타데이터이며 replay 증거가 아닙니다). |

### Program API

제한된 JavaScript Program API는 Core 그래프 위에서 동작합니다: `ProgramRegistryBuilder`, `ProgramCompiler`, `LocalProgramHost`. [Program and QuickJS](../../../docs/python-binding.md#program-and-quickjs)에 설명되어 있고, [`bindings/python/tests/program_e2e/`](../tests/program_e2e/README.md)의 엔드투엔드 시나리오가 join, 실패 라우팅, 예산, 일시 중지와 재개, fork, 충돌 복구를 다룹니다.

### 프로토콜: MCP, A2A, ACP

여기서 MCP는 클라이언트(`ng.mcp`)입니다. A2A와 ACP는 공식 Python SDK와 `ProtocolHostAdapter`로 호스팅하며, 중단할 때까지 실행되므로 스모크 검증은 `bindings/python/tests/test_protocol_sdk_e2e.py`가 맡습니다.

| # | 예제 | Offline | 보여 주는 것 |
|---|---|---|---|
| 26 | [`26_mcp_tools.py`](26_mcp_tools.py) | yes | 로컬 MCP 서버를 띄워 `fetch`를 찾고 명시적 재진입 정책으로 호출 세 번을 실행합니다. MCP가 켜진 빌드가 필요하며 스모크 테스트는 Linux에서 합니다. |
| 27 | [`27_a2a_server.py`](27_a2a_server.py) | service | 공식 A2A SDK로 에이전트를 호스팅합니다. `127.0.0.1:9999`의 에이전트 카드와 JSON-RPC, 스트리밍 아티팩트. `neograph-engine[a2a]`가 필요합니다. |
| 28 | [`28_acp_agent.py`](28_acp_agent.py) | service | 공식 ACP SDK로 stdio 위에서 에이전트를 호스팅합니다. initialize, 세션 생성, prompt, 토큰 업데이트, `end_turn`. `neograph-engine[acp]`가 필요합니다. |

### 영속성과 운영

체크포인트, 상태, 캐시, 트레이싱, 전송, 도구 동시성.

| # | 예제 | Offline | 보여 주는 것 |
|---|---|---|---|
| 07 | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | yes | 체크포인트가 결제 디스패치 전에 멈추고, 승인하면 모의 결제가 정확히 한 번 재개됩니다. |
| 09 | [`09_state_management.py`](09_state_management.py) | yes | `get_state`와 `fork`: alpha가 count 11에 이르고, beta가 그 값을 포크하며, 알 수 없는 스레드에는 체크포인트가 없습니다. |
| 18 | [`18_node_cache.py`](18_node_cache.py) | key | 주제 두 개에 걸친 다섯 번의 실행에서 공급자 기반 노드는 두 번만 실행되고, 반복된 입력은 캐시된 쓰기를 재생합니다. |
| 20 | [`20_otel_tracing.py`](20_otel_tracing.py) | yes | 콘솔에 실행과 노드 세 개의 OpenTelemetry span이 출력되고, 최종 경로는 `['A', 'B', 'C']`입니다. `opentelemetry-api`와 `opentelemetry-sdk`가 필요합니다. |
| 21 | [`21_http2_transport.py`](21_http2_transport.py) | key | 같은 SDK로 HTTP/1.1과 HTTP/2를 비교합니다. TLS Chat 엔드포인트와 HTTP/2를 지원하는 libcurl이 필요합니다. 결과는 이 엔드포인트에 대한 것이며 순위가 아닙니다. |
| 24 | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) | yes | 도구 게이트가 형제 도구 어느 쪽이든 실행되기 전에 멈춥니다. 거부하면 `list_files`만, 승인하면 둘 다 한 번씩 실행됩니다. 셸 동작은 모의입니다. |
| 25 | [`25_async_tools.py`](25_async_tools.py) | yes | I/O 중심 작업에서 동기 `Tool`과 겹쳐 실행되는 `AsyncTool`을 비교하고, CPU 중심 작업에서는 GIL 경계를 보여 줍니다. 시간 비율은 관찰값이며 스모크 테스트는 Linux에서 합니다. |

## 오프라인, 스크립트, 라이브

모델로 구동되는 예제는 `_common.py`의 `example_provider(script)`로 공급자를 만듭니다. `--live`가 없으면 `_scripted.ScriptedModel`이 응답합니다. 이것은 `127.0.0.1`에서 동작하는 작은 서버로, 스크립트에 따라 OpenAI Chat 또는 Responses 프로토콜로 응답합니다(텍스트, 도구 호출, 스트리밍, 사용량, HTTP 실패). 실제 `SchemaProvider`의 요청 구성, SSE 파싱, outcome 처리가 이 서버를 상대로 실행됩니다. 모델이 아니므로 무엇을 말할지는 스크립트가 정합니다.

```python
from _common import example_provider
from _scripted import call_tool, say

script = [call_tool("calc", expression="2 + 3"), say("5")]   # call 1 asks for a tool, call 2 answers
with example_provider(script) as chosen:
    provider = chosen.provider        # a SchemaProvider; chosen.scripted is None with --live
```

스크립트는 `turn.last_user_text`, `turn.system_text`, `turn.last_tool_result`를 읽는 함수 `(turn) -> Reply`일 수도 있습니다. `chosen.scripted.call_count`로 모델 호출이 몇 번 있었는지 단언할 수 있습니다. 규칙은 `_scripted.py`의 docstring에 있습니다.

`--live`, `NG_EXAMPLE_LIVE=1`, `OPENAI_API_BASE`, `NG_PROVIDER_DESCRIPTOR`는 대신 설정된 엔드포인트를 선택합니다. 호스팅 엔드포인트에는 `OPENAI_API_KEY`가 필요하고 선택적으로 `OPENAI_MODEL`(기본값 `gpt-4.1-mini`)을 쓰며, 둘 다 프로세스 환경에서만 읽습니다. `_common.py`는 `.env` 파일을 읽지 않으며, 키가 없으면 성공한 실행이 아니라 오류입니다. `_common.py`는 닫힌 OpenAI Chat 또는 Responses 디스크립터를 승인한 뒤 `SchemaProvider`를 만듭니다. 타입 SDK는 libcurl HTTP를 쓰며, 제거된 공급자 클래스, completion 파라미터, WebSocket 경로, 전송 백엔드 선택기는 사용할 수 없습니다. `OPENAI_API_BASE`는 origin 또는 게이트웨이 접두사이며 전체 `/v1/chat/completions` 경로가 아닙니다. 시스템 신뢰 저장소에 CA가 없는 HTTPS 피어는 `NG_EXAMPLE_CA_FILE`도 필요합니다. 대신 `NG_PROVIDER_DESCRIPTOR`로 승인된 완전한 디스크립터 JSON 파일을 지정할 수 있습니다. 타입 요청과 outcome 계약은 [Python 바인딩 가이드](../../../docs/python-binding.md)를 참고하세요.

## 상태와 스케줄링

노드는 현재 슈퍼스텝의 상태를 읽고 채널 쓰기를 반환합니다. 엔진은 다음 슈퍼스텝 전에 선언된 리듀서를 통해 이 쓰기들을 병합합니다. `Send`는 분기별 입력을 제공하고, `Command`는 상태를 갱신하며 다음 노드를 선택합니다. 작업자 풀이 있어도 CPython GIL 아래에서 CPU 중심 Python 콜백이 병렬로 실행되지는 않습니다.

`GraphEngine.compile()`은 Python으로 작성한 딕셔너리뿐 아니라 JSON에서 로드한 딕셔너리도 받습니다. 정의에는 연결 구조가 저장되며 실행 가능한 Python 클래스는 저장되지 않습니다. 저장된 정의를 컴파일하기 전에 사용자 정의 노드 팩터리를 등록하세요. 예제 14와 15는 스크립트 옆에 `my_graph.json`을 쓰고 읽습니다. 두 예제를 검증할 때는 폐기 가능한 체크아웃이나 이 디렉터리의 복사본을 사용하세요.

## 대화형 및 프로토콜 시나리오

16/17번에는 `gradio`를 설치한 뒤 스크립트를 실행하고 출력된 로컬 URL을 방문하세요. `hello`를 제출한 다음 `research apples`를 제출하세요. 17번에서는 실제 Crawl4AI `/md` 서비스 또는 프로토콜을 충실히 구현한 로컬 피어가 `{"success": true, "markdown": "..."}`를 반환해야 합니다. 실행 중인 빌드가 Postgres를 지원하고 데이터베이스에 연결할 수 있을 때만 Postgres를 설정하세요. 엔진 상태는 UI 기록과 별도로 확인하세요.

예제 27/28은 통신 프로토콜 처리를 위해 공식 Python SDK를 사용하고, 체크포인트를 고려하는 엔진 실행을 위해 `ProtocolHostAdapter`를 사용합니다. A2A 검증에는 에이전트 카드를 가져오고 메시지를 보내며 스트림을 소비하는 SDK 클라이언트가 필요합니다. ACP 검증에는 `initialize`, `session/new`, `session/prompt`를 위한 클라이언트 서브프로세스 연결이 필요하며, stdout은 프로토콜 메시지 전용입니다. 동일한 컨텍스트/세션에서 두 번째 프롬프트를 보내고 `(turn 2)`를 기대하세요. 취소는 성공처럼 보이는 대체 결과를 만드는 대신 활성 요청을 중단해야 합니다.

영구 ACP 세션에는 `NEOGRAPH_ACP_POSTGRES_URL` 또는 `NEOGRAPH_ACP_SQLITE_PATH` 중 정확히 하나를 설정하세요. 첫 프롬프트가 완료되면 `session/load`에 필요한 체크포인트가 생성됩니다. 세션마다 활성 에이전트 프로세스를 하나만 유지하세요. 체크포인트 저장소는 프로세스 간 동시 쓰기를 직렬화하지 않습니다. 이 예제는 풍부한 콘텐츠 블록을 그대로 돌려주지만 이미지/오디오 콘텐츠를 해석하거나 편집기 파일시스템/터미널 콜백을 제공하지는 않습니다.

## 배포 이름과 import 이름

```python
import neograph_engine as ng
from neograph_engine.llm import SchemaProvider
```

배포 이름은 `neograph-engine`입니다. PyPI의 `neograph`는 관련 없는 프로젝트입니다.

## 예제 추가하기

번호는 재사용하지 않습니다. 다음으로 비어 있는 번호는 29입니다. 새 예제는 파일 하나와 표의 행 하나로 이루어집니다.

1. 파일 이름을 `NN_short_name.py`로 짓고 모듈 docstring으로 시작합니다. 몇 줄로 보여 주는 내용, `Run:` 명령, 그다음 테스트 주석을 적습니다.
2. 그 docstring에 테스트 방법을 적습니다. 오프라인 예제는 반드시 출력해야 하는 줄을 적고, 키나 서비스가 필요한 예제는 오프라인으로 실행할 수 없는 이유를 적습니다(주석 문법은 `examples/run_example_smoke.py` 맨 위에 있습니다).
3. 모델로 구동되는 공급자는 `example_provider(script)`로 만들어, 기본은 오프라인이고 `--live`를 받게 합니다.
4. 해당 단계의 표에 Offline 값과 함께 행을 추가합니다.
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

예제를 오프라인으로 실행할 수 없으면 `smoke-expect` 줄 대신 `smoke-skip: <이유>`를 쓰세요. `smoke-requires: module1, module2`는 모듈이 없을 때 테스트를 건너뜁니다.
