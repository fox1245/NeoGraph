<!-- neograph-i18n: source=docs/STRICT_RUNTIME_INTERPOSITION.md locale=ko source_sha256=eee79303f2ce57761c1bf62d99755e7589782385c4ffa411c4d5c6ad4ad55207 -->
# 엄격한 런타임 인터포지션

**Languages:** [English](STRICT_RUNTIME_INTERPOSITION.md) | [한국어](STRICT_RUNTIME_INTERPOSITION.ko.md) | [日本語](STRICT_RUNTIME_INTERPOSITION.ja.md) | [简体中文](STRICT_RUNTIME_INTERPOSITION.zh-CN.md)

NeoGraph의 엄격한 런타임 경로는 필수 컨텍스트, 라이프사이클 Hook, provider 디스패치 증거를 모델이 임의로 건너뛸 수 없게 만듭니다. 신뢰할 수 있는 임베딩에서는 타입이 있는 provider를 직접 호출할 수 있으며, `StrictRuntimeProfile`은 엄격한 경로에 필요한 의존성을 조립합니다.

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

이 보장은 컨텍스트를 정확히 구성했는지, 필수 아티팩트가 빠지지 않았는지, 요청을 식별할 수 있는지, 디스패치를 승인(admission)했는지, provider 결과가 알려져 있는지 아니면 조정(reconciliation)이 필요한지를 다룹니다. LLM이 모든 토큰에 주의를 기울였거나 지시를 따랐다는 것까지 보장하지는 않습니다.

호스트가 작성한 사용자 정의 네이티브 노드는 계속 신뢰하는 코드입니다. 이런 노드에 원시 `Provider`를 건네면 의도적으로 엄격 프로파일을 벗어납니다. 생성된 토폴로지는 등록된 노드만 받을 수 있으며, 이 권한을 스스로 만들어 낼 수 없습니다.

## 엄격 프로파일

`StrictRuntimeProfileConfig`에는 다음이 필요합니다.

- provider 하나
- `DurableContextStore`
- 최종 결과(terminal outcome)를 지원하는 `DurableProviderDispatchReceiptStore`
- `HookRuntime`
- 콘텐츠 주소 방식(content-addressed)의 provider 바인딩 식별자
- 0이 아닌 입력 토큰 상한
- 선택 사항으로, 필수 컨텍스트와 Skill 아티팩트의 정확한 식별자

활성화할 수 있는 에포크는 `RuntimeGuaranteeProfile::Strict` 에포크뿐입니다. 프로파일을 `GraphEngine`에 연결하면 내장 소비자(consumer)에 provider 인터포지션과 라이프사이클 Hook이 모두 설치됩니다.

## provider 결과의 수명 주기

이제 provider 경계에서는 서로 분리된 불변 값 두 개를 기록합니다.

1. `ProviderDispatchReceipt`는 디스패치 전에 기록합니다.
2. `ProviderDispatchOutcomeReceipt`는 시도가 끝난 뒤 `Succeeded`, `Failed`, `ReconciliationRequired` 중 하나를 기록합니다.

성공한 결과에는 SDK 결과 관찰 전체의 다이제스트가 묶입니다. 전송되지 않았음이 증명된 타입 Failure는 `Failed`로 기록하고, 전달 여부가 불확실하면 `ReconciliationRequired`로 기록합니다. 디스패치 이후에 예외가 발생해도 원격 provider가 실제로 동작했는지는 알 수 없으므로, 컨트롤러는 조용히 재시도하지 않습니다. SQLite 스키마 v3는 최종 영수증을 따로 저장하고, 재시작한 뒤에 승인(admission)된 디스패치 바인딩과 정확히 일치하는지 검사합니다. 영수증 다이제스트는 증거일 뿐이며, 네이티브 continuation의 보관(custody)이나 실행 가능한 저장 결과가 아닙니다.

컨트롤러는 `ProviderRequest`를 받아 소유권이 있는 불변 `sp::runtime::Result`를 반환합니다. 이 결과에는 순서가 있는 메시지/파트와 부분 실패 증거가 남습니다. 실제 결과를 받은 뒤 정산(settlement)이나 영수증 영속화에 실패하면 `ProviderDispatchOutcomePersistenceError`가 그 결과와 원래 원인을 보존하며, 이차로 발생한 observer 실패는 `delivery_error()`에 남습니다. 토큰 청구/예약은 null일 수 있는 provider 사용량 보고서와 별개로 관리합니다.

## Program Core provider 호출 (독립 실행 Strict Runtime과는 별개)

Program이 내장 Core LLM 노드를 쓰면, 호스트는 `RuntimeConfig::core_provider_call_resolver`를 설정하고 `require_core_provider_call_broker = true`로 지정할 수 있습니다. resolver는 `ProgramCoreProviderCallContext`마다 `<neograph/program/sqlite_provider_call_broker.h>`의 `SQLiteProgramProviderCallJournal::bind(context, deployment_identity)`를 반환해야 합니다. 이를 쓰려면 `neograph::program_sqlite`를 링크합니다. 배포 식별자(deployment identity)는 실제 provider 경로, 모델 배포, 권한(자격 증명 버전 포함)에 대해 호스트가 소유하는 SHA-256 식별자여야 합니다. broker는 이 값을 `Provider` 객체에서 알아내지 않습니다. 재시작하거나 재연결할 때는 같은 영속 데이터베이스를 다시 바인딩합니다.

저널은 호출마다 소유자, 불변 Program 버전, run, operation, Core의 thread/task/node, 내장 호출 순번(ordinal)을 키로 사용하며, 요청 내용이나 Program attempt는 키에 **포함하지 않습니다**. 마커는 전송 전에 SQLite FULL 동기화로 커밋합니다. 이 마커는 전송이 *일어났을 수도 있다*는 뜻일 뿐, provider가 요청을 받았다거나 효과가 정확히 한 번 일어났다는 뜻이 아닙니다. SDK의 Completion/Failure 결과는 불변 값 전체를 인코딩 버전 2로 저장하며, 정확히 바인딩된 재생(replay)에 쓰입니다. 전송되지 않았음이 증명된 Failure는 `Failed`로 기록합니다. 전달 여부가 불확실하거나, 예외가 났거나, 정산(settlement) 전에 크래시가 나면 조정(reconciliation)이 필요하며, 조용히 다시 디스패치하는 일은 없습니다. 상태는 `inspect(owner, logical_call_id(context, core_identity))`로 확인합니다. 그다음 `reconcile_success`는 provider 쪽 증거를 독립적으로 검증했고 Completion 결과 전체가 있을 때에만 호출합니다. 스트리밍 호출을 재생(replay)하면 캡처해 둔 결과를 그대로 반환하며 스트림 이벤트를 새로 만들어 내지 않습니다.

출력 상한을 더 크게 잡으면 전송 재시도나 같은 저널 슬롯의 재생(replay)이 아니라 새로운 의미의 호출이 됩니다. Interface 4는 이 상한을 네이티브 재생(replay) 설정에서만 제외하며, 준비된 요청의 다이제스트와 보수적으로 잡은 리소스 청구량(resource claim)에는 여전히 포함합니다. 승인(admission)된 호출마다 서로 다른 결정적 순번을 부여하고, 모든 시도의 결과와 회계 기록, 그리고 원래의 데드라인을 보존해야 합니다. 승인(admission)은 같은 리소스 bank에서 받아야 합니다. 이미 있는 슬롯에서 다이제스트가 바뀌면 거부됩니다. 네이티브 기록이나 커서(cursor)가 크레딧을 갱신하는 일은 없으며, 전달이 불확실했거나 observer·정산(settlement)이 실패한 뒤에 다시 보내도록 허가하는 일도 없습니다.

순서가 있는 메시지 파트, 원시 관찰값, null일 수 있는 사용량, attempt 메타데이터, 네이티브 continuation은 모두 저장된 결과에 포함됩니다. 네이티브 결과를 다루려면 호스트의 `sp::NativeArchive`를 `SQLiteProgramProviderCallJournal(database_path, native_archive)`에 넘겨야 합니다. 이식 가능한 JSON 투영(portable projection)으로는 이 권한을 다시 만들 수 없습니다. 정보가 손실된 예전 영수증은 업그레이드하거나 조용히 다시 디스패치하지 않고 거부합니다. 저널은 보수적으로 잡은 청구량과 확정된(committed) 토큰 양을 provider 보고서와 따로 보관합니다. 데이터베이스 경로는 파일 시스템의 영속 경로를 써야 합니다. 빈 경로, `:memory:`, `file:` URI 경로는 거부됩니다. 이 broker는 조립된 `ContextEpoch`가 아니라 Core에 이미 있는 ReAct 메시지 상태를 대상으로 동작하며, 같은 내장 호출에 엔진의 Strict Runtime 인터포지션과 함께 쓸 수 없습니다. 호스트가 작성한 네이티브 Provider 호출은 이 broker의 대상이 아닙니다.

## 네이티브, stdio, HTTP로 실행하는 필수 Hook

`MandatoryHookRunner`는 기존 네이티브 어댑터나 전송 방식에 독립적인 `HookExecutionBackend` 중 하나를 받습니다. `RpcHookExecutionAdapter`는 `HookRpcExecutor`를 그 백엔드에 연결합니다. 고정된 `hooks/invoke` JSON-RPC 메서드는 `StdioJsonRpcTransport`와 `HttpJsonRpcTransport` 어느 쪽으로도 쓸 수 있습니다. 둘 다 `MCPClient` 세션 위에서 동작하므로, 엔드포인트는 `hooks/invoke`도 제공하는 MCP 서버여야 합니다. 이 메서드는 MCP 표준이 아니라 NeoGraph의 확장입니다. 이 메서드가 없는 MCP 서버는 `-32601`로 응답합니다.

RPC Hook이 만든 아티팩트는 증거일 뿐 권한이 아닙니다. `ContextStoreHookArtifactPublisher`는 다음 조건을 모두 만족하는 아티팩트만 받습니다.

- 종류가 `HookOutput`이어야 합니다.
- `source_digest`가 그 Hook 호출의 ID와 정확히 같아야 합니다.
- 런타임 이벤트가 그 호출과 일치해야 합니다.

게시는 소유자 범위로 이루어지며 멱등입니다. 외부 효과는 성공했지만 그 아티팩트를 게시하지 못하면 Hook은 `ReconciliationRequired`로 마무리되고, 깨끗한 성공으로 보고되지 않습니다.

## 필수 컨텍스트와 변환

`RuntimeContextRequirements`는 필수 아티팩트 ID 전체와, 그중 `RequiredSkill` 아티팩트여야 하는 부분집합을 구분합니다. `HardConstraint`는 필수 아티팩트 전용 종류입니다. 필수 아티팩트는 모두 활성 에포크에서 선택되어야 하고 `required=true`를 유지해야 하며, 필수 토큰 수에 포함됩니다.

`ContextTransformReceipt`는 v1에서 일부러 보수적으로 설계했습니다. 변환기는 선택 증거를 교체하거나 압축할 수 있지만, 필수 입력 아티팩트의 ID는 모두 출력 집합에 바이트 단위로 똑같이 들어 있어야 합니다. 바꿔 쓴(paraphrase) 내용은 제약이 보존되었다는 증명으로 인정하지 않습니다.

## 런타임 개발자 지침

`RuntimeDeveloperInstruction`은 불변인 개발자 입력일 뿐 권한이 아닙니다. `RuntimeInstructionController::submit_and_plan`은 다음 순서로 동작합니다.

```text
append Developer-trust history record
  -> load the exact active Program lineage/generation
  -> call the host planner
  -> validate decision against the current lineage head
  -> require an exact already-admitted target for transition decisions
  -> persist the required decision artifact
```

결정은 다음 네 가지로 한정됩니다.

- `SatisfiedInPlace`
- `Rejected`
- `ReplaceAtHandoff`
- `MigrateGraph`

전환(transition)을 적용할 때는 기존 `ProgramRuntime::replace` 또는 `migrate_graph` 경로에 넘기기 직전에 계보(lineage) head를 다시 확인합니다. 오래된 결정은 권한이 될 수 없습니다.

## 제한된 Program 합성

`ProgramSynthesisGateway`는 호스트가 소유하는, 생성된 후속 Program의 경로를 제공합니다.

```text
immutable ProgramSynthesisProposal
  -> durable host reservation receipt
  -> bounded QuickJS compilation
  -> proposal capability/effect closure check
  -> host-owned semantic contract validation
  -> ordinary ProgramCatalog admission
  -> immutable ProgramSynthesisReceipt
```

예약은 갱신할 수 없는 `max_dynamic_compiles` 한 단위가 정확히 하나 줄어든 것으로 나타나야 하며, 다른 어떤 예산도 늘려서는 안 됩니다. 예약은 컴파일 전에 일어나므로, 거부된 소스도 컴파일 단위를 돌려받지 못합니다. 의미 검증은 필수이며 컴파일 뒤, 승인(admission) resolver 앞에서 실행됩니다. 의미 검증의 불변 영수증에는 제안, 예약, 컴파일된 번들, 검증기 식별자, 의미 계약 식별자, 판정, 증거 다이제스트가 묶입니다. 판정이 거부이면 타입이 있는 증거가 노출되고 `ProgramVersion`은 게시할 수 없습니다. 게이트웨이는 결과를 활성화하거나 바인딩하거나 마이그레이션하거나 스폰하지 않습니다. 이 동작은 기존 Program API를 통한 호스트의 별도 결정으로 남습니다.

런타임 지침 planner는 게이트웨이를 호출한 뒤, 승인(admission)된 정확한 버전을 교체 또는 마이그레이션 결정에 담아 반환할 수 있습니다. 이렇게 하면 다음 흐름이 유지됩니다.

```text
proposal -> reserve -> compile -> semantic validate -> admit -> decide -> migrate/spawn
```

생성된 JavaScript에는 컴파일러, Catalog, 자격 증명, 활성화 권한을 노출하지 않습니다.
