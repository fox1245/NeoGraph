<!-- neograph-i18n: source=bindings/python/examples/README.md locale=ko source_sha256=26de7309b7f766019fcfd7817d7f697ddb001ae663c127d72fee305abdf2a559 -->
# Python API 예제

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

바인딩 표면 전체를 다루는 스물여덟 개의 스크립트.

## 설정

```bash
pip install neograph-engine python-dotenv
cp .env.example .env  # edit OPENAI_API_KEY for examples that hit a real LLM
```

`_common.py`는 이 디렉터리 또는 상위 디렉터리에서 `.env`를 자동 로드합니다. API 키가 필요한 예제는 키가 없을 때 충돌 없이 깨끗하게 건너뜁니다.

## 색인

| # | 파일 | 네트워크 | 패턴 |
|---|------|---------|---------|
| 01 | [`01_minimal.py`](01_minimal.py) | 오프라인 | `GraphNode` 서브클래스 + `engine.run()`. 가장 작은 유용한 그래프. |
| 02 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | 오프라인 | `Tool` 서브클래스 + 내장 `tool_dispatch`. 수제 tool_call(실제 LLM 없음). |
| 03 | [`03_send_fanout.py`](03_send_fanout.py) | 오프라인 | `run(input)` `NodeResult`를 반환하며 `Send` 목록 + `set_worker_count(4)`와 함께. Map-reduce. |
| 04 | [`04_async_concurrent.py`](04_async_concurrent.py) | 오프라인 | `engine.run_async` + 8개 동시 실행의 `asyncio.gather` + `run_stream_async`. |
| 05 | [`05_openai_provider.py`](05_openai_provider.py) | **OpenAI** | `OpenAIProvider` + 내장 `llm_call` 노드. 일회성 완료. |
| 06 | [`06_react_agent.py`](06_react_agent.py) | **OpenAI** | ReAct 루프: `llm_call` ↔ `tool_dispatch` with `has_tool_calls` 조건부. |
| 07 | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | 오프라인 | 두 단계 구조 제안/승인 작업 흐름 with mock LLM emitter. |
| 08 | [`08_intent_routing.py`](08_intent_routing.py) | **OpenAI** | 분류기 노드 + 조건부 에지 → 수학 / 번역 / 일반 전문가. |
| 09 | [`09_state_management.py`](09_state_management.py) | 오프라인 | `set_checkpoint_store(InMemoryCheckpointStore())` + `get_state` + `fork`. |
| 10 | [`10_command_routing.py`](10_command_routing.py) | 오프라인 | `run(input)` 반환 `Command(goto_node=…, updates=[…])`. |
| 11 | [`11_reflexion.py`](11_reflexion.py) | **OpenAI** | 반성 프롬프트가 있는 Actor + critic 루프 (Shinn et al. 2023). |
| 12 | [`12_self_ask.py`](12_self_ask.py) | **OpenAI** | Self-Ask 후속 질문 분해 (Press et al. 2022). |
| 13 | [`13_multi_agent_debate.py`](13_multi_agent_debate.py) | **OpenAI** | 토론자 2명 + 심판. 토론자들은 `Send`를 통해 fan-out합니다. |
| 14 | [`14_graph_to_json.py`](14_graph_to_json.py) | 오프라인 | 그래프 정의를 `.json` 파일로 직렬화합니다. |
| 15 | [`15_graph_from_json.py`](15_graph_from_json.py) | 오프라인 | `.json` 그래프를 로드하여 실행합니다(14의 동반 기능). |
| 16 | [`16_deep_research_chat.py`](16_deep_research_chat.py) | **OpenAI WS** | `조사해줘 / research / investigate`에서 병렬 딥-리서치 서브그래프graph로 전환하는 Multi-turn Gradio chat입니다. `SchemaProvider("openai_responses", use_websocket=True)`를 사용합니다. `pip install gradio`가 필요합니다. |
| 17 | [`17_deep_research_crawl4ai.py`](17_deep_research_crawl4ai.py) | **OpenAI WS + Crawl4AI + Postgres** | 16번과 동일한 채팅 형태이지만, 연구자들은 로컬 Crawl4AI 컨테이너(`docker run unclecode/crawl4ai`)를 통해 실제로 웹을 검색하며, 상태는 Postgres(`PostgresCheckpointStore`)에 영속화됩니다. 둘 다 env 변수를 통해 선택 사항이며, 없으면 정상적으로 폴백됩니다. Postgres 경로는 `-DNEOGRAPH_BUILD_POSTGRES=ON`로 소스 빌드합니다. |
| 18 | [`18_node_cache.py`](18_node_cache.py) | **OpenAI** | `engine.set_node_cache_enabled("ask", True)` — 동일한 입력에 대한 두 번째 실행은 캐시된 `NodeResult` 을 0ms 안에 재생(replay)하며, LLM 호출이 없습니다. 통계는 `engine.node_cache_stats()`. |
| 19 | [`19_streaming_messages.py`](19_streaming_messages.py) | 오프라인 | `from neograph_engine import message_stream` — 콜백을 래핑하여 `LLM_TOKEN` 이벤트가 LangChain 형태의 메시지 dict(`{role, content, content_so_far, node, metadata}`)으로 도착하도록 합니다. |
| 20 | [`20_otel_tracing.py`](20_otel_tracing.py) | 오프라인 | `from neograph_engine.tracing import otel_tracer` — 엔진 이벤트를 OpenTelemetry 스팬으로 브리지합니다. ConsoleSpanExporter가 포함되어 있으며, OTLP로 교체하여 Jaeger / Tempo / Honeycomb / Datadog으로 전송할 수 있습니다. |
| 21 | [`21_http2_transport.py`](21_http2_transport.py) | **OpenAI** | `SchemaProvider(..., prefer_libcurl=True)` — 옵트인(opt-in) HTTP/2(libcurl) 전송 방식과 기본 ConnPool(HTTP/1.1 keep-alive) 비교. 5-way 병렬 버스트에서 A/B 테스트를 수행하고 사용자 엔드포인트에서 어느 쪽이 더 빠른지 출력합니다. api.openai.com에서는 기본 ConnPool이 더 빠릅니다. CF-WAF 호환성, 기업 프록시를 통한 낮은 TCP fan-out, 또는 HTTP/3이 필요할 때 전환하세요. |
| 22 | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | **OpenAI** | 목표 중심 자기 진화: 에이전트가 실행되어 JSON 형태의 목표에 대해 출력을 평가하고, LLM에게 수정된 그래프 정의를 제안하도록 요청합니다. 점수가 ≥ 1.0이거나 max_iters에 도달하면 루프가 종료됩니다. 수정자의 유일한 출력이 새 그래프 사양인 JSON-as-Program을 보여줍니다. |
| 23 | [`23_evolving_chat_agent.py`](23_evolving_chat_agent.py) | **OpenAI** | 스레드별로 진화하는 채팅 에이전트: 지속적인 다중 턴 대화; 턴 사이에 에이전트의 JSON 정의는 누적된 기록을 기반으로 다시 작성됩니다. 이전 메시지가 유지되는 진화 전반에 걸친 체크포인트 재개(checkpoint-resume), `__graph_meta__` 감사 채널 패턴, 그리고 검증자 경계(노드 유형 화이트리스트, 필수 채널, 에지 연결성)를 보여줍니다. `OPENAI_API_KEY`를 요구하며, 없을 경우 완전히 종료됩니다. |
| 24 | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) | 오프라인 | 도구 게이트(#89): 모든 도구 호출에 대해 **어떤 도구가 실행되기 전에** `engine.set_tool_gate(...)`를 조회하며 Allow / Allow-with-rewritten-args / Deny / Interrupt를 반환합니다. 표준 승인 프롬프트를 보여줍니다 — *"에이전트가 `rm -rf build/`를 실행하려고 합니다. 허가하시겠습니까?"* — 그리고 중요한 것은, 인간이 결정하는 동안 무해한 형제 호출이 **실행되지 않았다는 것**, 즉 거부는 실제로 아무 일도 일어나지 않았음을 의미하며 승인은 재실행하지 않는다는 점입니다. |
| 25 | [`25_async_tools.py`](25_async_tools.py) | 오프라인 | 동시 도구(#96): `ng.AsyncTool` 대신 `ng.Tool`로, 세 개의 300ms 도구가 0.90초가 아닌 0.30초가 걸립니다. 또한 동일한 실행에서 경계를 측정합니다 — 세 개의 CPU-밀접 도구는 하나보다 3.2배의 시간이 걸리는데, 이는 Python 함수가 실행 중에 GIL을 보유하고 있어 스레드 수와 무관하기 때문입니다. 동시성은 옵트인 방식이므로 기존 상태형 도구가 갑자기 자기 자신과 경합을 일으키지 않습니다. |
| 26 | [`26_mcp_tools.py`](26_mcp_tools.py) | 오프라인 | MCP(#95): `ng.mcp.MCPClient(url).get_tools()`가 원격 도구 카탈로그를 가져와 바로 `NodeContext`에 전달합니다. 자체 MCP 서버를 시작하므로 네트워크 없이 실행됩니다. 반복되는 명명 호출은 기본적으로 직렬로 유지됩니다. 스레드 데모는 `fetch` Reentrant를 `ToolExecutionPolicyRegistry`를 통해 명시적으로 표시한 다음 0.4초인 3회의 HTTP 호출을 소요 시간이 0.41초로 측정합니다. stdio는 JSON-RPC ID도 멀티플렉싱하지만, 겹쳐서 실행하려면 해당 호스트 정책과 동시성 서버가 모두 필요합니다. |
| 27 | [`27_a2a_server.py`](27_a2a_server.py) | 로컬호스트 | A2A 호스팅(#120): 공식 `a2a-sdk`가 JSON-RPC, 태스크 상태, 에이전트 카드, 취소를 소유합니다. `ProtocolHostAdapter.stream()`는 엔진 토큰 이벤트를 청크된 A2A 아티팩트에 매핑하면서 체크포인트 컨텍스트를 보존합니다. Python 3.10+ 및 `pip install "neograph-engine[a2a]"`가 필요합니다. |
| 28 | [`28_acp_agent.py`](28_acp_agent.py) | stdio | ACP 호스팅(#120): 스트림 토큰 업데이트, 그래프를 위한 텍스트/이미지/오디오/리소스 콘텐츠 블록 보존, 그리고 `session/load` 또는 `NEOGRAPH_ACP_POSTGRES_URL`가 설정된 경우 내구성 있는 `NEOGRAPH_ACP_SQLITE_PATH`를 지원합니다. Python 3.10+ 및 `pip install "neograph-engine[acp]"`가 필요합니다. |

## 호스팅이 공식 SDK를 사용하는 이유

C++ 라이브러리에는 자체 `A2AServer`와 `ACPServer`가 있지만, 해당 클래스를 직접 노출하면 Python 사용자에게 공식 SDK보다 통합이 약한 두 번째 프로토콜 구현이 제공됩니다. 특히 공식 SDK는 이미 현재 wire-format 호환성, server transports, task 또는 session lifecycle, asyncio 취소를 담당합니다. NeoGraph는 해당 SDK가 제공할 수 없는 부분, 즉 체크포인트를 감지하는 C++ 그래프 엔진 호출만 제공합니다.

| 항목 | 결정 |
|------|----------|
| 누락된 것처럼 보이는 C++ 기능 | `A2AServer`, `ACPServer` 및 그 수명 주기 메서드는 Python 클래스로 미러링되지 않습니다. |
| Python 대안 | 공식 `a2a-sdk` 1.x 및 `agent-client-protocol` 0.11.x 서버 런타임. |
| NeoGraph 통합 | `ProtocolHostAdapter`는 프로토콜 대화 ID를 `RunConfig.thread_id`에 매핑하고, `resume_if_exists`를 활성화하며, `LLM_TOKEN` 이벤트를 스트리밍하고, 사용자 정의 JSON 안전 입력 페이로드를 수락하며, 활성 asyncio 작업을 취소합니다. |
| 종속성 정책 | 두 SDK 모두 선택 사항입니다. 왜냐하면 Python 3.10+가 필요하지만 `neograph-engine`는 Python 3.9를 지원하기 때문입니다. `neograph-engine[a2a]`, `neograph-engine[acp]`, 또는 `neograph-engine[protocols]`를 설치하세요. |
| 지속적 ACP 세션 | 휠 지원 영구 백엔드에 대해 `NEOGRAPH_ACP_POSTGRES_URL`를 설정합니다. `NEOGRAPH_BUILD_SQLITE=ON`로 빌드한 소스는 `NEOGRAPH_ACP_SQLITE_PATH`를 설정할 수 있습니다. 에이전트는 구성된 경우에만 `session/load`를 알립니다. 새 세션은 첫 번째 완료된 프롬프트가 체크포인트를 생성한 후에 로드 가능해집니다. 세션 ID는 서버 생성 자격 증명이며 체크포인트는 비공개 `acp:` 스레드 네임스페이스를 사용합니다. 세션당 하나의 활성 에이전트 프로세스를 유지합니다. 체크포인트 저장소는 프로세스 간 동시 쓰기를 직렬화하지 않습니다. |
| 현재 한도 | ACP editor 콜백(`fs/read_text_file`, 터미널 호출, 권한 프롬프트)은 공유 NeoGraph Python 도구에서 아직 안전하게 호출할 수 없습니다. 현재의 `AsyncTool`는 작업자 스레드에서 동기 함수를 실행하며 현재 프로토콜 세션 ID를 전달하지 않으므로, 대체 브리지는 잘못된 편집기 세션을 호출할 위험이 있습니다. |
| 직접 바인딩을 다시 검토할 시점 | 사용자는 정확한 C++ 서버를 Python에 내장해야 하며, 그렇지 않으면 공식 SDK 경로가 필수적인 NeoGraph 취소, 체크포인트, 트레이싱, 또는 tool-call 동작을 보존할 수 없다. |

`ProtocolHostAdapter.run_payload()` 구성된 `input_builder`를 통해 모든 JSON 안전 값을 전달합니다. 기본 `message_input` 는 사용자 메시지의 `content`로 풍부한 콘텐츠 블록을 유지합니다. 공급자가 다른 형태를 기대하는 그래프는 사용자 정의 빌더를 전달해야 합니다. `ProtocolHostAdapter.stream()` 는 `ProtocolStreamEvent(kind="token", ...)` 값과 정확히 하나의 최종 이벤트를 산출합니다. 라이브 토큰은 `stream_node` 가 최종 답변을 정확히 형성하는 토큰을 가진 그래프 노드를 명명하지 않는 한 비활성화됩니다. 이는 플래너/도구 노드 출력이 프로토콜 응답을 통해 유출되는 것을 방지합니다. asyncio 소비자 큐는 제한되어 있습니다(기본적으로 1,024개의 청크). 오버플로는 엔진 실행을 취소합니다. 네이티브 스트림 이벤트는 먼저 asyncio 루프에 스케줄링되므로, 이 큐는 느린 프로토콜 전송에 대한 백프레셔이며, 무제한 네이티브 프로듀서에 대한 프로세스 전체의 하드 메모리 상한이 아닙니다.

다음 중 하나를 실행하십시오:

```bash
python 01_minimal.py
```

## 정신적 모델

Python에서의 NeoGraph는 Python에서의 LangGraph와 유사합니다: 노드의 그래프, 리듀서가 있는 채널, `Send`를 통한 동적 fan-out, `Command`를 통한 라우팅 재정의, 명명된 조건(`route_channel`, `has_tool_calls` 등)을 통한 조건부 엣지. 동일한 프리미티브, 동일한 JSON 형태의 그래프 정의입니다. 차이점은 이를 실행하는 것이 무엇인가입니다 — 슈퍼스텝 루프, 스케줄링, 체크포인트를 단계당 마이크로초 단위로 수행하는 C++ 엔진으로, LangGraph의 약 600 µs 대신입니다.

세 가지 패턴이 예제 전반에 나타납니다:

1. **Python 커스텀 노드** (01, 03, 04, 07, 09, 10, 11, 12, 13)는 `neograph_engine.GraphNode`를 서브클래싱하고 `run(input)`를 구현합니다. `input.state`에서 채널을 읽고, 존재 시 `input.stream_cb`를 사용하며, 쓰기, `Command`, `Send` 또는 `NodeResult`를 반환합니다. 엔진은 GIL 처리 하에 Python으로 디스패치하므로 동시 커스텀 노드가 교착 상태에 빠지지 않습니다.

2. **Python 도구**(02, 06, 07)는 `neograph_engine.Tool`를 서브클래싱하고 인스턴스를 `NodeContext(tools=[…])`에 전달합니다. 엔진은 컴파일 시점에 소유권을 취득하며, 이후 Python 참조는 해제될 수 있습니다.

3. **Async** (04) — 모든 `*_async` 바인딩은 호출 스레드의 실행 루프에 바인딩된 `asyncio.Future`를 반환합니다. 스트림 콜백은 `loop.call_soon_threadsafe`를 통해 루프 스레드로 이동하므로, `cb(ev)`는 asyncio가 예상하는 위치에서 실행됩니다.

## 그래프 정의는 JSON입니다

`GraphEngine.compile(definition, ctx)` 는 Python `dict` 코드에서 직접 만들거나 `dict` 파일에서 `json.loads()` 가져온 — 동일한 형식 — 을 허용합니다. 예제 14와 15는 왕복(round-trip) 과정을 보여줍니다. 사용자 정의 노드 *유형*은 여전히 코드에 등록해야 합니다(Python 클래스는 JSON으로 인코딩할 수 없음). 그러나 배선 — 채널, 유형별 노드, 엣지, 조건부 엣지 — 은 데이터입니다.

## 배포 이름 대 가져오기 이름

PyPI 패키지는 **`neograph-engine`**입니다 (단순한 `neograph` 이름은 PyPI에서 무관한 프로젝트에 이미 사용 중입니다). Python import 이름은 `neograph_engine`입니다:

```python
import neograph_engine as ng
from neograph_engine.llm import OpenAIProvider, SchemaProvider
```
