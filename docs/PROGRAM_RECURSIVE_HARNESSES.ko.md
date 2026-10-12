<!-- neograph-i18n: source=docs/PROGRAM_RECURSIVE_HARNESSES.md locale=ko source_sha256=66abaa1eb78c67c16cf2ef0e6f42ad71632806116dc0b71dd952e8433d4b4970 -->
# 독립된 Harness 세대를 가진 재귀 에이전트

**Languages:** [English](PROGRAM_RECURSIVE_HARNESSES.md) | [한국어](PROGRAM_RECURSIVE_HARNESSES.ko.md) | [日本語](PROGRAM_RECURSIVE_HARNESSES.ja.md) | [简体中文](PROGRAM_RECURSIVE_HARNESSES.zh-CN.md)

검증된 시나리오는 논리 세션 하나를 한 소유자(owner) 아래에서 부모/자식/손자 트리로 실행합니다. 에이전트마다 고유한 ProgramVersion, Core 토폴로지, 상태, 예산, 계보(lineage)를 가집니다. 자식은 메인 오케스트레이터가 쓰는 것과 같은, 호스트가 소유한 합성(synthesis) 경계를 거쳐 자기 자식을 위한 JavaScript를 제안할 수 있습니다.

레퍼런스 테스트는 서로 다른 Core 플랜 네 개를 실행합니다. 메인 Harness의 플랜은 노드가 하나, 원래 자식은 둘, 교체된 자식은 셋, 손자는 넷입니다. 자식은 손자가 아직 살아 있는 동안 교체됩니다. 메인 오케스트레이터는 이미 걸어 둔 await로 교체된 자식의 결과를 받습니다. SQLite와 PostgreSQL 프로세스 종료 테스트는 중첩 교체가 커밋된 뒤 트리 전체를 다시 열고, 이미 끝난 작업을 다시 실행하지 않은 채 마무리합니다.

## 식별자와 저장

`ProgramRunRecord::logical_run_id()`와 `ProgramHandle::logical_run_id()`는 변하지 않는 에이전트 식별자를 반환합니다. 교체할 때마다 별도의 물리 run과 변경할 수 없는 ProgramVersion이 새로 만들어지고, 어느 세대가 활성인지는 계보가 정합니다. 그 세대의 핸들은 `reconnect(owner, logical_run_id)`로 얻습니다.

새 run 레코드 스키마 4는 물리 run과 다를 때 `logical_run_id`를 담습니다. 자식 관계는 계속 원래 자식의 식별자를 가리키며, 처음 승인(admission)된 link와 invocation을 유지합니다. 교체된 자식이 최종 결과를 제공하면, 관계에는 그 결과를 낸 실제 run/version 식별자와 함께 `terminal_generation`이 기록됩니다. 전이(transition) 백엔드는 이 값을 커밋된 자식 계보, 세대, 최종 run 레코드와 대조해 확인합니다.

기존 스키마 2/3 레코드는 계속 읽을 수 있고, 새 필드가 없는 일반 레코드는 스키마 3 표현을 그대로 유지합니다. 예전 Program 바이너리를 새 C++ 레코드 레이아웃과 섞어 쓰면 안 됩니다. Program을 사용하는 쪽은 모두 함께 다시 빌드해야 합니다.

## 자손이 있는 에이전트의 교체

실행 중인 에이전트 계열(family)을 교체하려면 소유 런타임과, 그 에이전트의 제너레이터에서 보류 중인(held) 완료된 최상위 체크포인트가 필요합니다. 이 체크포인트는 중첩된 에이전트의 것이어도 됩니다. 교체 대상은 이미 승인(admission)되어 있어야 합니다.

같은 트랜잭션이 새 세대를 게시하면서 기존 자식 관계와 커밋된 자손 예산을 복사합니다. 부모 식별자와 자식의 절대 깊이는 바꿀 수 없습니다. 교체 대상은 자식에게 부여된 capability/effect grant와 보장 하한(guarantee floor)을 그대로 유지해야 합니다. 중첩된 교체는 부모의 결과 계약(result contract)을 지켜야 합니다. 유지되는 자식은 binding 이름이 서로 달라야 하며, 중단된 자식이 있으면 부모를 교체하기 전에 처리 방침(disposition)을 명시해야 합니다.

이전 실행은 논리 에이전트의 종료 Hook을 호출하지 않고, 이전된 자식도 취소하지 않은 채 종료합니다. 자식 동시 실행 수와 quota의 정리는 후임 세대로 넘어가고, 시도(attempt)별 정리는 그대로 실행됩니다. 이미 그 자식을 기다리던 부모는 후임의 결과를 따라가며, 그 자식 핸들로 하는 취소도 같은 체인을 따라갑니다. 루트 세대의 핸들은 기존의 세대 의미를 유지합니다. 활성 루트를 다루려면 반환된 교체 핸들을 쓰거나 논리 식별자로 다시 연결(reconnect)해야 합니다.

세대를 만들 때 게시된 불변 데이터(publication)에 들어 있던 자식만 상속 binding이 됩니다. 기존 자식은 binding과 input이 정확히 일치할 때 다시 join할 수 있습니다.

```javascript
// Original child Harness:
const worker = yield ng.spawn("grandchild", {}, "child:spawn");
yield ng.checkpoint(
  {child: worker.child_run_id, replacement: "child-v2.json"},
  "child:swap"
);
```

```javascript
// Replacement Harness: this resolves the inherited child invocation.
const result = yield ng.await(
  ng.spawn("grandchild", {}, "replacement:join"),
  30000,
  "replacement:await"
);
return {generation: 2, result};
```

교체된 세대는 그 자식을 다시 만들지 않고, 자식의 invocation을 고쳐 쓰지 않으며, 자식 grant를 새로 받지도 않습니다. 이전 세대에서 연결되지 않은 채 남은 합성 binding은 새 권한이 되지 않습니다. 새 binding은 여전히 일반적인 합성 및 승인(admission) 경로를 거칩니다. 소스 제안은 독립적으로 검토한 호스트 템플릿 인스턴스와 일치해야 합니다. 레퍼런스 호스트는 에이전트가 체크포인트에 소스를 넣었다는 이유만으로 그 소스를 승인하지 않습니다.

## 프로세스 유실 후 미리 승인된 자식 복구

승인(admission)된 자식 binding을 복원하는 호스트는, 끝나지 않은 JavaScript `spawn`/`await` 명령이 이미 게시된 자식에 다시 연결되도록 명시적으로 허용할 수 있습니다.

```cpp
config.recover_existing_child_commands = true; // default: false
```

이 방식은 합성 게이트웨이 없이도 일반 정적 binding과 교체로 상속된 자식에 모두 동작합니다. 부모 관계, link receipt, 소유자, 정확한 invocation, 자식 깊이, 저장된 자식 run이 모두 일치해야 합니다. 정적 명령은 기록된 명령 좌표에서 같은 자식 ID를 계산해 내야 하고, 상속된 자식은 세대를 만들 때 게시된 데이터에서 승인(admission)된 ID를 그대로 유지합니다. 과거의 Program 버전과 호스트 grant도 계속 사용할 수 있어야 합니다. 호스트에 승인(admission) resolver가 설정되어 있으면, 그 resolver가 평소처럼 자식 시도를 승인(admission)합니다.

복구는 전용 reconnect 경로를 사용합니다. 확인 직후 자식이 사라져도 새 자식을 만드는 경로로 빠지지 않습니다. run이 없거나 input이 바뀌었거나 binding을 쓸 수 없으면, 기존 명령은 조정(reconciliation)을 기다리는 pending 상태로 남습니다. 명령 예약과 자손 회계는 복구 후에도 유지됩니다. 이 플래그가 provider, 도구, 네이티브 등 결과를 알 수 없는 외부 효과의 재생(replay)까지 허용하는 것은 아닙니다. 별도로 부여하는 합성 복구 프로토콜은 달라지지 않습니다.

이 플래그와 별개로, 실행 중이던 시도를 재생(replay)할 때는 끝나지 않은 봉인된(sealed) `ng.checkpoint`를 저널에 기록된 정확한 값과 원래의 명령 예약으로 마무리합니다. 이때 외부 dispatch는 하지 않습니다.
결과를 알 수 없는 다른 효과는 여전히 조정(reconciliation)을 위해 에이전트 계열을 일시 중지합니다. 중단된 자식은 dispatch된 채 재개할 수 있는 관계로 남고, 부모는 관계를 취소하거나 그 예약을 두 번 돌려주지 않고 진행 중인 명령 예약을 그대로 유지합니다.

저장소는 정상인데 결과 게시에 실패하면, dispatch된 명령의 결과와 pending effect를 모두 `Ambiguous`로 게시합니다. 취소해도 이 불확실성은 지워지지 않습니다. 낙관적(optimistic) 명령 head 읽기에서 충돌이 나면 재시도하며, 부모 head가 바뀌지 않았는데도 영구적으로 거부되는 경우가 자식 완료 알림을 무기한 막지는 못합니다.

`*StaticChild*` 테스트는 SQLite와 PostgreSQL의 프로세스 종료, 정적 자식과 상속된 재귀 자식, 바뀌지 않은 자식 식별자와 receipt, 그리고 손자의 체크포인트 32개가 저널 항목 정확히 64개를 만드는 경우를 다룹니다. 옵트인을 하지 않은 경우, run이나 binding이 없는 경우, input이나 receipt가 바뀐 경우, 복구 승인(admission)과 dispatch 사이에 run이 사라지는 경우도 검사합니다.

## JSON 산출물과 회계

JSON은 직렬화하고 검증을 마친 Program 번들입니다. 다른 JSON을 불러오면 변경할 수 없는 새 후보 버전이 선택될 뿐이며, 파일을 고쳐도 실행 중인 엔진은 바뀌지 않습니다. 호스트가 그 버전을 승인(admission)하고, 보류 중인 체크포인트에서 에이전트의 세대를 전환합니다. 제너레이터의 힙/스택 상태는 옮기지 않습니다. 애플리케이션 상태는 명시적인 `handoff` JSON으로 넘기고, 자식 관계는 영속 트리에 그대로 남습니다.

예제의 교체용 번들은 세션이 시작되기 전에 컴파일하고 승인(admission)합니다. 메인 에이전트와 자식 에이전트는 실행 중에 자기 자손이 쓸 JavaScript를 제안하며, 이 컴파일은 각자의 grant를 소비합니다. 새로 생성한 자기 교체본을 준비하는 일은 컴파일/승인(admission)이라는 별도 작업이며, `replace` API 자체는 이미 승인된 대상을 고르기만 합니다.

자손에게 위임한 컴파일 예산은 부모의 다른 합성 요청에 쓸 수 없습니다. 교체하면 남은 예산과 커밋된 자손이 정확히 그대로 넘어갑니다. 자식 복구도 첫 시도의 데드라인을 유지합니다. SQLite Catalog는 이제 busy timeout에 상한을 두며, 이는 전이(transition) 저장소가 동시에 일어나는 에이전트 게시와 데이터베이스를 공유할 수 있는 것과 같은 조건을 맞춘 것입니다.

## 레퍼런스 실행하기

Program과 QuickJS를 빌드하고, 테스트 케이스에 필요한 영속 백엔드를 둘 다 켭니다. `neograph_program_tests --gtest_filter="*Recursive*"`를 실행합니다. PostgreSQL 케이스는 기존의 일회용 `NEOGRAPH_TEST_POSTGRES_URL` 픽스처와 CTest 데이터베이스 리소스 락을 사용합니다. 이 구성에는 WSL의 네이티브 Docker를 쓸 수 있습니다.

토폴로지 성공 테스트를 실행할 때 `NEOGRAPH_RECURSIVE_ARTIFACT_DIR`을 지정하면 `main.json`, `child-v1.json`, `child-v2.json`, `grandchild.json`, `session.json`을 내보냅니다. 이 파일은 테스트용 레지스트리/컴파일러 식별자를 쓰므로 검토용 픽스처일 뿐, 단독으로 쓰는 프로덕션 설정이 아닙니다.

이 검증이 다루는 범위는 3단계 트리, 중첩된 체크포인트 교체, 실행 중인 자손의 유지와 취소, 세대 결과의 무결성, 예산 감쇠(attenuation), 그리고 SQLite/PostgreSQL에서의 세션 전체 프로세스 복구입니다. LLM 소스 생성 서비스, Python/전송 파사드, 소유자 간 세션 공유 정책, Core를 제자리에서 임의로 수정하는 기능은 새로 도입하지 않습니다. 네이티브 Core 마이그레이션은 기존의 경계를 그대로 유지합니다. 서로 독립된 여러 호스트 간의 실시간 소유권 이전과 모든 컨텍스트/Hook 실패 조합은 별도로 검증해야 합니다.

## 대화형 챗봇 예제

[진화하는 Harness 챗봇](../examples/cookbook/self_evolving_chatbot/README.md)은 이 레퍼런스에 다음을 더합니다. 격리된 테넌트 둘, OpenRouter 어댑터, 브라우저 인스펙터, 턴마다 검토된 템플릿 제안, 실행 중 후임 컴파일, SQLite 또는 PostgreSQL 위의 채팅/provider 원장입니다. 이 챗봇의 어시스턴트는 교체 후에 리뷰어를 합성할 수 있으며, 원래 오케스트레이터는 같은 논리 어시스턴트를 계속 기다립니다.

`RuntimeConfig::checkpoint_handler`를 쓰면 호스트가 영속 게시 이후에 핸들과 이동만 가능한(move-only) 체크포인트 lease를 큐에 넣을 수 있습니다. lease를 쥐고 있는 동안 제너레이터는 스케줄러 스레드를 막지 않은 채 일시 중지됩니다. 콜백은 곧바로 반환해야 하므로, 컴파일과 교체는 호스트의 워커에서 수행합니다. 다시 연결할 때는 가장 최근에 완료된 체크포인트의 재생(replay)도 이 콜백으로 전달됩니다. 명시적인 `next_handoff` 요청이 있으면 그것이 우선합니다.

`ProgramRuntime::reserve_synthesis`는 아직 배정되지 않은 동적 컴파일 한 건을 예상한 계보 head 기준으로 차감하고, 보류 중인 lease의 저널 참조를 갱신합니다. 체크포인트 식별자와 직렬화된 handoff 값은 바뀌지 않습니다. head가 오래되었거나, 소유자/런타임이 다르거나, 벽시계(wall) 예산이 만료되었거나, 컴파일 예산이 자손에게 배정되어 있으면 거부됩니다. 호스트는 예약하기 전에 의도를 영속화하고 결과를 기록해야 합니다. 확인 응답이 모호하다고 해서 예산 차감 없이 재시도할 수는 없습니다. 챗봇은 일반 `ProgramSynthesisGateway`에서 이 예약을 사용한 다음, 승인(admission)된 대상을 `replace`에 넘깁니다.

챗봇의 템플릿 게이트는 답변 품질을 보증하지 않으며, 모델 호출과 제너레이터 제어의 보장 수준은 `Unmanaged`로 유지됩니다. 프로세스 유실 뒤에 남은 pending provider 효과는 자동으로 다시 dispatch하지 않고 조정(reconciliation)을 거쳐야 합니다.
