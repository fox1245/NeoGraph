<!-- neograph-i18n: source=docs/concurrency.md locale=ko source_sha256=889743688862b981a4a8e8d8de0c0f3bc287693712453d430183d4dcdd7a030a -->
# 동시성과 비동기

**Languages:** [English](concurrency.md) | [한국어](concurrency.ko.md) | [日本語](concurrency.ja.md) | [简体中文](concurrency.zh-CN.md)

## executor 선택

동기 `run`, `run_stream`, `resume`은 async 대응 API와 같은 코루틴 구현을 구동하며 호출 스레드를 점유한다. host worker pool은 독립 세션을 동시에 실행할 수 있다. async API는 `asio::awaitable<RunResult>`를 반환하므로 직접 소유한 executor에서 구동한다. awaitable이 임의의 사용자 코드를 nonblocking으로 바꾸지는 않는다.

`EngineConfig::worker_count = 1`이 기본값이며 엔진 소유 fan-out pool은 없다. 일시 중단된 I/O 분기는 한 스레드에서도 겹치지만 CPU 작업은 직렬 실행된다. 다중 스레드 caller executor나 선택적 engine pool로 CPU 분기를 여러 코어에서 실행할 수 있다. 엔진 공개 전에 pool을 설정한다.

```cpp
#include <neograph/async/run_sync.h>

EngineConfig options;
options.node_context = ctx;
options.checkpoint_store = std::make_shared<InMemoryCheckpointStore>();
options.worker_count = 4;
auto engine = GraphEngine::build(def, std::move(options));
RunConfig run;
run.thread_id = "session-1";
run.input = {{"count", 0}};
auto result = neograph::async::run_sync(engine->run_async(run));
```

`27_async_concurrent_runs.cpp`는 한 io_context의 여러 세션을, `05_parallel_fanout.cpp`는 한 실행 안의 분기를 보여 준다. 역사적 처리량·메모리 측정은 [성능 상세](performance-deep-dive.md)에 있으며 안전한 세션 수 상한을 보장하지 않는다.

## shared engine 규칙

- 독립 세션에 서로 다른 `thread_id`를 쓴다. 같은 id의 동시 실행은 checkpoint 순서가 미정이므로 기록 순서가 필요하면 host에서 직렬화한다.
- 실행/관리 스레드에 공개하기 전에 설정 setter 호출과 tool 바인딩을 끝낸다. 실행 중 worker pool 크기 변경은 오류다.
- 한 engine에서는 관리와 실행이 상호 배타적이다. run/resume 중 state/history 읽기, update, fork는 `std::logic_error`로 거부되고 관리 중 실행도 거부된다. 취소·drain 후 완료를 기다린 뒤 관리를 재시도한다.
- 같은 store를 쓰는 서로 다른 engine은 이 admission 경계 밖이다. host에서 조정한다.
- node instance는 여러 실행에서 재사용된다. 실행별 scratch state는 channel에 두고 사용자 node/provider/tool/store는 stateless 또는 동기화된 구현으로 만든다. 내장 메모리 store는 mutex를 쓴다.

Provider 호출은 준비된 request와 runtime client를 소유한다. C++ event view의 수명은 callback 안으로 제한되므로 보존할 데이터는 복사한다. native replay와 accounting 권한은 실제 custody가 필요하며 portable JSON 재구성으로 얻을 수 없다. [비동기 가이드](ASYNC_GUIDE.md)를 참고한다.

## 제한된 동기 admission

`RequestQueue`는 `neograph::util`을 링크해서 쓴다. `moodycamel::ConcurrentQueue`를 사용하고 유휴 worker는 condition variable에서 기다린다. pending-slot 제한은 대기 세션 수를 제한하지, 실행 중 세션의 메모리를 제한하지 않는다.

```cpp
#include <neograph/util/request_queue.h>

neograph::util::RequestQueue queue(16, 1000);
auto [accepted, future] = queue.submit([engine, config] {
    auto result = engine->run(config);
    handle(result);
});
if (future.valid()) future.get();
if (!accepted) reject_request();
```

큐가 가득 차면 `accepted=false`와 invalid future를 반환한다. 내부 enqueue 실패는 `false`와 `std::runtime_error`를 담은 valid future를 반환한다. 모든 거부를 평범한 포화로 취급하지 말고 future를 확인한다. worker는 적어도 하나 필요하다.

`close()`는 멱등이다. 새 제출을 거부하고 claimed 작업을 끝내며 unclaimed future를 `std::runtime_error("RequestQueue is closed")`로 완료한다. worker가 close를 호출하면 자기 자신을 기다리지 않고 shutdown을 시작한다. 소멸자도 같은 경로를 쓰며 승인된 future를 방치하지 않는다.

## checkpoint I/O와 Python

메모리 checkpoint는 caller에서 mutex를 쓰고 SQLite와 sync 사용자 backend는 blocking 작업을 bounded worker로 넘긴다. PostgreSQL은 pipeline batching 없이 nonblocking libpq I/O를 쓴다. `NEOGRAPH_BUILD_POSTGRES=ON`은 libpq 개발 파일이 필요한 선택적 target을 켜고 `OFF`는 그 의존성만 제거한다.

Python callback은 GIL 아래 실행된다. CPU 위주 Python node/reducer는 worker_count만 늘려도 병렬화되지 않는다. native 호출은 그 구현이 GIL을 해제할 때만 겹쳐 실행될 수 있다. typed provider invoke/dispatch는 GIL을 해제하므로 `asyncio.to_thread`로 호출할 수 있다. native provider asyncio awaitable을 공개하는 것은 아니다.

## runtime 의존성과 플랫폼 검증

Core는 `NEOGRAPH_BUILD_LLM=OFF`여도 외부 `SchemaProvider::runtime`을 링크한다. PostgreSQL, LLM node, NeoGraph의 선택적 CurlH2Pool을 꺼도 SDK runtime의 libcurl 요구는 사라지지 않는다. 일치하는 installed SDK 또는 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`를 제공한다.

source resolution은 명시적 SDK source directory, installed package, revision-pinned 공개 archive fallback 순이다(`NEOGRAPH_FETCH_SCHEMAPROVIDER=ON` 기본). installed SDK offline build는 flag를 `OFF`로 하고 `CMAKE_PREFIX_PATH`에 prefix를 둔다. NeoGraph와 SDK source 설정은 CMake 3.20+가 필요하다.

현재 SDK runtime/archive의 검증 범위는 Linux/POSIX다. 기존 Linux/macOS/Windows package metadata는 새 의존성이 모든 플랫폼에서 동작한다는 근거가 아니다. macOS, Windows, WASM은 각각 runtime/build 검증이 필요하며 portable executor API만으로 그 검증을 대신할 수 없다.
