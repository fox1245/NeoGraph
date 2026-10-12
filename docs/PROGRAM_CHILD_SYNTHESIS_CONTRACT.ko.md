<!-- neograph-i18n: source=docs/PROGRAM_CHILD_SYNTHESIS_CONTRACT.md locale=ko source_sha256=b4350a0091100913423075c5ccded5a70741490d738e4bfaabaa0e42158b2c9e -->
# 자식 Program 합성: 호스트 인가 계약

**Languages:** [English](PROGRAM_CHILD_SYNTHESIS_CONTRACT.md) | [한국어](PROGRAM_CHILD_SYNTHESIS_CONTRACT.ko.md) | [日本語](PROGRAM_CHILD_SYNTHESIS_CONTRACT.ja.md) | [简体中文](PROGRAM_CHILD_SYNTHESIS_CONTRACT.zh-CN.md)

상태: N1 호스트 경계와 N2 영속 런타임 통합을 구현했습니다. [SQLite/PostgreSQL 영속화와 복구](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md)를 참고하세요. Program 쪽의 전용 제출 경로와 모델 생성은 후속 작업으로 남아 있습니다.

## 진입점과 소유권

`ProgramSynthesisGateway::synthesize_child(proposal, grant, parent)`는 따로 선택한 `ProgramChildSynthesisGrant`가 있을 때만 제안을 받습니다. 호스트는 부모의 버전, run 레코드, 계보 head, 세대를 `ProgramChildSynthesisParent`에 적재합니다. 이 스냅샷과 grant는 호스트 자신의 저장소와 검토를 마친 템플릿 정책에서 와야 하며, 요청하는 Program이 넘겨준 값이어서는 안 됩니다.

첫 번째 경계는 검토를 마친 템플릿 **인스턴스**(instantiation)를 정확히 일치하는 경우에만 승인(admission)합니다. grant는 `template_identity`와 `program_synthesis_source_identity(source)`에 묶이며, 후자는 import 식별자, 봉인된 모듈 본문, 소스 좌표, 런타임/프로파일 식별자를 포함한 정규 소스 봉투(envelope) 전체를 포함합니다. 호스트는 이 grant를 발급하기 전에 인스턴스를 렌더링하고 검토합니다. 템플릿 렌더링과 그 파라미터 스키마는 이 API가 구현하지 않습니다. grant는 의미 검증기(semantic validator)와 작업 계약(task-contract) 식별자도 고정합니다. 자식 컴파일은 부모의 봉인된 레지스트리 지문(fingerprint)을 써야 합니다. 이번 첫 구현에서는 소스 텍스트가 같아도 서로 다른 등록 구현을 선택할 수 없습니다.

저장된 grant의 해시는 무결성만 증명하며 누가 발급했는지는 증명하지 않습니다. 호출자가 스스로 만든 grant를 파싱할 수 있다고 해서 그것을 쓸 권한이 생기는 것은 아닙니다. 게이트웨이는 호스트의 grant와 부모 스냅샷을 대신하는 직렬화된 인가 영수증(authorization receipt)을 받지 않습니다.

## 예약 전 검사

부수 효과가 없는 `authorize_program_child_synthesis` 연산은 다음을 검사합니다.

- 소유자, 부모 run, 부모 ProgramVersion과 번들, 부모 정책 지문, 계보 식별자, 활성 세대, 그리고 레코드/저널/head의 정확한 짝이 맞는지
- 부모 스냅샷이 실행 중이며 종료 상태가 아닌지
- 검토된 소스 식별자, 정규 소스 봉투의 바이트, 봉인된 모듈 수가 정확히 일치하는지
- 요청한 capability/effect가 호스트 grant와 부모 정책 모두에 들어맞는지, import한 모듈 식별자가 부모 정책에 들어맞는지
- 자식 예산의 아홉 개 항목이 모두 호스트 상한과 부모의 남은 예산 안에 들어오는지(부모의 진행 중인 예약은 빌려 쓰지 않음)
- 컴파일 한 번, 자식 한 개, 자손 깊이 한 단계를 더할 여유가 있는지
- 자식의 보장 하한이 부모의 실행 보장을 약화시키지 않는지

이 검사는 리소스를 예약하지 않습니다. 변경할 수 없는 읽기 전용 `ProgramChildSynthesisAuthorization` 증거를 만들 뿐입니다. 다른 자식의 예약과 여유 용량은 여전히 일반 경계인 `ProgramRuntime::start_child`에서 확인해야 합니다. 동시에 수행한 사전 검사(preflight) 결과가 각각 독립적인 예산 grant가 되는 것은 아닙니다.

## 예약과 컴파일

자식 진입점에는 `ProgramSynthesisGatewayConfig::reserve_child`가 필요합니다. 이 콜백은 제안과 인가 증거를 모두 받으며, 여기에는 compare-and-swap의 기준이 되는 `source_lineage_head_id`와 `parent_remaining`이 정확히 들어 있습니다. 콜백은 그 head를 기준으로 컴파일 한 건을 원자적으로 차감하거나, 다른 세대에 비용을 청구하지 않은 채 실패해야 합니다. 더 새로운 head를 다시 읽어서 몰래 그쪽에 예약해서는 안 됩니다. 호스트는 실행 소유권, 취소, 현재 데드라인도 확인하며, 예약을 정산할 때 경과한 벽시계 시간(wall time)을 청구합니다. 저장된 스냅샷은 그 벽시계 시간 허용량이 지금도 남아 있다는 증거가 아닙니다.

반환되는 `ProgramSynthesisReservation`은 같은 제안, 계보, 소스 head, 시작 예산에 묶여 있어야 합니다. 게이트웨이는 시작 예산이 바뀌었거나, head가 틀렸거나, 예약 후 예산이 부족하거나, 자식/깊이 용량이 사라졌으면 컴파일러를 평가하기 전에 거부합니다. 예약 객체를 만들 때도 컴파일 차감이 정확히 한 건이어야 하고 예산 증가는 허용되지 않습니다.

자식 컴파일은 부모에게 남은 더 큰 예산이 아니라 요청된 자식의 예산 상한을 사용합니다. 컴파일은 소스 식별자, 요청한 capability/effect closure, 인가된 보장 하한을 그대로 유지해야 합니다. 호스트의 의미 검증기는 필수이며, grant에 적힌 검증기 및 계약 식별자와 일치해야 하고 Catalog 승인(admission) 전에 실행됩니다. 결과로 나온 자식 정책은 요청을 넘어서는 capability/effect 권한을 가질 수 없고, 제출된 소스가 import한 범위 밖의 모듈 권한도 가질 수 없습니다. 의미 검증에서 거부되더라도 앞서 차감한 컴파일 예산은 되돌려지지 않습니다. 재시도하려면 해당 head에 대한 유효한 예약이 새로 필요하며, 오래된 사전 검사 증거로 예산을 보충할 수 없습니다.

자식 전용 게이트웨이는 일반 `reserve` 콜백을 생략할 수 있으며, 이 경우 일반 `synthesize()` 진입점을 호출하면 실패합니다. 반대로 기존의 후임(successor) 게이트웨이는 `reserve`가 설정되어 있다는 이유만으로 자식 합성 권한을 얻지 않습니다. 어느 쪽 진입점도 다른 쪽으로 대체되지 않습니다.

## JavaScript 예산과 보장의 경계

이제 제너레이터 Program은 호스트가 소유한 `ProgramBudgetBounds`와 Catalog 정책을 통해 0이 아닌 동적 컴파일 상한을 받을 수 있습니다. 기본 컴파일은 여전히 동적 컴파일을 0건으로 허용합니다. 호출자는 `start` 시점에 이 한도를 늘릴 수 없습니다. 선언만 담은 JavaScript와 `expand_task_graph`가 없는 일반 C++ 플랜은 동적 컴파일 0건이라는 구조 규칙을 그대로 유지합니다.

이 예산 변경으로 JavaScript 명령이나 암묵적인 컴파일러 접근이 추가되지는 않습니다. `ng.hostCapability`는 기존의 신뢰된 네이티브 인터페이스 그대로이며, N1은 거기에 합성 플러그인을 설치하거나 네이티브 C ABI를 바꾸지 않습니다.

현재 컴파일러는 보수적으로 제너레이터 제어를 `Unmanaged`로 표시합니다. 제한된 제너레이터 언어 프로파일이라고 해서 자동으로 `Strict` 실행 보장 라벨을 붙여서는 안 됩니다. N1은 이 분류를 그대로 유지합니다. 검토를 마친 선언 전용 자식은 Core closure가 Strict이면 Strict grant를 충족할 수 있지만, 제너레이터 자식은 제안서가 요청했다는 이유만으로 그 하한을 통과할 수 없습니다. 더 약한 자식을 승인(admission)하려면 호환되는 호스트 grant와 부모의 보장이 명시적으로 있어야 하며, 컴파일 중에 하한을 슬쩍 낮추어서는 안 됩니다.

## 진단

| 코드 | 경계 |
|---|---|
| `P_CHILD_SYNTHESIS_OWNER` | 소유자 불일치 |
| `P_CHILD_SYNTHESIS_GENERATION` | run/버전/정책이 잘못됨, head가 오래되었거나 일관되지 않음, 비활성 세대 |
| `P_CHILD_SYNTHESIS_SOURCE` | 검토되지 않은 인스턴스, 또는 소스/모듈 크기 한도 초과 |
| `P_CHILD_SYNTHESIS_REGISTRY` | 컴파일된 자식이 다른 봉인 레지스트리를 사용 |
| `P_CHILD_SYNTHESIS_SEMANTICS` | 검증기 또는 작업 계약 식별자가 바뀜 |
| `P_CHILD_SYNTHESIS_AUTHORITY` | 요청 또는 자식 승인(admission) 정책이 권한을 확대 |
| `P_CHILD_SYNTHESIS_BUDGET` | 실행 예산이 잘못되었거나 컴파일/자식/깊이/호스트 용량이 부족 |
| `P_CHILD_SYNTHESIS_RESERVATION` | 예약이 인가된 head 및 남은 예산과 묶여 있지 않음 |
| `P_CHILD_SYNTHESIS_GUARANTEE` | grant 또는 컴파일된 자식이 보장 하한을 위반 |

의미 검증 거부의 증거는 계속 `ProgramSynthesisValidationError`가 담습니다. 컴파일 오류는 기존 컴파일러 진단을 그대로 유지합니다. 저장된 예산을 읽는 디코더는 정규 식별자를 검증하기 전에 음수, 소수, 범위를 벗어난 정수를 거부하므로, 잘못된 숫자가 오버플로로 되감겨 유효한 저장 ID가 되는 일은 없습니다.

## 영속 통합과 남은 검증

독립 실행형 `synthesize_child` API는 승인(admission)에서 끝납니다. N2 런타임 API는 여기에 원자적 예약, 영속적인 단계별 결과, 부모 run/세대 범위의 binding, 그리고 SQLite와 PostgreSQL 위에서 기존 자식 생명주기를 통한 dispatch를 더합니다. 설정, 복구 동작, N3 실패 매트릭스와 그 한계는 [영속화 계약](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md)을 참고하세요. 인가 값은 계속 증거로만 쓰이며, 저장된 값으로 새 인가를 흉내 낼 수 있는 공개 생성자는 없습니다.

C++ API가 추가되었으므로 Program을 사용하는 쪽은 다시 빌드해야 합니다. 기존 후임 합성 결과 형식, JavaScript 명령 프로토콜, 네이티브 제어 C ABI, Core 전용 의존성 경계는 달라지지 않았습니다.
