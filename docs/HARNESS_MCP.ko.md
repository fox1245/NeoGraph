<!-- neograph-i18n: source=docs/HARNESS_MCP.md locale=ko source_sha256=8601f242503105c39bba073ca851309cdc8898dccadab7d4d994fa2955bbc4e0 -->
# NeoGraph Harness MCP

**Languages:** [English](HARNESS_MCP.md) | [한국어](HARNESS_MCP.ko.md) | [日本語](HARNESS_MCP.ja.md) | [简体中文](HARNESS_MCP.zh-CN.md)

NeoGraph Harness는 실행하기 전에 범위가 제한된 다중 워커 워크플로를 컴파일합니다. 안정적인 MCP 인터페이스는 도구 여섯 개로 유지됩니다.

- `neograph_schema`는 설치된 요청 계약과 프리셋을 조회합니다.
- `neograph_compile`은 실행하지 않고 컴파일과 검증만 합니다.
- `neograph_start`는 보관된 아티팩트나 인라인 요청으로 워크플로를 시작합니다.
- `neograph_get`은 간단한 상태를 폴링하거나 결과 아티팩트 URI를 역참조합니다.
- `neograph_resume`은 대기 중인 바로 그 호스트 결과를 검증해 제출합니다.
- `neograph_cancel`은 대기열에 있거나, 실행 중이거나, 결과를 기다리는 워크플로를 협조적으로 취소합니다.

기본으로 제공하는 프리셋은 `fanout_judge`, `pr_review_panel`, `bug_triage`, `research_synthesis`입니다. 프리셋은 일반적인 strict-Core 그래프 아티팩트를 만들고, JavaScript 요청은 자체 `ProgramSource` 봉투(envelope)와 소스 맵을 그대로 유지합니다.

### JavaScript 작성 경계

새로 발행할 때 `harness.mode`에는 `preset` 또는 `javascript`를 쓸 수 있습니다. JavaScript 요청은 소스 텍스트를 `harness.source`에 담고, 필요하면 `harness.source_id`를 고정할 수 있습니다.

```json
{
  "harness": {
    "mode": "javascript",
    "source_id": "review:main.js",
    "source": "export function define() { const g = ng.graph('main'); /* ... */ return g; }"
  }
}
```

변환기는 이 텍스트를 정규 `ProgramSource` JavaScript 봉투(언어 `javascript`, QuickJS 엔진, 고정된 호스트 API, imports, 소스 맵)로 감싸서 `ProgramCompiler`, `ProgramCatalog`, `ProgramRuntime`에 차례로 넘깁니다. `define()`은 봉인된 `ng` 바인딩으로 그래프 하나를 구성합니다. 선택 사항인 제너레이터 `main()`은 일반적인 제어 흐름을 맡아 기존의 타입이 있는 Program command를 yield합니다. JavaScript는 Core 노드를 직접 디스패치하거나 provider와 도구를 고르지 못하며, 승인(admission), 예산, 저널, 재생(replay)을 우회할 수도 없습니다.

평가된 모듈은 결과 계약도 정합니다. `define()`만 내보내는 소스는 `channels.final_result.value` 래퍼를 포함한 Core 루트 계약을 유지합니다. 반면 런타임 `main(input)`을 내보내는 소스는 최종 반환값이 Harness 결과 스키마를 그대로 따르도록 공개하고 검증합니다.

#### 제어 흐름 마이그레이션 예시

`define()`은 컴파일 시점의 코드로만 두고, 런타임 효과는 모두 yield하는 타입 command 뒤에 두어야 합니다. 아래는 완성된 요청입니다. 제너레이터는 `ng.all` join과 그 안의 Core 호출 두 번까지 모두 세 개의 연산을 수행하며 병렬도는 2입니다. 워커 노드는 요청에서 봉인된 설정을 그대로 반복하고, 최종 반환값은 Harness 결과 형태입니다.

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

소스 위치를 나타내는 안정적인 문자열(source-site string)은 영속 command 좌표의 일부입니다. 재시도와 재시작을 거쳐도 항상 같은 값이 되도록 결정적으로 유지해야 합니다. 필요한 성공 개수가 먼저 채워진 쪽이 이기게 하려면 `ng.any(...)`를, 먼저 끝난 멤버가 이기게 하려면 `ng.race(...)`를 씁니다. 둘 다 구조적 동시성으로 아직 끝나지 않은 나머지 멤버를 취소합니다. 임의의 I/O, 타이머, 동적 로딩, `eval`, 네이티브 핸들은 계속 쓸 수 없습니다.

`harness.mode`는 반드시 명시해야 합니다. `dsl`은 `H_MIGRATION_CORE_DSL`, `core`는 `H_MIGRATION_CORE_JSON`, `program`과 `program_json`은 `H_MIGRATION_PROGRAM_JSON`을 반환합니다. 모두 `/harness/mode`를 가리키며, 요청 JSON의 모양이나 빠진 필드를 보고 모드를 추측하는 일은 없습니다. strict Core JSON은 검증된 Core·Program 아티팩트를 위한 내부/교환용 표현으로 남고, 신뢰할 수 있는 C++의 프로세스 내 구성도 계속 지원합니다. 하지만 둘 다 공개 Harness 작성 언어는 아닙니다.

스키마 내보내기, 컴파일, 시작은 모두 같은 불변 `HarnessAdmissionProfile`을 사용합니다. 이 프로파일의 범위가 지정된 `GraphRegistry`와 manifest에는 사용할 수 있는 모든 노드, 리듀서, 조건이 구현, 로워링(lowering), 호환성 메타데이터와 함께 나열됩니다. 프로세스 전역 레지스트리의 항목은 이 팔레트에 포함되지 않으며 Harness 승인(admission)에서 확인할 수 없습니다. 컴파일은 검증된 선언형 `TopologySpec`까지만 진행하므로, 거부된 입력은 `GraphNode`를 만들지 않고 워커나 효과를 디스패치하지도 않습니다. 보관된 아티팩트는 프로파일 ID와 지문(fingerprint)에 묶여 있습니다. 프로파일이 다르거나 프로파일이 도입되기 전에 만든 아티팩트는 다시 해석되지 않고 start/resume 단계에서 실패 시 차단으로 거부됩니다.

C++ 임베더는 기본값이 아닌 프로파일을 생성할 때 `HarnessServiceResources`로 넘깁니다. 이 리소스 경계는 기존 구조에 덧붙이는 방식이어서 `HarnessServiceConfig`의 레이아웃은 그대로입니다. 프로파일 지문은 manifest와, 범위가 지정된 레지스트리가 내보내는 의미 투영(semantic projection)을 포함합니다. 각 `implementation_identity`는 신뢰하는 선언이므로, 그 callable의 동작이 바뀔 때마다 함께 바꿔야 합니다.

이것은 현재 Program을 바탕으로 동작하는 Harness 호환 어댑터입니다. 수락된 Harness 요청은 지금도 기존 방식의 `ProgramSource`로 변환되어 `ProgramCompiler`로 컴파일되고, `ProgramCatalog`가 승인(admission)하며, `ProgramRuntime`에서 실행됩니다. 노드를 실행하는 주체는 여전히 `GraphEngine` 하나뿐입니다.

Harness 워크플로는 내장 QuickJS 위의 표준 JavaScript로 작성합니다. 이전의 `dsl`, 독립형 `core`, `program` 모드는 마이그레이션 진단 메시지와 함께 거부되며, strict Core JSON은 작성 언어가 아니라 내부 교환용 데이터입니다. 이 문서는 남아 있는 호환 동작과 그 진단을 설명합니다.

## 로컬에서 인증된 호스트 워커

`neograph-harness-mcp`는 워커의 추론을 직접 지정한 로컬 CLI 하나에 위임할 수 있습니다. 이 CLI는 이미 인증되어 있어야 합니다. 이것은 **모델 위임**이지 자격 증명 상속이 아닙니다. NeoGraph는 호스트의 인증 파일을 열지 않고, OAuth 토큰을 받지 않으며, 구독을 provider API 키로 바꾸지도 않습니다. 공식 호스트 프로세스가 자신이 저장해 둔 로그인을 사용합니다. 프로세스 그룹 격리를 위해 Linux가 필요하며, 다른 플랫폼에서는 호스트 백엔드가 실패 시 차단으로 거부됩니다. `auto` 선택은 없습니다. 인증된 호스트가 여러 개라는 이유로 청구나 정책에 관한 결정이 조용히 내려져서는 안 되기 때문입니다.

```bash
opencode auth login                 # or claude auth login / codex login, once
neograph-harness-mcp --executor opencode --host-status
neograph-harness-mcp --executor opencode --host-model openai/YOUR_MODEL
```

다른 CLI를 쓰려면 `--executor claude` 또는 `--executor codex`를 고릅니다. `--host-model`을 생략하면 그 어댑터의 호스트 기본 모델을 요청합니다. 환경 변수로는 `NEOGRAPH_HARNESS_EXECUTOR`와 `NEOGRAPH_HARNESS_HOST_MODEL`을 쓸 수 있습니다. 상태 조회와 일반 시작은 먼저 시간 제한이 있는, 비밀 정보 없는 버전·로그인 사전 점검(preflight)을 수행하고, OpenCode는 명시한 모델 이름을 `opencode models`로 확인합니다. Claude와 Codex는 이식 가능한 모델 목록 API를 같은 방식으로 제공하지 않으므로, 모델 선택 오류는 실제 요청을 시도한 뒤에 보고됩니다. CLI가 없거나, 로그아웃되었거나, 지원하지 않는 출력이거나, 쿼터·정책·모델 문제가 생겨도 조용히 provider 호출로 바꾸지 않습니다. 정확한 모델(또는 `host default`), CLI 버전, executor 식별 정보, 모드는 상태와 호스트 메타데이터에 표시되며 자격 증명은 포함되지 않습니다.

임베더와 앞으로 나올 공유 프로세스 MCP 설정은 `<neograph/mcp/harness_host_agent.h>`로 같은 경계를 구성할 수 있습니다. 먼저 `preflight_host_agent(config)`를 호출하고, 그다음 `HarnessProgramHostConfig::worker_executor = make_host_agent_executor(config)`를 지정합니다. `HostAgentExecutorConfig`의 workspace는 명시적으로 지정한 정규(canonical) 루트입니다. 비밀 정보가 아닌 executor/모델 식별 정보를 `provider_host_configuration`에 묶어 두면, 보관된 아티팩트를 다른 경로(route)로 재개할 수 없습니다. 이 경계는 직접 `Provider` executor를 바꾸지 않으며 외부로 나가는 MCP Sampling도 필요로 하지 않습니다.

서브프로세스 프로파일은 의도적으로 읽기 전용이며 로컬 stdio만 지원합니다. OpenCode는 새로 만든 임시 설정 디렉터리에서 `--pure`로 실행하며 read/glob/grep만 허용합니다. 모델의 read 도구는 `.env`와 알려진 호스트 자격 증명 경로에 계속 접근할 수 없고, workspace 루트에 대해서만 읽기 전용 외부 경로 접근이 허용됩니다. Claude는 Read/Glob/Grep만 허용한 `-p --safe-mode --permission-mode plan`으로 실행하며, 구독 로그인을 건너뛰는 `--bare`는 쓰지 않습니다. Codex는 `exec --ignore-user-config --ignore-rules --ephemeral --sandbox read-only`로 실행합니다. 모든 서브프로세스에는 다음 제한이 적용됩니다. 필요한 환경 변수만 허용 목록으로 전달하며 직접 provider 자격 증명과 관련 없는 저장소·클라우드 자격 증명은 제외합니다. NeoGraph 호스트 위임이 중첩되는 것을 막는 깊이 표지(depth marker)를 붙입니다. 프롬프트, 이벤트, stdout, stderr 캡처 크기를 제한하고 데드라인을 둡니다. 취소하면 프로세스 그룹을 종료합니다. 프롬프트나 모델 ID를 셸이 해석하는 일은 없습니다. Program 수준의 스키마 검증과 제한된 재시도도 그대로 적용됩니다. CLI가 생성한 JSON은 워커 스키마 게이트를 통과하기 전까지 신뢰하지 않습니다. 사용량은 호스트가 내보내는, 기계가 읽을 수 있는 완료 이벤트에서 집계합니다. 이 CLI 전송 방식에는 생성 중에 토큰 수를 강제로 제한하는 보편적인 상한이 없습니다. 예산을 넘은 응답은 거부되고 judge에게 전달되지 않지만, 업스트림에서는 이미 사용량이 청구되었을 수 있습니다. workspace 읽기 전용 권한은 호스트 정책 경계일 뿐 OS 마운트 네임스페이스가 아닙니다. 신뢰할 수 없는 저장소나 호스트 정책 때문에 강한 파일 시스템 격리가 필요하면 격리된 OS 계정이나 컨테이너를 사용해야 합니다. 로컬 CLI 어댑터는 Harness capability 도구를 받지 않습니다. capability가 많이 필요한 워커에는 직접 Provider executor를 사용합니다.

라이브 테스트는 신뢰할 수 있고 인증된 비공개 러너에서만 선택적으로 실행합니다. `NEOGRAPH_HARNESS_LIVE_HOST=claude|codex|opencode`로 설치된 CLI 하나를 고르고, `NEOGRAPH_HARNESS_LIVE_MODEL`로 모델을 고정할 수 있습니다(선택 사항). OpenCode OAuth 작업에서는 `NEOGRAPH_HARNESS_LIVE_OPENCODE_OAUTH=1`과 사용 가능한 `openai/...` 모델도 함께 설정합니다. 테스트는 실행 전에 `opencode auth list`가 OpenAI OAuth를 보고하는지(비밀 정보가 아닙니다) 확인합니다. 이 표지는 테스트를 켜는 게이트일 뿐 자격 증명이 아닙니다. 로그인, 모델, 게이트 중 하나라도 없으면 추론을 지어내지 않고 명시적으로 건너뜁니다.

각 호스트의 표준 로컬 stdio 설정으로 MCP 서버를 등록할 수 있습니다. PATH에서 찾은, 설치된 `neograph-harness-mcp` 바이너리를 지정합니다(MCP 항목에 자격 증명을 붙여 넣지 마세요).

```bash
claude mcp add neograph-harness -- neograph-harness-mcp --executor claude
codex mcp add neograph-harness -- neograph-harness-mcp --executor codex
```

OpenCode는 `opencode mcp add`를 실행하고 안내 프롬프트에서 로컬 서버를 고른 뒤 명령으로 `neograph-harness-mcp --executor opencode`를 입력합니다. 같은 설정을 `opencode.json`에서 지원되는 형식으로 쓰면 다음과 같습니다.

```json
{"mcp":{"neograph-harness":{"type":"local","command":["neograph-harness-mcp","--executor","opencode"],"enabled":true}}}
```

[OpenCode CLI](https://opencode.ai/docs/cli/), [Claude CLI](https://code.claude.com/docs/en/cli-reference), [Claude 인증](https://code.claude.com/docs/en/authentication), [Codex 비대화형 모드](https://learn.chatgpt.com/docs/non-interactive-mode), [Codex 인증](https://learn.chatgpt.com/docs/auth)을 참고하세요. 위임된 호출에도 호스트의 구독 쿼터, 속도 제한, 모델 사용 자격, 보존 정책, 조직 정책, 데이터 처리 방침이 그대로 적용됩니다. 서드파티 제품이 Claude.ai 구독을 사용하려면 Anthropic의 승인이 필요할 수 있습니다. 사용자가 직접 선택해 설치된 CLI를 쓰는 이 방식은 호스팅된 Claude 구독 백엔드를 재배포해도 된다는 승인이 **아닙니다**. 한 호스트에 로그인했다고 해서 다른 벤더나 모델을 쓸 수 있다고 가정해서는 안 됩니다. 이 로컬 백엔드는 원격 HTTP 위임이나 MCP Sampling(이식 가능한 호스트 기준에서는 지원되지 않음)을 켜지 않습니다. 원격·서버 배포에는 직접 Provider 자격 증명을 사용해야 합니다.

### 직접 API provider (독립 실행/서버)

선택 사항인 로컬 서버를 빌드하고 설치합니다(호스트 CLI 모드 자체에는 NeoGraph 전용 모델 키가 필요 없습니다).

```bash
cmake -S . -B build-harness \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_LLM=ON \
  -DNEOGRAPH_BUILD_MCP_SERVER=ON \
  -DNEOGRAPH_BUILD_HARNESS_MCP_BINARY=ON
cmake --build build-harness --target neograph_harness_mcp -j
cmake --install build-harness --prefix "$HOME/.local"
export NEOGRAPH_HARNESS_API_KEY=your-key
neograph-harness-mcp --executor provider
```

포함된 OpenRouter 예제에서는 `NEOGRAPH_HARNESS_API_KEY`가 `OPENROUTER_API_KEY`보다 우선합니다. 로컬 호스트 실행과 달리 직접 provider 모드에는 별도의 API 키가 필요합니다. 서버는 프로토콜 메시지를 stdout에만, 진단 메시지를 stderr에만 씁니다.

호스트 상호 운용성 스모크 테스트에만 `--executor provider`와 함께 `NEOGRAPH_HARNESS_SMOKE=1`을 설정합니다. 이 모드는 프로세스 안에서 동작하는 결정적 provider를 사용하며, 발견 사항이 0건인 유효한 리뷰를 반환합니다. API 키가 필요 없고, LLM 품질 테스트도 아닙니다.

### 자격 증명이 필요 없는 OpenCode 전역 MCP 도입

단일 사용자 로컬 설치에서는 `<neograph/mcp/adoption.h>`로 OpenCode 사용자 전역 `opencode.json`에 있는, 자격 증명이 필요 없는 stdio 서버 중 일부를 명시적으로 도입(adoption)할 수 있습니다. 탐색(discovery)은 조회만 합니다. 사용자 소유의 일반 파일인 그 전역 파일만 읽고, 프로젝트·workspace 설정이나 OAuth 저장소는 읽지 않으며, 서버를 시작하지도 않습니다. HTTP 항목, 비활성화된 항목, 가져온 환경 변수나 파일 참조, 지원하지 않는 필드, 셸 executor, 재귀적인 NeoGraph 항목은 거부됩니다.

도입에는 세 가지가 필요합니다. `argv`에 자격 증명이 없다는 명시적 증명(attestation), 별도로 승인된 실행(launch) 기록, 선택한 도구/스키마의 capability manifest입니다. `pinned` 기록은 소스 내용, 정규 cwd, 실행 파일/인터프리터 식별 정보, argv, 식별할 수 있는 스크립트/패키지를 묶어 고정합니다. 검증할 수 없는 실행 형태는 명시적인 `trusted_mutable` 승인이 있어야 하며, 조용히 낮춰서 허용하지 않습니다. 탐색은 방금 조사한 바이트를 승인하지 않습니다.

각 도구 manifest는 `argument_policy: "exact-arguments-v1"`을 쓰고, `argument_predicate: {"allowed_arguments": [<complete approved argument objects>]}`로 승인된 인자 객체를 빠짐없이 나열해야 합니다. 비교 대상은 스키마 모양뿐 아니라 리소스를 포함한 인자 값입니다. 일반적인 `read-only`나 SQL/경로 레이블은 거부됩니다. 이런 레이블로는 임의의 입력이 안전하다는 것을 증명할 수 없기 때문입니다. Harness가 가지는 권한은 계속 워커 선언, 도입한 manifest, 정적 정책, 프로세스 경계의 교집합입니다. MCP annotation으로 이를 넓힐 수 없습니다.

호스트 워커나 provider 워커를 만들기 전에 `HardenedMcpClientRegistry::configure_harness(host_config, provider_config)`를 호출합니다. 이 호출은 변경할 수 없는 네임스페이스 도구 메타데이터와 승인된 executor를 함께 설치하며, 요청에 필요한 대응 메타데이터는 `tool_catalog()`가 제공합니다. 승인이 철회되거나 스키마가 바뀌면(schema drift) 보관된 executor는 차단(fence)되고 공유 클라이언트는 명시적으로 종료됩니다. 다른 소유자가 아직 참조를 쥐고 있어도 마찬가지입니다. 스키마 갱신과 도구 디스패치는 호출자의 데드라인과 취소 토큰을 따릅니다.

도입한 클라이언트는 절대 경로의 실행 파일, 정규 cwd, 허용 목록으로 대체한 환경 변수, 크기가 제한된 프로토콜 프레임과 stderr, 프로세스 트리 종료를 사용합니다. Windows에서는 상속할 핸들의 허용 목록과, 닫히면 프로세스를 종료하는 Job Object를 사용해 실행합니다. 상태 기록에는 해시와 이름만 들어 있고 argv, 원본 설정, stderr, 자격 증명은 들어 있지 않습니다. 자격 증명을 다루는 모드(`secret_injected`, `host_brokered`), 원격 HTTP MCP, 임의의 셸·CLI 실행은 계속 지원하지 않습니다.

설치 가능한 예제는 provider 워커 executor와 함께 쓸 때만 도입을 지원합니다. 별도로 검토한 JSON 승인 파일을 읽으며, 자동으로 동의하는 환경 변수 플래그는 없습니다. `NEOGRAPH_HARNESS_MCP_APPROVAL`에는 파일의 내용이 아니라 경로를 넣습니다(파일 크기는 최대 1 MiB).

```bash
export NEOGRAPH_HARNESS_MCP_SOURCE="$HOME/.config/opencode/opencode.json"
export NEOGRAPH_HARNESS_MCP_APPROVAL="$PWD/approved-mcp.json"
neograph-harness-mcp --executor provider
```

승인 객체는 `McpLaunchApproval`, `McpToolApproval`과 대응하는 `launch`, `tools` 멤버를 가집니다. `launch`에는 `trust_mode`, 자격 증명이 없다는 명시적 증명을 나타내는 불리언 `argv_no_credentials_attested`, `source_path`, `source_content_hash`, `cwd`, `executable`, `executable_identity`, `argv_hash`, `interpreter_identity`, `package_identity`가 들어 있습니다. 호스트 쪽의 `make_mcp_launch_approval()` 도우미가 검토용 식별 정보를 계산합니다. `tools`에는 `server_name`, `launch_identity`(승인된 launch의 `executable_identity`), `selected_tools`, `schema_hashes`, `manifest`, `policy_version`이 들어 있습니다. 스키마 승인을 기록할 때는 정규화한 `ToolDefinition::from_json(definition).to_json()` 값을 `mcp_tool_schema_hash()`로 해시합니다.

소스를 명시적으로 지정해 덮어쓰더라도 문서에 정해진 사용자 전역 파일이어야 하며, 프로젝트 경로를 전역 설정으로 위장할 수 없습니다. 소스를 설정한 상태의 `--host-status`는 자격 증명을 확인하기 전에 탐색 결과만 담은, 민감 정보를 가린 기록을 출력하고 다운스트림 서버를 시작하지 않습니다.

영속 host-brokered 호출에는 기록(record)과 체크포인트 영속화가 모두 필요합니다. 예제는 디렉터리 하나를 명시적으로 지정해 두 가지를 모두 켭니다.

```bash
export NEOGRAPH_HARNESS_STATE_DIR="$PWD/.neograph-harness-state"
```

`runs.db`에는 변경할 수 없는 아티팩트, 변경 가능한 run 기록, 추가만 가능한(append-only) 인과 저널이 저장되고, `checkpoints.db`에는 그래프 체크포인트가 저장됩니다. 저널 행과 Harness가 만든 모든 체크포인트는 run을 그 run의 불변 아티팩트, 컴파일된 리비전 다이제스트, MCP 프로토콜 버전, Harness 프로파일에 묶습니다. 워커 시도(attempt)에는 소요 시간, 검증·재시도 결과, 그리고 provider·capability·host-brokered 호출을 그 호출을 발생시킨 시도에 연결하는 상관 관계 ID가 들어 있습니다. 두 SQLite 저장소는 모두 WAL 모드와 제한된 busy timeout을 사용합니다. 버전 1 기록 데이터베이스는 열 때 트랜잭션으로 버전 3으로 마이그레이션됩니다. 이 디렉터리는 서버를 재시작해도 유지됩니다. 두 저장소 중 하나라도 없으면 `host_brokered` 카탈로그 항목은 컴파일 시점에 거부되므로, 워크플로가 실제로 지원하지 않는 재개 가능성을 내세울 수 없습니다.

직접 구성하는 임베딩에서는 선택 사항인 `neograph::mcp_sqlite` 타깃의 `SqliteHarnessRecordStore`로 같은 백엔드를 만들 수 있습니다. 기본 저널 모드는 일반적인 비밀 필드와 콘텐츠 필드를 SQLite에 넘기기 전에 재귀적으로 `[REDACTED]`로 바꿉니다. `METADATA_ONLY`는 모든 이벤트 payload를 버리고, `FULL`은 provider 콘텐츠, 도구 인자, 결과를 그대로 보존하므로 저장이 승인된 데이터에만 켜야 합니다. 이벤트는 `HarnessJournal::list_events(run_id, after_sequence, limit)`로 run 순서대로 읽을 수 있습니다. 원자적인 JSON 파일을 선호하는 배포에는 `FileHarnessRecordStore`를 계속 쓸 수 있지만, 이 저장소는 저널 경계를 구현하지 않습니다.

### 보존

SQLite 저장소는 선택 사항인 형제 인터페이스 `HarnessRetentionStore`를 구현하며, 안정적인 `HarnessRecordStore`의 vtable은 바뀌지 않습니다. `HarnessService`는 아티팩트를 보관하거나 run을 시작하기 전에 `HarnessServiceConfig`의 `max_artifacts`와 `max_runs`를 적용합니다. 기본값은 각각 128입니다.

정리는 종료된 리프(leaf) run만 삭제합니다. 대기열에 있거나, 실행 중이거나, 입력을 기다리는 run은 보호되고, 저널 마무리를 아직 끝내지 못한 프로세스 내 실행도 보호됩니다. 재생(replay)이나 fork 행은 `source_run_id`를 기록하므로, 그 의존 행이 남아 있는 동안에는 원본을 삭제할 수 없습니다. 공간이 필요하면 의존하는 리프를 먼저 삭제하고, 원본은 자신을 참조하는 보존 행이 모두 없어진 뒤의 다음 단계에서야 삭제 대상이 됩니다. 따라서 모든 후보가 활성 상태이거나, 명시적으로 보호되거나, 아직 참조되고 있다면 한도는 엄격하게 지켜지지 않습니다(soft limit).

`runs.db` 안에서는 하나의 트랜잭션이 run의 저널 행을 먼저 삭제한 뒤 run 행을 삭제하며, 아티팩트는 참조하는 run이 없을 때에만 삭제합니다. 이 커밋이 끝나면 Harness는 삭제된 run의 체크포인트 스레드를 따로 설정된 체크포인트 저장소에서 제거합니다. 이 두 번째 단계에서 크래시가 나거나 체크포인트 백엔드가 실패하면 어디서도 도달할 수 없는 체크포인트 저장 공간이 남을 수 있습니다. 그래도 보존된 재생(replay)/fork가 이미 삭제된 원본 기록을 가리키는 일은 생기지 않습니다. 체크포인트만 남은 이런 잔여물은 나중에 관리 작업이나 백엔드별 고아 정리(orphan sweep)로 회수할 수 있습니다.

`FileHarnessRecordStore`는 영속 정리를 구현하지 않습니다. 메모리 아티팩트 캐시 제거와 run 용량의 하드 한도는 그대로 유지합니다.

## 디버거 뷰

`neograph_get`은 기본값으로 간단한 `status`를 유지하면서, MCP 도구를 늘리지 않고 디버거 뷰 네 가지를 추가했습니다.

| 뷰 | 결과 |
|---|---|
| `attempts` | 저널에 기록된 워커 시도의 시작·완료·중단 이벤트 |
| `trace` | 기존의 순서가 있는 GraphEngine 노드 트레이스와 인과 저널 타임라인 |
| `checkpoints` | payload를 뺀 체크포인트 메타데이터. ID, 부모, 노드, phase, step, 채널 이름 |
| `diff` | 각 체크포인트와 그 부모 사이에서 바뀐 채널 값과 버전 |

`attempts`와 `trace`는 `after_sequence`를 앞으로만 진행하는 불투명(opaque) 커서로 받습니다. 네 뷰 모두 `limit`을 1부터 1000까지 받습니다. 반환되는 아티팩트 URI에도 같은 페이지 매김을 쿼리로 붙일 수 있습니다. 예는 다음과 같습니다.

```text
neograph://runs/run_123/attempts?after_sequence=17&limit=50
```

이 URI에서는 `after_sequence`와 `limit`만 허용합니다. 알 수 없거나 형식이 잘못된 쿼리 필드는 무시되지 않고 오류가 됩니다. 저널에서 읽는 뷰는 payload를 저장된 그대로 반환하므로 설정된 가림(redaction) 모드가 유지됩니다. `diff` 뷰는 저널이 아니라 체크포인트 저장소에서 계산하며 채널 값 전체를 담을 수 있습니다. 이 뷰에 대한 접근 권한은 기존의 상세 run 결과에 대한 접근 권한과 같게 다뤄야 합니다.

## 재생(replay) 모드

`neograph_start`는 MCP 도구를 늘리지 않고도 완료된 run을 재생(replay)할 수 있습니다.

```json
{"replay":{"source_run_id":"run_123","mode":"recorded"}}
```

`recorded`는 컴파일러가 고정한 그래프를 원본 저널에 기록된 완료된 워커 시도 결과로 다시 실행합니다. 설정된 워커, provider, MCP, A2A, capability executor는 호출하지 않습니다. 원본 아티팩트의 리비전, 프로토콜, 프로파일이 여전히 일치해야 하고, 저널은 `FULL` payload 모드여야 합니다. `REDACTED`와 `METADATA_ONLY` 저널에는 워커의 정확한 출력이 없으므로 재생(replay)할 수 없습니다. 캡처된 정확한 연산 좌표, 원래의 소유자/버전/입력 권한, 영속적인 provider bank 보관(custody)이 필요합니다. 호환되지 않는 오래된 보관 상태는 거부하며, 토큰 합계나 시도 횟수로 다시 만들어 내지 않습니다.

`mode: "live"`를 쓰면 같은 보관 아티팩트를 실제 provider와 도구로 실행합니다. 스냅샷과 저널의 수명 주기 이벤트는 run을 `recorded_replay` 또는 `live_replay`로 표시하고 `source_run_id`를 포함합니다. 일반 시작은 계속 `live`입니다.

recorded 재생(replay)은 원본이 처음 가졌던 불변의 호출 권한을, 남아 있는 지출 권한과 따로 인증합니다. 원본 계보(lineage)에 대한 CAS 한 번으로 남은 금액이 재생(replay) run으로 넘어가며, 두 번째 재생(replay)이 같은 원본의 잔액을 다시 쓸 수는 없습니다. 캡처된 호출은 과거의 연산 좌표를 그대로 쓰며 연산 슬롯을 다시 채우지 않습니다. 재생(replay) 중에 실제로 새로 발생하는 Core와 wall-time 작업은 넘겨받은 잔액을 소비합니다.

provider 보고서, 확정된 청구액, 불확실한 예약, 효과 식별자는 관찰 전용의 별도 bank로 정확히 한 번 복원(hydrate)됩니다. 재생(replay) 중에는 예약, 환불, 정산, 보고서 추가, 새 효과의 디스패치를 할 수 없습니다. 유실된 재생(replay) 실행은 영속 lease로 차단(fence)되고, 넘겨받은 크레딧과 불확실한 크레딧은 그대로 유지됩니다. 실제 디스패치로 복구하지 않습니다. run 스냅샷에는 크레딧이 재생(replay)으로 넘어간 원본을 포함해, 계보의 현재 잔액이 표시됩니다.

## 호환되는 Fork

먼저 수정한 Harness를 컴파일한 다음, 기존 `neograph_start` 도구로 정확히 앞선 체크포인트를 그 대상 아티팩트로 분기합니다.

```json
{
  "fork": {
    "source_run_id": "run_123",
    "checkpoint_id": "550e8400-e29b-41d4-a716-446655440000",
    "artifact_id": "artifact_repaired"
  }
}
```

원본 체크포인트는 `source_run_id`에 속해야 합니다. Harness는 run을 할당하기 전에 대상 아티팩트와 비교하여 체크포인트 스키마, 원본 리비전, MCP 프로토콜, Harness 프로파일, 복원되는 모든 채널과 리듀서, 이어서 실행할 모든 노드, 활성 barrier 인터페이스를 검증합니다. 호환되지 않는 분기는 `started: false`, `status: "incompatible_fork"`와 `path`, `witness`가 담긴 기계 판독 가능한 `H_FORK_*` 진단을 반환하며, run이나 fork 체크포인트는 만들지 않습니다.

체크포인트 저장소가 필요합니다. 기록 저장소가 없으면 fork는 현재 서비스 프로세스에 아직 남아 있는 원본 run과 아티팩트만 참조할 수 있습니다. 재시작 후에도 fork 계보가 유지되어야 한다면 두 저장소를 모두 설정합니다.

호환되는 분기에는 `compatible_fork` 레이블이 붙고, start 응답, 스냅샷, 수명 주기 저널 이벤트에 `source_run_id`와 `source_checkpoint_id`가 모두 담깁니다. 실행은 선택한 체크포인트의 `next_nodes`에서 재개되며, 이미 커밋된 선행 노드는 다시 실행되지 않습니다. 수정된 토폴로지, 워커 계약, 도구 카탈로그는 대상 아티팩트에서 가져오고, 원래의 task 채널을 포함한 복원 채널 값은 원본 체크포인트에서 가져옵니다. 작업 입력 자체를 바꿔야 한다면 fork 대신 새로 시작해야 합니다.

원본 run, 아티팩트, 선택한 체크포인트는 fork가 참조하는 대상이므로, 호환성 검사나 fork 실행이 사용할 수 있는 동안에는 계속 보존되어야 합니다. 보존 정리는 의존하는 쪽을 먼저 삭제하거나 참조되는 원본을 남겨 두어야 하며, 사전 점검(preflight)과 분기 생성 사이에 원본 체크포인트를 삭제해서는 안 됩니다.

## Streamable HTTP

원격 전송은 선택 사항입니다. 그래서 stdio만 지원하는 기존 타깃은 작게 유지되고, HTTP/OpenSSL 의존성이 조용히 늘어나지 않습니다.

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

엔드포인트는 `http://127.0.0.1:8080/mcp`입니다. 공개된 MCP 2025-11-25 Streamable HTTP POST 계약을 세션별 MCP 수명 주기와 JSON 응답으로 구현합니다. 알림(notification)에는 HTTP 202를 반환합니다. 클라이언트가 취소한 호출은 빈 이벤트 스트림으로 끝나며, 클라이언트에는 응답이 전달되지 않습니다. 세션은 다음 경우에 끝납니다. DELETE를 받았을 때, 요청 없이 30분이 지났을 때(`session_idle_timeout`), 그리고 새 클라이언트가 들어갈 자리가 필요한데 `max_sessions`(64)에 도달했고 이 세션이 처리 중인 요청이 없는 가장 오래전에 쓰인 세션일 때입니다. 이렇게 끝난 세션의 id로 요청하면 404를 반환하며, 클라이언트는 이를 보고 다시 초기화해야 합니다. 선택 사항인 독립 GET/SSE 채널은 의도적으로 구현하지 않았고 HTTP 405를 반환합니다. 전송 사양이 이를 명시적으로 허용합니다.

보안 기본값은 전송 계층에서 적용되며, 인증을 `GraphEngine`이나 `HarnessService`에 결합하지 않습니다.

- 기본 바인딩 주소는 `127.0.0.1`입니다. bearer authorizer를 설정하지 않으면 루프백이 아닌 주소에는 바인딩할 수 없습니다.
- `Origin`이 오면 `NEOGRAPH_HARNESS_ALLOWED_ORIGINS`의 항목과 정확히 일치하거나(실행 파일에서는 쉼표로 구분합니다), loopback 바인드에서 서버 자신의 loopback 출처(바인드한 포트의 `http://localhost:<port>`, `http://127.0.0.1:<port>` 또는 `http://[::1]:<port>`)가 아닌 한 거부됩니다.
- `NEOGRAPH_HARNESS_BEARER_TOKEN`을 설정하면 실행 파일의 단일 principal bearer 경계가 켜집니다. 라이브러리로 임베딩할 때는 `MCPHttpServerConfig::bearer_authorizer`로 OAuth/JWT를 검증하고 안정적인 principal/scope를 반환할 수 있습니다.
- 세션은 반환된 인가 범위(scope)에 묶입니다. 유출된 `Mcp-Session-Id`를 다른 유효한 principal이 재사용할 수 없습니다.
- `MCPHttpServer` 팩토리는 검증된 scope를 받아 `MCPHttpServerSession` 소유자를 반환합니다. 멀티 테넌트 임베딩은 이 scope로 서로 격리된 Harness 기록/체크포인트 저장소를 골라야 하며, 인증 상태가 그래프 런타임 안으로 들어가는 일은 없습니다. authorizer가 없으면(루프백 바인딩에서만 가능) scope는 비어 있고, 실행 파일은 로컬의 단일 소유자 scope를 유지합니다.
- 요청 payload, HTTP 워커, 큐, 세션, 응답 대기의 한도는 `MCPHttpServerConfig`가 정합니다. 도구 호출이 반환될 때까지 스레드 하나를 붙잡고 있으므로, HTTP 스레드는 필요할 때 `max_http_threads`(64)까지 시작됩니다.
- bearer 토큰을 OAuth 인가 서버에서 받는 임베딩은 `MCPHttpServerConfig::protected_resource_metadata`를 설정합니다. 그러면 서버가 RFC 9728 메타데이터를 공개하고, 모든 401 응답의 challenge에서 그 메타데이터를 가리킵니다.

`neograph::mcp::ScopedHarnessStore`는 공개 ID와, 스키마가 소유한 기록/저널 참조를 되돌릴 수 있는 테넌트 네임스페이스로 매핑합니다. 불투명한 요청, 결과, 이벤트 payload는 고쳐 쓰지 않습니다. File 저장소와 SQLite 저장소는 이 비공개 ID를 받아들입니다. 너무 긴 File 키는 고정 길이 해시 파일 이름을 쓰며, 짧은 키의 경로는 바뀌지 않습니다. SQLite 보존 정책은 한 트랜잭션 안에서 개수와 삭제 후보를 모두 선택한 네임스페이스로 제한하므로 다른 테넌트와 보호된 원본 참조는 그대로 남습니다. 이 저장소 경계는 인증된 scope나, 애플리케이션의 provider/도구/쿼터 정책을 대신하지 않습니다.

루프백이 아닌 배포에서는 신뢰할 수 있는 리버스 프록시에서 TLS를 종료하고, 그 프록시의 OAuth/OIDC 검증이나 이에 준하는 `bearer_authorizer`를 사용해야 합니다. 원래의 `Authorization`과 `Origin` 헤더를 그대로 전달하고, 평문 공개 리스너는 노출하지 말고, Harness 상태 디렉터리 하나에는 인가 도메인 하나만 배포해야 합니다.

## 호스트 설정

`SERVER`에는 설치된 `neograph-harness-mcp` 바이너리의 절대 경로를 지정합니다. 아래 항목은 직접 Provider executor를 선택합니다. 위에서 설명한 로컬 호스트 모드를 쓰려면 대신 `--executor claude|codex|opencode` 중 하나를 넘깁니다.

```bash
SERVER=/absolute/path/to/neograph-harness-mcp
```

Claude Code(로컬 프로젝트 범위):

```bash
claude mcp add --scope local --transport stdio neograph-harness -- "$SERVER" --executor provider
claude mcp get neograph-harness
```

Codex CLI:

```bash
codex mcp add neograph-harness -- "$SERVER" --executor provider
codex mcp list
```

신뢰하는 이 로컬 서버를 대상으로 비대화형 `codex exec`를 실행하려면 Codex `config.toml`에 `mcp_servers.neograph-harness.default_tools_approval_mode = "approve"`를 설정합니다. 설정하지 않으면 Codex는 `neograph_compile`을 취소합니다. 아티팩트를 보관하는 동작은 읽기 전용으로 표시되어 있지 않으므로 이는 올바른 동작입니다. 대화형 세션에서는 기본 프롬프트를 그대로 써도 됩니다.

OpenCode(프로젝트의 `opencode.json` 또는 사용자 설정):

```json
{
  "$schema": "https://opencode.ai/config.json",
  "mcp": {
    "neograph-harness": {
      "type": "local",
      "command": ["/absolute/path/to/neograph-harness-mcp", "--executor", "provider"],
      "enabled": true,
      "environment": {
        "NEOGRAPH_HARNESS_API_KEY": "{env:NEOGRAPH_HARNESS_API_KEY}"
      }
    }
  }
}
```

`opencode mcp list`로 확인합니다. 이 형식들은 각 호스트의 공식 MCP 설정 계약을 따릅니다.

## PR 리뷰 워크플로

호스트에게 평소 쓰는 저장소 도구로 PR diff를 수집하게 한 다음 Harness 도구를 사용합니다. 요청은 다음과 같이 작성하면 됩니다.

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

### Provider 예산

`budgets.provider_timeout_seconds`는 준비된 provider 연산 하나의 절대 데드라인을 1~600초로 정합니다. `budgets.max_output_tokens`는 그 연산의 출력 한도를 1~128000 토큰으로 정합니다. 둘 다 선택 사항입니다. 생략하면 그 제어가 설정되지 않은 채로 남고, 승인(admission)된 SDK/기본 정책이 그대로 유지됩니다. 시간이나 출력이 무제한이라는 뜻이 아닙니다. provider 재시도는 준비된 연산의 데드라인 안에서 SDK가 맡습니다.

워커는 두 필드를 더 작은 값으로 설정할 수 있습니다. Harness 전체 값보다 큰 워커 값은 컴파일 시점에 거부됩니다. 데드라인이 되면 Harness는 그 provider 호출에 전달한 자식 취소 토큰만 취소하고, 다른 워커나 상위 run은 취소하지 않습니다. provider가 토큰을 따라야 하므로, 중단할 수 없는 provider는 데드라인 이후에 반환될 수도 있습니다.

호스트는 다음 순서를 따라야 합니다.

1. `neograph_compile`을 호출하고 `ok`가 false이면 중단합니다.
2. 반환된 `artifact_id`로 `neograph_start`를 호출합니다.
3. `run_id`로 `neograph_get`을 폴링합니다. 이때는 결과와 개수만 반환됩니다.
4. 자세한 내용이 필요하면 같은 `run_id`와, 반환된 `neograph://runs/...` URI를 `uri`로 넘겨 `neograph_get`을 호출합니다. 트레이스를 기본으로 컨텍스트에 가져오지 않습니다.

### 발견 사항의 출처

상세 아티팩트는 스키마 검증을 통과한 각 워커 응답을 `workers`에 그대로 보존하고, 기존 클라이언트를 위해 지금까지 쓰던 평평한 `findings` 배열도 유지합니다. `finding_sources`는 길이가 같은 병렬 배열입니다. 각 항목에는 전체 기준의 `finding_index`, 출처 `worker_id`, 그 워커 안에서의 `local_index`가 들어 있습니다. `F1`처럼 로컬 ID가 겹칠 때 출처를 구분하는 데 사용합니다. 워커가 선언한 finding 객체에 출처 필드를 추가해서는 안 됩니다.

## 호스트 중개(host-brokered) 재개

워커 프로세스가 아니라 MCP 호스트가 capability를 소유할 때는 `executor.kind: "host_brokered"`를 씁니다. `executor.interaction`은 `"tool_result"`(기본값) 또는 `"input"`으로 설정합니다. provider executor는 요청된 인자를 검증한 뒤 종료되지 않은 run 상태 두 가지 중 하나를 반환합니다.

- `awaiting_tool_results`: 호스트가 지정된 capability를 실행해야 합니다.
- `input_required`: 호스트가 입력 값을 수집해야 합니다.

`neograph_get`의 결과에는 고유한 `call_id`, `tool_id`, 검증된 `arguments`, `result_schema`를 담은 `pending` 객체가 들어 있습니다. 바로 그 호출을 다음과 같이 제출합니다.

```json
{
  "run_id": "run_...",
  "call_id": "hcall_...",
  "result": {"answer": "validated host result"}
}
```

`neograph_resume`은 call ID가 맞지 않는 요청, 선언된 스키마를 위반한 결과, 만료된 호출, 대기 중이 아닌 run에 늦게 도착한 결과를 거부합니다. 내용이 같은 중복 제출은 그래프를 다시 실행하지 않고 수락 응답만 돌려주고, 내용이 충돌하는 중복 제출은 거부합니다. 수락한 재개 요청은 실행을 예약하기 전에 영속화하므로, 프로세스가 크래시한 뒤 폴링하면 `NodeInterrupt` 체크포인트에서 재개가 다시 시작되며, 이미 성공한 다른 워커는 반복 실행되지 않습니다.

### 외부 효과와 조정(reconciliation)

일반적인 host-brokered 계약은 하위 호환됩니다. `executor.effect`가 없는 카탈로그 항목은 프로세스를 재시작한 뒤에도 `awaiting_tool_results` 상태를 유지하고, 같은 `{run_id, call_id, result}` 재개 요청을 받습니다.

외부에서 볼 수 있는 비멱등(non-idempotent) 변경을 일으킬 수 있는 호스트 capability에는 그 위험을 명시적으로 선언해야 합니다. effect 메타데이터는 기본인 `host_brokered`의 `tool_result` 상호작용에서만 유효하며, 입력 수집용 메타데이터가 아닙니다.

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

그러면 대기 중인 호출에 영속 `effect` 객체가 포함됩니다. `effect_id`와 `idempotency_key`는 Harness run 범위에서 유효하며 provider의 tool-call ID와는 다릅니다. `status_query`와 `fencing`은 호스트의 capability를 기술하는 값입니다. Harness는 이를 기록할 뿐 provider별 조회나 재시도 프로토콜을 임의로 만들지 않습니다.

`idempotency: "unsupported"` 호출이 아직 대기 중일 때 서비스가 다시 연결되면, 그 run만 `ambiguous_effect` 상태로 바뀝니다. 프로세스가 멈추기 전에 호스트가 효과를 이미 수행했을 수도 있지만 Harness가 어느 쪽인지 증명할 수 없다는 뜻입니다. 간단한 상태에는 `pending`과 `ambiguity`가 포함되고 저널에는 `host_brokered.effect.ambiguous`가 기록됩니다. Harness는 도구를 재생(replay)하지 않으며 효과를 실패나 완료로 보고하지도 않습니다.

호스트가 자신의 기준 시스템(authoritative system)에서 결과를 확인한 뒤 `neograph_resume`으로 모호함을 해소합니다.

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"completed","result":{"answer":"validated host result"}}
```

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"failed"}
```

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"unknown"}
```

`completed`는 `result`를 검증하고 소비한 뒤 체크포인트에서 재개합니다. `failed`는 워커를 다시 실행하지 않고 Harness의 종료 실패로 기록합니다. `unknown`은 run을 `ambiguous_effect` 상태로 두어 나중에 조정하게 합니다. 내용이 완전히 같은 completed, failed, unknown 중복 제출은 멱등으로 처리하고, 충돌하는 completed나 failed 제출은 거부합니다. 중복이 아닌 조정은 모두 `host_brokered.effect.reconciled`로 저널에 기록됩니다.

모호한 효과는 의도적으로 취소할 수 없고 만료되지도 않습니다. 취소나 타임아웃으로는 외부 효과가 실제로 일어났는지 알 수 없기 때문입니다. 기준 시스템에서 아직 판단할 수 없다면 `unknown`을 제출합니다.

이 프로토콜은 호스트가 크래시한 경우에 정확히 한 번(exactly-once) 전달됨을 보장하지 않습니다. 멱등성 키나 상태 조회를 지원하는 호스트는 조정을 제출하기 전에 그 기능으로 실제 결과를 확인해야 합니다.

run 스냅샷에는 `created_at`, `updated_at`, `expires_at`, `poll_after_ms`가 들어 있습니다. 기본 TTL은 24시간, 기본 폴링 간격은 1초이며, 임베딩에서는 `HarnessServiceConfig`로 둘 다 바꿀 수 있습니다.

## 실험적 Tasks 프로파일

구현되어 있지 않습니다. 선택적으로 켜는 MCP Tasks 프로파일은 존재하지 않습니다. Harness 서버는 tasks capability를 알리지 않으며, `HarnessServiceConfig::enable_experimental_tasks`와 `NEOGRAPH_HARNESS_EXPERIMENTAL_TASKS`는 아무 효과가 없고, `tasks/get`, `tasks/update`, `tasks/cancel`은 `-32601`로 응답합니다. 오래 걸리는 run은 안정적인 `run_id`와 `neograph_get` 폴링 계약을 사용합니다.

## Capability 백엔드

`make_provider_harness_executor`는 모든 NeoGraph `Provider`로 워커를 구동합니다. 모델이 선언된 도구를 요청하면 executor는 디스패치 전에 인자를, 디스패치 후에 출력을 카탈로그에 맞게 검증합니다.

provider executor는 타입이 있는 `ProviderRequest`를 만듭니다. `prepare`는 I/O 없이 검증하고 인코딩하며, `dispatch`는 준비된 요청을 소비합니다. `invoke`는 이 두 단계를 합친 것입니다. executor는 실패를 텍스트만 있는 성공으로 바꾸지 않고, 순서가 있는 메시지/파트와 부분 실패를 포함한 소유 SDK Completion/Failure 결과를 그대로 보관합니다. provider 보고서에서 알 수 없는 사용량은 null 가능 필드로 남기며, 보수적으로 산정한 토큰 청구와 예약은 별도의 권한입니다. 기록된 provider 재생(replay)은 이 결과에 다시 청구하거나 정산하지 않습니다. 재생(replay) 중에 새로 발생하는 CPU/Core 작업은 여전히 넘겨받은 잔액을 소비합니다.

초기화된 다운스트림 `MCPClient` 인스턴스에는 `make_mcp_harness_capability_executor`를, A2A 에이전트에는 `a2a::make_harness_capability_executor`를 사용합니다. 권한의 기준은 계속 요청입니다. 워커는 자신의 `tools` 배열에 나열된 도구 ID만 볼 수 있습니다.

파일 시스템 도구에서는 경로를 받는 모든 입력을 `path_arguments`에 선언하고 `policy.workspace_roots`를 설정합니다. 상대 경로는 첫 번째 루트 아래에서 해석합니다. 설정된 모든 루트 밖을 가리키는 정규 경로는 디스패치 전에 거부되며, 기존 심볼릭 링크를 통한 탈출도 포함됩니다. capability 백엔드에는 모델이 쓴 경로 표기 대신 정규 경로를 전달합니다. 다운스트림 MCP와 A2A 서비스는 별도의 신뢰 경계이므로, 파일 시스템의 time-of-check/time-of-use 경쟁을 막으려면 같은 루트 정책을 적용해야 합니다. `policy.read_only: true`이면 `read_only: true`로 표시되지 않은 카탈로그 항목이 하나라도 있을 때 컴파일이 거부됩니다.

## 배포와 프로토콜 프로파일

지원하는 로컬 배포 경로는 위에서 설명한 설치 가능한 `neograph-harness-mcp` 바이너리입니다. 소스 빌드에서는 계속 예제 타깃을 쓸 수 있고, Python wheel은 원격 데몬을 암묵적으로 설치하지 않는 라이브러리/런타임 패키지로 유지됩니다. MCPB와 공식 레지스트리 게시는 릴리스·검색용 패키징 선택지로 남아 있습니다. 와이어 프로토콜에는 필요하지 않으며, 서명된 릴리스 아티팩트와 명시적인 원격 인증 배포 manifest가 있을 때에만 추가해야 합니다.

NeoGraph는 날짜가 붙은 MCP `2025-11-25` 프로파일을 공개합니다. 클라이언트가 `2025-06-18`, `2025-03-26`, `2024-11-05`를 요청하면 그 리비전이 정의한 필드로 응답하고, `2025-03-26`과 `2024-11-05` 클라이언트가 보내는 JSON-RPC 배치도 받아들입니다. 현재 MCP 리비전인 `2026-07-28`은 상태를 저장하지 않으며(`initialize`와 세션이 없고, 모든 요청의 `_meta`에 버전이 들어 있습니다) 제공하지 않습니다. 두 시대를 모두 지원하는 공식 클라이언트는 `2025-11-25`로 폴백하고, `2026-07-28`로 고정한 클라이언트는 연결할 수 없습니다.
