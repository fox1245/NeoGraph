<!-- neograph-i18n: source=docs/concepts.md locale=ko source_sha256=39e6eb468894ddbf6852ef7d57b9b1d0a4c87e432e2fb0ef577113a9376bd72f -->
# NeoGraph 핵심 개념 — 처음부터 읽는 안내서

**Languages:** [English](concepts.md) | [한국어](concepts.ko.md) | [日本語](concepts.ja.md) | [简体中文](concepts.zh-CN.md)

예제보다 이 문서를 먼저 읽어 두면 좋습니다. 그래프를 만드는 순서대로 채널, 노드, 엣지, fan-out, 라우팅, 체크포인트, 스트리밍을 차례로 설명합니다.

LangGraph를 써 보셨다면 리듀서가 붙은 채널, `Send`, `Command`, 체크포인트가 낯익을 것입니다. NeoGraph의 [Core와 ProgramRuntime](../README.md#core-and-programruntime)은 맡은 역할이 서로 다르며, 이 문서는 Core의 그래프 실행부터 다룹니다. Python에서 provider를 호출할 때는 제거된 completion 클래스 대신 타입이 있는 [바인딩 계약](python-binding.md)을 사용합니다.

---

## 목차

(8.5절은 v0.6.0에서 추가되었으며 제목은 `Tracing — OpenTelemetry + Phoenix / Langfuse`입니다. 외부 문서의 링크가 깨지지 않도록 번호가 붙은 제목은 1-9로 유지하고, 8.5절은 스트리밍과 자주 하는 실수 사이에 있습니다.)


1. [전체 구조](#1-the-big-picture)
2. [채널과 리듀서](#2-channels--reducers)
3. [노드](#3-nodes)
4. [엣지와 조건부 라우팅](#4-edges--conditional-routing)
5. [Send — 동적 fan-out](#5-send--dynamic-fan-out)
6. [Command — 라우팅 재정의와 상태 패치](#6-command--routing-override--state-patch)
7. [체크포인트, 인터럽트, HITL](#7-checkpoints-interrupts-hitl)
8. [스트리밍 이벤트](#8-streaming-events)
9. [자주 하는 실수](#9-common-pitfalls)

---

<a id="1-the-big-picture"></a>
## 1. 전체 구조

NeoGraph의 **그래프**는 네 가지 요소로 이루어집니다.

| 요소 | 설명 | 정의 위치 |
|---|---|---|
| **채널** | 공유 상태에 이름을 붙인 저장 칸. 리듀서가 새 쓰기를 기존 값과 합치는 방법을 정한다. | `definition["channels"]` |
| **노드** | 상태를 읽고 쓰기를 내보내는 함수. 필요하면 `Send`와 `Command`도 내보낸다. | `definition["nodes"]` |
| **엣지** | 다음에 실행할 노드를 고정해서 가리킨다. | `definition["edges"]` |
| **조건부 엣지** | 조건 함수로 라우팅한다. 상태를 보고 다음 노드 후보 중 하나를 고른다. | `definition["conditional_edges"]` |

실행은 **슈퍼스텝 루프**로 진행됩니다.

```
1. ready_set = nodes routed from __start__
2. while ready_set is not empty:
   a. run the ready batch against its pre-update channel state
   b. buffer returned writes, then fold them through channel reducers
   c. execute emitted Sends after ordinary writes; fold their results
   d. combine routing signals and evaluate updated state → new ready_set
```

일반적인 ready batch(같은 슈퍼스텝에서 함께 실행할 준비가 된 노드 묶음)는 그 batch가 만든 변경이 반영되기 전의 채널 상태를 읽습니다. 같은 batch의 다른 노드가 반환한 쓰기는 실행 중에 읽을 수 없습니다. 엔진은 그 쓰기를 버퍼에 모아 두었다가 batch가 끝난 뒤 리듀서로 합치고, 갱신된 상태로 라우팅을 판단합니다. 이것은 그래프 수준의 스케줄링이며, 모델 내부의 계산까지 동기화하지는 않습니다.

예를 들어 `counter`의 초기값이 0이고 실행 대기 중인 두 노드가 모두 `counter + 1`을 반환하면, 두 노드는 둘 다 0을 읽습니다. overwrite 리듀서를 쓰면 결과는 2가 아니라 1입니다. 반면 사용자 정의 sum 리듀서를 쓰면 증가분 1짜리 쓰기 두 개를 합쳐 2를 만들 수 있습니다. `Send`는 하나든 여러 개든 각자 자기 payload를 적용한 격리된 상태 복사본에서 실행되므로, payload가 공유 상태에 닿는 일은 없습니다(#389). 리듀서 적용 순서가 정해져 있다고 해서 모델 응답이나 외부 효과까지 재현되는 것은 아닙니다.

---

<a id="2-channels--reducers"></a>
## 2. 채널과 리듀서

모든 상태는 이름이 붙은 채널에 저장됩니다. 채널은 기본적으로 슈퍼스텝과 체크포인트를 넘어서도 값을 유지하며, 노드는 채널에 값을 써서 서로 통신합니다. 값을 합치는 방식, 배열 보존 정책, 체크포인트 포함 여부는 각각 독립적으로 정합니다.

### 채널 정의

```python
"channels": {
    "messages":  {"reducer": "append"},     # conversation history
    "counter":   {"reducer": "overwrite"},  # latest value wins
    "summary":   {"reducer": "overwrite"},
}
```

### 내장 리듀서

| 리듀서 | 새 쓰기의 동작 | 주로 쓰는 곳 |
|---|---|---|
| `"overwrite"` | 새 값이 이전 값을 대체한다. 병렬로 쓰면 마지막에 쓴 값이 남는다. | 값 하나만 담는 임시 칸(현재 노드, 현재 질문, 라우팅 힌트). |
| `"append"` | 새 값(반드시 리스트여야 함)을 기존 리스트 뒤에 이어 붙인다. 이전 스텝의 값이 앞에 오고, 병렬 노드는 완료 순서가 아니라 스케줄러가 ready로 정한 순서로 반영하며, 한 노드 안에서는 반환한 쓰기 순서를 따른다. | 대화 메시지, 검색 결과, fan-out 결과 수집. |

> 두 리듀서는 엔진이 시작할 때 `ReducerRegistry::ReducerRegistry()`에서 등록됩니다([`src/core/graph_loader.cpp`](../src/core/graph_loader.cpp)).
> 사용자 정의 리듀서는 C++에서는 `ReducerRegistry::register_reducer(name, fn)`으로,
> Python에서는 다음과 같이(v0.1.9부터) 등록합니다.
>
> ```python
> ng.ReducerRegistry.register_reducer("sum",
>     lambda current, incoming: (current or 0) + incoming)
> ```
>
> Python 콜러블은 GIL을 잡고 실행되므로, 동시에 실행되는 Send fan-out도
> Python 사용자 정의 노드와 마찬가지로 이 콜러블 앞에서 직렬화됩니다.
> 같은 이름으로 다시 등록하면 이전 리듀서를 대체합니다.

### 채널 수명 주기와 체크포인트 계약

리듀서는 쓰기를 합치는 역할만 합니다. 배열을 얼마나 남길지는 별도의 보존 정책(retention)으로 정하며, `unbounded`(기본값), `latest`, 양수 `retention_limit`을 지정하는 `bounded` 중에서 고릅니다. 보존 정책은 `ChannelWrite.Mode.Overwrite`를 포함한 모든 쓰기 직후에 배열을 잘라 냅니다. `latest`는 다른 쓰기가 값을 대체하기 전까지 마지막 요소를 유지합니다. 저장 방식(persistence)도 따로 정합니다. `checkpoint`(기본값)는 값 전체와 버전을 저장하고, `ephemeral`은 둘 다 영구 저장되는 체크포인트에서 뺍니다. bounded 보존은 저장 크기만 줄이는 것이 아니라 읽을 수 있는 기록 자체를 바꿉니다.

엔진은 한 노드의 쓰기는 반환된 순서대로, 정적 batch는 스케줄러가 ready로 정한 순서대로, 여러 `Send`의 결과는 호출 순서대로 합치며, 어느 경우에도 끝난 순서는 따지지 않습니다. 대기 중인 쓰기(pending writes)는 같은 작업 슬롯으로 재생(replay)됩니다. overwrite는 정해진 순서에서 마지막에 쓴 값이 남고(last-writer-wins), append는 요소 순서를 유지합니다. 사용자 정의 리듀서는 순수 함수로 작성하고, 재생(replay)해도 결과가 달라지지 않아야 합니다. 묶는 단위를 바꿔도 결과가 같아야 한다면 결합법칙이 필요하고, 교환법칙은 순서와 무관한 결과가 필요할 때만 필요합니다. 명시적 overwrite는 리듀서를 거치지 않고, 그 뒤에 보존 정책만 적용됩니다. 이 규칙들이 모델 출력이나 외부 효과의 재현을 보장하는 것은 아닙니다.

ephemeral 값은 슈퍼스텝이 바뀌어도 그대로 유지되며 스텝마다 초기화되지 않습니다. 체크포인트에는 선언된 ephemeral 채널의 이름과 쓰기 여부만 기록하고 값은 기록하지 않습니다. 재개(resume), `resume_if_exists`, ID를 정확히 지정한 재개, 상태 갱신은 다음 경우에 거부됩니다. 이미 쓰인 ephemeral 상태가 있거나, 오래된 체크포인트에 가드가 없거나, ephemeral 채널 집합이 바뀐 경우입니다. ephemeral 채널에 처음 쓰기 전에 만든 체크포인트는, 대기 중인 쓰기(pending writes)를 문서에 정한 순서대로 재생(replay)하면서 재개할 수 있습니다. `update_state`는 ephemeral 채널에 대한 쓰기를 거부합니다. 같은 프로세스 안에서 여러 `Send`를 처리하는 워커는 격리된 복사본에 메모리에 있는 ephemeral 값을 그대로 물려받습니다. 정확성에 꼭 필요한 상태는 체크포인트에 저장하거나, 새로 실행하면서 영속 입력에서 다시 만들어야 합니다.

`GraphState::restore`는 ephemeral 채널이 있는 그래프를 거부합니다. 대신 그에 맞는 가드와 함께 `restore_checkpoint`를 쓰거나, 실행 중인 모든 값과 버전이 들어 있는 같은 프로세스의 스냅샷에는 `restore_runtime`을 씁니다. 가드는 체크포인트 메타데이터를 이용하므로 채널 blob 레이아웃이나 저장소 스키마는 바뀌지 않습니다. ephemeral 채널이 없는 그래프라면 이전에 만든 값 전체 방식의 체크포인트도 그대로 동작합니다. 이 가드가 없는 바이너리로 다운그레이드하기 전에는 ephemeral 스레드와 포크를 정리하거나 영속 입력에서 다시 시작해야 합니다. 이전 버전의 reader는 나중에 추가된 가드를 강제할 수 없기 때문입니다.

체크포인트를 쓰는 채널은 값 전체를 스냅샷으로 저장합니다. Memory, SQLite, PostgreSQL 백엔드는 바뀌지 않은 `(thread, channel, version)` 값을 중복 저장하지 않습니다. 하지만 append 채널은 쓸 때마다 버전이 바뀌므로 스냅샷은 계속 커집니다. 대기 중인 쓰기(pending writes)는 끝나지 않은 슈퍼스텝에서 성공한 작업을 기록한 것이며 일반적인 채널 델타가 아닙니다. 스텝마다 채널을 초기화하는 정책은 제공하지 않습니다. 안전하게 설계하려면 초기화 시점을 쓰기와 라우팅 뒤로 정하고, 인터럽트, 재생(replay), Send의 동작도 함께 정의해야 하기 때문입니다. ephemeral 저장 방식과 혼동해서는 안 됩니다.

기준 성능은 `bench_checkpoint_store --threads 1 --iters 1 --history-steps 256 --payload 512 --backends memory,sqlite`로 측정합니다. `postgres`와 `--pg-url`은 격리된 로컬 데이터베이스에서만 추가합니다. 결과의 각 행에는 논리적 직렬화 바이트, 저장·로드의 p50/p95, 복원 깊이(reconstruction depth)가 나오고, legacy 행은 blob 개수를 보고합니다. 할당 요청 수는 `heaptrack bench_checkpoint_store --threads 1 --iters 1 --history-steps 256 --payload 512 --backends memory`로 확인합니다. 네이티브 JSON/SQL 할당자 중에는 C++ `operator new`로 가로채지 못하는 것이 있어서 전부 집계되지는 않습니다. 같은 payload, 기록 길이, 백엔드 설정으로 비교하고, 절감량은 추정이 아니라 측정값으로 판단해야 합니다. 논리적 바이트는 실제로 디스크에 저장되는 물리적 바이트와 다릅니다. SQLite는 종료할 때 지워지는 고유한 임시 데이터베이스를 쓰며, `--sqlite-path`를 지정하면 새로 만든 파일을 남기고 이미 있는 경로는 거부합니다.

아래는 Linux x86-64 Debug 빌드에서 스레드 1개, 기록 스텝 256개, 메시지 512바이트, 반복 1회로 기록한 기준값입니다. 과거에 측정한 값이며 성능 목표가 아닙니다.

| 백엔드 | 논리적 체크포인트 크기(바이트) | 저장 p50/p95 (µs) | 로드 p50/p95 (µs) | 재생(replay) 깊이 |
| --- | ---: | ---: | ---: | ---: |
| Memory | 17,814,952 | 54 / 138 | 141 / 382 | 1 |
| SQLite | 17,814,952 | 289 / 1,589 | 176 / 474 | 1 |

따로 한 번 더 측정했을 때 SQLite 데이터베이스와 WAL의 크기는 14,811,136 / 4,210,672바이트(합계 19,021,808바이트)였습니다. Linux `LD_PRELOAD` shim으로 프로세스 전체의 `malloc`, `calloc`, 0이 아닌 `realloc` 요청을 센 결과도 있습니다. 생성과 JSON 파싱까지 포함한 수치이며, `--history-steps 0`일 때와 비교하면 Memory는 요청 88,277건 / 요청 바이트 605,289,027, SQLite는 114,295건 / 867,319,964바이트가 추가로 발생했습니다. 이 값은 누적 요청량이지 실제로 점유한 메모리나 저장소만의 할당량이 아닙니다. 정렬 할당과 내부 할당은 가로채지 못했습니다. 결론을 내리기 전에는 지원되는 프로파일러로 워밍업을 마친 실행을 여러 번 확인해야 합니다.

### 채널에 쓰기

노드는 `ChannelWrite` 목록을 반환합니다.

```python
return [
    ng.ChannelWrite("messages", [{"role": "assistant", "content": "Hi!"}]),
    ng.ChannelWrite("counter",  (state.get("counter") or 0) + 1),
]
```

값의 형태는 리듀서와 맞아야 합니다.
- `"append"` → 리스트여야 하며, 기존 리스트에 이어 붙습니다.
- `"overwrite"` → JSON으로 직렬화할 수 있는 값이면 무엇이든 됩니다.

### 노드에서 상태 읽기

```python
def run(self, input):
    msgs    = input.state.get("messages") or []  # list of message dicts
    counter = input.state.get("counter") or 0
    ...
```

`state.get(channel)`은 채널의 현재 값을 반환합니다. 채널은 있지만 아직 쓰인 적이 없으면 `None`을 반환합니다. 채팅 메시지를 타입이 있는 객체로 읽으려면 `state.get_messages()`를 씁니다. `messages` 채널을 파싱해 `list[ChatMessage]`를 반환하며, `llm_call`이 내부에서 사용합니다.

### 버전

각 채널은 단조 증가하는 `version` 번호를 가집니다. 체크포인트 간 차이를 비교할 때 엔진이 이 번호를 사용하며, `state.channel_version(name)` 조회 API로도 읽을 수 있습니다. 보통은 직접 읽을 일이 없습니다.

---

<a id="3-nodes"></a>
## 3. 노드

노드 타입을 등록하는 방법은 세 가지이며, 뒤로 갈수록 제어할 수 있는 범위가 넓어집니다.

### 3.1 내장 노드

| `type`(JSON) | 동작 | 설정 |
|---|---|---|
| `llm_call` | 소유권이 있는 타입 요청 하나를 준비하고, 게이트 검사·예약·영수증 기록을 거쳐 같은 핸들로 디스패치한다. 순서가 있는 모든 메시지와 전체 결과를 보관한다. | `NodeContext`에서 `provider`, `model`, `instructions`, `tools`를 읽는다. |
| `tool_dispatch` | 가장 최근 assistant 메시지의 `tool_calls`를 확인하고, 각 호출을 `Tool::execute`로 실행한 뒤 `{role: "tool", tool_call_id, content}` 결과를 추가한다. | `NodeContext`에서 `tools`를 읽는다. |
| `intent_classifier` | LLM이 사용자 의도를 N개의 레이블 중 하나로 분류하고, 고른 레이블을 `__route__`에 쓴다. `route_channel` 조건과 함께 쓴다. | `extra_config: {labels, prompt_template}` |
| `subgraph` | 다른 그래프를 노드 하나로 끼워 넣는다. 내부 상태는 설정한 키 재매핑으로 바깥 상태와 대응시킨다. | `extra_config: {graph_def, input_keys, output_keys}` |

### 3.2 `@ng.node` 데코레이터 (Python 전용)

쓰기만 하는 노드를 가장 짧게 정의하는 방법입니다.

```python
@ng.node("greet")
def greet_node(state):
    name = state.get("name") or "world"
    return [ng.ChannelWrite("messages",
        [{"role": "assistant", "content": f"Hello, {name}!"}])]
```

데코레이터를 붙인 함수는 `list[ChannelWrite]`를 반환해야 합니다(`None`을 반환하면 `[]`로 처리합니다). `Send`나 `Command`는 내보낼 수 없으므로, 필요하면 `GraphNode`를 상속해야 합니다.

### 3.3 `GraphNode` 서브클래스 (전체 기능)

`run(input)`을 오버라이드하면 노드를 완전히 제어할 수 있습니다. v0.4.0에서 도입되었고 v0.9.0부터는 사용자 정의 노드의 유일한 진입점입니다. 메서드도 시그니처도 하나뿐입니다.

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

Python에서는 `input.ctx`로 `cancel_token`, `usage`, `thread_id`, `step`, `stream_mode`, `store`, `resume_value`, `trace_id`, `run_id`, `model_token_budget`과 타입이 있는 provider 증거를 볼 수 있습니다. 데드라인은 `has_deadline`과 `deadline_remaining_ms`로 확인합니다. C++ steady clock의 원시 값은 불투명(opaque)합니다. C++ 호출자는 `RunMetadata`로 데드라인과 트레이스 메타데이터를 전달하며, 이 값은 중첩된 서브그래프까지 전파됩니다.

`Send`나 `Command`가 필요 없으면 `list[ChannelWrite]`만 반환해도 됩니다. 바인딩이 이를 자동으로 `NodeResult`로 감쌉니다.

> **v0.3.x에서 옮기는 경우:** v0.4 이전의 다중 진입점 노드 API는 제거되었고 대체 방법은 하나뿐입니다.
> `run(input)`을 오버라이드해야 합니다. 상태는 `input.state`에서 읽고, 토큰은 `input.stream_cb`가 None이 아닐 때 이를 통해 내보내며,
> 취소 토큰은 `input.ctx.cancel_token`에서 읽습니다.

JSON 로더가 인스턴스를 만들 수 있도록 타입을 등록합니다.

```python
ng.NodeFactory.register_type(
    "researcher",
    lambda name, config, ctx: Researcher(name),
)
```

팩토리는 `(name, per-node config, NodeContext)`를 받으므로, 같은 클래스를 서로 다른 이름과 설정으로 여러 번 인스턴스화할 수 있습니다.

### 3.4 도구 (별도 개념, `tool_dispatch`가 사용)

`Tool`은 노드가 아니라 `tool_dispatch`가 호출하는 대상입니다. `ng.Tool`을 상속해 메서드 세 개를 오버라이드하고, 인스턴스를 `NodeContext(tools=[…])`에 넘깁니다.

```python
class CalcTool(ng.Tool):
    def get_name(self):       return "calc"
    def get_definition(self): return ng.ChatTool("calc", "Double x", {"type": "object", "properties": {"x": {"type": "number"}}, "required": ["x"]})
    def execute(self, args):  return str(args["x"] * 2)
```

엔진은 컴파일할 때 도구 목록의 소유권을 가져가므로, 이후에는 로컬 참조를 놓아도 됩니다.

---

<a id="4-edges--conditional-routing"></a>
## 4. 엣지와 조건부 라우팅

### 정적 엣지

```python
"edges": [
    {"from": ng.START_NODE, "to": "llm"},
    {"from": "dispatch",    "to": "llm"},
    {"from": "summarizer",  "to": ng.END_NODE},
]
```

같은 출발 노드에서 엣지가 여러 개 나가면 fan-out이 됩니다. 모든 후속 노드가 다음 슈퍼스텝의 실행 대기 집합(ready set)에 들어갑니다. 한 슈퍼스텝에서 같은 목표 노드로 가는 엣지가 둘 이상이어도 중복이 제거되어 그 노드는 한 번만 실행됩니다.

### 조건부 엣지

조건부 엣지는 **이름이 붙은 조건**을 실행하고, 그 결과로 `routes` 맵에서 다음 노드를 고릅니다.

```python
"conditional_edges": [
    {
        "from": "llm",
        "condition": "has_tool_calls",
        "routes": {"true": "dispatch", "false": ng.END_NODE},
    }
]
```

조건 이름은 엔진에 등록된 `ConditionFn`으로 연결됩니다. 기본으로 두 가지가 들어 있습니다.

| 조건 | 반환값 | 사용 시점 |
|---|---|---|
| `has_tool_calls` | 가장 최근 assistant 메시지의 `tool_calls`가 비어 있지 않으면 `"true"`, 아니면 `"false"`. | ReAct 루프. LLM이 도구를 더 요청하지 않을 때까지 계속 도구를 실행한다. |
| `route_channel` | `__route__` 채널에 들어 있는 문자열. 없으면 `"default"`. | `intent_classifier`와 함께 써서 의도별로 명시적으로 라우팅한다. |

사용자 정의 조건은 C++에서는 `ConditionRegistry::register_condition(name, fn)`으로, Python에서는 다음과 같이(v0.1.9부터) 등록합니다.

```python
def is_long(state):
    msgs = state.get("messages") or []
    return "long" if len(msgs) > 10 else "short"

ng.ConditionRegistry.register_condition("is_long", is_long)
```

콜러블은 실행 중인 `GraphState`를 받으므로 `state.get(channel)`과 `state.get_messages()`를 쓸 수 있습니다. 반환값은 조건부 엣지의 `routes` 키 중 하나와 일치하는 문자열이어야 합니다.

### 동등한 두 가지 형식 — v0.1.8부터 둘 다 동작합니다

조건부 엣지는 `edges` 배열 안에(`condition` 필드와 함께) 두어도 되고, 별도의 `conditional_edges` 블록에 두어도 됩니다. 두 형식 모두 허용되므로 읽기 쉬운 쪽을 고르면 됩니다.

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

> **이력:** v0.1.8 이전에는 그래프 컴파일러가 형식 A를 아무 경고 없이 버렸습니다.
> README와 모든 Python 예제가 형식 A를 썼기 때문에 ReAct 루프가 LLM 호출 한 번으로 끝나 버렸습니다.
> 커밋 `e23a523`에서 고쳤습니다. wheel 0.1.7 이하에서 이 증상이 보이면 업그레이드하세요.

---

<a id="5-send--dynamic-fan-out"></a>
## 5. Send — 동적 fan-out

`Send`를 쓰면 노드가 실행 시점에 정해지는 개수만큼 대상 노드를 호출할 수 있습니다. 예를 들어 주제마다 researcher를 하나씩 띄우는 식입니다. 엔진은 일반 ready batch가 끝나고 그 쓰기가 반영된 뒤, 같은 번호의 슈퍼스텝 안에서 내보낸 Send를 실행합니다.

```python
class Planner(ng.GraphNode):
    def run(self, input):
        topics = decide_topics(input.state)            # e.g. 5 strings
        return ng.NodeResult(
            writes=[],
            sends=[ng.Send("researcher", {"topic": t}) for t in topics],
        )
```

### 동작 원리

엔진은 `Send` 하나마다 컴파일된 대상 노드를 한 번씩 호출하지만, 매번 새 노드 객체를 만든다고 보장하지는 않습니다. 그러므로 Send 대상 노드는 동시에 호출되어도 안전하게 멤버 상태를 다뤄야 합니다. payload는 대상 노드가 채널을 읽기 전에 적용됩니다. 그 스텝에서 Send를 하나만 보냈든 여러 개를 보냈든, 모든 Send는 ready batch가 끝난 시점의 상태를 격리해 복사한 뒤 자기 payload를 적용해서 실행합니다. 그래서 payload는 그 Send 자신의 실행(재시도 포함)에서만 보이고, 다음 스텝이나 최종 상태에는 나타나지 않습니다(#389). Send가 모두 끝나면 반환된 쓰기를 호출 순서대로 합칩니다. 그다음 일반 노드와 Send 대상 노드의 신호를 합쳐 다음 ready batch의 라우팅을 정합니다.

Send 대상 노드가 `NodeInterrupt`를 발생시키면 ready batch의 노드와 마찬가지로 실행이 일시 중지됩니다. 이때 만들어지는 `NodeInterrupt` 체크포인트는 슈퍼스텝 전체로 다시 들어가는 지점입니다. 체크포인트에는 그 스텝이 시작될 때의 상태가 담기고, 그 스텝의 ready 노드가 다음 노드로 기록되며, 일시 중지된 결과도 이 상태를 보고합니다. 재개하면 그 스텝을 다시 실행합니다. 이미 끝난 ready 노드와 Send는 기록해 둔 결과를 재생(replay)할 뿐 다시 실행하거나 과금하지 않습니다. 일시 중지된 Send와 끝나지 않은 다른 Send는 같은 task id로 재개 값을 받아 다시 실행되며, 앞선 시도에서 저널에 기록된 provider 호출은 재생(replay)됩니다. 그 스텝 앞에 체크포인트가 없는 경우(예를 들어 진입점의 map 노드)에는 끝난 결과를 인터럽트 체크포인트 자체에 기록합니다.

### 흔한 구조: 5갈래 fan-out 뒤 summarizer로 fan-in

```
planner ─┬─ Send("researcher", {topic: "A"})  ─┐
         ├─ Send("researcher", {topic: "B"})  ─┤
         ├─ Send("researcher", {topic: "C"})  ─┼─→ summarizer
         ├─ Send("researcher", {topic: "D"})  ─┤
         └─ Send("researcher", {topic: "E"})  ─┘
```

`researcher`에서 나가는 엣지는 `{"from": "researcher", "to": "summarizer"}` 하나면 충분합니다. 정적 엣지와 같은 중복 제거 규칙이 적용되므로 summarizer는 한 번만 실행됩니다.

### 워커 수 조정

`build()`의 기본값은 `EngineConfig::worker_count == 1`입니다. 엔진이 소유하는 스레드 풀을 만들지 않고, 각 분기를 호출자의 코루틴 executor에서 디스패치합니다. 코루틴 I/O는 서로 겹쳐 실행될 수 있지만, 스레드가 하나뿐인 executor에서는 CPU 위주의 작업이 직렬로 실행될 수 있습니다. 호출자의 executor가 멀티스레드이거나 실행을 동시에 여러 개 돌릴 때도 노드의 멤버 상태는 여전히 안전하게 다뤄야 합니다.

실제 병렬 실행이 필요하면 스레드 풀을 명시적으로 켜야 합니다. fan-out 폭에 맞춰 N을 직접 지정하거나, `hardware_concurrency()` 값을 쓰는 `set_worker_count_auto()`를 사용합니다. 이 값을 얻지 못하면 4를 씁니다.

```python
engine.set_worker_count(5)           # match a 5-way Send
# or
engine.set_worker_count_auto()       # hardware_concurrency()
```

Send를 여러 개 보내거나 나가는 엣지가 여러 개인 fan-out이 스레드 풀을 켜지 않은 채 실행되면, NeoGraph는 stderr에 경고를 한 번 출력합니다. 아무 표시 없이 직렬로 실행되는 상황을 놓치지 않게 하기 위해서입니다. 직렬 fan-out을 일부러 쓰는 경우(예: worker=1 빠른 경로의 벤치마크)에는 `NEOGRAPH_SUPPRESS_FANOUT_WARNING=1`로 경고를 끌 수 있습니다.

---

<a id="6-command--routing-override--state-patch"></a>
## 6. Command — 라우팅 재정의와 상태 패치

`Command`를 쓰면 노드가 반환값 하나로 다음에 갈 곳을 정하면서 상태도 바꿀 수 있습니다. 나가는 일반 엣지는 거치지 않습니다.

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

### Command와 조건부 엣지 중 무엇을 쓸까

- **조건부 엣지**: 노드 로직 없이 상태에 대한 조건만으로 라우팅이 정해질 때 씁니다. 선언적이라 더 깔끔합니다.
- **Command**: 노드 안에서 작성하는 편이 자연스러운 로직으로 라우팅을 정할 때 씁니다. 예를 들어 여러 기준을 합친 점수 계산, 내용 검사, 재시도 판단이 있습니다. 상태 갱신과 다음 노드 선택을 원자적으로 함께 할 수 있는 유일한 방법이기도 합니다.

### fan-in에서는 마지막 Command가 이깁니다 (last-writer-wins)

같은 batch의 여러 노드가 비어 있지 않은 `Command.goto_node`를 반환하면, 라우팅 순서에서 마지막 command가 이기며 일반 엣지와 barrier보다 우선합니다. 정적 batch의 순서는 ready 순서이고, 여러 `Send`의 순서는 완료 순서가 아니라 호출 순서입니다. command가 반환한 업데이트는 모두 그대로 쓰기 파이프라인을 거쳐 합쳐집니다. 서로 충돌하는 command가 워크플로를 바꿀 수 있다면 라우팅을 결정하는 노드를 하나로 두는 편이 좋습니다.

---

<a id="7-checkpoints-interrupts-hitl"></a>
## 7. 체크포인트, 인터럽트, HITL

### 체크포인트 저장소 설정

```python
engine.set_checkpoint_store(ng.InMemoryCheckpointStore())
# or: engine.set_checkpoint_store(ng.PostgresCheckpointStore(...))   # if built with PG
```

저장소를 연결하면 슈퍼스텝마다 `(thread_id, checkpoint_id)`를 키로 체크포인트를 저장소에 기록합니다. `RunResult.checkpoint_id`는 그중 가장 최근 것입니다.

### 정적 인터럽트 지점

```python
"interrupt_before": ["payment"],   # pause before this node runs
"interrupt_after":  ["llm"],       # pause after, before routing
```

엔진은 `interrupted=True`이고 `interrupt_node`가 채워진 `RunResult`를 반환합니다. 재개하려면 다음과 같이 합니다.

```python
result = await engine.resume_async(thread_id="t1",
                                   checkpoint_id=result.checkpoint_id,
                                   new_input={...})  # optional
```

재개는 자신이 응답하는 인터럽트의 게이트만 건너뜁니다. `interrupt_before`로 멈춘 실행을 재개하면 그때 멈춰 있던 노드를 실행하고, `NodeInterrupt`로 멈춘 실행을 재개하면 그 스텝을 이어서 진행합니다. 그 밖의 재개는 모두, 곧 실행하려는 `interrupt_before` 노드 앞에서 다시 멈춥니다. `interrupt_after`로 멈춘 뒤의 재개나, 프로세스가 중단된 뒤 스텝의 체크포인트에서 복구하는 경우가 여기에 해당합니다. 재개 값은 `interrupt_before`로 멈춰 있던 노드에만 전달되고 다른 노드에는 전달되지 않습니다. `interrupt_after` 뒤의 재개나 다른 체크포인트에서의 재개는 어떤 노드에도 값을 전달하지 않습니다(#393). 한 스텝에서 ready 상태인 게이트 노드가 여럿이면 하나씩 응답합니다(#396). 재개는 실행이 멈춘 게이트에만 응답하고, 스텝이 실행되기 전에 같은 스텝의 나머지 게이트 노드 중 아직 응답받지 못한 노드는 각자의 `interrupt_before` 중지로 실행을 다시 멈춥니다. 스텝의 게이트가 모두 응답받으면 스텝이 실행되며, 게이트 노드마다 자기 게이트의 응답을 받습니다.

### `NodeInterrupt`로 하는 동적 인터럽트

노드 본문 안에서 예외를 던집니다(Python은 `raise ng.NodeInterrupt(reason)`, C++는 `throw NodeInterrupt(...)`). 엔진이 이를 잡아 상태를 저장하고, 예외를 던진 노드에서 중단된 `RunResult`를 반환합니다. 재개 API는 같습니다. ready batch의 노드와 `Send` 대상 노드 모두 이렇게 동작하며, 동기·비동기·스트리밍 진입점에서도 마찬가지입니다. 중단된 스텝에서 이미 끝난 형제 노드는 재개할 때 다시 실행하지 않고 재생(replay)합니다. 그 스텝 앞에 체크포인트가 없어도 같습니다.

한 스텝에서 발생한 여러 인터럽트는 한 번에 하나씩 응답합니다(#388). 한 슈퍼스텝의 노드나 Send 여럿이 `NodeInterrupt`를 발생시키면 실행은 그 모두에서 일시 중지됩니다. `RunResult.interrupts`에는 각 인터럽트가 `{id, node, value}` 형태로 나열됩니다. id는 일시 중지된 호출의 task id이며 `s3:review`, Send라면 `s3:send[1]:worker:<hash>` 같은 형태입니다. `interrupt_node`, `interrupt_value`, `interrupt_id`는 그중 첫 번째 인터럽트를 가리킵니다. 재개하면 `interrupt_id=`로 지정한 인터럽트(C++에서는 `RunMetadata::resume_interrupt_id`)에 응답하고, 지정하지 않으면 첫 번째에 응답합니다. 그 스텝에서 멈춘 나머지 노드와 Send는 값 없이 다시 실행되어 각자 같은 질문을 다시 던지고, 그다음 재개가 그 질문에 응답합니다. 대기 중이 아닌 인터럽트를 지정하면 아무것도 실행하기 전에 `ValueError`(C++는 `std::invalid_argument`)가 발생합니다. 인터럽트 여러 개로 멈춘 체크포인트는 메타데이터(`_neograph.interrupts`)에 그 목록을 담고, 하나로 멈춘 체크포인트는 달라지지 않습니다. 재개 값은 멈춘 스텝에서 응답받은 호출에만 전달됩니다(#393). 재개된 실행의 이후 스텝에 있는 노드는 `ctx.resume_value`를 받지 못하므로, 거기서 질문하는 노드나 도구 게이트는 다른 질문에 대한 응답을 가져다 쓰지 않고 실행을 다시 멈춥니다. 부모를 통해 재개한 서브그래프는 부모에서 응답받은 노드를 통해 응답을 받습니다. `messages` 채널이 있는 그래프에서는 그 값을 사용자 메시지로도 여전히 받습니다. JavaScript의 join과 plan 수준의 `parallel`, `quorum`도 같은 규칙을 따릅니다. 재개할 때마다 멈춘 멤버 하나가 응답을 받고 나머지는 다시 질문합니다.

형제 노드의 결과가 서로 다르게 끝나는 스텝은 인덱스나 완료 순서와 상관없이 하나의 규칙으로 끝납니다(#392). 이 규칙은 fan-out의 분기와 한 스텝의 Send 모두에 적용되며, 워커가 하나든 여럿이든 같습니다. 우선 실행이 취소되면 스텝이 중단됩니다. 취소가 아니라면 불확실한 효과, 즉 payload의 `kind`가 `tool_effect_reconciliation`인 `NodeInterrupt`가 있으면 형제 노드가 실패했더라도 조정(reconciliation)을 위해 스텝을 일시 중지합니다. 실행을 끝내 버리면 결과를 아무도 모르는 효과를 방치하게 되기 때문입니다. 이때 일시 중지 결과에는 그런 인터럽트만 보고하며, 결과를 기록하지 못한 실패 노드는 스텝을 재개할 때 다시 실행됩니다. 불확실한 효과가 없고 실패가 있으면 디스패치 순서(ready 순서, 그다음 Send 순서)에서 가장 먼저 발생한 실패로 실행이 실패합니다. 이때 그 스텝의 일반 인터럽트는 보고되지 않고 체크포인트도 저장되지 않습니다. 실패도 없으면 위에서 설명한 대로 스텝이 모든 인터럽트에서 일시 중지됩니다. 실패 엣지를 따라 라우팅되는 실패는 완료와 마찬가지로 해당 노드의 결과입니다. 이 경우에도 형제 노드의 인터럽트는 스텝을 일시 중지시키고, 라우팅은 스텝이 완료될 때 적용됩니다. JavaScript의 join과 plan 수준의 `parallel`, `quorum`도 멤버의 순위를 같은 방식으로 정합니다(#397). 불확실한 효과로 멈춘 멤버가 있으면 실패한 멤버가 있어도 join이 그 멤버(여럿이면 첫 번째)에서 일시 중지됩니다. 다만 race의 승자가 나왔거나 quorum이 충족되는 등 성공이 이미 결과를 정했다면 그렇지 않습니다. 실패한 멤버는 기록해 두었다가 재개할 때 join을 끝냅니다. 재개에서 응답받은 멤버가 끝까지 실행되어 그 효과가 확정된 다음입니다(#400). join이 일시 중지될 때는 선언 순서와 상관없이, 불확실한 효과로 멈춘 멤버에게 일반 질문으로 멈춘 멤버보다 먼저 응답합니다(#401). 결과를 모으는 `ng.all`(`collect: true`)은 위 규칙의 의도된 예외입니다. 이 노드는 일시 중지된 멤버를 포함한 모든 멤버를 기다린 뒤에 결과를 보고합니다. 그래서 다른 멤버가 일반 질문으로 멈춰 있는 동안에는 한 멤버의 실패로 끝나지 않고 그 자리에서 일시 중지되며, 모든 멤버가 정리되고 나서 실패를 보고합니다(#401).

일시 중지할지 여부가 노드의 중간 출력에 달려 있을 때 유용합니다(예: "LLM이 사람에게 보여 줄 만한 결과를 냈는가?").

### 시간 여행

`engine.fork(source_thread_id, new_thread_id, checkpoint_id="")`는 체크포인트 하나를 호출자가 지정한 대상 스레드로 복사하고 새 체크포인트 ID를 반환합니다. 체크포인트 ID를 생략하면 원본 스레드의 가장 최근 체크포인트를 선택합니다. 복사본은 남아 있던 후속 실행 계획(pending continuation)을 그대로 가집니다. 상태를 편집한다고 해서 새 작업이 예약되지는 않습니다. `NodeInterrupt` 체크포인트를 fork하면 중단된 슈퍼스텝의 대기 중인 쓰기(pending writes)도 함께 복사됩니다. 그래서 fork를 재개하면 원본을 재개할 때처럼, 그 스텝에서 이미 끝난 노드와 Send를 다시 실행하거나 provider를 다시 호출하지 않고 재생(replay)합니다(#390). 원본은 자기 쓰기를 그대로 보존합니다.

`next_nodes == ["__end__"]`인, 이미 완료된 continuation을 재개하면 노드를 실행하지 않고 저장된 결과를 복원합니다. 중단된 작업을 편집한 상태로 이어 가려면 `get_state_history()`에서 아직 실행 대기 노드가 남아 있는 이전 체크포인트를 정확히 골라 그 ID를 fork하고, fork를 편집한 뒤 재개합니다. 과거 체크포인트의 `next_nodes`가 빈 벡터인 경우는 다릅니다. ID 없이 최신 상태로 재개하면 새로 시작하는 실행과 같게 동작하고, ID를 정확히 지정해 재개하면 그 스냅샷에 고정됩니다.

[예제 08](../examples/08_state_management.cpp)은 새 턴을 시작하는 흐름을 그대로 씁니다. 완료된 체크포인트를 fork하고 사용자 메시지를 편집한 뒤 `resume_if_exists=true`로 `run()`을 호출하며, 그 새 실행이 인터럽트될 때만 재개합니다. 일시 중지된 fork를 재개하는 예는 아닙니다.

체크포인트는 provider의 네이티브 기록(추론 내용, 서명, 순서가 있는 도구 호출)을 그것을 만든 프로세스 안에서만 C++ 사이드카로 보관합니다. `ChatMessage`, `ChatTool`, JSON은 이식 가능한 사본이며 네이티브 권한이 없습니다. 재시작 뒤에도 네이티브 기록을 유지해야 하는 호스트는 `sp::NativeArchive`를 설정합니다. 이 계약은 [타입이 있는 provider 레퍼런스](reference-en.md#portable-projections)에 설명되어 있습니다.

---

<a id="8-streaming-events"></a>
## 8. 스트리밍 이벤트

`run_stream` / `run_stream_async`는 이벤트가 발생할 때마다 콜백을 호출합니다. 모드는 OR로 조합할 수 있는 비트마스크입니다.

| 모드 | 내보내는 이벤트 |
|---|---|
| `EVENTS` | `NODE_START`, `NODE_END`, `INTERRUPT` |
| `TOKENS` | `Provider`가 스트리밍하는 토큰마다 `LLM_TOKEN` |
| `DEBUG` | 다음 ready 집합을 보여 주는 `__routing__` 이벤트 |
| `VALUES` | 슈퍼스텝이 끝날 때마다 전체 상태를 담은 `__state__` 이벤트 |
| `UPDATES` | `ChannelWrite`마다 `CHANNEL_WRITE` 이벤트 |
| `ALL` | 위의 모든 이벤트 |

```python
def cb(event):
    print(event.type, event.node_name, event.data)

engine.run_stream(
    ng.RunConfig(thread_id="t", input={...},
                 stream_mode=ng.StreamMode.EVENTS),
    cb)
```

> **참고:** `event.node`가 아니라 `event.node_name`입니다. C++ 구조체의 필드 이름이
> `node_name`이고, pybind가 원래 이름을 그대로 유지합니다.

채팅 형태로 스트리밍하려면(LangChain 호환 메시지 dict에 증분 `content_so_far`가 들어 있는 형태) 다음 헬퍼를 사용합니다.

```python
from neograph_engine import message_stream

engine.run_stream(
    ng.RunConfig(thread_id="t", input={...},
                 stream_mode=ng.StreamMode.TOKENS),
    message_stream(lambda chunk: print(chunk["content"], end="", flush=True)))
```

### `asio::io_context.run()` 위치 (C++)

C++에서 `engine.run_stream_async()`를 구동할 때는 가장 바깥의 `asio::io_context.run()`을 애플리케이션의 메인 스레드에서 호출해야 합니다. 일반적인 프로세스 시작 경로로 초기화된, 오래 유지되는 다른 스레드에서 호출해도 됩니다. 검증된 구성은 다음과 같습니다.

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

> 명시적인 `ProviderMode`와 함께 `invoke_async` / `dispatch_async`를 호출하고, 요청마다 `io.run()`을 중첩하지 말고 이미 있는 오래 유지되는 executor에서 작업을 구동하세요. 우회책으로 가짜 토큰 하나를 만들어 내거나 completion을 다시 보내서는 안 됩니다.

---

## 8.5. 트레이싱 — OpenTelemetry + Phoenix / Langfuse

`neograph_engine.tracing.otel_tracer`와 `neograph_engine.openinference.openinference_tracer`는 그래프 이벤트를 run/노드 span으로 바꿉니다. 후자는 span에 `CHAIN` 태그를 달고 노드 payload의 투영을 기록합니다. 실행마다 그래프 콜백은 하나만 고릅니다. 모델 호출을 `LLM` span으로 기록하려면, 그래프를 컴파일하기 전에 타입이 있는 provider를 `OpenInferenceProvider(inner, tracer, *, span_name="llm.complete")`로 감쌉니다. 이 래퍼는 네이티브 C++ observer를 사용하며 상속한 `prepare`와 일회성 `dispatch` 또는 `invoke`를 그대로 씁니다. 요청을 준비하거나 포기하기만 해서는 span이 열리지 않습니다. 승인(admission)된 dispatch는 span을 열되, 소유 결과(owned outcome), 취소, 데드라인, 타입 이벤트는 바꾸지 않습니다. 트레이서에서 오류가 나도 provider의 결과나 예외는 대체되지 않습니다.

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

로컬 Phoenix 엔드포인트를 쓰려면 `docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix:latest`를 실행하고 `opentelemetry-api opentelemetry-sdk opentelemetry-exporter-otlp`를 설치합니다. `trace_graph`에는 그래프 사양, 기존 provider, 명시적인 모델, `RunConfig`를 넘깁니다. `ParentContextTracer`는 run의 루트를 워커 dispatch로 명시적으로 전달합니다. 스레드를 넘나드는 자동 전파나 노드별 부모 자동 전파는 약속하지 않습니다. Python은 dispatch 시점에 활성화된 OTel 컨텍스트를 사용하며, 준비된 연산은 수명이 끝날 때까지 트레이서 어댑터를 유지합니다.

LLM span에는 공개된 role/텍스트 투영, 선언된 스칼라 값, 알려진 사용량 카운트가 들어갑니다. 알려진 0은 기록하고, 알 수 없는 값은 생략합니다. 네이티브 재생(replay)/추론, 원시 wire 봉투/이벤트, 인코딩된 요청 본문은 트레이스에 넣지 않으며, 요청/결과 안에서 원래의 보관 상태를 유지합니다. 실패했을 때의 부분 보고서를 포함해 사용량 속성은 벤더의 청구액이나 예산 권한을 증명하지 않습니다. 청구/예약 회계는 `UsageAccumulator.authority_snapshot()`과 Program의 `provider_budget_authority`가 맡습니다.

공개 텍스트, 예외 메시지, 그래프 payload에도 애플리케이션의 비밀 값이 들어 있을 수 있습니다. exporter가 받을 내용을 직접 고르거나 가려야 합니다. [OpenTelemetry의 민감 데이터 처리 지침](https://opentelemetry.io/docs/security/handling-sensitive-data/)을 참고하세요. `CHAIN`과 `LLM`은 [OpenInference 규약](https://github.com/Arize-ai/openinference/blob/main/spec/semantic_conventions.md)에서 정의합니다. NeoGraph가 쓰는 속성의 부분집합, 토큰 이벤트, Python 타입 호출 예시, C++ 수명 요구 사항은 [레퍼런스](reference-en.md#105-observability--opentelemetry--openinference)에 있습니다.

---

<a id="9-common-pitfalls"></a>
## 9. 자주 하는 실수

아래는 모두 실제 사용자가 겪은 문제이며 [`docs/troubleshooting.md`](troubleshooting.md)에서도 참조합니다.

### "ReAct 루프가 한 번만 실행됩니다"

wheel 0.1.7 이하를 쓰고 있을 때 생기는 문제입니다. 그래프 컴파일러가 `conditional_edges` 블록을 아무 경고 없이 버렸습니다. 0.1.8 이상으로 업그레이드하세요. `result.execution_trace == ['llm', 'dispatch', 'llm']`인지 확인합니다(`['llm']`만 나오면 안 됩니다).

### "provider 호출이 60초 동안 멈췄다가 오류가 납니다"

wheel 0.1.6 이하를 쓰고 있을 때 생기는 문제입니다. 번들된 OpenSSL에 RHEL의 CA 경로가 하드코딩되어 있는데, Ubuntu / Debian / macOS에는 그 경로가 없습니다. 0.1.7 이상으로 업그레이드하면 import할 때 `SSL_CERT_FILE`이 certifi 번들로 자동 설정됩니다. 업그레이드하지 않는다면 `SSL_CERT_FILE`을 직접 설정해야 합니다.

### "fan-out이 예상보다 느립니다"

`compile()`의 기본값은 `set_worker_count(1)`입니다. 엔진이 소유한 스레드 풀이 없어서 fan-out 분기가 호출자의 executor에서 직렬로 실행됩니다. 실제 병렬 실행이 필요하면 `engine.set_worker_count(N)`을 호출합니다. N은 Send fan-out 폭에 맞춥니다. `hardware_concurrency()` 값을 쓰려면 `engine.set_worker_count_auto()`를 호출합니다. 스레드 풀을 켜지 않은 채 Send를 여러 개 보내는 fan-out이 처음 실행될 때 NeoGraph는 stderr에 경고도 한 번 출력합니다. 이는 오류가 아니라 힌트입니다. Python 사용자 정의 노드는 작은 fan-out에서도 GIL 경합이 생기므로, 1과 N으로 모두 벤치마크해 보는 것이 좋습니다.

### Python RunResult의 status와 상태 읽기

`result.status`는 타입이 있는 상태값 `Completed`, `Interrupted`, `StepLimit`, `SafePoint`, `BarrierStalled`, `FailureExit` 중 하나를 제공합니다. `BarrierStalled`는 barrier가 더 이상 발동할 수 없어서 실행이 `__end__`에 도달하지 못하고 멈췄다는 뜻이고, `FailureExit`는 실패 종료 노드가 완료되어 실행이 실패로 끝났다는 뜻입니다. `result.output`은 이식 가능한 최종 상태이고, `result.interrupted`, `result.max_steps_exhausted`, `result.execution_trace`는 실행 과정을 설명합니다. `result.native_messages`와 `result.provider_outcomes`에는 타입이 있는 provider 증거 전체가 남아 있습니다. [Python 바인딩 가이드](python-binding.md#hitl-and-state)를 참고하세요.

### "Unknown reducer: <name>"

기본으로 제공하는 리듀서는 `overwrite`와 `append` 두 가지입니다. 사용자 정의 리듀서는 컴파일 전에 C++에서는 `ReducerRegistry::register_reducer`로, Python에서는 `ng.ReducerRegistry.register_reducer`로 등록해야 합니다.

### "조건을 등록했는데 조건부 엣지가 실행되지 않습니다"

로더가 허용하는 형식인지 확인하세요([§4](#4-edges--conditional-routing)의 형식 A 또는 형식 B). v0.1.8부터는 둘 다 동작하고, 그 이전 wheel에서는 형식 B만 동작합니다.

### "execution_trace에 시작 노드만 나옵니다"

라우팅이 `__end__`로 빠졌습니다. 시작 노드에서 나가는 엣지가 없거나, 조건이 `routes` 맵에 없는 값을 반환했는데 명시적인 `"default"` 라우트가 `__end__`를 가리키는 경우가 가장 흔합니다. strict 그래프는 더 이상 맵의 순서로 라우트를 고르지 않습니다. 열린(open) 조건이거나 조건 종류를 지정하지 않았다면 `"default"`가 선언되어 있을 때 그 라우트를 쓰고, 선언되어 있지 않으면 엔진이 출발 노드, 조건, 반환된 레이블을 담은 예외를 던집니다. 닫힌(closed) 조건이 선언된 레이블 밖의 값을 반환하면 항상 예외가 발생합니다.

---

## 다음에 볼 문서

- [Python 예제](../bindings/python/examples/) — 위에서 설명한 개념을 모두 다루는
  독립 실행 스크립트 21개.
- [C++ 예제](../examples/) — 같은 구성의 프로그램 36개.
- [`reference-en.md`](reference-en.md) — 클래스별로 정리한 전체 API.
- [`ASYNC_GUIDE.md`](ASYNC_GUIDE.md) — 비동기/코루틴 계층 심층 설명.
