<!-- neograph-i18n: source=docs/concepts.md locale=ko source_sha256=3d95cddd2a9d9ff0c7b8028968a5bfab4c44b404af3eff0115f8edfb25a7f1cc -->
# NeoGraph 핵심 개념 — 해설 가이드

**Languages:** [English](concepts.md) | [한국어](concepts.ko.md) | [日本語](concepts.ja.md) | [简体中文](concepts.zh-CN.md)

예제를 살펴보기 전에 이 문서를 먼저 읽어 보세요. 직접 그래프를 구성하는 순서대로 개념 모델을 구축하게 됩니다: 그래프 → 채널 → 노드 → 엣지 → fan-out → 라우팅 재정의 → 체크포인트 → 스트리밍.

코드 샘플은 더 간결하므로 Python 쪽으로 제공됩니다. 모든 내용은 C++ API에 1:1로 대응합니다(클래스 시그니처는 [`reference-en.md`](reference-en.md) 및 `include/neograph/` 아래의 공개 헤더 참조).

> **LangGraph를 사용해 본 적이 있다면:** 기본 요소는 의도적으로 동일합니다 — 리듀서가 있는 채널, 쓰기를 내보내는 노드, 조건부 엣지, `Send`, `Command`, 체크포인트. README는 NeoGraph의 [두 런타임 계층](../README.md#two-runtime-layers)을 요약합니다. 아래의 설명은 아무것도 가정하지 않습니다.

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
   a. run all nodes in ready_set (in parallel if the executor allows)
   b. apply each node's writes to state
   c. collect their Send / Command / outgoing-edge signals
   d. plan_next_step → new ready_set
```

수퍼-스텝은 병렬성, 체크포인트, 스트리밍 이벤트의 단위입니다. "지금" 실행될 수 있는 두 노드는 동일한 수퍼-스텝입니다; 그들은 동일한 입력 상태를 관찰하며 스텝이 끝날 때 쓰기가 리듀서를 통해 결합됩니다.

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

### 채널에 쓰기

노드는 `ChannelWrite` 목록을 반환합니다:

```python
return [
    ng.ChannelWrite("messages", [{"role": "assistant", "content": "Hi!"}]),
    ng.ChannelWrite("counter",  state.get("counter", 0) + 1),
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
| `llm_call` | `provider->complete_async(messages, tools)`를 호출하고 어시스턴트 메시지를 `messages`에 추가합니다. | 읽기 `provider`, `model`, `instructions`, `tools` 에서 `NodeContext`. |
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

Python은 `cancel_token`, `thread_id`, `step`, `stream_mode`, `store`, 그리고 `resume_value` 를 `input.ctx`에 노출합니다. C++ 호출자는 `deadline` 와 `trace_id` 를 `RunMetadata`에 설정할 수 있습니다; 엔진은 이를 중첩된 서브그래프를 통해 전파합니다. 이 두 필드는 아직 Python 바인딩에서 노출되지 않습니다.

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
    def get_definition(self): return ng.ChatTool(name="calc", ...)
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

`Send`는 다음 단계 노드 수가 상태에 따라 달라지는 경우를 위한 것입니다. 전형적인 사용 사례: 검색 주제 목록을 N개의 병렬 연구자 호출로 분할합니다.

```python
class Planner(ng.GraphNode):
    def run(self, input):
        topics = decide_topics(input.state)            # e.g. 5 strings
        return ng.NodeResult(
            writes=[],
            sends=[ng.Send("researcher", {"topic": t}) for t in topics],
        )
```

엔진의 `run_sends_async` 인스턴스화합니다 `researcher` 마다 한 번씩 `Send`, 각각 고유한 `state.get("topic")`, 그리고 이를 통해 병렬로 실행합니다 `asio::experimental::make_parallel_group`.

### 정신적 모델

`Send(target, payload)`는 "이 상태 패치로 `target`를 인스턴스화하고 준비 집합에 추가"하는 것입니다. 페이로드는 대상이 `state`를 보기 전에 상태 쓰기로 적용됩니다.

병렬 그룹이 완료된 후, 다음 슈퍼 스텝의 라우팅은 각 Send가 생성한 작업의 나가는 엣지(또는 하나를 방출한 경우 해당 `Command.goto`)에서 옵니다.

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

`build()`는 기본적으로 `EngineConfig::worker_count == 1`로 설정됩니다 — 엔진 소유 스레드 풀이 없으며, fan-out 분기는 코루틴 자체 실행기에서 인라인으로 디스패치됩니다. 이는 순차 그래프에 저렴하고 비스레드 안전 상태를 보유한 노드에 안전한 무할당 빠른 경로입니다.

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
                    updates=[ng.ChannelWrite("retries",  input.state.get("retries", 0) + 1)],
                ),
            )
```

### when Command works vs auxiliary edge condition.

- **조건부 엣지(conditional edge)**: 라우팅은 노드 로직이 필요 없는 상태 조건자(state predicate)에 의존합니다. 더 깔끔하고 선언적입니다.
- **Command**: 라우팅은 노드 내부에 작성하는 것이 가장 자연스러운 로직(다중 기준 점수 산정, 콘텐츠 검사, 재시도 결정)에 의존합니다. 또한 상태를 원자적으로 업데이트하면서 다음 노드를 선택하는 유일한 방법이기도 합니다.

### fan-in 상황에서의 마지막 쓰기 승리(Last-writer-wins)

동일한 슈퍼스텝에서 여러 Command가 발생하는 경우(드묾 — 여러 병렬 그룹 형제가 이를 방출할 때만 가능), 마지막 것이 우선합니다. 순서는 비결정적인 병렬 그룹 완료에 의해 결정됩니다 — 최대 하나의 형제만 `Command`를 방출하도록 보장하여 이를 설계하세요.

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

`engine.fork(thread_id, from_checkpoint_id)`는 과거 체크포인트에서 시작하는 새 스레드를 반환합니다. "다르게 답했다면 어땠을까" 분기 탐색에 유용합니다.

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

> **알려진 제한 사항 — HTTP 서버 워커 콜백 내부의 중첩된 `io.run()`** (이슈 #16): `asio::io_context.run()` 내부에 `httplib::Server::set_chunked_content_provider` (또는 `Provider::complete_stream_async`의 기본 브리지를 통해 자체적으로 하위 스레드를 생성하는 동등한 요청별 워커 콜백)을 중첩하면 일부 glibc / OpenSSL 조합에서 `getaddrinfo`의 SEGV가 관찰되었습니다. 트리 내부 테스트 ([`tests/test_schema_provider_stream_async_nested_thread.cpp`](../tests/test_schema_provider_stream_async_nested_thread.cpp))는 구조적 형태를 다루며 깨끗하게 통과하지만, 다운스트림 환경 (HTTPS를 통한 실제 `api.openai.com`, TSan / ASan 하의 glibc 리졸버, 동시 요청 부하)은 테스트 스위트에서 완전히 재현할 수 없습니다. **해결 방법:**
>
> 1. **`co_await provider->complete_async(...)`를 사용하고 HTTP 서버 콜백 안의 `complete_stream_async`는 사용하지 않습니다.** 조립한 응답은 헬퍼에서 하나의 `LLM_TOKEN` 이벤트로 내보냅니다. 토큰이 입력되는 듯한 UX는 사라지지만 엔진, 노드, 도구 루프는 종단 간으로 작동합니다. 현재 ProjectDatePop의 다운스트림 `cpp_backend`가 이 방식을 사용합니다.
> 2. **`io.run()`를 요청별 콜백 밖으로 이동**: 엔진용 전용 워커 스레드에서 하나의 장수명 `asio::io_context`를 실행하고, 요청별 작업을 큐에 넣고 결과를 HTTP 서버의 응답 싱크로 다시 게시합니다. SEGV와 상관관계가 있는 요청별 중첩 `std::thread` 생성이 방지됩니다.

---

## 8.5. 추적 — OpenTelemetry + Phoenix / Langfuse

스트리밍과 동일한 콜백 형태, 다른 소비자. OTel 트레이서 방출 콜백을 `engine.run_stream(cfg, cb)`에 전달하면 모든 `NODE_START` / `NODE_END` / `ERROR` / `INTERRUPT` 이벤트는 스팬이 됩니다.

두 레이어가 인트리로 제공됩니다

  - `neograph_engine.tracing.otel_tracer` — 벤더 중립적인 OTel 스팬. 스팬은 모든 OTel 백엔드(Jaeger, Tempo, Honeycomb, Datadog)로 전달됩니다.
  - `neograph_engine.openinference` — 동일한 스팬을 Phoenix / Arize / Langfuse에서 *LangSmith 스타일 채팅 버블 트레이스*로 변환하는 LLM 형태 속성 계층:

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

Phoenix를 한 번 실행하세요: `docker run -d -p 6006:6006 -p 4317:4317
arizephoenix/phoenix`. http://localhost:6006를 열면 트레이스가 체인(`graph.run` → `node.X` → `llm.complete`)으로 렌더링되며 프롬프트 / 응답 / 토큰 수가 LLM 세부 정보 창에 표시됩니다. 동일한 코드에서 OTLP 엔드포인트 URL을 Langfuse 자체 호스팅으로 바꾸면 동일한 형태로 트레이스가 표시됩니다.

이것이 *"NeoGraph에는 LangSmith가 없다"*에 대한 답변입니다 — Phoenix나 Langfuse를 하나의 Docker 명령으로 로컬에서 실행하면 LangSmith UX(채팅 버블, DAG 계층 구조, 토큰 비용)를 얻을 수 있습니다. SaaS 계약도, 건별 추적 가격도 없습니다.

참조 `docs/reference-en.md` §10.5에서 속성-키 스키마와 `otel_tracer` 및 `openinference_tracer` 간의 상충 관계(trade-off)에 대한 참고 사항을 확인하십시오.

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

### "Python RunResult에는 .status / .final_state 속성이 없습니다"

Python 바인딩은 해당 속성을 노출하지 않습니다. `result.output`, `result.interrupted`, `result.max_steps_exhausted`, `result.execution_trace`를 사용하십시오. C++ 호출자는 타입이 지정된 `Completed` / `Interrupted` / `StepLimit` 뷰를 위해 `RunResult::status()`를 사용할 수 있습니다. [Python 바인딩 가이드](python-binding.md#hitl-and-state)를 참조하십시오.

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
