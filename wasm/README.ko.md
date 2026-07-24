<!-- neograph-i18n: source=wasm/README.md locale=ko source_sha256=9126f4bfed65128d8b676e182ae6e0b13f5d544de24e73018ce2ba8d44fb8093 -->
**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

# NeoGraph WASM — 타당성 스파이크


WebAssembly로 컴파일된 그래프 엔진입니다. 이 디렉토리는
**1단계 스파이크** — 엔진 계층(컴파일, 실행, 실행기,
스케줄러, 코디네이터, 상태, 채널, NodeCache) 빌드 및 실행
Emscripten에서 수정되지 않았습니다.

## 결과

|미터법|값|
|---|---|
|WASM 바이너리(-O3 + LTO)|**712KB**|
|Emscripten JS 런타임|92KB|
|총 선박 크기|**~800KB**|
|엔진 소스 차이|0줄|
|첫 번째 실행 출력|`doubled = 42, trace = d` ✓|

비교를 위해 기본 NG의 총 크기는 5.5MB입니다. LangGraph 스택
(langgraph + langchain + openai + httpx + pydantic + langsmith)는 31MB입니다.
브라우저에 전달하려고 시도조차 하지 않는 순수 Python입니다. NG
L3 캐시 내부에 두 배 이상 들어가며 일반적인 SaaS보다 작습니다.
랜딩 페이지는 이미 이 엔진이 사용하는 것보다 더 많은 JS를 로드하고 있습니다.

## 오늘 실행되는 작업(1단계)

- `GraphEngine::compile(json)` — JSON 정의 → 실행 가능한 엔진.
- `engine->run(cfg)` — InMemoryCheckpointStore를 사용한 동기 실행.
- `NodeFactory::register_type` — 리프를 통해 등록된 사용자 정의 노드
의미 체계는 C++/Python 경로에서 이어집니다.
- 모든 v0.1.6 기능은 깔끔하게 컴파일됩니다: `set_worker_count`,
`set_node_cache_enabled`, 리듀서가 있는 채널, 조건부 에지,
팬아웃, 명령 라우팅, 인터럽트를 보냅니다.
- C++20 코루틴(asio의 헤더 전용 `awaitable` 조각)은 다음에서 작동합니다.
엠스크립트 5.0.

## 일부러 아직 배송하지 않은 것

|서브시스템|연기된 이유|단계|
|---|---|---|
|`neograph_async`(asio를 통한 HTTP/WebSocket)|브라우저는 원시 소켓이 아닌 `fetch` / 기본 WebSocket을 사용합니다.| 2 |
|`neograph_llm`(SchemaProvider, OpenAIProvider)|위의 비동기 전송에 따라 다름| 2 |
|`neograph_postgres`|관련 없는 브라우저| — |
|`neograph_mcp`|하위 프로세스 기반, 브라우저와 관련 없음| — |
|JS 바인딩 포함|JS가 노드 구현을 콜백으로 정의하도록 합니다.|2-A|

## 짓다

```bash
source /opt/emsdk/emsdk_env.sh

em++ -std=c++20 -O3 -flto -fexceptions -pthread \
  -sALLOW_MEMORY_GROWTH=1 -sPTHREAD_POOL_SIZE=4 \
  -DASIO_STANDALONE -DASIO_NO_DEPRECATED \
  -I include -I deps/asio/include -I deps/yyjson \
  wasm/smoke.cpp \
  src/core/json.cpp deps/yyjson/yyjson.c \
  src/core/graph_engine.cpp src/core/graph_compiler.cpp \
  src/core/graph_validator.cpp src/core/tool_dispatch.cpp \
  src/core/graph_coordinator.cpp src/core/graph_executor.cpp \
  src/core/scheduler.cpp src/core/graph_state.cpp \
  src/core/graph_node.cpp src/core/graph_loader.cpp \
  src/core/graph_checkpoint.cpp src/core/store.cpp \
  src/core/provider.cpp src/core/tool.cpp \
  src/core/react_graph.cpp src/core/plan_execute_graph.cpp \
  src/core/deep_research_graph.cpp src/core/node_cache.cpp \
  -o wasm/smoke.js
```

`node wasm/smoke.js`로 실행합니다. 브라우저 플래그가 필요하지 않습니다.

`compile()`는 기본값을 프로비저닝하므로 `-pthread`가 필요합니다.
thread_pool의 크기는 `hardware_concurrency()`입니다. 단일 스레드 WASM는
또한 가능합니다 — `-sPTHREAD_POOL_SIZE=0`를 전달하고 호출합니다.
`run()` 이전에 `engine->set_worker_count(1)`.

## 2단계 스케치

1. **2-A — JS 바인딩을 포함합니다.** `GraphEngine`, `RunConfig`,
`ChannelWrite`, `Send`, `Command`를 JS로 변환합니다. JS 함수는 등록할 수 있습니다.
노드 구현으로서의 자체; 엔진은 JS를 다시 호출하여
각 노드 실행. 1~2일 예상됩니다.

2. **2-B — 가져오기 기반 HTTP 전송.** 전송 제공
`SchemaProvider`가 사용하는 인터페이스; WASM 빌드가 연결됩니다.
`fetch()`로. 동일한 공급자 코드는 두 백엔드 중 하나를 대상으로 합니다. 추정된
3~5일.

3. **2-C — npm 패키지.** 앱이 다음을 수행할 수 있도록 `@neograph/wasm`로 게시합니다.
자체 빌드가 없는 엔진 + JS 바인딩 `npm install`.
1~2일 예상됩니다.

2단계 이후 엔진은 Originator에서 발행한 그래프를 완전히 실행할 수 있습니다.
브라우저 탭 — BYOK 호출을 종료합니다. Anthropic / OpenAI / Bedrock 키를 통해
`fetch()`, Transformers.js / 로컬 추론을 위한 AI 내장 및
결과는 채널을 통해 결과 봉투로 다시 흐릅니다. 그게 다야
런타임 측면
[NeoProtocol](https://github.com/fox1245/NeoProtocol) 실행자 역할.
