<!-- neograph-i18n: source=wasm/README.md locale=ko source_sha256=f54e1a1db994c330527456105294e83cafd203fe4ef6f37465cbd8476b292264 -->
# WebAssembly에서의 NeoGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

이 디렉터리에는 CI가 빌드하고 실행하는 WebAssembly 프로그램이 있습니다.

- `module.cpp`, `programs.cpp`, `neograph.mjs`는 JavaScript 모듈입니다. JSON으로 그래프를 컴파일해 실행하거나, Program(일시 정지, fork,
  모든 실행의 기록을 갖춘)을 컴파일하고 승인하고 실행하며, 모델 호출은 호스트의 `fetch`로 보냅니다. `neograph.d.ts`에 타입이 있습니다.
  `examples/`에는 이 모듈을 쓰는 Node 프로그램과 Cloudflare Workers 방식의 핸들러가 있고, `test/`에는 CI가 실행하는
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
| 런타임 | CI의 Node.js 22와 24. Workers 예제는 `Request`를 넘겨 Node에서 실행하며, workerd에서는 실행하지 않았습니다. 그래프 smoke는 `smoke.html`과 `serve_smoke.py`를 통해 Chromium에서도 실행됩니다(아래). 브라우저에서의 JavaScript 모듈, Deno, WASI는 검증하지 않았습니다. |
| SDK | SchemaProvider 0.4.0, 인터페이스 리비전 7. `cmake/NeoGraphSchemaProvider.cmake`가 고정한 커밋을 씁니다. libcurl 없이, 스레드 없이 빌드되며, SDK의 JavaScript 호스트(`sp_js_host`)가 모듈의 executor와 `fetch` 트랜스포트입니다. |
| 구성 요소 | Core, LLM(호스트를 통한 모델 호출), 그리고 Program(호스트 루프 위의 런타임. 저장소는 메모리에 있거나 호스트가 제공합니다). |

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
  -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON \
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
node wasm/test/program_test.mjs build-wasm/wasm
```

Program 저장소 테스트(JavaScript 저장소를 상대로 하는 적합성 스위트와 저장소 검사, 아래의 *JavaScript의 Program 저장소*와 *테스트*를 보세요)도
빌드하려면 configure 줄에 `-DNEOGRAPH_BUILD_WASM_STORE_TESTS=ON`을 더하세요. googletest를 내려받습니다. 그런 다음:

```bash
cmake --build build-wasm --target neograph_wasm_store_conformance neograph_wasm_store_checks
node wasm/test/store_conformance_test.mjs build-wasm/wasm
node wasm/test/js_stores_test.mjs build-wasm/wasm
```

`NEOGRAPH_BUILD_WASM_JS=ON`은 `build-wasm/wasm/`에 JavaScript 모듈을 만듭니다. `neograph_module.wasm`,
`neograph_module.mjs`(Emscripten 로더), `neograph.mjs`(이 디렉터리에서 복사한 API), `stores.mjs`(`neograph.mjs`가 import하는,
호스트 저장소로 가는 브리지), `neograph.d.ts`(타입)입니다. 앞의 네 파일을 프로젝트에 복사해 사용합니다.
`NEOGRAPH_BUILD_PROGRAM=ON`이면 모듈에 Program API가 들어갑니다([아래](#programs-in-the-module)). `NEOGRAPH_BUILD_QUICKJS_CONTROL=ON`은
JavaScript로 쓴 Program에 필요한 QuickJS 제어 런타임을 더하며, 없으면 문서 형태로 준 Program만 받아들입니다.
`NEOGRAPH_WASM_JS_PROGRAMS=OFF`는 그래프만 실행하는 모듈을 만들며, 크기는 약 절반입니다([측정한 크기](#measured-sizes)를 보세요).
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
작성한 도구와 노드는 이번 마일스톤에 포함되지 않습니다. Program에는 별도의 표면이 있으며, 아래에서 설명합니다.

<a id="programs-in-the-module"></a>
## 모듈의 Program

Program은 호스트가 컴파일하고 승인하는 작은 JavaScript 모듈로, 예산 아래에서 그래프를 실행합니다. 실행은 인터럽트에서 멈춰 답을
기다릴 수 있고, 재개하거나 fork할 수 있으며, 모든 단계의 기록을 저장소에 남깁니다([Program 가이드](../docs/program-guide.md)).
모듈은 Program을 JavaScript에서 컴파일하고, 승인하고, 호스트 이벤트 루프의 스레드 하나 위에서 실행합니다.

```js
import { load } from './neograph.mjs';

const SOURCE = `
export function define() {
  return ng.graph("two-turns")
    .channel("messages", {reducer: "append", initial: []})
    .node("first", {type: "llm_call"})
    .node("second", {type: "llm_call"})
    .entry("first").edge("first", "second").exit("second")
    .interruptBefore("second");                      // the run stops here and waits for an answer
}
export function* main(input) {
  const reply = yield ng.callCore("two-turns", input, "run");
  return {messages: reply.messages};
}
`;

const neograph = await load();
const programs = neograph.programs({
  provider: {
    baseUrl: 'https://api.example.com', path: '/v1/chat/completions', apiKey: env.API_KEY, model: 'a-model',
    catalogue: { version: 1, models: [{ id: 'a-model', input_limit: 4000, output_limit: 1000 }] },
  },
});
const version = await programs.admit(SOURCE);                  // compile and admit
const run = programs.start(version, {
  input: { messages: [{ role: 'user', content: 'Hello' }] },
  onEvent: (event) => console.log(event.kind),                 // optional: the run's events, in order
});
const paused = await run.result;                               // status 'interrupted'; paused.interrupt says where
const done = await run.resume('Go on').result;                 // the answer is the next message; 'completed'
console.log(done.output.messages.at(-1).content);
programs.release();
```

- **`neograph.programs({ provider, owner, limits, budget, stores })`**는 공급자 하나와 저장소 한 벌 위의 `Programs` 객체를 만들며,
  공급자가 잘못되었거나 모듈이 Program 지원 없이 빌드되었으면 `NeoGraphError`를 던집니다. `provider`는 `compile`과 같고 하나가
  더해집니다. `catalogue`는 SDK의 모델 카탈로그(`{ version: 1, models: [...] }`)이며, 내장 카탈로그 위에 덧씌워집니다.
  `owner`는 객체의 Program과 실행의 범위를 정합니다. `stores`는 호스트 자신의 저장소입니다([JavaScript의 Program 저장소](#program-stores-in-javascript)).
  생략한 저장소는 메모리에 있습니다. `limits`는 승인되는 Program에 줄 수 있는 최대치이고, `budget`은 예산을 주지 않은 실행의 예산이며,
  둘 다 아래처럼 씁니다.
- **`programs.admit(source)`**는 Program을 컴파일하고 승인한 뒤 `{ id, bundleId, name }`으로 결정됩니다. `source`는 JavaScript 제어
  소스의 텍스트, `{ javascript, name }`, 또는 이미 문서 형태인 Program을 위한 `{ document, name }`입니다. 컴파일러나 승인 정책이
  거부한 소스는 `compile` 또는 `admission` 타입의 `NeoGraphError`로 거부되며, `diagnostics`에 `{ code, message, pointer, witness }`가
  나열됩니다. 같은 소스를 다시 승인하면 같은 버전이 나옵니다.
- **`programs.start(version, { input, budget, runId, onEvent, signal })`**는 `ProgramRun`을 즉시 돌려줍니다.
  **`programs.run(version, options)`**은 `start(...).result`입니다.
- **`ProgramRun`**에는 `id`, `versionId`, `result`, `resume(value, options)`, `fork(value, options)`, `cancel()`이 있습니다.
  `result`는 실행이 끝나거나 답을 기다릴 때 결정됩니다. 실패하거나 취소된 실행은 그 상태로 결정되고, 호출 자체를 할 수 없을 때만
  거부됩니다(알 수 없는 버전, 버전이 승인하는 범위를 벗어난 예산, 이미 쓴 run id). `resume`은 실행이 기다리는 인터럽트에 답하고
  같은 id의 `ProgramRun`을 돌려줍니다. `fork`는 같은 일시 정지를 새 실행(`options.runId`가 이름을 정합니다)으로, 자기만의 답과
  함께 이어 가며, 일시 정지한 실행이 남긴 예산의 절반(또는 `options.budget`이 요청한 만큼이며, 일시 정지한 실행에 벽시계 시간과 Core 스텝이 조금은 남아야 합니다)을 가져가고,
  일시 정지한 실행은 계속 재개할 수 있게 둡니다. 끝난 노드는 다시 실행되지 않습니다. 한 일시 정지의 fork는 차례로 하세요. `cancel()`은 이 호출이 실행을 취소했는지로 결정됩니다.
  아직 보내지 않은 resume이나 fork(이어 갈 실행의 결과를 기다리는 중인 것)는 `cancel()`이나 중단된 `signal`이 대신 철회하며, 그 `result`는
  `cancelled` 오류로 거부됩니다. 이미 중단된 `signal`이면 호출은 이루어지지 않습니다.
- **`programs.result(runId)`**는 저장소가 실행에 대해 가진 것을 읽습니다. 가장 최근의 결과(기다리는 실행의 일시 정지도 결과입니다)이며, 아직 결과가 없으면 `{ runId, status: 'running' }`, 저장소가 그런 실행을 모르면 `null`입니다.
  **`programs.release()`**는 실행 중인 run을 취소하고 객체를 해제하며, 이미 한 호출은 먼저 끝납니다. 인터럽트에서 기다리는 실행은
  저장소에 있던 그대로 남습니다.
- **결과**는 `{ runId, versionId, status, output, trace, usage, remainingBudget, checkpointId?, interrupt?, failure? }`입니다.
  `status`는 `completed`, `interrupted`, `cancelled`, `budget_exhausted`, `timed_out`, `failed`, `ambiguous_effect`,
  `checkpoint_incompatible` 중 하나입니다. `output`은 `main()`이 돌려준 값입니다(실행이 기다리는 동안에는 `output.channels` 아래의
  그래프 상태). `trace`는 실행이 시작되거나 재개된 뒤 실행된 노드 목록입니다. `interrupt`는 `{ node, value, kind, pendingId }`이고,
  `failure`는 `{ code, message, node, attempts, witness }`입니다.
- **이벤트**는 런타임이 쓴 순서대로 오는 실행의 저널 항목 `{ sequence, kind, run_id, operation_id, payload, ... }`입니다. 모델의 답은
  `payload.type === 'llm_token'`인 이벤트로 옵니다. `onEvent`가 던진 오류는 그 실행을 취소하고 `result`를 그 오류로 거부하며,
  `signal`은 호출이 대기 중일 때 중단되면 호출을 취소하고, 호출이 결정된 뒤에는 효과가 없습니다(일시 정지한 실행에는 `cancel()`을 쓰세요). 런타임이 지원하지 않는 경우가 하나 있습니다. 첫 노드가 시작되기 전에 취소된 재개된 실행은 `P_TRANSITION_CONFLICT`와 함께 `failed`로 끝나고
  레코드는 `running`으로 남습니다. 런타임이 중단된 명령의 예약을 쥐고 있는 동안에는 취소를 게시할 수 없기 때문입니다. 재개된 실행은 노드가 시작된 뒤에 취소하거나, 끝나도록 두세요.
- **예산.** 예산은 `{ wallTimeMs, modelTokens, monetaryMicrounits, maxConcurrency, maxProgramOperations, maxCoreSteps, ... }`입니다.
  생략한 항목은 기본값을 유지합니다. 벽시계 시간 60초, 모델 토큰 10,000, 동시 분기 4, Program 연산 32, Core 스텝 100이며, 이는 JavaScript
  소스가 선언할 수 있는 최댓값이기도 합니다. 금액은 0부터 2^53까지의 정수여야 하고 이름은 예산의 멤버여야 하며, 아니면 호출이 거부됩니다.
  모델 호출은 최악의 경우가 남은 예산에 들어갈 때만 보냅니다. 최악의 경우는 모델의 입력 한도에 출력 한도를 더한 값이며, 카탈로그에서 읽습니다(출력은 `maxOutputTokens`로 줄 수도 있습니다).
  어떤 카탈로그도 한도를 주지 않는 모델은 호출하지 않으며, 실행은 `NG.ConfigError`와 함께 `failed`로 끝납니다. `catalogue`가 그 한도를 알려 줍니다. `modelTokens`에는 양수를 주세요. 0을 준 실행은 호출 전에는 제한되지 않으며, 런타임이 호출을 나중에 청구합니다.
- **오류.** `compile`과 `admission` 말고도, 런타임의 거부는 `code`(예를 들어 버전이 승인하는 범위를 벗어난 예산이면 `P_START_BUDGET`)와
  `witness`를 가진 `program` 타입의 `NeoGraphError`입니다.

모든 것은 호스트의 이벤트 루프 위에서 실행됩니다. 런타임은 저장소, 타이머, 모델 호출을 거기서 await하며 그중 무엇도 루프를 막지 않습니다.
제어 소스는 예외입니다. `define()`과 `main()`의 각 단계는 같은 스레드의 QuickJS에서 실행되므로, 끝나지 않는 소스는 시간 제한이
멈출 때까지 루프를 붙잡습니다. 시간 제한은 Program을 승인할 때 실행되는 `define()`에는 5초이고, `main()`의 단계에는 실행의 벽시계 시간 예산(기본 60초, `budget.wallTimeMs`)입니다.
QuickJS의 스택 제한은 이 빌드에서 동작하지 않으므로, 끝없이 재귀하는 소스는 엔진 자신의 스택을 넘치게 하며 모듈은 이를 잡을 수 없습니다. 호출은 결정되지 않고, 그 모듈 인스턴스는 쓸 수 없게 되며, Node에서는 호스트가 처리하지 않으면 잡히지 않은 `RangeError`로 프로세스가 종료됩니다. 호스트가 신뢰하거나 검증한 제어 소스만 실행하세요. 일시 정지한 그래프의 체크포인트는 모듈의 메모리에 있으므로, 실행은 그것을 시작한 `Programs` 객체가 살아 있는 동안에만 재개하거나 fork할 수 있습니다.

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
  루프에 기한을 알리고(`async::note_deadline`), 루프는 가장 이른 기한에 호스트 타이머 하나를 설정합니다. 이 타이머는 같은
  시각에 만료되는 Asio 타이머를 Asio가 디스패치하기 전에 실행될 수 있으므로(Asio의 Windows 리액터는 타이머가 만료되었음을
  자체 스레드로부터 전달받고, 호스트의 시계는 Asio의 시계보다 앞설 수 있습니다), 루프는 Asio가 그 기한까지의 타이머를
  디스패치할 때까지 기한을 유지하고 아직이면 1밀리초 뒤에 다시 확인합니다. `src/`나
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
Embind는 라이브러리와 모든 타입의 등록을 더하게 되는데, 표면은 몇 개의 함수뿐입니다. 그래프에는 `ng_compile`, `ng_run`, `ng_cancel`, `ng_release`, `ng_error`,
Program에는 `ng_programs_create`, `ng_programs_admit`, `ng_programs_start`, `ng_programs_resume`, `ng_programs_fork`, `ng_programs_cancel`,
`ng_programs_result`, `ng_programs_release`가 있습니다. 결과는 호스트 이벤트 루프의 이후 턴에서 호출되는 콜백으로 돌아옵니다. 그래프는 `Module.ngOnEvent`와
`Module.ngOnDone`, Program은 `Module.ngProgramEvent`와 `Module.ngProgramDone`입니다. 키는 `compile`에 넘기는 공급자 설명의 일부로, 그 그래프의 SDK 클라이언트가 보관합니다. 바이너리에도, 어떤
결과에도, 오류 메시지에도 들어 있지 않습니다.

## 호스트 루프 위의 Program

`program::ProgramRuntime`은 루프의 executor를 받아 모든 것을 그 위에서 실행합니다. Program의 attempt, 데드라인 타이머,
`ProgramHandle::wait_async`의 대기자가 그렇습니다. 스레드를 시작하지 않습니다. 읽고 쓰는 저장소는 awaitable이며,
이 빌드가 제공하는 것은 인메모리 저장소입니다.

```cpp
async::HostLoop loop(host_executor);                  // the sp::runtime::Executor the host runs
program::RuntimeConfig config{catalog, checkpoints, /*state_store=*/{}, transitions, /*scheduler_threads=*/1};
config.executor = loop.executor();                    // required here: a build without threads has no pool to fall back to
program::ProgramRuntime runtime(std::move(config));

loop.spawn(runtime.run_async(owner, version, invocation),
           [](std::exception_ptr error, program::ProgramResult result) { /* from a later turn */ });
```

- **필수.** `RuntimeConfig::executor`가 없으면 스레드 없는 빌드의 생성자는 `graph::ConfigurationFailure`인
  `ProgramExecutorRequired`를 던집니다. 네이티브 빌드에서는 지정하지 않으면 런타임의 자체 풀을 예전처럼 씁니다.
- **awaitable 형태.** 모든 연산에 있습니다. `start_async`, `reconnect_async`, `resume_handle_async`, `fork_async`,
  `replace_async`, `migrate_graph_async` 등입니다. 루프에서 실행되고 기다리는 코루틴의 executor에서 완료됩니다.
  `run_async`와 `resume_async`는 결과를 기다립니다.
- **동기 호출은 거부됩니다.** `start`, `resume`, `fork`, `run` 같은 동기 호출과, 결과가 아직 없을 때의
  `ProgramHandle::wait`는 Program을 실행해야 하는 스레드를 막게 됩니다. 이들은 아무 일도 하기 전에
  `program::ProgramBlockingCallRefused`(`graph::ConfigurationFailure`)를 던집니다. `operation()`은 호출을,
  `alternative()`는 awaitable 형태를 알려 줍니다. `hook_runtime`은 같은 이유로 생성자가 거부합니다.
- **타이머.** 런타임은 `async::HostTimer`로만 기다립니다. 이 타이머는 루프에 데드라인을 알리고, 취소되거나 다시
  설정되거나 소멸하면 그것을 거두므로 끝난 run은 호스트 타이머를 남기지 않습니다.
- **노드 본문.** executor가 있으면 자기 워커가 필요한 노드 attempt는 32스레드 프로세스 전역 풀 대신 그 executor의
  strand에서 실행되므로, 노드 본문은 자기 스레드를 막으면 안 됩니다.
- **락.** 저장소에 닿는 임계 구역은 코루틴이 막히지 않고 기다릴 수 있는 `OperationLock`을 쥡니다. `std::mutex`는
  메모리만 보호합니다.
- **awaitable 저장소.** Program 저장소 인터페이스 다섯 개는 `asio::awaitable`을 반환하며, 런타임의 모든 연산이 루프에서 이를 await합니다.
  진입점, 종료 게시, 자식 시작, Core 도구 grant 리졸버(`RuntimeConfig::core_tool_grant_awaitable_resolver`), `ProgramCatalog`의
  `_async` 형태, `ModuleResolver::resolve_async`가 그렇습니다. 인메모리 저장소처럼 연산이 즉시 완료되는 저장소는 턴을 쓰지 않고,
  호스트 루프의 이후 턴에서 완료되는 저장소도 동작합니다. 동기 형태는 같은 코루틴을 호출한 스레드에서 끝까지 실행하며, 위에서처럼 거부하므로
  기다리는 스레드만 돌릴 수 있는 루프를 기다리지 않습니다.
- **런타임을 소멸시키면** 실행 중인 run은 취소되지만 끝나기를 기다리지 않습니다. attempt가 끝날 때까지 런타임의
  상태를 붙들고 있으므로, 핸들이 종료 상태가 될 때까지 루프를 계속 돌리십시오.

<a id="program-stores-in-javascript"></a>
## JavaScript의 Program 저장소

WebAssembly에서 NeoGraph를 실행하는 호스트는 Program 런타임의 저장소를 JavaScript 객체로 제공합니다. C++ 쪽은 그 객체를 다섯 awaitable 저장소
인터페이스에 맞게 어댑트하므로, 런타임과 카탈로그는 그것을 다른 저장소와 똑같이 씁니다. 계약은 값과 결정을 옮길 뿐이고, 저장소가 그것으로 무엇을
하는지는 C++에 남으므로 호스트가 상태 기계를 쓸 일은 없습니다.

```js
const neograph = await load();
const programs = neograph.programs({
  provider,
  stores: { transitions, programs: programStore, fragments, modules },    // each member is optional
});
```

생략한 멤버는 모듈 자체의 인메모리 저장소이고, 멤버가 아닌 이름(오타 같은 것)은 거부됩니다. `transitions` 저장소는 아래에서 필수로 표시한 메서드뿐 아니라
`loadJavascriptCommands`, `loadMigrationPlan`, `loadRunLineage`, `loadGeneration`도 가져야 합니다. 실행은 명령 기록을, fork는 마이그레이션 증명을, 재개와 fork는 실행의 계보를 읽기 때문입니다. `Programs` 객체는 런타임이 그 객체로 만든 모든 저장소를 버릴 때까지 객체를 등록해 둡니다. 그 시점은
`programs.release()`가 취소한 실행이 자신의 끝을 게시한 뒤이므로, 객체를 해제해도 그 게시가 실패하지 않습니다. 모듈의 런타임은 `transitions`,
`programs`, `fragments`, `modules` 저장소를 씁니다. 모듈의 Program에는 부여할 도구가 없으므로 `grants`는 어댑터를 링크하는 C++ 코드를 위한 것입니다.

```js
import { installStoreBridge, registerStores, releaseStores, memoryStores } from './stores.mjs';

installStoreBridge(module);                       // once per module instance; load() does it for neograph.mjs
const handle = registerStores(module, {           // each member is optional
  transitions, programs, grants, fragments, modules,
});
// C++: neograph::wasm::JsStoreSet stores = neograph::wasm::adapt_js_stores(handle);
releaseStores(module, handle);
```

`wasm/js_stores.h`는 `adapt_js_stores(handle, lease)`를 선언하며, 이 함수는 `shared_ptr` 다섯 개의 `JsStoreSet`을 돌려줍니다(객체에 해당 종류의 저장소가
없으면 그 멤버는 null입니다). 호출할 때마다 같은 객체 위에 새 어댑터를 만들므로, 두 집합은 같은 데이터에 쓰는 두 작성자입니다. `lease`는 주어지면
모든 어댑터가 쥐고 있다가 마지막 어댑터와 함께 해제됩니다. 모듈의 `Programs` 객체는 그 삭제자에서 등록을 끝냅니다. `stores.mjs`는 JavaScript 쪽 절반이며
`neograph.mjs` 옆에 복사됩니다. `neograph_wasm_js_stores`를 링크한 모듈은 브리지가 부르는 함수(`ng_store_resolve`, `ng_store_reject`, `ng_store_alloc`,
`ng_store_free`)와 참조 저장소의 함수(`ng_memory_*`)를 export합니다.

**호출.** 저장소 멤버는 C++ 연산의 이름을 camelCase로 딴 메서드를 가진 객체이며, 메서드는 C++ 연산의 인자를 같은 순서로 받습니다. 메서드는 값 또는 그
값의 Promise를 돌려줍니다.

- thenable이 아닌 값은 기다리는 코루틴을 중단하지 않고, 호스트 루프의 턴도 거치지 않고 호출을 즉시 완료합니다. 그러면 동기 호출자(`Blocking...Store`)가
  기다릴 수 있습니다.
- Promise는 코루틴을 중단합니다. Promise가 결정되면 브리지가 task를 post하고, 코루틴은 루프의 한 턴에서 자기 executor 위에서 계속됩니다.
  `set_js_store_executor(executor)`는 완료를 다른 executor로 보내게 하며, 기본은 기다리는 코루틴이 실행되는 executor입니다.
- 인자는 코루틴이 중단될 수 있기 전에 호출 안으로 복사됩니다.
- Promise가 대기 중인 동안 기다리던 코루틴이 파괴되면(루프가 닫히거나 실행이 취소되면) 그 대기도 함께 사라집니다. 이후 도착한 결과는 받을 쪽이 없으므로 버려집니다.
- 호출은 직렬화되지 않습니다. 하나가 대기 중이어도 다른 호출을 할 수 있습니다.

**실패.** 던져진 예외나 거부된 Promise는 기다리는 호출에서 던져지는 C++ 예외가 되고, 런타임은 이를 실패한 저장소로 다룹니다(fail closed).
JavaScript 오류의 메시지, 또는 거부 값을 문자열로 바꾼 것을 담은 `std::runtime_error`입니다. `kind` 속성을 가진 오류는 대신 C++ 클래스를 지정합니다.
`invalid_argument`, `out_of_range`, `domain_error`, `length_error`, `logic_error`, `runtime_error` 중 하나이며, 다른 `kind`는 무시됩니다. 인메모리 저장소가 빈
소유자 범위를 `std::invalid_argument`로 거부하는 것처럼, 자기만의 이유로 호출을 거부하는 저장소는 이렇게 알립니다. 파싱되지 않거나 모양이 틀린 값은 메서드 이름을 담은
메시지(`JavaScript store method 'transitions.load' returned a malformed value: ...`)와 함께 같은 실패입니다. 어댑터는 실패한 호출에 기본값을 돌려주지 않습니다.

**값.**

| C++ | JavaScript |
|---|---|
| `std::string`, `std::string_view` | 문자열 |
| `std::uint64_t`(시퀀스, 세대, 개수) | 숫자: 2^53보다 작은 음이 아닌 정수 |
| `bool` | 불리언 |
| `std::optional<T>` | `T`의 값 또는 `null`(`undefined`는 `null`로 읽습니다) |
| `std::vector<T>` | 배열 |
| `ProgramTransitionPublishResult` | `"published"`, `"already_present"`, `"conflict"` |
| `ProgramActivationResult` | `"activated"`, `"already_present"`, `"conflict"` |
| `ProgramCoreToolGrantAdmission` | `"admitted"`, `"already_present"`, `"conflict"` |
| `TaskGraphPublishResult` | `"published"`, `"already_present"`, `"conflict"`, `"missing"` |
| `ModuleLifecycle` | `"active"`, `"deprecated"`, `"quarantined"`, `"revoked"` |
| `ProgramRunRecord`, `ProgramJournalRecord`, `ProgramEvent`, `ProgramEffectOutboxEntry`, `ProgramJavaScriptCommandJournalEntry`, `HookOutboxEntry`, `ProgramChildSynthesisRecord`, `MigrationPlan`, `ProgramRunLineage`, `ProgramRunGeneration`, `ProgramTransitionPublication`, `GraphMigrationCapsule`, `ProgramExecutionLease`, `ProgramRuntimeStateTransferReceipt`, `ContextEpoch`, `ContextArtifact`, `ProgramBundle`, `ProgramVersion`, `ProgramActivation`, `TaskGraphFragmentRecord`, `ProgramModule` | 문자열: 정규 텍스트, 즉 `serialize_canonical()`이며 타입의 `parse(bytes)`가 다시 읽습니다. 호스트는 그대로 저장하고 그대로 돌려줍니다. |
| `ProgramRunPublicationHead` | `{ run_record, journal_record }` |
| `ProgramCommandPublicationHead` | `{ run_record, journal_record, latest_command }`. `latest_command`는 명령 저널 항목 또는 `null`입니다 |
| `ProgramContextPublication` | `{ epoch, artifacts: [...], assembly_receipt }`, 각각 정규 텍스트입니다 |
| `ProgramEffectiveRuntimeState` | `{ context_publications: [ProgramContextPublication...], hook_outbox_entries: [...], inherited_hook_outbox_entries: [...], transfer_receipt }`. `transfer_receipt`는 정규 텍스트 또는 `null`입니다 |
| `ProgramActivationBinding` | `{ activation, version }`, 각각 정규 텍스트입니다 |
| `ProgramRetentionReport` | `{ versions_removed, bundles_removed, references: [{ version_id, reason }...] }` |
| `ProgramCoreToolGrantRecord` | `{ owner_scope, program_version_id, run_id, operation_id, attempt, binding_fingerprint, grant_id }` |
| `ProgramCoreToolGrantContext` | `grant_id`가 없는 같은 형태 |

객체는 나열된 멤버만 정확히 가집니다. 빠진 멤버나 남는 멤버는 잘못된 값입니다.

**메서드.** `ownerScope`, `runId` 등의 이름은 문자열입니다. 필수로 표시된 메서드는 있어야 합니다. 없으면 `adapt_js_stores`가
그 이름을 담아 `std::invalid_argument`를 던집니다. 선택 메서드가 없으면 C++ 인터페이스의 기본 동작이 유지되며, 마지막 열에 적었습니다.

| 멤버 | 메서드 | 결과 | 없을 때 |
|---|---|---|---|
| `transitions` | `load(ownerScope, runId)` | run 레코드 또는 `null` | 필수 |
| | `latest(ownerScope, runId)` | 저널 레코드 또는 `null` | 필수 |
| | `loadRunPublicationHead(ownerScope, runId)` | 게시 헤드 또는 `null` | `load`와 `latest`로 읽으며, 헤드가 움직이지 않았는지 보려고 다시 읽습니다 |
| | `loadEvents(ownerScope, runId, afterSequence)` | 이벤트 목록 | 필수 |
| | `loadEffects(ownerScope, runId, afterSequence)` | 이펙트 아웃박스 항목 목록 | 필수 |
| | `loadJavascriptCommands(ownerScope, runId, afterSequence)` | 명령 저널 항목 목록 | 실패 |
| | `loadCommandPublicationHead(ownerScope, runId)` | 명령 게시 헤드 또는 `null` | `load`, `latest`, `loadJavascriptCommands`로 읽습니다(그래서 그것들과 함께 실패합니다) |
| | `loadContextPublications(ownerScope, runId, afterSequence)` | 컨텍스트 게시 목록 | 실패 |
| | `loadHookOutboxEntries(ownerScope, runId)` | 훅 아웃박스 항목 목록 | 실패 |
| | `loadEffectiveRuntimeState(ownerScope, runId)` | 유효 런타임 상태 | 컨텍스트, 훅, run, 계보, 세대 읽기로 읽습니다 |
| | `loadChildSyntheses(ownerScope, runId)` | 자식 합성 레코드 목록 | 실패 |
| | `loadMigrationPlan(ownerScope, runId)` | 마이그레이션 계획 또는 `null` | `null` |
| | `loadLineage(ownerScope, lineageId)` | 계보 헤드 또는 `null` | `null` |
| | `loadRunLineage(ownerScope, runId)` | 계보 헤드 또는 `null` | `null` |
| | `loadLineageHead(ownerScope, lineageId, headId)` | 계보 헤드 또는 `null` | `null` |
| | `loadGeneration(ownerScope, lineageId, generation)` | 세대 또는 `null` | `null` |
| | `loadGenerationInitialPublication(ownerScope, lineageId, generation)` | 게시 또는 `null` | 실패 |
| | `loadGraphMigrationCapsule(ownerScope, runId, lineageHeadId)` | 캡슐 또는 `null` | `null` |
| | `loadExecutionLease(ownerScope, runId)` | 리스 또는 `null` | `null` |
| | `comparePublish(ownerScope, expectedJournalHead, publication)` | 게시 결과 | 필수 |
| | `comparePublishExecution(ownerScope, expectedJournalHead, publication, expectedLease, nextLease)` | 게시 결과. 리스는 정규 텍스트 또는 `null`입니다 | `"conflict"` |
| `programs` | `publishAdmitted(bundle, version)` | 없음 | 필수 |
| | `getBundle(id)`, `getBundle(ownerScope, id)` | 번들 또는 `null` | 필수 |
| | `getVersion(id)`, `getVersion(ownerScope, id)` | 버전 또는 `null` | 필수 |
| | `getActivation(ownerScope)` | 활성화 또는 `null` | 실패 |
| | `getActiveBinding(ownerScope)` | 활성화 바인딩 또는 `null` | `getActivation`과 `getVersion`으로 읽습니다 |
| | `compareActivate(ownerScope, expectedGeneration, versionId, policySnapshotHash)` | 활성화 결과 | 실패 |
| | `listVersions(ownerScope)` | 버전 목록 | 실패 |
| | `collectGarbage(ownerScope, pinnedVersionIds)` | 보존 보고 | 실패 |
| `grants` | `admit(record)` | 승인 | 필수 |
| | `load(context)` | grant 레코드 또는 `null` | 필수 |
| | `revoke(context)` | 불리언: grant를 철회했는지 | 필수 |
| `fragments` | `publish(record)` | 게시 결과 | 필수 |
| | `load(fragmentId)` | 프래그먼트 레코드 또는 `null` | 필수 |
| | `findPublishedLineage(ownerScope, parentRunId, expansionOperationId)` | 프래그먼트 레코드 또는 `null` | `null` |
| | `findPublished(ownerScope, parentRunId, expansionOperationId, proposalHash)` | 프래그먼트 레코드 또는 `null` | `null` |
| | `compareUpdate(fragmentId, expectedRevision, record)` | 게시 결과 | 필수 |
| `modules` | `publish(module)` | 없음 | 필수 |
| | `get(contentIdentity)`, `get(ownerScope, contentIdentity)` | 모듈 또는 `null` | 필수 |
| | `setLifecycle(contentIdentity, lifecycle)` | 없음 | 필수 |



- **한 메서드의 두 형태.** `getBundle`, `getVersion`, `get`은 C++에서 각각 소유자 없는 형태와 소유자가 붙은 형태를 가지며 카탈로그는 둘 다 씁니다.
  JavaScript 메서드 하나가 둘 다 처리하고, 인자의 개수가 둘을 구분합니다. 하나면 식별자이고, 둘이면 소유자 범위와 식별자입니다. 한 인자용으로 쓴 메서드는 다른 쪽에서 틀립니다.
- **게시는 넘어가기 전에 검사합니다.** `comparePublish`와 `comparePublishExecution`은 전이 저장소의 모든 백엔드가 시작하는 방식으로 시작합니다.
  저장된 상태가 필요 없는 검사(게시의 소유자, 기대하는 헤드의 모양, 이펙트 아웃박스, 게시의 정규 텍스트)입니다. 이를 통과하지 못한 게시는 `"conflict"`이며 객체에 닿지
  않습니다. 저장된 상태가 그 게시에 대해 무엇을 말하는지는 저장소가 결정합니다.
- **지원하지 않는 것.** `compare_publish_graph_safe_point`는 직렬화되지 않는 증거를 담으므로, JavaScript 저장소는 라이브 그래프 마이그레이션에 참여하지 않습니다.
  기반 클래스처럼 `"conflict"`로 답합니다.

**참조 저장소.** `neograph.memoryStores()`(`stores.mjs`의 `memoryStores(module)`)는 모듈의 인메모리 C++ 저장소를 호출해 이 계약을 구현하는
`{ transitions, programs, grants, fragments, modules }`를 돌려줍니다(`wasm/memory_stores.cpp`). 인자는 어댑터가 인코딩하는 것과 같은 표로 디코딩하고,
저장소 자신의 연산을 실행하고, 결과는 어댑터가 디코딩하는 표로 인코딩합니다. 메서드는 Promise가 아니라 값을 돌려주며, 인메모리 저장소의 거부는 `kind`를 통해
같은 C++ 예외 클래스로 어댑터에 닿습니다. 라이브러리에는 grant 저장소의 인메모리 형태가 없으므로, 참조 저장소는 SQLite grant 저장소의 판단을 따르는 작은 구현을 갖고 있습니다.
`stores.release()`는 집합을 버립니다. 그 위의 실행이 끝났을 때(`result`가 결정된 뒤) 하세요. 버려진 집합에 대한 호출은 실패하기 때문입니다.

영속 저장소가 필요한 호스트는 규칙을 다시 쓰지 않고 참조 저장소를 감쌉니다. 모든 메서드는 참조 저장소에 위임하고, 무언가를 바꾸는 호출의 인자는 호스트 자신의 스토리지에 쓰며,
시작할 때 감싼 객체가 그것을 순서대로 새 참조 저장소에 재생합니다. 참조 저장소는 각 호출을 그 인자와 그 이전의 상태로 결정하므로, 재생하면 로그가 설명하는 상태에 이릅니다.
아래 래퍼는 참조 저장소가 결정한 뒤, 결과를 돌려주기 전에 로그에 덧붙입니다. 그 사이의 크래시로 호출을 잃으면 안 되는 호스트는 로그 항목을 먼저 쓰고 호출을 그 뒤에 적용합니다.
`test/program_stores_test.mjs`는 Program 실행 전체에 대해 이를 합니다.

```js
const CHANGES = new Set(['transitions.comparePublish', 'transitions.comparePublishExecution', 'programs.publishAdmitted',
  'programs.compareActivate', 'programs.collectGarbage', 'grants.admit', 'grants.revoke', 'fragments.publish',
  'fragments.compareUpdate', 'modules.publish', 'modules.setLifecycle']);

function durable(reference, log) {                          // log.append(entry) is the host's storage
  const wrapped = {};
  for (const [member, target] of Object.entries(reference)) {
    wrapped[member] = {};
    for (const [method, call] of Object.entries(target)) {
      wrapped[member][method] = async (...args) => {
        const result = call(...args);                       // the reference store decides
        if (CHANGES.has(`${member}.${method}`)) await log.append({ member, method, args });
        return result;
      };
    }
  }
  return wrapped;
}
// On start: for (const { member, method, args } of log.entries()) fresh[member][method](...args);
```


## 예제

- `examples/node/chat.mjs`: Node에서 한 번 호출합니다.
  `API_KEY=... node wasm/examples/node/chat.mjs <base url> <path> <model> "<prompt>" [--stream]`.
- `examples/node/program.mjs`: 모델 호출 두 번과 그 사이의 일시 정지가 있는 Program을 Node에서 실행합니다.
  `API_KEY=... node wasm/examples/node/program.mjs <base url> <path> <model> "<prompt>" [--answer <text>]`.
  첫 답을 토큰 단위로 출력하고, 답(플래그, 또는 stdin의 한 줄. 빈 답은 실행을 취소합니다)을 기다린 다음 그 답으로 대화를 이어 갑니다.
- `examples/workers/worker.mjs`와 `wrangler.toml`: `POST {"message": "..."}`에 모델의 답을 JSON 또는 서버 전송 이벤트로
  돌려주는 요청 핸들러입니다. 템플릿에는 비밀이 없습니다. 키는 Worker 시크릿(`wrangler secret put API_KEY`)이고,
  `neograph_module.wasm`은 `WebAssembly.Module`로 import합니다.

## 테스트

CI는 모듈을 빌드해 Node 22와 24에서, 네트워크도 키도 없이 다음을 실행합니다.

- `test/llm_host_test.mjs`: 로컬 HTTP 서버가 Chat Completions 엔드포인트처럼 응답합니다. 버퍼링 응답과, 문자 하나가 두 청크에
  걸쳐 잘리는 서버 전송 이벤트 응답입니다. 테스트는 모듈로 노드 하나짜리 그래프를 실행해 최종 상태, 스트리밍 이벤트의 순서,
  나간 요청(실행 시점에 준 키와 모델), 공급자의 거부, `AbortSignal`에 의한 취소, 타임아웃, 동시 실행, 모델 호출이 진행
  중인 동안 호스트의 타이머가 계속 도는지를 확인합니다.
- `test/program_test.mjs`: 같은 서버를 상대로 하는 Program API. Program을 컴파일하고 승인하고(거부된 소스는 이유를 알려 줍니다), 실행은 이벤트를 스트리밍하고,
  인터럽트에서 멈추었다가 재개됩니다. fork는 첫 호출을 다시 묻지 않고 같은 일시 정지에 다른 답을 하고, 일시 정지한 실행은 취소되어 저장소에서 다시 읽히며,
  동시에 진행 중인 실행은 각자의 답을 지키고, `AbortSignal`은 모델 호출의 연결을 닫고, `onEvent`가 던진 오류는 그 실행을 취소합니다. 보내기 전에 취소된 fork는 철회되고, 이미 중단된 signal은 아무것도 시작하지 않으며, 끝나지 않는 제어 소스는 시간 제한이 멈춥니다. 실패로 남아야 하는 실패도 확인합니다.
  카탈로그에 한도가 없는 모델은 호출하지 않고, 호출을 감당하지 못하는 예산은 호출을 보내기 전에 실행을 소진시키고, 버전이 승인하는 범위를 벗어난 예산은 거부되며, 키는 어떤 결과나 이벤트에도 없습니다.
  Node 예제는 자식 프로세스로 실행합니다.
- `test/program_stores_test.mjs`: 모듈의 `stores` 옵션. 참조 저장소는 그대로, 그리고 이후 task에서 답하도록 감싼 채로 전체 수명 주기를 처리합니다(호출 사이에 호스트의 타이머가 계속 돕니다).
  거부된 호출, 던져진 오류, 값이 아닌 값은 저장소의 메시지와 메서드 이름과 함께 fail closed하고 모델은 호출되지 않습니다. 필수 메서드가 없는 객체는 거부됩니다.
  바뀐 것을 로그에 쓰는 래퍼 덕분에, 로그에서 재생한 저장소 위의 두 번째 객체가 끝난 실행을 다시 읽고, 멈춘 실행이 여전히 멈춰 있음을 보고, 첫 객체의 Program을 시작합니다.
  저장소가 아직 답하는 동안 취소된 fork는 취소로 끝나고, 오타 난 멤버와 빠진 계보 읽기 메서드는 거부되며, 객체를 해제하면 실행 중인 run은 취소되고 기다리는 run은 그대로 남습니다.
- `test/workers_test.mjs`: Workers 예제가 내보내는 `fetch` 핸들러를 같은 서버를 상대로 `Request`와 함께 호출합니다.
  버퍼링과 스트리밍 모두입니다. 핸들러의 로직과 플랫폼 API 사용을 보여 줄 뿐, workerd에서 실행하는 것은 아닙니다.
- `checks.cpp`(아래)는 호스트 펌프 공급자에 대한 동기 실행을 돌려 거부되는지, 아무것도 보내지 않았는지 확인합니다.
- `checks.cpp`는 `HostLoop` 위에 `ProgramRuntime`도 만듭니다. 실행기 없는 런타임과 동기 `start`는 거부되고, 인메모리
  저장소를 쓰는 노드 하나짜리 Program이 호스트의 턴에서 결과까지 실행됩니다.
  이어서 호스트 턴에서 awaitable 저장소로 run과 버전을 다시 읽고, 블로킹 뷰가 루프에서 기다리기를 거부함을 보입니다.
  또한 Program 저장소와 전이 저장소가 호출을 루프의 이후 턴에서만 완료하는 상태로 Program을 실행하고, 그런 저장소로 카탈로그를 읽습니다.
- `tests/test_program_runtime_executor.cpp`는 단일 스레드 `io_context`와 수동 실행기 위의 `HostLoop`에서 Program을
  네이티브로 실행하므로, 네이티브 빌드도 같은 경로를 다룹니다.
- `tests/test_program_host_pumped_stores.cpp`는 호출을 루프의 이후 턴에서만 완료하는 저장소 위의 `HostLoop`에서 Program의 수명 주기 전체를
  네이티브로 실행합니다. 시작, 모델 노드, 중단과 재개, 포크, 자식, Core 도구 grant, 완료, 취소를 다루며 아무것도 막히거나 거부되지 않습니다.
- `tests/test_program_store_awaitable.cpp`는 저장소 계약을 네이티브로 실행합니다. 인라인 저장소는 중단 없이 완료되고, 풀로
  넘기는 저장소는 기다리던 코루틴을 자기 executor에서 재개하며, 블로킹 뷰는 나중에 완료되는 저장소를 기다리거나 거부합니다.
- `tests/test_host_loop.cpp`는 수동 executor를 상대로 루프와 호스트 seam 공급자를 네이티브로 실행하므로, 네이티브 빌드도
  같은 경로를 다룹니다.
- `test/store_conformance_test.mjs`는 `tests/`의 저장소 적합성 스위트(`ProgramTransitionStoreConformance`, `ProgramStoreConformance`, 그래프 마이그레이션
  캡슐 행)를 JavaScript 저장소를 상대로 Node에서 실행합니다. 모듈은 `-DNEOGRAPH_BUILD_WASM_STORE_TESTS=ON`(googletest를 내려받습니다)으로
  타깃 `neograph_wasm_store_conformance`에서 빌드합니다. JavaScript 백엔드를 가진 스위트는 모든 케이스에 참조 저장소 위의 어댑터를 통한 저장소를 주며,
  두 형태로 실행합니다(`JavaScript`는 동기 호출자가 쓰는 연산을, `JavaScriptAwaited`는 awaitable 연산을 호출합니다). 스레드에서 두 작성자를 경쟁시키는 케이스는 스레드 하나에서
  차례로 실행하게 합니다. 스크립트는 백엔드별 개수와 건너뛴 모든 케이스의 이유를 출력하고, 실패가 있으면 0이 아닌 값으로 종료합니다.
- `test/js_stores_test.mjs`는 이후 task에서 결정되는 Promise를 돌려주는 메서드를 가진 저장소로 타깃 `neograph_wasm_store_checks`를 구동합니다. Promise는
  루프의 한 턴에서 기다리던 코루틴을 재개하고, 대기하는 동안 아무것도 기다리지 않습니다. Promise가 아닌 값은 턴을 쓰지 않습니다. 거부, throw, 파싱되지 않는 값은
  메서드 이름을 담은 C++ 예외가 됩니다. 둘을 섞은 객체도 동작합니다. 코루틴이 파괴되거나 취소된 뒤에 도착한 결과는 버려집니다. 필수 메서드가 없으면 `adapt_js_stores`가
  실패합니다. Program 전체가 Promise 저장소 위에서 실행되고, 거부하는 저장소 위에서는 fail closed합니다.

<a id="measured-sizes"></a>
## 측정한 크기

Release, `-O3 -flto`, Emscripten 6.0.13. 그래프 smoke는 Core 엔진과 SDK 런타임을 포함하고, 그래프 전용 모듈은 여기에 LLM 구성
요소, SDK의 JavaScript 호스트, C API를 더합니다. 나머지의 대부분은 Program 구성 요소입니다. Program이 있는 모듈은 그래프 전용 모듈의
두 배 크기이고, 그중 QuickJS가 약 1 MB입니다.

| 빌드 | `.wasm` | `.wasm` gzip -9 | `.js` |
|---|---:|---:|---:|
| 그래프 smoke, 싱글 스레드 | 5,615,386 B (5.4 MiB) | 1,642,238 B (1.6 MiB) | 83,223 B |
| 그래프 smoke, pthread | 5,448,684 B (5.2 MiB) | 1,660,246 B (1.6 MiB) | 103,691 B |
| Program, LLM, QuickJS를 포함한 checks, 싱글 스레드 | 12,495,665 B (11.9 MiB) | 3,741,204 B (3.6 MiB) | 91,040 B |
| JavaScript 모듈, 그래프 전용(`NEOGRAPH_WASM_JS_PROGRAMS=OFF`), 싱글 스레드 | 6,466,458 B (6.2 MiB) | 1,893,168 B (1.8 MiB) | 87,762 B |
| Program이 있고 QuickJS가 없는 JavaScript 모듈, 싱글 스레드 | 12,148,715 B (11.6 MiB) | 3,551,238 B (3.4 MiB) | 95,154 B |
| Program과 QuickJS가 있는 JavaScript 모듈, 싱글 스레드 | 13,209,571 B (12.6 MiB) | 3,931,479 B (3.7 MiB) | 96,889 B |

Program과 QuickJS가 있는 모듈을 `-DCMAKE_CXX_FLAGS_RELEASE="-Oz -DNDEBUG"`(C도 마찬가지)로 빌드하면 10,378,540 B, gzip한 크기 3,122,630 B입니다.
속도는 재지 않았습니다.

CI는 싱글 스레드 smoke를 `.wasm` 6,000,000바이트, gzip한 크기 1,750,000바이트(측정한 크기에 약 9%를 더한 값)로 제한하고,
모듈(CI는 Program과 QuickJS를 포함해 빌드합니다)은 `.wasm` 14,400,000바이트, gzip한 크기 4,300,000바이트로 제한하며, 크기를 잡 요약에 보고합니다. Worker 스크립트에는 요금제에 따른 크기 제한이 있으니,
모듈의 압축 크기를 사용하는 요금제의 제한과 비교하세요. 이전 버전의 이 문서에 있던 712 KB는 타입이 있는 SDK 이전에 잰 값이어서
더 이상 맞지 않습니다.

## 이 호스트에서 쓸 수 없는 것

NeoGraph가 호출을 소유하는 경우 타입이 있는 오류로 실패하고, 그렇지 않으면 빌드되지 않습니다.

- **호스트를 거치지 않는 모델 호출.** ASYNC, MCP 클라이언트, A2A는 소켓과 TLS가 필요합니다. 모델 호출은 위의 모듈로 호스트의
  `fetch`를 거칩니다. 다른 WebAssembly 호스트용 빌드는 자신의 executor와 트랜스포트를 `SchemaProvider`에 넘깁니다.
- **호스트 펌프 공급자에 대한 블로킹 진입점.** `run()`, `Provider::dispatch` 등은 거부합니다(위).
- **호스트 데이터베이스의 Program 저장소.** 저장소 인터페이스는 awaitable입니다.
  SQLite와 PostgreSQL 저장소는 시스템 라이브러리가 필요하므로 호스트 데이터베이스에는 직접 만든 저장소가 필요합니다.
  연산이 즉시 완료되는 저장소(호스트의 동기 스토리지)와 호스트의 이벤트 루프를 통해서만 완료되는 저장소 모두 `_async` 형태를 통해
  호스트 루프의 런타임에 쓸 수 있습니다([#557](https://github.com/fox1245/NeoGraph/issues/557)). 동기 Program
  호출은 호스트 루프에서 거부되므로 `_async` 형태를 쓰십시오. JavaScript 모듈은 그런 저장소를 JavaScript 객체(`stores`, 위)로 받습니다.
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

`smoke.html`은 `serve_smoke.py`로 서빙할 때 `http://127.0.0.1:8765/smoke.html`에서 생성된 `smoke.js`를 실행합니다.

```bash
python3 wasm/serve_smoke.py build-wasm/wasm --port 8765
```

`serve_smoke.py`는 `Cross-Origin-Opener-Policy: same-origin`과 `Cross-Origin-Embedder-Policy: require-corp`를 보냅니다.
`smoke.html`은 교차 출처 격리와 `SharedArrayBuffer`를 확인한 뒤 `/smoke.js`를 로드하며, 프로그램이 `doubled = 42`와
`trace = d`를 출력하고 표준 오류 출력 없이 종료 코드 `0`으로 끝나면 `document.body.dataset.result = "pass"`와
`document.body.dataset.exitCode = "0"`을 설정합니다. 기본 싱글 스레드 빌드로 헤드리스 Chromium 140.0.7339.186에서
검증했습니다(`doubled = 42`, `trace = d`, 종료 코드 `0`). pthread 빌드는 Node에서만 실행하며(`-sPROXY_TO_PTHREAD` 없이는
`main`이 메인 스레드를 막습니다), CI는 Node만 실행하고, JavaScript 모듈(`neograph.mjs`)은 브라우저에서 실행하지 않았습니다.

## 마일스톤

툴체인 검토 뒤의 계획은 [#535](https://github.com/fox1245/NeoGraph/issues/535)에 있습니다.

- **M0과 M1.** wasm32 수정, 싱글 스레드 빌드, CI의 Node smoke와 크기 게이트.
- **M2(이 문서).** 호스트의 `fetch`를 통한 모델 호출. 호스트 루프, 호스트 seam 공급자, JavaScript 모듈, Node와 Workers 예제.
- **M3.** 호스트 저장소 위의 Program([#557](https://github.com/fox1245/NeoGraph/issues/557),
  [#556](https://github.com/fox1245/NeoGraph/issues/556)). 런타임 쪽은 끝났습니다. `ProgramRuntime`이
  `HostLoop::executor()` 위에서 실행됩니다(위의 "호스트 루프 위의 Program"). 저장소 인터페이스도 awaitable이 되었습니다. JavaScript 표면도 마찬가지입니다(위의 "모듈의 Program").
- **M4.** 실시간 세션, WASI, 네이티브 아카이브용 보관 백엔드, 원격 Harness.
