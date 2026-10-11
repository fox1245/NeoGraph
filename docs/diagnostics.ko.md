<!-- neograph-i18n: source=docs/diagnostics.md locale=ko source_sha256=c4b7c36f1ce541c78785e7cd73889179b47e0c2fac4207ecf4eff50932331aa6 -->
# Program 진단

**Languages:** [English](diagnostics.md) | [한국어](diagnostics.ko.md) | [日本語](diagnostics.ja.md) | [简体中文](diagnostics.zh-CN.md)

Program 컴파일은 진단을 반환합니다. 실패하면 `ProgramCompileError::diagnostics()`, 성공하면 `ProgramBundle::compile_diagnostics()`, Python 예외에서는 `.diagnostics`로 받습니다. 이 페이지는 코드마다 의미와 고치는 방법을 설명합니다. 반환된 진단은 모두 `help.doc`으로 이 페이지의 해당 항목을 가리킵니다.

- [help 객체](#the-help-object)
- [Program 문서](#program-document)
- [JavaScript 프런트엔드](#javascript-frontend)
- [Core 파서](#core-parser)
- [Core 검증기](#core-validator)
- [레지스트리](#registry)
- [런타임과 승인](#runtime-and-admission)

<a id="the-help-object"></a>
## help 객체

반환된 진단에는 `help`가 있어서, 도구와 모델이 메시지를 해석하지 않고도 바로 조치할 수 있습니다.

| 필드 | 내용 |
|---|---|
| `doc` | 이 페이지에서 해당 코드 항목으로 가는 링크, 또는 코드 계열 섹션으로 가는 링크. |
| `related` | 같은 근본 원인이 나타나는 다른 위치. 각각 한 줄짜리 `label`이 붙습니다. |
| `suggestions` | 수정안. 각각 `message`, `edits`, `applicability`를 가집니다. |
| `candidates` | 알 수 없는 이름에서 편집 두 번 이내인 선언된 이름. 가까운 순서입니다. |

`edits`는 컴파일러가 파싱한 Program 문서에 적용하는 RFC 6902 JSON Patch 연산(`add`, `replace`, `remove`, `move`)입니다. JavaScript 소스라면 `define()`이 생성한 문서이며, 빌더 호출 위치는 진단의 span이 알려 줍니다. `applicability`는 `machine`(그대로 적용), `maybe`(먼저 검토, 기본값), `placeholder`(`<node>` 같은 값을 채워야 함) 중 하나입니다.

번들에 봉인되는 진단에는 `help`가 절대 들어가지 않으므로 번들 id는 `help`와 무관합니다. 컴파일은 근본 원인 하나에 진단 하나를 반환합니다. 여러 진단이 같은 알 수 없는 이름을 보고하면 첫 번째만 남고 나머지는 그 진단의 `related` 항목이 됩니다. 반환되는 진단은 오류를 먼저 두고 최대 20개이며, 나머지 개수는 `P_DIAGNOSTICS_TRUNCATED` 노트가 알려 줍니다.

메시지는 작성한 형태 그대로 이름을 붙입니다. JavaScript 소스라면 `graph.barrier("D", [...])` 같은 빌더 호출로, JSON 또는 C++ 빌더 문서라면 Core 키로 표시합니다.

<a id="program-document"></a>
## Program 문서

| 코드 | 의미와 해결 방법 |
|---|---|
| <a id="p_schema_version"></a>`P_SCHEMA_VERSION` | Program, 계약, 내장 Core 정의의 스키마 버전이 없거나 지원되지 않습니다. witness에 나열된 버전을 사용하세요. |
| <a id="p_schema_unknown_field"></a>`P_SCHEMA_UNKNOWN_FIELD` | 닫힌 Program 객체에 정의되지 않은 필드가 있습니다. 제거하거나 후보 이름을 사용하세요. |
| <a id="p_schema_required"></a>`P_SCHEMA_REQUIRED` | 필수 Program 필드가 없습니다. 추가하세요. |
| <a id="p_schema_type"></a>`P_SCHEMA_TYPE` | Program 필드의 JSON 타입이 틀렸거나 소스 맵에 같은 포인터가 반복됩니다. witness의 타입을 사용하세요. |
| <a id="p_schema_operation"></a>`P_SCHEMA_OPERATION` | 이 연산에는 더 새로운 Program 스키마 버전이 필요합니다. `program_schema_version`을 올리거나 다른 연산을 사용하세요. |
| <a id="p_contract_schema"></a>`P_CONTRACT_SCHEMA` | 입력 또는 출력 계약이 잘못되었거나 지원되지 않는 JSON Schema 키워드를 사용합니다. |
| <a id="p_root_name"></a>`P_ROOT_NAME` | 루트 이름이 없거나 비어 있거나 Core 정의의 `name`과 다릅니다. 둘을 같게 맞추세요. |
| <a id="p_root_operation"></a>`P_ROOT_OPERATION` | `call_core` 루트에 Core 노드가 없습니다. 노드를 하나 이상 선언하세요. |
| <a id="p_plan_operation"></a>`P_PLAN_OPERATION` | 지원되지 않는 연산입니다. |
| <a id="p_plan_core"></a>`P_PLAN_CORE` | 계획이 봉인된 루트가 아닌 Core를 호출하거나, Core를 호출하지 않으면서 자식도 디스패치하지 않습니다. |
| <a id="p_plan_bound"></a>`P_PLAN_BOUND` | loop, retry, timeout 한계가 범위를 벗어났습니다. |
| <a id="p_plan_condition"></a>`P_PLAN_CONDITION` | 계획 조건의 술어가 정확히 하나가 아니거나 경로가 JSON Pointer가 아닙니다. |
| <a id="p_plan_race_arity"></a>`P_PLAN_RACE_ARITY` | race의 분기가 두 개가 아닙니다. |
| <a id="p_plan_quorum"></a>`P_PLAN_QUORUM` | `min_success`가 0이거나 분기 수보다 큽니다. |
| <a id="p_plan_spawn_shape"></a>`P_PLAN_SPAWN_SHAPE` | spawn에 인라인 본문이 있거나 await의 직접 본문이 아닙니다. |
| <a id="p_plan_cancel_scope"></a>`P_PLAN_CANCEL_SCOPE` | cancel은 `run` 범위만 지원합니다. |
| <a id="p_plan_reference"></a>`P_PLAN_REFERENCE` | 봉인된 연산이 존재하지 않는 연산을 참조합니다. 컴파일러 결함이므로 보고해 주세요. |
| <a id="p_plan_source_pointer"></a>`P_PLAN_SOURCE_POINTER` | 봉인된 연산의 소스 포인터가 잘못되었습니다. 컴파일러 결함이므로 보고해 주세요. |
| <a id="p_plan_typed"></a>`P_PLAN_TYPED` | 계획을 타입이 있는 연산으로 봉인하지 못했습니다. 자세한 내용은 witness에 있습니다. |
| <a id="p_parallel_map_source"></a>`P_PARALLEL_MAP_SOURCE` | `item_source`에는 한계가 있는 리터럴 배열이나 입력 아티팩트 필드가 필요합니다. |
| <a id="p_parallel_map_binding"></a>`P_PARALLEL_MAP_BINDING` | `parallel_map` 바인딩이나 엔드포인트가 없거나 JSON Pointer가 아닙니다. |
| <a id="p_parallel_map_bound"></a>`P_PARALLEL_MAP_BOUND` | `parallel_map` 한계가 범위를 벗어났거나 `max_in_flight`가 `max_items`보다 큽니다. |
| <a id="p_parallel_map_failure"></a>`P_PARALLEL_MAP_FAILURE` | `failure_policy`는 `fail_fast` 또는 `collect`여야 합니다. |
| <a id="p_expand_source"></a>`P_EXPAND_SOURCE` | `proposal_source`에는 인라인 객체나 입력 JSON Pointer가 필요합니다. |
| <a id="p_expand_bound"></a>`P_EXPAND_BOUND` | `expand_task_graph` 한계가 범위를 벗어났습니다. |
| <a id="p_expand_failure"></a>`P_EXPAND_FAILURE` | `failure_policy`는 `fail_fast` 또는 `collect`여야 합니다. |
| <a id="p_budget_invalid"></a>`P_BUDGET_INVALID` | 예산 레코드가 빠졌거나, 반복되거나, 최소와 최대가 뒤집혔거나, 구조적 하한보다 작습니다. 아홉 가지 자원을 한 번씩 선언하세요. |
| <a id="p_budget_v1"></a>`P_BUDGET_V1` | Program 스키마 1에서는 `max_program_operations`가 정확히 1로 고정됩니다. |
| <a id="p_budget_static"></a>`P_BUDGET_STATIC` | 선언한 최대값으로는 계획이 항상 실행하는 연산을 감당할 수 없습니다. 값을 올리세요. |
| <a id="p_migration_program_json"></a>`P_MIGRATION_PROGRAM_JSON` | Program JSON 연산 트리는 더 이상 받지 않습니다. JavaScript나 신뢰된 C++ 빌더로 작성하세요. |
| <a id="p_compiler_internal"></a>`P_COMPILER_INTERNAL` | 컴파일러가 내부 오류로 안전하게 실패했습니다. 소스와 함께 보고해 주세요. |
| <a id="p_diagnostics_truncated"></a>`P_DIAGNOSTICS_TRUNCATED` | 진단이 20개보다 많아서 나머지는 `witness.omitted`에 개수만 남았습니다. 표시된 진단을 고친 뒤 다시 컴파일하세요. |

<a id="javascript-frontend"></a>
## JavaScript 프런트엔드

| 코드 | 의미와 해결 방법 |
|---|---|
| <a id="p_js_evaluation"></a>`P_JS_EVALUATION` | 소스가 예외를 던졌거나 파싱되지 않았습니다. span이 실패한 줄을 가리킵니다. |
| <a id="p_js_timeout"></a>`P_JS_TIMEOUT` | 평가가 컴파일 시간 한계나 예산의 `wall_time_ms`를 넘었습니다. |
| <a id="p_js_resource_limit"></a>`P_JS_RESOURCE_LIMIT` | 평가가 메모리 또는 스택 한계를 넘었습니다. |
| <a id="p_js_cancelled"></a>`P_JS_CANCELLED` | 평가가 취소되었습니다. |
| <a id="p_js_runtime"></a>`P_JS_RUNTIME` | QuickJS 런타임이나 빌더 호출이 내부적으로 실패했습니다. |
| <a id="p_js_runtime_identity"></a>`P_JS_RUNTIME_IDENTITY` | 소스가 컴파일러와 다른 QuickJS 빌드나 프로필용으로 만들어졌습니다. |
| <a id="p_js_unavailable"></a>`P_JS_UNAVAILABLE` | 이 빌드에는 QuickJS 프런트엔드가 없습니다. |
| <a id="p_js_ambient_denied"></a>`P_JS_AMBIENT_DENIED` | 소스가 프로필이 금지하는 주변 권한(시계, 난수, eval 등)을 사용했습니다. |
| <a id="p_js_define_missing"></a>`P_JS_DEFINE_MISSING` | 모듈이 `define()`을 export하지 않습니다. |
| <a id="p_js_define_async"></a>`P_JS_DEFINE_ASYNC` | `define()`은 동기 함수여야 합니다. |
| <a id="p_js_define_value"></a>`P_JS_DEFINE_VALUE` | `define()`은 열린 그래프 빌더 정확히 하나를 반환해야 합니다. |
| <a id="p_js_graph_argument"></a>`P_JS_GRAPH_ARGUMENT` | 빌더 메서드가 잘못된 형태의 인수, 예약되었거나 중복된 이름, 선언되지 않은 노드를 받았습니다. 메시지에 메서드 이름이 나옵니다. |
| <a id="p_js_graph_value"></a>`P_JS_GRAPH_VALUE` | 빌더 인수가 정규 JSON 데이터가 아닙니다(함수, 클래스 인스턴스, 유한하지 않은 수). |
| <a id="p_js_graph_limit"></a>`P_JS_GRAPH_LIMIT` | 생성된 그래프가 바이트 한계를 넘었습니다. |
| <a id="p_js_graph_owner"></a>`P_JS_GRAPH_OWNER` | 이 정의의 빌더가 아닌 객체에서 빌더 메서드를 호출했습니다. |
| <a id="p_js_graph_sealed"></a>`P_JS_GRAPH_SEALED` | `define()`이 반환된 뒤에 빌더 메서드를 호출했습니다. |
| <a id="p_js_graph_entry"></a>`P_JS_GRAPH_ENTRY` | 그래프에 노드는 있지만 `.entry(node)`가 없어서 아무것도 실행되지 않습니다. |
| <a id="p_js_control_character"></a>`P_JS_CONTROL_CHARACTER` | 설정 문자열에 제어 문자가 있습니다. 정규식을 일반 문자열로 쓴 경우가 많습니다. `/.../.source`나 ``String.raw`...` ``를 사용하세요. |
| <a id="p_js_control_main"></a>`P_JS_CONTROL_MAIN` | export된 `main`이 함수가 아닙니다. |
| <a id="p_js_control_async"></a>`P_JS_CONTROL_ASYNC` | `main`은 동기 제너레이터여야 합니다. |
| <a id="p_js_control_value"></a>`P_JS_CONTROL_VALUE` | `main`이 봉인된 명령이나 정규 JSON이 아닌 값을 yield하거나 반환했습니다. |
| <a id="p_js_control_command"></a>`P_JS_CONTROL_COMMAND` | 잘못된 인수로 명령을 만들었습니다. |
| <a id="p_js_control_finished"></a>`P_JS_CONTROL_FINISHED` | 이미 끝난 제어 제너레이터를 다시 재개했습니다. |
| <a id="p_js_import_dynamic"></a>`P_JS_IMPORT_DYNAMIC` | 동적 `import()`와 `require()`는 허용되지 않습니다. |
| <a id="p_js_import_invalid"></a>`P_JS_IMPORT_INVALID` | import 지정자 형식이 잘못되었습니다. |
| <a id="p_js_import_unavailable"></a>`P_JS_IMPORT_UNAVAILABLE` | import가 봉인된 모듈 클로저에 속하지 않습니다. |
| <a id="p_import_conflict"></a>`P_IMPORT_CONFLICT` | import 소스 하나가 서로 다른 콘텐츠 아이덴티티 두 개로 나타납니다. |
| <a id="p_import_dynamic"></a>`P_IMPORT_DYNAMIC` | 소스나 봉인된 모듈이 모듈을 동적으로 로드합니다. |
| <a id="p_import_unresolved"></a>`P_IMPORT_UNRESOLVED` | import가 검증된 정확한 모듈 영수증에 묶여 있지 않습니다. 모듈 해석 결과와 함께 컴파일하세요. |

<a id="core-parser"></a>
## Core 파서

Core 정의 파서가 내는 진단입니다. witness의 `core_code`에 Core 코드가 들어 있습니다.

| 코드 | 의미와 해결 방법 |
|---|---|
| <a id="p_core_gc_schema_version"></a>`P_CORE_GC_SCHEMA_VERSION` | Core `schema_version`이 지원되는 음이 아닌 정수가 아닙니다. |
| <a id="p_core_gc_unknown_field"></a>`P_CORE_GC_UNKNOWN_FIELD` | 엄격한 Core 객체에 엔진이 읽지 않는 키가 있습니다. 제거하거나 후보 이름을 쓰거나, 메모라면 `_` 또는 `x-` 주석 키로 옮기세요. |
| <a id="p_core_gc_field_type"></a>`P_CORE_GC_FIELD_TYPE` | Core 필드의 JSON 타입이 틀렸습니다. |
| <a id="p_core_gc_field_value"></a>`P_CORE_GC_FIELD_VALUE` | Core 필드 값이 잘못되었습니다. 빈 이름, 범위를 벗어난 수, 알 수 없는 옵션 등입니다. |
| <a id="p_core_gc_field_required"></a>`P_CORE_GC_FIELD_REQUIRED` | `type`, `from`, `to` 같은 필수 Core 필드가 없습니다. |
| <a id="p_core_gc_edge"></a>`P_CORE_GC_EDGE` | edge나 conditional edge가 객체가 아니거나 routes가 객체가 아닙니다. |
| <a id="p_core_gc_barrier"></a>`P_CORE_GC_BARRIER` | barrier의 `wait_for`가 없거나 비었거나 형식이 잘못되었습니다. |
| <a id="p_core_gc_node_config"></a>`P_CORE_GC_NODE_CONFIG` | 노드 설정이 타입 스키마의 `required`, `type`, `enum` 규칙과 맞지 않습니다. |
| <a id="p_core_gc_reserved_node"></a>`P_CORE_GC_RESERVED_NODE` | `__start__`와 `__end__`는 예약어라서 노드 이름으로 쓸 수 없습니다. |
| <a id="p_core_gc_no_entry"></a>`P_CORE_GC_NO_ENTRY` | `__start__`에서 나가는 edge가 없어서 아무것도 실행되지 않습니다. `.entry(node)`를 추가하세요. |
| <a id="p_core_gc_registry_miss"></a>`P_CORE_GC_REGISTRY_MISS` | 노드 타입, reducer, condition이 레지스트리 스냅숏에 없습니다. 등록하거나 후보 이름을 사용하세요. |
| <a id="p_core_gc_metadata_limit"></a>`P_CORE_GC_METADATA_LIMIT` | 그래프 메타데이터가 한계를 넘었습니다. 정규 JSON 8 KiB, 중첩 깊이 4, 객체 멤버 합계 64개, 키 128바이트가 한계입니다. witness에 `kind`, `limit`, `actual`이 있습니다. |
| <a id="p_core_gc_metadata_text"></a>`P_CORE_GC_METADATA_TEXT` | 메타데이터 키나 문자열에 보이지 않는 문자(탭, 줄바꿈, 캐리지 리턴 이외의 C0 제어 문자, bidi 제어 문자, 폭이 없는 문자, 태그 문자)나 잘못된 UTF-8이 있습니다. witness에 코드 포인트가 나열됩니다. |
| <a id="p_core_roundtrip"></a>`P_CORE_ROUNDTRIP` | 파싱된 그래프가 정의를 그대로 다시 내보내지 못합니다. 무언가가 빠졌거나 다르게 연결되었습니다. 컴파일러 결함이므로 보고해 주세요. |

<a id="core-validator"></a>
## Core 검증기

그래프의 의미를 분석한 결과입니다. 경고는 반환만 되며, 번들은 처음 봉인될 때의 규칙에서 나온 경고를 봉인합니다.

| 코드 | 의미와 해결 방법 |
|---|---|
| <a id="p_core_e3"></a>`P_CORE_E3` | edge, route, interrupt, barrier가 알 수 없는 노드를 가리키거나 barrier가 자기 자신을 기다립니다. 후보 이름을 사용하세요. |
| <a id="p_core_e4"></a>`P_CORE_E4` | 노드의 effect 계약이 선언되지 않은 채널에 쓰거나(오류) 읽습니다(경고). 채널을 선언하세요. |
| <a id="p_core_e5"></a>`P_CORE_E5` | fan-out 형제 노드들이 같은 overwrite 채널에 씁니다. 마지막 기록이 비결정적으로 이깁니다. append reducer를 사용하세요. |
| <a id="p_core_e6"></a>`P_CORE_E6` | 채널을 읽지도 쓰지도 않거나, 쓰기만 하고 읽지 않거나, 읽기만 하는데 초기값이 없습니다. |
| <a id="p_core_e7"></a>`P_CORE_E7` | edge와 route로 `__start__`에서 도달할 수 없는 노드가 있습니다. |
| <a id="p_core_e8"></a>`P_CORE_E8` | barrier가 자신에게 라우팅하지 않는 노드를, 또는 어떤 대안도 자신에게 라우팅하지 않는 `any_of` 슬롯을 기다리므로 join이 절대 발화하지 않습니다. |
| <a id="p_core_e9"></a>`P_CORE_E9` | 노드에 일반 in-edge가 여러 개인데 barrier가 없습니다. 도착하는 super-step마다 한 번씩 실행됩니다. AND join이라면 barrier를 추가하고, XOR 병합이라면 무시하세요. |
| <a id="p_core_e10"></a>`P_CORE_E10` | conditional edge에 route가 없거나, condition이 반환하지 않는 라벨을 라우팅하거나, 라벨을 라우팅하지 않은 채 남겨 둡니다. |
| <a id="p_core_e11"></a>`P_CORE_E11` | 노드들이 `__end__`로 가는 경로가 없는 사이클에 갇혀 있습니다. |
| <a id="p_core_e12"></a>`P_CORE_E12` | 로컬 레지스트리에 reducer, 노드 타입, condition이 없습니다. |
| <a id="p_core_e13"></a>`P_CORE_E13` | conditional edge로 라우팅하는 노드에서 정적 edge가 나갑니다. 이 edge는 절대 발화하지 않습니다. 제거하거나 route로 추가하세요. |
| <a id="p_core_e14"></a>`P_CORE_E14` | 노드에 두 번째 conditional edge가 있습니다. 첫 번째만 라우팅합니다. 하나로 합치세요. |
| <a id="p_core_e15"></a>`P_CORE_E15` | barrier 멤버가 자신의 join이 아닌 곳으로 라우팅할 수 있습니다. 그렇게 되면 실행은 barrier-stalled로 끝납니다. 같은 `any_of` 슬롯의 다른 대안으로 가는 라우트는 우회가 아닙니다. |
| <a id="p_core_e16"></a>`P_CORE_E16` | 그래프에 사이클이 있지만 이번 컴파일은 비순환 그래프를 요구합니다. |
| <a id="p_core_e17"></a>`P_CORE_E17` | 한 `any_of` 슬롯의 두 대안이 모두 실행될 수 있습니다. fan-out이 join을 거치지 않고 둘 다에 닿거나, 한 대안이 join과 다른 대안으로 함께 라우팅합니다. 두 번째 도착은 끝나지 않는 새 라운드를 열고 실행은 barrier-stalled로 끝납니다. 예를 들어 conditional edge를 써서 대안들이 서로 배타적으로 라우팅되게 하세요. witness에 fan-out 노드와 두 경로가 있습니다. |

<a id="registry"></a>
## 레지스트리

| 코드 | 의미와 해결 방법 |
|---|---|
| <a id="p_registry_missing"></a>`P_REGISTRY_MISSING` | 참조한 실행 단위가 레지스트리 스냅숏에 없습니다. 등록하거나 후보 이름을 사용하세요. |
| <a id="p_registry_dependency_missing"></a>`P_REGISTRY_DEPENDENCY_MISSING` | 실행 단위가 레지스트리 스냅숏에 없는 의존성을 요구합니다. |
| <a id="p_registry_identity_mismatch"></a>`P_REGISTRY_IDENTITY_MISMATCH` | 두 참조가 한 실행 단위의 서로 다른 정확한 아이덴티티로 해석됩니다. |
| <a id="p_registry_requirement_kind"></a>`P_REGISTRY_REQUIREMENT_KIND` | 노드의 요구 사항 해석기가 지원되지 않는 실행 단위 종류를 반환했습니다. |
| <a id="p_registry_requirement_resolver"></a>`P_REGISTRY_REQUIREMENT_RESOLVER` | 노드의 요구 사항 해석기가 예외를 던졌습니다. |

<a id="runtime-and-admission"></a>
## 런타임과 승인

승인 실패와 실행 실패는 Python에 `ng.ProgramAdmissionError`와 `ng.ProgramDiagnosticError`로 전달되며, 실행 결과에는 코드가 있는 `failure`가 들어 있습니다. 코드에 전용 항목이 없으면 `help.doc`은 이 섹션을 가리킵니다.

| 코드 | 의미와 해결 방법 |
|---|---|
| <a id="p_core_barrier_stalled"></a>`P_CORE_BARRIER_STALLED` | 더 이상 도착할 수 없는 신호를 join이 기다리는 상태로 Core 실행이 멈췄습니다. witness는 멈춘 join마다 `received`와 `missing`을 나열합니다. |
