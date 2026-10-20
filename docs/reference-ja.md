<!-- neograph-i18n: source=docs/reference-en.md locale=ja source_sha256=fac3eca036504cdb2245854cdb495e37b7e9752e4acddd7c9e1fd2346015da55 -->
# NeoGraph API — ナラティブツアー
**Languages:** [English](reference-en.md) | [한국어](reference-ko.md) | [日本語](reference-ja.md) | [简体中文](reference-zh-CN.md)
この文書は NeoGraph の公開 API を順に案内する **ナラティブツアー** であり、
完全なリファレンスではありません。実際のエージェントを構築するときに出会う順に、
基礎型 → プロバイダー/ツールインターフェース → グラフ型 → エンジン →
チェックポイントストア → マルチ LLM → MCP の各モジュールを説明します。
以下の provider 節は typed 移行を説明し、公開ヘッダーを正とします。
このツアーに含まれないモジュールもあります。
(`neograph::a2a`, `neograph::acp`, `neograph::async`,
`SqliteCheckpointStore`、`PostgresCheckpointStore`、
`NodeCache`、`AsyncTool`、`create_deep_research_graph`)
には、このツアーで扱っていない **ヘッダー上の公開 API があります**。
上記を含む全モジュールの型ごとの完全な API 一覧については、以下にリンクした
`include/neograph/` の公開ヘッダーを使用してください。このナラティブツアーが
推奨される入口であり、ヘッダーが正式なリファレンスです。
この分割により、ナラティブを最初から最後まで読める大きさに保ちながら、
詳細なリファレンスを `include/neograph/` の実装と一緒に維持できます。
**モジュール一覧:**
| モジュール | 名前空間 | 説明 | ツアー | ヘッダー |
|--------|-----------|-------------|------|---------|
| Core | `neograph` | 基礎型、Provider / Tool インターフェース | [§1–§3](#1-foundation-types) | [Provider](../include/neograph/provider.h) |
| Graph | `neograph::graph` | グラフエンジン、ノード、状態、チェックポイント、Store | [§4–§11](#4-graph-types) | [GraphEngine](../include/neograph/graph/engine.h) |
| LLM | `neograph::llm` | LLM プロバイダー実装と Agent | [§12](#12-llm-module) | [Agent](../include/neograph/llm/agent.h) |
| MCP | `neograph::mcp` | Model Context Protocol クライアント | [§13](#13-mcp-module) | [MCPClient](../include/neograph/mcp/client.h) |
| Util | `neograph::util` | 並行処理ユーティリティ | [§14](#14-util-module) | [RequestQueue](../include/neograph/util/request_queue.h) |
| **A2A** | `neograph::a2a` | Agent-to-Agent JSON-RPC ブリッジ (クライアント + サーバー + ストリーミング) | [公開ヘッダー](../include/neograph/) | [A2AClient](../include/neograph/a2a/client.h) |
| **ACP** | `neograph::acp` | Agent Client Protocol — stdio 上のエディター↔エージェント双方向 RPC | [公開ヘッダー](../include/neograph/) | [ACPServer](../include/neograph/acp/server.h) |
| **Async** | `neograph::async` | Asio の HTTP/SSE/WS ヘルパー、ConnPool、run_sync | [公開ヘッダー](../include/neograph/) | [WsClient](../include/neograph/async/ws_client.h) |
追加モジュールは `include/neograph/{a2a,acp,async}/` に公開ヘッダーを持ちます。このツアーではこれらのモジュールを扱わないため、正確な契約はヘッダーを参照してください。

CMake 3.20 以上が必要です。Core は所有 typed provider 契約を公開するため `NEOGRAPH_BUILD_LLM=OFF` でも SchemaProvider runtime は必須です。明示的な `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` が優先され、それがなければ設置済み `SchemaProvider` runtime package を探します。なければ `NEOGRAPH_FETCH_SCHEMAPROVIDER=ON`（既定）の時に `cmake/NeoGraphSchemaProvider.cmake` が固定した不変 GitHub archive を取得します。Offline build は SDK を設置し、`CMAKE_PREFIX_PATH` に prefix を設定して `-DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` を渡します。Sibling checkout は推測せず、削除済み bundled interpreter も選択しません。NeoGraph の任意 HTTP module を無効にしても SDK runtime の transport 依存は必要です。検証済みのプラットフォームは Linux/POSIX です。Windows NTFS と macOS の実装はありますが、この SDK では検証していません。WASM provider runtime のサポートは確立していません。

SDK imported target は `include/SchemaProvider` include root を提供します。公開例は recipe 専用 helper なしで `<descriptor/descriptor.h>`、`<runtime/client.h>`、`<neograph/llm/schema_provider.h>` を直接使います。

設置 SDK package は正確に `0.3.5` で、interface revision 6 header と shared-library generation 6 の一致が必要です。バージョン一致だけでは旧 interface/ABI binary を承認しません。

```cmake
find_package(SchemaProvider 0.3.5 EXACT CONFIG REQUIRED COMPONENTS runtime transport)
find_package(NeoGraph CONFIG REQUIRED)
target_link_libraries(app PRIVATE neograph::core neograph::llm SchemaProvider::runtime)
```

## 目次
- [1. 基礎型](#1-foundation-types)
  - [ToolCall](#toolcall)
  - [ChatMessage](#chatmessage)
  - [ChatTool](#chattool)
  - [Owned Outcome](#owned-outcome)
  - [Portable projections](#portable-projections)
  - [ADL シリアライズ](#adl-serialization)
- [2. Provider インターフェース](#2-provider-interface)
  - [ProviderRequest / ProviderControls](#providerrequest--providercontrols)
  - [PreparedProviderRequest / ProviderBudgetClaim](#preparedproviderrequest--providerbudgetclaim)
  - [価格の証拠とモデルカタログ](#pricing-evidence-and-the-model-catalogue)
- [3. Tool インターフェース](#3-tool-interface)
  - [Tool](#tool)
  - [ツール失敗の契約](#tool-failure-contract)
  - [ノード失敗の契約](#node-failure-contract)
- [4. グラフ型](#4-graph-types)
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
  - [EngineConfig と EngineResources](#engineconfig-and-engineresources)
  - [RunConfig](#runconfig)
  - [RunResult](#runresult)
  - [GraphEngine](#graphengine)
- [7b. エンジン内部](#7b-engine-internals)
  - [GraphCompiler](#graphcompiler)
  - [Scheduler](#scheduler)
  - [CheckpointCoordinator](#checkpointcoordinator)
  - [NodeExecutor](#nodeexecutor)
- [8. チェックポイント](#8-checkpoint)
  - [Checkpoint (struct)](#checkpoint-struct)
  - [CheckpointStore](#checkpointstore)
  - [InMemoryCheckpointStore](#inmemorycheckpointstore)
- [9. Store](#9-store)
  - [Namespace](#namespace)
  - [StoreItem](#storeitem)
  - [Store (abstract)](#store-abstract)
  - [InMemoryStore](#inmemorystore)
- [10. ローダー](#10-loader)
  - [ReducerRegistry](#reducerregistry)
  - [ConditionRegistry](#conditionregistry)
  - [NodeFactory](#nodefactory)
  - [Built-in Registrations](#built-in-registrations)
- [11. ReAct グラフ](#11-react-graph)
- [12. LLM モジュール](#12-llm-module)
  - [SchemaProvider](#schemaprovider)
  - [Agent](#agent)
- [13. MCP モジュール](#13-mcp-module)
  - [MCPTool](#mcptool)
  - [MCPClient](#mcpclient)
- [14. Util モジュール](#14-util-module)
  - [RequestQueue](#requestqueue)
- [使用例](#usage-examples)
  - [最小 ReAct エージェント](#minimal-react-agent)
  - [条件付きルーティングを持つカスタムグラフ](#custom-graph-with-conditional-routing)
  - [チェックポイント付き Human-in-the-Loop](#human-in-the-loop-with-checkpointing)
  - [Send による動的ファンアウト](#dynamic-fan-out-with-send)
  - [Command によるルーティング上書き](#routing-override-with-command)
  - [SchemaProvider のマルチ LLM 対応](#schemaprovider-multi-llm-support)
  - [MCP ツール統合](#mcp-tool-integration)
---
<a id="1-foundation-types"></a>
## 1. 基礎型
**ヘッダー:** `<neograph/types.h>`
**名前空間:** `neograph`
全モジュールで共有するコアデータ型です。LLM のチャットプロトコルにおける
メッセージ、ツール呼び出し、補完結果、およびそれらの JSON シリアライズを表します。
### ToolCall
LLM が要求した 1 回のツール呼び出しを表します。
```cpp
struct ToolCall {
    std::string id;         // Unique identifier assigned by the LLM
    std::string name;       // Name of the tool to call
    std::string arguments;  // JSON-encoded string of arguments
};
```

| フィールド | 型 | 説明 |
|-------|------|-------------|
| `id` | `std::string` | LLM が割り当てる、このツール呼び出し固有の識別子 |
| `name` | `std::string` | 呼び出すツール関数の名前 |
| `arguments` | `std::string` | 呼び出し引数を含む JSON エンコード文字列 |
### ChatMessage
会話中の 1 件のメッセージです。system、user、assistant、tool のすべてのロールに対応します。
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

| フィールド | 型 | 説明 |
|-------|------|-------------|
| `role` | `std::string` | メッセージのロール: `"system"`、`"user"`、`"assistant"`、`"tool"` |
| `content` | `std::string` | メッセージのテキスト内容 |
| `tool_calls` | `std::vector<ToolCall>` | assistant が要求したツール呼び出し (assistant 以外では空) |
| `tool_call_id` | `std::string` | このツール結果を元のツール呼び出しに結び付ける ID |
| `tool_name` | `std::string` | この結果を生成したツールの名前 |
| `image_urls` | `std::vector<std::string>` | マルチモーダル/vision メッセージの画像 URL。`data:image/...;base64,...` または `https://...` を受け付ける |
### ChatTool
LLM が利用できるツールを定義します。
```cpp
struct ChatTool {
    std::string name;        // Tool name (unique identifier)
    std::string description; // Human-readable description for the LLM
    json parameters;         // JSON Schema describing the tool's parameters
    std::optional<bool> strict; // OpenAI Responses strict mode; unset keeps the SDK default
};
```

| フィールド | 型 | 説明 |
|-------|------|-------------|
| `name` | `std::string` | ツール固有の名前 |
| `description` | `std::string` | ツールの目的を LLM に説明する表示用説明 |
| `parameters` | `json` | 受け付けるパラメーターを記述する JSON Schema オブジェクト |
| `strict` | `std::optional<bool>` | この function ツールの OpenAI Responses strict モード。未設定なら SDK policy の既定値に従い、既定値は strict 互換 schema（すべての object が `"additionalProperties": false` を指定し、各 property を `required` に列挙）にのみ適用され、それ以外の schema は `"strict": false` で送信される。`false` はそのまま送信され、互換でない schema に `true` を指定すると dispatch 前に `InvalidRequest` で失敗する。他の family は `true` を拒否する。Python: `ChatTool(..., strict=None)` |
### Owned Outcome

プロバイダー呼び出しは `sp::runtime::Result`、すなわち `sp::Completion` または `sp::Failure` を保持する不変の所有 `std::shared_ptr<const sp::Outcome>` を返します。表示テキストだけでなく結果全体を保持してください。順序付きメッセージ/パート、native continuation、存在する wire envelope、順序付き raw 観測、停止の根拠と実際の試行メタデータは呼び出しとクライアント破棄後も残ります。`input_total`、`output_total`、`total` などの使用量カウンターは `std::optional<sp::Count>` で、存在する count は `uint64_t value` と `Evidence` を持ちます。`Usage` は stage、quality、conflict も記録します。欠落は不明であり、ゼロを作りません。失敗も元の部分結果を保持します。`ProviderFailure::outcome()` と `ProviderObserverError::outcome()` は実際の結果を保持し、後者の `cause()` は観測者の例外を保持します。

`sp::Completion::wire_envelope` と `sp::PartialCompletion::wire_envelope` はプロバイダーファミリーごとに有無が異なり、空の場合があります。存在しなければ Python ビューは `None` を返します。現在のバッファ型 Chat はこのフィールドを空のままにし、完全な応答文書を `raw_events` の `sp::RawWire{type="chat.completion", payload=document}` に保持します（Python では `ProviderRawWire`）。有無と実際の型付き raw 証拠を確認してください。フォールバックでエンベロープを合成したり、部分的な失敗を成功に変えたりしません。プロバイダーの非公開フィールドを含む所有されたワイヤー証拠は、プロバイダー破棄後も保持された結果に残ります。ネイティブトレースは raw エンベロープ/イベントとネイティブ再生/推論を除外します。JSON 参照用のコピーはネイティブ権限や財務上の権限を与えません。

利用側は完全な型付きメッセージ/パート、論理的なロールとテキストを使い、続行に必要なら真正なネイティブ所有権を保持してください。チェックポイントや Chat リクエストのテキスト content は、テキスト文字列でも有効な型付きテキストパート配列でも表せます。偶然選ばれたどちらの直列化形式も、普遍的な契約ではありません。

真正な `NativeContext` の再生には、元の `request.messages` の完全な prefix を保持し、直接返された `outcome.messages` をネイティブ所有権を維持したまま順番に追加します。直接の結果には新しく返されたメッセージが含まれ、元のリクエスト履歴は含まれません。その assistant メッセージだけを再生すると、ワイヤー I/O 前に `ReplayIneligible` で拒否されます。`NativeArchive` から復元した assistant にも元の完全な prefix が必要です。アーカイブの保管権限は履歴の系譜チェックを代替しません。グラフの `RunResult.native_messages` はすでに全履歴を含むので、元の入力を再び先頭に付けないでください。

`UsageAccumulator::snapshot()` は累積報告を返します。`total_tokens_wide()` は計上済みトークンと未解決予約の合計で、報告使用量として表示してはいけません。精算には input/output count のある final・consistent 報告が必要で、根拠のある最大 total を計上し、超過使用量も clamp しません。累積対象の一つでも counter が欠落すれば集計も不明です。予約、ローカル計上、vendor 請求書は別の記録です。

実結果の後に post-effect 精算や terminal receipt 永続化が失敗すると、`ProviderDispatchOutcomePersistenceError::outcome()` は元の不変結果、`cause()` は元の永続例外を保持します。delivery も失敗した場合は `delivery_error()` が元の観測者例外を保持します。永続化成功後の観測者失敗は元の例外を変更せず再送出し、不明/結果なし transport 失敗では outcome を捏造しません。

### Portable projections

`ChatMessage` / `ChatTool` と JSON は portable projection であり native 権限ではありません。Portable 形式は [`provider-message-v3`](../schemas/provider-message-v3.schema.json)、[`runtime-history-record-v3`](../schemas/runtime-history-record-v3.schema.json) で、v2 形式のレコードも引き続き読み込めます。真正な C++ checkpoint sidecar はメモリ内の native seal を保持します。永続 native 履歴には host-owned `sp::NativeArchive` が必要です。closed v3 / `spna3` は独立キーによる認証済み owner-private custody で、archive v2 は更新・解釈せず拒否します。認証は全 semantic descriptor 選択（origin/path/header、policy、要求 field mapping、usage path、stop mapping）、owner と正確な custody binding を結び付けます。暗号化や vendor-issuer 認証ではありません。archive 本文・キー・native blob・raw wire 観測は公開しません。Archive は証拠保存であり、金銭 grant や spending lease ではありません。Program/external bank は独立 journal が所有し、snapshot コピーで credit は作れません。

`RuntimeHistoryRecord` の引数なしの `serialize_canonical()` はポータブルなレコードを扱います。永続ネイティブ履歴には `serialize_canonical(archive, owner_id)` と、その ID に所有者範囲が一致するアーカイブを使います。Python も同じオーバーロードと `RuntimeHistoryRecord.parse(stored_bytes, archive=None, owner_id="")` を公開します。ポータブルなレコード用の既定引数は、一致するアーカイブなしでのネイティブ復元を許可しません。Python のパースとアーカイブ対応の直列化はネイティブ処理中に GIL を解放します。JSON の観測データと、アーカイブで認証された保管権限は区別されます。

Python は `ContextStore.hydrate_records(range)` と `history_record_by_message_id(feed, message_id)` で型付き保管権限を保持します。後者はレコードまたは `None` を返します。`SQLiteContextStore(database_path, archive=None)` は実際のアーカイブを受け取り、`LocalProgramHost` は最後の任意引数 `native_history_archive=None` を `RuntimeConfig` に渡します。これらは権限を捏造したり、永続 Program ストアのバックエンドを追加したりしません。返された `RuntimeHistoryRecord.message` は真正な共有ネイティブ所有権を保持する独立した型付きコピーで、変更しても不変の RAW 識別子を書き換えられません。ストアの構築と型付き取得は GIL を解放し、保管権限が必要なネイティブ履歴はアーカイブなしのストアでは拒否されます。

`Assistant` メッセージを含む RAW `RuntimeHistoryRecord` には `RuntimeTrustClass.ModelOutput` が必要です。`RuntimeTrustClass.UntrustedInput` は `User` メッセージだけを受け入れます。これらは実際の Python enum 名です。trust class はレコードのロールと由来の境界を示し、選択してもネイティブ再生の保管権限や支出・実行権限を付与しません。

**Standalone 予算 bank。** Standalone bank は、単調な trusted-store namespace obligation と、実際の不変 original owner/thread/graph scope、ceiling、deadline/clock identity、generation を要求します。全 checkpoint commitment/revision に対する正確な durable head CAS だけが host-owned opaque lease を発行できます。正確な pending effect window を provider I/O 前に永続化し、真正な SDK outcome と実際の charge、nullable report、hold、dedup identity で精算しなければなりません。Checkpoint と next head は、同じ owned actor/revision 下で原子的に publish します。Bank metadata の削除、checkpoint pruning、old authenticated snapshot replay、同一 ID の overwrite、actor の喪失では credit を与えません。既存 65 hold がある ceiling 130 を 129 に下げると、別の 65 は許可できません。証明済み no-effect 失敗は unchanged head を release でき、authentic 130 の復旧を継続できます。Crash/unknown/lost-lease window は refund/retry/fallback なしで hold を保持します。Plain archive 設定は money も native spending lease も与えず、現在の `config.usage` は既存の standalone obligation を置換できません。Program/external bank は、それぞれの journal が所有します。

**Managed budget lease の宣言。** `<neograph/graph/checkpoint.h>` は `owner_scope`、logical `thread_id`、private backend `storage_thread_id`、`graph_identity`、`original_ceiling`、`original_deadline_ticks`、`deadline_clock_identity` を持つ `ManagedBudgetLeaseScope` を宣言します。`OwnedManagedBudgetLease` は read-only `scope()`、`actor_id()`、不変 `bank_generation()`、`revision()`、`head_checkpoint_id()`、`head_commitment()` を公開し、公開 authority-import constructor はありません。`ManagedBudgetEffectReceipt` は `active()`、`effect_id()`、`claim_amount()`、`request_digest()` を公開し、default receipt は権限を与えません。`CheckpointStore` は `acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`、`begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`、`settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`、`publish_managed_budget_checkpoint(lease, checkpoint)`、`release_managed_budget_lease(lease)` と `_async` counterpart を宣言します。Sync `CheckpointStoreCore` と `AsyncCheckpointStore` はそれぞれの variant を公開します。`managed_budget_checkpoint_commitment(checkpoint)` は bank JSON だけでなく全永続 checkpoint を結び付けます。

**Shared-bank fork（InMemory）。** 元の真正な C++ fork は ONE original financial journal と trusted current branch head を使い、grant を複製しません。`publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)`（および `_async`）は authentic current source/full commitment と実際の same-bank native C++ pointer を要求し、durable standalone fork は明示的に unsupported のままです。`OwnedManagedBudgetLease::scope()` と original owner/thread/graph、ceiling、deadline/clock、generation は不変です。Read-only store-issued `execution_thread_id()` / `execution_storage_thread_id()` は execution branch を別に選び、`GraphState::budget_original_thread_id()` は元の financial bank を示します。正確な selected-branch head CAS と global actor/revision は canonical current counter、pending effect、burned identity に対して全 branch を直列化します。Original/fork branch は補充なしで使用可能なままです。Stale snapshot、checkpoint copy、imported JSON は alias を発行したり head を巻き戻したりできません。保存された original ceiling は effective fork ceiling とは別で、これを広げることや JSON だけでの復元は拒否されます。Unbounded reported observation は factual data で finite grant ではありません。証明済み zero-effect lease だけが unchanged head を release でき、unknown/pending effect は obligation を保持します。

**Lease release エラー。** `<neograph/graph/engine.h>` の `graph::ManagedBudgetLeaseReleaseError` は `ProviderOutcomeError` を継承します。`cause()` は元の execution exception を保持し、`release_error()` は二次 durable lease-disposition 失敗を公開します。`outcome()` は真正な SDK 証拠があれば保持し、SDK outcome がなければ null です。Release 失敗は結果を捏造せず再 dispatch も許可しません。Closed `_neograph_managed_budget_scope` metadata は元の logical scope/cap/deadline clock/generation を記述しますが、backend CAS 権限ではなく data です。

**Archive owner と retention。** Finite standalone root または authenticated finite source だけが、実際に設定された `sp::NativeArchive::owner_scope()` から省略された original owner を継承します。Unbounded/plain owner metadata の意味は不変です。明示的に矛盾する archive owner は lease acquire 前に拒否します。`CheckpointStore::retains_native_checkpoint() const noexcept` と対応する Core/Async storage capability は既定 false で、実際の InMemory backend は true に override し、wrapper は実際の retention を委譲します。この read-only 記述は正当な unleased/plain/unbounded C++ native checkpoint custody を許しますが、spending credit や native replay authority は与えません。Leased custody は JSON flag や推測した store type ではなく実際の store-issued receipt を使います。

**Native-custody gate。** Managed effect begin は pending-effect/slot/held-window 変更前に、真正に結び付けた NativeArchive または実際の local store-issued private C++ retention capability を要求します。Private capability は JSON から import せず wire にも転送しません。C++ sidecar は境界を越えられないため、remote backend が InMemory でも gRPC は実際の client/server archive を要求します。Archive が finite source owner を提供しなければ元の anonymous owner scope は空のままで、実際の archive binding は original scope に一致しなければなりません。Financial head/lease 証拠だけでは native-custody readiness を証明しません。

診断 JSON は構文上有効な duplicate-key 文書も含め元の raw byte を保持しますが、実行可能な要求/config admission は重複を拒否します。元の non-2xx 応答 JSON は二度目の損失 parse なしで `http.error` 証拠に残ります。named SSE error は後続の正常 stream close より優先します。診断/provider metadata の上限は承認済み source extent であり、無関係な小さい error-text cap ではありません。

`ProviderRequest::observer_limits` は host-only です。明示した `max_events`・`max_bytes` は正数で、承認済み SDK 配信上限を下げることしかできません。`provider-request/v3` digest は実効 limit、mode、encoded body、retry policy と全 semantic descriptor binding を結び付けます。Bridge は queued/draining batch を通じて実際の PMR vector/map capacity と所有 event/document byte を計上し、queue mutex 外で cancellation を要求します。`messages` という名の Generic channel を chat に強制変換しません。native `history` channel を `messages` に mapping すると C++ sidecar が保持され、JSON から native 権限を作りません。

`ProviderOutcomeError` は結果を保持する共通 host-error base です。`ProviderObserverError` と `ProviderDispatchOutcomePersistenceError` は完全に drain した SDK 結果と元の `cause()` を保持し、後者は二次 observer 失敗も `delivery_error()` に保持します。`ProviderFailure::outcome()` は SDK 失敗自体を保持します。これは Node/Program の再 dispatch 権限ではありません。Provider retry の唯一の所有者は SDK で、caller が選んだ `max_output_tokens` を黙って clamp しません。

`ProgramFailure` は live `provider_outcome`・`provider_cause` を保持します。Canonical factual SDK witness は真正な archive custody を owner/run/version/bundle/operation/attempt に結び付け、Runtime は復旧失敗を公開する前に設定済み custody を eager に復元します。公開 data-only `ProgramResult::create()` は事前入力 witness で迂回できず、未解決の parsed seal は実行結果ではありません。プロセス再起動後は元の exception pointer がなく `provider_cause == nullptr` であり、text から再作成しません。`native_history_archive` のないホストは provider の native replay 状態を保管できないため、native メッセージを含む失敗を、その状態を除いて公開します。メッセージ、usage、stop reason、error、raw event はそのままで、各メッセージの native seal だけを除いた複製であり、witness に `provider_outcome_native_retained: false` が入ります。結果の `provider_outcome` はこの複製で、呼び出しを実行したプロセスでは元の outcome が `provider_cause` に残ります。それ以外の理由、つまり archive への書き込みが失敗して永続化できない失敗は、serialize/publish/replay できません。

Python の `LocalProgramHost` は破棄時に、`ProgramRuntime` がスケジューラーの処理をキャンセルし、残りの処理を完了させて join する間、呼び出し側の GIL を解放します。これにより実行中の Python ノードが終了できます。残りの host メンバーと、それらが所有する Python コールバック/オブジェクトを破棄する前に GIL を再取得します。変更は終了処理のみで、追加の capability binder や実行権限は公開しません。

`RecordedBindingSet` は source-bound の move-only data で、caller 提供 dispatcher ではありません。信頼された Catalog の `recorded_capability_binder` は実際の永続 source event を独立に読み、captured-only capability を materialize します。`ProgramRuntime::replay_recorded()` は元の selected-source permission を検証し、実際の残存 bank を durable CAS で移します。inherited spend は新しい model grant ではありません。旧 `start_recorded` 更新 API は削除されました。InMemory/File/SQLite/PostgreSQL Program store は実行全体で正確で不変の owned lease を保持し、expiry による更新をしません。Controlled JavaScript も underlying capability manifest を検証し、正確な completed command 結果を消費して external effect を再 dispatch しません。

**Recorded replay の精算。** Captured command replay は実行前に新しい CPU wall-time/Core work だけを durable に reserve し、測定済み work と新しく生成した Core checkpoint を result CAS で publish します。新しい model、money、Program-operation allowance を消費せず、captured external effect を再 dispatch しません。未精算 reservation は debit を保持します。Await channel receive、timer wait/cancel、handoff wait の開始/release は owning executor/strand 上で直列化します。

```cpp
#include <neograph/types.h>

neograph::json observe_result(const sp::runtime::Result& result) {
    if (!result) throw std::invalid_argument("Missing provider outcome");
    return neograph::outcome_projection_json(*result);
}
```

### ADL Serialization
ADL により `json j = my_tool_call;` や `my_tool_call = j.get<ToolCall>()` のように直接利用できます。
```cpp
void to_json(json& j, const ToolCall& tc);
void from_json(const json& j, ToolCall& tc);

void to_json(json& j, const ChatMessage& msg);
void from_json(const json& j, ChatMessage& msg);
```

ADL serialization は portable message/tool field と宣言済み既定値を保持します。JSON から native SDK continuation 権限は再構成しません。

<a id="2-provider-interface"></a>
## 2. Provider インターフェース

公開契約は所有 typed 準備/dispatch であり、同期・非同期の virtual completion 対ではありません。`ProviderRequest.payload` は Chat、Messages、Responses、Gemini、Interactions の SDK リクエスト variant です。`ProviderMode::Collect` / `Stream` は観測者の有無と独立に転送を選択します。`on_event` は借用 typed `sp::Event` view を受け取ります。コールバック後に必要なデータだけコピーします。raw JSON override や portable projection による native 権限のインポートは認めません。

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

### エンドポイント呼び出し

`Provider` は自身の origin にある SchemaProvider の非チャット・エンドポイントも転送します。
- 画像、decisions、埋め込み、rerank、moderation
- 音声合成、文字起こし、翻訳
- 動画ジョブ（投入、ポーリング、ダウンロード）
- ファイル（一括アップロードと再開可能なアップロード、取得、一覧、削除、ダウンロード）
- バッチ、トークン数のカウント、OpenRouter の生成統計

`EndpointOutcomeOf<Request>` は各リクエストの型付き結果を表します。単発のエンドポイントでは `std::variant<Result, sp::Failure>`、ジョブ・ファイル・バッチではそれぞれの状態です。

呼び出しは provider 自身のクライアントで実行され、チャット呼び出しと同じ認証情報、admission スロット、リトライ予算、deadline、エラー分類を使います。descriptor ポリシーは呼び出しを準備するときに origin ごとに各エンドポイントを許可します。拒否された呼び出しはそのエラーを持ち、ネットワークに届かないまま `Failure` で終わります。

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

- **準備。** `prepare_endpoint` は SDK の `PreparedEndpoint` を返します。ホストは `complete_endpoint` が実行する前に、これを確認する（`prepared().endpoint()`、`prepared().model()`、`error()`）ことも、ジャーナルに記録することもできます。準備スロットに空きがなければ `sp::runtime::AdmissionError` を投げます。
- **スレッドを止めない呼び出し。** `complete_endpoint_async` はスレッドを止めません。awaitable がクライアントと呼び出しを所有するため、リクエストも provider もそれより長く生きる必要はありません。待機中のコルーチンをキャンセルすると呼び出しもキャンセルされ、結果は SDK の `Cancelled` 失敗になります。
- **そのまま送信。** `SchemaProvider::Defaults` はチャット呼び出しにだけ適用されます。ここでは呼び出しをジャーナルに記録せず、予算の予約も課金もしません。
- **エンドポイントを持たない provider。** SchemaProvider のエンドポイントを持たない `Provider` は `std::logic_error` を投げます。サブクラスは `endpoint_runtime()` をオーバーライドして自身のクライアントを転送し、別の provider を包む provider は `endpoint_runtime_of(inner)` を返します。`has_endpoints()` は provider がエンドポイントを持つかどうかを知らせます。
- **チャット経路で。** `make_endpoint_request(request)` はエンドポイントのリクエストを `ProviderRequest` に入れるため、`prepare`、`dispatch`、`invoke` はこれをチャット呼び出しと同じように扱います：dispatch の Hook、観測者、provider 呼び出し broker とそのジャーナル、トークン予算。結果は型なしで、エンドポイントの `typed()`（`sp::embeddings::typed`、...）がそれに型を付けます。呼び出しはバッファリングされるため、`ProviderMode::Stream` は拒否され、ローカル provider（`prepare_local`）もこれを拒否します。dispatch identity（`request_digest`）にはエンドポイント、メソッド、URL も含まれます。1 つのエンドポイントを複数の操作が共有し、読み取りには本文がないためです。 エンドポイントの読み取り（`PreparedProviderRequest::is_endpoint_read()`：ジョブのポーリングやダウンロード、ファイルやバッチの参照などの GET）は課金されず、再送しても安全なため、ジャーナルを記録する broker はこれをリプレイせず毎回送ります。

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

`make_provider_request(provider, model, messages, tools, controls, mode)` は閉じた family を一つ選びます。ポータブルな制御は `ProviderControls` のメンバーで、family 固有の制御はその family のネストにあり、呼び出しは自分の family のネストだけを適用するので、`ProviderControls` 一つで異なる family を呼ぶノードをまとめて扱えます。family が表現できないポータブルな制御は `std::invalid_argument`、SDK prepare は enum 値、承認 origin、モデル規則、tool 宣言、native binding を I/O 前に検査します。型のない値はネストの `extensions` だけを通り、SDK ポリシーが origin ごとに値スキーマとともに承認します。0.15 のフラットな family メンバー（`chat_reasoning`、`responses_include`、`messages_tool_choice`、`gemini_thinking_level` など）は 0.16.0 ではネストに移される非推奨の別名で、ほかの family は 0.15 と同じくこれを拒否し、別名とネストのメンバーを両方指定すると例外になります。optional は未指定と明示的な `false`・空の選択を区別します。

| Family | ポータブルな `ProviderControls` フィールドと SDK 対応 | Family ネスト（`controls.<nest>`） |
|---|---|---|
| `openai.chat` | `max_output_tokens`, `temperature`, `top_p`, `reasoning_effort`, `service_tier`, `provider`, `response_format` | `chat`: `reasoning`, `include_reasoning`, `usage_include` → `usage.include`, `models`, `modalities`, `audio`, `image_config`, `plugins`, `extensions` |
| `openai.responses` | `max_output_tokens`, `max_tool_calls`, `temperature`, `top_p`, `reasoning_effort`/`reasoning_summary` → `reasoning`, `service_tier`, `required_tool`, `provider`, `response_format`, `store`, `system` → `instructions`, `account_scope` | `responses`: `previous_response_id`, `previous_response_history`, `parallel_tool_calls`, `verbosity` → `text.verbosity`, `truncation`, `include`, `include_names`, `hosted_tools` → `tools`, `modalities`, `image_config`, `plugins`, `mcp_approvals`, `extensions` |
| `anthropic.messages` | `max_output_tokens` → `max_tokens`, `temperature`, `top_p`, `thinking_budget`, `system`, `account_scope`, `provider` | `messages`: `thinking_mode`, `output_effort` → `output_config.effort`, `cache_control`, `tool_choice`, `tools` (server and typed client tools, after the portable tools), `container`, `skills`, `betas`, `mcp_servers`, `extensions` |
| `google.generate` | `max_output_tokens`, `temperature`, `thinking_budget`, `include_thoughts`, `required_tool`, `system`, `account_scope` | `generate`: `history_mode`, `thinking_level`, `safety_settings`, `tool_choice`, `response_modalities`, `speech_config`, `image_config`, `audio_transcription_config`, `audio_format`, `builtin_tools`, `mcp_servers`, `server_tool_invocations`, `retrieval_config`, `extensions` |
| `google.interactions` | `max_output_tokens`, `thinking_level` (optional string), `thinking_summaries`, `service_tier`, `required_tool`, `store`, `system`, `account_scope` | `interactions`: `response_format`, `speech_config`, `transcription_config`, `builtin_tools`, `previous_interaction_id`, `video_task`, `agent`, `background`, `extensions` |

Chat の `sp::chat::ReasoningOptions` は optional `effort`, `max_tokens`, `exclude`, `enabled` を持ちます。この nested reasoning object、`include_reasoning`、`usage_include`、代替 `models` は policy 宣言済み OpenRouter origin に限ります。`sp::OpenRouterRouting` も Chat/Responses/Messages の宣言済み OpenRouter origin のみです。gateway 形式のモデル名では別 origin を承認できません。Responses のホスト型ツール（`controls.responses.hosted_tools`）と Messages のサーバー・型付きクライアントツール（`controls.messages.tools`）は、ネスト内の型付き SDK 値です。

Responses SSE には終了イベント `response.completed` または `response.incomplete` が必要です。`response.done` は同じ完全な終了 Responses envelope（completed または incomplete）を運ぶエイリアスの場合にのみ受け入れられ、終了でない、または形式が不正な `response.done` はプロトコルエラーとして失敗します。`response.content_part.delta` などリストにないイベントは `Unsupported` です。宣言済みの OpenRouter origin では、Responses と Messages は `~vendor/model-latest` のようなルーティングエイリアスに対してゲートウェイが報告する実際のモデル名も受け入れます。サービングモデルは 1 つの応答内で変わってはならず、replay は要求したモデルに束縛されたままです。

| SDK 型 | 閉じた値またはメンバー |
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
| `sp::gemini::ToolChoice` / `ToolChoiceMode` | `mode`: `Auto`, `Any`, `None`, `Validated`; `allowed_function_names`: 宣言済み関数名の vector |

Messages は正の output cap が必要です。mode 未指定の thinking budget は `Manual` を選び、承認 minimum 以上かつ cap 未満にします。`Adaptive`/`Disabled` は明示 budget を拒否します。Manual/adaptive はそれ以外で有効な temperature を省略し、thinking `top_p` minimum を検査しますが、モデル別 temperature 禁止は上書きしません。強制 `Any`/`Tool` は tools と disabled thinking が必要です。`Tool` は宣言済み client tool 名、他 mode は `name` を拒否、`None` は `disable_parallel_tool_use` を拒否します。Generate の thinking budget/level と `required_tool`/`generate.tool_choice` はそれぞれ排他的です。allowed-function リストは宣言済み関数を指します。sampling は family/policy の範囲内です。Generate は `top_p` 非対応、Interactions は両 sampling field 非対応です。

`FamilyPolicy.temperature_forbidden_model_prefixes` はモデル全体と最後の `/` 以降を ASCII 大小文字無視で照合します。組込み Chat/Responses は `gpt-5`, `gpt-6`, `o1`, `o3`, `o4`; Messages は `claude-opus-4-7`, `claude-opus-4-8`, `claude-opus-5`, `claude-sonnet-5`, `claude-fable-` です。gateway prefix のモデルでも禁止 temperature の明示指定は I/O 前に拒否します。

#### Responses provider-held continuation

`previous_response_id` は provider 保管状態を選びます。要求 `messages` は NEW INPUT ONLY で、過去の会話を再送しません。`previous_response_history` は送信しないローカル所有根拠 `std::vector<sp::Message>` です。cursor 単体は新しい text/image 入力を認めても client tool result を承認しません。最初の captured response では authentic original prefix と cursor と同じ ID の terminal assistant response を渡します。後続の authentic in-process cursor-produced terminal response は、全 prefix の再構成なしで private completed tool ownership を持てます。content/origin/model/route/config 不一致は拒否し、任意 ID や projection JSON から ownership は作れません。

server cursor と private completed ownership は `NativeReplay`・native archive 権限を付与しません。`responses_include` 未指定は `reasoning.encrypted_content` を維持し、明示的な空 vector は `[]` を送ります。他の明示選択も尊重します。後続操作が native evidence を要求する場合、根拠不足は失敗します。

#### Explicit portable Gemini history

Generate の既定は `NativeOnly` です。明示 `PortableForeign` は native seal、`wire_output`、signature/native metadata のない caller-created assistant `Text`/`ToolCall` のみを認めます。最初の foreign `functionCall` にだけ Google の `skip_thought_signature_validator` を付け、text-only turn に signature は作りません。authentic native group は元の provenance/content/binding を検証します。破損・不一致 native group を portable に降格せず、imported foreign history は native replay 権限を得ません。

#### Output caps and native continuation

output generation cap は呼出しごとの承認 resource で、native replay config digest からこの cap だけを除外します。content/prefix、origin/route、policy identity、tools、reasoning controls など他の binding は維持します（文書化された per-turn tool selection/cursor 動作を除く）。encoded/prepared request は effective cap を保持し、request digest、journal slot、元 shared bank と絶対 deadline は拘束し続けます。cap を増やす semantic call は同じ grant の下で新しい call ordinal・admission が必要です。seal repair、deadline 更新、旧 effect replay は認めません。native archive は `spna3`/v3、portable JSON は v2 です。

Python は `ChatReasoningOptions`, `ResponsesVerbosity`, `ResponsesTruncation`, `ResponsesInclude`, `MessagesThinkingMode`, `MessagesOutputEffort`, `MessagesCacheControl`, `MessagesCacheTtl`, `MessagesToolChoice`, `MessagesToolChoiceMode`, `GeminiHistoryMode`, `GeminiThinkingLevel`, `GeminiSafetySetting`, `GeminiSafetyCategory`, `GeminiSafetyThreshold`, `GeminiToolChoice`, `GeminiToolChoiceMode` を公開します。enum は同じ値で C++ `None` のみ Python `None_` です。optional class control と message/safety/history vector は detached snapshot なので編集を再代入します。`ProviderControls.previous_response_history` は authentic `ProviderMessage` リストで、仮の単一 response ではありません。

#### Deployment header preprocessing

通常の `sp::descriptor::load(source[, policy])` は `${VAR}` も literal として扱います。明示 `load_with_environment_headers(source, overrides = {}, policy = {})` は Messages 用 optional `ANTHROPIC_WORKSPACE_ID`/`ANTHROPIC_BETA` を読み、unset/empty は省略します。deterministic `load_with_deployment_headers(source, overrides, DeploymentHeaderEnvironment, policy)` は指定 optional `anthropic_workspace_id`/`anthropic_beta` を使います。両方 `LoadResult` を返し descriptor admission 前に処理します。大小文字無視の優先順位は environment < descriptor literal headers < explicit overrides です。重複 override、invalid/reserved name、改行は admission で拒否し、承認後の環境評価・header 変更はありません。

Python 名は `ProviderDeploymentHeaderEnvironment`, `load_provider_descriptor_with_environment_headers(source, overrides=[], policy=None)`, `load_provider_descriptor_with_deployment_headers(source, overrides, environment, policy=None)` です。loader は `ValidatedDescriptor` を返し admission 失敗時は例外です。credential は runtime options に置きます。

#### Additional admission and event rules

Chat nested reasoning は空を拒否し effective scalar `reasoning_effort` と併用できません。`effort`/`max_tokens` は排他的で、budget は正、signed 64-bit wire 範囲内、effective output cap 以下です。`enabled=false` は effort/budget を拒否し、`exclude=true` は `include_reasoning=true` と衝突します。代替モデル名は nonempty・unique で承認 count limit 内です。各モデルに temperature 禁止を含む effective choices の検査を行います。

policy identity は native binding に残ります。公開済み組込み policy revision 4 は旧 policy-3 seal/archive を修復せず拒否します。Generate `safety_settings` は有効 category の重複を拒否し、nonempty allowed-function リストは `Any`/`Validated` のみです。モデルは承認済み二つの Generate descriptor path と一致する literal name が必要です。portable tool result は承認 call identity と一致し、foreign assistant call は client-executed で `wire_type`/`wire_metadata` を持たないものです。

semantic `Stop` 境界で valid/invalid client-call intent は `EndTurn` を `ToolUse` に変え、observer event と final outcome は一致します。具体的な `MaxTokens`, `ContentFilter`, `Unknown` 根拠は維持し、完了した server-executed hosted tool だけでは client `ToolUse` になりません。streaming OpenRouter reasoning fragment は index ごとに結合し最初の到着順を保持します。encrypted blob は個別項目です。owned raw frame と native continuation は元 content と tamper 検査を保持します。

### PreparedProviderRequest / ProviderBudgetClaim
`prepare()` は検証とエンコードを正確に一度行い、元の deadline とキャンセル状態を持つ移動専用 `PreparedProviderRequest` を生成します。永続呼び出し元は `Provider::request_digest()` を assembly に結び付け、承認された budget claim を予約し、dispatch receipt を記録してから、同じハンドルを `ControlledProvider::dispatch_prepared(_async)` で消費します。gate 後の再生成はありません。重複 receipt は再送しません。カスタム実装は `get_name()`、`family()`、`prepare()` を実装し `prepare_runtime()` または `prepare_local()` を使います。local callback は `this` ではなく所有 shared 状態をキャプチャします。

任意の `ProviderControls` は呼び出し元の選択であり、強制デフォルトや黙った cap clamp ではありません。非対応 family 制御は dispatch 前に拒否します。有界呼び出しには承認された実際のモデル input/output 上限が必要で、欠落は `LimitUnknown` です。予約は保守的な支出権限であり、報告使用量・予測・請求書ではありません。不明/部分/delivery-unknown の結果は hold を維持し、実際の最終報告で精算し、超過報告も全量を計上します。使用量の stage が `Missing` なら証拠がない状態で（0 ではありません）、`Partial` はそこまでに見えた値です。どちらも呼び出しを精算せず、hold が残ります。SDK が導出した件数（`Evidence::Derived`）は、報告された件数と同じように精算されます。出力予算を思考で使い切り `candidatesTokenCount` が省かれた Gemini の呼び出しは、導出された `output_total` と `total` だけで実際の使用量を請求できます。retry は明示的な単一層で、既定 off、有界 window と unknown-prior hold を使います。隠れた再送はありません。

`provider_failure_proves_not_accepted(Failure)` は、失敗した呼び出しが hold を解放するか維持するかを、すべての精算経路が共通で判断する唯一の規則です。対象は、bank、managed journal、MCP harness provider のトークン reservation、Program の金額 hold、SQLite provider journal、controlled provider の dispatch receipt（`Failed` または `ReconciliationRequired`）です。次のどちらかの根拠が、完全で矛盾なく揃っている必要があります。

- `NotSent`: リクエストのバイトが 1 つも出ていないことを SDK が証明している（`provider_failure_proves_not_sent(Failure)` はこの半分だけを判定します）。
- 完了した HTTP 4xx 応答（408、425、429 を除く）での `RejectedBeforeOutput`。たとえば不正なキー（401）、不正なリクエスト（400）、誰も満たせない `max_price` の固定（404）です。サーバーがリクエストを拒否したので、何も実行されず、請求もされません。失敗には出力、stop reason、使用量がなく、請求された可能性のある以前の試行もないことが必要です。

HTTP 408、425、429、5xx の応答、途中で切れたりタイムアウトしたりした呼び出し、リクエストが出た後の transport 失敗、部分的な出力や使用量を伴う失敗は、すべて hold を維持します。使用量の欠落、観測者や永続化の例外、SDK の `RejectedBeforeOutput` ラベルのない status code だけでは、費用ゼロも予算の更新も証明されません。

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


ソースとバイナリの破壊的変更です。全 C++ 利用者とカスタムプロバイダーを一致した新ヘッダー/ライブラリで再コンパイルします。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、descriptor interpreter、Responses WebSocket は alias/互換 bridge なしで削除されました。SDK は alpha `0.3.5`、interface revision 6 / shared-library generation 6、out-of-line capability check を使い、安定リリースの宣言ではありません。検証済みのプラットフォームは Linux/POSIX です。Windows NTFS と macOS の実装はありますが、この SDK では検証していません。WASM provider runtime のサポートは確立していません。

Python も C++ と同じ所有 request/outcome 境界を公開します: `make_provider_request`、`Provider.prepare`、`dispatch`、`invoke`。Provider 履歴には typed part を持つ `ProviderMessage` を使い、`ChatMessage` はグラフ用の便宜的 projection として残ります。SDK 失敗は `ProviderOutcome.failure` で読み、host observer/settlement 例外は `outcome` と `cause` を保持します。コンストラクターと GIL/コールバック動作は [Python binding ガイド](python-binding.md)を参照してください。

Python SDK の vector/map getter は独立した値を返します: `ProviderMessage.parts`、`ProviderRequest.messages`、`RunConfig.provider_messages`、completion/partial の `messages` と `raw_events`、`RunResult.native_messages`、`ProviderLoopEntry.messages`、usage の `extra`/`conflicts`。スナップショットを変更し、setter があるプロパティには再代入してください。getter の結果への append は所有オブジェクトを更新しません。任意の `ProviderControls.provider`/`response_format`、`SchemaProviderDefaults.provider`、`OpenRouterRouting.max_price`、`ProviderToolResult.host` も同じ読み取り・変更・再代入の規則に従います。読み取り専用の outcome 証拠は変わりません。これは Python バインディングの規則であり、すべての C++ getter がコピーを返すという保証ではありません。

ネイティブコードが Python provider の `prepare` オーバーライドを呼ぶ際に `None` を返すと、handle を消費する前に `TypeError` が発生します。`ProviderDescriptorPolicy.identity` は raw SHA-256 digest を格納する `bytes` で、`.hex()` は表示用の変換です。保存された Python provider/graph の cause を繰り返し調べても、元の例外値と traceback を保持し、保存された例外の restore 状態を消費しません。ネイティブの入れ子になった例外変換にも適用されます。

<a id="pricing-evidence-and-the-price-catalog"></a>
<a id="pricing-evidence-and-the-model-catalogue"></a>
### 価格の証拠とモデルカタログ

Program 実行の `monetary_microunits` 予算は micro-USD（1 microunit = 1e-6 USD）を数えます。ホストが組み込み `llm_call` ノードの provider 呼び出しを価格付け broker に通すと、その呼び出しに価格が付きます。プロセス内では `bind_priced_provider_call(context)`、クラッシュに安全な再生が必要なら `SQLiteProgramProviderCallJournal::bind_priced(context, deployment)` を使います。ランタイムは各 broker に attempt の金額アカウントを `ProgramCoreProviderCallContext::monetary_account` で渡します。Python の `LocalProgramHost(node_context=..., binding_label=...)` は常に呼び出しに価格を付けます（[Python ガイド](python-binding.md#model-calling-nodes)）。すべての `RunBudget` フィールドと同じく `monetary_microunits` は実行に与えられる量で、`0` は何も与えません。子と fork の予算は親の量から切り出され、`model_tokens = 0` でモデルトークンを使った完了実行は `BudgetExhausted` と報告されます。`monetary_microunits = 0` の実行は、価格の付くすべての呼び出しを送信前に拒否します。価格付けのコードは内部用（`src/program/monetary.h`）で、ABI を約束しません。

dispatch の前に broker は、provider の descriptor ポリシーが持つ SchemaProvider のモデルカタログから準備済みリクエストに価格を付けます。それ以外は使いません。項目は、ポリシーが family、base URL、許可されたモデルに対して、id、alias、または origin の範囲で対応づけるものです。hold は、項目の価格に対する `sp::pricing::upper_bound` に、1 回の dispatch が請求しうるモデル呼び出し数（`Provider::invocation_upper_bound`）を掛けた値です。この上限は、リクエストの入力上限と出力 cap、使用 cap までのサーバーツール、OpenRouter の origin では項目の route ceiling を含みます。ポリシーが client ツールとして示す Messages のツール（`memory_*`、`bash_*`、`text_editor_*`、computer と browser の toolset）は呼び出し側が実行し、トークンだけが請求されるため、使用 cap も料金も必要ありません。入力上限は、カタログがそのモデルについて知っている最小の入力上限、つまり `input_limit` または `context_window` です。すべての入力トークンは最も高い入力料金、すべての出力トークンは最も高い出力料金で計算し、上限内の呼び出しが届きうるすべての帯にわたって適用します。OpenRouter のモデル fallback（`models`）は最も高い候補を予約します。上限を決められない呼び出しや、hold が残り予算に収まらない呼び出しは、リクエストや journal marker ができる前に拒否されます。ノードは未送信を示す合成 provider 失敗で失敗し、実行の `ProgramFailure` が理由を示します。`failure.witness["monetary"]` には origin、model、項目、上限、カタログの digest が入ります。

| コード | ステータス | 拒否の理由 |
|---|---|---|
| `P_MONEY_UNKNOWN_ORIGIN` | `Failed` | provider に許可された descriptor がなく、カタログが分からない |
| `P_MONEY_UNKNOWN_MODEL` | `Failed` | カタログが base URL のそのモデルに価格を付けていない、またはその OpenRouter fallback のいずれかに価格を付けていない |
| `P_MONEY_UNMODELED` | `Failed` | 項目にリクエストの service tier の価格がない、またはサーバーツールや hosted tool のいずれかの料金（`tool:<type>`）がない、あるいはリクエストが OpenRouter の plugin を使う |
| `P_MONEY_NO_OUTPUT_CAP` | `Failed` | リクエストに出力トークン cap がない |
| `P_MONEY_NO_INPUT_BOUND` | `Failed` | カタログがそのモデルの入力上限もコンテキストウィンドウも知らない |
| `P_MONEY_NO_UPPER_BOUND` | `Failed` | サーバーツールや hosted tool に使用 cap がない（Messages の `max_uses`、Responses の `max_tool_calls`。Gemini の組み込みツールと、ポリシーが `max_uses` を認めない Messages の型である `mcp_toolset`、`tool_search_tool_*`、`code_execution_*` などにはない）、router alias のように OpenRouter の項目に route ceiling がない、または route ceiling を `provider.max_price` として固定できない |
| `P_MONEY_BUDGET` | `BudgetExhausted` | hold が残りの `monetary_microunits` に収まらない（請求額が予算を超えて終わった実行も同じコードで、その場合は `failure.witness["monetary"]` に `granted`、`used`、`over_by` が入る） |

結果が出ると hold はちょうど 1 回精算され、attempt の `ProgramResult::monetary_records()`（Python では `result.monetary_records`）に呼び出しごとに `ProgramMonetaryRecord` が 1 つ追加されます。フィールドは `call_id`（journal の論理呼び出し slot）、`operation_id`、`node`、`origin`（base URL）、`model`、`evidence`、`microunits`、`reserved_microunits`、`basis`、`catalog_version`、`catalog_digest`、`conservative`、`may_undercount`、`replayed`、`reason`、`pricing`（項目、その価格行、service tier、予約したツールの使用、推定の前提）です。`ProgramUsage::monetary_microunits` は attempt の請求額とまだ開いている hold の合計です。

| 証拠 | 条件 | 金額 |
|---|---|---|
| `reported` | 応答に provider 自身の費用がある：OpenRouter の `usage.cost` で、SDK が nano-USD 単位の `Usage::provider_cost` にパースします。BYOK なら報告された upstream の請求額を加えます。 | 報告額を整数 microunit に切り上げた値。記録には hold の算定に使ったカタログと行も残ります。 |
| `estimated` | 報告費用のない最終で一貫した usage。 | `sp::pricing::estimate` が hold に価格を付けたときのカタログのスナップショットから求めた値に、予約したサーバーツール料金を cap の分だけ加え、呼び出しごとに 1 回切り上げ。Messages の 1 時間キャッシュ書き込みと Gemini の画像トークンはそれぞれ固有の料金で、OpenRouter の呼び出しは route ceiling で請求されます。記録には `sp-model-catalogue/1`、カタログの digest、価格を付けた行が残ります。 |
| `held` | 結果が分からない（transport 例外、キャンセル、タイムアウト）、すでに始まった処理の後に失敗した可能性がある（HTTP 408、425、429、5xx の応答、途中で切れたストリーム、リクエストが出た後の transport 失敗）、SDK が transport 試行を再送した、または usage が部分的か一貫しない。 | hold。 |

プロバイダーが何も受け付けなかったことを証明する失敗は hold を解放し、記録を残しません。リクエストが送られなかった場合と、プロバイダーが完了した HTTP 4xx 応答（408、425、429 を除く）でリクエストを拒否した場合です。金額の hold もモデルトークンの reservation も請求されません。hold を超える請求額（BYOK の upstream 費用のように、固定した上限が抑えない費用が報告費用に含まれるときに起こりえます）は報告どおりに請求され、請求額が予算を超えた実行は `P_MONEY_BUDGET` とともに `BudgetExhausted` で終わります。並行呼び出し（`ng.all`、`ng.race`、`ng.quorum`、並列ノード）は 1 つのアカウントに予約するので、収まらない member は拒否されます。ノードの再試行は自分の effect namespace（`task#a<k>`）で呼び出すため、attempt ごとの呼び出しは新しい呼び出しで、再び請求されます。

`bind_priced` は呼び出しの記録を journal 行に書きます。dispatched marker と一緒に held 記録を、結果と一緒に精算済み記録を書きます。同じ Program attempt がプロセス停止前に記録した呼び出しは、復旧した attempt がその operation をバインドしたときに、Core が再び呼び出すかどうかにかかわらず 1 回だけ請求され、`replayed` と記されます。execution lease の期限切れで復旧する define() のみの実行も含みます。以前の attempt が記録した呼び出しは、その attempt の結果が残り予算にすでに精算しているので、後の attempt が結果を再生しても再び請求されません。recorded replay は金額を使いません。

フォークは、一時停止した呼び出しのプロバイダー呼び出しを、永続的なプロバイダー呼び出しのjournal（`SQLiteProgramProviderCallJournal::bind` または `bind_priced`）からだけリプレイします。プロセス内のブローカーにはjournalがないため、フォークで再び実行されるノードは、先に行った呼び出しを再送し、再課金します。`core_provider_call_resolver` がjournalのブローカーを返すホストは、`RuntimeConfig::core_provider_calls_journaled = true` を設定します。Pythonの `LocalProgramHost` は、`sqlite_directory` があるときにそうします。このフラグが `false` で、一時停止した試行が、止まっているオペレーションで価格付きのプロバイダー呼び出しを記録していた場合（コピー元の一時停止した `ProgramResult::monetary_records()`）、`ProgramRuntime::fork` は返すハンドルに `ProgramNotice` を1つ付けます。これは `ProgramHandle::notices()`（Pythonでは `handle.notices`）で読みます。`code` は `P_FORK_PROVIDER_CALLS_NOT_JOURNALED`、`witness` は `{source_run_id, source_attempt, operation_id, calls: [{call_id, node, model, microunits}], microunits}` です。通知はエラーではなく、フォークが送るものも課金するものも変えません。列挙された呼び出しのうちどれをフォークが再実行するかは分かりません。一時停止したステップで終わったノードの呼び出しは、そのステップに保存された書き込みからリプレイされるためです。通知は実行とともには保存されないので、`reconnect()` や `resume()` が返したハンドルにも、同じ実行の `fork()` を繰り返して得たハンドルにもありません。フラグが決めるのは通知を出すかどうかだけで、ブローカーにjournalを書かせるわけではありません。プロバイダー呼び出しを価格付きブローカー経由で送らないホストは、価格付きの呼び出しを記録しないので、通知も出ません。

OpenRouter の origin では、broker はリクエストを送信用に準備する前に route ceiling をリクエストに固定します。OpenRouter の `provider.max_price` には、項目の `ceiling_input` が `prompt` に、`ceiling_output` が `completion` に、100万トークンあたりの正確な10進 USD で入ります。ceiling が100万トークンあたり 0.44 USD と 1.32 USD なら、本文は `"provider":{"max_price":{"prompt":0.44,"completion":1.32}}` を含みます。fallback がある場合、固定値はフィールドごとに最も高い候補の ceiling です。OpenRouter はその価格以下で掲載された endpoint にだけルーティングし、該当がなければリクエストを失敗させるので、予約が endpoint のトークン価格を抑えます。ホストが `ProviderControls::provider` または `SchemaProvider::Defaults::provider` ですでに送る `max_price`（リクエスト自身の値が既定値を置き換えます）は、より低いフィールドで保たれ、broker が固定しない金額（`image`、`audio`、`request`）はホストの値のままです。固定されるのは Chat Completions、Responses、Messages のリクエストで、ほかの origin には `max_price` は付きません。価格は会話をユーザーメッセージ1つに置き換えたリクエストの probe から求め、probe は送信しません。リクエストは固定した状態で一度だけ準備されるので、provider journal は実際に送ったリクエストの digest を記録し、再生はそれと一致します。固定値のもとでキャプチャされた native history を持つ次のターンも同じ固定値を載せます。SDK policy がその base URL で OpenRouter routing を認めない、またはレートが `max_price` の認める 2^53 nanoUSD を超えるために route ceiling を固定できない呼び出しは `P_MONEY_NO_UPPER_BOUND` で拒否されます。

価格はホストが供給するデータです。組み込みの SchemaProvider カタログには価格が一つもないので、ホストがモデルに価格を付けるカタログを持つポリシーを provider に渡すまで、予算付きの実行は価格の付くすべての呼び出しを拒否します。C++ では `sp::descriptor::load_policy(family, resources, catalogue)` または `DescriptorPolicy::with_catalogue` を使い、ホストのレイヤーは `sp::descriptor::merge_catalogues` で統合します。Python では `load_provider_policy(family_json, resource_json, model_catalogue_json)` が、渡されたカタログを組み込みカタログの上に重ねます。項目の `pricing` は、単位を単位あたりの10進 USD に対応づけます（SchemaProvider の USAGE、"Pricing by the catalogue"）：
- `input`、`output`、`cache_read`、`cache_write`、`cache_write_1h`、`reasoning`、`output_image`、`request`。
- `ceiling_input`、`ceiling_output`、`ceiling_request`：OpenRouter の route ceiling。モデルの endpoint のうち最も高い値で、broker が `provider.max_price` として固定します。
- `<unit>/<tier>`：service tier の価格。`default`、`standard`、`standard_only` 以外の tier のリクエストは、その単位だけで価格が付きます。
- `<unit>+<tokens>`：long-context の帯。キャッシュを含む入力トークンがその数を超えると、帯の単位が呼び出し全体に価格を付けます。
- `tool:<type>`：サーバーツールまたは hosted tool を 1 回使うごとの料金。`"0"` は無料であることを宣言します。既定で無料のツールはありません。

origin の範囲（`origins`）を持つ項目は、それらの origin への呼び出しにだけ価格を付けます。同じモデルが 2 つの origin にある場合は、そのモデルを alias として名指しし、もう一方の origin に範囲を限った、独自の id を持つ 2 つ目の項目が必要です。[`tests/fixtures/pricing/model_prices.json`](../tests/fixtures/pricing/model_prices.json) は、0.15 の価格カタログをこの形で保持しています。2026-10-11 に取得した価格を持つテスト fixture であり、価格の出どころではありません。

NeoGraph は実行中に価格を取得も更新もしません。ホストは、たとえば OpenRouter の一覧から `sp::transport::refresh_catalogue` で、カタログを実行とは別の経路で更新し、それで作った provider をバインドします。各呼び出しは、カタログの digest と価格を付けた行を journal に記録し、dispatch された呼び出しは hold の価格を付けたスナップショットから精算され、復旧または fork された実行は journal に記録された金額を保ちます。0.16.0 より前に journal に記録されたレコードはバイトが変わりません。それらは `openrouter` のような価格カタログの origin、カタログの version と digest を名指しし、`pricing` はありません。

組織の費用レポート（OpenAI の organization costs endpoint、Anthropic の cost report）は admin key が必要で、時間帯ごとに集計され、遅れて届くため、ランタイムは使いません。ホストは後から `reported` と `estimated` の記録をこれらと照合できます。

---

<a id="3-tool-interface"></a>
## 3. Tool インターフェース
**ヘッダー:** `<neograph/tool.h>`
**名前空間:** `neograph`
LLM が呼び出せるツールの抽象インターフェースです。エージェントに関数を公開するにはこれを実装します。
> **カスタム Tool サブクラスを書く場合:** 同期の `Tool` と非同期の `AsyncTool` のどちらを継承するかは、
> [`ASYNC_GUIDE.md` §9.6](ASYNC_GUIDE.md#96-tool-vs-asynctool) で、
> 同期の `Tool` と非同期の `AsyncTool` のどちらを継承するか確認してください。2 つは
> 相互排他的なので、どちらか一方を選びます。
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

| メソッド | 戻り値 | 説明 |
|--------|---------|-------------|
| `get_definition()` | `ChatTool` | パラメーター用 JSON Schema を含むツールメタデータを返す |
| `execute(arguments)` | `std::string` | 解析済み JSON 引数でツールを実行し、LLM に返す文字列結果を返す |
| `get_name()` | `std::string` | このツールの固有識別子 |
**実装例:**
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
### ツール失敗の契約

モデルのツール呼び出しを実行するすべての経路が同じ規則を適用します。1 回の呼び出しまたは batch を処理する `dispatch_tool_calls`、`tool_dispatch` ノード(Program の Core `tool_dispatch` を含む)、`llm::Agent::run` と `run_stream`、plan-and-execute の executor、deep-research の researcher が対象で、C++、MCP、Python のツールでも同じです。

| 失敗 | 結果 |
|------|------|
| `HookBoundaryBlocked`: 必須 Hook が失敗した、タイムアウトした、実行できなかった、またはツールコードがそれを送出した | その例外で実行が停止し、invocation id と state はそのまま保たれます。グラフ実行も `llm::Agent` と同じく、それを `NodeExecutionError` で包まずにそのまま送出します。ノードのリトライポリシーはこれをリトライしません。 |
| 実行の `CancelToken` がキャンセルされた(ノード試行のトークンを含む) | `graph::CancelledException` で実行が停止します。実行中の MCP 呼び出しや `GrpcRemoteTool` の RPC はすぐに止まり、遅れて届いた応答は破棄されます。 |
| `ToolGate` またはツールコードからの `NodeInterrupt` | 実行が一時停止します。`llm::Agent` はそれを呼び出し元に送出します。 |
| 未知のツール、またはゲートの `Deny` | ステータス `rejected` のツール結果。 |
| 呼び出しが実行の期限(`RunContext::deadline`、`ToolExecutionContext::deadline`)以降に開始されることになる | ステータス `expired`、エラー `run deadline elapsed before the tool started` のツール結果。ツールもその Hook も実行されません。 |
| ツールのサービスが実行前に拒否したリクエスト: MCP の JSON-RPC エラー `-32700`、`-32600`、`-32601`、`-32602`、`NG.InvalidRequest` の目印が付いた MCP の `isError` 結果、gRPC `ToolService` のステータス `NOT_FOUND` または `INVALID_ARGUMENT`、あるいはツールコードが送出した `ToolRequestRejected` | ステータス `failed`、`retryable` と `effect_uncertain` がどちらも false のツール結果。何も実行されていないため、実行ポリシーによりません。 |
| ツールコードまたはその検索から送出されたそれ以外の例外(C++、Python、`-32603` など、それ以外の MCP の `isError` 結果または JSON-RPC エラー)、解析できない引数、またはポリシーの `output_limit_bytes` を超える出力 | ステータス `failed` のツール結果。呼び出しが開始済みで、実行ポリシーが効果の発生を否定しない(`ReadOnly` でも `ExternalRead` でも `Idempotent` でもない)場合は `reconciliation_required`。 |
| `timed_out`、`expired`、`killed`、`input_required` などの型付きコントローラーステータス | そのステータスのツール結果。実行の期限またはリクエストタイムアウトで止まった MCP 呼び出しや `GrpcRemoteTool` の RPC は `timed_out` です。 |
| `ToolEffectBroker` の下で効果が不確定なツール結果 | ペイロードの `kind` が `tool_effect_reconciliation` である `NodeInterrupt` で実行が一時停止します。 |

最初の 3 行がモデルに届くことはありません。ツール結果は `tool_status` を設定し、失敗した結果は 1 回の呼び出しでも batch でも `{"error", "status", "retryable", "effect_uncertain"}` を持ちます。batch では他の呼び出しが先に最後まで実行され、複数の呼び出しが実行を停止させる場合は、キャンセルが Hook のブロックより、Hook のブロックが割り込みより優先されます。ツールコードが送出した `NodeInterrupt` があっても兄弟の呼び出しは実行され、再開すると batch 全体が再び実行されます。何かが実行される前に確認するには `ToolGate` を使ってください。実行の期限(`RunMetadata::deadline`)は Hook とプロバイダー呼び出し、そして受け入れキューでの待機を含むすべてのツール呼び出しの開始を制限し、ツール自体の上限はポリシーの `execution_timeout` と `queue_timeout` です。ツールコードが開始後に送出したシステムエラーは受け入れの判定ではなくツールの失敗(`failed` または `reconciliation_required`)です。ホストの受け入れはリソースアービターと同じく、`rejected`(キューが満杯、容量なし)または `expired`(キューの待機時間切れ)で拒否します。開始されなかった呼び出し(`cancelled_before_start`、`expired`、`rejected`: `tool_call_not_started()`)は効果を生んでいません。永続的な `SQLiteToolEffectBroker` を含むすべての `ToolEffectBroker` は、その呼び出しをそのステータスのまま、終端で効果が不確定でない結果として返し、スロットを解放します。そのため、その呼び出しは照合のために実行を一時停止させることも、同じスロットの後の呼び出しを妨げることもありません。

**拒否されたリクエストは、結果が分かっている場合です。** `ToolRequestRejected`(`<neograph/tool_execution.h>`)は、ツールが自分の呼び出しは実行されなかったと分かるときに送出する例外です。`MCPError` のように独自の基底クラスを持つ例外型は、代わりに `ToolRequestRefusal` インターフェースを実装します。`retryable()` は、同じリクエストを再送すれば成功しうるかどうかを表します。`MCPTool` は、サーバーがツールを実行する前にリクエストを拒否する JSON-RPC エラー(`-32700`、`-32600`、`-32601`、`-32602`)を、拒否でもある `MCPError` として送出するので、`MCPError` を捕捉する呼び出し元は、コードとデータをそのまま受け取れます。`_meta["io.neograph/failure"].code` が `NG.InvalidRequest` の `isError` 結果は `ToolRequestRejected` として送出します。`MCPServer` は、ツールの入力スキーマに合わない引数をこの形で報告します。`GrpcRemoteTool` は、ステータス `NOT_FOUND` と `INVALID_ARGUMENT` に対して `ToolRequestRejected` を送出します。コントローラーは拒否を `failed` にします。同じリクエストはまた拒否され、モデルがリクエストを直す必要があるため `retryable` は false、`effect_uncertain` も false です。それ以外の失敗は結果が分からない(サーバーがツールを実行した後に失敗した可能性があります: 通常の `isError` 結果、`-32603`、`-32000` などのサーバーエラー)ので、ツールのポリシーが効果を除外しない限り `reconciliation_required` のままです。ノード失敗の分類器は拒否を通常の失敗として扱い、`retryable()` が false なら再試行しません。

**ツールの Hook。** すべての経路が、呼び出しの開始前に `BeforeToolExecution` イベントを 1 つ、確定後に `AfterToolExecution` イベントを 1 つ、同じデータ(`tool_call_id`、`tool_name`、`thread_id`、`request_id`、そして前では `arguments`、後では `status` と `result`)で、実行の owner scope と run id の下で発行します。`HookRuntime` は、直接のディスパッチでは `ToolExecutionContext` のもの、Agent では `llm::Agent::set_hook_runtime` または `run` に渡したコンテキストのもの、グラフ実行ではエンジンのもの(`EngineConfig::hook_runtime`、`GraphEngine::set_hook_runtime`)です。ただし `RunResources::hook_runtime` があれば、その実行ではそれに置き換わります。サブグラフのツール呼び出しは、親の実行と自身のエンジンの両方に `HookRuntime` があれば両方を通り、親の Hook が先に実行され(`HookRuntime::compose`)、どちらかがブロックすると呼び出しはブロックされます。Program Core のツール呼び出しは Program ホストのもの(`RuntimeConfig::hook_runtime`)を使います。呼び出しにタスクスロット(`ToolExecutionContext::effect_task_id`、すべてのグラフ実行で設定される)がある場合、イベントは `task_id` と `call_ordinal` も持ちます。これは呼び出しの永続的な識別で、効果の識別と同じです(実行、スレッド、`s3:act#a1` のようにノードの試行を含むタスク、batch 内での呼び出しの序数)。`request_id` はこの識別から導かれます。再開や再生が batch を再び実行しても、その識別ですでに配信されたイベントは再び配信されません(`HookRuntime::emit_once_async`、Hook ジャーナル自身の記録を使用)。必須 Hook が通過しなかった(失敗、タイムアウト、またはキャンセルにより実行を停止させた)発行は配信ではなかったので、次の発行で Hook が再び実行されます。拒否された、未知の、または期限切れの呼び出しは何も開始せず、Hook も発行しません。最後の試行が失敗した必須 Hook は `failed` を報告し、期限が過ぎたかターゲットがタイムアウトしたときだけ `timed_out` を報告します。タイムアウトした Hook ターゲットは最終状態です。タイムアウトしたターゲットはまだ実行中かもしれず、2 回目の試行がその横で並行して実行されうるため、Hook の試行回数やターゲットのポリシーの `retry_max_attempts` が残っていても、ランナーもツール実行コントローラーもターゲットを再び開始しません。まったく実行できない Hook 境界(イベントを帰属させる owner scope がない、ジャーナルが使えない)は、ツール、チェックポイント、プロバイダー、Program のイベントのいずれでも state `failed` で閉じた側に失敗します。Hook の実行中に実行がキャンセルされると、実行はキャンセルとして停止します。

**テキスト。** 境界を越えるテキストは正しい UTF-8 です。不正な形式の最大部分列 1 つごとに U+FFFD になり、これは Unicode と WHATWG の置換方法です。この規則はツール結果と失敗メッセージ、Hook のレシート、`json::dump`(不正な文字列を含むドキュメントは以前は空文字列にシリアライズされていました)、MCP と gRPC の応答、そして Python バインディングに適用されます。Python では C++ から読んだテキストも同じ方法でデコードされ、`str` が UTF-8 になるときは、surrogate-escape されたバイト(U+DC80 から U+DCFF、`errors="surrogateescape"` が作るもの)は元のバイトに戻り、それ以外の単独サロゲートは U+FFFD になります。

**例外を送出するファサード。** `ToolExecutionController::execute_async` は成功すると出力を返し、`cancelled_before_start` と `cancellation_requested` には `graph::CancelledException` を、それ以外のすべてのステータスには `ToolExecutionResult` 全体を持つ `ToolExecutionError` を送出します。ノード失敗の分類器はこれを読み、効果が不確定なら `NG.EffectUncertain`、タイムアウトまたは期限切れの呼び出しなら `NG.Timeout`、それ以外は Ordinary とします。ツールのサービスが実行前に拒否した呼び出し（`ToolRequestRefusal` のときに設定される `ToolExecutionResult::refused`）は、拒否そのものがノードに届いたときと同じく、`retryable` が再試行で成功しうると示さない限りリトライされません。

<a id="protocol-failure-mapping"></a>
### プロトコル失敗のマッピング

NeoGraph が MCP、gRPC、A2A、ACP でツール、グラフ、エージェントを提供するとき、すべての失敗分類は、ノード失敗の分類器が与える安定したコード(`graph::classify_failure`)で名付けられた、互いに区別され文書化された形をワイヤー上でも保ちます。実行制御シグナルが、モデルの読めるツール結果になることはありません。ワイヤーを越えるのは公開メッセージだけです。失敗の `public_message`(`NodeFailureError` 由来)、または分類の固定の説明であり、パスやペイロードや秘密を含みうる例外テキストは決して越えません。

| 分類(コード) | MCP `tools/call` (`MCPServer`) | gRPC `GraphService` | gRPC `ToolService.CallTool` | A2A タスク(`A2AServer`) | ACP `session/prompt` (`ACPServer`) |
|------|------|------|------|------|------|
| クライアントによるキャンセル(`NG.Cancelled`): `notifications/cancelled`、gRPC のキャンセル | どのトランスポートでも応答なし: stdio(`run`) は何も送らない。HTTP(`MCPHttpServer`) は POST を空の `text/event-stream` で終える。応答 sink を読むトランスポートは代わりに `NoResponseSink` で id を受け取る | `CANCELLED` | `CANCELLED` | `CancelTask`: `CANCELED`。stream もこの状態で閉じる | `session/cancel`: `stopReason: "cancelled"` |
| サーバーによるキャンセル(`NG.Cancelled`): `stop()`、またはハンドラー自身の処理 | JSON-RPC エラー `-32000`(`stop()`)または `-32603`、`data.code` | `CANCELLED` | `CANCELLED` | `CANCELED` | JSON-RPC エラー `-32800`、`data` `{code, retryable, retryable_by_class}` |
| Hook のブロック(`NG.HookBlocked`) | JSON-RPC エラー `-32010`、`data` `{code, invocation_id, state}` | `FAILED_PRECONDITION`、Hook がタイムアウトしたときは `DEADLINE_EXCEEDED`。ErrorInfo metadata `invocation_id`、`state` | `GraphService` と同じ | 完了した node がなければ `REJECTED`、それ以外は `FAILED`。`metadata["neograph/failure"]` `{code, retryable, retryable_by_class}` | JSON-RPC エラー `-32603`、`data` `{code, retryable, retryable_by_class}` |
| 割り込み(`NG.Interrupted`) | JSON-RPC エラー `-32011`、`data` `{code, reason, value}` | 一時停止した実行: `OK`、`interrupted`、`interrupt_node`、`interrupt_value_json`(ストリームの FINAL ペイロード: `interrupt_value`) | `FAILED_PRECONDITION`、ErrorInfo metadata `reason`、`value` | `INPUT_REQUIRED`: 理由を text、ペイロードを data part、`metadata["neograph/interrupt"]` `{node, checkpointId}`。後続の message が実行を再開 | 同じターン内の `session/request_permission`。その回答で実行を再開。アダプターやクライアントが回答できないときは `NG.Interrupted` を持つ JSON-RPC エラー `-32603` |
| 通常(`NG.TaskFailed`、作成者コード) | `isError: true` と `_meta["io.neograph/failure"]` `{code, retryable, retryable_by_class}` を持つ `result` | `UNKNOWN` | `error` と `error_code` を持つ `OK` | `FAILED`。公開メッセージを status message に、`metadata["neograph/failure"]` `{code, retryable, retryable_by_class}` | JSON-RPC エラー `-32603`。公開メッセージを message に、`data` `{code, retryable, retryable_by_class}` |
| タイムアウト(`NG.Timeout`) | 通常と同じで、それぞれのコードを持つ | `DEADLINE_EXCEEDED` | 通常と同じで、それぞれのコードを持つ | 通常と同じで、それぞれのコードを持つ | 通常と同じで、それぞれのコードを持つ |
| プロバイダー失敗(`NG.ProviderFailed`) | 通常と同じで、それぞれのコードを持つ | `UNAVAILABLE` | 通常と同じで、それぞれのコードを持つ | 通常と同じで、それぞれのコードを持つ | 通常と同じで、それぞれのコードを持つ |
| 予算(`NG.BudgetExhausted`) | 通常と同じで、それぞれのコードを持つ | `RESOURCE_EXHAUSTED` | 通常と同じで、それぞれのコードを持つ | 通常と同じで、それぞれのコードを持つ | 実行のモデルトークン予算を超えたときは `stopReason: "max_tokens"`、それ以外は通常と同じ |
| 安全または構成(`NG.SafetyBlocked`、`NG.ConfigError`) | 通常と同じで、それぞれのコードを持つ | `FAILED_PRECONDITION` | 通常と同じで、それぞれのコードを持つ | 通常と同じで、それぞれのコードを持つ | `NG.SafetyBlocked` は `stopReason: "refusal"`、`NG.ConfigError` は通常と同じ |
| 論理(`NG.LogicError`、`NG.BarrierStalled`) | 通常と同じで、それぞれのコードを持つ | `INTERNAL` | 通常と同じで、それぞれのコードを持つ | 通常と同じで、それぞれのコードを持つ | 通常と同じで、それぞれのコードを持つ |
| コミット済みの効果(`NG.EffectCommitted`) | 通常と同じで、それぞれのコードを持つ | `ABORTED` | 通常と同じで、それぞれのコードを持つ | 通常と同じで、それぞれのコードを持つ | 通常と同じで、それぞれのコードを持つ |
| 不確定な効果、不明、失敗出口(`NG.EffectUncertain`、`NG.UnknownFailure`、出口のコード) | 通常と同じで、それぞれのコードを持つ | `UNKNOWN` | 通常と同じで、それぞれのコードを持つ | 通常と同じで、それぞれのコードを持つ | 通常と同じで、それぞれのコードを持つ |
| ステップ上限(`NG.StepLimit`): `RunConfig::max_steps` | なし | `OK`、`max_steps_exhausted` | なし | `FAILED`、コード `NG.StepLimit` | `stopReason: "max_turn_requests"` |
| 不正なリクエスト(`NG.InvalidRequest`) | JSON-RPC エラー `-32602`。ツールの入力スキーマに合わない引数は、`isError: true` と `_meta["io.neograph/failure"]` `{code: "NG.InvalidRequest", retryable: false, retryable_by_class: false}` を持つ `result` で、NeoGraph のクライアントはこれを何も実行されなかった印として読みます | `INVALID_ARGUMENT`。実行するグラフがないときは `FAILED_PRECONDITION`(`NG.NoGraph`) | `INVALID_ARGUMENT` | `google.rpc.BadRequest` の field violation 付きの JSON-RPC エラー `-32602`。不正な envelope や batch は `-32600` | JSON-RPC エラー `-32602` |
| 未知のツール(`NG.ToolNotFound`) | JSON-RPC エラー `-32602`、`data.code` | なし | `NOT_FOUND` | なし | なし |
| 容量超過(`NG.Capacity`) | JSON-RPC エラー `-32000`、`data.capacity` | `RESOURCE_EXHAUSTED` | なし | JSON-RPC エラー `-32603`、ErrorInfo reason `RESOURCE_EXHAUSTED`、domain `neograph` | JSON-RPC エラー `-32603`、`data.reason` `overloaded` |

`retryable` はすべてのプロトコルで `FailureVerdict::retryable` で、同じリクエストをもう一度送れば成功しうるかを表します。ノードの再試行判定([ノード失敗の契約](#node-failure-contract))から始まり、プロバイダー失敗(`NG.ProviderFailed`)でも `true` です。ノードの再試行ループはこれをプロバイダーランタイムに任せますが、新しいリクエストは通りうるからです。ノードの再試行ループがその失敗にノードの試行を使い切ると(`kind` が `exhausted`)、`false` になります。再試行ポリシーのないノードも試行が 1 回なので、これに当たります。リクエストをもう一度送っても、エンジンがすでに試したことを繰り返すだけだからです。実行の期限や再試行の上限のためにループが再試行をやめた失敗は `true` のままです。新しい実行には新しい予算があるからです。`retryable_by_class`(`FailureVerdict::retryable_by_class`)は、試行が残っていたかどうかにかかわらず判定だけを表します。試行が 1 回のグラフを包む場合のように、エンジンの外で再試行する呼び出し元のためのものです。試行を使い切った一時的な失敗は `retryable: false, retryable_by_class: true`、決定的な失敗は `false, false` です。A2A はコードと 2 つのフラグを両バージョンとも task の `metadata["neograph/failure"]` に、ACP はエラーの `data` に、MCP サーバーは `_meta["io.neograph/failure"]` に、gRPC は ErrorInfo の metadata に `"true"` または `"false"` として載せます。A2A だけにあるリクエストエラーは [A2A エラーマッピング](#a2a-error-mapping) にあります。

OK でないすべての gRPC ステータスは、`grpc-status-details-bin` trailer に `google.rpc.ErrorInfo` を 1 つ持つ `google.rpc.Status` を載せます。`reason` は AIP-193 に従い、コードから導いた 63 文字以下の UPPER_SNAKE 定数です(`NG.HookBlocked` は `HOOK_BLOCKED`、ほかに `TIMEOUT`、`TOOL_NOT_FOUND`、`INVALID_REQUEST`、`NO_GRAPH`、`CAPACITY`、`BACKEND_UNAVAILABLE`、`BARRIER_STALLED`。作成者コードは `TASK_FAILED` のようにそのクラスの reason になります)。`domain` は `neograph` で、`metadata` には `code`(安定したコードそのもの)、`retryable`、`retryable_by_class` と上のフィールドが入ります。`RunGraphResponse.error` は非推奨で、公開メッセージだけを持って 1 リリースの間残ります。OK でない単項ステータスには応答本体が載りません。`RunGraphStream` は失敗をペイロードが `{error, code}` の `DEBUG` イベントで報告し、その後、単項呼び出しと同じステータスで終わります。`CheckpointService` はバックエンドの失敗に「チェックポイントなし」、空のリスト、成功した削除ではなく `UNAVAILABLE`(`NG.BackendUnavailable`)で、形式の不正な `SaveCheckpoint` には `INVALID_ARGUMENT` で応答します。

`RunGraphStream` は `RunGraphRequest.stream_mode`、つまり `StreamMode` のビットに従います(0 はすべてのイベントをストリーミングし、未知のビットは無視します)。状態スナップショットは `VALUES` イベント、ノードのチャネル書き込みは `UPDATES` イベント、割り込みとノードエラーは `DEBUG` イベントです。ノードエラーの `error` は `node failed` で、例外テキストは決して含みません。`RunGraphRequest.thread_id` は必須で、空なら `INVALID_ARGUMENT` で拒否されます。他の言語で書いた `CheckpointService` は、`RequiresManagedBudget` を含め `proto/neograph.proto` のすべての RPC を提供しなければなりません。エンジンは新規実行と再開される実行のたびにこれを問い合わせ、`UNIMPLEMENTED` なら実行を失敗させます。予算を管理しないサーバーは `required = false` を返します。NeoGraph の `CheckpointService` は JSON ではなく `Checkpoint` を保存するため、そのまま保持できない `checkpoint_json`(知らないトップレベルのメンバー、または自身より新しい `schema_version`)は、一部を捨てる代わりに `INVALID_ARGUMENT` で拒否します。`channel_values` と `metadata` は送られたとおりに保持します。

`run_server`、`run_tool_server`、`run_checkpoint_server` は標準のヘルスサービス `grpc.health.v1.Health` に応答します。呼び出しを受け付けている間はサーバーとそのサービスについて `SERVING`、停止を始めると `NOT_SERVING` です。`ServeOptions`(`<neograph/grpc/serve_options.h>`)を受け取るオーバーロードは、`ServeOptions::stop` が要求されると処理中の呼び出しに `shutdown_grace` を与えてから停止し、バインドしたポートを `on_serving` に通知します(そのためアドレスにポート 0 を指定できます)。サーバーリフレクションはホストが選びます。`gRPC::grpc++_reflection` をリンクし、サーバーの起動前に `grpc::reflection::InitProtoReflectionServerBuilderPlugin()` を呼ぶと、gRPC が組み込みサーバーを含むプロセスのすべてのサーバーにリフレクションを追加します。TLS、認証、メッセージサイズの上限が必要なら、`make_graph_service()`、`make_tool_service()`、`make_checkpoint_service()` を独自の `grpc::ServerBuilder` に登録してください。gRPC ヘッダーは gRPC 自身のヘッダーを含むため、`using namespace neograph;` を使うコードは `neograph::grpc::GrpcRemoteTool` と書きます。そこでは修飾なしの `grpc::` があいまいになります。

NeoGraph 自身のクライアントは呼び出しを制限します。MCP クライアントは、キャンセルまたはタイムアウトで待つのをやめたリクエストについて(`initialize` を除き)`notifications/cancelled` を送り、遅れた応答を破棄します。`GrpcRemoteTool`(`set_call_timeout`、既定 60 秒)と `GrpcCheckpointStore`(`set_rpc_timeout`、既定 30 秒)はすべての呼び出しに期限を設け、実行の cancel token はリモートツールの RPC をキャンセルします。これらのクライアントはリモートのコードをローカルの制御シグナルに戻しません。リモートの Hook ブロック(MCP `-32010`、`ErrorInfo` のコードが `NG.HookBlocked` である gRPC ステータス)とリモートの割り込み(`-32011`、`NG.Interrupted`)はリモートの実行のものです。`MCPTool` と `GrpcRemoteTool` は、そのコードとリモートの公開メッセージを持ち、リトライされない通常の失敗である `graph::RemoteControlFailure` を送出するので、ローカルの実行は失敗したツール結果とともに続きます。リモートサーバーからのそれ以外の MCP JSON-RPC エラー、gRPC のツールエラーやステータスは、呼び出し元にとって通常のツール失敗です。ただし、リモートが実行前に拒否したリクエスト(MCP の `-32700`、`-32600`、`-32601`、`-32602` と `NG.InvalidRequest` の目印が付いた結果、gRPC の `NOT_FOUND`、`INVALID_ARGUMENT`)は、効果が不確定でない失敗したツール結果になります([ツール失敗の契約](#tool-failure-contract))。エージェントのリクエストに ACP クライアントが JSON-RPC エラーで答えると(`ACPServer::call_client`、`ACPClient`)、それは `acp::ACPClientError` で、メソッド、コード、クライアントのメッセージと `data` を持ちます。リクエストそのものが成功しえないことを示すコード(`-32700`、`-32600`、`-32601`、`-32602`、`-32002` resource not found、`-32000` authentication required)なら `acp::ACPClientRefusal` になります。これはリトライされず、ツールの中では効果が不確定でない失敗した結果です。`-32603` を含むそれ以外のコードとタイムアウトは通常の失敗です。TODO(後の段階): リモートの割り込みを、人が答えるネイティブな一時停止としてローカルの実行に伝えます。

<a id="node-failure-contract"></a>
### ノード失敗の契約

ノードが例外を送出すると、そのリトライポリシーがノードを再実行してよいか、また失敗を失敗ハンドラーへルーティングしてよいかを、1 つの分類器が決めます。分類器は原因の連鎖全体(`NodeExecutionError` の原因、`std::nested_exception`、ノードが実行したサブグラフの失敗)をたどり、連鎖のどこかで見つかった最も深刻な分類が結果になります。上のツール失敗の契約も同じ分類器を使います。そのため、ツールの原因の連鎖のどこかに実行制御のシグナルがあれば実行は停止します。たとえば、ツールが実行したグラフの失敗に含まれる Hook のブロックがそれに当たります。

| 分類(深刻度の低い順) | コード | 発生元 | リトライ | ルーティング |
|------|------|------|------|------|
| Ordinary | `NG.TaskFailed`、`NG.SubgraphFailed` または作成者コード | `std::runtime_error` とそのサブクラス、`NodeFailureError`、Python の例外、根本原因が Ordinary のサブグラフの失敗 | する。作成者がリトライ不可とした場合と、リクエストが拒否された場合(そう示す `ToolRequestRefusal`、たとえば `acp::ACPClientRefusal`)を除く | 可 |
| Timeout | `NG.Timeout` | `attempt_timeout_ms` を超えてキャンセルされ、止まった試行（`NodeAttemptTimeout`）、タイムアウトした操作（`timed_out` の asio または `std::system_error`）、タイムアウトまたは期限切れのツール呼び出し（`ToolExecutionError`） | する | 可 |
| ProviderFailed | `NG.ProviderFailed` | 一時的な `ProviderFailure`: トランスポート、途切れた応答、期限、過負荷、レート制限、または HTTP 5xx ステータスのリモート失敗 | しない。プロバイダーのリトライはプロバイダーランタイムが担当します | 可 |
| EffectCommitted | `NG.EffectCommitted` | 試行が冪等でない Tool の効果をコミットした | 決してしない | 可。ハンドラーが補償できます |
| LogicError | `NG.LogicError` | 整合性チェックを含む `std::logic_error` | しない | 不可 |
| ConfigError | `NG.ConfigError` | プロバイダーの構成、リクエスト、認証、権限、not-found、または誤用による失敗。Program の診断(C++ または Python の `ProgramDiagnosticError`、コンパイルや承認のエラー)のように `ConfigurationFailure` を併せて継承した例外 | しない | 不可 |
| SafetyBlocked | `NG.SafetyBlocked` | コンテンツフィルターまたは拒否で止まったプロバイダーの失敗 | しない | 不可 |
| BudgetExhausted | `NG.BudgetExhausted` | 予算またはクォータの枯渇 | しない | 不可 |
| EffectUncertain | `NG.EffectUncertain` | 結果が不明なプロバイダーディスパッチを含む `ProviderOutcomeError`、結果が不明な Tool の効果（`ToolExecutionError` を含む）、または停止の期限までに止まらなかったタイムアウトした試行 | しない | 不可 |
| UnknownFailure | `NG.UnknownFailure` | `std::bad_alloc` またはその他すべての例外型 | しない | 不可 |
| 実行制御 | `NG.Interrupted`、`NG.HookBlocked`、`NG.Cancelled` | `NodeInterrupt`、`HookBoundaryBlocked`、キャンセル | しない | 不可。実行は一時停止または停止します。原因の連鎖のどこにあっても、Hook のブロックやキャンセルは、すべてのツール経路と同じく `ERROR` イベントなしで `HookBoundaryBlocked` や `CancelledException` そのものとしてグラフ実行を抜けます |

`graph::classify_failure(error, run_token)` は任意の例外についてこの判定を返します(`NodeFailure` レコードを持つ `NodeExecutionError` ならそのレコードの判定)。分類、コード、`public_message`、リトライやルーティングの可否、そして実行制御の分類ならシグナルそのもの(`FailureVerdict::signal`)です。試行を使い切ったレコードでは `retryable` が `false` で、`retryable_by_class` がレコードの `retryable` を保ちます。プロトコルのホストはこのコードをワイヤー上で報告します([プロトコル失敗のマッピング](#protocol-failure-mapping))。

`NG.` 接頭辞は予約されています。作成者コードは `[a-z][a-z0-9_]*(\.[a-z0-9_]+)+` に一致します。C++ のノードは `NodeFailureError(code, public_message, retryable)` を送出します。Python のノードは文字列の `code` 属性と、任意で真偽値の `retryable` と文字列の `public_message` を持つ任意の例外を送出し、呼び出し元はそれでも元の例外を受け取ります。エグゼキューターがノードをあきらめると、送出する `NodeExecutionError` に `NodeFailure` レコードが付き、`node_failure_of()` がそれを返します。レコードにはコード、分類、リトライをやめた理由(`exhausted`、`non_retryable`、`effect_committed`、実行のリトライ上限に達したときの `ceiling`、次の試行の前のバックオフが実行の期限を越えるところだったときの `deadline`、ノードが `max_visits` を超えて活性化されたときの `visit_limit`)、試行回数、リトライ可否とルーティング可否のフラグ、`public_message`、そして最後の試行の効果が `{tool, ordinal, status}` として入ります。`what()` は決してコピーしません。ノードの `non_retryable` に書かれたコードはリトライしません。

**ルーティング可能な失敗は、実行を失敗させる代わりに処理できます。** ノードが failure edge(`failure_edges`、[Program のグラフ機能](program-graphs.md#failure-routing))を持つと、実行器は何も送出しません。失敗した試行は何も書かず、構造化されたレコードが 1 つ edge の `error_channel` に入り、ノードは edge の対象にだけルーティングされます。レコードには正確に `node`、`kind`、`code`、`attempts`、`retryable`、`step`、`routed_to`、`effects` が入り、ノードが `error_text` で選び、ホストの `EngineConfig::failure_text_redactor` が `public_message` からテキストを残したときだけ `detail.untrusted_text` が加わります。ノードはトレースと `interruptAfter` では実行されたものとして数え、ノードの保留書き込みが処理された結果を持つため、再開はノードを再実行せずにルートを再生し、ノードの `ERROR` イベントは `handled: true` と `routed_to` を伝えます。静的ノード、ファンアウトの分岐、`Send` の対象、サブグラフのノードは、同期と非同期のどちらの入口からでもこの経路をたどります。ルーティングできない失敗や、failure edge のないノードの失敗は、上のとおり送出されます。

**試行ごとに独自の効果名前空間があります。** 試行 0 はタスク id(`s3:act`)を使うので、既存のジャーナルはスロットをそのまま保ちます。試行 k は `s3:act#a<k>` を使い、その中で呼び出しの序数がやり直されます。Tool の効果、プロバイダー呼び出し、サブグラフのスレッドはすべて試行に従うため、リトライが以前の試行の効果を再生したり衝突したりすることはありません。`ProviderCallBroker` はリトライのたびに新しい呼び出しスロットを受け取ります。試行 k の開始前に、エグゼキューターはその試行の名前空間をキーとする空の保留書き込みを記録します。チェックポイントストアのスキーマ変更は不要です。クラッシュや割り込みの後の再開は、開始済みの最新の試行をその名前空間で続けるので、`ToolEffectBroker` はその試行がコミットしたものを再生し、残りのリトライ回数も保たれます。`ToolEffectBroker` の下では、冪等でない効果(`ReadOnly` でも `ExternalRead` でもなく `Idempotent` でもないポリシーで成功した呼び出し)をコミットした試行は決してリトライされず、結果が不明な効果は、ノードがその割り込みを捕捉して別のエラーを送出した場合でも、`tool_effect_reconciliation` の割り込みで実行を一時停止させます。ノードが実行したサブグラフの効果は、そのノードの試行に含まれます。

**試行タイムアウトと実行の期限。** ノードごとのポリシーに `attempt_timeout_ms` があると（または定義の値を短くすることしかできないホストのタイムアウト `GraphEngine::set_node_attempt_timeout_ms` があると）、ノードの各試行は実行を駆動するコルーチンの外で実行され、タイムアウトが過ぎると自分の子の `CancelToken` を通じてキャンセルされます。最初の終了イベントが勝つので、タイムアウトの時刻かその後の完了はタイムアウトです。タイムアウトした試行の遅れた完了は捨てられます。書き込みも保留中の書き込みもタイムアウト後のイベントもなく、その試行が始める仲介された Tool 呼び出しは拒否されます。試行は止まってから `NG.Timeout` で失敗し、次の試行はそのときに初めて始まります。停止の期限（もう一度タイムアウト分、少なくとも 1 秒）内に止まらない試行は `NG.EffectUncertain` で失敗するので、実行は失敗します。その試行はバックグラウンドで続いて自分のノードを生かしておき、`wait_for_node_attempts()` がそれを待ちます。次の試行の前のバックオフが `RunContext::deadline`（Program の wall-time 予算）を越えるなら、ノードは kind `deadline` でリトライをやめ、失敗のコードを保ちます。Program の実行では、すべてのノード本体がこのように Program スケジューラーの外で実行されます。[Program のグラフ機能](program-graphs.md#attempt-timeouts-and-the-deadline)を参照してください。

---
<a id="4-graph-types"></a>
## 4. グラフ型
**ヘッダー:** `<neograph/graph/types.h>`
**名前空間:** `neograph::graph`
グラフエンジンの中核型です。チャネル、エッジ、イベント、制御フローの基本要素を定義します。
### ReducerType
複数のノードが書き込んだときに、チャネル値をどのようにマージするかを決めます。
```cpp
enum class ReducerType {
    OVERWRITE,  // New value replaces old value
    APPEND,     // New value is appended (for array channels)
    CUSTOM      // User-defined reducer function
};
```

### ReducerFn
カスタムリデューサー関数のシグネチャです。
```cpp
using ReducerFn = std::function<json(const json& current, const json& incoming)>;
```

| パラメーター | 説明 |
|-----------|-------------|
| `current` | 現在のチャネル値 |
| `incoming` | 書き込まれる新しい値 |
**戻り値:** 新しいチャネル値になるマージ済みの結果。
### Channel
関連するリデューサーを持つ、名前付きでバージョン管理された状態チャネルの内部表現です。
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
名前付きチャネルを対象とする 1 回の書き込み操作です。ノードはこれらのベクターを返します。
```cpp
struct ChannelWrite {
    enum class Mode { Reduce, Overwrite };
    std::string channel;
    json value;
    Mode mode = Mode::Reduce;
    std::shared_ptr<const std::vector<sp::Message>> native_messages;
};
```

`ChannelLifecyclePolicy` は retention（`Unbounded`、`Latest`、`Bounded` と `retention_limit`）と persistence（`Checkpoint`、`Ephemeral`）を分離します。`ChannelWrite::Mode::Overwrite` は reducer を迂回してから retention を適用します。真正 SDK 履歴の保持には `provider_messages_write(messages_or_outcome)` を使い、JSON-only 書き込みから native replay 権限は作れません。Resume guard と結合順序は [channel lifecycle](concepts.md#channel-lifecycle-and-checkpoint-contract)を参照してください。

### NodeInterrupt
動的な中断点 (Human-in-the-Loop) を発生させるためにノード内から送出する例外型です。
送出されると実行が一時停止し、チェックポイントが保存され、後から中断を再開できます。
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

| メソッド | 戻り値 | 説明 |
|--------|---------|-------------|
| `reason()` | `const std::string&` | コンストラクターに渡された理由文字列 |
| `value()` | `const json&` | 構造化ペイロード。付与されていない場合は null |
| `node()` | `const std::string&` | 例外を投げたノード。executor がここに記録するため、ノード本体はグラフ定義上の自分の名前を知る必要がありません |
**往復処理。** 承認プロンプトの情報は双方向に移動します。ノードは *何を* 承認すべきかを示し、人間の回答は
要求したノードへ戻らなければなりません。
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

呼び出し側には一時停止が通常の `RunResult` として見えます。`NodeInterrupt` が呼び出し側へ再送出されることはありません:
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

`resume_value` は `messages` チャネルがある場合、そのユーザーターンとしても届きます。
これはチャット形式のグラフが従来から値を受け取ってきた方法です。
`ctx.resume_value` は一般的な経路であり、グラフのチャネル名に関係なく利用できます。
これは *動的* な中断形式です。*静的* な形式である
グラフ定義の `interrupt_before` / `interrupt_after` は、グラフ作成時に選んだノードで一時停止します。
そのため「モデルが危険なものを要求したときだけ一時停止する」といった条件は表現できません。
### Send
動的なファンアウト要求を表します。ノードは異なる入力を持つ 1 つ以上のノードへ送る `Send` オブジェクトを返し、map-reduce パターンを実現できます。
```cpp
struct Send {
    std::string target_node;  // Node to dispatch
    json        input;        // Channel writes for that invocation
};
```

エンジンは各 `Send` の対象を専用の入力で実行し、すべての Send が完了してからグラフを続行します。同じノードへの複数の Send は順番に実行されます。
### Command
ルーティングの上書きと状態更新を組み合わせたものです。ノードが `Command` を返すと、状態更新を書き込み、特定の次ノードへ実行をリダイレクトし、通常のエッジルーティングを迂回できます。
```cpp
struct Command {
    std::string               goto_node;  // Next node (overrides edge routing)
    std::vector<ChannelWrite> updates;    // State updates to apply
};
```

| フィールド | 型 | 説明 |
|-------|------|-------------|
| `goto_node` | `std::string` | 次に実行するノード名。通常のエッジ解決を上書きします |
| `updates` | `std::vector<ChannelWrite>` | ルーティング前に適用するチャネル書き込み |
### RetryPolicy
ノード実行の失敗に対する自動再試行動作を設定します。
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

再試行 `n` の遅延は `min(initial_delay_ms * backoff_multiplier^n, max_delay_ms)` で、`jitter_pct > 0` の場合は任意で `1 + uniform(-jitter_pct, +jitter_pct)` を乗じます。

どの失敗をリトライするか、また試行ごとの効果をどう分けるかは、ノード失敗の契約(3 章)で説明しています。
### StreamMode
ストリーミング実行中にどのイベントを発行するかを制御するビットフィールドフラグです。
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

ビット単位の OR でフラグを組み合わせます:
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
2 つのノード間の静的な有向エッジです。
```cpp
struct Edge {
    std::string from;  // Source node name
    std::string to;    // Target node name
};
```

グラフの入口と出口には特殊定数 `START_NODE` と `END_NODE` を使用します。
### ConditionalEdge
実行時に名前付き条件関数が決める動的なエッジです。
```cpp
struct ConditionalEdge {
    std::string from;                              // Source node name
    std::string condition;                         // Name in ConditionRegistry
    std::map<std::string, std::string> routes;     // condition_result -> target node name
};
```

実行時にエンジンが条件関数を呼び出します (`ConditionRegistry` から名前で検索します)。
関数の戻り値を `routes` マップのキーとして使い、次のノードを決定します。
### NodeContext
ノードのコンストラクターに渡す依存性注入コンテナーです。LLM プロバイダー、ツール、設定にアクセスできます。
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

`NodeContext::tools` に `ToolSet(std::move(tools))` を設定するか、コンテキストが空の場合に
`EngineResources::tools` へ渡します。`GraphCompiler::compile()` とリンク済みエンジンは
同じツールを共有所有し、コンテキストの再代入では既存のエンジンを変更しません。
ファクトリーは `ctx.tools.view()` で一時的にポインターを参照できます。
Python および MCP ツールにも同じコンパイル時の所有権規則が適用されます。

Python の `NodeContext(provider=...)` と `provider` setter は、実際のネイティブ共有
ポインターに結び付けたコンテキストごとの所有者 lease で、元の Python provider
オブジェクトの寿命を保ちます。コンパイル済みノードとエンジンが持つネイティブ
コンテキストのコピーは、コンテキストを再代入した後や Python wrapper が回収
された後も、同じ Python override 所有者を保持します。再代入で解放するのは
変更可能なコンテキストの lease だけで、既存のコンパイル済みスナップショットは
自分のコピーを保持します。Lease の最終 deleter は GIL を取得します。借用した
C++ 参照や raw pointer だけでは Python override 所有者を保持できません。

### GraphEvent
ストリーミングによるグラフ実行中に発行されるイベントです。
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

**イベントデータのペイロード:**
| 型 | `data` の内容 |
|------|-----------------|
| `NODE_START` | `{}` またはノードメタデータ |
| `NODE_END` | 既定は `null`。ノードが Command を返したときは `command_goto`、Send を返したときは `sends`（件数）、Send で始まった実行なら `send: true` が入ります |
| `LLM_TOKEN` | `{"token": "..."}` |
| `CHANNEL_WRITE` | `{"channel": "...", "value": ...}` |
| `INTERRUPT` | `{"reason": "...", "node": "..."}` |
| `ERROR` | `{"error": "...", "node": "..."}` |
### GraphStreamCallback
ストリーミング実行で使うグラフイベントコールバックの型エイリアスです。
```cpp
using GraphStreamCallback = std::function<void(const GraphEvent&)>;
```

`GraphEvent` は安定したコールバック形式および JSON 向け形式です。型付きペイロードが必要なコードは、
エンジンの入口を変えずに同じストリームを適応できます:
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

`to_typed_event()` が直接変換します。不正なペイロードや将来のバージョンで追加された形は、
ストリーミングコールバックから例外を送出せず `RawGraphEvent` になります。
### NodeResult
ノード実行から返す拡張型です。チャネル書き込みを、任意の `Command` と `Send` の高度な制御フロー指示で包みます。
```cpp
struct NodeResult {
    std::vector<ChannelWrite> writes;           // Channel updates
    std::optional<Command>    command;           // Routing override (if set)
    std::vector<Send>         sends;             // Dynamic fan-out targets

    NodeResult() = default;
    NodeResult(std::vector<ChannelWrite> w);     // Implicit from plain writes
};
```

`command` が設定されると通常のエッジルーティングを迂回し、
`command->goto_node` へ実行を移します。`sends` が空でない場合、エンジンは指定された対象へ
動的にファンアウトします。
### ConditionFn
条件付きエッジで使う条件関数のシグネチャです。
```cpp
using ConditionFn = std::function<std::string(const GraphState&)>;
```

関数は現在のグラフ状態を調べて文字列キーを返します。このキーを `ConditionalEdge::routes` マップで検索し、次のノードを決定します。
### Constants
```cpp
constexpr const char* START_NODE = "__start__";  // Graph entry point
constexpr const char* END_NODE   = "__end__";    // Graph termination
```

これらはエッジ定義でグラフの入口と出口を示すために使用します:
```cpp
Edge{START_NODE, "my_first_node"}
Edge{"my_last_node", END_NODE}
```

---
## 5. GraphState
**ヘッダー:** `<neograph/graph/state.h>`
**名前空間:** `neograph::graph`
グラフ用のスレッドセーフでバージョン管理されたキー値状態コンテナーです。各エントリは、値のマージ方法を制御する
関連するリデューサーによって値のマージ方法が決まる、名前付きチャネルです。
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

| メソッド | 説明 |
|--------|-------------|
| `init_channel(name, type, reducer, initial_value)` | reducer と任意の初期値を指定してチャネルを登録します。そのチャネルを読み書きする前に呼び出す必要があります |
| `get(channel)` | チャネルの現在値を読みます。スレッドセーフです (共有ロック) |
| `get_messages()` | 便利メソッドです。`"messages"` チャネルを読み、`std::vector<ChatMessage>` としてデシリアライズします |
| `write(channel, value)` | reducer を通じて単一チャネルに値を書き込みます。スレッドセーフです (排他ロック) |
| `apply_writes(writes)` | `ChannelWrite` 操作のまとまりをアトミックに適用します。すべての書き込みは 1 つの排他ロック下で適用されます |
| `channel_version(channel)` | 指定チャネルのバージョンを返します。最後に書き込んだ時点の共有書き込みカウンターの値で、一度も書き込まれていなければ 0 です |
| `global_version()` | 共有の書き込みカウンターを返します (任意のチャネルへの書き込みごとに増加し、実行入力の書き込みも含みます) |
| `serialize()` | checkpoint-persistent なチャネル値と version を JSON にシリアライズします |
| `restore(data)` | シリアライズ済み JSON からチャネル値とバージョンを復元します |
| `channel_names()` | 初期化済みの全チャネル名を返します |
`serialize()` は checkpoint-persistent なチャネル値と version のみを含み、ephemeral 値は省略します。`restore_checkpoint` は一致する guard を要求し、書き込み済み ephemeral 状態が失われていれば拒否します。`serialize_runtime` / `restore_runtime` は同一 process 内コピーの live ephemeral 値を保持し、永続保存用ではありません。`checkpoint_snapshot()` は portable snapshot と真正な C++ native sidecar を組にします。`get_messages()` は便宜的 projection です。全 SDK 履歴には `get_provider_messages()`、任意 JSON を chat と解釈しない場合は `captured_provider_messages()` を使います。

---
## 6. GraphNode
**ヘッダー:** `<neograph/graph/node.h>`
**名前空間:** `neograph::graph`
ノードはグラフの計算単位です。ライブラリには抽象基底クラスと 4 種類の組み込みノードがあります。
<a id="graphnode-abstract"></a>
### GraphNode (抽象)
サブクラスがオーバーライドするメソッドは 1 つだけです: `run(NodeInput) -> awaitable<NodeOutput>`。
状態を読み取り、処理を決め、書き込み (必要なら `Command` / `Send` も) を返します。
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

| メンバー | 説明 |
|--------|-------------|
| `in.state` | 読み取り専用の `GraphState`。読み取りには `in.state.get(channel)` を使います |
| `in.ctx.cancel_token` | `provider.invoke(std::move(request))` の前に `request.cancel_token = in.ctx.cancel_token` を代入します。Provider のキャンセルは、対応する境界で協調的に処理します。独自のループでは、null でない `ctx.cancel_token` の `ctx.cancel_token->is_cancelled()` を確認します |
| `in.ctx.step` | 現在のスーパーステップ番号 |
| `in.ctx.thread_id` | `RunConfig::thread_id` を反映します |
| `in.stream_cb` | ストリーミング出力先。null でなければここから `LLM_TOKEN` イベントを送出します。非ストリーミング実行では null です |
| 戻り値: `NodeOutput.writes` | エンジンが reducer でマージするチャネル書き込み |
| 戻り値: `NodeOutput.command` | 任意のルーティング上書き (`goto_node` + 状態更新) |
| 戻り値: `NodeOutput.sends` | 任意の動的ファンアウト。エンジンは `Send` ごとに 1 つの分岐を生成します |
| `get_name()` | グラフ内で一意なノード名を返します |
最小例:
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

非同期ネイティブな LLM 呼び出し:
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

> **移行上の注意。** `GraphNode` のノードエントリポイントは
> `run(NodeInput)` の 1 つです。これは `Command` と `Send` を保持し、
> 非同期およびストリーミング実行に参加するため、サブクラスが実装する
> オーバーライドです。
### LLMCallNode
現在の会話状態で LLM を呼び出します。`"messages"` チャネルを読み、プロバイダーへ補完リクエストを送り、
assistant の応答をチャネルへ書き戻します。`run_stream` / `run_stream_async` で開始した場合は
`LLM_TOKEN` イベントをストリーミングします。
```cpp
class LLMCallNode : public GraphNode {
public:
    LLMCallNode(const std::string& name, const NodeContext& ctx);
    asio::awaitable<NodeOutput> run(NodeInput in) override;
    std::string get_name() const override;
};
```

| コンストラクターパラメーター | 説明 |
|-----------------------|-------------|
| `name` | ノード名 |
| `ctx` | LLM provider、tools、model、instructions を提供するノードコンテキスト |
(LLMCallNode、`ToolDispatchNode`、`IntentClassifierNode`、`SubgraphNode` は
すべて同じ `run(NodeInput)` 契約を実装します。)
### ToolDispatchNode
直近の assistant メッセージからツール呼び出しをディスパッチします。`"messages"` チャネルから保留中のツール呼び出しを読み、各ツールを実行して、ツール結果メッセージを書き戻します。
```cpp
class ToolDispatchNode : public GraphNode {
public:
    ToolDispatchNode(const std::string& name, const NodeContext& ctx);

    asio::awaitable<NodeOutput> run(NodeInput in) override;
    std::string get_name() const override;
};
```

| コンストラクターパラメーター | 説明 |
|-----------------------|-------------|
| `name` | ノード名 |
| `ctx` | ノードコンテキスト (`ctx.tools` でツールを検索して実行します) |
### IntentClassifierNode
LLM を使ってユーザーの意図を分類し、結果を `"__route__"` チャネルに書き込みます。
組み込み条件 `"route_channel"` と組み合わせて、意図に基づく動的なルーティングを可能にします。
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

| コンストラクターパラメーター | 型 | 説明 |
|-----------------------|------|-------------|
| `name` | `std::string` | ノード名 |
| `ctx` | `NodeContext` | 分類用 LLM 呼び出しに使う Provider、model、`provider_controls`。ノードは controls を設定どおりに送り、独自の temperature や出力上限は加えない |
| `prompt` | `std::string` | 分類プロンプトテンプレート |
| `valid_routes` | `std::vector<std::string>` | 許可する分類値。LLM 出力はこれらに照らして検証されます |
### SubgraphNode
コンパイル済みの `GraphEngine` を 1 つのノードとしてラップし、階層的なグラフ構成
(supervisor パターン、ネストしたワークフロー) を可能にします。チャネルマッピングで親子グラフ間のデータの流れを制御します。
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

| コンストラクターパラメーター | 型 | 説明 |
|-----------------------|------|-------------|
| `name` | `std::string` | 親グラフ内のノード名 |
| `subgraph` | `std::shared_ptr<GraphEngine>` | コンパイル済みの子グラフエンジン |
| `input_map` | `std::map<std::string, std::string>` | `parent_channel -> child_channel` の対応付け。親から読み、子の入力へ書き込みます |
| `output_map` | `std::map<std::string, std::string>` | `child_channel -> parent_channel` の対応付け。子が生成した write delta のチャネル名を変更して親へ転送します |
マップが空の場合、チャネルは名前でマッピングされます (identity mapping)。

入力マッピングは親の現在のチャネル値を子の入力へコピーします。出力マッピングは
意図的に異なり、子の最終シリアライズ状態を新しい reducer 入力として扱わず、子が
生成した順序で `ChannelWrite` delta を転送して各 write の `Mode` を保持します。
そのため、継承した append/custom 値が二重に適用されません。出力マッピングは
snapshot replacement を推論しません。マッピング先の親値を置き換える場合、子は
`ChannelWrite::Mode::Overwrite` を明示的に送出する必要があります。

#### 子の永続化と検査

| モード | 子の checkpoint namespace | 開始/resume | Store の優先順位 |
|------|----------------------------|--------------|------------------|
| `Legacy` (既定) | 長さ区切り parent thread、node、parent step、task ID (`subgraph/...`) | 新しい parent は新しい child を開始し、parent resume は対応 snapshot をロード | Parent run の checkpoint backend があれば使用、なければ child 設定 |
| `PerInvocation` | `subgraph/run/` + parent thread、node、永続 parent graph-invocation UUID、step、task ID | 新 parent run ごとに新 namespace; resume は UUID と child write journal を復元 | Parent、次に child |
| `PerThread` | `subgraph/thread/` + parent thread、node | 新呼び出しは以前の checkpoint で child state を seed して新 input を適用; parent resume は対応 snapshot を使用; 同じ compiled node/namespace の重複呼び出しはエラー | Parent、次に child |
| `Stateless` | なし | Child checkpoint 無効; interrupt/resume は拒否するが Store、キャンセル、ToolGate は伝播 | Checkpoint backend なし; parent Store、次に child Store |

明示的な stateful モードは空でない parent thread ID を要求します。`Legacy` は #238 前の namespace、checkpoint wire format、empty-thread 動作を維持します。`PerInvocation` は parent metadata に `_neograph.subgraph_invocation_id` を記録するため、この値のない旧 checkpoint は政策変更後に resume できません。`PerThread` は namespace を共有します。異なる engine/process が同じ backend を使うなら host も admission を調整する必要があり、node-local guard が保護するのは compiled node 一つです。明示的 migration なしに既存 thread の政策を変えないでください。

`GraphEngine::inspect_nested_checkpoint(root_thread, path[, run_store])` で子と孫の checkpoint を調べます。各 `SubgraphPathStep` は child node name、parent super-step、stable Core task ID (`s0:child` または Send task ID)、任意の exact parent checkpoint ID を指定します。結果は `graph_path`、child `thread_id`、完全な `Checkpoint`（チャネル値と checkpoint ID を含む）です。別 thread の checkpoint ID と stateless path は拒否します。`RunResources` で backend を上書きしたなら同じ run-scoped store を渡します。`SubgraphNode::checkpoint_thread_id()` は namespace の一 segment を再構成します。

#### ランタイムコンテキストの伝播

`SubgraphNode` はエンジン境界で子の実行コンテキストを派生させます。公開
`RunContext` のレイアウトは変更しません。

| コンテキスト値 | 子での意味 |
|---------------|-----------------|
| `cancel_token` | 子操作用トークンを作成するため、親のキャンセルはすべての子と孫へ届きます。 |
| `usage`, `deadline`, `trace_id`, `stream_mode` | 継承します。`deadline` と `trace_id` は `RunMetadata` 由来で、子は親の stream mode を広げられません。 |
| `thread_id` | 親 thread ID が空でなければ、親 ID、subgraph ノード名、super-step、invocation identity から決定的に派生します。そのため sibling `Send` 呼び出しは別々の checkpoint identity を得ます。親 thread ID が空なら子もスコープなしとなり checkpointing は無効です。 |
| `step` | 子実行にローカルで、子 checkpoint または 0 から始まります。 |
| `store` | 親 Store があれば継承し、なければ子エンジンに設定された Store を維持します。 |
| Tool policy | 親 `ToolGate` が子の gate より先に実行されます。子は許可された呼び出しをさらに制限または rewrite できますが、親の deny/interrupt は回避できません。 |
| Checkpoint backend and resume value | 親 backend があれば継承し、なければ子の backend を維持します。親 resume は派生した子 checkpoint identity が存在する場合のみ子 checkpoint を resume し、null でない resume value を転送します。Checkpoint routing は公開 `RunContext` フィールドではなく内部実装です。 |

---
## 7. GraphEngine
**ヘッダー:** `<neograph/graph/engine.h>`
**名前空間:** `neograph::graph`
コア実行エンジンです。グラフ定義をコンパイルし、状態遷移を管理し、スーパーステップループでノード実行を調整します。
<a id="engineconfig-and-engineresources"></a>
### EngineConfig と EngineResources
新しいコードでは、エンジンを作成する前に構築時の依存関係とポリシーを組み立てます:
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

`ToolSet` は固定されたツール集合を所有する move-only オーナーです。`GraphRegistry` はエンジンごとのリデューサー、条件、ノードファクトリーのオーバーレイです。
オーバーレイにない名前は既存のプロセス全体レジストリへフォールバックします。
`build()` または `link()` に渡す前に両方を構成してください。
実行時の変更はローカルレジストリ契約の対象外です。
### RunConfig
1 回のグラフ実行を設定します。
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

| フィールド | 型 | デフォルト | 説明 |
|-------|------|---------|-------------|
| `thread_id` | `std::string` | `""` | チェックポイント用に会話/セッションを識別します |
| `input` | `json` | `{}` | 実行開始前にチャネルへ書き込む初期値。通常は `{"messages": [...]}` です |
| `max_steps` | `int` | `50` | 強制終了までの最大スーパーステップ数 (無限ループを防ぎます) |
| `stream_mode` | `StreamMode` | `ALL` | ストリーミング中に送出するイベント種別を制御するビットフィールド |
| `cancel_token` | `std::shared_ptr<CancelToken>` | `nullptr` | 協調キャンセル用ハンドル。エンジンはこれを `RunContext` に包み、各ノードの `run(NodeInput)` 呼び出しへ `in.ctx.cancel_token` として渡します |
| `usage` | `std::shared_ptr<UsageAccumulator>` | `nullptr` | 任意のトークン集計器。省略時はエンジンが作成し、実行中の集計器を `in.ctx.usage` として公開します |
| `resume_if_exists` | `bool` | `false` | `true` かつ `thread_id` のチェックポイントが存在する場合、`input` を適用する前にそこから初期化します (複数ターンのチャット形状) |
### RunContext (v0.4、`NodeInput.ctx` 経由でノードに公開)
エンジンが実行ごとに運ぶディスパッチメタデータです。最初は `RunConfig`
（usage accumulator が未指定なら作成）、`RunMetadata`、有効な Store、
任意の resume value から構築されます。ノードは `run(NodeInput) -> NodeOutput`
のオーバーライド内で `in.ctx` 経由で利用します。

この作成は初期構築だけを説明します。チェックポイントの復元時には、真正な元の bank とその以前の報告を持つ accumulator に置き換わることがあります。したがって、resume や continuation は新しい返却使用量報告や、以前に報告された使用量が `None` になることを保証しません。

Python の `RunMetadata(timeout_ms=None, ...)` は既定で deadline を持ちません。コンストラクターと `set_timeout_ms(timeout)` は、残りの steady-clock 範囲内の非負の整数ミリ秒を受け取り、signed 変換や加算の前に範囲を検査します。負数や範囲外の値は `OverflowError` または `ValueError` を送出します。setter が失敗しても以前の絶対 deadline は保持されます。0 は即時に期限切れとなり、`clear_deadline()` は deadline を削除します。

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

| フィールド | 説明 |
|-------|-------------|
| `cancel_token` | 実行中のトークン。`ProviderRequest::cancel_token` に渡すと、キャンセル時に LLM HTTP ソケットを中断できます。独自ループでは `is_cancelled()` をポーリングします |
| `usage` | エンジンが値を入れる共有トークン集計先 |
| `deadline` | C++ `RunMetadata` から渡される任意の絶対 deadline |
| `trace_id` | C++ `RunMetadata` から渡される任意の trace correlator |
| `thread_id` | `RunConfig.thread_id` を反映します |
| `step` | 現在のスーパーステップ番号。反復ごとに更新されます |
| `stream_mode` | `RunConfig.stream_mode` を反映します |
| `resume_value` | `GraphEngine::resume()` に渡された値。新規実行では空です |
| `store` | エンジンに設定された Store。未設定の場合は `nullptr` |
| `tool_gate` | 継承された親ポリシーを含む、この invocation の有効なポリシー |
### CancelToken
呼び出し側とエンジンで共有する協調的なキャンセル基本単位です。`std::make_shared<CancelToken>()` で作成し、`RunConfig.cancel_token` に渡して、
実行中のスレッドから `cancel()` を呼ぶと実行を中断できます。
ノードが `provider.invoke_async` の途中にいる場合は LLM HTTP ソケットも含まれます。
各エンジン実行は専用の操作用子をフォークするため、
1 つの親から複数の同時実行を安全にキャンセルできます。asio のキャンセルスロットを共有する必要はありません。
エンジンの操作用子は、投稿したキャンセル通知が実行されるまで自身を保持します。
自分で作ったトークンに対して直接 `bind_executor()` を呼ぶ場合、外部オブジェクトの所有権をエンジンは提供できません。
エグゼキューターが排出されるまでトークンを生存させる必要があります。外部オブジェクトの所有権をエンジンは提供できません。
これらのメソッドは公開ヘッダーに inline で定義されているため、更新された `fork()` の寿命動作を受け取るには、
既存の C++ 利用側を再コンパイルする必要があります。
`CancelToken` オブジェクトのレイアウトは 0.11.x とバイナリ互換です。
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

#### 階層キャンセル (v0.4 `fork()`)
各子トークンは独自の `cancellation_signal` を持ち、親の
`cancel()` は生存中のすべての子へ連鎖します。これは v0.3.x の
`add_cancel_hook` リスト (非推奨、v1.0 で削除) に代わる構造的な仕組みです。
同時にネストしたスコープ、つまり各ワーカーが同時に `provider.invoke(std::move(request))` を呼ぶ
マルチ Send ファンアウトでは、各ワーカーが
`fork()` を 1 回だけ行い、互いのスロットを上書きしません。
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

| メソッド | 説明 |
|--------|-------------|
| `cancel()` | 冪等でスレッドセーフ。ポーリングフラグを設定し、バインドされたエグゼキューターで asio の cancellation_signal を発行し、`fork()` 経由ですべての生存中の子へ連鎖 |
| `is_cancelled()` | ロックフリーのポーリング読み取り |
| `fork()` | **v0.4。** 子の shared_ptr を返します。親の cancel() は連鎖し、fork() 時点ですでに親がキャンセル済みなら競合なしで子もキャンセル済みになります |
| `bind_executor(ex)` | シグナル発行を処理するエグゼキューターをエンジン内部でバインド |
| `slot()` | `co_spawn` 時に `bind_cancellation_slot` へ渡す asio `cancellation_slot` |
### RunResult
グラフ実行が完了または中断した後に返る結果です。
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

`RunResult::usage` は nullable provider 報告であり支出 bank ではありません。`native_messages` は真正 typed 履歴、`provider_outcomes` は各所有 Completion/Failure を保持します。JSON `output` は portable projection です。全履歴入力には `RunConfig::provider_messages`、typed event 観測には `on_provider_event` を使います。永続 native checkpoint/receipt custody には `native_history_archive` が必要ですがメモリ内 sidecar には不要です。

`output` の `channels.<name>.version` と `global_version` は、すべてのチャネルが共有する1つの書き込みカウンターから来ます。チャネルのバージョンは、最後に書き込んだ時点のカウンターの値なので、書き込みの順序を表します。チャネルが受けた書き込みの回数ではなく、`global_version` もスーパーステップの番号ではありません。[バージョン](concepts.md#versions)を参照してください。

resume や continuation では、`provider_outcomes` は元の結果に新しく生成した結果を続けた順序付きリストを保持します。`usage` にも復元した bank の以前の報告が残ることがあります。利用側は完了済み provider effect の再 dispatch と二重計上を防ぐ不変条件を保ってください。空またはリセットされた結果リストや `usage=None` は resume の不変条件ではありません。

`provider_messages` は全 typed 履歴を渡し、messages チャネルのみを置き換えます。`on_provider_event` は typed SDK event を観測します。`provider_outcomes`、`provider_loop_history` は結果と task-local continuation を inner turn 間で保持します。`native_history_archive` は永続 native custody を結び付け、モデル予算は与えません。任意の `model_token_budget` 上限と `budget_exhausted` 信号は予算対応 dispatch 用です。Python と C++ は共に `RunResult.native_messages` で全履歴、`provider_outcomes` で所有結果を返します。入力は引き続き `RunConfig.provider_messages` で、`RunResult.provider_messages` や `provider_history` alias はありません。
| フィールド | 型 | 説明 |
|-------|------|-------------|
| `output` | `json` | 全チャネルの最終シリアライズ状態 |
| `interrupted` | `bool` | 中断 (HITL) により実行が一時停止した場合は `true` |
| `interrupt_node` | `std::string` | 中断を発生させたノードの名前 |
| `interrupt_value` | `json` | 中断に関連する理由またはペイロード |
| `checkpoint_id` | `std::string` | 最後に保存したチェックポイントの UUID |
| `execution_trace` | `std::vector<std::string>` | 完了したノード実行ごとに 1 件。エンジンが結果を反映した順に並ぶ。スーパーステップは順番どおり、1 つのスーパーステップの中では実行可能なノードが先、そのあとに Send が送出された順で並び、実行が終わった順は関係ない。Send で実行されたノードは `<対象>[send]` と記録される。[Send](concepts.md#5-send--dynamic-fan-out) を参照 |
`max_steps_exhausted()` は、実行可能な作業が残っている状態でステップ上限に達して実行が停止した場合にのみ `true` を返します。
許可された最後のステップで `__end__` に到達したグラフは
`false` を返します。
`status()` は `RunStatus::Completed`、`RunStatus::Interrupted`、
`RunStatus::StepLimit`、`RunStatus::SafePoint`、`RunStatus::BarrierStalled`、または `RunStatus::FailureExit` のいずれかを返します。公開 `RunResult` のデータレイアウトは変わりません。
`BarrierStalled` は、実行可能なノードがなく、最後の super-step が `__end__` へ明示的にルーティングせず、barrier がまだシグナルを保持しているため join がもう発火できないことを示します。このとき `barrier_stalled()` は true になり、`output["_neograph"]["barrier_stalled"]` は停滞した join ごとに `{node, wait_for, received, missing}` を列挙します。
`FailureExit`(#364)は、failure exit のノードが完了し、実行が失敗として終わったことを示します。`failure_exited()` は true になり、`output["_neograph"]["failure_exit"]` は `{node, code}` です。`RunInvocation` はこれを `Error` として報告します。
`ChannelKey<T>` は再利用可能なチャネル名を、期待する C++ 型に結び付けます:
```cpp
inline const ChannelKey<std::string> Answer{"answer"};

auto answer = result.channel(Answer);
if (auto optional = result.try_channel(Answer)) {
    std::cout << *optional << '\n';
}
```

### GraphEngine
メインのエンジンクラスです。新しいコードでは JSON 定義に `build_strict()` を使ってください。
無効なトポロジーをノード作成前に拒否します。解析、検証、検査、変換を別工程に分ける必要がある場合は `link()` と
`ValidatedTopology` を使います。寛容な `build()`、`CompiledGraph` の link オーバーロード、`compile()`、
構築後の setter は互換性のための経路として残っています。
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

#### `build` と `link`
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

`build()` はコンパイル、検証、リンク、設定を行い、完全に構成されたエンジンを返します。
`link()` は `CompiledGraph` を move で受け取り、実行時設定を適用します。手動でコンパイルする呼び出し側は、
必要とするソースから IR への往復検証を自分で行う責任があります。
#### `compile` (互換性)
```cpp
static std::unique_ptr<GraphEngine> compile(
    const json& definition,
    const NodeContext& default_context,
    std::shared_ptr<CheckpointStore> store = nullptr);
```

JSON 定義からグラフをコンパイルし、実行可能なエンジンを返します。
元のシグネチャを維持し、`build()` に委譲します。新しいコードでストア、再試行ポリシー、ワーカー設定、
キャッシュ、ツールゲートが必要な場合は `EngineConfig` を優先してください。
| パラメーター | 型 | 説明 |
|-----------|------|-------------|
| `definition` | `const json&` | JSON 形式のグラフ定義 (下記参照) |
| `default_context` | `const NodeContext&` | すべてのノードに注入するデフォルトコンテキスト |
| `store` | `std::shared_ptr<CheckpointStore>` | 任意の永続化用チェックポイントストア |
**グラフ定義 JSON スキーマ:**
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

##### バリアノード (AND-join のオプトイン)
ノード宣言には、そのノードだけで AND-join の意味論を有効にする `barrier` フィールドを含められます。
デフォルトのシグナルディスパッチモデルでは、上流のいずれかがルーティングするたびにノードが発火します。
そのため、長さの異なる経路による非対称な直列 fan-in では join ノードが二重に発火します。
バリアは、記載されたすべての上流が少なくとも 1 回 (任意の数のスーパーステップをまたいで) シグナルを送るまでノードを待機させます。
```json
"join": {
  "type": "my_join",
  "barrier": {"wait_for": ["a", "s2"]}
}
```

`a` と `s2` の両方がシグナルを送ったときに 1 回発火します。バリアを通るループでは、
各ラウンドで新しいシグナルを集めるため、発火後に状態がリセットされます。
**永続化:** `CHECKPOINT_SCHEMA_VERSION = 2` なので、バリアアキュムレーターは各チェックポイントに保存され (`Checkpoint::barrier_state`、
`map<string, set<string>>`)、再開時に復元されます。蓄積途中で発生した中断も安全です。
部分的な上流集合が一時停止を越えて保持され、残りのシグナルが届くとバリアが発火します。
v1 の blob は空の `barrier_state` でデシリアライズされ、保存済みチェックポイントでは v2 より前の動作と一致します。
#### `run`
```cpp
RunResult run(const RunConfig& config);
```

グラフを同期的に (ブロッキングして) 実行します。`START_NODE` から開始し、`END_NODE` に到達するか
`max_steps` を超えるまでエッジをたどります。
#### `run_stream`
```cpp
RunResult run_stream(const RunConfig& config,
                     const GraphStreamCallback& cb);
```

ストリーミングイベント付きでグラフを実行します。コールバック `cb` は `config.stream_mode` フィルターに一致する各イベントで呼び出されます。
#### `resume`
```cpp
RunResult resume(const std::string& thread_id,
                 const json& resume_value = json(),
                 const GraphStreamCallback& cb = nullptr);
```

以前に中断されたチェックポイントから実行を再開します (Human-in-the-Loop)。
| パラメーター | 型 | 説明 |
|-----------|------|-------------|
| `thread_id` | `std::string` | 再開対象のスレッド ID |
| `resume_value` | `json` | 再開前に注入する任意の値 (例: 人間の承認) |
| `cb` | `GraphStreamCallback` | 任意のストリーミングコールバック。非ストリーミング再開では `nullptr` |
#### `get_state`
```cpp
std::optional<json> get_state(const std::string& thread_id) const;
```

スレッドの最新状態を返します。チェックポイントがなければ `std::nullopt` を返します。
#### `get_state_history`
```cpp
std::vector<Checkpoint> get_state_history(const std::string& thread_id,
                                          int limit = 100) const;
```

スレッドのチェックポイント履歴を、最後に保存されたものから順に返します。最初の要素は `get_state` が読むチェックポイントです。
#### `update_state`
```cpp
void update_state(const std::string& thread_id,
                  const json& channel_writes,
                  const std::string& as_node = "");

void update_state_writes(const std::string& thread_id,
                         const std::vector<ChannelWrite>& channel_writes,
                         const std::string& as_node = "");
```

チャネル write を適用してスレッド状態を手動更新します。JSON object 形式はチャネル名ごとに
reducer write を適用します。`ChannelWrite` vector 形式は write 順序と明示的な overwrite
mode を保持します。どちらも更新済み状態で新しい checkpoint を作成します。
| パラメーター | 型 | 説明 |
|-----------|------|-------------|
| `thread_id` | `std::string` | 対象スレッド |
| `channel_writes` | `json` | 適用する `{channel: value}` ペアのオブジェクト |
| `as_node` | `std::string` | 任意。特定ノードからの書き込みとして記録 |
#### `fork`
```cpp
std::string fork(const std::string& source_thread_id,
                 const std::string& new_thread_id,
                 const std::string& checkpoint_id = "");
```

スレッドの状態を新しいスレッドとしてコピーします。会話を分岐したり、what-if シナリオを作ったりするのに便利です。
| パラメーター | 型 | 説明 |
|-----------|------|-------------|
| `source_thread_id` | `std::string` | コピー元のスレッド |
| `new_thread_id` | `std::string` | 新しいスレッド識別子 |
| `checkpoint_id` | `std::string` | 任意。特定のチェックポイントから fork (デフォルト: 最新) |
**戻り値:** 新しく fork した状態のチェックポイント ID。

Fork は状態と選択 checkpoint の pending continuation をコピーし、新 turn は作りません。完了した `__end__` continuation の resume は node を実行しないため、質問だけ編集しても回答は生まれません。pending 作業には実際の `next_nodes` がある exact paused checkpoint ID を選んで fork し、portable state を編集して resume します。過去の empty-`next_nodes` latest-resume snapshot 全体を terminal sentinel と同一視しません。example 08 は terminal fork 後に別の new-turn flow を維持します。

authentic native state は元 shared-bank scope に留まります。Fork は spending grant を複製せず、managed-bank custody/source commitment/元 ceiling/deadline が適用されます。durable standalone fork は native 権限の import・更新を許可しません。
ツールは `NodeContext::tools` または `EngineResources::tools` でコンパイル前に所有されます。
コンパイル後の所有権移譲はありません。
#### `set_checkpoint_store`
```cpp
void set_checkpoint_store(std::shared_ptr<CheckpointStore> store);
```

チェックポイントストアを接続します。`resume()`、`get_state()`、`fork()` およびすべての状態検査メソッドに必要です。
#### `set_store`
```cpp
void set_store(std::shared_ptr<Store> store);
```

クロススレッド共有メモリストアを接続します ([Store](#9-store) 参照)。
#### `get_store`
```cpp
std::shared_ptr<Store> get_store() const;
```

接続された共有メモリストアを返します。設定されていなければ `nullptr` です。
#### `set_retry_policy`
```cpp
void set_retry_policy(const RetryPolicy& policy);
```

すべてのノードのデフォルト再試行ポリシーを設定します。固有のポリシーがないノードはこれを使用します。
#### `set_node_retry_policy`
```cpp
void set_node_retry_policy(const std::string& node_name, const RetryPolicy& policy);
```

特定ノードの再試行ポリシーを設定し、デフォルトを上書きします。
#### `get_graph_name`
```cpp
const std::string& get_graph_name() const;
```

定義で指定されたグラフ名を返します。
---
<a id="7b-engine-internals"></a>
## 7b. エンジン内部
`GraphEngine` は 4 つの目的別クラスに委譲する薄いオーケストレーターです。通常、利用者が直接触れることはありません。これらは `GraphEngine::build()` (または互換ファサード `compile()`) 内で生成され、`execute_graph()` から駆動されますが、高度な利用者が JSON なしで構築したり、カスタムチェックポイントフローを駆動したり、テストで部品をスタブ化したりできるよう公開されています。
| クラス | ヘッダー | 役割 |
|-------|--------|----------------|
| [`GraphCompiler`](#graphcompiler) | `<neograph/graph/compiler.h>` | JSON を `CompiledGraph` に解析 |
| [`Scheduler`](#scheduler) | `<neograph/graph/scheduler.h>` | ルーティング判断 (シグナルディスパッチ + バリア) |
| [`CheckpointCoordinator`](#checkpointcoordinator) | `<neograph/graph/coordinator.h>` | 実行ごとのチェックポイント寿命管理 |
| [`NodeExecutor`](#nodeexecutor) | `<neograph/graph/executor.h>` | 再試行、並列ファンアウト、Send ディスパッチ |
### GraphCompiler
**ヘッダー:** `<neograph/graph/compiler.h>`
純粋な JSON → 値型の変換です。実行時依存性はなく、生成された `CompiledGraph` は検査したり、テストで手作業により構築したりできる move 可能なバンドルです。
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

`GraphCompiler::parse()` はノードを構築せずに `TopologySpec` を生成します。
`GraphValidator::validate()` は構造化された診断情報を返し、`GraphValidator::require_valid()` は
`ValidatedTopology` を返すか `std::runtime_error` を送出します。
実行時ノードを解決してインスタンス化するのは `GraphCompiler::link()` だけです。
`compile()` は parse と link を組み合わせる互換経路であり、`GraphEngine::build()` は寛容な警告動作を維持します。
新しいコードでは `GraphEngine::build_strict()` を使って境界全体を強制できます:
```cpp
auto spec = GraphCompiler::parse(definition);
auto validated = GraphValidator::require_valid(std::move(spec));
auto engine = GraphEngine::link(std::move(validated), config, resources);
```

### Scheduler
**ヘッダー:** `<neograph/graph/scheduler.h>`
グラフトポロジーを所有し、前のスーパーステップが発行したルーティングシグナルから各ステップの ready 集合を計算します。
スレッド、チェックポイント、再試行、HITL の知識は持ちません。これらはエンジン側に残ります。
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

**意味論:**
- **シグナルディスパッチ:** ステップ S のノードが明示的にルーティングした場合に限り、ノードはスーパーステップ S+1 で ready になります。
  通常のエッジ、条件エッジの分岐、`Command::goto_node`、または Send が対象です。静的な predecessor マップはありません。
  それでは XOR ルーティングと AND fan-in を混同してしまうためです。
- **対応の不変条件:** 呼び出し側は `just_ran` と `results` を `just_ran[i] ↔ results[i]` の対応で渡す必要があります。
  2 引数オーバーロードの型シグネチャによって保証され、呼び出し側が対応をずらせないようになっています。
- **バリア:** `"barrier": {"wait_for": [...]}` を宣言したノードは、一覧にあるすべての上流がシグナルを送るまで待機します。
  シグナルは可変の `BarrierState` マップを通じてスーパーステップをまたいで蓄積されます。
  発火するとエントリがリセットされるため、バリアを通るループも正しく動きます。
### CheckpointCoordinator
**ヘッダー:** `<neograph/graph/coordinator.h>`
`(CheckpointStore, thread_id)` に対する実行単位のラッパーです。ストアが null または thread_id が空なら、各メソッドは
ストアが null または thread_id が空なら安全な no-op になるため、呼び出し側でガードする必要はありません。
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

**フェーズ対応のステップオフセット:** `load_for_resume()` は最新チェックポイントの `interrupt_phase` を読み、
`start_step` を適切に設定します。
`Before` / `NodeInterrupt` は `cp.step` で再入し、`After` / `Completed` /
`Updated` は +1 進めます。エンジンの再開経路がこのロジックを重複して実行することはありません。
### NodeExecutor
**ヘッダー:** `<neograph/graph/executor.h>`
スーパーステップごとのノード呼び出しを所有します。再試行ループ、リプレイ検索、保留書き込みの記録、
保留書き込みの記録、
`asio::experimental::make_parallel_group`、および Send dispatch です。3.0
同期版 `run_one` / `run_parallel` / `run_sends` の対は削除され、呼び出し側は `_async` 版を使います。
呼び出し側は `_async` 版を使います。
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

**不変条件:**
- `run_one_async` と `run_parallel_async` はどちらも
  割り込み元ノードに限定した `phase=NodeInterrupt` チェックポイントを保存します。
  その後 `NodeInterrupt` を再送出するため、再開時はそのノードだけに再入します。
  兄弟ノードの書き込みはすでに `pending_writes` にあり、マップ経由でリプレイされます。
- `run_parallel_async` は `ready` の順に書き込みと `Command.updates` を適用するため、
  後続の Scheduler 呼び出しでも `ready[i] ↔ results[i]` の対応が保たれます。
- `run_sends_async`: 一つでも複数でも、すべての Send は対象ごとに分離状態コピー (初期化 + 復元 + 入力適用) を作り、
  再試行付きで実行します。payload はその Send にだけ見えます (#389)。
- 任意の `fan_out_pool` が並列分岐のディスパッチ先を決めます。null の場合、分岐は
  `co_await asio::this_coro::executor` 上で実行されます。単一スレッドの非同期呼び出し側には十分ですが、
  CPU バウンドのファンアウトは直列化されます。null でなければ `run_parallel_async` と
  複数 Send 分岐は `pool->get_executor()` に `co_spawn` され、実際のスレッド並列性を得ます。
  `GraphEngine::set_worker_count(N)` は同期 `run()` 呼び出し側向けにプールを設定します。
- `execute_node_with_retry_async` は内部の再試行ループです。バックオフには `asio::steady_timer` を使うため、
  再試行待ちの間もエグゼキューターは停止しません。
---
<a id="8-checkpoint"></a>
## 8. チェックポイント
**ヘッダー:** `<neograph/graph/checkpoint.h>`
**名前空間:** `neograph::graph`
チェックポイントはグラフ実行状態を保存・復元することで、永続化、タイムトラベルデバッグ、Human-in-the-Loop ワークフローを可能にします。
### Checkpoint (struct)
時点におけるグラフ実行状態をシリアライズしたスナップショットです。
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

| フィールド | 型 | 説明 |
|-------|------|-------------|
| `id` | `std::string` | 一意の識別子 (UUID v4) |
| `thread_id` | `std::string` | 会話/セッションごとにチェックポイントをまとめます |
| `channel_values` | `json` | 全チャネルのシリアライズ済み状態 |
| `channel_versions` | `json` | エンジンはこのフィールドを設定しません。エンジンのチェックポイントは、チャネルごとのバージョンを `channel_values["channels"][name]["version"]` に、共有の書き込みカウンターを `channel_values["global_version"]` に保持します |
| `parent_id` | `std::string` | 直前のチェックポイント ID (タイムトラベル用の連結リストを形成します) |
| `current_node` | `std::string` | チェックポイント取得時に実行中だったノード |
| `next_nodes` | `std::vector<std::string>` | 次のスーパーステップに予定されている全ノード (`resume()` が使用)。signal dispatch では、1 つのスーパーステップ後に複数ノードが同時に ready になることがあります (並列 fan-out や条件分岐の同時活性化)。その全てを永続化する必要があり、単一ノードだけを保存するとクラッシュをまたいで兄弟ノードが黙って失われます |
| `interrupt_phase` | `CheckpointPhase` | 列挙値: `Before` (`interrupt_before` が発火)、`After` (`interrupt_after` が発火)、`Completed` (通常のスーパーステップ周期)、`NodeInterrupt` (実行中にノードが `NodeInterrupt` を投げた)、`Updated` (外部 `update_state()` 注入)。`to_string()` と `parse_checkpoint_phase()` が安定した wire/log エンコードを提供します |
| `barrier_state` | `map<string, set<string>>` | これまで signal した上流を barrier ごとに蓄積したもの。エントリは処理中 (まだ発火していない) barrier にだけ存在し、Scheduler は barrier 発火時にそのエントリを消します。形状は `scheduler.h` の `BarrierState` と一致します。schema v2 以降に存在します。v1 blob は空 map としてデシリアライズされ、v2 以前の動作と一致します |
| `metadata` | `json` | 任意のユーザー定義データ |
| `step` | `int64_t` | スーパーステップカウンター |
| `timestamp` | `int64_t` | Unix epoch ミリ秒での作成時刻 |
| `schema_version` | `std::uint32_t` | wire 上のレイアウトバージョン (`CHECKPOINT_SCHEMA_VERSION`、現在は `3` を参照)。固定幅の unsigned 型です。schema version は非負で、ディスクへ永続化して JSON 経由で往復する値だからです。永続化する `CheckpointStore` 実装はこれをシリアライズし、デシリアライズした blob の `0` は「バージョン付け前」(例: フィールドが存在しなかった。移行は呼び出し側の責任) と扱うべきです |
### CheckpointStore
チェックポイント永続化の抽象インターフェースです。データベース、ファイルシステム、その他のバックエンドに保存するには実装します。
データベース、ファイルシステム、その他のバックエンドにチェックポイントを保存するためのインターフェースです。
> **カスタムストアを書く場合:** 新しい実装では、適用できる最小の機能を実装してください。
> 適用する capability は `CheckpointStoreCore`、必要に応じて
> `AsyncCheckpointStore` や `PendingWritesCheckpointStore` を必要に応じて実装し、
> `adapt_checkpoint_store()` を通して渡します。既存の `CheckpointStore` は互換性契約として残ります。
> Sync-only checkpoint backend は有界 worker pool に offload します。Native async backend は coroutine capability を使い、欠落 sync operation は再帰せず例外を投げます。
> [`ASYNC_GUIDE.md` §9.4](ASYNC_GUIDE.md#94-checkpointstore) を参照してください。

Python の `CheckpointStore.requires_managed_budget(thread_id) -> bool` は、永続化
された managed-bank の拒否義務を読む同期仮想メソッドです。エンジンのネイティブ
`requires_managed_budget_async(thread_id)` facade は同期呼び出しを worker に
offload し、バインディングは GIL を取得して Python override を呼び出します。
Override がなければ、ネイティブ実装は `False` を返さず、未対応 backend の
明示的なエラーを送出します。Reader は、信頼する namespace/thread が過去に
アクティブな standalone managed bank を持っていたかを報告します。Bank を
除いた状態の保存や checkpoint の削除でも、この義務を消してはいけません。
実装はこれを正しく報告しなければなりません。この reader は支出、復元、
bank、lease の権限を与えません。
有限予算の実行には、対応する実際のネイティブ managed-budget lease が引き続き
必要です。

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

| メソッド | 説明 |
|--------|-------------|
| `save(cp)` / `save_async(cp)` | チェックポイントを永続化します。エンジンはスーパーステップごとに 1 つ書き込みます。 |
| `load_latest(thread_id)` / `_async` | スレッドの最新チェックポイントを読み込みます。 |
| `load_by_id(id)` / `_async` | UUID で指定したチェックポイントを読み込みます (タイムトラベル)。 |
| `list(thread_id, limit)` / `_async` | スレッドのチェックポイントを新しい順に最大 `limit` 件列挙します。 |
| `delete_thread(thread_id)` / `_async` | スレッドのすべてのチェックポイントを削除します。 |
| `put_writes(thread_id, parent_cp, write)` / `_async` | スーパーステップ途中で成功したノード実行を記録します。エンジンはノードが戻った直後、かつその書き込みを GraphState に適用する *前* にこれを呼び出します。デフォルトは no-op です。 |
| `get_writes(thread_id, parent_cp)` / `_async` | 親チェックポイントに紐づく保留中の書き込みを読み込みます。エンジンは resume 時にこれを呼び出し、完了済みタスクをスキップします。デフォルトは空です。 |
| `clear_writes(thread_id, parent_cp)` / `_async` | 後続スーパーステップのチェックポイントが永続保存された後、保留中の書き込みを破棄します。デフォルトは no-op です。 |
### InMemoryCheckpointStore
テストや単一プロセスのアプリケーションに適した、スレッドセーフなインメモリ実装です。
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
**ヘッダー:** `<neograph/graph/store.h>`
**名前空間:** `neograph::graph`
スレッドをまたいで共有するメモリストアです。名前空間付きキー値ストレージを提供し、スレッドとグラフ実行をまたいで永続します。長期的なユーザー設定、共有ナレッジベース、エージェントメモリなどに使えます。
### Namespace
文字列ベクターで表す階層パスです。
```cpp
using Namespace = std::vector<std::string>;
```

例: `{"users", "user123", "preferences"}` は `users/user123/preferences` というパスを表します。
### StoreItem
ストア内の 1 件の項目です。
```cpp
struct StoreItem {
    Namespace   ns;          // Namespace path
    std::string key;         // Item key within the namespace
    json        value;       // Stored value
    int64_t     created_at;  // Creation timestamp (Unix epoch millis)
    int64_t     updated_at;  // Last update timestamp (Unix epoch millis)
};
```

<a id="store-abstract"></a>
### Store (抽象)
クロススレッド共有メモリの抽象インターフェースです。
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

| メソッド | 説明 |
|--------|-------------|
| `put(ns, key, value)` | 値を挿入または更新します。項目がすでに存在する場合は `updated_at` を更新します |
| `get(ns, key)` | 単一の項目を取得します。見つからない場合は `std::nullopt` を返します |
| `search(ns_prefix, limit)` | namespace が指定 prefix で始まるすべての項目を探します |
| `delete_item(ns, key)` | store から項目を削除します |
| `list_namespaces(prefix)` | 指定 prefix で始まる一意な namespace をすべて列挙します |
### InMemoryStore
テストと単一プロセス用途向けのスレッドセーフなインメモリ実装です。
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
**ヘッダー:** `<neograph/graph/loader.h>`
**名前空間:** `neograph::graph`
リデューサー、条件、ノード型向けのレガシーなシングルトンレジストリです。
JSON 駆動のグラフ構築では、プロセス全体のフォールバックとして残っています。新しい
コードは `EngineResources` 経由で `GraphRegistry` を渡せます。ローカルの登録が優先され、
見つからない名前はここで解決されます。
### ReducerRegistry
文字列名を `ReducerFn` 実装へ対応付けるシングルトンレジストリです。
```cpp
class ReducerRegistry {
public:
    static ReducerRegistry& instance();

    void register_reducer(const std::string& name, ReducerFn fn);
    ReducerFn get(const std::string& name) const;
    std::vector<std::string> names() const;
};
```

| メソッド | 説明 |
|--------|-------------|
| `instance()` | singleton インスタンスを返します |
| `register_reducer(name, fn)` | カスタム reducer 関数を登録します |
| `get(name)` | reducer を名前で検索します。見つからない場合は例外を投げます |
| `names()` | 登録済み reducer 名のソート済みリスト (外部ツール用の introspection) |
### ConditionRegistry
文字列名を `ConditionFn` 実装へ対応付けるシングルトンレジストリです。
```cpp
class ConditionRegistry {
public:
    static ConditionRegistry& instance();

    void register_condition(const std::string& name, ConditionFn fn);
    ConditionFn get(const std::string& name) const;
    std::vector<std::string> names() const;
};
```

| メソッド | 説明 |
|--------|-------------|
| `instance()` | singleton インスタンスを返します |
| `register_condition(name, fn)` | カスタム condition 関数を登録します |
| `get(name)` | condition を名前で検索します。見つからない場合は例外を投げます |
| `names()` | 登録済み condition 名のソート済みリスト (外部ツール用の introspection) |
### NodeFactory
JSON 設定から `GraphNode` インスタンスを作成するシングルトンファクトリーです。
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

| メソッド | 説明 |
|--------|-------------|
| `instance()` | singleton インスタンスを返します |
| `register_type(type, fn)` | ノード factory を登録します。config schema のデフォルトは許容的な `{"type":"object"}` です |
| `register_type(type, fn, config_schema)` | 上と同じですが、ノードの `config` 用 JSON Schema (Draft 2020-12) を宣言します。追加的な変更であり、2 引数 overload はそのまま動作します。`export_schema()` でのみ使われ、エンジンはこれに対して config を検証しません |
| `create(type, name, config, ctx)` | 指定 type のノードを作成します。type が登録されていない場合は例外を投げます |
| `registered_types()` | 登録済みノード type 名のソート済みリスト |
| `export_schema()` | このエンジンが受け付ける topology JSON の機械可読な説明 ([Topology Schema Export](#topology-schema-export-issue-56) を参照) |
<a id="built-in-registrations"></a>
### 組み込み登録
ライブラリは次のコンポーネントをあらかじめ登録します:
**Reducers:**
| 名前 | 動作 |
|------|----------|
| `"overwrite"` | 現在値を入力値で置き換えます |
| `"append"` | 入力値を現在の配列へ追加します。入力値が配列の場合は、その要素を連結します |
**Conditions:**
| 名前 | 動作 |
|------|----------|
| `"has_tool_calls"` | `"messages"` チャネルの最後のメッセージを調べます。ツール呼び出しを含む場合は `"yes"`、それ以外は `"no"` を返します |
| `"route_channel"` | `"__route__"` チャネルを読み、その文字列値を返します。`IntentClassifierNode` と一緒に使います |
**Node types:**
| 型 | クラス | 説明 |
|------|-------|-------------|
| `"llm_call"` | `LLMCallNode` | 現在の会話状態で LLM を呼び出します |
| `"tool_dispatch"` | `ToolDispatchNode` | 最新の assistant メッセージからツール呼び出しを dispatch します |
| `"intent_classifier"` | `IntentClassifierNode` | LLM による意図分類。`config` から `prompt` と `valid_routes` を読みます |
| `"subgraph"` | `SubgraphNode` | コンパイル済みサブグラフを実行します。`config` から `input_map` と `output_map` を読みます |
| `"image_generate"`, `"image_edit"` | エンドポイントノード | 画像を生成または編集します (「エンドポイントノード」を参照) |
| `"embed"`, `"rerank"`, `"moderate"`, `"decide"` | エンドポイントノード | テキストの埋め込み、ドキュメントの順位付け、テキストの検査、または構造化された質問への回答を行います |
| `"speak"`, `"transcribe"`, `"translate"` | エンドポイントノード | テキストを音声合成するか、音声を文字起こしまたは翻訳します |
| `"job_submit"`, `"job_collect"` | エンドポイントノード | 動画ジョブを投入し、続いて回収します |

#### エンドポイントノード

エンドポイントノード (#531) は、それぞれノードコンテキストの provider で SchemaProvider のエンドポイントを 1 回呼び出します（`Provider::complete_endpoint_async`）。
- **設定。** `model` は必須です。ノードは、エンドポイントのオプションを SDK リクエストのメンバー名（`size`、`quality`、`dimensions`、`voice`、`top`、...）で受け取り、provider ポリシーが許可するメンバーは `extensions` で受け取ります。読み書きするチャネルの名前も受け取り、`output` は書き込むチャネルの名前です。厳密なグラフ（`schema_version` 1）は未知のキーを拒否します。チャネルは設定で指定されるため、エンドポイントノードを含むグラフでは、検証器の効果分析は実行されません。
- **テキスト。** `embed` と `moderate` は文字列または文字列の配列（チャネル `input`）を、`speak` は文字列（`text`）を、`rerank` はクエリ文字列（`query`）とドキュメントの配列（`documents`）を読みます。
- **メディア。** メディアのチャネルには、base64 の data URL、https URL、または provider の outcome format v3 のメディア JSON が入ります。ノードはメディアを v3 形式で書き込みます。`image_generate` と `image_edit` は `images` に、`speak` は `audio` に、`job_collect` は `video` に書き込みます。`image_edit` は編集する画像を `references` から読み、`transcribe` と `translate` は `audio` を読みます。
- **その他の結果。**
  - `embed` は入力ごとに数値の配列を 1 つ書き込みます（`embeddings`）。
  - `rerank` は `[{index, relevance}]` を関連度の高い順に書き込みます（`ranking`）。
  - `moderate` は `[{flagged, categories, scores}]` を書き込みます（`moderation`）。
  - `transcribe` と `translate` はテキストを書き込みます（`transcript`、`translation`）。
- **`decide`。** 任意の JSON を根拠（`input`）として読み、`questions` を受け取ります。各質問は `{name, kind, instructions, options, when_true, when_false}` で、`kind` は `predicate`、`choice`、`score`、`box` のいずれかです。回答は `decisions` に書き込みます。
- **動画ジョブ。**
  - `job_submit` はプロンプト（`prompt`）を読み、ジョブのハンドル（`job`）を書き込みます。
  - `job_collect` は実行されるたびにハンドルを 1 回ポーリングします。完了したジョブの出力は `video` にダウンロードされ、失敗、キャンセル、または期限切れのジョブはノードを失敗させます。
  - まだ実行中のジョブは、`{"job", "status", "raw_status"}` とともに `NodeInterrupt` を送出します。ホストは好きなときに実行を再開し、ノードは再びポーリングします。ステップの中ではポーリングせず、投入は再送されません。
  - Program でも実行は同じように一時停止します。そのジャーナルは投入を保持し、ポーリングとダウンロードは毎回送り直すため、再開した実行はジョブの今の状態を見ます。
- **使用量、予算、キャンセル。**
  - 呼び出しが報告したトークン使用量は、実行の使用量に加算されます。使用量を報告しない呼び出し（moderation、ダウンロード）は何も加算しないため、実行のほかの呼び出しの合計を消しません。
  - 呼び出しはチャット呼び出しと同じ経路をたどります（`make_endpoint_request`）。実行の provider 呼び出し broker がこれらをジャーナルに記録してリプレイし、Program ではホストが `add_core_endpoint_node` でノードを登録します。
  - model token 予算または managed budget lease は、エンドポイント呼び出しに出力の上限がないため、送信する前にその呼び出しを拒否します。価格付きの Program バインディングは `P_MONEY_UNMODELED` で拒否します。
  - Strict ランタイムのインターポジションはチャット呼び出しだけを対象とするため、エンドポイントノードは迂回させずに拒否します。
  - 実行のキャンセルトークンと deadline は呼び出しに届きます。

<a id="topology-schema-export-issue-56"></a>
### トポロジースキーマのエクスポート (issue #56)
NeoGraph は *JSON で記述された* グラフを実行します。JSON を差し替えれば、同じエンジンが別のハーネスになります。
同じエンジンが別のハーネスになります。`NodeFactory::export_schema()` は
このエンジンのバージョンが受け付けるトポロジー JSON の正確な機械可読説明を出力します。
そのため外部ツール、特にコードを書かないビジュアルブロックエディター (プライベートな関連リポジトリ NeoGraph Studio、
issue #56) はエンジンからパレットを生成でき、同期ずれを起こしません。
同期しなくなるのを避けるためです。
**3 つのアクセス経路、1 つのドキュメント:**
| 入力元 | 方法 |
|------|-----|
| C++ | `neograph::graph::NodeFactory::instance().export_schema()` → `json` |
| CLI | `./example_export_schema > schema.json` (`examples/52_export_schema.cpp`) |
| Python | `neograph_engine.export_schema()` → `dict` |
**ドキュメント形状:**
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

- **`neograph_version`** は唯一の真実の情報源である
  `pyproject.toml` からコンパイル時に刻印されます。ツールはキャッシュしたスキーマと比較し、
  エンジンより古いパレットなら警告します。
- **`node_types`** は呼び出し時に `NodeFactory` へ登録されているものを反映します。
  埋め込み側のカスタムノード型も現れるため、エクスポート前に登録してください。
  カスタムリデューサーや条件も同様です。3 引数の `register_type` で登録した型には
  宣言済み設定スキーマが付き、2 引数形式は寛容な `{"type":"object"}` になります。
- **往復契約。** トポロジー JSON を出力するツールはローダーを通して往復させ、構造が保たれることを表明すべきです。
  特にトップレベルの `conditional_edges` ブロックは v0.1.0〜v0.1.7 のコンパイラーで暗黙に破棄されていました (v0.1.8 で修正)。
  エンジンのテストスイート (`tests/test_schema_export.cpp`) がこの回帰を防ぎ、
  ツール側でも同様に検証すべきです。
```cpp
#include <neograph/graph/loader.h>
// register custom node types first if you want them in the palette …
auto schema = neograph::graph::NodeFactory::instance().export_schema();
std::cout << schema.dump(2) << "\n";
```

---
## 10.5. 観測 — OpenTelemetry + OpenInference

Python グラフ tracing は `neograph_engine.tracing`、`neograph_engine.openinference` にあります。`otel_tracer` は graph event から run/node span を作り、`openinference_tracer` は `CHAIN` タグと node payload projection を記録します。`neograph_engine.openinference.OpenInferenceProvider` は既存の typed provider を native C++ dispatch observer で包み、Python OpenTelemetry tracer に呼び出しごとの `LLM` span を送ります。C++ では `<neograph/observability/openinference.h>` を使います。

### `otel_tracer` — OTel 形式の span

以下の signature は参照宣言です。既定は `root_name=graph.run`、`node_span_prefix=node.`、`attribute_prefix=neograph` で、`on_event` は任意に graph event を他の利用者に渡します。

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

`NODE_START` は span を開き、`NODE_END` は成功で閉じ、`ERROR` はエラー、`INTERRUPT` は pause を記録します。Node name ごとに重複 event の stack があり、run 終了時に context manager が残った span を閉じます。Trace は exactly-once node 実行や全同時 task の固有 correlation を立証しません。

```python
from opentelemetry import trace
from neograph_engine.tracing import otel_tracer

tracer = trace.get_tracer("my-service")
with otel_tracer(tracer) as cb:
    engine.run_stream(cfg, cb)
```

### `openinference_tracer` — LLM 形式の属性

Graph span は `openinference.span.kind = "CHAIN"`、node input/output payload は JSON `input.value` / `output.value` projection です。Python graph tracing だけでは provider ごとの `LLM` span や vendor charge は作りません。Context attachment は元 Python context に限られ、thread/task 間の parent 伝播には tracing integration による context 保持が必要です。

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

### `OpenInferenceProvider` — Python と C++ の typed dispatch 観測者

Python では `OpenInferenceProvider(inner, tracer, *, span_name="llm.complete")` で構築します。`Provider` の `prepare(request)`、一度限りの `dispatch(prepared)`、`invoke(request)` を継承し、completion API は追加しません。Native wrapper は準備を一度だけ委譲し、dispatch 時に同じ owned handle を観測します。無効または破棄した準備は span を開きません。通常の tracing では承認済み dispatch ごとに LLM span を一つ開き、元 outcome、mode、deadline、キャンセル、event、provider identity をそのまま渡します。Tracing 失敗は provider outcome や例外を置換しません。

以下の関数は代替の呼び出し経路です。`inner` は設定済み `SchemaProvider` などの既存 typed provider、`model` は明示します。モデル呼び出し一回につき一方を選びます。

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

Python `invoke`/`dispatch` は GIL を解放し、tracer adapter は Python 呼び出しと参照破棄時に再取得します。Prepared operation が adapter と tracer を保持するため、dispatch 前に wrapper が回収されても存続します。Parent は prepare 時ではなく dispatch 時の active OpenTelemetry context に従います。Thread/task 間で処理を移す際は context を伝播してください。Wrapper 構築前に `opentelemetry-api` を設置します。

C++ session overload は session teardown でも parent を安全に接続し、raw parent lookup は呼び出し元が parent の寿命を保つ必要があります。Host 所有 tracer は全 operation より長く生存させます。

```cpp
#include <neograph/observability/openinference.h>

// tracer is a host-owned neograph::observability::Tracer adapter.
// Its lifetime must cover the session and every provider operation.
auto session = neograph::observability::openinference_tracer(tracer);
auto observed = std::make_shared<neograph::observability::OpenInferenceProvider>(
    inner_provider, tracer, session);
// Use observed in NodeContext before compiling the graph.
```

Native LLM 属性には公開 role/text projection、宣言済み scalar、既知 count のみを入れます。Native replay block、reasoning、raw wire envelope/event と `PreparedProviderRequest.encoded_body` は trace payload から除外し、本来の custody は request と outcome に残します。既知ゼロは記録し、不明は省略します。Signed span 範囲を超える count は decimal string として記録します。公開 text delta は Python OTel 属性 `{"chunk": text}` を持つ `llm.token` event を生成します。Completion は OK、failure は安全な provider message と ERROR を記録します。Dispatch 例外は元の product error を保ち、span にエラーを記録します。公開 prompt、output、例外メッセージにも application secret があり得るため、exporter に渡すデータを管理してください。

| 属性 | Native 元データ |
|---|---|
| `openinference.span.kind` | `"LLM"` |
| `llm.model_name` | 承認済み prepared model |
| `llm.invocation_parameters` | 存在する宣言済み temperature と output cap |
| `llm.input_messages.{i}.message.role` | 公開 role projection |
| `llm.input_messages.{i}.message.content` | 公開 text part |
| `input.value` / `input.mime_type` | 公開 message JSON / `application/json` |
| `llm.output_messages.{i}.message.role` | 全返却 message の role |
| `llm.output_messages.{i}.message.content` | 公開 text part |
| `output.value` / `output.mime_type` | 連結した公開 text / `text/plain` |
| `llm.token_count.prompt` | 存在する `usage.input_total.value` |
| `llm.token_count.completion` | 存在する `usage.output_total.value` |
| `llm.token_count.total` | 存在する `usage.total.value` |

Token 属性は失敗時の利用可能な partial usage も含め provider usage を報告します。Vendor charge の証明や budget authority の復元にはなりません。Charged/reserved accounting は `UsageAccumulator.authority_snapshot()` / Program の `provider_budget_authority` を使い、nullable usage report と分けて扱います。

### エンドツーエンド: NeoGraph + Phoenix を一つのブロックで

Phoenix を起動し、graph specification、既存 typed provider、明示的 model と `RunConfig` を `trace_graph` に渡します。Helper は graph compile 前に wrapper を設定し、graph/node `CHAIN` span と provider `LLM` span に同じ tracer を使います。実行した provider 呼び出しだけが LLM span を作り、trace count は charged accounting ではなく usage report です。

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

`ParentContextTracer` は呼び出し元の run context を graph worker の provider dispatch に明示的に渡します。LLM span の parent は run root であり、全同時 task の node 別 ancestry を保証しません。[OpenInference convention](https://github.com/Arize-ai/openinference/blob/main/spec/semantic_conventions.md) は `CHAIN` と `LLM` を定義し、NeoGraph は上記 subset を出力します。公開 text と graph payload の選択や redact は [OpenTelemetry の機密データ指針](https://opentelemetry.io/docs/security/handling-sensitive-data/)に従ってください。

### 注意

OpenTelemetry は opt-in で、base wheel の必須依存ではありません。API/SDK/exporter を別に設置します。同じ run には graph callback として `otel_tracer` または `openinference_tracer` の一つだけを使います。OTLP endpoint と credential は backend 設定と一致させる必要があり、URL だけの交換で全 backend と互換になるとは保証しません。

---

<a id="11-react-graph"></a>
## 11. ReAct グラフ
**ヘッダー:** `<neograph/graph/react_graph.h>`
**名前空間:** `neograph::graph`
標準的な ReAct (Reason + Act) エージェントを、2 ノードのグラフとして作成する便利な関数です。
`llm_call -> tool_dispatch -> (ツール呼び出しならループ、そうでなければ終了)` の構成になります。
```cpp
std::unique_ptr<GraphEngine> create_react_graph(
    std::shared_ptr<Provider> provider,
    std::vector<std::unique_ptr<Tool>> tools,
    const std::string& instructions = "",
    const std::string& model = "");
```

| パラメーター | 型 | 説明 |
|-----------|------|-------------|
| `provider` | `std::shared_ptr<Provider>` | LLM provider |
| `tools` | `std::vector<std::unique_ptr<Tool>>` | エージェントが利用できるツール (所有権は移動します) |
| `instructions` | `std::string` | システムプロンプト / 指示 |
| `model` | `std::string` | 明示的なモデル名; typed provider は既定モデルを選ばない |
**戻り値:** 実行可能な状態までコンパイルされた `GraphEngine`。
これは `Agent::run()` と機能的に同等ですがグラフエンジンとして動作し、チェックポイント、ストリーミングイベント、状態検査など
他のグラフエンジン機能にもアクセスできます。
---
## 11b. Plan-and-Execute グラフ
**ヘッダー:** `<neograph/graph/plan_execute_graph.h>`
**名前空間:** `neograph::graph`
Plan-and-Execute パターン向けの便利なファクトリーです。planner が手順の JSON 配列を出力し、executor が内部 ReAct ループで 1 件ずつ処理し、
responder が `past_steps` から最終回答を組み立てます。
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

| パラメーター | 型 | 説明 |
|-----------|------|-------------|
| `provider` | `std::shared_ptr<Provider>` | 全フェーズで共有する LLM provider |
| `tools` | `std::vector<std::unique_ptr<Tool>>` | executor が呼び出せるツール (所有権は移動します) |
| `planner_prompt` | `std::string` | planner 用の system prompt。手順の JSON 配列で応答するよう model に指示する必要があります (fenced ```json ブロックや先頭の説明文は許容されます) |
| `executor_prompt` | `std::string` | 単一ステップ executor 用の system prompt (内側の ReAct ループ) |
| `responder_prompt` | `std::string` | 最終 synthesis フェーズ用の system prompt |
| `model` | `std::string` | 明示的なモデル名; typed provider は既定モデルを選ばない |
| `max_step_iterations` | `int` | 各ステップで executor 内部のツール呼び出し反復回数の上限 |
**値が入るチャネル:** `plan`, `past_steps`, `final_response`, `messages`。
**戻り値:** 実行可能な状態までコンパイルされた `GraphEngine`。ファクトリーは初回呼び出し時に
3 つのカスタムノード型と `plan_empty` 条件を登録します (`std::call_once` により冪等)。
`examples/14_plan_executor.cpp` に、保留書き込みによるクラッシュ / 再開を扱う Send ファンアウト版があります。
---
<a id="12-llm-module"></a>
## 12. LLM モジュール

### SchemaProvider

`SchemaProvider` は承認済み `sp::descriptor::ValidatedDescriptor`、`sp::runtime::Options`、任意の `SchemaProvider::Defaults` を受け取ります。descriptor は closed/versioned データ admission であり、要求/応答 interpreter や任意 primitive registry ではありません。credential は公開 descriptor でなく runtime options に置きます。Defaults は typed OpenRouter routing と Responses 保持 (`responses_store`) のみで、後者は Responses 専用です。Hosted OpenRouter routing・retention・JSON 形式は宣言済み typed 制御です。Images、Veo、Decisions は別の NeoGraph typed client と別の承認を使い SDK chat grant を継承しません。

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

`Agent::run`、`run_stream`、`complete` は完全な `sp::runtime::Result` を返し `std::vector<sp::Message>` を受け取ります。`run_stream` は実際の各 turn の typed event を受け取り、表示のために回答を破棄して再要求しません。`outcomes()` は各結果を保持し、`usage()` は nullable 報告です。モデルは呼び出し元が明示的に選択します。

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

<a id="13-mcp-module"></a>
## 13. MCP モジュール

**ヘッダー:** `<neograph/mcp/client.h>`
**名前空間:** `neograph::mcp`

Model Context Protocol (MCP) クライアント実装です。MCP サーバーに接続し、利用可能なツールを検出して、NeoGraph の `Tool` インスタンスとしてラップします。

利用できるトランスポートは 2 つです:

- **HTTP** — `MCPClient("http://host:port/mcp")`。URL はサーバーの MCP エンドポイントで、パスとクエリを含めて与えられたとおりに使われ、何も付け加えられません (以前のクライアントは `/mcp` を付け加えていました)。検出されたツールは、`Mcp-Session-Id`、ネゴシエートされたプロトコルバージョン、タイムアウト、カスタムヘッダーを含む元の Streamable HTTP セッションを保持します。
- **stdio** — `MCPClient({"python", "server.py"})`。クライアントは `fork` の前に `PATH` を解決し、`execve` でサブプロセスを実行し、双方向パイプを接続して、子プロセスの stdin/stdout 上で改行区切りの JSON-RPC をやり取りします。サブプロセスは、生成元の `MCPClient` *またはいずれかの `MCPTool`* が生存する間だけ存在します。停止時はまず stdin を閉じて最大 1 秒終了を待ち、次にプロセスグループへ SIGTERM を送ってさらに最大 1 秒待ち、その後 SIGKILL を送ります。

### MCPTool

単一の MCP サーバーツールをローカル `Tool` 実装としてラップします。検出されたツールはトランスポートによらず、元のプロトコルセッションを保持します。

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


通常 `MCPTool` を直接構築することはありません — `MCPClient::get_tools()` がツールを検出してラップします。URL コンストラクタで作ったツールは、最初の呼び出しで MCP セッションを 1 つ開き、以後のすべての呼び出しで再利用し、ツールの破棄時に `DELETE` でそのセッションを終えます。

すべてのディスパッチャーはコンテキスト付きの `execute_async` を呼ぶため、実行のキャンセルトークン (attempt タイムアウト下ではノード attempt のトークン) と期限が、両トランスポートで呼び出しを制限します。キャンセルすると直ちに待機をやめて `graph::CancelledException` を送出し、ツール失敗契約はこれをキャンセルとして扱います。期限またはリクエストタイムアウトは `asio::system_error(asio::error::timed_out)`、つまり `timed_out` の呼び出しを送出します。いずれの場合もサーバーには `notifications/cancelled` で知らせ、実行が呼び出しを待つコルーチンをキャンセルした場合も同様です。その後に届いた応答は破棄されます。stdio は JSON-RPC id で応答をルーティングし、その呼び出しの id はもう待たれておらず、HTTP はリクエストを放棄します。`isError` の結果は通常のツール失敗である `std::runtime_error` を送出します。Python では、`mcp.MCPError` が JSON-RPC エラーの `code`、`data`、`http_status` を保持します。

### MCPClient

MCP サーバーに接続し、初期化ハンドシェイクを行い、ツールの検出と呼び出しのメソッドを提供するクライアントです。

> `MCPClient` はサブクラス化を想定していません — そのまま使います。
> `rpc_call_async()` が唯一のトランスポートレベルの呼び出しで、同期メソッドも同じセッションを駆動します。[`ASYNC_GUIDE.md` §9.5](ASYNC_GUIDE.md#95-mcpclient) を参照してください。

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


**ワイヤプロトコル:** NeoGraph の MCP クライアントは `protocolVersion = "2025-11-25"` を提示し、公式 SDK クライアントと同様に、サーバーが `2025-11-25`、`2025-06-18`、`2025-03-26`、`2024-11-05` のいずれかで応答すれば受け入れます (`MCP_SUPPORTED_PROTOCOL_VERSIONS`)。`2024-11-05` のサーバーには stdio または Streamable HTTP で接続し、以前の HTTP+SSE トランスポートには対応しません。それ以外の応答では、両方のバージョンを示すプロトコルエラーで `initialize` が失敗し、MCP のバージョンネゴシエーションが求めるとおりクライアントは切断します。HTTP は以後のすべてのリクエストの `MCP-Protocol-Version` ヘッダーでネゴシエートされたバージョンを送ります。MCP `2026-07-28` (`initialize` もセッションもない) にはまだ対応していません。その版だけを提供するサーバーは `-32022` でハンドシェイクを拒否し、呼び出し元には `MCPError` として届きます。

**セッションとトランスポートの所有:** 両方のコンストラクタは同じプロトコルセッションを作ります。セッションは JSON-RPC id、応答の検証、初期化 (initialized 通知を含む)、ページ送り、ツール結果の変換を所有します。まだハンドシェイクしていないクライアントで `rpc_call_async()` から送ったリクエストは、先にハンドシェイクを行います。エンドポイント URL、ヘッダー、ネゴシエートされた `Mcp-Session-Id`、リクエストタイムアウトは HTTP トランスポートだけが所有し、サブプロセス、パイプ、リーダー、応答 id の多重分離器は stdio トランスポートだけが所有します。検出されたツールはプロトコルセッションを保持するため、`MCPClient` の破棄後も使えます。セッションは、クライアントまたはツールへの最後の参照が解放されたときに終わります。

**トランスポート規則への準拠:** 両トランスポートはサーバーのリクエストに応答します。`ping` には空の結果を、それ以外のリクエストには `-32601` を返します。クライアントはクライアントケイパビリティ (roots、sampling、elicitation) を何も宣言しないためです。メッセージは `method` がないときだけ応答とみなされるため、保留中の呼び出しの id を再利用したサーバーリクエストがその呼び出しを完了させることはありません。サーバーからの JSON-RPC バッチ (MCP `2025-03-26`) はメッセージごとに処理し、その中のリクエストへの応答は 1 つの配列で返します。HTTP では、クライアントは JSON と Server-Sent Event の応答を読み (WHATWG の規則に従い、CRLF、LF、CR の行末、複数行の `data`、`id`、`retry`)、呼び出しのイベントストリームで届いたサーバーリクエストにはそのストリームを開いたまま別の POST で応答し、応答が届いたらストリームの読み取りをやめます。サーバーが応答の前に閉じたストリームは、ストリームの `retry` 間隔の後に `GET` と `Last-Event-ID` で再開します。`initialize` の後、クライアントは公式クライアントと同様に、どの呼び出しにも属さないメッセージのためにセッションのオプションの `GET` イベントストリームを開きます。それを持たないサーバーは 405 を返します。クライアントはこれを失敗ではなくストリームがないことと受け取り、再び要求せず、セッションと呼び出しはそのまま続きます。セッション id を付けたリクエストにサーバーが 404 を返したら、セッションは終わっています。クライアントは新しいセッションを初期化し、リクエストを 1 回だけ送り直します。`shutdown()` と最後の参照の解放は、最大 2 秒に制限された `DELETE` でセッションを終えます。

**通知:** サーバーが `notifications/tools/list_changed` を送ると、`set_tool_list_changed_handler()` で登録したハンドラーが呼ばれます。そのときに `get_tools()` または `get_tool_definitions()` を読み直してください。それまでは、キャッシュされたスキーマが古い可能性があるため `outputSchema` の検査を省略します。ハンドラーはクライアントのトランスポートスレッドで実行されるため、すぐに戻る必要があり、クライアントを呼び出してはいけません。

**強化された stdio (`StdioClientConfig`):** `argv`、`cwd`、`environment` は明示的です。`replace_environment` を使うと設定した項目がサーバーの環境のすべてになり、使わない場合はサーバーがこのプロセスの環境を継承し、設定した項目がそれに追加されるか上書きします (`PATH` の項目は `argv[0]` を探す場所も決めます)。セッションはサーバーの stderr を読み続け、最後の `max_stderr_bytes` (64 KiB) を保持します。stderr への出力でセッションが終わることはありません。`startup_timeout` は起動から initialized 通知までを、`request_timeout` は各リクエストを制限します。キャンセルまたはタイムアウトしたリクエストは強化されたセッションを終えます。クライアントは `notifications/cancelled` を送り、サーバーが止まるよう `cancel_grace` (500 ms) 待ってからプロセスツリーを終了し、以後のリクエストは `MCPFailure::shutdown` で失敗します。

**制限と失敗:** `rpc_call_async(method, params, deadline, cancel_token)` と `call_tool_result_async()` は、絶対的な steady-clock の期限と `graph::CancelToken` を受け取ります。キャンセルと期限はその呼び出しの待機をやめ、サーバーに `notifications/cancelled` で知らせ (`initialize` には送らない)、共有の stdio プロセスや他の応答には影響しません。`initialize(name, deadline, cancel_token)` も同じようにハンドシェイクを制限しますが、MCP は `initialize` をキャンセルしないため、サーバーには知らせません。その場合 stdio サーバーは 2 回目のハンドシェイクを拒否することがあります。HTTP はさらにリクエストごとに `MCPClientConfig::request_timeout` を適用します。トランスポートとワイヤの失敗は `MCPTransportError` を送出します。これは `failure()` (`connection`、`timeout`、`cancelled`、`shutdown`、`http_status`、`protocol`) と `http_status()` (HTTP 以外の失敗では 0) を公開する `std::runtime_error` のサブクラスです。JSON-RPC エラー応答は、サーバーの `code()` と `data()` を持つ `MCPError` で、HTTP のエラーステータスがそれを運んだ場合も同じです (そのとき `http_status()` がそのステータスを示します)。どちらのクラスもツールレベルの `isError` の結果を表しません。その結果は `call_tool_result()` で確認してください。

| メソッド | 説明 |
|--------|-------------|
| `MCPClient(url)` | MCP エンドポイント `url` を与えられたとおりに使う HTTP モードのクライアントを構築します |
| `MCPClient(argv)` | サブプロセスを起動し、stdio モードのクライアントを構築します。`argv[0]` は fork の前に `PATH` で解決され、exec の失敗は最初の RPC で接続エラーとして現れます。安全のため Windows の `.bat`/`.cmd` を拒否します |
| `MCPClient(StdioClientConfig)` | 明示的な環境、上限付きの stderr 取得、期限、キャンセルの猶予を持つ強化された stdio 起動 |
| `initialize(client_name)` | MCP の初期化ハンドシェイクを 1 回行います。繰り返しの呼び出しは冪等で、プロトコル/トランスポートの失敗は例外を送出します |
| `initialize(client_name, deadline, cancel_token)` | 期限とキャンセルトークンで制限されたハンドシェイク |
| `get_initialize_result()` | ネゴシエートされたプロトコル、ケイパビリティ、サーバー情報、instructions、生の結果を返します |
| `list_tools(cursor)` | カーソルを不透明なものとして扱い、1 ページを取得します |
| `get_tool_definitions()` | すべてのページをたどり、完全なツールメタデータを保持します |
| `get_tools()` | すべてのページを検出し、セッションを保持する `MCPTool` インスタンスを返します |
| `call_tool(name, arguments)` | 名前と引数でツールを呼び出します。生の JSON 応答を返します |
| `call_tool_result(name, arguments)` | content、structured content、`isError`、`_meta` を保持する型付きの結果 |
| `call_tool_result_async(name, arguments, deadline, cancel_token)` | `rpc_call_async` と同様に、絶対的な steady-clock の期限とキャンセルトークンで制限される型付きの呼び出し |
| `set_tool_list_changed_handler(handler)` | `notifications/tools/list_changed` で `handler` を呼びます。空のハンドラーで呼び出しを止めます |
| `shutdown()` | クライアントと、それが生成したすべてのツールを恒久的に終えます。HTTP セッションには `DELETE`、stdio サーバーには順序立った停止 |
| `rpc_call_async(method, params, deadline, cancel_token)` | 他のすべてのメソッドが使うトランスポートレベルの JSON-RPC 呼び出し |

**ツールの提供。** `MCPServer` (`<neograph/mcp/server.h>`) は、登録されたハンドラーで `tools/call` に応答します。ハンドラーの失敗がクライアントにどう届くかは [Protocol failure mapping](#protocol-failure-mapping) にあります。`2025-11-25`、`2025-06-18`、`2025-03-26`、`2024-11-05` を求めるクライアントには、その版とその版が定義するフィールドで応答し (`2025-06-18` には `icons` と `execution` がなく、`2025-03-26` には `title`、`outputSchema`、`_meta`、`structuredContent`、`resource_link` のコンテンツもなく、構造化データだけの結果はそれをテキストで運びます。`2024-11-05` には `annotations` と音声のコンテンツもありません。その版にないコンテンツブロックは、その JSON を持つテキストブロックで送ります)、それ以外の要求には `2025-11-25` で応答します。`protocol_version()` がその結果を示します。`2025-03-26` をネゴシエートしたクライアントは、その版が受信側に求めるとおり JSON-RPC バッチを送ることができ、`2024-11-05` のクライアントも同様です。バッチは stdio と HTTP で 1 つの配列で応答されます。後の版はバッチを廃止し、initialize はバッチに入れられないため、そのようなバッチは `-32600` のエラー 1 つ (HTTP 400) です。`handle_message(message, reply)` は各メッセージの結果 (応答、バッチの配列、またはなし) を呼び出し元に渡します。登録では、`inputSchema` または `outputSchema` が `"type": "object"` と述べていないツール (以前の既定値である空のオブジェクトは `{"type": "object"}` として読む)、検証器が対応する JSON Schema 2020-12 でないスキーマ、SEP-986 の `A-Za-z0-9_.-` 1〜128 文字から外れる名前を拒否します。引数は JSON Schema 2020-12 の検証器 (`$ref`、`anyOf`、`minimum`、整数としての整数値 `1.0`、そして core、applicator、validation の語彙の残り) で検証されます。未知のツールは JSON-RPC エラー `-32602` で、エラー応答は読み取れたリクエストの id を運び、読み取れなければ id を持たず、`null` は使いません。`MCPHttpServer` (`<neograph/mcp/http_server.h>`) は同じものを Streamable HTTP で提供します。セッションは `DELETE` で、リクエストのないまま `session_idle_timeout` (30 分) たったとき、または `max_sessions` (64) で新しいクライアントに空きが必要で、それが処理中のリクエストのない最も長く使われていないセッションであるときに終わり、その id には以後 404 が返ります。ツール呼び出しは戻るまで HTTP スレッドを 1 つ占有するため、スレッドは必要に応じて `max_http_threads` (64) まで起動します。`response_timeout` を超えたリクエストは 504 を受け取り、そのツール呼び出しはキャンセルされます。`DELETE` はその接続を閉じます。`protected_resource_metadata` があれば、サーバーは RFC 9728 のメタデータを公開し、すべての 401 チャレンジでその場所を示します。`initialize` 以降のリクエストは `MCP-Protocol-Version` にサポートするどのリビジョンを書いてもかまいません。セッションはネゴシエートしたリビジョンのまま動作し、セッション内で別のリビジョンを書いた最初のリクエストは stderr に記録され、サポートしない値は 400 です。ループバックにバインドしたサーバーは、`Host` が `localhost`、`127.0.0.1`、`[::1]` か `allowed_hosts` にある名前のときだけ応答します。それ以外の `Host` は 403、ないか形式の誤った `Host` は 400 です。リモートのバインドは、`allowed_hosts` に名前があるときだけ `Host` を確認します。

**HTTP の使い方:**

```cpp
neograph::mcp::MCPClient client("http://localhost:8000/mcp");
client.initialize();
auto tools = client.get_tools();
```


**stdio の使い方:**

```cpp
// argv[0] is resolved through PATH before fork; inherited fds close before execve.
neograph::mcp::MCPClient client({"python", "/path/to/server.py"});
client.initialize();
auto tools = client.get_tools();   // Tools retain the protocol session/process.
```


---

<a id="14-util-module"></a>
## 14. Util モジュール
**ヘッダー:** `<neograph/util/request_queue.h>`
**名前空間:** `neograph::util`
### RequestQueue
ワーカースレッドプールとバックプレッシャーに対応したロックフリーのリクエストキューです。
サーバーアプリケーションで HTTP 接続の受け付けと LLM 呼び出しの同時実行数を分離します。
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

| コンストラクターパラメーター | 型 | デフォルト | 説明 |
|-----------------------|------|---------|-------------|
| `num_workers` | `size_t` | `128` | プール内のワーカースレッド数。0 は `std::invalid_argument` を送出 |
| `max_queue_size` | `size_t` | `10000` | 保留できるタスクの最大数。超過分は拒否 |
| メソッド | 説明 |
|--------|-------------|
| `submit(task)` | pending capacity を原子的に予約してから callable を enqueue します。受理時は `first=true`。満杯または閉じたキューは invalid future と `false` を返し、内部 enqueue 失敗はエラーを伝播する valid future と `false` を返します。受理済み future は完了時に解決するかタスク例外を伝播します。 |
| `close()` | 新しい作業を冪等に拒否します。外部呼び出しは全 worker の終了を待ち、未取得作業は `std::runtime_error("RequestQueue is closed")` で完了します。取得済み callable は完了できます。callable 自身が `close()` を呼んで終了を開始できますが、自分自身は待ちません。 |
| `is_closed()` | `close()` が新しい作業の拒否を開始したか報告します。 |
| `stats()` | 現在のキュー統計のスナップショットを返す |
キュー内部ではロックフリーの enqueue/dequeue に `moodycamel::ConcurrentQueue` を使い、
条件変数でアイドル状態のワーカーを起こします。
**使用方法:**
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
<a id="usage-examples"></a>
## 使用例
<a id="minimal-react-agent"></a>
### 最小 ReAct エージェント



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


<a id="custom-graph-with-conditional-routing"></a>
### 条件付きルーティングを持つカスタムグラフ
条件エッジを持つグラフを構築します:
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

<a id="human-in-the-loop-with-checkpointing"></a>
### チェックポイントを使う Human-in-the-Loop
人間の承認に中断を使います:
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

<a id="dynamic-fan-out-with-send"></a>
### Send による動的ファンアウト
map-reduce パターンに `Send` を使います:
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

各 `Send` は異なる入力で `"process_item"` ノードをディスパッチします。エンジンはすべての Send を実行し、
<a id="routing-override-with-command"></a>
### Command によるルーティング上書き
状態を同時に更新し、ルーティングを制御するために `Command` を使います:
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

`Command` が返されると、その `updates` が状態に適用され、通常のエッジルーティングを迂回して
指定された `goto_node` へ直接移動します。
<a id="schemaprovider-multi-llm-support"></a>
### SchemaProvider によるマルチ LLM 対応

`SchemaProvider` は承認済み `sp::descriptor::ValidatedDescriptor`、`sp::runtime::Options`、任意の `SchemaProvider::Defaults` を受け取ります。descriptor は closed/versioned データ admission であり、要求/応答 interpreter や任意 primitive registry ではありません。credential は公開 descriptor でなく runtime options に置きます。Defaults は typed OpenRouter routing と Responses 保持 (`responses_store`) のみで、後者は Responses 専用です。Hosted OpenRouter routing・retention・JSON 形式は宣言済み typed 制御です。Images、Veo、Decisions は別の NeoGraph typed client と別の承認を使い SDK chat grant を継承しません。

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


<a id="mcp-tool-integration"></a>
### MCP ツール統合

`Agent::run`、`run_stream`、`complete` は完全な `sp::runtime::Result` を返し `std::vector<sp::Message>` を受け取ります。`run_stream` は実際の各 turn の typed event を受け取り、表示のために回答を破棄して再要求しません。`outcomes()` は各結果を保持し、`usage()` は nullable 報告です。モデルは呼び出し元が明示的に選択します。

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

## このツアーの対象外
`include/neograph/` 以下のヘッダーには、上で扱っていない公開 API も含まれています。
以下の各節は、その正式なソースレベルリファレンスへの短い案内です。
### `neograph::a2a` — Agent-to-Agent プロトコル

**ヘッダー:** `<neograph/a2a/{client,server,types,a2a_caller_node}.h>`
`A2AServer` は graph(または admitted な `ProgramVersion`)を HTTP と SSE による A2A JSON-RPC binding で公開し、`A2AClient` はリモート agent を呼び出します。A2A 1.0(v1.0.1)が主要バージョンで、0.3 は互換 dialect です(`WireDialect::{V1_0,V0_3}`)。公式 SDK(a2a-sdk 1.2.2、@a2a-js/sdk 1.3.0 と 0.3.14)が `bindings/python/tests/interop/a2a_suite` で双方向を検証します。

#### A2A バージョンポリシー

- **リクエスト.** server は各リクエストに、header または `A2A-Version` query parameter の `A2A-Version` が示す dialect で応答します。バージョンがなければ 0.3 です(spec §3.6.2)。Major.Minor の `1.0` と `0.3` だけを扱い、patch は無視します。`0.2`、`1.5`、`2.0` などそれ以外は、対応バージョンを示す VersionNotSupportedError(`-32009`)になります。両方の method 表記(`SendMessage`、`message/send`)をどちらのバージョンでも受け付けます。
- **Card.** card はバージョンごとに提供します。`A2A-Version: 1.0` には純粋な 1.0 `AgentCard` を返し、バージョンのないリクエストには 0.3 card を返します。0.3 card も `supportedInterfaces` の先頭に 1.0 interface を載せます。応答には `ETag`、`Cache-Control: public, max-age=300`、`Vary: A2A-Version` が付き、`If-None-Match` には 304 で応えます。`AgentCard::url` を設定しない場合、card は client が使ったアドレスを広告します。すべての skill は少なくとも 1 つの tag を持ちます。card は実装していない push notification、extended card、HTTP+JSON・gRPC binding を主張しません。
- **Client.** client は最初の呼び出しの前に card を取得し、`1.x` または `0.x` バージョンの最初の JSONRPC interface を選びます。URL が client の `base_url` と一致する interface を優先します。card を取得できない場合だけ probe します。まず 1.0 を送り、`-32601`、`-32009`、HTTP 400 なら 0.3 に切り替えて結果を記憶します。選んだ interface URL が `base_url` と同じ origin なら RPC はその URL に送られるので、endpoint が `/a2a/v1` の agent も動作します。`set_allow_cross_origin_interfaces(true)` で許可しない限り card は client を別 origin に向けられず、その場合 RPC は `base_url` に送られ、client は従わなかった origin を示す警告を stderr に書きます。`endpoint_url()` は使用中の endpoint を返し、message id はランダムな UUID です。
- **0.3.** 0.3 は同じ意味を持つ互換 dialect として残ります。新機能は 1.0 を対象にします。

#### Task と method

すべての run は専用の worker thread で実行されます。stream や blocking send はそれを観測するだけなので、task は切断された接続より長く存続します。`SendMessage` は task が terminal または interrupted になるまで block しますが、`returnImmediately`(0.3 では `blocking: false`)を指定すると SUBMITTED の task を即座に返します。`SendStreamingMessage` と `SubscribeToTask`(0.3 では `tasks/resubscribe`)は Task を最初に送り、続けて status・artifact update を送り、terminal または interrupted の状態で閉じます。0.3 の stream は `final: true` で終わります。サーバーが再起動後に復旧した Program task は、このプロセスが run を開始していないため、専用の channel を持ちません。この task に `SubscribeToTask` を呼ぶと、Task を送ったあと永続 run を追い、run に結果が出るか cancel が確定した時点で、結果 artifact と最終 status を他の run と同じように送ります(停止した run は `TASK_STATE_INPUT_REQUIRED` で終わります)。復旧後にすでに終わっていた run は先に読み出し、terminal として `-32004` で答えます。run を追えないときは、stream が `-32603` のエラーイベント(ErrorInfo reason `RUN_UNAVAILABLE`)で終わり、このサーバーに run がない task なら呼び出しが `-32603` で答えます。空のまま閉じることはありません。5 秒間静かだった stream は SSE コメント `: keep-alive` を書きます。アイドル接続にタイムアウトを掛けるクライアントやプロキシが、まだ作業中の run を切断しないようにするためです。`GetTask` と `ListTasks` は `historyLength` に従います(0 なら history を省略)。`ListTasks` は `contextId`、`status`、`statusTimestampAfter` で絞り込み、`pageSize`(1〜100、既定 50)と `pageToken` で新しい順にページングし、`includeArtifacts` がなければ artifact を省略します。すべての status に timestamp が付きます。

task id は server が生成します。未知の `taskId` を持つ message は `-32001`、terminal な task は `-32004`、task と異なる `contextId` は `-32602`、まだ実行中の task は `-32603`(ErrorInfo reason `TASK_BUSY`)で失敗します。INPUT_REQUIRED の task に送った message は、interrupt が保存した checkpoint から run を再開します。`GraphAgentAdapter::build_resume_value` が message を resume value に変換し、checkpoint store を持たない engine はその message を input として graph を再実行します。Program を基にした task は、まだ A2A でその回答を受け取れません。message は `-32004` になり、task は待ち続けます。NeoGraph の collaboration envelope は送信者が決めた冪等な task id を保ちます([A2A collaboration 拡張](#a2a-collaboration-extension))。リクエストは dialect ごとに検証し、違反は `google.rpc.BadRequest` の field violation 付きで `-32602` になり、すべての error は `google.rpc.ErrorInfo` detail を持ちます。`CollaborationAuthenticator` がある場合、card は Bearer security scheme を宣言し、リクエストの認証が必須なら全体の requirement も宣言します。credential がなければ `WWW-Authenticate` 付きの HTTP 401、認証済みでも collaboration link を使えない peer は HTTP 403 になります。

<a id="a2a-error-mapping"></a>
#### A2A エラーマッピング

run の失敗 class は [プロトコル失敗のマッピング](#protocol-failure-mapping) のとおり peer に届きます。task の状態、status message としての public message、`metadata["neograph/failure"] = {code, retryable, retryable_by_class}` で、例外テキストは決して届きません。完了した run は agent の回答を status message とし adapter の artifact を伴う `COMPLETED`、step limit は code `NG.StepLimit` の `FAILED`、failure exit は exit が宣言した code の `FAILED` です。server が拒否するリクエスト:

| リクエスト | A2A の結果 |
|---|---|
| 未知の task を指す message | `-32001` TaskNotFoundError |
| terminal な task への message | `-32004` UnsupportedOperationError。collaboration の再配送はその task を返す |
| `contextId` が task と異なる message | field violation 付きの `-32602` |
| まだ実行中の task への message | `-32603`、ErrorInfo reason `TASK_BUSY`、domain `neograph` |
| input を待つ Program task への message | `-32004`。task は待ち続ける(Program の input 要求に A2A で答える機能はまだない) |
| in-flight 上限に到達 | `-32603`、ErrorInfo reason `RESOURCE_EXHAUSTED`、domain `neograph`、metadata `retryAfterSeconds` |
| agent が求める credential がない、または不正(collaboration message、または strict 認証でのすべてのリクエスト) | `WWW-Authenticate` 付きの HTTP 401。body は ErrorInfo reason `UNAUTHENTICATED`、domain `neograph` の JSON-RPC `-32600` |
| collaboration link が許可しない認証済み peer | HTTP 403。body は ErrorInfo reason `PERMISSION_DENIED`、domain `neograph` の JSON-RPC `-32600` |
| terminal な task の cancel | `-32002` TaskNotCancelableError。task は変わらない |
| push notification 設定; extended card; card が `streaming = false` のときの streaming method | `-32003`; `-32004`; `-32004` |
| 1.0 でも 0.3 でもない `A2A-Version` | `-32009` VersionNotSupportedError |

すべての error は `google.rpc.ErrorInfo` を 1 つ持ち、行に `neograph` とない限り domain は `a2a-protocol.org` です。

<a id="a2a-collaboration-extension"></a>
#### A2A collaboration 拡張

`CollaborationMailbox` を持つ Program agent は NeoGraph の collaboration envelope を受け付け、両バージョンの card の `capabilities.extensions` でそれを宣言します。`uri` は `a2a::COLLABORATION_EXTENSION_URI`(この節)、`required` は false、`params` は `{"format": "neograph-a2a-collaboration-v1", "metadataKey": "neograph_collaboration"}` です。そのような message は data part `{format, envelope}` と `metadata.neograph_collaboration` を持ち、リンクされた collaboration の認証済み peer だけが送れます。送信者が受信側の task id を先に決めるので、最初の配送はその id で task を作り、再配送は新しい run を始めずに既存の task を返します。通常の A2A client には不要です。

#### Client の stream と Python

SSE は LF/CRLF/CR、comment、multiline data、最後の unterminated data を処理します。opening task snapshot、status update(metadata を含む)、artifact の append/replace update を蓄積して返す task を作ります。event を観測すると external callback がなくても dialect の再送を禁止します。non-SSE の RPC error は `A2ARpcError::code()` で code を保持し、non-2xx の HTTP が成功 task になることはありません。caller node の回答優先順位は final/interrupted agent status text、最初の artifact text、最後の agent history text で、progress status や user history は回答を上書きしません。汎用の `async_post_stream` は空でない固定 `Content-Length` の body を status とともに一度渡し、長さ 0 では chunk を出しません。body limit、early EOF、既に buffer された余剰は拒否します。redirect/chunked/close-delimited の規則は変わりません。

Python の `neograph_engine.a2a` は `WireDialect`、`AgentInterface`、`AgentCard.supported_interfaces`/`raw`、`Part.media_type`、`MessageSendConfiguration`、`MessageSendParams`、`StreamEvent` と status/artifact record を提供します。`A2AClient.wire_dialect()` は enum または `None` を返し、`set_authorization_header()`、`set_allow_cross_origin_interfaces()`、`endpoint_url()` は native client と同じです。`send_message(params)` は multipart overload です。`send_message_stream(text, on_event, task_id="", context_id="")` または `(params, on_event)` は蓄積した `Task` を返し、bool を返す callback に owned event snapshot を渡します。blocking call は GIL を解放します。`a2a.A2ARpcError.code` はリモートの整数 code を保持します。

**公開ヘッダー:** [`include/neograph/a2a/`](../include/neograph/a2a/)。

### `neograph::acp` — Agent Client Protocol
**ヘッダー:** `<neograph/acp/{server,types}.h>`
stdio 上の改行区切り JSON によるエディター↔エージェント JSON-RPC 2.0 です (Zed、Gemini CLI、Neovim CodeCompanion)。ACP プロトコルバージョン 1 で、公式スキーマ v1.25.0 と公式 Python (0.12、1.0)・TypeScript SDK で検証しています。client→agent は `initialize`、`session/{new,resume,prompt,cancel}` です。agent→client は `session/update`、`session/request_permission` で、`fs/*`、`terminal/*`、`elicitation/*` はクライアントが capability を通知したときだけ、遅延バインドされた `ACPClient` から送ります。クライアントが `error` で答えると、メソッド、コード、メッセージ、`data` を持つ `acp::ACPClientError` を送出し、リクエストが成功しえないことを示すコードならリトライされない `acp::ACPClientRefusal` になります([プロトコル失敗のマッピング](#protocol-failure-mapping))。`ACPServer::handle_message` は各プロンプトターンをワーカースレッドで実行し、同時に最大 `max_inflight_prompts` (32) 件、セッションごとに 1 件に制限します。拒否したターンは JSON-RPC `-32603` で、`data.reason` は `"overloaded"` または `"session_busy"` です。`-32000` は ACP の "Authentication required" なので使いません。`initialize` 前の呼び出しは `-32600`、不明なセッションは `-32002` で、エージェントが知っていてもチェックポイントのないセッションの `session/resume` は成功します。

プロンプトは text と `resource_link` ブロック (基本コンテンツで、既定アダプターはリンクを入力に `[name](uri)` として入れます) を受け付け、image・audio・埋め込みリソースは `capabilities().prompt` で通知したときだけ受け付けます。不正な、または通知していないブロックは `-32602` です。既定アダプターは `_acp_session_id`、`_acp_cwd`、`_acp_additional_directories`、`_acp_mcp_tools` を設定します。変えるには `build_session_state` (または従来の `build_initial_state`) をオーバーライドします。`agentInfo` は常に ACP `Implementation` で、欠けた name や version は補います。

ターンの終わり方:

| NeoGraph の結果 | ACP の結果 |
|---|---|
| 完了 | `agent_message_chunk` 1 件 (空テキストならなし) の後に `stopReason: "end_turn"` |
| `session/cancel` による取り消し | `stopReason: "cancelled"` |
| ステップ上限 (`RunConfig::max_steps`) | `stopReason: "max_turn_requests"` |
| モデルトークン予算の超過 | `stopReason: "max_tokens"` |
| プロバイダーの拒否またはコンテンツフィルター (`NG.SafetyBlocked`) | `stopReason: "refusal"` |
| `NodeInterrupt` | 同じターン内の `session/request_permission`。回答で実行を再開します |
| Hook によるブロック (`NG.HookBlocked`)、ノード・ツールの失敗 (`NG.TaskFailed`、作成者コード)、タイムアウト (`NG.Timeout`)、プロバイダーの失敗 (`NG.ProviderFailed`)、金額・クォータ予算 (`NG.BudgetExhausted`)、設定・ロジック・不明な失敗 | JSON-RPC エラー `-32603`、message = 公開メッセージ、`data: {code, retryable, retryable_by_class}`([プロトコル失敗のマッピング](#protocol-failure-mapping)) |
| クライアント以外による取り消し (ノードの中断、シャットダウン) | JSON-RPC エラー `-32800` |

例外テキスト (`what()`) はクライアントに届きません。message は型付きの公開メッセージ (`NodeFailureError::public_message`) か失敗クラスごとの汎用文で、詳細はホストのログにだけ残ります。割り込みは、理由をタイトル、ペイロードを `rawInput` とし、`_meta["neograph/interrupt"]` と選択肢 "approve" (`allow_once`)・"reject" (`reject_once`) を持つツール呼び出しになります。選ばれた option id が再開値です。変えるには `interrupt_permission` と `interrupt_resume_value` をオーバーライドします。`cancelled` の回答はターンを取り消しで終えます。アダプターが `std::nullopt` を返すか、クライアントが回答できない場合、ターンは `data.code: "NG.Interrupted"` で失敗し、セッションは一時停止のまま残りません。エージェントの再起動後のように `session/resume` がチェックポイントから復元した一時停止には、次のプロンプトのテキストが回答します。

`session/cancel` は進行中のターンだけを取り消し、ターンが実行中でないときの取り消しは無視します (メッセージの順序を入れ替えるトランスポートは `set_pre_armed_cancel(true)` で従来の事前取り消しを維持できます)。`requestId` が実行中の `session/prompt` の id である `$/cancel_request` 通知も同じようにターンを取り消し、そのプロンプトには引き続き応答が 1 つ (`stopReason: "cancelled"`) 返ります。未知または終了済みなど、ほかのリクエストの `requestId` は無視します。取り消し後、保留中の agent→client 要求には回答のために 500 ms が与えられ、その後サーバーは `$/cancel_request` を送ってターンを終えます。権限要求は `ACPClient::set_permission_timeout` で設定しない限りタイムアウトしません。stdin が EOF に達すると、`run()` は実行中のターンが `set_eof_grace` (1 秒) の間に終わるのを待ってから取り消します。

`session/new` と `session/resume` の stdio `mcpServers` は NeoGraph の MCP クライアント (`NEOGRAPH_BUILD_MCP_CLIENT` でビルド) でセッションの `cwd` から起動し、環境は最小限で、エージェントの `PATH`(Windows では `SystemRoot` も)にクライアントの `env` を加えたものです。プロバイダーの API キーを含みうるエージェントのその他の環境は渡しません。ノードは `ACPClient::session_tools(session_id)` でそのツールを使います。HTTP と SSE の MCP サーバーは通知せず、`-32602` で拒否します。起動できないサーバーは要求を `-32603`、`data.reason: "mcp_server_unavailable"` で失敗させます。

**公開ヘッダー:** [`include/neograph/acp/`](../include/neograph/acp/)。
### `neograph::async` — HTTP/SSE/WS ヘルパー
**ヘッダー:** `<neograph/async/{conn_pool,http_client,sse_parser,ws_client,curl_h2_pool,run_sync}.h>`
汎用 NeoGraph HTTP/SSE/WebSocket helper は非 provider 統合で使えますが、SchemaProvider の transport・codec・retry 権限ではありません。typed chat-family 呼び出しは SDK runtime と `ProviderMode` を使い、旧 Responses WebSocket provider 経路と descriptor stream parser は削除されました。

**公開ヘッダー:** [`include/neograph/async/`](../include/neograph/async/)。
### 永続チェックポイントバックエンド
**ヘッダー:** `<neograph/graph/postgres_checkpoint.h>`、
`<neograph/graph/sqlite_checkpoint.h>`
`PostgresCheckpointStore` — libpq ベースの 3 テーブルスキーマ (`neograph_*`) で、
チャネル blob を `(thread_id, channel, version)` をキーに重複排除します。LangGraph の `PostgresSaver` と同等です。
非同期の初回/差し替え接続は全ホストに対する 1 つのグローバル期限を使います。
正の `connect_timeout` は接続文字列へ直接書き込まれ (最小 2 秒)、それ以外は 30 秒の安全なデフォルトになります。
環境変数とサービスファイルのタイムアウト値は初回接続確立前には利用できず、そのデフォルトが使われます。
`SqliteCheckpointStore` — 同じ形の単一ファイルバックエンドで、
エッジ / 単一ホスト配置に適しています。
**公開ヘッダー:**
[`PostgresCheckpointStore`](../include/neograph/graph/postgres_checkpoint.h) ·
[`SqliteCheckpointStore`](../include/neograph/graph/sqlite_checkpoint.h)。
### このツアーにないその他の公開 API
- **`neograph::AsyncTool`** — コルーチン向き (HTTP fetch、MCP 呼び出し) の仕事に対して
  `execute_async(json)` を公開する `Tool` の対となる実装です。同期 `execute()` は
  `run_sync` を経由して `final` ルーティングされます。
- **`neograph::graph::NodeCache`** — ノード単位のメモ化です。
  構築時に `EngineConfig::cached_nodes` でオプトインします (setter は互換性のため残ります)。
- **`neograph::graph::create_deep_research_graph`** —
  `examples/25_deep_research.cpp` で使われます。
  `BriefNode` による LLM の書き換え、`FinalReportNode` のトークン上限再試行、
  `ClarifyNode` の HITL gate を含みます。`DeepResearchConfig::model` は必須で、
  各ステップは `DeepResearchConfig::controls` と設定された出力上限を送り、
  独自のサンプリング設定は送りません。
このツアーに必要な型がない場合は `include/neograph/` を直接確認してください。
すべての公開ヘッダーにリファレンス文書があります。
