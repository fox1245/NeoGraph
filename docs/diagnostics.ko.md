<!-- neograph-i18n: source=docs/diagnostics.md locale=ko source_sha256=19c234158e09885ff4fb5bc8e8882e0e7b88c44a47398dab835822e3c65e466e -->
# Program 진단

**Languages:** [English](diagnostics.md) | [한국어](diagnostics.ko.md) | [日本語](diagnostics.ja.md) | [简体中文](diagnostics.zh-CN.md)

Program을 컴파일하면 진단이 반환됩니다. 컴파일이 실패하면 `ProgramCompileError::diagnostics()`로, 성공하면 `ProgramBundle::compile_diagnostics()`로, Python 예외에서는 `.diagnostics`로 얻습니다. 이 문서는 각 코드의 의미와 해결 방법을 설명합니다. 반환된 진단은 모두 `help.doc`으로 이 문서의 해당 항목에 연결됩니다.

- [help 객체](#the-help-object)
- [Program 문서](#program-document)
- [JavaScript 프런트엔드](#javascript-frontend)
- [Core 파서](#core-parser)
- [Core 검증기](#core-validator)
- [건전성 검사](#soundness-check)
- [레지스트리](#registry)
- [런타임과 승인(admission)](#runtime-and-admission)

<a id="the-help-object"></a>
## help 객체

반환된 진단에는 `help`가 들어 있어서, 도구와 모델이 메시지를 파싱하지 않고도 이를 바탕으로 조치할 수 있습니다.

| 필드 | 내용 |
|---|---|
| `doc` | 이 문서에서 해당 코드의 항목으로 가는 링크. 항목이 따로 없으면 그 코드 계열의 섹션으로 가는 링크다. |
| `related` | 같은 근본 원인에 속한 다른 위치. 각 위치에 한 줄짜리 `label`이 붙는다. |
| `suggestions` | 수정 제안. 각 제안은 `message`, `edits`, `applicability`를 가진다. |
| `candidates` | 알 수 없는 이름과 편집 거리 2 이내인 선언된 이름. 가까운 순서다. |

`edits`는 컴파일러가 파싱한 Program 문서에 대한 RFC 6902 JSON Patch 연산(`add`, `replace`, `remove`, `move`)입니다. JavaScript 소스라면 `define()`이 생성한 문서이며, 진단의 span은 빌더 호출 위치를 가리킵니다. `applicability`는 `machine`(그대로 적용), `maybe`(먼저 검토, 기본값), `placeholder`(`<node>` 같은 값을 채워야 함) 중 하나입니다.

bundle에 봉인되는 진단에는 `help`가 없으므로 bundle id는 그 영향을 받지 않습니다. 컴파일은 근본 원인마다 진단 하나를 반환합니다. 여러 진단이 같은 알 수 없는 이름을 보고하면 첫 번째만 남기고 나머지는 그 진단의 `related` 항목이 됩니다. 진단은 최대 20개까지 반환되며 오류가 먼저 나옵니다. 나머지 개수는 `P_DIAGNOSTICS_TRUNCATED` 메모에 적혀 있습니다.

메시지에는 작성자가 쓴 표현이 그대로 쓰입니다. JavaScript 소스라면 `graph.barrier("D", [...])` 같은 빌더 호출이, JSON이나 C++ 빌더 문서라면 Core 키가 쓰입니다.

<a id="program-document"></a>
## Program 문서

| 코드 | 의미와 해결 방법 |
|---|---|
| <a id="p_schema_version"></a>`P_SCHEMA_VERSION` | Program, 계약, 포함된 Core 정의 중 하나의 스키마 버전이 지원되지 않거나 없다. 컴파일러가 witness에 나열한 버전을 쓴다. |
| <a id="p_schema_unknown_field"></a>`P_SCHEMA_UNKNOWN_FIELD` | 닫힌 Program 객체에 정의되지 않은 필드가 있다. 그 필드를 지우거나 candidate를 쓴다. |
| <a id="p_schema_required"></a>`P_SCHEMA_REQUIRED` | 필수 Program 필드가 없다. 추가한다. |
| <a id="p_schema_type"></a>`P_SCHEMA_TYPE` | Program 필드의 JSON 타입이 틀렸거나, 소스 맵에 같은 포인터가 반복된다. witness에 나온 타입을 쓴다. |
| <a id="p_schema_operation"></a>`P_SCHEMA_OPERATION` | 이 연산은 더 새로운 Program 스키마 버전이 필요하다. `program_schema_version`을 올리거나 다른 연산을 쓴다. |
| <a id="p_contract_schema"></a>`P_CONTRACT_SCHEMA` | 입력 또는 출력 계약이 올바르지 않거나 지원되지 않는 JSON Schema 키워드를 쓴다. |
| <a id="p_root_name"></a>`P_ROOT_NAME` | 루트 이름이 없거나 비어 있거나 Core 정의의 `name`과 다르다. 둘을 같게 맞춘다. |
| <a id="p_root_operation"></a>`P_ROOT_OPERATION` | `call_core` 루트에 Core 노드가 없다. 노드를 하나 이상 선언한다. |
| <a id="p_plan_operation"></a>`P_PLAN_OPERATION` | 지원되지 않는 연산이다. |
| <a id="p_plan_core"></a>`P_PLAN_CORE` | plan이 봉인된 루트가 아닌 Core를 호출하거나, 어떤 Core도 호출하지 않으면서 자식도 디스패치하지 않는다. |
| <a id="p_plan_bound"></a>`P_PLAN_BOUND` | 루프, 재시도, 타임아웃의 한도가 허용 범위를 벗어난다. |
| <a id="p_plan_condition"></a>`P_PLAN_CONDITION` | plan 조건에 조건식(predicate)이 정확히 하나 있지 않거나, 경로가 JSON Pointer가 아니다. |
| <a id="p_plan_race_arity"></a>`P_PLAN_RACE_ARITY` | race의 분기가 두 개가 아니다. |
| <a id="p_plan_quorum"></a>`P_PLAN_QUORUM` | `min_success`가 0이거나 분기 수보다 크다. |
| <a id="p_plan_spawn_shape"></a>`P_PLAN_SPAWN_SHAPE` | spawn에 인라인 본문이 있거나, await의 직접 본문이 아니다. |
| <a id="p_plan_cancel_scope"></a>`P_PLAN_CANCEL_SCOPE` | cancel은 `run` 범위만 지원한다. |
| <a id="p_plan_reference"></a>`P_PLAN_REFERENCE` | 봉인된 연산이 존재하지 않는 연산을 참조한다. 컴파일러 결함이므로 보고한다. |
| <a id="p_plan_source_pointer"></a>`P_PLAN_SOURCE_POINTER` | 봉인된 연산의 소스 포인터가 올바르지 않다. 컴파일러 결함이므로 보고한다. |
| <a id="p_plan_typed"></a>`P_PLAN_TYPED` | plan을 타입이 있는 연산으로 봉인할 수 없었다. 자세한 내용은 witness에 있다. |
| <a id="p_parallel_map_source"></a>`P_PARALLEL_MAP_SOURCE` | `item_source`에는 크기가 제한된 리터럴 배열이나 입력 artifact 필드가 필요하다. |
| <a id="p_parallel_map_binding"></a>`P_PARALLEL_MAP_BINDING` | `parallel_map`의 바인딩이나 엔드포인트가 없거나 JSON Pointer가 아니다. |
| <a id="p_parallel_map_bound"></a>`P_PARALLEL_MAP_BOUND` | `parallel_map`의 한도가 범위를 벗어나거나 `max_in_flight`가 `max_items`를 넘는다. |
| <a id="p_parallel_map_failure"></a>`P_PARALLEL_MAP_FAILURE` | `failure_policy`는 `fail_fast` 또는 `collect`여야 한다. |
| <a id="p_expand_source"></a>`P_EXPAND_SOURCE` | `proposal_source`에는 인라인 객체나 입력 JSON Pointer가 필요하다. |
| <a id="p_expand_bound"></a>`P_EXPAND_BOUND` | `expand_task_graph`의 한도가 범위를 벗어난다. |
| <a id="p_expand_failure"></a>`P_EXPAND_FAILURE` | `failure_policy`는 `fail_fast` 또는 `collect`여야 한다. |
| <a id="p_budget_invalid"></a>`P_BUDGET_INVALID` | 예산 레코드가 불완전하거나, 반복되거나, 최솟값과 최댓값이 뒤집혔거나, 구조상 필요한 최소치보다 낮다. 아홉 가지 리소스를 모두 한 번씩 선언한다. |
| <a id="p_budget_v1"></a>`P_BUDGET_V1` | Program 스키마 1에서는 `max_program_operations`가 정확히 1로 고정된다. |
| <a id="p_budget_static"></a>`P_BUDGET_STATIC` | 선언한 최댓값이 plan이 항상 실행하는 연산을 감당하지 못한다. 최댓값을 올린다. |
| <a id="p_migration_program_json"></a>`P_MIGRATION_PROGRAM_JSON` | Program JSON 연산 트리는 더 이상 받지 않는다. JavaScript나 신뢰할 수 있는 C++ 빌더로 작성한다. |
| <a id="p_compiler_internal"></a>`P_COMPILER_INTERNAL` | 컴파일러가 내부 오류로 실패 시 차단되었다. 소스와 함께 보고한다. |
| <a id="p_diagnostics_truncated"></a>`P_DIAGNOSTICS_TRUNCATED` | 진단이 20개를 넘게 발견되었으며 나머지 개수는 `witness.omitted`에 있다. 표시된 진단을 고친 뒤 다시 컴파일한다. |

<a id="javascript-frontend"></a>
## JavaScript 프런트엔드

| 코드 | 의미와 해결 방법 |
|---|---|
| <a id="p_js_evaluation"></a>`P_JS_EVALUATION` | 소스가 예외를 던졌거나 파싱되지 않았다. span이 실패한 줄을 가리킨다. |
| <a id="p_js_timeout"></a>`P_JS_TIMEOUT` | 평가가 컴파일 시간 한도나 예산의 `wall_time_ms`를 넘었다. |
| <a id="p_js_resource_limit"></a>`P_JS_RESOURCE_LIMIT` | 평가가 메모리 또는 스택 한도를 넘었다. |
| <a id="p_js_cancelled"></a>`P_JS_CANCELLED` | 평가가 취소되었다. |
| <a id="p_js_runtime"></a>`P_JS_RUNTIME` | QuickJS 런타임이나 빌더 호출에서 내부 오류가 났다. |
| <a id="p_js_runtime_identity"></a>`P_JS_RUNTIME_IDENTITY` | 소스가 컴파일러와 다른 QuickJS 빌드나 프로파일용으로 만들어졌다. |
| <a id="p_js_unavailable"></a>`P_JS_UNAVAILABLE` | 이 빌드에는 QuickJS 프런트엔드가 없다. |
| <a id="p_js_ambient_denied"></a>`P_JS_AMBIENT_DENIED` | 소스가 프로파일이 허용하지 않는 ambient 권한(시계, 난수, eval 등)을 사용했다. |
| <a id="p_js_define_missing"></a>`P_JS_DEFINE_MISSING` | 모듈이 `define()`을 export하지 않는다. |
| <a id="p_js_define_async"></a>`P_JS_DEFINE_ASYNC` | `define()`은 동기 함수여야 한다. |
| <a id="p_js_define_value"></a>`P_JS_DEFINE_VALUE` | `define()`은 열려 있는 그래프 빌더를 정확히 하나 반환해야 한다. |
| <a id="p_js_graph_argument"></a>`P_JS_GRAPH_ARGUMENT` | 빌더 메서드가 형태가 맞지 않는 인자, 예약되었거나 중복된 이름, 선언되지 않은 노드를 받았다. 메시지에 메서드 이름이 나온다. |
| <a id="p_js_graph_value"></a>`P_JS_GRAPH_VALUE` | 빌더 인자가 정규 JSON 데이터가 아니다(함수, 클래스 인스턴스, 유한하지 않은 수). |
| <a id="p_js_graph_limit"></a>`P_JS_GRAPH_LIMIT` | 생성된 그래프가 바이트 한도를 넘는다. |
| <a id="p_js_graph_owner"></a>`P_JS_GRAPH_OWNER` | 이 정의의 빌더가 아닌 객체에서 빌더 메서드를 호출했다. |
| <a id="p_js_graph_sealed"></a>`P_JS_GRAPH_SEALED` | `define()`이 반환된 뒤에 빌더 메서드를 호출했다. |
| <a id="p_js_graph_entry"></a>`P_JS_GRAPH_ENTRY` | 그래프에 노드는 있지만 `.entry(node)`가 없어서 아무것도 실행되지 않는다. |
| <a id="p_js_control_character"></a>`P_JS_CONTROL_CHARACTER` | 설정 문자열에 제어 문자가 들어 있다. 정규식을 일반 문자열로 쓴 경우가 많다. `/.../.source`나 ``String.raw`...` ``를 쓴다. |
| <a id="p_js_control_main"></a>`P_JS_CONTROL_MAIN` | export한 `main`이 함수가 아니다. |
| <a id="p_js_control_async"></a>`P_JS_CONTROL_ASYNC` | `main`은 동기 제너레이터여야 한다. |
| <a id="p_js_control_value"></a>`P_JS_CONTROL_VALUE` | `main`이 봉인된 command나 정규 JSON이 아닌 값을 yield하거나 반환했다. |
| <a id="p_js_control_command"></a>`P_JS_CONTROL_COMMAND` | command를 올바르지 않은 인자로 만들었다. |
| <a id="p_js_control_finished"></a>`P_JS_CONTROL_FINISHED` | 제어 제너레이터가 끝난 뒤에 다시 재개되었다. |
| <a id="p_js_import_dynamic"></a>`P_JS_IMPORT_DYNAMIC` | 동적 `import()`와 `require()`는 허용되지 않는다. |
| <a id="p_js_import_invalid"></a>`P_JS_IMPORT_INVALID` | import 지정자의 형식이 올바르지 않다. |
| <a id="p_js_import_unavailable"></a>`P_JS_IMPORT_UNAVAILABLE` | import가 봉인된 모듈 클로저에 포함되어 있지 않다. |
| <a id="p_import_conflict"></a>`P_IMPORT_CONFLICT` | 하나의 import 소스가 두 가지 콘텐츠 identity로 나타난다. |
| <a id="p_import_dynamic"></a>`P_IMPORT_DYNAMIC` | 소스나 봉인된 모듈이 모듈을 동적으로 로드한다. |
| <a id="p_import_unresolved"></a>`P_IMPORT_UNRESOLVED` | import가 검증된 정확한 모듈 receipt에 바인딩되어 있지 않다. 모듈 해석(resolution)을 지정해 컴파일한다. |

<a id="core-parser"></a>
## Core 파서

이 코드들은 Core 정의 파서에서 발생하며, witness에 Core 코드가 `core_code`로 들어 있습니다.

| 코드 | 의미와 해결 방법 |
|---|---|
| <a id="p_core_gc_schema_version"></a>`P_CORE_GC_SCHEMA_VERSION` | Core `schema_version`이 지원되는 0 이상의 정수가 아니다. |
| <a id="p_core_gc_unknown_field"></a>`P_CORE_GC_UNKNOWN_FIELD` | strict Core 객체에 엔진이 읽지 않는 키가 있다. 키를 지우거나 candidate를 쓰거나, 메모라면 `_` 또는 `x-`로 시작하는 주석 키로 옮긴다. |
| <a id="p_core_gc_field_type"></a>`P_CORE_GC_FIELD_TYPE` | Core 필드의 JSON 타입이 틀렸다. |
| <a id="p_core_gc_field_value"></a>`P_CORE_GC_FIELD_VALUE` | Core 필드의 값이 올바르지 않다. 빈 이름, 범위를 벗어난 숫자, 알 수 없는 옵션이 그 예다. |
| <a id="p_core_gc_field_required"></a>`P_CORE_GC_FIELD_REQUIRED` | `type`, `from`, `to` 같은 필수 Core 필드가 없다. |
| <a id="p_core_gc_edge"></a>`P_CORE_GC_EDGE` | 엣지나 조건부 엣지가 객체가 아니거나, 그 routes가 객체가 아니다. |
| <a id="p_core_gc_barrier"></a>`P_CORE_GC_BARRIER` | barrier의 `wait_for`가 없거나 비어 있거나 형식이 올바르지 않다. |
| <a id="p_core_gc_node_config"></a>`P_CORE_GC_NODE_CONFIG` | 노드의 config가 그 노드 타입 스키마의 `required`, `type`, `enum` 규칙에 맞지 않는다. |
| <a id="p_core_gc_reserved_node"></a>`P_CORE_GC_RESERVED_NODE` | `__start__`와 `__end__`는 예약어이므로 노드 이름으로 쓸 수 없다. |
| <a id="p_core_gc_no_entry"></a>`P_CORE_GC_NO_ENTRY` | `__start__`에서 나가는 엣지가 없어서 아무것도 실행되지 않는다. `.entry(node)`를 추가한다. |
| <a id="p_core_gc_registry_miss"></a>`P_CORE_GC_REGISTRY_MISS` | 노드 타입, 리듀서, 조건이 레지스트리 스냅샷에 없다. 등록하거나 candidate를 쓴다. |
| <a id="p_core_gc_metadata_limit"></a>`P_CORE_GC_METADATA_LIMIT` | 그래프 메타데이터가 한도를 넘었다. 한도는 정규 JSON 8 KiB, 중첩 깊이 4, 객체 멤버 총 64개, 키 128바이트다. witness에 `kind`, `limit`, `actual`이 있다. |
| <a id="p_core_gc_metadata_text"></a>`P_CORE_GC_METADATA_TEXT` | 메타데이터의 키나 문자열에 보이지 않는 문자(탭, 줄바꿈, 캐리지 리턴을 제외한 C0 제어 문자, bidi 제어 문자, 너비가 없는 문자, 태그 문자)나 올바르지 않은 UTF-8이 들어 있다. witness에 해당 코드 포인트가 나열된다. |
| <a id="p_core_gc_failure_edge"></a>`P_CORE_GC_FAILURE_EDGE` | 실패 엣지가 대상에 관한 규칙을 어겼다. `witness.reason`은 `sentinel_source`, `duplicate_from`(노드마다 실패 엣지는 하나), `sentinel_target`(대신 실패 종료 노드로 실행을 끝낸다), `self_target`, `join_target`(실패로 도착한 신호는 join 슬롯을 채우지 않는다), `implicit_end_target`(대상에 엣지나 라우트를 주거나 실패 종료 노드로 만든다), `error_channel_reducer`(오류 채널은 `append` 리듀서를 써야 한다) 중 하나다. |
| <a id="p_core_roundtrip"></a>`P_CORE_ROUNDTRIP` | 파싱한 그래프가 정의를 그대로 다시 내보내지 못한다. 무언가 빠지거나 연결이 바뀌었다. 컴파일러 결함이므로 보고한다. |

<a id="core-validator"></a>
## Core 검증기

이 코드들은 그래프가 의미하는 바를 분석합니다. 경고는 반환만 되며, bundle은 처음 봉인될 때 적용된 규칙의 경고를 봉인합니다.

| 코드 | 의미와 해결 방법 |
|---|---|
| <a id="p_core_e3"></a>`P_CORE_E3` | 엣지, 라우트, 인터럽트, barrier가 알 수 없는 노드를 가리키거나, barrier가 자기 자신을 기다린다. candidate를 쓴다. |
| <a id="p_core_e4"></a>`P_CORE_E4` | 노드의 효과 계약이 선언되지 않은 채널에 쓰거나(오류) 읽는다(경고). 그 채널을 선언한다. |
| <a id="p_core_e5"></a>`P_CORE_E5` | fan-out 형제 노드가 같은 overwrite 채널에 쓴다. 마지막으로 쓴 값이 남는데 어느 노드일지 비결정적이다. append 리듀서를 쓴다. |
| <a id="p_core_e6"></a>`P_CORE_E6` | 채널을 읽지도 쓰지도 않거나, 쓰기만 하고 읽지 않거나, 읽기만 하고 쓰지 않으면서 초기값도 없다. |
| <a id="p_core_e7"></a>`P_CORE_E7` | 엣지와 라우트로 `__start__`에서 도달할 수 없는 노드가 있다. |
| <a id="p_core_e8"></a>`P_CORE_E8` | barrier가 기다리는 노드(또는 `any_of` 슬롯의 대안 중 어느 것)가 barrier로 라우팅되지 않아서 join이 발동할 수 없다. |
| <a id="p_core_e9"></a>`P_CORE_E9` | 노드에 일반 들어오는 엣지가 여럿 있는데 barrier가 없다. 이 노드는 도착하는 슈퍼스텝마다 한 번씩 실행된다. AND join이 필요하면 barrier를 추가하고, XOR 병합이면 무시한다. |
| <a id="p_core_e10"></a>`P_CORE_E10` | 조건부 엣지에 routes가 없거나, 조건이 반환하지 않는 레이블을 라우팅하거나, 반환할 수 있는 레이블에 라우트가 없다. |
| <a id="p_core_e11"></a>`P_CORE_E11` | `__end__`로 가는 경로가 없는 순환에 노드들이 갇혀 있다. |
| <a id="p_core_e12"></a>`P_CORE_E12` | 로컬 레지스트리에 리듀서, 노드 타입, 조건이 없다. |
| <a id="p_core_e13"></a>`P_CORE_E13` | 조건부 엣지로 라우팅되는 노드에서 정적 엣지가 나온다. 이 엣지는 발동하지 않는다. 엣지를 지우거나 라우트로 추가한다. |
| <a id="p_core_e14"></a>`P_CORE_E14` | 노드에 두 번째 조건부 엣지가 있다. 첫 번째 엣지만 라우팅에 쓰인다. 둘을 합친다. |
| <a id="p_core_e15"></a>`P_CORE_E15` | barrier 멤버가 join이 아닌 다른 곳으로 라우팅될 수 있다. 그렇게 되면 실행이 barrier 정체로 끝난다. 같은 `any_of` 슬롯의 다른 대안으로 가는 경로는 우회가 아니다. `witness.paths`에 우회 경로가 하나씩 나오고, 그중 join으로 돌아오는 경로가 있으면 `suggested_wait_for`와 반환된 제안이 join 바로 앞의 노드가 그 멤버를 대신하게 한다. |
| <a id="p_core_e16"></a>`P_CORE_E16` | 그래프에 순환이 있는데 이 컴파일은 순환 없는 그래프를 요구한다. |
| <a id="p_core_e17"></a>`P_CORE_E17` | 한 `any_of` 슬롯의 대안 두 개가 모두 실행될 수 있다. fan-out이 join을 거치지 않고 두 대안에 모두 도달하거나, 한 대안이 join으로 들어갔다가 다른 대안으로도 이어진다. 두 번째 도착은 끝나지 않는 라운드를 열고 실행은 barrier 정체로 끝난다. 예를 들어 조건부 엣지를 써서 대안들이 서로 배타적인 경로가 되게 한다. witness에 fan-out 노드와 두 경로가 나온다. |
| <a id="p_core_e18"></a>`P_CORE_E18` | 노드 config에 그 노드 타입이 선언하지 않은 `retry_policy` 키가 있어서 아무도 읽지 않는다. 노드의 정책은 `graph.retryPolicy(node, {...})`(Core에서는 `node_retry_policies`)로 지정한다. |
| <a id="p_core_routes"></a>`P_CORE_ROUTES` | `ProgramCompilerConfig::echo_routes`(Python에서는 `echo_routes=True`)를 설정했을 때 반환되는 메모다. `witness.routes`는 각 노드와 `__start__`를 스케줄러가 라우팅할 수 있는 대상에 대응시킨다. 봉인되지 않는다. |

<a id="soundness-check"></a>
## 건전성 검사

이 코드들은 선택적으로 켜는 건전성 검사 `ProgramCompilerConfig::soundness_check`(Python에서는 `soundness_check=True`)에서 발생합니다. [Program 그래프 기능](program-graphs.md#soundness-check)을 참고하세요. 반환만 되며 봉인되지 않습니다. `witness.trace`는 발견한 문제를 보여 주는 가장 짧은 실행의 슈퍼스텝을 나열합니다. `ready`는 실행된 노드이고, `routes`는 선택지가 있는 노드가 택한 라우트입니다(실패 엣지는 `on_failure`, `max_visits`가 강제한 경우는 `visit_limit`). `witness.choices`는 그 라우트 선택을, 각 선택을 선언한 엣지의 Core 포인터와 함께 나열하고, `help.related`가 그 위치들을 가리킵니다.

| 코드 | 의미와 해결 방법 |
|---|---|
| <a id="p_core_s1"></a>`P_CORE_S1` | 완료 가능성(option to complete). 트레이스가 도달하는 상태(`witness.state`)에서는 어떤 실행도 완료될 수 없다. 모든 후속 경로가 `stalls_at`에 있는 join에서 barrier 정체로 끝나거나, 방문 한도에서 실패하거나, 루프에 빠지며, 어느 경우인지는 `ends`에 나온다. 모든 경로가 제공하는 것을 join이 기다리게 하거나(예: `any_of` 슬롯), 분기를 join으로 다시 라우팅한다. |
| <a id="p_core_s2"></a>`P_CORE_S2` | 올바른 완료(proper completion). join에 신호가 남은 채로 실행이 끝난다. `kind`는 `stalled`(실행이 `P_CORE_BARRIER_STALLED`로 실패) 또는 `completed`(명시적 종료가 신호를 버리고 실행이 완료로 보고됨)다. witness에 `received`와 `missing`이 있다. 정체가 슬롯 하나를 채우지 못했고 슬롯 밖의 노드 하나가 신호를 보냈다면, `suggested_wait_for`가 그 노드로 슬롯을 채우게 하며 반환된 제안이 이를 적용한다. |
| <a id="p_core_s3"></a>`P_CORE_S3` | 어떤 실행에서도 join이 발동하지 않는다. `kind`는 `dead`(신호를 보내는 노드가 없음) 또는 `unfireable`(신호는 도착하지만 모든 슬롯이 한꺼번에 채워지지는 않음)이며, `slots_never_filled`에는 어떤 실행도 신호를 보내지 않는 슬롯이 나열된다. 모든 실행을 탐색한 경우에만 보고된다. |
| <a id="p_core_s4"></a>`P_CORE_S4` | 실행이 빠져나오지 못하는 루프에 들어갈 수 있어서 스텝 한도에서만 멈춘다. `nodes`는 루프를 나열하고, `trace`는 루프까지 가는 경로이며, `cycle`은 루프를 한 바퀴 도는 경로다. 루프 안의 조건부 노드에 루프 밖으로 나가는 라우트를 준다. 반환된 제안은 placeholder다. |
| <a id="p_core_s5"></a>`P_CORE_S5` | 같은 overwrite 채널에 쓰는 두 노드가 같은 슈퍼스텝에서 실행될 수 있다. `P_CORE_E5`가 보고하는 fan-out 형제 노드 외의 경우이며, 마지막으로 쓴 값이 남는데 어느 노드일지 비결정적이다. append 리듀서를 쓰거나(반환된 제안) 쓰는 노드들을 직렬화한다. 모든 노드 타입에 효과 계약이 있을 때만 보고된다. |
| <a id="p_core_soundness"></a>`P_CORE_SOUNDNESS` | 요약이다. 모든 실행을 탐색했으면 `status`가 `checked`(메모)이고, 그렇지 않으면 `partial` 또는 `not_checked`(경고)다. witness에 `states`, `transitions`, `state_cap`, 탐색한 실행이 도달하는 `outcomes`, 코드별 `findings`가 있다. `partial`은 타입이 `Command.goto`나 `Send`를 선언하는 노드를 `not_analyzable`에 나열하며 각 노드까지 가는 트레이스를 함께 보여 준다. 그 노드 이후의 실행은 탐색하지 않았다. `not_checked`는 `soundness_state_cap`에 도달했다는 뜻이다. |

<a id="registry"></a>
## 레지스트리

| 코드 | 의미와 해결 방법 |
|---|---|
| <a id="p_registry_missing"></a>`P_REGISTRY_MISSING` | 참조한 실행 대상이 레지스트리 스냅샷에 없다. 등록하거나 candidate를 쓴다. |
| <a id="p_registry_dependency_missing"></a>`P_REGISTRY_DEPENDENCY_MISSING` | 실행 대상이 필요로 하는 의존성이 레지스트리 스냅샷에 없다. |
| <a id="p_registry_identity_mismatch"></a>`P_REGISTRY_IDENTITY_MISMATCH` | 한 실행 대상에 대한 두 참조가 서로 다른 정확한 identity로 해석된다. |
| <a id="p_registry_requirement_kind"></a>`P_REGISTRY_REQUIREMENT_KIND` | 노드의 요구 사항 resolver가 지원되지 않는 실행 대상 종류를 반환했다. |
| <a id="p_registry_requirement_resolver"></a>`P_REGISTRY_REQUIREMENT_RESOLVER` | 노드의 요구 사항 resolver가 예외를 던졌다. |

<a id="runtime-and-admission"></a>
## 런타임과 승인(admission)

승인(admission) 실패와 실행 실패는 Python에서 `ng.ProgramAdmissionError`와 `ng.ProgramDiagnosticError`로 전달되고, 실행 결과에는 코드가 달린 `failure`가 들어 있습니다. 코드에 자기 항목이 따로 없으면 `help.doc`이 이 섹션으로 연결됩니다.

Core 노드의 실패는 진단이 아닙니다. #364부터 `failure.code`는 노드 실패 분류기의 코드(`NG.TaskFailed`, `app.compile_error` 같은 작성자 코드, `NG.ProviderFailed` 등)이고 `failure.witness.node_failure`에 레코드가 담기며, 실패 종료 노드는 선언된 코드로 실행을 끝냅니다([Program 그래프 기능](program-graphs.md#failure-routing) 참고). `P_RUNTIME_CORE_FAILURE`는 노드 실패 레코드가 없는 Core 실패에 계속 쓰입니다.

| 코드 | 의미와 해결 방법 |
|---|---|
| <a id="p_core_barrier_stalled"></a>`P_CORE_BARRIER_STALLED` | Core 실행이 멈췄는데, join이 더는 도착할 수 없는 신호를 계속 기다리고 있다. witness에는 정체된 join마다 `received`와 `missing`이 나열되고, 실행된 누락 멤버에서 시작하는 `paths`가 있으며, 트레이스에 대체 노드가 보이면 `suggested_wait_for`도 있다. |
