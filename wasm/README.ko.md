<!-- neograph-i18n: source=wasm/README.md locale=ko source_sha256=998352566107627e9cf22b256b5d1e2648275aa3c8ad5bd60d513c6fc59a7314 -->
# NeoGraph WASM smoke 프로그램

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`smoke.cpp`는 `DoubleNode` 하나로 그래프를 컴파일하고 `doubled = seed * 2`를 쓴 뒤 `InMemoryCheckpointStore`로 실행합니다. `seed = 21`이면 예상 출력은 `doubled = 42`와 `trace = d`를 포함합니다. 네트워크나 모델 호출은 없습니다. 브라우저 SDK가 아니라 Node.js smoke 타깃입니다.

## 현재 빌드 경계

타입 공급자 전환 이후 `NEOGRAPH_BUILD_LLM=OFF`여도 Core는 `SchemaProvider::runtime`에 의존합니다.
CMake 3.20+는 명시적 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`, 설치 runtime, 고정 public GitHub source archive 순으로 SDK를 선택합니다.
`NEOGRAPH_FETCH_SCHEMAPROVIDER`의 download fallback은 기본 ON이며 offline package/source 빌드에서는 OFF로 설정하세요.
SDK runtime에는 libcurl, OpenSSL, threads도 필요합니다. 현재 transport와 archive에는 플랫폼별 의존성이 있으며 이 worktree의 Emscripten SDK runtime 빌드는 검증되지 않았습니다. native SDK를 WebAssembly에 링크할 수는 없습니다. 아래 과거 smoke 결과는 현재 소스가 Emscripten으로 빌드된다는 증거가 아닙니다.

NeoGraph `0.13.0`에는 동일 target용 alpha SDK `0.1.0`, interface revision/shared generation 4가 필요합니다. 실제 Emscripten SDK runtime 검증은 여전히 선행 조건입니다. 이 문서는 WASM 지원을 삭제하거나 현재 빌드를 보장하지 않습니다. Archive v3 / `spna3`와 portable JSON v2는 그대로입니다.

## 과거 결과

아래 값은 타입 SDK 전환 전의 로컬 smoke 실행에서 나온 값입니다. 생성된 `.wasm`/`.js`는 커밋하지 않았고 CI도 WASM 크기 artifact를 배포하지 않습니다. 크기는 당시 빌드를 설명하며 현재 배포 비용을 뜻하지 않습니다.

| 지표 | 값 |
|---|---|
| WASM binary (-O3 + LTO) | 712 KB |
| Emscripten JS runtime | 92 KB |
| JavaScript + WASM 합계 | ~800 KB |
| 당시 엔진 소스 변경 | 0 lines |
| 첫 실행 출력 | `doubled = 42, trace = d` |

## 타깃과 전제 조건

타깃은 `neograph_core`에 직접 링크하고 Core의 소스 목록, C++20 coroutine, exception, Emscripten pthread를 사용합니다. CMake는 distro Emscripten 3.1.x의 Asio coroutine을 활성화하고 WASM 타깃에서만 지원되지 않는 stack-protector symbol을 비활성화합니다. native hardening은 그대로입니다. 타깃은 `PTHREAD_POOL_SIZE=4`, 그래프는 기본 `worker_count=1`을 사용합니다. 여기에는 단일 스레드 WASM용 CMake 옵션이 없습니다.

동작하는 Emscripten SDK runtime 빌드가 확보된 후 사용할 configure/build/run 명령은 다음과 같습니다.

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

`NEOGRAPH_USE_LIBCURL=OFF`는 NeoGraph의 선택적 HTTP/2 backend만 끄며 SDK runtime의 libcurl 의존성은 끄지 않습니다. 현재 검증된 빌드 명령이 아닙니다.

생성 loader가 filesystem path를 `fetch`에 전달하는 Node.js에서 사용한 과거 우회 명령은 다음과 같습니다.

```bash
node -e 'const fs=require("fs"); WebAssembly.instantiateStreaming=undefined; global.fetch=async p=>({ok:true,arrayBuffer:async()=>fs.promises.readFile(p)}); require("./build-wasm/wasm/smoke.js");'
```


## 브라우저 상태와 제안된 작업

이 저장소에는 browser loader, npm package, Embind API가 없습니다. browser pthread 빌드는 cross-origin isolation header(`Cross-Origin-Opener-Policy: same-origin`, `Cross-Origin-Embedder-Policy: require-corp`)와 생성된 worker asset이 필요합니다. 이것만으로 SDK runtime이 브라우저와 호환되는 것은 아닙니다.

브라우저 포트에는 먼저 SDK와 호환되는 transport/archive 설계가 필요하며 이후 JS node callback과 packaging을 구현해야 합니다. 제안된 `fetch()` adapter, hosted model 접근, local browser inference, NeoProtocol Executor 연동은 이 smoke 프로그램에서 제공하거나 검증하지 않습니다.
