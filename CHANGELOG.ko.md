<!-- neograph-i18n: source=CHANGELOG.md locale=ko source_sha256=7532000a606d31bd33b572489fe0c8ae482c0497c04cdf645187a820824f1258 -->
# 변경 로그

**Languages:** [English](CHANGELOG.md) | [한국어](CHANGELOG.ko.md) | [日本語](CHANGELOG.ja.md) | [简体中文](CHANGELOG.zh-CN.md)

NeoGraph에 대한 모든 주요 변경 사항은 이 파일에 기록됩니다.

형식은 [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)를 따릅니다. 버전 관리는 [Semantic Versioning](https://semver.org/spec/v2.0.0.html)를 따릅니다.

---

## [Unreleased]

### 추가됨
- **Program 기반 A2A 서비스의 엄격 인증.** Program A2A 생성자에 선택적
  `require_authenticated_requests` 플래그를 추가했습니다. 활성화하면 협업
  envelope뿐 아니라 일반 message, stream, task 조회 및 취소 RPC에도 설정된
  authenticator를 적용합니다. 기존 호환성을 위해 기본값은 false입니다.
- **호스트 소유 A2A 제어 메시지 가로채기.** `ProgramAgentAdapter`가 인증된
  typed message를 `ProgramRuntime` 시작 전에 호스트 callback으로 전달할 수
  있습니다. callback은 task/context identity를 유지해야 하며 반환된 Task는
  일반 task 관측을 위해 보존됩니다.
- **신뢰되지 않은 보조 런타임 컨텍스트.**
  `ContextArtifactKind::UntrustedSupplemental`과
  `ContextPlacement::AfterHistory`를 추가했습니다. 기존 artifact kind의
  system message 동작은 유지하면서, 명시적으로 신뢰되지 않은 RAW 또는
  derived context를 receipt에 결합해 전체 Human/AI/Tool 시간순 이력 뒤에
  user data로 전달할 수 있습니다.

## [0.12.1] - 2026-08-23

### 수정됨
- **Python 예제 런타임 계약.** 라우트 채널 예제가 예약된 `__route__`를
  사용하도록 수정하고, 동적 `Send` 페이로드 채널을 선언했으며, 순수 캐시
  예제를 `CacheScope::Reusable`로 설정했습니다. 또한 OpenRouter 기본 URL을
  보존하고 Windows 콘솔 출력을 UTF-8 안전하게 만들었으며, 성공했지만 비어
  있는 reasoning 모델 응답은 거부하거나 재시도합니다.
- **Responses 전송 데모.** 공식 OpenAI는 실제 WebSocket 스트리밍 진입점을
  사용하고, 호환 게이트웨이는 빌드에 포함된 HTTP/2 또는 HTTP/1.1을
  선택합니다. 딥 리서치 데모에는 제한된 토큰·시간 예산과 3-way/2-worker
  fan-out을 적용했으며 OpenRouter로 실제 검증했습니다.
- **조치 가능한 Windows 네트워크 오류.** Python에서 `_HAVE_LIBCURL`을
  노출하고 로캘 인코딩된 `std::system_error` 문구를 category와 code가 포함된
  안전한 ASCII 진단으로 변환합니다. 따라서 Winsock 10060 같은 원인이
  `UnicodeDecodeError`로 대체되지 않습니다.

## [0.12.0] - 2026-08-23

### 추가됨
- **엄격한 런타임 인터포지션 완성.** `StrictRuntimeProfile`, 내구성 있는 제공자 터미널 결과 영수증, SQLite 스키마 v3 마이그레이션, JSON-RPC stdio/HTTP 아티팩트 게시를 포함한 전송 중립적 필수 Hook 백엔드, 일반 필수 컨텍스트 및 `HardConstraint` 아티팩트, 정확한 보존 `ContextTransformReceipt`, 내구성 있는 런타임 개발자 지침 결정, 그리고 컴파일 전 예약 `ProgramSynthesisGateway`을 추가했습니다. 생성된 소스는 여전히 스스로 활성화, 바인딩, 마이그레이션 또는 생성을 할 수 없으며, 해당 전환은 별도의 호스트 소유 Program 전환으로 유지됩니다. `DurableProviderDispatchReceiptStore`의 커스텀 하위 클래스는 이제 터미널 `settle`/`outcome`을 구현해야 하며, C++ 소비자는 이 ABI 변경에 맞춰 재빌드해야 합니다.

- **격리된 PostgreSQL Program-store 통합 픽스처(fixture).** 다이제스트 고정(digest-pinned) 및 루프백 전용을 추가했으며, `tests/fixtures/q7-postgres/compose.yaml` tmpfs 스토리지와 헬스 게이트(health gate)를 포함합니다. `NEOGRAPH_TEST_POSTGRES_URL` 이(가) 일회용 테스트 데이터베이스를 가리킬 때, `ProgramCatalogTest.PostgreSQLProgramStoreReopensActivationAndOwnerVisibility` 은(는) 게시(publish), 활성화(activation), 재개(reopen), 소유자 격리(owner isolation)를 다룹니다. 이는 테스트 인프라이며, Q7 최종 증명 스냅샷이 아닙니다.

- **Fail-closed QuickJS 레거시 드레인 감사.** `scripts/audit_legacy_drain.py` 및 해당 CTest 계약이 추가되었습니다. 이 도구는 명시적으로 열거된 동결된 Program/Harness 저장소 스냅샷에서 표준적이고 콘텐츠 주소 지정된 증명을 생성합니다. 알 수 없거나 변경 가능한 레코드, 분류되지 않은 레거시 소스, 드레인 전용 레코드, 그리고 활성 또는 복구 가능한 레거시 실행을 거부합니다. 활성 `-wal`, `-shm`, 또는 `-journal` 사이드카가 있는 SQLite 입력은 열기 전에 거부되므로, 활성 WAL 데이터베이스의 원시 복사본은 최종 증명으로 사용될 수 없습니다. 이는 Q7 증거 메커니즘을 확립하지만, 배포 특정 최종 드레인 또는 레거시 파서 삭제가 완료되었다고 주장하지는 않습니다.

- **PostgreSQL 최종 드레인 아카이브 스캔.** 레거시 드레인 감사기이제 고정된 `program_postgres_dump` 커스텀 아카이브를 허용하고, `pg_restore`를 데이터 전용, strict-table, 스크립트 출력 모드에서만 호출하며, 데이터베이스로 복원하지 않습니다. Program 번들, 버전 및 활성화 테이블을 지속된 식과 비교하여 검증하고, 필수 테이블을 누락하거나 변경하는 아카이브를 거부하며, 레거시 Program 버전의 활성화를 최종 제거 차단 요인으로 처리합니다.

- **배포 없는 Q7 최종 증명 모드.** 레거시 드레인 감사자는 사전 릴리스 또는 프로덕션 NeoGraph 배포가 전혀 존재한 적이 없는 경우에만 명명된 운영자 증명을 수락합니다. 해당 모드는 스토리지 대상을 허용하지 않으며, 과거 레거시 아티팩트도 허용하지 않고, 생성된 증명에 `evidence_mode: "no_deployment_attestation"` 라벨을 붙이며, 혼합되거나 증명되지 않은 빈 인벤토리에 대해 실패 시 차단합니다. 드레인되거나 삭제되거나 손실되었거나 접근할 수 없는 과거 상태는 포함할 수 없습니다.

- **OpenRouter 공급자 라우팅.** `OpenAIProvider` 이제 `CompletionParams::extra_fields.provider` 에 전달된 객체를 Chat Completions 요청 본문의 `provider`로 전달합니다. 객체가 아닌 값은 HTTP 요청 전에 실패합니다. 이는 OpenRouter의 문서화된 호출별 라우팅 기본 설정을 노출하면서 다른 네이티브 `extra_fields` 키는 무시된 채로 둡니다. 라이브 Beast 쿡북은 공급자를 고정하고 4,000토큰 생성 예산에 대해 명시적인 180초 타임아웃을 사용합니다.

- **Copy Ninja 로컬 그래프 노드 브리지.** 전송-프리 `a2a::CopyNinjaNode`를 추가했습니다. 별도로 구체화된 Copy Ninja harness를 래핑하며, `prompt`를 읽고 `response`를 덮어씁니다. `cookbook_the_beast_copy_ninja` 라이브 cookbook를 추가했습니다: 해당 LLM은 이 고정 로컬 노드만 작성할 수 있으며, 일반 Core 게이트들 이후에 네 번째 로컬 바인딩 게이트를 통과해야 하며, 합성 소스 에이전트가 RPC를 관찰하면 실패합니다. 카드 텍스트, 엔드포인트, 자격 증명 및 소스는 승인되지 않은 후보에서 제외되며, 호출자 프롬프트는 작성 LLM 요청에 들어가지 않습니다.


- **선택적 Program 컴포넌트 경계.** 옵트인 `NEOGRAPH_BUILD_PROGRAM` 스위치, 내보낸 `neograph::program` 대상, `<neograph/program/program.h>` 진입점을 추가했습니다. 설치된 패키지 컴포넌트 검색은 이제 빌드된 경우에만 Program을 보고하며, Core 전용 설치에서는 기존 `neograph::core` 링크 인터페이스를 유지합니다.

- **불변 Program 값 모델.** 안정적인 타입 진단, 깊이 소유된 정규 JSON 및 C++ 빌더 `ProgramSource` 입력, 불변 콘텐츠 주소 지정 `ProgramBundle`/`ProgramVersion` 값, 정규 직렬화, SHA-256 알고리즘 태그 식별, 소스 맵, 임포트, 엄격한 버전 저장 값 스키마를 추가했습니다. `neograph::program`는 이제 컴파일된 내보내기 라이브러리이며 Core에만 의존합니다. Bundle/version v1 프로젝션은 이제 봉인된 Core 정의와 계획 식별, 의미 버전 실행 다이제스트, 계약, 클로저, 경계, 타입 지정 승인(admission)/구체화 영수증을 요구합니다. 해당 식별은 형식 및 저장 버전을 바인딩하고, 의미 집합은 안정적인 순서를 사용하며, 진단은 잘못된 포인터, 역전된 범위, 알 수 없는 열거형을 거부하면서 정확한 오프셋이 없을 때 파서 범위는 생략합니다.

- **밀봉된 Program 승인(admission) 폐쇄.** 변경 불가능한 `RegistrySnapshot`, `AdmissionProfile`, 및 `PolicySnapshot` 값이 빌더 타임 호출 가능 캡처, 엄격한 정규 매니페스트, 도메인 분리 지문, 그리고 `ProgramVersion`에서의 실패 시 차단 교차 지문 검증과 함께 추가되었습니다. Core는 이제 Program 구체화를 위한 명시적으로 명명된 로컬 전용 parse/link/validate 진입점을 노출하며, 기존 로컬 우선/글로벌 폴백 overcoated 오버로드는 변경되지 않습니다. 레지스트리 항목은 이제 전이적 승인(admission) 폐쇄를 위한 정규 정확한 실행 가능 실행 진입 가능 종속성 에지를 기록하며, 로컬 전용 조건 검사 로컬 조건 검사는 프로세스 글로벌 레지스트리를 참조하지 않고 configlegacy keyed-edge 문서를 다룹니다.

- **단일 루트 `call_core` Program 컴파일러.** 닫힌 Program-v1 봉투만 허용하고, 봉인 전에 순수 로컬 Core parse/round-trip/validation을 수행하며, RFC 6901 포인터와 소스 맵 귀속이 포함된 집계 타입 진단을 내보내는 `ProgramCompiler`를 추가했습니다. 컴파일은 팩토리나 호출 가능 항목을 호출하지 않고 정규 Program, 레지스트리, 전이적 실행 가능 클로저, 기능/효과, 임포트 Merkle, 봉인 정의, Core 계획 식별을 도출합니다. 작성된 문서 스키마, 완전한 유한 예산 계약, 제로 디스패치 거부 테스트, 정적 및 공유 설치 소비자 커버리지가 컴파일러와 함께 제공됩니다. Core는 가산적 총 parse/round-trip 및 로컬 검증 보고서를 얻었으며 레거시 예외 발생 API는 기존 동작을 유지합니다.

- **고정 Program 런타임 수직 슬라이스.** `ProgramCatalog`, `EngineGenerationCache`, `ProgramRuntime`, 공유 `ProgramHandle`, 불변 `ProgramResult`, 타입 지정 Program 이벤트 봉투, 인메모리 `ProgramStore`, 추가 전용 CAS `ProgramJournal`를 추가했습니다. 승인(admission)은 구체화 전에 신뢰할 수 없는 번들 의미를 재계산하며, 각 시도는 하나의 불변 Core 세대를 고정하고 기존 `GraphEngine` 비동기 경로를 호출합니다. 런타임 실행은 이제 완료, 중단, 정확한 체크포인트 재개, 취소, 시간 초과, Core 단계 소진, 체크포인트 비호환성, 실패를 타입 지정 터미널 상태로 매핑하면서 비재생 가능 예산과 체크포인트 계보를 보존합니다. 저널 커밋은 체크포인트/터미널 이벤트 전달보다 먼저 수행되며, 동시 재개는 하나의 CAS 승자가 있고, PR6 슬라이스는 Core 브로커가 존재할 때까지 효과가 있거나 비어 있지 않은 스키마 Program을 거부합니다.

- **QuickJS 제어 언어 프론트엔드.** 옵트인 `NEOGRAPH_BUILD_QUICKJS_CONTROL`, 봉인된 `ProgramSource::from_javascript(...)`, 비공개 컴파일 전용 QuickJS 컨텍스트를 추가했습니다. 소스 봉투는 엔진/언어/호스트 API 버전을 고정하며, 유일한 `ng` 호스트 표면은 버전 지정 그래프 빌더이고, 메모리, 스택, 인터럽트 폴링 한도는 실패 시 차단됩니다. JavaScript는 하나의 불변 `call_core` Program 계획을 생성하며 런타임 VM, 바이트코드 아티팩트, Core 의존성이 되지 않습니다.

- **A2A Agent Card 호환성 후보.** 단일 요청, 인증 없음, 리디렉션 없음 well-known-card 수집기와 팩토리 전용 불변 후보 컴파일러를 추가했습니다. 후보는 다이제스트 고정 출처, 경계 있는 프로토콜 사실, 안전한 Skill ID만 유지하며, 자유 형식 카드 텍스트, 광고된 RPC 엔드포인트, 공급자/보안 구성, 자격 증명은 제외됩니다. Copy Ninja PoC는 추가로 해당 다이제스트에 고정된 독립적으로 관찰된 동작을 요구하며 소스 에이전트를 절대 디스패치하지 않습니다.

- **SQLite Harness 레코드 저장소(이슈 #147 후속 조치).** WAL 기반, 스키마 버전 지정 아티팩트/실행 지속성을 위한 선택적 `neograph::mcp_sqlite` 대상과 `SqliteHarnessRecordStore`를 불변 아티팩트 및 실행-아티팩트 바인딩과 함께 추가했습니다. Harness MCP 바이너리는 이제 `runs.db`에 레코드를 저장하며, 체크포인트는 `checkpoints.db`에 유지됩니다.
- **AMD OpenMP 대상 오프로드 개념 증명.** 선택적 `bench_openmp_offload` 벤치마크가 추가되었으며, 동일한 숫자 fan-out 워크로드에서 직렬 CPU 실행, OpenMP 자동 스레딩, 반복별 GPU 매핑, 지속적 GPU 데이터를 비교합니다. 실제 디바이스 대비 호스트 폴백 실행, 정확성, 전송 포함 대기 시간, 커널 전용 대기 시간, 속도 향상을 보고합니다. `NEOGRAPH_OPENMP_OFFLOAD_ARCH=gfx1201`는 Radeon AI PRO R9700용 ROCm/Clang 디바이스 이미지를 활성화합니다.


### 변경됨

- **C++ ABI 및 SOVERSION 정책(이슈 #194).** 컴파일된 모든 공개 `neograph_*` 라이브러리는 이제 프로젝트 `VERSION` 및 주요 `SOVERSION`를 포함합니다. 설치된 공유 라이브러리는 자체 디렉터리에서 형제 종속성을 해결합니다. 사전 v1 릴리스는 ABI 세대 0을 사용하지만 필수 재빌드 경계를 선언할 수 있습니다. `0.11.1` 또는 이전 버전에 대해 빌드된 모든 C++ 소비자는 다음 릴리스에서 재빌드해야 합니다. `NodeCache`, `EngineConfig`, `CompletionParams`, `Agent`, `RequestOptions`, `SseEventParser` 및 공급자 구성 공개 레이아웃이 변경되었기 때문입니다. 제한된 `UsageAccumulator` 예약을 포함하는 릴리스는 또 다른 필수 재빌드 경계입니다. 해당 공개 객체 레이아웃은 이제 예약 회계 상태를 포함합니다. 버전 1.0은 ABI 세대를 1로 변경하고 지원되는 v1 레이아웃을 고정합니다. CI는 이제 격리된 정적 및 공유 설치 소비자를 빌드하고 실행하며 ELF/Mach-O 로더 메타데이터를 확인합니다. [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md) 참조.
- **`GraphNode::run(input)` 마이그레이션 가이드 완료.** Python `GraphNode` 기본 클래스는 더 이상 삭제된 `execute*` 메서드를 참조하지 않습니다. `run(input)`이 없으면 마이그레이션 문서 경로를 포함하는 `NotImplementedError`를 발생시킵니다. C++/Python 참조, async/streaming 가이드 및 예제 README는 실제 v0.9.0 단일 진입점에 맞춰 정렬되었습니다. 마이그레이션 절차는 [`docs/migration-v0.4-to-v1.0.md`](docs/migration-v0.4-to-v1.0.md)에 C++ 및 Python 예제로 문서화되어 있습니다.
- **공급자 API 영구 호환성 정책(이슈 #5).** `Provider::complete()`, `complete_async()`, `complete_stream()`, `complete_stream_async()` 및 콜백 기반 `invoke()`의 계획된 제거가 철회되었으며 `[[deprecated]]` 경고가 제거되었습니다. 기존 API는 계속해서 호환성 및 보안 수정을 받습니다. 새 공급자 구현 및 직접 호출자는 각각 `CompletionProvider::do_invoke()` 및 `invoke_request(CompletionRequest)`를 사용하는 것이 권장됩니다. 모든 새 기능을 기존 API에 백포트하는 것은 보장되지 않습니다. 공개 시그니처, 가상 순서, 객체 크기 및 vtable은 변경되지 않습니다.

### 제거됨

- **더 이상 사용되지 않는 TransformerCPP 통합 예제.** 더 이상 사용할 수 없는 외부 호스팅 저장소에 의존했던 `example_inproc_gemma`, `NEOGRAPH_BUILD_LOCAL_INFERENCE_EXAMPLE` 및 `TRANSFORMERCPP_DIR`가 제거되었습니다. 표준 OpenAI 호환 로컬 서버를 사용하는 `example_local_transformer`는 유지됩니다.

### 수정됨

- **Python Program 휠 의존성 완결.** PyPI 빌드는 이제 QuickJS 제어 런타임을 활성화하고, 확장 모듈 옆에 `neograph_program` 로더를 번들하며, Windows의 Program/Harness DLL 경계를 가로질러 사용되는 비공개 평가기를 내보냅니다. 이를 통해 `P_JS_UNAVAILABLE`, 로더 누락으로 인한 import 실패, 공유 빌드의 `LNK2019` 실패를 방지합니다. 또한 명령 identity 구성에서 컴포넌트 dylib 경계를 가로지르는 중첩 initializer-list JSON 복사를 피하여, Program 실행 중 macOS arm64에서 발생할 수 있던 충돌을 방지합니다. Windows 휠 repair는 외부 DLL 이름을 해시 맹글링하여, 먼저 로드된 시스템 `sqlite3.dll`이 번들된 빌드를 덮어쓰고 `SQLiteContextStore` 생성 중 충돌하는 것을 방지합니다.
- **제한된 원격 전송 및 자격 증명 출처.** HTTP/1.1, HTTP/2, SSE 및 WebSocket 수신 경로는 이제 신뢰할 수 없는 크기가 할당되기 전에 보수적인 응답, 헤더, 청크, 라인, 프레임, 핸드셰이크 및 메시지 한도를 적용합니다. Redirect된 POST 요청은 정규화된 동일 출처 내에서만 follow됩니다. 공급업체 자격 증명은 명시적 numeric loopback 개발 예외가 활성화되지 않는 한 TLS를 필요로 하며, WebSocket 디버그 출력은 더 이상 요청 헤더 또는 페이로드를 포함하지 않습니다.
- **QuickJS `all` 생성 시작 경쟁(join startup race) 수정.** Completion 핸들러는 이제 JavaScript join을 닫기 전에 초기 멤버 시작 등록을 기다립니다. 즉시 완료되는 자식은 더 이상 형제 초기 또는 대체 명령이 전달되기 전에 생성기를 재개할 수 없습니다. 반복되는 런타임 회귀 테스트가 두 경로를 모두 다룹니다.
- **Harness 집계 결과 출처(issue #174).** 세부 정보에는 이제 `finding_sources` 배열이 기존의 플랫 `findings` 배열과 정렬되어 포함됩니다. 각 항목은 집계 인덱스, 소스 워커 ID, 워커 로컬 인덱스를 기록하며, 스키마 검증된 워커 출력이나 기존의 `findings` 형태를 변경하지 않습니다.
- **Harness 내보내기 결과 린트(issue #173).** 노드 효과 계약은 이제 호출자가 그래프 실행 후 소비할 때 선택적 `exports` 배열에 기록된 채널을 선언할 수 있습니다. 따라서 Harness 컴파일과 `GraphEngine` 런타임 검증 모두 실제 쓰기 전용 채널에 대해 E6을 유지하면서 `final_result`에 대해 잘못된 경고를 하지 않습니다.
- **MCP 2025-11-25 도구-클라이언트 계약 현대화 (이슈 #147 M0).** 초기화는 이제 멱등적이며 협상된 서버 메타데이터를 유지합니다. HTTP 도구는 검색 세션을 재사용합니다. `/mcp` 엔드포인트 구성은 요청과 알림에 공유됩니다. 도구 검색은 불투명 커서를 따릅니다. JSON-RPC 코드/데이터, 전체 도구 메타데이터, 비텍스트 콘텐츠, `structuredContent`, `isError`, `_meta`는 C++ 및 Python 경로에서 유지됩니다. 구성 가능한 JSON 시간 제한/정적/동적 헤더, 출력 스키마 검증, 엄격한 응답 ID 확인, 그리고 타입화된 `InitializeResult`, `ToolDefinition`, `ListToolsPage`, `CallToolResult` API가 추가되었습니다. SSE 감지는 `Content-Type`를 사용하여 `data:` URL을 포함하는 JSON을 잘못 분류하지 않습니다.
- **작업별 취소 상태 및 게시된 emit 수명 안전성.** `GraphEngine::run`, `run_async`, `run_stream`, `run_stream_async` 각각은 호출자가 제공한 부모로부터 실행당 하나의 실행 자식을 생성하고, 해당 자식만 내부 `co_spawn`/동기화 브리지에 바인딩하며, 동일한 자식을 `RunContext`로 전달합니다. 따라서 단일 부모 아래의 모든 동시 실행을 취소해도 서로의 취소 슬롯을 덮어쓸 수 없습니다. 분기된 실행 자식은 게시된 emit을 통해 기존 `shared_ptr` 소유권을 유지하여 엔진 작업 완료와 emit 실행 사이의 use-after-free를 방지합니다. 취소로 인한 asio `operation_aborted`는 재시도 가능한 노드 오류가 아닌 `CancelledException`로 전파됩니다. `CancelToken` 0.11.x 객체 레이아웃 및 인라인/헤더 전용 동작은 변경되지 않습니다. 이미 컴파일된 C++ 소비자가 업데이트된 `fork()` 수명 동작을 적용하려면 재컴파일이 필요합니다. 공유 라이브러리만 교체하면 객체 레이아웃 호환성은 유지되지만, 소비자 바이너리에 포함된 기존 인라인 함수 본문은 변경되지 않습니다. 그러나 외부 코드가 직접 생성한 토큰에 대해 `bind_executor()`를 호출하는 경우, 실행기의 게시된 작업이 완료될 때까지 토큰을 유지할 책임은 여전히 호출자에게 있습니다.
- **PostgreSQL 비동기 연결 전역 타임아웃 정책 문서화됨.** 비동기 초기 연결 및 교체는 모든 호스트/IP 주소에 대해 단일 타임아웃을 사용합니다. 양수 연결 문자열에 직접 작성된 명시적 `connect_timeout`는 최소 2초로 적용되며, 지정되지 않았거나, 0, 음수, 또는 환경 변수/서비스 파일 전용 값은 운영상 안전한 기본값인 30초를 사용합니다. 이는 libpq의 호스트별 동기 타임아웃과 의도적으로 다릅니다. 동기 생성/교체 동작은 변경되지 않습니다.
- **JARVIS 목업 빌드 수정 (이슈 #130).** `cookbook_jarvis` 오디오 의존성이 없을 때 `MicCapture`가 불완전한 타입으로 남아 발생하는 컴파일 실패를 수정했습니다. `NEOGRAPH_JARVIS_FORCE_MOCK`를 추가하여 ASan CI가 러너에 설치된 패키지와 관계없이 항상 목업 구성을 빌드하도록 했습니다. 세션 러너는 이제 실제 CMake 출력 경로와 전문 대상 이름을 사용하며, 기존 `demo_mcp_server.py`를 올바르게 실행합니다.
- **노드 실패 컨텍스트 보존 (issue #123).** C++ 실행 오류는 원본 `NodeExecutionError`, 실패한 노드 이름, 시도 횟수를 포함하는 `exception_ptr`로 전파됩니다. 종료 `ERROR` 이벤트도 동일한 컨텍스트를 기록합니다. Python에서 원래 예외 객체, 유형, args, 사용자 속성, 및 traceback은 원래대로 저장되며, `.node_name`와 `.attempts` 속성만 추가됩니다. `NodeInterrupt`, 취소(cancellation), 및 메모리 부족 예외는 기존 제어 흐름을 따르며 컨텍스트로 감싸지지 않습니다.

### 수정됨(문서).

- **Provider cookbook에서 무시되는 노드별 프롬프트를 제거했습니다(이슈 #116).** 다음을 사용하여 다중 역할 동작을 설명하던 세 개의 Python 예제를 수정했습니다. `config.system` 내장된 `llm_call` 에서 읽지 않는. 각 예제는 다음을 사용하는 엄격한 단일 호출 그래프로 다시 작성되었습니다. `NodeContext.instructions`, 관련 README는 실제 동작에 맞게 조정되었습니다.
- **예약된 `RunContext::deadline` 문서 수정 (이슈 #115).** `deadline`와 `trace_id`를 `RunConfig`를 통해 설정할 수 없고 Python에서 노출되지 않음에도 불구하고 실행별 메타데이터로 사용 가능하다고 제시한 문서 및 Doxygen 주석을 수정했습니다.
- **`GraphNode::run` 예제 서명 수정(이슈 #129).** `const NodeInput&`(참조로)을 수용하던 공용 헤더 예제를 수정했습니다. 이 예제는 실제 값-기반 virtual을 override하지 못했으며, 코루틴 인자 수명에 필요한 값-기반 계약을 컴파일 타임 테스트로 고정했습니다.

### 추가됨

- **하위 호환 가능한 Provider 마이그레이션 경로.** 새로운 `CompletionRequest`는 스트리밍 모드를 콜백 존재 여부와 분리하며, `CompletionProvider`는 새 구현이 `do_invoke()`만 작성하도록 요구합니다. 기존 `Provider` vtable, 4개의 레거시 가상 함수, 콜백 기반 `invoke()`, 그리고 Python `complete()` 서브클래스 계약은 유지됩니다.

- **Python 영속성 백엔드** (#117) — `Store` 및 `CheckpointStore`는 이제 C++ 가상 디스패치를 통해 Python으로 연결되는 생성 가능한 서브클래스 베이스입니다. `StoreItem`, `CheckpointPhase`, `Checkpoint` 및 `PendingWrite`는 JSON 형태의 필드로 노출됩니다; 체크포인트 대기 중 쓰기 메서드는 선택 사항으로 유지됩니다.
- **Python 동기 취소** (#119) — Python 호출자는 `CancelToken`을 구성하고 `RunConfig.cancel_token`에 할당하여, 다른 스레드에서 `engine.run()`에 대한 협조적 중단을 수행할 수 있습니다.

- **Python 체크포인트 기록** (#118) — `GraphEngine.get_state_history()`은 최신-우선 체크포인트 레코드를 노출하여 호출자가 parent 링크, 메타데이터, 단계, 및 ID를 historical 상태에서 분기하기 전에 검사할 수 있습니다.

- **DSL 표면(정교화 계층) + 스키마 진화 게이트** (#75 M4).
  - **Elaborator**: `vars` (`{"$var":...}` / `${...}` 보간, 비순환 강제) / `templates`+`use` (정확한 매개변수 일치 강제, 노드 접두사 이름 변경 — 로컬 참조, 배리어, 라우트 포함; 채널은 공유 상태이므로 전역적으로 병합됨) / `when` 조건부 포함. **비튜링 완전하며 전(total)적임**: 모든 DSL 문서는 유한 시간 내에 고유한 코어로 정규화되며 해당 코어에 대해 멱등적입니다. 모든 오류는 DSL 소스 좌표(`use[2].args`, `vars.model`)와 함께 보고되며 소스 맵(출력 위치 → 생성 구문)이 포함됩니다. 잠금 파일 워크플로: `./example_elaborate harness.dsl.json > harness.json` (예제 53).
  - **`GraphCompiler::upgrade_to_latest()`**: 무손실 v0→v1 기계적 변환 — strict이 거부하는 키는 `x-upgraded-<key>` 주석 네임스페이스로 격리되며(데이터 삭제 없음), 빈 배리어는 명시적으로 제거된다. 전체 코퍼스는 "레거시 관대 컴파일 IR == 업그레이드 후 strict 컴파일 IR"(정규 동등성, 버전 스탬프 제외)을 보장하도록 테스트된다.
  - **스키마 진화 게이트**: `tests/fixtures/schema_snapshot.json` 기준선에 대한 추가 전용 부분 집합 판정(JSON Subschema 계열의 결정 가능한 부분 집합) — 노드 유형/속성/리듀서/조건 제거, 필수 집합 증가, 폐쇄 조건 레이블 변경, 효과 계약 변경은 모두 테스트 실패 = CI 병합 차단을 초래합니다. 호환되지 않는 변경은 동일한 검토 커밋에서 버전 범프 + 업그레이더 + 스냅샷 재생성을 강제합니다.

- **PBT / 델타 검증 하네스** (#75 M3). 300-시드 결정적 토폴로지 생성기(스키마 앙상블의 유효한 strict 문서, 자체 계측 기능 커버리지 — conditional_edges/barrier/interrupt 발생률이 30% 미만으로 떨어지면 테스트 실패: 미테스트 기능은 조용한 구멍이 아닌 실패가 된다).
  - **돌연변이 탐지**: 300-시드 코퍼스에서 번역 검증이 5가지 제거 유형(conditional_edges/edge/barrier/interrupt/channel) + 3가지 배선 오류 유형(route 붕괴 / edge 재타겟팅 / node 이름 변경 = 제거+위조 균형)의 모든 적용을 포착함을 확인. 적용률 하한(시드의 10%)도 단언된다.
  - **참조 인터프리터 델타**: 문서화된 슈퍼-스텝 시맥스(goto 선점, 배리어 누적, 사전식 폴백, 암시적 __end__)를 코드 분리 구현에서 재구현한 독립 모델로, 12-스텝 × 300-그래프에서 스케줄러와 비교한다(DESIL 교훈: 검증기만으로는 잘못된 실행을 잡을 수 없다).
  - **엔진 ↔ Studio 공유 코퍼스**: `tests/fixtures/topology_corpus/` 15개 변형(유효 3개 + E3–E11 위반 12개)이 NeoGraph-Studio `tests/corpus/`와 완전 동일하며, 둘 다 동일한 판정(코드:심각도 멀티셋)을 단언한다 — 두 구현은 조용히 분기할 수 없다.

- **GraphValidator — 토폴로지 정적 의미 검사 (E3–E11 + 효과)** (#75 M2). 파싱(M1)과 실행 사이의 통과 계층. strict 문서(schema_version>=1)에서 오류는 컴파일 실패, 경고는 stderr 린트; 관대 문서에서는 오류 수준 진단만 stderr 경고로 나타난다(기존 그래프에서 제로 노이즈). 판정 철학 = 검사기 건전성 우선: 엔진 의미론에서 결코 올바를 수 없는 것만 오류다(모호 참조 E3, 신호 경로 없는 배리어 E8 — goto가 배리어 회계를 우회하므로 복구 불가, 빈 route E10 — 디스패치가 rend() UB를 역참조, 선언되지 않은 채널 쓰기 E4 — 런타임 오류에서 throw를 런타임에서 확인); Command.goto/Send가 정당화할 수 있는 것은 경고(도달 가능성 E7, 탈출 없는 사이클 E11, 배리어 없는 순수 fan-in E9, 따라쓰기 경쟁 E5, 죽은 채널 E6). 모든 진단에는 기계 판독 가능한 반례(counterexample) JSON이 동반된다 — Studio 캔버스 하이라이트용(M3).
  - **Route 완전성 (E10)**: `ConditionSpec` 라벨 계약 도입. `register_condition` 3-인자 오버로드를 통해 조건의 출력 라벨 집합을 선언하면 closed-condition 라우트가 라벨과 정확히 일치해야 한다 — 커버되지 않은 라벨은 스케줄러의 "사전식 마지막 라우트" 폴백(순서 종속 임의 타깃)에 떨어지며, 이는 오류다. 내장 `has_tool_calls` = closed {false,true}, `route_channel` = open + 알려진 {default}.
  - **채널 효과 계약**: `register_type` 4-인자 오버로드가 노드 유형별 읽기/쓰기 채널을 선언한다. E4/E5/E6 분석은 그래프의 **모든** 노드 유형이 선언된 경우에만 활성화된다(단일 알 수 없는 유형이 전체 분석을 건너뜀 — 커버리지보다 건전성 우선). 내장 3가지 유형(llm_call/tool_dispatch/intent_classifier) 완전 선언됨.
  - `node_effects` · `condition_specs` 에 추가됨 `export_schema()` (기존 `conditions` 배열은 하위 호환성을 위해 유지됨). 신규 테스트 22개.

- **토폴로지 컴파일타임 일관성 게이트 — 소비 키 회계 + 번역 검증** (#75 M1). "조용한 의미 손실" 클래스(v0.1.0–v0.1.7 `conditional_edges` 조용한 손실과 동일 종)를 구조적으로 차단하는 2중 매커니즘:
  - **소비 키 회계(Consumed-key accounting)**: `"schema_version": 1`를 선언하는 문서는 엄격한 컴파일로 전환됩니다 — 소비되지 않은 키(오타 `conditionnal_edges`, 지원되지 않는 필드, 빈 `wait_for`에 의해 조용히 삭제되는 장벽, 인라인 조건부에서 무시되는 `to`)는 모두 수집되어 컴파일 오류로 보고됩니다. 마킹은 파스 블록 **내부**에서 발생하므로, 파스 단계를 제거하면 마크도 제거되어 해당 기능을 사용하는 엄격한 문서는 즉시 실패합니다 — 삭제 회귀가 조용히 발생할 수 없는 구조입니다. `_`/`x-` 접두사 키(`_comment`, `x-studio-*`)는 항상 주석 네임스페이스로 허용됩니다. `schema_version`가 없는 기존 문서는 허용적 동작(바이트 보존)을 유지합니다.
  - **번역 검증**: 모든 컴파일에서 `CompiledGraph::to_json()` 재방출 + `GraphCompiler::canon()` 정규형 검사 `canon(input) ==
    canon(re-emit)`. 불일치(= 컴파일러가 무언가를 누락했거나 잘못 연결한 경우)는 엄격한 문서에서는 throw하고 허용적 문서에서는 stderr 경고를 발생시킵니다. 동등성은 구조적 비교입니다 — 라우트 키 교체 같은 잘못된 연결도 포착됩니다(존재 비교가 놓치는 클래스).
  - `NodeFactory::config_schema(type)` query 추가, `schema_version` 필드가 문서화됨( `export_schema()`). 27개의 새 테스트(`tests/test_compiler_strict.cpp`) — v0.1.x 드롭-뮤턴트 시뮬레이션(conditional_edges/barrier/interrupt 드롭)
    + route miswirings) included.

## [0.11.1] - 2026-06-25

### 변경됨

- **stdio MCP 동시 호출 — I/O 중첩을 위한 correlation-ID 역다중화기.** `0.11.0` 동시 도구 디스패치는 실제로 HTTP MCP에서만 중첩되었습니다. stdio MCP는 `StdioSession::rpc_call_async`에서 **전체 요청→응답 왕복** 동안 용량 1 채널 잠금을 유지하여, 단일 세션 파이프를 통해 한 턴의 여러 호출을 직렬화했습니다(경과 시간 ≈ 지연 시간의 합). 단일 파이프가 근본 원인은 아니었습니다 — JSON-RPC `id`는 정확히 하나의 연결에서 파이프라이닝하기 위해 존재합니다. 잠금을 correlation-ID 역다중화기로 교체했습니다:
  - 용량-1 채널을 **쓰기 전용 잠금(write-only lock)**으로 재사용 — 프레임 쓰기 순간에만 유지되므로, 두 호출의 바이트가 서로 interleave되지 않으며 읽기는 더 이상 직렬화되지 않는다.
  - 단일 리더 코루틴(`run_reader`)이 읽기 측을 독점적으로 소유하고 JSON-RPC `id`를 통해 각 응답 라인을 올바른 호출자의 싱크로 전달합니다. N개의 동시 호출이 읽기를 중첩하므로 경과 시간 ≈ max(지연 시간) — **단, 피어 MCP 서버가 동시에 처리할 때만** (단일 스레드 순차 서버는 Amdahl의 한계에 도달합니다).
  - 리더는 진행 중인 호출이 있는 동안에만 지연 실행되며 대기자가 없으면 종료되므로, 개인 `run_sync` io_context가 정상적으로 반환됩니다. 대기자는 호출자가 대기하는 동안에만 존재하며 `MCPTool`의 `shared_ptr`를 통해 세션을 유지하므로, 리더는 파괴된 세션에 절대 접근하지 않습니다(소멸자 조인 불필요). 파이프 EOF/오류 시 리더가 모든 싱크를 닫으므로, 대기 중인 호출자는 무한 대기 대신 예외를 받습니다.
  - **API/문법 변경 없음** — 공개 헤더는 변경되지 않았으며, 기존 코드는 재컴파일이 필요 없습니다. 엔진 오버헤드 회귀 0(`bench_neograph` 인터리브 A/B, seq/par Δ 0%).
  - 테스트: 스레드 기반 지연 픽스처 `tests/fixtures/mcp_stdio_slow.py` + `ConcurrentStdioCallsOverlapIO`(5×100 ms 호출이 500 ms 직렬 하한 대비 약 130 ms에 완료; 각 응답이 `id`를 통해 해당 호출자로 라우팅되는지 검증). ASan+UBSan ×3 클린.

## [0.11.0] - 2026-06-25

### 추가됨

- **동시 도구 디스패치 — `Tool::execute_async` 공식 비동기 경로.** `ToolDispatchNode` 는 단일 어시스턴트 턴에서 여러 `tool_call`들을 엔진의 `make_parallel_group`를 사용하여 **동시에** 실행합니다. 이전에는 각 호출이 동기식 `execute()`를 통해 순차적으로 실행되었으며, 특히 MCP 도구는 호출별로 `io_context` 를 생성하는 데 차단되어 병렬 MCP 호출이 겹치지 못했습니다(병렬 MCP 호출이 있는 외부 C++ 포크에서 발견됨). 수정: `run_sync` 를 통해 자체
  - 가상 `execute_async()`가 `Tool`에 추가됨 — 기본 구현은 동기식 `execute()`로 브리지되므로 기존 도구는 변경 없이 작동합니다.
  - `MCPTool`가 `AsyncTool`(으)로 변환되었으며, 네이티브 `execute_async`를 사용합니다(stdio는 `rpc_call_async`를 사용하고, HTTP는 비동기 핸드셰이크를 위해 새 `MCPClient::initialize_async`/`call_tool_async`를 사용합니다 — `run_sync`는 제거됨).
  - `ToolDispatchNode::run`는 노드 fan-out과 동일한 `make_parallel_group` 관용구를 통해 호출을 동시에 디스패치하며(단일 호출은 인라인), 결과는 호출 순서대로 적용됩니다. 동기식 `execute()` 파사드를 통해 이전 버전과 호환됩니다.
  - 검증: 478/478 ctest, Valgrind 0 누수, TSAN 0 레이스.

### 수정됨

- **Python 비동기 실행 예외 보존(issue #122).** `run_async`, `run_stream_async`, `resume_async`가 원래 Python 노드 예외를 문자열로 감싼 새 `RuntimeError`로 덮어쓰는 문제를 수정했습니다. 이제 원래 Python 예외 객체, 유형, 사용자 속성, 추적이 pybind11의 표준 예외 변환 경로를 통해 보존되며, C++ `py::type_error`는 동기 실행과 일치하는 Python `TypeError`로 전달됩니다. `resume_async`의 빈 콜백은 코루틴이 완료될 때까지 유지되어 pybind11 3.x에서 노출된 댕글링 참조 충돌도 수정합니다.

### 수정됨(문서).

- **README 요약 배지가 누락된 조건과 샌드박스 측정으로 드러난 내부 모순에 대해 수정되었습니다.** "네 가지 축" 요약 표 배지는 본문/심층 분석에서 측정 조건을 제거하여 과장된 것으로 읽혔습니다. 본문 측정 수치 및 조건과 일치하도록 수정되었습니다(측정 데이터 표 자체는 변경되지 않음):
  - **`p99 17 µs flat` → `p99 7 µs @ 10 K (1 CPU sandbox)`** — 배지의 17 µs가 본문(`At N=10,000 concurrent ... 7 µs p99`)과 모순되었고, `flat`는 GPU 바운드 부하 테스트 실행 지연 시간(648 ms)을 설명한 것이지 µs 단위 측정이 아니었습니다. 배지를 본문의 측정 수치 및 조건과 일치시켰습니다.
  - **`1.2 MB stripped binary` → `... (MinSizeRel static)`** — `libc.so.6` 전용 및 1.2 MB는 MinSizeRel + 정적 libstdc++ 빌드에만 해당합니다(기본 Release는 libstdc++/libgcc_s/libm/libc를 동적으로 링크). 이 조건은 이미 deep-dive §size에 문서화되어 있으며 배지에 복원되었습니다.
  - **`2 wheel deps` → `2 direct wheel deps (... ; 7 with transitive)`** — 직접 의존성은 실제로 `certifi` + `pydantic`(두 개)이지만, 실제 설치 트리는 pydantic 전이 의존성(pydantic-core, typing-extensions, annotated-types, typing-inspection)을 포함한 7개 패키지입니다.
- **deep-dive MinSizeRel 재현 명령에 `-DNEOGRAPH_BUILD_POSTGRES=OFF` 추가.** PostgreSQL은 기본값이 ON이므로, libpq가 없는 호스트에서 그대로 실행하면 구성이 실패합니다. 수정했습니다.

## [0.10.0] — 2026-05-20

### 추가됨

- **직렬 fan-out 일회성 stderr 경고(이슈 #62, PR #63).** `compile()`의 기본값은 `set_worker_count(1)`입니다 — fan-out 분기는 엔진 소유 스레드 풀 없이 호출자의 실행기에서 직렬로 실행됩니다. 이 의도된 동작은 문서만 보고 멀티-Send 그래프를 구축한 사용자에게는 조용한 직렬 실행처럼 보입니다. `NodeExecutor`가 풀 없이 멀티-Send(또는 멀티-발신-에지) fan-out을 디스패치할 때 처음으로 stderr에 일회성 안내 메시지를 추가했습니다. `std::atomic` + compare-exchange는 동시 fan-out에서도 정확히 한 번의 발행을 보장합니다. `set_worker_count(N>=2)`를 호출하면 `NodeExecutor`가 재구축되어 플래그가 자연스럽게 재설정됩니다. 환경 변수 `NEOGRAPH_SUPPRESS_FANOUT_WARNING=1`(또는 `true` / `yes`)로 억제 가능합니다 — 의도적인 worker=1 직렬 실행, 벤치마크, CI stderr 검증 사례를 위한 것입니다. 5개의 Linux + macOS 단위 테스트(`test_fanout_worker_warning.py`)로 검증됨: fire / one-shot / pool opt-in silence / env-var silence / single-Send no-warning. Windows: pytest capfd는 휠 바이너리의 MSVC CRT fd 캐싱과 호환되지 않으므로 모듈 수준에서 건너뜀 — 휠 바이너리 stderr 출력 자체는 정상입니다.

- **토폴로지 JSON Schema 내보내기 — `NodeFactory::export_schema()`**(이슈 #56, 코드 없는 시각적 블록 편집기의 전제 조건). 엔진이 소비하는 토폴로지 JSON 형식을 기계 판독 가능한 스키마(JSON Schema Draft 2020-12)로 한 조각에 내보냅니다: `{ neograph_version, $schema, topology (fixed
  envelope), node_types, reducers, conditions }`. 별도 저장소의 블록 편집기는 이 스키마에서 팔레트를 자동 생성합니다 → 편집기와 엔진은 버전 간에 드리프트할 수 없습니다. 전적으로 추가적입니다:
    - `NodeFactory::register_type(type, fn, json config_schema)` 3-argument
      variant added. Existing 2-argument delegates to permissive default
      schema — existing user nodes/calls unaffected.
    - `ReducerRegistry::names()` / `ConditionRegistry::names()` /
      `NodeFactory::registered_types()` query accessors added.
    - Configuration schema declared for 4 built-in types (`llm_call`/
      `tool_dispatch`/`intent_classifier`/`subgraph`). `NEOGRAPH_VERSION`
      exposed as a compile definition (pyproject.toml single source of truth)
      → schema version stamp.
    - `examples/52_export_schema.cpp` (`example_export_schema`):
      `./example_export_schema > schema.json` — standard path for the editor
      repo CI to produce the artifact pinned to a NeoGraph version.
    - Python: `neograph_engine.export_schema()` → dict (editor repo CI
      dumps after `pip install neograph-engine`).
    - `tests/test_schema_export.cpp` 8 + `test_export_schema.py` 4. Key:
      top-level `conditional_edges` surviving the loader→compile round-trip
      (regression guard against v0.1.0–v0.1.7 silent-drop recurrence).

### 수정됨

- **토폴로지 최상위 컨테이너 형식 검증(#126).** `channels`/`nodes`는 객체여야 하며, 그렇지 않으면 모든 모드에서 거부됩니다. `edges`/`conditional_edges` 배열 검증은 엄격 모드에서 강제되며, 레거시 키-에지-맵 호환성은 유지됩니다. 오류는 경로와 JSON 유형을 기록하며 전체 입력을 기록하지 않습니다.
- **`max_steps` 종료 상태 노출(#114).** `RunResult::max_steps_exhausted()`와 읽기 전용 Python 속성 `RunResult.max_steps_exhausted`가 추가되었습니다. 실행할 노드가 남아 있는 동안 `max_steps`에 도달한 경우에만 true이며, 동일한 상태가 gRPC 단일 응답 및 스트리밍 최종 JSON에서 제공됩니다. C++ 구조체 크기는 변경되지 않았습니다.

- **`set_worker_count` / `set_worker_count_auto` docstring 수정(이슈 #62, PR #63).** v1.0 준비 주기에서 의도적으로 `compile()` worker 풀 기본값을 `set_worker_count(hardware_concurrency())`에서 `set_worker_count(1)`로 되돌렸지만(근거는 `src/core/graph_engine.cpp:69-93` 주석 참조), 네 개의 사용자 대상 docstring에 이전 주장이 남아 있었습니다 → 문서를 신뢰하고 멀티-Send fan-out 그래프를 구축한 사용자는 단일 스레드에서 조용한 직렬 실행을 경험했습니다. 단위 테스트(가짜 스폰, 즉시 본문)로는 보이지 않으며, 실제 벽시계 시간 e2e에서만 노출됩니다.
  - `set_worker_count` / `set_worker_count_auto` Python docstring을 모두 다시 작성했습니다. `bindings/python/src/bind_graph.cpp` 실제 동작에 맞게 수정했습니다: `compile()` 기본값은 1이고, `set_worker_count_auto()` / `set_worker_count(N>=2)` 는 명시적 옵트인입니다.
  - `include/neograph/graph/engine.h`의 두 Doxygen 주석을 그에 따라 수정했습니다. Doxygen Pages는 master 푸시 시 자동 재빌드됩니다.
  - `docs/concepts.md` / `docs/troubleshooting.md` / `docs/reference-en.md`에서 동일한 오래된 주장(기본값 = hardware_concurrency)을 수정했습니다.

- **v0.9.0에서 누락된 API 마이그레이션 3건이 보완되어 제공됩니다.** v1.0 준비 주기의 PR `9b` (`19819d8`)은 `GraphNode` 레거시 8-가상 체인을 파괴적으로 제거했지만, PR `#48` (`6e654ad`, "C++ 예제가 `GraphNode::run()`로 마이그레이션")은 `examples/`만 마이그레이션했습니다 — 다음 3개 파일이 누락되어 v0.9.0이 빌드가 깨진 상태로 출시되었습니다:
    - `benchmarks/stress/bench_sustained_concurrent.cpp` (Phase 3
      sustained-burst verification key benchmark)
    - `benchmarks/concurrent/bench_concurrent_neograph.cpp` (memory/
      concurrency comparison matrix body against LangGraph and other engines)
    - `wasm/smoke.cpp` (Phase 1 WASM feasibility smoke)

CI는 이러한 대상들을 add_executables로 감지하지 못했거나 (Docker build dependency) 별도의 환경에서 격리하지 않았으므로 master merge와 tag 통과.

**수정**: 세 파일 모두 `std::vector<ChannelWrite> execute(const
  GraphState&) override` → `asio::awaitable<NodeOutput> run(NodeInput in)
  override` + `co_return out` 패턴으로 마이그레이션되었습니다. 노드 로직은 변경되지 않았습니다.

**v1.0 핵심 판매 포인트 네이티브 재검증** (`benchmarks/concurrent/results_v0.9.0_native_recheck.jsonl`):
    - Concurrency 10K · wall 10–23 ms · p99 17–21 µs · peak RSS **5.6 MB**
      (matches v0.3.0 / v0.5.0 measurements — no memory selling-point
      regression after destructive 9b)
    - 0 errors at 10K
  **Docker matrix (LangGraph / Haystack / pydantic-graph / LlamaIndex /
  AutoGen 6-way comparison) also re-measured within the same session**
  (`results_v0.9.0_docker_recheck.jsonl`).

매트릭스 재실행 중 누락된 API 마이그레이션과 함께 독립적인 회귀 문제가 하나 발견되었습니다 — `benchmarks/concurrent/Dockerfile.neograph`는 마스터의 CMake 옵션 기본값 변경을 추적하지 못해 전혀 빌드할 수 없었습니다(v0.9.0 출시 시점과 동일). 시간이 지나면서 다음 옵션 기본값이 OFF → ON으로 전환되었습니다:
    - `NEOGRAPH_BUILD_POSTGRES` / `NEOGRAPH_BUILD_SQLITE`
      (requiring `libpq-dev` / `libsqlite3-dev` respectively)
    - `NEOGRAPH_BUILD_A2A` / `NEOGRAPH_BUILD_ACP`
    - `NEOGRAPH_USE_LIBCURL` (one prior incident closed in
      `feedback_libcurl_unconditional_dep.md` — only the option toggle was
      added while the default remained ON, breaking the empty-container build
      path again)
    - `find_package(OpenSSL REQUIRED)` is unconditional without an option
      toggle (CMakeLists.txt:256) — separate v1.0 cleanup candidate

**Dockerfile 수정**: `libssl-dev` apt 추가 + 모든 비핵심 옵션을 명시적 `-DNEOGRAPH_BUILD_*=OFF` / `-DNEOGRAPH_USE_LIBCURL=OFF`로 고정. 주석에는 "두 번의 드리프트 사고로 인한 명시적 고정"이라고 명시되어 있습니다. CMakeLists.txt의 `find_package(OpenSSL REQUIRED)` 조건부 처리는 별도 작업으로 남겨두었습니다 — 다른 빌드 경로(PyPI wheel, ARM64 등)에 대한 영향 검증이 필요합니다.

**6-way matrix 주요 결과**(동시성=10000, 2 cpus / 1 GiB):

    | engine          | mode          | wall_ms | p99_us      | peak_MB | ok/err |
  |---|---|---|---|---|---|
    | **neograph**    | threadpool    | **16**  | **18**      | **5.1** | 10000/0 |
    | pydantic-graph  | asyncio       | 895     | 160         | 42.8    | 10000/0 |
    | haystack        | mp-pool-8     | 1472    | 2972        | 68.3    | 10000/0 |
    | langgraph       | mp-pool-8     | 3802    | 74415       | 60.6    | 10000/0 |
    | autogen         | mp-pool-8     | 22428   | 82361       | 49.1    | 10000/0 |
    | llamaindex      | asyncio       | 26303   | 25912204    | 582.7   | 10000/0 |

NG vs LangGraph(마케팅 비교 축): 벽 **237× 더 빠름**, p99 **4134× 더 빠름**, 최대 RSS **12× 더 낮음**.

**극한 시나리오**(동시성=10000, 1 cpu / 512 MiB):
    - NG: 8 ms / 5.2 MB / 0 err / **ok**
    - LangGraph mp-pool-8: 7821 ms / 60.9 MB / 0 err / ok
    - **LlamaIndex asyncio: OOM killed** (exceeded 512 MB cap)
    - **AutoGen asyncio: OOM killed**

동일한 v0.3.0 / v0.5.0 측정값 — 파괴적인 9b 이후 NeoGraph의 "10K 동시 워커, 최대 RSS 5MB, OOM 없음" 판매 포인트의 **회귀 없음**.

## [0.9.0] — 2026-05-14 — v1.0 준비(Candidate 1 Phase B + Candidate 6)

ROADMAP_v1.md의 두 v1.0 단일 디스패치 통합이 한 주기에서 수렴합니다:

  - **후보 1 Phase B (`9b`–`9f`)** — `GraphNode`의 레거시 8개 가상(`execute` / `execute_async` / `execute_stream` / `execute_stream_async` / `execute_full` / `execute_full_async` / `execute_full_stream` / `execute_full_stream_async`) + `add_cancel_hook` + `CurrentCancelTokenScope` + `state.
    run_cancel_token_` + 6개 `PyGraphNodeOwner` 레거시 오버라이드가 모두 제거되었습니다. **파괴적** — 지원 중단 창이 종료되었습니다. 사용자 GraphNode 하위 클래스 / 사용자 Python 노드는 단일 메서드 `run(NodeInput)` / `def run(self, input)`로 마이그레이션해야 합니다.
  - **후보 6** — `Provider` 4-가상 교차곱 → 1-가상 `invoke()`. 여전히 추가 + 지원 중단 단계에 있습니다 — 레거시 4개 가상은 변경되지 않고 기능하며, 지원 중단 경고만 표시됩니다. 해당 측의 Phase B(`Provider` 레거시 제거)도 v1.0.0 출시 직전에 종료됩니다.

동일한 주기에는 b59444f의 잠재적 병렬 회귀 되돌리기(`e5ecb08`) + 명시적 fan-out 예제 호출 + CI 환경 수정 3건(httplib 매크로 가드 / Windows MSVC unistd.h / pybind pytest 마이그레이션)이 포함되어 있으며, 모두 이 [Unreleased]의 일부입니다.

### 추가됨

- **`Provider::invoke(params, on_chunk = nullptr)`** — v1.0 표준 단일 디스패치 진입점. 비스트리밍(`on_chunk == nullptr`)과 스트리밍(`on_chunk` 제공)을 하나의 메서드로 처리합니다. 이전 4-가상 교차곱(`complete` / `complete_async` / `complete_stream` / `complete_stream_async`)을 하나의 비동기 스트리밍 슈퍼셋으로 통합합니다. 기본 구현은 4개 레거시 가상으로 전달되어 기존 Provider 하위클래스가 변경 없이 작동합니다. 새 ctest 6개(`ProviderInvokeDefault`). (PR #40)
- **`invoke()` 취소 전파 패리티** — `params.cancel_token`이 설정되지 않고 엔진 스레드-로컬 범위가 활성화된 경우 `current_cancel_token()`이 자동으로 스탬프됩니다. 레거시 동기 `complete()` 동작과 동일합니다(엔진 내부의 노드 본문이 `provider->invoke(params, ...)`를 호출하면 실행 중인 그래프의 취소 신호를 자동으로 수신). 새 ctest 3개 추가(`InvokeCancelPropagation`). (PR #43)
### 변경됨

- **엔진의 모든 내부 LLM 호출이 `invoke()`로 라우팅됨** — `LLMCallNode`, `IntentClassifierNode` (PR #41/#42), `Agent::complete` / `Agent::run_stream` (PR #43), `SupervisorLLMNode` / `ResearcherLLMNode` / `CompressNotesNode` / `FinalReportNode` (PR #43), `PlannerNode` / `ExecutorNode` (PR #44). NeoGraph 내 LLM 디스패치가 단일 표면으로 통합되었습니다.
- **C++ 예제 마이그레이션(파일 2개)** — `31_local_transformer.cpp`, `cookbook/ai-assembly/member_server.cpp`는 이제 새 `invoke()`를 사용합니다. 사용자 빌드에서 더 이상 사용 중단 경고가 없습니다. (PR #45)
- **`GraphEngine::compile()` 기본 작업자 수가 1로 되돌아감** (`e5ecb08`). `b59444f`는 잠복된 18일(2026-04-26 → 2026-05-13) 병렬 마이크로 벤치마크 회귀(11.8 → 283 µs, 24배)의 근본 원인이었으며, 커밋은 이분법(병렬로 11개의 작업 트리)을 통해 정확히 찾아냈습니다. v1.0부터 기본값=1(CPU-소형 순차/병렬 디스패치에 최적); 의도적인 fan-out을 위해 `engine->set_worker_count_auto()` 한 줄을 추가하여 hardware_concurrency를 엽니다. 영향을 받는 5개 fan-out 예제(10/14/21/36 + deep_research_graph 빌더)에 명시적 호출이 추가되었습니다. 자세한 내용은 ROADMAP_v1.md의 "Perf retrospective" 섹션을 참조하십시오.

### 폐기 예정

- **`Provider::complete` / `complete_async` / `complete_stream` / `complete_stream_async`** — 4개의 레거시 가상 함수 모두 `[[deprecated("v1.0 single-dispatch: use invoke(...)")]]` 마커를 보유합니다. 레거시 메서드는 더 이상 사용 중단 기간 동안 그대로 작동합니다. v1.0.0에서 제거됩니다. 내부 포워더는 `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED`로 래핑되어 경고가 사용자 대상 오버라이드/호출 지점에서만 나타납니다. (PR #44)

### 제거됨(후보 1 단계 B — 파괴적)

- **`GraphNode` 레거시 8개 가상 함수** — `execute(GraphState&)` / `execute_full(...)` / 6개 변형 + `ExecuteDefaultGuard` 재귀 가드
  + 300줄 이상의 기본 체인. 모두 제거됨. `run(NodeInput)`가 유일한 순수 가상 함수입니다. (커밋 `19819d8`)
- **`add_cancel_hook` + `Hook` RAII + `hooks_*` 멤버 + `cancel()` 훅 반복** — `cancel.h`는 `fork()` + `cancel()` + `is_cancelled()` + `slot()`만 유지합니다. (커밋 `1d786a5`)
- **`CurrentCancelTokenScope` + `current_cancel_token()` thread_local + `GraphState::run_cancel_token_` + 접근자 3개** — `RunContext::cancel_token`는 유일한 취소 채널입니다. `src/core/cancel.cpp`는 스텁으로 축소됨(파일 자체는 향후 삭제 후보). (커밋 `9e8e956`)
- **6개 `PyGraphNodeOwner` 레거시 오버라이드** — pybind 트램폴린은 `run(self, input)`만 호출합니다. Python 사용자 코드도 v0.9.0부터 단일 메서드를 요구합니다. (커밋 `9e8e956`)
- **오래된 pytest 파일 2개** — `test_execute_stream_dispatch.py` (v0.3.2 스트림 전용 폴백 디스패치 검증) + `test_streaming_only_error_
  hint.py` (execute_full_stream이 우선 — v1.0에서 의미 없음). (커밋 `4392fbb`)

### 수정됨

- **5개 fan-out 예제에 명시적 호출 추가** — `e5ecb08`의 기본 작업자 수 되돌림으로 묻힌 실제 병렬 의도를 복원: `examples/10_send_command.cpp`, `examples/14_plan_executor.cpp`, `examples/21_mcp_fanout.cpp`, `examples/36_classifier_fanout.cpp`, `src/core/deep_research_graph.cpp`의 `create_deep_research_graph()` 빌더가 이제 `set_worker_count_auto()`를 호출합니다. 검증: `classifier_fanout` 4.22배 속도 향상(25.2 ms 순차 → 6.0 ms 병렬). (커밋 `99c470b`)
- **`bench_async_http` httplib 매크로 가드** — `bench_async_http.cpp` 포함 `<httplib.h>` 경유 `<neograph/async/conn_pool.h>` 그러나 `CPPHTTPLIB_OPENSSL_SUPPORT` 정의되지 않아 ODR 가드가 거부했습니다. CMake 타겟에 `target_compile_definitions(... PRIVATE ...)` 추가됨. (커밋 `d4be42a`)
- **Windows MSVC `unistd.h` 누락** — `test_schema_provider_extra_
  fields_temperature.cpp`는 POSIX 전용 `mkstemps` + `close`를 사용하여 Windows 빌드가 완전히 실패했습니다. 전체 파일을 `#ifndef _WIN32` 가드로 래핑했습니다(커버리지는 Linux/macOS에서 보장). (커밋 `3c49f12`)
- **Python 테스트 16개 마이그레이션 완료** — wheel CI pytest가 `AttributeError` 레거시 `def execute(self, state)` 패턴을 사용하는 28개 노드 클래스에서 발생했습니다. 배치 마이그레이션을 `def run(self, input)`로 수행했으며, 스트리밍 노드에는 `input.stream_cb` None-가드를 추가했습니다. (커밋 `4392fbb`)

### 마이그레이션(사용자 코드)

**공급자 호출 (후보 6 — 폐기 단계)**

새 코드:
```cpp
// non-streaming
auto completion = co_await provider->invoke(params, nullptr);

// streaming
auto completion = co_await provider->invoke(params, on_chunk);

// sync site (replaces old complete())
auto completion = neograph::async::run_sync(provider->invoke(params, nullptr));
```

레거시 가상 오버라이드 4개는 폐기 예고 기간 동안 계속 작동하지만, `-Wdeprecated-declarations` 경고는 사용자 오버라이드 사이트에서 표시됩니다. 제거는 v1.0.0 직전에 이루어지며, 폐기 예고 기간 내 마이그레이션이 권장됩니다.

**`GraphNode` 서브클래스 (후보 1 Phase B — 파괴적)**

C++ 코드:
```cpp
// old (up to v0.8.x)
class MyNode : public GraphNode {
    NodeResult execute_full(const GraphState& state) override {
        auto x = state.get("x");
        NodeResult out;
        out.writes.push_back(ChannelWrite{"y", json(/*...*/)});
        return out;
    }
};

// v0.9.0+ current code (single method, coroutine entry)
class MyNode : public GraphNode {
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto x = in.state.get("x");
        // in.ctx.cancel_token / in.ctx.step / in.stream_cb also accessible
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"y", json(/*...*/)});
        co_return out;
    }
};
```

Python 코드:
```python
# old (up to v0.8.x)
class MyNode(neograph_engine.GraphNode):
    def execute(self, state):
        x = state.get("x") or 0
        return [neograph_engine.ChannelWrite("y", x * 2)]

# v0.9.0+ current code
class MyNode(neograph_engine.GraphNode):
    def run(self, input):
        state = input.state  # input.ctx.cancel_token / input.stream_cb etc. also accessible
        x = state.get("x") or 0
        return [neograph_engine.ChannelWrite("y", x * 2)]
```

**Fan-out 의도(worker count 기본값 변경)**

```cpp
// old (v0.x April+): default was hardware_concurrency but micro-bench burden
// v1.0: default=1. Add one line for intentional fan-out.
auto engine = GraphEngine::compile(def, ctx);
engine->set_worker_count_auto();  // ← this line added (hardware_concurrency)
// or engine->set_worker_count(N);  // explicit N
```

`docs/migration-v0.4-to-v1.0.md`의 마이그레이션 1/2/3 섹션(run() / ctx.cancel_token / worker count 기본값) + Provider 섹션(다음 문서 정리에서 추가 예정)은 사례별 before/after 지침을 제공합니다.

## [0.8.0] — 2026-05-13 — DX 정책 + 다운스트림 주도 API 격차 해소

실제 다운스트림(ProjectDatePop) 피드백과 내부 커버리지 차이에서 드러난 8개 이슈(#22, #25, #26, #27, #28, #34, #35 + #16 후속)를 단일 마이너 버전 업으로 묶습니다. 공개 헬퍼 2개(`RunResult::channel<T>`, `RunContext::store`), 오프라인 예제 11개, `docs/migration-v0.4-to-v1.0.md` 마이그레이션 가이드, 그리고 신규 사용자가 처음 30분 안에 겪는 마찰을 줄이는 5개 항목 DX 번들을 포함합니다.

### 추가됨

- **`RunResult::channel<T>(name)` / `channel_raw(name)` / `has_channel(name)`** — 결과에서 채널 값을 추출하는 한 줄 헬퍼입니다. 두 출력 형태(중첩 `output["channels"][name]["value"]` 표준 + `react_graph` 같은 빌더가 추가한 플랫 키)가 자동으로 처리됩니다. ctest 9개 추가. (이슈 #25)
- **`RunContext::store`** — 노드 본문이 한 줄 `in.ctx.store->get(ns, key)`로 Store에 도달합니다. 기존 패턴(`shared_ptr<Store>`를 `NodeFactory` 람다에서 캡처)은 여전히 작동하며, 새 코드는 새 형태만 필요합니다. ctest 3개 추가. (이슈 #27)
- **`Provider::complete_stream` 비순수 기본 본문** — 최소 mock / 테스트 픽스처는 `complete()`만 오버라이드하면 됩니다. 기존 스트리밍 네이티브 오버라이드는 변경되지 않습니다. ctest 2개 추가. (이슈 #22)
- **`neograph::json` 배열 `.front()` / `.back()`** — nlohmann 근육 기억 패턴(`msgs.back()["content"]`)이 이제 컴파일됩니다. ctest 4개 추가. (이슈 #26)
- **오프라인 예제 11개 (41-51)** — `resume_if_exists_chat`, `custom_reducer_condition`, `store_personalization`, `request_queue_backpressure`, `cancel_token`, `node_cache`, `sqlite_checkpoint`, `openinference`, `async_tool`, `minimal`. 모두 rc=0, API 키 / 외부 서비스 의존성 없음. 이전에 참조가 전혀 없던 27/53 `NEOGRAPH_API` 클래스 사이의 공백을 메웁니다.
- **`examples/51_minimal.cpp`** — 노드 1개, LLM 없음, 도구 없음, mock 프로바이더 없음의 30줄 소개 예시. NeoGraph가 5분 안에 어떻게 실행되는지 이해합니다.
- **`docs/migration-v0.4-to-v1.0.md`** — 사례별 before/after 4가지 예시 + `[[deprecated]]` 기존 8-가상 체인(`execute` / `execute_async` 등)에서 새 `run(NodeInput) ->
  awaitable<NodeOutput>`로 마이그레이션할 때 흔한 실수. 또한 `NEOGRAPH_DEPRECATED_VIRTUAL` 매크로 메시지에서 링크됨.
- **README "일반적인 함정 5" 섹션** — 신규 사용자가 처음 30분 안에 겪는 다섯 가지 사항(`channel<T>` 사용법, `in.ctx.store`, `neograph::graph::` 하위 네임스페이스, `<httplib.h>` 매크로, GCC 13 코루틴 ICE)을 한곳에 정리. 각 항목에는 수정 방법 + 관련 예제/이슈 링크가 포함됨.
- **컴파일 타임 `#error` 가드 (`include/neograph/api.h`)** — 사용자 TU가 `<httplib.h>` 를 NeoGraph 헤더 앞에 포함하되 `CPPHTTPLIB_OPENSSL_SUPPORT`없이 포함하면, 컴파일이 명확한 메시지와 옵트아웃 매크로 (`NEOGRAPH_SKIP_HTTPLIB_MACRO_GUARD`)와 함께 실패합니다. 기존 #16 런타임 SEGV를 컴파일 타임 실패로 승격시킵니다.
- **`example_minimal` 5개의 새로운 친화적 오류 메시지 ctest** — `Unknown reducer` / `Unknown condition` / `Unknown node type` / `Write to
  unknown channel` 메시지에 사용 가능한 이름 + 등록 방법 + 문제 해결 링크를 메시지 본문에 포함하는 계약 잠금.
- **`docs/troubleshooting.md` 4개 새 항목** — Tracer 어댑터 `close()` 중단/충돌(#24), GCC 13 코루틴 ICE(#23), 친화적 오류 메시지 지침(#22), `RunResult::output` 형태(#25).
- **`Tracer` + `OpenInferenceTracerSession::close()` `@warning` 블록** — 어댑터 작성자를 위한 원시 포인터 함정을 명시적으로 문서화. `RecordedSpan` + 래퍼 분리 패턴을 올바른 접근 방식으로 지적. 기존 `tests/test_openinference_cpp.cpp::InMemoryTracer` + 새 `examples/49_openinference.cpp::PrintTracer` 참조. (이슈 #24)

### 수정됨

- **`SchemaProvider::build_body` `extra_fields`가 `params.tools`일 때 조용히 누락됨.** 이전 코드는 `extra_fields` 적용을 `if (!params.tools.empty())` 내부에서 게이팅하여, `reasoning` 및 `response_format` 같은 핵심 스키마 필드가 도구 없는 호출에서 완전히 사라지게 했습니다. 수정: 도구 분기 밖으로 이동하여 항상 적용되도록 함. ctest 3개 추가. (이슈 #34)
- **`temperature_path` 스키마 측 옵트아웃.** 추론 모델(gpt-5.x, o-시리즈)은 `temperature`와 `reasoning.effort`가 상호 배타적이지만, 스키마에는 "이 공급자는 temperature를 허용하지 않음"을 선언할 방법이 없어 모든 호출에서 `params.temperature = -1.0f` 센티널 우회 작업을 강제함. 수정: 스키마에서 `"temperature_path": null`를 지정하면 build_body가 이를 완전히 건너뜀. ctest 4개 추가. (이슈 #35)
- **친화적 RuntimeError 메시지** — `ReducerRegistry::get` / `ConditionRegistry::get` / `NodeFactory::create` "알 수 없는 <thing>: foo" 및 `GraphState::write` / `apply_writes` `Write to unknown channel` 이제 사용 가능한 이름 + 등록 절차 + 문제 해결 링크를 메시지 본문에 포함. 신규 사용자는 메시지만으로 다음 단계를 결정할 수 있음.
- **`SchemaProvider::complete_stream_async` HTTP/SSE 분기** 이제 수명이 긴 전용 `bridge_thread_`에서 디스패치됨(기존: `Provider` 기본값이 호출마다 새 `std::thread`를 생성). 기존 동작은 콜드 스레드 로컬 리졸버/NSS 상태에서 glibc `internal_strlen`의 SEGV를 유발함. WS 분기는 이미 네이티브 co_await이므로 영향 없음. awaiter의 실행기에서 토큰 디스패치 유지(PR #10 불변). (이슈 #16)
- **`example/09_all_features.cpp`** Store 데모 — 노드 본문 읽기 패턴에 대한 `examples/43_store_personalization.cpp`를 가리키는 docstring 포인터 추가. 옵션 2 — 옵션 3(인라인 라이브 노드)은 #27의 `RunContext::store`가 도착하면 함께 정리 예정. (이슈 #28)

### 문서

- `RunResult::output`(채널 래핑)의 표준 형태와 `react_graph` 같은 빌더가 추가하는 플랫 키 프로젝션과의 관계가 헤더 docstring에 문서화됨. 새 헬퍼(`channel<T>` / `channel_raw` / `has_channel`) 사용 권장. (이슈 #25)
- `RunContext::store` 필드 `@brief` 블록 — 두 가지 플러밍 패턴(`in.ctx.store` 권장 / 기존 팩토리 클로저 캡처 호환)을 코드 예시와 함께 나란히 제시. (이슈 #27)
- 두 경로 모두 `examples/43_store_personalization.cpp` 파일 헤더 주석에 문서화됨.

## [0.7.0] — 2026-05-11 — C++ openinference + 비동기 스트리밍 브리지

v0.6.0에 대해 제기된 네 가지 이슈를 하나의 마이너 버전 업으로 종결합니다. 핵심: `Provider::complete_stream_async` 기본값이 외부 엔진 코루틴 내부에서 대기될 때 더 이상 세그폴트가 발생하지 않습니다(이슈 #4) — NeoGraph 앞에 있는 SSE/스트리밍 HTTP 백엔드의 가장 일반적인 형태. 동반: v0.6.0 Python OpenInference 레이어의 C++ 대응물로, Phoenix / Arize / Langfuse가 C++ 기반 트레이스를 Python 기반 트레이스와 동일하게 렌더링합니다(이슈 #9). 추가: 미용적 Python OTel 분리 노이즈가 제거되었고(이슈 #2), 동일한 `thread_id` 동시 실행 + `schema_mutex_` × on_chunk 잠금 불변식이 이제 docstring에 고정되었습니다(이슈 #6).

### 추가됨

- `neograph_engine.openinference`의 C++ 대응물(이슈 #9). 새 `neograph::observability` 모듈은 두 부분을 다룹니다:
  - `Tracer` / `Span` — NeoGraph 자체가 opentelemetry-cpp를 끌어오지 않도록 하는 작은 무의존성 추상 인터페이스입니다. 다운스트림은 자체 백엔드(OTel SDK, 인메모리 테스트 페이크, 로깅 레코더 등)를 감싸는 어댑터를 제공합니다. 4개의 속성 세터(string, int64, double, bool — bool은 `set_attribute_bool` 리터럴이 실수로 이를 해석하지 못하도록 의도적으로 `const char*`로 이름 변경됨)와 스트리밍 토큰 진단용 `add_event`, 상태, `end()`가 포함됩니다.
  - `openinference_tracer(tracer)` — CHAIN 종류의 루트 스팬을 열고, `OpenInferenceTracerSession`를 반환하며, 그 `cb` 필드는 `engine.run_stream()`에 연결되고 노드마다 CHAIN 종류의 자식 스팬을 열며, `NODE_START`/`END` 페이로드를 `input.value` / `output.value` JSON 블롭에 넣고 `LLM_TOKEN` 이벤트를 개별 스팬 이벤트로 기록합니다.
  - `OpenInferenceProvider(inner, tracer)` — 모든 `Provider`를 래핑하고, 모든`llm.model_name`, `llm.invocation_parameters`, `llm.input_messages.{i}.message.{role,content}`, `llm.output_messages.0.message.{role,content}`, `llm.token_count.{prompt,completion,total}`)을 첨부합니다. 스트리밍 오버로드는 또한 `complete*` 호출에 OpenInference LLM 종류 속성 집합( `llm.token` 이벤트와 최종 조합된 `output.value`.
  - `tests/test_openinference_cpp.cpp`의 7개 패리티 테스트가 `InMemoryTracer` 참조 어댑터를 구동합니다 — 루트 + 노드별 CHAIN 스팬 계층, ERROR / INTERRUPT 상태 표면화, LLM_TOKEN 스팬 이벤트 기록, 세션 종료 시 지연 스팬 정리, LLM 공급자 속성 집합, 스트리밍 토큰 이벤트, 예외 상태 전파를 검증합니다.

### 수정됨

- `Provider::complete_stream_async` 기본 브리지는 더 이상 스트림 기간 동안 대기 중인 코루틴의 실행기를 차단하지 않습니다. 수정 전 기본값은 `co_return complete_stream(...)` 인라인이었으며, 이는 (a) 엔진의 `io_context` worker 스레드를 HTTP/SSE recv 루프 전체 동안 중단시켜 — 동일한 executor의 다른 노드 코루틴이 지연되었고 — (b) `SchemaProvider`의 WebSocket Responses 분기의 경우, 추가로 `run_sync` io_context를 `run_sync(complete_stream_ws_responses(...))`를 통해 엔진 worker 위에 중첩시켜, 공유 provider 상태에 대한 경합을 일으키고 외부 `GraphEngine::run_stream_async`내부에서 호출될 때 간헐적 segfault를 발생시켰습니다. 새 기본값은 동기식 `complete_stream`를 위해 전용 worker 스레드를 생성하고, 각 토큰을 awaiter의 executor로 다시 디스패치하며(사용자의 `on_chunk` 가 대기 중인 코루틴과 단일 스레드로 실행됨 — 재진입 없음), 일회성 `steady_timer.cancel()`를 통해 코루틴을 재개합니다. Worker 스레드 예외는 awaiter에서 다시 발생합니다. `SchemaProvider` 는 WebSocket 경로에 대해 worker 스레드조차 건너뛰는 네이티브 `complete_stream_async` 오버라이드를 추가하여 `co_await`를 직접 `complete_stream_ws_responses`. `OpenAIProvider` 합니다. `tests/test_provider_async_default.cpp`: `StreamAsyncBridgeDoesNotBlockExecutor` 의 두 가지 새 테스트: `StreamAsyncBridgeRethrowsWorkerException`(동시 티커 코루틴이 스트림 중에 진행되고 청크가 worker가 아닌 awaiter의 스레드에서 전달됨) 및

- `openinference_tracer`: OTel SDK가 종료할 때마다 stderr에 출력하던 `Failed to detach context` 트레이스백을 없앴습니다. 이 문제는 트레이서를 `engine.run_stream_async` 및 `StreamMode.ALL`과 함께 사용할 때 발생했습니다. NODE_START에서 생성된 OTel contextvars 토큰을 다른 `asyncio.Task`에서 분리하려 했습니다. NODE_END 콜백은 호출자 태스크가 아니라 엔진 continuation에서 실행되므로 `Context.reset(token)`이 `ValueError`를 발생시켰습니다. SDK가 예외 자체는 삼켰지만 전체 트레이스백을 `logger.exception`으로 계속 전달해, 의미에는 영향을 주지 않으면서 운영 로그를 오염시켰습니다. 수정 후에는 attach 시점의 스레드와 태스크를 기록하고 둘이 다르면 detach를 건너뜁니다. 또한 범위가 좁은 `logging.Filter`를 `opentelemetry.context`에 설치해 `_safe_detach`가 스택에 있을 때만 해당 메시지를 버립니다. 동기 호출자와 같은 태스크의 비동기 호출자는 여전히 노드 스팬 아래에 올바르게 중첩된 LLM 스팬을 얻습니다. (이슈 #2)

---

## [0.6.0] — 2026-05-07 — OpenInference 관측 가능성 레이어

LangSmith UX 격차를 해소합니다. NeoGraph는 이미 OTel 형태 스팬을 생성했습니다(그래프는 모든 OTel 백엔드로 흘렀습니다); 이 릴리스는 Phoenix / Arize / Langfuse가 추적을 일반적인 응용 스팬 목록이 아닌 채팅 버블 + 토큰 수 UI로 렌더링하기 위해 사용하는 LLM별 속성 레이어를 추가합니다. 로컬 Phoenix 컨테이너를 대상으로 종단간 검증됨 — writer→critic 그래프는 모델 이름, 프롬프트/응답 및 토큰 수가 Phoenix UI에서 표시되는 6-스팬 계층(CHAIN 루트 → 노드 스팬 → LLM 스팬)을 생성합니다.

### 추가됨

- `neograph_engine.openinference` 모듈:
  - `openinference_tracer(tracer)` — `otel_tracer`를 반영하지만 루트 및 노드 스팬에 `openinference.span.kind = "CHAIN"`를 태그하고 노드 페이로드를 `input.value` / `output.value` JSON 블롭에 넣는 컨텍스트 관리자.
  - `OpenInferenceProvider(inner, tracer)` — 모든 `Provider`를 감싸고, 모든 `complete()` 호출에서 `llm.complete` 자식 스팬을 열고 `span.kind = "LLM"`를 태그하며, `llm.model_name`, `llm.invocation_parameters`, `llm.input_messages.{i}.message.{role,content}`, `llm.output_messages.0.message.{role,content}`, `llm.token_count.{prompt,completion,total}`, 그리고 Langfuse 호환 `input.value` / `output.value` 블롭을 캡처합니다.
- `bindings/python/tests/test_openinference.py`의 테스트 4개 — InMemorySpanExporter의 속성 존재, 스팬 계층, 예외 경로, 노드 입력/출력 JSON blob에 대한 단언.

### 수정됨

- `openinference_tracer`는 이제 각 노드 스팬을 OTel *현재* 컨텍스트로 첨부한다(`otel_context.attach` 통해) — 노드 본문 내부에서 열린 자식 LLM 스팬이 노드 스팬 아래에 중첩되도록. 이것이 없으면 C++→Python pybind 콜백 경계를 가로지르는 contextvar 전파가 예상된 단일 트레이 트리 대신 실행당 3개 이상의 관련 없는 trace_id를 생성했다. 토큰은 NODE_END / ERROR / INTERRUPT에서 분리되어 이전 현재 스팬을 복원한다. 기존 `otel_tracer`가 문서화하는 것과 동일한 패턴 — `trace.use_span(...).__enter__()` 대신 명시적 attach/detach, 이는 일치하는 `__exit__` 없이 사용하기에 안전하지 않다.

### 메모

- OpenTelemetry는 여전히 선택적 의존성입니다. `neograph_engine.openinference`를 가져오면 `opentelemetry-api`가 설치되지 않은 경우에만 첫 사용 시 명확한 ImportError가 발생합니다. 가져오기 시점이 아닙니다.
- Phoenix 종단간 실행::

      docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix
      pip install opentelemetry-exporter-otlp

OTLP gRPC exporter를 `http://localhost:4317`로 구성하고 `http://localhost:6006`를 열어 트레이스를 확인하세요. 모듈 docstring에 전체 스니펫이 있습니다.

---

## [0.5.0] — 2026-05-07 — 바인딩 사용성 개선: 실시간 변경 목록 속성

바인딩을 통해 노출된 message / writes / sends 목록을 변경하는 가장 자연스러운 Python 관용구에서 발생하는 무음 무작동 함정을 해결합니다. 이전에는 `params.messages.append(msg)`가 복사본을 변경했고 기본 C++ 벡터는 새 항목을 보지 못했습니다 — 저하된 LLM 응답을 생성하는 무해한 실패(충돌 없음, 경고 없음)였습니다. 이제 `.append()`는 라이브 std::vector까지 전달합니다.

### 추가됨

- `bindings/python/src/opaque_types.h` — 다섯 가지 벡터 유형에 대한 `PYBIND11_MAKE_OPAQUE`: `std::vector<ChatMessage>`, `<ChatTool>`, `<ToolCall>`, `<graph::ChannelWrite>`, `<graph::Send>`.
- `module.cpp` `init_opaque_vectors` — `py::bind_vector`는 각각을 Python 클래스(`ChatMessageList`, `ChatToolList`, `ToolCallList`, `ChannelWriteList`, `SendList`)로 등록하며, 라이브 C++ 벡터에 대한 완전한 가변 시퀀스 프로토콜을 지원합니다.
- 각각에 대한 `py::implicitly_convertible<py::list, …>` — 기존의 빌드-후-할당 패턴(`params.messages = [m1, m2]`)은 변경 없이 계속 작동합니다. 할당은 Python 목록을 바인딩된 클래스로 자동 변환합니다.
- `bindings/python/examples/23_evolving_chat_agent.py` — 스레드별 진화하는 채팅 에이전트(라이브 LLM): 에이전트의 JSON 정의는 누적된 대화 기록을 기반으로 턴 사이에 다시 작성됩니다. 진화에 걸친 체크포인트-재개(이전 메시지 유지), `__graph_meta__` 감사 채널 패턴, 검증자 경계(노드 유형 화이트리스트, 필수 채널)를 보여줍니다.

### 변경됨

- `params.messages` / `.tools` / `chat_message.tool_calls` / `node_result.writes` / `.sends`는 이제 일반 `list` 대신 바인딩된 클래스를 반환합니다. `len()`, 반복, `__getitem__`, `__setitem__`, `.append()`, `.extend()`, 슬라이싱 — 모두 Python 목록처럼 동작합니다. `isinstance(x, list)`만 False를 반환합니다. 저장소 + 다운스트림 grep은 이러한 isinstance 호출 사이트가 0개임을 확인합니다.
- `.github/workflows/nightly.yml` — `ops/s ≥ 600K` 게이트를 제거합니다. `err=0` 및 `leak=false`로 4회 연속 실패 후, 임계값(로컬 하드웨어에서 969K ops/s로 보정)은 공유 GitHub 호스팅 러너에서 도달할 수 없었습니다(측정 233~273K ops/s, 로컬보다 3~4배 낮음). 처리량 회귀 감지는 PR 시점의 `bench-regression` 작업(안정적인 하드웨어, µs 단위의 단일 발송)에서 수행됩니다. 야간 소크의 실제 가치는 5분 동안 `err==0` + `leak_suspect==false`이며, 둘 다 하드 게이트로 유지됩니다.

### 메모

- `ChatMessage.image_urls` (`std::vector<std::string>`)는 의도적으로 마이그레이션하지 않았습니다 — `vector<string>`는 바인딩 전반에 걸쳐 너무 널리 사용되어 전역 OPAQUE를 적용하려면 모든 호출 지점을 정리해야 합니다. 남은 제한 사항으로 문서화됨; v0.6+ 후보.

---

## [0.4.0] — 2026-05-05 — v1.0 준비: 통합된 `run(NodeInput)` 디스패치

v1.0 정밀화 트랙(ROADMAP_v1.md)의 첫 릴리스입니다. 8개의 가상 `GraphNode` 교차 곱(`execute` / `execute_async` / `execute_full` / … / `execute_full_stream_async`)이 단일 표준 메서드인 `run(NodeInput) -> awaitable<NodeOutput>`로 축소됩니다. 실행별 취소 메타데이터는 비채널 집합 `GraphState` 멤버와 스레드 로컬 밀반입 채널에서 명시적 `RunContext` 인수로 이동합니다. `deadline` 및 `trace_id`는 예약된 확장 슬롯으로만 추가되었으며 `RunConfig`에 의해 채워지지 않습니다. `CancelToken`는 계층적 `fork()`을 획득하여 다중 전송 fan-out 작업자가 각각 부모의 `cancel()`가 계단식으로 전파되는 개인 신호를 소유합니다.

### 추가됨

- `RunContext` (`include/neograph/graph/engine.h`) — 명시적 실행별 메타데이터: 사용 가능한 `cancel_token`, `thread_id`, `step`, `stream_mode`, 예약된 `deadline` 및 `trace_id` 슬롯. 엔진은 모든 `NodeExecutor::run` 호출을 통해 이를 전달합니다. **PR 1, 커밋 `a473f0e`.**
- `GraphNode::run(NodeInput) -> awaitable<NodeOutput>` — 단일 표준 디스패치 진입점. `NodeInput { state, ctx,
  stream_cb }`; `NodeOutput { writes, command, sends }`. 기본 본문은 레거시 8개 가상 메서드로 전달되어 기존 하위 클래스가 계속 컴파일되도록 합니다. **PR 2, 커밋 `607ce66`.**
- `CancelToken::fork() -> shared_ptr<CancelToken>` — 자체 `cancellation_signal`를 가진 하위 토큰. 부모 `cancel()`는 모든 활성 하위 항목(및 재귀적으로 손자 항목)에 캐스케이드됩니다. `run_sync(aw, parent_token)`는 `parent_token->fork()`로 전환되어 각 중첩 작업이 자체 슬롯을 바인딩합니다 — v0.3.x의 emit-대-bind 경쟁 및 다중 Send 단일 핸들러 덮어쓰기를 해결합니다. v0.3.x `add_cancel_hook` 목록은 더 이상 사용되지 않음(deprecation)을 통해 계속 작동합니다. **PR 3, 커밋 `897645c`.**
- `[[deprecated]]` 8개의 레거시 `GraphNode` 가상 함수 + `add_cancel_hook`에 대해. 내부 호출 지점(graph_node.cpp 기본 체인, 기본 `run()` 포워더)은 새로운 `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` 매크로로 둘러싸여 있습니다(`api.h` — GCC / clang / MSVC 이식 가능). 더 이상 사용되지 않는 가상 함수를 재정의하는 사용자 코드는 마이그레이션 경고를 확인합니다. 엔진 내부는 깨끗하게 유지됩니다. **PR 4, 커밋 `35a4517`.**
- `engine.get_state_view(thread_id) -> StateView`는 이제 표준 상태 읽기입니다. 원시 사전 `engine.get_state(...)`는 docstring에서 소프트 비권장 처리됩니다(경고가 발생하지 않음 — 원시 사전은 유효한 탈출구로 유지됨). **PR 5, 커밋 `f31aa53`.**
- 7개의 C++ 및 19개의 Python 예제가 `run(NodeInput)`로 마이그레이션되었습니다. 스모크 실행은 v0.3.2 출력과 비트 단위로 일치합니다. **PR 6a/6b, 커밋 `a2a24ef` / `0a76e3a`.**
- Pybind `PyGraphNodeOwner`는 `run(NodeInput)`를 재정의하고 Python 사용자의 `run` 메서드(정의된 경우)로 디스패치하며, 그렇지 않으면 레거시 체인으로 폴백합니다. `RunContext` / `NodeInput` / `CancelToken`가 Python에 노출됩니다. `cancel_token`는 스레드 로컬 밀반입 없이 `input.ctx.cancel_token`로 접근 가능합니다. **PR 7, 커밋 `4e186a5`.**
- `docs/reference-en.md` §6 GraphNode는 단일 `run()`로 축소되었습니다. RunContext + `fork()` 예제 하위 섹션이 §7 아래에 추가되었습니다. README의 "Differences from LangGraph"에 "One node method" 항목이 추가되었습니다. **PR 8, 커밋 `519a00b`.**
- 내장 C++ 노드(`LLMCallNode`, `ToolDispatchNode`, `RouteToNode`)가 `run(NodeInput)` 재정의로 마이그레이션되었습니다. **PR 9a, 커밋 `d1070dc`.**
- 신규 사용자 모드 함정 수정: README CMake 스니펫은 `graph::` 하위 네임스페이스, cppdotenv 경로, `OpenAIProvider::create()` 대 `create_shared()`, nlohmann 하위 집합으로서의 `neograph::json`, 3-인수 vs 2-인수 `compile()`를 문서화합니다. Python `compile(def, ctx, store=None)` 키워드 인수가 추가되었습니다(추가적, 비호환성 없음). **커밋 `ee11ed6`.**

### 변경됨

- README: "10K-worker 측정 스트레스 테스트" 섹션 — neoclaw에서 RTX 4070 Ti + Gemma 4 E2B Q4, N=10000 완료 @ 0 err / 424s / 2572 MB 피크 / ~1 KB 한계 워커 비용 / p99 648 ms (`7840b81`).
- README: "프로덕션 경제성" 섹션 — 플릿 안전성 + RAM 델타 프레이밍 (`b82b15a`).
- README: LangGraph 델타 목록의 "Docker 불필요" + "의존성 드리프트 면역" 불릿 (`333b482`, `a6061d7`).

### 폐기 예정

- `GraphNode::execute / execute_async / execute_full /
  execute_full_async / execute_stream / execute_stream_async /
  execute_full_stream / execute_full_stream_async` — v0.5.x까지 `[[deprecated]]` 주석과 함께 작동 유지, v1.0에서 제거됨.
- `CancelToken::add_cancel_hook` — `fork()`로 대체됨. 동일한 폐기 기간.

### 메모

- 검증: v0.4.0 태그에서 442 → 452 ctest(3개 NodeRunDispatch + 7개 CancelTokenFork 추가) + 96 pytest + 5개 라이브 LLM/WS 모두 통과.
- 하위 PR(`run(const NodeInput&)` 참조 매개변수)이 pybind 비동기 경로에서 v0.2.0 RunConfig 코루틴 참조 UAF 크래시 형태를 유발함. 병합 전에 수정 완료: `NodeInput in` 값 전달 방식. `node.h`에 문서화됨.

---

## [0.3.2] — 2026-05-05 — 취소 전파 강화(5 라운드)

v0.3.0 단발 취소가 남긴 격차를 메우는 5라운드 패치 시리즈: Send fan-out 전파, 프로세스 내 폴링, Python용 훅, C++ 범위, 예외 타입. 또한 FastAPI SSE 채팅 데모 평가에서 나온 TODO_v0.3.md 피드백 배치를 반영 — `resume_if_exists`, dict-or-list `update_state`, 타입 상태 읽기를 위한 StateView.

### 추가됨

- `RunConfig::resume_if_exists` — 명시적 `resume()` 호출 없이 이전 스레드의 체크포인트를 옵트인 재개. 표준 다중 턴 채팅 의미론: `engine.run(cfg)`는 `thread_id`가 존재하면 대화를 계속함.
- `engine.update_state(thread_id, dict | list[ChannelWrite],
  as_node="")` — 두 형태 모두 허용. 수정 전에는 `dict`만 작동했고, 리스트 전달은 조용히 no-op였음. 리스트 형태는 모든 노드 본문의 emit 형태와 대칭적임.
- `StateView` (`bindings/python/neograph_engine/state_view.py`) — Pydantic 타입 상태 읽기. `engine.get_state_view(thread_id) ->
  StateView`는 평면 점 접근(`view.messages` / `view.foo`)에 더해 dict 이스케이프 해치용 `view.raw`를 반환. 타입 채널 정의를 위한 서브클래스: `class ChatState(ng.StateView): messages: list[dict] = []`.
- `bindings/python/tests/test_async_cancel_live_llm_fanout.py` — 중간 취소가 실제로 Send로 생성된 모든 형제를 소켓 계층에서 중단하는지 검증(이것이 v0.3.1 근본 원인 패치였음).
- `examples/22_self_evolving_graph.py` — TODO_v0.3.md #9 cookbook 폴드와 함께 v0.3.2로 이동됨.
- ROADMAP_v1.md — 취소 라운드 사후 분석에서 도출된 설계 정교화 후보(단일 dispatch, RunContext, 계층 CancelToken — 모두 v0.4.0에서 전달됨).
- Doxygen `/* */` 와일드카드 수정 — `acp/types.h` 에는 `/**` 블록에 경로 와일드카드(`fs/*`, `terminal/*`)가 포함되어 중첩 주석이 열리고 이후의 모든 진단이 억제되었습니다. 다음으로 대체됨: `&#42;` HTML 엔티티.

### 수정됨

- 취소 전파, 누적 5라운드:
  1. v0.3.0 단일 노드 — `cancel_token`가 `Provider::complete`에 도달합니다.
  2. v0.3.1 다중 Send 포인터 드롭 — fan-out 워커가 이제 `run_cancel_token_shared()`를 공유합니다(`init_state +
     restore`가 채널 세트 외부에서 워커별 상태를 재구축할 때 손실되었음).
  3. v0.3.1+ 프로세스 내 폴링 — 엔진 슈퍼-단계 loop가 LLM I/O 단계에서뿐만 아니라 step들 사이에서도 주기적으로 폴링합니다.
  4. v0.3.2 Python용 Hook — `add_cancel_hook`가 실행별 토큰에 콜백을 등록하고 `cancel()`에서 발생합니다. 동기 Python `execute()`가 스레드-로컬 범위 없이 임시 취소 핸들러를 설치할 수 있게 합니다.
  5. v0.3.2 C++ 범위 + 재시도 + 예외 유형 — 메인 스레드에서 새로 던지는 `NodeInterrupt`(libstdc++ `__exception_ptr::_M_release` 경쟁 회피), 재시도 예산이 취소를 존중하며, 런타임-대-로직 예외 분리.
- `execute_stream`-전용 Python 노드가 기본 `execute` 경로(NotImplementedError)로 조용히 빠져나갔습니다. 이제 사용자가 스트리밍 변형만 재정의한 경우 `run_stream`가 `execute_stream`를 직접 연결합니다.
- `update_state`가 list[ChannelWrite]를 수용 — 조용한 no-op을 종료합니다(TODO_v0.3.md #5).

### 메모

- 442 ctest + 96 pytest + 2 라이브 LLM(단일 + fan-out 취소)이 v0.3.2 태그(`915e90e`)에서 그린(green) 상태입니다.
- 27/30 C++ 예제 + 20/22 Python 예제가 `examples/run_all.py`에서 통과합니다. 건너뛴 테스트는 외부 서비스(Postgres / Crawl4AI / 라이브 OpenAI)가 필요합니다.
- Valgrind 6개 예제에서 오류 0건, 815개의 allocs / 815개의 frees가 깨끗함.
- 벤치 중앙값은 seq 경로에서 반복당 5.185 µs(v0.3.0 기준선) — 이번 라운드 전체에서 성능 회귀 0건.

---

## [0.3.0] — 2026-05-04 — 협력적 취소 전파(Cooperative cancel propagation)

FastAPI SSE 채팅 데모 평가 중 보고된 프로덕션 비용 누수 격차를 해소합니다: 프론트엔드 `AbortController`가 asyncio 작업을 취소해도 업스트림 OpenAI 요청이 완료될 때까지 실행되지 않습니다. 취소가 실행의 모든 계층을 통해 전파됩니다.

### 추가됨

- `neograph::graph::CancelToken`(원자 플래그 + asio `cancellation_signal`) 및 `CancelledException` — `include/neograph/graph/cancel.h`. 협력적 취소 프리미티브. `RunConfig::cancel_token`를 통해 전달(선택적 `shared_ptr`); 엔진 슈퍼-스텝 루프가 단계 사이에 `is_cancelled()`를 폴링하고 `CancelledException`로 중단합니다. 토큰의 `cancellation_slot()`가 실행의 `co_spawn`에 바인딩되어 진행 중인 LLM HTTP 소켓 작업이 와이어에서 중단됩니다(asio `operation_aborted`).
- `CompletionParams::cancel_token` — 여러 번의 `provider.complete()` 호출에 걸쳐 abort를 스레딩하는 사용자를 위한 명시적 핀. `Provider::complete`는 이를 읽고(또는 `current_cancel_token()`에 의해 설정된 스레드-로컬 `PyGraphNode::execute_full_async`로 폴백) 슬롯을 내부 `run_sync` io_context에 바인딩하므로, 취소에 적중된 동기 Python 노드조차도 청구를 중지합니다.
- `GraphState::run_cancel_token()` — 실행별, 직렬화되지 않는 핸들로, pybind가 `PyGraphNode` 동기식 Python `CurrentCancelTokenScope` 호출 주변에 `execute()` 를 설치하는 데 사용합니다. 이는 동기식 Python 사용자가 노드 코드를 변경하지 않고도 투명한 취소 전파를 얻을 수 있게 해줍니다.
- pybind `engine.run_async` / `run_stream_async`: asyncio `Future.cancel()`는 이제 `add_done_callback`를 통해 `CancelToken::cancel()`로 연결되며, `co_spawn`는 토큰의 취소 슬롯을 바인딩합니다.
- pybind 안전-해석 헬퍼 `_safe_set_future_result` / `_safe_set_future_exception` — 가드 `future.set_result` / `set_exception` 를 통해 게시된 호출을 `call_soon_threadsafe` 취소된-퓨처 `InvalidStateError` 폭풍으로부터 보호합니다.
- `bindings/python/tests/test_async_cancel_live_llm.py` — `Future.cancel()` 이후 3초 이내에 OpenAI HTTP가 완료됨을 주장하는 라이브 OpenAI E2E 테스트(실제로는 즉시; 수정 전에는 취소되지 않은 스트리밍으로 약 7~8초 소요). `NEOGRAPH_LIVE_LLM=1`가 아니면 건너뜁니다.
- `examples/22_self_evolving_graph.py` — 자가 진화 그래프 PoC: `prompted_llm` 노드는 JSON 구성에서 자체 프롬프트를 읽어 LLM 재작성기가 실행 간에 그래프 정의를 변경하고 재컴파일할 수 있게 합니다. `0.0 → 0.4` 점수 개선을 입증하며, 리작성기의 채널 흐름 추론 격차를 문서화합니다.

### 변경됨

- `Provider::complete(params)` 이제 내부 취소 슬롯을 해당 `run_sync` 에 바인딩합니다. `params.cancel_token` 가 설정된 경우 또는 스레드-로컬 `current_cancel_token()` 가 활성화된 경우입니다. 이전 기본 동작(취소 없음)은 옵트인하지 않는 호출자에게 유지됩니다.
- `neograph::async::run_sync`는 선택적 `graph::CancelToken*` 매개변수를 얻었으며, null이 아닌 경우 바인딩된 스폰이 토큰의 슬롯을 바인딩합니다.
- pybind `resolve_future_async` 안전-해석(safe-resolve) 헬퍼를 경유하여 호출하는 대신 `future.set_result` 를 직접 호출하지 않고 `call_soon_threadsafe`.

### 로드맵(v0.3.x로 연기 — `TODO_v0.3.md` 참조)

- 동일한 `thread_id`에서 LangGraph 스타일 자동 체크포인트 재개.
- `run_async` 오류 메시지의 스트리밍 전용 노드 힌트.
- `cb.emit_token(node, data)` 인체공학적 헬퍼.
- README "Differences from LangGraph" 섹션.
- `update_state` 서명이 문서와 정렬됨.
- `get_state` 플랫 헬퍼 / Pydantic 접근자.
- `run_parallel_async` 및 `run_sends_async` 브랜치 fan-out에서 취소 전파의 실시간 검증.
- pgvector RAG 예제.

---

## [미출시] — 4단계

Stage 4는 비동기 경로의 마지막 `run_sync` 홉을 닫습니다. `run_async`는 이제 호출자의 실행기에서 종단 간 유지됩니다: 하나의 `io_context` 스레드에서 50ms 에이전트 3개가 `examples/27_async_concurrent_runs`에서 ~150ms(직렬)에서 ~50ms(중첩)로 감소합니다.

### Breaking

- **`GraphNode::execute_full_async` 기본값이 async-first로 전환되었습니다.** 이제 `co_await execute_async(state)` 를 `NodeResult` 로 감싸서 동기식 `execute_full(state)`를 호출하는 대신 사용합니다. 다음을 발생시키는 모든 하위 클래스는 `Command`/`Send` 를 동기식 `execute_full` 재정의에서만 발생시키는 경우, 한 줄짜리 `execute_full_async` 브리지를 반드시 추가해야 합니다:
  ```cpp
  asio::awaitable<NodeResult>
  execute_full_async(const GraphState& state) override {
      co_return execute_full(state);
  }
  ```
브리지가 없으면 `Command`/`Send`는 비동기 경로에서 조용히 삭제됩니다 — 3.0이 동기 경유로 라우팅하여 수정한 2.0의 잠재적 디스패치 버그로, 슈퍼스텝당 `io_context` 스폰 비용이 발생했습니다. 트리 내 모든 하위 클래스(`deep_research_graph`, 예제 10/14/21, 테스트 5개 사이트)는 이제 브리지를 포함합니다.

### 성능

- 예제 27 벽시계 시간: **152 ms → 53 ms** (하나의 `io_context` 스레드에서 3개 에이전트 × 50ms 타이머 단계, 완전 중첩).
- 단일 실행 벤치마크에서 측정 가능한 성능 저하가 없습니다. `run()` 여전히 동일한 코루틴을 새로운 단일 스레드 `io_context` 를 통해 구동합니다. `run_sync`.

### 테스트

- 341/341 ctest 통과
- 295/295 ASan+UBSan 통과
- Valgrind, 코루틴 중심 하위 집합에서 깨끗함(테스트 20개, 2.4초)

### 출시 후 검증(당일)

- **모든 30개 예제 재실행:** 26/29 PASS, 0 FAIL, 3개 환경 게이트(clay_chatbot → raylib, postgres_react_hitl → docker compose, deep_research 전체 루프 → crawl4ai 서비스). `21_mcp_fanout`는 3 MCP 호출 / 8ms 벽 시간으로 측정됨 — Stage 4 중첩은 실제 네트워크 I/O에서 유지됩니다.

- **ARM64 호환성 (docker buildx --platform linux/arm64):** 저장소 루트의 `Dockerfile.arm64-smoke`. ubuntu:24.04-arm64 + core+llm+async+sqlite+tests 빌드가 QEMU 에뮬레이션에서 ~15분 만에 완료됨; **ARM64에서 306/306 ctest 통과.** 스트립된 바이너리 크기 0.81-0.88 MB (x86_64와 거의 동일). 예제 27은 에뮬레이션에서 65ms로 실행됨 (네이티브 x86_64: 53ms). Linux/ARM64가 macOS 베타(Apple Silicon)와 함께 지원 대상임을 확인.

- **캐시 지역성 (Ryzen 5800X / Zen 3, Valgrind cachegrind, 32 KB L1i/d 8-way, 32 MB L3 16-way):** `bench_concurrent_neograph` 스윕 N=1 → 10,000.

    | N | I refs | LLi 미스 | LLi 미스% | 네이티브 p50 |
  |---:|---:|---:|---:|---:|
    | 1 | 5.3 M | 4,313 | 0.08% | 17 µs |
    | 100 | 11.8 M | 4,320 | 0.04% | 6 µs |
    | 10,000 | 648 M | 4,329 | 0.00% | 5 µs |

최종 레벨 명령어 미스는 N의 4자리 수 차수에 걸쳐 약 4,320건으로 일정하게 유지됩니다. 고유한 핫 코드 작업 집합은 약 277KB(L3의 0.85백분율)입니다. N=10,000에서 6억 4,800만 개의 명령어는 4,329개의 LL 미스만 발생시킵니다 — 약 150,000개 명령어당 1개의 미스입니다. 네이티브 p50은 순전히 I-캐시 예열만으로 17 µs에서 5 µs로 떨어집니다. "버스트 동시성 견고성" 포지셔닝에 대한 최초의 측정 증거입니다.

---

## [3.0.0] — 2026-04-22

3.0은 Taskflow 의존성을 제거하고 단일 asio 코루틴 경로에서 동기 및 비동기 슈퍼스텝 실행을 통합합니다. 그래프 정의 JSON, 노드 ABI, 체크포인트 스키마, 그리고 공개 진입점(`run`, `run_async`, `run_stream`, `resume`)은 2.0과 소스 호환됩니다; 변경은 다음으로 제한됩니다. `GraphNode` **sync**에서 `Command`/`Send` 를 내보내는 서브클래스 `execute_full` 오버라이드에만 해당됩니다.

### Breaking

- **`deps/taskflow/` 및 Taskflow INTERFACE 대상이 제거되었습니다.** 동기 슈퍼스텝 루프, `run_one`, `run_parallel`, `run_sends`, 그리고 프로세스 전역 `tf::Executor` 정적 변수가 삭제되었습니다. NeoGraph의 include 경로를 통해 `#include <taskflow/...>`를 사용하는 다운스트림 소비자는 Taskflow를 별도로 벤더링해야 합니다.
- **`GraphNode::execute_full_async`의 기본 구현은 이제 동기 `execute_full`을 직접 호출하는 브리지입니다(`co_await execute_async`를 사용하지 않음).** 따라서 2.0에서 흔히 사용하던 동기 전용 오버라이드가 내보낸 `Command`/`Send`도 모든 진입점이 공유하는 비동기 경로에서 보존됩니다. 비차단 I/O와 `Command`/`Send`가 모두 필요한 비동기 네이티브 노드는 `execute_full_async`를 직접 오버라이드해야 합니다. 이 요구사항은 2.0부터 docstring에 있었지만, 당시 동기 `run()`이 코루틴 경로를 완전히 우회했기 때문에 실제로 적용되지 않았습니다.
- **`NodeExecutor::run_one` / `run_parallel` / `run_sends` 동기 메서드가 제거되었습니다.** `_async` 피어를 사용하십시오.
- **CPU 병렬 fan-out은 옵트인입니다.** 이전에는 Taskflow가 기본적으로 프로세스 전역 스레드 풀을 제공했습니다. 3.0에서 `run_parallel_async` 및 `run_sends_async`의 다중 전송 분기는 코루틴을 구동하는 실행기를 기준으로 분기를 디스패치합니다 — 동기 `run()`가 생성한 단일 스레드 io_context 또는 `run_async()`에 대한 호출자의 자체 실행기. I/O 바운드 fan-out은 여전히 중첩됩니다(단일 스레드에서 co_await 일시 중단). CPU 바운드 fan-out은 호출자가 `run_async()`에 다중 스레드 실행기를 사용하거나 `engine->set_worker_count(N)`를 통해 엔진 소유 풀을 선택하지 않는 한 직렬화됩니다.

### 추가됨

- `neograph::async::run_sync_pool(awaitable, n_threads)` — 기존 단일 스레드 `run_sync`와 함께 N-워커 동기-비동기 브리지. 호출을 위해 새 `asio::thread_pool`를 생성하여 내부 `make_parallel_group` 분기가 별도 워커에서 실행되도록 합니다.
- `GraphEngine::set_worker_count(n)` — 병렬 fan-out 디스패치를 위해 `NodeExecutor`가 사용하는 선택적 엔진 소유 thread_pool. 실행기를 재구축합니다. 동시 실행 전에 호출해야 합니다.

### 변경됨

- `GraphEngine::execute_graph` (sync)는 사라졌습니다. 모든 진입점(`run`, `run_stream`, `resume`)은 `execute_graph_async` 를 통해 `neograph::async::run_sync`로 라우팅되므로, 슈퍼스텝 루프, 재시도 백오프, 체크포인트 I/O, 그리고 병렬 fan-out이 이제 하나의 코루틴 경로에 종단 간(end-to-end)으로 실행됩니다.
- `benchmarks/concurrent/bench_concurrent_neograph.cpp`가 호출자측 드라이버를 위해 `tf::Executor`/`tf::Taskflow`에서 `asio::thread_pool` + `asio::post`로 전환되었습니다.

### 성능 (참조 Linux에서 bench_neograph Release -O3 -DNDEBUG, 10회 실행 중앙값)

- `seq` 엔진 오버헤드(3-노드 체인, 카운터): **~5.0 µs** 호출당.
- `par` 엔진 오버헤드(5-워커 fan-out + 요약기): **~11.8 µs** 호출당.
- 전체 벤치 프로세스의 최대 RSS(워밍업 + 순차 + 병렬 반복): **4.8 MB**.
- 동일 워크로드에서 LangGraph 1.1.9 대비: 반복당 **순차 131배, 병렬 199배** 더 빠름; RSS 약 12배 더 가벼움.

이전 CHANGELOG 초안은 "~46 µs seq / ~114 µs par"을 3.0 회귀로 나열했습니다. 그 숫자들은 `CMAKE_BUILD_TYPE`가 설정되지 않은 빌드 트리에서 나온 것이므로, 벤치 바이너리는 `-O3 -DNDEBUG` 없이 컴파일되었습니다. 올바른 Release 빌드에서 비동기 피어 축소는 2.0의 Taskflow 동기 경로(같은 호스트에서 2.0 README가 20.65 µs seq / 150.7 µs par로 광고)에 비해 **승리**입니다. 수정된 차트는 [`docs/images/bench-engine-overhead.png`](docs/images/bench-engine-overhead.png)에 있습니다.

### 마이그레이션

- 노드가 `execute()`/`execute_async()`를 오버라이드하고 `Command`/`Send`를 발생시키지 않으면 조치가 필요하지 않습니다.
- sync `execute_full`를 재정의하여 `Command` / `Send`를 내보내는 경우: 변경 불필요 — 3.0 비동기 경로 기본값이 이제 sync 재정의를 직접 호출합니다. `Command.goto_node` 라우팅은 sync 및 async 진입점 모두에서 동일하게 작동합니다.
- 비동기 네이티브 I/O를 위해 `execute_async`를 오버라이드하면서 `Command` / `Send`도 사용하려면 `execute_full_async`를 직접 오버라이드하고 그 안에서 `NodeResult`를 조립하십시오. `execute_async`만 오버라이드하면 `Command` / `Send`가 조용히 사라집니다. 기본 `execute_full_async`가 이제 동기 `execute_full`을 통하고 비동기 `execute_async`를 통하지 않기 때문입니다.
- Taskflow의 프로세스 전역 풀을 CPU 병렬 fan-out에 의존했다면 `engine->run()`: `engine->set_worker_count(N)` 를 compile() 후에 한 번 호출하거나, 엔진을 `run_async()` 를 통해 자체 멀티스레드 `asio::thread_pool` / io_context에서 구동하거나.

---

## [2.0.0] — 2026-04-22

Stage 3 async API가 포함된 첫 공개 릴리스입니다. 이는 브레이킹 릴리스이며, 아래 변경 사항은 컴파일(C++ 표준)과 ABI(추상 기본 클래스에 async 피어(peer) 추가)에 영향을 미칩니다. Sync 호출 지점은 비트 단위로 보존되므로, **`Provider` / `CheckpointStore` / `GraphNode` / `Tool`를 재정의하지 않는 애플리케이션 코드는 변경 없이 계속 작동합니다**.

### Breaking

- **C++20 필수.** 공개 API는 `asio::awaitable<T>` 지원이 필요한 `std::coroutine` 반환 타입을 노출합니다. 소비자는 `-std=c++20`(또는 그 이상)으로 컴파일해야 합니다. GCC 13+, Clang 15+ 테스트 완료; GCC 13 코루틴 해결 방법은 `docs/ASYNC_GUIDE.md` §4.1 참조.
- **libpqxx 의존성 제거.** `neograph::postgres`는 이제 libpq를 직접 링크합니다. Ubuntu 24.04 사용자는 더 이상 libpqxx-7.8t64의 C++17/C++20 ABI 분할로 인해 발생한 `pqxx::argument_error::argument_error(..., std::source_location)` 링크 오류를 겪지 않습니다. CMake find는 이제 `PostgreSQL::PostgreSQL`(CMake 번들 FindPostgreSQL)을 대상으로 합니다. `libpqxx-dev`만 설치한 사용자는 이제 `libpq-dev`도 설치/유지해야 합니다.
- **`Provider`, `CheckpointStore`, `GraphNode`, `MCPClient` ABI 확장.** 각각 비동기 피어(peer) 가상 함수(`complete_async`, `save_async`, `execute_async`, `rpc_call_async` 및 그 변형)가 추가되었습니다. 하위 서브클래스는 2.0 헤더에 맞춰 다시 컴파일합니다. 소스는 서브클래스가 네이티브 async 재정의를 제공하려는 경우가 아니면 변경되지 않습니다(실제 I/O를 수행하는 구현자에게 권장).
- **`CheckpointStore::save` / `load_latest` / `load_by_id` / `list` / `delete_thread` 더 이상 순수 가상이 아닙니다.** 이제 기본 구현을 가지며, 일치하는 `_async` 피어(peer)로 `neograph::async::run_sync`를 통해 브리지합니다. 동기 측을 오버라이드하는 서브클래스는 계속 작동합니다. 오버라이드를 제공하지 않은 서브클래스(이전에는 컴파일 오류였을 것)는 이제 무한 재귀에 빠집니다 — 계약: 각 동기/비동기 쌍 중 하나 이상을 오버라이드하십시오.

### 추가됨

- 모든 I/O 계층의 **Async API**(전체 참조는 `docs/ASYNC_GUIDE.md`):
  - 기본 클래스 및 모든 내장 공급자(OpenAI, Schema, RateLimited)의 `Provider::complete_async`.
  - HTTP 및 stdio 전송 모두에 대한 `MCPClient::rpc_call_async`. stdio는 `asio::posix::stream_descriptor`를 사용합니다.
  - 8개 sync 메서드 모두에 대한 `CheckpointStore::*_async`.
  - `GraphNode::execute_async` + stream / full / full_stream 변형, 비동기 네이티브 교차 기본값 포함.
  - `GraphEngine::run_async` / `run_stream_async` / `resume_async`가 `execute_graph_async`를 구동 — `asio::experimental::make_parallel_group`를 통한 병렬 fan-out을 포함한 종단 간 코루틴 슈퍼 스텝 루프.
  - `neograph::AsyncTool` 어댑터 — 동기 `Tool` 인터페이스를 유지하면서 코루틴 본문을 원하는 사용자 도구용.
- **`neograph::async` 네임스페이스** — HTTP 클라이언트, 연결 풀, SSE 파서, run_sync 브리지, URL 엔드포인트 분할기. `include/neograph/async/*.h` 참조.
- **새로운 예제**:
  - `examples/27_async_concurrent_runs.cpp` — 하나의 `io_context`에서 여러 에이전트.
  - `examples/05_parallel_fanout.cpp` (재작성됨) — `run_parallel_async`를 사용한 단일 그래프 실행 내 비동기 fan-out.
- **CI 벤치 회귀 게이트** (`.github/workflows/ci.yml`) — PR 검사가 `bench_async_http` / `bench_async_fanout` / `bench_neograph`에 대한 하한을 강제.

### 성능

feat/async-api 분기에서 Stage 2 동기 기준선 대비 측정:

- `bench_async_http --mode async_pool --concur 1000`: 6064 ops/s → **17834 ops/s**(2.9배).
- `bench_async_fanout --concur 50000`: 스레드-당-에이전트 달성 불가 → **541K ops/s / 67 MB RSS**.
- `examples/27_async_concurrent_runs` (3 × 50ms 비동기 작업): 150ms (동기) → **50ms** (1 io_context 스레드).
- `examples/05_parallel_fanout` (3 × 100-150ms 비동기 작업): 370ms (순차) → **150ms** (1 io_context 스레드).
- `bench_neograph` 엔진 오버헤드: 변경 없음 (~30 µs 순차 / ~205 µs 병렬). 코루틴 메커니즘이 핫 경로를 회귀시키지 않음.

### 아직 2.0.0에 없음

- **Taskflow 의존성**이 남아 있습니다. 동기 `engine.run()` 경로가 fan-out에 여전히 이를 사용합니다. Sem 4.5에서는 동기 경로를 `run_sync(*_async)`로 대체할 수 있는지 재검토하여 의존성을 완전히 제거할 수 있는지 확인합니다.

### 크로스플랫폼

2.0.0에서는 세 플랫폼이 서로 다른 안정성 계층으로 지원됩니다. 계층은 릴리스 전에 플랫폼이 얼마나 많은 실제 검증을 거쳤는지를 반영합니다 — 기능 범위가 아닙니다(코드베이스는 `#ifdef _WIN32` 분할로 단일 소스입니다; 테스트가 통과하면 기능은 플랫폼 간에 동일합니다).

#### Linux — **GA** ( production-READY)

* Ubuntu 24.04, GCC 13.
* 로컬에서 전체 332/332 ctest 통과(Postgres는 docker `postgres:16-alpine` 사용) 및 커밋된 CI 하한 내의 모든 벤치마크.
* fork/pipe/execvp 기반 MCP stdio + `asio::posix::stream_descriptor`.
* libpq 비차단 방식의 Postgres 비동기 피어 + `asio::posix::stream_
  descriptor`가 `PQsocket`를 래핑(wrapping).
* 위에 인용된 모든 성능 수치에 대한 참조 플랫폼.

#### macOS — beta

* macos-latest(Apple Silicon), Xcode를 통한 Clang.
* CI는 비-Postgres 테스트를 빌드하고 실행합니다. Postgres 통합 테스트는 서비스 컨테이 role 없이 자체 건너뜁니다. POSIX 세계관 (fork/pipe + asio::posix 경로와 동일)은 실행됩니다.
* `CoreFoundation` + `Security` 프레임워크가 httplib를 통해 연결되어 TLS에서 시스템 인증서 로드를 위한 용도로 사용됨.
* 2-4주간의 CI 실행과 사용자 보고를 통해 런타임 동작 차이(코루틴 스케줄링, SIGPIPE/EPIPE 형태, 파이프 버퍼 크기)가 없음이 확인될 때까지 베타로 취급하십시오. 이러한 결과가 문제없이 들어오면 GA로의 표적 승격이 이루어집니다.

#### Windows — **알파**

* windows-latest, MSVC 19.44(VS 2022), x64.
* CI 범위: **core + async + MCP + LLM만 해당**. Postgres 및 SQLite 백엔드는 Windows CI 작업에서 비활성화되어 있습니다. vcpkg가 매 실행마다 OpenSSL / libpq / zlib / lz4를 소스에서 컴파일하기 때문입니다(약 20분 소요, `x-gha`가 제거된 이후로 작동하는 상위 바이너리 캐시 백엔드가 없음). Windows 사용자는 자체 vcpkg / choco 설정을 통해 로컬에서 컴파일합니다.
* 실행기의 사전 설치된 choco 패키지(`C:/Program Files/OpenSSL-Win64/`)를 통한 OpenSSL. httplib + asio::ssl의 TLS 경로가 컴파일 및 링크됩니다.
* MCP stdio: `CreateProcess` + named-pipe(FILE_FLAG_OVERLAPPED) + `asio::windows::stream_handle`. overlapped-pipe 경로는 로컬 Windows 검증 없이 MSDN 사양에 따라 작성되었습니다. 초기 사용자가 엣지 케이스(ERROR_IO_PENDING 처리, 대형 JSON 응답의 파이프 버퍼 경계)를 발견할 것으로 예상합니다.
* Postgres 비동기 피어(로컬에서 활성화된 경우): `asio::ip::tcp::
  socket::assign`가 `PQsocket`가 반환한 SOCKET을 래핑(wrapping)합니다(`native_handle_type`를 통해 캐스팅하여 64비트 SOCKET 값을 보존). Windows CI에서 테스트되지 않음 — 로컬 전용.
* 코루틴 메커니즘은 MSVC의 `<coroutine>`에 있으며, 동작은 사양상 GCC/Clang과 일치할 것으로 예상되지만 `examples/27` 교차 실행 중첩 측정은 아직 Windows에서 확인되지 않았습니다.
* 2.0.0까지 **알파**로 취급합니다. 한 명의 프로덕션 사용자가 stdio/파이프 또는 코루틴 스케줄러 문제 없이 1주일 동안 멀티에이전트 워크로드를 실행하고, 그리고 사용자가 vcpkg의 전체 libpq 빌드를 실행하려는 의지가 있다면 베타로 승격하십시오.

> **패턴**: CI 그린은 최소 기준이지, 상한이 아닙니다. 계층 3 런타임
> 동작 차이(코루틴 스케줄링 타이밍, 파이프 버퍼
> 경계, 소켓 인수인계 의미)는 실제
> 워크로드에서만 표면됩니다.위의 계층 언어는 사용자에게 각 플랫폼에 대한 올바른
> 각 플랫폼에 대한 기대치이며, 세 가지 모두를 가장하는 것이 아닙니다.
> 기대치를 제공하며, 첫날부터 세 가지 모두

### 사후 증가에서 수정됨

- **`async::HttpResponse` 헤더 맵** — 응답 표면은 이제 `headers` 쌍의 새로운 `(name, value)` 벡터를 노출하며, 와이어 순서와 원본 대소문자를 보존하고, 추가로 `get_header(name)` 를 대소문자 구분 없는 접근자로 제공합니다. Retry-After와 Location은 하위 호환성을 위해 전용 필드로 유지됩니다. 아래의 MCP 세션 추적 수정을 가능하게 합니다.
- **MCP `Mcp-Session-Id` 헤더 추적** — Sem 2.6 httplib→async_post 마이그레이션이 이를 조용히 누락했습니다. 이제 초기화 이후의 모든 RPC는 새 headers 접근자를 통해 서버가 할당한 세션 ID를 다시 에코하므로, 서버의 세션 상태는 라우팅 가능한 상태로 유지됩니다.
- **MCP stdio 대기 가능 뮤텍스** — `StdioSession::rpc_call_async`가 `std::mutex`를 사용했는데, 이는 동일한 단일 스레드 io_context에서 두 코루틴이 동일한 세션을 호출할 때 교착 상태를 일으켰습니다(두 번째의 `lock_guard`가 첫 번째가 필요로 하는 워커를 차단했습니다). 이를 `asio::experimental::channel<void(error_code)>` 용량-1 세마포어로 교체하여 두 번째 획득자가 협력적으로 일시 중단되도록 했습니다.
- **`PostgresCheckpointStore` 비동기 피어** — CheckpointStore의 8개 async 메서드(`save_async`, `load_latest_async`, `load_by_id_async`, `list_async`, `delete_thread_async`, `put_writes_async`, `get_writes_async`, `clear_writes_async`)는 이제 진정한 비동기입니다. 내부: `PQsetnonblocking(1)` + `PQsendQueryParams` + `asio::posix::stream_descriptor` on `PQsocket()` + `co_await sock.async_wait(wait_read/wait_write)`. 4개 슬롯 풀에서 4개의 동시 `save_async` 호출이 이제 `run_sync`를 통해 직렬화되는 대신 wire-level에서 병렬로 commit-fsync를 수행합니다.

---

## [0.1.0] — 2026-04 이전

사전 릴리스 개발. 공개 API 안정성 보장 없음.
