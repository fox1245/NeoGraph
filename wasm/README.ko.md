<!-- neograph-i18n: source=wasm/README.md locale=ko source_sha256=7499ea89c1228c6687e0d960ad790d3e072b4157c3c125fdf95cc4eca2656e5f -->
**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

# NeoGraph WASM - 1단계 스모크 빌드

이 디렉터리에는 1단계 WebAssembly 스모크 프로그램이 들어 있습니다. Emscripten에서
WASM 전용 코드 분기 없이 코어 엔진으로 그래프를 컴파일하고 실행할 수 있는지 확인합니다.
현재 경로는 Node.js 스모크 테스트용이며, 브라우저 SDK는 아닙니다.

## 과거 실행 결과

아래 수치는 이전에 로컬에서 실행한 스모크 테스트의 결과입니다. 참고용으로만 유지하며,
생성된 `.wasm`이나 `.js` 파일은 저장소에 포함하지 않습니다. 현재 CI도 WASM 크기
아티팩트를 게시하지 않습니다.

| 측정 항목 | 값 |
|---|---|
| WASM 바이너리 (-O3 + LTO) | **712 KB** |
| Emscripten JS 런타임 | 92 KB |
| JavaScript + WASM 합계 | **약 800 KB** |
| 엔진 소스 변경 | 0줄 |
| 첫 실행 출력 | `doubled = 42, trace = d` ✓ |

비교 기준인 네이티브 NG 빌드는 5.5 MB이며, LangGraph 스택
(langgraph + langchain + openai + httpx + pydantic + langsmith)은 31 MB의
순수 Python 코드입니다. 이 스택은 브라우저 배포를 지원하지 않지만, NeoGraph 코어는
브라우저에 전달하기에 충분히 작습니다.

## 현재 실행되는 항목 (1단계)

- `GraphEngine::compile(json)` - JSON 정의를 실행 가능한 엔진으로 컴파일합니다.
- `engine->run(cfg)` - `InMemoryCheckpointStore`를 사용해 동기 실행합니다.
- `NodeFactory::register_type`로 등록한 사용자 정의 노드를 실행합니다. 노드의 기본
  동작은 C++ 및 Python 경로와 같습니다.
- 1단계 스모크 테스트는 `set_worker_count`, `set_node_cache_enabled`, 리듀서가 있는
  채널, 조건부 엣지, Send 팬아웃, Command 라우팅, 인터럽트를 포함합니다.
- C++20 코루틴(asio의 헤더 전용 `awaitable` 구성 요소)은 Emscripten 5.0에서
  동작합니다.

## 아직 제공하지 않는 항목

| 서브시스템 | 보류 이유 | 단계 |
|---|---|---|
| `neograph_async` (asio 기반 HTTP/WebSocket) | 브라우저에서는 원시 소켓 대신 `fetch`와 네이티브 WebSocket을 사용합니다. | 2 |
| `neograph_llm` (SchemaProvider, OpenAIProvider) | 위 비동기 전송 계층이 필요합니다. | 2 |
| `neograph_postgres` | 브라우저 환경에는 적합하지 않습니다. | - |
| `neograph_mcp` | 서브프로세스 기반이라 브라우저에서 사용할 수 없습니다. | - |
| Embind JS 바인딩 | JavaScript에서 노드 구현을 콜백으로 정의할 수 있게 합니다. | 2-A |

## 빌드 및 실행

```bash
source /opt/emsdk/emsdk_env.sh

emcmake cmake -S . -B build-wasm \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_WASM=ON \
  -DNEOGRAPH_BUILD_ASYNC=OFF \
  -DNEOGRAPH_BUILD_LLM=OFF \
  -DNEOGRAPH_BUILD_MCP=OFF \
  -DNEOGRAPH_BUILD_MCP_CLIENT=OFF \
  -DNEOGRAPH_BUILD_MCP_SERVER=OFF \
  -DNEOGRAPH_BUILD_MCP_HTTP_SERVER=OFF \
  -DNEOGRAPH_BUILD_A2A=OFF \
  -DNEOGRAPH_BUILD_ACP=OFF \
  -DNEOGRAPH_BUILD_GRPC=OFF \
  -DNEOGRAPH_BUILD_POSTGRES=OFF \
  -DNEOGRAPH_BUILD_SQLITE=OFF \
  -DNEOGRAPH_BUILD_UTIL=OFF \
  -DNEOGRAPH_BUILD_EXAMPLES=OFF \
  -DNEOGRAPH_BUILD_TESTS=OFF \
  -DNEOGRAPH_BUILD_BENCHMARKS=OFF \
  -DNEOGRAPH_USE_LIBCURL=OFF
cmake --build build-wasm --target neograph_wasm_smoke -j
node build-wasm/wasm/smoke.js
```

이 타깃은 `neograph_core`를 직접 링크하므로 소스 목록은 이 문서에 복사해 두지 않고
주 CMake 빌드에서 관리합니다. 정상 실행 시 `doubled = 42`와 노드 실행 추적이
출력됩니다.

`compile()`의 기본값은 `worker_count=1`이므로 엔진이 소유한 스레드 풀을 만들지
않습니다. 이 스모크 빌드는 `set_worker_count(N >= 2)`로 병렬 팬아웃을 선택할 수
있도록 Emscripten 스레드 풀 네 개를 활성화하지만, 스모크 자체는 단일 작업자 기본값을
사용합니다. 단일 스레드 빌드가 필요하면 `-sPTHREAD_POOL_SIZE=0`을 사용하면 됩니다.

## 브라우저 지원 현황

현재 저장소에는 브라우저 로더, npm 패키지, Embind API가 없습니다. 따라서 현재 타깃은
Node.js 전용입니다. Emscripten pthreads를 사용하는 브라우저 빌드는 교차 출처 격리
헤더(`Cross-Origin-Opener-Policy: same-origin`,
`Cross-Origin-Embedder-Policy: require-corp`)를 제공하는 웹 서버와 생성된 워커
아티팩트가 추가로 필요합니다.

## 2단계 계획

1. **2-A - Embind JS 바인딩.** `GraphEngine`, `RunConfig`, `ChannelWrite`, `Send`,
   `Command`를 JavaScript에 노출합니다. JavaScript 함수로 노드 구현을 등록하면
   엔진이 각 노드 실행 시 해당 함수를 호출합니다. 예상 기간은 1~2일입니다.

2. **2-B - fetch 기반 HTTP 전송.** `SchemaProvider`가 사용하는 전송 인터페이스를
   제공하고 WASM 빌드에서는 이를 `fetch()`에 연결합니다. 하나의 Provider 코드가
   두 전송 백엔드를 모두 사용할 수 있습니다. 예상 기간은 3~5일입니다.

3. **2-C - npm 패키지.** 자체 빌드 없이 엔진과 JS 바인딩을 설치할 수 있도록
   `@neograph/wasm`으로 배포합니다. 예상 기간은 1~2일입니다.

2단계가 끝나면 브라우저 탭에서 Originator가 발행한 그래프를 실행할 수 있습니다.
각 노드는 `fetch()`를 통해 BYOK 방식으로 Anthropic, OpenAI, Bedrock 키를 호출하고,
transformers.js 또는 내장 AI로 로컬 추론을 수행하며, 결과는 채널을 거쳐 Result
Envelope로 돌아옵니다. 이는 [NeoProtocol](https://github.com/fox1245/NeoProtocol)
Executor 역할의 런타임 부분입니다.
