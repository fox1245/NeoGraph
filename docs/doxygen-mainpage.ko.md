<!-- neograph-i18n: source=docs/doxygen-mainpage.md locale=ko source_sha256=0db34ca3ab8056fa7c8562fd8e774815e8c6b76787b74570c41b80974be4fd88 -->
**Languages:** [English](doxygen-mainpage.md) | [한국어](doxygen-mainpage.ko.md) | [日本語](doxygen-mainpage.ja.md) | [简体中文](doxygen-mainpage.zh-CN.md)

# NeoGraph C++ API 참조 {#mainpage}


C++20 그래프 에이전트 엔진 라이브러리 - C++용 LangGraph(옵션 포함)
파이썬 바인딩. 이 사이트는 다음에 대한 **생성된 참조**입니다.
`include/neograph/`의 공개 C++ 헤더.

## 어디서부터 시작해야 할까요?

NeoGraph를 처음 사용하는 경우 **내러티브 문서를 먼저 읽으십시오** — 이
생성된 참조는 알고 나면 클래스 서명을 찾기 위한 것입니다.
당신이 찾고있는 것.

|을 위한|이동|
|---|---|
|NeoGraph가 무엇인지, 왜, 벤치마크|[README](https://github.com/fox1245/NeoGraph#readme)|
|정신 모델 - 채널, 노드, 에지, 보내기, 명령|[Core Concepts](https://github.com/fox1245/NeoGraph/blob/master/docs/concepts.md)|
|일반적인 문제에 대한 증상 우선 수정|[Troubleshooting](https://github.com/fox1245/NeoGraph/blob/master/docs/troubleshooting.md)|
|39개의 실행 가능한 C++ 프로그램|[examples/](https://github.com/fox1245/NeoGraph/tree/master/examples)|
|실행 가능한 Python 프로그램 23개|[bindings/python/examples/](https://github.com/fox1245/NeoGraph/tree/master/bindings/python/examples)|
|비동기/코루틴 내부|[ASYNC_GUIDE](https://github.com/fox1245/NeoGraph/blob/master/docs/ASYNC_GUIDE.md)|

## 최상위 헤더

편의 헤더는 전체 코어 + 그래프 엔진 API를 가져옵니다.

```cpp
#include <neograph/neograph.h>

using namespace neograph;
using namespace neograph::graph;
```

하위 네임스페이스:

- `neograph` — 기초 유형(`Provider`, `Tool`, `ChatMessage`)
- `neograph::graph` — 엔진, 노드, 상태, 체크포인트
- `neograph::llm` — 공급자 구현(OpenAI, 스키마 기반, 에이전트 도우미)
- `neograph::mcp` — 모델 컨텍스트 프로토콜 클라이언트
- `neograph::async` — 코루틴 + io_context 인프라
- `neograph::util` — 동시성 프리미티브

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

실제 LLM를 사용하려면 `MockProvider`를 `llm::OpenAIProvider`로 바꾸거나
`llm::SchemaProvider`. 전체 `Provider` 인터페이스는 다음과 같습니다.
`neograph::Provider`.

## 참조 색인

사이드바의 클래스 목록, 파일 목록, 네임스페이스 목록은 다음과 같습니다.
`include/neograph/` 아래의 헤더에서 생성됩니다.
[Class list](annotated.html)는 가장 유용한 진입점입니다.

## 원천

프로젝트 홈: <https://github.com/fox1245/NeoGraph>

라이센스: MIT.
