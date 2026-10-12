<!-- neograph-i18n: source=docs/VALGRIND.md locale=ko source_sha256=21a939b18bdcef34c5ead883275d4932d5f9eeec6c25401be1dfd8836025e75f -->
# 메모리 및 새니타이저 검사

**Languages:** [English](VALGRIND.md) | [한국어](VALGRIND.ko.md) | [日本語](VALGRIND.ja.md) | [简体中文](VALGRIND.zh-CN.md)

## 범위와 전제 조건

잘못된 메모리 접근과 할당 누수는 Memcheck로, 주소 관련 오류는 ASan으로, 점검 범위에 들어 있는 정의되지 않은 동작은 UBSan으로, 데이터 레이스는 TSan으로 찾습니다. 도구 하나를 통과했다고 해서 다른 도구가 점검하는 항목까지 확인된 것은 아닙니다.

현재 Core 빌드에는 LLM 노드와 PostgreSQL을 끄더라도 외부 `SchemaProvider::runtime`이 필요합니다. 맞는 버전으로 설치한 SDK를 `SDK_PREFIX`로 지정하거나, `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`과 그 개발 의존성을 제공해야 합니다. 새니타이저로 SDK 내부까지 보려면 SDK도 같은 계측 옵션(`SP_SANITIZE=address` 또는 `thread`)으로 빌드합니다.

NeoGraph에는 CMake 3.20 이상이 필요합니다. SDK 소스는 명시한 소스 디렉터리를 가장 먼저 쓰고, 다음으로 설치된 패키지를, 마지막으로 리비전이 고정된 공개 archive를 내려받아 씁니다(기본 동작). 설치된 SDK를 오프라인에서 쓰려면 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`로 지정합니다. 소스로 빌드하거나 내려받아 빌드할 때는 NeoGraph의 Asio/yyjson을 쓰지만, 시스템의 libcurl/OpenSSL 개발 파일은 여전히 필요합니다.

## Memcheck 시나리오 실행

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug \
  -DNEOGRAPH_BUILD_TESTS=ON -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_POSTGRES=OFF -DCMAKE_PREFIX_PATH="$SDK_PREFIX"
cmake --build build-debug --target example_custom_graph -j
valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all \
  --error-exitcode=42 ./build-debug/example_custom_graph
```

다시 빌드한 산출물로 네트워크가 필요 없는 관련 예제와 provider 수명/취소 시나리오를 실행합니다. 네트워크 경로와 Python 경로에는 범위를 좁힌 진단과, 검토를 마친 서드파티 suppression이 필요합니다. 이 경로들도 메모리 검사 대상에서 빠지지 않습니다. mock provider로 실행해서는 SDK 전송 계층이나 실제 네이티브 재생(replay) 보관(custody)을 점검하지 못합니다.

## ASan, UBSan 방법

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$SDK_PREFIX" \
  -DNEOGRAPH_BUILD_TESTS=ON -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
  LSAN_OPTIONS="suppressions=$PWD/tests/lsan_suppressions.txt" \
  ctest --test-dir build-asan -E 'BIG_|valgrind' --output-on-failure
```

Python에서는 확장을 import하기 전에 맞는 ASan 런타임을 먼저 preload하고, 누수 검사를 통째로 끄는 대신 `tests/lsan_suppressions.txt`를 사용합니다. 실제 인터프리터 설정은 CI 워크플로에 정의되어 있습니다. 제외한 항목이 있으면 결과와 함께 보고하고.

## CI와 하드닝 계약

[확장 CI 워크플로](../.github/workflows/ci-extended.yml)는 `sanitizer-test`, `tsan-test`, `fuzz-canary`를 정의합니다. 이 작업들은 push마다 실행되지 않고 매일 밤과 요청 시에 실행됩니다. C++ 테스트/예제와 Python은 ASan/LSan으로, 동시성 경로는 TSan으로, 컴파일러 입력은 libFuzzer로 점검합니다.

TSan과 ASan은 빌드를 따로 해야 합니다. Linux의 `setarch x86_64 -R`은 `ADDR_NO_RANDOMIZE`를 설정해 주소 무작위화를 끄지만, 권한이나 커널의 주소 배치 때문에 TSan이 막힐 수 있습니다. ASan을 통과했다고 해서 레이스가 없다는 뜻은 아닙니다.

`NEOGRAPH_ENABLE_HARDENING=ON`은 컴파일러/플랫폼별로 검증된 assertion, stack-protector, FORTIFY, 링커 플래그를 켭니다. 설정된 `-fcf-protection` 옵션은 Linux x86에서 지원되며, 다른 플랫폼에는 이 보장이 적용되지 않습니다. 새니타이저 플래그가 있거나 MSVC를 쓰는 경우 CMake는 이 플래그들을 건너뜁니다. CET 태깅이 모든 제어 흐름 공격을 막는다는 일반적인 보장은 아닙니다.

하드닝 비용을 한 번 측정했을 때 기준선은 seq 5.1 µs / par 275.2 µs, 하드닝 적용 후는 seq 5.1 µs / par 275.6 µs였습니다. 이는 그 측정에서 감지할 만한 차이가 없었다는 뜻이지, 모든 워크로드에서 비용이 0이라는 뜻은 아닙니다.

## 초기화되지 않은 읽기와 suppression

MemorySanitizer를 쓰려면 C++ 표준 라이브러리와 런타임 라이브러리를 포함한 의존성이 모두 계측되어 있어야 합니다. ASan/UBSan/TSan은 초기화되지 않은 읽기를 일반적으로 찾아 주는 도구가 아니며, ASan에는 범용 `detect_uninitialized_reads` 옵션도 없습니다. [MemorySanitizer 문서](https://clang.llvm.org/docs/MemorySanitizer.html)를 참고하세요.

[LSan suppression](../tests/lsan_suppressions.txt)과 [TSan suppression](../tests/tsan_suppressions.txt)은 보고서마다 대조해서 검토해야 합니다. 여기에는 서드파티 패턴이 들어 있고, 과거의 libpqxx 항목도 포함되어 있습니다. 현재 PostgreSQL 연동은 libpq를 씁니다. NeoGraph 자체의 누수나 레이스를 suppression으로 가리면 안 됩니다. suppression은 라이브러리 프레임을 거친 할당까지 덮을 수 있으므로, suppression된 보고서가 소유권이 모두 올바르다는 증거는 아닙니다.
