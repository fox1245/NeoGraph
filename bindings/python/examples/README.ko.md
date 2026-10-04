<!-- neograph-i18n: source=bindings/python/examples/README.md locale=ko source_sha256=a1ffbe746f41909d860beac33ef1f3ea473e10ad6162985e4fba7b3810b4dd22 -->
# Python API 예제

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

이 28개 스크립트는 그래프 상태, 라우팅, 도구, 공급자 요청, 프로토콜 호스팅을 보여 줍니다. 오프라인 예제부터 시작하세요. 호스팅된 모델을 호출하려면 자격 증명을 명시적으로 제공해야 하며, 공급자 요금이 발생합니다.

## 설정

```bash
pip install neograph-engine
python 01_minimal.py
```

이 디렉터리에서 명령을 실행하세요. `python-dotenv`는 선택 사항입니다. `_common.py`가 예제 또는 저장소에서 가장 가까운 `.env`를 로드하게 하려면 설치하세요. 내보낸 환경 변수가 우선합니다. 호스팅 서비스의 자격 증명이 없으면 오류이며, 검증에 성공한 실행으로 간주하지 않습니다.

호스팅 예제에서는 `OPENAI_API_KEY`를 설정하고, 필요하면 `OPENAI_MODEL`도 설정하세요(기본값은 `gpt-4.1-mini`). `_common.py`는 닫힌 OpenAI Chat 또는 Responses 디스크립터를 검증하여 받아들인 뒤 `SchemaProvider`를 생성합니다. 타입 기반 SDK는 libcurl HTTP를 사용합니다. 제거된 공급자 클래스, 완료 매개변수, WebSocket 경로, 전송 백엔드 선택기는 사용할 수 없습니다.

키 없이 프로토콜을 검증하려면 `OPENAI_API_BASE`를 Chat/Responses 프로토콜을 충실히 구현한 루프백 피어로 설정하세요. 피어는 Python 모의 객체를 반환하는 대신 예제가 사용하는 경로와 응답 형식을 구현해야 합니다. 로컬 TLS CA에는 `NG_EXAMPLE_CA_FILE`을 사용하세요. 대신 `NG_PROVIDER_DESCRIPTOR`에 게이트웨이 접두사, 경로, 허용 헤더를 포함하는 완전한 디스크립터 JSON 파일을 지정할 수도 있습니다. 이 디스크립터도 검증을 통과해야 합니다. 기본 URL은 완전한 `/v1/chat/completions` 경로가 아닙니다. 타입 기반 요청/결과 계약은 [Python 바인딩 가이드](../../../docs/python-binding.md)를 참고하세요.

## 색인과 예상 동작

각 항목은 `python <file>`로 실행하세요. Linux x86_64 스모크 실행에서는 두 조사 앱을 포함한 일부 타입 기반 provider 애플리케이션을 자격 증명 없는 localhost 피어로 실행했습니다. 모든 스크립트, 호스팅 공급자 또는 릴리스 플랫폼의 검증을 뜻하지 않으며 원격 CI와 배포는 아직 남아 있습니다. 범위는 [현재 릴리스 근거](../../../CHANGELOG.md#unreleased)를 참조하세요. 표는 예상 동작을 설명하며 모델의 문구는 결정적이지 않습니다.

| # | 파일 | 사전 조건 | 예상 동작 |
|---|------|---------------|-------------------|
| 01 | [`01_minimal.py`](01_minimal.py) | 없음 | 사용자 정의 노드가 21을 두 배인 42로 만듭니다. |
| 02 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | 없음 | 스크립트로 구성한 도구 호출이 `tool_dispatch`를 거치며, 계산기가 `42`를 반환합니다. |
| 03 | [`03_send_fanout.py`](03_send_fanout.py) | 없음 | 8개의 `Send` 분기가 제곱값 `[0, 1, 4, 9, 16, 25, 36, 49]`를 병합합니다. 추가 순서는 가정하지 않습니다. |
| 04 | [`04_async_concurrent.py`](04_async_concurrent.py) | 없음 | 8개의 비동기 실행이 `[0, 2, 4, 6, 8, 10, 12, 14]`를 생성하며, 스트리밍 실행은 노드 이벤트를 내보냅니다. |
| 05 | [`05_openai_provider.py`](05_openai_provider.py) | Chat 피어 또는 호스팅 서비스 키 | `llm_call`이 `hello world`라고 말하는 assistant 메시지를 씁니다. |
| 06 | [`06_react_agent.py`](06_react_agent.py) | Responses 피어 또는 호스팅 서비스 키 | 모델이 요청한 `calc` 호출이 `4053`을 반환한 뒤 모델이 최종 답변을 제공합니다. |
| 07 | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | 없음 | 체크포인트가 결제 디스패치 전에 실행을 일시 중지합니다. 승인하면 모의 결제를 정확히 한 번 재개합니다. 실제 금액은 청구되지 않습니다. |
| 08 | [`08_intent_routing.py`](08_intent_routing.py) | Chat 피어 또는 호스팅 서비스 키 | 세 질문이 수학, 번역, 일반 전문가에게 라우팅되며, 각 전문가가 답변을 씁니다. |
| 09 | [`09_state_management.py`](09_state_management.py) | 없음 | Alpha의 count가 11에 도달하고 beta가 그 값을 분기합니다. 알 수 없는 스레드에는 체크포인트가 없습니다. |
| 10 | [`10_command_routing.py`](10_command_routing.py) | 없음 | 입력 200, 50, -10이 `Command`를 통해 accept, manual, reject 노드를 선택합니다. |
| 11 | [`11_reflexion.py`](11_reflexion.py) | Chat 피어 또는 호스팅 서비스 키 | Actor/critic 호출이 성찰 내용을 다음 시도로 전달하며, `ok` 또는 슈퍼스텝 상한에 도달하면 중단합니다. 범위를 제한한 교육용 변형입니다. |
| 12 | [`12_self_ask.py`](12_self_ask.py) | Chat 피어 또는 호스팅 서비스 키 | 최종 답변 전에 중간 질문과 답변이 스크래치패드에 누적됩니다. 검색 서비스는 연결되어 있지 않습니다. |
| 13 | [`13_multi_agent_debate.py`](13_multi_agent_debate.py) | Chat 피어 또는 호스팅 서비스 키 | 두 `Send` 분기가 서로 반대되는 주장을 생성하고, 한 심판이 병합된 주장을 읽습니다. |
| 14 | [`14_graph_to_json.py`](14_graph_to_json.py) | 없음 | 두 배로 계산한 결과 42와 저장된 `my_graph.json` 정의를 얻습니다. |
| 15 | [`15_graph_from_json.py`](15_graph_from_json.py) | 14번을 먼저 실행 | 저장된 정의가 5를 10으로, 100을 200으로 만듭니다. 사용자 정의 노드 타입은 별도로 등록합니다. |
| 16 | [`16_deep_research_chat.py`](16_deep_research_chat.py) | Responses 피어/키; `gradio` | 일반 채팅 또는 세 질문으로 구성된 조사 보고서를 제공합니다. 조사자는 웹 검색 대신 모델 지식을 사용합니다. |
| 17 | [`17_deep_research_crawl4ai.py`](17_deep_research_crawl4ai.py) | Responses 피어/키; `gradio`, `requests`; 선택적으로 Crawl4AI/Postgres | `CRAWL4AI_URL`은 실제 `/md` 검색을 활성화하며, `NEOGRAPH_PG_DSN`은 엔진 체크포인트의 영구 저장을 활성화합니다. 설정된 서비스의 실패를 다른 동작으로 조용히 대체하지 않습니다. 두 설정이 모두 없으면 스크립트가 모델만 사용하는 조사와 메모리 내 상태임을 명시적으로 알립니다. Gradio 기록은 자동 복원되지 않습니다. |
| 18 | [`18_node_cache.py`](18_node_cache.py) | Chat 피어 또는 호스팅 서비스 키 | 두 주제로 다섯 번 실행하면서 공급자를 사용하는 노드는 두 번 실행합니다. 반복 입력은 캐시된 쓰기를 재생합니다. 고정 지연 시간은 보장하지 않습니다. |
| 19 | [`19_streaming_messages.py`](19_streaming_messages.py) | 없음 | 스크립트로 구성한 다섯 토큰 이벤트가 `Octopuses have three hearts.`와 메시지 스트림 청크를 만듭니다. |
| 20 | [`20_otel_tracing.py`](20_otel_tracing.py) | `opentelemetry-api`, `opentelemetry-sdk` | 콘솔 스팬이 실행과 세 노드를 포함하며, 최종 trail은 `['A', 'B', 'C']`입니다. |
| 21 | [`21_http2_transport.py`](21_http2_transport.py) | TLS Chat 피어/키; libcurl HTTP/2 지원 | 동일한 SDK를 통해 HTTP/1.1과 HTTP/2 요청을 비교합니다. 협상된 프로토콜은 별도로 확인하세요. 소요 시간은 엔드포인트에 따라 달라집니다. |
| 22 | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | Chat 피어 또는 호스팅 서비스 키 | 프로필 JSON을 평가하고, 실패하면 수정된 그래프를 요청하여 다시 컴파일하고 재시도합니다. 등록된 노드 타입만 허용합니다. 모델은 성공하지 못한 채 반복 상한에 도달할 수 있습니다. |
| 23 | [`23_evolving_chat_agent.py`](23_evolving_chat_agent.py) | Chat 피어 또는 호스팅 서비스 키 | 허용된 그래프 재작성 과정에서 대화 체크포인트를 보존하고, 버전/해시를 `__graph_meta__`에 기록합니다. 이 메타데이터는 애플리케이션 수준이며, 권위 있는 재생 증거가 아닙니다. |
| 24 | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) | 없음 | 두 형제 도구 중 어느 것도 실행되기 전에 일시 중지합니다. 거부하면 `list_files`만 실행하고, 승인하면 둘 다 한 번씩 실행합니다. 셸 동작은 모의 실행입니다. |
| 25 | [`25_async_tools.py`](25_async_tools.py) | 없음 | 직렬 `Tool`과 I/O 작업이 겹쳐 실행되는 `AsyncTool`을 비교한 뒤 CPython GIL의 경계를 보여 줍니다. 시간 비율은 관측값이며 통과 기준이 아닙니다. |
| 26 | [`26_mcp_tools.py`](26_mcp_tools.py) | MCP를 활성화한 빌드 | 실제 로컬 JSON-RPC MCP 피어를 시작하고, `fetch`를 발견하여 명시적인 재진입 정책으로 세 번 호출합니다. 외부 네트워크나 키는 필요하지 않습니다. |
| 27 | [`27_a2a_server.py`](27_a2a_server.py) | Python 3.10+; `neograph-engine[a2a]` | `127.0.0.1:9999`에서 에이전트 카드와 A2A JSON-RPC를 제공하며, 스트리밍 아티팩트가 `NeoGraph received: hello (turn 1)`을 만듭니다. |
| 28 | [`28_acp_agent.py`](28_acp_agent.py) | Python 3.10+; `neograph-engine[acp]` | stdio를 통해 ACP를 제공합니다. 초기화하고 세션을 만든 뒤 프롬프트를 보내면 토큰 업데이트와 `end_turn`을 받습니다. 영구 세션의 `session/load`에는 설정된 Postgres 또는 SQLite 백엔드가 필요합니다. |

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
