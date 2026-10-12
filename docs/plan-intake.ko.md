<!-- neograph-i18n: source=docs/plan-intake.md locale=ko source_sha256=69600014da30bc764c1fde85c2622f45ee13bd5f0a98c35f647aeff50dc58cc9 -->
# Plan intake와 plan repair

**Languages:** [English](plan-intake.md) | [한국어](plan-intake.ko.md) | [日本語](plan-intake.ja.md) | [简体中文](plan-intake.zh-CN.md)

task graph를 만들어야 하는 planner 모델은 닫힌 형식의 타입 있는 문서를 써야 합니다. step plan은 같은 일을 하는 더 작은 문서로, 순서가 있는 평평한 step 목록입니다. 순수 함수가 호스트의 템플릿 allow-list로 step plan을 검사하고, 모델이 고칠 수 있는 형태로 모든 문제를 보고하며, 올바른 계획을 `TaskGraphProposal`로 변환합니다. 이후 proposal은 기존의 신뢰 경로를 그대로 지납니다. `TaskGraphProposal::parse`, `TaskGraphFragmentCompiler::compile`, 게시, `expand_task_graph` 순서입니다.

step plan은 데이터이며 권한이 없습니다. intake를 통과했다는 것은 계획의 형식이 맞고 정책 안에 있다는 뜻입니다. 계획이 요청을 해결한다는 뜻은 아닙니다. 모든 검사를 통과하고도 엉뚱한 도구나 잘못된 순서를 고른 계획일 수 있습니다.

```
planner text --intake_step_plan--> violations | TaskGraphProposal JSON
TaskGraphProposal JSON --parse--> --compile--> --publish--> expand_task_graph
```

이 페이지는 step plan, 그 violation, step plan이 변환되는 스키마 2 proposal(명시적 예산, 리터럴 입력, 시도 횟수), task 재시도, plan repair 루프를 다룹니다. 이미 실행 중인 그래프를 바꾸는 기능은 이번 릴리스에 없습니다.

## step plan

```json
{
  "schema": "step-plan/v1",
  "steps": [
    {"id": "s1", "use": "web.search/v2",
     "params": {"query": "weather in Seoul tomorrow", "limit": 3},
     "retry": {"max_attempts": 2}},
    {"id": "s2", "use": "text.summarize/v1",
     "params": {"text": {"from_step": "s1", "field": "items/0/snippet"}, "style": "one sentence"}},
    {"id": "s3", "use": "notes.save/v1",
     "params": {"title": "Seoul weather", "body": {"from_step": "s2", "output": "summary", "field": "text"}},
     "after": ["s1"]}
  ]
}
```

형식은 닫혀 있습니다. 아래에 없는 키는 모두 violation입니다.

| 키 | 타입 | 규칙 |
|---|---|---|
| `schema` | string | `step-plan/v1`이어야 함 |
| `steps` | array | step 1개 이상, `StepPlanLimits::max_steps`(기본 32) 이하 |
| `steps[].id` | string | `[a-z][a-z0-9_]{0,31}`, 유일 |
| `steps[].use` | string | 호스트 allow-list에 있는 템플릿 id. 동작을 고르는 것은 이것뿐 |
| `steps[].params` | object | 선택. 키는 템플릿의 입력 이름으로, `input_fields` 항목에서 맨 앞의 `/`를 뺀 것(중첩 입력 `/filter/key`는 `filter/key`로 씀). 값은 리터럴 또는 참조 |
| 참조 | object | `from_step`(앞선 step의 id)이 있음. `output`은 artifact 이름이며 기본값은 생산자의 `out`, 없으면 유일한 artifact. `field`는 맨 앞의 `/`를 뺀 출력 필드이며 기본값은 artifact의 유일한 필드 |
| `steps[].out` | string | 선택. step이 만들어 낼 artifact 이름. 이후 참조의 `output` 기본값이 됨. 템플릿에 선언된 artifact여야 함 |
| `steps[].after` | id 배열 | 선택. 앞선 step에만 거는 추가 순서 엣지 |
| `steps[].retry` | object | 선택. `{"max_attempts": n}`. n은 1부터 `StepPlanLimits::max_retry_attempts`(3)와 `TaskGraphProposalLimits::max_attempts` 중 작은 값까지 |

리터럴은 모든 JSON 값입니다. 문자열, 유한한 숫자, boolean, null, 배열, 객체가 해당합니다. 리터럴 하나는 정규 JSON으로 `max_literal_bytes`(4096) 이하이고, 계획 전체의 리터럴은 합쳐서 `max_total_literal_bytes`(32768) 이하이며, 중첩은 `max_literal_depth`(6) 이하입니다. 호스트의 `TaskGraphProposalLimits::max_literal_bytes`와 `max_total_literal_bytes`도 넘을 수 없고, 리터럴 안의 어떤 객체도 `from_step` 키를 가질 수 없습니다.

참조와 `after`는 앞에 나열된 step만 가리킬 수 있습니다. 그래서 의존 그래프는 구조상 비순환이며, 형식에 순환 코드가 없고, 모델이 따를 규칙도 하나뿐입니다. 서로 독립인 step은 병렬로 남습니다. 엣지는 참조나 `after`가 만들 때만 생깁니다.

### step plan이 말할 수 없는 것

| 형식에 없는 것 | 이유 |
|---|---|
| provider, 모델, endpoint, URL, 자격 증명, 헤더 | 신뢰와 권한은 호스트 정책입니다. 템플릿 id가 유일한 선택자입니다. |
| 코드, 스크립트, 명령, 표현식, 템플릿 언어 | 계획은 데이터입니다. 참조는 필드를 고를 뿐 아무것도 평가하지 않습니다. |
| 예산, 타임아웃, capability/effect 목록, child binding | 호스트가 템플릿 계약과 정책에서 정합니다. |
| join 정책 | task graph는 all-of로 join합니다. |
| 조건문, 반복문, 중첩 계획 | 형식을 작게 유지합니다. 분기는 계획을 실행하는 Program의 몫입니다. |

권한을 가리키는 키(`provider`, `endpoint`, `url`, `api_key`, `token`, `secret`, `credential`, `command`, `code`, `script`, `model`, `budget`, `timeout`)에는 `STEP_FORBIDDEN_FIELD` 코드와 호스트가 정하는 것이라는 힌트가 붙습니다. 이 목록은 힌트를 개선할 뿐입니다. 권한이 계획에 들어오지 못하게 막는 것은 닫힌 키 집합입니다.

## 계획 검사

```cpp
StepPlanIntakeOptions options;
options.source_id          = "planner:turn-1";
options.proposal_limits    = policy.limits;              // the limits the host gives TaskGraphProposal::parse
options.template_allowlist = policy.template_allowlist;  // the same trusted list

const auto result = intake_step_plan(model_text, options);
if (result.ok()) {
    const json& proposal = *result.proposal;             // schema 2, canonical; result.proposal_hash is its identity
} else {
    const auto feedback = format_violations_for_model(result.violations, 2048);
}
```

`intake_step_plan`은 planner의 텍스트나 이미 파싱한 `json`을 받습니다. 텍스트에서는 맨 처음 나오는 최상위 JSON 객체를 쓰며, 코드 펜스 안에 있어도 되고 설명문 뒤에 있어도 됩니다. `result.envelope`는 객체를 찾은 방법을 알려 줍니다. 텍스트 전체가 객체이면 `Strict`, 그렇지 않으면 `Recovered`입니다. 한 객체에서 키가 두 번 나오면 `STEP_DUPLICATE_KEY`입니다. 파싱된 `json`을 받는 오버로드는 중복 키를 볼 수 없습니다.

결과에는 정렬된 violation, `plan_digest`(정규화한 계획의 sha256 아이덴티티), 변환된 `proposal`과 `proposal_hash`, 변환된 proposal의 포인터를 계획의 step으로 되돌리는 `source_map`, 그리고 `steps`, `edges`, `depth`, `width`, `literal_bytes` 수치를 담은 `report`가 있습니다. intake는 결정적입니다. 같은 문서 값(키 순서와 공백은 무관)과 같은 옵션이면 violation도 proposal 바이트도 같습니다. 계획의 내용 때문에 예외를 던지지 않고, 옵션 자체가 잘못되었을 때만 `std::invalid_argument`를 던집니다.

`StepPlanIntakeOptions::template_info`는 참고용이며 거부를 더할 수만 있습니다. `required_params`는 빠진 입력을 violation으로 만들고, `summary`는 planner 프롬프트에 쓸 텍스트입니다. `step_plan_json_schema(templates, limits)`는 카탈로그에 맞춘 JSON Schema를 만듭니다. `use`는 허용된 id의 enum이고, `params`는 템플릿마다 입력 이름을 나열합니다. 구조화된 출력을 강제하는 provider용이며, 검증기가 여전히 최종 권한을 가집니다. [`schemas/step-plan-v1.schema.json`](../schemas/step-plan-v1.schema.json)은 카탈로그와 무관한 형태입니다.

## violation

violation은 `{path, code, message, hint, detail}`입니다. `path`는 제출된 문서를 가리키는 JSON Pointer입니다(`""`는 문서 전체). 메시지에는 id, 템플릿 이름, 코드만 들어갑니다. 계획에서 가져온 문자열은 64바이트로 자르고 출력 가능한 문자만 남긴 뒤 보여 주며, 리터럴 값은 절대 보여 주지 않습니다. 그래서 피드백에 모델이 쓴 지시가 실려 다음 프롬프트로 돌아가지 못합니다. 힌트는 명령문 한 문장이고 유효한 이름을 최대 다섯 개 나열합니다.

목록은 path 순으로 정렬하되 세그먼트별로 비교하고, 숫자 세그먼트끼리는 정수로 비교합니다(`/steps/2`가 `/steps/10`보다 앞). 그다음 코드, detail 순입니다. `max_violations`(32)에서 자르고, 빠진 것이 있으면 `STEP_VIOLATIONS_TRUNCATED`로 끝납니다. 구조에서 violation이 하나라도 나오면 계획 단위 한도(`STEP_LIMIT_EDGES`, `STEP_LIMIT_DEPTH`, `STEP_LIMIT_BUDGET`)는 계산하지 않습니다. 건전한 그래프가 있어야 계산할 수 있기 때문입니다.

| 코드 | path | 의미 |
|---|---|---|
| `STEP_NOT_JSON` | `""` | 텍스트에 완전하고 올바른 JSON 객체가 없음 |
| `STEP_DUPLICATE_KEY` | 두 번째 키 | 객체에서 키가 반복됨 |
| `STEP_TYPE` | 해당 값 | JSON 타입이 틀림. `detail.expected`가 맞는 타입을 알려 줌 |
| `STEP_SCHEMA` | `/schema` | 없거나 `step-plan/v1`이 아님 |
| `STEP_UNKNOWN_FIELD` | 해당 키 | 닫힌 집합에 없는 키 |
| `STEP_FORBIDDEN_FIELD` | 해당 키 | 권한을 가리키는 단어. 호스트가 정함 |
| `STEP_EMPTY`, `STEP_LIMIT_STEPS` | `/steps` | step이 없거나 너무 많음 |
| `STEP_LIMIT_PARAMS`, `STEP_LIMIT_AFTER`, `STEP_LIMIT_DOCUMENT` | 해당 목록 또는 `""` | step의 입력이나 `after` 항목이 한도보다 많거나, 문서가 `max_document_bytes`보다 크거나 너무 깊게 중첩됨 |
| `STEP_ID_REQUIRED`, `STEP_ID_FORMAT`, `STEP_ID_DUPLICATE` | `/steps/i/id` | id 문제 |
| `STEP_USE_REQUIRED`, `STEP_USE_UNKNOWN` | `/steps/i/use` | 없거나 allow-list에 없음. 힌트가 가장 가까운 id를 알려 줌 |
| `STEP_PARAM_UNKNOWN`, `STEP_PARAM_MISSING` | `/steps/i/params/key` | 템플릿에 그런 입력이 없거나 필수 입력이 없음 |
| `STEP_LITERAL_TOO_LARGE`, `STEP_LITERAL_DEPTH`, `STEP_LITERAL_RESERVED_KEY`, `STEP_LITERAL_NUMBER` | 해당 param | 리터럴 한도(리터럴을 받지 않는 호스트 포함), 리터럴 안의 `from_step` 키, 유한하지 않은 숫자 |
| `STEP_REF_SYNTAX` | 해당 참조 | 잘못된 참조 객체 |
| `STEP_REF_UNKNOWN_STEP`, `STEP_REF_FORWARD` | `.../from_step` | step이 아니거나 앞선 step이 아님(step은 자기 자신을 가리킬 수 없음) |
| `STEP_REF_OUTPUT_UNKNOWN`, `STEP_REF_FIELD_UNKNOWN`, `STEP_REF_FIELD_AMBIGUOUS`, `STEP_REF_OUT_MISMATCH` | `.../output`, `.../field` 또는 해당 param | 출력이나 필드가 생산자의 계약에 없음. 여럿인데 고르지 않았음(힌트가 나열함). `output`이 생산자의 `out`과 다름 |
| `STEP_OUT_UNKNOWN` | `/steps/i/out` | 템플릿에 선언된 artifact가 아님 |
| `STEP_AFTER_UNKNOWN`, `STEP_AFTER_FORWARD`, `STEP_AFTER_DUPLICATE` | `/steps/i/after/j` | 순서 엣지 문제 |
| `STEP_RETRY_FIELD`, `STEP_RETRY_RANGE` | `/steps/i/retry/...` | `max_attempts` 이외의 키이거나 허용 범위 밖의 값 |
| `STEP_LIMIT_EDGES`, `STEP_LIMIT_DEPTH`, `STEP_LIMIT_BUDGET` | `/steps` | 계획 단위 상한. 의존 수, 가장 긴 사슬, step별 예약 예산의 합(재시도는 시도마다 한 번씩 셈) |
| `STEP_LOWERING_REJECTED` | 대응하는 step | `TaskGraphProposal::parse`가 변환 결과를 거부함. `detail.proposal_code`에 `P_PROPOSAL_*` 코드가 있음. intake가 검사하는 한도와 호스트 정책이 다르거나 intake에 빈틈이 있다는 뜻이며, 최종 권한은 parse에 있음 |
| `STEP_VIOLATIONS_TRUNCATED` | `""` | 목록이 잘림 |

모델이 받는 내용의 예입니다.

```json
[
  {"path": "/steps/2/use", "code": "STEP_USE_UNKNOWN",
   "message": "Template 'web.serach' is not available.",
   "hint": "Use one of: text.summarize/v1, web.search/v2. Closest: web.search/v2.",
   "detail": {"given": "web.serach", "choices": ["text.summarize/v1", "web.search/v2"]}},
  {"path": "/steps/3/params/text/from_step", "code": "STEP_REF_FORWARD",
   "message": "Step 's4' does not come before this step.",
   "hint": "Reference only steps listed earlier in 'steps'; reorder the steps or drop the reference."}
]
```

## 변환(lowering)

변환은 문서와 옵션만으로 결정되는 순수 함수입니다.

- task는 step 순서를 따릅니다. 참조는 입력 binding이 되고, 리터럴은 스키마 2 리터럴이 되며, `after`와 참조의 생산자는 `depends_on`(정렬, 중복 없음)이 됩니다.
- planner는 예산을 정하지 않습니다. 각 step은 모든 차원에서 템플릿 상한과 호스트의 task별 상한 중 작은 값을 예약하고, 네 차원이 모두 명시됩니다. `n`번 시도하는 재시도는 이를 `n`번 예약합니다.
- proposal은 항상 스키마 2이므로 호스트의 시작 예산이 정확하며(아래 참고), intake가 돌려주기 전에 `TaskGraphProposal::parse`로 파싱됩니다. 돌려주는 `proposal`은 이미 정규 형태입니다.

## 스키마 2 proposal

`TaskGraphProposal` 스키마 2는 step plan이 변환되는 형식이며, planner가 직접 써도 됩니다([`schemas/task-graph-proposal-v2.schema.json`](../schemas/task-graph-proposal-v2.schema.json) 참고). 스키마 1 문서는 정규 바이트와 아이덴티티가 그대로입니다. 스키마 1이라고 선언하고 스키마 2 멤버를 쓰는 문서는 `P_PROPOSAL_SCHEMA_VERSION`입니다.

### 명시적 예산 (#412)

스키마 1에서 예산 차원을 0으로 둔 task는 proposal과 fragment의 상한에서 0으로 집계되었지만, 자식은 그 차원에 대해 link 상한 전체를 받고 시작했고, 비워 둔 `output_bytes`는 어떤 출력이든 허용했습니다. 스키마 2는 이 틈을 닫습니다.

- task는 템플릿 상한이 0이 아닌 모든 차원에 0이 아닌 값을 명시해야 합니다(`P_PROPOSAL_BUDGET_ZERO`). 템플릿 상한이 0인 차원만 0으로 남길 수 있습니다.
- 스키마 2 fragment는 각 task를 예약한 값 그대로 시작합니다. 0인 차원은 `RunBudget`처럼 아무것도 허용하지 않고, 출력은 명시한 `output_bytes`로 제한됩니다.

| | 스키마 1 | 스키마 2 |
|---|---|---|
| 비워 둔 예산 차원 | 허용. 0으로 집계. 자식은 link 상한을 받음 | 템플릿이 쓸 수 있는 차원이면 `P_PROPOSAL_BUDGET_ZERO` |
| `output_bytes` 0 | 어떤 출력이든 허용 | 어떤 출력도 들어가지 않음 |
| 기존 문서의 정규 바이트와 아이덴티티 | 변함없음 | 해당 없음 |

planner에게서 proposal을 받는 호스트는 `TaskGraphProposalLimits::min_schema_version`을 2로 두어 스키마 1을 거부합니다.

### 리터럴 입력

task는 `literals`를 가질 수 있습니다. `[{"to": {"field": "/question"}, "value": ...}]` 형태입니다. 리터럴은 템플릿에 선언된 입력 필드 하나에 JSON 값을 대입하며, 자식의 입력에서 binding 다음에 적용됩니다. 대상은 템플릿의 입력이어야 하고(`P_PROPOSAL_LITERAL_TARGET`), 다른 binding이나 리터럴을 받지 않아야 합니다(`P_PROPOSAL_LITERAL_DUPLICATE_TARGET`). 리터럴은 데이터이며 평가되지 않습니다. 정규 형태에서는 대상 순으로 정렬됩니다.

### 시도 횟수

task는 `max_attempts`를 선언할 수 있습니다(1부터 호스트의 `TaskGraphProposalLimits::max_attempts`까지, `P_PROPOSAL_ATTEMPTS`). 시도마다 task의 예산 전체를 가진 새 자식을 시작하므로, 집계는 예산을 시도 횟수만큼 예약합니다. task의 `reserved_budget`은 `budget`에 `max_attempts`를 곱한 값이고, proposal과 fragment 모두 그 합을 상한과 비교합니다.

### 호스트 한도

| `TaskGraphProposalLimits` | 기본값 | 의미 |
|---|---|---|
| `min_schema_version` | 1 | 받아들이는 가장 낮은 스키마 |
| `max_attempts` | 1 | task가 선언할 수 있는 최대 시도 횟수. 1이면 재시도 없음 |
| `max_literal_bytes` | 0 | 리터럴 하나의 정규 JSON 바이트. 0이면 리터럴을 받지 않음 |
| `max_total_literal_bytes` | 0 | proposal의 모든 리터럴의 정규 JSON 바이트 합계 |

거부된 expansion은 처음 몇 개 diagnostic을 `failure.witness.diagnostics`에 `{code, pointer}`로 보고합니다.

## task 재시도

task 묶음이 끝난 뒤, 자식이 노드 실패 분류기가 재시도 가능하다고 판정한 실패로 끝났고(`failure.witness.node_failure.retryable`이 true이며 `kind`가 `effect_committed`, `ceiling`, `deadline`이 아님. plan 수준 `retry` 연산과 같은 규칙) 남은 시도가 있는 task는 다음 시도 번호로 다시 디스패치됩니다. 취소되었거나 시간 초과된 task, 노드 실패가 아닌 실패(없는 입력, 계약을 벗어난 출력)는 재시도하지 않습니다.

- 종료 상태는 한번 정해지면 바뀌지 않으므로, 재시도할 시도에 대해서는 `Failed`를 기록하지 않습니다. 다음 디스패치가 task를 다음 시도 번호와 새 시도 아이덴티티로 `Active`로 기록하고, 이전 시도의 실패를 다음 결과가 대체할 때까지 레코드에 남겨 둡니다. 시도 번호는 줄어들지 않습니다.
- 시도마다 별도의 자식이 있으며, run id는 시도 아이덴티티에서 파생하고 task 예산 전체를 받습니다.
- 재시도는 그래프를 다음에 스캔할 때 일어나며, 새로 준비된 task와 같은 방식입니다. 재시도되는 task에 의존하는 task는 기다리고, `fail_fast` 정책에서도 재시도 가능한 실패는 다른 task를 취소하지 않습니다.
- 다음 시도가 시작되기 전에 크래시가 나면 같은 시도를 다시 시작합니다. 그 자식이 이미 실패를 가지고 있으므로 같은 판정이 다시 내려집니다.

## plan repair

모델의 첫 계획은 올바르지 않은 경우가 많습니다. plan repair는 violation을 프롬프트에 넣어 정해진 횟수까지 다시 묻습니다. 새 plan 연산이 아니라 기존 메커니즘 위의 패턴입니다. Core 그래프 `plan -> intake -> (plan again)`이며, `intake`는 등록된 순수 노드입니다. 실행의 예산, provider 호출 저널, Core 체크포인트가 루프에 필요한 것을 줍니다. 횟수 제한, 토큰과 비용의 상한, 마감 시각, 기록된 상태이며, 그래서 재개된 run은 이미 받은 답을 모델에게 다시 묻지 않습니다.

```json
{
  "schema_version": 1,
  "name": "main",
  "channels": {
    "messages": {"reducer": "overwrite", "initial": [{"role": "user", "content": "..."}]},
    "proposal": {"reducer": "overwrite", "initial": null},
    "repair": {"reducer": "overwrite", "initial": {}}
  },
  "nodes": {
    "plan": {"type": "llm_call"},
    "intake": {"type": "neograph.step_plan_intake/v1", "max_attempts": 3, "feedback": "replace"}
  },
  "edges": [
    {"from": "__start__", "to": "plan"},
    {"from": "plan", "to": "intake"}
  ],
  "conditional_edges": [
    {"from": "intake", "condition": "neograph.step_plan_route/v1",
     "routes": {"ok": "__end__", "retry": "plan", "give_up": "__end__"}}
  ],
  "node_retry_policies": {"plan": {"max_visits": 3}}
}
```

Program 안에서는 Core 정의의 `name`이 그 정의를 담는 루트 연산의 이름(여기서는 `main`)과 같아야 합니다. `plan`은 `messages`를 읽고 모델의 답을 assistant 메시지로 덧붙이는 노드면 무엇이든 됩니다. 이후 Program은 `"proposal_source": {"artifact": "input", "field": "/channels/proposal/value"}`로 planner의 출력에서 받아들여진 proposal을 expand합니다. [`examples/65_program_plan_intake.cpp`](../examples/65_program_plan_intake.cpp)는 이 흐름을 스크립트된 planner로 오프라인에서 실행합니다. 실수가 세 개 든 첫 응답, 수정 턴 한 번, 받아들여진 계획이 자식 Program으로 expand되는 과정입니다.

### 등록되는 실행체

`add_step_plan_repair(builder, options)`는 `RegistrySnapshotBuilder`에 실행체 세 개를 등록합니다. `StepPlanRepairNodeOptions::intake`가 신뢰하는 allow-list와 한도를 담습니다. 봉인된 그래프는 이를 바꿀 수 없고 실행체의 아이덴티티가 이를 포함하므로, 한 allow-list로 승인된 Program이 다른 allow-list로 실행되지 않습니다.

| 실행체 | 역할 |
|---|---|
| `neograph.step_plan_intake/v1` (노드) | `messages`의 마지막 assistant 메시지를 읽어 `intake_step_plan`을 실행하고, `proposal`(변환된 proposal 또는 null)과 `repair`를 쓰며, 다음 시도가 있으면 `messages`도 씁니다. 설정은 `max_attempts`(1부터 `max_attempts_ceiling`까지, 기본 3)와 `feedback`입니다. |
| `neograph.step_plan_route/v1` (조건) | `repair.status`에 따라 `ok`, `retry`, `give_up`을 반환합니다. 그 밖의 값은 `give_up`입니다. |
| `neograph.step_plan_report/v1` (노드) | `repair.status`를 `exhausted`로 다시 쓰고 다른 것은 하지 않습니다. 루프를 failure exit로 끝내는 그래프에서 씁니다. |

`feedback: "replace"`(기본값)는 시도마다 `messages`를 system 메시지, 첫 user 메시지, 거부된 계획, violation으로 다시 만듭니다. 그래서 컨텍스트가 커지지 않고 이전 실수를 다시 읽지 않습니다. `"append"`는 대화 전체를 유지하고 violation을 덧붙입니다. 노드가 `messages`를 통째로 쓰므로 채널의 reducer는 상관없습니다. violation은 `format_violations_for_model`로 렌더링하며 `max_feedback_bytes`(2048)로 제한됩니다.

### 보고서

`repair` 채널에는 보고서가 들어 있으며, `repair_step_plan`이 돌려주는 값과 같습니다.

```json
{"status": "exhausted", "attempts": 3, "max_attempts": 3,
 "last_violations": [{"path": "...", "code": "STEP_USE_UNKNOWN", "message": "...", "hint": "...", "detail": {}}],
 "violation_code_histogram": {"STEP_USE_UNKNOWN": 3, "STEP_REF_FORWARD": 1},
 "plan_digests": ["sha256:...", "sha256:...", "sha256:..."],
 "envelope": ["strict", "recovered", "strict"]}
```

`status`는 `ok`, `retry`, `exhausted` 중 하나입니다. `last_violations`는 8 KiB에서 잘립니다.

### 한도와 최종 실패

| 한도 | 메커니즘 |
|---|---|
| 시도 횟수 | 호스트 상한 안에서 intake 노드의 `max_attempts`. `plan`의 `max_visits`는 위 로직이 틀렸을 때 run을 실패시키는 안전망 |
| 토큰과 비용 | run의 예산. planner 그래프를 자식 Program(`spawn`을 `await`로 join)으로 실행하면 계획 단계에 별도의 상한을 줄 수 있음 |
| 경과 시간 | run의 `wall_time_ms`와 모델 호출마다의 `attempt_timeout_ms` |
| 모델 호출 재시도 | `plan` 노드의 `max_retries`. 위의 시도 횟수와는 별개 |

마지막 시도 뒤에 끝내는 방법은 두 가지입니다.

- 대체 경로(사용자와 마주하는 흐름에 권장). `give_up` 경로가 `__end__`로 가고, 보고서는 `exhausted`라고 말하며, 부모 Program이 `/channels/repair/value/status`로 분기해 직접 답하거나 사용자에게 묻습니다. fragment는 게시되지 않습니다.
- 실패. `give_up` 경로가 코드 `plan.repair_exhausted`인 failure exit로 선언된 `neograph.step_plan_report/v1` 노드로 갑니다. run은 그 코드로 `Failed`가 되고 출력에는 여전히 보고서가 있습니다.

예산 때문에 멈추면 기존의 `BudgetExhausted` 상태와 코드가 나오므로, 호스트는 "모델이 고치지 못했다"와 "돈이 떨어졌다"를 구분할 수 있습니다.

### run 전에 계획하기

run을 시작하기 전에 계획하는 호스트는 `repair_step_plan`을 씁니다.

```cpp
PlanRepairOptions options;
options.intake       = intake_options;
options.max_attempts = 3;
const auto result = repair_step_plan(options, request_text,
    [&](const std::vector<PlannerMessage>& messages) {
        return PlannerReply{call_model(messages), prompt_tokens, completion_tokens};
    });
if (result.ok) start_run(result.proposal);   // the proposal becomes the run input
```

받아들여진 proposal이 run의 입력이 되고 run 레코드가 이를 저장하므로, 재개해도 다시 계획하지 않습니다. 비용 집계는 호스트 몫입니다. run의 예산에는 계획이 포함되지 않습니다. 계획의 비용이 run에 계상되어야 하면 Core 그래프를 쓰세요.

## 호환성과 롤아웃

트리는 v1 이전입니다. 이 변경은 공개 구조체에 멤버를 추가하므로 NeoGraph, 모든 C++ 사용 코드, Python 확장을 함께 재빌드하세요([`docs/ABI_POLICY.md`](ABI_POLICY.md)와 변경 로그 항목 참고).

| 레코드 | 규칙 |
|---|---|
| 스키마 1 proposal | 정규 바이트와 아이덴티티 변함없음 |
| 스키마 2 proposal | 새로 추가됨. 이전 바이너리는 `P_PROPOSAL_SCHEMA_VERSION`으로 거부함 |
| 스키마 1 fragment와 그 저장 레코드 | 변함없고, 이전과 똑같이 시작됨(비워 둔 차원은 link 상한을 받음) |
| 스키마 2 fragment | 새로 추가됨. 이전 바이너리는 파싱을 거부함 |

저장된 데이터를 마이그레이션할 필요는 없습니다. 어떤 쓰기 주체가 스키마 2 proposal, 리터럴, 재시도를 만들기 시작하기 전에, run을 복구할 수 있는 모든 프로세스를 먼저 업그레이드하세요. 스키마 2 fragment가 생긴 뒤에 롤백하려면 먼저 그 expansion을 모두 끝내야 합니다.

이번 릴리스에 없는 것은 실행 중 그래프 변경, SQLite나 PostgreSQL용 내구성 있는 fragment 저장소, planner를 위한 템플릿 비용 카탈로그 뷰입니다.
