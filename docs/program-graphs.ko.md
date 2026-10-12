<!-- neograph-i18n: source=docs/program-graphs.md locale=ko source_sha256=ca511a2202153d10cd6eae7d3747c97d14de5984171708d264fa048b1ed2dc1b -->
# Program 그래프 기능

**Languages:** [English](program-graphs.md) | [한국어](program-graphs.ko.md) | [日本語](program-graphs.ja.md) | [简体中文](program-graphs.zh-CN.md)

이 문서는 `define()` 프로그램이 노드, 엣지, barrier, 그래프 수준 재시도 정책 외에 쓸 수 있는 그래프 기능을 설명합니다. 각 기능은 strict Core 정의(`schema_version` 1)의 키이며, `javascript_authoring_capability_manifest()["features"]`에 대응하는 플래그가 있습니다. 어떤 기능을 쓰지 않는 정의는 그 기능이 생기기 전과 바이트, `definition_hash`, bundle id가 완전히 같습니다.

이 키 중 하나라도 쓰는 정의는 검증기 경고를 bundle에 봉인하지 않습니다. 경고는 컴파일 결과로 계속 반환됩니다. [진단 가이드](diagnostics.md)를 참고하세요.

<a id="authoring-rules"></a>
## 작성 규칙

Program은 호스트가 컴파일하는 JavaScript 모듈 하나입니다. 아래 규칙은 모듈을 컴파일하거나 실행할 때 검사하며, 어기면 각 항목에 적은 진단 코드로 보고합니다([진단 가이드](diagnostics.md) 참고).

- **모듈 형태.** 모듈은 그래프 빌더를 정확히 하나 반환하는 동기 `define()`을 내보냅니다(`P_JS_DEFINE_MISSING`, `P_JS_DEFINE_ASYNC`, `P_JS_DEFINE_VALUE`). `main()`만 내보내는 모듈은 거부됩니다. `main()`은 선택 사항이며, 내보낸다면 동기 제너레이터여야 합니다(`P_JS_CONTROL_MAIN`, `P_JS_CONTROL_ASYNC`). `main()`이 없으면 실행은 그래프를 곧바로 실행합니다. 최상위 `await`가 있는 모듈도 거부됩니다.
- **`define()` 안.** 반복문, 보조 함수, 템플릿 문자열 같은 평범하고 결정적인 JavaScript로 `ng.graph()`를 통해 그래프를 만듭니다. 명령을 `yield`할 수 없고 노드도 실행되지 않습니다. 빌더 인자는 정규 JSON 데이터여야 하고(`P_JS_GRAPH_VALUE`), `define()`이 반환한 뒤에는 빌더를 쓸 수 없습니다(`P_JS_GRAPH_SEALED`).
- **`main()` 안.** 반복, 재귀, 클로저, 예외, 지역 상태는 평소처럼 쓸 수 있습니다. 다만 `main()`은 `ng`로 만든 명령(`ng.callCore`, `ng.all` 등)을 `yield`하는 방식으로만 동작합니다. 런타임이 명령마다 검사하고 저널에 기록한 뒤 실행하고, 그 결과로 제너레이터를 재개합니다. 소스 코드는 동작을 요청할 뿐 스스로 권한을 얻지 못합니다. 모든 노드 타입, 조건, 리듀서는 호스트의 레지스트리에 있어야 하고, 호스트의 예산이 실행을 제한합니다.
- **거부되는 것.** `Date`, `eval`, `Function` 생성자는 `P_JS_AMBIENT_DENIED`로 실패합니다. `Math.random()`은 항상 0을 반환하므로 Program에는 난수가 없습니다. 전역 `std`, `os`, `process`, `require`, `fetch`, `crypto`, `performance`, 타이머 함수, `Worker`, `SharedArrayBuffer`, `Atomics`, `WebAssembly`, `WeakRef`, `FinalizationRegistry`는 정의되어 있지 않습니다. 동적 `import()`와 `require()`는 `P_JS_IMPORT_DYNAMIC`으로 실패하고, 정적 `import`는 소스와 함께 봉인된 모듈을 가리켜야 합니다(`P_JS_IMPORT_UNAVAILABLE`).
- **소스 언어가 아닌 것.** Program을 쓰는 언어는 JavaScript뿐입니다. `define()`이 만드는 strict Core JSON, bundle, 저널, 기록은 저장된 산출물이며 NeoGraph는 이를 Program 소스로 받지 않습니다. 이전의 Core DSL과 Program JSON 연산 트리는 제거되었습니다.

## 그래프 메타데이터

플래그: `graph.metadata`

```js
export function define() {
  return ng.graph("plan")
    .metadata({owner: "agent:planner", requirements: ["REQ-12"]})
    .node("draft", {type: "my.task"})
    .entry("draft").exit("draft");
}
```

`.metadata(object)`는 그래프마다 한 번만 호출할 수 있습니다. Core 정의는 이를 최상위 `"metadata"` 객체로 저장하며, 빈 객체는 없는 것과 같습니다.

- **한도.** 정규(canonical) JSON 기준 최대 8 KiB, `metadata` 아래 중첩 깊이 최대 4, 객체 멤버 총 64개, 키 길이 1~128바이트입니다. `neograph.`로 시작하는 최상위 키는 예약되어 있습니다.
- **텍스트.** 키와 문자열에는 보이지 않는 문자를 넣을 수 없습니다. 탭, 줄바꿈, 캐리지 리턴을 제외한 C0 제어 문자, bidi 제어 문자 U+061C, U+200E, U+200F, U+202A~U+202E, U+2066~U+2069, 너비가 없는 문자 U+200B~U+200D, U+2060, U+FEFF, 태그 문자 U+E0000~U+E007F가 여기에 해당합니다.
- **식별자.** 메타데이터는 정의의 일부이므로 `definition_hash`와 bundle id가 달라집니다. 키 순서는 영향을 주지 않습니다.
- **읽는 쪽.** 호스트만 봉인된 정의에서 읽을 수 있습니다. C++에서는 `bundle.sealed_core_definitions()[0].definition["metadata"]`, Python에서는 `bundle.sealed_core_definitions[0]["definition"]["metadata"]`로 읽습니다. 예를 들어 `LocalProgramHost.compile()`이 반환하는 bundle이 그렇습니다. 노드 팩토리, 실행 상태, 체크포인트, 출력, 이벤트에는 보이지 않습니다.
- **권한.** 메타데이터는 작성자가 주장하는 내용을 기록할 뿐이며 아무 권한도 부여하지 않습니다. 위임 한도는 호스트가 승인(admission)한 `RunBudget`(`max_child_depth`, `max_total_children`, `max_concurrency`)에 그대로 남습니다.
- **마이그레이션.** 메타데이터는 `GraphSemanticMigrationAdapter`의 토폴로지 형태에 포함되지 않으므로, 바꿔도 마이그레이션이 막히지 않습니다. 이를 승인하는 호스트에는 `metadata_changes()`가 추가, 삭제, 변경된 포인터를 나열합니다.

오류: 두 번째 호출이나 객체가 아닌 인자는 `P_JS_GRAPH_ARGUMENT`, 그 밖에 `P_CORE_GC_FIELD_TYPE`, `P_CORE_GC_METADATA_LIMIT`, `P_CORE_GC_METADATA_TEXT`, `P_CORE_GC_FIELD_VALUE`(빈 키 또는 예약된 키)가 있습니다.

## any_of를 이용한 join 슬롯

플래그: `barrier.any_of`

```js
graph.conditionalEdge("B", "t.check", {pass: "D", fail: "FIX"})
  .edge("FIX", "D").edge("C", "D")
  .barrier("D", [{any_of: ["B", "FIX"]}, "C"]);
```

`barrier(node, waitFor)`의 항목은 노드 이름이거나 슬롯 `{any_of: [...]}`입니다. join은 슬롯 단위로 보면 여전히 AND join입니다.

- 슬롯은 대안 중 하나라도 join에 신호를 보내면 채워집니다. join은 모든 슬롯이 채워지면 실행되고, 그 뒤 모든 신호가 지워집니다. 일반 barrier와 같습니다.
- 슬롯에 없는 노드가 보낸 신호는 기록되지만 계산에 들어가지 않습니다. `Command.goto`와 `Send`는 이전처럼 join을 우회합니다.
- 같은 슈퍼스텝이나 join이 실행되기 전에 대안 두 개가 도착해도 달라지는 것은 없습니다. join이 실행된 뒤에 대안이 도착하면 끝날 수 없는 새 라운드가 열리므로, 실행은 `P_CORE_BARRIER_STALLED`로 끝납니다(실패 시 차단). 그러므로 대안은 서로 배타적인 경로여야 하며, 두 대안이 모두 실행될 수 있으면 `P_CORE_E17` 경고가 나옵니다.
- Core 정의에서 슬롯은 `{"any_of": [...]}`로 출력되고 이름은 UTF-8 바이트 순으로 정렬됩니다. 슬롯끼리는 가장 작은 이름 순으로 정렬됩니다. 이름이 하나뿐인 슬롯은 그 이름 하나로 출력되므로, `[{any_of: ["B"]}, "C"]`는 `["B", "C"]`와 바이트도 id도 같습니다.
- 슬롯이 있는 join이 정체되면 witness에 슬롯이 나열됩니다. `wait_for`와 `missing`에는 이름과 `{"any_of": [...]}` 객체가 들어 있습니다.
- 검증기는 슬롯마다 검사합니다. 어떤 대안도 join으로 라우팅되지 않으면 `P_CORE_E8`이고, 멤버가 다른 곳으로 라우팅될 수 있으면 `P_CORE_E15`입니다. 같은 슬롯의 다른 대안으로 가는 경로는 우회가 아닙니다.

오류: 다른 형태의 항목은 `P_JS_GRAPH_ARGUMENT`이고, `any_of`가 비어 있거나 문자열이 아닌 경우, `any_of` 옆에 다른 키가 있는 경우, `any_of`가 중첩된 경우, 한 슬롯 안이나 슬롯 사이에 같은 노드가 반복된 경우는 `P_CORE_GC_BARRIER`입니다.

## Witness 경로와 라우트 표

- **우회 경로.** `P_CORE_E15`의 witness에는 `paths`가 있습니다. 우회 경로마다, 멤버에서 시작해 그 경로를 지나는 라우트 순서이며 join으로 돌아오면 join에서 끝납니다. join으로 돌아오는 경우에는 join 바로 앞의 노드가 그 멤버를 대신할 수 있습니다. 이때 witness에 `substitute`와 `suggested_wait_for`가 추가되고, 반환되는 진단은 `wait_for`를 그 값으로 바꾸라고 제안합니다(적용 가능성 `maybe`). #357 형태에서 제안은 `[{"any_of": ["B", "FIX"]}, "C"]`입니다. 이미 다른 슬롯을 채우는 노드는 제안에 포함되지 않습니다.
- **정체 경로.** 정체된 join의 witness(`output["_neograph"]["barrier_stalled"]`, Program 실패에서는 `stalled_barriers`)에는 `paths`가 추가됩니다. 이는 실행된 누락 멤버에서 join에 신호를 보낸 노드까지의 경로이며, 트레이스와 라우팅된 후속 노드 맵에서 가져옵니다. 제안할 수 있으면 같은 `suggested_wait_for`도 추가됩니다.
- **라우트 표.** `ProgramCompilerConfig::echo_routes`(Python에서는 `ProgramCompiler(..., echo_routes=True)`)를 켜면 `P_CORE_ROUTES` 메모를 반환합니다. 이 메모의 `witness.routes`는 노드마다 스케줄러가 라우팅할 수 있는 대상을 보여 줍니다. 첫 번째 조건부 엣지의 경로이고, 조건부 엣지가 없으면 정적 엣지입니다. 이 메모는 봉인되지 않으므로 bundle은 켜든 끄든 같습니다.

## 실패 라우팅

플래그: `retry_policy.per_node`, `failure_edges`, `failure_exits`, `retry.attempt_timeout`

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

TEST는 세 번 시도합니다. 그래도 실패하면 실행은 실패하지 않고 TEST가 ASK_USER로 라우팅되며, ASK_USER는 실패 레코드를 가지고 사람의 응답을 기다리며 일시 중지합니다. `retry`로 응답하면 IMPLEMENT와 TEST가 다시 실행됩니다. `abort`로 응답하면 실패 종료 노드인 ABORT가 실행되어 실행은 `plan.user_abort` 코드와 함께 `Failed`로 끝나며, `Completed`로 보고되는 일은 없습니다.

`retryPolicy(node, {...})`는 두 개의 Core 키로 변환됩니다. 타이밍과 정책 필드는 `node_retry_policies`로, `on_failure`와 `error_channel`은 `failure_edges`로 들어갑니다. `failureExit(node, {code})`는 `failure_exits`를 기록합니다. 쓰지 않는 키는 정의에 나타나지 않으며, 그래프 수준 `retryPolicy({...})`는 바뀌지 않습니다.

```json
"node_retry_policies": {"TEST": {"max_retries": 2, "non_retryable": ["app.compile_error"], "max_visits": 3}},
"failure_edges": [{"from": "TEST", "to": "ASK_USER", "error_channel": "failures"}],
"failure_exits": {"ABORT": {"code": "plan.user_abort"}}
```

### 키

- **노드별 정책.** 쓴 필드만 저장됩니다. `max_retries`는 0~10, `initial_delay_ms`는 10~60 000, `backoff_multiplier`는 1.0~10.0, `max_delay_ms`는 `initial_delay_ms`부터 300 000까지, `non_retryable`은 고유한 코드 최대 16개, `max_visits`는 1~100, `error_text`, `attempt_timeout_ms`는 100~3 600 000입니다. 범위를 벗어난 값은 `{min, max, actual}`이 담긴 `P_CORE_GC_FIELD_VALUE`입니다.
- **코드.** 작성자 코드는 `[a-z][a-z0-9_]*(\.[a-z0-9_]+)+` 패턴과 일치해야 하고, `NG.` 접두사는 예약되어 있습니다. `non_retryable`에는 분류기의 코드(`NG.TaskFailed`, `NG.Timeout`, `NG.SubgraphFailed`와 레퍼런스의 노드 실패 계약에 있는 나머지 코드)도 나열할 수 있습니다.
- **실패 엣지.** 노드마다 최대 하나이며 `from` 기준으로 정렬됩니다. 대상은 `__start__`, `__end__`, 노드 자신, join일 수 없고, 실패 종료 노드가 아닌 한 엣지나 라우트가 없는 노드일 수도 없습니다(`P_CORE_GC_FAILURE_EDGE`). `error_channel`은 `append` 리듀서를 써야 합니다.
- **실패 종료 노드.** 코드의 기본값은 `NG.Aborted`입니다. 실패 종료 노드에는 엣지나 라우트가 없습니다.
- **검증.** 알 수 없는 노드와 채널은 `P_CORE_E3`입니다. 실패 엣지는 라우팅된 후속 노드 맵에 들어가므로 도달 가능성, 종료, barrier 우회(`P_CORE_E15`, 라우트 `on_failure`) 검사에서도 보이며, `error_channel`은 쓰기가 있는 채널로 계산됩니다. 노드 타입이 선언하지 않은 `retry_policy` 키가 노드 설정에 있으면 `P_CORE_E18`로 보고됩니다. 그래프 수준 `retryPolicy({...})`에 `on_failure`를 쓰면 노드별 형식을 쓰라는 힌트와 함께 `P_CORE_GC_UNKNOWN_FIELD`가 됩니다.
- **마이그레이션.** `failure_edges`와 `failure_exits`는 마이그레이션 어댑터의 토폴로지 형태에 포함되고, `node_retry_policies`는 포함되지 않습니다.

### 실행 시점 동작

- **재시도 타이밍**은 필드 단위로 정해집니다. 먼저 호스트 기본값(`EngineConfig::retry_policy`, `set_retry_policy`)을 쓰고, 없으면 그래프의 `retryPolicy`, 그것도 없으면 내장 기본값을 쓴 다음, 노드 자신의 필드를 적용합니다. 호스트가 노드별 정책(`EngineConfig::node_retry_policies`, `set_node_retry_policy`)을 지정하면, 노드별 정책이 없는 노드의 결과를 이 정책이 이전처럼 대체합니다. 노드별 정책이 있는 노드에는 호스트가 필드를 더 엄격하게만 바꿀 수 있습니다. 즉 `max_retries`는 더 작은 값이, 지연과 배수는 더 큰 값이 적용되며, 이런 노드는 10번을 넘게 재시도하지 않습니다. 결과는 `GraphEngine::resolved_retry_policy(node)`가 반환합니다.
- **무엇을 재시도하고 라우팅하는지**는 분류기가 정합니다(레퍼런스의 노드 실패 계약). `non_retryable`에 나열한 코드는 재시도하지 않습니다. 노드 안에서 발생한 Program 진단(`ProgramDiagnosticError` 또는 컴파일/승인(admission) 오류)은 `NG.ConfigError`이며 재시도도 라우팅도 하지 않습니다.
- **처리된 실패.** 노드가 라우팅할 수 있는 실패로 재시도를 포기했고 실패 엣지가 있으면 실행은 계속됩니다. 노드의 실패한 시도는 아무것도 쓰지 않습니다. 노드에 `error_channel`이 있으면 노드의 슈퍼스텝에서 레코드 하나가 거기에 추가됩니다. 노드는 자신의 엣지나 라우트에 신호를 보내지 않고 실패 엣지의 대상으로만 라우팅됩니다. 같은 슈퍼스텝의 형제 노드는 평소처럼 결과를 적용하며, 그 슈퍼스텝의 `Command.goto`는 실패 라우트를 포함한 모든 라우트보다 여전히 우선합니다. 실패로 도착한 신호는 join 슬롯을 채우지 않습니다. 실행된 핸들러가 그 멤버를 대신할 수는 있으며, #357 형태가 그렇습니다. 이 노드는 트레이스와 `interruptAfter`에서 실행된 것으로 취급되고, 대기 중인 쓰기에 처리된 결과가 기록됩니다. 그래서 크래시 뒤에 재개하면 노드를 다시 실행하지 않고 그 라우트를 재생(replay)합니다. 정적 노드, fan-out 분기, `Send` 대상(하나든 여럿이든), 서브그래프 노드도 똑같이 동작합니다. 라우팅할 수 없는 실패나 실패 엣지가 없는 노드의 실패는 이전처럼 실행을 실패시킵니다.
- **레코드**에는 구조화된 필드만 들어갑니다.
  ```json
  {"node": "TEST", "kind": "exhausted", "code": "NG.TaskFailed", "attempts": 3, "retryable": true,
   "step": 1, "routed_to": "ASK_USER", "effects": []}
  ```
  `kind`는 `exhausted`, `non_retryable`, `effect_committed`, `ceiling`, `visit_limit`, `deadline`(백오프가 실행의 데드라인을 넘기게 되는 경우) 중 하나입니다. `effects`에는 마지막 시도에서 중개된 Tool 효과가 `{tool, ordinal, status}` 형태로 나열됩니다.
- **자유 텍스트**는 노드마다 `error_text: true`로 켜야 하고, 호스트의 redactor가 필요합니다. `EngineConfig::failure_text_redactor`, `CatalogConfig::failure_text_redactor`, 또는 Python의 `GraphEngine.compile`과 `LocalProgramHost`에 넘기는 `failure_text_redactor=`로 지정합니다. 텍스트의 유일한 출처는 타입이 있는 공개 메시지(`NodeFailureError`의 `public_message` 또는 Python 예외의 `public_message` 속성)이며, 예외 텍스트는 쓰지 않습니다. redactor가 남길 텍스트를 반환하면 엔진은 C0, 양방향(bidi), 너비 없는 문자, 태그 문자를 제거하고, UTF-8 경계에서 512바이트로 자른 뒤 `"detail": {"untrusted_text": "..."}`로 저장합니다. redactor가 없으면 텍스트를 기록하지 않습니다.
- **인터럽트.** 대상에 `interruptBefore`가 있으면 인터럽트 값에 `{"reason": "failure_route", "failure": record}`가 담깁니다. Program에서는 `result.interrupt.value`와 `result.interrupt.pending_input.core_interrupt_value`에서 읽을 수 있습니다.
- **이벤트.** 재시도를 포기한 노드의 `ERROR` 이벤트에는 `handled`(실패가 라우팅되었으면 true)가 있고, 라우팅되었으면 `routed_to`도 있습니다.
- **루프 제한: `max_visits`.** 이 값을 선언한 노드는 최대 그 횟수만큼만 실행됩니다. 그다음 활성화에서는 노드가 실행되지 않고 `NG.VisitLimit`(`kind`는 `visit_limit`, 시도 0회)으로 실패하며, 이 실패는 노드의 실패 엣지로 라우팅됩니다. 실패 엣지가 없으면 실행이 실패합니다. 카운터는 실패 엣지의 대상이 완료되면 초기화됩니다. 카운터는 체크포인트 메타데이터에 `_neograph.node_visits`로 저장되며, 이 값을 선언한 노드에 대해서만 저장됩니다. 스텝 예산은 그대로 전역 안전장치로 남습니다.
- **실패 종료 노드.** 실패 종료 노드가 완료되면 그 슈퍼스텝이 끝난 뒤 실행이 종료되며, `output["_neograph"]["failure_exit"] = {node, code}`와 `RunStatus::FailureExit`(Python에서는 `RunStatus.FailureExit`, `RunInvocation`은 `Error`로 보고)이 설정됩니다. Program 실행은 선언된 코드와 그 노드를 가리키는 `core_node`와 함께 `Failed`로 끝납니다.
- **재시도 상한.** `RunBudget::max_node_retries`(Python에서는 `ProgramRunBudget.max_node_retries`)는 모든 Core 호출, 노드, 분기, `Send`, 서브그래프를 포함해 실행 전체의 노드 재시도 횟수를 제한합니다. 이 상한에 도달하면 `kind`가 `ceiling`인 상태로 재시도를 멈춥니다. 사용한 재시도 횟수는 `ProgramUsage::node_retries`, 남은 횟수는 `remaining_budget.max_node_retries`입니다. 값이 없으면 상한이 없다는 뜻이고, 두 필드는 설정된 경우에만 직렬화됩니다. `ng.spawn`으로 시작한 자식은 부모의 상한을 물려받습니다. Program 밖에서는 `NodeRetryCeiling`을 `RunMetadata::node_retry_ceiling`으로 넘깁니다.
- **Program에서 라우팅되지 않은 실패.** 실패 엣지가 없는 노드의 실패는 `P_RUNTIME_CORE_FAILURE`가 아니라 분류된 코드(`NG.TaskFailed`, 작성자 코드, `NG.ProviderFailed` 등)로 Program을 `Failed`로 끝내며, `failure.witness.node_failure`에 `{node, code, kind, attempts, retryable, routable}`이 담깁니다. 오케스트레이션 plan의 `retry` 연산도 같은 분류기에 묻습니다. 본문은 재시도할 수 있는 실패 뒤에만 다시 실행되며, 커밋된 효과, 재시도하지 않는 분류, 도달한 재시도 상한, 데드라인에서 멈춘 경우 뒤에는 다시 실행되지 않습니다.

### 시도 타임아웃과 데드라인

플래그: `retry.attempt_timeout`

```js
graph.retryPolicy("TEST", {attempt_timeout_ms: 30000, max_retries: 2, on_failure: "ASK_USER"});
```

- **시간이 초과된 시도.** `attempt_timeout_ms`는 노드의 시도마다 시간을 제한합니다. 시간이 지나면 시도는 자신의 취소 토큰(노드의 `ctx.cancel_token`이며 실행 토큰의 자식)으로 취소되고 `NG.Timeout`으로 실패합니다. 이 실패는 일반 실패처럼 재시도하고 라우팅할 수 있습니다. 첫 타임아웃에서 멈추려면 `NG.Timeout`을 `non_retryable`에 나열합니다. 먼저 일어난 종료 이벤트가 이깁니다. 타임아웃 시각이거나 그 이후에 완료된 시도는, 타이머가 아직 발동하지 않았더라도 시간이 초과된 것으로 취급합니다.
- **늦은 완료는 버려집니다.** 시간이 초과된 시도가 그 뒤에 하는 일은 모두 효력이 없습니다. 쓰기와 결과는 버려지고, 내보낸 이벤트는 실행의 콜백에 도달하지 않으며, 시작한 중개 Tool 호출은 토큰이 취소되었으므로 거부됩니다. 다음 시도는 취소된 시도가 멈춘 뒤에야 시작되므로, 한 노드의 두 시도가 겹치는 일은 없습니다.
- **중지 데드라인.** 취소된 시도에는 실행에 허용된 시간만큼, 최소 1초의 중지 시간이 주어집니다. 그때까지 멈추지 않은 시도는 아직 효과를 수행하는 중일 수 있으므로, 노드는 `NG.EffectUncertain`으로 실패하며 이 실패는 재시도도 라우팅도 하지 않아 실행이 실패합니다. 그 시도는 백그라운드에서 계속 실행되며 반환될 때까지 노드를 살려 둡니다. `wait_for_node_attempts()`가 이런 시도를 기다리고, Python 바인딩은 인터프리터가 종료될 때 이를 호출합니다. 멱등하지 않은 효과를 커밋한 뒤에 멈춘 시도는 이전처럼 `NG.EffectCommitted`로 실패하고, 중개된 효과를 불확실한 상태로 남긴 시도는 조정(reconciliation)을 위해 실행을 일시 중지합니다.
- **모든 경로에서 동일.** 정적 노드, fan-out 분기, `Send` 대상(하나든 여럿이든), 서브그래프 노드가 같은 경로를 거칩니다. 서브그래프 노드의 자식 실행은 그 시도와 함께 취소됩니다. 시간이 초과된 시도가 있은 뒤 크래시가 나면 실행 중이던 시도에서 재개하며, 재개가 시간이 초과된 시도의 결과를 적용하는 일은 없습니다.
- **호스트 타임아웃.** `GraphEngine::set_node_attempt_timeout_ms(node, ms)`(Python의 `set_node_attempt_timeout_ms`, `None`이면 해제)는 정의에 타임아웃이 없는 노드에 타임아웃을 설정합니다. 정의에 타임아웃이 있는 노드는 호스트가 줄이기만 할 수 있으며, 둘 중 짧은 쪽이 적용됩니다. `resolved_attempt_timeout_ms(node)`가 그 결과를 반환합니다.
- **노드 본문이 실행되는 곳.** 실행을 구동하는 코루틴은 타임아웃이 있는 본문을 직접 실행하지 않고, Program 실행에서는 어떤 노드 본문도 직접 실행하지 않습니다. 이런 본문은 프로세스 전체가 공유하는 워커 풀(`NEOGRAPH_NODE_ATTEMPT_THREADS`, 1~1024, 기본값 32)에서 실행됩니다. 그래서 스레드를 막는 본문이 자신의 타임아웃도, join 멤버와 `await` 타이머가 공유하는 Program 스케줄러도 붙잡아 두지 못합니다. 이는 Python 노드뿐 아니라 C++ 노드와 Core 서브그래프에도 적용됩니다. 그 밖의 `GraphEngine` 실행은 타임아웃이 없는 본문을 계속 호출한 스레드에서 실행합니다.
- **데드라인을 고려한 백오프.** 다음 시도 전의 백오프가 실행의 데드라인(Program에서는 `wall_time_ms`, Program 밖에서는 `RunMetadata::deadline`)을 넘기게 되면, 노드는 예산이 실행을 끝낼 때까지 대기하는 대신 즉시 `kind`가 `deadline`인 상태로 재시도를 멈춥니다. 실패는 자기 코드(`NG.TaskFailed`, `NG.Timeout` 등)를 유지하고 노드의 실패 엣지로 라우팅됩니다. 실패 엣지가 없으면 실행이 실패합니다.

## 건전성 검사

플래그: `diagnostics.soundness_check`. `ProgramCompilerConfig::soundness_check`(Python에서는 `ProgramCompiler(..., soundness_check=True)` 또는 `LocalProgramHost(..., soundness_check=True)`)는 루트 그래프의 라우트가 허용하는 모든 실행을 탐색해서 발견한 내용을 반환합니다. 기본값은 꺼짐이며, 라우트 표와 마찬가지로 봉인되지 않으므로 bundle은 켜든 끄든 같습니다. `GraphValidator`를 호출하는 C++ 코드는 `ValidationOptions::soundness_check`를 설정합니다.

검사는 스케줄러와 같은 방식으로 슈퍼스텝을 진행합니다. 상태는 ready 노드의 집합, 각 join이 가진 신호(`any_of` 슬롯 포함), `max_visits`가 있는 노드의 방문 횟수로 이루어집니다. 방문 횟수는 실행 시점과 마찬가지로 그 노드의 실패 엣지 대상이 완료될 때만 초기화됩니다. 조건부 엣지는 어느 라우트든 택할 수 있고, 실패 엣지가 있는 노드는 대신 그 엣지를 택할 수 있으며, 실패 종료 노드는 실행을 끝냅니다. 발견한 내용은 하나하나가 경고이며, 경고의 `witness.trace`는 그 문제를 보여 주는 가장 짧은 실행입니다. 항목은 슈퍼스텝마다 하나이고, 실행된 노드(`ready`)와 그 노드가 택한 라우트(`routes`, 실패 엣지는 `on_failure`)로 구성됩니다.

| 코드 | 발견 내용 |
|---|---|
| `P_CORE_S1` | 어느 시점부터는 어떤 실행도 완료될 수 없다. 모든 후속 경로가 barrier 정체로 끝나거나, 방문 한도에서 실패하거나, 루프에 빠진다. |
| `P_CORE_S2` | join에 신호가 남은 채로 실행이 끝난다. barrier 정체로 끝나거나, 신호를 버리는 명시적 종료로 끝난다. |
| `P_CORE_S3` | 어떤 실행에서도 join이 발동하지 않는다. |
| `P_CORE_S4` | 실행이 빠져나올 수 없는 루프에 들어가 스텝 한도에서만 멈출 수 있다. |
| `P_CORE_S5` | overwrite 채널 하나에 쓰는 두 노드가 같은 슈퍼스텝에서 실행될 수 있다(`P_CORE_E5`가 보고하는 fan-out 형제 노드는 제외). |

`P_CORE_SOUNDNESS` 요약은 검사가 어디까지 진행되었는지 알려 줍니다. 모든 실행을 탐색했으면 메모로, 그렇지 않으면 경고로 나옵니다. 탐색하지 않은 부분에 대해서는 아무것도 주장하지 않습니다.

- `Command.goto`나 `Send`를 반환할 수 있는 노드 타입은 효과 계약에 `"control": ["goto"]` 또는 `["send"]`로 이를 선언합니다. 이런 노드가 ready가 된 뒤의 실행은 분석할 수 없습니다. 요약에 그 노드들이 나열되고, `P_CORE_S1`과 `P_CORE_S4`는 모든 후속 경로를 탐색한 곳에서만 보고되며, `P_CORE_S3`는 전혀 보고되지 않습니다. 검사는 계약을 신뢰하므로, 선언하지 않고 `Command.goto`를 반환하는 노드 타입은 선언된 라우트로만 분석됩니다.
- `soundness_state_cap`(기본값 100 000 상태)이 작업량을 제한합니다. 검사가 이 한도에 도달하면 요약에 `not_checked`가 표시되고, 그전에 발견한 `P_CORE_S2`와 `P_CORE_S5`만 반환됩니다.

#357 형태에서 `barrier("D", ["B", "C"])`를 쓰는 경우입니다.

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

`P_CORE_S1`은 같은 그래프를 `B`가 `fail`을 택한 시점부터 보고합니다. `barrier("D", [{any_of: ["B", "FIX"]}, "C"])`로 바꾸면 검사에서 발견되는 것이 없고, 요약 메모에는 모든 실행을 탐색했다고 나옵니다. witness의 항목별 설명은 [진단 가이드](diagnostics.md#soundness-check)에 있습니다.
