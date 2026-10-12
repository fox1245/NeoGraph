<!-- neograph-i18n: source=bindings/python/tests/program_e2e/README.md locale=ko source_sha256=de091620c868e73d05f56031616a513cfb2e77ee896820b52d4a10cd0d472b3e -->
# Program API 엔드투엔드 시나리오

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

이 테스트들은 [Python 가이드](../../../../docs/python-binding.md)를 읽은 사용자가 하는 방식 그대로 Python 바인딩을 통해 sealed-v1 JavaScript Program API를 실행합니다. Python `GraphNode` 클래스, 등록된 조건, `ProgramRegistryBuilder` 스냅샷, `ProgramCompiler`, `LocalProgramHost`를 사용합니다. Program과 QuickJS 제어를 켜고 빌드한 `neograph_engine`이 필요하며, 둘 중 하나라도 없으면 이 패키지는 건너뜁니다.

## 범위

| 파일 | 시나리오 |
|---|---|
| `test_01_quickstart.py` | 문서의 예제, Python 노드와 조건을 쓰는 `define()` 그래프, 프로세스 간 식별자, 활성화와 롤백 |
| `test_02_hitl.py` | `interruptBefore`, `NodeInterrupt`, 기본 및 명시적 pending id, `main()` 안의 인터럽트, 프로세스 재시작 후 재개 |
| `test_03_failures.py` | 노드 예외, fan-out 분기 실패, `main()`의 throw, 실패한 `callCore`, 공급자 없는 `llm_call` |
| `test_04_budgets.py` | 컴파일 시점과 실행 시점의 JavaScript 시간 초과, 스텝 및 연산 예산, 벽시계 시간 취소, 승인된 예산 |
| `test_05_diagnostics.py` | 구문 오류와 빌더 오류의 소스 위치, 레지스트리 누락, 린트 스팬, 제어 문자, 승인 오류 |
| `test_06_graph_shapes.py` | 배리어 조인, 조건부 라우팅, `Send` 맵-리듀스, 서브그래프, 잘못된 그래프 형태 |
| `test_07_main_programs.py` | `callCore`, `all`, `race`, `quorum`, `await` 시간 초과, `cancelScope`, 외부 취소, `spawn` |
| `test_08_live_llm.py` | 노드가 모델을 호출하는 Program: 기본은 로컬 피어, 활성화하면 OpenRouter. Python 노드와 바인딩된 `llm_call` |
| `test_09_bound_llm.py` | `node_context`로 바인딩한 `llm_call`(#372): 보고 비용과 추정 비용, 보내기 전 거부(예산, 알 수 없는 모델, router alias), 0 예산, 공급자 없는 Program의 id, 한 예산에 대한 `all`과 `quorum`, 호출이 journal에 기록된 뒤의 크래시와 한 번만 청구하는 복구 |
| `test_10_failure_routing.py` | 실패 라우팅(#364): 실패한 TEST가 레코드와 함께 `interruptBefore` 핸들러로 라우팅됨, retry와 abort 라우트로 재개, 작성자 코드, 가린 오류 텍스트, `max_visits`, `max_node_retries` 상한, 노드 안의 Program 진단, SQLite를 쓰는 새 프로세스에서의 재개 |
| `test_11_attempt_timeout.py` | 시도 타임아웃(#364): 타임아웃된 뒤 재시도에서 회복하는 느린 Python 노드, 항상 타임아웃되어 `NG.Timeout`으로 핸들러에 라우팅되는 노드(핸들러가 없으면 실행 실패), 결과와 트레이스에 나타나지 않는 늦은 완료, 정지 마감 시각을 넘겨 취소를 무시하는 노드(`NG.EffectUncertain`), `wall_time_ms`를 넘게 될 백오프가 kind `deadline`으로 멈추는 경우 |
| `test_12_send_interrupts.py` | `NodeInterrupt`를 던지는 `Send` target(#385): Program이 그 target에서 일시 정지함, 재개 시 map 노드와 끝난 Send가 재생됨, SQLite를 쓰는 재시작 뒤의 재개, 바인딩된 `llm_call`을 호출하고 끝난 Send는 다시 보내지도 다시 과금되지도 않음 |

`support.py`에는 공유하는 Python 노드, 조건, 레지스트리, 예산, JavaScript 빌더가 있습니다.

## 실행

저장소 루트에서, Python 바인딩과 Program, QuickJS 제어를 켠 빌드로 실행합니다.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEOGRAPH_BUILD_PYBIND=ON \
  -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON
cmake --build build
PYTHONPATH=build:bindings/python python -m pytest -q bindings/python/tests/program_e2e
```

오프라인 시나리오는 약 20초가 걸립니다. `-m "not slow"`를 주면 의도적으로 1초 이상 기다리는 시나리오를 건너뜁니다. CI는 모든 push와 pull request마다 네이티브 Linux 레인(`scripts/verify_ci.py native-linux`)에서 `bindings/python/tests/`의 일부로 이 시나리오들을 실행합니다.

## 라이브 모델 테스트

`test_live_openrouter_model_call_inside_a_program`과 `test_live_openrouter_bound_llm_call_is_priced`는 각각 유료 OpenRouter 요청을 한 번 보냅니다(`max_output_tokens=64`, 데이터 무보존 라우팅). 두 번째 테스트는 `LocalProgramHost(node_context=...)`로 바인딩한 내장 `llm_call`을 1,000,000 microunit(1 USD) 금액 예산으로 실행하고 정산된 기록을 확인합니다. 두 변수가 모두 설정되지 않으면 건너뛰며, CI는 이 변수들을 설정하지 않습니다.

```bash
NEOGRAPH_E2E_LIVE=1 OPENROUTER_API_KEY=... PYTHONPATH=build:bindings/python \
  python -m pytest -q -rs bindings/python/tests/program_e2e/test_08_live_llm.py
```

키는 환경 변수에서만 읽습니다. `OPENROUTER_MODEL`, `OPENROUTER_PRICED_MODEL`(카탈로그 항목에 route ceiling이 있는 모델, 기본값 `deepseek/deepseek-v4-flash-0731`), `OPENROUTER_BASE_URL`로 모델과 엔드포인트를 바꿀 수 있습니다.

## 알려진 이슈

의도한 동작은 정해졌지만 아직 구현되지 않은 시나리오에는 엄격한(strict) `xfail`인 `support.known_issue("#NNN", reason)`를 붙입니다. 수정이 들어오면 테스트가 통과하고, strict 모드는 이를 실패로 보고하므로 같은 변경에서 마커를 제거해야 합니다. 현재 실패를 확인하려면 다음을 실행합니다.

```bash
PYTHONPATH=build:bindings/python python -m pytest -q --runxfail -m known_issue bindings/python/tests/program_e2e
```
