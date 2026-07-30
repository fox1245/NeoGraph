<!-- neograph-i18n: source=CHANGELOG.md locale=ko source_sha256=f2880aa80b6972d92480cd2326c3e30415da12ba158d9cec878d05c116f65490 -->
# 변경 기록

**Languages:** [English](CHANGELOG.md) | [한국어](CHANGELOG.ko.md) | [日本語](CHANGELOG.ja.md) | [简体中文](CHANGELOG.zh-CN.md)

NeoGraph의 주목할 만한 모든 변경 사항을 이 파일에 기록한다.

형식은 [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)를, 버전은 [Semantic Versioning](https://semver.org/spec/v2.0.0.html)을 따른다.

---

## [Unreleased]

### 추가

- **선택적 Program 컴포넌트 경계.** 선택형 `NEOGRAPH_BUILD_PROGRAM`
  스위치, 내보내는 `neograph::program` 타깃, 그리고
  `<neograph/program/program.h>` 진입점을 추가했다. 설치 패키지의 컴포넌트
  탐색은 Program을 빌드했을 때만 이를 보고하며, Core 전용 설치는 기존
  `neograph::core` 링크 인터페이스를 유지한다.

- **불변 Program 값 모델.** 안정적인 형식화 진단, 깊은 소유권을 가진
  canonical JSON/C++ 빌더 `ProgramSource` 입력, 불변 콘텐츠 주소형
  `ProgramBundle`/`ProgramVersion` 값, 정규 직렬화, SHA-256 알고리즘 태그
  식별자, 소스 맵, import, 엄격한 버전별 저장 값 스키마를 추가했다.
  `neograph::program`은 이제 Core에만 의존하는 컴파일된 내보내기 라이브러리다.
  Bundle/version v1 프로젝션은 봉인된 Core 정의와 계획 식별자, 시맨틱 버전이
  포함된 실행 항목 다이제스트, 계약, 클로저, 경계, 형식화된 승인/구체화 영수증을
  필수로 요구한다. 식별자는 형식과 저장 버전을 포함하고 의미적 집합은 안정적으로
  정렬된다. 진단은 잘못된 포인터, 역순 span, 알 수 없는 enum을 거부하며 정확한
  파서 오프셋이 없으면 span을 비워 둔다.

- **봉인된 Program 승인 클로저.** 빌더 시점 호출 가능 객체 캡처, 엄격한 정규
  매니페스트, 도메인 분리 지문을 갖춘 불변 `RegistrySnapshot`,
  `AdmissionProfile`, `PolicySnapshot` 값을 추가하고 `ProgramVersion`에서
  지문 간 일관성을 fail-closed 방식으로 검증한다. Core에는 Program 구체화를
  위한 명시적 로컬 전용 parse/link/validate 진입점을 추가했으며, 기존
  로컬 우선/전역 대체 오버로드는 변경하지 않았다.

- **SQLite Harness 레코드 저장소 (이슈 #147 후속).** WAL 지원, 스키마 버전 관리가 된 아티팩트/실행 영속성과 변경 불가능한 아티팩트 및 실행-아티팩트 바인딩을 제공하는 선택적 `neograph::mcp_sqlite` 대상과 `SqliteHarnessRecordStore` 추가. Harness MCP 바이너리가 이제 `runs.db`에 레코드를 저장하며, 체크포인트는 `checkpoints.db`에 남는다.
- **AMD OpenMP GPU 이관 개념 증명.** 같은 숫자 팬아웃 작업에서 직렬 CPU,
  OpenMP 자동 스레딩, 반복마다 데이터를 옮기는 GPU 실행, 데이터를 GPU에
  유지하는 실행을 비교하는 선택적 `bench_openmp_offload` 벤치마크를 추가했다.
  실제 GPU 실행과 호스트 대체 실행, 계산 정확성, 전송 포함 지연 시간,
  커널만의 지연 시간, 직렬 CPU 대비 속도 향상을 각각 보고한다. Radeon AI PRO
  R9700에서는 `NEOGRAPH_OPENMP_OFFLOAD_ARCH=gfx1201`로 ROCm/Clang 장치
  이미지를 활성화한다.


### 변경

- **C++ ABI 및 SOVERSION 정책 (이슈 #194).** 이제 모든 공개
  `neograph_*` 바이너리 라이브러리에 프로젝트 `VERSION`과 주 버전
  `SOVERSION`이 들어가며, 설치된 공유 라이브러리는 자기 디렉터리에서 형제
  의존성을 찾는다. v1 이전 릴리스는 ABI 세대 0을 쓰지만 필수 재빌드 경계를
  공지할 수 있다. bounded `NodeCache`가 들어간 릴리스에서는 `NodeCache`와
  `EngineConfig` 공개 객체 배치가 바뀌므로 `0.11.1` 이하로 빌드한 모든 C++
  프로그램을 다시 빌드해야 한다. 1.0은 ABI 세대를 1로 바꾸고 v1 배치를
  확정한다. CI는 격리된 정적·공유 설치 소비 프로그램을 빌드·실행하고
  ELF/Mach-O 로더 정보도 검사한다. 자세한 내용은
  [`docs/ABI_POLICY.md`](docs/ABI_POLICY.md)를 참고한다.
- **`GraphNode::run(input)` 이전 안내 완성.** Python `GraphNode` 기본 클래스가 더 이상 삭제된 `execute*` 메서드를 참조하지 않으며, `run(input)`이 없으면 이전 문서 경로가 포함된 `NotImplementedError`를 발생시킨다. C++/Python 참조, 비동기/스트리밍 안내, 예제 README가 실제 v0.9.0 단일 진입점에 맞춰졌다. 이전 절차는 C++ 및 Python 예제와 함께 [`docs/migration-v0.4-to-v1.0.md`](docs/migration-v0.4-to-v1.0.md)에 문서화되었다.
- **Provider API 영구 호환성 정책 (이슈 #5).** `Provider::complete()`, `complete_async()`, `complete_stream()`, `complete_stream_async()`, 그리고 콜백 기반 `invoke()`의 제거 계획이 철회되고 `[[deprecated]]` 경고가 제거되었다. 기존 API는 계속 호환성 및 보안 수정을 받는다. 새 Provider 구현과 직접 호출자는 각각 `CompletionProvider::do_invoke()`와 `invoke_request(CompletionRequest)` 사용을 권장하며, 모든 새 기능을 기존 API로 백포트하는 것은 보장되지 않는다. 공개 시그니처, 가상 함수 순서, 객체 크기, vtable은 변경되지 않는다.

### 제거

- **사용 중단된 TransformerCPP 통합 예제 제거.** 더 이상 이용할 수 없는 외부 호스팅 저장소에 의존하던 `example_inproc_gemma`, `NEOGRAPH_BUILD_LOCAL_INFERENCE_EXAMPLE`, `TRANSFORMERCPP_DIR` 제거. 표준 OpenAI 호환 로컬 서버를 사용하는 `example_local_transformer`는 유지.

### 수정

- **Harness 집계 발견 출처 (이슈 #174).** 세부 사항(details)이 이제 기존 평면 `findings` 배열과 정렬된 `finding_sources` 배열을 포함. 각 항목은 스키마 검증된 작업자 출력이나 확립된 `findings` 모양을 변경하지 않고 집계 색인, 소스 작업자 ID, 작업자-로컬 색인을 기록.
- **Harness 내보낸 결과 린트 (이슈 #173).** 노드 효과 계약이 이제 호출자가 그래프 실행 후 소비할 때 선택적 `exports` 배열에 쓰여진 채널을 선언할 수 있음. Harness 컴파일과 `GraphEngine` 런타임 검증 모두 `final_result`에 대해 거짓 경고 없이 진짜 쓰기 전용 채널에 대해 E6을 유지.
- **MCP 2025-11-25 도구-클라이언트 계약 현대화 (이슈 #147 M0).** 초기화가 이제 멱등이며 협상된 서버 메타데이터를 유지; HTTP 도구가 발견 세션을 재사용; `/mcp` 엔드포인트 구성이 요청과 알림에 의해 공유; 도구 발견이 불투명한 커서를 따름; JSON-RPC 코드/데이터, 전체 도구 메타데이터, 비텍스트 콘텐츠, `structuredContent`, `isError`, `_meta`가 C++ 및 Python 경로에서 생존. 구성 가능한 HTTP 타임아웃/정적/동적 헤더, 출력-스키마 검증, 엄격한 응답-ID 검사, 타입 있는 `InitializeResult`, `ToolDefinition`, `ListToolsPage`, `CallToolResult` API 추가. SSE 감지가 이제 `data:` URL을 포함하는 JSON을 잘못 분류하는 대신 `Content-Type` 사용.
- **작업별 취소 상태 및 게시된 방출(emit) 수명 안전성.** `GraphEngine::run`, `run_async`, `run_stream`, `run_stream_async`가 각각 호출자 제공 부모로부터 실행별 자식 하나를 생성하고, 그 자식만 내부 `co_spawn`/동기 브리지에 바인딩하며, 동일한 자식을 `RunContext`로 전달. 따라서 단일 부모 아래 모든 동시 실행을 취소해도 서로의 취소 슬롯을 덮어쓸 수 없다. 포크된 실행 자식은 게시된 방출을 통해 기존 `shared_ptr` 소유권을 유지하여, 엔진 작업 완료와 방출 실행 사이의 해제 후 사용(use-after-free)을 방지. 취소로 인한 asio `operation_aborted`는 재시도 가능한 노드 오류가 아니라 `CancelledException`으로 전파. `CancelToken` 0.11.x 객체 레이아웃과 인라인/헤더 전용 동작은 변경되지 않음. 이미 컴파일된 C++ 소비자가 갱신된 `fork()` 수명 동작을 반영하려면 재컴파일 필요. 공유 라이브러리만 교체하면 객체 레이아웃 호환성은 유지되지만, 소비자 바이너리에 내장된 기존 인라인 함수 본문은 변경되지 않음. 그러나 외부 코드가 직접 생성한 토큰에 `bind_executor()`를 호출할 때는, 실행자의 게시된 작업이 완료될 때까지 호출자가 토큰을 살려둘 책임이 여전히 있음.
- **PostgreSQL 비동기 연결 전역 타임아웃 정책 문서화.** 비동기 초기 연결과 교체가 모든 호스트/IP 주소에 걸쳐 단일 타임아웃을 사용. 연결 문자열에 직접 작성된 양수 `connect_timeout`은 최소 2초로 강제; 지정되지 않았거나 0, 음수, 또는 환경 변수/서비스 파일 전용 값은 운영적으로 안전한 기본값 30초 사용. 이는 libpq의 호스트별 동기 타임아웃과 의도적으로 다르며, 동기 생성/교체 동작은 변경되지 않음.
- **JARVIS 모의 빌드 수정 (이슈 #130).** 오디오 의존성이 없을 때 `MicCapture`가 불완전한 타입으로 남아 발생하던 `cookbook_jarvis` 컴파일 실패 수정. ASan CI가 러너의 설치된 패키지와 무관하게 항상 모의 구성을 빌드하도록 `NEOGRAPH_JARVIS_FORCE_MOCK` 추가. 세션 실행자가 이제 실제 CMake 출력 경로와 전문가 대상 이름을 사용하며, 기존 `demo_mcp_server.py`를 올바르게 실행.
- **노드 실패 컨텍스트 보존 (이슈 #123).** C++ 실행 오류가 원본 `exception_ptr`, 실패한 노드 이름, 시도 횟수를 포함하는 `NodeExecutionError`로 전파되며, 종단 `ERROR` 이벤트도 동일한 컨텍스트를 기록. Python에서는 원본 예외 객체, 타입, args, 사용자 속성, 추적(traceback)이 그대로 보존되고 `.node_name`과 `.attempts` 속성만 추가됨. `NodeInterrupt`, 취소, 메모리 부족 예외는 감싸지지 않고 기존 제어 흐름을 따름.

### 수정 (docs)

- **Provider 쿡북에서 무시된 노드별 프롬프트 제거 (이슈 #116).** 내장 `llm_call`이 읽지 않는 `config.system`을 사용하여 다중 역할 동작을 설명하던 세 개의 Python 예제 수정. 각 예제가 `NodeContext.instructions`를 사용하는 엄격한 단일 호출 그래프로 재작성되었으며, 관련 README가 실제 동작에 맞춰짐.
- **예약된 `RunContext::deadline` 문서 수정 (이슈 #115).** `deadline`과 `trace_id`를 사용 가능한 실행별 메타데이터로 제시하던 문서와 Doxygen 주석 수정 — 이들은 `RunConfig`로 설정할 수 없으며 Python에도 노출되지 않음.
- **`GraphNode::run` 예제 시그니처 수정 (이슈 #129).** `const NodeInput&`(참조)를 받아 실제 값-전달 가상 함수를 재정의하지 못하던 공개 헤더 예제를 수정하고, 코루틴 인자 수명에 필요한 값-전달 계약을 컴파일 시 테스트로 고정.

### 추가

- **하위 호환 Provider 이전 경로.** 새 `CompletionRequest`가 스트리밍 모드를 콜백 존재와 분리하고, `CompletionProvider`는 새 구현이 `do_invoke()`만 작성하도록 요구. 기존 `Provider` vtable, 네 개의 기존 가상 함수, 콜백 기반 `invoke()`, Python `complete()` 하위 클래스 계약은 유지.

- **Python 영속성 백엔드** (#117) — `Store`와 `CheckpointStore`가 이제 C++ 가상 디스패치를 Python으로 하는 구성 가능한 하위 클래스 기반. `StoreItem`, `CheckpointPhase`, `Checkpoint`, `PendingWrite`가 JSON 모양의 필드로 노출되며, 체크포인트 대기 쓰기(pending-write) 메서드는 선택 사항으로 유지.
- **Python 동기 취소** (#119) — Python 호출자가 `CancelToken`을 생성하고 `RunConfig.cancel_token`에 할당하여 다른 스레드에서 `engine.run()`을 협력적으로 중지 가능.

- **Python 체크포인트 이력** (#118) — `GraphEngine.get_state_history()`가 최신 우선 체크포인트 레코드를 노출하여 호출자가 역사적 상태에서 포크하기 전에 부모 링크, 메타데이터, 단계, ID를 검사 가능.

- **DSL 표면 (정교화 계층) + 스키마 진화 게이트** (#75 M4).
  - **Elaborator**: `vars` (`{"$var":...}` / `${...}` 보간, 비순환 강제) / `templates`+`use` (정확한 매개변수 일치 강제, 노드 접두사 이름 변경 — 로컬 참조, 장벽, 경로 포함; 채널은 공유 상태이므로 전역 병합) / `when` 조건부 포함. **비튜링 완전이며 전체(total)**: 모든 DSL 문서가 유한 시간 내에 고유한 코어로 정규화되고 그 코어에 대해 멱등. 모든 오류는 DSL 소스 좌표 (`use[2].args`, `vars.model`)와 소스 맵(출력 위치 → 생성 구문)을 포함. 잠금 파일 워크플로: `./example_elaborate harness.dsl.json > harness.json` (예제 53).
  - **`GraphCompiler::upgrade_to_latest()`**: 무손실 v0→v1 기계적 변환 — 엄격 모드가 거부하는 키는 `x-upgraded-<key>` 주석 네임스페이스로 격리(데이터 삭제 0), 빈 장벽은 명시적으로 제거. 전체 말뭉치가 "기존 허용적 컴파일 IR == 업그레이드 후 엄격 컴파일 IR"을 보장하도록 테스트됨 (정식 동등성, 버전 스탬프 제외).
  - **스키마 진화 게이트**: `tests/fixtures/schema_snapshot.json` 기준에 대한 추가 전용 하위 집합 판정 (JSON Subschema 계열의 결정 가능한 하위 집합) — 노드 타입/속성/리듀서/조건 제거, 필수 집합 증가, 닫힌 조건 레이블 변경, 효과 계약 변경은 모두 테스트 실패 = CI 병합 차단. 호환되지 않는 변경은 동일한 검토 커밋에서 버전 범프 + 업그레이더 + 스냅샷 재생성을 강제.

- **PBT / 델타 검증 도구** (#75 M3). 300-시드 결정론적 토폴로지 생성기(스키마 봉투에서 유효한 엄격 문서, 자체 계측된 기능 커버리지 — conditional_edges/barrier/interrupt 발생이 30% 미만으로 떨어지면 테스트 실패: 테스트되지 않은 기능이 조용한 구멍이 아니라 실패가 됨).
  - **변이 감지**: 300-시드 말뭉치에서 번역 검증이 5가지 모든 누락 유형(conditional_edges/edge/barrier/interrupt/channel) + 3가지 잘못된 배선 유형(경로 축소 / 간선 재대상 지정 / 노드 이름 변경 = 누락+조작 균형추)의 모든 적용을 포착함을 확인. 적용률 하한(시드의 10%)도 주장됨.
  - **참조 인터프리터 델타**: 문서화된 슈퍼스텝 의미(goto 선점, 장벽 누적, 사전식 대체, 암시적 __end__)를 코드-분리된 구현에서 재구현한 독립 모델을 12단계 × 300그래프에서 Scheduler와 비교 (DESIL 교훈: 검증자만으로는 잘못된 실행을 잡을 수 없음).
  - **엔진 ↔ Studio 공유 말뭉치**: `tests/fixtures/topology_corpus/` 15개 변종(3개 유효 + 12개 E3–E11 위반)이 NeoGraph-Studio `tests/corpus/`와 바이트 동일, 둘 다 동일한 평결(code:severity 다중집합)을 주장 — 두 구현이 조용히 갈라질 수 없음.

- **GraphValidator — 토폴로지 정적 의미 검사 (E3–E11 + 효과)** (#75 M2). 파싱(M1)과 실행 사이의 통과 계층. 엄격 문서(schema_version>=1)에서는 오류가 컴파일 실패, 경고는 stderr 린트; 허용적 문서에서는 오류 수준 진단만 stderr 경고로 표면화(기존 그래프에서 잡음 0). 판정 철학 = 검사기 건전성 우선: 엔진 의미 아래 절대 올바를 수 없는 것만 오류(허상 참조 E3, 신호 경로 없는 장벽 E8 — goto가 장벽 회계를 우회하므로 복구 불가, 빈 경로 E10 — 디스패치가 rend() UB 역참조, 선언되지 않은 채널 쓰기 E4 — 런타임 throw 확인됨); Command.goto/Send가 정당화할 수 있는 것은 경고(도달 가능성 E7, 탈출 없는 순환 E11, 장벽 없는 일반 팬인 E9, 덮어쓰기 경쟁 E5, 죽은 채널 E6). 모든 진단은 기계 판독 가능한 증인(반례) JSON을 동반 — Studio 캔버스 하이라이트용(M3).
  - **경로 완전성 (E10)**: `ConditionSpec` 레이블 계약 도입. `register_condition` 3-인자 오버로드를 통해 조건의 출력 레이블 집합을 선언하면 닫힌 조건 경로가 레이블과 정확히 일치해야 함 — 커버되지 않은 레이블은 스케줄러의 "사전식 마지막 경로" 대체(순서 의존적 임의 대상)로 빠지며, 이는 오류. 내장 `has_tool_calls` = 닫힌 {false,true}, `route_channel` = 열린 + 알려진 {default}.
  - **채널 효과 계약**: `register_type` 4-인자 오버로드가 노드 타입별 읽기/쓰기 채널을 선언. E4/E5/E6 분석은 그래프의 **모든** 노드 타입이 선언된 경우에만 활성화(단일 미지 타입이 전체 분석을 건너뜀 — 커버리지보다 건전성). 내장 3개 타입(llm_call/tool_dispatch/intent_classifier) 완전 선언.
  - `node_effects` · `condition_specs`가 `export_schema()`에 추가됨(기존 `conditions` 배열은 하위 호환성 유지). 22개 새 테스트.

- **토폴로지 컴파일 시 일관성 게이트 — 소비된 키 회계 + 번역 검증** (#75 M1). "조용한 의미 손실" 부류(v0.1.0–v0.1.7 `conditional_edges` 조용한 누락과 동일한 종)를 구조적으로 차단하는 이중 메커니즘:
  - **소비된 키 회계**: `"schema_version": 1`을 선언한 문서가 엄격 컴파일로 전환 — 소비되지 않은 키(오타 `conditionnal_edges`, 지원되지 않는 필드, 빈 `wait_for`로 조용히 누락된 장벽, 인라인 조건부에서 무시된 `to`)가 모두 수집되어 컴파일 오류로 보고. 표시는 파싱 블록 **안에서** 발생하므로, 파싱 단계를 지우면 표시도 지워져 해당 기능을 사용하는 엄격 문서가 즉시 실패 — 누락 회귀가 조용할 수 없는 구조. `_`/`x-` 접두사 키(`_comment`, `x-studio-*`)는 항상 주석 네임스페이스로 허용. `schema_version`이 없는 기존 문서는 허용적 동작 유지(바이트 보존).
  - **번역 검증**: `CompiledGraph::to_json()` 재방출 + `GraphCompiler::canon()` 정규형 검사 `canon(input) == canon(re-emit)`을 모든 컴파일에서 수행. 불일치(= 컴파일러가 무언가를 누락했거나 잘못 배선)는 엄격 문서에서 throw, 허용적 문서에서 stderr 경고. 동등성은 구조적 비교 — 경로 키 교체 같은 잘못된 배선도 잡힘(존재-비교가 놓치는 부류).
  - `NodeFactory::config_schema(type)` 쿼리 추가, `schema_version` 필드가 `export_schema()`에 문서화. 27개 새 테스트 (`tests/test_compiler_strict.cpp`) — v0.1.x 누락-변이 시뮬레이션(conditional_edges/barrier/interrupt 누락 + 경로 잘못된 배선) 포함.

## [0.11.1] - 2026-06-25

### 변경

- **stdio MCP 동시 호출 — I/O 중첩을 위한 상관-ID 역다중화기.** `0.11.0` 동시 도구 디스패치는 실제로 HTTP MCP만 중첩. stdio MCP는 `StdioSession::rpc_call_async`에서 **전체 요청→응답 왕복** 동안 용량-1 채널 잠금을 유지하여, 단일 세션 파이프를 통해 한 턴의 여러 호출을 직렬화(벽 시간 ≈ 지연 시간 합계). 단일 파이프가 근본 원인이 아니었음 — JSON-RPC `id`는 정확히 하나의 연결 위에서 파이프라이닝하기 위해 존재. 잠금을 상관-ID 역다중화기로 교체:
  - 용량-1 채널을 **쓰기 전용 잠금**으로 용도 변경 — 프레임 쓰기 순간에만 유지되어, 읽기가 더 이상 직렬화되지 않으면서 두 호출의 바이트가 절대 섞이지 않음.
  - 단일 리더 코루틴(`run_reader`)이 읽기 측을 배타적으로 소유하고 JSON-RPC `id`를 통해 각 응답 줄을 올바른 호출자의 싱크로 전달. N개 동시 호출이 읽기를 중첩하여 벽 시간 ≈ max(지연 시간) — 하지만 **피어 MCP 서버가 동시에 처리할 때만** (단일 스레드 순차 서버는 Amdahl 하한을 만남).
  - 리더는 진행 중인 호출이 있을 때만 지연 실행되고 대기자가 비면 종료되어, 비공개 `run_sync` io_context가 정상 반환. 대기자는 호출자가 기다리는 동안만 존재하며 `MCPTool`의 `shared_ptr`을 통해 세션을 살려두므로, 리더가 파괴된 세션을 절대 건드리지 않음(소멸자 join 불필요). 파이프 EOF/오류 시 리더가 모든 싱크를 닫아, 대기 중인 호출자가 무한정 멈추지 않고 예외를 받음.
  - **API/구문 변경 없음** — 공개 헤더 변경 없음, 기존 코드 재컴파일 불필요. 엔진 오버헤드 회귀 0 (`bench_neograph` 교차 A/B, seq/par Δ 0%).
  - 테스트: 스레드 기반 지연 픽스처 `tests/fixtures/mcp_stdio_slow.py` + `ConcurrentStdioCallsOverlapIO` (5×100 ms 호출이 ~130 ms에 완료 vs. 500 ms 직렬 하한; 각 응답이 `id`를 통해 호출자에게 라우팅되는지 검증). ASan+UBSan ×3 깨끗.

## [0.11.0] - 2026-06-25

### 추가

- **동시 도구 디스패치 — `Tool::execute_async` 공식 비동기 경로.** `ToolDispatchNode`가 엔진의 `make_parallel_group`을 사용하여 단일 어시스턴트 턴의 여러 `tool_call`을 **동시에** 실행. 이전에는 각 호출이 동기 `execute()`를 통해 순차 실행되었고, 특히 MCP 도구가 호출당 `run_sync`를 통해 자체 `io_context`를 생성하는 것을 차단하여 병렬 MCP 호출이 중첩되지 못함(병렬 MCP 호출이 있는 외부 C++ 포크에서 발견). 수정:
  - 가상 `execute_async()`가 `Tool`에 추가 — 기본 구현이 동기 `execute()`로 연결되어 기존 도구가 변경 없이 작동.
  - `MCPTool`이 네이티브 `execute_async`를 갖춘 `AsyncTool`로 변환 (stdio는 `rpc_call_async` 사용, HTTP는 비동기 핸드셰이크를 위해 새 `MCPClient::initialize_async`/`call_tool_async` 사용 — `run_sync` 제거됨).
  - `ToolDispatchNode::run`이 노드 팬아웃과 동일한 `make_parallel_group` 관용구로 호출을 동시에 디스패치(단일 호출은 인라인), 결과는 호출 순서대로 적용. 동기 `execute()` 파사드를 통해 하위 호환.
  - 검증: 478/478 ctest, Valgrind 누수 0, TSAN 경쟁 0.

### 수정

- **Python 비동기 실행 예외 보존 (이슈 #122).** `run_async`, `run_stream_async`, `resume_async`가 원본 Python 노드 예외를 문자열로 감싼 새 `RuntimeError`로 덮어쓰던 문제 수정. 이제 원본 Python 예외 객체, 타입, 사용자 속성, 추적이 pybind11의 표준 예외 변환 경로를 통해 보존되며, C++ `py::type_error`는 동기 실행과 일치하는 Python `TypeError`로 전달. `resume_async`의 빈 콜백이 이제 코루틴이 완료될 때까지 유지되어, pybind11 3.x에서 드러난 허상 참조 충돌도 수정.

### 수정 (docs)

- **README 요약 배지가 빠진 조건과 샌드박스 측정으로 드러난 내부 모순 수정.** "네 축" 요약 표 배지가 본문/심층 분석의 측정 조건을 벗겨내 과장되어 읽힘. 본문 측정 수치와 조건에 맞게 수정(측정 데이터 표 자체는 변경 없음):
  - **`p99 17 µs flat` → `p99 7 µs @ 10 K (1 CPU sandbox)`** — 배지의 17 µs가 본문(`At N=10,000 concurrent ... 7 µs p99`)과 모순되었고 `flat`은 µs 측정이 아닌 GPU-바운드 부하 테스트 실행 지연(648 ms)을 설명. 배지를 본문 측정 수치와 조건에 맞춤.
  - **`1.2 MB stripped binary` → `... (MinSizeRel static)`** — `libc.so.6`-only와 1.2 MB는 MinSizeRel + 정적 libstdc++ 빌드에만 해당(기본 Release는 libstdc++/libgcc_s/libm/libc 동적 링크). 심층 분석 §크기에 이미 문서화된 조건을 배지에 복원.
  - **`2 wheel deps` → `2 direct wheel deps (... ; 7 with transitive)`** — 직접 의존성은 실제로 `certifi` + `pydantic` (2개)이지만, 실제 설치 트리는 pydantic 전이 의존성(pydantic-core, typing-extensions, annotated-types, typing-inspection)을 포함한 7개 패키지.
- **심층 분석 MinSizeRel 재현 명령에 `-DNEOGRAPH_BUILD_POSTGRES=OFF` 추가.** PostgreSQL은 기본값 ON이므로, libpq가 없는 호스트에서 그대로 실행하면 구성 실패. 수정.

## [0.10.0] — 2026-05-20

### 추가

- **직렬 팬아웃 일회성 stderr 경고 (이슈 #62, PR #63).** `compile()`의 기본값은 `set_worker_count(1)` — 팬아웃 분기가 엔진 소유 스레드 풀 없이 호출자의 실행자에서 직렬 실행. 이 의도된 동작이 문서만으로 다중 Send 그래프를 구축한 사용자에게는 조용한 직렬 실행으로 보임. `NodeExecutor`가 풀 없이 다중 Send(또는 다중 나가는 간선) 팬아웃을 디스패치하는 첫 순간에 stderr로 일회성 안내 메시지 추가. `std::atomic` + compare-exchange가 동시 팬아웃 아래에서도 정확히 한 번만 방출 보장. `set_worker_count(N>=2)` 호출이 `NodeExecutor`를 재구축하여 자연스럽게 플래그 재설정. 환경 변수 `NEOGRAPH_SUPPRESS_FANOUT_WARNING=1` (또는 `true` / `yes`)로 억제 가능 — 의도적인 worker=1 직렬 실행, 벤치마크, CI stderr 단언 케이스용. Linux + macOS 단위 테스트 5개로 커버 (`test_fanout_worker_warning.py`): 발화 / 일회성 / 풀 선택 침묵 / env-var 침묵 / 단일 Send 경고 없음. Windows: pytest capfd가 wheel 바이너리의 MSVC CRT fd 캐싱과 호환되지 않아 모듈 수준 건너뜀 — wheel 바이너리 stderr 출력 자체는 정상.

- **토폴로지 JSON 스키마 내보내기 — `NodeFactory::export_schema()`** (이슈 #56, 코드 없는 비주얼 블록 편집기의 전제 조건). 엔진이 소비하는 토폴로지 JSON 형식을 기계 판독 가능한 스키마(JSON Schema Draft 2020-12)로 한 조각으로 내보냄: `{ neograph_version, $schema, topology (고정 봉투), node_types, reducers, conditions }`. 별도 저장소의 블록 편집기가 이 스키마에서 팔레트를 자동 생성 → 편집기와 엔진이 버전 간에 표류할 수 없음. 완전히 추가적:
    - `NodeFactory::register_type(type, fn, json config_schema)` 3-인자 변형 추가. 기존 2-인자는 허용적 기본 스키마로 위임 — 기존 사용자 노드/호출 영향 없음.
    - `ReducerRegistry::names()` / `ConditionRegistry::names()` / `NodeFactory::registered_types()` 쿼리 접근자 추가.
    - 4개 내장 타입(`llm_call`/`tool_dispatch`/`intent_classifier`/`subgraph`)에 대한 구성 스키마 선언. `NEOGRAPH_VERSION`이 컴파일 정의로 노출(pyproject.toml 단일 진실 원천) → 스키마 버전 스탬프.
    - `examples/52_export_schema.cpp` (`example_export_schema`): `./example_export_schema > schema.json` — 편집기 저장소 CI가 NeoGraph 버전에 고정된 아티팩트를 생성하는 표준 경로.
    - Python: `neograph_engine.export_schema()` → dict (편집기 저장소 CI가 `pip install neograph-engine` 후 덤프).
    - `tests/test_schema_export.cpp` 8 + `test_export_schema.py` 4. 핵심: 최상위 `conditional_edges`가 로더→컴파일 왕복에서 생존 (v0.1.0–v0.1.7 조용한 누락 재발에 대한 회귀 방지).

### 수정

- **토폴로지 최상위 컨테이너 형식 검증 (#126).** `channels`/`nodes`는 객체여야 하며, 아니면 모든 모드에서 거부. `edges`/`conditional_edges` 배열 검증은 엄격 모드에서 강제, 기존 키-간선-맵 호환성 유지. 오류는 전체 입력이 아니라 경로와 JSON 타입을 기록.
- **`max_steps` 종료 상태 노출 (#114).** `RunResult::max_steps_exhausted()`와 읽기 전용 Python 속성 `RunResult.max_steps_exhausted` 추가. 실행할 노드가 남아 있는데 `max_steps`에 도달한 경우에만 참; 동일한 상태가 gRPC 단일 응답과 스트리밍 최종 JSON에도 제공. C++ 구조체 크기 변경 없음.

- **`set_worker_count` / `set_worker_count_auto` docstring 수정 (이슈 #62, PR #63).** v1.0 준비 주기가 의도적으로 `compile()` 작업자 풀 기본값을 `set_worker_count(hardware_concurrency())`에서 `set_worker_count(1)`로 되돌렸지만(근거는 `src/core/graph_engine.cpp:69-93` 주석 참조), 네 개의 사용자 대면 docstring이 옛 주장을 유지 → 문서를 신뢰하고 다중 Send 팬아웃 그래프를 구축한 사용자가 단일 스레드에서 조용한 직렬 실행을 얻음. 단위 테스트로는 보이지 않음(가짜 생성, 즉시 본문); 실제 wall-time e2e에서만 노출.
  - `bindings/python/src/bind_graph.cpp`의 `set_worker_count` / `set_worker_count_auto` Python docstring을 실제 동작과 일치하도록 재작성: `compile()` 기본값은 1, `set_worker_count_auto()` / `set_worker_count(N>=2)`는 명시적 선택.
  - `include/neograph/graph/engine.h`의 두 Doxygen 주석도 그에 맞게 수정. Doxygen Pages는 master 푸시 시 자동 재구축.
  - `docs/concepts.md` / `docs/troubleshooting.md` / `docs/reference-en.md`의 동일한 오래된 주장(기본값 = hardware_concurrency)도 수정.

- **v0.9.0 배포에서 누락된 세 가지 API 이전 보충.** v1.0 준비 주기의 PR `9b` (`19819d8`)가 `GraphNode` 기존 8-가상 체인을 파괴적으로 제거했지만, PR `#48` (`6e654ad`, "C++ examples migrate to `GraphNode::run()`")이 `examples/`만 이전 — 다음 3개 파일이 누락되어 v0.9.0이 빌드 불가 상태로 배포:
    - `benchmarks/stress/bench_sustained_concurrent.cpp` (Phase 3 지속-버스트 검증 핵심 벤치마크)
    - `benchmarks/concurrent/bench_concurrent_neograph.cpp` (LangGraph 및 다른 엔진과의 메모리/동시성 비교 행렬 본문)
    - `wasm/smoke.cpp` (Phase 1 WASM 실현 가능성 스모크)

  CI가 이 대상들을 add_executables로 잡지 않았거나 (Docker 빌드 의존성) 별도 환경에 격리했기 때문에 master 병합과 태그가 통과.

  **수정**: 세 파일 모두 `std::vector<ChannelWrite> execute(const GraphState&) override` → `asio::awaitable<NodeOutput> run(NodeInput in) override` + `co_return out` 패턴으로 이전. 노드 로직 변경 없음.

  **v1.0 핵심 판매 포인트 네이티브 재검증** (`benchmarks/concurrent/results_v0.9.0_native_recheck.jsonl`):
    - 동시성 10K · wall 10–23 ms · p99 17–21 µs · 최대 RSS **5.6 MB** (v0.3.0 / v0.5.0 측정과 일치 — 파괴적 9b 이후 메모리 판매 포인트 회귀 없음)
    - 10K에서 오류 0
  **Docker 행렬 (LangGraph / Haystack / pydantic-graph / LlamaIndex / AutoGen 6-way 비교)도 같은 세션에서 재측정** (`results_v0.9.0_docker_recheck.jsonl`).

  행렬 재실행 중 누락된 API 이전과 함께 독립적 회귀 하나 발견 — `benchmarks/concurrent/Dockerfile.neograph`가 master의 CMake 옵션 기본값 변경을 추적하지 못해 전혀 빌드할 수 없었음(v0.9.0 배포 시점과 동일). 시간이 지나면서 다음 옵션 기본값이 OFF → ON으로 뒤집힘:
    - `NEOGRAPH_BUILD_POSTGRES` / `NEOGRAPH_BUILD_SQLITE` (각각 `libpq-dev` / `libsqlite3-dev` 필요)
    - `NEOGRAPH_BUILD_A2A` / `NEOGRAPH_BUILD_ACP`
    - `NEOGRAPH_USE_LIBCURL` (하나의 이전 사고가 `feedback_libcurl_unconditional_dep.md`에서 닫힘 — 옵션 토글만 추가되고 기본값은 ON으로 남아 빈 컨테이너 빌드 경로를 다시 깸)
    - `find_package(OpenSSL REQUIRED)`는 옵션 토글 없이 무조건적 (CMakeLists.txt:256) — 별도 v1.0 정리 후보

  **Dockerfile 수정**: `libssl-dev` apt 추가 + 모든 비핵심 옵션을 명시적 `-DNEOGRAPH_BUILD_*=OFF` / `-DNEOGRAPH_USE_LIBCURL=OFF`로 고정. 주석에 "두 번의 표류 사고로 인한 명시적 동결" 기록. `find_package(OpenSSL REQUIRED)`의 CMakeLists.txt 조건부화는 별도 작업으로 남김 — 다른 빌드 경로(PyPI wheel, ARM64 등)에 대한 영향 검증 필요.

  **6-way 행렬 핵심 결과** (동시성=10000, 2 cpus / 1 GiB):

  | engine          | mode          | wall_ms | p99_us      | peak_MB | ok/err |
  |---|---|---|---|---|---|
  | **neograph**    | threadpool    | **16**  | **18**      | **5.1** | 10000/0 |
  | pydantic-graph  | asyncio       | 895     | 160         | 42.8    | 10000/0 |
  | haystack        | mp-pool-8     | 1472    | 2972        | 68.3    | 10000/0 |
  | langgraph       | mp-pool-8     | 3802    | 74415       | 60.6    | 10000/0 |
  | autogen         | mp-pool-8     | 22428   | 82361       | 49.1    | 10000/0 |
  | llamaindex      | asyncio       | 26303   | 25912204    | 582.7   | 10000/0 |

  NG vs LangGraph (마케팅 비교 축): wall **237× 더 빠름**, p99 **4134× 더 빠름**, 최대 RSS **12× 더 낮음**.

  **가혹한 시나리오** (동시성=10000, 1 cpu / 512 MiB):
    - NG: 8 ms / 5.2 MB / 0 err / **ok**
    - LangGraph mp-pool-8: 7821 ms / 60.9 MB / 0 err / ok
    - **LlamaIndex asyncio: OOM 종료** (512 MB 제한 초과)
    - **AutoGen asyncio: OOM 종료**

  v0.3.0 / v0.5.0과 동일한 측정 — **파괴적 9b 이후 NeoGraph의 "10K 동시 작업자, 최대 RSS 5 MB, OOM 없음" 판매 포인트 회귀 없음.**

## [0.9.0] — 2026-05-14 — v1.0 준비 (Candidate 1 Phase B + Candidate 6)

ROADMAP_v1.md의 두 v1.0 단일-디스패치 통합이 한 주기에 수렴:

  - **Candidate 1 Phase B (`9b`–`9f`)** — `GraphNode`의 모든 기존 8 가상 함수 (`execute` / `execute_async` / `execute_stream` / `execute_stream_async` / `execute_full` / `execute_full_async` / `execute_full_stream` / `execute_full_stream_async`) + `add_cancel_hook` + `CurrentCancelTokenScope` + `state.run_cancel_token_` + 모든 6개 `PyGraphNodeOwner` 기존 재정의 제거. **파괴적** — 사용 중단 창 닫힘. 사용자 GraphNode 하위 클래스 / 사용자 Python 노드는 단일 메서드 `run(NodeInput)` / `def run(self, input)`으로 이전해야 함.
  - **Candidate 6** — `Provider` 4-가상 데카르트 곱 → 1-가상 `invoke()`. 여전히 추가 + 사용 중단 단계 — 기존 4 가상 함수는 변경되지 않고 작동, 사용 중단 경고만 보임. 그 쪽의 Phase B (`Provider` 기존 제거)도 v1.0.0 배포 직전에 닫힘.

동일한 주기에 b59444f의 잠재적 병렬-회귀 되돌리기 (`e5ecb08`) + 명시적 팬아웃 예제 호출 + 3개 CI 환경 수정 (httplib 매크로 보호 / Windows MSVC unistd.h / pybind pytest 이전)도 모두 이 [Unreleased]의 일부.

### 추가

- **`Provider::invoke(params, on_chunk = nullptr)`** — v1.0 표준 단일 디스패치 진입점. 하나의 메서드에서 비스트리밍 (`on_chunk == nullptr`)과 스트리밍 (`on_chunk` 제공) 모두 처리. 이전 4-가상 데카르트 곱 (`complete` / `complete_async` / `complete_stream` / `complete_stream_async`)을 하나의 비동기-스트리밍 상위 집합으로 통합. 기본 구현이 4개 기존 가상 함수로 전달되어 기존 Provider 하위 클래스가 변경 없이 작동. 6개 새 ctest (`ProviderInvokeDefault`). (PR #40)
- **`invoke()` 취소 전파 동등성** — `params.cancel_token`이 설정되지 않았고 엔진 스레드-로컬 스코프가 활성 상태일 때, `current_cancel_token()`이 자동으로 찍힘. 기존 동기 `complete()` 동작과 동등 (엔진 안의 노드 본문이 `provider->invoke(params, ...)`를 호출하면 실행 중인 그래프의 취소 신호를 자동으로 받음). 3개 새 ctest (`InvokeCancelPropagation`). (PR #43)
### 변경

- **엔진의 모든 내부 LLM 호출이 `invoke()`를 통해 라우팅** — `LLMCallNode`, `IntentClassifierNode` (PR #41/#42), `Agent::complete` / `Agent::run_stream` (PR #43), `SupervisorLLMNode` / `ResearcherLLMNode` / `CompressNotesNode` / `FinalReportNode` (PR #43), `PlannerNode` / `ExecutorNode` (PR #44). NeoGraph 내 LLM 디스패치가 단일 표면으로 통합.
- **C++ 예제 이전 (2개 파일)** — `31_local_transformer.cpp`, `cookbook/ai-assembly/member_server.cpp`가 이제 새 `invoke()` 사용. 사용자 빌드에서 사용 중단 경고 없음. (PR #45)
- **`GraphEngine::compile()` 기본 작업자 수를 1로 되돌림** (`e5ecb08`). `b59444f`가 18일간 잠복한 (2026-04-26 → 2026-05-13) 병렬 마이크로 벤치 회귀 11.8 → 283 µs (24×)의 근본 원인 — 이분 탐색으로 커밋 식별 (11개 worktree 병렬). v1.0부터 기본값=1 (CPU가 작은 순차/병렬 디스패치에 최적); 의도적인 팬아웃에는 한 줄 `engine->set_worker_count_auto()`를 추가하여 hardware_concurrency 열기. 영향을 받는 5개 팬아웃 예제(10/14/21/36 + deep_research_graph 빌더)에 명시적 호출 추가. 상세는 ROADMAP_v1.md의 "성능 회고" 섹션 참조.

### 사용 중단

- **`Provider::complete` / `complete_async` / `complete_stream` / `complete_stream_async`** — 4개 기존 가상 함수 모두 `[[deprecated("v1.0 single-dispatch: use invoke(...)")]]` 표시. 기존 메서드는 사용 중단 창 동안 그대로 작동. v1.0.0에서 제거. 내부 전달자는 `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED`로 감싸져 경고가 사용자 대면 재정의 / 호출 지점에서만 나타남. (PR #44)

### 제거 (Candidate 1 Phase B — 파괴적)

- **`GraphNode` 기존 8 가상 함수** — `execute(GraphState&)` / `execute_full(...)` / 6개 변형 + `ExecuteDefaultGuard` 재귀 방지 + 300+ 줄의 기본 체인. 모두 제거. `run(NodeInput)`이 유일한 순수 가상. (커밋 `19819d8`)
- **`add_cancel_hook` + `Hook` RAII + `hooks_*` 멤버 + `cancel()` 훅 반복** — `cancel.h`가 `fork()` + `cancel()` + `is_cancelled()` + `slot()`만 유지. (커밋 `1d786a5`)
- **`CurrentCancelTokenScope` + `current_cancel_token()` thread_local + `GraphState::run_cancel_token_` + 3개 접근자** — `RunContext::cancel_token`이 유일한 취소 채널. `src/core/cancel.cpp`가 빈 스텁으로 축소 (파일 자체는 미래 삭제 후보). (커밋 `9e8e956`)
- **6개 `PyGraphNodeOwner` 기존 재정의** — pybind 트램펄린이 `run(self, input)`만 호출. Python 사용자 코드도 v0.9.0부터 단일 메서드 필요. (커밋 `9e8e956`)
- **2개 폐기된 pytest 파일** — `test_execute_stream_dispatch.py` (v0.3.2 스트림 전용 대체 디스패치 검증) + `test_streaming_only_error_hint.py` (execute_full_stream 우선 — v1.0에서는 무의미). (커밋 `4392fbb`)

### 수정

- **5개 팬아웃 예제에 명시적 호출 추가** — `e5ecb08`의 기본 작업자 수 되돌리기에 묻힌 실제 병렬 의도 복원: `examples/10_send_command.cpp`, `examples/14_plan_executor.cpp`, `examples/21_mcp_fanout.cpp`, `examples/36_classifier_fanout.cpp`, `src/core/deep_research_graph.cpp`의 `create_deep_research_graph()` 빌더가 이제 `set_worker_count_auto()` 호출. 검증: `classifier_fanout` 4.22× 속도 향상 (25.2 ms 순차 → 6.0 ms 병렬). (커밋 `99c470b`)
- **`bench_async_http` httplib 매크로 보호** — `bench_async_http.cpp`가 `<neograph/async/conn_pool.h>`를 통해 `<httplib.h>`를 포함하지만 `CPPHTTPLIB_OPENSSL_SUPPORT`가 정의되지 않아 ODR 보호 장치가 거부. CMake 대상에 `target_compile_definitions(... PRIVATE ...)` 추가. (커밋 `d4be42a`)
- **Windows MSVC `unistd.h` 누락** — `test_schema_provider_extra_fields_temperature.cpp`가 POSIX 전용 `mkstemps` + `close`를 사용하여 Windows 빌드 전체 실패. `#ifndef _WIN32` 보호로 파일 전체 감쌈 (Linux/macOS로 커버리지 보장). (커밋 `3c49f12`)
- **16개 Python 테스트 이전** — wheel CI pytest가 기존 `def execute(self, state)` 패턴을 가진 28개 노드 클래스에서 `AttributeError` 발생. `def run(self, input)`으로 일괄 이전; 스트리밍 노드는 `input.stream_cb` None-보호 추가. (커밋 `4392fbb`)

### 이전 (사용자 코드)

**Provider 호출 (Candidate 6 — 사용 중단 단계)**

새 코드:
```cpp
// non-streaming
auto completion = co_await provider->invoke(params, nullptr);

// streaming
auto completion = co_await provider->invoke(params, on_chunk);

// sync site (replaces old complete())
auto completion = neograph::async::run_sync(provider->invoke(params, nullptr));
```

4개 기존 가상 재정의는 사용 중단 창 동안 계속 작동하지만, `-Wdeprecated-declarations` 경고가 사용자 재정의 지점에서 보임. v1.0.0 직전에 제거; 사용 중단 창 내 이전 권장.

**`GraphNode` 하위 클래스 (Candidate 1 Phase B — 파괴적)**

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

**팬아웃 의도 (작업자 수 기본값 변경)**

```cpp
// old (v0.x April+): default was hardware_concurrency but micro-bench burden
// v1.0: default=1. Add one line for intentional fan-out.
auto engine = GraphEngine::compile(def, ctx);
engine->set_worker_count_auto();  // ← this line added (hardware_concurrency)
// or engine->set_worker_count(N);  // explicit N
```

`docs/migration-v0.4-to-v1.0.md`의 이전 1/2/3 섹션(run() / ctx.cancel_token / worker count default) + Provider 섹션(다음 문서 정리에서 추가 예정)이 사례별 이전/이후 안내를 제공.

## [0.8.0] — 2026-05-13 — DX 정책 + 다운스트림 주도 API 간극 해결

실제 다운스트림(ProjectDatePop) 피드백과 내부 커버리지 차이로 표면화된 8개 이슈(#22, #25, #26, #27, #28, #34, #35 + #16 후속)를 단일 마이너 범프로 묶음. 두 개의 새 공개 도우미(`RunResult::channel<T>`, `RunContext::store`), 11개 새 오프라인 예제, `docs/migration-v0.4-to-v1.0.md` 이전 안내, 신규 사용자가 처음 30분에 부딪히는 마찰을 줄이는 5개 항목 DX 묶음.

### 추가

- **`RunResult::channel<T>(name)` / `channel_raw(name)` / `has_channel(name)`** — 결과에서 채널 값을 추출하는 한 줄 도우미. 두 출력 모양(중첩 `output["channels"][name]["value"]` 표준 + `react_graph` 같은 빌더가 추가한 평탄 키) 자동 처리. 9개 새 ctest. (이슈 #25)
- **`RunContext::store`** — 노드 본문이 한 줄로 Store에 도달 `in.ctx.store->get(ns, key)`. 옛 패턴(`shared_ptr<Store>`를 `NodeFactory` 람다에 캡처)은 여전히 작동 — 새 코드는 새 모양만 필요. 3개 새 ctest. (이슈 #27)
- **`Provider::complete_stream` 비순수 기본 본문** — 최소 모의 / 테스트 픽스처가 `complete()`만 재정의하면 됨. 기존 스트리밍 네이티브 재정의는 변경 없음. 2개 새 ctest. (이슈 #22)
- **`neograph::json` 배열 `.front()` / `.back()`** — nlohmann 근육 기억 패턴 (`msgs.back()["content"]`)이 이제 컴파일. 4개 새 ctest. (이슈 #26)
- **11개 새 오프라인 예제 (41-51)** — `resume_if_exists_chat`, `custom_reducer_condition`, `store_personalization`, `request_queue_backpressure`, `cancel_token`, `node_cache`, `sqlite_checkpoint`, `openinference`, `async_tool`, `minimal`. 모두 rc=0, API 키 / 외부 서비스 의존성 없음. 이전에 참조가 0이었던 27/53 `NEOGRAPH_API` 클래스 중 간극 채움.
- **`examples/51_minimal.cpp`** — 노드 하나, LLM 없음, 도구 없음, 모의 제공자 없는 30줄 입문 예제. 5분 안에 NeoGraph 작동 방식 이해.
- **`docs/migration-v0.4-to-v1.0.md`** — `[[deprecated]]` 옛 8-가상 체인(`execute` / `execute_async` / 등) → 새 `run(NodeInput) -> awaitable<NodeOutput>`으로 이전하는 사례별 이전/이후 4개 예제 + 흔한 실수. `NEOGRAPH_DEPRECATED_VIRTUAL` 매크로 메시지에서도 링크.
- **README "흔한 함정 5" 섹션** — 신규 사용자가 처음 30분에 부딪히는 다섯 가지(`channel<T>` 사용법, `in.ctx.store`, `neograph::graph::` 하위 네임스페이스, `<httplib.h>` 매크로, GCC 13 코루틴 ICE)를 한 곳에. 각 항목에 수정 + 관련 예제/이슈 링크.
- **컴파일 시 `#error` 보호 장치 (`include/neograph/api.h`)** — 사용자 TU가 `CPPHTTPLIB_OPENSSL_SUPPORT` 없이 NeoGraph 헤더보다 `<httplib.h>`를 먼저 포함하면, 명확한 메시지 + 선택 해제 매크로(`NEOGRAPH_SKIP_HTTPLIB_MACRO_GUARD`)로 컴파일 실패. 옛 #16 런타임 SEGV를 컴파일 시 실패로 승격.
- **`example_minimal` 5개 새 친절한 오류 메시지 ctest** — `Unknown reducer` / `Unknown condition` / `Unknown node type` / `Write to unknown channel` 메시지가 메시지 본문에 사용 가능한 이름 + 등록 방법 + 문제 해결 링크를 포함하는 계약 잠금.
- **`docs/troubleshooting.md` 4개 새 항목** — Tracer 어댑터 `close()` 멈춤/충돌(#24), GCC 13 코루틴 ICE(#23), 친절한 오류 메시지 안내(#22), `RunResult::output` 모양(#25).
- **`Tracer` + `OpenInferenceTracerSession::close()` `@warning` 블록** — 어댑터 작성자를 위한 원시 포인터 함정을 명시적으로 문서화. 올바른 접근 방식으로 `RecordedSpan` + 래퍼 분리 패턴을 가리킴. 기존 `tests/test_openinference_cpp.cpp::InMemoryTracer` + 새 `examples/49_openinference.cpp::PrintTracer` 참조. (이슈 #24)

### 수정

- **`SchemaProvider::build_body`가 `params.tools`가 비어 있을 때 `extra_fields`를 조용히 누락.** 옛 코드가 `if (!params.tools.empty())` 안에 `extra_fields` 적용을 가두어, `reasoning`과 `response_format` 같은 핵심 스키마 필드가 도구 없는 호출에서 완전히 사라짐. 수정: 도구 분기 밖으로 이동하여 항상 적용. 3개 새 ctest. (이슈 #34)
- **`temperature_path` 스키마 측 선택 해제.** 추론 모델(gpt-5.x, o-series)은 `temperature`와 `reasoning.effort`가 상호 배타적이지만, 스키마에 "이 제공자는 temperature를 받지 않는다"고 선언할 방법이 없어 모든 호출에서 `params.temperature = -1.0f` 센티널 해결책을 강제. 수정: 스키마에 `"temperature_path": null`을 지정하면 build_body가 완전히 건너뜀. 4개 새 ctest. (이슈 #35)
- **친절한 RuntimeError 메시지** — `ReducerRegistry::get` / `ConditionRegistry::get` / `NodeFactory::create` "Unknown <thing>: foo" 및 `GraphState::write` / `apply_writes` `Write to unknown channel`이 이제 메시지 본문에 사용 가능한 이름 + 등록 방법 + 문제 해결 링크 포함. 신규 사용자가 메시지만으로 다음 단계 결정 가능.
- **`SchemaProvider::complete_stream_async` HTTP/SSE 분기**가 이제 수명이 긴 전용 `bridge_thread_`에서 디스패치 (옛: `Provider` 기본 기본값이 호출당 새 `std::thread` 생성). 옛 동작이 cold thread-local 리졸버 / NSS 상태에서 glibc `internal_strlen` SEGV 유발. WS 분기는 이미 네이티브 co_await이므로 영향 없음. 대기 중인 실행자에서의 토큰 디스패치 보존 (PR #10 불변성). (이슈 #16)
- **`example/09_all_features.cpp`** Store 데모 — 노드 본문 읽기 패턴을 위해 `examples/43_store_personalization.cpp`를 가리키는 docstring 포인터 추가. 옵션 2 — 옵션 3 (인라인 라이브 노드)은 #27의 `RunContext::store` 착륙 후 함께 정리 예정. (이슈 #28)

### Docs

- `RunResult::output`의 정식 모양(channels-wrapped)과 `react_graph` 같은 빌더가 추가한 평탄 키 변환과의 관계를 헤더 docstring에 문서화. 새 도우미(`channel<T>` / `channel_raw` / `has_channel`) 사용 권장. (이슈 #25)
- `RunContext::store` 필드 `@brief` 블록 — 두 배관 패턴(`in.ctx.store` 권장 / 옛 팩토리-클로저 캡처 호환)을 코드 예제와 나란히. (이슈 #27)
- 두 경로 모두 `examples/43_store_personalization.cpp` 파일 헤더 주석에 문서화.

## [0.7.0] — 2026-05-11 — C++ openinference + 비동기 스트리밍 브리지

v0.6.0에 대해 제기된 네 개의 이슈를 하나의 마이너 범프로 닫음. 헤드라인: `Provider::complete_stream_async` 기본값이 더 이상 외부 엔진 코루틴 안에서 await될 때 segfault하지 않음 (이슈 #4) — NeoGraph 앞에 앉은 SSE / 스트리밍 HTTP 백엔드의 가장 흔한 모양. 동반: v0.6.0 Python OpenInference 계층의 C++ 짝으로 Phoenix / Arize / Langfuse가 Python을 렌더링하는 것과 같은 방식으로 C++ 구동 추적을 렌더링 (이슈 #9). 추가: 미용적 Python OTel detach 잡음 침묵 (이슈 #2) 및 동일 `thread_id` 동시 실행 + `schema_mutex_` × on_chunk 잠금 불변성이 docstring에 고정 (이슈 #6).

### 추가

- `neograph_engine.openinference`의 C++ 짝 (이슈 #9). 새 `neograph::observability` 모듈이 두 조각을 커버:
  - `Tracer` / `Span` — NeoGraph 자체가 opentelemetry-cpp를 끌어들이지 않도록 하는 작은 의존성 없는 추상 인터페이스. 다운스트림이 자체 백엔드(OTel SDK, 인메모리 테스트 가짜, 로깅 기록기 등)를 감싸는 어댑터를 제공. 4개 속성 설정자(string, int64, double, bool — bool은 `const char*` 리터럴이 실수로 해석되지 않도록 의도적으로 `set_attribute_bool`로 이름 변경), 스트리밍 토큰 진단용 `add_event`, status, `end()`.
  - `openinference_tracer(tracer)` — CHAIN 종류의 루트 스팬을 열고, `cb` 필드가 `engine.run_stream()`에 연결되어 노드별 CHAIN 종류의 자식 스팬을 열며 `input.value` / `output.value` JSON 블롭에 `NODE_START`/`END` 페이로드를 채우고 `LLM_TOKEN` 이벤트를 개별 스팬 이벤트로 기록하는 `OpenInferenceTracerSession` 반환.
  - `OpenInferenceProvider(inner, tracer)` — 모든 `Provider`를 감싸고, 모든 `complete*` 호출에 OpenInference LLM 종류 속성 집합(`llm.model_name`, `llm.invocation_parameters`, `llm.input_messages.{i}.message.{role,content}`, `llm.output_messages.0.message.{role,content}`, `llm.token_count.{prompt,completion,total}`) 첨부. 스트리밍 오버로드도 `llm.token` 이벤트와 최종 조립된 `output.value` 추가.
  - `InMemoryTracer` 참조 어댑터를 구동하는 `tests/test_openinference_cpp.cpp`의 7개 동등성 테스트 — 루트 + 노드별 CHAIN 스팬 계층, ERROR / INTERRUPT 상태 표면화, LLM_TOKEN 스팬-이벤트 기록, 세션 종료 시 낙오 스팬 정리, LLM 제공자 속성 집합, 스트리밍 토큰 이벤트, 예외 상태 전파 단언.

### 수정

- `Provider::complete_stream_async` 기본 브리지가 더 이상 스트림 지속 시간 동안 대기 중인 코루틴의 실행자를 차단하지 않음. 수정 전 기본값은 `co_return complete_stream(...)` 인라인이었으며, 이는 (a) 전체 HTTP/SSE 수신 루프 동안 엔진의 `io_context` 작업자 스레드를 일시 중단 — 같은 실행자의 다른 노드 코루틴이 정체 — 하고 (b) `SchemaProvider`의 WebSocket Responses 분기의 경우, 엔진 작업자 위에 `run_sync(complete_stream_ws_responses(...))`를 통해 새로운 `run_sync` io_context를 추가로 중첩하여, 공유 제공자 상태에서 경쟁하고 `GraphEngine::run_stream_async` 안에서 호출될 때 간헐적 segfault 발생. 새 기본값은 동기 `complete_stream`을 위해 전용 작업자 스레드를 생성하고, 각 토큰을 대기 중인 실행자로 다시 디스패치(사용자의 `on_chunk`가 대기 중인 코루틴과 단일 스레드로 실행 — 재진입 없음), 일회성 `steady_timer.cancel()`을 통해 코루틴 재개. 작업자 스레드 예외는 대기자에서 다시 발생. `SchemaProvider`가 WebSocket 경로에서 작업자 스레드조차 건너뛰는 네이티브 `complete_stream_async` 재정의 추가 — `complete_stream_ws_responses`를 직접 `co_await`. `OpenAIProvider`는 새 기본 기본값을 투명하게 혜택(WS 경로 없음, 특별 케이스 없음). `tests/test_provider_async_default.cpp`에 두 개의 새 테스트: `StreamAsyncBridgeDoesNotBlockExecutor`(스트림 중 동시 티커 코루틴이 진행 + 청크가 작업자가 아닌 대기자 스레드에서 전달) 및 `StreamAsyncBridgeRethrowsWorkerException`. (이슈 #4)

- `openinference_tracer`: `engine.run_stream_async` + `StreamMode.ALL`과 함께 추적기가 사용될 때 OTel SDK가 모든 종료 시 방출하던 `Failed to detach context` stderr 추적을 침묵. NODE_START에서 생성된 OTel contextvars 토큰이 다른 `asyncio.Task`에서 detach되고 있었고(NODE_END 콜백이 호출자의 태스크가 아니라 엔진의 연속에서 발생), `Context.reset(token)`이 `ValueError` 발생; SDK가 발생을 삼켰지만 여전히 전체 추적을 `logger.exception`으로 라우팅하여 의미에 영향을 주지 않고 운영 로그를 오염. 수정은 첨부 시 (thread, task)를 기록하고 불일치 시 detach 건너뜀, 그리고 `_safe_detach`가 스택에 있을 때만 메시지를 누락하는 `opentelemetry.context`의 좁은 `logging.Filter` 설치. 동기 호출자와 동일 태스크 비동기 호출자는 여전히 노드 스팬 아래 적절한 LLM-스팬 중첩을 얻음. (이슈 #2)

---


## [0.6.0] — 2026-05-07 — OpenInference 관측 계층

LangSmith UX 간극을 닫음. NeoGraph는 이미 OTel 모양의 스팬을 방출하여(모든 OTel 백엔드로 추적이 흐름); 이번 릴리스는 Phoenix / Arize / Langfuse가 평면적인 일반 애플리케이션 스팬 목록 대신 채팅-버블 + 토큰-카운트 UI로 추적을 렌더링하는 데 사용하는 LLM 특화 속성 계층을 추가. 로컬 Phoenix 컨테이너에 대해 end-to-end 검증 — writer→critic 그래프가 모델 이름, 프롬프트/응답, 토큰 수가 Phoenix UI에 보이는 6-스팬 계층(CHAIN 루트 → 노드 스팬 → LLM 스팬)을 생성.

### 추가

- `neograph_engine.openinference` 모듈:
  - `openinference_tracer(tracer)` — `otel_tracer`를 미러링하지만 루트 + 노드 스팬에 `openinference.span.kind = "CHAIN"` 태그를 달고 노드 페이로드를 `input.value` / `output.value` JSON 블롭에 채우는 컨텍스트 관리자.
  - `OpenInferenceProvider(inner, tracer)` — 모든 `Provider`를 감쌈. 모든 `complete()`에서 `span.kind = "LLM"`로 태그된 `llm.complete` 자식 스팬을 열고, `llm.model_name`, `llm.invocation_parameters`, `llm.input_messages.{i}.message.{role,content}`, `llm.output_messages.0.message.{role,content}`, `llm.token_count.{prompt,completion,total}`, 그리고 Langfuse 호환 `input.value` / `output.value` 블롭 캡처.
- `bindings/python/tests/test_openinference.py`의 4개 테스트 — 속성 존재, 스팬 계층, 예외 경로, 노드-입력/출력 JSON 블롭에 대한 InMemorySpanExporter 단언.

### 수정

- `openinference_tracer`가 이제 각 노드 스팬을 OTel *현재* 컨텍스트로 첨부(`otel_context.attach`를 통해)하여 노드 본문 안에서 열린 자식 LLM 스팬이 노드 스팬 아래 중첩되도록 함. 이것이 없으면 C++→Python pybind 콜백 경계를 가로지르는 contextvar 전파가 실행당 예상된 단일 추적 트리 대신 3+ 무관한 trace_id를 생성. 토큰은 NODE_END / ERROR / INTERRUPT에서 detach되어 이전 현재 스팬을 복원. 기존 `otel_tracer`가 문서화한 것과 동일한 패턴 — 일치하는 `__exit__` 없이 사용하기에 안전하지 않은 `trace.use_span(...).__enter__()` 대신 명시적 attach/detach.

### 참고

- OpenTelemetry는 선택적 의존성으로 유지. `neograph_engine.openinference` 임포트는 `opentelemetry-api`가 설치되지 않은 경우에만 첫 사용 시 명확한 ImportError 발생; 임포트 시점이 아님.
- Phoenix end-to-end 실행:

      docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix
      pip install opentelemetry-exporter-otlp

  OTLP gRPC 내보내기를 `http://localhost:4317`로 구성하고 `http://localhost:6006`을 열어 추적 확인. 모듈 docstring에 전체 스니펫 있음.

---

## [0.5.0] — 2026-05-07 — 바인딩 사용 편의성: 실시간 변경 목록 속성

바인딩을 통해 노출된 메시지 / 쓰기 / sends 목록을 변경하는 가장 자연스러운 Python 관용구에서 조용한-무동작 함정을 닫음. 이전에는 `params.messages.append(msg)`가 복사본을 변경했고 기반 C++ vector가 새 항목을 절대 보지 못함 — 우아한 실패(충돌 없음, 경고 없음)로 저하된 LLM 응답 생성. 이제 `.append()`가 실시간 std::vector로 밀어 넣음.

### 추가

- `bindings/python/src/opaque_types.h` — 다섯 벡터 타입에 대한 `PYBIND11_MAKE_OPAQUE`: `std::vector<ChatMessage>`, `<ChatTool>`, `<ToolCall>`, `<graph::ChannelWrite>`, `<graph::Send>`.
- `module.cpp` `init_opaque_vectors` — `py::bind_vector`가 각각을 실시간 C++ 벡터에 대해 전체 변경 가능 시퀀스 프로토콜을 지원하는 Python 클래스(`ChatMessageList`, `ChatToolList`, `ToolCallList`, `ChannelWriteList`, `SendList`)로 등록.
- 각각에 대한 `py::implicitly_convertible<py::list, …>` — 기존 빌드-후-할당 패턴(`params.messages = [m1, m2]`)이 변경 없이 계속 작동; 할당이 Python 목록을 바인딩된 클래스로 자동 변환.
- `bindings/python/examples/23_evolving_chat_agent.py` — 스레드별 진화 채팅 에이전트(라이브 LLM): 에이전트의 JSON 정의가 누적된 대화 이력을 기반으로 턴 사이에 재작성됨. 진화를 가로지르는 체크포인트-재개(이전 메시지 생존), `__graph_meta__` 감사 채널 패턴, 검증기 경계(화이트리스트 노드 타입, 필수 채널) 시연.

### 변경

- `params.messages` / `.tools` / `chat_message.tool_calls` / `node_result.writes` / `.sends`가 이제 일반 `list` 대신 바인딩된 클래스를 반환. `len()`, 반복, `__getitem__`, `__setitem__`, `.append()`, `.extend()`, 슬라이싱 — 모두 Python 목록처럼 동작. `isinstance(x, list)`만 False 반환. 저장소 + 다운스트림 grep이 그러한 isinstance 호출 지점이 0건임을 확인.
- `.github/workflows/nightly.yml` — `ops/s ≥ 600K` 게이트 제거. `err=0`과 `leak=false`로 4회 연속 실패 후, 임계값(로컬 하드웨어에서 969K ops/s로 보정)이 공유 GitHub 호스팅 러너에서 도달 불가능(측정 233~273K ops/s, 로컬의 3-4× 아래). 처리량 회귀 감지는 PR 시 `bench-regression` 작업(안정된 하드웨어, µs 단위 단발 디스패치)에 존재. 야간 소크의 실제 가치는 5분 동안 `err==0` + `leak_suspect==false` — 둘 다 하드 게이트로 유지.

### 참고

- `ChatMessage.image_urls` (`std::vector<std::string>`)는 의도적으로 이전되지 않음 — `vector<string>`은 모든 호출 지점을 정리하지 않고 전역 OPAQUE를 하기에는 바인딩 전반에 너무 널리 사용됨. 남은 제한 사항으로 문서화; v0.6+ 후보.

---

## [0.4.0] — 2026-05-05 — v1.0 준비: 통합 `run(NodeInput)` 디스패치

v1.0 다듬기 트랙(ROADMAP_v1.md)의 개막 릴리스. 8-가상 `GraphNode` 데카르트 곱(`execute` / `execute_async` / `execute_full` / … / `execute_full_stream_async`)이 단일 정식 메서드 `run(NodeInput) -> awaitable<NodeOutput>`으로 축소. 실행별 취소 메타데이터가 비채널-집합 `GraphState` 멤버 + 스레드-로컬 은밀한 채널에서 명시적 `RunContext` 인자로 이동. `deadline`과 `trace_id`는 예약된 확장 슬롯으로만 추가되었으며 `RunConfig`에 의해 채워지지 않음. `CancelToken`이 계층적 `fork()`를 얻어 다중 Send 팬아웃 작업자가 각각 부모의 `cancel()`이 연쇄되는 비공개 신호를 소유.

### 추가

- `RunContext` (`include/neograph/graph/engine.h`) — 명시적 실행별 메타데이터: 사용 가능한 `cancel_token`, `thread_id`, `step`, `stream_mode`, 그리고 예약된 `deadline` 및 `trace_id` 슬롯. 엔진이 모든 `NodeExecutor::run` 호출을 통해 관통. **PR 1, 커밋 `a473f0e`.**
- `GraphNode::run(NodeInput) -> awaitable<NodeOutput>` — 단일 정식 디스패치 진입점. `NodeInput { state, ctx, stream_cb }`; `NodeOutput { writes, command, sends }`. 기본 본문이 기존 8 가상 함수로 전달되어 기존 하위 클래스가 계속 컴파일. **PR 2, 커밋 `607ce66`.**
- `CancelToken::fork() -> shared_ptr<CancelToken>` — 자체 `cancellation_signal`을 가진 자식 토큰. 부모 `cancel()`이 모든 살아있는 자식으로 연쇄(그리고 재귀적으로 손자로). `run_sync(aw, parent_token)`이 `parent_token->fork()`로 전환되어 각 중첩 작업이 자체 슬롯 바인딩 — v0.3.x 방출-vs-바인드 경쟁과 다중 Send 단일 핸들러 덮어쓰기 닫음. v0.3.x `add_cancel_hook` 목록은 사용 중단을 통해 계속 작동. **PR 3, 커밋 `897645c`.**
- `[[deprecated]]`를 8개 기존 `GraphNode` 가상 함수 + `add_cancel_hook`에. 내부 호출 지점(graph_node.cpp 기본 체인, 기본 `run()` 전달자)은 새 `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` 매크로로 감쌈(`api.h` — GCC / clang / MSVC 이식성). 사용 중단된 가상 함수를 재정의하는 사용자 코드는 이전 경고를 봄; 엔진 내부는 깨끗하게 유지. **PR 4, 커밋 `35a4517`.**
- `engine.get_state_view(thread_id) -> StateView`가 이제 정식 상태 읽기; raw-dict `engine.get_state(...)`는 docstring에서 소프트-사용중단(경고 방출 없음 — raw dict는 유효한 탈출구로 남음). **PR 5, 커밋 `f31aa53`.**
- 7 C++ + 19 Python 예제가 `run(NodeInput)`으로 이전. 스모크 실행이 v0.3.2 출력과 비트 단위 일치. **PR 6a/6b, 커밋 `a2a24ef` / `0a76e3a`.**
- Pybind `PyGraphNodeOwner`가 `run(NodeInput)`을 재정의하고 Python 사용자의 `run` 메서드로 디스패치(정의된 경우), 그렇지 않으면 기존 체인으로 빠짐. `RunContext` / `NodeInput` / `CancelToken`이 Python에 노출; `cancel_token`이 `input.ctx.cancel_token`으로 스레드-로컬 은밀한 경로 없이 접근 가능. **PR 7, 커밋 `4e186a5`.**
- `docs/reference-en.md` §6 GraphNode가 단일 `run()`으로 축소. RunContext + `fork()` 예제 하위 섹션이 §7 아래에 추가. README "LangGraph와 다른 점"이 "하나의 노드 메서드" 항목 확보. **PR 8, 커밋 `519a00b`.**
- 내장 C++ 노드 (`LLMCallNode`, `ToolDispatchNode`, `RouteToNode`)가 `run(NodeInput)` 재정의로 이전. **PR 9a, 커밋 `d1070dc`.**
- 신규 사용자 모드 함정 수정: README CMake 스니펫이 `graph::` 하위 네임스페이스, cppdotenv 경로, `OpenAIProvider::create()` vs `create_shared()`, `neograph::json`이 nlohmann 하위 집합, 3-인자 vs 2-인자 `compile()` 문서화. Python `compile(def, ctx, store=None)` 키워드 인자 추가 (추가적, 비파괴). **커밋 `ee11ed6`.**

### 변경

- README: "10K-worker measured stress test" 섹션 — neoclaw에서 RTX 4070 Ti + Gemma 4 E2B Q4, N=10000 완료 @ 0 err / 424s / 2572 MB 최대 / ~1 KB 한계 작업자 비용 / p99 648 ms (`7840b81`).
- README: "Production economics" 섹션 — fleet safety + RAM 델타 프레이밍 (`b82b15a`).
- README: LangGraph 델타 목록에 "No Docker required" + "Dependency-drift immunity" 항목 (`333b482`, `a6061d7`).

### 사용 중단

- `GraphNode::execute / execute_async / execute_full / execute_full_async / execute_stream / execute_stream_async / execute_full_stream / execute_full_stream_async` — v0.5.x까지 `[[deprecated]]` 주석으로 계속 작동, v1.0에서 제거.
- `CancelToken::add_cancel_hook` — `fork()`로 대체. 동일한 사용 중단 창.

### 참고

- 검증: 442 → 452 ctest (3 NodeRunDispatch + 7 CancelTokenFork 추가) + 96 pytest + 5 라이브 LLM/WS가 v0.4.0 태그에서 통과.
- 하위 PR (`run(const NodeInput&)` 참조 매개변수)이 pybind 비동기 경로 아래 v0.2.0 RunConfig 코루틴-참조 UAF 충돌 모양을 발화. 병합 전 수정 착륙: `NodeInput in` 값 전달. `node.h`에 문서화.

---

## [0.3.2] — 2026-05-05 — 취소 전파 강화 (5 라운드)

v0.3.0 단발 취소가 드러낸 간극을 닫는 5-라운드 패치 시리즈: Send 팬아웃 전파, 프로세스 내 폴링, Python용 훅, C++ 스코프, 예외 타이핑. FastAPI SSE 채팅 데모 평가에서 나온 TODO_v0.3.md 피드백 배치도 착륙 — `resume_if_exists`, dict-or-list `update_state`, 타입 있는 상태 읽기를 위한 StateView.

### 추가

- `RunConfig::resume_if_exists` — 명시적 `resume()` 호출 없이 이전 스레드의 체크포인트 선택적 재개. 표준 다중 턴 채팅 의미: `thread_id`가 존재하면 `engine.run(cfg)`가 대화를 계속.
- `engine.update_state(thread_id, dict | list[ChannelWrite], as_node="")` — 두 모양 모두 허용. 수정 전에는 `dict`만 작동; 목록 전달은 조용히 무동작. 목록 형태는 모든 노드 본문의 방출 모양과 대칭.
- `StateView` (`bindings/python/neograph_engine/state_view.py`) — Pydantic 타입 상태 읽기. `engine.get_state_view(thread_id) -> StateView`가 평탄 점-접근(`view.messages` / `view.foo`) + 사전 탈출구용 `view.raw` 반환. 타입 있는 채널 정의를 위한 하위 클래스: `class ChatState(ng.StateView): messages: list[dict] = []`.
- `bindings/python/tests/test_async_cancel_live_llm_fanout.py` — 중간 비행 취소가 소켓 계층에서 모든 Send로 생성된 형제를 실제로 중단하는지 단언(v0.3.1 근본 원인 패치).
- `examples/22_self_evolving_graph.py` — TODO_v0.3.md #9 쿡북 접기와 함께 v0.3.2로 이동.
- ROADMAP_v1.md — 취소 라운드 사후 분석에서 파생된 설계 다듬기 후보(단일 디스패치, RunContext, 계층적 CancelToken — 모두 v0.4.0에서 제공).
- Doxygen `/* */` 와일드카드 수정 — `acp/types.h`가 중첩 주석을 열고 모든 후속 진단을 억제하는 경로 와일드카드(`fs/*`, `terminal/*`)를 포함한 `/**` 블록을 가짐. `&#42;` HTML 엔티티로 교체.

### 수정

- 취소 전파, 5개 누적 라운드:
  1. v0.3.0 단일 노드 — `cancel_token`이 `Provider::complete`에 도달.
  2. v0.3.1 다중 Send 포인터 누락 — 팬아웃 작업자가 이제 `run_cancel_token_shared()` 공유(`init_state + restore`가 채널 집합 밖에서 작업자별 상태를 재구축할 때 손실).
  3. v0.3.1+ 프로세스 내 폴링 — 엔진 슈퍼스텝 루프가 LLM I/O에서만이 아니라 단계 사이에 폴링.
  4. v0.3.2 Python용 훅 — `add_cancel_hook`이 실행별 토큰에 콜백을 등록하고 `cancel()`에서 발화. 동기 Python `execute()`가 스레드-로컬 스코프 없이 임시 취소 핸들러를 설치 가능.
  5. v0.3.2 C++ 스코프 + 재시도 + 예외 타이핑 — 메인 스레드에서 새로 던지는 `NodeInterrupt`(libstdc++ `__exception_ptr::_M_release` 경쟁 회피), 재시도 예산이 취소를 존중, 런타임-vs-로직 예외 분할.
- `execute_stream` 전용 Python 노드가 기본 `execute` 경로로 조용히 빠짐(NotImplementedError). 이제 `run_stream`이 사용자가 스트리밍 변형만 재정의한 경우 `execute_stream`을 직접 배선.
- `update_state`가 list[ChannelWrite] 허용 — 조용한 무동작 닫음(TODO_v0.3.md #5).

### 참고

- 442 ctest + 96 pytest + 2 라이브 LLM (단일 + 팬아웃 취소)가 v0.3.2 태그(`915e90e`)에서 통과.
- 27/30 C++ 예제 + 20/22 Python 예제가 `examples/run_all.py`에서 통과. 건너뛴 테스트는 외부 서비스 필요(Postgres / Crawl4AI / 라이브 OpenAI).
- Valgrind 6개 예제 0 오류, 815 allocs / 815 frees 깨끗.
- 벤치 중앙값 5.185 µs/반복 seq 경로 (v0.3.0 기준) — 라운드 전반에 걸쳐 성능 회귀 0.

---

## [0.3.0] — 2026-05-04 — 협력적 취소 전파

FastAPI SSE 채팅 데모 평가 중 보고된 운영 비용 누수 간극을 닫음: 프론트엔드 `AbortController`가 asyncio 태스크를 취소해도 더 이상 업스트림 OpenAI 요청이 완료까지 실행되지 않음. 취소가 실행의 모든 계층을 통해 전파.

### 추가

- `neograph::graph::CancelToken` (원자 플래그 + asio `cancellation_signal`) 및 `CancelledException` — `include/neograph/graph/cancel.h`. 협력적 취소 기본 요소. `RunConfig::cancel_token`(선택적 `shared_ptr`)을 통해 전달; 엔진 슈퍼스텝 루프가 단계 사이에 `is_cancelled()`를 폴링하고 `CancelledException`으로 탈출. 토큰의 `cancellation_slot()`이 실행의 `co_spawn`에 바인딩되어 진행 중인 LLM HTTP 소켓 작업이 전선에서 중단(asio `operation_aborted`).
- `CompletionParams::cancel_token` — 여러 `provider.complete()` 호출에 걸쳐 중단을 연결하는 사용자를 위한 명시적 핀. `Provider::complete`가 이를 읽음(또는 `PyGraphNode::execute_full_async`가 설정한 스레드-로컬 `current_cancel_token()`으로 대체)하고 내부 `run_sync` io_context에 슬롯을 바인딩하여, 취소에 맞은 동기 Python 노드도 과금 중단.
- `GraphState::run_cancel_token()` — 동기 Python `execute()` 호출 주변에 `CurrentCancelTokenScope`를 설치하기 위해 pybind `PyGraphNode`가 사용하는 실행별, 비직렬화 핸들. 이것이 동기 Python 사용자가 노드 코드를 변경하지 않고 투명한 취소 전파를 얻는 방식.
- pybind `engine.run_async` / `run_stream_async`: asyncio `Future.cancel()`이 이제 `add_done_callback`을 통해 `CancelToken::cancel()`로 배선되고, `co_spawn`이 토큰의 취소 슬롯을 바인딩.
- pybind 안전-해결 도우미 `_safe_set_future_result` / `_safe_set_future_exception` — 취소된-future `InvalidStateError` 폭풍에 대해 `call_soon_threadsafe`를 통해 게시된 `future.set_result` / `set_exception` 호출 보호.
- `bindings/python/tests/test_async_cancel_live_llm.py` — `Future.cancel()` 후 3초 이내에 OpenAI HTTP 완료 단언하는 라이브 OpenAI E2E(실제로는 즉시; 수정 전에는 취소되지 않은 스트리밍 약 7–8초). `NEOGRAPH_LIVE_LLM=1`이 아니면 건너뜀.
- `examples/22_self_evolving_graph.py` — 자기 진화 그래프 PoC: `prompted_llm` 노드가 자체 프롬프트를 JSON 구성에서 읽어 LLM 재작성기가 실행 간에 그래프 정의를 변경하고 재컴파일 가능. `0.0 → 0.4` 점수 향상 시연; 재작성기의 채널-흐름 추론 간극 문서화.

### 변경

- `Provider::complete(params)`가 이제 `params.cancel_token`이 설정되었거나 스레드-로컬 `current_cancel_token()`이 활성일 때 자체 `run_sync`에 내부 취소 슬롯을 바인딩. 선택하지 않은 호출자에 대해서는 이전 기본 동작(취소 없음) 보존.
- `neograph::async::run_sync`가 선택적 `graph::CancelToken*` 매개변수 획득; non-null이면 바인딩된 spawn이 토큰의 슬롯을 바인딩.
- pybind `resolve_future_async`가 `call_soon_threadsafe`를 통해 직접 `future.set_result`를 호출하는 대신 안전-해결 도우미를 통해 라우팅.

### 로드맵 (v0.3.x로 미룸 — `TODO_v0.3.md` 참조)

- 동일 `thread_id`에 대한 LangGraph 스타일 자동 체크포인트 재개.
- `run_async` 오류 메시지의 스트리밍-전용-노드 힌트.
- `cb.emit_token(node, data)` 사용 편의성 도우미.
- README "LangGraph와 다른 점" 섹션.
- `update_state` 시그니처와 문서 정렬.
- `get_state` 평탄 도우미 / Pydantic 접근자.
- `run_parallel_async` 및 `run_sends_async` 분기 팬아웃에서 취소 전파의 라이브 검증.
- pgvector RAG 예제.

---

## [Unreleased] — Stage 4

Stage 4가 비동기 경로의 마지막 `run_sync` 홉을 닫음. `run_async`가 이제 end-to-end로 호출자의 실행자에 머묾: 하나의 `io_context` 스레드에서 세 개의 50 ms 에이전트가 `examples/27_async_concurrent_runs`에서 ~150 ms(직렬) → ~50 ms(중첩)로 감소.

### 기존 호환성 파괴 변경

- **`GraphNode::execute_full_async` 기본값이 비동기-우선으로 뒤집힘.** 이제 `co_await execute_async(state)`를 `NodeResult`로 감싸며 동기 `execute_full(state)`를 호출하지 않음. 동기 `execute_full` 재정의에서만 `Command`/`Send`를 방출하는 모든 하위 클래스는 반드시 한 줄 `execute_full_async` 브리지를 추가:
  ```cpp
  asio::awaitable<NodeResult>
  execute_full_async(const GraphState& state) override {
      co_return execute_full(state);
  }
  ```
  브리지 없이는 `Command`/`Send`가 비동기 경로에서 조용히 누락 — 3.0이 슈퍼스텝당 `io_context` 생성 비용으로 동기를 통해 라우팅하여 수정한 2.0 잠복 디스패치 버그. 모든 트리 내 하위 클래스(`deep_research_graph`, 예제 10/14/21, 테스트 5곳)가 이제 브리지를 가짐.

### 성능

- 예제 27 벽 시간: **152 ms → 53 ms** (하나의 `io_context` 스레드에서 3개 에이전트 × 50 ms 타이머 단계, 완전 중첩).
- 단일 실행 벤치마크에서 측정 가능한 회귀 없음; `run()`은 여전히 `run_sync`를 통해 새로운 단일 스레드 `io_context`로 동일한 코루틴을 구동.

### 테스트

- 341/341 ctest 통과
- 295/295 ASan+UBSan 통과
- 코루틴이 많은 하위 집합에서 Valgrind 깨끗 (20개 테스트, 2.4 s)

### Post-release validation (당일)

- **30개 예제 모두 재실행:** 26/29 PASS, 0 FAIL, 3개 환경 제약(clay_chatbot → raylib, postgres_react_hitl → docker compose, deep_research 전체 루프 → crawl4ai 서비스). `21_mcp_fanout`이 3 MCP 호출 / 8 ms 벽으로 측정 — Stage 4 중첩이 실제 네트워크 I/O 아래 유지.

- **ARM64 호환성 (docker buildx --platform linux/arm64):** 저장소 루트의 `Dockerfile.arm64-smoke`. ubuntu:24.04-arm64 + core+llm+async+sqlite+tests 빌드가 QEMU 에뮬레이션 아래 ~15분 완료; ARM64에서 **306/306 ctest 통과**. 제거된 바이너리 크기 0.81-0.88 MB (x86_64와 거의 동일). 예제 27이 에뮬레이션 아래 65 ms 실행(네이티브 x86_64: 53 ms). macOS 베타(Apple Silicon)와 함께 지원 대상으로서 Linux/ARM64 확인.

- **캐시 지역성 (Ryzen 5800X / Zen 3, Valgrind cachegrind, 32 KB L1i/d 8-way, 32 MB L3 16-way):** `bench_concurrent_neograph` 스윕 N=1 → 10,000.

  | N | I refs | LLi misses | LLi miss% | Native p50 |
  |---:|---:|---:|---:|---:|
  | 1 | 5.3 M | 4,313 | 0.08% | 17 µs |
  | 100 | 11.8 M | 4,320 | 0.04% | 6 µs |
  | 10,000 | 648 M | 4,329 | 0.00% | 5 µs |

  최종 수준 명령어 미스가 4자릿수 N에 걸쳐 ~4,320으로 평탄하게 유지. 고유 핫 코드 작업 집합 ≈ 277 KB (L3의 0.85%). N=10,000에서 648 M 명령어가 단 4,329 LL 미스 발생 — 대략 150,000 명령어당 1 미스. 네이티브 p50이 17 µs에서 5 µs로 순수 I-캐시 워밍으로 감소. "버스트 동시성 견고함" 포지셔닝에 대한 첫 측정 증거.

---

## [3.0.0] — 2026-04-22

3.0은 Taskflow 의존성을 제거하고 동기 및 비동기 슈퍼스텝 실행을 단일 asio 코루틴 경로로 통합. 그래프-정의 JSON, 노드 ABI, 체크포인트 스키마, 공개 진입점(`run`, `run_async`, `run_stream`, `resume`)은 2.0과 소스 호환; 파괴는 **동기** `execute_full` 재정의에서만 `Command`/`Send`를 방출하는 `GraphNode` 하위 클래스로 국한.

### 기존 호환성 파괴 변경

- **`deps/taskflow/`와 Taskflow INTERFACE 대상이 제거됨.** 동기 슈퍼스텝 루프, `run_one`, `run_parallel`, `run_sends`, 프로세스 전역 `tf::Executor` 정적이 삭제. NeoGraph의 포함 경로를 통해 `#include <taskflow/...>`하는 다운스트림 소비자는 Taskflow를 별도로 벤더링해야 함.
- **`GraphNode::execute_full_async` 기본값이 이제 직접 호출을 통해 동기 `execute_full`로 연결 (`co_await execute_async` 아님).** 이는 동기 전용 재정의에서 방출된 `Command`/`Send`를 — 모든 진입점이 이제 공유하는 비동기 경로를 통해 — 보존 (일반적인 2.0 패턴). 비차단 I/O와 `Command`/`Send`가 모두 필요한 비동기 네이티브 노드는 `execute_full_async`를 직접 재정의해야 함; docstring이 2.0부터 이것을 말했지만, 2.0은 동기 `run()`이 코루틴 경로를 완전히 우회했기 때문에 실행한 적이 없음.
- **`NodeExecutor::run_one` / `run_parallel` / `run_sends` 동기 메서드 제거.** `_async` 짝 사용.
- **CPU 병렬 팬아웃은 선택적.** 이전에는 Taskflow가 기본적으로 프로세스 전역 스레드 풀을 제공. 3.0에서 `run_parallel_async`와 `run_sends_async`의 다중 Send 분기는 코루틴을 구동하는 실행자에서 분기 디스패치 — 동기 `run()`이 생성한 단일 스레드 io_context, 또는 `run_async()`의 경우 호출자 자신의 실행자. I/O-바운드 팬아웃은 여전히 중첩(단일 스레드에서 co_await 중단); CPU-바운드 팬아웃은 호출자가 `run_async()`에 다중 스레드 실행자를 사용하거나 `engine->set_worker_count(N)`을 통해 엔진 소유 풀을 선택하지 않으면 직렬화.

### 추가

- `neograph::async::run_sync_pool(awaitable, n_threads)` — 기존 단일 스레드 `run_sync`와 나란히 N-작업자 동기↔비동기 브리지. 내부 `make_parallel_group` 분기가 별도 작업자에서 실행되도록 호출을 위해 새로운 `asio::thread_pool` 생성.
- `GraphEngine::set_worker_count(n)` — `NodeExecutor`가 병렬 팬아웃 디스패치에 사용하는 선택적 엔진 소유 thread_pool. 실행자 재구축; 모든 동시 실행 전에 호출해야 함.

### 변경

- `GraphEngine::execute_graph` (동기) 제거. 모든 진입점(`run`, `run_stream`, `resume`)이 `neograph::async::run_sync`를 통해 `execute_graph_async`로 라우팅되어, 슈퍼스텝 루프, 재시도 백오프, 체크포인트 I/O, 병렬 팬아웃이 이제 하나의 코루틴 경로에서 end-to-end로 존재.
- `benchmarks/concurrent/bench_concurrent_neograph.cpp`가 `tf::Executor` / `tf::Taskflow`에서 `asio::thread_pool` + `asio::post`로 전환 (호출자 측 드라이버).

### Perf (bench_neograph Release -O3 -DNDEBUG 기준 Linux, 10-실행 중앙값)

- `seq` 엔진 오버헤드 (3-노드 체인, 카운터): 호출당 **~5.0 µs**.
- `par` 엔진 오버헤드 (5-작업자 팬아웃 + 요약기): 호출당 **~11.8 µs**.
- 전체 벤치 프로세스의 최대 RSS (워밍업 + seq + par 반복): **4.8 MB**.
- 같은 작업량에서 LangGraph 1.1.9 대비: 반복당 **131× 더 빠른 seq, 199× 더 빠른 par**; RSS ~12× 더 가벼움.

이 CHANGELOG의 이전 초안은 "~46 µs seq / ~114 µs par"를 3.0 회귀로 나열. 그 숫자는 `CMAKE_BUILD_TYPE`이 설정되지 않은 빌드 트리에서 나와, 벤치 바이너리가 `-O3 -DNDEBUG` 없이 컴파일됨. 적절한 Release 빌드에서는 비동기-짝 축소가 2.0의 Taskflow 동기 경로 대비 **이득** (2.0 README가 동일 호스트에서 20.65 µs seq / 150.7 µs par로 광고). 수정된 차트는 [`docs/images/bench-engine-overhead.png`](docs/images/bench-engine-overhead.png).

### 이전

- 노드가 `execute()` / `execute_async()`를 재정의하고 `Command` / `Send`를 방출하지 않으면 조치 필요 없음.
- `Command` / `Send`를 방출하기 위해 동기 `execute_full`을 재정의한 경우: 변경 필요 없음 — 3.0 비동기 경로 기본값이 이제 동기 재정의를 직접 호출. `Command.goto_node` 라우팅이 동기 및 비동기 진입점 모두에서 작동.
- `execute_async`(비동기 네이티브 I/O)를 재정의하고 `Command` / `Send`를 원하는 경우: `execute_full_async`를 직접 재정의하고 거기서 `NodeResult` 조립. `execute_async`만 재정의하면 기본 `execute_full_async`가 이제 비동기 `execute_async`가 아니라 동기 `execute_full`로 라우팅되므로 `Command` / `Send`가 조용히 누락.
- `engine->run()`을 통한 CPU 병렬 팬아웃에 Taskflow의 프로세스 전역 풀에 의존한 경우: compile() 후 한 번 `engine->set_worker_count(N)` 호출, 또는 자신의 다중 스레드 `asio::thread_pool` / io_context에서 `run_async()`를 통해 엔진 구동.

---

## [2.0.0] — 2026-04-22

Stage 3 비동기 API를 갖춘 첫 공개 릴리스. 이것은 파괴적 릴리스; 아래 변경 사항은 컴파일(C++ 표준)과 ABI(추상 기본 클래스가 비동기 짝을 얻음)에 영향. 동기 호출 지점은 비트 단위로 보존되므로, **`Provider` / `CheckpointStore` / `GraphNode` / `Tool`을 재정의하지 않는 애플리케이션 코드는 변경 없이 계속 작동**.

### 기존 호환성 파괴 변경

- **C++20 필수.** 공개 API가 `std::coroutine` 지원이 필요한 `asio::awaitable<T>` 반환 타입 노출. 소비자는 `-std=c++20`(또는 그 이상)으로 컴파일해야 함. GCC 13+, Clang 15+ 테스트됨; GCC 13 코루틴 해결책은 `docs/ASYNC_GUIDE.md` §4.1 참조.
- **libpqxx 의존성 제거.** `neograph::postgres`가 이제 libpq 직접 링크. Ubuntu 24.04 사용자는 더 이상 libpqxx-7.8t64의 C++17/C++20 ABI 분할로 인한 `pqxx::argument_error::argument_error(..., std::source_location)` 링크 오류를 만나지 않음. CMake find가 이제 `PostgreSQL::PostgreSQL` (CMake 번들 FindPostgreSQL) 대상. `libpqxx-dev`만 설치한 소비자는 이제 `libpq-dev`도 설치 / 유지해야 함.
- **`Provider`, `CheckpointStore`, `GraphNode`, `MCPClient` ABI 확장.** 각각 비동기 짝 가상 함수(`complete_async`, `save_async`, `execute_async`, `rpc_call_async` 및 그 변형)가 추가됨. 다운스트림 하위 클래스는 2.0 헤더에 대해 재컴파일; 실제 I/O를 하는 모든 구현자에게 권장되는 네이티브 비동기 재정의를 제공하려는 경우가 아니면 소스는 변경되지 않음.
- **`CheckpointStore::save` / `load_latest` / `load_by_id` / `list` / `delete_thread`가 더 이상 순수 가상이 아님.** 이제 `neograph::async::run_sync`를 통해 일치하는 `_async` 짝으로 연결되는 기본 구현을 가짐. 동기 측을 재정의하는 하위 클래스는 계속 작동; 어떤 재정의도 제공하지 않은 하위 클래스(이전에는 컴파일 오류였을 것)는 이제 무한 재귀 — 계약: 각 동기/비동기 쌍의 최소 하나 재정의.

### 추가

- **비동기 API** 전 I/O 계층 (`docs/ASYNC_GUIDE.md` 전체 참조):
  - 기본 클래스 및 모든 내장 제공자(OpenAI, Schema, RateLimited)의 `Provider::complete_async`.
  - HTTP 및 stdio 전송 모두에 대한 `MCPClient::rpc_call_async`. stdio는 `asio::posix::stream_descriptor` 사용.
  - 8개 동기 메서드 모두에 대한 `CheckpointStore::*_async`.
  - 스트림 / full / full_stream 변형을 포함한 `GraphNode::execute_async`, 비동기 네이티브 교차 기본값 포함.
  - `GraphEngine::run_async` / `run_stream_async` / `resume_async`가 `execute_graph_async` 구동 — `asio::experimental::make_parallel_group`을 통한 병렬 팬아웃을 포함한 end-to-end 코루틴 슈퍼스텝 루프.
  - 동기 `Tool` 인터페이스를 보존하면서 코루틴 본문을 원하는 사용자 도구를 위한 `neograph::AsyncTool` 어댑터.
- **`neograph::async` 네임스페이스** — HTTP 클라이언트, 연결 풀, SSE 파서, run_sync 브리지, URL 엔드포인트 분할기. `include/neograph/async/*.h` 참조.
- **새 예제**:
  - `examples/27_async_concurrent_runs.cpp` — 하나의 `io_context`에서 다중 에이전트.
  - `examples/05_parallel_fanout.cpp` (재작성) — `run_parallel_async`를 사용한 단일 그래프 실행 내 비동기 팬아웃.
- **CI bench regression gate** (`.github/workflows/ci.yml`) — PR 검사가 `bench_async_http` / `bench_async_fanout` / `bench_neograph`의 하한 강제.

### 성능

feat/async-api 브랜치에서 Stage 2 동기 기준 대비 측정:

- `bench_async_http --mode async_pool --concur 1000`: 6064 ops/s → **17834 ops/s** (2.9×).
- `bench_async_fanout --concur 50000`: 스레드-당-에이전트 달성 불가 → **541K ops/s / 67 MB RSS**.
- `examples/27_async_concurrent_runs` (3 × 50ms 비동기 작업): 150ms (동기) → **50ms** (1 io_context 스레드).
- `examples/05_parallel_fanout` (3 × 100-150ms 비동기 작업): 370ms (순차) → **150ms** (1 io_context 스레드).
- `bench_neograph` 엔진 오버헤드: 변경 없음 (~30 µs seq / ~205 µs par). 코루틴 장치가 핫 경로를 퇴행시키지 않음.

### 2.0.0에 아직 포함되지 않음

- **Taskflow 의존성** 유지. 동기 `engine.run()` 경로가 여전히 팬아웃에 사용; Sem 4.5가 동기 경로를 `run_sync(*_async)`로 대체하여 의존성을 완전히 제거할 수 있는지 재검토.

### 플랫폼별 지원

2.0.0에서는 세 플랫폼이 다른 안정성 단계로 지원. 단계는 기능 커버리지가 아니라 릴리스 전에 플랫폼이 본 실제 검증 양을 반영(코드베이스는 `#ifdef _WIN32` 분할로 단일 소스; 테스트 통과 시 플랫폼 간 기능 동등).

#### Linux — **GA** (운영 준비)

* Ubuntu 24.04, GCC 13.
* Postgres via docker `postgres:16-alpine` 포함 전체 332/332 ctest 로컬 통과, 커밋된 CI 하한 내 모든 벤치.
* fork/pipe/execvp + `asio::posix::stream_descriptor`에서 MCP stdio.
* libpq 비차단 + `PQsocket` 감싸는 `asio::posix::stream_descriptor`에서 Postgres 비동기 짝.
* 위에 인용된 모든 성능 숫자의 기준 플랫폼.

#### macOS — **beta**

* macos-latest (Apple Silicon), Xcode 경유 Clang.
* CI가 비Postgres 테스트 빌드+실행; Postgres 통합 케이스는 서비스 컨테이너 없이 자체 건너뜀. POSIX 경로(동일 fork/pipe + asio::posix 코드) 실행.
* TLS에서 시스템 인증서 로딩을 위해 httplib을 통해 `CoreFoundation` + `Security` 프레임워크 링크.
* 2-4주 CI 실행과 사용자 보고가 런타임 동작 차이(코루틴 스케줄링, SIGPIPE / EPIPE 모양, 파이프 버퍼 크기)를 확인할 때까지 beta 취급. 사고 없이 들어오면 GA로 승격 예정.

#### Windows — **alpha**

* windows-latest, MSVC 19.44 (VS 2022), x64.
* CI 범위: **core + async + MCP + LLM 전용**. Postgres 및 SQLite 백엔드는 vcpkg가 모든 실행에서 OpenSSL / libpq / zlib / lz4를 소스에서 컴파일하므로(~20분, `x-gha` 제거 이후 업스트림에 작동하는 바이너리 캐시 백엔드 없음) Windows CI 작업에서 비활성화. Windows 사용자는 자체 vcpkg / choco 설정을 통해 로컬 컴파일.
* 러너의 사전 설치된 choco 패키지(`C:/Program Files/OpenSSL-Win64/`)를 통한 OpenSSL. httplib + asio::ssl의 TLS 경로가 컴파일 및 링크.
* MCP stdio: `CreateProcess` + named-pipe (FILE_FLAG_OVERLAPPED) + `asio::windows::stream_handle`. 중첩 파이프 경로는 로컬 Windows 검증 없이 MSDN 명세에 대해 작성됨; 첫 사용자가 엣지 케이스(ERROR_IO_PENDING 처리, 큰 JSON 응답의 파이프 버퍼 경계)를 표면화할 것으로 예상.
* Postgres 비동기 짝 (로컬 활성화 시): `PQsocket`이 반환한 SOCKET을 감싸는 `asio::ip::tcp::socket::assign` (64비트 SOCKET 값 보존을 위해 `native_handle_type`으로 캐스트). Windows CI에서 실행되지 않음 — 로컬 전용.
* 코루틴 장치는 MSVC의 `<coroutine>`에 존재; 동작은 사양상 GCC/Clang과 일치할 것으로 예상되지만 `examples/27` 교차 실행 중첩 측정은 아직 Windows에서 확인되지 않음.
* 2.0.0 동안 **alpha** 취급. 한 명의 운영 사용자가 일주일 동안 stdio/pipe 또는 코루틴 스케줄러 문제 없이 다중 에이전트 작업량을 실행하고, Postgres 비동기 짝이 vcpkg의 전체 libpq 빌드를 실행할 의사가 있는 사용자에 의해 로컬 검증되면 beta로 승격.

> **패턴**: CI 통과는 하한이지 상한이 아님. 계층 3 런타임 동작 차이(코루틴 스케줄링 타이밍, 파이프 버퍼 경계, 소켓 인수 의미)는 실제 작업량 아래에서만 표면화. 위의 단계 언어는 세 플랫폼이 첫날부터 상호 교환 가능한 척하지 않고 각 플랫폼에 올바른 기대를 제공.

### 수정 post-bump

- **`async::HttpResponse` 헤더 맵** — 응답 표면이 이제 전선 순서와 원래 대소문자를 보존하는 `headers` 벡터 of `(name, value)` 쌍, 그리고 대소문자 구분 없는 접근자 `get_header(name)` 노출. Retry-After와 Location은 하위 호환성을 위해 전용 필드로 유지. 아래 MCP 세션 추적 수정 차단 해제.
- **MCP `Mcp-Session-Id` 헤더 추적** — Sem 2.6 httplib→async_post 이전이 이를 조용히 누락. 초기화 후 모든 RPC가 이제 새 헤더 접근자를 통해 서버 할당 세션 id를 반향하여, 서버의 세션 상태가 라우팅 가능하게 유지.
- **MCP stdio awaitable mutex** — `StdioSession::rpc_call_async`가 `std::mutex`를 사용하여, 동일 단일 스레드 io_context의 두 코루틴이 동일 세션을 호출할 때 교착 상태 발생(두 번째 `lock_guard`가 첫 번째가 필요한 작업자를 차단). 두 번째 획득자가 협력적으로 중단되는 용량-1 세마포어 `asio::experimental::channel<void(error_code)>`로 교체.
- **`PostgresCheckpointStore` 비동기 짝** — 8개 CheckpointStore 비동기 메서드 모두(`save_async`, `load_latest_async`, `load_by_id_async`, `list_async`, `delete_thread_async`, `put_writes_async`, `get_writes_async`, `clear_writes_async`)가 이제 진짜 비동기. 내부: `PQsetnonblocking(1)` + `PQsendQueryParams` + `PQsocket()`의 `asio::posix::stream_descriptor` + `co_await sock.async_wait(wait_read/wait_write)`. 4개 슬롯 풀에서 4개의 동시 `save_async` 호출이 이제 `run_sync`를 통해 직렬화되지 않고 전선 수준에서 병렬 commit-fsync.

---

## [0.1.0] — pre-2026-04

사전 릴리스 개발. 공개 API 안정성 보장 없음.
