<!-- neograph-i18n: source=docs/troubleshooting.md locale=ko source_sha256=7f1eeda183c21890031bb9b18196a7377f140553c17f4362974f8d7341588208 -->
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

## checkpoint와 PostgreSQL 실패

PostgresCheckpointStore export 누락은 wheel/source build가 component를 껐기 때문일 수 있다. 옛 wheel 목록이 아니라 해당 artifact 설정을 확인한다. 선택 target build에는 matching libpq가 필요하다. URI password의 특수 문자는 percent-encode하거나 libpq key=value 형식을 쓴다. 실제 credential은 공개하지 않는다.

async connect/reconnect는 모든 host/IP에 하나의 deadline을 쓴다. 명시적 양수 `connect_timeout=N`은 초 단위이며 `1`은 2초로 올림한다. absent/zero/negative 및 PGCONNECT_TIMEOUT/service file에만 둔 값은 async 기본 30초를 쓴다. sync libpq의 host별 timeout과 다르다.

권한이 있으면 store가 table을 만든다. CREATE 권한이 없으면 [PostgreSQL header](../include/neograph/graph/postgres_checkpoint.h)의 schema를 적용한다. pending-write capability 부재는 전체 super-step replay이지 외부 효과 exactly-once가 아니다. async-only backend는 AsyncCheckpointStore와 `adapt_async_checkpoint_store`를 쓰며 옛 상호 sync/async crossover는 쓰지 않는다.

## tracing adapter와 역사적 수정

Tracer adapter는 session close로 파괴되는 Span wrapper의 raw pointer가 아니라 기록 data를 소유해야 한다. [C++ tracing 예제](../examples/49_openinference.cpp)를 참고한다. Python contextvars는 C++ callback 경계를 자동으로 넘지 않는다. parent context를 명시적으로 전달/attach하고 engine compile 전에 wrapper를 설치한다.

역사적 수정에는 v0.1.8 top-level conditional_edges 수용, node 이중 실행 fallback 제거, 전환 전 httplib macro layout 불일치(issue #16)가 있다. 삭제된 provider signature를 복구하지 않는다. 여러 translation unit에서 header-only httplib를 쓰면 CPPHTTPLIB_OPENSSL_SUPPORT를 일관되게 정의해야 한다. 현재 typed-provider HTTP는 SDK libcurl이다.

opaque convenience vector property는 Python list가 아닐 수 있다. iteration/`list(value)`로 확인하고 ChatMessage.image_urls처럼 복사되는 sequence는 만들어서 할당한다. 반환된 copy 변경이 C++ request를 수정한다고 가정하지 않는다. 새 typed request/outcome은 옛 CompletionParams 예제와 별개다.

## 안전한 버그 보고

version/platform, 최소 topology와 호출, execution_trace/status, 가린 typed failure/stop/usage 근거를 제공한다. installed wheel인지 rebuilt checkout인지 밝힌다. credential, private prompt, encoded native request body, 가리지 않은 packet capture는 넣지 않는다. <https://github.com/fox1245/NeoGraph/issues>에 보고한다.
