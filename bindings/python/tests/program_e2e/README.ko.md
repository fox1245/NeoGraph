<!-- neograph-i18n: source=bindings/python/tests/program_e2e/README.md locale=ko source_sha256=4b60f3f5da8c193b45612166c7cfad3b9a22a09a0ca1a44d4feba1699c77fd43 -->
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
| `test_08_live_llm.py` | 노드가 모델을 호출하는 Program: 기본은 로컬 피어, 활성화하면 OpenRouter |

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

`test_live_openrouter_model_call_inside_a_program`은 유료 OpenRouter 요청을 한 번 보냅니다(`max_output_tokens=64`, 데이터 무보존 라우팅). 두 변수가 모두 설정되지 않으면 건너뛰며, CI는 이 변수들을 설정하지 않습니다.

```bash
NEOGRAPH_E2E_LIVE=1 OPENROUTER_API_KEY=... PYTHONPATH=build:bindings/python \
  python -m pytest -q -rs bindings/python/tests/program_e2e/test_08_live_llm.py
```

키는 환경 변수에서만 읽습니다. `OPENROUTER_MODEL`과 `OPENROUTER_BASE_URL`로 모델과 엔드포인트를 바꿀 수 있습니다.

## 알려진 이슈

의도한 동작은 정해졌지만 아직 구현되지 않은 시나리오에는 엄격한(strict) `xfail`인 `support.known_issue("#NNN", reason)`를 붙입니다. 수정이 들어오면 테스트가 통과하고, strict 모드는 이를 실패로 보고하므로 같은 변경에서 마커를 제거해야 합니다. 현재 실패를 확인하려면 다음을 실행합니다.

```bash
PYTHONPATH=build:bindings/python python -m pytest -q --runxfail -m known_issue bindings/python/tests/program_e2e
```
