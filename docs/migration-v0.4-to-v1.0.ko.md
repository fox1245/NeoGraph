<!-- neograph-i18n: source=docs/migration-v0.4-to-v1.0.md locale=ko source_sha256=d2b0e6c51e07b5877e07c5768ade91ce2129fec5f6441f92c7f7b8feae6026b9 -->
# 이전 안내: 기존 8 가상 함수 → `run(NodeInput)` (v0.4.x → v0.9+)

**Languages:** [English](migration-v0.4-to-v1.0.md) | [한국어](migration-v0.4-to-v1.0.ko.md) | [日本語](migration-v0.4-to-v1.0.ja.md) | [简体中文](migration-v0.4-to-v1.0.zh-CN.md)

NeoGraph v0.4는 노드 진입점을 단일 `run(NodeInput) -> awaitable<NodeOutput>`으로 통합했다. 기존 8개 가상 함수 (`execute` / `execute_async` / `execute_stream` / `execute_stream_async`와 각각의 `_full` 대응)는 v0.4.x에서 사용 중단(deprecated)되었고 v1 준비 릴리스인 v0.9.0에서 제거되었다. 이 문서는 기존 노드를 현재 API로 이전하는 절차를 설명한다.

> v0.9.0 이후로는 `run(NodeInput)`을 구현하지 않은 C++ 하위 클래스는 추상 클래스로 컴파일에 실패한다. Python 하위 클래스도 `run(self, input)`을 구현해야 한다.

## 왜 이전하는가

옛 패턴 — `(sync/async) × (writes/full) × (stream/non-stream)` = 8 가상 함수 데카르트 곱. 하나만 재정의하면 나머지 7개가 기본 체인으로 대체된다. 일부 조합은 안전하지만 런타임 함정이 있다(예: 동기 `execute_full` + 비동기 디스패치 → 중첩 `run_sync` 경쟁). 사용자가 어떤 함수를 재정의해야 하는지 불분명했다.

새 패턴 — 단일 `run(NodeInput) -> awaitable<NodeOutput>`. 하나만 재정의. 동기 vs 비동기 구분은 호출자의 관심사(사용자는 코루틴 안에서 `co_await`를 쓰거나 일반 동기 코드를 자유롭게 사용 가능). Command / Send는 `NodeOutput`에 포함되어 있어 추가 가상 함수가 필요 없다. 스트리밍 콜백은 `NodeInput::stream_cb` (nullable 포인터)를 통해 도착한다.

## 8 가상 함수 → 새 `run()` 매핑

| 기존 가상 함수 | 이전된 형태 |
|---|---|
| `execute(state)` | `NodeOutput out; out.writes = {...}; co_return out;` (동기 본문) |
| `execute_async(state)` | `co_return co_await provider->complete_async(...);` 같은 네이티브 비동기 |
| `execute_stream(state, cb)` | `if (in.stream_cb) (*in.stream_cb)(event); co_return NodeOutput{...};` |
| `execute_stream_async(state, cb)` | 위 + 네이티브 비동기 (`co_await ...`) |
| `execute_full(state)` | `NodeOutput out; out.writes=...; out.command=...; co_return out;` |
| `execute_full_async(state)` | 위 + 네이티브 비동기 |
| `execute_full_stream(state, cb)` | `execute_full` + `in.stream_cb` 사용 |
| `execute_full_stream_async(state, cb)` | 위 + 네이티브 비동기 |

핵심: **8개 변종은 어떤 `NodeOutput` 필드를 채웠는지 + `in.stream_cb` 사용 여부 + `co_await` 사용 여부의 조합으로 표현 가능하다.** 단 하나의 가상 함수만 남는다.

### 가장 흔한 Python 이전

**옛 코드:**

```python
class CounterNode(ng.GraphNode):
    def execute(self, state):
        current = state.get("count") or 0
        return [ng.ChannelWrite("count", current + 1)]
```

**현재 코드:**

```python
class CounterNode(ng.GraphNode):
    def run(self, input):
        current = input.state.get("count") or 0
        return [ng.ChannelWrite("count", current + 1)]
```

Python의 `run`은 `async def`가 아닌 일반 `def`이다. 스트리밍 실행 시 `input.stream_cb`는 이벤트를 받는 함수이며, 일반 실행 시 `None`이다.

## 사례별 변환 예제

### 사례 1 — 가장 단순한 동기 노드

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

차이점:
- `state` → `in.state`
- 반환값이 `NodeOutput`으로 감싸짐 (`writes` 필드)
- 함수가 `asio::awaitable<NodeOutput>`이며 `co_return`으로 끝남

### 사례 2 — 비동기 LLM 노드 (`execute_async` 이전)

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

### 사례 3 — 스트리밍 노드 (`execute_stream` 이전)

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
    // in.stream_cb is a pointer — null means the caller does not want streaming.
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

### 사례 4 — Command / Send를 사용하는 노드 (`execute_full` 이전)

**옛:**
```cpp
NodeResult execute_full(const GraphState& state) override {
    NodeResult r;
    r.writes.push_back({"step", json("dispatched")});
    Command command;
    command.goto_node = "next_router";
    r.command = command;   // Force routing
    return r;
}
```

**새:**
```cpp
asio::awaitable<NodeOutput> run(NodeInput in) override {
    NodeOutput out;   // NodeOutput == NodeResult — alias of the same type
    out.writes.push_back({"step", json("dispatched")});
    Command command;
    command.goto_node = "next_router";
    out.command = command;
    co_return out;
}
```

`NodeOutput`은 `NodeResult`의 별칭 — 기존 `NodeResult` 코드는 여전히 컴파일된다.

## 흔한 실수

### `NodeInput in`은 값으로 받는다

```cpp
// ❌ Wrong — coroutine ref-param UAF, SEGV in pybind async path
asio::awaitable<NodeOutput> run(const NodeInput& in) override { ... }

// ✅ Correct
asio::awaitable<NodeOutput> run(NodeInput in) override { ... }
```

이유: 코루틴 프레임이 안전을 위해 인자 복사본을 가져야 한다. 참조로 받으면 호출자 스택 프레임이 사라진 뒤 `in.state`가 허상(dangling)이 된다. PR 2 작업 중 발생한 실제 버그.

### cancel / store / stream_cb는 모두 `in.ctx`에서 온다

기존 노드는 `state.run_cancel_token_` 같은 은밀한 채널(smuggling channel)을 통해 취소 토큰을 받았지만, v0.4는 `RunContext`를 공식 배관(plumbing)으로 도입했다:

```cpp
asio::awaitable<NodeOutput> run(NodeInput in) override {
    // Check cancellation signal
    if (in.ctx.cancel_token && in.ctx.cancel_token->is_cancelled()) {
        throw CancelledException("user cancelled");
    }

    // Store access (issue #27)
    if (in.ctx.store) {
        auto user_pref = in.ctx.store->get({"users", in.ctx.thread_id}, "lang");
        // ...
    }

    // Streaming sink (nullable)
    if (in.stream_cb) {
        (*in.stream_cb)({GraphEvent::Type::NODE_END, "my_node", json(...)});
    }

    co_return NodeOutput{};
}
```

C++ 노드에서 사용 가능한 `in.ctx` 필드: `cancel_token`, `usage`, `thread_id`,
`step`, `stream_mode`, `store`, `resume_value`, `deadline`, `trace_id`. 마지막 두
필드는 `RunMetadata`에서 설정하며 엔진은 중첩 subgraph까지 보존한다. 체크포인트
라우팅은 엔진 내부 구현이며 공개 `RunContext` 필드가 아니다. Python은 `trace_id`,
`run_id`, `model_token_budget`, `has_deadline`, `deadline_remaining_ms`를 노출한다.
원시 C++ steady-clock 데드라인은 의도적으로 불투명하게 유지된다.

### `_full` 가상 함수 이전 — `co_return out;` 한 줄로 마무리

기존 `execute_full` 사용자에게 가장 흔한 혼란:
"`NodeResult`는 옛 타입인데 `NodeOutput`을 반환해야 하나?"
→ 둘은 같은 타입의 별칭이다. 그냥 `NodeOutput out; out.writes=...; out.command=...; out.sends=...; co_return out;` 하면 된다.

## 이전하지 않으면 어떻게 되는가

v0.9.0 이후로 기존 8개 가상 함수는 사라졌다.

- C++ 기존 `override`는 `'execute' marked override but does not override`와 같은 컴파일 오류 발생.
- `execute()`만 구현한 Python 노드는 `run(input)`을 요구하는 `NotImplementedError` 발생.

옛 메서드 이름을 그대로 두는 전환 패턴을 사용하지 말 것. 엔진은 `run(NodeInput)`만 호출하므로, 옛 본문은 절대 실행되지 않는다.

## 대량 이전 스크립트가 있는가?

없다 — 가상 함수 시그니처가 8가지 형태로 달라 정규식 기반 변환이 비현실적이다. 사용자는 사례별 예제(위의 4개 예제)를 읽고 수동으로 이전해야 한다.

가장 흔한 패턴 (`execute(state)`만 재정의)의 경우, 다음 sed/awk 한 줄짜리가 초기 통과에 도움이 될 수 있다 — 사람 검토는 필수:

```bash
# Very rough initial pass — nodes with single-line execute override only.
# Always dry-run without -i first.
grep -lE 'execute\(const GraphState' src/**/*.cpp
# Manually edit each resulting file to the new pattern.
```

복잡한 노드 (`execute_full`, `execute_stream_async` 등)는 반드시 수동 편집. 지름길은 없다.

---

# 이전 2: `Provider` 호환성 정책과 새 명시적 요청 API (v0.9+)

기존 `Provider::complete*` 네 메서드와 콜백 기반 `invoke()`는 제거 계획이 없는 안정된 API이다. 기존 구현과 호출자는 이전할 필요가 없다. 그러나 새 `Provider` 구현은 `CompletionProvider::do_invoke()`만 재정의하고, 새 직접 호출은 `invoke_request(CompletionRequest)`를 사용해야 한다.

## 기존 vs 권장 새 호출 패턴

| 안정된 호환 API | `CompletionProvider` 직접 사용 시 권장 API |
|---|---|
| `complete(params)` | `run_sync(invoke_request(CompletionRequest::collect(params)))` |
| `complete_async(params)` | `co_await invoke_request(CompletionRequest::collect(params))` |
| `complete_stream(params, on_chunk)` | `run_sync(invoke_request(CompletionRequest::stream(params, on_chunk)))` |
| `complete_stream_async(params, on_chunk)` | `co_await invoke_request(CompletionRequest::stream(params, on_chunk))` |

`CompletionRequest`는 콜백 존재와 전송 모드를 분리한다. 따라서 콜백이 없어도 `CompletionRequest::stream(params)`는 명시적으로 스트리밍 전송을 요청한다. `Provider&` 또는 `Provider*`만 보유한 코드는 기존 `complete*` 메서드를 그대로 사용할 수 있다.

## 사례별 변환

### 사용자 코드에서 Provider 호출

```cpp
// Stable compatible API — continues to be supported
auto completion = co_await provider->complete_async(params);
```

```cpp
// New code using `CompletionProvider` directly — mode is explicit
auto completion = co_await provider.invoke_request(
    CompletionRequest::collect(params));
```

### 사용자 정의 Provider 하위 클래스

기존 `Provider` 하위 클래스는 계속 작동한다. 새 구현은 `CompletionProvider`를 상속하고 `do_invoke()`만 구현해야 한다. 기존 `Provider` vtable과 Python `complete()` 계약은 변경되지 않는다.

```cpp
// Old pattern — async-native provider
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
// New pattern — transport mode is decoupled from callback presence
class MyProvider : public neograph::CompletionProvider {
public:
    asio::awaitable<ChatCompletion>
    do_invoke(CompletionRequest request) override {
        if (request.streaming()) {
            // Uses SSE/WS transport even without callback.
            // With callback, invokes request.on_chunk()(token) per token.
        } else {
            // ... HTTP call ...
        }
        co_return result;
    }

    std::string get_name() const override { return "my"; }
};
```

새 호출자는 모드를 명시적으로 지정.

```cpp
auto full = co_await provider.invoke_request(
    CompletionRequest::collect(params));

auto streamed = co_await provider.invoke_request(
    CompletionRequest::stream(params, on_chunk));

// Can explicitly request streaming transport even without observing tokens.
auto streamed_without_observer = co_await provider.invoke_request(
    CompletionRequest::stream(params));
```

기존 네 가상 함수와 `invoke(params, on_chunk)`는 계속 지원된다. `CompletionProvider` 최종 어댑터는 모든 기존 진입점을 `do_invoke()`로 한 번 연결하여 상호 재귀를 방지한다.

## 자동 취소 전파

취소가 필요하면 `CompletionParams::cancel_token`을 지정. 내부 엔진 노드는 `RunContext` 토큰을 params의 제공자에게 전달. 스레드-로컬 암시적 전파 경로는 더 이상 사용되지 않는다.

```cpp
// Node body inside engine — both cancel identically
co_await provider->invoke(params, nullptr);                    // OK
neograph::async::run_sync(provider->invoke(params, nullptr));  // OK
```

그래프 밖의 직접 호출자(예: `Agent` 사용자 코드)도 동일한 패턴을 따른다 — 명시적 cancel_token 없이는 취소를 받을 수 없다(이전과 동일).

## 이전하지 않으면 어떻게 되는가

조치가 필요하지 않다:
- 기존 가상 함수 재정의와 직접 호출은 계속 작동.
- Provider 관련 `-Wdeprecated-declarations` 경고는 더 이상 발생하지 않음.
- 호환성 및 보안 수정은 기존 API에도 적용.

기존 API 제거 계획은 없다. 그러나 새 기능은 명시적 요청 계약에만 추가될 수 있으므로, 새 기능 구현 시 `CompletionProvider`가 바람직하다.

## 자동 변환 가능한가?

호출 지점은 단순한 패턴을 따른다:

```bash
# Review with dry-run
grep -rnE '->complete(_async|_stream|_stream_async)?\(' your/code

# Then manually edit case-by-case (refer to the mapping table above).
```

기존 `Provider` 하위 클래스를 단일 `CompletionProvider::do_invoke()`로 병합하는 것은 선택 사항이며, 얽힌 로직 때문에 수동 편집이 필요하다.

## 관련 문서 / 이슈

- [`include/neograph/graph/node.h`](../include/neograph/graph/node.h) — 새 `run(NodeInput)` 가상 함수의 인라인 docstring (예제 포함)
- [ROADMAP_v1.md](../ROADMAP_v1.md) — Candidate 1 (GraphNode 8-가상-함수 평탄화)의 상세 설계 노트
- [troubleshooting.md](troubleshooting.md) — 실제 이전 중 만나는 컴파일 오류와 런타임 차이
- [Issue #5](https://github.com/fox1245/NeoGraph/issues/5) — Provider 메서드 구현 경로와 영구 호환성 정책 결정 기록

---

# 이전 3: `compile()` 작업자 풀 기본값은 1 (v0.1.4 회귀 복원)

## 무엇이 바뀌었는가

`GraphEngine::compile(def, ctx)` 기본 작업자 수가 v0.1.4 (`b59444f`)에서 `std::thread::hardware_concurrency()`였으나 v1.0에서 **`1` (= 엔진 소유 thread_pool 없음)** 로 복원.

## 왜

`hardware_concurrency` 기본값은 모든 팬아웃 노드에 스레드 간 제출 오버헤드(~6-7 µs/작업)를 부과 — 벤치 par 측정 (5 작업자 + 요약기)이 11.6 µs에서 44 µs로 퇴행, 4× 느려짐. 측정 환경 이분 탐색(bisect) 결과 v0.1.4의 `b59444f`가 원인.

실제 운영 작업량(LLM 호출 ms~s 범위)은 제출 오버헤드를 무시할 수 있지만:
- **단순 그래프 (팬아웃 없음)** 도 풀 오버헤드를 지불 — 무의미
- **스레드 안전하지 않은 노드 상태** 가 기본적으로 다중 작업자에 노출 — 실제 발등 찍기

따라서 기본값을 안전하게 1로 설정하고, 사용자는 실제 팬아웃 병렬화를 위해 명시적으로 선택해야 한다.

## 이전

팬아웃 병렬화가 필요한 그래프 (예: 다중 `Send` 디스패치, `parallel_group`, deep_research의 5-연구자 팬아웃)는 `compile()` 후 명시적으로 호출:

```cpp
auto engine = GraphEngine::compile(def, ctx);
engine->set_worker_count_auto();  // hardware_concurrency()
// or
engine->set_worker_count(4);  // specify exact N
```

```python
engine = ng.GraphEngine.compile(def, ctx)
engine.set_worker_count_auto()
```

단순 그래프(팬아웃 없음) 또는 가벼운 팬아웃 그래프(LLM 호출이 지배적)는 기본값 유지 — 풀 오버헤드 0.

## 이전하지 않으면 어떻게 되는가

- 팬아웃이 있는 사용자 그래프는 단일 스레드에서 직렬 실행 (일관성 보장)
- 실제 wallclock 회복은 이루어지지 않음 — 명시적 `set_worker_count_auto()` 필요

## 영향을 받는 NeoGraph 내부 예제

이 변경과 함께 추가된 팬아웃 가시성 패치 — 의도를 보존하기 위해 명시적 호출 추가. 사용자 코드가 일치하면 같은 패턴 적용:

- `examples/10_send_command.cpp` — 동기 `sleep_for` ResearcherNode가 Send로 팬아웃, `engine->set_worker_count_auto()` 추가
- `examples/14_plan_executor.cpp` — 5 서브토픽 Send 팬아웃 (동기 sleep_for), 동일 추가
- `examples/21_mcp_fanout.cpp` — 3 MCP 도구 호출 동시 발사, 동일
- `examples/36_classifier_fanout.cpp` — 이미 `set_worker_count(5)` 명시적. 잘못된 기본값을 명시한 주석 수정 (현재 기본값은 hardware_concurrency)
- `src/core/deep_research_graph.cpp` `create_deep_research_graph()` 빌더 — `compile()` 직후 `set_worker_count_auto()` 호출하여 감독자의 N 연구자가 실제로 동시에 실행

`examples/05_parallel_fanout.cpp`는 `io_context`에서 코루틴 타이머 중첩 사용 (동기 sleep 없음), 따라서 작업자 풀이 효과 없음 — 변경 없음.

사용자 코드에 동일한 패턴이 있다면:

```cpp
auto engine = GraphEngine::compile(def, ctx);
engine->set_worker_count_auto();   // ← add this line
```

상세 측정은 ROADMAP_v1.md 성능 섹션 참조 (별도 추가).

---

# 이전 4: C++ ABI와 필수 재빌드

이제 NeoGraph는 모든 공개 바이너리 라이브러리에 프로젝트 `VERSION`과 주
버전 `SOVERSION`을 설정합니다. v1 이전 릴리스는 모두 ABI 세대 0을 쓰지만,
`0.x` 사이의 바이너리 호환성을 보장하지는 않습니다. 변경 기록에서 경계를
공지한 릴리스로 올릴 때는 모든 C++ 프로그램을 다시 빌드해야 합니다. 특히
`0.11.1` 이하에서 bounded `NodeCache`가 들어간 릴리스로 올릴 때는
`NodeCache`와 `EngineConfig` 객체 배치가 바뀌었으므로 재빌드가 필수입니다.

앞의 Provider 이전은 기존 `Provider` vtable을 바꾸지 않습니다. Provider
바이너리는 릴리스 전체에 공지된 재빌드 경계만 따르면 됩니다. 앞으로 진행할
`CheckpointStore` 비동기 이전도 같은 정책을 따라야 합니다. v1 전 vtable
변경은 재빌드 경계를 공지해야 하고, v1 뒤에는 안정된 객체 배치를 바꾸지 말고
별도 기능 인터페이스와 어댑터를 더하는 방식을 우선합니다.

플랫폼별 라이브러리 이름, 알려진 재빌드 경계, CI 검증 방법은
[바이너리 호환성 정책](ABI_POLICY.md)을 참고하세요.
