<!-- neograph-i18n: source=docs/VALGRIND.md locale=ko source_sha256=77ef4aac4dcc1d2275fa67596cf241a8e86f7437291204b5eaab2f91233d1308 -->
# 메모리와 sanitizer 검사

**Languages:** [English](VALGRIND.md) | [한국어](VALGRIND.ko.md) | [日本語](VALGRIND.ja.md) | [简体中文](VALGRIND.zh-CN.md)

## 범위와 현재 준비 조건

Memcheck는 잘못된 메모리 접근과 allocation leak, ASan은 address 오류, UBSan은 지원 범위의 undefined behavior, TSan은 data race를 검사한다. 하나의 통과가 다른 도구의 속성을 증명하지 않는다. 아래 보고는 날짜가 있는 역사적 결과이며 새 typed-provider 검증이 아니다.

현재 Core build는 LLM node와 PostgreSQL을 꺼도 외부 `SchemaProvider::runtime`이 필요하다. `SDK_PREFIX`를 일치하는 installed SDK로 지정하거나 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`와 SDK 개발 의존성을 제공한다. SDK 내부 sanitizer 검사는 SDK도 일치하는 계측으로 빌드한다(`SP_SANITIZE=address` 또는 `thread`). 기록된 interface-3 runtime/archive 검증은 Linux/POSIX 범위이며 interface 4를 검증하지 않는다. 이 Linux 명령은 Windows, macOS, WASM 검증이 아니다.

NeoGraph는 CMake 3.20+가 필요하다. 명시적 SDK source directory, installed package, 기본 fetch되는 revision-pinned 공개 archive 순으로 선택한다. installed SDK를 offline으로 쓰면 `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`를 설정한다. source/fetched build는 NeoGraph의 Asio/yyjson을 쓰지만 system libcurl/OpenSSL 개발 파일은 여전히 필요하다.

## 현재 Memcheck 시나리오 실행

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug \
  -DNEOGRAPH_BUILD_TESTS=ON -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_POSTGRES=OFF -DCMAKE_PREFIX_PATH="$SDK_PREFIX"
cmake --build build-debug --target example_custom_graph -j
valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all \
  --error-exitcode=42 ./build-debug/example_custom_graph
```

재빌드 artifact에서 관련 no-network 예제와 provider 수명/취소 시나리오를 실행한다. network와 Python 경로도 좁힌 진단과 검토된 third-party suppression이 필요하며 검사 면제 대상이 아니다. mock-provider 통과는 SDK transport와 실제 native replay custody를 검증하지 않는다.

## 역사적 Memcheck sweep, 2026-04-29

기록된 sweep은 master `4b02dea`, GCC 13.3 Debug, Valgrind 3.22.0을 썼다. key 없는 예제 11개에서 invalid access, mismatched free, leak이 0이었다.

| Example at 4b02dea | Allocations / frees | Allocated bytes | Errors |
|---|---:|---:|---:|
| example_all_features | 5097 / 5097 | 1080618 | 0 |
| example_async_concurrent_runs | 683 / 683 | 226919 | 0 |
| example_checkpoint_hitl | 1973 / 1973 | 524478 | 0 |
| example_classifier_fanout | 1696 / 1696 | 419024 | 0 |
| example_custom_graph | 799 / 799 | 231767 | 0 |
| example_intent_routing | 3960 / 3960 | 916910 | 0 |
| example_parallel_fanout | 1330 / 1330 | 364867 | 0 |
| example_plan_executor | 3616 / 3616 | 823613 | 0 |
| example_send_command | 3279 / 3279 | 747161 | 0 |
| example_state_management | 2540 / 2540 | 640311 | 0 |
| example_subgraph | 1568 / 1568 | 419423 | 0 |
| Total | 26541 / 26541 | 6395091 | 0 |

`Smoke`/`GraphCompiler`/`GraphState` subset은 31/31 test, allocation/free 12551, 1890717 byte, 오류 0을 기록했다. 수치는 그 revision의 것이며 전체 suite는 이 Memcheck 결과의 범위가 아니다.

## 현재 ASan과 UBSan 절차

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

Python은 extension import 전에 일치하는 ASan runtime을 preload하고 누수 감지를 일괄 해제하지 말고 `tests/lsan_suppressions.txt`를 쓴다. 실제 interpreter 설정은 CI workflow에 있다. 제외 항목을 결과와 함께 밝히고 옛 Python test 수를 새 실행 결과로 복사하지 않는다.

## 역사적 sanitizer와 soak 근거

2026-04-29 master `6bd9632`의 기록된 sweep은 mock 예제 11개와 CTest 322/322 case에서 ASan/UBSan 보고 오류가 0이었다. 옛 default-chain recursion guard 수정은 그 역사에 속한다. 현재 GraphNode는 단일 `run(NodeInput)` override를 쓴다.

과거 Counter/Send 10000회 soak는 `wall=0.68s`, `ops=14728/s`, run 100·9999에서 RSS 4608 KB(차이 0 KB)를 기록했다. 별도 debug 동시 실행 1000회는 차이 128 KB였다. 관측 RSS가 제한되었다고 미실행 코드의 leak 경로 부재를 증명하지 않는다. allocator 재사용은 짧은 구간의 증가를 숨길 수 있다.

과거 hardening 측정은 baseline seq 5.1 µs/par 275.2 µs, hardened seq 5.1 µs/par 275.6 µs였다. 그 측정에서 차이를 감지하지 못했을 뿐 모든 작업의 비용이 0이라는 뜻은 아니다.

## CI와 hardening 계약

[CI workflow](../.github/workflows/ci.yml)는 `sanitizer-test`, `tsan-test`, `fuzz-canary`를 정의한다. C++ test/example와 Python ASan/LSan, TSan 동시성 경로, libFuzzer compiler input이 대상이다. workflow 정의는 의도된 gate이며 현재 의존성 설정의 통과 증거는 아니다.

TSan과 ASan은 별도 build가 필요하다. Linux `setarch x86_64 -R`은 `ADDR_NO_RANDOMIZE`를 설정해 주소 randomization을 끈다. 권한과 kernel layout이 여전히 TSan을 막을 수 있다. address sanitization은 race freedom을 증명하지 않는다.

`NEOGRAPH_ENABLE_HARDENING=ON`은 compiler/platform에 맞는 assertion, stack-protector, FORTIFY, linker flag를 켠다. Linux x86은 설정된 `-fcf-protection`을 지원하지만 다른 platform으로 그 주장을 확대할 수 없다. CMake는 sanitizer flag가 있거나 MSVC이면 해당 flag를 건너뛴다. CET tagging이 모든 control-flow 공격 실패를 보장하지 않는다.

## 미초기화 읽기와 suppression

MemorySanitizer는 C++ 표준 라이브러리와 runtime library를 포함한 계측 dependency가 필요하다. ASan/UBSan/TSan은 일반적인 미초기화 읽기 검사를 대체하지 않으며 ASan에 일반적인 `detect_uninitialized_reads` 옵션은 없다. [MemorySanitizer 문서](https://clang.llvm.org/docs/MemorySanitizer.html)를 참고한다.

각 보고에 [LSan suppression](../tests/lsan_suppressions.txt)과 [TSan suppression](../tests/tsan_suppressions.txt)을 검토한다. third-party pattern에 역사적 libpqxx 항목도 있지만 현재 PostgreSQL은 libpq를 쓴다. NeoGraph 소유 leak/race를 숨기지 않는다. library frame을 통과한 allocation도 suppression에 포함될 수 있으므로 suppressed 보고가 올바른 전체 소유권의 증거는 아니다.
