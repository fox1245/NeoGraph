<!-- neograph-i18n: source=docs/DSL_CAPABILITY_EVAL.md locale=ko source_sha256=a2b954c6e311d3acabd6fcc547511af245a49b2303e4897e12628ae7187191c4 -->
# QuickJS DSL 기능 및 모델 합성 평가

**Languages:** [English](DSL_CAPABILITY_EVAL.md) | [한국어](DSL_CAPABILITY_EVAL.ko.md) | [日本語](DSL_CAPABILITY_EVAL.ja.md) | [简体中文](DSL_CAPABILITY_EVAL.zh-CN.md)

상태: 구현됨, 결정적 적합성 게이팅 적용; 실시간 모델 평가는 옵트인. 관찰일: 2026-08-22

## 질문

NeoGraph는 두 가지 주장을 구분해야 한다:

1. 승인(admission)된 QuickJS DSL이 기능을 표현할 수 있다는 것; 그리고
2. LLM이 해당 기능을 실제로 올바르게 사용하는 소스를 합성할 수 있다는 것.

두 번째 주장은 첫 번째 주장에서 함의되지 않는다. 실제 `ProgramCompiler`가 소스를 수락하고 사례별 의미 검증자가 하향 컴파일된 Core IR 또는 봉인된 JavaScript 명령 트리를 확인할 때만 소스가 이 평가를 통과한다. 소스 텍스트 키워드 매칭으로는 충분하지 않다.

## 기능 매니페스트

[`tests/fixtures/dsl_capabilities/cases.json`](../tests/fixtures/dsl_capabilities/cases.json)는 기계 판독 가능한 평가 인벤토리이다. 현재 다음을 포함한다:

- 그래프/채널/노드/엔트리/엣지/엑시트 및 일반 JavaScript 구성;
- 등록된 조건부 라우팅;
- 정적 fan-out, fan-in 및 배리어;
- 정적 HITL 인터럽트 및 그래프 재시도 정책;
- 레지스트리 매개 동적 `Send` 및 `NodeInterrupt` 동작;
- 일반 JavaScript 분기/루프 제어를 포함한 `callCore`;
- 제한된 `ng.all`로 하향 컴파일된 JavaScript map
- `all`, `parallel`, generic `join`, `race`, 그리고 `quorum`;
- 자식 `spawn`이 `await` 내부에 중첩됨;
- `emit`, `checkpoint`, and `cancelScope`; and
- 승인된 네이티브 `hostCapability` 임포트 슬롯.

각 체크인된 JavaScript 픽스처는 `program_dsl_capability_probe`에 의해 컴파일되고 의미 검증됩니다. 이들은 결정적 CTest 테스트이며 모델이나 네트워크가 필요 없습니다.

```powershell
cmake --build build --config Release --target program_dsl_capability_probe
ctest --test-dir build -C Release --output-on-failure `
  -R '^Program\.DslCapability\.'
```

집중된 그래프 빌더 회귀 테스트는 반복적인 뮤테이터 호출이 올바르게 누적되며, 배리어, 인터럽트, 재시도 선언이 하향 변환(lowering) 후에도 유지된다는 것도 증명합니다.

```powershell
build\tests\Release\neograph_program_tests.exe `
  --gtest_filter=ProgramCompilerTest.JavaScriptGraphBuilderLowersEveryDeclaredPrimitiveAndAccumulatesCalls
```

## 라이브 모델 평가

옵트인 러너는 자연어 의미론과 공개 API 시그니처를 사용하여 소스를 요청합니다. 모델에게 체크인된 답변을 제공하지 않습니다. 모든 응답은 결정적 CTest에서 사용하는 동일한 네이티브 프로브로 전송됩니다.

```powershell
bun --env-file=C:\path\to\.env run scripts/run_dsl_capability_eval.ts `
  --probe build\tests\Release\program_dsl_capability_probe.exe `
  --model deepseek/deepseek-v4-flash-0731 `
  --repair-attempts 2 `
  --output dsl-capability-evidence.json
```

`--case` 는 쉼표로 구분된 하위 집합을 허용하고, `--attempts` 는 독립적인 일회성 시도를 반복하며, `--repair-attempts` 는 거부된 전체 소스와 함께 권위 있는 프로브 진단을 모델에 반환한다. 공급자/응답 실패는 컴파일 또는 의미 rejection과 분리되어 유지된다.

## 관찰된 DeepSeek 결과

초기 실행과 정확한 식별자 재평가에 걸쳐, 모델은 11개 기능 그룹 모두에 대해 프로브 검증된 소스를 생성했습니다. 정적 HITL/재시도는 진단 기반 수리 한 번이 필요했습니다. 구조적 동시성은 두 번의 수리가 필요했습니다: 첫 번째는 ES 모듈 내보내기를 복원하기 위한 것이고, 두 번째는 잘못된 Core 바인딩을 정확히 승인된 이름으로 교체하기 위한 것이었습니다. 제어 흐름은 정확한 바인딩 수리 한 번 후 통과했습니다. Map은 호스트가 네이티브 API 매니페스트를 제공하고 승인된 Core 식별자를 명확하게 명시한 후 한 번에 통과했습니다.

| 기능 사례 | 모델 증거 | 중요 관찰 |
|---|---|---|
| `graph_basics` | Passed | 루프 기반 노드가 올바르게 낮아짐 |
| `graph_routing` | 반복된 시도에서 통과함 | 이전 출력은 금지된 CommonJS/`require`를 사용함 |
| `graph_fanout_barrier` | Passed | Fan-out edges와 barrier 멤버십이 정확함 |
| `graph_hitl_retry` | 수리 한 번 후 통과 | 초기 출력이 잘못된 노드 이름을 사용함 |
| `registry_mediated` | Passed | Model 이 호스트가 승인(admission)한 동적 노드를 올바르게 참조 |
| `program_control_flow` | 수리 한 번 후 통과 | 이전 시도들은 반복적으로 `core`/`Core`를 호출했으며, 승인(admission)되지 않은 Core `capability`는 호출하지 않았습니다. |
| `program_map` | 정확한 식별자 주입 후 통과 | 이전 시도에서는 CommonJS, `yield*`, 그리고 잘못된 Core 이름을 사용했습니다. |
| `program_structured_concurrency` | 수리 후 통과 | 중첩된 명령과 Core 바인딩이 정확히 검증되었습니다. |
| `program_spawn_await` | Passed | `Await(Spawn(...))` 및 timeout이 구조적으로 검증되었습니다. |
| `program_durability` | Passed | Emit, 체크포인트 및 취소 명령이 정확히 일치했습니다. |
| `program_host_capability` | Passed | 가져오기 슬롯과 표준 입력이 일치했습니다. |

초기 검증기는 `callCore` 사례에 대해 거짓 양성을 생성했는데, 이는 명령 종류와 입력을 확인했지만 정확한 Core 이름을 확인하지 않았기 때문입니다. 검증기는 모든 중첩된 `capability` 에 대해 `callCore`를 요구하도록 강화되었습니다. 이전에 허용되었던 `core`, `Core`, 또는 노드 이름 `work` 을 사용하는 모델 소스는 이제 올바르게 거부됩니다.

이는 능력에 대한 증명일 뿐이며 통계적 신뢰성 주장이 아닙니다. 사례별 일회성 시도 및 복구 성공률은 여전히 공급자 실패를 별도로 보고한 상태에서 반복 시험이 필요합니다.

## 생성된 Program에 대한 귀결

원시 일회성 소스 생성은 충분한 제품 보증이 아닙니다. 최소 안전 합성 경로는 다음과 같습니다:

```text
capability manifest + exact admitted identifiers
  -> model source proposal
  -> bounded QuickJS compilation
  -> semantic capability probe
  -> diagnostic-guided repair within a fixed budget
  -> ordinary admission and publication
```

모델은 ES 모듈과 CommonJS, 그래프 이름과 노드 이름, 요청된 Core 식별자와 `core`와 같은 일반 단어를 반복적으로 혼동했습니다. 따라서 NeoCode는 정확한 시그니처와 승인된 식별자를 주입하고 가능한 경우 고정 모듈 골격을 유지해야 하며, 그럴듯해 보이는 소스를 요청된 토폴로지가 구축되었다는 증거로 취급해서는 안 됩니다.

NeoGraph는 이제 `ProgramSynthesisGateway`에서 이 경계를 강제합니다: 모든 게이트웨이 구성은 호스트 소유 의미 검증기를 제공해야 합니다. 검증 성공 시 Catalog 승인(admission) 전에 콘텐츠 주소 기반 영수증을 생성하며, 거부된 결정은 `ProgramSynthesisValidationError`를 발생시키고, 정확한 증거를 보존하며, 승인(admission) 리졸버를 절대 호출하지 않습니다. 이미 소비된 동적 컴파일 예약은 계속 소비된 상태로 유지됩니다.

`javascript_authoring_capability_manifest()`는 설치된 그래프 빌더 및 명령 어휘, 정확한 서명, 분류, 제한 및 프로필 제약 사항을 기계가 읽을 수 있는 데이터로 노출합니다. 적합성 테스트는 해당 매니페스트를 두 QuickJS 컨텍스트에 설치된 실제 속성과 비교하므로 API 드리프트가 발생하면 테스트 스위트가 실패합니다.
