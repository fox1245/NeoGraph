<!-- neograph-i18n: source=docs/migration-v0.4-to-v1.0.md locale=ko source_sha256=9cf72b62901f60584777cbb139275d7fd8c1449f797cd9b7d68b2e1b05be1e56 -->
# 마이그레이션 가이드: 기존 가상 함수 8개 → `run(NodeInput)` (v0.4.x → v0.9+)

**Languages:** [English](migration-v0.4-to-v1.0.md) | [한국어](migration-v0.4-to-v1.0.ko.md) | [日本語](migration-v0.4-to-v1.0.ja.md) | [简体中文](migration-v0.4-to-v1.0.zh-CN.md)

NeoGraph v0.4는 노드 진입점을 `run(NodeInput) -> awaitable<NodeOutput>` 하나로 통합했습니다. 기존 가상 함수 8개(`execute` / `execute_async` / `execute_stream` / `execute_stream_async`와 각각의 `_full` 짝)는 v0.4.x에서 deprecated되었고, v1 준비 릴리스인 v0.9.0에서 제거되었습니다. 이 문서는 기존 노드를 현재 API로 옮기는 절차를 설명합니다.

> v0.9.0 이후에는 `run(NodeInput)`을 구현하지 않은 C++ 서브클래스가 추상 클래스라서 컴파일되지 않습니다. Python 서브클래스도 `run(self, input)`을 구현해야 합니다.

이벤트 기반 Provider dispatch는 v1 이전에 반드시 다시 빌드해야 하는 경계를 하나 더 만듭니다. `CancelToken`이 `std::stop_source`를 사용하게 되었고, executor에 묶이지 않는 네이티브 구독을 위해 `stop_token()`을 제공합니다. 기존 `cancel()`, `is_cancelled()`, `fork()`, `bind_executor()`, `slot()` 호출부는 소스 수준에서 그대로 호환되지만, 이전의 inline 구현은 바이너리 호환이 아닙니다. 모든 C++ 소비자와 확장을 NeoGraph의 헤더·라이브러리와 맞춰 다시 컴파일해야 하며, 공유 라이브러리만 바꾸는 것으로는 충분하지 않습니다. Provider 알림은 SDK의 `join()`, FIFO 순서의 소유 결과(owned outcome), 절대 데드라인, 승인(admission)·권한 예산을 그대로 유지합니다. [ABI 정책](ABI_POLICY.md)과 [동일 조건 측정 결과](../benchmarks/provider-notification-summary.json)를 참고하세요.

## 왜 마이그레이션하는가

기존 방식은 `(sync/async) × (writes/full) × (stream/non-stream)`의 곱, 즉 가상 함수 8개였습니다. 그중 하나만 오버라이드하면 나머지 7개는 기본 체인으로 대체됩니다. 안전한 조합도 있지만 런타임에 문제가 생기는 조합도 있습니다. 예를 들어 동기 `execute_full`에 비동기 dispatch를 조합하면 `run_sync`가 중첩되면서 경쟁 상태가 생깁니다. 그래서 사용자가 어느 함수를 오버라이드해야 하는지도 분명하지 않았습니다.

새 방식은 `run(NodeInput) -> awaitable<NodeOutput>` 하나뿐이며, 이것만 오버라이드하면 됩니다. 동기냐 비동기냐는 호출하는 쪽이 신경 쓸 일입니다. 코루틴 안에서 `co_await`를 쓰든 평범한 동기 코드를 쓰든 자유입니다. Command와 Send는 `NodeOutput`에 들어 있으므로 가상 함수를 더 둘 필요가 없습니다. 스트리밍 콜백은 `NodeInput::stream_cb`(null일 수 있는 포인터)로 전달됩니다.

## 가상 함수 8개 → 새 `run()` 대응표

| 기존 가상 함수 | 변환 결과 |
|---|---|
| `execute(state)` | `NodeOutput out; out.writes = {...}; co_return out;` (동기 본문) |
| `execute_async(state)` | `co_await provider->invoke_async(std::move(request));` 같은 네이티브 비동기 |
| `execute_stream(state, cb)` | `if (in.stream_cb) (*in.stream_cb)(event); co_return NodeOutput{...};` |
| `execute_stream_async(state, cb)` | 위 코드 + 네이티브 비동기(`co_await ...`) |
| `execute_full(state)` | `NodeOutput out; out.writes=...; out.command=...; co_return out;` |
| `execute_full_async(state)` | 위 코드 + 네이티브 비동기 |
| `execute_full_stream(state, cb)` | `execute_full` + `in.stream_cb` 사용 |
| `execute_full_stream_async(state, cb)` | 위 코드 + 네이티브 비동기 |

핵심은 이렇습니다. **8가지 변형은 어떤 `NodeOutput` 필드를 채우는지, `in.stream_cb`를 쓰는지, `co_await`를 쓰는지의 조합으로 모두 표현할 수 있습니다.** 그래서 가상 함수는 하나만 남습니다.

### 가장 흔한 Python 마이그레이션

**기존 코드:**

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

Python의 `run`은 `async def`가 아니라 일반 `def`입니다. 스트리밍 실행에서 `input.stream_cb`는 이벤트를 받는 함수이고, 일반 실행에서는 `None`입니다.

## 사례별 변환 예제

### 사례 1 — 가장 단순한 동기 노드

**기존:**
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

**변경 후:**
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

달라진 점은 다음과 같습니다.
- `state` → `in.state`
- 반환 값을 `NodeOutput`(`writes` 필드)으로 감쌉니다
- 함수가 `asio::awaitable<NodeOutput>`을 반환하고 `co_return`으로 끝납니다

### 사례 2 — 비동기 LLM 노드 (`execute_async` 마이그레이션)

```cpp
#include <neograph/graph/node.h>
#include <neograph/graph/run_context.h>
#include <neograph/provider.h>
#include <neograph/runtime_interposition_consumer.h>

class ChatNode : public neograph::graph::GraphNode,
                 public neograph::RuntimeInterpositionConsumer {
    std::shared_ptr<neograph::Provider> provider_;
    std::string model_;
public:
    ChatNode(std::shared_ptr<neograph::Provider> provider, std::string model)
        : provider_(std::move(provider)), model_(std::move(model)) {}
    asio::awaitable<neograph::graph::NodeOutput>
    run(neograph::graph::NodeInput in) override {
        auto request = neograph::make_provider_request(
            *provider_, model_, in.state.get_provider_messages());
        request.cancel_token = in.ctx.cancel_token;
        request.options.deadline = in.ctx.deadline;
        auto result = co_await neograph::graph::observe_provider_result(
            in.ctx, invoke_provider(provider_, std::move(request), {}, {},
                neograph::graph::provider_call_broker(in.ctx),
                neograph::graph::make_provider_call_identity(in.ctx, get_name())));
        neograph::graph::record_usage(in.ctx, result);
        neograph::outcome_or_throw(result);
        neograph::graph::NodeOutput out;
        out.writes.push_back(neograph::graph::provider_messages_write(result));
        co_return out;
    }
    std::string get_name() const override { return "chat"; }
};
```

### 사례 3 — 스트리밍 노드 (`execute_stream` 마이그레이션)

공개 계약은 짝을 이루는 completion 가상 메서드가 아니라, 소유권이 있는 타입 요청의 준비(prepare)와 dispatch입니다. `ProviderRequest.payload`는 Chat, Messages, Responses, Gemini, Interactions 요청을 담는 SDK variant입니다. `ProviderMode::Collect` / `Stream`은 observer와 무관하게 전송 방식을 고릅니다. `on_event`는 빌려 온(borrowed) 타입 `sp::Event` 뷰를 받으므로, 콜백이 끝난 뒤에도 필요한 데이터는 복사해 두어야 합니다. 원시 JSON으로 덮어쓰거나 이식 가능한 투영으로 네이티브 상태를 가져오는 방식은 승인(admission)되지 않습니다.

```cpp
#include <neograph/graph/node.h>
#include <neograph/graph/run_context.h>
#include <neograph/provider.h>
#include <neograph/runtime_interposition_consumer.h>

class StreamingChatNode : public neograph::graph::GraphNode,
                 public neograph::RuntimeInterpositionConsumer {
    std::shared_ptr<neograph::Provider> provider_;
    std::string model_;
public:
    StreamingChatNode(std::shared_ptr<neograph::Provider> provider, std::string model)
        : provider_(std::move(provider)), model_(std::move(model)) {}
    asio::awaitable<neograph::graph::NodeOutput>
    run(neograph::graph::NodeInput in) override {
        auto request = neograph::make_provider_request(
            *provider_, model_, in.state.get_provider_messages(), {}, {},
            neograph::ProviderMode::Stream);
        request.on_event = [sink = in.stream_cb](const sp::Event& event) {
            if (!sink) return;
            const auto* delta = std::get_if<sp::PartDelta>(&event);
            if (delta && delta->payload.kind == sp::PartKind::Text &&
                delta->payload.channel == sp::DeltaChannel::Content)
                (*sink)({neograph::graph::GraphEvent::Type::LLM_TOKEN,
                         "chat", neograph::json(std::string(delta->payload.bytes))});
        };
        request.cancel_token = in.ctx.cancel_token;
        request.options.deadline = in.ctx.deadline;
        auto result = co_await neograph::graph::observe_provider_result(
            in.ctx, invoke_provider(provider_, std::move(request), {}, {},
                neograph::graph::provider_call_broker(in.ctx),
                neograph::graph::make_provider_call_identity(in.ctx, get_name())));
        neograph::graph::record_usage(in.ctx, result);
        neograph::outcome_or_throw(result);
        neograph::graph::NodeOutput out;
        out.writes.push_back(neograph::graph::provider_messages_write(result));
        co_return out;
    }
    std::string get_name() const override { return "chat"; }
};
```

### 사례 4 — Command / Send를 쓰는 노드 (`execute_full` 마이그레이션)

**기존:**
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

**변경 후:**
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

`NodeOutput`은 `NodeResult`의 별칭이므로 기존 `NodeResult` 코드도 그대로 컴파일됩니다.

## 흔한 실수

### `NodeInput in`은 값으로 받아야 합니다

```cpp
// ❌ Wrong — coroutine ref-param UAF, SEGV in pybind async path
asio::awaitable<NodeOutput> run(const NodeInput& in) override { ... }

// ✅ Correct
asio::awaitable<NodeOutput> run(NodeInput in) override { ... }
```

이유: 안전하려면 코루틴 프레임이 인자의 복사본을 가져야 합니다. 참조로 받으면 호출자의 스택 프레임이 사라진 뒤 `in.state`가 댕글링 참조가 됩니다. PR 2 작업 중에 실제로 발생한 버그입니다.

### cancel, store, stream_cb는 모두 `in.ctx`에서 얻습니다

기존 노드는 `state.run_cancel_token_` 같은 우회 경로로 취소 토큰을 받았습니다. v0.4부터는 `RunContext`가 정식 전달 경로입니다.

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

C++ 노드에서 쓸 수 있는 `in.ctx`의 필드는 `cancel_token`, `usage`, `thread_id`, `step`, `stream_mode`, `store`, `resume_value`, `deadline`, `trace_id`입니다. 마지막 두 개는 `RunMetadata`에 설정하며, 엔진이 중첩된 서브그래프까지 그대로 전달합니다. 체크포인트 라우팅은 엔진 내부 사항이라 `RunContext`의 공개 필드가 아닙니다. Python에서는 `trace_id`, `run_id`, `model_token_budget`, `has_deadline`, `deadline_remaining_ms`를 쓸 수 있습니다. C++ steady clock의 원시 데드라인 값은 의도적으로 불투명(opaque)하게 두었습니다.

### `_full` 가상 함수 마이그레이션 — `co_return out;` 한 줄로 마무리하기

기존 `execute_full` 사용자가 가장 흔히 헷갈리는 점은 "`NodeResult`는 옛 타입인데 `NodeOutput`을 반환해야 하나?"입니다. 둘은 같은 타입의 별칭이므로 `NodeOutput out; out.writes=...; out.command=...; out.sends=...; co_return out;`처럼 쓰면 됩니다.

## 마이그레이션하지 않으면 어떻게 되는가

v0.9.0 이후에는 기존 가상 함수 8개가 없습니다.

- C++에서 기존 `override`를 그대로 두면 `'execute' marked override but does not override` 같은 컴파일 오류가 납니다.
- `execute()`만 구현한 Python 노드는 `run(input)`을 구현하라는 `NotImplementedError`를 발생시킵니다.

기존 메서드 이름을 남겨 두는 식의 과도기 패턴은 쓰지 마세요. 엔진은 `run(NodeInput)`만 호출하므로 옛 본문은 절대 실행되지 않습니다.

## 일괄 마이그레이션 스크립트가 있는가

없습니다. 가상 함수 시그니처가 8가지 형태로 제각각이라 정규식으로 변환하기는 현실적이지 않습니다. 위의 사례 4개를 보고 직접 옮겨야 합니다.

가장 흔한 패턴(`execute(state)`만 오버라이드한 경우)이라면 아래 sed/awk 한 줄짜리 명령이 첫 작업에 도움이 될 수 있습니다. 결과는 반드시 사람이 검토해야 합니다.

```bash
# Very rough initial pass — nodes with single-line execute override only.
# Always dry-run without -i first.
grep -lE 'execute\(const GraphState' src/**/*.cpp
# Manually edit each resulting file to the new pattern.
```

복잡한 노드(`execute_full`, `execute_stream_async` 등)는 직접 고쳐야 합니다. 지름길은 없습니다.

---

# 마이그레이션 2: 타입이 있는 무손실 Provider 전환 (재컴파일 필수)

이 변경은 소스 호환과 바이너리 호환을 모두 깨뜨립니다. 모든 C++ 소비자와 사용자 정의 provider를 새 헤더·라이브러리에 맞춰 다시 컴파일해야 합니다. `CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, descriptor 인터프리터, Responses WebSocket 경로는 제거되었고 별칭이나 호환용 브리지도 없습니다. SDK는 알파 `0.1.1`이며 인터페이스 리비전 4 / 공유 ABI 4입니다. 기능 검사는 out-of-line으로 하며, 이것이 안정 릴리스라는 뜻은 아닙니다. 기록된 interface 3의 SDK 런타임/archive 자격 검증은 Linux/POSIX를 대상으로 했으며 interface 4를 검증한 것은 아닙니다. Windows NTFS와 macOS 구현은 있지만 새 플랫폼의 자격 검증에는 런타임 증거가 필요합니다. WASM에서 provider 런타임이 검증된 바는 없습니다.

제거된 `Provider::complete`, `complete_async`, `complete_stream`, `complete_stream_async` 호출부는 모드를 명시한 요청과 `invoke` / `dispatch`(또는 C++ 비동기 짝)로 바꿉니다. `Agent::complete`는 소유 결과를 반환하는 단일 턴 API로 별도로 남아 있습니다.

CMake 3.20 이상이 필요합니다. `NEOGRAPH_BUILD_LLM=OFF`로 빌드해도 Core가 소유권이 있는 타입 provider 계약을 export하므로 SchemaProvider 런타임은 여전히 필수입니다. `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`를 명시하면 그 경로를 최우선으로 씁니다. 지정하지 않으면 CMake는 먼저 설치된 `SchemaProvider` 런타임 패키지를 찾고, 없으면 `NEOGRAPH_FETCH_SCHEMAPROVIDER=ON`(기본값)일 때 `cmake/NeoGraphSchemaProvider.cmake`에 고정된 변경 불가능한 GitHub archive를 내려받습니다. 오프라인으로 빌드하려면 SDK를 설치하고 `CMAKE_PREFIX_PATH`에 그 설치 prefix를 지정한 뒤 `-DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`를 넘깁니다. CMake는 옆에 있는 체크아웃을 추측해서 쓰지 않고, 제거된 번들 인터프리터를 선택하지도 않습니다. NeoGraph의 선택 사항인 HTTP 모듈을 꺼 두어도 SDK 런타임에는 전송 계층 의존성이 필요합니다. 기록된 SDK 런타임/archive 자격 검증은 Linux/POSIX를 대상으로 했습니다. Windows NTFS와 macOS 구현은 있지만 새 플랫폼의 자격 검증에는 런타임 증거가 필요합니다. WASM에서 provider 런타임이 검증된 바는 없습니다.

Python도 C++와 같이 소유권이 있는 요청/결과 경계를 제공합니다. 이 경계는 `make_provider_request`, `Provider.prepare`, `dispatch`, `invoke`로 이루어집니다. provider 기록에는 타입이 있는 파트를 가진 `ProviderMessage`를 사용해야 합니다. `ChatMessage`는 그래프용 편의 투영으로 남아 있습니다. 반환된 SDK 실패는 `ProviderOutcome.failure`로 확인합니다. 호스트의 observer나 정산(settlement)에서 예외가 나면 그 예외가 `outcome`과 `cause`를 유지합니다. 생성자와 GIL/콜백 동작은 [Python 바인딩 가이드](python-binding.md)를 참고하세요.

interface 4 이전에는 설치된 find_package로 빌드한 Program C++/C ABI/dualQuickJS 소비자와, NeoGraph/SchemaProvider의 타입 기반 2요청 수명/네이티브/raw/불일치 소비자가 통과했습니다. 이는 과거 패키지의 결과이지 interface 4가 통과했다는 주장이 아닙니다. 더 넓은 플랫폼이나 안정 릴리스도 주장하지 않습니다.

공개 계약은 짝을 이루는 completion 가상 메서드가 아니라, 소유권이 있는 타입 요청의 준비(prepare)와 dispatch입니다. `ProviderRequest.payload`는 Chat, Messages, Responses, Gemini, Interactions 요청을 담는 SDK variant입니다. `ProviderMode::Collect` / `Stream`은 observer와 무관하게 전송 방식을 고릅니다. `on_event`는 빌려 온(borrowed) 타입 `sp::Event` 뷰를 받으므로, 콜백이 끝난 뒤에도 필요한 데이터는 복사해 두어야 합니다. 원시 JSON으로 덮어쓰거나 이식 가능한 투영으로 네이티브 상태를 가져오는 방식은 승인(admission)되지 않습니다.

```cpp
#include <neograph/provider.h>
#include <neograph/runtime_interposition_consumer.h>
#include <neograph/controlled_provider.h>

// Selected public declarations from neograph::Provider.
class Provider {
public:
    virtual ~Provider() = default;
    virtual std::string get_name() const = 0;
    virtual std::string_view family() const noexcept = 0;
    virtual PreparedProviderRequest prepare(ProviderRequest request) = 0;
    sp::runtime::Result dispatch(PreparedProviderRequest request);
    asio::awaitable<sp::runtime::Result> dispatch_async(PreparedProviderRequest request);
    sp::runtime::Result invoke(ProviderRequest request);
    asio::awaitable<sp::runtime::Result> invoke_async(ProviderRequest request);
    static std::string request_digest(const PreparedProviderRequest& request);
    static std::optional<std::uint64_t> conservative_token_upper_bound(
        const PreparedProviderRequest& request);
};
```

### ProviderRequest / ProviderControls

```cpp
#include <neograph/llm/schema_provider.h>
#include <neograph/types.h>

sp::runtime::Result call_provider(
    neograph::Provider& provider, std::string model,
    std::vector<sp::Message> history,
    std::function<void(const sp::Event&)> observer) {
    neograph::ProviderControls controls;
    controls.max_output_tokens = 128;  // optional caller-selected wire cap
    auto request = neograph::make_provider_request(
        provider, std::move(model), std::move(history), {},
        std::move(controls), neograph::ProviderMode::Stream);
    request.on_event = std::move(observer);
    auto prepared = provider.prepare(std::move(request));
    return provider.dispatch(std::move(prepared));  // owns Completion or Failure
}
```

interface 4는 유지된 호출별 제어 항목을 `extra_fields` 딕셔너리가 아니라 타입이 있는 필드로 제공합니다.

| 계열 | 추가 `ProviderControls` |
|---|---|
| Chat | `chat_reasoning`, `include_reasoning`, `usage_include`, `models`. 선언된 OpenRouter origin이 있어야 함. 스칼라 `reasoning_effort`는 별도 항목. |
| Responses | `previous_response_id`, `previous_response_history`, `parallel_tool_calls`, `verbosity`, `truncation`, `responses_include`. |
| Messages | `thinking_mode`, `output_effort`, `cache_control`, `messages_tool_choice`, origin을 선언한 OpenRouter `provider` 라우팅. |
| Gemini Generate | `gemini_history_mode`, `gemini_thinking_level`, `temperature`, `safety_settings`, `gemini_tool_choice`. |

Messages에서 manual thinking을 쓰려면 정책에서 승인(admission)된, 출력 상한보다 작은 예산이 필요합니다. adaptive와 disabled 모드에서는 thinking 예산을 지정할 수 없습니다. manual/adaptive thinking에서는 유효한 temperature를 요청에서 뺍니다. 다만 유효하지 않은 값이나 모델이 금지한 temperature는 값을 빼기 전에 거부합니다. disabled thinking에서는 승인(admission)된 temperature를 그대로 보냅니다. 모델 접두사 제한은 승인(admission)된 정책상의 사실이며, 전체 모델 이름 또는 마지막 `/` 뒤의 접미사와 비교합니다. 이때 ASCII 대소문자를 구분하지 않습니다. Gemini의 thinking level과 thinking budget은 함께 쓸 수 없고, 타입이 있는 tool choice와 `required_tool`도 함께 쓸 수 없습니다. 지원하지 않는 계열/origin/값의 조합은 I/O 전에 거부합니다.

### PreparedProviderRequest / ProviderBudgetClaim
`prepare()`는 검증과 인코딩을 정확히 한 번만 수행하고, 원래의 데드라인과 취소 상태를 가진 이동 전용 `PreparedProviderRequest`를 만듭니다. 영속(durable) 호출자는 이 핸들의 `Provider::request_digest()`를 자신의 조립 정보에 묶고, 승인(admission)된 예산 claim을 예약하고, dispatch receipt를 기록한 다음, 같은 핸들을 `ControlledProvider::dispatch_prepared(_async)`로 소비합니다. 게이트를 지난 뒤에는 요청을 다시 만들지 않습니다. 중복된 receipt로는 다시 dispatch하지 않습니다. 사용자 정의 provider는 `prepare_runtime()` 또는 `prepare_local()`을 이용해 `get_name()`, `family()`, `prepare()`를 구현합니다. 로컬 콜백은 `this`가 아니라 소유권이 있는 공유 상태를 캡처해야 합니다.

선택 사항인 `ProviderControls`는 호출자가 고르는 값이며, 필수 기본값도 아니고 몰래 잘라 내는 상한도 아닙니다. 계열이 지원하지 않는 제어 항목은 dispatch 전에 실패합니다. 한도가 있는 호출에는 승인(admission)된 실제 모델 입력/출력 사실이 필요하며, 그런 사실이 없으면 `LimitUnknown`으로 실패합니다. 예약(reservation)은 보수적으로 잡은 지출 권한이지 보고된 사용량도, 예측도, 청구서도 아닙니다. 결과가 알 수 없음, 부분, 전달 여부 불명인 경우에는 hold가 유지됩니다. 실제 최종 보고서가 오면 사용량이 지나치게 큰 경우까지 포함해 hold를 정산합니다. 재시도는 명시적인 단일 계층이며 기본값은 꺼짐입니다. 재시도에는 제한된 시간 창과, 이전 결과를 알 수 없을 때 유지하는 hold가 있고, 숨겨진 재전송은 없습니다.


provider 호출은 `sp::runtime::Result`를 반환합니다. 이는 `sp::Completion` 또는 `sp::Failure`를 담은, 바뀌지 않고 소유권이 있는 `std::shared_ptr<const sp::Outcome>`입니다. 화면에 표시할 텍스트만 챙기지 말고 결과 전체를 보관해야 합니다. 순서가 있는 메시지/파트, 유지되는 네이티브 continuation과 계열이 제공한 wire 증거, 순서가 있는 원시 관찰값, stop 증거, 실제 시도 메타데이터는 호출이 끝나고 클라이언트가 소멸한 뒤에도 남습니다. `input_total`, `output_total`, `total` 같은 사용량 카운터는 `std::optional<sp::Count>`이며, 값이 있는 카운터는 `uint64_t value`와 `Evidence`를 가집니다. `Usage`에는 단계(stage), 품질(quality), 충돌(conflicts)도 기록됩니다. 값이 없으면 알 수 없다는 뜻이며, 0으로 채우는 일은 없습니다. 실패한 경우에도 원래의 부분 결과가 보존됩니다. `ProviderFailure::outcome()`과 `ProviderObserverError::outcome()`이 그 결과를 돌려주고, 후자는 observer 예외도 `cause()`에 보존합니다.

wire 증거는 계열이 제공하는 선택 사항입니다. `sp::Completion::wire_envelope`는 null일 수 있습니다(Python에서는 `ProviderCompletion.wire_envelope`가 `None`입니다). 버퍼링 방식 Chat은 현재 전체 응답 JSON을 `raw_events`에 `type == "chat.completion"`인 `RawWire`로 보관하며, 문서는 `payload`에 들어 있고 `wire_envelope`는 null로 둡니다. 증거는 계열이 보관하는 위치에서 읽어야 하며, 대체 envelope를 만들어 내지 않습니다. 네이티브 continuation과 raw 버퍼는 보호되는 증거로 남으며 트레이스 payload에서는 제외됩니다.

`UsageAccumulator::snapshot()`은 누적된 보고서를 반환합니다. `total_tokens_wide()`는 청구된 토큰에 아직 해소되지 않은 예약을 더한 값을 반환하므로, 보고된 사용량으로 표시하면 안 됩니다. 정산(settlement)에는 입력·출력 카운트를 갖춘 최종 보고서가 일관되게 있어야 하며, 지원되는 총계 중 가장 큰 값을 청구하고 너무 큰 사용량도 상한으로 잘라 내지 않습니다. 누적된 보고서 중 하나라도 카운터가 없으면 집계 값에서도 알 수 없는 값으로 남습니다. 예약, 로컬 청구, 벤더 청구서는 서로 다른 기록입니다.

실제 결과가 나온 뒤 효과 후 회계(post-effect accounting)나 최종 receipt 저장에 실패하면, `ProviderDispatchOutcomePersistenceError`가 원래의 변경 불가능한 결과를 `outcome()`에, 원래의 저장 예외를 `cause()`에 보존합니다. 전달(delivery)도 실패했다면 `delivery_error()`가 원래의 observer 예외를 보존합니다. 저장은 성공했지만 그 뒤 observer가 실패하면 원래의 observer 예외를 그대로 다시 던집니다. 결과가 없거나 알 수 없는 전송 실패에서는 결과를 만들어 내지 않습니다.

### SchemaProvider

`SchemaProvider`는 승인(admission)된 `sp::descriptor::ValidatedDescriptor`, `sp::runtime::Options`, 선택 사항인 `SchemaProvider::Defaults`를 받습니다. descriptor를 불러오는 일은 닫힌 형식의 버전이 있는 데이터를 승인(admission)하는 과정이며, 요청/응답 인터프리터나 임의 primitive 레지스트리가 아닙니다. 자격 증명은 공개 descriptor 파일이 아니라 런타임 옵션에 넣어야 합니다. Defaults에는 타입이 있는 OpenRouter 라우팅과 Responses 보존(`responses_store`)만 들어가며, 후자는 Responses에서만 유효합니다. 호스팅되는 OpenRouter의 라우팅, 보존, JSON 형식은 계속 선언된 타입 제어 항목입니다. Images, Veo, Decisions는 NeoGraph의 별도 타입 클라이언트와 별도 권한을 사용하며, SDK chat grant를 물려받지 않습니다.

```cpp
#include <neograph/llm/schema_provider.h>
#include <descriptor/descriptor.h>
#include <stdexcept>
#include <variant>

std::shared_ptr<neograph::llm::SchemaProvider> admitted_provider(
    std::string_view descriptor_json, std::string api_key) {
    auto loaded = sp::descriptor::load(descriptor_json);
    if (const auto* error = std::get_if<sp::descriptor::ConfigError>(&loaded))
        throw std::invalid_argument(error->message);
    sp::runtime::Options options;
    options.api_key = std::move(api_key);
    neograph::llm::SchemaProvider::Defaults defaults;
    return std::make_shared<neograph::llm::SchemaProvider>(
        std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded)),
        std::move(options), std::move(defaults));
}
```

일반 `sp::descriptor::load`는 헤더를 리터럴로 취급하므로 `${VAR}`를 확장하지 않습니다. 승인(admission) 전에 호스트 쪽에서 명시적으로 전처리하려면 `sp::descriptor::load_with_environment_headers(source, overrides, policy)`를 쓰거나, 결정적인 `load_with_deployment_headers(source, overrides, environment, policy)`를 `DeploymentHeaderEnvironment`와 함께 씁니다. 환경 헬퍼는 Messages용으로 선택 사항인 `ANTHROPIC_WORKSPACE_ID` / `ANTHROPIC_BETA`를 읽으며, 설정되지 않았거나 비어 있는 값은 건너뜁니다. descriptor에 리터럴로 적은 헤더는 환경 값보다 우선하고, 명시적 override는 그 둘보다 우선합니다. 이때 대소문자를 구분하지 않습니다. 중복된 override, 유효하지 않거나 예약된 헤더는 최종 승인(admission)에서 실패합니다. 어떤 인코더도 템플릿을 평가하거나 승인(admission)된 descriptor를 수정하지 않습니다.

### 네이티브 기록과 예산

`ChatMessage` / `ChatTool`과 JSON은 이식 가능한 투영(portable projection)일 뿐 네이티브 권한(native authority)이 아닙니다. 이식 가능한 형식은 계속 [`provider-message-v2`](../schemas/provider-message-v2.schema.json)와 [`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json)입니다. 실제 C++ 체크포인트 사이드카는 네이티브 봉인(native seal)을 메모리에 보관합니다. 네이티브 기록을 영구 저장하려면 호스트가 소유하는 `sp::NativeArchive`가 필요합니다. 닫힌 형식인 v3 / `spna3`이며, 인증된 소유자 전용 보관(custody)과 독립된 키를 갖춰야 합니다. archive v2는 업그레이드하거나 해석하지 않고 거부합니다. 인증은 의미를 정하는 descriptor의 모든 선택(origin/경로/헤더, 정책, 요청 필드 매핑, 사용량 경로와 stop 매핑), 소유자, 정확한 보관 바인딩을 묶어서 검증합니다. 이는 암호화도 아니고 벤더 발급자 인증도 아닙니다. archive 본문, 키, 네이티브 blob, 원시 wire 관찰값은 절대 공개해서는 안 됩니다. archive는 증거를 저장하는 곳이지 비용 승인(money grant)이나 지출 임대(spending lease)가 아닙니다. Program과 외부 예산(bank)은 각자 저널이 따로 소유하며, 스냅샷을 복사해도 크레딧이 생기지 않습니다.

interface 4는 네이티브 재생(replay)의 설정 digest에서 출력 생성 상한만 뺍니다. 콘텐츠, origin, route, 정책 식별자, prefix, 도구, 추론 제어는 계속 바인딩됩니다. 실제 상한은 여전히 인코딩된 요청과 준비된 요청의 digest에 들어갑니다. 상한을 더 크게 잡은 의미상 새 호출마다 리소스 bank의 새로운 승인(admission), 그 호출만의 결정적인 호출 서수/효과 식별자, 원래의 데드라인이 필요합니다. 이미 정산된 호출 슬롯을 재사용하거나, 크레딧을 갱신하거나, 봉인을 복구하거나, 결과가 불확실한 효과를 다시 보낼 수는 없습니다. archive v3와 이식 가능한 JSON v2는 바뀌지 않습니다. 이전 정책으로 만든 네이티브 archive는 그 정책에 계속 묶여 있으며, 정책이 맞지 않으면 마이그레이션되지 않고 거부됩니다.

같은 경로(route)에서 이어 가는 네이티브 continuation과 명시적인 foreign 투영은 계약이 서로 다릅니다. Gemini의 기본값은 `sp::gemini::HistoryMode::NativeOnly`입니다. `PortableForeign`은 호출자가 만든 assistant Text/ToolCall 기록을 받아들이며, 이때는 네이티브 봉인, wire 출력, 서명이 없습니다. foreign assistant 턴마다 첫 번째 함수 호출에만 `skip_thought_signature_validator`를 붙이고, 텍스트만 있는 기록에는 서명을 붙이지 않습니다. 실제 네이티브 그룹은 여전히 엄격한 검증을 받으며, 실패했거나 맞지 않는 봉인을 떼어 내거나 이식 가능한 기록으로 격하하는 일은 없습니다. Responses의 `previous_response_id`는 provider가 보관한 상태를 고르며, `messages`에는 새 입력만 보냅니다. `previous_response_history`는 로컬에서 소유권을 확인하는 증거일 뿐 요청으로 내보내지 않습니다. 클라이언트 도구의 소유권 확인에 필요하다면 원래 prefix 전체와, ID가 커서와 같은 실제 최종 assistant 메시지를 제공해야 합니다. 이후 같은 프로세스에서 나오는 커서 결과는 비공개로 완료된 소유권만 유지하며, 전체 `NativeReplay`나 archive 권한은 유지하지 않습니다. 커서는 네이티브 archive도 아니고 이식 가능한 가져오기 grant도 아닙니다.

**독립형 bank 저널 정정 — 현재 계약을 수정했으며, 실제로 실행한 런타임 증거는 아래에 있습니다.** 소유자가 승인한 프로토콜은 다음을 요구합니다. 신뢰할 수 있는 저장소의 네임스페이스에 대해 단조 증가하는 의무(obligation)가 있어야 하고, 실제로 변경할 수 없는 원본 소유자/스레드/그래프 범위, 상한(ceiling), 데드라인/시계 식별자, 세대(generation)가 있어야 합니다. 체크포인트 전체 커밋먼트와 리비전에 대한 정확한 영속 head CAS만이 호스트 소유의 불투명한 lease를 발급할 수 있습니다. 대기 중인 효과의 정확한 구간(pending effect window)은 provider I/O 이전에 영속화되어야 합니다. 정산(settlement)에는 실제 SDK 결과와 실제 청구액, null 가능한 보고서, hold, 중복 제거 식별자를 써야 합니다. 체크포인트와 다음 head는 같은 소유 actor/리비전 아래서 원자적으로 게시되어야 합니다. bank 메타데이터를 지우거나, 체크포인트를 정리(prune)하거나, 인증된 예전 스냅샷을 재생(replay)하거나, 같은 ID를 덮어쓰거나, actor를 잃어도 크레딧이 생겨서는 안 됩니다. 이미 hold 65가 있는 상태에서 상한을 130에서 129로 줄이면 65를 추가로 승인(admission)할 수 없습니다. 효과가 없었다고 증명된 실패는 바뀌지 않은 head를 해제할 수 있으며, 그러면 실제 130 기준의 복구는 계속 진행할 수 있습니다. 크래시, 상태 불명, lease 유실로 생긴 구간은 환불, 재시도, 대체(fallback) 없이 hold 상태로 남습니다. 일반적이거나 처음 상태 그대로인 archive 설정은 금액이나 네이티브 지출 lease를 부여하지 않으며, 현재의 `config.usage`는 이미 있는 독립형 의무를 대체할 수 없습니다. Program/외부 bank 저널의 소유권은 바뀌지 않습니다. 여기에 적은 내용은 요구되는 계약이며, 실제 통화/보관(custody) 증거와 계측의 한계는 아래에 보고합니다. 안정 릴리스된 API를 보장한다는 뜻이 아닙니다.

**현재 선언 내용. 통합 런타임 증거는 아래에 있습니다.** `<neograph/graph/checkpoint.h>`는 `ManagedBudgetLeaseScope`를 선언하며, 이 타입은 `owner_scope`, 논리적 `thread_id`, 비공개 백엔드용 `storage_thread_id`, `graph_identity`, `original_ceiling`, `original_deadline_ticks`, `deadline_clock_identity`를 가집니다. `OwnedManagedBudgetLease`는 읽기 전용인 `scope()`, `actor_id()`, 변경할 수 없는 `bank_generation()`, `revision()`, `head_checkpoint_id()`, `head_commitment()`를 제공하며, 권한을 외부에서 가져오는 공개 생성자는 없습니다. `ManagedBudgetEffectReceipt`는 `active()`, `effect_id()`, `claim_amount()`, `request_digest()`를 제공하고, 기본 생성된 receipt는 아무 권한도 부여하지 않습니다. `CheckpointStore`는 `acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`, `begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`, `settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`, `publish_managed_budget_checkpoint(lease, checkpoint)`, `release_managed_budget_lease(lease)`를 선언하며 각각 `_async` 짝이 있습니다. 동기 `CheckpointStoreCore`와 `AsyncCheckpointStore`는 각자에 맞는 변형을 제공합니다. `managed_budget_checkpoint_commitment(checkpoint)`는 bank JSON만이 아니라 영속 체크포인트 전체를 대상으로 합니다. 이 선언만으로는 백엔드 CAS, 통화 안전성, 설치된 ABI 호환성, 실제로 실행해 본 런타임 경로가 보장되지 않습니다.

**실제 InMemory 공유 bank fork를 유지하고 실행해 확인했습니다.** 실제 C++ fork는 복제된 grant가 아니라 원본 금융 저널 하나와 신뢰할 수 있는 현재 브랜치 head를 사용합니다. `publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)`(와 `_async`)는 인증된 현재 원본/전체 커밋먼트와, 같은 bank를 가리키는 실제 네이티브 C++ 포인터를 요구합니다. 영속 독립형 fork는 명시적으로 지원하지 않습니다. `OwnedManagedBudgetLease::scope()`와 원본 소유자/스레드/그래프, 상한, 데드라인/시계, 세대는 변경할 수 없습니다. 저장소가 발급하는 읽기 전용 `execution_thread_id()` / `execution_storage_thread_id()`는 실행 브랜치를 따로 선택하고, `GraphState::budget_original_thread_id()`는 원본 금융 bank를 식별합니다. 선택된 브랜치 head에 대한 정확한 CAS와 전역 actor/리비전이 모든 브랜치를 정규 현재 카운터, 대기 중인 효과, 소진된(burned) 식별자에 맞춰 직렬화합니다. 원본 브랜치와 fork 브랜치는 보충 없이도 계속 쓸 수 있습니다. 오래된 스냅샷, 복사한 체크포인트, 가져온 JSON으로는 별칭을 만들거나 head를 되돌릴 수 없습니다. 원본 root30 → charge3 → 원본 continuation6 → fork lower20 → continuation9로 이어지는 같은 bank 증명은 바뀌지 않은 test_graph_engine.cpp:810–913에서 통과했습니다. 저장된 원본 상한 30은 fork의 유효 상한 20과 별개이며, 31로 넓히는 시도와 JSON만으로 하는 복원은 거부되어야 합니다. 상한이 없는 보고 관찰값은 사실 데이터일 뿐 유한한 grant가 아닙니다. 효과가 없었다고 증명된 lease만 바뀌지 않은 head를 해제할 수 있고, 알 수 없거나 대기 중인 효과는 의무를 그대로 유지합니다.

**현재 release 오류 계약. 실행한 테스트와 프로브는 아래에 있습니다.** `<neograph/graph/engine.h>`의 `graph::ManagedBudgetLeaseReleaseError`는 `ProviderOutcomeError`에서 파생됩니다. `cause()`는 원래의 실행 예외를 보존하고, `release_error()`는 이차적으로 발생한 영속 lease 처리 실패를 노출합니다. `outcome()`은 실제 SDK 증거가 있으면 그 증거를 유지하고, SDK 결과가 없으면 null입니다. release에 실패해도 결과를 지어내거나 재디스패치를 허용하지 않습니다. 닫힌 형식의 `_neograph_managed_budget_scope` 메타데이터는 원본 논리 범위/상한/데드라인 시계/세대를 기술하지만 데이터일 뿐 백엔드 CAS 권한이 아닙니다.

**Archive 소유자/보존 계약. 실행한 테스트와 프로브는 아래에 있습니다.** 유한한 독립형 루트와 인증된 유한 원본만이 생략된 원본 소유자를 실제로 설정된 `sp::NativeArchive::owner_scope()`에서 물려받습니다. 상한이 없거나 일반적인 소유자 메타데이터의 의미는 바뀌지 않았습니다. archive 소유자가 명시적으로 충돌하면 lease를 얻기 전에 거부합니다. `CheckpointStore::retains_native_checkpoint() const noexcept`와 이에 대응하는 Core/Async 저장소 기능은 기본값이 false이고, 실제 InMemory 백엔드는 true로 재정의합니다. 래퍼는 실제 보존 여부를 위임해야 합니다. 이 읽기 전용 설명은 lease가 없거나 일반적이거나 상한이 없는 C++ 네이티브 체크포인트를 정당하게 보관할 수 있게 합니다. 다만 지출 크레딧이나 네이티브 재생(replay) 권한은 부여하지 않습니다. lease가 있는 보관은 JSON 플래그나 추측한 저장소 타입이 아니라 저장소가 실제로 발급한 receipt를 사용합니다.

**네이티브 보관 사전 I/O 게이트. 실행한 테스트와 프로브는 아래에 있습니다.** 관리되는 효과(managed effect)를 시작하려면, 대기 효과·슬롯·보류 구간(held window)을 변경하기 전에 실제로 바인딩된 NativeArchive 또는 로컬 저장소가 실제로 발급한 비공개 C++ 보존 기능(retention capability)이 필요합니다. 이 비공개 기능은 JSON에서 가져오거나 wire로 전달하지 않습니다. 원격 백엔드가 InMemory여도 gRPC에서는 클라이언트와 서버 모두에 실제 archive가 필요합니다. C++ 사이드카는 그 경계를 넘을 수 없기 때문입니다. archive가 유한한 원본 소유자를 제공하지 않으면 원본의 익명 소유자 범위는 비어 있는 채로 남고, 실제 archive를 바인딩할 때는 원본 범위와 일치해야 합니다. 금융 head/lease 증거만으로는 네이티브 보관 준비가 되었다는 것을 증명하지 못합니다.

진단용 JSON은 원래의 raw 바이트를 그대로 보존하며, 문법상 유효한 중복 키 문서도 포함합니다. 다만 실행 가능한 요청/설정을 승인(admission)할 때는 여전히 중복 키를 거부합니다. 2xx가 아닌 원본 응답 JSON은 손실이 생기는 두 번째 파싱 없이 `http.error` 증거에 그대로 남습니다. 이름이 붙은 SSE 오류는 나중에 정상적으로 스트림이 닫히는 것보다 우선합니다. 진단/provider 메타데이터는 승인(admission)된 원본의 범위로 제한되며, 이와 무관한 작은 오류 텍스트 상한으로 제한되지 않습니다.

`ProviderRequest::observer_limits`는 호스트 전용입니다. 명시적으로 지정하는 `max_events`와 `max_bytes`는 양수여야 하고, 승인(admission)된 SDK 전달 상한을 낮추는 용도로만 쓸 수 있습니다. `provider-request/v3`는 실제 적용되는 한도, 모드, 인코딩된 본문, 재시도 정책, 의미를 정하는 descriptor 바인딩 전체를 digest에 포함합니다. 브리지는 대기 중인 batch와 비우는 중인 batch 양쪽에 대해 실제 PMR vector/map 용량에 소유한 이벤트/문서 바이트를 더해 계상하며, 취소는 큐 mutex 바깥에서 요청합니다. 이름이 `messages`인 일반 채널을 chat으로 강제 변환하지는 않습니다. 네이티브 `history` 채널을 `messages`로 매핑하면 JSON에서 네이티브 권한을 만들어 내지 않고 C++ 사이드카를 그대로 보존합니다.

`ProviderOutcomeError`는 결과를 보존하는 호스트 오류의 공통 기본 클래스입니다. `ProviderObserverError`와 `ProviderDispatchOutcomePersistenceError`는 모두 비워 낸 SDK 결과 전체와 원래의 `cause()`를 보존합니다. 저장 오류는 부차적으로 발생한 observer 실패도 `delivery_error()`에 보존합니다. `ProviderFailure::outcome()`은 SDK 실패 자체를 보존합니다. 이것들은 증거일 뿐 Node나 Program이 다시 dispatch해도 된다는 허가가 아닙니다. 전송 재시도는 SDK가 전담하며, 호출자가 정한 `max_output_tokens`를 몰래 잘라 내는 일은 없습니다.

`ProgramFailure`는 살아 있는 `provider_outcome`과 `provider_cause`를 보관합니다. 사실을 기록한 정규 SDK 증인(witness)은 실제 archive 보관을 소유자/run/버전/번들/연산/시도에 묶습니다. Runtime은 복구한 실패를 노출하기 전에 설정된 보관을 먼저 복원합니다. 데이터만 담는 공개 `ProgramResult::create()`는 미리 채운 증인으로 이 과정을 우회할 수 없으며, 해석되지 않은 봉인은 실행 가능한 결과가 아닙니다. 프로세스를 재시작하면 원래의 예외 포인터는 사용할 수 없고(`provider_cause == nullptr`), 텍스트로 다시 만들지도 않습니다. 영속화할 수 없는 실패는 직렬화하거나 게시하거나 재생(replay)할 수 없습니다.

`RecordedBindingSet`은 원본에 묶여 있고 이동만 가능한 데이터이며, 호출자가 제공하는 dispatcher가 아닙니다. 신뢰할 수 있는 Catalog의 `recorded_capability_binder`가 실제로 저장된 원본 이벤트로부터 캡처된 기능(captured-only capabilities)만을 독립적으로 구성합니다. `ProgramRuntime::replay_recorded()`는 원래 선택된 원본의 권한을 확인한 다음, 실제로 남은 bank를 영속 CAS로 넘겨줍니다. 물려받은 지출은 새로운 모델 grant가 아닙니다. 예전의 `start_recorded` 갱신 API는 제거되었습니다. InMemory, File, SQLite, PostgreSQL Program 저장소는 실행하는 동안 변경할 수 없는 소유 lease를 그대로 보존하며, 만료되어도 갱신되지 않습니다. 제어되는 JavaScript도 기반이 되는 capability manifest를 계속 검증하고, 이미 완료된 command 결과를 그대로 사용하며 외부 효과를 다시 디스패치하지 않습니다.

**전체 테스트 스위트에서 실행해 확인한 Recorded-control 인과 수정.** 캡처된 command의 재생(replay)은 실행 전에 새로 필요한 CPU wall-time과 Core 작업분만 영속적으로 예약하고, 실행 뒤 측정된 작업량과 새로 만들어진 Core 체크포인트를 결과 CAS로 게시합니다. 새로운 모델, 금액, Program 연산 허용량은 소비하지 않으며 캡처된 외부 효과를 다시 디스패치하지도 않습니다. 조정(reconcile)되지 않은 예약은 차감된 채로 남습니다. 예약은 일반적인 Running→Running 전이가 아니라 인증된 정산(settlement) 전이를 선택합니다. 일반 전이는 처음 만들어지는 새 Core 체크포인트를 거부했습니다. await 채널 수신, 타이머 대기/취소, handoff 대기 시작/해제는 각각을 소유한 executor/strand에서 직렬화됩니다. 기존 Recorded CPU/Memory의 await/handoff 시나리오는 전체 스위트에서 통과했고, 원격 TSan 커버리지의 한계는 아래에 따로 적었습니다.

아래 관찰 기록은 이 문서를 정리하기 전에 작성했습니다. 과거의 증거이며 새로 실행한 테스트가 아니고, 모든 플랫폼·전송 방식·보안 속성에 대한 보증도 아닙니다.

**비용이 든 관찰 완료. 보편적인 자격 검증은 아님.** 원래의 `SPQUAL1`의 기본 한도 630/1000000 microUSD는 바뀌지 않았습니다. 해시로 연결된 `A` 하나가 같은 원본 원장 안에서 승인된 확장분 480/3000000을 승인(admission)해 합계가 1110/4000000이 되었고, 호출/지출/hold/정산(settlement)은 누적되며 새 grant ID, 헤더, 리셋은 없습니다. 선언 바이트와 파일 식별자 자체, 원래의 authorization/baseline/catalog/activation/원장 prefix의 해시와 합계는 그대로 고정되어 있으며, 제거·교체·변경은 실패 시 차단으로 처리합니다. 최종 정규 원장은 calls1110/spent437958/held1287828 microUSD, eventA1, limits1110/4000000입니다. spent+held는 US$1.725786이며 이는 로컬 카탈로그 미터 값이지 청구서가 아닙니다. 문서화된 5개 계열 × 60쌍의 기준 측정은 600건의 요청을 완료했습니다. Chat 60/60, Responses 60/60, Messages 60/60, Generate 56/60(비전 오답 SSE 4건), Interactions 57/60(비전 오답: 버퍼 1건, SSE 2건)이며 합계는 300쌍 중 293쌍이지 300/300이 아닙니다. 그 밖의 예전 600건의 금융 기록은 그대로 보존되어 있지만 동작 전체를 증명하지는 않습니다. 이전의 M5/미디어 1회성 코호트는 바뀌지 않았습니다. 이전 3라운드 Google 사전 조건에서는 잘못된 도구 2건과 읽을 수 없는 양성(positive) 1건의 실패가 그대로 남아 있습니다. 추가 유료 호출은 승인되지 않았습니다. 최종 SDK 증거와 네이티브 축의 한계는 기준 측정의 성공과 별개입니다. 이전의 활성화/재오픈 스모크 테스트는 두 번 재오픈한 뒤 calls610/spent219159/held751233으로 기록되어 있고, SDK meter/canary/vision 테스트 4건이 19.38초 만에 통과했습니다. 이는 범위가 한정된 이전 체크포인트이며 최종 원장 합계가 아닙니다. 이전에 검증한 Chat 60쌍 코호트에는 실제 시도 120건, UpperBound 청구 120건이 있고 UnknownHold는 없습니다.

**네이티브 축 관찰. 암호학적 검증이나 네이티브 소비/동등성을 뜻하지 않습니다.** Generate는 변조, 누락, 중복을 받아들였습니다. Interactions는 격리된 실제 원본(양성 대조군), 한 소유자의 서명 변조, thought 운반체 누락, call 운반체 누락과 중복을 받아들였습니다. 모든 thought와 서명을 제거하면 일반적인 400이 반환되었고, THOUGHT 항목은 남기고 모든 서명 필드만 제거해도 일반적인 400이 반환되었습니다. 마지막 캡처에는 로컬에서 인코딩한 원본을 유지하는 대조군이 있었으나 같은 캡처에서 얻은 서버 측 양성 결과는 아니었습니다. 이전의 양성 코호트는 여전히 실제 데이터입니다. 이 관찰들이 보여 주는 것은 운반체(carrier)가 전부 없는 경우의 경계뿐이며, 발급자나 서명 검증, 벤더의 실제 소비 여부를 입증하지는 않습니다. 실제 보고서는 SDK의 `config/qualification-extension-results.json`, `qualification-final-summary.json`, `qualification-native-axis-results.json`, `qualification-combined-omission-results.json`, `qualification-signature-presence-results.json`입니다. 사전 조건 실패, 미실행, 음성(negative) 불확정 상태도 있는 그대로 기록해 두었습니다. thought만 또는 운반체만 누락한 경우에도 다른 운반체가 남아 있으면 받아들여졌지만, 이 결과는 발급자 검증이나 네이티브 소비에 관한 주장을 강화하지 않습니다.

**실제로 실행해 확인한 통합 증명과 남은 한계.** 가장 최근의 Core 전체 실행은 테스트 2242개, 실패 0, 건너뜀 16(RAM 프로세스 유실 케이스 14개는 해당 없음, 실제 자격 증명이 필요한 게이트 2개), 130.17초였습니다. `PgNestedJsonRoundTrips`는 중복 키, 순서, null 메타데이터, blob, residual을 정확히 보존했고 0.18초가 걸렸습니다. 바뀌지 않은 원본 공유 bank fork와 기존 Recorded CPU/Memory await/handoff 시나리오는 통과했습니다. 실제 wrapped Memory/SQLite/PostgreSQL/gRPC에 대한 finite130/hold65/lower129/strip/old-head/pruning/no-archive/import 프로브는 일반 빌드와 ASan+UBSan 빌드 모두 통과했습니다. 로컬 Memory/SQLite/PostgreSQL TSan 범위에서는 7건이 통과했고 경고는 0건입니다. gRPC까지 섞은 전체 구성에 시스템 Abseil/Protobuf를 더한 TSan 실행은 종료 코드 66으로 끝났고, 의존성과 생성된 RPC 스택에서 race 경고가 402건 나왔습니다. 이는 계측/커버리지의 한계이지 오탐으로 증명된 것은 아닙니다. 원격 TSan이나 race-freedom은 주장하지 않으며 경고도 억제하지 않았습니다. 설치된 find_package로 Program C++/C ABI/dualQuickJS 소비자 세 개를 빌드해 통과했습니다. 새로 설치한 NeoGraph/SchemaProvider 타입 소비자는 실제 HTTP 요청 2건, 코루틴 시작 전 provider 소멸, 네이티브/도구 재생(replay), 거부(refusal), known-zero/raw 보존, 실제 LinkedMismatch 거부를 통과했습니다. 브라우저에서 Alice/Bob 격리와 generation 2 교체는 눈으로 확인했고, PostgreSQL Program Chat 블랙박스 테스트 6건은 18.989초에 통과했습니다. 가장 최근의 SDK 테스트는 26/26 통과, 실패 0, 74.07초였습니다. 최종 Release Graph는 16개 구성 × 새 프로세스 3회 반복(기록 48건)을 38.29초에 완료했고 실패 0건이며 실제 프로토콜과 소유 결과(owned-outcome) 검사를 모두 통과했습니다. NeoGraph의 `benchmarks/provider-cutover-final-results.json`과 `benchmarks/provider-cutover-final-summary.json`에 이 최종 코호트가 따로 보관되어 있습니다. 측정 중에는 컴파일러도 유료 모델도 실행하지 않았습니다. 과거 코호트는 바뀌지 않았으며 의미나 자원 사용의 동등성은 주장하지 않습니다. 불안정한 SDK/ABI3은 안정 릴리스도, 더 넓은 플랫폼에 대한 자격 검증도 아닙니다.

**최소한의 유료 증거(2026-10-03). 폭넓은 자격 검증이 아님.** 따로 승인한 1회성 호출 3건의 결과는 다음과 같습니다. Images는 JPEG 1장(1024×1024, 360685바이트), 입력/출력/전체 토큰 19/1408/1427이며 눈으로 확인했습니다. Veo는 MP4 1개(1280×720, 4초, 437737바이트)이며 생성 1회와 상태 조회 GET 3회가 있었고, 사용량은 null 가능한 값이며 Chromium에서 디코딩해 눈으로 확인했습니다. Decisions는 `typesafe/jev-1.13`, 확률 0.93, 입력/출력 토큰 283/21, 전체 토큰은 알 수 없음, API가 보고한 비용은 USD 0.000011886입니다. 이미지의 기본 요금 USD 0.0336에 텍스트/thinking 요금을 더한 값과 Veo의 USD 0.20은 카탈로그상의 예상 금액이지 청구서가 아닙니다. 최소 이미지 스모크 테스트에서는 가격대별 내역을 수집하지 못했습니다. 어느 결과도 1회성 권한을 갱신하지 않으며 재실행을 승인하지도 않습니다.

완료된 chat 쌍만으로는 하위 벤더가 네이티브 continuation을 소비했다는 사실이 입증되지 않습니다.

- [ABI_POLICY.md](ABI_POLICY.md)
- [ASYNC_GUIDE.md](ASYNC_GUIDE.md)
- [Issue #5](https://github.com/fox1245/NeoGraph/issues/5) — 과거의 결정. 대체된 provider 호환 정책.

---

# 마이그레이션 3: `compile()`의 워커 풀 기본값이 1 (v0.1.4 회귀 복원)

## 변경 내용

`GraphEngine::compile(def, ctx)`의 기본 워커 수는 v0.1.4(`b59444f`)부터 `std::thread::hardware_concurrency()`였지만, 현재의 v1 이전 API에서는 **`1`(엔진이 소유하는 thread_pool 없음)**로 되돌렸습니다.

## 이유

`hardware_concurrency`를 기본값으로 쓰면 모든 fan-out 노드에서 스레드 간 submit 오버헤드(작업당 약 6-7 µs)가 생깁니다. bench par 측정(워커 5개 + summarizer)이 11.6 µs에서 44 µs로 늘어 4배 느려졌습니다. 측정 환경에서 이분 탐색(bisect)으로 확인한 원인은 v0.1.4의 `b59444f`였습니다.

ms~s 단위인 LLM 호출 같은 실제 프로덕션 워크로드에서는 submit 오버헤드를 무시할 수 있습니다. 그래도 다음 문제가 있습니다.
- **fan-out이 없는 단순한 그래프**도 풀 오버헤드를 내야 하는데, 얻는 것이 없습니다.
- **스레드 안전하지 않은 노드 상태**가 기본적으로 멀티 워커에 노출됩니다. 실수하기 쉬운 함정입니다.

그래서 기본값을 안전한 1로 정했고, 실제 fan-out 병렬화가 필요하면 명시적으로 켜야 합니다.

## 마이그레이션

fan-out 병렬화가 필요한 그래프(예: `Send`를 여러 번 dispatch하는 경우, `parallel_group`, deep_research의 researcher 5개 fan-out)는 `compile()` 뒤에 다음을 명시적으로 호출해야 합니다.

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

fan-out이 없는 단순한 그래프나 가벼운 fan-out 그래프(LLM 호출이 대부분인 경우)는 기본값을 그대로 두면 됩니다. 풀 오버헤드는 0입니다.

## 마이그레이션하지 않으면 어떻게 되는가

워커 풀을 켜지 않으면 CPU 위주의 fan-out이 호출자의 executor에서 실행됩니다. 비동기 I/O는 그래도 서로 겹쳐 실행될 수 있습니다. 별도의 실행 스레드가 필요하면 `set_worker_count_auto()`나 명시적인 워커 수를 사용하세요. 워커 수만으로 일관성이나 속도 향상이 보장되지는 않습니다.

## 영향을 받는 NeoGraph 내부 예제

이 변경과 함께 추가된 fan-out 가시성 패치에서는 의도를 유지하기 위해 명시적 호출을 넣었습니다. 사용자 코드가 같은 경우라면 같은 패턴을 적용하세요.

- `examples/10_send_command.cpp` — 동기 `sleep_for`를 쓰는 ResearcherNode가 Send로 fan-out하며, `engine->set_worker_count_auto()`를 추가함
- `examples/14_plan_executor.cpp` — 하위 주제 5개를 Send로 fan-out(동기 sleep_for)하며, 같은 호출을 추가함
- `examples/21_mcp_fanout.cpp` — MCP 도구 호출 3개를 동시에 실행하며, 같은 호출을 추가함
- `examples/36_classifier_fanout.cpp` — 이미 `set_worker_count(5)`가 명시되어 있었음. 잘못된 기본값을 설명하던 주석을 고침(현재 기본값은 1, 엔진이 소유하는 풀 없음)
- `src/core/deep_research_graph.cpp`의 `create_deep_research_graph()` 빌더 — `compile()` 직후 `set_worker_count_auto()`를 호출해서 supervisor의 researcher N개가 실제로 동시에 실행되도록 함

`examples/05_parallel_fanout.cpp`는 `io_context` 위에서 코루틴 타이머가 겹쳐 실행되는 방식(동기 sleep 없음)이라 워커 풀이 영향을 주지 않으므로 그대로 두었습니다.

사용자 코드에 같은 패턴이 있다면 다음과 같이 합니다.

```cpp
auto engine = GraphEngine::compile(def, ctx);
engine->set_worker_count_auto();   // ← add this line
```

---

# 마이그레이션 4: C++ ABI와 필수 재빌드

이제 NeoGraph는 컴파일되는 모든 공개 라이브러리에 프로젝트의 `VERSION`과 메이저 `SOVERSION`을 부여합니다. v1 이전의 모든 릴리스는 ABI 세대 0을 쓰지만 `0.x` 바이너리 호환은 보장되지 않습니다. 변경 기록(changelog)이 경계를 알리면 모든 C++ 소비자를 다시 빌드해야 합니다. 특히 `0.11.1` 이하에서 bounded `NodeCache`가 들어간 릴리스로 올릴 때는 `NodeCache`와 `EngineConfig`의 객체 레이아웃이 바뀌었으므로 다시 빌드해야 합니다.

타입 기반 Provider 마이그레이션은 가상 함수 계약을 `get_name`, `family`, `prepare`로 바꾸고 기존 completion 가상 함수를 제거합니다. 기존 Provider 바이너리는 호환되지 않으므로, 사용자 정의 provider와 이에 의존하는 모든 C++ 소비자를 맞는 헤더·라이브러리로 다시 빌드해야 합니다. Core가 SchemaProvider 타입도 노출하므로 LLM을 끈 빌드에도 SDK 런타임이 필요합니다. 앞으로의 안정 인터페이스는 안정된 레이아웃을 수정하지 말고 별도의 capability 인터페이스와 어댑터를 우선해야 합니다. 현재의 v1 이전 인터페이스는 바이너리 호환을 약속하지 않습니다.

플랫폼별 라이브러리 이름, 알려진 모든 재빌드 경계, CI 검증은 [바이너리 호환성 정책](ABI_POLICY.md)을 참고하세요.
