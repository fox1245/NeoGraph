<!-- neograph-i18n: source=README.md locale=ko source_sha256=9e3cece3555cbcb547daaaf29d7c6ce6aa422e229bc3899ce0fad78eac0400c9 -->
# NeoGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

NeoGraph는 상태를 유지하는 워크플로를 그래프로 표현하는 C++20 런타임입니다. 그래프는 실행할 노드, 이름이 붙은 상태 채널, 쓰기를 병합하는 규칙, 다음 실행 대상을 결정하는 간선을 정의합니다. 노드는 일반적인 계산을 수행하거나, 도구를 호출하거나, 모델 출력을 요청할 수 있습니다. 런타임은 노드를 스케줄링하고 노드가 반환한 쓰기를 적용합니다. 체크포인트 저장소를 설정하면 실행 진행 상황을 저장하여 중단한 뒤 재개할 수 있습니다. Python 바인딩도 같은 C++ 엔진을 사용합니다.

문서를 검색하고, 여러 문서에서 조사 결과를 추출하고, 결과를 합친 뒤, 검토자에게 추가 검색이 필요한지 묻는 조사 워크플로를 생각해 봅시다. 문서와 조사 결과는 상태 채널에 저장하고, 검색·추출·검토는 노드로 정의합니다. 간선은 다음 단계로 이동할지 검색으로 돌아갈지 결정합니다. 그래프를 사용하면 이러한 전이를 일련의 모델 프롬프트 속에 감추지 않고 명시적으로 표현할 수 있습니다. 조사, 도구 사용, 사람의 검토, 다중 에이전트 워크플로는 [예제](examples/README.md)를 참고하세요.

## 그래프가 상태를 변경하는 방식

`count`라는 채널에 `2`가 저장되어 있다고 합시다. 증가 노드는 `2`를 읽고 새 값으로 `3`을 제안하는 쓰기를 반환합니다. 런타임은 스케줄링된 노드 묶음의 실행이 끝난 뒤 채널의 리듀서를 통해 이 쓰기를 적용합니다. 다음 묶음에서 실행되는 후속 노드는 `3`을 읽습니다.

```text
Committed state       Node computation          Reduced state
count = 2       ->    read 2; propose 3     ->    count = 3
                                                  |
                                            next node reads 3
```

위 실행 흐름에 사용한 용어는 다음과 같습니다.

| 용어 | NeoGraph에서의 의미 |
|---|---|
| 노드(Node) | 호스트에 등록된 실행 단위입니다. 입력 상태를 읽고 채널 쓰기와 선택적인 라우팅 명령을 반환합니다. |
| 상태(State) | 현재 실행 단계에서 볼 수 있는 채널 값입니다. 설정에 따라 런타임이 소유하는 이력과 사용량·예산 회계 정보도 포함합니다. |
| 채널(Channel) | 이름이 붙은 값으로, 리듀서를 가지며 선택적으로 보존 정책과 체크포인트 영속화 정책을 설정할 수 있습니다. |
| 리듀서(Reducer) | 현재 채널 값과 새로 들어온 쓰기를 결합하는 함수입니다. `overwrite`는 값을 대체하고, `append`는 배열 원소를 누적하며, 사용자 정의 리듀서는 다른 결합 방식을 정의합니다. |
| 간선(Edge) | 노드 사이의 스케줄링 규칙입니다. 무조건 간선, 조건부 간선, 여러 선행 노드를 기다리는 배리어 간선이 있습니다. 순환 간선을 사용하면 단계를 반복할 수 있습니다. |
| 슈퍼스텝(Superstep) | 실행 준비가 된 노드들을 하나의 묶음으로 실행한 뒤, 그 쓰기를 적용하고 다음 실행 일정을 정하는 단계입니다. |

일반적인 실행 묶음에서 준비된 노드들은 묶음 실행 전의 채널 상태를 읽습니다. 한 노드가 `ChannelWrite`를 반환해도 같은 묶음의 다른 노드가 읽는 값은 즉시 바뀌지 않습니다. 실행이 성공적으로 완료되면 실행기가 결과를 적용하고 이후 단계에서 갱신된 상태를 읽습니다. 여러 분기로 구성된 `Send` 실행 묶음에서는 각 분기가 격리된 상태 복사본으로 입력을 받고, 실행 후 출력을 병합합니다. 이는 Pregel 방식으로 작업을 구성한 것입니다. NeoGraph의 채널과 리듀서 규칙은 NeoGraph 자체의 계약이며, Pregel의 모든 기능을 구현했다는 뜻은 아닙니다.

동시 실행이 모든 리듀서의 순서 독립성을 보장하지는 않습니다. 두 노드가 텍스트를 추가하거나 같은 채널을 덮어쓰면 쓰기 순서가 결과에 영향을 줍니다. 워크플로에 순서 독립성이 필요하다면 서로 독립된 채널이나 순서에 영향을 받지 않는 리듀서를 사용하세요. 채널의 보존 정책도 결합 방식과 별개입니다. 예를 들어 append 채널은 누적 값 중 정해진 범위의 마지막 부분만 유지할 수 있습니다. 스케줄링, 리듀서, 배리어, 취소에 관한 내용은 [개념](docs/concepts.md)과 [동시성](docs/concurrency.md)을 참고하세요.

## Core 예제 따라가기

[전체 C++ 빠른 시작 예제](examples/62_core_quickstart.cpp)는 문자열을 대문자로 바꾸는 노드를 등록하고 다음 토폴로지를 컴파일합니다.

```text
__start__ -> upper -> __end__

Input channel:   text = "hello"
Node reads:      "hello"
Node returns:    ChannelWrite{"text", "HELLO"}
Reducer:         overwrite
Output channel:  text = "HELLO"
```

노드 내부의 계산은 일반적인 C++ 코드입니다.

```cpp
class UpperNode final : public neograph::graph::GraphNode {
public:
    asio::awaitable<neograph::graph::NodeOutput> run(
        neograph::graph::NodeInput input) override {
        auto text = input.state.get(neograph::graph::ChannelKey<std::string>{"text"});
        for (auto& character : text)
            character = static_cast<char>(
                std::toupper(static_cast<unsigned char>(character)));
        co_return neograph::graph::NodeOutput{{
            neograph::graph::ChannelWrite{"text", neograph::json(std::move(text))}}};
    }
    std::string get_name() const override { return "upper"; }
};
```

전체 소스에는 헤더, 읽기·쓰기 채널 선언을 포함한 노드 등록, 토폴로지, `GraphEngine::build_strict`, 실행 입력, 타입이 지정된 출력 접근이 포함되어 있습니다. 모델 호출이나 API 키는 필요하지 않습니다. 예상 출력은 `HELLO`입니다.

### 빌드 및 실행

Core가 타입이 지정된 provider 계약을 공개하므로 `NEOGRAPH_BUILD_LLM=OFF`일 때도 외부 SDK인 SchemaProvider가 필요합니다. 아래 명령은 설치된 [SchemaProvider 런타임 패키지](https://github.com/fox1245/SchemaProvider)를 사용합니다. `SCHEMAPROVIDER_PREFIX`를 설치 접두 경로로 설정하세요. `-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=../SchemaProvider`로 명시한 체크아웃이 최우선으로 사용됩니다. 명시하지 않으면 CMake는 설치된 패키지를 우선 사용하고, 패키지를 찾지 못하면 고정된 버전의 공개 SDK 아카이브를 가져옵니다. 설치된 패키지나 명시적인 체크아웃으로 오프라인 빌드를 하려면 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`를 설정하세요. CMake는 이웃 디렉터리의 체크아웃을 추측하지 않으며, 제거된 내장 인터프리터도 사용하지 않습니다.

C++20 컴파일러, CMake 3.20 이상, OpenSSL과 libcurl 7.88 이상을 비롯한 SDK 런타임 의존성이 필요합니다. NeoGraph의 HTTPS 구성 요소를 포함하는 전체 빌드에는 OpenSSL 3이 필요합니다. 기본 빌드는 SQLite와 PostgreSQL 통합도 활성화합니다. 아래 명령은 SDK 의존성을 제거하지 않고 불필요한 NeoGraph 구성 요소를 비활성화합니다. 기록된 SDK 인터페이스 4 검증은 Linux x86_64와 로컬 프로토콜·상태 피어를 다룹니다. 정확한 범위는 [SDK 검증 기록](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/CONFORMANCE.md#interface-4-execution-record)에 있습니다. 이는 Windows, macOS, ARM64, HTTP/3, 호스팅 공급업체 또는 WASM에 대한 새로운 검증을 뜻하지 않습니다. 플랫폼 및 빌드 제약은 [문제 해결](docs/troubleshooting.md)을 참고하세요.

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

`GraphEngine`은 컴파일된 그래프를 실행합니다. 노드 스케줄링, 상태 갱신, 라우팅, 재시도, 스트리밍, 취소, 그래프 체크포인트 저장 및 재개를 담당합니다. 컴파일된 토폴로지는 불변입니다. 지원되는 세대 마이그레이션은 노드 실행 중 임의로 토폴로지를 변경하는 대신, 통제된 안전 지점에서 이루어집니다.

`ProgramRuntime`은 승인을 받은 Program을 조율합니다. Program은 Core 그래프를 호출하고 자식 Program을 관리할 수 있습니다. ProgramRuntime은 불변 Program 버전, 카탈로그와 정책 스냅샷, 명령 저널, 자식 계보, 예산, 재생, 승인된 교체 또는 마이그레이션을 추가로 제공합니다. 호스트는 실행 가능한 기능을 등록하고, Program을 컴파일하고 승인한 다음, 호출을 시작합니다. 그래프 노드를 실행하는 역할은 계속 Core가 맡습니다.

조사 워크플로에서는 하나의 Core 그래프가 검색과 검토를 수행할 수 있습니다. Program은 이 그래프를 호출하고, 별도 작업을 위한 자식 Program을 시작하고, 결과를 기다리며, 수명 주기 전이를 기록할 수 있습니다. 프로세스 종료 후에도 복구하려면 설정된 저장소와 관련 보관 책임 계약을 충족해야 합니다. 메모리 내 저장소는 프로세스가 종료되면 사라지며, 저널만으로는 외부 도구의 효과가 정확히 한 번만 발생하도록 보장할 수 없습니다.

QuickJS는 선택적으로 사용할 수 있는 Program 작성 인터페이스입니다. Program은 제한된 범위의 JavaScript 계산과 `callCore`, `spawn`, `await`, `all`, `parallel`, `race`, `quorum`, `emit`, `checkpoint`, `cancelScope` 같은 제너레이터 명령을 사용합니다. 호스트는 기능 사용을 승인하고 생성된 소스를 검증한 뒤 공개합니다. 모델이 생성한 제안에는 컴파일러, 자격 증명, 카탈로그, 권한 부여 기능에 대한 접근을 허용하지 않습니다.

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

[Program 빠른 시작 예제](examples/63_program_quickstart.cpp)는 증가 그래프를 호출하는 Program을 컴파일하고 승인합니다. 예상 출력은 `1`입니다. 이 예제는 메모리 내 저장소와 C++ Program 빌더를 사용합니다. JavaScript 작성과 영속 실행은 [작성 경계](docs/QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md), [재귀 Program](docs/PROGRAM_RECURSIVE_HARNESSES.md), [엄격한 런타임 계약](docs/STRICT_RUNTIME_INTERPOSITION.md)에서 시작하세요.

## 타입이 지정된 provider 호출

모델 호출은 검증된 디스크립터, 런타임 옵션, 타입이 지정된 요청을 사용합니다. 디스크립터 승인 과정은 허용된 필드가 고정되어 있고 버전이 지정된 데이터를 받아들입니다. 요청·응답 인터프리터를 실행하지는 않습니다. 자격 증명은 공개 디스크립터 파일이 아니라 런타임 옵션에 넣어야 합니다. `SchemaProvider::Defaults`에는 타입이 지정된 OpenRouter 라우팅 및 Responses 보존 제어가 들어 있습니다. Images, Veo, Decisions는 각각 별도의 타입 지정 클라이언트와 인가를 사용합니다.

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

준비된 요청은 한 번만 소비됩니다. `sp::runtime::Result`는 `Completion` 또는 `Failure`를 담은 불변 `sp::Outcome`을 소유합니다. 순서가 보존된 메시지와 파트, 네이티브 이어 실행 정보, 원시 관측값, 중단 근거, 시도 메타데이터, 실패 시 부분 결과가 필요하면 이 outcome을 유지하세요. 사용량 카운터는 null일 수 있습니다. 값이 없으면 알 수 없다는 뜻이며, 실제로 관측한 0은 그대로 0입니다. 사용량 보고와 예산 차감은 별도의 기록입니다. 이식 가능한 보고 데이터는 지출 권한을 부여할 수 없습니다.

`first_call`이 반환된 뒤에는 `std::get_if<sp::Completion>(result.get())`를 사용하여 완료 결과의 `messages`, `stop`, `usage`를 살펴볼 수 있습니다. 완료 결과가 아니라면 `std::get<sp::Failure>(*result)`로 `error.kind`, `error.safe_message`, 재시도 근거, `partial`에 담긴 부분 메시지와 사용량을 확인할 수 있습니다. 예를 들어 `completion.usage.output_total`이 없으면 출력 토큰 수를 알 수 없다는 뜻입니다. 카운터가 있고 그 `value`가 `0`이면 보고된 값은 0입니다. 표시용 텍스트는 보존된 outcome을 바라보는 여러 방식 중 하나일 뿐입니다.

`ChatMessage`, `ChatTool`, JSON은 이식 가능한 투영 표현입니다. 진본 네이티브 이력은 네이티브 체크포인트 사이드카와 함께 메모리에 유지할 수 있습니다. 네이티브 이력을 영속적으로 보존하려면 실제 `sp::NativeArchive`와 소유자만 접근할 수 있도록 보호된 보관 책임 체계가 필요합니다. 이식 가능한 JSON으로는 이 권한을 재현할 수 없습니다. 아카이브는 독립적인 키로 보관 책임의 진위를 인증합니다. 이는 암호화도, 공급업체 발급자 인증도 아닙니다. 아카이브 본문, 키, 네이티브 blob, 원시 통신 관측값을 공개하지 마세요. 영속화 실패, 관측자, 관리형 예산 은행, 재생 경계는 [provider 참조](docs/reference-en.md)와 [마이그레이션 가이드](docs/migration-v0.4-to-v1.0.md)를 참고하세요.

타입 지정 인터페이스로의 전환으로 `CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, 디스크립터 인터프리터, Responses WebSocket 경로가 제거됩니다. C++ 사용 코드를 다시 컴파일하고 사용자 정의 provider를 마이그레이션해야 합니다. 호환성 별칭은 제공하지 않습니다. SDK 패키지 버전은 `0.1.0`이며 인터페이스는 알파 단계입니다. 인터페이스 리비전은 4, 공유 ABI는 4입니다. 리비전이 일치해야 하며, 이 번호들이 SDK 인터페이스의 안정성을 선언하는 것은 아닙니다.

## Python

```bash
pip install neograph-engine
```

여기서 설명하는 타입 지정 provider API는 NeoGraph `0.13.0`을 대상으로 합니다. 이전 wheel은 구형 인터페이스를 제공합니다. [Python 바인딩 가이드](docs/python-binding.md)는 소스 API와 빌드에 필요한 조건을 설명합니다.

이 그래프에는 API 키가 필요하지 않습니다.

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

예상 메시지 내용은 `Hello, NeoGraph!`입니다. Python에서도 Program 컴파일 및 실행, Hooks, 런타임 컨텍스트 요구 사항, 엄격한 프로필, SQLite 영속성, 정확한 체크포인트 재개를 사용할 수 있습니다. 타입 지정 provider는 `ProviderMessage`와 `make_provider_request`를 사용한 다음 `prepare`/`dispatch` 또는 `invoke`를 호출합니다. outcome은 네이티브 객체가 소유하는 완료 또는 실패 근거를 유지합니다. 이 메시지는 그래프 편의 타입인 `ChatMessage` 값과 다릅니다. 블로킹 provider 호출은 GIL을 해제합니다. `asyncio.to_thread`를 사용하면 이벤트 루프 스레드 밖에서 실행할 수 있습니다. 완전한 provider 및 Program 입력은 [Python 예제](bindings/python/examples/README.md)를 참고하세요.

## 적합한 작업과 한계

NeoGraph는 명시적인 상태 전이, 분기 또는 반복, 병렬 작업, 체크포인트, 사람의 검토가 필요한 워크플로에 적합합니다. 작은 고정 그래프를 C++ 애플리케이션에 내장할 수도 있습니다. 노드의 계산은 애플리케이션이 책임집니다. 그래프 런타임은 모델을 학습시키거나 수치 계산 라이브러리를 대체하지 않으며, 모델 호출의 지연 시간, 가용성, 비용도 여전히 provider에 좌우됩니다.

런타임은 그래프 전체 및 노드별 재시도 정책, 크기가 제한된 재사용 가능 노드 캐시, `Send` 팬아웃, `Command` 라우팅, 서브그래프, 상태 이력, 포크, HITL, `NodeInterrupt`를 지원합니다. MCP, A2A, ACP, gRPC, 관측성 통합은 선택적 구성 요소입니다. 런타임 컨텍스트와 Hooks는 특정 디스패치 입력을 요구하고 전달 근거를 기록할 수 있습니다. 이러한 검사로 모델이 모든 토큰에 주의를 기울였다고 증명할 수는 없습니다. 영속 네이티브 복구와 예산이 제한된 포크에는 원래의 공유 회계 권한이 필요합니다. 스냅샷을 복사해도 예산이 갱신되지 않습니다.

실제 노드, provider, 저장소, 동시성, 빌드 프로필을 사용하여 워크로드를 측정하세요. [벤치마크](benchmarks/README.md)와 [성능 가이드](docs/performance-deep-dive.md)는 측정한 구성과 한계를 설명하며 보편적인 속도를 주장하지 않습니다. 단일 구성 빌드에서 최적화된 실행을 측정하려면 `CMAKE_BUILD_TYPE=Release`를 지정해야 합니다. `NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION=ON`은 지원되는 컴파일러에서 호스트별 튜닝을 추가합니다. 배포용 바이너리에서는 이 옵션을 끄세요.

## 빌드 설정과 추가 문서

| 옵션 | 용도 |
|---|---|
| `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` | SDK 체크아웃 경로를 명시합니다. 설치된 패키지 탐색과 아카이브 가져오기보다 우선합니다. |
| `NEOGRAPH_FETCH_SCHEMAPROVIDER` | 설치된 패키지가 없으면 고정된 버전의 공개 SDK 아카이브를 가져옵니다. 기본값은 켜짐입니다. 오프라인 빌드에서는 비활성화하세요. |
| `NEOGRAPH_BUILD_PROGRAM` | Program 런타임, 카탈로그, 계보, 마이그레이션을 활성화합니다. 기본값은 꺼짐입니다. |
| `NEOGRAPH_BUILD_QUICKJS_CONTROL` | 내장 QuickJS Program 작성 기능을 활성화합니다. 기본값은 꺼짐입니다. |
| `NEOGRAPH_BUILD_PYBIND` | Python 확장을 빌드합니다. 기본값은 꺼짐입니다. |
| `NEOGRAPH_BUILD_LLM` | NeoGraph 모델 호출 어댑터를 활성화합니다. 비활성화해도 SDK 의존성은 제거되지 않습니다. |
| `NEOGRAPH_BUILD_SQLITE` / `NEOGRAPH_BUILD_POSTGRES` | 선택적인 영속 저장소를 활성화합니다. 둘 다 기본값은 켜짐입니다. |
| `NEOGRAPH_BUILD_MCP_CLIENT` / `NEOGRAPH_BUILD_MCP_SERVER` | MCP 클라이언트와 서버 구성 요소를 활성화합니다. |
| `NEOGRAPH_BUILD_A2A` / `NEOGRAPH_BUILD_ACP` / `NEOGRAPH_BUILD_GRPC` | 프로토콜 통합을 활성화합니다. gRPC의 기본값은 꺼짐입니다. |
| `NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION` | 최적화 구성에 이식 불가능한 호스트별 튜닝을 적용합니다. 기본값은 꺼짐입니다. |

설치된 패키지를 사용하는 코드는 필요한 활성 구성 요소만 링크합니다.

```cmake
find_package(SchemaProvider 0.1.0 CONFIG REQUIRED COMPONENTS runtime)
find_package(NeoGraph CONFIG REQUIRED)
target_link_libraries(app PRIVATE neograph::core neograph::llm SchemaProvider::runtime)
```

- [개념과 그래프 의미론](docs/concepts.md)
- [C++ 참조](docs/reference-en.md) 및 [Python 바인딩 가이드](docs/python-binding.md)
- [비동기 가이드](docs/ASYNC_GUIDE.md) 및 [동시성/취소](docs/concurrency.md)
- [런타임 컨텍스트와 엄격한 interposition](docs/STRICT_RUNTIME_INTERPOSITION.md)
- [Harness MCP](docs/HARNESS_MCP.md) 및 [QuickJS 작성](docs/QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)
- [마이그레이션 가이드](docs/migration-v0.4-to-v1.0.md) 및 [문제 해결](docs/troubleshooting.md)
- [C++ 예제](examples/README.md), [Python 예제](bindings/python/examples/README.md), [벤치마크 방법론](benchmarks/README.md)

## 라이선스

MIT입니다. [LICENSE](LICENSE)를 참고하세요. 타사 라이선스 고지는 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)에 있습니다.
