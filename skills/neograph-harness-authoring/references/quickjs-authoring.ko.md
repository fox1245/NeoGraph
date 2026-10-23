<!-- neograph-i18n: source=skills/neograph-harness-authoring/references/quickjs-authoring.md locale=ko source_sha256=9c608357cfda36a56045f97d4b5bbd5398e96e016e90c4b6690f4d2d2e3949fe -->
# QuickJS 소스 작성

**Languages:** [English](quickjs-authoring.md) | [한국어](quickjs-authoring.ko.md) | [日本語](quickjs-authoring.ja.md) | [简体中文](quickjs-authoring.zh-CN.md)

## 이름은 호스트에서 가져온다

요청과 제공된 매니페스트가 쓸 수 있는 모든 이름을 줍니다. 노드 타입, 리듀서, 조건,
그래프(Core) 이름, 자식 바인딩, 임포트 슬롯, 입력 형태, `main`이 반환해야 하는 값이
그것입니다. 매니페스트는 구문(시그니처, 옵션 키, 한도)만 나열하며 애플리케이션
노드는 담고 있지 않습니다. 필요한 이름이 없으면 어떤 이름이 없는지 말하십시오.
그럴듯하기만 한 이름은 틀린 이름입니다.

그래프 이름도 그중 하나입니다. 호스트가 이름을 주면 그 문자열 그대로 `ng.graph(...)`와
모든 `ng.callCore(...)`에 넘기십시오. 작업 내용에서 이름을 지어내지 마십시오.

## 모듈

전역 `ng`를 쓰는 ES 모듈입니다. `define`을 내보내고, 작업에 런타임 제어가 필요하면
제너레이터 `main`도 내보냅니다. `require`, `module.exports`, `export default`,
`import`는 없습니다.

    // GRAPH, REDUCER, NODE_TYPE은 호스트가 준 이름을 대신하는 자리표시자입니다.
    export function define() {
      const g = ng.graph("GRAPH");
      g.channel("value", {reducer: "REDUCER", initial: 0});
      g.node("step", {type: "NODE_TYPE"});
      g.entry("step");
      g.exit("step");
      return g;
    }

    export function* main(input) {
      const result = yield ng.callCore("GRAPH", {value: input.value}, "run");
      return {value: result.channels.value.value};  // 그래프 실행의 출력
    }

`async`, Promise, 타이머, `eval`, `Date`, I/O는 쓰지 마십시오. `Date`와 `eval`은 예외를
던지고, `setTimeout`은 존재하지 않으며, `Math.random()`은 항상 0을 반환합니다. 반환하는
값과 모든 명령 입력은 순수 JSON이어야 합니다. `undefined`, 함수, `Date`, `Map`은
안 됩니다.

## define(): 그래프

`define()`은 동기 함수이며 `ng.graph(name)`이 준 빌더를 반환합니다. `ng.callCore` 같은
명령은 여기에 없습니다.

- 빌더 메서드는 모두 같은 빌더를 반환하며 노드 핸들은 반환하지 않습니다. 매니페스트의
  시그니처와 옵션 키를 그대로 쓰십시오. 키는 snake_case이고(`initial_delay_ms`이지
  `initialDelay`가 아닙니다) 알 수 없는 키는 거부됩니다.
- 그래프에는 `entry`와 `exit`가 필요합니다. 모든 엣지, 배리어, 인터럽트, 재시도는
  선언된 노드를 가리켜야 합니다. `__start__`와 `__end__`는 노드 이름이 될 수 없습니다.
- `node(name, {type, ...config})`: `type`은 필수이며 등록되어 있어야 합니다.
  `channel(name, {reducer, initial})`: 등록된 리듀서를 지정하십시오.
- `conditionalEdge(from, condition, routes)`: `routes`는 등록된 조건이 반환할 수 있는
  모든 레이블을, 그리고 그 레이블만을 노드에 대응시킵니다.
- 팬아웃은 한 노드에서 나가는 여러 엣지입니다. 모든 분기를 기다려야 하는 조인에는
  `barrier(join, [branchNodes])`도 필요합니다. 한 채널에 동시에 쓰려면 그 쓰기를
  병합하는 리듀서가 필요합니다.

## main(): 런타임 명령

`main`은 동기 제너레이터입니다. 각 `yield`는 `ng` 생성자가 만든 명령을 정확히 하나
런타임에 넘기고 그 명령의 결과를 돌려받습니다. 원시 배열, 일반 객체, Promise,
`yield*`는 거부됩니다. 분기, 반복, `map` 같은 평범한 JavaScript로 명령과 명령 배열을
만들고, 여러 개를 한꺼번에 실행하려면 `ng.all(commands, {max_in_flight: N})`을 한 번
`yield`하십시오.

| 생성자 | 의미 |
|---|---|
| `ng.callCore(name, input, site)` | 그래프를 실행합니다. `name`은 `ng.graph`에 준 문자열이고 `input`은 채널 이름을 값에 대응시킵니다. 그래프 실행의 출력은 `result.channels.NAME.value`로 읽습니다. |
| `ng.all(commands, options, site)` | 명령들을 한 번에 `max_in_flight`개씩 함께 실행하고 결과를 순서대로 반환합니다. `ng.parallel`도 같습니다. |
| `ng.race(commands, options, site)` | 가장 먼저 나온 결과가 이깁니다. 명령이 두 개 이상 필요합니다. |
| `ng.quorum(commands, k, site)` | `k`개가 성공하면 끝납니다. `1 <= k <= commands.length`. |
| `ng.join(commands, mode, options, site)` | 일반 형태입니다. `mode`는 `"all"`, `"race"`, `"quorum"`(`required_successes`와 함께)입니다. `failure_policy`는 `"fail_fast"` 또는 `"collect"`입니다. |
| `ng.spawn(binding, input, site)` | 호스트가 승인한 자식을 시작합니다. 결과를 받으려면 `ng.await(spawnCommand, timeoutMs, site)`로 감싸십시오. |
| `ng.emit(value, site)`, `ng.checkpoint(value, site)` | JSON 값을 실행 저널에 기록합니다. |
| `ng.cancelScope("current", reason, site)` | 실행을 취소 상태로 끝냅니다. |
| `ng.hostCapability(slot, input, site)` | 호스트가 승인한 임포트 슬롯(정수)을 호출합니다. |

- `site`는 선택 사항인 마지막 인자로, `"review:" + id` 같은 안정적인 레이블입니다. id나
  인덱스에서 만들고 시간이나 난수에서는 만들지 마십시오. 복구된 실행은 `main`을 다시
  실행하므로 같은 결과에 대해 같은 명령을 내야 합니다.
- 옵션 키는 snake_case입니다. `max_in_flight`, `required_successes`, await 타임아웃은
  양의 정수입니다.
- 명령이 하나라도 실패하면(`ng.all`의 멤버 하나여도) 실행이 끝나고 `main`은 다시 재개되지
  않으므로, `yield` 둘레의 `try`/`catch`로는 복구할 수 없습니다. 논리적 재시도는 결과의
  필드(예: `done` 플래그)를 보고 반복하십시오. 실패하는 노드의 재시도는 `define()`의
  `retryPolicy`가 맡습니다.
- 결과는 작업이 말하는 형태로 읽으십시오. 응답이 `status` 같은 필드를 가진다고 하면
  그 필드는 `result.status`입니다. `channels` 래퍼는 그래프 실행의 출력에만 해당하므로,
  작업이 다르게 설명한 응답에 붙이지 마십시오.
- `define()`만 있는 모듈은 Core 결과를 반환합니다. `main`이 있으면 반환값은 호스트가
  승인한 출력 계약과 일치해야 합니다.

## 응답과 수정

소스 평가에서는 문자열 필드 `source` 하나만 가진 JSON 객체 하나로 답하고, 그 필드에
모듈 전체를 담습니다. JSON으로 이스케이프하고(`\n`, `\"`) 중괄호 앞뒤에는 아무것도
두지 마십시오. Markdown 펜스, 설명 문장, 패치, Program JSON은 안 됩니다.

    {"source": "export function define() {\n  ...\n}\n"}

Harness MCP에서는 먼저 `neograph_schema`를 호출한 뒤, 모듈을 `harness.mode:
"javascript"`와 `source_id`, `source`, 그리고 필요한 모든 작업, 워커, 예산, 정책
필드와 함께 보냅니다. 소스만으로는 요청이 되지 않습니다. `neograph_compile`은 그
도구가 있을 때만 쓰십시오.

거부되면 `code`, `message`, 컴파일된 정의 안을 가리키는 JSON `pointer`, `witness`가 있는
진단이 돌아옵니다. 진단이 가리키는 원인을 고치고, 작업이 정한 것은 그대로 두며,
같은 봉투에 전체 모듈을 반환하십시오. 권한을 넓히거나 무관한 동작을 더하거나
호스트의 검사가 받아들이기 전에 성공했다고 주장하지 마십시오. 컴파일되는 소스도
잘못된 토폴로지를 만들 수 있습니다.
