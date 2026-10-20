<!-- neograph-i18n: source=wasm/README.md locale=zh-CN source_sha256=4e62534e14c092fd8f32cd6e52366b4f1ae493d69de08a965bff04e45f5165ff -->
# WebAssembly 上的 NeoGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

本目录包含 CI 构建并运行的 WebAssembly 程序。`smoke.cpp` 编译一个只有一个 `DoubleNode` 的图，写入 `doubled = seed * 2`，
并用 `InMemoryCheckpointStore` 运行；`seed = 21` 时输出 `doubled = 42` 和 `trace = d`。`checks.cpp` 检查
没有线程的 32 位宿主上有所不同的地方（见下文）。两者都不进行网络或模型调用。目前能在 WebAssembly 宿主上运行的是图引擎；
模型调用、Program 和存储还不能。计划见 [#535](https://github.com/fox1245/NeoGraph/issues/535)，
这不是浏览器 SDK。

## 现状

| 项目 | 状态 |
|---|---|
| 工具链 | Emscripten 6.0.13。CI 中固定到 emsdk 的发布提交。 |
| 构建模式 | 单线程（WebAssembly 上的默认值）和 pthread，二者都在 CI 中构建并运行。 |
| 运行时 | CI 中的 Node.js 22 和 24。浏览器、Cloudflare Workers、Deno 和 WASI 尚未验证。 |
| SDK | SchemaProvider 0.4.0，接口修订版 7，使用 `cmake/NeoGraphSchemaProvider.cmake` 固定的提交。可在没有 libcurl、没有线程的情况下构建。 |
| 组件 | Core，以及可选的 Program。 |

## 构建

构建需要 Emscripten 6.0.13（其 libc++ 提供 C++20 协程、`<stop_token>` 和 `std::bit_cast`）、CMake 3.20 或更新版本以及 Ninja。
LLM、ASYNC、MCP 客户端、A2A、gRPC 以及 SQLite 和 PostgreSQL 存储需要 WebAssembly 宿主不提供的套接字、TLS 或系统库，
因此 CMake 会拒绝它们，下面也将它们关闭。

单线程，WebAssembly 上的默认值：

```bash
source /path/to/emsdk/emsdk_env.sh

emcmake cmake -S . -B build-wasm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_WASM=ON \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_ASYNC=OFF -DNEOGRAPH_BUILD_LLM=OFF -DNEOGRAPH_BUILD_MCP=OFF \
  -DNEOGRAPH_BUILD_MCP_CLIENT=OFF -DNEOGRAPH_BUILD_MCP_SERVER=OFF -DNEOGRAPH_BUILD_MCP_HTTP_SERVER=OFF \
  -DNEOGRAPH_BUILD_A2A=OFF -DNEOGRAPH_BUILD_ACP=OFF -DNEOGRAPH_BUILD_GRPC=OFF \
  -DNEOGRAPH_BUILD_POSTGRES=OFF -DNEOGRAPH_BUILD_SQLITE=OFF -DNEOGRAPH_BUILD_UTIL=OFF \
  -DNEOGRAPH_BUILD_EXAMPLES=OFF -DNEOGRAPH_BUILD_TESTS=OFF -DNEOGRAPH_BUILD_BENCHMARKS=OFF \
  -DNEOGRAPH_USE_LIBCURL=OFF
cmake --build build-wasm --target neograph_wasm_smoke neograph_wasm_checks
node build-wasm/wasm/smoke.js
node build-wasm/wasm/checks.js
```

pthread：在 configure 命令行中加上 `-DNEOGRAPH_THREADS=ON`，并使用另一个构建目录。
只有原生插件检查才需要 `NEOGRAPH_BUILD_PROGRAM=ON`；只构建图 smoke 时请关闭它。

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

## 实测大小

Release，`-O3 -flto`，Emscripten 6.0.13。图 smoke 包含 Core 引擎和 SDK 运行时。

| 构建 | `.wasm` | `.wasm` gzip -9 | `.js` |
|---|---:|---:|---:|
| 图 smoke，单线程 | 5,484,652 B (5.2 MiB) | 1,601,090 B (1.5 MiB) | 83,223 B |
| 图 smoke，pthread | 5,432,156 B (5.2 MiB) | 1,655,480 B (1.6 MiB) | 103,691 B |
| 含 Program 的 checks，单线程 | 5,017,721 B (4.8 MiB) | 1,554,991 B (1.5 MiB) | 83,031 B |

CI 把单线程 smoke 限制在 `.wasm` 6,000,000 字节、gzip 后 1,750,000 字节（实测大小加约 9%）以内，并在作业摘要中报告大小。
本页旧版本中的 712 KB 是在带类型的 SDK 之前测得的，现已不适用。

## 这些宿主上不可用的功能

凡是由 NeoGraph 拥有的调用，都会以带类型的错误失败；其余的则不会被构建。

- **模型调用。** LLM、ASYNC、MCP 客户端和 A2A 需要套接字和 TLS。通过宿主 `fetch` 的模型调用将在 #535 的 M2 中提供。
- **Program。** Program 组件可以编译，但 `ProgramRuntime` 会启动线程，所以在单线程构建中，它的构造、节点 attempt 超时
  以及 `n > 1` 的 `set_worker_count(n)` 都会以 `thread: Not supported` 失败（#557）。宿主上的 Program 存储在 #556 中跟踪。
- **原生 Program 控件。** C v1 插件 ABI 是 64 位布局，因此在 wasm32 上 `NativeControlBinding::create` 会以
  `NativeControlUnavailable` 拒绝。
- **同步检查点存储。** 非异步的存储会在工作线程池中执行每个操作。单线程构建没有线程池，因此操作会以
  `SynchronousCheckpointStoreUnavailable` 被拒绝，而不是等待线程；请实现 `AsyncCheckpointStore`。内置的内存存储在
  检查点上是异步的，但在 managed 预算操作上不是，所以在存储变为 awaitable 之前（#556），带 `model_token_budget` 的运行
  同样会被拒绝。
- **带期限的 managed 预算。** 期限绑定到重启后仍可恢复的时钟起点，而 WebAssembly 宿主没有这样的起点。
  运行会在询问存储之前，以 `ManagedBudgetDeadlineClockUnavailable` 被拒绝。
- **原生归档。** 没有文件保管（custody）后端，SDK 会以 `Unsupported` 错误拒绝。
- **子进程委托**（Harness CLI、stdio MCP 服务器）、**监听套接字**、**gRPC**、**文件路径上传**和 **Python**。

## 浏览器

浏览器测试工具不在仓库中：`serve_smoke.py` 期望一个并不存在的 `smoke.html`，也没有浏览器运行记录（#435）。
Node.js 上的运行不能作为浏览器上的证据。

## 里程碑

工具链评审之后的计划见 [#535](https://github.com/fox1245/NeoGraph/issues/535)：

- **M0 和 M1（本页）。** wasm32 修复、单线程构建，以及 CI 中的 Node smoke 和大小门禁。
- **M2。** 通过宿主 `fetch` 进行模型调用，在浏览器和 Workers 中。
- **M3。** 宿主存储上的 Program（[#557](https://github.com/fox1245/NeoGraph/issues/557)、
  [#556](https://github.com/fox1245/NeoGraph/issues/556)）。
- **M4。** 实时会话、WASI、原生归档的保管后端，以及远程 Harness。
