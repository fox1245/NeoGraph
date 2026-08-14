<!-- neograph-i18n: source=docs/python-binding.md locale=ko source_sha256=aa37c77f9c0d5edbcefc44cab95fa0bbd0eaf928751367baa9e4091c7cbbf592 -->
**Languages:** [English](python-binding.md) | [한국어](python-binding.ko.md) | [日本語](python-binding.ja.md) | [简体中文](python-binding.zh-CN.md)

# 파이썬 바인딩


NeoGraph는 `pip`로 설치하는 Python 패키지로도 제공됩니다. 동일한 C++ 엔진을
Jupyter 노트북, Gradio 앱, FastAPI 서비스에서 LangGraph와 비슷한 워크플로로
구동할 수 있습니다.

```bash
pip install neograph-engine
```

## 5초 데모(API 키 없음)

설치가 제대로 되었는지 확인하는 가장 짧은 예제입니다. 데코레이터로 노드를
정의하고 실행한 뒤 결과를 읽습니다.

```python
import neograph_engine as ng

@ng.node("greet")
def greet(state):
    return [ng.ChannelWrite("messages",
        [{"role": "assistant", "content": f"Hello, {state.get('name')}!"}])]

definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "demo",
    "channels": {"name":     {"reducer": "overwrite"},
                 "messages": {"reducer": "append"}},
    "nodes":    {"greet": {"type": "greet"}},
    "edges":    [{"from": ng.START_NODE, "to": "greet"},
                 {"from": "greet",       "to": ng.END_NODE}],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())
result = engine.run(ng.RunConfig(thread_id="t1", input={"name": "NeoGraph"}))

print(result.output["channels"]["messages"]["value"])
# [{'role': 'assistant', 'content': 'Hello, NeoGraph!'}]
```

## 실제 LLM를 사용하는 ReAct 에이전트

```python
import neograph_engine as ng
from neograph_engine.llm import OpenAIProvider

class CalcTool(ng.Tool):
    def get_name(self):       return "calc"
    def get_definition(self): return ng.ChatTool(name="calc", description="multiply by 2",
        parameters={"type":"object","properties":{"x":{"type":"number"}}})
    def execute(self, args):  return str(args["x"] * 2)

ctx = ng.NodeContext(
    provider=OpenAIProvider(api_key="sk-..."),
    tools=[CalcTool()],
    instructions="Use `calc` for arithmetic.",
)

definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "react",
    "channels": {"messages": {"reducer": "append"}},
    "nodes":    {"llm": {"type": "llm_call"}, "dispatch": {"type": "tool_dispatch"}},
    "edges":    [{"from": ng.START_NODE, "to": "llm"}, {"from": "dispatch", "to": "llm"}],
    "conditional_edges": [{"from": "llm", "condition": "has_tool_calls",
                           "routes": {"true": "dispatch", "false": ng.END_NODE}}],
}
engine = ng.GraphEngine.compile(definition, ctx)
result = engine.run(ng.RunConfig(thread_id="t1",
    input={"messages": [{"role": "user", "content": "What is 21 * 2?"}]},
    max_steps=10))
```

## 출력 읽기

`engine.run(...)`는 다음 필드가 포함된 `RunResult`를 반환합니다.

|필드|유형|의미|
|---|---|---|
|`output`|`dict`|최종 상태 — `{"channels": {...}, "global_version": int}`. 채널을 읽으려면 `output["channels"][name]["value"]`를 사용하세요.|
|`max_steps_exhausted`|`bool`|`True`는 실행 가능한 작업이 남아 있는 동안 단계 한도가 실행을 중지한 경우에만 해당됩니다.|
|`interrupted`|`bool`|실행이 `interrupt_before` / `interrupt_after` / `NodeInterrupt`에서 일시 중지된 경우 `True`입니다.|
|`interrupt_node`|`str`|인터럽트를 트리거한 노드의 이름입니다(`interrupted`인 경우).|
|`interrupt_value`|`dict`|동적 인터럽트의 경우 `{"reason": str, "type": "NodeInterrupt", "value": ...}`(노드가 페이로드를 연결할 때만 존재하는 `"value"`) 또는 정적 `interrupt_before`/`interrupt_after`의 경우 `{"message": ...}`입니다.|
|`checkpoint_id`|`str`|실행 중 저장된 최신 체크포인트의 ID입니다. 참고용 값이며, `resume_async()`는 체크포인트 ID가 아니라 `thread_id`로 재개합니다.|
|`execution_trace`|`list[str]`|실행된 순서대로의 노드 이름 — 라우팅 디버깅에 유용합니다.|

`RunConfig`는 LangGraph `RunnableConfig`의 개념을 따릅니다.

중단된 실행을 비동기로 재개하려면 스레드 ID를 전달하고, 필요하면 사람의 응답도
함께 전달하세요.

```python
result = await engine.resume_async(thread_id="t1", resume_value=answer)
```

|필드|기본|의미|
|---|---|---|
|`thread_id`|필수|대화 또는 세션 식별자입니다. 체크포인트 스트림을 분리합니다.|
|`input`|`{}`|초기 채널 값입니다. 키는 그래프의 `channels` 정의와 일치해야 합니다.|
|`max_steps`|50|슈퍼스텝 실행 한도입니다. ReAct 루프에는 보통 10 이상이 필요합니다.|
|`stream_mode`|`StreamMode.OFF`|`EVENTS \| TOKENS \| DEBUG \| VALUES \| UPDATES \| ALL` 비트마스크입니다. `run_stream`과 `run_stream_async`에서만 사용합니다.|
|`resume_if_exists`|`False`|`True`이고 체크포인트 저장소가 설정되어 있으면 `thread_id`의 최신 체크포인트를 불러온 뒤 `input`을 채널 리듀서로 적용합니다. 따라서 이전 상태를 `input`에 직접 넣지 않고도 다중 턴 대화를 이어갈 수 있습니다. 기본값은 새로 시작하는 동작을 유지합니다. HITL 중단을 재개할 때는 `engine.resume_async()`를 사용하세요.|
|`cancel_token`|`None`|협력적 취소를 위한 선택적 `CancelToken`입니다. `engine.run()` 전에 할당하고 다른 Python 스레드에서 `token.cancel()`을 호출하세요. 엔진은 다음 슈퍼스텝 경계에서 멈추며, 오래 실행되는 노드는 `input.ctx.cancel_token`을 확인해야 합니다.|

```python
token = ng.CancelToken()
config = ng.RunConfig(thread_id="job-42", input={"query": "..."})
config.cancel_token = token

# Run engine.run(config) in a worker thread, then request cancellation from
# the caller thread when the request disconnects or the user presses Stop.
token.cancel()
```

## Python 노드에서 사람의 확인을 기다리기

그래프 정의의 `interrupt_before`는 그래프를 작성할 때 지정한 노드 앞에서 실행을
멈춥니다. 하지만 실제 HITL에서는 어떤 작업이 위험한지 모델의 요청을 확인한 뒤에야
알 수 있으므로, 이 설정만으로는 충분하지 않습니다. 예를 들면 다음과 같습니다.

> *"에이전트가 `rm -rf build/`를 실행하려고 합니다. 허용하시겠습니까?"*

이 경우에는 노드가 직접 `NodeInterrupt`를 발생시키고 승인에 필요한 정보를 함께
전달합니다. 엔진은 체크포인트를 저장한 뒤 일반 `RunResult`를 반환하며, 사람의
응답은 요청을 생성한 노드로 돌아갑니다.

```python
import neograph_engine as ng

class ApprovalNode(ng.GraphNode):
    def run(self, input):
        # The human's answer. None until someone has actually answered — which
        # is how you tell "nobody has looked yet" from "the answer was no".
        verdict = input.ctx.resume_value

        if verdict is None:
            raise ng.NodeInterrupt(
                {"tool": "shell", "cmd": "rm -rf build/"},
                reason="shell command needs approval")

        if not verdict.get("approved"):
            return [ng.ChannelWrite("result", "refused")]
        return [ng.ChannelWrite("result", "done")]

    def get_name(self):
        return "risky"
```

```python
result = engine.run(cfg)

if result.interrupted:
    print(result.interrupt_node)               # "risky"      — which node paused
    print(result.interrupt_value["reason"])    # for a human to read
    print(result.interrupt_value["value"])     # for your code to branch on

    result = engine.resume(cfg.thread_id, {"approved": True})   # the answer
```

일반 문자열을 사용하는 `NodeInterrupt(reason)`도 작동하며 `"value"`를 생략합니다.
열쇠. 당신이 제기하는 다른 모든 것은 오류로 남아 있습니다. 노드의 버그로 인해 실행이 실패합니다.
인간에게 질문처럼 보이기보다는 큰 소리로.

체크포인트 저장소가 필요합니다. 그렇지 않으면 재개할 것이 없습니다.

## 대화 전반에 걸쳐 사용자 기억 — 스토어

체크포인트는 **한 번의 대화**를 기억합니다. Store는 **사용자**를 기억합니다.
그들 모두에 걸쳐.

```python
store = ng.InMemoryStore()
engine.set_store(store)

class Greet(ng.GraphNode):
    def run(self, input):
        seen = input.ctx.store.get(["users", "u1"], "visits")
        n = (seen.value["n"] if seen else 0) + 1
        input.ctx.store.put(["users", "u1"], "visits", {"n": n})
        return [ng.ChannelWrite("greeting", f"visit #{n}")]
```

네임스페이스는 계층적 목록이므로 `store.search(["users"])`는 모든 것을 찾습니다.
모든 사용자 아래에서 `store.search(["users", "u1"])`는 한 사용자의 항목을 찾습니다.
`get()`는 누락된 경우 `None`를 반환합니다. 부재는 오류가 아니라 대답입니다.

대신 `ng.Store`를 서브클래스하여 데이터베이스에 넣습니다.

사용자 정의 체크포인트 지속성은 동일한 방식으로 작동합니다: 하위 클래스
`ng.CheckpointStore` 및 `save`, `load_latest`, `load_by_id`, `list`를 구현합니다.
및 `delete_thread`. 선택적 `put_writes`, `get_writes` 및
`clear_writes` 메소드는 기본적으로 no-op/full-super-step 재생 동작을 수행합니다. 가치
내부 `StoreItem`, `Checkpoint` 및 `PendingWrite`는 일반 Python JSON입니다.
도형(`dict`, `list`, 문자열, 숫자, 부울 및 `None`).

## 429에서 물러남 — RateLimitedProvider

```python
from neograph_engine.llm import RateLimitedProvider, OpenAIProvider

provider = RateLimitedProvider(OpenAIProvider(...), max_retries=5)
engine = ng.GraphEngine.compile(definition, ng.NodeContext(provider=provider))
```

이것이 없으면 결국 자체 재시도 루프에 `engine.run()`를 래핑하게 됩니다.
**전체 그래프** 재시도 - 이미 성공한 모든 노드를 다시 실행합니다. 이것
실패한 HTTP 요청 하나를 다시 시도합니다.

업스트림의 `Retry-After`가 있는 경우 이를 존중하고 다음으로 대체됩니다.
`default_wait_seconds`가 없으면 단일 절전 모드를 다음으로 제한합니다.
`max_wait_seconds`, `max_total_wait_seconds`의 수면이 일단 포기됩니다.
누적됩니다(`0` = 총 한도 없음).

귀하의 공급자는 올바른 예외를 발생시켜 선택합니다.

```python
class MyProvider(ng.Provider):
    def complete(self, params):
        r = requests.post(...)
        if r.status_code == 429:
            raise ng.RateLimitError(
                "rate limited",
                retry_after_seconds=int(r.headers.get("Retry-After", -1)))
```

당신이 제기하는 다른 모든 것은 오류로 남아 있습니다.

## 그래프를 실행하기 전에 확인하기 — `validate`

```python
report = ng.validate(definition)
if report.has_errors():
    print(report.summary())
    for d in report.errors():
        print(d.code, d.path, d.message)
```

매달린 가장자리, 도달할 수 없는 노드, 죽은 장벽 — 읽을 수 있는 보고서
`compile()`가 언제 발생하는지 알아내는 것보다.

알아야 할 한 가지 가장자리: `validate()`는 먼저 정의를 컴파일하므로 노드
아무도 등록하지 않은 표면을 진단이 아닌 **예외**로 입력하세요. 등록하다
컴파일하기 전과 똑같이 검증하기 전에 노드 유형을 지정합니다.

**노드 수준의 재시도에는 클래스가 필요하지 않습니다.** `"retry_policy": {...}`를
그래프 정의와 엔진은 이를 존중합니다. 이는 항상 Python에서 작동했습니다.

```python
definition["retry_policy"] = {"max_retries": 5, "initial_delay_ms": 100}
```

## MCP — 원격 도구 서버 사용

```python
client = ng.mcp.MCPClient("http://localhost:8931")     # or ["python", "server.py"]
client.initialize()

engine = ng.GraphEngine.compile(
    definition, ng.NodeContext(tools=client.get_tools()))
```

이것이 전체 통합입니다. `get_tools()`는 서버의 카탈로그를 다음과 같이 반환합니다.
그래프가 전달할 수 있는 도구를 자신의 Python과 자유롭게 혼합할 수 있습니다.
동일한 `NodeContext`의 도구.

`client.call_tool(name, args)`는 그래프 외부에서 직접 호출합니다.

**서버가 겹치면 중복됩니다.** MCP 도구는 네트워크 왕복입니다.
동시 파견이 지불되는 경우이고 `MCPTool`는 실제 C++입니다.
`AsyncTool`. HTTP는 동시 요청을 사용합니다. stdio 프레임은 쓰기 및 상관 관계를 작성합니다.
JSON-RPC id의 잘못된 응답:

|수송|3통화 × 0.4초|
|---|---|
|HTTP|**0.41초** — 각 호출은 자체 요청입니다.|
|스튜디오|동시 서버 포함 **~0.4초** — 파이프 1개, 요청 ID 다중화|

요청을 순차적으로 처리하는 하위 프로세스에는 여전히 ~1.2초가 걸립니다. 멀티플렉싱
클라이언트 측 병목 현상을 제거합니다. 서버측 동시성을 생성할 수 없습니다.

`get_tools()` 및 `call_tool()`의 경우 초기화가 자동으로 이루어지며 명시적
`initialize()`는 유효하고 멱등성을 유지합니다. `get_initialize_result()` 노출
협상된 프로토콜, 기능, 서버 정보 및 지침.
`get_tool_definitions()`는 모든 페이지 매김 커서를 따르고 전체 MCP를 유지합니다.
메타데이터. 필요할 때 `call_tool_result()` 또는 `MCPTool.execute_result()`를 사용하세요.
`structured_content`, 텍스트가 아닌 블록, `is_error` 또는 `_meta`; `call_tool()`는
소스 호환 원시 JSON 외관.

세션에 대한 마지막 참조가 다음과 같을 때 stdio 하위 프로세스가 종료됩니다.
삭제 — 클라이언트 또는 클라이언트가 생성한 도구.

실행 가능, 오프라인(자체 MCP 서버 시작): [`examples/26_mcp_tools.py`](../bindings/python/examples/26_mcp_tools.py).

## 도구를 동시에 실행하기

모델이 한 번에 여러 도구를 요청하면 NeoGraph는 이를 파견합니다.
함께. 실제로 *겹치는*지 여부는 도구의 선택입니다.

```python
class Fetch(ng.AsyncTool):          # ng.Tool -> serial;  ng.AsyncTool -> overlaps
    def execute(self, arguments):
        return requests.get(arguments["url"]).text
    ...
```

각각 300ms를 기다리는 20개의 도구가 측정되었습니다.

|도구 기본 클래스|벽시계|
|---|---|
|`ng.Tool`|6.0초|
|`ng.AsyncTool`|**0.30초** (19.9×)|

**선택한 이유.** 동기화 `Tool`는 다음 동기화 전에 완료되도록 실행됩니다.
시작 - 상태를 유지하는 기존 도구가 갑자기 자신을 찾을 수 없도록
자신의 복사본을 경주합니다. 동시성은 선언하는 것이지 어떤 것이 아닙니다.
그런 일이 당신에게 일어납니다. 반대 측면: 동일한 `AsyncTool`에 대한 두 번의 호출이 가능합니다.
한 번에 비행(모델이 한 차례에 두 번 요청할 수 있음)하므로 호출별로 유지하세요.
`self`가 아닌 스택의 상태입니다.

**명확하게 명시된 경계.** Python 함수는 실행되는 동안 GIL를 보유합니다.
귀하의 도구는 도구를 잡고 있지 *않을* 동안에만 형제 도구와 겹쳐집니다.
I/O에서 차단되는 동안입니다. 왜냐하면 CPython이 이를 놓아주기 때문입니다. HTTP
호출, 소켓 읽기, 데이터베이스 쿼리, `time.sleep`: 모두 해제하고 모두 겹칩니다.

**Python에서** CPU를 굽는 도구는 몸 전체에 GIL를 보유하고
겹치지 않지만 처리되는 스레드 수는 많습니다.

|CPU 바운드 `AsyncTool` 3개|3.1× 하나의 시간|
|---|---|

그러한 도구 `AsyncTool`를 선언하면 아무것도 사지 않습니다. (힘든 일이 생기면
numpy, C 확장 또는 하위 프로세스 내에서 GIL가 릴리스되고
중복됩니다.) 이는 테스트에 의해 고정되었으므로 주장이 조용히 표류할 수 없습니다.

동시성은 내부 작업자 풀(기본적으로 32개 스레드)로 제한됩니다.
`NEOGRAPH_TOOL_THREADS`. 그들은 I/O에 차단된 시간을 보냅니다.
수영장 비용은 거의 들지 않습니다.

실행 가능, 오프라인: [`examples/25_async_tools.py`](../bindings/python/examples/25_async_tools.py).

## 게이팅 도구 호출 - "에이전트가 `rm -rf build/`를 실행하려고 합니다. 허용하시겠습니까?"

*도구 X를 요청한 모델*과 *도구 X 실행* 사이에는 하나의 후크가 있으며,
세 가지 결과 중 하나를 반환합니다.

```python
def gate(call, gctx):
    if call.name not in DANGEROUS:
        return ng.ToolDecision.allow()

    # None until a human has actually answered — which is how the gate tells
    # "nobody has been asked yet" from "the answer was no", and so avoids
    # asking the same question forever.
    if gctx.resume_value is None:
        return ng.ToolDecision.interrupt(
            f"{call.name} needs approval",
            {"tool": call.name, "arguments": call.arguments})

    if gctx.resume_value.get("approved"):
        return ng.ToolDecision.allow()
    return ng.ToolDecision.deny("the operator refused this command")

engine.set_tool_gate(gate)
```

```python
result = engine.run(cfg)
if result.interrupted:
    print(result.interrupt_value["reason"])   # "shell needs approval"
    print(result.interrupt_value["value"])    # {"tool": ..., "arguments": ...}
    result = engine.resume(cfg.thread_id, {"approved": True})
```

|평결|효과|
|---|---|
|`ToolDecision.allow()`|실행하세요.|
|`ToolDecision.allow({...})`|대신 이러한 인수를 사용하여 실행하세요. 모든 도구가 이에 대해 알고 있는 것이 아니라 주변 값(테넌트, 스레드, 자격 증명)이 주입되는 곳입니다.|
|`ToolDecision.deny(reason)`|실행하지 마십시오. 그 이유는 도구의 결과로 모델에 돌아가서 다음 차례에 동일한 도구를 다시 요구하는 대신 적응할 수 있기 때문입니다.|
|`ToolDecision.interrupt(reason, payload)`|실행하지 말고 전체 실행을 일시 중지하세요. 페이로드는 `RunResult.interrupt_value["value"]`에 나타납니다.|

허가, 감사, 인수 재작성 및 호출별 인터럽트가 4개가 아닙니다.
특징; 그들은 네 개의 모자를 쓴 하나의 원시인입니다.

**게이트는 도구가 실행되기 전에 모든 호출을 확인하며 해당 순서는
design.** 모델이 `list_files`와 `shell`를 함께 요구한다고 가정하고,
`shell`에는 승인이 필요합니다. 실행이 일시 중지되면 `list_files`는 실행되지 **않습니다**
— 게이트가 허용했지만.

그것은 실수가 아닙니다. `resume()`는 위에서부터 노드에 다시 들어갑니다.
중단된 노드는 쓰기를 기록하지 않았습니다. `list_files`가 이미 실행되었다면
승인은 *두 번째* 실행됩니다. `git commit`로 바꾸고 프롬프트를 표시합니다.
`rm -rf`의 경우 방금 이중 커밋했습니다. 그리고 인간이 **아니요**라고 말하면 무엇이든 가능합니다.
이미 실행된 작업은 취소할 수 없습니다. "거부"가 수행되는 권한 시스템
"아무 일도 일어나지 않았다"는 의미는 허가 시스템이 아닙니다.

두 가지 실용적인 참고 사항:

- **체크포인트 저장소가 필요합니다.** 인터럽트는 재개 가능해야 합니다. 없이
다시 시작할 것이 없는 가게.
- **게이트는 `RunConfig`가 아닌 엔진에 있습니다.** `resume()`는 자체적으로 구축됩니다.
내부적으로는 `RunConfig`이므로 실행별 게이트는 사람이 접속하는 순간 사라집니다.
발생한 프롬프트에 응답하면 위험한 도구가 실행됩니다.
선택 해제됨. 엔진에 한 번 설정하면 매 실행 및 재개 시 유지됩니다.

종단 간 실행 가능, API 키 없음: [`examples/24_tool_approval_gate.py`](../bindings/python/examples/24_tool_approval_gate.py).

## 내장형 감속기

채널에는 새로운 쓰기가 기존 값과 결합되는 방식인 감속기가 필요합니다.
오늘부터 두 가지 내장 기능이 출시됩니다.

|감속기|행동|일반적인 사용|
|---|---|---|
|`"overwrite"`|새로운 가치가 이전 가치를 대체합니다.|단일 값 채널: `name`, `current_question`, 중간 스크래치.|
|`"append"`|기존 목록에 연결된 새 목록입니다.|대화 기록, 중간 결과, 노드 전체에 축적하고 싶은 모든 것.|

사용자 정의 감속기는 Python에서 등록됩니다(v0.1.9부터):

```python
ng.ReducerRegistry.register_reducer("sum",
    lambda current, incoming: (current or 0) + incoming)

# Now `"reducer": "sum"` works in your channel definitions.
```

조건부 라우팅과 동일한 패턴 — `ng.ConditionRegistry.register_condition("name", fn)`
여기서 `fn(state) -> str`는 경로 키 중 하나를 반환합니다.

## 바인딩으로 다루는 내용

- **엔진 표면** — `GraphEngine.compile / run / run_stream / run_async / run_stream_async / resume / resume_async / get_state / get_state_history / update_state / fork`, `RunConfig`, `RunResult`, `set_worker_count`, `set_checkpoint_store`, `set_node_cache_enabled`.
- **사용자 정의 Python 노드** — `neograph_engine.GraphNode` 서브클래스, `NodeFactory.register_type` 또는 `@neograph_engine.node` 데코레이터를 통해 등록합니다. 팬아웃 작업자 스레드를 포함하여 적절한 GIL 처리에 따라 엔진이 디스패치됩니다.
- **사용자 정의 Python 도구** — 하위 클래스 `neograph_engine.Tool`, `NodeContext(tools=[...])`에 전달합니다. 엔진은 컴파일 타임에 소유권을 갖습니다.
- **비동기** — 모든 `*_async` 바인딩은 호출 스레드의 실행 루프에 바인딩된 `asyncio.Future`를 반환합니다. 스트림 콜백은 `loop.call_soon_threadsafe`를 통해 루프 스레드로 호핑되므로 asyncio가 예상하는 곳에서 콜백이 실행됩니다.
- **체크포인트** — Python 백엔드는 `CheckpointStore`를 서브클래싱하거나 `InMemoryCheckpointStore`를 직접 사용할 수 있습니다. 바인딩을 `-DNEOGRAPH_BUILD_POSTGRES=ON`으로 빌드하면 `PostgresCheckpointStore`도 사용할 수 있으며, 지원되는 휠에는 libpq가 함께 포함됩니다.
- **WebSocket을 통한 OpenAI 응답** — `SchemaProvider(schema="openai_responses", use_websocket=True)`.

휠: Linux x86_64(manylinux_2_34), Linux aarch64(manylinux_2_34),
macOS arm64(14+), Windows x64(MSVC), Python 3.9 → 3.13용. **바퀴 20개
+ 릴리스당 sdist** cibuildwheel을 통해.

자세한 내용은 [`bindings/python/examples/`](../bindings/python/examples/)를 참조하세요.
전체 예시 인덱스 — 최소 그래프, ReAct, HITL, 인텐트 라우팅, 비동기,
다중 에이전트 토론, JSON 그래프 왕복, Gradio 채팅
심층 연구 하위 그래프(Crawl4AI + Postgres 선택 사항).

## LangGraph(Python 바인딩)와의 차이점

주제는 "C++용 LangGraph"이지만 몇 가지 의미가 다음과 다릅니다.
LangGraph Python — 포트 중간에 충돌하지 않도록 여기에 표시됩니다.

- **다회전 `thread_id`는 선택 사항입니다** — `engine.run(cfg)`는
동일한 `thread_id`는 이전 턴을 자동으로 로드하지 **않습니다**
기본적으로 체크포인트; 모든 실행은 `cfg.input`에서 새로 시작됩니다.
LangGraph 스타일의 "로드"에 대해 `cfg.resume_if_exists = True`를 설정합니다.
최신, 상단에 입력 적용" 동작. 기본값은 `False`이므로
이미 `input` 자체를 통해 상태를 스레드하는 호출자는
영향을 받지 않습니다. 위의 `RunConfig` 테이블을 참조하세요.
- **`update_state`는 `ChannelWrite`의 dict 또는 목록을 허용합니다** —
`update_state(thread_id, channel_writes, as_node='')` 소요
`channel_writes`의 두 가지 모양 중 하나:
  - dict: `{"messages": [...]}` — 직접 입력된 형식, 가장 가까운 형식
    to LangGraph's `values={...}` (kwarg name differs).
  - 목록: `[ChannelWrite("messages", [...]), ...]` — 대칭
    what every node body emits.

목록 항목은 중복 채널을 포함해 순서대로 적용되고
`ChannelWrite.Mode.OVERWRITE`를 보존합니다. dict 형식은 각 키 값에
reducer를 적용합니다. 다른 유형은 자동 no-op 대신 `TypeError`를
발생시킵니다(v0.3.2 이전 트랩은 #5 항목으로 닫혔습니다).
- **`get_state(thread_id)`는 중첩된 사전 — ​​`get_state_view`를 반환합니다.
플랫 도우미** — `state["channels"]["messages"]["value"]`
표준 원시 형태입니다(버전 전체에서 안정적임). 을 위한
인체공학적 도트 액세스, 사용
`view = engine.get_state_view(thread_id)` 및 `view.messages` 읽기,
`view.scratch` 등을 직접 사용합니다. `view.raw`는 평평하지 않은 부분을 노출합니다.
버전/메타데이터가 필요한 호출자를 위한 dict입니다. 서브클래스 `StateView`
유형화된 액세스를 위해 선언된 필드(Pydantic v2) 포함:
그러면 `class ChatState(ng.StateView): messages: list[dict] = []`
`engine.get_state_view(thread_id, model=ChatState)`.
- **Python `Provider` 하위 클래스는 동기화 `complete` 및
`complete_stream` 메소드** — 비동기 가상은 Python에 바인딩되지 않습니다.
사용자 정의 공급자 하위 클래스이므로 사용자 정의 Python 공급자는 다음을 통해 서비스를 제공합니다.
항목을 동기화합니다. 비동기 네이티브 공급자 통합(HTTP/2 멀티플렉싱,
다른 코루틴과 실제로 겹치는 경우) C++에 머무르고 다음에서 파생됩니다.
`neograph::CompletionProvider`가 있습니다.
- **한 줄 토큰 방출** — `neograph_engine.streaming import에서
Emit_token`, then `emit_token(cb, self._name, token)` 내부
스트리밍 노드. 4라인 `GraphEvent` 구성을 대체합니다.
의례.
- **관측성은 별도의 SaaS가 아닌 트리 내에서 제공됩니다** — 쌍
`neograph_engine.tracing.otel_tracer`(공급업체 중립적인 OTel 범위)
`neograph_engine.openinference.OpenInferenceProvider` +로
`openinference_tracer`(LLM 모양 속성 키),
모든 OpenInference 인식 백엔드(Phoenix, Arize,
Langfuse — 모두 OSS, 모두 자체 호스팅 가능), LangSmith를 얻습니다.
UX(턴당 채팅 버블, DAG 계층, prompt/response 캡처,
호출당 토큰 수 및 비용) 공급업체 SaaS 계약 없이.

  ```bash
  docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix
  pip install neograph-engine opentelemetry-exporter-otlp
  ```
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

  wrapped = OpenInferenceProvider(OpenAIProvider(api_key=...), tracer)
  ctx = ng.NodeContext(provider=wrapped)
  engine = ng.GraphEngine.compile(graph, ctx)
  with openinference_tracer(tracer) as cb:
      engine.run_stream(cfg, cb)
  # → http://localhost:6006 renders the trace as a LangSmith-style chain.
  ```

LangGraph의 호스팅 LangSmith는 일반적인 관찰 경로입니다.
그 생태계에서; LangFuse / Phoenix는 OSS 대체품입니다.
그러나 통합 접착제가 필요합니다. 네오그래프의 `OpenInferenceProvider`
*는* 통합 접착제입니다. `Provider.complete()`마다 드롭인하세요.
자동으로 LLM 범위가 됩니다.
- **단일 노드 방식** — `def run(self, input)`는 다음에서 도입되었습니다.
**v0.4.0**이며 **v0.9.0** 이후의 유일한 사용자 정의 노드 재정의입니다.
실시간 `input.state`에서 상태 읽기
스트리밍 싱크인 `input.ctx.cancel_token`의 취소 핸들
(또는 `input.stream_cb`의 `None`). `list[ChannelWrite]`를 반환합니다.
`list[Send]`, `Command` 또는 `NodeResult`. 보다
이동 시 [`migration-v0.4-to-v1.0.md`](migration-v0.4-to-v1.0.md)
이전 `execute*` 노드.
- **Python Dep 2개, 마침표** — `pip install neograph-engine`
`certifi` 및 `pydantic>=2.0`를 가져오며 이것이 전체 런타임입니다.
의존성 트리. 그래프 엔진, 스케줄러, 체크포인트 스토어,
HTTP/WebSocket 클라이언트, MCP/A2A/ACP 전송, OpenAI 호환
공급자 및 Postgres/SQLite 체크포인트 백엔드는 모두 기본입니다.
C++는 휠에 구워졌습니다.
LangGraph의 전이적 런타임 비교: `langgraph` →
`langchain-core` → `langchain` → `langchain-community` (각각
빠르게 움직이는 패키지) 및 통합별 패키지(`langchain-openai`,
`langchain-anthropic`, `langchain-postgres`, `langchain-chroma`, …).
이것이 바로 작동하는 LangGraph 스크립트가 6개월 후에 중단되는 이유입니다.
Pydantic v1→v2는 2024년에 세계를 강타했으며 수입 경로는 표류합니다.
모든 마이너 릴리스.
NeoGraph의 Python 표면은 고정된 레이어 위의 얇은 pybind11 레이어입니다.
의미론적 버전 관리에서 C++ ABI. 다음에 대해 작성된 사용자 정의 노드
v0.4.x `execute*` 호환성 창은 `run(input)`로 마이그레이션되어야 합니다.
v0.9.0은 v1 준비 중에 해당 레거시 노드 표면을 제거했습니다.
- **배포에 Docker가 필요하지 않습니다** — 다음의 직접적인 결과
위의 단일 깊이 트리. 프로덕션 LangChain 배포
효과적으로 *필요* Docker + 완전히 고정된 `requirements.txt`;
그것이 없으면 전이적 패키지가 다음 패키지에서 조용히 사소한 충돌을 일으키게 됩니다.
배포는 런타임에 서버를 중단시킬 수 있습니다. NeoGraph의 휠쉽
전체 기본 런타임이 포함되어 있으므로 다음과 같습니다.

  - 베어 메탈의 `pip install neograph-engine` / VPS / a
    serverless function works — the host's other Python packages
    can't reach into NeoGraph's C++ engine.
  - 컨테이너 이미지는 **alpine + musl + ~20MB**일 수 있습니다(engine .so +
    Python interpreter + 2 deps), or static-linked C++ binary at
    **~1.2 MB** with `libc.so.6` as the only dynamic dep.
  - 서버리스(Lambda, Cloud Run)의 콜드 스타트는 ms 클래스가 아니라
    seconds — there's no LangChain import graph to walk.
  - 잠금 파일 유지 관리 부담은 거의 0에 가깝습니다. `pydantic>=2.0`는
    the only constraint that could ever drift, and you'd see it at
    install time, not 3 AM in production.
