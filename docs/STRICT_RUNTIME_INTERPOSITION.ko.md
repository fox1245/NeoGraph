<!-- neograph-i18n: source=docs/STRICT_RUNTIME_INTERPOSITION.md locale=ko source_sha256=b039e7b57feafc338a8b6fea0cdbc8c0a492c3dbec6a269f986de85d1c629d09 -->
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

활성화할 수 있는 에포크는 `RuntimeGuaranteeProfile::Strict` 에포크뿐입니다. 프로파일을 `GraphEngine`에 연결하면 내장 소비자(consumer)에 provider 인터포지션과 라이프사이클 Hook이 모두 설치됩니다. 다만 provider 호출 사이에 컨텍스트를 대신 진행해 주지는 않습니다. [호스트 단계 진행 계약](#host-stepped-contract)을 참고하세요.

<a id="host-stepped-contract"></a>
## 호스트 단계 진행(host-stepped) 계약

엄격 경로는 provider 요청을 모두 활성 에포크로 만듭니다. 재료는 에포크의 고정된 RAW 범위와 선택된 아티팩트입니다. 그래프나 에이전트가 들고 있는 대화는 읽지 않습니다. 에포크 외에 프롬프트에 들어가는 것은 내장 노드가 채우는 호스트 지침 슬롯과 신뢰하는 보충 입력(trusted supplemental input) 슬롯뿐입니다. 그래서 호스트가 기록하기 전에는 NeoGraph가 모델 응답이나 도구 결과를 알 수 없습니다. 계약은 다음과 같습니다.

**호스트는 provider를 호출할 때마다 RAW 레코드를 추가하고 새 에포크를 활성화합니다.**

두 호출 사이에 호스트는 직전 턴(모델 응답, 이어서 도구 결과)을 `ContextStore`에 추가하고, 그 턴까지 RAW 범위에 담은 에포크를 만들어 `StrictRuntimeProfile::activate`로 활성화합니다. 첫 에포크도 사용자 메시지로 같은 방식으로 만듭니다. 아래 코드가 한 단계입니다. `head`는 `store.history_head(feed)`이고, `epoch_sequence`와 `previous_epoch_id`는 마지막 에포크에서 가져옵니다.

```cpp
for (auto& [message, trust] : turn) {  // the reply (ModelOutput), then the tool results (ToolOutput)
    RuntimeHistoryRecordData record;
    record.feed_id = feed.feed_id;
    record.sequence = ++head.sequence;
    record.message_id = "message-" + std::to_string(record.sequence);
    record.trust = trust;
    record.message = std::move(message);
    record.predecessor_id = head.record_id;
    const auto created = RuntimeHistoryRecord::create(std::move(record));
    if (store.append_history(feed, created, head.record_id) != ContextStoreAppendResult::Appended)
        throw std::runtime_error("the context feed moved");
    head.record_id = created.id();
}
ContextEpochData next;
next.run_id = run_id;
next.sequence = ++epoch_sequence;
next.predecessor_id = previous_epoch_id;
next.feed_id = feed.feed_id;
next.raw_from_sequence = 1;
next.raw_through_sequence = head.sequence;
next.raw_window_digest = store.snapshot_history(feed, 1, head.sequence).digest;
next.guarantee_profile = RuntimeGuaranteeProfile::Strict;
auto epoch = ContextEpoch::create(std::move(next));
previous_epoch_id = epoch.id();
profile.activate(owner_id, std::move(epoch));
```

NeoGraph에는 실행 도중에 에포크를 진행하는 도우미가 아직 없습니다.

**한 번의 실행 안에서 도는 루프.** 프로파일을 일반 `GraphEngine`에 연결해도 컨텍스트는 진행되지 않습니다. `engine.run()` 한 번 안에서 도구 루프(LLM 호출, 도구 디스패치, LLM 호출. `create_react_graph`가 만드는 구조)가 돌면 두 번째 LLM 호출도 같은 에포크로 실행됩니다. 도구 결과는 그래프 상태에는 있지만 에포크에는 없습니다. provider는 첫 턴의 컨텍스트를 다시 받고, 모델은 도구 결과를 끝내 보지 못하며, 단계 상한이나 예산에 걸려 실행이 멈출 때까지 같은 도구를 계속 요청할 수 있습니다.

그래서 NeoGraph는 이런 호출을 거부합니다. 그래프가 이미 진행시킨 턴의 에포크로 provider 호출이 들어오면 `std::logic_error`를 던집니다. 호출자가 제시한 메시지에 에포크의 첫 디스패치 이후 추가된 어시스턴트 메시지나 도구 메시지가 있고, 호출이 그 메시지를 호스트 지침이나 신뢰하는 보충 입력으로도 싣지 않았다면 턴이 진행된 것으로 봅니다. 오류 메시지는 이 계약을 밝힙니다.

```text
Strict provider dispatch is host-stepped: the host appends RAW records and activates a new epoch for every provider call. This call presents assistant or tool messages added since the active epoch's first dispatch, so the graph or agent loop advanced inside one run and the provider would not receive them. Append those messages to the context feed and activate a new epoch before this call.
```

그래프에서는 실패한 노드가 `NodeExecutionError`로 보고합니다. logic error는 재시도하지 않으므로 `attempts()`는 1입니다. `set_runtime_interposition`으로 컨트롤러를 설정한 `llm::Agent`도 두 번째 provider 호출에서 같은 방식으로 실패합니다.

다음은 거부하지 않습니다.

- 같은 턴의 재시도. 노드 재시도나 provider 실패 뒤의 반복 호출은 같은 메시지를 다시 제시합니다.
- 에포크의 첫 디스패치 때 이미 어시스턴트 메시지나 도구 메시지를 포함하던 대화. 그 메시지들이 기준선입니다.
- 사용자 메시지만 늘어난 경우. 검사하는 것은 어시스턴트 메시지와 도구 메시지입니다.
- 늘어난 대화를 신뢰하는 보충 입력으로 넘기는 내장 노드(예: plan-execute 그래프의 executor 노드). 응답과 도구 결과는 provider에 전달되고 조립 영수증에 묶입니다.

에포크를 활성화하거나 `clear()`를 호출하면 제시된 내용을 모두 잊습니다. 컨트롤러는 호출 지점(노드 이름)별로 첫 제시를 기억하며, 에포크마다 서로 다른 제시를 일정한 개수까지만 보관합니다. 그 한도를 넘는 제시는 추적하지 않으므로, 그 경우 검사는 최선 노력(best effort)입니다.

한 대화의 실행은 호스트가 직접 직렬화해야 합니다. 프로파일은 활성 에포크를 하나만 가지며 `thread_id`를 보지 않습니다. [한 대화의 실행](concurrency.md#runs-of-one-conversation)을 참고하세요.

## 컨텍스트 관리 방식 고르기

NeoGraph에는 모델 호출의 컨텍스트를 관리하는 방식이 네 가지 있습니다. 프롬프트를 무엇이 만드는지, 무엇을 보장하는지가 서로 다릅니다.

| 방식 | 프롬프트의 출처 | 보장하는 것 |
| --- | --- | --- |
| `messages` 채널 | 노드가 남긴 그대로의 그래프 상태 `messages` 채널. `LLMCallNode`가 이 채널을 보내고 응답을 덧붙이며 `ToolDispatchNode`가 도구 결과를 덧붙이므로, 한 번의 실행 안에서 도구 루프가 동작함 | 그래프 상태로 요청을 만든다는 점, 체크포인트 저장소가 그 상태를 저장할 수 있다는 점. 호출별 영수증, 필수 아티팩트, 컨텍스트 상한은 없음 |
| `history::compact_history` | 독립된 방식이 아니라 더 짧은 메시지 목록을 돌려주는 도우미. `estimate_tokens`가 한도를 넘으면 오래된 텍스트 전용 앞부분을 모델이 쓴 요약으로 바꾸고 최근 부분은 유지함. 결과는 다른 방식의 입력으로 사용. `RuntimeInterpositionController`를 받는 오버로드는 요약 호출을 그 컨트롤러로 보냄 | 히스토리를 유지했는지 바꿨는지와 그 이유를 알려 주는 상태. 네이티브 재생(replay) 봉인이 있는 히스토리는 그대로 반환. 요약은 모델 출력이므로 사실이나 제약이 요약에 남았다는 증거는 없음. 추정치는 휴리스틱. 에포크와 영수증은 없음 |
| 기록(Recorded) 프로파일 | `RuntimeInterpositionController`를 거치는 활성 에포크(`RuntimeGuaranteeProfile::Recorded`, 기본값). `ContextStore`의 RAW 범위와 에포크의 아티팩트로 구성. 호스트가 단계를 진행함 | 요청을 정확히 그 에포크로 조립해 조립 영수증에 묶고, 디스패치마다 영수증을 남김. 저장소가 영속적일 필요가 없고 입력 상한을 0으로 둘 수 있으므로, 증거를 얼마나 오래 보존할지와 상한을 둘지는 호스트가 정함 |
| 엄격(Strict) 프로파일 | 기록 프로파일과 같으며 `StrictRuntimeProfile`을 거침. 호스트가 단계를 진행함 | 기록 프로파일의 보장 전부에 더해, 영속적인 컨텍스트·디스패치 저장소, 호출마다 거는 필수 Hook, 에포크에 반드시 들어 있어야 하는 필수 아티팩트, 예산을 넘는 턴을 디스패치 전에 막는 0이 아닌 입력 상한. 모델이 컨텍스트에 주의를 기울였거나 따랐다는 보장은 없고, 에포크를 대신 진행해 주지도 않음 |

`messages` 채널에서 에포크로 옮기면 컨텍스트의 주인이 바뀝니다. 그래프 상태는 더 이상 프롬프트의 원천이 아니고, RAW 히스토리는 호스트가 보관합니다.

## 컨텍스트 예산

엄격 경로는 조립한 요청마다 입력 상한(`max_input_tokens`)과 비교하고, 예산을 넘는 턴은 디스패치 전에 `ContextBudgetBlocked`로 막습니다. 조립 영수증에는 사용한 추정치(`estimated_input_tokens`)와 그중 필수 아티팩트 텍스트의 몫(`mandatory_input_tokens`)이 기록됩니다.

추정치는 `RuntimeTurnAssembler::estimate_input_tokens`입니다. 인코딩된 요청 본문의 바이트 길이를 3으로 나눠 올림한 값입니다. 휴리스틱이며 증명된 상한이 아닙니다. provider의 토크나이저에서 나온 값이 아니므로 provider가 세는 수보다 작을 수도 클 수도 있습니다. 눈에 보이는 텍스트만이 아니라 인코딩된 요청 전체를 세며, 도구 스키마, 필드 이름, provider 프레이밍도 바이트에 들어갑니다. `max_input_tokens`는 지나치게 큰 요청을 막는 안전장치로 보고, provider가 보고하거나 청구하는 수에 대한 약속으로 보지 마세요. `mandatory_input_tokens`는 필수 아티팩트의 텍스트에만 같은 비율을 적용합니다.

디스패치 토큰 예약은 별개의 리소스 계약입니다. run에 모델 토큰 예산이 있으면 디스패치마다 모델의 입력 한도에 출력 허용량을 더한 만큼을 시도 가능한 횟수만큼 예약하며, 이 추정치는 쓰지 않습니다.

## provider 결과의 수명 주기

provider 경계에서는 서로 분리된 불변 값 두 개를 기록합니다.

1. `ProviderDispatchReceipt`는 디스패치 전에 기록합니다.
2. `ProviderDispatchOutcomeReceipt`는 시도가 끝난 뒤 `Succeeded`, `Failed`, `ReconciliationRequired` 중 하나를 기록합니다.

성공한 결과에는 SDK 결과 관찰 전체의 다이제스트가 묶입니다. 받아들여지지 않았음이 증명된 타입 Failure(요청이 전송되지 않았거나, 완료된 HTTP 4xx 응답이 거절한 경우)는 `Failed`로 기록하고, 전달 여부가 불확실하면 `ReconciliationRequired`로 기록합니다. 디스패치 이후에 예외가 발생해도 원격 provider가 실제로 동작했는지는 알 수 없으므로, 컨트롤러는 조용히 재시도하지 않습니다. SQLite 스키마 v3는 최종 영수증을 따로 저장하고, 재시작한 뒤에 승인(admission)된 디스패치 바인딩과 정확히 일치하는지 검사합니다. 영수증 다이제스트는 증거일 뿐이며, 네이티브 continuation의 보관(custody)이나 실행 가능한 저장 결과가 아닙니다.

컨트롤러는 `ProviderRequest`를 받아 소유권이 있는 불변 `sp::runtime::Result`를 반환합니다. 이 결과에는 순서가 있는 메시지/파트와 부분 실패 증거가 남습니다. 실제 결과를 받은 뒤 정산(settlement)이나 영수증 영속화에 실패하면 `ProviderDispatchOutcomePersistenceError`가 그 결과와 원래 원인을 보존하며, 이차로 발생한 observer 실패는 `delivery_error()`에 남습니다. 토큰 청구/예약은 null일 수 있는 provider 사용량 보고서와 별개로 관리합니다.

## Program Core provider 호출 (독립 실행 Strict Runtime과는 별개)

Program이 내장 Core LLM 노드를 쓰면, 호스트는 `RuntimeConfig::core_provider_call_resolver`를 설정하고 `require_core_provider_call_broker = true`로 지정할 수 있습니다. resolver는 `ProgramCoreProviderCallContext`마다 `<neograph/program/sqlite_provider_call_broker.h>`의 `SQLiteProgramProviderCallJournal::bind(context, deployment_identity)`를 반환해야 합니다. 이를 쓰려면 `neograph::program_sqlite`를 링크합니다. 배포 식별자(deployment identity)는 실제 provider 경로, 모델 배포, 권한(자격 증명 버전 포함)에 대해 호스트가 소유하는 SHA-256 식별자여야 합니다. broker는 이 값을 `Provider` 객체에서 알아내지 않습니다. 재시작하거나 재연결할 때는 같은 영속 데이터베이스를 다시 바인딩합니다. `RuntimeConfig::core_provider_calls_journaled = true`도 설정합니다. fork는 이 저널에서 provider 호출을 재생하며, 이 플래그가 있으면 이 호스트의 fork가 `ProgramHandle::notices()`에 `P_FORK_PROVIDER_CALLS_NOT_JOURNALED`를 알리지 않습니다. source의 같은 위치 호출을 찾았지만 인수할 수도, 달라졌음을 보일 수도 없는 fork는, `RuntimeConfig::fork_may_resend_provider_calls`가 true가 아니면 거부됩니다(레퍼런스 참조).

저널은 호출마다 소유자, 불변 Program 버전, run, operation, Core의 thread/task/node, 내장 호출 순번(ordinal)을 키로 사용하며, Program attempt는 키에 **포함하지 않습니다**. 키 옆에 호출의 요청 내용 다이제스트와 descriptor policy identity를 기록하고 호출을 요청 내용으로 매칭하므로(#543), 같은 요청은 SchemaProvider descriptor policy identity가 달라져도 같은 호출입니다. 마커는 전송 전에 SQLite FULL 동기화로 커밋합니다. 이 마커는 전송이 *일어났을 수도 있다*는 뜻일 뿐, provider가 요청을 받았다거나 효과가 정확히 한 번 일어났다는 뜻이 아닙니다. SDK의 Completion/Failure 결과는 불변 값 전체를 인코딩 버전 2로 저장하며, 정확히 바인딩된 재생(replay)에 쓰입니다. 받아들여지지 않았음이 증명된 Failure(전송되지 않았거나, 완료된 HTTP 4xx 응답이 거절한 경우)는 `Failed`로 기록합니다. 전달 여부가 불확실하거나, 예외가 났거나, 정산(settlement) 전에 크래시가 나면 조정(reconciliation)이 필요하며, 조용히 다시 디스패치하는 일은 없습니다. 상태는 `inspect(owner, logical_call_id(context, core_identity))`로 확인합니다. 그다음 `reconcile_success`는 provider 쪽 증거를 독립적으로 검증했고 Completion 결과 전체가 있을 때에만 호출합니다. 스트리밍 호출을 재생(replay)하면 캡처해 둔 결과를 그대로 반환하며 스트림 이벤트를 새로 만들어 내지 않습니다.

출력 상한을 더 크게 잡으면 전송 재시도나 같은 저널 슬롯의 재생(replay)이 아니라 새로운 의미의 호출이 됩니다. 네이티브 재생(replay) 설정은 이 상한을 제외하지만, 준비된 요청의 다이제스트와 보수적으로 잡은 리소스 청구량(resource claim)에는 여전히 포함합니다. 승인(admission)된 호출마다 서로 다른 결정적 순번을 부여하고, 모든 시도의 결과와 회계 기록, 그리고 원래의 데드라인을 보존해야 합니다. 승인(admission)은 같은 리소스 bank에서 받아야 합니다. 이미 있는 슬롯에서 요청 내용 다이제스트가 바뀌면 거부됩니다. 네이티브 기록이나 커서(cursor)가 크레딧을 갱신하는 일은 없으며, 전달이 불확실했거나 observer·정산(settlement)이 실패한 뒤에 다시 보내도록 허가하는 일도 없습니다.

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

## 아티팩트 배치

아티팩트의 `placement`는 그 텍스트가 RAW 메시지 사이 어디에 들어가는지를 정합니다. 어셈블러는 다음 순서로 프롬프트를 만듭니다. 같은 배치 안에서는 `priority`가 높은 쪽이 먼저이고, 그다음 `producer_id`, `id` 순입니다.

```text
host instructions
BeforeHistory artifacts
RAW messages before the latest user message
BeforeLatestUser artifacts
latest user message
AfterLatestUser artifacts
RAW messages after the latest user message
AfterHistory artifacts
trusted supplemental input
```

턴이 바뀌어도 그대로인 아티팩트는 `BeforeHistory`에 두세요. 기본 배치는 `BeforeLatestUser`이므로, 배치를 지정하지 않고 만든 아티팩트는 턴마다 움직입니다.

- `BeforeLatestUser`와 `AfterLatestUser`는 가장 최근 사용자 메시지에 고정됩니다. 새 사용자 메시지가 오면 아티팩트도 함께 움직이며, 프롬프트는 아티팩트의 이전 위치부터 이전 프롬프트와 달라집니다. 이 두 배치에는 RAW 윈도에 사용자 메시지가 있어야 하며, 없으면 조립이 실패합니다.
- `AfterHistory`는 항상 맨 끝입니다. 앞쪽 히스토리는 늘어나지만, 그 앞부분의 프롬프트는 바꾸지 않습니다.
- `BeforeHistory`는 호스트 지침 바로 뒤에 오며 히스토리와 무관합니다. 아티팩트 집합과 우선순위가 같으면 텍스트와 위치도 같습니다.

아티팩트 집합이 바뀌거나 RAW 윈도가 가장 오래된 레코드를 버리면, 그 지점부터 프롬프트가 달라집니다. 프롬프트의 앞부분을 캐시하는 provider는 첫 바이트부터 이전 요청과 같은 부분만 재사용할 수 있습니다. 배치가 캐시 적중에 미치는 영향은 NeoGraph에서 아직 측정하지 않았습니다.

`RequiredSkill`과 `HardConstraint` 아티팩트는 System 메시지로, 나머지 종류는 User 메시지로 렌더링됩니다. System 메시지가 어떻게 처리되는지는 provider 계열에 따라 다릅니다.

- `anthropic.messages`, `google.generate`, `google.interactions`에서는 요청 빌더가 모든 System 메시지를 배치와 상관없이 최상위 system 필드로 옮깁니다(빈 줄로 구분하며 조립된 순서대로). 그러면 Skill이나 제약의 배치는 대화 안의 위치가 아니라 system 텍스트들 사이의 순서만 정합니다.
- `openai.chat`과 `openai.responses`에서는 System 메시지가 배치가 정한 자리에 그대로 남습니다.

요청의 시작 부분을 안정적으로 유지하려면 System 역할 아티팩트의 텍스트를 턴마다 바꾸지 마세요. 여기까지는 레거시 레이아웃에 대한 설명이며, 에포크가 다른 레이아웃을 요청하지 않는 한 이것이 쓰입니다("안정 우선 레이아웃" 참조).

## 안정성 클래스, 순서, 프리픽스 선언

공급자의 프롬프트 캐시가 요청의 앞부분을 유지하는지는 호출 사이에 어떤 아티팩트가 그대로인지에 달려 있습니다. 데이터 모델은 이를 기록하고, 안정 우선 레이아웃(다음 절)이 이를 사용합니다.

- 아티팩트는 `StabilityClass`를 선언할 수 있습니다. `Stable`(실행 내내 같은 바이트), `Slow`(압축이나 그 밖에 선언된 프리픽스 단절 때에만 바뀜), `Volatile`(호출마다 바뀔 수 있음)입니다. 선언하지 않은 아티팩트는 읽을 때 클래스를 얻습니다. `RequiredSkill`과 `HardConstraint`는 `Stable`, 프로듀서가 `COMPACTION_PRODUCER_ID`인 `DerivedContext`는 `Slow`, 나머지는 모두 `Volatile`입니다. System 메시지로 렌더링되는 종류의 `Volatile` 아티팩트를 만들면 `ContextStabilityError`로 거부됩니다.
- 아티팩트는 `AnchorMode`를 선언할 수 있습니다. `Replace`(기본값) 또는 `KeepInPlace`입니다. 아직 이를 사용하는 것은 없습니다.
- 에포크는 아티팩트를 고정된 순서로 담은 `ordered_artifact_ids`와 `prefix_claim`을 기록할 수 있습니다. `prefix_claim`은 `Extends`(요청의 앞부분이 직전 에포크와 같고 끝에서만 늘어남) 또는 `break_reason`을 동반한 `Breaks`입니다. `Extends`를 선언한 에포크를 활성화하면 활성 에포크, 또는 호스트가 `activate`에 넘긴 직전 에포크에 대해 선언을 검사합니다. 같은 `raw_from_sequence`, 더 큰 `raw_through_sequence`, 같은 순서의 같은 `Stable`·`Slow` 아티팩트, 그리고 여전히 선택되어 있고 RAW 윈도 안에 있는 `KeepInPlace` 아티팩트가 조건입니다. 순서를 기록하지 않은 에포크(이 필드가 생기기 전에 저장된 것 등)는 이어 갈 수 없습니다.
- 어셈블러가 쓰는 영수증은 프롬프트 순서대로의 아티팩트, 그 클래스, 인코더 계열, 도구의 다이제스트, 에포크의 단절 사유를 기록합니다.

## 안정 우선 레이아웃

에포크는 `assembly_layout`으로 아티팩트를 어떻게 배치할지 정합니다. `Legacy`는 기본값이며, 이 필드가 생기기 전에 저장된 에포크가 가질 수 있는 유일한 레이아웃으로, "아티팩트 배치"에서 설명한 조립 방식입니다. `StableFirst`는 직접 선택해야 하고 `ordered_artifact_ids`가 필요하며, 프롬프트를 다음 순서로 만듭니다.

```text
host instructions
Stable artifacts, in the recorded order
Slow artifacts, in the recorded order
RAW messages
Volatile artifacts, in the recorded order
trusted supplemental input
```

- **에포크의 목록이 곧 순서입니다.** `order_context_artifacts`가 아티팩트로부터 이 목록을 만듭니다. 안정성 클래스, 그다음 높은 `priority`, 그다음 `producer_id`, 그다음 호스트가 주는 키 순입니다. 내용의 해시는 관여하지 않으므로 텍스트가 바뀌는 형제 아티팩트도 자리를 지키며, 네 가지가 모두 같은 두 아티팩트는 모호하다며 거부됩니다.
- **배치는 클래스를 따릅니다.** `Stable`과 `Slow` 아티팩트는 `BeforeHistory`에, `Volatile` 아티팩트는 `AfterHistory`에 두고, 목록은 `Stable`, `Slow`, `Volatile` 순으로 나열해야 합니다. 그 밖의 경우(예: 기본 배치 `BeforeLatestUser`로 만든 아티팩트)는 옮겨 주지 않고 `ContextStabilityError`로 거부됩니다.
- **System 텍스트는 앞에 머뭅니다.** System 메시지는 호스트 지침이거나 `Stable` 또는 `Slow` 아티팩트일 수만 있습니다. RAW나 보조 입력의 System 메시지는 `anthropic.messages`, `google.generate`, `google.interactions`에서 system 필드로 옮겨져 요청의 시작을 바꾸므로 거부됩니다. 옮겨진 텍스트는 `Stable`, `Slow` 순입니다.
- **레이아웃 변경은 단절입니다.** 연속한 두 에포크의 레이아웃이 다르려면 뒤의 에포크가 이유 `LayoutChange`와 함께 `Breaks`를 선언해야 합니다. 그렇지 않으면 `verify_context_epoch_claim`이 거부하며, 레이아웃이 바뀌는 지점을 가로지르는 `Extends` 선언도 거부합니다.
- **본문에서 읽은 두 경계.** 영수증은 `Stable` 부분(호스트 지침과 `Stable` 아티팩트)이 끝나는 곳, 추가만 되는 부분(`Volatile` 아티팩트 앞의 모든 것)이 끝나는 곳, 그리고 두 번째 경계까지의 요청 다이제스트를 기록합니다. 경계는 인코딩된 본문에서 프리픽스에 속하지 않는 첫 메시지와 파트이므로, System 텍스트를 옮기는 계열에서 `{0, 0}`은 도구와 system 필드만의 프리픽스입니다. 위치는 System 텍스트를 옮기고 도구 결과를 다시 묶은 뒤의 본문 기준입니다. 어셈블러는 프리픽스의 메시지와 그 뒤의 표식 메시지만으로 요청을 두 번째, 세 번째 인코딩해 위치를 찾으며, 인코딩된 본문들이 서로 맞지 않으면 `ContextStabilityError`로 에포크를 거부합니다. `Slow` System 텍스트가 `Stable` 텍스트와 system 필드를 함께 쓰면 필드가 한 블록이므로 `Stable` 경계는 기록되지 않습니다. 다이제스트는 스트림 플래그와 출력 상한을 뺀 본문의 모든 멤버를 담고, 메시지는 경계에서 잘립니다. 레거시 에포크에는 경계가 없습니다.

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
