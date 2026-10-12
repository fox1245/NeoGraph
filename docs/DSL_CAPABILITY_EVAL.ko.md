<!-- neograph-i18n: source=docs/DSL_CAPABILITY_EVAL.md locale=ko source_sha256=8918e3e19d555bc582f3c36b4a9bfa63dd9b80ea655074a7214a5c5b233a5c27 -->
# QuickJS DSL 기능 범위와 모델 합성 평가

**Languages:** [English](DSL_CAPABILITY_EVAL.md) | [한국어](DSL_CAPABILITY_EVAL.ko.md) | [日本語](DSL_CAPABILITY_EVAL.ja.md) | [简体中文](DSL_CAPABILITY_EVAL.zh-CN.md)

## 평가 질문

NeoGraph는 다음 두 가지 주장을 구분해야 합니다.

1. 승인(admission)된 QuickJS DSL로 어떤 기능을 표현할 수 있다는 주장
2. LLM이 그 기능을 실제로 올바르게 쓰는 소스를 합성할 수 있다는 주장

첫 번째 주장이 맞다고 해서 두 번째 주장이 따라 나오지는 않습니다. 이 평가에서 소스가 통과하려면 실제 `ProgramCompiler`가 소스를 받아들이고, 케이스별 의미 검증기가 lowering을 거친 Core IR 또는 봉인된 JavaScript command tree를 확인해야 합니다. 소스 텍스트에서 키워드를 찾는 것만으로는 충분하지 않습니다.

## 기능 매니페스트(capability manifest)

[`tests/fixtures/dsl_capabilities/cases.json`](../tests/fixtures/dsl_capabilities/cases.json)은 기계가 읽을 수 있는 평가 목록입니다. 현재 다루는 범위는 다음과 같습니다.

- 그래프/채널/노드/entry/엣지/exit와 일반 JavaScript 구성
- 등록된 조건부 라우팅
- 정적 fan-out, fan-in, barrier
- 정적 HITL 인터럽트와 그래프 재시도 정책
- 레지스트리를 거치는 동적 `Send`와 `NodeInterrupt` 동작
- 일반 JavaScript의 분기/반복 제어와 함께 쓰는 `callCore`
- 제한된 `ng.all`로 lowering되는 JavaScript map
- `all`, `parallel`, 일반 `join`, `race`, `quorum`
- `await` 안에 중첩된 자식 `spawn`
- `emit`, `checkpoint`, `cancelScope`
- 승인(admission)된 네이티브 `hostCapability` import 슬롯

저장소에 포함된 JavaScript 픽스처는 모두 `program_dsl_capability_probe`가 컴파일하고 의미 검증을 수행합니다. 이 테스트는 결정적인 CTest 테스트이며 모델이나 네트워크가 필요하지 않습니다.

```powershell
cmake --build build --config Release --target program_dsl_capability_probe
ctest --test-dir build -C Release --output-on-failure `
  -R '^Program\.DslCapability\.'
```

graph-builder만 집중해서 보는 회귀 테스트는 mutator를 반복 호출해도 올바르게 누적되는지, barrier·인터럽트·재시도 선언이 lowering 뒤에도 유지되는지도 증명합니다.

```powershell
build\tests\Release\neograph_program_tests.exe `
  --gtest_filter=ProgramCompilerTest.JavaScriptGraphBuilderLowersEveryDeclaredPrimitiveAndAccumulatesCalls
```

## 실제 모델 평가

opt-in 방식의 러너는 자연어로 된 의미 설명과 공개 API 시그니처를 주고 소스를 요청합니다. 저장소에 있는 정답은 모델에 주지 않습니다. 모델의 응답은 모두 결정적 CTest가 쓰는 것과 같은 네이티브 프로브로 보냅니다. 러너는 `skills/neograph-harness-authoring/SKILL.md`와 `references/quickjs-authoring.md`를 모델의 컨텍스트에 불러오고, 이 안내문의 다이제스트를 보고서에 기록합니다. `--skill`로 다른 진입점을 고를 수 있으며, 그 진입점에도 짝이 되는 참조 문서가 있어야 합니다. 실제 컴파일러 브리지는 호스트가 호출합니다. 모델은 소스를 반환하고, 제한된 횟수의 수정을 위해 진단 메시지를 받습니다. Skill은 런타임 권한을 부여하지 않습니다.

```powershell
bun --env-file=C:\path\to\.env run scripts/run_dsl_capability_eval.ts `
  --probe build\tests\Release\program_dsl_capability_probe.exe `
  --model z-ai/glm-5.3-flash `
  --repair-attempts 2 `
  --output dsl-capability-evidence.json
```

`--case`에는 쉼표로 구분한 일부 케이스를 지정할 수 있고, `--attempts`는 독립적인 one-shot 시도를 반복하며, `--repair-attempts`는 거부된 소스 전체와 함께 프로브가 낸 공식 진단 메시지를 모델에 돌려줍니다. provider나 응답 자체의 실패는 컴파일 거부나 의미 검증 거부와 구분해서 기록합니다. 기본 모델은 `z-ai/glm-5.3-flash`이며, 생성할 때마다 완성 토큰을 4,096개로 제한하고 cookbook의 ZDR provider 라우팅 설정을 사용합니다. 아래의 DeepSeek 결과는 러너가 Skill을 로딩하기 전에 기록한 것입니다.

## 지침은 따로 평가하기

소스 케이스를 통과했다는 것은 Skill, 제공한 네이티브 API 참조, 케이스 계약, 모델 구성이 합쳐진 결과를 측정했다는 뜻입니다. Skill 하나만으로 충분하다는 것을 입증하지는 않습니다. 러너는 시스템 프롬프트, API 참조, 모델이 낸 원문 내용, 사용량, 중단 이유(stop reason), strict-envelope 결과를 증거에 보관합니다. 예전 방식의 소스 추출기는 코드 펜스 안의 JSON도 복구할 수 있습니다. strictEnvelope는 이 경우와, 요청한 대로 원시 JSON 봉투(envelope)를 지킨 경우를 구분합니다.

--reasoning-effort는 별도의 비교 프로파일을 명시적으로 고를 때에만 사용합니다. 생략하면 provider의 기본값이 유지됩니다. 전후 비교마다 이 값, 모델, 샘플링, 출력 상한, 과제, 컴파일러를 고정해야 합니다.

챗봇 내부의 Harness 선택기에는 짝을 이루는 평가기가 있습니다.

~~~powershell
bun run scripts/run_harness_skill_ab.ts --before saved-skill/SKILL.md `
  --after skills/neograph-harness-authoring/SKILL.md `
  --provider relace --repeats 2 --output skill-comparison.json
~~~

두 디렉터리 모두 references/chat-template-proposals.md를 포함해야 합니다. 기본 픽스처에는 인사말, 독립 검토를 계속 요청하는 상황, 기록된 검토 시나리오가 들어 있습니다. 기대하는 plan은 모델의 입력에 넣지 않습니다. 평가기는 실제 채팅 프로파일을 고정하고, AB/BA 순서를 번갈아 실행하며, 원본 응답을 보관하고, JSON 형태의 채점을 기대 plan/confidence의 채점과 따로 합니다. --provider는 fallback을 끄고 백엔드를 고정합니다. 지정하지 않으면 라우팅 정책은 같지만 실제 provider는 달라질 수 있습니다. 반환된 provider/모델 이름은 기록합니다. 케이스와 반복 횟수에는 상한이 있으며, 수정 시도나 조용한 재시도는 하지 않습니다.

이는 프롬프트 수준의 평가이며, 런타임 승인(admission)도 아니고 답변 품질에 대한 통계적 주장도 아닙니다. 특정 케이스를 겨냥해 작게 반복한 표본은 회귀 증거일 뿐, 모델의 일반적인 신뢰도에 대한 추정치가 아닙니다.

## DeepSeek 관찰 결과

실제 모델 실행에서 모델은 11개 기능 그룹 모두에서 프로브로 검증된 소스를 만들었습니다. 정적 HITL/재시도는 진단 메시지에 따라 한 번 수정해야 했습니다. 구조적 동시성은 두 번 수정해야 했는데, 먼저 ES 모듈 export를 되살리고 그다음 잘못된 Core 바인딩을 승인(admission)된 정확한 이름으로 바꿨습니다. 제어 흐름은 정확한 바인딩으로 한 번 고친 뒤 통과했습니다. Map은 호스트가 네이티브 API 매니페스트를 제공하고 승인(admission)된 Core 식별자를 모호하지 않게 명시한 뒤에는 one-shot으로 통과했습니다.

| 기능 케이스 | 모델 결과 | 주요 관찰 |
|---|---|---|
| `graph_basics` | 통과 | 반복문으로 만든 노드가 올바르게 lowering됨 |
| `graph_routing` | 반복 시도에서 통과 | 앞선 출력은 금지된 CommonJS/`require`를 사용함 |
| `graph_fanout_barrier` | 통과 | fan-out 엣지와 barrier 구성원이 정확했음 |
| `graph_hitl_retry` | 한 번 수정한 뒤 통과 | 첫 출력에서 노드 이름이 틀렸음 |
| `registry_mediated` | 통과 | 호스트가 승인(admission)한 동적 노드를 모델이 올바르게 참조함 |
| `program_control_flow` | 한 번 수정한 뒤 통과 | 앞선 시도에서는 승인(admission)된 Core `capability`가 아니라 `core`/`Core`를 반복해서 호출함 |
| `program_map` | 정확한 식별자를 넣은 뒤 통과 | 앞선 시도에서는 CommonJS, `yield*`, 틀린 Core 이름을 사용함 |
| `program_structured_concurrency` | 두 번 수정한 뒤 통과 | 중첩된 command와 Core 바인딩이 정확한지 확인함 |
| `program_spawn_await` | 통과 | `Await(Spawn(...))`와 timeout을 구조 수준에서 검증함 |
| `program_durability` | 통과 | emit, 체크포인트, 취소 command가 정확했음 |
| `program_host_capability` | 통과 | import 슬롯과 정규 입력이 일치함 |

검증기는 command 종류와 입력뿐 아니라 정확한 Core 이름도 확인하며, 중첩된 `callCore`마다 `capability`를 요구합니다. `core`, `Core`, 노드 이름 `work`를 쓴 모델 소스는 거부됩니다.

이 결과는 모델에 그런 능력이 있다는 증명이며, 통계적인 신뢰도에 대한 주장이 아닙니다. 케이스별 one-shot 성공률과 수정 후 성공률을 알려면 반복 시도가 더 필요하고, provider 실패는 따로 보고해야 합니다.

## 생성된 Program에 미치는 영향

모델이 한 번에 생성한 소스를 그대로 쓰는 방식만으로는 제품 보증으로 충분하지 않습니다. 최소한으로 안전한 합성 경로는 다음과 같습니다.

```text
capability manifest + exact admitted identifiers
  -> model source proposal
  -> bounded QuickJS compilation
  -> semantic capability probe
  -> diagnostic-guided repair within a fixed budget
  -> ordinary admission and publication
```

모델은 ES 모듈과 CommonJS, 그래프 이름과 노드 이름, 요청한 Core 식별자와 `core` 같은 일반 단어를 반복해서 혼동했습니다. 따라서 호스트는 정확한 시그니처와 승인(admission)된 식별자를 주입하고, 가능하면 고정된 모듈 골격(scaffold)을 유지해야 합니다. 그럴듯해 보이는 소스를 요청한 토폴로지가 만들어졌다는 증거로 취급해서는 안 됩니다.

NeoGraph는 `ProgramSynthesisGateway`에서 이 경계를 강제합니다. 모든 게이트웨이 구성은 호스트가 소유하는 의미 검증기를 제공해야 합니다. 의미 검증에 성공하면 Catalog 승인(admission) 전에 콘텐츠 주소 방식(content-addressed)의 영수증이 만들어집니다. 거부 판정이 나오면 `ProgramSynthesisValidationError`를 던지고 정확한 증거를 보관하며, 승인(admission) resolver는 호출하지 않습니다. 이미 소비한 동적 컴파일 예약은 소비된 채로 남습니다.

`javascript_authoring_capability_manifest()`는 설치된 graph-builder와 command 어휘, 정확한 시그니처, 분류, 한도, 프로파일 제약을 기계가 읽을 수 있는 데이터로 제공합니다. 적합성 테스트는 이 매니페스트를 두 QuickJS 컨텍스트에 실제로 설치된 프로퍼티와 비교하므로, API가 어긋나면 테스트 스위트가 실패합니다.

매니페스트는 `features`도 공개합니다. 작성 기능마다 불리언 플래그 하나씩(예: `graph.metadata`, `barrier.any_of`, `failure_edges`)이고, 모든 메서드의 TypeScript `dts` 시그니처(공통 선언은 `dts_prelude`에 있음)와 graph-builder 인자마다 JSON Schema도 함께 공개합니다. 스키마 파일은 두 개입니다. [`schemas/javascript-capability-manifest-v1.schema.json`](../schemas/javascript-capability-manifest-v1.schema.json)은 매니페스트 자체를 기술하고, [`schemas/core-definition-v1.schema.json`](../schemas/core-definition-v1.schema.json)은 봉인된 Core 정의를 기술하며 생성한 정의를 제약 디코딩(constrained decoding)할 때 씁니다. 테스트는 매니페스트를 해당 스키마로 검증하고, golden identity 코퍼스의 정의를 Core 스키마로 검증합니다.
