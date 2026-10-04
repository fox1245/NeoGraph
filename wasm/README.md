# NeoGraph WASM smoke program

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`smoke.cpp` compiles a graph with one `DoubleNode`, writes `doubled = seed * 2`,
and runs it with `InMemoryCheckpointStore`. With `seed = 21`, its expected output
includes `doubled = 42` and `trace = d`. It performs no network or model call.
This is a Node.js smoke target, not a browser SDK.

## Current build boundary

The typed provider cutover makes `SchemaProvider::runtime` a dependency of Core,
even with `NEOGRAPH_BUILD_LLM=OFF`. CMake 3.20+ selects an explicit
`NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`, then an installed runtime, then a pinned
public GitHub source archive. The download fallback defaults to ON through
`NEOGRAPH_FETCH_SCHEMAPROVIDER`; turn it OFF for offline package/source builds.
The SDK runtime also needs libcurl, OpenSSL, and threads. Its current transport
and archive have platform-specific dependencies; this worktree has no qualified
Emscripten SDK runtime build. A native SDK installation cannot be linked into
WebAssembly. The historical smoke below does not establish that the current
source tree builds under Emscripten.

NeoGraph `0.13.0` requires alpha SDK `0.1.0`, interface revision/shared
generation 4, built for the same target. Qualifying a genuine Emscripten SDK
runtime remains a prerequisite; this page neither drops WASM support nor
guarantees a current build. Archive v3 / `spna3` and portable JSON v2 are unchanged.

## Historical result

These values came from an earlier local smoke run, before the typed SDK cutover.
No generated `.wasm` or `.js` artifact is committed, and CI does not publish a
WASM size artifact. Sizes describe that build, not current deployment costs.

| Metric | Value |
|---|---|
| WASM binary (-O3 + LTO) | 712 KB |
| Emscripten JS runtime | 92 KB |
| Total JavaScript + WASM | ~800 KB |
| Engine source diff in that run | 0 lines |
| First run output | `doubled = 42, trace = d` |

## Target and prerequisites

The target links `neograph_core` directly. It uses the main Core source list,
C++20 coroutines, exceptions, and Emscripten pthreads. CMake enables Asio's
coroutine surface for distro Emscripten 3.1.x and disables unsupported
stack-protector symbols only for WASM targets; native hardening is unchanged.
The target sets `PTHREAD_POOL_SIZE=4`, while the graph uses the default
`worker_count=1`. There is no CMake option here for a single-thread WASM variant.

Once a working Emscripten build of the SDK runtime is available, the target's
configure/build/run sequence is:

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

`NEOGRAPH_USE_LIBCURL=OFF` disables NeoGraph's optional HTTP/2 backend, not the
SDK runtime's libcurl dependency. This sequence is not a verified current build.

For Node.js versions whose generated loader passes a filesystem path to `fetch`,
the historical loader workaround was:

```bash
node -e 'const fs=require("fs"); WebAssembly.instantiateStreaming=undefined; global.fetch=async p=>({ok:true,arrayBuffer:async()=>fs.promises.readFile(p)}); require("./build-wasm/wasm/smoke.js");'
```

## Browser status and proposed work

There is no browser loader, npm package, or Embind API in this repository.
Browser pthread builds need cross-origin isolation headers
(`Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp`) and generated worker assets;
these requirements alone do not make the SDK runtime browser-compatible.

A browser port would first need an SDK-compatible transport and archive design,
then JS node callbacks and packaging. A proposed `fetch()` adapter, hosted-model
access, local browser inference, and NeoProtocol Executor integration are not
shipped or qualified by this smoke program.
