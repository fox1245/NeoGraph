<!-- neograph-i18n: source=docs/reference-en.md locale=ko source_sha256=976c9b048dbd0cb5fcef306e22f4155d5e496ae10e5ed4b846c8d8ab575023b2 -->
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

SchemaProvider는 `NEOGRAPH_BUILD_LLM=OFF`여도 필수 외부 C++ 의존성이다. Core도 소유 typed provider 계약을 공개한다. SDK runtime 패키지를 설치하고 설치 prefix를 `SCHEMAPROVIDER_PREFIX`로 지정한다. 아래 configure는 `-DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX"`를 사용한다. 또는 `-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=../SchemaProvider`로 checkout을 명시한다. 추측한 sibling checkout이나 구 bundled interpreter를 자동 선택하지 않는다. 현재 SDK runtime/archive는 Linux/POSIX이며 의존성 없음·OpenSSL 불필요·native Windows/macOS·WASM runtime을 약속하지 않는다.

SDK imported target이 `include/SchemaProvider` include root를 제공한다. 공개 예시는 recipe 전용 helper 없이 `<descriptor/descriptor.h>`, `<runtime/client.h>`, `<neograph/llm/schema_provider.h>`를 직접 사용한다.

```cmake
find_package(SchemaProvider CONFIG REQUIRED COMPONENTS runtime)
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
  - [json_path Utilities](#json_path-utilities)
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
    std::vector<std::string> image_urls; // base64 data URLs or HTTP URLs for Vision
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

공급자 호출은 `sp::runtime::Result`, 즉 `sp::Completion` 또는 `sp::Failure`를 담은 불변 소유 `std::shared_ptr<const sp::Outcome>`를 반환한다. 표시 텍스트만이 아니라 전체 결과를 보존한다. 순서 있는 메시지/파트, native continuation, 전체 wire envelope, 순서 있는 raw 관측, 중단 근거와 실제 시도 메타데이터는 호출 및 클라이언트 소멸 후에도 남는다. 사용량은 근거·단계·품질을 갖는 nullable `uint64_t`이며 누락은 0이 아니라 미상이다. 실패도 원래의 부분 결과를 보존한다. `ProviderFailure::outcome()`과 `ProviderObserverError::outcome()`은 실제 결과를 보존하며 후자의 `cause()`에는 관측자 예외가 남는다.

### Portable projections


실제 결과 이후 post-effect 정산이나 terminal receipt 영속화가 실패하면 `ProviderDispatchOutcomePersistenceError`의 `outcome()`은 원래 불변 결과를, `cause()`는 원래 영속 예외를 보존한다. 전달도 실패했으면 `delivery_error()`가 원래 관측자 예외를 보존한다. 영속화 성공 뒤 관측자 실패는 원래 예외를 그대로 다시 던진다. 미상/결과 없는 transport 실패는 결과를 조작하지 않는다.
`ChatMessage` / `ChatTool`과 JSON은 portable projection이지 native 권한이 아니다. Portable 포맷은 [`provider-message-v2`](../schemas/provider-message-v2.schema.json), [`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json)를 유지한다. 실제 C++ checkpoint sidecar는 메모리에서 native seal을 보존한다. 영속 native 기록에는 host-owned `sp::NativeArchive`가 필요하다. closed v3 / `spna3`는 독립 키를 쓰는 인증된 owner-private custody이며 archive v2는 업그레이드하거나 해석하지 않고 거부한다. 인증은 모든 semantic descriptor 선택(origin/path/header, policy, 요청 field mapping, usage path, stop mapping), owner와 정확한 custody binding을 결합한다. 암호화나 vendor-issuer 인증은 아니다. archive 본문·키·native blob·raw wire 관측을 공개하지 않는다. Archive는 증거 저장소이지 돈의 grant나 spending lease가 아니다. Program/external bank는 독립 journal 소유이며 snapshot 복사로 credit을 만들 수 없다.

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

`RecordedBindingSet`는 source-bound move-only data이지 caller가 제공하는 dispatcher가 아니다. 신뢰된 Catalog `recorded_capability_binder`는 실제 영속 source event를 독립적으로 읽어 captured-only capability를 materialize한다. `ProgramRuntime::replay_recorded()`는 원래 selected-source permission을 검사한 뒤 실제 남은 bank를 durable CAS로 이전한다. inherited spend는 새 model grant가 아니다. 구 `start_recorded` 갱신 API는 제거되었다. InMemory/File/SQLite/PostgreSQL Program store는 실행 내내 정확하고 불변인 owned lease를 보존하며 expiry로 갱신하지 않는다. Controlled JavaScript도 underlying capability manifest를 검사하고 정확한 completed command 결과를 소비하며 external effect를 재dispatch하지 않는다.

**Recorded-control causal fix는 full suite에서 실제 증명 완료.** Captured command replay는 실행 전에 새 CPU wall-time/Core work만 durable reserve하고 측정 work와 새 Core checkpoint를 result CAS로 publish한다. 새 model·money·Program-operation allowance를 소비하지 않고 captured external effect를 재dispatch하지 않는다. 미정산 reservation은 debit을 유지한다. Reservation은 첫 새 Core checkpoint를 거부했던 일반 Running→Running transition 대신 인증된 settlement transition을 선택한다. Await channel receive·timer wait/cancel·handoff wait 시작/release는 소유 executor/strand에서 직렬화한다. 기존 Recorded CPU/Memory await/handoff scenario는 full suite에서 pass했다. Remote TSan coverage 한계는 아래에 명시한다.

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

All fields use `value()` with empty-string defaults, making deserialization tolerant
of missing fields.

---

## 2. Provider Interface

공개 계약은 소유 typed 준비/dispatch이며 동기·비동기 가상 completion 쌍이 아니다. `ProviderRequest.payload`는 Chat, Messages, Responses, Gemini, Interactions의 SDK 요청 variant이다. `ProviderMode::Collect` / `Stream`은 관측자 유무와 독립적으로 전송을 선택한다. `on_event`는 빌린 typed `sp::Event` view를 받는다. 콜백 이후 필요한 데이터만 복사한다. raw JSON override나 portable projection을 통한 native 권한 가져오기는 허용되지 않는다.

```cpp
#include <neograph/provider.h>
#include <neograph/runtime_interposition_consumer.h>
#include <neograph/controlled_provider.h>

// Public operation signatures (the only virtual operation is prepare).
// ProviderRequest owns the SDK request variant, mode, options and observer.
// invoke[_async](request) = prepare once, then dispatch the same handle.
// dispatch[_async](prepared) returns sp::runtime::Result.
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


소스 및 바이너리 단절이다. 모든 C++ 소비자와 사용자 공급자를 새 헤더/라이브러리로 재컴파일한다. `CompletionParams`, `ChatCompletion`, `CompletionProvider`, `OpenAIProvider`, `RateLimitedProvider`, `SchemaPrimitiveRegistry`, descriptor interpreter와 Responses WebSocket은 alias/호환 bridge 없이 제거되었다. SDK는 불안정 `0.0.0`, interface revision 3 / shared ABI 3이며 out-of-line capability check를 사용한다. 안정 릴리스 선언이 아니다. 현재 runtime/archive는 Linux/POSIX이며 Windows·macOS·WASM runtime 검증을 뜻하지 않는다. Python provider binding/wrapper는 유예되었고 이 C++ 변경으로 포팅되지 않는다.

Fresh installed find_package Program C++/C ABI/dualQuickJS consumer와 NeoGraph/SchemaProvider typed2-request lifetime/native/raw/mismatch consumer가 pass했다. Interface/ABI 선언만과 실제 package 결과는 별개이며 더 넓은 platform이나 stable release를 주장하지 않는다.

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
    json        value;                             // Current value
    uint64_t    version = 0;                       // Write counter
};
```

### ChannelWrite

A single write operation targeting a named channel. Nodes return vectors of these.

```cpp
struct ChannelWrite {
    std::string channel;  // Target channel name
    json        value;    // Value to write (merged via the channel's reducer)
};
```

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
    ToolSet                  tools;      // Owned fixed collection of tools
    std::string               model;      // Model override (empty = provider default)
    std::string               instructions; // System prompt / instructions
    json                      extra_config; // Additional configuration (node-type-specific)
};
```

Set `NodeContext::tools = ToolSet(std::move(tools))`, or supply
`EngineResources::tools` when the context has no tools. Compilation and the
engine share ownership of the exact collection; reassigning the context cannot
invalidate an earlier engine. Factories may use `ctx.tools.view()` for temporary
raw lookup. Python and MCP tools use the same compile-time ownership contract.

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
                      const json& initial_value = json());

    json get(const std::string& channel) const;
    std::vector<ChatMessage> get_messages() const;

    void write(const std::string& channel, const json& value);
    void apply_writes(const std::vector<ChannelWrite>& writes);

    uint64_t channel_version(const std::string& channel) const;
    uint64_t global_version() const;

    json serialize() const;
    void restore(const json& data);

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
| `serialize()` | Serializes all channel values and versions to JSON (for checkpointing) |
| `restore(data)` | Restores channel values and versions from serialized JSON |
| `channel_names()` | Returns the names of all initialized channels |

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
| `in.ctx.cancel_token` | Pass to `provider.invoke(std::move(request))` so an LLM HTTP socket aborts on cancel, or poll `ctx.cancel_token->is_cancelled()` for your own loops |
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
                 std::map<std::string, std::string> output_map = {});
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

엔진이 전달하는 실행별 dispatch metadata입니다. `RunConfig`(미지정 시 새 usage
accumulator 포함), `RunMetadata`, 유효 Store, 선택적 resume value로 구성합니다.
노드는 `run(NodeInput) -> NodeOutput` override 안에서 `in.ctx`로 사용합니다.

```cpp
struct RunContext {
    std::shared_ptr<CancelToken>  cancel_token;
    std::shared_ptr<UsageAccumulator> usage;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::string                   trace_id;
    std::string                   thread_id;
    int                           step;
    StreamMode                    stream_mode;
    std::optional<json>           resume_value;
    std::shared_ptr<Store>        store;
    ToolGate                      tool_gate;
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
    RunStatus status() const noexcept;            // Completed, Interrupted, or StepLimit

    template <typename T> T channel(const std::string& name) const;
    template <typename T> T channel(const ChannelKey<T>& key) const;
    template <typename T>
    std::optional<T> try_channel(const ChannelKey<T>& key) const;
};
```

`RunResult::usage`는 nullable provider 보고이며 지출 bank가 아니다. `native_messages`는 실제 typed 기록을, `provider_outcomes`는 각 소유 Completion/Failure를 보존한다. JSON `output`은 portable projection이다. 전체 입력 기록은 `RunConfig::provider_messages`, typed 이벤트는 `on_provider_event`를 사용한다. 영속 native checkpoint/receipt custody에는 `native_history_archive`가 필요하지만 메모리 sidecar에는 필요하지 않다.
| Field | Type | Description |
|-------|------|-------------|
| `output` | `json` | 모든 채널의 직렬화된 최종 상태 |
| `interrupted` | `bool` | 인터럽트(HITL)로 실행이 일시 중지되었으면 `true` |
| `interrupt_node` | `std::string` | 인터럽트를 발생시킨 노드 이름 |
| `interrupt_value` | `json` | 인터럽트의 이유 또는 페이로드 |
| `checkpoint_id` | `std::string` | 마지막으로 저장된 체크포인트의 UUID |
| `execution_trace` | `std::vector<std::string>` | 실행 순서대로 기록된 노드 이름 목록 |

`max_steps_exhausted()` returns `true` only when the step ceiling stopped the
run while runnable work remained. A graph that reaches `__end__` exactly on its
last permitted step returns `false`.

`status()` returns `RunStatus::Completed`, `RunStatus::Interrupted`, or
`RunStatus::StepLimit` without changing the public `RunResult` data layout.
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

    // Bind tools through NodeContext or EngineResources before compilation.
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
> a sync-only backend therefore remains valid, but its async calls are blocking.
> See [`ASYNC_GUIDE.md` §9.4](ASYNC_GUIDE.md#94-checkpointstore).

```cpp
class CheckpointStore {
public:
    virtual ~CheckpointStore() = default;

    // ── Sync core (5 virtuals, non-pure with bridge defaults) ──────
    virtual void save(const Checkpoint& cp);
    virtual std::optional<Checkpoint> load_latest(const std::string& thread_id);
    virtual std::optional<Checkpoint> load_by_id(const std::string& id);
    virtual std::vector<Checkpoint>   list(const std::string& thread_id,
                                           int limit = 100);
    virtual void delete_thread(const std::string& thread_id);

    // ── Async peers (5 virtuals, default co_return the sync call) ──
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

## 10.5. Observability — OpenTelemetry + OpenInference

> 아래 Python provider/wrapper 예시는 과거 기록이며 typed C++ 계약으로 포팅되지 않았다. 현재 provider 지침이 아니다. C++ 변경은 Python binding을 구현하거나 검증하지 않는다. C++ 관측자는 기존 공개 텍스트/scalar/nullable count만 내보내며 raw native 상태를 내보내지 않는다.
**Module:** `neograph_engine.tracing` (OTel-shape) +
`neograph_engine.openinference` (LLM-shape)
**Since:** OTel layer in v0.3.x; OpenInference layer in **v0.6.0**.

NeoGraph emits its `GraphEvent` stream through the same callback the
streaming API uses. Two helpers ride on top:

  - **`otel_tracer(tracer)`** — vendor-neutral OpenTelemetry spans.
    Root span per run + child span per node + status / error / interrupt
    mapping. Spans flow to any OTel backend (Jaeger, Tempo, Honeycomb,
    Datadog, …). Useful when you already run an APM that just needs
    spans-shaped data.
  - **`openinference_tracer(tracer)` + `OpenInferenceProvider`** —
    LLM-shape attribute layer on top. Same OTel mechanics, but each
    span carries `openinference.span.kind` (`"CHAIN"` / `"LLM"`) plus
    LLM-specific keys (`llm.model_name`, `llm.input_messages.{i}.…`,
    `llm.token_count.{prompt,completion,total}`, etc.) so a backend
    that recognises the OpenInference convention — Phoenix, Arize,
    Langfuse — renders the trace as a chat-bubble + DAG hierarchy +
    per-call token cost UI (the "LangSmith UX").

### `otel_tracer` — OTel-shape spans

```python
from contextlib import contextmanager
from typing import Any, Callable, Iterator, Optional

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

| Knob | Default | Purpose |
|---|---|---|
| `root_name` | `"graph.run"` | Span name for the per-run root span |
| `node_span_prefix` | `"node."` | Prefix concatenated with each node name |
| `attribute_prefix` | `"neograph"` | Prefix for engine-specific attributes (`neograph.node`, `neograph.next_nodes`, etc.) |
| `on_event` | `None` | Optional secondary callback receiving every raw `GraphEvent` — useful for chaining with logging / metrics |

Events handled: `NODE_START` opens a child span, `NODE_END` closes
it (with `Status.OK`), `ERROR` records the exception and ends the
span with `Status.ERROR`, `INTERRUPT` tags
`{attribute_prefix}.interrupted = true` and ends.

Concurrent fan-out (multi-Send): each node-name keeps a stack of
open spans; `NODE_END` pops the most recent. Always-end-on-exit:
the context-manager's `finally` block force-closes any spans still
open if the run raises.

```python
from opentelemetry import trace
from neograph_engine.tracing import otel_tracer

tracer = trace.get_tracer("my-service")
with otel_tracer(tracer) as cb:
    engine.run_stream(cfg, cb)
```

### `openinference_tracer` — adds LLM-shape attributes

Same shape, plus each span tagged
`openinference.span.kind = "CHAIN"` and node payload encoded as
`input.value` / `output.value` JSON blobs. Phoenix / Arize / Langfuse
treat the trace as an LLM chain in their UI.

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

The tracer also attaches each node span as the OTel *current
context* (via `otel_context.attach`) so a `Provider.complete()`
call inside the node body opens its `llm.complete` span as a child
of that node — the trace is a single connected tree, not 3+ orphan
trace-IDs (the v0.6.0 contextvar-propagation fix).

### `OpenInferenceProvider` — wraps any `Provider`

> **과거 Python-only 예시.** 아래 Python Provider/OpenInference wrapper는 현재 C++ typed 전환에서 port/qualification하지 않았다. 새 `ProviderRequest`/owned-outcome 계약의 호환 bridge가 아니다.

```python
class OpenInferenceProvider(Provider):
    def __init__(self, inner: Provider, tracer: Any,
                 *, span_name: str = "llm.complete"):
        ...
```

On every `complete(params)` call it opens an LLM-kind child span
under the current OTel context (so it nests under whichever node
span is active), captures the OpenInference attributes, delegates
to `inner.complete()`, then closes the span. Tracing failures are
swallowed — observability never breaks the LLM call. Inner-provider
exceptions are re-raised after the span is marked ERROR.

Captured attributes per LLM span:

| Attribute | Source |
|---|---|
| `openinference.span.kind` | constant `"LLM"` |
| `llm.model_name` | `params.model` |
| `llm.invocation_parameters` | JSON blob of `temperature`, `max_tokens`, `top_p`, `frequency_penalty`, `presence_penalty` (when set) |
| `llm.input_messages.{i}.message.role` | `params.messages[i].role` |
| `llm.input_messages.{i}.message.content` | `params.messages[i].content` |
| `input.value` / `input.mime_type` | `params.messages` JSON / `application/json` (Langfuse-compatible blob) |
| `llm.output_messages.0.message.role` | `result.message.role` |
| `llm.output_messages.0.message.content` | `result.message.content` |
| `output.value` / `output.mime_type` | `result.message.content` / `text/plain` |
| `llm.token_count.prompt` | `result.usage.prompt_tokens` |
| `llm.token_count.completion` | `result.usage.completion_tokens` |
| `llm.token_count.total` | `result.usage.total_tokens` |

### End-to-end: NeoGraph + Phoenix in one block

```bash
docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix:latest
pip install neograph-engine opentelemetry-exporter-otlp
```

```python
from opentelemetry import trace
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import BatchSpanProcessor
from opentelemetry.exporter.otlp.proto.grpc.trace_exporter import OTLPSpanExporter
from neograph_engine.openinference import OpenInferenceProvider, openinference_tracer
from neograph_engine.llm import OpenAIProvider
import os
import neograph_engine as ng

provider = TracerProvider()
provider.add_span_processor(
    BatchSpanProcessor(OTLPSpanExporter(endpoint="http://localhost:4317", insecure=True)))
trace.set_tracer_provider(provider)
tracer = trace.get_tracer("my-app")

inner = OpenAIProvider(api_key=os.environ["OPENAI_API_KEY"])
wrapped = OpenInferenceProvider(inner, tracer)
ctx = ng.NodeContext(provider=wrapped)
engine = ng.GraphEngine.compile(graph_def, ctx)

with openinference_tracer(tracer) as cb:
    engine.run_stream(ng.RunConfig(input={"messages": [...]}), cb)

# Open http://localhost:6006 — the trace renders as a chain with
# each LLM call expanded into prompt / response / token counts.
```

Endpoint URL is the only thing you change to point this at Langfuse
self-host instead of Phoenix — both honour OpenInference and OTLP.

### Notes

- **Opt-in dependency.** `opentelemetry-api` is not pulled by the
  base wheel. Importing `neograph_engine.tracing` /
  `.openinference` raises `ImportError` on first use only when the
  package is missing — install with
  `pip install opentelemetry-api opentelemetry-sdk opentelemetry-exporter-otlp`.
- **OTel contextvars across pybind.** The `otel_tracer` in v0.3.x
  documented that `trace.use_span(...).__enter__()` without
  `__exit__()` leaks the contextvar AND doesn't reliably propagate
  through the C++ → Python callback boundary. Both tracers now use
  explicit `otel_context.attach` + `detach` token pairs to control
  current-span activation deterministically.
- **`otel_tracer` vs `openinference_tracer`.** Use the OTel one
  when your backend is APM-shape (Jaeger, Datadog) and you want
  generic spans. Use the OpenInference one when your backend is
  Phoenix / Langfuse / Arize and you want LLM-shape rendering. The
  two can't be combined on the same run — they're alternative
  callbacks for the engine's event stream.

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
| `model` | `std::string` | Model override (empty uses provider default) |

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
| `model` | `std::string` | Model override (empty uses provider default) |
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

### json_path Utilities

**Header:** `<neograph/llm/json_path.h>`
**Namespace:** `neograph::llm::json_path`

Utility functions for navigating and manipulating JSON values using dot-separated
path strings. Available for general JSON use; not the typed SDK request/response codec.

```cpp
namespace json_path {
    std::vector<std::string> split_path(const std::string& path);
    const json* at_path(const json& root, const std::string& path);
    json* at_path_mut(json& root, const std::string& path);
    bool has_path(const json& root, const std::string& path);

    template<typename T>
    T get_path(const json& root, const std::string& path, const T& default_val);

    void set_path(json& root, const std::string& path, const json& value);
}
```

| Function | Description |
|----------|-------------|
| `split_path(path)` | Splits a dot-path string into segments. Example: `"choices.0.message"` becomes `["choices", "0", "message"]` |
| `at_path(root, path)` | Navigates into a JSON value by dot-path. Numeric segments index into arrays. Returns `nullptr` if the path does not exist |
| `at_path_mut(root, path)` | Mutable version of `at_path` |
| `has_path(root, path)` | Returns `true` if the dot-path exists in the JSON value |
| `get_path<T>(root, path, default_val)` | Returns the value at the path converted to type `T`, or `default_val` if the path does not exist or conversion fails |
| `set_path(root, path, value)` | Sets a value at a dot-path, creating intermediate objects as needed |

**Examples:**

```cpp
using namespace neograph::llm::json_path;

json data = json::parse(R"({"choices": [{"message": {"content": "Hello"}}]})");

// Navigate
const json* msg = at_path(data, "choices.0.message.content");
// *msg == "Hello"

// Check existence
bool exists = has_path(data, "choices.0.message.role");
// exists == false

// Get with default
std::string role = get_path<std::string>(data, "choices.0.message.role", "assistant");
// role == "assistant"

// Set value (creates intermediates)
set_path(data, "metadata.version", 2);
```

---

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
    std::string model) {
    auto provider = std::make_shared<neograph::llm::SchemaProvider>(
        std::move(descriptor), std::move(options));

    std::vector<std::unique_ptr<neograph::Tool>> tools;
    tools.push_back(std::make_unique<SearchTool>());
    tools.push_back(std::make_unique<CalculatorTool>());

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
             {"routes", {{"yes", "tools"}, {"no", "__end__"}}}}
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
                {{{"channel", "priority"}, {"value", "high"}}} // state updates
            };
        } else {
            result.command = Command{
                "normal_handler",
                {{{"channel", "priority"}, {"value", "normal"}}}
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
JSON-RPC 2.0 over Streamable HTTP. `A2AClient` calls a remote
agent (`message/send`, `tasks/get`, `tasks/cancel`, AgentCard
discovery, `message/stream` SSE); the server side adapts a
NeoGraph `GraphEngine` into an A2A endpoint via
`GraphAgentAdapter`. Dual `v0.3` / `v1` method-name dispatch —
see commit `bc675a1`. Streaming uses `SseFrameSplitter` (client)
and httplib chunked (server). Caller node embeds an A2A call as
a graph node.

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
