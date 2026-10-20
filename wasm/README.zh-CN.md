<!-- neograph-i18n: source=wasm/README.md locale=zh-CN source_sha256=bc86c1517c7fd9e5993fbaff32747ad839a0cfd34435186ee87fb98af403cc31 -->
# WebAssembly 上的 NeoGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

本目录包含 CI 构建并运行的 WebAssembly 程序。

- `module.cpp` 和 `neograph.mjs` 是 JavaScript 模块：从 JSON 编译图并运行，模型调用通过宿主的 `fetch` 发送。`examples/` 中有使用
  该模块的 Node 程序和 Cloudflare Workers 风格的处理程序，`test/` 中是 CI 运行的离线测试（由本地 HTTP 服务器代替模型）。
- `smoke.cpp` 编译一个只有一个 `DoubleNode` 的图，写入 `doubled = seed * 2`，并用 `InMemoryCheckpointStore` 运行；
  `seed = 21` 时输出 `doubled = 42` 和 `trace = d`。
- `checks.cpp` 检查没有线程的 32 位宿主上有所不同的地方（见下文）。

计划见 [#535](https://github.com/fox1245/NeoGraph/issues/535)，这不是浏览器 SDK。

## 现状

| 项目 | 状态 |
|---|---|
| 工具链 | Emscripten 6.0.13。CI 中固定到 emsdk 的发布提交。 |
| 构建模式 | 单线程（WebAssembly 上的默认值）和 pthread，二者都在 CI 中构建并运行。JavaScript 模块是单线程的。 |
| 运行时 | CI 中的 Node.js 22 和 24。Workers 示例在 Node 中以 `Request` 调用来运行，尚未在 workerd 上运行。浏览器、Deno 和 WASI 尚未验证。 |
| SDK | SchemaProvider 0.4.0，接口修订版 7，使用 `cmake/NeoGraphSchemaProvider.cmake` 固定的提交。可在没有 libcurl、没有线程的情况下构建，其 JavaScript 宿主（`sp_js_host`）就是模块的 executor 和 `fetch` 传输层。 |
| 组件 | Core、LLM（通过宿主的模型调用），以及可选的 Program。 |

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

`NEOGRAPH_BUILD_WASM_JS=ON` 会在 `build-wasm/wasm/` 中生成 JavaScript 模块：`neograph_module.wasm`、
`neograph_module.mjs`（Emscripten 加载器）和 `neograph.mjs`（从本目录复制的 API）。把这三个文件复制到项目中即可使用。
只有原生插件检查才需要 `NEOGRAPH_BUILD_PROGRAM=ON`；只构建图 smoke 和模块时请关闭它。pthread：在 configure 命令行中加上
`-DNEOGRAPH_THREADS=ON`，保持 `NEOGRAPH_BUILD_WASM_JS` 和 `NEOGRAPH_BUILD_LLM` 关闭，并使用另一个构建目录。

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

图可以使用不需要工具的内置节点类型：`llm_call`、`intent_classifier` 和 `subgraph`。用 JavaScript 编写的工具和节点、
检查点存储以及 Program 不属于本里程碑。

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
- **Asio 定时器。** 重试退避或资源等待轮询都是 Asio 定时器，而宿主的循环看不到 Asio 的定时器队列。引擎的等待会把自己的
  截止时间告知循环（`async::note_deadline`），循环为最早的截止时间设置一个宿主定时器。在 `HostLoop` 下等待 Asio 定时器的
  代码也要这样做。

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
而接口只有五个函数：`ng_compile`、`ng_run`、`ng_cancel`、`ng_release` 和 `ng_error`。结果通过两个回调 `Module.ngOnEvent` 和
`Module.ngOnDone` 返回，它们在宿主事件循环之后的回合中被调用。密钥是交给 `compile` 的提供方描述的一部分，由该图的 SDK
客户端持有；它不在二进制文件中，不在任何结果中，也不在错误消息中。

## 示例

- `examples/node/chat.mjs`：从 Node 发起一次调用，
  `API_KEY=... node wasm/examples/node/chat.mjs <base url> <path> <model> "<prompt>" [--stream]`。
- `examples/workers/worker.mjs` 和 `wrangler.toml`：一个请求处理程序，用 JSON 或服务器发送事件来回答 `POST {"message": "..."}`，
  返回模型的回复。模板中没有任何机密：密钥是 Worker 的 secret（`wrangler secret put API_KEY`），`neograph_module.wasm` 作为
  `WebAssembly.Module` 导入。

## 测试

CI 构建该模块，并在 Node 22 和 24 下运行以下测试，不需要网络，也不需要密钥：

- `test/llm_host_test.mjs`：本地 HTTP 服务器像 Chat Completions 端点一样应答，包括缓冲响应，以及一个字符被切在两个数据块之间的
  服务器发送事件响应。测试通过模块运行单节点图，检查最终状态、流式事件的顺序、发出的请求（运行时给出的密钥和模型）、
  提供方的拒绝、通过 `AbortSignal` 取消、超时、并发运行，以及模型调用进行期间宿主的定时器仍在运行。
- `test/workers_test.mjs`：以 `Request` 调用 Workers 示例导出的 `fetch` 处理程序，对着同一个服务器，分别缓冲和流式。它展示的是
  处理程序的逻辑及其对平台 API 的使用，并不是在 workerd 上运行。
- `checks.cpp`（见下文）对宿主驱动的提供方运行一次同步运行，检查它被拒绝且没有发送任何东西。
- `tests/test_host_loop.cpp` 针对手动 executor 在原生环境中运行循环和宿主接缝提供方，所以原生构建也覆盖相同的路径。

## 实测大小

Release，`-O3 -flto`，Emscripten 6.0.13。图 smoke 包含 Core 引擎和 SDK 运行时；模块在此基础上增加了 LLM 组件、SDK 的
JavaScript 宿主和 C API。

| 构建 | `.wasm` | `.wasm` gzip -9 | `.js` |
|---|---:|---:|---:|
| 图 smoke，单线程 | 5,509,570 B (5.3 MiB) | 1,608,986 B (1.5 MiB) | 83,223 B |
| 图 smoke，pthread | 5,448,684 B (5.2 MiB) | 1,660,246 B (1.6 MiB) | 103,691 B |
| 含 Program 和 LLM 的 checks，单线程 | 6,039,737 B (5.8 MiB) | 1,842,525 B (1.8 MiB) | 84,263 B |
| JavaScript 模块，单线程 | 6,270,525 B (6.0 MiB) | 1,835,562 B (1.8 MiB) | 87,762 B |

CI 把单线程 smoke 限制在 `.wasm` 6,000,000 字节、gzip 后 1,750,000 字节（实测大小加约 9%）以内，把模块限制在 `.wasm` 6,850,000 字节、gzip 后 2,010,000 字节 以内，
并在作业摘要中报告大小。Worker 脚本有随套餐而定的大小限制；请把模块压缩后的大小与你所用套餐的限制比较。
本页旧版本中的 712 KB 是在带类型的 SDK 之前测得的，现已不适用。

## 这些宿主上不可用的功能

凡是由 NeoGraph 拥有的调用，都会以带类型的错误失败；其余的则不会被构建。

- **不经宿主的模型调用。** ASYNC、MCP 客户端和 A2A 需要套接字和 TLS。模型调用通过上述模块走宿主的 `fetch`；面向其他
  WebAssembly 宿主的构建则把自己的 executor 和传输层交给 `SchemaProvider`。
- **宿主驱动的提供方上的阻塞入口。** `run()`、`Provider::dispatch` 等会予以拒绝（见上文）。
- **Program。** Program 组件可以编译，但 `ProgramRuntime` 会启动线程，所以在单线程构建中，它的构造、节点 attempt 超时
  以及 `n > 1` 的 `set_worker_count(n)` 都会以 `thread: Not supported` 失败（#557）。宿主上的 Program 存储在 #556 中跟踪。
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

浏览器测试工具不在仓库中：`serve_smoke.py` 期望一个并不存在的 `smoke.html`，也没有浏览器运行记录（#435）。
Node.js 上的运行不能作为浏览器上的证据。该模块为 `web`、`worker` 和 `node` 环境构建，但只运行过 Node。

## 里程碑

工具链评审之后的计划见 [#535](https://github.com/fox1245/NeoGraph/issues/535)：

- **M0 和 M1。** wasm32 修复、单线程构建，以及 CI 中的 Node smoke 和大小门禁。
- **M2（本页）。** 通过宿主 `fetch` 进行模型调用：宿主循环、宿主接缝提供方、JavaScript 模块，以及 Node 和 Workers 示例。
- **M3。** 宿主存储上的 Program（[#557](https://github.com/fox1245/NeoGraph/issues/557)、
  [#556](https://github.com/fox1245/NeoGraph/issues/556)）。`HostLoop::executor()` 就是 Program 运行时将要接收的
  `any_io_executor`。
- **M4。** 实时会话、WASI、原生归档的保管后端，以及远程 Harness。
