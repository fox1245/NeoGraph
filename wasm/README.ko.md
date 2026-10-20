<!-- neograph-i18n: source=wasm/README.md locale=ko source_sha256=8e7b97974a770ba4d847ca114f60b481fbf19ceec801c1203da661cdf6b459af -->
# WebAssembly에서의 NeoGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

이 디렉터리에는 CI가 빌드하고 실행하는 WebAssembly 프로그램이 있습니다. `smoke.cpp`는 `DoubleNode` 하나로 이루어진
그래프를 컴파일하고, `doubled = seed * 2`를 쓰며, `InMemoryCheckpointStore`로 실행합니다. `seed = 21`이면
`doubled = 42`와 `trace = d`를 출력합니다. `checks.cpp`는 스레드가 없는 32비트 호스트에서 달라지는 점(아래)을
검사합니다. 어느 쪽도 네트워크나 모델 호출을 하지 않습니다. 지금은 그래프 엔진이 WebAssembly 호스트에서 실행됩니다.
모델 호출, Program, 저장소는 아직 아닙니다. 계획은 [#535](https://github.com/fox1245/NeoGraph/issues/535)에 있으며,
이것은 브라우저 SDK가 아닙니다.

## 상태

| 항목 | 상태 |
|---|---|
| 툴체인 | Emscripten 6.0.13. CI에서 emsdk 릴리스 커밋으로 고정합니다. |
| 빌드 모드 | 싱글 스레드(WebAssembly의 기본값)와 pthread. 둘 다 CI에서 빌드하고 실행합니다. |
| 런타임 | CI의 Node.js 22와 24. 브라우저, Cloudflare Workers, Deno, WASI는 검증하지 않았습니다. |
| SDK | SchemaProvider 0.4.0, 인터페이스 리비전 7. `cmake/NeoGraphSchemaProvider.cmake`가 고정한 커밋을 씁니다. libcurl 없이, 스레드 없이 빌드됩니다. |
| 구성 요소 | Core, 그리고 선택적으로 Program. |

## 빌드

빌드에는 Emscripten 6.0.13(C++20 코루틴, `<stop_token>`, `std::bit_cast`가 libc++에 있습니다), CMake 3.20 이상,
Ninja가 필요합니다. LLM, ASYNC, MCP 클라이언트, A2A, gRPC, SQLite와 PostgreSQL 저장소는 WebAssembly 호스트가 제공하지
않는 소켓, TLS, 시스템 라이브러리가 필요하므로 CMake가 거부하며, 아래에서는 꺼 둡니다.

싱글 스레드, WebAssembly의 기본값:

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

pthread: configure 줄에 `-DNEOGRAPH_THREADS=ON`을 더하고 다른 빌드 디렉터리를 씁니다.
`NEOGRAPH_BUILD_PROGRAM=ON`은 네이티브 플러그인 검사에만 필요합니다. 그래프 smoke만 빌드하려면 끄세요.

`NEOGRAPH_THREADS`가 두 모드를 가르는 유일한 스위치입니다. Emscripten과 WASI에서는 `OFF`, 그 밖에서는 `ON`입니다.

- **싱글 스레드(`OFF`).** `-pthread`도 `Threads::Threads`도 없습니다. `asio` 타깃이 `ASIO_DISABLE_THREADS`를 내보내고,
  SDK도 같은 방식으로 빌드합니다(`SP_THREADS=OFF`, libcurl 트랜스포트 없음). 작업은 호스트가 제공하는
  executor에서 실행됩니다.
- **pthread(`ON`).** 모든 곳에 `-pthread`를 쓰고, smoke 프로그램에는 워커 4개짜리 풀을 둡니다. SDK는
  `SP_THREADS=ON`으로 빌드되며, WebAssembly에서는 여전히 libcurl 트랜스포트가 없습니다.
- **예외.** `-fexceptions`(JavaScript 기반 예외)를 C 소스를 포함한 모든 NeoGraph 타깃에 지정하고, SDK도 같은 플래그를
  씁니다. 이미 `-fexceptions`나 `-fwasm-exceptions`를 지정한 설정은 그대로 두며, 예외가 꺼져 있으면 configure가
  실패합니다.
- **프로그램 하나에 설정 하나.** Asio는 헤더 전용이므로 스레드 설정이 서로 다른 번역 단위는 어떤 도구도 잡아내지 못하는
  ODR 위반이 됩니다. 생성되는 `neograph/build_config.h`가 설정을 기록하고, 모든 공개 헤더가 `neograph/api.h`를 통해
  이 헤더에 닿으며, 설정이 다른 번역 단위는 컴파일되지 않습니다. SDK도 자신의 설정을 같은 방식으로 기록하고
  (`sp/build_config.h`), 두 설정이 다르면 configure가 실패합니다.

## 측정한 크기

Release, `-O3 -flto`, Emscripten 6.0.13. 그래프 smoke는 Core 엔진과 SDK 런타임을 포함합니다.

| 빌드 | `.wasm` | `.wasm` gzip -9 | `.js` |
|---|---:|---:|---:|
| 그래프 smoke, 싱글 스레드 | 5,502,805 B (5.2 MiB) | 1,607,052 B (1.5 MiB) | 83,223 B |
| 그래프 smoke, pthread | 5,448,684 B (5.2 MiB) | 1,660,246 B (1.6 MiB) | 103,691 B |
| Program을 포함한 checks, 싱글 스레드 | 5,035,831 B (4.8 MiB) | 1,559,615 B (1.5 MiB) | 83,031 B |

CI는 싱글 스레드 smoke를 `.wasm` 6,000,000바이트, gzip한 크기 1,750,000바이트(측정한 크기에 약 9%를 더한 값)로 제한하고, 크기를 잡 요약에 보고합니다.
이전 버전의 이 문서에 있던 712 KB는 타입이 있는 SDK 이전에 잰 값이어서 더 이상 맞지 않습니다.

## 이 호스트에서 쓸 수 없는 것

NeoGraph가 호출을 소유하는 경우 타입이 있는 오류로 실패하고, 그렇지 않으면 빌드되지 않습니다.

- **모델 호출.** LLM, ASYNC, MCP 클라이언트, A2A는 소켓과 TLS가 필요합니다. 호스트의 `fetch`를 통한 모델 호출은
  #535의 M2에서 옵니다.
- **Program.** Program 구성 요소는 컴파일되지만 `ProgramRuntime`이 스레드를 시작하므로, 싱글 스레드 빌드에서는 생성,
  노드 attempt 타임아웃, `n > 1`인 `set_worker_count(n)`이 `thread: Not supported`로 실패합니다(#557). 호스트의
  Program 저장소는 #556에서 다룹니다.
- **네이티브 Program 컨트롤.** C v1 플러그인 ABI는 64비트 레이아웃이므로 wasm32에서
  `NativeControlBinding::create`는 `NativeControlUnavailable`로 거부합니다.
- **동기 체크포인트 저장소.** 비동기가 아닌 저장소는 각 연산을 워커 스레드 풀에서 실행합니다. 싱글 스레드 빌드에는
  풀이 없으므로, 스레드를 기다리는 대신 `SynchronousCheckpointStoreUnavailable`로 연산을 거부합니다.
  `AsyncCheckpointStore`를 구현하세요. 내장 인메모리 저장소는 managed 예산을 포함한 모든 연산을 호출한 스레드에서
  실행하므로, `model_token_budget`이 있는 실행이 그 위에서 동작합니다.
- **기한이 있는 managed 예산.** 기한은 재시작 후에도 복구할 수 있는 시계 원점에 묶이는데 WebAssembly 호스트에는 그런
  것이 없습니다. 실행은 저장소에 묻기 전에 `ManagedBudgetDeadlineClockUnavailable`로 거부됩니다.
- **네이티브 아카이브.** 파일 보관(custody) 백엔드가 없어서 SDK가 `Unsupported` 오류로 거부합니다.
- **서브프로세스 위임**(Harness CLI, stdio MCP 서버), **수신 소켓**, **gRPC**, **파일 경로 업로드**, **Python**.

## 브라우저

브라우저 하니스는 저장소에 없습니다. `serve_smoke.py`는 존재하지 않는 `smoke.html`을 기대하며, 브라우저 실행 기록도
없습니다(#435). Node.js 실행은 브라우저에서 동작한다는 증거가 아닙니다.

## 마일스톤

툴체인 검토 뒤의 계획은 [#535](https://github.com/fox1245/NeoGraph/issues/535)에 있습니다.

- **M0과 M1(이 문서).** wasm32 수정, 싱글 스레드 빌드, CI의 Node smoke와 크기 게이트.
- **M2.** 호스트의 `fetch`를 통한 모델 호출. 브라우저와 Workers에서.
- **M3.** 호스트 저장소 위의 Program([#557](https://github.com/fox1245/NeoGraph/issues/557),
  [#556](https://github.com/fox1245/NeoGraph/issues/556)).
- **M4.** 실시간 세션, WASI, 네이티브 아카이브용 보관 백엔드, 원격 Harness.
