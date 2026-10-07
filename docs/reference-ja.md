<!-- neograph-i18n: source=docs/reference-en.md locale=ja source_sha256=ba7bc78e107bf6ae0eb0f629071a28038e4ff8c2eef2dfa7aa4f435d4dab63bb -->
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
追加モジュールは `include/neograph/{a2a,acp,async}/` に公開ヘッダーを持ちます。個別ナラティブは延期されているため、正確な契約はヘッダーを参照してください。存在するだけで現統合資格検証を主張しません。

CMake 3.20 以上が必要です。Core は所有 typed provider 契約を公開するため `NEOGRAPH_BUILD_LLM=OFF` でも SchemaProvider runtime は必須です。明示的な `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` が優先され、それがなければ設置済み `SchemaProvider` runtime package を探します。なければ `NEOGRAPH_FETCH_SCHEMAPROVIDER=ON`（既定）の時に `cmake/NeoGraphSchemaProvider.cmake` が固定した不変 GitHub archive を取得します。Offline build は SDK を設置し、`CMAKE_PREFIX_PATH` に prefix を設定して `-DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` を渡します。Sibling checkout は推測せず、削除済み bundled interpreter も選択しません。NeoGraph の任意 HTTP module を無効にしても SDK runtime の transport 依存は必要です。記録された SDK runtime/archive 検証は Linux/POSIX の範囲です。Windows NTFS と macOS の実装はありますが、新 platform の検証には runtime 証拠が必要です。WASM provider runtime の検証は確立していません。

SDK imported target は `include/SchemaProvider` include root を提供します。公開例は recipe 専用 helper なしで `<descriptor/descriptor.h>`、`<runtime/client.h>`、`<neograph/llm/schema_provider.h>` を直接使います。

設置 SDK package は最低 `0.1.1` で interface revision 4 header と shared-library generation 4 の一致が必要です。バージョン一致だけでは旧 interface/ABI binary を承認しません。

```cmake
find_package(SchemaProvider 0.1.1 CONFIG REQUIRED COMPONENTS runtime)
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
- [3. Tool インターフェース](#3-tool-interface)
  - [Tool](#tool)
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
};
```

| フィールド | 型 | 説明 |
|-------|------|-------------|
| `name` | `std::string` | ツール固有の名前 |
| `description` | `std::string` | ツールの目的を LLM に説明する表示用説明 |
| `parameters` | `json` | 受け付けるパラメーターを記述する JSON Schema オブジェクト |
### Owned Outcome

プロバイダー呼び出しは `sp::runtime::Result`、すなわち `sp::Completion` または `sp::Failure` を保持する不変の所有 `std::shared_ptr<const sp::Outcome>` を返します。表示テキストだけでなく結果全体を保持してください。順序付きメッセージ/パート、native continuation、存在する wire envelope、順序付き raw 観測、停止の根拠と実際の試行メタデータは呼び出しとクライアント破棄後も残ります。`input_total`、`output_total`、`total` などの使用量カウンターは `std::optional<sp::Count>` で、存在する count は `uint64_t value` と `Evidence` を持ちます。`Usage` は stage、quality、conflict も記録します。欠落は不明であり、ゼロを作りません。失敗も元の部分結果を保持します。`ProviderFailure::outcome()` と `ProviderObserverError::outcome()` は実際の結果を保持し、後者の `cause()` は観測者の例外を保持します。

`sp::Completion::wire_envelope` と `sp::PartialCompletion::wire_envelope` はプロバイダーファミリーごとに有無が異なり、空の場合があります。存在しなければ Python ビューは `None` を返します。現在のバッファ型 Chat はこのフィールドを空のままにし、完全な応答文書を `raw_events` の `sp::RawWire{type="chat.completion", payload=document}` に保持します（Python では `ProviderRawWire`）。有無と実際の型付き raw 証拠を確認してください。フォールバックでエンベロープを合成したり、部分的な失敗を成功に変えたりしません。プロバイダーの非公開フィールドを含む所有されたワイヤー証拠は、プロバイダー破棄後も保持された結果に残ります。ネイティブトレースは raw エンベロープ/イベントとネイティブ再生/推論を除外します。JSON 参照用のコピーはネイティブ権限や財務上の権限を与えません。

利用側は完全な型付きメッセージ/パート、論理的なロールとテキストを使い、続行に必要なら真正なネイティブ所有権を保持してください。チェックポイントや Chat リクエストのテキスト content は、テキスト文字列でも有効な型付きテキストパート配列でも表せます。偶然選ばれたどちらの直列化形式も、普遍的な契約ではありません。

真正な `NativeContext` の再生には、元の `request.messages` の完全な prefix を保持し、直接返された `outcome.messages` をネイティブ所有権を維持したまま順番に追加します。直接の結果には新しく返されたメッセージが含まれ、元のリクエスト履歴は含まれません。その assistant メッセージだけを再生すると、ワイヤー I/O 前に `ReplayIneligible` で拒否されます。`NativeArchive` から復元した assistant にも元の完全な prefix が必要です。アーカイブの保管権限は履歴の系譜チェックを代替しません。グラフの `RunResult.native_messages` はすでに全履歴を含むので、元の入力を再び先頭に付けないでください。

`UsageAccumulator::snapshot()` は累積報告を返します。`total_tokens_wide()` は計上済みトークンと未解決予約の合計で、報告使用量として表示してはいけません。精算には input/output count のある final・consistent 報告が必要で、根拠のある最大 total を計上し、超過使用量も clamp しません。累積対象の一つでも counter が欠落すれば集計も不明です。予約、ローカル計上、vendor 請求書は別の記録です。

### Portable projections


実結果の後に post-effect 精算や terminal receipt 永続化が失敗すると、`ProviderDispatchOutcomePersistenceError::outcome()` は元の不変結果、`cause()` は元の永続例外を保持します。delivery も失敗した場合は `delivery_error()` が元の観測者例外を保持します。永続化成功後の観測者失敗は元の例外を変更せず再送出し、不明/結果なし transport 失敗では outcome を捏造しません。
`ChatMessage` / `ChatTool` と JSON は portable projection であり native 権限ではありません。Portable 形式は [`provider-message-v2`](../schemas/provider-message-v2.schema.json)、[`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json) のままです。真正な C++ checkpoint sidecar はメモリ内の native seal を保持します。永続 native 履歴には host-owned `sp::NativeArchive` が必要です。closed v3 / `spna3` は独立キーによる認証済み owner-private custody で、archive v2 は更新・解釈せず拒否します。認証は全 semantic descriptor 選択（origin/path/header、policy、要求 field mapping、usage path、stop mapping）、owner と正確な custody binding を結び付けます。暗号化や vendor-issuer 認証ではありません。archive 本文・キー・native blob・raw wire 観測は公開しません。Archive は証拠保存であり、金銭 grant や spending lease ではありません。Program/external bank は独立 journal が所有し、snapshot コピーで credit は作れません。

`RuntimeHistoryRecord` の引数なしの `serialize_canonical()` はポータブルなレコードを扱います。永続ネイティブ履歴には `serialize_canonical(archive, owner_id)` と、その ID に所有者範囲が一致するアーカイブを使います。Python も同じオーバーロードと `RuntimeHistoryRecord.parse(stored_bytes, archive=None, owner_id="")` を公開します。ポータブルなレコード用の既定引数は、一致するアーカイブなしでのネイティブ復元を許可しません。Python のパースとアーカイブ対応の直列化はネイティブ処理中に GIL を解放します。JSON の観測データと、アーカイブで認証された保管権限は区別されます。

Python は `ContextStore.hydrate_records(range)` と `history_record_by_message_id(feed, message_id)` で型付き保管権限を保持します。後者はレコードまたは `None` を返します。`SQLiteContextStore(database_path, archive=None)` は実際のアーカイブを受け取り、`LocalProgramHost` は最後の任意引数 `native_history_archive=None` を `RuntimeConfig` に渡します。これらは権限を捏造したり、永続 Program ストアのバックエンドを追加したりしません。返された `RuntimeHistoryRecord.message` は真正な共有ネイティブ所有権を保持する独立した型付きコピーで、変更しても不変の RAW 識別子を書き換えられません。ストアの構築と型付き取得は GIL を解放し、保管権限が必要なネイティブ履歴はアーカイブなしのストアでは拒否されます。

`Assistant` メッセージを含む RAW `RuntimeHistoryRecord` には `RuntimeTrustClass.ModelOutput` が必要です。`RuntimeTrustClass.UntrustedInput` は `User` メッセージだけを受け入れます。これらは実際の Python enum 名です。trust class はレコードのロールと由来の境界を示し、選択してもネイティブ再生の保管権限や支出・実行権限を付与しません。

**Standalone bank journal 修正 — 現在の契約を改訂；実際の runtime 証拠は下記。** Owner-approved protocol は単調 trusted-store namespace obligation と、実際の不変 original owner/thread/graph scope、ceiling、deadline/clock identity、generation を要求します。全 checkpoint commitment/revision に対する正確な durable head CAS だけが host-owned opaque lease を発行できます。正確な pending effect window を provider I/O 前に永続化し、真正な SDK outcome と実際の charge、nullable report、hold、dedup identity で精算しなければなりません。Checkpoint/next head は同じ owned actor/revision 下で原子的に publish します。Bank metadata 削除、checkpoint pruning、old authenticated snapshot replay、同一 ID overwrite、actor 喪失で credit を与えてはなりません。既存 65 hold がある ceiling 130 を 129 に下げると別の 65 は許可できません。証明済み no-effect 失敗は unchanged head を release し authentic 130 復旧を可能にできます。Crash/unknown/lost-lease window は refund/retry/fallback なしで hold を保持します。Plain/pristine archive 設定は money/native spending lease を与えず、現在の `config.usage` は既存 standalone obligation を置換できません。Program/external-bank journal 所有は不変です。これは要求契約です。実際の currency/custody 証拠と instrumentation 制約は下記であり、安定 released API 保証ではありません。

**現在の宣言；統合 runtime 証拠は下記:** `<neograph/graph/checkpoint.h>` は `owner_scope`、logical `thread_id`、private backend `storage_thread_id`、`graph_identity`、`original_ceiling`、`original_deadline_ticks`、`deadline_clock_identity` を持つ `ManagedBudgetLeaseScope` を宣言します。`OwnedManagedBudgetLease` は read-only `scope()`、`actor_id()`、不変 `bank_generation()`、`revision()`、`head_checkpoint_id()`、`head_commitment()` を公開し、公開 authority-import constructor はありません。`ManagedBudgetEffectReceipt` は `active()`、`effect_id()`、`claim_amount()`、`request_digest()` を公開し、default receipt は権限を与えません。`CheckpointStore` は `acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`、`begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`、`settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`、`publish_managed_budget_checkpoint(lease, checkpoint)`、`release_managed_budget_lease(lease)` と `_async` counterpart を宣言します。Sync `CheckpointStoreCore` と `AsyncCheckpointStore` はそれぞれの variant を公開します。`managed_budget_checkpoint_commitment(checkpoint)` は bank JSON だけでなく全永続 checkpoint を結び付けます。これらの宣言は backend CAS、currency 安全性、installed ABI 互換性、実際に成功した runtime 経路を証明しません。

**真正な InMemory shared-bank fork は保持・実証済み。** 元の真正な C++ fork は ONE original financial journal と trusted current branch head を使い、grant を複製しません。`publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)`（および `_async`）は authentic current source/full commitment と実際の same-bank native C++ pointer を要求し、durable standalone fork は明示的に unsupported のままです。`OwnedManagedBudgetLease::scope()` と original owner/thread/graph、ceiling、deadline/clock、generation は不変です。Read-only store-issued `execution_thread_id()` / `execution_storage_thread_id()` は execution branch を別に選び、`GraphState::budget_original_thread_id()` は元の financial bank を示します。正確な selected-branch head CAS と global actor/revision は canonical current counter、pending effect、burned identity に対して全 branch を直列化します。Original/fork branch は補充なしで使用可能なままです。Stale snapshot、checkpoint copy、imported JSON は alias を発行したり head を巻き戻したりできません。元の root30 → charge3 → original continuation6 → fork lower20 → continuation9 の same-bank 証明は未変更 test_graph_engine.cpp:810–913 で PASSED です。Saved original ceiling30 は effective fork ceiling20 と別です。Widening31 と JSON-only restore は拒否必須です。Unbounded reported observation は factual data で finite grant ではありません。証明済み zero-effect lease だけが unchanged head を release でき、unknown/pending effect は obligation を保持します。

**現在の release-error 契約；実際の suite/probe は下記。** `<neograph/graph/engine.h>` の `graph::ManagedBudgetLeaseReleaseError` は `ProviderOutcomeError` を継承します。`cause()` は元の execution exception を保持し、`release_error()` は二次 durable lease-disposition 失敗を公開します。`outcome()` は真正な SDK 証拠があれば保持し、SDK outcome がなければ null です。Release 失敗は結果を捏造せず再 dispatch も許可しません。Closed `_neograph_managed_budget_scope` metadata は元の logical scope/cap/deadline clock/generation を記述しますが、backend CAS 権限ではなく data です。

**Archive-owner/retention 契約；実際の suite/probe は下記。** Finite standalone root または authenticated finite source だけが、実際に設定された `sp::NativeArchive::owner_scope()` から省略された original owner を継承します。Unbounded/plain owner metadata の意味は不変です。明示的に矛盾する archive owner は lease acquire 前に拒否します。`CheckpointStore::retains_native_checkpoint() const noexcept` と対応する Core/Async storage capability は既定 false で、実際の InMemory backend は true に override し、wrapper は実際の retention を委譲します。この read-only 記述は正当な unleased/plain/unbounded C++ native checkpoint custody を許しますが、spending credit や native replay authority は与えません。Leased custody は JSON flag や推測した store type ではなく実際の store-issued receipt を使います。

**Native-custody pre-I/O gate；実際の suite/probe は下記。** Managed effect begin は pending-effect/slot/held-window 変更前に、真正に結び付けた NativeArchive または実際の local store-issued private C++ retention capability を要求します。Private capability は JSON から import せず wire にも転送しません。C++ sidecar は境界を越えられないため、remote backend が InMemory でも gRPC は実際の client/server archive を要求します。Archive が finite source owner を提供しなければ元の anonymous owner scope は空のままで、実際の archive binding は original scope に一致しなければなりません。Financial head/lease 証拠だけでは native-custody readiness を証明しません。

診断 JSON は構文上有効な duplicate-key 文書も含め元の raw byte を保持しますが、実行可能な要求/config admission は重複を拒否します。元の non-2xx 応答 JSON は二度目の損失 parse なしで `http.error` 証拠に残ります。named SSE error は後続の正常 stream close より優先します。診断/provider metadata の上限は承認済み source extent であり、無関係な小さい error-text cap ではありません。

`ProviderRequest::observer_limits` は host-only です。明示した `max_events`・`max_bytes` は正数で、承認済み SDK 配信上限を下げることしかできません。`provider-request/v3` digest は実効 limit、mode、encoded body、retry policy と全 semantic descriptor binding を結び付けます。Bridge は queued/draining batch を通じて実際の PMR vector/map capacity と所有 event/document byte を計上し、queue mutex 外で cancellation を要求します。`messages` という名の Generic channel を chat に強制変換しません。native `history` channel を `messages` に mapping すると C++ sidecar が保持され、JSON から native 権限を作りません。

`ProviderOutcomeError` は結果を保持する共通 host-error base です。`ProviderObserverError` と `ProviderDispatchOutcomePersistenceError` は完全に drain した SDK 結果と元の `cause()` を保持し、後者は二次 observer 失敗も `delivery_error()` に保持します。`ProviderFailure::outcome()` は SDK 失敗自体を保持します。これは Node/Program の再 dispatch 権限ではありません。Provider retry の唯一の所有者は SDK で、caller が選んだ `max_output_tokens` を黙って clamp しません。

`ProgramFailure` は live `provider_outcome`・`provider_cause` を保持します。Canonical factual SDK witness は真正な archive custody を owner/run/version/bundle/operation/attempt に結び付け、Runtime は復旧失敗を公開する前に設定済み custody を eager に復元します。公開 data-only `ProgramResult::create()` は事前入力 witness で迂回できず、未解決の parsed seal は実行結果ではありません。プロセス再起動後は元の exception pointer がなく `provider_cause == nullptr` であり、text から再作成しません。永続化できない失敗は serialize/publish/replay できません。

Python の `LocalProgramHost` は破棄時に、`ProgramRuntime` がスケジューラーの処理をキャンセルし、残りの処理を完了させて join する間、呼び出し側の GIL を解放します。これにより実行中の Python ノードが終了できます。残りの host メンバーと、それらが所有する Python コールバック/オブジェクトを破棄する前に GIL を再取得します。変更は終了処理のみで、追加の capability binder や実行権限は公開しません。

`RecordedBindingSet` は source-bound の move-only data で、caller 提供 dispatcher ではありません。信頼された Catalog の `recorded_capability_binder` は実際の永続 source event を独立に読み、captured-only capability を materialize します。`ProgramRuntime::replay_recorded()` は元の selected-source permission を検証し、実際の残存 bank を durable CAS で移します。inherited spend は新しい model grant ではありません。旧 `start_recorded` 更新 API は削除されました。InMemory/File/SQLite/PostgreSQL Program store は実行全体で正確で不変の owned lease を保持し、expiry による更新をしません。Controlled JavaScript も underlying capability manifest を検証し、正確な completed command 結果を消費して external effect を再 dispatch しません。

**Recorded-control causal fix は full suite で実証済み。** Captured command replay は実行前に新しい CPU wall-time/Core work だけを durable に reserve し、測定済み work と新しく生成した Core checkpoint を result CAS で publish します。新しい model、money、Program-operation allowance を消費せず、captured external effect を再 dispatch しません。未精算 reservation は debit を保持します。Reservation により、最初の新しい Core checkpoint を拒否した通常の Running→Running transition ではなく認証済み settlement transition を選びます。Await channel receive、timer wait/cancel、handoff wait の開始/release は owning executor/strand 上で直列化します。既存 Recorded CPU/Memory await/handoff scenario は full suite で pass しました。Remote TSan coverage 制約は下記に明記します。

以下の観測はこの文書整備より前に記録されたものです。歴史的証拠であり、新 test 実行や全 platform・transport・security 性質の保証ではありません。

**有料観測は完了；普遍的な qualification ではありません。** 元の `SPQUAL1` base630/1000000 microUSD は不変です。同じ元 ledger の ONE hash-chained `A` が承認済み extension480/3000000 を受け入れ、aggregate1110/4000000 になります。Calls/spent/hold/settlement は累積で新 grant ID/header/reset はありません。正確な declaration byte/file identity と original authorization/baseline/catalog/activation/ledger-prefix の hash/totals は固定され、削除・置換・変更は fail closed です。最終 canonical ledger は calls1110/spent437958/held1287828 microUSD、eventA1、limits1110/4000000；spent+held US$1.725786 は LOCAL catalogue meter で invoice ではありません。記録済み five-family60-pair baseline は600 request 完了：Chat60/60、Responses60/60、Messages60/60、Generate56/60（incorrect-vision SSE4件）、Interactions57/60（incorrect-vision buffered1件/SSE2件）；合計293/300 pair で300/300ではありません。他の old600 financial record は保持しますが完全な behavioral proof ではありません。以前の M5/media one-shot cohort は不変です。以前の Google3-round prerequisite は invalid-tool2件/unreadable-positive1件の失敗状態を保持します。追加有料呼出しは承認されません。最終 SDK 証拠と native-axis 制約は baseline 成功とは別です。 以前の activation/reopen smoke は2回 reopen 後 calls610/spent219159/held751233、SDK meter/canary/vision4-test19.38秒 pass として保持します。これは限定された以前の checkpoint で最終 ledger totals ではありません。以前の検証済み Chat60-pair cohort は実際の attempt120、UpperBound charge120、UnknownHold なしを保持します。

**Native-axis 観測は cryptographic 検証・native consumption/equivalence ではありません。** Generate は mutation/omission/duplication を受け入れました。Interactions は isolated genuine source/positive control、one-owner signature mutation、thought-carrier omission、call-carrier omission、duplication を受け入れました。全 thought/signature 削除は generic400、THOUGHT item を保持して全 signature field を削除した場合も generic400 でした。最後の capture は local encoded-original retention control で、same-capture server positive ではありません。以前の positive cohort は真正です。観測は aggregate-carrier-absence boundary のみを示し、issuer/signature 検証や vendor consumption を証明しません。実際の report：SDK `config/qualification-extension-results.json`、`qualification-final-summary.json`、`qualification-native-axis-results.json`、`qualification-combined-omission-results.json`、`qualification-signature-presence-results.json`。Prerequisite-failed/not-run/negative-inconclusive の状態は事実のままです。 Thought-only/carrier-only omission は別 carrier が残る状態で受け入れられました。Issuer-validation/native-consumption の主張を強めません。

**実際の統合証明と残る制約。** 最新 Core full run は2242 test、失敗0、skip16（RAM process-loss 非適用14件/live-credential gate2件）、130.17秒です。`PgNestedJsonRoundTrips` は duplicate key/order/null metadata、blob、residual を正確に保持し0.18秒で pass。未変更の元 shared-bank fork と既存 Recorded CPU/Memory await/handoff scenario も pass。実際の wrappedMemory/SQLite/PostgreSQL/gRPC finite130/hold65/lower129/strip/old-head/pruning/no-archive/import probe は plain と ASan+UBSan で pass。LOCAL Memory/SQLite/PostgreSQL TSan scope は7件 pass、warning0。System Abseil/Protobuf を含む full mixed gRPC TSan は exit66、dependency/generated-RPC stack に race warning402件。これは instrumentation/coverage 制約で proven false positive ではありません。Remote TSan/race-free は主張せず warning を suppress しません。Installed find_package Program C++/C ABI/dualQuickJS の3 consumer は pass。Fresh installed NeoGraph/SchemaProvider typed consumer は実際の HTTP request2件、coroutine 開始前の provider 破棄、native/tool replay、refusal、known-zero/raw 保持、実際の LinkedMismatch 拒否で pass。Browser Alice/Bob isolation と generation2 replacement を目視検証し、PostgreSQL Program Chat black-box6件は18.989秒で pass。最新 SDK26/26 は失敗0、74.07秒で pass。最終 ReleaseGraph16設定 ×fresh process3回/48記録は38.29秒、失敗0、全 actual protocol/owned-outcome check pass で完了しました。NeoGraph `benchmarks/provider-cutover-final-results.json` と `benchmarks/provider-cutover-final-summary.json` は独立した最終 cohort を保持します。測定中 compiler/有料 model は実行せず、歴史 cohort は不変で semantic/resource equivalence は主張しません。Unstable SDK/ABI3 は安定 release や広い platform qualification ではありません。

**最小有料証拠（2026-10-03）は広範な qualification ではありません。** 個別承認の one-shot 3 呼出しの結果：Images—JPEG 1 個、1024×1024、360685 byte、input/output/total token 19/1408/1427、実際に目視確認；Veo—MP4 1 個、1280×720、4 秒、437737 byte、generation 1 回と status GET 3 回、usage nullable、Chromium で decode・目視確認；Decisions—`typesafe/jev-1.13`、probability 0.93、input/output token 283/21、total 不明、API 報告費用 USD 0.000011886。Image USD 0.0336 base + text/thinking、Veo USD 0.20 は catalog 予測で invoice ではなく、最小 image smoke は価格帯内訳を取得していません。結果は one-shot 権限を更新せず再実行も許可しません。

完了した chat pair は downstream vendor の native-continuation 消費を証明しません。

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

`make_provider_request(provider, model, messages, tools, controls, mode)` は閉じた family を一つ選びます。別 family の制御は `std::invalid_argument`、SDK prepare は enum 値、承認 origin、モデル規則、tool 宣言、native binding を I/O 前に検査します。`ProviderControls` に dictionary の迂回路はありません。optional は未指定と明示的な `false`・空の選択を区別します。

| Family | `ProviderControls` フィールドと SDK 対応 |
|---|---|
| `openai.chat` | `max_output_tokens`, `temperature`, `top_p`, `reasoning_effort`, `service_tier`, `provider`, `response_format`; `chat_reasoning` → `sp::chat::Request::reasoning`, `include_reasoning`, `usage_include` → `usage.include`, `models` |
| `openai.responses` | `max_output_tokens`, `max_tool_calls`, `temperature`, `top_p`, `reasoning_effort`/`reasoning_summary` → `reasoning`, `service_tier`, `required_tool`, `provider`, `response_format`, `store`, `system` → `instructions`, `account_scope`; `previous_response_id`, `previous_response_history`, `parallel_tool_calls`, `verbosity` → `text.verbosity`, `truncation`, `responses_include` → `include` |
| `anthropic.messages` | `max_output_tokens` → `max_tokens`, `temperature`, `top_p`, `thinking_budget`, `system`, `account_scope`, `provider`; `thinking_mode`, `output_effort` → `output_config.effort`, `cache_control`, `messages_tool_choice` → `tool_choice` |
| `google.generate` | `max_output_tokens`, `temperature`, `thinking_budget`, `include_thoughts`, `required_tool`, `system`, `account_scope`; `gemini_history_mode` → `history_mode`, `gemini_thinking_level` → `thinking_level`, `safety_settings`, `gemini_tool_choice` → `tool_choice` |
| `google.interactions` | `max_output_tokens`, `thinking_level` (optional string), `thinking_summaries`, `service_tier`, `required_tool`, `system`, `account_scope`; Generate の enum `gemini_thinking_level` は適用されません |

Chat の `sp::chat::ReasoningOptions` は optional `effort`, `max_tokens`, `exclude`, `enabled` を持ちます。この nested reasoning object、`include_reasoning`、`usage_include`、代替 `models` は policy 宣言済み OpenRouter origin に限ります。`sp::OpenRouterRouting` も Chat/Responses/Messages の宣言済み OpenRouter origin のみです。gateway 形式のモデル名では別 origin を承認できません。SDK payload は family ごとの typed tool 宣言、Responses `hosted_tools`、strict/deferred tool option も持ちます。raw JSON でなく実際の payload variant を使います。

| SDK 型 | 閉じた値またはメンバー |
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
| `sp::gemini::ToolChoice` / `ToolChoiceMode` | `mode`: `Auto`, `Any`, `None`, `Validated`; `allowed_function_names`: 宣言済み関数名の vector |

Messages は正の output cap が必要です。mode 未指定の thinking budget は `Manual` を選び、承認 minimum 以上かつ cap 未満にします。`Adaptive`/`Disabled` は明示 budget を拒否します。Manual/adaptive はそれ以外で有効な temperature を省略し、thinking `top_p` minimum を検査しますが、モデル別 temperature 禁止は上書きしません。強制 `Any`/`Tool` は tools と disabled thinking が必要です。`Tool` は宣言済み client tool 名、他 mode は `name` を拒否、`None` は `disable_parallel_tool_use` を拒否します。Generate の thinking budget/level と `required_tool`/`gemini_tool_choice` はそれぞれ排他的です。allowed-function リストは宣言済み関数を指します。sampling は family/policy の範囲内です。Generate は `top_p` 非対応、Interactions は両 sampling field 非対応です。

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

任意の `ProviderControls` は呼び出し元の選択であり、強制デフォルトや黙った cap clamp ではありません。非対応 family 制御は dispatch 前に拒否します。有界呼び出しには承認された実際のモデル input/output 上限が必要で、欠落は `LimitUnknown` です。予約は保守的な支出権限であり、報告使用量・予測・請求書ではありません。不明/部分/delivery-unknown の結果は hold を維持し、実際の最終報告で精算し、超過報告も全量を計上します。retry は明示的な単一層で、既定 off、有界 window と unknown-prior hold を使います。隠れた再送はありません。

`provider_failure_proves_not_sent(Failure)` は完全で矛盾のない NotSent 根拠を要求します。status code・使用量欠落・観測者/永続例外だけでは費用ゼロや予算更新を証明しません。

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


ソースとバイナリの破壊的変更です。全 C++ 利用者とカスタムプロバイダーを一致した新ヘッダー/ライブラリで再コンパイルします。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、descriptor interpreter、Responses WebSocket は alias/互換 bridge なしで削除されました。SDK は alpha `0.1.1`、interface revision 4 / shared-library generation 4、out-of-line capability check を使い、安定リリースの宣言ではありません。記録済み interface-3 SDK runtime/archive 検証は Linux/POSIX の過去の根拠で、interface-4 pass ではありません。Windows NTFS/macOS 実装は存在しますが新 platform 検証には runtime 根拠が必要です。WASM provider runtime は未検証です。

Python も C++ と同じ所有 request/outcome 境界を公開します: `make_provider_request`、`Provider.prepare`、`dispatch`、`invoke`。Provider 履歴には typed part を持つ `ProviderMessage` を使い、`ChatMessage` はグラフ用の便宜的 projection として残ります。SDK 失敗は `ProviderOutcome.failure` で読み、host observer/settlement 例外は `outcome` と `cause` を保持します。コンストラクターと GIL/コールバック動作は [Python binding ガイド](python-binding.md)を参照してください。

Python SDK の vector/map getter は独立した値を返します: `ProviderMessage.parts`、`ProviderRequest.messages`、`RunConfig.provider_messages`、completion/partial の `messages` と `raw_events`、`RunResult.native_messages`、`ProviderLoopEntry.messages`、usage の `extra`/`conflicts`。スナップショットを変更し、setter があるプロパティには再代入してください。getter の結果への append は所有オブジェクトを更新しません。任意の `ProviderControls.provider`/`response_format`、`SchemaProviderDefaults.provider`、`ProviderToolResult.host` も同じ読み取り・変更・再代入の規則に従います。読み取り専用の outcome 証拠は変わりません。これは Python バインディングの規則であり、すべての C++ getter がコピーを返すという保証ではありません。

ネイティブコードが Python provider の `prepare` オーバーライドを呼ぶ際に `None` を返すと、handle を消費する前に `TypeError` が発生します。`ProviderDescriptorPolicy.identity` は raw SHA-256 digest を格納する `bytes` で、`.hex()` は表示用の変換です。保存された Python provider/graph の cause を繰り返し調べても、元の例外値と traceback を保持し、保存された例外の restore 状態を消費しません。ネイティブの入れ子になった例外変換にも適用されます。

以前記録した installed find_package Program C++/C ABI/dualQuickJS consumer と NeoGraph/SchemaProvider typed2-request lifetime/native/raw/mismatch consumer は当時の snapshot で pass しました。これらは interface-4 package 検証を証明しません。宣言一致だけでは新 runtime 結果や広い platform 対応を証明できません。

現 SDK4 Linux x86_64 根拠は登録済み 27 case をすべて対象とします。最初の full run で 25 が pass、obsolete assertion 二つを修正後、`native_archive` と `stop_reasoning_preservation` は focused 2/2 で pass しました。二度目の full-suite 27/27 run ではありません。未変更の buffered/SSE Stop probe と設置 SDK の exact README consumer も pass しました。後者は zero-usage、unknown-usage、HTTP-400-failure variant ごとに credential なしで一要求を実行しました。この SDK 結果は新 NeoGraph native build/wheel pass や Windows/macOS/ARM64/HTTP3 検証を証明しません。

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
    uint64_t    version = 0;                       // Write counter
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
| `NODE_END` | ノードが生成したチャネル書き込み |
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
| `channel_version(channel)` | 指定チャネルの書き込みカウンターを返します |
| `global_version()` | グローバルバージョンカウンターを返します (任意のチャネルへの書き込みごとに増加) |
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
| `ctx` | `NodeContext` | 分類用 LLM 呼び出しに使う Provider と model |
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
### RunContext (v0.4 PR 1、`NodeInput.ctx` 経由でノードに公開)
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
| `fork()` | **v0.4 PR 3。** 子の shared_ptr を返します。親の cancel() は連鎖し、fork() 時点ですでに親がキャンセル済みなら競合なしで子もキャンセル済みになります |
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
    RunStatus status() const noexcept;            // Completed, Interrupted, StepLimit, or SafePoint

    template <typename T> T channel(const std::string& name) const;
    template <typename T> T channel(const ChannelKey<T>& key) const;
    template <typename T>
    std::optional<T> try_channel(const ChannelKey<T>& key) const;
};
```

`RunResult::usage` は nullable provider 報告であり支出 bank ではありません。`native_messages` は真正 typed 履歴、`provider_outcomes` は各所有 Completion/Failure を保持します。JSON `output` は portable projection です。全履歴入力には `RunConfig::provider_messages`、typed event 観測には `on_provider_event` を使います。永続 native checkpoint/receipt custody には `native_history_archive` が必要ですがメモリ内 sidecar には不要です。

resume や continuation では、`provider_outcomes` は元の結果に新しく生成した結果を続けた順序付きリストを保持します。`usage` にも復元した bank の以前の報告が残ることがあります。利用側は完了済み provider effect の再 dispatch と二重計上を防ぐ不変条件を保ってください。空またはリセットされた結果リストや `usage=None` は resume の不変条件ではありません。

`provider_messages` は全 typed 履歴を渡し、messages チャネルのみを置き換えます。`on_provider_event` は typed SDK event を観測します。`provider_outcomes`、`provider_loop_history` は結果と task-local continuation を inner turn 間で保持します。`native_history_archive` は永続 native custody を結び付け、モデル予算は与えません。任意の `model_token_budget` 上限と `budget_exhausted` 信号は予算対応 dispatch 用です。Python と C++ は共に `RunResult.native_messages` で全履歴、`provider_outcomes` で所有結果を返します。入力は引き続き `RunConfig.provider_messages` で、`RunResult.provider_messages` や `provider_history` alias はありません。
| フィールド | 型 | 説明 |
|-------|------|-------------|
| `output` | `json` | 全チャネルの最終シリアライズ状態 |
| `interrupted` | `bool` | 中断 (HITL) により実行が一時停止した場合は `true` |
| `interrupt_node` | `std::string` | 中断を発生させたノードの名前 |
| `interrupt_value` | `json` | 中断に関連する理由またはペイロード |
| `checkpoint_id` | `std::string` | 最後に保存したチェックポイントの UUID |
| `execution_trace` | `std::vector<std::string>` | 実行順に並んだノード名の一覧 |
`max_steps_exhausted()` は、実行可能な作業が残っている状態でステップ上限に達して実行が停止した場合にのみ `true` を返します。
許可された最後のステップで `__end__` に到達したグラフは
`false` を返します。
`status()` は `RunStatus::Completed`、`RunStatus::Interrupted`、
`RunStatus::StepLimit`、または `RunStatus::SafePoint` のいずれかを返します。公開 `RunResult` のデータレイアウトは変わりません。
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

スレッドのチェックポイント履歴をタイムスタンプ順 (新しいものから) で返します。
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
- `run_sends_async`: 単一 Send は共有状態で再試行付きで実行します。
  複数 Send では対象ごとに分離状態コピー (初期化 + 復元 + 入力適用) を作り、再試行なしで実行します。
  これは 3.0 より前の意味論を保ちます。
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

| フィールド | 型 | 説明 |
|-------|------|-------------|
| `id` | `std::string` | 一意の識別子 (UUID v4) |
| `thread_id` | `std::string` | 会話/セッションごとにチェックポイントをまとめます |
| `channel_values` | `json` | 全チャネルのシリアライズ済み状態 |
| `channel_versions` | `json` | 各チャネルのバージョンカウンター |
| `parent_id` | `std::string` | 直前のチェックポイント ID (タイムトラベル用の連結リストを形成します) |
| `current_node` | `std::string` | チェックポイント取得時に実行中だったノード |
| `next_nodes` | `std::vector<std::string>` | 次のスーパーステップに予定されている全ノード (`resume()` が使用)。signal dispatch では、1 つのスーパーステップ後に複数ノードが同時に ready になることがあります (並列 fan-out や条件分岐の同時活性化)。その全てを永続化する必要があり、単一ノードだけを保存するとクラッシュをまたいで兄弟ノードが黙って失われます |
| `interrupt_phase` | `CheckpointPhase` | 列挙値: `Before` (`interrupt_before` が発火)、`After` (`interrupt_after` が発火)、`Completed` (通常のスーパーステップ周期)、`NodeInterrupt` (実行中にノードが `NodeInterrupt` を投げた)、`Updated` (外部 `update_state()` 注入)。`to_string()` と `parse_checkpoint_phase()` が安定した wire/log エンコードを提供します |
| `barrier_state` | `map<string, set<string>>` | これまで signal した上流を barrier ごとに蓄積したもの。エントリは処理中 (まだ発火していない) barrier にだけ存在し、Scheduler は barrier 発火時にそのエントリを消します。形状は `scheduler.h` の `BarrierState` と一致します。schema v2 以降に存在します。v1 blob は空 map としてデシリアライズされ、v2 以前の動作と一致します |
| `metadata` | `json` | 任意のユーザー定義データ |
| `step` | `int64_t` | スーパーステップカウンター |
| `timestamp` | `int64_t` | Unix epoch ミリ秒での作成時刻 |
| `schema_version` | `std::uint32_t` | wire 上のレイアウトバージョン (`CHECKPOINT_SCHEMA_VERSION`、現在は `3` を参照)。Round 5 で `int` から固定幅の unsigned へ広げました。schema version は非負であり、ディスクへ永続化して JSON 経由で往復する値に、プラットフォームで幅が変わる `int` を使うのは不適切だったためです。永続化する `CheckpointStore` 実装はこれをシリアライズし、デシリアライズした blob の `0` は「バージョン付け前」(例: フィールドが存在しなかった。移行は呼び出し側の責任) と扱うべきです |
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
Model Context Protocol (MCP) クライアント実装です。MCP サーバーに接続し、利用可能なツールを検出して、
利用できるトランスポートは 2 つです:
- **HTTP** — `MCPClient("http://host:port")`。検出されたツールは、元の Streamable HTTP セッションを保持します。
- **stdio** — `MCPClient({"python", "server.py"})`。クライアントは `fork` の前に `PATH` を解決し、子プロセスで `execve` して双方向パイプを接続します。
  子プロセスの stdin/stdout 上で通信します。サブプロセスは、
  生成元の `MCPClient` またはいずれかの `MCPTool` が生存する間だけ存在します。
  破棄時には SIGTERM を送り、`waitpid` で回収します (約 500 ms 後に SIGKILL へフォールバック)。
### MCPTool
単一の MCP サーバーツールをローカル `Tool` 実装としてラップします。検出されたツールは
トランスポートによらず、元のプロトコルセッションを保持します。
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

通常、`MCPTool` を直接構築することはありません。`MCPClient::get_tools()` が検出してラップします。
### MCPClient
MCP サーバーに接続し、初期化ハンドシェイクを行い、ツールの検出と呼び出しのメソッドを提供するクライアントです。
> `MCPClient` はサブクラス化を想定していません。そのまま使用してください。
> `rpc_call_async()` が実装本体で、`rpc_call()` は薄い同期ファサードです。
> [`ASYNC_GUIDE.md` §9.5](ASYNC_GUIDE.md#95-mcpclient) を参照してください。
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

**ワイヤープロトコル:** NeoGraph の MCP クライアントは
`protocolVersion = "2025-11-25"` を使用します。HTTP トランスポートは
すべての JSON-RPC リクエストに `MCP-Protocol-Version` ヘッダーを送り (Round 1 +
Round 3 の仕様に整合)、stdio トランスポートは同じバージョンを `initialize` ペイロードに持たせます。
古いプロトコルバージョンで動作するサーバーはリクエストを拒否する可能性があるため、サーバー側を固定するかアップグレードしてください。
| メソッド | 説明 |
|--------|-------------|
| `MCPClient(url)` | HTTP モードのクライアントを構築 |
| `MCPClient(argv)` | サブプロセスを起動して stdio モードのクライアントを構築。`argv[0]` は fork 前に `PATH` で解決し、exec 失敗は最初の RPC で接続エラーになります。安全対策のため Windows の `.bat` / `.cmd` は拒否 |
| `initialize(client_name)` | MCP 初期化ハンドシェイクを 1 回実行。再呼び出しは冪等で、プロトコル/トランスポート失敗時は例外 |
| `get_initialize_result()` | ネゴシエートしたプロトコル、機能、サーバー情報、指示、未加工の結果を返す |
| `list_tools(cursor)` | カーソルを不透明な値として扱い、1 ページを取得 |
| `get_tool_definitions()` | 全ページをたどり、ツールメタデータ全体を保持 |
| `get_tools()` | 全ページを検出し、セッションを保持する `MCPTool` インスタンスを返す |
| `call_tool(name, arguments)` | 指定された引数で名前付きツールを呼び出し、未加工の JSON 応答を返す |
| `call_tool_result(name, arguments)` | content、structured content、`isError`、`_meta` を保持する型付き結果 |
| `rpc_call_async(method, params)` | コルーチン版。実装本体で、`rpc_call` は薄い同期ラッパー |
**HTTP の使用方法:**
```cpp
neograph::mcp::MCPClient client("http://localhost:8000");
client.initialize();
auto tools = client.get_tools();
```

**stdio の使用方法:**
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
`A2AClient` と `GraphAgentAdapter` は `WireDialect::{V0_3,V1_0}` の JSON-RPC 2.0 HTTP/SSE を実装します。`AgentCard.supported_interfaces` は新旧 card から解析した順序付き `AgentInterface{url, protocol_binding, protocol_version, tenant}` 観測で、raw card も保持します。遅延選択は compatible JSONRPC 0.x/1.x の最初の interface、設定 base URL と末尾 slash 正規化後に一致するものを優先します。card URL は RPC endpoint を redirect しません。tenant は send/get/cancel/stream に適用します。`wire_dialect()` は選択・成功 probe まで空、force discovery が初期化します。取得済み incompatible card は別 dialect を probe せず拒否します。

card なしでは 0.3 を probe し、数値 JSON-RPC `-32601` の場合だけ切替えて成功 dialect を記憶します。method/body/header は同時に変わります。V1 は PascalCase、`A2A-Version: 1.0`、`ROLE_*`/`TASK_STATE_*`、`kind` のない flat text/raw/url/data part、`blocking` を反転した `returnImmediately` です。task/message wrapper と bare get/cancel task を decode します。server の応答 encoding は method spelling でなく version header に従い、header なしは 0.3、非対応 major は `-32009`、既定 card は両方を広告します。bound discovery URL は restart を保持し explicit endpoint を置換しません。

SSE は LF/CRLF/CR、comment、multiline data、最後の unterminated data を処理します。opening task、status、artifact append/replace を蓄積して Task を返します。V1 は opening submitted task と artifact の後に terminal status を送り、legacy `kind`/`final`/trailing task は不要です。event 観測後は external callback がなくても dialect redispatch を禁止します。non-SSE RPC error は `A2ARpcError::code()` を保持し、non-2xx HTTP は成功 task になりません。

caller node の回答優先順位は final/interrupted agent status text、最初の artifact text、最後の agent history text です。progress status や user history は回答を上書きしません。汎用 `async_post_stream` は nonempty fixed-`Content-Length` body を status とともに一度渡し、zero length は chunk を出しません。body limit/early EOF/既に buffer された surplus は拒否し、redirect/chunked/close-delimited 規則は維持します。

Python `neograph_engine.a2a` は `WireDialect`, `AgentInterface`, `AgentCard.supported_interfaces`/`raw`, `Part.media_type`, `MessageSendConfiguration`, `MessageSendParams`, `StreamEvent` と status/artifact record を公開します。`A2AClient.wire_dialect()` は enum または `None`、`set_authorization_header()` は native setter、`send_message(params)` は multipart overload です。`send_message_stream(text, on_event, task_id="", context_id="")` または `(params, on_event)` は蓄積 `Task` を返し bool callback に owned event snapshot を渡します。blocking call は GIL を解放し callback owner は安全に再取得します。`a2a.A2ARpcError.code` は remote 整数 code です。vector/optional child は detached snapshot、`Task.status` と `MessageSendParams.message` は live inline field です。JSON 観測は provider native 権限を付与しません。

**公開ヘッダー:** [`include/neograph/a2a/`](../include/neograph/a2a/)。
### `neograph::acp` — Agent Client Protocol
**ヘッダー:** `<neograph/acp/{server,types}.h>`
stdio 上の改行区切り JSON によるエディター↔エージェント JSON-RPC です (Zed、
Gemini CLI、Neovim CodeCompanion)。双方向で、client→agent
(`initialize`、`session/{new,prompt,cancel}`) と agent→client
(`fs/{read,write}_text_file`、`session/request_permission`) を遅延バインドされた `ACPClient` 経由で扱います。
`ACPServer::handle_message` はワーカースレッド上でプロンプトを非同期ディスパッチし、
`max_inflight_prompts=32`、セッションごとの single-flight、`-32000` のバックプレッシャーで上限を設けます。

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
- 任意の `ProviderControls` は呼び出し元の選択であり、強制デフォルトや黙った cap clamp ではありません。非対応 family 制御は dispatch 前に拒否します。有界呼び出しには承認された実際のモデル input/output 上限が必要で、欠落は `LimitUnknown` です。予約は保守的な支出権限であり、報告使用量・予測・請求書ではありません。不明/部分/delivery-unknown の結果は hold を維持し、実際の最終報告で精算し、超過報告も全量を計上します。retry は明示的な単一層で、既定 off、有界 window と unknown-prior hold を使います。隠れた再送はありません。
- **`neograph::AsyncTool`** — コルーチン向き (HTTP fetch、MCP 呼び出し) の仕事に対して
  `execute_async(json)` を公開する `Tool` の対となる実装です。同期 `execute()` は
  `run_sync` を経由して `final` ルーティングされます。
- **`neograph::graph::NodeCache`** — ノード単位のメモ化です。
  構築時に `EngineConfig::cached_nodes` でオプトインします (setter は互換性のため残ります)。
- **`neograph::graph::create_deep_research_graph`** —
  `examples/25_deep_research.cpp` で使われます。Round 2 の監査で
  `BriefNode` の LLM 書き換え、`FinalReportNode` のトークン上限再試行、
  `ClarifyNode` HITL gate で触れています。
このツアーに必要な型がない場合は `include/neograph/` を直接確認してください。
すべての公開ヘッダーにリファレンス文書があります。
