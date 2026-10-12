<!-- neograph-i18n: source=docs/concepts.md locale=ko source_sha256=3ddbf9b3903df0b61f2694ecfbb12af0682b96f25f81d50b4efc4865c4c576a1 -->
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

델타 기반 체크포인트는 채널 설정이 아니라 설계안입니다. 이 방식은 전체 스냅샷에서 시작해, 순서가 있는 `{channel, version, write mode, value}` 델타를 최대 *K*개(바이트 기준을 함께 둘 수도 있음)까지 재생(replay)합니다. 설계안은 다음 조건을 지켜야 합니다. overwrite, 보존 정책, 버전, 리듀서 식별 정보를 그대로 유지해야 합니다. 대기 중인 쓰기(pending writes)를 지우기 전에 스냅샷·델타와 체크포인트 포인터를 원자적으로 공개해야 합니다. 연결이 끊긴 기록, 버전 공백, 알 수 없는 리듀서, 재생(replay) 실패는 거부해야 합니다. 도입하려면 새 스키마 버전과 실측으로 확인한 이득이 필요합니다. 기존 스냅샷은 가짜 델타를 만들지 않고 기준(base)으로 옮기며, 되돌릴 수 있는 단계적 배포 동안에는 이전 reader를 위해 전체 스냅샷도 함께 남겨 둡니다. 델타만 있는 기록은 원래의 리듀서 레지스트리로 값을 복원할 수 없다면 다운그레이드를 거부합니다. 현재의 저장소는 모두 전체 스냅샷 방식입니다.

기준 성능은 `bench_checkpoint_store --threads 1 --iters 1 --history-steps 256 --payload 512 --backends memory,sqlite`로 측정합니다. `postgres`와 `--pg-url`은 격리된 로컬 데이터베이스에서만 추가합니다. 결과의 각 행에는 논리적 직렬화 바이트, 저장·로드의 p50/p95, 복원 깊이(reconstruction depth)가 나오고, legacy 행은 blob 개수를 보고합니다. 할당 요청 수는 `heaptrack bench_checkpoint_store --threads 1 --iters 1 --history-steps 256 --payload 512 --backends memory`로 확인합니다. 네이티브 JSON/SQL 할당자 중에는 C++ `operator new`로 가로채지 못하는 것이 있어서 전부 집계되지는 않습니다. 같은 payload, 기록 길이, 백엔드 설정으로 비교하고, 절감량은 추정이 아니라 측정값으로 판단해야 합니다. 논리적 바이트는 실제로 디스크에 저장되는 물리적 바이트와 다릅니다. SQLite는 종료할 때 지워지는 고유한 임시 데이터베이스를 쓰며, `--sqlite-path`를 지정하면 새로 만든 파일을 남기고 이미 있는 경로는 거부합니다.

아래는 Linux x86-64 Debug 빌드에서 스레드 1개, 기록 스텝 256개, 메시지 512바이트, 반복 1회로 기록한 기준값입니다. 과거에 측정한 값이며 성능 목표가 아닙니다.

| 백엔드 | 논리적 체크포인트 크기(바이트) | 저장 p50/p95 (µs) | 로드 p50/p95 (µs) | 재생(replay) 깊이 |
| --- | ---: | ---: | ---: | ---: |
| Memory | 17,814,952 | 54 / 138 | 141 / 382 | 1 |
| SQLite | 17,814,952 | 289 / 1,589 | 176 / 474 | 1 |

파일을 지우기 전에 따로 한 번 더 측정했을 때 SQLite 데이터베이스와 WAL의 크기는 14,811,136 / 4,210,672바이트(합계 19,021,808바이트)였습니다. Linux `LD_PRELOAD` shim으로 프로세스 전체의 `malloc`, `calloc`, 0이 아닌 `realloc` 요청을 센 결과도 있습니다. 생성과 JSON 파싱까지 포함한 수치이며, `--history-steps 0`일 때와 비교하면 Memory는 요청 88,277건 / 요청 바이트 605,289,027, SQLite는 114,295건 / 867,319,964바이트가 추가로 발생했습니다. 이 값은 누적 요청량이지 실제로 점유한 메모리나 저장소만의 할당량이 아닙니다. 정렬 할당과 내부 할당은 가로채지 못했습니다. 이 shim은 NeoGraph의 의존성이 아닙니다. 결론을 내리기 전에는 지원되는 프로파일러로 워밍업을 마친 실행을 여러 번 확인해야 합니다.

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

`ChatMessage` / `ChatTool`과 JSON은 이식 가능한 투영(portable projection)일 뿐 네이티브 권한(native authority)이 아닙니다. 이식 가능한 형식은 계속 [`provider-message-v2`](../schemas/provider-message-v2.schema.json)와 [`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json)입니다. 실제 C++ 체크포인트 사이드카는 네이티브 봉인(native seal)을 메모리에 보관합니다. 네이티브 기록을 영구 저장하려면 호스트가 소유하는 `sp::NativeArchive`가 필요합니다. 닫힌 형식인 v3 / `spna3`이며, 인증된 소유자 전용 보관(custody)과 독립된 키를 갖춰야 합니다. archive v2는 업그레이드하거나 해석하지 않고 거부합니다. 인증은 의미를 정하는 descriptor의 모든 선택(origin/경로/헤더, 정책, 요청 필드 매핑, 사용량 경로와 stop 매핑), 소유자, 정확한 보관 바인딩을 묶어서 검증합니다. 이는 암호화도 아니고 벤더 발급자 인증도 아닙니다. archive 본문, 키, 네이티브 blob, 원시 wire 관찰값은 절대 공개해서는 안 됩니다. archive는 증거를 저장하는 곳이지 비용 승인(money grant)이나 지출 임대(spending lease)가 아닙니다. Program과 외부 예산(bank)은 각자 저널이 따로 소유하며, 스냅샷을 복사해도 크레딧이 생기지 않습니다.

provider 기록에는 모드가 따로 있습니다. 같은 경로(route)에서 네이티브로 이어 갈 때는 원래의 바인딩 아래 실제 추론 내용, 서명, 순서가 있는 도구 그룹을 그대로 보존합니다. Gemini의 기본값은 `NativeOnly`입니다. `PortableForeign`을 명시하면 호출자가 만든 assistant 텍스트와 도구 호출을 받아들이며, 이때는 네이티브 봉인, wire 출력, 서명이 없습니다. 첫 번째 외부(foreign) 함수 호출에만 Google이 문서화한 우회 표식(bypass marker)을 붙이고, 텍스트만 있는 턴에는 서명을 붙이지 않습니다. 이 투영은 네이티브 권한을 부여하지 않으며, 실패한 네이티브 봉인을 복구하거나 격하하지도 않습니다. 여러 벤더가 섞인 임의의 기록을 네이티브 방식으로 옮길 수 있게 해 주는 것도 아닙니다.

Responses의 `previous_response_id`는 provider가 보관한 대화 상태를 고르며, 요청에는 새 입력만 담깁니다. 클라이언트 도구의 소유권을 확인하는 데 로컬 증거가 필요하면 `previous_response_history`가 실제 이전 소유권 증거를 제공하며, 이 증거는 입력으로 다시 전송되지 않습니다. 커서(cursor)는 완전한 네이티브 재생(replay) 봉인도 아니고 archive 권한도 아닙니다. origin, route, 모델, 설정, 완료 상태 검사는 그대로 적용됩니다.

현재 SDK 인터페이스 리비전과 공유 라이브러리 세대(generation)는 4입니다. 사용자는 이에 맞는 헤더와 라이브러리로 다시 빌드해야 합니다. 출력 생성 상한은 호출마다 네이티브 재생(replay) 설정과 별개로 승인(admission)을 받고 집계됩니다. 새로운 의미의 호출을 위해 이 상한을 올려도 원래의 bank, grant, deadline이 갱신되지는 않습니다. 문서에 명시된 턴별 선택을 제외하면 콘텐츠, prefix, origin, route, 정책, 도구, 추론 제어는 계속 바인딩된 채로 유지됩니다. 이식 가능한 JSON v2와 네이티브 archive v3 / `spna3`은 바뀌지 않았습니다. 아래에 있는 과거 ABI3 측정값은 interface4의 결과가 아닙니다.

Python도 C++와 같이 소유권이 있는 요청/결과 경계를 제공합니다. 이 경계는 `make_provider_request`, `Provider.prepare`, `dispatch`, `invoke`로 이루어집니다. provider 기록에는 타입이 있는 파트를 가진 `ProviderMessage`를 사용해야 합니다. `ChatMessage`는 그래프용 편의 투영으로 남아 있습니다. 반환된 SDK 실패는 `ProviderOutcome.failure`로 확인합니다. 호스트의 observer나 정산(settlement)에서 예외가 나면 그 예외가 `outcome`과 `cause`를 유지합니다. 생성자와 GIL/콜백 동작은 [Python 바인딩 가이드](python-binding.md)를 참고하세요.

`input_total`, `output_total`, `total` 같은 사용량 카운터는 `std::optional<sp::Count>`입니다. 값이 있는 카운터는 `uint64_t value`와 `Evidence`를 가집니다. `Usage`에는 단계(stage), 품질(quality), 충돌(conflicts)도 기록됩니다. 값이 없으면 알 수 없다는 뜻이며, 0으로 채우는 일은 없습니다.

`UsageAccumulator::snapshot()`은 누적된 보고서를 반환합니다. `total_tokens_wide()`는 청구된 토큰에 아직 해소되지 않은 예약을 더한 값을 반환하므로, 보고된 사용량으로 표시하면 안 됩니다. 정산(settlement)에는 입력·출력 카운트를 갖춘 최종 보고서가 일관되게 있어야 하며, 지원되는 총계 중 가장 큰 값을 청구하고 너무 큰 사용량도 상한으로 잘라 내지 않습니다. 누적된 보고서 중 하나라도 카운터가 없으면 집계 값에서도 알 수 없는 값으로 남습니다. 예약, 로컬 청구, 벤더 청구서는 서로 다른 기록입니다.

**독립형 bank 저널 정정 — 현재 계약을 수정했으며, 실제로 실행한 런타임 증거는 아래에 있습니다.** 소유자가 승인한 프로토콜은 다음을 요구합니다. 신뢰할 수 있는 저장소의 네임스페이스에 대해 단조 증가하는 의무(obligation)가 있어야 하고, 실제로 변경할 수 없는 원본 소유자/스레드/그래프 범위, 상한(ceiling), 데드라인/시계 식별자, 세대(generation)가 있어야 합니다. 체크포인트 전체 커밋먼트와 리비전에 대한 정확한 영속 head CAS만이 호스트 소유의 불투명한 lease를 발급할 수 있습니다. 대기 중인 효과의 정확한 구간(pending effect window)은 provider I/O 이전에 영속화되어야 합니다. 정산(settlement)에는 실제 SDK 결과와 실제 청구액, null 가능한 보고서, hold, 중복 제거 식별자를 써야 합니다. 체크포인트와 다음 head는 같은 소유 actor/리비전 아래서 원자적으로 게시되어야 합니다. bank 메타데이터를 지우거나, 체크포인트를 정리(prune)하거나, 인증된 예전 스냅샷을 재생(replay)하거나, 같은 ID를 덮어쓰거나, actor를 잃어도 크레딧이 생겨서는 안 됩니다. 이미 hold 65가 있는 상태에서 상한을 130에서 129로 줄이면 65를 추가로 승인(admission)할 수 없습니다. 효과가 없었다고 증명된 실패는 바뀌지 않은 head를 해제할 수 있으며, 그러면 실제 130 기준의 복구는 계속 진행할 수 있습니다. 크래시, 상태 불명, lease 유실로 생긴 구간은 환불, 재시도, 대체(fallback) 없이 hold 상태로 남습니다. 일반적이거나 처음 상태 그대로인 archive 설정은 금액이나 네이티브 지출 lease를 부여하지 않으며, 현재의 `config.usage`는 이미 있는 독립형 의무를 대체할 수 없습니다. Program/외부 bank 저널의 소유권은 바뀌지 않습니다. 여기에 적은 내용은 요구되는 계약이며, 실제 통화/보관(custody) 증거와 계측의 한계는 아래에 보고합니다. 안정 릴리스된 API를 보장한다는 뜻이 아닙니다.

**현재 선언 내용. 통합 런타임 증거는 아래에 있습니다.** `<neograph/graph/checkpoint.h>`는 `ManagedBudgetLeaseScope`를 선언하며, 이 타입은 `owner_scope`, 논리적 `thread_id`, 비공개 백엔드용 `storage_thread_id`, `graph_identity`, `original_ceiling`, `original_deadline_ticks`, `deadline_clock_identity`를 가집니다. `OwnedManagedBudgetLease`는 읽기 전용인 `scope()`, `actor_id()`, 변경할 수 없는 `bank_generation()`, `revision()`, `head_checkpoint_id()`, `head_commitment()`를 제공하며, 권한을 외부에서 가져오는 공개 생성자는 없습니다. `ManagedBudgetEffectReceipt`는 `active()`, `effect_id()`, `claim_amount()`, `request_digest()`를 제공하고, 기본 생성된 receipt는 아무 권한도 부여하지 않습니다. `CheckpointStore`는 `acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`, `begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`, `settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`, `publish_managed_budget_checkpoint(lease, checkpoint)`, `release_managed_budget_lease(lease)`를 선언하며 각각 `_async` 짝이 있습니다. 동기 `CheckpointStoreCore`와 `AsyncCheckpointStore`는 각자에 맞는 변형을 제공합니다. `managed_budget_checkpoint_commitment(checkpoint)`는 bank JSON만이 아니라 영속 체크포인트 전체를 대상으로 합니다. 이 선언만으로는 백엔드 CAS, 통화 안전성, 설치된 ABI 호환성, 실제로 실행해 본 런타임 경로가 보장되지 않습니다.

**실제 InMemory 공유 bank fork를 유지하고 실행해 확인했습니다.** 실제 C++ fork는 복제된 grant가 아니라 원본 금융 저널 하나와 신뢰할 수 있는 현재 브랜치 head를 사용합니다. `publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)`(와 `_async`)는 인증된 현재 원본/전체 커밋먼트와, 같은 bank를 가리키는 실제 네이티브 C++ 포인터를 요구합니다. 영속 독립형 fork는 명시적으로 지원하지 않습니다. `OwnedManagedBudgetLease::scope()`와 원본 소유자/스레드/그래프, 상한, 데드라인/시계, 세대는 변경할 수 없습니다. 저장소가 발급하는 읽기 전용 `execution_thread_id()` / `execution_storage_thread_id()`는 실행 브랜치를 따로 선택하고, `GraphState::budget_original_thread_id()`는 원본 금융 bank를 식별합니다. 선택된 브랜치 head에 대한 정확한 CAS와 전역 actor/리비전이 모든 브랜치를 정규 현재 카운터, 대기 중인 효과, 소진된(burned) 식별자에 맞춰 직렬화합니다. 원본 브랜치와 fork 브랜치는 보충 없이도 계속 쓸 수 있습니다. 오래된 스냅샷, 복사한 체크포인트, 가져온 JSON으로는 별칭을 만들거나 head를 되돌릴 수 없습니다. 원본 root30 → charge3 → 원본 continuation6 → fork lower20 → continuation9로 이어지는 같은 bank 증명은 바뀌지 않은 test_graph_engine.cpp:810–913에서 통과했습니다. 저장된 원본 상한 30은 fork의 유효 상한 20과 별개이며, 31로 넓히는 시도와 JSON만으로 하는 복원은 거부되어야 합니다. 상한이 없는 보고 관찰값은 사실 데이터일 뿐 유한한 grant가 아닙니다. 효과가 없었다고 증명된 lease만 바뀌지 않은 head를 해제할 수 있고, 알 수 없거나 대기 중인 효과는 의무를 그대로 유지합니다.

**현재 release 오류 계약. 실행한 테스트와 프로브는 아래에 있습니다.** `<neograph/graph/engine.h>`의 `graph::ManagedBudgetLeaseReleaseError`는 `ProviderOutcomeError`에서 파생됩니다. `cause()`는 원래의 실행 예외를 보존하고, `release_error()`는 이차적으로 발생한 영속 lease 처리 실패를 노출합니다. `outcome()`은 실제 SDK 증거가 있으면 그 증거를 유지하고, SDK 결과가 없으면 null입니다. release에 실패해도 결과를 지어내거나 재디스패치를 허용하지 않습니다. 닫힌 형식의 `_neograph_managed_budget_scope` 메타데이터는 원본 논리 범위/상한/데드라인 시계/세대를 기술하지만 데이터일 뿐 백엔드 CAS 권한이 아닙니다.

**Archive 소유자/보존 계약. 실행한 테스트와 프로브는 아래에 있습니다.** 유한한 독립형 루트와 인증된 유한 원본만이 생략된 원본 소유자를 실제로 설정된 `sp::NativeArchive::owner_scope()`에서 물려받습니다. 상한이 없거나 일반적인 소유자 메타데이터의 의미는 바뀌지 않았습니다. archive 소유자가 명시적으로 충돌하면 lease를 얻기 전에 거부합니다. `CheckpointStore::retains_native_checkpoint() const noexcept`와 이에 대응하는 Core/Async 저장소 기능은 기본값이 false이고, 실제 InMemory 백엔드는 true로 재정의합니다. 래퍼는 실제 보존 여부를 위임해야 합니다. 이 읽기 전용 설명은 lease가 없거나 일반적이거나 상한이 없는 C++ 네이티브 체크포인트를 정당하게 보관할 수 있게 합니다. 다만 지출 크레딧이나 네이티브 재생(replay) 권한은 부여하지 않습니다. lease가 있는 보관은 JSON 플래그나 추측한 저장소 타입이 아니라 저장소가 실제로 발급한 receipt를 사용합니다.

**네이티브 보관 사전 I/O 게이트. 실행한 테스트와 프로브는 아래에 있습니다.** 관리되는 효과(managed effect)를 시작하려면, 대기 효과·슬롯·보류 구간(held window)을 변경하기 전에 실제로 바인딩된 NativeArchive 또는 로컬 저장소가 실제로 발급한 비공개 C++ 보존 기능(retention capability)이 필요합니다. 이 비공개 기능은 JSON에서 가져오거나 wire로 전달하지 않습니다. 원격 백엔드가 InMemory여도 gRPC에서는 클라이언트와 서버 모두에 실제 archive가 필요합니다. C++ 사이드카는 그 경계를 넘을 수 없기 때문입니다. archive가 유한한 원본 소유자를 제공하지 않으면 원본의 익명 소유자 범위는 비어 있는 채로 남고, 실제 archive를 바인딩할 때는 원본 범위와 일치해야 합니다. 금융 head/lease 증거만으로는 네이티브 보관 준비가 되었다는 것을 증명하지 못합니다.

`ProgramFailure`는 살아 있는 `provider_outcome`과 `provider_cause`를 보관합니다. 사실을 기록한 정규 SDK 증인(witness)은 실제 archive 보관을 소유자/run/버전/번들/연산/시도에 묶습니다. Runtime은 복구한 실패를 노출하기 전에 설정된 보관을 먼저 복원합니다. 데이터만 담는 공개 `ProgramResult::create()`는 미리 채운 증인으로 이 과정을 우회할 수 없으며, 해석되지 않은 봉인은 실행 가능한 결과가 아닙니다. 프로세스를 재시작하면 원래의 예외 포인터는 사용할 수 없고(`provider_cause == nullptr`), 텍스트로 다시 만들지도 않습니다. 영속화할 수 없는 실패는 직렬화하거나 게시하거나 재생(replay)할 수 없습니다.

`RecordedBindingSet`은 원본에 묶여 있고 이동만 가능한 데이터이며, 호출자가 제공하는 dispatcher가 아닙니다. 신뢰할 수 있는 Catalog의 `recorded_capability_binder`가 실제로 저장된 원본 이벤트로부터 캡처된 기능(captured-only capabilities)만을 독립적으로 구성합니다. `ProgramRuntime::replay_recorded()`는 원래 선택된 원본의 권한을 확인한 다음, 실제로 남은 bank를 영속 CAS로 넘겨줍니다. 물려받은 지출은 새로운 모델 grant가 아닙니다. 예전의 `start_recorded` 갱신 API는 제거되었습니다. InMemory, File, SQLite, PostgreSQL Program 저장소는 실행하는 동안 변경할 수 없는 소유 lease를 그대로 보존하며, 만료되어도 갱신되지 않습니다. 제어되는 JavaScript도 기반이 되는 capability manifest를 계속 검증하고, 이미 완료된 command 결과를 그대로 사용하며 외부 효과를 다시 디스패치하지 않습니다.

**전체 테스트 스위트에서 실행해 확인한 Recorded-control 인과 수정.** 캡처된 command의 재생(replay)은 실행 전에 새로 필요한 CPU wall-time과 Core 작업분만 영속적으로 예약하고, 실행 뒤 측정된 작업량과 새로 만들어진 Core 체크포인트를 결과 CAS로 게시합니다. 새로운 모델, 금액, Program 연산 허용량은 소비하지 않으며 캡처된 외부 효과를 다시 디스패치하지도 않습니다. 조정(reconcile)되지 않은 예약은 차감된 채로 남습니다. 예약은 일반적인 Running→Running 전이가 아니라 인증된 정산(settlement) 전이를 선택합니다. 일반 전이는 처음 만들어지는 새 Core 체크포인트를 거부했습니다. await 채널 수신, 타이머 대기/취소, handoff 대기 시작/해제는 각각을 소유한 executor/strand에서 직렬화됩니다. 기존 Recorded CPU/Memory의 await/handoff 시나리오는 전체 스위트에서 통과했고, 원격 TSan 커버리지의 한계는 아래에 따로 적었습니다.

아래 관찰 기록은 이 문서를 정리하기 전에 작성했습니다. 과거의 증거이며 새로 실행한 테스트가 아니고, 모든 플랫폼·전송 방식·보안 속성에 대한 보증도 아닙니다.

**비용이 든 관찰 완료. 보편적인 자격 검증은 아님.** 원래의 `SPQUAL1`의 기본 한도 630/1000000 microUSD는 바뀌지 않았습니다. 해시로 연결된 `A` 하나가 같은 원본 원장 안에서 승인된 확장분 480/3000000을 승인(admission)해 합계가 1110/4000000이 되었고, 호출/지출/hold/정산(settlement)은 누적되며 새 grant ID, 헤더, 리셋은 없습니다. 선언 바이트와 파일 식별자 자체, 원래의 authorization/baseline/catalog/activation/원장 prefix의 해시와 합계는 그대로 고정되어 있으며, 제거·교체·변경은 실패 시 차단으로 처리합니다. 최종 정규 원장은 calls1110/spent437958/held1287828 microUSD, eventA1, limits1110/4000000입니다. spent+held는 US$1.725786이며 이는 로컬 카탈로그 미터 값이지 청구서가 아닙니다. 문서화된 5개 계열 × 60쌍의 기준 측정은 600건의 요청을 완료했습니다. Chat 60/60, Responses 60/60, Messages 60/60, Generate 56/60(비전 오답 SSE 4건), Interactions 57/60(비전 오답: 버퍼 1건, SSE 2건)이며 합계는 300쌍 중 293쌍이지 300/300이 아닙니다. 그 밖의 예전 600건의 금융 기록은 그대로 보존되어 있지만 동작 전체를 증명하지는 않습니다. 이전의 M5/미디어 1회성 코호트는 바뀌지 않았습니다. 이전 3라운드 Google 사전 조건에서는 잘못된 도구 2건과 읽을 수 없는 양성(positive) 1건의 실패가 그대로 남아 있습니다. 추가 유료 호출은 승인되지 않았습니다. 최종 SDK 증거와 네이티브 축의 한계는 기준 측정의 성공과 별개입니다. 이전의 활성화/재오픈 스모크 테스트는 두 번 재오픈한 뒤 calls610/spent219159/held751233으로 기록되어 있고, SDK meter/canary/vision 테스트 4건이 19.38초 만에 통과했습니다. 이는 범위가 한정된 이전 체크포인트이며 최종 원장 합계가 아닙니다. 이전에 검증한 Chat 60쌍 코호트에는 실제 시도 120건, UpperBound 청구 120건이 있고 UnknownHold는 없습니다.

**네이티브 축 관찰. 암호학적 검증이나 네이티브 소비/동등성을 뜻하지 않습니다.** Generate는 변조, 누락, 중복을 받아들였습니다. Interactions는 격리된 실제 원본(양성 대조군), 한 소유자의 서명 변조, thought 운반체 누락, call 운반체 누락과 중복을 받아들였습니다. 모든 thought와 서명을 제거하면 일반적인 400이 반환되었고, THOUGHT 항목은 남기고 모든 서명 필드만 제거해도 일반적인 400이 반환되었습니다. 마지막 캡처에는 로컬에서 인코딩한 원본을 유지하는 대조군이 있었으나 같은 캡처에서 얻은 서버 측 양성 결과는 아니었습니다. 이전의 양성 코호트는 여전히 실제 데이터입니다. 이 관찰들이 보여 주는 것은 운반체(carrier)가 전부 없는 경우의 경계뿐이며, 발급자나 서명 검증, 벤더의 실제 소비 여부를 입증하지는 않습니다. 실제 보고서는 SDK의 `config/qualification-extension-results.json`, `qualification-final-summary.json`, `qualification-native-axis-results.json`, `qualification-combined-omission-results.json`, `qualification-signature-presence-results.json`입니다. 사전 조건 실패, 미실행, 음성(negative) 불확정 상태도 있는 그대로 기록해 두었습니다. thought만 또는 운반체만 누락한 경우에도 다른 운반체가 남아 있으면 받아들여졌지만, 이 결과는 발급자 검증이나 네이티브 소비에 관한 주장을 강화하지 않습니다.

**실제로 실행해 확인한 통합 증명과 남은 한계.** 가장 최근의 Core 전체 실행은 테스트 2242개, 실패 0, 건너뜀 16(RAM 프로세스 유실 케이스 14개는 해당 없음, 실제 자격 증명이 필요한 게이트 2개), 130.17초였습니다. `PgNestedJsonRoundTrips`는 중복 키, 순서, null 메타데이터, blob, residual을 정확히 보존했고 0.18초가 걸렸습니다. 바뀌지 않은 원본 공유 bank fork와 기존 Recorded CPU/Memory await/handoff 시나리오는 통과했습니다. 실제 wrapped Memory/SQLite/PostgreSQL/gRPC에 대한 finite130/hold65/lower129/strip/old-head/pruning/no-archive/import 프로브는 일반 빌드와 ASan+UBSan 빌드 모두 통과했습니다. 로컬 Memory/SQLite/PostgreSQL TSan 범위에서는 7건이 통과했고 경고는 0건입니다. gRPC까지 섞은 전체 구성에 시스템 Abseil/Protobuf를 더한 TSan 실행은 종료 코드 66으로 끝났고, 의존성과 생성된 RPC 스택에서 race 경고가 402건 나왔습니다. 이는 계측/커버리지의 한계이지 오탐으로 증명된 것은 아닙니다. 원격 TSan이나 race-freedom은 주장하지 않으며 경고도 억제하지 않았습니다. 설치된 find_package로 Program C++/C ABI/dualQuickJS 소비자 세 개를 빌드해 통과했습니다. 새로 설치한 NeoGraph/SchemaProvider 타입 소비자는 실제 HTTP 요청 2건, 코루틴 시작 전 provider 소멸, 네이티브/도구 재생(replay), 거부(refusal), known-zero/raw 보존, 실제 LinkedMismatch 거부를 통과했습니다. 브라우저에서 Alice/Bob 격리와 generation 2 교체는 눈으로 확인했고, PostgreSQL Program Chat 블랙박스 테스트 6건은 18.989초에 통과했습니다. 가장 최근의 SDK 테스트는 26/26 통과, 실패 0, 74.07초였습니다. 최종 Release Graph는 16개 구성 × 새 프로세스 3회 반복(기록 48건)을 38.29초에 완료했고 실패 0건이며 실제 프로토콜과 소유 결과(owned-outcome) 검사를 모두 통과했습니다. NeoGraph의 `benchmarks/provider-cutover-final-results.json`과 `benchmarks/provider-cutover-final-summary.json`에 이 최종 코호트가 따로 보관되어 있습니다. 측정 중에는 컴파일러도 유료 모델도 실행하지 않았습니다. 과거 코호트는 바뀌지 않았으며 의미나 자원 사용의 동등성은 주장하지 않습니다. 불안정한 SDK/ABI3은 안정 릴리스도, 더 넓은 플랫폼에 대한 자격 검증도 아닙니다.

호스트 전달 한도, 범위가 제한된 진단/원시 증거, 공통 provider 오류, 최소한의 미디어 증거는 [타입이 있는 provider 레퍼런스](reference-en.md#owned-outcome)에 문서화되어 있습니다.

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

> 과거 이슈 #16에서는 일부 glibc/OpenSSL 조합에서, 요청마다 `io.run()`을 중첩해 호출하고 예전의 자식 스레드 provider 스트리밍 브리지를 쓸 때 `getaddrinfo`에서 SEGV가 관찰되었습니다. 이 브리지는 타입 기반 전환(typed cutover)에서 제거되었습니다. 당시의 구조 테스트 결과는 다운스트림 HTTPS, sanitizer, 부하 조건을 빠짐없이 재현한 것이 아니므로 현재의 자격 검증으로 볼 수 없습니다. 지금은 명시적인 `ProviderMode`와 `invoke_async` / `dispatch_async`를 쓰며, 요청마다 이벤트 루프를 중첩하지 않고 이미 있는 오래 유지되는 executor에서 작업을 구동해야 합니다. 우회책으로 가짜 토큰 하나를 만들어 내거나 completion을 다시 보내서는 안 됩니다.

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
