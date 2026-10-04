<!-- neograph-i18n: source=docs/PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md locale=ko source_sha256=7a6203d69da6c363bf3c4c5b6aa5b1b1fcd2f36c656c6f22bffc36cddeeb1e72 -->
# 영속적인 자식 합성

**Languages:** [English](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md) | [한국어](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.ko.md) | [日本語](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.ja.md) | [简体中文](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.zh-CN.md)

상태: 메모리 내 reference store, SQLite, PostgreSQL에 N2가 구현되었다. 단일 자식 시나리오의 N3 복구 검증은 [복구 matrix](PROGRAM_CHILD_SYNTHESIS_RECOVERY.md)에 설명한다. 호스트 진입점은 기존 top-level checkpoint와 기존 `ng.spawn` / `ng.await` child lifecycle을 사용한다. 이 경로는 범위가 제한되고 검토된 source를 위한 경로다. 모델 generator, template renderer, 새 DSL command를 구현하지 않는다.

## 호스트 통합

`RuntimeConfig::child_synthesis_gateway`와 `child_synthesis_grant_resolver`를 구성한다. resolver는 `(owner_scope, parent_run_id, grant_id)`를 사용해 신뢰된 호스트 policy에서 정확한 grant를 선택해야 한다. synthesis record에서 grant를 읽는다고 권한이 입증되지는 않는다.

1. 부모의 다음 top-level checkpoint에서 `ProgramHandoff`를 준비한다.
2. 부모 run, 활성 lineage, generation을 읽고 [권한 부여 계약](PROGRAM_CHILD_SYNTHESIS_CONTRACT.md)에 따라 검토된 source 인스턴스와 grant를 선택한다.
3. handoff를 유지한 채 `ProgramRuntime::prepare_child_synthesis(owner, parent, handoff, proposal, grant, binding_name)`을 호출한다.
4. 반환된 record가 `Bound`에 도달한 뒤 handoff를 해제한다.
5. 부모는 일반 `ng.spawn` / `ng.await` 경로에서 해당 binding 이름을 사용한다.

예를 들어 승인된 부모는 다음과 같이 yield할 수 있다.

```javascript
yield ng.checkpoint({request: "reviewed-child"}, "synthesis:request");
return yield ng.await(
  ng.spawn("generated-child", input.childInput, "generated:spawn"),
  5000,
  "generated:await"
);
```

checkpoint 요청을 검토된 source와 policy에 어떻게 연결할지는 호스트가 결정한다. checkpoint payload는 자신에게 compilation 또는 execution 권한을 부여할 수 없다. 영속 경로는 runtime transaction을 통해 예약을 소유하며 gateway의 독립 `reserve` 또는 `reserve_child` callback을 호출하지 않는다.

## 원자적 게시

`ProgramTransitionPublication::child_synthesis_records`는 부모의 run snapshot, journal head, lineage와 같은 transaction에서 불변 record 하나를 append한다. 첫 `Reserved` record는 정확한 이전 부모 snapshot, source lineage, 결과 lineage, dynamic compile 한 단위 차감을 바인딩해야 한다. 충돌하는 요청이 동일한 budget 전이를 재사용해 다른 binding의 비용을 충당할 수 없다. 정확히 같은 publication retry는 `AlreadyPresent`를 반환한다.

SQLite는 기존 `BEGIN IMMEDIATE` transaction을 사용한다. PostgreSQL은 기존 transaction과 owner advisory lock을 사용한다. 둘 다 synthesis log에 append하고 append 순서대로 검증된 request head를 재구성한다. 쓰기에 실패하면 부모, lineage, synthesis record가 함께 rollback된다. 메모리 내 reference는 strong exception guarantee를 유지한다.

synthesis record를 담은 publication은 storage schema 6을 사용한다. 일반 publication은 계속 schema 5로 직렬화되며 reader는 schema 1–5 지원을 유지한다. `load_child_syntheses(owner, parent_run)`은 현재 request head를 반환한다. 이 이력을 지원하지 않는 custom store에서는 base 구현이 안전하게 거부한다. dynamic budget을 가진 run을 복구하려면 이 읽기 기능이 필요하다.

record는 8 MiB와 16 revision으로 제한된다. proposal, 원래 호스트 grant와 부모 context, reservation, 누적 stage output을 유지한다. canonical 신원과 stage 검증은 이전 artifact 변경, 다른 generation, binding 이름 변경, 증거 수정을 거부한다. binding 이름은 한 parent run 안에서 고유하다. SQL log는 append-only다. 대규모 이력의 retention과 compaction은 후속 작업으로 남아 있다.

## 단계와 복구

| 영속 상태 | 복구 후 다음 동작 |
|---|---|
| `Reserved` | 작업 소유권을 획득하고 한 번 컴파일 |
| `Compiling` | `ReconciliationRequired`로 표시하고 불확실한 compile을 replay하지 않음 |
| `Compiled` | 저장된 bundle을 재사용하고 semantic validation 소유권 획득 |
| `Validating` | `ReconciliationRequired`로 표시하고 불확실한 validator를 다시 실행하지 않음 |
| `Validated` | 승인된 receipt와 증거를 재사용하고 admission policy 선택 |
| `Admitting` | 고정된 admission과 멱등적인 Catalog admission 재사용 |
| `Admitted` | 범위가 지정된 module receipt로 정확히 승인된 version 연결 |
| `Bound` | 부모의 일반 child command 재개 |
| `Dispatching` | 기록된 child ID와 input을 `start_child`를 통해 재사용 |
| `Spawned` | child/result 재사용 또는 영속 checkpoint 없는 Core dispatch reconciliation |
| `Failed` / `ReconciliationRequired` | 결과를 유지하고 자동 parent replay 차단 |

미완료 synthesis가 있는 비활성 부모에 reconnect하기 전에 `recover_child_synthesis(owner, parent_run, proposal_id)`를 호출한다. 살아 있는 부모에는 유지 중인 checkpoint API가 필요하다. 모든 복구는 호스트 권한을 다시 선택하고 원래 generation을 확인한다. 완료된 semantic validation과 고정된 admission은 재사용한다. semantic 거부는 receipt와 증거를 유지하며 admission에 도달하지 않는다.

생성된 binding은 static resolver의 parent-version 범위가 아니라 실제 parent run과 generation으로 해석한다. dispatch는 `start_child` 전에 안정적인 child ID와 input을 기록한다. 하나의 grant로 두 번째 invocation을 만들 수 없다. 일반 child budget 처리, publication, execution, join이 계속 기준이 된다.

부모는 완료, retry, 복구 시 compile 차감을 유지하며 commit acknowledgement를 잃어도 마찬가지다. synthesis는 경과한 wall time을 차감한다. 복구된 attempt도 원래 synthesis deadline을 유지한다. spawn 상태 publication은 기존 command의 진행 중 예약을 보존한다.

## 검증과 남은 작업

backend conformance test는 성공적인 spawn/join, canonical round trip, owner/run 격리, 중복 및 동시 요청, backend 쓰기 실패 시 rollback, commit된 stage 재사용, semantic 거부, 철회된 grant, 모호한 compile/validation claim을 다룬다. Linux process-exit test는 admission을 고정한 후 destructor 없이 종료하고 SQLite와 PostgreSQL 모두에서 Program store, transition store, checkpoint store를 다시 연 뒤 자식을 성공적으로 join한다.

[N3 복구 matrix](PROGRAM_CHILD_SYNTHESIS_RECOVERY.md)는 database마다 16개 process-exit 경계, 동시 process 복구, acknowledgement 상실, 실제 PostgreSQL connection 종료, cancellation, version retention, activation, Program replacement까지 검증을 확장한다. 그 한도는 명시적으로 유지된다. 자동 reconciliation authorizer는 없으며, 일반 mixed-effect join, power loss, cross-host failover, 모든 graph-migration/retention 조합은 단일 자식 시나리오로 검증되지 않는다.

이 변경은 공개 C++ type, virtual method, configuration field를 추가한다. 모든 Program consumer를 다시 빌드하고 오래된 object와 새 library를 혼용하지 않는다. Core-only dependency나 native control C ABI 변경은 추가하지 않는다.
