<!-- neograph-i18n: source=docs/HARNESS_MCP.md locale=ja source_sha256=ca4698088011b7213d7e665a73d15a483aae48056f7a18dd2fc504a92f589ed6 -->
# NeoGraph Harness MCP

**Languages:** [English](HARNESS_MCP.md) | [한국어](HARNESS_MCP.ko.md) | [日本語](HARNESS_MCP.ja.md) | [简体中文](HARNESS_MCP.zh-CN.md)

NeoGraph Harnessは、実行前に境界付きマルチワーカーワークフローをコンパイルする。安定したMCPサーフェスは6つのツールに留まる:

- `neograph_schema`はインストール済みのリクエスト契約とプリセットを検出する。
- `neograph_compile`は実行せずにコンパイルおよび検証する。
- `neograph_start`は保持されたアーティファクトまたはインラインリクエストを開始する。
- `neograph_get`はコンパクトなステータスをポーリングするか、結果アーティファクトURIを参照解決する。
- `neograph_resume` は、保留中のホスト結果を正確に検証し送信する。
- `neograph_cancel`は、キュー済み、実行中、または待機中のワークフローを協調的にキャンセルする。

同梱されるプリセットは `fanout_judge`、`pr_review_panel`、`bug_triage`、`research_synthesis` です。プリセットは通常の strict-Core グラフ成果物を生成します。JavaScript リクエストは、独自の `ProgramSource` エンベロープとソースマップを保持します。

### JavaScript オーサリング境界

`harness.mode` は、新規発行に `preset` または `javascript` を受け入れます。JavaScript リクエストはソーステキストを `harness.source` に保持し、`harness.source_id` を固定できます:

```json
{
  "harness": {
    "mode": "javascript",
    "source_id": "review:main.js",
    "source": "export function define() { const g = ng.graph('main'); /* ... */ return g; }"
  }
}
```

翻訳者は、そのテキストを正規の `ProgramSource` JavaScript エンベロープ (言語 `javascript`、QuickJS エンジン、凍結されたホスト API、インポート、ソースマップ) でラップし、`ProgramCompiler`、`ProgramCatalog`、`ProgramRuntime` を通して送信します。`define()` は、封止された `ng` バインディングを通じて 1 つのグラフを構築します。オプションのジェネレータ `main()` は通常の制御フローを所有し、既存の型付き Program コマンドを生成します。JavaScript は Core ノードをディスパッチせず、プロバイダー/ツールを選択せず、admission、予算、ジャーナル、リプレイを迂回しません。

評価されたモジュールは、結果コントラクトも選択します。`define()` のみをエクスポートするソースは、`channels.final_result.value` ラッパーを含む Core ルートコントラクトを保持します。代わりにランタイムの `main(input)` をエクスポートするソースは、その終端戻り値を Harness 結果スキーマに対して直接宣伝し検証します。

#### 制御フロー移行の例

`define()` のコンパイル時とすべてのランタイム効果を、生成された型付きコマンドの背後に保持してください。この完全なリクエストは、ジェネレータに 3 つの操作—`ng.all` 結合とその 2 つの Core 呼び出し—と双方向並列性を与えます。そのワーカーノードは、リクエストから封止された構成を正確に繰り返し、その終端戻り値は Harness 結果形状を持ちます:

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

安定したソースサイト文字列は、永続的なコマンド座標の一部です。再試行や再起動をまたいで決定的に保ってください。最初に成功した結果を優先すべき場合には`ng.any(...)`を使い、最初のターミナルメンバーが勝つべき場合には`ng.race(...)`を使うこと。どちらも構造化された並行性を通じて未処理の兄弟をキャンセルします。周囲のI/O、タイマー、動的読み込み、`eval`、ネイティブハンドルは利用できないままです。

: `harness.mode`は明示的でなければならない。`dsl`は`H_MIGRATION_CORE_DSL`を返し、`core`は`H_MIGRATION_CORE_JSON`を返し、`program`/`program_json`は`H_MIGRATION_PROGRAM_JSON`を返す。これらはすべて`/harness/mode`を指し、リクエストのJSON形状や欠落フィールドから選択されることはない。厳密なCore JSONは、検証済みのCoreおよびProgramアーティファクトに対する内部/交換表現であり続け、信頼されたC++プロセス内構築は引き続きサポートされる。どちらも公開のHarness作成言語ではない。

スキーマのエクスポート、コンパイル、および開始は現在、同じ不変の`HarnessAdmissionProfile`を消費します。そのスコープされた`GraphRegistry`とマニフェストは、実装、下位化、互換性のメタデータとともに、利用可能なすべてのノード、レデューサー、条件を列挙します。プロセスグローバルレジストリエントリはこのパレットの一部ではなく、Harness admissionによって解決されません。コンパイルは検証済みの宣言型`TopologySpec`で停止するため、拒否された入力構造は`GraphNode`を構築せず、ワーカーやエフェクトをディスパッチしません。保持されたアーティファクトはプロファイルIDとフィンガープリントをバインドします。異なるまたは事前プロファイルのアーティファクトは、再解釈されるのではなく、開始/再開時にフェイルクローズします。

構造時に`HarnessServiceResources`を通じて、C++エンベッダーは非デフォルトのプロファイルを渡します。この追加的なリソース境界により、既存の`HarnessServiceConfig`レイアウトが維持されます。プロファイルフィンガープリントはマニフェストと、スコープされたレジストリのエクスポートされた意味論的プロジェクションを示します。各`implementation_identity`は信頼された宣言であり、対応する呼び出し可能な動作が変更されるたびに変更されなければなりません。

これは現行のProgram-Backed Harness互換性アダプタです。受け入れられたHarnessリクエストは従来の`ProgramSource`へ変換され、`ProgramCompiler`を通じてコンパイルされ、`ProgramCatalog`を通じてadmissionされ、`ProgramRuntime`を通じて実行されます。`GraphEngine`は唯一のノード実行器のままです。

汎用オーサリングの許容される代替手段は、組み込みQuickJS上の標準JavaScriptです。以前の `dsl`、スタンドアロン `core`、および `program` モードは、明示的な移行診断とともに新規公開では拒否されます。厳密なCore JSONは内部/インターチェンジデータに留まります。[`QUICKJS_CONTROL_ARCHITECTURE.md`](QUICKJS_CONTROL_ARCHITECTURE.md) および [`QUICKJS_CONTROL_MIGRATION.md`](QUICKJS_CONTROL_MIGRATION.md) を参照してください。本ドキュメントでは、保持されている互換性の動作と移行診断について説明します。新しいレガシーソース内容は認可しません。

## ビルドと実行

OpenAI互換プロバイダアダプタを用いてローカルstdioサーバを構築する：

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

`NEOGRAPH_HARNESS_API_KEY` は `OPENAI_API_KEY` よりも優先されます。`NEOGRAPH_HARNESS_BASE_URL` は任意のOpenAI互換エンドポイントを選択します。サーバーは `https://openrouter.ai/api` のようなバージョンなしのベースと、プロバイダが文書化した `https://openrouter.ai/api/v1`のようなバージョン付き形式の両方を受け入れます。 `/v1` が欠落している場合にのみ追加されます。サーバーはプロトコルメッセージをstdoutにのみ書き込み、診断情報をstderrにのみ書き込みます。[OpenRouterクイックスタート](https://openrouter.ai/docs/quickstart)を参照して、現在のエンドポイント形式を確認してください。

ホスト相互運用性スモークテスト専用に、`NEOGRAPH_HARNESS_SMOKE=1`を設定してください。その明示モードは、有効なゼロ検出レビューを返す決定的なインプロセスプロバイダーを使用し、APIキーを必要とせず、LLM品質テストとして使用してはなりません。

永続的なホスト仲介呼び出しには、レコードとチェックポイントの両方の永続化が必要です。この例では、1つの明示的なディレクトリで両方を有効化しています:

```bash
export NEOGRAPH_HARNESS_STATE_DIR="$PWD/.neograph-harness-state"
```

これにより、不変アーティファクト、可変実行レコード、追記専用の因果ジャーナルが`runs.db`に保存され、グラフチェックポイントは`checkpoints.db`に保存される。ジャーナル行およびHarnessが作成した各チェックポイントは、そのランを不変アーティファクト、コンパイル済みリビジョンダイジェスト、MCPプロトコルバージョン、およびHarnessプロファイルにバインドする。ワーカーの試行には、時間、検証/リトライ結果、そしてプロバイダ、機能、ホスト仲介の呼び出しを発行試行に結び付ける相関IDが含まれる。両方のSQLiteストアはWALモードと境界付きビジータイムアウトを使用する。既存のバージョン1のレコードデータベースは、自動起動時にバージョン3へトランザクション的に移行する。ディレクトリはサーバ再起動後も保持される。`host_brokered`のカタログエントリは、ソースのいずれかが欠けている場合にはコンパイル時に拒否されるため、ワークフローは実際には存在しない再開可能性を宣伝することはできない。

カスタム埋め込みは、同じバックエンドを `SqliteHarnessRecordStore` からオプションの `neograph::mcp_sqlite` ターゲット経由で構築できます。デフォルトのジャーナルモードは、SQLiteで処理される前に、一般的な秘密およびコンテンツフィールドを再帰的に `[REDACTED]` で置き換えます。 `METADATA_ONLY` はすべてのイベントペイロードを破棄します。 `FULL` プロバイダコンテンツ、ツール引数、および結果を正確に保持し、ストレージ用に承認されたデータに対してのみ有効にする必要があります。イベントは `HarnessJournal::list_events(run_id, after_sequence, limit)` を通じて実行順に読み取ることができます。`FileHarnessRecordStore` は、原子JSONファイルを好む展開環境では引き続き利用可能です。その場合、それはジャーナルの境界 (journal boundary) を実装していません。

### 保持

SQLiteストアはオプションの `HarnessRetentionStore` 兄弟インターフェースを実装する；安定した `HarnessRecordStore` vtableは変更されない。アーティファクトを保持する前または実行を開始する前に、 `HarnessService` は `max_artifacts` と `max_runs` を `HarnessServiceConfig`から適用する。デフォルトはそれぞれ128である。

クリーンアップは、終端のリーフランのみを削除する。キューされたラン、実行中ラン、入力待ちランは保護されており、ジャーナルのファイナライズを終了していない進行中の実行も保護される。リプレイまたはフォークの行には`source_run_id`が記録されるため、その依存が保持されている間はそのソースを削除できない。帯域が必要な場合は、依存するリーフが先に削除され、ソースは保持された行がそれを参照しなくなった後でのみ、後のステップで削除対象となる。したがって、すべての候補がアクティブ、明示的に保護されている、またはまだ参照されるレコードである場合、制限はソフトである。

`runs.db`内では、1つのトランザクションがラン行の前にランのジャーナル行を削除し、アーティファクトは、それに参照するランがなくなった後にのみ削除される。そのコミットの後、Harnessは、削除されたランのチェックポイントスレッドを、別途設定されたチェックポイントストアから削除する。その第2フェーズ中にクラッシュまたはチェックポイントバックエンドの失敗が発生した場合、到達不能なチェックポイントストレージを残す可能性があるが、削除されたソースレコードを指す残存リプレイ/フォークを残すことはない。後の管理者またはバックエンド専用の孤立スイープにより、そのようなチェックポイント専用の残査を回収できる。

`FileHarnessRecordStore`は永続的なクリーンアップを実装していない。履歴上のインメモリアーティファクトキャッシュの退避とハードな実行容量制限の挙動はそのまま維持される。

## デバッガービュー

`neograph_get`は`status`をコンパクトなデフォルトとして保持し、別のMCPツールを追加することなく4つのデバッガービューを追加する。

| ビュー | 結果 |
|---|---|
| `attempts` | ジャーナル化されたワーカー試行の開始・完了・割り込みイベント |
| `trace` | 既存の順序付けられたGraphEngineノードトレースに加え、因果ジャーナルのタイムライン |
| `checkpoints` | ペイロードなしのチェックポイントメタデータ：ID、親、ノード、フェーズ、ステップ、チャネル名 |
| `diff` | 各チェックポイントとその親との間で変更されたチャンネル値とバージョン |

`attempts`と`trace`は`after_sequence`を不透明なフォワードカーソルとして受け付ける。4つのビューはすべて`limit`を1から1000まで受け付ける。返されるアーティファクトURIは、クエリと同じページネーションを保持することができる。例えば：

```text
neograph://runs/run_123/attempts?after_sequence=17&limit=50
```

これらのURIでは`after_sequence`と`limit`のみが受け付けられる。不明または不正なクエリフィールドは無視されずに失敗する。ジャーナルベースのビューは、永続化されたままのペイロードを返し、設定されたリダクションモードが保持される。`diff`ビューはジャーナルではなくチェックポイントストアから計算され、完全なチャンネル値を含む場合がある。そのビューへのアクセスは、既存の詳細な実行結果へのアクセスと同様に扱うこと。

## リプレイモード

`neograph_start`は別のMCPツールを追加することなく、完了した実行をリプレイできる。

```json
{"replay":{"source_run_id":"run_123","mode":"recorded"}}
```

⟪4810b78ec243⟫: `recorded` は、ソースジャーナルの完了済みワーカー試行結果を用いて、コンパイラロックされたグラフを再実行します。構成されたワーカー、プロバイダー、MCP、A2A、または capability executor を呼び出すことはありません。ソース成果物リビジョン、プロトコル、プロファイルは依然として一致している必要があり、ジャーナルは `FULL` ペイロードモードを使用しなければなりません。`REDACTED` および `METADATA_ONLY` ジャーナルは、正確なワーカー出力を保持していないため、意図的にリプレイできません。中断された試行は破棄されます。再開後に完了したワーカー呼び出しがリプレイされます。

`mode: "live"`を使用して、同じ保持されたアーティファクトをライブプロバイダーとツールで実行します。スナップショットとジャーナルライフサイクルイベントは、実行を`recorded_replay`または`live_replay`としてラベル付けし、`source_run_id`を含みます。通常の開始は`live`のままです。

## 互換性のあるフォーク

まず修復済みのHarnessをコンパイルし、その後、既存の`neograph_start`ツールを介して、直前のチェックポイントを正確にそのターゲットアーティファクトに分岐させる：

```json
{
  "fork": {
    "source_run_id": "run_123",
    "checkpoint_id": "550e8400-e29b-41d4-a716-446655440000",
    "artifact_id": "artifact_repaired"
  }
}
```

ソースチェックポイントは `source_run_id`に属している必要があります。ランを割り当てる前に、Harnessはチェックポイントスキーマ、ソースリビジョン、MCPプロトコル、Harnessプロファイル、復元されたすべてのチャネルとリデューサー、すべての継続ノード、およびアクティブなバリアインターフェースをターゲットアーティファクトに対して検証します。互換性のないブランチは `started: false`, `status: "incompatible_fork"`、および機械可読な `H_FORK_*` 診断を `path` と `witness`とともに返します。ランやフォークチェックポイントは作成されません。

チェックポイントストアが必要です。レコードストアがない場合、フォークは現在のサービスプロセス内に常駐するソース実行とアーティファクトのみを参照できます。再起動後も存続するフォーク系統には、両方のストアを構成してください。

互換性のあるブランチは`compatible_fork`としてラベル付けされ、開始応答、スナップショット、およびジャーナルライフサイクルイベントに`source_run_id`と`source_checkpoint_id`の両方を保持します。実行は選択されたチェックポイントの`next_nodes`で再開されます。すでにコミットされた先行ノードは再実行されません。ターゲットアーティファクトは修復されたトポロジー、ワーカー契約、ツールカタログを提供し、元のタスクチャネルを含む復元されたチャネル値はソースチェックポイントから取得されます。タスク入力自体を変更する必要がある場合は、フォークではなく新しい開始を使用してください。

ソース実行、アーティファクト、および選択されたチェックポイントはフォークの参照であり、互換性チェックまたはフォーク実行がそれらを使用できる間は保持されたままである必要があります。保持クリーンアップは、依存先を最初に削除するか、参照されたソースを保持する必要があります。プリフライトとブランチ作成の間にソースチェックポイントを削除してはなりません。

## Streamable HTTP

リモートトランスポートはオプトインであるため、既存のstdioのみのターゲットは小規模なままであり、HTTP/OpenSSL依存関係を暗黙的に追加することはありません：

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

エンドポイントは`http://127.0.0.1:8080/mcp`です。これは公開されたMCP 2025-11-25 Streamable HTTP POST契約を、セッションごとのMCPライフサイクルとJSON応答で実装します。通知はHTTP 202を返します。DELETEはセッションを終了します。オプションのスタンドアロンGET/SSEチャネルは意図的に実装されておらず、HTTP 405を返します。これはトランスポート仕様で明示的に許可されています。

セキュリティデフォルトはトランスポートレベルであり、認証を`GraphEngine`または`HarnessService`に結合しません。

- デフォルトのバインドは`127.0.0.1`です。ループバック以外のバインドは、ベアラーオーソライザーが設定されていない限り拒否されます。
- 提供されたすべての`Origin`は、`NEOGRAPH_HARNESS_ALLOWED_ORIGINS`（実行可能ファイル内ではカンマ区切り）のエントリと完全に一致しない限り拒否されます。
- `NEOGRAPH_HARNESS_BEARER_TOKEN`は、実行可能ファイルの単一プリンシパルベアラー境界を有効にします。ライブラリ埋め込みでは`MCPHttpServerConfig::bearer_authorizer`をOAuth/JWT検証に使用し、安定したプリンシパル/スコープを返すことができます。
- セッションは返された認可スコープにバインドされます。別の有効なプリンシパルは、漏洩した`Mcp-Session-Id`を再利用できません。
- `MCPHttpServer`ファクトリは検証済みスコープを受け取り、`MCPHttpServerSession`オーナーを返します。マルチテナント埋め込みは、分離されたHarnessレコード/チェックポイントストアを選択するためにスコープを使用する必要があります。認証状態はグラフランタイム自体には入りません。
- リクエストペイロード、HTTPワーカー、キュー、セッション、レスポンス待機の制限は`MCPHttpServerConfig`によって境界付けられる。

任意の非ループバック展開においては、信頼できるリバースプロキシでTLSを終端し、そのOAuth/OIDC検証、または同等の`bearer_authorizer`を使用してください。元の`Authorization`および`Origin`ヘッダーを転送し、平文の公開リスナーを公開せず、Harness状態ディレクトリごとに1つの認可ドメインを展開してください。

## ホストのセットアップ

`SERVER` には絶対パスを使用する:

```bash
SERVER=/absolute/path/to/build-harness/example_harness_mcp_server
```

Claude Code、ローカルプロジェクトスコープ:

```bash
claude mcp add --scope local --transport stdio neograph-harness -- "$SERVER"
claude mcp get neograph-harness
```

Codex CLI:

```bash
codex mcp add neograph-harness -- "$SERVER"
codex mcp list
```

非対話型の `codex exec` をこの信頼されたローカルサーバーに対して行う場合は、Codex 内で `mcp_servers.neograph-harness.default_tools_approval_mode = "approve"` に `config.toml` を設定します。これがない場合、Codex は `neograph_compile` を正しくキャンセルします。これは、成果物の保持が読み取り専用として注釈されていないためです。対話型セッションでは、代わりにデフォルトのプロンプトを維持してもかまいません。

OpenCode、プロジェクト`opencode.json`またはユーザー設定内:

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

`opencode mcp list`で検証してください。これらの形式は、各ホストの公式MCP設定契約に従って 2026-07-21 に審査されたものです。

## PRレビューワークフロー

ホストに通常のリポジトリツールでPR差分を収集させ、その後Harnessツールを使用するよう依頼してください。適切なリクエストは、

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

### プロバイダー予算

`budgets.provider_timeout_seconds` 1回のプロバイダー完了試行を1〜600秒に制限します。 `budgets.max_output_tokens` 1回の完了を1〜128000生成トークンに制限します。両方ともオプションです。どちらかを省略すると、以前の動作が維持され、Harnessの期限なしで、プロバイダーの既存の出力制限が適用されます。

A workerは、どちらのフィールドもより小さい値に設定できます。Harness全体の値を超えるworkerの値は、コンパイル時に拒否されます。期限が来ると、Harnessはそのプロバイダー呼び出しに渡された子キャンセレーション。トークンのみをキャンセルし、兄弟ワーカーやenclosing runはキャンセルしません。プロバイダーはトークンを尊重する必要があるため、割り込みできないプロバイダーは期限後に戻ることがあります。

ホストは次の順序に従うべきです：⟦8472bf5349dc⟧

1. コール`neograph_compile`を行い、`ok`が偽の場合は停止する。
2. `neograph_start`を、返された`artifact_id`とともに呼び出します。
3. Poll `neograph_get` を `run_id` で実行; これは結果とカウントのみを返します。
4. 詳細が必要な場合は、 `neograph_get` 同じ `run_id` と、返された `neograph://runs/...` URI を `uri`として呼び出してください。デフォルトではトレースをコンテキストに取り込まないでください。

### 来歴の特定

detailsアーティファクトは、スキーマ検証済みの各ワーカー応答を`workers`に保持し、既存クライアント向けに確立されたフラットな`findings`配列を維持します。`finding_sources`は同じ長さの並列配列です。各エントリには、集約された`finding_index`、ソース`worker_id`、およびそのワーカーの`local_index`が含まれます。`F1`などの重複するローカルIDのソースを特定するためにこれを使用してください。ワーカーが宣言したfindingオブジェクトに来歴フィールドを追加しないでください。

## ホスト仲介による再開

MCPホストがワーカープロセスではなく機能を所有する場合に`executor.kind: "host_brokered"`を使用してください。`executor.interaction`を`"tool_result"`(デフォルト)または`"input"`に設定します。プロバイダーexecutorは要求された引数を検証し、次の2つの非終端実行状態のいずれかを返します：

- `awaiting_tool_results`：ホストが指定された機能を実行する必要があります。
- `input_required`:ホストは入力値を収集する必要がある。

`neograph_get` には、一意の `pending` オブジェクトと、`call_id`、`tool_id`、検証済みの `arguments`、および `result_schema` が含まれます。その呼び出しを正確に次の方法で送信してください:

```json
{
  "run_id": "run_...",
  "call_id": "hcall_...",
  "result": {"answer": "validated host result"}
}
```

`neograph_resume`は、不一致のコールID、宣言されたスキーマに違反する結果、期限切れのコール、および待機していない実行に対する遅延結果を拒否する。同一の重複はグラフを再実行せずに確認されるが、矛盾する重複は拒否される。受け入れられたレジューム意図は、実行がスケジュールされる前に永続化されるため、プロセスクラッシュ後のポーリングは、成功した並行する兄弟ワーカーを繰り返すことなく、`NodeInterrupt`チェックポイントからレジュームを再開する。

### 外部効果と調整

通常のホスト仲介契約は後方互換性がある。`executor.effect`のないカタログエントリは、プロセス再起動後も`awaiting_tool_results`のままであり、同じ`{run_id, call_id, result}`レジューム要求を受け入れる。

ホストのケイパビリティが外部から観測可能で非冪等な変更を行える場合、そのリスクを明示的に宣言してください。エフェクトメタデータはデフォルトの`host_brokered` `tool_result`インタラクションでのみ有効です。これは入力収集メタデータではありません。

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

保留中の呼び出しには、永続的な `effect` オブジェクトが含まれます。その `effect_id` と `idempotency_key` はHarness実行にスコープされ、プロバイダーのツールコールIDとは異なります。 `status_query` と `fencing` はホストの機能を説明します。Harnessはそれらを記録しますが、プロバイダー固有のクエリや再試行プロトコルを独自に作成することはありません。

サービスが`idempotency: "unsupported"`呼び出しをまだ待機中に再接続した場合、その実行のみが`ambiguous_effect`に変更されます。つまり、ホストがプロセス停止前にエフェクトを実行した可能性はありますが、Harnessはどちらの結果も証明できません。コンパクトステータスには`pending`および`ambiguity`が含まれ、ジャーナルには`host_brokered.effect.ambiguous`が記録され、Harnessはツールのリプレイを行わず、エフェクトを失敗または完了として報告しません。

ホスト自らの権威あるシステムを確認した後、`neograph_resume`を通してそのあいまいさを解決する：

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"completed","result":{"answer":"validated host result"}}
```

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"failed"}
```

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"unknown"}
```

`completed`は`result`を検証・消費し、その後チェックポイントから再開します。`failed`はワーカーを再実行せずに終端のHarness失敗を記録します。`unknown`は実行を`ambiguous_effect`状態のままにして、後の調整に備えます。完了・失敗・不明の重複送信は冪等です。競合する完了または失敗の送信は拒否されます。重複しないすべての委託は`host_brokered.effect.reconciled`としてジャーナルに記録されます。

曖昧なエフェクトは意図的にキャンセル不可であり、失効もしない。キャンセルやタイムアウトでは外部エフェクトが発生したかどうかを確定できず、権限を持つシステムがまだそれを解決できない場合は `unknown` を提出されたし。

本プロトコルは、ホストクラッシュをまたいだ完全に一回だけの配信（exactly-once delivery）を主張しない。幂等キーやステータスクエリをサポートするホストは、それらのシステムを使用して、実際の結果を確定してからreconciliationを提出すべきである。

実行スナップショットには`created_at`、`updated_at`、`expires_at`、`poll_after_ms`が含まれます。デフォルトのTTLは24時間、デフォルトのポーリング間隔は1秒です。埋め込みは`HarnessServiceConfig`を通じて両方を上書きできます。

## 実験的タスクプロファイル

MCPタスクはコアMCP 2025-11-25の一部ではなく、上流エクステンションはまだ自身を実験的と表示しています。したがって、NeoGraphはそれをデフォルトで無効にし、安定した`run_id`および`neograph_get`ポーリング契約から分離したままにします。

オプトインするには、example サーバー上で永続状態(durable state)も有効化する必要があります:

```bash
export NEOGRAPH_HARNESS_STATE_DIR="$PWD/.neograph-harness-state"
export NEOGRAPH_HARNESS_EXPERIMENTAL_TASKS=1
```

その後、サーバーは `io.modelcontextprotocol/tasks`をアドバタイズし、 `neograph_start` にオプションのタスクサポートをマークし、さらに `tasks/get`, `tasks/update`と `tasks/cancel`を提供します。また、 `CreateTaskResult` は、個々の `tools/call` リクエストに以下が含まれる場合にのみ返されます:

```json
{
  "_meta": {
    "io.modelcontextprotocol/clientCapabilities": {
      "extensions": {"io.modelcontextprotocol/tasks": {}}
    }
  }
}
```

そのリクエストのオプトインがないクライアントは、通常の `CallToolResult` を受け取り、ポーリングを継続します `neograph_get`；プロファイルを有効にしても、安定したフォールバックは変わりません。タスクステータスは `working`, `input_required`, `completed`, `failed`、および `cancelled`. `tasks/update.inputResponses` は保留中の `call_id`によってキーが設定され、ポーリングするクライアントは `pollIntervalMs` と `ttlMs`.

## 機能バックエンド

`make_provider_harness_executor` は、任意の NeoGraph `Provider` を通じてワーカーを駆動します。モデルが宣言されたツールを要求した場合、エグゼキュータはディスパッチの前後で、その引数と出力をカタログに対して意味検証します。

`make_mcp_harness_capability_executor` を使用して初期化された下流の `MCPClient` インスタンス、または `a2a::make_harness_capability_executor` を A2Aエージェント向けに使用する。リクエストが権限の源泉であり続ける：ワーカーは自分の `tools` 配列に記載されたツールIDのみを見る。

ファイルシステムツールの場合、`path_arguments`内のすべてのパスを含む入力を宣言し、`policy.workspace_roots`を設定してください。相対パスは最初のルートの下で解決されます。設定されたすべてのルートの外にある正規パスは、既存のシンボリックリンクを介したエスケープを含め、ディスパッチ前に拒否されます。正規パスは、モデルが提供した表記ではなく、ケイパビリティバックエンドに渡されます。下流のMCPおよびA2Aサービスは、別の信頼境界のままであり、ファイルシステムのtime-of-check/time-of-use競合を閉じるために同じルートポリシーを適用する必要があります。`policy.read_only: true`を使用すると、コンパイルは`read_only: true`とマークされていないすべてのカタログエントリを拒否します。

## デプロイメントとプロトコルプロファイル

サポートされるローカル配布パスは、上記のインストール可能な`neograph-harness-mcp`バイナリです。ソースビルドは引き続きサンプルターゲットを使用でき、Pythonホイールはリモートデーモンを暗黙的にインストールするのではなく、ライブラリ/ランタイムパッケージのままです。MCPBおよび公式レジストリへの公開は、リリース/ディスカバリのパッケージングオプションのままです。これらはワイヤプロトコルに必須ではなく、署名付きリリースアーティファクトと明示的なリモート認証デプロイメントマニフェストを使用した場合にのみ追加されるべきです。

NeoGraphは現在、日付付きのMCP `2025-11-25`プロファイルのみを公開しています。将来のステートレスプロトコルを説明する最終SEPは、新しいワイヤバージョンを作成しません。MCPプロジェクトが新しい日付付き仕様を公開するまで、後継プロファイルはアドバタイズされません。
