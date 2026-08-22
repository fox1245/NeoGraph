<!-- neograph-i18n: source=docs/STRICT_RUNTIME_INTERPOSITION.md locale=ko source_sha256=59193d3d0f34fd9e49284edb43ce62f5ec0352c27fdcfce47bf1dceec7f9454a -->
# 엄격한 런타임 컨텍스트

**Languages:** [English](STRICT_RUNTIME_INTERPOSITION.md) | [한국어](STRICT_RUNTIME_INTERPOSITION.ko.md) | [日本語](STRICT_RUNTIME_INTERPOSITION.ja.md) | [简体中文](STRICT_RUNTIME_INTERPOSITION.zh-CN.md)

NeoGraph의 엄격한 런타임 경로는 필수 컨텍스트, 라이프사이클 Hook, 공급자 디스패치 증거를 모델 재량에서 벗어나게 한다. 이는 추가적이다: 신뢰할 수 있는 임베딩을 위한 기존의 직접 공급자 호출은 여전히 존재하며, `StrictRuntimeProfile`가 엄격한 경로에 필요한 의존성을 조립한다.

## 보장 경계

```text
durable RAW history + admitted artifacts + required Skills/constraints
  -> immutable ContextEpoch
  -> RuntimeTurnAssembler
  -> ContextAssemblyReceipt
  -> mandatory BeforeProviderRequest Hooks
  -> durable ProviderDispatchReceipt
  -> provider
  -> ProviderDispatchOutcomeReceipt
  -> mandatory AfterProviderResponse Hooks
```

이 보장은 정확한 컨텍스트 구성, 필수 아티팩트 존재, 요청 식별, 디스패치 승인(admission), 알려진/조정이 필요한 공급자 결과를 포함한다. LLM이 모든 토큰에 주의를 기울였거나 따랐다는 주장은 하지 않는다.

호스트가 작성한 사용자 정의 네이티브 노드는 신뢰된 코드로 남아 있다. 이러한 노드에 원시 `Provider`를 부여하는 것은 의도적으로 엄격한 프로필을 벗어난다. 생성된 토폴로지는 등록된 노드만 수신하며 해당 권한을 제조할 수 없다.

## 엄격한 프로파일

`StrictRuntimeProfileConfig`는 다음을 요구한다:

- 공급자;
- `DurableContextStore` 하나;
- 터미널 결과 지원을 갖춘 `DurableProviderDispatchReceiptStore`;
- `HookRuntime`;
- 콘텐츠 주소 기반 공급자 바인딩 아이덴티티;
- 0이 아닌 입력 토큰 상한; 그리고
- 선택적인 정확한 필수 컨텍스트 및 Skill 아티팩트 아이덴티티.

`RuntimeGuaranteeProfile::Strict` 에포크만 활성화될 수 있다. 프로파일을 `GraphEngine`에 연결하는 것은 내장된 소비자에 공급자 인터포지션과 라이프사이클 Hook을 모두 설치한다.

## 공급자 결과 라이프사이클

공급자 경계는 이제 두 개의 별도 불변 값을 기록합니다:

1. `ProviderDispatchReceipt`는 디스패치 전에 기록됩니다.
2. `ProviderDispatchOutcomeReceipt`는 시도 후에 `Succeeded`, `Failed` 또는 `ReconciliationRequired`를 기록한다.

성공적인 결과는 정규화된 완료의 다이제스트를 바인딩한다. 발송 후 발생한 예외는 원격 제공자가 동작했는지 증명할 수 없으므로 컨트롤러는 재시도 대신 `ReconciliationRequired`를 기록한다. SQLite 스키마 v3는 결과를 별도로 저장하고 재시작 후 각 결과가 정확히 승인된 발송 영수증을 여전히 바인딩하는지 검증한다.

## 네이티브, stdio 또는 HTTP에 대한 필수 Hook

`MandatoryHookRunner`는 기존 네이티브 어댑터 또는 전송 중립 `HookExecutionBackend`를 수용한다. `RpcHookExecutionAdapter`는 `HookRpcExecutor`를 해당 백엔드에 바인딩한다. 동일한 고정된 `hooks/invoke` JSON-RPC 메서드는 `StdioJsonRpcTransport` 또는 `HttpJsonRpcTransport`를 사용할 수 있다.

RPC Hook 아티팩트는 증거이지 권위가 아닙니다. `ContextStoreHookArtifactPublisher`는 다음과 같은 아티팩트만 허용합니다:

- 종류가 `HookOutput`입니다;
- `source_digest`는 정확한 Hook 호출 ID와 동일하며; 그리고
- 런타임 이벤트가 호출과 일치합니다.

게시는 소유자 범위에 국한되고 멱등적이다. 외부 효과는 성공했지만 해당 산출물을 게시할 수 없는 경우 Hook은 `ReconciliationRequired`로 확정된다. 완전한 성공으로 보고되지 않는다.

## 필수 컨텍스트 및 변환

`RuntimeContextRequirements`는 모든 필수 산출물 ID를 `RequiredSkill` 산출물이어야 하는 하위 집합과 분리한다. `HardConstraint`는 전용 필수 산출물 유형이다. 모든 필수 산출물은 활성 에포크에 의해 선택되어야 하며, `required=true`를 유지해야 하며, 필수 토큰 수에 기여한다.

`ContextTransformReceipt`는 v1에서 의도적으로 보수적입니다. 변환기는 선택적 증거를 대체하거나 압축할 수 있지만, 모든 필수 입력 아티팩트 ID는 출력 집합에 바이트 단위로 동일하게 나타나야 합니다. 의역은 제약 보존의 증거로 인정되지 않습니다.

## 런타임 개발자 지침

`RuntimeDeveloperInstruction`는 불변의 개발자 입력이며, 권한이 아닙니다. `RuntimeInstructionController::submit_and_plan`는 다음 순서를 수행합니다:

```text
append Developer-trust history record
  -> load the exact active Program lineage/generation
  -> call the host planner
  -> validate decision against the current lineage head
  -> require an exact already-admitted target for transition decisions
  -> persist the required decision artifact
```

확정된 결정 사항은 다음과 같습니다:

- `SatisfiedInPlace`;
- `Rejected`;
- `ReplaceAtHandoff`; 및
- `MigrateGraph`.

전환 적용 시 기존 `ProgramRuntime::replace` 또는 `migrate_graph` 경로에 위임하기 직전에 lineage head를 즉시 다시 검사합니다. 오래된 결정은 권위를 가질 수 없습니다.

## 제한된 Program합성

`ProgramSynthesisGateway`는 호스트 소유의 생성된 후속 경로를 제공합니다:

```text
immutable ProgramSynthesisProposal
  -> durable host reservation receipt
  -> bounded QuickJS compilation
  -> proposal capability/effect closure check
  -> host-owned semantic contract validation
  -> ordinary ProgramCatalog admission
  -> immutable ProgramSynthesisReceipt
```

예약은 재생 불가능한 `max_dynamic_compiles` 단위 하나의 정확한 감소를 보여야 하며 다른 예산을 증가시킬 수 없습니다. 예약은 컴파일 전에 발생하므로, 거부된 소스는 컴파일 단위를 돌려받지 않습니다. 의미 검증은 필수이며 컴파일 후, 승인(admission) 리졸버 전에 실행됩니다. 그 불변 영수증은 제안, 예약, 컴파일된 번들, 검증자 신원, 의미 계약 신원, 평결, 증거 다이제스트를 바인딩합니다. 거부된 평결은 타입 있는 증거를 노출하며 `ProgramVersion`를 게시할 수 없습니다. 게이트웨이는 그 결과를 활성화, 바인딩, 마이그레이션, 또는 스폰하지 않습니다. 그러한 것들은 기존 Program API를 통한 별도의 호스트 결정으로 남습니다.

런타임 명령 플래너는 게이트웨이를 호출한 다음 교체 또는 마이그레이션 결정에서 정확히 승인된 버전을 반환할 수 있습니다. 이는 다음을 보존합니다:

```text
proposal -> reserve -> compile -> semantic validate -> admit -> decide -> migrate/spawn
```

commentary (explanation): without exposing, with design, Allel: Another CLI. Other risks:: because the generated JavaScript... impossible, as Compiler and compile details, security, Submit.)
