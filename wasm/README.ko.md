<!-- neograph-i18n: source=wasm/README.md locale=ko source_sha256=b356aef7a1ba972201c4a1d99a2769c2bd587dd8e021fee6ec4084c036b068cf -->
# WebAssembly에서의 NeoGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

이 디렉터리에는 CI가 빌드하고 실행하는 WebAssembly 프로그램이 있습니다.

- `module.cpp`와 `neograph.mjs`는 JavaScript 모듈입니다. JSON으로 그래프를 컴파일해 실행하며, 모델 호출은 호스트의 `fetch`로
  보냅니다. `examples/`에는 이 모듈을 쓰는 Node 프로그램과 Cloudflare Workers 방식의 핸들러가 있고, `test/`에는 CI가 실행하는
  오프라인 테스트가 있습니다(로컬 HTTP 서버가 모델을 대신합니다).
- `smoke.cpp`는 `DoubleNode` 하나로 이루어진 그래프를 컴파일하고, `doubled = seed * 2`를 쓰며, `InMemoryCheckpointStore`로
  실행합니다. `seed = 21`이면 `doubled = 42`와 `trace = d`를 출력합니다.
- `checks.cpp`는 스레드가 없는 32비트 호스트에서 달라지는 점(아래)을 검사합니다.

계획은 [#535](https://github.com/fox1245/NeoGraph/issues/535)에 있으며, 이것은 브라우저 SDK가 아닙니다.

## 상태

| 항목 | 상태 |
|---|---|
| 툴체인 | Emscripten 6.0.13. CI에서 emsdk 릴리스 커밋으로 고정합니다. |
| 빌드 모드 | 싱글 스레드(WebAssembly의 기본값)와 pthread. 둘 다 CI에서 빌드하고 실행합니다. JavaScript 모듈은 싱글 스레드입니다. |
| 런타임 | CI의 Node.js 22와 24. Workers 예제는 `Request`를 넘겨 Node에서 실행하며, workerd에서는 실행하지 않았습니다. 브라우저, Deno, WASI는 검증하지 않았습니다. |
| SDK | SchemaProvider 0.4.0, 인터페이스 리비전 7. `cmake/NeoGraphSchemaProvider.cmake`가 고정한 커밋을 씁니다. libcurl 없이, 스레드 없이 빌드되며, SDK의 JavaScript 호스트(`sp_js_host`)가 모듈의 executor와 `fetch` 트랜스포트입니다. |
| 구성 요소 | Core, LLM(호스트를 통한 모델 호출), 그리고 선택적으로 Program. |

## 빌드

빌드에는 Emscripten 6.0.13(C++20 코루틴, `<stop_token>`, `std::bit_cast`가 libc++에 있습니다), CMake 3.20 이상,
Ninja가 필요합니다. ASYNC, MCP 클라이언트, A2A, gRPC, SQLite와 PostgreSQL 저장소는 WebAssembly 호스트가 제공하지
않는 소켓, TLS, 시스템 라이브러리가 필요하므로 CMake가 거부하며, 아래에서는 꺼 둡니다. LLM은 호스트용으로
빌드됩니다(아래).

싱글 스레드, WebAssembly의 기본값:

```bash
source /path/to/emsdk/emsdk_env.sh

emcmake cmake -S . -B build-wasm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_WASM=ON -DNEOGRAPH_BUILD_WASM_JS=ON \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_ASYNC=OFF -DNEOGRAPH_BUILD_LLM=ON -DNEOGRAPH_BUILD_MCP=OFF \
  -DNEOGRAPH_BUILD_MCP_CLIENT=OFF -DNEOGRAPH_BUILD_MCP_SERVER=OFF -DNEOGRAPH_BUILD_MCP_HTTP_SERVER=OFF \
  -DNEOGRAPH_BUILD_A2A=OFF -DNEOGRAPH_BUILD_ACP=OFF -DNEOGRAPH_BUILD_GRPC=OFF \
  -DNEOGRAPH_BUILD_POSTGRES=OFF -DNEOGRAPH_BUILD_SQLITE=OFF -DNEOGRAPH_BUILD_UTIL=OFF \
  -DNEOGRAPH_BUILD_EXAMPLES=OFF -DNEOGRAPH_BUILD_TESTS=OFF -DNEOGRAPH_BUILD_BENCHMARKS=OFF \
  -DNEOGRAPH_USE_LIBCURL=OFF
cmake --build build-wasm --target neograph_wasm_smoke neograph_wasm_checks neograph_js
node build-wasm/wasm/smoke.js
node build-wasm/wasm/checks.js
node wasm/test/llm_host_test.mjs build-wasm/wasm
node wasm/test/workers_test.mjs build-wasm/wasm
```

`NEOGRAPH_BUILD_WASM_JS=ON`은 `build-wasm/wasm/`에 JavaScript 모듈을 만듭니다. `neograph_module.wasm`,
`neograph_module.mjs`(Emscripten 로더), `neograph.mjs`(이 디렉터리에서 복사한 API)입니다. 이 세 파일을 프로젝트에 복사해
사용합니다. `NEOGRAPH_BUILD_PROGRAM=ON`은 네이티브 플러그인 검사에만 필요합니다. 그래프 smoke와 모듈만 빌드하려면 끄세요.
pthread: configure 줄에 `-DNEOGRAPH_THREADS=ON`을 더하고, `NEOGRAPH_BUILD_WASM_JS`와 `NEOGRAPH_BUILD_LLM`은 끄고,
다른 빌드 디렉터리를 씁니다.

`NEOGRAPH_THREADS`가 두 모드를 가르는 유일한 스위치입니다. Emscripten과 WASI에서는 `OFF`, 그 밖에서는 `ON`입니다.

- **싱글 스레드(`OFF`).** `-pthread`도 `Threads::Threads`도 없습니다. `asio` 타깃이 `ASIO_DISABLE_THREADS`를 내보내고,
  SDK도 같은 방식으로 빌드합니다(`SP_THREADS=OFF`, libcurl 트랜스포트 없음). 작업은 호스트가 제공하는
  executor에서 실행됩니다.
- **pthread(`ON`).** 모든 곳에 `-pthread`를 쓰고, smoke 프로그램에는 워커 4개짜리 풀을 둡니다. SDK는
  `SP_THREADS=ON`으로 빌드되며, WebAssembly에서는 여전히 libcurl 트랜스포트가 없습니다.
- **예외.** `-fexceptions`(JavaScript 기반 예외)를 C 소스를 포함한 모든 NeoGraph 타깃에 지정하고, SDK도 같은 플래그를
  씁니다. 이미 `-fexceptions`나 `-fwasm-exceptions`를 지정한 설정은 그대로 두며, 예외가 꺼져 있으면 configure가
  실패합니다.
- **프로그램 하나에 설정 하나.** Asio는 헤더 전용이므로 스레드 설정이 서로 다른 번역 단위는 어떤 도구도 잡아내지 못하는
  ODR 위반이 됩니다. 생성되는 `neograph/build_config.h`가 설정을 기록하고, 모든 공개 헤더가 `neograph/api.h`를 통해
  이 헤더에 닿으며, 설정이 다른 번역 단위는 컴파일되지 않습니다. SDK도 자신의 설정을 같은 방식으로 기록하고
  (`sp/build_config.h`), 두 설정이 다르면 configure가 실패합니다.

## JavaScript 모듈

```js
import { load } from './neograph.mjs';

const neograph = await load();                      // a Worker passes its imported module: load({ wasmModule })
const result = await neograph.run({
  graph: {                                          // a graph definition, as JSON text or an object
    channels: { messages: { reducer: 'append' } },
    nodes: { llm: { type: 'llm_call' } },
    edges: [{ from: '__start__', to: 'llm' }, { from: 'llm', to: '__end__' }],
  },
  provider: {                                       // given at run time; nothing of it is in the module
    baseUrl: 'https://api.example.com',             // the origin of an OpenAI-compatible Chat Completions endpoint
    path: '/v1/chat/completions',
    apiKey: env.API_KEY,
    model: 'a-model',
  },
  input: { messages: [{ role: 'user', content: 'Hello' }] },
  onEvent: (event) => console.log(event),           // optional: streams the run
});
console.log(result.text);
```

- **`load(options)`**는 모듈을 한 번 인스턴스화합니다. Node와 브라우저에서는 `neograph_module.mjs` 옆의
  `neograph_module.wasm`을 찾습니다. Worker는 실행 중에 바이트를 컴파일할 수 없으므로, 번들러가 import한
  `WebAssembly.Module`을 `wasmModule`로 넘깁니다. `wasmBinary`와 `locateFile`은 Emscripten의 옵션입니다.
- **`neograph.compile(graph, provider)`**는 `graph.release()`까지 여러 번 실행할 수 있는 `Graph`
  (`graph.run(input, run)`)를 돌려줍니다. **`neograph.run({ graph, provider, input, ...run })`**은 컴파일하고, 한 번
  실행하고, 해제합니다.
- **`provider`**는 Chat Completions 엔드포인트라면 `{ baseUrl, path, apiKey, model }`입니다. `family`는 다른 와이어
  family를, `paths: { buffered, streaming }`은 다른 경로를 지정하고, `descriptor`에는 완전한 SchemaProvider
  descriptor를 넘길 수 있습니다. `instructions`, `maxOutputTokens`, `temperature`는 선택입니다. 키는 descriptor의
  family가 키를 보내는 방식대로 전송됩니다.
- **`run`** 옵션: `onEvent`(실행을 스트리밍합니다. 모델을 스트리밍 모드로 호출합니다), `signal`(실행과 모델 호출을
  취소하는 `AbortSignal`), `timeoutMs`.
- **결과**는 `{ status, text, output, trace, usage }`입니다. `status`는 `completed`, `interrupted`, `step_limit` 등이고,
  `text`는 마지막 어시스턴트 메시지, `output`은 최종 상태(`output.channels.<name>.value`), `trace`는 실행된 노드 목록,
  `usage`는 공급자가 보고하면 `input_tokens`, `output_tokens`, `total_tokens`를 가집니다.
- **이벤트**는 `{ type, node, data }`이며 `type`은 `node_start`, `node_end`, `llm_token`(`data`는 델타의 텍스트),
  `channel_write`, `interrupt`, `error` 중 하나로, 엔진이 만든 순서대로 옵니다.
- **오류**는 `NeoGraphError`입니다. `type`은 `provider`(모델 호출이 실패했습니다. `kind`, `reason`, `httpStatus`가
  경위를 알려 줍니다), `cancelled`, `error`(잘못된 그래프나 공급자, 또는 실패한 노드) 중 하나입니다. 잘못된 그래프나
  공급자는 `compile`에서 던집니다.

그래프에는 도구가 필요 없는 내장 노드 타입인 `llm_call`, `intent_classifier`, `subgraph`를 쓸 수 있습니다. JavaScript로
작성한 도구와 노드, 체크포인트 저장소, Program은 이번 마일스톤에 포함되지 않습니다.

## 동작 방식

### 두 이벤트 루프

NeoGraph의 그래프 엔진은 Asio `io_context` 위의 코루틴 집합입니다. WebAssembly 호스트가 스레드와 이벤트 루프를 소유하며,
모듈 안의 어떤 것도 이를 막아서는 안 됩니다. Asyncify도, JSPI도, 모듈 자체의 루프도 없습니다. SchemaProvider도 같은
문제를 갖고 있으며, 호스트 seam으로 해결합니다. 호스트가 실행하는 `Executor`와 HTTP를 보내는 `HostTransport`이고,
`sp_js_host`가 이를 `queueMicrotask`, `setTimeout`, `fetch` 위에 구현합니다. 모듈은 다음 네 가지로 엔진을 그 루프에
잇습니다.

- **`io_context`를 누가 펌프하는가.** `async::HostLoop`가 `io_context`를 소유하며 `run()`을 부르지 않습니다. 한 턴은
  루프 전용 SchemaProvider executor에 게시된 작업입니다(Client는 끝날 때 자신의 executor를 종료하므로 둘이 하나를 공유할
  수 없습니다). 턴은 실행 가능한 핸들러가 없을 때까지 `poll_one()`을 부르되 한 턴에 최대 256개까지만 실행하고, 일이
  남으면 호스트에 양보하며 다음 턴을 게시합니다. 루프의 executor에서 작업이 실행되면 턴이 예약됩니다.
  `HostLoop::executor()`는 `io_context`의 executor를 감싸서 그 위에서 핸들러를 실행하면 턴을 게시하게 하며, 한 번에 하나만,
  턴 안에서는 게시하지 않습니다. 실행 시작, 취소, SDK 완료가 모두 그렇게 합니다. 유휴 상태의 루프는 아무것도
  예약하지 않습니다.
- **SDK 완료가 그래프의 코루틴에 닿는 방법.** `llm_call`은 SDK 작업을 시작하고 깨움 채널에서 일시 중단됩니다. SDK는 fetch
  청크나 결과를 자신의 executor 작업, 즉 JavaScript 마이크로태스크에서 전달합니다. 콜백은 이벤트를 보관하고 채널에 신호를
  보내며, 채널은 코루틴의 continuation을 루프의 executor에 게시합니다. 그러면 턴이 예약되고, 그 턴에서 코루틴이 재개되어
  이벤트를 순서대로 실행의 옵저버에 넘기고, 결과가 오면 노드를 끝냅니다. SDK 콜백 안에서 인라인으로 실행되는 것은 없고,
  스레드도 락도 관여하지 않습니다.
- **`run_async`를 막지 않고 끝까지 구동하는 방법.** `HostLoop::spawn(awaitable, done)`은 `co_spawn`에 턴 하나를 더한
  것입니다. 즉시 돌아오고, `done(exception, result)`는 이후의 턴에서 호출됩니다. 모듈의 `ng_run`이 돌아오고 호스트의
  이벤트 루프가 계속되며, 실행이 끝나면 `ng_js_done`에서 JavaScript `Promise`가 이행됩니다.
- **Asio 타이머.** 재시도 백오프나 자원 대기 폴링은 Asio 타이머이고, 호스트의 루프는 Asio의 타이머 큐를 볼 수 없습니다.
  엔진은 `async::HostTimer`(`src/core/host_timer.h`)로 기다리며, 이 타이머는 Asio 타이머를 설정하는 호출 하나로
  루프에 기한을 알리고(`async::note_deadline`), 루프는 가장 이른 기한에 호스트 타이머 하나를 설정합니다. `src/`나
  `include/`의 일반 Asio 타이머는 호스트에서 절대 발화하지 않으므로 `host_timer_guard` 테스트가 이를 거부하며,
  호스트 루프에서 실행되지 않는 코드는 `scripts/host_timer_allowlist.txt`에 나열됩니다. `HostLoop` 아래에서 Asio
  타이머를 기다리는 직접 작성한 코드는 `async::note_deadline`을 스스로 호출합니다.

동기 진입점은 심볼을 유지한 채 거부합니다. `GraphEngine::run`, `resume`, `Provider::dispatch`와 `invoke`, 동기 엔드포인트 호출은
호출이 끝날 때까지 전용 `io_context`를 구동하는데, 이는 기다리는 그 스레드만 실행할 수 있는 호스트 루프를 기다리게
됩니다. 호스트 펌프 executor 위의 공급자(`Provider::host_pumped()`. 그렇게 선언하는 executor로 만든 `SchemaProvider`는
true입니다)에 이런 호출이 닿으면, 아무것도 보내기 전에 SDK의 `runtime.blocking.host_pumped` 이유(`ErrorKind::Misuse`,
재시도 안전성 `NotSent`)로 실패합니다. 메시지는 써야 할 `_async` 형태를 알려 줍니다. 호스트 펌프 공급자에 닿지 않는
실행은 smoke 프로그램처럼 여전히 `run()`으로 동작합니다.

### TLS와 OpenSSL

TLS는 호스트의 몫이며 `fetch`가 처리합니다. SDK에 libcurl 트랜스포트가 없는 곳(WebAssembly, 또는
`NEOGRAPH_THREADS=OFF` 빌드)에서 `neograph::llm`은 호스트 전용으로 빌드됩니다. CMake는 OpenSSL을 찾지 않고,
`neograph::llm`은 `neograph::async`도 `SchemaProvider::transport`도 링크하지 않으며, 네이티브 호스트에서 LLM, MCP 클라이언트,
A2A가 쓰는 `asio::ssl` HTTP 클라이언트인 `NEOGRAPH_BUILD_ASYNC`는 계속 거부됩니다. 그러면 `SchemaProvider`의 libcurl 생성자는
SDK의 `ConfigError`를 `/transport`에서 던집니다. 네이티브 빌드는 바뀌지 않습니다. 같은 라이브러리, 같은 링크 줄, 같은
생성자입니다.

### 호스트 seam 위에 공급자 만들기

`SchemaProvider`에는 두 번째 생성자 `SchemaProvider(descriptor, options, executor, transport, defaults)`와 이에 대응하는
`create`가 있습니다. SDK의 공개 `Client(descriptor, options, executor, transport)`로 전달하며, libcurl도 OpenSSL도
필요 없고 executor의 `host_pumped()` 선언을 받아 옵니다. 모듈은 컴파일된 그래프마다 executor 하나와 fetch 트랜스포트
하나를 만듭니다.

```cpp
auto executor  = sp::runtime::js::make_executor();
auto transport = sp::runtime::js::make_fetch_transport(executor);
auto provider  = neograph::llm::SchemaProvider::create(descriptor, sp::runtime::Options::with_api_key(key),
                                                       executor, transport);
```

### JavaScript 표면

모듈은 C API를 내보내고 `neograph.mjs`가 이를 감쌉니다. JSON 문자열을 쓰는 C API는 바인딩 라이브러리가 필요 없습니다.
Embind는 라이브러리와 모든 타입의 등록을 더하게 되는데, 표면은 `ng_compile`, `ng_run`, `ng_cancel`, `ng_release`, `ng_error`
다섯 함수뿐입니다. 결과는 호스트 이벤트 루프의 이후 턴에서 호출되는 두 콜백 `Module.ngOnEvent`와 `Module.ngOnDone`으로
돌아옵니다. 키는 `compile`에 넘기는 공급자 설명의 일부로, 그 그래프의 SDK 클라이언트가 보관합니다. 바이너리에도, 어떤
결과에도, 오류 메시지에도 들어 있지 않습니다.

## 예제

- `examples/node/chat.mjs`: Node에서 한 번 호출합니다.
  `API_KEY=... node wasm/examples/node/chat.mjs <base url> <path> <model> "<prompt>" [--stream]`.
- `examples/workers/worker.mjs`와 `wrangler.toml`: `POST {"message": "..."}`에 모델의 답을 JSON 또는 서버 전송 이벤트로
  돌려주는 요청 핸들러입니다. 템플릿에는 비밀이 없습니다. 키는 Worker 시크릿(`wrangler secret put API_KEY`)이고,
  `neograph_module.wasm`은 `WebAssembly.Module`로 import합니다.

## 테스트

CI는 모듈을 빌드해 Node 22와 24에서, 네트워크도 키도 없이 다음을 실행합니다.

- `test/llm_host_test.mjs`: 로컬 HTTP 서버가 Chat Completions 엔드포인트처럼 응답합니다. 버퍼링 응답과, 문자 하나가 두 청크에
  걸쳐 잘리는 서버 전송 이벤트 응답입니다. 테스트는 모듈로 노드 하나짜리 그래프를 실행해 최종 상태, 스트리밍 이벤트의 순서,
  나간 요청(실행 시점에 준 키와 모델), 공급자의 거부, `AbortSignal`에 의한 취소, 타임아웃, 동시 실행, 모델 호출이 진행
  중인 동안 호스트의 타이머가 계속 도는지를 확인합니다.
- `test/workers_test.mjs`: Workers 예제가 내보내는 `fetch` 핸들러를 같은 서버를 상대로 `Request`와 함께 호출합니다.
  버퍼링과 스트리밍 모두입니다. 핸들러의 로직과 플랫폼 API 사용을 보여 줄 뿐, workerd에서 실행하는 것은 아닙니다.
- `checks.cpp`(아래)는 호스트 펌프 공급자에 대한 동기 실행을 돌려 거부되는지, 아무것도 보내지 않았는지 확인합니다.
- `tests/test_host_loop.cpp`는 수동 executor를 상대로 루프와 호스트 seam 공급자를 네이티브로 실행하므로, 네이티브 빌드도
  같은 경로를 다룹니다.

## 측정한 크기

Release, `-O3 -flto`, Emscripten 6.0.13. 그래프 smoke는 Core 엔진과 SDK 런타임을 포함하고, 모듈은 여기에 LLM 구성
요소, SDK의 JavaScript 호스트, C API를 더합니다.

| 빌드 | `.wasm` | `.wasm` gzip -9 | `.js` |
|---|---:|---:|---:|
| 그래프 smoke, 싱글 스레드 | 5,509,570 B (5.3 MiB) | 1,608,986 B (1.5 MiB) | 83,223 B |
| 그래프 smoke, pthread | 5,448,684 B (5.2 MiB) | 1,660,246 B (1.6 MiB) | 103,691 B |
| Program과 LLM을 포함한 checks, 싱글 스레드 | 6,039,737 B (5.8 MiB) | 1,842,525 B (1.8 MiB) | 84,263 B |
| JavaScript 모듈, 싱글 스레드 | 6,270,525 B (6.0 MiB) | 1,835,562 B (1.8 MiB) | 87,762 B |

CI는 싱글 스레드 smoke를 `.wasm` 6,000,000바이트, gzip한 크기 1,750,000바이트(측정한 크기에 약 9%를 더한 값)로 제한하고,
모듈은 `.wasm` 6,850,000바이트, gzip한 크기 2,010,000바이트로 제한하며, 크기를 잡 요약에 보고합니다. Worker 스크립트에는 요금제에 따른 크기 제한이 있으니,
모듈의 압축 크기를 사용하는 요금제의 제한과 비교하세요. 이전 버전의 이 문서에 있던 712 KB는 타입이 있는 SDK 이전에 잰 값이어서
더 이상 맞지 않습니다.

## 이 호스트에서 쓸 수 없는 것

NeoGraph가 호출을 소유하는 경우 타입이 있는 오류로 실패하고, 그렇지 않으면 빌드되지 않습니다.

- **호스트를 거치지 않는 모델 호출.** ASYNC, MCP 클라이언트, A2A는 소켓과 TLS가 필요합니다. 모델 호출은 위의 모듈로 호스트의
  `fetch`를 거칩니다. 다른 WebAssembly 호스트용 빌드는 자신의 executor와 트랜스포트를 `SchemaProvider`에 넘깁니다.
- **호스트 펌프 공급자에 대한 블로킹 진입점.** `run()`, `Provider::dispatch` 등은 거부합니다(위).
- **Program.** Program 구성 요소는 컴파일되지만 `ProgramRuntime`이 스레드를 시작하므로, 싱글 스레드 빌드에서는 생성,
  노드 attempt 타임아웃, `n > 1`인 `set_worker_count(n)`이 `thread: Not supported`로 실패합니다(#557). 호스트의
  Program 저장소는 #556에서 다룹니다.
- **네이티브 Program 컨트롤.** C v1 플러그인 ABI는 64비트 레이아웃이므로 wasm32에서
  `NativeControlBinding::create`는 `NativeControlUnavailable`로 거부합니다.
- **동기 체크포인트 저장소.** 비동기가 아닌 저장소는 각 연산을 워커 스레드 풀에서 실행합니다. 싱글 스레드 빌드에는
  풀이 없으므로, 스레드를 기다리는 대신 `SynchronousCheckpointStoreUnavailable`로 연산을 거부합니다.
  `AsyncCheckpointStore`를 구현하세요. 내장 인메모리 저장소는 managed 예산을 포함한 모든 연산을 호출한 스레드에서
  실행하므로, `model_token_budget`이 있는 실행이 그 위에서 동작합니다.
- **기한이 있는 managed 예산.** 기한은 재시작 후에도 복구할 수 있는 시계 원점에 묶이는데 WebAssembly 호스트에는 그런
  것이 없습니다. 실행은 저장소에 묻기 전에 `ManagedBudgetDeadlineClockUnavailable`로 거부됩니다.
- **네이티브 아카이브.** 파일 보관(custody) 백엔드가 없어서 SDK가 `Unsupported` 오류로 거부합니다.
- **서브프로세스 위임**(Harness CLI, stdio MCP 서버), **수신 소켓**, **gRPC**, **파일 경로 업로드**, **Python**.

## 브라우저

브라우저 하니스는 저장소에 없습니다. `serve_smoke.py`는 존재하지 않는 `smoke.html`을 기대하며, 브라우저 실행 기록도
없습니다(#435). Node.js 실행은 브라우저에서 동작한다는 증거가 아닙니다. 모듈은 `web`, `worker`, `node` 환경용으로
빌드되지만 실행해 본 것은 Node뿐입니다.

## 마일스톤

툴체인 검토 뒤의 계획은 [#535](https://github.com/fox1245/NeoGraph/issues/535)에 있습니다.

- **M0과 M1.** wasm32 수정, 싱글 스레드 빌드, CI의 Node smoke와 크기 게이트.
- **M2(이 문서).** 호스트의 `fetch`를 통한 모델 호출. 호스트 루프, 호스트 seam 공급자, JavaScript 모듈, Node와 Workers 예제.
- **M3.** 호스트 저장소 위의 Program([#557](https://github.com/fox1245/NeoGraph/issues/557),
  [#556](https://github.com/fox1245/NeoGraph/issues/556)). `HostLoop::executor()`가 Program 런타임이 받게 될
  `any_io_executor`입니다.
- **M4.** 실시간 세션, WASI, 네이티브 아카이브용 보관 백엔드, 원격 Harness.
