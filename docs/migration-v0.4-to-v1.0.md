# Migration: 옛 8-virtual chain → 새 `run(NodeInput)` (v0.4 → v1.0)

NeoGraph v0.4 가 노드의 dispatch 진입점을 `run(NodeInput) ->
awaitable<NodeOutput>` 하나로 통합했습니다. 옛 8개 virtual
(`execute` / `execute_async` / `execute_stream` /
`execute_stream_async` 와 그 `_full` 짝꿍) 은 `[[deprecated]]` 마킹돼
있고 v1.0 에서 삭제됩니다. 이 문서가 옮기는 절차.

> v0.4 ~ v0.x 동안에는 옛 virtual 들이 그대로 동작합니다 — 한 번에
> 다 옮길 필요 없고, 새 노드만 새 모양으로 짜도 무방. 이 문서는
> v1.0 직전 (또는 v0.x 사이클에서 컴파일러 경고가 거슬리기 시작할
> 때) 보면 됩니다.

## 왜 옮기나

옛 모양 — `(sync/async) × (writes/full) × (stream/non-stream)` =
8 virtual cross-product. 어느 하나만 override 하면 다른 7 개에서
default 가 fallback chain 을 타고 호출됨. 안전한 조합도 있고
런타임 함정 (예: sync `execute_full` + async dispatch → nested
`run_sync` race) 도 있어서 사용자가 어느 함수를 override 해야 하는지
명확하지 않음.

새 모양 — `run(NodeInput) -> awaitable<NodeOutput>` 한 개. 하나만
override. sync vs async 는 호출자 안 신경 씀 (사용자가 코루틴 안에
`co_await` 으로 async 호출하든, 그냥 동기 코드 쓰든 자유). Command /
Send 는 `NodeOutput` 에 들어 있어서 추가 virtual 필요 없음. 스트리밍
콜백은 `NodeInput::stream_cb` (포인터, null 가능) 로 들어옴.

## 8 virtual → 새 `run()` 매핑표

| 옛 virtual | 옮긴 모양 |
|---|---|
| `execute(state)` | `co_return NodeOutput{ .writes = {...} };` (sync 본문) |
| `execute_async(state)` | `co_return co_await provider->complete_async(...);` 같은 native async |
| `execute_stream(state, cb)` | `if (in.stream_cb) (*in.stream_cb)(event); co_return NodeOutput{...};` |
| `execute_stream_async(state, cb)` | 위 + native async (`co_await ...`) |
| `execute_full(state)` | `NodeOutput out; out.writes=...; out.command=...; co_return out;` |
| `execute_full_async(state)` | 위 + native async |
| `execute_full_stream(state, cb)` | `execute_full` + `in.stream_cb` 사용 |
| `execute_full_stream_async(state, cb)` | 위 + native async |

핵심: **8 갈래가 `NodeOutput` 의 어느 필드를 채우느냐 + `in.stream_cb`
사용 유무 + `co_await` 사용 유무 의 조합**으로 표현 가능. virtual 은 1개.

## 케이스별 변환 예제

### 케이스 1 — 가장 단순한 sync 노드

**옛:**
```cpp
class MyNode : public GraphNode {
public:
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        int n = state.get("counter").get<int>();
        return {ChannelWrite{"counter", json(n + 1)}};
    }
    std::string get_name() const override { return "my_node"; }
};
```

**새:**
```cpp
class MyNode : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        int n = in.state.get("counter").get<int>();
        NodeOutput out;
        out.writes.push_back({"counter", json(n + 1)});
        co_return out;
    }
    std::string get_name() const override { return "my_node"; }
};
```

차이:
- `state` → `in.state`
- 반환값을 `NodeOutput` 에 담음 (`writes` 필드)
- 함수가 `asio::awaitable<NodeOutput>` 이고 마지막에 `co_return`

### 케이스 2 — async LLM 노드 (`execute_async` 옮기기)

**옛:**
```cpp
class TalkNode : public GraphNode {
    std::shared_ptr<Provider> prov_;
public:
    asio::awaitable<std::vector<ChannelWrite>>
    execute_async(const GraphState& state) override {
        auto reply = co_await prov_->complete_async({
            .messages = state.get_messages(),
            .model    = "gpt-mock",
        });
        co_return std::vector<ChannelWrite>{
            {"reply", json(reply.message.content)}
        };
    }
    std::string get_name() const override { return "talk"; }
};
```

**새:**
```cpp
class TalkNode : public GraphNode {
    std::shared_ptr<Provider> prov_;
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto reply = co_await prov_->complete_async({
            .messages = in.state.get_messages(),
            .model    = "gpt-mock",
        });
        NodeOutput out;
        out.writes.push_back({"reply", json(reply.message.content)});
        co_return out;
    }
    std::string get_name() const override { return "talk"; }
};
```

### 케이스 3 — 스트리밍 노드 (`execute_stream` 옮기기)

**옛:**
```cpp
std::vector<ChannelWrite>
execute_stream(const GraphState& state, const GraphStreamCallback& cb) override {
    auto reply = prov_->complete_stream(params, [&](const std::string& chunk) {
        cb({GraphEvent::Type::LLM_TOKEN, "talk", json(chunk)});
    });
    return {ChannelWrite{"reply", json(reply.message.content)}};
}
```

**새:**
```cpp
asio::awaitable<NodeOutput> run(NodeInput in) override {
    // in.stream_cb 는 포인터 — null 이면 호출자가 streaming 안 원함.
    auto on_chunk = [&](const std::string& chunk) {
        if (in.stream_cb) {
            (*in.stream_cb)({GraphEvent::Type::LLM_TOKEN, "talk", json(chunk)});
        }
    };
    auto reply = prov_->complete_stream(params, on_chunk);
    NodeOutput out;
    out.writes.push_back({"reply", json(reply.message.content)});
    co_return out;
}
```

### 케이스 4 — Command / Send 쓰는 노드 (`execute_full` 옮기기)

**옛:**
```cpp
NodeResult execute_full(const GraphState& state) override {
    NodeResult r;
    r.writes.push_back({"step", json("dispatched")});
    r.command = Command{.goto_node = "next_router"};   // 라우팅 강제
    return r;
}
```

**새:**
```cpp
asio::awaitable<NodeOutput> run(NodeInput in) override {
    NodeOutput out;   // NodeOutput == NodeResult — 같은 타입의 alias
    out.writes.push_back({"step", json("dispatched")});
    out.command = Command{.goto_node = "next_router"};
    co_return out;
}
```

`NodeOutput` 은 `NodeResult` 의 별명 — 옛 코드의 `NodeResult` 도
그대로 컴파일됩니다.

## 자주 하는 실수

### `NodeInput in` 은 by-value

```cpp
// ❌ 잘못 — 코루틴 ref-param UAF, pybind async path 에서 SEGV
asio::awaitable<NodeOutput> run(const NodeInput& in) override { ... }

// ✅ 맞음
asio::awaitable<NodeOutput> run(NodeInput in) override { ... }
```

이유: 코루틴 frame 이 인자 사본을 가져야 안전. 참조로 받으면
호출자 stack frame 이 사라진 뒤 `in.state` 가 dangling. PR 2 작업
중 실제로 발생한 버그.

### cancel / store / stream_cb 는 모두 `in.ctx` 에서

옛 노드는 cancel token 을 `state.run_cancel_token_` 같은 smuggling
채널로 받았는데, v0.4 부터는 `RunContext` 가 정식 plumbing:

```cpp
asio::awaitable<NodeOutput> run(NodeInput in) override {
    // 취소 신호 확인
    if (in.ctx.cancel_token && in.ctx.cancel_token->is_cancelled()) {
        throw CancelledException("user cancelled");
    }

    // Store 접근 (issue #27)
    if (in.ctx.store) {
        auto user_pref = in.ctx.store->get({"users", in.ctx.thread_id}, "lang");
        // ...
    }

    // Streaming sink (null 가능)
    if (in.stream_cb) {
        (*in.stream_cb)({GraphEvent::Type::NODE_END, "my_node", json(...)});
    }

    co_return NodeOutput{};
}
```

현재 노드가 활용할 수 있는 `in.ctx` 필드는 `cancel_token`, `usage`,
`thread_id`, `step`, `stream_mode`, `store`, `resume_value`입니다.
`deadline`과 `trace_id`는 향후 `RunConfig` 확장을 위한 예약 슬롯으로,
현재 엔진은 값을 채우지 않으며 Python에도 노출하지 않습니다.

### `_full` virtual 옮기는 사람 — `co_return out;` 한 줄로 끝

옛 `execute_full` 사용자가 가장 자주 헤매는 부분:
"`NodeResult` 는 옛 타입인데 `NodeOutput` 을 반환해야 하나?"
→ 둘은 같은 타입의 별명입니다. `NodeOutput out; out.writes=...;
out.command=...; out.sends=...; co_return out;` 만 하면 됨.

### 옛 virtual 도 같이 두는 transitional 패턴

마이그레이션 중간에 새 `run()` 도 박고 옛 `execute()` 도 그대로
두면, **새 `run()` 이 우선**되어서 호출됩니다 (engine 의 PR 2
dispatch). 옛 본문은 dead code — 안전하게 지우면 됩니다.

```cpp
class MyNode : public GraphNode {
public:
    // 새 진입점 — 우선됨
    asio::awaitable<NodeOutput> run(NodeInput in) override { ... }

    // 옛 진입점 — 호출 안 됨 (dead). 지워도 됨.
    std::vector<ChannelWrite> execute(const GraphState&) override {
        // ...
    }
};
```

## 마이그레이션 안 하면

v0.4 ~ v0.x 동안:
- 옛 virtual override 는 그대로 동작
- `[[deprecated]]` 컴파일러 경고가 뜸 (`-Wdeprecated-declarations`)
- 컴파일 / 런타임 모두 멀쩡 — 그냥 시끄러울 뿐

v1.0 에서:
- 옛 8 virtual 전부 삭제
- 옛 모양 노드는 컴파일 안 됨 (`'execute' marked override but doesn't
  override anything in the base class`)
- 그 시점에 옮겨야 함

선제 마이그레이션 추천 — 노드 개수가 많을수록 v1.0 직후 한꺼번에
옮기는 게 부담. 새 노드부터 새 모양으로 짜고, 옛 노드는 시간 날 때
하나씩.

## 일괄 옮기는 스크립트가 있나?

없습니다 — virtual 시그니처가 8 가지로 다양해서 정규식 변환이 깔끔하지
않습니다. 케이스별 변환 (위 4 가지 예제) 을 사용자가 보고 손으로
옮기는 게 안전.

가장 자주 쓰이는 패턴 (`execute(state)` 만 override) 의 경우 다음
정도의 sed/awk 한 줄이면 1차 변환 가능 — 검토는 사람이 해야 합니다:

```bash
# 매우 거친 1차 변환 — 한 줄짜리 execute 만 override 하는 노드 한정.
# 절대 -i 없이 dry-run 부터.
grep -lE 'execute\(const GraphState' src/**/*.cpp
# 결과 파일 하나씩 열어서 새 모양으로 손편집.
```

복잡한 노드 (`execute_full`, `execute_stream_async` 등) 는 무조건
손편집. 쇼트컷 없습니다.

---

# Migration 2: `Provider` 호환 정책과 새 명시적 요청 API (v0.9+)

기존 `Provider::complete*` 네 메서드와 callback 기반 `invoke()`는 안정 API로
계속 지원하며 제거 계획이 없다. 기존 구현과 호출자는 옮기지 않아도 된다.
다만 새 Provider 구현에는 `CompletionProvider::do_invoke()` 하나를 재정의하는
방식을, 새 직접 호출에는 `invoke_request(CompletionRequest)`를 권장한다.

## 기존 호출과 권장 새 호출

| 안정 호환 API | `CompletionProvider`를 직접 쓸 때 권장하는 API |
|---|---|
| `complete(params)` | `run_sync(invoke_request(CompletionRequest::collect(params)))` |
| `complete_async(params)` | `co_await invoke_request(CompletionRequest::collect(params))` |
| `complete_stream(params, on_chunk)` | `run_sync(invoke_request(CompletionRequest::stream(params, on_chunk)))` |
| `complete_stream_async(params, on_chunk)` | `co_await invoke_request(CompletionRequest::stream(params, on_chunk))` |

`CompletionRequest`는 callback 유무와 전송 방식을 분리한다. 따라서 callback이
없어도 `CompletionRequest::stream(params)`로 streaming 전송을 명시할 수 있다.
`Provider&`나 `Provider*`만 가진 코드는 기존 `complete*`를 그대로 쓰면 된다.

## 케이스별 변환

### Provider를 호출하는 사용자 코드

```cpp
// 안정 호환 API — 계속 지원됨
auto completion = co_await provider->complete_async(params);
```

```cpp
// CompletionProvider를 직접 쓰는 새 코드 — mode가 명시적
auto completion = co_await provider.invoke_request(
    CompletionRequest::collect(params));
```

### 사용자 정의 Provider subclass

기존 `Provider` subclass는 계속 동작한다. 새 구현은 별도
`CompletionProvider`를 상속하고 `do_invoke()` 하나만 구현하는 방식을 권장한다.
기존 `Provider`의 vtable과 Python `complete()` 계약은 바꾸지 않는다.

```cpp
// 옛 모양 — async-native provider
class MyProvider : public neograph::Provider {
public:
    asio::awaitable<ChatCompletion>
    complete_async(const CompletionParams& params) override {
        // ... HTTP call ...
        co_return result;
    }

    ChatCompletion complete_stream(const CompletionParams& params,
                                   const StreamCallback& on_chunk) override {
        // ... SSE call ...
        return result;
    }

    std::string get_name() const override { return "my"; }
};
```

```cpp
// 새 모양 — transport mode가 callback 유무와 분리됨
class MyProvider : public neograph::CompletionProvider {
public:
    asio::awaitable<ChatCompletion>
    do_invoke(CompletionRequest request) override {
        if (request.streaming()) {
            // callback이 없어도 SSE/WS transport를 사용한다.
            // callback이 있으면 토큰마다 request.on_chunk()(token).
        } else {
            // ... HTTP call ...
        }
        co_return result;
    }

    std::string get_name() const override { return "my"; }
};
```

새 호출자는 mode를 명시한다.

```cpp
auto full = co_await provider.invoke_request(
    CompletionRequest::collect(params));

auto streamed = co_await provider.invoke_request(
    CompletionRequest::stream(params, on_chunk));

// token을 관찰하지 않아도 streaming transport를 명시할 수 있다.
auto streamed_without_observer = co_await provider.invoke_request(
    CompletionRequest::stream(params));
```

기존 네 virtual과 `invoke(params, on_chunk)`는 계속 지원된다.
`CompletionProvider`의 final adapter가 모든 기존 진입점을 `do_invoke()`로 한 번만
연결하므로 상호 재귀가 생기지 않는다.

## cancel 자동 propagation

취소가 필요하면 `CompletionParams::cancel_token`을 명시한다. 엔진 내부 노드는
`RunContext`의 token을 params에 넣어 Provider로 전달한다. thread-local 기반의
암묵적 전달 경로는 더 이상 사용하지 않는다.

```cpp
// engine 안 노드 본문 — 둘 다 동일하게 cancel 됨
co_await provider->invoke(params, nullptr);                    // OK
neograph::async::run_sync(provider->invoke(params, nullptr));  // OK
```

graph 밖에서 직접 호출하는 케이스 (`Agent` user code 등) 도 동일 —
explicit cancel_token 안 주면 그냥 cancel 못 받음 (예전과 동일).

## 마이그레이션 안 하면

아무 조치도 필요 없다:
- 기존 네 virtual 재정의와 직접 호출은 그대로 동작한다.
- Provider 관련 `-Wdeprecated-declarations` 경고는 더 이상 발생하지 않는다.
- 호환성·보안 수정은 기존 API에도 적용한다.

기존 API 제거 계획은 없다. 다만 새 기능은 명시적 요청 계약에만 추가될 수
있으므로, 새 구현은 `CompletionProvider`로 작성하는 편이 낫다.

## 자동 변환 가능?

호출 사이트는 패턴 단순:

```bash
# dry-run 으로 검토
grep -rnE '->complete(_async|_stream|_stream_async)?\(' your/code

# 그 다음 케이스별로 손편집 (위 매핑표 참고).
```

기존 Provider subclass를 `CompletionProvider::do_invoke()` 하나로 합치는 작업은
선택 사항이며, 로직이 섞여 있어서 손편집이 필요하다.

## 관련 문서 / 이슈

- [`include/neograph/graph/node.h`](../include/neograph/graph/node.h) —
  새 `run(NodeInput)` virtual 의 인라인 docstring (예제 포함)
- [ROADMAP_v1.md](../ROADMAP_v1.md) — Candidate 1 (GraphNode 8-virtual
  flattening) 의 상세 설계 메모
- [troubleshooting.md](troubleshooting.md) — 실제 옮기다 부딪치는
  컴파일 에러 / 런타임 차이 정리
- [Issue #5](https://github.com/fox1245/NeoGraph/issues/5) — Provider의
  한 메서드 구현 경로와 영구 호환 정책 결정 기록

---

# Migration 3: `compile()` 의 worker pool default 가 1 로 (v0.1.4 회귀 복귀)

## 무엇이 바뀌었나

`GraphEngine::compile(def, ctx)` 의 default worker count 가 v0.1.4
(`b59444f`) 부터 `std::thread::hardware_concurrency()` 였음. v1.0
에서 다시 **`1` (= no engine-owned thread_pool)** 로 복귀.

## 왜

`hardware_concurrency` default 가 모든 fan-out 노드에 cross-thread
submit 비용 (~6-7 µs/task) 부담시킴 — bench 의 par 측정 (5 워커
+ summarizer) 이 11.6 µs → 44 µs 로 4× 회귀. 회귀를 우리 측정
환경에서 git bisect 로 v0.1.4 의 `b59444f` 까지 좁힘.

진짜 production workload (LLM call ms~s 단위) 는 submit overhead
무시 가능, 그러나:
- **단순 그래프 (no fan-out)** 도 pool overhead 부담 — 의미 없음
- **non-thread-safe 노드 상태** 가 default 로 멀티 워커 노출 — 진짜
  footgun

그래서 default 를 안전한 1 로 두고, 사용자가 진짜 fan-out 병렬화
필요할 때 명시 opt-in.

## 마이그레이션

fan-out 병렬화가 필요한 그래프 (예: `Send` 다수 발사, `parallel_group`,
deep_research 의 5-researcher fan-out) 는 `compile()` 후 명시 호출:

```cpp
auto engine = GraphEngine::compile(def, ctx);
engine->set_worker_count_auto();  // hardware_concurrency()
// 또는
engine->set_worker_count(4);  // 정확한 N 지정
```

```python
engine = ng.GraphEngine.compile(def, ctx)
engine.set_worker_count_auto()
```

단순 그래프 (no fan-out) 또는 fan-out 이 가벼운 그래프 (LLM call
dominant) 는 default 그대로 — pool overhead 0.

## 마이그레이션 안 하면

- 사용자 그래프가 fan-out 시 단일 thread 로 직렬 실행 (정합 보장)
- 진짜 wallclock 회복은 못 받음 — `set_worker_count_auto()` 명시
  필요

## 영향 받는 NeoGraph 내부 예제

본 변경 직후 함께 들어간 fan-out 가시화 패치 — 의도 보존을 위해
명시 호출 추가. 사용자 코드도 같은 패턴이면 따라 적용:

- `examples/10_send_command.cpp` — sync `sleep_for` ResearcherNode 가
  Send 로 fan-out, `engine->set_worker_count_auto()` 추가
- `examples/14_plan_executor.cpp` — 5 sub-topic Send fan-out (sync
  sleep_for), 동일하게 추가
- `examples/21_mcp_fanout.cpp` — MCP tool call 3개 동시 발사, 동일
- `examples/36_classifier_fanout.cpp` — 이미 `set_worker_count(5)` 명시
  돼 있었음. 주석에서 거짓말 (지금 기본=hardware_concurrency) 만 수정
- `src/core/deep_research_graph.cpp` 의 `create_deep_research_graph()`
  builder — `compile()` 직후 `set_worker_count_auto()` 호출. supervisor
  가 띄우는 N researcher 가 진짜로 동시 실행되도록

`examples/05_parallel_fanout.cpp` 는 `io_context` 위 코루틴 timer
overlap 방식 (sync sleep 없음) 이라 워커 풀 영향 없음 — 그대로 둠.

같은 패턴이 사용자 코드에 있다면:

```cpp
auto engine = GraphEngine::compile(def, ctx);
engine->set_worker_count_auto();   // ← 본 줄 추가
```

자세한 측정 결과는 ROADMAP_v1.md 의 perf section 참조 (별도 추가).
