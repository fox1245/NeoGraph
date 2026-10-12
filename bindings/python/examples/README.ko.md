<!-- neograph-i18n: source=bindings/python/examples/README.md locale=ko source_sha256=5ec25b467bef853ac9df52be725f8c16b6c63aef9ab5f6f5b64c974749579bee -->
# Python API 예제

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

이 스크립트 28개는 그래프 상태, 라우팅, 도구, provider 요청, 프로토콜 호스팅을 보여 줍니다. 오프라인 예제부터 시작하는 편이 좋습니다. 호스팅된 모델을 호출하려면 자격 증명을 명시적으로 지정해야 하며 provider 요금이 발생합니다.

## 설정

```bash
pip install neograph-engine
python 01_minimal.py
```

명령은 이 디렉터리에서 실행합니다. `python-dotenv`는 선택 사항입니다. `_common.py`가 가장 가까운 예제/저장소의 `.env`를 읽게 하려면 설치합니다. 이미 export한 환경 변수가 우선합니다. 호스팅 서비스의 자격 증명이 없으면 오류이며, 검증에 성공한 것으로 처리되지 않습니다.

소스 빌드로 실행하려면 `-DNEOGRAPH_BUILD_PYBIND=ON`으로 구성하고 `PYTHONPATH`를 빌드 디렉터리로 지정합니다. 빌드 트리는 패키지의 Python 의존성을 설치하지 않으므로 먼저 `pip install 'pydantic>=2' certifi`를 실행해야 합니다. `import neograph_engine`에는 pydantic이 필요하고, certifi가 없으면 TLS가 호스트의 신뢰 저장소를 사용합니다.

호스팅 서비스를 쓰는 예제에는 `OPENAI_API_KEY`를 설정하고, 필요하면 `OPENAI_MODEL`도 설정합니다(기본값 `gpt-4.1-mini`). `_common.py`는 닫힌 OpenAI Chat 또는 Responses descriptor를 승인(admission)한 다음 `SchemaProvider`를 만듭니다. 타입이 있는 SDK는 libcurl HTTP를 사용하며, 제거된 provider 클래스, completion 파라미터, WebSocket 경로, 전송 백엔드 선택자는 쓸 수 없습니다.

키 없이 프로토콜을 검증하려면 `OPENAI_API_BASE`를 실제와 같은 동작을 하는 루프백 Chat/Responses peer로 지정합니다. 이 peer는 예제가 사용하는 라우트와 응답 형태를 실제로 구현해야 하며, Python 목(mock)으로 응답을 흉내 내서는 안 됩니다. 로컬 TLS CA에는 `NG_EXAMPLE_CA_FILE`을 씁니다. 대신 `NG_PROVIDER_DESCRIPTOR`에 승인(admission)을 마친 완전한 descriptor JSON 파일(게이트웨이 접두사, 라우트, 허용 헤더 포함)을 지정할 수도 있습니다. 기본 URL은 전체 `/v1/chat/completions` 라우트가 아닙니다. 타입이 있는 요청/결과 계약은 [Python 바인딩 가이드](../../../docs/python-binding.md)를 참고하세요.

## 목록과 기대 동작

각 항목은 `python <file>`로 실행합니다. Linux x86_64 스모크 실행에서는 자격 증명이 필요 없는 localhost peer를 상대로, 두 리서치 앱을 포함해 타입이 있는 provider를 쓰는 일부 애플리케이션을 실행해 보았습니다. 이것이 모든 스크립트, 호스팅 벤더, 릴리스 플랫폼에 대한 자격 검증은 아닙니다. 원격 CI와 게시는 아직 남아 있습니다. 범위는 [현재 릴리스 증거](../../../CHANGELOG.md#unreleased)를 참고하세요. 표에는 기대 동작이 적혀 있을 뿐, 모델이 내는 문구가 결정적으로 같다는 뜻은 아닙니다.

| # | 파일 | 사전 조건 | 기대 동작 |
|---|------|-----------|-----------|
| 01 | [`01_minimal.py`](01_minimal.py) | 없음 | 사용자 정의 노드가 21을 두 배로 만들어 42를 출력한다. |
| 02 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | 없음 | 미리 정해 둔 도구 호출이 `tool_dispatch`를 거치고, 계산기가 `42`를 반환한다. |
| 03 | [`03_send_fanout.py`](03_send_fanout.py) | 없음 | `Send` 분기 8개가 제곱 `[0, 1, 4, 9, 16, 25, 36, 49]`를 합친다. append 순서는 가정하지 않는다. |
| 04 | [`04_async_concurrent.py`](04_async_concurrent.py) | 없음 | 비동기 실행 8개가 `[0, 2, 4, 6, 8, 10, 12, 14]`를 만들고, 스트리밍 실행은 노드 이벤트를 내보낸다. |
| 05 | [`05_openai_provider.py`](05_openai_provider.py) | Chat peer 또는 호스팅 키 | `llm_call`이 `hello world`라고 말하는 assistant 메시지를 쓴다. |
| 06 | [`06_react_agent.py`](06_react_agent.py) | Responses peer 또는 호스팅 키 | 모델이 낸 `calc` 호출이 `4053`을 반환하고, 모델이 최종 답을 낸다. |
| 07 | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | 없음 | 체크포인트가 결제 디스패치 전에 일시 중지한다. 승인하면 모의 결제가 정확히 한 건만 실행된다. 실제로 청구되는 금액은 없다. |
| 08 | [`08_intent_routing.py`](08_intent_routing.py) | Chat peer 또는 호스팅 키 | 질문 세 개가 수학, 번역, 일반 전문가로 각각 라우팅되고, 각 전문가가 답을 쓴다. |
| 09 | [`09_state_management.py`](09_state_management.py) | 없음 | alpha는 카운트 11에 도달하고, beta는 그 값을 fork하며, 알 수 없는 스레드에는 체크포인트가 없다. |
| 10 | [`10_command_routing.py`](10_command_routing.py) | 없음 | 입력 200, 50, -10이 `Command`로 각각 accept, manual, reject 노드를 선택한다. |
| 11 | [`11_reflexion.py`](11_reflexion.py) | Chat peer 또는 호스팅 키 | actor/critic 호출이 성찰 내용을 다음 시도로 넘기며, `ok`가 나오거나 슈퍼스텝 한도에 도달하면 멈춘다. 한도를 둔 교육용 변형이다. |
| 12 | [`12_self_ask.py`](12_self_ask.py) | Chat peer 또는 호스팅 키 | 중간 질문과 답이 scratchpad에 쌓인 뒤 최종 답이 나온다. 검색 서비스는 연결하지 않는다. |
| 13 | [`13_multi_agent_debate.py`](13_multi_agent_debate.py) | Chat peer 또는 호스팅 키 | `Send` 분기 두 개가 서로 반대되는 논거를 만들고, 심판 하나가 합쳐진 논거를 읽는다. |
| 14 | [`14_graph_to_json.py`](14_graph_to_json.py) | 없음 | 두 배가 된 결과 42와, 저장된 `my_graph.json` 정의가 나온다. |
| 15 | [`15_graph_from_json.py`](15_graph_from_json.py) | 14번을 먼저 실행 | 저장된 정의가 5를 10으로, 100을 200으로 두 배로 만든다. 사용자 정의 노드 타입은 따로 등록한다. |
| 16 | [`16_deep_research_chat.py`](16_deep_research_chat.py) | Responses peer/키, `gradio` | 일반 채팅이나 질문 세 개로 구성된 리서치 보고서가 나온다. 리서처는 웹 검색이 아니라 모델의 지식을 쓴다. |
| 17 | [`17_deep_research_crawl4ai.py`](17_deep_research_crawl4ai.py) | Responses peer/키, `gradio`, `requests`, 선택적으로 Crawl4AI/Postgres | `CRAWL4AI_URL`을 설정하면 실제 `/md` 검색을 쓰고, `NEOGRAPH_PG_DSN`을 설정하면 엔진 체크포인트를 영구 저장한다. 설정한 서비스가 실패하면 조용히 다른 방식으로 대체하지 않는다. 둘 다 설정하지 않으면 스크립트가 모델만 쓰는 리서치와 인메모리 상태로 동작한다고 명시적으로 알려 준다. Gradio 기록은 자동으로 복원되지 않는다. |
| 18 | [`18_node_cache.py`](18_node_cache.py) | Chat peer 또는 호스팅 키 | 주제 두 개에 걸친 실행 다섯 번에서 provider를 쓰는 노드가 두 번만 실행되고, 입력이 반복되면 캐시된 쓰기를 재생(replay)한다. 고정된 지연 시간은 약속하지 않는다. |
| 19 | [`19_streaming_messages.py`](19_streaming_messages.py) | 없음 | 미리 정해 둔 토큰 이벤트 다섯 개가 `Octopuses have three hearts.`와 메시지 스트림 청크를 이룬다. |
| 20 | [`20_otel_tracing.py`](20_otel_tracing.py) | `opentelemetry-api`, `opentelemetry-sdk` | 콘솔 span이 실행과 노드 세 개를 다루고, 최종 trail은 `['A', 'B', 'C']`다. |
| 21 | [`21_http2_transport.py`](21_http2_transport.py) | TLS Chat peer/키, libcurl HTTP/2 지원 | 같은 SDK로 HTTP/1.1 요청과 HTTP/2 요청을 비교한다. 협상된 프로토콜은 따로 확인해야 하며, 시간은 엔드포인트에 따라 다르다. |
| 22 | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | Chat peer 또는 호스팅 키 | 프로필 JSON에 점수를 매기고, 실패하면 수정된 그래프를 요청해 다시 컴파일하고 재시도한다. 등록된 노드 타입만 승인(admission)된다. 모델이 성공하지 못한 채 반복 한도를 모두 쓸 수도 있다. |
| 23 | [`23_evolving_chat_agent.py`](23_evolving_chat_agent.py) | Chat peer 또는 호스팅 키 | 승인(admission)된 그래프 재작성 전후로 대화 체크포인트를 보존하고, `__graph_meta__`에 버전/해시를 기록한다. 이 메타데이터는 애플리케이션 수준의 값이며 재생(replay)의 권위 있는 증거가 아니다. |
| 24 | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) | 없음 | 형제 도구 두 개 중 어느 쪽이 실행되기 전에 일시 중지한다. 거부하면 `list_files`만 실행되고, 승인하면 둘 다 한 번씩 실행된다. 셸 동작은 모의로 처리한다. |
| 25 | [`25_async_tools.py`](25_async_tools.py) | 없음 | 직렬로 실행되는 `Tool`과 I/O 위주 작업이 겹쳐 실행되는 `AsyncTool`을 비교한 뒤, CPython GIL의 경계를 보여 준다. 시간 비율은 관찰 값이며 통과 기준이 아니다. |
| 26 | [`26_mcp_tools.py`](26_mcp_tools.py) | MCP를 켠 빌드 | 실제 로컬 JSON-RPC MCP peer를 시작하고 `fetch`를 찾아서, 명시적인 재진입 허용 정책으로 호출을 세 번 실행한다. 외부 네트워크나 키가 필요 없다. |
| 27 | [`27_a2a_server.py`](27_a2a_server.py) | Python 3.10 이상, `neograph-engine[a2a]` | `127.0.0.1:9999`에서 agent card와 A2A JSON-RPC를 제공하고, 스트리밍된 artifact가 `NeoGraph received: hello (turn 1)`을 이룬다. |
| 28 | [`28_acp_agent.py`](28_acp_agent.py) | Python 3.10 이상, `neograph-engine[acp]` | stdio로 ACP를 제공한다. initialize, 세션 생성, 프롬프트 전송을 거쳐 토큰 업데이트와 `end_turn`을 받는다. 영구 `session/load`에는 Postgres나 SQLite 백엔드 설정이 필요하다. |

## 상태와 스케줄링

노드는 현재 슈퍼스텝의 상태를 읽고 채널 쓰기를 반환합니다. 엔진은 다음 슈퍼스텝 전에 선언된 리듀서로 그 쓰기를 합칩니다. `Send`는 분기별 입력을 전달하고, `Command`는 상태를 갱신하면서 다음 노드를 고릅니다. 워커 풀을 써도 CPU 위주의 Python 콜백이 CPython GIL 아래에서 병렬로 실행되지는 않습니다.

`GraphEngine.compile()`은 Python으로 작성한 딕셔너리뿐 아니라 JSON에서 읽은 딕셔너리도 받습니다. 정의에는 실행 가능한 Python 클래스가 아니라 연결 구조만 저장되므로, 저장된 정의를 컴파일하기 전에 사용자 정의 노드 팩토리를 등록해야 합니다. 예제 14와 15는 스크립트 옆에 `my_graph.json`을 쓰고 읽습니다. 이 한 쌍을 검증할 때는 일회용 체크아웃이나 이 디렉터리의 복사본을 사용합니다.

## 대화형 시나리오와 프로토콜 시나리오

16번과 17번은 `gradio`를 설치하고 스크립트를 실행한 뒤 출력되는 로컬 URL에 접속합니다. `hello`를 입력한 다음 `research apples`를 입력합니다. 17번에서는 실제 Crawl4AI `/md` 서비스나 실제와 같은 동작을 하는 로컬 peer가 `{"success": true, "markdown": "..."}`를 반환해야 합니다. Postgres는 실행 중인 빌드가 지원하고 데이터베이스에 접속할 수 있을 때만 설정합니다. 엔진 상태는 UI 기록과 별도로 확인합니다.

27번과 28번은 와이어 처리에 공식 Python SDK를, 체크포인트를 인식하는 엔진 실행에 `ProtocolHostAdapter`를 사용합니다. A2A를 검증하려면 SDK 클라이언트가 agent card를 가져오고, 메시지를 보내고, 스트림을 소비해야 합니다. ACP를 검증하려면 클라이언트가 하위 프로세스로 연결해 `initialize`, `session/new`, `session/prompt`를 호출해야 하며, stdout은 프로토콜 메시지 전용입니다. 같은 컨텍스트/세션에서 두 번째 프롬프트를 보내면 `(turn 2)`가 나와야 합니다. 취소하면 진행 중인 요청이 멈춰야 하며, 성공처럼 보이는 대체 결과를 만들어서는 안 됩니다.

ACP 세션을 영구 저장하려면 `NEOGRAPH_ACP_POSTGRES_URL`과 `NEOGRAPH_ACP_SQLITE_PATH` 중 정확히 하나만 설정합니다. 첫 프롬프트가 끝나면 `session/load`에 필요한 체크포인트가 만들어집니다. 세션마다 활성 에이전트 프로세스는 하나만 유지해야 합니다. 체크포인트 저장소는 여러 프로세스의 동시 쓰기를 직렬화하지 않기 때문입니다. 이 예제는 리치 콘텐츠 블록을 그대로 돌려줄 뿐 이미지/오디오 콘텐츠를 해석하지 않으며, 에디터의 파일시스템/터미널 콜백도 제공하지 않습니다.

## 배포 이름과 import 이름

```python
import neograph_engine as ng
from neograph_engine.llm import SchemaProvider
```

배포 패키지 이름은 `neograph-engine`입니다. PyPI의 `neograph`는 이 프로젝트와 무관한 다른 프로젝트입니다.
