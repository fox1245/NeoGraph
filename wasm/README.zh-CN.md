<!-- neograph-i18n: source=wasm/README.md locale=zh-CN source_sha256=8e30105731bb1ac98766f4394fa60e0e4f62118a6fc7b7636239550b62b97b13 -->
# WebAssembly 上的 NeoGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

本目录包含 CI 构建并运行的 WebAssembly 程序。

- `module.cpp`、`programs.cpp` 和 `neograph.mjs` 是 JavaScript 模块：从 JSON 编译图并运行，或者编译、准入并运行 Program（带暂停、分叉和每次运行的记录），
  模型调用通过宿主的 `fetch` 发送。类型在 `neograph.d.ts` 中。`examples/` 中有使用该模块的 Node 程序和 Cloudflare Workers 风格的处理程序，
  `test/` 中是 CI 运行的离线测试（由本地 HTTP 服务器代替模型）。
- `smoke.cpp` 编译一个只有一个 `DoubleNode` 的图，写入 `doubled = seed * 2`，并用 `InMemoryCheckpointStore` 运行；
  `seed = 21` 时输出 `doubled = 42` 和 `trace = d`。
- `checks.cpp` 检查没有线程的 32 位宿主上有所不同的地方（见下文）。

计划见 [#535](https://github.com/fox1245/NeoGraph/issues/535)，这不是浏览器 SDK。

## 现状

| 项目 | 状态 |
|---|---|
| 工具链 | Emscripten 6.0.13。CI 中固定到 emsdk 的发布提交。 |
| 构建模式 | 单线程（WebAssembly 上的默认值）和 pthread，二者都在 CI 中构建并运行。JavaScript 模块是单线程的。 |
| 运行时 | CI 中的 Node.js 22 和 24。Workers 示例在 Node 中以 `Request` 调用来运行，尚未在 workerd 上运行。图 smoke 也通过 `smoke.html` 和 `serve_smoke.py` 在 Chromium 中运行（见下文）；浏览器中的 JavaScript 模块、Deno 和 WASI 尚未验证。 |
| SDK | SchemaProvider 0.4.0，接口修订版 7，使用 `cmake/NeoGraphSchemaProvider.cmake` 固定的提交。可在没有 libcurl、没有线程的情况下构建，其 JavaScript 宿主（`sp_js_host`）就是模块的 executor 和 `fetch` 传输层。 |
| 组件 | Core、LLM（通过宿主的模型调用），以及 Program（宿主循环上的运行时；存储在内存中，或由宿主提供）。 |

## 构建

构建需要 Emscripten 6.0.13（其 libc++ 提供 C++20 协程、`<stop_token>` 和 `std::bit_cast`）、CMake 3.20 或更新版本以及 Ninja。
ASYNC、MCP 客户端、A2A、gRPC 以及 SQLite 和 PostgreSQL 存储需要 WebAssembly 宿主不提供的套接字、TLS 或系统库，
因此 CMake 会拒绝它们，下面也将它们关闭。LLM 按宿主方式构建（见下文）。

单线程，WebAssembly 上的默认值：

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

如果还要构建 Program 存储测试（针对 JavaScript 存储的一致性测试套件和存储检查，见下文“JavaScript 中的 Program 存储”和“测试”），
请在 configure 命令行中加上 `-DNEOGRAPH_BUILD_WASM_STORE_TESTS=ON`；它会获取 googletest。然后：

```bash
cmake --build build-wasm --target neograph_wasm_store_conformance neograph_wasm_store_checks
node wasm/test/store_conformance_test.mjs build-wasm/wasm
node wasm/test/js_stores_test.mjs build-wasm/wasm
```

`NEOGRAPH_BUILD_WASM_JS=ON` 会在 `build-wasm/wasm/` 中生成 JavaScript 模块：`neograph_module.wasm`、
`neograph_module.mjs`（Emscripten 加载器）、`neograph.mjs`（从本目录复制的 API）、`stores.mjs`（`neograph.mjs` 导入的、通向宿主存储的桥）和
`neograph.d.ts`（类型）。把前四个文件复制到项目中即可使用。有 `NEOGRAPH_BUILD_PROGRAM=ON` 时，模块带有 Program API（[见下文](#programs-in-the-module)）；
`NEOGRAPH_BUILD_QUICKJS_CONTROL=ON` 会加入用 JavaScript 编写的 Program 所需的 QuickJS 控制运行时，没有它就只接受以文档形式给出的 Program。
`NEOGRAPH_WASM_JS_PROGRAMS=OFF` 构建只运行图的模块，大小约为一半（见[实测大小](#measured-sizes)）。
pthread：在 configure 命令行中加上 `-DNEOGRAPH_THREADS=ON`，保持 `NEOGRAPH_BUILD_WASM_JS` 和 `NEOGRAPH_BUILD_LLM` 关闭，并使用另一个构建目录。

两种模式之间只有 `NEOGRAPH_THREADS` 这一个开关。它在 Emscripten 和 WASI 上为 `OFF`，其他平台为 `ON`。

- **单线程（`OFF`）。** 没有 `-pthread`，也没有 `Threads::Threads`。`asio` 目标导出 `ASIO_DISABLE_THREADS`，
  SDK 也以同样方式构建（`SP_THREADS=OFF`，没有 libcurl 传输层）。工作在宿主提供的 executor 上运行。
- **pthread（`ON`）。** 所有地方都使用 `-pthread`，smoke 程序带一个四个工作线程的线程池。SDK 以
  `SP_THREADS=ON` 构建；在 WebAssembly 上它仍然没有 libcurl 传输层。
- **异常。** 每个 NeoGraph 目标（包括 C 源文件）都设置 `-fexceptions`（基于 JavaScript 的异常），SDK 采用相同的标志。
  已经指定 `-fexceptions` 或 `-fwasm-exceptions` 的配置保持不变，异常被禁用时配置会失败。
- **一个程序只有一种设置。** Asio 只有头文件，因此在线程设置上不一致的翻译单元会构成没有任何工具能发现的
  单一定义规则违反。生成的 `neograph/build_config.h` 记录该设置，所有公开头文件都通过 `neograph/api.h` 到达它，
  与之不一致的翻译单元无法编译。SDK 以同样的方式记录自己的设置（`sp/build_config.h`），二者不同时配置会失败。

## JavaScript 模块

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

- **`load(options)`** 只实例化模块一次。在 Node 和浏览器中，它会在 `neograph_module.mjs` 旁边找到 `neograph_module.wasm`。
  Worker 不能在运行时编译字节码，所以把打包器导入的 `WebAssembly.Module` 作为 `wasmModule` 传入。`wasmBinary` 和
  `locateFile` 是 Emscripten 的选项。
- **`neograph.compile(graph, provider)`** 返回一个 `Graph`，可以运行任意多次（`graph.run(input, run)`），直到调用
  `graph.release()`。**`neograph.run({ graph, provider, input, ...run })`** 先编译，运行一次，然后释放。
- **`provider`** 对 Chat Completions 端点是 `{ baseUrl, path, apiKey, model }`。`family` 指定另一种线路协议 family，
  `paths: { buffered, streaming }` 指定另外的路由，`descriptor` 则接受完整的 SchemaProvider descriptor。`instructions`、
  `maxOutputTokens` 和 `temperature` 是可选的。密钥按 descriptor 的 family 发送密钥的方式发送。
- **`run`** 的选项：`onEvent`（对运行进行流式传输：以流式模式调用模型）、`signal`（取消运行和模型调用的
  `AbortSignal`）和 `timeoutMs`。
- **结果**是 `{ status, text, output, trace, usage }`：`status` 为 `completed`、`interrupted`、`step_limit` 等；`text` 是最后一条
  助手消息；`output` 是最终状态（`output.channels.<name>.value`）；`trace` 列出运行过的节点；提供方报告时，`usage` 含
  `input_tokens`、`output_tokens` 和 `total_tokens`。
- **事件**是 `{ type, node, data }`，`type` 为 `node_start`、`node_end`、`llm_token`（`data` 是增量的文本）、`channel_write`、
  `interrupt` 和 `error` 之一，顺序与引擎产生的顺序相同。
- **错误**是 `NeoGraphError`：`type` 为 `provider`（模型调用失败：`kind`、`reason` 和 `httpStatus` 说明原因）、`cancelled`
  或 `error`（无效的图或提供方，或失败的节点）。无效的图或提供方会在 `compile` 中抛出。

图可以使用不需要工具的内置节点类型：`llm_call`、`intent_classifier` 和 `subgraph`。用 JavaScript 编写的工具和节点
不属于本里程碑；Program 有自己的接口，见下文。

<a id="programs-in-the-module"></a>
## 模块中的 Program

Program 是一个由宿主编译并准入的小型 JavaScript 模块，它在预算之内运行一张图：运行可以在中断处暂停并等待回答，可以被恢复或分叉，并把每一步的记录留在存储里
（[Program 指南](../docs/program-guide.md)）。该模块用 JavaScript 编译、准入并运行 Program，运行在宿主事件循环的那一个线程上：

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

- **`neograph.programs({ provider, owner, limits, budget, stores, javascript })`** 创建一个基于一个提供方和一套存储的 `Programs` 对象；提供方无效，或模块构建时不带 Program 支持，
  就抛出 `NeoGraphError`。`provider` 与 `compile` 相同，只多一项：`catalogue`，即 SDK 的模型目录（`{ version: 1, models: [...] }`），它叠加在内置目录之上。`owner` 为该对象的
  Program 和运行划定范围。`stores` 是宿主自己的存储（[JavaScript 中的 Program 存储](#program-stores-in-javascript)）；省略的存储放在内存中。`limits` 是任何被准入的 Program 可获得的上限，
  `budget` 是没有给出预算的运行所用的预算，二者的写法见下。`javascript` 是 `{ stackBytes, defineTimeMs }`，即控制源码可以使用的栈，以及 `define()` 可以运行的时间，在预算之后说明。
- **`programs.admit(source)`** 编译并准入 Program，以 `{ id, bundleId, name }` 完成。`source` 是 JavaScript 控制源码的文本、`{ javascript, name }`，或者给已经是文档形式的 Program 用的
  `{ document, name }`。被编译器或准入策略拒绝的源码，会以 `compile` 或 `admission` 类型的 `NeoGraphError` 被拒绝，其 `diagnostics` 列出 `{ code, message, pointer, witness }`。再次准入同一份源码，得到同一个版本。
- **`programs.start(version, { input, budget, runId, onEvent, signal })`** 立即返回一个 `ProgramRun`。**`programs.run(version, options)`** 就是 `start(...).result`。
- **`ProgramRun`** 有 `id`、`versionId`、`result`、`resume(value, options)`、`fork(value, options)` 和 `cancel()`。`result` 在运行结束或等待回答时完成；失败或被取消的运行以该状态完成，
  只有调用本身无法发出时才被拒绝（未知的版本、超出该版本准入范围的预算、已经用过的 run id）。`resume` 回答运行所等待的中断，并返回 id 相同的 `ProgramRun`。`fork` 把同一次暂停作为新的运行
  （`options.runId` 为它命名）继续下去，带着它自己的回答，拿走暂停的运行所剩预算的一半（或 `options.budget` 所要求的数量，它必须给暂停的运行留下一些墙钟时间和 Core 步骤），并让暂停的运行仍可恢复；已经完成的节点不会再次运行。同一次暂停的多个分叉可以一起创建：它们依次轮流，每个分叉读取前面的分叉给暂停的运行留下的数量，并拿走其中的一半。要拿走暂停的运行所剩的全部墙钟时间或 Core 步骤的分叉会被拒绝（`P_FORK_SOURCE_BUDGET`），要求超过剩余数量的分叉也会被拒绝（`P_FORK_BUDGET`），两者都不是冲突。以同一个 `runId` 再次创建的分叉就是那个分叉，保有它最初得到的数量。已发出、在等待轮到自己时被取消的分叉，仍会在轮到它时拿走自己的份额，然后以取消结束。`cancel()` 以“这次调用是否取消了该运行”完成。
  尚未发出的 resume 或 fork（它在等待所继续的运行的结果）会被 `cancel()` 或已中止的 `signal` 撤回，其 `result` 以 `cancelled` 错误被拒绝；`signal` 一开始就已中止，则不会发出调用。
- **`programs.result(runId)`** 读取存储中关于某次运行的内容：它最新的结果（等待中的运行的暂停也算结果），还没有结果时为 `{ runId, status: 'running' }`，存储不认识该运行时为 `null`。**`programs.release()`** 取消正在执行的运行并释放对象，已经发出的调用先完成。
  在中断处等待的运行在存储中保持原样。
- **结果**是 `{ runId, versionId, status, output, trace, usage, remainingBudget, checkpointId?, interrupt?, failure? }`。`status` 是 `completed`、`interrupted`、`cancelled`、`budget_exhausted`、
  `timed_out`、`failed`、`ambiguous_effect` 或 `checkpoint_incompatible`。`output` 是 `main()` 返回的值（运行等待期间则是 `output.channels` 下的图状态）。`trace` 列出自运行开始或恢复以来执行过的节点。
  `interrupt` 是 `{ node, value, kind, pendingId }`，`failure` 是 `{ code, message, node, attempts, witness }`。
- **事件**是按运行时写入的顺序到达的运行日志条目 `{ sequence, kind, run_id, operation_id, payload, ... }`。模型的回复以 `payload.type === 'llm_token'` 的事件到达。`onEvent` 抛出的错误会取消该运行，
  并以该错误拒绝它的 `result`；`signal` 在调用尚未完成时中止，就取消该调用；调用一旦敲定则没有作用（对已暂停的运行请用 `cancel()`）。已恢复的运行如果在其 Core 调用开始之前被取消，或 `onEvent` 抛出错误，就会以 `cancelled` 结束，存储也将其保存为
  `cancelled`。恢复会为该调用预留墙钟时间、模型 token、金额和 Core 步骤；在调用用到其中任何一项之前就结束的运行，不会退回这些预留：
  它的 `remainingBudget` 中这四项为 0。在调用开始之后结束的运行，会取回调用没有用到的部分。
- **预算。** 预算是 `{ wallTimeMs, modelTokens, monetaryMicrounits, maxConcurrency, maxProgramOperations, maxCoreSteps, ... }`；省略的成员保持默认值：60 秒墙钟时间、10,000 个模型 token、
  4 个并发分支、32 个 Program 操作和 100 个 Core 步骤，这也是 JavaScript 源码能声明的最大值。数额是 0 到 2^53 的整数，名字必须是预算的成员，否则调用被拒绝。只有在最坏情况放得进剩余预算时才会发送模型调用：最坏情况是模型的输入上限加输出上限，
  从目录中读取（输出也可以用 `maxOutputTokens` 给定）。任何目录都没有给出上限的模型不会被调用，运行以 `NG.ConfigError` 结束为 `failed`。`catalogue` 用来说明这些上限。请给 `modelTokens` 一个正数：给 0 的运行在调用之前不受限制，运行时在调用之后才计费。
- **错误。** 除 `compile` 和 `admission` 之外，运行时的拒绝是 `program` 类型的 `NeoGraphError`，带 `code`（例如预算超出该版本准入范围时为 `P_START_BUDGET`）和它的 `witness`。

一切都在宿主的事件循环上运行：运行时在那里 await 它的存储、定时器和模型调用，这些都不会阻塞它。控制源码是例外：`define()` 和 `main()` 的每一步都在同一个线程的 QuickJS 中运行，
永不结束的源码会占住循环，直到时间限制把它停下：`define()` 在 Program 被准入时运行，限时 5 秒（`javascript.defineTimeMs` 可以在 1 到 3,600,000 毫秒之间另选一个时间）；`main()` 的各步受运行的墙钟时间预算限制（默认 60 秒，`budget.wallTimeMs`）。
QuickJS 的栈限制在这个构建中是开启的，所以无限递归、或嵌套得比解析器能跟上的更深的源码，会以诊断 `P_JS_RESOURCE_LIMIT` 失败，而不是让宿主的栈溢出：`admit` 以 `compile` 错误被拒绝，出现这种情况的 `main()` 会让其运行以 `failed` 结束。这一限制数的是模块自己在线性内存中的栈，对 `define()` 和 `main()` 同样适用，诊断 witness 中的 `max_stack_bytes` 报告生效的大小。开发者不用 `javascript.stackBytes` 另选大小时，它是 48 KiB。控制源码能走多深随大小增长，每 KiB 约 4 层嵌套调用：

| `stackBytes` | 嵌套调用 | 经过 `sort` 比较函数的调用 | 经过 `map` 回调的调用 | 源码中嵌套的括号、数组或对象字面量、调用 |
| --- | --- | --- | --- | --- |
| 48 KiB（49152，默认值） | 190 | 32 | 57 | 131 |
| 96 KiB | 382 | 64 | 115 | 265 |
| 128 KiB | 510 | 85 | 154 | 354 |

使用 512 KiB C 栈的原生构建，允许的嵌套调用约为模块在默认值下允许的四倍（x86-64 上的 GCC 约为 700 层，随编译器及其选项而异），源码中的嵌套字面量也约为四倍（约 500 层）。这一限制看不到由 `Object.getPrototypeOf` 或 `Object.isExtensible` 跟随的、没有陷阱的 Proxy 对象链：约 5,000 个这样的链仍会让宿主的栈溢出。嵌套得比解析器能跟上的更深的函数同样会被拒绝，但并不总是以这个诊断被拒绝：函数表达式（默认值下约 40 层）以 `P_JS_RESOURCE_LIMIT` 结束，而函数声明（约 180 层）以及默认大小下的箭头函数（约 85 层）以 `P_JS_EVALUATION` 和消息 `SyntaxError: missing formal parameter` 结束，因为引擎在该位置把解析器的溢出报告为语法错误。无论哪种情况，`admit` 都以 `compile` 错误被拒绝。

大小由开发者决定，模块只强制一项界限：`stackBytes` 是从 16384 到模块自己的栈所能容纳的大小之间的整数，在本目录构建的模块里是 983,040（1 MiB 的栈减去 64 KiB，这 64 KiB 是引擎自己的预留，以及引擎启动时栈上已有的帧）。更大的值、不是整数的值、不存在的成员，都会在调用 `programs()` 时被拒绝，错误信息会写明界限。宿主自己的调用栈能承受多少，模块读不到，而最先耗尽的正是它：宿主的栈比模块的栈消耗得更快，普通调用约快 1.5 倍，试过的形态里最差的一种，即对嵌套很深的数据做 `JSON.stringify`，约快 6 到 7 倍。在宿主栈约 984 KiB 的 Node 22 和 24 上，分别用优化编译器和基线编译器（`node --liftoff-only`）测量，`wasm/test/program_test.mjs` 所拒绝的 17 种形态（经过调用、getter、Proxy 陷阱、`sort`、`map` 和 `JSON.parse` 回调的递归，构造函数和生成器，经过 5,000 个 bound 函数才到达的构造函数，对嵌套 100,000 层的数据做 `JSON.stringify` 和 `Array.prototype.flat`，对嵌套 100,000 层的文本做 `JSON.parse`，以及嵌套 2,000 层的源码）在 48、64、96、128 和 144 KiB 下都以 `P_JS_RESOURCE_LIMIT` 结束。到 160 KiB 时，对嵌套数据做 `JSON.stringify` 会让宿主的栈溢出，到 192 KiB 时对该数据做 `Array.prototype.flat` 也会，256 KiB 时有七种形态会，512 KiB 时有十四种。把宿主的栈减半（`node --stack-size=492`）后，所有形态都能被拦住的最大大小是 64 KiB，96 KiB 时同样的两种形态会溢出。所以宿主栈的八分之一（Node 的栈约为 123 KiB）对这些形态都能拦住，四分之一则不能。默认的 48 KiB 是最先溢出的大小的三分之一。`stackBytes` 只按源码的需要调高，并保持在运行它的宿主调用栈的八分之一以内（Worker、浏览器标签页和 Node 进程的栈各不相同），而且在信赖某个设置之前，先在那个宿主上用无限递归的源码试一试：宿主的栈承受不了的大小，会以下面描述的陷阱告终。

既要在原生又要在模块中运行的源码，应当保持在模块的限制之内。原生构建数的是运行源码的线程的 C 栈，默认 512 KiB（`define()` 用 `JavaScriptCompileLimits::max_stack_bytes`，`main()` 用 `RuntimeConfig::javascript_stack_bytes`），模块数的是自己的栈，默认 48 KiB，而一次调用在两者中用掉的字节数不同。因此同一份源码可能在一边被拒绝、在另一边却能运行：面向两者的源码，要按上表中较小的数字来写（190 层嵌套调用、131 层嵌套字面量），或者把两个限制设成测量过的深度。

准入不受信任的控制源码的宿主，应当在可以丢弃的 Worker 里进行。上面的限制能挡住试过的递归形态，但 WebAssembly 中的陷阱无法在同一个模块实例里恢复：宿主的栈一旦溢出，调用永远不会敲定，该实例随之无法使用，在 Node 中，若宿主不处理，未捕获的 `RangeError` 会使进程退出。请只在模块中运行宿主信任或已审查过的控制源码。

暂停的图的检查点保存在模块的内存里，所以运行只有在启动它的 `Programs` 对象存活期间才能被恢复或分叉。

## 工作原理

### 两个事件循环

NeoGraph 的图引擎是 Asio `io_context` 上的一组协程。WebAssembly 宿主拥有线程和事件循环，模块中的任何东西都不能阻塞它：
没有 Asyncify，没有 JSPI，也没有模块自己的循环。SchemaProvider 有同样的问题，并用宿主接缝解决：宿主运行的 `Executor`
和发送 HTTP 的 `HostTransport`，`sp_js_host` 用 `queueMicrotask`、`setTimeout` 和 `fetch` 实现它们。模块分四步把引擎接到
那个循环上。

- **谁来驱动 `io_context`。** `async::HostLoop` 拥有它，并且从不调用 `run()`。一个回合是投递到循环专属的 SchemaProvider
  executor 上的任务（Client 结束时会关闭自己的 executor，所以二者不能共用一个）；它调用 `poll_one()`，直到没有就绪的
  处理程序，每回合最多 256 个，仍有工作时再把控制权交还宿主并投递下一个回合。当工作在循环的 executor 上被执行时，会安排一个
  回合：`HostLoop::executor()` 包装了 `io_context` 的 executor，使得在其上执行处理程序就会投递一个回合，一次只投递一个，
  且从不在回合内部投递。启动运行、取消运行和 SDK 完成都会这样做。空闲的循环不安排任何东西。
- **SDK 的完成如何到达图的协程。** `llm_call` 启动 SDK 操作，并在唤醒通道上挂起。SDK 在自己 executor 的任务（即 JavaScript
  微任务）中交付 fetch 数据块或结果。回调保存事件并向通道发信号，通道把协程的 continuation 投递到循环的 executor。
  这会安排一个回合，在该回合中协程恢复，按顺序把事件交给本次运行的观察者，结果到达时结束节点。没有任何东西在 SDK
  回调内部内联运行，也不涉及线程或锁。
- **如何在不阻塞的情况下驱动 `run_async` 到结束。** `HostLoop::spawn(awaitable, done)` 就是 `co_spawn` 加一个回合：它立即
  返回，`done(exception, result)` 在之后的回合中被调用。模块的 `ng_run` 返回，宿主事件循环继续，运行结束时 JavaScript 的
  `Promise` 由 `ng_js_done` 兑现。
- **Asio 定时器。** 重试退避或资源等待轮询都是 Asio 定时器，而宿主的循环看不到 Asio 的定时器队列。引擎通过
  `async::HostTimer`（`src/core/host_timer.h`）等待，它在设置 Asio 定时器的同一次调用中把截止时间告知循环
  （`async::note_deadline`），循环为最早的截止时间设置一个宿主定时器。该定时器可能先于 Asio 分发在同一时刻到期的定时器而运行
  （Asio 的 Windows reactor 由自己的线程得知定时器已到期，宿主的时钟也可能比 Asio 的时钟快），所以循环会保留截止时间，直到
  Asio 分发了该时间之前到期的定时器，在此之前每隔 1 毫秒再查看一次。`src/` 或 `include/` 中未经包装的 Asio 定时器在宿主上
  永远不会触发，因此 `host_timer_guard` 测试会拒绝它，从不在宿主循环上运行的代码列在 `scripts/host_timer_allowlist.txt`
  中。在 `HostLoop` 下等待 Asio 定时器的自有代码需要自己调用 `async::note_deadline`。

同步入口保留符号并予以拒绝。`GraphEngine::run`、`resume`、`Provider::dispatch` 和 `invoke` 以及同步的端点调用会驱动一个私有的
`io_context` 直到调用结束，这会去等待一个只有等待者所在线程才能运行的宿主循环。当这样的调用到达宿主驱动 executor 上的提供方
（`Provider::host_pumped()`；由声明如此的 executor 构造的 `SchemaProvider` 为 true）时，它会在发送任何东西之前，以 SDK 的
`runtime.blocking.host_pumped` 原因（`ErrorKind::Misuse`，重试安全性 `NotSent`）失败。消息会指出应使用的 `_async` 形式。
不会到达宿主驱动提供方的运行，仍然可以像 smoke 程序那样通过 `run()` 工作。

### TLS 与 OpenSSL

TLS 属于宿主：由 `fetch` 负责。在 SDK 没有 libcurl 传输层的地方（WebAssembly，或 `NEOGRAPH_THREADS=OFF` 的构建），
`neograph::llm` 仅按宿主方式构建：CMake 不查找 OpenSSL，`neograph::llm` 既不链接 `neograph::async` 也不链接
`SchemaProvider::transport`，而 `NEOGRAPH_BUILD_ASYNC`（原生宿主上 LLM、MCP 客户端和 A2A 使用的 `asio::ssl` HTTP 客户端）
仍然被拒绝。此时 `SchemaProvider` 的 libcurl 构造函数会在 `/transport` 处抛出 SDK 的 `ConfigError`。原生构建不变：相同的库、
相同的链接行、相同的构造函数。

### 在宿主接缝上构建提供方

`SchemaProvider` 有第二个构造函数 `SchemaProvider(descriptor, options, executor, transport, defaults)`，以及对应的 `create`。
它转发到 SDK 公开的 `Client(descriptor, options, executor, transport)`，不需要 libcurl 和 OpenSSL，并沿用 executor 的
`host_pumped()` 声明。模块为每个编译后的图创建一个 executor 和一个 fetch 传输层：

```cpp
auto executor  = sp::runtime::js::make_executor();
auto transport = sp::runtime::js::make_fetch_transport(executor);
auto provider  = neograph::llm::SchemaProvider::create(descriptor, sp::runtime::Options::with_api_key(key),
                                                       executor, transport);
```

### JavaScript 接口

模块导出 C API，`neograph.mjs` 对其进行封装。使用 JSON 字符串的 C API 不需要绑定库；Embind 会多出一个库和每个类型的注册，
而接口只有几个函数：图用 `ng_compile`、`ng_run`、`ng_cancel`、`ng_release` 和 `ng_error`，Program 用 `ng_programs_create`、`ng_programs_admit`、`ng_programs_start`、
`ng_programs_resume`、`ng_programs_fork`、`ng_programs_cancel`、`ng_programs_result` 和 `ng_programs_release`。结果通过回调返回，它们在宿主事件循环之后的回合中被调用：
图用 `Module.ngOnEvent` 和 `Module.ngOnDone`，Program 用 `Module.ngProgramEvent` 和 `Module.ngProgramDone`。密钥是交给 `compile` 的提供方描述的一部分，由该图的 SDK
客户端持有；它不在二进制文件中，不在任何结果中，也不在错误消息中。

## 宿主循环上的 Program

`program::ProgramRuntime` 接收循环的 executor，并把所有工作都放在上面执行：Program 的 attempt、截止时间定时器，以及
`ProgramHandle::wait_async` 的等待者。它不会启动任何线程。它读写的存储是 awaitable 的，这个构建提供的是内存存储。

```cpp
async::HostLoop loop(host_executor);                  // the sp::runtime::Executor the host runs
program::RuntimeConfig config{catalog, checkpoints, /*state_store=*/{}, transitions, /*scheduler_threads=*/1};
config.executor = loop.executor();                    // required here: a build without threads has no pool to fall back to
program::ProgramRuntime runtime(std::move(config));

loop.spawn(runtime.run_async(owner, version, invocation),
           [](std::exception_ptr error, program::ProgramResult result) { /* from a later turn */ });
```

- **必须设置。** 没有 `RuntimeConfig::executor` 时，没有线程的构建的构造函数会抛出 `ProgramExecutorRequired`（一个
  `graph::ConfigurationFailure`）。原生构建在未设置时仍使用运行时自己的线程池，和以前一样。
- **awaitable 形式。** 每个操作都有：`start_async`、`reconnect_async`、`resume_handle_async`、`fork_async`、
  `replace_async`、`migrate_graph_async` 等。它们在循环上运行，并在等待它们的协程的 executor 上完成。`run_async` 和
  `resume_async` 会等待结果。
- **同步调用会被拒绝。** `start`、`resume`、`fork`、`run` 等同步调用，以及结果尚未就绪时的 `ProgramHandle::wait`，
  会阻塞本该运行 Program 的线程。它们在做任何事之前就抛出 `program::ProgramBlockingCallRefused`
  （一个 `graph::ConfigurationFailure`）。`operation()` 给出被拒绝的调用，`alternative()` 给出 awaitable 形式。
  `hook_runtime` 出于同样的原因被构造函数拒绝。
- **定时器。** 运行时只通过 `async::HostTimer` 等待。这种定时器会把截止时间告知循环，并在被取消、被重新设置或被销毁时
  撤回，所以已结束的运行不会留下宿主定时器。
- **节点主体。** 有 executor 时，需要自己的工作线程的节点 attempt 在该 executor 的 strand 上运行，而不是使用 32 线程的
  进程级线程池，所以节点主体不能阻塞自己的线程。
- **锁。** 会访问存储的临界区持有 `OperationLock`，协程可以不阻塞地等待它；`std::mutex` 只保护内存。
- **awaitable 存储。** 五个 Program 存储接口都返回 `asio::awaitable`，运行时的每个操作都在循环上 await 它们：入口、终态发布、子运行启动、
  Core 工具 grant 解析器（`RuntimeConfig::core_tool_grant_awaitable_resolver`）、`ProgramCatalog` 的 `_async` 形式以及
  `ModuleResolver::resolve_async`。像内存存储那样操作立即完成的存储不占用回合，在宿主循环的后续回合才完成的存储也能工作。
  同步形式在调用线程上把同一个协程运行到结束，并如上所述予以拒绝，而不是去等待只有等待线程才能转动的循环。
- **销毁运行时**会取消其运行，但不会等待它们结束。attempt 会持有运行时的状态直到结束，所以要让循环一直运行，
  直到各句柄进入终态。

<a id="program-stores-in-javascript"></a>
## JavaScript 中的 Program 存储

在 WebAssembly 中运行 NeoGraph 的宿主，把 Program 运行时的存储作为一个 JavaScript 对象提供。C++ 一侧把该对象适配成五个 awaitable 存储接口，所以运行时和目录像使用任何其他存储一样使用它。
契约只搬运值和决定；存储如何处理它们仍留在 C++ 中，所以宿主不需要写状态机。

```js
const neograph = await load();
const programs = neograph.programs({
  provider,
  stores: { transitions, programs: programStore, fragments, modules },    // each member is optional
});
```

省略的成员是模块自己的内存存储，不是成员的名字（比如拼错的）会被拒绝。`transitions` 存储除了下面标为必需的方法，还必须有 `loadJavascriptCommands`、`loadMigrationPlan`、`loadRunLineage` 和 `loadGeneration`：
运行要读取命令历史，分叉要读取迁移证明，恢复和分叉要读取运行的谱系。`Programs` 对象会让该对象一直保持注册，直到运行时丢弃了用它创建的每一个存储，那是在被 `programs.release()` 取消的运行发布完它们的终结之后，所以释放对象不会让这些发布失败。
模块的运行时使用 `transitions`、`programs`、`fragments` 和 `modules` 存储。模块中的 Program 没有可授权的工具，所以 `grants` 是给链接这些适配器的 C++ 代码用的：

```js
import { installStoreBridge, registerStores, releaseStores, memoryStores } from './stores.mjs';

installStoreBridge(module);                       // once per module instance; load() does it for neograph.mjs
const handle = registerStores(module, {           // each member is optional
  transitions, programs, grants, fragments, modules,
});
// C++: neograph::wasm::JsStoreSet stores = neograph::wasm::adapt_js_stores(handle);
releaseStores(module, handle);
```

`wasm/js_stores.h` 声明了 `adapt_js_stores(handle, lease)`，它返回由五个 `shared_ptr` 组成的 `JsStoreSet`（对象没有该种存储时，对应成员为 null）。每次调用都会在同一个对象上创建新的适配器，
所以两个集合就是同一份数据上的两个写入者。`lease` 如果给出，由每个适配器持有，并随最后一个适配器一起释放；模块的 `Programs` 对象在它的 deleter 中结束注册。`stores.mjs` 是 JavaScript 一半，
复制在 `neograph.mjs` 旁边。链接了 `neograph_wasm_js_stores` 的模块会导出桥所调用的函数（`ng_store_resolve`、`ng_store_reject`、`ng_store_alloc`、`ng_store_free`）以及参考存储的函数（`ng_memory_*`）。

**调用。** 存储成员是一个对象，其方法以 C++ 操作命名（camelCase），并按顺序接受 C++ 操作的参数。方法返回值，或该值的 Promise。

- 不是 thenable 的值会立即完成调用，既不挂起等待的协程，也不占用宿主循环的回合。同步调用方（`Blocking...Store`）这时可以等待它。
- Promise 会挂起协程。它敲定时，桥会 post 一个任务，协程在循环的某个回合里，在它自己的 executor 上继续。`set_js_store_executor(executor)` 可以指定另一个 executor 来接收完成；默认是等待的协程所在的 executor。
- 参数在协程可能挂起之前就被复制进调用。
- Promise 悬而未决时，如果等待的协程被销毁（它的循环被关闭，或它的运行被取消），它的等待也随之消失。之后到达的结果找不到接收者，会被丢弃。
- 调用不会被串行化：一个调用悬而未决时，可以发起另一个调用。

**失败。** 抛出的异常或被拒绝的 Promise 会变成从等待的调用中抛出的 C++ 异常，运行时把它当作失败的存储处理（fail closed）。它是带有 JavaScript 错误消息（或被转成字符串的拒绝值）的 `std::runtime_error`。
带有 `kind` 属性的错误改为指定 C++ 类：`invalid_argument`、`out_of_range`、`domain_error`、`length_error`、`logic_error` 或 `runtime_error`；其他 `kind` 会被忽略。因自己的理由拒绝调用的存储，
就像内存存储以 `std::invalid_argument` 拒绝空的所有者范围那样，用这种方式表明。无法解析或形状不对的值是同一种失败，消息中带有方法名
（`JavaScript store method 'transitions.load' returned a malformed value: ...`）。适配器绝不会为失败的调用返回默认值。

**值。**

| C++ | JavaScript |
|---|---|
| `std::string`, `std::string_view` | 字符串 |
| `std::uint64_t`（序号、代、计数） | 数字：小于 2^53 的非负整数 |
| `bool` | 布尔值 |
| `std::optional<T>` | `T` 的值，或 `null`（`undefined` 按 `null` 读取） |
| `std::vector<T>` | 数组 |
| `ProgramTransitionPublishResult` | `"published"`, `"already_present"`, `"conflict"` |
| `ProgramActivationResult` | `"activated"`, `"already_present"`, `"conflict"` |
| `ProgramCoreToolGrantAdmission` | `"admitted"`, `"already_present"`, `"conflict"` |
| `TaskGraphPublishResult` | `"published"`, `"already_present"`, `"conflict"`, `"missing"` |
| `ModuleLifecycle` | `"active"`, `"deprecated"`, `"quarantined"`, `"revoked"` |
| `ProgramRunRecord`, `ProgramJournalRecord`, `ProgramEvent`, `ProgramEffectOutboxEntry`, `ProgramJavaScriptCommandJournalEntry`, `HookOutboxEntry`, `ProgramChildSynthesisRecord`, `MigrationPlan`, `ProgramRunLineage`, `ProgramRunGeneration`, `ProgramTransitionPublication`, `GraphMigrationCapsule`, `ProgramExecutionLease`, `ProgramRuntimeStateTransferReceipt`, `ContextEpoch`, `ContextArtifact`, `ProgramBundle`, `ProgramVersion`, `ProgramActivation`, `TaskGraphFragmentRecord`, `ProgramModule` | 字符串：规范文本，即 `serialize_canonical()`，由该类型的 `parse(bytes)` 读回。宿主原样存储、原样返回。 |
| `ProgramRunPublicationHead` | `{ run_record, journal_record }` |
| `ProgramCommandPublicationHead` | `{ run_record, journal_record, latest_command }`；`latest_command` 是命令日志条目或 `null` |
| `ProgramContextPublication` | `{ epoch, artifacts: [...], assembly_receipt }`，各为规范文本 |
| `ProgramEffectiveRuntimeState` | `{ context_publications: [ProgramContextPublication...], hook_outbox_entries: [...], inherited_hook_outbox_entries: [...], transfer_receipt }`；`transfer_receipt` 是规范文本或 `null` |
| `ProgramActivationBinding` | `{ activation, version }`，各为规范文本 |
| `ProgramRetentionReport` | `{ versions_removed, bundles_removed, references: [{ version_id, reason }...] }` |
| `ProgramCoreToolGrantRecord` | `{ owner_scope, program_version_id, run_id, operation_id, attempt, binding_fingerprint, grant_id }` |
| `ProgramCoreToolGrantContext` | 同上，但没有 `grant_id` |

对象恰好只有列出的成员；缺少或多出的成员都是格式错误的值。

**方法。** `ownerScope`、`runId` 等名字都是字符串。标为必需的方法必须存在，否则 `adapt_js_stores` 抛出 `std::invalid_argument` 并指明它。
可选方法缺失时保持 C++ 接口的默认行为，写在最后一列。

| 成员 | 方法 | 结果 | 缺失时 |
|---|---|---|---|
| `transitions` | `load(ownerScope, runId)` | 运行记录或 `null` | 必需 |
| | `latest(ownerScope, runId)` | 日志记录或 `null` | 必需 |
| | `loadRunPublicationHead(ownerScope, runId)` | 发布头或 `null` | 由 `load` 和 `latest` 读取，并再读一次，确认头没有移动 |
| | `loadEvents(ownerScope, runId, afterSequence)` | 事件列表 | 必需 |
| | `loadEffects(ownerScope, runId, afterSequence)` | 效果发件箱条目列表 | 必需 |
| | `loadJavascriptCommands(ownerScope, runId, afterSequence)` | 命令日志条目列表 | 失败 |
| | `loadCommandPublicationHead(ownerScope, runId)` | 命令发布头或 `null` | 由 `load`、`latest` 和 `loadJavascriptCommands` 读取（所以与它们一起失败） |
| | `loadContextPublications(ownerScope, runId, afterSequence)` | 上下文发布列表 | 失败 |
| | `loadHookOutboxEntries(ownerScope, runId)` | 钩子发件箱条目列表 | 失败 |
| | `loadEffectiveRuntimeState(ownerScope, runId)` | 有效运行时状态 | 由上下文、钩子、运行、谱系和代的读取得到 |
| | `loadChildSyntheses(ownerScope, runId)` | 子合成记录列表 | 失败 |
| | `loadMigrationPlan(ownerScope, runId)` | 迁移计划或 `null` | `null` |
| | `loadLineage(ownerScope, lineageId)` | 谱系头或 `null` | `null` |
| | `loadRunLineage(ownerScope, runId)` | 谱系头或 `null` | `null` |
| | `loadLineageHead(ownerScope, lineageId, headId)` | 谱系头或 `null` | `null` |
| | `loadGeneration(ownerScope, lineageId, generation)` | 代或 `null` | `null` |
| | `loadGenerationInitialPublication(ownerScope, lineageId, generation)` | 发布或 `null` | 失败 |
| | `loadGraphMigrationCapsule(ownerScope, runId, lineageHeadId)` | 胶囊或 `null` | `null` |
| | `loadExecutionLease(ownerScope, runId)` | 租约或 `null` | `null` |
| | `comparePublish(ownerScope, expectedJournalHead, publication)` | 发布结果 | 必需 |
| | `comparePublishExecution(ownerScope, expectedJournalHead, publication, expectedLease, nextLease)` | 发布结果；租约是规范文本或 `null` | `"conflict"` |
| `programs` | `publishAdmitted(bundle, version)` | 无 | 必需 |
| | `getBundle(id)`, `getBundle(ownerScope, id)` | 包或 `null` | 必需 |
| | `getVersion(id)`, `getVersion(ownerScope, id)` | 版本或 `null` | 必需 |
| | `getActivation(ownerScope)` | 激活或 `null` | 失败 |
| | `getActiveBinding(ownerScope)` | 激活绑定或 `null` | 由 `getActivation` 和 `getVersion` 读取 |
| | `compareActivate(ownerScope, expectedGeneration, versionId, policySnapshotHash)` | 激活结果 | 失败 |
| | `listVersions(ownerScope)` | 版本列表 | 失败 |
| | `collectGarbage(ownerScope, pinnedVersionIds)` | 保留报告 | 失败 |
| `grants` | `admit(record)` | 准入 | 必需 |
| | `load(context)` | grant 记录或 `null` | 必需 |
| | `revoke(context)` | 布尔值：是否撤销了 grant | 必需 |
| `fragments` | `publish(record)` | 发布结果 | 必需 |
| | `load(fragmentId)` | 片段记录或 `null` | 必需 |
| | `findPublishedLineage(ownerScope, parentRunId, expansionOperationId)` | 片段记录或 `null` | `null` |
| | `findPublished(ownerScope, parentRunId, expansionOperationId, proposalHash)` | 片段记录或 `null` | `null` |
| | `compareUpdate(fragmentId, expectedRevision, record)` | 发布结果 | 必需 |
| `modules` | `publish(module)` | 无 | 必需 |
| | `get(contentIdentity)`, `get(ownerScope, contentIdentity)` | 模块或 `null` | 必需 |
| | `setLifecycle(contentIdentity, lifecycle)` | 无 | 必需 |



- **一个方法的两种形式。** `getBundle`、`getVersion` 和 `get` 在 C++ 中各有不带所有者的形式和带所有者的形式，目录两种都用。一个 JavaScript 方法同时服务两种，由参数个数区分：一个是标识，
  两个是先所有者范围、后标识。只为其中一种参数写的方法，对另一种就是错的。
- **发布在越过边界之前先检查。** `comparePublish` 和 `comparePublishExecution` 的开头与转换存储的每个后端一样：不需要已存储状态的检查（发布的所有者、预期头的形状、效果发件箱、发布的规范文本）。
  没通过这些检查的发布是 `"conflict"`，永远到不了该对象。已存储的状态对它怎么说，由存储决定。
- **不支持。** `compare_publish_graph_safe_point` 带有永不序列化的证据，所以 JavaScript 存储不参与实时的图迁移：它像基类一样回答 `"conflict"`。

**参考存储。** `neograph.memoryStores()`（即 `stores.mjs` 的 `memoryStores(module)`）返回 `{ transitions, programs, grants, fragments, modules }`，它们通过调用模块的内存 C++ 存储来实现这份契约
（`wasm/memory_stores.cpp`）：参数用适配器编码时所用的同一张表解码，运行存储自己的操作，结果用适配器解码时所用的表编码。它的方法返回值而不是 Promise，内存存储的拒绝会通过 `kind` 以同一个 C++ 异常类到达适配器。
库里没有 grant 存储的内存形式，所以参考存储自带一个小的实现，沿用 SQLite grant 存储的判断。`stores.release()` 丢弃这个集合；请在其上的运行已经结束（它们的 `result` 已敲定）之后再做，因为对已丢弃集合的调用会失败。

想要持久存储的宿主，应当包装参考存储，而不是重新写一遍规则。每个方法都委托给它；改变了什么的调用，其参数被写入宿主自己的存储；启动时，包装层把它们按顺序重放进一个新的参考存储。
参考存储根据每次调用的参数和它之前的状态做决定，所以重放会到达日志所描述的状态。下面的包装层在参考存储做出决定之后、返回结果之前追加日志；不能因两者之间的崩溃而丢失调用的宿主，应先写日志条目，再应用调用。
`test/program_stores_test.mjs` 对一次完整的 Program 运行做的就是这件事。

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


## 示例

- `examples/node/chat.mjs`：从 Node 发起一次调用，
  `API_KEY=... node wasm/examples/node/chat.mjs <base url> <path> <model> "<prompt>" [--stream]`。
- `examples/node/program.mjs`：一个带有两次模型调用、中间有一次暂停的 Program，从 Node 运行，
  `API_KEY=... node wasm/examples/node/program.mjs <base url> <path> <model> "<prompt>" [--answer <text>]`。它逐 token 打印第一次回复，等待回答（用该标志，或在 stdin 输入一行；空回答会取消运行），
  然后用这个回答继续对话。
- `examples/workers/worker.mjs` 和 `wrangler.toml`：一个请求处理程序，用 JSON 或服务器发送事件来回答 `POST {"message": "..."}`，
  返回模型的回复。模板中没有任何机密：密钥是 Worker 的 secret（`wrangler secret put API_KEY`），`neograph_module.wasm` 作为
  `WebAssembly.Module` 导入。

## 测试

CI 构建该模块，并在 Node 22 和 24 下运行以下测试，不需要网络，也不需要密钥：

- `test/llm_host_test.mjs`：本地 HTTP 服务器像 Chat Completions 端点一样应答，包括缓冲响应，以及一个字符被切在两个数据块之间的
  服务器发送事件响应。测试通过模块运行单节点图，检查最终状态、流式事件的顺序、发出的请求（运行时给出的密钥和模型）、
  提供方的拒绝、通过 `AbortSignal` 取消、超时、并发运行，以及模型调用进行期间宿主的定时器仍在运行。
- `test/program_test.mjs`：针对同一个服务器的 Program API。Program 被编译并准入（被拒绝的源码会说明原因），运行流式输出事件、在中断处暂停并被恢复，分叉在不重新询问第一次调用的情况下给同一次暂停另一个回答，在事件循环的同一轮中创建的同一次暂停的分叉各拿走前面的分叉所剩的一半、其余的因预算被拒绝，再次创建的分叉保有自己的份额，
  暂停的运行被取消并从存储读回，同时进行的运行各自保持自己的回答，`AbortSignal` 关闭模型调用的连接，`onEvent` 抛出的错误会取消它的运行。发出之前被取消的 fork 会被撤回，已中止的 signal 不会启动任何东西，永不结束的控制源码会被时间限制停下，递归过深的源码（`define()` 和源码中的十七种形态，以及 `main()`）会带着诊断失败，并让模块之后仍能准入和运行 Program。必须保持为失败的失败也被检查：
  目录里没有上限的模型不会被调用，无法覆盖一次调用的预算会在调用发出之前耗尽运行，超出版本准入范围的预算被拒绝，密钥不在任何结果或事件中。Node 示例作为子进程运行。
- `test/program_stores_test.mjs`：模块的 `stores` 选项。参考存储原样、以及包装成在后续任务中应答，都能服务完整的生命周期（调用之间宿主的定时器继续运行）；被拒绝的调用、抛出的错误和不是值的值，
  都带着存储的消息和方法名 fail closed，且不调用模型；缺少必需方法的对象被拒绝；一个把变化写入日志的包装层，让第二个对象在由日志重放得到的存储之上读回已完成的运行，看到暂停的运行仍在暂停，
  并启动第一个对象的 Program；存储仍在应答时被取消的 fork 以取消结束，一起创建的 fork 依次拿走各自的份额；拼错的成员和缺少的谱系读取方法会被拒绝；释放对象会取消正在执行的运行，让等待的运行保持原样。
- `test/workers_test.mjs`：以 `Request` 调用 Workers 示例导出的 `fetch` 处理程序，对着同一个服务器，分别缓冲和流式。它展示的是
  处理程序的逻辑及其对平台 API 的使用，并不是在 workerd 上运行。
- `checks.cpp`（见下文）对宿主驱动的提供方运行一次同步运行，检查它被拒绝且没有发送任何东西。
- `checks.cpp` 还在 `HostLoop` 上构建 `ProgramRuntime`：没有执行器的运行时和同步的 `start` 都会被拒绝，使用内存存储的
  单节点 Program 在宿主的回合中运行到结果。
  随后它在宿主回合中通过 awaitable 存储读回运行和版本，并展示阻塞视图拒绝在循环上等待。
  它还在 Program 存储和转换存储只在循环的后续回合才完成调用的情况下运行 Program，并通过这样的存储读取目录。
- `tests/test_program_runtime_executor.cpp` 在单线程 `io_context` 和手动执行器之上的 `HostLoop` 上原生运行 Program，
  所以原生构建也覆盖同样的路径。
- `tests/test_program_host_pumped_stores.cpp` 在只在循环的后续回合才完成调用的存储之上的 `HostLoop` 上，原生运行 Program 的整个生命周期：启动、
  模型节点、中断及其恢复、分叉、子运行、Core 工具 grant、完成和取消，没有任何东西被阻塞或拒绝。它还在一轮中对同一次暂停创建多个分叉，并显示它们的预算之和不超过暂停的运行原有的数量。
- `tests/test_program_store_awaitable.cpp` 在原生环境中运行存储契约：内联存储不挂起即完成，交给线程池的存储在自己的 executor 上
  恢复等待的协程，阻塞视图会等待稍后完成的存储，或者拒绝。
- `tests/test_host_loop.cpp` 针对手动 executor 在原生环境中运行循环和宿主接缝提供方，所以原生构建也覆盖相同的路径。
- `test/store_conformance_test.mjs` 在 Node 下针对 JavaScript 存储运行 `tests/` 里的存储一致性测试套件（`ProgramTransitionStoreConformance`、`ProgramStoreConformance` 和图迁移胶囊那一行）。
  模块用 `-DNEOGRAPH_BUILD_WASM_STORE_TESTS=ON`（会获取 googletest）从目标 `neograph_wasm_store_conformance` 构建：带有 JavaScript 后端的测试套件，给每个用例的存储都经过参考存储之上的适配器，
  并以两种形式运行（`JavaScript` 调用同步调用方使用的操作，`JavaScriptAwaited` 调用 awaitable 操作）。在线程上让两个写入者竞争的用例，在那一个线程上让它们轮流执行。脚本打印每个后端的计数和每个被跳过用例的原因，有失败则以非零码退出。
- `test/js_stores_test.mjs` 用其方法返回 Promise（在后续任务中敲定）的存储驱动目标 `neograph_wasm_store_checks`：Promise 在循环的某个回合里恢复等待的协程，悬而未决期间没有任何东西在等待；不是 Promise 的值不占用回合；
  拒绝、抛出和无法解析的值变成带方法名的 C++ 异常；混用两者的对象也能工作；协程被销毁或取消之后才到达的结果被丢弃；缺少必需方法会使 `adapt_js_stores` 失败；整个 Program 在 Promise 存储之上运行，在拒绝的存储之上 fail closed。

<a id="measured-sizes"></a>
## 实测大小

Release，`-O3 -flto`，Emscripten 6.0.13。图 smoke 包含 Core 引擎和 SDK 运行时；只含图的模块在此基础上增加了 LLM 组件、SDK 的
JavaScript 宿主和 C API。其余的大部分是 Program 组件：带 Program 的模块是只含图的模块的两倍大，其中 QuickJS 约 1 MB。

| 构建 | `.wasm` | `.wasm` gzip -9 | `.js` |
|---|---:|---:|---:|
| 图 smoke，单线程 | 5,615,386 B (5.4 MiB) | 1,642,238 B (1.6 MiB) | 83,223 B |
| 图 smoke，pthread | 5,448,684 B (5.2 MiB) | 1,660,246 B (1.6 MiB) | 103,691 B |
| 含 Program、LLM 和 QuickJS 的 checks，单线程 | 12,495,665 B (11.9 MiB) | 3,741,204 B (3.6 MiB) | 91,040 B |
| JavaScript 模块，只含图（`NEOGRAPH_WASM_JS_PROGRAMS=OFF`），单线程 | 6,466,458 B (6.2 MiB) | 1,893,168 B (1.8 MiB) | 87,762 B |
| 带 Program、不带 QuickJS 的 JavaScript 模块，单线程 | 12,148,715 B (11.6 MiB) | 3,551,238 B (3.4 MiB) | 95,154 B |
| 带 Program 和 QuickJS 的 JavaScript 模块，单线程 | 13,209,571 B (12.6 MiB) | 3,931,479 B (3.7 MiB) | 96,889 B |

带 Program 和 QuickJS 的模块用 `-DCMAKE_CXX_FLAGS_RELEASE="-Oz -DNDEBUG"`（C 同样）构建时为 10,378,540 B，gzip 后 3,122,630 B；没有测速度。

CI 把单线程 smoke 限制在 `.wasm` 6,000,000 字节、gzip 后 1,750,000 字节（实测大小加约 9%）以内，把模块（CI 带 Program 和 QuickJS 构建它）限制在 `.wasm` 14,400,000 字节、gzip 后 4,300,000 字节 以内，
并在作业摘要中报告大小。Worker 脚本有随套餐而定的大小限制；请把模块压缩后的大小与你所用套餐的限制比较。
本页旧版本中的 712 KB 是在带类型的 SDK 之前测得的，现已不适用。

## 这些宿主上不可用的功能

凡是由 NeoGraph 拥有的调用，都会以带类型的错误失败；其余的则不会被构建。

- **不经宿主的模型调用。** ASYNC、MCP 客户端和 A2A 需要套接字和 TLS。模型调用通过上述模块走宿主的 `fetch`；面向其他
  WebAssembly 宿主的构建则把自己的 executor 和传输层交给 `SchemaProvider`。
- **宿主驱动的提供方上的阻塞入口。** `run()`、`Provider::dispatch` 等会予以拒绝（见上文）。
- **宿主数据库上的 Program 存储。** 存储接口是 awaitable 的。SQLite 和 PostgreSQL 存储需要系统库，
  所以宿主数据库需要自己的存储。操作立即完成的存储（宿主的同步存储）和只能通过宿主事件循环完成的存储，都可以通过 `_async` 形式
  在宿主循环上为运行时服务（[#557](https://github.com/fox1245/NeoGraph/issues/557)）。同步的 Program 调用在宿主循环上会被拒绝，请使用 `_async` 形式。JavaScript 模块把这样的存储作为 JavaScript 对象接收（上文的 `stores`）。
- **原生 Program 控件。** C v1 插件 ABI 是 64 位布局，因此在 wasm32 上 `NativeControlBinding::create` 会以
  `NativeControlUnavailable` 拒绝。
- **同步检查点存储。** 非异步的存储会在工作线程池中执行每个操作。单线程构建没有线程池，因此操作会以
  `SynchronousCheckpointStoreUnavailable` 被拒绝，而不是等待线程；请实现 `AsyncCheckpointStore`。内置的内存存储在
  调用线程上执行其全部操作（包括 managed 预算），所以带 `model_token_budget` 的运行可以在它上面工作。
- **带期限的 managed 预算。** 期限绑定到重启后仍可恢复的时钟起点，而 WebAssembly 宿主没有这样的起点。
  运行会在询问存储之前，以 `ManagedBudgetDeadlineClockUnavailable` 被拒绝。
- **原生归档。** 没有文件保管（custody）后端，SDK 会以 `Unsupported` 错误拒绝。
- **子进程委托**（Harness CLI、stdio MCP 服务器）、**监听套接字**、**gRPC**、**文件路径上传**和 **Python**。

## 浏览器

由 `serve_smoke.py` 提供服务时，`smoke.html` 会在 `http://127.0.0.1:8765/smoke.html` 运行生成的 `smoke.js`：

```bash
python3 wasm/serve_smoke.py build-wasm/wasm --port 8765
```

`serve_smoke.py` 发送 `Cross-Origin-Opener-Policy: same-origin` 和 `Cross-Origin-Embedder-Policy: require-corp`。
`smoke.html` 会检查跨源隔离和 `SharedArrayBuffer`，加载 `/smoke.js`，并在程序输出 `doubled = 42` 和 `trace = d`、
退出码为 `0` 且无标准错误输出时，把 `document.body.dataset.result` 设为 `"pass"`、`document.body.dataset.exitCode` 设为 `"0"`。
已用默认单线程构建在无头 Chromium 140.0.7339.186 中验证（`doubled = 42`、`trace = d`、退出码 `0`）；pthread 构建仅在 Node
中运行（不带 `-sPROXY_TO_PTHREAD` 时 `main` 会阻塞主线程），CI 仅运行 Node，JavaScript 模块（`neograph.mjs`）尚未在浏览器中运行。

## 里程碑

工具链评审之后的计划见 [#535](https://github.com/fox1245/NeoGraph/issues/535)：

- **M0 和 M1。** wasm32 修复、单线程构建，以及 CI 中的 Node smoke 和大小门禁。
- **M2（本页）。** 通过宿主 `fetch` 进行模型调用：宿主循环、宿主接缝提供方、JavaScript 模块，以及 Node 和 Workers 示例。
- **M3。** 宿主存储上的 Program（[#557](https://github.com/fox1245/NeoGraph/issues/557)、
  [#556](https://github.com/fox1245/NeoGraph/issues/556)）。运行时这一半已完成：`ProgramRuntime` 在
  `HostLoop::executor()` 上运行（见上文“宿主循环上的 Program”）。存储接口也已成为 awaitable。JavaScript 接口也是如此（见上文“模块中的 Program”）。
- **M4。** 实时会话、WASI、原生归档的保管后端，以及远程 Harness。
