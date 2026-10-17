<!-- neograph-i18n: source=wasm/README.md locale=ko source_sha256=d8bdd389cf6283acf21ef72f09e20605fedd14159f577eb18184463e046c9dd8 -->
# NeoGraph WASM 스모크 프로그램

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`smoke.cpp`는 `DoubleNode` 하나로 이루어진 그래프를 컴파일하고, `doubled = seed * 2`를 기록한 뒤
`InMemoryCheckpointStore`로 실행합니다. `seed = 21`이면 출력에 `doubled = 42`와 `trace = d`가
나와야 합니다. 네트워크 호출이나 모델 호출은 하지 않습니다. 같은 빌드 결과물을 Node.js에서도,
아래의 브라우저 하네스에서도 실행할 수 있습니다. 다만 브라우저용 SDK는 아닙니다.

## 현재 빌드 범위

Core는 `NEOGRAPH_BUILD_LLM=OFF`여도 `SchemaProvider::runtime`에
의존합니다. CMake 3.20 이상은 먼저 명시적으로 지정한 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`을
고르고, 없으면 설치된 런타임을, 그것도 없으면 버전이 고정된 공개 GitHub 소스 아카이브를
선택합니다. 다운로드 대체 경로는 `NEOGRAPH_FETCH_SCHEMAPROVIDER`로 제어하며 기본값은 ON입니다.
오프라인 패키지/소스 빌드에서는 OFF로 바꿉니다. Core는 SDK 런타임에 링크하며, SDK의 libcurl 전송
계층에는 링크하지 않습니다. 확인한 SDK 고정 버전(pin)에서는 런타임 타깃이 CURL에 링크하지 않는데도,
최상위 SDK CMake가 구성 단계에서 여전히 CURL을 요구합니다. OpenSSL은 런타임의 직접 의존성이
아닙니다. 네이티브 SDK 라이브러리는 WebAssembly에 링크할 수 없습니다. Emscripten 타깃에서
SDK 런타임, 아카이브 지원, C++ 표준 라이브러리가 동작해야 하며, 아래의 과거 스모크 결과가 현재
코드의 빌드 성공을 보여 주지는 않습니다.

현재 빌드에는 SDK `0.3.3`, 인터페이스 리비전/공유 라이브러리 세대 6이 필요하며, SchemaProvider
`maint/interface6`의 커밋 `0437853529b5932e7097a38629a95779d08f03aa`를 같은 타깃용으로 빌드해야
합니다. 실제 Emscripten용 SDK 런타임 빌드가 선행 조건이며,
이 문서는 현재 빌드의 성공을 보장하지 않습니다.

### 지원 대상의 요구 사항

Emscripten 툴체인과 C++ 표준 라이브러리는 `std::bit_cast`, `std::stop_token`을 포함한 SDK의
C++20 계약을 지원해야 합니다. 네이티브 아카이브 보관(custody)에는 그 타깃에서 지원되는,
덮어쓰지 않는(no-replace) 원자적 파일 시스템 백엔드가 필요합니다. 빌드를 통과시키려고 보관
기능을 꺼서는 안 됩니다.

새 빌드 디렉터리를 쓰고, SDK 구성 단계에 필요한 CURL 의존성을 포함해 같은 타깃용으로 만든 실제
의존성을 사용합니다. 네이티브 라이브러리를 쓰거나 의존성이 있는 것처럼 꾸며서는 유효한 WebAssembly
빌드가 되지 않습니다. 현재 Node.js와 브라우저에서의 실행은 아직 확인되지 않았습니다.

## 과거 결과

아래 값은 typed SDK 전환 이전에 실행한 스모크 결과입니다. 생성된 `.wasm`이나 `.js` 산출물은
커밋되어 있지 않고, CI도 WASM 크기 아티팩트를 게시하지 않습니다. 이 크기는 당시 빌드의 값일 뿐
현재 배포 비용이 아닙니다.

| 항목 | 값 |
|---|---|
| WASM 바이너리 (-O3 + LTO) | 712 KB |
| Emscripten JS 런타임 | 92 KB |
| JavaScript + WASM 합계 | ~800 KB |
| 당시 실행에서 엔진 소스 diff | 0줄 |
| 첫 실행 출력 | `doubled = 42, trace = d` |

## 타깃과 사전 요구 사항

이 타깃은 `neograph_core`에 직접 링크합니다. Core의 주 소스 목록, C++20 코루틴, 예외, Emscripten
pthread를 사용합니다. 배포판(distro)에서 제공하는 Emscripten 3.1.x를 위해 CMake가 Asio의 코루틴
인터페이스를 켜고, 지원되지 않는 스택 보호(stack-protector) 심볼은 WASM 타깃에서만 끕니다.
네이티브 빌드의 하드닝 설정은 그대로입니다. 타깃은 `PTHREAD_POOL_SIZE=4`로 설정하지만 그래프는
기본값인 `worker_count=1`을 씁니다. 단일 스레드 WASM 변형을 위한 CMake 옵션은 없습니다.

Emscripten으로 빌드한 SDK 런타임이 준비되면, 이 타깃의 구성/빌드/실행 순서는 다음과 같습니다.

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

`NEOGRAPH_USE_LIBCURL=OFF`는 NeoGraph의 선택 기능인 HTTP/2 백엔드를 끕니다. SDK 최상위의 CURL
탐색은 없애지 않습니다. 이 순서를 따르려면 여전히 실제 Emscripten용 의존성이 필요하며, 현재
빌드가 검증된 것은 아닙니다.

생성된 로더가 `fetch`에 파일 시스템 경로를 넘기는 Node.js 버전에서는, 예전에 다음 로더 우회
방법을 썼습니다.

```bash
node -e 'const fs=require("fs"); WebAssembly.instantiateStreaming=undefined; global.fetch=async p=>({ok:true,arrayBuffer:async()=>fs.promises.readFile(p)}); require("./build-wasm/wasm/smoke.js");'
```

## 브라우저 스모크

실제 타깃을 빌드했다면, 표준 라이브러리만 쓰는 루프백 서버로 JS, WASM, 생성된 pthread 워커
에셋을 서빙합니다.

```sh
python3 wasm/serve_smoke.py build-wasm/wasm --port 8765
# Open http://127.0.0.1:8765/smoke.html in a browser.
```

서버는 `Cross-Origin-Opener-Policy: same-origin`과 `Cross-Origin-Embedder-Policy: require-corp`
헤더를 보냅니다. 하네스는 cross-origin isolation / `SharedArrayBuffer`가 없으면 실행을
거부하며, 대체 런타임이 아니라 실제로 생성된 `smoke.js`를 불러옵니다. 통과하려면 출력이
`doubled = 42`, `trace = d`이고 종료 코드가 0이어야 하며, 이 결과는
`document.body.dataset.result === "pass"`와 `dataset.exitCode === "0"`으로 노출됩니다. 브라우저와
버전, 콘솔 오류, 관찰한 출력을 기록해 둡니다. Node.js 실행에 성공했다는 것만으로는 브라우저에서
동작한다는 증거가 되지 않습니다.

이 하네스는 npm 패키지, Embind API, JS 그래프 콜백, provider 전송 계층을 제공하지 않습니다.
호스팅 모델 접근, 로컬 추론, NeoProtocol Executor 통합은 모델을 쓰지 않는 이 스모크의
범위 밖입니다.
