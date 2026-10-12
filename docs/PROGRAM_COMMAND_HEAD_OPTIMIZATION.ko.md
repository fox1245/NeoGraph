<!-- neograph-i18n: source=docs/PROGRAM_COMMAND_HEAD_OPTIMIZATION.md locale=ko source_sha256=52b5dae9fe5c514792e6ec85508f8e548faef3d737711c484cd04294fa3542ef -->
# Command 게시 head 최적화 — 2026-09-07

**Languages:** [English](PROGRAM_COMMAND_HEAD_OPTIMIZATION.md) | [한국어](PROGRAM_COMMAND_HEAD_OPTIMIZATION.ko.md) | [日本語](PROGRAM_COMMAND_HEAD_OPTIMIZATION.ja.md) | [简体中文](PROGRAM_COMMAND_HEAD_OPTIMIZATION.zh-CN.md)

이 변경은 JavaScript 명령을 게시하기 전의 읽기 횟수를 줄입니다. run/저널을 따로 읽고 전체 명령 이력을 구체화하는 대신, run, 저널, 가장 최근 명령을 한꺼번에 읽습니다. 영속 게시, CAS, 예약, 체크포인트, 복구의 의미는 바뀌지 않습니다.

## 구현

- `ProgramTransitionStore::load_command_publication_head`는 서로 일관된 run/저널 쌍과 가장 최근에 추가된 명령을 반환합니다. memory 저장소는 불변 스냅샷 하나를 쓰고, SQLite와 PostgreSQL은 쿼리 한 번과 기존 기본 키 `(owner_scope, run_id, sequence)`를 씁니다.
- 데이터베이스 프로젝션은 쓰지 않는 마이그레이션 바이트와 마지막 게시 바이트를 가져오지 않습니다. 가장 최근 명령의 시퀀스와 좌표는 정규 값과 대조해 확인하고, run/소유자/번들/저널 binding도 검증합니다.
- 일반적인 추가와 정산은 가장 최근 명령을 사용합니다. 더 오래된 좌표의 재시도는 여전히 전체 이력을 사용합니다. 재생(replay)과 저장소 쪽의 추가/예약 검증도 이전처럼 이력을 살펴보므로, 이력 스캔을 완전히 없앤 것은 아닙니다.
- 기존 C++ 저장소 래퍼에는 이미 있는 가상 읽기 메서드를 쓰는 대체 구현이 제공되며, run을 한 번 더 읽어 앞뒤를 확인(fence)합니다. 그 사이에 동시 변경이 있으면 실패 시 차단으로 처리합니다. 래퍼는 자체 필터링과 권한 의미를 유지한 채 새 읽기를 직접 구현할 수도 있습니다.
- 스키마 마이그레이션, 새 인덱스, 캐시 수명, 영속성 설정, 그래프 세대, 컴파일러 식별자, 모델 설정은 바뀌지 않았습니다.
- 공개 C++ 가상 인터페이스에 메서드가 하나 추가되었으므로 Program 라이브러리와 C++ 사용 측을 함께 다시 빌드해야 합니다. 기존 래퍼와의 소스 호환성은 확인했지만, 미리 빌드된 C++ 사용 측은 ABI 호환이 되지 않는 것으로 가정합니다.

## 정확성

WSL GCC Debug 빌드의 Program 테스트는 **680개가 통과했고**, 해당하지 않는 **memory 저장소 프로세스 재시작 케이스 2개는 건너뛰었습니다**. 추가한 테스트 7개와 보강한 백엔드 이력 테스트는 불변 스냅샷, 독립된 reader/writer 연결, 대체 구현 래퍼, 대체 읽기 도중 값이 어긋나는 경우(torn read), 선택된 꼬리(tail)의 손상, 범위가 제한된 tail 읽기를 다룹니다. 과거 이력의 손상은 전체 이력 읽기가 여전히 거부합니다.

전체 테스트는 명령 복구, 계보 CAS, 재귀 자식 교체, 갱신할 수 없는 예산, SQLite/PostgreSQL에서의 실제 프로세스 유실 경계도 검증했습니다. 이 변경에 대해 Windows나 sanitizer 재실행은 하지 않았습니다.

## 쌍별 Release 비교

기준선과 같은 Ryzen 7 5800X / WSL2 Ubuntu 24.04 / GCC 13.3 Release 구성입니다. 비교하기 전에 두 바이너리를 모두 고정했습니다. 케이스마다 독립된 프로세스 쌍 5개를 쓰고, 프로세스마다 워밍업 3번과 측정 12번을 호출합니다. 케이스와 AB/BA 순서는 번갈아 바꿨습니다. PostgreSQL은 WSL의 네이티브 Docker와 프로세스마다 새로 만든 데이터베이스를 쓰고, SQLite는 WSL ext4에 새로 만든 파일을 씁니다. 쌍별 매트릭스를 실행하는 동안 빌드나 다른 테스트 작업은 돌지 않았습니다.

변경 전 바이너리의 해시는 원래 기준선과 같습니다. 아래의 변경 전/후 측정값은 이번에 함께 측정한 것이어서, 예전 벽시계 시간과 비교한 결과보다 우선합니다. JIT나 LLM은 관여하지 않습니다.

| 케이스 | 변경 전 중앙값 (ms) | 변경 후 중앙값 (ms) | 쌍별 감소율 중앙값 | 쌍별 변경 후/변경 전 범위 |
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

감소율은 쌍으로 묶은 프로세스 중앙값에서 계산하므로, 독립적으로 요약한 두 중앙값의 비율과 같지 않을 수 있습니다. 쌍별 산포 안에 들어오는 작은 차이로는 결론을 낼 수 없습니다. direct Core와 C++ Program 행은 음성 대조군이며, 최적화한 명령 게시 읽기를 쓰지 않는 경로입니다. 작은 payload의 memory 케이스에서는 뚜렷한 속도 향상을 확인하지 못했습니다.

direct Core에서 작은 신호가 보여, 범위를 넓힌 대조 검사를 한 번 더 했습니다. 프로세스 쌍 7개에 각각 워밍업 100번, 측정 direct 호출 10,000번입니다. 변경 후/변경 전 비율의 중앙값은 1.0098이고 쌍별 범위는 0.9825에서 1.0123이었습니다. 원시 결과는 `direct-control/`에 보관합니다. Core 소스나 라이브러리 코드는 바꾸지 않았으며, 이 검사로 Core 성능에 의미 있는 변화가 있었다고 볼 수는 없습니다.

## 데이터베이스 API 호출 횟수

계측을 넣은 별도의 비교에서는 호출 1번과 4번의 차이를 구하고 이를 두 번 반복했습니다. run당 호출 횟수의 차이는 두 번의 반복에서 모두 같았습니다. 이 계측에서 얻은 시간은 대표 비교에 사용하지 않습니다. SQLite step 호출에는 행 순회가 포함되며, libpq는 동기 호출만 다룹니다.

| 백엔드 / Program run당 Core 호출 수 | 변경 전 SQL API 호출 | 변경 후 SQL API 호출 | 변경 전 커밋 | 변경 후 커밋 |
| --- | --- | --- | --- | --- |
| sqlite / 1 | 135 | 130 | 9 | 9 |
| sqlite / 16 | 2715 | 2155 | 114 | 114 |
| postgres / 1 | 108 | 104 | 4 | 4 |
| postgres / 16 | 1053 | 989 | 34 | 34 |

PostgreSQL의 커밋 열은 동기 libpq 호출 중 일부일 뿐이며, 비동기 체크포인트 경로를 포함한 전체 트랜잭션 수가 아닙니다. 두 바이너리는 같은 영속화 인터페이스를 실행하며, 측정한 범위에서 커밋 횟수도 같습니다.

## 재현 방법과 증거

변경 전 리비전(`8fce5e14`)과 변경된 리비전에서 `bench_program_cost`를 같은 Release 옵션으로 빌드하고, 두 바이너리를 모두 보관합니다. 일회용 Postgres 컨테이너가 실행 중이고 `NEOGRAPH_COST_POSTGRES_URL`이 설정된 상태에서 다음을 실행합니다.

```sh
python3 scripts/compare_program_costs.py \
  --before /tmp/before/bench_program_cost \
  --after /tmp/after/bench_program_cost \
  --output /tmp/new-command-head-comparison \
  --postgres-container neograph-n2-postgres
```

자세한 방법은 [Program 비용 측정](PROGRAM_COST_MEASUREMENT.md)에 있습니다. 쌍별 원시 샘플, 호출 횟수, 소스/바이너리 해시, 검증 로그는 로컬의 `artifacts/program-command-head-20260907/`에 보관하며, 큰 SQLite 파일은 WSL에 남겨 두었습니다. 러너는 출력 디렉터리를 덮어쓰거나 기존 애플리케이션 데이터베이스를 재사용하지 않습니다.

다음으로 남은 대상은 게시와 재생(replay) 중의 전체 이력 검증/구체화, 그리고 정규 레코드 구성입니다. 이 변경이 그 비용을 없앴다거나 JIT가 도움이 된다는 것을 보여 주지는 않습니다.
