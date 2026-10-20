<!-- neograph-i18n: source=docs/reference-en.md locale=ko source_sha256=b36f5252664d1d0c4ec3acfc047d7c0edccc64c4bf7436974d4ef8f42f34d710 -->
# NeoGraph API — 내러티브 투어

**Languages:** [English](reference-en.md) | [한국어](reference-ko.md) | [日本語](reference-ja.md) | [简体中文](reference-zh-CN.md)

이 문서는 NeoGraph 공개 API를 안내하는 **내러티브 투어**이며,
완전한 참조 문서가 아니다. 실제 에이전트를 만들 때 만나게 될 모듈을
기초 타입 → 제공자/도구 인터페이스 → 그래프 타입 → 엔진 → 체크포인트 저장소 →
provider/tool interfaces → graph types → engine → checkpoint store →
다중 LLM → MCP 순서로 살펴본다. provider 절은 typed 전환을 설명하며
공개 헤더가 기준이다. 다만 다음 모듈은
(`neograph::a2a`, `neograph::acp`, `neograph::async`,
`SqliteCheckpointStore`, `PostgresCheckpointStore`,
`NodeCache`, `AsyncTool`, `create_deep_research_graph`)
이 투어에서 다루지 않는 **헤더 공개 API를 포함한다**.

> **위의 모든 모듈을 포함해 타입별 전체 API 표면을 보려면, 아래에 연결된
> `include/neograph/`의 공개 헤더를 사용하라. 이 내러티브 투어는 권장
> 진입점이고, 헤더가 정식 참조 문서다.**

이렇게 나누면 내러티브는 끝까지 읽을 수 있을 만큼 작게 유지되고,
상세 참조는 `include/neograph/`의 구현과 함께 유지된다.

**모듈 한눈에 보기:**

| 모듈 | 네임스페이스 | 설명 | 투어 | 헤더 |
|--------|-----------|-------------|------|---------|
| 핵심 | `neograph` | 기초 타입, Provider와 Tool 인터페이스 | [§1–§3](#1-foundation-types) | [Provider](../include/neograph/provider.h) |
| 그래프 | `neograph::graph` | 그래프 엔진, 노드, 상태, 체크포인트, 저장소 | [§4–§11](#4-graph-types) | [GraphEngine](../include/neograph/graph/engine.h) |
| LLM | `neograph::llm` | LLM 제공자 구현과 Agent | [§12](#12-llm-module) | [Agent](../include/neograph/llm/agent.h) |
| MCP | `neograph::mcp` | Model Context Protocol 클라이언트 | [§13](#13-mcp-module) | [MCPClient](../include/neograph/mcp/client.h) |
| 유틸리티 | `neograph::util` | 동시성 유틸리티 | [§14](#14-util-module) | [RequestQueue](../include/neograph/util/request_queue.h) |
| **A2A** | `neograph::a2a` | 에이전트 간 JSON-RPC 브리지(클라이언트 + 서버 + 스트리밍) | [공개 헤더](../include/neograph/) | [A2AClient](../include/neograph/a2a/client.h) |
| **ACP** | `neograph::acp` | Agent Client Protocol — stdio 기반 편집기↔에이전트 양방향 RPC | [공개 헤더](../include/neograph/) | [ACPServer](../include/neograph/acp/server.h) |
| **Async** | `neograph::async` | Asio HTTP/SSE/WS 도우미, ConnPool, run_sync | [공개 헤더](../include/neograph/) | [WsClient](../include/neograph/async/ws_client.h) |

추가 모듈은 `include/neograph/{a2a,acp,async}/` 공개 헤더를 제공한다. 이 투어는 이 모듈을 다루지 않으므로 정확한 계약은 헤더를 참조한다.

**편의 헤더:** `#include <neograph/neograph.h>`에는 전체 핵심 API와 그래프 엔진 API가 포함된다.

CMake 3.20 이상이 필요하다. Core가 소유 typed provider 계약을 공개하므로 `NEOGRAPH_BUILD_LLM=OFF`여도 SchemaProvider runtime은 필수다. 명시적 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`를 우선하고 없으면 설치된 `SchemaProvider` runtime package를 찾는다. 없고 `NEOGRAPH_FETCH_SCHEMAPROVIDER=ON`(기본)이면 `cmake/NeoGraphSchemaProvider.cmake`에 고정된 불변 GitHub archive를 다운로드한다. Offline build에서는 SDK를 설치하고 `CMAKE_PREFIX_PATH`에 prefix를 지정한 뒤 `-DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`를 준다. Sibling checkout을 추측하거나 제거된 bundled interpreter를 선택하지 않는다. NeoGraph 선택적 HTTP module을 꺼도 SDK runtime의 transport 의존성은 필요하다. Linux/POSIX가 검증된 플랫폼이다. Windows NTFS와 macOS 구현이 있으나 이 SDK로 검증하지 않았으며, WASM provider runtime 지원은 확립되지 않았다.

SDK imported target이 `include/SchemaProvider` include root를 제공한다. 공개 예시는 recipe 전용 helper 없이 `<descriptor/descriptor.h>`, `<runtime/client.h>`, `<neograph/llm/schema_provider.h>`를 직접 사용한다.

설치 SDK package는 정확히 `0.4.0`이어야 하며 interface revision 7 header와 shared-library generation 7이 일치해야 한다. 버전 일치만으로 이전 interface/ABI binary를 승인하지 않는다.

```cmake
find_package(SchemaProvider 0.4.0 EXACT CONFIG REQUIRED COMPONENTS runtime transport)
find_package(NeoGraph CONFIG REQUIRED)
target_link_libraries(app PRIVATE neograph::core neograph::llm SchemaProvider::runtime)
```

---

## 목차

- [1. Foundation Types](#1-foundation-types)
  - [ToolCall](#toolcall)
  - [ChatMessage](#chatmessage)
  - [ChatTool](#chattool)
  - [Owned Outcome](#owned-outcome)
  - [Portable projections](#portable-projections)
  - [ADL Serialization](#adl-serialization)
- [2. Provider Interface](#2-provider-interface)
  - [ProviderRequest / ProviderControls](#providerrequest--providercontrols)
  - [PreparedProviderRequest / ProviderBudgetClaim](#preparedproviderrequest--providerbudgetclaim)
  - [가격 증거와 모델 카탈로그](#pricing-evidence-and-the-model-catalogue)
- [3. Tool Interface](#3-tool-interface)
  - [Tool](#tool)
  - [도구 실패 계약](#tool-failure-contract)
  - [노드 실패 계약](#node-failure-contract)
- [4. Graph Types](#4-graph-types)
  - [ReducerType](#reducertype)
  - [ReducerFn](#reducerfn)
  - [Channel](#channel)
  - [ChannelWrite](#channelwrite)
  - [NodeInterrupt](#nodeinterrupt)
  - [Send](#send)
  - [Command](#command)
  - [RetryPolicy](#retrypolicy)
  - [StreamMode](#streammode)
  - [Edge](#edge)
  - [ConditionalEdge](#conditionaledge)
  - [NodeContext](#nodecontext)
  - [GraphEvent](#graphevent)
  - [GraphStreamCallback](#graphstreamcallback)
  - [NodeResult](#noderesult)
  - [ConditionFn](#conditionfn)
  - [Constants](#constants)
- [5. GraphState](#5-graphstate)
- [6. GraphNode](#6-graphnode)
  - [GraphNode (abstract)](#graphnode-abstract)
  - [LLMCallNode](#llmcallnode)
  - [ToolDispatchNode](#tooldispatchnode)
  - [IntentClassifierNode](#intentclassifiernode)
  - [SubgraphNode](#subgraphnode)
- [7. GraphEngine](#7-graphengine)
  - [EngineConfig and EngineResources](#engineconfig-and-engineresources)
  - [RunConfig](#runconfig)
  - [RunResult](#runresult)
  - [GraphEngine](#graphengine)
- [7b. Engine Internals](#7b-engine-internals)
  - [GraphCompiler](#graphcompiler)
  - [Scheduler](#scheduler)
  - [CheckpointCoordinator](#checkpointcoordinator)
  - [NodeExecutor](#nodeexecutor)
- [8. Checkpoint](#8-checkpoint)
  - [Checkpoint (struct)](#checkpoint-struct)
  - [CheckpointStore](#checkpointstore)
  - [InMemoryCheckpointStore](#inmemorycheckpointstore)
- [9. Store](#9-store)
  - [Namespace](#namespace)
  - [StoreItem](#storeitem)
  - [Store (abstract)](#store-abstract)
  - [InMemoryStore](#inmemorystore)
- [10. Loader](#10-loader)
  - [ReducerRegistry](#reducerregistry)
  - [ConditionRegistry](#conditionregistry)
  - [NodeFactory](#nodefactory)
  - [Built-in Registrations](#built-in-registrations)
- [11. React Graph](#11-react-graph)
- [12. LLM Module](#12-llm-module)
  - [SchemaProvider](#schemaprovider)
  - [Agent](#agent)
- [13. MCP Module](#13-mcp-module)
  - [MCPTool](#mcptool)
  - [MCPClient](#mcpclient)
- [14. Util Module](#14-util-module)
  - [RequestQueue](#requestqueue)
- [Usage Examples](#usage-examples)
  - [Minimal ReAct Agent](#minimal-react-agent)
  - [Custom Graph with Conditional Routing](#custom-graph-with-conditional-routing)
  - [Human-in-the-Loop with Checkpointing](#human-in-the-loop-with-checkpointing)
  - [Dynamic Fan-Out with Send](#dynamic-fan-out-with-send)
  - [Routing Override with Command](#routing-override-with-command)
  - [SchemaProvider Multi-LLM Support](#schemaprovider-multi-llm-support)
  - [MCP Tool Integration](#mcp-tool-integration)

---

## 1. Foundation Types

**Header:** `<neograph/types.h>`
**Namespace:** `neograph`

Core data types shared across all modules. These model the LLM chat protocol:
messages, tool calls, completions, and their JSON serialization.

### ToolCall

Represents a single tool invocation requested by the LLM.

```cpp
struct ToolCall {
    std::string id;         // Unique identifier assigned by the LLM
    std::string name;       // Name of the tool to call
    std::string arguments;  // JSON-encoded string of arguments
};
```

| 필드 | 유형 | 설명 |
|-------|------|-------------|
| `id` | `std::string` | Unique identifier for this tool call (assigned by the LLM) |
| `name` | `std::string` | Name of the tool function to invoke |
| `arguments` | `std::string` | JSON-encoded string containing the call arguments |

### ChatMessage

A single message in a conversation. Covers all roles: system, user, assistant, and tool.

```cpp
struct ChatMessage {
    std::string role;                    // "system", "user", "assistant", or "tool"
    std::string content;                 // Text content of the message
    std::vector<ToolCall> tool_calls;    // Tool calls (assistant messages only)
    std::string tool_call_id;           // ID of the tool call this responds to (tool messages)
    std::string tool_name;              // Name of the tool (tool messages)
    std::string tool_status;
    bool tool_retryable = false;
    bool tool_effect_uncertain = false;
    std::vector<std::string> image_urls; // base64 data URLs or HTTP URLs for Vision
    std::string reasoning;
    json reasoning_details = json::array(); // portable data, not native authority
};
```

| Field | Type | Description |
|-------|------|-------------|
| `role` | `std::string` | Message role: `"system"`, `"user"`, `"assistant"`, or `"tool"` |
| `content` | `std::string` | Text content of the message |
| `tool_calls` | `std::vector<ToolCall>` | Tool calls requested by the assistant (empty for non-assistant messages) |
| `tool_call_id` | `std::string` | ID linking this tool result to its originating tool call |
| `tool_name` | `std::string` | Name of the tool that produced this result |
| `image_urls` | `std::vector<std::string>` | Image URLs for multi-modal/vision messages. Accepts `data:image/...;base64,...` or `https://...` |

### ChatTool

Defines a tool available to the LLM.

```cpp
struct ChatTool {
    std::string name;        // Tool name (unique identifier)
    std::string description; // Human-readable description for the LLM
    json parameters;         // JSON Schema describing the tool's parameters
    std::optional<bool> strict; // OpenAI Responses strict mode; unset keeps the SDK default
};
```

| Field | Type | Description |
|-------|------|-------------|
| `name` | `std::string` | Unique tool name |
| `description` | `std::string` | Description shown to the LLM to explain the tool's purpose |
| `parameters` | `json` | JSON Schema object describing accepted parameters |
| `strict` | `std::optional<bool>` | 이 function 도구의 OpenAI Responses strict 모드. 비워 두면 SDK policy 기본값을 따르며, 이 기본값은 strict 호환 schema(모든 object가 `"additionalProperties": false`를 지정하고 각 property를 `required`에 나열)에만 적용되고 그 밖의 schema는 `"strict": false`로 전송된다. `false`는 그대로 전송되고, 호환되지 않는 schema에 `true`를 지정하면 dispatch 전에 `InvalidRequest`로 실패한다. 다른 family는 `true`를 거부한다. Python: `ChatTool(..., strict=None)` |

### Owned Outcome

공급자 호출은 `sp::runtime::Result`, 즉 `sp::Completion` 또는 `sp::Failure`를 담은 불변 소유 `std::shared_ptr<const sp::Outcome>`를 반환한다. 표시 텍스트만이 아니라 전체 결과를 보존한다. 순서 있는 메시지/파트, native continuation, 존재하는 wire envelope, 순서 있는 raw 관측, 중단 근거와 실제 시도 메타데이터는 호출 및 클라이언트 소멸 후에도 남는다. `input_total`, `output_total`, `total` 같은 사용량 카운터는 `std::optional<sp::Count>`이며 존재하는 count는 `uint64_t value`와 `Evidence`를 가진다. `Usage`에는 stage, quality, conflict도 남는다. 누락은 미상이며 0을 만들어 넣지 않는다. 실패도 원래의 부분 결과를 보존한다. `ProviderFailure::outcome()`과 `ProviderObserverError::outcome()`은 실제 결과를 보존하며 후자의 `cause()`에는 관측자 예외가 남는다.

`sp::Completion::wire_envelope`과 `sp::PartialCompletion::wire_envelope`은 제공자 계열별로 존재 여부가 다르며 비어 있을 수 있다. 없으면 Python 뷰는 `None`을 반환한다. 현재 버퍼링 Chat은 이 필드를 비워 두고 전체 응답 문서를 `raw_events`의 `sp::RawWire{type="chat.completion", payload=document}`로 보존한다(Python에서는 `ProviderRawWire`). 존재 여부와 실제 타입 기반 raw 증거를 확인한다. 폴백으로 봉투를 합성하거나 부분 실패를 성공으로 바꾸지 않는다. 제공자의 비공개 필드를 포함한 소유된 와이어 증거는 제공자 소멸 후에도 보존된 결과에 남는다. 네이티브 추적은 raw 봉투/이벤트와 네이티브 재생/추론을 제외한다. JSON 조회 복사본은 네이티브 권한이나 재정 권한을 부여하지 않는다.

소비자는 전체 타입 기반 메시지/파트, 논리적 역할과 텍스트를 사용하고, 이어서 실행하는 데 필요하면 실제 네이티브 소유권을 유지해야 한다. 체크포인트와 Chat 요청의 텍스트 content는 텍스트 문자열 또는 유효한 타입 기반 텍스트 파트 배열로 표현할 수 있다. 우연히 선택된 어느 직렬화 형태도 보편적 계약이 아니다.

실제 `NativeContext` 재생에는 원래 `request.messages`의 전체 prefix를 보존하고 직접 반환된 `outcome.messages`를 네이티브 소유권을 유지한 채 순서대로 이어 붙여야 한다. 직접 반환된 결과에는 새로 반환된 메시지만 있으며 원래 요청 이력은 없다. 그 결과의 assistant 메시지만 재생하면 와이어 I/O 전에 `ReplayIneligible`로 거부된다. `NativeArchive`에서 복원한 assistant도 원래의 전체 prefix가 필요하다. 아카이브 보관 권한은 이력 계보 검사를 대신하지 않는다. 그래프의 `RunResult.native_messages`에는 이미 전체 이력이 있으므로 원래 입력을 다시 앞에 붙이지 않는다.

`UsageAccumulator::snapshot()`은 누적 보고를 반환한다. `total_tokens_wide()`는 청구 토큰과 미해결 예약의 합이며 보고 사용량으로 표시하면 안 된다. 정산에는 input/output count가 있는 final·consistent 보고가 필요하며 근거가 있는 가장 큰 total을 차감하고 초과 사용량도 clamp하지 않는다. 누적 보고 중 하나라도 counter가 없으면 합계도 미상이다. 예약, 로컬 차감, vendor 청구서는 서로 다른 기록이다.

실제 결과 이후 post-effect 정산이나 terminal receipt 영속화가 실패하면 `ProviderDispatchOutcomePersistenceError`의 `outcome()`은 원래 불변 결과를, `cause()`는 원래 영속 예외를 보존한다. 전달도 실패했으면 `delivery_error()`가 원래 관측자 예외를 보존한다. 영속화 성공 뒤 관측자 실패는 원래 예외를 그대로 다시 던진다. 미상/결과 없는 transport 실패는 결과를 조작하지 않는다.

### Portable projections

`ChatMessage` / `ChatTool`과 JSON은 portable projection이지 native 권한이 아니다. Portable 포맷은 [`provider-message-v3`](../schemas/provider-message-v3.schema.json), [`provider-outcome-v3`](../schemas/provider-outcome-v3.schema.json), [`runtime-history-record-v3`](../schemas/runtime-history-record-v3.schema.json)이며 v2 포맷 레코드도 계속 읽을 수 있다. 실제 C++ checkpoint sidecar는 메모리에서 native seal을 보존한다. 영속 native 기록에는 host-owned `sp::NativeArchive`가 필요하다. closed v3 / `spna3`는 독립 키를 쓰는 인증된 owner-private custody이며 archive v2는 업그레이드하거나 해석하지 않고 거부한다. 인증은 모든 semantic descriptor 선택(origin/path/header, policy, 요청 field mapping, usage path, stop mapping), owner와 정확한 custody binding을 결합한다. 암호화나 vendor-issuer 인증은 아니다. archive 본문·키·native blob·raw wire 관측을 공개하지 않는다. Archive는 증거 저장소이지 돈의 grant나 spending lease가 아니다. Program/external bank는 독립 journal 소유이며 snapshot 복사로 credit을 만들 수 없다.

`RuntimeHistoryRecord`의 인자 없는 `serialize_canonical()`은 이식 가능한 기록을 지원한다. 영속 네이티브 이력에는 `serialize_canonical(archive, owner_id)`와 해당 ID에 소유자 범위가 일치하는 아카이브를 사용한다. Python도 같은 오버로드와 `RuntimeHistoryRecord.parse(stored_bytes, archive=None, owner_id="")`를 제공한다. 이식 가능한 기록용 기본 인자는 일치하는 아카이브 없이 네이티브 복원을 허가하지 않는다. Python의 파싱과 아카이브를 사용하는 직렬화는 네이티브 작업 중 GIL을 해제한다. JSON 관찰 데이터와 아카이브로 인증된 보관 권한은 구별된다.

Python은 `ContextStore.hydrate_records(range)`와 `history_record_by_message_id(feed, message_id)`로 타입 기반 보관 권한을 유지한다. 후자는 기록 또는 `None`을 반환한다. `SQLiteContextStore(database_path, archive=None)`는 실제 아카이브를 받으며 `LocalProgramHost`는 마지막 선택적 인자 `native_history_archive=None`을 `RuntimeConfig`로 전달한다. 이 인자는 권한을 조작하거나 영속 Program 저장소 백엔드를 추가하지 않는다. 반환된 `RuntimeHistoryRecord.message`는 실제 공유 네이티브 소유권을 유지하는 분리된 타입 기반 복사본이다. 이를 변경해도 불변 RAW 식별자는 바뀌지 않는다. 저장소 생성과 타입 기반 조회는 GIL을 해제하며, 보관 권한이 필요한 네이티브 이력은 아카이브 없는 저장소에서 거부한다.

`Assistant` 메시지를 담은 RAW `RuntimeHistoryRecord`에는 `RuntimeTrustClass.ModelOutput`이 필요하다. `RuntimeTrustClass.UntrustedInput`은 `User` 메시지만 허용한다. 이 이름들은 실제 Python enum 이름이다. trust class는 기록의 역할과 출처 경계를 나타내며, 이를 선택해도 네이티브 재생 보관 권한이나 지출·실행 권한은 생기지 않는다.

**독립 실행형 예산 bank.** 독립 실행형 bank는 단조 trusted-store namespace obligation과 실제 불변 original owner/thread/graph scope, ceiling, deadline/clock identity, generation을 요구한다. 전체 checkpoint commitment·revision에 대한 정확한 durable head CAS만 host-owned opaque lease를 발급할 수 있다. 정확한 pending effect window를 provider I/O 전에 영속화해야 하며 실제 SDK outcome, charge, nullable report, hold, dedup identity로 정산해야 한다. Checkpoint와 next head는 같은 owned actor/revision 아래 원자적으로 publish한다. Bank metadata 제거·checkpoint pruning·old authenticated snapshot replay·같은 ID overwrite·actor 상실은 credit을 주지 않는다. 기존 65 hold에서 ceiling 130을 129로 낮추면 추가 65를 허용할 수 없다. 입증된 no-effect 실패는 unchanged head를 release해 authentic 130 복구가 가능하다. Crash/unknown/lost-lease window는 refund/retry/fallback 없이 hold를 유지한다. Plain archive 설정은 money/native spending lease를 주지 않고 현재 `config.usage`는 기존 독립 실행형 obligation을 대체할 수 없다. Program/external bank는 각자의 journal이 소유한다.

**관리형 예산 lease 선언.** `<neograph/graph/checkpoint.h>`는 `owner_scope`, logical `thread_id`, private backend `storage_thread_id`, `graph_identity`, `original_ceiling`, `original_deadline_ticks`, `deadline_clock_identity`를 가진 `ManagedBudgetLeaseScope`를 선언한다. `OwnedManagedBudgetLease`는 read-only `scope()`, `actor_id()`, 불변 `bank_generation()`, `revision()`, `head_checkpoint_id()`, `head_commitment()`를 노출하며 공개 authority-import constructor가 없다. `ManagedBudgetEffectReceipt`는 `active()`, `effect_id()`, `claim_amount()`, `request_digest()`를 노출하고 default receipt는 권한을 주지 않는다. `CheckpointStore`는 `acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`, `begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`, `settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`, `publish_managed_budget_checkpoint(lease, checkpoint)`, `release_managed_budget_lease(lease)`와 `_async` counterpart를 선언한다. Sync `CheckpointStoreCore`와 `AsyncCheckpointStore`는 각각 해당 variant를 제공한다. `managed_budget_checkpoint_commitment(checkpoint)`는 bank JSON뿐 아니라 전체 영속 checkpoint를 결합한다.

**공유 bank fork(InMemory).** 원래 실제 C++ fork는 ONE original financial journal과 trusted current branch head를 쓰며 grant를 복제하지 않는다. `publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)` 및 `_async`는 authentic current source/full commitment와 실제 same-bank native C++ pointer를 요구하고 durable standalone fork는 명시적 unsupported로 남는다. `OwnedManagedBudgetLease::scope()`와 original owner/thread/graph, ceiling, deadline/clock, generation은 불변이다. Read-only store-issued `execution_thread_id()` / `execution_storage_thread_id()`는 execution branch를 별도로 선택하며 `GraphState::budget_original_thread_id()`는 원래 financial bank를 가리킨다. 정확한 selected-branch head CAS와 global actor/revision은 canonical current counter, pending effect, burned identity에 대해 모든 branch를 직렬화한다. Original/fork branch는 보충 없이 계속 사용할 수 있다. Stale snapshot·checkpoint copy·imported JSON은 alias를 발급하거나 head를 되돌릴 수 없다. 저장된 original ceiling은 effective fork ceiling과 별개이며, 이를 넓히거나 JSON만으로 복원하는 것은 거부된다. Unbounded reported observation은 사실 data이지 finite grant가 아니다. 입증된 zero-effect lease만 unchanged head를 release할 수 있고 unknown/pending effect는 obligation을 유지한다.

**Lease release 오류.** `<neograph/graph/engine.h>`의 `graph::ManagedBudgetLeaseReleaseError`는 `ProviderOutcomeError`를 상속한다. `cause()`는 원래 execution exception을 보존하고 `release_error()`는 보조 durable lease-disposition 실패를 노출한다. `outcome()`은 실제 SDK 증거가 있으면 보존하고 SDK outcome이 없으면 null이다. Release 실패는 결과를 만들거나 재dispatch를 허용할 수 없다. Closed `_neograph_managed_budget_scope` metadata는 원래 logical scope/cap/deadline clock/generation을 설명하지만 backend CAS 권한이 아닌 data이다.

**Archive owner와 retention.** Finite standalone root 또는 authenticated finite source만 실제 설정된 `sp::NativeArchive::owner_scope()`에서 생략된 original owner를 상속한다. Unbounded/plain owner metadata 의미는 바뀌지 않는다. 명시적으로 충돌하는 archive owner는 lease acquire 전에 거부한다. `CheckpointStore::retains_native_checkpoint() const noexcept`와 대응 Core/Async storage capability는 기본 false이며 실제 InMemory backend만 true로 override하고 wrapper는 실제 retention을 위임해야 한다. 이 read-only 설명은 정당한 unleased/plain/unbounded C++ native checkpoint custody를 허용하되 spending credit이나 native replay authority를 주지 않는다. Leased custody는 JSON flag나 추측한 store type 대신 실제 store-issued receipt를 사용한다.

**Native-custody gate.** Managed effect begin은 pending-effect/slot/held-window 변경 전에 실제 결합된 NativeArchive 또는 실제 local store-issued private C++ retention capability를 요구한다. Private capability는 JSON에서 import하거나 wire로 전달하지 않는다. C++ sidecar는 경계를 넘을 수 없으므로 remote backend가 InMemory여도 gRPC는 실제 client·server archive를 요구한다. Archive가 finite source owner를 제공하지 않으면 원래 anonymous owner scope는 빈 값으로 유지하고 실제 archive binding은 original scope와 일치해야 한다. Financial head/lease 증거만으로 native-custody readiness를 증명하지 않는다.

진단 JSON은 문법상 유효한 duplicate-key 문서를 포함해 원래 raw byte를 보존하지만 실행 가능한 요청/config admission은 중복을 계속 거부한다. 원래 non-2xx 응답 JSON은 두 번째 손실 parse 없이 `http.error` 증거에 남는다. named SSE error는 뒤의 정상 stream close보다 우선한다. 진단/provider metadata는 무관한 작은 error-text cap이 아니라 승인된 source extent로 제한된다.

`ProviderRequest::observer_limits`는 host-only이다. 명시한 `max_events`·`max_bytes`는 양수여야 하며 승인된 SDK 전달 상한을 낮출 수만 있다. `provider-request/v3` digest는 실제 limit, mode, encoded body, retry policy와 모든 semantic descriptor binding을 결합한다. Bridge는 queued·draining batch 전체에서 실제 PMR vector/map capacity와 소유 event/document byte를 계산하며 queue mutex 밖에서 cancellation을 요청한다. 이름이 `messages`인 Generic channel을 chat으로 강제 변환하지 않는다. native `history` channel을 `messages`에 mapping하면 JSON에서 native 권한을 만드는 대신 C++ sidecar를 보존한다.

`ProviderOutcomeError`는 결과를 보존하는 공통 host-error base이다. `ProviderObserverError`와 `ProviderDispatchOutcomePersistenceError`는 완전히 drain된 SDK 결과와 원래 `cause()`를 보존하며 후자는 보조 observer 실패도 `delivery_error()`에 보존한다. `ProviderFailure::outcome()`은 SDK 실패 자체를 보존한다. 이 증거는 Node/Program 재dispatch 권한이 아니다. Provider retry의 유일한 소유자는 SDK이며 caller가 선택한 `max_output_tokens`를 조용히 clamp하지 않는다.

`ProgramFailure`는 live `provider_outcome`·`provider_cause`를 보존한다. Canonical factual SDK witness는 실제 archive custody를 owner/run/version/bundle/operation/attempt에 결합하며 Runtime은 복구 실패를 노출하기 전에 설정된 custody를 즉시 복원한다. 공개 data-only `ProgramResult::create()`는 미리 채운 witness로 우회할 수 없고 unresolved parsed seal은 실행 결과가 아니다. 프로세스 재시작 후 원래 exception pointer는 없으므로 `provider_cause == nullptr`이며 text에서 재생성하지 않는다. `native_history_archive`가 없는 호스트는 provider의 native replay 상태를 보관할 수 없으므로, native 메시지가 든 실패를 그 상태 없이 발행한다. 메시지, usage, stop reason, error, raw event는 그대로 두고 메시지마다 native seal만 뺀 사본이며, witness에 `provider_outcome_native_retained: false`가 들어간다. 결과의 `provider_outcome`은 이 사본이고, 호출을 실행한 프로세스에서는 원래 outcome이 `provider_cause`에 남는다. 그 밖의 이유로, 곧 archive 쓰기가 실패해 영속화할 수 없는 실패는 serialize/publish/replay할 수 없다.

Python `LocalProgramHost`는 소멸 시 `ProgramRuntime`이 스케줄러 작업을 취소하고 비운 뒤 join하는 동안 호출자의 GIL을 해제하여 실행 중인 Python 노드가 끝날 수 있게 한다. 나머지 host 멤버와 이들이 소유한 Python 콜백/객체를 소멸하기 전에 GIL을 다시 획득한다. 이 변경은 종료 처리에만 적용되며 추가 capability binder나 실행 권한을 공개하지 않는다.

`RecordedBindingSet`는 source-bound move-only data이지 caller가 제공하는 dispatcher가 아니다. 신뢰된 Catalog `recorded_capability_binder`는 실제 영속 source event를 독립적으로 읽어 captured-only capability를 materialize한다. `ProgramRuntime::replay_recorded()`는 원래 selected-source permission을 검사한 뒤 실제 남은 bank를 durable CAS로 이전한다. inherited spend는 새 model grant가 아니다. 구 `start_recorded` 갱신 API는 제거되었다. InMemory/File/SQLite/PostgreSQL Program store는 실행 내내 정확하고 불변인 owned lease를 보존하며 expiry로 갱신하지 않는다. Controlled JavaScript도 underlying capability manifest를 검사하고 정확한 completed command 결과를 소비하며 external effect를 재dispatch하지 않는다.

**Recorded replay 정산.** Captured command replay는 실행 전에 새 CPU wall-time/Core work만 durable reserve하고 측정 work와 새 Core checkpoint를 result CAS로 publish한다. 새 model·money·Program-operation allowance를 소비하지 않고 captured external effect를 재dispatch하지 않는다. 미정산 reservation은 debit을 유지한다. Await channel receive·timer wait/cancel·handoff wait 시작/release는 소유 executor/strand에서 직렬화한다.

```cpp
#include <neograph/types.h>

neograph::json observe_result(const sp::runtime::Result& result) {
    if (!result) throw std::invalid_argument("Missing provider outcome");
    return neograph::outcome_projection_json(*result);
}
```

### ADL Serialization

Argument-Dependent Lookup (ADL) serialization functions for nlohmann/json integration.
These allow direct use with `json j = my_tool_call;` and `my_tool_call = j.get<ToolCall>()`.

```cpp
void to_json(json& j, const ToolCall& tc);
void from_json(const json& j, ToolCall& tc);

void to_json(json& j, const ChatMessage& msg);
void from_json(const json& j, ChatMessage& msg);
```

ADL serialization은 portable message/tool field와 선언된 기본값을 보존한다. JSON으로 native SDK continuation 권한을 재구성하지 않는다.

---

## 2. Provider Interface

공개 계약은 소유 typed 준비/dispatch이며 동기·비동기 가상 completion 쌍이 아니다. `ProviderRequest.payload`는 Chat, Messages, Responses, Gemini, Interactions의 SDK 요청 variant이다. `ProviderMode::Collect` / `Stream`은 관측자 유무와 독립적으로 전송을 선택한다. `on_event`는 빌린 typed `sp::Event` view를 받는다. 콜백 이후 필요한 데이터만 복사한다. raw JSON override나 portable projection을 통한 native 권한 가져오기는 허용되지 않는다.

```cpp
#include <neograph/provider.h>
#include <neograph/runtime_interposition_consumer.h>
#include <neograph/controlled_provider.h>

// Selected public declarations from neograph::Provider.
class Provider {
public:
    virtual ~Provider() = default;
    virtual std::string get_name() const = 0;
    virtual std::string_view family() const noexcept = 0;
    virtual PreparedProviderRequest prepare(ProviderRequest request) = 0;
    sp::runtime::Result dispatch(PreparedProviderRequest request);
    asio::awaitable<sp::runtime::Result> dispatch_async(PreparedProviderRequest request);
    sp::runtime::Result invoke(ProviderRequest request);
    asio::awaitable<sp::runtime::Result> invoke_async(ProviderRequest request);
    static std::string request_digest(const PreparedProviderRequest& request);
    static std::optional<std::uint64_t> conservative_token_upper_bound(
        const PreparedProviderRequest& request);

    // SchemaProvider endpoints at the provider's origin (#531).
    template <EndpointRequest Request>
    sp::runtime::PreparedEndpoint<EndpointOutcomeOf<Request>> prepare_endpoint(
        Request request, sp::runtime::RunOptions options = {});
    template <class Outcome>
    Outcome complete_endpoint(sp::runtime::PreparedEndpoint<Outcome> prepared);
    template <EndpointRequest Request>
    EndpointOutcomeOf<Request> complete_endpoint(Request request, sp::runtime::RunOptions options = {});
    template <class Outcome>
    asio::awaitable<Outcome> complete_endpoint_async(sp::runtime::PreparedEndpoint<Outcome> prepared);
    template <EndpointRequest Request>
    asio::awaitable<EndpointOutcomeOf<Request>> complete_endpoint_async(
        Request request, sp::runtime::RunOptions options = {});
};
```

### 엔드포인트 호출

`Provider`는 자기 origin의 SchemaProvider 비채팅 엔드포인트도 전달한다.
- 이미지, decisions, 임베딩, rerank, moderation
- 음성 합성, 전사, 번역
- 비디오 job(제출, 조회, 다운로드)
- 파일(통째 업로드와 이어 올리기, 조회, 목록, 삭제, 다운로드)
- 배치, 토큰 수 세기, OpenRouter 생성 통계

`EndpointOutcomeOf<Request>`는 요청마다 타입이 정해진 결과를 가리킨다. 일회성 엔드포인트는 `std::variant<Result, sp::Failure>`이고, job·파일·배치는 각자의 상태다.

호출은 provider 자신의 클라이언트에서 실행되며, 채팅 호출과 같은 자격 증명, admission 슬롯, 재시도 예산, deadline, 오류 분류를 쓴다. descriptor 정책은 호출을 준비할 때 origin별로 각 엔드포인트를 승인한다. 정책이 거부한 호출은 그 오류를 담고, 네트워크에 닿지 않은 채 `Failure`로 끝난다.

```cpp
#include <neograph/llm/schema_provider.h>

std::vector<std::vector<float>> embed(neograph::Provider& provider, std::vector<std::string> texts) {
    sp::embeddings::Request request;
    request.model = "text-embedding-3-small";
    for (auto& text : texts) request.inputs.push_back({std::move(text)});
    auto outcome = provider.complete_endpoint(std::move(request));
    if (const auto* failure = std::get_if<sp::Failure>(&outcome))
        throw std::runtime_error(failure->error.safe_message);
    return std::get<sp::embeddings::Result>(std::move(outcome)).vectors;
}
```

- **준비.** `prepare_endpoint`는 SDK의 `PreparedEndpoint`를 돌려준다. 호스트는 `complete_endpoint`가 실행하기 전에 이를 살펴보거나(`prepared().endpoint()`, `prepared().model()`, `error()`) 저널에 남길 수 있다. 준비 슬롯이 비어 있지 않으면 `sp::runtime::AdmissionError`를 던진다.
- **스레드를 막지 않는 호출.** `complete_endpoint_async`는 스레드를 막지 않는다. awaitable이 클라이언트와 호출을 소유하므로 요청도 provider도 그보다 오래 살 필요가 없다. 기다리는 코루틴을 취소하면 호출이 취소되고, 결과는 SDK의 `Cancelled` 실패다.
- **주어진 그대로 전송.** `SchemaProvider::Defaults`는 채팅 호출에만 적용된다. 여기서는 어떤 호출도 저널에 남기지 않고, 예산을 예약하거나 차감하지도 않는다.
- **엔드포인트가 없는 provider.** SchemaProvider 엔드포인트가 없는 `Provider`는 `std::logic_error`를 던진다. 하위 클래스는 `endpoint_runtime()`을 재정의해 자기 클라이언트를 전달하고, 다른 provider를 감싸는 provider는 `endpoint_runtime_of(inner)`를 반환한다. `has_endpoints()`는 provider에 엔드포인트가 있는지 알려 준다.
- **채팅 경로에서.** `make_endpoint_request(request)`는 엔드포인트 요청을 `ProviderRequest`에 담으므로, `prepare`, `dispatch`, `invoke`가 이를 채팅 호출과 똑같이 처리한다: dispatch Hook, 관측자, provider 호출 broker와 그 저널, 토큰 예산. 결과는 타입이 없으며, 엔드포인트의 `typed()`(`sp::embeddings::typed`, ...)가 결과에 타입을 부여한다. 호출은 버퍼링되므로 `ProviderMode::Stream`은 거부되며, 로컬 provider(`prepare_local`)도 이를 거부한다. dispatch identity(`request_digest`)에는 엔드포인트, 메서드, URL도 들어간다. 하나의 엔드포인트를 여러 연산이 공유하고, 읽기에는 본문이 없기 때문이다. 업로드의 identity에는 인코딩된 본문이 담을 수 없는 것도 들어간다. 업로드가 보내는 바이트의 SHA-256, 오프셋, 크기이고, 파일 전체 업로드(`sp::files::Upload`)라면 파일 이름, MIME 타입, purpose, 만료 시간도 들어간다. 바이트는 메모리에서 오든 파일 조각에서 오든 호출을 prepare할 때 해시된다. 파일 경로는 identity에 포함되지 않으므로, 내용이 바뀐 파일은 다른 호출이 되고 옮기기만 한 파일은 같은 호출이다. 파일이 없거나 조각보다 짧으면 호출을 prepare할 때 거부된다(`ErrorKind::InvalidRequest`). 엔드포인트 읽기(`PreparedProviderRequest::is_endpoint_read()`: job 폴링이나 다운로드, 파일이나 배치 조회 같은 GET)는 과금되지 않고 다시 보내도 안전하므로, 저널을 남기는 broker는 이를 replay하지 않고 매번 보낸다.

### ProviderRequest / ProviderControls

```cpp
#include <neograph/llm/schema_provider.h>
#include <neograph/types.h>

sp::runtime::Result call_provider(
    neograph::Provider& provider, std::string model,
    std::vector<sp::Message> history,
    std::function<void(const sp::Event&)> observer) {
    neograph::ProviderControls controls;
    controls.max_output_tokens = 128;  // optional caller-selected wire cap
    auto request = neograph::make_provider_request(
        provider, std::move(model), std::move(history), {},
        std::move(controls), neograph::ProviderMode::Stream);
    request.on_event = std::move(observer);
    auto prepared = provider.prepare(std::move(request));
    return provider.dispatch(std::move(prepared));  // owns Completion or Failure
}
```

#### Typed controls

`make_provider_request(provider, model, messages, tools, controls, mode)`는 닫힌 family 하나를 선택한다. 이식 가능한 제어는 `ProviderControls`의 멤버이고 family 고유 제어는 그 family의 nest에 있으며, 호출은 자기 family의 nest만 적용하므로 `ProviderControls` 하나로 서로 다른 family를 부르는 노드를 모두 처리할 수 있다. family가 표현할 수 없는 이식 가능한 제어는 `std::invalid_argument`로 거부하고 SDK prepare가 enum 값, 승인 origin, 모델 규칙, 도구 선언, native binding을 I/O 전에 검사한다. 타입 없는 값은 nest의 `extensions`로만 보내며, SDK 정책이 origin별로 값 스키마와 함께 승인한다. 0.15의 평평한 family 멤버(`chat_reasoning`, `responses_include`, `messages_tool_choice`, `gemini_thinking_level` 등)는 0.16.0에서 nest로 옮겨지는 폐기 예정 별칭이며, 다른 family는 0.15처럼 이를 거부하고, 별칭과 nest 멤버를 함께 지정하면 예외가 난다. optional 값은 미지정과 명시적 `false`·빈 선택을 구분한다.

| Family | 이식 가능한 `ProviderControls` 필드와 SDK 매핑 | Family nest(`controls.<nest>`) |
|---|---|---|
| `openai.chat` | `max_output_tokens`, `temperature`, `top_p`, `reasoning_effort`, `service_tier`, `provider`, `response_format` | `chat`: `reasoning`, `include_reasoning`, `usage_include` → `usage.include`, `models`, `modalities`, `audio`, `image_config`, `plugins`, `extensions` |
| `openai.responses` | `max_output_tokens`, `max_tool_calls`, `temperature`, `top_p`, `reasoning_effort`/`reasoning_summary` → `reasoning`, `service_tier`, `required_tool`, `provider`, `response_format`, `store`, `system` → `instructions`, `account_scope` | `responses`: `previous_response_id`, `previous_response_history`, `parallel_tool_calls`, `verbosity` → `text.verbosity`, `truncation`, `include`, `include_names`, `hosted_tools` → `tools`, `modalities`, `image_config`, `plugins`, `mcp_approvals`, `extensions` |
| `anthropic.messages` | `max_output_tokens` → `max_tokens`, `temperature`, `top_p`, `thinking_budget`, `system`, `account_scope`, `provider` | `messages`: `thinking_mode`, `output_effort` → `output_config.effort`, `cache_control`, `tool_choice`, `tools` (server and typed client tools, after the portable tools), `container`, `skills`, `betas`, `mcp_servers`, `extensions` |
| `google.generate` | `max_output_tokens`, `temperature`, `thinking_budget`, `include_thoughts`, `required_tool`, `system`, `account_scope` | `generate`: `history_mode`, `thinking_level`, `safety_settings`, `tool_choice`, `response_modalities`, `speech_config`, `image_config`, `audio_transcription_config`, `audio_format`, `builtin_tools`, `mcp_servers`, `server_tool_invocations`, `retrieval_config`, `extensions` |
| `google.interactions` | `max_output_tokens`, `thinking_level` (optional string), `thinking_summaries`, `service_tier`, `required_tool`, `store`, `system`, `account_scope` | `interactions`: `response_format`, `speech_config`, `transcription_config`, `builtin_tools`, `previous_interaction_id`, `video_task`, `agent`, `background`, `extensions` |

Chat의 `sp::chat::ReasoningOptions`는 optional `effort`, `max_tokens`, `exclude`, `enabled`를 담는다. 이 nested reasoning object, `include_reasoning`, `usage_include`, 대체 `models`는 policy가 선언한 OpenRouter origin에서만 허용된다. `sp::OpenRouterRouting`도 Chat/Responses/Messages의 선언된 OpenRouter origin에서만 지원한다. gateway 형태 모델 이름이 다른 origin을 승인하지 않는다. Responses hosted tool(`controls.responses.hosted_tools`)과 Messages 서버·타입 클라이언트 도구(`controls.messages.tools`)는 nest 안의 타입 있는 SDK 값이다.

Responses SSE는 종료 이벤트 `response.completed` 또는 `response.incomplete`를 요구합니다. `response.done`은 같은 완전한 종료 Responses envelope(completed 또는 incomplete)를 담은 별칭일 때만 허용되며, 종료가 아니거나 형식이 잘못된 `response.done`은 프로토콜 오류로 실패합니다. `response.content_part.delta`를 비롯해 목록에 없는 이벤트는 `Unsupported`입니다. 선언된 OpenRouter origin에서는 Responses와 Messages가 `~vendor/model-latest` 같은 라우팅 별칭에 대해 게이트웨이가 보고하는 실제 모델명도 받아들입니다. 서빙 모델은 한 응답 안에서 바뀌면 안 되고, replay는 요청한 모델에 묶인 채로 유지됩니다.

| SDK 타입 | 닫힌 값 또는 멤버 |
|---|---|
| `sp::responses::Verbosity` | `Low`, `Medium`, `High` |
| `sp::responses::Truncation` | `Disabled`, `Auto` |
| `sp::responses::Include` | `ReasoningEncryptedContent`, `WebSearchSources`, `FileSearchResults`, `MessageOutputTextLogprobs`, `ComputerCallOutputImageUrl`, `CodeInterpreterCallOutputs` |
| `sp::messages::ThinkingMode` | `Manual`, `Adaptive`, `Disabled` |
| `sp::messages::OutputEffort` | `Low`, `Medium`, `High`, `Max`, `XHigh` |
| `sp::messages::CacheControl` / `CacheTtl` | optional `ttl`: `FiveMinutes`, `OneHour`; wire type `ephemeral`, optional TTL `5m`/`1h` |
| `sp::messages::ToolChoice` / `ToolChoiceMode` | `mode`: `Auto`, `Any`, `None`, `Tool`; `name`; optional `disable_parallel_tool_use` |
| `sp::gemini::HistoryMode` | `NativeOnly`, `PortableForeign` |
| `sp::gemini::ThinkingLevel` | `Minimal`, `Low`, `Medium`, `High` |
| `sp::gemini::SafetySetting` / `SafetyCategory` | `category`: `Harassment`, `HateSpeech`, `SexuallyExplicit`, `DangerousContent`, `CivicIntegrity`; `threshold`: `SafetyThreshold` |
| `sp::gemini::SafetyThreshold` | `BlockNone`, `BlockOnlyHigh`, `BlockMediumAndAbove`, `BlockLowAndAbove`, `Off` |
| `sp::gemini::ToolChoice` / `ToolChoiceMode` | `mode`: `Auto`, `Any`, `None`, `Validated`; `allowed_function_names`: 선언된 함수 이름 vector |

Messages는 양수 output cap이 필요하다. mode 없이 thinking budget을 주면 `Manual`이며 승인 minimum 이상, cap 미만이어야 한다. `Adaptive`/`Disabled`는 명시적 budget을 거부한다. Manual/adaptive는 그 외 유효한 temperature를 생략하고 thinking `top_p` minimum을 검사하지만 모델별 temperature 금지는 무시하지 않는다. 강제 `Any`/`Tool`은 도구와 disabled thinking이 필요하다. `Tool` 이름은 선언된 client tool이어야 하고 다른 mode는 `name`을 거부하며 `None`은 `disable_parallel_tool_use`를 거부한다. Generate의 thinking budget/level과 `required_tool`/`generate.tool_choice`는 각각 동시 사용 불가다. allowed-function 목록은 선언된 함수를 가리킨다. sampling은 family/policy 범위를 따른다. Generate에는 `top_p`가 없고 Interactions에는 두 sampling 필드 모두 없다.

`FamilyPolicy.temperature_forbidden_model_prefixes`는 전체 모델과 마지막 `/` 뒤 suffix를 ASCII 대소문자 무시로 비교한다. 내장 Chat/Responses prefix는 `gpt-5`, `gpt-6`, `o1`, `o3`, `o4`; Messages는 `claude-opus-4-7`, `claude-opus-4-8`, `claude-opus-5`, `claude-sonnet-5`, `claude-fable-`다. 명시적 금지 temperature는 gateway prefix 모델에서도 I/O 전에 거부한다.

#### Responses provider-held continuation

`previous_response_id`는 provider 보관 상태를 선택한다. 요청 `messages`는 NEW INPUT ONLY이며 과거 대화 전체를 다시 보내지 않는다. `previous_response_history`는 전송되지 않는 로컬 소유 증거 `std::vector<sp::Message>`다. cursor만으로 새 text/image 입력은 허용할 수 있지만 client tool result를 승인하지 못한다. 최초 captured response에는 authentic original prefix와 ID가 cursor인 terminal assistant response를 함께 준다. 이후 authentic in-process cursor-produced terminal response는 전체 prefix 재구성 없이 private completed tool ownership을 가질 수 있다. content/origin/model/route/config 불일치는 거부하며 임의 ID나 projection JSON으로 ownership을 만들 수 없다.

server cursor와 private completed ownership은 `NativeReplay`·native archive 권한을 주지 않는다. `responses_include` 미지정은 `reasoning.encrypted_content`를 유지하고 명시적 빈 vector는 `[]`를 보낸다. 다른 명시적 선택도 그대로 적용한다. 이후 작업이 native evidence를 요구하면 근거 누락은 정직하게 실패한다.

#### Explicit portable Gemini history

Generate 기본은 `NativeOnly`다. 명시적 `PortableForeign`은 native seal, `wire_output`, signature/native metadata가 없는 caller-created assistant `Text`/`ToolCall`만 허용한다. 최초 foreign `functionCall`에만 Google의 `skip_thought_signature_validator`를 붙이고 text-only turn에는 signature를 만들지 않는다. authentic native group은 원래 provenance/content/binding 검증을 유지한다. 손상·불일치 native group을 portable로 강등하지 않으며 imported foreign history는 native replay 권한을 얻지 않는다.

#### Output caps and native continuation

output generation cap은 호출별 승인 resource이며 native replay config digest에서 이 cap만 제외한다. content/prefix, origin/route, policy identity, tools, reasoning controls 등 다른 binding은 유지한다(문서화된 per-turn tool selection/cursor 동작 제외). 각 encoded/prepared request는 effective cap을 보존하며 request digest, journal slot, 원래 shared bank와 절대 deadline은 계속 구속한다. cap을 높인 semantic call은 같은 grant 아래 새 call ordinal·admission이 필요하다. seal repair, deadline 갱신, 이전 effect replay가 아니다. native archive는 `spna3`/v3 그대로이고, portable JSON은 v3로 기록하며 v2 레코드도 계속 읽을 수 있다.

Python은 `ChatReasoningOptions`, `ResponsesVerbosity`, `ResponsesTruncation`, `ResponsesInclude`, `MessagesThinkingMode`, `MessagesOutputEffort`, `MessagesCacheControl`, `MessagesCacheTtl`, `MessagesToolChoice`, `MessagesToolChoiceMode`, `GeminiHistoryMode`, `GeminiThinkingLevel`, `GeminiSafetySetting`, `GeminiSafetyCategory`, `GeminiSafetyThreshold`, `GeminiToolChoice`, `GeminiToolChoiceMode`를 제공한다. enum 값은 같되 C++ `None`은 Python `None_`다. optional class control과 message/safety/history vector는 detached snapshot이므로 수정 후 재대입한다. `ProviderControls.previous_response_history`는 authentic `ProviderMessage` 목록이며 provisional 단일 response가 아니다.

#### Deployment header preprocessing

일반 `sp::descriptor::load(source[, policy])`는 `${VAR}`까지 literal로 취급한다. 명시적 `load_with_environment_headers(source, overrides = {}, policy = {})`는 Messages용 optional `ANTHROPIC_WORKSPACE_ID`/`ANTHROPIC_BETA`를 읽고 unset/empty는 생략한다. deterministic `load_with_deployment_headers(source, overrides, DeploymentHeaderEnvironment, policy)`는 주어진 optional `anthropic_workspace_id`/`anthropic_beta`를 사용한다. 둘 다 `LoadResult`를 반환하며 descriptor admission 전에 처리한다. 대소문자 무시 우선순위는 environment < descriptor literal headers < explicit overrides다. 중복 override, invalid/reserved name, 줄바꿈은 admission에서 거부한다. 승인 뒤 env 평가나 header 변경은 없다.

Python 이름은 `ProviderDeploymentHeaderEnvironment`, `load_provider_descriptor_with_environment_headers(source, overrides=[], policy=None)`, `load_provider_descriptor_with_deployment_headers(source, overrides, environment, policy=None)`다. loader는 `ValidatedDescriptor`를 반환하며 admission 실패 시 예외를 낸다. credential은 runtime options에 둔다.

#### Additional admission and event rules

Chat nested reasoning은 비어 있으면 안 되고 effective scalar `reasoning_effort`와 동시 사용하지 못한다. `effort`/`max_tokens`는 배타적이다. budget은 양수, signed 64-bit wire 범위 이내, effective output cap 이하여야 한다. `enabled=false`는 effort/budget을 거부하고 `exclude=true`는 `include_reasoning=true`와 충돌한다. 대체 모델 이름은 nonempty·unique이고 승인 count limit 이내여야 한다. 각 모델은 temperature 금지를 포함한 effective choices 검사를 받는다.

policy identity는 native binding에 남는다. 공개된 내장 policy revision 4는 이전 policy-3 seal/archive를 repair하지 않고 거부한다. Generate `safety_settings`는 유효한 category가 중복되지 않아야 한다. nonempty allowed-function 목록은 `Any`/`Validated`에서만 허용한다. 모델은 두 승인 Generate descriptor path와 일치하는 literal name이어야 한다. portable tool result는 승인된 call identity와 맞아야 하며 foreign assistant call은 client-executed이고 `wire_type`/`wire_metadata`가 없어야 한다.

semantic `Stop` 경계에서 valid/invalid client-call intent 모두 `EndTurn`을 `ToolUse`로 바꾸며 observer event와 final outcome이 일치한다. 구체적인 `MaxTokens`, `ContentFilter`, `Unknown` 증거는 유지하고 완료된 server-executed hosted tool만으로 client `ToolUse`가 되지 않는다. streaming OpenRouter reasoning fragment는 index별로 합치고 최초 도착 순서를 유지한다. encrypted blob은 개별 항목으로 남는다. owned raw frame과 native continuation은 원래 content와 tamper 검사를 보존한다.

### PreparedProviderRequest / ProviderBudgetClaim
`prepare()`는 검증·인코딩을 정확히 한 번 수행하고 원래 deadline과 취소 상태를 가진 이동 전용 `PreparedProviderRequest`를 만든다. 영속 호출자는 `Provider::request_digest()`를 assembly에 바인딩하고 승인된 예산 claim을 예약하며 dispatch receipt를 기록한 다음 같은 핸들을 `ControlledProvider::dispatch_prepared(_async)`로 소비한다. gate 이후 요청을 재생성하지 않는다. 중복 receipt는 재전송하지 않는다. 사용자 공급자는 `get_name()`, `family()`, `prepare()`를 구현하고 `prepare_runtime()` 또는 `prepare_local()`을 사용한다. local callback은 `this` 대신 소유 shared 상태를 캡처한다.

선택적 `ProviderControls`는 호출자 선택이며 강제 기본값이나 몰래 clamp한 cap이 아니다. family가 지원하지 않는 제어는 dispatch 전에 거부한다. 유한 예산 호출에는 승인된 실제 모델 input/output 한계가 필요하며 없으면 `LimitUnknown`으로 실패한다. 예약은 보수적인 지출 권한이지 보고 사용량·예측·청구서가 아니다. 미상/부분/delivery-unknown 결과는 hold를 유지하고 실제 최종 보고로 정산하며 초과 보고도 전부 청구한다. 사용량 stage가 `Missing`이면 근거가 없는 것이지 0이 아니고, `Partial`은 지금까지 본 값이다. 둘 다 호출을 정산하지 못하므로 hold가 남는다. SDK가 도출한 개수(`Evidence::Derived`)는 보고된 개수처럼 정산한다. thinking에 출력 예산을 다 써서 `candidatesTokenCount`가 빠진 Gemini 호출은, 도출된 `output_total`과 `total`만으로 실제 사용량을 청구한다. 재시도는 단일 명시적 계층이며 기본 off, 유한 window와 unknown-prior hold를 사용한다. 숨은 재전송은 없다.

`provider_failure_proves_not_accepted(Failure)`는 실패한 호출이 hold를 풀지 유지할지를 모든 정산 경로가 함께 쓰는 단일 규칙이다. 적용 대상은 bank, managed journal, MCP harness provider의 토큰 reservation, Program의 금액 hold, SQLite provider journal, controlled provider의 dispatch receipt(`Failed` 또는 `ReconciliationRequired`)다. 아래 두 근거 중 하나가 완전하고 모순 없이 갖춰져야 한다.

- `NotSent`: 요청 바이트가 하나도 나가지 않았음을 SDK가 증명한다(`provider_failure_proves_not_sent(Failure)`는 이 절반만 판정한다).
- 완료된 HTTP 4xx 응답(408, 425, 429 제외)의 `RejectedBeforeOutput`. 예를 들면 잘못된 키(401), 잘못된 요청(400), 아무도 맞출 수 없는 `max_price` 고정값(404)이다. 서버가 요청을 거절했으므로 아무것도 실행되지 않았고 청구되지도 않는다. 실패에는 출력·stop reason·사용량이 없어야 하고, 청구되었을 수 있는 이전 시도도 없어야 한다.

HTTP 408, 425, 429, 5xx 응답, 끊기거나 timeout된 호출, 요청이 나간 뒤의 transport 실패, 부분 출력이나 사용량이 있는 모든 실패는 hold를 유지한다. 누락된 사용량, 관측자·영속 예외, SDK의 `RejectedBeforeOutput` 라벨이 없는 상태 코드만으로는 비용 0이나 예산 갱신을 증명하지 않는다.

```cpp
#include <neograph/controlled_provider.h>

sp::runtime::Result dispatch_admitted(
    neograph::ControlledProvider& gateway, std::string owner_scope,
    std::string dispatch_id, const neograph::ContextAssemblyReceipt& assembly,
    neograph::PreparedProviderRequest prepared,
    neograph::ProviderDispatchBudget budget) {
    auto claim = neograph::reserve_provider_dispatch(prepared, std::move(budget));
    return gateway.dispatch_prepared(
        std::move(owner_scope), std::move(dispatch_id), assembly,
        std::move(prepared), std::move(claim));
}
```


소스 및 바이너리 단절이다. 모든 C++ 소비자와 사용자 공급자를 새 헤더/라이브러리로 재컴파일한다. `CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, descriptor interpreter와 Responses WebSocket은 alias/호환 bridge 없이 제거되었다. SDK는 alpha `0.4.0`, interface revision 7 / shared-library generation 7이며 out-of-line capability check를 사용한다. 안정 릴리스 선언이 아니다. Linux/POSIX가 검증된 플랫폼이다. Windows NTFS와 macOS 구현이 있으나 이 SDK로 검증하지 않았으며, WASM provider runtime 지원은 확립되지 않았다.

Python도 C++과 같은 소유 request/outcome 경계를 제공한다: `make_provider_request`, `Provider.prepare`, `dispatch`, `invoke`. Provider 기록에는 typed part를 가진 `ProviderMessage`를 사용하며 `ChatMessage`는 그래프 편의 projection으로 남는다. SDK 실패는 `ProviderOutcome.failure`로 읽고 host observer/settlement 예외는 `outcome`과 `cause`를 보존한다. 생성자와 GIL/콜백 동작은 [Python binding 안내](python-binding.md)를 참조한다.

Python SDK의 vector/map getter는 분리된 값을 반환한다: `ProviderMessage.parts`, `ProviderRequest.messages`, `RunConfig.provider_messages`, completion/partial의 `messages`와 `raw_events`, `RunResult.native_messages`, `ProviderLoopEntry.messages`, usage의 `extra`/`conflicts`. 스냅샷을 수정한 뒤 setter가 있는 속성에 다시 대입한다. getter 결과에 append해도 소유 객체는 바뀌지 않는다. 선택적 `ProviderControls.provider`/`response_format`, `SchemaProviderDefaults.provider`, `OpenRouterRouting.max_price`, `ProviderToolResult.host`도 같은 읽기·수정·대입 규칙을 따른다. 읽기 전용 outcome 증거는 바뀌지 않는다. 이는 Python 바인딩의 규칙이며 모든 C++ getter가 복사본을 반환한다는 보장이 아니다.

네이티브 코드가 Python provider의 `prepare` override를 호출할 때 `None`을 반환하면 handle을 소비하기 전에 `TypeError`가 발생한다. `ProviderDescriptorPolicy.identity`는 raw SHA-256 digest를 담은 `bytes`이며 `.hex()`는 표시용 변환이다. 저장된 Python provider/graph cause를 반복해서 읽어도 원래 예외 값과 traceback을 유지하고 저장된 예외의 restore 상태를 소비하지 않는다. 중첩된 네이티브 예외 변환에도 같은 규칙이 적용된다.

<a id="pricing-evidence-and-the-price-catalog"></a>
<a id="pricing-evidence-and-the-model-catalogue"></a>
### 가격 증거와 모델 카탈로그

Program 실행의 `monetary_microunits` 예산은 micro-USD(1 microunit = 1e-6 USD)를 센다. 호스트가 내장 `llm_call` 노드의 provider 호출을 가격 broker로 보낼 때 호출에 가격이 매겨진다. 프로세스 안에서는 `bind_priced_provider_call(context)`, 크래시에 안전한 재생이 필요하면 `SQLiteProgramProviderCallJournal::bind_priced(context, deployment)`를 쓴다. 런타임은 각 broker에 attempt의 금액 계정을 `ProgramCoreProviderCallContext::monetary_account`로 넘긴다. Python `LocalProgramHost(node_context=..., binding_label=...)`는 항상 호출에 가격을 매긴다([Python 가이드](python-binding.md#model-calling-nodes)). 모든 `RunBudget` 필드와 마찬가지로 `monetary_microunits`는 실행에 부여된 양이며 `0`은 아무것도 부여하지 않는다. 자식과 fork 예산은 부모의 양에서 떼어 내고, `model_tokens = 0`으로 모델 토큰을 쓴 완료 실행은 `BudgetExhausted`로 보고된다. `monetary_microunits = 0`인 실행은 가격이 매겨지는 모든 호출을 보내기 전에 거부한다. 가격 코드는 내부용(`src/program/monetary.h`)이며 ABI를 약속하지 않는다.

dispatch 전에 broker는 provider descriptor 정책의 SchemaProvider 모델 카탈로그로 준비된 요청에 가격을 매기며, 그 밖의 것은 쓰지 않는다. 항목은 정책이 family, base URL, 승인된 모델에 대해 id, alias 또는 origin 범위로 맞추는 것이다. hold는 항목의 가격에 대한 `sp::pricing::upper_bound`에, dispatch 한 번이 청구할 수 있는 모델 호출 수(`Provider::invocation_upper_bound`)를 곱한 값이다. 이 상한은 요청의 입력 상한과 출력 cap, 사용 cap만큼 쓰는 서버 도구, OpenRouter origin에서는 항목의 route ceiling을 포괄한다. 정책이 client 도구로 표시한 Messages 도구(`memory_*`, `bash_*`, `text_editor_*`, computer·browser toolset)는 호출자가 실행하며 토큰만 청구되므로, 사용 cap도 요금도 필요 없다. 입력 상한은 카탈로그가 그 모델에 대해 아는 가장 작은 입력 한도, 즉 `input_limit` 또는 `context_window`다. 모든 입력 토큰은 가장 비싼 입력 요율로, 모든 출력 토큰은 가장 비싼 출력 요율로 계산하며, 상한 안의 호출이 닿을 수 있는 모든 구간에 걸쳐 그렇게 한다. OpenRouter 모델 fallback(`models`)은 가장 비싼 후보를 예약한다. 상한을 정할 수 없거나 hold가 남은 예산에 들어가지 않는 호출은 요청이나 journal marker가 생기기 전에 거부된다. 노드는 보내지 않았음을 나타내는 합성 provider 실패로 실패하고, 실행의 `ProgramFailure`가 이유를 밝힌다. `failure.witness["monetary"]`에는 origin, model, 항목, 상한, 카탈로그 digest가 들어 있다.

| 코드 | 상태 | 거부 이유 |
|---|---|---|
| `P_MONEY_UNKNOWN_ORIGIN` | `Failed` | provider에 승인된 descriptor가 없어서 카탈로그를 알 수 없다 |
| `P_MONEY_UNKNOWN_MODEL` | `Failed` | 카탈로그가 base URL의 그 모델에 가격을 주지 않거나, 그 OpenRouter fallback 중 하나에 가격을 주지 않는다 |
| `P_MONEY_UNMODELED` | `Failed` | 요청이 SchemaProvider 엔드포인트 호출이다(catalogue는 아직 가격을 매기지 못한다), 항목에 요청의 service tier 가격이 없거나 서버 도구·hosted tool 중 하나의 요금(`tool:<type>`)이 없다, 또는 요청이 OpenRouter plugin을 쓴다 |
| `P_MONEY_NO_OUTPUT_CAP` | `Failed` | 요청에 출력 토큰 cap이 없다 |
| `P_MONEY_NO_INPUT_BOUND` | `Failed` | 카탈로그가 그 모델의 입력 한도나 context window를 알지 못한다 |
| `P_MONEY_NO_UPPER_BOUND` | `Failed` | 서버 도구나 hosted tool에 사용 cap이 없거나(Messages `max_uses`, Responses `max_tool_calls`이며 Gemini의 내장 도구와, 정책이 `max_uses`를 허용하지 않는 Messages 타입인 `mcp_toolset`, `tool_search_tool_*`, `code_execution_*` 등에는 없다), router alias처럼 OpenRouter 항목에 route ceiling이 없거나, route ceiling을 `provider.max_price`로 고정할 수 없다 |
| `P_MONEY_BUDGET` | `BudgetExhausted` | hold가 남은 `monetary_microunits`에 들어가지 않는다(청구액이 예산을 넘긴 채 끝난 실행도 같은 코드를 쓰며, 이때 `failure.witness["monetary"]`에 `granted`, `used`, `over_by`가 있다) |

결과가 나오면 hold는 정확히 한 번 정산되고, attempt의 `ProgramResult::monetary_records()`(Python `result.monetary_records`)에 호출마다 `ProgramMonetaryRecord` 하나가 생긴다. 필드는 `call_id`(journal의 논리 호출 slot), `operation_id`, `node`, `origin`(base URL), `model`, `evidence`, `microunits`, `reserved_microunits`, `basis`, `catalog_version`, `catalog_digest`, `conservative`, `may_undercount`, `replayed`, `reason`, `pricing`(항목, 그 가격 행, service tier, 예약된 도구 사용, 추정의 가정)이다. `ProgramUsage::monetary_microunits`는 attempt의 청구액과 아직 열린 hold의 합이다.

| 증거 | 조건 | 금액 |
|---|---|---|
| `reported` | 응답에 provider 자체 비용이 있다: OpenRouter `usage.cost`이며 SDK가 nano-USD 단위의 `Usage::provider_cost`로 파싱한다. BYOK이면 보고된 upstream 청구액을 더한다. | 보고 금액을 정수 microunit으로 올림한 값. 기록은 hold를 산정한 카탈로그와 행도 함께 남긴다. |
| `estimated` | 보고 비용이 없는 최종·일관 usage. | `sp::pricing::estimate`가 hold에 가격을 매길 때의 카탈로그 스냅샷으로 계산한 값에, 예약된 서버 도구 요금을 cap만큼 더하고 호출당 한 번 올림. Messages의 1시간 캐시 쓰기와 Gemini의 이미지 토큰은 각자의 요율로, OpenRouter 호출은 route ceiling으로 청구된다. 기록에 `sp-model-catalogue/1`, 카탈로그 digest, 가격을 매긴 행이 남는다. |
| `held` | 결과를 모른다(transport 예외, 취소, timeout), 호출이 이미 시작된 작업 뒤에 실패했을 수 있다(HTTP 408·425·429·5xx 응답, 끊긴 스트림, 요청이 나간 뒤의 transport 실패), SDK가 transport 시도를 다시 보냈다, 또는 usage가 부분적이거나 일관되지 않다. | hold. |

provider가 아무것도 받아들이지 않았음을 증명하는 실패는 hold를 풀고 기록을 남기지 않는다. 요청을 보내지 않았거나, provider가 완료된 HTTP 4xx 응답(408, 425, 429 제외)으로 요청을 거절한 경우다. 금액 hold도 모델 토큰 reservation도 청구되지 않는다. hold보다 큰 청구액(BYOK upstream 비용처럼 고정값이 제한하지 않는 비용이 보고 비용에 포함될 때 가능)은 보고된 대로 청구되며, 청구액이 예산을 넘은 실행은 `P_MONEY_BUDGET`과 함께 `BudgetExhausted`로 끝난다. 동시 호출(`ng.all`, `ng.race`, `ng.quorum`, 병렬 노드)은 한 계정에 예약하므로 들어가지 않는 member는 거부된다. 노드 재시도는 자기 effect namespace(`task#a<k>`)로 호출하므로 attempt마다의 호출은 새 호출이며 다시 청구된다.

`bind_priced`는 호출의 기록을 journal 행에 쓴다. dispatched marker와 함께 held 기록을, 결과와 함께 정산된 기록을 쓴다. 같은 Program attempt가 프로세스가 멈추기 전에 기록한 호출은, 복구된 attempt가 그 operation을 바인딩할 때 Core가 다시 호출하든 아니든 한 번 청구되고 `replayed`로 표시된다. execution lease 만료로 복구되는 define() 전용 실행도 포함된다. 이전 attempt가 기록한 호출은 그 attempt의 결과가 남은 예산에 이미 정산했으므로, 이후 attempt가 결과를 재생해도 다시 청구되지 않는다. recorded replay는 금액을 쓰지 않는다.

fork는 일시 중지된 호출의 provider 호출을 영속적인 provider 호출 journal(`SQLiteProgramProviderCallJournal::bind` 또는 `bind_priced`)에서만 재생한다. 프로세스 내 broker에는 journal이 없으므로, fork에서 다시 실행되는 노드는 앞서 한 호출을 다시 전송하고 다시 과금한다. `core_provider_call_resolver`가 journal의 broker를 반환하는 호스트는 `RuntimeConfig::core_provider_calls_journaled = true`로 설정한다. Python `LocalProgramHost`는 `sqlite_directory`가 있을 때 그렇게 한다. 이 플래그가 `false`이고 일시 중지된 시도가 일시 중지된 연산에서 가격이 매겨진 provider 호출을 기록했다면(원본의 일시 중지된 `ProgramResult::monetary_records()`), `ProgramRuntime::fork`는 반환하는 핸들에 `ProgramNotice` 하나를 담는다. `ProgramHandle::notices()`(Python `handle.notices`)로 읽는다. `code`는 `P_FORK_PROVIDER_CALLS_NOT_JOURNALED`이고 `witness`는 `{source_run_id, source_attempt, operation_id, calls: [{call_id, node, model, microunits}], microunits}`이다. 알림은 오류가 아니며 fork가 전송하거나 과금하는 내용을 바꾸지 않는다. 나열된 호출 가운데 fork가 다시 실행하는 것이 무엇인지는 알 수 없다. 일시 중지된 스텝에서 끝난 노드의 호출은 그 스텝에 저장된 쓰기에서 재생되기 때문이다. 알림은 실행과 함께 저장되지 않으므로 `reconnect()`나 `resume()`이 반환한 핸들, 같은 실행을 다시 `fork()`해서 얻은 핸들에는 없다. 플래그는 알림을 줄지 말지만 정할 뿐, broker가 journal을 쓰게 만들지는 않는다. provider 호출을 가격 broker로 보내지 않는 호스트는 가격이 매겨진 호출을 기록하지 않으므로 알림도 받지 않는다.

OpenRouter origin에서 broker는 요청을 전송용으로 준비하기 전에 route ceiling을 요청에 고정한다. OpenRouter의 `provider.max_price`에는 항목의 `ceiling_input`이 `prompt`로, `ceiling_output`이 `completion`으로, 백만 토큰당 정확한 십진 USD로 들어간다. ceiling이 백만 토큰당 0.44 USD와 1.32 USD이면 본문에 `"provider":{"max_price":{"prompt":0.44,"completion":1.32}}`가 실린다. fallback이 있으면 고정값은 가장 비싼 후보의 ceiling을 필드별로 취한 값이다. OpenRouter는 그 가격 이하로 등록된 endpoint로만 라우팅하고 그런 endpoint가 없으면 요청을 실패시키므로, 예약이 endpoint의 토큰 가격을 제한한다. 호스트가 `ProviderControls::provider`나 `SchemaProvider::Defaults::provider`로 이미 보내는 `max_price`(요청 자체의 값이 기본값을 대체한다)는 더 낮은 필드에서 유지되고, broker가 고정하지 않는 금액(`image`, `audio`, `request`)은 호스트의 값 그대로다. Chat Completions, Responses, Messages 요청이 고정되며 다른 origin에는 `max_price`가 붙지 않는다. 가격은 대화를 사용자 메시지 하나로 바꾼 요청의 probe로 매기고 probe는 보내지 않는다. 요청은 고정된 상태로 한 번만 준비되므로 provider journal은 실제로 보낸 요청의 digest를 기록하고 재생은 그것과 일치한다. 고정값 아래에서 캡처된 native history를 가진 다음 턴도 같은 고정값을 싣는다. SDK policy가 그 base URL에서 OpenRouter routing을 허용하지 않거나 요율이 `max_price`가 허용하는 2^53 nanoUSD를 넘어 route ceiling을 고정할 수 없으면 호출은 `P_MONEY_NO_UPPER_BOUND`로 거부된다.

가격은 호스트가 공급하는 데이터다. 내장 SchemaProvider 카탈로그에는 가격이 하나도 없으므로, 호스트가 모델에 가격을 매기는 카탈로그를 담은 정책을 provider에 줄 때까지 예산이 있는 실행은 가격이 매겨지는 모든 호출을 거부한다. C++에서는 `sp::descriptor::load_policy(family, resources, catalogue)` 또는 `DescriptorPolicy::with_catalogue`를 쓰고, 호스트 레이어는 `sp::descriptor::merge_catalogues`로 병합한다. Python에서는 `load_provider_policy(family_json, resource_json, model_catalogue_json)`가 주어진 카탈로그를 내장 카탈로그 위에 얹는다. 항목의 `pricing`은 단위를 단위당 십진 USD에 대응시킨다(SchemaProvider의 USAGE, "Pricing by the catalogue"):
- `input`, `output`, `cache_read`, `cache_write`, `cache_write_1h`, `reasoning`, `output_image`와 `request`.
- `ceiling_input`, `ceiling_output`, `ceiling_request`: OpenRouter의 route ceiling으로, 모델의 endpoint 가운데 가장 비싼 값이며 broker가 `provider.max_price`로 고정한다.
- `<unit>/<tier>`: service tier의 가격. `default`, `standard`, `standard_only`가 아닌 tier의 요청은 그 단위만으로 가격이 매겨진다.
- `<unit>+<tokens>`: long-context 구간. 캐시를 포함한 입력 토큰이 그 수를 넘으면 구간의 단위가 호출 전체에 가격을 매긴다.
- `tool:<type>`: 서버 도구나 hosted tool을 한 번 쓸 때의 요금. `"0"`은 무료임을 선언하며, 기본적으로 무료인 도구는 없다.

origin 범위(`origins`)가 있는 항목은 그 origin으로 가는 호출에만 가격을 매긴다. 같은 모델이 origin 두 곳에 있으면, 그 모델을 alias로 이름 붙이고 다른 origin으로 범위를 정한, 자체 id를 가진 두 번째 항목이 필요하다. [`tests/fixtures/pricing/model_prices.json`](../tests/fixtures/pricing/model_prices.json)은 0.15 가격 카탈로그를 이 형태로 담고 있다. 2026-10-11에 가져온 가격을 담은 테스트 fixture이며 가격 출처가 아니다.

NeoGraph는 실행 중에 가격을 가져오거나 갱신하지 않는다. 호스트는 카탈로그를 실행과 별도로 갱신하고(예를 들어 `sp::transport::refresh_catalogue`로 OpenRouter 목록에서), 그것으로 만든 provider를 바인딩한다. 호출마다 카탈로그 digest와 가격을 매긴 행을 journal에 기록하고, dispatch된 호출은 hold의 가격을 매긴 스냅샷으로 정산하며, 복구되거나 fork된 실행은 journal에 기록된 금액을 유지한다. 0.16.0 이전에 journal에 기록된 레코드는 바이트가 그대로다. 그 레코드는 `openrouter` 같은 가격 카탈로그 origin, 카탈로그 version과 digest를 이름으로 담고 `pricing`은 없다.

조직 비용 보고서(OpenAI organization costs endpoint, Anthropic cost report)는 admin key가 필요하고 시간 구간별로 집계되며 늦게 도착하므로 런타임은 쓰지 않는다. 호스트는 나중에 `reported`·`estimated` 기록을 이 보고서와 대조할 수 있다.

---

## 3. Tool Interface

**Header:** `<neograph/tool.h>`
**Namespace:** `neograph`

Abstract interface for tools that LLMs can call. Implement this to expose functions
to the agent.

> **Writing a custom Tool subclass?** See
> [`ASYNC_GUIDE.md` §9.6](ASYNC_GUIDE.md#96-tool-vs-asynctool) for
> when to inherit `Tool` (sync) vs `AsyncTool` (async). The two are
> mutually exclusive — pick one.

### Tool

```cpp
class Tool {
public:
    virtual ~Tool() = default;

    // Returns the tool's definition (name, description, parameter schema)
    virtual ChatTool get_definition() const = 0;

    // Executes the tool with the given arguments, returns result as string
    virtual std::string execute(const json& arguments) = 0;

    // Returns the tool's unique name
    virtual std::string get_name() const = 0;
};
```

| Method | Returns | Description |
|--------|---------|-------------|
| `get_definition()` | `ChatTool` | Returns the tool's metadata including JSON Schema for parameters |
| `execute(arguments)` | `std::string` | Runs the tool with parsed JSON arguments. Returns the result as a string that will be sent back to the LLM |
| `get_name()` | `std::string` | Unique identifier for this tool |

**Example implementation:**

```cpp
class WeatherTool : public neograph::Tool {
public:
    ChatTool get_definition() const override {
        return {"get_weather", "Get current weather for a city", json::parse(R"({
            "type": "object",
            "properties": {
                "city": {"type": "string", "description": "City name"}
            },
            "required": ["city"]
        })")};
    }

    std::string execute(const json& args) override {
        std::string city = args.at("city");
        return "Weather in " + city + ": 22C, sunny";
    }

    std::string get_name() const override { return "get_weather"; }
};
```

<a id="tool-failure-contract"></a>
### 도구 실패 계약

모델의 도구 호출을 실행하는 모든 경로가 같은 규칙을 적용합니다. 단일 호출이나 batch를 처리하는 `dispatch_tool_calls`, `tool_dispatch` 노드(Program의 Core `tool_dispatch` 포함), `llm::Agent::run`과 `run_stream`, plan-and-execute 실행기, deep-research 연구자가 여기에 해당하며, C++, MCP, Python 도구 모두 마찬가지입니다.

| 실패 | 결과 |
|------|------|
| `HookBoundaryBlocked`: 필수 Hook이 실패했거나, 시간을 초과했거나, 실행될 수 없었거나, 도구 코드가 이를 발생시킴 | 실행이 그 예외로 중단되며, invocation id와 state는 그대로 유지됩니다. 그래프 실행도 `llm::Agent`처럼 이 예외를 `NodeExecutionError`로 감싸지 않고 그대로 던집니다. 노드 재시도 정책은 이를 재시도하지 않습니다. |
| 실행의 `CancelToken`이 취소됨(노드 시도의 토큰 포함) | 실행이 `graph::CancelledException`으로 중단됩니다. 진행 중인 MCP 호출이나 `GrpcRemoteTool` RPC는 즉시 멈추고, 늦게 도착한 응답은 버려집니다. |
| `ToolGate` 또는 도구 코드에서 발생한 `NodeInterrupt` | 실행이 일시 정지합니다. `llm::Agent`는 이를 호출자에게 던집니다. |
| 알 수 없는 도구, 또는 게이트의 `Deny` | 상태가 `rejected`인 도구 결과. |
| 호출이 실행의 마감 시각(`RunContext::deadline`, `ToolExecutionContext::deadline`) 또는 그 뒤에 시작될 예정 | 상태가 `expired`이고 오류가 `run deadline elapsed before the tool started`인 도구 결과. 도구도 그 Hook도 실행되지 않습니다. |
| 도구가 속한 서비스가 실행하기 전에 거부한 요청: MCP JSON-RPC 오류 `-32700`, `-32600`, `-32601`, `-32602`, `NG.InvalidRequest`로 표시된 MCP `isError` 결과, gRPC `ToolService`의 `NOT_FOUND` 또는 `INVALID_ARGUMENT` 상태, 또는 도구 코드가 던진 `ToolRequestRejected` | 상태가 `failed`이고 `retryable`과 `effect_uncertain`이 모두 false인 도구 결과. 아무것도 실행되지 않았으므로 실행 정책과 관계없습니다. |
| 도구 코드나 도구 조회에서 발생한 그 밖의 예외(C++, Python, `-32603` 같은 그 밖의 MCP `isError` 결과 또는 JSON-RPC 오류), 파싱할 수 없는 인수, 또는 정책의 `output_limit_bytes`를 넘는 출력 | 상태가 `failed`인 도구 결과. 호출이 이미 시작되었고 실행 정책이 효과의 발생을 배제하지 않으면(`ReadOnly`도, `ExternalRead`도, `Idempotent`도 아니면) `reconciliation_required`. |
| `timed_out`, `expired`, `killed`, `input_required` 같은 타입이 지정된 컨트롤러 상태 | 그 상태를 가진 도구 결과. 실행의 마감 시각이나 요청 타임아웃으로 멈춘 MCP 호출이나 `GrpcRemoteTool` RPC는 `timed_out`입니다. |
| `ToolEffectBroker` 아래에서 효과가 불확실한 도구 결과 | 페이로드 `kind`가 `tool_effect_reconciliation`인 `NodeInterrupt`로 실행이 일시 정지합니다. |

처음 세 행은 결코 모델에 도달하지 않습니다. 도구 결과는 `tool_status`를 설정하며, 실패한 결과는 단일 호출이든 batch든 `{"error", "status", "retryable", "effect_uncertain"}`를 담습니다. batch에서는 다른 호출이 먼저 끝까지 실행되며, 여러 호출이 실행을 중단시키면 취소가 Hook 차단보다, Hook 차단이 인터럽트보다 우선합니다. 도구 코드가 발생시킨 `NodeInterrupt`가 있어도 형제 호출은 실행되며, 재개하면 batch 전체가 다시 실행됩니다. 아무것도 실행되기 전에 묻으려면 `ToolGate`를 사용하세요. 실행 기한(`RunMetadata::deadline`)은 Hook과 제공자 호출, 그리고 승인 큐에서의 대기를 포함한 모든 도구 호출의 시작을 제한하며, 도구 자체의 한도는 정책의 `execution_timeout`과 `queue_timeout`입니다. 도구 코드가 시작한 뒤에 발생시킨 시스템 오류는 승인 판정이 아니라 도구의 실패(`failed` 또는 `reconciliation_required`)입니다. 호스트 승인은 리소스 중재기와 마찬가지로 `rejected`(큐가 가득 참, 용량 없음) 또는 `expired`(큐 대기 시간이 다 됨)로 거절합니다. 시작되지 않은 호출(`cancelled_before_start`, `expired`, `rejected`: `tool_call_not_started()`)은 효과를 내지 않았습니다. 영속적인 `SQLiteToolEffectBroker`를 포함한 모든 `ToolEffectBroker`는 그 호출을 그 상태 그대로, 종료 상태이며 효과가 불확실하지 않은 결과로 돌려주고 슬롯을 해제합니다. 따라서 그 호출은 조정을 위해 실행을 일시 정지시키지도, 같은 슬롯의 이후 호출을 막지도 않습니다.

**거부된 요청은 결과가 알려진 경우입니다.** `ToolRequestRejected`(`<neograph/tool_execution.h>`)는 도구가 자기 호출이 실행되지 않았음을 알 때 던지는 예외입니다. `MCPError`처럼 자체 기반 클래스가 있는 예외 타입은 대신 `ToolRequestRefusal` 인터페이스를 구현합니다. `retryable()`은 같은 요청을 다시 보내면 성공할 수 있는지를 나타냅니다. `MCPTool`은 서버가 도구를 실행하기 전에 요청을 거부하는 JSON-RPC 오류(`-32700`, `-32600`, `-32601`, `-32602`)를 거부이기도 한 `MCPError`로 던지므로, `MCPError`를 잡는 호출자는 코드와 데이터를 그대로 받습니다. `_meta["io.neograph/failure"].code`가 `NG.InvalidRequest`인 `isError` 결과는 `ToolRequestRejected`로 던지며, `MCPServer`는 도구의 입력 스키마에 맞지 않는 인수를 이렇게 보고합니다. `GrpcRemoteTool`은 `NOT_FOUND`와 `INVALID_ARGUMENT` 상태에 대해 `ToolRequestRejected`를 던집니다. 컨트롤러는 거부를 `failed`로 바꿉니다. 같은 요청은 다시 거부되고 모델이 요청을 고쳐야 하므로 `retryable`은 false이고, `effect_uncertain`도 false입니다. 그 밖의 실패는 결과를 알 수 없으므로(서버가 도구를 실행한 뒤 실패했을 수 있습니다: 일반 `isError` 결과, `-32603`, `-32000` 같은 서버 오류), 도구의 정책이 효과를 배제하지 않는 한 `reconciliation_required`로 남습니다. 노드 실패 분류기는 거부를 일반 실패로 읽으며, `retryable()`이 false이면 재시도하지 않습니다.

**도구 Hook.** 모든 경로가 호출이 시작되기 전에 `BeforeToolExecution` 이벤트 하나를, 호출이 확정된 뒤에 `AfterToolExecution` 이벤트 하나를 같은 데이터(`tool_call_id`, `tool_name`, `thread_id`, `request_id`, 그리고 앞에서는 `arguments`, 뒤에서는 `status`와 `result`)로, 실행의 owner scope와 run id 아래에서 내보냅니다. `HookRuntime`은 직접 디스패치에서는 `ToolExecutionContext`의 것이고, Agent에서는 `llm::Agent::set_hook_runtime` 또는 `run`에 넘긴 컨텍스트의 것이며, 그래프 실행에서는 엔진의 것(`EngineConfig::hook_runtime`, `GraphEngine::set_hook_runtime`)입니다. 단, `RunResources::hook_runtime`이 있으면 그 실행에서는 그것으로 대체됩니다. 서브그래프의 도구 호출은 부모 실행과 자신의 엔진에 모두 `HookRuntime`이 있으면 둘 다를 거치며, 부모의 Hook이 먼저 실행되고(`HookRuntime::compose`) 어느 쪽이든 차단하면 호출이 차단됩니다. Program Core의 도구 호출은 Program 호스트의 것(`RuntimeConfig::hook_runtime`)을 사용합니다. 호출에 태스크 슬롯(`ToolExecutionContext::effect_task_id`, 모든 그래프 실행에서 설정됨)이 있으면 이벤트는 `task_id`와 `call_ordinal`도 담습니다. 이는 호출의 영속적인 신원으로, 효과 신원과 같습니다(실행, 스레드, `s3:act#a1`처럼 노드 시도를 포함한 태스크, 그리고 batch 안에서의 호출 서수). `request_id`는 이 신원에서 도출됩니다. 재개나 재생이 batch를 다시 실행해도 그 신원으로 이미 전달된 이벤트는 다시 전달되지 않습니다(`HookRuntime::emit_once_async`, Hook 저널 자체의 기록을 사용). 필수 Hook이 통과하지 못한(실패, 시간 초과 또는 취소되어 실행을 멈춘) 발행은 전달이 아니었으므로, 다음 발행에서 Hook이 다시 실행됩니다. 거부되었거나 알 수 없거나 만료된 호출은 아무것도 시작하지 않으며 Hook도 내보내지 않습니다. 마지막 시도가 실패한 필수 Hook은 `failed`를 보고하며, 마감 시각이 지났거나 대상이 시간을 초과했을 때만 `timed_out`을 보고합니다. 시간을 초과한 Hook 대상은 최종 상태입니다. 시간을 초과한 대상이 아직 실행 중일 수 있고 두 번째 시도가 그 옆에서 함께 실행될 수 있으므로, Hook 시도 횟수나 대상 정책의 `retry_max_attempts`가 남아 있어도 실행기도 도구 실행 컨트롤러도 대상을 다시 시작하지 않습니다. 아예 실행될 수 없는 Hook 경계(이벤트를 귀속시킬 owner scope가 없음, 저널을 쓸 수 없음)는 도구, 체크포인트, 제공자, Program 이벤트 모두에서 state `failed`로 닫힌 채 실패합니다. Hook이 실행되는 동안 실행이 취소되면 실행은 취소로 중단됩니다.

**텍스트.** 경계를 넘는 텍스트는 올바른 UTF-8입니다. 잘못된 형식의 최대 부분 시퀀스 하나하나가 U+FFFD가 되며, 이는 Unicode와 WHATWG의 대체 방식입니다. 이 규칙은 도구 결과와 실패 메시지, Hook 영수증, `json::dump`(잘못된 문자열을 담은 문서가 예전에는 빈 문자열로 직렬화되었습니다), MCP와 gRPC 응답, 그리고 Python 바인딩에 적용됩니다. Python에서는 C++에서 읽은 텍스트도 같은 방식으로 디코딩되며, `str`은 UTF-8로 바뀔 때 surrogate-escape된 바이트(U+DC80부터 U+DCFF까지, `errors="surrogateescape"`가 만드는 것)는 그것이 나타내는 바이트로 되돌아가고, 그 밖의 단독 surrogate는 U+FFFD가 됩니다.

**예외를 던지는 파사드.** `ToolExecutionController::execute_async`는 성공하면 출력을 반환하고, `cancelled_before_start`와 `cancellation_requested`에는 `graph::CancelledException`을, 그 밖의 모든 상태에는 `ToolExecutionResult` 전체를 담은 `ToolExecutionError`를 던집니다. 노드 실패 분류기는 이를 읽어, 효과가 불확실하면 `NG.EffectUncertain`, 시간을 초과했거나 만료된 호출이면 `NG.Timeout`, 그 밖에는 Ordinary로 분류합니다. 도구의 서비스가 실행 전에 거부한 호출(`ToolRequestRefusal`일 때 설정되는 `ToolExecutionResult::refused`)은 거부가 노드에 그대로 도달했을 때처럼, `retryable`이 재시도가 도움이 될 수 있다고 하지 않는 한 재시도되지 않습니다.

<a id="protocol-failure-mapping"></a>
### 프로토콜 실패 매핑

NeoGraph가 MCP, gRPC, A2A, ACP로 도구나 그래프나 에이전트를 제공할 때, 모든 실패 분류는 노드 실패 분류기가 주는 안정된 코드(`graph::classify_failure`)로 이름 붙은, 서로 구별되고 문서화된 형태를 와이어에서도 유지합니다. 실행 제어 신호는 결코 모델이 읽을 수 있는 도구 결과가 되지 않습니다. 와이어를 넘는 것은 공개 메시지뿐입니다. 실패의 `public_message`(`NodeFailureError`에서 온 것) 또는 분류에 대한 고정된 설명이며, 경로나 페이로드나 비밀을 담을 수 있는 예외 텍스트는 결코 넘지 않습니다.

| 분류(코드) | MCP `tools/call` (`MCPServer`) | gRPC `GraphService` | gRPC `ToolService.CallTool` | A2A task(`A2AServer`) | ACP `session/prompt`(`ACPServer`) |
|------|------|------|------|------|------|
| 클라이언트의 취소(`NG.Cancelled`): `notifications/cancelled`, gRPC 취소 | 어떤 전송에서도 응답 없음: stdio(`run`)는 아무것도 보내지 않음. HTTP(`MCPHttpServer`)는 POST를 빈 `text/event-stream`으로 끝냄. 응답 sink를 읽는 전송은 대신 `NoResponseSink`로 id를 받음 | `CANCELLED` | `CANCELLED` | `CancelTask`: `CANCELED`. stream도 이 상태로 닫힘 | `session/cancel`: `stopReason: "cancelled"` |
| 서버의 취소(`NG.Cancelled`): `stop()`, 또는 핸들러 자신의 작업 | JSON-RPC 오류 `-32000`(`stop()`) 또는 `-32603`, `data.code` | `CANCELLED` | `CANCELLED` | `CANCELED` | JSON-RPC 오류 `-32800`, `data` `{code, retryable, retryable_by_class}` |
| Hook 차단(`NG.HookBlocked`) | JSON-RPC 오류 `-32010`, `data` `{code, invocation_id, state}` | `FAILED_PRECONDITION`, Hook이 시간을 초과했으면 `DEADLINE_EXCEEDED`. ErrorInfo metadata `invocation_id`, `state` | `GraphService`와 같음 | 끝난 노드가 없으면 `REJECTED`, 그 외에는 `FAILED`. `metadata["neograph/failure"]` `{code, retryable, retryable_by_class}` | JSON-RPC 오류 `-32603`, `data` `{code, retryable, retryable_by_class}` |
| 인터럽트(`NG.Interrupted`) | JSON-RPC 오류 `-32011`, `data` `{code, reason, value}` | 일시 정지한 실행: `OK`, `interrupted`, `interrupt_node`, `interrupt_value_json`(스트림의 FINAL 페이로드: `interrupt_value`) | `FAILED_PRECONDITION`, ErrorInfo metadata `reason`, `value` | `INPUT_REQUIRED`: 사유는 text, 페이로드는 data part, `metadata["neograph/interrupt"]` `{node, checkpointId}`. 후속 message가 실행을 재개 | 같은 턴 안의 `session/request_permission`, 그 응답이 실행을 재개. 어댑터나 클라이언트가 응답하지 못하면 `NG.Interrupted`를 담은 JSON-RPC 오류 `-32603` |
| 일반(`NG.TaskFailed`, 작성자 코드) | `isError: true`와 `_meta["io.neograph/failure"]` `{code, retryable, retryable_by_class}`를 가진 `result` | `UNKNOWN` | `error`와 `error_code`를 가진 `OK` | `FAILED`, 공개 메시지를 status message로, `metadata["neograph/failure"]` `{code, retryable, retryable_by_class}` | JSON-RPC 오류 `-32603`, 공개 메시지를 message로, `data` `{code, retryable, retryable_by_class}` |
| 타임아웃(`NG.Timeout`) | 일반과 같되 자기 코드를 가짐 | `DEADLINE_EXCEEDED` | 일반과 같되 자기 코드를 가짐 | 일반과 같되 자기 코드를 가짐 | 일반과 같되 자기 코드를 가짐 |
| 제공자 실패(`NG.ProviderFailed`) | 일반과 같되 자기 코드를 가짐 | `UNAVAILABLE` | 일반과 같되 자기 코드를 가짐 | 일반과 같되 자기 코드를 가짐 | 일반과 같되 자기 코드를 가짐 |
| 예산(`NG.BudgetExhausted`) | 일반과 같되 자기 코드를 가짐 | `RESOURCE_EXHAUSTED` | 일반과 같되 자기 코드를 가짐 | 일반과 같되 자기 코드를 가짐 | 실행의 모델 토큰 예산을 넘었으면 `stopReason: "max_tokens"`, 아니면 일반과 같음 |
| 안전 또는 구성(`NG.SafetyBlocked`, `NG.ConfigError`) | 일반과 같되 자기 코드를 가짐 | `FAILED_PRECONDITION` | 일반과 같되 자기 코드를 가짐 | 일반과 같되 자기 코드를 가짐 | `NG.SafetyBlocked`는 `stopReason: "refusal"`, `NG.ConfigError`는 일반과 같음 |
| 논리(`NG.LogicError`, `NG.BarrierStalled`) | 일반과 같되 자기 코드를 가짐 | `INTERNAL` | 일반과 같되 자기 코드를 가짐 | 일반과 같되 자기 코드를 가짐 | 일반과 같되 자기 코드를 가짐 |
| 커밋된 효과(`NG.EffectCommitted`) | 일반과 같되 자기 코드를 가짐 | `ABORTED` | 일반과 같되 자기 코드를 가짐 | 일반과 같되 자기 코드를 가짐 | 일반과 같되 자기 코드를 가짐 |
| 불확실한 효과, 알 수 없음, 실패 출구(`NG.EffectUncertain`, `NG.UnknownFailure`, 출구의 코드) | 일반과 같되 자기 코드를 가짐 | `UNKNOWN` | 일반과 같되 자기 코드를 가짐 | 일반과 같되 자기 코드를 가짐 | 일반과 같되 자기 코드를 가짐 |
| 단계 한도(`NG.StepLimit`): `RunConfig::max_steps` | 없음 | `OK`, `max_steps_exhausted` | 없음 | `FAILED`, 코드 `NG.StepLimit` | `stopReason: "max_turn_requests"` |
| 잘못된 요청(`NG.InvalidRequest`) | JSON-RPC 오류 `-32602`. 도구의 입력 스키마에 맞지 않는 인수는 `isError: true`와 `_meta["io.neograph/failure"]` `{code: "NG.InvalidRequest", retryable: false, retryable_by_class: false}`를 담은 `result`이며, NeoGraph 클라이언트는 이를 아무것도 실행되지 않았다는 표시로 읽습니다 | `INVALID_ARGUMENT`. 실행할 그래프가 없으면 `FAILED_PRECONDITION`(`NG.NoGraph`) | `INVALID_ARGUMENT` | `google.rpc.BadRequest` field violation을 담은 JSON-RPC 오류 `-32602`. 형식이 잘못된 envelope나 batch는 `-32600` | JSON-RPC 오류 `-32602` |
| 알 수 없는 도구(`NG.ToolNotFound`) | JSON-RPC 오류 `-32602`, `data.code` | 없음 | `NOT_FOUND` | 없음 | 없음 |
| 용량 초과(`NG.Capacity`) | JSON-RPC 오류 `-32000`, `data.capacity` | `RESOURCE_EXHAUSTED` | 없음 | JSON-RPC 오류 `-32603`, ErrorInfo reason `RESOURCE_EXHAUSTED`, domain `neograph` | JSON-RPC 오류 `-32603`, `data.reason` `overloaded` |

`retryable`은 모든 프로토콜에서 `FailureVerdict::retryable`이며, 같은 요청을 다시 보내면 성공할 수 있는지를 뜻합니다. 노드의 재시도 판정([노드 실패 계약](#node-failure-contract))에서 출발하며, 제공자 실패(`NG.ProviderFailed`)에서도 `true`입니다. 노드 재시도 루프는 이를 제공자 런타임에 맡기지만 새 요청은 통과할 수 있기 때문입니다. 노드 재시도 루프가 그 실패에 노드의 시도를 다 쓰면(`kind` `exhausted`) `false`가 됩니다. 재시도 정책이 없는 노드도 시도가 한 번뿐이므로 여기에 해당합니다. 요청을 다시 보내면 엔진이 이미 해 본 일을 되풀이할 뿐이기 때문입니다. 실행의 마감 시각이나 재시도 상한 때문에 루프가 재시도를 멈춘 실패는 `true`를 유지합니다. 새 실행에는 새 예산이 있기 때문입니다. `retryable_by_class`(`FailureVerdict::retryable_by_class`)는 시도가 남았는지와 관계없이 판정만 담습니다. 시도가 한 번뿐인 그래프를 감싸는 경우처럼 엔진 밖에서 재시도하는 호출자를 위한 것입니다. 시도를 다 쓴 일시적 실패는 `retryable: false, retryable_by_class: true`이고, 결정적 실패는 `false, false`입니다. A2A는 코드와 두 플래그를 두 버전 모두 task의 `metadata["neograph/failure"]`에, ACP는 오류의 `data`에, MCP 서버는 `_meta["io.neograph/failure"]`에, gRPC는 ErrorInfo metadata에 `"true"` 또는 `"false"`로 담습니다. A2A에만 있는 요청 오류는 [A2A error 매핑](#a2a-error-mapping)에 있습니다.

OK가 아닌 모든 gRPC 상태는 `grpc-status-details-bin` trailer에 `google.rpc.ErrorInfo` 하나를 담은 `google.rpc.Status`를 실어 보냅니다. `reason`은 AIP-193을 따라 코드에서 도출한 63자 이하의 UPPER_SNAKE 상수입니다(`NG.HookBlocked`는 `HOOK_BLOCKED`, 그 밖에 `TIMEOUT`, `TOOL_NOT_FOUND`, `INVALID_REQUEST`, `NO_GRAPH`, `CAPACITY`, `BACKEND_UNAVAILABLE`, `BARRIER_STALLED`. 작성자 코드는 `TASK_FAILED`처럼 그 클래스의 reason이 됩니다). `domain`은 `neograph`이며, `metadata`에는 `code`(안정적인 코드 자체), `retryable`, `retryable_by_class`와 위의 필드가 들어 있습니다. `RunGraphResponse.error`는 폐기 예정이며 공개 메시지만 담은 채 한 릴리스 동안 유지됩니다. OK가 아닌 단항 상태에는 응답 본문이 실리지 않습니다. `RunGraphStream`은 실패를 페이로드가 `{error, code}`인 `DEBUG` 이벤트로 알린 뒤 단항 호출과 같은 상태로 끝납니다. `CheckpointService`는 백엔드 실패에 "체크포인트 없음", 빈 목록, 성공한 삭제 대신 `UNAVAILABLE`(`NG.BackendUnavailable`)로, 형식이 잘못된 `SaveCheckpoint`에는 `INVALID_ARGUMENT`로 응답합니다.

`RunGraphStream`은 `RunGraphRequest.stream_mode`, 곧 `StreamMode`의 비트를 따릅니다(0은 모든 이벤트를 스트리밍하고, 알 수 없는 비트는 무시합니다). 상태 스냅샷은 `VALUES` 이벤트, 노드의 채널 쓰기는 `UPDATES` 이벤트, 인터럽트와 노드 오류는 `DEBUG` 이벤트입니다. 노드 오류의 `error`는 `node failed`이며 예외 텍스트는 절대 담지 않습니다. `RunGraphRequest.thread_id`는 필수입니다. 비어 있으면 `INVALID_ARGUMENT`로 거부됩니다. 다른 언어로 작성한 `CheckpointService`는 `RequiresManagedBudget`를 포함해 `proto/neograph.proto`의 모든 RPC를 제공해야 합니다. 엔진은 새 실행과 재개되는 실행마다 이를 묻고 `UNIMPLEMENTED`면 실행을 실패시키며, 예산을 관리하지 않는 서버는 `required = false`로 답합니다. NeoGraph의 `CheckpointService`는 JSON이 아니라 `Checkpoint`를 저장하므로, 온전히 보관할 수 없는 `checkpoint_json`(모르는 최상위 멤버, 또는 자신보다 새로운 `schema_version`)은 일부를 버리는 대신 `INVALID_ARGUMENT`로 거부합니다. `channel_values`와 `metadata`는 보낸 그대로 보관합니다.

`run_server`, `run_tool_server`, `run_checkpoint_server`는 표준 헬스 서비스 `grpc.health.v1.Health`에 응답합니다. 호출을 받는 동안 서버와 그 서비스에 대해 `SERVING`, 멈추기 시작하면 `NOT_SERVING`입니다. `ServeOptions`(`<neograph/grpc/serve_options.h>`)를 받는 오버로드는 `ServeOptions::stop`이 요청되면 진행 중인 호출에 `shutdown_grace`를 준 뒤 멈추고, 바인딩한 포트를 `on_serving`에 알립니다(그래서 주소에 포트 0을 쓸 수 있습니다). 서버 리플렉션은 호스트가 선택합니다. `gRPC::grpc++_reflection`을 링크하고 서버가 시작되기 전에 `grpc::reflection::InitProtoReflectionServerBuilderPlugin()`을 호출하면, gRPC가 내장 서버를 포함해 프로세스의 모든 서버에 리플렉션을 추가합니다. TLS, 인증, 메시지 크기 한도가 필요하면 `make_graph_service()`, `make_tool_service()`, `make_checkpoint_service()`를 직접 만든 `grpc::ServerBuilder`에 등록하십시오. gRPC 헤더가 gRPC 자체 헤더를 포함하므로 `using namespace neograph;`를 쓰는 코드는 `neograph::grpc::GrpcRemoteTool`이라고 써야 합니다. 그곳에서 한정자 없는 `grpc::`는 모호합니다.

NeoGraph 자체 클라이언트는 호출을 제한합니다. MCP 클라이언트는 취소되었거나 시간을 초과해 더 이상 기다리지 않는 요청에 대해(`initialize`는 제외) `notifications/cancelled`를 보내고 늦은 응답을 버립니다. `GrpcRemoteTool`(`set_call_timeout`, 기본 60초)과 `GrpcCheckpointStore`(`set_rpc_timeout`, 기본 30초)는 모든 호출에 마감 시각을 두며, 실행의 cancel token은 원격 도구의 RPC를 취소합니다. 이 클라이언트들은 원격 코드를 로컬 제어 신호로 되돌리지 않습니다. 원격 Hook 차단(MCP `-32010`, `ErrorInfo` 코드가 `NG.HookBlocked`인 gRPC 상태)과 원격 인터럽트(`-32011`, `NG.Interrupted`)는 원격 실행의 것입니다. `MCPTool`과 `GrpcRemoteTool`은 그 코드와 원격의 공개 메시지를 담고 재시도되지 않는 일반 실패인 `graph::RemoteControlFailure`를 발생시키므로, 로컬 실행은 실패한 도구 결과와 함께 계속됩니다. 원격 서버의 그 밖의 MCP JSON-RPC 오류, gRPC 도구 오류나 상태는 호출자에게 일반 도구 실패입니다. 다만 원격이 실행하기 전에 거부한 요청(MCP `-32700`, `-32600`, `-32601`, `-32602`와 `NG.InvalidRequest`로 표시된 결과, gRPC `NOT_FOUND`, `INVALID_ARGUMENT`)은 효과가 불확실하지 않은 실패한 도구 결과입니다([도구 실패 계약](#tool-failure-contract)). 에이전트의 요청에 ACP 클라이언트가 JSON-RPC 오류로 답하면(`ACPServer::call_client`, `ACPClient`) 그 오류는 `acp::ACPClientError`이며, 메서드, 코드, 클라이언트의 메시지와 `data`를 담습니다. 요청 자체가 성공할 수 없다는 코드(`-32700`, `-32600`, `-32601`, `-32602`, `-32002` resource not found, `-32000` authentication required)이면 `acp::ACPClientRefusal`이 됩니다. 이 오류는 재시도되지 않고, 도구 안에서는 효과가 불확실하지 않은 실패한 결과입니다. `-32603`을 비롯한 그 밖의 코드와 시간 초과는 일반 실패입니다. TODO(이후 단계): 원격 인터럽트를 사람이 답하는 네이티브 일시 정지로 로컬 실행에 전달합니다.

<a id="node-failure-contract"></a>
### 노드 실패 계약

노드가 예외를 던지면, 재시도 정책이 그 노드를 다시 실행할 수 있는지와 실패를 실패 핸들러로 라우팅할 수 있는지를 하나의 분류기가 결정합니다. 분류기는 원인 사슬 전체(`NodeExecutionError`의 원인, `std::nested_exception`, 노드가 실행한 서브그래프의 실패)를 따라가며, 사슬 어디에서든 발견된 가장 심각한 분류가 결과가 됩니다. 위의 도구 실패 계약도 같은 분류기를 사용합니다. 따라서 도구의 원인 사슬 어디에든 실행 제어 신호가 있으면 실행이 중단됩니다. 예를 들어 도구가 실행한 그래프의 실패 안에 들어 있는 Hook 차단이 그렇습니다.

| 분류(덜 심각한 것부터) | 코드 | 발생 원인 | 재시도 | 라우팅 |
|------|------|------|------|------|
| Ordinary | `NG.TaskFailed`, `NG.SubgraphFailed` 또는 작성자 코드 | `std::runtime_error`와 그 하위 클래스, `NodeFailureError`, Python 예외, 근본 원인이 Ordinary인 서브그래프 실패 | 예. 작성자가 재시도 불가로 표시했거나 요청이 거부된 경우(그렇다고 알리는 `ToolRequestRefusal`, 예: `acp::ACPClientRefusal`)는 제외 | 예 |
| Timeout | `NG.Timeout` | `attempt_timeout_ms`를 넘겨 취소되고 멈춘 시도(`NodeAttemptTimeout`), 시간을 초과한 작업(`timed_out`인 asio 또는 `std::system_error`), 시간을 초과했거나 만료된 도구 호출(`ToolExecutionError`) | 예 | 예 |
| ProviderFailed | `NG.ProviderFailed` | 일시적인 `ProviderFailure`: 전송, 잘린 응답, 기한, 과부하, 속도 제한, 또는 HTTP 5xx 상태의 원격 실패 | 아니요. 제공자 재시도는 제공자 런타임이 담당합니다 | 예 |
| EffectCommitted | `NG.EffectCommitted` | 시도가 멱등하지 않은 Tool 효과를 커밋함 | 절대 안 함 | 예. 핸들러가 보상할 수 있습니다 |
| LogicError | `NG.LogicError` | 무결성 검사를 포함한 `std::logic_error` | 아니요 | 아니요 |
| ConfigError | `NG.ConfigError` | 제공자 구성, 요청, 인증, 권한, not-found 또는 오용 실패. Program 진단(C++나 Python의 `ProgramDiagnosticError`, 컴파일 또는 승인 오류)처럼 `ConfigurationFailure`를 함께 상속한 예외 | 아니요 | 아니요 |
| SafetyBlocked | `NG.SafetyBlocked` | 콘텐츠 필터나 거부로 멈춘 제공자 실패 | 아니요 | 아니요 |
| BudgetExhausted | `NG.BudgetExhausted` | 예산 또는 할당량 소진 | 아니요 | 아니요 |
| EffectUncertain | `NG.EffectUncertain` | 결과를 알 수 없는 제공자 디스패치를 포함한 `ProviderOutcomeError`, 결과를 알 수 없는 Tool 효과(`ToolExecutionError` 포함), 또는 정지 마감 시각까지 멈추지 않은 타임아웃된 시도 | 아니요 | 아니요 |
| UnknownFailure | `NG.UnknownFailure` | `std::bad_alloc` 또는 그 밖의 모든 예외 타입 | 아니요 | 아니요 |
| 실행 제어 | `NG.Interrupted`, `NG.HookBlocked`, `NG.Cancelled` | `NodeInterrupt`, `HookBoundaryBlocked`, 취소 | 아니요 | 아니요. 실행은 일시 정지하거나 중단됩니다. 원인 사슬 어디에 있든 Hook 차단이나 취소는 모든 도구 경로에서처럼 `ERROR` 이벤트 없이 `HookBoundaryBlocked`나 `CancelledException` 그 자체로 그래프 실행을 빠져나옵니다 |

`graph::classify_failure(error, run_token)`는 어떤 예외에 대해서든 이 판정을 반환합니다(`NodeFailure` 레코드를 지닌 `NodeExecutionError`라면 그 레코드의 판정). 분류, 코드, `public_message`, 재시도나 라우팅 가능 여부, 그리고 실행 제어 분류라면 신호 그 자체(`FailureVerdict::signal`)입니다. 시도를 다 쓴 레코드에서는 `retryable`이 `false`이고, `retryable_by_class`가 레코드의 `retryable`을 유지합니다. 프로토콜 호스트는 이 코드를 와이어로 보고합니다([프로토콜 실패 매핑](#protocol-failure-mapping)).

`NG.` 접두사는 예약되어 있습니다. 작성자 코드는 `[a-z][a-z0-9_]*(\.[a-z0-9_]+)+`에 맞아야 합니다. C++ 노드는 `NodeFailureError(code, public_message, retryable)`를 던집니다. Python 노드는 문자열 `code` 속성과, 선택적으로 불리언 `retryable`과 문자열 `public_message`를 가진 아무 예외나 발생시키면 되며, 호출자는 여전히 원래 예외를 받습니다. 실행기가 노드를 포기하면 던지는 `NodeExecutionError`에 `NodeFailure` 레코드가 실리며, `node_failure_of()`가 이를 반환합니다. 레코드에는 코드, 분류, 재시도를 멈춘 이유(`exhausted`, `non_retryable`, `effect_committed`, 실행의 재시도 상한에 이르면 `ceiling`, 다음 시도 전의 백오프가 실행의 마감 시각을 넘었을 것이면 `deadline`, 노드가 `max_visits`를 넘어 활성화되면 `visit_limit`), 시도 횟수, 재시도 가능 및 라우팅 가능 플래그, `public_message`, 그리고 마지막 시도의 효과가 `{tool, ordinal, status}` 형태로 담깁니다. `what()`은 절대 복사하지 않습니다. 노드의 `non_retryable`에 적힌 코드는 재시도하지 않습니다.

**라우팅할 수 있는 실패는 실행을 실패시키는 대신 처리될 수 있습니다.** 노드에 failure edge(`failure_edges`, [Program 그래프 기능](program-graphs.md#failure-routing))가 있으면 실행기는 아무것도 던지지 않습니다. 실패한 시도는 아무것도 쓰지 않고, 구조화된 레코드 하나가 edge의 `error_channel`로 가며, 노드는 edge의 대상으로만 라우팅됩니다. 레코드에는 정확히 `node`, `kind`, `code`, `attempts`, `retryable`, `step`, `routed_to`, `effects`가 있고, 노드가 `error_text`로 선택하고 호스트의 `EngineConfig::failure_text_redactor`가 `public_message`에서 텍스트를 남길 때만 `detail.untrusted_text`가 추가됩니다. 노드는 트레이스와 `interruptAfter`에서 실행된 것으로 세며, 노드의 대기 쓰기에 처리된 결과가 담기므로 재개는 노드를 다시 실행하지 않고 라우트를 재생하고, 노드의 `ERROR` 이벤트는 `handled: true`와 `routed_to`를 알려 줍니다. 정적 노드, 팬아웃 분기, `Send` 대상, 서브그래프 노드가 동기와 비동기 진입점 모두에서 이 경로를 따릅니다. 라우팅할 수 없는 실패나 failure edge가 없는 노드의 실패는 위와 같이 던져집니다.

**시도마다 고유한 효과 네임스페이스가 있습니다.** 시도 0은 태스크 id(`s3:act`)를 사용하므로 기존 저널의 슬롯이 그대로 유지됩니다. 시도 k는 `s3:act#a<k>`를 사용하며, 그 안에서 호출 서수가 다시 시작됩니다. Tool 효과, 제공자 호출, 서브그래프 스레드가 모두 시도를 따르므로, 재시도가 이전 시도의 효과를 재생하거나 그것과 충돌하는 일이 없습니다. `ProviderCallBroker`는 재시도마다 새 호출 슬롯을 봅니다. 시도 k가 시작되기 전에 실행기는 그 시도의 네임스페이스를 키로 하는 빈 대기 쓰기를 기록하며, 체크포인트 저장소의 스키마는 바뀌지 않습니다. 크래시나 인터럽트 후의 재개는 시작된 가장 최근 시도를 그 네임스페이스에서 이어 가므로, `ToolEffectBroker`는 그 시도가 커밋한 것을 재생하고 남은 재시도 횟수도 유지됩니다. `ToolEffectBroker` 아래에서, 멱등하지 않은 효과(`ReadOnly`나 `ExternalRead`가 아니고 `Idempotent`도 아닌 정책으로 성공한 호출)를 커밋한 시도는 절대 재시도되지 않으며, 결과를 알 수 없는 효과는 노드가 그 인터럽트를 잡고 다른 오류를 발생시켰더라도 `tool_effect_reconciliation` 인터럽트로 실행을 일시 정지시킵니다. 노드가 실행한 서브그래프의 효과는 그 노드의 시도에 포함됩니다.

**시도 타임아웃과 실행의 마감 시각.** 노드별 정책에 `attempt_timeout_ms`가 있으면(또는 정의의 값을 줄이기만 할 수 있는 호스트 타임아웃 `GraphEngine::set_node_attempt_timeout_ms`가 있으면) 노드의 각 시도는 실행을 구동하는 코루틴 밖에서 실행되고, 타임아웃이 지나면 자기 자식 `CancelToken`으로 취소됩니다. 먼저 일어난 종료 이벤트가 이기므로, 타임아웃 시각이나 그 뒤의 완료는 타임아웃입니다. 타임아웃된 시도의 늦은 완료는 버려집니다. 쓰기도, 보류 쓰기도, 타임아웃 뒤의 이벤트도 없으며, 그 시도가 시작하는 중개된 Tool 호출은 거부됩니다. 시도는 멈춘 뒤에 `NG.Timeout`으로 실패하고, 다음 시도는 그때에야 시작합니다. 정지 마감 시각(다시 타임아웃만큼, 최소 1초) 안에 멈추지 않는 시도는 `NG.EffectUncertain`으로 실패하므로 실행이 실패합니다. 그 시도는 백그라운드에서 계속되며 자기 노드를 살려 두고, `wait_for_node_attempts()`가 이를 기다립니다. 다음 시도 전의 백오프가 `RunContext::deadline`(Program의 wall-time 예산)을 넘게 되면 노드는 kind `deadline`으로 재시도를 멈추고 실패의 코드를 유지합니다. Program 실행에서는 모든 노드 본문이 이렇게 Program 스케줄러 밖에서 실행됩니다. [Program 그래프 기능](program-graphs.md#attempt-timeouts-and-the-deadline)을 참고하세요.

---

## 4. Graph Types

**Header:** `<neograph/graph/types.h>`
**Namespace:** `neograph::graph`

Core types for the graph engine: channels, edges, events, and control-flow primitives.

### ReducerType

Determines how channel values are merged when written by multiple nodes.

```cpp
enum class ReducerType {
    OVERWRITE,  // New value replaces old value
    APPEND,     // New value is appended (for array channels)
    CUSTOM      // User-defined reducer function
};
```

### ReducerFn

Signature for custom reducer functions.

```cpp
using ReducerFn = std::function<json(const json& current, const json& incoming)>;
```

| Parameter | Description |
|-----------|-------------|
| `current` | The current channel value |
| `incoming` | The new value being written |

**Returns:** The merged result that becomes the new channel value.

### Channel

Internal representation of a named, versioned state channel with an associated reducer.

```cpp
struct Channel {
    std::string name;                              // Channel name
    ReducerType reducer_type = ReducerType::OVERWRITE; // Merge strategy
    ReducerFn   reducer;                           // Custom reducer (when type == CUSTOM)
    ChannelLifecyclePolicy lifecycle;
    json        value;                             // Current value
    uint64_t    version = 0;                       // Shared write counter at the last write
};
```

### ChannelWrite

A single write operation targeting a named channel. Nodes return vectors of these.

```cpp
struct ChannelWrite {
    enum class Mode { Reduce, Overwrite };
    std::string channel;
    json value;
    Mode mode = Mode::Reduce;
    std::shared_ptr<const std::vector<sp::Message>> native_messages;
};
```

`ChannelLifecyclePolicy`는 retention(`Unbounded`, `Latest`, `Bounded`와 `retention_limit`)과 persistence(`Checkpoint`, `Ephemeral`)를 분리한다. `ChannelWrite::Mode::Overwrite`는 리듀서를 건너뛴 뒤 retention을 적용한다. 실제 SDK 기록을 보존하려면 `provider_messages_write(messages_or_outcome)`을 쓰며 JSON-only 쓰기는 native replay 권한을 만들지 않는다. Resume guard와 결합 순서는 [채널 lifecycle](concepts.md#channel-lifecycle-and-checkpoint-contract)을 참조한다.

### NodeInterrupt

Exception type thrown from within a node to trigger a dynamic breakpoint (human-in-the-loop).
When thrown, execution pauses, a checkpoint is saved, and the interrupt can be resumed later.

```cpp
class NodeInterrupt : public std::runtime_error {
public:
    explicit NodeInterrupt(const std::string& reason);
    NodeInterrupt(const std::string& reason, json value);   // with a payload
    const std::string& reason() const;
    const json&        value()  const;   // null when no payload was attached
    const std::string& node()   const;   // stamped by the executor
};
```

| Method | Returns | Description |
|--------|---------|-------------|
| `reason()` | `const std::string&` | The reason string passed to the constructor |
| `value()` | `const json&` | The structured payload, or null if none was attached |
| `node()` | `const std::string&` | The node that threw. The executor stamps this — a node body does not know what the graph definition called it |

**The round trip.** An approval prompt needs information to travel in both
directions: the node says *what* needs approving, and the human's answer has to
come back to the node that asked.

```cpp
asio::awaitable<NodeResult> run(NodeInput in) override {
    // The human's answer. Empty until someone has actually answered — which is
    // how you tell "nobody has looked yet" from "the answer was no".
    const auto& verdict = in.ctx.resume_value;

    if (needs_approval(in.state) && !verdict) {
        throw NodeInterrupt("shell command needs approval",
                            json{{"tool", "shell"}, {"cmd", "rm -rf build/"}});
    }
    if (verdict && !verdict->value("approved", false)) {
        co_return refused();
    }
    co_return proceed();
}
```

The caller sees the pause as a normal `RunResult` — `NodeInterrupt` is not
re-thrown at them:

```cpp
auto r = engine->run(cfg);
if (r.interrupted) {
    r.interrupt_node;                          // "risky"  — which node paused
    r.interrupt_value["reason"];               // the sentence, for a human
    r.interrupt_value["value"];                // the payload, to branch on
                                               //   (key absent if none attached)
    engine->resume(cfg.thread_id, json{{"approved", true}});   // the answer
}
```

`resume_value`는 그래프에 `messages` 채널이 있으면 그 채널에 사용자 턴으로도
전달되며, 채팅 형태의 그래프는 예전부터 이 방식으로 값을 받아 왔다. 다만 도구
게이트가 인터럽트한 뒤처럼 어시스턴트 턴의 도구 호출에 아직 결과가 없을 때는
전달되지 않는다. 그 자리의 사용자 턴은 호출과 결과 사이에 끼어들어 다음 모델
호출이 거부되기 때문이다(#544). 게이트는 `ToolGateContext::resume_value`에서
응답을 읽는다. `ctx.resume_value`는 일반 경로이며, 그래프의 채널 이름과
상관없이 동작한다.

This is the *dynamic* form of interruption. The *static* form —
`interrupt_before` / `interrupt_after` in the graph definition — pauses at a
node chosen when the graph was written, which cannot express "pause only if the
model asked for something dangerous".

### Send

Represents a dynamic fan-out request. A node can return `Send` objects to dispatch
one or more nodes with different inputs, enabling map-reduce patterns.

```cpp
struct Send {
    std::string target_node;  // Node to dispatch
    json        input;        // Channel writes for that invocation
};
```

The engine executes each `Send` target with its own input, then continues the graph
after all sends complete. Multiple sends to the same node run in sequence.

### Command

Combined routing override and state update. A node returns a `Command` to simultaneously
write state updates AND redirect execution to a specific next node, bypassing normal
edge routing.

```cpp
struct Command {
    std::string               goto_node;  // Next node (overrides edge routing)
    std::vector<ChannelWrite> updates;    // State updates to apply
};
```

| Field | Type | Description |
|-------|------|-------------|
| `goto_node` | `std::string` | Name of the node to execute next. Overrides normal edge resolution |
| `updates` | `std::vector<ChannelWrite>` | Channel writes to apply before routing |

### RetryPolicy

Configures automatic retry behavior for node execution failures.

```cpp
struct RetryPolicy {
    int   max_retries        = 0;      // 0 = no retry
    int   initial_delay_ms   = 100;    // First retry delay in milliseconds
    float backoff_multiplier = 2.0f;   // Exponential backoff factor
    int   max_delay_ms       = 5000;   // Maximum delay cap in milliseconds
    float jitter_pct         = 0.0f;   // Per-retry jitter as a fraction of
                                       // the computed delay (0.25 = ±25%).
                                       // Default 0 = back-compat. Per-thread
                                       // RNG, no global state.
};
```

Delay for retry `n` is `min(initial_delay_ms * backoff_multiplier^n, max_delay_ms)`,
optionally multiplied by `1 + uniform(-jitter_pct, +jitter_pct)` when
`jitter_pct > 0`.

어떤 실패를 재시도하는지, 그리고 시도마다 효과를 어떻게 분리하는지는 노드 실패 계약(3절)에 설명되어 있습니다.

### StreamMode

Bitfield flags controlling which events are emitted during streaming execution.

```cpp
enum class StreamMode : uint8_t {
    EVENTS  = 0x01,  // NODE_START, NODE_END, INTERRUPT, ERROR
    TOKENS  = 0x02,  // LLM_TOKEN (individual tokens from streaming LLM calls)
    VALUES  = 0x04,  // Full state snapshot after each step
    UPDATES = 0x08,  // Channel write deltas per node
    DEBUG   = 0x10,  // Internal debug info (retry attempts, routing decisions)
    ALL     = 0xFF   // All event types
};
```

Combine flags with bitwise OR:

```cpp
StreamMode mode = StreamMode::EVENTS | StreamMode::TOKENS;
```

**Operators:**

```cpp
StreamMode operator|(StreamMode a, StreamMode b);  // Combine flags
StreamMode operator&(StreamMode a, StreamMode b);  // Mask flags
bool has_mode(StreamMode flags, StreamMode test);   // Test if flag is set
```

### Edge

A static directed edge between two nodes.

```cpp
struct Edge {
    std::string from;  // Source node name
    std::string to;    // Target node name
};
```

Use the special constants `START_NODE` and `END_NODE` for graph entry and exit points.

### ConditionalEdge

A dynamic edge whose target is determined at runtime by a named condition function.

```cpp
struct ConditionalEdge {
    std::string from;                              // Source node name
    std::string condition;                         // Name in ConditionRegistry
    std::map<std::string, std::string> routes;     // condition_result -> target node name
};
```

At runtime, the engine calls the condition function (looked up by name in `ConditionRegistry`).
The function's return value is used as a key into the `routes` map to determine the next node.

### NodeContext

Dependency injection container passed to node constructors. Provides access to the
LLM provider, tools, and configuration.

```cpp
struct NodeContext {
    ProviderControls provider_controls;
    std::shared_ptr<Provider> provider;   // LLM provider
    ToolSet                  tools;      // Owned fixed collection of available tools
    std::string               model;      // Explicit model name; no provider default
    std::string               instructions; // System prompt / instructions
    json                      extra_config; // Additional configuration (node-type-specific)
};
```

Set `NodeContext::tools = ToolSet(std::move(tools))`, or supply
`EngineResources::tools` when the context has no tools. Compilation and the
engine share ownership of the exact collection; reassigning the context cannot
invalidate an earlier engine. Factories may use `ctx.tools.view()` for temporary
raw lookup. Python and MCP tools use the same compile-time ownership contract.

Python `NodeContext(provider=...)`와 `provider` setter는 실제 네이티브 공유
포인터에 연결된 컨텍스트별 소유자 lease로 원래 Python provider 객체의 수명을 유지한다.
컴파일된 노드와 엔진이 보유한 네이티브 컨텍스트 복사본은 컨텍스트를 재할당하거나
Python wrapper가 수거된 뒤에도 같은 Python override 소유자를 유지한다.
재할당은 변경 가능한 컨텍스트의 lease만 해제하며, 기존 컴파일 스냅샷은 자신의
복사본을 유지한다. Lease의 최종 deleter는 GIL을 획득한다. 빌린 C++ 참조나
raw pointer만으로는 Python override 소유자를 유지할 수 없다.

### GraphEvent

Event emitted during streaming graph execution.

```cpp
struct GraphEvent {
    enum class Type {
        NODE_START,     // A node is about to execute
        NODE_END,       // A node has finished executing
        LLM_TOKEN,      // A single token from a streaming LLM call
        CHANNEL_WRITE,  // A channel value was updated
        INTERRUPT,      // Execution paused (NodeInterrupt or configured breakpoint)
        ERROR           // An error occurred during execution
    };

    Type        type;       // Event type
    std::string node_name;  // Name of the node that produced this event
    json        data;       // Event payload (varies by type)
};
```

**Event data payloads:**

| Type | `data` contents |
|------|-----------------|
| `NODE_START` | `{}` or node metadata |
| `NODE_END` | 기본은 `null`이고, 노드가 Command를 돌려주면 `command_goto`, Send를 돌려주면 `sends`(개수), Send로 시작된 실행이면 `send: true`가 든 객체입니다 |
| `LLM_TOKEN` | `{"token": "..."}` |
| `CHANNEL_WRITE` | `{"channel": "...", "value": ...}` |
| `INTERRUPT` | `{"reason": "...", "node": "..."}` |
| `ERROR` | `{"error": "...", "node": "..."}` |

### GraphStreamCallback

Type alias for the graph event callback used in streaming execution.

```cpp
using GraphStreamCallback = std::function<void(const GraphEvent&)>;
```

`GraphEvent` remains the stable callback and JSON-facing shape. Code that wants
typed payloads can adapt the same stream without changing the engine entry
point:

```cpp
using TypedGraphEvent = std::variant<NodeStartEvent, NodeEndEvent,
    LlmTokenEvent, ChannelWriteEvent, StateSnapshotEvent, RoutingEvent,
    SendDispatchEvent, InterruptEvent, ErrorEvent, RawGraphEvent>;

auto callback = adapt_typed_stream([](const TypedGraphEvent& event) {
    std::visit([](const auto& typed) {
        // Handle NodeStartEvent, LlmTokenEvent, and the other alternatives.
    }, event);
});
```

`to_typed_event()` performs the conversion directly. Malformed payloads and
payload shapes introduced by future versions become `RawGraphEvent` rather
than throwing from the streaming callback.

### NodeResult

Extended return type from node execution. Wraps channel writes with optional
`Command` and `Send` directives for advanced control flow.

```cpp
struct NodeResult {
    std::vector<ChannelWrite> writes;           // Channel updates
    std::optional<Command>    command;           // Routing override (if set)
    std::vector<Send>         sends;             // Dynamic fan-out targets

    NodeResult() = default;
    NodeResult(std::vector<ChannelWrite> w);     // Implicit from plain writes
};
```

When `command` is set, normal edge routing is bypassed and execution jumps to
`command->goto_node`. When `sends` is non-empty, the engine performs dynamic
fan-out to the specified targets.

### ConditionFn

Signature for condition functions used in conditional edges.

```cpp
using ConditionFn = std::function<std::string(const GraphState&)>;
```

The function inspects the current graph state and returns a string key. This key is
looked up in the `ConditionalEdge::routes` map to determine the next node.

### Constants

```cpp
constexpr const char* START_NODE = "__start__";  // Graph entry point
constexpr const char* END_NODE   = "__end__";    // Graph termination
```

These are used in edge definitions to mark graph entry and exit:

```cpp
Edge{START_NODE, "my_first_node"}
Edge{"my_last_node", END_NODE}
```

---

## 5. GraphState

**Header:** `<neograph/graph/state.h>`
**Namespace:** `neograph::graph`

Thread-safe, versioned key-value state container for the graph. Each entry is a
named channel with an associated reducer that controls how values are merged.

```cpp
class GraphState {
public:
    void init_channel(const std::string& name,
                      ReducerType type,
                      ReducerFn reducer,
                      const json& initial_value = json(),
                      ChannelLifecyclePolicy lifecycle = {});

    json get(const std::string& channel) const;
    std::vector<ChatMessage> get_messages() const;
    std::vector<sp::Message> get_provider_messages(
        const std::string& channel = "messages") const;
    std::optional<std::vector<sp::Message>> captured_provider_messages(
        const std::string& channel = "messages") const;

    void write(const std::string& channel, const json& value);
    void apply_writes(const std::vector<ChannelWrite>& writes);

    uint64_t channel_version(const std::string& channel) const;
    uint64_t global_version() const;

    json serialize() const;
    void restore(const json& data);
    json serialize_runtime() const;
    void restore_runtime(const json& data);
    json ephemeral_checkpoint_guard() const;
    void restore_checkpoint(const json& data, const json& guard,
                            std::shared_ptr<const NativeGraphCheckpoint> native = {});
    std::pair<json, std::shared_ptr<const NativeGraphCheckpoint>> checkpoint_snapshot() const;

    std::vector<std::string> channel_names() const;
};
```

| Method | Description |
|--------|-------------|
| `init_channel(name, type, reducer, initial_value)` | Register a channel with its reducer and optional initial value. Must be called before any read/write to that channel |
| `get(channel)` | Read the current value of a channel. Thread-safe (shared lock) |
| `get_messages()` | Convenience method: reads the `"messages"` channel and deserializes it as `std::vector<ChatMessage>` |
| `write(channel, value)` | Write a value to a single channel through its reducer. Thread-safe (exclusive lock) |
| `apply_writes(writes)` | Atomically apply a batch of `ChannelWrite` operations. All writes are applied under a single exclusive lock |
| `channel_version(channel)` | 해당 채널의 버전을 반환한다. 마지막으로 쓴 시점의 공유 쓰기 카운터 값이며, 한 번도 쓰지 않았으면 0이다 |
| `global_version()` | 공유 쓰기 카운터를 반환한다. 어느 채널에 쓰든(실행 입력의 쓰기 포함) 쓸 때마다 1씩 늘어난다 |
| `serialize()` | Serializes checkpoint-persistent channel values and versions |
| `restore(data)` | Restores channel values and versions from serialized JSON |
| `channel_names()` | Returns the names of all initialized channels |

`serialize()`는 checkpoint-persistent 채널값과 version만 포함하며 ephemeral 값은 생략한다. `restore_checkpoint`는 일치하는 guard를 요구하고 이미 쓴 ephemeral 상태가 유실되면 거부한다. `serialize_runtime` / `restore_runtime`은 같은 process 복사에 live ephemeral 값을 보존하며 영속 저장용이 아니다. `checkpoint_snapshot()`은 portable snapshot과 실제 C++ native sidecar를 함께 반환한다. `get_messages()`는 편의 projection이다. 전체 SDK 기록에는 `get_provider_messages()`를 쓰고 임의 JSON을 chat으로 해석하지 않으려면 `captured_provider_messages()`를 쓴다.

---

## 6. GraphNode

**Header:** `<neograph/graph/node.h>`
**Namespace:** `neograph::graph`

Nodes are the computational units of a graph. The library provides an
abstract base class and four built-in node types.

### GraphNode (abstract)

A subclass overrides ONE method: `run(NodeInput) -> awaitable<NodeOutput>`.
Read state, decide what to do, return writes (and optionally `Command` /
`Send`).

```cpp
class GraphNode {
public:
    virtual ~GraphNode() = default;

    // The only custom-node dispatch entry.
    virtual asio::awaitable<NodeOutput> run(NodeInput in) = 0;

    virtual std::string get_name() const = 0;
};

struct NodeInput {
    const GraphState&          state;       // channels visible to this node
    const RunContext&          ctx;         // cancel_token, step, thread_id, ...
    const GraphStreamCallback* stream_cb;   // null when not streaming
};

using NodeOutput = NodeResult;  // writes + optional Command + optional Sends
```

| Member | Description |
|--------|-------------|
| `in.state` | Read-only `GraphState`. Use `in.state.get(channel)` for reads |
| `in.ctx.cancel_token` | `provider.invoke(std::move(request))` 전에 `request.cancel_token = in.ctx.cancel_token`을 대입한다. Provider 취소는 지원되는 경계에서 협력적으로 처리된다. 직접 작성한 루프에서는 null이 아닌 `ctx.cancel_token`의 `ctx.cancel_token->is_cancelled()`를 확인한다 |
| `in.ctx.step` | Current super-step index |
| `in.ctx.thread_id` | Mirrors `RunConfig::thread_id` |
| `in.stream_cb` | Streaming sink; if non-null, emit `LLM_TOKEN` events through it. Null on non-streaming runs |
| Return: `NodeOutput.writes` | Channel writes the engine merges via reducers |
| Return: `NodeOutput.command` | Optional routing override (`goto_node` + state updates) |
| Return: `NodeOutput.sends` | Optional dynamic fan-out — engine spawns one branch per `Send` |
| `get_name()` | Returns the node's unique name within the graph |

Minimal example:

```cpp
class CounterNode : public neograph::graph::GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto current = in.state.get("count");
        int n = current.is_number() ? current.get<int>() : 0;
        NodeOutput out;
        out.writes.push_back({"count", n + 1});
        co_return out;
    }
    std::string get_name() const override { return "counter"; }
};
```

Async-native LLM call:

```cpp
#include <neograph/graph/node.h>
#include <neograph/graph/run_context.h>
#include <neograph/provider.h>
#include <neograph/runtime_interposition_consumer.h>

class ChatNode : public neograph::graph::GraphNode,
                 public neograph::RuntimeInterpositionConsumer {
    std::shared_ptr<neograph::Provider> provider_;
    std::string model_;
public:
    ChatNode(std::shared_ptr<neograph::Provider> provider, std::string model)
        : provider_(std::move(provider)), model_(std::move(model)) {}
    asio::awaitable<neograph::graph::NodeOutput>
    run(neograph::graph::NodeInput in) override {
        auto request = neograph::make_provider_request(
            *provider_, model_, in.state.get_provider_messages());
        request.cancel_token = in.ctx.cancel_token;
        request.options.deadline = in.ctx.deadline;
        auto result = co_await neograph::graph::observe_provider_result(
            in.ctx, invoke_provider(provider_, std::move(request), {}, {},
                neograph::graph::provider_call_broker(in.ctx),
                neograph::graph::make_provider_call_identity(in.ctx, get_name())));
        neograph::graph::record_usage(in.ctx, result);
        neograph::outcome_or_throw(result);
        neograph::graph::NodeOutput out;
        out.writes.push_back(neograph::graph::provider_messages_write(result));
        co_return out;
    }
    std::string get_name() const override { return "chat"; }
};
```

> **마이그레이션 참고.** `GraphNode`에는 하나의 노드 진입점만 있습니다:
> `run(NodeInput)`. 이는 `Command`와 `Send`를 보존하고 비동기 및 스트리밍 실행에
> 참여하며, 서브클래스가 구현해야 하는 오버라이드입니다.

### LLMCallNode

Calls the LLM with the current conversation state. Reads from the
`"messages"` channel, sends a completion request to the provider, and
writes the assistant's response back. Streams `LLM_TOKEN` events when
the run was started via `run_stream` / `run_stream_async`.

```cpp
class LLMCallNode : public GraphNode {
public:
    LLMCallNode(const std::string& name, const NodeContext& ctx);
    asio::awaitable<NodeOutput> run(NodeInput in) override;
    std::string get_name() const override;
};
```

| Constructor Parameter | Description |
|-----------------------|-------------|
| `name` | Node name |
| `ctx` | Node context providing the LLM provider, tools, model, and instructions |

(LLMCallNode, `ToolDispatchNode`, `IntentClassifierNode`, and `SubgraphNode`
all implement the same `run(NodeInput)` contract.)

### ToolDispatchNode

Dispatches tool calls from the latest assistant message. Reads pending tool calls from
the `"messages"` channel, executes each tool, and writes tool result messages back.

```cpp
class ToolDispatchNode : public GraphNode {
public:
    ToolDispatchNode(const std::string& name, const NodeContext& ctx);

    asio::awaitable<NodeOutput> run(NodeInput in) override;
    std::string get_name() const override;
};
```

| Constructor Parameter | Description |
|-----------------------|-------------|
| `name` | Node name |
| `ctx` | Node context (uses `ctx.tools` to look up and execute tools) |

### IntentClassifierNode

Uses the LLM to classify user intent, then writes the classification result to the
`"__route__"` channel. Designed for use with the `"route_channel"` built-in condition
to enable dynamic intent-based routing.

```cpp
class IntentClassifierNode : public GraphNode {
public:
    IntentClassifierNode(const std::string& name, const NodeContext& ctx,
                         const std::string& prompt,
                         std::vector<std::string> valid_routes);

    asio::awaitable<NodeOutput> run(NodeInput in) override;
    std::string get_name() const override;
};
```

| Constructor Parameter | Type | Description |
|-----------------------|------|-------------|
| `name` | `std::string` | Node name |
| `ctx` | `NodeContext` | Provider, model and `provider_controls` for the classification LLM call. The node sends the controls as set and adds no temperature or output cap of its own |
| `prompt` | `std::string` | Classification prompt template |
| `valid_routes` | `std::vector<std::string>` | Allowed classification values. The LLM output is validated against these |

### SubgraphNode

Wraps a compiled `GraphEngine` as a single node, enabling hierarchical graph composition
(supervisor pattern, nested workflows). Channel mappings control data flow between
parent and child graphs.

```cpp
class SubgraphNode : public GraphNode {
public:
    SubgraphNode(const std::string& name,
                 std::shared_ptr<GraphEngine> subgraph,
                 std::map<std::string, std::string> input_map = {},
                 std::map<std::string, std::string> output_map = {},
                 SubgraphPersistence persistence = SubgraphPersistence::Legacy);
    asio::awaitable<NodeOutput> run(NodeInput in) override;
    std::string get_name() const override;
};
```

| Constructor Parameter | Type | Description |
|-----------------------|------|-------------|
| `name` | `std::string` | Node name in the parent graph |
| `subgraph` | `std::shared_ptr<GraphEngine>` | The compiled child graph engine |
| `input_map` | `std::map<std::string, std::string>` | `parent_channel -> child_channel` mapping. Read from parent, write to child input |
| `output_map` | `std::map<std::string, std::string>` | `child_channel -> parent_channel` 매핑. 자식이 생성한 write delta의 채널명을 바꿔 부모로 전달 |

맵이 비어 있으면 채널 이름을 그대로 사용하는 identity mapping이 적용됩니다.

입력 매핑은 부모의 현재 채널 값을 자식 입력으로 복사합니다. 출력 매핑은 의도적으로
다르게 동작합니다. 자식의 최종 직렬화 상태를 새 reducer 입력으로 취급하지 않고, 자식이
생성한 순서대로 `ChannelWrite` delta를 전달하며 각 write의 `Mode`를 보존합니다. 따라서
상속된 append/custom 값이 두 번 적용되지 않습니다. 출력 매핑은 snapshot replacement를
추론하지 않습니다. 매핑된 부모 값을 교체하려면 자식이 명시적으로
`ChannelWrite::Mode::Overwrite`를 내보내야 합니다.

#### 자식 영속성과 검사

| 모드 | 자식 checkpoint namespace | 시작/resume | Store 우선순위 |
|------|----------------------------|--------------|------------------|
| `Legacy` (기본) | 길이 구분 parent thread, node, parent step, task ID (`subgraph/...`) | 새 parent는 새 child를 시작하고 parent resume은 대응 snapshot을 로드 | Parent run의 checkpoint backend가 있으면 사용, 없으면 child 설정 |
| `PerInvocation` | `subgraph/run/` + parent thread, node, 영속 parent graph-invocation UUID, step, task ID | 새 parent run마다 새 namespace; resume은 UUID와 child write journal 복원 | Parent, 다음 child |
| `PerThread` | `subgraph/thread/` + parent thread, node | 새 호출은 이전 checkpoint로 child state를 seed하고 새 input 적용; parent resume은 대응 snapshot 사용; 같은 compiled node/namespace 중첩 호출은 오류 | Parent, 다음 child |
| `Stateless` | 없음 | Child checkpoint 비활성화; interrupt/resume은 거부하지만 Store, 취소, ToolGate는 전달 | Checkpoint backend 없음; parent Store, 다음 child Store |

명시적인 stateful 모드는 비어 있지 않은 parent thread ID를 요구한다. `Legacy`는 #238 이전 namespace와 checkpoint wire format 및 empty-thread 동작을 유지한다. `PerInvocation`은 parent metadata에 `_neograph.subgraph_invocation_id`를 기록하므로 이 값이 없는 과거 checkpoint는 정책 변경 후 resume할 수 없다. `PerThread`는 namespace를 공유한다. 서로 다른 engine/process가 같은 backend를 사용하면 host도 admission을 조정해야 하며 node-local guard는 compiled node 하나만 보호한다. 명시적 migration 없이 기존 thread의 정책을 바꾸지 않는다.

`GraphEngine::inspect_nested_checkpoint(root_thread, path[, run_store])`로 자식과 손자의 checkpoint를 조회한다. 각 `SubgraphPathStep`은 child node name, parent super-step, stable Core task ID (`s0:child` 또는 Send task ID), 선택적 exact parent checkpoint ID를 제공한다. 결과는 `graph_path`, child `thread_id`, 전체 `Checkpoint`(채널값과 checkpoint ID 포함)를 담는다. 다른 thread의 checkpoint ID나 stateless path는 거부한다. `RunResources`로 backend를 override했다면 같은 run-scoped store를 전달한다. `SubgraphNode::checkpoint_thread_id()`는 namespace 한 segment를 재구성한다.

#### 런타임 컨텍스트 전파

`SubgraphNode`는 엔진 경계에서 자식 실행 컨텍스트를 파생합니다. 공개 `RunContext`
레이아웃은 변경하지 않습니다.

| 컨텍스트 값 | 자식 의미 |
|---------------|-----------------|
| `cancel_token` | 하위 작업 토큰을 생성하므로 부모 취소가 모든 자식과 손자에 도달합니다. |
| `usage`, `deadline`, `trace_id`, `stream_mode` | 상속됩니다. `deadline`과 `trace_id`는 `RunMetadata`에서 오며, 자식은 부모의 stream mode를 넓힐 수 없습니다. |
| `thread_id` | 부모 thread ID가 비어 있지 않으면 부모 ID, subgraph 노드명, super-step, invocation identity에서 결정적으로 파생합니다. 따라서 sibling `Send` 호출은 서로 다른 checkpoint identity를 받습니다. 빈 부모 thread ID는 자식도 scope 없이 유지해 checkpointing을 비활성화합니다. |
| `step` | 자식 실행에 로컬이며 자식 checkpoint 또는 0에서 시작합니다. |
| `store` | 부모 Store가 있으면 상속하고, 없으면 자식 엔진에 설정된 Store를 유지합니다. |
| Tool policy | 부모 `ToolGate`가 자식 gate보다 먼저 실행됩니다. 자식은 허용된 호출을 더 제한하거나 rewrite할 수 있지만 부모 deny/interrupt를 우회할 수 없습니다. |
| Checkpoint backend and resume value | 부모 backend가 있으면 상속하고, 없으면 자식 backend를 유지합니다. 부모 resume은 파생된 자식 checkpoint identity가 존재할 때만 해당 자식 checkpoint를 resume하며 null이 아닌 resume value를 전달합니다. Checkpoint routing은 공개 `RunContext` 필드가 아니라 내부 구현입니다. |

---

## 7. GraphEngine

**Header:** `<neograph/graph/engine.h>`
**Namespace:** `neograph::graph`

The core execution engine. Compiles graph definitions, manages state transitions,
and orchestrates node execution through a super-step loop.

### EngineConfig and EngineResources

New code should assemble construction dependencies and policies before creating
the engine:

```cpp
struct EngineConfig {
    NodeContext node_context;
    std::shared_ptr<CheckpointStore> checkpoint_store;
    std::shared_ptr<Store> store;
    std::optional<RetryPolicy> retry_policy;
    std::map<std::string, RetryPolicy> node_retry_policies;
    ToolGate tool_gate;
    std::size_t worker_count = 1;
    std::set<std::string> cached_nodes;
    std::size_t node_cache_max_entries = 0;
    std::map<std::string, CacheKeyPolicy> node_cache_policies;
    std::shared_ptr<ToolExecutionController> tool_execution_controller;
    std::shared_ptr<::neograph::HookRuntime> hook_runtime;
    std::shared_ptr<::neograph::RuntimeInterpositionController> runtime_interposition;
    std::shared_ptr<sp::NativeArchive> native_history_archive;
};

struct EngineResources {
    ToolSet tools;
    std::shared_ptr<const GraphRegistry> registry;
};
```

`ToolSet` is a move-only owner for a fixed tool collection. `GraphRegistry` is
a per-engine reducer, condition, and node-factory overlay; names absent from the
overlay fall back to the existing process-global registries. Configure both
before passing them to `build()` or `link()`. Runtime mutation is intentionally
not part of the local-registry contract.

### RunConfig

Configuration for a single graph execution run.

```cpp
struct RunConfig {
    std::string                 thread_id;
    json                        input;
    int                         max_steps    = 50;
    StreamMode                  stream_mode  = StreamMode::ALL;
    std::shared_ptr<CancelToken> cancel_token;          // v0.3+
    std::shared_ptr<UsageAccumulator> usage;             // optional accumulator
    std::optional<std::vector<sp::Message>> provider_messages;
    std::shared_ptr<sp::NativeArchive> native_history_archive;
    std::function<void(const sp::Event&)> on_provider_event;
    std::shared_ptr<ProviderOutcomes> provider_outcomes;
    std::shared_ptr<ProviderLoopHistory> provider_loop_history;
    std::uint64_t model_token_budget = 0;
    std::shared_ptr<std::atomic_bool> budget_exhausted;
    bool                        resume_if_exists = false; // v0.3.1+
};
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `thread_id` | `std::string` | `""` | Identifies the conversation/session for checkpointing |
| `input` | `json` | `{}` | Initial values written to channels before execution starts. Typically `{"messages": [...]}` |
| `max_steps` | `int` | `50` | Maximum number of super-steps before forced termination (prevents infinite loops) |
| `stream_mode` | `StreamMode` | `ALL` | Bitfield controlling which event types are emitted during streaming |
| `cancel_token` | `std::shared_ptr<CancelToken>` | `nullptr` | Cooperative cancel handle. Engine wraps this into a `RunContext` and threads it to every node's `run(NodeInput)` call as `in.ctx.cancel_token` |
| `usage` | `std::shared_ptr<UsageAccumulator>` | `nullptr` | Optional token accumulator. The engine creates one when omitted and exposes the active accumulator as `in.ctx.usage` |
| `resume_if_exists` | `bool` | `false` | If `true` and a checkpoint exists for `thread_id`, seed from it before applying `input` (multi-turn chat shape) |

### RunContext (v0.4, exposed to nodes via `NodeInput.ctx`)

엔진이 전달하는 실행별 dispatch metadata입니다. 처음에는 `RunConfig`(usage
accumulator가 없으면 생성), `RunMetadata`, 유효 Store, 선택적 resume value로
구성합니다. 노드는 `run(NodeInput) -> NodeOutput` override 안에서 `in.ctx`로
사용합니다.

이 생성은 초기 구성만 설명한다. 체크포인트 복원은 실제 원래 bank와 그 이전 보고로 accumulator를 교체할 수 있다. 따라서 resume이나 continuation은 새 반환 사용량 보고나 이전에 보고한 사용량의 `None`을 보장하지 않는다.

Python `RunMetadata(timeout_ms=None, ...)`에는 기본적으로 deadline이 없다. 생성자와 `set_timeout_ms(timeout)`는 남은 steady-clock 범위 안의 음이 아닌 정수 밀리초를 받으며, signed 변환이나 덧셈 전에 범위를 검사한다. 음수나 범위를 넘는 값은 `OverflowError` 또는 `ValueError`를 발생시킨다. setter가 실패하면 이전 절대 deadline은 유지된다. 0은 즉시 만료되며 `clear_deadline()`은 deadline을 제거한다.

```cpp
struct RunContext {
    std::shared_ptr<CancelToken>  cancel_token;
    std::shared_ptr<UsageAccumulator> usage;
    std::shared_ptr<ProviderOutcomes> provider_outcomes;
    std::shared_ptr<ProviderLoopHistory> provider_loop_history;
    std::function<void(const sp::Event&)> on_provider_event;
    std::shared_ptr<sp::NativeArchive> native_history_archive;
    std::uint64_t model_token_budget = 0;
    std::shared_ptr<std::atomic_bool> budget_exhausted;
    std::string run_id;
    std::shared_ptr<CancelToken> budget_cancel_token;
    std::shared_ptr<OwnedManagedBudgetLease> managed_budget_lease;
    std::shared_ptr<CheckpointStore> managed_budget_store;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::string                   trace_id;
    std::string                   thread_id;
    std::uint64_t                 cache_execution_id = 0;
    int                           step;
    StreamMode                    stream_mode;
    std::optional<json>           resume_value;
    std::shared_ptr<Store>        store;
    ToolGate                      tool_gate;
    std::shared_ptr<ToolExecutionController> tool_execution_controller;
    ToolExecutionIdentity tool_execution_identity;
};
```

| Field | Description |
|-------|-------------|
| `cancel_token` | The active token. Pass to `ProviderRequest::cancel_token` so an LLM HTTP socket aborts on cancel, or poll `is_cancelled()` for your own loops |
| `usage` | Shared token-accounting sink populated by the engine |
| `deadline` | C++ `RunMetadata`의 선택적 절대 deadline |
| `trace_id` | C++ `RunMetadata`의 선택적 trace correlator |
| `thread_id` | Mirror of `RunConfig.thread_id` |
| `step` | Current super-step index, updated each iteration |
| `stream_mode` | Mirror of `RunConfig.stream_mode` |
| `resume_value` | Value supplied to `GraphEngine::resume()`, or empty on a fresh run |
| `store` | Store installed on the engine, or `nullptr` when none is configured |
| `tool_gate` | 상속된 부모 정책을 포함해 이 invocation에 적용되는 유효 정책 |

### CancelToken

Cooperative cancel primitive shared between caller and engine. Construct
via `std::make_shared<CancelToken>()`, hand to `RunConfig.cancel_token`,
and call `cancel()` from any thread to abort the in-flight run —
including the LLM HTTP socket if a node is mid-`provider.invoke_async`.
Each engine run forks its own operation child, so one parent can safely cancel
multiple concurrent runs without sharing an asio cancellation slot.

Engine operation children retain themselves until their posted cancellation
emit executes. If application code calls `bind_executor()` directly on a token
it constructed itself, the application must keep that token alive until the
executor drains; the engine cannot supply ownership for an external object.
Because these methods are inline in the public header, existing C++ consumers
must be recompiled to receive the updated `fork()` lifetime behavior. The
`CancelToken` object layout remains binary-compatible with 0.11.x.

```cpp
class CancelToken {
public:
    void cancel() noexcept;                            // request cancellation
    bool is_cancelled() const noexcept;                // polling read

    std::shared_ptr<CancelToken> fork();                // v0.4: child token
    void bind_executor(asio::any_io_executor ex);
    asio::cancellation_slot slot() noexcept;
};
```

#### Hierarchical cancel (v0.4 `fork()`)

Each child token has its own `cancellation_signal`; the parent's
`cancel()` cascades to every live child. This is the structural
replacement for the v0.3.x `add_cancel_hook` list (deprecated, removed
in v1.0). Concurrent nested scopes — a multi-Send fan-out where every
worker calls `provider.invoke(std::move(request))` simultaneously — each
`fork()` once and never overwrite each other's slot.

```cpp
// Caller side: one parent token, fan it out across N concurrent runs.
auto parent = std::make_shared<neograph::graph::CancelToken>();

RunConfig cfg_a; cfg_a.thread_id = "user-1"; cfg_a.cancel_token = parent;
RunConfig cfg_b; cfg_b.thread_id = "user-2"; cfg_b.cancel_token = parent;

auto fut_a = std::async(std::launch::async, [&] { return engine->run(cfg_a); });
auto fut_b = std::async(std::launch::async, [&] { return engine->run(cfg_b); });

// User hits stop in the UI:
parent->cancel();   // cascades to every fork() child, every run aborts

// Inside a RuntimeInterpositionConsumer node, pass cancellation in the owned request.
// socket aborts on parent cancel without you doing any wiring:
asio::awaitable<NodeOutput> run(NodeInput in) override {
    auto request = neograph::make_provider_request(
        *provider_, model_, in.state.get_provider_messages());
    request.cancel_token = in.ctx.cancel_token;
    request.options.deadline = in.ctx.deadline;
    auto reply = co_await neograph::graph::observe_provider_result(
        in.ctx, invoke_provider(provider_, std::move(request), {}, {},
            neograph::graph::provider_call_broker(in.ctx),
            neograph::graph::make_provider_call_identity(in.ctx, get_name())));
    neograph::graph::record_usage(in.ctx, reply);
    neograph::outcome_or_throw(reply);
    NodeOutput out;
    out.writes.push_back(neograph::graph::provider_messages_write(reply));
    co_return out;
}
```

| Method | Description |
|--------|-------------|
| `cancel()` | Idempotent, thread-safe. Sets the polling flag and emits the asio cancellation_signal on the bound executor; cascades to all live children via `fork()` |
| `is_cancelled()` | Lock-free polling read |
| `fork()` | **v0.4.** Returns a child shared_ptr. Parent.cancel() cascades; if the parent is already cancelled at fork() time the child is constructed pre-cancelled (no emit-vs-bind race) |
| `bind_executor(ex)` | Engine-internal; binds the executor that handles signal emits |
| `slot()` | asio `cancellation_slot` for `bind_cancellation_slot` at `co_spawn` time |

### RunResult

Result returned after graph execution completes or is interrupted.

```cpp
struct RunResult {
    sp::Usage usage;
    std::vector<sp::Message> native_messages;
    std::vector<sp::runtime::Result> provider_outcomes;
    json        output;                          // Final serialized state
    bool        interrupted       = false;       // True if execution was paused (HITL)
    std::string interrupt_node;                  // Node that caused the interrupt
    json        interrupt_value;                 // Value associated with the interrupt
    std::string checkpoint_id;                   // ID of the last checkpoint saved
    std::vector<std::string> execution_trace;    // Ordered list of executed node names

    bool max_steps_exhausted() const noexcept;    // Limit stopped runnable work
    bool barrier_stalled() const noexcept;        // A barrier can no longer fire
    bool failure_exited() const noexcept;         // A failure-exit node completed (#364)
    RunStatus status() const noexcept;            // Completed, Interrupted, StepLimit, SafePoint, BarrierStalled or FailureExit

    template <typename T> T channel(const std::string& name) const;
    template <typename T> T channel(const ChannelKey<T>& key) const;
    template <typename T>
    std::optional<T> try_channel(const ChannelKey<T>& key) const;
};
```

`RunResult::usage`는 nullable provider 보고이며 지출 bank가 아니다. `native_messages`는 실제 typed 기록을, `provider_outcomes`는 각 소유 Completion/Failure를 보존한다. JSON `output`은 portable projection이다. 전체 입력 기록은 `RunConfig::provider_messages`, typed 이벤트는 `on_provider_event`를 사용한다. 영속 native checkpoint/receipt custody에는 `native_history_archive`가 필요하지만 메모리 sidecar에는 필요하지 않다.

`output`의 `channels.<name>.version`과 `global_version`은 모든 채널이 공유하는 쓰기 카운터 하나에서 나온다. 채널의 버전은 그 채널을 마지막으로 쓴 시점의 카운터 값이므로 쓰기의 순서를 나타낼 뿐, 채널이 받은 쓰기 횟수가 아니다. `global_version`도 슈퍼스텝 번호가 아니다. [버전](concepts.md#versions)을 참고한다.

resume이나 continuation에서 `provider_outcomes`는 원래 결과 뒤에 새로 생성된 결과를 이어 순서 있는 목록으로 보존한다. `usage`에도 복원한 bank의 이전 보고가 남을 수 있다. 소비자는 완료된 provider effect를 재dispatch하거나 이중 차감하지 않아야 한다. 비어 있거나 초기화된 결과 목록, `usage=None`은 resume 불변 조건이 아니다.
| Field | Type | Description |
|-------|------|-------------|
| `output` | `json` | 모든 채널의 직렬화된 최종 상태 |
| `interrupted` | `bool` | 인터럽트(HITL)로 실행이 일시 중지되었으면 `true` |
| `interrupt_node` | `std::string` | 인터럽트를 발생시킨 노드 이름 |
| `interrupt_value` | `json` | 인터럽트의 이유 또는 페이로드 |
| `checkpoint_id` | `std::string` | 마지막으로 저장된 체크포인트의 UUID |
| `execution_trace` | `std::vector<std::string>` | 끝난 노드 실행마다 항목 하나. 엔진이 결과를 반영한 순서대로 적는다. 슈퍼스텝은 차례대로, 한 슈퍼스텝 안에서는 ready 노드를 먼저, 그다음 Send를 내보낸 순서대로 적으며 끝난 순서와는 무관하다. Send로 실행된 노드는 `<대상>[send]`로 적힌다. [Send](concepts.md#5-send--dynamic-fan-out) 참고 |

`provider_messages`는 전체 typed 기록을 제공하고 messages 채널만 대체한다. `on_provider_event`는 typed SDK 이벤트를 관측한다. `provider_outcomes`, `provider_loop_history`는 결과와 task-local continuation을 inner turn 사이에 보존한다. `native_history_archive`는 영속 native custody를 결합하며 모델 예산을 주지 않는다. 선택적 `model_token_budget` 상한과 `budget_exhausted` 신호는 예산 인식 dispatch에 사용한다. Python과 C++은 모두 `RunResult.native_messages`로 전체 기록을, `provider_outcomes`로 소유 결과를 반환한다. 입력은 계속 `RunConfig.provider_messages`이며 `RunResult.provider_messages`나 `provider_history` alias는 없다.

`max_steps_exhausted()` returns `true` only when the step ceiling stopped the
run while runnable work remained. A graph that reaches `__end__` exactly on its
last permitted step returns `false`.

`status()` returns `RunStatus::Completed`, `RunStatus::Interrupted`,
`RunStatus::StepLimit`, `RunStatus::SafePoint`, `RunStatus::BarrierStalled`, or `RunStatus::FailureExit` without changing the public `RunResult` data layout.
`BarrierStalled`는 실행할 노드가 없고, 마지막 super-step이 `__end__`로 명시적으로 라우팅하지 않았으며, barrier가 아직 신호를 쥐고 있어 join이 더 이상 발화할 수 없다는 뜻입니다. 이때 `barrier_stalled()`는 true이고, `output["_neograph"]["barrier_stalled"]`는 정체된 join마다 `{node, wait_for, received, missing}`를 나열합니다.
`FailureExit`(#364)는 failure exit 노드가 완료되어 실행이 실패로 끝났다는 뜻입니다. `failure_exited()`가 true이고 `output["_neograph"]["failure_exit"]`는 `{node, code}`입니다. `RunInvocation`은 이를 `Error`로 보고합니다.
`ChannelKey<T>` binds a reusable channel name to its expected C++ type:

```cpp
inline const ChannelKey<std::string> Answer{"answer"};

auto answer = result.channel(Answer);
if (auto optional = result.try_channel(Answer)) {
    std::cout << *optional << '\n';
}
```

### GraphEngine

The main engine class. New code should use `build_strict()` for a JSON definition;
it rejects invalid topology before any node is instantiated. Use `link()` with a
`ValidatedTopology` when parse, validation, inspection, or transformation must be
separate steps. The lenient `build()`, `CompiledGraph` link overloads, `compile()`,
and post-construction setters remain compatibility paths.

```cpp
class GraphEngine {
public:
    // ---- Construction ----

    static std::unique_ptr<GraphEngine> build(
        const json& definition, EngineConfig config);
    static std::unique_ptr<GraphEngine> build(
        const json& definition, EngineConfig config, EngineResources resources);

    static std::unique_ptr<GraphEngine> build_strict(
        const json& definition, EngineConfig config);
    static std::unique_ptr<GraphEngine> build_strict(
        const json& definition, EngineConfig config, EngineResources resources);

    static std::unique_ptr<GraphEngine> link(
        ValidatedTopology topology, EngineConfig config = {});
    static std::unique_ptr<GraphEngine> link(
        ValidatedTopology topology, EngineConfig config, EngineResources resources);

    static std::unique_ptr<GraphEngine> link(
        CompiledGraph graph, EngineConfig config = {});
    static std::unique_ptr<GraphEngine> link(
        CompiledGraph graph, EngineConfig config, EngineResources resources);

    static std::unique_ptr<GraphEngine> compile( // compatibility facade
        const json& definition, const NodeContext& default_context,
        std::shared_ptr<CheckpointStore> store = nullptr);

    // ---- Execution (sync) ----

    RunResult run(const RunConfig& config);

    RunResult run_stream(const RunConfig& config,
                         const GraphStreamCallback& cb);

    RunResult resume(const std::string& thread_id,
                     const json& resume_value = json(),
                     const GraphStreamCallback& cb = nullptr);

    // ---- Execution (async, 3.0) ----

    asio::awaitable<RunResult> run_async(const RunConfig& config);

    asio::awaitable<RunResult> run_stream_async(
        const RunConfig& config, const GraphStreamCallback& cb);

    asio::awaitable<RunResult> resume_async(
        const std::string& thread_id,
        const json& resume_value = json(),
        const GraphStreamCallback& cb = nullptr);

    // ---- State Inspection & Manipulation ----

    GraphAdmin admin(); // borrowed facade; must not outlive this engine

    std::optional<json> get_state(const std::string& thread_id) const;

    std::vector<Checkpoint> get_state_history(const std::string& thread_id,
                                              int limit = 100) const;

    void update_state(const std::string& thread_id,
                      const json& channel_writes,
                      const std::string& as_node = "");

    std::string fork(const std::string& source_thread_id,
                     const std::string& new_thread_id,
                     const std::string& checkpoint_id = "");

    // ---- Compatibility configuration (prefer EngineConfig/EngineResources) ----

    void set_checkpoint_store(std::shared_ptr<CheckpointStore> store);
    void set_store(std::shared_ptr<Store> store);
    std::shared_ptr<Store> get_store() const;
    void set_retry_policy(const RetryPolicy& policy);
    void set_node_retry_policy(const std::string& node_name, const RetryPolicy& policy);

    // Fan-out worker pool. n==1 keeps the engine on the caller's
    // executor (no engine-owned thread_pool); n>=2 installs an
    // owned `asio::thread_pool` of size n. build() defaults to
    // n==1 — prefer EngineConfig::worker_count to opt into
    // real parallel fan-out. Throws `std::logic_error` if called
    // while a run is in flight (an `active_runs_`
    // counter prevents tasks queued on the old pool from being
    // silently dropped on swap).
    void set_worker_count(std::size_t n);

    // Compatibility convenience: set_worker_count(hardware_concurrency()).
    void set_worker_count_auto();

    // Per-node result caching. Disabled by default; opt in per node.
    void set_node_cache_enabled(const std::string& node_name, bool enabled);
    void clear_node_cache();
    const NodeCache& node_cache() const;

    const std::string& get_graph_name() const;
};
```

#### `build` and `link`

```cpp
EngineConfig config;
config.node_context.provider = provider;
config.checkpoint_store = checkpoint_store;
config.store = store;
config.worker_count = 4;
config.cached_nodes.insert("retrieve");

std::vector<std::unique_ptr<Tool>> owned_tools;
owned_tools.push_back(std::make_unique<SearchTool>());
auto registry = std::make_shared<GraphRegistry>();
// Register engine-local reducers, conditions, or node types on registry.

EngineResources resources{
    .tools = ToolSet(std::move(owned_tools)),
    .registry = registry,
};

auto engine = GraphEngine::build(definition, std::move(config),
                                 std::move(resources));
```

`build()` compiles, verifies, links, and returns a fully configured engine.
`link()` consumes a `CompiledGraph` by move and applies runtime configuration;
callers that compile manually remain responsible for any source-to-IR
round-trip verification they require.

#### `compile` (compatibility)

```cpp
static std::unique_ptr<GraphEngine> compile(
    const json& definition,
    const NodeContext& default_context,
    std::shared_ptr<CheckpointStore> store = nullptr);
```

Compiles a graph from a JSON definition and returns an engine ready for execution.
This original signature is preserved and delegates to `build()`. Prefer
`EngineConfig` when new code needs stores, retry policy, worker configuration,
caching, or a tool gate.

| Parameter | Type | Description |
|-----------|------|-------------|
| `definition` | `const json&` | Graph definition in JSON format (see below) |
| `default_context` | `const NodeContext&` | Default context injected into all nodes |
| `store` | `std::shared_ptr<CheckpointStore>` | Optional checkpoint store for persistence |

**Graph definition JSON schema:**

```json
{
  "name": "my_graph",
  "channels": {
    "messages": {"reducer": "append"},
    "status": {"reducer": "overwrite", "initial": "idle"}
  },
  "nodes": {
    "llm": {"type": "llm_call"},
    "tools": {"type": "tool_dispatch"}
  },
  "edges": [
    {"from": "__start__", "to": "llm"},
    {"from": "tools", "to": "llm"}
  ],
  "conditional_edges": [
    {
      "from": "llm",
      "condition": "has_tool_calls",
      "routes": {"yes": "tools", "no": "__end__"}
    }
  ],
  "interrupt_before": [],
  "interrupt_after": ["tools"]
}
```

##### Barrier nodes (AND-join opt-in)

A node declaration may include a `barrier` field to opt into AND-join
semantics for that specific node. Under the default signal-dispatch
model, a node fires every super-step that any upstream routes to it
— which double-fires join nodes on asymmetric serial fan-in (paths
of different lengths). A barrier gates the node until **all** listed
upstreams have signaled at least once (across any number of
super-steps):

```json
"join": {
  "type": "my_join",
  "barrier": {"wait_for": ["a", "s2"]}
}
```

Fires once when both `a` and `s2` have signaled. State resets on
fire, so loops through the barrier collect fresh signals each round.

**Persistence:** since `CHECKPOINT_SCHEMA_VERSION = 2`, the barrier
accumulator is persisted on every checkpoint (`Checkpoint::barrier_state`,
a `map<string, set<string>>`) and restored on resume. Interrupts that
land mid-accumulation are therefore safe — the partial upstream set
survives the pause and the barrier fires as soon as the remaining
signals arrive. v1 blobs deserialize with an empty `barrier_state`,
matching pre-v2 behavior for those stored checkpoints.

#### `run`

```cpp
RunResult run(const RunConfig& config);
```

Executes the graph synchronously (blocking). Starts from `START_NODE`, follows edges
until `END_NODE` is reached or `max_steps` is exceeded.

#### `run_stream`

```cpp
RunResult run_stream(const RunConfig& config,
                     const GraphStreamCallback& cb);
```

Executes the graph with streaming events. The callback `cb` is invoked for each event
matching the `config.stream_mode` filter.

#### `resume`

```cpp
RunResult resume(const std::string& thread_id,
                 const json& resume_value = json(),
                 const GraphStreamCallback& cb = nullptr);
```

Resumes execution from a previously interrupted checkpoint (human-in-the-loop).

| Parameter | Type | Description |
|-----------|------|-------------|
| `thread_id` | `std::string` | Thread ID to resume |
| `resume_value` | `json` | Optional value to inject before resuming (e.g., human approval) |
| `cb` | `GraphStreamCallback` | Optional streaming callback. Pass `nullptr` for non-streaming resume |

#### `get_state`

```cpp
std::optional<json> get_state(const std::string& thread_id) const;
```

Returns the latest state for a thread, or `std::nullopt` if no checkpoint exists.

#### `get_state_history`

```cpp
std::vector<Checkpoint> get_state_history(const std::string& thread_id,
                                          int limit = 100) const;
```

스레드의 체크포인트 이력을 가장 최근에 저장된 것부터 반환합니다. 첫 항목은 `get_state`가 읽는 체크포인트입니다.

#### `update_state`

```cpp
void update_state(const std::string& thread_id,
                  const json& channel_writes,
                  const std::string& as_node = "");

void update_state_writes(const std::string& thread_id,
                         const std::vector<ChannelWrite>& channel_writes,
                         const std::string& as_node = "");
```

채널 write를 적용해 thread 상태를 수동으로 갱신합니다. JSON object 형식은 채널명별
reducer write를 적용합니다. `ChannelWrite` vector 형식은 write 순서와 명시적 overwrite
mode를 보존합니다. 두 형식 모두 갱신된 상태로 새 checkpoint를 생성합니다.

| Parameter | Type | Description |
|-----------|------|-------------|
| `thread_id` | `std::string` | Target thread |
| `channel_writes` | `json` | Object of `{channel: value}` pairs to apply |
| `as_node` | `std::string` | Optional: record these writes as if from a specific node |

#### `fork`

```cpp
std::string fork(const std::string& source_thread_id,
                 const std::string& new_thread_id,
                 const std::string& checkpoint_id = "");
```

Creates a copy of a thread's state as a new thread. Useful for branching conversations
or creating what-if scenarios.

| Parameter | Type | Description |
|-----------|------|-------------|
| `source_thread_id` | `std::string` | Thread to copy from |
| `new_thread_id` | `std::string` | New thread identifier |
| `checkpoint_id` | `std::string` | Optional: fork from a specific checkpoint (default: latest) |

**Returns:** The checkpoint ID of the new forked state.

Fork는 상태와 선택한 checkpoint의 pending continuation을 복사하며 새 turn을 만들지 않는다. 완료된 `__end__` continuation을 resume하면 어떤 node도 실행하지 않으므로 질문만 수정해도 답은 생기지 않는다. pending 작업은 실제 `next_nodes`가 있는 exact paused checkpoint ID를 골라 fork하고 portable state를 수정한 뒤 resume한다. 과거 empty-`next_nodes` latest-resume snapshot 전체를 terminal sentinel로 일반화하지 않는다. example 08은 terminal fork 뒤 별도의 new-turn 흐름을 유지한다.

authentic native state는 원래 shared-bank scope를 유지한다. Fork는 spending grant를 복제하지 않으며 managed-bank custody/source commitment/원래 ceiling/deadline이 적용된다. durable standalone fork로 native 권한을 import하거나 갱신할 수 없다.

Tool ownership is established in `NodeContext::tools` or
`EngineResources::tools` before compilation. There is no post-compile transfer.

#### `set_checkpoint_store`

```cpp
void set_checkpoint_store(std::shared_ptr<CheckpointStore> store);
```

Attaches a checkpoint store. Required for `resume()`, `get_state()`, `fork()`, and
all state inspection methods.

#### `set_store`

```cpp
void set_store(std::shared_ptr<Store> store);
```

Attaches a cross-thread shared memory store (see [Store](#9-store)).

#### `get_store`

```cpp
std::shared_ptr<Store> get_store() const;
```

Returns the attached shared memory store, or `nullptr` if none is set.

#### `set_retry_policy`

```cpp
void set_retry_policy(const RetryPolicy& policy);
```

Sets the default retry policy for all nodes. Nodes without a specific policy
will use this one.

#### `set_node_retry_policy`

```cpp
void set_node_retry_policy(const std::string& node_name, const RetryPolicy& policy);
```

Sets a retry policy for a specific node, overriding the default.

#### `get_graph_name`

```cpp
const std::string& get_graph_name() const;
```

Returns the name of the graph as specified in the definition.

---

## 7b. Engine Internals

`GraphEngine` is a thin orchestrator that delegates to four purpose-built
classes. Users typically never touch them directly — they are instantiated
inside `GraphEngine::build()` (or its `compile()` compatibility facade) and
driven from `execute_graph()` — but
they are public so advanced callers can build without JSON, drive custom
checkpoint flows, or stub pieces in tests.

| Class | Header | Responsibility |
|-------|--------|----------------|
| [`GraphCompiler`](#graphcompiler) | `<neograph/graph/compiler.h>` | Parses JSON → `CompiledGraph` |
| [`Scheduler`](#scheduler) | `<neograph/graph/scheduler.h>` | Routing decisions (signal dispatch + barriers) |
| [`CheckpointCoordinator`](#checkpointcoordinator) | `<neograph/graph/coordinator.h>` | Per-run checkpoint lifecycle |
| [`NodeExecutor`](#nodeexecutor) | `<neograph/graph/executor.h>` | Retry, parallel fan-out, Send dispatch |

### GraphCompiler

**Header:** `<neograph/graph/compiler.h>`

Pure JSON → value-type translation. No provider dispatch during compilation; the installed Core target still requires SchemaProvider runtime — the
resulting `CompiledGraph` is a movable bundle you can inspect or
construct by hand in tests.

```cpp
namespace neograph::graph {

struct ChannelDef {
    std::string  name;
    ReducerType  type = ReducerType::OVERWRITE;
    std::string  reducer_name = "overwrite";
    json         initial_value;
};

struct CompiledGraph {
    std::string name;
    std::vector<ChannelDef> channel_defs;
    std::map<std::string, std::unique_ptr<GraphNode>> nodes;
    std::vector<Edge> edges;
    std::vector<ConditionalEdge> conditional_edges;
    BarrierSpecs barrier_specs;
    std::set<std::string> interrupt_before;
    std::set<std::string> interrupt_after;
    std::optional<RetryPolicy> retry_policy;
};

class GraphCompiler {
public:
    static TopologySpec parse(const json& definition);
    static CompiledGraph link(TopologySpec topology,
                              const NodeContext& default_context);
    static CompiledGraph compile(const json& definition,
                                 const NodeContext& default_context);
};

} // namespace neograph::graph
```

`GraphCompiler::parse()` produces a `TopologySpec` without constructing nodes.
`GraphValidator::validate()` returns structured diagnostics, while
`GraphValidator::require_valid()` returns a `ValidatedTopology` or throws
`std::runtime_error`. Only `GraphCompiler::link()` resolves factories and
instantiates runtime nodes. `compile()` remains the compatibility composition of
parse and link, and `GraphEngine::build()` retains its lenient warning behavior.
New code can enforce the full boundary with `GraphEngine::build_strict()` or:

```cpp
auto spec = GraphCompiler::parse(definition);
auto validated = GraphValidator::require_valid(std::move(spec));
auto engine = GraphEngine::link(std::move(validated), config, resources);
```

### Scheduler

**Header:** `<neograph/graph/scheduler.h>`

Owns the graph topology and computes each super-step's ready set from
routing signals emitted by the previous step. No knowledge of
threading, checkpointing, retries, or HITL — those stay in the engine.

```cpp
namespace neograph::graph {

struct StepRouting {
    std::string node_name;
    std::optional<std::string> command_goto;
};

struct NextStepPlan {
    std::vector<std::string> ready;
    bool hit_end = false;
    std::optional<std::string> winning_command_goto;
};

using BarrierSpecs = std::map<std::string, std::set<std::string>>;
using BarrierState = std::map<std::string, std::set<std::string>>;

class Scheduler {
public:
    Scheduler(const std::vector<Edge>& edges,
              const std::vector<ConditionalEdge>& conditional_edges,
              BarrierSpecs barrier_specs = {});

    std::vector<std::string> plan_start_step() const;

    NextStepPlan plan_next_step(
        const std::vector<std::string>& just_ran,
        const std::vector<NodeResult>& results,
        const GraphState& state,
        BarrierState& barrier_state) const;

    std::vector<std::string> resolve_next_nodes(
        const std::string& current,
        const GraphState& state) const;

    const BarrierSpecs& barrier_specs() const;
};

} // namespace neograph::graph
```

**Semantics:**

- **Signal dispatch**: a node becomes ready in super-step S+1 iff some
  node in step S explicitly routed to it (regular edge, conditional
  edge branch, `Command::goto_node`, or Send). No static predecessor
  map — that would conflate XOR routing with AND fan-in.
- **Pairing invariant**: the caller must pass `just_ran` and `results`
  with `just_ran[i] ↔ results[i]`. Enforced by the two-argument
  overload's type signature so callers cannot desynchronize them.
- **Barriers**: nodes declared with `"barrier": {"wait_for": [...]}`
  gate on ALL listed upstreams having signaled, accumulated across
  super-steps via the mutable `BarrierState` map. Fires reset the
  entry so loops through the barrier work correctly.

### CheckpointCoordinator

**Header:** `<neograph/graph/coordinator.h>`

Per-run wrapper over `(CheckpointStore, thread_id)`. Every method is a
safe no-op when the store is null or thread_id is empty, so call sites
never need to guard.

```cpp
namespace neograph::graph {

struct ResumeContext {
    bool have_cp = false;
    std::string checkpoint_id;
    json channel_values;
    int start_step = 0;  // Phase-adjusted
    CheckpointPhase phase = CheckpointPhase::Completed;
    std::vector<std::string> next_nodes;
    std::unordered_map<std::string, NodeResult> replay_results;
    BarrierState barrier_state;
};

class CheckpointCoordinator {
public:
    CheckpointCoordinator(std::shared_ptr<CheckpointStore> store,
                          std::string thread_id);

    bool enabled() const noexcept;

    std::string save_super_step(
        const GraphState& state,
        const std::string& current_node,
        const std::vector<std::string>& next_nodes,
        CheckpointPhase phase,
        int step,
        const std::string& parent_id,
        const BarrierState& barrier_state) const;

    ResumeContext load_for_resume() const;

    void record_pending_write(
        const std::string& parent_cp_id,
        const std::string& task_id,
        const std::string& task_path,
        const std::string& node_name,
        const NodeResult& nr,
        int step) const;

    void clear_pending_writes(const std::string& parent_cp_id) const;
};

} // namespace neograph::graph
```

**Phase-aware step offset:** `load_for_resume()` reads the latest
checkpoint's `interrupt_phase` and sets `start_step` accordingly —
`Before` / `NodeInterrupt` re-enter at `cp.step`, `After` / `Completed` /
`Updated` advance by +1. The engine's resume path never repeats this
logic.

### NodeExecutor

**Header:** `<neograph/graph/executor.h>`

Owns per-super-step node invocation: retry loop, replay lookup,
pending-write recording, parallel fan-out via
`asio::experimental::make_parallel_group`, and Send dispatch. 3.0
removed the sync `run_one` / `run_parallel` / `run_sends` twins;
callers use the `_async` peers.

```cpp
namespace neograph::graph {

class NodeExecutor {
public:
    using RetryPolicyLookup = std::function<RetryPolicy(const std::string&)>;

    NodeExecutor(
        const std::map<std::string, std::unique_ptr<GraphNode>>& nodes,
        const std::vector<ChannelDef>& channel_defs,
        RetryPolicyLookup retry_policy_for,
        asio::thread_pool* fan_out_pool = nullptr);

    asio::awaitable<NodeResult> run_one_async(
        const std::string& node_name, int step,
        GraphState& state,
        const std::unordered_map<std::string, NodeResult>& replay,
        CheckpointCoordinator& coord,
        const std::string& parent_cp_id,
        const BarrierState& barrier_state,
        std::vector<std::string>& trace,
        const GraphStreamCallback& cb, StreamMode stream_mode);

    asio::awaitable<std::vector<NodeResult>> run_parallel_async(
        const std::vector<std::string>& ready, int step,
        GraphState& state,
        const std::unordered_map<std::string, NodeResult>& replay,
        CheckpointCoordinator& coord,
        const std::string& parent_cp_id,
        const BarrierState& barrier_state,
        std::vector<std::string>& trace,
        const GraphStreamCallback& cb, StreamMode stream_mode);

    asio::awaitable<void> run_sends_async(
        const std::vector<Send>& sends, int step,
        GraphState& state,
        const std::unordered_map<std::string, NodeResult>& replay,
        CheckpointCoordinator& coord,
        const std::string& parent_cp_id,
        std::vector<std::string>& trace,
        const GraphStreamCallback& cb, StreamMode stream_mode);

    asio::awaitable<NodeResult> execute_node_with_retry_async(
        const std::string& node_name,
        GraphState& state,
        const GraphStreamCallback& cb, StreamMode stream_mode);
};

} // namespace neograph::graph
```

**Invariants:**

- `run_one_async` and `run_parallel_async` both save a
  `phase=NodeInterrupt` checkpoint scoped to the interrupting node
  before rethrowing `NodeInterrupt`, so resume re-enters just that
  node (sibling writes are already in `pending_writes` and replay via
  the map).
- `run_parallel_async` applies writes + `Command.updates` in `ready`
  order so `ready[i] ↔ results[i]` pairing holds for the subsequent
  Scheduler call.
- `run_sends_async`: every Send, one or many, gives its target an
  isolated state copy (init + restore + apply input) and runs it with
  retry, so a payload is visible only to its own Send (#389).
- `fan_out_pool` (optional) determines where parallel branches
  dispatch. When null, branches run on `co_await asio::this_coro::
  executor` — fine for single-thread async callers, but CPU-bound
  fan-out serializes. When non-null, `run_parallel_async` and the
  multi-Send branch `co_spawn` onto `pool->get_executor()` for real
  thread parallelism. `GraphEngine::set_worker_count(N)` installs the
  pool for sync `run()` callers.
- `execute_node_with_retry_async` is the inner retry loop: backoff
  uses an `asio::steady_timer` so the executor isn't frozen during
  retry waits.

---

## 8. Checkpoint

**Header:** `<neograph/graph/checkpoint.h>`
**Namespace:** `neograph::graph`

Checkpointing enables persistence, time-travel debugging, and human-in-the-loop
workflows by saving and restoring graph execution state.

### Checkpoint (struct)

A serialized snapshot of graph execution state at a point in time.

```cpp
struct Checkpoint {
    std::string id;                // UUID v4
    std::string thread_id;         // Conversation/session identifier
    json        channel_values;    // Serialized channel data
    json        channel_versions;  // Not set by the engine (see below)
    std::string parent_id;         // Previous checkpoint ID (for time-travel chain)
    std::string current_node;      // Node that was active at checkpoint time
    std::vector<std::string> next_nodes;  // Nodes to execute on resume
    CheckpointPhase interrupt_phase;  // Before | After | Completed | NodeInterrupt | Updated
    std::map<std::string, std::set<std::string>> barrier_state;  // v2+: in-flight barrier accumulators
    json        metadata;          // User-defined metadata
    int64_t     step;              // Super-step number
    int64_t     timestamp;         // Unix epoch milliseconds
    std::uint32_t schema_version = CHECKPOINT_SCHEMA_VERSION;  // Layout version

    static std::string generate_id();  // Generate UUID v4
};

// Wire-stable schema version. Bump on layout-incompatible changes.
// v2 added `barrier_state`; v3 records pending-write mode support.
// Typed `uint32_t`: schema versions are non-negative wire values.
constexpr std::uint32_t CHECKPOINT_SCHEMA_VERSION = 3;
```

| Field | Type | Description |
|-------|------|-------------|
| `id` | `std::string` | Unique identifier (UUID v4) |
| `thread_id` | `std::string` | Groups checkpoints by conversation/session |
| `channel_values` | `json` | Serialized state of all channels |
| `channel_versions` | `json` | 엔진은 이 필드를 채우지 않는다. 엔진의 체크포인트는 채널별 버전을 `channel_values["channels"][name]["version"]`에, 공유 쓰기 카운터를 `channel_values["global_version"]`에 담는다 |
| `parent_id` | `std::string` | ID of the preceding checkpoint (forms a linked list for time-travel) |
| `current_node` | `std::string` | Node that was executing when the checkpoint was taken |
| `next_nodes` | `std::vector<std::string>` | All nodes scheduled for the next super-step (used by `resume()`). Under signal dispatch a super-step can leave several nodes simultaneously ready (parallel fan-out, conditional branches activating together), and every one of them must be persisted — storing a single node would silently drop siblings across a crash |
| `interrupt_phase` | `CheckpointPhase` | Enum: `Before` (interrupt_before fired), `After` (interrupt_after fired), `Completed` (normal super-step cadence), `NodeInterrupt` (node threw `NodeInterrupt` mid-execution), `Updated` (external `update_state()` injection). `to_string()` and `parse_checkpoint_phase()` give a stable wire/log encoding |
| `barrier_state` | `map<string, set<string>>` | Per-barrier accumulator of upstreams that have signaled so far. Entries only exist for barriers that are in-flight (not yet fired) — the Scheduler clears an entry when its barrier fires. Shape matches `BarrierState` from `scheduler.h`. Present since schema v2; v1 blobs deserialize with an empty map, which matches their pre-v2 behavior |
| `metadata` | `json` | Arbitrary user-defined data |
| `step` | `int64_t` | Super-step counter |
| `timestamp` | `int64_t` | Creation time in Unix epoch milliseconds |
| `schema_version` | `std::uint32_t` | On-wire layout version (see `CHECKPOINT_SCHEMA_VERSION`, currently `3`). It is a fixed-width unsigned type because a schema version is non-negative and is persisted to disk and round-tripped through JSON. Persistent `CheckpointStore` implementations should serialize it and treat `0` on a deserialized blob as "pre-versioned" (e.g. the field was absent — migration is the caller's responsibility) |

### CheckpointStore

Abstract interface for checkpoint persistence. Implement this to store checkpoints
in a database, file system, or any other backend.

> **Writing a custom store?** New implementations should implement the smallest
> applicable capability: `CheckpointStoreCore`, optionally
> `AsyncCheckpointStore` and/or `PendingWritesCheckpointStore`, then pass it
> through `adapt_checkpoint_store()`. The existing `CheckpointStore` interface
> remains the compatibility contract. Its async defaults invoke the sync methods;
> Sync-only checkpoint backend는 유한 worker pool로 offload한다. Native async backend는 coroutine capability를 사용하고 누락 sync operation은 재귀 대신 예외를 던진다.
> See [`ASYNC_GUIDE.md` §9.4](ASYNC_GUIDE.md#94-checkpointstore).

Python `CheckpointStore.requires_managed_budget(thread_id) -> bool`은 영속화된
managed-bank 거부 의무를 읽는 동기 가상 메서드다. 엔진의 네이티브
`requires_managed_budget_async(thread_id)` facade는 동기 호출을 별도 worker에
넘기고, 바인딩은 GIL을 획득해 Python override를 호출한다. Override가 없으면
네이티브 구현은 `False`를 반환하지 않고 지원하지 않는 backend라는 명시적
오류를 발생시킨다. Reader는 신뢰할 수 있는 namespace/thread가 활성 standalone
managed bank를 보유한 적이 있는지 보고한다. 상태에서 bank를 제거해 저장하거나
checkpoint를 삭제해도 이 의무를 지워서는 안 된다. 구현은 이를 사실대로 보고해야
한다. 이 reader는 지출, 복원, bank 또는 lease 권한을 부여하지 않는다. 유한 예산 실행에는
지원되는 실제 네이티브 managed-budget lease가 여전히 필요하다.

```cpp
class CheckpointStore {
public:
    virtual ~CheckpointStore() = default;

    // ── Sync facade (5 virtuals; missing operation throws) ──────
    virtual void save(const Checkpoint& cp);
    virtual std::optional<Checkpoint> load_latest(const std::string& thread_id);
    virtual std::optional<Checkpoint> load_by_id(const std::string& id);
    virtual std::vector<Checkpoint>   list(const std::string& thread_id,
                                           int limit = 100);
    virtual void delete_thread(const std::string& thread_id);

    // ── Async peers (5 virtuals; sync-only operations offload) ──
    virtual asio::awaitable<void> save_async(const Checkpoint& cp);
    virtual asio::awaitable<std::optional<Checkpoint>>
        load_latest_async(const std::string& thread_id);
    virtual asio::awaitable<std::optional<Checkpoint>>
        load_by_id_async(const std::string& id);
    virtual asio::awaitable<std::vector<Checkpoint>>
        list_async(const std::string& thread_id, int limit = 100);
    virtual asio::awaitable<void>
        delete_thread_async(const std::string& thread_id);

    // ── Pending writes — fine-grained super-step progress log ──────
    //
    // Default no-ops: backends that don't support per-node durable
    // writes fall back to "full super-step replay" on resume.
    virtual void put_writes(const std::string& thread_id,
                            const std::string& parent_checkpoint_id,
                            const PendingWrite& write) {}
    virtual std::vector<PendingWrite> get_writes(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id) { return {}; }
    virtual void clear_writes(const std::string& thread_id,
                              const std::string& parent_checkpoint_id) {}

    // ── Async pending-writes peers (default-bridge to sync) ────────
    virtual asio::awaitable<void> put_writes_async(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id,
        const PendingWrite& write);
    virtual asio::awaitable<std::vector<PendingWrite>> get_writes_async(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id);
    virtual asio::awaitable<void> clear_writes_async(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id);
};
```

| Method | Description |
|--------|-------------|
| `save(cp)` / `save_async(cp)` | Persist a checkpoint. Engine writes one per super-step. |
| `load_latest(thread_id)` / `_async` | Load the most recent checkpoint for a thread. |
| `load_by_id(id)` / `_async` | Load a specific checkpoint by UUID (time-travel). |
| `list(thread_id, limit)` / `_async` | List checkpoints for a thread, newest first, up to `limit`. |
| `delete_thread(thread_id)` / `_async` | Delete all checkpoints for a thread. |
| `put_writes(thread_id, parent_cp, write)` / `_async` | Record a successful node execution mid-super-step. Engine calls this immediately after a node returns and *before* its writes apply to GraphState. Default no-op. |
| `get_writes(thread_id, parent_cp)` / `_async` | Load pending writes attached to a parent checkpoint. Engine calls this on resume to skip already-completed tasks. Default empty. |
| `clear_writes(thread_id, parent_cp)` / `_async` | Discard pending writes after the successor super-step's checkpoint has been durably saved. Default no-op. |

### InMemoryCheckpointStore

Thread-safe in-memory implementation suitable for testing and single-process applications.

```cpp
class InMemoryCheckpointStore : public CheckpointStore {
public:
    void save(const Checkpoint& cp) override;
    std::optional<Checkpoint> load_latest(const std::string& thread_id) override;
    std::optional<Checkpoint> load_by_id(const std::string& id) override;
    std::vector<Checkpoint> list(const std::string& thread_id,
                                  int limit = 100) override;
    void delete_thread(const std::string& thread_id) override;

    size_t size() const;  // Total number of stored checkpoints
};
```

---

## 9. Store

**Header:** `<neograph/graph/store.h>`
**Namespace:** `neograph::graph`

Cross-thread shared memory store. Provides namespaced key-value storage that persists
across threads and graph executions. Use cases include long-term user preferences,
shared knowledge bases, and agent memory.

### Namespace

A hierarchical path represented as a vector of strings.

```cpp
using Namespace = std::vector<std::string>;
```

Example: `{"users", "user123", "preferences"}` represents the path `users/user123/preferences`.

### StoreItem

A single item in the store.

```cpp
struct StoreItem {
    Namespace   ns;          // Namespace path
    std::string key;         // Item key within the namespace
    json        value;       // Stored value
    int64_t     created_at;  // Creation timestamp (Unix epoch millis)
    int64_t     updated_at;  // Last update timestamp (Unix epoch millis)
};
```

### Store (abstract)

Abstract interface for cross-thread shared memory.

```cpp
class Store {
public:
    virtual ~Store() = default;

    // Put a value (create or update)
    virtual void put(const Namespace& ns, const std::string& key,
                     const json& value) = 0;

    // Get a single item
    virtual std::optional<StoreItem> get(const Namespace& ns,
                                         const std::string& key) const = 0;

    // Search items under a namespace prefix
    virtual std::vector<StoreItem> search(const Namespace& ns_prefix,
                                           int limit = 100) const = 0;

    // Delete an item
    virtual void delete_item(const Namespace& ns, const std::string& key) = 0;

    // List namespaces under a prefix
    virtual std::vector<Namespace> list_namespaces(
        const Namespace& prefix = {}) const = 0;
};
```

| Method | Description |
|--------|-------------|
| `put(ns, key, value)` | Insert or update a value. Updates `updated_at` if the item already exists |
| `get(ns, key)` | Retrieve a single item. Returns `std::nullopt` if not found |
| `search(ns_prefix, limit)` | Find all items whose namespace starts with the given prefix |
| `delete_item(ns, key)` | Remove an item from the store |
| `list_namespaces(prefix)` | List all unique namespaces that start with the given prefix |

### InMemoryStore

Thread-safe in-memory implementation for testing and single-process use.

```cpp
class InMemoryStore : public Store {
public:
    void put(const Namespace& ns, const std::string& key,
             const json& value) override;
    std::optional<StoreItem> get(const Namespace& ns,
                                 const std::string& key) const override;
    std::vector<StoreItem> search(const Namespace& ns_prefix,
                                   int limit = 100) const override;
    void delete_item(const Namespace& ns, const std::string& key) override;
    std::vector<Namespace> list_namespaces(
        const Namespace& prefix = {}) const override;

    size_t size() const;  // Total number of stored items
};
```

---

## 10. Loader

**Header:** `<neograph/graph/loader.h>`
**Namespace:** `neograph::graph`

Legacy singleton registries for reducers, conditions, and node types. These
remain the process-global fallback for JSON-driven graph construction. New
code can pass a `GraphRegistry` through `EngineResources`; its local entries
take precedence while missing names continue to resolve here.

### ReducerRegistry

Singleton registry mapping string names to `ReducerFn` implementations.

```cpp
class ReducerRegistry {
public:
    static ReducerRegistry& instance();

    void register_reducer(const std::string& name, ReducerFn fn);
    ReducerFn get(const std::string& name) const;
    std::vector<std::string> names() const;
};
```

| Method | Description |
|--------|-------------|
| `instance()` | Returns the singleton instance |
| `register_reducer(name, fn)` | Registers a custom reducer function |
| `get(name)` | Looks up a reducer by name. Throws if not found |
| `names()` | Sorted list of all registered reducer names (introspection for external tooling) |

### ConditionRegistry

Singleton registry mapping string names to `ConditionFn` implementations.

```cpp
class ConditionRegistry {
public:
    static ConditionRegistry& instance();

    void register_condition(const std::string& name, ConditionFn fn);
    ConditionFn get(const std::string& name) const;
    std::vector<std::string> names() const;
};
```

| Method | Description |
|--------|-------------|
| `instance()` | Returns the singleton instance |
| `register_condition(name, fn)` | Registers a custom condition function |
| `get(name)` | Looks up a condition by name. Throws if not found |
| `names()` | Sorted list of all registered condition names (introspection for external tooling) |

### NodeFactory

Singleton factory for creating `GraphNode` instances from JSON configuration.

```cpp
using NodeFactoryFn = std::function<std::unique_ptr<GraphNode>(
    const std::string& name,
    const json& config,
    const NodeContext& ctx)>;

class NodeFactory {
public:
    static NodeFactory& instance();

    void register_type(const std::string& type, NodeFactoryFn fn);
    void register_type(const std::string& type, NodeFactoryFn fn,
                       json config_schema);
    std::unique_ptr<GraphNode> create(const std::string& type,
                                       const std::string& name,
                                       const json& config,
                                       const NodeContext& ctx) const;
    std::vector<std::string> registered_types() const;
    json export_schema() const;
};
```

| Method | Description |
|--------|-------------|
| `instance()` | Returns the singleton instance |
| `register_type(type, fn)` | Registers a node factory. Config schema defaults to a permissive `{"type":"object"}` |
| `register_type(type, fn, config_schema)` | As above, with a declared JSON Schema (Draft 2020-12) for the node's `config`. Additive — the 2-arg overload still works unchanged. Used only by `export_schema()`; the engine does not validate config against it |
| `create(type, name, config, ctx)` | Creates a node of the given type. Throws if the type is not registered |
| `registered_types()` | Sorted list of all registered node type names |
| `export_schema()` | Machine-readable description of the topology JSON this engine accepts (see [Topology Schema Export](#topology-schema-export-issue-56)) |

### Built-in Registrations

The library pre-registers the following components:

**Reducers:**

| Name | Behavior |
|------|----------|
| `"overwrite"` | Replaces the current value with the incoming value |
| `"append"` | Appends the incoming value to the current array. If the incoming value is an array, its elements are concatenated |

**Conditions:**

| Name | Behavior |
|------|----------|
| `"has_tool_calls"` | Inspects the last message in the `"messages"` channel. Returns `"yes"` if it contains tool calls, `"no"` otherwise |
| `"route_channel"` | Reads the `"__route__"` channel and returns its string value. Used with `IntentClassifierNode` |

**Node types:**

| 타입 | 클래스 | 설명 |
|------|-------|-------------|
| `"llm_call"` | `LLMCallNode` | 현재 대화 상태로 LLM을 호출한다 |
| `"tool_dispatch"` | `ToolDispatchNode` | 최신 assistant 메시지의 도구 호출을 실행한다 |
| `"intent_classifier"` | `IntentClassifierNode` | LLM 기반 의도 분류. `config`에서 `prompt`와 `valid_routes`를 읽는다 |
| `"subgraph"` | `SubgraphNode` | 컴파일된 서브그래프를 실행한다. `config`에서 `input_map`과 `output_map`을 읽는다 |
| `"image_generate"`, `"image_edit"` | 엔드포인트 노드 | 이미지를 생성하거나 편집한다 ("엔드포인트 노드" 참고) |
| `"embed"`, `"rerank"`, `"moderate"`, `"decide"` | 엔드포인트 노드 | 텍스트를 임베딩하거나, 문서의 순위를 매기거나, 텍스트를 검사하거나, 구조화된 질문에 답한다 |
| `"speak"`, `"transcribe"`, `"translate"` | 엔드포인트 노드 | 텍스트를 음성으로 합성하거나, 음성을 전사하거나 번역한다 |
| `"job_submit"`, `"job_collect"` | 엔드포인트 노드 | 비디오 job을 제출한 다음 수집한다 |

#### 엔드포인트 노드

엔드포인트 노드(#531)는 노드 컨텍스트의 provider에서 SchemaProvider 엔드포인트를 각각 한 번 호출한다. 노드는 `make_endpoint_request`를 `invoke_provider`로 보내며, 이는 채팅 호출이 거치는 경로다. 실행의 provider 호출 broker(Program에서는 저널)와 토큰 예산이 있으면 적용되고, 없으면 provider가 호출을 직접 받는다.
- **설정.** `model`은 필수다. 노드는 엔드포인트의 옵션을 SDK 요청의 멤버 이름(`size`, `quality`, `dimensions`, `voice`, `top`, ...)으로 받고, provider 정책이 승인하는 멤버는 `extensions`로 받는다. 읽고 쓰는 채널의 이름도 받으며, `output`은 노드가 쓰는 채널의 이름이다. 엄격한 그래프(`schema_version` 1)는 모르는 키를 거부한다. 채널이 설정이므로, 엔드포인트 노드가 있는 그래프에서는 검증기의 효과 분석이 실행되지 않는다.
- **텍스트.** `embed`와 `moderate`는 문자열 또는 문자열 배열(채널 `input`)을 읽고, `speak`는 문자열(`text`)을 읽으며, `rerank`는 질의 문자열(`query`)과 문서 배열(`documents`)을 읽는다.
- **미디어.** 미디어 채널에는 base64 data URL, https URL 또는 provider outcome format v3의 미디어 JSON이 들어간다. 노드는 미디어를 v3 형식으로 쓴다. `image_generate`와 `image_edit`는 `images`에, `speak`는 `audio`에, `job_collect`는 `video`에 쓴다. `image_edit`는 편집할 이미지를 `references`에서 읽고, `transcribe`와 `translate`는 `audio`를 읽는다.
- **그 밖의 결과.**
  - `embed`는 입력마다 숫자 배열 하나를 쓴다(`embeddings`).
  - `rerank`는 `[{index, relevance}]`를 관련도가 높은 순으로 쓴다(`ranking`).
  - `moderate`는 `[{flagged, categories, scores}]`를 쓴다(`moderation`).
  - `transcribe`와 `translate`는 텍스트를 쓴다(`transcript`, `translation`).
- **`decide`.** 임의의 JSON을 증거(`input`)로 읽고 `questions`를 받는다. 각 질문은 `{name, kind, instructions, options, when_true, when_false}`이며, `kind`는 `predicate`, `choice`, `score`, `box` 중 하나다. 답은 `decisions`에 쓴다.
- **비디오 job.**
  - `job_submit`은 프롬프트(`prompt`)를 읽고 job의 핸들(`job`)을 쓴다.
  - `job_collect`는 실행될 때마다 핸들을 한 번 폴링한다. 완료된 job의 출력은 `video`로 다운로드되고, 실패했거나 취소되었거나 만료된 job은 노드를 실패시킨다.
  - 아직 실행 중인 job이면 `{"job", "status", "raw_status"}`를 담은 `NodeInterrupt`를 던진다. 호스트는 원하는 때에 실행을 재개하고, 노드는 다시 폴링한다. step 안에서는 아무것도 폴링하지 않으며, 제출은 다시 보내지 않는다.
  - Program에서도 실행은 같은 방식으로 멈춘다. 그 저널은 제출을 보관하고 폴링과 다운로드는 매번 다시 보내므로, 재개된 실행은 job의 지금 상태를 본다.
- **사용량, 예산, 취소.**
  - 호출이 보고한 토큰 사용량은 실행의 사용량에 더해진다. 사용량을 보고하지 않는 호출(moderation, 다운로드)은 아무것도 더하지 않으므로, 실행의 다른 호출이 남긴 합계를 지우지 않는다.
  - 호출은 채팅 호출이 거치는 경로를 따른다(`make_endpoint_request`). 실행의 provider 호출 broker가 이를 저널에 남기고 재생하며, Program에서는 호스트가 `add_core_endpoint_node`로 노드를 등록한다.
  - 모델 토큰 예산이나 managed budget lease는 엔드포인트 호출에 출력 상한이 없으므로 호출을 보내기 전에 거부한다. 가격을 매기는 Program binding은 `P_MONEY_UNMODELED`로 거부한다.
  - Strict Runtime 인터포지션은 채팅 호출만 대상으로 하므로, 엔드포인트 노드가 우회되도록 두지 않고 거부한다.
  - 실행의 취소 토큰과 deadline은 호출에 전달된다.

### Topology Schema Export (issue #56)

NeoGraph runs a graph that is *described in JSON*; swap the JSON and the
same engine becomes a different harness. `NodeFactory::export_schema()`
emits a machine-readable description of exactly what topology JSON this
engine version accepts, so external tooling — notably a code-free
visual block editor (NeoGraph Studio, a private companion repo,
issue #56) — can generate its palette from the engine and never drift
out of sync.

**Three access paths, one document:**

| From | How |
|------|-----|
| C++ | `neograph::graph::NodeFactory::instance().export_schema()` → `json` |
| CLI | `./example_export_schema > schema.json` (`examples/52_export_schema.cpp`) |
| Python | `neograph_engine.export_schema()` → `dict` |

**Document shape:**

```jsonc
{
  "neograph_version": "0.9.0",
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "topology":   { /* JSON Schema for the top-level envelope:
                     name, channels, nodes (type + config + barrier),
                     edges, conditional_edges, interrupt_before,
                     interrupt_after, retry_policy */ },
  "node_types": { "<type>": { /* config JSON Schema */ }, ... },
  "reducers":   ["append", "overwrite", ...],
  "conditions": ["has_tool_calls", "route_channel", ...]
}
```

- **`neograph_version`** is stamped at compile time from
  `pyproject.toml` (single source of truth). A tool compares it to its
  cached schema and warns when its palette is older than the engine.
- **`node_types`** reflects whatever is registered in `NodeFactory` at
  call time, so an embedder's custom node types appear too — register
  them (and any custom reducers/conditions) *before* exporting, exactly
  as you would before `compile()`. A type registered via the 3-arg
  `register_type` carries its declared config schema; the 2-arg form
  yields a permissive `{"type":"object"}`.
- **Round-trip contract.** A tool that emits topology JSON should
  round-trip it through the loader and assert structure is preserved.
  In particular the top-level `conditional_edges` block was silently
  dropped by the compiler in v0.1.0–v0.1.7 (fixed v0.1.8); the engine
  test suite (`tests/test_schema_export.cpp`) guards this regression,
  and tooling should too.

```cpp
#include <neograph/graph/loader.h>
// register custom node types first if you want them in the palette …
auto schema = neograph::graph::NodeFactory::instance().export_schema();
std::cout << schema.dump(2) << "\n";
```

---

## 10.5. 관측 — OpenTelemetry + OpenInference

Python 그래프 tracing은 `neograph_engine.tracing`, `neograph_engine.openinference`에 있다. `otel_tracer`는 graph event로 run/node span을 만들고 `openinference_tracer`는 `CHAIN` 태그와 node payload projection을 기록한다. `neograph_engine.openinference.OpenInferenceProvider`는 기존 typed provider를 native C++ dispatch observer로 감싸고 Python OpenTelemetry tracer에 호출별 `LLM` span을 보낸다. C++에서는 `<neograph/observability/openinference.h>`를 쓴다.

### `otel_tracer` — OTel 형태 span

아래 signature는 참조 선언이다. 기본값은 `root_name=graph.run`, `node_span_prefix=node.`, `attribute_prefix=neograph`이며 `on_event`는 선택적으로 graph event를 다른 소비자에게 전달한다.

```python
@contextmanager
def otel_tracer(
    tracer: Any,
    *,
    root_name: str = "graph.run",
    node_span_prefix: str = "node.",
    attribute_prefix: str = "neograph",
    on_event: Optional[Callable[[Any], None]] = None,
) -> Iterator[Callable[[Any], None]]:
    ...
```

`NODE_START`는 span을 열고 `NODE_END`는 성공으로 닫으며 `ERROR`는 오류를 기록하고 `INTERRUPT`는 pause를 표시한다. Node name마다 중첩 event용 stack이 있으며 run 종료 시 context manager가 남은 span을 닫는다. Trace만으로 exactly-once node 실행이나 모든 동시 task의 고유 correlation을 입증하지 않는다.

```python
from opentelemetry import trace
from neograph_engine.tracing import otel_tracer

tracer = trace.get_tracer("my-service")
with otel_tracer(tracer) as cb:
    engine.run_stream(cfg, cb)
```

### `openinference_tracer` — LLM 형태 속성

Graph span은 `openinference.span.kind = "CHAIN"`이고 node input/output payload는 JSON `input.value` / `output.value` projection이 된다. Python graph tracing만으로 provider별 `LLM` span이나 vendor charge를 만들지 않는다. Context attachment는 원래 Python context에 한정되며 thread/task 사이 parent 전달에는 tracing integration의 context 보존이 필요하다.

```python
@contextmanager
def openinference_tracer(
    tracer: Any,
    *,
    root_name: str = "graph.run",
    node_span_prefix: str = "node.",
    on_event: Optional[Callable[[Any], None]] = None,
) -> Iterator[Callable[[Any], None]]:
    ...
```

### `OpenInferenceProvider` — Python과 C++ typed dispatch 관측자

Python에서는 `OpenInferenceProvider(inner, tracer, *, span_name="llm.complete")`로 생성한다. `Provider`의 `prepare(request)`, 일회성 `dispatch(prepared)`, `invoke(request)`를 상속하며 completion API는 추가하지 않는다. Native wrapper는 준비를 정확히 한 번 위임하고 dispatch 시 같은 owned handle을 관측한다. 유효하지 않거나 버린 준비는 span을 열지 않는다. 정상 tracing에서는 승인된 dispatch마다 LLM span 하나를 열고 원래 outcome, mode, deadline, 취소, event, provider identity를 그대로 전달한다. Tracing 실패는 provider outcome이나 예외를 대체하지 않는다.

다음 함수는 서로 다른 호출 경로를 보여 준다. `inner`는 설정된 `SchemaProvider` 같은 기존 typed provider이며 `model`은 명시한다. 모델 호출 한 번에는 함수 하나를 선택한다.

```python
from neograph_engine import ProviderMessage, ProviderRole, Text, make_provider_request
from neograph_engine.openinference import OpenInferenceProvider


def traced_dispatch(inner, model, tracer):
    observed = OpenInferenceProvider(inner, tracer)
    request = make_provider_request(observed, model, [
        ProviderMessage(ProviderRole.User, [Text("Say hello.")])])
    prepared = observed.prepare(request)
    return observed.dispatch(prepared)


def traced_invoke(inner, model, tracer):
    observed = OpenInferenceProvider(inner, tracer)
    request = make_provider_request(observed, model, [
        ProviderMessage(ProviderRole.User, [Text("Say hello.")])])
    return observed.invoke(request)
```

Python `invoke`/`dispatch`는 GIL을 해제하고 tracer adapter는 Python 호출과 참조 파괴 때 GIL을 다시 획득한다. Prepared operation이 adapter와 tracer를 보유하므로 dispatch 전에 wrapper가 수거되어도 유지된다. Parent는 prepare가 아닌 dispatch 시 활성 OpenTelemetry context를 따른다. Thread/task 사이 작업을 옮길 때 context를 전달한다. Wrapper 생성 전에 `opentelemetry-api`를 설치한다.

C++ session overload는 session teardown에도 parent를 안전하게 연결하며 raw parent lookup은 호출자가 parent 수명을 유지해야 한다. Host 소유 tracer는 모든 operation보다 오래 살아야 한다.

```cpp
#include <neograph/observability/openinference.h>

// tracer is a host-owned neograph::observability::Tracer adapter.
// Its lifetime must cover the session and every provider operation.
auto session = neograph::observability::openinference_tracer(tracer);
auto observed = std::make_shared<neograph::observability::OpenInferenceProvider>(
    inner_provider, tracer, session);
// Use observed in NodeContext before compiling the graph.
```

Native LLM 속성에는 공개 role/text projection, 선언된 scalar와 알려진 count만 넣는다. Native replay block, reasoning, raw wire envelope/event와 `PreparedProviderRequest.encoded_body`는 trace payload에서 제외하며 실제 custody는 request와 outcome에 남는다. 알려진 0은 기록하고 미상은 생략한다. Signed span 범위를 넘는 count는 decimal string으로 기록한다. 공개 text delta는 Python OTel 속성 `{"chunk": text}`를 가진 `llm.token` event를 만든다. Completion은 OK, failure는 안전한 provider message와 ERROR를 기록한다. Dispatch 예외의 원래 product error는 유지하고 span에 오류를 기록한다. 공개 prompt, output, 예외 메시지에도 application secret이 있을 수 있으므로 exporter에 전달할 데이터를 관리한다.

| 속성 | Native 근거 |
|---|---|
| `openinference.span.kind` | `"LLM"` |
| `llm.model_name` | 승인된 prepared model |
| `llm.invocation_parameters` | 존재하면 선언된 temperature와 output cap |
| `llm.input_messages.{i}.message.role` | 공개 role projection |
| `llm.input_messages.{i}.message.content` | 공개 text part |
| `input.value` / `input.mime_type` | 공개 message JSON / `application/json` |
| `llm.output_messages.{i}.message.role` | 반환된 모든 message의 role |
| `llm.output_messages.{i}.message.content` | 공개 text part |
| `output.value` / `output.mime_type` | 연결한 공개 text / `text/plain` |
| `llm.token_count.prompt` | 존재하는 `usage.input_total.value` |
| `llm.token_count.completion` | 존재하는 `usage.output_total.value` |
| `llm.token_count.total` | 존재하는 `usage.total.value` |

Token 속성은 실패 시 사용 가능한 partial usage를 포함해 provider usage를 보고한다. Vendor charge를 입증하거나 budget authority를 복원하지 않는다. Charged/reserved accounting에는 `UsageAccumulator.authority_snapshot()` / Program의 `provider_budget_authority`를 쓰고 nullable usage report와 구분한다.

### End-to-end: NeoGraph + Phoenix 한 블록

Phoenix를 실행한 뒤 graph specification, 기존 typed provider, 명시적 model과 `RunConfig`를 `trace_graph`에 전달한다. Helper는 graph compile 전에 wrapper를 설치하고 graph/node `CHAIN` span과 provider `LLM` span에 같은 tracer를 쓴다. 실제 실행한 provider 호출만 LLM span을 만들며 trace count는 charged accounting이 아닌 usage report다.

```bash
docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix:latest
pip install neograph-engine opentelemetry-api opentelemetry-sdk opentelemetry-exporter-otlp
```

```python
from opentelemetry import context as otel_context
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import BatchSpanProcessor
from opentelemetry.exporter.otlp.proto.grpc.trace_exporter import OTLPSpanExporter
from neograph_engine import GraphEngine, NodeContext
from neograph_engine.openinference import OpenInferenceProvider, openinference_tracer


class ParentContextTracer:
    def __init__(self, tracer, parent_context):
        self.tracer, self.parent_context = tracer, parent_context

    def start_span(self, name):
        return self.tracer.start_span(name, context=self.parent_context)


def trace_graph(graph_spec, inner_provider, model, cfg):
    provider = TracerProvider()
    provider.add_span_processor(BatchSpanProcessor(
        OTLPSpanExporter(endpoint="http://localhost:4317", insecure=True)))
    tracer = provider.get_tracer("my-app")
    try:
        with openinference_tracer(tracer) as cb:
            parent = ParentContextTracer(tracer, otel_context.get_current())
            observed = OpenInferenceProvider(inner_provider, parent)
            engine = GraphEngine.compile(
                graph_spec, NodeContext(provider=observed, model=model))
            return engine.run_stream(cfg, cb)
    finally:
        provider.shutdown()
```

`ParentContextTracer`는 호출자의 run context를 graph worker의 provider dispatch에 명시적으로 전달한다. LLM span을 run root 아래 연결하며 모든 동시 task의 node별 ancestry를 보장하지 않는다. [OpenInference convention](https://github.com/Arize-ai/openinference/blob/main/spec/semantic_conventions.md)은 `CHAIN`과 `LLM`을 정의하고 NeoGraph는 위 subset을 내보낸다. 공개 text와 graph payload를 선택하거나 redact할 때 [OpenTelemetry 민감 데이터 지침](https://opentelemetry.io/docs/security/handling-sensitive-data/)을 따른다.

### 참고

OpenTelemetry는 opt-in이다. Base wheel에는 필요하지 않으며 API/SDK/exporter를 별도로 설치한다. 같은 run에 graph callback으로 `otel_tracer` 또는 `openinference_tracer` 하나만 쓴다. OTLP endpoint와 credential은 backend 설정과 맞아야 하며 URL만 바꾸면 모든 backend와 호환된다는 보장은 없다.

---

## 11. React Graph

**Header:** `<neograph/graph/react_graph.h>`
**Namespace:** `neograph::graph`

Convenience function that creates a standard ReAct (Reason + Act) agent as a two-node
graph: `llm_call -> tool_dispatch -> (loop back if tool calls, else end)`.

```cpp
std::unique_ptr<GraphEngine> create_react_graph(
    std::shared_ptr<Provider> provider,
    std::vector<std::unique_ptr<Tool>> tools,
    const std::string& instructions = "",
    const std::string& model = "");
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `provider` | `std::shared_ptr<Provider>` | LLM provider |
| `tools` | `std::vector<std::unique_ptr<Tool>>` | Tools available to the agent (ownership transferred) |
| `instructions` | `std::string` | System prompt / instructions |
| `model` | `std::string` | 명시적 모델 이름; typed provider는 기본 모델을 선택하지 않는다 |

**Returns:** A compiled `GraphEngine` ready to run.

This is functionally equivalent to using `Agent::run()` but as a graph engine, giving
you access to checkpointing, streaming events, state inspection, and all other graph
engine features.

---

## 11b. Plan-and-Execute Graph

**Header:** `<neograph/graph/plan_execute_graph.h>`
**Namespace:** `neograph::graph`

Convenience factory for the Plan-and-Execute pattern: a planner emits a JSON
array of steps, an executor consumes them one-by-one via an inner ReAct loop,
and a responder composes the final answer from `past_steps`.

```
__start__ → planner → [plan_empty? responder : executor]
                      executor → [plan_empty? responder : executor]
                      responder → __end__
```

```cpp
std::unique_ptr<GraphEngine> create_plan_execute_graph(
    std::shared_ptr<Provider> provider,
    std::vector<std::unique_ptr<Tool>> tools,
    const std::string& planner_prompt,
    const std::string& executor_prompt,
    const std::string& responder_prompt,
    const std::string& model = "",
    int max_step_iterations = 5);
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `provider` | `std::shared_ptr<Provider>` | LLM provider shared by every phase |
| `tools` | `std::vector<std::unique_ptr<Tool>>` | Tools the executor may invoke (ownership transferred) |
| `planner_prompt` | `std::string` | System prompt for the planner; must instruct the model to reply with a JSON array of steps (fenced ```json blocks and leading prose are tolerated) |
| `executor_prompt` | `std::string` | System prompt for the single-step executor (inner ReAct loop) |
| `responder_prompt` | `std::string` | System prompt for the final synthesis phase |
| `model` | `std::string` | 명시적 모델 이름; typed provider는 기본 모델을 선택하지 않는다 |
| `max_step_iterations` | `int` | Upper bound on tool-call iterations inside the executor per step |

**Channels populated:** `plan`, `past_steps`, `final_response`, `messages`.

**Returns:** A compiled `GraphEngine` ready to run. The factory registers its
three custom node types and the `plan_empty` condition on first call
(idempotent via `std::call_once`).

See `examples/14_plan_executor.cpp` for a Send-fan-out variant with crash /
resume via pending-writes.

---

## 12. LLM Module

### SchemaProvider

`SchemaProvider`는 승인된 `sp::descriptor::ValidatedDescriptor`, `sp::runtime::Options`, 선택적 `SchemaProvider::Defaults`를 받는다. descriptor는 closed/versioned 데이터 admission이지 요청/응답 interpreter나 임의 primitive registry가 아니다. credential은 공개 descriptor가 아니라 runtime options에 둔다. Defaults는 typed OpenRouter routing과 Responses 보관(`responses_store`)만 포함하고 후자는 Responses에만 유효하다. Hosted OpenRouter routing·retention·JSON 형식은 선언된 typed 제어다. Images, Veo, Decisions는 별도 NeoGraph typed client와 별도 승인을 쓰며 SDK chat grant를 물려받지 않는다.

```cpp
#include <neograph/llm/schema_provider.h>
#include <descriptor/descriptor.h>
#include <stdexcept>
#include <variant>

std::shared_ptr<neograph::llm::SchemaProvider> admitted_provider(
    std::string_view descriptor_json, std::string api_key) {
    auto loaded = sp::descriptor::load(descriptor_json);
    if (const auto* error = std::get_if<sp::descriptor::ConfigError>(&loaded))
        throw std::invalid_argument(error->message);
    sp::runtime::Options options;
    options.api_key = std::move(api_key);
    neograph::llm::SchemaProvider::Defaults defaults;
    return std::make_shared<neograph::llm::SchemaProvider>(
        std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded)),
        std::move(options), std::move(defaults));
}
```

### Agent

`Agent::run`, `run_stream`, `complete`는 전체 `sp::runtime::Result`를 반환하고 `std::vector<sp::Message>`를 받는다. `run_stream`은 실제 매 turn에서 typed event를 받으며 표시를 위해 답을 버리고 재요청하지 않는다. `outcomes()`는 각 결과를 보존하고 `usage()`는 nullable 보고를 제공한다. 모델은 호출자가 명시적으로 선택한다.

```cpp
#include <neograph/llm/agent.h>
#include <neograph/llm/schema_provider.h>

sp::runtime::Result run_agent(
    sp::descriptor::ValidatedDescriptor descriptor, sp::runtime::Options options,
    std::string model, std::vector<std::unique_ptr<neograph::Tool>> tools,
    std::vector<sp::Message>& history) {
    auto provider = std::make_shared<neograph::llm::SchemaProvider>(
        std::move(descriptor), std::move(options));
    neograph::llm::Agent agent(provider, std::move(tools), "", model);
    return agent.run(history);
}
```

## 13. MCP Module

**Header:** `<neograph/mcp/client.h>`
**Namespace:** `neograph::mcp`

Model Context Protocol(MCP) 클라이언트 구현입니다. MCP 서버에 연결하고, 사용 가능한
도구를 발견하며, 이를 NeoGraph `Tool` 인스턴스로 감쌉니다.

두 가지 전송을 사용할 수 있습니다:

- **HTTP** — `MCPClient("http://host:port/mcp")`. URL은 서버의 MCP 엔드포인트이며
  경로와 쿼리를 포함해 주어진 그대로 사용되고, 아무것도 덧붙이지 않습니다(이전
  클라이언트는 `/mcp`를 덧붙였습니다). 발견된 도구는 원래의 Streamable HTTP 세션을
  유지하며, 여기에는 `Mcp-Session-Id`, 협상된 프로토콜 버전, 타임아웃, 사용자 정의
  헤더가 포함됩니다.
- **stdio** — `MCPClient({"python", "server.py"})`. 클라이언트는 `fork` 전에 `PATH`를
  해석하고, `execve`로 하위 프로세스를 실행하며, 양방향 파이프를 연결해 자식의
  stdin/stdout으로 줄 단위 JSON-RPC를 주고받습니다. 하위 프로세스는 그것을 만든
  `MCPClient` *또는 어떤 `MCPTool`*이 살아 있는 동안 유지됩니다. 종료할 때는 먼저
  stdin을 닫고 최대 1초 동안 종료를 기다린 뒤, 프로세스 그룹에 SIGTERM을 보내 최대
  1초를 더 기다리고, 그다음 SIGKILL을 보냅니다.

### MCPTool

단일 MCP 서버 도구를 로컬 `Tool` 구현으로 감쌉니다. 발견된 도구는 전송과 관계없이
원래의 프로토콜 세션을 유지합니다.

```cpp
class MCPTool : public AsyncTool, public ContextualAsyncTool {
public:
    // One known tool at an HTTP endpoint. Discovered tools reuse their client session.
    MCPTool(const std::string& server_url,
            const std::string& name,
            const std::string& description,
            const json& input_schema);

    const ToolDefinition& get_mcp_definition() const noexcept;
    CallToolResult execute_result(const json& arguments);
    asio::awaitable<CallToolResult> execute_result_async(const json& arguments);
    asio::awaitable<CallToolResult> execute_result_async(const json& arguments,
                                                         ToolExecutionContext context);
    ChatTool get_definition() const override;
    asio::awaitable<std::string> execute_async(const json& arguments) override;
    asio::awaitable<std::string> execute_async(const json& arguments,
                                               ToolExecutionContext context) override;
    std::string get_name() const override;
};
```


보통 `MCPTool`을 직접 만들지 않습니다 — `MCPClient::get_tools()`가 도구를 발견하고
감쌉니다. URL 생성자로 만든 도구는 첫 호출에서 MCP 세션 하나를 열고, 이후의 모든
호출에서 재사용하며, 도구가 소멸될 때 `DELETE`로 세션을 끝냅니다.

모든 디스패처는 컨텍스트를 받는 `execute_async`를 호출하므로, 실행의 cancel token(attempt
timeout 아래에서는 노드 attempt의 token)과 deadline이 두 전송 모두에서 호출을 제한합니다.
취소하면 즉시 기다림을 멈추고 `graph::CancelledException`을 던지며, 도구 실패 계약은 이를
취소로 다룹니다. deadline이나 요청 타임아웃은 `asio::system_error(asio::error::timed_out)`,
즉 `timed_out` 호출을 던집니다. 어느 경우든 서버에는 `notifications/cancelled`로 알리며,
실행이 호출을 기다리는 코루틴을 취소한 경우에도 마찬가지입니다. 그 뒤에 도착한 응답은
버려집니다. stdio는 JSON-RPC id로 응답을 라우팅하는데 그 호출의 id는 더 이상 기다리지
않으며, HTTP는 요청을 포기합니다. `isError` 결과는 일반 도구 실패인 `std::runtime_error`를
던집니다. Python에서 `mcp.MCPError`는 JSON-RPC 오류의 `code`, `data`, `http_status`를
유지합니다.

### MCPClient

MCP 서버에 연결하고, 초기화 핸드셰이크를 수행하며, 도구를 발견하고 호출하는 메서드를
제공하는 클라이언트입니다.

> `MCPClient`는 하위 클래스로 만들도록 설계되지 않았습니다 — 그대로 사용합니다.
> `rpc_call_async()`가 유일한 전송 수준 호출이며, 동기 메서드도 같은 세션을 구동합니다.
> [`ASYNC_GUIDE.md` §9.5](ASYNC_GUIDE.md#95-mcpclient)를 참고하세요.

```cpp
class MCPClient {
public:
    // HTTP transport: the server's MCP endpoint URL, used as given.
    explicit MCPClient(const std::string& server_url);
    MCPClient(const std::string& server_url, MCPClientConfig config);

    // stdio transport — fork+exec the subprocess.
    explicit MCPClient(std::vector<std::string> argv);
    // Hardened stdio: explicit environment, cwd, bounds, cancellation grace.
    explicit MCPClient(StdioClientConfig config);

    bool initialize(const std::string& client_name = "neograph");
    bool initialize(const std::string& client_name,
                    std::chrono::steady_clock::time_point deadline,
                    std::shared_ptr<graph::CancelToken> cancel_token);
    bool is_initialized() const noexcept;
    InitializeResult get_initialize_result() const;
    std::vector<std::unique_ptr<Tool>> get_tools();
    ListToolsPage list_tools(std::optional<std::string> cursor = std::nullopt);
    std::vector<ToolDefinition> get_tool_definitions();
    json call_tool(const std::string& name, const json& arguments);
    CallToolResult call_tool_result(const std::string& name,
                                    const json& arguments);
    asio::awaitable<CallToolResult> call_tool_result_async(
        const std::string& name, const json& arguments,
        std::chrono::steady_clock::time_point deadline,
        std::shared_ptr<graph::CancelToken> cancel_token);
    void set_tool_list_changed_handler(std::function<void()> handler);
    void shutdown() noexcept;

    // The transport-level JSON-RPC call every other method uses.
    asio::awaitable<json> rpc_call_async(
        const std::string& method, const json& params = json::object(),
        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::time_point::max(),
        std::shared_ptr<graph::CancelToken> cancel_token = {});
};
```


**와이어 프로토콜:** NeoGraph의 MCP 클라이언트는 `protocolVersion = "2025-11-25"`를 제안하고,
공식 SDK 클라이언트처럼 서버가 `2025-11-25`, `2025-06-18`, `2025-03-26` 또는 `2024-11-05`로
응답하면 받아들입니다(`MCP_SUPPORTED_PROTOCOL_VERSIONS`). `2024-11-05` 서버에는 stdio나
Streamable HTTP로 연결하며, 이전의 HTTP+SSE 전송은 지원하지 않습니다. 그 밖의 응답이면 두 버전을 모두 밝히는 프로토콜 오류로
`initialize`가 실패하고, MCP의 버전 협상이 요구하는 대로 클라이언트는 연결을 끊습니다.
HTTP는 이후의 모든 요청의 `MCP-Protocol-Version` 헤더에 협상된 버전을 보냅니다. MCP
`2026-07-28`(`initialize`도 세션도 없음)은 아직 지원하지 않습니다. 그 개정판만 제공하는
서버는 `-32022`로 핸드셰이크를 거부하며, 호출자에게는 `MCPError`로 전달됩니다.

**세션과 전송의 소유:** 두 생성자는 같은 프로토콜 세션을 만듭니다. 세션은 JSON-RPC id,
응답 검증, 초기화(initialized 알림 포함), 페이지 처리, 도구 결과 변환을 소유합니다. 아직
핸드셰이크하지 않은 클라이언트에서 `rpc_call_async()`로 보낸 요청은 먼저 핸드셰이크를
수행합니다. 엔드포인트 URL, 헤더, 협상된 `Mcp-Session-Id`, 요청 타임아웃은 HTTP 전송만이
소유하고, 하위 프로세스, 파이프, 리더, 응답 id 역다중화기는 stdio 전송만이 소유합니다.
발견된 도구는 자신의 프로토콜 세션을 유지하므로 `MCPClient`가 소멸된 뒤에도 사용할 수
있으며, 세션은 클라이언트나 도구에 대한 마지막 참조가 해제될 때 끝납니다.

**전송 규칙 준수:** 두 전송 모두 서버의 요청에 응답합니다. `ping`에는 빈 결과를, 그 밖의
모든 요청에는 `-32601`을 보냅니다. 클라이언트는 어떤 client capability(roots, sampling,
elicitation)도 선언하지 않기 때문입니다. 메시지는 `method`가 없을 때만 응답으로 간주되므로,
대기 중인 호출의 id를 재사용한 서버 요청이 그 호출을 끝내는 일은 없습니다. 서버가 보낸
JSON-RPC 배치(MCP `2025-03-26`)는 메시지마다 따로 처리하고, 그 안의 요청에 대한 응답은 하나의
배열로 돌려보냅니다. HTTP에서
클라이언트는 JSON 응답과 Server-Sent Event 응답을 읽고(WHATWG 규칙에 따라: CRLF, LF, CR
줄 끝, 여러 줄 `data`, `id`, `retry`), 호출의 이벤트 스트림으로 도착한 서버 요청에는 그
스트림을 열어 둔 채 별도의 POST로 응답하며, 응답이 도착하면 스트림 읽기를 멈춥니다.
서버가 응답 전에 닫은 스트림은 스트림의 `retry` 간격이 지난 뒤 `GET`과 `Last-Event-ID`로
이어 받습니다. `initialize` 후 클라이언트는 공식 클라이언트들처럼, 어떤 호출에도 속하지
않는 메시지를 위해 세션의 선택적 `GET` 이벤트 스트림을 엽니다. 그런 스트림이 없는 서버는
405로 응답하며, 클라이언트는 이를 실패가 아니라 스트림이 없다는 뜻으로 받아들여 다시 요청하지
않고 세션과 호출을 그대로 이어 갑니다. 세션 id를 실은 요청에 서버가 404로 응답하면 세션이 끝난 것입니다.
클라이언트는 새 세션을 초기화하고 요청을 한 번 다시 보냅니다. `shutdown()`과 마지막
참조의 해제는 최대 2초로 제한된 `DELETE`로 세션을 끝냅니다.

**알림:** 서버가 `notifications/tools/list_changed`를 보내면
`set_tool_list_changed_handler()`로 등록한 핸들러가 호출됩니다. 그때 `get_tools()`나
`get_tool_definitions()`를 다시 읽으세요. 그때까지는 캐시된 스키마가 오래되었을 수 있으므로
`outputSchema` 검사를 건너뜁니다. 핸들러는 클라이언트의 전송 스레드에서 실행되므로 빨리
반환해야 하며 클라이언트를 호출해서는 안 됩니다.

**강화된 stdio(`StdioClientConfig`):** `argv`, `cwd`, `environment`를 명시합니다.
`replace_environment`를 쓰면 설정한 항목이 서버 환경의 전부가 되고, 쓰지 않으면 서버는 이
프로세스의 환경을 물려받으며 설정한 항목이 그것에 추가되거나 그것을 덮어씁니다(`PATH`
항목은 `argv[0]`을 찾을 위치도 결정합니다). 세션은 서버의 stderr를 계속 비우며 마지막
`max_stderr_bytes`(64 KiB)를 보관합니다. stderr 출력은 세션을 끝내지 않습니다.
`startup_timeout`은 실행부터 initialized 알림까지를, `request_timeout`은 각 요청을
제한합니다. 취소되거나 타임아웃된 요청은 강화된 세션을 끝냅니다. 클라이언트는
`notifications/cancelled`를 보내고, 서버가 멈추도록 `cancel_grace`(500 ms)를 기다린 뒤
프로세스 트리를 종료하며, 이후의 요청은 `MCPFailure::shutdown`으로 실패합니다.

**제한과 실패:** `rpc_call_async(method, params, deadline, cancel_token)`과
`call_tool_result_async()`는 절대 steady-clock deadline과 `graph::CancelToken`을 받습니다.
취소와 deadline은 그 호출에 대한 기다림을 멈추고, 서버에 `notifications/cancelled`로
알리며(`initialize`에는 보내지 않음), 공유 stdio 프로세스와 다른 응답에는 영향을 주지
않습니다. `initialize(name, deadline, cancel_token)`도 같은 방식으로 핸드셰이크를
제한하지만, MCP는 `initialize`를 취소하지 않으므로 서버에 알리지 않습니다. 이때 stdio
서버는 두 번째 핸드셰이크를 거부할 수 있습니다. HTTP는 요청마다
`MCPClientConfig::request_timeout`도 적용합니다. 전송과 와이어 실패는 `MCPTransportError`를
던집니다. 이는 `failure()`(`connection`, `timeout`, `cancelled`, `shutdown`, `http_status`,
`protocol`)와 `http_status()`(HTTP가 아닌 실패에서는 0)를 제공하는 `std::runtime_error`
하위 클래스입니다. JSON-RPC 오류 응답은 서버의 `code()`와 `data()`를 가진 `MCPError`이며,
HTTP 오류 상태가 그것을 실어 온 경우에도 그렇습니다(이때 `http_status()`가 그 상태를
알려 줍니다). 두 클래스 모두 도구 수준의 `isError` 결과를 나타내지 않습니다. 그 결과는
`call_tool_result()`로 확인하세요.

| 메서드 | 설명 |
|--------|-------------|
| `MCPClient(url)` | MCP 엔드포인트 `url`을 주어진 그대로 쓰는 HTTP 모드 클라이언트를 만듭니다 |
| `MCPClient(argv)` | 하위 프로세스를 실행하고 stdio 모드 클라이언트를 만듭니다. `argv[0]`은 fork 전에 `PATH`로 해석되며, exec 실패는 첫 RPC에서 연결 오류로 나타납니다. 안전을 위해 Windows `.bat`/`.cmd`를 거부합니다 |
| `MCPClient(StdioClientConfig)` | 명시적 환경, 제한된 stderr 캡처, deadline, 취소 유예 시간을 갖춘 강화된 stdio 실행 |
| `initialize(client_name)` | MCP 초기화 핸드셰이크를 한 번 수행합니다. 반복 호출은 멱등적이며, 프로토콜/전송 실패는 예외를 던집니다 |
| `initialize(client_name, deadline, cancel_token)` | deadline과 cancel token으로 제한된 핸드셰이크 |
| `get_initialize_result()` | 협상된 프로토콜, capability, 서버 정보, instructions, 원본 결과를 반환합니다 |
| `list_tools(cursor)` | cursor를 불투명하게 다루며 한 페이지를 가져옵니다 |
| `get_tool_definitions()` | 모든 페이지를 따라가며 완전한 도구 메타데이터를 보존합니다 |
| `get_tools()` | 모든 페이지를 발견하고 세션을 보존하는 `MCPTool` 인스턴스를 반환합니다 |
| `call_tool(name, arguments)` | 주어진 인수로 이름에 해당하는 도구를 호출합니다. 원본 JSON 응답을 반환합니다 |
| `call_tool_result(name, arguments)` | content, structured content, `isError`, `_meta`를 보존하는 타입화된 결과 |
| `call_tool_result_async(name, arguments, deadline, cancel_token)` | `rpc_call_async`처럼 절대 steady-clock deadline과 cancel token으로 제한되는 타입화된 호출 |
| `set_tool_list_changed_handler(handler)` | `notifications/tools/list_changed`에서 `handler`를 호출합니다. 빈 핸들러는 호출을 멈춥니다 |
| `shutdown()` | 클라이언트와 그것이 만든 모든 도구를 영구히 끝냅니다. HTTP 세션에는 `DELETE`, stdio 서버에는 순서 있는 종료 |
| `rpc_call_async(method, params, deadline, cancel_token)` | 다른 모든 메서드가 사용하는 전송 수준 JSON-RPC 호출 |

**도구 제공.** `MCPServer`(`<neograph/mcp/server.h>`)는 등록된 핸들러로 `tools/call`에
응답합니다. 핸들러의 실패가 클라이언트에 어떻게 전달되는지는
[Protocol failure mapping](#protocol-failure-mapping)에 있습니다. `2025-11-25`,
`2025-06-18`, `2025-03-26` 또는 `2024-11-05`를 요청하는 클라이언트에는 그 개정판과 그것이
정의하는 필드로 응답하고(`2025-06-18`에는 `icons`와 `execution`이 없고, `2025-03-26`에는
`title`, `outputSchema`, `_meta`, `structuredContent`, `resource_link` 콘텐츠도 없으며, 구조화된
데이터만 있는 결과는 그것을 텍스트로 싣습니다. `2024-11-05`에는 `annotations`와 오디오 콘텐츠도
없습니다. 그 개정판에 없는 콘텐츠 블록은 그 JSON을 담은 텍스트 블록으로 보냅니다), 그 밖의 요청에는 `2025-11-25`로 응답합니다. `protocol_version()`이 그
결과를 알려 줍니다. `2025-03-26`을 협상한 클라이언트는 그 개정판이 수신자에게 요구하는 대로
JSON-RPC 배치를 보낼 수 있고 `2024-11-05` 클라이언트도 마찬가지이며, 배치는 stdio와 HTTP에서
하나의 배열로 응답됩니다. 이후 개정판은 배치를 없앴고 initialize는 배치에 들어갈 수 없으므로,
그런 배치는 `-32600` 오류 하나(HTTP 400)입니다. `handle_message(message, reply)`는 각 메시지의
결과(응답, 배치의 배열 또는 없음)를 호출자에게 넘깁니다. 등록은 `inputSchema`나 `outputSchema`가 `"type": "object"`라고 말하지
않는 도구(예전 기본값인 빈 객체는 `{"type": "object"}`로 읽음), 검증기가 지원하는 JSON
Schema 2020-12가 아닌 스키마, SEP-986의 `A-Za-z0-9_.-` 1~128자를 벗어나는 이름을
거부합니다. 인수는 JSON Schema 2020-12 검증기(`$ref`, `anyOf`, `minimum`, 정수로서의
정수값 `1.0`, 그리고 core, applicator, validation 어휘의 나머지)로 검증됩니다. 알 수 없는
도구는 JSON-RPC 오류 `-32602`이며, 오류 응답은 읽을 수 있었던 요청 id를 싣고 그렇지
않으면 id를 싣지 않습니다. `null`은 절대 쓰지 않습니다. `MCPHttpServer`
(`<neograph/mcp/http_server.h>`)는 같은 것을 Streamable HTTP로 제공합니다. 세션은
`DELETE`로, 요청 없이 `session_idle_timeout`(30분)이 지나면, 또는 `max_sessions`(64)에서
새 클라이언트에게 자리가 필요하고 그것이 진행 중인 요청이 없는 가장 오래 쓰이지 않은
세션일 때 끝나며, 그 id에는 이후 404가 응답됩니다. 도구 호출은 반환될 때까지 HTTP 스레드
하나를 점유하므로 스레드는 필요할 때 `max_http_threads`(64)까지 시작됩니다.
`response_timeout`을 넘긴 요청은 504를 받고 그 도구 호출은 취소됩니다. `DELETE`는 그
연결을 닫습니다. `protected_resource_metadata`가 있으면 서버는 RFC 9728 메타데이터를
게시하고 모든 401 challenge에서 그 위치를 알립니다. `initialize` 이후의 요청은
`MCP-Protocol-Version`에 지원하는 어느 리비전이든 적을 수 있습니다. 세션은 협상한 리비전을
그대로 쓰고, 한 세션에서 다른 리비전을 적은 첫 요청은 stderr에 기록되며, 지원하지 않는 값은
400입니다. loopback 바인드에서는 `Host`가 `localhost`, `127.0.0.1`, `[::1]`이거나
`allowed_hosts`에 있는 이름일 때만 응답합니다. 다른 `Host`는 403, 없거나 형식이 잘못된
`Host`는 400입니다. 원격 바인드는 `allowed_hosts`에 이름이 있을 때만 `Host`를 확인합니다.

**HTTP 사용법:**

```cpp
neograph::mcp::MCPClient client("http://localhost:8000/mcp");
client.initialize();
auto tools = client.get_tools();
```


**stdio 사용법:**

```cpp
// argv[0] is resolved through PATH before fork; inherited fds close before execve.
neograph::mcp::MCPClient client({"python", "/path/to/server.py"});
client.initialize();
auto tools = client.get_tools();   // Tools retain the protocol session/process.
```


---

## 14. Util Module

**Header:** `<neograph/util/request_queue.h>`
**Namespace:** `neograph::util`

### RequestQueue

Lock-free request queue with a worker thread pool and backpressure support.
Decouples HTTP connection acceptance from LLM call concurrency in server applications.

```cpp
class RequestQueue {
public:
    struct Stats {
        size_t pending;        // Tasks waiting in queue
        size_t active;         // Tasks currently executing
        size_t completed;      // Total settled tasks, including cancellation
        size_t rejected;       // Tasks rejected during admission
        size_t num_workers;    // Number of worker threads
        size_t max_queue_size; // Maximum queue capacity
    };

    // num_workers must be greater than zero.
    RequestQueue(size_t num_workers = 128, size_t max_queue_size = 10000);
    ~RequestQueue();

    // Non-copyable
    RequestQueue(const RequestQueue&) = delete;
    RequestQueue& operator=(const RequestQueue&) = delete;

    // Submit a task. Concurrent callers cannot exceed max_queue_size.
    // A full queue returns {false, invalid_future}; an internal enqueue
    // failure returns {false, valid_future}, which throws on get().
    template<typename F>
    std::pair<bool, std::future<void>> submit(F&& task);

    // Idempotently reject new work, cancel queued tasks, and wait for workers.
    void close();
    bool is_closed() const noexcept;

    // Get current queue statistics
    Stats stats() const;
};
```

| Constructor Parameter | Type | Default | Description |
|-----------------------|------|---------|-------------|
| `num_workers` | `size_t` | `128` | Worker thread 수. 0이면 `std::invalid_argument` 발생 |
| `max_queue_size` | `size_t` | `10000` | Maximum number of pending tasks. Tasks beyond this limit are rejected |

| Method | Description |
|--------|-------------|
| `submit(task)` | pending capacity를 원자적으로 예약한 뒤 callable을 enqueue합니다. 수락되면 `first`가 `true`입니다. 가득 찼거나 닫힌 큐는 invalid future와 함께 `false`를 반환하고, 내부 enqueue 실패는 오류를 전파하는 valid future와 함께 `false`를 반환합니다. 수락된 future는 작업 완료 시 resolve되거나 작업 예외를 전파합니다. |
| `close()` | 멱등적으로 새 작업을 거부합니다. 외부 호출자는 모든 worker 종료를 기다리고, 가져가지 않은 작업은 `std::runtime_error("RequestQueue is closed")`로 완료됩니다. 이미 가져간 callable은 완료할 수 있습니다. callable이 `close()`를 호출해 종료를 시작할 수 있지만 자기 자신을 기다리지는 않습니다. |
| `is_closed()` | `close()`가 새 작업 거부를 시작했는지 보고합니다. |
| `stats()` | Returns a snapshot of current queue statistics |

The queue uses `moodycamel::ConcurrentQueue` internally for lock-free enqueue/dequeue
and a condition variable to wake idle workers.

**Usage:**

```cpp
neograph::util::RequestQueue queue(4, 100);  // 4 workers, max 100 pending

auto [accepted, future] = queue.submit([&] {
    // Handle an incoming HTTP request
    auto result = engine->run(config);
    send_response(result);
});

if (!accepted) {
    send_503_service_unavailable();
}
```

---

## Usage Examples

### Minimal ReAct Agent



```cpp
#include <neograph/graph/react_graph.h>
#include <neograph/llm/schema_provider.h>

neograph::graph::RunResult run_react(
    sp::descriptor::ValidatedDescriptor descriptor, sp::runtime::Options options,
    std::string model, std::vector<std::unique_ptr<neograph::Tool>> tools,
    neograph::graph::RunConfig config) {
    auto provider = std::make_shared<neograph::llm::SchemaProvider>(
        std::move(descriptor), std::move(options));
    auto engine = neograph::graph::create_react_graph(
        provider, std::move(tools), "", model);
    return engine->run(config);
}
```


### Custom Graph with Conditional Routing

Building a graph with conditional edges:

```cpp
#include <neograph/neograph.h>
#include <neograph/llm/schema_provider.h>

using namespace neograph::graph;
using json = nlohmann::json;

void run_custom_graph(
    sp::descriptor::ValidatedDescriptor descriptor, sp::runtime::Options options,
    std::string model, std::vector<std::unique_ptr<neograph::Tool>> tools) {
    auto provider = std::make_shared<neograph::llm::SchemaProvider>(
        std::move(descriptor), std::move(options));

    json definition = {
        {"name", "assistant_graph"},
        {"channels", {
            {"messages", {{"reducer", "append"}}},
            {"status",   {{"reducer", "overwrite"}, {"initial", "idle"}}}
        }},
        {"nodes", {
            {"llm",   {{"type", "llm_call"}}},
            {"tools", {{"type", "tool_dispatch"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "llm"}},
            {{"from", "tools"},     {"to", "llm"}}
        })},
        {"conditional_edges", json::array({
            {{"from", "llm"},
             {"condition", "has_tool_calls"},
             {"routes", {{"true", "tools"}, {"false", "__end__"}}}}
        })}
    };

    auto store = std::make_shared<InMemoryCheckpointStore>();
    EngineConfig engine_config;
    engine_config.node_context.provider = provider;
    engine_config.node_context.model = model;
    engine_config.node_context.instructions = "You are a helpful assistant.";
    engine_config.checkpoint_store = store;
    EngineResources resources{.tools = ToolSet(std::move(tools))};
    auto engine = GraphEngine::build(definition, std::move(engine_config),
                                     std::move(resources));

    RunConfig config;
    config.thread_id = "session-1";
    config.input = {{"messages", json::array({
        {{"role", "user"}, {"content", "Search for NeoGraph C++ library"}}
    })}};

    auto result = engine->run(config);

    // Inspect execution trace
    for (const auto& node : result.execution_trace) {
        std::cout << "Executed: " << node << "\n";
    }
}
```

### Human-in-the-Loop with Checkpointing

Using interrupts for human approval:

```cpp
auto store = std::make_shared<InMemoryCheckpointStore>();
EngineConfig engine_config;
engine_config.node_context = ctx;
engine_config.checkpoint_store = store;
auto engine = GraphEngine::build(definition, std::move(engine_config));

// Configure interrupt after the "tools" node
// (set "interrupt_after": ["tools"] in the JSON definition)

RunConfig config;
config.thread_id = "approval-session";
config.input = {{"messages", json::array({
    {{"role", "user"}, {"content", "Delete all files in /tmp"}}
})}};

auto result = engine->run(config);

if (result.interrupted) {
    std::cout << "Interrupted at: " << result.interrupt_node << "\n";
    std::cout << "Reason: " << result.interrupt_value.dump() << "\n";

    // Get human input...
    std::string approval = get_human_approval();

    // Resume with the human's decision
    auto resumed = engine->resume(
        "approval-session",
        {{"approved", approval == "yes"}}
    );
}
```

### Dynamic Fan-Out with Send

Using `Send` for map-reduce patterns:

```cpp
class FanOutNode : public GraphNode {
public:
    std::string get_name() const override { return "fan_out"; }

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto items = in.state.get("items");
        NodeOutput result;
        for (const auto& item : items) {
            result.sends.push_back(Send{
                "process_item",       // target node
                {{"item", item}}      // input for that invocation
            });
        }
        co_return result;
    }
};
```

Each `Send` dispatches the `"process_item"` node with a different input. The engine
executes all sends, collecting their channel writes, before proceeding to the next
edge in the graph.

### Routing Override with Command

Using `Command` to simultaneously update state and control routing:

```cpp
class RouterNode : public GraphNode {
public:
    std::string get_name() const override { return "router"; }

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto messages = in.state.get_messages();
        auto last = messages.back().content;

        NodeOutput result;

        if (last.find("urgent") != std::string::npos) {
            result.command = Command{
                "urgent_handler",                          // goto node
                {{"priority", neograph::json("high")}} // state updates
            };
        } else {
            result.command = Command{
                "normal_handler",
                {{"priority", neograph::json("normal")}}
            };
        }

        co_return result;
    }
};
```

When `Command` is returned, its `updates` are applied to the state and execution
jumps directly to the specified `goto_node`, bypassing normal edge routing.

### SchemaProvider Multi-LLM Support

`SchemaProvider`는 승인된 `sp::descriptor::ValidatedDescriptor`, `sp::runtime::Options`, 선택적 `SchemaProvider::Defaults`를 받는다. descriptor는 closed/versioned 데이터 admission이지 요청/응답 interpreter나 임의 primitive registry가 아니다. credential은 공개 descriptor가 아니라 runtime options에 둔다. Defaults는 typed OpenRouter routing과 Responses 보관(`responses_store`)만 포함하고 후자는 Responses에만 유효하다. Hosted OpenRouter routing·retention·JSON 형식은 선언된 typed 제어다. Images, Veo, Decisions는 별도 NeoGraph typed client와 별도 승인을 쓰며 SDK chat grant를 물려받지 않는다.

```cpp
#include <neograph/llm/schema_provider.h>
#include <descriptor/descriptor.h>
#include <stdexcept>
#include <variant>

std::shared_ptr<neograph::llm::SchemaProvider> admitted_provider(
    std::string_view descriptor_json, std::string api_key) {
    auto loaded = sp::descriptor::load(descriptor_json);
    if (const auto* error = std::get_if<sp::descriptor::ConfigError>(&loaded))
        throw std::invalid_argument(error->message);
    sp::runtime::Options options;
    options.api_key = std::move(api_key);
    neograph::llm::SchemaProvider::Defaults defaults;
    return std::make_shared<neograph::llm::SchemaProvider>(
        std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded)),
        std::move(options), std::move(defaults));
}
```


### MCP Tool Integration

`Agent::run`, `run_stream`, `complete`는 전체 `sp::runtime::Result`를 반환하고 `std::vector<sp::Message>`를 받는다. `run_stream`은 실제 매 turn에서 typed event를 받으며 표시를 위해 답을 버리고 재요청하지 않는다. `outcomes()`는 각 결과를 보존하고 `usage()`는 nullable 보고를 제공한다. 모델은 호출자가 명시적으로 선택한다.

```cpp
#include <neograph/mcp/client.h>
#include <neograph/llm/agent.h>
#include <neograph/llm/schema_provider.h>

sp::runtime::Result run_mcp_agent(
    neograph::mcp::MCPClient& mcp,
    sp::descriptor::ValidatedDescriptor descriptor, sp::runtime::Options options,
    std::string model, std::vector<sp::Message>& history) {
    mcp.initialize("neograph-example");
    auto tools = mcp.get_tools();
    auto provider = std::make_shared<neograph::llm::SchemaProvider>(
        std::move(descriptor), std::move(options));
    neograph::llm::Agent agent(provider, std::move(tools), "", model);
    return agent.run(history);
}
```


## Beyond this tour

The headers under `include/neograph/` carry public surface that
isn't walked through above. Each block below is a one-paragraph
pointer to that canonical source-level reference.

### `neograph::a2a` — Agent-to-Agent protocol

**Header:** `<neograph/a2a/{client,server,types,a2a_caller_node}.h>`
`A2AServer`는 graph(또는 admitted `ProgramVersion`)를 HTTP와 SSE 기반 A2A JSON-RPC binding으로 노출하고, `A2AClient`는 원격 agent를 호출한다. A2A 1.0(v1.0.1)이 기본 버전이고 0.3은 호환 dialect다(`WireDialect::{V1_0,V0_3}`). 공식 SDK(a2a-sdk 1.2.2, @a2a-js/sdk 1.3.0과 0.3.14)가 `bindings/python/tests/interop/a2a_suite`에서 양방향을 시험한다.

#### A2A 버전 정책

- **요청.** server는 각 요청을 header 또는 `A2A-Version` query parameter의 `A2A-Version`이 지정한 dialect로 응답한다. 버전이 없으면 0.3이다(spec §3.6.2). Major.Minor `1.0`과 `0.3`만 지원하며 patch는 무시한다. `0.2`, `1.5`, `2.0` 같은 다른 값은 지원 버전을 명시한 VersionNotSupportedError(`-32009`)를 받는다. 두 method 표기(`SendMessage`, `message/send`)는 어느 버전에서도 받는다.
- **Card.** card는 버전별로 제공한다. `A2A-Version: 1.0`은 순수 1.0 `AgentCard`를 받고, 버전 없는 요청은 0.3 card를 받는다. 0.3 card도 `supportedInterfaces` 첫 항목으로 1.0 interface를 나열한다. 응답에는 `ETag`, `Cache-Control: public, max-age=300`, `Vary: A2A-Version`이 붙고 `If-None-Match`에는 304로 답한다. `AgentCard::url`을 설정하지 않으면 card는 client가 사용한 주소를 광고한다. 모든 skill은 tag를 하나 이상 갖는다. card는 구현하지 않은 push notification, extended card, HTTP+JSON·gRPC binding을 주장하지 않는다.
- **Client.** client는 첫 호출 전에 card를 가져와 `1.x` 또는 `0.x` 버전의 첫 JSONRPC interface를 선택하며, URL이 client의 `base_url`과 같은 interface를 우선한다. card를 가져올 수 없을 때만 probe한다. 1.0을 먼저 보내고 `-32601`, `-32009`, HTTP 400이면 0.3으로 바꾸며 결과를 기억한다. RPC는 선택한 interface URL이 `base_url`과 같은 origin이면 그 URL로 가므로 endpoint가 `/a2a/v1`인 agent도 동작한다. `set_allow_cross_origin_interfaces(true)`로 허용하지 않으면 card는 client를 다른 origin으로 보낼 수 없고, 이때 RPC는 `base_url`로 가며, client는 따르지 않은 origin을 밝히는 경고를 stderr에 쓴다. `endpoint_url()`은 사용 중인 endpoint를 알려 주며 message id는 random UUID다.
- **0.3.** 0.3은 같은 의미를 가진 호환 dialect로 남는다. 새 기능은 1.0을 대상으로 한다.

#### Task와 method

모든 run은 자체 worker thread에서 실행된다. stream이나 blocking send는 이를 관찰할 뿐이므로 task는 끊긴 연결보다 오래 산다. `SendMessage`는 task가 terminal 또는 interrupted 상태가 될 때까지 block하지만, `returnImmediately`(0.3 `blocking: false`)를 주면 SUBMITTED task를 즉시 반환한다. `SendStreamingMessage`와 `SubscribeToTask`(0.3 `tasks/resubscribe`)는 Task를 먼저 보내고 status·artifact update를 이어 보내며 terminal 또는 interrupted 상태에서 닫힌다. 0.3 stream은 `final: true`로 끝난다. 서버가 재시작 후 복구한 Program task는 이 프로세스가 run을 시작하지 않았으므로 자체 channel이 없다. 이런 task에 `SubscribeToTask`를 호출하면 Task를 보낸 뒤 영속 run을 따라가다가, run에 결과가 생기거나 cancel이 확정되면 결과 artifact와 최종 status를 다른 run과 같게 보낸다(멈춘 run은 `TASK_STATE_INPUT_REQUIRED`로 끝난다). 복구 이후 이미 끝난 run은 먼저 읽어서 terminal로 보고 `-32004`로 답한다. run을 따라갈 수 없으면 stream은 `-32603` 오류 이벤트(ErrorInfo reason `RUN_UNAVAILABLE`)로 끝나고, 이 서버에 run이 없는 task면 호출이 `-32603`으로 답한다. 빈 채로 닫히는 경우는 없다. 5초 동안 조용한 stream은 SSE 주석 `: keep-alive`를 쓴다. 유휴 연결에 타임아웃을 거는 클라이언트나 프록시가 아직 작업 중인 run을 끊지 않게 하려는 것이다. `GetTask`와 `ListTasks`는 `historyLength`를 따른다(0이면 history 생략). `ListTasks`는 `contextId`, `status`, `statusTimestampAfter`로 거르고 `pageSize`(1~100, 기본 50)와 `pageToken`으로 최신순 paging하며, `includeArtifacts`가 없으면 artifact를 생략한다. 모든 status에는 timestamp가 붙는다.

task id는 server가 생성한다. 모르는 `taskId`를 가진 message는 `-32001`, terminal task는 `-32004`, task와 다른 `contextId`는 `-32602`, 아직 실행 중인 task는 `-32603`(ErrorInfo reason `TASK_BUSY`)으로 실패한다. INPUT_REQUIRED task에 보낸 message는 interrupt가 저장한 checkpoint에서 run을 재개한다. `GraphAgentAdapter::build_resume_value`가 message를 resume value로 바꾸며, checkpoint store가 없는 engine은 그 message를 input으로 graph를 다시 실행한다. Program 기반 task는 아직 A2A로 그 답을 받을 수 없다. message는 `-32004`를 받고 task는 계속 기다린다. NeoGraph collaboration envelope는 발신자가 정한 idempotent task id를 유지한다([A2A collaboration 확장](#a2a-collaboration-extension)). 요청은 dialect별로 검증하며 위반은 `google.rpc.BadRequest` field violation과 함께 `-32602`를 받고, 모든 error는 `google.rpc.ErrorInfo` detail을 담는다. `CollaborationAuthenticator`가 있으면 card는 Bearer security scheme을 선언하고, 요청 인증이 필수면 전역 requirement도 선언한다. credential이 없으면 `WWW-Authenticate`와 함께 HTTP 401, 인증됐지만 collaboration link를 쓸 수 없는 peer는 HTTP 403을 받는다.

<a id="a2a-error-mapping"></a>
#### A2A error 매핑

run의 실패 class는 [프로토콜 실패 매핑](#protocol-failure-mapping)대로 peer에 전달된다. task 상태, status message인 public message, `metadata["neograph/failure"] = {code, retryable, retryable_by_class}`이며 exception text는 절대 전달되지 않는다. 완료한 run은 agent 답을 status message로 하고 adapter artifact를 담은 `COMPLETED`, step limit은 code `NG.StepLimit`의 `FAILED`, failure exit는 exit가 선언한 code의 `FAILED`다. server가 거절하는 요청:

| 요청 | A2A 결과 |
|---|---|
| 모르는 task를 가리키는 message | `-32001` TaskNotFoundError |
| terminal task에 보낸 message | `-32004` UnsupportedOperationError. collaboration 재전달은 그 task를 반환 |
| `contextId`가 task와 다른 message | field violation과 함께 `-32602` |
| 아직 실행 중인 task에 보낸 message | `-32603`, ErrorInfo reason `TASK_BUSY`, domain `neograph` |
| input을 기다리는 Program task에 보낸 message | `-32004`. task는 계속 기다림(Program의 input 요청에 A2A로 답하는 기능은 아직 없음) |
| in-flight 한도 도달 | `-32603`, ErrorInfo reason `RESOURCE_EXHAUSTED`, domain `neograph`, metadata `retryAfterSeconds` |
| agent가 요구하는 credential이 없거나 틀림(collaboration message, 또는 strict 인증에서 모든 요청) | `WWW-Authenticate`와 함께 HTTP 401. body는 ErrorInfo reason `UNAUTHENTICATED`, domain `neograph`인 JSON-RPC `-32600` |
| collaboration link가 허용하지 않는 인증된 peer | HTTP 403. body는 ErrorInfo reason `PERMISSION_DENIED`, domain `neograph`인 JSON-RPC `-32600` |
| terminal task의 cancel | `-32002` TaskNotCancelableError. task는 변하지 않음 |
| push notification 설정; extended card; card가 `streaming = false`일 때의 streaming method | `-32003`; `-32004`; `-32004` |
| 1.0도 0.3도 아닌 `A2A-Version` | `-32009` VersionNotSupportedError |

모든 error는 `google.rpc.ErrorInfo`를 하나 담으며, 행에 `neograph`라고 적혀 있지 않으면 domain은 `a2a-protocol.org`다.

<a id="a2a-collaboration-extension"></a>
#### A2A collaboration 확장

`CollaborationMailbox`를 가진 Program agent는 NeoGraph collaboration envelope를 받으며, 두 버전의 card 모두 `capabilities.extensions`에 이를 선언한다. `uri`는 `a2a::COLLABORATION_EXTENSION_URI`(이 절), `required`는 false, `params`는 `{"format": "neograph-a2a-collaboration-v1", "metadataKey": "neograph_collaboration"}`이다. 그런 message는 data part `{format, envelope}`와 `metadata.neograph_collaboration`을 담으며, 연결된 collaboration의 인증된 peer만 보낼 수 있다. 발신자가 receiver task id를 미리 정하므로 첫 전달은 그 id로 task를 만들고, 재전달은 run을 새로 시작하지 않고 기존 task를 반환한다. 일반 A2A client에는 필요 없다.

#### Client stream과 Python

SSE는 LF/CRLF/CR, comment, multiline data, 마지막 unterminated data를 처리한다. opening task snapshot, status update(metadata 포함), artifact append/replace update를 누적해 반환 task를 만든다. event를 관측하면 external callback이 없어도 dialect redispatch를 금지한다. non-SSE RPC error는 `A2ARpcError::code()`로 code를 보존하고, non-2xx HTTP는 성공 task가 될 수 없다. caller node 답 우선순위는 final/interrupted agent status text, 첫 artifact text, 마지막 agent history text이며 progress status나 user history는 답을 덮지 않는다. 일반 `async_post_stream`은 nonempty fixed-`Content-Length` body를 status와 함께 한 번 전달하고 zero length는 chunk를 내지 않는다. body limit, early EOF, 이미 buffer된 surplus는 거부한다. redirect/chunked/close-delimited 규칙은 그대로다.

Python `neograph_engine.a2a`는 `WireDialect`, `AgentInterface`, `AgentCard.supported_interfaces`/`raw`, `Part.media_type`, `MessageSendConfiguration`, `MessageSendParams`, `StreamEvent`와 status/artifact record를 제공한다. `A2AClient.wire_dialect()`는 enum 또는 `None`을 반환하고, `set_authorization_header()`, `set_allow_cross_origin_interfaces()`, `endpoint_url()`은 native client와 같다. `send_message(params)`는 multipart overload다. `send_message_stream(text, on_event, task_id="", context_id="")` 또는 `(params, on_event)`는 누적 `Task`를 반환하고 bool을 돌려주는 callback에 owned event snapshot을 전달한다. blocking call은 GIL을 해제한다. `a2a.A2ARpcError.code`는 원격 정수 code를 보존한다.

**공개 헤더:** [`include/neograph/a2a/`](../include/neograph/a2a/).

### `neograph::acp` — Agent Client Protocol

**헤더:** `<neograph/acp/{server,types}.h>`
stdio 위 줄 단위 JSON으로 주고받는 에디터↔에이전트 JSON-RPC 2.0이다 (Zed, Gemini CLI, Neovim CodeCompanion). ACP 프로토콜 버전 1이며, 공식 스키마 v1.25.0과 공식 Python(0.12, 1.0)·TypeScript SDK로 검증한다. client→agent는 `initialize`, `session/{new,resume,prompt,cancel}`이다. agent→client는 `session/update`, `session/request_permission`이며, `fs/*`, `terminal/*`, `elicitation/*`는 클라이언트가 해당 capability를 알렸을 때만 지연 바인딩된 `ACPClient`로 보낸다. 클라이언트가 `error`로 답하면 메서드, 코드, 메시지, `data`를 담은 `acp::ACPClientError`를 던지며, 요청이 성공할 수 없다는 코드이면 재시도되지 않는 `acp::ACPClientRefusal`이 된다([프로토콜 실패 매핑](#protocol-failure-mapping)). `ACPServer::handle_message`는 각 프롬프트 턴을 워커 스레드에서 실행하며, 동시에 최대 `max_inflight_prompts`(32)개, 세션당 한 개만 받는다. 거절된 턴은 JSON-RPC `-32603`이고 `data.reason`은 `"overloaded"` 또는 `"session_busy"`이다. `-32000`은 ACP의 "Authentication required"이므로 쓰지 않는다. `initialize` 전 호출은 `-32600`, 알 수 없는 세션은 `-32002`이며, 에이전트가 알지만 체크포인트가 없는 세션의 `session/resume`은 성공한다.

프롬프트는 text와 `resource_link` 블록(기본 콘텐츠이며, 기본 어댑터는 링크를 입력에 `[name](uri)`로 넣는다)을 받고, image·audio·embedded resource는 `capabilities().prompt`로 알렸을 때만 받는다. 형식이 잘못되었거나 알리지 않은 블록은 `-32602`이다. 기본 어댑터는 `_acp_session_id`, `_acp_cwd`, `_acp_additional_directories`, `_acp_mcp_tools`를 채운다. 바꾸려면 `build_session_state`(또는 기존 `build_initial_state`)를 재정의한다. `agentInfo`는 항상 ACP `Implementation`이며, 빠진 name이나 version은 채운다.

턴이 끝나는 방식:

| NeoGraph 결과 | ACP 결과 |
|---|---|
| 완료 | `agent_message_chunk` 하나(빈 텍스트면 없음) 뒤 `stopReason: "end_turn"` |
| `session/cancel`로 취소 | `stopReason: "cancelled"` |
| 단계 한도(`RunConfig::max_steps`) | `stopReason: "max_turn_requests"` |
| 모델 토큰 예산 초과 | `stopReason: "max_tokens"` |
| 프로바이더 거부 또는 콘텐츠 필터(`NG.SafetyBlocked`) | `stopReason: "refusal"` |
| `NodeInterrupt` | 같은 턴 안의 `session/request_permission`. 응답으로 실행을 재개한다 |
| Hook 차단(`NG.HookBlocked`), 노드·도구 실패(`NG.TaskFailed`, 작성자 코드), 시간 초과(`NG.Timeout`), 프로바이더 실패(`NG.ProviderFailed`), 금액·할당량 예산(`NG.BudgetExhausted`), 설정·논리·알 수 없는 실패 | JSON-RPC 오류 `-32603`, message = 공개 메시지, `data: {code, retryable, retryable_by_class}`([프로토콜 실패 매핑](#protocol-failure-mapping)) |
| 클라이언트가 아닌 원인으로 취소(노드 중단, 종료) | JSON-RPC 오류 `-32800` |

예외 텍스트(`what()`)는 클라이언트로 가지 않는다. message는 타입이 정해진 공개 메시지(`NodeFailureError::public_message`)나 실패 클래스별 일반 문구이며, 세부 내용은 호스트 로그에만 남는다. 인터럽트는 사유를 제목으로, 페이로드를 `rawInput`으로 하고 `_meta["neograph/interrupt"]`와 선택지 "approve"(`allow_once`), "reject"(`reject_once`)를 가진 도구 호출이 된다. 선택된 option id가 재개 값이다. 바꾸려면 `interrupt_permission`과 `interrupt_resume_value`를 재정의한다. `cancelled` 응답은 턴을 취소로 끝낸다. 어댑터가 `std::nullopt`를 반환하거나 클라이언트가 응답하지 못하면 턴은 `data.code: "NG.Interrupted"`로 실패하고 세션은 일시 정지 상태로 남지 않는다. 에이전트 재시작 뒤처럼 `session/resume`이 체크포인트에서 복원한 일시 정지는 다음 프롬프트의 텍스트가 응답한다.

`session/cancel`은 진행 중인 턴만 취소한다. 실행 중인 턴이 없을 때 온 취소는 무시한다(메시지 순서를 바꾸는 전송은 `set_pre_armed_cancel(true)`로 이전의 사전 취소를 유지할 수 있다). `requestId`가 실행 중인 `session/prompt`의 id인 `$/cancel_request` 알림도 같은 방식으로 턴을 취소하며, 그 프롬프트는 여전히 응답을 하나(`stopReason: "cancelled"`) 받는다. 알 수 없거나 끝난 요청 등 다른 요청의 `requestId`는 무시한다. 취소 뒤 대기 중인 agent→client 요청은 응답할 시간 500 ms를 받고, 그 뒤 서버는 `$/cancel_request`를 보내고 턴을 끝낸다. 권한 요청은 `ACPClient::set_permission_timeout`으로 정하지 않으면 시간 제한이 없다. stdin이 EOF에 닿으면 `run()`은 진행 중인 턴이 `set_eof_grace`(1초) 동안 끝나기를 기다린 뒤 취소한다.

`session/new`와 `session/resume`의 stdio `mcpServers`는 NeoGraph의 MCP 클라이언트(`NEOGRAPH_BUILD_MCP_CLIENT`로 빌드)로 세션의 `cwd`에서 시작하며, 환경은 최소한으로, 에이전트의 `PATH`(Windows에서는 `SystemRoot`도)에 클라이언트의 `env` 항목을 더한 것이다. provider API 키가 있을 수 있는 나머지 에이전트 환경은 넘기지 않는다. 노드는 `ACPClient::session_tools(session_id)`로 그 도구를 쓴다. HTTP와 SSE MCP 서버는 알리지 않으며 `-32602`로 거절한다. 시작하지 못한 서버는 요청을 `-32603`, `data.reason: "mcp_server_unavailable"`로 실패시킨다.

**공개 헤더:** [`include/neograph/acp/`](../include/neograph/acp/).

### `neograph::async` — HTTP/SSE/WS helpers

**Header:** `<neograph/async/{conn_pool,http_client,sse_parser,ws_client,curl_h2_pool,run_sync}.h>`
일반 NeoGraph HTTP/SSE/WebSocket helper는 비-provider 통합에서 사용할 수 있다. SchemaProvider의 transport·codec·retry 권한이 아니다. typed chat-family 호출은 SDK runtime과 `ProviderMode`를 사용하며 구 Responses WebSocket provider 경로와 descriptor stream parser는 제거되었다.

**공개 헤더:** [`include/neograph/async/`](../include/neograph/async/).

### Persistent checkpoint backends

**Header:** `<neograph/graph/postgres_checkpoint.h>`,
`<neograph/graph/sqlite_checkpoint.h>`
`PostgresCheckpointStore` — libpq-based, 3-table schema (`neograph_*`)
with channel-blob deduplication keyed on
`(thread_id, channel, version)`; LangGraph `PostgresSaver` parity. Async
initial/replacement connections use one global deadline across all hosts:
positive `connect_timeout` written directly in the connection string (minimum
2s), otherwise a 30s safety default. Environment and service-file timeout
values are not available before initial connection setup and use that default.
Synchronous libpq connection timeout behavior is unchanged.
`SqliteCheckpointStore` — same shape, single-file backend, fits the
edge / single-host deployments.
**공개 헤더:**
[`PostgresCheckpointStore`](../include/neograph/graph/postgres_checkpoint.h) ·
[`SqliteCheckpointStore`](../include/neograph/graph/sqlite_checkpoint.h).

### Other public surface not in this tour

- **`neograph::AsyncTool`** — `Tool` peer that exposes
  `execute_async(json)` for tools whose work is naturally
  coroutine-shaped (HTTP fetch, MCP call). Sync `execute()` is
  `final`-routed through `run_sync`.
- **`neograph::graph::NodeCache`** — per-node memoization, opt-in
  at construction via `EngineConfig::cached_nodes` (the setter remains a
  compatibility surface).
- **`neograph::graph::create_deep_research_graph`** —
  open_deep_research-style supervisor + sub-researcher fan-out,
  used by `examples/25_deep_research.cpp`. It includes a
  `BriefNode` LLM rewrite, a `FinalReportNode` token-limit retry and
  a `ClarifyNode` HITL gate. `DeepResearchConfig::model` is required;
  the steps send `DeepResearchConfig::controls` and their configured
  output caps, and no sampling of their own.

If the type you need isn't in this tour, check `include/neograph/`
directly. Every public header carries its reference documentation.
