<!-- neograph-i18n: source=docs/PROGRAM_COMMAND_HEAD_OPTIMIZATION.md locale=ko source_sha256=52b5dae9fe5c514792e6ec85508f8e548faef3d737711c484cd04294fa3542ef -->
# Command publication head 최적화 — 2026-09-07

**Languages:** [English](PROGRAM_COMMAND_HEAD_OPTIMIZATION.md) | [한국어](PROGRAM_COMMAND_HEAD_OPTIMIZATION.ko.md) | [日本語](PROGRAM_COMMAND_HEAD_OPTIMIZATION.ja.md) | [简体中文](PROGRAM_COMMAND_HEAD_OPTIMIZATION.zh-CN.md)

이 변경은 JavaScript command를 게시하기 전의 읽기를 줄인다. run/journal을 따로 읽고 전체 command history를 materialize하는 대신 run, journal, 최신 command를 함께 읽는다. 영속 publication, CAS, reservation, checkpoint, recovery 의미는 바꾸지 않는다.

## 구현

- `ProgramTransitionStore::load_command_publication_head`는 일관된 run/journal 쌍과 최신 command append를 반환한다. memory는 불변 snapshot 하나를 사용하고 SQLite와 PostgreSQL은 statement 하나와 기존 `(owner_scope, run_id, sequence)` primary key를 사용한다.
- database projection은 사용하지 않는 migration 및 last-publication byte를 제외한다. 최신 command sequence와 좌표를 canonical 값과 대조하고 run/owner/bundle/journal binding을 검증한다.
- 일반 append와 settlement는 최신 command를 사용한다. 더 오래된 좌표의 retry는 계속 전체 history를 사용한다. replay와 store 측 append/reservation 검증도 이전처럼 history를 검사한다. history scan을 완전히 제거한 변경은 아니다.
- 기존 C++ store wrapper에는 기존 virtual read를 사용하고 두 번째 run read로 경계를 확인하는 fallback이 제공된다. 동시 변경은 안전하게 거부한다. wrapper는 자체 filtering과 authority 의미를 보존하면서 새 read를 구현할 수 있다.
- schema migration, 새 index, cache lifetime, durability setting, graph generation, compiler 신원, model configuration은 바꾸지 않았다.
- 공개 C++ virtual interface에 method 하나가 추가되었다. Program library와 C++ consumer를 함께 다시 빌드한다. 기존 wrapper의 source compatibility는 검증했지만 prebuilt C++ consumer의 ABI compatibility를 가정하지 않는다.

## 정확성

WSL GCC Debug Program suite에서 **680개 test가 통과**했고 **적용되지 않는 memory-store process-restart case 2개는 skip**했다. 추가한 test 7개와 확장한 backend history test는 불변 snapshot, 독립 reader/writer connection, fallback wrapper, 불일치하는 fallback read, 선택된 tail의 corruption, 제한된 tail read를 다룬다. 과거 history corruption은 full-history read가 계속 거부한다.

전체 suite는 command recovery, lineage CAS, recursive child replacement, 재충전할 수 없는 budget, SQLite/PostgreSQL의 실제 process-loss 경계도 실행했다. 이 변경에 대해 Windows 또는 sanitizer 재실행을 주장하지 않는다.

## Paired Release 비교

baseline과 같은 Ryzen 7 5800X / WSL2 Ubuntu 24.04 / GCC 13.3 Release 구성을 사용했다. 비교 전에 두 binary를 고정했다. case마다 독립 process pair 5개를 사용하고 process마다 warmup 3회와 invocation 측정 12회를 수행했다. case와 AB/BA 순서를 번갈아 사용했다. PostgreSQL은 native WSL Docker와 process별 새 database를 사용했고 SQLite는 새 WSL-ext4 file을 사용했다. paired matrix를 측정하는 동안 build나 다른 test job은 실행하지 않았다.

변경 전 binary hash는 원래 baseline과 일치한다. 아래 변경 전/후 측정은 당시 같은 turn에 함께 측정했으며 오래된 wall-clock timing에 대한 비교를 대체한다. JIT나 LLM은 관여하지 않는다.

| Case | 변경 전 median (ms) | 변경 후 median (ms) | Paired 감소율 median | Paired 변경 후/전 범위 |
| --- | --- | --- | --- | --- |
| direct-0 | 0.0076 | 0.0077 | -2.71% | 1.0007–1.0418 |
| memory-cpp-0 | 2.0373 | 2.0332 | -0.58% | 0.9857–1.0336 |
| memory-javascript-0-commands-1 | 3.3096 | 3.3046 | +0.59% | 0.9780–1.0432 |
| memory-javascript-65536-commands-1 | 31.0390 | 30.7436 | +0.89% | 0.9843–1.0272 |
| memory-javascript-0-commands-16 | 32.0270 | 32.2163 | -0.13% | 0.9808–1.0261 |
| sqlite-javascript-0-commands-1 | 19.5180 | 18.9032 | +3.20% | 0.9645–1.0497 |
| sqlite-javascript-65536-commands-1 | 130.1083 | 122.0217 | +6.02% | 0.9213–0.9490 |
| sqlite-javascript-0-commands-16 | 326.5041 | 256.5506 | +20.94% | 0.7812–0.8014 |
| postgres-javascript-0-commands-1 | 81.8919 | 78.3417 | +4.34% | 0.9037–1.0263 |
| postgres-javascript-65536-commands-1 | 257.4166 | 240.3124 | +6.76% | 0.9198–0.9608 |
| postgres-javascript-0-commands-16 | 1035.1816 | 922.3063 | +10.90% | 0.8689–0.9304 |
| postgres-cpp-0 | 49.0719 | 48.2794 | +1.48% | 0.9555–1.0062 |

감소율은 paired process median으로 계산하므로 독립적인 두 summary median의 비율과 같을 필요가 없다. paired 분산 안의 작은 차이는 결론을 내릴 수 없다. direct Core와 C++ Program 행은 negative control이다. 이 경로에서는 최적화된 command-publication read를 사용하지 않는다. 작은 payload의 memory case는 명확한 속도 향상을 확립하지 못했다.

작은 direct-Core 차이를 보고 더 넓은 control 검사를 한 번 수행했다. paired process 7개, 각각 warmup 100회와 direct invocation 측정 10,000회를 사용했다. 변경 후/전 ratio median은 1.0098이었고 pair 범위는 0.9825–1.0123이었다. raw result는 `direct-control/` 아래 보존한다. Core source나 library code는 바꾸지 않았으며 이 검사로 유의미한 Core 성능 변화를 확립하지 못했다.

## Database API 횟수

별도의 instrumentation 비교는 invocation 1회와 4회 차이를 구하고 두 번 반복했다. 두 반복에서 실행당 모든 count 차이가 동일했다. 이 instrumentation의 timing은 대표 비교에 사용하지 않는다. SQLite step call에는 row iteration이 포함되며 libpq는 synchronous call만 다룬다.

| Backend / Program run당 Core call | 변경 전 SQL API call | 변경 후 SQL API call | 변경 전 commit | 변경 후 commit |
| --- | --- | --- | --- | --- |
| sqlite / 1 | 135 | 130 | 9 | 9 |
| sqlite / 16 | 2715 | 2155 | 114 | 114 |
| postgres / 1 | 108 | 104 | 4 | 4 |
| postgres / 16 | 1053 | 989 | 34 | 34 |

PostgreSQL commit 열은 synchronous libpq 부분집합만 나타낸다. async checkpoint 경로의 전체 transaction 수가 아니다. 두 binary는 같은 persistence interface를 실행하며 측정 범위 안에서 같은 commit 횟수를 유지한다.

## 재현과 증거

변경 전 revision(`8fce5e14`)과 변경 revision에서 같은 Release option으로 `bench_program_cost`를 빌드한다. 두 binary를 보존한다. 일회용 Postgres container를 실행하고 `NEOGRAPH_COST_POSTGRES_URL`을 설정한 상태에서:

```sh
python3 scripts/compare_program_costs.py \
  --before /tmp/before/bench_program_cost \
  --after /tmp/after/bench_program_cost \
  --output /tmp/new-command-head-comparison \
  --postgres-container neograph-n2-postgres
```

자세한 방법은 [Program 비용 측정](PROGRAM_COST_MEASUREMENT.md)을 참조한다. raw paired sample, count, source/binary hash, verification log는 `artifacts/program-command-head-20260907/`에 local 보존하며 큰 SQLite file은 WSL에 남아 있다. runner는 output directory를 덮어쓰거나 기존 application database를 재사용하지 않는다.

다음으로 남은 대상은 publication과 replay 중의 full-history validation/materialization, canonical record 생성이다. 이 변경은 그 비용을 제거했거나 JIT가 도움이 된다는 사실을 입증하지 않는다.
