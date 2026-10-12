<!-- neograph-i18n: source=tests/model/README.md locale=ko source_sha256=fce8bbe8820b221df14af8afbf931bd239bf19a334f5f3993a7a36a5c34365c6 -->
# Join 모델 (#363)

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`BarrierJoin.tla`는 BSP 슈퍼스텝을 모델링합니다. 모델에는 AND join이 하나 있고, 이 join의
`wait_for` 슬롯에는 `any_of` 대안이 여러 개 들어갈 수 있습니다. 이것이 #363에서 승인된
범위입니다(join 모드는 보류했습니다). `tests/test_join_exhaustive.cpp`는 같은 규칙을 참조
인터프리터로 표현하고, 더 큰 그래프에서 실제 `Scheduler`와 `GraphEngine`이 이 인터프리터와
일치하는지 검사합니다. 의미가 바뀌면 두 곳을 함께 고쳐야 합니다.

## 규칙

- 슬롯은 대안 중 하나라도 join에 신호를 보내면 채워진 것으로 봅니다. 모든 슬롯이 채워지면
  join이 실행되고(fire), 그때까지 쌓인 신호는 모두 지워집니다.
- 멤버가 아닌 쪽에서 온 신호는 기록만 하고 조건으로 세지 않습니다.
- 조건부 라우트는 레이블을 비결정적으로 고르는 것으로 모델링합니다.
- `Command.goto(__end__)`는 같은 슈퍼스텝의 모든 라우트보다 우선합니다.
- 실행 가능한 노드가 없으면 실행이 끝납니다. 이때 join에 신호가 남아 있으면(멤버가 보낸
  신호이든 아니든) 실행은 정체(stalled)되어 `Failed`가 됩니다. 다만 마지막 슈퍼스텝(그 뒤로
  실행 가능한 노드가 없는 슈퍼스텝)이 명시적 라우트로 `__end__`에 도달했다면 예외입니다.
  명시적 라우트는 `__end__`로 가는 정적 엣지(`.exit()`가 쓰는 엣지), `__end__`로 가는 조건부
  라우트, `Command.goto(__end__)`입니다. 암묵적 종료(후속 노드가 없는 노드)는 정체를 막지 못하고,
  이전 슈퍼스텝에서의 명시적 종료도 막지 못합니다.

따라서 join이 빠져나가는 바로 그 슈퍼스텝에 도착한 `any_of` 잔여 대안은 정상적으로 완료되고,
슈퍼스텝 하나 뒤에 도착했는데 다른 실행 가능한 노드가 없으면 실패합니다.

C++ 하네스는 실패 라우트(#364)도 검사합니다. 실패 엣지를 따라 실패한 노드는 그 엣지의 대상으로만
라우팅되고, `Command.goto`가 여전히 그보다 우선합니다. 실패 대상은 join이나 `__end__`가 될 수
없으므로 위 규칙은 그대로 유지됩니다. 이 TLA+ 모델에는 실패 엣지가 없습니다.

## 불변 조건

| 이름 | 의미 |
|---|---|
| `NoDoubleFirePerRound`, `JoinRunsOnlyWhenFired`, `FireClearsSignals` | fire할 때마다 직전 fire 이후에 도착해 모든 슬롯을 채운 신호를 소비한다. join은 fire 직후에만 실행되고, fire하면 모든 신호가 지워진다. |
| `StallImpliesFailed`, `FailedOnlyOnStall` | join에 신호가 남은 채 끝난 실행은 실패하고, 그런 실행만 실패한다. |
| `OnlyExplicitEndSuppressesStall` | 신호가 남은 채 완료된 실행은 마지막 슈퍼스텝에서 명시적으로 `__end__`에 도달한 것이다. |
| `LeftoverAlternativeStalls` | join이 fire한 뒤에 도착한 대안이 있고, 이후 fire가 없으며 마지막 슈퍼스텝에서 명시적 종료도 없으면 실행을 실패 처리한다(실패 시 차단). |
| `NamedOutcomes` | #357의 서로 배타적이지 않은 두 변형이다. 종료와 함께 도착한 잔여 대안은 완료되고, 슈퍼스텝 하나 뒤에 도착하면 실패한다. |
| `LegacyEquivalence` | 슬롯이 모두 항목 하나뿐이면 슬롯 규칙은 기존 barrier와 같다. `wait_for`는 수신한 신호의 부분집합이다. |
| `Termination` (속성) | 모든 실행은 완료되거나, 실패하거나, 스텝 한도에 도달한다. |

## TLC 실행

TLC는 CI에 포함되어 있지 않습니다. Java 11 이상과 TLA+ 릴리스의 `tla2tools.jar`
(<https://github.com/tlaplus/tlaplus/releases>)가 필요합니다.

```sh
cd tests/model
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC \
    -workers auto -config BarrierJoin.cfg BarrierJoin.tla
```

`BarrierJoin.cfg`는 노드가 세 개 이하인 모든 그래프를 검사합니다. 각 그래프에서 join 멤버의
`any_of` 분할을 모두 시도하며, 스펙에 이름이 붙은 #357 그래프(기존 barrier, `any_of` 슬롯,
서로 배타적이지 않은 두 변형, `Command.goto`에 의한 명시적 종료)도 함께 검사합니다. TLC는 서로
다른 상태 335,521개를 찾았고 오류는 없었습니다. 워커 8개로 2분 걸리는 시간의 대부분은 초기 상태
21,707개를 계산하는 데 쓰입니다. `MaxNodes = 4`는 TLC로 감당할 수 없고, C++ 하네스는 노드 다섯
개까지 검사합니다.

불변 조건이 실제로 위반을 잡아내는지 보려면, 암묵적 종료가 정체를 막도록 모델을 바꾸거나
(`StallImpliesFailed`가 실패합니다), 멤버 신호 하나만 와도 join이 fire하도록 바꿔 보면 됩니다
(`NoDoubleFirePerRound`가 실패합니다).

## 재개 모델

`tests/resume_model.h`와 `tests/test_resume_model.cpp`는 join 하네스가 join을 검사하는 것과 같은
방식으로, `GraphEngine`의 인터럽트, 재개, 재생(replay) 계약을 참조 인터프리터와 대조해 검사합니다.
Program 런타임은 `tests/test_program_resume_model.cpp`가 같은 방식으로 검사합니다. 참조
인터프리터에 담은 계약은 문서(`docs/concepts.md`)에 있는 내용입니다. 슈퍼스텝은 실행 가능한
노드를 먼저 실행하고 그다음에 그 노드들의 Send를 실행합니다. 호출들이 질문을 하는 스텝은 그
질문 모두에서 일시 중지되고, 끝난 작업은 보존합니다. 재개는 이름을 지정한 인터럽트에, 지정하지
않으면 첫 번째 인터럽트에 응답하며, 값은 그 호출만 받습니다(#388, #393). `interrupt_before`
승인은 그 승인이 잡고 있던 노드에 전달되고, 한 스텝의 각 게이트는 개별로 응답하며, 응답받은
게이트만 통과합니다(#382, #396). 한 스텝에서 실패는 일반 인터럽트보다 우선합니다(#392).

### Core 하네스

- **그래프 계열.** 주 노드 `n0`..`n{N-1}`이 앞쪽으로만 향하는 엣지로 이어지고(노드마다 후속
  노드는 최대 두 개), 모든 노드에 도달할 수 있습니다. 노드의 종류는 다음과 같습니다. `Ok`,
  `Ask`(응답을 받을 때까지 `NodeInterrupt`를 발생시키며, 후속 노드가 둘이면 응답에 따라 라우팅할
  수 있음), `Fail`(실패 엣지가 있거나 없음, #364), `Flaky`(한 번 실패하고, 재시도는 자기 시도
  네임스페이스에서 성공함), `Map`(항목이 `Ok`, `Ask`, `Fail`인 워커에 Send를 하나 또는 두 개
  보냄)입니다. 정적 선행 노드가 둘인 마지막 노드를 barrier join으로 쓸 수 있고, `Ok` 또는 `Ask`
  노드 하나에 `interrupt_before` 또는 `interrupt_after`를 붙입니다(확장 범위에서는
  `interrupt_before` 게이트가 둘).
- **스케줄.** 참조 인터프리터가 허용하는 모든 응답 순서를 시도합니다. 보류 중인 질문마다
  인터럽트 id를 지정해 응답하는 경우와 id 없이 기본값으로 응답하는 경우를 모두 다루고, 응답에
  따라 라우팅하는 `Ask`는 두 레이블을 모두 시도합니다. 각 스케줄은 영속 연산마다 크래시를 넣어서도
  실행합니다. 대상은 체크포인트 `save`와 `put_writes`이며, 각 연산의 직전과 마지막 연산의
  직후에 크래시를 넣습니다. 확장 범위에서는 각 연산의 직전과 직후에 넣고, 복구 중에 두 번째
  크래시도 넣습니다. 일시 중지 지점마다 fork한 경우도 함께 실행합니다. 크래시는 프로세스를
  멈춥니다. 저장소는 이후의 모든 연산을 거부하고, 호스트는 실제 호스트처럼 가장 최근
  체크포인트에서 복구합니다. 복구한 체크포인트가 자신이 응답했던 체크포인트이면 실행하던 재개를
  반복하고, 아직 보지 못한 `interrupt_before`나 `interrupt_after` 일시 중지가 있으면 드러내며, 그
  밖의 경우에는 값 없이 재개하고, 체크포인트가 없는 스레드는 처음부터 다시 실행합니다. 모든
  스케줄은 `InMemoryCheckpointStore`에서 실행하고, `SqliteCheckpointStore`에서는 크래시 없이
  실행합니다(노드 두 개 이하의 그래프에서는 크래시도 넣고, 확장 범위에서는 세 개 이하). 이때
  clock을 거칠게(coarse) 설정해 모든 체크포인트를 같은 타임스탬프로 저장합니다
  (`tests/fixtures/coarse_clock_store.h`).
- **Ask 동작.** 모든 스케줄은 두 번 실행합니다. 한 번은 자기 호출 id로 지정된 응답만 받는
  Ask로, 다른 한 번은 승인 노드처럼 어떤 재개 값이든 받는 Ask로 실행합니다.
- **검증하는 속성.** P1 정확히 한 번: 완료가 영속화된(pending write 또는 해당 스텝의 커밋)
  뒤에는 어떤 노드 본문도 다시 완료되지 않습니다. 본문이 다시 실행되는 것은 크래시로 완료 기록을
  잃은 경우뿐입니다. P2 모든 질문이 제시됨: 일시 중지와 그 대기 질문(payload 포함)이 참조
  인터프리터와 같고, 각 `Ask`는 자신에게 주어진 응답만 정확히 소비합니다. P3 같은 결과: 상태,
  로그, 실패 기록, 라우트가 참조 인터프리터와 같습니다. P4 크래시 투명성과 P5 fork 동치: 모든
  크래시 및 fork 스케줄에서 P1-P3이 성립합니다.

### Program 하네스

검사 대상은 두 가지입니다. 하나는 JavaScript `main()` program(`callCore`, 멤버가 둘인 `ng.all`,
`ng.race`, `ng.quorum`을 함께 또는 하나씩 실행하는 경우, `ng.await`, 둘 다 질문할 수 있는 노드
두 개를 Core 스텝에서 실행하는 `callCore`, #388과 #394)이고, 다른 하나는 typed plan(`parallel`,
3개 중 2개를 기다리는 `quorum`, `sequence`)입니다. 두 경우 모두 노드가 하나인 Core를 대상으로
하며, 이 노드는 바인딩된 provider를 호출하거나 질문할 수 있습니다. 호스트는 `LocalProgramHost`가
구성하는 방식 그대로 구성합니다. 즉 가격을 계산하는 in-process broker를 쓰는 메모리 구성이거나,
모든 저장소를 SQLite 파일 하나에 두고 체크포인트를 같은 coarse clock으로 저장하며 SQLite provider
저널을 쓰는 구성입니다. provider는 요청 수를 세는 로컬 전송 위에서 동작하는 실제 SDK
클라이언트입니다. P1은 "한 번 보내고 한 번 청구"도 검증합니다. 실행에 청구되는 요청 수, 토큰,
금액이 참조 인터프리터와 같아야 합니다(문서에 적힌 대로, in-process broker에서는 다시 실행되는
노드가 다시 호출하고 다시 비용을 냅니다). 크래시 검사는 호출을 한 번에 하나씩 하는 program을
SQLite 호스트에서 실행합니다. 체크포인트 저장, `put_writes`, Program 전이, provider dispatch,
저널에 기록된 호출이 일어날 때마다 데이터베이스를 복사하고, 복사본 위에서 새 런타임이 실행을
다시 연결합니다.

JavaScript program 네 개는 실패 라우팅(#364, #418)을 실행합니다. 노드가 실패하고, 0번 또는 2번
재시도한 뒤 `interruptBefore` 핸들러로 라우팅되는 경우로, `callCore`가 실행하는 Core에서 run의
재시도 상한(`max_node_retries`)이 있을 때와 없을 때를 모두 봅니다. 참조 인터프리터는 재시도와 상한이
일시 정지를 바꾸지 않는다고 규정합니다. 질문이 하나 나오고, 그 뒤 핸들러가 실행되며 `main()`이
끝납니다.

Core 체크포인트를 저장한 호출은 크래시 뒤에 그 체크포인트에서 이어지고(#402), fork는 일시 정지한
호출이 이미 한 provider 호출을 재생합니다(#405). 이와 다르게 동작하는 스케줄은 실패로 처리합니다.
하네스는 P4가 다루지 않는 문서화된 동작 세 가지를 따르는 스케줄을 실패 처리하지 않고 보고합니다.
하나는 JavaScript 프로그램이든 plan이든 첫 Core 체크포인트 전에 멈춘 호출을 조정하도록 호스트에
돌려주는 동작입니다(#402, #404). 하네스는 크래시가 그 호출에 체크포인트를 남기지 않았을 때만 이를
받아들입니다. 또 하나는 `interrupt_before` 게이트(실패 라우팅 program)에서 멈춘 호출도 같은 방식으로
돌려주는 동작입니다. 복구는 응답받지 않은 게이트를 지나가지 않기 때문입니다(#402). 나머지 하나는
요청이 나가 있는 동안 크래시가 나면 그 호출을 보류(hold)하고 다시 보내지 않는 동작입니다(#364). 호스트가 완료로 조정할 수 있는 호출(질문도 실패도 하지 않는 호출, await한
호출, 호출 쌍 가운데 뒤에 과금되는 호출이 없는 것)은 참조 결과로 조정하며, 그 뒤 실행은 참조와 같게
끝나야 합니다(#403).

### 범위와 변수

| | CI 범위 | `NEOGRAPH_RESUME_MODEL_BOUND=extended` |
|---|---|---|
| 주 노드, 특수 노드 | 3, 2 | 4 (노드 3개 이하는 특수 노드 3, 4개는 특수 노드 2) |
| 스케줄당 응답, 크래시 | 4, 1 | 6, 2 |
| Send 항목 | `ok`, `ask`, `ask ok`, `ask ask`, `ask fail` | 항목 하나 또는 둘로 만들 수 있는 모든 패턴 |
| 게이트 | 하나, `Ok` 또는 `Ask`에 부착 | 아무 노드에나 하나, 또는 `interrupt_before` 둘 |

CI 범위에서 Core 하네스는 그래프 2,196개를 실행하며, Ask 종류마다 응답 스케줄이 약 6,100개,
각각 약 83,000회 실행됩니다. Program 하네스는 JavaScript program 128개와 plan 18개, 스케줄
약 1,010개를 실행합니다. 각 테스트는 워커 4개로 1분 안에 끝납니다.

환경 변수는 다음과 같습니다. `NEOGRAPH_RESUME_MODEL_NODES`, `_SPECIAL`, `_SPECIAL_LARGEST`,
`_ANSWERS`, `_CRASHES`는 개별 한도를 바꿉니다. `NEOGRAPH_RESUME_MODEL_SQLITE=0`은 SQLite를
제외하고, `NEOGRAPH_RESUME_MODEL_THREADS`는 워커 수를 정합니다(기본값 4). Program 하네스에서는
`NEOGRAPH_PROGRAM_RESUME_MODEL_BOUND=extended`가 command 두 개짜리 program, 모든 fork, 모든 연산
뒤의 크래시를 추가합니다. `_ONLY`는 설명에 그 문자열이 들어 있는 program만 남기고, `_CRASHES=0`은
크래시를 제외하며, `_THREADS`는 워커 수를 정하고(기본값 4), `_DEBUG`는 모든 결과와 영속 연산을
출력합니다.
