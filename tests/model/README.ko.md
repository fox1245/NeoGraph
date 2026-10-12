<!-- neograph-i18n: source=tests/model/README.md locale=ko source_sha256=ff8e590d7785c7ce1b9c737b233df1d9367d921ad10e276d55d2565169d2c88d -->
# Join 모델 (#363)

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`BarrierJoin.tla`는 BSP super-step(슈퍼스텝)과 AND join 하나를 모델링합니다. 이 join의 `wait_for`
slot에는 여러 `any_of` 대안(alternative)이 들어갈 수 있으며, 이것이 #363에서 승인된 범위입니다(join
모드는 보류되었습니다). `tests/test_join_exhaustive.cpp`는 같은 규칙을 참조 인터프리터로 기술하고,
더 큰 그래프에서 실제 `Scheduler`와 `GraphEngine`을 이 인터프리터와 대조해 검사합니다. 의미론(semantics)이
바뀌면 두 쪽을 함께 맞춰 두세요.

## 규칙

- slot은 그 대안 중 하나라도 join에 신호(signal)를 보내면 채워집니다. join은 모든 slot이 채워지면
  발화(fire)하고, 그 뒤 누적된 신호를 모두 비웁니다.
- 멤버가 아닌 노드가 보낸 신호는 기록되지만 절대 집계되지 않습니다.
- 조건부 라우트는 비결정적인 레이블 선택입니다.
- `Command.goto(__end__)`는 해당 super-step의 모든 라우트를 선점합니다.
- 준비된 것이 없으면 실행(run)이 끝납니다. join이 아직 신호를 보유하고 있으면(멤버가 보낸 것이든
  아니든) 실행은 정체(stall) 상태이며 `Failed`가 됩니다. 단, 마지막 super-step(그 이후로 준비된 것이
  없는 super-step)이 명시적 라우트로 `__end__`에 도달했다면 예외입니다. 명시적 라우트란 `__end__`로
  가는 정적 edge(`.exit()`가 기록하는 것), `__end__`로 가는 조건부 라우트, `Command.goto(__end__)`입니다.
  암묵적 종료(후속 노드가 없는 노드)는 정체를 절대 억제하지 않으며, 더 이른 super-step의 명시적 종료도
  억제하지 않습니다.

따라서 남은 `any_of` 대안이 join이 exit하는 super-step에 도착하면 실행은 완료되고, 한 super-step 뒤에
도착하면서 그 밖에 준비된 것이 없으면 실행은 실패합니다.

C++ 하네스는 실패 라우트(#364)도 다룹니다. failure edge를 따라 실패한 노드는 그 edge의 대상으로만 라우팅되며, `Command.goto`는 여전히 이보다 우선합니다. 실패 대상은 조인이나 `__end__`가 될 수 없으므로 위 규칙은 그대로 성립하며, 이 TLA+ 모델은 failure edge를 포함하지 않습니다.

## 불변식

| 이름 | 의미 |
|---|---|
| `NoDoubleFirePerRound`, `JoinRunsOnlyWhenFired`, `FireClearsSignals` | 모든 발화는 직전 발화 이후 도착해 모든 slot을 채운 신호를 소비합니다. join은 발화 직후에만 실행되며, 발화는 모든 신호를 비웁니다. |
| `StallImpliesFailed`, `FailedOnlyOnStall` | join에 신호가 대기 중인 채로 끝나는 실행은 실패하며, 그런 실행만 실패합니다. |
| `OnlyExplicitEndSuppressesStall` | 신호가 대기 중인 채로 완료되는 실행은 마지막 super-step에서 `__end__`에 명시적으로 도달한 실행입니다. |
| `LeftoverAlternativeStalls` | join이 발화한 뒤 도착한 대안은, 이후 발화가 없고 마지막 super-step에 명시적 종료도 없으면 실행을 실패시킵니다(fail-closed). |
| `NamedOutcomes` | 서로 배타적이지 않은 #357의 두 변형입니다. 남은 대안이 exit와 함께 도착하면 완료되고, 한 super-step 늦게 도착하면 실패합니다. |
| `LegacyEquivalence` | slot이 모두 단일 원소(singleton)이면 slot 규칙은 현재의 barrier와 같습니다. 즉 `wait_for`가 수신한 신호의 부분집합이어야 합니다. |
| `Termination` (속성) | 모든 실행은 완료되거나, 실패하거나, step 한도에 도달합니다. |

## TLC 실행

TLC는 CI에 포함되지 않습니다. Java 11 이상과 TLA+ 릴리스(<https://github.com/tlaplus/tlaplus/releases>)의
`tla2tools.jar`가 필요합니다.

```sh
cd tests/model
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC \
    -workers auto -config BarrierJoin.cfg BarrierJoin.tla
```

`BarrierJoin.cfg`는 노드가 최대 세 개인 모든 그래프를 join 멤버의 모든 `any_of` 분할과 함께 검사하고,
spec에서 이름을 붙인 #357 그래프(legacy barrier, `any_of` slot, 서로 배타적이지 않은 두 변형,
`Command.goto`에 의한 명시적 종료)도 검사합니다. TLC는 서로 다른 상태 335,521개를 찾으며 오류는
없습니다. worker 8개에서 걸리는 약 2분의 대부분은 초기 상태 21,707개를 계산하는 데 쓰입니다.
`MaxNodes = 4`는 TLC로는 감당할 수 없으며, C++ harness가 노드 다섯 개까지 다룹니다.

불변식이 실제로 위반을 잡아내는지 보려면, 암묵적 종료가 정체를 억제하도록 바꾸거나(`StallImpliesFailed`가
실패합니다), 아무 멤버 신호로나 join이 발화하도록 바꾸세요(`NoDoubleFirePerRound`가 실패합니다).

## 재개 모델

`tests/resume_model.h`와 `tests/test_resume_model.cpp`는 join 하네스가 join을 검사하듯
`GraphEngine`의 인터럽트, 재개, 재생 계약을 참조 인터프리터와 대조해 검사합니다.
`tests/test_program_resume_model.cpp`는 Program 런타임에 대해 같은 일을 합니다. 참조
인터프리터는 문서화된 계약(`docs/concepts.md`)을 기술합니다. super-step은 준비된 노드를
실행한 뒤 그 노드들의 Send를 실행합니다. 호출이 질문하는 step은 그 모두에서 일시 정지하고 끝난
것은 보존합니다. 재개는 지정한 인터럽트, 또는 첫 인터럽트에 답하고, 그 호출만 값을 받습니다
(#388, #393). `interrupt_before` 승인은 그 게이트가 붙잡은 노드에 전달되고, step의 각 게이트는
따로 답하며, 답한 게이트만 통과합니다(#382, #396). 실패는 같은 step의 일반 인터럽트보다
우선합니다(#392).

### Core 하네스

- **그래프 집합.** 주 노드 `n0`..`n{N-1}`은 앞으로 가는 간선(노드마다 후속 노드 최대 두 개)으로
  이어지고 모든 노드에 도달할 수 있습니다. 노드는 `Ok`, `Ask`(답을 받을 때까지 `NodeInterrupt`를
  던지며, 후속 노드가 둘이면 답에 따라 경로를 고를 수 있습니다), `Fail`(실패 간선이 있거나 없음,
  #364), `Flaky`(실패한 뒤 자기 attempt 네임스페이스의 재시도가 성공함) 또는 `Map`(항목이
  `Ok`, `Ask`, `Fail`인 worker로 가는 Send 한두 개)입니다. 정적 선행 노드가 둘인 마지막 노드는
  barrier join일 수 있고, `Ok`나 `Ask` 노드 하나에 `interrupt_before`나 `interrupt_after`를 둘
  수 있습니다(확장 범위에서는 `interrupt_before` 게이트 둘).
- **스케줄.** 참조 인터프리터의 모든 답 순서: 대기 중인 각 질문을 인터럽트 id로, 그리고 기본값으로
  답하며, 답에 따라 경로를 고르는 `Ask`에는 두 라벨을 모두 줍니다. 각 스케줄은 모든 내구 연산
  (체크포인트 `save`와 `put_writes` 각각; 각 연산 앞과 마지막 연산 뒤, 확장 범위에서는 각 연산의
  앞과 뒤이며 복구 중 두 번째 크래시도 냅니다)에서의 크래시와, 모든 일시 정지에서의 포크로도
  실행됩니다. 크래시는 프로세스를 멈춥니다. 저장소는 이후의 모든 연산을 거부하고, 호스트는
  실제 호스트처럼 최신 체크포인트에서 복구합니다. 그 체크포인트가 자신이 답하던 것이면 실행하던
  재개를 다시 하고, 보지 못한 `interrupt_before`나 `interrupt_after` 일시 정지는 질문으로 올리며,
  그 밖에는 값 없이 재개하고, 체크포인트가 없는 스레드는 다시 실행합니다. 모든 스케줄은
  `InMemoryCheckpointStore`에서 실행되고, 거친 시계(모든 체크포인트를 같은 타임스탬프로 저장,
  `tests/fixtures/coarse_clock_store.h`)를
  쓰는 `SqliteCheckpointStore`에서는 크래시 없이 실행됩니다(노드 두 개, 확장 범위는 세 개까지의
  그래프는 크래시도 냅니다).
- **Ask.** 모든 스케줄을 두 번 실행합니다. 자기 호출 id로 보낸 답만 받는 Ask와, 승인 노드처럼
  어떤 재개 값이든 받는 Ask입니다.
- **성질.** P1 정확히 한 번: 완료가 내구 상태가 된 뒤(대기 중 쓰기, 또는 그 step의 커밋)에는
  노드 본문이 다시 완료되지 않습니다. 본문은 크래시가 그 완료를 잃은 뒤에만 다시 실행됩니다.
  P2 모든 질문을 묻는다: 일시 정지와 대기 중인 질문, 그 payload가 참조와 같고, 각 `Ask`는 자신이
  받은 답을 정확히 소비합니다. P3 같은 결과: 상태, 로그, 실패 기록, 경로가 참조와 같습니다.
  P4 크래시 투명성과 P5 포크 동등성: 모든 크래시 스케줄과 포크 스케줄에서 P1-P3가 성립합니다.

### Program 하네스

JavaScript `main()` 프로그램(`callCore`, 멤버 둘을 함께 또는 하나씩 실행하는 `ng.all`,
`ng.race`, `ng.quorum`, `ng.await`, 그리고 Core step에서 둘 다 질문할 수 있는 노드 두 개를 실행하는
`callCore`, #388과 #394)과 타입 있는 plan(`parallel`, 셋 중 둘의 `quorum`,
`sequence`)을, 바인딩된 provider를 호출하고 질문할 수 있는 노드 하나짜리 Core 위에서 실행합니다.
호스트는 `LocalProgramHost`와 같은 방식으로 구성합니다. 프로세스 내 가격 브로커를 쓰는 메모리
호스트, 또는 모든 저장소를 한 SQLite 파일에 두고 체크포인트를 같은 거친 시계로 저장하며 SQLite
provider 저널을 쓰는 호스트입니다.
provider는 보낸 요청을 세는 로컬 전송 위의 실제 SDK 클라이언트입니다. P1은 "한 번 보내고 한 번
청구한다"도 다룹니다. 요청 수와 run에 청구된 토큰과 금액이 참조와 같습니다(프로세스 내
브로커에서는 문서대로 다시 실행되는 노드가 다시 호출하고 다시 지불합니다). 크래시는 한 번에 호출
하나를 실행하는 프로그램에 대해 SQLite 호스트에서 냅니다. 데이터베이스를 모든 체크포인트 저장,
`put_writes`, Program 전이, provider dispatch, 저널에 기록된 호출에서 복사하고, 복사본 위의 새
런타임이 run에 다시 연결합니다.

Core 체크포인트를 저장한 호출은 크래시 뒤 그 체크포인트에서 이어지고(#402), 포크는 일시 정지한
호출이 한 provider 호출을 재생합니다(#405). 그렇지 않은 스케줄은 실패합니다. 하네스는 P4가 다루지
않는 두 가지 문서화된 동작을 따르는 스케줄을 실패로 보지 않고 보고합니다. JavaScript 프로그램이든
plan이든 첫 Core 체크포인트 전에 멈춘 호출은 조정을 위해 호스트에 돌아오며(#402, #404), 하네스는
크래시가 그 호출에 체크포인트를 남기지 않았을 때만 이를 받아들입니다. 그리고 요청이 나가 있는 동안의
크래시는 그 호출을 보류하며, 그 호출은 다시 보내지지 않습니다(#364). 호스트가 완료로 조정할 수 있는
호출(질문도 실패도 하지 않고 뒤에 가격이 매겨지는 호출이 없는 호출, await한 호출, 쌍)은 참조 결과로
조정하고, 그 뒤 run은 참조와 같이 끝나야 합니다(#403).

### 범위와 변수

| | CI 범위 | `NEOGRAPH_RESUME_MODEL_BOUND=extended` |
|---|---|---|
| 주 노드, 특수 노드 | 3, 2 | 4 (노드 세 개까지 특수 3, 네 개에서 2) |
| 스케줄당 답, 크래시 | 4, 1 | 6, 2 |
| Send 항목 | `ok`, `ask`, `ask ok`, `ask ask`, `ask fail` | 하나 또는 둘의 모든 패턴 |
| 게이트 | `Ok`나 `Ask`에 하나 | 어떤 노드에든 하나, 또는 `interrupt_before` 둘 |

CI 범위에서 Core 하네스는 그래프 2,196개와 Ask 종류마다 답 스케줄 약 6,100개, 각각 약 83,000번의
실행을 다룹니다. Program 하네스는 JavaScript 프로그램 124개와 plan 18개, 약 950개의 스케줄을
실행합니다. 각 테스트는 워커 네 개로 1분 안에 끝납니다.

변수: `NEOGRAPH_RESUME_MODEL_NODES`, `_SPECIAL`, `_SPECIAL_LARGEST`, `_ANSWERS`, `_CRASHES`는 개별
한도를 덮어씁니다. `NEOGRAPH_RESUME_MODEL_SQLITE=0`은 SQLite를 빼고,
`NEOGRAPH_RESUME_MODEL_THREADS`는 워커 수를 정합니다(기본 4). Program 하네스에서는
`NEOGRAPH_PROGRAM_RESUME_MODEL_BOUND=extended`가 명령 둘짜리 프로그램, 모든 포크, 모든 연산 뒤의
크래시를 더합니다. `_ONLY`는 설명에 그 문자열이 든 프로그램만 남기고, `_CRASHES=0`은 크래시를 빼며,
`_THREADS`는 워커 수를 정하고(기본 4), `_DEBUG`는 모든 결과와 내구 연산을 출력합니다.
