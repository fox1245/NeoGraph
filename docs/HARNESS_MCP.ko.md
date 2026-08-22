<!-- neograph-i18n: source=docs/HARNESS_MCP.md locale=ko source_sha256=ca4698088011b7213d7e665a73d15a483aae48056f7a18dd2fc504a92f589ed6 -->
# NeoGraph Harness MCP

**Languages:** [English](HARNESS_MCP.md) | [한국어](HARNESS_MCP.ko.md) | [日本語](HARNESS_MCP.ja.md) | [简体中文](HARNESS_MCP.zh-CN.md)

NeoGraph Harness는 실행 전에 경계가 있는 다중 작업자 워크플로를 컴파일합니다. 안정적인 MCP 표면은 여섯 개의 도구로 유지됩니다:

- `neograph_schema` 는 설치된 요청 계약과 사전 설정을 발견합니다.
- `neograph_compile` 실행 없이 컴파일하고 검증합니다.
- `neograph_start` 보존된 artifact 또는 inline 요청을 시작합니다.
- `neograph_get` 컴팩트 상태를 확인하거나, result artifact URI를 역참조합니다.
- `neograph_resume`는 정확한 pending host 결과를 검증하고 제출합니다.
- `neograph_cancel`는 대기 중, 실행 중, 또는 대기 상태의 워크플로를 협조적으로 취소합니다.

제공된 프리셋은 `fanout_judge`, `pr_review_panel`, `bug_triage`, 및 `research_synthesis`입니다. 프리셋은 일반적인 strict-Core 그래프 아티팩트를 생성합니다. JavaScript 요청은 자체 `ProgramSource` 엔벨로프와 소스 맵을 유지합니다.

### JavaScript 작성 경계

새로운 발행을 위해 `harness.mode`는 `preset` 또는 `javascript`를 허용합니다. JavaScript 요청은 `harness.source`에서 소스 텍스트를 전달하고 `harness.source_id`을 고정할 수 있습니다:

```json
{
  "harness": {
    "mode": "javascript",
    "source_id": "review:main.js",
    "source": "export function define() { const g = ng.graph('main'); /* ... */ return g; }"
  }
}
```

변환기는 그 텍스트를 표준 `ProgramSource` JavaScript 봉투(언어 `javascript`, QuickJS 엔진, 고정 호스트 API, 임포트, 및 소스 맵)로 감싸고 `ProgramCompiler`, `ProgramCatalog`, 그리고 `ProgramRuntime`를 통해 전송합니다. `define()`은 밀봉된 `ng` 바인딩을 통해 하나의 그래프를 구성합니다; 선택적 발전기 `main()`는 일반 제어 흐름을 소유하고 존재하는 입력된 Program 명령을 산출합니다. JavaScript는 Core 노드를 파견하지 않으며, 공급자/도구를 선택하거나, 승인(admission), 예산, 저널, 또는 재생(replay)을 우회하지 않습니다.

평가된 모듈은 또한 결과 계약을 선택합니다. 다음만 내보내는 소스는 `define()` Core 루트 계약을 유지하며, 여기에는 `channels.final_result.value` 래퍼가 포함됩니다. 런타임을 내보내는 소스는 `main(input)` 대신 Harness 결과 스키마에 대해 종단 반환값을 직접 광고하고 검증합니다.

#### 제어 흐름 마이그레이션 예시

유지 `define()` 컴파일 타임과 모든 런타임 효과를 yield된 typed command 뒤에 둡니다. 이 완전한 요청은 생성기에 세 가지 작업(즉, `ng.all` 조인(join)과 그 두 번의 Core 호출) 및 양방향 병렬성을 제공합니다. 그 worker 노드는 요청에서 밀봉(sealed)된 구성을 정확히 반복하며, 터미널 반환 terminal return은 Harness 결과 형태를 가집니다:

```javascript
const source = String.raw`
function workerConfig() {
  return {
    type: "neograph_harness_worker",
    worker_id: "reviewer",
    instructions: "Return structured findings",
    tool_ids: [],
    tool_descriptions: {},
    output_schema: {type: "object", additionalProperties: true},
    provider_timeout_ms: 30000,
    max_output_tokens: 512,
    input_token_ceiling: 16384,
    max_retries: 1,
    max_provider_tool_rounds: 8,
    evidence_required: [],
    read_only: true
  };
}

export function define() {
  const graph = ng.graph("review");
  graph.channel("task", {reducer: "overwrite", initial: {}});
  graph.channel("worker_results", {reducer: "append", initial: []});
  graph.channel("final_result", {reducer: "overwrite", initial: null});
  graph.node("reviewer", workerConfig());
  graph.node("judge", {
    type: "neograph_harness_judge",
    barrier: {wait_for: ["reviewer"]}
  });
  graph.edge("__start__", "reviewer");
  graph.edge("reviewer", "judge");
  graph.edge("judge", "__end__");
  return graph;
}

export function* main(input) {
  const results = yield ng.all([
    ng.callCore("review", {task: input.task}, "review:first"),
    ng.callCore("review", {task: input.task}, "review:second")
  ], {max_in_flight: 2}, "review:all");
  return results[0].channels.final_result.value;
}
`;

const request = {
  task: {
    objective: "Review the change",
    acceptance: ["Return structured, evidence-backed findings"]
  },
  harness: {mode: "javascript", source_id: "review:main.js", source},
  workers: [{
    id: "reviewer",
    instructions: "Return structured findings",
    tools: [],
    output_schema: {type: "object", additionalProperties: true},
    provider_timeout_seconds: 30,
    max_output_tokens: 512
  }],
  tool_catalog: [],
  budgets: {
    max_steps: 40,
    timeout_seconds: 60,
    max_parallel_workers: 2,
    max_program_operations: 3,
    max_worker_retries: 1,
    provider_timeout_seconds: 30,
    max_output_tokens: 512
  },
  policy: {read_only: true, evidence_required: []}
};
```

안정적인 소스 사이트 문자열은 지속적인 명령 좌표의 일부입니다. 재시도와 재시작 전반에서 결정론적으로 유지됩니다. 첫 번째 필수 성공이 우선 적용되어야 할 때 `ng.any(...)`을 사용하고, 첫 번째 터미널 구성원이 먼저 종료되어야 할 때 `ng.race(...)`를 사용합니다. 둘 다 구조적 동시성을 통해 대기 중인 형제들을 취소합니다. 환경 I/O, 타이머, 동적 로딩, `eval`, 그리고 네이티브 핸들은 계속 사용할 수 없습니다.

`harness.mode`는 명시적이어야 합니다. `dsl`는 `H_MIGRATION_CORE_DSL`를 반환하고, `core`는 `H_MIGRATION_CORE_JSON`를 반환하며, `program`/`program_json`는 `H_MIGRATION_PROGRAM_JSON`를 반환합니다. 이 모두는 `/harness/mode`를 가리키며 요청의 JSON 형태나 누락된 필드에서 선택되지 않습니다. 엄격한 Core JSON은 검증된 Core 및 Program 아티팩트를 위한 내부/교환 표현으로 남아 있으며, 신뢰할 수 있는 C++ 인프로세스 구성도 계속 지원됩니다; 둘 다 공개 Harness 작성 언어가 아닙니다.

스키마 내보내기, 컴파일, 시작은 이제 동일한 불변 `HarnessAdmissionProfile`를 사용합니다. 그 범위가 지정된 `GraphRegistry`와 매니페스트는 구현, 하향 변환, 호환성 메타데이터와 함께 모든 사용 가능한 노드, 리듀서, 조건을 나열합니다. 프로세스 전역 레지스트리 항목은 이 팔레트에 포함되지 않으며 Harness 승인(admission)으로 해석될 수 없습니다. 컴파일은 검증된 선언적 토폴로지의 `TopologySpec`에서 중단되므로, 거부된 입력 구성은 `GraphNode` 를 생성하지 않고 작업자나 효과를 디스패치하지 않습니다. 보존된 아티팩트는 프로필 ID와 지문을 바인딩합니다; 다른 또는 프로필 이전의 아티팩트는 재해석되는 대신 시작/재개(resume) 시 실패 시 차단됩니다.

C++ 임베더는 생성 시 `HarnessServiceResources` 를 통해 비기본 프로필을 전달합니다. 이 추가적 리소스 경계는 기존 `HarnessServiceConfig` 레이아웃을 유지합니다. 프로필 지문은 매니페스트와 scoped registry의 내보내진 의미적 전사 정보를 포괄합니다. 각각의 `implementation_identity` 는 신뢰된 선언으로, 해당 callable 동작 동작이 변경될 때마다 변경되어야 합니다.

이것은 현재 Program 기반 Harness 호환성 어댑터입니다. 허용된 Harness 요청은 여전히 레거시 `ProgramSource`로 번역되고, `ProgramCompiler`로 컴파일되며, `ProgramCatalog`로 승인되고, `ProgramRuntime`로 실행됩니다; `GraphEngine`가 유일한 노드 실행기로 유지됩니다.

일반 작성에 대해 허용된 대체는 임베디드 QuickJS에서 표준 JavaScript입니다. 이전의 `dsl`, 독립형 `core`, `program` 모드는 명시적 마이그레이션 진단과 함께 새 게시에 대해 거부됩니다; strict Core JSON은 내부/교환 데이터로 남습니다. [`QUICKJS_CONTROL_ARCHITECTURE.md`](QUICKJS_CONTROL_ARCHITECTURE.md) 및 [`QUICKJS_CONTROL_MIGRATION.md`](QUICKJS_CONTROL_MIGRATION.md)를 참조하세요. 이 문서는 유지된 호환성 동작과 마이그레이션 진단을 설명하며, 새 레거시 소스 의미를 허용하지 않습니다.

## 빌드 및 실행

OpenAI 호환 제공자 어댑터를 사용하여 로컬 stdio 서버를 빌드하세요:

```bash
cmake -S . -B build-harness \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_LLM=ON \
  -DNEOGRAPH_BUILD_MCP_SERVER=ON
cmake --build build-harness --target example_harness_mcp_server -j
export OPENAI_API_KEY=your-key
export NEOGRAPH_HARNESS_MODEL=gpt-4o-mini
```

`NEOGRAPH_HARNESS_API_KEY`는 `OPENAI_API_KEY`보다 우선합니다. `NEOGRAPH_HARNESS_BASE_URL`는 OpenAI 호환 엔드포인트를 선택합니다. 서버는 `https://openrouter.ai/api`와 같은 버전 미지정 기본 형식과 `https://openrouter.ai/api/v1`와 같은 공급자의 문서화된 버전 지정 형식을 모두 허용하며, `/v1`가 누락된 경우에만 추가합니다. 서버는 프로토콜 메시지만 stdout에, 진단 정보만 stderr에 기록합니다. 현재 엔드포인트 형식은 [OpenRouter quickstart](https://openrouter.ai/docs/quickstart)를 참조하십시오.

호스트 상호 운용 스모크 테스트에만 `NEOGRAPH_HARNESS_SMOKE=1`를 설정하세요. 이 명시적 모드는 유효한 무발견 검토를 반환하는 결정론적 프로세스 내 공급자를 사용하며, API 키가 필요 없고, LLM 품질 테스트로 사용해서는 안 됩니다.

내구성 있는 호스트 중개 호출은 레코드와 체크포인트 지속성 모두를 요구합니다. 예제는 하나의 명시적 디렉터리로 두 가지를 모두 활성화합니다:

```bash
export NEOGRAPH_HARNESS_STATE_DIR="$PWD/.neograph-harness-state"
```

이 저장소는 `runs.db`에 불변 아티팩트, 가변 실행 기록, 추가 전용 인과 저널을 저장하고, `checkpoints.db`에 그래프 체크포인트를 저장합니다. 저널 행과 모든 Harness 생성 체크포인트는 실행을 해당 불변 아티팩트, 컴파일된 리비전 다이제스트, MCP 프로토콜 버전 및 Harness 프로필에 바인딩합니다. 작업자 시도에는 기간, 검증/재시도 결과, 그리고 제공자, 기능 및 호스트 중개 호출을 발행 시도에 연결하는 상관 ID가 포함됩니다. 두 SQLite 저장소 모두 WAL 모드와 제한된 busy 타임아웃을 사용합니다. 기존 버전-1 레코드 데이터베이스는 열 때 트랜잭션 방식으로 버전 3으로 마이그레이션됩니다. 디렉터리는 서버 재시작 후에도 유지됩니다. `host_brokered` 카탈로그 항목은 두 저장소 중 하나가 없을 때 컴파일 타임에 거부되므로, 워크플로는 실제로 가지지 않은 재개 가능성을 광고할 수 없습니다.

사용자 정의 임베딩은 선택적 `SqliteHarnessRecordStore` 대상의 `neograph::mcp_sqlite` 를 통해 동일한 백엔드를 구성할 수 있습니다. 기본 저널 모드는 SQLite가 이를 확인하기 전에 일반적인 비밀 및 콘텐츠 필드를 `[REDACTED]` 로 재귀적으로 대체합니다. `METADATA_ONLY` 는 모든 이벤트 페이로드를 폐기합니다. `FULL` 는 제공자 콘텐츠, 도구 인수, 결과를 정확히 보존하며 저장용으로 승인된 데이터에만 활성화해야 합니다. 이벤트는 `HarnessJournal::list_events(run_id, after_sequence, limit)`를 통해 실행 순서로 읽을 수 있습니다. `FileHarnessRecordStore`는 원자적 JSON 파일을 선호하는 배포 환경에서 계속 사용할 수 있으며, 저널 경계를 구현하지 않습니다.

### 보존(Retention)

The SQLite 스토어는 선택적 `HarnessRetentionStore` 형제 인터페이스를 구현하며, 안정적인 `HarnessRecordStore` vtable은 변경되지 않습니다. 아티팩트를 보유하거나 실행을 시작하기 전에, `HarnessService` 은 `max_artifacts` 및 `max_runs` 를 `HarnessServiceConfig`에서 적용합니다. 기본값은 각각 128입니다.

정리(Cleanup)는 종료된 리프 실행(terminal leaf run)만 제거합니다. 대기 중, 실행 중, 입력 대기 중인 실행은 보호되며, 저널 최종화를 완료하지 않은 진행 중인 실행도 보호됩니다. 재생(replay) 또는 포크 행은 `source_run_id`를 기록하므로 해당 종속 항목이 유지되는 동안 소스를 제거할 수 없습니다. 때문에 공간이 필요 Support Needed, 종속 리프가 먼저 제거됩니다. 소스는 Save 됩니다 retained); 보존된 행이 더 이상 이를 참조하지 않을 때 이후 단계에서 only then_eligibility. 따라서 모든 후보가 활성 상태이거나 명시적으로 보호된 상태이거나 여전히 참조되고 있는경우 제한은 소프트(soft)합니다.

`runs.db`에서는 하나의 트랜잭션이 실행 행보다 먼저 해당 실행의 저널 행을 삭제하고, 어떤 실행도 참조하지 않는 아티팩트만 삭제합니다. 이 커밋이 끝나면 Harness가 별도로 구성된 체크포인트 Store에서 삭제된 실행의 체크포인트 스레드를 제거합니다. 두 번째 단계에서 충돌하거나 체크포인트 백엔드가 실패하면 도달 불가능한 체크포인트 저장소가 남을 수 있지만, 보존된 재생(replay)이나 포크가 삭제된 소스 레코드를 가리키는 상태는 생기지 않습니다. 이후 관리 작업이나 백엔드별 고아 정리 작업으로 이러한 체크포인트 전용 잔여물을 회수할 수 있습니다.

`FileHarnessRecordStore`는 영구 정리(durable cleanup)를 구현하지 않으며, 기존의 메모리 내 아티팩트 캐시 제거 및 하드 실행 용량 동작은 그대로 유지됩니다.

## 디버거 뷰

`neograph_get`는 `status`를 기본값으로 유지하면서 MCP 도구를 추가하지 않고 디버거 뷰 네 가지를 추가합니다:

| 뷰 | 결과 |
|---|---|
| `attempts` | 저널링된 작업자 시도 시작/완료/중단 이벤트 |
| `trace` | 기존의 정렬된 GraphEngine 노드 추적 및 인과 저널 타임라인 |
| `checkpoints` | 페이로드가 없는 체크포인트 메타데이터: ID, 상위, 노드, 단계, 스텝 및 채널 이름 |
| `diff` | 각 체크포인트와 해당 상위 체크포인트 간에 변경된 채널 값 및 버전 |

`attempts` 및 `trace`는 `after_sequence`를 불투명한 전방 커서로 수용합니다. 네 뷰 모두 `limit` 값을 1부터 1000까지 허용합니다. 반환된 아티팩트 URI는 쿼리와 동일한 페이지네이션을 포함할 수 있습니다. 예를 들면:

```text
neograph://runs/run_123/attempts?after_sequence=17&limit=50
```

이 URI에는 `after_sequence` 및 `limit`만 허용됩니다. 알 수 없거나 잘못된 쿼리 필드는 무시되지 않고 실패 처리가 됩니다. 저널 백업 뷰는 저장된 페이로드를 그대로 반환하므로, 구성된 편집 모드가 보존됩니다. `diff` 뷰는 저널이 아닌 체크포인트 저장소에서 계산되며 전체 채널 값을포함할 수 있습니다. 이에 대한 접근은 기존의 상세 실행 결과에 대한 접근과 동일하게 취급하십시오.

## 재생 모드

`neograph_start`는 MCP 도구를 추가하지 않고 완료된 실행을 재생(replay)할 수 있습니다:

```json
{"replay":{"source_run_id":"run_123","mode":"recorded"}}
```

`recorded`는 소스 저널의 완료된 worker-attempt 결과와 함께 컴파일러 잠금 그래프를 재실행합니다. 구성된 worker, provider, MCP, A2A 또는 capability 실행기를 호출하지 않습니다. 소스 아티팩트 리비전, 프로토콜 및 프로필은 여전히 일치해야 하며, 저널은 `FULL` 페이로드 모드를 사용해야 합니다. `REDACTED` 및 `METADATA_ONLY` 저널은 정확한 worker 출력을 보존하지 않기 때문에 의도적으로 재생(replay)할 수 없습니다. 중단된 시도는 폐기되며, 재개 후 완료된 worker 호출이 재생(replay)됩니다.

`mode: "live"`를 사용하여 동일한 유지된 아티팩트를 라이브 공급자 및 도구로 실행하십시오. 스냅샷 및 저널 수명 주기 이벤트는 실행을 `recorded_replay` 또는 `live_replay`로 표시하고 `source_run_id`를 포함하며, 일반 시작은 `live`로 유지됩니다.

## 호환 포크

먼저 수리된 Harness를 컴파일한 다음, 기존 `neograph_start` 도구를 통해 정확한 선행 체크포인트를 해당 대상 아티팩트로 분기(branch)합니다:

```json
{
  "fork": {
    "source_run_id": "run_123",
    "checkpoint_id": "550e8400-e29b-41d4-a716-446655440000",
    "artifact_id": "artifact_repaired"
  }
}
```

소스 체크포인트는 `source_run_id`에 속해야 합니다. 실행(run)을 할당하기 전에 Harness는 체크포인트 스키마, 소스 리비전, MCP 프로토콜, Harness 프로필, 복원된 모든 채널과 리듀서, 모든 연속 노드, 그리고 활성 배리어 인터페이스를 대상 아티팩트에 대해 검증합니다. 호환되지 않는 분기는 `started: false`, `status: "incompatible_fork"` 및 기계 판독 가능한 `H_FORK_*` 진단을 `path` 및 `witness`와 함께 반환합니다. 이 경우 실행(run)이나 포크 체크포인트가 생성되지 않습니다.

체크포인트 저장소가 필요합니다. 레코드 저장소가 없으면 포크는 현재 서비스 프로세스에 상주하는 소스 실행(run) 및 아티팩트만 참조할 수 있습니다. 재시작 후에도 유지되어야 하는 포크 계보(lineage)를 위해 두 저장소를 모두 구성하세요.

호환 가능한 브랜치는 `compatible_fork`로 표시되며 시작 응답, 스냅샷, 수명 주기 저널 이벤트 내 `source_run_id` 및 `source_checkpoint_id`를 모두 포함합니다. 실행은 선택된 체크포인트의 `next_nodes`에서 재개되며, 이미 커밋된 선행자들은 다시 실행되지 않습니다. 대상 아티팩트는 수정된 토폴로지, 작업자 계약 및 도구 카탈로그를 제공하는 반면, 소스 체크포인트에서 채널 복원 값(원래 작업 채널 포함)이 제공됩니다. 작업 입력 자체가 변경되어야 할 때는 포크 대신 새 시작을 사용하십시오.

소스 실행, 아티팩트 및 선택된 체크포인트는 포크의 참조이며, 호환성 검사나 포크 실행이 참조를 사용할 수 있는 동안 유지되어야 합니다. 보존 정리는 종속 항목을 먼저 제거하거나 참조된 소스를 유지해야 하며, 사전 점검과 브랜치 생성 사이에 소스 체크포인트를 절대 삭제하지 않아야 합니다.

## 스트리밍 가능한 HTTP

원격 전송은 옵트인 방식이므로 기존 stdio 전용 대상은 작게 유지되며 HTTP/OpenSSL 의존성이 조용히 추가되지 않습니다:

```bash
cmake -S . -B build-harness-http \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_EXAMPLES=OFF \
  -DNEOGRAPH_BUILD_LLM=ON \
  -DNEOGRAPH_BUILD_MCP_SERVER=ON \
  -DNEOGRAPH_BUILD_MCP_HTTP_SERVER=ON \
  -DNEOGRAPH_BUILD_HARNESS_MCP_BINARY=ON
cmake --build build-harness-http --target neograph_harness_mcp -j
cmake --install build-harness-http --prefix "$HOME/.local"

export NEOGRAPH_HARNESS_TRANSPORT=http
export NEOGRAPH_HARNESS_HTTP_HOST=127.0.0.1
export NEOGRAPH_HARNESS_HTTP_PORT=8080
"$HOME/.local/bin/neograph-harness-mcp"
```

엔드포인트는 `http://127.0.0.1:8080/mcp`입니다. 세션별 MCP 수명 주기와 JSON 응답을 갖춘 공개된 MCP 2025-11-25 Streamable HTTP POST 계약을 구현합니다. 알림은 HTTP 202를 반환합니다. DELETE는 세션을 종료합니다. 선택적 독립형 GET/SSE 채널은 의도적으로 구현되지 않으며 HTTP 405를 반환하는데, 이는 전송 사양에서 명시적으로 허용합니다.

보안 기본값은 전송 수준이며 인증을 `GraphEngine` 또는 `HarnessService`에 결합하지 않습니다:

- 기본 바인드는 `127.0.0.1`입니다. 비루프백 바인드는 bearer 승인자가 구성되지 않는 한 거부됩니다.
- 제공된 모든 `Origin`는 `NEOGRAPH_HARNESS_ALLOWED_ORIGINS`(실행 파일에서 쉼표로 구분됨)의 항목과 정확히 일치하지 않는 한 거부됩니다.
- `NEOGRAPH_HARNESS_BEARER_TOKEN`는 실행 파일의 단일-주체 bearer 경계를 활성화합니다. 라이브러리 임베딩은 OAuth/JWT 검증을 위해 `MCPHttpServerConfig::bearer_authorizer`를 사용하고 안정적인 주체/범위를 반환할 수 있습니다.
- 세션은 반환된 권한 부여 범위에 바인딩됩니다. 다른 유효한 주체는 유출된 `Mcp-Session-Id`를 재사용할 수 없습니다.
- The `MCPHttpServer` 팩토리는 검증된 범위를 수신하고 `MCPHttpServerSession` 소유자를 반환합니다. 멀티테넌트 임베딩은 범위를 사용하여 격리된 Harness 레코드/체크포인트 저장소를 선택해야 합니다. 인증 상태는 그래프 런타임 자체에 들어가지 않습니다.
- 요청 페이로드, HTTP 작업자, 큐, 세션 및 응답 대기 한도는 `MCPHttpServerConfig`에 의해 제한됩니다.

모든 non-loopback 배포의 경우, 신뢰할 수 있는 역방향 프록시에서 TLS를 종료하고 해당 프록시의 OAuth/OIDC 검증 또는 동등한 `bearer_authorizer`를 사용하십시오. 원래 `Authorization` 및 `Origin` 헤더를 전달하고, 평문 공개 리스너를 노출하지 말며, Harness 상태 디렉터리마다 하나의 인증 도메인을 배포하십시오.

## 호스트 설정

`SERVER`에 절대 경로를 사용하십시오:

```bash
SERVER=/absolute/path/to/build-harness/example_harness_mcp_server
```

Claude Code, 로컬 프로젝트 범위:

```bash
claude mcp add --scope local --transport stdio neograph-harness -- "$SERVER"
claude mcp get neograph-harness
```

Codex CLI:

```bash
codex mcp add neograph-harness -- "$SERVER"
codex mcp list
```

비대화형 `codex exec` 이 신뢰된 로컬 서버에 대해, `mcp_servers.neograph-harness.default_tools_approval_mode = "approve"` Codex `config.toml`에 설정하십시오. 그것 없이, Codex는 이를 올바르게 취소합니다. `neograph_compile` 왜냐하면 artifact를 보존하는 것이 읽기 전용으로 주석 처리되지 않기 때문입니다. 대화형 세션은 기본 프롬프트그대로 유지할 수 있습니다.

OpenCode, 프로젝트 `opencode.json` 또는 사용자 구성에서:

```json
{
  "$schema": "https://opencode.ai/config.json",
  "mcp": {
    "neograph-harness": {
      "type": "local",
      "command": ["/absolute/path/to/example_harness_mcp_server"],
      "enabled": true,
      "environment": {
        "OPENAI_API_KEY": "{env:OPENAI_API_KEY}",
        "NEOGRAPH_HARNESS_MODEL": "gpt-4o-mini"
      }
    }
  }
}
```

`opencode mcp list`로 검증하십시오. 이러한 형식들은 2026-07-21에 검토된 각 호스트의 공식 MCP 구성 계약을 따릅니다.

## PR 검토 워크플로우

호스트에게 일반 저장소 도구로 PR diff를 수집하도록 요청한 다음 Harness 도구를 사용하십시오. 적합한 요청은 다음과 같습니다:

```json
{
  "task": {
    "objective": "Review this PR diff. Report only actionable correctness, security, or regression findings. Include the diff after this sentence.",
    "acceptance": [
      "Every finding identifies a file and line",
      "Every finding quotes concrete evidence",
      "Return an empty findings array when no issue is proven"
    ]
  },
  "harness": {"mode": "preset", "preset": "pr_review_panel"},
  "workers": [
    {
      "id": "correctness",
      "instructions": "Review behavior, edge cases, and regressions.",
      "tools": [],
      "output_schema": {
        "type": "object",
        "required": ["status", "findings"],
        "properties": {
          "status": {"enum": ["ok", "partial", "failed"]},
          "findings": {
            "type": "array",
            "items": {
              "type": "object",
              "required": ["file", "line", "evidence", "message"],
              "properties": {
                "file": {"type": "string"},
                "line": {"type": "integer"},
                "evidence": {"type": "string"},
                "message": {"type": "string"}
              },
              "additionalProperties": false
            }
          }
        },
        "additionalProperties": false
      }
    },
    {
      "id": "security",
      "instructions": "Review trust boundaries, validation, and unsafe side effects.",
      "tools": [],
      "output_schema": {
        "type": "object",
        "required": ["status", "findings"],
        "properties": {
          "status": {"enum": ["ok", "partial", "failed"]},
          "findings": {
            "type": "array",
            "items": {
              "type": "object",
              "required": ["file", "line", "evidence", "message"],
              "properties": {
                "file": {"type": "string"},
                "line": {"type": "integer"},
                "evidence": {"type": "string"},
                "message": {"type": "string"}
              },
              "additionalProperties": false
            }
          }
        },
        "additionalProperties": false
      }
    }
  ],
  "tool_catalog": [],
  "budgets": {
    "max_steps": 10,
    "timeout_seconds": 600,
    "max_parallel_workers": 2,
    "max_worker_retries": 1,
    "provider_timeout_seconds": 60,
    "max_output_tokens": 4096
  },
  "policy": {
    "read_only": true,
    "evidence_required": ["file", "line", "evidence"]
  }
}
```

### 공급자 예산

`budgets.provider_timeout_seconds`는 한 번의 공급자 완료 시도를 1~600초로 제한합니다. `budgets.max_output_tokens`는 한 번의 완료를 1~128000개의 생성된 토큰으로 제한합니다. 둘 다 선택 사항입니다: 하나를 생략하면 이전 동작이 유지되어 Harness 기한이 없고 공급자의 기존 출력 한도가 적용됩니다.

작업자는 두 필드 중 하나를 더 작은 값으로 설정할 수 있습니다. Harness 전체 값보다 높은 작업자 값은 컴파일 시간에 거부됩니다. 데드라인이 되면 Harness는 해당 공급자 호출에 제공된 하위 취소 토큰만 취소합니다. 형제 작업자나 포함하는 실행은 취소되지 않습니다. 공급자는 토큰을 존중해야 하므로, 중단될 수 없는 공급자는 데드라인 이후에 반환될 수 있습니다.

호스트는 다음 순서를 따라야 합니다:

1. `neograph_compile`를 호출하고 `ok`이(가) 거짓이면 중지합니다.
2. `neograph_start`를 반환된 `artifact_id`로 호출합니다.
3. `neograph_get`를 `run_id`와 함께 폴링하십시오; 이는 결과와 개수만 반환합니다.
4. 세부 정보가 필요하면 호출하십시오. `neograph_get` 동일한 `run_id` 및 반환된 `neograph://runs/...` URI를 `uri`로 사용하십시오. 기본적으로 트레이스를 컨텍스트로 가져오지 마십시오.

### 출처 찾기(Provenance)

details 아티팩트는 스키마 검증을 거친 각 작업자 응답을 `workers`에 보존하며, 기존 클라이언트를 위해 기존의 플랫한 `findings` 배열을 유지합니다. `finding_sources`는 동일한 길이의 병렬 배열입니다. 각 항목에는 집계된 `finding_index`, 소스 `worker_id` 및 해당 작업자의 `local_index`가 포함됩니다. 이를 사용하여 `F1`와 같은 중복 로컬 ID의 출처를 식별하십시오. 작업자의 선언된 파인딩 객체에 출처 필드를 추가하지 마십시오.

## 호스트 중개 재개(Host-Brokered Resume)

MCP 호스트가 작업자 프로세스가 아닌 기능을 소유할 때는 `executor.kind: "host_brokered"`을 사용하십시오. `executor.interaction`을 `"tool_result"`(기본값) 또는 `"input"`으로 설정하십시오. 공급자 실행기는 요청된 인수를 검증하고 두 가지 비종료 실행 상태 중 하나를 반환합니다:

- `awaiting_tool_results`: 호스트는 명명된 기능을 실행해야 합니다.
- : `input_required`: 호스트는 입력 값을 수집해야 합니다.

`neograph_get`에는 `pending` 객체가 포함되며, 이 객체는 고유한 `call_id`, `tool_id`, 검증된 `arguments`, `result_schema`를 담습니다. 다음 경로를 통해 바로 그 호출을 제출하십시오:

```json
{
  "run_id": "run_...",
  "call_id": "hcall_...",
  "result": {"answer": "validated host result"}
}
```

`neograph_resume`은 불일치한 호출 ID, 선언된 스키마를 위반하는 결과, 만료된 호출, 및 대기 중이 아닌 런을 위한 늦은 결과를 거부합니다. 동일한 중복은 그래프를 재실행하지 않고 승인되며. 충돌하는 중복은 거부됩니다. 승인된 재개 의도는 실행이 예약되기 전에 영속화되므로, 프로세스 충돌 후 폴링은 성공한 형제 작업자를 반복하지 않고 `NodeInterrupt` 체크포인트에서 재개를 재시작합니다.

### 외부 효과 및 조정

일반 호스트-중개 계약은 하위 호환됩니다: `executor.effect`가 없는 카탈로그 항목은 프로세스 재시작 후에도 `awaiting_tool_results`로 유지되며 동일한 `{run_id, call_id, result}` 재개 요청을 수락합니다.

외부에서 관찰 가능하고 비멱등적(non-idempotent) 변경을 만들 수 있는 호스트 기능의 경우 해당 위험을 명시적으로 선언하십시오. 효과 메타데이터는 기본 `host_brokered` `tool_result` 상호 작용에서만 유효하며, 입력 수집 메타데이터가 아닙니다.

```json
{
  "executor": {
    "kind": "host_brokered",
    "effect": {
      "idempotency": "unsupported",
      "status_query": true,
      "fencing": true
    }
  }
}
```

보류 중인 호출에는 내구성 있는 `effect` 객체가 포함됩니다. 해당 객체의 `effect_id` 및 `idempotency_key` 는 Harness 실행에 범위가 지정되며 제공자의 도구 호출 ID와 다릅니다. `status_query` 및 `fencing` 는 호스트 기능을 설명하며, Harness는 이를 기록하지만 제공자별 조회 또는 재시도 프로토콜을 임의로 만들지 않습니다.

서비스가 재연결되는 동안 `idempotency: "unsupported"` 호출이 여전히 대기 중이면, 해당 실행만 `ambiguous_effect`로 변경됩니다. 이는 호스트가 프로세스가 중지되기 전에 효과를 수행했을 수 있지만, Harness는 어느 결과도 증명할 수 없음을 의미합니다. 압축 상태에는 `pending` 및 `ambiguity`가 포함되고, 저널은 `host_brokered.effect.ambiguous`를 기록하며, Harness는 도구를 재생(replay)하지도 않고 효과를 실패 또는 완료로 보고하지도 않습니다.

호스트가 자체 권위 시스템을 확인한 후 `neograph_resume`을 통해 모호성을 해결합니다:

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"completed","result":{"answer":"validated host result"}}
```

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"failed"}
```

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"unknown"}
```

`completed`는 `result`를 검증하고 소비한 후 체크포인트에서 재개합니다. `failed`는 작업자를 다시 실행하지 않고 터미널 Harness 실패를 기록합니다. `unknown`는 실행을 추후 조정을 위해 `ambiguous_effect`으로 남겨둡니다. 정확한 중복 완료, 실패 또는 알 수 없는 제출은 멱등적입니다. 충돌하는 완료 또는 실패 제출은 거부됩니다. 모든 비중복 조정은 `host_brokered.effect.reconciled`으로 저널링됩니다.

모호한 효과는 의도적으로 취소할 수 없으며 만료되지 않습니다. 취소나 타임아웃은 외부 효과가 발생했는지 여부를 확정할 수 없습니다. 신뢰할 수 있는 시스템이 아직 이를 해결할 수 없다면 `unknown`를 제출하십시오.

이 프로토콜은 호스트 크래시를 넘어서는 exactly-once delivery(정확히 한 번 전달)를 보장하지 않습니다. Idempotency keys(멱등 키) 또는 status query(상태 조회)를 지원하는 호스트는 이러한 시스템을 사용하여 reconciliation(조정)을 제출하기 전에 실제 결과를 판정해야 합니다.

Run snapshots(스냅숏)은 `created_at`, `updated_at`, `expires_at`, 그리고 `poll_after_ms`를 포함합니다. Default TTL은 24시간, default polling interval은 1초이며, embeddings(임베딩(들)은 `HarnessServiceConfig`을 통해 둘 다 override할 수 있습니다.

## Experimental Tasks Profile(실험적 Tasks Profile)

MCP Tasks는 핵심 MCP 2025-11-25의 일부가 아니며, 업스트림 확장은 여전히 실험적이라고 표시합니다. 따라서 NeoGraph는 기본적으로 비활성화 상태로 유지하며, 안정적인 `run_id` 및 `neograph_get` 폴링 계약과 분리합니다.

예제 서버에서 옵트인하려면 내구성 상태도 활성화해야 합니다:

```bash
export NEOGRAPH_HARNESS_STATE_DIR="$PWD/.neograph-harness-state"
export NEOGRAPH_HARNESS_EXPERIMENTAL_TASKS=1
```

그런 다음 서버는 `io.modelcontextprotocol/tasks`을(를) 광고하고, `neograph_start` 에 선택적 작업 지원을 표시하며, 다음 항목을 제공합니다: `tasks/get`, `tasks/update`, 및 `tasks/cancel`. 서버는 개별 `CreateTaskResult` 요청이 다음을 포함할 때만 `tools/call`를 반환합니다:

```json
{
  "_meta": {
    "io.modelcontextprotocol/clientCapabilities": {
      "extensions": {"io.modelcontextprotocol/tasks": {}}
    }
  }
}
```

해당 요청 옵트인(opt-in)이 없는 클라이언트는 일반적인 `CallToolResult`를 수신하고 `neograph_get` 폴링을 계속합니다. 프로필을 활성화해도 안정적인 폴백은 변경되지 않습니다. 작업 상태는 `working`, `input_required`, `completed`, `failed`, `cancelled`입니다. `tasks/update.inputResponses`는 대기 중인 `call_id`를 기준으로 키가 지정되며, 폴링 클라이언트는 `pollIntervalMs`와 `ttlMs`를 준수해야 합니다.

## 기능 백엔드(Capability Backends)

`make_provider_harness_executor`는 모든 NeoGraph `Provider`를 통해 워커를 구동합니다. 모델이 선언된 도구를 요청하면 실행기는 디스패치 전후에 인수와 출력을 카탈로그에 대해 검증합니다.

`make_mcp_harness_capability_executor` 초기화된 다운스트림 `MCPClient` 인스턴스에 사용하거나, `a2a::make_harness_capability_executor` A2A 에이전트에 사용합니다. 요청이 권위를 유지합니다: 작업자는 자신의 `tools` 배열에 나열된 도구 ID만 볼 수 있습니다.

파일시스템 도구의 경우, 모든 경로를 포함하는 입력을 `path_arguments`에 선언하고 `policy.workspace_roots`를 설정하십시오. 상대 경로는 첫 번째 루트 아래에서 해석되며, 기존 심볼릭 링크를 통한 탈출을 포함하여 구성된 모든 루트 외부의 정규 경로는 디스패치 전에 거부됩니다. 정규 경로는 모델이 제공한 표기 대신 기능 백엔드에 전달됩니다. 다운스트림 MCP 및 A2A 서비스는 별도의 신뢰 경계로 유지되며, 파일시스템 TOCTOU(시간 검사/시간 사용) 경합을 차단하기 위해 동일한 루트 정책을 적용해야 합니다. `policy.read_only: true`을 사용하면 컴파일이 `read_only: true`로 표시되지 않은 모든 카탈로그 항목을 거부합니다.

## 배포 및 프로토콜 프로필

지원되는 로컬 배포 경로는 위의 설치 가능한 `neograph-harness-mcp` 바이너리입니다. 소스 빌드는 예제 대상을 계속 사용할 수 있으며, Python 휠은 원격 데몬을 암시적으로 설치하는 대신 라이브러리/런타임 패키지로 유지됩니다. MCPB 및 공식 레지스트리 게시는 릴리스/발견 패키징 옵션으로 남아 있습니다. 이는 와이어 프로토콜에 필요하지 않으며, 서명된 릴리스 아티팩트와 명시적인 원격 인증 배포 매니페스트가 있는 경우에만 추가해야 합니다.

NeoGraph는 현재 날짜가 표시된 MCP `2025-11-25` 프로필만 게시합니다. 향후 무상태 프로토콜을 설명하는 최종 SEP는 새로운 와이어 버전을 생성하지 않습니다. MCP 프로젝트가 새로운 날짜 지정 사양을 게시할 때까지 후속 프로필은 광고되지 않습니다.
