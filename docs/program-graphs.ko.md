<!-- neograph-i18n: source=docs/program-graphs.md locale=ko source_sha256=ba8de002d040f5f18052d1e8d7971509fa764fcf24fb3859a39cf510c181c2c2 -->
# Program 그래프 기능

**Languages:** [English](program-graphs.md) | [한국어](program-graphs.ko.md) | [日本語](program-graphs.ja.md) | [简体中文](program-graphs.zh-CN.md)

이 페이지는 `define()` 프로그램이 노드, 엣지, barrier, 그래프 단위 재시도 정책 외에 사용할 수 있는 그래프 기능을 설명합니다. 각 기능은 엄격한 Core 정의(`schema_version` 1)의 키이며, `javascript_authoring_capability_manifest()["features"]`에 플래그가 있습니다. 기능을 쓰지 않는 정의는 그 기능이 생기기 전과 똑같은 바이트, `definition_hash`, 번들 id를 유지합니다.

이 키 중 하나라도 쓰는 정의는 번들에 검증기 경고를 봉인하지 않습니다. 경고는 컴파일 결과로 계속 반환됩니다. [진단 가이드](diagnostics.md)를 참고하세요.

## 그래프 메타데이터

플래그: `graph.metadata`.

```js
export function define() {
  return ng.graph("plan")
    .metadata({owner: "agent:planner", requirements: ["REQ-12"]})
    .node("draft", {type: "my.task"})
    .entry("draft").exit("draft");
}
```

`.metadata(object)`는 그래프마다 한 번만 호출할 수 있습니다. Core 정의는 이를 최상위 `"metadata"` 객체로 저장하며, 빈 객체는 없는 것과 같습니다.

- **한계.** 정규 JSON으로 최대 8 KiB, `metadata` 아래 중첩 깊이 4, 객체 멤버 합계 64개, 키 길이 1~128바이트입니다. `neograph.`로 시작하는 최상위 키는 예약되어 있습니다.
- **텍스트.** 키와 문자열에는 보이지 않는 문자가 들어갈 수 없습니다. 탭, 줄바꿈, 캐리지 리턴 이외의 C0 제어 문자, bidi 제어 문자 U+061C, U+200E, U+200F, U+202A~U+202E, U+2066~U+2069, 폭이 없는 문자 U+200B~U+200D, U+2060, U+FEFF, 그리고 태그 문자 U+E0000~U+E007F가 해당됩니다.
- **아이덴티티.** 메타데이터는 정의의 일부이므로 `definition_hash`와 번들 id를 바꿉니다. 키 순서는 영향을 주지 않습니다.
- **읽는 쪽.** 봉인된 정의에서 호스트만 읽습니다. C++에서는 `bundle.sealed_core_definitions()[0].definition["metadata"]`, Python에서는 `bundle.sealed_core_definitions[0]["definition"]["metadata"]`이며, 예를 들어 `LocalProgramHost.compile()`이 반환한 번들에서 읽습니다. 노드 팩토리, 실행 상태, 체크포인트, 출력, 이벤트는 메타데이터를 보지 않습니다.
- **권한.** 메타데이터는 작성자의 주장을 기록할 뿐 아무 권한도 주지 않습니다. 위임 한계는 호스트가 승인한 `RunBudget`(`max_child_depth`, `max_total_children`, `max_concurrency`)에 그대로 남습니다.
- **마이그레이션.** 메타데이터는 `GraphSemanticMigrationAdapter`의 토폴로지 형태에 포함되지 않으므로, 바꿔도 마이그레이션을 막지 않습니다. `metadata_changes()`는 승인하는 호스트를 위해 추가, 삭제, 변경된 포인터를 나열합니다.

오류: 두 번째 호출이나 객체가 아닌 인수에는 `P_JS_GRAPH_ARGUMENT`, 그리고 `P_CORE_GC_FIELD_TYPE`, `P_CORE_GC_METADATA_LIMIT`, `P_CORE_GC_METADATA_TEXT`, `P_CORE_GC_FIELD_VALUE`(빈 키나 예약된 키)가 있습니다.

## any_of 조인 슬롯

플래그: `barrier.any_of`.

```js
graph.conditionalEdge("B", "t.check", {pass: "D", fail: "FIX"})
  .edge("FIX", "D").edge("C", "D")
  .barrier("D", [{any_of: ["B", "FIX"]}, "C"]);
```

`barrier(node, waitFor)`의 항목은 노드 이름이거나 슬롯 `{any_of: [...]}`입니다. 조인은 여전히 슬롯 단위의 AND 조인입니다.

- 슬롯은 대안 중 하나가 조인에 신호를 보내면 채워집니다. 모든 슬롯이 채워지면 조인이 실행되고, 일반 barrier와 마찬가지로 모든 신호가 지워집니다.
- 슬롯에 속하지 않은 노드의 신호는 기록되지만 절대 계산되지 않습니다. `Command.goto`와 `Send`는 지금처럼 조인을 우회합니다.
- 두 대안이 같은 super-step에 도착하거나 조인이 실행되기 전에 도착하면 아무것도 바뀌지 않습니다. 조인이 실행된 뒤에 도착한 대안은 결코 끝나지 않는 새 라운드를 열기 때문에 실행은 `P_CORE_BARRIER_STALLED`로 끝납니다(안전한 실패). 따라서 대안들은 서로 배타적으로 라우팅되어야 하며, 두 대안이 모두 실행될 수 있으면 `P_CORE_E17`이 경고합니다.
- Core 정의에서 슬롯은 이름을 UTF-8 바이트 순으로 정렬한 `{"any_of": [...]}`로 출력되며, 슬롯은 가장 작은 이름 순으로 정렬됩니다. 이름이 하나인 슬롯은 그 이름 그대로 출력되므로 `[{any_of: ["B"]}, "C"]`는 `["B", "C"]`와 바이트와 id가 같습니다.
- 멈춘 슬롯 조인의 witness는 슬롯을 나열합니다. `wait_for`와 `missing`에는 이름과 `{"any_of": [...]}` 객체가 들어갑니다.
- 검증기는 슬롯마다 검사합니다. 어떤 대안도 조인으로 라우팅하지 않으면 `P_CORE_E8`, 멤버가 다른 곳으로 라우팅할 수 있으면 `P_CORE_E15`를 냅니다. 같은 슬롯의 다른 대안으로 가는 라우트는 우회가 아닙니다.

오류: 다른 형태의 항목에는 `P_JS_GRAPH_ARGUMENT`, 비었거나 문자열이 아닌 `any_of`, `any_of` 옆의 다른 키, 중첩된 `any_of`, 슬롯 안이나 슬롯 사이에서 반복된 노드에는 `P_CORE_GC_BARRIER`가 납니다.

## Witness 경로와 라우트 표

- **우회 경로.** `P_CORE_E15` witness에는 `paths`가 있습니다. 우회 라우트마다 멤버에서 그 라우트를 거치는 라우트 순서이며, 다시 조인으로 돌아오면 조인에서 끝납니다. 돌아오는 경우 조인 직전의 노드가 멤버를 대신할 수 있으므로 witness에 `substitute`와 `suggested_wait_for`가 추가되고, 반환된 진단은 `wait_for`를 그것으로 바꾸도록 제안합니다(적용 가능성 `maybe`). #357 형태라면 제안은 `[{"any_of": ["B", "FIX"]}, "C"]`입니다. 이미 다른 슬롯을 채우는 노드는 제안하지 않습니다.
- **멈춤 경로.** 멈춘 조인의 witness(`output["_neograph"]["barrier_stalled"]`, 그리고 Program 실패의 `stalled_barriers`)에는 `paths`가 추가됩니다. 실행된 누락 멤버마다 조인에 신호를 보낸 노드까지의 라우트이며, 트레이스와 라우팅된 후속 노드 맵에서 구합니다. 해당하는 경우 같은 `suggested_wait_for`도 들어갑니다.
- **라우트 표.** `ProgramCompilerConfig::echo_routes`(Python: `ProgramCompiler(..., echo_routes=True)`)는 `P_CORE_ROUTES` 노트를 반환합니다. 이 노트의 `witness.routes`는 각 노드를 스케줄러가 라우팅할 수 있는 대상, 즉 첫 번째 conditional edge의 라우트나 그렇지 않으면 정적 edge에 대응시킵니다. 봉인되지 않으므로 어느 쪽이든 번들은 같습니다.

## 실패 라우팅

플래그: `retry_policy.per_node`, `failure_edges`, `failure_exits`.

```js
ng.graph("fix")
  .channel("failures", {reducer: "append", initial: []})
  .node("IMPLEMENT", {type: "t.task"}).node("TEST", {type: "t.task"})
  .node("ASK_USER", {type: "t.review"}).node("ABORT", {type: "t.task"})
  .entry("IMPLEMENT").edge("IMPLEMENT", "TEST").exit("TEST")
  .conditionalEdge("ASK_USER", "t.decision", {retry: "IMPLEMENT", abort: "ABORT"})
  .retryPolicy("TEST", {max_retries: 2, non_retryable: ["app.compile_error"], max_visits: 3,
                        on_failure: "ASK_USER", error_channel: "failures"})
  .interruptBefore("ASK_USER")
  .failureExit("ABORT", {code: "plan.user_abort"});
```

TEST는 세 번 시도됩니다. 그래도 실패하면 실행은 실패하지 않습니다. TEST는 ASK_USER로 라우팅되고, ASK_USER는 실패 레코드를 들고 사람의 답을 기다리며 멈춥니다. `retry`라고 답하면 IMPLEMENT와 TEST가 다시 실행되고, `abort`라고 답하면 failure exit인 ABORT가 실행되어 실행은 `plan.user_abort`로 `Failed`로 끝나며 결코 `Completed`를 보고하지 않습니다.

`retryPolicy(node, {...})`는 Core 키 두 개로 낮춰집니다. 타이밍과 정책 필드는 `node_retry_policies`로, `on_failure`와 `error_channel`은 `failure_edges`로 갑니다. `failureExit(node, {code})`는 `failure_exits`를 씁니다. 각 키는 쓰지 않으면 없고, 그래프 단위 `retryPolicy({...})`는 바뀌지 않습니다.

```json
"node_retry_policies": {"TEST": {"max_retries": 2, "non_retryable": ["app.compile_error"], "max_visits": 3}},
"failure_edges": [{"from": "TEST", "to": "ASK_USER", "error_channel": "failures"}],
"failure_exits": {"ABORT": {"code": "plan.user_abort"}}
```

### 키

- **노드별 정책.** 적은 필드만 저장됩니다. `max_retries` 0~10, `initial_delay_ms` 10~60 000, `backoff_multiplier` 1.0~10.0, `max_delay_ms`는 `initial_delay_ms`부터 300 000까지, `non_retryable`(고유 코드 최대 16개), `max_visits` 1~100, `error_text`입니다. `attempt_timeout_ms`는 아직 받지 않습니다. 범위를 벗어나면 `{min, max, actual}`을 담은 `P_CORE_GC_FIELD_VALUE`입니다.
- **코드.** 작성자 코드는 `[a-z][a-z0-9_]*(\.[a-z0-9_]+)+`와 맞아야 하며 `NG.` 접두사는 예약되어 있습니다. `non_retryable`에는 분류기의 코드(`NG.TaskFailed`, `NG.Timeout`, `NG.SubgraphFailed`, 그리고 레퍼런스의 노드 실패 계약에 있는 나머지)도 쓸 수 있습니다.
- **failure edge.** 노드마다 최대 하나이고 `from` 순으로 정렬됩니다. 대상은 `__start__`, `__end__`, 노드 자신, 조인, 그리고 failure exit가 아니면서 edge나 라우트가 없는 노드일 수 없습니다(`P_CORE_GC_FAILURE_EDGE`). `error_channel`은 `append` reducer를 써야 합니다.
- **failure exit.** 코드의 기본값은 `NG.Aborted`입니다. failure exit 노드에는 edge나 라우트가 없습니다.
- **검증.** 알 수 없는 노드와 채널은 `P_CORE_E3`입니다. failure edge는 라우팅된 후속 노드 맵에 들어가므로 도달 가능성, 종료, barrier 우회(`P_CORE_E15`, 라우트 `on_failure`) 검사가 이를 보며, `error_channel`은 쓰인 것으로 셉니다. 노드 타입이 선언하지 않은 노드 설정의 `retry_policy` 키는 `P_CORE_E18`로 보고됩니다. 그래프 단위 `retryPolicy({...})`에 `on_failure`를 쓰면 노드별 형태를 알려 주는 힌트와 함께 `P_CORE_GC_UNKNOWN_FIELD`가 납니다.
- **마이그레이션.** `failure_edges`와 `failure_exits`는 마이그레이션 어댑터의 토폴로지 형태에 포함되고, `node_retry_policies`는 포함되지 않습니다.

### 실행 시점

- **재시도 타이밍**은 필드마다 정해집니다. 호스트 기본값(`EngineConfig::retry_policy`, `set_retry_policy`), 없으면 그래프의 `retryPolicy`, 그것도 없으면 내장 기본값에서 시작해 노드 자신의 필드가 덮어씁니다. 호스트의 노드별 정책(`EngineConfig::node_retry_policies`, `set_node_retry_policy`)은 노드별 정책이 없는 노드에서는 지금처럼 결과를 대체합니다. 노드별 정책이 있는 노드에서는 각 필드를 조이기만 할 수 있습니다. `max_retries`는 더 작은 값, 지연과 배수는 더 큰 값을 쓰며, 이런 노드는 10번 넘게 재시도하지 않습니다. 결과는 `GraphEngine::resolved_retry_policy(node)`로 볼 수 있습니다.
- **무엇을 재시도하고 라우팅할지**는 분류기(레퍼런스의 노드 실패 계약)가 정합니다. `non_retryable`에 적힌 코드는 재시도하지 않습니다. 노드 안에서 발생한 Program 진단(`ProgramDiagnosticError`, 또는 컴파일이나 승인 오류)은 `NG.ConfigError`이며 재시도하지도, 라우팅하지도 않습니다.
- **처리된 실패.** 노드가 라우팅할 수 있는 실패로 포기하고 failure edge가 있으면 실행은 계속됩니다. 실패한 시도는 아무것도 쓰지 않고, `error_channel`이 있으면 노드의 super-step에서 레코드 하나가 거기에 추가되며, 노드는 자기 edge나 라우트에 신호를 보내지 않고 edge의 대상으로만 라우팅됩니다. 같은 super-step의 형제 노드는 평소처럼 결과를 적용하고, 그 super-step의 `Command.goto`는 실패 라우트를 포함한 모든 라우트보다 여전히 우선합니다. 실패로 도착한 신호는 조인 슬롯을 채우지 않습니다. #357 형태처럼 실행된 핸들러가 멤버를 대신할 수 있습니다. 노드는 트레이스와 `interruptAfter`에서 실행된 것으로 셈하며, 노드의 pending write가 처리된 결과를 기록하므로 크래시 뒤 재개는 노드를 다시 실행하지 않고 라우트를 재생합니다. 정적 노드, 팬아웃 분기, 하나 또는 여러 `Send` 대상, 서브그래프 노드가 모두 똑같이 동작합니다. 라우팅할 수 없는 실패나 failure edge가 없는 노드의 실패는 지금처럼 실행을 실패시킵니다.
- **레코드**에는 구조화된 필드만 들어갑니다.
  ```json
  {"node": "TEST", "kind": "exhausted", "code": "NG.TaskFailed", "attempts": 3, "retryable": true,
   "step": 1, "routed_to": "ASK_USER", "effects": []}
  ```
  `kind`는 `exhausted`, `non_retryable`, `effect_committed`, `ceiling`, `visit_limit`, `deadline`(시도 타임아웃용으로 예약) 중 하나입니다. `effects`는 마지막 시도의 중개된 Tool 효과를 `{tool, ordinal, status}`로 나열합니다.
- **자유 텍스트**는 노드마다 `error_text: true`로 선택하며 호스트의 redactor가 필요합니다. `EngineConfig::failure_text_redactor`, `CatalogConfig::failure_text_redactor`, 또는 Python `GraphEngine.compile`과 `LocalProgramHost`의 `failure_text_redactor=`입니다. 출처는 타입이 있는 공개 메시지(`NodeFailureError`의 `public_message`나 Python 예외의 `public_message` 속성)뿐이고 예외 텍스트는 절대 쓰지 않습니다. redactor가 남길 텍스트를 돌려주면 엔진은 C0, 양방향, 너비 0, 태그 문자를 제거하고 UTF-8 경계에서 512바이트로 자른 뒤 `"detail": {"untrusted_text": "..."}`로 저장합니다. redactor가 없으면 텍스트를 쓰지 않습니다.
- **인터럽트.** 대상에 `interruptBefore`가 있으면 인터럽트 값에 `{"reason": "failure_route", "failure": record}`가 들어갑니다. Program에서는 `result.interrupt.value`와 `result.interrupt.pending_input.core_interrupt_value`입니다.
- **이벤트.** 포기한 노드의 `ERROR` 이벤트에는 `handled`(실패가 라우팅되면 true)와, 라우팅된 경우 `routed_to`가 있습니다.
- **루프 상한: `max_visits`.** 이를 선언한 노드는 최대 그 횟수만큼 실행됩니다. 다음 활성화는 실행되지 않고 `NG.VisitLimit`(`kind` `visit_limit`, 시도 0번)으로 실패하며, 노드의 failure edge를 따라 라우팅되거나 edge가 없으면 실행을 실패시킵니다. 카운터는 failure edge의 대상이 완료되면 초기화됩니다. 카운터는 선언한 노드에 대해서만 체크포인트 메타데이터의 `_neograph.node_visits`에 있습니다. 스텝 예산은 여전히 전체 안전장치입니다.
- **failure exit.** failure exit 노드가 완료되면 실행은 그 super-step 뒤에 `output["_neograph"]["failure_exit"] = {node, code}`와 `RunStatus::FailureExit`(Python `RunStatus.FailureExit`, `RunInvocation`은 `Error`)로 끝납니다. Program 실행은 선언된 코드로 `Failed`가 되고 `core_node`는 그 노드입니다.
- **재시도 상한.** `RunBudget::max_node_retries`(Python `ProgramRunBudget.max_node_retries`)는 실행 전체, 즉 모든 Core 호출, 노드, 분기, `Send`, 서브그래프의 노드 재시도를 제한합니다. 상한에 이르면 `kind` `ceiling`으로 재시도를 멈춥니다. 사용한 재시도는 `ProgramUsage::node_retries`, 남은 양은 `remaining_budget.max_node_retries`입니다. 없으면 상한이 없으며, 두 필드는 설정된 경우에만 직렬화됩니다. `ng.spawn`으로 시작한 자식은 부모의 상한을 물려받습니다. Program 밖에서는 `NodeRetryCeiling`을 `RunMetadata::node_retry_ceiling`으로 넘깁니다.
- **Program에서 라우팅되지 않은 실패.** failure edge가 없는 노드 실패는 Program을 `P_RUNTIME_CORE_FAILURE` 대신 분류된 코드(`NG.TaskFailed`, 작성자 코드, `NG.ProviderFailed` 등)로 `Failed`가 되게 하고, `failure.witness.node_failure`에는 `{node, code, kind, attempts, retryable, routable}`이 들어갑니다. 오케스트레이션 계획의 `retry` 연산도 같은 분류기에 묻습니다. 재시도할 수 있는 실패 뒤에만 본문을 다시 실행하고, 커밋된 효과, 재시도하지 않는 클래스, 도달한 재시도 상한 뒤에는 다시 실행하지 않습니다.

## 건전성 검사

플래그: `diagnostics.soundness_check`. `ProgramCompilerConfig::soundness_check`(Python: `ProgramCompiler(..., soundness_check=True)` 또는 `LocalProgramHost(..., soundness_check=True)`)는 루트 그래프의 라우트가 허용하는 모든 실행을 탐색하고 찾은 것을 반환합니다. 기본값은 꺼짐이며, 라우트 표처럼 봉인되지 않으므로 어느 쪽이든 번들은 같습니다. `GraphValidator`를 쓰는 C++ 호출자는 `ValidationOptions::soundness_check`를 설정합니다.

검사는 스케줄러와 같은 방식으로 super-step을 진행합니다. 상태는 실행 준비된 노드 집합, 각 join이 쥔 신호(`any_of` 슬롯 포함), 그리고 `max_visits`가 있는 노드별 방문 횟수입니다. 방문 횟수는 실행 때와 같이 그 노드의 failure edge 대상이 완료될 때만 초기화됩니다. conditional edge는 어느 라우트든 택할 수 있고, 실패 edge가 있는 노드는 대신 그 edge를 택할 수 있으며, failure exit는 실행을 끝냅니다. 발견 사항은 모두 경고이고, `witness.trace`는 이를 보여 주는 가장 짧은 실행으로 super-step마다 항목 하나씩입니다. 실행된 노드(`ready`)와 그 노드가 택한 라우트(`routes`, 실패 edge는 `on_failure`)가 들어 있습니다.

| 코드 | 발견 사항 |
|---|---|
| `P_CORE_S1` | 어떤 시점부터 어떤 실행도 완료할 수 없습니다. 모든 후속 실행이 barrier-stalled로 끝나거나, 방문 한도에서 실패하거나, 반복합니다. |
| `P_CORE_S2` | join이 아직 신호를 쥔 채 실행이 끝납니다. barrier-stalled로 끝나거나, 명시적 종료가 신호를 버립니다. |
| `P_CORE_S3` | join이 어떤 실행에서도 발화하지 않습니다. |
| `P_CORE_S4` | 실행이 출구 없는 루프에 들어갈 수 있고, 그러면 step 한도에서만 멈춥니다. |
| `P_CORE_S5` | 한 overwrite 채널에 쓰는 두 노드가 같은 super-step에서 실행될 수 있습니다(`P_CORE_E5`가 보고하는 fan-out 형제 외의 경우). |

`P_CORE_SOUNDNESS` 요약은 검사가 어디까지 했는지 알려 줍니다. 모든 실행을 탐색했으면 노트, 그렇지 않으면 경고입니다. 탐색하지 않은 것은 결코 주장하지 않습니다.

- `Command.goto`나 `Send`를 반환할 수 있는 노드 타입은 effect 계약에 `"control": ["goto"]` 또는 `["send"]`로 이를 선언합니다. 그런 노드가 준비된 뒤의 실행은 분석할 수 없습니다. 요약이 그 노드들을 나열하고, `P_CORE_S1`과 `P_CORE_S4`는 모든 후속 실행을 탐색한 곳에서만, `P_CORE_S3`는 아예 보고하지 않습니다. 검사는 계약을 신뢰하므로, 선언하지 않고 `Command.goto`를 반환하는 노드 타입은 선언된 라우트대로 분석됩니다.
- `soundness_state_cap`(기본 100 000 상태)이 작업량을 제한합니다. 검사가 여기에 도달하면 요약은 `not_checked`이고, 그 전에 찾은 `P_CORE_S2`와 `P_CORE_S5` 발견 사항만 반환됩니다.

`barrier("D", ["B", "C"])`인 #357 형태라면:

```python
compiler = ng.ProgramCompiler(registry, soundness_check=True)
bundle = compiler.compile(ng.ProgramSource.from_javascript("plan.js", source))
stall = [d for d in bundle.compile_diagnostics if d["code"] == "P_CORE_S2"][0]
stall["witness"]["trace"]
# [{"ready": ["A"], "routes": {}},
#  {"ready": ["B", "C"], "routes": {"B": "fail"}},
#  {"ready": ["FIX"], "routes": {}}]
stall["witness"]["missing"]             # ["B"]
stall["witness"]["suggested_wait_for"]  # [{"any_of": ["B", "FIX"]}, "C"]
```

`P_CORE_S1`은 같은 그래프를 `B`가 `fail`을 택한 시점부터 보고합니다. `barrier("D", [{any_of: ["B", "FIX"]}, "C"])`라면 검사는 아무것도 찾지 않고, 요약 노트는 모든 실행을 탐색했다고 알려 줍니다. 각 witness는 [진단 가이드](diagnostics.md#soundness-check)에서 설명합니다.
