<!-- neograph-i18n: source=docs/PROGRAM_CAPABILITY_CONTEXT.md locale=ko source_sha256=19977a5adc5fc8fac5dafb2f604519c6974ccafa509aa01ccafd393cf0c7c962 -->
# Program 노드의 capability와 중개된 효과

**Languages:** [English](PROGRAM_CAPABILITY_CONTEXT.md) | [한국어](PROGRAM_CAPABILITY_CONTEXT.ko.md) | [日本語](PROGRAM_CAPABILITY_CONTEXT.ja.md) | [简体中文](PROGRAM_CAPABILITY_CONTEXT.zh-CN.md)

`ProgramCatalog`는 전체 실행 가능 코드의 폐쇄 집합을 한 번 바인딩하지만, 노드 팩터리에는 해당 노드의 승인된 구성에 선언된 capability만 보여야 한다. 따라서 `RegistrySnapshotBuilder`는 노드 manifest의 직접 실행 요구사항과 구성별 요구사항 resolver를 사용해 생성 시의 `NodeContext`를 좁힌다. 다른 노드가 사용하는 바인딩된 Provider나 Tool이 이 노드의 암묵적인 capability가 되지는 않는다.

| 등록된 노드 | Provider 포인터 | Tool 포인터 | 안전한 메타데이터 |
| --- | --- | --- | --- |
| 임의의 `Brokered` C++ 팩터리 | 없음 | 없음 | 정확한 `provider_name`과 `tool_definitions` |
| 호스트가 검토한 `add_host_brokered_node` | 정확히 선언된 Provider | 정확히 선언된 Tool | 정확한 메타데이터 |
| 고정된 `add_core_llm_call` | 정확히 선언된 Provider | 모델 정의를 위해 정확히 선언된 Tool | 정확한 메타데이터 |
| 고정된 `add_core_tool_dispatch` | 없음 | 정확히 선언된 Tool | 정확한 메타데이터 |
| `TrustedNative` C++ 팩터리 | 정확히 선언된 Provider | 정확히 선언된 Tool | 정확한 메타데이터 |

두 고정 Core 등록은 NeoGraph 자체의 `LLMCallNode`와 `ToolDispatchNode`를 생성하며 호출자가 제공하는 팩터리를 받지 않는다. 정확하고 불변인 요구사항 resolver로 등록한다. 모델이 작성한 토폴로지는 승인된 타입과 노드 구성을 선택할 수 있지만, 임의의 brokered 팩터리에 raw Tool 포인터를 전달하게 만들 수는 없다. 표준 dispatch 노드는 여전히 `dispatch_tool_calls`를 호출하며, 이 함수는 바인딩된 Tool을 호출하기 전에 해당 실행의 `ToolGate`와 `ToolExecutionController`를 확인한다.

`TrustedNative`는 호스트가 증명하는 별도 경계다. Program 승인은 이미 이 effect 모드에 대해 `TrustedEmbedding` 모드와 인증된 Catalog 호스트 신원의 일치를 요구한다. 네이티브 C++ 코드는 `NodeContext` 밖에서 캡처한 자원을 유지하거나 자체 효과를 수행할 수 있다. 이 규칙은 capability를 제한하는 규칙이지, 프로세스 샌드박스나 임의의 네이티브 코드가 순수하다는 증명이 아니다. 호스트는 네이티브 팩터리를 검토하고 고정하며 그 효과를 정확히 선언해야 한다.

Program Core 연산에서 중개된 Tool dispatch를 수행하려면 호스트 소유의 `ProgramCoreToolGrant`가 필요하다. 런타임은 owner, Program version, run, operation, attempt, **승인된 실행 바인딩 fingerprint**, 비어 있지 않은 grant ID, gate, controller를 검사한다. grant가 없거나 오래되었으면 reconnect 이후를 포함해 Tool 호출을 거부한다. 거부된 Tool 호출도 Tool 결과이므로 Program 자체는 `Completed`로 끝날 수 있다. 요청한 작업의 성공 여부를 판단할 때는 effect receipt를 확인한다.

지속되는 grant 신원이 필요한 호스트는 명시적인 run ID로 실행을 시작하기 전에 `SQLiteProgramCoreToolGrantStore`에 정확한 `ProgramCoreToolGrantRecord`를 승인할 수 있다. 바인딩 fingerprint는 `capability_binding_receipt_root(version.core_materialization_receipt().capability_bindings)`다. `RuntimeConfig.core_tool_grant_resolver`에 `make_durable_core_tool_grant_resolver(store, policy_factory)`를 설치한다. 이 resolver는 reconnect를 포함해 매 연산마다 레코드를 다시 읽고, grant ID, owner, version, run, operation, attempt, binding이 동일한 새 호스트 정책만 받아들인다. 승인은 활성 레코드가 완전히 같을 때만 멱등적이다. 충돌하는 ID나 바인딩으로 기존 레코드를 대체할 수 없다. 재개된 Program은 attempt가 증가하므로, 호스트는 재개 전에 해당 attempt에 대해 여전히 권한이 있는 grant를 명시적으로 승인해야 한다. 동일한 grant ID는 같은 owner/version/run/operation/binding 안에서만 여러 attempt를 포괄할 수 있다. 철회하면 그 ID의 승인된 모든 attempt가 비활성화된다. 저장 레코드에는 credential, Tool 포인터, callback이 없다.

승인 여부와 Tool별 gate/controller 정책을 재구성하는 방법은 호스트만 결정한다. 레코드는 권한의 증거이며 effect 결과가 **아니다**. 호스트는 여전히 호출별 effect broker를 통해 각각의 Tool dispatch, 완료, 불확실한 결과를 journal에 기록해야 한다. 이는 외부 실행의 exactly-once를 보장하지 않으며, 중개된 dispatch 밖에서 Tool을 호출하는 임의의 신뢰된 네이티브 코드를 제한하지 않는다.

`neograph::sqlite`의 `SQLiteToolEffectBroker`는 `dispatch_tool_calls`에 대해 영속적인 write-ahead dispatch를 구현한다. 영속 SQLite 경로와 호스트가 등록한 `{Tool*, executable_id}` 바인딩으로 생성한다. executable ID는 모델에 보이는 Tool 이름만이 아니라 실제 코드, 구성, 원격 목적지, credential 권한을 고정해야 한다. Program grant 또는 독립 Core의 `RunResources::tool_effect_broker`에 broker를 전달하고, 독립 Core에는 일치하는 `tool_effect_grant`도 설정한다. `make_tool_execution_context`를 사용하는 내장 노드와 호스트 등록 노드는 공유 엔진을 변경하지 않고 호출 범위의 이 자원을 상속한다. 독립적인 `llm::Agent::run`/`run_stream`은 동일한 broker, 안정적인 owner/run/thread 신원, operation, 호스트 grant를 가진 실행별 `ToolExecutionContext`를 받는다. reconnect 시 대기 중인 batch를 replay하려면 호출자는 native/tool 부분을 포함한 전체 순서의 `sp::Message` 이력을 보존해야 한다. 표시 전용 `ChatMessage` 투영으로는 native continuation 권한을 재구성할 수 없다. 영속적인 native custody에는 호스트의 `sp::NativeArchive`도 필요하다. 기존의 broker 없는 Agent/Core 경로는 명시적으로 더 낮은 보장 수준을 유지한다.

각 Core task와 batch ordinal에 대해 broker는 controller를 호출하기 전에 SQLite FULL-sync marker를 commit한다. marker에 실패하면 Tool을 실행하지 않는다. receipt는 owner/run/thread/task/ordinal, Program version/binding, operation/grant, 정확히 등록된 executable, canonical 형태로 재작성된 인자를 바인딩한다. 모델의 `ToolCall.id`는 상관관계 식별용일 뿐이다. 확인된 receipt는 dispatch 없이 replay하며, 신원/권한/인자가 충돌하면 거부한다. 대기 중인 marker, timeout, cancellation, dispatch 후 실행 실패, receipt commit 실패는 외부 reconciliation이 필요하며 restart 이후에도 재dispatch를 차단한다. attempt 변경은 새 호출 슬롯이 아니라 provenance로 기록한다. 외부 결과의 exactly-once를 주장해서는 안 된다. 영속 marker라도 crash 전에 외부 서비스가 commit했는지는 판단할 수 없다.

`NodeContext.provider`나 `NodeContext.tools`를 사용하던 기존 brokered 사용자 정의 팩터리는 실행을 고정 Core 노드로 옮기거나, 별도로 검토한 호스트 broker를 쓰거나, 호스트의 trusted embedding 정책에 따라 `TrustedNative`로 승인받아야 한다. `add_host_brokered_node`는 Harness worker가 사용하는 명시적인 네이티브 호스트 broker 등록 경로다. 이 경로가 호출자 제공 팩터리를 안전하게 만들지는 않는다. 호스트는 그 효과를 검토하고 고정하며 각 연산을 승인된 provider/tool 폐쇄 집합 안에 유지해야 한다. 일반 `add_node`는 메타데이터 전용 경계를 유지한다. Program 밖의 직접 Core 그래프는 기존 `NodeContext` 동작을 유지한다. 남아 있는 영속성 및 네이티브 코드 경계는 [#291](https://github.com/fox1245/NeoGraph/issues/291), [#292](https://github.com/fox1245/NeoGraph/issues/292), [#293](https://github.com/fox1245/NeoGraph/issues/293)을 참조한다.
