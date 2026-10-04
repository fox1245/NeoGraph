<!-- neograph-i18n: source=docs/STRICT_RUNTIME_INTERPOSITION.md locale=ko source_sha256=9b872c2d049d049aa2c5e7393b485bc79c3b89a5268878b1fff5a263c347208f -->
# 엄격한 런타임 컨텍스트

**Languages:** [English](STRICT_RUNTIME_INTERPOSITION.md) | [한국어](STRICT_RUNTIME_INTERPOSITION.ko.md) | [日本語](STRICT_RUNTIME_INTERPOSITION.ja.md) | [简体中文](STRICT_RUNTIME_INTERPOSITION.zh-CN.md)

NeoGraph의 엄격한 런타임 경로는 필수 컨텍스트, 라이프사이클 Hook, 공급자 dispatch 증거를 모델 재량에서 분리한다. 신뢰된 임베딩은 직접 typed provider 호출을 사용할 수 있고, `StrictRuntimeProfile`은 엄격한 경로의 의존성을 조립한다.

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

성공 결과는 전체 SDK outcome 관측의 digest를 바인딩한다. 전송되지 않았음이 입증된 typed Failure는 `Failed`, 전달이 불확실하면 `ReconciliationRequired`를 기록한다. dispatch 뒤 예외만으로 원격 공급자의 실행 여부를 알 수 없으므로 컨트롤러는 암묵적으로 재시도하지 않는다. SQLite schema v3는 terminal receipt를 별도로 저장하고 재시작 후 정확한 admitted dispatch binding을 검사한다. Receipt digest는 증거이지 native continuation custody나 실행 가능한 저장 outcome이 아니다.

컨트롤러는 `ProviderRequest`를 받아 불변 소유 `sp::runtime::Result`를 반환하며 순서 있는 메시지/part와 부분 실패 증거를 보존한다. 실제 결과 뒤 정산이나 receipt 영속화가 실패하면 `ProviderDispatchOutcomePersistenceError`가 결과와 원래 cause를 보존하며, 이차 observer 실패는 `delivery_error()`에 남는다. 토큰 charge/reservation은 nullable provider 사용량 report와 별개다.

## Program Core provider 호출 (독립 Strict Runtime과 별개)

Program이 내장 Core LLM node를 쓰는 경우 호스트는
`RuntimeConfig::core_provider_call_resolver`와
`require_core_provider_call_broker = true`를 설정할 수 있다. 정확한
`ProgramCoreProviderCallContext`마다
`SQLiteProgramProviderCallJournal::bind(context, deployment_identity)`를 반환한다.
헤더는 `<neograph/program/sqlite_provider_call_broker.h>`, 링크 대상은
`neograph::program_sqlite`다. Deployment identity는 실제 provider route,
model deployment, credential version을 포함한 권한의 호스트 소유 SHA-256
identity다. Broker는 이를 `Provider`에서 추측하지 않는다. 재시작/reconnect
시 같은 durable database를 다시 바인딩한다.

Journal은 owner, 불변 Program version, run, operation, Core thread/task/node,
내장 call ordinal을 키로 삼는다. Request 내용이나 Program attempt는 키가
아니다. 전송 전에 SQLite FULL 동기화로 marker를 commit한다. Marker는 전송이
일어났을 수 있음을 뜻하며 provider 수신이나 exactly-once 효과를 증명하지 않는다.
전체 불변 SDK Completion/Failure outcome을 encoding version 2로 저장하여
정확히 바인딩된 replay에 사용한다. 전송되지 않았음이 입증된 Failure는 `Failed`,
불확실한 전달·예외·정산 전 crash는 reconciliation이 필요하며 암묵적으로
재dispatch하지 않는다. 상태는
`inspect(owner, logical_call_id(context, core_identity))`로 확인한다.
`reconcile_success`는 독립적으로 확인된 provider-side 증거와 전체 Completion
outcome이 있을 때만 사용한다. Streaming replay는 captured outcome을 반환하고
stream event를 만들어내지 않는다.

늘어난 output cap은 새 semantic call이지 같은 journal slot의 transport retry나 replay가 아니다. Interface 4는 native replay configuration에서만 cap을 제외하며 prepared-request digest와 보수적인 resource claim에는 cap이 남는다. 승인된 각 call에 고유하고 결정적인 ordinal을 주고 모든 attempt의 outcome/accounting과 원래 deadline을 보존하며 같은 resource bank에서 admission을 받아야 한다. 기존 slot의 digest를 바꾸면 거부한다. Native history나 cursor는 credit을 갱신하거나 uncertain delivery, observer/settlement 실패 후 새 전송을 허용하지 않는다.

순서 있는 message part, raw 관측, nullable 사용량, attempt metadata,
native continuation을 저장 결과에 그대로 보존한다. Native outcome은
`SQLiteProgramProviderCallJournal(database_path, native_archive)`에 전달한
호스트의 `sp::NativeArchive`가 필요하다. Portable JSON projection은 그 권한을
재생성하지 못한다. 이전의 lossy receipt는 upgrade하거나 조용히 재dispatch하지
않고 거부한다. Journal은 보수적인 claim/committed 토큰 양을 provider report와
분리해 보존한다. Durable filesystem database 경로를 사용한다. 빈 경로,
`:memory:`, `file:` URI는 거부한다.

이 broker는 assembled `ContextEpoch`가 아니라 Core의 기존 ReAct message
state를 사용한다. 같은 내장 호출에서 engine Strict Runtime interposition과
함께 사용할 수 없다. 호스트가 작성한 native Provider 호출은 범위 밖이다.


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

이 경로는 생성된 JavaScript에 compiler, Catalog, credential, activation 권한을 노출하지 않는다.
