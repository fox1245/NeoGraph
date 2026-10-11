# NeoGraph WASM smoke program

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`smoke.cpp` compiles a graph with one `DoubleNode`, writes `doubled = seed * 2`,
and runs it with `InMemoryCheckpointStore`. With `seed = 21`, its expected output
includes `doubled = 42` and `trace = d`. It performs no network or model call.
The same generated target can run in Node.js or the browser harness below;
this is not a browser SDK.

## Current build boundary

The typed provider cutover makes `SchemaProvider::runtime` a dependency of Core,
even with `NEOGRAPH_BUILD_LLM=OFF`. CMake 3.20+ selects an explicit
`NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`, then an installed runtime, then a pinned
public GitHub source archive. The download fallback defaults to ON through
`NEOGRAPH_FETCH_SCHEMAPROVIDER`; turn it OFF for offline package/source builds.
Core links the SDK runtime, not its libcurl transport. At the examined SDK pin,
the top-level SDK CMake still requires CURL during configuration even though
the runtime target does not link it; OpenSSL is not a direct runtime dependency.
Native SDK libraries cannot be linked into WebAssembly. Runtime/archive and
C++ standard-library support must be qualified for the Emscripten target;
the historical smoke below does not establish a current successful build.

Current builds require SDK `0.3.0`, interface revision/shared generation 6,
at merged SchemaProvider PR #21 (`83112573ba59e3b561fc33c22394638be7aa5294`),
built for the same target. Qualifying a genuine Emscripten SDK
runtime remains a prerequisite; this page neither drops WASM support nor
guarantees a current build. Archive v3 / `spna3` and portable JSON v2 are unchanged.

### Supported-target requirements

The Emscripten toolchain and C++ standard library must support the SDK's
C++20 contracts, including `std::bit_cast` and `std::stop_token`. Native
archive custody requires a supported atomic no-replace filesystem backend
for the target; custody must not be disabled to obtain a build.

Use a fresh build directory and genuine same-target dependencies, including
any CURL dependency required during SDK configuration. Native libraries or
fabricated dependency availability cannot qualify a WebAssembly build.
Current Node.js and browser execution remain unqualified.

## Historical result

These values came from an earlier smoke run, before the typed SDK cutover.
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

`NEOGRAPH_USE_LIBCURL=OFF` disables NeoGraph's optional HTTP/2 backend. It does
not remove the SDK's top-level CURL discovery. This sequence still requires
genuine Emscripten dependencies and is not a verified current build.

For Node.js versions whose generated loader passes a filesystem path to `fetch`,
the historical loader workaround was:

```bash
node -e 'const fs=require("fs"); WebAssembly.instantiateStreaming=undefined; global.fetch=async p=>({ok:true,arrayBuffer:async()=>fs.promises.readFile(p)}); require("./build-wasm/wasm/smoke.js");'
```

## Browser smoke

After the genuine target builds, serve its JS, WASM, and generated pthread
worker assets using the standard-library-only loopback server:

```sh
python3 wasm/serve_smoke.py build-wasm/wasm --port 8765
# Open http://127.0.0.1:8765/smoke.html in a browser.
```

The server sends `Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp`. The harness refuses execution
without cross-origin isolation / `SharedArrayBuffer`; it loads the actual
generated `smoke.js`, not a substitute runtime. A pass requires output
`doubled = 42`, `trace = d`, and exit code 0, exposed as
`document.body.dataset.result === "pass"` and `dataset.exitCode === "0"`.
Record browser/version, console errors, and the observed output. A successful
Node.js run alone is not browser evidence.

This harness supplies no npm package, Embind API, JS graph callbacks, or
provider transport. Hosted-model access, local inference, and NeoProtocol
Executor integration are not qualified by this model-free smoke.
