<!-- neograph-i18n: source=docs/PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md locale=ko source_sha256=14283f61af8f5c3ec52011a184bc863369346836057023301f372ff29340a1ab -->
# 영속 자식 합성

**Languages:** [English](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md) | [한국어](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.ko.md) | [日本語](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.ja.md) | [简体中文](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.zh-CN.md)

자식 합성의 영속화는 인메모리 레퍼런스 저장소, SQLite, PostgreSQL에서 사용할 수 있습니다.
[복구 매트릭스](PROGRAM_CHILD_SYNTHESIS_RECOVERY.md)에는 단일 자식 시나리오에서 테스트한 복구 동작이 나와 있습니다.
호스트 진입점은 기존의 최상위 체크포인트와 기존 `ng.spawn` / `ng.await` 자식 생명주기를 사용합니다. 이 경로는 검토를 마친 소스만 다루는 제한된 경로이며, 모델 생성기, 템플릿 렌더러, 새 DSL 명령은 구현하지 않습니다.

## 호스트 통합

`RuntimeConfig::child_synthesis_gateway`와 `child_synthesis_grant_resolver`를 설정합니다. resolver는 `(owner_scope, parent_run_id, grant_id)`로 신뢰된 호스트 정책에서 정확한 grant를 골라야 합니다. 합성 레코드에서 grant를 읽어 왔다고 해서 그 grant에 권한이 생기는 것은 아닙니다.

1. 부모의 다음 최상위 체크포인트에 `ProgramHandoff`를 준비합니다.
2. 부모 run, 활성 계보, 세대를 읽어 오고, [인가 계약](PROGRAM_CHILD_SYNTHESIS_CONTRACT.md)에 설명한 대로 검토를 마친 소스 인스턴스와 grant를 고릅니다.
3. handoff를 쥔 채로 `ProgramRuntime::prepare_child_synthesis(owner, parent, handoff,
   proposal, grant, binding_name)`를 호출합니다.
4. 반환된 레코드가 `Bound`에 도달하면 handoff를 해제합니다.
5. 부모는 그 binding 이름을 일반적인 `ng.spawn` / `ng.await` 경로에서 사용합니다.

예를 들어 승인(admission)된 부모는 다음과 같이 yield할 수 있습니다.

```javascript
yield ng.checkpoint({request: "reviewed-child"}, "synthesis:request");
return yield ng.await(
  ng.spawn("generated-child", input.childInput, "generated:spawn"),
  5000,
  "generated:await"
);
```

체크포인트 요청을 검토된 소스와 정책에 어떻게 대응시킬지는 호스트가 정합니다. 체크포인트 payload가 스스로 컴파일이나 실행 권한을 부여할 수는 없습니다. 영속 경로에서는 런타임 트랜잭션이 예약을 직접 맡으므로, 게이트웨이의 독립 `reserve`나 `reserve_child` 콜백은 호출하지 않습니다.

## 원자적 게시

`ProgramTransitionPublication::child_synthesis_records`는 부모의 run 스냅샷, 저널 head, 계보와 같은 트랜잭션에서 변경할 수 없는 레코드 하나를 추가합니다. 첫 `Reserved` 레코드는 직전 부모 스냅샷, 원본 계보, 결과 계보, 그리고 동적 컴파일 1건 차감에 정확히 묶여 있어야 합니다. 충돌하는 요청이 똑같은 예산 전이를 재사용해 다른 binding의 비용을 치르게 할 수는 없습니다. 똑같은 게시를 다시 시도하면 `AlreadyPresent`를 반환합니다.

SQLite는 기존의 `BEGIN IMMEDIATE` 트랜잭션을, PostgreSQL은 기존 트랜잭션과 소유자 advisory lock을 사용합니다. 두 백엔드 모두 합성 로그에 레코드를 추가하고, 추가된 순서대로 검증된 요청 head를 재구성합니다. 쓰기가 실패하면 부모, 계보, 합성 레코드가 함께 롤백됩니다. 인메모리 레퍼런스는 강한 예외 보장(strong exception guarantee)을 유지합니다.

합성 레코드를 담은 게시는 저장소 스키마 6을 씁니다. 일반 게시는 계속 스키마 5로 직렬화되며, 읽는 쪽은 스키마 1–5를 계속 지원합니다. `load_child_syntheses(owner, parent_run)`는 현재의 요청 head를 반환합니다. 이 이력을 지원하지 않는 커스텀 저장소에서는 기본 구현이 실패 시 차단으로 동작합니다. 동적 예산이 있는 run을 복구하려면 이 읽기 기능이 필요합니다.

레코드는 8 MiB, 리비전 16개로 제한됩니다. 레코드에는 제안, 원래의 호스트 grant와 부모 컨텍스트, 예약, 누적된 단계별 출력이 남습니다. 정규 식별자와 단계 검증은 앞선 산출물이 바뀌었거나, 세대가 다르거나, binding 이름이 바뀌었거나, 증거가 수정된 경우를 거부합니다. binding 이름은 부모 run 하나 안에서 고유해야 합니다. SQL 로그는 추가만 가능(append-only)하며, 큰 이력을 보존하거나 압축(compaction)하는 기능은 없습니다.

## 단계와 복구

| 영속 상태 | 복구 후 다음 동작 |
|---|---|
| `Reserved` | claim한 뒤 한 번만 컴파일한다 |
| `Compiling` | `ReconciliationRequired`로 표시한다. 결과가 불확실한 컴파일은 재생(replay)하지 않는다 |
| `Compiled` | 저장된 번들을 재사용하고 의미 검증을 claim한다 |
| `Validating` | `ReconciliationRequired`로 표시한다. 결과가 불확실한 검증기는 다시 실행하지 않는다 |
| `Validated` | 받아들여진 receipt와 증거를 재사용하고 승인(admission) 정책을 고른다 |
| `Admitting` | 고정된(frozen) 승인(admission)을 재사용하고 멱등한 Catalog 승인(admission)을 수행한다 |
| `Admitted` | 범위가 한정된 모듈 receipt로 승인된 버전을 정확히 연결(link)한다 |
| `Bound` | 부모의 일반 자식 명령을 재개한다 |
| `Dispatching` | 기록된 자식 ID와 input을 `start_child`에 다시 사용한다 |
| `Spawned` | 자식/결과를 재사용하거나, 영속 체크포인트가 없는 Core dispatch를 조정(reconcile)한다 |
| `Failed` / `ReconciliationRequired` | 결과를 유지하고 부모의 자동 재생(replay)을 막는다 |

합성이 끝나지 않은 비활성 부모를 다시 연결하기 전에 `recover_child_synthesis(owner, parent_run, proposal_id)`를 호출합니다. 실행 중인 부모에는 보류 체크포인트(held checkpoint) API가 필요합니다. 복구할 때마다 호스트 권한을 다시 선택하고 원래 세대를 확인합니다. 완료된 의미 검증과 고정된(frozen) 승인(admission)은 재사용됩니다. 의미 검증에서 거부된 경우에도 receipt와 증거는 남으며, 승인(admission) 단계에는 도달하지 않습니다.

생성된 binding은 정적 resolver의 부모 버전 범위가 아니라 실제 부모 run과 세대를 기준으로 해석됩니다. dispatch는 `start_child`를 호출하기 전에 고정된 자식 ID와 input을 기록하므로, grant 하나로 invocation을 두 번 만들 수 없습니다. 자식의 일반적인 예산 처리, 게시, 실행, join이 계속 최종 권위를 갖습니다.

완료, 재시도, 복구 시에도, 커밋 확인 응답을 잃은 경우까지 포함해 부모는 컴파일 차감분을 그대로 유지합니다. 합성은 경과한 벽시계 시간을 차감하며, 복구된 시도도 원래 합성의 데드라인을 유지합니다. spawn 상태를 게시할 때는 기존 명령의 진행 중인 예약이 보존됩니다.

## 테스트 범위와 한계

백엔드 적합성 테스트는 spawn/join 성공, 정규 직렬화 왕복, 소유자/run 격리, 중복 요청과 동시 요청, 백엔드 쓰기 실패 시 롤백, 커밋된 단계의 재사용, 의미 검증 거부, 취소(revoke)된 grant, 결과가 모호한 컴파일/검증 claim을 다룹니다. Linux 프로세스 종료 테스트는 고정된 승인(admission) 직후 소멸자를 실행하지 않고 프로세스를 종료한 다음, SQLite와 PostgreSQL 양쪽에서 Program 저장소, 전이 저장소, 체크포인트 저장소를 다시 열고 자식을 정상적으로 join합니다.

[복구 매트릭스](PROGRAM_CHILD_SYNTHESIS_RECOVERY.md)는 검증 범위를 데이터베이스마다 16개의 프로세스 종료 지점, 동시 프로세스 복구, 확인 응답 유실, 실제 PostgreSQL 연결 종료, 취소, 버전 보존, 활성화, Program 교체까지 넓힙니다. 한계도 분명합니다. 자동 조정(reconciliation) 인가기는 없고, 여러 종류의 효과가 섞인 일반적인 join, 정전(power loss), 호스트 간 장애 조치(failover), 그래프 마이그레이션/보존의 모든 조합은 단일 자식 시나리오가 다루지 않습니다.
