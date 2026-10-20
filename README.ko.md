<!-- neograph-i18n: source=README.md locale=ko source_sha256=9cad6efa3db9b5f3166d7c9c9ac08dabc8fb0322f0542fb48c44debd81ff4840 -->
# NeoGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

[![CI](https://github.com/fox1245/NeoGraph/actions/workflows/ci.yml/badge.svg)](https://github.com/fox1245/NeoGraph/actions/workflows/ci.yml)
[![PyPI](https://img.shields.io/pypi/v/neograph-engine)](https://pypi.org/project/neograph-engine/)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

NeoGraph는 그래프로 기술한 워크플로를 실행하는 C++20 라이브러리입니다. 노드가 일을 하고, 이름이 붙은 상태 채널이 데이터를 담으며, 엣지가 다음에 무엇을 실행할지 정합니다. 런타임은 노드를 스케줄링하고 노드가 쓴 값을 상태에 병합하며, 체크포인트 저장소를 설정했다면 단계마다 체크포인트를 저장하고, 사람의 입력을 받기 위해 실행을 멈췄다가 나중에 재개할 수도 있습니다.

같은 엔진을 C++과 Python(`pip install neograph-engine`)에서 쓸 수 있습니다. 선택 구성 요소로는 SchemaProvider SDK를 통한 모델 호출, MCP·A2A·ACP·gRPC 서버와 클라이언트, SQLite와 PostgreSQL 체크포인트 저장소, 그리고 제한된 JavaScript 프로그램이 예산 안에서 여러 그래프를 조율하는 Program 계층이 있습니다.

## 대상 독자

- 루프, 분기, 병렬 fan-out, 재시도, 체크포인트, 사람의 승인 단계처럼 상태가 분명한 에이전트·파이프라인 워크플로를 만드는 Python·C++ 개발자.
- 그런 워크플로를 C++ 서비스 안에서 실행하거나, MCP·A2A·ACP로 다른 에이전트와 도구에 공개하려는 팀.
- LangGraph를 아는 독자. 리듀서가 붙은 채널, `Send`, `Command`, 체크포인트는 같은 개념이므로 [핵심 개념 가이드](docs/concepts.md)를 빠르게 읽을 수 있습니다.

NeoGraph는 알파 단계 소프트웨어입니다(버전 0.16.0). C++ 바이너리 인터페이스는 0.x 릴리스 사이에 바뀔 수 있으니 [ABI 정책](docs/ABI_POLICY.md)을 보세요. 모델을 학습시키거나 수치 계산 라이브러리를 대신하지 않으며, 모델 호출은 노드가 하는 일 가운데 하나일 뿐입니다.

## 빠른 시작

두 예제 모두 노드가 하나인 그래프를 실행합니다. API 키가 필요 없고, 실행할 때 네트워크 호출도 하지 않습니다.

### Python

Python 3.9 이상이 필요합니다.

```bash
pip install neograph-engine
```

다음 내용을 `hello.py`로 저장합니다.

```python
import neograph_engine as ng

@ng.node("greet")
def greet(state):
    return [ng.ChannelWrite(
        "messages",
        [{"role": "assistant", "content": f"Hello, {state.get('name')}!"}],
    )]

definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "demo",
    "channels": {
        "name": {"reducer": "overwrite"},
        "messages": {"reducer": "append"},
    },
    "nodes": {"greet": {"type": "greet"}},
    "edges": [
        {"from": ng.START_NODE, "to": "greet"},
        {"from": "greet", "to": ng.END_NODE},
    ],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())
result = engine.run(ng.RunConfig(thread_id="t1", input={"name": "NeoGraph"}))
print(result.output["channels"]["messages"]["value"])
```

```bash
python hello.py
```

```text
[{'role': 'assistant', 'content': 'Hello, NeoGraph!'}]
```

PyPI의 휠은 0.16.0 릴리스로 빌드한 것입니다. [변경 이력](CHANGELOG.md)의 *Unreleased* 항목에 있는 변경은 휠에 들어 있지 않습니다. 그 변경을 쓰려면 소스에서 빌드하세요([빌드 설정](#build-configuration-and-further-reading) 참고).

### C++

C++20 컴파일러, CMake 3.20 이상, git, libcurl 개발 파일(libcurl 7.88 이상, Debian·Ubuntu에서는 `libcurl4-openssl-dev`)이 필요합니다. 처음 configure할 때 고정된 버전의 SchemaProvider SDK를 내려받습니다. 모델 호출을 하지 않더라도 Core에는 이 SDK가 필요합니다. 오프라인 환경에서는 아래의 [빌드와 실행](#build-and-run)을 보세요.

이 문서와 맞는 태그 릴리스가 아직 없으므로, 아래 프로젝트는 `master` 브랜치를 가져옵니다. 빌드를 재현할 수 있게 하려면 `master`를 커밋 해시로 바꾸세요.

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(hello_neograph CXX)

include(FetchContent)
# Build only Core: no model calls, protocols, databases, examples or tests.
set(NEOGRAPH_BUILD_EXAMPLES OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_LLM OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_ASYNC OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_MCP OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_A2A OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_ACP OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_POSTGRES OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_SQLITE OFF CACHE BOOL "")
FetchContent_Declare(NeoGraph
  GIT_REPOSITORY https://github.com/fox1245/NeoGraph.git
  GIT_TAG master)
FetchContent_MakeAvailable(NeoGraph)

add_executable(hello main.cpp)
target_compile_features(hello PRIVATE cxx_std_20)
target_link_libraries(hello PRIVATE neograph::core)
```

`main.cpp`:

```cpp
#include <neograph/neograph.h>

#include <cctype>
#include <iostream>
#include <memory>
#include <utility>

using namespace neograph;
using namespace neograph::graph;

namespace {

const ChannelKey<std::string> text_channel{"text"};

class UpperNode final : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        auto text = input.state.get(text_channel);
        for (auto& character : text)
            character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        co_return NodeOutput{{ChannelWrite{"text", json(std::move(text))}}};
    }

    std::string get_name() const override { return "upper"; }
};

}  // namespace

int main() {
    NodeFactory::instance().register_type(
        "quickstart.upper", [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<UpperNode>();
        },
        json::object(),
        json{{"reads", json::array({"text"})},
             {"writes", json::array({"text"})},
             {"exports", json::array({"text"})}});

    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "hello"},
        {"channels", {{"text", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"upper", {{"type", "quickstart.upper"}}}}},
        {"edges",
         {{{"from", "__start__"}, {"to", "upper"}},
          {{"from", "upper"}, {"to", "__end__"}}}},
    };

    auto engine = GraphEngine::build_strict(definition, EngineConfig{});
    RunConfig config;
    config.input = {{"text", "hello"}};
    const auto result = engine->run(config);
    std::cout << result.channel(text_channel) << '\n';
    return 0;
}
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/hello
```

```text
HELLO
```

같은 그래프가 [examples/62_core_quickstart.cpp](examples/62_core_quickstart.cpp)입니다. `cmake --install`로 설치한 NeoGraph를 대신 쓰려면 `find_package(NeoGraph CONFIG REQUIRED)`를 호출하고 `neograph::core`를 링크하세요. [빌드 설정](#build-configuration-and-further-reading)을 참고하세요.

### 다음 단계

- [문서 목차](docs/README.md): 모든 가이드와 대상 독자.
- [핵심 개념](docs/concepts.md): 채널, 노드, 엣지, `Send`, `Command`, 체크포인트, 스트리밍.
- [Python 바인딩 가이드](docs/python-binding.md)와 [C++ 예제](examples/README.md).
- 서버와 클라이언트: [MCP](docs/mcp.md), [A2A](docs/a2a.md), [ACP](docs/acp.md).
- [Program 가이드](docs/program-guide.md): fan-out, 실패 라우팅, 승인, 예산을 갖춘 제한된 JavaScript 프로그램.

## 그래프가 상태를 바꾸는 방식

`count`라는 채널에 `2`가 있다고 합시다. 증가 노드는 `2`를 읽고, `3`을 제안하는 쓰기를 반환합니다. 런타임은 스케줄된 노드 묶음이 끝난 뒤 채널의 리듀서로 이 쓰기를 적용합니다. 다음 묶음에서 실행되는 후속 노드는 `3`을 읽습니다.

```text
Committed state       Node computation          Reduced state
count = 2       ->    read 2; propose 3     ->    count = 3
                                                  |
                                            next node reads 3
```

위 흐름에 나온 용어는 실행 모델을 설명합니다.

| 용어 | NeoGraph에서의 의미 |
|---|---|
| 노드(Node) | 호스트에 등록된 실행 단위. 입력 상태를 읽고, 채널 쓰기와 선택적인 라우팅 명령을 반환합니다. |
| 상태(State) | 현재 실행 단계에서 보이는 채널 값. 설정에 따라 런타임이 관리하는 이력과 사용량 집계도 포함합니다. |
| 채널(Channel) | 리듀서가 붙은 이름 있는 값. 보존 정책과 체크포인트 영속화 정책을 선택적으로 지정할 수 있습니다. |
| 리듀서(Reducer) | 채널의 현재 값과 새로 들어온 쓰기를 합치는 함수. `overwrite`는 값을 바꾸고, `append`는 배열 원소를 쌓으며, 사용자 정의 리듀서는 다른 합치기 방식을 정의합니다. |
| 엣지(Edge) | 노드 사이의 스케줄링 규칙. 무조건 엣지, 조건부 엣지, 여러 선행 노드를 기다리는 배리어가 있습니다. 순환을 두면 같은 단계를 반복할 수 있습니다. |
| 슈퍼스텝(Superstep) | 실행 준비가 된 노드를 한 묶음으로 실행하고, 그 쓰기를 적용한 뒤 다음 일정을 진행하는 단위. |

일반적인 묶음에서 준비된 노드는 묶음이 시작되기 전의 채널 상태를 읽습니다. 노드가 `ChannelWrite`를 반환해도 같은 묶음의 다른 노드가 읽는 값은 바로 바뀌지 않습니다. 묶음이 성공적으로 끝나면 실행기가 결과를 적용하고, 이후 단계는 갱신된 상태를 봅니다. 분기가 여러 개인 `Send` 묶음에서는 각 분기가 격리된 상태 복사본에서 입력을 받고, 끝난 뒤에 출력을 병합합니다. 이는 Pregel 방식의 작업 구성입니다. 채널과 리듀서 규칙은 NeoGraph 고유의 계약이며, Pregel의 모든 기능을 구현했다는 뜻은 아닙니다.

동시에 실행한다고 해서 모든 리듀서가 순서와 무관해지는 것은 아닙니다. 두 노드가 텍스트를 덧붙이거나 같은 채널을 덮어쓰면 쓰기 순서가 결과에 영향을 줍니다. 워크플로가 순서 무관성을 요구한다면 채널을 따로 쓰거나 순서와 무관한 리듀서를 쓰세요. 채널 보존도 합치기와는 별개입니다. append 채널은 마지막 일부만 남기도록 제한할 수 있습니다. 스케줄링, 리듀서, 배리어, 취소는 [핵심 개념](docs/concepts.md)과 [동시성](docs/concurrency.md)을 보세요.

## Core 예제 따라가기

위의 [C++ 빠른 시작](examples/62_core_quickstart.cpp)은 문자를 대문자로 바꾸는 노드를 등록하고 다음 토폴로지를 컴파일합니다.

```text
__start__ -> upper -> __end__

Input channel:   text = "hello"
Node reads:      "hello"
Node returns:    ChannelWrite{"text", "HELLO"}
Reducer:         overwrite
Output channel:  text = "HELLO"
```

소스에는 노드가 읽고 쓰는 채널이 선언되어 있어서, `GraphEngine::build_strict`가 실행 전에 토폴로지를 검사할 수 있습니다. 모델 호출도 API 키도 필요 없습니다. 기대 출력은 `HELLO`입니다.

<a id="build-and-run"></a>
### 빌드와 실행

`NEOGRAPH_BUILD_LLM=OFF`일 때도 SchemaProvider는 꼭 필요한 외부 SDK입니다. Core가 타입이 지정된 provider 계약을 내보내기 때문입니다. 아래 명령은 설치된 [SchemaProvider 런타임 패키지](https://github.com/fox1245/SchemaProvider)를 사용하며, `SCHEMAPROVIDER_PREFIX`를 그 설치 경로로 지정합니다. `-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=../SchemaProvider`로 지정한 체크아웃이 가장 우선합니다. 그렇지 않으면 CMake는 설치된 패키지를 먼저 찾고, 없으면 고정된 공개 SDK 아카이브를 내려받습니다. 설치된 패키지나 지정한 체크아웃으로 오프라인 빌드를 하려면 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`로 지정하세요. CMake는 이웃 디렉터리의 체크아웃을 추측하지 않으며, 제거된 번들 인터프리터도 쓰지 않습니다.

필요한 것은 C++20 컴파일러, CMake 3.20 이상, 그리고 SDK의 런타임 의존성인 libcurl 7.88 이상입니다. SDK의 TLS는 libcurl 백엔드를 쓰므로 SDK 자체에는 OpenSSL이 필요 없습니다. NeoGraph의 HTTPS 구성 요소를 포함한 전체 빌드에는 OpenSSL 3이 필요합니다. 기본 빌드는 SQLite와 PostgreSQL 연동도 켜므로, 아래 명령은 필요 없는 NeoGraph 구성 요소를 끕니다. SDK의 의존성은 그대로 남습니다. 플랫폼과 빌드 제약은 [문제 해결](docs/troubleshooting.md)을 보세요.

```bash
git clone https://github.com/fox1245/NeoGraph.git
cd NeoGraph
cmake -S . -B build-core \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_PROGRAM=OFF \
  -DNEOGRAPH_BUILD_LLM=OFF \
  -DNEOGRAPH_BUILD_ASYNC=OFF \
  -DNEOGRAPH_BUILD_MCP=OFF \
  -DNEOGRAPH_BUILD_A2A=OFF \
  -DNEOGRAPH_BUILD_ACP=OFF \
  -DNEOGRAPH_BUILD_POSTGRES=OFF \
  -DNEOGRAPH_BUILD_SQLITE=OFF
cmake --build build-core --parallel --target example_core_quickstart
./build-core/example_core_quickstart
```

## Core와 ProgramRuntime

`GraphEngine`은 컴파일된 그래프를 실행합니다. 노드 스케줄링, 상태 갱신, 라우팅, 재시도, 스트리밍, 취소, 그래프 체크포인트와 재개를 맡습니다. 컴파일된 토폴로지는 바꿀 수 없습니다. 지원하는 세대 이전(migration)은 노드가 실행되는 도중에 임의로 바꾸는 방식이 아니라, 통제된 안전 지점에서 이루어집니다.

`ProgramRuntime`은 승인(admission)된 Program을 조율합니다. Program은 Core 그래프를 호출하고 자식 Program을 관리할 수 있습니다. 불변인 Program 버전, 카탈로그와 정책 스냅샷, 명령 저널, 자식 계보, 예산, 재생(replay), 승인된 교체와 이전을 더해 줍니다. 호스트는 실행 가능한 기능을 등록하고, Program을 컴파일해 승인한 뒤 호출을 시작합니다. 그래프 노드를 실행하는 것은 여전히 Core입니다.

앞의 조사 워크플로에서는 Core 그래프 하나가 검색과 검토를 수행할 수 있습니다. Program은 그 그래프를 호출하고, 별도 작업을 위해 자식 Program을 시작하고, 그 결과를 기다리고, 수명 주기 전환을 기록할 수 있습니다. 영속적인 복구에는 설정된 저장소와 해당 보관(custody) 계약이 필요합니다. 메모리 저장소는 프로세스가 끝나면 사라지고, 저널만으로 외부 도구의 효과가 정확히 한 번 일어나게 되지는 않습니다.

QuickJS는 선택 사항인 Program 작성 인터페이스입니다. Program은 범위가 제한된 JavaScript 계산과 `callCore`, `spawn`, `await`, `all`, `parallel`, `race`, `quorum`, `emit`, `checkpoint`, `cancelScope` 같은 제너레이터 명령을 씁니다. 호스트는 기능을 승인하고, 생성된 소스를 게시하기 전에 검증합니다. 모델이 생성한 제안은 컴파일러, 자격 증명, 카탈로그, 권한 부여에 접근할 수 없습니다.

```bash
cmake -S . -B build-program \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON \
  -DNEOGRAPH_BUILD_EXAMPLES=ON
cmake --build build-program --parallel --target example_program_quickstart
./build-program/example_program_quickstart
```

[Program 빠른 시작](examples/63_program_quickstart.cpp)은 증가 그래프를 호출하는 Program을 컴파일해 승인하며, 기대 출력은 `1`입니다. 메모리 저장소와 C++ Program 빌더를 씁니다. JavaScript 작성과 영속적 실행은 [Program 가이드](docs/program-guide.md), [재귀적 Program](docs/PROGRAM_RECURSIVE_HARNESSES.md), [엄격한 런타임 계약](docs/STRICT_RUNTIME_INTERPOSITION.md)부터 보세요.

<a id="typed-provider-calls"></a>
## 타입이 지정된 provider 호출

모델 호출에는 검증된 디스크립터, 런타임 옵션, 타입이 지정된 요청을 씁니다. 디스크립터 승인은 닫힌 버전 데이터만 받으며, 요청·응답 인터프리터를 실행하지 않습니다. 자격 증명은 공개 디스크립터 파일이 아니라 런타임 옵션에 둡니다. `SchemaProvider::Defaults`에는 타입이 지정된 OpenRouter 라우팅과 Responses 보존 설정이 들어 있습니다. 이미지, Veo, Decisions는 별도의 타입 클라이언트와 권한을 씁니다.

```cpp
#include <neograph/llm/schema_provider.h>
#include <neograph/types.h>

sp::runtime::Result first_call(
    sp::descriptor::ValidatedDescriptor descriptor,
    sp::runtime::Options options, std::string model) {
    neograph::llm::SchemaProvider provider(
        std::move(descriptor), std::move(options), {});
    std::vector<sp::Message> history{
        {.role = sp::Role::User, .parts = {sp::Text{"hi"}}}};
    auto request = neograph::make_provider_request(
        provider, std::move(model), std::move(history));
    auto prepared = provider.prepare(std::move(request));
    return provider.dispatch(std::move(prepared));
}
```

준비된 요청은 한 번만 소비됩니다. `sp::runtime::Result`는 `Completion` 또는 `Failure`를 담은 불변 `sp::Outcome`을 소유합니다. 순서 있는 메시지와 파트, 네이티브 연속 정보, 원시 관찰값, 중단 근거, 시도 메타데이터, 실패 시의 부분 결과가 필요하면 이 결과를 그대로 보관하세요. 사용량 카운터는 null일 수 있습니다. 없으면 알 수 없다는 뜻이고, 관찰된 0은 0입니다. 사용량 보고와 예산 차감은 별개의 기록이며, 이식 가능한 보고서로는 지출 권한을 줄 수 없습니다.

`usage.provider_cost`는 provider가 보고한 nanoUSD 금액, 필드별 상태, 출처, 품질, BYOK 근거, 파싱한 binary64 값을 올림한 내역을 보관합니다. 청구서, 토큰 수, 추정치, 지출 권한이 아닙니다. 빠진 금액 메타데이터는 표준 JSON에서도 빠진 채로 남습니다. 보고된 0이나 `is_byok=false`는 여전히 근거입니다. 알 수 없는 통화와 서로 충돌하거나 형식이 잘못된 보고는 USD나 토큰으로 변환하지 않고 그대로 보관합니다. 블로킹 provider 호출도 단일 비동기 SDK 파이프라인을 구동하며, SDK 이벤트 루프나 콜백 워커로의 재진입은 거부됩니다.

`first_call`이 반환한 뒤에는 `std::get_if<sp::Completion>(result.get())`로 완료 결과의 `messages`, `stop`, `usage`를 확인합니다. 완료가 아니면 `std::get<sp::Failure>(*result)`가 `error.kind`, `error.safe_message`, 재시도 근거, 그리고 `partial`에 담긴 부분 메시지와 사용량을 제공합니다. 예를 들어 `completion.usage.output_total`이 없으면 출력 토큰 수를 알 수 없다는 뜻이고, 있고 `value`가 `0`이면 0을 보고한 것입니다. 표시용 텍스트는 보관된 결과의 한 가지 보기일 뿐입니다.

`ChatMessage`, `ChatTool`, JSON은 이식 가능한 투영입니다. 진짜 네이티브 이력은 네이티브 체크포인트 사이드카와 함께 메모리에 둘 수 있고, 영속적인 네이티브 이력에는 실제 `sp::NativeArchive`와 소유자 전용 보호 보관이 필요합니다. 이식 가능한 JSON으로는 그 권한을 다시 만들 수 없습니다. 아카이브는 독립된 키로 보관 상태를 인증합니다. 암호화도 아니고 벤더 발급자 인증도 아닙니다. 아카이브 본문, 키, 네이티브 블롭, 원시 와이어 관찰값은 공개하지 마세요. 영속성 실패, 옵저버, 관리되는 예산 뱅크, 재생 경계는 [provider 레퍼런스](docs/reference-en.md)와 [마이그레이션 가이드](docs/migration-v0.4-to-v1.0.md)를 보세요.

타입 전환으로 `CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, 디스크립터 인터프리터, Responses WebSocket 경로가 제거되었습니다. C++ 사용 코드를 다시 컴파일하고 사용자 정의 provider를 이전하세요. 호환용 별칭은 없습니다. SDK 패키지는 `0.4.0` 알파이며, `cmake/NeoGraphSchemaProvider.cmake`가 고정한 공개 커밋(`main` 라인)을 쓰며, 인터페이스 리비전 7, 공유 ABI 7입니다. 타입이 있는 미디어와 새 요청 제어가 공개 레이아웃을 바꾸므로, SDK를 쓰는 코드, NeoGraph, Python 확장을 함께 다시 빌드하세요. 네이티브 아카이브 v3(`spna3`)는 그대로입니다. 이식 가능한 JSON은 `provider-message-v3`, `provider-outcome-v3`, `runtime-history-record-v3`로 기록하며, v2 형식의 레코드도 계속 읽습니다. 이 세대는 안정적인 SDK 인터페이스를 선언하지 않습니다.

## Python

```bash
pip install neograph-engine
```

여기서 설명하는 타입이 지정된 provider API는 NeoGraph `0.14.0`을 대상으로 합니다. 이전 휠은 구버전 인터페이스를 노출합니다. 소스 API와 빌드 전제 조건은 [Python 바인딩 가이드](docs/python-binding.md)에 있습니다. [빠른 시작](#python)의 그래프에는 API 키가 필요 없습니다.

Python은 Program 컴파일과 실행, Hook, 런타임 컨텍스트 요구 사항, 엄격 프로필, SQLite 영속성, 정확한 체크포인트 재개도 제공합니다. 타입이 지정된 provider는 `ProviderMessage`와 `make_provider_request`를 쓰고, `prepare`/`dispatch` 또는 `invoke`를 호출합니다. 결과에는 네이티브가 소유한 완료 또는 실패 근거가 남습니다. 이 메시지는 그래프용 편의 타입인 `ChatMessage`와 다릅니다. 블로킹 provider 호출은 GIL을 놓으며, `asyncio.to_thread`로 이벤트 루프 스레드 밖에서 실행할 수 있습니다. 완전한 provider 입력은 [Python 예제](bindings/python/examples/README.md)를, 실행 가능한 Program 예제와 그 규칙은 [Python 바인딩 가이드](docs/python-binding.md#program-and-quickjs)를 보세요.

## 적합한 작업과 한계

NeoGraph는 상태 전이가 분명하고, 분기나 루프, 병렬 작업, 체크포인트, 사람의 검토가 있는 워크플로에 맞습니다. 고정된 작은 그래프를 C++ 애플리케이션에 내장할 수도 있습니다. 노드가 하는 계산은 애플리케이션의 몫입니다. 그래프 런타임은 모델을 학습시키지 않고 수치 계산 라이브러리를 대신하지도 않으며, 모델 호출에는 provider의 지연, 가용성, 비용이 그대로 따릅니다.

런타임은 그래프 전체와 노드별 재시도 정책, 범위가 제한된 재사용 가능한 노드 캐시, `Send` fan-out, `Command` 라우팅, 서브그래프, 상태 이력, 포크, HITL, `NodeInterrupt`를 지원합니다. MCP, A2A, ACP, gRPC, 관측 연동은 선택 구성 요소입니다. 런타임 컨텍스트와 Hook은 특정 디스패치 입력을 요구하고 전달 근거를 기록할 수 있지만, 이 검사가 모델이 모든 토큰에 주의를 기울였음을 증명하지는 않습니다. 영속적인 네이티브 복구와 범위가 제한된 포크에는 원래의 공유 회계 권한이 필요합니다. 스냅샷을 복사해도 예산이 새로 생기지 않습니다.

실제 노드, provider, 저장소, 동시성, 빌드 프로필로 워크로드를 측정하세요. [벤치마크](benchmarks/README.md)와 [성능 가이드](docs/performance-deep-dive.md)는 측정한 구성과 한계를 설명하며, 어디서나 통하는 속도 주장이 아닙니다. 최적화된 실행을 측정하려면 단일 구성 빌드에서 `CMAKE_BUILD_TYPE=Release`를 지정하세요. `NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION=ON`은 지원하는 컴파일러에서 호스트별 튜닝을 추가하므로, 배포용 바이너리에서는 끄세요.

<a id="build-configuration-and-further-reading"></a>
## 빌드 설정과 추가 문서

| 옵션 | 용도 |
|---|---|
| `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` | 지정한 SDK 체크아웃. 설치된 패키지 탐색과 내려받기보다 우선합니다. |
| `NEOGRAPH_FETCH_SCHEMAPROVIDER` | 설치된 패키지가 없을 때 고정된 공개 SDK 아카이브를 내려받습니다. 기본값은 켜짐이며, 오프라인 빌드에서는 끄세요. |
| `NEOGRAPH_BUILD_PROGRAM` | Program 런타임, 카탈로그, 계보, 이전. 기본값은 꺼짐. |
| `NEOGRAPH_BUILD_QUICKJS_CONTROL` | 내장 QuickJS Program 작성. 기본값은 꺼짐. |
| `NEOGRAPH_BUILD_PYBIND` | Python 확장. 기본값은 꺼짐. |
| `NEOGRAPH_BUILD_LLM` | NeoGraph의 모델 호출 어댑터. 끄더라도 SDK 의존성은 사라지지 않습니다. |
| `NEOGRAPH_BUILD_SQLITE` / `NEOGRAPH_BUILD_POSTGRES` | 선택 사항인 영속 저장소. 둘 다 기본값은 켜짐. |
| `NEOGRAPH_BUILD_MCP_CLIENT` / `NEOGRAPH_BUILD_MCP_SERVER` | MCP 클라이언트와 stdio 서버 구성 요소. 둘 다 기본값은 켜짐(`NEOGRAPH_BUILD_MCP`가 기본값을 정함). |
| `NEOGRAPH_BUILD_MCP_HTTP_SERVER` | MCP Streamable HTTP 서버 전송. 기본값은 꺼짐. |
| `NEOGRAPH_BUILD_HARNESS_MCP_BINARY` | `neograph-harness-mcp` 실행 파일을 빌드하고 설치합니다. 기본값은 꺼짐. |
| `NEOGRAPH_BUILD_A2A` / `NEOGRAPH_BUILD_ACP` / `NEOGRAPH_BUILD_GRPC` | 프로토콜 연동. A2A와 ACP는 기본값이 켜짐, gRPC는 꺼짐. |
| `NEOGRAPH_BUILD_ASYNC` | HTTP, SSE, WebSocket 헬퍼. 기본값은 켜짐. |
| `NEOGRAPH_BUILD_EXAMPLES` / `NEOGRAPH_BUILD_TESTS` | 예제 프로그램(기본값 켜짐)과 단위 테스트(기본값 꺼짐). |
| `NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION` | 최적화 구성용 호스트별 튜닝. 이식할 수 없습니다. 기본값은 꺼짐. |

Core를 가져오는 대신 설치해서 다른 프로젝트에서 쓰려면, 한 번 빌드하고 설치하세요. CMake가 SDK를 내려받았다면 `cmake --install`이 SDK를 같은 경로에 설치하므로 `CMAKE_PREFIX_PATH`에 항목 하나만 지정하면 됩니다.

```bash
git clone https://github.com/fox1245/NeoGraph.git
cmake -S NeoGraph -B NeoGraph/build-install -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_EXAMPLES=OFF -DNEOGRAPH_BUILD_LLM=OFF -DNEOGRAPH_BUILD_ASYNC=OFF \
  -DNEOGRAPH_BUILD_MCP=OFF -DNEOGRAPH_BUILD_A2A=OFF -DNEOGRAPH_BUILD_ACP=OFF \
  -DNEOGRAPH_BUILD_POSTGRES=OFF -DNEOGRAPH_BUILD_SQLITE=OFF
cmake --build NeoGraph/build-install --parallel
cmake --install NeoGraph/build-install --prefix "$HOME/neograph"
```

다른 프로젝트에서는 빠른 시작의 `FetchContent` 줄을 `find_package(NeoGraph CONFIG REQUIRED)`로 바꾸고 `-DCMAKE_PREFIX_PATH="$HOME/neograph"`로 configure하세요. 설치된 구성 요소를 쓰는 쪽은 필요한 구성 요소만 링크합니다.

```cmake
find_package(SchemaProvider 0.4.0 EXACT CONFIG REQUIRED COMPONENTS runtime transport)
find_package(NeoGraph CONFIG REQUIRED)
target_link_libraries(app PRIVATE neograph::core neograph::llm SchemaProvider::runtime)
```

### 로컬 CI 검증 (Windows와 WSL)

소스 루트에서 실행합니다. Python 3.10 이상, CMake/CTest, 그리고 프로필에 필요한 네이티브 툴체인과 의존성이 미리 준비되어 있어야 합니다. Windows에서는 VS2022 x64 개발자 PowerShell을 쓰고, `CMAKE_TOOLCHAIN_FILE`로 vcpkg 전송 툴체인을, 전용 `VCPKG_INSTALLED_DIR`을, `PATH`에 OpenSSL/curl 런타임 도구를 지정하세요. WSL에서는 C++20 컴파일러, pkg-config, libpq, SQLite, OpenSSL, HTTP/2를 지원하는 curl 개발 패키지를 준비하세요. Linux의 `native-linux`와 `asan`에는 `psql`과, 접속할 수 있는 **파괴적 테스트 전용** 데이터베이스를 가리키는 `NEOGRAPH_TEST_POSTGRES_URL`도 필요합니다. Python pytest/pydantic/certifi를 준비하세요. `native-linux`에는 선택한 인터프리터에 `a2a-sdk[http-server]>=1.1,<2`, `agent-client-protocol==0.12.1`, uvicorn, httpx도 필요합니다.

```powershell
# Windows: choose a new output path for each invocation.
python scripts/verify_ci.py native-windows --work-dir build/local-windows-01 --jobs 4
python scripts/verify_ci.py install --work-dir build/local-install-01 --jobs 4 --shared --program
```

```sh
# WSL/Linux: provision the test database and dependencies before running.
python scripts/verify_ci.py native-linux --work-dir build/local-linux-01 --jobs 4
python scripts/verify_ci.py quickjs-performance --work-dir build/local-quickjs-01 --jobs 4
```

`--work-dir`은 소스 루트나 그 상위 경로가 아닌, 본인 소유의 새로운(존재하지 않는) 출력 경로여야 합니다. `--jobs`는 양수이거나 `auto`(CPU 수)여야 합니다. 이미 있는 출력은 거부하고 그대로 보존하며, 자동으로 지우지 않습니다. 실행기는 의존성을 설치하거나 호스트 정책을 바꾸지 않습니다. 고정된 의존성 내려받기와 cibuildwheel이 선언한 부트스트랩·복구는 그대로 적용됩니다.

| 프로필 | 유지되는 목적 / 전제 조건 |
|---|---|
| `native-linux` | PostgreSQL을 포함한 전체 네이티브 게이트, 이어서 직렬로 실행하는 전체 Python·프로토콜 테스트와 DB 없는 ACP 영속 재실행. |
| `native-posix` | Linux ARM/macOS 네이티브 테스트. 테스트 서비스 없이 PostgreSQL 빌드·링크만 검증. |
| `native-windows` | MSVC 기반 DB 없는 네이티브/Program/QuickJS 테스트와 별도의 C 임베딩 ABI 스모크 테스트. x64 MSVC 환경에서 Ninja로 빌드하며, `--generator "Visual Studio 17 2022"`를 줄 수도 있습니다. |
| `asan` | Linux ASan/UBSan/LSan, 예제 11개와 전체 Python 테스트. GCC libasan/libstdc++ 필요. |
| `tsan` | 별도의 Linux TSan 테스트와 예제 5개. 프로세스 로컬 `setarch -R` 허용, 억제 목록은 그대로. |
| `msvc-asan` | Windows에서 직렬로 실행하는 Program/QuickJS 카나리. 활성화된 `cl >=19.50`(VS2026)이 필요하며 MSVC 19.44는 안 됩니다. |
| `grpc` | gRPC 그래프 계약. gRPC/Protobuf 컴파일러와 라이브러리 필요. |
| `benchmark` | Linux Release에서 워크로드 네 가지를 보는 회귀 게이트. 처리량, 지연, 목표 RSS의 원래 한도 적용. |
| `bench-ab` | Linux Release. `benchmarks/ci_ab_gates.json`의 벤치마크를 이 빌드와 `--base <ref>`(`--base head`는 빌드를 자기 자신과 비교)에서 번갈아 실행해 케이스별 허용 오차와 비교. [CI 성능 게이트](benchmarks/README.md#ci-performance-gates) 참고. |
| `quickjs-performance` | Linux에서 켜짐/꺼짐 빌드를 맞춰 비교. 불변 출처 정보를 위해 실제 소스 루트 Git 체크아웃 필요. 상한은 CI의 hosted x86-64 러너 기준이라, 더 느린 하드웨어에서는 회귀가 없어도 `javascript_wrapped_core`가 실패할 수 있음. `--base <ref>`를 주면 그 커밋도 빌드해 같은 머신에서 케이스별로 비교함. |
| `fuzz` | Linux Clang/libFuzzer 60초 카나리. 코퍼스를 본인 소유 출력으로 복사. |
| `install` | 격리된 내보내기 접두사에서 ABI·심볼·C++·C11·충돌·재배치를 검사하는 소비자. Git/Bash 불필요. 선택 옵션 `--shared`, `--core-only`, `--program`. `--quickjs`는 `--program`과 ELF/Mach-O 검사가 필요하며 Windows 행에는 없습니다. |
| `sdist` | 소스 아카이브와 Twine. build/twine/scikit-build-core>=1.0/pybind11==2.13.6/ninja>=1.10 필요. 선택 옵션 `--release-tag v0.16.0`. |
| `wheel` | 복구를 거친 설치형 휠. cibuildwheel==2.23.0과 네이티브/컨테이너 provider 필요. `--arch x86_64\|aarch64\|arm64\|AMD64` 필수. 선택 옵션 `--python cp312`는 모든 CPython 대신 하나만 빌드합니다. |
| `runtime-archive` | 공유 SDK 네이티브 아카이브 이식성 타깃/CTest. Ninja와 플랫폼 런타임 의존성 필요. |

로컬 Windows/WSL 결과가 네이티브 ARM/macOS나 VS2026 새니타이저 행을 대신하지는 않습니다. push와 pull request마다 `ci.yml`이 실행됩니다. Linux, macOS, Windows의 전체 네이티브 테스트에 설치형 소비자 행 두 개가 더해지며, 문서만 바꾼 변경은 건너뜁니다. `ci-extended.yml`은 매일 밤과 요청 시에 실행됩니다. 새니타이저, 퍼즈 카나리, 성능 게이트, 네이티브 ARM64, gRPC, Visual Studio 제너레이터, 나머지 설치형 소비자 행 여덟 개를 다룹니다. 릴리스 후보에서는 태그를 붙이기 전에 실행하세요. `wheels.yml`은 패키징 변경 때 플랫폼마다 CPython 3.12 휠 하나를 빌드합니다. 릴리스 태그에서는 Linux(x86_64, aarch64), macOS(arm64), Windows(AMD64)용으로 CPython 3.9–3.13 휠을 빌드하며, 최소 플랫폼 버전은 glibc 2.34와 macOS 14입니다.

- [문서 목차](docs/README.md)
- [핵심 개념과 그래프 의미](docs/concepts.md)
- [C++ 레퍼런스](docs/reference-en.md)와 [Python 바인딩 가이드](docs/python-binding.md)
- [MCP](docs/mcp.md), [A2A](docs/a2a.md), [ACP](docs/acp.md) 가이드와 [Program 가이드](docs/program-guide.md)
- [비동기 가이드](docs/ASYNC_GUIDE.md)와 [동시성·취소](docs/concurrency.md)
- [런타임 컨텍스트와 엄격한 개입 계층](docs/STRICT_RUNTIME_INTERPOSITION.md)
- [Harness MCP](docs/HARNESS_MCP.md)
- [마이그레이션 가이드](docs/migration-v0.4-to-v1.0.md)와 [문제 해결](docs/troubleshooting.md)
- [C++ 예제](examples/README.md), [Python 예제](bindings/python/examples/README.md), [벤치마크 방법론](benchmarks/README.md)

## 라이선스

MIT. [LICENSE](LICENSE)를 보세요. 서드파티 고지는 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)에 있습니다.
