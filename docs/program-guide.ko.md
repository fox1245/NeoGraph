<!-- neograph-i18n: source=docs/program-guide.md locale=ko source_sha256=faed88529c59a389348b12b563d87661b14d1256ca7667e0f728c3529e6f7124 -->
# Program 가이드: 그래프를 조율하는 JavaScript

**Languages:** [English](program-guide.md) | [한국어](program-guide.ko.md) | [日本語](program-guide.ja.md) | [简体中文](program-guide.zh-CN.md)

그래프의 모양은 고정되어 있습니다. 노드, 엣지, 채널은 컴파일할 때 정해집니다. Program은 호스트가 컴파일하고, 승인(admission)하고, 실행하는 작은 JavaScript 모듈로, 그래프 한 단계 위에 놓입니다. Program은 자신이 소유한 그래프 하나를 돌려주는 `define()`을 내보내고, 선택적으로 `main()`을 내보냅니다. `main()`은 그 그래프를 어떻게 실행할지 정하는 제너레이터입니다. 한 번에 몇 개를 돌릴지, 결과를 어떻게 합칠지, 예산이 바닥나면 어떻게 할지를 `main()`이 결정합니다. 런타임은 `main()`의 모든 단계를 저널에 기록하므로, 실행은 사람을 기다리며 멈췄다가 이어질 수 있습니다.

이 페이지는 모델을 설명하고 Program 하나를 처음부터 끝까지 따라가 봅니다. 규칙은 [Python 바인딩 가이드](python-binding.md#program-and-quickjs)에, 모듈에 쓸 수 있는 것과 쓸 수 없는 것은 [작성 규칙](program-graphs.md#authoring-rules)에, 그래프 기능은 [Program 그래프 기능](program-graphs.md)에, 오류 코드는 [진단 가이드](diagnostics.md)에 있습니다.

## Program을 쓸 때

그래프 하나와 그 체크포인트로 충분하다면 평범한 그래프를 쓰세요. Program은 그래프 바깥의 일을 위한 것입니다. 여러 복사본을 병렬로 실행해 규칙(`ng.all`, `ng.race`, `ng.quorum`)으로 합치는 일, 실행 전체에 예산을 거는 일, 다른 프로세스에서 재개할 수 있게 실행을 기록하는 일, 모델이나 사용자가 쓴 코드를 호스트가 승인하게 하는 일이 여기에 속합니다. 이 JavaScript는 파일 시스템, 네트워크, 시계, 난수, `eval`, 동적 `import`, `Promise`, `async`에 접근할 수 없습니다. 할 수 있는 일은 그래프를 만들고 런타임에 명령을 `yield`하는 것뿐입니다. 이름을 쓰는 모든 노드 타입, 조건, 리듀서는 호스트가 만든 레지스트리에 있어야 하므로, 생성된 Program이 호스트가 등록하지 않은 코드를 실행할 수는 없습니다.

## 요구 사항

Program 계층에는 Program과 QuickJS 제어가 포함된 빌드가 필요합니다. `pip install neograph-engine`에 이미 들어 있고, 소스 빌드에는 `-DNEOGRAPH_BUILD_PYBIND=ON -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON`이 필요합니다(그다음 `pip install pydantic certifi`를 실행하고 빌드 디렉터리를 `PYTHONPATH`에 추가).

아래 예제는 실패 라우팅, `fork`, SQLite 상태 디렉터리를 씁니다. PyPI의 0.14.0 휠에는 이 기능들이 있고 0.13.1 휠에는 없으므로, 더 오래된 휠이라면 현재 소스로 빌드한 것으로 실행하세요. 이 기능을 쓰는 예제(Python 32, 35, 36)도 마찬가지입니다. 더 작은 것부터 시작하려면 [`29_program_hello.py`](../bindings/python/examples/29_program_hello.py)를 실행하세요. Program 하나를 100줄 안에 컴파일하고, 승인(admission)하고, 실행합니다. C++에서는 [`65_program_javascript.cpp`](../examples/65_program_javascript.cpp)에서 시작하세요.

## 실전 예제: 릴리스 점검

릴리스 작업이 서비스 세 개를 병렬로 점검합니다. `payments`의 점검이 실패합니다. 작업 전체를 실패시키는 대신, 실패는 검토 단계로 가고 작업은 사람을 기다립니다. 사람이 릴리스를 승인하면 나머지 서비스는 다시 점검하지 않고 작업이 끝납니다. 같은 작업이 예산 안에서 실행됩니다.

예제는 Python 파일 세 개입니다. `release_program.py`는 실행마다 바뀌지 않는 모든 것을 담고, `run_release.py`와 `restart.py`가 이를 가져다 씁니다.

아래 코드는 실패 라우팅, 검토를 위한 일시 중지, 예산, 재시작을 하나의 이야기로 묶었습니다. 저장소에는 이 부분을 하나씩 따로 보여 주는 실행 가능한 예제가 있습니다. [`32_program_failure_routing.py`](../bindings/python/examples/32_program_failure_routing.py)(실패 경로와 운영자의 `retry`/`abort`), [`34_program_pause_resume.py`](../bindings/python/examples/34_program_pause_resume.py)(`interruptBefore`와 `host.resume`), [`33_program_budgets.py`](../bindings/python/examples/33_program_budgets.py), [`35_program_fork.py`](../bindings/python/examples/35_program_fork.py), [`36_program_crash_recovery.py`](../bindings/python/examples/36_program_crash_recovery.py)입니다. C++로 짝을 이루는 예제는 [66](../examples/66_program_failure_routing.cpp)번부터 [69](../examples/69_program_crash_recovery.cpp)번까지입니다.

### 1. 노드 코드

노드는 평범한 Python입니다. `check`는 `payments`에서 실패합니다. 실패는 예외의 `code`, `public_message`, `retryable` 속성으로 스스로를 설명하며, 예외의 텍스트는 노드 밖으로 나가지 않습니다. `Review`는 사람이 답한 뒤에 실행되고, 그 답이 재개 값입니다.

```python
import hashlib

import neograph_engine as ng


# 1. The node code. Programs refer to these by name; they stay ordinary Python.

CHECKED = []   # which services ran their checks, to show that nothing runs twice

class ChecksFailed(Exception):
    code = "release.checks_failed"        # an author code: lower-case, dotted
    public_message = "the release checks failed"
    retryable = True


@ng.node("release.check")
def check(state):
    service = state.get("service")
    CHECKED.append(service)
    if service == "payments":
        raise ChecksFailed(f"{service}: 2 tests failed")
    return [ng.ChannelWrite("report", [f"{service}: checks passed"])]


class Review(ng.GraphNode):
    """Runs after a person answered; the answer is the resume value."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        answer = input.ctx.resume_value or {}
        return [ng.ChannelWrite("decision", answer.get("decision", "hold"))]


@ng.node("release.ship")
def ship(state):
    return [ng.ChannelWrite("report", [f"{state.get('service')}: shipped"])]


@ng.node("release.hold")
def hold(state):
    return [ng.ChannelWrite("report", [f"{state.get('service')}: held"])]


ng.NodeFactory.register_type("release.review", lambda name, config, ctx: Review(name))
ng.ConditionRegistry.register_condition(
    "release.decision", lambda state: state.get("decision") or "hold")
```

### 2. 레지스트리

레지스트리는 Program이 쓸 수 있는 실행 가능한 이름의 목록이며, 이름마다 버전과 다이제스트가 붙습니다. 호스트는 이 프로세스에 이미 등록된 노드, 조건, 리듀서로 레지스트리를 만듭니다.

```python
# 2. The registry: the only executable names a Program may use.

def digest(label):
    return "sha256:" + hashlib.sha256(label.encode()).hexdigest()


def build_registry():
    builder = ng.ProgramRegistryBuilder()
    for node_type in ("release.check", "release.review", "release.ship", "release.hold"):
        builder.add_registered_node(node_type, "1.0.0", digest(node_type))
    builder.add_registered_condition("release.decision", "1.0.0", digest("release.decision"))
    for reducer in ("append", "overwrite"):
        builder.add_registered_reducer(reducer, "1.0.0", digest(reducer))
    return builder.build()
```

### 3. Program

`define()`은 그래프를 기술합니다. `check`는 두 번 시도합니다(`max_retries: 1`). 그래도 실패하면 실행을 끝내는 대신 실패를 `review`로 보내고, 실패 기록을 `failures` 채널에 덧붙입니다. `review`는 `interruptBefore`로 붙잡혀 있으므로 실행은 거기서 멈춰 답을 기다립니다. 답이 오면 조건 `release.decision`이 실행을 `ship`이나 `hold`로 보냅니다. `hold`는 실패 종료 노드이며, 실행을 코드 `release.held`와 함께 `Failed`로 끝냅니다.

`main()`은 `ng.all`로 서비스마다 그래프를 한 번씩 실행하며 한 번에 최대 세 개까지 돌리고, 리포트를 서비스 순서대로 돌려줍니다.

```python
# 3. The Program: define() describes the graph, main() coordinates runs of it.

SOURCE = r'''
export function define() {
  const g = ng.graph("release");
  g.channel("service", {reducer: "overwrite", initial: ""});
  g.channel("report", {reducer: "append", initial: []});
  g.channel("failures", {reducer: "append", initial: []});
  g.channel("decision", {reducer: "overwrite", initial: ""});
  g.node("check", {type: "release.check"});
  g.node("review", {type: "release.review"});
  g.node("ship", {type: "release.ship"});
  g.node("hold", {type: "release.hold"});
  g.entry("check");
  g.edge("check", "ship");
  g.exit("ship");
  // Two attempts. Then the failure goes to "review" instead of failing the run.
  g.retryPolicy("check", {max_retries: 1, initial_delay_ms: 10, backoff_multiplier: 1,
                          max_delay_ms: 10, on_failure: "review", error_channel: "failures"});
  g.interruptBefore("review");               // a person answers before "review" runs
  g.conditionalEdge("review", "release.decision", {approve: "ship", hold: "hold"});
  g.failureExit("hold", {code: "release.held"});
  return g;
}

export function* main(input) {
  const results = yield ng.all(
    input.services.map((name) => ng.callCore("release", {service: name}, "release:" + name)),
    {max_in_flight: 3}, "fan-out");
  return results.map((result) => result.report);
}
'''
```

기억해 둘 규칙이 둘 있습니다. `ng.callCore`는 `define()`이 돌려주는 그래프만, `ng.graph()`에 준 이름으로 지정할 수 있습니다. 그리고 실패한 명령은 실행을 끝냅니다. 제너레이터가 재개되지 않으므로 `yield` 둘레의 `try`/`catch`는 실행되지 않습니다. 실패는 여기서 `retryPolicy`가 하듯 그래프에서 처리하거나, 호스트에서 `result.status`와 `result.failure`를 읽어 처리하세요.

### 4. 예산과 호스트

호스트는 승인하는 모든 Program에 상한을 둡니다. Program은 실행 예산 하나와 함께 승인되며, 그 버전의 모든 실행은 정확히 그 예산을 써야 합니다. 상한의 필드는 자식 한도 두 개를 제외하고, 모델을 호출하지 않는 Program이라도 양수여야 합니다.

```python
# 4. The budgets. The host ceiling bounds every Program; the run budget bounds one run.

def budget(**fields):
    value = ng.ProgramRunBudget()
    for name, number in fields.items():
        setattr(value, name, number)
    return value


ceiling = budget(wall_time_ms=120_000, model_tokens=1, monetary_microunits=1,
                 max_concurrency=8, max_program_operations=64, max_core_steps=2_000,
                 max_dynamic_compiles=1)
run_budget = budget(wall_time_ms=10_000, max_concurrency=6, max_program_operations=16,
                    max_core_steps=50)


def admit(host, run_budget):
    """Compile the Program, check it against the host's ceiling and return its version."""
    source = ng.ProgramSource.from_javascript("release.js", SOURCE)
    return host.compile_admit(source, run_budget)


def make_host(state_dir=None):
    host = ng.LocalProgramHost(build_registry(), "tenant:docs", ceiling,
                               sqlite_directory=state_dir)
    return host, admit(host, run_budget)
```

`compile_admit`은 소스를 컴파일하고, 번들이 상한을 넘지 않는지 확인한 뒤 `ProgramVersion`을 돌려줍니다. 소스, 레지스트리, 예산이 같으면 다른 프로세스에서도 버전 ID가 같으며, 테스트가 프로세스 간에 이를 확인합니다.

### 5. 실행하고 멈추기

`run_release.py`를 만듭니다.

```python
from release_program import CHECKED, admit, budget, make_host, run_budget

host, version = make_host()

# Run: "payments" fails its checks, so the run pauses for a person.

services = {"services": ["api", "payments", "web"]}
paused = host.run(version, services, run_budget)
print(paused.status)
print(paused.interrupt.core_node, paused.interrupt.value["failure"]["code"])
```

```text
ProgramTerminalStatus.Interrupted
review release.checks_failed
```

`api`와 `web`은 점검을 마쳤습니다. `payments`는 두 번 실패했으므로 그 실패가 `review`로 라우팅되었고, 실행은 상태 `Interrupted`로 멈췄습니다. `result.interrupt.core_node`는 기다리는 노드를 알려 주고, `result.interrupt.value`에는 `{"reason": "failure_route", "failure": {...}}`가 들어 있습니다. 실패 기록에는 `node`, `kind`(여기서는 `exhausted`), `code`, `attempts`, `retryable`, `step`, `routed_to`, `effects`가 있습니다. `api`와 `web`의 리포트는 이미 실행에 들어 있습니다.

### 6. 포크해서 다른 답 시험하기

```python
# Fork the paused run to see what "hold" would do. The original stays paused.
# The fork takes a share of the paused run's remaining budget.

fork_budget = budget(wall_time_ms=2_000, max_concurrency=3, max_program_operations=8,
                     max_core_steps=20)
forked = host.fork(paused.run_id, {"decision": "hold"}, version, fork_budget).wait()
print(forked.status, forked.failure.code)
```

```text
ProgramTerminalStatus.Failed release.held
```

`fork`는 멈춘 실행의 체크포인트에서 새 실행을 시작하고 질문에 `"hold"`로 답합니다. 포크는 실패 종료의 코드와 함께 `Failed`로 끝납니다. 원래 실행은 여전히 멈춰 있습니다. 끝난 작업은 다시 실행하지 않고 재생(replay)합니다.

포크의 예산은 원래 실행의 남은 예산에서 떼어 낸 몫입니다. `wall_time_ms`, `model_tokens`, `monetary_microunits`, `max_program_operations`, `max_core_steps`, `max_dynamic_compiles`는 양입니다. 포크는 원래 실행에 남은 양에서 자기 몫을 가져가고, 나머지는 원래 실행이 그대로 가집니다. 이 예에서 원래 실행에는 남은 `wall_time_ms`에서 2,000을 뺀 값과 남은 `max_core_steps`에서 20을 뺀 값이 남습니다. `max_concurrency`는 양이 아니라 한도입니다. 포크는 원래 실행의 한도 이하로 자기 한도를 받고, 원래 실행은 자기 한도를 그대로 유지하므로, 포크의 3은 원래 실행에서 아무것도 가져가지 않습니다. 포크 때문에 원래 실행의 `wall_time_ms`나 `max_core_steps`가 0이 되면 `fork` 호출이 `P_FORK_SOURCE_BUDGET`으로 거부하고, 원래 실행에 남은 양보다 큰 포크는 `P_FORK_BUDGET`으로 거부합니다. 어느 쪽도 원래 실행을 바꾸지 않습니다.

fork에서 다시 실행되는 노드는, 질문한 노드처럼, 질문하기 전에 한 provider 호출을 반복합니다. 영속적인 provider 호출 저널이 있으면 fork가 그 호출을 재생(replay)하므로 다시 전송되거나 과금되지 않습니다. `LocalProgramHost`는 `sqlite_directory`가 있을 때 이 저널을 가집니다. 모든 것을 메모리에 두는 호스트에는 저널이 없으므로, 그런 호스트의 fork는 해당 호출을 다시 전송하고 다시 과금합니다. 오류는 아니며 fork가 전송하고 과금하는 내용도 달라지지 않지만, fork가 이 사실을 알려 줍니다. 일시 중지된 시도가 일시 중지된 연산에서 provider 호출을 기록했다면, `fork`가 반환한 핸들의 `handle.notices`에 코드가 `P_FORK_PROVIDER_CALLS_NOT_JOURNALED`인 알림이 하나 들어 있고, 그 `witness`에 해당 호출(`call_id`, `node`, `model`, `microunits`)과 합계가 나열됩니다. 알림만으로는 그 가운데 fork가 다시 실행하는 호출이 무엇인지 알 수 없습니다. 일시 중지된 스텝에서 끝난 노드의 호출은 그 스텝에 저장된 쓰기에서 재생되며 다시 전송되지 않기 때문입니다. 저널이 있는 호스트의 fork나, 일시 중지된 시도가 그런 호출을 기록하지 않은 fork에는 알림이 없습니다. 알림은 실행과 함께 저장되지 않으므로 `reconnect`가 반환한 핸들에는 없습니다. 자세한 내용은 [Python 바인딩 가이드](python-binding.md#in-memory-state-and-resume)와 [가격 증거와 모델 카탈로그](reference-en.md#pricing-evidence-and-the-model-catalogue)를 참고하세요.

저널은 호출을 요청이 묻는 내용으로 식별하고 그 요청을 승인한 policy로는 식별하지 않으므로, fork는 SchemaProvider의 descriptor policy가 바뀐 뒤에도 호출을 재생합니다. 이때 핸들에 `P_PROVIDER_CALL_POLICY_CHANGED` 알림이 붙으며, `wait()` 뒤에 `handle.notices`로 볼 수 있습니다. 저널에 source의 호출이 있지만 0.15가 쓴 저널처럼 그 호출을 매칭할 수 없으면, fork는 호출이 다르다는 것을 보일 수 없습니다. 그러면 호출을 다시 전송하고 과금하는 대신 `NG.ConfigError`로 실패합니다. `LocalProgramHost(fork_may_resend_provider_calls=True)`는 재전송을 허용하며, 그때 핸들에 `P_FORK_PROVIDER_CALL_RESENT`가 붙습니다.

### 7. 답으로 재개하기

```python
# Resume the original with "approve". The other services do not run again.

done = host.resume(paused.run_id, {"decision": "approve"}).wait()
print(done.status)
print(done.output)
print(sorted(CHECKED))
```

```text
ProgramTerminalStatus.Completed
[['api: checks passed', 'api: shipped'], ['payments: shipped'], ['web: checks passed', 'web: shipped']]
['api', 'payments', 'payments', 'web']
```

`resume`은 대기 중인 질문에 답하고 같은 실행을 이어 갑니다. 사람이 승인했으므로 `payments`는 점검을 건너뛰고 출고됩니다. 마지막 줄은 아무것도 두 번 실행되지 않았음을 보여 줍니다. `api`와 `web`은 한 번씩, `payments`는 실패한 두 번의 시도로 두 번 나타납니다.

### 8. 너무 작은 예산

```python
# A budget that is too small refuses the join before any member starts.

small = budget(wall_time_ms=10_000, max_concurrency=6, max_program_operations=2,
               max_core_steps=50)
refused = host.run(admit(host, small), services, small)
print(refused.status, refused.failure.code)
```

```text
ProgramTerminalStatus.BudgetExhausted P_PROGRAM_OPERATION_BUDGET
```

이 예산(`max_program_operations=2`)이 허용하는 것보다 조인에 더 많은 Program 연산이 필요합니다. 런타임은 멤버를 하나도 시작하기 전에 조인 전체를 거절하므로, 어떤 서비스도 점검되지 않습니다. 예산을 바꾸려면 `compile_admit`을 다시 해서 새 버전을 만들어야 합니다. 승인된 예산과 다른 예산으로 `host.run`을 호출하면 `P_START_BUDGET`이 발생합니다.

### 9. 재시작을 견디기

기본적으로 `LocalProgramHost`는 모든 것을 메모리에 두며, `resume`은 실행을 시작한 호스트 객체를 통해서만 동작합니다. `sqlite_directory`를 주면 호스트가 실행, 버전, 체크포인트를 SQLite 파일에 보관하므로, 새 프로세스가 실행을 이어 갈 수 있습니다. 같은 레지스트리를 쓰고 같은 Program을 승인해야 합니다.

`restart.py`를 만듭니다.

```python
import sys

from release_program import make_host, run_budget

state_dir, step = sys.argv[1], sys.argv[2]
host, version = make_host(state_dir)

if step == "start":
    paused = host.run(version, {"services": ["api", "payments", "web"]}, run_budget)
    print(paused.status, paused.run_id)
else:
    done = host.resume(sys.argv[3], {"decision": "approve"}).wait()
    print(done.status, done.output)
```

```bash
python restart.py state start
python restart.py state resume run-...
```

첫 번째 명령은 상태 `Interrupted`와 실행 ID를 출력합니다. 그 ID를 두 번째 명령에 넘기면 `Completed`와 리포트를 출력합니다. `state` 디렉터리에는 `program.db`, `checkpoints.db`, `provider_calls.db`가 들어 있습니다.

프로세스가 일시 정지가 아니라 실행 중인 채로 남긴 실행은 `host.reconnect(run_id)`로 이어 갑니다. 어떻게 이어지는지는 Program에 따라 다릅니다. 자식 예산 없이 시작한 `define()`만 있는 Program(`max_child_depth`와 `max_total_children`이 모두 0인 기본값)은 `wall_time_ms` 동안 실행 lease를 잡습니다. 소유자가 아직 살아 있을 수 있는 실행을 다른 프로세스가 이어 가지 못하게 하기 위해서입니다. lease가 만료될 때까지 `reconnect`는 `P_RUN_LEASE_HELD`를 발생시키고, 만료된 뒤에는 실행을 이어 가지 않고 `TimedOut`으로 끝냅니다. `main()`이 있는 Program이나 자식 예산(둘 다 0보다 큼)으로 시작한 Program은 lease를 잡지 않으며, `reconnect`는 마지막 체크포인트에서 바로 이어 가면서 저널에 기록된 모델 호출을 재생하고 한 번만 과금합니다([`36_program_crash_recovery.py`](../bindings/python/examples/36_program_crash_recovery.py)).

## `define()`이 표현할 수 있는 것

`ng.graph(name)`은 빌더를 돌려줍니다. 모든 메서드는 빌더를 돌려줍니다.

| 메서드 | 의미 |
|---|---|
| `channel(name, {reducer, initial})` | 리듀서(`overwrite`, `append`, 또는 등록된 리듀서)가 붙은 상태 채널. |
| `node(name, {type, ...config})` | 등록된 타입의 노드. 나머지 키는 그 노드의 설정입니다. |
| `edge(from, to)`, `conditionalEdge(from, condition, routes)` | 고정 엣지와 경로가 갈리는 엣지. 조건은 등록된 이름입니다. |
| `entry(node)`, `exit(node)` | `__start__`와 `__end__`를 연결합니다. |
| `barrier(node, waitFor)` | 나열된 모든 노드, 또는 `{any_of: [...]}` 슬롯을 기다리는 조인. |
| `interruptBefore(...)`, `interruptAfter(...)` | 노드 앞이나 뒤에서 실행을 멈추고 답을 기다립니다. |
| `retryPolicy(node, {...})` | 재시도, 타임아웃, 그리고 `on_failure`가 있으면 실패 엣지. |
| `failureExit(node, {code})` | 실행을 그 코드와 함께 `Failed`로 끝내는 노드. |
| `metadata({...})` | 그래프에 대한 주장. 권한은 부여하지 않습니다. |

`ng.javascript_authoring_capability_manifest()`는 이 빌드가 받아들이는 어휘를 정확히 돌려주며, TypeScript 선언은 `dts_prelude`에 있습니다. 덜 쓰는 키는 [Program 그래프 기능](program-graphs.md)에서 설명합니다.

## `main()`이 `yield`할 수 있는 것

`yield`는 명령 하나를 런타임에 넘기고, 명령이 끝나면 그 결과를 받습니다.

| 명령 | 결과 |
|---|---|
| `ng.callCore(name, input, site)` | 그래프를 실행하고 출력을 돌려줍니다. 출력은 채널별 `value`와 `version`을 담은 `channels`이며, 각 채널의 값은 채널 이름의 키로도 들어 있습니다. |
| `ng.all(members, options, site)`(`ng.parallel`도 같음) | 멤버 명령을 `max_in_flight`개씩 동시에 실행하고 결과를 주어진 순서대로 돌려줍니다. 멤버 하나가 실패하면 실행이 실패합니다. |
| `ng.race(members, options, site)` | 가장 먼저 끝난 멤버의 결과를 돌려줍니다. |
| `ng.quorum(members, required, site)` | 필요한 수의 멤버가 성공하면 돌려줍니다. |
| `ng.await(command, timeoutMs, site)` | 명령 하나를 마감 시각까지 기다립니다. 마감이 지나면 실행은 `P_AWAIT_TIMEOUT`과 함께 `TimedOut`으로 끝납니다. |
| `ng.spawn(binding, input, site)` | 자식 Program을 시작합니다. 자식 바인딩 리졸버가 있는 C++ 호스트가 필요하며, `LocalProgramHost`에는 없어서 `P_CHILD_BINDING`으로 실패합니다. |
| `ng.emit(value, site)`, `ng.checkpoint(value, site)` | `value`를 실행 저널에 기록하고 그대로 돌려줍니다. 각각 Program 연산 하나로 셉니다. |
| `ng.cancelScope(scope, reason, site)` | 실행을 취소합니다(`scope`는 `"current"`). 실행은 `P_RUNTIME_CANCELLED`와 함께 `Cancelled`로 끝납니다. |

모든 명령의 마지막 인자는 호출 위치를 나타내는 레이블입니다. `ng.all`의 멤버 하나가 멈추면 실행이 멈춥니다. 나머지 멤버는 끝까지 실행되고, 실행은 `Interrupted`가 되며, `resume`이 멈춘 멤버에 답합니다.

## 상태

`ProgramResult.status`는 `ProgramTerminalStatus`입니다. `Completed`가 아니면 `result.failure`(`code`, `message`, `core_node`, `witness`)를 읽으세요.

| 상태 | 의미 |
|---|---|
| `Completed` | `main()`이 값을 반환했거나 그래프가 종료 노드에 도달했습니다. `result.output`에 그 값이 있습니다. |
| `Interrupted` | 실행이 답을 기다립니다. `result.interrupt`가 위치를 알려 줍니다. `resume`이나 `fork`로 이어 갑니다. |
| `Failed` | 그래프가 라우팅하지 않은 실패, 실패 종료, 또는 `main()`이 던진 예외. `result.failure.code`가 이를 알려 줍니다. |
| `BudgetExhausted` | 예산이 걸린 자원이 바닥났습니다. 예: `P_PROGRAM_OPERATION_BUDGET`, `P_CORE_STEP_BUDGET`, `P_CONCURRENCY_BUDGET`. 모델 토큰은 응답이 예산을 넘겼을 때도, 다음 호출이 전송 전에 거부되었을 때도 `NG.BudgetExhausted`로 끝납니다. 금액과 Core 스텝은 실행이 얼마나 넘겼는지 알려 주는 기록과 함께 끝나며([예산](#budgets)), 실행이 예산을 넘긴 채 끝난 경우도 같습니다. |
| `TimedOut` | 실행의 `wall_time_ms`나 `ng.await` 마감이 지났습니다. `wall_time_ms`가 지난 뒤에 끝난 실행도 `TimedOut`입니다([예산](#budgets)). |
| `Cancelled` | `ng.cancelScope`나 핸들의 `cancel()`이 실행을 멈췄습니다. |

`AmbiguousEffect`와 `CheckpointIncompatible`도 있습니다. `include/neograph/program/result.h`를 보세요.

<a id="budgets"></a>
## 예산

`ProgramRunBudget`에는 필드가 열 개 있습니다. `wall_time_ms`, `model_tokens`, `monetary_microunits`, `max_concurrency`, `max_program_operations`, `max_core_steps`, `max_dynamic_compiles`, `max_child_depth`, `max_total_children`, `max_node_retries`입니다. `result.usage`는 실행이 쓴 양을, `result.remaining_budget`은 남은 양을 알려 줍니다. 0은 아무것도 허용하지 않는다는 뜻입니다. `model_tokens = 0`인 실행이 모델을 호출하면 `BudgetExhausted`로 끝납니다. 상한, 승인된 예산, 모델 비용 규칙은 [Python 바인딩 가이드](python-binding.md#budgets)에 있습니다.

예산을 넘겨 끝난 실행에는 어느 예산인지 알려 주는 failure 기록이 있고, witness가 얼마나 넘겼는지 알려 줍니다. `failure.witness`에는 예산의 키로 `granted`, `used`, `over_by`가 들어 있습니다. 실행이 허용량에서 멈췄거나 모델 호출이 전송 전에 거부된 경우 `over_by`는 0입니다.

| 예산 | 상태 | `failure.code` | witness 키 |
|---|---|---|---|
| `wall_time_ms` | `TimedOut` | `P_RUNTIME_TIMEOUT` | `wall_time_ms` |
| `model_tokens` | `BudgetExhausted` | `NG.BudgetExhausted` | `model_tokens` |
| `monetary_microunits` | `BudgetExhausted` | `P_MONEY_BUDGET` | `monetary` |
| `max_core_steps` | `BudgetExhausted` | `P_CORE_STEP_BUDGET` | `core_steps` |

예산이 실행을 도중에 멈췄든 실행이 예산을 넘긴 채 끝났든 기록은 같습니다. 사용량은 plan이 끝난 뒤에 정산되므로 plan이 허용량을 넘겨 끝날 수 있습니다. 응답의 비용이 그 호출에 예약한 금액보다 클 수 있고, 취소를 무시하는 노드가 마감 뒤에 반환할 수 있으며, 서브그래프 노드의 스텝은 그 노드를 담은 그래프를 멈추지 않으면서 실행의 Core 스텝에 더해집니다. 실행이 예산 여러 개를 넘기면 witness에 각각의 항목이 들어가고, status와 코드는 표에서 가장 앞에 있는 예산(예산 필드의 순서)을 따릅니다. 마감은 다른 사용량과 상관없이 실행을 `TimedOut`으로 끝내므로 wall time이 맨 앞에 있습니다. 가격이 매겨지는 호출이 전송 전에 거부되면 거부의 증거가 `monetary` 아래에 남습니다. 예약액, `ceiling`, `used`입니다.

가격이 매겨지는 호출의 예약액은 호출의 출력 cap을 가장 비싼 출력 요율로 계산한 값이며, 모델의 upstream이 cap에서 멈출 때만 지출의 상한이 됩니다. NeoGraph는 cap을 보내지만 upstream이 그것을 적용하는지 확인할 수 없으므로, cap을 무시하는 upstream의 응답은 예약액보다 비쌀 수 있습니다. 그 호출은 응답이 보고한 금액으로 청구되고, 실행에 남은 `monetary_microunits`는 그만큼 줄며, 청구액이 허용량을 넘긴 실행은 위와 같이 `P_MONEY_BUDGET`과 함께 `BudgetExhausted`로 끝납니다. 허용량 안에서는 실행이 계속됩니다. NeoGraph는 그런 호출마다 실행 핸들에 `P_MONEY_OVER_HOLD` notice를 내고(`handle.notices`, `wait()` 뒤에 읽습니다), `result.monetary_records`에 있는 그 호출의 기록은 `microunits`가 `reserved_microunits`보다 큽니다. 둘 다 응답이 이름을 주면 호출을 처리한 업스트림(`served_by`)을 담습니다. notice는 다른 것을 바꾸지 않으며, NeoGraph는 notice 뒤에 route를 거부하지 않습니다. 그것은 호스트의 정책입니다. cap에 의존하는 호스트는 OpenRouter 호출을 cap을 지킨다고 아는 upstream으로만 보내고(provider routing의 `only`나 `order`), 실행마다 notice와 기록에 대응하고, 잃어도 감당할 수 있는 금액보다 적게 부여할 수 있습니다. 자세한 내용은 [가격 증거와 모델 카탈로그](reference-en.md#pricing-evidence-and-the-model-catalogue)의 출력 cap 단락에 있습니다.

## Program의 작업이 실행되는 곳 (C++ 호스트)

C++ 호스트는 `RuntimeConfig`로 `ProgramRuntime`을 만듭니다. 호스트가 달리 지정하지 않으면 런타임은 attempt를
`scheduler_threads`개 스레드의 풀에서, 데드라인 타이머를 스레드 하나에서 실행합니다. `LocalProgramHost(scheduler_threads=...)`가
설정하는 것이 이것입니다. 자체 executor가 있는 호스트는 `RuntimeConfig::executor`를 설정합니다. 단일 스레드 `io_context`,
다른 작업과 공유하는 `thread_pool`, WebAssembly 호스트라면 `async::HostLoop::executor()`입니다. 그러면 런타임은 스레드를
시작하지 않으며, 모든 attempt, 데드라인 타이머, 대기자가 그 executor에서 실행되고, 어느 것도 시작한 호출 안에서 인라인으로
실행되지 않습니다.

- **모든 연산에 awaitable 짝이 있습니다.** `start_async`, `reconnect_async`, `resume_handle_async`, `fork_async`,
  `replace_async`, `migrate_graph_async` 등은 런타임의 executor에서 실행되고 기다리는 코루틴의 executor에서 완료됩니다.
  `run_async`와 `resume_async`는 결과를 기다립니다.
- **동기 호출과 `ProgramHandle::wait`는 거부될 수 있습니다.** 호스트의 이벤트 루프가 구동하는 executor와 스레드가 없는
  빌드에서는 Program을 실행해야 하는 스레드를 막게 되므로, 아무 일도 하기 전에 `ProgramBlockingCallRefused`를 던집니다.
  `operation()`은 호출을, `alternative()`는 awaitable 형태를 알려 줍니다. 주입된 executor를 돌리는 스레드에서 한 대기도
  거부됩니다. executor가 없는 네이티브 런타임은 바뀌지 않습니다.
- **호스트가 executor를 주었다면 노드 본문은 자기 스레드를 막으면 안 됩니다.** 자기 워커가 필요한 노드 attempt는 그
  executor의 strand에서 실행되며, 막히는 본문은 그 executor를 공유하는 모든 run을 붙잡습니다.
- **런타임을 소멸시키면** 실행 중인 run이 취소되며, 주입된 executor에서는 끝나기를 기다리지 않습니다. 핸들이 종료 상태가 될
  때까지 executor를 계속 돌리십시오.

실행할 수 있는 점검이 있는 WebAssembly 쪽은 [`wasm/README.md`](../wasm/README.md)에 있습니다.

## Program의 상태가 보관되는 곳 (C++ 호스트)

`ProgramRuntime`은 run 기록을 `ProgramTransitionStore`에, 버전을 `ProgramStore`에 보관하고, 호스트가 쓰는 경우 작업 그래프
조각, 연결된 모듈, Core 도구 grant를 각각 `TaskGraphFragmentStore`, `ModuleStore`, `ProgramCoreToolGrantStore`에 보관합니다.
이 인터페이스들은 `asio::awaitable`을 반환하므로, 호스트는 호출이 나중에 완료되는 데이터베이스 위에 저장소를 둘 수 있고,
런타임은 스레드를 붙잡는 대신 이를 await합니다.

- **동기 백엔드는 해당하는 `Sync...Store` 클래스에서 상속합니다**(`SyncProgramTransitionStore` 등 다섯 개). 그리고 `..._sync`
  연산(`load_sync`, `compare_publish_sync` 등)을 구현합니다. 기반 클래스는 이로부터 각 awaitable 연산을 완료시킵니다.
  호출한 스레드에서 즉시 완료하거나, 저장소가 `offloads()`를 true로 재정의하면 제한된 풀의 스레드에서 완료합니다.
  인메모리와 SQLite 저장소는 즉시 완료하고, PostgreSQL은 풀로 넘기며, 기다리던 코루틴은 자기 executor에서 재개됩니다.
  await하는 호출의 인자는 string view를 포함해 호출이 완료될 때까지 살아 있어야 합니다.
- **중단할 수 없는 호출자는 블로킹 뷰를 씁니다.** `BlockingProgramTransitionStore`와 나머지 넷은 인터페이스가 전에 갖던
  시그니처를 가집니다. 동기 저장소 위의 뷰는 그 저장소의 `..._sync` 연산을 호출합니다. 다른 저장소 위에서는 호출한
  스레드의 전용 `io_context`에서 awaitable을 돌려 기다립니다. 다만 `BlockingStorePolicy::refuse`가 기다리면 호출을 완료해야 할
  스레드가 멈춘다고 하면 거부합니다. 런타임의 정책은 호스트가 구동하는 executor와 자기 executor를 돌리는 스레드에서
  거부하며, 이때 호출은 저장소에 닿기 전에 `ProgramBlockingCallRefused`를 던집니다.
- **무엇이 await하고 무엇이 기다리는가.** 런타임의 모든 연산은 자기 저장소를 await하는 코루틴 하나입니다. 진입점(`start`, `reconnect`,
  `resume`, `fork`, `replace` 등), run의 종료 게시, 자식 시작과 그 복구, awaitable 쪽일 때의 Core 도구 grant 리졸버
  (`RuntimeConfig::core_tool_grant_awaitable_resolver`, 저장소로부터 `make_durable_core_tool_grant_awaitable_resolver`가 만듭니다),
  `ProgramCatalog`(`resolve_version_async`, `admit_async` 등 `_async` 형태), `ModuleResolver::resolve_async`가 그렇습니다.
  `_async` 형태는 그 본문을 런타임의 executor에서 실행합니다. 동기 형태는 같은 본문을 호출한 스레드에서 끝까지 실행하며,
  그 스레드가 멈출 곳에서는 거부됩니다. 따라서 호스트 이벤트 루프의 이후 턴에서만 완료되는 저장소가 호스트가 구동하는
  executor에서 `_async` 형태로 동작하고(`ProgramHandle::cancel_async`, `snapshot_async`, `latest_handoff_async`,
  `next_handoff_async` 포함), 그 루프의 턴 안에서는 동기 형태가 거부됩니다. 아직 호출자를 막는 것은 `ProgramHandoff::reference()`,
  `RuntimeInstructionController`, `inspect_program_replacement_chain`, `hook_runtime`이며, 런타임은 `graph::CheckpointStore`를
  그 저장소의 동기 호출로 읽습니다.
- **awaitable을 버리는 호출은 아무 일도 하지 않습니다.** 연산은 `[[nodiscard]]`입니다. `co_await`하거나 블로킹 뷰를 쓰십시오.
- **계약**(원자적 게시, 헤드 compare-and-swap, fail-closed)은 모든 저장소가 지켜야 하며, `tests/test_program_transition_store.cpp`와
  `tests/test_program_catalog.cpp`의 적합성(conformance) 스위트가 인메모리, SQLite, PostgreSQL 저장소에 대해 각 형태로 한 번씩 실행합니다.

## 동작을 검증하는 테스트

예제의 각 단계는 `bindings/python/tests/program_e2e`의 시나리오에 대응합니다. `test_07_main_programs.py`(`ng.all`, `race`, `quorum`, `await`), `test_10_failure_routing.py`(실패 경로와 승인), `test_02_hitl.py`(인터럽트와 재개, 메모리에서와 재시작 후), `test_04_budgets.py`(예산), `test_13_fork_replays_the_paused_step.py`와 `test_20_javascript_fork.py`(포크)입니다. [그 디렉터리의 README](../bindings/python/tests/program_e2e/README.md)에 모든 시나리오가 나와 있습니다. 예제에서 다루지 않은 Hook, provider 호출, 자식 Program은 [Python 바인딩 가이드](python-binding.md)와 [재귀적 Harness](PROGRAM_RECURSIVE_HARNESSES.md)에서 다룹니다.
