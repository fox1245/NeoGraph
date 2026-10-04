<!-- neograph-i18n: source=docs/PROGRAM_CANONICAL_JSON_OPTIMIZATION.md locale=ko source_sha256=21e00a791dfa14a8b9748d723c10121eb2c8acdbd477978e058c932110f2d41d -->
# AgentX 분석을 활용한 canonical JSON 최적화 — 2026-09-07

**Languages:** [English](PROGRAM_CANONICAL_JSON_OPTIMIZATION.md) | [한국어](PROGRAM_CANONICAL_JSON_OPTIMIZATION.ko.md) | [日本語](PROGRAM_CANONICAL_JSON_OPTIMIZATION.ja.md) | [简体中文](PROGRAM_CANONICAL_JSON_OPTIMIZATION.zh-CN.md)

측정한 64 KiB Program workload에서 string scan/copy를 묶어 처리하자 이미 최적화된 command-head 구현(`ef56ff89`)에 비해 latency가 **memory store 약 54%, SQLite 50%, PostgreSQL 26%** 줄었다. 개선은 native canonical JSON 처리에서 나온다. 이 실험은 JIT backend를 평가하지 않는다.

## 변경 선택의 근거

owner-local AgentX radare2 MCP peer(connector version 1.8.6)가 content 신원으로 정확히 고정한 Release ELF를 검사했다. artifact 열기, symbol lookup, read-only disassembly만 사용했다. function disassembly는 처음에 analysis metadata가 없다고 보고했다. read-only service는 `analyze`를 제공하지 않는다. 주소 기반 `disassemble`은 성공했으며 `nm`의 ELF symbol 주소/크기로 발견 사항의 범위를 제한했다.

그 후 Linux software CPU-clock sampler는 새로 생성한 benchmark process와 그 thread만 실행하며 kernel/hypervisor sample을 제외했다. 주기는 1 ms, buffer는 per-CPU inherited buffer를 사용했다. 이는 setup, 호출자 측 verification, teardown을 포함한 전체 process의 leaf-IP profile이다. wall-time 분할이나 inclusive call graph가 아니다. 해석하지 못한 shared-library symbol은 명시적으로 미해결 상태로 남긴다.

- 작은 memory input: sample 4,196개, loss 0.
- 64 KiB memory input: sample 5,155개, loss 0. `append_escaped`가 sample 25.68%, UTF-8 sequence validation이 17.58%, escaped-size 계산이 11.68%로 합계 약 55%를 차지했다.
- AgentX disassembly는 오래된 loop가 `0x66a349`에서 UTF-8 validation을 호출하고 반환된 sequence 길이만큼 진행하는 모습을 보였다. 이 길이는 모든 ASCII byte에 대해 1이었다. source 검사에서도 byte별 escape-size loop와 output loop가 별도로 있음을 확인했다.

이 증거를 바탕으로 추측에 기반한 VM 변경보다 string 처리를 먼저 선택했다. 아래의 별도 unprofiled 비교가 측정된 개선을 결정한다. sampling API는 [Linux perf_event_open](https://man7.org/linux/man-pages/man2/perf_event_open.2.html)을 참조한다.

## 변경과 불변 조건

`src/core/canonical_json.cpp`는 이제 8-byte word 전체에서 ASCII 또는 JSON escape byte를 검사한 다음 나머지를 scalar 방식으로 처리한다. 정렬되지 않은 word는 `memcpy`로 읽고 repeated-byte mask는 byte order에 의존하지 않는다. UTF-8 validation은 ASCII 구간을 한 번에 소비하며 기존 non-ASCII 유효성 규칙을 유지한다. string 출력은 검증 후 escape가 필요 없는 span을 한 연산으로 append한다.

size 계산은 raw-size 하한에서 시작해 escape 확장량을 더한다. newline 같은 짧은 escape를 포함해 모든 control byte에 적용하던 보수적인 6-byte 계산을 유지한다. 16 MiB 한도, 유효하지 않은 UTF-8 거부, escape byte, 정렬된 key, number encoding, owned-value 동작, content 신원은 바뀌지 않는다. 새 dependency, architecture별 compiler option, public API, SQL query, persistence setting을 도입하지 않았다.

변경 후 ELF도 같은 AgentX peer로 검사했다. `0x668710`의 `unescaped_prefix`에는 `mov rax, qword [r8 + rcx]`가 있으며 `rcx`를 8씩 증가시킨다. 컴파일된 code에 의도한 wide-load loop가 있음을 확인한다.

## 정확성 검증

- 새 canonical JSON test 6개는 word 경계 주변의 모든 ASCII byte, Unicode/escaping 혼합, 결정적인 Unicode corpus 4,096개, ASCII prefix 뒤의 유효하지 않은 UTF-8, 짧거나 정렬되지 않은 view, 보수적인 materialized-size 경계를 다룬다.
- 같은 test 6개가 AddressSanitizer와 UndefinedBehaviorSanitizer에서 통과했다. leak detection을 포함했고 suppression은 없었다.
- 집중 Core test 211개가 두 번의 실행에 걸쳐 통과했다. 처음 180개, 이후 전용 backend를 활성화한 PostgreSQL checkpoint case 31개 전부다.
- 전체 Program suite: 680개 통과, 적용되지 않는 memory-store process-restart case 2개 skip. golden canonical identity와 SQLite/PostgreSQL recovery, lineage, replacement, budget 검사를 포함한다.
- 모든 microbenchmark output이 독립 scalar escape oracle과 일치했고 모든 Program invocation이 예상 counter와 전체 payload를 유지했다.
- 검증 환경은 WSL GCC 13.3이었다. Windows, ARM, sanitizer로 전체 engine을 검증했다는 주장은 하지 않는다.

## String 전용 benchmark

case마다 새 process pair 7개, warmup 20회, process마다 serialization 측정 500회를 사용하고 AB/BA와 case 순서를 번갈아 바꿨다. input에는 ASCII, 한국어/emoji, escape가 있는 mixed text, control/quote/backslash가 많은 case가 있다. 준비와 전체 output verification은 timed call 밖에서 수행한다. 1 µs 미만의 작은 case는 timer 해상도에 가까우므로 정밀한 regression gate로 사용해서는 안 된다.

| Input | 변경 전 median (µs) | 변경 후 median (µs) | Paired 감소율 median |
| --- | --- | --- | --- |
| ascii-8 | 0.060 | 0.050 | +16.67% |
| ascii-32 | 0.190 | 0.090 | +50.28% |
| unicode-32 | 0.170 | 0.110 | +35.29% |
| mixed-32 | 0.190 | 0.130 | +31.58% |
| escaped-32 | 0.241 | 0.201 | +12.61% |
| ascii-256 | 1.031 | 0.160 | +84.34% |
| unicode-256 | 0.892 | 0.361 | +59.60% |
| mixed-256 | 1.002 | 0.501 | +50.50% |
| escaped-256 | 1.212 | 0.932 | +22.73% |
| ascii-65536 | 298.398 | 87.008 | +70.91% |
| unicode-65536 | 263.603 | 136.770 | +48.33% |
| mixed-65536 | 295.183 | 169.748 | +42.37% |
| escaped-65536 | 492.026 | 416.920 | +14.27% |

## Program runtime 비교

`compare_program_costs.py`를 사용해 case마다 새 process pair 5개, warmup 3회, process마다 invocation 측정 12회를 수행했다. 두 binary를 고정했으며 변경 전 binary는 앞선 command-head 최적화다. 같은 Ryzen 7 5800X, WSL ext4, repository hardening을 적용한 Release `-O3 -DNDEBUG`, Runtime scheduler thread 하나, native-WSL PostgreSQL 16.15 구성을 사용했다. 각 database case는 새 database로 시작한다. 이 timing pair를 측정하는 동안 build, profiler, 다른 test job은 실행하지 않았다.

| Case | 변경 전 median (ms) | 변경 후 median (ms) | Paired 감소율 median | Paired ratio 범위 |
| --- | --- | --- | --- | --- |
| direct-0 | 0.0078 | 0.0078 | +0.83% | 0.7911–1.2164 |
| memory-cpp-0 | 2.1190 | 1.9813 | +9.03% | 0.9054–0.9640 |
| memory-javascript-0-commands-1 | 3.4078 | 3.2520 | +4.61% | 0.9378–0.9557 |
| memory-javascript-65536-commands-1 | 32.0098 | 14.9131 | +54.19% | 0.4524–0.4727 |
| memory-javascript-0-commands-16 | 32.9070 | 31.2267 | +5.71% | 0.8915–0.9682 |
| sqlite-javascript-0-commands-1 | 19.9449 | 19.9057 | +0.47% | 0.9158–1.0023 |
| sqlite-javascript-65536-commands-1 | 127.6179 | 64.3529 | +50.19% | 0.4935–0.5101 |
| sqlite-javascript-0-commands-16 | 272.5562 | 259.2726 | +4.87% | 0.9361–0.9640 |
| postgres-javascript-0-commands-1 | 81.3888 | 80.5189 | +1.71% | 0.9621–1.0215 |
| postgres-javascript-65536-commands-1 | 246.5232 | 184.0142 | +25.66% | 0.7035–0.7682 |
| postgres-javascript-0-commands-16 | 973.4043 | 945.9677 | +2.14% | 0.9462–1.0037 |
| postgres-cpp-0 | 51.8192 | 50.9214 | +1.73% | 0.8937–0.9983 |

percentage는 paired ratio를 요약하므로 독립적인 두 median 열의 비율과 같을 필요가 없다. 아주 작은 direct-Core timing과 작은 database case는 상대적 분산이 크다. 큰 input의 개선은 pair 5개 모두에서 일관되었다. 이는 synthetic engine overhead이며 chatbot/model 응답 시간이나 모든 workload의 속도 향상이 아니다.

## 남은 부분

후속 64 KiB CPU profile은 sample 2,317개를 수집했고 loss는 0이었다. SHA-256 identity 계산이 sample 약 35.17%, unescaped-span scanning이 10.96%, UTF-8 validation이 1.90%를 차지했다. hash 비율이 커진 것은 전체 작업이 줄어든 결과이지 hashing이 느려졌다는 증거가 아니다. 미해결 비용을 전부 allocation이나 copying에 배정하기 전에 shared-library symbol의 attribution을 개선해야 한다.

## 재현과 범위

`ef56ff89`와 변경 revision에서 같은 Release option으로 `bench_canonical_json`과 `bench_program_cost`를 빌드한다. 새 microbenchmark source는 변경 전 library를 바꾸지 않고 그 library에 연결해 빌드할 수 있다. micro command 예:

```sh
bench_canonical_json unicode 65536 500
```

end-to-end case에는 [paired Program 비교](PROGRAM_COMMAND_HEAD_OPTIMIZATION.md)를 사용한다. raw RPC request/response, tool schema, ELF/source hash, CPU sample/map, helper source, micro/runtime result, test log는 `artifacts/program-canonical-agentx-20260907/` 아래 local 보존한다. 완전한 read-only input ELF는 할당된 AgentX artifact root의 digest 기반 directory에 남아 있으며 evidence에 credential은 포함하지 않는다.

owner가 허가한 host에서 AgentX analysis peer를 직접 사용했다. AgentX model WorkOrder를 시작하거나 새 ToolBindingSet을 게시하거나 AgentX Analysis Graph에 record를 import하지 않았다. 그 workflow/provenance guarantee는 주장하지 않는다. AgentX service configuration과 다른 application data는 바꾸지 않았다.
