<!-- neograph-i18n: source=docs/ASYNC_GUIDE.md locale=ko source_sha256=a48e529a151529d9711ee297aed8bc4b7e603c0198c596188608cb6a8565dc13 -->
# NeoGraph 비동기 가이드

**Languages:** [English](ASYNC_GUIDE.md) | [한국어](ASYNC_GUIDE.ko.md) | [日本語](ASYNC_GUIDE.ja.md) | [简体中文](ASYNC_GUIDE.zh-CN.md)

## 실행 진입점

`GraphEngine::run`, `run_stream`, `resume`은 동기 브리지로 코루틴 구현을 실행한다. `run_async`, `run_stream_async`, `resume_async`는 `asio::awaitable<RunResult>`를 반환하며 호출자의 executor를 쓴다. 작업이 끝날 때까지 executor를 구동하고 콜백이 쓰는 자원을 먼저 파괴하지 않는다.

`EngineConfig::worker_count`의 기본값은 `1`이며 엔진 소유 fan-out 풀이 없다. I/O 분기는 일시 중단 시 겹쳐 실행될 수 있지만 단일 executor 스레드의 CPU 작업은 직렬 실행된다. 여러 코어가 필요하면 엔진을 공개하기 전에 풀을 설정한다. [동시성](concurrency.md)과 `examples/27_async_concurrent_runs.cpp`를 참고한다.

## 준비된 공급자 호출

`SchemaProvider`는 닫힌 `sp::descriptor::ValidatedDescriptor`, `sp::runtime::Options`, 선택적인 typed `SchemaProvider::Defaults`로 만든다. 런타임은 libcurl을 쓴다. 과거 descriptor interpreter, `prefer_libcurl` 선택자, Responses WebSocket 경로는 제거되었다.

`make_provider_request`는 family별 typed payload를 만든다. 기본 모드는 `ProviderMode::Collect`이며 `on_event`와 별개로 `Stream`을 명시한다. `prepare`는 한 번 검증·인코딩하고 `dispatch(_async)`는 이동 전용 핸들을 한 번 소비한다. `invoke(_async)`는 두 단계를 결합한다. 반환된 awaitable은 요청/client 상태를 소유하므로 원래 요청과 Provider가 스케줄링 이후까지 살아 있을 필요는 없다.

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

C++ `on_event`는 빌린 `sp::Event` view를 받으므로 콜백 이후 필요한 바이트는 복사한다. 이벤트에는 텍스트 외에 사용량, 추론, 도구, raw wire 관측이 있다. 불변 `sp::runtime::Result`는 `sp::Completion` 또는 부분 실패 근거를 포함한 `sp::Failure`를 소유한다. 관측자 실패 시 `ProviderObserverError::outcome()`에 결과, `cause()`에 콜백 예외가 남는다.

dispatch는 용량 1의 병합 알림을 기다리고 bounded Bridge가 이벤트와 결과를 보존한다. 알림 자체가 이벤트 큐는 아니다. 취소는 SDK에 stop을 전달하며 `operation.join()`은 관측자/자원 실패 시에도 콜백 반환과 admission 슬롯 해제를 기다린다. deadline은 작업을 제한하지만 호출자 중단이 서버 미수신을 증명하지는 않는다.

영속 dispatch는 `Provider::request_digest()`를 assembly에 바인딩하고 승인된 claim을 예약한 뒤 receipt를 저장하고 같은 핸들을 `ControlledProvider::dispatch_prepared(_async)`로 소비한다. 중복 receipt는 재전송 권한이 아니다. 예약은 이미 승인된 지출 권한을 차감하거나 보류하며 공급자 보고 사용량이나 청구서가 아니다. 누락된 counter는 미상이며 부분/전달 미상 근거로 미해결 hold를 해제할 수 없다.

전체 메시지는 메모리에서 진짜 native continuation을 보존한다. portable JSON projection은 관측값이지 native replay나 재정 권한이 아니다. native continuation은 승인된 `sp::NativeArchive`로만 저장하며 텍스트나 raw JSON으로 재구성하지 않는다. [이전 가이드](migration-v0.4-to-v1.0.md)를 참고한다.

## 사용자 공급자와 노드

Provider 하위 클래스는 `get_name`, `family`, `prepare`만 구현한다. 공통 invoke/dispatch는 가상 completion hook이 아니다.

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

C++ adapter는 실제 local-dispatch 상태를 소유할 때 `prepare_local`을 쓸 수 있다. 빌린 `this` 대신 소유 shared 상태를 캡처한다. Python 하위 클래스는 진짜 provider에 준비를 위임하며 결과나 local-dispatch 권한을 조작할 수 없다.

사용자 그래프 노드는 `run(NodeInput) -> asio::awaitable<NodeOutput>`를 구현한다. `in.state`, `in.ctx`를 읽고 null이 아닌 `in.stream_cb`로만 이벤트를 내보내며 하나의 `NodeOutput`에 writes/Command/Send를 반환한다. 동기·비동기 streaming 모두 dispatch당 한 번 호출된다. 전체 공급자 기록은 `RunConfig::provider_messages`로 전달한다. `ChatMessage`는 그래프 편의 타입이며 SDK 메시지 모델이 아니다.

<a id="94-checkpointstore"></a>
## CheckpointStore 어댑터

`CheckpointStoreCore`는 save, load_latest, load_by_id, list, delete_thread의 다섯 동기 연산을 요구한다. `adapt_checkpoint_store`는 엔진 계약을 제공하고 blocking 작업을 제한된 worker로 넘긴다. `AsyncCheckpointStore`는 다섯 async 대응 연산을 요구하며 `adapt_async_checkpoint_store`가 명시적인 동기 관리 facade를 제공한다.

레거시 `CheckpointStore`의 동기 기본값은 `std::logic_error`로 실패하고 async 기본값은 동기 override를 offload한다. async-only 하위 클래스는 명시적 async capability/adapter로 이전해야 한다. 메모리 저장소는 호출자에서 mutex를 쓰고 SQLite는 blocking 작업을 offload하며 PostgreSQL은 pipeline batching 없이 nonblocking libpq I/O를 쓴다.

pending-write 내구성은 별도 `PendingWritesCheckpointStore` capability다. 없으면 resume은 전체 super-step을 재실행한다. no-op 메서드는 외부 효과를 중복 제거하지 않는다. Python checkpoint 하위 클래스는 동기 메서드를 구현하며 async-native backend는 C++ adapter가 맡는다.

<a id="95-mcpclient"></a>
## MCPClient

`rpc_call`은 `rpc_call_async`를 동기로 구동한다. MCPClient는 사용자 transport 상속 인터페이스가 아니다. HTTP 호출은 겹쳐 실행된다. stdio 세션은 frame 쓰기를 직렬화하고 단일 reader가 JSON-RPC id로 응답을 전달한다. waiter 하나의 취소가 shared transport를 닫지 않는다. 직렬 subprocess의 처리량 한계는 남는다.

<a id="96-tool-vs-asynctool"></a>
## Tool과 AsyncTool

동기 Tool은 `execute`, `get_definition`, `get_name`을 구현한다. AsyncTool은 `execute_async`, `get_definition`, `get_name`을 구현한다. 동기 `execute`는 final이며 private `run_sync` context를 구동한다. 두 실행 인터페이스를 함께 override하지 말고 shared event-loop 스레드에서 긴 동기 작업을 하지 않는다.

## Generic HTTP streaming

NeoGraph의 `async_post_stream`은 retained consumer가 쓰는 별도 generic HTTP utility로 남는다. SDK libcurl dispatch가 이를 대체하지 않는다. Chunked, 제한된 `Content-Length`, close-delimited response body를 지원한다. 비어 있지 않은 fixed-length body는 callback에 한 번 전달하고 길이 0이면 callback을 내보내지 않는다. 반환된 `HttpStreamResponse.status`는 200과 non-2xx status를 보존하므로 caller는 non-SSE reply를 빈 성공으로 읽는 대신 JSON error body를 해석할 수 있다.

`RequestOptions` 기본값은 status/header 64 KiB, decoded body 16 MiB, transfer chunk 1 MiB 제한이며 각 값 0은 해당 제한을 해제한다. Fixed-length 분기는 할당 전에 body limit을 검사한다. 잘못되거나 모호한 framing, premature EOF, 이미 buffer에 들어온 surplus byte는 거부한다. 반환 뒤 도착한 byte를 탐지한다는 보장은 아니다. Redirect body는 별도로 처리한다. 기본 per-hop timeout은 0이고 redirect는 비활성화되어 있다. 이 generic option은 SDK policy나 model spending grant가 아니다.

## Python과 수명 경계

Python은 typed `ProviderRequest`, `PreparedProviderRequest`, 불변 `ProviderOutcome`을 사용하는 `prepare`, `dispatch`, `invoke`를 공개한다. invoke/dispatch는 GIL을 해제하고 콜백과 Python 객체 파괴는 GIL을 획득한다. asyncio 경계에서는 `asyncio.to_thread(provider.invoke, request)`를 쓴다. C++ provider awaitable을 asyncio awaitable로 자동 변환하지 않는다. [Python binding](python-binding.md)을 참고한다.

`run_sync`는 호출마다 private 단일 스레드 context를, `run_sync_pool`은 호출별 풀을 만든다. 그 executor에 묶인 handle은 호출 밖으로 나가면 안 된다. 일반 함수는 다른 awaitable을 직접 반환할 수 있고 코루틴 본문은 `co_return co_await`를 쓸 수 있다. 코루틴 본문에 일반 `return`을 섞지 않는다.

## 빌드와 역사적 근거

외부 `SchemaProvider::runtime`은 `NEOGRAPH_BUILD_LLM=OFF`여도 `neograph::core`에 필수다. 설치된 SDK prefix 또는 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`를 쓴다. SDK 빌드는 CMake 3.20+, C++20, Python, standalone Asio, yyjson, libcurl 7.88+, OpenSSL Crypto가 필요하다. 기록된 interface-4 이전 runtime/archive 검증은 Linux/POSIX 범위이며 interface 4를 검증하지 않는다. 기존 macOS/Windows metadata가 새 의존성을 검증하지 않으며 WASM 통합도 확립되지 않았다.

NeoGraph 설정에는 CMake 3.20+가 필요하다. 명시적 SDK source directory를 먼저 쓰고, 없으면 installed package, 기본 revision-pinned 공개 archive fallback 순으로 해석한다. installed SDK로 offline 설정할 때 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`, 해당 prefix를 `CMAKE_PREFIX_PATH`에 지정한다. fetched/source SDK는 NeoGraph의 체크인된 Asio와 yyjson을 쓰지만 system 개발 의존성은 여전히 필요하다.

[Stage 3 설계](ASYNC_STAGE3_DESIGN.md)는 2026년 4월 제안의 보관 기록이다. 2.0/3.0 명칭, completion crossover, 테스트/benchmark 수치는 역사적 단계이며 현재 버전·API·새 통과 주장이 아니다. 버전은 `pyproject.toml`에서 읽으며 역사적 측정은 [성능 상세](performance-deep-dive.md)에 있다.

원래 Stage 3 가이드는 당시 sync 경로의 기존 test 276+, engine seq ~30 µs/par ~205 µs, HTTP async_pool 17834 ops/s(Stage 2 async 8401/s, sync 6064/s), timer 50000개 fan-out 541K ops/s·RSS 67 MB를 기록했다. 또한 50 ms agent 세 개의 완료 50 ms, researcher 세 개의 완료 150 ms(직렬 370 ms)를 기록했다. 이는 당시 작업의 역사적 관측이며 현재 provider 전환 결과가 아니다.
