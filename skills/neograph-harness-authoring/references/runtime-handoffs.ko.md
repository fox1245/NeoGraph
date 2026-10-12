<!-- neograph-i18n: source=skills/neograph-harness-authoring/references/runtime-handoffs.md locale=ko source_sha256=26d4edee068a5a8108f1a4bc1e21380f319667d5477acf772d58904c8d0ae05c -->
# 런타임 자식 합성과 교체

**Languages:** [English](runtime-handoffs.md) | [한국어](runtime-handoffs.ko.md) | [日本語](runtime-handoffs.ja.md) | [简体中文](runtime-handoffs.zh-CN.md)

이 문서는 아래 서비스를 실제로 제공하는 호스트를 위한 참고 자료입니다. Skill이 있다고 해서
ng.compile, ng.replace, 또는 상상으로 만든 MCP 교체 도구가 API가 되지는 않습니다.
호환용 Harness MCP 인터페이스는 워크플로를 컴파일하고 시작합니다. 실행 중인 Harness를
교체하려면 여기서 설명하는 네이티브 호스트 통합이 필요합니다.

## 필요한 호스트 패킷

제공된 호스트 서비스에서 다음을 가져옵니다. 유효한 owner/run/버전, 현재 lineage head,
사용할 수 있는 레지스트리, 제안 스키마(템플릿 매개변수 또는 전체 소스), 입력/출력 계약,
남은 한도, 결과/진단 형식입니다. 요청에 이 정보가 없으면 사용 가능한 설정을 살펴봅니다.
바인딩을 짐작하거나 작업이 실행됐다고 주장하지 말고, 빠진 계약이 무엇인지 밝혀야 합니다.

모델은 소스나 매개변수를 제안하고, 호스트는 권한을 부여하고 원자적인 상태 변경을
수행합니다. provider 자격 증명과 신뢰할 수 있는 grant는 호스트가 보관합니다.

## 자식 만들기

1. 구체적인 자식 작업과 필요한 출력에 맞춰 소스 또는 템플릿 매개변수를 제안합니다.
   부모에게 허용된 capability와 남은 한도 안에서만 요청합니다.
2. 필요한 경계에서 호스트의 제안 봉투(proposal envelope)를 제출합니다. 호스트는 실제
   부모/head를 바인딩하고, 자원을 예약하고, 컴파일하고, 의미 검증을 하고, 승인(admission)하고,
   게시하고, 자식을 바인딩합니다. 모델이 제공한 grant는 권한이 아닙니다.
3. 결과를 읽습니다. 거부됐으면 반환된 진단과 남은 수정 허용량을 사용합니다. 바인딩된
   결과라면 정확한 자식 바인딩과 입력 계약을 사용합니다.
4. 승인(admission)된 spawn 명령을 yield합니다. 결과가 필요하면 await로 감쌉니다. 자식의
   결과나 정확한 실패 원인을 기록하고, 큐에 들어간 제안을 실행된 자식으로 보고해서는
   안 됩니다.

예를 들어 챗봇의 검토를 마친 review 템플릿은 kind="review", turn, source를 담은
체크포인트를 내보냅니다. 이 챗봇의 호스트는 이름을 "review-" + turn으로 만들고, 바인딩하기
전에 source가 검토된 템플릿과 일치하는지 확인합니다. 이 필드 이름은 이 예제에만 해당하며
범용 제안 스키마가 아닙니다.

## 현재 Harness 교체

1. 후속 Harness에 필요한 내용을 직렬화할 수 있는 명시적 상태로 구성합니다. 대화/작업 진행
   상황과, 유지된 작업에 다시 합류하는 데 필요한 정확한 입력을 담습니다.
2. checkpoint(state, stableSite)를 yield합니다. 호스트는 그 경계에서 lease를 유지합니다.
3. 호스트는 합성 의도를 영속적으로 기록하고, 동적 컴파일을 예약하고, 후속 Harness를
   컴파일, 검증, 승인(admission)한 뒤, 교체할 정확한 버전을 generation CAS로 선택합니다.
4. 후속 Harness는 새 제너레이터를 시작합니다. 새 작업을 하기 전에 input.handoff에 선언된
   인계 상태를 복원해야 합니다. 인계가 있으면 카운터와 이력을 처음부터 다시 초기화하면
   안 됩니다. 호스트는 previous_run_id도 함께 바인딩합니다.
5. 논리적 부모, 자식 관계, 출력 계약, 부여된 권한은 호환되게 유지해야 합니다. 아직 남아 있는
   갱신 불가 자원은 후속 Harness로 이전됩니다.

경계를 넘어가는 것은 JSON 상태이며, QuickJS 힙, 제너레이터 스택, 포인터, promise, 네이티브
핸들은 넘어가지 않습니다. 게시된 그래프/버전은 변경할 수 없습니다.

## 유지되는 자식과 복구

유지되는 자식은 바인딩과 원래 입력으로 기존 작업이 식별됩니다. 현재 네이티브 런타임에서는
물려받은 바인딩을 기록된 입력으로 spawn하면 그 자식에 다시 합류합니다. 입력은 원래 텍스트의
키 순서가 아니라 정규(canonical) JSON 내용으로 비교하며, 이 내용을 바꾸면 충돌입니다. 새
작업에는 호스트가 승인(admission)한 새 바인딩과 새로 예약한, 축소된(attenuated) 자식 예산이
필요합니다. 교체 과정에서 이미 끝난 외부 작업을 반복하거나 이미 쓴 한도를 되살려서는 안
됩니다.

head가 오래되어 충돌하면 활성 generation을 다시 불러옵니다. provider나 컴파일러의 결과가
불확실하면 제공된 호스트 경로로 조정(reconciliation)해야 하며, 쓸 만한 답이 없다는 이유만으로
다시 dispatch해서는 안 됩니다. 취소가 자식에서 부모로, 또는 전역으로 전파된다고 가정하지
말고, 호스트가 반환한 논리적 계열(logical family)의 취소 계약을 따릅니다. 반환된 실행
보장(execution guarantee)은 있는 그대로 보고합니다.

## 네이티브 호스트 구현 진입점

- RuntimeConfig::checkpoint_handler: handle/lease를 큐에 넣고 바로 반환합니다. lease를
  유지하면 해당 generation이 일시 중지되므로, 호스트 작업은 스케줄러 밖에서 수행합니다.
- ProgramRuntime::prepare_child_synthesis: 제안과, 호스트가 따로 발급한 권한 grant를 잡아 둔
  부모 체크포인트에 바인딩합니다. 이 grant는 부모의 남은 컴파일 예산이나 wall-time 예산을
  다시 채워 주지 않습니다.
- ProgramSynthesisGateway: 컴파일, 의미 검증, Catalog 승인(admission)을 맡습니다.
- ProgramRuntime::reserve_synthesis: expected-head CAS로 아직 할당되지 않은 컴파일 한 건을
  차감합니다. 체크포인트 식별은 그대로 두고 lease의 journal head를 갱신합니다.
- ProgramRuntime::replace: 잡아 둔 체크포인트와 승인(admission)된 대상을 소비합니다.
  명시적인 handoff/선행 실행(predecessor) 입력과 소스의 남은 자원을 사용합니다.
- reconnect/recover_child_synthesis: 기록된 generation/단계를 복구하거나, 조정이 필요하다는
  요구를 반환합니다.

여기 나온 이름은 C++ 호스트 API이며, JavaScript 명령이나 모델이 자동으로 쓰는 도구가
아닙니다. 시그니처와 전체 호출 방법은 실제 저장소의 헤더와 서비스 스키마를 따릅니다.
제너레이터 제어는 API의 Unmanaged 실행 보장을 그대로 유지합니다. 이 이름이 호스트 제어가
없다는 뜻은 아닙니다. 컴파일러가 받아들였다고 해서 동작 품질이 입증되는 것은 아닙니다.
