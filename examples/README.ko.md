<!-- neograph-i18n: source=examples/README.md locale=ko source_sha256=ce7e094fabd0d6304f5961a14e5ca75acbfa89a345a01c74939bf55595ae91f6 -->
# C++ API 예제
**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)


## 타입 C++ 전환 상태

현재 C++ recipe는 다섯 타입 SDK family의 소유된 `ProviderRequest`, 순서 있는
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
수행하며 SDK package는 alpha `0.1.1` (interface/shared ABI 4)입니다.

보존된 interface-3 model-free C++ E2E 실행에서는 번호가 있는 타깃 39개를 검증했습니다. finite offline 29개와
실제 MCP/ACP/A2A/Harness 및 gRPC graph/checkpoint/tool 경로입니다. gRPC-vs-JSON-RPC
측정 예제도 실행했지만 반환값을 검사하지 않아 행동 E2E 통과로 세지 않습니다.
live/외부 모델 경로 22개와 비활성 Clay GUI는 미검증이며 공개 vendor 요청이나 새 grant는 없었습니다.
아래 과거 측정은 새 전환의 qualification이 아닙니다. live에는 키/네트워크/모델 접근과 비용이 필요합니다.
키, prompt, artifact는 비공개로 유지하고 민감한 envelope/native 출력을 공개 log에 내보내지 마세요.
native archive는 owner-private 인증 custody이며 암호화나 vendor issuer 인증이 아닙니다.

현재 interface-4 증거는 별도입니다. Linux x86_64에서 native research 복구,
ToT/Forge/rewrite helper와 A2A 0.3/1.0 peer를 실행했고, 설치된 Python application 11개가
분리된 실행 묶음에서 credential-free localhost 요청 48개를 수행했습니다.
추적된 evolution file 모드와 Plan resume도 아래와 같이 실행했습니다.
보존된 전체 C++ suite를 재실행한 결과가 아닌 범위가 한정된 local 실행입니다.
[SDK interface-4 실행 기록](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/CONFORMANCE.md#interface-4-execution-record)을 참조하세요.
새 hosted vendor, Windows/macOS/ARM64, HTTP/3 또는 sanitizer 검증은 주장하지 않으며,
remote CI와 공개 배포는 아직 대기 중입니다.




번호가 있는 예제는 NeoGraph 엔진 API를 다루며 Core와 Program quickstart도 포함합니다.
대부분은 이 디렉터리의 단일 소스 파일입니다. Docker Compose가 필요한
[`26_postgres_react_hitl/`](26_postgres_react_hitl/)도 있습니다.
예제를 프로젝트에 복사하고 `neograph::core` 및 필요한 구성요소를 링크하세요.

## 빌드

기본 CMake 구성은 활성화된 구성요소가 지원하는 예제를 빌드합니다.
Program quickstart와 Program 기반 예제에는
`-DNEOGRAPH_BUILD_PROGRAM=ON`이 필요합니다. gRPC와 Python 바인딩은
선택 사항이며 해당 옵션을 켜지 않으면 관련 예제가 생략됩니다.

```bash
cmake -S . -B build -DNEOGRAPH_BUILD_EXAMPLES=ON
cmake --build build -j$(nproc)
```

Program 기반 예제와 A2A 예제를 포함하려면 다음 구성요소도 활성화하세요.

```bash
cmake -S . -B build \
  -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_A2A=ON
cmake --build build -j$(nproc)
```

예제를 건너뛰려면 `-DNEOGRAPH_BUILD_EXAMPLES=OFF`를 전달하세요. 추가
의존성(Crawl4AI Docker, Postgres, MCP 서버, Clay+Raylib)이 필요한 예제는
명시적인 CMake 옵션 또는 런타임 프로브로 제어됩니다. 아래의 '설정' 열을
참조하세요.

## 설정

실제 LLM에 연결하는 예제는 cppdotenv를 통해 현재 디렉터리의 `.env`를 자동으로
불러옵니다. 모든 라이브 예제는 다음 키 형식을 사용합니다.

```
OPENROUTER_API_KEY=sk-or-...
```

아래 표의 설정 열에 별도 표시가 없는 예제는 API 키가 필요하지 않습니다.
`MockProvider` 또는 순수한 모의 노드만 사용합니다.

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
|[`01_react_agent.cpp`](01_react_agent.cpp)|실제 LLM + 계산기 도구를 사용한 ReAct 루프. **`OPENROUTER_API_KEY`가 필요합니다.**|
|[`14_plan_executor.cpp`](14_plan_executor.cpp)|계획 → 병렬 하위 작업 → 해결사, 체크포인트 저장소를 통한 충돌 복구. API 키가 없습니다.|

기본 예제를 확인한 뒤에는 아래 색인을 기능별로 살펴보세요. 파일 번호순이 아니라
기능과 사용 목적에 따라 그룹화되어 있습니다.

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
| 56 |[`56_history_compaction.cpp`](56_history_compaction.cpp)|오프라인(선택적 OpenRouter)|제한된 메시지 창 — 기록이 예산을 초과하면 삭제된 접두사가 LLM로 작성된 요약으로 대체됩니다. 기본적으로 모의 공급자.|

### Real LLM — 공급자, 도구, ReAct

| # |파일|설정|그것이 보여주는 것|
|---|------|-------|---------------|
| 01 |[`01_react_agent.cpp`](01_react_agent.cpp)|OpenRouter|ReAct 루프: `llm_call` ⇔ `tool_dispatch`(`has_tool_calls` 조건부 포함) 계산기 도구.|
| 12 |[`12_rag_agent.cpp`](12_rag_agent.cpp)|OpenRouter|OpenRouter 호환 embedding + 메모리 내 코사인 검색을 사용하는 RAG.|
| 13 |[`13_openrouter_responses_sse.cpp`](13_openrouter_responses_sse.cpp)| OpenRouter | 타입 Responses SSE 요청, 순서 있는 `sp::Event` 관찰 및 소유된 `sp::Outcome`. |
| 34 |[`34_openrouter_responses_tools_sse.cpp`](34_openrouter_responses_tools_sse.cpp)| OpenRouter | 일곱 hosted-tool 섹션을 모두 타입 SSE로 실행하며 전체 Outcome과 순서 있는 wire 관찰을 보존합니다. |
| 29 |[`29_responses_envelope.cpp`](29_responses_envelope.cpp)|OpenRouter|한 번의 tool-call 요청에 대한 원시 `/api/v1/responses` JSON envelope 덤프.|
| 30 |[`30_reasoning_effort.cpp`](30_reasoning_effort.cpp)|OpenRouter|고정 DeepSeek 모델의 reasoning effort를 한 prompt에서 sweep합니다.|

### 추론 패턴

| # |파일|설정|무늬|
|---|------|-------|---------|
| 15 |[`15_reflexion.cpp`](15_reflexion.cpp)|OpenRouter|생성기 ⇔ 비평가가 ACCEPT할 때까지 반복하는 Reflexion.|
| 16 |[`16_tree_of_thoughts.cpp`](16_tree_of_thoughts.cpp)|OpenRouter|후보 thought를 생성·평가·선별·확장하는 Tree of Thoughts.|
| 17 |[`17_self_ask.cpp`](17_self_ask.cpp)|OpenRouter|다중 홉 추론을 위한 명시적 후속 질문 분해 Self-Ask.|
| 18 |[`18_multi_agent_debate.cpp`](18_multi_agent_debate.cpp)|OpenRouter|Researcher / Skeptic / Judge 세 system prompt와 공유 transcript.|
| 19 |[`19_rewoo.cpp`](19_rewoo.cpp)|OpenRouter|REWOO planner placeholder, 병렬 tool worker, solver 합성.|

### 지속성 및 HITL

| # |파일|설정|그것이 보여주는 것|
|---|------|-------|---------------|
| 04 |[`04_checkpoint_hitl.cpp`](04_checkpoint_hitl.cpp)|오프라인|`interrupt_before` 결제 노드, 체크포인트 유지, 운영자 승인 후 재개. 모의 공급자.|
| 14 |[`14_plan_executor.cpp`](14_plan_executor.cpp)|오프라인|시뮬레이션된 중간 팬아웃 오류가 있는 Plan-and-Executor - 체크포인트 재생은 실패한 형제만 다시 실행합니다. 보류 중인 쓰기 기계가 작동 중입니다.|
| 26 |[`26_postgres_react_hitl/`](26_postgres_react_hitl/)|OpenRouter + Postgres + Crawl4AI|프로세스 중단 심층 연구 HITL — PG 지원 체크포인트는 보고와 재개 사이에 `exit`를 유지합니다. Docker-Compose 기반.|
| 41 |[`41_resume_if_exists_chat.cpp`](41_resume_if_exists_chat.cpp)|오프라인|LangGraph 스타일의 다중 턴 채팅 — `resume_if_exists`는 이전 체크포인트를 다시 로드하고 새 턴을 추가합니다. 모의 공급자.|
| 48 |[`48_sqlite_checkpoint.cpp`](48_sqlite_checkpoint.cpp)|오프라인|SQLite `:memory:` checkpoint/resume와 thread 격리; 파일·process restart 영속성 검증은 아님.|

### MCP(모델 컨텍스트 프로토콜)

| # |파일|설정|그것이 보여주는 것|
|---|------|-------|---------------|
| 03 |[`03_mcp_agent.cpp`](03_mcp_agent.cpp)|OpenRouter + MCP HTTP 서버|스트리밍 가능한 http MCP 서버에서 도구를 검색하고 ReAct 루프를 구동하세요.|
| 22 |[`22_mcp_stdio.cpp`](22_mcp_stdio.cpp)|OpenRouter + Python stdio 스크립트|03과 동일하지만 MCP 서버는 stdin/stdout를 통한 하위 하위 프로세스이며 네트워크 스택이 없습니다.|
| 23 |[`23_mcp_multi.cpp`](23_mcp_multi.cpp)|OpenRouter + 서버 2개|하나의 에이전트, 두 개의 MCP 서버(HTTP + stdio), 도구가 하나의 목록으로 병합되었습니다. LLM는 두 가지 모두를 투명하게 선택합니다.|
| 21 |[`21_mcp_fanout.cpp`](21_mcp_fanout.cpp)|MCP HTTP 서버(LLM 없음)|고정 planner가 MCP 호출마다 Send 하나를 만들고 `make_parallel_group`으로 동시에 실행합니다. 모델 호출은 없지만 MCP 서버에 연결할 수 있어야 합니다.|
| 20 |[`20_mcp_hitl.cpp`](20_mcp_hitl.cpp)|OpenRouter + MCP HTTP 서버|`interrupt_before` 모든 MCP 도구 호출 — 운영자는 보류 중인 도구 이름 + 인수를 확인하고 승인하고 재개합니다.|
| 24 |[`24_mcp_feedback.cpp`](24_mcp_feedback.cpp)|OpenRouter + MCP HTTP 서버|교환원은 상담원의 답변 초안을 읽고 피드백을 입력합니다. 두 번째 실행에서는 해당 피드백을 새로운 대화 컨텍스트로 통합합니다.|

### 비동기, 동시성, 성능

| # |파일|설정|그것이 보여주는 것|
|---|------|-------|---------------|
| 27 |[`27_async_concurrent_runs.cpp`](27_async_concurrent_runs.cpp)|오프라인|3개의 에이전트가 `engine->run_async()`를 통해 하나의 `io_context` 스레드에서 인터리브를 실행합니다. 벽은 3×50ms 대신 50ms입니다. 4단계 비동기 엔드투엔드.|
| 40 |[`40_react_async_streaming.cpp`](40_react_async_streaming.cpp)| OpenRouter | 타입 공급자 이벤트를 사용하는 비동기 ReAct. text delta는 표시용 투영이며 native history가 아닙니다. |
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
| 25 |[`25_deep_research.cpp`](25_deep_research.cpp)|OpenRouter DeepSeek + Crawl4AI 도커|`langchain-ai/open_deep_research`의 C++ 포트입니다. 감독자는 병렬 하위 연구원(각각 자체 ReAct 루프)을 계획하고 보고서를 종합합니다.|
| 28 |[`28_corrective_rag.cpp`](28_corrective_rag.cpp)|OpenRouter|CRAG(Yan 외. 2024). 검색 → 등급 → 관련성에 따라 refine(KB) / 정제+웹 / 웹 전용으로 라우팅합니다. `/api/v1/responses` 내장 도구를 통한 웹 검색.|

### 로컬/하이브리드 LLM 백엔드

| # |파일|설정|그것이 보여주는 것|
|---|------|-------|---------------|
| 31 | [`31_local_transformer.cpp`](31_local_transformer.cpp) | llama.cpp / vLLM | `http://localhost:8090`의 타입 Chat 클라이언트. 모델 가중치는 에이전트 프로세스 밖에 있습니다. |

### 유리 진열장

| # |파일|설정|그것이 보여주는 것|
|---|------|-------|---------------|
| 11 |[`11_clay_chatbot.cpp`](11_clay_chatbot.cpp)|클레이 + Raylib (`-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON`)|Clay/Raylib UI를 사용한 다중 회전 채팅. Pure-C++ 데스크톱 앱, NeoGraph 백엔드. 모의 또는 `--live`.|
| 35 |[`35_re_agent.cpp`](35_re_agent.cpp)|OpenRouter + Ghidra + ghidra-mcp|리버스 엔지니어링 에이전트: Ghidra로 심볼이 제거된 바이너리의 함수 이름과 요약을 복원한다. 과거 종단 간 결과는 함수 6개인 crackme에서 matched_score 0.92였다. 전체 파이프라인은 별도의 비공개 `fox1245/re-agent` 저장소에서 관리한다.|
| 36 |[`36_classifier_fanout.cpp`](36_classifier_fanout.cpp)|오프라인|5개의 작은 "분류자"(감정/독성/언어/주제/의도)가 Send를 통해 팬아웃되어 병렬로 실행됩니다. 벽 시간 ≒ max(per-classifier)(합계 아님) — 소형 모델 에지 스토리입니다. DistilBERT/MiniLM 패스에 대한 5ms 지연 대기 시간을 모의합니다. 인라인 `[ONNX SWAP-IN]` 블록은 `Ort::Session`를 사용한 30라인 대체를 보여줍니다. 추론 런타임 종속성이 없습니다.|

예제 35에는 `bridge_mcp_ghidra.py` 스크립트 경로를 지정하는 `GHIDRA_MCP_BRIDGE`가 필요하다. `GHIDRA_MCP_PYTHON`은 인터프리터를 선택하며 기본값은 `python3`이다. `GHIDRA_SERVER_URL`은 플러그인 엔드포인트를 선택하며 기본값은 `http://127.0.0.1:18080/`이다. 실행 전에 Ghidra와 MCP 플러그인을 시작해야 한다. 이 예제는 `OPENROUTER_API_KEY`도 필요하며 유료 모델을 호출한다. 위 점수는 과거 관측값으로, 새 실행 결과나 일반적인 정확도 보장이 아니다.

## 보존된 예제 계약

이 소스 계약은 SDK interface 4를 사용합니다. 위 C++ 실행 기록은 과거 증거이며
interface-4 검증이나 새로운 live 호출이 아닙니다. family별 제어는 `ProviderControls`의
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

이 다섯 명령은 모델 호출 3 + 4 + 14 + 1 + 4 = 26회와 설정된 출력 토큰 212,992개로 제한되며 입력 토큰은 별도입니다. 달러 가격을 입증하거나 지출을 승인하지 않습니다. 소유자가 승인한 모델/경로를 사용하고 로그는 비공개로 유지하세요. 원래 작업량 qualification은 shape override만 빼고 명시적 cap/재요청 선택은 기록해 둡니다. 이 명시적 설정에서 전체 ToT + Baldwin + 서버는 최대 175회 호출 / 설정된 출력 토큰 1,433,600개입니다.

ToT는 비어 있지 않은 최종 `EXPR`/`CHECK`를 출력해야 합니다. 실제 식을 독립적으로 계산해 4 하나, 7 하나, 8 두 개를 정확히 사용하고 결과가 24인지 확인하세요. 모델의 점수는 산술 oracle이 아닙니다. Baldwin은 두 모드 모두 실제 pipeline fitness와 hit 수를 보여야 합니다. 특정 진화 trace를 강제하거나 live learner를 오프라인 oracle로 대체하지 마세요. 서버는 비어 있지 않은 모델/judge 출력과 직렬화/provider 실패 없이 선택한 네 번째 턴에 도달해야 합니다. 30은 네 가지 effort를 관찰하는 sweep입니다. reasoning 토큰, 실행 시간, 정확도의 단조성은 약속하지 않으며 빈 답변은 Outcome을 보존한 채 exit 2를 반환합니다. 29는 답변 텍스트 없이 tool call만 반환하는 것이 정상일 수 있습니다. 이는 envelope inspection이지 날씨 답변 benchmark가 아닙니다. Provider 실패는 계속 nonzero입니다.

### 체크포인트 및 진화

예제 08은 terminal checkpoint를 fork한 뒤 새 user turn을 시작하는 기존 흐름을 유지합니다.
일시 정지한 reviewer의 resume 예제가 아닙니다. 예제 14는 실제 executor 횟수로 절약량을 계산합니다.
최초 다섯 호출 뒤 실패한 sibling 하나만 다시 실행하면 네 호출을 절약합니다.

예제 54는 smoke/file 모드를 선택하기 전에 `pnoop`를 등록합니다. 저장소 root에서
추적된 seed/task 파일을 사용하세요:

```bash
./build/example_evolution --smoke
./build/example_evolution examples/54_evolution_seed.json examples/54_evolution_task.json
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

## 정신 모델 - 3개의 레이어, 중간에 JSON

각 예는 다음 세 가지 설정 중 하나입니다.

1. **내장 노드만 해당** (02, 04, 07, 14): `llm_call` / `tool_dispatch`
/ 모의 제공자 노드 — JSON에서 완전히 연결된 그래프, 아니요
서브클래싱. `create_react_graph()`가 생산하는 것과 가장 가깝습니다.
2. **사용자 정의 `GraphNode` 하위 클래스**(05, 09, 10, 25):
정확한 `run(NodeInput)` 본체 — `ChannelWrite`, `Send` 또는
`Command`부터 `NodeOutput`까지. 여기에서 팬아웃을 보내고
명령 라우팅이 실시간보다 우선합니다.
3. **타입 공급자 요청과 Outcome** (13, 15, 16, 17): SDK admission, 순서 있는 이벤트, 불변 Outcome을 사용합니다. descriptor interpreter와 WebSocket adapter는 없습니다.

그래프 정의는 JSON 형태(`std::map<std::string, json>`)입니다.
[Python examples](../bindings/python/examples/)도 같은 topology 형식을 사용합니다.
공급자 요청과 Outcome은 topology JSON과 별개인 타입 객체입니다.

## API 키 경제

|공급자|예|
|---|---|
|`OPENROUTER_API_KEY`| 01, 03, 12, 13, 15, 16, 17, 18, 19, 20, 22, 23, 24, 25, 28, 29, 30, 34, 35, 40 |
|로컬 서버(키 없음)| 31 |
|**없음**| 02, 04, 05, 06, 07, 08, 09, 10, 14, 21, 27, 36, 37, 38, 39, 41, 42, 43, 44, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57 |

예제 25와 26은 local Crawl4AI도 사용합니다. 현재 secure Docker image에는
비어 있지 않은 `CRAWL4AI_API_TOKEN`이 필요합니다. 예제 26의 `.env.example`을 보세요.

31개 예제는 API 키 없이 실행할 수 있습니다. 예제 21은 고정 MCP planner,
27은 `steady_timer`로 모델 지연을 대신하며 token을 쓰지 않고 engine 동작을 보여줍니다.
gRPC 예제(52–55, 57)도 키가 없지만 `-DNEOGRAPH_BUILD_GRPC=ON`과 `grpc++`/`protoc`가 필요합니다.
56(`history_compaction`)은 기본 mock provider이며 키가 있을 때만 OpenRouter에 접근합니다.

## CMake 구성 후 다시 실행

빌드된 바이너리는 이름이 지정된 빌드 디렉터리의 루트에 위치합니다.
`example_<short_name>`(예: `example_react_agent`,
`example_custom_graph`). 정확한 이름은 각 `.cpp` 상단에 있습니다.
`Usage:` 아래에 댓글을 남겨주세요.

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
