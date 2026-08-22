<!-- neograph-i18n: source=README.md locale=ko source_sha256=6ba467cfa403c387e0a433c35a7d0002d1579850b8820d50544b399c8cadb239 -->
<p align="center">
<h1 align="center">NeoGraph</h1>
  <p align="center">
<strong>빠른 C++ 그래프 런타임으로, 내구성 있는 프로그래머블 에이전트 제어 플레인을 갖춘다.</strong><br>
지연 시간이 중요할 때는 정적 Core 실행. 제어가 중요할 때는 QuickJS Programs, 서브에이전트, Hooks, 런타임 컨텍스트, 검증된 토폴로지 진화.
  </p>
</p>

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

<p align="center">
  <a href="https://pypi.org/project/neograph-engine/"><img alt="PyPI" src="https://img.shields.io/pypi/v/neograph-engine?label=pip%20install%20neograph-engine&color=blue"></a>
  <a href="https://pypi.org/project/neograph-engine/"><img alt="Python versions" src="https://img.shields.io/pypi/pyversions/neograph-engine"></a>
  <a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-MIT-green.svg"></a>
</p>

<p align="center">
<a href="#quick-start">빠른 시작</a> &middot;
<a href="#two-runtime-layers">아키텍처</a> &middot;
<a href="#python">Python</a> &middot;
<a href="examples/README.md">예제</a> &middot;
<a href="docs/reference-en.md">C++ 참조</a> &middot;
<a href="docs/python-binding.md">Python 참조</a>
</p>

---

<p align="center">
  <a href="docs/videos/neograph-promo-v3.mp4">
    <img src="docs/images/neograph-promo-v3.gif" alt="NeoGraph — generated Programs, semantic admission, runtime topology, Hooks, context and Python parity" width="900">
  </a>
</p>

## 오늘날 NeoGraph가 무엇인지

NeoGraph에는 의도적으로 분리된 두 개의 실행 계층이 있습니다:

| 계층 | 용도로 사용하십시오 | 계약 |
|---|---|---|
| **GraphEngine / Core** | 고정 또는 호스트 선택 그래프, 낮은 오버헤드, 임베디드 배포 | 불변 컴파일 토폴로지; C++ 노드는 Pregel 스타일 슈퍼스텝을 통해 실행됩니다 |
| **ProgramRuntime / QuickJS** | 런타임 제어, 하위 Program, 구조적 동시성, 토폴로지 교체 및 마이그레이션 | 불변 Program 세대; 지속형 타입 명령; 저널링된 전환 및 재생(replay) |

모델은 컴파일러, 카탈로그, 자격 증명, 마이그레이션 또는 권한 부여 액세스를 절대 받지 않습니다. 생성된 소스는 다음과 같습니다:

```text
proposal → reserve → compile → semantic validate → admit → publish → migrate or spawn
```

거부된 제안은 `ProgramVersion`를 게시할 수 없으며, 동적 컴파일 예산도 복원되지 않습니다. [엄격한 런타임 개입](docs/STRICT_RUNTIME_INTERPOSITION.md) 및 [DSL 기능 평가](docs/DSL_CAPABILITY_EVAL.md)를 참조하십시오.

<a id="quick-start"></a>
## 빠른 시작

### C++ Core

```bash
git clone https://github.com/fox1245/NeoGraph.git
cd NeoGraph
cmake -S . -B build -DNEOGRAPH_BUILD_EXAMPLES=ON
cmake --build build --parallel
./build/example_core_quickstart
```

전체 소스는 [examples/62_core_quickstart.cpp](examples/62_core_quickstart.cpp)에 있습니다. C++ 노드 하나를 등록하고, 엄격한 그래프를 컴파일하고, 실행하고, 타입이 지정된 채널을 읽습니다.

필요할 때 프로그래밍 가능한 제어 평면을 활성화합니다:

```bash
cmake -S . -B build-program \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON \
  -DNEOGRAPH_BUILD_EXAMPLES=ON
cmake --build build-program --parallel
./build-program/example_program_quickstart
```

[examples/63_program_quickstart.cpp](examples/63_program_quickstart.cpp) 및 [QuickJS 작성 경계](docs/QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)를 참조하십시오.

<a id="two-runtime-layers"></a>
## 두 가지 런타임 계층

### GraphEngine / Core

- 정적 및 조건부 엣지, 사이클, 배리어, `Send` fan-out 및 `Command` 라우팅;
- 체크포인트/재개, 정확한 체크포인트 재개, 포크, 상태 기록, HITL 및 `NodeInterrupt`;
- 동기 및 코루틴 API, 스트리밍, 취소 및 토큰 회계;
- 그래프 전체 및 노드별 재시도 정책, 지터 및 경계 있는 재사용 가능 노드 캐싱;
- 사용자 정의 레지스트리, 공급자, 도구, MCP, A2A 및 ACP 통합;
- 안전 지점 캡처 및 형태 보존 GraphEngine 생성 마이그레이션;

### ProgramRuntime / QuickJS

- 제한된 QuickJS `define()` 및 생성기 `main(input)`에서의 표준 JavaScript 계산;
- 봉인된 명령: `callCore`, `spawn`, `await`, `all`, `parallel`, `race`, `quorum`, `emit`, `checkpoint`, `cancelScope` 및 승인된 호스트 기능;
- 불변 Program 번들, 버전, 카탈로그, 승인(admission) 프로필 및 정책 스냅샷;
- 지속적 명령 저널, 정확한 재생(replay), 자식 계보, 비갱신 예산 및 프로세스 복구;
- 체크포인트 교체 및 제한된 라이브 GraphEngine 토폴로지 마이그레이션;
- 생성된 Program의 승인(admission) 전 호스트 소유 의미 검증.

설치된 JavaScript 표면은 `javascript_authoring_capability_manifest()`을 통해 기계 판독 가능하며 CI에서 실제 QuickJS 바인딩과 대조하여 확인됩니다.

## 런타임 안전 및 컨텍스트

NeoGraph는 중요한 동작을 모델 재량 밖으로 이동시킵니다:

- 불변 RAW 메시지 기록 및 `ContextEpoch` 선택;
- 파생 컨텍스트, 필수 Skill 및 하드 제약 조건;
- 필수 아티팩트를 정확히 보존하는 보수적 변환 영수증;
- 네이티브, stdio 또는 HTTP 실행 백엔드에 대한 필수 수명주기 Hook;
- 공급자 디스패치 및 최종 결과 영수증;
- 지속적인 런타임 개발자 지침 및 승인된 토폴로지 전환.

NeoGraph는 구성, 승인(admission), 디스패치 및 증거 경계들을 보장합니다. LLM이 모든 토큰에 주의를 기울였다고 주장하지 않습니다.

## Python

Python 패키지는 동일한 C++ 엔진을 사용하며 이제 Program, Hook, 엄격한 컨텍스트, 런타임 정책 및 SQLite 지속성 표면을 포함합니다:

```bash
pip install neograph-engine
```

### 5초 데모 (API 키 불필요)

```python
import neograph_engine as ng

@ng.node("greet")
def greet(state):
    return [ng.ChannelWrite(
        "messages",
        [{"role": "assistant", "content": f"Hello, {state.get('name')}!"}],
    )]

definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "demo",
    "channels": {
        "name": {"reducer": "overwrite"},
        "messages": {"reducer": "append"},
    },
    "nodes": {"greet": {"type": "greet"}},
    "edges": [
        {"from": ng.START_NODE, "to": "greet"},
        {"from": "greet", "to": ng.END_NODE},
    ],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())
result = engine.run(ng.RunConfig(thread_id="t1", input={"name": "NeoGraph"}))
print(result.output["channels"]["messages"]["value"])
```

Python은 추가로 다음을 노출합니다:

- `RetryPolicy`, 노드별 런타임 재정의, `RunMetadata`, 정확한 `resume_from` 및 재사용 가능한 캐시 범위;
- `ProgramSource`, `ProgramRegistryBuilder`, `ProgramCompiler`, `LocalProgramHost`, 핸들 및 결과;
- 필수 `HookRuntime` 콜백 및 실패 시 차단 수명주기 전달;
- `RuntimeContextRequirements`, `ContextTransformReceipt`, SQLite 영속 컨텍스트/디스패치 저장소, 및 `StrictRuntimeProfile`.

[Python 바인딩 가이드](docs/python-binding.md) 및 [Python 예제](bindings/python/examples/README.md)를 참조하십시오.

## 빌드 구성

Core 전용 사용자는 Program 또는 QuickJS 비용을 지불하지 않습니다:

```bash
cmake -S . -B build-core \
  -DNEOGRAPH_BUILD_PROGRAM=OFF \
  -DNEOGRAPH_BUILD_LLM=OFF \
  -DNEOGRAPH_BUILD_MCP=OFF
```

주요 옵션:

| 옵션 | 용도 |
|---|---|
| `NEOGRAPH_BUILD_PROGRAM` | 내구성 있는 Program 값, 카탈로그, 런타임, 계보 및 마이그레이션 |
| `NEOGRAPH_BUILD_QUICKJS_CONTROL` | QuickJS Program 작성 및 생성기 명령 |
| `NEOGRAPH_BUILD_PYBIND` | `neograph-engine` Python 확장 |
| `NEOGRAPH_BUILD_SQLITE` | SQLite 체크포인트, 컨텍스트, Hook 및 공급자 영수증 저장소 |
| `NEOGRAPH_BUILD_POSTGRES` | PostgreSQL 체크포인트 및 Program 영속성 구성 요소 |
| `NEOGRAPH_BUILD_MCP_CLIENT` / `SERVER` | MCP 클라이언트 및 서버 역할 |
| `NEOGRAPH_BUILD_A2A` / `ACP` / `GRPC` | 선택적 프로토콜 통합 |

배포 환경에 맞는 좁은 CMake 대상을 사용하십시오: `neograph::core`, `neograph::llm`, `neograph::program`, `neograph::mcp`, `neograph::a2a`, 또는 기타 활성화된 구성 요소.

## 검증

저장소는 결정적 C++ 및 Python 스위트, Program 재생(replay)/마이그레이션 프로브, DSL 기능 픽스처, 문서/i18n 검사, 새니타이저, 그리고 선택적 라이브 모델 평가를 실행합니다. 벤치마크 주장은 [benchmarks](benchmarks/README.md)와 날짜가 표시된 [performance report](docs/performance-deep-dive.md)에 속하며, 시대를 초월한 API 보장으로서가 아닙니다.

## 문서

- [개념](docs/concepts.md)
- [C++ 참조](docs/reference-en.md)
- [Python 바인딩](docs/python-binding.md)
- [동시성 및 취소](docs/concurrency.md)
- [비동기 가이드](docs/ASYNC_GUIDE.md)
- [Harness MCP](docs/HARNESS_MCP.md)
- [QuickJS 공개 작성 경계](docs/QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)
- [엄격한 런타임 인터포지션](docs/STRICT_RUNTIME_INTERPOSITION.md)
- [문제 해결](docs/troubleshooting.md)
- [예시](examples/README.md)

## 라이선스

MIT — [라이선스](LICENSE) 참조. 타사 고지 사항: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
