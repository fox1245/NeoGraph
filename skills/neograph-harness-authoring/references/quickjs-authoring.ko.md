<!-- neograph-i18n: source=skills/neograph-harness-authoring/references/quickjs-authoring.md locale=ko source_sha256=53dc89ee33f26b7d0f9c6d328453f73aaaf9e53f0b8b4cea360ca1cdc2b800c8 -->
# QuickJS 소스 작성

**Languages:** [English](quickjs-authoring.md) | [한국어](quickjs-authoring.ko.md) | [日本語](quickjs-authoring.ja.md) | [简体中文](quickjs-authoring.zh-CN.md)

## 호스트 계약 확인

코드를 작성하기 전에 다음 바인딩을 확인하세요. 제공된 설정을 사용하거나
사용 가능한 스키마를 살펴보세요. 누락된 이름을 그럴듯한 추측으로 채우지 마세요.

| 계약 | 필요한 정보 |
|---|---|
| JS API 매니페스트 | 이 빌드의 정확한 빌더 및 명령 시그니처 |
| 레지스트리 | 노드/리듀서/조건/임포트 이름, 노드 설정 스키마와 효과 |
| 호출 | 입력 JSON 형태와 callCore로 호출할 수 있는 Core 이름 |
| 결과 | Core 채널 이름과 제너레이터에 요구되는 최종 출력 |
| 자식 | 승인된 바인딩 이름, 자식 입력/출력, 부여된 한도 |
| 컴파일러 브리지 | 허용되는 소스 봉투, 진단, 남은 수정 허용량 |

내장 JS 매니페스트에는 문법이 나열되며 애플리케이션 노드는 나열되지 않습니다.
예를 들어 probe.node는 기능 시험 팔레트에 속하며 애플리케이션 LLM 노드가
아닙니다. 고정된 기능 프로브는 각 사례의 그래프/명령 계약을 검사하며,
임의의 프로덕션 동작을 검사하지 않습니다.

## 컴파일 시점 그래프와 런타임 Program

**define()**은 열린 빌더 하나를 반환하는 동기식 컴파일 시점 함수입니다.
해당 빌더로 노드와 간선을 만드세요. 빌더 메서드는 노드 핸들이 아니라
빌더를 반환합니다. 노드 동작은 호스트에 등록된 C++ 구현에서 나오며,
일반적인 JS 콜백은 그래프 노드가 아닙니다.

**main(input)**은 나중에 실행되는 선택적 동기식 제너레이터입니다. 순수 JS로
분기를 선택하고, 배열과 루프를 만들고, JSON을 변환할 수 있습니다.
런타임 효과는 제너레이터에서 yield하는 봉인된 ng 명령을 사용합니다.
define()에서 런타임 명령을 호출하거나, 게시된 그래프를 변경하거나,
그래프 형태의 객체를 반환하거나, async 함수, 프로미스, 타이머, require(),
주변 환경 I/O, eval, 동적 임포트를 사용하지 마세요.

다음 연결 예제는 호스트가 프로브 팔레트를 등록했고 숫자 input.value를
요구한다고 가정합니다. 이 노드는 아무 동작도 하지 않으므로 데이터 흐름을
보여 줍니다. LLM 작업이나 이름이 지정된 기능 사례의 답변을 보여 주지는 않습니다.

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

callCore에서는 호스트가 제공한 그래프 이름을 정확히 사용하세요. "main",
"example", "capability" 또는 노드 이름 중 어느 것도 범용 별칭이 아닙니다.

## 데이터 흐름과 토폴로지

일반적인 callCore 입력은 채널 이름에서 들어오는 값으로의 맵입니다.
선언된 각 채널은 등록된 리듀서를 적용합니다. 일반적인 Core 결과는
직렬화된 채널을 포함합니다. 채널의 래퍼가 아니라 값을 읽으세요.

예를 들어 챗봇의 기존 템플릿은 다음과 같이 답변 노드를 호출합니다.

~~~javascript
const reply = yield ng.callCore(
  "main", {payload: {phase: "answer", task: task}}, "answer"
);
const answer = reply.channels.result.value;
~~~

여기서 payload/result는 선언된 채널이며 chat.step은 payload.phase를 읽습니다.
이 이름과 동작은 해당 호스트의 레지스트리/템플릿에서 나옵니다. 이 조각을
다른 레지스트리에 복사한다고 해당 바인딩이 생성되지는 않습니다.

선언만 있는 모듈은 Core 결과 형태를 유지합니다. main()이 있으면 제너레이터의
반환값은 별도로 승인된 Program 출력 계약에 맞아야 합니다. 기능 평가기는
때때로 {accepted: true} 같은 합성 명령 응답을 제공합니다. 합성 응답에
Core 채널 래퍼를 덧붙이지 말고 해당 사례에 명시된 응답 계약을 사용하세요.

| 의도 | 구성과 주의할 사항 |
|---|---|
| 선형 경로 | 노드, 진입/종료, 모든 연결 간선을 추가하세요 |
| 조건부 라우팅 | conditionalEdge(from, registeredCondition, routes); 모든 조건 레이블을 노드에 매핑하세요 |
| 정적 팬아웃/팬인 | 나가는 두 분기와 들어오는 합류 간선을 추가하세요. 합류에 모든 분기가 필요하면 barrier(joinNode, branchNames)를 추가하세요 |
| 병렬 쓰기 | 동시 쓰기를 처리하는 호스트 리듀서/채널을 선택하세요. 간선만으로 병합이 정의되지는 않습니다 |
| 그래프 인터럽트/재시도 | 매니페스트의 interruptBefore/After 및 retryPolicy 키를 사용하세요. 이는 JS 루프나 논리적 재시도와 별개입니다 |

## 런타임 명령 조합

- callCore(coreName, input, site)는 명령을 생성합니다. 이를 yield하면
  Core가 호출되고 결과가 반환됩니다.
- all(commands, {max_in_flight: N}, site)는 봉인된 명령을 받습니다. 일반적인
  JS로 목록을 만들고 all 명령을 한 번 yield하세요. 원시 배열을 yield하거나,
  명령을 프로미스로 바꾸거나, 봉인된 명령에 yield*를 사용하지 마세요.
- spawn(binding, input, site)는 호스트가 승인한 자식 바인딩을 선택합니다.
  결과를 기다리려면 명령을 await(spawnCommand, timeoutMs, site)로 감싸세요.
- checkpoint(state, site)는 명시적인 JSON 상태를 게시합니다. 자체적으로
  무엇을 컴파일하거나 승인하거나 교체하지는 않습니다.
- emit과 cancelScope는 매니페스트에 선언된 의미를 따릅니다. hostCapability에는
  승인된 임포트 슬롯이 필요합니다. 임의의 네이티브 API로 가는 경로가 아닙니다.

예를 들어 승인된 바인딩과 그 정확한 원래 입력이 주어진 경우:

~~~javascript
const result = yield ng.await(
  ng.spawn(binding, originalChildInput, "child:spawn"),
  timeoutMs,
  "child:wait"
);
~~~

내보낸 자식 ID 자체는 봉인된 await 명령이 아닙니다. 호스트의 실제
합류/복구 계약을 사용하세요. 교체 과정에서 자식을 유지하는 경우에는
[runtime-handoffs.md](runtime-handoffs.md)를 참고하세요.

소스 위치 레이블은 영속 좌표의 일부입니다. 안정적인 작업 식별자나 결정적인
인덱스에서 레이블을 도출하고, 시각/난수 값은 피하세요. 이미 완료된 작업은
재생 시 원래 입력을 유지해야 합니다. JS 루프, 재시도, 병렬 분기 또는
새 자식이 예산을 보충하지는 않습니다.

accepted=false처럼 성공한 결과를 기준으로 반복하는 루프는 논리적 재시도입니다.
실패한 Core 명령은 Program 결과입니다. 일반적인 JS try/catch로 이를 재개할 수
있다고 가정하지 마세요. 호스트의 실패/재개/대조·조정 계약을 사용하세요.

## 출력, 컴파일, 수정

활성 인터페이스가 요구하는 봉투를 사용하세요.
- 소스 평가: source 문자열이 전체 모듈인 JSON 객체 하나만 반환하세요.
  Markdown 펜스, 패치, ProgramBundle JSON 또는 설명문은 넣지 마세요.
- Harness MCP: 먼저 neograph_schema를 조회하세요. 모듈을
  harness.mode="javascript", source_id, source 아래에 넣고, 필요한
  작업/작업자/예산/정책 필드를 모두 함께 제공하세요. 소스만으로는 완전한 MCP 요청이 아닙니다.
- 호스트 네이티브 제안: 해당 호스트의 스키마를 따르세요. 이 스킬은
  범용 자유 형식 컴파일 또는 교체 RPC를 정의하지 않습니다.

제공된 컴파일러 브리지로 제출하세요. 평가 모드에서는 호스트가 반환된 소스를
제출하고 진단을 반환합니다. 모델이 호출할 수 있는 도구가 있다는 뜻은 아닙니다.
MCP 모드에서는 해당 도구를 사용할 수 있을 때만 neograph_compile을 사용하세요.

실패하면 보고된 코드/경로/소스 위치를 찾아 위반된 계약을 수정하세요. 예를 들어:
- 알 수 없는 노드/리듀서/조건: 등록된 바인딩과 설정에 맞추세요.
- 잘못된 Core 바인딩: 모든 명령에서 승인된 그래프 이름에 맞추세요.
- 명령이 아닌 값의 yield: 봉인된 명령 하나 또는 승인된 구조화된 합류를 생성하세요.
- 잘못된 출력/입력: 채널 매핑 또는 Program 결과 계약을 바로잡으세요.
- 누락된 간선/배리어: 소스 문구만 고치지 말고 낮춰 변환된 토폴로지를 수정하세요.

기존 수정 허용량 안에서 완전한 대체 소스를 반환하세요.
진단을 지우거나, 권한을 확대하거나, 무관한 작업을 추가하거나,
컴파일러와 작업별 검사가 승인하기 전에 성공했다고 주장하지 마세요.

체크아웃의 기능 브리지는 다음과 같이 살펴보고 사용할 수 있습니다.

~~~text
program_dsl_capability_probe --manifest
program_dsl_capability_probe graph_basics source.js
~~~

빌드 대상은 program_dsl_capability_probe입니다. 이름이 지정된 사례에는 각각
제공된 계약이 필요합니다. 컴파일 성공만으로 사례를 통과한 것은 아니며,
프로브 성공이 프로덕션 실행 권한을 부여하지도 않습니다. 실행기
scripts/run_dsl_capability_eval.ts는 제한된 횟수의 모델/진단 반복을 수행합니다.
