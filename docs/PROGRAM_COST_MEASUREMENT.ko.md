<!-- neograph-i18n: source=docs/PROGRAM_COST_MEASUREMENT.md locale=ko source_sha256=f22d74f413d9d1665f1444d3a29a05212e4d0ff9c515be846e5439fabe2177e1 -->
# Program 비용 측정

**Languages:** [English](PROGRAM_COST_MEASUREMENT.md) | [한국어](PROGRAM_COST_MEASUREMENT.ko.md) | [日本語](PROGRAM_COST_MEASUREMENT.ja.md) | [简体中文](PROGRAM_COST_MEASUREMENT.zh-CN.md)

`bench_program_cost`와 `scripts/run_program_costs.py`는 어떤 Program 비용을 최적화할지 결정하기 위한 diagnostic baseline을 제공한다. runtime 의미를 바꾸거나 사전 등록한 QuickJS performance gate를 대체하지 않는다. LLM, network model call, model credential은 필요 없다.

구현을 비교할 때는 `scripts/compare_program_costs.py`가 새 process pair에서 고정된 `bench_program_cost` binary 두 개를 번갈아 실행한다. 첫 비교는 [Command publication head 최적화](PROGRAM_COMMAND_HEAD_OPTIMIZATION.md)에 기록되어 있다. 후속 [canonical JSON 최적화](PROGRAM_CANONICAL_JSON_OPTIMIZATION.md)는 AgentX disassembly와 CPU sampling으로 native string-processing 변경을 선택한다. 이어지는 [SHA-256 최적화](PROGRAM_SHA256_OPTIMIZATION.md)는 독립적으로 검증된 portable fallback과 함께 runtime 조건으로 선택하는 CPU 가속을 추가한다. `bench_canonical_json CASE BYTES ITERATIONS`는 scalar output oracle과 대조하며 ASCII, Unicode, mixed, escape가 많은 string을 별도로 실행한다.

## 빌드와 실행

native filesystem에서 optimized build를 사용한다. 예를 들어 Linux에서는:

```sh
cmake -S . -B build-cost -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DNEOGRAPH_BUILD_TESTS=OFF \
  -DNEOGRAPH_BUILD_BENCHMARKS=ON -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON \
  -DNEOGRAPH_BUILD_SQLITE=ON -DNEOGRAPH_BUILD_POSTGRES=ON
cmake --build build-cost -j4 --target \
  bench_program_cost bench_quickjs_primitives bench_quickjs_control
python3 scripts/run_program_costs.py \
  --build-dir build-cost --output-dir /tmp/neograph-cost-results
```

runner에는 **새** output directory가 필요하다. 기본 matrix는 memory와 SQLite, 독립 process 반복 7회, warmup invocation 10회, lifecycle case마다 invocation 측정 100회를 사용한다. `--iterations`와 `--warmup`은 lifecycle/direct case를 제어한다. generator microbenchmark는 command 1,000개를 사용한다. case는 순차 실행하며 반복마다 정방향/역방향 순서를 번갈아 사용한다. SQLite file은 output directory에 저장하고 검사를 위해 남겨 둔다.

PostgreSQL을 포함하려면 먼저 전용 local test container를 시작하고 `NEOGRAPH_COST_POSTGRES_URL`에 공개된 localhost connection URL을 설정한 뒤 `--postgres-container <container-name>`을 추가한다. runner는 공개 port를 확인한다. sample마다 고유 이름의 database를 만들고 이후 그 database만 drop한다. container를 시작하거나 멈추지 않으며 URL에 지정한 database를 사용하거나 drop하지 않는다. 일회용 server는 `postgres`가 `docker exec`를 통해 database를 생성/drop하도록 허용해야 한다. application server를 대상으로 삼지 않는다. credential은 metadata나 command record에 기록하지 않는다.

## 각 측정 범위에 포함되는 것

| Case | 포함 | 제외 |
| --- | --- | --- |
| `direct` | Warm `GraphEngine::run`, 순차 increment node 3개, state channel 2개 | Program admission, journal, checkpoint persistence, JS |
| `lifecycle --mode cpp` | 승인된 C++ builder v1 Program, 실제 Core, result 생성, journal과 checkpoint | JavaScript control execution; compilation/admission은 별도 cold field |
| `lifecycle --mode javascript` | 승인된 `define()` + generator `main()`, 실제 Core, JS/C++ conversion, command journal과 checkpoint | Compilation/admission은 별도 cold field |
| `generator` | Generator open/compile/init, 첫 `next`, 반복 host-command 왕복, terminal conversion, teardown | 실제 Core, scheduling, journal, database I/O; response는 synthetic |
| `resident` | 첫 command 이후 일시 중단된 여러 독립 QuickJS runtime | 전체 Program agent, catalog, engine, journal storage |
| 기존 primitive/control case | `bench_quickjs_primitives`와 `bench_quickjs_control`에 이미 문서화된 범위 | 이 sample은 enabled/disabled gate 실행이 아님 |

direct, C++, JavaScript run은 같은 3-node topology, increment 동작, payload channel을 사용한다. 실제 run은 모두 counter `3`과 변경되지 않은 전체 payload로 끝나야 한다. payload 크기는 0, 4,096, 65,536 byte다. JavaScript multi-command 행은 같은 Core graph를 4회 또는 16회 실행하며 매 호출마다 counter input을 0으로 초기화한다. 최종 counter는 `3`이다.

C++ v1은 정확히 하나의 Program operation을 승인한다. JavaScript generator는 모든 command-count 행을 지원하기 위해 호스트가 선언한 operation 상한 64를 사용한다. 둘은 나머지 resource 상한과 scheduler thread 하나를 동일하게 사용한다. source 형식과 command-journaling 의미가 다르므로 JavaScript에서 C++ 값을 빼도 interpreter 시간이 분리되지 **않는다**.

각 lifecycle case는 선택한 backend의 ProgramStore, CheckpointStore, ProgramTransitionStore를 생성한다. memory mode는 실제 in-memory journal/checkpoint 값을 유지하며 persistence interface를 우회하지 않는다. SQLite는 독립 store connection으로 하나의 database file을 사용한다. 기존 CheckpointStore는 WAL과 `synchronous=NORMAL`을 설정하며 Program store connection은 SQLite synchronous 기본값을 유지한다. PostgreSQL은 기존 store 구현과 4-connection checkpoint pool을 사용한다. 결과를 비교할 때 실제 server setting을 기록한다. 이는 backend 기본값에 대한 설명이지 동일한 power-loss guarantee를 주장하는 내용이 아니다.

## Timing과 memory field 읽기

`samples`는 측정한 모든 invocation을 담고 `first_run`은 warmup이 끝나기 전에 별도로 기록한다. `cold`는 store 생성, Program compilation, admission, Runtime 생성을 담는다. 이 field는 일부 fixture setup을 제외하므로 전체 process-startup 분할이 아니다.

하나의 순차 invocation 안에서 `before_core_us`, `core_span_us`, `after_core_us`, `wake_us`는 Program event timestamp를 사용해 시작부터 wait 반환까지의 wall time을 분할한다. **Core event 구간은 순수 Core CPU 시간이 아니다.** checkpoint 작업과 multi-command 행에서 Core call 사이의 interval을 포함한다. `start_us`와 `post_start_us`는 두 번째 분할을 제공한다. Core 작업이 `start()` 반환 전에 시작할 수 있으므로 두 분할을 더해서는 안 된다.

event sink는 atomic timestamp를 기록한다. 따라서 lifecycle result에는 이 observer가 포함되지만 direct/generator case에는 event sink가 없다. 이는 diagnostic timing이며 observer 없는 최소 latency를 주장하지 않는다.

runner는 먼저 각 process를 요약하고 그 다음 독립 process result를 요약한다. `invocation_median.total_us.median`은 process별 median의 median이다. `invocation_p95.total_us.median`은 process 내부 p95의 median이다. percentile은 nearest rank를 사용한다. process sample이 7개이면 process-level p95는 그 최댓값이다. raw sample, MAD, 극값은 계속 사용할 수 있다. median 사이의 작은 차이에서 통계적 유의성을 추론하지 않는다.

Linux RSS는 process memory이며 JS allocation accounting이 아니다. lifecycle RSS 증가에는 유지된 run history와 allocator 동작이 포함된다. resident-generator RSS에는 runtime/bootstrap overhead와 allocator granularity가 포함된다. 더 큰 generator 수 사이의 기울기로 추가 점유량을 추정한다. 둘 다 재귀 agent 하나의 전체 memory를 측정한 값이 아니다. 다른 platform의 memory field가 0이면 Linux 전용 probe를 사용할 수 없다는 의미다.

runner는 binary/source hash, source commit과 tracked diff hash, CMake option, CPU, process affinity를 기록한다. report와 함께 raw evidence를 보관하고 측정 실행 중에는 binary를 다시 빌드하지 않는다.

## Linux에서 선택적으로 수행하는 database API 진단

`benchmarks/program_store_profile.c`는 Program runtime을 바꾸지 않고 database-library call의 횟수와 시간을 측정할 수 있다. main unprofiled matrix를 완료한 **뒤에** 별도로 빌드한다.

```sh
cc -O2 -shared -fPIC -I/usr/include/postgresql \
  benchmarks/program_store_profile.c -o /tmp/program_store_profile.so -ldl -pthread
NEOGRAPH_COST_STORE_PROFILE=/tmp/new-store-profile.json \
LD_PRELOAD=/tmp/program_store_profile.so \
  build-cost/bench_program_cost --case lifecycle --backend sqlite \
  --storage /tmp/new-cost-profile.sqlite --iterations 40 --warmup 0
```

profiler는 category와 aggregate count/time만 기록하며 SQL text, parameter, credential은 기록하지 않는다. `sqlite3_step`, synchronous `PQexec`와 `PQexecParams`, `sqlite3_exec`(transaction control 포함)를 측정한다. 이중 계산을 피하기 위해 `exec` 안의 중첩 SQLite step은 제외한다. SQLite row iteration은 query 하나에 대해 `step`을 여러 번 호출할 수 있다. async libpq call, connection establishment, SQLite prepare time, 호출자 측 serialization은 이 API interval 밖에 있다. 전체 process total에는 constructor, schema 생성, admission, warmup이 포함된다. profiling은 overhead를 추가하고 동시 API interval은 겹칠 수 있으므로 이 값을 정확한 CPU 분할로 보고 baseline wall time에서 빼서는 안 된다. invocation 수가 다른 새 database run을 비교해 고정 setup call count와 run별 call을 분리한다.

## 이 baseline의 경계

이 matrix는 순차 synthetic execution과 일시 중단 generator 점유량을 측정한다. tenant fairness, concurrent throughput, recursive spawn, live replacement, process-loss recovery, production storage, JIT backend를 검증하지 않는다. generator replay 행은 기록된 synthetic command response를 replay한다. 전체 영속 reconnect 시간을 측정하지 않는다. JIT의 가능성을 평가하려면 warmup, compilation, memory 비용을 포함한 실제 비교가 필요하다.
