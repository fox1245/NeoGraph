<!-- neograph-i18n: source=bindings/python/examples/README.md locale=ko source_sha256=1c9ad12b9098111ceefe6e550a72390df5e35292924c7ad4bd899bac79e9519f -->
**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

# Python API 예제


바인딩 표면 전체를 덮는 28개의 스크립트.

## 설정

```bash
pip install neograph-engine python-dotenv
cp .env.example .env  # edit OPENAI_API_KEY for examples that hit a real LLM
```

`_common.py`는 이 디렉터리나 상위 디렉터리에서 `.env`를 자동 로드합니다.
API 키가 필요한 예는 다음과 같은 경우에 (충돌 없이) 깔끔하게 건너뜁니다.
열쇠가 없습니다.

## 색인

| # |파일|회로망|무늬|
|---|------|---------|---------|
| 01 |[`01_minimal.py`](01_minimal.py)|오프라인|`GraphNode` 서브클래스 + `engine.run()`. 가장 작은 유용한 그래프.|
| 02 |[`02_tool_dispatch.py`](02_tool_dispatch.py)|오프라인|`Tool` 하위 클래스 + 내장 `tool_dispatch`. 손으로 제작한 tool_call(실제 LLM 없음).|
| 03 |[`03_send_fanout.py`](03_send_fanout.py)|오프라인|`run(input)`는 `Send` 목록 + `set_worker_count(4)`를 사용하여 `NodeResult`를 반환합니다. 맵 축소.|
| 04 |[`04_async_concurrent.py`](04_async_concurrent.py)|오프라인|8개의 동시 실행의 `engine.run_async` + `asyncio.gather` + `run_stream_async`.|
| 05 |[`05_openai_provider.py`](05_openai_provider.py)|**오픈AI**|`OpenAIProvider` + 내장 `llm_call` 노드. 원샷완성.|
| 06 |[`06_react_agent.py`](06_react_agent.py)|**오픈AI**|ReAct 루프: `llm_call` ⇔ `tool_dispatch`(`has_tool_calls` 조건부 포함)|
| 07 |[`07_checkpoint_hitl.py`](07_checkpoint_hitl.py)|오프라인|모의 LLM 이미터를 사용하는 2단계 propose/approve 워크플로.|
| 08 |[`08_intent_routing.py`](08_intent_routing.py)|**오픈AI**|분류기 노드 + 조건부 에지 → 수학/번역/일반 전문가.|
| 09 |[`09_state_management.py`](09_state_management.py)|오프라인|`set_checkpoint_store(InMemoryCheckpointStore())` + `get_state` + `fork`.|
| 10 |[`10_command_routing.py`](10_command_routing.py)|오프라인|`run(input)`가 `Command(goto_node=…, updates=[…])`를 반환합니다.|
| 11 |[`11_reflexion.py`](11_reflexion.py)|**오픈AI**|성찰 프롬프트가 포함된 배우 + 비평가 루프(Shinn et al. 2023).|
| 12 |[`12_self_ask.py`](12_self_ask.py)|**오픈AI**|Self-Ask 후속 질문 분해(Press et al. 2022).|
| 13 |[`13_multi_agent_debate.py`](13_multi_agent_debate.py)|**오픈AI**|2인 토론자 + 심사위원. 토론자는 `Send`를 통해 팬 아웃됩니다.|
| 14 |[`14_graph_to_json.py`](14_graph_to_json.py)|오프라인|그래프 정의를 `.json` 파일로 직렬화합니다.|
| 15 |[`15_graph_from_json.py`](15_graph_from_json.py)|오프라인|`.json` 그래프를 로드하고 실행합니다(14와 함께).|
| 16 |[`16_deep_research_chat.py`](16_deep_research_chat.py)|**오픈AI WS**|`조사해줘 / research / investigate`에서 병렬 심층 연구 하위 그래프로 전환되는 다중 회전 Gradio 채팅입니다. `SchemaProvider("openai_responses", use_websocket=True)`를 사용합니다. `pip install gradio`가 필요합니다.|
| 17 |[`17_deep_research_crawl4ai.py`](17_deep_research_crawl4ai.py)|**OpenAI WS + Crawl4AI + Postgres**|16과 동일한 채팅 형태이지만 연구원들은 실제로 로컬 Crawl4AI 컨테이너(`docker run unclecode/crawl4ai`)를 통해 웹을 검색하고 상태는 Postgres(`PostgresCheckpointStore`)에서 지속됩니다. 둘 다 env vars를 통한 선택 사항입니다. 부재시에는 우아하게 뒤로 물러납니다. Postgres 경로에 대해 `-DNEOGRAPH_BUILD_POSTGRES=ON`를 사용하여 소스 빌드합니다.|
| 18 |[`18_node_cache.py`](18_node_cache.py)|**오픈AI**|`engine.set_node_cache_enabled("ask", True)` — 동일한 입력에 대한 두 번째 실행은 LLM 호출 없이 캐시된 `NodeResult`를 0ms 내에 재생합니다. `engine.node_cache_stats()`를 통한 통계.|
| 19 |[`19_streaming_messages.py`](19_streaming_messages.py)|오프라인|`from neograph_engine import message_stream` — `LLM_TOKEN` 이벤트가 LangChain 형태의 메시지 지시문(`{role, content, content_so_far, node, metadata}`)으로 도착하도록 콜백을 래핑합니다.|
| 20 |[`20_otel_tracing.py`](20_otel_tracing.py)|오프라인|`from neograph_engine.tracing import otel_tracer` — 엔진 이벤트를 OpenTelemetry 범위에 연결합니다. 배송 ConsoleSpanExporter; Jaeger / Tempo / Honeycomb / Datadog으로 보내려면 OTLP로 교체하세요.|
| 21 |[`21_http2_transport.py`](21_http2_transport.py)|**오픈AI**|`SchemaProvider(..., prefer_libcurl=True)` — 옵트인 HTTP/2(libcurl) 전송과 기본 ConnPool(HTTP/1.1 연결 유지) 비교. A/Bs는 5방향 병렬 버스트와 YOUR 엔드포인트에서 더 빠른 인쇄를 지원합니다. 기본 ConnPool은 api.openai.com에서 더 빠릅니다. CF-WAF 호환성이 필요한 경우 뒤집거나, 기업 프록시를 통해 TCP 팬아웃을 낮추거나, HTTP/3을 사용하세요.|
| 22 |[`22_self_evolving_graph.py`](22_self_evolving_graph.py)|**오픈AI**|목표 중심 자체 진화: 에이전트가 실행되고 JSON 모양 목표에 대한 출력의 점수를 매기고 LLM에 수정된 그래프 정의를 제안하도록 요청합니다. 점수 ≥ 1.0 또는 max_iters에 도달하면 루프가 닫힙니다. 수정자의 유일한 출력이 새로운 그래프 사양인 JSON-as-program을 보여줍니다.|
| 23 |[`23_evolving_chat_agent.py`](23_evolving_chat_agent.py)|오프라인(모의) / **OpenAI**|스레드별로 진화하는 채팅 에이전트: 지속적인 다중 전환 대화; 그 사이에 에이전트의 JSON 정의는 누적된 기록을 기반으로 다시 작성됩니다. 진화(이전 메시지 유지), `__graph_meta__` 감사 채널 패턴 및 유효성 검사기 경계(화이트리스트 노드 유형, 필수 채널, 에지 연결) 전반에 걸쳐 체크포인트 재개를 보여줍니다. 결정론적 모의 공급자 + 휴리스틱을 통해 API 키 없이 엔드투엔드 실행모의 진화자.|
| 24 |[`24_tool_approval_gate.py`](24_tool_approval_gate.py)|오프라인|도구 게이트(#89): `engine.set_tool_gate(...)`는 **도구가 실행되기 전에** 모든 도구 호출에 대해 참조되어 Allow / Allow-with-rewrite-args / Deny / Interrupt를 반환합니다. *"에이전트가 `rm -rf build/`를 실행하려고 합니다. 허용하시겠습니까?"*라는 표준 승인 프롬프트를 표시합니다. 그리고 결정적으로 사람이 결정하는 동안 무해한 형제 호출이 실행되지 **않았기** 때문에 거부는 실제로 아무 일도 일어나지 않았음을 의미하며 승인은 다시 실행되지 않는다는 의미입니다.|
| 25 |[`25_async_tools.py`](25_async_tools.py)|오프라인|동시 도구(#96): `ng.Tool` 대신 `ng.AsyncTool` 및 3개의 300ms 도구는 0.90초 대신 0.30초가 소요됩니다. 또한 동일한 실행에서 경계를 측정합니다. 세 가지 *CPU 바인딩* 도구는 하나보다 3.2배의 시간이 걸립니다. Python 함수가 실행되는 동안 GIL를 보유하고 스레드 수가 이를 변경하지 않기 때문입니다. 동시성은 선택되어 있으므로 기존의 상태 저장 도구가 갑자기 자체 경쟁을 할 수 없습니다.|
| 26 |[`26_mcp_tools.py`](26_mcp_tools.py)|오프라인|MCP (#95): `ng.mcp.MCPClient(url).get_tools()`는 원격 도구 카탈로그를 가져와 `NodeContext`에 직접 전달합니다. 자체 MCP 서버를 시작하므로 네트워크 없이 실행됩니다. 중요한 것을 측정합니다. 즉, HTTP보다 0.41초 만에 세 번의 0.4초 MCP 호출을 수행하고, 유지되지 *않는* 부분에 대해 큰 소리로 말합니다. stdio에는 파이프가 하나 있으므로 이러한 호출은 직렬화됩니다.|
| 27 |[`27_a2a_server.py`](27_a2a_server.py)|로컬호스트|A2A 호스팅(#120): 공식 `a2a-sdk`는 JSON-RPC, 작업 상태, 에이전트 카드 및 취소를 소유합니다. `ProtocolHostAdapter.stream()`는 체크포인트 컨텍스트를 유지하면서 엔진 토큰 이벤트를 청크된 A2A 아티팩트에 매핑합니다. Python 3.10+ 및 `pip install "neograph-engine[a2a]"`가 필요합니다.|
| 28 |[`28_acp_agent.py`](28_acp_agent.py)|스튜디오|ACP 호스팅(#120): 토큰 업데이트를 스트리밍하고, 그래프에 대한 text/image/audio/resource 콘텐츠 블록을 유지하며, `NEOGRAPH_ACP_POSTGRES_URL` 또는 `NEOGRAPH_ACP_SQLITE_PATH`가 설정된 경우 내구성 있는 `session/load`를 지원합니다. Python 3.10+ 및 `pip install "neograph-engine[acp]"`가 필요합니다.|

## 호스팅이 공식 SDK를 사용하는 이유

C++ 라이브러리에는 자체 `A2AServer` 및 `ACPServer`가 있지만 이를 노출합니다.
클래스는 Python 사용자에게 다음과 같은 두 번째 프로토콜 구현을 직접 제공합니다.
Python의 공식 SDK보다 통합이 약합니다. 특히, 공식
SDK는 이미 현재의 유선 형식 호환성, 서버 전송, 작업 또는
세션 수명주기 및 asyncio 취소. NeoGraph는 부품만 공급합니다.
해당 SDK는 C++ 그래프 엔진에 대한 체크포인트 인식 호출을 수행할 수 없습니다.

|목|결정|
|------|----------|
|누락된 것으로 보이는 C++ 기능|`A2AServer`, `ACPServer` 및 해당 수명 주기 메서드는 Python 클래스로 미러링되지 않습니다.|
|파이썬 대안|공식 `a2a-sdk` 1.x 및 `agent-client-protocol` 0.11.x 서버 런타임.|
|네오그래프 통합|`ProtocolHostAdapter`는 프로토콜 대화 ID를 `RunConfig.thread_id`에 매핑하고, `resume_if_exists`를 활성화하고, `LLM_TOKEN` 이벤트를 스트리밍하고, 사용자 지정 JSON 안전 입력 페이로드를 허용하고, 활성 asyncio 작업을 취소합니다.|
|종속성 정책|두 SDK 모두 Python 3.10 이상이 필요하고 `neograph-engine`는 Python 3.9를 지원하므로 선택 사항입니다. `neograph-engine[a2a]`, `neograph-engine[acp]` 또는 `neograph-engine[protocols]`를 설치합니다.|
|내구성 있는 ACP 세션|휠 지원 내구성 백엔드에 대해 `NEOGRAPH_ACP_POSTGRES_URL`를 설정합니다. `NEOGRAPH_BUILD_SQLITE=ON`를 사용한 소스 빌드는 `NEOGRAPH_ACP_SQLITE_PATH`를 설정할 수 있습니다. 에이전트는 `session/load`가 구성된 경우에만 광고합니다. 첫 번째 완료된 프롬프트가 체크포인트를 생성한 후 새 세션을 로드할 수 있게 됩니다. 세션 ID는 서버에서 생성된 기능이며 검사점은 개인용 `acp:` 스레드 네임스페이스를 사용합니다. 세션당 하나의 활성 에이전트 프로세스를 유지합니다. 체크포인트 저장소는 직렬화되지 않습니다.프로세스 전반의 동시 작성자.|
|현재 한도|ACP 편집기 콜백(`fs/read_text_file`, 터미널 호출, 권한 프롬프트)은 아직 공유 NeoGraph Python 도구에서 안전하게 호출할 수 없습니다. 현재의 `AsyncTool`는 작업자 스레드에서 동기 함수를 실행하며 현재 프로토콜 세션 ID를 전달하지 않습니다. 가짜 브리지는 잘못된 편집 세션을 호출할 위험이 있습니다.|
|다음과 같은 경우 직접 바인딩을 다시 살펴보세요.|사용자는 Python에 정확한 C++ 서버를 포함해야 합니다. 그렇지 않으면 공식 SDK 경로가 필수 NeoGraph 취소, 체크포인트, 추적 또는 도구 호출 동작을 보존할 수 없습니다.|

`ProtocolHostAdapter.run_payload()`는 다음을 통해 JSON 안전 값을 전달합니다.
`input_builder`를 구성했습니다. 기본 `message_input`는 풍부한 콘텐츠를 유지합니다.
사용자 메시지의 `content`로 차단됩니다. 공급자가 다른 그래프를 기대하는 그래프
모양은 맞춤 빌더를 전달해야 합니다. `ProtocolHostAdapter.stream()` 수익률
`ProtocolStreamEvent(kind="token", ...)` 값 뒤에 정확히 하나의 최종 값이 옵니다.
이벤트. `stream_node`가 그래프 노드의 이름을 지정하지 않는 한 라이브 토큰은 비활성화됩니다.
토큰은 최종 답을 정확히 형성합니다. 이렇게 하면 planner/tool-node 출력이 방지됩니다.
프로토콜 응답을 통해 누출되는 것을 방지합니다. asyncio 소비자 큐는 제한되어 있습니다.
(기본적으로 1,024개 청크) 오버플로는 엔진 작동을 취소합니다. 네이티브 스트림 이벤트
먼저 asyncio 루프에 예약되므로 이 대기열은
느린 프로토콜 전송, 하드 프로세스 전체 메모리 제한이 아님
무제한의 네이티브 프로듀서.

다음을 사용하여 실행하십시오.

```bash
python 01_minimal.py
```

## 정신 모델

Python의 NeoGraph는 Python의 LangGraph와 유사합니다.
노드, 리듀서가 있는 채널, `Send`를 통한 동적 팬아웃, 라우팅
`Command`를 통한 재정의, 명명된 조건을 통한 조건부 에지
(`route_channel`, `has_tool_calls` 등). 동일한 프리미티브,
동일한 JSON 모양의 그래프 정의. 차이점은 실행중인 내용입니다.
it — 슈퍼스텝 루프, 스케줄링 및 작업을 수행하는 C++ 엔진입니다.
LangGraph 대신 단계당 마이크로초 단위의 체크포인트
~600μs.

예제 전반에 걸쳐 세 가지 패턴이 나타납니다.

1. **Python 커스텀 노드** (01, 03, 04, 07, 09, 10, 11, 12, 13)
`neograph_engine.GraphNode`를 서브클래스로 만들고 `run(input)`를 구현합니다.
`input.state`에서 채널을 읽고, 존재하는 경우 `input.stream_cb`를 사용합니다.
쓰기 `Command`, `Send` 또는 `NodeResult`를 반환합니다. 엔진
GIL 처리에 따라 Python으로 디스패치하므로 동시 사용자 정의 노드
교착 상태에 빠지지 마십시오.

2. **Python 도구** (02, 06, 07) 하위 클래스 `neograph_engine.Tool` 및
인스턴스를 `NodeContext(tools=[…])`에 전달합니다. 엔진이 걸립니다
컴파일 타임의 소유권; Python 참조는 나중에 삭제될 수 있습니다.

3. **Async** (04) — 모든 `*_async` 바인딩은
`asyncio.Future`는 호출 스레드의 실행 루프에 바인딩됩니다.
스트림 콜백은 다음을 통해 루프 스레드로 이동됩니다.
`loop.call_soon_threadsafe`이므로 `cb(ev)`는 asyncio에서 실행됩니다.
기대합니다.

## 그래프 정의는 JSON입니다.

`GraphEngine.compile(definition, ctx)`는 Python 중 하나를 허용합니다.
`dict` 코드로 빌드하거나 파일에서 `json.loads()` `dict`
— 같은 모양. 예제 14 + 15는 왕복을 보여줍니다. 커스텀 노드
*유형*은 여전히 ​​코드에 등록되어야 합니다(Python 클래스는 등록할 수 없습니다).
JSON로 인코딩되지만 배선 — 채널, 유형별 노드,
가장자리, 조건부 가장자리 - 데이터입니다.

## 배포 이름과 수입 이름

PyPI 패키지는 **`neograph-engine`**(기본 `neograph` 이름)입니다.
관련 없는 프로젝트에서 이미 PyPI를 사용했습니다). 파이썬
가져오기 이름은 `neograph_engine`입니다.

```python
import neograph_engine as ng
from neograph_engine.llm import OpenAIProvider, SchemaProvider
```
