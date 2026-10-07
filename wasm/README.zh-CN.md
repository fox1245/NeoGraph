<!-- neograph-i18n: source=wasm/README.md locale=zh-CN source_sha256=94363c98fc3c2931d87e8b916dbff497b8b2ce92fcecaf905bdbb1cf98b1bdfe -->
# NeoGraph WASM smoke 程序

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`smoke.cpp` 编译只有一个 `DoubleNode` 的图，写入 `doubled = seed * 2`，并使用 `InMemoryCheckpointStore` 运行。`seed = 21` 时，预期输出包含 `doubled = 42` 和 `trace = d`。没有网络或模型调用。这是 Node.js smoke 目标，不是浏览器 SDK。

## 当前构建边界

类型化 provider 切换后，即使 `NEOGRAPH_BUILD_LLM=OFF`，Core 也依赖 `SchemaProvider::runtime`。
CMake 3.20+ 按显式 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`、已安装 runtime、固定 public GitHub source archive 的顺序选择 SDK。
`NEOGRAPH_FETCH_SCHEMAPROVIDER` 的 download fallback 默认 ON，offline package/source 构建应设为 OFF。
SDK runtime 还需要 libcurl、OpenSSL 和 threads。当前 transport 与 archive 有平台特定依赖；此 worktree 没有经过验证的 Emscripten SDK runtime 构建。native SDK 不能链接到 WebAssembly。下方历史 smoke 不证明当前源码能在 Emscripten 下构建。

NeoGraph `0.13.1` 需要为相同 target 构建的 alpha SDK `0.1.1`，interface revision/shared generation 4。验证真实 Emscripten SDK runtime 仍是前提；本页既不删除 WASM 支持，也不保证当前构建可用。Archive v3 / `spna3` 和 portable JSON v2 不变。

## 历史结果

以下数据来自类型化 SDK 切换前的一次本地 smoke 执行。生成的 `.wasm`/`.js` 没有提交，CI 也不发布 WASM 大小 artifact。大小描述当时的构建，不是当前部署成本。

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
source /opt/emsdk/emsdk_env.sh

emcmake cmake -S . -B build-wasm \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR="$SCHEMAPROVIDER_SOURCE_DIR" \
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

`NEOGRAPH_USE_LIBCURL=OFF` 禁用的是 NeoGraph 可选 HTTP/2 backend，不是 SDK runtime 的 libcurl 依赖。此命令不是已验证的当前构建。

对于生成 loader 向 `fetch` 传入 filesystem path 的 Node.js 版本，历史 workaround 如下：

```bash
node -e 'const fs=require("fs"); WebAssembly.instantiateStreaming=undefined; global.fetch=async p=>({ok:true,arrayBuffer:async()=>fs.promises.readFile(p)}); require("./build-wasm/wasm/smoke.js");'
```


## 浏览器状态与提议工作

此仓库没有 browser loader、npm package 或 Embind API。browser pthread 构建还需要 cross-origin isolation header（`Cross-Origin-Opener-Policy: same-origin` 和 `Cross-Origin-Embedder-Policy: require-corp`）以及生成的 worker asset；仅满足这些条件并不能让 SDK runtime 兼容浏览器。

浏览器移植首先需要兼容 SDK 的 transport/archive 设计，然后才是 JS node callback 与 packaging。提议的 `fetch()` adapter、hosted model access、local browser inference 和 NeoProtocol Executor 集成都没有由此 smoke 程序提供或验证。
