<!-- neograph-i18n: source=examples/README.md locale=ko source_sha256=b38723d49825be5864da3e487bb598e327050e455c01e93cc3d22f838f91396d -->
**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

P8 cutover inventory: [`spec/neograph-example-disposition-v1.json`](../spec/neograph-example-disposition-v1.json).

# C++ API 예제


NeoGraph 엔진 표면을 다루는 56개의 실행 가능한 C++ 프로그램.
각각은 이 디렉터리에 있는 단일 파일입니다(하나의 Docker-Compose 포함).
예외, [`26_postgres_react_hitl/`](26_postgres_react_hitl/)) — 복사
하나를 자신의 프로젝트에 추가하고 `neograph::core`에 연결 +
`neograph::llm`, 그리고 출발점이 있습니다.

## 짓다

기본 CMake 빌드는 모든 예제를 빌드합니다.

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

건너뛰려면 `-DNEOGRAPH_BUILD_EXAMPLES=OFF`를 전달하세요. 필요한 예
추가 deps(Crawl4AI Docker, Postgres, MCP 서버, Clay+Raylib)는
명시적인 CMake 옵션 또는 런타임 프로브에 의해 제어됩니다.
아래의 '설정' 열.

## 설정

실제 LLM에 도달하는 예는 cwd에서 `.env`를 자동 로드합니다.
상위) cppdotenv를 통해. 인식되는 두 개의 키는 다음과 같습니다.

```
OPENAI_API_KEY=sk-...
ANTHROPIC_API_KEY=sk-ant-...
```

아래의 "설정" 항목이 없는 예에는 API 키가 필요하지 않습니다.
진행 중인 `MockProvider` 또는 순수 모의 노드.

## 여기서 시작하세요

이번이 처음이라면:

|첫 번째|당신이 배우는 것|
|---|---|
|[`62_core_quickstart.cpp`](62_core_quickstart.cpp)|**Core 빠른 시작** — 설치된 `neograph::core` 대상, 엄격한 그래프 하나, 타입 채널 하나. 선택적 컴포넌트와 API 키가 필요하지 않습니다.|
|[`63_program_quickstart.cpp`](63_program_quickstart.cpp)|**Program 빠른 시작** — 설치된 `neograph::program` 대상으로 `call_core` Program을 컴파일, 승인, 실행합니다. `-DNEOGRAPH_BUILD_PROGRAM=ON`이 필요합니다.|
|[`51_minimal.cpp`](51_minimal.cpp)|가장 작은 작업 프로그램 — `result.channel<T>("name")`를 빌드하고, 실행하고, 읽습니다. API 키가 없습니다.|
|[`02_custom_graph.cpp`](02_custom_graph.cpp)|JSON 그래프 정의를 빌드하고 실행합니다. API 키가 없습니다.|
|[`05_parallel_fanout.cpp`](05_parallel_fanout.cpp)|`make_parallel_group`를 사용한 비동기 팬아웃. API 키가 없습니다.|
|[`10_send_command.cpp`](10_send_command.cpp)|`Send`(동적 팬아웃) + `Command`(라우팅 재정의). API 키가 없습니다.|
|[`01_react_agent.cpp`](01_react_agent.cpp)|실제 LLM + 계산기 도구를 사용한 ReAct 루프. **`OPENAI_API_KEY`가 필요합니다.**|
|[`14_plan_executor.cpp`](14_plan_executor.cpp)|계획 → 병렬 하위 작업 → 해결사, 체크포인트 저장소를 통한 충돌 복구. API 키가 없습니다.|

이러한 내용이 이해되면 아래의 나머지 내용은 해당 내용에 따라 그룹화됩니다.
파일 번호가 아닌 시연합니다.

## 색인

### 핵심 엔진 - 그래프, 상태, 라우팅

| # |파일|설정|그것이 보여주는 것|
|---|------|-------|---------------|
| 02 |[`02_custom_graph.cpp`](02_custom_graph.cpp)|오프라인|JSON 그래프를 빌드하고 실행합니다. 이 저장소에서 가장 짧고 유용한 프로그램입니다.|
| 05 |[`05_parallel_fanout.cpp`](05_parallel_fanout.cpp)|오프라인|비동기 팬아웃 — 3개의 "연구원" 노드가 하나의 io_context에서 공동 실행되고 요약기가 이를 팬아웃합니다.|
| 06 |[`06_subgraph.cpp`](06_subgraph.cpp)|오프라인|계층적 구성 - 외부 감독자 그래프가 내부 ReAct 하위 그래프에 위임됩니다.|
| 07 |[`07_intent_routing.cpp`](07_intent_routing.cpp)|오프라인|분류기 → 조건부 엣지 → 수학/번역/일반 전문가.|
| 08 |[`08_state_management.cpp`](08_state_management.cpp)|오프라인|`get_state` / `update_state` / `fork` — LangGraph의 체크포인터 API가 C++에 매핑되었습니다.|
| 09 |[`09_all_features.cpp`](09_all_features.cpp)|오프라인|하나의 데모에 포함된 6가지 기능 — `NodeInterrupt`, `RetryPolicy`, `StreamMode`, `Send`, `Command`, `Store`.|
| 10 |[`10_send_command.cpp`](10_send_command.cpp)|오프라인|기획자→보내기→연구원→명령(루프)|완료) - 표준 Send+Command 패턴.|
| 42 |[`42_custom_reducer_condition.cpp`](42_custom_reducer_condition.cpp)|오프라인|C++에서 사용자 정의 채널 리듀서 및 에지 조건을 등록하세요. 엔진을 건드리지 않고 JSON 어휘를 확장하세요.|
| 43 |[`43_store_personalization.cpp`](43_store_personalization.cpp)|오프라인|`in.ctx.store`를 통해 노드 내부에서 도달한 크로스 스레드 `Store` — 공유 네임스페이스 메모리의 사용자별 노드 동작입니다.|
| 51 |[`51_minimal.cpp`](51_minimal.cpp)|오프라인|가장 짧은 작업 프로그램 — 빌드, 실행, `result.channel<T>("name")`. 신규 사용자 템플릿.|
| 52 |[`52_export_schema.cpp`](52_export_schema.cpp)|오프라인|`NodeFactory::export_schema()` → 토폴로지 JSON 스키마 덤프. 코드가 없는 시각적 편집기가 팔레트를 구성하는 버전 고정 소스입니다.|
| 56 |[`56_history_compaction.cpp`](56_history_compaction.cpp)|오프라인(선택적 OpenAI)|제한된 메시지 창 - 기록이 예산을 초과하면 삭제된 접두사가 LLM로 작성된 요약으로 대체됩니다. 기본적으로 모의 공급자.|

### Real LLM — 공급자, 도구, ReAct

| # |파일|설정|그것이 보여주는 것|
|---|------|-------|---------------|
| 01 |[`01_react_agent.cpp`](01_react_agent.cpp)|오픈AI|ReAct 루프: `llm_call` ⇔ `tool_dispatch`(`has_tool_calls` 조건부 포함) 계산기 도구.|
| 12 |[`12_rag_agent.cpp`](12_rag_agent.cpp)|오픈AI|실제 `text-embedding-3-small` 임베딩 + 메모리 내 코사인 검색을 사용하는 RAG.|
| 13 |[`13_openai_responses.cpp`](13_openai_responses.cpp)|오픈AI|동일한 ReAct 루프이지만 `SchemaProvider("openai_responses")`를 통해 `/v1/responses`에 연결되었습니다. 하나의 구성 전환, 공급자 하위 클래스 없음.|
| 33 |[`33_openai_responses_ws.cpp`](33_openai_responses_ws.cpp)|오픈AI|WebSocket을 통한 API 응답 — `wss://api.openai.com/v1/responses`. 다중 도구 에이전트 루프의 대기 시간이 최대 40% 감소합니다.|
| 34 |[`34_openai_responses_ws_tools.cpp`](34_openai_responses_ws_tools.cpp)|오픈AI|모든 Responses-API 내장 도구(web_search, image_ Generation, file_search, tool_search, Skills, Shell)를 둘러보세요. 와이어 수준(SchemaProvider 없음).|
| 29 |[`29_responses_envelope.cpp`](29_responses_envelope.cpp)|오픈AI|디버그 지원: 하나의 도구 호출 요청에 대해 원시 `/v1/responses` JSON 봉투를 덤프합니다. 의도적으로 SchemaProvider를 우회합니다.|
| 30 |[`30_reasoning_effort.cpp`](30_reasoning_effort.cpp)|오픈AI|하나의 프롬프트에서 `{minimal, low, medium, high}`의 `reasoning_effort`를 스윕합니다. 대기 시간/숨겨진 CoT 토큰/응답 품질 트레이드오프를 확인하세요.|

### 추론 패턴

| # |파일|설정|무늬|
|---|------|-------|---------|
| 15 |[`15_reflexion.cpp`](15_reflexion.cpp)|인류학|반성 - 비평가가 ACCEPT라고 말할 때까지 생성기 ⇔ 비평가 루프(Shinn et al. 2023). 하이쿠 제약 작업.|
| 16 |[`16_tree_of_thoughts.cpp`](16_tree_of_thoughts.cpp)|인류학|생각의 나무 — 각 깊이에서 N개의 후보 생각을 생성하고 점수를 매기고 상위 K를 유지하고 확장합니다. 24일 경기.|
| 17 |[`17_self_ask.cpp`](17_self_ask.cpp)|인류학|자체 질문 - 명시적인 "후속 질문이 필요합니까?" 다중 홉 추론을 위한 분해(Press et al. 2022).|
| 18 |[`18_multi_agent_debate.cpp`](18_multi_agent_debate.cpp)|인류학|연구원/회의론자/판사 — 3개의 시스템 프롬프트, 공유된 기록, 판사 판결.|
| 19 |[`19_rewoo.cpp`](19_rewoo.cpp)|인류학|REWOO — 플래너는 `#E1 / #E2` 자리 표시자를 사용하여 전체 계획을 커밋하고 작업자는 도구를 병렬로 팬아웃하고 솔버를 합성합니다.|

### 지속성 및 HITL

| # |파일|설정|그것이 보여주는 것|
|---|------|-------|---------------|
| 04 |[`04_checkpoint_hitl.cpp`](04_checkpoint_hitl.cpp)|오프라인|`interrupt_before` 결제 노드, 체크포인트 유지, 운영자 승인 후 재개. 모의 공급자.|
| 14 |[`14_plan_executor.cpp`](14_plan_executor.cpp)|오프라인|시뮬레이션된 중간 팬아웃 오류가 있는 Plan-and-Executor - 체크포인트 재생은 실패한 형제만 다시 실행합니다. 보류 중인 쓰기 기계가 작동 중입니다.|
| 26 |[`26_postgres_react_hitl/`](26_postgres_react_hitl/)|OpenAI WS + Postgres + Crawl4AI|프로세스 중단 심층 연구 HITL — PG 지원 체크포인트는 보고와 재개 사이에 `exit`를 유지합니다. Docker-Compose 기반.|
| 41 |[`41_resume_if_exists_chat.cpp`](41_resume_if_exists_chat.cpp)|오프라인|LangGraph 스타일의 다중 턴 채팅 — `resume_if_exists`는 이전 체크포인트를 다시 로드하고 새 턴을 추가합니다. 모의 공급자.|
| 48 |[`48_sqlite_checkpoint.cpp`](48_sqlite_checkpoint.cpp)|오프라인|`SqliteCheckpointStore` — 단일 파일 지속성 실행, 서버 없음. InMemory/Postgres와 동일한 `CheckpointStore` 인터페이스.|

### MCP(모델 컨텍스트 프로토콜)

| # |파일|설정|그것이 보여주는 것|
|---|------|-------|---------------|
| 03 |[`03_mcp_agent.cpp`](03_mcp_agent.cpp)|OpenAI + MCP HTTP 서버|스트리밍 가능한 http MCP 서버에서 도구를 검색하고 ReAct 루프를 구동하세요.|
| 22 |[`22_mcp_stdio.cpp`](22_mcp_stdio.cpp)|OpenAI + Python stdio 스크립트|03과 동일하지만 MCP 서버는 stdin/stdout를 통한 하위 하위 프로세스이며 네트워크 스택이 없습니다.|
| 23 |[`23_mcp_multi.cpp`](23_mcp_multi.cpp)|OpenAI + 서버 2개|하나의 에이전트, 두 개의 MCP 서버(HTTP + stdio), 도구가 하나의 목록으로 병합되었습니다. LLM는 두 가지 모두를 투명하게 선택합니다.|
| 21 |[`21_mcp_fanout.cpp`](21_mcp_fanout.cpp)|MCP HTTP 서버(LLM 없음)|Planner는 MCP 호출당 하나의 Send를 내보냅니다. `make_parallel_group`는 이를 동시에 실행합니다. 결정적 — LLM는 도구를 직접 선택하므로 데모는 LLM 축에서 오프라인으로 유지됩니다.|
| 20 |[`20_mcp_hitl.cpp`](20_mcp_hitl.cpp)|OpenAI + MCP HTTP 서버|`interrupt_before` 모든 MCP 도구 호출 — 운영자는 보류 중인 도구 이름 + 인수를 확인하고 승인하고 재개합니다.|
| 24 |[`24_mcp_feedback.cpp`](24_mcp_feedback.cpp)|OpenAI + MCP HTTP 서버|교환원은 상담원의 답변 초안을 읽고 피드백을 입력합니다. 두 번째 실행에서는 해당 피드백을 새로운 대화 컨텍스트로 통합합니다.|

### 비동기, 동시성, 성능

| # |파일|설정|그것이 보여주는 것|
|---|------|-------|---------------|
| 27 |[`27_async_concurrent_runs.cpp`](27_async_concurrent_runs.cpp)|오프라인|3개의 에이전트가 `engine->run_async()`를 통해 하나의 `io_context` 스레드에서 인터리브를 실행합니다. 벽은 3×50ms 대신 50ms입니다. 4단계 비동기 엔드투엔드.|
| 40 |[`40_react_async_streaming.cpp`](40_react_async_streaming.cpp)|오픈AI|외부 `asio::io_context` + `co_spawn` + `co_await engine->run_stream_async(...)`는 LLM 노드의 토큰이 `SchemaProvider("openai_responses")`에 대해 `co_await provider->complete_stream_async(...)`를 통해 표준 출력으로 스트리밍되는 ReAct 루프를 구동합니다. **PR-#10 이전에 세그먼트 오류가 발생한 정확한 모양** — 수정 후 깔끔하게 실행됩니다. 도구 왕복 + 최종 답변은 ~4초 내에 완료됩니다.|
| 44 |[`44_request_queue_backpressure.cpp`](44_request_queue_backpressure.cpp)|오프라인|배압이 있는 고정 작업자 풀(`neograph::util::RequestQueue`) — 제한된 기내 작업, 부하 시 무제한 증가가 없습니다.|
| 46 |[`46_cancel_token.cpp`](46_cancel_token.cpp)|오프라인|협력 취소 — 자녀당 `CancelToken::fork()`, 부모 `cancel()`는 비행 중인 모든 자녀에게 계단식으로 전달됩니다.|
| 47 |[`47_node_cache.cpp`](47_node_cache.cpp)|오프라인|노드 + 입력에 맞춰진 노드별 결과 캐시 - 실행 전반에 걸쳐 동일한 입력에 대한 재계산을 건너뜁니다.|
| 50 |[`50_async_tool.cpp`](50_async_tool.cpp)|오프라인|`AsyncTool` — 코루틴 모양의 도구 실행 어댑터이므로 도구는 io_context를 차단하지 않고 `co_await`를 수행할 수 있습니다.|

### 에이전트 상호 운용성 — A2A & ACP

| # |파일|설정|그것이 보여주는 것|
|---|------|-------|---------------|
| 38 |[`38_a2a_server.cpp`](38_a2a_server.cpp)|오프라인|컴파일된 NeoGraph를 에이전트 간 엔드포인트(HTTP, 스트리밍 SSE)로 노출합니다. 이것을 먼저 실행하십시오.|
| 37 |[`37_a2a_client.cpp`](37_a2a_client.cpp)|오프라인(예제 38 실행 필요)|*원격* A2A 에이전트 구동 — `A2ACallerNode`는 원격 에이전트를 로컬 노드처럼 보이게 만듭니다.|
| 39 |[`39_acp_server.cpp`](39_acp_server.cpp)|오프라인|에이전트 클라이언트 프로토콜(Zed 스타일)이 구동하는 형태인 stdio를 통한 양방향 JSON-RPC를 통해 NeoGraph를 노출합니다.|

### 분산 — gRPC 서비스 및 원격 checkpoint/tool

`-DNEOGRAPH_BUILD_GRPC=ON`로만 제작되었습니다(`grpc++` / `protoc` 필요).

| # |파일|설정|그것이 보여주는 것|
|---|------|-------|---------------|
| 52 |[`52_grpc_server.cpp`](52_grpc_server.cpp)|오프라인(grpc++)|gRPC(느리게 컴파일되고 캐시된 개별 그래프별 엔진)를 통해 `GraphEngine`를 노출합니다.|
| 53 |[`53_grpc_client.cpp`](53_grpc_client.cpp)|오프라인(grpc++)|C++ 클라이언트에서 NeoGraph gRPC `GraphService`를 호출합니다.|
| 54 |[`54_grpc_checkpoint.cpp`](54_grpc_checkpoint.cpp)|오프라인(grpc++)|`GrpcCheckpointStore` — 정직한 대기 시간 측정을 통해 네트워크 경계를 넘어 원격 `CheckpointStore`입니다.|
| 55 |[`55_grpc_vs_jsonrpc_toolcall.cpp`](55_grpc_vs_jsonrpc_toolcall.cpp)|오프라인(grpc++)|정면 대결: JSON-RPC 대 gRPC에 대한 도구 호출 - "70×는 Nagle 인공물이었습니다" 뒤에 있는 마이크로 벤치입니다.|
| 57 |[`57_grpc_remote_tool.cpp`](57_grpc_remote_tool.cpp)|오프라인(grpc++)|로컬 `neograph::Tool`로 노출되는 다른 프로세스에 있는 도구입니다.|

### 관찰 가능성

| # |파일|설정|그것이 보여주는 것|
|---|------|-------|---------------|
| 49 |[`49_openinference.cpp`](49_openinference.cpp)|오프라인|OpenInference 추적기 어댑터 — `graph.run > node.* > llm.complete`는 하나의 추적 트리(12개 특성)로 표시됩니다. 피닉스 검증. 모의 공급자.|

### 심층 연구 / RAG 변형

| # |파일|설정|그것이 보여주는 것|
|---|------|-------|---------------|
| 25 |[`25_deep_research.cpp`](25_deep_research.cpp)|Anthropic + Crawl4AI 도커|`langchain-ai/open_deep_research`의 C++ 포트입니다. 감독자는 병렬 하위 연구원(각각 자체 ReAct 루프)을 계획하고 인하 보고서를 종합합니다.|
| 28 |[`28_corrective_rag.cpp`](28_corrective_rag.cpp)|오픈AI|CRAG(Yan 외. 2024). 검색 → 등급 → 관련성에 따라 refine(KB) / 정제+웹 / 웹 전용으로 라우팅합니다. `/v1/responses` 내장 도구를 통한 웹 검색.|

### 로컬/하이브리드 LLM 백엔드

| # |파일|설정|그것이 보여주는 것|
|---|------|-------|---------------|
| 31 | [`31_local_transformer.cpp`](31_local_transformer.cpp) | local server (llama.cpp / vLLM) | Point `OpenAIProvider` at `http://localhost:8090`. Two-process split keeps model weights out of the agent's address space. |

### 유리 진열장

| # |파일|설정|그것이 보여주는 것|
|---|------|-------|---------------|
| 11 |[`11_clay_chatbot.cpp`](11_clay_chatbot.cpp)|클레이 + Raylib (`-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON`)|Clay/Raylib UI를 사용한 다중 회전 채팅. Pure-C++ 데스크톱 앱, NeoGraph 백엔드. 모의 또는 `--live`.|
| 35 |[`35_re_agent.cpp`](35_re_agent.cpp)|OpenAI + 기드라 + 기드라-mcp|리버스 엔지니어링 에이전트 — Ghidra를 통해 제거된 바이너리에서 함수 이름 + 요약을 복구합니다. 엔드 투 엔드 검증(matched_score 0.92, 6-fn crackme). [`fox1245/re-agent`](https://github.com/fox1245/re-agent)의 전체 파이프라인.|
| 36 |[`36_classifier_fanout.cpp`](36_classifier_fanout.cpp)|오프라인|5개의 작은 "분류자"(감정/독성/언어/주제/의도)가 Send를 통해 팬아웃되어 병렬로 실행됩니다. 벽 시간 ≒ max(per-classifier)(합계 아님) — 소형 모델 에지 스토리입니다. DistilBERT/MiniLM 패스에 대한 5ms 지연 대기 시간을 모의합니다. 인라인 `[ONNX SWAP-IN]` 블록은 `Ort::Session`를 사용한 30라인 대체를 보여줍니다. 추론 런타임 종속성이 없습니다.|

## 정신 모델 - 3개의 레이어, 중간에 JSON

각 예는 다음 세 가지 설정 중 하나입니다.

1. **내장 노드만 해당** (02, 04, 07, 14): `llm_call` / `tool_dispatch`
/ 모의 제공자 노드 — JSON에서 완전히 연결된 그래프, 아니요
서브클래싱. `create_react_graph()`가 생산하는 것과 가장 가깝습니다.
2. **사용자 정의 `GraphNode` 하위 클래스**(05, 09, 10, 25):
정확한 `run(NodeInput)` 본체 — `ChannelWrite`, `Send` 또는
`Command`부터 `NodeOutput`까지. 여기에서 팬아웃을 보내고
명령 라우팅이 실시간보다 우선합니다.
3. **OpenAI가 아닌 형태용 `SchemaProvider`** (13, 15, 16, 17, 33):
하나의 JSON 스키마는 와이어 모양(Anthropic, Gemini,
OpenAI 응답 API, 원시 WebSocket), 따라서 동일한 `Agent`/
`llm_call` 노드는 서브클래싱 없이 다른 끝점에 도달합니다.

그래프 정의는 JSON 모양(`std::map<std::string, json>`)입니다.
어느 쪽이든 — [Python examples](../bindings/python/examples/)의 예제 14 및 15
동일한 정의가 `json.dumps`를 통해 왕복하는 방법을 보여줍니다.

## API 키 경제

|공급자|예|
|---|---|
|`OPENAI_API_KEY`| 01, 03, 12, 13, 20, 22, 23, 24, 28, 29, 30, 33, 34, 35, 40 |
|`ANTHROPIC_API_KEY`| 15, 16, 17, 18, 19, 25 |
|로컬 서버(키 없음)| 31 |
|**없음**| 02, 04, 05, 06, 07, 08, 09, 10, 14, 21, 27, 36, 37, 38, 39, 41, 42, 43, 44, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57 |

31개의 예제는 API 키 없이 실행됩니다. 즉 "타이어를 걷어차기"입니다.
바닥. 예 21(MCP 팬아웃, 결정적 플래너) 및 27(비동기)
동시성, LLM 대기 시간을 대신하는 `steady_timer`) 특히
토큰을 쓰지 않고 엔진 배관을 시연해 보세요. gRPC 제품군
(52–55, 57)도 키가 없지만 `-DNEOGRAPH_BUILD_GRPC=ON`가 필요합니다.
(`grpc++` / `protoc`); 56(`history_compaction`)은 기본적으로 모의로 설정됩니다.
공급자는 키가 있는 경우에만 OpenAI에 접근합니다.

## CMake 구성 후 다시 실행

빌드된 바이너리는 이름이 지정된 빌드 디렉터리의 루트에 위치합니다.
`example_<short_name>`(예: `example_react_agent`,
`example_custom_graph`). 정확한 이름은 각 `.cpp` 상단에 있습니다.
`Usage:` 아래에 댓글을 남겨주세요.
