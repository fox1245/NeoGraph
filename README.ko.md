<!-- neograph-i18n: source=README.md locale=ko source_sha256=73d8153a6b0ecc5957982362ae8429663ac2730be7c65242cb1c4b1031fc170c -->
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

SchemaProvider는 `NEOGRAPH_BUILD_LLM=OFF`여도 필수 외부 C++ 의존성이다. Core도 소유 typed provider 계약을 공개한다. SDK runtime 패키지를 설치하고 설치 prefix를 `SCHEMAPROVIDER_PREFIX`로 지정한다. 아래 configure는 `-DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX"`를 사용한다. 또는 `-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=../SchemaProvider`로 checkout을 명시한다. 추측한 sibling checkout이나 구 bundled interpreter를 자동 선택하지 않는다. 현재 SDK runtime/archive는 Linux/POSIX이며 의존성 없음·OpenSSL 불필요·native Windows/macOS·WASM runtime을 약속하지 않는다.

```bash
git clone https://github.com/fox1245/NeoGraph.git
cd NeoGraph
cmake -S . -B build -DNEOGRAPH_BUILD_EXAMPLES=ON -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX"
cmake --build build --parallel
./build/example_core_quickstart
```

전체 소스는 [examples/62_core_quickstart.cpp](examples/62_core_quickstart.cpp)에 있습니다. C++ 노드 하나를 등록하고, 엄격한 그래프를 컴파일하고, 실행하고, 타입이 지정된 채널을 읽습니다.

필요할 때 프로그래밍 가능한 제어 평면을 활성화합니다:

```bash
cmake -S . -B build-program \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON \
  -DNEOGRAPH_BUILD_EXAMPLES=ON
cmake --build build-program --parallel
./build-program/example_program_quickstart
```

[examples/63_program_quickstart.cpp](examples/63_program_quickstart.cpp) 및 [QuickJS 작성 경계](docs/QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)를 참조하십시오.

### 성능 빌드

Ninja와 Unix Makefiles 같은 단일 구성 생성기는 `CMAKE_BUILD_TYPE`이 비어
있으면 최적화 수준을 선택하지 않습니다. NeoGraph는 이 구성을 경고합니다.
GCC/Clang에서는 QuickJS와 NeoGraph가 Release의 `-O3 -DNDEBUG` 플래그 없이
컴파일되기 때문입니다.

GCC 또는 Clang에서 로컬 호스트 전용 성능 빌드를 수행하려면:

```bash
cmake -S . -B build-performance -G Ninja \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_ENABLE_NATIVE_OPTIMIZATION=ON \
  -DNEOGRAPH_BUILD_BENCHMARKS=ON \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON
cmake --build build-performance --parallel
```

`NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION=ON`은 최적화 구성에
`-march=native -mtune=native`를 추가합니다. 로컬 처리량은 좋아지지만
아티팩트가 비이식적이므로 배포용 바이너리에서는 끄십시오. Release 하드닝은
기본으로 활성화됩니다.

GCC/Clang에서 최종 Release 프로파일은 QuickJS에 C11, NeoGraph에 C++20,
`-O3 -DNDEBUG`를 사용합니다. 기본 하드닝은
`-D_GLIBCXX_ASSERTIONS`, `-fstack-protector-strong`,
`-fcf-protection=full`, Linux의 `-D_FORTIFY_SOURCE=2` 및
RELRO/NOW 링크를 포함합니다. LTO와 호스트 전용 튜닝은 기본으로 켜지지
않습니다.

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
## Typed C++ provider 호출

`SchemaProvider`는 승인된 `sp::descriptor::ValidatedDescriptor`, `sp::runtime::Options`, 선택적 `SchemaProvider::Defaults`를 받는다. descriptor는 closed/versioned 데이터 admission이지 요청/응답 interpreter나 임의 primitive registry가 아니다. credential은 공개 descriptor가 아니라 runtime options에 둔다. Defaults는 typed OpenRouter routing과 Responses 보관(`responses_store`)만 포함하고 후자는 Responses에만 유효하다. Hosted OpenRouter routing·retention·JSON 형식은 선언된 typed 제어다. Images, Veo, Decisions는 별도 NeoGraph typed client와 별도 승인을 쓰며 SDK chat grant를 물려받지 않는다.

```cpp
#include <neograph/llm/schema_provider.h>
#include <neograph/types.h>

sp::runtime::Result first_call(
    sp::descriptor::ValidatedDescriptor descriptor, sp::runtime::Options options,
    std::string model) {
    neograph::llm::SchemaProvider provider(
        std::move(descriptor), std::move(options), {});
    std::vector<sp::Message> history{
        {.role = sp::Role::User, .parts = {sp::Text{"hi"}}}};
    auto request = neograph::make_provider_request(
        provider, std::move(model), std::move(history));
    auto prepared = provider.prepare(std::move(request));
    return provider.dispatch(std::move(prepared));
}
```

공급자 호출은 `sp::runtime::Result`, 즉 `sp::Completion` 또는 `sp::Failure`를 담은 불변 소유 `std::shared_ptr<const sp::Outcome>`를 반환한다. 표시 텍스트만이 아니라 전체 결과를 보존한다. 순서 있는 메시지/파트, native continuation, 전체 wire envelope, 순서 있는 raw 관측, 중단 근거와 실제 시도 메타데이터는 호출 및 클라이언트 소멸 후에도 남는다. 사용량은 근거·단계·품질을 갖는 nullable `uint64_t`이며 누락은 0이 아니라 미상이다. 실패도 원래의 부분 결과를 보존한다. `ProviderFailure::outcome()`과 `ProviderObserverError::outcome()`은 실제 결과를 보존하며 후자의 `cause()`에는 관측자 예외가 남는다.

`ChatMessage` / `ChatTool`과 JSON은 portable projection이지 native 권한이 아니다. 현재 포맷은 [`provider-message-v2`](schemas/provider-message-v2.schema.json), [`runtime-history-record-v2`](schemas/runtime-history-record-v2.schema.json)이다. 실제 C++ 메모리 checkpoint sidecar는 archive 없이 native seal을 유지한다. 영속 native 기록과 bank 참조에는 실제 `sp::NativeArchive`가 필요하다. closed v2 / `spna2`는 독립 키를 쓰는 인증된 owner-private 보호 custody이며 암호화나 vendor-issuer 인증이 아니다. archive 본문·키·native blob·raw wire 관측을 공개하지 않는다. managed 복구/fork는 charged/reserved/report/dedup canonical bank를 공유하며 예산을 갱신하지 않는다. 일반 bounded 영속 fork는 외부 host-shared bank/journal이 필요하며 복사한 snapshot은 독립 지출 권한을 주지 않는다.


실제 결과 이후 post-effect 정산이나 terminal receipt 영속화가 실패하면 `ProviderDispatchOutcomePersistenceError`의 `outcome()`은 원래 불변 결과를, `cause()`는 원래 영속 예외를 보존한다. 전달도 실패했으면 `delivery_error()`가 원래 관측자 예외를 보존한다. 영속화 성공 뒤 관측자 실패는 원래 예외를 그대로 다시 던진다. 미상/결과 없는 transport 실패는 결과를 조작하지 않는다.
소스 및 바이너리 단절이다. 모든 C++ 소비자와 사용자 공급자를 새 헤더/라이브러리로 재컴파일한다. `CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, descriptor interpreter와 Responses WebSocket은 alias/호환 bridge 없이 제거되었다. SDK는 불안정 `0.0.0`, interface revision 3 / shared ABI 3이며 out-of-line capability check를 사용한다. 안정 릴리스 선언이 아니다. 현재 runtime/archive는 Linux/POSIX이며 Windows·macOS·WASM runtime 검증을 뜻하지 않는다. Python provider binding/wrapper는 유예되었고 이 C++ 변경으로 포팅되지 않는다.

## Python

> 아래 Python 자료는 기존 binding을 설명한다. provider binding/wrapper는 명시적으로 유예되었고 typed lossless C++ 전환으로 포팅·실행되지 않았다. 과거 wheel 설치는 새 C++ provider API를 제공하지 않는다.
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

Core 전용 빌드는 Program/QuickJS를 제외하지만 SchemaProvider runtime은 제외하지 않는다:

```bash
cmake -S . -B build-core \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DNEOGRAPH_BUILD_PROGRAM=OFF \
  -DNEOGRAPH_BUILD_LLM=OFF \
  -DNEOGRAPH_BUILD_MCP=OFF
```

주요 옵션:

| 옵션 | 용도 |
|---|---|
| `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` | 명시적 SDK source checkout. 미지정 시 설치된 runtime 패키지가 필수. |
| `NEOGRAPH_BUILD_PROGRAM` | 내구성 있는 Program 값, 카탈로그, 런타임, 계보 및 마이그레이션 |
| `NEOGRAPH_BUILD_QUICKJS_CONTROL` | QuickJS Program 작성 및 생성기 명령 |
| `NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION` | 최적화 구성에서 비이식적인 호스트 전용 명령어 튜닝을 선택적으로 활성화 |
| `NEOGRAPH_WARN_ON_UNOPTIMIZED_SINGLE_CONFIG` | 단일 구성 빌드에서 `CMAKE_BUILD_TYPE`이 없어 Release 최적화 플래그를 놓칠 때 경고 |
| `NEOGRAPH_BUILD_PYBIND` | `neograph-engine` Python 확장 |
| `NEOGRAPH_BUILD_SQLITE` | SQLite 체크포인트, 컨텍스트, Hook 및 공급자 영수증 저장소 |
| `NEOGRAPH_BUILD_POSTGRES` | PostgreSQL 체크포인트 및 Program 영속성 구성 요소 |
| `NEOGRAPH_BUILD_MCP_CLIENT` / `SERVER` | MCP 클라이언트 및 서버 역할 |
| `NEOGRAPH_BUILD_A2A` / `ACP` / `GRPC` | 선택적 프로토콜 통합 |

배포 환경에 맞는 좁은 CMake 대상을 사용하십시오: `neograph::core`, `neograph::llm`, `neograph::program`, `neograph::mcp`, `neograph::a2a`, 또는 기타 활성화된 구성 요소.

SDK imported target이 `include/SchemaProvider` include root를 제공한다. 공개 예시는 recipe 전용 helper 없이 `<descriptor/descriptor.h>`, `<runtime/client.h>`, `<neograph/llm/schema_provider.h>`를 직접 사용한다.

```cmake
find_package(SchemaProvider CONFIG REQUIRED COMPONENTS runtime)
find_package(NeoGraph CONFIG REQUIRED)
target_link_libraries(app PRIVATE neograph::core neograph::llm SchemaProvider::runtime)
```

## 검증

`scripts/test_find_package.sh`는 installed-consumer 검사 절차이며 파일 존재만으로 현재 통과를 주장하지 않는다. 현재 SDK ABI3 전체 재빌드/CTest는 26/26 통과했고 shared 설치 소비자는 실제 local HTTP 두 turn typed 요청, tool/native/refusal/known-zero 결과와 mismatch 거부를 실행했다. 이는 NeoGraph·Python·Windows·macOS·WASM·유료 live-provider 호환 검증이 아니다. NeoGraph 통합 검증은 별도로 보고한다.

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
