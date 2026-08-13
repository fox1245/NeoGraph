<!-- neograph-i18n: source=docs/HARNESS_MCP.md locale=ko source_sha256=b09a63e8a367d734aa3e9a1be015fcef1bc3d3b9fe472e138e040ed6ce0f53f9 -->
**Languages:** [English](HARNESS_MCP.md) | [한국어](HARNESS_MCP.ko.md) | [日本語](HARNESS_MCP.ja.md) | [简体中文](HARNESS_MCP.zh-CN.md)

# 네오그래프 하네스 MCP


NeoGraph Harness는 실행되기 전에 제한된 다중 작업자 워크플로우를 컴파일합니다. 그만큼
안정적인 MCP 표면은 6개의 도구에 유지됩니다.

- `neograph_schema`는 설치된 요청 계약 및 사전 설정을 검색합니다.
- `neograph_compile`는 실행하지 않고 정교화, 컴파일 및 검증합니다.
- `neograph_start`는 보유된 아티팩트 또는 인라인 요청을 시작합니다.
- `neograph_get`는 압축 상태를 폴링하거나 결과 아티팩트 URI를 역참조합니다.
- `neograph_resume`는 정확한 보류 중인 호스트 결과를 검증하고 제출합니다.
- `neograph_cancel`는 대기열에 있거나 실행 중이거나 대기 중인 워크플로를 협력적으로 취소합니다.

제공되는 프리셋은 `fanout_judge`, `pr_review_panel`, `bug_triage`,
`research_synthesis`입니다. 프리셋은 strict-Core 그래프 아티팩트를 만들고,
JavaScript 요청은 자체 `ProgramSource` 봉투와 소스 맵을 보존합니다.

### JavaScript 저작 경계

새 발행에서 `harness.mode`는 `preset` 또는 `javascript`만 허용합니다.
JavaScript 소스는 `harness.source`에 넣고, `harness.source_id`로 소스 ID를
고정할 수 있습니다. `define()`은 봉인된 `ng` 바인딩으로 그래프 하나를
구성하며, 선택적인 제너레이터 `main()`은 일반 JavaScript 반복과 분기를
수행하고 `ng.callCore`, `ng.all`, `ng.any`, `ng.race` 같은 타입 명령만
yield합니다.

```json
{
  "harness": {
    "mode": "javascript",
    "source_id": "review:main.js",
    "source": "export function define() { const g = ng.graph('main'); /* ... */ return g; }"
  }
}
```

#### 제어 흐름 마이그레이션 예시

`define()`은 컴파일 시점에만 실행하고, 모든 런타임 효과는 yield한 타입 명령
뒤에 둡니다. 이 완전한 요청은 제너레이터에 연산 2개와 병렬도 2의 상한을
부여합니다. 워커 노드는 요청에서 호스트가 봉인한 구성과 정확히 일치하며,
터미널 반환값은 Harness 결과 형태입니다.

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
    max_program_operations: 2,
    max_worker_retries: 1,
    provider_timeout_seconds: 30,
    max_output_tokens: 512
  },
  policy: {read_only: true, evidence_required: []}
};
```

안정적인 소스 위치 문자열은 내구성 명령 좌표의 일부입니다. 재시도와 재시작
사이에서 결정적으로 유지하십시오. `ng.any(...)`는 필요한 성공 수가 먼저
충족될 때, `ng.race(...)`는 첫 번째 터미널 멤버가 이길 때 사용합니다. 둘 다
구조적 동시성을 통해 남은 형제를 취소합니다. 주변 I/O, 타이머, 동적 로딩,
`eval`, 네이티브 핸들은 계속 사용할 수 없습니다.

`harness.mode`는 명시해야 합니다. `dsl`은 `H_MIGRATION_CORE_DSL`, `core`는
`H_MIGRATION_CORE_JSON`, `program`/`program_json`은
`H_MIGRATION_PROGRAM_JSON`으로 실패합니다. strict Core JSON과 신뢰된 C++
구성은 내부 표현으로만 남으며 공개 Harness 저작 언어가 아닙니다.

스키마 내보내기, 컴파일, 시작은 동일한 불변
`HarnessAdmissionProfile`을 사용합니다. 범위가 지정된 `GraphRegistry`만
해석되며 프로세스 전역 등록 항목으로 폴백하지 않습니다. 거부된 입력은
노드, 작업자, 효과를 디스패치하지 않습니다.

프리셋과 JavaScript 요청은 모두 `ProgramSource`, `ProgramCompiler`,
`ProgramCatalog`, `ProgramRuntime` 경로를 사용하며 `GraphEngine`이 유일한
노드 실행기입니다. 저장된 레거시 아티팩트는 `translated`, `drain_only`,
`rejected`로 명시 분류됩니다. `drain_only`는 새 발행이나 새 실행을 허용하지
않고 정확히 보존된 레거시 런타임에서 기존 실행만 재개합니다.

## 빌드 및 실행

OpenAI 호환 공급자 어댑터를 사용하여 로컬 stdio 서버를 구축합니다.

```bash
cmake -S . -B build-harness \
  -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_LLM=ON \
  -DNEOGRAPH_BUILD_MCP_SERVER=ON
cmake --build build-harness --target example_harness_mcp_server -j
export OPENAI_API_KEY=your-key
export NEOGRAPH_HARNESS_MODEL=gpt-4o-mini
```

`NEOGRAPH_HARNESS_API_KEY`는 `OPENAI_API_KEY`보다 우선합니다.
`NEOGRAPH_HARNESS_BASE_URL`는 OpenAI 호환 엔드포인트를 선택합니다. 서버
accepts both an unversioned base such as `https://openrouter.ai/api` and the
provider's documented versioned form such as `https://openrouter.ai/api/v1`;
누락된 경우에만 `/v1`를 추가합니다. 서버는 다음에만 프로토콜 메시지를 씁니다.
stdout 및 진단은 stderr에만 적용됩니다. 참조
현재의 [OpenRouter quickstart](https://openrouter.ai/docs/quickstart)
끝점 형식.

호스트 상호 운용성 연기 테스트에 대해서만 `NEOGRAPH_HARNESS_SMOKE=1`를 설정합니다.
해당 명시적 모드는 유효한 값을 반환하는 결정적 프로세스 내 공급자를 사용합니다.
제로 결과 검토, API 키가 필요하지 않으며 LLM로 사용해서는 안 됩니다.
품질 테스트.

내구성 있는 호스트 중개 호출에는 기록과 체크포인트 지속성이 모두 필요합니다.
이 예에서는 하나의 명시적 디렉터리를 사용하여 두 가지를 모두 활성화합니다.

```bash
export NEOGRAPH_HARNESS_STATE_DIR="$PWD/.neograph-harness-state"
```

이는 변경할 수 없는 아티팩트, 변경 가능한 실행 레코드 및 추가 전용 인과 관계를 저장합니다.
`runs.db`의 저널과 `checkpoints.db`의 그래프 체크포인트입니다. 저널 행
하네스에서 생성된 모든 체크포인트는 실행을 불변의 아티팩트에 바인딩합니다.
컴파일된 개정 다이제스트, MCP 프로토콜 버전 및 하네스 프로필. 노동자
시도에는 기간, validation/retry 결과 및 상관 관계 ID가 포함됩니다.
발급 시도에 공급자, 기능 및 호스트 중개 호출을 참여시킵니다.
두 SQLite 저장소 모두 WAL 모드와 제한된 사용 중을 사용합니다.
시간 초과. 기존 버전 1 레코드 데이터베이스는 트랜잭션 방식으로 버전으로 마이그레이션됩니다.
3개 열었을 때. 디렉터리는 서버를 다시 시작해도 유지됩니다. 에이
`host_brokered` 카탈로그 항목은 두 저장소 중 하나가 있는 경우 컴파일 시 거부됩니다.
누락되어 워크플로에 없는 재개 가능성을 광고할 수 없습니다.

커스텀 임베딩은 다음을 통해 동일한 백엔드를 구성할 수 있습니다.
선택적 `neograph::mcp_sqlite` 대상의 `SqliteHarnessRecordStore`.
기본 저널 모드는 공통 비밀 및 콘텐츠 필드를 반복적으로 대체합니다.
SQLite가 보기 전에 `[REDACTED]`를 사용합니다. `METADATA_ONLY`는 모든 이벤트를 삭제합니다.
유효 탑재량; `FULL`는 공급자 콘텐츠, 도구 인수 및 결과를 정확하게 보존합니다.
저장이 승인된 데이터에 대해서만 활성화해야 합니다. 이벤트는 다음에서 읽을 수 있습니다.
`HarnessJournal::list_events(run_id, after_sequence, limit)`를 통해 주문을 실행하세요.
`FileHarnessRecordStore`는 원자성을 선호하는 배포에 계속 사용할 수 있습니다.
JSON 파일; 저널 경계를 구현하지 않습니다.

### 보유

SQLite 저장소는 선택적 `HarnessRetentionStore` 형제를 구현합니다.
인터페이스; 안정적인 `HarnessRecordStore` vtable은 변경되지 않습니다. 보관하기 전에
아티팩트 또는 실행 시작 시 `HarnessService`는 `max_artifacts`를 적용하고
`HarnessServiceConfig`의 `max_runs`. 기본값은 각각 128입니다.

정리는 터미널 리프 실행만 제거합니다. 대기 중인 실행, 실행 중인 실행 및 입력 대기 실행
저널이 완료되지 않은 진행 중인 실행과 마찬가지로 보호됩니다.
마무리. 재생 또는 분기 행은 `source_run_id`를 기록하므로 해당 소스는
해당 종속 항목은 그대로 유지되는 동안 제거됩니다. 공간이 필요한 경우,
종속 리프가 먼저 제거됩니다. 소스는 나중에만 자격을 갖추게 됩니다.
유지된 행이 이를 참조하지 않은 후의 단계입니다. 그러므로 한계는 다음과 같은 경우에 유연합니다.
후보가 활성 상태이거나 명시적으로 보호되거나 여전히 참조되고 있습니다.

`runs.db` 내에서 하나의 트랜잭션은 실행 전에 실행의 저널 행을 삭제합니다.
실행이 참조하지 않은 후에만 아티팩트를 삭제합니다. 커밋한 후,
하네스는 삭제된 실행의 체크포인트 스레드를 별도로 제거합니다.
구성된 체크포인트 저장소. 해당 기간 동안 충돌 또는 체크포인트 백엔드 실패
두 번째 단계에서는 도달할 수 없는 체크포인트 저장소를 남길 수 있지만
삭제된 소스 레코드를 가리키는 replay/fork가 유지되었습니다. 나중에 행정
또는 백엔드별 고아 청소는 이러한 체크포인트 전용 잔여물을 회수할 수 있습니다.

`FileHarnessRecordStore`는 내구성 있는 정리를 구현하지 않습니다. 그 역사적
인메모리 아티팩트 캐시 제거 및 하드 실행 용량 동작은 그대로 유지됩니다.

## 디버거 보기

`neograph_get`는 `status`를 컴팩트 기본값으로 유지하고 4개의 디버거를 추가합니다.
다른 MCP 도구를 추가하지 않고 보기:

|보다|결과|
|---|---|
|`attempts`|저널링된 작업자 시도 start/completion/interruption 이벤트|
|`trace`|기존의 정렬된 GraphEngine 노드 추적과 인과 저널 타임라인|
|`checkpoints`|페이로드 없는 체크포인트 메타데이터: ID, 상위, 노드, 단계, 단계 및 채널 이름|
|`diff`|각 체크포인트와 해당 상위 항목 간에 채널 값 및 버전이 변경되었습니다.|

`attempts` 및 `trace`는 `after_sequence`를 불투명 정방향 커서로 허용합니다. 모두
4개의 보기는 1부터 1000까지의 `limit`를 허용합니다. 반환된 아티팩트 URI는
쿼리와 동일한 페이지 매기기입니다. 예를 들면 다음과 같습니다.

```text
neograph://runs/run_123/attempts?after_sequence=17&limit=50
```

이러한 URI에는 `after_sequence` 및 `limit`만 허용됩니다. 알 수 없거나
잘못된 쿼리 필드는 무시되지 않고 실패합니다. 저널 기반 견해
페이로드를 지속된 그대로 정확하게 반환하므로 구성된 수정 모드는 다음과 같습니다.
보존. `diff` 뷰는 체크포인트 저장소가 아닌 체크포인트 저장소에서 계산됩니다.
저널이며 전체 채널 값을 포함할 수 있습니다. 액세스를 액세스처럼 취급합니다.
기존 세부 실행 결과에 적용됩니다.

## 재생 모드

`neograph_start`는 다른 MCP 도구를 추가하지 않고도 완료된 실행을 재생할 수 있습니다.

```json
{"replay":{"source_run_id":"run_123","mode":"recorded"}}
```

`recorded`는 소스 저널의 컴파일러로 잠긴 그래프를 다시 실행합니다.
작업자 시도 결과가 완료되었습니다. 구성된 작업자를 호출하지 않습니다.
공급자, MCP, A2A 또는 기능 실행자입니다. 소스 아티팩트 개정,
프로토콜과 프로필은 여전히 ​​일치해야 하며 저널은 `FULL` 페이로드를 사용해야 합니다.
방법. `REDACTED` 및 `METADATA_ONLY` 저널은 다음과 같은 이유로 의도적으로 재생할 수 없습니다.
정확한 작업자 출력을 유지하지 않습니다. 중단된 시도는 삭제됩니다.
완료된 재개 후 작업자 호출이 재생됩니다.

`mode: "live"`를 사용하여 라이브 제공자와 동일한 보유 아티팩트를 실행하고
도구. 스냅샷 및 저널 수명 주기 이벤트 레이블은 `recorded_replay` 또는
`live_replay` 및 `source_run_id`를 포함합니다. 일반 시작은 `live`로 유지됩니다.

## 호환 가능한 포크

먼저 수리된 하네스를 컴파일한 다음 정확한 이전 체크포인트를
기존 `neograph_start` 도구를 통해 아티팩트를 대상으로 합니다.

```json
{
  "fork": {
    "source_run_id": "run_123",
    "checkpoint_id": "550e8400-e29b-41d4-a716-446655440000",
    "artifact_id": "artifact_repaired"
  }
}
```

소스 체크포인트는 `source_run_id`에 속해야 합니다. 실행을 할당하기 전에
하네스는 체크포인트 스키마, 소스 개정, MCP 프로토콜,
하네스 프로필, 모든 복원된 채널 및 감속기, 모든 연속 노드,
및 대상 아티팩트에 대한 활성 장벽 인터페이스. 호환되지 않는
분기는 `started: false`, `status: "incompatible_fork"`를 반환하고
`path` 및 `witness`를 사용한 기계 판독 가능한 `H_FORK_*` 진단; 그것은 아니오를 생성합니다
체크포인트를 실행하거나 포크하세요.

체크포인트 상점이 필요합니다. 레코드 저장소가 없으면 포크가 참조할 수 있습니다.
현재 서비스 프로세스에 여전히 상주하는 소스 실행 및 아티팩트만 있습니다.
다시 시작해도 유지되어야 하는 포크 ​​계보에 대해 두 저장소를 모두 구성합니다.

호환 가능한 분기에는 `compatible_fork`라는 라벨이 붙어 있으며 두 가지를 모두 전달합니다.
시작 응답, 스냅샷 및 `source_run_id` 및 `source_checkpoint_id`
라이프사이클 저널 이벤트. 선택한 체크포인트에서 실행이 재개됩니다.
`next_nodes`; 이미 커밋된 선행 작업은 다시 실행되지 않습니다. 목표
아티팩트는 수리된 토폴로지, 작업자 계약 및 도구 카탈로그를 제공합니다.
원래 작업 채널을 포함하여 복원된 채널 값은
소스 체크포인트 작업 입력 시 포크 대신 새로운 시작을 사용하세요.
그 자체가 바뀌어야 합니다.

소스 실행, 아티팩트 및 선택된 체크포인트는 포크의 참조이며
호환성 검사 또는 포크 실행이 사용할 수 있는 동안 유지되어야 합니다.
그들을. 보존 정리는 먼저 종속 항목을 제거하거나 참조된 항목을 보존해야 합니다.
출처; 프리플라이트와 브랜치 사이의 소스 체크포인트를 삭제하면 안 됩니다.
창조.

## 스트리밍 가능한 HTTP

원격 전송이 선택되어 있으므로 기존 stdio 전용 대상은 작게 유지되고
HTTP/OpenSSL 종속성을 자동으로 얻지 않습니다.

```bash
cmake -S . -B build-harness-http \
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

엔드포인트는 `http://127.0.0.1:8080/mcp`입니다. 이 엔드포인트는 세션별 MCP 수명주기와
JSON 응답을 포함하는 공개된 MCP 2025-11-25 Streamable HTTP POST 계약을 구현합니다. 알림은 HTTP 202를 반환합니다. DELETE는 세션을 종료합니다.
선택적 독립형 GET/SSE 채널은 의도적으로 구현되지 않으며
전송 사양에서 명시적으로 허용하는 HTTP 405를 반환합니다.

보안 기본값은 전송 수준이며 인증을 결합하지 않습니다.
`GraphEngine` 또는 `HarnessService`:

- 기본 바인드는 `127.0.0.1`입니다. 루프백이 아닌 바인드는 다음과 같은 경우가 아니면 거부됩니다.
Bearer 승인자가 구성되었습니다.
- 제공된 모든 `Origin`는 다음 항목과 정확히 일치하지 않는 한 거부됩니다.
`NEOGRAPH_HARNESS_ALLOWED_ORIGINS`(실행 파일에서 쉼표로 구분).
- `NEOGRAPH_HARNESS_BEARER_TOKEN`는 실행 파일의 단일 주체를 활성화합니다.
무기명 경계. 라이브러리 임베딩은 다음을 사용할 수 있습니다.
OAuth/JWT 검증을 위해 `MCPHttpServerConfig::bearer_authorizer`를 사용하고
안정적인 principal/scope.
- 세션은 반환된 인증 범위에 바인딩됩니다. 다른 유효한
교장은 유출된 `Mcp-Session-Id`를 재사용할 수 없습니다.
- `MCPHttpServer` 팩토리는 검증된 범위를 수신하고
`MCPHttpServerSession` 소유자입니다. 다중 테넌트 임베딩은 범위를 사용하여 다음을 수행해야 합니다.
격리된 Harness record/checkpoint 매장을 선택합니다. 인증 상태가 입력되지 않습니다.
그래프 런타임 자체.
- 요청 페이로드, HTTP 작업자, 대기열, 세션 및 응답 대기 제한은 다음과 같습니다.
`MCPHttpServerConfig`로 제한됩니다.

루프백이 아닌 배포의 경우 신뢰할 수 있는 역방향 프록시에서 TLS를 종료하고
OAuth/OIDC 검증 또는 이에 상응하는 `bearer_authorizer`를 사용하세요. 전달
원본 `Authorization` 및 `Origin` 헤더는 일반 텍스트 공개를 노출하지 않습니다.
하네스 상태 디렉터리당 하나의 인증 도메인을 배포합니다.

## 호스트 설정

`SERVER`에 절대 경로를 사용하십시오.

```bash
SERVER=/absolute/path/to/build-harness/example_harness_mcp_server
```

Claude Code, 로컬 프로젝트 범위:

```bash
claude mcp add --scope local --transport stdio neograph-harness -- "$SERVER"
claude mcp get neograph-harness
```

코덱스 CLI:

```bash
codex mcp add neograph-harness -- "$SERVER"
codex mcp list
```

이 신뢰할 수 있는 로컬 서버에 대한 비대화형 `codex exec`의 경우 다음을 설정합니다.
`mcp_servers.neograph-harness.default_tools_approval_mode = "approve"` 에
코덱스 `config.toml`. 그것이 없으면 Codex는 `neograph_compile`를 올바르게 취소합니다.
아티팩트를 유지하는 것은 읽기 전용으로 주석 처리되지 않기 때문입니다. 대화형 세션
대신 기본 프롬프트를 유지할 수 있습니다.

OpenCode, 프로젝트 `opencode.json` 또는 사용자 구성:

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

`opencode mcp list`로 확인하세요. 이 양식은 각 호스트의 공식 MCP를 따릅니다.
2026년 7월 21일에 검토된 구성 계약입니다.

## PR 리뷰 워크플로

호스트에게 일반 저장소 도구를 사용하여 PR diff를 수집하도록 요청한 다음 다음을 사용하십시오.
하네스 도구. 적합한 요청은 다음과 같습니다.

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

`budgets.provider_timeout_seconds`는 공급자 완료 시도를 한 번으로 제한합니다.
1--600초. `budgets.max_output_tokens`는 한 번의 완료를 1--128000으로 제한합니다.
생성된 토큰. 둘 다 선택 사항입니다. 둘 중 하나를 생략하면 이전 항목이 유지됩니다.
하네스 기한이 없고 공급자의 기존 출력 제한이 없습니다.

작업자는 두 필드 중 하나를 더 작은 값으로 설정할 수 있습니다. 그 이상의 근로자 가치
하네스 전체 값은 컴파일 타임에 거부됩니다. 마감일에 Harness가 취소합니다.
해당 공급자 호출에 제공된 하위 취소 토큰만 해당됩니다. 그렇지 않다
형제 워커 또는 인클로징 실행을 취소합니다. 공급자는 토큰을 존중해야 합니다.
따라서 중단할 수 없는 제공자는 마감일 이후에 돌아올 수 있습니다.

호스트는 다음 순서를 따라야 합니다.

1. `neograph_compile`를 호출하고 `ok`가 false이면 중지합니다.
2. 반환된 `artifact_id`를 사용하여 `neograph_start`를 호출합니다.
3. `run_id`로 `neograph_get`를 폴링합니다. 이는 결과와 개수만 반환합니다.
4. If detail is needed, call `neograph_get` with the same `run_id` and a
`neograph://runs/...` URI를 `uri`로 반환했습니다. 추적을 상황에 맞게 가져오지 마세요.
기본적으로.

### 출처 찾기

세부정보 아티팩트는 스키마가 검증된 각 작업자 응답을 보존합니다.
`workers`는 기존 클라이언트에 대해 설정된 플랫 `findings` 배열을 유지합니다.
`finding_sources`는 동일한 길이의 병렬 배열입니다. 각 항목에는
`finding_index`, 소스 `worker_id` 및 해당 작업자의 `local_index`를 집계합니다.
이를 사용하여 `F1`와 같은 중복 로컬 ID의 소스를 식별합니다. 추가하지 마세요
작업자가 선언한 찾기 개체에 대한 출처 필드입니다.

## 호스트 중개 이력서

MCP 호스트인 경우 작업자 대신 `executor.kind: "host_brokered"`를 사용하십시오.
프로세스, 능력을 소유하고 있습니다. `executor.interaction`를 `"tool_result"`로 설정
(기본값) 또는 `"input"`. 공급자 실행자는 요청된 인수의 유효성을 검사합니다.
두 개의 비터미널 실행 상태 중 하나를 반환합니다.

- `awaiting_tool_results`: 호스트는 명명된 기능을 실행해야 합니다.
- `input_required`: 호스트는 입력 값을 수집해야 합니다.

`neograph_get`에는 고유한 `call_id`, `tool_id`가 있는 `pending` 객체가 포함되어 있습니다.
`arguments` 및 `result_schema`를 검증했습니다. 다음을 통해 해당 통화를 정확하게 제출하세요.

```json
{
  "run_id": "run_...",
  "call_id": "hcall_...",
  "result": {"answer": "validated host result"}
}
```

`neograph_resume`는 일치하지 않는 호출 ID를 거부합니다. 이는 다음을 위반하는 결과입니다.
선언된 스키마, 만료된 호출, 대기하지 않는 실행에 대한 지연 결과. 안
그래프를 다시 실행하지 않고도 동일한 중복이 인식됩니다. 에이
충돌하는 중복은 거부됩니다. 허용된 재개 의도가 지속됩니다.
실행이 예약되기 전에 프로세스 충돌 후 폴링이 다시 시작됩니다.
성공적인 형제를 반복하지 않고 `NodeInterrupt` 체크포인트에서 재개합니다.
노동자.

### 외부효과와 화해

일반적인 호스트 중개 계약은 이전 버전과 호환됩니다. 카탈로그 항목
`executor.effect`가 없으면 프로세스 후에도 `awaiting_tool_results`로 유지됩니다.
다시 시작하고 동일한 `{run_id, call_id, result}` 재개 요청을 수락합니다.

외부에서 볼 수 있는 비멱등성을 만들 수 있는 호스트 기능의 경우
변경하려면 해당 위험을 명시적으로 선언하세요. 효과 메타데이터는
기본 `host_brokered` `tool_result` 상호 작용; 입력 컬렉션이 아닙니다.
메타데이터.

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

보류 중인 호출에는 내구성 있는 `effect` 개체가 포함됩니다. `effect_id`와
`idempotency_key`는 하네스 실행 범위로 지정되며 공급자의 범위와 다릅니다.
도구 호출 ID. `status_query` 및 `fencing`는 호스트 기능을 설명합니다. 마구
이를 기록하지만 공급자별 쿼리나 재시도 프로토콜을 만들지는 않습니다.

`idempotency: "unsupported"` 호출이 계속되는 동안 서비스가 다시 연결되는 경우
기다리면 `ambiguous_effect`로 실행되는 것만 변경됩니다. 이는 호스트를 의미합니다.
프로세스가 중지되기 전에 효과를 수행했을 수도 있지만 Harness는 수행할 수 없습니다.
둘 중 하나의 결과를 증명하십시오. 컴팩트 상태에는 `pending` 및 `ambiguity`가 포함됩니다.
저널에는 `host_brokered.effect.ambiguous`가 기록되어 있고 Harness에는 둘 다 기록되어 있지 않습니다.
도구를 재생하지 않고 결과를 실패 또는 완료된 것으로 보고합니다.

호스트가 자체적으로 확인한 후 `neograph_resume`를 통해 모호성을 해결합니다.
권위 있는 시스템:

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"completed","result":{"answer":"validated host result"}}
```

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"failed"}
```

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"unknown"}
```

`completed`는 `result`를 검증하고 사용한 다음 체크포인트에서 다시 시작합니다.
`failed`는 작업자를 다시 실행하지 않고 터미널 하네스 오류를 기록합니다.
`unknown`는 나중에 조정하기 위해 `ambiguous_effect`에서 실행을 종료합니다. 정확한
중복 완료, 실패 또는 알 수 없는 제출은 멱등성을 갖습니다. 상충되는
완료되거나 실패한 제출은 거부됩니다. 중복되지 않는 모든 조정
`host_brokered.effect.reconciled`로 저널링됩니다.

모호한 효과는 의도적으로 취소할 수 없으며 만료되지 않습니다. 에이
취소 또는 타임아웃은 외부 효과가 발생했는지 여부를 확인할 수 없습니다. 제출하다
권한 있는 시스템이 아직 문제를 해결할 수 없는 경우 `unknown`입니다.

이 프로토콜은 호스트 충돌 시 정확히 한 번만 전달된다고 주장하지 않습니다. 호스트
멱등성 키를 지원하거나 상태 쿼리는 해당 시스템을 사용하여
조정을 제출하기 전에 실제 결과를 결정합니다.

실행 스냅샷에는 `created_at`, `updated_at`, `expires_at` 및
`poll_after_ms`. 기본 TTL는 24시간이고 기본 폴링 간격은 다음과 같습니다.
1초; 임베딩은 `HarnessServiceConfig`를 통해 둘 다 재정의할 수 있습니다.

## 실험 작업 프로필

MCP 작업은 핵심 MCP 2025-11-25의 일부가 아니며 업스트림 확장은 여전히
자체적으로 실험적이라고 표시합니다. 따라서 NeoGraph는 기본적으로 비활성화되어 있으며
안정적인 `run_id`와 `neograph_get` 폴링 계약과는 별개입니다.

예제 서버를 옵트인하려면 내구성 상태도 활성화해야 합니다.

```bash
export NEOGRAPH_HARNESS_STATE_DIR="$PWD/.neograph-harness-state"
export NEOGRAPH_HARNESS_EXPERIMENTAL_TASKS=1
```

그런 다음 서버는 `io.modelcontextprotocol/tasks`를 광고하고 표시합니다.
선택적 작업 지원이 포함된 `neograph_start`이며 `tasks/get`를 제공합니다.
`tasks/update` 및 `tasks/cancel`. 다음과 같은 경우에만 `CreateTaskResult`를 반환합니다.
개별 `tools/call` 요청에는 다음이 포함됩니다.

```json
{
  "_meta": {
    "io.modelcontextprotocol/clientCapabilities": {
      "extensions": {"io.modelcontextprotocol/tasks": {}}
    }
  }
}
```

해당 요청 옵트인이 없는 클라이언트는 일반 `CallToolResult`를 수신하고
`neograph_get` 폴링을 계속합니다. 프로필을 활성화해도 안정 상태는 변경되지 않습니다.
대체. 작업 상태는 `working`, `input_required`, `completed`, `failed`,
및 `cancelled`. `tasks/update.inputResponses`는 보류 중인 키로 입력됩니다.
`call_id` 및 폴링 클라이언트는 `pollIntervalMs` 및 `ttlMs`를 준수해야 합니다.

## 기능 백엔드

`make_provider_harness_executor`는 모든 NeoGraph를 통해 작업자를 유도합니다.
`Provider`. 모델이 선언된 도구를 요청하면 실행자는 해당 도구의 유효성을 검사합니다.
발송 전후 카탈로그에 대한 인수 및 출력.

초기화된 다운스트림에는 `make_mcp_harness_capability_executor`를 사용하세요.
`MCPClient` 인스턴스 또는 A2A의 경우 `a2a::make_harness_capability_executor`
자치령 대표. 요청은 권한으로 유지됩니다. 작업자는 나열된 도구 ID만 볼 수 있습니다.
`tools` 배열에 있습니다.

파일 시스템 도구의 경우 `path_arguments`에서 모든 경로 포함 입력을 선언하고
`policy.workspace_roots`를 설정합니다. 상대 경로는 첫 번째 루트에서 확인됩니다.
구성된 모든 루트 외부의 정식 경로는 발송 전에 거부됩니다.
기존 심볼릭 링크를 통한 이스케이프를 포함합니다. 정식 경로는 다음으로 전달됩니다.
모델에서 제공하는 철자가 아닌 기능 백엔드. 다운스트림 MCP
및 A2A 서비스는 별도의 신뢰 경계로 유지되며 동일한 신뢰 경계를 적용해야 합니다.
파일 시스템 time-of-check/time-of-use 경주를 종료하는 루트 정책.
`policy.read_only: true`를 사용하면 컴파일 시 모든 카탈로그 항목이 거부됩니다.
`read_only: true`로 표시되었습니다.

## 배포 및 프로토콜 프로필

지원되는 로컬 배포 경로는 설치 가능
위의 `neograph-harness-mcp` 바이너리. 소스 빌드는 다음을 계속 사용할 수 있습니다.
예제 대상이며 Python 휠은 library/runtime 패키지로 유지됩니다.
암시적으로 원격 데몬을 설치합니다. MCPB 및 공식 레지스트리 게시
release/discovery 패키징 옵션을 유지합니다. 전선에는 필요하지 않습니다.
프로토콜이며 서명된 릴리스 아티팩트와 명시적인 프로토콜을 통해서만 추가되어야 합니다.
원격 인증 배포 매니페스트.

NeoGraph는 현재 날짜가 지정된 MCP `2025-11-25` 프로필만 게시합니다. 결정적인
미래의 무상태 프로토콜을 설명하는 SEP는 새로운 유선 버전을 생성하지 않습니다.
MCP 프로젝트가 새로운 프로필을 게시할 때까지 후속 프로필은 광고되지 않습니다.
일자 사양.
