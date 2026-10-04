<!-- neograph-i18n: source=docs/PROGRAM_SHA256_OPTIMIZATION.md locale=ko source_sha256=7e4fa7e81f456ac2c8027d75d34ccf3f85a9cbc18acbd7cef91f1282558b981a -->
# SHA-256 CPU 가속 — 2026-09-07

**Languages:** [English](PROGRAM_SHA256_OPTIMIZATION.md) | [한국어](PROGRAM_SHA256_OPTIMIZATION.ko.md) | [日本語](PROGRAM_SHA256_OPTIMIZATION.ja.md) | [简体中文](PROGRAM_SHA256_OPTIMIZATION.zh-CN.md)

런타임 조건으로 선택하는 x86 SHA 명령어로 측정된 64 KiB identity-hash 비용이 약 **6.2배** 줄었다. 앞선 canonical JSON 최적화(`779be451`)와 짝지어 비교한 큰 input case에서 Program latency는 **memory store 약 28%, SQLite 20%, PostgreSQL 8%** 개선되었다. 작은 database case에서는 유의미한 개선을 확립하지 못했다.

## 구현과 호환성

원래 scalar SHA-256 compression과 padding을 `canonical_json.cpp`에서 내부 `sha256.cpp` module로 옮겼다. 별도의 inline되지 않는 x86-64 함수가 SHA 명령어로 round와 message schedule을 수행한다. register layout은 출처를 명시한 public-domain [SHA-Intrinsics reference](https://github.com/noloader/SHA-Intrinsics/blob/d03795497f3e4576083fc2cd8fe0b924f24d0bb2/sha256-x86.c)를 따른다. 구현은 회전하는 4-vector schedule을 사용한다. 원래 scalar 구현도 계속 사용할 수 있다.

automatic dispatch는 가속 함수를 선택하기 전에 CPUID basic leaf 사용 가능 여부, SSSE3, SSE4.1, SHA 지원을 확인한다. GCC/Clang의 명령어 활성화는 그 함수에만 적용하며 전역 architecture flag를 사용하지 않는다. MSVC는 같은 runtime 검사 아래 같은 intrinsic을 컴파일한다. 다른 architecture와 `NEOGRAPH_ENABLE_SHA256_ACCELERATION=OFF` build는 portable 경로를 선택한다. 추가 library dependency는 도입하지 않았다.

private backend selector를 통해 process-wide 동작을 바꾸지 않고 portable/hardware correctness test를 강제로 선택할 수 있다. 설치되는 SDK의 일부는 아니다. hardware를 명시적으로 선택했는데 사용할 수 없으면 예외를 던진다. digest state는 호출별로 local이며 CPU 사용 가능 여부와 선택된 함수 포인터만 cache한다.

identity framing은 그대로다. `preamble || NUL || domain || NUL || decimal(payload byte length) || NUL || payload`. padding, digest의 256 bit 전체, 소문자 hexadecimal, `sha256:` prefix를 보존한다. 기존 input-buffer 생성도 그대로 유지한다. 이 변경은 streaming, hash caching, 더 약한 checksum, 새 storage format, 다른 authority/budget 규칙을 도입하지 않는다.

명령어 의미는 [Intel SHA extensions](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sha-extensions.html)를 참조한다. 실행한 Ryzen은 필요한 x86 instruction bit를 노출한다. 이 dispatch는 Intel vendor 전용이 아니다.

## 검증

- 빈 input, `abc`, 56-byte vector, `a` 100만 byte를 포함한 표준 SHA-256 known answer.
- binary payload, padding 경계, 정렬되지 않은 input view, identity framing, 내장 NUL을 다루는 독립 Python-hashlib vector 35개.
- portable, automatic, hardware output을 비교하는 결정적인 512-input corpus와 동시 독립 hashing.
- CPU-feature predicate test가 없는 leaf 또는 필요한 instruction bit의 누락을 거부한다.
- Linux ASan/UBSan: 일반 build와 가속 비활성 build 모두 SHA test 6개가 전부 통과했다. leak detection을 켰고 suppression은 없었다.
- Windows MSVC x64: hardware를 사용할 수 있는 경우와 가속을 compile에서 제외한 경우 모두 독립 vector 35개가 통과했다. 이는 native backend correctness 검증이며 전체 Windows Program suite나 performance 주장에는 해당하지 않는다.
- 집중 Core suite: PostgreSQL checkpoint와 identity consumer를 포함해 217개가 통과했다.
- 전체 Program suite: 680개 통과, 적용되지 않는 memory-store process-restart case 2개 skip. golden stored identity, SQLite/PostgreSQL recovery, replacement, lineage, budget 검증을 유지한다.

## 독립 identity-hash 비교

input 크기마다 새 process pair 7개, warmup 20회, process마다 hash 측정 500회를 사용했고 1 MiB에서는 100회로 줄였다. AB/BA와 case 순서를 번갈아 사용했다. benchmark는 바뀌지 않은 identity framing과 hex encoding을 포함한다. 반환된 모든 digest를 Python hashlib로 독립 검증했다. 아래 timing은 process별 median들의 median이며 감소율은 paired ratio의 median으로 계산했다.

| Payload byte | 변경 전 (µs) | 변경 후 (µs) | Paired 감소율 |
| --- | --- | --- | --- |
| 0 | 0.391 | 0.240 | +38.62% |
| 32 | 0.591 | 0.291 | +50.76% |
| 55 | 0.721 | 0.281 | +61.03% |
| 56 | 0.591 | 0.290 | +50.96% |
| 63 | 0.591 | 0.280 | +53.36% |
| 64 | 0.591 | 0.281 | +52.45% |
| 128 | 0.782 | 0.311 | +59.26% |
| 1024 | 4.218 | 0.731 | +82.67% |
| 65536 | 190.305 | 30.727 | +83.79% |
| 1048576 | 3367.703 | 507.140 | +84.14% |

## Program 비교

동일한 Ryzen 7 5800X, WSL2 Ubuntu 24.04, repository hardening을 적용한 GCC 13.3 Release `-O3 -DNDEBUG`, Runtime scheduler thread 하나, native-WSL PostgreSQL 16.15 구성을 사용했다. 두 binary는 고정했다. case마다 process pair 5개, process마다 warmup 3회와 invocation 측정 12회, process마다 새 SQLite/PostgreSQL database를 사용했다. 이 timing pair를 측정하는 동안 build, profiler, 다른 test는 실행하지 않았다.

| Case | 변경 전 (ms) | 변경 후 (ms) | Paired 감소율 | Paired 변경 후/전 범위 |
| --- | --- | --- | --- | --- |
| direct-0 | 0.0081 | 0.0129 | -3.22% | 0.5786–1.6575 |
| memory-cpp-0 | 2.0560 | 1.9606 | +4.64% | 0.8751–0.9940 |
| memory-javascript-0-commands-1 | 3.2981 | 3.1661 | +3.73% | 0.9093–1.1659 |
| memory-javascript-65536-commands-1 | 15.2129 | 10.9126 | +28.27% | 0.6833–0.7740 |
| memory-javascript-0-commands-16 | 32.9455 | 30.7737 | +3.57% | 0.9186–1.0903 |
| sqlite-javascript-0-commands-1 | 19.3532 | 20.0416 | +3.06% | 0.9324–1.0396 |
| sqlite-javascript-65536-commands-1 | 65.1713 | 52.0023 | +20.30% | 0.7795–0.8390 |
| sqlite-javascript-0-commands-16 | 268.0836 | 263.5490 | +0.58% | 0.9359–1.0201 |
| postgres-javascript-0-commands-1 | 83.2907 | 83.2282 | -0.07% | 0.9840–1.0128 |
| postgres-javascript-65536-commands-1 | 190.9782 | 176.1487 | +8.45% | 0.8923–0.9774 |
| postgres-javascript-0-commands-16 | 992.9859 | 991.9219 | +0.21% | 0.9310–1.0355 |
| postgres-cpp-0 | 53.1536 | 53.1421 | +0.81% | 0.9605–1.0069 |

paired percentage는 독립적으로 요약한 median 열의 비율과 같을 필요가 없다. 큰 input의 개선은 pair 5개 모두에서 일관되었다. ratio가 1의 양쪽에 분포한 작은 case는 결론을 내릴 수 없다. 보편적인 속도 향상이나 LLM 응답 시간 비교가 아니다.

매우 작은 direct-Core 행의 noise가 커서 더 넓은 control을 한 번 수행했다. pair 7개, warmup 100회, direct invocation 측정 10,000회를 사용했다. 변경 후/전 ratio의 median은 1.0014, 범위는 0.9838–1.0331이었다. 유의미한 direct-Core 변화를 확립하지 못했다.

## Machine code와 후속 증거

owner-local AgentX radare2 read-only peer가 content-addressed 변경 후 ELF를 검사했다. symbol 경계 내 disassembly는 `compress_x86_sha` 안의 `SHA256RNDS2`, `SHA256MSG1`, `SHA256MSG2`, 검사한 portable compressor 안에 SHA 명령어가 없음, availability check의 CPUID 명령어를 확인했다. 특수 함수는 baseline 경로와 분리되어 있다.

변경 후 64 KiB memory workload의 software CPU-clock profile은 sample 1,418개를 수집했으며 loss는 0이었다. hardware compressor는 sample 약 9.31%, unescaped-string scanning은 17.91%를 차지했다. 약 39%는 해석하지 못한 shared-library symbol로 남아 있으므로 추가 attribution 없이 전부 allocation으로 돌려서는 안 된다. 이는 전체 process의 leaf-IP profile이지 wall-time 분할이 아니며 대표 timing에는 사용하지 않았다.

AgentX는 owner가 직접 허가한 analysis peer로 사용했다. AgentX model WorkOrder 또는 Analysis Graph provenance import를 주장하지 않는다. service configuration은 변경하지 않았다.

## 재현과 보존된 증거

변경 전 revision(`779be451`)과 변경 revision에서 같은 Release option으로 `bench_sha256`와 `bench_program_cost`를 빌드한다. 새 identity benchmark source는 오래된 Core library에 연결해 컴파일할 수 있다. 예:

```sh
bench_sha256 65536 500
```

end-to-end 비교에는 [paired Program runner](PROGRAM_COMMAND_HEAD_OPTIMIZATION.md)를 사용한다. build 시 가속을 끄려면 `-DNEOGRAPH_ENABLE_SHA256_ACCELERATION=OFF`를 설정한다.

raw result, 독립 vector, source/binary hash, upstream source 신원, AgentX RPC record, 경계가 제한된 assembly, CPU sample/map, Windows smoke log, test log는 `artifacts/program-sha256-20260907/`에 local 보존한다. database file은 WSL에 남아 있다. credential은 포함하지 않는다.
