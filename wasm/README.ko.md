<!-- neograph-i18n: source=wasm/README.md locale=ko source_sha256=7c8f151dff4b7ff5a98911a0fea484b6ce1676ec148e844d900366af7b7d2a1e -->
# NeoGraph WASM smoke 프로그램

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`smoke.cpp`는 `DoubleNode` 하나로 그래프를 컴파일하고 `doubled = seed * 2`를 쓴 뒤 `InMemoryCheckpointStore`로 실행합니다. `seed = 21`이면 예상 출력은 `doubled = 42`와 `trace = d`를 포함합니다. 네트워크나 모델 호출은 없습니다. 같은 생성 타깃을 Node.js 또는 아래 브라우저 harness에서 실행할 수 있지만 브라우저 SDK는 아닙니다.

## 현재 빌드 경계

타입 공급자 전환 이후 `NEOGRAPH_BUILD_LLM=OFF`여도 Core는 `SchemaProvider::runtime`에 의존합니다.
CMake 3.20+는 명시적 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`, 설치 runtime, 고정 public GitHub source archive 순으로 SDK를 선택합니다.
`NEOGRAPH_FETCH_SCHEMAPROVIDER`의 download fallback은 기본 ON이며 offline package/source 빌드에서는 OFF로 설정하세요.
Core는 SDK runtime에 링크하며 libcurl transport에는 링크하지 않습니다. 조사한 SDK pin의 최상위 CMake는 runtime 타깃이 CURL에 링크하지 않아도 구성 시 CURL을 요구합니다. OpenSSL은 runtime의 직접 의존성이 아닙니다. native SDK library를 WebAssembly에 링크할 수는 없습니다. Emscripten용 runtime/archive와 C++ 표준 라이브러리 지원을 검증해야 하며 아래 과거 smoke는 현재 빌드 성공의 증거가 아닙니다.

현재 빌드에는 동일 target용 SDK `0.3.0`, interface revision/shared generation 6 및 병합된 SchemaProvider PR #21(`83112573ba59e3b561fc33c22394638be7aa5294`)이 필요합니다. 실제 Emscripten SDK runtime 검증은 여전히 선행 조건입니다. 이 문서는 WASM 지원을 삭제하거나 현재 빌드를 보장하지 않습니다. Archive v3 / `spna3`와 portable JSON v2는 그대로입니다.

### 지원 대상 요구사항

Emscripten 도구 체인과 C++ 표준 라이브러리는 `std::bit_cast`와 `std::stop_token`을 포함한 SDK의 C++20 계약을 지원해야 합니다. native archive custody에는 해당 target에서 지원되는 atomic no-replace filesystem backend가 필요하며 빌드를 위해 custody를 비활성화해서는 안 됩니다.

새 빌드 디렉터리와 실제 동일 target 의존성을 사용하세요. SDK 구성에 필요한 CURL 의존성도 포함합니다. native library나 위조된 의존성 가용성으로 WebAssembly 빌드를 검증할 수 없습니다. 현재 Node.js와 브라우저 실행은 검증되지 않았습니다.

## 과거 결과

아래 값은 타입 SDK 전환 전의 과거 smoke 실행에서 나온 값입니다. 생성된 `.wasm`/`.js`는 커밋하지 않았고 CI도 WASM 크기 artifact를 배포하지 않습니다. 크기는 당시 빌드를 설명하며 현재 배포 비용을 뜻하지 않습니다.

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

`NEOGRAPH_USE_LIBCURL=OFF`는 NeoGraph의 선택적 HTTP/2 backend만 끕니다. SDK 최상위 CURL 검색은 제거하지 않습니다. 실제 Emscripten 의존성이 필요하며 현재 검증된 빌드 명령은 아닙니다.

생성 loader가 filesystem path를 `fetch`에 전달하는 Node.js에서 사용한 과거 우회 명령은 다음과 같습니다.

```bash
node -e 'const fs=require("fs"); WebAssembly.instantiateStreaming=undefined; global.fetch=async p=>({ok:true,arrayBuffer:async()=>fs.promises.readFile(p)}); require("./build-wasm/wasm/smoke.js");'
```


## 브라우저 smoke

실제 타깃 빌드 후 표준 라이브러리만 사용하는 loopback 서버로 JS, WASM, 생성된 pthread worker asset을 제공합니다.

```sh
python3 wasm/serve_smoke.py build-wasm/wasm --port 8765
# Open http://127.0.0.1:8765/smoke.html in a browser.
```

서버는 `Cross-Origin-Opener-Policy: same-origin`과 `Cross-Origin-Embedder-Policy: require-corp`를 보냅니다. harness는 cross-origin isolation / `SharedArrayBuffer`가 없으면 실행을 거부하며 대체 runtime이 아닌 실제 생성된 `smoke.js`를 로드합니다. 통과하려면 출력 `doubled = 42`, `trace = d` 및 종료 코드 0이 필요합니다. 이는 `document.body.dataset.result === "pass"`와 `dataset.exitCode === "0"`으로 노출됩니다. 브라우저/버전, console 오류 및 관측한 출력을 기록하세요. Node.js 성공만으로는 브라우저 증거가 아닙니다.

이 harness는 npm package, Embind API, JS graph callback, provider transport를 제공하지 않습니다. hosted model 접근, local inference, NeoProtocol Executor 연동은 이 모델 없는 smoke로 검증되지 않습니다.
