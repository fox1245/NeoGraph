<!-- neograph-i18n: source=docs/migration-v0.4-to-v1.0.md locale=ko source_sha256=47da3b0da6a1a0469315657c9eb8079b216d0c51eafce0bed090d91fc5c3c7b5 -->
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
| `execute_async(state)` | `co_await provider->invoke_async(std::move(request));` 같은 네이티브 비동기 |
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

### 사례 3 — 스트리밍 노드 (`execute_stream` 이전)

공개 계약은 소유 typed 준비/dispatch이며 동기·비동기 가상 completion 쌍이 아니다. `ProviderRequest.payload`는 Chat, Messages, Responses, Gemini, Interactions의 SDK 요청 variant이다. `ProviderMode::Collect` / `Stream`은 관측자 유무와 독립적으로 전송을 선택한다. `on_event`는 빌린 typed `sp::Event` view를 받는다. 콜백 이후 필요한 데이터만 복사한다. raw JSON override나 portable projection을 통한 native 권한 가져오기는 허용되지 않는다.

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

# 이전 2: typed lossless Provider 전환 (필수 재컴파일)

소스 및 바이너리 단절이다. 모든 C++ 소비자와 사용자 공급자를 새 헤더/라이브러리로 재컴파일한다. `CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, descriptor interpreter와 Responses WebSocket은 alias/호환 bridge 없이 제거되었다. SDK는 불안정 `0.0.0`, interface revision 3 / shared ABI 3이며 out-of-line capability check를 사용한다. 안정 릴리스 선언이 아니다. 현재 runtime/archive는 Linux/POSIX이며 Windows·macOS·WASM runtime 검증을 뜻하지 않는다. Python provider binding/wrapper는 유예되었고 이 C++ 변경으로 포팅되지 않는다.

Fresh installed find_package Program C++/C ABI/dualQuickJS consumer와 NeoGraph/SchemaProvider typed2-request lifetime/native/raw/mismatch consumer가 pass했다. Interface/ABI 선언만과 실제 package 결과는 별개이며 더 넓은 platform이나 stable release를 주장하지 않는다.

공개 계약은 소유 typed 준비/dispatch이며 동기·비동기 가상 completion 쌍이 아니다. `ProviderRequest.payload`는 Chat, Messages, Responses, Gemini, Interactions의 SDK 요청 variant이다. `ProviderMode::Collect` / `Stream`은 관측자 유무와 독립적으로 전송을 선택한다. `on_event`는 빌린 typed `sp::Event` view를 받는다. 콜백 이후 필요한 데이터만 복사한다. raw JSON override나 portable projection을 통한 native 권한 가져오기는 허용되지 않는다.

```cpp
#include <neograph/provider.h>
#include <neograph/runtime_interposition_consumer.h>
#include <neograph/controlled_provider.h>

// Public operation signatures (the only virtual operation is prepare).
// ProviderRequest owns the SDK request variant, mode, options and observer.
// invoke[_async](request) = prepare once, then dispatch the same handle.
// dispatch[_async](prepared) returns sp::runtime::Result.
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

### PreparedProviderRequest / ProviderBudgetClaim
`prepare()`는 검증·인코딩을 정확히 한 번 수행하고 원래 deadline과 취소 상태를 가진 이동 전용 `PreparedProviderRequest`를 만든다. 영속 호출자는 `Provider::request_digest()`를 assembly에 바인딩하고 승인된 예산 claim을 예약하며 dispatch receipt를 기록한 다음 같은 핸들을 `ControlledProvider::dispatch_prepared(_async)`로 소비한다. gate 이후 요청을 재생성하지 않는다. 중복 receipt는 재전송하지 않는다. 사용자 공급자는 `get_name()`, `family()`, `prepare()`를 구현하고 `prepare_runtime()` 또는 `prepare_local()`을 사용한다. local callback은 `this` 대신 소유 shared 상태를 캡처한다.

선택적 `ProviderControls`는 호출자 선택이며 강제 기본값이나 몰래 clamp한 cap이 아니다. family가 지원하지 않는 제어는 dispatch 전에 거부한다. 유한 예산 호출에는 승인된 실제 모델 input/output 한계가 필요하며 없으면 `LimitUnknown`으로 실패한다. 예약은 보수적인 지출 권한이지 보고 사용량·예측·청구서가 아니다. 미상/부분/delivery-unknown 결과는 hold를 유지하고 실제 최종 보고로 정산하며 초과 보고도 전부 청구한다. 재시도는 단일 명시적 계층이며 기본 off, 유한 window와 unknown-prior hold를 사용한다. 숨은 재전송은 없다.


공급자 호출은 `sp::runtime::Result`, 즉 `sp::Completion` 또는 `sp::Failure`를 담은 불변 소유 `std::shared_ptr<const sp::Outcome>`를 반환한다. 표시 텍스트만이 아니라 전체 결과를 보존한다. 순서 있는 메시지/파트, native continuation, 전체 wire envelope, 순서 있는 raw 관측, 중단 근거와 실제 시도 메타데이터는 호출 및 클라이언트 소멸 후에도 남는다. 사용량은 근거·단계·품질을 갖는 nullable `uint64_t`이며 누락은 0이 아니라 미상이다. 실패도 원래의 부분 결과를 보존한다. `ProviderFailure::outcome()`과 `ProviderObserverError::outcome()`은 실제 결과를 보존하며 후자의 `cause()`에는 관측자 예외가 남는다.

실제 결과 이후 post-effect 정산이나 terminal receipt 영속화가 실패하면 `ProviderDispatchOutcomePersistenceError`의 `outcome()`은 원래 불변 결과를, `cause()`는 원래 영속 예외를 보존한다. 전달도 실패했으면 `delivery_error()`가 원래 관측자 예외를 보존한다. 영속화 성공 뒤 관측자 실패는 원래 예외를 그대로 다시 던진다. 미상/결과 없는 transport 실패는 결과를 조작하지 않는다.
### SchemaProvider

`SchemaProvider`는 승인된 `sp::descriptor::ValidatedDescriptor`, `sp::runtime::Options`, 선택적 `SchemaProvider::Defaults`를 받는다. descriptor는 closed/versioned 데이터 admission이지 요청/응답 interpreter나 임의 primitive registry가 아니다. credential은 공개 descriptor가 아니라 runtime options에 둔다. Defaults는 typed OpenRouter routing과 Responses 보관(`responses_store`)만 포함하고 후자는 Responses에만 유효하다. Hosted OpenRouter routing·retention·JSON 형식은 선언된 typed 제어다. Images, Veo, Decisions는 별도 NeoGraph typed client와 별도 승인을 쓰며 SDK chat grant를 물려받지 않는다.

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

### Native 기록 / 예산

`ChatMessage` / `ChatTool`과 JSON은 portable projection이지 native 권한이 아니다. Portable 포맷은 [`provider-message-v2`](../schemas/provider-message-v2.schema.json), [`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json)를 유지한다. 실제 C++ checkpoint sidecar는 메모리에서 native seal을 보존한다. 영속 native 기록에는 host-owned `sp::NativeArchive`가 필요하다. closed v3 / `spna3`는 독립 키를 쓰는 인증된 owner-private custody이며 archive v2는 업그레이드하거나 해석하지 않고 거부한다. 인증은 모든 semantic descriptor 선택(origin/path/header, policy, 요청 field mapping, usage path, stop mapping), owner와 정확한 custody binding을 결합한다. 암호화나 vendor-issuer 인증은 아니다. archive 본문·키·native blob·raw wire 관측을 공개하지 않는다. Archive는 증거 저장소이지 돈의 grant나 spending lease가 아니다. Program/external bank는 독립 journal 소유이며 snapshot 복사로 credit을 만들 수 없다.

**Standalone bank journal 수정 — 현재 계약 개정; 실제 runtime 증거는 아래.** Owner-approved protocol은 단조 trusted-store namespace obligation과 실제 불변 original owner/thread/graph scope, ceiling, deadline/clock identity, generation을 요구한다. 전체 checkpoint commitment·revision에 대한 정확한 durable head CAS만 host-owned opaque lease를 발급할 수 있다. 정확한 pending effect window를 provider I/O 전에 영속화해야 하며 실제 SDK outcome, charge, nullable report, hold, dedup identity로 정산해야 한다. Checkpoint와 next head는 같은 owned actor/revision 아래 원자적으로 publish해야 한다. Bank metadata 제거·checkpoint pruning·old authenticated snapshot replay·같은 ID overwrite·actor 상실은 credit을 주면 안 된다. 기존 65 hold에서 ceiling 130을 129로 낮추면 추가 65를 허용할 수 없다. 입증된 no-effect 실패는 unchanged head를 release해 authentic 130 복구가 가능해야 한다. Crash/unknown/lost-lease window는 refund/retry/fallback 없이 hold를 유지한다. Plain/pristine archive 설정은 money/native spending lease를 주지 않고 현재 `config.usage`는 기존 standalone obligation을 대체할 수 없다. Program/external-bank journal 소유는 유지된다. 이는 요구 계약이다. 실제 currency/custody 증거와 instrumentation 한계는 아래에 있으며 stable released API 보장은 아니다.

**현재 선언; 통합 runtime 증거는 아래:** `<neograph/graph/checkpoint.h>`는 `owner_scope`, logical `thread_id`, private backend `storage_thread_id`, `graph_identity`, `original_ceiling`, `original_deadline_ticks`, `deadline_clock_identity`를 가진 `ManagedBudgetLeaseScope`를 선언한다. `OwnedManagedBudgetLease`는 read-only `scope()`, `actor_id()`, 불변 `bank_generation()`, `revision()`, `head_checkpoint_id()`, `head_commitment()`를 노출하며 공개 authority-import constructor가 없다. `ManagedBudgetEffectReceipt`는 `active()`, `effect_id()`, `claim_amount()`, `request_digest()`를 노출하고 default receipt는 권한을 주지 않는다. `CheckpointStore`는 `acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`, `begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`, `settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`, `publish_managed_budget_checkpoint(lease, checkpoint)`, `release_managed_budget_lease(lease)`와 `_async` counterpart를 선언한다. Sync `CheckpointStoreCore`와 `AsyncCheckpointStore`는 각각 해당 variant를 제공한다. `managed_budget_checkpoint_commitment(checkpoint)`는 bank JSON뿐 아니라 전체 영속 checkpoint를 결합한다. 이 선언은 backend CAS·currency 안전성·installed ABI 호환성·실제 성공 runtime 경로를 입증하지 않는다.

**실제 InMemory shared-bank fork는 유지되었으며 실제 증명 완료.** 원래 실제 C++ fork는 ONE original financial journal과 trusted current branch head를 쓰며 grant를 복제하지 않는다. `publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)` 및 `_async`는 authentic current source/full commitment와 실제 same-bank native C++ pointer를 요구하고 durable standalone fork는 명시적 unsupported로 남는다. `OwnedManagedBudgetLease::scope()`와 original owner/thread/graph, ceiling, deadline/clock, generation은 불변이다. Read-only store-issued `execution_thread_id()` / `execution_storage_thread_id()`는 execution branch를 별도로 선택하며 `GraphState::budget_original_thread_id()`는 원래 financial bank를 가리킨다. 정확한 selected-branch head CAS와 global actor/revision은 canonical current counter, pending effect, burned identity에 대해 모든 branch를 직렬화한다. Original/fork branch는 보충 없이 계속 사용할 수 있다. Stale snapshot·checkpoint copy·imported JSON은 alias를 발급하거나 head를 되돌릴 수 없다. 원래 root30 → charge3 → original continuation6 → fork lower20 → continuation9 same-bank 증명은 변경하지 않은 test_graph_engine.cpp:810–913에서 PASSED했다. Saved original ceiling30은 effective fork ceiling20과 별개이며 widening31과 JSON-only restore는 거부해야 한다. Unbounded reported observation은 사실 data이지 finite grant가 아니다. 입증된 zero-effect lease만 unchanged head를 release할 수 있고 unknown/pending effect는 obligation을 유지한다.

**현재 release-error 계약; 실제 suite/probe는 아래.** `<neograph/graph/engine.h>`의 `graph::ManagedBudgetLeaseReleaseError`는 `ProviderOutcomeError`를 상속한다. `cause()`는 원래 execution exception을 보존하고 `release_error()`는 보조 durable lease-disposition 실패를 노출한다. `outcome()`은 실제 SDK 증거가 있으면 보존하고 SDK outcome이 없으면 null이다. Release 실패는 결과를 만들거나 재dispatch를 허용할 수 없다. Closed `_neograph_managed_budget_scope` metadata는 원래 logical scope/cap/deadline clock/generation을 설명하지만 backend CAS 권한이 아닌 data이다.

**Archive-owner/retention 계약; 실제 suite/probe는 아래.** Finite standalone root 또는 authenticated finite source만 실제 설정된 `sp::NativeArchive::owner_scope()`에서 생략된 original owner를 상속한다. Unbounded/plain owner metadata 의미는 바뀌지 않는다. 명시적으로 충돌하는 archive owner는 lease acquire 전에 거부한다. `CheckpointStore::retains_native_checkpoint() const noexcept`와 대응 Core/Async storage capability는 기본 false이며 실제 InMemory backend만 true로 override하고 wrapper는 실제 retention을 위임해야 한다. 이 read-only 설명은 정당한 unleased/plain/unbounded C++ native checkpoint custody를 허용하되 spending credit이나 native replay authority를 주지 않는다. Leased custody는 JSON flag나 추측한 store type 대신 실제 store-issued receipt를 사용한다.

**Native-custody pre-I/O gate; 실제 suite/probe는 아래.** Managed effect begin은 pending-effect/slot/held-window 변경 전에 실제 결합된 NativeArchive 또는 실제 local store-issued private C++ retention capability를 요구한다. Private capability는 JSON에서 import하거나 wire로 전달하지 않는다. C++ sidecar는 경계를 넘을 수 없으므로 remote backend가 InMemory여도 gRPC는 실제 client·server archive를 요구한다. Archive가 finite source owner를 제공하지 않으면 원래 anonymous owner scope는 빈 값으로 유지하고 실제 archive binding은 original scope와 일치해야 한다. Financial head/lease 증거만으로 native-custody readiness를 증명하지 않는다.

진단 JSON은 문법상 유효한 duplicate-key 문서를 포함해 원래 raw byte를 보존하지만 실행 가능한 요청/config admission은 중복을 계속 거부한다. 원래 non-2xx 응답 JSON은 두 번째 손실 parse 없이 `http.error` 증거에 남는다. named SSE error는 뒤의 정상 stream close보다 우선한다. 진단/provider metadata는 무관한 작은 error-text cap이 아니라 승인된 source extent로 제한된다.

`ProviderRequest::observer_limits`는 host-only이다. 명시한 `max_events`·`max_bytes`는 양수여야 하며 승인된 SDK 전달 상한을 낮출 수만 있다. `provider-request/v3` digest는 실제 limit, mode, encoded body, retry policy와 모든 semantic descriptor binding을 결합한다. Bridge는 queued·draining batch 전체에서 실제 PMR vector/map capacity와 소유 event/document byte를 계산하며 queue mutex 밖에서 cancellation을 요청한다. 이름이 `messages`인 Generic channel을 chat으로 강제 변환하지 않는다. native `history` channel을 `messages`에 mapping하면 JSON에서 native 권한을 만드는 대신 C++ sidecar를 보존한다.

`ProviderOutcomeError`는 결과를 보존하는 공통 host-error base이다. `ProviderObserverError`와 `ProviderDispatchOutcomePersistenceError`는 완전히 drain된 SDK 결과와 원래 `cause()`를 보존하며 후자는 보조 observer 실패도 `delivery_error()`에 보존한다. `ProviderFailure::outcome()`은 SDK 실패 자체를 보존한다. 이 증거는 Node/Program 재dispatch 권한이 아니다. Provider retry의 유일한 소유자는 SDK이며 caller가 선택한 `max_output_tokens`를 조용히 clamp하지 않는다.

`ProgramFailure`는 live `provider_outcome`·`provider_cause`를 보존한다. Canonical factual SDK witness는 실제 archive custody를 owner/run/version/bundle/operation/attempt에 결합하며 Runtime은 복구 실패를 노출하기 전에 설정된 custody를 즉시 복원한다. 공개 data-only `ProgramResult::create()`는 미리 채운 witness로 우회할 수 없고 unresolved parsed seal은 실행 결과가 아니다. 프로세스 재시작 후 원래 exception pointer는 없으므로 `provider_cause == nullptr`이며 text에서 재생성하지 않는다. 영속화할 수 없는 실패는 serialize/publish/replay할 수 없다.

`RecordedBindingSet`는 source-bound move-only data이지 caller가 제공하는 dispatcher가 아니다. 신뢰된 Catalog `recorded_capability_binder`는 실제 영속 source event를 독립적으로 읽어 captured-only capability를 materialize한다. `ProgramRuntime::replay_recorded()`는 원래 selected-source permission을 검사한 뒤 실제 남은 bank를 durable CAS로 이전한다. inherited spend는 새 model grant가 아니다. 구 `start_recorded` 갱신 API는 제거되었다. InMemory/File/SQLite/PostgreSQL Program store는 실행 내내 정확하고 불변인 owned lease를 보존하며 expiry로 갱신하지 않는다. Controlled JavaScript도 underlying capability manifest를 검사하고 정확한 completed command 결과를 소비하며 external effect를 재dispatch하지 않는다.

**Recorded-control causal fix는 full suite에서 실제 증명 완료.** Captured command replay는 실행 전에 새 CPU wall-time/Core work만 durable reserve하고 측정 work와 새 Core checkpoint를 result CAS로 publish한다. 새 model·money·Program-operation allowance를 소비하지 않고 captured external effect를 재dispatch하지 않는다. 미정산 reservation은 debit을 유지한다. Reservation은 첫 새 Core checkpoint를 거부했던 일반 Running→Running transition 대신 인증된 settlement transition을 선택한다. Await channel receive·timer wait/cancel·handoff wait 시작/release는 소유 executor/strand에서 직렬화한다. 기존 Recorded CPU/Memory await/handoff scenario는 full suite에서 pass했다. Remote TSan coverage 한계는 아래에 명시한다.

**유료 관측 완료; 보편적 qualification은 아님.** 원래 `SPQUAL1` base630/1000000 microUSD는 불변이다. 같은 원래 ledger의 ONE hash-chained `A`가 승인 extension480/3000000을 받아 aggregate1110/4000000이 된다. Calls/spent/hold/settlement는 누적이며 새 grant ID/header/reset은 없다. 정확한 declaration byte/file identity와 original authorization/baseline/catalog/activation/ledger-prefix hash/totals는 고정되고 삭제·교체·변경은 fail closed한다. 최종 canonical ledger는 calls1110/spent437958/held1287828 microUSD, eventA1, limits1110/4000000이다. Spent+held US$1.725786은 LOCAL catalogue meter이지 invoice가 아니다. 기록된 five-family60-pair baseline은600 request를 완료했다: Chat60/60, Responses60/60, Messages60/60, Generate56/60(incorrect-vision SSE4개), Interactions57/60(incorrect-vision buffered1개/SSE2개). 합계293/300 pair이며300/300은 아니다. 다른 old600 financial record는 보존하되 완전한 behavioral proof는 아니다. 이전 M5/media one-shot cohort는 그대로다. 이전 Google3-round prerequisite의 invalid-tool2개/unreadable-positive1개 실패 상태를 유지한다. 추가 유료 호출은 승인되지 않는다. 최종 SDK 증거와 native-axis 한계는 baseline 성공과 별개다. 이전 activation/reopen smoke는 두 번 reopen한 calls610/spent219159/held751233 및 SDK meter/canary/vision4-test19.38초 pass로 보존한다. 이는 범위가 정해진 이전 checkpoint이지 최종 ledger totals가 아니다. 이전 검증된 Chat60-pair cohort의 실제 attempt120,UpperBound charge120,UnknownHold 없음도 보존한다.

**Native-axis 관측은 cryptographic 검증·native consumption/equivalence가 아니다.** Generate는 mutation/omission/duplication을 받아들였다. Interactions는 isolated genuine source/positive control, one-owner signature mutation, thought-carrier omission, call-carrier omission, duplication을 받아들였다. 모든 thought/signature 제거는 generic400을 반환했고 THOUGHT item을 유지한 채 모든 signature field를 제거해도 generic400이었다. 마지막 capture에는 local encoded-original retention control만 있고 same-capture server positive는 없었다. 이전 positive cohort는 실제 증거다. 이는 aggregate-carrier-absence boundary만 입증하며 issuer/signature 검증이나 vendor consumption을 입증하지 않는다. 실제 report: SDK `config/qualification-extension-results.json`, `qualification-final-summary.json`, `qualification-native-axis-results.json`, `qualification-combined-omission-results.json`, `qualification-signature-presence-results.json`. Prerequisite-failed/not-run/negative-inconclusive 상태는 사실 그대로 유지한다. Thought-only/carrier-only omission은 다른 carrier가 남아 있는 상태에서 받아들여졌다. Issuer-validation/native-consumption 주장을 강화하지 않는다.

**실제 통합 증명과 남은 한계.** 최신 Core full run:2242 test, 실패0,skip16(RAM process-loss 비적용14개/live-credential gate2개),130.17초. `PgNestedJsonRoundTrips`는 duplicate key/order/null metadata,blob,residual을 정확히 보존하며0.18초 pass했다. 변경하지 않은 원래 shared-bank fork와 기존 Recorded CPU/Memory await/handoff scenario가 pass했다. 실제 wrappedMemory/SQLite/PostgreSQL/gRPC finite130/hold65/lower129/strip/old-head/pruning/no-archive/import probe는 plain과 ASan+UBSan에서 pass했다. LOCAL Memory/SQLite/PostgreSQL TSan scope는7개 pass,warning0이다. System Abseil/Protobuf를 포함한 full mixed gRPC TSan은 exit66,dependency/generated-RPC stack에 race warning402개였다. 이는 instrumentation/coverage 한계이지 proven false positive가 아니다. Remote TSan/race-free를 주장하지 않으며 warning을 suppress하지 않는다. Installed find_package Program C++/C ABI/dualQuickJS3 consumer는 pass했다. Fresh installed NeoGraph/SchemaProvider typed consumer는 실제 HTTP request2개,coroutine 시작 전 provider 소멸,native/tool replay,refusal,known-zero/raw 보존,실제 LinkedMismatch 거부를 pass했다. Browser Alice/Bob isolation·generation2 replacement를 실제 시각 검증했고 PostgreSQL Program Chat black-box6개는18.989초 pass했다. 최신 SDK26/26은 실패0,74.07초 pass했다. 최종 ReleaseGraph16설정 ×fresh process3회/48기록은38.29초,실패0,모든 actual protocol/owned-outcome check pass로 완료했다. NeoGraph `benchmarks/provider-cutover-final-results.json`과 `benchmarks/provider-cutover-final-summary.json`은 별도의 최종 cohort를 보존한다. 측정 중 compiler/유료 model은 실행하지 않았고 historical cohort는 그대로이며 semantic/resource equivalence를 주장하지 않는다. Unstable SDK/ABI3는 stable release나 더 넓은 platform qualification이 아니다.

**최소 유료 증거(2026-10-03)는 광범위 qualification이 아니다.** 별도로 승인한 one-shot 세 호출 결과: Images—JPEG 1개, 1024×1024, 360685 byte, input/output/total token 19/1408/1427, 실제 시각 검사; Veo—MP4 1개, 1280×720, 4초, 437737 byte, generation 1회와 status GET 3회, usage nullable, Chromium decode·시각 검사; Decisions—`typesafe/jev-1.13`, probability 0.93, input/output token 283/21, total 미상, API 보고 비용 USD 0.000011886. Image USD 0.0336 base + text/thinking, Veo USD 0.20은 catalog 예상이지 invoice가 아니다. 최소 image smoke에서는 가격대별 내역을 수집하지 않았다. 결과는 one-shot 권한을 갱신하거나 재실행을 승인하지 않는다.

완료된 chat pair는 downstream vendor의 native-continuation 소비를 입증하지 않는다.

Stage 3(2026-04) 설계와 당시 측정 테스트 수는 역사로 보존한다. provider 호환/crossover 결정은 아래 typed lossless 전환으로 대체되었으며 과거 설계 기록은 현재 provider API가 아니다.

- [ABI_POLICY.md](ABI_POLICY.md)
- [ASYNC_GUIDE.md](ASYNC_GUIDE.md)
- [Issue #5](https://github.com/fox1245/NeoGraph/issues/5) — historical decision; superseded provider compatibility policy.

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

> Stage 3(2026-04) 설계와 당시 측정 테스트 수는 역사로 보존한다. provider 호환/crossover 결정은 아래 typed lossless 전환으로 대체되었으며 과거 설계 기록은 현재 provider API가 아니다.

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
