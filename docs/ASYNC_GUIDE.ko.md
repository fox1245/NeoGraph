<!-- neograph-i18n: source=docs/ASYNC_GUIDE.md locale=ko source_sha256=867535aebac0ee2f3fd44ffbb7b05c6f71b27535a3417f62efdd74cdc9aa14cf -->
# NeoGraph 비동기 안내

**Languages:** [English](ASYNC_GUIDE.md) | [한국어](ASYNC_GUIDE.ko.md) | [日本語](ASYNC_GUIDE.ja.md) | [简体中文](ASYNC_GUIDE.zh-CN.md)

Stage 3 / 2026-04 릴리스. 대상 독자: 기존 NeoGraph 코드를 비동기 API로 이전하거나,
비동기 API로 새 코드를 작성하는 사용자.

이 안내서는 **무엇이** 바뀌었는지, 모양이 **왜** 그런지, 그리고 점진적으로 이전하는
**방법**을 다룬다. 개별 학기의 설계 근거는
[`ASYNC_STAGE3_DESIGN.md`](ASYNC_STAGE3_DESIGN.md)를,
분 단위 커밋 장부는 `feat/async-api` 브랜치의 git 로그를 참조.

---

## 1. 새로운 점

엔진의 모든 동기 I/O 지점에 이제 기다릴 수 있는(awaitable) 짝이 생겼다:

| 계층 | 동기 (변경 없음) | 비동기 짝 |
|---|---|---|
| Provider | `complete` / `complete_stream` | `complete_async` / `complete_stream_async` |
| CheckpointStore | `save` / `load_latest` / `load_by_id` / `list` / `delete_thread` / `put_writes` / `get_writes` / `clear_writes` | 각각에 대한 `*_async` |
| GraphNode | — | `run(NodeInput) -> asio::awaitable<NodeOutput>` 이 유일한 표준 재정의(override) |
| GraphEngine | `run` / `run_stream` / `resume` | `run_async` / `run_stream_async` / `resume_async` |
| MCPClient | `rpc_call` | `rpc_call_async` |
| Tool | `execute` (사용자 인터페이스 — 고정) | `AsyncTool` 어댑터로 감싸기 |

비동기 짝은 `asio::awaitable<T>`를 반환한다. 어떤 `asio::io_context`(또는 스트랜드, `any_io_executor`가 있는 스레드 풀)에서든 구동 가능.
하나의 `io_context`가 실행당 OS 스레드를 전담하지 않고 수천 개의 동시 `run_async` 호출을 호스팅할 수 있다 — 이 재설계 전체를 동기 부여한 동시성 모델이다.

동기 표면은 보존된다. `engine->run(cfg)`나 `provider->complete*` 진입점을 호출하는 기존 코드는 계속 지원된다.
Stage 3 이전에 존재했던 276+ 테스트 케이스는 여전히 동기 경로에서 통과한다.

---

## 2. 교차 기본값 패턴

Provider와 영속성 추상화에 남아 있는 모든 동기/비동기 쌍은 서로를 연결하는 한 쌍의 기본 구현으로 이어진다:

```cpp
class Provider {
  public:
    // Sync default: drive the async peer on a private io_context.
    virtual ChatCompletion complete(const CompletionParams& params);

    // Async default: co_return the sync peer (single-threaded on
    // the resuming coroutine).
    virtual asio::awaitable<ChatCompletion>
    complete_async(const CompletionParams& params);

    // ...
};
```

**계약: 둘 중 최소한 하나는 재정의할 것.** 둘 다 재정의하지 않으면 어느 쪽을 호출해도 두 기본값 사이에서 무한 재귀가 발생해 스택 오버플로가 일어난다. 문서화되어 있으며, 런타임 보호 장치는 없다(모든 구현자의 모든 호출을 느리게 만들기 때문).

### 어느 쪽을 재정의할지

| 코드 모양 | 재정의할 것 |
|---|---|
| 실제 비차단 I/O 수행 (HTTP, MCP, DB, 타이머) | **비동기 짝** — 동기 파사드를 상속 |
| 순수 CPU 작업, 또는 동기 라이브러리에서 잠시 블록 | **동기 짝** — 비동기 브리지를 상속 |
| 사용자 정의 `GraphNode` | `run(NodeInput)` 재정의; 쓰기, `Command`, `Send`를 하나의 `NodeOutput`으로 반환 |

### 왜 단일 통합 API가 아닌가?

모든 공개 추상화를 비동기로 무너뜨리면 모든 기존 Tool과 CheckpointStore 하위 클래스가 비동기 장치를 인지해야 한다 — 아무 이득이 없는 경우(두 숫자를 더하는 도구)도 포함. 교차 쌍은 그 추상화에 대해 이전 비용이 0인 경로로 남는다. `GraphNode`는 의도적으로 v1.0에서 하나의 코루틴 재정의로 통합되었다.

---

## 3. 이전 방법

### 3.1 동기 호출자가 비동기로 이전

**이전:**

```cpp
auto result = engine->run_stream(config, event_cb);
```

**이후:**

```cpp
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

asio::io_context io;
RunResult result;
asio::co_spawn(
    io,
    [&]() -> asio::awaitable<void> {
        result = co_await engine->run_stream_async(config, event_cb);
    },
    asio::detached);
io.run();
```

`io.run()`은 코루틴이 완료될 때 반환된다. 많은 동시 실행에서는 `io.run()` 호출 전에 같은 `io_context`에 각각 co_spawn한다 — `examples/27_async_concurrent_runs.cpp` 참조.

### 3.2 새 비동기 제공자 작성

`CompletionProvider`를 상속하고 `do_invoke()`만 구현한다. 그 최종 어댑터가 기존의 모든 `Provider` 진입점을 계속 작동시키며, `CompletionRequest`가 수집(collect) 대 스트림(stream) 모드를 명시적으로 만든다.

```cpp
class MyProvider : public CompletionProvider {
  public:
    asio::awaitable<ChatCompletion>
    do_invoke(CompletionRequest request) override {
        auto ex = co_await asio::this_coro::executor;
        const auto& params = request.params();
        auto res = co_await neograph::async::async_post(
            ex, host, port, path, body, headers, /*tls=*/true);
        if (request.streaming() && request.on_chunk()) {
            // Deliver parsed chunks through request.on_chunk().
        }
        co_return parse_response(res);
    }

    std::string get_name() const override { return "my-provider"; }
};
```

### 3.3 비동기 Tool 작성

`Tool` 인터페이스는 설계상 동기이다(Stage 3에서 기존 사용자 도구의 이전 비용을 0에 가깝게 유지하기 위해 고정). 내부에 코루틴 모양의 작업이 필요할 때는 `AsyncTool`을 사용:

```cpp
class FetchTool : public neograph::AsyncTool {
  public:
    ChatTool get_definition() const override { ... }
    std::string get_name() const override { return "fetch"; }

    asio::awaitable<std::string>
    execute_async(const json& args) override {
        auto ex = co_await asio::this_coro::executor;
        auto res = co_await neograph::async::async_post(
            ex, /*host*/, /*port*/, /*path*/, /*body*/);
        co_return res.body;
    }
};
```

`AsyncTool::execute`는 `final` — `execute_async`를 구동하기 위해 비공개 `io_context`를 생성하는 동기 파사드이다. 양쪽을 모두 재정의하는 것은 계약 위반.

### 3.4 비동기 제공자를 사용하는 그래프 노드 작성

```cpp
class MyNode : public GraphNode {
  public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        CompletionParams params = build_params(in.state);
        params.cancel_token = in.ctx.cancel_token;
        auto completion = co_await provider_->complete_async(params);

        neograph::json msg;
        to_json(msg, completion.message);
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"messages", json::array({msg})});
        co_return out;
    }

    std::string get_name() const override { return name_; }
  private:
    std::shared_ptr<Provider> provider_;
    std::string name_;
};
```

엔진은 동기와 비동기 진입점 모두에서 이 같은 코루틴을 구동한다. `engine->run_async()`를 사용하면 노드가 실행당 OS 스레드 없이 io_context 중첩에 참여한다.

---

## 4. 주의사항과 발등 찍기

### 4.1 GCC 13 코루틴 ICE

두 가지 특정 C++20 코루틴 모양이 GCC 13의 `build_special_member_call` ICE를 유발한다 (GCC 13.3 기준):

**모양 1 — `co_await`가 `catch` 블록 안에 있을 때:**

```cpp
try { ... }
catch (const MyError& e) {
    co_await something();  // ICE
}
```

**해결책 — 오류를 밖에서 캡처하고 나중에 처리:**

```cpp
std::optional<MyError> err;
std::optional<Result> ok;
try { ok.emplace(co_await op()); }
catch (const MyError& e) { err.emplace(e); }

if (err) {
    co_await recover();
    throw *err;
}
```

**모양 2 — 코루틴 본문 안의 중첩 brace-init:**

```cpp
co_await fn(std::vector<std::string>{name},    // ICE
            json{{"key", "value"}});
```

**해결책 — 밖에서 빌드하고 참조로 전달:**

```cpp
std::vector<std::string> v;
v.push_back(name);
json j;
j["key"] = "value";
co_await fn(v, j);
```

두 모양 모두 Stage 3 중 여러 번 표면화되었으며 해결책은 안정적. Clang 18+과 GCC 14+는 \"자연스러운\" 형태를 문제없이 컴파일하지만, NeoGraph는 GCC 13을 기준으로 삼는다.

### 4.2 `run_sync` 수명 위험

`neograph::async::run_sync<T>(asio::awaitable<T>)`는 호출마다 새로운 단일 스레드 `io_context`를 생성한다. 그 실행자에 바인딩된 수명이 긴 asio 핸들 — 풀의 소켓, 타이머, 파일 디스크립터 — 은 `run_sync`가 반환될 때 허상(dangling)이 된다. 초기 ConnPool 작업이 이 함정을 밟았고, 현재 아키텍처는 동기 파사드를 통해 의도적으로 아무것도 풀링하지 않음으로써 이를 회피한다.

규칙: 단일 호출보다 오래 살아야 하는 자원(연결 풀, 장기 실행 스트림 디스크립터)은 프로세스 수명 동안 소유한 실행자에만 바인딩할 것. 동기 파사드 경로는 요청당 새 연결을 생성한다.

### 4.3 `return x`가 아니라 `co_return co_await x`

`asio::awaitable<T>`를 반환하는 코루틴 함수는 본문 어딘가에서 `co_return` (또는 `co_await`)을 사용해야 한다. 일반 `return other_awaitable()`은 컴파일은 되는 것처럼 보이지만 런타임에 감싼 `T`를 기본 생성한다. 항상 `co_return co_await`로 연결할 것:

```cpp
asio::awaitable<RunResult>
GraphEngine::run_async(const RunConfig& config) {
    co_return co_await execute_graph_async(config, nullptr);
}
```

### 4.4 사용자 정의 노드에서 스트리밍

`GraphNode::run(NodeInput)`은 디스패치당 한 번 실행된다. `in.stream_cb`가 null이 아닐 때만 이벤트를 방출하고, 호출자가 스트리밍 엔진 진입점을 사용했는지와 관계없이 동일한 `NodeOutput`을 반환한다. 기존 `execute_full_stream_async` 이중 실행 대체 경로는 더 이상 존재하지 않는다.

### 4.5 MCP stdio 단일 세션 동시성

`StdioSession::rpc_call_async`는 `std::mutex`로 동시 호출을 직렬화한다. **같은** 세션을 **같은 단일 스레드** `io_context`에서 두 코루틴이 호출하면 교착 상태(deadlock)가 발생한다 — 두 번째 코루틴의 `lock_guard`가 첫 번째가 I/O 완료를 구동하는 데 필요한 작업자를 막는다. 일반적인 사용법(세션당 하나의 논리적 호출자, *다른* 세션으로의 비동기 팬아웃)은 영향을 받지 않는다. 기다릴 수 있는 뮤텍스(awaitable-mutex) 버전은 향후 작업으로 추적 중.

---

## 5. 성능 노트

비동기 전선이 단일 에이전트를 더 빠르게 만들지는 않는다 — `bench_neograph`는 Stage 3 이전과 동일한 seq (~30 µs) 및 par (~205 µs) 수치를 보고한다. 가치 축은 **동시성 견고함**이지 엔진 지연 시간이 아니다.

실제 모양 벤치마크에서 측정된 개선:

* `bench_async_http --mode async_pool --concur 1000` — 17834 ops/s, 대비 Stage 2 비동기 (8401 ops/s) 및 동기 (6064 ops/s).
* `bench_async_fanout --concur 50000` — 541K ops/s, 67 MB RSS. 스레드-당-에이전트 기준은 ~1000 동시 에이전트 이상으로 확장할 수 없었다; 50K는 이제 오후 작업.
* `examples/27_async_concurrent_runs` — 하나의 io_context에서 3 에이전트 × 50ms 작업: 50ms 총합 (vs. 150ms 순차).
* `examples/05_parallel_fanout` — 하나의 io_context에서 3 병렬 연구자: 150ms 총합 (vs. 370ms 순차).

### 언제 동기 API를 계속 사용할지

작업량이 ≤ 1000 동시 에이전트이고 각 에이전트가 전용 OS 스레드에서 실행된다면, 동기 API는 완전히 합리적인 선택이다. 그 규모에서는 스레드가 충분히 저렴하고, 동기 코드가 추론하기 더 단순하다. 비동기 API는 동기 모양이 다룰 수 없는 작업량 — 하나의 프로세스를 공유하는 수백 개의 장기 실행 에이전트, 단일 이벤트 루프에서 많은 사용자를 호스팅하는 경우 등 — 을 위해 존재한다.

---

## 6. 깨끗한 이전을 위한 체크리스트

- [ ] 에이전트 호스트 패턴 식별: 단일 에이전트 프로세스, 풀, 또는 공유 이벤트 루프?
- [ ] 공유 이벤트 루프인 경우 → 호출 지점을 `run_async` / `run_stream_async`로 이전.
- [ ] 사용자 정의 노드는 `run(NodeInput)`을 구현하고 실제 I/O를 직접 `co_await`.
- [ ] 도구가 실제 I/O를 하는 경우 → `AsyncTool`을 상속, `execute_async` 재정의.
- [ ] Postgres 체크포인트 저장소를 사용하는 경우 → 공유 이벤트 루프에서 해당 `*_async` 메서드 사용. 이 메서드들은 libpq의 비차단 전선 프로토콜과 코루틴 친화적 연결 풀을 사용.
- [ ] 측정. 가치 축은 동시성; 작업량이 동시성 제약이 아니라면 이전하지 말 것.

---

## 7. 아직 다루지 않은 것

* **Postgres 파이프라인 모드** — 비동기 체크포인트 메서드는 이미 비차단 libpq I/O를 사용하지만, libpq 파이프라인 모드를 통한 다중 명령 일괄 처리는 아직 하지 않음.
* **`async::HttpResponse` 헤더 맵** — 응답 표면은 status / body / retry_after / location만 노출. 임의의 헤더 접근(예: MCP 세션 ID 헤더 추적)은 Sem 1 후속 작업.

---

## 8. 3.0에서 변경된 점

3.0 (`feat/taskflow-removal`)은 Taskflow를 제거하고 동기 진입점을 `run_sync(execute_graph_async)`를 통해 라우팅함으로써 동기와 비동기를 하나의 코루틴 런타임으로 통합했다. 2.0 비동기 API 모양은 변경되지 않음 — 차이는 기본값과 새 선택 사항에 있다.

### 8.1 `GraphNode::run(NodeInput)`이 기존 재정의 체인을 대체

v0.9.0 v1-준비 릴리스가 8개의 `execute*` 가상 함수를 제거했다. 이제 사용자 정의 노드는 동기·비동기 엔진 진입점, 스트리밍, 제어 흐름을 위한 하나의 재정의를 가진다:

```cpp
asio::awaitable<NodeOutput> run(NodeInput in) override {
    NodeOutput out;
    out.writes.push_back({"answer", co_await fetch_answer(in)});
    Command command;
    command.goto_node = "review";
    out.command = command;
    if (in.stream_cb) {
        (*in.stream_cb)({GraphEvent::Type::LLM_TOKEN, get_name(), json("done")});
    }
    co_return out;
}
```

이전 릴리스에서 이전하는 코드는 상태 읽기를 `in.state`로, 실행 메타데이터를 `in.ctx`로, 스트리밍 싱크를 `in.stream_cb`로, 쓰기/`Command`/`Send` 값을 반환된 `NodeOutput`으로 이동해야 한다.

### 8.2 `GraphEngine::set_worker_count(N)` — 선택적 CPU 병렬 팬아웃

기본값: `run_parallel_async`와 `run_sends_async`의 다중 Send 분기는 현재 코루틴을 구동하는 실행자에서 분기를 디스패치한다. 동기 `run()`의 경우 단일 스레드 io_context — I/O-바운드 분기는 co_await 일시 중단을 통해 여전히 중첩되지만, CPU-바운드 분기는 직렬화된다.

```cpp
EngineConfig engine_config;
engine_config.node_context = ctx;
engine_config.checkpoint_store = store;
engine_config.worker_count = std::thread::hardware_concurrency();
auto engine = GraphEngine::build(def, std::move(engine_config));
// Now run_parallel_async dispatches branches to an engine-owned
// asio::thread_pool of that size.
```

가능하면 생성 전에 설정할 것. 호환성 설정자는 동시 `run()`이 있기 전에 호출해야 함; 실행 중인 실행을 가로지르며 풀을 다시 빌드하는 것은 안전하지 않다. 다중 스레드 `asio::thread_pool`을 직접 구동하는 `run_async` 호출자는 이것이 필요하지 않다 — 그들의 호출자 측 실행자가 이미 분기를 병렬화한다.

### 8.3 `neograph::async::run_sync_pool(aw, n_threads)` — N-작업자 동기 브리지

```cpp
#include <neograph/async/run_sync.h>

int result = neograph::async::run_sync_pool(
    my_coroutine_that_uses_make_parallel_group(), /*n_threads=*/4);
```

기존 단일 스레드 `run_sync`의 동반자. 호출을 위해 새로운 `asio::thread_pool`을 생성하여 내부 `make_parallel_group` 분기가 별도 작업자에서 실행되도록 한다. 호출당 풀 생성은 작업자당 하나의 `std::thread`를 생성 — 핫 경로에서는 비용이 무시할 수 없으므로, 요청당 코드가 아니라 가끔 경계에서의 동기 브리지 용도.

### 8.4 제거된 표면

- `NodeExecutor::run_one` / `run_parallel` / `run_sends` (동기) — `_async` 짝을 사용.
- `GraphEngine::execute_graph` (동기) — 삭제; `run()` / `run_stream()` / `resume()`은 `run_sync`를 통해 비동기 짝을 경유.
- `tf::Executor`, `tf::Taskflow`, `deps/taskflow/` 디렉터리 — 제거. Taskflow를 호출자 측 드라이버로 사용하던 벤치마크 (`bench_concurrent_neograph.cpp`)는 `asio::thread_pool` + `asio::post`로 전환.

---

## 9. 재정의 결정 안내

`GraphNode`는 하나의 표준 재정의를 가진다. Provider와 영속성 인터페이스는 호환성을 위해 별도의 동기/비동기 짝을 유지한다.

### 9.1 2분 버전

| 작성 대상… | 재정의 | 그대로 상속 |
|---|---|---|
| 모든 사용자 정의 `GraphNode` | `run(NodeInput)` | `get_name()`만 다른 필수 가상 함수 |
| 새 사용자 정의 LLM 백엔드 | `CompletionProvider` 상속, `do_invoke()` 재정의 | 모든 기존 `Provider` 진입점은 최종 어댑터 |
| 사용자 정의 `CheckpointStore`, 비동기 가능 백엔드 | 8개 모두 `*_async` 짝 | 동기 짝은 `run_sync`로 연결 |
| 사용자 정의 `CheckpointStore`, 동기 전용 백엔드 | 8개 모두 동기 짝 | 비동기 짝은 `run_sync`로 연결 |
| 사용자 정의 동기 `Tool` | `Tool` 상속, `execute()` 재정의 | — |
| 사용자 정의 비동기 `Tool` | `AsyncTool` 상속, `execute_async()` 재정의 | 동기 `execute()`는 `final`, 연결 |

### 9.2 `GraphNode`

항상 `run(NodeInput)`을 재정의. CPU 전용 작업은 `co_return` 전에 직접 실행 가능; 실제 비동기 I/O는 `co_await`할 것. 엔진은 `run`, `run_async`, 스트리밍, 재개, Send 팬아웃에서 동일한 메서드를 호출하므로, 재정의 선택 행렬도 없고 동기/비동기 대체 재귀도 없다.

공유 단일 스레드 `io_context`를 오랫동안 막지 말 것. 블로킹 작업을 실행자로 이동하거나 코루틴 친화적 I/O를 사용. `EngineConfig::worker_count`는 병렬 팬아웃이 필요한 동기 호출자가 사용하는 엔진 소유 풀을 제어.

### 9.3 `Provider`

기존 `Provider` 하위 클래스는 네 개의 동기/비동기 수집/스트림 가상 함수를 계속 사용할 수 있다. 이들은 제거 계획이 없고 지원 중단 경고(deprecation warning)도 없는 안정된 호환성 API이다. 각 쌍은 여전히 최소한 하나의 재정의가 필요:

| 재정의 | 동작 |
|---|---|
| `complete()`만 | 동기는 직접 작동; 비동기 `complete_async`는 기본 클래스 기본값 `co_return complete()`로 연결. CPU 전용 모의 제공자에 적합. |
| `complete_async()`만 | 비동기는 직접 작동; 동기 `complete`는 `run_sync(complete_async())`로 연결. |
| `complete_stream()`만 | 동기 스트리밍은 직접 작동; 비동기 짝은 작업자 스레드에서 실행하고 대기 중인 실행자에서 콜백 전달. |
| `complete_stream_async()`만 | 네이티브 비동기 스트리밍은 직접 작동; 직접 동기 스트리밍 호출이 기본 수집 대체 경로를 피해야 한다면 동기 짝도 구현. |

**새** 백엔드의 경우 이 쌍들 중에서 선택하지 말 것. `CompletionProvider`를 상속하고 `do_invoke(CompletionRequest)`를 구현하며 `request.streaming()`으로 전송 방식을 선택. 새 직접 호출자는 `CompletionRequest::collect(...)` 또는 `CompletionRequest::stream(...)`과 함께 `invoke_request()`를 사용해야 한다. 호환성 및 보안 수정은 기존 진입점에도 계속 적용되지만, 새 기능은 명시적 요청 전용일 수 있다.

### 9.4 `CheckpointStore`

8개 동기 메서드, 8개 비동기 짝, 1:1 일치. 배포된 저장소 (`InMemoryCheckpointStore`, `SqliteCheckpointStore`, `PostgresCheckpointStore`)는 모두 비동기 측을 구현하고 동기 연결은 기본 클래스 기본값을 통해.

- **비동기 가능 백엔드** (libpq 비차단, 비동기 MongoDB 드라이버 등): 8개 `*_async` 짝을 모두 재정의. 동기 호출 경로는 호출당 하나의 `run_sync` 비용 — `get_state` / `update_state` 관리 호출에는 괜찮지만 핫 루프에서는 안 됨(하지만 엔진은 동기 체크포인트 메서드를 절대 호출하지 않음; 사용자 도구만 함).
- **블로킹 전용 백엔드** (오래된 파일 I/O, 일부 ODBC 래퍼): 8개 동기 메서드 재정의. 비동기 호출자는 각 호출에서 `run_sync`를 통해 코루틴 스레드를 블록하며, 이는 체크포인트 쓰기가 노드 디스패치에 비해 드물기 때문에 일반적으로 허용됨.
- **섞지 말 것**: `save()`를 재정의하고 `save_async()`를 기본값으로 두면, 비동기 짝이 기본 클래스 기본값을 통해 다시 동기로 연결 — 정확하지만 비동기 I/O 이점을 잃음. 인터페이스당 전부 동기 또는 전부 비동기로 갈 것.

### 9.5 `MCPClient`

`rpc_call_async()`가 실제 구현; `rpc_call()`은 얇은 `run_sync(rpc_call_async(...))` 파사드. **사용자 확장 불가** — `MCPClient`는 상속되도록 설계되지 않았으며, 그대로 사용. 사용자 정의 MCP 전송이 필요하면 새 클래스를 작성; 상속하지 말 것.

HTTP 요청은 정상적으로 중첩된다. stdio 쓰기는 짧은 쓰기 잠금 아래 JSON 줄을 완성하고, 단일 리더가 JSON-RPC id로 순서가 다른 응답을 연관 짓는다. 따라서 stdio 호출도 서브프로세스가 요청을 동시에 처리할 때 중첩된다; 직렬 서브프로세스는 처리량 하한으로 남는다.

### 9.6 `Tool` vs `AsyncTool`

설계상 비대칭. 클래스 선언 시 하나를 선택:

```cpp
class MyCpuTool : public Tool {
  public:
    std::string execute(const json& args) override { /* sync */ }
    ChatTool get_definition() const override { /* ... */ }
    std::string get_name() const override { return "cpu-tool"; }
};

class MyHttpTool : public AsyncTool {
  public:
    asio::awaitable<std::string> execute_async(const json& args) override {
        auto ex = co_await asio::this_coro::executor;
        auto r = co_await neograph::async::async_post(ex, /* ... */);
        co_return r.body;
    }
    // sync execute() is final and routes through run_sync automatically.
    ChatTool get_definition() const override { /* ... */ }
    std::string get_name() const override { return "http-tool"; }
};
```

둘 다 상속하거나 한 클래스의 양쪽 표면을 모두 재정의하려고 하지 **말 것** — `AsyncTool::execute`는 정확히 그것을 방지하기 위해 `final`이다.