<!-- neograph-i18n: source=docs/PROGRAM_CHILD_SYNTHESIS_RECOVERY.md locale=ko source_sha256=6ed07e73d44459a4ff1885cb7f07064be6994987113c36f0c7bae8c9c79d76ed -->
# 자식 합성 복구 검증

**Languages:** [English](PROGRAM_CHILD_SYNTHESIS_RECOVERY.md) | [한국어](PROGRAM_CHILD_SYNTHESIS_RECOVERY.ko.md) | [日本語](PROGRAM_CHILD_SYNTHESIS_RECOVERY.ja.md) | [简体中文](PROGRAM_CHILD_SYNTHESIS_RECOVERY.zh-CN.md)

N3는 SQLite와 PostgreSQL에서 검토된 source를 사용하는 단일 자식 checkpoint 시나리오를 검증한다. 복구는 이제 기록된 child operation과 분류되지 않은 외부 effect를 구분하며, Core dispatch에 영속 checkpoint나 result가 없으면 명시적인 reconciliation을 계속 요구한다.

## 복구 규칙

복구에서 dispatch를 검토하기 전에 부모 command journal이 승인된 Program, command ordinal, payload, effect 신원과 일치해야 한다. `spawn` 또는 합성된 `spawn`으로 끝나는 `await` chain만 이 복구 경로를 사용할 수 있다. 호스트는 grant, 원래 generation, 실제 compile 차감, 생성된 binding을 재검증한다. 이미 기록된 dispatch도 정확한 child run ID와 canonical input에 일치해야 한다.

복구는 command의 기존 resource reservation과 operation 차감을 재사용한다. 같은 command에 다시 비용을 부과하거나 dynamic compilation을 하나 더 부여하지 않는다. child reservation 재구성에는 대기 중인 command가 보유한 resource가 포함된다. 이 재구성은 process-local accounting view를 바꾸는 것이지 영속 budget을 바꾸는 것이 아니다. 기존 child record와 terminal result가 계속 기준이 된다.

동일한 `Compiling` 또는 `Validating` publication이 이미 존재한다는 사실은 새 호출자가 그 작업을 소유한다는 증거가 아니다. `Published`를 받은 writer만 claim한 stage를 시작할 수 있다. 동시 cold recovery test는 이 publication 전에 독립 process 두 개를 동기화하고, 성공한 worker 하나, conflict 하나, semantic validation 한 번을 요구한다.

## Process-exit matrix

각 case는 선택한 publication 이후 destructor 없이 process를 종료한 다음 새 Catalog, transition, checkpoint store를 연다. 두 database가 같은 case를 실행한다.

| 중단 지점 | 요구되는 복구 |
|---|---|
| `Reserved` | 기존 reservation으로 한 번 컴파일 |
| `Compiling` | 차감을 보존하고 reconciliation 요구 |
| `Compiled` | bundle 재사용 |
| `Validating` | 차감을 보존하고 reconciliation 요구 |
| `Validated` | semantic 증거 재사용 |
| `Admitting` | 고정된 admission 재사용 |
| `Admitted` | 승인된 version 바인딩 |
| `Bound` | 기록된 parent command 실행 |
| `Dispatching` | child 신원과 input 재사용 |
| `Spawned` | 기존 child 복구 또는 불확실한 Core dispatch에 명시적 reconciliation |
| Child relation `Publishing` | 기존 child의 최초 publication 완료 |
| Child relation `Dispatched` | replay-safe가 아닌 Core 작업에 checkpoint/result가 없으면 reconciliation |
| Child terminal result | result 재사용; 완료된 child를 다시 실행하지 않음 |
| Parent child-result attachment | 기존 join result 재사용 |
| Parent command result | 기록된 command result replay |
| Parent terminal result | 동일한 terminal result 신원 반환 |

database마다 process-exit 경계 16개와 두 process 간 복구 race 하나를 다룬다. test는 원래 child 신원, 영속 child relation 하나, 단일 compile 차감, 유지된 command-operation 수도 검증한다. execution marker로 process exit 이후 완료된 child 작업이 반복되지 않음을 확인한다.

## 불확실한 자식 실행

`Dispatched` relation만으로는 process가 사라지기 전에 Core 작업이 실행되었는지 확정할 수 없다. child가 여전히 running으로 기록되어 있고 정확한 checkpoint가 없으며 그 plan이 checkpoint 없이 replay하기에 안전하지 않으면 parent reconnect는 안전하게 거부한다. `recover_child_synthesis`는 child ID와 input을 유지하면서 `P_CHILD_SYNTHESIS_CHILD_UNCERTAIN`을 가진 `ReconciliationRequired`를 기록한다.

이 inactive-parent 복구 검사는 살아 있는 부모에 reconnect하는 경우를 lost execution으로 잘못 분류하지 않는다. live reconnect는 기존 attempt를 반환한다.

불확실성 처리는 synthesis record가 이미 `Spawned`라고 표시해도 적용된다. record는 이전 artifact를 바꾸지 않고 reconciliation 처리를 append할 수 있다. 대체 child를 발급하거나 성공 result를 만들어 내지 않는다. 이 변경은 자동 reconciliation 승인 API를 제공하지 않는다.

## 추가 검사

- reservation acknowledgement를 잃은 뒤 synthesis history를 읽을 수 없더라도 compile 단위를 환급할 수 없다. 읽기가 복구되면 원래 요청을 재사용한다.
- PostgreSQL test는 reservation transaction 안에서 database connection을 종료한다. 새 connection은 원래 부모와 부분 reservation이 없음을 확인하며, 다음 attempt는 한 번만 차감한다.
- semantic validation 중 cancellation이 발생하면 admission이나 child dispatch에 도달할 수 없다.
- Catalog activation은 생성된 binding을 다른 version으로 돌릴 수 없다.
- retention pin은 필요한 version을 보존한다. 호스트가 child version을 제거하면 캐시된 synthesis 증거로 실행할 수 없다. 모든 synthesis history에서 retention root를 자동 수집하는 일은 계속 호스트 책임이다.
- Program replacement는 compile 차감을 유지하며 오래된 run의 연결되지 않은 generated binding을 successor generation으로 넘기지 않는다. 연결된 descendant는 이제 [재귀 Harness 계약](PROGRAM_RECURSIVE_HARNESSES.md)을 통해 유지할 수 있다.
- PostgreSQL test는 process-exit matrix와 connection-termination case를 포함해 기존 CTest database resource lock을 공유한다.

## 이 검증의 한도

이 검증은 명시된 수직 시나리오의 process 및 connection failure를 다룬다. 모든 외부 effect의 exactly-once, power-loss 검증, host 간 database failover, 불확실한 Core/provider 결과의 자동 해결을 주장하지 않는다. mixed host effect가 포함된 일반 structured join은 기존 reconciliation 동작을 유지한다. 여러 generated child tree, 임의의 외부 validator 중 cancellation, 전체 graph-migration 조합, retention-root 자동 탐색, synthesis performance 및 storage-growth 검증은 별도 검증이 필요하다.

호스트 통합은 [영속성 계약](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md)을, 변경되지 않은 검토 source 및 권한 경계는 [권한 부여 계약](PROGRAM_CHILD_SYNTHESIS_CONTRACT.md)을 참조한다.
