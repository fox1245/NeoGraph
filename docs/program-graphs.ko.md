<!-- neograph-i18n: source=docs/program-graphs.md locale=ko source_sha256=8082d68c638414c879b2854352e5fd4c95bb82f0ac014d1efa11b852de5991e1 -->
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
