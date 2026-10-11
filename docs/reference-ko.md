<!-- neograph-i18n: source=docs/reference-en.md locale=ko source_sha256=79727c2b30c2623180976aae1d3a09ad00a17554779a9b48e209c585311165bf -->
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

추가 모듈은 `include/neograph/{a2a,acp,async}/` 공개 헤더를 제공한다. 개별 서술 안내는 유예되었으므로 정확한 계약은 헤더를 참조한다. 파일 존재는 현재 통합 검증을 뜻하지 않는다.

**편의 헤더:** `#include <neograph/neograph.h>`에는 전체 핵심 API와 그래프 엔진 API가 포함된다.

CMake 3.20 이상이 필요하다. Core가 소유 typed provider 계약을 공개하므로 `NEOGRAPH_BUILD_LLM=OFF`여도 SchemaProvider runtime은 필수다. 명시적 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`를 우선하고 없으면 설치된 `SchemaProvider` runtime package를 찾는다. 없고 `NEOGRAPH_FETCH_SCHEMAPROVIDER=ON`(기본)이면 `cmake/NeoGraphSchemaProvider.cmake`에 고정된 불변 GitHub archive를 다운로드한다. Offline build에서는 SDK를 설치하고 `CMAKE_PREFIX_PATH`에 prefix를 지정한 뒤 `-DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`를 준다. Sibling checkout을 추측하거나 제거된 bundled interpreter를 선택하지 않는다. NeoGraph 선택적 HTTP module을 꺼도 SDK runtime의 transport 의존성은 필요하다. 기록된 SDK runtime/archive 검증은 Linux/POSIX 범위다. Windows NTFS와 macOS 구현이 있으나 새 platform 검증에는 runtime 증거가 필요하며 WASM provider runtime 검증은 입증되지 않았다.

SDK imported target이 `include/SchemaProvider` include root를 제공한다. 공개 예시는 recipe 전용 helper 없이 `<descriptor/descriptor.h>`, `<runtime/client.h>`, `<neograph/llm/schema_provider.h>`를 직접 사용한다.

설치 SDK package는 최소 `0.1.1`이고 interface revision 4 header와 shared-library generation 4가 일치해야 한다. 버전 일치만으로 이전 interface/ABI binary를 승인하지 않는다.

```cmake
find_package(SchemaProvider 0.1.1 CONFIG REQUIRED COMPONENTS runtime)
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
- [3. Tool Interface](#3-tool-interface)
  - [Tool](#tool)
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
};
```

| Field | Type | Description |
|-------|------|-------------|
| `name` | `std::string` | Unique tool name |
| `description` | `std::string` | Description shown to the LLM to explain the tool's purpose |
| `parameters` | `json` | JSON Schema object describing accepted parameters |

### Owned Outcome

공급자 호출은 `sp::runtime::Result`, 즉 `sp::Completion` 또는 `sp::Failure`를 담은 불변 소유 `std::shared_ptr<const sp::Outcome>`를 반환한다. 표시 텍스트만이 아니라 전체 결과를 보존한다. 순서 있는 메시지/파트, native continuation, 존재하는 wire envelope, 순서 있는 raw 관측, 중단 근거와 실제 시도 메타데이터는 호출 및 클라이언트 소멸 후에도 남는다. `input_total`, `output_total`, `total` 같은 사용량 카운터는 `std::optional<sp::Count>`이며 존재하는 count는 `uint64_t value`와 `Evidence`를 가진다. `Usage`에는 stage, quality, conflict도 남는다. 누락은 미상이며 0을 만들어 넣지 않는다. 실패도 원래의 부분 결과를 보존한다. `ProviderFailure::outcome()`과 `ProviderObserverError::outcome()`은 실제 결과를 보존하며 후자의 `cause()`에는 관측자 예외가 남는다.

`sp::Completion::wire_envelope`과 `sp::PartialCompletion::wire_envelope`은 제공자 계열별로 존재 여부가 다르며 비어 있을 수 있다. 없으면 Python 뷰는 `None`을 반환한다. 현재 버퍼링 Chat은 이 필드를 비워 두고 전체 응답 문서를 `raw_events`의 `sp::RawWire{type="chat.completion", payload=document}`로 보존한다(Python에서는 `ProviderRawWire`). 존재 여부와 실제 타입 기반 raw 증거를 확인한다. 폴백으로 봉투를 합성하거나 부분 실패를 성공으로 바꾸지 않는다. 제공자의 비공개 필드를 포함한 소유된 와이어 증거는 제공자 소멸 후에도 보존된 결과에 남는다. 네이티브 추적은 raw 봉투/이벤트와 네이티브 재생/추론을 제외한다. JSON 조회 복사본은 네이티브 권한이나 재정 권한을 부여하지 않는다.

소비자는 전체 타입 기반 메시지/파트, 논리적 역할과 텍스트를 사용하고, 이어서 실행하는 데 필요하면 실제 네이티브 소유권을 유지해야 한다. 체크포인트와 Chat 요청의 텍스트 content는 텍스트 문자열 또는 유효한 타입 기반 텍스트 파트 배열로 표현할 수 있다. 우연히 선택된 어느 직렬화 형태도 보편적 계약이 아니다.

실제 `NativeContext` 재생에는 원래 `request.messages`의 전체 prefix를 보존하고 직접 반환된 `outcome.messages`를 네이티브 소유권을 유지한 채 순서대로 이어 붙여야 한다. 직접 반환된 결과에는 새로 반환된 메시지만 있으며 원래 요청 이력은 없다. 그 결과의 assistant 메시지만 재생하면 와이어 I/O 전에 `ReplayIneligible`로 거부된다. `NativeArchive`에서 복원한 assistant도 원래의 전체 prefix가 필요하다. 아카이브 보관 권한은 이력 계보 검사를 대신하지 않는다. 그래프의 `RunResult.native_messages`에는 이미 전체 이력이 있으므로 원래 입력을 다시 앞에 붙이지 않는다.

`UsageAccumulator::snapshot()`은 누적 보고를 반환한다. `total_tokens_wide()`는 청구 토큰과 미해결 예약의 합이며 보고 사용량으로 표시하면 안 된다. 정산에는 input/output count가 있는 final·consistent 보고가 필요하며 근거가 있는 가장 큰 total을 차감하고 초과 사용량도 clamp하지 않는다. 누적 보고 중 하나라도 counter가 없으면 합계도 미상이다. 예약, 로컬 차감, vendor 청구서는 서로 다른 기록이다.

### Portable projections


실제 결과 이후 post-effect 정산이나 terminal receipt 영속화가 실패하면 `ProviderDispatchOutcomePersistenceError`의 `outcome()`은 원래 불변 결과를, `cause()`는 원래 영속 예외를 보존한다. 전달도 실패했으면 `delivery_error()`가 원래 관측자 예외를 보존한다. 영속화 성공 뒤 관측자 실패는 원래 예외를 그대로 다시 던진다. 미상/결과 없는 transport 실패는 결과를 조작하지 않는다.
`ChatMessage` / `ChatTool`과 JSON은 portable projection이지 native 권한이 아니다. Portable 포맷은 [`provider-message-v2`](../schemas/provider-message-v2.schema.json), [`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json)를 유지한다. 실제 C++ checkpoint sidecar는 메모리에서 native seal을 보존한다. 영속 native 기록에는 host-owned `sp::NativeArchive`가 필요하다. closed v3 / `spna3`는 독립 키를 쓰는 인증된 owner-private custody이며 archive v2는 업그레이드하거나 해석하지 않고 거부한다. 인증은 모든 semantic descriptor 선택(origin/path/header, policy, 요청 field mapping, usage path, stop mapping), owner와 정확한 custody binding을 결합한다. 암호화나 vendor-issuer 인증은 아니다. archive 본문·키·native blob·raw wire 관측을 공개하지 않는다. Archive는 증거 저장소이지 돈의 grant나 spending lease가 아니다. Program/external bank는 독립 journal 소유이며 snapshot 복사로 credit을 만들 수 없다.

`RuntimeHistoryRecord`의 인자 없는 `serialize_canonical()`은 이식 가능한 기록을 지원한다. 영속 네이티브 이력에는 `serialize_canonical(archive, owner_id)`와 해당 ID에 소유자 범위가 일치하는 아카이브를 사용한다. Python도 같은 오버로드와 `RuntimeHistoryRecord.parse(stored_bytes, archive=None, owner_id="")`를 제공한다. 이식 가능한 기록용 기본 인자는 일치하는 아카이브 없이 네이티브 복원을 허가하지 않는다. Python의 파싱과 아카이브를 사용하는 직렬화는 네이티브 작업 중 GIL을 해제한다. JSON 관찰 데이터와 아카이브로 인증된 보관 권한은 구별된다.

Python은 `ContextStore.hydrate_records(range)`와 `history_record_by_message_id(feed, message_id)`로 타입 기반 보관 권한을 유지한다. 후자는 기록 또는 `None`을 반환한다. `SQLiteContextStore(database_path, archive=None)`는 실제 아카이브를 받으며 `LocalProgramHost`는 마지막 선택적 인자 `native_history_archive=None`을 `RuntimeConfig`로 전달한다. 이 인자는 권한을 조작하거나 영속 Program 저장소 백엔드를 추가하지 않는다. 반환된 `RuntimeHistoryRecord.message`는 실제 공유 네이티브 소유권을 유지하는 분리된 타입 기반 복사본이다. 이를 변경해도 불변 RAW 식별자는 바뀌지 않는다. 저장소 생성과 타입 기반 조회는 GIL을 해제하며, 보관 권한이 필요한 네이티브 이력은 아카이브 없는 저장소에서 거부한다.

`Assistant` 메시지를 담은 RAW `RuntimeHistoryRecord`에는 `RuntimeTrustClass.ModelOutput`이 필요하다. `RuntimeTrustClass.UntrustedInput`은 `User` 메시지만 허용한다. 이 이름들은 실제 Python enum 이름이다. trust class는 기록의 역할과 출처 경계를 나타내며, 이를 선택해도 네이티브 재생 보관 권한이나 지출·실행 권한은 생기지 않는다.

**Standalone bank journal 수정 — 현재 계약 개정; 실제 runtime 증거는 아래.** Owner-approved protocol은 단조 trusted-store namespace obligation과 실제 불변 original owner/thread/graph scope, ceiling, deadline/clock identity, generation을 요구한다. 전체 checkpoint commitment·revision에 대한 정확한 durable head CAS만 host-owned opaque lease를 발급할 수 있다. 정확한 pending effect window를 provider I/O 전에 영속화해야 하며 실제 SDK outcome, charge, nullable report, hold, dedup identity로 정산해야 한다. Checkpoint와 next head는 같은 owned actor/revision 아래 원자적으로 publish해야 한다. Bank metadata 제거·checkpoint pruning·old authenticated snapshot replay·같은 ID overwrite·actor 상실은 credit을 주면 안 된다. 기존 65 hold에서 ceiling 130을 129로 낮추면 추가 65를 허용할 수 없다. 입증된 no-effect 실패는 unchanged head를 release해 authentic 130 복구가 가능해야 한다. Crash/unknown/lost-lease window는 refund/retry/fallback 없이 hold를 유지한다. Plain/pristine archive 설정은 money/native spending lease를 주지 않고 현재 `config.usage`는 기존 standalone obligation을 대체할 수 없다. Program/external-bank journal 소유는 유지된다. 이는 요구 계약이다. 실제 currency/custody 증거와 instrumentation 한계는 아래에 있으며 stable released API 보장은 아니다.

**현재 선언; 통합 runtime 증거는 아래:** `<neograph/graph/checkpoint.h>`는 `owner_scope`, logical `thread_id`, private backend `storage_thread_id`, `graph_identity`, `original_ceiling`, `original_deadline_ticks`, `deadline_clock_identity`를 가진 `ManagedBudgetLeaseScope`를 선언한다. `OwnedManagedBudgetLease`는 read-only `scope()`, `actor_id()`, 불변 `bank_generation()`, `revision()`, `head_checkpoint_id()`, `head_commitment()`를 노출하며 공개 authority-import constructor가 없다. `ManagedBudgetEffectReceipt`는 `active()`, `effect_id()`, `claim_amount()`, `request_digest()`를 노출하고 default receipt는 권한을 주지 않는다. `CheckpointStore`는 `acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`, `begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`, `settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`, `publish_managed_budget_checkpoint(lease, checkpoint)`, `release_managed_budget_lease(lease)`와 `_async` counterpart를 선언한다. Sync `CheckpointStoreCore`와 `AsyncCheckpointStore`는 각각 해당 variant를 제공한다. `managed_budget_checkpoint_commitment(checkpoint)`는 bank JSON뿐 아니라 전체 영속 checkpoint를 결합한다. 이 선언은 backend CAS·currency 안전성·installed ABI 호환성·실제 성공 runtime 경로를 입증하지 않는다.

**실제 InMemory shared-bank fork는 유지되었으며 실제 증명 완료.** 원래 실제 C++ fork는 ONE original financial journal과 trusted current branch head를 쓰며 grant를 복제하지 않는다. `publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)` 및 `_async`는 authentic current source/full commitment와 실제 same-bank native C++ pointer를 요구하고 durable standalone fork는 명시적 unsupported로 남는다. `OwnedManagedBudgetLease::scope()`와 original owner/thread/graph, ceiling, deadline/clock, generation은 불변이다. Read-only store-issued `execution_thread_id()` / `execution_storage_thread_id()`는 execution branch를 별도로 선택하며 `GraphState::budget_original_thread_id()`는 원래 financial bank를 가리킨다. 정확한 selected-branch head CAS와 global actor/revision은 canonical current counter, pending effect, burned identity에 대해 모든 branch를 직렬화한다. Original/fork branch는 보충 없이 계속 사용할 수 있다. Stale snapshot·checkpoint copy·imported JSON은 alias를 발급하거나 head를 되돌릴 수 없다. 원래 root30 → charge3 → original continuation6 → fork lower20 → continuation9 same-bank 증명은 변경하지 않은 test_graph_engine.cpp:810–913에서 PASSED했다. Saved original ceiling30은 effective fork ceiling20과 별개이며 widening31과 JSON-only restore는 거부해야 한다. Unbounded reported observation은 사실 data이지 finite grant가 아니다. 입증된 zero-effect lease만 unchanged head를 release할 수 있고 unknown/pending effect는 obligation을 유지한다.

**현재 release-error 계약; 실제 suite/probe는 아래.** `<neograph/graph/engine.h>`의 `graph::ManagedBudgetLeaseReleaseError`는 `ProviderOutcomeError`를 상속한다. `cause()`는 원래 execution exception을 보존하고 `release_error()`는 보조 durable lease-disposition 실패를 노출한다. `outcome()`은 실제 SDK 증거가 있으면 보존하고 SDK outcome이 없으면 null이다. Release 실패는 결과를 만들거나 재dispatch를 허용할 수 없다. Closed `_neograph_managed_budget_scope` metadata는 원래 logical scope/cap/deadline clock/generation을 설명하지만 backend CAS 권한이 아닌 data이다.

**Archive-owner/retention 계약; 실제 suite/probe는 아래.** Finite standalone root 또는 authenticated finite source만 실제 설정된 `sp::NativeArchive::owner_scope()`에서 생략된 original owner를 상속한다. Unbounded/plain owner metadata 의미는 바뀌지 않는다. 명시적으로 충돌하는 archive owner는 lease acquire 전에 거부한다. `CheckpointStore::retains_native_checkpoint() const noexcept`와 대응 Core/Async storage capability는 기본 false이며 실제 InMemory backend만 true로 override하고 wrapper는 실제 retention을 위임해야 한다. 이 read-only 설명은 정당한 unleased/plain/unbounded C++ native checkpoint custody를 허용하되 spending credit이나 native replay authority를 주지 않는다. Leased custody는 JSON flag나 추측한 store type 대신 실제 store-issued receipt를 사용한다.

**Native-custody pre-I/O gate; 실제 suite/probe는 아래.** Managed effect begin은 pending-effect/slot/held-window 변경 전에 실제 결합된 NativeArchive 또는 실제 local store-issued private C++ retention capability를 요구한다. Private capability는 JSON에서 import하거나 wire로 전달하지 않는다. C++ sidecar는 경계를 넘을 수 없으므로 remote backend가 InMemory여도 gRPC는 실제 client·server archive를 요구한다. Archive가 finite source owner를 제공하지 않으면 원래 anonymous owner scope는 빈 값으로 유지하고 실제 archive binding은 original scope와 일치해야 한다. Financial head/lease 증거만으로 native-custody readiness를 증명하지 않는다.

진단 JSON은 문법상 유효한 duplicate-key 문서를 포함해 원래 raw byte를 보존하지만 실행 가능한 요청/config admission은 중복을 계속 거부한다. 원래 non-2xx 응답 JSON은 두 번째 손실 parse 없이 `http.error` 증거에 남는다. named SSE error는 뒤의 정상 stream close보다 우선한다. 진단/provider metadata는 무관한 작은 error-text cap이 아니라 승인된 source extent로 제한된다.

`ProviderRequest::observer_limits`는 host-only이다. 명시한 `max_events`·`max_bytes`는 양수여야 하며 승인된 SDK 전달 상한을 낮출 수만 있다. `provider-request/v3` digest는 실제 limit, mode, encoded body, retry policy와 모든 semantic descriptor binding을 결합한다. Bridge는 queued·draining batch 전체에서 실제 PMR vector/map capacity와 소유 event/document byte를 계산하며 queue mutex 밖에서 cancellation을 요청한다. 이름이 `messages`인 Generic channel을 chat으로 강제 변환하지 않는다. native `history` channel을 `messages`에 mapping하면 JSON에서 native 권한을 만드는 대신 C++ sidecar를 보존한다.

`ProviderOutcomeError`는 결과를 보존하는 공통 host-error base이다. `ProviderObserverError`와 `ProviderDispatchOutcomePersistenceError`는 완전히 drain된 SDK 결과와 원래 `cause()`를 보존하며 후자는 보조 observer 실패도 `delivery_error()`에 보존한다. `ProviderFailure::outcome()`은 SDK 실패 자체를 보존한다. 이 증거는 Node/Program 재dispatch 권한이 아니다. Provider retry의 유일한 소유자는 SDK이며 caller가 선택한 `max_output_tokens`를 조용히 clamp하지 않는다.

`ProgramFailure`는 live `provider_outcome`·`provider_cause`를 보존한다. Canonical factual SDK witness는 실제 archive custody를 owner/run/version/bundle/operation/attempt에 결합하며 Runtime은 복구 실패를 노출하기 전에 설정된 custody를 즉시 복원한다. 공개 data-only `ProgramResult::create()`는 미리 채운 witness로 우회할 수 없고 unresolved parsed seal은 실행 결과가 아니다. 프로세스 재시작 후 원래 exception pointer는 없으므로 `provider_cause == nullptr`이며 text에서 재생성하지 않는다. 영속화할 수 없는 실패는 serialize/publish/replay할 수 없다.

Python `LocalProgramHost`는 소멸 시 `ProgramRuntime`이 스케줄러 작업을 취소하고 비운 뒤 join하는 동안 호출자의 GIL을 해제하여 실행 중인 Python 노드가 끝날 수 있게 한다. 나머지 host 멤버와 이들이 소유한 Python 콜백/객체를 소멸하기 전에 GIL을 다시 획득한다. 이 변경은 종료 처리에만 적용되며 추가 capability binder나 실행 권한을 공개하지 않는다.

`RecordedBindingSet`는 source-bound move-only data이지 caller가 제공하는 dispatcher가 아니다. 신뢰된 Catalog `recorded_capability_binder`는 실제 영속 source event를 독립적으로 읽어 captured-only capability를 materialize한다. `ProgramRuntime::replay_recorded()`는 원래 selected-source permission을 검사한 뒤 실제 남은 bank를 durable CAS로 이전한다. inherited spend는 새 model grant가 아니다. 구 `start_recorded` 갱신 API는 제거되었다. InMemory/File/SQLite/PostgreSQL Program store는 실행 내내 정확하고 불변인 owned lease를 보존하며 expiry로 갱신하지 않는다. Controlled JavaScript도 underlying capability manifest를 검사하고 정확한 completed command 결과를 소비하며 external effect를 재dispatch하지 않는다.

**Recorded-control causal fix는 full suite에서 실제 증명 완료.** Captured command replay는 실행 전에 새 CPU wall-time/Core work만 durable reserve하고 측정 work와 새 Core checkpoint를 result CAS로 publish한다. 새 model·money·Program-operation allowance를 소비하지 않고 captured external effect를 재dispatch하지 않는다. 미정산 reservation은 debit을 유지한다. Reservation은 첫 새 Core checkpoint를 거부했던 일반 Running→Running transition 대신 인증된 settlement transition을 선택한다. Await channel receive·timer wait/cancel·handoff wait 시작/release는 소유 executor/strand에서 직렬화한다. 기존 Recorded CPU/Memory await/handoff scenario는 full suite에서 pass했다. Remote TSan coverage 한계는 아래에 명시한다.

아래 관측은 이 문서 정리 이전에 기록되었다. 과거 증거이며 새 테스트 실행이나 모든 platform·transport·security 속성의 보장이 아니다.

**유료 관측 완료; 보편적 qualification은 아님.** 원래 `SPQUAL1` base630/1000000 microUSD는 불변이다. 같은 원래 ledger의 ONE hash-chained `A`가 승인 extension480/3000000을 받아 aggregate1110/4000000이 된다. Calls/spent/hold/settlement는 누적이며 새 grant ID/header/reset은 없다. 정확한 declaration byte/file identity와 original authorization/baseline/catalog/activation/ledger-prefix hash/totals는 고정되고 삭제·교체·변경은 fail closed한다. 최종 canonical ledger는 calls1110/spent437958/held1287828 microUSD, eventA1, limits1110/4000000이다. Spent+held US$1.725786은 LOCAL catalogue meter이지 invoice가 아니다. 기록된 five-family60-pair baseline은600 request를 완료했다: Chat60/60, Responses60/60, Messages60/60, Generate56/60(incorrect-vision SSE4개), Interactions57/60(incorrect-vision buffered1개/SSE2개). 합계293/300 pair이며300/300은 아니다. 다른 old600 financial record는 보존하되 완전한 behavioral proof는 아니다. 이전 M5/media one-shot cohort는 그대로다. 이전 Google3-round prerequisite의 invalid-tool2개/unreadable-positive1개 실패 상태를 유지한다. 추가 유료 호출은 승인되지 않는다. 최종 SDK 증거와 native-axis 한계는 baseline 성공과 별개다. 이전 activation/reopen smoke는 두 번 reopen한 calls610/spent219159/held751233 및 SDK meter/canary/vision4-test19.38초 pass로 보존한다. 이는 범위가 정해진 이전 checkpoint이지 최종 ledger totals가 아니다. 이전 검증된 Chat60-pair cohort의 실제 attempt120,UpperBound charge120,UnknownHold 없음도 보존한다.

**Native-axis 관측은 cryptographic 검증·native consumption/equivalence가 아니다.** Generate는 mutation/omission/duplication을 받아들였다. Interactions는 isolated genuine source/positive control, one-owner signature mutation, thought-carrier omission, call-carrier omission, duplication을 받아들였다. 모든 thought/signature 제거는 generic400을 반환했고 THOUGHT item을 유지한 채 모든 signature field를 제거해도 generic400이었다. 마지막 capture에는 local encoded-original retention control만 있고 same-capture server positive는 없었다. 이전 positive cohort는 실제 증거다. 이는 aggregate-carrier-absence boundary만 입증하며 issuer/signature 검증이나 vendor consumption을 입증하지 않는다. 실제 report: SDK `config/qualification-extension-results.json`, `qualification-final-summary.json`, `qualification-native-axis-results.json`, `qualification-combined-omission-results.json`, `qualification-signature-presence-results.json`. Prerequisite-failed/not-run/negative-inconclusive 상태는 사실 그대로 유지한다. Thought-only/carrier-only omission은 다른 carrier가 남아 있는 상태에서 받아들여졌다. Issuer-validation/native-consumption 주장을 강화하지 않는다.

**실제 통합 증명과 남은 한계.** 최신 Core full run:2242 test, 실패0,skip16(RAM process-loss 비적용14개/live-credential gate2개),130.17초. `PgNestedJsonRoundTrips`는 duplicate key/order/null metadata,blob,residual을 정확히 보존하며0.18초 pass했다. 변경하지 않은 원래 shared-bank fork와 기존 Recorded CPU/Memory await/handoff scenario가 pass했다. 실제 wrappedMemory/SQLite/PostgreSQL/gRPC finite130/hold65/lower129/strip/old-head/pruning/no-archive/import probe는 plain과 ASan+UBSan에서 pass했다. LOCAL Memory/SQLite/PostgreSQL TSan scope는7개 pass,warning0이다. System Abseil/Protobuf를 포함한 full mixed gRPC TSan은 exit66,dependency/generated-RPC stack에 race warning402개였다. 이는 instrumentation/coverage 한계이지 proven false positive가 아니다. Remote TSan/race-free를 주장하지 않으며 warning을 suppress하지 않는다. Installed find_package Program C++/C ABI/dualQuickJS3 consumer는 pass했다. Fresh installed NeoGraph/SchemaProvider typed consumer는 실제 HTTP request2개,coroutine 시작 전 provider 소멸,native/tool replay,refusal,known-zero/raw 보존,실제 LinkedMismatch 거부를 pass했다. Browser Alice/Bob isolation·generation2 replacement를 실제 시각 검증했고 PostgreSQL Program Chat black-box6개는18.989초 pass했다. 최신 SDK26/26은 실패0,74.07초 pass했다. 최종 ReleaseGraph16설정 ×fresh process3회/48기록은38.29초,실패0,모든 actual protocol/owned-outcome check pass로 완료했다. NeoGraph `benchmarks/provider-cutover-final-results.json`과 `benchmarks/provider-cutover-final-summary.json`은 별도의 최종 cohort를 보존한다. 측정 중 compiler/유료 model은 실행하지 않았고 historical cohort는 그대로이며 semantic/resource equivalence를 주장하지 않는다. Unstable SDK/ABI3는 stable release나 더 넓은 platform qualification이 아니다.

**최소 유료 증거(2026-10-03)는 광범위 qualification이 아니다.** 별도로 승인한 one-shot 세 호출 결과: Images—JPEG 1개, 1024×1024, 360685 byte, input/output/total token 19/1408/1427, 실제 시각 검사; Veo—MP4 1개, 1280×720, 4초, 437737 byte, generation 1회와 status GET 3회, usage nullable, Chromium decode·시각 검사; Decisions—`typesafe/jev-1.13`, probability 0.93, input/output token 283/21, total 미상, API 보고 비용 USD 0.000011886. Image USD 0.0336 base + text/thinking, Veo USD 0.20은 catalog 예상이지 invoice가 아니다. 최소 image smoke에서는 가격대별 내역을 수집하지 않았다. 결과는 one-shot 권한을 갱신하거나 재실행을 승인하지 않는다.

완료된 chat pair는 downstream vendor의 native-continuation 소비를 입증하지 않는다.

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
};
```

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

#### Interface 4 typed controls

`make_provider_request(provider, model, messages, tools, controls, mode)`는 닫힌 family 하나를 선택한다. 다른 family의 제어는 `std::invalid_argument`로 거부하고 SDK prepare가 enum 값, 승인 origin, 모델 규칙, 도구 선언, native binding을 I/O 전에 검사한다. `ProviderControls`에 dictionary 우회 경로는 없다. optional 값은 미지정과 명시적 `false`·빈 선택을 구분한다.

| Family | `ProviderControls` 필드와 SDK 매핑 |
|---|---|
| `openai.chat` | `max_output_tokens`, `temperature`, `top_p`, `reasoning_effort`, `service_tier`, `provider`, `response_format`; `chat_reasoning` → `sp::chat::Request::reasoning`, `include_reasoning`, `usage_include` → `usage.include`, `models` |
| `openai.responses` | `max_output_tokens`, `max_tool_calls`, `temperature`, `top_p`, `reasoning_effort`/`reasoning_summary` → `reasoning`, `service_tier`, `required_tool`, `provider`, `response_format`, `store`, `system` → `instructions`, `account_scope`; `previous_response_id`, `previous_response_history`, `parallel_tool_calls`, `verbosity` → `text.verbosity`, `truncation`, `responses_include` → `include` |
| `anthropic.messages` | `max_output_tokens` → `max_tokens`, `temperature`, `top_p`, `thinking_budget`, `system`, `account_scope`, `provider`; `thinking_mode`, `output_effort` → `output_config.effort`, `cache_control`, `messages_tool_choice` → `tool_choice` |
| `google.generate` | `max_output_tokens`, `temperature`, `thinking_budget`, `include_thoughts`, `required_tool`, `system`, `account_scope`; `gemini_history_mode` → `history_mode`, `gemini_thinking_level` → `thinking_level`, `safety_settings`, `gemini_tool_choice` → `tool_choice` |
| `google.interactions` | `max_output_tokens`, `thinking_level`(optional string), `thinking_summaries`, `service_tier`, `required_tool`, `system`, `account_scope`; Generate의 enum `gemini_thinking_level`은 적용되지 않는다 |

Chat의 `sp::chat::ReasoningOptions`는 optional `effort`, `max_tokens`, `exclude`, `enabled`를 담는다. 이 nested reasoning object, `include_reasoning`, `usage_include`, 대체 `models`는 policy가 선언한 OpenRouter origin에서만 허용된다. `sp::OpenRouterRouting`도 Chat/Responses/Messages의 선언된 OpenRouter origin에서만 지원한다. gateway 형태 모델 이름이 다른 origin을 승인하지 않는다. SDK payload는 family별 typed tool 선언, Responses `hosted_tools`, strict/deferred tool 옵션도 제공한다. raw JSON 대신 실제 payload variant를 사용한다.

Responses SSE는 종료 이벤트 `response.completed` 또는 `response.incomplete`를 요구합니다. `response.done`은 같은 완전한 종료 Responses envelope(completed 또는 incomplete)를 담은 별칭일 때만 허용되며, 종료가 아니거나 형식이 잘못된 `response.done`은 프로토콜 오류로 실패합니다. `response.content_part.delta`를 비롯해 목록에 없는 이벤트는 `Unsupported`입니다. 선언된 OpenRouter origin에서는 Responses와 Messages가 `~vendor/model-latest` 같은 라우팅 별칭에 대해 게이트웨이가 보고하는 실제 모델명도 받아들입니다. 서빙 모델은 한 응답 안에서 바뀌면 안 되고, replay는 요청한 모델에 묶인 채로 유지됩니다.

| SDK 타입 | 닫힌 값 또는 멤버 |
|---|---|
| `sp::responses::Verbosity` | `Low`, `Medium`, `High` |
| `sp::responses::Truncation` | `Disabled`, `Auto` |
| `sp::responses::Include` | `ReasoningEncryptedContent`, `WebSearchSources`, `FileSearchResults`, `MessageOutputTextLogprobs`, `ComputerCallOutputImageUrl`, `CodeInterpreterCallOutputs` |
| `sp::messages::ThinkingMode` | `Manual`, `Adaptive`, `Disabled` |
| `sp::messages::OutputEffort` | `Low`, `Medium`, `High`, `Max` |
| `sp::messages::CacheControl` / `CacheTtl` | optional `ttl`: `FiveMinutes`, `OneHour`; wire type `ephemeral`, optional TTL `5m`/`1h` |
| `sp::messages::ToolChoice` / `ToolChoiceMode` | `mode`: `Auto`, `Any`, `None`, `Tool`; `name`; optional `disable_parallel_tool_use` |
| `sp::gemini::HistoryMode` | `NativeOnly`, `PortableForeign` |
| `sp::gemini::ThinkingLevel` | `Minimal`, `Low`, `Medium`, `High` |
| `sp::gemini::SafetySetting` / `SafetyCategory` | `category`: `Harassment`, `HateSpeech`, `SexuallyExplicit`, `DangerousContent`, `CivicIntegrity`; `threshold`: `SafetyThreshold` |
| `sp::gemini::SafetyThreshold` | `BlockNone`, `BlockOnlyHigh`, `BlockMediumAndAbove`, `BlockLowAndAbove`, `Off` |
| `sp::gemini::ToolChoice` / `ToolChoiceMode` | `mode`: `Auto`, `Any`, `None`, `Validated`; `allowed_function_names`: 선언된 함수 이름 vector |

Messages는 양수 output cap이 필요하다. mode 없이 thinking budget을 주면 `Manual`이며 승인 minimum 이상, cap 미만이어야 한다. `Adaptive`/`Disabled`는 명시적 budget을 거부한다. Manual/adaptive는 그 외 유효한 temperature를 생략하고 thinking `top_p` minimum을 검사하지만 모델별 temperature 금지는 무시하지 않는다. 강제 `Any`/`Tool`은 도구와 disabled thinking이 필요하다. `Tool` 이름은 선언된 client tool이어야 하고 다른 mode는 `name`을 거부하며 `None`은 `disable_parallel_tool_use`를 거부한다. Generate의 thinking budget/level과 `required_tool`/`gemini_tool_choice`는 각각 동시 사용 불가다. allowed-function 목록은 선언된 함수를 가리킨다. sampling은 family/policy 범위를 따른다. Generate에는 `top_p`가 없고 Interactions에는 두 sampling 필드 모두 없다.

`FamilyPolicy.temperature_forbidden_model_prefixes`는 전체 모델과 마지막 `/` 뒤 suffix를 ASCII 대소문자 무시로 비교한다. 내장 Chat/Responses prefix는 `gpt-5`, `gpt-6`, `o1`, `o3`, `o4`; Messages는 `claude-opus-4-7`, `claude-opus-4-8`, `claude-opus-5`, `claude-sonnet-5`, `claude-fable-`다. 명시적 금지 temperature는 gateway prefix 모델에서도 I/O 전에 거부한다.

#### Responses provider-held continuation

`previous_response_id`는 provider 보관 상태를 선택한다. 요청 `messages`는 NEW INPUT ONLY이며 과거 대화 전체를 다시 보내지 않는다. `previous_response_history`는 전송되지 않는 로컬 소유 증거 `std::vector<sp::Message>`다. cursor만으로 새 text/image 입력은 허용할 수 있지만 client tool result를 승인하지 못한다. 최초 captured response에는 authentic original prefix와 ID가 cursor인 terminal assistant response를 함께 준다. 이후 authentic in-process cursor-produced terminal response는 전체 prefix 재구성 없이 private completed tool ownership을 가질 수 있다. content/origin/model/route/config 불일치는 거부하며 임의 ID나 projection JSON으로 ownership을 만들 수 없다.

server cursor와 private completed ownership은 `NativeReplay`·native archive 권한을 주지 않는다. `responses_include` 미지정은 `reasoning.encrypted_content`를 유지하고 명시적 빈 vector는 `[]`를 보낸다. 다른 명시적 선택도 그대로 적용한다. 이후 작업이 native evidence를 요구하면 근거 누락은 정직하게 실패한다.

#### Explicit portable Gemini history

Generate 기본은 `NativeOnly`다. 명시적 `PortableForeign`은 native seal, `wire_output`, signature/native metadata가 없는 caller-created assistant `Text`/`ToolCall`만 허용한다. 최초 foreign `functionCall`에만 Google의 `skip_thought_signature_validator`를 붙이고 text-only turn에는 signature를 만들지 않는다. authentic native group은 원래 provenance/content/binding 검증을 유지한다. 손상·불일치 native group을 portable로 강등하지 않으며 imported foreign history는 native replay 권한을 얻지 않는다.

#### Output caps and native continuation

output generation cap은 호출별 승인 resource이며 native replay config digest에서 이 cap만 제외한다. content/prefix, origin/route, policy identity, tools, reasoning controls 등 다른 binding은 유지한다(문서화된 per-turn tool selection/cursor 동작 제외). 각 encoded/prepared request는 effective cap을 보존하며 request digest, journal slot, 원래 shared bank와 절대 deadline은 계속 구속한다. cap을 높인 semantic call은 같은 grant 아래 새 call ordinal·admission이 필요하다. seal repair, deadline 갱신, 이전 effect replay가 아니다. native archive는 `spna3`/v3, portable JSON은 v2다.

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

선택적 `ProviderControls`는 호출자 선택이며 강제 기본값이나 몰래 clamp한 cap이 아니다. family가 지원하지 않는 제어는 dispatch 전에 거부한다. 유한 예산 호출에는 승인된 실제 모델 input/output 한계가 필요하며 없으면 `LimitUnknown`으로 실패한다. 예약은 보수적인 지출 권한이지 보고 사용량·예측·청구서가 아니다. 미상/부분/delivery-unknown 결과는 hold를 유지하고 실제 최종 보고로 정산하며 초과 보고도 전부 청구한다. 재시도는 단일 명시적 계층이며 기본 off, 유한 window와 unknown-prior hold를 사용한다. 숨은 재전송은 없다.

`provider_failure_proves_not_sent(Failure)`는 완전하고 모순 없는 NotSent 근거를 요구한다. 상태 코드·누락 사용량·관측자/영속 예외만으로 비용 0이나 예산 갱신을 증명하지 않는다.

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


소스 및 바이너리 단절이다. 모든 C++ 소비자와 사용자 공급자를 새 헤더/라이브러리로 재컴파일한다. `CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, descriptor interpreter와 Responses WebSocket은 alias/호환 bridge 없이 제거되었다. SDK는 alpha `0.1.1`, interface revision 4 / shared-library generation 4이며 out-of-line capability check를 사용한다. 안정 릴리스 선언이 아니다. 기록된 interface-3 SDK runtime/archive 검증은 Linux/POSIX의 과거 증거이지 interface-4 pass가 아니다. Windows NTFS와 macOS 구현이 있으나 새 platform 검증에는 runtime 증거가 필요하며 WASM provider runtime 검증은 입증되지 않았다.

Python도 C++과 같은 소유 request/outcome 경계를 제공한다: `make_provider_request`, `Provider.prepare`, `dispatch`, `invoke`. Provider 기록에는 typed part를 가진 `ProviderMessage`를 사용하며 `ChatMessage`는 그래프 편의 projection으로 남는다. SDK 실패는 `ProviderOutcome.failure`로 읽고 host observer/settlement 예외는 `outcome`과 `cause`를 보존한다. 생성자와 GIL/콜백 동작은 [Python binding 안내](python-binding.md)를 참조한다.

Python SDK의 vector/map getter는 분리된 값을 반환한다: `ProviderMessage.parts`, `ProviderRequest.messages`, `RunConfig.provider_messages`, completion/partial의 `messages`와 `raw_events`, `RunResult.native_messages`, `ProviderLoopEntry.messages`, usage의 `extra`/`conflicts`. 스냅샷을 수정한 뒤 setter가 있는 속성에 다시 대입한다. getter 결과에 append해도 소유 객체는 바뀌지 않는다. 선택적 `ProviderControls.provider`/`response_format`, `SchemaProviderDefaults.provider`, `ProviderToolResult.host`도 같은 읽기·수정·대입 규칙을 따른다. 읽기 전용 outcome 증거는 바뀌지 않는다. 이는 Python 바인딩의 규칙이며 모든 C++ getter가 복사본을 반환한다는 보장이 아니다.

네이티브 코드가 Python provider의 `prepare` override를 호출할 때 `None`을 반환하면 handle을 소비하기 전에 `TypeError`가 발생한다. `ProviderDescriptorPolicy.identity`는 raw SHA-256 digest를 담은 `bytes`이며 `.hex()`는 표시용 변환이다. 저장된 Python provider/graph cause를 반복해서 읽어도 원래 예외 값과 traceback을 유지하고 저장된 예외의 restore 상태를 소비하지 않는다. 중첩된 네이티브 예외 변환에도 같은 규칙이 적용된다.

이전에 기록한 installed find_package Program C++/C ABI/dualQuickJS consumer와 NeoGraph/SchemaProvider typed2-request lifetime/native/raw/mismatch consumer는 당시 snapshot에서 pass했다. 이 결과가 interface-4 package 검증을 입증하지는 않는다. 선언 일치만으로 새 runtime 결과나 더 넓은 platform 지원을 증명하지 않는다.

현재 SDK4 Linux x86_64 증거는 등록된 27 case를 모두 다룬다. 최초 full run에서 25개가 pass했고 obsolete assertion 두 개를 고친 뒤 `native_archive`와 `stop_reasoning_preservation`이 focused 2/2로 pass했다. 두 번째 full-suite 27/27 실행은 아니다. 변경 없는 buffered/SSE Stop probe와 설치 SDK의 exact README consumer도 pass했다. 후자는 zero-usage, unknown-usage, HTTP-400-failure variant마다 credential 없는 요청 하나를 실행했다. 이 SDK 결과는 새 NeoGraph native build/wheel pass나 Windows/macOS/ARM64/HTTP3 검증을 입증하지 않는다.

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
    uint64_t    version = 0;                       // Write counter
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

`resume_value` also arrives as a user turn on a `messages` channel when the
graph has one, which is how chat-shaped graphs have always received it.
`ctx.resume_value` is the general path — it works whatever the graph's channels
are called.

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
| `NODE_END` | Channel writes produced by the node |
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
| `channel_version(channel)` | Returns the write counter for a specific channel |
| `global_version()` | Returns the global version counter (incremented on every write to any channel) |
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
| `ctx` | `NodeContext` | Provider and model for the classification LLM call |
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

### RunContext (v0.4 PR 1, exposed to nodes via `NodeInput.ctx`)

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
| `fork()` | **v0.4 PR 3.** Returns a child shared_ptr. Parent.cancel() cascades; if the parent is already cancelled at fork() time the child is constructed pre-cancelled (no emit-vs-bind race) |
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
    RunStatus status() const noexcept;            // Completed, Interrupted, StepLimit, or SafePoint

    template <typename T> T channel(const std::string& name) const;
    template <typename T> T channel(const ChannelKey<T>& key) const;
    template <typename T>
    std::optional<T> try_channel(const ChannelKey<T>& key) const;
};
```

`RunResult::usage`는 nullable provider 보고이며 지출 bank가 아니다. `native_messages`는 실제 typed 기록을, `provider_outcomes`는 각 소유 Completion/Failure를 보존한다. JSON `output`은 portable projection이다. 전체 입력 기록은 `RunConfig::provider_messages`, typed 이벤트는 `on_provider_event`를 사용한다. 영속 native checkpoint/receipt custody에는 `native_history_archive`가 필요하지만 메모리 sidecar에는 필요하지 않다.

resume이나 continuation에서 `provider_outcomes`는 원래 결과 뒤에 새로 생성된 결과를 이어 순서 있는 목록으로 보존한다. `usage`에도 복원한 bank의 이전 보고가 남을 수 있다. 소비자는 완료된 provider effect를 재dispatch하거나 이중 차감하지 않아야 한다. 비어 있거나 초기화된 결과 목록, `usage=None`은 resume 불변 조건이 아니다.
| Field | Type | Description |
|-------|------|-------------|
| `output` | `json` | 모든 채널의 직렬화된 최종 상태 |
| `interrupted` | `bool` | 인터럽트(HITL)로 실행이 일시 중지되었으면 `true` |
| `interrupt_node` | `std::string` | 인터럽트를 발생시킨 노드 이름 |
| `interrupt_value` | `json` | 인터럽트의 이유 또는 페이로드 |
| `checkpoint_id` | `std::string` | 마지막으로 저장된 체크포인트의 UUID |
| `execution_trace` | `std::vector<std::string>` | 실행 순서대로 기록된 노드 이름 목록 |

`provider_messages`는 전체 typed 기록을 제공하고 messages 채널만 대체한다. `on_provider_event`는 typed SDK 이벤트를 관측한다. `provider_outcomes`, `provider_loop_history`는 결과와 task-local continuation을 inner turn 사이에 보존한다. `native_history_archive`는 영속 native custody를 결합하며 모델 예산을 주지 않는다. 선택적 `model_token_budget` 상한과 `budget_exhausted` 신호는 예산 인식 dispatch에 사용한다. Python과 C++은 모두 `RunResult.native_messages`로 전체 기록을, `provider_outcomes`로 소유 결과를 반환한다. 입력은 계속 `RunConfig.provider_messages`이며 `RunResult.provider_messages`나 `provider_history` alias는 없다.

`max_steps_exhausted()` returns `true` only when the step ceiling stopped the
run while runnable work remained. A graph that reaches `__end__` exactly on its
last permitted step returns `false`.

`status()` returns `RunStatus::Completed`, `RunStatus::Interrupted`,
`RunStatus::StepLimit`, or `RunStatus::SafePoint` without changing the public `RunResult` data layout.
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
    // while a run is in flight (Round 3 guard — `active_runs_`
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

Returns the checkpoint history for a thread, ordered by timestamp (newest first).

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
- `run_sends_async`: single Send runs on the shared state with retry;
  multi Send gives each target an isolated state copy (init + restore
  + apply input) without retry — preserves pre-3.0 semantics.
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
    json        channel_versions;  // Per-channel version counters
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
| `channel_versions` | `json` | Version counter for each channel |
| `parent_id` | `std::string` | ID of the preceding checkpoint (forms a linked list for time-travel) |
| `current_node` | `std::string` | Node that was executing when the checkpoint was taken |
| `next_nodes` | `std::vector<std::string>` | All nodes scheduled for the next super-step (used by `resume()`). Under signal dispatch a super-step can leave several nodes simultaneously ready (parallel fan-out, conditional branches activating together), and every one of them must be persisted — storing a single node would silently drop siblings across a crash |
| `interrupt_phase` | `CheckpointPhase` | Enum: `Before` (interrupt_before fired), `After` (interrupt_after fired), `Completed` (normal super-step cadence), `NodeInterrupt` (node threw `NodeInterrupt` mid-execution), `Updated` (external `update_state()` injection). `to_string()` and `parse_checkpoint_phase()` give a stable wire/log encoding |
| `barrier_state` | `map<string, set<string>>` | Per-barrier accumulator of upstreams that have signaled so far. Entries only exist for barriers that are in-flight (not yet fired) — the Scheduler clears an entry when its barrier fires. Shape matches `BarrierState` from `scheduler.h`. Present since schema v2; v1 blobs deserialize with an empty map, which matches their pre-v2 behavior |
| `metadata` | `json` | Arbitrary user-defined data |
| `step` | `int64_t` | Super-step counter |
| `timestamp` | `int64_t` | Creation time in Unix epoch milliseconds |
| `schema_version` | `std::uint32_t` | On-wire layout version (see `CHECKPOINT_SCHEMA_VERSION`, currently `3`). Round 5 widened this from `int` to fixed-width unsigned — schema versions are non-negative and a platform-variable `int` width was wrong for a value persisted to disk and round-tripped through JSON. Persistent `CheckpointStore` implementations should serialize it and treat `0` on a deserialized blob as "pre-versioned" (e.g. the field was absent — migration is the caller's responsibility) |

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

| Type | Class | Description |
|------|-------|-------------|
| `"llm_call"` | `LLMCallNode` | Calls the LLM with current conversation state |
| `"tool_dispatch"` | `ToolDispatchNode` | Dispatches tool calls from the latest assistant message |
| `"intent_classifier"` | `IntentClassifierNode` | LLM-based intent classification. Reads `prompt` and `valid_routes` from `config` |
| `"subgraph"` | `SubgraphNode` | Runs a compiled subgraph. Reads `input_map` and `output_map` from `config` |

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

Model Context Protocol (MCP) client implementation. Connects to MCP servers, discovers
available tools, and wraps them as NeoGraph `Tool` instances.

Two transports are available:

- **HTTP** — `MCPClient("http://host:port")`. Discovered tools retain the
  originating Streamable HTTP session, including `Mcp-Session-Id`, negotiated
  protocol version, timeout, and custom headers.
- **stdio** — `MCPClient({"python", "server.py"})`. The client resolves `PATH`
  before `fork`, executes with `execve`, wires bidirectional pipes, and exchanges newline-delimited JSON-RPC
  over the child's stdin/stdout. The subprocess lives as long as the
  `MCPClient` *or any `MCPTool`* it produced; destruction sends SIGTERM and
  reaps via `waitpid` (SIGKILL fallback after ~500 ms).

### MCPTool

Wraps a single MCP server tool as a local `Tool` implementation. Discovered
tools retain their originating protocol session, regardless of transport.

```cpp
class MCPTool : public AsyncTool {
public:
    // Legacy direct-construction mode. Discovered tools reuse their client session.
    MCPTool(const std::string& server_url,
            const std::string& name,
            const std::string& description,
            const json& input_schema);

    const ToolDefinition& get_mcp_definition() const noexcept;
    CallToolResult execute_result(const json& arguments);
    asio::awaitable<CallToolResult> execute_result_async(const json& arguments);
    ChatTool get_definition() const override;
    asio::awaitable<std::string> execute_async(const json& arguments) override;
    std::string get_name() const override;
};
```

Usually you do not construct `MCPTool` directly — `MCPClient::get_tools()`
discovers and wraps them.

### MCPClient

Client that connects to an MCP server, performs the initialization handshake, and
provides methods to discover and invoke tools.

> `MCPClient` is not designed to be subclassed — you use it as-is.
> `rpc_call_async()` is the real implementation and `rpc_call()` is
> a thin sync facade. See [`ASYNC_GUIDE.md` §9.5](ASYNC_GUIDE.md#95-mcpclient).

```cpp
class MCPClient {
public:
    // HTTP transport.
    explicit MCPClient(const std::string& server_url);
    MCPClient(const std::string& server_url, MCPClientConfig config);

    // stdio transport — fork+exec the subprocess.
    explicit MCPClient(std::vector<std::string> argv);

    bool initialize(const std::string& client_name = "neograph");
    bool is_initialized() const noexcept;
    InitializeResult get_initialize_result() const;
    std::vector<std::unique_ptr<Tool>> get_tools();
    ListToolsPage list_tools(std::optional<std::string> cursor = std::nullopt);
    std::vector<ToolDefinition> get_tool_definitions();
    json call_tool(const std::string& name, const json& arguments);
    CallToolResult call_tool_result(const std::string& name,
                                    const json& arguments);

    // Low-level async dispatch retained for source compatibility.
    asio::awaitable<json>
    rpc_call_async(const std::string& method, const json& params);
};
```

**Wire protocol:** NeoGraph's MCP client speaks
`protocolVersion = "2025-11-25"`. HTTP transport sends the
`MCP-Protocol-Version` header on every JSON-RPC request (Round 1 +
Round 3 spec alignment); stdio transport carries the same version
in the `initialize` payload. Servers running older protocol
versions may reject these requests — pin server-side or upgrade.

| Method | Description |
|--------|-------------|
| `MCPClient(url)` | Construct an HTTP-mode client |
| `MCPClient(argv)` | Spawn a subprocess and construct a stdio-mode client. `argv[0]` is resolved through `PATH` before fork; failed exec surfaces as a connection error on first RPC. Refuses Windows `.bat`/`.cmd` for safety (Round 3 hardening) |
| `initialize(client_name)` | Perform the MCP initialization handshake once. Repeated calls are idempotent; protocol/transport failures throw |
| `get_initialize_result()` | Return negotiated protocol, capabilities, server info, instructions, and raw result |
| `list_tools(cursor)` | Fetch one page while treating the cursor as opaque |
| `get_tool_definitions()` | Follow all pages and preserve complete tool metadata |
| `get_tools()` | Discover all pages and return session-preserving `MCPTool` instances |
| `call_tool(name, arguments)` | Invokes a tool by name with the given arguments. Returns the raw JSON response |
| `call_tool_result(name, arguments)` | Typed result preserving content, structured content, `isError`, and `_meta` |
| `rpc_call_async(method, params)` | Coroutine version. The "real" implementation — `rpc_call` is a thin sync wrapper. |

**HTTP usage:**

```cpp
neograph::mcp::MCPClient client("http://localhost:8000");
client.initialize();
auto tools = client.get_tools();
```

**stdio usage:**

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
`A2AClient`와 `GraphAgentAdapter`는 `WireDialect::{V0_3,V1_0}`의 JSON-RPC 2.0 HTTP/SSE를 구현한다. `AgentCard.supported_interfaces`는 현대/legacy card에서 파싱한 순서 있는 `AgentInterface{url, protocol_binding, protocol_version, tenant}` 관측이며 raw card도 보존한다. 선택은 lazy: compatible JSONRPC 0.x/1.x 중 첫 interface, 설정 base URL과 trailing-slash 정규화 후 일치하는 것을 우선한다. card URL로 RPC endpoint를 redirect하지 않는다. 선택 tenant는 send/get/cancel/stream에 적용한다. `wire_dialect()`는 선택·성공 probe 전까지 비어 있고 force discovery가 초기화한다. fetched incompatible card는 다른 dialect probe 없이 거부한다.

card 없이는 0.3을 probe하고 숫자 JSON-RPC `-32601`에서만 전환하며 성공 dialect를 기억한다. method/body/header를 함께 바꾼다. V1은 PascalCase, `A2A-Version: 1.0`, `ROLE_*`/`TASK_STATE_*`, `kind` 없는 flat text/raw/url/data part와 `blocking`을 반전한 `returnImmediately`를 쓴다. task/message wrapper와 bare get/cancel task를 decode한다. server 응답 encoding은 method spelling이 아니라 version header로 결정한다. header 없음은 0.3, 미지원 major는 `-32009`; 기본 card는 둘 다 광고한다. bound discovery URL은 restart를 보존하며 explicit endpoint를 대체하지 않는다.

SSE는 LF/CRLF/CR, comment, multiline data, 마지막 unterminated data를 처리한다. opening task, status, artifact append/replace를 누적해 Task를 반환한다. V1은 opening submitted task와 artifact 뒤 terminal status를 보내며 legacy `kind`/`final`/trailing task가 필요 없다. event를 관측하면 external callback이 없어도 dialect redispatch를 금지한다. non-SSE RPC error는 `A2ARpcError::code()`를 보존하고 non-2xx HTTP를 성공 task로 바꾸지 않는다.

caller node 답 우선순위는 final/interrupted agent status text, 첫 artifact text, 마지막 agent history text다. progress status나 user history는 답을 덮지 않는다. 일반 `async_post_stream`은 nonempty fixed-`Content-Length` body를 status와 함께 한 번 전달하고 zero length는 chunk를 내지 않는다. body limit/early EOF/이미 buffer된 surplus는 거부한다. redirect/chunked/close-delimited 규칙은 유지한다.

Python `neograph_engine.a2a`는 `WireDialect`, `AgentInterface`, `AgentCard.supported_interfaces`/`raw`, `Part.media_type`, `MessageSendConfiguration`, `MessageSendParams`, `StreamEvent`와 status/artifact record를 제공한다. `A2AClient.wire_dialect()`는 enum 또는 `None`; `set_authorization_header()`는 실제 native setter; `send_message(params)`는 multipart overload다. `send_message_stream(text, on_event, task_id="", context_id="")` 또는 `(params, on_event)`는 누적 `Task`를 반환하고 bool callback에 owned event snapshot을 준다. blocking call은 GIL을 해제하며 callback owner는 안전하게 재획득한다. `a2a.A2ARpcError.code`는 remote 정수 code다. vector/optional child는 detached snapshot, `Task.status`와 `MessageSendParams.message`는 live inline field다. JSON 관측은 provider native 권한을 주지 않는다.

**공개 헤더:** [`include/neograph/a2a/`](../include/neograph/a2a/).

### `neograph::acp` — Agent Client Protocol

**Header:** `<neograph/acp/{server,types}.h>`
Editor↔agent JSON-RPC over newline-delimited JSON on stdio (Zed,
Gemini CLI, Neovim CodeCompanion). Bidirectional: client→agent
(`initialize`, `session/{new,prompt,cancel}`) and agent→client
(`fs/{read,write}_text_file`, `session/request_permission`) via
late-bound `ACPClient`. `ACPServer::handle_message` async-dispatches
prompts on a worker thread, capped at `max_inflight_prompts=32`
with per-session single-flight + `-32000` backpressure.

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

- 선택적 `ProviderControls`는 호출자 선택이며 강제 기본값이나 몰래 clamp한 cap이 아니다. family가 지원하지 않는 제어는 dispatch 전에 거부한다. 유한 예산 호출에는 승인된 실제 모델 input/output 한계가 필요하며 없으면 `LimitUnknown`으로 실패한다. 예약은 보수적인 지출 권한이지 보고 사용량·예측·청구서가 아니다. 미상/부분/delivery-unknown 결과는 hold를 유지하고 실제 최종 보고로 정산하며 초과 보고도 전부 청구한다. 재시도는 단일 명시적 계층이며 기본 off, 유한 window와 unknown-prior hold를 사용한다. 숨은 재전송은 없다.
- **`neograph::AsyncTool`** — `Tool` peer that exposes
  `execute_async(json)` for tools whose work is naturally
  coroutine-shaped (HTTP fetch, MCP call). Sync `execute()` is
  `final`-routed through `run_sync`.
- **`neograph::graph::NodeCache`** — per-node memoization, opt-in
  at construction via `EngineConfig::cached_nodes` (the setter remains a
  compatibility surface).
- **`neograph::graph::create_deep_research_graph`** —
  open_deep_research-style supervisor + sub-researcher fan-out,
  used by `examples/25_deep_research.cpp`. Round 2 audit added
  `BriefNode` LLM rewrite, `FinalReportNode` token-limit retry,
  `ClarifyNode` HITL gate.

If the type you need isn't in this tour, check `include/neograph/`
directly. Every public header carries its reference documentation.
