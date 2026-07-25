<!-- neograph-i18n: source=PYBIND11_HANDOFF.md locale=ko source_sha256=ef50f7757126e94faa6a671e4cc22a02c5c49481f94e6d0652d812d1c4734b07 -->
# Pybind11 바인딩 — 다음 세션 인계

**Languages:** [English](PYBIND11_HANDOFF.md) | [한국어](PYBIND11_HANDOFF.ko.md) | [日本語](PYBIND11_HANDOFF.ja.md) | [简体中文](PYBIND11_HANDOFF.zh-CN.md)

> **역사적 인계:** 여기 기술된 바인딩은 이미 배포되었다. 현재
> 사용자 정의 Python 노드는 을 구현하며, v0.9.0에서 기존 `run(input)` `execute*`
> `docs/python-binding.md` 노드 표면이 제거되었다. 지원되는 API는
> 와 를 참조. `docs/migration-v0.4-to-v1.0.md`

다음 NeoGraph 세션을 위한 현행 계획. 다음 세션도 함께 읽어야 할 자매
문서 / 컨텍스트:

- `README.md` — 아키텍처, 빌드 옵션, BUILD_SHARED_LIBS 섹션
  (2026-04-25 추가)
- 메모리: `~/.claude/projects/-root-Coding-NeoGraph/memory/project_neograph.md`
 `plan_pybind11_binding.md` (프로젝트 개요),  (설계 노트), 그리고
  더 넓은 MEMORY.md 색인.

## TL;DR — 이어서 진행하기

1. **설계 + 첫 커밋 (~2-4 h)**: , , ,
 `Provider` `Tool` , 를 노출하는 최소한의 pybind11 모듈. `GraphEngine::compile` `RunConfig` `RunResult`
 `compile(json_def, ctx).run(config)`  목표: Python 스크립트가 를
   호출하고 JSON 상태를 왕복할 수 있어야 한다.
2. **사용자 정의 Python 노드 (~2-4 h)**: pybind11 트램펄린(trampoline)으로
 `neograph.GraphNode` `run(input)` 의 Python 하위 클래스가 을 구현할 수
   있고, 엔진이 적절한 GIL 처리 아래 Python으로 디스패치한다.
3. **Wheel 패키징 (~1-2 d)**: manylinux + macOS arm64 + Win wheel이
 `libneograph_*.so`  와 바인딩을 담아 배포. 방금 착륙한 shared-libs
   작업(커밋 85619e6)과 시너지 — wheel 설치는 본질적으로 동적 링크이므로
   SHARED가 올바른 모드.

## 왜 중요한가

제안은 *"LangGraph 수준의 Python 사용 편의성 + NeoGraph µs 지연 시간을
하나의 import로"*이다. 현재 NeoGraph 사용자는 두 가지 선택지가 있다:
C++로 전부 작성하거나(Python에 익숙한 대다수 에이전트 개발자에게는 실질적
장벽) NeoGraph 자체를 무시하는 것. Pybind11은 엔진을 포크하지 않고 이
간극을 닫는다.

순수 LangGraph 대비 구체적인 이점:
- **엔진 오버헤드 130×–640× 감소** — README.md "Python 그래프/파이프라인
  프레임워크 대비 엔진 오버헤드" 벤치마크 참조. NeoGraph의 슈퍼스텝당
  5 µs 대 LangGraph의 656 µs가 바인딩 계층을 통과해도 그대로 유지된다
  (pybind11 디스패치는 µs 미만).
- **** — Docker 없이, venv 헬 없이. 번들 가 `pip install neograph`
  포함된 자체 완결 wheel.
- **LangChain 사용자가 이미 아는 Python 기본 요소** —
 `state.get("messages")` , `ChannelWrite("findings", ...)` 등.
- **이전 경로** — LangGraph 사용자가 도구 통합이나 LLM 클라이언트 코드를
  다시 작성하지 않고 엔진만 교체할 수 있다.

## 현재 컨텍스트 (2026-04-25)

이번 세션에서 착륙했으며 바인딩이 구축될 기반:

- **BUILD_SHARED_LIBS 지원** (커밋 , 후속 ): `85619e6` `110a5fb`
 `.so` NeoGraph가 이제 Linux/macOS에서 /로 빌드된다. RPATH `.dylib`
 `$ORIGIN` (/)가 깔끔하게 배선된다. 330/330 ctest 통과. `@loader_path`
  다운스트림 재에이전트(re-agent) 검증 완료.
- ** 저장소** (, 비공개, Phase 3 마무리): `re-agent` `fox1245/re-agent`
  병렬 팬아웃 + 체크포인트 + 재개 + 이중 백엔드, ~750 LOC 단일 파일
 `110a5fb` C++. NeoGraph 로 핀 버전 갱신(커밋 ). `2d57787`
- ** 저장소** (, 비공개, `re-agent-reimpl` `fox1245/re-agent-reimpl`
  Phase 4 마무리): 클린룸 재구현 에이전트. 테스트 주도 루프가 crackme01
  에서 4회 반복에 \/bin/bash.025로 10/10 수렴.

두 다운스트림 저장소는 비공개 — 개인 사용 RE 도구이다.
Pybind11 작업은 엄격히 NeoGraph(공개)에 속한다.

## Pybind11 설계

### 노출할 표면 (커밋 1)

얇은 마샬링(marshalling)으로 읽기 전용 매핑. JSON → `py::dict`

```python
import neograph
from neograph.llm import OpenAIProvider

provider = OpenAIProvider(api_key="sk-...", default_model="gpt-4o-mini")

definition = {
    "name": "demo",
    "channels": {"messages": {"reducer": "append"}},
    "nodes": {"llm": {"type": "llm_call"}},
    "edges": [
        {"from": "__start__", "to": "llm"},
        {"from": "llm", "to": "__end__"},
    ],
}

ctx = neograph.NodeContext(provider=provider)
engine = neograph.GraphEngine.compile(definition, ctx)

result = engine.run({"messages": [{"role": "user", "content": "Hi"}]})
print(result.output["channels"]["messages"]["value"])
```

변환은
경계에서 일어나며, 엔진 내부가 아니다.



감쌀 기호(symbols):

| C++ 타입 / 함수 | Python | 비고 |
|---|---|---|
| `neograph::Provider` | `neograph.Provider` (추상) | 기본 클래스. 하위 클래스 , 는 에 노출. | `OpenAIProvider` `SchemaProvider` `neograph.llm`
| `neograph::CompletionParams` | `neograph.CompletionParams` | 평범한 데이터 클래스. |
| `neograph::ChatCompletion` | `neograph.ChatCompletion` | 평범한 데이터 클래스. |
| `neograph::Tool` | `neograph.Tool` (추상) | 순수 Python 도구 하위 클래스에는 트램펄린 사용(커밋 2). |
| `neograph::graph::NodeContext` | `neograph.NodeContext` | 키워드 인자로 생성 가능(, , , ). | `provider=` `tools=` `model=` `instructions=`
| `neograph::graph::GraphEngine` | `neograph.GraphEngine` | 정적 `compile(definition, ctx, store=None)` + 인스턴스 `run(input)` / . | `run_async(input)`
| `neograph::graph::RunConfig` | `neograph.RunConfig` | 생성 가능, 은 사전을 받음. | `input`
| `neograph::graph::RunResult` | `neograph.RunResult` | 이 중첩 사전을 노출. | `.output`
| `neograph::graph::ChannelWrite` | `neograph.ChannelWrite` | 평범한 데이터 클래스. |
| `neograph::graph::Send` | `neograph.Send` | 평범한 데이터 클래스. |
| `neograph::graph::Command` | `neograph.Command` | 평범한 데이터 클래스. |
| `neograph::graph::GraphState` | `neograph.GraphState` | 사용자 정의 노드 안에서의 읽기 전용 뷰. 이 JSON 타입에 따라 /// 반환. | `.get(channel)` `dict` `list` `str` `int`

### 사용자 정의 Python 노드 (커밋 2)

Python `neograph.GraphNode` 하위 클래스가 C++ 스케줄러에 연결될 수 있게
하는 트램펄린:

```python
class MyAnalyzeNode(neograph.GraphNode):
    def __init__(self, provider):
        super().__init__()
        self.provider = provider

    def run(self, input):
        target = input.state.get("target_function")
        # ...do work in Python, possibly calling self.provider.complete(...)
        return [neograph.ChannelWrite("findings", [proposal])]

# Register so the JSON definition can reference it by type name
neograph.NodeFactory.instance().register_type(
    "analyze",
    lambda name, json, ctx: MyAnalyzeNode(ctx.provider),
)
```

GIL 처리 — *가장 까다로운 부분*. 두 가지 규칙:

1. **Python을 호출하지 않는 C++는 GIL을 해제**:
 `engine->run(...)` `engine->run_async(...)` 과 가
 `py::gil_scoped_release`  로 감싸져 엔진이 동작하는 동안 다른 Python
   스레드가 계속 실행된다.
2. **Python 노드를 호출하는 C++는 GIL을 획득**:
   노드 트램펄린이 모든 디스패치를 로 감싼 뒤
 `py::gil_scoped_acquire` `run` Python  메서드를 호출하고, 반환 시 해제한다.

엔진은 이미 Send 분기를 `fan_out_pool_` 작업자 스레드에서 실행한다
(re-agent의 `set_worker_count` 작업에서 추가 — 계약은
 참조). 각 작업자 스레드는 Python을 호출할 때 `graph_engine.cpp:60-62`
자신의 GIL 획득이 필요하다. Pybind11의 가 재진입
가능하게 처리한다. `gil_scoped_acquire`

### Wheel 패키징 (커밋 3+)

- 에 `pyproject.toml` 또는 로 `cmake-build-extension` `scikit-build-core`
 `scikit-build-core` CMake 호출 구동. 가 현대적 기본값, ~30줄 설정.
- wheel 빌드에서 을 강제하여 번들 `BUILD_SHARED_LIBS=ON`
 `libneograph_*.so` `.so` 형제들이 바인딩 와 함께 동작. RPATH
 `$ORIGIN`  설정(NeoGraph CMakeLists에 이미 구성됨).
- CI의 `cibuildwheel` 행렬: manylinux2014 (x86_64, aarch64),
  macOS (universal2), Windows. 참고로 Windows DLL 익스포트는 아직 배선되지
  않음(구성 시 경고) — Win 행은  매크로 작업이 끝날 때까지
  **뒤로 미룸**.

## 첫 커밋 모양 — 최소 pybind11 모듈

커밋 1에 착륙할 대상:

```
bindings/
  python/
    CMakeLists.txt         ← pybind11_add_module(...)
    src/
      module.cpp           ← root PYBIND11_MODULE
      bind_provider.cpp
      bind_graph.cpp
      bind_state.cpp
    pyneograph/
      __init__.py          ← re-exports + version
    tests/
      test_smoke.py        ← compile + run a 2-node graph end-to-end
```

루트 의 CMake 통합: `CMakeLists.txt`

```cmake
option(NEOGRAPH_BUILD_PYBIND "Build Python bindings (pybind11)" OFF)
if(NEOGRAPH_BUILD_PYBIND)
    add_subdirectory(bindings/python)
endif()
```

Usage

  cmake [options] <path-to-source>
  cmake [options] <path-to-existing-build>
  cmake [options] -S <path-to-source> -B <path-to-build>

Specify a source directory to (re-)generate a build system for it in the
current working directory.  Specify an existing build directory to
re-generate its build system.

Run 'cmake --help' for more information.

기본값 OFF로 기존 C++ 빌드에 영향 없음. Wheel 빌드가 scikit-build
구성을 통해 ON으로 전환.

## 사용자에게 물어볼 열린 질문

커밋 1 착륙 전에 확인할 결정 사항:

1. **저장소 위치** — 바인딩이 NeoGraph **안에** () `bindings/python/`
 `pyneograph`  있는가, 아니면 별도 자매 저장소 ()로?
   - 저장소 내: 더 빠른 피드백 루프, 단일 진실 원천(single source of
     truth), 버전이 항상 엔진과 일치.
   - 별도 저장소: NeoGraph 태그 컷과 독립적인 PyPI 배포.
   - 권장: **저장소 내**. 배포 표면이 관리 가능한 수준을 유지.

2. **PyPI 패키지 이름** — `neograph-engine` (깔끔하지만 다른 사람이 가져가면
 `pyneograph`  네임스페이스 선점 위험) vs  /  (더 안전)?
 `neograph`  - 권장: PyPI 먼저 확인, 비어 있으면  사용.

3. **Async API 노출** — 를 Python에서 로 `engine.run_async()` `async def`
 `run()`  노출(async 가능한 객체 반환)할지, 동기 만?
   - Async는 asio↔asyncio 브리지 필요 → 더 까다로움. 커밋 1에서는
     동기 우선으로 충분.

4. **사용자 정의 노드 API 사용 편의성** — `class MyNode(GraphNode)`
 `@neograph.node`  트램펄린 패턴, 또는 일반 함수를 감싸는 데코레이터 ()?
   데코레이터가 더 Pythonic하지만  /  방출(emit)에 접근할
 `Command` `Send` 수 없음.
   - 권장: **둘 다**. 트램펄린이 주, 데코레이터는 일반적인 쓰기 전용
     경우의 편의 구문.

## pybind11 커밋에 포함하지 않을 것

- **Anthropic / Gemini 제공자** — NeoGraph 측 작업이지 바인딩 작업이
  아님. 바인딩 착륙 시점에 C++에 아직 없으면 에 나타나지
 `neograph.llm` 않을 뿐.
- **MCP 클라이언트 바인딩** — 는 Python에서 `neograph::mcp::MCPClient`
  유용하지만 서브프로세스 생성의 까다로운 부분이 있어 신중한 GIL 처리가
  필요. 커밋 4+로 미룸.
- **Postgres 체크포인트 바인딩** — Python 사용자에게는 이미 `psycopg2` /
 `asyncpg` 가 있다. `PostgresCheckpointStore` 감싸기는 중복. SQLite
  체크포인트 바인딩은 전선 형식 중복 제거(wire format dedup)가 중요하고
  SQLite에 같은 스키마를 가진 Python 동등물이 없으므로 할 가치가 있음.

## 예상 총 범위

- 커밋 1 (기본 표면): 2–4시간
- 커밋 2 (Python 사용자 정의 노드 + GIL): 2–4시간
- 커밋 3 (wheel 패키징, Linux 전용): 4–8시간
- 커밋 4+ (cibuildwheel 행렬, MCP 바인딩, async): 1–2일
- **Linux에서 "pip install neograph" 동작까지 총합**: 1–2일
- **다중 플랫폼 wheel까지 총합**: 3–5일

## 인계 확인

다음 세션에서 이어받을 때 업스트림 상태가 변하지 않았는지 온전성 확인:

2932361 fix(python): keep reassigned providers alive
6b3459a feat(python): Store, RateLimitedProvider and validate() — the last of the parity gaps (#97)
24793e3 Merge pull request #110 from fox1245/feat/95-python-mcp
21973ab fix(wheel): ship libneograph_mcp.so — and correct the size number I got wrong
035688c fix(mcp): a stdio session must own its io_context — using one in a graph was a use-after-free
On branch feat/97-python-parity
Your branch is up to date with 'origin/feat/97-python-parity'.

Changes not staged for commit:
  (use "git add <file>..." to update what will be committed)
  (use "git restore <file>..." to discard changes in working directory)
	modified:   .gitignore

Untracked files:
  (use "git add <file>..." to include in what will be committed)
	.cache/
	.omo/
	cuda-profiler-api-12-8_12.8.90-1_amd64.deb

no changes added to commit (use "git add" and/or "git commit -a")
build-shared-test/libneograph_async.so
build-shared-test/libneograph_core.so
build-shared-test/libneograph_llm.so
build-shared-test/libneograph_mcp.so
build-shared-test/libneograph_sqlite.so
shared build cached

그런 다음 위의 설계 질문부터 시작하여 커밋 1을 착륙.

```bash
cd /root/Coding/NeoGraph
git log --oneline -5
# expect: 110a5fb cleanup, 85619e6 BUILD_SHARED_LIBS, c7ee23e split, ...
git status                  # should be clean
ls build-shared-test/lib*.so 2>/dev/null && echo "shared build cached" \
                              || echo "rebuild needed: cmake -S . -B build-shared-test -DBUILD_SHARED_LIBS=ON -DNEOGRAPH_BUILD_TESTS=ON && cmake --build build-shared-test -j"
```
