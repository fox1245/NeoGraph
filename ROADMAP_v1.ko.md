<!-- neograph-i18n: source=ROADMAP_v1.md locale=ko source_sha256=e5d4df28a5ba92ca1778fdff4fa741fc4c435c9dfba1fddbe8dacd31ca1f3121 -->
# NeoGraph v1.0 — 설계 다듬기 로드맵

**Languages:** [English](ROADMAP_v1.md) | [한국어](ROADMAP_v1.ko.md) | [日本語](ROADMAP_v1.ja.md) | [简体中文](ROADMAP_v1.zh-CN.md)

이 파일은 미래 v1.0 주요 버전 범프(major bump)를 목표로 하는 **아키텍처** 변경을 추적한다. 이들은 점진적 패치가 아니며, 각각은 사용 중단 기간(deprecation window)이 필요한 공개 API 파괴(break) 후보이다. 살아있는 문서로 유지 — v0.3.x 패치 시리즈가 구조적 통증 지점을 드러내면 여기에 후보를 추가하고, 착륙하면 가지치기한다.

## 이 파일이 존재하는 이유

v0.3.x 취소-전파 시리즈(5 라운드: v0.3.0 단일 노드, v0.3.1 다중 Send 포인터, v0.3.1+ 프로세스 내 폴링, v0.3.2 Python용 훅, v0.3.2 C++ 스코프+재시도+예외 타이핑)는 하나의 논리적 수정이었는데, **같은 교차 관심사(cancel)가 ~8개의 디스패치 진입점과 2개 진입 언어(C++/Python)를 관통해야 했기 때문에** 5개 패치가 필요했다. 각 패치는 다른 경로를 열어둔 채 하나의 진입 경로를 닫았다.

버그 패턴은 거의 "아키텍처가 틀렸다"가 아니었다 — "올바른 패턴이 N개 장소 중 M개에만 적용되었다"였다. v0.3.x 시리즈는 *핵심* 설계(Pregel BSP 슈퍼스텝, 채널 리듀서, Send/Command 동적 디스패치, asio 코루틴 전반)를 예외로 검증했다: 모델 자체를 의심하지 않고 버그를 잡아냈다.

이 시리즈가 *실제로* 드러낸 것은 현재 설계에서 N-장소 구현 분산을 오류 발생하기 쉽게 만드는 세 개의 높은 인지 부하 이음새이다. 아래 각 후보는 하나의 이음새를 다룬다.

---

## Candidate 1 — 태그 기반 라우팅을 갖춘 단일 디스패치 진입

### 증상

`GraphNode`가 8개의 가상 메서드를 노출:

```
execute            execute_async
execute_full       execute_full_async
execute_stream     execute_stream_async
execute_full_stream execute_full_stream_async
```

이들은 `(sync/async) × (writes/full) × (stream/non-stream)` 데카르트 곱을 형성한다. 기본값이 서로를 통해 연결되며, 우선순위가 일관성을 유지해야 한다. 모든 기본 체인 홉(hop)은 버그가 숨을 수 있는 장소:

- v0.3.1 #2: 힌트 메시지가 스트리밍 변종을 언급하지 않음.
- v0.3.2 #10 (Python): `PyGraphNode::execute_full_stream`이 `execute_stream` 분기를 건너뜀 — `run_stream`이 스트리밍 전용 노드에 쓸모없어짐.
- v0.3.2 #10 (C++): `GraphNode::execute_full_stream` 기본값이 `execute_full`을 먼저 호출 → `ExecuteDefaultGuard` 재귀가 던짐 → `execute_stream`에 도달하지 못함.
- `execute_full_stream_async`에서 `co_await` 주변의 `catch(T&)`가 조용히 놓치기 때문에 GCC-13 코드 생성 해결책 필요.

### 다듬기

단일 가상 디스패치:

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

사용자는 하나의 메서드를 재정의. 동기/비동기 구분은 엔진이 처리(엔진이 동기 재정의를 run_sync로 감싸고, 비동기 재정의는 직접 await — 하지만 이는 사용자 관심사가 아니라 엔진 관심사). 스트리밍 구분: `stream_cb` non-null = 스트리밍 기대; 사용자가 사용하거나 무시. Command/Send: 그냥 필드를 채움.

이전: 한 릴리스 동안 8개 가상 함수를 사용 중단(deprecated)된 얇은 심(shim)으로 유지. 새 코드는 `run()`을 재정의. 트램펄린(`PyGraphNode`)이 한 줄짜리가 됨.

### 비용

- 공개 API 파괴 — 모든 기존 GraphNode 하위 클래스가 `run()` 재작성 필요.
- `RunContext`(Candidate 2)가 강한 전제 조건 — 없으면 `run()`이 실행별 메타데이터를 전달할 수 없음.
- 엔진 내부 디스패치 로직은 단순해지지만, 엔진이 런타임 힌트나 관례에 기반해 동기-vs-비동기를 선택해야 함.

---

## Candidate 2 — 실행별 메타데이터를 위한 명시적 `RunContext`

### 증상

현재 `RunConfig::cancel_token`이 호출자가 설정할 수 있는 유일한 실행별 "메타데이터"다. 엔진이 이를 두 메커니즘으로 은밀히 전달:

1. `GraphState::run_cancel_token_` — GraphState에 살지만 **채널 집합 안에 없어서** `serialize()`가 잃어버리는 멤버.

   - v0.3.1 다중 Send 수정: `init_state(send_state) + send_state.restore(snapshot)`이 작업자별 상태를 재구축했지만 채널 집합 밖에 있기 때문에 `run_cancel_token_`을 누락. 모든 Send 팬아웃 작업자에서 명시적 `send_state.set_run_cancel_token(parent.run_cancel_token_shared())`이 필요했음.
   - 다음 실행별 필드(deadline? trace_id? metric handle?)를 추가하는 사람은 이 똑같은 버그를 다시 만날 것.

2. `current_cancel_token()` thread_local — `CurrentCancelTokenScope`가 execute_full_async 진입에서 설정.

   - v0.3.2 C++ 수정: PyGraphNode가 스코프를 설치했지만, 네이티브 C++ `GraphNode::execute_full_async` 기본값은 설치하지 않아서, 다중 Send C++ 작업자의 `Provider::complete`가 null thread-local을 보고 run_sync가 취소 바인딩 없이 실행. 7s 비용 누수.
   - 모든 새 디스패치 진입점이 스코프 설치를 기억해야 함. 잊음 = 조용한 기능 파괴.

두 메커니즘 모두 실행별 메타데이터를 위한 일급(first-class) 장소가 없기 때문에 존재. 이들은 해결책(workaround)이다.

### 다듬기

명시적 `RunContext`가 모든 디스패치를 통해 `GraphState`와 함께 전달:

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

`Provider::complete(params, ctx)`도 컨텍스트를 받음. thread_local 없음. `current_cancel_token()` 없음. Send 팬아웃 작업자가 `ctx`를 값으로 복사 (저렴 — shared_ptr + 몇몇 문자열).

### 비용

- 공개 API 파괴 — 모든 Provider, 모든 GraphNode, 모든 Tool.
- 시그니처가 전반적으로 넓어짐 — `state, ctx`가 도처에.
- 하지만: "취소/추적/deadline을 관통시키는 것을 잊음" 버그 부류 전체를 닫음. 하나의 시그니처, 새 필드를 추가할 한 장소, 해결책 없음.

### 이게 막았을 v0.3.x 버그들

- v0.3.1 다중 Send 포인터 누락: ctx는 그냥 명시적 필드, 비직렬화 멤버에 묻혀 있지 않음.
- v0.3.2 C++ thread_local 누락: thread_local이 전혀 없음.
- 미래 deadline / trace_id / metric 누수: 같은 모양, 같은 예방 커버리지.

---

## Candidate 3 — 계층적 / 소비자별 CancelToken

### 증상

`CancelToken`은 하나의 `cancellation_signal sig_` + 하나의 `bind_executor` 슬롯을 중심으로 설계됨. asio의 `cancellation_slot`은 단일 핸들러 — 마지막 `bind_cancellation_slot`이 이김. 동시 소비자(다중 Send 팬아웃 작업자가 각각 Provider::complete → 내부 run_sync → bind_cancellation_slot 호출)가 서로의 바인딩을 조용히 덮어씀; 마지막으로 바인딩된 HTTP만 취소됨.

v0.3.2가 `add_cancel_hook` 목록을 이 단일 신호 설계 위에 이식하여, 각 중첩 run_sync가 부모의 `cancel()`이 훅을 반복하여 발화하는 자신만의 비공개 신호를 소유하도록 함. 작동하지만, "N-소비자 컨텍스트에서 사용되는 단일 소비자 기본 요소를 보상하는" 것으로 읽힘. 게다가 방출-vs-바인드 경쟁: add_cancel_hook이 호출될 때 cancel이 이미 설정되어 있으면, 동기 발화가 co_spawn이 슬롯을 바인딩하기 전에 POST되고 방출이 손실됨. v0.3.2가 run_sync 진입에서 이를 피하기 위해 즉시 `is_cancelled()` 단락(short-circuit)을 추가 — 패치 위의 또 다른 패치.

### 다듬기

계층적 취소:

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

각 `run_sync(aw, parent_token)`이:
```cpp
auto child = parent_token->fork();
child->bind_executor(io.get_executor());
asio::co_spawn(io, body(),
    asio::bind_cancellation_slot(child->slot(), asio::detached));
```

이식할 add_cancel_hook 목록 없음. 방출-vs-바인드 경쟁 없음(자식이 새로 생성되고, 신호가 먼저 바인딩되며, fork()가 부모 상태의 스냅샷). 다중 Send 팬아웃: 3개 형제 토큰, 부모가 세 개 모두 취소.

차용 출처: Go의 `context.Context` 취소, asio의 `asio::cancellation_state` / `make_cancellation_filter`(asio가 올바른 API를 얻으면). 패턴은 잘 알려져 있음.

### 비용

- CancelToken의 공개 API 변경 (추가적 — `fork()`가 새로움). 옛 `add_cancel_hook`은 사용 중단될 것.
- 내부: 모든 `run_sync(aw, cancel)`이 `run_sync(aw, cancel->fork())`로 됨.
- 순효과: 하나의 기본 요소가 "단일 신호 + 훅 목록 + 즉시-취소-단락 + 소비자별 경쟁 노트"를 대체.

---

## 교차 관찰

세 후보는 조합된다: Candidate 2가 Candidate 3의 토큰을 디스패치 경로를 통해 전달; Candidate 1의 단일 `run()`이 자연스럽게 취소 자식을 포함하는 `RunContext`를 받음.

만약 하나만 착륙한다면, Candidate 2를 선호 — 가장 큰 종류의 반복 버그(모든 디스패치를 관통해야 하는 모든 것)를 죽임.

추적: 이 파일은 v0.3.x 패치 라운드가 새 아키텍처 이음새를 드러내거나, 후보가 착륙할 때 갱신(취소선 및 병합 커밋 링크).

---

## 패턴 회고 — 9개 다운스트림 발견 (이슈 #36)

ProjectDatePop의 `cpp_backend` 스트레스 테스트가 v0.5 → v0.8 창에 걸쳐 9개의 NeoGraph 발견을 낳았다. **9개 중 최소 7개는** Candidates 1 + 6이 닫는 — 점진적으로가 아니라 *패턴이 재발할 수 있는 표면을 제거함으로써* — 동일한 구조적 패턴으로 추적된다.

### 통합 패턴

> **"X는 Y일 때만 안전하다" — 하지만 Y 전제 조건이 docstring에 명시되지 않았고, 컴파일 시 강제되지 않으며, 위반 시 런타임에도 표면화되지 않는다. 기본 경로가 조용히 잘못된 일을 하며, 종종 입력 데카르트 곱의 특정 모서리에서만 그렇다.**

| # | 숨겨진 조건부 불변성 |
|---|---|
| #4 | `Provider::complete_stream_async` 기본 브리지는 네이티브 동기 `complete_stream` 자체가 `run_sync`를 사용하지 **않을 때만** 안전 — `SchemaProvider` WS 경로에서 조용히 위반 |
| #5 | `Provider`의 4-가상 데카르트 곱은 선택된 재정의 표면이 우연히 브리지 중첩을 피할 **때만** 안전 — `provider.h`에서는 불변성이 보이지 않음 |
| #6 | `schema_mutex_` × on_chunk 잠금은 사용자 콜백이 SchemaProvider에 재진입하지 **않을 때만** 안전 — 수정 전 문서화되지 않음 |
| #9 | C++ openinference 동등성은 Python 래퍼가 번역되지 않는 콜백-스레드 정체성에 대한 숨겨진 가정을 가졌기 때문에 필요 |
| #16 | NeoGraph의 번들 cpp-httplib은 모든 소비자 TU가 `CPPHTTPLIB_OPENSSL_SUPPORT`를 정의할 **때만** 올바름 — 그렇지 않으면 조용한 ODR 위반 |
| #34 | `extra_fields`는 `params.tools`가 비어있지 **않을 때만** 적용 — 도구 없는 호출에서 추론 필드가 조용히 누락 |
| #35 | `temperature`는 `params.temperature ≥ 0`일 **때만** 전송 — 하지만 스키마에 "이 제공자는 temperature를 전혀 받지 않는다"고 선언할 방법이 없어, 모든 호출 지점이 기본값을 부정해야 함 |

두 개의 추가 발견(#17 문서 간극, #33 호출별 바인딩 간극)은 숨겨진-불변성 함정이 아니라 간극 보고; 동일한 근본 진단(추상화가 정적 표면을 선언했지만 동적 동등물을 노출하지 않음)이 적용됨.

### 왜 Candidates 1 + 6이 *인스턴스*가 아니라 *부류*를 닫는가

위 각 발견은 잘못 동작한 특정 재정의 지점에 대한 **대상 패치** (PR #10, PR #11, PR #12, PR #19, PR #20, PR #37, PR #37)로 닫혔다. 각 패치는 *패턴을 허용한 표면*을 변경하지 않고 남겼다: 8개 GraphNode 가상 함수, 4개 Provider 가상 함수, 스키마 build_body 분기 트리. 다음 다운스트림 — 또는 다음 벤더 스키마, 또는 하나의 기본값을 조정하는 다음 리팩터 — 이 같은 데카르트 곱의 새로운 모서리에서 다른 "X는 Y일 때만 안전하다"가 숨어 있는 것을 발견할 것이다.

Candidates 1과 6은 그 데카르트 곱을 **각각 하나의 가상 함수**로 무너뜨린다. 착륙 후:

- **Candidate 1** (GraphNode 8 → 1): "8개 가상 함수 중 어느 것을 재정의하느냐가 브리지가 안전한지 결정한다"는 결정이 더 이상 없다. 사용자는 `run(NodeInput)`을 재정의. 동기 vs 비동기, 스트림 vs 비스트림, writes vs full-result는 모두 본문 모양 선택 — 가상 함수 정체성에 묶인 숨겨진 불변성 없음.
- **Candidate 6** (Provider 4 → 1): 새 구현은 `CompletionProvider::do_invoke()`를 하나의 재정의 지점으로 사용. 기존 `Provider` 표면은 호환성을 위해 안정적으로 유지되며, 새 경로는 하나의 명시적 요청 모드와 하나의 드레인 패턴을 가짐.

나머지 2개 발견(#9 스레드 정체성, #16 ODR 매크로)은 Candidates 1 + 6으로 *수정되지 않음* — 별도의 이슈 부류(관측 계층 동등성, 빌드 시스템 관례). #9는 이미 해결(PR #12 + 동등성 테스트). #16은 이제 컴파일 시 보호(v0.8.0 `api.h`).

### 이 회고에서 계속 부하 지탱되는 것

9개 발견은 **프로젝트 나이와 무관하게** 표면화되었을 것이다. 그 중 어느 것도 장기 실행 운영 배포나 이국적 벤더가 필요하지 않았다 — 단일 다운스트림 소비자(ProjectDatePop)가 ~3주에 걸쳐 현실적인 에이전트 흐름을 작성하면서 나왔다. Candidates 1 + 6 없이는, 비슷한 깊이의 다음 다운스트림이 같은 모양의 또 다른 5-10개 발견을 낳을 것이다. 둘이 있으면, 부류가 닫힌다.

이것이 **v1.0 주기에서 Candidates 1 + 6을 우선시하는** 구조적 논거 — 더 미용적인 v0.x 정리보다. 각각의 새 "X는 Y일 때만 안전하다" 발견은 패치 노력으로 자체 비용을 지불했지만, 7개 발견에 걸친 누적 노력은 이미 Candidates 1 + 6의 추정 비용을 초과한다.

### v0.x 사용 중단 창에서의 완화

Candidates 1 + 6이 착륙할 때까지, 오늘 존재하는 곳에 불변성을 고정:

- `[[deprecated]]`를 기존 8 가상 함수에 + `docs/migration-v0.4-to-v1.0.md` — v0.4 / v0.8에 착륙.
- "Y일 때만 안전하다" 전제 조건이 있는 모든 재정의 지점에 `@warning` 블록 (예: `Tracer::start_span`, `OpenInferenceTracerSession::close`).
- 언어가 표현할 수 있는 TU 간 불변성에 대한 컴파일 시 `#error` 보호 장치 (예: `CPPHTTPLIB_OPENSSL_SUPPORT` 매크로 일관성 — v0.8에 착륙).
- 위반 시 불변성의 이름을 알려주는 친절한 런타임 오류 (예: `Unknown reducer: 'foo'. Available: ...` — v0.8에 착륙).

이들은 패턴이 물어뜯는 창을 좁히지만, 부류를 닫지는 않는다. Candidate 1 + 6이 닫는다.

---

## 상태

| # | 후보 | 상태 | 발화 라운드 / 이슈 |
|---|---|---|---|
| 1 | 단일 `run()` 디스패치 + 태그 | **v0.9.0에 착륙.** `run(NodeInput)`이 순수 가상; 기존 8 가상 함수와 대체 체인은 제거됨. | v0.3.1 #2, v0.3.2 #10 (×2 언어); #36에 의해 강화된 패턴 (9개 다운스트림 발견) |
| 2 | 명시적 `RunContext` 인자 | **v0.4–v0.8에 착륙** (`RunContext::store` 필드가 v0.8 #27에 추가) | v0.3.1 다중 Send, v0.3.2 C++ 스코프 |
| 3 | 계층적 CancelToken | **v0.4에 착륙** (`CancelToken::fork()` + 연쇄) | v0.3.2 훅, v0.3.2 방출-vs-바인드 |
| 4 | 자기 진화 그래프 런타임 훅 | 연구 | TODO_v0.3.md #8 |
| 5 | pgvector RAG 예제 | 쿡북 | TODO_v0.3.md #9 |
| 6 | Provider 단일 디스패치 | **제거 없이 착륙.** `CompletionProvider::do_invoke()`가 권장되는 하나의 재정의 경로. 기존 `Provider::complete*` 메서드는 계속 지원됨; 사용 중단 경고는 철회되었고 제거 계획 없음. | #4 (v0.7에서 닫힘), #5 (호환성 정책), #36에 의해 강화된 패턴 |

---

# 실행 계획

> **상태:** Candidate 1 완료. 아래 계획은 v0.4.0부터 파괴적 v0.9.0 v1-준비 릴리스까지 이전이 어떻게 단계화되었는지를 기록한다; 남은 작업이 아니라 역사적 맥락이다.

## 사용자 대면 동기

잠시 버그-부류 프레이밍을 잊자. **README를 여는 신규 사용자** 관점에서 오늘날 표면은 파편화되어 보인다:

  - "노드를 어떻게 작성하나요?" — 8개 가상 함수(`execute` / `execute_async` / `execute_full` / `execute_full_async` / `execute_stream` / `execute_stream_async` / `execute_full_stream` / `execute_full_stream_async`). 어느 것을 고를까? 답은 "Send/Command, sync/async, streaming/non-streaming에 따라 다르다" — 사용자가 선행 추론해야 하는 세 개의 직교 축.
  - "어떻게 취소하나요?" — `RunConfig::cancel_token`이 존재하지만, 취소가 LLM에 도달하려면 추가로 필요: (a) 엔진이 thread_local 스코프를 설치, (b) Provider::complete가 읽음, (c) run_sync가 훅을 등록, (d) 작업자가 재시도하지 않음. 이 중 어느 것도 읽을 한 곳에 있지 않다.
  - "상태를 어떻게 갱신하나요?" — v0.3.2에서는 `dict | list[ChannelWrite]`. 그 전에는 README가 한 모양을 문서화하고 바인딩이 다른 모양에서 조용히 무동작(no-op). 신규 사용자는 "왜 내 쓰기가 적용되지 않았지?"를 만나고 디버깅해야 함.
  - "상태를 어떻게 읽나요?" — 중첩 `state["channels"][name]["value"]` 또는 평탄 `engine.get_state_view(thread_id).<channel>` 또는 타입 있는 Pydantic 하위 클래스. 세 개의 유효한 답변; 단일 정식 답변 없음.
  - "그래프를 어떻게 실행하나요?" — `run` (동기) vs `run_async` vs `run_stream` vs `run_stream_async` vs `resume` vs `resume_async`. 여섯 개의 진입점, 다시 다축 행렬.

**각 개별 추가는 정당화되었다** (resume_if_exists는 실제 채팅 의미, StateView는 실제 사용성 향상 등). 하지만 **누적 효과는 하나의 일을 하는 2-4가지 방법이 문서, 예제, 바인딩 코드에 흩어져 있는 표면**. v0.3.x 패치가 계속 쌓였고, v0.3.x 취소 라운드(5개)는 이 파편화가 버그가 숨는 곳이기도 함을 가시화했다 — "올바른 방법"이 N개 장소 중 M개에 있을 때, 장소 N+1에서의 누락은 조용한-무동작 / 잊혀진-패턴 버그.

아키텍처 다듬기(Candidates 1-3)는 이것을 다음과 같이 무너뜨린다:

  - **노드 작성의 한 가지 방법** (`run(NodeInput) -> NodeOutput` + 태그).
  - **실행별 메타데이터를 관통시키는 한 가지 방법** (`RunContext` 인자).
  - **취소의 한 가지 방법** (`token->fork()` for nested ops, parent cancels all).
  - **상태 읽기의 한 가지 방법** (StateView가 정식; raw dict가 탈출구).
  - **실행의 한 가지 방법** (run / run_async 등을 스트림 콜백을 받거나 반복자를 반환하는 하나의 메서드로 통합).

이것이 v1.0 계약 — 문서 페이지가 다시 짧게 읽힌다.

## 버전 전략

| 버전 | 범위 | 공개 API |
|---|---|---|
| **v0.4.x** | RunContext가 *새* 매개변수로 착륙, 옛 메서드는 사용 중단되었지만 여전히 작동. CancelToken이 추가적 `fork()`를 얻음. 새 `run(NodeInput)`이 추가적으로 착륙. | 두 API 모두 호출 가능. 사용 중단 경고. |
| **v0.5.x** | 예제와 pybind 바인딩이 새 API로 이전. 옛 API는 사용 중단 유지. | 두 API 모두 호출 가능. 더 무거운 사용 중단 경고 + 문서가 새 것으로 안내. |
| **v1.0.0** | 옛 API 제거 (8개 가상 함수, thread_local 스코프, 단일 핸들러 CancelToken signal-on-self). | 단일 정식 API. |

근거: **v0.4 → v1.0 도약 없음.** 두 릴리스 사용 중단 창이 다운스트림 소비자(neoclaw, NeoProtocol Executor, WASM 스파이크, 이 저장소 밖의 모든 것)가 한 번에 하나의 구성 요소를 이전할 수 있게 한다. cibuildwheel 행렬은 창 전반에 걸쳐 그대로 유지 — 릴리스 경로당 20개 wheel 변경 없음, 옛 메서드에 대한 의존성만 천천히 감소.

이전이 예상보다 오래 걸리면(예: 서드파티 C++ GraphNode 하위 클래스가 흔한 경우), v0.5가 v0.5.x로 연장된 사용 중단, v0.6이 창을 늘림. 사용 중단 경고가 한 릴리스 동안 조용했을 때만 옛 API 제거.

## PR 순서

각 행은 하나의 병합 가능한 PR. 순서대로 master에 착륙(장기 기능 브랜치 없음 — 프로젝트의 커밋 이력은 직선이고 사용 중단 전략은 각 PR이 v0.4.0+i, v0.4.0+(i+1) 등으로 PyPI에 독립적으로 배포 가능함을 의미).

| # | PR | 범위 | 착륙 버전 |
|---|---|---|---|
| 1 ✓ | **`RunContext` 배관 (내부)** — `a473f0e` 착륙 | `struct RunContext`를 `engine.h`에 추가. 엔진의 `execute_graph_async`가 구성하고 관통. NodeExecutor가 `execute_full_async`로 전달. Pybind가 감쌈. **공개 대면 변경 없음** — 옛 메서드는 여전히 `state`만 받음; 새 `ctx`는 디스패치 경로에서 나란히 존재. ctest 442/442 + pytest 96/96 통과. 벤치 중앙값 5.365 µs (BASE 5.285 µs, +1.5%) — WSL2 ~3% 잡음 층 내, 5.185 µs 기준의 ±5% 대역 내. Pybind 감싸기는 PR 7 (바인딩 이전)로 미룸 — PR 1은 pybind 차이가 0이기 때문. | v0.4.0 |
| 2 ✓ | **`GraphNode::run(NodeInput) -> NodeOutput`** — `607ce66` 착륙 | GraphNode에 새 가상 함수. 기본 구현이 옛 8 가상 함수로 위임(우선순위 보존). 엔진의 선호 디스패치 진입으로 등록. 기존 C++ 하위 클래스는 기본 대체 경로를 통해 여전히 컴파일+작동. ctest 442 → 445 (3개 새 NodeRunDispatch 테스트) + pytest 96/96 + 5개 라이브 LLM/WS 통과. 벤치 중앙값 6.122 µs vs PR1 BASE 6.160 µs (Δ -0.6%) A/B 10 라운드 (호스트가 오늘 시끄러움, PR1 BASE가 어제의 5.285 → 6.160으로 표류 — 같은 코드, WSL2 지터; A/B 비교가 호스트 표류를 상쇄). **잡힌 함정**: `run(const NodeInput&)`가 asio 실행자 내에서 pybind 비동기 경로 아래 SEGV (코루틴-참조-매개변수 UAF, v0.2.0 RunConfig 충돌 모양). 수정: `NodeInput`을 값으로 받음. node.h에 문서화. | v0.4.0 |
| 3 ✓ | **CancelToken `fork()` 추가적** — `897645c` 착륙 | `std::shared_ptr<CancelToken> CancelToken::fork()` 추가. 부모 `cancel()`이 자식으로 연쇄. `add_cancel_hook`은 계속 작동 (사용 중단; `[[deprecated]]` 주석은 PR 4에서 착륙). `run_sync(aw, cancel)`이 `cancel->fork()`로 전환. 단일 신호 `slot()` API는 엔진의 외부 co_spawn을 위해 유지. ctest 445 → 452 (7개 새 CancelTokenFork 테스트) + pytest 96/96 + 5개 라이브 LLM/WS 통과. 벤치 A/B 20 라운드 (양방향 교차): Δ 최소 +1.0%, Δ 중앙값 +1.5% — ±5% 대역 내; 벤치 경로에 `cancel_token`이 없어 `fork()`를 만나지 않음, 작은 델타는 바이너리 레이아웃 잡음 (PR3 벤치 바이너리가 PR2보다 3.7KB 작음, 레이아웃 다름). | v0.4.0 |
| 4 ✓ | **사용 중단 주석** — `35a4517` 착륙 | `[[deprecated]]`를 옛 8 가상 함수 + `add_cancel_hook`에 추가 (Hook은 간접적으로 사용 중단). 트램펄린 스코프 (`CurrentCancelTokenScope` / `current_cancel_token()`)는 미룸 — 이는 PR 7 (바인딩 이전)이 `ctx.cancel_token` 읽기로 대체하는 은밀한 채널이므로, 지금 사용 중단하면 명확한 이전 경로 없이 모든 은밀한 지점에서 억제를 강제함. 내부 호출 지점 (graph_node.cpp 기본 체인, 기본 `run()` 전달자)은 새 `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` 매크로로 감쌈 (api.h — GCC/clang/MSVC 이식성). 사용 중단된 가상 함수를 재정의하거나 `add_cancel_hook`을 호출하는 사용자 코드는 이전 경고를 봄; 엔진 내부는 깨끗하게 유지. ctest 452/452 + pytest 96/96 + 5개 라이브 LLM/WS 통과. 벤치 A/B 10 라운드: Δ 중앙값 +0.3%, 최소 +0.8% — 순수 속성 변경, 레이아웃 잡음. `-Werror=deprecated-declarations` 활성화되지 않음 (CI가 원래 `-Werror`를 가진 적이 없음; 경고는 사용 중단 창 동안 정보용으로 유지). | v0.4.0 |
| 5 ✓ | **StateView 정식, raw dict 사용 중단** — `f31aa53` 착륙 | pybind docstring에서 `engine.get_state(thread_id) -> dict`를 소프트-사용중단으로 표시. 새 정식 = `get_state_view(thread_id) -> StateView` (이미 v0.3.2에). `DeprecationWarning` 방출 없음, `[[deprecated]]` 주석 없음 — raw dict는 정당한 용도가 있음 (채널별 `version` 접근, 스냅샷 직렬화). 소프트-사용중단이 큰 피드백을 생성하지 않는 한 v1.0은 탈출구로 유지. 동작 변경 없음. ctest 452/452 + pytest 96/96 통과. | v0.4.0 |
| 6 ✓ | **예제 이전** — `a2a24ef` (PR 6a, C++) + `0a76e3a` (PR 6b, Python) 착륙 | 7 C++ + 19 Python 예제 (총 44개 GraphNode 하위 클래스)가 통합 `run(NodeInput)` API로 전환. PR 6a는 수동 이전; PR 6b는 AST 범위 도우미를 사용해 안전하게 일괄 재작성. 스모크 실행이 v0.3.2 출력과 비트 단위 일치. ctest 452/452 + pytest 96/96 통과. | v0.4.x (6a + 6b로 분할) |
| 7 ✓ | **Pybind 바인딩 이전** — `4e186a5` 착륙 | `PyGraphNodeOwner`가 이제 `GraphNode::run(NodeInput)`을 재정의하고 `has_user_method` MRO 탐색을 통해 Python 사용자의 `run` 메서드로 디스패치; 존재하지 않으면 기존 체인으로 빠짐. `RunContext` / `NodeInput` / `CancelToken`을 Python에 바인딩 (패키지에서 다시 내보내기). 은밀한 `CurrentCancelTokenScope`는 남음 — 기존 체인이 아직 이전되지 않은 사용자 코드를 위해 계속 설치. PR 9가 기존 8 가상 함수와 함께 삭제. ctest 452/452 + pytest 96/96 + 5개 라이브 LLM/WS 통과; 새 `run(input)` API가 end-to-end로 실행됨. | v0.4.x |
| 8 ✓ | **문서 재작성** — `519a00b` 착륙 | `docs/reference-en.md` §6 GraphNode가 단일 `run()` 가상 함수로 축소; 새 RunContext + CancelToken (`fork()` 예제 포함) 하위 섹션이 §7 아래에. README "LangGraph와 다른 점"이 `run(input)`을 가리키는 "하나의 노드 메서드" 항목을 확보. `@ng.node` 데코레이터의 내부 `_DecoratorNode`가 이제 `run()`을 사용하여 5초 데모가 새 경로를 통과. concepts.md / troubleshooting.md 정리는 PR 9로 미룸 (기존 체인이 삭제되면 명백히 오래된 것이 됨). | v0.5.0 |
| 9 ✓ | **옛 API 제거** — 내장 이전 `d1070dc`; 기존 GraphNode 체인 `19819d8`; cancel hook `1d786a5`; thread-local/Python 기존 브리지 `9e8e956`; 폐기된 Python 테스트 `4392fbb`. | v0.9.0 |

## 완료된 v0.4.0 이후 계획 (역사적)

v0.4.0 배포 2026-05-05 (`4cae42c`, 태그 `v0.4.0`). 아래 설명된 관찰 창과 파괴적 제거는 모두 완료됨; v0.9.0이 2026-05-14에 제거를 배포.

### Phase A — 사용 중단 창 (완료)

기간: 주 ~ 한 마이너 사이클. 엔진 코드 변경 없음; 이 단계는 v1.0이 기반 코드를 삭제하기 전에 사용 중단 경고가 실제 다운스트림 파괴를 표면화할 시간을 주기 위해 존재.

관찰할 것:

  1. **사용 중단 가시성** — 사용자가 8개 기존 가상 함수 + `add_cancel_hook`의 `[[deprecated]]` 경고를 실제로 보고 있는가? PR 4 (`35a4517`)가 내부 호출 지점을 `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` 아래에 두었으므로 경고는 사용자 재정의 지점에서만 와야 함. "이 경고 뭐죠?" 언급에 대한 이슈 트래커 / 토론 / 직접 피드백 채널.
  2. **기존 체인 회귀** — 기존 8-가상 기본 체인이 깨지는 새로 발견된 경우 (조용한 무동작, 잊혀진 스코프 등). v0.3.x는 이런 것들의 5 라운드를 가졌다; 하나 더 있을 가능성 있음.
  3. **다운스트림 소비자 파괴** — `GraphNode`의 서드파티 C++ 하위 클래스. 이 저장소의 궤도에서 알려진 소비자:
      - `neoclaw` — `src/neoclaw_nodes.cpp:94`가 여전히 `std::vector<ChannelWrite> execute(const GraphState&) override`를 가짐. v1.0 배포 전에 `run(NodeInput)`으로 자체 이전해야 하며, 그렇지 않으면 neoclaw가 v1.0 wheel에서 깨짐.
      - `NeoProtocol` Executor 런타임 — NeoGraph WASM 빌드 사용; v0.4.0 바인딩 테스트 권장.
      - WASM 스파이크 — 엔진-제로-차이(engine-zero-diff) 경로가 v0.3.x 기준이었음; v0.4 run() 추가는 추가적이므로 아마 괜찮지만 확인.
  4. **신규 사용자 모드 함정 표면** — `ee11ed6` 신규 사용자 정리가 채팅 데모 세션에서 5개 함정을 닫음. 스트리밍 / MCP / 비동기 팬아웃 / HITL 재개는 데모가 건드리지 않은 경로; 비슷한 함정 밀도 가능. 새로운 `cibuildwheel` + 최초 사용자 시뮬레이션, 또는 별도 세션 프라이밍을 통해 표면화.
  5. **선택적 패치 릴리스** — Phase A가 실제 버그를 표면화하면 v0.4.x 패치 배포. v1.0 전에 새 기능이 진짜로 필요하면 v0.5.0 마이너 배포 (여전히 사용 중단 창 안).

종료 기준: Phase A는 사용 중단 경고가 "한 릴리스 동안 조용했을 때" 종료 — 구체적으로, 하나의 전체 마이너 사이클(예: v0.5.0 배포)에서 기존 경로에 묶인 사용자-가시적 파괴가 0건일 때.

### Phase B — 파괴적 제거 (v0.9.0에서 완료)

하위 PR은 아래 순서로 독립적으로 착륙하여 각 단계가 자체적으로 검토되고 되돌려질 수 있게 함.

| 하위 PR | 범위 | 위험 | 변경 파일 |
|---|---|---|---|
| **9b** | `graph_node.cpp` 기존 기본 체인 삭제 (`ExecuteDefaultGuard` 재귀 감지를 갖춘 8-가상 교차 라우팅 로직); `node.h`에서 8개 가상 선언 삭제; `src/core/deep_research_graph.cpp` (5+ 하위 클래스) 및 `src/core/plan_execute_graph.cpp` (3+ 하위 클래스)의 내부 노드를 `execute()` / `execute_full()` 재정의에서 `run(NodeInput)`으로 이전. | **높음** — 모든 내부 GraphNode 하위 클래스가 하나의 PR에서 이전되어야 함. 내장 노드는 이미 PR 9a (`d1070dc`)에서 이전됨; 이 두 그래프 팩토리는 노드가 `nodes/`가 아니라 파일-로컬이기 때문에 남아 있었음. | `node.h`, `graph_node.cpp`, `deep_research_graph.cpp`, `plan_execute_graph.cpp` |
| **9c** | `add_cancel_hook` + `Hook` RAII 클래스 + `hooks_` 멤버 + `hooks_mu_` + `cancel()`의 훅 반복 루프 삭제. `cancel.h`가 `fork()` + `cancel()` + `is_cancelled()` + `slot()` + `bind_executor()`로 축소. | **중간** — `fork()`가 정식 대체, 7개 CancelTokenFork ctest로 실행됨. 실패 모드는 여전히 `add_cancel_hook`을 호출하는 모든 사용자 코드에서 링크 오류 (컴파일 시 잡힘, 조용하지 않음). | `cancel.h`만 (구현은 헤더 전용) |
| **9d** | `CurrentCancelTokenScope` (헤더 + 구현) + `current_cancel_token()` thread_local 접근자 + `state.run_cancel_token_` 멤버 + `set_run_cancel_token` / `run_cancel_token` / `run_cancel_token_shared` 접근자 삭제. `cancel.cpp`가 비게 됨 (파일 제거 가능). `RunContext::cancel_token`이 유일한 경로. | **중간** — 모든 내부 은밀한 지점이 이미 `ctx.cancel_token`을 읽어야 함 (PR 7 바인딩 완료; 제공자 측 읽기는 감사 필요). 실패 모드: 여전히 `current_cancel_token()`을 읽는 제공자가 null 반환 → 취소가 LLM HTTP로 전파되지 않음. | `cancel.h`, `cancel.cpp` (삭제), `state.h`, `graph_state.cpp`, 그리고 `provider/*`에 대한 감사 정리 |
| **9e** | `PyGraphNodeOwner`의 6개 기존 GraphNode 재정의 (`execute(GraphState&)`, `execute_full`, `execute_full_async`, `execute_stream`, `execute_full_stream`, `execute_full_stream_async`) 삭제 — `run(NodeInput)` + `get_name()` + dtor만 유지. `tests/test_node_default_dispatch.cpp` + `tests/test_node_async_default.cpp` + 해당 CMakeLists 항목 삭제. | **낮음** — 순수 제거, 깨질 로직 없음. 실패 모드: `execute()`만 정의한 모든 사용자 Python 클래스 (`run()` 없음)가 디스패치 시 NotImplementedError 발생. Phase A가 이들을 표면화했어야 함. | `bindings/python/src/bind_node.cpp`, `tests/CMakeLists.txt`, 두 테스트 파일 |

9b–e 착륙 후:

  - **SOVERSION 도입** ("범프"가 아님 — 현재 어떤 neograph_* lib에도 `set_target_properties(... VERSION ... SOVERSION ...)`이 존재하지 않음). v1.0.0은 `libneograph_core` / `_llm` / `_postgres` / `_sqlite` / `_mcp` / `_a2a` / `_acp` 전반에 SOVERSION 1을 도입할 자연스러운 순간. cibuildwheel 행렬 확인 (manylinux soname 접미사, macOS install_name, Windows DLL — 각각 SOVERSION을 다르게 처리; "CMake 속성 = 작동한다"고 가정하지 말고 벤치 스타일로 확인).
  - **문서 정리** — `docs/concepts.md` "8 디스패치 진입점" 문단이 하나로 축소; `docs/troubleshooting.md`가 기존 체인 항목 삭제; README "LangGraph와 다른 점"이 "NeoGraph의 사고 방식"으로 변경 (대부분의 LG-델타 항목이 간극이 닫혀 더 이상 적용되지 않음).
  - **v1.0.0 태그 → PyPI 게시** — 마지막 단계. 롤백 비용이 높으므로 (PyPI 릴리스 철회 + 태그 되돌리기), 태그를 푸시하기 전에 전체 ctest + pytest + 5개 라이브 LLM + cibuildwheel 20-wheel 행렬이 통과하는지 확인.

### 이 로드맵에 대한 v0.4.0 이후 사소한 수정

감사에서 발견된 이전 초안의 두 가지 작은 부정확성:

  - **PyGraphNodeOwner 기존 재정의 개수는 7이 아니라 6.** 이전 노트는 "7개 재정의 제거, run()만 남음"이라고 했음. `bind_node.cpp:183`의 `PyGraphNodeOwner`에서 실제 GraphNode 파생 재정의는 6개 (8개 GraphNode 가상 함수에서 `execute_async`와 `execute_stream_async`를 뺀 것 — 이 둘은 재정의된 적이 없음 — 기본 체인이 처리). 9e 이후: `run()` + `get_name()` + dtor가 남음, `run()`만이 아님.
  - **SOVERSION은 "범프"가 아니라 "도입".** `CMakeLists.txt:5` 주석이 SOVERSION을 언급하지만 실제 `set_target_properties(... SOVERSION ...)` 호출이 존재하지 않음. v1.0이 설정하는 첫 버전. 함의: Linux .so / macOS dylib install_name에 SOVERSION 접미사가 나타날 때 cibuildwheel 행렬 실행이 wheel 레이아웃이 퇴행하지 않는지 확인해야 함.

### 역사적 반사실: "제거하지 않으면 어떻게 될까?"

Phase B가 착륙하지 않으면 (기존이 v1.0+에 남으면), 시스템은 **깨지지 않는다** — 모든 현재 시나리오가 계속 작동하고, 452 ctest 모두 통과, 사용 중단 경고는 사용자 재정의 지점에서만 발화. 비용은 급성이 아니라 구조적:

  - **버그-부류 번식지가 열려 있음.** v0.3.x의 5-라운드 취소 전파 패치 시리즈는 같은 패턴이 8개 디스패치 진입점 × 2개 언어를 관통해야 했기 때문에 발생. 기존 체인을 남기면 M-of-N 누락 버그가 다음 교차 관심사(deadline / trace_id / metric handle / 관측 추적)에 대해 계속 사용 가능.
  - **`state.run_cancel_token_` 비채널-집합 멤버**가 명시적으로 전달되지 않는 한 모든 다중 Send 팬아웃에서 누락. 여기에 추가된 모든 새 실행별 필드는 v0.3.1 포인터 누락 버그를 반복.
  - **문서에 두 개의 API 표면** — 신규 사용자는 소스를 읽지 않고 `execute` vs `run`을 구분할 수 없음; `ee11ed6` 신규 사용자 정리의 5개 함정은 정확히 이 문서-간극 모양.
  - **SOVERSION이 깔끔하게 도입되지 않음.** 배포판 패키저(Debian, Homebrew, conda-forge)는 SOVERSION이 없는 라이브러리를 업스트림 관리 부실로 취급.
  - **경고 피로.** 영구적 사용 중단 경고가 사용자를 무시하도록 훈련시켜, 다음 실제 사용 중단이 묻힘.

이 중 어느 것도 오늘 v0.4.0을 깨지 않는다. 이들은 모든 미래 진화를 더 느리고 버그 발생하기 쉽게 만든다. "단일 정식 방법"이라는 v1.0 약속은 이 다섯 가지 모두에 대한 한 번의 답변.

## PR별 계약

각 PR은 반드시:

  - **ctest 442/442 + pytest 96/96을 병합 시점에 깨지 않을 것** (빌드에서 사용 중단 경고는 허용, 오류는 불가).
  - **벤치를 퇴행시키지 않을 것** (`bench_neograph` seq 경로의 중앙값 µs/반복, `feedback_wsl2_bench_isolation.md`에 따라 측정 — 새 worktree, taskset+chrt).
  - **다음 중 최대 하나를 건드릴 것**: 헤더 표면 OR 엔진 내부 OR 바인딩 OR 예제. 혼합 PR은 검토를 어렵게 하고 되돌리기를 비싸게 함.
  - **병합 시 이 표에 행을 추가할 것** — 제안된 줄에 취소선, 병합 커밋 링크, 범위 표류 기록.

## 성능 회고 — `b59444f` 18일 잠복 par 회귀

v1.0 주기 말미에 README의 "엔진 오버헤드" 자랑 (par 11.8 µs)이 깨져 있음이 드러났다. 측정 + 병렬 bisect 결과: 단일 커밋 `b59444f`가 18일 동안 잠복했던 회귀 (2026-04-26 → 2026-05-13).

### 무슨 일이 있었나

- `b59444f`가 `GraphEngine::compile()`의 기본 작업자 수를 `1`에서 `std::thread::hardware_concurrency()`로 변경. 의도: 팬아웃 노드가 명시적 구성 없이 실제 병렬 실행을 받음.
- 부작용: 1-노드 순차 + 5-노드 팬아웃 마이크로 벤치가 반복당 추가 **~75 µs/반복의 스레드 간 제출 비용**을 떠안음. 11.8 µs → 283 µs (24×).
- 2026-04-27 성능 감사 (`project_perf_audit_2026-04-27.md`)가 `fd60aab`을 "수정"으로 기록하지만, 그것은 별도 회귀 (타이밍-측정 패턴)였고 작업자-수 기본값은 변경하지 않음. par 마이크로 벤치 자체가 "default=hardware_concurrency" 모드에서 측정되고 있었으므로 수치적으로 정상으로 보였지만, README의 실제 11.8 µs 주장은 pre-`b59444f` 값.
- v1.0 주기의 PR별 계약이 "벤치를 퇴행시키지 않을 것"을 요구했지만, 당시 벤치는 같은 (퇴행된) 기준에 대해 측정되고 있었으므로 ±5% 대역 안에 들어 조용히 통과. 18일 동안 잠복.
- 2026-05-13, 커밋별 병렬 bisect (11개 병렬 worktree에 대한 `git worktree add`, taskset+chrt 측정)가 `b59444f`를 par 11.8 µs → 283 µs 점프의 단일 책임 커밋으로 확인. 되돌리기 (`e5ecb08`)가 11.8 → 12.2 µs로 복원.

### 트레이드오프 — 왜 default=1이 올바른가

`asio::thread_pool` 스레드 간 제출은 작업당 대략 75 µs 비용. 단일 노드 실행 시간이 ms 단위(LLM 호출, HTTP 등)이면 그 비용은 잡음 속으로 사라짐 — 하지만 NeoGraph의 축하받는 "엔진 오버헤드 순차/병렬 µs-규모" 경로에서는 같은 자릿수이며 직접 드러남.

- **CPU가 작은 / 순차 노드 (마이크로 벤치, 검증기 체인 등)** — default=1이 압도적으로 더 좋음. 작업자 풀 없이 단일 io_context 스레드에서 순차.
- **진짜 팬아웃 의도 (sleep-bound 시뮬레이션, 별도 프로세스 호출, 동기 HTTP)** — 사용자가 명시적으로 `engine->set_worker_count_auto()` 또는 `set_worker_count(N)`을 호출해야 함. 한 줄.

이 트레이드오프를 명시적으로 만들기 위해, `e5ecb08`의 커밋 메시지 + 다음 팬아웃 예제 5곳 (10/14/21/36 + `deep_research_graph` 빌더)이 명시적 `set_worker_count_auto()` 호출을 추가했고, 이전 문서의 Migration 3 섹션이 강화됨.

### PR별 계약 강화 (다음 회귀 방지)

`bench_neograph par` 마이크로 벤치가 기준의 ±5% 이내인지만 확인하는 것은 불충분했다 — 기준 자체가 퇴행했을 때 함께 미끄러져 내려감. 후속 패치에서:

  - 벤치-회귀 CI가 **README에 명시된 절대값** (`seq ≈ 5.0 µs`, `par ≈ 12 µs` 등)을 두 번째 wall-time-앵커 게이트로 사용. 기준-자체 회귀를 잡아냄.
  - 또는 master → master 7일 회귀 측정을 위한 GitHub Actions cron 추가 (야간-soak 스타일 패턴).
  - PR별 차이가 `GraphEngine::compile()` 또는 `set_worker_count`를 건드리면 PR 본문이 "별도 마이크로 벤치 측정 결과"를 포함해야 함 (CODEOWNERS 훅으로 자동화).

세 가지 모두 후속 작업. v1.0 릴리스 전에 최소 하나는 착륙해야 함.

### 배운 것

1. **"기본값 변경"은 기능적 의미가 없어도 성능-중요 계약이 될 수 있다.** README의 자랑 숫자가 "기본" 경로에서 나온다면, 기본값 변경 = README 변경.
2. **회귀 측정의 기준 자체가 퇴행할 수 있다.** ±대역 비교만 하지 말고 절대값 앵커도 설정.
3. **병렬 bisect (11개 병렬 worktree + 결과 집계)가 18일-잠복 회귀를 30분 만에 정확히 찾아냈다.** 선형 bisect보다 훨씬 빠름 — master가 길어졌을 때의 기본 도구.

## 리팩터 중 피해야 할 v0.3.x 함정의 기억

빌드/릴리스 파이프라인이 v0.1.x → v0.3.x에서 지뢰를 축적했다. 각각은 메모리 항목을 가짐 — 이 표는 관련 영역을 건드릴 때의 체크리스트:

| 함정 | 물어뜯는 곳 | 메모리 항목 |
|---|---|---|
| 모든 공개 클래스 + 자유 함수에 `NEOGRAPH_API` 매크로 | 새 엔진 하위 라이브러리 (postgres / sqlite / mcp / a2a / acp). Windows DLL 경계. | `feedback_neograph_api_discipline.md` |
| 브랜치 간 오래된 .so 오염 | 브랜치 간에 사용되는 `BUILD_SHARED_LIBS=ON` build/ → compile()에서 ABI 불일치 SEGV | `feedback_cross_branch_stale_so_trap.md` |
| 벤치 측정 시 빌드 디렉터리 오염 | 오래된 build/ 디렉터리가 새 worktree 빌드보다 느린 바이너리 생산 (+0.4 µs/반복 거짓 신호) | `feedback_bench_build_dir_contamination.md` |
| WSL2 측정 지터 | 평범한 "많은 반복 + 중앙값"이 수렴하지 않음 — taskset + chrt FIFO 99 필요 | `feedback_wsl2_bench_isolation.md` |
| Doxygen `/*` 와일드카드 in comments | `/**` 안의 `fs/*` / `terminal/*`이 중첩 주석을 열고 후속 진단을 억제. `&#42;` HTML 엔티티 사용. | `feedback_doxygen_slash_star_trap.md` |
| ASan `__cxa_throw` 인터셉터 CHECK | pybind 경계를 가로지르는 C++ 예외가 `LD_PRELOAD libasan.so` 아래 인터셉터를 발화. CI에서 키워드로 선택 해제; 취소/던지기 정확성은 TSan + 라이브 LLM 테스트로 실행. | (이번 세션 — 피드백에 노트 추가) |
| TSan eptr 수명 경쟁 | NodeInterrupt의 exception_ptr이 co_await 경계를 가로질러 libstdc++ `__exception_ptr::_M_release`를 발화. 수정: 이유를 `std::string`으로 추출, 메인 스레드에서 새로 던짐. | `feedback_parallel_group_eptr_race.md` |
| MSVC는 명시적 `<array>` / `<algorithm>` 필요 | libstdc++가 전이적으로 포함; MSVC v143은 안 함. `std::array` 등을 사용하는 테스트 파일이 Windows CI를 조용히 깸. | (이번 세션 — 피드백에 노트 추가) |
| scikit-build-core 0.12.2 Windows single_config | `-G` 플래그 감지됨, 환경 변수 무시됨 — Windows wheel이 SQLite=OFF 재정의를 잃음. `[[tool.scikit-build.overrides]]` + `cmake.define` 사용. | `feedback_libcurl_unconditional_dep.md` |
| Wheel OpenSSL CA 경로 | manylinux libssl이 Ubuntu에 없는 AlmaLinux 경로 사용. `__init__.py`가 certifi에서 `SSL_CERT_FILE` 자동 설정. | `feedback_wheel_openssl_ca.md` |
| pyproject.toml 런타임 의존성이 CI의 PYTHONPATH 흐름에서 자동 설치되지 않음 | `pip install --quiet pytest` 줄이 pyproject.toml의 `dependencies = [...]`를 반영해야 함. v0.3.2가 pydantic에 대해 이것을 잃음. | (이번 세션 — 피드백에 노트 추가) |
| `compile()` 기본 작업자 수 회귀 | `b59444f`가 기본값을 `1 → hardware_concurrency`로 변경, 잠복 par 마이크로 벤치 11.8 → 283 µs (24×). 기준-자체-퇴행 패턴. `e5ecb08`에서 수정. | "성능 회고" 섹션 (위) |

리팩터 PR이 새 하위 라이브러리, 새 공개 클래스, 새 런타임 의존성, pybind를 가로질러 던지는 새 테스트 패턴, 새 wheel 플랫폼을 추가하면 — 이 표를 먼저 열 것. v0.3.x 패치 시리즈의 절반은 이미 이 목록에 있는 항목을 재발견하는 것이었다.

## 문서 영향 맵

리팩터가 착륙할 때 이 페이지들에 편집 필요:

  - **`README.md`** — "Python Binding" 섹션의 RunConfig 표, "LangGraph와 다른 점" 델타 (대부분 항목이 폐기되어 삭제되어야 하며 편집이 아님), "바인딩이 커버하는 것" 표면 목록.
  - **`docs/reference-en.md`** (1622줄) — GraphNode / Node / Provider / CancelToken / RunConfig 섹션. 대략 30-40% 재작성. 서사-투어 구조는 유지; API 표면은 축소.
  - **`docs/concepts.md`** — 531줄 개념 서사. "8 디스패치 진입점" 문단이 하나로 축소. 취소 전파 문단 정리.
  - **`docs/troubleshooting.md`** — 대부분의 v0.3.x 항목이 폐기됨. "조용한 무동작" / "재정의 잊음" / "thread_local 누락" 항목 삭제 가능.
  - **`bindings/python/examples/`** — 모든 예제 (22 Python + 30 C++) 갱신.
  - **`Doxyfile`** — 변경 없음; PROJECT_NUMBER가 pyproject.toml에서 읽히므로 v1.0.0이 자동 전파.
  - **`ROADMAP_v1.md`** (이 파일) — 착륙한 후보에 취소선, 예상보다 어려웠거나 쉬웠던 점에 대한 사후 분석 추가.

## v1.0의 완료 정의

  1. 각각에 대해 단일 정식 방법: 노드 작성, 실행 취소, 상태 읽기, 상태 갱신, 그래프 실행.
  2. README의 "Python Binding" 섹션이 신규 사용자가 5분 안에 읽을 수 있음.
  3. `docs/reference-en.md` GraphNode 섹션이 8개가 아니라 하나의 메서드.
  4. v0.3.x 사용 중단 경고가 최종 제거 전에 최소 한 릴리스 동안 조용했음.
  5. ctest / pytest / 라이브 LLM / Valgrind / Doxygen이 v1.0 태그에서 모두 통과.

---

# 연구 트랙 (위의 v1.0 다듬기보다 부하 지탱이 덜함)

## Candidate 4 — 자기 진화 JSON 에이전트 v2 (연구)

### 컨텍스트

`bindings/python/examples/22_self_evolving_graph.py`가 루프가 닫힘을 증명: LLM 수정자가 실행 중인 그래프에 JSON 편집을 제안하고, 엔진이 재컴파일하며, 새 그래프가 실행됨. PoC는 작동하지만 LLM이 편집 제안 시 채널 데이터 흐름을 추론하는 데 어려움을 겪음 — 어떤 노드가 어떤 채널을 읽고 쓰는지 "보지" 못해서, 제안이 자주 잘못된 전선으로 데이터를 라우팅.

### 연구 방향

채널 토폴로지를 수정자 프롬프트에 명시적으로 노출. 조사할 두 가지 형태:

1. **프롬프트 내 토폴로지 요약** — 엔진이 컴파일된 채널 접근 패턴에서 파생된 `"node X reads {a,b}, writes {c}"` 같은 노드별 명세를 방출. 수정자 프롬프트가 JSON 정의와 함께 이를 받음.

2. **단계별 채널 제안** — 수정자가 평면 집합이 아니라 *단계별* (분할 / 합성 / 등)로 채널을 제안. 엔진이 각 제안 단계의 채널 집합이 업스트림/다운스트림 단계와 일관되는지 구성-검사.

### 왜 v1.0 필수가 아닌가

- 배포된 엔진의 사용자 장애 요소가 아님 — PoC의 간극은 *프롬프트 엔지니어링*에 있지 엔진에 있지 않음.
- 모든 엔진 측 표면 변경이 정당화되기 전에 LLM 평가 도구(토폴로지 변종별 정확도율, 비용, 수렴까지의 편집 주기)가 필요.
- 평가가 LLM이 실제로 사용하는 내부 구조 조회를 보여주면 더 넓은 "그래프 내부 구조 조회 API" v1.x 기능으로 접힐 수 있음.

### 비용

- 연구가 검증하면 엔진 표면 추가(토폴로지 접근자)는 작음.
- 대부분의 작업은 이 저장소의 핫 경로 밖의 LLM 측 실험.

### 발화 라운드

TODO_v0.3.md 항목 #8 — 연구로서 v0.3.x에서 미룸, 사용자 장애 요소 아님.

## Candidate 5 — 쿡북 트랙: pgvector RAG 예제

### 컨텍스트

`bindings/python/examples/` (23개 예제)가 ReAct, HITL, 의도 라우팅, 다중 에이전트 토론, 심층 연구 (웹 크롤 / 웹 검색), 자기 진화 그래프 등을 커버 — 하지만 **벡터 검색 / RAG 예제 없음**. 16/17로 접히지 않음 확인: 그것들은 임베딩 기반 검색이 아니라 웹 연구.

RAG는 가장 흔한 LLM 패턴 중 하나; 부재는 NeoGraph를 평가하는 사용자에게 실제 발견 가능성 간극.

### 왜 엔진 관심사가 아니라 쿡북 항목인가

엔진 표면은 현재 그대로 충분 — `PostgresCheckpointStore`가 이미 연결 풀 / 구성 이야기를 가져와서 임베딩 + pgvector 노드가 재사용 가능. 엔진 API 추가 필요 없음; 작업은 순수한 예제 (~150-200줄):

  - `EmbeddingNode` — OpenAI 임베딩 또는 로컬 모델 호출.
  - `RetrieveNode` — 미리 채워진 테이블에 대한 pgvector 유사도 쿼리.
  - `RAGCallNode` — 검색된 컨텍스트를 연결한 LLM 호출.
  - 일회성 색인 설정 스크립트 (예제가 실행마다 재색인하지 않도록 예제 본문과 분리).

### 왜 v0.3.x에서 미뤘는가

v0.3.x 시리즈는 FastAPI SSE 채팅 데모 피드백이 드러낸 엔진 버그 / 사용 편의성을 중심으로 범위가 정해졌다. RAG는 엔진 버그가 아님; "흔한 패턴에 작업 예제가 필요함." 각 항목이 실제 레시피인 별도 쿡북 북소리에 속하며 v-범프가 아님.

### 발화 라운드

TODO_v0.3.md 항목 #9 — 쿡북 자료 확인 (엔진 간극 없음), 미래 "예제 트랙" 정리로 미룸.

---

## Candidate 6 — Provider 단일 디스패치

### 증상

`Provider`가 네 개의 가상 메서드를 노출 (GraphNode의 8개보다 한 차원 작음):

```
complete           complete_async
complete_stream    complete_stream_async
```

`(sync/async) × (stream/non-stream)` — Candidate 1과 같은 모양, 같은 "N개 중 최소 하나 재정의" 계약, 같은 함정. 비스트리밍 쌍은 안전 (브리지 한 단계 깊이). 스트리밍 쌍은 pre-#10에서 안전하지 않았음: `complete_stream`은 동기 httplib, 기본 `complete_stream_async` 브리지가 인라인으로 감쌌으며 (그리고 WebSocket Responses 경로가 엔진의 io_context 작업자 위에 `run_sync`를 중첩), `GraphEngine::run_stream_async` 안에서 호출될 때 간헐적 segfault 발생 (이슈 #4, PR #10의 작업자-스레드 브리지 + `SchemaProvider` 네이티브 재정의로 수정).

### 왜 이것이 장애 요소가 아니라 정리인가

구체적 충돌 (#4)은 닫힘 — PR #10이 동기 `complete_stream`을 위해 전용 작업자 스레드를 생성하고 토큰을 대기 중인 실행자로 다시 디스패치; `SchemaProvider`가 WS 경로를 재정의하여 작업자 스레드조차 건너뜀. `OpenAIProvider`와 `SchemaProvider` HTTP/SSE 경로 모두 안전한 기본값을 상속하므로, 4-가상 데카르트 곱은 더 이상 충돌 위험이 없음. 남은 것은 아키텍처 사마귀: 재정의 표면이 필요 이상으로 넓고, 브리지의 안전성은 데카르트 곱의 어느 모서리를 재정의하느냐에 달려 있음 (컴파일 시 아무것도 고정하지 않는 불변성).

### 착륙한 방향

하나의 재정의 경로는 대체가 아니라 추가적:

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

`CompletionRequest`는 콜백 존재에서 전송을 추론하지 않고 수집 또는 스트림 모드를 명시적으로 선택. 최종 어댑터가 모든 기존 `Provider` 진입점을 보존. 이는 새 구현에 하나의 재정의를 제공하면서 기존 소스 및 바이너리 계약을 그대로 유지.

### 인접 — `schema_provider.cpp` 분할

`schema_provider.cpp`는 다중 벤더 스키마 매핑 + HTTP/SSE 전선 + 본문-빌드 + 응답-파싱을 집중시키는 ~1,800 LoC. 단일-디스패치 재작성은 `SchemaParser` / `SchemaWireBuilder` / `SchemaProviderImpl`로 분할할 자연스러운 순간. 별도 ROADMAP 항목이 필요하지 않도록 여기에 언급; 작업이 다른 PR에서 발생하면 분할 가능.

### 발화 라운드

이슈 #5 — #4 디버깅 중 표면화. 구체적 충돌은 PR #10에서 닫힘; 아키텍처 정리는 추가적 `CompletionProvider` 경로와 영구 호환성 정책을 통해 착륙.

### 착륙 로그 (v0.9.0 후보 주기)

5개 PR이 2026-05-13 중순 순차적으로 master에 착륙:

| PR | 범위 | 착륙 버전 |
|---|---|---|
| **#40 (PR1)** | 새 가상 `Provider::invoke(params, on_chunk = nullptr)` 추가. 기본 구현이 4개 기존 가상 체인으로 전달 (모든 기존 Provider 하위 클래스는 변경 없이 동작). 6개 새 ctest. | v0.9.0 |
| **#41+#42 (PR2)** | 엔진 내장 LLM 노드 (`LLMCallNode`, `IntentClassifierNode`)가 `provider->invoke(params, on_chunk)`로 디스패치. PR #41은 쌓인 기준에서만 병합된 후 PR #42를 통해 master에 재적용. | v0.9.0 |
| **#43 (PR3)** | 모든 엔진 내부 동기 LLM 호출 지점 이전 — `agent.cpp` (5곳), `deep_research_graph.cpp` (6곳). `Provider::invoke()` 기본값에 스레드-로컬 취소 전파 동등성 추가 (기존 `complete()`의 `current_cancel_token()` 동작 재현). 3개 새 ctest. | v0.9.0 |
| **#44 (PR4)** | 4개 기존 가상 함수 모두 `[[deprecated]]`로 표시. `plan_execute_graph.cpp`의 3곳을 `invoke()`로 이전. `OpenInferenceProvider`와 `RateLimitedProvider` (데코레이터)의 4개 가상 재정의 블록을 `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED`로 감쌈 — 내부 전달자 경고 차단; 사용자 대면 재정의 / 호출 지점만 경고. | v0.9.0 |
| **#45 (PR5)** | C++ 예제 이전 (`31_local_transformer.cpp`, `cookbook/ai-assembly/member_server.cpp`). | v0.9.0 |

### 추가적 호환성 경로 (2026-07 개정)

기존 `Provider` vtable을 유지하고 별도 `CompletionProvider` 추가. 새 구현은 명시적 `CompletionRequest`를 받고 `do_invoke()`만 재정의. 기존 네 가상 함수와 콜백 기반 `invoke()`는 최종 어댑터를 통해 새 구현에 배선됨; 기존 Provider 하위 클래스와 Python 트램펄린은 그대로 보존.

기존 가상 함수는 제거 계획 없이 안정된 API로 계속 지원. 호환성 및 보안 수정은 계속 적용되지만, 모든 새 기능을 기존 네 가상 함수로 백포트할 의무는 없음. 새 구현과 새 직접 호출은 각각 `do_invoke()`와 `invoke_request()`를 사용해야 함. 이 정책은 기존 공개 시그니처, 가상 함수 순서, 객체 크기, vtable을 변경하지 않음. #127의 네이티브 비동기 전송과 작업 소유권은 이 API 정책과 별개.

후속:

  - **6b**: 새 Provider는 `CompletionProvider`에 대해 작성. ABI 영향 때문에 기존 내장의 직접 상속을 변경하지 않음.
  - **6c**: 네이티브 비동기 전송과 요청 소유 취소 / 수명 완성.
  - **인접**: `schema_provider.cpp` (1800 LoC)를 `SchemaParser` / `SchemaWireBuilder` / `SchemaProviderImpl`로 분할 (위의 6b와 같은 PR 또는 별도 — 구현 시 결정).

---

## Candidate 7 — gRPC 전송 (선택적 구성 요소)

### 컨텍스트

HasMCP 콜드 이메일(2026-05-15)이 계기는 아니었지만, 그 이메일은 "gRPC가 다음 전송 방향이다"라는 무료 업계 신호를 주었다. MCP 커뮤니티가 [gRPC를 표준 전송으로 추가](https://github.com/modelcontextprotocol/modelcontextprotocol/issues/966)하는 것을 논의 중이며, Google은 gRPC-as-native-MCP-transport 작업 중. gRPC는 NeoGraph의 4축 서사의 거의 모든 축과 정렬 — protobuf 바이너리 직렬화 (성능 / 가벼움), HTTP/2 다중화 (멀티 테넌트 연결 비용), 네이티브 양방향 스트리밍 (토큰 / 이벤트), 그리고 스키마 강제 + 작은 전선 (임베디드).

### 결정 (2026-05-15)

- **자체 구현 없음.** 통신 프로토콜은 바퀴 재발명 위험이 큼 — 표준 `grpc++` + `protoc` 사용.
- **선택적 전용.** `NEOGRAPH_BUILD_GRPC` 옵션, **기본값 OFF**. grpc++가 protobuf + abseil + c-ares + re2 + zlib (수십 MB 전이)를 끌어와 "2 deps / libc.so.6 only / 1.2 MB binary" 가벼움 축을 깸. 기본값 OFF가 그것을 막는 유일한 방법이며, `cmake-option-default-flip-trap` (EDDSkills, 이번 세션에 새로 추가) 규율을 적용: `find_package(Protobuf/gRPC)`는 옵션 게이트 안에서만; 기본값 뒤집힘 없음.
- **MCP 표준과 독립적인 NeoGraph-네이티브 API.** `proto/neograph.proto` = `GraphService { RunGraph(unary) / RunGraphStream(server-stream) / Health }`. 페이로드는 JSON 문자열 (그래프-데이터 속성 보존 — 강타입 proto 메시지로 모델링하면 모든 사용자 그래프 변경이 .proto를 재생성). MCP-over-gRPC 표준이 확정되면 이 서비스 옆에 MCP 모양의 서비스 추가 (이 서비스는 변경 없음).

### 착륙 (v0.9.x 주기, 스캐폴드)

- `NEOGRAPH_BUILD_GRPC=OFF` 옵션 + 조건부 `find_package` + 치명적 보호.
- `proto/neograph.proto`, `src/grpc/graph_service.cpp` (해시 키 컴파일 캐시 — multi_tenant_chatbot 쿡북 패턴 재사용), `include/neograph/grpc/graph_service.h` (`NEOGRAPH_HAVE_GRPC` 보호), `examples/52_grpc_server.cpp`.

### 검증 — 첫 grpc++ 장착 빌드 (2026-05-16)

apt `libgrpc++-dev protobuf-compiler-grpc` (1.51.1) + protoc 3.21.12 설치 후,
`-DNEOGRAPH_BUILD_GRPC=ON`으로 빌드하고 end-to-end 통과:
  - `neograph_grpc` / `example_grpc_server` / `example_grpc_client` 모두 컴파일 및 링크 OK.
  - C++ 클라이언트 → 서버: **Health** (ok/version/default_graph), **RunGraph** 단항 (`{"text":"hello from grpc"}` → `"HELLO FROM GRPC"`, trace=[upper]), **RunGraphStream** (5개 이벤트, FINAL 페이로드, status OK). `RESULT: PASS (failures=0)`.
  - protoc 코드 생성 경로 (raw `add_custom_command`) 작동. **버그 하나 수정**: VERBATIM 모드에서 `ARGS --proto_path="${dir}"`의 따옴표가 문자 그대로 전달되어, protoc이 `"…/proto"` (따옴표 포함)을 디렉터리로 봄 → "directory does not exist". 따옴표 제거 (`--proto_path=${dir}`)로 닫힘.

### WSL Windows-PATH 누출 함정 (재현 가능 — 빌드 환경 경고)

grpc++ ON으로 이 환경(WSL2, 거대한 Windows PATH 누출)에서 빌드할 때 두 가지 오염이 잡힘. 깨끗한 Linux 호스트 / CI에서는 나타나지 않지만 WSL 개발자는 만남:

  1. **anaconda re2** — `gRPCConfig.cmake`가 `find_package(re2)`를 할 때, 시스템 re2 cmake 구성이 존재하지 않으면 (apt `libre2-dev` 미설치), PATH에서 `/mnt/c/ProgramData/anaconda3/Library/lib/cmake/re2/re2Targets.cmake` (Windows)를 잡아 `set_target_properties`에서 오류. 수정: `-DCMAKE_IGNORE_PREFIX_PATH=/mnt/c;…` + `-DCMAKE_IGNORE_PATH=…/anaconda3/Library/lib/cmake;…` → grpc가 시스템 pkg-config re2로 대체 ("Found RE2 via pkg-config").
  2. **ZLIB include** — `FindZLIB`가 라이브러리는 시스템 (`/usr/lib/.../libz.so`)에서, `ZLIB_INCLUDE_DIR`은 PATH의 `/mnt/c/gtk/include` (Windows zlib.h)에서 가져옴 → `-isystem /mnt/c/gtk/include`가 모든 grpc 연결 대상으로 누출 → `/mnt/c/gtk/include/libintl.h`가 `printf`를 `libintl_printf` 매크로로 재작성 → `std::printf` 컴파일 오류. 수정: 명시적으로 `-DZLIB_INCLUDE_DIR=/usr/include -DZLIB_LIBRARY=/usr/lib/x86_64-linux-gnu/libz.so` 설정.

  → 둘 다 `cmake-option-default-flip-trap`의 사촌 (환경 누출이 `find_package`를 잘못된 접두사로 끌어감). EDDSkills SKILL `wsl-windows-path-cmake-find-leak`이 추가됨 (2026-05-16).

### NexaGraph 선행 분석 — gRPC-MCP의 실제 ROI는 체크포인트

NeoGraph의 전신 NexaGraph (`/root/Coding/NexaGraph`)는 이미 gRPC-MCP를 초기에 구현하고 운영했었다. 조사 결과 (Explore, 2026-05-16):

- **구현 실체**: `proto/rag_service.proto` (RAGService, 11개 단항 RPC — vector_search / graph_search / ingest / chat history / image task / **graph checkpoint** 5개 RPC), `src/nexagraph/grpc_client.cpp` 완전 구현, api_server.cpp에서 `GRPC_TARGET` env를 통해 운영 통합. 서버는 이중 전송 (HTTP JSON-RPC + gRPC 50051). 스트리밍 없음 (모두 단항).
- **오버헤드 감소 주장** (`DOCS/grpc-client-plan.md`): 직렬화 1ms→0.01ms, 임베딩 1536d 15KB→6KB, 요청당 새 연결 → HTTP/2 다중화. **측정 없음 — 설계 근거만.**
- **정직한 평가**: 일반적인 MCP 도구 호출의 경우 LLM 추론 (수백 ms)이 지배적이므로, 1ms 직렬화 절약은 잡음. gRPC의 이득이 *실제로 존재하는* 영역은 **큰 구조화된 페이로드** — 임베딩 벡터, RAG 수집, 그리고 특히 **그래프 체크포인트** (`channel_values_json` + `channel_versions_json`이 매 단계마다 커짐). 작은 도구 메타데이터 / 문자열 쿼리는 <1% (인지 복잡성이 가치가 없음). 다시 말해, "MCP가 일반적으로 더 빠르다"가 아니라 "큰-페이로드 MCP"로 제한.

**핵심 발견 — NeoGraph 도입 시 우선순위 재조정:**

1. **gRPC CheckpointStore = 실제 ROI (최우선 후보)**. NexaGraph의 `grpc_checkpoint.cpp`가 이미 **`neograph::graph::CheckpointStore`를 상속** — 그 시점부터 NeoGraph의 체크포인트 추상화 사용. 다시 말해, NeoGraph의 `Postgres/Sqlite CheckpointStore` 옆에 `GrpcCheckpointStore`를 추가하는 형태로 거의 그대로 이식 (~150 LoC). 큰 페이로드 + (MCP #966) 표준과 독립 + 방금 구축된 `neograph::grpc` 구성 요소 안에 자연스럽게 들어맞음. 체크포인트는 매 단계 큰 JSON 블롭, gRPC 바이너리 이득이 실제로 측정 가능한 유일한 핫 경로.
2. **MCP-over-gRPC 전송 (일반 도구 호출) = 낮은 우선순위**. LLM 지배적이라 이득이 작음 + MCP-over-gRPC 표준이 아직 확정되지 않음 (#966). 표준이 확정된 후에도 RAG / 임베딩 같은 큰-페이로드 도구에만.

### GrpcCheckpointStore — 착륙 + 측정 (2026-05-16)

`neograph::grpc`에 추가: `GrpcCheckpointStore` (클라이언트, `CheckpointStore` 상속 — NexaGraph와 동일) + `CheckpointServiceImpl`+`run_checkpoint_server` (서버, 임의의 `CheckpointStore` 백엔드 감싸기) + `checkpoint_to/from_json` 도우미. `CheckpointService` proto에 5개 RPC. NexaGraph의 평면 매핑이 처리할 수 없었던 NeoGraph의 풍부한 필드 (next_nodes 벡터 / `CheckpointPhase` 열거형 / `barrier_state` 중첩 맵 / `schema_version`)의 왕복 보존 — 예제 54 정확성 PASS.

**측정 결과 (example_grpc_checkpoint, 1536-d 임베딩 + 12턴, 200회 반복, localhost 루프백) — "그럴듯하지만 입증되지 않음" 닫힘. 하지만 정직하게 말하면 절반은 기각:**

| 지표 | 값 |
|---|---|
| JSON (checkpoint_json) | 29 080 B |
| Protobuf 전선 (CheckpointBlob) | 29 131 B |
| 개념적 JSON-RPC 봉투 | 29 155 B |
| protobuf / JSON-RPC 페이로드 | **99.9%** |
| InMemory 인프로세스 | save 27 µs / load 36 µs |
| gRPC 왕복 | save 720 µs / load 755 µs |
| gRPC 네트워크 오버헤드 | **+693 µs save / +719 µs load** |

**정직한 결론 — NexaGraph의 "직렬화 15KB→6KB 바이너리 압축" 주장은 NeoGraph의 JSON-in-proto 설계에서 충족되지 않음 (페이로드 99.9% 동일).** 이유: 그래프-데이터 견고함을 위해 전체 체크포인트가 단일 proto 문자열 필드로 패킹됨 → protobuf 필드 수준 압축이 적용되지 않음. NexaGraph는 멤버별 proto 필드가 있어 압축되었지만, 모든 체크포인트 형식 변경 (`next_nodes` / `barrier_state` / `schema_version` 추가 시마다)은 proto 재생성이 필요. 다시 말해, **이 트레이드오프에서 의도적으로 압축보다 스키마 안정성을 선택했으므로, 페이로드 이득은 실제로 0 (입증: 설계상 이익 없음).**

gRPC의 *실제* 이득은 전송만 — HTTP/2 연결 재사용 (JSON-RPC / HTTP1.1의 호출당 연결 제거). 단일 루프백 왕복의 +700 µs는 이것을 보여주지 않음 (델타는 부하 / 원격 RTT 아래에서만 나타남). 다시 말해, **전송 이득은 여전히 부하 테스트 의존적 — 단일 측정으로 입증할 수 없음.**

→ 우선순위 재확인:
  - **GrpcCheckpointStore의 실제 가치 = "타입 있는 RPC를 통한 원격 체크포인트 + HTTP/2 연결-재사용" + "다언어: 어떤 언어 서버든 `CheckpointService` 구현 가능"**. NexaGraph가 광고한 페이로드 압축이 아님. 쿡북으로 배포하되, 정직한 판매 포인트는 "압축"이 아니라 "타입 있는 원격 체크포인트, 에이전트 프로세스에 DB 드라이버 0개".
  - **MCP-over-gRPC 전송 (일반 도구 호출) = 보류**. 체크포인트 측정에서 확인된 대로 JSON-in-proto에서는 페이로드 압축이 적용되지 않으므로, 도구 호출이 같은 설계를 따르면 압축 이득은 0 + LLM 지배적. 표준 (#966)이 확정되고 필드-멤버별이 정당화되는 큰 바이너리 도구(원시 임베딩 등)에 대해서만 재고려.
  - 남은 검증: 부하 아래 (N 동시 체크포인트 저장), HTTP/2 다중화가 실제로 호출당-연결 대비 델타를 내는지 — 벤치 작업 후보 (단발이 아닌 지속).

### ToolCalling: JSON-RPC vs gRPC 직접 비교 (2026-05-16)

사용자 요청 — 체크포인트가 아니라 *도구 호출* 자체, 두 전송을 실제 서버에서 직접 비교. `proto`가 `ToolService.CallTool`을 얻고, 예제 55가 **동일한 계산 함수를 (a) httplib JSON-RPC 2.0 `tools/call` (MCP 모양, HTTP/1.1 keep-alive) (b) gRPC ToolService (HTTP/2)** 양쪽에서 시작하고 동일하게 측정.

**정직성 사건 — "gRPC 70배 빠름"은 측정 아티팩트였다.** 첫 실행: JSON-RPC p50 43 ms (페이로드와 무관하게 일정). 43 ms = TCP delayed-ACK 타이머의 교과서적 서명. 원인: `CPPHTTPLIB_TCP_NODELAY` 기본값 **false** → Nagle 켜짐, gRPC는 TCP_NODELAY 기본값 켜짐 → 불공정. "gRPC 70×"를 그대로 커밋하는 것은 거짓이었을 것. 양쪽에 `Server/Client::set_tcp_nodelay(true)` 적용 후 재측정.

**공정 조건 결과 (루프백, 양쪽 keep-alive + NODELAY, N=300 p50, 2회 재현):**

| 페이로드 | gRPC p50 | JSON-RPC p50 | 비율 |
|---|---|---|---|
| 작은 인자 (~30 B) | 433 / 448 µs | 436 / 410 µs | **0.99–1.09× (무승부)** |
| 1536-float (~12 KB) | 655 / 680 µs | 1079 / 1016 µs | **0.61–0.67× (gRPC ~1.5×)** |
| 인자 전선 (작은) | 42 B | 118 B | 봉투 오버헤드 |
| 인자 전선 (12 KB) | 12025 B | 12100 B | **99% (압축 0)** |

**진실:**
- **작은 도구 호출 (실제 도구 호출의 대부분): JSON-RPC ≈ gRPC 무승부.** 전송-전환 ROI ≈ 0.
- **큰-페이로드 도구 호출 (~12 KB+, 임베딩 / RAG 청크 반환): gRPC ~1.5×.** NexaGraph가 언급한 영역이지만 70×가 아니라 1.5×.
- 페이로드 압축은 여전히 0 (JSON-in-proto, 체크포인트 측정과 일관).
- 루프백 상한 — 실제 네트워크에서는 RTT가 양쪽에 동등하게 더해지고 비율은 1로 더 수렴. 1.5×가 최선의 경우.

**Candidate 7 최종 평결:**
- gRPC의 ROI는 (1) **다언어 사이드카 / 원격 타입 있는 RPC** (언어 경계), (2) **큰-페이로드 도구 / 체크포인트에서 ~1.5×**. 일반 도구 호출의 대규모 이전은 무가치 (무승부 + 표준 #966 미확정).
- MCP-over-gRPC 전송: **보류, 확인**. "일반 MCP 도구 호출이 더 빨라진다"는 측정으로 반증됨 (무승부). 표준이 확정된 후에만, 그리고 임베딩이 무거운 도구에만.
- Nagle 사건 → EDDSkills SKILL 후보 `bench-shock-number-nagle-first` (충격적인 전송-벤치 숫자 = TCP_NODELAY / Nagle / delayed-ACK을 먼저 의심; `perf-regression-bench-bisect`의 사촌). 사용자 승인 후 추가.

### 왜 NeoGraph JSON-RPC가 gRPC와 무승부인가 — yyjson (입증)

사용자 통찰: "JSON-RPC는 yyjson으로 파싱하므로 빠르다; 구조적으로 gRPC가 이겨야 한다." 예제 55에 전송을 제거한 순수 코덱 마이크로벤치를 추가하여 검증:

| 12 KB 페이로드, 코덱 전용, 5000회 반복 | µs |
|---|---|
| yyjson parse+dump | **38.9** |
| protobuf ser+parse | **1.75** |
| → yyjson / protobuf | **22.3× 더 느림** |

**사용자가 정확히 맞다.** protobuf는 구조적으로 22× 더 빠른 코덱. 하지만 왕복에서 그 차이는 12 KB에서 1.5×로 희석 — 직렬화 간극 ~37 µs가 전체 왕복 692–1096 µs의 작은 조각 (나머지는 소켓 I/O / syscall / HTTP 프레이밍). **도구 호출 핫 경로가 코덱이 아니라 소켓 I/O에 지배된다는 정량적 증거.**

핵심 함의 — **NeoGraph의 JSON-RPC가 gRPC와 무승부인 것은 yyjson 덕분이지, JSON-RPC 프로토콜이 빠르기 때문이 아니다.** 일반적인 스택 (Python의 `json`은 yyjson보다 ~50× 느림, 12 KB에서 ~2 ms)에서는 코덱이 왕복을 지배 → 거기서는 gRPC가 구조적으로 지배. NeoGraph만 yyjson을 사용하므로 그 함정을 피함.

→ 이것은 숨은 판매 포인트이자 Candidate 7 보류의 *최종* 정당화: "다른 프레임워크는 JSON-파싱이 병목이므로 gRPC 전송이 중요하지만, NeoGraph의 MCP / JSON-RPC는 yyjson 때문에 그렇지 않다." NeoGraph에 구체적으로, MCP-over-gRPC는 ROI가 훨씬 적다 (코덱 이점이 이미 yyjson에 의해 상쇄됨). gRPC는 다언어 / 원격 경계 + 큰-페이로드 용도로만 — 확인.

### NexaGraph 두 번째 수확 — 이력 압축 + GrpcRemoteTool (2026-05-16)

전체 NexaGraph 조사 후, 이미 이식된 `GrpcCheckpointStore` 외에 세 개의 추가 *범용, 중복 아님, 아직 NeoGraph에 없음* 항목이 추가로 이식됨. (RAG 앱 특화 stdio / HTTP MCP in `proto/rag_mcp_server/backend`는 NeoGraph가 이미 가지고 있거나 앱 특화이므로 제외. `DOCS/graph-engine-design.md`는 사실상 NeoGraph의 설계 선조이므로 "이식" 대상이 아님.)

1. **`neograph::history` (새 핵심 유틸리티, 추가적)** — NexaGraph의 CAF `compress_history` 액터에서 액터 껍질을 벗기고 핵심만 이식:
   - `compact_history(messages, Provider&, model, max_tokens=12000, recent_keep=6) -> awaitable<CompactedHistory>` — 토큰 추정이 예산을 초과하면, (system 1 + 최근 N) 사이 구간을 단일 LLM 호출로 요약하여 system-summary 메시지로 교체. `co_await provider.invoke()` (사용 중단된 `complete()` 사용 안 함, 비동기 lib 의존성 0 — 핵심 내부가 이미 코루틴 사용).
   - `sanitize_tool_calls(messages&)` — NeoGraph가 **완전히 결여했던** 방어: 잘림으로 인해 깨진 OpenAI 도구 쌍(응답 없는 assistant `tool_call` / 호출 없는 tool message)의 2-패스 제거, 멱등. `compact_history`가 출력에 내부적으로 적용 → 압축 결과가 절대 400을 만들지 않음.
   - `estimate_tokens` — 보수적인 ~3 chars/tok 추정 (혼합 KO / EN).
   - 예제 56 `history_compaction` (오프라인 MockProvider, 키 불필요) — sanitize 3→1, compact 29 msgs/975 tok → 6 msgs/208 tok, 원본-미변경 검증 PASS. `src/core/history.cpp`가 모든 구성에 대해 `neograph_core`로 빌드 — 496/497 ctest PASS (1 실패 = 기존 `pybind_smoke` openinference 모듈 누락, 무관).

2. **`neograph::grpc::GrpcRemoteTool`** — 예제 55는 gRPC를 통해 도구를 *내보내는* 쪽 (`run_tool_server`), 이것은 그 거울 — 원격 `ToolService.CallTool`을 일반 `neograph::Tool`로 *가져오는* 쪽. NexaGraph의 `GrpcTool` 어댑터 이식. pimpl (공개 헤더는 grpc++-free, `GrpcCheckpointStore`와 같은 자세). 단순 proto에 list-tools RPC가 없으므로 정의는 생성자를 통해 주입. 서버 오류 → `runtime_error`로 재던짐 (도구 오류, 전송 오류가 아님 — 로컬 Tool과 같은 계약). 예제 57 `grpc_remote_tool` — 서버 스레드 + `Tool&` 다형적 호출 + 오류 경로 PASS. **gRPC의 ROI #1 (다언어 원격 타입 있는 RPC)의 소비자 측 구체화** — 에이전트 관점에서 프로세스 경계 도구는 호출 지점에서 로컬 도구와 구별 불가.

### 남은 것 (여전히 열림)

  - CI에 `grpc-build` 작업 추가 (apt deps + ON 빌드 + `example_grpc_client` / `server` 스모크 — 깨끗한 ubuntu 러너에서는 위의 WSL 함정이 적용되지 않음).
  - `RunGraphStream`의 `ServerWriter::Write`가 스트리밍-노드 콜백 안에서 호출됨 — 현재 단일 슈퍼스텝 루프 스레드를 가정. 다중 작업자 팬아웃 그래프에서 콜백이 작업자 스레드에서 호출되면 `ServerWriter` 동기화가 필요 (gRPC `ServerWriter`는 스레드 안전하지 않음). 현재 예제는 단일 노드이므로 노출되지 않음.
  - TLS / 인증: `run_server`의 안전하지 않은 기본값 대신 사용자 배선 문서화.