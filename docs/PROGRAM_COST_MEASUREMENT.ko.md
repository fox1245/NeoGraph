<!-- neograph-i18n: source=docs/PROGRAM_COST_MEASUREMENT.md locale=ko source_sha256=f22d74f413d9d1665f1444d3a29a05212e4d0ff9c515be846e5439fabe2177e1 -->
# Program 비용 측정

**Languages:** [English](PROGRAM_COST_MEASUREMENT.md) | [한국어](PROGRAM_COST_MEASUREMENT.ko.md) | [日本語](PROGRAM_COST_MEASUREMENT.ja.md) | [简体中文](PROGRAM_COST_MEASUREMENT.zh-CN.md)

`bench_program_cost`와 `scripts/run_program_costs.py`는 어떤 Program 비용부터 최적화할지 정할 때 쓰는 진단용 기준선(baseline)을 제공합니다. 런타임 의미는 바꾸지 않으며, 사전에 등록한 QuickJS 성능 게이트를 대신하지도 않습니다. LLM, 네트워크 모델 호출, 모델 자격 증명은 필요 없습니다.

구현을 비교할 때는 `scripts/compare_program_costs.py`가 고정된 `bench_program_cost` 바이너리 두 개를 새 프로세스 쌍으로 번갈아 실행합니다. 첫 번째 비교는 [Command 게시 head 최적화](PROGRAM_COMMAND_HEAD_OPTIMIZATION.md)에 문서화되어 있습니다. 이어서 진행한 [정규 JSON 최적화](PROGRAM_CANONICAL_JSON_OPTIMIZATION.md)는 AgentX 디스어셈블리와 CPU 샘플링으로 네이티브 문자열 처리 변경을 골랐습니다. 그다음 [SHA-256 최적화](PROGRAM_SHA256_OPTIMIZATION.md)는 런타임에 켜고 끄는 CPU 가속과, 독립적으로 검증한 이식 가능한 대체 구현을 추가합니다. `bench_canonical_json CASE BYTES ITERATIONS`는 이와 별도로 ASCII, 유니코드, 혼합, 이스케이프가 많은 문자열을 스칼라 출력 오라클과 비교하며 실행합니다.

## 빌드와 실행

네이티브 파일 시스템에서 최적화 빌드를 사용합니다. Linux에서는 예를 들어 다음과 같이 합니다.

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

러너는 **새** 출력 디렉터리를 요구합니다. 기본 매트릭스는 memory와 SQLite를 사용하고, 독립된 프로세스를 7번 반복하며, 생명주기 케이스마다 워밍업 호출 10번과 측정 호출 100번을 실행합니다. `--iterations`와 `--warmup`은 생명주기/direct 케이스를 조절하고, 제너레이터 마이크로벤치마크는 명령 1,000개를 사용합니다. 케이스는 순차적으로 실행하되, 반복할 때마다 정방향과 역방향 순서를 번갈아 씁니다. SQLite 파일은 출력 디렉터리에 만들어지며 나중에 살펴볼 수 있도록 남습니다.

PostgreSQL을 포함하려면 먼저 전용 로컬 테스트 컨테이너를 띄우고, `NEOGRAPH_COST_POSTGRES_URL`에 그 컨테이너가 게시한 localhost 연결 URL을 설정한 다음 `--postgres-container <container-name>`을 추가합니다. 러너는 게시된 포트를 확인합니다. 샘플마다 고유한 이름의 데이터베이스를 만들고, 끝나면 그 데이터베이스만 삭제합니다. 컨테이너를 시작하거나 중지하지 않으며, URL에 적힌 데이터베이스는 사용하지도 삭제하지도 않습니다. 이 일회용 서버는 `postgres` 사용자가 `docker exec`로 데이터베이스를 만들고 삭제할 수 있어야 합니다. 애플리케이션 서버를 대상으로 삼아서는 안 됩니다. 자격 증명은 메타데이터나 명령 기록에 쓰지 않습니다.

## 케이스별 측정 범위

| 케이스 | 포함 | 제외 |
| --- | --- | --- |
| `direct` | 워밍업을 마친 `GraphEngine::run`, 순차 증가 노드 3개, 상태 채널 2개 | Program 승인(admission), 저널, 체크포인트 영속화, JS |
| `lifecycle --mode cpp` | 승인(admission)된 C++ builder v1 Program, 실제 Core, 결과 구성, 저널과 체크포인트 | JavaScript 제어 실행. 컴파일/승인(admission)은 별도의 cold 필드로 기록 |
| `lifecycle --mode javascript` | 승인(admission)된 `define()` + 제너레이터 `main()`, 실제 Core, JS/C++ 변환, 명령 저널과 체크포인트 | 컴파일/승인(admission)은 별도의 cold 필드로 기록 |
| `generator` | 제너레이터 열기/컴파일/초기화, 첫 `next`, 반복되는 호스트 명령 왕복, 종료 변환, 정리(teardown) | 실제 Core, 스케줄링, 저널, 데이터베이스 I/O. 응답은 합성(synthetic) 데이터 |
| `resident` | 첫 명령 뒤에 일시 중지된 독립 QuickJS 런타임 여러 개 | 완전한 Program 에이전트, 카탈로그, 엔진, 저널 저장소 |
| 기존 primitive/control 케이스 | `bench_quickjs_primitives`와 `bench_quickjs_control`에 이미 문서화된 범위 | 이 샘플은 게이트를 켠 실행과 끈 실행의 비교가 아님 |

direct, C++, JavaScript 실행은 모두 같은 3노드 토폴로지, 증가 동작, payload 채널을 사용합니다. 실제 실행은 모두 카운터 `3`과 변경 없는 payload 전체로 끝나야 합니다. payload 크기는 0, 4,096, 65,536바이트입니다. JavaScript 다중 명령 행은 같은 Core 그래프를 4번 또는 16번 실행하며, 호출할 때마다 카운터 입력을 0으로 재설정합니다. 이 경우에도 카운터는 `3`으로 끝납니다.

C++ v1은 Program 연산을 정확히 하나만 승인(admission)합니다. JavaScript 제너레이터는 명령 수별 행을 모두 지원하도록 호스트가 선언한 연산 상한 64를 사용합니다. 둘 다 남은 리소스 상한은 같고 스케줄러 스레드도 하나입니다. 소스 형태와 명령 저널링 의미가 다르므로, JavaScript에서 C++를 빼도 인터프리터 시간만 분리되는 것은 **아닙니다**.

생명주기 케이스는 선택한 백엔드에서 ProgramStore, CheckpointStore, ProgramTransitionStore를 만듭니다. memory 모드도 실제 인메모리 저널/체크포인트 값을 유지하며 영속화 인터페이스를 우회하지 않습니다. SQLite는 데이터베이스 파일 하나를 쓰고 저장소마다 독립된 연결을 둡니다. 기존 CheckpointStore는 WAL과 `synchronous=NORMAL`을 설정하고, Program 저장소 연결은 SQLite의 기본 synchronous 값을 그대로 씁니다. PostgreSQL은 기존 저장소 구현과 연결 4개짜리 체크포인트 풀을 사용합니다. 결과를 비교할 때는 실제 서버 설정을 기록해 두어야 합니다. 이 설정들은 백엔드 기본값일 뿐 정전 시 보장 수준이 같다는 뜻이 아닙니다.

## 시간 및 메모리 필드 읽는 법

`samples`에는 측정한 호출이 모두 들어 있고, `first_run`은 워밍업이 끝나기 전에 따로 기록합니다. `cold`에는 저장소 생성, Program 컴파일, 승인(admission), Runtime 생성이 들어갑니다. 이 필드들은 일부 픽스처 준비 시간을 제외하므로 프로세스 시작 시간을 빠짐없이 나눈 것은 아닙니다.

순차 호출 한 번 안에서 `before_core_us`, `core_span_us`, `after_core_us`, `wake_us`는 Program 이벤트 타임스탬프를 써서 시작부터 wait가 반환될 때까지의 벽시계 시간을 나눕니다. **Core 이벤트 구간은 순수한 Core CPU 시간이 아닙니다.** 체크포인트 작업이 들어 있고, 다중 명령 행에서는 Core 호출 사이의 간격도 포함합니다. `start_us`와 `post_start_us`는 두 번째 분할을 제공합니다. 두 분할을 더해서는 안 됩니다. Core 작업은 `start()`가 반환되기 전에 시작될 수 있습니다.

이벤트 싱크는 원자적 타임스탬프를 기록합니다. 따라서 생명주기 결과에는 이 관찰자의 비용이 포함되고, direct/generator 케이스에는 이벤트 싱크가 없습니다. 이 값은 진단용 시간이며 관찰자가 없을 때의 최소 지연 시간을 나타내지 않습니다.

러너는 먼저 프로세스별로 요약한 다음, 독립된 프로세스 결과들을 다시 요약합니다. `invocation_median.total_us.median`은 프로세스별 중앙값의 중앙값이고, `invocation_p95.total_us.median`은 프로세스 안에서 구한 p95들의 중앙값입니다. 백분위수는 nearest rank 방식을 쓰므로, 프로세스 샘플이 7개일 때 프로세스 수준 p95는 그 최댓값입니다. 원시 샘플, MAD, 극값도 그대로 남아 있습니다. 중앙값의 작은 차이로 통계적 유의성을 판단해서는 안 됩니다.

Linux RSS는 프로세스 메모리이며 JS 할당 회계가 아닙니다. 생명주기의 RSS 증가에는 보존된 run 이력과 할당자 동작이 포함됩니다. 상주 제너레이터의 RSS에는 런타임/부트스트랩 오버헤드와 할당자 단위(granularity)가 포함되므로, 점유량이 늘어나는 정도는 제너레이터 수가 큰 구간 사이의 기울기로 추정할 수 있습니다. 어느 쪽도 재귀 에이전트 하나당 전체 메모리를 측정한 값이 아닙니다. 다른 플랫폼에서 메모리 필드가 0이면 Linux 전용 프로브를 쓸 수 없다는 뜻입니다.

러너는 바이너리/소스 해시, 소스 커밋과 추적 중인 diff의 해시, CMake 옵션, CPU와 프로세스 affinity를 기록합니다. 보고서를 낼 때는 원시 증거를 함께 보관하고, 측정하는 동안에는 바이너리를 다시 빌드해서는 안 됩니다.

## 선택 사항: Linux에서 데이터베이스 API 진단

`benchmarks/program_store_profile.c`를 쓰면 Program 런타임을 바꾸지 않고 데이터베이스 라이브러리 호출의 횟수와 시간을 잴 수 있습니다. 프로파일링하지 않은 기본 매트릭스가 **끝난 뒤에** 따로 빌드합니다.

```sh
cc -O2 -shared -fPIC -I/usr/include/postgresql \
  benchmarks/program_store_profile.c -o /tmp/program_store_profile.so -ldl -pthread
NEOGRAPH_COST_STORE_PROFILE=/tmp/new-store-profile.json \
LD_PRELOAD=/tmp/program_store_profile.so \
  build-cost/bench_program_cost --case lifecycle --backend sqlite \
  --storage /tmp/new-cost-profile.sqlite --iterations 40 --warmup 0
```

프로파일러는 분류와 집계된 횟수/시간만 기록하며 SQL 텍스트, 파라미터, 자격 증명은 기록하지 않습니다. 측정 대상은 `sqlite3_step`, 동기 방식의 `PQexec`와 `PQexecParams`, 그리고 `sqlite3_exec`(트랜잭션 제어 포함)입니다. `exec` 안에서 중첩되는 SQLite step은 이중 집계를 피하려고 제외합니다. SQLite에서 행을 순회하면 쿼리 하나에 `step`이 여러 번 호출될 수 있습니다. 비동기 libpq 호출, 연결 수립, SQLite prepare 시간, 호출자 쪽 직렬화는 이 API 구간에 들어가지 않습니다. 프로세스 전체 합계에는 생성자, 스키마 생성, 승인(admission), 워밍업이 포함됩니다. 프로파일링은 오버헤드를 더하고 동시에 실행되는 API 구간은 겹칠 수 있으므로, 이 값을 기준선의 벽시계 시간에서 빼서 정확한 CPU 분할로 쓰면 안 됩니다. 고정된 준비 단계의 호출 횟수와 실행마다 발생하는 호출 횟수를 나누려면, 새 데이터베이스에서 호출 횟수를 달리해 실행한 결과를 비교하면 됩니다.

## 이 기준선의 한계

이 매트릭스가 측정하는 것은 순차적인 합성(synthetic) 실행과 일시 중지된 제너레이터의 점유량입니다. 테넌트 공정성, 동시 처리량, 재귀 spawn, 실행 중 교체, 프로세스 유실 복구, 프로덕션 저장소, JIT 백엔드는 검증하지 않습니다. 제너레이터 재생(replay) 행은 기록해 둔 합성 명령 응답을 재생(replay)할 뿐, 영속 상태에서 완전히 다시 연결하는 시간을 재지는 않습니다. JIT의 잠재력을 알려면 워밍업, 컴파일, 메모리 비용을 포함한 실제 비교가 필요합니다.
