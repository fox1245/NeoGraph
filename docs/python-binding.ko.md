<!-- neograph-i18n: source=docs/python-binding.md locale=ko source_sha256=61dd8227b6a8807710fb014cacdf14a64257a18b35778a30981f34bd1eefb35f -->
# Python 바인딩

**Languages:** [English](python-binding.md) | [한국어](python-binding.ko.md) | [日本語](python-binding.ja.md) | [简体中文](python-binding.zh-CN.md)

`neograph-engine`는 동일한 C++ 런타임의 pybind11 표면입니다. 휠은 Core, LLM, Program/QuickJS, MCP 및 SQLite 런타임 지속성을 활성화합니다. 선택적 소스 빌드는 컴파일하는 구성 요소만 노출합니다.

```bash
pip install neograph-engine
```

## Core 그래프 빠른 시작

```python
import neograph_engine as ng

@ng.node("greet")
def greet(state):
    return [ng.ChannelWrite("messages", [
        {"role": "assistant", "content": f"Hello, {state.get('name')}!"}
    ])]

definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "demo",
    "channels": {
        "name": {"reducer": "overwrite"},
        "messages": {"reducer": "append"},
    },
    "nodes": {"greet": {"type": "greet"}},
    "edges": [
        {"from": ng.START_NODE, "to": "greet"},
        {"from": "greet", "to": ng.END_NODE},
    ],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())
result = engine.run(ng.RunConfig(thread_id="t1", input={"name": "NeoGraph"}))
print(result.output["channels"]["messages"]["value"])
```

## Core API 패리티

Python은 별도의 Python 스케줄러가 아닌 C++ 실행 기능을 노출합니다:

- 동기 및 asyncio run/stream/resume;
- 정확한 체크포인트 `resume_from`, 포크, 상태 검사 및 순서화된 상태 쓰기;
- 그래프 인터럽트 및 `NodeInterrupt`를 통한 정적 및 동적 HITL;
- `RunMetadata` 데드라인, 추적/실행 ID 및 모델 토큰 상한;
- 그래프 전체 및 노드별 `RetryPolicy`, 지터 포함;
- 실행 로컬 또는 명시적으로 재사용 가능한 `CacheScope`;
- 체크포인트 및 장기 Store 백엔드;
- 사용자 정의 노드, 리듀서, 조건, 공급자 및 도구;
- 도구 게이트, 실행 정책, 필수 수명주기 Hook 및 엄격한 런타임 개입.

### 패리티 계약

여기서 “패리티”는 Python이 동일한 네이티브 실행 경로와 안전 계약을 사용한다는 뜻입니다. 모든 내부 C++ 저장소나 권한 타입을 Python에 그대로 복제한다는 뜻은 아닙니다.

| 기능 | 네이티브 C++ 경로 | Python 표면 | 상태 |
|---|---|---|---|
| Core 그래프 컴파일 및 실행 | `GraphEngine` | `GraphEngine.compile`, run/stream/async 메서드 | 동일한 스케줄러와 런타임 |
| 런타임 ID, 데드라인 및 예산 | `RunMetadata`, `RunConfig` | `RunMetadata`, `RunConfig.model_token_budget` | 실행별 동일한 값 |
| 재시도 및 노드 캐시 정책 | `RetryPolicy`, `CacheScope` | 그래프/노드 setter 및 캐시 범위 | 동일한 런타임 정책 |
| 체크포인트, HITL 및 시간 이동 | 체크포인트 Store 및 재개 API | 재개, 정확한 `resume_from`, 포크, 상태 이력/갱신 | 동일한 체크포인트 계약 |
| Program 작성 및 로컬 실행 | 컴파일러, Catalog 및 `ProgramRuntime` | `ProgramCompiler`, `LocalProgramHost`, 핸들/결과 | 네이티브 소유자 범위 편의 호스트 |
| 필수 수명주기 Hook | 레지스트리, 실행기 및 `HookRuntime` | 정의와 `create_hook_runtime` 콜백 | 동일한 실패 시 차단 수명주기 경계 |
| 런타임 컨텍스트 및 엄격한 디스패치 | 컨텍스트 Store, 영수증 및 개입 | 대응하는 불변 값, Store 및 `StrictRuntimeProfile` | 동일한 네이티브 컨트롤러 |
| 영속성 휠 기본값 | SQLite Core/컨텍스트/디스패치 Store | `_HAVE_SQLITE` 내보내기 | PyPI 휠에서 활성화 |

원시 `ProgramCatalog`, 전환 Store, 교체/마이그레이션 컨트롤러, 합성 게이트웨이, Hook 저널 및 RPC 실행기는 호스트 조합 API로 남습니다. 권한을 가진 이 경로들의 일부만 노출하면 필수 `proposal -> compile -> admit -> publish -> migrate/spawn` 프로토콜을 우회하게 됩니다. 향후 Python 호스트 컨트롤러는 이 프로토콜과 비갱신 계보 예산을 하나의 소유자 범위 단위로 묶어야 합니다. `_HAVE_PROGRAM`은 원시 제어 평면 관리 패리티를 주장하지 않습니다.

### 런타임 재시도 재정의

```python
policy = ng.RetryPolicy()
policy.max_retries = 3
policy.initial_delay_ms = 100
policy.backoff_multiplier = 2.0
policy.max_delay_ms = 2_000
policy.jitter_pct = 0.2

engine.set_retry_policy(policy)
engine.set_node_retry_policy("remote_call", policy)
```

그래프 정의의 `"retry_policy"`는 선언적 기본값으로 유지됩니다. 런타임 세터는 별도의 C++/Python 구성 표면입니다.

### 메타데이터 및 정확한 재생(replay)

```python
config = ng.RunConfig(thread_id="job-42", input={"task": "..."})
config.model_token_budget = 20_000
metadata = ng.RunMetadata(
    timeout_ms=30_000,
    trace_id="trace-42",
    run_id="run-42",
    owner_scope="tenant-a",
)
result = engine.run(config, metadata)

# Never substitutes a newer checkpoint:
result = engine.resume_from(config, checkpoint_id, {"approved": True}, metadata)
```

Python 노드 내부에서는 동일한 값들이 `input.ctx.trace_id`, `run_id`, `has_deadline`, `deadline_remaining_ms`, `model_token_budget`를 통해 사용할 수 있습니다.

### 캐시 범위

```python
engine.set_node_cache_enabled("pure_parser", True)  # execution-local default
engine.set_node_cache_enabled("pure_parser", True, ng.CacheScope.Reusable)
```

`Reusable`는 노드가 테넌트, 제공자, Store, 도구, 자격 증명, 시간 및 재생(replay) 상태와 무관하다는 명시적 주장입니다.

## Program 및 QuickJS

Python 휠 빌드 `neograph::program` 및 제한된 QuickJS 프론트엔드. Python으로 정의된 노드는 변경 불가능한 Program 레지스트리에 참여하고 네이티브 `ProgramRuntime`를 통해 실행할 수 있습니다.

```python
import neograph_engine as ng

registry = (
    ng.ProgramRegistryBuilder()
    .add_registered_node(
        "my_node", "1.0.0", "sha256:" + "1" * 64
    )
    .add_registered_reducer(
        "overwrite", "1.0.0", "sha256:" + "2" * 64
    )
    .build()
)

source = ng.ProgramSource.from_javascript("agent.js", r'''
export function define() {
  const graph = ng.graph("main");
  graph.channel("value", {reducer: "overwrite", initial: 0});
  graph.node("work", {type: "my_node"});
  graph.entry("work");
  graph.exit("work");
  return graph;
}
export function* main(input) {
  return yield ng.callCore("main", input, "python:main");
}
''')

ceiling = ng.ProgramRunBudget()
ceiling.wall_time_ms = 10_000
ceiling.model_tokens = 1_000
ceiling.monetary_microunits = 1_000
ceiling.max_concurrency = 2
ceiling.max_program_operations = 32
ceiling.max_core_steps = 20
ceiling.max_dynamic_compiles = 1

run_budget = ng.ProgramRunBudget()
run_budget.wall_time_ms = 10_000
run_budget.max_concurrency = 2
run_budget.max_program_operations = 32
run_budget.max_core_steps = 20

host = ng.LocalProgramHost(registry, "tenant-a", ceiling)
version = host.compile_admit(source, run_budget)
result = host.run(version, {}, run_budget)
```

`LocalProgramHost`는 소유자 범위의 인메모리 편의 호스트입니다. 여전히 C++ 컴파일러, Catalog, 승인(admission) 정책, 전환 저장소 및 ProgramRuntime을 사용합니다. 생성된 제안은 승인(admission) 전에 호스트 의미 검증을 추가로 통과해야 합니다. [DSL 기능 평가](DSL_CAPABILITY_EVAL.md)를 참조하십시오.

정확히 설치된 JavaScript 어휘는 dict로 사용할 수 있습니다:

```python
manifest = ng.javascript_authoring_capability_manifest()
```

## 필수 수명 주기 Hooks

Hook은 모델이 도구를 호출하기로 결정하는 것이 아니라 호스트 수명 주기 이벤트에 의해 트리거됩니다.

```python
data = ng.HookDefinitionData()
data.phase = ng.HookPhase.CheckpointPublished
data.target_id = "audit"
data.delivery = ng.HookDelivery.BlockingMandatory
data.failure_mode = ng.HookFailureMode.FailClosed
data.effect = ng.ToolEffectClass.ReadOnly

mapper = ng.HookInputMapper()
mapper.kind = ng.HookInputMapperKind.Template
mapper.value_template = {"kind": "checkpoint"}
data.input_mapper = mapper

definition = ng.HookDefinition.create(data)
runtime = ng.create_hook_runtime(
    [definition],
    {"audit": lambda arguments, event_type, event_data: persist(arguments)},
)
engine.set_hook_runtime(runtime)
```

`FailClosed` 하의 콜백 실패는 보호된 런타임 경계를 차단합니다. `Continue`는 관찰 손실이 허용 가능한 경우에만 사용할 수 있습니다.

## 런타임 컨텍스트, Skill 및 엄격한 디스패치

바인딩은 변경 불가능한 RAW 기록, 컨텍스트 아티팩트, 에포크, 필수 Skill/제약 조건, 변환 영수증 및 제공자 디스패치 영수증을 노출합니다.

```python
requirements = ng.RuntimeContextRequirements()
requirements.required_artifact_ids = [skill.id, constraint.id]
requirements.required_skill_artifact_ids = [skill.id]

assembler = ng.RuntimeTurnAssembler(
    context_store,
    max_input_tokens=32_000,
    requirements=requirements,
)
```

`ContextTransformReceipt`는 임의의 파생 증거를 허용하지만 모든 필수 아티팩트가 바이트 단위로 동일하게 유지되도록 요구합니다.

전체 엄격 경로에는 지속형 SQLite 저장소를 사용하십시오:

```python
contexts = ng.SQLiteContextStore("runtime.sqlite3")
receipts = ng.SQLiteProviderDispatchReceiptStore("runtime.sqlite3")
hooks = ng.create_hook_runtime(definitions, callbacks)

profile = ng.StrictRuntimeProfile(
    provider,
    contexts,
    receipts,
    hooks,
    provider_binding_identity,
    max_input_tokens=32_000,
    required_context_artifact_ids=[constraint.id],
    required_skill_artifact_ids=[skill.id],
)
profile.activate("tenant-a", strict_epoch)
completion = profile.invoke(params)
profile.attach(engine)
```

## HITL 및 상태

정적 `interrupt_before`/`interrupt_after`, 동적 `NodeInterrupt`, 동기식 `resume`, asyncio `resume_async`, 그리고 정확한 `resume_from`은 체크포인트 저장소를 필요로 합니다.

```python
if result.interrupted:
    result = engine.resume(result_thread_id, {"approved": True})
```

검사 및 시간 이동(time-travel)에는 `get_state_history`, `update_state`, `fork`를 사용하십시오. `get_state_view()`는 평면적인 Pydantic 기반 채널 접근을 제공하는 반면, `get_state()`는 표준 중첩 표현을 유지합니다.

## 비동기 및 취소

`run_async`, `run_stream_async`, `resume_async`는 `asyncio.Future` 객체를 반환합니다. Future를 취소하면 `CancelToken`를 통해 진행 중인 네이티브 I/O로 전파됩니다. 스트리밍 콜백은 호출자의 asyncio 루프 스레드로 다시 마샬링됩니다.

Python 정의 공급자는 동기식 `complete`/`complete_stream`를 구현합니다; 비동기 네이티브 공급자 구현은 C++ 확장으로 유지됩니다.

## 프로토콜 및 관찰 가능성

- MCP 클라이언트 도구는 빌드 시 `neograph_engine.mcp`를 통해 사용할 수 있습니다.
- A2A 클라이언트 유형은 빌드 시 `neograph_engine.a2a`를 통해 사용할 수 있습니다.
- `ProtocolHostAdapter`는 공식 Python A2A/ACP 서버 SDK를 NeoGraph 세션 의미론과 통합합니다.
- `neograph_engine.tracing` 및 `neograph_engine.openinference`는 Phoenix, Langfuse, Arize 및 호환 백엔드를 위한 공급업체 중립적 OTel/OpenInference 데이터를 내보냅니다.

## 선택적 구성 요소

공개 패키지는 선택적 C++ 구성 요소를 정직하게 표시합니다:

- `_HAVE_PROGRAM`, `_HAVE_SQLITE`, `_HAVE_POSTGRES`, `_HAVE_MCP`, `_HAVE_A2A`;
- 누락된 구성 요소는 Python에서 에뮬레이션되지 않고 존재하지 않습니다;
- PyPI 휠은 Program/QuickJS, LLM, MCP 및 SQLite를 활성화합니다; 소스 빌드는 CMake 옵션을 따릅니다.

## 테스트 및 예제

바인딩 스위트는 Core 실행, 사용자 정의 콜백, asyncio, 취소, Program 컴파일/런타임, 필수 Hook, 엄격한 컨텍스트, SQLite 지속성, 프로토콜 및 README 예제를 다룹니다.

- [Python 예제](../bindings/python/examples/README.md)
- [C++ 예제](../examples/README.md)
- [QuickJS 작성 경계](QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)
- [엄격한 런타임 인터포지션](STRICT_RUNTIME_INTERPOSITION.md)
