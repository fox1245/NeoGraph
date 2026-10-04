<!-- neograph-i18n: source=docs/PROGRAM_RECURSIVE_HARNESSES.md locale=ko source_sha256=66abaa1eb78c67c16cf2ef0e6f42ad71632806116dc0b71dd952e8433d4b4970 -->
# 독립적인 Harness generation을 가진 재귀 agent

**Languages:** [English](PROGRAM_RECURSIVE_HARNESSES.md) | [한국어](PROGRAM_RECURSIVE_HARNESSES.ko.md) | [日本語](PROGRAM_RECURSIVE_HARNESSES.ja.md) | [简体中文](PROGRAM_RECURSIVE_HARNESSES.zh-CN.md)

검증된 시나리오는 하나의 owner 아래에서 하나의 논리적 session을 부모/자식/손자 tree로 실행한다. 각 agent는 자체 ProgramVersion, Core topology, state, budget, lineage를 가진다. 자식은 main orchestrator가 사용하는 것과 동일한 호스트 소유 synthesis 경계를 통해 자신의 자식을 위한 JavaScript를 제안할 수 있다.

reference test는 서로 다른 Core plan 네 개를 실행한다. main Harness에 노드 하나, 원래 자식에 둘, replacement 자식에 셋, 손자에 넷이 있다. 손자가 살아 있는 동안 자식을 교체한다. main orchestrator는 기존 await를 통해 replacement 자식의 result를 받는다. SQLite와 PostgreSQL process-exit test는 중첩 replacement commit 이후 전체 tree를 다시 열고 완료된 작업을 다시 실행하지 않은 채 마친다.

## 신원과 저장

`ProgramRunRecord::logical_run_id()`와 `ProgramHandle::logical_run_id()`는 안정적인 agent 신원을 노출한다. 각 replacement는 여전히 별도의 physical run과 불변 ProgramVersion을 만든다. lineage가 활성 generation을 선택한다. `reconnect(owner, logical_run_id)`로 해당 generation의 handle을 얻는다.

새 run-record schema 4는 physical run과 다를 때 `logical_run_id`를 담는다. child relation은 계속 원래 child 신원을 가리키며 최초로 승인된 link와 invocation을 유지한다. replacement가 terminal result를 제공하면 relation은 그 result의 실제 run/version 신원과 함께 `terminal_generation`을 기록한다. transition backend는 이를 commit된 child lineage, generation, terminal run record와 대조한다.

기존 schema 2/3 record는 계속 읽을 수 있으며, 새 field가 없는 일반 record는 schema 3 표현을 유지한다. 오래된 Program binary와 새 C++ record layout을 혼용하지 않는다. Program consumer를 함께 다시 빌드한다.

## Descendant를 가진 replacement

살아 있는 family의 replacement는 소유 runtime과 해당 agent generator의 유지 중이며 완료된 top-level checkpoint를 사용한다. 이 checkpoint는 중첩 agent에 속할 수 있다. target은 이미 승인되어 있어야 한다.

같은 transaction이 새 generation을 게시하고 기존 child relation과 commit된 descendant budget을 복사한다. parent 신원과 절대 child depth는 바뀔 수 없다. target은 자식의 capability/effect grant와 guarantee 하한을 유지해야 한다. 중첩 replacement는 부모의 result contract를 보존해야 한다. 유지되는 자식에는 고유한 binding 이름이 필요하다. 중단된 자식은 부모를 교체하기 전에 명시적으로 처리 방침을 정해야 한다.

오래된 execution은 logical-agent terminal hook을 호출하거나 이전된 자식을 취소하지 않고 종료된다. logical child-concurrency와 quota 정리는 successor로 옮기며 attempt별 정리는 계속 실행한다. 이미 자식을 기다리는 부모는 그 successor result를 따라가고, 그 child handle을 통한 cancellation도 같은 chain을 따라간다. root-generation handle은 기존 generation 의미를 유지한다. 활성 root에는 반환된 replacement handle을 사용하거나 logical 신원으로 reconnect한다.

불변 generation-creation publication에 존재하는 자식만 inherited binding이 된다. 기존 자식은 정확히 일치하는 binding과 input으로 다시 join할 수 있다.

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

replacement는 해당 child를 다시 만들거나 invocation을 다시 쓰거나 또 다른 child grant를 얻지 않는다. 오래된 generation의 연결되지 않은 synthesis binding은 새 권한이 되지 않는다. 새 binding은 여전히 일반 synthesis 및 admission 경로를 사용한다. source proposal은 독립적으로 검토한 호스트 template 인스턴스와 일치해야 한다. reference host는 agent가 checkpoint에 source를 포함했다는 이유만으로 승인하지 않는다.

## Process 상실 후 사전 승인된 자식 복구

승인된 child binding을 복원하는 호스트는 미완료 JavaScript `spawn`/`await` command가 이미 게시된 자식에 reconnect하도록 명시적으로 허용할 수 있다.

```cpp
config.recover_existing_child_commands = true; // default: false
```

synthesis gateway 없이도 일반 static binding과 replacement를 통해 상속된 자식에 적용된다. parent relation, link receipt, owner, 정확한 invocation, child depth, 저장된 child run이 일치해야 한다. static command는 기록된 command 좌표에서 같은 child ID를 도출해야 한다. inherited child는 generation-creation publication에서 승인된 ID를 유지한다. 과거 Program version과 호스트 grant도 여전히 사용 가능해야 한다. 호스트 admission resolver가 구성되어 있으면 계속 일반 절차로 child attempt를 승인한다.

복구는 전용 reconnect 경로를 사용한다. 검사 이후 자식이 사라져도 새 child 생성으로 넘어갈 수 없다. run이 없거나 input이 바뀌었거나 binding을 사용할 수 없으면 기존 command를 reconciliation 대기 상태로 남긴다. command reservation과 descendant accounting은 복구 이후에도 유지된다. 이 flag는 알 수 없는 provider, tool, native 또는 다른 외부 effect의 replay를 허가하지 않는다. 별도 grant를 받는 synthesis 복구 protocol은 바뀌지 않는다.

이 flag와 무관하게 running attempt의 replay는 정확히 journal에 기록된 값과 원래 command reservation으로 미완료 봉인 `ng.checkpoint`를 완료한다. 외부 dispatch는 수행하지 않는다. 다른 알 수 없는 effect는 여전히 family를 reconciliation을 위해 일시 중지한다. 중단된 자식은 dispatch되었으며 재개 가능한 relation으로 남고, 부모는 relation을 취소하거나 reservation을 두 번 환급하는 대신 진행 중 command reservation을 유지한다.

storage가 계속 사용 가능한 상태에서 result publication이 실패하면 dispatch된 command의 outcome과 pending effect를 모두 `Ambiguous`로 게시한다. cancellation은 그 불확실성을 지울 수 없다. 낙관적 command-head 읽기 충돌은 retry하지만, 변경되지 않은 parent head에 대한 영구적인 거부가 child completion notification을 무한히 차단할 수는 없다.

`*StaticChild*` test는 SQLite와 PostgreSQL process exit, static 및 inherited recursive child, 바뀌지 않은 child 신원과 receipt, 손자의 checkpoint 32개가 정확히 journal entry 64개를 만드는 경우를 다룬다. 또한 opt-in 없음, run/binding 없음, 변경된 input/receipt, recovery admission과 dispatch 사이에 run이 사라지는 경우를 확인한다.

## JSON artifact와 accounting

JSON은 직렬화되고 검증된 Program bundle이다. 다른 JSON을 읽으면 후보 불변 version을 선택한다. 파일을 바꾼다고 live engine이 변경되지는 않는다. 호스트는 version을 승인하고 유지 중인 checkpoint에서 agent의 generation을 전환한다. generator heap/stack state는 이식하지 않는다. application state는 명시적인 `handoff` JSON을 통해 전달되고 child relationship은 영속 tree에 남는다.

예제의 replacement bundle은 session 시작 전에 컴파일되고 승인된다. main과 child agent는 실행 중 descendant JavaScript를 제안하며 그 컴파일은 각자의 grant를 소비한다. 새로 생성한 self-replacement를 준비하는 일은 별도의 compilation/admission 연산이다. `replace` API 자체는 이미 승인된 target을 선택한다.

위임한 compile budget은 다른 부모 synthesis 요청에 쓸 수 없다. replacement는 정확한 잔여 budget과 commit된 descendant를 이전한다. child recovery는 최초 attempt의 deadline도 유지한다. SQLite Catalog는 이제 제한된 busy timeout을 사용한다. 이는 동시 agent publication과 database를 공유할 수 있는 transition store의 동작에 맞춘 것이다.

## Reference 실행

Program과 QuickJS를 빌드하고 각 case에 대해 두 persistence backend를 모두 활성화한다. `neograph_program_tests --gtest_filter="*Recursive*"`를 실행한다. PostgreSQL case는 기존 일회용 `NEOGRAPH_TEST_POSTGRES_URL` fixture와 CTest database resource lock을 사용한다. native WSL Docker는 이 구성에 적합하다.

성공적인 topology test 실행 시 `NEOGRAPH_RECURSIVE_ARTIFACT_DIR`를 설정하면 `main.json`, `child-v1.json`, `child-v2.json`, `grandchild.json`, `session.json`을 export한다. 이 파일은 test registry/compiler 신원을 사용하며 검토 fixture이지 독립적인 production configuration이 아니다.

이 검증은 3단계 tree, 중첩 checkpoint replacement, live descendant 유지 및 cancellation, generation-result 무결성, budget 제한, SQLite/PostgreSQL의 전체 session process recovery를 다룬다. LLM source-generation service, Python/transport facade, cross-owner session 공유 policy, 임의의 in-place Core 변경을 도입하지 않는다. native Core migration은 기존 경계를 유지한다. 독립적인 multi-host live ownership 이전과 모든 context/hook failure 조합은 별도 검증이 필요하다.

## 대화형 chatbot 예제

[진화하는 Harness chatbot](../examples/cookbook/self_evolving_chatbot/README.md)은 reference에 격리된 tenant 둘, OpenRouter adapter, browser inspector, turn별 검토 template proposal, runtime successor compilation, SQLite 또는 PostgreSQL의 chat/provider ledger를 추가한다. assistant는 replacement 이후 reviewer를 합성할 수 있으며 원래 orchestrator는 계속 같은 logical assistant를 await한다.

`RuntimeConfig::checkpoint_handler`를 사용하면 호스트가 영속 publication 이후 handle과 이동 전용 checkpoint lease를 queue에 넣을 수 있다. lease를 유지하면 scheduler thread를 차단하지 않고 generator를 일시 중지한다. callback은 신속히 반환해야 한다. compilation과 replacement는 호스트 worker에서 수행한다. reconnect 시 최신 완료 checkpoint의 replay도 관찰한다. 명시적인 `next_handoff` 요청이 우선한다.

`ProgramRuntime::reserve_synthesis`는 예상 lineage head에 대해 미할당 dynamic compile 하나를 차감하고 유지 중인 lease의 journal 참조를 갱신한다. checkpoint 신원과 직렬화된 handoff 값은 바뀌지 않는다. 오래된 head, 다른 owner/runtime, 만료된 wall budget, descendant에 할당된 compile budget은 거부한다. 호스트는 reservation 전에 intent를 저장하고 outcome을 기록해야 한다. 모호한 acknowledgement는 무료 retry를 허가할 수 없다. chatbot은 일반 `ProgramSynthesisGateway`에서 이 reservation을 사용한 뒤 승인된 target을 `replace`에 전달한다.

chatbot의 template gate는 답변 품질의 증명이 아니며 model call과 generator control은 `Unmanaged` guarantee를 유지한다. process 상실 이후 pending provider effect에는 자동 재dispatch 대신 reconciliation이 필요하다.
