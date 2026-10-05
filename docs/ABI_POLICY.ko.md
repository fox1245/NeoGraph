<!-- neograph-i18n: source=docs/ABI_POLICY.md locale=ko source_sha256=0abd9601232a6d6e83d263798993650759083561557b016d44f91152f04c0bb7 -->
# 바이너리 호환 정책

**Languages:** [English](ABI_POLICY.md) | [한국어](ABI_POLICY.ko.md) | [日本語](ABI_POLICY.ja.md) | [简体中文](ABI_POLICY.zh-CN.md)

## 버전과 loader 계약

CMake는 `pyproject.toml`에서 NeoGraph 버전을 읽어 공개 컴파일 라이브러리의 `VERSION`에 전체 버전, `SOVERSION`에 major 값을 설정한다.

| 릴리스 계열 | loader 세대 | 계약 |
|---|---:|---|
| `0.x` | `0` | pre-v1; 공지된 재빌드 경계에서 바이너리 호환이 깨질 수 있다. |
| `1.x` | `1` | 계획된 stable v1 정책이며 v1 출시 주장이 아니다. |
| `N.x`, `N >= 2` | `N` | major ABI 경계; 소비자를 재빌드한다. |

`SOVERSION 0`은 pre-v1 package의 loader 이름이며 layout 호환 보장이 아니다. loader가 모든 비호환 `0.x` 교체를 거부할 수 없다. 대상 릴리스 노트를 읽고 헤더/라이브러리를 함께 교체하며 공지된 경계마다 재빌드한다. 실행 중 process에 개별 pre-v1 라이브러리를 hot-swap하지 않는다.

## 필수 재빌드 경계

- pre-`0.9.0`에서 `0.9.0+`: GraphNode의 레거시 실행 virtual 8개가 제거되었다. 사용자 node는 `run(NodeInput)`을 구현하고 SyncGraphNode는 추가 adapter다.
- 이후 pre-v1 bounded-resource, UsageAccumulator 예약, issue #216 변경: 공개 layout에 accounting, admission, cache, runtime-interposition 상태가 추가되었다. 일치하는 헤더로 재빌드한다.
- typed-provider 전환: 일치하는 NeoGraph와 SchemaProvider SDK 헤더/라이브러리로 모든 C++ consumer와 custom provider를 재빌드한다. `CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, 옛 descriptor interpreter, Responses WebSocket 경로는 alias 없이 제거되었다.
- event-driven provider dispatch: CancelToken은 `std::stop_source`를 쓰고 `stop_token()`을 공개한다. `cancel`, `fork`, Asio-slot signature가 같아도 옛 inline 취소 코드가 호환되지는 않는다.
- 완료된 취소 context 분리: CancelToken의 공개 layout에 generation으로 보호하는 emit 상태가 추가되었다. NeoGraph와 모든 native consumer를 함께 재빌드한다. Detach는 context 파괴 전에 완료된 slot handler를 해제하며, operation이 먼저 완료되어야 하고 취소 상태를 초기화하지 않는다.
- 미래 `1.0.0` 경계는 loader 세대를 `1`로 바꾸며 재빌드가 필요하다. generation-1 동결은 해당 릴리스 정책이지 현재 검증 결과가 아니다.

## 공개 인터페이스

Provider의 subclass hook은 `get_name`, `family`, `prepare(ProviderRequest)` 세 개다. 공통 `invoke(_async)`, `dispatch(_async)`는 소유 typed request를 소비하고 불변 `sp::runtime::Result`를 반환한다. 가상 completion override 쌍이 아니다.

CheckpointStore는 명시적 adapter 이전을 위해 레거시 layout을 유지한다. sync 기본값은 async override로 넘기지 않고 실패하며 async 기본값은 sync override를 offload한다. 새 async-only backend는 AsyncCheckpointStore와 `adapt_async_checkpoint_store`를 쓰고 sync capability backend는 CheckpointStoreCore와 `adapt_checkpoint_store`를 쓴다. 공지된 pre-v1 경계에서 재빌드한다. adapter 설계가 라이브러리만 교체할 권한을 주지는 않는다.

## SDK와 Python 경계

LLM node를 꺼도 Core는 외부 `SchemaProvider::runtime`을 요구한다. 선택된 SDK 릴리스는 `0.1.0` alpha, interface revision `4`, shared-library ABI revision `4`이며 out-of-line capability check를 쓴다. stable interface 선언은 아니다. 일치하는 SDK component를 함께 설치한다. `libsp_*.so.4` 세대는 NeoGraph loader 세대 및 Python `abi3` wheel tag와 별개다.

Interface 4는 family별 request control을 추가하고 공개 request layout을 바꾼다. SDK consumer, NeoGraph, Python extension을 함께 재빌드한다. interface-3 header나 library를 interface 4와 혼용할 수 없다. Native archive v3 / `spna3`와 portable JSON v2는 독립적이며 형식은 바뀌지 않는다. 과거 interface-3 측정은 interface 4를 검증하지 않는다.

Python은 prepared handle과 불변 outcome을 포함한 typed provider 계약을 공개하며 레거시 completion shim은 없다. extension과 일치하는 라이브러리를 한 wheel로 설치한다. bundled NeoGraph/SDK 라이브러리를 개별 교체하지 않는다. 그래프의 ChatMessage 편의 값이 native ProviderMessage custody를 대신하지 않는다. [Python binding](python-binding.md)을 참고한다.

wheel은 일치하는 SDK runtime shared library 6개를 포함하며 SDK C++ header나 CMake package는 포함하지 않는다. C++ consumer는 SDK를 별도 설치한다. source resolution은 명시적 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`, installed package, 공개 revision-pinned archive fallback 순이다. installed SDK를 쓰는 offline build는 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`를 설정하고 `CMAKE_PREFIX_PATH`로 prefix를 제공한다.

## 설치 이름과 플랫폼 한계

Linux shared library는 versioned file, major-generation SONAME link, unversioned linker 이름을 갖는다. NeoGraph shared library는 sibling 의존성에 `$ORIGIN`을 쓴다. macOS `.dylib`/`@loader_path`, Windows unsuffixed `.dll` 이름은 packaging 규칙이며 새 SDK runtime을 검증하지 않는다. static archive는 SONAME이 없고 transitive link 요구를 없애지 않는다.

기록된 interface-3 SDK runtime/archive 검증은 Linux/POSIX 범위이며 interface 4를 검증하지 않는다. 기존 macOS/Windows metadata와 의존성 검증은 별개이며 WASM runtime 검증은 확립되지 않았다. wheel tag는 Python/ABI/platform 호환을 표시하지 모든 runtime 경로 실행의 증거는 아니다. [PyPA tag 명세](https://packaging.python.org/en/latest/specifications/platform-compatibility-tags/)를 참고한다.

## 검증 근거

`scripts/test_find_package.sh`는 installed-consumer 검사 정의이지 통과 결과가 아니다. 날짜가 있는 이전 측정은 [역사적 근거](VALGRIND.md)로 남긴다. 현재 NeoGraph/SDK/Python 결과는 통합 릴리스 보고에서 build, platform, 실행 경로를 밝혀야 한다. 이 정책은 새 통과 주장을 만들지 않는다.
