<!-- neograph-i18n: source=docs/concepts.md locale=ko source_sha256=0f718bca31f68497ef00b56cb3dd01cd534f53f3dfd2a42741524fae51a18a36 -->
# NeoGraph 핵심 개념 — 해설 가이드

**Languages:** [English](concepts.md) | [한국어](concepts.ko.md) | [日本語](concepts.ja.md) | [简体中文](concepts.zh-CN.md)

예제 전에 이 문서를 읽는다. 그래프를 만드는 순서에 따라 채널, 노드, 엣지, fan-out, 라우팅, 체크포인트, 스트리밍을 설명한다.

LangGraph를 사용했다면 리듀서가 있는 채널, `Send`, `Command`, 체크포인트가 익숙할 것이다. NeoGraph의 [Core와 ProgramRuntime](../README.md#core-and-programruntime)은 역할이 다르며 이 안내는 Core 그래프 실행부터 시작한다. Python provider 호출에는 제거된 completion 클래스 대신 typed [binding 계약](python-binding.md)을 사용한다.

---

## 목차

(섹션 8.5는 v0.6.0에서 추가됨 — `Tracing — OpenTelemetry + Phoenix / Langfuse`. 번호가 매겨진 제목은 외부 문서 링크를 안정적으로 유지하기 위해 1-9로 유지됩니다; 8.5는 Streaming과 Common pitfalls 사이에 위치합니다.)


1. [큰 그림](#1-the-big-picture)
2. [채널 및 리듀서](#2-channels--reducers)
3. [노드](#3-nodes)
4. [엣지 및 조건부 라우팅](#4-edges--conditional-routing)
5. [Send — 동적 fan-out](#5-send--dynamic-fan-out)
6. [Command — 라우팅 재정의 + 상태 패치](#6-command--routing-override--state-patch)
7. [체크포인트, 인터럽트, HITL](#7-checkpoints-interrupts-hitl)
8. [스트리밍 이벤트](#8-streaming-events)
9. [일반적인 함정](#9-common-pitfalls)

---

<a id="1-the-big-picture"></a>
## 1. 큰 그림

NeoGraph **그래프**는 네 가지로 구성됩니다:

| 구성 요소 | 설명 | 정의 기준 |
|---|---|---|
| **채널** | 공유 상태의 명명된 슬롯. 각각은 새 쓰기가 기존 값과 결합되는 방식을 정의하는 리듀서를 가짐. | `definition["channels"]` |
| **노드** | 상태를 읽고 쓰기를 내보내는 함수 (선택적으로 `Send` / `Command` 포함). | `definition["nodes"]` |
| **엣지** | 정적 다음 노드 포인터. | `definition["edges"]` |
| **조건부 엣지** | 프레디킷 기반 라우팅 — 상태를 기반으로 여러 다음 노드 중 하나를 선택. | `definition["conditional_edges"]` |

실행은 **super-step 루프**입니다:

```
1. ready_set = nodes routed from __start__
2. while ready_set is not empty:
   a. run the ready batch against its pre-update channel state
   b. buffer returned writes, then fold them through channel reducers
   c. execute emitted Sends after ordinary writes; fold their results
   d. combine routing signals and evaluate updated state → new ready_set
```

일반 ready batch는 해당 batch의 update 이전 채널 상태를 읽는다. 형제 노드가 실행 중 다른 형제의 반환 쓰기를 읽을 수는 없다. 엔진은 쓰기를 buffer에 모아 batch 이후 리듀서로 결합하고 갱신된 상태로 라우팅을 평가한다. 이는 그래프 스케줄링이며 모델 내부 계산을 동기화하지 않는다.

예를 들어 `counter`가 0이고 두 ready 노드가 각각 `counter + 1`을 반환하면 둘 다 0을 읽는다. overwrite 리듀서 결과는 2가 아닌 1이다. 사용자 sum 리듀서에 증가량 1을 각각 쓰면 2로 결합할 수 있다. 다중 분기 `Send`는 각 payload를 적용한 격리 상태 복사본을 쓰고 단일 `Send`는 공유 상태에 payload를 적용한다. 리듀서 순서만으로 모델 응답이나 외부 효과가 재현되지는 않는다.

---

<a id="2-channels--reducers"></a>
## 2. 채널 및 리듀서

모든 상태 조각은 명명된 채널에 존재합니다. 채널은 노드와 수퍼-스텝에 걸쳐 지속됩니다; 노드는 채널에 쓰는 방식으로 통신합니다.

### 채널 정의

```python
"channels": {
    "messages":  {"reducer": "append"},     # conversation history
    "counter":   {"reducer": "overwrite"},  # latest value wins
    "summary":   {"reducer": "overwrite"},
}
```

### 내장 리듀서

| Reducer | 새 쓰기 의미론 | 일반적인 사용 사례 |
|---|---|---|
| `"overwrite"` | 새 값이 이전 값을 대체합니다. 병렬 쓰기에서는 마지막 쓰기가 승리합니다. | 단일 값 스크래치(현재 노드, 현재 질문, 라우팅 힌트). |
| `"append"` | 새 목록(반드시 목록이어야 함!)은 기존 목록에 연결됩니다. 순서: 이전 스텝의 값이 먼저, 이 스텝의 쓰기는 노드 실행 순서대로 추가됩니다. | 대화 메시지, 검색 결과, fan-out 수집. |

> 두 리듀서 모두 엔진 시작 시 `ReducerRegistry::ReducerRegistry()`에 등록됩니다 ([`src/core/graph_loader.cpp`](../src/core/graph_loader.cpp)). 사용자 정의 리듀서는 C++의 `ReducerRegistry::register_reducer(name, fn)` 또는 Python(v0.1.9부터)을 통해 등록합니다:
>
> ```python
> ng.ReducerRegistry.register_reducer("sum",
>     lambda current, incoming: (current or 0) + incoming)
> ```
>
> Python 호출 가능 객체는 GIL 하에서 실행됩니다. 동시 Send fan-out은 Python 사용자 정의 노드와 동일한 방식으로 이에 대해 직렬화됩니다. 이름을 다시 등록하면 이전 리듀서를 대체합니다.

### 채널 lifecycle과 checkpoint 계약

리듀서는 쓰기를 결합한다. 배열 retention은 별도 정책이며 `unbounded`(기본), `latest`, 양수 `retention_limit`을 가진 `bounded`를 선택한다. Retention은 `ChannelWrite.Mode.Overwrite`를 포함한 매 쓰기 뒤 배열을 자른다. `latest`는 다음 쓰기까지 마지막 요소를 유지한다. Persistence는 독립적으로 `checkpoint`(기본, materialized 값과 version) 또는 `ephemeral`(둘 다 영속 checkpoint에서 생략)을 선택한다. Bounded retention은 저장 크기뿐 아니라 관측 가능한 기록을 바꾼다.

엔진은 노드의 반환 쓰기 순서, static batch의 scheduler-ready 순서, 다중 `Send`의 호출 순서로 결합하며 완료 순서는 사용하지 않는다. Pending write는 같은 task slot으로 재생한다. Overwrite는 순서에 따른 last-writer-wins이며 append는 요소 순서를 유지한다. 사용자 리듀서는 replay에서 순수하고 안정적이어야 한다. Regrouping에 결과가 같아야 하면 결합법칙, 순서 독립성이 필요하면 교환법칙이 필요하다. 명시적 overwrite는 리듀서를 건너뛴 다음 retention을 적용한다. 이 규칙은 모델 응답이나 외부 효과의 재현성을 보장하지 않는다.

Ephemeral 값은 superstep 사이에도 살아 있으며 매 step 초기화하지 않는다. Checkpoint는 선언된 ephemeral 이름과 쓰기 여부만 기록하고 값은 기록하지 않는다. Resume, `resume_if_exists`, exact-ID resume, state update는 이미 쓴 ephemeral 상태, 과거 checkpoint의 누락 guard, 변경된 ephemeral 채널 집합을 거부한다. 최초 ephemeral 쓰기 전 checkpoint는 문서화된 순서로 pending write를 재생하며 resume할 수 있다. `update_state`는 ephemeral 쓰기를 거부한다. 다중 `Send`의 in-process worker는 격리 복사본에 live ephemeral 값을 상속한다. 정확성에 필요한 상태는 checkpoint에 남기거나 새 run에서 영속 입력으로 재구성한다.

`GraphState::restore`는 ephemeral 채널이 있는 그래프를 거부한다. 일치하는 guard와 `restore_checkpoint`를 쓰거나 모든 live 값과 version을 포함한 같은 process snapshot에는 `restore_runtime`을 쓴다. Guard는 checkpoint metadata를 사용하며 channel blob layout이나 store schema를 바꾸지 않는다. Ephemeral 채널이 없는 그래프의 과거 full-value checkpoint는 계속 동작한다. Guard 없는 binary로 downgrade하기 전에 ephemeral thread와 fork를 drain하거나 영속 입력에서 재시작한다. 과거 reader는 추가 guard를 강제할 수 없다.

Checkpoint 채널은 full materialized snapshot을 쓴다. Memory, SQLite, PostgreSQL은 바뀌지 않은 `(thread, channel, version)` 값을 중복 제거하지만 append 기록은 쓰기마다 version이 바뀌므로 snapshot도 커진다. Pending write는 미완료 superstep의 성공 task를 기록하며 일반 channel delta가 아니다. Per-step reset 정책은 제공하지 않는다. 안전한 설계에는 쓰기·라우팅 이후 reset과 interrupt, replay, Send 동작의 정의가 필요하며 ephemeral persistence와 혼동해서는 안 된다.

Delta-backed checkpoint는 설계이며 채널 설정이 아니다. 이 형식은 full snapshot부터 순서 있는 `{channel, version, write mode, value}` delta를 최대 *K*개(선택적 byte 기준) 재생할 것이다. Overwrite, retention, version, reducer identity를 유지하고 pending write 삭제 전 snapshot/delta와 checkpoint pointer를 원자 publish해야 한다. 누락 link, version gap, 미상 리듀서, replay 실패는 거부해야 한다. 채택에는 새 schema version과 측정된 이점이 필요하다. 과거 snapshot을 허구 delta 없이 base로 옮기고 되돌릴 수 있는 rollout에는 old-reader full snapshot을 유지한다. Delta-only record가 있으면 원래 reducer registry로 materialize하지 않는 한 downgrade를 거부한다. 현재 store는 full-snapshot 방식이다.

Baseline은 `bench_checkpoint_store --threads 1 --iters 1 --history-steps 256 --payload 512 --backends memory,sqlite`로 측정한다. 격리된 local DB에만 `postgres`, `--pg-url`을 더한다. 행은 logical serialized byte, save/load p50/p95, reconstruction depth를 보고하고 legacy 행은 blob count를 보고한다. Allocation request에는 `heaptrack bench_checkpoint_store --threads 1 --iters 1 --history-steps 256 --payload 512 --backends memory`를 사용한다. Native JSON/SQL allocator는 C++ `operator new`로 모두 측정되지 않는다. 같은 payload, history, backend로 측정값을 비교한다. Logical byte와 durable physical byte는 다르다. SQLite는 종료 시 지우는 고유 임시 DB를 쓰며 `--sqlite-path`는 새 파일을 남기고 기존 경로를 거부한다.

기록된 Linux x86-64 Debug baseline은 thread 1개, history step 256개, 512-byte 메시지, iteration 1회였다. 과거 실측이며 성능 목표가 아니다:

| Backend | Logical checkpoint bytes | Save p50/p95 (µs) | Load p50/p95 (µs) | Replay depth |
| --- | ---: | ---: | ---: | ---: |
| Memory | 17,814,952 | 54 / 138 | 141 / 382 | 1 |
| SQLite | 17,814,952 | 289 / 1,589 | 176 / 474 | 1 |

삭제 전 별도 반복에서 SQLite DB/WAL은 14,811,136 / 4,210,672 byte(합계 19,021,808)였다. 구성·JSON parse를 포함해 process 전체 `malloc`, `calloc`, 0이 아닌 `realloc` request를 세는 Linux `LD_PRELOAD` shim은 `--history-steps 0` 대비 Memory 추가 88,277 request / 605,289,027 requested byte, SQLite 114,295 / 867,319,964를 측정했다. 이는 누적 request이지 live memory나 store 전용 allocation이 아니다. Aligned/internal allocation은 측정하지 않았다. Shim은 의존성이 아니며 결론 전에 지원 profiler와 여러 warm run을 사용한다.

### 채널에 쓰기

노드는 `ChannelWrite` 목록을 반환합니다:

```python
return [
    ng.ChannelWrite("messages", [{"role": "assistant", "content": "Hi!"}]),
    ng.ChannelWrite("counter",  (state.get("counter") or 0) + 1),
]
```

값의 형태는 리듀서와 일치해야 합니다:
- `"append"` → 목록이어야 합니다 (연결됩니다).
- `"overwrite"` → JSON 직렬화 가능한 모든 값.

### 노드에서 상태 읽기

```python
def run(self, input):
    msgs    = input.state.get("messages") or []  # list of message dicts
    counter = input.state.get("counter") or 0
    ...
```

`state.get(channel)`는 채널의 현재 값을 반환하거나, 채널이 존재하지만 아직 기록되지 않은 경우 `None`를 반환합니다. 채팅 메시지에 대한 타입 기반 접근의 경우, `state.get_messages()`는 `list[ChatMessage]`를 반환합니다 (`messages` 채널에서 파싱됨) — `llm_call`에서 내부적으로 사용됩니다.

### 버전

각 채널은 단조 증가하는 `version` 번호를 전달합니다. 엔진은 이를 체크포인트 diffing 및 `state.channel_version(name)` 검사 API에 사용합니다. 일반적으로 직접 읽지 않습니다.

---

<a id="3-nodes"></a>
## 3. 노드

노드 유형을 등록하는 세 가지 방법, 제어 수준이 증가하는 순서:

### 3.1 내장 노드

| `type` (JSON에서) | 기능 | 구성 |
|---|---|---|
| `llm_call` | 소유 typed 요청을 한 번 준비하고 gate/reserve/receipt 후 같은 핸들을 dispatch하여 모든 순서 메시지와 전체 결과를 보존한다. | 읽기 `provider`, `model`, `instructions`, `tools` 에서 `NodeContext`. |
| `tool_dispatch` | 최신 어시스턴트 메시지의 `tool_calls`를 확인하고, 각각을 `Tool::execute`를 통해 실행한 후 `{role: "tool", tool_call_id, content}` 결과를 추가합니다. | `tools`를 `NodeContext`에서 읽습니다. |
| `intent_classifier` | LLM은 사용자 의도를 N개의 레이블 중 하나로 분류하고 선택된 레이블을 `__route__`에 기록합니다. `route_channel` 조건부와 함께 사용하세요. | `extra_config: {labels, prompt_template}` |
| `subgraph` | 다른 그래프를 단일 노드로 내장합니다. 내부 상태는 구성된 키 재매핑을 통해 매핑됩니다. | `extra_config: {graph_def, input_keys, output_keys}` |

### 3.2 `@ng.node` 데코레이터(Python 전용)

쓰기 전용 노드를 정의하는 가장 짧은 방법:

```python
@ng.node("greet")
def greet_node(state):
    name = state.get("name") or "world"
    return [ng.ChannelWrite("messages",
        [{"role": "assistant", "content": f"Hello, {name}!"}])]
```

데코레이트된 함수는 `list[ChannelWrite]`(또는 `None`, `[]`로 처리됨)를 반환해야 합니다. `Send` 또는 `Command`를 내보낼 수 없습니다 — 그러한 경우 `GraphNode`를 서브클래싱하세요.

### 3.3 전체 `GraphNode` 서브클래스

전체 제어를 위해 `run(input)`를 오버라이드하세요. 이 메서드는 v0.4.0에서 도입되었으며 v0.9.0부터 유일한 커스텀 노드 진입점입니다 — 하나의 메서드, 하나의 시그니처:

```python
class Researcher(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        # input.state    — read channels via input.state.get(...)
        # input.ctx      — RunContext (cancel_token, thread_id, step, ...)
        # input.stream_cb — non-None when running in streaming mode
        topic = input.state.get("topic")
        result = await_llm(topic, cancel_token=input.ctx.cancel_token)
        return ng.NodeResult(
            writes=[ng.ChannelWrite("findings", [result])],
            command=ng.Command(goto_node="evaluator"),  # optional
            sends=[],                                    # optional
        )
```

Python은 `input.ctx`에 `cancel_token`, `usage`, `thread_id`, `step`, `stream_mode`, `store`, `resume_value`, `trace_id`, `run_id`, `model_token_budget`, typed provider 증거를 노출한다. Deadline은 `has_deadline`, `deadline_remaining_ms`로 확인하며 원시 C++ steady-clock 값은 불투명하다. C++ 호출자는 `RunMetadata`로 deadline과 trace metadata를 제공하고 중첩 subgraph로 전달한다.

`list[ChannelWrite]`만 반환할 수도 있습니다. `Send`나 `Command`가 필요하지 않을 때 말이죠 — 바인딩이 이를 `NodeResult`로 자동 승격합니다.

> **v0.3.x에서 마이그레이션:** 제거된 pre-v0.4 다중 진입점 노드 API에는 하나의 대체가 있습니다: `run(input)`를 오버라이드하세요. `input.state`에서 상태를 읽고, non-None일 때 `input.stream_cb`를 통해 토큰을 내보내며, `input.ctx.cancel_token`에서 취소 토큰을 읽으세요.

로더가 인스턴스화할 수 있도록 유형을 등록하십시오:

```python
ng.NodeFactory.register_type(
    "researcher",
    lambda name, config, ctx: Researcher(name),
)
```

팩토리는 `(name, per-node config, NodeContext)`를 확인하므로 동일한 클래스가 서로 다른 구성으로 여러 이름 아래에서 인스턴스화될 수 있습니다.

### 3.4 도구(별도 개념, `tool_dispatch`에서 사용)

`Tool`는 노드가 아닙니다 — `tool_dispatch`가 호출하는 것입니다. `ng.Tool`를 서브클래싱하고, 세 개의 메서드를 오버라이드하며, 인스턴스를 `NodeContext(tools=[…])`에 전달하세요:

```python
class CalcTool(ng.Tool):
    def get_name(self):       return "calc"
    def get_definition(self): return ng.ChatTool("calc", "Double x", {"type": "object", "properties": {"x": {"type": "number"}}, "required": ["x"]})
    def execute(self, args):  return str(args["x"] * 2)
```

엔진은 컴파일 시점에 도구 목록의 소유권을 가져갑니다 — 이후에 로컬 참조는 해제될 수 있습니다.

---

<a id="4-edges--conditional-routing"></a>
## 4. 엣지 & 조건부 라우팅

### 정적 엣지

```python
"edges": [
    {"from": ng.START_NODE, "to": "llm"},
    {"from": "dispatch",    "to": "llm"},
    {"from": "summarizer",  "to": ng.END_NODE},
]
```

동일한 소스 노드에서 여러 모서리는 fan-out됩니다 (모든 후속자는 다음 슈퍼 단계의 준비 집합에 들어갑니다). 한 슈퍼 단계에서 동일한 대상으로 가는 두 모서리는 하나의 대상 실행으로 중복 제거됩니다.

### 조건부 엣지

조건부 엣지는 **명명된 조건**을 실행하고 `routes` 맵에서 다음 노드를 선택합니다:

```python
"conditional_edges": [
    {
        "from": "llm",
        "condition": "has_tool_calls",
        "routes": {"true": "dispatch", "false": ng.END_NODE},
    }
]
```

조건 이름은 엔진에 등록된 `ConditionFn`로 해석됩니다. 두 가지가 기본 제공됩니다:

| Condition | 반환값 | 사용 시점 |
|---|---|---|
| `has_tool_calls` | `"true"` (최신 어시스턴트 메시지에 비어 있지 않은 `tool_calls`가 있는 경우), 그 외에는 `"false"`. | ReAct 루프 — LLM이 도구 호출을 중단할 때까지 도구 디스패치를 계속 유지합니다. |
| `route_channel` | `__route__` 채널에 있는 문자열; `"default"`로 폴백됩니다. | 명시적 의도 라우팅을 위해 `intent_classifier`와 함께 사용하세요. |

사용자 정의 조건은 C++에서 `ConditionRegistry::register_condition(name, fn)`를 통해, 또는 Python에서(v0.1.9부터) 등록합니다:

```python
def is_long(state):
    msgs = state.get("messages") or []
    return "long" if len(msgs) > 10 else "short"

ng.ConditionRegistry.register_condition("is_long", is_long)
```

호출 가능 객체는 라이브 `GraphState`를 수신하므로(`state.get(channel)` 및 `state.get_messages()`가 작동함) 조건부 엣지의 `routes` 키 중 하나와 일치하는 문자열을 반환해야 합니다.

### 두 가지 동등한 형식 — 둘 다 v0.1.8부터 작동합니다

조건부 엣지는 `edges` 배열 내부(`condition` 필드 포함) **또는** 별도의 `conditional_edges` 블록에 있을 수 있습니다. 두 형식 모두 허용됩니다. 더 명확한 쪽을 선택하세요:

```python
# Form A — top-level (LangGraph parity, recommended for Python)
"edges":             [{"from": "__start__", "to": "llm"}, ...],
"conditional_edges": [{"from": "llm", "condition": "...", "routes": {...}}]

# Form B — inline (used by every C++ example)
"edges": [
    {"from": "__start__", "to": "llm"},
    {"from": "llm", "condition": "...", "routes": {...}},
]
```

> **이력:** 형식 A는 v0.1.8 이전에 그래프 컴파일러에 의해 조용히 삭제되었습니다 — README와 모든 Python 예제가 이를 사용했기 때문에 ReAct 루프가 단일 LLM 호출로 퇴화되었습니다. 커밋 `e23a523`에서 수정되었습니다. wheel ≤ 0.1.7에서 이 문제가 보이면 업그레이드하세요.

---

<a id="5-send--dynamic-fan-out"></a>
## 5. 전송 — 동적 fan-out

`Send`는 주제마다 researcher 하나처럼 실행 중 target 호출 수를 정하게 한다. 엔진은 일반 ready batch가 반환하고 쓰기를 적용한 뒤 같은 번호의 superstep 안에서 생성된 Send를 실행한다.

```python
class Planner(ng.GraphNode):
    def run(self, input):
        topics = decide_topics(input.state)            # e.g. 5 strings
        return ng.NodeResult(
            writes=[],
            sends=[ng.Send("researcher", {"topic": t}) for t in topics],
        )
```

### 정신적 모델

엔진은 `Send`마다 compiled target을 호출하며 새 node 객체를 보장하지 않는다. 동시 호출에서 target의 member state를 안전하게 다뤄야 한다. Payload는 target의 채널 읽기 전에 적용한다. 단일 Send는 공유 상태, 다중 Send는 ready batch 이후 상태의 격리 복사본을 사용하며 모든 분기 완료 뒤 반환 쓰기를 호출 순서로 결합한다. 다음 ready batch 라우팅은 일반 node와 Send target의 신호를 함께 사용한다.

### 일반적인 형태: fan-out 5, 요약자 summarizer fan-in

```
planner ─┬─ Send("researcher", {topic: "A"})  ─┐
         ├─ Send("researcher", {topic: "B"})  ─┤
         ├─ Send("researcher", {topic: "C"})  ─┼─→ summarizer
         ├─ Send("researcher", {topic: "D"})  ─┤
         └─ Send("researcher", {topic: "E"})  ─┘
```

`researcher`의 나가는 엣지는 `{"from": "researcher", "to": "summarizer"}` 하나뿐입니다 — 정적 엣지와 동일한 중복 제거 규칙이 적용되므로 서머라이저는 한 번만 실행됩니다.

### Worker 수 튜닝

`build()` 기본값은 `EngineConfig::worker_count == 1`이며 engine-owned thread pool 없이 호출자의 coroutine executor로 분기를 dispatch한다. Coroutine I/O는 겹칠 수 있지만 단일 thread executor의 CPU-bound 작업은 직렬화될 수 있다. Multi-thread caller executor나 동시 run에서는 node member state를 안전하게 다뤄야 한다.

실제 병렬 처리를 위해, 풀(pool)을 명시적으로 선택하십시오. fan-out 폭에 맞게 정확히 N을 선택하거나, `set_worker_count_auto()`을(를) `hardware_concurrency()`에 사용하십시오 (기본값 4로 대체됨):

```python
engine.set_worker_count(5)           # match a 5-way Send
# or
engine.set_worker_count_auto()       # hardware_concurrency()
```

멀티-Send(또는 멀티 나가는 엣지) fan-out이 선택된 풀 없이 실행되면, NeoGraph는 일회성 stderr 경고를 출력하여 조용한 직렬 실행이 눈에 띄지 않게 지나가지 않도록 합니다. 의도적으로 직렬 fan-out을 구동하는 경우(예: worker=1 빠른 경로의 벤치마크) `NEOGRAPH_SUPPRESS_FANOUT_WARNING=1`로 억제하세요.

---

<a id="6-command--routing-override--state-patch"></a>
## 6. 명령 — 라우팅 재정의 + 상태 패치

`Command`는 노드가 다음 위치를 결정하고 동일한 반환 값에서 상태를 변경할 수 있게 합니다. 이는 일반 나가는 엣지를 우회합니다.

```python
class Evaluator(ng.GraphNode):
    def run(self, input):
        if score(input.state) >= 0.8:
            return ng.NodeResult(
                writes=[],
                command=ng.Command(
                    goto_node="summarizer",
                    updates=[ng.ChannelWrite("verdict", "accepted")],
                ),
            )
        else:
            return ng.NodeResult(
                writes=[],
                command=ng.Command(
                    goto_node="planner",                  # loop back
                    updates=[ng.ChannelWrite("retries",  (input.state.get("retries") or 0) + 1)],
                ),
            )
```

### when Command works vs auxiliary edge condition.

- **조건부 엣지(conditional edge)**: 라우팅은 노드 로직이 필요 없는 상태 조건자(state predicate)에 의존합니다. 더 깔끔하고 선언적입니다.
- **Command**: 라우팅은 노드 내부에 작성하는 것이 가장 자연스러운 로직(다중 기준 점수 산정, 콘텐츠 검사, 재시도 결정)에 의존합니다. 또한 상태를 원자적으로 업데이트하면서 다음 노드를 선택하는 유일한 방법이기도 합니다.

### fan-in 상황에서의 마지막 쓰기 승리(Last-writer-wins)

여러 형제가 비어 있지 않은 `Command.goto_node`를 반환하면 전달된 라우팅 순서의 마지막 command가 일반 엣지와 barrier를 재정의한다. Static batch는 ready 순서, 다중 `Send`는 완료 순서가 아닌 호출 순서를 제공한다. 반환된 command update는 모두 쓰기 pipeline으로 결합한다. 상충하는 command로 workflow가 달라진다면 라우팅 결정 노드 하나를 두는 편이 낫다.

---

<a id="7-checkpoints-interrupts-hitl"></a>
## 7. 체크포인트, 인터럽트, HITL

### 체크포인트 저장소 설정

```python
engine.set_checkpoint_store(ng.InMemoryCheckpointStore())
# or: engine.set_checkpoint_store(ng.PostgresCheckpointStore(...))   # if built with PG
```

저장소가 연결되면, 모든 슈퍼스텝은 `(thread_id, checkpoint_id)`를 키로 하는 체크포인트를 저장소에 기록합니다. `RunResult.checkpoint_id` 필드가 최신 것입니다.

### 정적 인터럽트 지점

```python
"interrupt_before": ["payment"],   # pause before this node runs
"interrupt_after":  ["llm"],       # pause after, before routing
```

엔진은 `RunResult`와 함께 `interrupted=True` 및 `interrupt_node`가 설정된 상태로 반환합니다. 재개하려면:

```python
result = await engine.resume_async(thread_id="t1",
                                   checkpoint_id=result.checkpoint_id,
                                   new_input={...})  # optional
```

### `NodeInterrupt`를 통한 동적 인터럽트

노드 본문 내부에서 던지기(Python: `raise ng.NodeInterrupt(reason)`, C++: `throw NodeInterrupt(...)`). 엔진이 포착하고, 상태를 유지하며, 던지는 노드에서 중단된 `RunResult`를 반환합니다 — 동일한 재개 API입니다.

중간 노드 출력에 따라 일시 중지 여부를 결정해야 할 때 유용합니다(예: "LLM이 인간에게 보여줄 만한 것을 생성했는가?").

### 시간 여행

`engine.fork(source_thread_id, new_thread_id, checkpoint_id="")`는 체크포인트를 호출자가 지정한 대상 스레드에 복사하고 새 체크포인트 ID를 반환한다. 체크포인트 ID를 생략하면 원본의 최신 체크포인트를 선택한다. 복사본은 대기 중인 continuation을 유지하며 상태를 편집해도 새 작업이 예약되지는 않는다.

`next_nodes == ["__end__"]`인 완료된 continuation을 resume하면 노드를 실행하지 않고 저장된 결과를 복원한다. 편집한 상태로 중단된 작업을 계속하려면 `get_state_history()`에서 대기 노드가 남은 정확한 이전 체크포인트 ID를 선택해 fork한 뒤 복사본을 편집하고 resume한다. 과거의 빈 `next_nodes` 벡터는 다르다. 정확한 ID 없이 최신 상태를 resume하면 새 실행을 시작하는 기존 동작을 유지하지만 exact-ID resume은 해당 스냅샷에 고정된다.

[Example 08](../examples/08_state_management.cpp)은 새 turn 흐름을 유지한다. 완료된 체크포인트를 fork하고 사용자 메시지를 편집한 뒤 `resume_if_exists=true`로 `run()`을 호출한다. 그 새 실행이 중단된 경우에만 resume한다. 일시 중지된 fork를 resume하는 예제는 아니다.

`ChatMessage` / `ChatTool`과 JSON은 portable projection이지 native 권한이 아니다. Portable 포맷은 [`provider-message-v2`](../schemas/provider-message-v2.schema.json), [`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json)를 유지한다. 실제 C++ checkpoint sidecar는 메모리에서 native seal을 보존한다. 영속 native 기록에는 host-owned `sp::NativeArchive`가 필요하다. closed v3 / `spna3`는 독립 키를 쓰는 인증된 owner-private custody이며 archive v2는 업그레이드하거나 해석하지 않고 거부한다. 인증은 모든 semantic descriptor 선택(origin/path/header, policy, 요청 field mapping, usage path, stop mapping), owner와 정확한 custody binding을 결합한다. 암호화나 vendor-issuer 인증은 아니다. archive 본문·키·native blob·raw wire 관측을 공개하지 않는다. Archive는 증거 저장소이지 돈의 grant나 spending lease가 아니다. Program/external bank는 독립 journal 소유이며 snapshot 복사로 credit을 만들 수 없다.

Provider 기록은 서로 다른 모드를 가진다. 동일 경로의 native continuation은 실제 reasoning, signature와 순서 있는 tool group을 원래 binding 아래 보존한다. Gemini의 기본값은 `NativeOnly`다. 명시적 `PortableForeign`은 native seal, wire output, signature가 없는 호출자 작성 assistant text와 tool call을 받아들인다. 첫 외부 function call에만 Google의 문서화된 bypass marker를 붙이며 text-only turn에는 signature를 넣지 않는다. 이 projection은 native 권한을 부여하지 않고 실패한 native seal을 복구하거나 portable로 강등하지도 않는다. 임의의 여러 vendor 기록이 native로 이식 가능해지는 것은 아니다.

Responses `previous_response_id`는 provider가 보관하는 대화 상태를 선택하며 요청에는 새 입력만 넣는다. Client-tool 소유권에 local 증거가 필요할 때 `previous_response_history`가 실제 이전 소유권 증거를 제공하며 반복 입력으로 전송되지는 않는다. Cursor는 전체 native replay seal이나 archive 권한이 아니며 origin, route, model, configuration, 완료 상태 검사를 받는다.

현재 SDK interface revision과 shared-library generation은 4이며 소비자는 일치하는 header와 library로 다시 빌드해야 한다. Output generation cap은 native replay configuration과 별도로 각 호출에서 admission과 accounting을 거친다. 새 semantic call의 cap을 올려도 원래 bank, grant, deadline을 갱신하지 않는다. 명시적으로 문서화된 per-turn 선택을 제외하면 content, prefix, origin, route, policy, tools, reasoning controls의 binding은 유지된다. Portable JSON v2와 native archive v3 / `spna3`는 바뀌지 않으며 아래 과거 ABI3 측정은 interface4 결과가 아니다.

Python도 C++과 같은 소유 request/outcome 경계를 제공한다: `make_provider_request`, `Provider.prepare`, `dispatch`, `invoke`. Provider 기록에는 typed part를 가진 `ProviderMessage`를 사용하며 `ChatMessage`는 그래프 편의 projection으로 남는다. SDK 실패는 `ProviderOutcome.failure`로 읽고 host observer/settlement 예외는 `outcome`과 `cause`를 보존한다. 생성자와 GIL/콜백 동작은 [Python binding 안내](python-binding.md)를 참조한다.

`input_total`, `output_total`, `total` 같은 사용량 카운터는 `std::optional<sp::Count>`이며 존재하는 count는 `uint64_t value`와 `Evidence`를 가진다. `Usage`에는 stage, quality, conflict도 남는다. 누락은 미상이며 0을 만들어 넣지 않는다.

`UsageAccumulator::snapshot()`은 누적 보고를 반환한다. `total_tokens_wide()`는 청구 토큰과 미해결 예약의 합이며 보고 사용량으로 표시하면 안 된다. 정산에는 input/output count가 있는 final·consistent 보고가 필요하며 근거가 있는 가장 큰 total을 차감하고 초과 사용량도 clamp하지 않는다. 누적 보고 중 하나라도 counter가 없으면 합계도 미상이다. 예약, 로컬 차감, vendor 청구서는 서로 다른 기록이다.

**Standalone bank journal 수정 — 현재 계약 개정; 실제 runtime 증거는 아래.** Owner-approved protocol은 단조 trusted-store namespace obligation과 실제 불변 original owner/thread/graph scope, ceiling, deadline/clock identity, generation을 요구한다. 전체 checkpoint commitment·revision에 대한 정확한 durable head CAS만 host-owned opaque lease를 발급할 수 있다. 정확한 pending effect window를 provider I/O 전에 영속화해야 하며 실제 SDK outcome, charge, nullable report, hold, dedup identity로 정산해야 한다. Checkpoint와 next head는 같은 owned actor/revision 아래 원자적으로 publish해야 한다. Bank metadata 제거·checkpoint pruning·old authenticated snapshot replay·같은 ID overwrite·actor 상실은 credit을 주면 안 된다. 기존 65 hold에서 ceiling 130을 129로 낮추면 추가 65를 허용할 수 없다. 입증된 no-effect 실패는 unchanged head를 release해 authentic 130 복구가 가능해야 한다. Crash/unknown/lost-lease window는 refund/retry/fallback 없이 hold를 유지한다. Plain/pristine archive 설정은 money/native spending lease를 주지 않고 현재 `config.usage`는 기존 standalone obligation을 대체할 수 없다. Program/external-bank journal 소유는 유지된다. 이는 요구 계약이다. 실제 currency/custody 증거와 instrumentation 한계는 아래에 있으며 stable released API 보장은 아니다.

**현재 선언; 통합 runtime 증거는 아래:** `<neograph/graph/checkpoint.h>`는 `owner_scope`, logical `thread_id`, private backend `storage_thread_id`, `graph_identity`, `original_ceiling`, `original_deadline_ticks`, `deadline_clock_identity`를 가진 `ManagedBudgetLeaseScope`를 선언한다. `OwnedManagedBudgetLease`는 read-only `scope()`, `actor_id()`, 불변 `bank_generation()`, `revision()`, `head_checkpoint_id()`, `head_commitment()`를 노출하며 공개 authority-import constructor가 없다. `ManagedBudgetEffectReceipt`는 `active()`, `effect_id()`, `claim_amount()`, `request_digest()`를 노출하고 default receipt는 권한을 주지 않는다. `CheckpointStore`는 `acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`, `begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`, `settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`, `publish_managed_budget_checkpoint(lease, checkpoint)`, `release_managed_budget_lease(lease)`와 `_async` counterpart를 선언한다. Sync `CheckpointStoreCore`와 `AsyncCheckpointStore`는 각각 해당 variant를 제공한다. `managed_budget_checkpoint_commitment(checkpoint)`는 bank JSON뿐 아니라 전체 영속 checkpoint를 결합한다. 이 선언은 backend CAS·currency 안전성·installed ABI 호환성·실제 성공 runtime 경로를 입증하지 않는다.

**실제 InMemory shared-bank fork는 유지되었으며 실제 증명 완료.** 원래 실제 C++ fork는 ONE original financial journal과 trusted current branch head를 쓰며 grant를 복제하지 않는다. `publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)` 및 `_async`는 authentic current source/full commitment와 실제 same-bank native C++ pointer를 요구하고 durable standalone fork는 명시적 unsupported로 남는다. `OwnedManagedBudgetLease::scope()`와 original owner/thread/graph, ceiling, deadline/clock, generation은 불변이다. Read-only store-issued `execution_thread_id()` / `execution_storage_thread_id()`는 execution branch를 별도로 선택하며 `GraphState::budget_original_thread_id()`는 원래 financial bank를 가리킨다. 정확한 selected-branch head CAS와 global actor/revision은 canonical current counter, pending effect, burned identity에 대해 모든 branch를 직렬화한다. Original/fork branch는 보충 없이 계속 사용할 수 있다. Stale snapshot·checkpoint copy·imported JSON은 alias를 발급하거나 head를 되돌릴 수 없다. 원래 root30 → charge3 → original continuation6 → fork lower20 → continuation9 same-bank 증명은 변경하지 않은 test_graph_engine.cpp:810–913에서 PASSED했다. Saved original ceiling30은 effective fork ceiling20과 별개이며 widening31과 JSON-only restore는 거부해야 한다. Unbounded reported observation은 사실 data이지 finite grant가 아니다. 입증된 zero-effect lease만 unchanged head를 release할 수 있고 unknown/pending effect는 obligation을 유지한다.

**현재 release-error 계약; 실제 suite/probe는 아래.** `<neograph/graph/engine.h>`의 `graph::ManagedBudgetLeaseReleaseError`는 `ProviderOutcomeError`를 상속한다. `cause()`는 원래 execution exception을 보존하고 `release_error()`는 보조 durable lease-disposition 실패를 노출한다. `outcome()`은 실제 SDK 증거가 있으면 보존하고 SDK outcome이 없으면 null이다. Release 실패는 결과를 만들거나 재dispatch를 허용할 수 없다. Closed `_neograph_managed_budget_scope` metadata는 원래 logical scope/cap/deadline clock/generation을 설명하지만 backend CAS 권한이 아닌 data이다.

**Archive-owner/retention 계약; 실제 suite/probe는 아래.** Finite standalone root 또는 authenticated finite source만 실제 설정된 `sp::NativeArchive::owner_scope()`에서 생략된 original owner를 상속한다. Unbounded/plain owner metadata 의미는 바뀌지 않는다. 명시적으로 충돌하는 archive owner는 lease acquire 전에 거부한다. `CheckpointStore::retains_native_checkpoint() const noexcept`와 대응 Core/Async storage capability는 기본 false이며 실제 InMemory backend만 true로 override하고 wrapper는 실제 retention을 위임해야 한다. 이 read-only 설명은 정당한 unleased/plain/unbounded C++ native checkpoint custody를 허용하되 spending credit이나 native replay authority를 주지 않는다. Leased custody는 JSON flag나 추측한 store type 대신 실제 store-issued receipt를 사용한다.

**Native-custody pre-I/O gate; 실제 suite/probe는 아래.** Managed effect begin은 pending-effect/slot/held-window 변경 전에 실제 결합된 NativeArchive 또는 실제 local store-issued private C++ retention capability를 요구한다. Private capability는 JSON에서 import하거나 wire로 전달하지 않는다. C++ sidecar는 경계를 넘을 수 없으므로 remote backend가 InMemory여도 gRPC는 실제 client·server archive를 요구한다. Archive가 finite source owner를 제공하지 않으면 원래 anonymous owner scope는 빈 값으로 유지하고 실제 archive binding은 original scope와 일치해야 한다. Financial head/lease 증거만으로 native-custody readiness를 증명하지 않는다.

`ProgramFailure`는 live `provider_outcome`·`provider_cause`를 보존한다. Canonical factual SDK witness는 실제 archive custody를 owner/run/version/bundle/operation/attempt에 결합하며 Runtime은 복구 실패를 노출하기 전에 설정된 custody를 즉시 복원한다. 공개 data-only `ProgramResult::create()`는 미리 채운 witness로 우회할 수 없고 unresolved parsed seal은 실행 결과가 아니다. 프로세스 재시작 후 원래 exception pointer는 없으므로 `provider_cause == nullptr`이며 text에서 재생성하지 않는다. 영속화할 수 없는 실패는 serialize/publish/replay할 수 없다.

`RecordedBindingSet`는 source-bound move-only data이지 caller가 제공하는 dispatcher가 아니다. 신뢰된 Catalog `recorded_capability_binder`는 실제 영속 source event를 독립적으로 읽어 captured-only capability를 materialize한다. `ProgramRuntime::replay_recorded()`는 원래 selected-source permission을 검사한 뒤 실제 남은 bank를 durable CAS로 이전한다. inherited spend는 새 model grant가 아니다. 구 `start_recorded` 갱신 API는 제거되었다. InMemory/File/SQLite/PostgreSQL Program store는 실행 내내 정확하고 불변인 owned lease를 보존하며 expiry로 갱신하지 않는다. Controlled JavaScript도 underlying capability manifest를 검사하고 정확한 completed command 결과를 소비하며 external effect를 재dispatch하지 않는다.

**Recorded-control causal fix는 full suite에서 실제 증명 완료.** Captured command replay는 실행 전에 새 CPU wall-time/Core work만 durable reserve하고 측정 work와 새 Core checkpoint를 result CAS로 publish한다. 새 model·money·Program-operation allowance를 소비하지 않고 captured external effect를 재dispatch하지 않는다. 미정산 reservation은 debit을 유지한다. Reservation은 첫 새 Core checkpoint를 거부했던 일반 Running→Running transition 대신 인증된 settlement transition을 선택한다. Await channel receive·timer wait/cancel·handoff wait 시작/release는 소유 executor/strand에서 직렬화한다. 기존 Recorded CPU/Memory await/handoff scenario는 full suite에서 pass했다. Remote TSan coverage 한계는 아래에 명시한다.

아래 관측은 이 문서 정리 이전에 기록되었다. 과거 증거이며 새 테스트 실행이나 모든 platform·transport·security 속성의 보장이 아니다.

**유료 관측 완료; 보편적 qualification은 아님.** 원래 `SPQUAL1` base630/1000000 microUSD는 불변이다. 같은 원래 ledger의 ONE hash-chained `A`가 승인 extension480/3000000을 받아 aggregate1110/4000000이 된다. Calls/spent/hold/settlement는 누적이며 새 grant ID/header/reset은 없다. 정확한 declaration byte/file identity와 original authorization/baseline/catalog/activation/ledger-prefix hash/totals는 고정되고 삭제·교체·변경은 fail closed한다. 최종 canonical ledger는 calls1110/spent437958/held1287828 microUSD, eventA1, limits1110/4000000이다. Spent+held US$1.725786은 LOCAL catalogue meter이지 invoice가 아니다. 기록된 five-family60-pair baseline은600 request를 완료했다: Chat60/60, Responses60/60, Messages60/60, Generate56/60(incorrect-vision SSE4개), Interactions57/60(incorrect-vision buffered1개/SSE2개). 합계293/300 pair이며300/300은 아니다. 다른 old600 financial record는 보존하되 완전한 behavioral proof는 아니다. 이전 M5/media one-shot cohort는 그대로다. 이전 Google3-round prerequisite의 invalid-tool2개/unreadable-positive1개 실패 상태를 유지한다. 추가 유료 호출은 승인되지 않는다. 최종 SDK 증거와 native-axis 한계는 baseline 성공과 별개다. 이전 activation/reopen smoke는 두 번 reopen한 calls610/spent219159/held751233 및 SDK meter/canary/vision4-test19.38초 pass로 보존한다. 이는 범위가 정해진 이전 checkpoint이지 최종 ledger totals가 아니다. 이전 검증된 Chat60-pair cohort의 실제 attempt120,UpperBound charge120,UnknownHold 없음도 보존한다.

**Native-axis 관측은 cryptographic 검증·native consumption/equivalence가 아니다.** Generate는 mutation/omission/duplication을 받아들였다. Interactions는 isolated genuine source/positive control, one-owner signature mutation, thought-carrier omission, call-carrier omission, duplication을 받아들였다. 모든 thought/signature 제거는 generic400을 반환했고 THOUGHT item을 유지한 채 모든 signature field를 제거해도 generic400이었다. 마지막 capture에는 local encoded-original retention control만 있고 same-capture server positive는 없었다. 이전 positive cohort는 실제 증거다. 이는 aggregate-carrier-absence boundary만 입증하며 issuer/signature 검증이나 vendor consumption을 입증하지 않는다. 실제 report: SDK `config/qualification-extension-results.json`, `qualification-final-summary.json`, `qualification-native-axis-results.json`, `qualification-combined-omission-results.json`, `qualification-signature-presence-results.json`. Prerequisite-failed/not-run/negative-inconclusive 상태는 사실 그대로 유지한다. Thought-only/carrier-only omission은 다른 carrier가 남아 있는 상태에서 받아들여졌다. Issuer-validation/native-consumption 주장을 강화하지 않는다.

**실제 통합 증명과 남은 한계.** 최신 Core full run:2242 test, 실패0,skip16(RAM process-loss 비적용14개/live-credential gate2개),130.17초. `PgNestedJsonRoundTrips`는 duplicate key/order/null metadata,blob,residual을 정확히 보존하며0.18초 pass했다. 변경하지 않은 원래 shared-bank fork와 기존 Recorded CPU/Memory await/handoff scenario가 pass했다. 실제 wrappedMemory/SQLite/PostgreSQL/gRPC finite130/hold65/lower129/strip/old-head/pruning/no-archive/import probe는 plain과 ASan+UBSan에서 pass했다. LOCAL Memory/SQLite/PostgreSQL TSan scope는7개 pass,warning0이다. System Abseil/Protobuf를 포함한 full mixed gRPC TSan은 exit66,dependency/generated-RPC stack에 race warning402개였다. 이는 instrumentation/coverage 한계이지 proven false positive가 아니다. Remote TSan/race-free를 주장하지 않으며 warning을 suppress하지 않는다. Installed find_package Program C++/C ABI/dualQuickJS3 consumer는 pass했다. Fresh installed NeoGraph/SchemaProvider typed consumer는 실제 HTTP request2개,coroutine 시작 전 provider 소멸,native/tool replay,refusal,known-zero/raw 보존,실제 LinkedMismatch 거부를 pass했다. Browser Alice/Bob isolation·generation2 replacement를 실제 시각 검증했고 PostgreSQL Program Chat black-box6개는18.989초 pass했다. 최신 SDK26/26은 실패0,74.07초 pass했다. 최종 ReleaseGraph16설정 ×fresh process3회/48기록은38.29초,실패0,모든 actual protocol/owned-outcome check pass로 완료했다. NeoGraph `benchmarks/provider-cutover-final-results.json`과 `benchmarks/provider-cutover-final-summary.json`은 별도의 최종 cohort를 보존한다. 측정 중 compiler/유료 model은 실행하지 않았고 historical cohort는 그대로이며 semantic/resource equivalence를 주장하지 않는다. Unstable SDK/ABI3는 stable release나 더 넓은 platform qualification이 아니다.

Host 전달 limit, extent-bounded 진단/raw 증거, 공통 provider error와 최소 media 증거는 [typed provider reference](reference-en.md#owned-outcome)에 설명되어 있다.

---

<a id="8-streaming-events"></a>
## 8. 스트리밍 이벤트

`run_stream` / `run_stream_async`는 이벤트가 발생할 때 콜백을 호출합니다. 모드는 OR 가능한 비트마스크입니다:

| 모드 | 내보냅니다. |
|---|---|
| `EVENTS` | `NODE_START`, `NODE_END`, `INTERRUPT` |
| `TOKENS` | `LLM_TOKEN` 모든 스트리밍 토큰에 대해 `Provider` |
| `DEBUG` | `__routing__` 다음 준비 완료 집합을 보여주는 이벤트 |
| `VALUES` | `__state__` 모든 슈퍼스텝 이후 완전한 상태를 포함한 이벤트 |
| `UPDATES` | `CHANNEL_WRITE` `ChannelWrite`별 이벤트 |
| `ALL` | 위의 모든 것. |

```python
def cb(event):
    print(event.type, event.node_name, event.data)

engine.run_stream(
    ng.RunConfig(thread_id="t", input={...},
                 stream_mode=ng.StreamMode.EVENTS),
    cb)
```

> **참고:** `event.node_name` (`event.node` 아님). C++ 구조체 필드는 `node_name`이고, pybind는 원래 이름을 보존합니다.

채팅 형태의 스트리밍(증분 `content_so_far`을 포함한 LangChain 호환 메시지 딕셔너리)의 경우 헬퍼를 사용하세요:

```python
from neograph_engine import message_stream

engine.run_stream(
    ng.RunConfig(thread_id="t", input={...},
                 stream_mode=ng.StreamMode.TOKENS),
    message_stream(lambda chunk: print(chunk["content"], end="", flush=True)))
```

### `asio::io_context.run()` 배치(C++)

C++에서 `engine.run_stream_async()`를 구동할 때, 외부 `asio::io_context.run()`는 애플리케이션의 메인 스레드(또는 일반 프로세스 시작 경로를 통해 초기화된 오래 지속되는 스레드)에서 호출해야 합니다. 테스트된 정상 형태:

```cpp
// Main-thread driver — what examples/40 and the SchemaProvider tests use.
asio::io_context io;
asio::co_spawn(io, [&]() -> asio::awaitable<void> {
    result = co_await engine->run_stream_async(cfg, cb);
}, asio::detached);
io.run();
```

```cpp
// Dedicated worker thread driver — also fine.
std::thread t([&]() {
    asio::io_context io;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        result = co_await engine->run_stream_async(cfg, cb);
    }, asio::detached);
    io.run();
});
t.join();
```

> 과거 issue #16은 일부 glibc/OpenSSL 조합에서 request별 중첩 `io.run()`과 구 child-thread provider streaming bridge 사용 시 `getaddrinfo` SEGV를 관측했다. 해당 bridge는 typed 전환으로 제거되었다. 당시 구조 테스트는 downstream HTTPS/sanitizer/load 조건을 완전히 재현하지 못했으며 현재 검증이 아니다. 현재 호출자는 명시적 `ProviderMode`와 `invoke_async` / `dispatch_async`를 사용하며 request별 loop 중첩 대신 기존 장수 executor에서 실행한다. 가짜 단일 token이나 completion 재전송을 우회법으로 만들지 않는다.

---

## 8.5. Tracing — OpenTelemetry + Phoenix / Langfuse

`neograph_engine.tracing.otel_tracer`와 `neograph_engine.openinference.openinference_tracer`는 graph event를 run/node span으로 바꾼다. 후자는 `CHAIN` 태그와 node payload projection을 기록한다. Run마다 graph callback 하나를 선택한다. 모델 호출을 `LLM` span으로 기록하려면 graph compile 전에 typed provider를 `OpenInferenceProvider(inner, tracer, *, span_name="llm.complete")`로 감싼다. Wrapper는 native C++ observer와 상속받은 `prepare`/일회성 `dispatch` 또는 `invoke`를 쓴다. Request 준비나 폐기는 span을 열지 않고 승인된 dispatch는 owned outcome, 취소, deadline, typed event를 바꾸지 않으며 span을 연다. Tracer 실패는 provider 결과나 예외를 대체하지 않는다.

```python
from opentelemetry import context as otel_context
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import BatchSpanProcessor
from opentelemetry.exporter.otlp.proto.grpc.trace_exporter import OTLPSpanExporter
from neograph_engine import GraphEngine, NodeContext
from neograph_engine.openinference import OpenInferenceProvider, openinference_tracer


class ParentContextTracer:
    def __init__(self, tracer, parent_context):
        self.tracer, self.parent_context = tracer, parent_context

    def start_span(self, name):
        return self.tracer.start_span(name, context=self.parent_context)


def trace_graph(graph_spec, inner_provider, model, cfg):
    provider = TracerProvider()
    provider.add_span_processor(BatchSpanProcessor(
        OTLPSpanExporter(endpoint="http://localhost:4317", insecure=True)))
    tracer = provider.get_tracer("my-app")
    try:
        with openinference_tracer(tracer) as cb:
            parent = ParentContextTracer(tracer, otel_context.get_current())
            observed = OpenInferenceProvider(inner_provider, parent)
            engine = GraphEngine.compile(
                graph_spec, NodeContext(provider=observed, model=model))
            return engine.run_stream(cfg, cb)
    finally:
        provider.shutdown()
```

Local Phoenix endpoint에는 `docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix:latest`를 실행하고 `opentelemetry-api opentelemetry-sdk opentelemetry-exporter-otlp`를 설치한다. Graph specification, 기존 provider, 명시적 model과 `RunConfig`를 `trace_graph`에 전달한다. `ParentContextTracer`가 run root를 worker dispatch에 명시적으로 전달하며 자동 cross-thread 또는 node별 parent 전달은 보장하지 않는다. Python은 dispatch 시 활성 OTel context를 쓰고 prepared operation은 수명 동안 tracer adapter를 보유한다.

LLM span에는 공개 role/text projection, 선언된 scalar와 알려진 usage count만 넣는다. 알려진 0은 기록하고 미상은 생략한다. Native replay/reasoning, raw wire envelope/event와 encoded request body는 trace에서 제외하고 request/outcome에 실제 custody를 유지한다. 실패의 partial report를 포함한 usage 속성은 vendor charge나 budget authority를 입증하지 않는다. Charged/reserved accounting은 `UsageAccumulator.authority_snapshot()`과 Program의 `provider_budget_authority`에서 다룬다.

공개 text, 예외 메시지와 graph payload에도 application secret이 있을 수 있다. Exporter가 받는 데이터를 선택하거나 redact한다. [OpenTelemetry 민감 데이터 지침](https://opentelemetry.io/docs/security/handling-sensitive-data/)을 참고한다. [OpenInference convention](https://github.com/Arize-ai/openinference/blob/main/spec/semantic_conventions.md)은 `CHAIN`과 `LLM`을 정의한다. [참조](reference-en.md#105-observability--opentelemetry--openinference)는 NeoGraph의 속성 subset, token event, Python typed 호출 예제와 C++ 수명 요구사항을 설명한다.

---

<a id="9-common-pitfalls"></a>
## 9. 일반적인 함정

이들은 모두 실제 사용자가 접한 사례이며 [`docs/troubleshooting.md`](troubleshooting.md)에서 상호 참조됩니다.

### 내 ReAct 루프는 한 번만 실행됩니다

사용 중인 wheel이 ≤ 0.1.7입니다. 그래프 컴파일러가 `conditional_edges` 블록을 자동으로 삭제했습니다. ≥ 0.1.8로 업그레이드하세요. `result.execution_trace == ['llm', 'dispatch', 'llm']`로 확인하세요(`['llm']`만으로는 불충분).

### 제공자 호출이 60초 동안 멈춘 후 오류가 발생합니다

사용 중인 wheel이 ≤ 0.1.6입니다. 번들된 OpenSSL이 Ubuntu / Debian / macOS에 존재하지 않는 RHEL CA 경로를 하드코딩합니다. ≥ 0.1.7로 업그레이드하거나(가져오기 시 `SSL_CERT_FILE`를 certifi 번들로 자동 설정) `SSL_CERT_FILE`를 수동으로 설정하세요.

### 내 fan-out이 예상보다 느립니다

`compile()` 기본값은 `set_worker_count(1)` (엔진 소유 스레드 풀 없음 — fan-out 분기는 호출자의 실행기에서 직렬로 실행됨). 실제 병렬 처리를 위해서는 `engine.set_worker_count(N)` 를 호출하세요. 여기서 N은 Send fan-out 폭과 일치해야 하며, 또는 `engine.set_worker_count_auto()` 를 `hardware_concurrency()`용으로 호출하세요. NeoGraph는 또한 opt-in 풀 없이 다중 Send fan-out이 처음 실행될 때 일회성 stderr 경고를 출력합니다 — 이는 힌트이지 오류가 아닙니다. Python 사용자 정의 노드는 작은 fan-out에서 GIL 경합을 겪으므로, 1과 N 모두로 벤치마크하세요.

### Python RunResult 상태와 state 읽기

`result.status`는 typed `Completed`, `Interrupted`, `StepLimit`, `SafePoint` 상태를 노출한다. `result.output`은 portable 최종 state이며 `result.interrupted`, `result.max_steps_exhausted`, `result.execution_trace`는 run을 설명한다. `result.native_messages`, `result.provider_outcomes`는 전체 typed provider 증거를 보존한다. [Python binding 안내](python-binding.md#hitl-and-state)를 참조한다.

### "알 수 없는 리듀서: <name>"

두 리듀서가 기본 제공됩니다: `overwrite`와 `append`. 컴파일 전에 C++에서는 `ReducerRegistry::register_reducer`, Python에서는 `ng.ReducerRegistry.register_reducer`로 사용자 정의 리듀서를 등록하십시오.

### "조건이 등록되었지만 내 조건부 엣지가 실행되지 않습니다"

로더가 수용하는 형식( [§4](#4-edges--conditional-routing)의 형식 A 또는 형식 B)인지 확인하세요 — v0.1.8부터 둘 다 작동합니다. 이전 wheel에서는 형식 B만 작동합니다.

### "execution_trace에 시작 노드만 표시됩니다"

라우팅이 `__end__`로 폴스루(fall through)되었습니다. 대부분 시작 노드에서 엣지가 누락되었거나, 조건부가 `routes` 맵에 없는 값을 반환했고 명시적 `"default"` 경로가 `__end__`를 가리키는 경우입니다. 엄격한 그래프는 더 이상 맵 순서로 경로를 선택하지 않습니다. 열린 또는 지정되지 않은 조건은 선언된 경우 `"default"`를 사용하며, 그렇지 않으면 엔진이 소스 노드, 조건, 반환된 레이블과 함께 예외를 발생시킵니다. 닫힌 조건은 선언된 레이블 밖의 값을 반환하면 항상 예외를 발생시킵니다.

---

## 다음 단계

- [Python 예제](../bindings/python/examples/) — 위의 모든 개념을 다루는 21개의 자체 포함 스크립트.
- [C++ 예제](../examples/) — 동일한 구조의 36개 프로그램.
- [`reference-en.md`](reference-en.md) — 클래스별 완전한 API.
- [`ASYNC_GUIDE.md`](ASYNC_GUIDE.md) — 비동기/코루틴 계층에 대한 심층 분석.
