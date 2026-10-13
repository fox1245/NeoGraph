<!-- neograph-i18n: source=bindings/python/tests/program_e2e/README.md locale=ko source_sha256=76b292dbdaebc0e300c9c939e569845c546cea947e8e2fcdb7373e08e5125ea1 -->
# Program API 엔드투엔드 시나리오

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

이 테스트들은 사용자가 [Python 가이드](../../../../docs/python-binding.md)를 읽은 다음 하는 방식 그대로 Python 바인딩을 거쳐 sealed-v1 JavaScript Program API를 구동합니다. 사용하는 것은 Python `GraphNode` 클래스, 등록된 조건, `ProgramRegistryBuilder` 스냅샷, `ProgramCompiler`, `LocalProgramHost`입니다. Program과 QuickJS control을 켜고 빌드한 `neograph_engine`이 있어야 하며, 없으면 이 패키지의 테스트는 건너뜁니다.

## 다루는 범위

| 파일 | 시나리오 |
|---|---|
| `test_01_quickstart.py` | 문서에 있는 예제, Python 노드와 조건이 있는 `define()` 그래프, 프로세스를 넘나드는 identity, 활성화(activation)와 롤백 |
| `test_02_hitl.py` | `interruptBefore`, `NodeInterrupt`, 기본 pending id와 명시적 pending id, `main()` 안의 인터럽트, 프로세스를 재시작한 뒤의 재개 |
| `test_03_failures.py` | 노드가 예외를 던지는 경우, fan-out 분기가 실패하는 경우, `main()`이 예외를 던지는 경우, 실패한 `callCore`, provider 없는 `llm_call` |
| `test_04_budgets.py` | 컴파일 시점과 실행 시점의 JavaScript 타임아웃, 스텝 및 연산 예산, wall-time 취소, 승인(admission)된 예산, Core 스텝이나 wall time 허용량을 넘긴 채 끝난 실행의 기록(#443), 원본 실행의 예산에서 떼어 낸 포크의 몫(#414) |
| `test_05_diagnostics.py` | 문법 오류와 builder 오류의 소스 위치, registry miss, lint span, 제어 문자, 승인(admission) 오류 |
| `test_06_graph_shapes.py` | barrier join, 조건부 라우팅, `Send` map-reduce, 서브그래프, 잘못된 그래프 형태 |
| `test_07_main_programs.py` | `callCore`, `all`, `race`, `quorum`, `await` 타임아웃, `cancelScope`, 외부 취소, `spawn` |
| `test_08_live_llm.py` | 노드가 모델을 호출하는 Program(기본은 로컬 peer, 활성화하면 OpenRouter), Python 노드와 바인딩된 `llm_call` |
| `test_09_bound_llm.py` | `node_context`로 바인딩한 `llm_call`(#372): 보고된 비용과 추정 비용, `provider.max_price`로 고정한 route 상한, 전송 전 거부(예산, 알 수 없는 모델, 라우터 별칭), 완료된 HTTP 4xx 응답은 hold를 풀고 아무것도 청구하지 않지만 429나 503은 hold를 유지하는 동작, 예산 0, provider 없이 쓴 id, 하나의 예산에 대한 `all`과 `quorum`, 호출이 저널에 기록된 뒤 발생한 크래시와 그 비용을 한 번만 청구하는 복구, 모델 토큰 예산이 소진될 때의 하나의 종료 모양(#420), 금액 허용량을 넘긴 채 끝난 실행이나 토큰과 금액을 함께 넘긴 실행의 기록(#443), 실행 lease에 묶인 실행의 오류(#421), 중간에 끊긴 스트림(#426): 실패한 run이 terminal 상태를 발행하고 다시 연결되며, native archive가 없는 호스트는 이식 가능한 증거를, archive가 있는 호스트는 native 증거를 보관 |
| `test_10_failure_routing.py` | 실패 라우팅(#364): 실패한 TEST를 기록과 함께 `interruptBefore` 핸들러로 라우팅, 재시도 라우트와 중단 라우트를 따른 재개, 작성자 정의 코드, 가려진(redacted) 오류 텍스트, `max_visits`, `max_node_retries` 상한과 `main()`이 있을 때와 없을 때의 남은 값(#419), 상한과 `main()`을 함께 써도 재시도를 마친 실패가 핸들러에서 멈추는 경우(#418), 노드 안의 Program 진단, SQLite를 쓴 새 프로세스에서의 재개 |
| `test_11_attempt_timeout.py` | 시도 타임아웃(#364): 타임아웃이 나고 재시도에서 복구되는 느린 Python 노드, 항상 타임아웃이 나서 `NG.Timeout`으로 핸들러에 라우팅되는 노드(핸들러가 없으면 실행이 실패), 결과나 트레이스에 전혀 반영되지 않는 늦은 완료, 중지 기한이 지나도록 취소를 무시하는 노드(`NG.EffectUncertain`), `wall_time_ms`를 넘길 백오프가 `deadline` 종류로 실행을 멈추는 경우 |
| `test_12_send_interrupts.py` | `NodeInterrupt`를 발생시키는 `Send` 대상(#385): Program이 그 지점에서 일시 중지되고, 재개하면 map 노드와 끝난 Send를 재생(replay)하며, SQLite로 재시작한 뒤에도 재개할 수 있고, 바인딩된 `llm_call`을 호출한 뒤 끝난 Send는 다시 보내지도 다시 청구하지도 않는다 |
| `test_13_fork_replays_the_paused_step.py` | 바인딩된 `llm_call`이 이미 끝난 상태로 일시 중지된 실행의 `host.fork()`(#390): fork가 질문에 응답하고, 끝난 호출은 그 스텝의 pending write에서 재생(replay)하므로 요청을 다시 보내지 않으며 토큰과 금액도 다시 청구하지 않는다. fan-out 스텝과 Send 모두에 대해, 메모리와 SQLite 저널에서 확인한다 |
| `test_14_await_deadline.py` | `ng.await(..., timeout_ms)`는 절대 기한을 유지한다(#391): 기한 안에 재개하면 완료되고, 기한 뒤에 재개하면 즉시 `P_AWAIT_TIMEOUT`으로 타임아웃된다(메모리, SQLite, 재시작한 호스트에서). 재개된 await 안에서 크래시가 난 뒤의 `reconnect()`는 그 await가 시작할 때의 기한을 적용한다 |
| `test_15_send_isolation.py` | `Send` payload는 자기 `Send`에만 보인다(#389): Send 하나와 세 개, 워커 한 개와 네 개, 라이브 상태와 SQLite에서 재개한 뒤 모두에서 다음 스텝이나 최종 상태에 payload가 남지 않는다 |
| `test_16_interrupt_identity.py` | 한 스텝의 Core 인터럽트 두 개와, 이미 끝난 바인딩된 `llm_call`(#388): pending 입력이 첫 번째 Core 인터럽트를 가리키고, 재개하면 그 인터럽트에 응답하며 두 번째 노드는 자기 질문을 다시 하고, 다음 재개가 그 질문에 응답한다. 끝난 호출은 다시 보내거나 다시 청구하지 않는다(메모리와 SQLite 저널) |
| `test_17_resume_value_scope.py` | 응답은 그 응답이 답하는 질문에만 전달된다(#393): 바인딩된 `llm_call` 뒤에 `first`가 질문하고 다음 Core 스텝에서 `second`가 질문한다. `first`에 응답하는 재개는 `second`가 자기 질문을 하도록 두고, 다음 재개가 그 질문에 응답한다. 끝난 호출은 다시 보내거나 다시 청구하지 않는다(메모리와 SQLite 저널) |
| `test_18_step_failure_outranks_interrupt.py` | 질문하는 노드의 인덱스와 상관없이 실패가 그 스텝을 끝낸다(#392): 한 Core 스텝에서 바인딩된 `llm_call`, 질문하는 노드, 실패하는 노드가 있을 때 두 순서 모두에서 Program은 실패한 노드의 분류된 실패로 끝나고 남아 있는 질문은 없으며, 모델 호출은 한 번만 보내고 한 번만 청구된다(메모리와 SQLite 저널) |
| `test_19_fork_interrupt_identity.py` | Core 스텝이 인터럽트 두 개로 일시 중지되고 이미 끝난 바인딩된 `llm_call`이 있는 실행의 `host.fork()`(#394): fork는 pending 호출의 `core_interrupt_id`가 가리키는 인터럽트에 응답하고, 다른 노드는 fork에서 다시 질문한다. 끝난 호출은 다시 보내거나 다시 청구하지 않는다(메모리와 SQLite 저널) |
| `test_20_javascript_fork.py` | 각 callCore에서 바인딩된 `llm_call`을 호출한 뒤 두 번째 `callCore`에서 일시 중지된 JavaScript `main()`의 `host.fork()`(#395): fork는 첫 번째 command를 command 저널에서 재생(replay)하고, 일시 중지된 호출을 응답과 함께 재개해 완료한다. 두 모델 호출 모두 다시 보내거나 다시 청구하지 않는다(메모리와 SQLite 저널) |
| `test_21_fork_recorded_failure.py` | 결과를 모으는 `ng.all`이, 바인딩된 `llm_call`이 거부된 멤버를 기록한 JavaScript `main()`의 `host.fork()`(#398): fork는 기록된 provider 증거를 자기 자신에 다시 바인딩하고, 일시 중지된 멤버에 응답하며, 기록된 실패를 증거와 함께 보고한다. 어느 호출도 다시 보내지 않는다(메모리와 SQLite 저널) |
| `test_22_fork_provider_journal_notice.py` | `host.fork()`와 provider 호출 저널(#431): 끝난 바인딩 `llm_call` 옆에서 일시 중지된 실행을 메모리에서 fork하면, 반환된 핸들의 `notices`가 기록된 호출을 알리고(`P_FORK_PROVIDER_CALLS_NOT_JOURNALED`), fork는 알림이 없을 때와 똑같이 전송하고 과금하며 완료된다. SQLite 저널을 쓸 때와 provider 호출이 없던 실행의 fork에는 알림이 없고, `start()`와 `resume()`의 핸들에도 없다 |

`support.py`에는 공용 Python 노드, 조건, registry, 예산, JavaScript builder가 들어 있습니다.

## 실행

저장소 루트에서, Python 바인딩, Program, QuickJS control을 켜고 빌드한 환경에서 실행합니다.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEOGRAPH_BUILD_PYBIND=ON \
  -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON
cmake --build build
PYTHONPATH=build:bindings/python python -m pytest -q bindings/python/tests/program_e2e
```

오프라인 시나리오는 약 20초가 걸립니다. `-m "not slow"`를 주면 일부러 1초 이상 기다리는 시나리오를 건너뜁니다. CI는 네이티브 Linux 레인(`scripts/verify_ci.py native-linux`)에서 `bindings/python/tests/`의 일부로 push와 pull request마다 이 시나리오를 실행합니다.

## 라이브 모델 테스트

`test_live_openrouter_model_call_inside_a_program`과 `test_live_openrouter_bound_llm_call_is_priced`는 각각 OpenRouter에 유료 요청을 한 번씩 보냅니다(`max_output_tokens=64`, 데이터 보존 없음(zero data retention) 라우팅). 두 번째 테스트는 내장 `llm_call`을 `LocalProgramHost(node_context=...)`로 바인딩해 1,000,000 마이크로 단위(1 USD)의 금액 예산 안에서 실행하고, 정산된 기록을 확인합니다. 이 요청은 카탈로그의 route 상한을 `provider.max_price`로 싣고 가므로, OpenRouter는 상한을 넘는 엔드포인트로 라우팅하지 않고 요청을 실패시킵니다. 아래 두 변수가 모두 설정되어 있지 않으면 이 테스트들은 건너뛰며, CI는 이 변수를 설정하지 않습니다.

```bash
NEOGRAPH_E2E_LIVE=1 OPENROUTER_API_KEY=... PYTHONPATH=build:bindings/python \
  python -m pytest -q -rs bindings/python/tests/program_e2e/test_08_live_llm.py
```

키는 환경 변수에서만 읽습니다. `OPENROUTER_MODEL`, `OPENROUTER_PRICED_MODEL`(카탈로그 항목에 route 상한이 있는 모델이며 기본값은 `deepseek/deepseek-v4-flash-0731`), `OPENROUTER_BASE_URL`로 모델과 엔드포인트를 바꿀 수 있습니다.

## 알려진 문제

의도한 동작은 정해졌지만 아직 구현되지 않은 시나리오에는 strict `xfail`인 `support.known_issue("#NNN", reason)` 표시를 붙입니다. 수정이 들어가면 테스트가 통과하는데, strict 모드는 이를 실패로 보고하므로 같은 변경에서 표시를 제거해야 합니다. 현재의 실패를 보려면 다음을 실행합니다.

```bash
PYTHONPATH=build:bindings/python python -m pytest -q --runxfail -m known_issue bindings/python/tests/program_e2e
```
