<!-- neograph-i18n: source=docs/doxygen-mainpage.md locale=ko source_sha256=a2fa88f0d8ef08b3821d8ffe9810bc53b0ed7cd56251bd719b552628036a5b0d -->
# NeoGraph C++ API 참조 {#mainpage}

**Languages:** [English](doxygen-mainpage.md) | [한국어](doxygen-mainpage.ko.md) | [日本語](doxygen-mainpage.ja.md) | [简体中文](doxygen-mainpage.zh-CN.md)

C++20 그래프 에이전트 엔진 라이브러리 — C++용 LangGraph로, 선택적 Python 바인딩이 포함됩니다. 이 사이트는 `include/neograph/`의 공개 C++ 헤더에 대한 **생성된 참조**입니다.

## 시작 위치

NeoGraph를 처음 접하는 경우, **먼저 서술형 문서를 읽으십시오** — 이 생성된 참조는 찾고 있는 클래스 시그니처를 알게 된 후 조회용으로 사용됩니다.

| 용 | 이동 |
|---|---|
| NeoGraph가 무엇인지, 이유, 벤치마크 | [README](https://github.com/fox1245/NeoGraph#readme) |
| 개념 모델 — 채널, 노드, 엣지, Send, Command | [Core Concepts](https://github.com/fox1245/NeoGraph/blob/master/docs/concepts.md) |
| 일반적인 문제에 대한 증상 우선 해결 방법 | [Troubleshooting](https://github.com/fox1245/NeoGraph/blob/master/docs/troubleshooting.md) |
| 39개의 실행 가능한 C++ 프로그램 | [examples/](https://github.com/fox1245/NeoGraph/tree/master/examples) |
| 23개의 실행 가능한 Python 프로그램 | [bindings/python/examples/](https://github.com/fox1245/NeoGraph/tree/master/bindings/python/examples) |
| Async / 코루틴 내부 구조 | [ASYNC_GUIDE](https://github.com/fox1245/NeoGraph/blob/master/docs/ASYNC_GUIDE.md) |

## 최상위 헤더

편의 헤더는 전체 Core + GraphEngine API를 포함합니다:

```cpp
#include <neograph/neograph.h>

using namespace neograph;
using namespace neograph::graph;
```

하위 네임스페이스:

- `neograph`           — 기반 타입(`Provider`, `Tool`, `ChatMessage`)
- `neograph::graph`    — 엔진, 노드, 상태, 체크포인트
- `neograph::llm`      — 공급자 구현(OpenAI, 스키마 기반, 에이전트 헬퍼)
- `neograph::mcp`      — Model Context Protocol 클라이언트
- `neograph::async`    — 코루틴 + io_context 인프라
- `neograph::util`     — 동시성 기본 요소

## 첫 번째 프로그램

```cpp
#include <neograph/neograph.h>
#include <neograph/llm/mock_provider.h>

using namespace neograph;
using namespace neograph::graph;

int main() {
    json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes",    {{"echo",     {{"type", "llm_call"}}}}},
        {"edges",    json::array({
            {{"from", "__start__"}, {"to", "echo"}},
            {{"from", "echo"},      {"to", "__end__"}}})}
    };

    NodeContext ctx;
    ctx.provider = std::make_shared<llm::MockProvider>();
    auto engine = GraphEngine::build_strict(
        definition, EngineConfig{.node_context = std::move(ctx)});

    RunConfig cfg;
    cfg.thread_id = "demo";
    cfg.input["messages"] = json::array({{{"role","user"},{"content","hi"}}});
    auto result = engine->run(cfg);
    return 0;
}
```

실제 LLM 사용 시 `MockProvider`를 `llm::OpenAIProvider` 또는 `llm::SchemaProvider`로 교체하세요. 전체 `Provider` 인터페이스는 `neograph::Provider`에 있습니다.

## 참조 색인

사이드바의 클래스 목록, 파일 목록, 네임스페이스 목록은 `include/neograph/` 아래의 헤더에서 생성됩니다. [클래스 목록](annotated.html)이 가장 유용한 진입점입니다.

## 소스

프로젝트 홈: <https://github.com/fox1245/NeoGraph>

라이선스: MIT.
