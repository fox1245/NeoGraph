<!-- neograph-i18n: source=docs/ABI_POLICY.md locale=ko source_sha256=5ecfa5a3968c2ebfb4344b913e7b9be42f04485f90940d647b0d693b1f9d129a -->
# 바이너리 호환 정책

**Languages:** [English](ABI_POLICY.md) | [한국어](ABI_POLICY.ko.md) | [日本語](ABI_POLICY.ja.md) | [简体中文](ABI_POLICY.zh-CN.md)

## 버전과 로더 계약

CMake는 `pyproject.toml`에서 NeoGraph 버전을 읽어, 컴파일되는 공개 라이브러리의 `VERSION`을 그 버전으로, `SOVERSION`을 그 주(major) 버전 번호로 설정합니다.

| 릴리스 계열 | 로더 세대 | 계약 |
|---|---:|---|
| `0.x` | `0` | v1 이전 단계다. 공지된 재빌드 경계에서는 바이너리 호환이 깨질 수 있다. |
| `1.x` | `1` | 계획 중인 안정(stable) v1 정책이다. v1이 출시되었다는 뜻은 아니다. |
| `N.x`, `N >= 2` | `N` | 주(major) ABI 경계다. 소비자를 다시 빌드해야 한다. |

`SOVERSION 0`은 v1 이전 패키지에 로더가 인식하는 이름을 줄 뿐, 레이아웃을 서로 바꿔 쓸 수 있다는 뜻이 아닙니다. 로더가 호환되지 않는 `0.x` 교체를 모두 걸러 낼 수는 없습니다. 대상 릴리스 노트를 읽고, 헤더와 라이브러리를 한꺼번에 교체하고, 공지된 경계마다 다시 빌드해야 합니다. 이미 실행 중인 프로세스에서 v1 이전 라이브러리를 하나씩 핫스왑해서는 안 됩니다.

## 필수 재빌드 경계

- `0.9.0` 이전에서 `0.9.0+`로: GraphNode에서 기존 실행용 virtual 함수 여덟 개가 제거되었습니다. 사용자 정의 노드는 `run(NodeInput)`을 구현해야 하며, SyncGraphNode는 추가로 제공되는 어댑터입니다.
- 그 뒤 v1 이전에 이루어진 bounded-resource, UsageAccumulator 예약, 이슈 #216 변경: 공개 레이아웃에 회계(accounting), 승인(admission), 캐시, runtime-interposition 상태가 추가되었습니다. 일치하는 헤더로 다시 빌드해야 합니다.
- 타입 기반 provider 전환(typed-provider cutover): 모든 C++ 소비자와 사용자 정의 provider를, 서로 맞는 NeoGraph 및 SchemaProvider SDK 헤더·라이브러리로 다시 빌드해야 합니다. `CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, 기존 descriptor 해석기, Responses WebSocket 경로는 별칭(alias) 없이 제거되었습니다.
- 이벤트 기반 provider 디스패치: CancelToken은 `std::stop_source`를 사용하고 `stop_token()`을 공개합니다. `cancel`, `fork`, Asio 슬롯 시그니처가 그대로 남아 있다고 해서 기존의 인라인 취소 코드가 호환되는 것은 아닙니다.
- 비워진(drained) 취소 컨텍스트의 detach: CancelToken의 공개 레이아웃에 세대(generation)로 보호되는 emit 상태가 추가되었습니다. NeoGraph와 모든 네이티브 소비자를 함께 다시 빌드해야 합니다. detach는 컨텍스트를 소멸시키기 전에 완료된 슬롯 핸들러를 해제합니다. 이때도 진행 중인 연산이 모두 비워져 있어야 하며, 취소 상태를 초기화하지는 않습니다.
- `ChatTool::strict`: 이식 가능한 도구 정의에 멤버가 하나 추가되었습니다. `ChatTool`은 `Tool::get_definition()`과 `make_provider_request`에서 값으로 라이브러리 경계를 넘나드므로, NeoGraph, 모든 네이티브 소비자, 사용자 정의 `Tool`, Python 확장을 함께 다시 빌드해야 합니다.
- 앞으로 나올 `1.0.0` 경계에서는 로더 세대가 `1`로 바뀌므로 다시 빌드해야 합니다. 세대 1 동결(freeze)은 그 릴리스에 적용할 정책이며, 현재 검증을 마친 결과가 아닙니다.

## 공개 인터페이스

Provider에는 서브클래스가 구현하는 훅이 세 개 있습니다. `get_name`, `family`, `prepare(ProviderRequest)`입니다. 공통 메서드인 `invoke(_async)`와 `dispatch(_async)`는 소유권이 있는 타입 요청을 소비하고 불변 `sp::runtime::Result`를 반환합니다. 짝을 이루는 virtual completion 오버라이드가 아닙니다.

CheckpointStore는 명시적인 어댑터 마이그레이션을 위해 기존 레이아웃을 유지합니다. 동기 기본 구현은 비동기 오버라이드로 넘어가지 않고 실패하며, 비동기 기본 구현은 동기 오버라이드를 다른 스레드로 넘겨(offload) 실행합니다. 새로 만드는 비동기 전용 백엔드는 AsyncCheckpointStore를 구현하고 `adapt_async_checkpoint_store`를 사용합니다. 동기 방식의 capability 백엔드는 CheckpointStoreCore를 구현하고 `adapt_checkpoint_store`를 사용합니다. 공지된 v1 이전 경계에서는 다시 빌드해야 합니다. 이 어댑터 설계가 공유 라이브러리만 교체해도 된다는 뜻은 아닙니다.

## SDK와 Python 경계

Core는 LLM 노드를 꺼 두더라도 외부 `SchemaProvider::runtime`이 필요합니다. 선택된 SDK 릴리스는 `0.3.0` 알파이고, 공개 `maint/interface6` 커밋 `f7ed8161fbcf39a6bf9358f78cfdcfda133537a2`, 인터페이스 리비전 `6`, 공유 라이브러리 ABI 리비전 `6`이며 out-of-line capability 검사를 사용합니다. 안정적인 인터페이스라는 주장은 아닙니다. 맞는 SDK 구성 요소는 함께 설치해야 합니다. SDK의 `libsp_*.so.6` 세대는 NeoGraph의 로더 세대, Python의 `abi3` wheel 태그와 각각 별개입니다.

인터페이스 6은 공개 usage 레이아웃에 provider가 보고하는 고정 크기 금액 메타데이터를 추가하고, 블로킹 SDK 호출과 비동기 SDK 호출에 비동기 파이프라인 하나를 함께 사용합니다. 선택된 커밋은 리비전 6 안에서 `sp::OpenRouterRouting`에 `max_price`를 추가해 그 크기를 늘렸고, Chat, Messages, Responses 요청 레이아웃도 함께 커졌습니다. capability 비트 `OpenRouterRoutingMaxPrice`는 `RequiredProvider`에 속하고 NeoGraph의 핸드셰이크가 요구하므로, 이보다 이전의 인터페이스 6 라이브러리는 거부됩니다. SDK 소비자, NeoGraph, Python 확장을 함께 다시 빌드해야 하며, 이전의 헤더나 라이브러리는 인터페이스 6과 호환되지 않습니다. 블로킹 래퍼는 SDK 이벤트 루프와 콜백 워커로의 재진입을 계속 거부합니다. 네이티브 archive v3 / `spna3`와 이식 가능한 JSON v2는 독립적인 형식이며 바뀌지 않았습니다. 정규 outcome v2의 usage는 금액 메타데이터가 있을 때에 한해 선택 필드 `provider_cost`를 추가합니다. 금액 메타데이터가 없던 과거 기록은 원래 바이트를 그대로 유지하며 비용을 지어내지 않습니다. 이전의 인터페이스 3/4/5 측정값은 원래의 범위에서만 유효하며 인터페이스 6을 검증하지 않습니다.

Python은 준비된 핸들과 불변 outcome을 포함한 타입 기반 provider 계약을 공개하며, 기존 completion shim은 없습니다. 확장 모듈과 거기에 맞는 라이브러리는 wheel 하나로 함께 설치합니다. wheel에 포함된 NeoGraph 라이브러리나 SDK 라이브러리를 개별적으로 교체해서는 안 됩니다. 그래프의 ChatMessage 편의 값은 네이티브 ProviderMessage의 보관(custody)을 대체하지 않습니다. [Python 바인딩](python-binding.md)을 참고하세요.

wheel에는 curl을 쓰지 않는 와이어 계층을 포함해, 맞는 SDK 런타임 공유 라이브러리 일곱 개가 들어 있습니다. SDK의 C++ 헤더나 CMake 패키지는 포함되지 않으므로 C++ 소비자는 SDK를 따로 설치해야 합니다. 소스는 명시적으로 지정한 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`을 가장 먼저 사용하고, 없으면 설치된 패키지, 그것도 없으면 리비전이 고정된 공개 아카이브를 폴백으로 사용합니다. 설치된 SDK로 오프라인 빌드하려면 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`로 설정하고, SDK의 prefix는 `CMAKE_PREFIX_PATH`로 지정합니다.

## 설치 이름과 플랫폼 한계

Linux에서 공유 라이브러리는 버전이 붙은 파일, 주(major) 세대 SONAME 링크, 버전이 없는 링커 이름을 가집니다. NeoGraph 공유 라이브러리는 같은 디렉터리의 의존 라이브러리를 찾기 위해 `$ORIGIN`을 사용합니다. macOS의 `.dylib`/`@loader_path`와 Windows의 접미사 없는 `.dll` 이름은 패키징 관례일 뿐이며, 새 SDK 런타임을 검증한 것은 아닙니다. 정적 아카이브에는 SONAME이 없고, 전이적(transitive) 링크 요구 사항도 없애 주지 않습니다.

기록된 인터페이스 3 SDK 런타임/아카이브 검증은 Linux/POSIX만 다루며 인터페이스 6은 검증하지 않습니다. 기존 macOS/Windows 패키지 메타데이터는 의존성 검증과 별개입니다. WASM 런타임은 검증된 바가 없습니다. wheel 태그는 Python/ABI/플랫폼 호환성을 나타낼 뿐, 모든 런타임 경로를 실행해 봤다는 증거가 아닙니다. [PyPA 태그 사양](https://packaging.python.org/en/latest/specifications/platform-compatibility-tags/)을 참고하세요.

## 검증 근거

`python scripts/verify_ci.py install --work-dir <fresh-output> --jobs 2`는 설치된 소비자 검사를 정의하는 명령일 뿐, 통과했다는 결과가 아닙니다. 각 플랫폼이 지원하는 소비자 행에 맞춰 `--shared`, `--core-only`, `--program --quickjs` 중 하나를 사용합니다. 이 드라이버는 Git이나 Bash 없이 체크아웃한 저장소나 압축을 푼 sdist에서 실행됩니다. 날짜가 붙은 cutover 이전 측정값은 [과거 증거](VALGRIND.md)로 남아 있습니다. 현재의 NeoGraph, SDK, Python 결과는 통합 릴리스 보고서에 빌드, 플랫폼, 실행한 경로를 명시해야 합니다. 이 정책이 새로운 통과 주장을 만들어 내지는 않습니다.
