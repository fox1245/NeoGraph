<!-- neograph-i18n: source=ROADMAP_v1.md locale=ko source_sha256=875b4e187558a80c736ec1c41c98cf9d7cf9aa19e84238e35b7f790e6b935563 -->
# NeoGraph v1.0 — 설계 개선 로드맵

**Languages:** [English](ROADMAP_v1.md) | [한국어](ROADMAP_v1.ko.md) | [日本語](ROADMAP_v1.ja.md) | [简体中文](ROADMAP_v1.zh-CN.md)

이 파일은 v1 이전의 리팩터링 주기에서 나온 설계 제안과 반영 기록을 보존합니다.
기록 안의 버전 이름, 코드 스케치, 호환성 정책, 테스트 수, 측정값은 당시의 상태를 설명할 뿐이며
릴리스 일정이나 현재 API 명세가 아닙니다.

## 현재 계약

사용자 정의 노드는 `GraphNode::run(NodeInput) -> asio::awaitable<NodeOutput>`을 구현합니다.
`RunContext`는 실행별 메타데이터를 전달하고, `CancelToken::fork()`는 자식 취소를 제공합니다.
Provider는 이제 `get_name()`, `family()`, `prepare()`를 제공합니다. 공통 invoke/dispatch 경로는
소유권이 있는 요청과 이동 전용(move-only) prepared 핸들을 받아 처리하고, 변경할 수 없는 SDK Outcome을 보관합니다.
예전의 `CompletionProvider`, `CompletionRequest`, `CompletionParams`, `ChatCompletion`, `complete*`,
`OpenAIProvider`, `RateLimitedProvider`는 호환용 shim 없이 제거되었습니다.
아래 7월의 추가 호환 정책은 이 전환으로 대체되었습니다.

LLM 기능을 꺼도 Core에는 외부 `SchemaProvider::runtime`이 필요합니다.
NeoGraph `0.13.1`은 alpha SDK `0.1.1`(인터페이스 리비전·공유 세대 4)을 사용하므로,
소비자는 맞는 헤더와 라이브러리로 다시 빌드해야 합니다. 네이티브 archive는 계속 v3 / `spna3`이고
portable JSON은 v2입니다. 현재 버전의 통합 검증은 아직 끝나지 않았습니다.
Python은 타입이 있는 request/prepared/Outcome 객체를 사용하며, `ChatMessage`는 그래프용 편의 타입으로 따로 유지됩니다.
현재 API는 [C++ 레퍼런스](docs/reference-en.md)와 [Python 바인딩](docs/python-binding.md)을 참고하세요.
Linux, ARM64, WASM에서 과거에 한 관찰은 그 플랫폼에서 현재 SDK 런타임이 동작함을 보증하지 않으며,
애플리케이션 성능을 예측하는 근거도 아닙니다.

## 이 파일이 있는 이유

v0.3.x의 취소 전파 시리즈는 5라운드(v0.3.0 단일 노드, v0.3.1 다중 Send 포인터, v0.3.1+ 프로세스 내 폴링,
v0.3.2 Python용 훅, v0.3.2 C++ 스코프+재시도+예외 타이핑)에 걸쳐 패치가 5개 필요했지만, 논리적으로는 하나의 수정이었습니다.
**같은 횡단 관심사(취소)를 디스패치 진입점 약 8곳과 진입 언어 2개(C++/Python)에 모두 연결해야 했기** 때문입니다.
패치마다 진입 경로 하나를 막았지만 나머지는 열려 있었습니다.

버그의 패턴은 거의 "아키텍처가 잘못됐다"가 아니라 "올바른 패턴이 N곳 중 M곳에만 적용됐다"였습니다.
버그는 예외 케이스로 잡혔을 뿐 모델 자체를 의심할 일은 없었으므로, v0.3.x 시리즈는 *핵심* 설계
(Pregel BSP 슈퍼스텝, 채널 리듀서, Send/Command 동적 디스패치, 전반에 걸친 asio 코루틴)를 검증한 셈입니다.

이 시리즈가 실제로 드러낸 것은 현재 설계에서 인지 부하가 큰 지점 세 곳입니다.
이 지점들 때문에 구현을 N곳에 나눠 넣는 일이 실수하기 쉬워집니다. 아래 후보는 각각 한 지점을 다룹니다.

---

## 후보 1 — 태그 기반 라우팅을 쓰는 단일 디스패치 진입점

### 증상

`GraphNode`는 가상 메서드 8개를 제공합니다.

```
execute            execute_async
execute_full       execute_full_async
execute_stream     execute_stream_async
execute_full_stream execute_full_stream_async
```

이 메서드들은 `(sync/async) × (writes/full) × (stream/non-stream)`의 곱집합을 이룹니다.
기본 구현이 서로를 거쳐 연결되므로 우선순위가 일관되게 유지되어야 합니다.
기본 구현을 한 단계 거칠 때마다 버그가 숨을 수 있습니다.

- v0.3.1 #2: 힌트 메시지가 스트리밍 변형을 언급하지 않았습니다.
- v0.3.2 #10 (Python): `PyGraphNode::execute_full_stream`이 `execute_stream` 분기를 건너뛰어,
  스트리밍 전용 노드에서는 `run_stream`이 쓸모없었습니다.
- v0.3.2 #10 (C++): `GraphNode::execute_full_stream`의 기본 구현이 `execute_full`을 먼저 호출했고,
  그 결과 `ExecuteDefaultGuard` 재귀가 예외를 던져 `execute_stream`에 도달하지 못했습니다.
- `execute_full_stream_async`에는 GCC-13 코드 생성 우회책이 필요했습니다.
  `co_await` 주변의 `catch(T&)`가 예외를 조용히 놓치기 때문입니다.

### 개선안

가상 디스패치를 하나로 줄입니다.

```cpp
class GraphNode {
public:
    virtual NodeOutput run(const NodeInput& in) = 0;
};

struct NodeInput {
    const GraphState&    state;
    const RunContext&    ctx;          // see Candidate 2
    GraphStreamCallback  stream_cb;    // null if non-stream
    bool                 is_async;     // hint, not a hard contract
};

struct NodeOutput {
    std::vector<ChannelWrite> writes;
    std::optional<Command>    command;
    std::vector<Send>         sends;
};
```

사용자는 메서드 하나만 오버라이드합니다. 동기/비동기 구분은 엔진이 처리합니다.
엔진은 동기 오버라이드를 run_sync로 감싸고 비동기 오버라이드는 곧바로 await하지만,
이는 사용자가 아니라 엔진이 신경 쓸 일입니다. 스트리밍 구분은 `stream_cb`가 null이 아니면
스트리밍을 기대한다는 뜻이며, 사용자는 이를 쓰거나 무시하면 됩니다. Command/Send는 필드를 채우기만 하면 됩니다.

마이그레이션: 가상 함수 8개는 한 릴리스 동안 deprecated된 얇은 shim으로 남겨 둡니다.
새 코드는 `run()`을 오버라이드합니다. 트램펄린(`PyGraphNode`)은 한 줄로 줄어듭니다.

### 비용

- 공개 API가 깨집니다. 기존 GraphNode 서브클래스는 모두 `run()`으로 다시 작성해야 합니다.
- `RunContext`(후보 2)가 반드시 먼저 있어야 합니다. 없으면 `run()`이 실행별 메타데이터를 전달할 수 없습니다.
- 엔진 내부의 디스패치 로직은 단순해지지만, 동기와 비동기 중 어느 쪽으로 실행할지는
  엔진이 런타임 힌트나 관례로 골라야 합니다.

---

## 후보 2 — 실행별 메타데이터를 위한 명시적 `RunContext`

### 증상

현재 호출자가 설정할 수 있는 실행별 "메타데이터"는 `RunConfig::cancel_token`뿐입니다.
엔진은 이를 두 가지 방식으로 몰래 끼워 넣습니다.

1. `GraphState::run_cancel_token_` — GraphState에 있지만 **채널 집합에는 없는** 멤버라서
   `serialize()`가 이를 잃어버립니다.

   - v0.3.1의 다중 Send 수정: `init_state(send_state) + send_state.restore(snapshot)`가 worker별 상태를
     다시 만들었지만, `run_cancel_token_`은 채널 집합 밖에 있어서 사라졌습니다.
     그래서 모든 Send fan-out worker에 `send_state.set_run_cancel_token(parent.run_cancel_token_shared())`를
     명시적으로 호출해야 했습니다.
   - 다음 실행별 필드(deadline? trace_id? metric handle?)를 추가하는 사람도 똑같은 버그를 겪게 됩니다.

2. `current_cancel_token()` thread_local — execute_full_async 진입 시 `CurrentCancelTokenScope`가 설정합니다.

   - v0.3.2의 C++ 수정: PyGraphNode는 이 스코프를 설치했지만, 네이티브 C++ `GraphNode::execute_full_async`의
     기본 구현은 설치하지 않았습니다. 그래서 다중 Send C++ worker의 `Provider::complete`가 thread-local에서
     null을 보았고, run_sync가 취소 바인딩 없이 실행되었습니다. 7초 분량의 비용이 새어 나갔습니다.
   - 디스패치 진입점을 새로 만들 때마다 스코프 설치를 기억해야 합니다. 잊으면 기능이 조용히 깨집니다.

두 방식 모두 실행별 메타데이터를 둘 정식 위치가 없어서 생긴 우회책입니다.

### 개선안

`GraphState`와 함께 `RunContext`를 모든 디스패치에 명시적으로 전달합니다.

```cpp
struct RunContext {
    std::shared_ptr<CancelToken>  cancel_token;
    std::optional<Deadline>       deadline;
    std::string                   trace_id;
    std::string                   thread_id;
    int                           step;
    StreamMode                    stream_mode;
    // ... extension point for future cross-cutting concerns
};

class GraphNode {
public:
    virtual NodeOutput run(const NodeInput& in) = 0;
    // in.ctx is the RunContext — no thread_local, no
    // serialize-loses-it. Every dispatch path threads it explicitly.
};
```

`Provider::complete(params, ctx)`도 컨텍스트를 받습니다. thread_local도, `current_cancel_token()`도 없어집니다.
Send fan-out worker는 `ctx`를 값으로 복사합니다. shared_ptr 하나와 문자열 몇 개라서 비용이 적습니다.

### 비용

- 공개 API가 깨집니다. 모든 Provider, GraphNode, Tool이 영향을 받습니다.
- 어디서나 `state, ctx`를 받아야 하므로 시그니처가 넓어집니다.
- 하지만 취소, trace, deadline을 전달하는 것을 잊는 부류의 버그를 통째로 없앱니다.
  시그니처 하나, 필드를 추가할 곳 하나이며 우회책이 필요 없습니다.

### 이 구조였다면 막을 수 있었던 v0.3.x 버그

- v0.3.1의 다중 Send 포인터 누락: ctx가 직렬화되지 않는 멤버에 묻히지 않고 명시적인 필드가 됩니다.
- v0.3.2의 C++ thread_local 누락: thread_local 자체가 없습니다.
- 앞으로 생길 deadline, trace_id, metric 누수: 형태가 같으므로 같은 예방 효과가 있습니다.

---

## 후보 3 — 계층형 / 소비자별 CancelToken

### 증상

`CancelToken`은 `cancellation_signal sig_` 하나와 `bind_executor` 슬롯 하나를 전제로 설계되었습니다.
asio의 `cancellation_slot`은 핸들러를 하나만 받아서 마지막에 `bind_cancellation_slot`을 호출한 쪽이 이깁니다.
소비자가 동시에 여럿인 경우(다중 Send fan-out worker가 각자 Provider::complete → 내부 run_sync →
bind_cancellation_slot을 호출)에는 서로의 바인딩을 조용히 덮어썼고, 마지막에 바인딩한 HTTP 요청만 취소되었습니다.

v0.3.2는 이 단일 시그널 설계 위에 `add_cancel_hook` 목록을 덧붙였습니다.
중첩된 run_sync가 각자 전용 시그널을 갖고, 부모의 `cancel()`이 훅 목록을 순회하며 이를 발동하는 방식입니다.
동작은 하지만, 소비자가 하나라는 전제로 만든 프리미티브를 소비자가 N개인 상황에 쓰면서 보정하는 모양새입니다.
게다가 emit과 bind 사이에 경쟁 상태가 있습니다. add_cancel_hook을 호출할 때 이미 취소된 상태이면,
동기 발동이 co_spawn이 슬롯을 바인딩하기 전에 emit을 게시해서 그 emit이 유실됩니다.
v0.3.2는 이를 피하려고 run_sync 진입 시점에 `is_cancelled()`를 먼저 확인해 건너뛰는 단락 처리를 넣었는데,
패치 위에 패치를 얹은 셈입니다.

### 개선안

계층형 취소로 바꿉니다.

```cpp
class CancelToken {
public:
    /// Create a child token. Parent.cancel() cascades to child.
    /// Each child has its OWN cancellation_signal — no
    /// single-consumer assumption.
    std::shared_ptr<CancelToken> fork();

    /// Cancel this token (and recursively all children).
    void cancel();

    bool is_cancelled() const noexcept;
    asio::cancellation_slot slot();  // each token has its own
    void bind_executor(asio::any_io_executor ex);
};
```

`run_sync(aw, parent_token)`는 매번 다음과 같이 동작합니다.

```cpp
auto child = parent_token->fork();
child->bind_executor(io.get_executor());
asio::co_spawn(io, body(),
    asio::bind_cancellation_slot(child->slot(), asio::detached));
```

add_cancel_hook 목록을 덧붙일 필요가 없습니다. emit과 bind 사이의 경쟁도 없습니다
(자식을 새로 만들고, 시그널을 먼저 바인딩하고, fork()가 부모 상태의 스냅샷을 찍습니다).
다중 Send fan-out에서는 형제 토큰이 3개 생기고, 부모가 취소하면 셋이 모두 취소됩니다.

참고한 선례는 Go의 `context.Context` 취소와 asio의 `asio::cancellation_state` / `make_cancellation_filter`
(asio가 알맞은 API를 갖추게 되면)입니다. 잘 알려진 패턴입니다.

### 비용

- CancelToken의 공개 API가 바뀝니다(`fork()`가 새로 생기는 것이므로 추가 방식입니다).
  기존 `add_cancel_hook`은 deprecated됩니다.
- 내부적으로 모든 `run_sync(aw, cancel)`이 `run_sync(aw, cancel->fork())`가 됩니다.
- 결과적으로 프리미티브 하나가 "단일 시그널 + 훅 목록 + 선제 취소 단락 처리 + 소비자별 경쟁 상태 메모"를 대신합니다.

---

## 횡단 관찰

세 후보는 서로 맞물립니다. 후보 2는 후보 3의 토큰을 디스패치 경로로 운반하고,
후보 1의 단일 `run()`은 취소 자식 토큰이 담긴 `RunContext`를 자연스럽게 받습니다.

하나만 반영한다면 후보 2를 택하는 편이 좋습니다. 모든 디스패치에 연결해야 하는 것들에서
반복되던 버그의 가장 큰 부류를 없애기 때문입니다.

추적: v0.3.x 패치 라운드에서 새로운 구조적 이음새가 드러나거나 후보가 반영되면 이 파일을 갱신합니다
(취소선을 긋고 병합 커밋 링크를 겁니다).

---

## 패턴 회고 — 하위 소비자의 발견 9건 (이슈 #36)

ProjectDatePop의 `cpp_backend`가 v0.5 → v0.8 기간에 수행한 스트레스 테스트에서 NeoGraph에 대한 발견이 9건 나왔습니다.
**9건 중 적어도 7건은 같은 구조적 패턴에서 비롯되며**, 후보 1 + 6은 이 패턴을 하나씩 고치는 방식이 아니라
*패턴이 다시 생길 수 있는 표면을 없애는* 방식으로 막습니다.

### 공통 패턴

> **"X는 Y일 때만 안전합니다." 그런데 Y라는 전제 조건이 docstring에 적혀 있지 않고, 컴파일 타임에 강제되지 않으며,
> 어겨도 런타임에 드러나지 않습니다. 기본 경로는 조용히 잘못 동작하고, 대개 입력의 곱집합 중 특정 모서리에서만 그렇습니다.**

| # | 숨은 조건부 불변식 |
|---|---|
| #4 | `Provider::complete_stream_async`의 기본 브리지는 네이티브 동기 `complete_stream`이 스스로 `run_sync`를 쓰지 **않을 때만** 안전하다. `SchemaProvider`의 WS 경로가 이를 조용히 어겼다. |
| #5 | `Provider`의 가상 함수 4개 곱집합은 선택한 오버라이드 표면이 우연히 브리지 중첩을 피할 때**만** 안전하다. 이 불변식은 `provider.h`만 봐서는 보이지 않는다. |
| #6 | `schema_mutex_`와 on_chunk 락은 사용자 콜백이 SchemaProvider에 재진입하지 않을 때**만** 안전하다. 수정 전에는 문서에 없었다. |
| #9 | C++ openinference와 동등해야 했다. Python 래퍼가 콜백 스레드 신원에 대해 숨은 가정을 두었는데, 이 가정이 C++로 옮겨지지 않았기 때문이다. |
| #16 | NeoGraph에 번들된 cpp-httplib는 모든 소비자 TU가 `CPPHTTPLIB_OPENSSL_SUPPORT`를 정의할 때**만** 올바르다. 그렇지 않으면 조용히 ODR 위반이 된다. |
| #34 | `extra_fields`는 `params.tools`가 비어 있지 않을 때**만** 적용된다. 도구가 없는 호출에서는 reasoning 필드가 조용히 버려졌다. |
| #35 | `temperature`는 `params.temperature ≥ 0`일 때**만** 전송된다. 그런데 스키마에는 "이 provider는 temperature를 아예 받지 않는다"고 선언할 방법이 없어서, 모든 호출 지점이 기본값을 부정해야 했다. |

나머지 두 건(#17 문서 공백, #33 호출별 바인딩 공백)은 숨은 불변식의 함정이 아니라 부족한 부분에 대한 보고입니다.
다만 근본 원인은 같습니다. 추상화가 정적인 표면은 선언했지만 그에 해당하는 동적인 표면은 제공하지 않았습니다.

### 후보 1 + 6이 사례가 아니라 *부류*를 막는 이유

위의 발견은 각각 문제가 된 특정 오버라이드 지점을 **겨냥한 패치**(PR #10, PR #11, PR #12, PR #19, PR #20, PR #37, PR #37)로
해결되었습니다. 그 패치들은 *패턴을 허용한 표면*, 즉 GraphNode 가상 함수 8개, Provider 가상 함수 4개, 스키마 build_body의
분기 트리를 그대로 남겼습니다. 다음 소비자나 다음 벤더 스키마, 또는 기본 구현 하나를 바꾸는 다음 리팩터링이
같은 곱집합의 새로운 모서리에서 또 다른 "X는 Y일 때만 안전하다"를 찾아낼 것입니다.

후보 1과 6은 이 곱집합을 각각 **가상 함수 하나**로 줄입니다. 반영되고 나면 다음과 같습니다.

- **후보 1** (GraphNode 8 → 1): 어떤 가상 함수를 오버라이드하느냐에 따라 브리지가 안전한지가 갈리는 결정이 더는 없습니다.
  사용자는 `run(NodeInput)`을 오버라이드합니다. 동기/비동기, 스트림/비스트림, 쓰기/전체 결과는 모두 본문 형태의 선택이며,
  가상 함수의 종류에 묶인 숨은 불변식은 없습니다.
- **후보 6** (Provider 4 → 1): 새 구현은 `CompletionProvider::do_invoke()`를 유일한 오버라이드 지점으로 씁니다.
  기존 `Provider` 표면은 호환성을 위해 안정적으로 유지하고, 새 경로에는 명시적인 요청 모드 하나와 drain 패턴 하나가 있습니다.

나머지 2건(#9 스레드 신원, #16 ODR 매크로)은 후보 1 + 6으로 해결되지 *않습니다*. 이들은 별개의 문제 부류
(관측 계층의 동등성, 빌드 시스템 관례)입니다. #9는 이미 해결되었습니다(PR #12 + 동등성 테스트).
#16은 이제 컴파일 타임에 막습니다(v0.8.0 `api.h`).

### 이 회고에서 여전히 중요한 점

이 9건은 **프로젝트의 나이와 상관없이** 드러났을 것입니다. 장기 운영 배포도, 특이한 벤더도 필요하지 않았습니다.
소비자 한 곳(ProjectDatePop)이 약 3주 동안 현실적인 에이전트 흐름을 작성하는 과정에서 나왔습니다.
후보 1 + 6이 없으면, 비슷한 깊이로 쓰는 다음 소비자가 같은 형태의 발견을 5~10건 더 올릴 것입니다.
후보 1 + 6이 있으면 이 부류는 닫힙니다.

이것이 v0.x의 외형 정리보다 **v1.0 주기에서 후보 1 + 6을 우선해야 하는** 구조적 근거입니다.
새로 발견된 "X는 Y일 때만 안전하다" 건마다 그 패치 비용은 감수할 만했지만,
7건에 든 노력을 합치면 이미 후보 1 + 6의 예상 비용보다 큽니다.

### v0.x deprecation 기간의 완화책

후보 1 + 6이 반영되기 전까지는 불변식이 현재 존재하는 자리에서 고정해 둡니다.

- 예전 가상 함수 8개에 `[[deprecated]]`를 붙이고 `docs/migration-v0.4-to-v1.0.md`를 둡니다 — v0.4 / v0.8에 반영.
- "Y일 때만 안전하다"는 전제 조건이 있는 모든 오버라이드 지점에 `@warning` 블록을 둡니다
  (예: `Tracer::start_span`, `OpenInferenceTracerSession::close`).
- 언어로 표현할 수 있는 TU 간 불변식에는 컴파일 타임 `#error` 가드를 둡니다
  (예: `CPPHTTPLIB_OPENSSL_SUPPORT` 매크로 일관성 — v0.8에 반영).
- 불변식을 어겼을 때 그 불변식을 이름으로 알려 주는 친절한 런타임 오류를 둡니다
  (예: `Unknown reducer: 'foo'. Available: ...` — v0.8에 반영).

이런 조치는 패턴이 문제를 일으키는 범위를 좁힐 뿐 부류 자체를 닫지는 못합니다. 후보 1 + 6은 부류를 닫습니다.

---

## 상태

| # | 후보 | 상태 | 계기가 된 라운드 / 이슈 |
|---|---|---|---|
| 1 | 단일 `run()` 디스패치 + 태그 | **v0.9.0에 반영.** `run(NodeInput)`이 순수 가상 함수가 되었고, 예전 가상 함수 8개와 대체(fallback) 체인은 사라졌다. | v0.3.1 #2, v0.3.2 #10 (2개 언어). #36(하위 소비자의 발견 9건)으로 근거가 강화됨 |
| 2 | 명시적 `RunContext` 인자 | **v0.4–v0.8에 반영** (`RunContext::store` 필드는 v0.8 #27에서 추가) | v0.3.1 다중 Send, v0.3.2 C++ 스코프 |
| 3 | 계층형 CancelToken | **v0.4에 반영** (`CancelToken::fork()` + 연쇄 취소) | v0.3.2 훅, v0.3.2 emit-vs-bind |
| 4 | 자기 진화 그래프 런타임 훅 | 연구 | TODO_v0.3.md #8 |
| 5 | pgvector RAG 예제 | 쿡북 | TODO_v0.3.md #9 |
| 6 | Provider 단일 디스패치 | **타입이 있는 prepare/dispatch로 대체됨.** 예전 completion 표면과 추가 어댑터는 shim 없이 제거되었다. 위의 현재 계약을 참고. | #4 (v0.7에서 닫힘), #5 (과거의 호환 정책), #36으로 근거가 강화됨 |

---

# 실행 계획

> **상태:** 후보 1은 완료되었습니다. 아래 계획은 v0.4.0부터 파괴적 변경이 들어간 v0.9.0(v1 준비 릴리스)까지
> 마이그레이션을 어떻게 단계적으로 진행했는지 기록한 것입니다. 과거의 맥락일 뿐 남은 작업이 아닙니다.

## 사용자 관점의 동기

버그 부류라는 관점은 잠시 접어 두겠습니다. 지금 **README를 처음 여는 사용자**에게 표면은 흩어져 보입니다.

  - "노드는 어떻게 작성하나?" — 가상 함수가 8개입니다(`execute` / `execute_async`
    / `execute_full` / `execute_full_async` / `execute_stream` /
    `execute_stream_async` / `execute_full_stream` /
    `execute_full_stream_async`). 어느 것을 골라야 하는지 물으면 답은 "Send/Command 사용 여부, 동기/비동기,
    스트리밍/비스트리밍에 따라 다르다"입니다. 서로 직교하는 세 축을 처음부터 따져야 합니다.
  - "취소는 어떻게 하나?" — `RunConfig::cancel_token`이 있지만, 취소가 LLM까지 전달되려면
    (a) 엔진이 thread_local 스코프를 설치하고, (b) Provider::complete가 이를 읽고,
    (c) run_sync가 훅을 등록하고, (d) worker가 재시도하지 않아야 합니다.
    이 내용은 한곳에 모여 있지 않습니다.
  - "상태는 어떻게 갱신하나?" — v0.3.2에서는 `dict | list[ChannelWrite]`입니다.
    그 전에는 README가 한 가지 형태를 문서화했는데 바인딩은 다른 형태를 조용히 무시했습니다.
    새 사용자는 "내 쓰기가 왜 반영되지 않았지?"라는 의문을 만나 디버깅해야 했습니다.
  - "상태는 어떻게 읽나?" — 중첩된 `state["channels"][name]["value"]`,
    평평한 `engine.get_state_view(thread_id).<channel>`,
    타입이 있는 Pydantic 서브클래스가 있습니다. 올바른 답이 세 가지인데 표준은 하나도 없습니다.
  - "그래프는 어떻게 실행하나?" — `run`(동기) 대 `run_async` 대 `run_stream` 대 `run_stream_async` 대
    `resume` 대 `resume_async`. 진입점이 여섯 개이고, 여기서도 여러 축이 곱집합을 이룹니다.

**추가한 것 하나하나에는 이유가 있었습니다**(resume_if_exists는 실제 채팅 의미론이고, StateView는 실제로 사용성을 높입니다).
하지만 **누적된 결과는, 한 가지 일을 하는 방법이 문서, 예제, 바인딩 코드에 걸쳐 2~4가지로 흩어진 표면입니다.**
v0.3.x 패치가 계속 쌓였고, v0.3.x의 취소 라운드(5번)는 이런 파편화가 버그가 숨는 자리이기도 하다는 점을 보여 주었습니다.
"올바른 방법"이 N곳 중 M곳에만 있으면, 빠진 곳에서는 아무 일도 하지 않고 조용히 넘어가거나 패턴을 잊은 버그가 생깁니다.

구조 개선(후보 1~3)은 이를 다음과 같이 줄입니다.

  - **노드를 쓰는 방법은 하나**(`run(NodeInput) -> NodeOutput` + 태그).
  - **실행별 메타데이터를 전달하는 방법은 하나**(`RunContext` 인자).
  - **취소하는 방법은 하나**(중첩된 작업에는 `token->fork()`, 부모가 취소하면 모두 취소).
  - **상태를 읽는 방법은 하나**(StateView가 표준이고 raw dict는 탈출구).
  - **실행하는 방법은 하나**(run / run_async 등을 스트림 콜백을 받거나 이터레이터를 반환하는
    메서드 하나로 합침).

이것이 v1.0 계약이며, 문서 페이지도 다시 짧아집니다.

## 버전 전략

| 버전 | 범위 | 공개 API |
|---|---|---|
| **v0.4.x** | RunContext가 *새* 매개변수로 들어온다. 기존 메서드는 deprecated되지만 계속 동작한다. CancelToken에 `fork()`가 추가된다. 새 `run(NodeInput)`이 추가로 들어온다. | 두 API 모두 호출 가능. Deprecation 경고. |
| **v0.5.x** | 예제와 pybind 바인딩이 새 API로 이전한다. 기존 API는 계속 deprecated. | 두 API 모두 호출 가능. 더 강한 deprecation 경고와, 새 API로 안내하는 문서. |
| **v1.0.0** | 기존 API(가상 함수 8개, thread_local 스코프, 스스로 시그널을 받는 단일 핸들러 CancelToken)를 제거한다. | 정식 API 하나. |

근거: **v0.4에서 v1.0으로 한 번에 건너뛰지 않습니다.** deprecation 기간을 두 릴리스로 잡으면
하위 소비자(neoclaw, NeoProtocol Executor, WASM 스파이크, 이 저장소 밖의 모든 것)가 컴포넌트를 하나씩 이전할 수 있습니다.
이 기간에도 cibuildwheel 매트릭스는 그대로이고 릴리스 경로당 wheel 20개도 변하지 않으며,
예전 메서드에 대한 의존만 서서히 줄어듭니다.

이전이 예상보다 오래 걸리면(예: 서드파티 C++ GraphNode 서브클래스가 흔한 경우) v0.5는 deprecation을 연장한 v0.5.x가 되고
v0.6까지 기간을 늘립니다. 예전 API는 deprecation 경고가 한 릴리스 동안 조용했을 때만 제거합니다.

## PR 순서

각 행은 병합 가능한 PR 하나입니다. PR은 순서대로 모두 master에 반영합니다.
오래 가는 기능 브랜치는 없습니다. 프로젝트의 커밋 이력은 직선형이고, deprecation 전략 덕분에
PR마다 독립적으로 PyPI에 v0.4.0+i, v0.4.0+(i+1) 식으로 배포할 수 있기 때문입니다.

| # | PR | 범위 | 반영 버전 |
|---|---|---|---|
| 1 ✓ | **`RunContext` 연결(내부)** — `a473f0e`에 반영 | `engine.h`에 `struct RunContext`를 추가한다. 엔진의 `execute_graph_async`가 이를 만들어 디스패치 경로 전체로 전달한다. NodeExecutor는 이를 `execute_full_async`에 넘긴다. Pybind가 이를 감싼다. **공개 API는 달라지지 않는다.** 기존 메서드는 여전히 `state`만 받고, 새 `ctx`는 디스패치 경로 안에서 나란히 존재한다. ctest 442/442, pytest 96/96 통과. 벤치마크 중앙값은 5.365 µs(BASE 5.285 µs, +1.5%)로, 과거 잡음 수준인 약 3% 안이며 기준 5.185 µs에서 ±5% 범위 안이다. PR 1에는 pybind 변경이 없으므로 Pybind 래핑은 PR 7(바인딩 이전)로 미뤘다. | v0.4.0 |
| 2 ✓ | **`GraphNode::run(NodeInput) -> NodeOutput`** — `607ce66`에 반영 | GraphNode에 새 가상 함수를 추가한다. 기본 구현은 기존 가상 함수 8개로 위임한다(우선순위 순서는 유지). 엔진이 우선적으로 쓰는 디스패치 진입점으로 등록된다. 기존 C++ 서브클래스는 기본 대체 경로 덕분에 그대로 컴파일되고 동작한다. ctest 442 → 445(NodeRunDispatch 테스트 3개 추가), pytest 96/96, 실제 LLM/WS 5건 통과. 벤치마크 중앙값은 6.122 µs로 PR 1 BASE 6.160 µs 대비 Δ -0.6%(A/B 10라운드)이다. PR 1 BASE는 코드가 같은데도 5.285 → 6.160으로 흔들렸으므로, A/B 비교로 측정 드리프트를 통제했다. **발견한 함정**: pybind 비동기 경로에서 ``run(const NodeInput&)``이 asio executor 안에서 SEGV를 일으켰다(코루틴 참조 매개변수 UAF로, v0.2.0 RunConfig 크래시와 같은 형태). 수정: ``NodeInput``을 값으로 받는다. node.h에 문서화했다. | v0.4.0 |
| 3 ✓ | **CancelToken `fork()` 추가** — `897645c`에 반영 | `std::shared_ptr<CancelToken> CancelToken::fork()`를 추가한다. 부모의 `cancel()`이 자식으로 전파된다. `add_cancel_hook`은 계속 동작한다(deprecated 처리하며, `[[deprecated]]` 표기는 PR 4에서 들어간다). `run_sync(aw, cancel)`은 `cancel->fork()`를 쓰도록 바뀐다. 엔진의 바깥쪽 co_spawn을 위해 단일 시그널 `slot()` API는 그대로 둔다. ctest 445 → 452(CancelTokenFork 테스트 7개 추가), pytest 96/96, 실제 LLM/WS 5건 통과. 벤치마크 A/B 20라운드(양방향 교차): Δ 최솟값 +1.0%, Δ 중앙값 +1.5%로 ±5% 범위 안이다. 벤치마크 경로에는 `cancel_token`이 없어 `fork()`를 거치지 않으므로, 작은 차이는 바이너리 레이아웃 잡음이다(PR 3의 벤치마크 바이너리는 PR 2보다 3.7KB 작아 레이아웃이 다르다). | v0.4.0 |
| 4 ✓ | **Deprecation 표기** — `35a4517`에 반영 | 기존 가상 함수 8개와 `add_cancel_hook`에 `[[deprecated]]`를 붙인다(이 함수가 반환하는 Hook은 간접적으로 deprecated된다). 트램펄린 스코프(`CurrentCancelTokenScope` / `current_cancel_token()`)는 미뤘다. 이 스코프는 몰래 끼워 넣는 통로이고 PR 7(바인딩 이전)이 `ctx.cancel_token` 읽기로 대체할 대상이므로, 지금 deprecated로 표시하면 명확한 이전 경로 없이 모든 사용 지점에서 경고를 억제해야 한다. 내부 호출 지점(graph_node.cpp의 기본 체인, 기본 `run()` 전달자)은 새 `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` 매크로(api.h, GCC/clang/MSVC 이식 가능)로 감싼다. deprecated된 가상 함수를 오버라이드하거나 `add_cancel_hook`을 호출하는 사용자 코드에는 이전 경고가 나오고, 엔진 내부는 깨끗하게 유지된다. ctest 452/452, pytest 96/96, 실제 LLM/WS 5건 통과. 벤치마크 A/B 10라운드: Δ 중앙값 +0.3%, 최솟값 +0.8%로, 속성만 바꾼 변경이라 레이아웃 잡음이다. `-Werror=deprecated-declarations`는 켜지 않았다(CI에는 원래 `-Werror`가 없었고, 경고는 deprecation 기간 내내 참고용으로 남는다). | v0.4.0 |
| 5 ✓ | **StateView를 표준으로, raw dict는 deprecated** — `f31aa53`에 반영 | pybind docstring에서 `engine.get_state(thread_id) -> dict`를 soft-deprecated로 표시한다. 새로운 표준은 `get_state_view(thread_id) -> StateView`이다(이미 v0.3.2에 있다). `DeprecationWarning`도 `[[deprecated]]` 표기도 내지 않는다. raw dict에는 정당한 용도(채널별 `version` 접근, 스냅샷 직렬화)가 있기 때문이다. soft-deprecation이 큰 반응을 일으키지 않으면 v1.0에서도 탈출구로 남긴다. 동작은 전혀 달라지지 않는다. ctest 452/452, pytest 96/96 통과. | v0.4.0 |
| 6 ✓ | **예제 이전** — `a2a24ef`(PR 6a, C++)와 `0a76e3a`(PR 6b, Python)에 반영 | C++ 예제 7개와 Python 예제 19개(GraphNode 서브클래스 총 44개)를 통합 ``run(NodeInput)`` API로 바꿨다. PR 6a는 손으로 이전했고, PR 6b는 AST 범위를 지정한 도우미로 안전하게 일괄 수정했다. 스모크 실행 결과는 v0.3.2 출력과 비트 단위로 일치한다. ctest 452/452, pytest 96/96 통과. | v0.4.x (6a와 6b로 분리) |
| 7 ✓ | **Pybind 바인딩 이전** — `4e186a5`에 반영 | ``PyGraphNodeOwner``가 이제 ``GraphNode::run(NodeInput)``을 오버라이드하고, `has_user_method` MRO 탐색으로 Python 사용자의 ``run`` 메서드에 디스패치한다. 그 메서드가 없으면 기존 체인으로 넘긴다. ``RunContext`` / ``NodeInput`` / ``CancelToken``을 Python에 바인딩했다(패키지에서 다시 내보낸다). 몰래 끼워 넣는 ``CurrentCancelTokenScope``는 **유지한다.** 아직 이전하지 않은 사용자 코드를 위해 기존 체인이 계속 설치하기 때문이다. PR 9가 기존 가상 함수 8개와 함께 이를 삭제한다. ctest 452/452, pytest 96/96, 실제 LLM/WS 5건 통과. 새 ``run(input)`` API를 끝까지 실행해 확인했다. | v0.4.x |
| 8 ✓ | **문서 재작성** — `519a00b`에 반영 | `docs/reference-en.md` §6 GraphNode를 `run()` 가상 함수 하나로 줄이고, §7에 RunContext와 CancelToken(`fork()` 예시 포함) 하위 절을 새로 넣었다. README의 "LangGraph와의 차이" 절에 `run(input)`을 가리키는 "노드 메서드는 하나" 항목을 추가했다. `@ng.node` 데코레이터의 내부 `_DecoratorNode`가 이제 `run()`을 쓰므로, 5초 데모도 새 경로로 실행된다. concepts.md와 troubleshooting.md 정리는 PR 9로 미뤘다(기존 체인이 삭제되면 두 문서가 분명히 낡게 되기 때문이다). | v0.5.0 |
| 9 ✓ | **기존 API 제거** — 내장 마이그레이션 `d1070dc`, 기존 GraphNode 체인 `19819d8`, 취소 훅 `1d786a5`, thread-local/Python 기존 브리지 `9e8e956`, 불필요해진 Python 테스트 `4392fbb`. | v0.9.0 |

## 완료된 v0.4.0 이후 계획 (과거 기록)

v0.4.0은 2026-05-05에 배포되었습니다(`4cae42c`, 태그 `v0.4.0`). 아래에 설명하는 관찰 기간과 파괴적 제거는
모두 끝났고, v0.9.0이 2026-05-14에 제거를 배포했습니다.

### 단계 A — deprecation 기간 (완료)

기간: 몇 주, 마이너 주기 하나 정도. 엔진 코드는 바꾸지 않습니다. 이 단계는 v1.0이 해당 코드를 삭제하기 전에,
deprecation 경고가 하위 소비자의 실제 문제를 드러낼 시간을 주기 위한 것입니다.

살펴볼 항목은 다음과 같습니다.

  1. **Deprecation 가시성** — 사용자가 기존 가상 함수 8개와 `add_cancel_hook`에 붙은 `[[deprecated]]` 경고를
     실제로 보고 있는가? PR 4(`35a4517`)는 내부 호출 지점을 `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED`로 감쌌으므로,
     경고는 사용자의 오버라이드 지점에서**만** 나와야 합니다. 이슈 트래커, 토론, 직접 피드백 채널에서
     "이 경고가 뭔가요?"라는 언급이 있는지 확인합니다.
  2. **기존 체인 회귀** — 기존 가상 함수 8개의 기본 체인이 깨지는 새로운 사례(조용히 아무 일도 하지 않음,
     잊힌 스코프 등)가 발견되는가? v0.3.x에서 이런 일이 5라운드 있었고, 한 번 더 생길 가능성이 있습니다.
  3. **하위 소비자의 breakage** — `GraphNode`의 서드파티 C++ 서브클래스. 이 저장소 주변에 알려진 소비자는 다음과 같습니다.
     - `neoclaw` — `src/neoclaw_nodes.cpp:94`에 아직
       `std::vector<ChannelWrite> execute(const GraphState&) override`가 있습니다.
       v1.0이 나오기 전에 스스로 `run(NodeInput)`으로 이전해야 하며, 그렇지 않으면 v1.0 wheel에서 neoclaw가 깨집니다.
     - `NeoProtocol` Executor 런타임 — NeoGraph WASM 빌드를 씁니다.
       v0.4.0 바인딩 테스트를 권장합니다.
     - WASM 스파이크 — 엔진을 전혀 바꾸지 않는 경로가 v0.3.x의 기준이었습니다. v0.4의 run() 추가는
       추가 방식이라 아마 문제가 없겠지만 확인해야 합니다.
  4. **신규 사용자 모드의 함정 표면** — `ee11ed6`의 신규 사용자 점검에서 챗봇 데모 세션이 보여 준 함정 5개를 해결했습니다.
     스트리밍, MCP, 비동기 fan-out, HITL 재개는 그 데모가 건드리지 않은 경로이므로 비슷한 밀도로 함정이 있을 수 있습니다.
     새 `cibuildwheel`과 처음 쓰는 사용자 시뮬레이션, 또는 별도의 세션 준비(priming)로 드러냅니다.
  5. **선택적 패치 릴리스** — 단계 A에서 실제 버그가 드러나면 v0.4.x 패치를 배포합니다.
     v1.0 전에 새 기능이 꼭 필요하다면 v0.5.0 마이너를 배포합니다(여전히 deprecation 기간 안입니다).

종료 기준: 단계 A는 deprecation 경고가 "한 릴리스 동안 조용했을" 때 끝납니다. 구체적으로는 마이너 주기 하나
(예: v0.5.0 배포)를 마치는 동안 기존 경로와 관련해 사용자에게 보이는 breakage가 0건이어야 합니다.

### 단계 B — 파괴적 제거 (v0.9.0에서 완료)

하위 PR은 아래 순서로 각각 독립적으로 반영되었습니다. 각 단계를 따로 검토하고 되돌릴 수 있게 하기 위해서입니다.

| 하위 PR | 범위 | 위험 | 변경한 파일 |
|---|---|---|---|
| **9b** | `graph_node.cpp`의 기존 기본 체인(`ExecuteDefaultGuard` 재귀 감지가 있는, 가상 함수 8개의 교차 라우팅 로직)을 삭제한다. `node.h`의 가상 함수 선언 8개를 삭제한다. `src/core/deep_research_graph.cpp`(서브클래스 5개 이상)와 `src/core/plan_execute_graph.cpp`(서브클래스 3개 이상)의 내부 노드를 `execute()` / `execute_full()` 오버라이드에서 `run(NodeInput)`으로 이전한다. | **높음** — 내부의 모든 GraphNode 서브클래스를 PR 하나에서 이전해야 한다. 내장 노드는 이미 PR 9a(`d1070dc`)에서 이전했다. 이 두 그래프 팩토리는 노드가 `nodes/`가 아니라 파일 로컬이어서 마지막까지 남았다. | `node.h`, `graph_node.cpp`, `deep_research_graph.cpp`, `plan_execute_graph.cpp` |
| **9c** | `add_cancel_hook`, `Hook` RAII 클래스, `hooks_` 멤버, `hooks_mu_`, `cancel()`의 훅 순회 루프를 삭제한다. `cancel.h`에는 `fork()` + `cancel()` + `is_cancelled()` + `slot()` + `bind_executor()`만 남는다. | **중간** — `fork()`가 표준 대체 수단이며 CancelTokenFork ctest 7개로 검증된다. 실패 양상은 `add_cancel_hook`을 계속 호출하는 사용자 코드의 링크 오류이며, 컴파일 단계에서 잡히고 조용히 넘어가지 않는다. | `cancel.h`만 (구현이 헤더 전용) |
| **9d** | `CurrentCancelTokenScope`(헤더 + 구현), `current_cancel_token()` thread_local 접근자, `state.run_cancel_token_` 멤버, `set_run_cancel_token` / `run_cancel_token` / `run_cancel_token_shared` 접근자를 삭제한다. `cancel.cpp`는 비게 되어 파일을 지울 수 있다. 이제 `RunContext::cancel_token`이 유일한 경로이다. | **중간** — 내부에서 몰래 끼워 넣던 지점은 모두 이미 `ctx.cancel_token`을 읽고 있어야 한다(PR 7에서 바인딩은 끝났고, provider 쪽 읽기는 감사가 필요하다). 실패 양상: 여전히 `current_cancel_token()`을 읽는 provider는 null을 받아 취소가 LLM HTTP까지 전파되지 않는다. | `cancel.h`, `cancel.cpp`(삭제), `state.h`, `graph_state.cpp`, 그리고 `provider/*` 전체 감사 |
| **9e** | `PyGraphNodeOwner`의 기존 GraphNode 오버라이드 6개(`execute(GraphState&)`, `execute_full`, `execute_full_async`, `execute_stream`, `execute_full_stream`, `execute_full_stream_async`)를 삭제하고 `run(NodeInput)` + `get_name()` + 소멸자만 남긴다. `tests/test_node_default_dispatch.cpp`와 `tests/test_node_async_default.cpp` 및 이들의 CMakeLists 항목을 삭제한다. | **낮음** — 순수한 삭제이며 깨질 로직이 없다. 실패 양상: `execute()`만 정의하고 `run()`이 없는 Python 사용자 클래스는 디스패치할 때 NotImplementedError를 만난다. 단계 A에서 이런 사례가 드러났어야 한다. | `bindings/python/src/bind_node.cpp`, `tests/CMakeLists.txt`, 테스트 파일 2개 |

9b–e가 반영된 뒤에는 다음 일이 남습니다.

  - **SOVERSION 도입**("올리는" 것이 아닙니다 — 현재 어떤 neograph_* 라이브러리에도
    `set_target_properties(... VERSION ... SOVERSION ...)`가 없습니다). v1.0.0은 `libneograph_core` / `_llm` /
    `_postgres` / `_sqlite` / `_mcp` / `_a2a` / `_acp` 전체에 SOVERSION 1을 도입할 자연스러운 시점입니다.
    cibuildwheel 매트릭스를 확인해야 합니다(manylinux soname 접미사, macOS install_name, Windows DLL은
    SOVERSION을 각자 다르게 처리하므로, "CMake 속성을 설정했으니 동작할 것"이라고 가정하지 말고
    벤치마크를 하듯 검증합니다).
  - **문서 정리** — `docs/concepts.md`의 "디스패치 진입점 8개" 문단이 하나로 줄어들고,
    `docs/troubleshooting.md`에서 기존 체인 항목을 삭제하며, README의 "LangGraph와의 차이"는
    "NeoGraph의 사고방식"이 됩니다(차이가 메워졌으므로 LG 대비 항목 대부분은 더 이상 해당하지 않습니다).
  - **v1.0.0 태그 → PyPI 배포** — 마지막 단계입니다. 되돌리는 비용이 크므로(PyPI 릴리스를 yank하고 태그를 되돌려야 함)
    태그를 푸시하기 **전에** 전체 ctest, pytest, 실제 LLM 5건, cibuildwheel의 wheel 20개 매트릭스가
    모두 통과하는지 확인합니다.

### v0.4.0 이후 이 로드맵의 사소한 정정

감사에서 이전 초안의 작은 오류 두 가지가 발견되었습니다.

  - **PyGraphNodeOwner의 기존 오버라이드 수는 7개가 아니라 6개입니다.** 이전 메모에는
    "오버라이드 7개를 제거하면 run()만 남는다"고 적혀 있었습니다. 실제로 `bind_node.cpp:183`의
    `PyGraphNodeOwner`에 있는 GraphNode 파생 오버라이드는 6개입니다(GraphNode 가상 함수 8개에서,
    한 번도 오버라이드된 적이 없어 기본 체인이 처리하던 `execute_async`와 `execute_stream_async`를 뺀 수).
    9e 이후에는 `run()`만이 아니라 `run()` + `get_name()` + 소멸자가 남습니다.
  - **SOVERSION은 "올리는" 것이 아니라 "도입하는" 것입니다.** `CMakeLists.txt:5`의 주석에는 SOVERSION이
    언급되지만 실제 `set_target_properties(... SOVERSION ...)` 호출은 없습니다. v1.0이 이를 처음 설정하는 버전입니다.
    따라서 Linux .so나 macOS dylib install_name에 SOVERSION 접미사가 붙을 때 wheel 레이아웃이 퇴보하지 않는지
    cibuildwheel 매트릭스 실행으로 확인해야 합니다.

### 과거의 반사실: "제거하지 않았다면?"

단계 B가 끝내 반영되지 않아 기존 코드가 v1.0 이후에도 남더라도 시스템은 **깨지지 않습니다.**
현재의 모든 시나리오가 계속 동작하고, 452개 ctest가 모두 통과하며, deprecation 경고는 사용자 오버라이드 지점에서만 나옵니다.
비용은 급성이 아니라 구조적입니다.

  - **버그 부류가 자라는 터가 열린 채로 남습니다.** v0.3.x의 5라운드 취소 전파 패치 시리즈는 같은 패턴을
    디스패치 진입점 8곳 × 언어 2개에 모두 연결해야 했기 때문에 생겼습니다. 기존 체인을 남겨 두면,
    다음 횡단 관심사(deadline / trace_id / metric handle / 관측용 트레이싱)에서도
    N곳 중 M곳만 반영하는 누락 버그가 생길 여지가 남습니다.
  - **`state.run_cancel_token_`은 채널 집합에 속하지 않는 멤버**라서 다중 Send fan-out마다 명시적으로
    전달하지 않으면 사라집니다. 여기에 실행별 필드를 새로 추가하면 v0.3.1의 포인터 누락 버그가 반복됩니다.
  - **문서에 API 표면이 두 개 있습니다.** 신규 사용자는 소스를 읽지 않고는 `execute`와 `run`을 구분할 수 없습니다.
    `ee11ed6` 신규 사용자 점검에서 찾은 함정 5개가 정확히 이런 문서 공백의 형태였습니다.
  - **SOVERSION을 깔끔하게 도입하지 못합니다.** 배포판 패키저(Debian, Homebrew, conda-forge)는
    SOVERSION이 없는 라이브러리를 업스트림이 제대로 관리하지 않는 것으로 봅니다.
  - **경고 피로.** deprecation 경고가 영구적으로 남으면 사용자가 경고를 무시하게 되어, 다음 진짜 deprecation이 묻힙니다.

이 중 어느 것도 지금 v0.4.0을 깨뜨리지는 않습니다. 하지만 앞으로의 모든 진화를 느리고 버그가 나기 쉽게 만듭니다.
"정식 방법은 하나"라는 v1.0의 약속이 다섯 가지에 대한 답을 한꺼번에 줍니다.

## PR별 계약

각 PR은 다음을 지켜야 합니다.

  - 병합 시점에 **ctest 442/442와 pytest 96/96을 깨뜨리면 안 됩니다**(빌드에서 deprecation 경고는 허용하고 오류는 허용하지 않습니다).
  - **벤치마크가 퇴보하면 안 됩니다**(`bench_neograph` seq 경로의 µs/iter 중앙값을, 공개된 측정/빌드 가이드에 따라
    새 워크트리에서 taskset+chrt로 측정합니다).
  - **헤더 표면, 엔진 내부, 바인딩, 예제 중 하나만 건드려야 합니다.** 섞인 PR은 검토가 어렵고 되돌리는 비용이 큽니다.
  - **병합할 때 이 표에 행을 추가해야 합니다** — 제안했던 줄에 취소선을 긋고, 병합 커밋을 링크하고,
    범위가 달라졌다면 적습니다.

## 성능 회고 — `b59444f`, 18일간 잠복한 par 회귀

v1.0 주기의 막바지에, README에 적힌 "엔진 오버헤드" 수치(par 11.8 µs)가 실제로는 깨져 있다는 사실이 드러났습니다.
측정과 병렬 bisect 결과, 커밋 `b59444f` 하나가 18일(2026-04-26 → 2026-05-13) 동안 잠복해 있던 회귀의 원인이었습니다.

### 무슨 일이 있었나

- `b59444f`가 `GraphEngine::compile()`의 기본 worker 수를 `1`에서 `std::thread::hardware_concurrency()`로 바꿨습니다.
  의도는 fan-out 노드가 따로 설정하지 않아도 실제 병렬 실행을 받게 하는 것이었습니다.
- 부작용으로 노드 1개 순차 + 노드 5개 fan-out 마이크로 벤치마크에 **반복당 스레드 간 submit 비용 약 75 µs**가
  추가되었습니다. 11.8 µs가 283 µs(24배)가 되었습니다.
- 4월 27일의 성능 감사(`project_perf_audit_2026-04-27.md`)는 `fd60aab`을 "수정"으로 기록하지만,
  그것은 별개의 회귀(시간 측정 패턴)였고 worker 수 기본값은 그대로 두었습니다.
  par 마이크로 벤치마크 자체는 "기본값=hardware_concurrency" 모드로 측정되고 있어서 수치상 정상으로 보였습니다.
  그러나 README의 11.8 µs는 `b59444f` 이전의 값이었습니다.
- v1.0 주기의 PR별 계약은 "벤치마크가 퇴보하면 안 된다"를 요구하지만, 당시 벤치마크는 같은(이미 회귀한) 기준선과
  비교하고 있었으므로 ±5% 범위 안에 들어 조용히 통과했습니다. 18일간 잠복한 이유입니다.
- 2026-05-13에 커밋별 병렬 bisect(`git worktree add`로 워크트리 11개를 병렬로 만들고 taskset+chrt로 측정)를 수행해,
  par 11.8 µs → 283 µs 급등의 원인 커밋이 `b59444f` 하나임을 확인했습니다.
  되돌리기(`e5ecb08`)로 11.8 → 12.2 µs를 회복했습니다.

### 절충 — 기본값 1이 맞는 이유

`asio::thread_pool`에서 스레드 간 submit은 작업당 대략 75 µs가 듭니다. 노드 하나의 실행 시간이 ms 단위
(LLM 호출, HTTP 등)라면 이 비용은 잡음에 묻힙니다. 하지만 NeoGraph이 내세우는 "엔진 오버헤드가 직렬/병렬 모두 µs 수준"인
경로에서는 같은 자릿수라서 그대로 드러납니다.

- **CPU 부담이 작거나 순차적인 노드(마이크로 벤치마크, 검증기 체인 등)** — 기본값 1이 압도적으로 낫습니다.
  worker 풀 없이 io_context 스레드 하나에서 순차 실행합니다.
- **실제로 fan-out이 목적인 경우(sleep 위주의 시뮬레이션, 별도 프로세스 호출, 동기 HTTP)** — 사용자가
  `engine->set_worker_count_auto()` 또는 `set_worker_count(N)`을 명시적으로 호출해야 합니다. 한 줄이면 됩니다.

이 절충을 분명히 하려고 `e5ecb08`의 커밋 메시지와 다음 fan-out 예제 5곳(10/14/21/36번과
`deep_research_graph` 빌더)에 `set_worker_count_auto()` 호출을 명시적으로 추가했고,
마이그레이션 문서의 Migration 3 절을 보강했습니다.

### PR별 계약 강화 (다음 회귀를 막기 위해)

`bench_neograph par` 마이크로 벤치마크가 기준선의 ±5% 안에 있는지만 확인하는 것으로는 충분하지 않았습니다.
기준선 자체가 회귀하면 함께 내려앉기 때문입니다. 후속 패치에서 다음을 합니다.

  - 벤치마크 회귀 CI가 **README에 적힌 절대값**(`seq ≈ 5.0 µs`, `par ≈ 12 µs` 등)을
    두 번째 실측 시간 기준 관문으로 씁니다. 기준선 자체의 회귀를 잡아냅니다.
  - 또는 GitHub Actions cron으로 master → master 7일 회귀 측정(야간 soak 방식)을 추가합니다.
  - PR diff가 `GraphEngine::compile()`이나 `set_worker_count`를 건드리면 PR 본문에
    "별도 마이크로 벤치마크 측정 결과"를 넣어야 합니다(CODEOWNERS 훅으로 자동화).

세 가지 모두 후속 작업입니다. v1.0 릴리스 전에 적어도 하나는 반영되어야 합니다.

### 배운 점

1. **기본값 변경은 기능상 의미가 없어도 성능에 결정적인 계약이 될 수 있습니다.**
   README에 내세운 수치가 "기본" 경로에서 나왔다면, 기본값을 바꾸는 것은 README를 바꾸는 것입니다.
2. **회귀 측정의 기준선도 회귀할 수 있습니다.**
   ±범위 비교만 하지 말고 절대값 기준점도 두어야 합니다.
3. **병렬 bisect(워크트리 11개를 병렬로 돌리고 결과를 취합)로 18일간 잠복한 회귀를 30분 만에 찾아냈습니다.**
   선형 bisect보다 훨씬 빠르며, master가 길어졌을 때의 기본 도구입니다.

## 리팩터링 중에 피해야 할 v0.3.x 함정 기록

v0.1.x에서 v0.3.x까지 빌드/릴리스 파이프라인에는 지뢰가 쌓였습니다. 각 항목에는 메모리 항목(memory entry)이 있으며,
이 표는 그 영역을 건드릴 때 확인할 체크리스트입니다.

| 함정 | 문제가 생기는 곳 | 메모리 항목 |
|---|---|---|
| 모든 공개 클래스와 자유 함수에 붙이는 `NEOGRAPH_API` 매크로 | 새 엔진 하위 라이브러리(postgres / sqlite / mcp / a2a / acp). Windows DLL 경계. | 공개된 측정/빌드 가이드 |
| 브랜치 간에 남아 있는 오래된 .so 오염 | `BUILD_SHARED_LIBS=ON`인 build/를 여러 브랜치에서 재사용하면 ABI 불일치로 compile()에서 SEGV가 난다. | 공개된 측정/빌드 가이드 |
| 벤치마크 측정 시 빌드 디렉터리 오염 | 오래 쓴 build/ 디렉터리가 새 워크트리 빌드보다 느린 바이너리를 만든다(반복당 +0.4 µs의 거짓 신호). | 공개된 측정/빌드 가이드 |
| 과거 측정의 흔들림 | 단순히 "여러 번 반복하고 중앙값을 낸다"로는 수렴하지 않는다. taskset + chrt FIFO 99가 필요하다. | 공개된 측정/빌드 가이드 |
| 주석 안의 Doxygen `/*` 와일드카드 | `/**` 안의 `fs/*`, `terminal/*`가 중첩 주석을 열어 이후 진단을 막는다. `&#42;` HTML 엔티티를 쓴다. | 공개된 측정/빌드 가이드 |
| ASan `__cxa_throw` 인터셉터 CHECK | pybind 경계를 넘는 C++ 예외가 `LD_PRELOAD libasan.so` 아래에서 인터셉터를 건드린다. CI에서는 키워드로 제외하고, 취소/throw의 정확성은 TSan과 실제 LLM 테스트로 검증한다. | (이번 세션 — 피드백에 메모 추가) |
| TSan eptr 수명 경쟁 | NodeInterrupt의 exception_ptr가 co_await 경계를 넘으면서 libstdc++ `__exception_ptr::_M_release`를 건드린다. 수정: reason을 `std::string`으로 꺼내고 메인 스레드에서 새로 throw한다. | 공개된 측정/빌드 가이드 |
| MSVC에서는 `<array>` / `<algorithm>`을 명시해야 함 | libstdc++는 이를 간접 포함하지만 MSVC v143은 그렇지 않다. `std::array` 등을 쓰는 테스트 파일이 Windows CI를 조용히 깨뜨린다. | (이번 세션 — 피드백에 메모 추가) |
| scikit-build-core 0.12.2의 Windows single_config | `-G` 플래그가 감지되고 환경 변수는 무시된다. 그래서 Windows wheel에서 SQLite=OFF 재정의가 사라진다. `[[tool.scikit-build.overrides]]`와 `cmake.define`을 쓴다. | 공개된 측정/빌드 가이드 |
| Wheel의 OpenSSL CA 경로 | manylinux libssl은 Ubuntu에 없는 AlmaLinux 경로를 쓴다. `__init__.py`가 certifi에서 `SSL_CERT_FILE`을 자동 설정한다. | 공개된 측정/빌드 가이드 |
| CI의 PYTHONPATH 흐름에서 pyproject.toml의 런타임 의존성이 자동 설치되지 않음 | `pip install --quiet pytest` 줄이 pyproject.toml의 `dependencies = [...]`를 그대로 반영해야 한다. v0.3.2에서 pydantic이 빠졌다. | (이번 세션 — 피드백에 메모 추가) |
| `compile()` 기본 worker 수 회귀 | `b59444f`가 기본값을 `1 → hardware_concurrency`로 바꿔, par 마이크로 벤치마크가 11.8 → 283 µs(24배)로 느려진 채 잠복했다. 기준선 자체가 회귀하는 패턴이다. `e5ecb08`에서 수정. | 위의 "성능 회고" 절 |

리팩터링 PR이 새 하위 라이브러리, 새 공개 클래스, 새 런타임 의존성, pybind를 넘어 예외를 던지는 새 테스트 패턴,
새 wheel 플랫폼을 추가한다면 먼저 이 표를 확인하세요. v0.3.x 패치 시리즈의 절반은 이미 이 목록에 있는 항목을
다시 발견한 것이었습니다.

## 문서 영향 지도

리팩터링이 반영되면 다음 페이지를 고쳐야 합니다.

  - **`README.md`** — "Python Binding" 절의 RunConfig 표, "LangGraph와의 차이" 항목들(대부분이 낡은 내용이 되므로
    고치지 말고 삭제해야 합니다), "바인딩이 다루는 범위" 표면 목록.
  - **`docs/reference-en.md`** (1622줄) — GraphNode / Node / Provider / CancelToken / RunConfig 절.
    대략 30~40%를 다시 씁니다. 둘러보는 형태의 서술 구조는 유지하고 API 표면은 줄어듭니다.
  - **`docs/concepts.md`** — 531줄의 개념 서술. "디스패치 진입점 8개" 문단이 하나로 줄고,
    취소 전파 문단이 정리됩니다.
  - **`docs/troubleshooting.md`** — v0.3.x 항목 대부분이 낡은 내용이 됩니다.
    "조용히 아무 일도 하지 않음" / "오버라이드를 잊음" / "thread_local 누락" 항목은 삭제할 수 있습니다.
  - **`bindings/python/examples/`** — 모든 예제(Python 22개 + C++ 30개)를 갱신합니다.
  - **`Doxyfile`** — 변경 없음. PROJECT_NUMBER를 pyproject.toml에서 읽으므로 v1.0.0이 자동으로 반영됩니다.
  - **`ROADMAP_v1.md`**(이 파일) — 반영된 후보를 취소선으로 지우고,
    예상보다 어려웠거나 쉬웠던 점에 대한 사후 분석을 추가합니다.

## v1.0 완료 기준

  1. 노드 작성, 실행 취소, 상태 읽기, 상태 갱신, 그래프 실행 각각에 정식 방법이 하나씩 있어야 합니다.
  2. README의 "Python Binding" 절을 신규 사용자가 5분 안에 읽을 수 있어야 합니다.
  3. `docs/reference-en.md`의 GraphNode 절이 메서드 8개가 아니라 1개여야 합니다.
  4. v0.3.x deprecation 경고가 최종 제거 전에 적어도 한 릴리스 동안 조용해야 합니다.
  5. v1.0 태그에서 ctest / pytest / 실제 LLM / Valgrind / Doxygen이 모두 통과해야 합니다.

---

# 연구 트랙 (위의 v1.0 개선안보다 중요도가 낮음)

## 후보 4 — 자기 진화 JSON 에이전트 v2 (연구)

### 배경

`bindings/python/examples/22_self_evolving_graph.py`는 루프가 닫힌다는 것을 보여 줍니다.
LLM 수정자(modifier)가 실행 중인 그래프에 대한 JSON 수정안을 제안하고, 엔진이 다시 컴파일하고, 새 그래프가 실행됩니다.
PoC는 동작하지만, LLM이 수정안을 제안할 때 채널 데이터 흐름을 추론하는 데 어려움을 겪습니다.
어떤 노드가 어떤 채널을 읽고 쓰는지 "보지" 못하기 때문에, 제안이 데이터를 엉뚱한 선으로 흘려보내는 경우가 많습니다.

### 연구 방향

수정자 프롬프트에 채널 토폴로지를 명시적으로 노출합니다. 검토할 형태는 두 가지입니다.

1. **프롬프트에 토폴로지 요약 넣기** — 엔진이 컴파일된 채널 접근 패턴에서 얻은 노드별 사양을
   ``"node X reads {a,b}, writes {c}"`` 형태로 내보냅니다. 수정자 프롬프트는 JSON 정의와 함께 이를 받습니다.

2. **단계별 채널 제안** — 수정자가 채널을 평평한 집합으로 제안하는 대신 *단계별*(분할 / 합성 등)로 제안합니다.
   엔진은 제안된 각 단계의 채널 집합이 상류·하류 단계와 일관되는지 구성 시점에 검사합니다.

### v1.0의 필수 항목이 아닌 이유

- 출시된 엔진에 대한 사용자 차단 요인이 아닙니다. PoC의 부족함은 엔진이 아니라
  *프롬프트 엔지니어링*에 있습니다.
- 엔진 쪽 표면을 바꾸는 것이 정당화되려면 먼저 LLM 평가 하네스(토폴로지 변형별 정답률, 비용,
  수렴까지의 수정 횟수)가 필요합니다.
- 평가를 통해 LLM이 실제로 어떤 introspection을 쓰는지 드러나면, 더 넓은 "그래프 introspection API"라는
  v1.x 기능에 합쳐질 수 있습니다.

### 비용

- 연구에서 타당성이 확인되면 엔진 표면에 추가할 것(토폴로지 접근자)은 작습니다.
- 작업 대부분은 이 저장소의 핵심 경로 밖에서 이루어지는 LLM 쪽 실험입니다.

### 계기가 된 라운드

TODO_v0.3.md 항목 #8 — 사용자 차단 요인이 아니라서 연구 과제로 v0.3.x에서 미뤘습니다.

## 후보 5 — 쿡북 트랙: pgvector RAG 예제

### 배경

`bindings/python/examples/`(예제 23개)는 ReAct, HITL, 의도 라우팅, 멀티 에이전트 토론,
딥 리서치(웹 크롤 / 웹 검색), 자기 진화 그래프 등을 다루지만 **벡터 검색 / RAG** 예제는 없습니다.
16/17번에 포함되지 않았음을 확인했습니다. 그 둘은 웹 리서치이지 임베딩 기반 검색이 아닙니다.

RAG는 가장 흔한 LLM 패턴 중 하나이며, 예제가 없으면 NeoGraph을 평가하는 사용자가 찾아보기 어려워집니다.

### 엔진이 아니라 쿡북 항목인 이유

엔진 표면은 지금도 충분합니다. `PostgresCheckpointStore`가 이미 연결 풀과 설정 방식을 갖추고 있어,
임베딩 + pgvector 노드가 이를 재사용할 수 있습니다. 엔진 API를 추가할 필요가 없고,
필요한 것은 동작하는 예제(150~200줄 정도)뿐입니다.

  - `EmbeddingNode` — OpenAI 임베딩이나 로컬 모델을 호출합니다.
  - `RetrieveNode` — 미리 채워 둔 테이블에 pgvector 유사도 쿼리를 보냅니다.
  - `RAGCallNode` — 검색된 컨텍스트를 이어 붙여 LLM을 호출합니다.
  - 인덱스 1회 설정 스크립트 — 예제를 실행할 때마다 다시 인덱싱하지 않도록 예제 본문과 분리합니다.

### v0.3.x에서 미룬 이유

v0.3.x 시리즈는 FastAPI SSE 채팅 데모 피드백에서 드러난 엔진 버그와 사용성 문제를 범위로 삼았습니다.
RAG는 엔진 버그가 아니라 "흔한 패턴에는 동작하는 예제가 필요하다"는 문제입니다.
버전을 올리는 일이 아니라 각 항목이 실제 활용 레시피인 별도의 쿡북 작업에 속합니다.

### 계기가 된 라운드

TODO_v0.3.md 항목 #9 — 쿡북 소재(엔진 공백 없음)로 확인되어 이후의 "예제 트랙" 정리로 미뤘습니다.

---

## 후보 6 — Provider 단일 디스패치

이 절은 이전의 completion API 제안을 기록합니다. 코드 스케치와 7월 호환 정책은 과거 기록이며,
위에서 설명한 타입 기반 prepare/dispatch 전환으로 대체되었습니다.

### 증상

`Provider`는 가상 메서드 4개를 제공합니다(GraphNode의 8개보다 한 차원 적습니다).

```
complete           complete_async
complete_stream    complete_stream_async
```

`(sync/async) × (stream/non-stream)`이므로 후보 1과 형태가 같고, "N개 중 적어도 하나를 오버라이드해야 한다"는
계약도, 함정도 같습니다. 비스트리밍 쌍은 안전합니다(브리지가 한 단계뿐입니다).
스트리밍 쌍은 #10 이전에는 안전하지 않았습니다. `complete_stream`은 동기 httplib이고,
기본 `complete_stream_async` 브리지는 이를 인라인으로 감쌌습니다(WebSocket Responses 경로는
엔진 io_context worker 위에 `run_sync`를 중첩했습니다). 그래서 `GraphEngine::run_stream_async` 안에서 호출될 때
간헐적인 segfault가 났습니다(이슈 #4, PR #10의 worker 스레드 브리지와 `SchemaProvider` 네이티브 오버라이드로 해결).

### 블로커가 아니라 정리 작업인 이유

구체적인 크래시(#4)는 해결되었습니다. PR #10이 동기 `complete_stream`을 위한 전용 worker 스레드를 띄우고
토큰을 awaiter의 executor로 되돌려 보내며, `SchemaProvider`는 WS 경로를 오버라이드해 worker 스레드조차 건너뜁니다.
`OpenAIProvider`와 `SchemaProvider`의 HTTP/SSE 경로는 모두 안전한 기본 구현을 물려받으므로,
가상 함수 4개의 곱집합이 더는 크래시를 일으키지 않습니다.
남은 것은 구조상의 흠입니다. 오버라이드 표면이 필요 이상으로 넓고, 브리지의 안전성이 곱집합의 어느 모서리를
오버라이드하느냐에 달려 있습니다(컴파일 타임에는 아무것도 이 불변식을 고정하지 않습니다).

### 반영된 방향

오버라이드 하나로 끝나는 경로는 대체가 아니라 추가 방식입니다.

```cpp
class CompletionProvider : public Provider {
public:
    asio::awaitable<ChatCompletion>
    invoke_request(CompletionRequest request);

protected:
    virtual asio::awaitable<ChatCompletion>
    do_invoke(CompletionRequest request) = 0;
};
```

`CompletionRequest`는 콜백이 있는지로 전송 방식을 추측하지 않고, 수집(collect) 모드와 스트림 모드를 명시적으로 선택합니다.
최종 어댑터는 기존 `Provider` 진입점을 모두 유지합니다.
그래서 새 구현은 오버라이드를 하나만 쓰면서, 기존 소스와 바이너리 계약은 그대로 유지됩니다.

### 인접 작업 — `schema_provider.cpp` 분리

`schema_provider.cpp`는 약 1,800줄로, 멀티 벤더 스키마 매핑, HTTP/SSE wire, 본문 생성, 응답 파싱이 한곳에 모여 있습니다.
단일 디스패치로 다시 쓰는 시점이 `SchemaParser` / `SchemaWireBuilder` / `SchemaProviderImpl`로 나누기에 알맞습니다.
별도의 ROADMAP 항목이 필요 없도록 여기에 적어 두며, 작업이 서로 다른 PR에서 이뤄진다면 나눠서 진행할 수 있습니다.

### 계기가 된 라운드

이슈 #5 — #4를 디버깅하다가 드러났습니다. 구체적인 크래시는 PR #10에서 해결되었고,
구조 정리는 추가 방식의 `CompletionProvider` 경로와 영구적인 호환 정책으로 반영되었습니다.

### 반영 기록 (v0.9.0 후보 주기)

2026-05-13 중순에 PR 5개가 master에 순서대로 반영되었습니다.

| PR | 범위 | 반영 버전 |
|---|---|---|
| **#40 (PR1)** | 새 가상 함수 `Provider::invoke(params, on_chunk = nullptr)`를 추가한다. 기본 구현은 기존 가상 함수 4개의 체인으로 전달한다(기존 Provider 서브클래스는 모두 그대로 동작한다). 새 ctest 6개. | v0.9.0 |
| **#41+#42 (PR2)** | 엔진 내장 LLM 노드(`LLMCallNode`, `IntentClassifierNode`)가 `provider->invoke(params, on_chunk)`로 디스패치한다. PR #41은 스택된 베이스에만 병합되었고, 이후 PR #42로 master에 다시 적용했다. | v0.9.0 |
| **#43 (PR3)** | 엔진 내부의 동기 LLM 호출 지점을 모두 이전한다. `agent.cpp`(5곳), `deep_research_graph.cpp`(6곳). `Provider::invoke()` 기본 구현에 thread-local 취소 전파 동등성을 추가한다(기존 `complete()`의 `current_cancel_token()` 동작을 재현). 새 ctest 3개. | v0.9.0 |
| **#44 (PR4)** | 기존 가상 함수 4개에 모두 `[[deprecated]]`를 붙인다. `plan_execute_graph.cpp`의 3곳을 `invoke()`로 이전한다. 데코레이터인 `OpenInferenceProvider`와 `RateLimitedProvider`의 가상 함수 오버라이드 블록 4개를 `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED`로 감싸 내부 전달자 경고를 막는다. 사용자에게 보이는 오버라이드/호출 지점에서만 경고가 난다. | v0.9.0 |
| **#45 (PR5)** | C++ 예제 이전(`31_local_transformer.cpp`, `cookbook/ai-assembly/member_server.cpp`). | v0.9.0 |

### 추가 방식의 호환 경로 (2026-07 개정)

기존 `Provider` vtable은 그대로 두고 별도의 `CompletionProvider`를 추가합니다.
새 구현은 명시적인 `CompletionRequest`를 받아 `do_invoke()`만 오버라이드합니다.
기존 가상 함수 4개와 콜백 기반 `invoke()`는 final 어댑터를 통해 새 구현에 연결됩니다.
기존 Provider 서브클래스와 Python 트램펄린은 그대로 보존됩니다.

기존 가상 함수는 제거 계획 없이 안정 API로 계속 지원합니다. 호환성 및 보안 수정은 계속 적용하지만,
모든 새 기능을 기존 가상 함수 4개에 백포트할 의무는 없습니다.
새 구현은 `do_invoke()`를, 새로운 직접 호출은 `invoke_request()`를 써야 합니다.
이 정책은 기존 공개 시그니처, 가상 함수 순서, 객체 크기, vtable을 바꾸지 않습니다.
#127의 네이티브 비동기 전송과 연산 소유권은 이 API 정책과 별개입니다.

후속 작업:

  - **6b**: 새 Provider는 `CompletionProvider` 기반으로 작성합니다. ABI에 영향을 주므로
    기존 내장 Provider의 직접 상속 관계는 바꾸지 않습니다.
  - **6c**: 네이티브 비동기 전송과 요청이 소유하는 취소 / 수명 관리를 완성합니다.
  - **인접 작업**: `schema_provider.cpp`(1800줄)를 `SchemaParser` / `SchemaWireBuilder` /
    `SchemaProviderImpl`로 나눕니다(위 6b와 같은 PR로 할지 따로 할지는 구현 시점에 정합니다).

---

## 후보 7 — gRPC 전송 (선택 컴포넌트)

### 배경

HasMCP의 콜드 이메일(2026-05-15)이 계기는 아니었지만, 그 이메일은 "gRPC가 다음 전송 방향"이라는 업계 신호를 공짜로 주었습니다.
MCP 커뮤니티에서는 [gRPC를 표준 전송으로 추가하는 방안](https://github.com/modelcontextprotocol/modelcontextprotocol/issues/966)을 논의하고 있고,
Google은 gRPC를 MCP의 네이티브 전송으로 만드는 작업을 하고 있습니다.
gRPC는 NeoGraph의 4축 서사와 거의 모든 축에서 맞아떨어집니다. protobuf 바이너리 직렬화(성능 / 경량),
HTTP/2 멀티플렉싱(멀티 테넌트의 연결 비용), 네이티브 양방향 스트리밍(토큰 / 이벤트),
스키마 강제와 작은 wire(임베디드)입니다.

### 결정 (2026-05-15)

- **자체 구현은 하지 않습니다.** 통신 프로토콜은 바퀴를 다시 발명할 위험이 큽니다.
  표준 `grpc++`와 `protoc`를 씁니다.
- **선택 사항(opt-in)으로만 제공합니다.** `NEOGRAPH_BUILD_GRPC` 옵션을 두고 **기본값은 OFF**입니다.
  grpc++는 protobuf, abseil, c-ares, re2, zlib을 끌고 오는데(간접 의존이 수십 MB), 그러면
  "의존성 2개 / libc.so.6만 / 바이너리 1.2 MB"라는 경량 축이 깨집니다. 기본값 OFF가 이를 막는 유일한 방법이며,
  `cmake-option-default-flip-trap`(EDDSkills, 이번 세션에 새로 추가됨)의 원칙을 적용합니다.
  `find_package(Protobuf/gRPC)`는 옵션 게이트 안에서만 호출하고 기본값은 뒤집지 않습니다.
- **MCP 표준과 무관한 NeoGraph 고유 API.** `proto/neograph.proto` = `GraphService { RunGraph(unary) /
  RunGraphStream(server-stream) / Health }`. payload는 JSON 문자열입니다(그래프가 곧 데이터라는 성질을 유지하기 위해서입니다.
  강한 타입의 proto 메시지로 모델링하면 사용자가 그래프를 바꿀 때마다 .proto를 다시 생성해야 합니다).
  MCP-over-gRPC 표준이 확정되면 이 서비스 옆에 MCP 형태의 서비스를 추가합니다(이 서비스는 그대로 둡니다).

### 반영됨 (v0.9.x 주기, 뼈대)

- `NEOGRAPH_BUILD_GRPC=OFF` 옵션 + 조건부 `find_package` + 치명적 가드.
- `proto/neograph.proto`, `src/grpc/graph_service.cpp`(해시를 키로 하는 컴파일 캐시 —
  multi_tenant_chatbot 쿡북 패턴 재사용), `include/neograph/grpc/graph_service.h`(`NEOGRAPH_HAVE_GRPC`
  가드), `examples/52_grpc_server.cpp`.

### 검증됨 — grpc++를 갖춘 첫 빌드 (2026-05-16)

apt `libgrpc++-dev protobuf-compiler-grpc`(1.51.1)와 protoc 3.21.12를 설치하고
`-DNEOGRAPH_BUILD_GRPC=ON`으로 빌드하자 엔드투엔드가 통과했습니다.
  - `neograph_grpc` / `example_grpc_server` / `example_grpc_client`가 모두 컴파일되고 링크됩니다.
  - C++ 클라이언트 → 서버: **Health**(ok/version/default_graph),
    **RunGraph** unary(`{"text":"hello from grpc"}` →
    `"HELLO FROM GRPC"`, trace=[upper]), **RunGraphStream**
    (이벤트 5개, FINAL payload, 상태 OK). `RESULT: PASS (failures=0)`.
  - protoc 코드 생성 경로(순수 `add_custom_command`)가 동작합니다. **버그 하나를 고쳤습니다.**
    VERBATIM 모드에서는 `ARGS --proto_path="${dir}"`의 따옴표가 그대로 전달되어
    protoc이 `"…/proto"`(따옴표 포함)를 디렉터리로 보고
    "directory does not exist"를 냈습니다. 따옴표를 빼고(`--proto_path=${dir}`) 해결했습니다.

### WSL Windows-PATH 누수 함정 (재현 가능 — 빌드 환경 경고)

grpc++를 켜고 빌드할 때 Windows PATH 항목이 Linux의 의존성 탐색을 오염시킬 수 있습니다.
대상 의존성 접두사(prefix)는 따로 유지해야 합니다.

아래 경로는 Windows 마운트 루트, Anaconda 설치 접두사, GTK 설치 접두사를 각각
환경 변수 `WINDOWS_MOUNT_ROOT`, `WINDOWS_ANACONDA_PREFIX`, `WINDOWS_GTK_PREFIX`로 나타냅니다.

  1. **anaconda re2** — `gRPCConfig.cmake`가 `find_package(re2)`를 호출할 때,
     시스템에 re2 cmake 설정이 없으면(apt `libre2-dev` 미설치)
     PATH에 있는 `$WINDOWS_ANACONDA_PREFIX/Library/lib/cmake/re2/re2Targets.cmake`
     (Windows)를 가져와 `set_target_properties`에서 오류가 납니다.
     해결: `-DCMAKE_IGNORE_PREFIX_PATH="$WINDOWS_MOUNT_ROOT;…"`
     + `-DCMAKE_IGNORE_PATH="$WINDOWS_ANACONDA_PREFIX/Library/lib/cmake;…"`를 지정하면 grpc가
     시스템 pkg-config의 re2로 되돌아갑니다("Found RE2 via
     pkg-config").
  2. **ZLIB include** — `FindZLIB`는 라이브러리를 시스템
     (`/usr/lib/.../libz.so`)에서 가져오지만 `ZLIB_INCLUDE_DIR`는 PATH에 있는
     `$WINDOWS_GTK_PREFIX/include`(Windows의 zlib.h)에서 가져옵니다.
     그 결과 `-isystem "$WINDOWS_GTK_PREFIX/include"`가 grpc를 링크하는 모든 타깃으로 새어 들어가고,
     `$WINDOWS_GTK_PREFIX/include/libintl.h`가 `printf`를
     `libintl_printf` 매크로로 바꿔 `std::printf` 컴파일 오류가 납니다. 해결:
     `-DZLIB_INCLUDE_DIR=/usr/include
     -DZLIB_LIBRARY=/usr/lib/x86_64-linux-gnu/libz.so`를 명시적으로 지정합니다.

  의존성 탐색은 대상에 맞는 접두사만 사용해야 합니다.

### NexaGraph 선행 사례 분석 — gRPC-MCP의 실질적인 ROI는 체크포인트

NeoGraph의 선행 프로젝트인 NexaGraph는 gRPC-MCP를 이미 일찍부터 구현해 운영했습니다.
조사 결과(Explore, 2026-05-16)는 다음과 같습니다.

- **구현의 실체**: `proto/rag_service.proto`
  (RAGService, unary RPC 11개 — vector_search / graph_search / ingest
  / 채팅 기록 / 이미지 작업 / **그래프 체크포인트** RPC 5개), `src/nexagraph/grpc_client.cpp`는
  완전히 구현되어 있고, `GRPC_TARGET` 환경 변수를 통해 api_server.cpp에서 운영 환경에 통합되어 있습니다.
  서버는 이중 전송(HTTP JSON-RPC + gRPC 50051)입니다. 스트리밍은 없습니다(모두 unary).
- **오버헤드 감소 주장**(`DOCS/grpc-client-plan.md`):
  직렬화 1ms→0.01ms, 임베딩 1536d 15KB→6KB, 요청마다 새 연결 → HTTP/2 멀티플렉싱.
  **측정값은 없고 설계상의 근거만 있습니다.**
- **솔직한 평가**: 일반적인 MCP 도구 호출에서는 LLM 추론(수백 ms)이 지배적이므로 1ms의 직렬화 절감은 잡음입니다.
  gRPC의 이득이 *실제로 존재하는* 영역은 **큰 구조화 payload**, 즉 임베딩 벡터, RAG ingest,
  특히 **그래프 체크포인트**(`channel_values_json`과 `channel_versions_json`이 스텝마다 커집니다)입니다.
  작은 도구 메타데이터나 문자열 쿼리에서는 1% 미만이어서(인지 복잡도를 감수할 가치가 없습니다),
  "MCP 일반이 빨라진다"가 아니라 "대용량 payload의 MCP"에 한정됩니다.

**핵심 발견 — NeoGraph에 도입할 때 우선순위 재조정:**

1. **gRPC CheckpointStore = 실질적인 ROI(최우선 후보)**.
   NexaGraph의 `grpc_checkpoint.cpp`는 이미 **`neograph::graph::CheckpointStore`를 상속**합니다.
   그 시점부터 NeoGraph의 체크포인트 추상화를 쓴 셈입니다. 즉 NeoGraph의 `Postgres/Sqlite CheckpointStore` 옆에
   `GrpcCheckpointStore`를 추가하는 형태로 거의 그대로 이식할 수 있습니다(~150 LoC).
   payload가 크고, (MCP #966) 표준과 무관하며, 방금 만든 `neograph::grpc` 컴포넌트 안에 자연스럽게 들어맞습니다.
   체크포인트는 스텝마다 생기는 큰 JSON blob이어서, gRPC 바이너리의 이득을 실제로 측정할 수 있는 유일한 핫 경로입니다.
2. **MCP-over-gRPC 전송(일반 도구 호출) = 우선순위 낮음**.
   LLM이 지배적이어서 이득이 작고, MCP-over-gRPC 표준도 아직 확정되지 않았습니다(#966).
   표준이 확정되더라도 RAG나 임베딩처럼 payload가 큰 도구에 한합니다.

### GrpcCheckpointStore — 반영 및 측정 완료 (2026-05-16)

`neograph::grpc`에 추가했습니다. `GrpcCheckpointStore`(클라이언트, NexaGraph와 같은 방식으로
`CheckpointStore`를 상속) + `CheckpointServiceImpl` + `run_checkpoint_server`(서버, 임의의 `CheckpointStore` 백엔드를 감쌈) +
`checkpoint_to/from_json` 도우미입니다. `CheckpointService` proto에는 RPC가 5개 있습니다.
NexaGraph의 평평한 매핑으로는 다룰 수 없던 NeoGraph의 풍부한 필드(next_nodes 벡터 / `CheckpointPhase` enum
/ `barrier_state` 중첩 맵 / `schema_version`)가 왕복해도 보존되며, 예제 54의 정확성 검사를 통과했습니다.

**측정 결과** (example_grpc_checkpoint, 1536차원 임베딩 +
12턴, 200회 반복, localhost 루프백) — "그럴듯하지만 증명되지 않음" 상태를 끝냈습니다.
다만 솔직히 말하면 절반은 기각되었습니다:**

| 지표 | 값 |
|---|---|
| JSON (checkpoint_json) | 29 080 B |
| Protobuf wire (CheckpointBlob) | 29 131 B |
| 가상의 JSON-RPC 봉투 | 29 155 B |
| protobuf / JSON-RPC payload | **99.9%** |
| InMemory 프로세스 내 | save 27 µs / load 36 µs |
| gRPC 왕복 | save 720 µs / load 755 µs |
| gRPC 네트워크 오버헤드 | **+693 µs save / +719 µs load** |

**솔직한 결론 — NexaGraph이 내세운 "직렬화 15KB→6KB 바이너리 압축"은 NeoGraph의 JSON-in-proto 설계에서는
성립하지 않습니다(payload가 99.9% 같습니다).** 이유: 그래프가 곧 데이터라는 설계의 견고함을 위해
체크포인트 전체를 proto 문자열 필드 하나에 담기 때문에 protobuf의 필드 단위 압축이 적용되지 않습니다.
NexaGraph은 멤버마다 필드가 있는 proto였으므로 압축되었지만, 체크포인트 형식이 바뀔 때마다
(`next_nodes` / `barrier_state` / `schema_version`이 추가될 때마다) proto를 다시 생성해야 합니다. 즉
**이 절충에서 우리는 압축보다 스키마 안정성을 일부러 택했으므로, payload 이득은 실제로 0입니다
(증명됨: 설계상 이득이 없음).**

gRPC의 *실제* 이득은 전송에만 있습니다. HTTP/2 연결 재사용입니다(JSON-RPC / HTTP1.1의 호출마다 연결을 없앱니다).
루프백 왕복 한 번에서 +700 µs가 나왔다는 사실로는 이를 확인할 수 없습니다(차이는 부하나 원격 RTT에서만 나타납니다).
즉 **전송 이득은 아직 부하 테스트에 달려 있으며, 한 번의 측정으로 증명할 수 없습니다.**

→ 우선순위 재확인:
  - **GrpcCheckpointStore의 실질적인 가치 = "타입이 있는 RPC와 HTTP/2 연결 재사용을 통한 원격 체크포인트" +
    "다언어 지원(polyglot): 어떤 언어의 서버든 `CheckpointService`를 구현할 수 있음"**.
    NexaGraph이 내세운 payload 압축이 아닙니다. 쿡북으로 제공하되, 솔직한 셀링 포인트는 "압축"이 아니라
    "에이전트 프로세스에 DB 드라이버가 전혀 없는, 타입이 있는 원격 체크포인트"입니다.
  - **MCP-over-gRPC 전송(일반 도구 호출) = 보류**. 체크포인트 측정에서 확인했듯 JSON-in-proto에서는
    payload 압축이 적용되지 않으므로, 도구 호출도 같은 설계를 따른다면 압축 이득은 0이고 LLM이 지배적입니다.
    표준(#966)이 확정되고 멤버별 필드가 정당화되는 큰 바이너리 도구(원시 임베딩 등)에 대해서만 다시 검토합니다.
  - 남은 검증: 부하에서(N개의 동시 체크포인트 저장) HTTP/2 멀티플렉싱이 호출마다 연결하는 방식에 비해
    실제로 차이를 만드는가. 벤치마크 작업 후보입니다(단발 측정이 아니라 지속적으로).

### 도구 호출: JSON-RPC 대 gRPC 정면 비교 (2026-05-16)

사용자 요청 — 체크포인트가 아니라 *도구 호출* 자체를 실제 서버에서 두 전송 방식으로 정면 비교합니다.
`proto`에 `ToolService.CallTool`을 추가했고, 예제 55는 **같은 연산 함수를 (a) httplib JSON-RPC 2.0 `tools/call`
(MCP 형태, HTTP/1.1 keep-alive)과 (b) gRPC ToolService(HTTP/2) 양쪽에 띄워** 같은 방식으로 측정합니다.

**정직성 사고 — "gRPC가 70배 빠르다"는 측정 아티팩트였습니다.**
첫 실행에서 JSON-RPC p50이 43 ms였고 payload와 무관하게 일정했습니다.
43 ms는 TCP delayed-ACK 타이머의 전형적인 특징입니다. 원인은 `CPPHTTPLIB_TCP_NODELAY` 기본값이
**false**여서 Nagle이 켜져 있었고, gRPC는 TCP_NODELAY가 기본으로 켜져 있어 불공정했습니다.
"gRPC 70배"를 그대로 커밋했다면 거짓이었을 것입니다.
양쪽에 `Server/Client::set_tcp_nodelay(true)`를 적용하고 다시 측정했습니다.

**공정한 조건에서의 결과(루프백, 양쪽 모두 keep-alive + NODELAY, N=300 p50, 2회 재현):**

| payload | gRPC p50 | JSON-RPC p50 | 비율 |
|---|---|---|---|
| tiny args (~30 B) | 433 / 448 µs | 436 / 410 µs | **0.99–1.09× (동률)** |
| 1536-float (~12 KB) | 655 / 680 µs | 1079 / 1016 µs | **0.61–0.67× (gRPC 약 1.5배)** |
| args wire (tiny) | 42 B | 118 B | 봉투 오버헤드 |
| args wire (12 KB) | 12025 B | 12100 B | **99% (압축 0)** |

**실제 결과:**
- **작은 도구 호출(실제 도구 호출의 대다수): JSON-RPC ≈ gRPC 동률.**
  전송 방식을 바꿔서 얻는 ROI ≈ 0.
- **큰 payload의 도구 호출(~12 KB 이상, 임베딩 / RAG 청크 반환): gRPC 약 1.5배.**
  NexaGraph이 언급한 영역이지만 70배가 아니라 1.5배입니다.
- payload 압축은 여전히 0입니다(JSON-in-proto, 체크포인트 측정과 일치).
- 이는 루프백 결과입니다. 원격 RTT, 동시성, payload 형태, 연결 관리는 따로 측정해야 하며,
  1.5배가 상한은 아닙니다.

**후보 7의 최종 판정:**
- gRPC는 다언어 사이드카 / 원격 타입 RPC 경계를 제공합니다. 측정된 약 1.5배의 이점은 시험한
  12 KB 도구 payload에 해당하며, 모든 큰 payload나 체크포인트에 적용되는 것은 아닙니다.
- 이 기록은 MCP-over-gRPC 도입을 보류했습니다. 작은 루프백 워크로드에서 동률이라는 사실이
  다른 워크로드나 전송에서의 이점이 없음을 증명하지는 않습니다.
- Nagle 사고 → EDDSkills SKILL 후보 `bench-shock-number-nagle-first`
  (전송 벤치마크에서 충격적인 수치가 나오면 TCP_NODELAY / Nagle / delayed-ACK를 먼저 의심.
  `perf-regression-bench-bisect`의 사촌). 사용자 승인 후 추가합니다.

### NeoGraph JSON-RPC가 gRPC와 동률인 이유 — yyjson (증명됨)

사용자의 통찰: "JSON-RPC는 yyjson으로 파싱하니 빠르다. 구조적으로는 gRPC가 이길 수밖에 없다."
이를 검증하기 위해 전송 계층을 걷어 낸 순수 코덱 마이크로벤치를 예제 55에 추가했습니다.

| 12 KB payload, 코덱만, 5000회 | µs |
|---|---|
| yyjson parse+dump | **38.9** |
| protobuf ser+parse | **1.75** |
| → yyjson / protobuf | **22.3배 느림** |

이 코덱 전용 실험에서 protobuf ser+parse는 1.75 µs, yyjson parse+dump는 38.9 µs로,
시험한 표현에서는 22.3배 차이였습니다. 위의 전체 전송 측정에서는 차이가 더 작습니다.
전체 측정에는 프레이밍과 I/O 작업이 달라서 포함되어 있으므로,
코덱 표만으로는 왕복 시간 차이의 원인을 분리할 수 없습니다.

이 측정은 Python의 `json`과 비교하지 않았고, 모든 경우에 통하는 코덱 비율을 확립하지도 않으며,
yyjson이 NeoGraph에만 있다는 것을 보여 주지도 않습니다. 전송 방식은 필요한 RPC 의미에 따라 고르고,
실제 payload와 배포 환경에서 측정해야 합니다.

### NexaGraph 두 번째 수확 — 기록 압축 + GrpcRemoteTool (2026-05-16)

NexaGraph 전체를 조사한 뒤, 이미 이식한 `GrpcCheckpointStore` 외에
*범용적이고, 중복되지 않으며, NeoGraph에 아직 없는* 항목 세 가지를 추가로 이식했습니다.
(`proto/rag_mcp_server/backend`의 RAG 앱 전용 stdio / HTTP MCP는 NeoGraph에 이미 있거나
앱 전용이어서 제외했습니다. `DOCS/graph-engine-design.md`는 사실 NeoGraph의 설계 조상이라
"이식" 대상이 아닙니다.)

1. **`neograph::history` (새 core 유틸리티, 추가 방식)** — NexaGraph의 CAF `compress_history` 액터에서
   액터 껍데기를 벗겨 내고 핵심만 이식했습니다.
   - `compact_history(messages, Provider&, model, max_tokens=12000,
     recent_keep=6, summary_controls=default_summary_controls())
     -> awaitable<CompactedHistory>` — 토큰 추정치가 예산을 넘으면, 요약 대상이 될 수 있는 봉인되지 않은
     텍스트 전용 앞부분만 `provider.invoke_async()` 호출 한 번으로 요약합니다.
     맨 앞의 system 메시지와 타입이 있는 뒷부분 전체는 그대로 둡니다.
     요약은 라벨을 붙인 user 역할의 파생 컨텍스트로 구성하며 지시문으로 쓰지 않습니다.
     EndTurn/StopSequence로 끝났고 ASCII 공백 이외의 텍스트가 있는 경우에만 요약이 기록을 대체하고,
     그렇지 않으면 입력 전체를 유지합니다.
     네이티브 봉인이 하나라도 있으면 그 봉인이 앞선 전체 기록에 묶여 있으므로 압축하지 않습니다.
     NativeReplayProtected는 요약기를 호출하지 않고 입력을 유지합니다.
     결과에는 상태, 처리한 입력 구간, 전체 outcome이 담깁니다.
     명시적인 `ProviderControls`가 예전의 기본값 0.2 / 500을 대체하며,
     지정하지 않은 temperature나 reasoning 필드를 강제로 채우지 않습니다.
   - `sanitize_tool_calls(messages&)` — 클라이언트 도구 호출/결과의 짝을 메시지를 수정하지 않고 검증합니다.
     호출 ID가 없거나 중복되거나, 짝이 없는 결과, 결과가 없는 호출은 `std::invalid_argument`를 던집니다.
     압축은 도구/네이티브 그룹을 유지하며 sanitize로 없애지 않습니다.
   - `estimate_tokens` — 보수적인 추정치(문자 약 3개당 토큰 1개, KO / EN 혼합).
     thinking과 reasoning 파트도 포함합니다.
   - 예제 56 `history_compaction`(오프라인 MockProvider, 키 불필요) — 잘못된 짝 거부,
     비권위 컨텍스트로의 압축, 잘린 요약에서의 유지를 보여 줍니다.
     `src/core/history.cpp`는 모든 구성에서 `neograph_core`에 빌드됩니다.
     ctest 496/497 통과(실패 1건은 이미 있던 `pybind_smoke`의 openinference 모듈 누락으로, 관련이 없습니다).

2. **`neograph::grpc::GrpcRemoteTool`** — 예제 55가 gRPC로 도구를 *내보내는* 쪽(`run_tool_server`)이라면,
   이것은 그 거울상으로 원격 `ToolService.CallTool`을 평범한 `neograph::Tool`로 *가져오는* 쪽입니다.
   NexaGraph의 `GrpcTool` 어댑터를 이식했습니다.
   pimpl 방식이라 공개 헤더에 grpc++가 없습니다(`GrpcCheckpointStore`와 같은 방침).
   단순한 proto에는 도구 목록 RPC가 없으므로 정의는 생성자로 주입합니다. 서버 오류는
   `runtime_error`로 다시 던집니다(전송 오류가 아니라 도구 오류이며, 로컬 Tool과 같은 계약입니다).
   예제 57 `grpc_remote_tool` — 서버 스레드 + `Tool&` 다형 호출 + 오류 경로가 통과합니다.
   **gRPC의 ROI #1(다언어 원격 타입 RPC)을 소비하는 쪽에서 구현한 것입니다.**
   에이전트 관점에서 프로세스 경계를 넘는 도구는 호출 지점에서 로컬 도구와 구분되지 않습니다.

### 2026-05-16 기록 시점에 남은 일

  - CI에 `grpc-build` 작업을 추가합니다(apt 의존성 + ON 빌드 +
    `example_grpc_client` / `server` 스모크 — 깨끗한 ubuntu 러너에서는 위의 WSL 함정이 해당하지 않습니다).
  - `RunGraphStream`의 `ServerWriter::Write`는 스트리밍 노드 콜백 안에서 호출되며,
    현재는 슈퍼스텝 루프 스레드가 하나라고 가정합니다. 멀티 worker fan-out 그래프에서 콜백이
    worker 스레드에서 호출된다면 `ServerWriter` 동기화가 필요합니다
    (gRPC `ServerWriter`는 스레드 안전하지 않습니다). 현재 예제는 단일 노드라서 드러나지 않습니다.
  - TLS / 인증: `run_server`의 비보안 기본값 대신 사용자가 직접 연결하는 방법을 문서화합니다.
