<!-- neograph-i18n: source=docs/doxygen-mainpage.md locale=ko source_sha256=f92342d3e71ccd35386584d6ec5ccdd2d61c6e6241fc26c74f884f70b4f3e934 -->
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
| C++ 예제 (검증은 별도 보고) | [examples/](https://github.com/fox1245/NeoGraph/tree/master/examples) |
| Python 예제 (provider 포팅 유예) | [bindings/python/examples/](https://github.com/fox1245/NeoGraph/tree/master/bindings/python/examples) |
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
- `neograph::llm` — `SchemaProvider`, `Agent`; typed SDK runtime
- `neograph::mcp`      — Model Context Protocol 클라이언트
- `neograph::async`    — 코루틴 + io_context 인프라
- `neograph::util`     — 동시성 기본 요소

## 첫 번째 프로그램

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

`SchemaProvider`는 승인된 `sp::descriptor::ValidatedDescriptor`, `sp::runtime::Options`, 선택적 `SchemaProvider::Defaults`를 받는다. descriptor는 closed/versioned 데이터 admission이지 요청/응답 interpreter나 임의 primitive registry가 아니다. credential은 공개 descriptor가 아니라 runtime options에 둔다. Defaults는 typed OpenRouter routing과 Responses 보관(`responses_store`)만 포함하고 후자는 Responses에만 유효하다. Hosted OpenRouter routing·retention·JSON 형식은 선언된 typed 제어다. Images, Veo, Decisions는 별도 NeoGraph typed client와 별도 승인을 쓰며 SDK chat grant를 물려받지 않는다.

공급자 호출은 `sp::runtime::Result`, 즉 `sp::Completion` 또는 `sp::Failure`를 담은 불변 소유 `std::shared_ptr<const sp::Outcome>`를 반환한다. 표시 텍스트만이 아니라 전체 결과를 보존한다. 순서 있는 메시지/파트, native continuation, 전체 wire envelope, 순서 있는 raw 관측, 중단 근거와 실제 시도 메타데이터는 호출 및 클라이언트 소멸 후에도 남는다. 사용량은 근거·단계·품질을 갖는 nullable `uint64_t`이며 누락은 0이 아니라 미상이다. 실패도 원래의 부분 결과를 보존한다. `ProviderFailure::outcome()`과 `ProviderObserverError::outcome()`은 실제 결과를 보존하며 후자의 `cause()`에는 관측자 예외가 남는다.

소스 및 바이너리 단절이다. 모든 C++ 소비자와 사용자 공급자를 새 헤더/라이브러리로 재컴파일한다. `CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, descriptor interpreter와 Responses WebSocket은 alias/호환 bridge 없이 제거되었다. SDK는 불안정 `0.0.0`, interface revision 3 / shared ABI 3이며 out-of-line capability check를 사용한다. 안정 릴리스 선언이 아니다. 현재 runtime/archive는 Linux/POSIX이며 Windows·macOS·WASM runtime 검증을 뜻하지 않는다. Python provider binding/wrapper는 유예되었고 이 C++ 변경으로 포팅되지 않는다.


실제 결과 이후 post-effect 정산이나 terminal receipt 영속화가 실패하면 `ProviderDispatchOutcomePersistenceError`의 `outcome()`은 원래 불변 결과를, `cause()`는 원래 영속 예외를 보존한다. 전달도 실패했으면 `delivery_error()`가 원래 관측자 예외를 보존한다. 영속화 성공 뒤 관측자 실패는 원래 예외를 그대로 다시 던진다. 미상/결과 없는 transport 실패는 결과를 조작하지 않는다.
## 참조 색인

사이드바의 클래스 목록, 파일 목록, 네임스페이스 목록은 `include/neograph/` 아래의 헤더에서 생성됩니다. [클래스 목록](annotated.html)이 가장 유용한 진입점입니다.

## 소스

프로젝트 홈: <https://github.com/fox1245/NeoGraph>

라이선스: MIT.
