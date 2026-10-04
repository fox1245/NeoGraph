<!-- neograph-i18n: source=docs/PROGRAM_CHILD_SYNTHESIS_CONTRACT.md locale=ko source_sha256=b4350a0091100913423075c5ccded5a70741490d738e4bfaabaa0e42158b2c9e -->
# 자식 Program 합성: 호스트 권한 부여 계약

**Languages:** [English](PROGRAM_CHILD_SYNTHESIS_CONTRACT.md) | [한국어](PROGRAM_CHILD_SYNTHESIS_CONTRACT.ko.md) | [日本語](PROGRAM_CHILD_SYNTHESIS_CONTRACT.ja.md) | [简体中文](PROGRAM_CHILD_SYNTHESIS_CONTRACT.zh-CN.md)

상태: N1 호스트 경계와 N2 영속 런타임 통합이 구현되었다. [SQLite/PostgreSQL 영속성 및 복구](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md)를 참조한다. 전용 Program 측 제출과 모델 생성은 후속 작업으로 남아 있다.

## 진입점과 소유권

`ProgramSynthesisGateway::synthesize_child(proposal, grant, parent)`는 별도로 선택한 `ProgramChildSynthesisGrant`가 있을 때만 제안을 받는다. 호스트는 부모 version, run record, lineage head, generation을 `ProgramChildSynthesisParent`에 읽어 넣는다. 이 snapshot과 grant는 요청하는 Program이 아니라 호스트 자체 저장소와 검토된 template 정책에서 가져와야 한다.

첫 경계는 정확히 검토한 template **인스턴스**를 승인한다. grant는 `template_identity`와 `program_synthesis_source_identity(source)`를 바인딩한다. 후자는 import 신원, 봉인된 module 본문, source 좌표, runtime/profile 신원을 포함한 전체 canonical source envelope를 포괄한다. 호스트는 grant를 발급하기 전에 인스턴스를 렌더링하고 검토한다. 이 API는 template 렌더링과 그 parameter schema를 구현하지 않는다. grant는 semantic validator와 task-contract 신원도 고정한다. 자식 컴파일에는 부모의 봉인된 registry fingerprint를 사용해야 한다. 이 첫 구현에서는 source text가 같아도 다른 등록 구현을 선택할 수 없다.

저장된 grant의 hash는 무결성을 증명하지 발급자를 증명하지 않는다. 호출자가 스스로 작성한 grant를 파싱한다고 해서 사용할 권한이 생기지 않는다. gateway는 호스트 grant와 부모 snapshot 대신 직렬화된 authorization receipt를 받지 않는다.

## 예약 전 검사

순수 연산인 `authorize_program_child_synthesis`는 다음을 검사한다.

- owner, parent run, parent ProgramVersion 및 bundle, parent policy fingerprint, lineage 신원, 활성 generation, 정확한 record/journal/head 조합.
- 부모 snapshot이 실행 중이며 terminal 상태가 아닌지.
- 정확히 검토한 source 신원, canonical source-envelope byte, 봉인된 module 수.
- 요청한 capability/effect가 호스트 grant와 부모 policy를 모두 만족하는지, import된 module 신원이 부모 policy를 만족하는지.
- 부모의 진행 중 예약을 빌리지 않고, 자식 budget의 아홉 차원이 모두 호스트 상한과 사용 가능한 부모 잔여량 안에 있는지.
- compile 한 번, child 하나, descendant-depth 한 단계에 필요한 여유.
- 부모의 execution guarantee를 낮출 수 없는 자식 guarantee 하한.

이 검사는 자원을 예약하지 않는다. 불변의 읽기 전용 `ProgramChildSynthesisAuthorization` 증거를 만든다. 다른 자식 예약과 capacity는 여전히 일반 `ProgramRuntime::start_child` 경계에서 검사해야 한다. 동시 preflight 결과는 서로 독립적인 budget grant가 아니다.

## 예약과 컴파일

자식 진입점에는 `ProgramSynthesisGatewayConfig::reserve_child`가 필요하다. 이 callback의 signature는 proposal과 authorization을 모두 받으며, compare-and-swap에 사용할 정확한 `source_lineage_head_id`와 `parent_remaining`도 포함한다. 해당 head에 대해 compile 하나를 원자적으로 차감해야 하며, 실패하면 다른 generation에 비용을 부과해서는 안 된다. 더 새로운 head를 다시 읽어 그 head를 대상으로 조용히 예약해서는 안 된다. 호스트는 실행 소유권, cancellation, 현재 deadline도 확인하고 예약 정산 시 경과한 wall time을 차감한다. 저장된 snapshot만으로는 지금도 wall-time 허용량이 남아 있음을 증명할 수 없다.

반환된 `ProgramSynthesisReservation`은 동일한 proposal, lineage, source head, 시작 budget을 바인딩해야 한다. gateway는 컴파일러 평가 전에 변경된 시작 budget, 잘못된 head, 예약 후의 부족한 budget, child/depth capacity 상실을 거부한다. reservation 생성 자체도 정확히 한 번의 compile 차감을 요구하고 어떤 budget 증가도 금지한다.

자식 컴파일은 부모의 더 큰 잔여 budget이 아니라 요청한 자식의 budget 상한을 사용한다. 컴파일은 source 신원, 요청한 capability/effect 폐쇄 집합, 승인된 guarantee 하한을 보존해야 한다. 필수 호스트 semantic validator는 grant의 validator 및 contract 신원과 일치해야 하며 Catalog 승인 전에 실행된다. 결과 자식 policy는 요청을 넘는 capability/effect 권한이나 제출 source의 import 밖에 있는 module 권한을 가질 수 없다. semantic 거부는 앞서 차감한 compile을 되돌리지 않는다. retry에는 유효한 head별 예약이 필요하다. 오래된 preflight 증거를 재사용해 budget을 보충해서는 안 된다.

자식 전용 gateway는 일반 `reserve` callback을 생략할 수 있다. 이때 일반 `synthesize()` 진입점 호출은 실패한다. 반대로 기존 successor gateway에 `reserve`가 구성되었다는 이유만으로 자식 합성 권한이 생기지 않는다. 어느 진입점도 다른 진입점으로 fallback하지 않는다.

## JavaScript budget과 guarantee 경계

Generator Program은 이제 호스트 소유 `ProgramBudgetBounds`와 Catalog policy를 통해 0이 아닌 dynamic-compile 상한을 받을 수 있다. 기본 컴파일은 여전히 dynamic compile을 0회 부여한다. 호출자는 `start`에서 이 한도를 늘릴 수 없다. 선언 전용 JavaScript와 `expand_task_graph`가 없는 일반 C++ plan은 dynamic compile이 0회라는 구조 규칙을 유지한다.

이 budget 변경은 JavaScript command나 암묵적인 compiler 접근 권한을 추가하지 않는다. `ng.hostCapability`는 기존 trusted-native interface를 유지한다. N1은 여기에 합성 plugin을 설치하거나 native C ABI를 변경하지 않는다.

현재 compiler는 generator control을 보수적으로 `Unmanaged`로 분류한다. 제한된 generator language profile을 자동 `Strict` execution-guarantee 표지로 소개해서는 안 된다. N1은 이 분류를 유지한다. 검토된 선언 전용 자식은 Core 폐쇄 집합이 Strict이면 Strict grant를 충족할 수 있다. generator 자식은 proposal에서 그 하한을 요청했다는 이유만으로 통과할 수 없다. 더 약한 자식을 승인하려면 명시적으로 호환되는 호스트 grant와 부모 guarantee가 필요하며, 컴파일 중 하한을 조용히 낮춰서는 안 된다.

## 진단

| 코드 | 경계 |
|---|---|
| `P_CHILD_SYNTHESIS_OWNER` | Owner 불일치 |
| `P_CHILD_SYNTHESIS_GENERATION` | 잘못된 run/version/policy, 오래되거나 불일치하는 head, 비활성 generation |
| `P_CHILD_SYNTHESIS_SOURCE` | 검토하지 않은 인스턴스 또는 source/module 크기 한도 |
| `P_CHILD_SYNTHESIS_REGISTRY` | 컴파일된 자식이 다른 봉인 registry를 사용함 |
| `P_CHILD_SYNTHESIS_SEMANTICS` | Validator 또는 task-contract 신원이 변경됨 |
| `P_CHILD_SYNTHESIS_AUTHORITY` | 요청 또는 자식 승인 policy가 권한을 확장함 |
| `P_CHILD_SYNTHESIS_BUDGET` | 유효하지 않은 execution budget 또는 compile/child/depth/host capacity 부족 |
| `P_CHILD_SYNTHESIS_RESERVATION` | 예약이 승인된 head와 잔여 budget을 바인딩하지 않음 |
| `P_CHILD_SYNTHESIS_GUARANTEE` | Grant 또는 컴파일된 자식이 guarantee 하한을 위반함 |

`ProgramSynthesisValidationError`는 계속 semantic 거부 증거를 담는다. compile error는 기존 compiler 진단을 유지한다. 저장 budget decoder는 canonical 신원을 검증하기 전에 음수, 소수, 범위를 벗어난 정수를 거부한다. 유효하지 않은 수가 overflow로 변환되어 그 외에는 유효한 stored ID 안에 들어갈 수 없다.

## 영속 통합과 남은 검증

독립적인 `synthesize_child` API는 승인 단계에서 끝난다. N2 runtime API는 SQLite와 PostgreSQL에서 원자적 예약, 영속 stage 결과, parent-run/generation 범위의 binding, 기존 child lifecycle을 통한 dispatch를 추가한다. 구성, 복구 동작, N3 failure matrix와 명시된 한도는 [영속성 계약](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md)을 참조한다. authorization 값은 계속 증거일 뿐이며, 새 authorization으로 오인할 수 있는 공개 stored-value 생성자는 없다.

이 C++ API 추가로 인해 Program consumer를 다시 빌드해야 한다. 기존 successor synthesis 결과 형식, JavaScript command protocol, native control C ABI, Core-only dependency 경계는 바뀌지 않는다.
