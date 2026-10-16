<!-- neograph-i18n: source=docs/migration-v0.4-to-v1.0.md locale=ko source_sha256=eef2e9b21ee3b2b0cc351a474c51177be3409e7f36c2def9e729f7d94a1295ee -->
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

이 변경은 소스 호환과 바이너리 호환을 모두 깨뜨립니다. 모든 C++ 소비자와 사용자 정의 provider를 새 헤더·라이브러리에 맞춰 다시 컴파일해야 합니다. `CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, descriptor 인터프리터, Responses WebSocket 경로는 제거되었고 별칭이나 호환용 브리지도 없습니다. SDK는 알파 `0.3.2`이며 인터페이스 리비전 6 / 공유 ABI 6입니다. 기능 검사는 out-of-line으로 하며, 이것이 안정 릴리스라는 뜻은 아닙니다.

제거된 `Provider::complete`, `complete_async`, `complete_stream`, `complete_stream_async` 호출부는 모드를 명시한 요청과 `invoke` / `dispatch`(또는 C++ 비동기 짝)로 바꿉니다. `Agent::complete`는 소유 결과를 반환하는 단일 턴 API로 별도로 남아 있습니다.

CMake 3.20 이상이 필요합니다. `NEOGRAPH_BUILD_LLM=OFF`로 빌드해도 Core가 소유권이 있는 타입 provider 계약을 export하므로 SchemaProvider 런타임은 여전히 필수입니다. `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`를 명시하면 그 경로를 최우선으로 씁니다. 지정하지 않으면 CMake는 먼저 설치된 `SchemaProvider` 런타임 패키지를 찾고, 없으면 `NEOGRAPH_FETCH_SCHEMAPROVIDER=ON`(기본값)일 때 `cmake/NeoGraphSchemaProvider.cmake`에 고정된 변경 불가능한 GitHub archive를 내려받습니다. 오프라인으로 빌드하려면 SDK를 설치하고 `CMAKE_PREFIX_PATH`에 그 설치 prefix를 지정한 뒤 `-DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`를 넘깁니다. CMake는 옆에 있는 체크아웃을 추측해서 쓰지 않고, 제거된 번들 인터프리터를 선택하지도 않습니다. NeoGraph의 선택 사항인 HTTP 모듈을 꺼 두어도 SDK 런타임에는 전송 계층 의존성이 필요합니다. SDK 런타임은 WebAssembly 빌드에서 쓸 수 없습니다.

Python도 C++와 같이 소유권이 있는 요청/결과 경계를 제공합니다. 이 경계는 `make_provider_request`, `Provider.prepare`, `dispatch`, `invoke`로 이루어집니다. provider 기록에는 타입이 있는 파트를 가진 `ProviderMessage`를 사용해야 합니다. `ChatMessage`는 그래프용 편의 투영으로 남아 있습니다. 반환된 SDK 실패는 `ProviderOutcome.failure`로 확인합니다. 호스트의 observer나 정산(settlement)에서 예외가 나면 그 예외가 `outcome`과 `cause`를 유지합니다. 생성자와 GIL/콜백 동작은 [Python 바인딩 가이드](python-binding.md)를 참고하세요.

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

유지된 호출별 제어 항목은 `extra_fields` 딕셔너리가 아니라 타입이 있는 필드입니다.

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

### 네이티브 기록

체크포인트는 provider의 네이티브 기록(추론 내용, 서명, 순서가 있는 도구 호출)을 그것을 만든 프로세스 안에서만 C++ 사이드카로 보관합니다. `ChatMessage`, `ChatTool`, JSON은 이식 가능한 사본이며 네이티브 권한이 없습니다. 재시작 뒤에도 네이티브 기록을 유지해야 하는 호스트는 `sp::NativeArchive`를 설정합니다. 이 계약은 [타입이 있는 provider 레퍼런스](reference-en.md#portable-projections)에 설명되어 있습니다.

함께 보기:
- [ABI_POLICY.md](ABI_POLICY.md)
- [ASYNC_GUIDE.md](ASYNC_GUIDE.md)

---

# 마이그레이션 3: `compile()`의 워커 풀 기본값이 1 (v0.1.4 회귀 복원)

## 변경 내용

`GraphEngine::compile(def, ctx)`의 기본 워커 수는 v0.1.4(`b59444f`)부터 `std::thread::hardware_concurrency()`였지만, 현재의 v1 이전 API에서는 **`1`(엔진이 소유하는 thread_pool 없음)**로 되돌렸습니다.

## 이유

`hardware_concurrency`를 기본값으로 쓰면 모든 fan-out 노드에서 스레드 간 submit 오버헤드(작업당 약 6-7 µs)가 생깁니다. bench par 측정(워커 5개 + summarizer)이 11.6 µs에서 44 µs로 늘어 4배 느려졌습니다.

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

---

# 마이그레이션 4: C++ ABI와 필수 재빌드

이제 NeoGraph는 컴파일되는 모든 공개 라이브러리에 프로젝트의 `VERSION`과 메이저 `SOVERSION`을 부여합니다. v1 이전의 모든 릴리스는 ABI 세대 0을 쓰지만 `0.x` 바이너리 호환은 보장되지 않습니다. 변경 기록(changelog)이 경계를 알리면 모든 C++ 소비자를 다시 빌드해야 합니다. 특히 `0.11.1` 이하에서 bounded `NodeCache`가 들어간 릴리스로 올릴 때는 `NodeCache`와 `EngineConfig`의 객체 레이아웃이 바뀌었으므로 다시 빌드해야 합니다.

타입 기반 Provider 마이그레이션은 가상 함수 계약을 `get_name`, `family`, `prepare`로 바꾸고 기존 completion 가상 함수를 제거합니다. 기존 Provider 바이너리는 호환되지 않으므로, 사용자 정의 provider와 이에 의존하는 모든 C++ 소비자를 맞는 헤더·라이브러리로 다시 빌드해야 합니다. Core가 SchemaProvider 타입도 노출하므로 LLM을 끈 빌드에도 SDK 런타임이 필요합니다. 앞으로의 안정 인터페이스는 안정된 레이아웃을 수정하지 말고 별도의 capability 인터페이스와 어댑터를 우선해야 합니다. 현재의 v1 이전 인터페이스는 바이너리 호환을 약속하지 않습니다.

플랫폼별 라이브러리 이름, 알려진 모든 재빌드 경계, CI 검증은 [바이너리 호환성 정책](ABI_POLICY.md)을 참고하세요.
