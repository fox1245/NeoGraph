<!-- neograph-i18n: source=docs/concurrency.md locale=ko source_sha256=6a70472a787f8ffaece52d4630a02370df682588e64986fd104f49284ad21a2d -->
# 동시성과 비동기

**Languages:** [English](concurrency.md) | [한국어](concurrency.ko.md) | [日本語](concurrency.ja.md) | [简体中文](concurrency.zh-CN.md)

## executor 선택

동기 방식의 `run`, `run_stream`, `resume`은 비동기 짝과 같은 코루틴 구현을 구동하면서 호출한 스레드를 블로킹합니다. 호스트의 워커 풀이 서로 독립된 세션을 동시에 실행할 수 있습니다. 비동기 짝은 `asio::awaitable<RunResult>`를 반환하며, 직접 소유한 executor에서 구동해야 합니다. awaitable을 쓴다고 해서 임의의 사용자 코드가 논블로킹이 되지는 않습니다.

`EngineConfig::worker_count = 1`이 기본값이며, 이때 엔진이 소유하는 fan-out 풀은 만들어지지 않습니다. I/O에서 대기 중인 분기는 한 스레드 안에서 서로 겹쳐 실행될 수 있지만, 그 스레드의 CPU 작업은 직렬로 실행됩니다. CPU 분기를 여러 코어에서 실행하려면 멀티스레드 호출자 executor를 쓰거나 엔진의 워커 풀을 직접 켭니다. 풀은 엔진을 공개하기 전에 설정해야 합니다.

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

예제 `27_async_concurrent_runs.cpp`는 하나의 io_context에서 여러 세션을 실행하는 방법을, `05_parallel_fanout.cpp`는 한 실행 안에서 분기를 병렬로 실행하는 방법을 보여 줍니다. 과거의 처리량과 메모리 측정값은 [성능 심층 분석](performance-deep-dive.md)에 있으며, 안전한 세션 수 상한을 보장하는 값이 아닙니다.

## 엔진을 공유할 때의 규칙

- 독립된 세션에는 서로 다른 `thread_id`를 씁니다. 같은 id로 동시에 실행하면 체크포인트가 섞이는 순서가 정해져 있지 않으므로, 기록 순서가 중요하면 호스트에서 직렬화합니다.
- 설정 setter 호출과 도구 바인딩은 엔진을 실행 스레드나 관리 스레드에 노출하기 전에 끝냅니다. 실행 중에 워커 풀 크기를 바꾸는 것은 치명적 오류입니다.
- 한 엔진에서 관리와 실행은 동시에 할 수 없습니다. run이나 resume이 하나라도 진행 중이면 상태/이력 조회, 갱신, fork는 `std::logic_error`로 거부되고, 관리 호출이 진행 중이면 실행이 거부됩니다. 관리 호출을 다시 시도하기 전에 취소하거나 작업을 비우고 완료를 기다립니다.
- 저장소를 공유하는 서로 다른 엔진은 이 승인(admission) 경계 밖에 있으므로 호스트에서 조율합니다.
- 노드 인스턴스는 실행 사이에 재사용됩니다. 실행별 임시 상태는 채널에 두고, 사용자 정의 노드, provider, 도구, 저장소는 상태가 없거나 동기화되어야 합니다. 번들된 인메모리 저장소는 뮤텍스를 사용합니다.

provider 호출은 준비된 요청과 런타임 클라이언트를 직접 소유합니다. C++ 이벤트 뷰는 콜백 안에서만 유효하므로 보관할 데이터는 복사해야 합니다. 네이티브 재생(replay) 상태와 회계 권한에는 이식 가능한 JSON으로 재구성한 것이 아니라 실제 보관(custody)이 필요합니다. [비동기 가이드](ASYNC_GUIDE.md)를 참고하세요.

## 한 대화의 실행

한 대화는 두 계층에 저장될 수 있고, 두 계층은 동시에 쓰는 쪽을 서로 다르게 다룹니다.

- **컨텍스트 피드**는 실패 시 차단(fail closed)합니다. `ContextStore::append_history`는 쓰는 쪽이 기대하는 head를 받습니다. 기대한 head, 시퀀스, 선행 레코드가 피드의 현재 값과 다른 추가는 `Conflict`를 반환하고 아무것도 저장하지 않으므로, 쓰는 쪽 둘이 같은 head를 함께 이을 수 없습니다. 이미 저장된 레코드를 똑같이 다시 추가하면 head가 움직인 뒤에도 `AlreadyPresent`를 반환합니다.
- **체크포인트 저장소**는 `thread_id`마다 마지막에 쓴 쪽이 이깁니다(last-writer-wins). 호출 하나는 원자적이지만 호출의 연속은 그렇지 않습니다. 같은 `thread_id`의 실행 둘이 겹치면 `save_checkpoint` 호출이 정해진 순서 없이 섞이고, 마지막 저장이 남으며, `load_latest`는 마지막으로 저장한 쪽의 체크포인트를 돌려줍니다. 엔진도 저장소도 두 번째 실행을 거부하지 않습니다.

같은 `thread_id`(와 같은 피드)의 실행과 재개를 직렬화하는 일은 호스트의 몫입니다. 프로세스가 여러 개일 때도 마찬가지입니다. 엄격 프로파일은 이 일을 하지 않습니다. 프로파일은 호출자 전체에 활성 에포크 하나만 두고 `thread_id`를 보지 않습니다. 한 대화에 요청이 겹치는 것은 대개 우연한 사고 때문입니다. 재시도된 웹훅이나 더블 클릭이 그 예입니다. 엔진에 닿기 전에 호스트에서 대기시키거나 거부하세요.

프로세스 내부 가드를 계획하고 있습니다. 같은 `thread_id`의 실행이나 재개가 진행 중일 때 두 번째 요청을 거부하는 방식입니다. 이 가드는 프로세스 하나만 대상으로 합니다. 가드가 생기기 전에는 NeoGraph가 같은 `thread_id`의 실행 둘을 막지 않습니다. [호스트 단계 진행 계약](STRICT_RUNTIME_INTERPOSITION.md#host-stepped-contract)도 참고하세요.

## 동기 요청의 승인(admission) 한도

`RequestQueue`를 쓰려면 `neograph::util`을 링크합니다. 내부에서 `moodycamel::ConcurrentQueue`를 사용하고, 유휴 워커는 조건 변수에서 대기합니다. 대기 슬롯 한도는 큐에 쌓이는 세션 수를 제한할 뿐, 실행 중인 세션이 쓰는 메모리를 제한하지는 않습니다.

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

큐가 가득 차면 `accepted=false`와 유효하지 않은 future를 반환합니다. 내부에서 큐에 넣지 못하면 `false`와 함께 `std::runtime_error`를 담은 유효한 future를 반환합니다. 모든 거부를 일반적인 포화로 취급하지 말고 이 future를 확인해야 합니다. 생성하려면 워커가 최소 하나 있어야 합니다.

`close()`는 멱등입니다. 새 요청을 거부하고, 이미 워커가 가져간 작업은 끝나도록 두며, 아직 가져가지 않은 작업의 future는 `std::runtime_error("RequestQueue is closed")`로 완료합니다. 워커가 close를 호출하면 자기 자신을 기다리지 않고 종료를 시작합니다. 소멸자도 같은 경로를 쓰므로, 수락된 future가 아무 알림 없이 방치되는 일은 없습니다.

## 체크포인트 I/O와 Python

인메모리 체크포인트 연산은 뮤텍스를 잡고 호출자 스레드에서 실행됩니다. SQLite와 동기 방식의 사용자 정의 백엔드는 블로킹 작업을 개수가 제한된 워커에 넘깁니다. PostgreSQL은 논블로킹 libpq I/O를 사용하며 파이프라인 배치는 쓰지 않습니다. `NEOGRAPH_BUILD_POSTGRES=ON`은 이 선택 타깃을 켜며 libpq 개발 파일이 필요합니다. `OFF`로 하면 이 선택 의존성만 사라집니다.

Python 콜백은 GIL을 잡고 실행됩니다. CPU 위주의 Python 노드/리듀서는 worker_count를 늘리는 것만으로는 병렬성을 얻을 수 없습니다. 네이티브 호출은 그 구현이 직접 GIL을 놓을 때만 겹쳐 실행될 수 있습니다. 타입이 있는 provider의 invoke/dispatch는 GIL을 놓으므로 `asyncio.to_thread`로 호출할 수 있습니다. 그렇다고 네이티브 provider의 asyncio awaitable이 제공되는 것은 아닙니다.

## 런타임 의존성

Core는 `NEOGRAPH_BUILD_LLM=OFF`일 때도 외부 `SchemaProvider::runtime`을 항상 링크합니다. PostgreSQL, LLM 노드, NeoGraph의 선택 기능인 CurlH2Pool을 꺼도 이 런타임의 libcurl 요구 사항은 사라지지 않습니다. 맞는 버전의 설치된 SDK를 제공하거나 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`을 지정해야 합니다.

소스는 명시적으로 지정한 SDK 소스 디렉터리를 먼저 쓰고, 없으면 설치된 패키지를, 그것도 없으면 리비전이 고정된 공개 archive를 대체 경로로 사용합니다(`NEOGRAPH_FETCH_SCHEMAPROVIDER=ON`이 기본값). 설치된 SDK로 오프라인 빌드를 구성하려면 이 플래그를 `OFF`로 하고 SDK의 prefix를 `CMAKE_PREFIX_PATH`에 지정합니다. NeoGraph와 SDK 소스를 구성하려면 CMake 3.20 이상이 필요합니다.

SDK와의 WASM 런타임 통합은 확인되지 않았으며, 이식 가능한 executor API만으로는 이를 제공하지 못합니다.
