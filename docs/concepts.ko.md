<!-- neograph-i18n: source=docs/concepts.md locale=ko source_sha256=a7d9bae682dca57211d6c7ad0795977dc811bd5123934d73e9187a13e89e25f1 -->
**Languages:** [English](concepts.md) | [한국어](concepts.ko.md) | [日本語](concepts.ja.md) | [简体中文](concepts.zh-CN.md)

# NeoGraph 핵심 개념 - 서술형 가이드


예제를 살펴보기 전에 이 내용을 한 번 읽어보세요. 그것은
당신이 직접 구성한 순서대로 정신 모델: 그래프 →
채널 → 노드 → 에지 → 팬아웃 → 라우팅 재정의 →
체크포인트 → 스트리밍.

코드 샘플은 더 간결하기 때문에 Python 측입니다. 모든 것이 지도에 나온다
C++ API에 1:1(클래스는 [`reference-en.md`](reference-en.md) 참조)
서명 또는 [Doxygen](https://fox1245.github.io/NeoGraph/)
생성된 참조).

> **이전에 LangGraph를 사용한 적이 있는 경우:** 기본 요소는 의도적으로
> 동일 — 리듀서가 있는 채널, 쓰기를 내보내는 노드, 조건부
> 가장자리, `Send`, `Command`, 체크포인트. 차이점은 다음에 설명되어 있습니다.
> [Comparison with LangGraph](../README.md#vs-langgraph) 켜짐
> README. 아래 서술은 아무 것도 가정하지 않습니다.

---

## 목차

(v0.6.0 — `Tracing — OpenTelemetry + Phoenix / Langfuse`에 섹션 8.5가 추가되었습니다.
번호가 매겨진 제목은 외부 문서 링크를 안정적으로 유지하기 위해 1-9로 유지됩니다.
8.5는 스트리밍과 일반적인 함정 사이에 있습니다.)


1. [The big picture](#1-the-big-picture)
2. [Channels & reducers](#2-channels--reducers)
3. [Nodes](#3-nodes)
4. [Edges & conditional routing](#4-edges--conditional-routing)
5. [Send — dynamic fan-out](#5-send--dynamic-fan-out)
6. [Command — routing override + state patch](#6-command--routing-override--state-patch)
7. [Checkpoints, interrupts, HITL](#7-checkpoints-interrupts-hitl)
8. [Streaming events](#8-streaming-events)
9. [Common pitfalls](#9-common-pitfalls)

---

<a id="1-the-big-picture"></a>
## 1. 큰 그림

NeoGraph **그래프**는 다음 네 가지를 의미합니다.

|물건|그것은 무엇입니까|정의|
|---|---|---|
|**채널**|공유 상태의 명명된 슬롯입니다. 각각에는 새로운 쓰기가 기존 값과 결합되는 방식을 정의하는 감속기가 있습니다.|`definition["channels"]`|
|**노드**|상태를 읽고 쓰기를 내보내는 함수입니다(선택적으로 `Send` / `Command`).|`definition["nodes"]`|
|**가장자리**|정적 다음 노드 포인터.|`definition["edges"]`|
|**조건부 모서리**|조건자 기반 라우팅 — 상태에 따라 여러 다음 노드 중 하나를 선택합니다.|`definition["conditional_edges"]`|

실행은 **수퍼 스텝 루프**입니다.

```
1. ready_set = nodes routed from __start__
2. while ready_set is not empty:
   a. run all nodes in ready_set (in parallel if the executor allows)
   b. apply each node's writes to state
   c. collect their Send / Command / outgoing-edge signals
   d. plan_next_step → new ready_set
```

슈퍼스텝은 병렬성, 체크포인트, 그리고
스트리밍 이벤트. "지금"을 모두 실행할 수 있는 두 노드는 동일합니다.
슈퍼스텝; 동일한 입력 상태와 쓰기를 관찰합니다.
단계가 끝나면 감속기를 통해 결합합니다.

---

<a id="2-channels--reducers"></a>
## 2. 채널 및 리듀서

모든 상태는 이름이 지정된 채널에 있습니다. 채널은 여러 곳에서 지속됩니다.
노드 및 상위 단계 전반에 걸쳐; 노드는 쓰기를 통해 통신합니다.

### 채널 정의

```python
"channels": {
    "messages":  {"reducer": "append"},     # conversation history
    "counter":   {"reducer": "overwrite"},  # latest value wins
    "summary":   {"reducer": "overwrite"},
}
```

### 내장형 감속기

|감속기|새로운 쓰기 의미|일반적인 사용|
|---|---|---|
|`"overwrite"`|새로운 가치가 이전 가치를 대체합니다. 병렬 쓰기에서 마지막 작성자가 승리합니다.|단일 값 스크래치(현재 노드, 현재 질문, 경로 힌트)|
|`"append"`|새 목록(목록이어야 함)은 기존 목록에 연결됩니다. 순서: 이전 단계 값이 먼저, 이번 단계에서는 노드 실행 순서에 따라 쓰기가 추가됩니다.|대화 메시지, 검색 결과, 팬아웃 컬렉션.|

> 두 감속기는 모두 `ReducerRegistry::ReducerRegistry()`에 등록되어 있습니다.
> 엔진 시동 시([`src/core/graph_loader.cpp`](../src/core/graph_loader.cpp)).
> 사용자 지정 감속기는 `ReducerRegistry::register_reducer(name, fn)`를 통해 C++에서 등록됩니다.
> 또는 Python에서(v0.1.9부터):
>
> ```파이썬
> ng.ReducerRegistry.register_reducer("합계",
>     람다 전류, 수신: (현재 또는 0) + 수신)
> ```
>
> Python 호출 가능 항목은 GIL에서 실행됩니다. 동시 전송 팬아웃
> Python 사용자 정의 노드와 동일한 방식으로 직렬화합니다. 재등록 중
> 이름이 이전 감속기를 대체합니다.

### 채널에 쓰기

노드는 `ChannelWrite` 목록을 반환합니다.

```python
return [
    ng.ChannelWrite("messages", [{"role": "assistant", "content": "Hi!"}]),
    ng.ChannelWrite("counter",  state.get("counter", 0) + 1),
]
```

값의 모양은 감속기와 일치해야 합니다.
- `"append"` → 목록이어야 합니다(연결됨).
- `"overwrite"` → 모든 JSON 직렬화 가능 값.

### 노드에서 상태 읽기

```python
def run(self, input):
    msgs    = input.state.get("messages") or []  # list of message dicts
    counter = input.state.get("counter") or 0
    ...
```

`state.get(channel)`는 채널의 현재 값을 반환하거나 다음과 같은 경우 `None`를 반환합니다.
채널이 존재하지만 아직 기록되지 않았습니다. 입력하여 액세스하려면
채팅 메시지, `state.get_messages()`는 `list[ChatMessage]`를 반환합니다.
(`messages` 채널에서 구문 분석됨) — `llm_call`에서 내부적으로 사용됩니다.

### 버전

각 채널은 단조로운 `version` 번호를 전달합니다. 엔진이 사용하는
이는 체크포인트 비교 및 ​​`state.channel_version(name)`용입니다.
API 검사. 일반적으로 직접 읽지는 않습니다.

---

<a id="3-nodes"></a>
## 3. 노드

제어 순서에 따라 노드 유형을 등록하는 세 가지 방법은 다음과 같습니다.

### 3.1 내장 노드

|`type`(JSON에서)|기능|구성|
|---|---|---|
|`llm_call`|`provider->complete_async(messages, tools)`를 호출하고 보조 메시지를 `messages`에 추가합니다.|`NodeContext`에서 `provider`, `model`, `instructions`, `tools`를 읽습니다.|
|`tool_dispatch`|최신 보조 메시지의 `tool_calls`를 살펴보고 `Tool::execute`를 통해 각각을 실행하고 `{role: "tool", tool_call_id, content}` 결과를 추가합니다.|`NodeContext`에서 `tools`를 읽습니다.|
|`intent_classifier`|LLM는 사용자 의도를 N개 레이블 중 하나로 분류하고 선택한 레이블을 `__route__`에 씁니다. 조건부로 `route_channel`와 페어링됩니다.|`extra_config: {labels, prompt_template}`|
|`subgraph`|다른 그래프를 단일 노드로 포함합니다. 내부 상태는 구성된 키 재매핑을 통해 매핑됩니다.|`extra_config: {graph_def, input_keys, output_keys}`|

### 3.2 `@ng.node` 데코레이터(Python에만 해당)

쓰기 전용 노드를 정의하는 가장 짧은 방법은 다음과 같습니다.

```python
@ng.node("greet")
def greet_node(state):
    name = state.get("name") or "world"
    return [ng.ChannelWrite("messages",
        [{"role": "assistant", "content": f"Hello, {name}!"}])]
```

장식된 함수는 `list[ChannelWrite]`(또는 `None`,
`[]`)로 처리됩니다. `Send` 또는 `Command`를 방출할 수 없습니다.
서브클래스 `GraphNode`.

### 3.3 전체 `GraphNode` 하위 클래스

모든 권한을 부여하려면 `run(input)`를 재정의하세요. v0.4.0에서 도입되었으며
v0.9.0 이후의 유일한 사용자 정의 노드 진입점 — 하나의 방법, 하나
서명:

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

Python은 `cancel_token`, `thread_id`, `step`, `stream_mode`, `store`를 노출합니다.
및 `input.ctx`의 `resume_value`. C++ `deadline` 및 `trace_id` 슬롯은 다음과 같습니다.
향후 `RunConfig` 필드를 위해 예약되었습니다. 엔진은 그것들을 채우지 않으며
Python은 아직 이를 노출하지 않습니다.

필요하지 않은 경우 기본 `list[ChannelWrite]`를 반환할 수도 있습니다.
`Send` 또는 `Command` - 바인딩이 `NodeResult`로 들어갑니다.
자동으로.

> **v0.3.x에서 마이그레이션:** 8개 가상 체인(`execute`,
> `execute_async`, `execute_full`, `execute_full_async`,
> `execute_stream`, `execute_stream_async`, `execute_full_stream`,
> `execute_full_stream_async`)는 v0.4.x에서 더 이상 사용되지 않으며 다음에서 제거되었습니다.
> v1 준비 중 v0.9.0. 싱글로 교체하세요
> `run(input)` 재정의; `input.state`에서 상태를 읽고 토큰을 내보냅니다.
> None이 아닌 경우 `input.stream_cb`를 통해 취소 토큰을 읽습니다.
> `input.ctx.cancel_token`.

JSON 로더가 인스턴스화할 수 있도록 유형을 등록합니다.

```python
ng.NodeFactory.register_type(
    "researcher",
    lambda name, config, ctx: Researcher(name),
)
```

공장에서는 `(name, per-node config, NodeContext)`를 확인하므로 동일합니다.
클래스는 다른 구성을 사용하여 여러 이름으로 인스턴스화될 수 있습니다.

### 3.4 도구(`tool_dispatch`에서 사용되는 별도의 개념)

`Tool`는 노드가 아닙니다. `tool_dispatch`가 호출하는 것입니다. 아강
`ng.Tool`, 세 가지 메서드를 재정의하고 인스턴스를 다음으로 전달합니다.
`NodeContext(tools=[…])`:

```python
class CalcTool(ng.Tool):
    def get_name(self):       return "calc"
    def get_definition(self): return ng.ChatTool(name="calc", ...)
    def execute(self, args):  return str(args["x"] * 2)
```

엔진은 컴파일 타임에 도구 목록의 소유권을 갖습니다.
나중에 로컬 참조가 삭제될 수 있습니다.

---

<a id="4-edges--conditional-routing"></a>
## 4. 에지 및 조건부 라우팅

### 정적 가장자리

```python
"edges": [
    {"from": ng.START_NODE, "to": "llm"},
    {"from": "dispatch",    "to": "llm"},
    {"from": "summarizer",  "to": ng.END_NODE},
]
```

동일한 소스 노드의 여러 에지가 팬아웃됩니다(모든 후속 노드는
다음 슈퍼스텝의 레디 세트에 포함) 동일한 대상에 대한 두 개의 모서리
한 번의 슈퍼 단계 중복 제거부터 한 번의 대상 실행까지.

### 조건부 모서리

조건부 에지는 **명명된 조건**을 실행하고 다음 노드를 선택합니다.
`routes` 지도에서:

```python
"conditional_edges": [
    {
        "from": "llm",
        "condition": "has_tool_calls",
        "routes": {"true": "dispatch", "false": ng.END_NODE},
    }
]
```

조건 이름은 다음에 등록된 `ConditionFn`로 확인됩니다.
엔진. 두 가지가 내장되어 제공됩니다.

|상태|보고|언제 사용하나요?|
|---|---|---|
|`has_tool_calls`|최신 보조 메시지에 비어 있지 않은 `tool_calls`가 있는 경우 `"true"`; 그렇지 않으면 `"false"`입니다.|ReAct 루프 — LLM가 요청을 멈출 때까지 도구를 계속 파견합니다.|
|`route_channel`|`__route__` 채널에 있는 문자열은 무엇이든 상관없습니다. `"default"`로 대체됩니다.|명시적인 의도 라우팅을 위해 `intent_classifier`와 페어링됩니다.|

`ConditionRegistry::register_condition(name, fn)`를 통해 C++에서 사용자 정의 조건 등록
또는 Python에서(v0.1.9부터):

```python
def is_long(state):
    msgs = state.get("messages") or []
    return "long" if len(msgs) > 10 else "short"

ng.ConditionRegistry.register_condition("is_long", is_long)
```

콜러블은 라이브 `GraphState`를 수신합니다(따라서 `state.get(channel)` 및
`state.get_messages()` 작업) 다음 중 하나와 일치하는 문자열을 반환해야 합니다.
조건부 가장자리의 `routes` 키.

### 두 가지 동등한 형식 - 둘 다 v0.1.8부터 작동합니다.

조건부 가장자리는 `edges` 배열 내부에 있을 수 있습니다(
`condition` 필드) **또는** 별도의 `conditional_edges` 블록에 있습니다.
두 가지 양식 모두 허용됩니다. 더 명확한 것을 선택하십시오.

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

> **역사:** 양식 A는 이전에 그래프 컴파일러에 의해 자동으로 삭제되었습니다.
> v0.1.8 — README 및 모든 Python 예제가 이를 사용하므로 ReAct가 반복됩니다.
> 단일 LLM 호출로 변질되었습니다. 커밋 `e23a523`에서 수정되었습니다. 당신이
> 0.1.7 이하 휠에서 이를 확인하세요. 업그레이드하세요.

---

<a id="5-send--dynamic-fan-out"></a>
## 5. 보내기 - 동적 팬아웃

`Send`는 다음 단계 노드의 수가 의존하는 경우를 위한 것입니다.
상태. 고전적인 용도: 검색 주제 목록을 N개 병렬로 분할
연구원 호출.

```python
class Planner(ng.GraphNode):
    def execute_full(self, state):
        topics = decide_topics(state)                  # e.g. 5 strings
        return ng.NodeResult(
            writes=[],
            sends=[ng.Send("researcher", {"topic": t}) for t in topics],
        )
```

엔진의 `run_sends_async`는 1회당 한 번씩 `researcher`를 인스턴스화합니다.
`Send`는 각각 고유한 `state.get("topic")`를 가지며 다음에서 실행됩니다.
`asio::experimental::make_parallel_group`를 통해 병렬.

### 정신 모델

`Send(target, payload)`는 "이 상태로 `target`를 인스턴스화합니다.
패치를 적용하고 준비된 세트에 추가합니다." 페이로드는 다음과 같이 적용됩니다.
대상이 `state`를 보기 전에 상태 쓰기를 수행합니다.

병렬 그룹이 완료된 후 다음 상위 단계의 라우팅이 시작됩니다.
Send가 생성한 각 작업의 나가는 가장자리(또는 해당 `Command.goto`,
하나를 방출한 경우).

### 일반적인 형태: 팬아웃 5, 요약기로 팬인

```
planner ─┬─ Send("researcher", {topic: "A"})  ─┐
         ├─ Send("researcher", {topic: "B"})  ─┤
         ├─ Send("researcher", {topic: "C"})  ─┼─→ summarizer
         ├─ Send("researcher", {topic: "D"})  ─┤
         └─ Send("researcher", {topic: "E"})  ─┘
```

`researcher`의 나가는 가장자리는 단지 `{"from": "researcher", "to": "summarizer"}`입니다.
— 정적 에지와 동일한 중복 제거 규칙이므로 요약자가 한 번 실행됩니다.

### 작업자 수 조정

`build()`의 기본값은 `EngineConfig::worker_count == 1`입니다. 엔진 소유 스레드가 없습니다.
풀, 팬아웃 분기는 코루틴 자체에서 인라인으로 전달됩니다.
집행자. 이는 순차적인 경우 비용이 적게 드는 할당되지 않은 빠른 경로입니다.
스레드로부터 안전하지 않은 상태를 유지하는 노드에 대한 그래프 및 안전입니다.

실제 병렬 처리를 위해서는 풀을 명시적으로 선택하세요. 정확히 N을 선택하세요.
팬아웃 너비를 일치시키거나 `set_worker_count_auto()`를 사용하여
`hardware_concurrency()`(대체 4):

```python
engine.set_worker_count(5)           # match a 5-way Send
# or
engine.set_worker_count_auto()       # hardware_concurrency()
```

다중 전송(또는 다중 송신 에지) 팬아웃이
옵트인 풀에서 NeoGraph는 일회성 stderr 경고를 내보냅니다.
무음 직렬 케이스는 레이더 아래로 날아가지 않습니다. 다음으로 억제
의도적으로 운전하는 경우 `NEOGRAPH_SUPPRESS_FANOUT_WARNING=1`
직렬 팬아웃(예: 작업자 벤치마크=1 빠른 경로)

---

<a id="6-command--routing-override--state-patch"></a>
## 6. 명령 - 라우팅 재정의 + 상태 패치

`Command`를 사용하면 노드가 다음에 AND로 이동할 위치를 결정할 수 있습니다.
동일한 반환 값. 이는 일반 나가는 가장자리를 우회합니다.

```python
class Evaluator(ng.GraphNode):
    def execute_full(self, state):
        if score(state) >= 0.8:
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
                    updates=[ng.ChannelWrite("retries",  state.get("retries", 0) + 1)],
                ),
            )
```

### 명령과 조건부 가장자리를 사용하는 경우

- **조건부 에지**: 라우팅은 상태 조건자에 따라 달라집니다.
노드 로직이 필요하지 않습니다. 더 깨끗하고 선언적입니다.
- **명령**: 라우팅은 작성하기에 가장 자연스러운 논리에 따라 달라집니다.
노드 내부 — 다중 기준 채점, 콘텐츠 검사, 재시도
결정. 또한 상태 AND를 원자적으로 업데이트하는 유일한 방법은 다음을 선택합니다.
다음 노드.

### 마지막 작가가 팬인을 통해 승리

여러 명령이 동일한 슈퍼 단계에서 실행되는 경우(드물지만
여러 병렬 그룹 형제가 이를 방출할 때 가능), 마지막
하나가 승리합니다. 순서는 병렬 그룹 완료에 따라 결정됩니다.
비결정적입니다. 최대 하나를 보장하여 이를 중심으로 설계합니다.
형제는 `Command`를 내보냅니다.

---

<a id="7-checkpoints-interrupts-hitl"></a>
## 7. 체크포인트, 인터럽트, HITL

### 체크포인트 저장소 설정

```python
engine.set_checkpoint_store(ng.InMemoryCheckpointStore())
# or: engine.set_checkpoint_store(ng.PostgresCheckpointStore(...))   # if built with PG
```

저장소가 연결되면 모든 슈퍼 단계는
`(thread_id, checkpoint_id)`를 입력한 저장소입니다. `RunResult.checkpoint_id`
필드가 최신입니다.

### 정적 인터럽트 지점

```python
"interrupt_before": ["payment"],   # pause before this node runs
"interrupt_after":  ["llm"],       # pause after, before routing
```

엔진은 `interrupted=True`와 함께 `RunResult`를 반환하고
`interrupt_node`가 설정되었습니다. 재개하려면:

```python
result = await engine.resume_async(thread_id="t1",
                                   checkpoint_id=result.checkpoint_id,
                                   new_input={...})  # optional
```

### `NodeInterrupt`를 통한 동적 인터럽트

노드 본문 내부에서 던지기(Python: `raise ng.NodeInterrupt(reason)`,
C++: `throw NodeInterrupt(...)`). 엔진이 상태를 포착하고 지속하며,
던지는 노드에서 중단된 `RunResult`를 반환합니다 — 동일
API를 재개합니다.

일시 중지 결정이 중간 노드 출력에 따라 달라질 때 유용합니다.
(예: "LLM가 인간에게 보여줄 가치가 있는 것을 생산했습니까?")

### 시간여행

`engine.fork(thread_id, from_checkpoint_id)`는 다음과 같은 새 스레드를 반환합니다.
과거 체크포인트부터 시작됩니다. "내가 대답했다면 어땠을까?"에 유용합니다.
다르게" 분기.

---

<a id="8-streaming-events"></a>
## 8. 스트리밍 이벤트

`run_stream` / `run_stream_async`는 이벤트가 발생하면 콜백을 호출합니다.
모드는 OR 가능 비트마스크입니다.

|방법|방출|
|---|---|
|`EVENTS`|`NODE_START`, `NODE_END`, `INTERRUPT`|
|`TOKENS`|`Provider`의 모든 스트리밍 토큰에 대한 `LLM_TOKEN`|
|`DEBUG`|다음 준비 세트를 보여주는 `__routing__` 이벤트|
|`VALUES`|모든 슈퍼 단계 후에 전체 상태가 포함된 `__state__` 이벤트|
|`UPDATES`|`ChannelWrite`당 `CHANNEL_WRITE` 이벤트|
|`ALL`|위의 모든 것|

```python
def cb(event):
    print(event.type, event.node_name, event.data)

engine.run_stream(
    ng.RunConfig(thread_id="t", input={...},
                 stream_mode=ng.StreamMode.EVENTS),
    cb)
```

> **참고:** `event.node_name`(`event.node` 아님). C++ 구조체 필드
> `node_name`입니다. pybind는 원래 이름을 유지합니다.

채팅 형태의 스트리밍(LangChain 호환 메시지는
증분 `content_so_far`), 도우미를 사용하십시오.

```python
from neograph_engine import message_stream

engine.run_stream(
    ng.RunConfig(thread_id="t", input={...},
                 stream_mode=ng.StreamMode.TOKENS),
    message_stream(lambda chunk: print(chunk["content"], end="", flush=True)))
```

### `asio::io_context.run()` 배치(C++)

C++에서 `engine.run_stream_async()`를 구동할 때 외부
`asio::io_context.run()`는 애플리케이션의 메인에서 호출되어야 합니다.
스레드(또는 다음을 통해 초기화된 수명이 긴 스레드)
일반 프로세스 시작 경로). 테스트를 거친 양호한 모양:

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

> **알려진 제한 사항 — HTTP 서버 작업자 내부에 중첩된 `io.run()`
> 콜백**(#16 문제): `asio::io_context.run()`를 내부에 중첩
> `httplib::Server::set_chunked_content_provider`(또는 이에 상응하는 것)
> 자체적으로 하위 스레드를 생성하는 요청별 작업자 콜백
> `Provider::complete_stream_async`의 기본 브리지)가 관찰되었습니다.
> 일부 glibc/OpenSSL 조합에서는 `getaddrinfo`의 SEGV로 변경됩니다. 그만큼
> 트리 내 테스트
> ([`tests/test_schema_provider_stream_async_nested_thread.cpp`](../tests/test_schema_provider_stream_async_nested_thread.cpp))
> 구조적 형상을 덮고 깨끗하게 통과하지만 하류
> 환경(glibc 해석기인 HTTPS에 대한 실제 `api.openai.com`)
> TSan/ASan에서 동시 요청 로드)는 철저하지 않습니다.
> 테스트 스위트에서 재현 가능합니다. **해결 방법:**
>
> 1. **대신 `co_await provider->complete_async(...)`를 사용하세요.
>    HTTP 서버 콜백 내부의 `complete_stream_async`** 및
>    조합된 응답을 하나의 `LLM_TOKEN` 이벤트로 내보냅니다.
>    돕는 사람. 토큰 입력 UX가 손실되었습니다. 엔진 + 노드 + 도구 루프 작업
>    끝까지. 이것이 ProjectDatePop의 다운스트림 `cpp_backend`입니다.
>    오늘 사용합니다.
> 2. **요청별 콜백에서 `io.run()`를 이동**: 하나 실행
>    전용 작업자 스레드의 수명이 긴 `asio::io_context`
>    엔진에 요청별 작업을 대기열에 추가하고 결과를 다시 게시합니다.
>    HTTP 서버의 응답 싱크에 넣습니다. 요청별 회피
>    SEGV와 상관 관계가 있는 중첩된 `std::thread` 생성입니다.

---

## 8.5. 추적 — OpenTelemetry + Phoenix / Langfuse

스트리밍과 동일한 콜백 형태, 다른 소비자. Otel 통과
`engine.run_stream(cfg, cb)` 및 모든
`NODE_START` / `NODE_END` / `ERROR` / `INTERRUPT` 이벤트는
기간.

두 개의 레이어가 트리 내로 제공됩니다.

  - `neograph_engine.tracing.otel_tracer` — 공급업체 중립적인 OTel
    spans. Spans flow to any OTel backend (Jaeger, Tempo, Honeycomb,
    Datadog).
  - `neograph_engine.openinference` — LLM 모양 속성 레이어
    that turns the same spans into a *LangSmith-style chat-bubble
    trace* in Phoenix / Arize / Langfuse:

```python
from opentelemetry import trace
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import BatchSpanProcessor
from opentelemetry.exporter.otlp.proto.grpc.trace_exporter import OTLPSpanExporter
from neograph_engine.openinference import OpenInferenceProvider, openinference_tracer

trace.set_tracer_provider(TracerProvider())
trace.get_tracer_provider().add_span_processor(
    BatchSpanProcessor(OTLPSpanExporter(endpoint="http://localhost:4317", insecure=True)))
tracer = trace.get_tracer("my-app")

# Wrap the provider — every Provider.complete() now emits an LLM-kind span.
wrapped = OpenInferenceProvider(real_provider, tracer)
ctx = ng.NodeContext(provider=wrapped)
engine = ng.GraphEngine.compile(graph_def, ctx)

with openinference_tracer(tracer) as cb:
    engine.run_stream(ng.RunConfig(input={"messages": [...]}), cb)
```

Phoenix를 한 번 가동하세요. `docker run -d -p 6006:6006 -p 4317:4317
arizephoenix/phoenix`. http://localhost:6006 열기 — 추적
체인(`graph.run` → `node.X` → `llm.complete`)으로 렌더링됩니다.
프롬프트/응답/토큰 수는 LLM 세부 정보 창에 표시됩니다.
동일한 코드, OTLP 엔드포인트 URL를 Langfuse 자체 호스트로 교체하고
같은 모양으로 흔적이 나타납니다.

*"NeoGraph에는 LangSmith가 없습니다"*에 대한 답변입니다.
LangSmith UX(채팅 풍선, DAG 계층 구조, 토큰 비용)를 다음 방법으로 가져옵니다.
하나의 Docker 명령으로 Phoenix 또는 Langfuse를 로컬로 실행합니다. 아니요
SaaS 계약, 추적당 가격 책정 없음.

속성 키 스키마에 대해서는 `docs/reference-en.md` §10.5를 참조하세요.
`otel_tracer`와 `openinference_tracer` 사이의 절충 사항.

---

<a id="9-common-pitfalls"></a>
## 9. 일반적인 함정

이것들은 모두 실제 사용자들에게 타격을 입었습니다. 에서 상호 참조됨
[`docs/troubleshooting.md`](troubleshooting.md).

### "내 ReAct 루프는 한 번만 실행됩니다."

당신은 0.1.7 이하의 휠을 사용하고 있습니다. 그래프 컴파일러가
`conditional_edges`는 자동으로 차단됩니다. ≥ 0.1.8로 업그레이드하세요. 확인
`result.execution_trace == ['llm', 'dispatch', 'llm']`(그뿐만 아니라
`['llm']`).

### "공급자 호출이 60초 동안 중단된 후 오류가 발생합니다."

당신은 0.1.6 이하의 휠을 타고 있습니다. 번들 OpenSSL 하드코드 RHEL CA 경로
Ubuntu / Debian / macOS에는 존재하지 않습니다. ≥ 0.1.7로 업그레이드
(가져올 때 `SSL_CERT_FILE`를 인증서 번들로 자동 설정) 또는 설정
`SSL_CERT_FILE`를 수동으로.

### "팬아웃이 예상보다 느립니다."

`compile()`의 기본값은 `set_worker_count(1)`입니다(엔진 소유 스레드 없음).
풀 — 팬아웃 분기는 호출자의 실행기에서 직렬로 실행됩니다. 을 위한
N이 일치하는 실제 병렬 처리 호출 `engine.set_worker_count(N)`
팬아웃 너비 보내기 또는 `engine.set_worker_count_auto()`
`hardware_concurrency()`. NeoGraph는 또한 원샷 stderr를 인쇄합니다.
다중 전송 팬아웃이 옵트인 없이 처음 실행될 때 경고
pool — 그건 힌트이지 오류가 아닙니다. Python 사용자 정의 노드는 GIL를 참조하세요.
소규모 팬아웃에 대한 경합이 있으므로 1과 N을 모두 사용합니다.

### "Python RunResult에는 .status / .final_state 속성이 없습니다."

Python 바인딩은 이러한 속성을 노출하지 않습니다. `result.output`를 사용하세요.
`result.interrupted`, `result.max_steps_exhausted` 및
`result.execution_trace`. C++ 호출자는 `RunResult::status()`를 사용할 수 있습니다.
`Completed` / `Interrupted` / `StepLimit` 보기를 입력했습니다. 의 표를 참조하세요.
README의 "출력 읽기" 섹션.

### "알 수 없는 감속기: <name>"

두 가지 감속기가 배송됩니다: `overwrite` 및 `append`. 맞춤형 감속기에는 다음이 필요합니다.
C++의 `ReducerRegistry::register_reducer`(아직 Python 후크 없음).

### "조건이 등록되었지만 조건부 에지가 실행되지 않습니다."

양식이 로더가 허용하는 양식인지 확인하십시오(양식 A 또는 양식 B).
[§4](#4-edges--conditional-routing)) — 둘 다 v0.1.8부터 작동합니다. ~에
오래된 바퀴는 B형만 작동합니다.

### "execution_trace는 시작 노드만 표시합니다"

라우팅은 `__end__`로 이루어졌습니다. 가장자리가 누락되었을 가능성이 높습니다.
시작 노드 또는 조건이
`routes` 맵(이 경우 엔진은
대체 수단으로 사전식 마지막 경로 — 놀라운 요소).

---

## 다음은 어디로

- [Python examples](../bindings/python/examples/) — 21개 독립형
위의 모든 개념을 다루는 스크립트.
- [C++ examples](../examples/) — 동일한 구조를 가진 36개의 프로그램.
- [`reference-en.md`](reference-en.md) — 철저한 클래스별 API.
- [Doxygen](https://fox1245.github.io/NeoGraph/) — 생성된 참조
C++ 헤더의 경우.
- [`ASYNC_GUIDE.md`](ASYNC_GUIDE.md) — 비동기/코루틴에 대한 심층 분석
층.
