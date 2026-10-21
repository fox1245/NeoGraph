<!-- neograph-i18n: source=docs/troubleshooting.md locale=ko source_sha256=8e71ee3b8404e9ce2011d4723a95dca7936e1a14993bc971778b8b865401e537 -->
# 문제 해결

**Languages:** [English](troubleshooting.md) | [한국어](troubleshooting.ko.md) | [日本語](troubleshooting.ja.md) | [简体中文](troubleshooting.zh-CN.md)

## 설치된 산출물 확인

`neograph_engine.__version__`, Python 버전, OS/아키텍처, 설치 방법, 처음 발생한 오류를 기록합니다. 소스 체크아웃과 이전 wheel이 같은 API를 제공한다고 가정하지 말고, 해당 릴리스에서 받을 수 있는 wheel 파일을 확인합니다. 타입 기반 전환(typed cutover)으로 예전 provider 이름이 제거되었으므로, 이전 wheel을 import해 보는 것은 새 바인딩을 테스트하는 것이 아닙니다.

NeoGraph가 공개한 플랫폼 메타데이터에는 Linux, macOS, Windows가 들어 있습니다. wheel 태그나 컴파일러가 호환된다는 사실만으로는 런타임 통합이 입증되지 않습니다. WASM에서 provider 런타임을 지원하는지는 확인되지 않았습니다.

GLIBC 심볼이 없다는 오류가 나면 wheel의 manylinux 태그와 호스트의 glibc를 비교합니다. `manylinux_2_34` 산출물에는 glibc 2.34 이상이 필요합니다. Windows에서 DLL 오류가 나면 x64 Python인지 확인하고 빠진 의존성 DLL을 점검합니다. 아키텍처만이 원인인 것은 아닙니다. 번들된 라이브러리를 하나씩 바꿔 끼우면 안 됩니다.

## 소스 구성과 누락된 의존성

`NEOGRAPH_BUILD_LLM=OFF`로 설정해도 Core에는 `SchemaProvider::runtime`이 필요합니다. NeoGraph를 구성하려면 CMake 3.20 이상이 필요합니다. SchemaProvider는 명시적으로 지정한 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`을 먼저 쓰고, 없으면 설치된 패키지를, 그것도 없으면 리비전이 고정된 공개 archive를 기본 대체 경로로 받아 옵니다. 오프라인에서 구성하려면 맞는 SDK를 설치하고 그 prefix를 `CMAKE_PREFIX_PATH`로 지정한 다음 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`로 설정합니다. SDK를 소스에서 빌드하려면 C++20, Python, 독립형 Asio, yyjson, libcurl 7.88 이상, OpenSSL Crypto가 필요합니다. 내려받거나 소스로 통합하는 경우에는 NeoGraph가 저장소에 포함된 Asio/yyjson을 제공합니다.

SDK 자체는 OpenSSL Crypto의 최소 버전을 선언하지 않습니다. NeoGraph의 전체 HTTPS 구성과 wheel/sdist 경로는 비동기/MCP HTTP 의존성 때문에 OpenSSL 3이 필요합니다. Crypto 헤더만 설치해서는 이 빌드들에 충분하지 않습니다.

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="$SDK_PREFIX" \
  -DNEOGRAPH_BUILD_PYBIND=ON
cmake --build build -j
```

빌드 트리는 설치된 패키지가 아닙니다. `PYTHONPATH=build python -c "import neograph_engine"`을 실행하기 전에 `pip install 'pydantic>=2' certifi`로 wheel이 요구하는 Python 의존성을 설치해야 합니다. pydantic이 없으면 import가 실패하고, certifi가 없으면 TLS가 호스트의 신뢰 저장소를 사용합니다.

`Could NOT find CURL` 오류는 `NEOGRAPH_USE_LIBCURL`을 꺼도 해결되지 않습니다. 이 플래그는 NeoGraph의 선택 기능인 CurlH2Pool을 제어할 뿐, SDK의 필수 전송 계층과는 무관합니다. libcurl 개발 파일을 설치하고(Debian/Ubuntu는 `libcurl4-openssl-dev`, Fedora는 `libcurl-devel`) 툴체인과 prefix를 일관되게 사용합니다.

SQLite와 PostgreSQL은 NeoGraph의 선택 컴포넌트입니다. 개발 패키지를 설치하거나 `NEOGRAPH_BUILD_SQLITE`/`NEOGRAPH_BUILD_POSTGRES`를 명시적으로 끄면 됩니다. 이렇게 해도 SDK 런타임은 사라지지 않습니다. 헤더나 API를 바꾼 뒤 바인딩 심볼을 찾을 수 없다는 오류가 나면, 새 빌드 디렉터리에서 다시 구성하고 맞는 헤더/라이브러리로 모든 사용처를 다시 빌드합니다.

GCC 13에서 코루틴 관련 내부 컴파일러 오류가 보고된 적이 있습니다. 적합한 컴파일러로 업그레이드하거나 오류가 보고된 식의 구조를 바꿔야 합니다. catch 핸들러 안에서는 `co_await`를 쓰면 안 됩니다. C++ 자체가 그 위치의 await 식을 금지합니다. 오류를 변수에 저장해 두고, 핸들러 밖에서 복구 작업을 await합니다.

## 제거된 provider API와 타입이 있는 실패

`CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, descriptor 인터프리터 설정, `prefer_libcurl`, Responses WebSocket 선택은 모두 제거되었습니다. 대신 `SchemaProvider(load_provider_descriptor(descriptor_json), ProviderRuntimeOptions(...), SchemaProviderDefaults(...))`와 `make_provider_request`를 사용합니다. 생성자의 정확한 형태는 [Python 바인딩](python-binding.md)에, C++ 쪽은 [마이그레이션 가이드](migration-v0.4-to-v1.0.md)에 있습니다.

`prepare`는 한 번만 쓸 수 있는 `PreparedProviderRequest`를 반환하고 `dispatch`가 이를 소비합니다. `invoke`는 둘을 한 번에 실행합니다. 디스패치 receipt를 저장한 뒤에는 승인(admission)된 요청을 다시 만들면 안 되고, 이미 소비된 핸들로 재시도해서도 안 됩니다. 실패 결과(outcome)는 실제 오류/부분 결과 증거를 담은 데이터입니다. 성공처럼 보이는 completion으로 읽지 말고 `outcome.failure`를 확인합니다.

observer에서 예외가 발생하면 `ProviderObserverError`가 타입이 있는 결과(outcome)와 원인(cause)을 유지합니다. 예산 정산(settlement)과 최종 receipt 저장에서 난 오류도 실제 결과를 유지하지만, 그 증거가 다시 전송해도 된다는 뜻은 아닙니다. 전달 여부를 알 수 없을 때는 무작정 재시도하지 말고 조정(reconciliation)해야 합니다.

## 스트리밍과 Python 비동기 경계

`ProviderMode.Stream`을 명시적으로 설정해야 합니다. observer를 지정한다고 스트리밍이 선택되지는 않습니다. 토큰을 화면에 표시하려면 내용이 비어 있지 않은 content-text 델타만 골라냅니다. 사용량, reasoning, 도구, raw-wire, envelope 이벤트는 토큰이 아닙니다. C++의 이벤트 뷰는 콜백이 실행되는 동안에만 빌려 쓰는 값이고, Python의 ProviderEvent는 빌린 데이터를 복사해서 소유합니다. 느린 observer는 호스트의 전달 용량을 소모하므로 피해야 합니다.

```python
import asyncio
import neograph_engine as ng

text = ng.Text()
text.value = "Hello"
request = ng.make_provider_request(
    provider, model, [ng.ProviderMessage(ng.ProviderRole.User, [text])],
    mode=ng.ProviderMode.Stream,
)
request.on_event = observe
outcome = await asyncio.to_thread(provider.invoke, request)
if outcome.failure is not None:
    handle_failure(outcome.failure)
else:
    handle_completion(outcome.completion)
```

Provider의 invoke/dispatch는 GIL을 놓는 동기 Python 메서드입니다. 네이티브 asyncio awaitable이 아니므로 `asyncio.to_thread`를 사용합니다. 콜백 호출, 복사, 소멸은 GIL을 잡습니다. 취소는 그래프의 CancelToken을 사용하며 중지 요청을 SDK에 전달합니다. 타임아웃이 났거나 기다리던 쪽이 취소되었다고 해서 원격 효과가 일어나지 않았다는 것이 증명되지는 않습니다.

대화를 이어 가려면 도구/거부/reasoning 파트를 포함한 전체 `ProviderMessage` 값을 보관합니다. `ChatMessage`와 `ToolCall`은 그래프에서 편하게 쓰는 값일 뿐 전체 provider 기록의 별칭이 아닙니다. 사용량이 없으면 `None`이지 0이 아니며, 0으로 확인된 카운터는 증거가 붙은 UsageCount 값으로 남습니다. 이식 가능한 결과 투영으로는 네이티브 재생(replay)이나 금전 권한을 가져올 수 없습니다. 네이티브 이어 가기는 승인(admission)된 NativeArchive 보관(custody) 아래에서만 영구 저장합니다.

## TLS와 로컬 엔드포인트

신뢰 번들을 직접 지정하려면 `ProviderRuntimeOptions.ca_file`을 설정합니다. Python은 import할 때 이미 설정된 `SSL_CERT_FILE`을 그대로 두고, 없으면 certifi를 쓸 수 있을 때 certifi를 선택합니다. 선택된 CA 파일은 런타임 옵션을 통해 SDK의 libcurl로 전달됩니다. `NEOGRAPH_SKIP_CERT_AUTOFIX=1`을 설정하면 호스트의 선택을 건드리지 않습니다. 엔드포인트나 신뢰 저장소 오류를 감추려고 인증서 검증을 끄면 안 됩니다.

로컬 HTTP의 승인(admission)은 URL을 직접 덮어쓰는 방식이 아니라 검증된 descriptor에 넣어야 합니다. [예제 31](../examples/31_local_transformer.cpp)을 따라 하면 됩니다.

## 그래프 정의와 등록

알 수 없는 리듀서/조건/노드 타입이라는 진단은 컴파일에 쓴 레지스트리에 그 이름이 없다는 뜻입니다. 내장 리듀서는 `overwrite`와 `append`이며, 사용자 정의 리듀서, 조건, 노드 팩토리는 컴파일 전에 등록해야 합니다. `has_tool_calls`와 `route_channel`은 내장 조건입니다. 등록한 Python 콜백은 GIL을 잡고 실행됩니다.

```python
import neograph_engine as ng

ng.ReducerRegistry.register_reducer("sum",
    lambda current, incoming: (current or 0) + incoming)
ng.ConditionRegistry.register_condition("is_long",
    lambda state: "long" if len(state.get("messages") or []) > 10 else "short")
```

쓰기는 이미 있는 채널의 이름을 정확히 지정해야 합니다. 조건은 라우트 레이블을 반환합니다. 열린 조건은 명시적인 `default`를 둘 수 있으며, 없으면 일치하지 않는 레이블에서 예외가 발생합니다. 닫힌 조건은 선언된 집합 밖의 레이블을 거부합니다. `__start__`에서 나가는 엣지와 루프의 탈출 경로가 있는지 확인합니다. `RunConfig.max_steps`의 기본값은 `50`입니다. 잘려서 끝난 실행을 정상 완료로 취급하지 말고 스텝 한도 상태를 확인합니다.

`schema_version: 1`에서는 strict 토폴로지 파싱이 알 수 없는 키, 사용되지 않는 키, 왕복 변환에서 손실되는 값을 거부합니다. 메타데이터에는 `_`나 `x-`로 시작하는 주석을, barrier에는 비어 있지 않은 `wait_for`를, 조건부 엣지에는 `routes`를 사용합니다. 사용자 정의 항목을 등록한 뒤에는 `ng.export_schema()`로 현재 스키마를 내보내며, 에디터용 팔레트를 따로 관리하면 안 됩니다. 버전이 없거나 0인 예전 정의는 `0.x` 경로에서 계속 느슨하게 처리됩니다. `ng.upgrade_topology()`는 무시된 데이터를 충돌하지 않는 주석에 보존합니다.

## fan-out과 관리 API

worker_count의 기본값은 `1`이므로 엔진이 소유하는 풀이 만들어지지 않습니다. I/O 분기는 중단(suspend)될 때 서로 겹쳐 실행되지만, 호출자 스레드 하나에서 실행되는 CPU 위주의 본문은 직렬로 실행됩니다. CPU 작업에 풀이 도움이 된다면 동시 실행 전에 `set_worker_count(N)` 또는 `set_worker_count_auto()`를 설정합니다. Python의 CPU 위주 콜백은 네이티브 작업이 GIL을 놓지 않는 한 GIL 때문에 여전히 직렬로 실행됩니다.

같은 엔진에서 run이나 resume이 진행되는 동안 상태 조회/이력/갱신/fork 같은 관리 호출을 하면 `std::logic_error`로 실패합니다. 먼저 취소하거나 진행 중인 작업을 비우고 완료를 기다려야 하며, 이 예외를 억제하면 안 됩니다. 같은 thread id로 동시에 실행하면 체크포인트가 섞이는 순서가 정해져 있지 않고, 저장소를 공유하는 별도 엔진들은 호스트가 조율해야 합니다. [동시성 문서](concurrency.md)를 참고하세요.

## Python Program 오류

이 오류들은 `LocalProgramHost`와 QuickJS Program 컴파일러에서 발생합니다. 배경이 되는 규칙은 [Program과 QuickJS](python-binding.md#program-and-quickjs)에 있습니다.

| 오류 | 원인 | 해결 |
|---|---|---|
| 노드에서 발생하는 `TypeError: get(): incompatible function arguments` | `GraphState.get`은 채널 이름만 받는다 | `state.get("value") or 0`을 쓴다 |
| `add_registered_node`, `add_registered_reducer`, `add_registered_condition`에서 발생하는 `ng.ProgramRegistryError` | 노드 타입, 리듀서, 조건이 이 프로세스에 등록되지 않았다 | 먼저 `@ng.node`, `ng.NodeFactory.register_type`, `ng.ReducerRegistry.register_reducer`, `ng.ConditionRegistry.register_condition`으로 등록한다 |
| `P_JS_DEFINE_MISSING` | 모듈이 `define()`을 export하지 않는다 | `define()`을 export하고 Program의 그래프 하나를 반환한다 |
| `P_JS_GRAPH_ENTRY`, 또는 노드 이름이 `__start__`나 `__end__`일 때의 `P_JS_GRAPH_ARGUMENT` | 그래프에 `.entry()`가 없거나, 노드가 예약된 이름을 쓴다 | `.entry()`를 호출하고 노드 이름을 바꾼다. `.entry()`와 `.exit()`가 시작/종료 sentinel을 연결한다 |
| `P_CORE_E13` 또는 `P_CORE_E14` | 노드에 첫 번째 조건부 엣지 외에 정적 엣지나 두 번째 조건부 엣지가 있다. 둘 다 발동하지 않는다 | 모든 대상을 첫 번째 조건부 엣지의 routes에 넣는다 |
| `P_JS_CONTROL_COMMAND`: JavaScript command references a Core outside the admitted root | `ng.callCore`가 `define()`이 반환한 그래프가 아닌 다른 그래프의 이름을 쓴다 | `ng.graph(...)`에 넘긴 이름을 쓴다 |
| `ValueError: Every policy budget ceiling except child budgets must be positive; zero: ...` | 나열된 상한 필드 중 0인 것이 있다 | 양수로 바꾼다. Program이 모델을 호출하지 않아도 마찬가지다 |
| `ng.ProgramAdmissionError`의 `P_ADMIT_PROVIDER` | `llm_call` 노드가 필요로 하는 provider를 호스트가 바인딩하지 않았거나, `llm_call` 또는 `intent_classifier` 노드를 `provider=` 없이 등록했다 | `add_provider(...)`와 `add_registered_node("llm_call", ..., provider=...)`를 등록하고, `LocalProgramHost`에 `node_context=`와 `binding_label=`을 넘긴다 |
| `P_MONEY_BUDGET`(`BudgetExhausted`) 또는 다른 `P_MONEY_*` 코드(`Failed`)가 나오고 provider가 요청을 받지 못함 | 바인딩된 호출의 예약액이 `monetary_microunits`에 맞지 않거나(0은 아무것도 허용하지 않음), 호출의 상한을 정할 수 없다. provider 정책의 카탈로그에 그 모델의 가격이 없음, route 상한이 없는 router 별칭, 출력 상한 없음, 알려진 입력 한도 없음, 가격이 없는 service tier나 서버 도구, 사용 상한이 없는 도구 중 하나이다 | `monetary_microunits`를 올리거나 `provider_controls.max_output_tokens`를 설정하거나 provider 정책의 카탈로그 레이어에서 그 모델에 가격을 매긴다. [가격 증거와 모델 카탈로그](reference-en.md#pricing-evidence-and-the-model-catalogue)를 참고한다 |
| `P_MONEY_BUDGET`(`BudgetExhausted`)이 나오고 `failure.witness["monetary"]`에 `granted`, `used`, `over_by`가 있음 | provider가 요청을 받았고, 응답이 예약액보다 큰 비용을 보고해서(예: upstream이 요청의 출력 cap보다 많은 출력을 돌려준 경우) 실행의 청구액이 `monetary_microunits`를 넘겼다 | `monetary_microunits`를 올리거나, 호출 비용을 낮추거나, 호출을 출력 cap을 지키는 upstream으로만 보낸다(provider routing의 `only`나 `order`). `handle.notices`에 그런 호출마다 `P_MONEY_OVER_HOLD` notice가 있고, `result.monetary_records`에서 `microunits`가 `reserved_microunits`보다 큰 기록이 그 호출이다 |
| `P_START_BUDGET`: `Program invocation budget <field>=<n> is outside its admitted bounds` | 실행 예산이 `compile_admit`에 넘긴 예산과 다르다 | 같은 예산을 넘기거나, 다시 컴파일하고 승인(admission)한다 |
| `ng.spawn`에서 발생하는 `P_CHILD_BINDING` | `LocalProgramHost`에 자식 바인딩 resolver가 없다 | 자식 Program은 C++ 호스트에서 실행한다 |
| `resume()`에서 발생하는 `P_RUN_NOT_FOUND`: `run '<id>' was not found in this LocalProgramHost` | 그 실행이 다른 `LocalProgramHost`나 이미 소멸한 호스트의 것이다 | 그 실행을 시작한 호스트로 재개한다 |
| `reconnect()`에서 발생하는 `P_RUN_LEASE_HELD` | 자식 예산 없이 시작한 `define()`만 있는 Program은 `wall_time_ms` 동안 실행 lease를 잡으므로, 소유자가 아직 살아 있을 수 있는 실행을 다른 런타임이 이어 가지 못한다 | 진단의 witness에 있는 `retry_after_ms`가 지난 뒤에 다시 시도한다. 그러면 실행은 `TimedOut`으로 끝난다. 멈춘 실행을 마지막 체크포인트에서 이어 가려면 `main()`을 쓰거나 자식 예산(`max_child_depth`와 `max_total_children`을 0보다 크게)으로 Program을 시작한다 |
| `fork()`에서 발생하는 `P_FORK_SOURCE_BUDGET` | 포크의 예산이 일시 정지한 실행에 남은 `wall_time_ms`나 `max_core_steps`를 전부 가져가서, 그 실행이 재개할 수 없게 된다 | 포크에는 `paused.remaining_budget`의 일부(예: 절반)를 준다. `max_concurrency`는 그대로 줘도 된다 |
| `P_CORE_BARRIER_STALLED` | barrier 멤버가 join으로 이어지지 않는 경로를 택해서 join이 더는 발동할 수 없다 | 모든 멤버를 join이나 `__end__`로 라우팅한다. `P_CORE_E15`가 컴파일 시점에 이런 경로를 경고한다 |
| `P_RUNTIME_TIMEOUT`: `Program wall-time budget (N ms) expired` | 실행이 `wall_time_ms`를 모두 썼거나 그 뒤에 끝났다. `failure.witness["wall_time_ms"]`가 얼마나 넘겼는지 알려 주고, 중단된 연산의 텍스트는 `failure.witness["cause_message"]`에 있다 | 더 큰 `wall_time_ms`로 다시 컴파일하고 승인(admission)한다 |

`callCore`가 실패하면 실행은 그 실패로 끝납니다. `main()`은 `try`/`catch`로 이를 잡을 수 없습니다. Python에서 `result.status`와 `result.failure`를 확인합니다.

## 체크포인트와 PostgreSQL 오류

PostgresCheckpointStore export가 없다면 wheel이나 소스 빌드에서 이 컴포넌트를 껐다는 뜻일 수 있습니다. 이전 wheel의 기능 목록이 아니라 해당 산출물의 구성을 확인합니다. 선택 타깃을 빌드할 때는 맞는 libpq를 설치합니다. 연결 URI에서는 비밀번호의 특수 문자를 퍼센트 인코딩하거나 libpq의 key=value 형식을 씁니다. 실제 자격 증명은 절대 공개하면 안 됩니다.

비동기 연결/재연결은 모든 호스트/IP에 걸쳐 데드라인 하나를 사용합니다. 양수인 명시적 `connect_timeout=N`은 초 단위이며 `1`은 2초로 올림됩니다. 값이 없거나 0 또는 음수인 경우, 그리고 PGCONNECT_TIMEOUT이나 service 파일로만 지정한 타임아웃에는 비동기 기본값인 30초를 씁니다. 동기 libpq의 호스트별 타임아웃과는 다릅니다.

저장소는 권한이 있으면 테이블을 만듭니다. CREATE 권한이 없으면 [PostgreSQL 헤더](../include/neograph/graph/postgres_checkpoint.h)에 있는 스키마를 직접 적용해야 합니다. 대기 중인 쓰기(pending write)를 지원하지 않으면 슈퍼스텝 전체를 재생(replay)하게 되며, 외부 효과가 정확히 한 번만 일어난다는 뜻이 아닙니다. 비동기만 지원하는 사용자 정의 백엔드는 AsyncCheckpointStore와 `adapt_async_checkpoint_store`를 사용하며, 예전처럼 동기/비동기 기본 구현이 서로를 호출하는 방식은 쓰지 않습니다.

## 트레이싱 어댑터와 그 밖의 함정

트레이서 어댑터는 세션을 닫을 때 소멸하는 Span 래퍼를 가리키는 원시 포인터가 아니라, 기록한 데이터를 직접 소유해야 합니다. [C++ 트레이싱 예제](../examples/49_openinference.cpp)를 참고하세요. Python contextvars는 C++ 콜백 경계를 자동으로 넘지 않습니다. 부모 컨텍스트를 명시적으로 전달하거나 연결하고, 엔진을 컴파일하기 전에 래퍼를 설치해야 합니다.

헤더 전용 httplib를 여러 번역 단위에서 인스턴스화하는 사용처는 CPPHTTPLIB_OPENSSL_SUPPORT 정의를 일관되게 맞춰야 합니다. 타입이 있는 provider의 HTTP 디스패치는 SDK의 libcurl을 씁니다.

불투명한 편의용 벡터 속성은 Python 리스트가 아닐 수 있습니다. 내용을 확인할 때는 순회하거나 `list(value)`를 쓰고, ChatMessage.image_urls 같은 복사된 시퀀스 필드를 바꿀 때는 새 값을 만들어서 대입합니다. 반환된 복사본을 수정하면 C++ 요청이 바뀐다고 가정하면 안 됩니다.

## 도구 호출, Hook, 프로토콜 오류

그래프 실행에서 도구 호출, 노드, 서브그래프가 필수 Hook의 차단에 걸리면 `llm::Agent`와 마찬가지로 `HookBoundaryBlocked`를 그대로 던지며, 이를 감싼 `NodeExecutionError`를 던지지 않고 이에 대한 `ERROR` 이벤트도 내지 않습니다. `HookBoundaryBlocked`(Python에서는 `ng.HookBoundaryBlocked`)를 잡아 `invocation_id`와 `state`를 읽습니다. 중첩된 엔진 실행 등에서 이 예외를 발생시킨 Python 노드는 재시도되지 않습니다. [도구 실패 계약](reference-en.md#tool-failure-contract)을 참고하세요.

엔진의 `HookRuntime`은 그 엔진의 도구 호출에 대한 `BeforeToolExecution`, `AfterToolExecution` Hook도 실행하고, Program 호스트의 Hook 런타임은 그 Core의 도구 호출에 대한 Hook을 실행합니다. 그래프 실행이 `HookBoundaryBlocked` 상태 `failed`와 `lifecycle hook could not run` 메시지로 멈춘다면 실행에 소유자 범위(`RunMetadata::owner_scope`)를 지정해야 합니다. 소유자 범위가 없는 Hook 경계는 체크포인트 Hook이든 도구 Hook이든 실패 시 차단으로 처리됩니다.

`state == "timed_out"`은 Hook의 데드라인이 지났거나 대상이 타임아웃되었다는 뜻입니다. 마지막 시도가 실패한 Hook은 `failed`로 보고합니다.

도구 결과의 상태가 `expired`이고 오류가 `run deadline elapsed before the tool started`이면, 호출을 시작하기 전에 실행의 데드라인이 지났다는 뜻입니다. 이때 도구와 그 Hook은 실행되지 않았습니다. 호출을 재시도하지 말고 `RunMetadata::deadline`(Python에서는 `timeout_ms`, Program에서는 `wall_time_ms`)을 늘립니다. 실행의 데드라인이나 클라이언트의 요청 타임아웃이 중단시킨 MCP 호출은 `timed_out`이 되고, 실행을 취소하면 진행 중인 MCP 호출도 즉시 중단됩니다.

`MCPServer`는 전송 방식과 상관없이 클라이언트가 취소한 `tools/call`에는 응답을 보내지 않습니다. Hook 차단에는 `-32010`으로, 인터럽트에는 `-32011`로 응답합니다. 그 밖의 모든 실패는 `isError` 결과로 응답하며, 이 결과의 `_meta["io.neograph/failure"].code`가 실패 코드를 알려 줍니다. 결과의 텍스트는 공개 메시지이고 예외 텍스트는 절대 쓰지 않습니다(텍스트를 직접 정하려면 `public_message`를 담은 `NodeFailureError`를 발생시킵니다). gRPC 클라이언트는 `OK` 응답의 `RunGraphResponse.error`가 아니라 status를 읽어야 합니다. 실패한 실행은 OK가 아닌 status로 오며, `google.rpc.ErrorInfo`가 코드를 `metadata["code"]`에 담습니다. [프로토콜 실패 매핑](reference-en.md#protocol-failure-mapping)을 참고하세요.

유효한 UTF-8이 아닌 도구 출력이나 예외 메시지는 잘못된 시퀀스마다 U+FFFD로 대체되어 전달됩니다. 빈 도구 메시지, 빈 MCP/gRPC 페이로드, Python의 `UnicodeDecodeError`로 바뀌지 않습니다.

## 버그를 안전하게 보고하기

보고에는 버전과 플랫폼, 최소한의 토폴로지와 호출, execution_trace와 status, 민감한 부분을 가린 타입이 있는 실패/중지/사용량 증거를 포함합니다. 실패가 설치한 wheel에서 났는지 다시 빌드한 체크아웃에서 났는지도 밝힙니다. 자격 증명, 비공개 프롬프트, 인코딩된 네이티브 요청 본문, 정제하지 않은 패킷 캡처는 절대 포함해서는 안 됩니다. 보고는 <https://github.com/fox1245/NeoGraph/issues>에서 합니다.
