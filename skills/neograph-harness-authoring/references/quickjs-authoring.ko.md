<!-- neograph-i18n: source=skills/neograph-harness-authoring/references/quickjs-authoring.md locale=ko source_sha256=53dc89ee33f26b7d0f9c6d328453f73aaaf9e53f0b8b4cea360ca1cdc2b800c8 -->
# QuickJS 소스 작성

**Languages:** [English](quickjs-authoring.md) | [한국어](quickjs-authoring.ko.md) | [日本語](quickjs-authoring.ja.md) | [简体中文](quickjs-authoring.zh-CN.md)

## 호스트 계약 확인

코드를 쓰기 전에 아래 바인딩을 확인합니다. 제공된 설정을 쓰거나 사용 가능한 스키마를
살펴보고, 빠진 이름을 그럴듯하게 짐작해서 채워서는 안 됩니다.

| 계약 | 필요한 것 |
|---|---|
| JS API 매니페스트 | 이 빌드에서 쓰는 builder와 command의 정확한 시그니처 |
| 레지스트리 | 노드/리듀서/조건/import 이름, 노드 설정 스키마, effect |
| 호출 | 입력 JSON의 형태와 callCore에서 쓸 수 있는 Core 이름 |
| 결과 | Core 채널 이름과, 제너레이터가 마지막에 내놓아야 하는 출력 |
| 자식 | 승인(admission)된 바인딩 이름, 자식의 입력/출력, 부여된 한도 |
| 컴파일러 브리지 | 받아들이는 소스 봉투(envelope), 진단, 남은 수정 허용량 |

내장 JS 매니페스트가 나열하는 것은 문법이지 애플리케이션 노드가 아닙니다. 예를 들어
probe.node는 capability 테스트 팔레트에 속하며 애플리케이션의 LLM 노드가 아닙니다. 고정된
capability probe는 케이스별 그래프/command 계약을 확인할 뿐, 임의의 프로덕션 동작을
확인하지 않습니다.

## 컴파일 시점 그래프와 런타임 Program

**define()**은 컴파일 시점에 실행되는 동기 함수로, 열려 있는 builder 하나를 반환합니다. 노드와
엣지는 이 builder로 만듭니다. builder의 메서드는 노드 핸들이 아니라 builder 자신을 반환합니다.
노드의 동작은 호스트가 등록한 C++ 구현에서 오며, 일반 JS 콜백은 그래프 노드가 아닙니다.

**main(input)**은 나중에 실행되는 선택적 동기 제너레이터입니다. 순수 JS로 분기를 고르고, 배열과
반복문을 만들고, JSON을 변환할 수 있습니다. 런타임 effect는 제너레이터에서 yield하는 봉인된(sealed)
ng command로 수행합니다. define() 안에서 런타임 command를 호출하거나, 게시된 그래프를
수정하거나, 그래프 모양의 객체를 반환하거나, async 함수, promise, 타이머, require(), ambient
I/O, eval, 동적 import를 써서는 안 됩니다.

이 배선(plumbing) 예제는 호스트가 probe 팔레트를 등록해 두었고 숫자형 input.value가 필요하다고
가정합니다. 예제의 노드는 아무 일도 하지 않으므로, 데이터 흐름만 보여 줄 뿐 LLM 동작이나 이름이
붙은 capability 케이스의 답은 아닙니다.

~~~javascript
export function define() {
  const g = ng.graph("example");
  g.channel("value", {reducer: "probe.overwrite", initial: 0});
  g.node("step", {type: "probe.node"});
  g.entry("step");
  g.exit("step");
  return g;
}

export function* main(input) {
  const result = yield ng.callCore("example", {value: input.value}, "copy:value");
  return {value: result.channels.value.value};
}
~~~

callCore에는 호스트가 제공한 그래프 이름을 그대로 써야 합니다. "main", "example", "capability",
노드 이름 중 어느 것도 어디서나 통하는 별칭이 아닙니다.

## 데이터 흐름과 토폴로지

일반적인 callCore 입력은 채널 이름을 들어오는 값에 대응시킨 맵입니다. 선언된 각 채널은 등록된
리듀서를 적용합니다. 일반적인 Core 결과에는 직렬화된 채널이 들어 있으며, 읽어야 하는 것은 채널을
감싼 래퍼가 아니라 채널의 값입니다.

예를 들어 챗봇의 기존 템플릿은 answer 노드를 다음과 같이 호출합니다.

~~~javascript
const reply = yield ng.callCore(
  "main", {payload: {phase: "answer", task: task}}, "answer"
);
const answer = reply.channels.result.value;
~~~

여기서 payload/result는 선언된 채널이고, chat.step은 payload.phase를 읽습니다. 이 이름과 동작은
그 호스트의 레지스트리와 템플릿에서 온 것입니다. 이 조각을 다른 레지스트리에 복사한다고 해서
같은 바인딩이 생기지는 않습니다.

선언만 있는 모듈은 Core 결과의 형태를 그대로 유지합니다. main()이 있으면 제너레이터의 반환값이
따로 승인(admission)된 Program 출력 계약과 맞아야 합니다. capability 평가기는 가끔
{accepted: true} 같은 합성(synthetic) command 응답을 제공합니다. 이때는 합성 응답에 Core 채널
래퍼를 덧붙이지 말고, 해당 케이스에 명시된 응답 계약을 따릅니다.

| 목적 | 구성 방법과 주의할 점 |
|---|---|
| 선형 경로 | 노드, entry/exit, 그리고 이들을 잇는 모든 엣지를 추가한다 |
| 조건부 라우팅 | conditionalEdge(from, registeredCondition, routes)를 쓰고, 조건의 모든 레이블을 노드에 대응시킨다 |
| 정적 fan-out/fan-in | 나가는 분기 엣지와 들어오는 join 엣지를 모두 추가한다. join이 모든 분기를 기다려야 하면 barrier(joinNode, branchNames)를 추가한다 |
| 병렬 쓰기 | 동시 쓰기를 처리할 수 있는 호스트 리듀서/채널을 고른다. 엣지만으로는 병합 방식이 정해지지 않는다 |
| 그래프 인터럽트/재시도 | 매니페스트의 interruptBefore/After와 retryPolicy 키를 쓴다. JS 반복문이나 논리적 재시도와는 별개다 |

## 런타임 command 구성

- callCore(coreName, input, site)는 command를 만듭니다. 이를 yield하면 Core를 호출하고 그
  결과를 돌려받습니다.
- all(commands, {max_in_flight: N}, site)는 봉인된 command를 받습니다. 목록은 일반 JS로 만들고,
  all command는 한 번만 yield합니다. 배열 자체를 yield하거나, command를 Promise로 바꾸거나,
  봉인된 command에 yield*를 쓰면 안 됩니다.
- spawn(binding, input, site)은 호스트가 승인(admission)한 자식 바인딩을 고릅니다. 결과를
  기다리려면 command를 await(spawnCommand, timeoutMs, site)로 감쌉니다.
- checkpoint(state, site)는 명시적인 JSON 상태를 게시합니다. 그 자체로 컴파일, 승인(admission),
  교체를 하지는 않습니다.
- emit과 cancelScope는 매니페스트에 선언된 의미를 따릅니다. hostCapability는 승인(admission)된
  import 슬롯이 있어야 하며, 임의의 네이티브 API로 가는 통로가 아닙니다.

예를 들어 승인(admission)된 바인딩과 그 정확한 원래 입력이 있다면 다음과 같이 씁니다.

~~~javascript
const result = yield ng.await(
  ng.spawn(binding, originalChildInput, "child:spawn"),
  timeoutMs,
  "child:wait"
);
~~~

emit으로 내보낸 자식 ID는 그 자체로 봉인된 await command가 아닙니다. 호스트의 실제 join/복구
계약을 따릅니다. 교체 후에도 자식을 유지해야 하면 [runtime-handoffs.md](runtime-handoffs.md)를
참고하세요.

소스 사이트(source-site) 레이블은 영속 좌표의 일부가 됩니다. 안정적인 작업 식별자나 결정적인
인덱스에서 만들고, 시계나 난수 값은 쓰지 않습니다. 이미 완료된 연산은 재생(replay)할 때 원래
입력을 그대로 유지해야 합니다. JS 반복문, 재시도, 병렬 분기, 새 자식이 예산을 다시 채워 주지는
않습니다.

accepted=false처럼 성공한 결과를 보고 도는 반복문은 논리적 재시도입니다. 실패한 Core command는
Program의 결과이므로, 일반 JS try/catch로 이어서 실행할 수 있다고 가정하면 안 됩니다. 호스트의
실패/재개/조정 계약을 따릅니다.

## 출력, 컴파일, 수정

현재 인터페이스가 요구하는 봉투(envelope)를 씁니다.
- 소스 평가: source 문자열에 완전한 모듈이 들어 있는 JSON 객체 하나만 내놓습니다. Markdown
  펜스, 패치, ProgramBundle JSON, 설명하는 글은 넣지 않습니다.
- Harness MCP: 먼저 neograph_schema로 확인합니다. 모듈은 harness.mode="javascript", source_id,
  source에 담고, 필요한 task/worker/budget/policy 필드를 모두 함께 지정합니다. source만으로는
  완전한 MCP 요청이 아닙니다.
- 호스트 네이티브 제안: 그 호스트의 스키마를 따릅니다. 이 Skill은 범용 자유 형식 컴파일/교체
  RPC를 정의하지 않습니다.

제공된 컴파일러 브리지로 제출합니다. 평가 모드에서는 호스트가 반환된 소스를 제출하고 진단
결과를 돌려주며, 모델이 호출할 수 있는 도구가 있다는 뜻은 아닙니다. MCP 모드에서는
neograph_compile 도구를 쓸 수 있을 때만 사용합니다.

실패하면 보고된 code/path/소스 사이트를 찾아, 위반한 계약을 고칩니다. 예를 들면 다음과
같습니다.
- 알 수 없는 노드/리듀서/조건: 등록된 바인딩과 설정에 맞춥니다.
- 잘못된 Core 바인딩: command 전체에서 승인(admission)된 그래프 이름과 맞춥니다.
- command가 아닌 값을 yield: 봉인된 command 하나를 만들거나, 승인(admission)된 구조화 join을
  씁니다.
- 잘못된 출력/입력: 채널 매핑이나 Program 결과 계약을 바로잡습니다.
- 빠진 엣지/barrier: 소스의 표현만이 아니라, 낮춰진(lowered) 토폴로지를 고칩니다.

남은 수정 허용량 안에서 완전한 대체 소스를 반환합니다. 진단을 지우거나, grant를 넓히거나, 관련
없는 연산을 추가하거나, 컴파일러와 작업별 검사가 받아들이기 전에 성공했다고 주장해서는 안
됩니다.

체크아웃한 저장소의 capability 브리지는 다음과 같이 확인하고 사용할 수 있습니다.

~~~text
program_dsl_capability_probe --manifest
program_dsl_capability_probe graph_basics source.js
~~~

빌드 타깃은 program_dsl_capability_probe입니다. 이름이 붙은 케이스는 각자 제공된 계약이
필요합니다. 컴파일에 성공했다는 것만으로 케이스를 통과한 것은 아니며, probe가 성공해도
프로덕션 실행 권한이 생기지는 않습니다. 실행 스크립트 scripts/run_dsl_capability_eval.ts는
횟수가 제한된 모델/진단 반복을 수행합니다.
