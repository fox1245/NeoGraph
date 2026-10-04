<!-- neograph-i18n: source=docs/PROGRAM_COST_BASELINE_2026-09-07.md locale=ko source_sha256=a0929a5678214c2e42de563f8e5eb1d0137f06092e7ca01d27aa3a727b351339 -->
# Program 비용 baseline — 2026-09-07

**Languages:** [English](PROGRAM_COST_BASELINE_2026-09-07.md) | [한국어](PROGRAM_COST_BASELINE_2026-09-07.ko.md) | [日本語](PROGRAM_COST_BASELINE_2026-09-07.ja.md) | [简体中文](PROGRAM_COST_BASELINE_2026-09-07.zh-CN.md)

작은 payload의 baseline은 Program bookkeeping과 영속 store 작업을 첫 최적화 대상으로 가리킨다. JavaScript interpretation이 지배적인 비용임을 확립하지 않으며 JIT 속도 향상을 측정하지 않는다.

## 환경과 방법

- Source base: `319f8748c986a2f345ae6de88abb42d89eef9837`와 `metadata.json`의 source/binary hash로 기록한 측정 전용 추가분.
- AMD Ryzen 7 5800X, WSL2 Ubuntu 24.04, 사용 가능한 logical CPU 16개. GCC 13.3, Release `-O3 -DNDEBUG`, repository hardening 활성화.
- binary와 SQLite database는 WSL ext4 filesystem에 있다. PostgreSQL 16.15는 native WSL Docker의 local Docker volume에서 실행하며 localhost TCP로 연결한다. `fsync`, `synchronous_commit`, `full_page_writes`는 켜져 있고 WAL sync method는 `fdatasync`다.
- case마다 새 process 7개, lifecycle/direct process마다 warmup 10회와 invocation 측정 40회. Runtime scheduler thread 하나이며 동시 test job이나 model call은 없다. desktop/VM은 격리된 bare-metal benchmark host가 아니다.
- 성공한 process sample 308개, case 44개. 실제 invocation마다 counter와 payload output을 확인했다. 반복마다 case 순서를 정방향/역방향으로 바꿨다.
- 대표 값은 process별 median 7개의 median이다. 아래 p95는 process별 nearest-rank p95 7개의 median이지, 통계적으로 독립적인 pooled p95가 아니다.
- raw metadata, case, sample, summary는 `artifacts/program-costs-20260907/`에 local 보존한다. SQLite database는 WSL의 `/tmp/neograph-program-costs-20260907/` 아래 남아 있으며 database file은 commit하지 않는다.

[측정 정의와 재현](PROGRAM_COST_MEASUREMENT.md)을 참조한다. 이 diagnostic matrix는 기존 QuickJS acceptance gate를 대체하지 않는다.

## 빈 payload로 Core 한 번 호출

| 경로 | Median (ms) | Process 내 p95 (ms) | Process별 median의 MAD (ms) |
| --- | --- | --- | --- |
| Core 직접 실행, checkpoint 없음 | 0.0074 | 0.0076 | 0.0000 |
| C++ Program / memory | 2.0031 | 2.2607 | 0.0355 |
| JavaScript Program / memory | 3.3192 | 3.4898 | 0.0196 |
| C++ Program / SQLite | 8.5885 | 13.5360 | 0.1155 |
| JavaScript Program / SQLite | 16.4288 | 20.9731 | 0.0253 |
| C++ Program / PostgreSQL | 49.0355 | 51.2952 | 0.5896 |
| JavaScript Program / PostgreSQL | 80.1248 | 82.8138 | 0.5005 |

직접 Core 행은 Program lifecycle과 checkpoint 의미를 제외한다. Program에 대한 그 비율은 전체 실행 범위의 비교이지 동일한 durability의 engine 비교나 regression 증거가 아니다. C++ Program도 실제 journal/checkpoint 작업을 수행한다. JS–C++ 차이에는 interpreter뿐 아니라 추가 command journaling과 conversion도 포함된다.

## Payload 크기에 따른 변화

| 경로 | 0 byte (ms) | 4 KiB (ms) | 64 KiB (ms) |
| --- | --- | --- | --- |
| Core 직접 실행 | 0.0074 | 0.0079 | 0.1308 |
| C++ Program / memory | 2.0031 | 2.8567 | 12.2975 |
| JavaScript Program / memory | 3.3192 | 5.2922 | 30.8116 |
| JavaScript Program / SQLite | 16.4288 | 23.9060 | 130.6772 |
| JavaScript Program / PostgreSQL | 80.1248 | 93.5064 | 252.1259 |

payload는 실제 channel state를 통해 전달하며 변경되지 않았는지 확인한다. model output이나 모의 network delay가 아니다.

## Generator와 bridge 비용

| Payload | Open (µs) | 첫 command (µs) | Warm command 왕복 (µs) | Terminal next (µs) | Close (µs) |
| --- | --- | --- | --- | --- | --- |
| 0 | 330.34 | 45.65 | 26.66 | 5.73 | 35.85 |
| 4096 | 349.72 | 171.95 | 153.32 | 30.99 | 39.99 |
| 65536 | 811.60 | 2038.30 | 2088.19 | 423.18 | 41.01 |

이 분리된 호출은 synthetic Core response를 사용한다. warm 왕복 시간은 native JSON serialization/parsing, host command 생성, JS execution을 포함한다. 순수 bytecode execution 시간이 아니며 lifecycle 행에서 직접 뺄 수 없다. 큰 payload에서는 상당한 data-conversion 비용이 드러난다.

## Cold 준비

| Mode/backend | Store open (ms) | Compile (ms) | Admit (ms) | Runtime 생성 (ms) | 첫 run (ms) |
| --- | --- | --- | --- | --- | --- |
| memory-cpp-0 | 0.0054 | 0.6768 | 1.0492 | 0.1405 | 4.4991 |
| memory-javascript-0 | 0.0054 | 1.2498 | 1.2180 | 0.1420 | 5.9273 |
| sqlite-javascript-0 | 20.0656 | 1.3064 | 2.9385 | 0.1705 | 25.2293 |
| postgres-javascript-0 | 169.2527 | 1.2941 | 5.9809 | 0.1714 | 98.6726 |

compilation/admission은 warm invocation timing 밖에서 process당 한 번 발생한다. 새로 시작하는 JS Program마다 generator open은 다시 발생한다. 이 cold field는 일부 fixture setup을 제외하며 live replacement나 migration을 측정하지 않는다.

## Event marker별 분할

이 표에서만 각 cell은 측정한 모든 invocation의 평균이다. 따라서 rounding을 제외하면 marker interval 네 개의 합이 평균 total과 같다. 첫 Core event와 마지막 Core event 사이에는 checkpoint 작업이 포함되므로 순수 Core CPU 시간이 아니다.

| Backend (JavaScript, 0 byte) | 첫 Core event 전 (ms) | Core event 구간 (ms) | 마지막 Core event 후 (ms) | Terminal에서 wait 반환까지 (ms) |
| --- | --- | --- | --- | --- |
| memory | 1.1931 | 0.4062 | 1.7080 | 0.0340 |
| sqlite | 7.0816 | 1.2735 | 8.7646 | 0.0353 |
| postgres | 29.8013 | 9.1486 | 41.4887 | 0.0357 |

## 한 generator에서 여러 Core 호출

| Backend | Run당 1회 호출 (ms) | Run당 4회 호출 (ms) | Run당 16회 호출 (ms) |
| --- | --- | --- | --- |
| memory | 3.319 | 9.041 | 31.596 |
| sqlite | 16.429 | 55.527 | 331.089 |
| postgres | 80.125 | 244.105 | 1034.104 |

실제 journal과 checkpoint를 사용하는 순차 호출이다. 호출 수로 나누면 run startup/termination 비용이 분산되지만 dispatch 비용이 분리되지는 않는다.

## 일시 중단된 generator의 memory와 replay

| 일시 중단된 generator 수 | RSS 증가 median (MiB) |
| --- | --- |
| 1 | 1.285 |
| 32 | 6.309 |
| 128 | 21.934 |

| Replay한 synthetic recorded command 수 | 새 open + replay (ms) |
| --- | --- |
| 0 | 0.1276 |
| 10 | 0.5526 |
| 100 | 3.4148 |
| 1000 | 33.4788 |

위 memory는 bootstrap/allocator 효과를 포함한 독립적인 일시 중단 QuickJS runtime의 값이다. 전체 agent, catalog, Core engine, 영속 history는 제외한다. replay는 ProgramRuntime scheduling과 journal I/O를 제외하므로 process-loss recovery latency가 아니다.

## 보충 database API profile

baseline 이후 추가 run 72개에서 profiling on/off, invocation 1회와 21회, 반복 3회, 모든 backend의 두 control mode를 비교했다. 표는 `(21-run process total − 1-run process total) / 20`을 사용한 뒤 반복 간 median을 구한다. call-count 차이는 세 반복 모두 동일했다. timing 차이는 cold-process 변동의 영향을 받는 추정치로 남는다.

| 경로 | Run당 SQLite step call | Run당 SQLite exec call | Run당 SQLite commit | Run당 synchronous libpq call |
| --- | --- | --- | --- | --- |
| memory-cpp | 0 | 0 | 0 | 0 |
| memory-javascript | 0 | 0 | 0 | 0 |
| sqlite-cpp | 73 | 14 | 7 | 0 |
| sqlite-javascript | 117 | 18 | 9 | 0 |
| postgres-cpp | 0 | 0 | 0 | 64 |
| postgres-javascript | 0 | 0 | 0 | 108 |

SQLite step은 row에 따라 반복될 수 있으므로 고유한 SQL statement 수가 아니다. `exec`는 transaction control을 포함하며 중첩 step은 제외한다. libpq count/time은 synchronous `PQexec`/`PQexecParams`만 다루고 async checkpoint 경로와 connection establishment는 제외한다. client CPU는 PostgreSQL server를 제외한다. API wall interval과 client CPU는 겹치므로 합산하거나 baseline latency의 완전한 분할로 취급해서는 안 된다.

memory control에서는 database call이 0회였다. JS SQLite case는 invocation마다 commit 9회와 외부 step call 117회를 수행한다. 그 timed database call 밖의 CPU 작업도 profiling할 필요가 있다. JS PostgreSQL case는 invocation마다 synchronous call 108회를 추가하므로 JIT 실험 전에 왕복 횟수가 구체적인 대상이 된다.

보충 증거는 `artifacts/program-costs-20260907/profile/`에 있다. 첫 profiler는 SQLite `exec` transaction call을 누락했고 그 초기 output은 사용하지 않았다. file copying이 멈춘 뒤 수정된 profile을 다시 실행했다. 그러나 이후의 profiled **및 unprofiled** run은 memory control을 포함해 main baseline보다 상당히 느렸다. 이 환경 변화의 원인은 분리해 내지 못했다. 따라서 여기서는 안정적인 call count만 검증된 값으로 인정한다. raw API와 CPU time은 보존하지만 **main baseline latency의 attribution에는 사용하지 않는다**. main baseline data는 copy 문제 전에 수집했고 반복 7개 전체와 분산을 유지한다.

기존 `bench_program`도 새 process 3개에서 C++ Program 경로 2.21–2.31 ms(Core: 6.41–6.65 µs)를 보였다. 그 fixture는 channel 하나와 다른 bounds를 사용하므로 밀리초 범위를 교차 확인하는 자료이지 main matrix를 대체하는 matched 비교가 아니다.

## 다음 측정과 최적화 순서

1. Program startup/termination과 publication serialization을 profile한다. in-memory store와 C++ control에서도 작은 payload 비용이 상당하므로 JS engine 교체만으로 전체 실행 범위의 비용을 해결할 수 없다.
2. 측정된 SQLite/libpq call count로 불필요한 read와 왕복을 조사하되 동일한 journal, owner, authority, budget, atomicity guarantee를 보존한다. 더 나은 수치를 얻으려고 영속 commit을 비활성화하지 않는다.
3. payload sweep으로 반복되는 canonical JSON conversion과 copying을 조사한다. compiled-source reuse는 warm generator execution과 별도로 benchmark한다.
4. 실제 JS CPU 작업을 분리한 뒤에만 JIT backend를 비교하며 현실적인 agent 수의 cold-start 비용과 memory를 포함한다.

전체 CPU attribution, 동시 multi-tenant throughput/fairness, recursive spawn, live replacement, 실제 JIT 비교는 측정되지 않았다. 이 baseline에서는 production runtime을 최적화하지 않았다.
