<!-- neograph-i18n: source=wasm/README.md locale=zh-CN source_sha256=000ed65af24d7a4009772586f8895758fc873841f13515a94b4ab0cfd16866b7 -->
# NeoGraph WASM smoke 程序

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`smoke.cpp` 编译只有一个 `DoubleNode` 的图，写入 `doubled = seed * 2`，并使用 `InMemoryCheckpointStore` 运行。`seed = 21` 时，预期输出包含 `doubled = 42` 和 `trace = d`。没有网络或模型调用。同一生成目标可以在 Node.js 或下方浏览器 harness 中运行，但这不是浏览器 SDK。

## 当前构建边界

类型化 provider 切换后，即使 `NEOGRAPH_BUILD_LLM=OFF`，Core 也依赖 `SchemaProvider::runtime`。
CMake 3.20+ 按显式 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`、已安装 runtime、固定 public GitHub source archive 的顺序选择 SDK。
`NEOGRAPH_FETCH_SCHEMAPROVIDER` 的 download fallback 默认 ON，offline package/source 构建应设为 OFF。
Core 链接 SDK runtime，而不链接 libcurl transport。在所检查的 SDK pin 中，顶层 CMake 即使 runtime 目标不链接 CURL，仍在配置时要求 CURL；OpenSSL 不是 runtime 的直接依赖。native SDK library 不能链接到 WebAssembly。必须验证 Emscripten 目标的 runtime/archive 和 C++ 标准库支持；下方历史 smoke 不是当前构建成功的证据。

当前构建需要相同 target 的 SDK `0.3.0`、interface revision/shared generation 6，以及已合并的 SchemaProvider PR #16（`3b88e4ba020c3a4d39ff0660014e7292b516b7cb`）。验证真实 Emscripten SDK runtime 仍是前提；本页既不删除 WASM 支持，也不保证当前构建可用。Archive v3 / `spna3` 和 portable JSON v2 不变。

### 支持目标的要求

Emscripten 工具链和 C++ 标准库必须支持 SDK 的 C++20 契约，包括 `std::bit_cast` 和 `std::stop_token`。native archive custody 需要目标支持的 atomic no-replace filesystem backend；不得为获得构建而禁用 custody。

请使用新构建目录和真正的相同 target 依赖，包括 SDK 配置所需的 CURL 依赖。native library 或伪造的依赖可用性不能验证 WebAssembly 构建。当前 Node.js 和浏览器执行仍未验证。

## 历史结果

以下数据来自类型化 SDK 切换前的一次历史 smoke 执行。生成的 `.wasm`/`.js` 没有提交，CI 也不发布 WASM 大小 artifact。大小描述当时的构建，不是当前部署成本。

| 指标 | 值 |
|---|---|
| WASM binary (-O3 + LTO) | 712 KB |
| Emscripten JS runtime | 92 KB |
| JavaScript + WASM 总计 | ~800 KB |
| 当时引擎源码改动 | 0 lines |
| 首次运行输出 | `doubled = 42, trace = d` |

## 目标与前提条件

目标直接链接 `neograph_core`，使用 Core 的源码列表、C++20 coroutine、exception 和 Emscripten pthread。CMake 为 distro Emscripten 3.1.x 启用 Asio coroutine，仅在 WASM 目标中禁用不支持的 stack-protector symbol；native hardening 不变。目标设置 `PTHREAD_POOL_SIZE=4`，而图使用默认 `worker_count=1`。这里没有单线程 WASM 变体的 CMake 选项。

取得可工作的 Emscripten SDK runtime 构建后，目标的 configure/build/run 命令如下：

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

`NEOGRAPH_USE_LIBCURL=OFF` 仅禁用 NeoGraph 可选 HTTP/2 backend，不会移除 SDK 顶层 CURL 检测。仍需真正的 Emscripten 依赖；此命令不是已验证的当前构建。

对于生成 loader 向 `fetch` 传入 filesystem path 的 Node.js 版本，历史 workaround 如下：

```bash
node -e 'const fs=require("fs"); WebAssembly.instantiateStreaming=undefined; global.fetch=async p=>({ok:true,arrayBuffer:async()=>fs.promises.readFile(p)}); require("./build-wasm/wasm/smoke.js");'
```


## 浏览器 smoke

真正的目标构建后，使用仅依赖标准库的 loopback 服务器提供 JS、WASM 和生成的 pthread worker asset：

```sh
python3 wasm/serve_smoke.py build-wasm/wasm --port 8765
# Open http://127.0.0.1:8765/smoke.html in a browser.
```

服务器发送 `Cross-Origin-Opener-Policy: same-origin` 和 `Cross-Origin-Embedder-Policy: require-corp`。harness 若无 cross-origin isolation / `SharedArrayBuffer` 就拒绝执行；加载的是实际生成的 `smoke.js`，而非替代 runtime。通过要求输出 `doubled = 42`、`trace = d` 且退出码为 0，通过 `document.body.dataset.result === "pass"` 和 `dataset.exitCode === "0"` 暴露结果。记录浏览器/版本、console 错误及观测输出。Node.js 成功本身不是浏览器证据。

此 harness 不提供 npm package、Embind API、JS graph callback 或 provider transport。hosted model access、local inference 和 NeoProtocol Executor 集成都未由此无模型 smoke 验证。
