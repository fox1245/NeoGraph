<!-- neograph-i18n: source=wasm/README.md locale=zh-CN source_sha256=677a943827a8ac2e6d20cd23d1b1071ad0c3cade39c959acfc68762b7cad4abc -->
# NeoGraph WASM 冒烟程序

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`smoke.cpp` 编译一张只有一个 `DoubleNode` 的图，写入 `doubled = seed * 2`，
并配合 `InMemoryCheckpointStore` 运行。`seed = 21` 时，预期输出包含
`doubled = 42` 和 `trace = d`。它不发起任何网络或模型调用。
同一个生成的目标既可以在 Node.js 中运行，也可以在下面的浏览器测试环境中运行；
它不是浏览器 SDK。

## 当前的构建边界

即使设置了 `NEOGRAPH_BUILD_LLM=OFF`，Core 也依赖 `SchemaProvider::runtime`。CMake 3.20 及以上会按以下顺序选择：
先是显式指定的 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`，其次是已安装的运行时，
最后是固定版本的公开 GitHub 源码归档。下载这一兜底方式通过
`NEOGRAPH_FETCH_SCHEMAPROVIDER` 控制，默认为 ON；离线的包构建或源码构建要把它设为 OFF。
Core 链接的是 SDK 运行时，而不是它的 libcurl 传输层。在所检查的 SDK 固定版本上，
SDK 顶层的 CMake 在配置阶段仍然要求 CURL，尽管运行时目标并不链接它；
OpenSSL 不是运行时的直接依赖。
SDK 的原生库无法链接进 WebAssembly。Emscripten 目标上，SDK 运行时、archive 支持和
C++ 标准库都必须能正常工作；下面那次历史上的冒烟测试，不能说明当前的代码能构建成功。

当前的构建需要 SDK `0.4.0`，接口修订版/共享库代数为 7，
对应 `cmake/NeoGraphSchemaProvider.cmake` 所固定的 SchemaProvider 提交，
并且要针对同一个目标构建（WebAssembly 方面的工作由 #535 跟踪）。必须先有真正可用的
Emscripten 版 SDK 运行时构建；本页不保证当前能构建成功。

### 对受支持目标的要求

Emscripten 工具链和 C++ 标准库必须支持 SDK 的 C++20 约定，
包括 `std::bit_cast` 和 `std::stop_token`。native archive 托管需要目标平台上受支持的、
原子且不覆盖已有文件（no-replace）的文件系统后端；不能为了能构建成功而关闭托管。

请使用全新的构建目录，以及真正针对同一目标的依赖，包括 SDK 配置阶段需要的
CURL 依赖。原生库或伪造的依赖可用性，都不能得到有效的 WebAssembly 构建。
目前在 Node.js 和浏览器中的执行尚未验证。

## 历史结果

这些数值来自较早的一次冒烟测试，当时还没有切换到带类型的 SDK。
仓库中没有提交生成的 `.wasm` 或 `.js` 产物，CI 也不发布 WASM 体积的产物。
这些体积只描述那次构建，不代表当前的部署成本。

| 指标 | 数值 |
|---|---|
| WASM 二进制文件（-O3 + LTO） | 712 KB |
| Emscripten 的 JS 运行时 | 92 KB |
| JavaScript 与 WASM 合计 | ~800 KB |
| 那次运行中引擎源码的改动 | 0 行 |
| 首次运行的输出 | `doubled = 42, trace = d` |

## 目标与前置条件

这个目标直接链接 `neograph_core`。它使用 Core 的主源码列表、C++20 协程、异常和
Emscripten 的 pthread。CMake 会为发行版自带的 Emscripten 3.1.x 启用 Asio 的协程接口，
并且只在 WASM 目标上禁用不受支持的栈保护符号；原生构建的加固选项保持不变。
该目标设置了 `PTHREAD_POOL_SIZE=4`，而图使用的是默认的 `worker_count=1`。
这里没有用于构建单线程 WASM 变体的 CMake 选项。

一旦有了可用的、Emscripten 版的 SDK 运行时，这个目标的配置/构建/运行步骤如下：

```bash
# For an emsdk install only: source /opt/emsdk/emsdk_env.sh
# A distro installation with emcmake/em++ on PATH needs no emsdk script.

emcmake cmake -S . -B build-wasm \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR="$SCHEMAPROVIDER_SOURCE_DIR" \
  -DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF \
  -DNEOGRAPH_BUILD_WASM=ON \
  -DNEOGRAPH_BUILD_ASYNC=OFF \
  -DNEOGRAPH_BUILD_LLM=OFF \
  -DNEOGRAPH_BUILD_MCP=OFF \
  -DNEOGRAPH_BUILD_MCP_CLIENT=OFF \
  -DNEOGRAPH_BUILD_MCP_SERVER=OFF \
  -DNEOGRAPH_BUILD_MCP_HTTP_SERVER=OFF \
  -DNEOGRAPH_BUILD_A2A=OFF \
  -DNEOGRAPH_BUILD_ACP=OFF \
  -DNEOGRAPH_BUILD_GRPC=OFF \
  -DNEOGRAPH_BUILD_POSTGRES=OFF \
  -DNEOGRAPH_BUILD_SQLITE=OFF \
  -DNEOGRAPH_BUILD_UTIL=OFF \
  -DNEOGRAPH_BUILD_EXAMPLES=OFF \
  -DNEOGRAPH_BUILD_TESTS=OFF \
  -DNEOGRAPH_BUILD_BENCHMARKS=OFF \
  -DNEOGRAPH_USE_LIBCURL=OFF
cmake --build build-wasm --target neograph_wasm_smoke -j
node build-wasm/wasm/smoke.js
```

`NEOGRAPH_USE_LIBCURL=OFF` 会关闭 NeoGraph 可选的 HTTP/2 后端，
但不会去掉 SDK 顶层对 CURL 的查找。这套步骤仍然需要真正的 Emscripten 依赖，
也不是经过验证的当前构建。

对于生成的加载器会把文件系统路径传给 `fetch` 的 Node.js 版本，
历史上的加载器变通办法是：

```bash
node -e 'const fs=require("fs"); WebAssembly.instantiateStreaming=undefined; global.fetch=async p=>({ok:true,arrayBuffer:async()=>fs.promises.readFile(p)}); require("./build-wasm/wasm/smoke.js");'
```

## 浏览器冒烟测试

真正的目标构建成功之后，用只依赖标准库的回环服务器，
来提供它的 JS、WASM 以及生成的 pthread worker 资源：

```sh
python3 wasm/serve_smoke.py build-wasm/wasm --port 8765
# Open http://127.0.0.1:8765/smoke.html in a browser.
```

服务器会发送 `Cross-Origin-Opener-Policy: same-origin` 和
`Cross-Origin-Embedder-Policy: require-corp`。没有跨源隔离 / `SharedArrayBuffer`
时，测试环境拒绝执行；它加载的是真正生成的 `smoke.js`，而不是替身运行时。
通过的条件是：输出 `doubled = 42`、`trace = d`，退出码为 0，并分别以
`document.body.dataset.result === "pass"` 和 `dataset.exitCode === "0"` 的形式暴露出来。
请记录浏览器及其版本、控制台错误，以及实际观察到的输出。只在 Node.js 中运行成功，
不能算作浏览器方面的证据。

这个测试环境不提供 npm 包、Embind API、JS 的图回调或 provider 传输层。
托管模型的访问、本地推理，以及与 NeoProtocol Executor 的集成，
都不在这个不涉及模型的冒烟测试的覆盖范围内。
