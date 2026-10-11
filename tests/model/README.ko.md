<!-- neograph-i18n: source=tests/model/README.md locale=ko source_sha256=ebc1899d90bb82a927d22ab53185e0a8eb158ece9982be9b3b138d329fc47be1 -->
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
