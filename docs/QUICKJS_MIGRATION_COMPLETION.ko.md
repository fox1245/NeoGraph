<!-- neograph-i18n: source=docs/QUICKJS_MIGRATION_COMPLETION.md locale=ko source_sha256=6dec683c8c617650e19e61abb2a66a03ff7f0fbe738d65f37a7b12e18b95466a -->
# QuickJS 마이그레이션 완료 런북

**Languages:** [English](QUICKJS_MIGRATION_COMPLETION.md) | [한국어](QUICKJS_MIGRATION_COMPLETION.ko.md) | [日本語](QUICKJS_MIGRATION_COMPLETION.ja.md) | [简体中文](QUICKJS_MIGRATION_COMPLETION.zh-CN.md)

**상태:** 2026-08-11에 제안한 릴리스 완료 절차의 과거 기록입니다.
시작 상태 표와 순서가 정해진 게이트는 계획 기록이며, 현재 후보가 그 게이트를 통과했다는 주장이 아닙니다.

**날짜:** 2026-08-11

**기준 문서:** [QuickJS Control Architecture](QUICKJS_CONTROL_ARCHITECTURE.md),
[QuickJS Control Runtime Migration Plan](QUICKJS_CONTROL_MIGRATION.md), 그리고
소유자 로컬의 실행 가능한 런타임 계약.

**실행용 짝 문서:** 소유자 로컬의 마이그레이션 완료 계약.

---

## 1. 목적과 고정된 범위

이 런북은 남아 있는 Q0와 Q7 릴리스 게이트를, 순서가 정해져 있고 실패 시 차단으로 동작하는 절차로 정리합니다. 아키텍처는 **바꾸지 않습니다**.

- JavaScript는 사용자가 작성하는 유일한 그래프/제어 언어로 남습니다.
- `GraphEngine`은 Core/애플리케이션 노드를 실행하는 유일한 실행기로 남습니다.
- `ProgramRuntime`은 command 승인(admission), 영속 효과, 재생(replay), 취소, 자식 스케줄링을 단독으로 소유합니다.
- 엄격한 Core JSON은 공개 소스 언어가 아니라 정규 데이터로 남습니다.
- C++ 직접 구성은 신뢰할 수 있는 임베딩 API로 남으며, 공개 Program JSON 연산 언어를 호환성 명목으로 유지할 구실이 되지 못합니다.

이 절차에서는 이슈 #35(`trusted_direct`)나 영속 Promise 스케줄러 작업을 시작하지 않습니다. 둘 다 제한된 영속 프로파일을 완료하기 위한 선행 조건이 아니며, 레거시 제거와 묶어서 진행해서도 안 됩니다.

## 2. 과거에 검증한 출발점

| 영역 | 2026-08-11에 기록한 상태 | 증거 |
| --- | --- | --- |
| Q1–Q6 기본 런타임과 작성 방식 전환(cutover) | 구현됨 | `quickjs-control-runtime.sdd.yaml`의 `completion_state`. JavaScript `define()`/generator 동작은 `tests/test_harness_program_cutover.cpp`가 검증함. |
| Core DSL/elaborator와 Harness DSL 작성 | 제거/거부됨 | 상위 스펙의 `authoring_cutover_contract.completed_removals`. Harness 변환기는 레거시 모드를 거부함. |
| 레거시 스토리지 drain | 범위가 한정된 "배포 없음(no-deployment)" 증명으로만 통과함 | `docs/QUICKJS_CONTROL_MIGRATION.md`에 증명 `sha256:06f362…fd6217`이 기록되어 있음. 이후의 프로덕션 스냅샷을 대신할 수는 없음. |
| Linux QuickJS | 구현 및 실행 확인됨 | Linux CI가 `NEOGRAPH_BUILD_QUICKJS_CONTROL`을 켬. `neograph_quickjs_tests`, ASan/UBSan, 사전 등록(preregistered)된 성능 매트릭스가 있음. |
| macOS QuickJS | 부분 지원 | 설치된 공유 Program 소비자는 QuickJS를 켜지만, 일반 macOS 빌드/테스트 작업은 켜지 않으며 정적 QuickJS 소비자 행도 없음. |
| Windows QuickJS | 자격 검증 안 됨 | `CMakeLists.txt`가 Windows에서 `NEOGRAPH_BUILD_QUICKJS_CONTROL=ON`을 의도적으로 거부함. Windows Program 소비자는 QuickJS를 끈 채로 실행됨. |
| Q7 소스/런타임 제거 | 시작 전 | `SourceKind::CanonicalJson`, Program 문서 스키마, `ProgramPlan`, 레거시 컴파일러 파싱, 연산 트리 디스패처가 소스에 남아 있음. |

따라서 이 릴리스는 Q7 삭제를 진행할 준비가 **되어 있지 않습니다**. 먼저 플랫폼 자격 검증을 끝내야 하고, 최종 drain 증명은 실제로 제거하는 시점에 새로 만들어야 합니다.

위 과거 표에 있던 Windows 소스 제한은 그 뒤 NeoGraph가 소유하는 MSVC 빌드 오버레이로 대체되었고, 이 오버레이는 `ProgramSource` 런타임 식별자에 고정됩니다. 소스 수준의 지원이 곧 플랫폼 자격 검증은 아닙니다. 유지 중인 정규 스토리지와, 신뢰할 수 있는 C++의 타입 있는 연산 인프라는 여전히 남아 있습니다. 공개 JSON 작성 방식을 없앴다고 해서 공유 디스패처와 저장된 스키마가 모두 삭제된 것은 아닙니다.

## 3. 모든 단계에 적용되는 규칙

1. **범위를 몰래 줄이지 않습니다.** Windows는 Q0 요구 사항입니다. 로컬의 `WIN32` 예외 처리는 완료로 인정하지 않습니다. 이 요구 사항을 없애려면 구현 쪽의 면제가 아니라, 따로 승인받은 아키텍처 변경이 필요합니다.
2. **vendor 소스가 모르는 사이에 달라지지 않게 합니다.** QuickJS 릴리스, 아카이브 다이제스트, 라이선스, 제외한 `quickjs-libc.c`, 비공개 심볼 네임스페이스를 명시적으로 유지합니다. 플랫폼 shim이나 패치는 실행 의미를 바꾸므로 런타임 식별자와 출처(provenance) 증거에 반영해야 합니다.
3. **cutover 펜스 이후에는 레거시로 되돌아가지 않습니다.** JavaScript 승인(admission)에 실패하더라도 JSON, Core DSL, 예전 연산 트리 컴파일러를 선택해서는 안 됩니다.
4. **파일 이름만 보고 삭제하지 않습니다.** 유지 중인 Program 타입 중에는 정규 스토리지나 신뢰할 수 있는 C++ 인프라인 것이 있습니다. 삭제하기 전에 각 의존성을 legacy-only, shared-with-JavaScript, shared-with-trusted-C++ 중 하나로 분류합니다.
5. **소스 체크아웃은 증명이 되지 못합니다.** 최종 스토리지 증명은 동결한 전체 인벤토리 또는 정확한 "배포 없음" 확인서(attestation)에서 얻으며, 빈 체크아웃이나 합성 CTest 픽스처로는 만들지 않습니다.
6. **모든 단계는 실패 시 차단으로 동작합니다.** 진입/종료 조건이 하나라도 실패하면 순서가 멈춥니다. 필수 게이트를 경고로 낮추거나 레거시 작성 방식을 다시 여는 일은 없습니다.

## 4. 완료 절차 (순서대로)

### M0 — 릴리스 경계 고정과 증거 색인 작성

**진입 조건:** 승인된 상위 계약과 이 짝 스펙이 있어야 합니다.

1. 후보 커밋 하나와 cutover ID를 고릅니다.
2. 소스 트리 밖에 변경할 수 없는 증거 색인을 기록합니다. 이 색인에는 커밋, 컴파일러/툴체인, CMake 캐시/옵션, 대상 플랫폼/아키텍처, QuickJS 릴리스/아카이브 다이제스트/빌드 옵션, 아래의 모든 명령, 모든 결과 파일의 SHA-256 식별자가 묶여 있어야 합니다.
3. 측정하기 전에 지원 매트릭스를 기록합니다. Linux x86_64와 arm64, macOS, Windows x64이며, 상위 계약이 요구하는 경우 각 플랫폼은 Core 전용 구성과 QuickJS를 켠 Program 구성을 모두 다뤄야 합니다.
4. 기존의 "배포 없음" 증명은 과거 증거로만 보존합니다. 2026-08-10의 식별자로 이후 삭제 릴리스를 인증해서는 안 됩니다.

**종료 조건:** 후보와 증거 스키마가 변경 불가능한 상태가 되어야 하며, 증거 색인 항목이 없으면 다음 단계는 통과 결과를 보고할 수 없습니다.

### M1 — QuickJS 이식성 구현 완료

**진입 조건:** M0을 통과해야 합니다.

1. Windows에서도 `NEOGRAPH_BUILD_QUICKJS_CONTROL=ON`으로 빌드되게 합니다. 이때 QuickJS를 기본으로 켜거나 `quickjs-libc.c`, `std`, `os`, 동적 모듈 로더, 공개 QuickJS export를 추가해서는 안 됩니다.
2. 타이밍, 스택 한도, 할당자, 컴파일러 호환을 위한 수정은 검토를 거친 NeoGraph 소유 계층에 둡니다. vendor 아카이브를 몰래 패치해서는 안 됩니다.
3. `JavaScriptRuntimeIdentity`와 출처(provenance)를 확장해, 승인(admission)된 번들이 upstream 아카이브와 빌드 옵션뿐 아니라 정확한 포팅/shim 구현도 식별하게 합니다. 이 식별자가 다르면 재생(replay)은 디스패치 전에 실패해야 합니다.
4. `neograph_quickjs`는 비공개로 유지하고 정적 C 심볼에 접두사를 붙입니다. 공유 `neograph::program`이 그 심볼을 export하지 않는지, 정적 소비자가 따로 링크된 QuickJS와 충돌하지 않는지 확인합니다.
5. 최소한의 C 임베딩 스모크 실행 파일을 추가합니다. 이 파일은 런타임/컨텍스트를 만들고, 결정적인 소스를 평가하고, 명시적인 C 함수 하나를 바인딩하고, 인터럽트와 메모리/스택 상한을 강제하고, `std`/`os`가 없음을 증명해야 합니다.

**종료 조건:** 지원하는 세 운영체제 모두에서 깨끗한 디렉터리로 QuickJS를 켠 구성과 Core 전용 구성을 모두 configure하고 빌드할 수 있어야 하며, 새 런타임 식별자가 기록되어 있어야 합니다.

### M2 — 빌드, 패키지, 설치 소비자 매트릭스 자격 검증

**진입 조건:** M1을 통과해야 합니다.

아래의 모든 행에서 깨끗한 빌드 디렉터리와 설치 디렉터리를 사용합니다. 빌드 트리에서만 성공한 것으로는 충분하지 않습니다.

| 플랫폼 | 필수: QuickJS 사용 행 | 필수: Core 전용 행 |
| --- | --- | --- |
| Linux x86_64 | Program 정적 + 공유, 런타임/Harness 테스트, C 스모크, 설치 소비자 | 정적 + 공유 설치 소비자, QuickJS 링크/인터페이스/export가 없다는 증거 |
| Linux arm64 | Program 정적 + 공유, 런타임/Harness 테스트, C 스모크, 설치 소비자 | 정적 + 공유 설치 소비자 |
| macOS | Program 정적 + 공유, 런타임/Harness 테스트, C 스모크, 설치 소비자 | 정적 + 공유 설치 소비자 |
| Windows x64 | MSVC로 빌드한 Program 정적 + 공유, 런타임/Harness 테스트, C 스모크, 설치 소비자 | 정적 + 공유 설치 소비자 |

설치된 QuickJS 소비자는 문법 오류를 거부하는 데 그치지 않고, 설치된 패키지로 `define()`과 `function* main()`을 **성공적으로** 게시하고 실행해야 합니다. 독립적으로 링크한 두 번째 QuickJS와의 충돌 프로브도 기존대로 유지해야 합니다. 설치 소비자 드라이버는 `python scripts/verify_ci.py install --work-dir <fresh-output> --jobs 2 --program --quickjs`입니다(공유 라이브러리는 `--shared`를 추가). 이 드라이버의 비공개 심볼 검사는 지금도 ELF/Mach-O에서만 동작하며, 현재 Windows 설치 행에서는 `--quickjs`를 생략합니다. 이 완료 기준을 충족하려면 Windows의 정적 네임스페이스 처리와 공유 라이브러리의 export 숨김에 대해서도 네이티브 검사 증거가 필요합니다. 네이티브 런타임/C ABI 테스트로는 이를 대신할 수 없습니다.

**종료 조건:** 모든 행이 설치된 prefix에서 실행되어야 하며, 패키지 메타데이터, 로더/링크 closure, 실행 출력, 비공개 심볼 검사 결과를 기록해야 합니다.

### M3 — 격리, 재생(replay), ABI, 해제(teardown) 안전성 자격 검증

**진입 조건:** M2를 통과해야 합니다.

1. QuickJS를 켠 모든 플랫폼에서 QuickJS 런타임 격리 코퍼스, JavaScript/Harness 종단 간 테스트, 결정적 재생(replay)/복구 테스트, 장애 주입 테스트, 네이티브 ABI 적합성 테스트를 실행합니다.
2. Linux에서는 QuickJS를 켠 채로 ASan+UBSan과 TSan을 실행해야 합니다. macOS에서는 지원되는 ASan/UBSan 대응 도구를, Windows에서는 지원되는 MSVC AddressSanitizer 구성을 QuickJS를 켠 채로 실행해야 합니다. 쓸 수 없는 sanitizer를 실행했다고 주장해서는 안 되며, 툴체인별 제약은 증거 색인에 기록합니다.
3. 테스트 코퍼스에 다음을 포함합니다. 오래 걸리는 JavaScript 평가 도중의 취소, 대기 중인 `callCore`에서의 중단, 할당자 해제, 취소 이후의 콜백 완료, 경쟁하는 두 번째 QuickJS 엔진.
4. 소스/런타임/프로파일/네이티브 바인딩이 일치하지 않으면 프로세스를 재시작한 뒤에도 디스패치 전에 실패하는지 확인합니다.

**종료 조건:** 지원되는 sanitizer/런타임 행이 모두 통과해야 하며, NeoGraph나 QuickJS의 소유권 결함을 가리는 suppression을 두어서는 안 됩니다. 거부된 픽스처는 모두 디스패치가 0건임을 확인해야 합니다.

### M4 — 성능, 시작 시간, 할당, 바이너리 크기 게이트 마감

**진입 조건:** M3을 통과해야 합니다.

1. 기존의 Linux 차단(blocking) 매트릭스를 실행합니다.

   ```sh
   python scripts/verify_ci.py quickjs-performance --work-dir <fresh-build-root> --jobs 4
   ```

   새로 만든, 존재하지 않는 소유 빌드 루트를 쓰고, 소스 루트에는 실제 Git 체크아웃을 사용합니다. 변경하지 않은 `run_quickjs_performance.py`가 커밋/dirty 출처를 요구하므로, 무관한 체크아웃 아래에 풀어 놓은 아카이브로는 충분하지 않습니다. QuickJS를 끈 빌드와 켠 빌드 한 쌍과 그 결과는 이 루트 아래에 남으며, `<fresh-build-root>/quickjs-performance.json`을 비공개 증거 색인에 보관합니다.

   이 매트릭스의 승인된 사전 등록(preregistration)이 임계값의 기준입니다. 후보가 실패한다고 해서 임계값을 완화해서는 안 됩니다.
2. macOS와 Windows를 측정하기 전에, 시작 시간, 할당, 켰지만 쓰지 않는 구성/Core 전용 구성의 측정을 위한 사전 등록을 버전 관리하에 추가합니다. 한 번 잰 값이 아니라 콜드/웜 분포를 반복 측정해 기록합니다.
3. 플랫폼마다 기계가 읽을 수 있는 자격 검증 보고서를 만듭니다. 보고서에는 설치된 Core 전용 바이너리와 Program 바이너리의 크기, 동적 의존성 closure, 런타임 생성/시작 샘플, 할당자 최고 사용량(high-water mark), 정확한 빌드 구성이 들어가야 합니다. Core 전용 행에는 QuickJS 오브젝트, 링크 의존성, export된 심볼, 할당 경로가 없어야 하고 QuickJS로 인한 크기 증가도 없어야 합니다.
4. 실패했거나 노이즈가 큰 지표는 사전 등록된 방법으로 다시 실행하기 전까지 실패한 게이트로 취급합니다. 평균을 내서 회귀를 없애거나, 옵션이 다른 구성끼리 비교해서는 안 됩니다.

**종료 조건:** 차단 대상인 성능/크기/시작 시간/할당 임계값이 모두 통과해야 하며, 서명된 결과 식별자가 증거 색인에 있어야 합니다.

### M5 — 최종 스토리지 drain 재확립과 삭제 경계 목록화

**진입 조건:** M4를 통과해야 하며, 레거시 게시/재개가 공지한 펜스 뒤에 있어야 합니다.

1. 프리릴리스나 프로덕션 배포가 한 번도 없었다면, 이 cutover ID에 대해 이름이 명시된 새 "배포 없음" 확인서를 받습니다. 그렇지 않으면 레거시 쓰기를 펜스 뒤로 옮기고, 모든 영속 Program 저장소와 Harness 저장소의 일관된 읽기 전용 스냅샷을 만듭니다.
2. SQLite는 실제로 일관된 스냅샷을 쓰고, 살아 있는 WAL/SHM/저널 사이드카는 거부합니다. PostgreSQL은 일반적인 custom 형식의 `pg_dump`를 받으며, `PGDATA`를 마운트하거나 실행 중인 데이터베이스를 감사해서는 안 됩니다.
3. 모든 저장소와, 발견한 모든 레거시 아티팩트를 열거합니다. 각각을 `translated`, `drain_only`, `rejected` 중 하나로 분류합니다. `drain_only`, 활성 또는 복구 가능한 run, 알 수 없는 결과, 활성 상태의 레거시 activation, 검사하지 않은 참조는 반드시 해결해야 하는 차단 요인입니다.
4. 삭제 직전에 실제 최종 증명을 실행합니다.

   ```sh
   python3 scripts/audit_legacy_drain.py \
     --inventory <inventory.json> \
     --root <frozen-export-root> \
     --output <evidence-root>/legacy-drain-proof.json \
     --require-final
   ```

5. 소스 의존성 인벤토리를 따로 만듭니다. 레거시처럼 보이는 모든 타입/파일/테스트/스키마에 `legacy_only`, `shared_with_javascript`, `shared_with_trusted_cpp` 중 하나의 레이블을 붙이고, 대체물이나 유지하는 이유와 회귀 테스트 이름을 적습니다. 이름만 보고 추정해서는 안 됩니다.

**종료 조건:** 감사 도구가 `final_drain.passed_is_true`를 보고해야 합니다. 증명과 소스 의존성 인벤토리는 같은 cutover ID에 묶여야 하고, 레거시 쓰기 펜스는 릴리스가 끝날 때까지 유지되어야 합니다.

### M6 — 공유 의존성 교체 후 레거시 작성/런타임 코드 삭제

**진입 조건:** M5를 통과해야 합니다.

1. 먼저, 유지 중인 신뢰할 수 있는 C++ 호출부가 Program 문서 JSON과 `ProgramPlan` 연산 트리 디스패처를 쓰지 않도록 옮깁니다. 신뢰할 수 있는 API가 프로세스 안에서 정규 데이터를 구성할 수는 있지만, 사용자가 작성하는 두 번째 제어 언어를 노출하거나 저장해서는 안 됩니다.
2. 레거시 의존성을 끊는 동안 JavaScript command 경로는 그대로 유지합니다. 대체 구현은 `ProgramRuntime`의 승인(admission), 예산, 저널/효과, 취소, 재생(replay) 불변식을 똑같이 지켜야 합니다. 예전 경로를 지우기 전에 이 동작에 대한 회귀 테스트를 먼저 추가합니다.
3. 별칭이나 파서 fallback을 남기지 않고 다음을 삭제합니다.
   - Program 문서 v1–v4 스키마와 그 공개 소스 경로
   - 저장된 아티팩트 중 필요로 하는 것이 없어진 뒤의 레거시 `CanonicalJson` 소스 디코딩
   - 레거시 `ProgramPlan` 연산 트리의 파싱, lowering, 디스패치
   - Harness의 Program JSON 변환, 레거시 전용 예제/테스트/문서
   - 호환용 빌드/링크 의존성과 남아 있는 런타임 선택 코드
4. 의존성 인벤토리에서 정규 스토리지나 필수 신뢰 C++ 임베딩 인프라로 표시된 아티팩트만 남깁니다. 특히 JavaScript 소스/번들/저널 형식이나 Harness JavaScript 변환기를 단지 `program` 네임스페이스를 공유한다는 이유로 삭제해서는 안 됩니다.
5. drain 감사 도구와 변경 불가능한 증명은 필요하면 릴리스 증거로 보관해도 되지만, 배포 가능한 레거시 런타임 fallback을 남겨서는 안 됩니다.

**종료 조건:** 새 실행 경로이든 저장된 실행 경로이든 Program JSON 연산 트리를 파싱, 컴파일, 디스패치하거나 그쪽으로 fallback할 수 없어야 하며, 공개 작성 경계에는 JavaScript와 신뢰할 수 있는 C++ 임베딩만 노출되어야 합니다.

### M7 — 최종 클린룸 검증과 릴리스 기록

**진입 조건:** M6을 통과해야 합니다.

1. 삭제한 뒤에 새 디렉터리에서 M2–M4를 다시 수행하며, 설치 소비자와 sanitizer 빌드도 포함합니다. 호환용 체크아웃의 결과를 재사용해서는 안 됩니다.
2. 최종 트리에 대해 그래프 동등성, 스토리지 마이그레이션, 결정적 재생(replay), 크래시/장애, 네이티브 ABI, JavaScript/Harness 스모크 스위트를 실행합니다.
3. 스냅샷 이후의 변경 중 레거시 상태를 승인(admission)하거나 재개하거나 바꿀 수 있는 것이 있었다면 M5를 다시 실행합니다. 그렇지 않으면 검증된 증명의 식별자를 릴리스 증거 색인에 그대로 보존합니다.
4. 모든 증거가 승인된 뒤에만 `completion_state.legacy_runtime_removal`을 갱신합니다. 그다음 같은 릴리스 변경에서 아키텍처, 마이그레이션 계획, 공개 경계, changelog, 예제, 이슈 상태를 갱신합니다.
5. 증거 색인을 릴리스와 함께 공개합니다. 롤백은 이전에 승인(admission)된 JavaScript 릴리스/activation을 선택하거나 호환되는 바이너리를 복원하는 방식으로 하며, Core DSL이나 Program JSON 작성 방식을 다시 여는 일은 없습니다.

**종료 조건:** Q0와 Q7 게이트가 모두 완료되어야 합니다. 사용자가 작성하는 언어는 하나, 정규 Core IR도 하나, Program 영속성/효과 모델도 하나이고, 레거시 구현으로 돌아가는 fallback은 남지 않습니다.

## 5. 삭제 목록: 반드시 구분해야 할 것

현재 다음 영역은 M5에서 명시적으로 분류해야 합니다. 이 목록은 출발점이 되는 체크리스트이며, 전부 무작정 삭제해도 된다는 허가가 아닙니다.

| 영역 | 이름만으로 삭제할 수 없는 이유 |
| --- | --- |
| `include/neograph/program/plan.h`, `src/program/plan.cpp`, `compiler.cpp`와 `run_attempt.cpp`의 레거시 부분 | Program JSON 연산 트리를 구현하므로 삭제 후보이지만, 호출부가 먼저 레거시가 아닌 신뢰 C++ 경로나 JavaScript 경로로 옮겨 가야 함 |
| `SourceKind::CanonicalJson`, `ProgramSource::parse`, 번들/카탈로그/마이그레이션의 호환용 분기 | 유지 중인 레거시 상태를 디코딩함. M5에서 이 코드가 필요한 상태가 없음을 증명한 뒤에만 삭제할 수 있음 |
| `schemas/program-document-v1.schema.json`부터 `v4`까지 | 레거시 작성용 스키마이며 공개 경로와 함께 없어져야 함 |
| `program-source`, `program-bundle`, `program-version`, command-journal, 엄격한 Core 형식 | JavaScript나 신뢰 C++가 필요로 하는 정규 스토리지일 수 있으므로 하나씩 유지 여부를 결정해야 함 |
| `authoring.h`, JavaScript 소스/command 코드, Harness 변환기 | 최종 JavaScript 경계를 담당하므로 유지해야 함 |
| `tests/integration/find_package_program/main.cpp` | 현재 Program 문서를 담는 C++ 빌더를 실행해 보는 테스트이므로, 삭제 전에 최종 신뢰 C++ 경로나 JavaScript 경로로 옮겨야 함 |
| `scripts/audit_legacy_drain.py` | 증거 수집용 도구이지 런타임 fallback이 아님. 유지할지 보관할지 의도적으로 정해야 함 |

## 6. 완료 기록

단계는 그 증거가 변경 불가능한 색인에 추가되었고, 종료 조건을 통과했으며, 다음 단계의 진입 조건이 충족되었을 때에만 완료로 봅니다. 단위 테스트가 모두 통과했다고 해서 플랫폼, 설치 소비자, 성능, 최종 drain 게이트를 대신할 수는 없습니다.
