<!-- neograph-i18n: source=docs/troubleshooting.md locale=ko source_sha256=44721ce0c61105b48ef97804504e90f95838e8412f062dd1296149ec1c803099 -->
# 문제 해결

**Languages:** [English](troubleshooting.md) | [한국어](troubleshooting.ko.md) | [日本語](troubleshooting.ja.md) | [简体中文](troubleshooting.zh-CN.md)

## 설치 artifact 확인

`neograph_engine.__version__`, Python 버전, OS/architecture, 설치 방식, 첫 오류를 기록한다. checkout과 옛 wheel의 API가 같다고 가정하지 말고 해당 릴리스의 wheel 파일을 확인한다. typed 전환은 레거시 provider 이름을 제거한다. 옛 wheel import는 새 binding 검사가 아니다.

공개 NeoGraph platform metadata에는 Linux, macOS, Windows가 있다. 현재 SchemaProvider runtime/archive 검증은 Linux/POSIX이며 macOS/Windows port는 새 wheel 사용 가능 주장을 하기 전에 별도 검증해야 한다. wheel tag나 compiler만으로 runtime 통합을 증명하지 않는다. WASM provider runtime 지원은 확립되지 않았다.

GLIBC symbol 누락은 wheel manylinux tag와 host glibc를 비교한다. `manylinux_2_34` artifact는 glibc 2.34 이상이 필요하다. Windows DLL 오류는 x64 Python과 누락된 dependency DLL을 확인한다. architecture만이 유일한 원인은 아니다. bundled 라이브러리를 개별 교체하지 않는다.

## source 설정과 누락된 의존성

Core는 `NEOGRAPH_BUILD_LLM=OFF`여도 `SchemaProvider::runtime`이 필요하다. NeoGraph 설정은 CMake 3.20+를 요구한다. 명시적 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`, installed package, 기본 revision-pinned 공개 archive fallback 순으로 해석한다. offline 설정은 matching SDK를 설치하고 `CMAKE_PREFIX_PATH`에 prefix, `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`를 지정한다. SDK source build는 C++20, Python, standalone Asio, yyjson, libcurl 7.88+, OpenSSL Crypto가 필요하며 fetched/source 통합은 NeoGraph의 체크인된 Asio/yyjson을 쓴다.

SDK 단독은 OpenSSL Crypto 최소 버전을 선언하지 않는다. NeoGraph 전체 HTTPS 설정과 wheel/sdist 경로는 async/MCP HTTP 의존성을 통해 OpenSSL 3가 필요하다. 그 build에는 Crypto header만 설치해서는 부족하다.

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="$SDK_PREFIX" \
  -DNEOGRAPH_BUILD_PYBIND=ON
cmake --build build -j
```

빌드 트리는 설치된 패키지가 아니다. `PYTHONPATH=build python -c "import neograph_engine"` 전에 `pip install 'pydantic>=2' certifi`로 wheel의 Python 의존성을 설치한다. pydantic이 없으면 import가 실패하고, certifi가 없으면 TLS는 호스트 신뢰 저장소를 사용한다.

`Could NOT find CURL`은 `NEOGRAPH_USE_LIBCURL`을 꺼서 해결되지 않는다. 이 flag는 NeoGraph 선택적 CurlH2Pool만 제어하며 SDK의 필수 transport는 아니다. libcurl 개발 파일(Debian/Ubuntu의 `libcurl4-openssl-dev`, Fedora의 `libcurl-devel`)과 일관된 toolchain/prefix를 제공한다.

SQLite와 PostgreSQL은 NeoGraph 선택 component다. 개발 package를 제공하거나 `NEOGRAPH_BUILD_SQLITE`/`NEOGRAPH_BUILD_POSTGRES`를 명시적으로 끈다. SDK runtime은 제거되지 않는다. header/API 변경 후 binding symbol이 해결되지 않으면 fresh build directory에서 CMake를 재설정하고 matching header/library로 모든 consumer를 재빌드한다.

GCC 13 coroutine ICE는 Stage 3 때 보고되었다. 적합한 compiler로 올리거나 해당 표현식을 재구성한다. 옛 workaround가 전체 SDK/platform 검증은 아니다. catch handler 안에서 `co_await`하지 않는다. C++ 자체가 금지한다. 오류를 저장하고 handler 밖에서 recovery를 await한다.

## 제거된 provider API와 typed 실패

`CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, descriptor-interpreter 설정, `prefer_libcurl`, Responses WebSocket 선택은 제거되었다. `SchemaProvider(load_provider_descriptor(descriptor_json), ProviderRuntimeOptions(...), SchemaProviderDefaults(...))`와 `make_provider_request`를 쓴다. 정확한 constructor는 [Python binding](python-binding.md), C++는 [이전 가이드](migration-v0.4-to-v1.0.md)를 참고한다.

`prepare`는 single-use `PreparedProviderRequest`를 반환하고 `dispatch`가 소비한다. `invoke`는 두 단계를 합친다. dispatch receipt 저장 후 admitted request를 재구성하거나 소비된 handle을 재시도하지 않는다. failure outcome은 진짜 error/partial 근거를 담은 data다. 성공 completion으로 읽지 말고 `outcome.failure`를 확인한다.

관측자 예외는 `ProviderObserverError`에 typed outcome과 cause를 보존한다. budget settlement와 terminal receipt persistence 오류도 진짜 outcome을 보존하지만 그것이 추가 send 권한은 아니다. 전달 미상은 blind retry가 아닌 reconciliation이 필요하다.

## streaming과 Python async 경계

`ProviderMode.Stream`을 명시한다. observer가 streaming을 선택하지 않는다. token display는 비어 있지 않은 content-text delta만 고른다. usage, reasoning, tool, raw-wire, envelope는 token이 아니다. C++ event view는 callback 중 빌리며 Python ProviderEvent는 빌린 데이터를 복사해 소유한다. 느린 observer는 host delivery capacity를 소비한다.

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

Provider invoke/dispatch는 GIL을 해제하는 동기 Python 메서드다. `asyncio.to_thread`를 쓰며 native asyncio awaitable은 아니다. callback 호출·복사·파괴는 GIL을 획득한다. 취소는 graph CancelToken으로 SDK에 stop을 전달한다. timeout이나 waiter 취소가 remote effect 부재를 증명하지 않는다.

continuation에는 tool/refusal/reasoning part를 포함한 전체 `ProviderMessage`를 보존한다. `ChatMessage`, `ToolCall`은 graph 편의 값이지 전체 provider history의 alias가 아니다. 사용량 누락은 `None`이며 0이 아니다. known-zero counter는 근거를 가진 UsageCount다. portable projection으로 native replay나 financial authority를 가져올 수 없으며 native continuation은 admitted NativeArchive custody로만 저장한다.

## TLS와 local endpoint

명시적인 trust bundle은 `ProviderRuntimeOptions.ca_file`로 지정한다. Python import는 기존 `SSL_CERT_FILE`을 보존하며 없으면 certifi가 있을 때 선택한다. runtime option은 이 CA file을 SDK libcurl에 전달한다. `NEOGRAPH_SKIP_CERT_AUTOFIX=1`은 host 설정을 그대로 둔다. endpoint/trust-store 오류를 숨기려고 인증 검증을 끄지 않는다.

v0.1.0–v0.1.6 wheel CA 경로의 ConnPool timeout은 역사적 문제이며 v0.1.7에 CA 자동 선택이 추가되었다. 현재 provider는 그 경로 대신 SDK libcurl을 쓴다. local HTTP는 raw URL override가 아니라 validated descriptor에 admitted data로 둔다. [예제 31](../examples/31_local_transformer.cpp)을 따른다. Responses WebSocket close=1000 지침은 삭제된 과거 transport에만 해당한다.

## 그래프 정의와 등록

unknown reducer/condition/node-type은 compile에 사용한 registry에 이름이 없다는 뜻이다. 기본 reducer는 `overwrite`, `append`다. custom reducer/condition/node factory는 compile 전에 등록한다. `has_tool_calls`, `route_channel`은 기본 condition이다. Python callback은 GIL 아래 실행된다.

```python
import neograph_engine as ng

ng.ReducerRegistry.register_reducer("sum",
    lambda current, incoming: (current or 0) + incoming)
ng.ConditionRegistry.register_condition("is_long",
    lambda state: "long" if len(state.get("messages") or []) > 10 else "short")
```

write의 channel 이름은 정확히 존재해야 한다. condition은 route label을 반환한다. open condition은 명시적 `default`를 쓸 수 있으나 없으면 unmatched label이 오류이고 closed condition은 선언 밖 label을 거부한다. `__start__` edge와 loop escape를 확인한다. `RunConfig.max_steps` 기본값은 `50`이다. 잘린 실행을 완료로 간주하지 말고 step-limit status를 확인한다.

`schema_version: 1`은 unknown/unconsumed key와 round-trip 손실을 거부하는 strict parsing이다. metadata는 `_`/`x-` annotation, barrier는 비어 있지 않은 `wait_for`, conditional edge는 `routes`를 쓴다. custom 등록 뒤 `ng.export_schema()`로 live schema를 내보내고 editor palette를 따로 유지하지 않는다. absent/zero version은 `0.x`에서 lenient하며 `ng.upgrade_topology()`는 무시된 data를 충돌 안전 annotation에 남긴다.

## fan-out과 관리

기본 worker_count `1`은 engine pool을 만들지 않는다. I/O branch는 suspend 시 겹치며 단일 caller 스레드의 CPU body는 직렬 실행된다. pool이 도움이 되면 동시 실행 전에 `set_worker_count(N)`/`set_worker_count_auto()`를 설정한다. native operation이 GIL을 해제하지 않으면 Python CPU callback은 여전히 직렬이다.

같은 engine의 run/resume 중 admin state/history/update/fork는 `std::logic_error`로 실패한다. 취소·drain하고 완료를 기다린 뒤 호출한다. 오류를 숨기지 않는다. 같은 thread_id 동시 실행은 checkpoint 순서가 미정이며 store를 공유하는 다른 engine은 host 조정이 필요하다. [동시성](concurrency.md)을 참고한다.

## Python Program 오류

`LocalProgramHost`와 QuickJS Program compiler에서 나오는 오류다. 배경 규칙은 [Program 및 QuickJS](python-binding.md#program-and-quickjs)에 있다.

| 오류 | 원인 | 해결 |
|---|---|---|
| node에서 발생한 `TypeError: get(): incompatible function arguments` | `GraphState.get`은 channel 이름만 받는다 | `state.get("value") or 0`을 쓴다 |
| `add_registered_node`, `add_registered_reducer` 또는 `add_registered_condition`에서 발생한 `ng.ProgramRegistryError` | node type, reducer 또는 condition이 이 process에 등록되어 있지 않다 | 먼저 `@ng.node`, `ng.NodeFactory.register_type`, `ng.ReducerRegistry.register_reducer` 또는 `ng.ConditionRegistry.register_condition`으로 등록한다 |
| `P_JS_DEFINE_MISSING` | module이 `define()`을 export하지 않는다 | `define()`을 export하고 Program의 graph 하나를 반환한다 |
| `P_JS_GRAPH_ENTRY`, 또는 이름이 `__start__`나 `__end__`인 node에 대한 `P_JS_GRAPH_ARGUMENT` | graph에 `.entry()`가 없거나 node가 예약된 이름을 쓴다 | `.entry()`를 호출하고 node 이름을 바꾼다. `.entry()`와 `.exit()`가 sentinel을 연결한다 |
| `P_CORE_E13` 또는 `P_CORE_E14` | node의 첫 conditional edge 옆에 static edge나 두 번째 conditional edge가 있으며, 어느 쪽도 발화하지 않는다 | 모든 target을 첫 conditional edge의 route에 넣는다 |
| `P_JS_CONTROL_COMMAND`: JavaScript command references a Core outside the admitted root | `ng.callCore`가 `define()`이 반환한 graph가 아닌 다른 graph를 지정한다 | `ng.graph(...)`에 전달한 이름을 쓴다 |
| `ValueError: Every policy budget ceiling except child budgets must be positive; zero: ...` | 나열된 ceiling field가 0이다 | Program이 model을 호출하지 않더라도 양수로 설정한다 |
| `ng.ProgramAdmissionError`의 `P_ADMIT_PROVIDER` | `llm_call` node가 호스트가 바인딩하지 않은 provider를 요구하거나, `llm_call` 또는 `intent_classifier` node가 `provider=` 없이 등록되었다 | `add_provider(...)`와 `add_registered_node("llm_call", ..., provider=...)`로 등록하고 `LocalProgramHost`에 `node_context=`와 `binding_label=`을 전달한다 |
| `P_MONEY_BUDGET`(`BudgetExhausted`) 또는 다른 `P_MONEY_*` 코드(`Failed`)이고 provider가 요청을 받지 않았다 | 바인딩된 호출의 예약이 `monetary_microunits`에 들어가지 않거나(0은 아무것도 부여하지 않음), 상한을 정할 수 없다: 알 수 없는 model 또는 origin, route ceiling 없는 router alias, 출력 cap 없음, 알려진 입력 한도 없음, 가격 카탈로그가 반영하지 않는 기능 | `monetary_microunits`를 늘리거나, `provider_controls.max_output_tokens`를 설정하거나, 카탈로그 항목이 있는 model을 쓴다. [가격 증거와 가격 카탈로그](reference-en.md#pricing-evidence-and-the-price-catalog) 참조 |
| `P_START_BUDGET`: `Program invocation budget <field>=<n> is outside its admitted bounds` | run budget이 `compile_admit`에 전달한 budget과 다르다 | 같은 budget을 전달하거나 다시 compile하고 승인(admission)한다 |
| `ng.spawn`에서 발생한 `P_CHILD_BINDING` | `LocalProgramHost`에는 child binding resolver가 없다 | child Program은 C++ host에서 실행한다 |
| `resume()`에서 발생한 `P_RUN_NOT_FOUND`: `run '<id>' was not found in this LocalProgramHost` | run이 다른 `LocalProgramHost`나 이미 소멸한 `LocalProgramHost`에 속한다 | run을 시작한 host를 통해 재개한다 |
| `P_CORE_BARRIER_STALLED` | barrier member가 join으로 이어지지 않는 route를 택해 join이 더 이상 발화할 수 없다 | 모든 member를 join이나 `__end__`로 라우팅한다. `P_CORE_E15`가 compile 시점에 이런 route를 경고한다 |
| `P_RUNTIME_TIMEOUT`: `Program wall-time budget (N ms) expired` | run이 `wall_time_ms`를 다 썼다. 중단된 operation의 텍스트는 `failure.witness["cause_message"]`에 있다 | 더 큰 `wall_time_ms`로 다시 compile하고 승인(admission)한다 |

실패한 `callCore`는 그 실패로 run을 끝내며 `main()`은 `try`/`catch`로 잡을 수 없다. Python에서 `result.status`와 `result.failure`를 확인한다.

## checkpoint와 PostgreSQL 실패

PostgresCheckpointStore export 누락은 wheel/source build가 component를 껐기 때문일 수 있다. 옛 wheel 목록이 아니라 해당 artifact 설정을 확인한다. 선택 target build에는 matching libpq가 필요하다. URI password의 특수 문자는 percent-encode하거나 libpq key=value 형식을 쓴다. 실제 credential은 공개하지 않는다.

async connect/reconnect는 모든 host/IP에 하나의 deadline을 쓴다. 명시적 양수 `connect_timeout=N`은 초 단위이며 `1`은 2초로 올림한다. absent/zero/negative 및 PGCONNECT_TIMEOUT/service file에만 둔 값은 async 기본 30초를 쓴다. sync libpq의 host별 timeout과 다르다.

권한이 있으면 store가 table을 만든다. CREATE 권한이 없으면 [PostgreSQL header](../include/neograph/graph/postgres_checkpoint.h)의 schema를 적용한다. pending-write capability 부재는 전체 super-step replay이지 외부 효과 exactly-once가 아니다. async-only backend는 AsyncCheckpointStore와 `adapt_async_checkpoint_store`를 쓰며 옛 상호 sync/async crossover는 쓰지 않는다.

## tracing adapter와 역사적 수정

Tracer adapter는 session close로 파괴되는 Span wrapper의 raw pointer가 아니라 기록 data를 소유해야 한다. [C++ tracing 예제](../examples/49_openinference.cpp)를 참고한다. Python contextvars는 C++ callback 경계를 자동으로 넘지 않는다. parent context를 명시적으로 전달/attach하고 engine compile 전에 wrapper를 설치한다.

역사적 수정에는 v0.1.8 top-level conditional_edges 수용, node 이중 실행 fallback 제거, 전환 전 httplib macro layout 불일치(issue #16)가 있다. 삭제된 provider signature를 복구하지 않는다. 여러 translation unit에서 header-only httplib를 쓰면 CPPHTTPLIB_OPENSSL_SUPPORT를 일관되게 정의해야 한다. 현재 typed-provider HTTP는 SDK libcurl이다.

opaque convenience vector property는 Python list가 아닐 수 있다. iteration/`list(value)`로 확인하고 ChatMessage.image_urls처럼 복사되는 sequence는 만들어서 할당한다. 반환된 copy 변경이 C++ request를 수정한다고 가정하지 않는다. 새 typed request/outcome은 옛 CompletionParams 예제와 별개다.

## 도구 호출, Hook, 프로토콜 오류

도구 호출, 노드 또는 서브그래프가 필수 Hook 차단을 만난 그래프 실행은 이제 `llm::Agent`가 늘 그랬듯이 그것을 감싼 `NodeExecutionError` 대신 `HookBoundaryBlocked` 그 자체를 던지며, 이에 대한 `ERROR` 이벤트는 내보내지 않습니다. `HookBoundaryBlocked`(Python: `ng.HookBoundaryBlocked`)를 잡고 `invocation_id`와 `state`를 읽으세요. 예를 들어 중첩된 엔진 실행에서 이를 발생시킨 Python 노드는 더 이상 재시도되지 않습니다. [도구 실패 계약](reference-en.md#tool-failure-contract)을 참고하세요.

엔진의 `HookRuntime`은 이제 그 도구 호출의 `BeforeToolExecution`과 `AfterToolExecution` Hook도 실행하며, Program 호스트의 Hook 런타임은 그 Core의 도구 호출의 Hook을 실행합니다. 그래프 실행이 이제 state `failed`와 메시지 `lifecycle hook could not run`을 가진 `HookBoundaryBlocked`로 멈춘다면, 실행에 owner scope(`RunMetadata::owner_scope`)를 주세요. owner scope가 없는 Hook 경계는 도구 Hook에서와 마찬가지로 체크포인트 Hook에서도 닫힌 채 실패합니다.

`state == "timed_out"`은 이제 Hook의 마감 시각이 지났거나 대상이 시간을 초과했다는 뜻입니다. 마지막 시도가 실패한 Hook은 `failed`를 보고합니다. 예전에는 `timed_out`을 보고했습니다.

상태가 `expired`이고 오류가 `run deadline elapsed before the tool started`인 도구 결과는 호출이 시작되기 전에 실행의 마감 시각이 지났다는 뜻이며, 도구와 그 Hook은 실행되지 않았습니다. 호출을 다시 시도하기보다 `RunMetadata::deadline`(Python `timeout_ms`, Program `wall_time_ms`)을 늘리세요. 실행의 마감 시각이나 클라이언트의 요청 타임아웃으로 멈춘 MCP 호출은 `timed_out`이며, 실행을 취소하면 진행 중인 MCP 호출이 즉시 멈춥니다.

`MCPServer`는 클라이언트가 취소한 `tools/call`에 stdio에서는 응답하지 않고 HTTP에서는 `-32800`으로 응답하며, Hook 차단에는 `-32010`, 인터럽트에는 `-32011`로 응답합니다. 그 밖의 모든 실패는 `_meta["io.neograph/failure"].code`가 이름을 알려 주는 `isError` 결과이고, 그 텍스트는 예외 텍스트가 아니라 공개 메시지입니다(텍스트를 고르려면 `public_message`를 가진 `NodeFailureError`를 발생시키세요). `OK` 응답에서 `RunGraphResponse.error`를 읽던 gRPC 클라이언트는 이제 상태를 읽어야 합니다. 실패한 실행은 `google.rpc.ErrorInfo.reason`이 코드인, OK가 아닌 상태입니다. [프로토콜 실패 매핑](reference-en.md#protocol-failure-mapping)을 참고하세요.

올바른 UTF-8이 아닌 도구 출력이나 예외 메시지는 예전에는 빈 도구 메시지, 빈 MCP나 gRPC 페이로드, 또는 Python의 `UnicodeDecodeError`로 도착했습니다. 이제는 잘못된 형식의 시퀀스마다 U+FFFD로 바뀌어 도착합니다.

## 안전한 버그 보고

version/platform, 최소 topology와 호출, execution_trace/status, 가린 typed failure/stop/usage 근거를 제공한다. installed wheel인지 rebuilt checkout인지 밝힌다. credential, private prompt, encoded native request body, 가리지 않은 packet capture는 넣지 않는다. <https://github.com/fox1245/NeoGraph/issues>에 보고한다.
