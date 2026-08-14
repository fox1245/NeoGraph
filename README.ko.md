<!-- neograph-i18n: source=README.md locale=ko source_sha256=6c67c286aae76e1f4dcc6a25b9e04af02b9d362083721bce943f3fddb381b168 -->
<p align="center">
  <h1 align="center">NeoGraph</h1>
  <p align="center">
    <strong>C++ 그래프 에이전트 엔진 — Python 바인딩 포함.</strong><br>
    LangGraph 수준의 기능 · 5&nbsp;µs 엔진 오버헤드 · 라즈베리파이에 맞는 단일 정적 바이너리.
  </p>
</p>

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

<p align="center">
  <a href="https://pypi.org/project/neograph-engine/"><img alt="PyPI" src="https://img.shields.io/pypi/v/neograph-engine?label=pip%20install%20neograph-engine&color=blue"></a>
  <a href="https://pypi.org/project/neograph-engine/"><img alt="Python versions" src="https://img.shields.io/pypi/pyversions/neograph-engine"></a>
  <a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-MIT-green.svg"></a>
</p>

<p align="center">
  <a href="#quick-start">빠른 시작</a> &middot;
  <a href="#use-from-a-cmake-project">CMake</a> &middot;
  <a href="#python">Python</a> &middot;
  <a href="docs/concepts.md">개념</a> &middot;
  <a href="examples/README.md">예제</a> &middot;
  <a href="docs/troubleshooting.md">문제 해결</a> &middot;
  <a href="docs/reference-en.md">API 참조</a> &middot;
  <a href="#vs-langgraph">vs LangGraph</a>
</p>

---

<p align="center">
  <a href="docs/videos/neograph-promo.mp4">
    <img src="docs/images/neograph-promo.gif" alt="NeoGraph 프로모션 — 5µs 엔진 오버헤드, 10K 동시에서 5.5MB RSS, 1.2MB 정적 바이너리, 라즈베리파이 적합" width="900">
  </a>
</p>

## NeoGraph란?

NeoGraph는 LangGraph 수준의 기능을 C++로 제공하는 **C++20 그래프 기반 에이전트 오케스트레이션 엔진**입니다. 에이전트 워크플로를 JSON으로 정의하고, 병렬 팬아웃으로 실행하며, 시간 여행 디버깅과 인간 개입(HITL)을 위한 상태를 체크포인트할 수 있습니다. LLM 제공자도 자유롭게 연결할 수 있으며 Python 없이 사용할 수 있습니다.

```cpp
#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/graph/react_graph.h>

auto provider = neograph::llm::OpenAIProvider::create({
    .api_key = "sk-...", .default_model = "gpt-4o-mini"
});
auto engine = neograph::graph::create_react_graph(provider, std::move(tools));

neograph::graph::RunConfig config;
config.input = {{"messages", json::array({{{"role","user"},{"content","Hello!"}}})}};
auto result = engine->run(config);
```

위 에이전트는 실제로 엔진이 실행하는 JSON일 뿐이다 — JSON을 바꾸면 다른 에이전트가 된다 ([`docs/concepts.md`](docs/concepts.md) 참조):

```json
{
  "schema_version": 1,
  "channels": { "messages": {"reducer": "append"}, "__route__": {"reducer": "overwrite"} },
  "nodes": {
    "planner":    {"type": "llm_call"},
    "researcher": {"type": "tool_dispatch"},
    "classifier": {"type": "intent_classifier", "routes": ["deep_dive", "summarize"]}
  },
  "edges": [
    {"from": "__start__", "to": "planner"},
    {"from": "planner", "condition": "has_tool_calls",
     "routes": {"true": "researcher", "false": "classifier"}},
    {"from": "researcher", "to": "planner"},
    {"from": "classifier", "condition": "route_channel",
     "routes": {"deep_dive": "__end__", "summarize": "__end__"}}
  ]
}
```

**NeoGraph는 C++을 위한 유일한 그래프 에이전트 엔진이다.** 로보틱스, 임베디드 시스템, 게임, 고빈도 트레이딩, Python이 선택지가 아닌 모든 곳에서 에이전트를 만든다면 — 바로 이것이다.

## 네 축

각 항목은 명령 한 번으로 확인할 수 있습니다. 라이브 LLM 예제만 API 키가 필요합니다.

|   | 축 | 측정값 | 상세 |
|---|---|---|---|---|
| ⚡ | **성능** | 5 µs 엔진 오버헤드 · 10 K 동시에 5.5 MB · p99 7 µs @ 10 K (1 CPU 샌드박스) | [성능 심층 분석](docs/performance-deep-dive.md) |
| 🧬 | **자기 진화** | LLM 평가자 → `graph_def` 실시간 교체 · 5명 고객 → 3개 창발 토폴로지 군집 | [self_evolving_chatbot](examples/cookbook/self_evolving_chatbot/) |
| 🔌 | **내장형 준비** | 1.2 MB 제거된 정적 바이너리 · `libc.so.6`만 · RPi Zero 2W에서 실행 | [임베디드 / 로보틱스](docs/performance-deep-dive.md#what-the-numbers-mean-for-embedded--robotics) |
| 🪶 | **가벼움** | 2개 직접 wheel 의존성 · 1K-고객 멀티 테넌트 → 29 MB · t2.micro 친화적 | [multi_tenant_chatbot](examples/cookbook/multi_tenant_chatbot/) |

### 벤치마크

동일 토폴로지, I/O 없는 엔진 오버헤드 — 순수 노드 디스패치 + 상태 쓰기 + 리듀서 호출 (µs/반복, 낮을수록 좋음):

| 프레임워크 | `seq` (3-노드) | `par` (팬아웃 5) | vs. NeoGraph |
|---|--:|--:|--:|
| **NeoGraph master** | **5.0 µs** | **11.8 µs** | 1× |
| Haystack 2.28 | 144 µs | 290 µs | 29× |
| pydantic-graph 1.85 | 236 µs | 286 µs | 47× |
| LangGraph 1.1.9 | 657 µs | 2,349 µs | 131× |
| LlamaIndex 0.14 | 1,780 µs | 4,684 µs | 356× |
| AutoGen 0.7.5 | 3,209 µs | 7,293 µs | 642× |

N=10,000 동시 (1 CPU / 512 MB 샌드박스): NeoGraph 52 ms / 7 µs p99 / 5.5 MB · LangGraph 23.4 s / 416 MB · LlamaIndex & AutoGen OOM-종료.
전체 행렬 + 방법론: [`docs/performance-deep-dive.md`](docs/performance-deep-dive.md) · [`benchmarks/README.md`](benchmarks/README.md).

<a id="quick-start"></a>
## 빠른 시작

**요구사항** — C++20 컴파일러(GCC 13.3은 코어만 지원하며, 전체 기능에는 GCC 14.2+ / Clang 18+ / MSVC 2022 권장), CMake 3.16+, Python 3(빌드 시 코드 생성). 기본 옵션으로 구성하려면 OpenSSL, SQLite3, libpq, libcurl **개발** 패키지도 필요합니다. 런타임 `.so`만으로는 `find_package`를 충족할 수 없습니다.

```bash
# Ubuntu / Debian
sudo apt install libssl-dev libsqlite3-dev libpq-dev libcurl4-openssl-dev
# macOS (SQLite ships with the system)
brew install openssl libpq curl
```

Postgres / SQLite 체크포인트나 HTTP/2 백엔드가 필요 없다면 해당 패키지를 설치하지 않고 `-DNEOGRAPH_BUILD_POSTGRES=OFF -DNEOGRAPH_BUILD_SQLITE=OFF -DNEOGRAPH_USE_LIBCURL=OFF`로 구성하세요.

**플랫폼** — Linux x86_64 **GA** (기준, 429/429 ctest, 새니타이저 깨끗); macOS arm64, Linux ARM64, Windows MSVC 2022 **beta**. 플랫폼별 근거는 [`CHANGELOG.md`](CHANGELOG.md).

```bash
git clone https://github.com/fox1245/NeoGraph.git
cd NeoGraph
cmake -S . -B build
cmake --build build -j$(nproc)

# Run an example — no API key needed:
./build/example_custom_graph      # mock ReAct agent
./build/example_parallel_fanout   # parallel fan-out/fan-in
./build/example_send_command      # dynamic Send + Command routing
```

실제 LLM에 대해 실행 — API를 사용하는 모든 예제는 현재 디렉터리의 `.env`를 자동으로 불러온다 (내장 `cppdotenv`):

```bash
echo "OPENAI_API_KEY=sk-..." > .env
./build/example_react_agent
```

<a id="use-from-a-cmake-project"></a>
## CMake 프로젝트에서 사용

`pip install`은 Python 전용 (C++ 헤더 없음). C++의 경우 `FetchContent`가 CMake의 `pip install`처럼 동작:

```cmake
include(FetchContent)
FetchContent_Declare(NeoGraph
    GIT_REPOSITORY https://github.com/fox1245/NeoGraph.git
    GIT_TAG        master)
# Optional: trim heavy components you don't need.
set(NEOGRAPH_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(NEOGRAPH_BUILD_PYBIND   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(NeoGraph)

add_executable(my_agent main.cpp)
target_link_libraries(my_agent PRIVATE neograph::core neograph::llm neograph::a2a)
```

통합은 이것이 전부다. 처음이라면? [**처음 30분에 걸리는 5가지 함정**](docs/troubleshooting.md) (채널 접근자 모양, `neograph::graph::` 하위 네임스페이스, `<httplib.h>` OpenSSL 매크로, GCC 13 코루틴 ICE, …)이 디버깅 시간을 아껴줄 것이다. 전체 빌드 옵션과 CMake 대상: [`docs/reference-en.md`](docs/reference-en.md).

## Python

같은 C++ 엔진을 `pip` 설치 가능하고, 노트북, Gradio, FastAPI 서비스에서 구동:

```bash
pip install neograph-engine
```

```python
import neograph_engine as ng

definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "demo",
    "channels": {"messages": {"reducer": "append"}},
    "nodes":    {"llm": {"type": "llm_call"}},
    "edges":    [{"from": ng.START_NODE, "to": "llm"},
                 {"from": "llm", "to": ng.END_NODE}],
}
engine = ng.GraphEngine.compile(definition, ng.NodeContext())
result = engine.run(ng.RunConfig(thread_id="t1", input={"messages": [...]}))
```

배포당 20개 wheel + sdist (Linux x86_64/aarch64, macOS arm64, Windows x64 · Python 3.9–3.13). 전체 안내 — 실제 LLM으로 ReAct, 비동기, 사용자 정의 리듀서, LangGraph 차이점 목록, 관측 가능성, Docker 없는 배포: [`docs/python-binding.md`](docs/python-binding.md).

## 기능

**핵심 엔진 (`neograph::core`)** — JSON 정의 그래프 (워크플로 변경 시 재컴파일 불필요) · Pregel 슈퍼스텝 실행 (순환 포함) · 병렬 팬아웃/팬인 · `Send` (동적 팬아웃) + `Command` (라우팅+상태 덮어쓰기) · 체크포인트 + HITL (`interrupt_before/after`, `resume()`, `NodeInterrupt`) · `get_state` / `update_state` / `fork` / 시간 여행 · 재시도 정책 · 스트림 모드 · 서브그래프 · 의도 라우팅 · 스레드 간 `Store` · `NodeFactory`를 통한 사용자 정의 노드 · 비동기 네이티브 (`run_async` / `run_stream_async`) · 협력적 `CancelToken` · 이력 압축 · 노드별 캐시 · `NodeFactory::export_schema()` (버전 고정 비주얼 편집기 구동). 내장 **OpenInference 추적기**, 별도 링크 불필요.

**LLM 제공자 (`neograph::llm`)** — `OpenAIProvider` (OpenAI/Groq/Together/vLLM/Ollama — 모든 OpenAI 호환 API) · `SchemaProvider` (Claude, Gemini, 또는 JSON 스키마를 통한 모든 사용자 정의 벤더) · 스트리밍이 포함된 ReAct `Agent` 루프.

**통합** — MCP 클라이언트 (`neograph::mcp`, HTTP + stdio) · 로컬 MCP 서버 (`neograph::mcp_server`, stdio) · 선택적 Streamable HTTP 서버 (`neograph::mcp_http_server`) · SQLite Harness 레코드 (`neograph::mcp_sqlite`) · 컴파일러 기반 다중 작업자 [Harness MCP](docs/HARNESS_MCP.md) · Agent-to-Agent (`neograph::a2a`, 서버 + 클라이언트 + 호출자 노드) · Agent Client Protocol (`neograph::acp`, 편집기 구동) · gRPC 서비스 (`neograph::grpc`, 선택적) · 비동기 HTTP/HTTPS/WS + SSE (`neograph::async`).

**영속 상태** — 하나의 `CheckpointStore` 인터페이스 뒤에 `PostgresCheckpointStore`, `SqliteCheckpointStore`, `InMemoryCheckpointStore` (모두 Python 바인딩), 그리고 변경 불가능한 Harness 아티팩트와 재시작 안전 실행 레코드를 위한 `SqliteHarnessRecordStore`. `neograph::util`에 잠금 없는 `RequestQueue` + `AsyncTool`.

`NEOGRAPH_BUILD_MCP`는 두 MCP 역할의 호환성 우산으로 유지. 좁은 빌드에는 `NEOGRAPH_BUILD_MCP_CLIENT` 또는 `NEOGRAPH_BUILD_MCP_SERVER` 사용; stdio 서버 전용 대상은 `neograph::async`나 OpenSSL이 필요 없음. 원격 HTTP에는 `NEOGRAPH_BUILD_MCP_HTTP_SERVER`를 명시적으로 활성화.

전체 기능 목록과 55개 이상의 실행 가능한 예제: [`examples/README.md`](examples/README.md).

## 아키텍처

`GraphEngine`은 네 개의 목적별, 독립적으로 단위 테스트된 클래스에 위임하는 얇은 슈퍼스텝 조율자:

- **`GraphCompiler`** — 순수 `JSON → CompiledGraph` 파서.
- **`Scheduler`** — 신호 디스패치 라우팅 + 장벽(barrier) 누적.
- **`NodeExecutor`** — 재시도 루프, 병렬 팬아웃 (`asio::make_parallel_group`), `Send` 디스패치.
- **`CheckpointCoordinator`** — `(store, thread_id)` 파사드 뒤의 저장 / 재개 / 대기 쓰기.

`neograph::core`는 네트워크 의존성이 전혀 없다 (`yyjson` + 헤더 전용 `asio`); `httplib`은 `llm`/`mcp`에 PRIVATE으로 유지되며 사용자 코드에 절대 노출되지 않는다. 두 가지 동시성 모델이 기본 제공 — 스레드-당-에이전트 (동기)와 코루틴-비동기 (하나의 `asio::io_context`에 수천 에이전트). 상세: [`docs/reference-en.md` §7b](docs/reference-en.md#7b-engine-internals) · [`docs/concurrency.md`](docs/concurrency.md) · [`docs/ASYNC_GUIDE.md`](docs/ASYNC_GUIDE.md).

## vs LangGraph

| | LangGraph (Python) | NeoGraph (C++) |
|---|---|---|
| 엔진 | StateGraph | GraphEngine |
| 체크포인트 / HITL / fork / 시간 여행 | 예 | 예 (+ `NodeInterrupt`) |
| 병렬 팬아웃 | 정적 | `make_parallel_group` (+ 선택적 `asio::thread_pool`) |
| Send / Command | 예 | `NodeResult::sends` / `::command` |
| 다중 LLM | LangChain 필요 | `SchemaProvider` 내장 (3개 벤더) |
| MCP | 별도 구현 | 내장 |
| 런타임 / 메모리 | Python GIL · ~300 MB+ | C++20 코루틴 + asio · ~10 MB |
| 엣지 / 임베디드 | 불가능 | 라즈베리파이, Jetson, IoT |

LangGraph가 *고객당 프로세스*를 필요로 하는 동일한 멀티 테넌트 모양(StateGraph는 Python 객체)을 NeoGraph는 graph-as-JSON으로 하나의 프로세스에서 제공 — [multi-tenant](examples/cookbook/multi_tenant_chatbot/)와 [self-evolving](examples/cookbook/self_evolving_chatbot/) 쿡북이 그 이유를 보여준다.

## 감사의 말

[LangGraph](https://github.com/langchain-ai/langgraph), [agent.cpp](https://github.com/mozilla-ai/agent.cpp), [asio](https://think-async.com/Asio/) (3.0 엔진 런타임), [Clay](https://github.com/nicbarker/clay)에서 영감을 얻었다.

## 라이선스

MIT — [LICENSE](LICENSE) 참조. 서드파티: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
