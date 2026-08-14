<!-- neograph-i18n: source=wasm/README.md locale=zh-CN source_sha256=7499ea89c1228c6687e0d960ad790d3e072b4157c3c125fdf95cc4eca2656e5f -->
# NeoGraph WASM — 可行性验证

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

图引擎已编译到 WebAssembly。本目录是 **phase-1 spike** — 证明 engine layer（compile、run、executor、scheduler、coordinator、state、channels、NodeCache）能在 Emscripten 下不修改源码地构建并执行。

## 历史运行结果

| 指标 | 值 |
|---|---|
| WASM 二进制文件（-O3 + LTO） | **712 KB** |
| Emscripten JS runtime    | 92 KB |
| JavaScript + WASM 总计 | **~800 KB** |
| 引擎源码差异       | 0 行 |
| 首次运行输出         | `doubled = 42, trace = d` ✓ |

对比：native NG 总计 5.5 MB；LangGraph stack（langgraph + langchain + openai + httpx + pydantic + langsmith）是 31 MB 的纯 Python，而且甚至不尝试发往浏览器。NG 能装进 L3 cache 两次，而且足够小，小到一个典型 SaaS landing page 加载的 JS 已经比这个引擎更多。

## 目前已运行的（Phase 1）

- `GraphEngine::compile(json)` — JSON 定义 → 可执行引擎。
- `engine->run(cfg)` — 使用 InMemoryCheckpointStore 的同步运行。
- 通过 `NodeFactory::register_type` 注册的自定义节点 — 叶节点语义沿用 C++ / Python 路径。
- 所有 v0.1.6 功能都能干净编译：`set_worker_count`、`set_node_cache_enabled`、带 reducers 的 channels、条件边、Send fan-out、Command 路由、中断。
- C++20 协程（asio 的 header-only `awaitable` 组件）可在 Emscripten 5.0 下工作。

## 故意未发布的

| 子系统 | 延迟原因 | 阶段 |
|---|---|---|
| `neograph_async` (HTTP/WebSocket via asio) | Browser 使用 `fetch` / native WebSocket，不使用 raw sockets | 2 |
| `neograph_llm` (SchemaProvider, OpenAIProvider) | 依赖上面的 async transport | 2 |
| `neograph_postgres` | Browser irrelevant | — |
| `neograph_mcp` | 基于子进程，与浏览器无关 | — |
| Embind JS bindings | 让 JS 以 callbacks 定义 node implementations | 2-A |

## 构建并运行

```bash
source /opt/emsdk/emsdk_env.sh

emcmake cmake -S . -B build-wasm \
  -DCMAKE_BUILD_TYPE=Release \
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

该目标直接链接 `neograph_core`，因此源文件列表由主 CMake 构建维护，而不是复制到
本文档中。成功运行时会输出 `doubled = 42` 和节点执行轨迹。

`compile()` 默认使用 `worker_count=1`，因此不会创建引擎自有线程池。该冒烟命令仍启用四个 Emscripten 线程，以便调用方可通过 `set_worker_count(N >= 2)` 选择并行扇出；冒烟测试本身使用单工作线程默认值。单线程构建只需传入 `-sPTHREAD_POOL_SIZE=0`，无需调用 `set_worker_count(1)`。

## 浏览器支持状态

当前仓库还没有浏览器加载器、npm 包或 Embind API，因此该目标仅支持 Node.js。使用
Emscripten pthreads 的浏览器构建还需要提供跨源隔离标头的 Web 服务器
(`Cross-Origin-Opener-Policy: same-origin` 和
`Cross-Origin-Embedder-Policy: require-corp`)，以及生成的 worker 资源。

## Phase 2 计划

1. **2-A — Embind JS bindings.** 暴露 `GraphEngine`、`RunConfig`、`ChannelWrite`、`Send`、`Command` 给 JS。JS function 可以把自己注册为 node implementation；engine 会在每次 node execution 时回调 JS。预计 1-2 天。

2. **2-B — fetch-based HTTP transport.** 提供一个由 `SchemaProvider` 消费的 transport interface；WASM build 将其接到 `fetch()`。同一份 provider code 可面向任一 backend。预计 3-5 天。

3. **2-C — npm package.** 发布为 `@neograph/wasm`，让 apps 可以 `npm install` engine + JS bindings，而无需自己 build。预计 1-2 天。

Phase 2 之后，引擎可以在 browser tab 中完全运行 Originator-issued graphs — leaves 通过 `fetch()` 调用 BYOK Anthropic / OpenAI / Bedrock keys，使用 transformers.js / built-in AI 做 local inference，结果通过 channels 流回 Result Envelope。这是 [NeoProtocol](https://github.com/fox1245/NeoProtocol) Executor role 的 runtime side。
