<!-- neograph-i18n: source=docs/ASYNC_GUIDE.md locale=ko source_sha256=6683a6d1c43ac965183efed84bcb41bd6eb07d6f3cd7aa66f84de0dd8b61dea3 -->
# NeoGraph 비동기 가이드

**Languages:** [English](ASYNC_GUIDE.md) | [한국어](ASYNC_GUIDE.ko.md) | [日本語](ASYNC_GUIDE.ja.md) | [简体中文](ASYNC_GUIDE.zh-CN.md)

## 실행 진입점

`GraphEngine::run`, `run_stream`, `resume`은 동기 브리지를 거쳐 대응하는 코루틴을 구동합니다. `run_async`, `run_stream_async`, `resume_async`는 `asio::awaitable<RunResult>`를 반환하고 호출자의 executor를 사용합니다. 작업이 끝날 때까지 그 executor를 계속 구동해야 하며, 콜백이 아직 쓰고 있는 리소스를 먼저 파괴하면 안 됩니다.

`EngineConfig::worker_count`의 기본값은 `1`이며, 엔진이 소유하는 fan-out 풀은 없습니다. I/O 위주의 분기는 대기(suspend)할 때 서로 겹쳐 실행될 수 있지만, executor 스레드 하나에서 실행되는 CPU 위주의 본문은 직렬로 실행됩니다. 이런 본문이 여러 코어를 써야 한다면 엔진을 공개하기 전에 워커 풀을 설정합니다. [동시성 문서](concurrency.md)와 `examples/27_async_concurrent_runs.cpp`를 참고하세요.

## 준비된 provider 호출

`SchemaProvider`는 닫힌 `sp::descriptor::ValidatedDescriptor`, `sp::runtime::Options`, 그리고 선택 사항인 타입이 있는 `SchemaProvider::Defaults`로 만듭니다. 런타임은 libcurl을 사용하며, 예전의 descriptor 인터프리터, `prefer_libcurl` 선택자, Responses WebSocket 경로는 제거되었습니다.

`make_provider_request`는 계열별로 타입이 있는 payload를 만듭니다. `ProviderMode::Collect`가 기본값이며, `Stream`은 `on_event`와 별개로 명시적으로 선택합니다. `prepare`는 검증과 인코딩을 한 번 수행합니다. `dispatch(_async)`는 이동만 가능한 핸들을 한 번 소비하고, `invoke(_async)`는 준비와 디스패치를 한 번에 합니다. 반환된 awaitable이 요청과 클라이언트 상태를 소유하므로, 원래의 요청과 Provider는 스케줄링이 끝날 때까지 살아 있지 않아도 됩니다.

```cpp
#include <neograph/async/run_sync.h>
#include <neograph/provider.h>

// provider owns a validated descriptor and SDK runtime policy.
auto request = neograph::make_provider_request(
    *provider, model, messages, {}, {}, neograph::ProviderMode::Collect);
request.cancel_token = cancel_token;
request.options.deadline = deadline;
auto prepared = provider->prepare(std::move(request));
auto result = neograph::async::run_sync(
    provider->dispatch_async(std::move(prepared)));
```

C++의 `on_event`는 빌린 `sp::Event` 뷰를 받으므로, 콜백이 끝난 뒤에도 필요한 바이트는 복사해 두어야 합니다. 이벤트에는 텍스트뿐 아니라 사용량, reasoning, 도구, raw wire 관찰값도 포함됩니다. 변경할 수 없는 `sp::runtime::Result`가 `sp::Completion` 또는 `sp::Failure`를 소유하며, 여기에는 부분 실패 증거도 들어 있습니다. observer가 실패하면 그 결과는 `ProviderObserverError::outcome()`에, 콜백의 예외는 `cause()`에 보존됩니다.

디스패치는 용량이 1인 병합(coalesced) 알림을 기다립니다. 이벤트와 결과는 크기가 제한된 Bridge가 보관하며, 알림은 이벤트 큐가 아닙니다. 취소는 중지 요청을 SDK에 전달합니다. `operation.join()`은 observer나 리소스 실패가 있는 경우를 포함해 콜백 반환과 승인(admission) 슬롯 해제를 보장하는 경계입니다. 데드라인은 작업 시간을 제한하지만, 호출자를 멈췄다고 해서 서버가 요청을 받지 못했다는 것이 증명되지는 않습니다.

영속성을 갖춘 디스패치를 하려면 `Provider::request_digest()`를 assembly에 바인딩하고, 승인(admission)된 청구권(claim)을 예약하고 receipt를 저장한 뒤에, 같은 핸들을 `ControlledProvider::dispatch_prepared(_async)`로 소비합니다. receipt가 중복되었다고 해서 다시 디스패치해도 되는 것은 아닙니다. 예약은 이미 승인(admission)된 지출 권한에서 차감하거나 그만큼을 묶어 두는 것이며, provider가 보고한 사용량이나 청구서가 아닙니다. 없는 카운터는 알 수 없는 값으로 남습니다. 전달 여부를 알 수 없거나 부분적인 증거로는 해소되지 않은 hold를 해제할 근거가 되지 못합니다.

전체 메시지는 실제 네이티브 이어 가기 정보를 메모리에 보관합니다. 이식 가능한 JSON 투영에는 관찰 내용만 있고 네이티브 재생(replay) 권한이나 금전 권한은 없습니다. 네이티브 이어 가기는 승인(admission)된 `sp::NativeArchive`로만 영구 저장해야 하며, 텍스트나 원시 JSON에서 복원하면 안 됩니다. [마이그레이션 가이드](migration-v0.4-to-v1.0.md)를 참고하세요.

## 사용자 정의 provider와 노드

Provider 서브클래스가 구현하는 것은 `get_name`, `family`, `prepare`뿐입니다. 공통 invoke/dispatch 메서드는 completion을 가로채는 가상 훅이 아닙니다.

```cpp
#include <neograph/provider.h>

class MyProvider final : public neograph::Provider {
    std::string family_;
    std::shared_ptr<sp::runtime::Client> client_;
public:
    MyProvider(sp::descriptor::ValidatedDescriptor descriptor,
               sp::runtime::Options options)
        : family_(descriptor.family()),
          client_(std::make_shared<sp::runtime::Client>(
              std::move(descriptor), std::move(options))) {}
    std::string get_name() const override { return "my-provider"; }
    std::string_view family() const noexcept override { return family_; }
    neograph::PreparedProviderRequest prepare(neograph::ProviderRequest request) override {
        return prepare_runtime(client_, std::move(request));
    }
};
```

`prepare_local`은 실제 로컬 디스패치 상태를 소유한 C++ 어댑터가 쓸 수 있습니다. 캡처할 때는 빌린 `this`가 아니라 소유한 공유 상태를 캡처해야 합니다. Python 서브클래스는 준비 작업을 실제 provider에 위임하며, 결과나 로컬 디스패치 권한을 위조할 수 없습니다.

사용자 정의 그래프 노드는 `run(NodeInput) -> asio::awaitable<NodeOutput>`을 오버라이드합니다. `in.state`와 `in.ctx`를 읽고, 이벤트는 null이 아닌 `in.stream_cb`로만 내보내며, 쓰기/Command/Send는 `NodeOutput` 하나에 담아 반환합니다. 엔진은 동기 스트리밍이든 비동기 스트리밍이든 디스패치마다 노드를 한 번만 호출합니다. 전체 provider 기록에는 `RunConfig::provider_messages`를 사용합니다. `ChatMessage`는 그래프에서 편하게 쓰는 타입일 뿐 SDK의 메시지 모델이 아닙니다.

<a id="94-checkpointstore"></a>
## CheckpointStore 어댑터

`CheckpointStoreCore`에는 save, load_latest, load_by_id, list, delete_thread의 동기 연산 다섯 개가 필요합니다. `adapt_checkpoint_store`는 엔진이 요구하는 계약을 제공하고 블로킹 작업을 개수가 제한된 워커에 넘깁니다. `AsyncCheckpointStore`에는 이에 대응하는 비동기 연산 다섯 개가 필요하며, `adapt_async_checkpoint_store`는 명시적인 동기 관리용 파사드를 제공합니다.

예전 `CheckpointStore`의 동기 기본 구현은 `std::logic_error`로 실패하고, 비동기 기본 구현은 동기 오버라이드를 워커에 넘깁니다. 비동기만 지원하는 예전 서브클래스는 명시적인 비동기 capability/어댑터로 옮겨야 합니다. 인메모리 연산은 호출자 스레드에서 뮤텍스로 보호되는 저장소를 쓰고, SQLite는 블로킹 작업을 워커에 넘기며, PostgreSQL은 파이프라인 배치 없이 논블로킹 libpq I/O를 사용합니다.

대기 중인 쓰기(pending write)의 영속성은 별도의 `PendingWritesCheckpointStore` capability입니다. 이 capability가 없으면 재개할 때 슈퍼스텝 전체를 재생(replay)하며, no-op 메서드는 외부 효과의 중복을 제거하지 않습니다. Python 체크포인트 서브클래스는 동기 메서드를 구현하고, 비동기 네이티브 백엔드는 C++ 어댑터가 호스팅합니다.

<a id="95-mcpclient"></a>
## MCPClient

`rpc_call`은 `rpc_call_async`를 동기 방식으로 구동합니다. MCPClient는 사용자 정의 전송 계층을 위한 서브클래스 인터페이스가 아닙니다. HTTP 호출은 서로 겹쳐 실행됩니다. stdio 세션 하나는 프레임 쓰기를 직렬화하고, 읽기 담당 하나가 JSON-RPC id로 응답을 분배합니다. 대기자 하나를 취소해도 공유 전송 계층은 닫히지 않습니다. 직렬로 동작하는 하위 프로세스는 여전히 처리량을 제한합니다.

<a id="96-tool-vs-asynctool"></a>
## Tool과 AsyncTool

동기 Tool은 `execute`, `get_definition`, `get_name`을 구현합니다. AsyncTool은 `execute_async`, `get_definition`, `get_name`을 구현하며, 동기 `execute`는 final이고 전용 `run_sync` 컨텍스트를 구동합니다. 두 인터페이스를 모두 오버라이드하면 안 되고, 오래 걸리는 동기 작업으로 공유 이벤트 루프 스레드를 막아서도 안 됩니다.

## 범용 HTTP 스트리밍

NeoGraph의 `async_post_stream`은 기존 사용처가 쓰는 별도의 범용 HTTP 유틸리티로 남아 있으며, SDK의 libcurl 디스패치가 이를 대체하지 않습니다. chunked, 크기가 제한된 `Content-Length`, 연결 종료로 끝나는(close-delimited) 응답 본문을 지원합니다. 비어 있지 않은 고정 길이 본문은 콜백에 한 번 전달되고, 길이가 0인 본문은 콜백을 호출하지 않습니다. 반환되는 `HttpStreamResponse.status`는 200과 2xx가 아닌 상태를 그대로 보존하므로, 호출자는 SSE가 아닌 응답을 빈 성공으로 취급하지 않고 JSON 오류 본문을 해석할 수 있습니다.

`RequestOptions`의 기본값은 상태/헤더 64 KiB, 디코딩된 본문 16 MiB, 전송 청크 1 MiB 한도이며, 0이면 해당 한도를 끕니다. 고정 길이 분기는 메모리를 할당하기 전에 본문 한도를 검사합니다. 올바르지 않거나 모호한 프레이밍, 너무 이른 EOF, 이미 버퍼에 들어온 초과 바이트는 거부합니다. 반환된 뒤에 도착하는 바이트까지 감지한다는 약속은 아닙니다. 리다이렉트 본문은 따로 처리합니다. 홉(hop)별 타임아웃의 기본값은 0이고 리다이렉트는 꺼져 있습니다. `connect_timeout`(기본값 0, 제한 없음)은 `async_post`, `async_get`, `async_post_stream`, `async_request_stream`의 이름 해석, TCP 연결, TLS 핸드셰이크만 제한하며, 풀(pool)은 이 값을 읽지 않습니다. 이 범용 옵션은 SDK 정책도, 모델 지출 허용(spending grant)도 아닙니다.

## Python과 수명 경계

Python은 타입이 있는 `ProviderRequest`, `PreparedProviderRequest`, 변경할 수 없는 `ProviderOutcome`과 함께 `prepare`, `dispatch`, `invoke`를 export합니다. invoke/dispatch는 GIL을 놓고, 콜백과 Python 객체의 소멸은 GIL을 잡습니다. asyncio 경계에서는 `asyncio.to_thread(provider.invoke, request)`를 사용합니다. C++ provider의 awaitable을 asyncio awaitable로 자동 변환하는 기능은 없습니다. [Python 바인딩](python-binding.md)을 참고하세요.

`run_sync`는 호출마다 전용 단일 스레드 컨텍스트를, `run_sync_pool`은 호출마다 풀을 만듭니다. 이 executor에 바인딩된 핸들은 그 호출 밖으로 나가면 안 됩니다. 코루틴이 다른 awaitable을 반환할 때는 일반 함수로서 직접 반환하거나, 코루틴 본문에서 `co_return co_await`를 쓸 수 있습니다. 그 본문에 일반 `return`을 섞어서는 안 됩니다.

## 빌드

`neograph::core`는 `NEOGRAPH_BUILD_LLM=OFF`일 때도 외부 `SchemaProvider::runtime`을 필요로 합니다. 설치된 SDK의 prefix를 쓰거나 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`을 지정합니다. SDK를 빌드하려면 CMake 3.20 이상, C++20, Python, 독립형 Asio, yyjson, libcurl 7.88 이상, OpenSSL Crypto가 필요합니다. WASM 런타임 통합은 확인되지 않았습니다.

NeoGraph를 구성하려면 CMake 3.20 이상이 필요합니다. 명시적으로 지정한 SDK 소스 디렉터리를 먼저 쓰고, 없으면 설치된 패키지를, 그것도 없으면 리비전이 고정된 공개 archive를 기본 대체 경로로 씁니다. 설치된 SDK로 오프라인 구성을 하려면 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`로 설정하고 `CMAKE_PREFIX_PATH`를 그 SDK의 prefix로 지정합니다. 내려받은 SDK나 소스 SDK는 NeoGraph에 포함된 Asio와 yyjson을 쓰지만, 시스템 개발 의존성은 여전히 필요합니다.
