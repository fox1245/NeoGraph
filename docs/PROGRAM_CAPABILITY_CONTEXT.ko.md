<!-- neograph-i18n: source=docs/PROGRAM_CAPABILITY_CONTEXT.md locale=ko source_sha256=05fbc110d09704e01a8f8dd64767d56b589b973a31fb3fbda55f9077f2686930 -->
# Program 노드의 capability와 중개되는 효과

**Languages:** [English](PROGRAM_CAPABILITY_CONTEXT.md) | [한국어](PROGRAM_CAPABILITY_CONTEXT.ko.md) | [日本語](PROGRAM_CAPABILITY_CONTEXT.ja.md) | [简体中文](PROGRAM_CAPABILITY_CONTEXT.zh-CN.md)

`ProgramCatalog`는 실행 가능한 전체 closure를 한 번에 바인딩하지만, 노드 팩토리에는 그 노드의 승인(admission)된 설정이 선언한 capability만 보여야 합니다. 그래서 `RegistrySnapshotBuilder`는 노드 매니페스트의 직접 실행 요구 사항과 설정별 요구 사항 resolver를 사용해 생성 시점의 `NodeContext`를 좁힙니다. 다른 노드가 쓰는 바인딩된 Provider나 Tool이 이 노드의 암묵적인 capability가 되지는 않습니다.

| 등록된 노드 | Provider 포인터 | Tool 포인터 | 안전한 메타데이터 |
| --- | --- | --- | --- |
| 임의의 `Brokered` C++ 팩토리 | 없음 | 없음 | 정확한 `provider_name`과 `tool_definitions` |
| 호스트가 검토한 `add_host_brokered_node` | 선언한 Provider 그대로 | 선언한 Tool 그대로 | 메타데이터 그대로 |
| 고정된 `add_core_llm_call` | 선언한 Provider 그대로 | 모델 정의용으로 선언한 Tool 그대로 | 메타데이터 그대로 |
| 고정된 `add_core_tool_dispatch` | 없음 | 선언한 Tool 그대로 | 메타데이터 그대로 |
| 고정된 `add_core_endpoint_node` | 선언한 Provider 그대로 | 없음 | 메타데이터 그대로 |
| `TrustedNative` C++ 팩토리 | 선언한 Provider 그대로 | 선언한 Tool 그대로 | 메타데이터 그대로 |

고정된 Core 등록은 NeoGraph 자체의 `LLMCallNode`, `ToolDispatchNode`, SchemaProvider 엔드포인트 노드(`add_core_endpoint_node`이며, 매니페스트가 노드 타입을 지정합니다: `embed`, `image_generate`, ..., `job_collect`, #531)를 만들며, 호출자가 제공한 팩토리는 받지 않습니다. 엔드포인트 노드의 호출은 `llm_call`의 호출처럼 Core provider 경로를 따릅니다. 호스트의 provider 호출 broker가 이를 저널에 기록하고 재생하며, 모델 토큰 예산은 출력 상한이 없으므로 이를 거부하고, 가격을 매기는 binding은 모델 카탈로그가 엔드포인트 호출의 상한을 정할 수 있을 때까지 `P_MONEY_UNMODELED`로 거부합니다. 고정된 Core 노드는 모두 정확하고 변경할 수 없는 요구 사항 resolver와 함께 등록해야 합니다. 모델이 작성한 토폴로지는 승인(admission)된 타입과 노드 설정을 고를 수는 있지만, 임의의 brokered 팩토리가 원시 Tool 포인터를 받게 만들 수는 없습니다. 표준 dispatch 노드는 바인딩된 Tool을 호출하기 전에 항상 `dispatch_tool_calls`를 거치며, 이 함수가 그 실행의 `ToolGate`와 `ToolExecutionController`를 확인합니다.

`TrustedNative`는 호스트가 보증(attest)하는 별도의 경계입니다. Program 승인(admission)은 이 효과 모드에 대해 이미 `TrustedEmbedding` 모드와, 일치하는 인증된 Catalog 호스트 식별자를 요구합니다. 네이티브 C++ 코드는 `NodeContext` 밖에서 캡처한 리소스를 계속 쥐고 있거나 자체 효과를 일으킬 수 있습니다. 여기서 설명하는 규칙은 capability를 줄이는 것(attenuation)일 뿐 프로세스 샌드박스가 아니며, 임의의 네이티브 코드가 순수하다는 증명도 아닙니다. 호스트는 네이티브 팩토리를 검토해 고정(pin)하고, 그 효과를 있는 그대로 선언해야 합니다.

Program의 Core 연산이 중개되는 Tool dispatch를 하려면 호스트가 소유한 `ProgramCoreToolGrant`가 필요합니다. 런타임은 소유자, Program 버전, run, 연산, 시도(attempt), **승인(admission)된 실행 가능 binding 지문(fingerprint)**, 비어 있지 않은 grant ID, gate, controller를 확인합니다. grant가 없거나 오래되었으면 다시 연결한 뒤에도 Tool 호출을 거부합니다. 거부된 Tool 호출은 하나의 Tool 결과이므로 Program 자체는 `Completed`로 끝날 수 있습니다. 요청한 작업이 성공했는지는 effect receipt를 확인해서 판단해야 합니다.

grant 식별자를 영속적으로 유지해야 하는 호스트는, run ID를 명시해 run을 시작하기 전에 `SQLiteProgramCoreToolGrantStore`에 정확한 `ProgramCoreToolGrantRecord`를 승인(admission)할 수 있습니다. binding 지문은 `capability_binding_receipt_root(version.core_materialization_receipt().capability_bindings)`입니다. `make_durable_core_tool_grant_resolver(store, policy_factory)`를 `RuntimeConfig.core_tool_grant_resolver`로 설치하면, 다시 연결할 때를 포함해 연산마다 레코드를 새로 읽고, grant ID, 소유자, 버전, run, 연산, 시도, binding이 모두 같은 새 호스트 정책만 받아들입니다. 승인(admission)은 활성 레코드가 완전히 같을 때만 멱등(idempotent)이며, ID나 binding이 충돌하는 레코드로는 바꿀 수 없습니다. 재개된 Program은 시도 횟수가 올라가므로, 호스트는 재개하기 전에 그 시도에 대해 아직 유효한 grant를 명시적으로 승인(admission)해야 합니다. 같은 grant ID가 여러 시도를 포괄할 수 있는 것은 소유자/버전/run/연산/binding이 모두 같은 경우뿐이며, grant를 취소(revoke)하면 승인된 시도가 모두 비활성화됩니다. 저장된 레코드에는 자격 증명, Tool 포인터, 콜백이 들어 있지 않습니다.

승인(admission)할지 여부와 Tool별 gate/controller 정책을 어떻게 다시 구성할지는 호스트만 결정합니다. 이 레코드는 권한의 증거일 뿐 effect 결과가 **아닙니다**. 호스트는 호출 단위 effect broker로 Tool dispatch, 완료, 결과가 불확실한 경우를 여전히 저널에 기록해야 합니다. 이는 외부 실행이 정확히 한 번만 일어난다는 약속이 아니며, 신뢰된 임의의 네이티브 코드가 중개되는 dispatch 밖에서 Tool을 호출하는 것을 막지도 않습니다.

`neograph::sqlite`의 `SQLiteToolEffectBroker`는 `dispatch_tool_calls`를 위한 영속 write-ahead dispatch를 구현합니다. 영속 SQLite 경로와, 호스트가 등록한 `{Tool*, executable_id}` binding으로 생성합니다. executable ID는 모델에게 보이는 Tool 이름만이 아니라 실제 코드, 설정, 원격 대상, 자격 증명 권한까지 고정(pin)해야 합니다. 이 broker는 Program grant에 담아 전달하거나, 독립 실행 Core에서는 `RunResources::tool_effect_broker`로 전달합니다. 독립 실행 Core에서는 이에 맞는 `tool_effect_grant`도 설정해야 합니다. `make_tool_execution_context`를 쓰는 내장 노드와 호스트가 등록한 노드는 공유 엔진을 바꾸지 않고도 이 호출 범위의 리소스를 물려받습니다. 독립 실행 `llm::Agent::run`/`run_stream`은 같은 broker, 안정적인 소유자/run/스레드 식별자, 연산, 호스트 grant를 담은 run별 `ToolExecutionContext`를 받습니다. 다시 연결할 때 대기 중인 batch를 재생(replay)하려면, 호출자가 네이티브/Tool 파트를 포함한 순서 있는 `sp::Message` 전체 기록을 보관해야 합니다. 표시용 `ChatMessage` 투영만으로는 네이티브 이어가기 권한을 다시 만들 수 없습니다. 네이티브 기록을 영속적으로 보관하려면 호스트의 `sp::NativeArchive`도 필요합니다. broker를 쓰지 않는 Agent/Core 경로는 보장 수준이 더 낮습니다.

broker는 Core 작업과 batch 순번마다 controller를 호출하기 전에 SQLite FULL-sync 마커를 커밋합니다. 마커를 커밋하지 못하면 Tool을 실행하지 않습니다. receipt에는 소유자/run/스레드/작업/순번, Program 버전/binding, 연산/grant, 등록된 정확한 executable, 정규화되어 다시 쓰인 인자가 묶입니다. 모델의 `ToolCall.id`는 상관 관계(correlation) 용도로만 쓰입니다. 확정된 receipt는 dispatch 없이 재생(replay)되고, 식별자/권한/인자가 충돌하면 거부됩니다. 마커가 pending 상태이거나, 타임아웃, 취소, dispatch 이후의 실행 실패, receipt 커밋 실패가 있으면 외부 조정(reconciliation)이 필요하며 재시작해도 재dispatch가 차단됩니다. 시도(attempt)의 변경은 출처 정보(provenance)로 기록할 뿐 새 호출 슬롯이 아닙니다. 외부 결과가 정확히 한 번만 일어난다고 주장해서는 안 됩니다. 영속 마커가 있더라도 충돌 이전에 외부 서비스가 커밋했는지는 알 수 없기 때문입니다.

brokered 커스텀 팩토리는 `NodeContext.provider`나 `NodeContext.tools`를 쓸 수 없습니다. 실행을 고정된 Core 노드로 옮기거나, 따로 검토한 호스트 broker를 쓰거나, 호스트의 신뢰된 임베딩 정책 아래 `TrustedNative`로 승인(admission)하세요. `add_host_brokered_node`는 Harness 워커가 쓰는, 네이티브 호스트 broker를 명시적으로 등록하는 경로입니다. 호출자가 제공한 팩토리가 이 경로로 안전해지는 것은 아닙니다. 호스트가 그 효과를 검토해 고정(pin)하고, 각 연산이 승인(admission)된 provider/tool closure 안에 머물도록 해야 합니다. 일반 `add_node`는 메타데이터만 받는 경계를 그대로 유지합니다. Program 밖의 직접 Core 그래프는 범위가 좁혀지지 않고 `NodeContext` 전체를 받습니다. 영속성과 네이티브 코드 경계는 [#291](https://github.com/fox1245/NeoGraph/issues/291), [#292](https://github.com/fox1245/NeoGraph/issues/292), [#293](https://github.com/fox1245/NeoGraph/issues/293)을 참고하세요.
