<!-- neograph-i18n: source=docs/QUICKJS_MIGRATION_COMPLETION.md locale=ko source_sha256=11b6f9b3c96b82c4bfc8364c832e675413d7d12bc8689c2ea6119295c4a6d278 -->
# QuickJS 마이그레이션 완료 런북

**Languages:** [English](QUICKJS_MIGRATION_COMPLETION.md) | [한국어](QUICKJS_MIGRATION_COMPLETION.ko.md) | [日本語](QUICKJS_MIGRATION_COMPLETION.ja.md) | [简体中文](QUICKJS_MIGRATION_COMPLETION.zh-CN.md)

**상태:** 릴리스 완료 절차로 제안됨. 남아 있는 게이트가 통과되었음을 증명하는 문서는 아님.

**일자:** 2026-08-11

**권한:** [QuickJS 제어 아키텍처](QUICKJS_CONTROL_ARCHITECTURE.md), [QuickJS 제어 런타임 마이그레이션 계획](QUICKJS_CONTROL_MIGRATION.md), 및 소유자 로컬 실행 가능 런타임 계약.

**실행 가능 동반 문서:** 소유자 로컬 마이그레이션 완료 계약.

---

## 1. 목적 및 확정 범위

이 런북은 남은 Q0 및 Q7 릴리스 게이트를 순서화된 실패 시 차단 절차로 전환한다. 아키텍처를 **변경하지 않는다**:

- JavaScript는 사용자가 작성하는 유일한 그래프/제어 언어로 유지된다;
- `GraphEngine`는 Core/애플리케이션 노드 실행기의 유일한 실행기로 유지된다;
- `ProgramRuntime`는 명령 승인(admission), 지속적 효과, 재생(replay), 취소, 및 자식 스케줄링의 단독 소유자로 유지된다;
- 엄격한 Core JSON은 표준 데이터로서 유지되며, 공개 소스 언어로 유지되지 않는다; 그리고
- 직접 C++ 구성은 신뢰할 수 있는 임베딩 API로 유지되며, 공개 Program JSON 연산 언어에 대한 호환성 변명으로 사용되지 않는다.

이 절차는 이슈 #35 (`trusted_direct`) 또는 지속 가능한 Promise 스케줄러를 시작하지 않는다. 어느 것도 제한된 지속 가능한 프로파일을 완료하기 위한 선행 조건이 아니며, 어느 것도 레거시 제거와 함께 번들될 수 없다.

## 2. 검증된 시작 지점

| 영역 | 현재 상태 | 증거 |
| --- | --- | --- |
| Q1–Q6 기본 런타임 및 작성(authoring) 전환 | 구현됨 | `quickjs-control-runtime.sdd.yaml` `completion_state`; JavaScript `define()`/generator 동작은 `tests/test_harness_program_cutover.cpp`에 포함됨. |
| Core DSL/elaborator 및 Harness DSL 작성 | 제거/거부됨 | 부모 사양의 `authoring_cutover_contract.completed_removals`에서; Harness 변환기는 레거시 모드를 거부함. |
| 레거시 스토리지 드레인 | 범위가 제한된 비배포 증명으로만 통과 | `docs/QUICKJS_CONTROL_MIGRATION.md` 증거를 기록합니다 `sha256:06f362…fd6217`; 이는 이후의 프로덕션 스냅샷을 대체하지 않습니다. |
| Linux QuickJS | 구현 및 실행됨 | Linux CI는 `NEOGRAPH_BUILD_QUICKJS_CONTROL`을 활성화함; `neograph_quickjs_tests`, ASan/UBSan, 및 사전 등록된 성능 매트릭스가 존재함. |
| macOS QuickJS | 부분적 | 설치된 공유 Program 소비자는 QuickJS를 활성화하지만, 일반 macOS 빌드/테스트 작업은 이를 활성화하지 않으며 정적 QuickJS 소비자 행(row)은 존재하지 않음. |
| Windows QuickJS | 자격이 없음 | `CMakeLists.txt`는 Windows에서 `NEOGRAPH_BUILD_QUICKJS_CONTROL=ON`를 의도적으로 거부합니다. Windows Program 소비자는 QuickJS가 비활성화된 상태로 실행됩니다. |
| Q7 소스/런타임 제거 | 시작 안 됨 | `SourceKind::CanonicalJson`, Program 문서 스키마, `ProgramPlan`, 레거시 컴파일러 파싱, 그리고 연산 트리 디스패처가 소스에 남아 있습니다. |

따라서 이번 릴리스는 Q7 삭제를 위한 준비가 **되지** 않았습니다. 플랫폼 자격 검증이 먼저 종료되어야 하며, 최종 드레인 증명은 실제 제거 경계에서 새로 확립되어야 합니다.

## 3. 모든 단계에 적용되는 규칙

1. **묵시적 범위 축소 금지.** Windows는 Q0 요구사항입니다. 지역적 `WIN32` 예외는 완료가 아닙니다. 해당 요구사항 제거는 구현 측 면제가 아닌 별도로 수용된 아키텍처 변경이 필요합니다.
2. **우발적인 공급업체 소스 드리프트 금지.** QuickJS 릴리스, 아카이브 다이제스트, 라이선스, 제외된 `quickjs-libc.c`, 그리고 비공개 심볼 네임스페이스를 명시적으로 유지하세요. 플랫폼 셤 또는 패치는 실행 시맨틱스를 변경하며 런타임 정체성 및 출처 증거에 포함되어야 합니다.
3. **컷오버 펜스 이후 레거시 폴백 금지.** 실패한 JavaScript 승인(admission)은 JSON, Core DSL 또는 기존 연산 트리 컴파일러를 선택해서는 안 됩니다.
4. **파일이름 기반 삭제 금지.** 일부 유지된 Program 유형은 정식 저장소이거나 신뢰받는 C++ 인프라입니다. 삭제 전에 각 의존성을 레거시 전용, JavaScript와 공유, 신뢰받는 C++과 공유로 분류하세요.
5. **소스 체크아웃 증명 금지.** 최종 저장소 증명은 동결된 전체 인벤토리 또는 정확한 무배포 증명에서 비롯되며, 빈 체크아웃이나 합성 CTest 픽스처에서 비롯되지 않습니다.
6. **모든 단계는 실패 시 차단됩니다.** 실패한 진입/종료 조건은 시퀀스를 중단합니다. 하드 게이트를 경고로 강등하거나 레거시 작성을 재개하지 않습니다.

## 4. 순서화된 완료 절차

### M0 — 릴리스 경계 및 증거 색인 동결 및 작성

**진입:** 승인된 상위 계약과 이 동반 명세가 존재한다.

1. 후보 커밋 하나와 전환(cutover) ID를 선택한다.
2. 소스 트리 외부에 변경 불가능한 증거 인덱스를 기록한다. 이 인덱스는 커밋, 컴파일러/툴체인, CMake 캐시/옵션, 대상 플랫폼/아키텍처, QuickJS 릴리스/아카이브 다이제스트/빌드 옵션, 아래의 모든 명령, 그리고 모든 결과 파일의 SHA-256 정체성을 바인딩해야 한다.
3. 측정 전에 지원 매트릭스를 기록한다: Linux x86_64 및 arm64, macOS, Windows x64; 각각은 상위 계약이 요구하는 곳에서 Core-only 및 QuickJS 지원 Program을 모두 포함해야 한다.
4. 기존 무배포 증명을 역사적 증거로만 보존한다. 이후 삭제 릴리스를 인증하는 데 2026-08-10 정체성을 사용하지 않는다.

**종료:** 후보와 증거 스키마는 변경 불가능하며, 증거 인덱스 항목 없이 다음 단계가 통과 결과를 보고하는 것은 허용되지 않는다.

### M1 — QuickJS 이식성 구현 완료

**진입:** M0 통과.

1. `NEOGRAPH_BUILD_QUICKJS_CONTROL=ON`가 Windows에서 QuickJS를 기본 활성화하거나 `quickjs-libc.c`, `std`, `os`, 동적 모듈 로더, 또는 공개 QuickJS 내보내기를 추가하지 않고 빌드되도록 한다.
2. 타이밍, 스택 한도, 할당자, 또는 컴파일러 호환성 적응은 검토된 NeoGraph 소유 계층에 배치한다. 벤더된 아카이브를 조용히 패치하지 않는다.
3. `JavaScriptRuntimeIdentity`/provenance를 확장하여 승인된 번들이 업스트림 아카이브 및 빌드 옵션뿐만 아니라 정확한 포트/심 구현을 식별하도록 한다. 이 정체성이 다르면 재생(replay)은 디스패치 전에 실패해야 한다.
4. `neograph_quickjs`를 비공개로 유지하고 정적 C 심볼에 접두사를 붙인다. 공유 `neograph::program`가 이를 내보내지 않으며 정적 소비자가 별도로 링크된 QuickJS와 충돌할 수 없음을 검증한다.
5. 최소 C 임베딩 스모크 실행 파일을 추가한다. 런타임/컨텍스트를 생성하고, 결정적 소스를 평가하고, 명시적 C 함수 하나를 바인딩하고, 인터럽트 및 메모리/스택 상한을 적용하고, `std`/`os`가 없음을 증명해야 한다.

**종료:** 지원되는 세 운영 체제 모두가 깨끗한 디렉터리에서 QuickJS 지원 및 Core-only 구성을 모두 구성하고 빌드할 수 있으며, 새 런타임 정체성이 기록된다.

### M2 — 빌드, 패키지, 설치된 소비자 매트릭스 검증

**진입:** M1 통과.

아래의 각 행에 대해 깨끗한 빌드 및 설치 디렉터리를 사용하십시오. 빌드 트리 전용 성공만으로는 충분하지 않습니다.

| 플랫폼 | QuickJS가 필요한 행 | Core 전용 필수 행 |
| --- | --- | --- |
| Linux x86_64 | Program 정적 + 공유; 런타임/Harness 테스트; C 스모크; 설치된 컨슈머 | 정적 + 공유 설치 컨슈머; QuickJS 링크/인터페이스/내보내기 증거 없음 |
| Linux arm64 | Program 정적 + 공유; 런타임/Harness 테스트; C 스모크; 설치된 컨슈머 | 정적 + 공유 설치 컨슈머 |
| macOS | Program 정적 + 공유; 런타임/Harness 테스트; C 스모크; 설치된 컨슈머 | 정적 + 공유 설치 컨슈머 |
| Windows x64 | MSVC를 사용한 Program 정적 + 공유; 런타임/Harness 테스트; C 스모크; 설치된 컨슈머 | 정적 + 공유 설치 컨슈머 |

설치된 QuickJS 소비자는 단순한 구문 거부가 아닌 설치된 패키지를 통한 **성공적인** `define()` 및 `function* main()` 게시/실행을 수행해야 합니다. 또한 기존의 독립적으로 링크된 두 번째 QuickJS 충돌 프로브를 유지해야 합니다. `scripts/test_find_package.sh`를 확장하거나(또는 이를 동등한 플랫폼 인식 드라이버로 대체) Windows에서 기본 검사 도구로 정적 심볼 네임스페이싱과 공유 내보내기 숨김을 검증하여 검사를 건너뛰지 않도록 하십시오.

**Exit:** all rows run at the installed prefix and record package metadata, loader/link closure, executable output, and private-symbol inspection results.

### M3 — 격리, 재생(replay), ABI, 및 해체 안전성 검증

**항목:** M2 통과.

1. 모든 활성화된 플랫폼에서 QuickJS 런타임 격리 코퍼스, JavaScript/Harness 엔드투엔드 테스트, 결정적 재생(replay)/복구 테스트, 장애-주입 테스트, 네이티브 ABI 적합성 테스트를 실행하세요.
2. Linux는 QuickJS를 활성화한 상태에서 ASan+UBSan 및 TSan을 실행해야 하며, macOS는 QuickJS를 활성화한 상태에서 지원되는 ASan/UBSan 등가물을 실행해야 한다. Windows는 QuickJS를 활성화한 상태에서 지원되는 MSVC AddressSanitizer 구성을 실행해야 한다. 사용할 수 없는 sanitizer를 실행했다고 주장하지 말고, 툴체인별 제한 사항을 증거 인덱스에 기록해야 한다.
3. 긴 JavaScript 평가 중 취소, 대기 중인 `callCore`에서의 중단, 할당자 종료, 취소 후 콜백 완료, 그리고 테스트 코퍼스에서 경쟁하는 두 번째 QuickJS 엔진을 포함합니다.
4. 소스/런타임/프로필/네이티브 바인딩 불일치가 디스패치 전에 실패하는지 확인하며, 프로세스 재시작 후에도 포함합니다.

**종료:** 모든 지원되는 샌티타이저/런타임 행이 NeoGraph 또는 QuickJS 소유권 결함을 숨기는 억제 없이 통과합니다. 거부된 각 픽스처는 제로 디스패치를 확인합니다.

### M4 — 성능, 시작, 할당, 바이너리 크기 게이트 종료

**진입:** M3 통과.

1. 기존 Linux 차단 매트릭스를 실행합니다:

   ```sh
   scripts/build_quickjs_performance_matrix.sh \
     <fresh-build-root> <evidence-root>/quickjs-performance-linux.json
   ```

승인된 사전 등록은 임계값 권위로 유지됩니다. 실패한 후보에 대응하여 임계값을 완화하지 마십시오.
2. macOS와 Windows를 측정하기 전에 시작, 할당, 활성화되었지만 사용되지 않음/Core 전용 측정에 대한 버전 관리된 사전 등록을 추가합니다. 단일 타이밍이 아닌 반복된 콜드/웜 분포를 기록합니다.
3. 각 플랫폼에 대해 설치된 Core-only 및 Program 바이너리 크기, 동적 종속성 클로저, 런타임 생성/시작 샘플, 할당자 최고 수위(high-water mark), 및 정확한 빌드 구성이 포함된 기계 판독 가능 자격 보고서를 생성합니다. Core-only 행에는 QuickJS 객체, 링크 종속성, 내보낸 기호, 할당 경로, 또는 QuickJS에 귀속될 수 있는 크기 증가가 없어야 합니다.
4. 실패하거나 노이즈가 있는 메트릭은 사전 등록된 방법으로 재실행될 때까지 실패한 게이트로 처리합니다. 회귀를 평균으로 희석하거나 다른 옵션 세트를 비교하지 마십시오.

**종료:** 각 차단 성능/크기/시작/할당 임계값을 통과하고 서명된 결과 ID가 증거 인덱스에 존재한다.

### M5 — 최종 스토리지 드레인을 재확립하고 삭제 경계를 인벤토리화함

**항목:** M4 통과; 레거시 게시/재개는 공지된 펜스 뒤에 있다.

1. 사전 출시 또는 프로덕션 배포가 한 번도 존재한 적이 없다면, 이 전환 ID에 대해 새로운 이름 있는 배포 없음 증명을 획득한다. 그 외에는 레거시 쓰기를 펜스 뒤에 배치하고 모든 지속적 Program 및 Harness 저장소의 일관된 읽기 전용 스냅샷을 캡처한다.
2. SQLite의 경우 실제 일관된 스냅샷을 사용하고 라이브 WAL/SHM/저널 사이드카를 거부한다. PostgreSQL의 경우 일반 사용자 지정 형식 `pg_dump`을 캡처한다. `PGDATA`을 절대 마운트하거나 라이브 데이터베이스를 감사하지 않는다.
3. 모든 저장소와 발견된 모든 레거시 아티팩트를 열거한다. 각각을 `translated`, `drain_only` 또는 `rejected`로 분류한다; `drain_only`, 활성/복구 가능 실행, 알 수 없는 결과, 활성 레거시 활성화 및 검사되지 않은 참조는 하드 차단 항목이다.
4. 삭제 직전에 실제 최종 증명을 실행한다:

   ```sh
   python3 scripts/audit_legacy_drain.py \
     --inventory <inventory.json> \
     --root <frozen-export-root> \
     --output <evidence-root>/legacy-drain-proof.json \
     --require-final
   ```

5. 별도로 소스 의존성 인벤토리를 생성한다. 레거시처럼 보이는 모든 유형/파일/테스트/스키마에 대해 `legacy_only`, `shared_with_javascript` 또는 `shared_with_trusted_cpp`로 레이블을 붙이고, 대체 또는 보존 사유를 명명하며, 회귀 테스트를 명명한다. 이름에서 이를 추론하지 않는다.

**Exit:** 감사관이 `final_drain.passed_is_true`을 보고한다; 증명과 소스 의존성 인벤토리가 모두 동일한 컷오버 ID에 바인딩된다; 레거시 쓰기 펜싱은 릴리스까지 활성 상태로 유지된다.

### M6 — 공유 의존성을 교체한 다음 레거시 작성/런타임 코드를 삭제

**Entry:** M5 통과.

1. 먼저 유지되는 신뢰된 C++ 호출 사이트를 Program 문서 JSON과 `ProgramPlan` 연산 트리 디스패처에서 빼낸다. 신뢰된 API는 프로세스 내에서 표준 데이터를 구성할 수 있지만, 두 번째 사용자 작성 제어 언어를 노출하거나 지속화해서는 안 된다.
2. 레거시 의존성을 분리하면서 JavaScript 명령 경로를 보존한다. 교체는 동일한 `ProgramRuntime` 승인(admission), 예산, 저널/효과, 취소 및 재생(replay) 불변 조건을 유지해야 한다. 구 경로를 제거하기 전에 해당 동작 회귀 테스트를 추가한다.
3. 별칭이나 파서 폴리백 없이 삭제한다:
   - Program 문서 v1~v4 스키마와 해당 공개 소스 라우트;
   - 저장된 아티팩트가 더 이상 요구하지 않으면 레거시 `CanonicalJson` 소스 디코딩;
   - 레거시 `ProgramPlan` 작업 트리 파싱, 하향 변환(lowering), 디스패치
   - Harness Program-JSON 번역 및 레거시 전용 예제/테스트/문서
   - 호환성 빌드/링크 의존성 및 유지되는 런타임 선택 코드
4. 의존성 인벤토리가 표준 저장소(스토리지)나 필수 신뢰 C++ 임베딩 인프라로 표시한 산출물만 유지할 것. 특히 `program` 네임스pace를 공유한다는 이유만으로 JavaScript 소스/번들/저널 형식 또는 Harness JavaScript 변환기를 삭제하지 말 것.
5. 드레인 감사 도구와 불변 증거를 유용한 경우 릴리스 증거로 유지하되, 배포 가능한 레거시 런타임 폴백을 보유해서는 안 됩니다.

**종료:** 어떤 신규 또는 저장된 실행 경로도 Program JSON 작업 트리를 파싱, 컴파일, 디스패치, 또는 폴백할 수 없습니다. 공개 작성 경계는 JavaScript 및 신뢰할 수 있는 C++ 임베딩만 노출합니다.

### M7 — 최종 클린룸 검증 및 릴리스 기록

Entry: M6 passed.

1. 삭제 후 새 디렉터리에서 M2~M4를 반복하고, 설치된 소비자 및 샌티타이저 빌드를 포함합니다. 호환성 체크아웃의 결과는 재사용할 수 없습니다.
2. 최종 트리에 대해 그래프 동등성, 스토리지 마이그레이션, 결정적 재생(replay), 크래시/오류, 네이티브 ABI, 및 JavaScript/Harness 스모크 테스트 제품군을 실행합니다.
3. 스냅샷 이후의 변경 사항이 레거시 상태를 승인, 재개 또는 변경했을 수 있다면 M5를 다시 실행하십시오. 그렇지 않으면 릴리스 증거 인덱스에서 검증된 증명 정체성을 유지하십시오.
4. 모든 증거가 수락된 후에만 `completion_state.legacy_runtime_removal`를 업데이트합니다. 그런 다음 동일한 릴리스 변경에 아키텍처, 마이그레이션 계획, 공개 경계, 변경 로그, 예제, 및 문제 상태를 업데이트합니다.
5. 증거 인덱스를 릴리스와 함께 게시하십시오. 롤백은 이전에 승인된 JavaScript 릴리스/활성화를 선택하거나 호환 가능한 바이너리를 복원합니다. Core DSL 또는 Program JSON 작성은 절대 다시 열지 않습니다.

**종료:** Q0 및 Q7 게이트가 완료되었습니다: 하나의 사용자 작성 언어, 하나의 정식 Core IR, 하나의 Program 지속성/효과 모델, 그리고 레거시 구현 폴백이 남아 있지 않습니다.

## 5. 삭제 인벤토리: 필수 구분

다음 현재 영역은 M5 중에 명시적 분류가 필요합니다. 이는 시작 체크리스트이며, 모두 무작정 제거할 수 있는 허가가 아닙니다.

| 영역 | 이름만으로 삭제할 수 없는 이유 |
| --- | --- |
| `include/neograph/program/plan.h`, `src/program/plan.cpp`, `compiler.cpp` 및 `run_attempt.cpp`의 레거시 부분 | 이들은 Program JSON 실행 트리를 구현하며 제거 대상 후보이지만, 호출자는 먼저 비레거시 신뢰된 C++ 또는 JavaScript 경로로 전환해야 합니다. |
| `SourceKind::CanonicalJson`, `ProgramSource::parse`, 번들/카탈로그/마이그레이션 호환성 분기 | ⟪P0a6c6b4b20bd0c6b0e4b2b0e4b4b0b0b4b0b0b4b0b0b4b0b0b0b-e작성 legacy 상태를 디코딩합니다. M5가 어떤 상태도 요구하지 않음을 증명한 후에만 제거될 수 있습니다. |
| `schemas/program-document-v1.schema.json`에서 `v4`까지 | 이들은 레거시 작성 스키마이며 공개 라우트와 함께 사라져야 합니다. |
| `program-source`, `program-bundle`, `program-version`, command-journal 및 엄격한 Core 형식 | 이들은 JavaScript 또는 신뢰할 수 있는 C++에 필요한 표준 저장소가 될 수 있으며 개별 보존 결정이 필요합니다. |
| `authoring.h`, JavaScript 소스/명령 코드, Harness 변환기 | They own the final JavaScript boundary and must be retained. |
| `tests/integration/find_package_program/main.cpp` | 현재 Program 문서를 담은 C++ 빌더를 실행 중입니다. 삭제 전에 최종 신뢰할 수 있는 C++ 또는 JavaScript 경로로 마이그레이션하세요. |
| `scripts/audit_legacy_drain.py` | 이는 런타임 폴백이 아닌 증거 도구입니다. 의도적으로 보존하거나 아카이브하세요. |

## 6. 완료 레코드

다음 단계의 진입 조건이 true이고, 종료 조건이 통과되었으며, 불변 인덱스에 그 증거가 추가된 경우에만 단계가 완료됩니다. 녹색 단위 스위트는 플랫폼, 설치된 소비자, 성능 또는 최종 드레인 게이트를 대체할 수 없습니다.
