<!-- neograph-i18n: source=docs/doxygen-mainpage.md locale=ko source_sha256=e7253d2aacfe88e0cb5c6ef9f02834b805e0ddcfb739e7c9882235f90eba9b52 -->
# NeoGraph C++ API 레퍼런스 {#mainpage}

**Languages:** [English](doxygen-mainpage.md) | [한국어](doxygen-mainpage.ko.md) | [日本語](doxygen-mainpage.ja.md) | [简体中文](doxygen-mainpage.zh-CN.md)

C++20 그래프 에이전트 엔진 라이브러리입니다. C++용 LangGraph에 해당하며, Python 바인딩은 선택 사항입니다. 이 사이트는 `include/neograph/`의 공개 C++ 헤더를 바탕으로 **자동 생성한 레퍼런스**입니다.

## 시작하기

NeoGraph를 처음 쓴다면 **설명 위주의 문서를 먼저 읽으세요**. 이 자동 생성 레퍼런스는 찾는 대상이 무엇인지 알고 있을 때 클래스 시그니처를 조회하는 용도입니다.

| 목적 | 이동할 곳 |
|---|---|
| NeoGraph가 무엇이고 왜 만들었는지, 벤치마크 | [README](https://github.com/fox1245/NeoGraph#readme) |
| 기본 개념 — 채널, 노드, 엣지, Send, Command | [핵심 개념](https://github.com/fox1245/NeoGraph/blob/master/docs/concepts.md) |
| 자주 겪는 문제를 증상부터 찾아 해결 | [문제 해결](https://github.com/fox1245/NeoGraph/blob/master/docs/troubleshooting.md) |
| C++ 예제 (검증 결과는 별도로 보고) | [examples/](https://github.com/fox1245/NeoGraph/tree/master/examples) |
| Python의 타입 기반 provider 및 그래프 예제 | [bindings/python/examples/](https://github.com/fox1245/NeoGraph/tree/master/bindings/python/examples) |
| 비동기 / 코루틴 내부 구조 | [ASYNC_GUIDE](https://github.com/fox1245/NeoGraph/blob/master/docs/ASYNC_GUIDE.md) |

## 최상위 헤더

편의 헤더 하나로 core 전체와 그래프 엔진 API를 모두 가져옵니다.

```cpp
#include <neograph/neograph.h>

using namespace neograph;
using namespace neograph::graph;
```

하위 네임스페이스는 다음과 같습니다.

- `neograph` — 기본 타입(`Provider`, `Tool`, `ChatMessage`)
- `neograph::graph` — 엔진, 노드, 상태, 체크포인트 저장
- `neograph::llm` — `SchemaProvider`, `Agent`, 타입 기반 SDK 런타임
- `neograph::mcp` — Model Context Protocol 클라이언트
- `neograph::async` — 코루틴과 io_context 기반 구조
- `neograph::util` — 동시성 기본 요소

## 첫 프로그램

```cpp
#include <neograph/llm/schema_provider.h>
#include <neograph/types.h>

sp::runtime::Result first_call(
    sp::descriptor::ValidatedDescriptor descriptor, sp::runtime::Options options,
    std::string model) {
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

`SchemaProvider`는 승인(admission)된 `sp::descriptor::ValidatedDescriptor`, `sp::runtime::Options`, 선택 사항인 `SchemaProvider::Defaults`를 받습니다. descriptor를 불러오는 일은 닫힌 형식의 버전이 있는 데이터를 승인(admission)하는 과정이며, 요청/응답 인터프리터나 임의 primitive 레지스트리가 아닙니다. 자격 증명은 공개 descriptor 파일이 아니라 런타임 옵션에 넣어야 합니다. Defaults에는 타입이 있는 OpenRouter 라우팅과 Responses 보존(`responses_store`)만 들어가며, 후자는 Responses에서만 유효합니다. 호스팅되는 OpenRouter의 라우팅, 보존, JSON 형식은 계속 선언된 타입 제어 항목입니다. Images, Veo, Decisions는 NeoGraph의 별도 타입 클라이언트와 별도 권한을 사용하며, SDK chat grant를 물려받지 않습니다.

provider 호출은 `sp::runtime::Result`를 반환합니다. 이는 `sp::Completion` 또는 `sp::Failure`를 담은, 바뀌지 않고 소유권이 있는 `std::shared_ptr<const sp::Outcome>`입니다. 화면에 표시할 텍스트만 챙기지 말고 결과 전체를 보관해야 합니다. 순서가 있는 메시지/파트, 유지되는 네이티브 continuation과 계열이 제공한 wire 증거, 순서가 있는 원시 관찰값, stop 증거, 실제 시도 메타데이터는 호출이 끝나고 클라이언트가 소멸한 뒤에도 남습니다. 사용량 카운터는 증거, 단계, 품질을 가진 null 가능한 `uint64_t` 값입니다. 값이 없으면 알 수 없다는 뜻이며 0이 아닙니다. 실패한 경우에도 원래의 부분 결과가 보존됩니다. `ProviderFailure::outcome()`과 `ProviderObserverError::outcome()`이 그 결과를 돌려주고, 후자는 observer 예외도 `cause()`에 보존합니다.

wire 증거는 계열이 제공하는 선택 사항입니다. `sp::Completion::wire_envelope`는 null일 수 있습니다(Python에서는 `ProviderCompletion.wire_envelope`가 `None`입니다). 버퍼링 방식 Chat은 현재 전체 응답 JSON을 `raw_events`에 `type == "chat.completion"`인 `RawWire`로 보관하며, 문서는 `payload`에 들어 있고 `wire_envelope`는 null로 둡니다. 증거는 계열이 보관하는 위치에서 읽어야 하며, 대체 envelope를 만들어 내지 않습니다. 네이티브 continuation과 raw 버퍼는 보호되는 증거로 남으며 트레이스 payload에서는 제외됩니다.

이 변경은 소스 호환과 바이너리 호환을 모두 깨뜨립니다. 모든 C++ 소비자와 사용자 정의 provider를 새 헤더·라이브러리에 맞춰 다시 컴파일해야 합니다. `CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, descriptor 인터프리터, Responses WebSocket 경로는 제거되었고 별칭이나 호환용 브리지도 없습니다. 릴리스 대상은 SDK `0.1.1` 알파, 인터페이스 리비전 4 / 공유 ABI 4이며 기능 검사는 out-of-line으로 합니다. 알파는 안정 API를 약속하지 않습니다. 기록된 interface 3의 런타임/archive 자격 검증은 Linux/POSIX를 대상으로 했으며, interface 4나 Windows, macOS, WASM의 런타임 검증은 포함하지 않습니다.

Python에서는 `SchemaProvider(ValidatedDescriptor, ProviderRuntimeOptions, SchemaProviderDefaults)`와 `make_provider_request`를 쓰고, 메시지는 타입이 있는 `ProviderMessage` 파트로 만듭니다. `prepare`는 `PreparedProviderRequest`를 반환하고, `dispatch`는 이를 한 번만 소비합니다. `invoke`는 요청을 준비하고 dispatch까지 합니다. `ProviderOutcome`은 변경할 수 없는 completion/failure/partial 뷰를 보존하며, 알 수 없는 사용량은 `None`입니다. 그래프의 `ChatMessage`는 별도의 편의 타입이며 provider 메시지의 별칭이 아닙니다. 블로킹 provider 호출은 GIL을 놓습니다. Python 비동기 호출자는 `asyncio.to_thread`를 쓸 수 있습니다. 생성자, 오류, 제한 사항은 [Python 바인딩 가이드](python-binding.md)를 참고하세요.

실제 결과가 나온 뒤 효과 후 회계(post-effect accounting)나 최종 receipt 저장에 실패하면, `ProviderDispatchOutcomePersistenceError`가 원래의 변경 불가능한 결과를 `outcome()`에, 원래의 저장 예외를 `cause()`에 보존합니다. 전달(delivery)도 실패했다면 `delivery_error()`가 원래의 observer 예외를 보존합니다. 저장은 성공했지만 그 뒤 observer가 실패하면 원래의 observer 예외를 그대로 다시 던집니다. 결과가 없거나 알 수 없는 전송 실패에서는 결과를 만들어 내지 않습니다.

## 참조 색인

사이드바의 클래스 목록, 파일 목록, 네임스페이스 목록은 `include/neograph/` 아래의 헤더에서 생성됩니다. [클래스 목록](annotated.html)이 가장 쓸모 있는 진입점입니다.

## 소스

프로젝트 홈: <https://github.com/fox1245/NeoGraph>

라이선스: MIT.
