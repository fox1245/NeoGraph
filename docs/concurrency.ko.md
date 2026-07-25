<!-- neograph-i18n: source=docs/concurrency.md locale=ko source_sha256=fe3657f31d0895edf67431f7c6135b418f6677d2d4b8cc90699617f5ab343df8 -->
**Languages:** [English](concurrency.md) | [한국어](concurrency.ko.md) | [日本語](concurrency.ja.md) | [简体中文](concurrency.zh-CN.md)

# 동시성 및 비동기


NeoGraph는 기본적으로 두 가지 동시성 모델을 지원합니다.
귀하의 호스팅 패턴에 맞는 것:

* **에이전트당 스레드(동기화)** — `run()` / `run_stream()` / `resume()`
이미 사용하고 있는 실행기로 전달됩니다. 최대 약 1년까지 안전
수천 명의 동시 상담원; 호출당 최대 5μs의 엔진 오버헤드
`-O3 -DNDEBUG` 빌드 출시(수퍼 스텝 루프는
`run_sync(execute_graph_async)`이므로 두 진입점 모두 하나를 공유합니다.
코루틴 경로).
* **코루틴 기반 비동기** — `run_async()` / `run_stream_async()` /
`resume_async()`가 `asio::awaitable<RunResult>`를 반환합니다. 하나
`asio::io_context`는 별도의 연결 없이 수천 명의 동시 에이전트를 호스팅합니다.
실행당 스레드; 모든 공급자 / MCP / 체크포인트 I/O 포인트는
후드 아래의 비 차단 `co_await`. 전체 마이그레이션 가이드
[`ASYNC_GUIDE.md`](ASYNC_GUIDE.md).

## 비동기(3단계)

```cpp
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

asio::io_context io;
for (const auto& user : users) {
    asio::co_spawn(
        io,
        [&, user]() -> asio::awaitable<void> {
            RunConfig cfg;
            cfg.thread_id = user.session_id;
            cfg.input     = {{"messages", user.history}};
            auto result = co_await engine->run_async(cfg);
            handle(result);
        },
        asio::detached);
}
io.run();  // drives all agents on this thread
```

`engine->run_async()`는 호출자의 실행자에 엔드투엔드 상태로 유지됩니다.
모든 슈퍼 스텝 정지 지점(노드 디스패치, 체크포인트 I/O,
병렬 팬아웃, 재시도 백오프)는 실제 `co_await`입니다. 세 가지
위의 50ms 단계는 하나의 io_context 스레드에서 겹치고
벽 시간은 3 × 50ms가 아닌 ~50ms에 도달합니다. 하나의 스레드, N 동시
자치령 대표. 코어 전반에 걸쳐 CPU 바운드 팬아웃의 경우 드라이버를
공유 `asio::thread_pool` — 이것이 패턴입니다
[`benchmarks/concurrent/CONCURRENT.md`](../benchmarks/concurrent/CONCURRENT.md)
여기서 N = 10,000은 52ms 내에 완료됩니다. 단일 실행 내에서
`make_parallel_group` 팬아웃도 중복됨: 3개의 병렬 팬아웃
연구원들은 순차 370ms에서 150ms로 축소되었습니다.

사용자 정의 노드는 `asio::awaitable`를 반환하여 비동기 경로에 참여합니다.
통합 `run(NodeInput)` 진입점에서(v0.4.0에 도입됨;
레거시 8-가상 체인은 v0.9.0에서 제거되었습니다.)

```cpp
class FetchNode : public GraphNode {
  public:
    asio::awaitable<NodeOutput>
    run(NodeInput in) override {
        auto ex = co_await asio::this_coro::executor;
        auto res = co_await neograph::async::async_post(ex, /*...*/);
        // in.ctx.cancel_token, in.state, in.stream_cb available.
        co_return NodeOutput{ {ChannelWrite{"out", res}} };
    }
    std::string get_name() const override { return "fetch"; }
};
```

비동기식 도구는 `AsyncTool`에서 파생됩니다.

```cpp
class FetchTool : public neograph::AsyncTool {
  public:
    asio::awaitable<std::string>
    execute_async(const json& args) override { /* co_await HTTP */ }
    // sync execute() is final, routes through run_sync automatically.
};
```

다중 에이전트는 `examples/27_async_concurrent_runs.cpp`를 참조하세요.
팬아웃을 위한 패턴 및 `examples/05_parallel_fanout.cpp`
한 번의 실행.

## 동기화(에이전트당 스레드)

NeoGraph는 자체 비동기 런타임을 제공하지 않습니다.
`run()` / `run_stream()` / `resume()`를 사용하면 실행자를 선택할 수 있습니다.
컴파일된 단일 `GraphEngine`는 다음 스레드 간에 공유해도 안전합니다.
**고유한 `thread_id`s**와 동시에 `run()`를 호출하므로 호스팅
다중 테넌트 에이전트 워크로드는 무엇이든 디스패치하는 문제입니다.
이미 사용하고 있는 실행자입니다.

```cpp
// One engine, many concurrent sessions — no external runtime required.
EngineConfig engine_config;
engine_config.node_context = ctx;
engine_config.checkpoint_store = std::make_shared<InMemoryCheckpointStore>();
auto engine = GraphEngine::build(def, std::move(engine_config));

std::vector<std::future<RunResult>> sessions;
for (const auto& user : users) {
    sessions.push_back(std::async(std::launch::async, [&engine, user]() {
        RunConfig cfg;
        cfg.thread_id = user.session_id;
        cfg.input = {{"messages", user.history}};
        return engine->run(cfg);
    }));
}
for (auto& f : sessions) handle(f.get());
```

`std::async` 지원 `asio::thread_pool`와 동일한 방식으로 작동합니다.
작업 시스템 또는 웹 프레임워크의 작업자 풀 - NeoGraph는 제외됩니다.
집행자의 결정. CPU 병렬 팬아웃 *내부*가 필요한 경우
단일 동기화 `run()` 호출(N 스레드의 N 동기화 `run()` 대신)
설치하려면 `build()`보다 먼저 `EngineConfig::worker_count`를 설정하세요.
엔진 소유 `asio::thread_pool` `run_parallel_async` 및
다중 전송 지점 파견.

## 번들 `RequestQueue` 사용

고정 작업자 풀을 원하는 다중 테넌트 서버의 경우
역압(큐가 포화되면 새 세션 거부)
무제한 메모리 증가 대신) `neograph::util`를 연결하고 사용
내장된 잠금 없는 대기열 - 외부 실행자가 필요하지 않습니다.

```cpp
#include <neograph/util/request_queue.h>
using namespace neograph::util;

RequestQueue pool(16, 1000);           // 16 workers, max 1000 pending sessions
EngineConfig engine_config;
engine_config.node_context = ctx;
engine_config.checkpoint_store = std::make_shared<InMemoryCheckpointStore>();
auto engine = GraphEngine::build(def, std::move(engine_config));

std::vector<RunResult>          results(users.size());
std::vector<std::future<void>>  futs;

for (size_t i = 0; i < users.size(); ++i) {
    auto [accepted, fut] = pool.submit([&, i]() {
        RunConfig cfg;
        cfg.thread_id = users[i].session_id;
        cfg.input     = {{"messages", users[i].history}};
        results[i]    = engine->run(cfg);
    });
    if (!accepted) {
        // Backpressure: queue is full — shed load, return 503, retry later, …
        reject(users[i]);
        continue;
    }
    futs.push_back(std::move(fut));
}

for (auto& f : futs) f.get();           // propagates exceptions from run()

auto s = pool.stats();
log("pending={} active={} completed={} rejected={}",
    s.pending, s.active, s.completed, s.rejected);
```

`submit()`는 `{accepted, std::future<void>}`를 반환합니다.
공유 출력 슬롯(위와 같음) 또는 작업별을 통한 `RunResult`
`std::promise<RunResult>`. 대기열은 다음에 의해 지원됩니다.
`moodycamel::ConcurrentQueue`(잠금 장치 없음) 및 작업자 주차 공간
유휴 상태일 때 condvar - 바쁜 회전이 없습니다.

## 안전한 동시 사용을 위한 규칙

- 구성 변경자(`set_retry_policy`, `set_checkpoint_store`,
`set_store`, `own_tools`, …)는 동시 실행 **전에** 호출되어야 합니다.
`run()`. 첫 번째 파견 이후에는 엔진을 정지 상태로 취급하십시오.
- **동일** `thread_id`를 공유하는 동시 `run()` 통화는 충돌하지 않습니다.
그러나 지정되지 않은 체크포인트 인터리빙을 생성합니다. 세션별 ​​직렬화
결정론적 이력이 필요한 경우 직접 액세스하세요.
- 사용자 정의 `GraphNode` 서브클래스는 **상태 비저장 또는 자체 동기화**되어야 합니다.
노드 인스턴스는 엔진이 소유하며 모든 실행에서 재사용됩니다.
모든 스레드 — 실행별 스크래치 데이터는 그래프 채널이 아닌 그래프 채널에 속합니다.
노드 멤버 변수.
- 사용자 제공 `CheckpointStore`, `Store`, `Provider` 및 `Tool`
구현은 스레드로부터 안전해야 합니다. 번들로 제공되는 `InMemoryCheckpointStore`
`InMemoryStore`는 이미 있습니다.

## PostgreSQL을 사용한 지속적인 체크포인트

다중 프로세스 배포의 경우 또는 다시 시작해도 체크포인트가 유지되어야 하는 경우
`neograph::postgres`를 연결하고 `InMemoryCheckpointStore`를 다음으로 교체합니다.
`PostgresCheckpointStore`:

```cpp
#include <neograph/graph/postgres_checkpoint.h>

auto store = std::make_shared<PostgresCheckpointStore>(
    "postgresql://user:pass@host:5432/dbname");
EngineConfig engine_config;
engine_config.node_context = ctx;
engine_config.checkpoint_store = store;
auto engine = GraphEngine::build(def, std::move(engine_config));
```

스키마는 LangGraph의 `PostgresSaver`(접두사가 붙은 3개의 테이블)를 미러링합니다.
동일한 데이터베이스에서 LangGraph 상태와 공존하는 `neograph_*`) 및
`(thread_id, channel, version)`로 채널 값을 중복 제거합니다. 에이
슈퍼 스텝당 하나의 채널을 터치하는 1000단계 세션의 비용은 대략적입니다.
`O(steps × channels)` 대신 `O(steps + channels)` Blob 행.

**빌드 플래그**: `-DNEOGRAPH_BUILD_POSTGRES=ON`(기본값). 필요하다
`libpq-dev`(적당) / `libpq-devel`(rpm). 건너뛰도록 플래그 `OFF`를 설정합니다.
의존성을 완전히.

**통합 테스트 실행**: 일회용 로컬 PG를 가동하고
테스트 바이너리를 가리킵니다.

```bash
docker run -d --rm --name neograph-pg-test \
    -e POSTGRES_PASSWORD=test -e POSTGRES_DB=neograph_test \
    -p 55432:5432 postgres:16-alpine

NEOGRAPH_TEST_POSTGRES_URL='postgresql://postgres:test@localhost:55432/neograph_test' \
    ctest --test-dir build -R PostgresCheckpoint --output-on-failure
```

env var가 없으면 PG 테스트는 `GTEST_SKIP`이므로 나머지는
이 제품군은 Postgres가 없어도 컴퓨터에서 녹색으로 유지됩니다.

적용 범위: `tests/test_graph_engine.cpp` 포함
`ConcurrentRunDifferentThreadIds`(16개 스레드 × 25개 실행 = 400개 병렬)
실행, 세션별 출력 유효성 검사 + 체크포인트 격리) 및
`ConcurrentRunSameThreadIdNoCrash`(하나의 공유에서 8개 스레드 × 50개 실행)
`thread_id`, 충돌 없는 동작 검증).
