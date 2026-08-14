<!-- neograph-i18n: source=docs/VALGRIND.md locale=ko source_sha256=38c9d7fc8073d2557d9b5533dfb33ec2de6867a55ba394bc26d6a5cf6b2e6215 -->
# 메모리 & 새니타이저 검사 (Valgrind / ASan / UBSan / 장시간 부하)

**Languages:** [English](VALGRIND.md) | [한국어](VALGRIND.ko.md) | [日本語](VALGRIND.ja.md) | [简体中文](VALGRIND.zh-CN.md)

예제 바이너리와 테스트 바이너리가 할당한 모든 메모리를 해제하는지
확인하는 기초 진실 검증. 로 실행. 기준은 API 키가 필요
없는 예제 표면 전반에 걸쳐 **누수 0, 오류 0**이다.

## 로컬에서 재현

```bash
mkdir build-debug && cd build-debug
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DNEOGRAPH_BUILD_TESTS=ON \
      -DNEOGRAPH_BUILD_EXAMPLES=ON \
      -DNEOGRAPH_BUILD_BENCHMARKS=OFF \
      -DNEOGRAPH_BUILD_POSTGRES=OFF ..
cmake --build . -j$(nproc)

# Sweep all no-API-key examples
for ex in example_custom_graph example_parallel_fanout example_send_command \
          example_intent_routing example_state_management example_all_features \
          example_plan_executor example_async_concurrent_runs \
          example_classifier_fanout example_subgraph example_checkpoint_hitl; do
    echo "=== $ex ==="
    valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=42 \
        ./$ex >/dev/null
done
```

## 마지막 검사

master(커밋 4b02dea, classifier-fanout 예제 이후) 기준 2026-04-29 실행.
Valgrind 3.22.0, GCC 13.3 Debug 빌드.

### 예제 — 11 / 11 깨끗

| 예제 | 할당 | 바이트 | 오류 |
|---:|---:|---:|---:|
| `example_all_features` | 5,097 / 5,097 | 1,080,618 | 0 |
| `example_async_concurrent_runs` | 683 / 683 | 226,919 | 0 |
| `example_checkpoint_hitl` | 1,973 / 1,973 | 524,478 | 0 |
| `example_classifier_fanout` | 1,696 / 1,696 | 419,024 | 0 |
| `example_custom_graph` | 799 / 799 | 231,767 | 0 |
| `example_intent_routing` | 3,960 / 3,960 | 916,910 | 0 |
| `example_parallel_fanout` | 1,330 / 1,330 | 364,867 | 0 |
| `example_plan_executor` | 3,616 / 3,616 | 823,613 | 0 |
| `example_send_command` | 3,279 / 3,279 | 747,161 | 0 |
| `example_state_management` | 2,540 / 2,540 | 640,311 | 0 |
| `example_subgraph` | 1,568 / 1,568 | 419,423 | 0 |
| **누적** | **26,541 / 26,541** | **6,395,091** | **0** |

모든 할당 해제, 잘못된 읽기 0, 잘못된 쓰기 0, 불일치 해제 0,
해제 후 사용 0.

### 테스트 — `*Smoke*:GraphCompiler*:GraphState*` 깨끗

| 모음 | 테스트 | 할당 | 바이트 | 오류 |
|---:|---:|---:|---:|---:|
| Smoke / GraphCompiler / GraphState (31개 테스트) | 31 / 31 통과 | 12,551 / 12,551 | 1,890,717 | 0 |

전체 `neograph_tests` 모음을 valgrind로 돌리면 ~30분 소요 — 위
하위 집합이 PR별 최소 기준; 전체 검사는 야간 CI 작업의 일부로 가능
(아직 배선되지 않음).

## 다루지 않는 것

- 네트워크를 사용하는 예제 (, , `example_react_agent` `example_mcp_*`
 `example_*_responses_*` ) — TLS/소켓 상호작용이 libssl /
  libcurl에서 valgrind 제외(suppression)로 가려야 하는 잡음을 발생.
  대신 모의 제공자(mock provider)로 엔진 경로 누수 검사 사용.
- Crawl4AI / Postgres 예제 — 외부 프로세스나 라이브러리 상태가
  누수 검사를 혼란시킴; 이 경로의 커버리지는 valgrind 대신 CI에서
  ASan으로 확보.
- Python 바인딩 () — Python 인터프리터가 종료 시 의도적인 `_neograph.so`
  "누수"(할당되었지만 해제되지 않은 모듈 상태)를 많이 가지고 있어
 `LSAN_OPTIONS=detect_leaks=0` valgrind 신호를 압도. 을 사용한 ASan이
  올바른 도구.

## ASan + UBSan + LSan 검사 — 11/11 예제 + 322 ctest 깨끗

새니타이저와 함께 컴파일:

```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
    -DNEOGRAPH_BUILD_TESTS=ON -DNEOGRAPH_BUILD_EXAMPLES=ON \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -O1" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j$(nproc)
```

-- The C compiler identification is GNU 13.3.0
-- The CXX compiler identification is GNU 13.3.0
-- Detecting C compiler ABI info
-- Detecting C compiler ABI info - done
-- Check for working C compiler: /usr/bin/cc - skipped
-- Detecting C compile features
-- Detecting C compile features - done
-- Detecting CXX compiler ABI info
-- Detecting CXX compiler ABI info - done
-- Check for working CXX compiler: /usr/bin/c++ - skipped
-- Detecting CXX compile features
-- Detecting CXX compile features - done
-- NeoGraph hardening: enabled (canaries always; CFI/FORTIFY/RELRO Linux-only).
-- Performing Test CMAKE_HAVE_LIBC_PTHREAD
-- Performing Test CMAKE_HAVE_LIBC_PTHREAD - Success
-- Found Threads: TRUE
-- Found OpenSSL: /usr/lib/x86_64-linux-gnu/libcrypto.so (found version "3.0.13")
-- Found SQLite3: /usr/lib/x86_64-linux-gnu/libsqlite3.so (found version "3.45.1")
-- Found PostgreSQL: /usr/lib/x86_64-linux-gnu/libpq.so (found version "16.14")
-- Found Python3: /usr/bin/python3.12 (found version "3.12.3") found components: Interpreter
-- Found CURL: /usr/lib/x86_64-linux-gnu/libcurl.so (found version "8.5.0")
-- Skipping example_openrouter_responses_tools_sse: needs GCC >= 14 (current: GNU 13.3.0)
-- Configuring done (20.6s)
-- Generating done (0.6s)
-- Build files have been written to: /root/Coding/NeoGraph/.claude/worktrees/i18n-ko-a/build-asan

와 로 예제 + 테스트 실행: `LSAN_OPTIONS=` `UBSAN_OPTIONS=`

```bash
export ASAN_OPTIONS="detect_leaks=1:halt_on_error=0"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0"
ctest --test-dir build-asan -E "BIG_|valgrind"   # 322/322 pass (2026-04-29)
```

Test project /root/Coding/NeoGraph/.claude/worktrees/i18n-ko-a/build-asan

master HEAD(커밋 6bd9632, 2026-04-29)에서의 마지막 검사:

| 표면 | 결과 |
|---|---|
| 11개 모의 예제 (custom_graph, send_command, intent_routing, state_management, all_features, subgraph, checkpoint_hitl, classifier_fanout, async_concurrent_runs, parallel_fanout, plan_executor) | ✓ exit=0, ASan/UBSan 오류 0 |
| `neograph_tests` ctest (새니타이저 아래 322개 테스트) | ✓ 322/322 통과 |

ASan이 오늘 추가된 재귀 방지 장치에서 거짓 양성 하나를 노출 —
원래 `thread_local int` 깊이 카운터가 중첩 그래프(같은 스레드에서
서브그래프 노드의 내부 엔진이 디스패치하면서 외부 카운터를 0이 아니게
만듦)에서 발화. 노드별  키로 전환하여 해결, 동일
노드가 자신의 기본 체인에 재진입할 때만 방지 장치가 발화. `const GraphNode*`

## 장시간 부하 스트레스 테스트 — 10,000회 그래프 실행, RSS Δ = 0 KB

실행당 깊이 3까지 재귀적으로 Send 팬아웃을 방출하는 Counter 노드로
master HEAD에서 실행 — 현실적인 부하 모양:

```cpp
// /tmp/stress_runs.cpp — see commit log
for (int i = 0; i < 10000; ++i) {
    engine->run(RunConfig{.thread_id = "t" + std::to_string(i),
                          .input    = {{"count", 0}}});
    if (i == 100)  rss_at_100  = read_rss_kb();
    if (i == 9999) rss_at_1000 = read_rss_kb();
}
```

10,000회 순차 그래프 실행, 각각 수 KB를 할당하고 해제, 마지막 9,900회
반복에 걸쳐 **주거 집합(resident-set) 증가 0 KB**. Linux glibc
할당자가 해제된 블록을 풀에 깨끗하게 반환 — 실행당 누수 경로가 존재하지
않음.

## CI 게이트 (sanitizer-test, tsan-test, fuzz-canary)

의 세 CI 작업 `.github/workflows/ci.yml`

```
10000 runs wall=0.68s  ops=14728/s  RSS@100=4608kB  RSS@9999=4608kB  Δ=0kB
PASS: RSS growth bounded
```

이 모든 PR과 master 푸시에서
다음을 강제:

### `sanitizer-test` — ASan + UBSan + LSan

| 단계 | 표면 |
|---|---|
| Test project /root/Coding/NeoGraph/.claude/worktrees/i18n-ko-a under `ctest -E "BIG_\|valgrind"` | 외부 표면(서비스 컨테이너를 통한 Postgres, MCP HTTP/stdio, libssl/libcurl ConnPool)을 포함한 모든 단위 테스트 | `-fsanitize=address,undefined`
| 같은 플래그 아래 11개 모의 예제 | 전체 엔진 경로 조율(orchestration) 커버리지 |
| `pytest bindings/python/tests/` + 로 ============================= test session starts ============================== `LD_PRELOAD=libasan.so` `detect_leaks=1` `__cxa_throw`
platform linux -- Python 3.12.3, pytest-9.0.3, pluggy-1.6.0
rootdir: /root/Coding/NeoGraph/.claude/worktrees/i18n-ko-a
configfile: pyproject.toml
plugins: anyio-4.13.0, langsmith-0.7.37
collected 76 items / 21 errors / 2 skipped

==================================== ERRORS ====================================
__________ ERROR collecting bindings/python/tests/test_async_tool.py ___________
bindings/python/tests/test_async_tool.py:38: in <module>
    class _SleepTool(ng.Tool):
                     ^^^^^^^
E   AttributeError: module 'neograph_engine' has no attribute 'Tool'
______ ERROR collecting bindings/python/tests/test_channel_write_mode.py _______
bindings/python/tests/test_channel_write_mode.py:26: in <module>
    class _SeedNode(ng.GraphNode):
                    ^^^^^^^^^^^^
E   AttributeError: module 'neograph_engine' has no attribute 'GraphNode'
_______ ERROR collecting bindings/python/tests/test_dynamic_interrupt.py _______
bindings/python/tests/test_dynamic_interrupt.py:29: in <module>
    class _ApprovalNode(ng.GraphNode):
                        ^^^^^^^^^^^^
E   AttributeError: module 'neograph_engine' has no attribute 'GraphNode'
_______ ERROR collecting bindings/python/tests/test_emit_token_helper.py _______
ImportError while importing test module '/root/Coding/NeoGraph/.claude/worktrees/i18n-ko-a/bindings/python/tests/test_emit_token_helper.py'.
Hint: make sure your test modules/packages have valid Python names.
Traceback:
/usr/lib/python3.12/importlib/__init__.py:90: in import_module
    return _bootstrap._gcd_import(name[level:], package, level)
           ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
bindings/python/tests/test_emit_token_helper.py:13: in <module>
    from neograph_engine.streaming import emit_token
E   ModuleNotFoundError: No module named 'neograph_engine.streaming'
_______ ERROR collecting bindings/python/tests/test_llm_call_contract.py _______
bindings/python/tests/test_llm_call_contract.py:53: in <module>
    class _CapturingProvider(ng.Provider):
                             ^^^^^^^^^^^
E   AttributeError: module 'neograph_engine' has no attribute 'Provider'
_______ ERROR collecting bindings/python/tests/test_max_steps_status.py ________
bindings/python/tests/test_max_steps_status.py:6: in <module>
    @ng.node("max_steps_status_noop")
     ^^^^^^^
E   AttributeError: module 'neograph_engine' has no attribute 'node'
__________ ERROR collecting bindings/python/tests/test_node_cache.py ___________
bindings/python/tests/test_node_cache.py:8: in <module>
    class _CountingNode(ng.GraphNode):
                        ^^^^^^^^^^^^
E   AttributeError: module 'neograph_engine' has no attribute 'GraphNode'
_________ ERROR collecting bindings/python/tests/test_openinference.py _________
ImportError while importing test module '/root/Coding/NeoGraph/.claude/worktrees/i18n-ko-a/bindings/python/tests/test_openinference.py'.
Hint: make sure your test modules/packages have valid Python names.
Traceback:
/usr/lib/python3.12/importlib/__init__.py:90: in import_module
    return _bootstrap._gcd_import(name[level:], package, level)
           ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
bindings/python/tests/test_openinference.py:16: in <module>
    from neograph_engine import _neograph as _native
E   ImportError: cannot import name '_neograph' from 'neograph_engine' (unknown location)
____________ ERROR collecting bindings/python/tests/test_parity.py _____________
ImportError while importing test module '/root/Coding/NeoGraph/.claude/worktrees/i18n-ko-a/bindings/python/tests/test_parity.py'.
Hint: make sure your test modules/packages have valid Python names.
Traceback:
/usr/lib/python3.12/importlib/__init__.py:90: in import_module
    return _bootstrap._gcd_import(name[level:], package, level)
           ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
bindings/python/tests/test_parity.py:28: in <module>
    import neograph_engine.llm as nglm
E   ModuleNotFoundError: No module named 'neograph_engine.llm'
_______ ERROR collecting bindings/python/tests/test_provider_lifetime.py _______
bindings/python/tests/test_provider_lifetime.py:24: in <module>
    class _Provider(ng.Provider):
                    ^^^^^^^^^^^
E   AttributeError: module 'neograph_engine' has no attribute 'Provider'
_____ ERROR collecting bindings/python/tests/test_python_store_backends.py _____
bindings/python/tests/test_python_store_backends.py:13: in <module>
    class DictStore(ng.Store):
                    ^^^^^^^^
E   AttributeError: module 'neograph_engine' has no attribute 'Store'
_________ ERROR collecting bindings/python/tests/test_state_history.py _________
bindings/python/tests/test_state_history.py:7: in <module>
    class _PassthroughNode(ng.GraphNode):
                           ^^^^^^^^^^^^
E   AttributeError: module 'neograph_engine' has no attribute 'GraphNode'
__________ ERROR collecting bindings/python/tests/test_state_view.py ___________
ImportError while importing test module '/root/Coding/NeoGraph/.claude/worktrees/i18n-ko-a/bindings/python/tests/test_state_view.py'.
Hint: make sure your test modules/packages have valid Python names.
Traceback:
/usr/lib/python3.12/importlib/__init__.py:90: in import_module
    return _bootstrap._gcd_import(name[level:], package, level)
           ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
bindings/python/tests/test_state_view.py:15: in <module>
    from neograph_engine import StateView
E   ImportError: cannot import name 'StateView' from 'neograph_engine' (unknown location)
___________ ERROR collecting bindings/python/tests/test_streaming.py ___________
ImportError while importing test module '/root/Coding/NeoGraph/.claude/worktrees/i18n-ko-a/bindings/python/tests/test_streaming.py'.
Hint: make sure your test modules/packages have valid Python names.
Traceback:
/usr/lib/python3.12/importlib/__init__.py:90: in import_module
    return _bootstrap._gcd_import(name[level:], package, level)
           ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
bindings/python/tests/test_streaming.py:6: in <module>
    from neograph_engine.streaming import message_stream
E   ModuleNotFoundError: No module named 'neograph_engine.streaming'
___________ ERROR collecting bindings/python/tests/test_tool_gate.py ___________
bindings/python/tests/test_tool_gate.py:18: in <module>
    class _SpyTool(ng.Tool):
                   ^^^^^^^
E   AttributeError: module 'neograph_engine' has no attribute 'Tool'
_____________ ERROR collecting bindings/python/tests/test_tools.py _____________
bindings/python/tests/test_tools.py:25: in <module>
    class _MockToolEmittingNode(neograph.GraphNode):
                                ^^^^^^^^^^^^^^^^^^
E   AttributeError: module 'neograph_engine' has no attribute 'GraphNode'
___ ERROR collecting bindings/python/tests/test_topology_container_types.py ____
bindings/python/tests/test_topology_container_types.py:71: in <module>
    ("edges", {"direct": {"from": ng.START_NODE, "to": ng.END_NODE}}),
                                  ^^^^^^^^^^^^^
E   AttributeError: module 'neograph_engine' has no attribute 'START_NODE'
____________ ERROR collecting bindings/python/tests/test_tracing.py ____________
ImportError while importing test module '/root/Coding/NeoGraph/.claude/worktrees/i18n-ko-a/bindings/python/tests/test_tracing.py'.
Hint: make sure your test modules/packages have valid Python names.
Traceback:
/usr/lib/python3.12/importlib/__init__.py:90: in import_module
    return _bootstrap._gcd_import(name[level:], package, level)
           ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
bindings/python/tests/test_tracing.py:28: in <module>
    from neograph_engine.tracing import otel_tracer
E   ModuleNotFoundError: No module named 'neograph_engine.tracing'
______ ERROR collecting bindings/python/tests/test_update_state_shapes.py ______
bindings/python/tests/test_update_state_shapes.py:28: in <module>
    class _PassthroughNode(neograph.GraphNode):
                           ^^^^^^^^^^^^^^^^^^
E   AttributeError: module 'neograph_engine' has no attribute 'GraphNode'
_______ ERROR collecting bindings/python/tests/test_usage_accounting.py ________
bindings/python/tests/test_usage_accounting.py:12: in <module>
    class _UsageProvider(ng.Provider):
                         ^^^^^^^^^^^
E   AttributeError: module 'neograph_engine' has no attribute 'Provider'
__________ ERROR collecting bindings/python/tests/test_usage_live.py ___________
ImportError while importing test module '/root/Coding/NeoGraph/.claude/worktrees/i18n-ko-a/bindings/python/tests/test_usage_live.py'.
Hint: make sure your test modules/packages have valid Python names.
Traceback:
/usr/lib/python3.12/importlib/__init__.py:90: in import_module
    return _bootstrap._gcd_import(name[level:], package, level)
           ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
bindings/python/tests/test_usage_live.py:29: in <module>
    from neograph_engine.llm import OpenAIProvider, SchemaProvider
E   ModuleNotFoundError: No module named 'neograph_engine.llm'
=========================== short test summary info ============================
ERROR bindings/python/tests/test_async_tool.py - AttributeError: module 'neog...
ERROR bindings/python/tests/test_channel_write_mode.py - AttributeError: modu...
ERROR bindings/python/tests/test_dynamic_interrupt.py - AttributeError: modul...
ERROR bindings/python/tests/test_emit_token_helper.py
ERROR bindings/python/tests/test_llm_call_contract.py - AttributeError: modul...
ERROR bindings/python/tests/test_max_steps_status.py - AttributeError: module...
ERROR bindings/python/tests/test_node_cache.py - AttributeError: module 'neog...
ERROR bindings/python/tests/test_openinference.py
ERROR bindings/python/tests/test_parity.py
ERROR bindings/python/tests/test_provider_lifetime.py - AttributeError: modul...
ERROR bindings/python/tests/test_python_store_backends.py - AttributeError: m...
ERROR bindings/python/tests/test_state_history.py - AttributeError: module 'n...
ERROR bindings/python/tests/test_state_view.py
ERROR bindings/python/tests/test_streaming.py
ERROR bindings/python/tests/test_tool_gate.py - AttributeError: module 'neogr...
ERROR bindings/python/tests/test_tools.py - AttributeError: module 'neograph_...
ERROR bindings/python/tests/test_topology_container_types.py - AttributeError...
ERROR bindings/python/tests/test_tracing.py
ERROR bindings/python/tests/test_update_state_shapes.py - AttributeError: mod...
ERROR bindings/python/tests/test_usage_accounting.py - AttributeError: module...
ERROR bindings/python/tests/test_usage_live.py
!!!!!!!!!!!!!!!!!!! Interrupted: 21 errors during collection !!!!!!!!!!!!!!!!!!!
======================== 2 skipped, 21 errors in 1.12s ========================= | 누수 감지가 활성화된 46/48 Python 테스트(pybind를 가로지르는 Python 예외를 전파하는 2개 제외 — 알려진 ASan  가로채기(interception) 제한, NeoGraph 버그 아님) |

### `tsan-test` — 엔진 동시성 경로의 경쟁 감지

| 단계 | 커버리지 |
|---|---|
| Test project /root/Coding/NeoGraph/.claude/worktrees/i18n-ko-a under `setarch x86_64 -R ctest -E "BIG_\|valgrind"` | 새 (200개 동시 `-fsanitize=thread` × 3-way Send 팬아웃 — 작업자 풀, 스케줄러, parallel_group, CheckpointStore 동시성 경로의 데이터 경쟁 포착)를 포함한 모든 344개 단위 테스트 | `ConcurrentStress.TwoHundredOverlappingRunsAllSucceed` `run_async`
| TSan 아래 5개 팬아웃 / async 예제 | `example_classifier_fanout` + `parallel_fanout` + `send_command` + `plan_executor` + `async_concurrent_runs` |

vnic is waydroid0 래핑은 를 해제(Ubuntu 24.04+ `setarch x86_64 -R` `ADDR_NO_RANDOMIZE`
커널 `mmap_rnd_bits` 기본값이 TSan  FATAL을
유발); 플래그가 를 통해 상속되므로 모든 테스트 자식도 TSan 친화적 ` FATAL); the flag inherits through `
주소 배치를 얻는다.

TSan + ASan은 링크 시 상호 배타적이므로 와 별도 작업. `sanitizer-test`

### `fuzz-canary` — `GraphCompiler::compile`에 libFuzzer

| 단계 | 커버리지 |
|---|---|
| `fuzz_graph_compile` 60초 wall () | `-max_total_time=60` 아래 시드 말뭉치를 변이시켜 바이트를 `tests/fuzz/corpus/graph_compile/` → 로 공급. 파서 UB, 처리되지 않은 예외, 힙 버퍼 오버플로 회귀 포착. master HEAD 첫 실행에서 충돌 없이 194만 회 반복. | `neograph::json::parse` `GraphCompiler::compile`

Clang의 로 빌드되어 모든 충돌이 `-fsanitize=fuzzer,address,undefined`
같은 추적에서 ASan/UBSan 진단을 표시.

## Release 빌드 강화

Release / RelWithDebInfo / MinSizeRel 빌드는 기본적으로 심층 방어
플래그를 활성화(): `NEOGRAPH_ENABLE_HARDENING=ON`

| 플래그 | 잡아내는 것 |
|---|---|
| `-D_GLIBCXX_ASSERTIONS` | std::vector 범위 초과(OOB), `end()` 역참조, 반복자 무효화, 초기화되지 않은 `std::optional` 접근 — 조용한 UB 대신 진단과 함께 중단. Debug + Release에서 활성. |
| `-fstack-protector-strong` | 반환 주소를 덮어쓸 버퍼 오버플로 — `ret` 전에 카나리(canary) 검사 발화. |
| `-fcf-protection=full` | 간접 호출/점프 대상에 태그를 달아 제어 흐름 무결성(control-flow integrity) 확보. ROP 스타일 공격이 호출 지점에서 실패. CET-IBT가 있는 amd64에서 저렴. |
| `-D_FORTIFY_SOURCE=2` | libc 문자열/메모리 루틴의 인라인 검사. Release 전용(≥ 필요). | `-O1`
| `-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack` | 읽기 전용 재배치, 즉시 바인딩(늦은 PLT 쓰기 없음), no-exec 스택 — RELRO 기준. |

master HEAD에서 로 측정한 성능 영향: `bench_neograph`

|  | seq µs | par µs |
|---|---:|---:|
| 기준 Release | 5.1 | 275.2 |
| 강화된 Release | 5.1 | 275.6 |

측정 잡음 내에서 **오버헤드 0 %**. 플래그가 작업을 링커(재배치, PLT)와
함수별 8바이트 카나리 적재+비교로 이동 — NeoGraph 엔진 경로의 µs
규모에서는 둘 다 보이지 않음.

새니타이저 빌드(ASan/TSan/UBSan)에서는 자동으로 비활성화 — 새니타이저
자체 검사와 중복. MSVC에서도 비활성화(다른 강화 기본 요소 사용 —
등, 여기서 다루지 않음). `/GS`

## 탐색했지만 실행 불가능한 새니타이저 조합

**MemorySanitizer**(초기화되지 않은 읽기 감지): 연결된 모든 C/C++
라이브러리 — libstdc++, libssl, libcurl, libpqxx 포함 — 가 MSan
계측(instrumented)되어야 하며, 그렇지 않으면 호출 시 신호를 압도하는
거짓 양성이 발생. Ubuntu 24.04의 Clang 사전 빌드 는 MSan `libc++`
변종을 제공하지 않으며, 표준 라이브러리 + 모든 전이 의존성을 다시
빌드하는 것은 비현실적. ASan+UBSan+TSan 3종 세트는 이미
힙 할당 상태로 탈출하는 초기화되지 않은 읽기를 포착(일부 패스에서
 의미를 가진 ASan 아래 힙이 할당 시
독극물 처리(poisoning)됨). 건너뜀. `detect_uninitialized_reads=1`

## 제외 목록(Suppressions)

| 파일 | 다루는 것 |
|---|---|
| [](../tests/lsan_suppressions.txt) | libssl / libcurl / libpq / libpqxx / libstdc++ ABI / glibc TLS / CPython 인터프리터 / pybind11 타입 초기화 / pydantic-core. 서드파티 전용 — NeoGraph 심볼 추가는 실제 버그, 대신 누수를 수정. | `tests/lsan_suppressions.txt`
| [](../tests/tsan_suppressions.txt) | asio 리액터 & 소켓 서비스(epoll happens-before), yyjson SIMD 읽기, OpenSSL CRYPTO_THREAD_run_once. 라이브러리 내부의 무해한 경쟁. | `tests/tsan_suppressions.txt`

## 동시성 스트레스 테스트

가 표준 ctest 모음의 일부로 실행 `tests/test_concurrent_stress.cpp`
(Debug와 ASan 모두에서):

- **TwoHundredOverlappingRunsAllSucceed** — 단일 io_context 위에서 `engine->run_async()`
  200개  호출이 겹쳐서 실행, 각각 3-way Send
  팬아웃. parallel-group + 대기 쓰기(pending-writes) 장치가 ASan 아래
 `{0, 1, 4}` 경쟁이 없고 200개 실행 모두 예상된  작업자 출력을
  생산하는지 검증.
- **RssBoundedOverHundredsOfConcurrentRuns** — 200개 실행 5회 연속
  (총 1,000회 동시) 실행, RSS Δ ≤ 10 MB 임계값. ASan 아래에서는 건너뜀
  (새니타이저의 그림자 메모리(shadow-memory) 증가가 신호를 압도).

Debug 빌드 실행 결과 1,000회 동시 실행에서 RSS Δ=128 kB —
지속적인 동시 부하 아래 엔진 측 메모리 프로필이 평탄.
