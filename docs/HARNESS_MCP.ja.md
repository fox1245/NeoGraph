<!-- neograph-i18n: source=docs/HARNESS_MCP.md locale=ja source_sha256=d649c0a0a5d99d39d6a84ec5a4b48707f6b5f49a7a5143ff3ce3aa13c8b9436b -->
# NeoGraph ハーネス MCP

**Languages:** [English](HARNESS_MCP.md) | [한국어](HARNESS_MCP.ko.md) | [日本語](HARNESS_MCP.ja.md) | [简体中文](HARNESS_MCP.zh-CN.md)

NeoGraph Harness は、実行前に制限されたマルチワーカー ワークフローをコンパイルします。の
安定した MCP 表面は 6 つのツールで維持されます。

- `neograph_schema` は、インストールされているリクエスト コントラクトとプリセットを検出します。
- `neograph_compile` は、実行せずにエラボレート、コンパイル、および検証を行います。
- `neograph_start` は、保持されたアーティファクトまたはインライン リクエストを開始します。
- `neograph_get` は、コンパクト ステータスをポーリングするか、結果のアーティファクト URI を逆参照します。
- `neograph_resume` は、保留中のホストの正確な結果を検証して送信します。
- `neograph_cancel` は、キューに入れられた、実行中、または待機中のワークフローを連携してキャンセルします。

同梱プリセットは `fanout_judge`、`pr_review_panel`、`bug_triage`、
`research_synthesis` です。プリセットは strict-Core グラフ成果物を生成し、
JavaScript リクエストは独自の `ProgramSource` エンベロープとソースマップを保持します。

### JavaScript オーサリング境界

新規公開で `harness.mode` が受け入れるのは `preset` と `javascript` だけです。
JavaScript ソースは `harness.source` に置き、`harness.source_id` でソース ID を
固定できます。`define()` は封印された `ng` バインディングからグラフを一つ構築し、
任意のジェネレーター `main()` は通常の JavaScript ループと分岐を実行しながら、
`ng.callCore`、`ng.all`、`ng.any`、`ng.race` などの型付きコマンドだけを yield
します。

```json
{
  "harness": {
    "mode": "javascript",
    "source_id": "review:main.js",
    "source": "export function define() { const g = ng.graph('main'); /* ... */ return g; }"
  }
}
```

#### 制御フロー移行例

`define()` はコンパイル時に限定し、実行時のエフェクトはすべて yield された型付き
コマンドの背後に置きます。この完全なリクエストは、ジェネレーターに 2 操作と
2 並列の上限を与えます。ワーカーノードはリクエストからホストが封印した設定と
完全に一致し、終端戻り値は Harness の結果形式です。

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

安定したソースサイト文字列は永続コマンド座標の一部です。再試行や再起動の
間でも決定的に保ってください。`ng.any(...)` は必要数の成功が先に揃った場合、
`ng.race(...)` は最初の終端メンバーを勝者にする場合に使います。どちらも構造化
並行処理により未完了の兄弟をキャンセルします。周辺 I/O、タイマー、動的ロード、
`eval`、ネイティブハンドルは引き続き利用できません。

`harness.mode` は必須です。`dsl` は `H_MIGRATION_CORE_DSL`、`core` は
`H_MIGRATION_CORE_JSON`、`program`/`program_json` は
`H_MIGRATION_PROGRAM_JSON` で失敗します。strict Core JSON と信頼済み C++
構築は内部表現としてのみ残り、公開 Harness オーサリング言語ではありません。

スキーマ出力、コンパイル、開始は同一の不変 `HarnessAdmissionProfile` を使用します。
解決対象はスコープ付き `GraphRegistry` だけで、プロセス全体の登録へのフォールバックは
ありません。拒否された入力はノード、ワーカー、エフェクトをディスパッチしません。

プリセットと JavaScript はともに `ProgramSource`、`ProgramCompiler`、
`ProgramCatalog`、`ProgramRuntime` を通り、`GraphEngine` が唯一のノード実行器です。
保存済みレガシー成果物は `translated`、`drain_only`、`rejected` に明示分類されます。
`drain_only` は新規公開や新規実行を許可せず、完全に保持された旧ランタイムで既存実行だけを
再開できます。

## 構築して実行

OpenAI 互換プロバイダー アダプターを使用してローカル stdio サーバーを構築します。

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

`NEOGRAPH_HARNESS_API_KEY` は `OPENAI_API_KEY` よりも優先されます。
`NEOGRAPH_HARNESS_BASE_URL` は、OpenAI 互換のエンドポイントを選択します。サーバー
`https://openrouter.ai/api` などのバージョン管理されていないベースと、
プロバイダーの文書化されたバージョン形式 (`https://openrouter.ai/api/v1` など)。
`/v1` が欠落している場合にのみ追加されます。サーバーはプロトコル メッセージを次の宛先にのみ書き込みます。
stdout と診断は stderr のみに出力されます。を参照してください。
現在の[OpenRouter quickstart](https://openrouter.ai/docs/quickstart)
エンドポイント形式。

ホスト相互運用性スモーク テストのみの場合は、`NEOGRAPH_HARNESS_SMOKE=1` を設定します。
この明示的モードでは、有効な値を返す決定論的なインプロセス プロバイダーが使用されます。
ゼロ結果レビュー、API キーは不要、LLM として使用しないでください
品質テスト。

永続的なホストブローカー呼び出しには、レコードとチェックポイントの両方の永続性が必要です。
この例では、1 つの明示的なディレクトリで両方を有効にします。

```bash
export NEOGRAPH_HARNESS_STATE_DIR="$PWD/.neograph-harness-state"
```

これには、不変のアーティファクト、変更可能な実行レコード、および追加専用の因果関係が保存されます。
`runs.db` のジャーナル、`checkpoints.db` のグラフ チェックポイント。仕訳帳行
そして、ハーネスによって作成されたすべてのチェックポイントは、実行をその不変のアーティファクトにバインドします。
コンパイルされたリビジョン ダイジェスト、MCP プロトコル バージョン、およびハーネス プロファイル。ワーカー
試行には、期間、検証/再試行の結果、相関 ID が含まれます。
プロバイダー、機能、およびホストが仲介する呼び出しを、発行試行に参加させます。
どちらの SQLite ストアも WAL モードと制限付きビジーを使用します。
タイムアウト。既存のバージョン 1 レコード データベースはトランザクション的にバージョン 1 に移行されます
開けると3。ディレクトリはサーバーの再起動後も存続します。あ
いずれかのストアが存在する場合、`host_brokered` カタログ エントリはコンパイル時に拒否されます。
欠落しているため、ワークフローは、それが持っていない再開可能性をアドバタイズできません。

カスタム埋め込みは、同じバックエンドを構築できます。
オプションの `neograph::mcp_sqlite` ターゲットからの `SqliteHarnessRecordStore`。
デフォルトのジャーナル モードは、共通の秘密フィールドとコンテンツ フィールドを再帰的に置き換えます。
SQLite がそれらを認識する前に、`[REDACTED]` を使用します。 `METADATA_ONLY` はすべてのイベントを破棄します
ペイロード; `FULL` はプロバイダーのコンテンツ、ツールの引数、結果を正確に保存します
また、保存が承認されたデータに対してのみ有効にする必要があります。イベントを読み込むことができます
`HarnessJournal::list_events(run_id, after_sequence, limit)` を通じて注文を実行します。
`FileHarnessRecordStore` は、アトミックを優先するデプロイメントで引き続き利用可能です
JSON ファイル。ジャーナル境界は実装されません。

### 保持

SQLite ストアはオプションの `HarnessRetentionStore` 兄弟を実装します。
インタフェース;安定した `HarnessRecordStore` vtable は変更されません。保持する前
アーティファクトまたは実行の開始時、`HarnessService` は `max_artifacts` を適用し、
`HarnessServiceConfig`からの`max_runs`。デフォルトはそれぞれ 128 です。

クリーンアップでは、ターミナル リーフの実行のみが削除されます。キューに入れられた実行、実行中の実行、および入力待機中の実行
ジャーナルが完了していないインプロセス実行も保護されます。
ファイナライズ。リプレイ行またはフォーク行は `source_run_id` を記録するため、そのソースは実行できません。
依存関係は保持されたままですが、削除されます。スペースが必要な場合は、
依存するリーフが最初に削除されます。ソースは後でのみ適格になります
保持された行が参照されなくなった後のステップ。したがって、制限は、次の場合にソフトになります。
候補はアクティブであるか、明示的に保護されているか、まだ参照されています。

`runs.db` 内では、1 つのトランザクションが実行前に実行のジャーナル行を削除します。
行を削除し、実行が参照しない場合にのみアーティファクトを削除します。そのコミット後、
ハーネスは、削除された実行のチェックポイント スレッドを個別に削除します。
構成されたチェックポイント ストア。その際のクラッシュまたはチェックポイント バックエンドの障害
2 番目のフェーズでは、到達不能なチェックポイント ストレージを残すことができますが、ストレージを残すことはできません。
削除されたソース レコードを指すリプレイ/フォークが保持されています。その後の行政
または、バックエンド固有の孤立したスイープによって、そのようなチェックポイントのみの残余が再利用される可能性があります。

`FileHarnessRecordStore` は永続的なクリーンアップを実装しません。その歴史的な
メモリ内のアーティファクト キャッシュの削除とハード ラン容量の動作は残ります。

## デバッガービュー

`neograph_get` は `status` をコンパクトなデフォルトとして維持し、4 つのデバッガを追加します
別の MCP ツールを追加せずにビューを表示します。

|見る |結果 |
|---|---|
| `attempts` |ジャーナリングされたワーカー試行の開始/完了/中断イベント |
| `trace` |既存の順序付けされた GraphEngine ノード トレースと因果ジャーナル タイムライン |
| `checkpoints` |ペイロードフリー チェックポイント メタデータ: ID、親、ノード、フェーズ、ステップ、およびチャネル名 |
| `diff` |各チェックポイントとその親の間でチャネルの値とバージョンが変更されました。 |

`attempts` および `trace` は、`after_sequence` を不透明な前方カーソルとして受け入れます。全て
4 つのビューは、1 から 1000 までの `limit` を受け入れます。返されるアーティファクト URI は次のとおりです。
クエリと同じページネーション。例:

```text
neograph://runs/run_123/attempts?after_sequence=17&limit=50
```

これらの URI では、`after_sequence` と `limit` のみが受け入れられます。不明または
不正な形式のクエリ フィールドは無視されずに失敗します。ジャーナルに裏付けられたビュー
永続化されたペイロードを正確に返すため、構成されたリダクション モードは次のようになります。
保存されています。 `diff` ビューは、チェックポイント ストアではなくチェックポイント ストアから計算されます。
ジャーナルには完全なチャネル値を含めることができます。それへのアクセスをアクセスと同様に扱います
既存の詳細な実行結果に反映されます。

## リプレイモード

`neograph_start` は、別の MCP ツールを追加せずに、完了した実行を再生できます。

```json
{"replay":{"source_run_id":"run_123","mode":"recorded"}}
```

`recorded` は、ソース ジャーナルのコンパイラ ロックされたグラフを再実行します。
完了したワーカーの試行結果。設定されたワーカーを呼び出すことはありません。
プロバイダー、MCP、A2A、または機能エグゼキューター。ソースアーティファクトのリビジョン、
プロトコル、プロファイルは依然として一致する必要があり、ジャーナルは `FULL` ペイロードを使用する必要があります
モード。 `REDACTED` および `METADATA_ONLY` ジャーナルは意図的に再生できません。
正確なワーカー出力は保持されません。中断された試行は破棄されます。
完了した再開後のワーカー呼び出しが再実行されます。

`mode: "live"` を使用して、保持されている同じアーティファクトをライブプロバイダーで実行し、
ツール。スナップショットとジャーナルのライフサイクル イベント ラベルは `recorded_replay` として実行されるか、
`live_replay` および `source_run_id` を含む。通常のスタートは`live`のままです。

## 互換性のあるフォーク

まず修復したハーネスをコンパイルし、次に正確な直前のチェックポイントを分岐させます。
既存の `neograph_start` ツールを使用してそのターゲット アーティファクトを実行します。

```json
{
  "fork": {
    "source_run_id": "run_123",
    "checkpoint_id": "550e8400-e29b-41d4-a716-446655440000",
    "artifact_id": "artifact_repaired"
  }
}
```

ソース チェックポイントは `source_run_id` に属している必要があります。実行を割り当てる前に、
ハーネスはチェックポイント スキーマ、ソース リビジョン、MCP プロトコルを検証します。
ハーネス プロファイル、復元されたすべてのチャネルとリデューサー、すべての継続ノード、
ターゲットアーティファクトに対するアクティブなバリアインターフェイス。互換性のない
ブランチは `started: false`、`status: "incompatible_fork"`、および
`path` および `witness` による機械可読 `H_FORK_*` 診断。それは何も生み出さない
チェックポイントを実行またはフォークします。

チェックポイント ストアが必要です。レコード ストアがなければ、フォークが参照する可能性があります。
ソースの実行とアーティファクトのみが現在のサービス プロセスにまだ存在します。
再起動後も存続する必要があるフォークリネージ用に両方のストアを構成します。

互換性のあるブランチには `compatible_fork` というラベルが付けられ、両方のブランチが含まれます。
開始応答、スナップショット、および
ライフサイクルジャーナルイベント。選択したチェックポイントで実行が再開されます
`next_nodes`;すでにコミットされた先行プログラムは再度実行されません。ターゲット
アーティファクトは、修復されたトポロジ、作業者契約、およびツール カタログを提供します。
一方、元のタスク チャネルを含む復元されたチャネル値は、
ソースチェックポイント。タスク入力時にフォークではなく新規スタートを使用する
それ自体が変わらなければなりません。

ソースの実行、アーティファクト、選択されたチェックポイントはフォークの参照であり、
互換性チェックまたはフォーク実行が使用できる間は保持しておく必要があります。
彼ら。保持クリーンアップでは、最初に依存関係を削除するか、参照を保持する必要があります
情報源。プリフライトとブランチの間のソース チェックポイントを決して削除してはなりません
創造。

## ストリーミング可能なHTTP

リモート トランスポートはオプトインであるため、既存の stdio のみのターゲットは小規模のままであり、
黙って HTTP/OpenSSL 依存関係を取得しません。

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

エンドポイントは `http://127.0.0.1:8080/mcp` です。公開された MCP を実装します
2025-11-25 セッションごとの MCP ライフサイクルを備えたストリーミング可能な HTTP POST コントラクトと
JSON 応答。通知は HTTP 202 を返します。DELETE はセッションを終了します。
オプションのスタンドアロン GET/SSE チャネルは意図的に実装されておらず、
HTTP 405 を返しますが、これはトランスポート仕様で明示的に許可されています。

セキュリティのデフォルトはトランスポートレベルであり、認証を連動させません。
`GraphEngine` または `HarnessService`:

- デフォルトのバインドは `127.0.0.1` です。非ループバック バインドは、
  ベアラーオーソライザーが設定されています。
- 指定されたすべての `Origin` は、次のエントリと正確に一致しない限り拒否されます。
  `NEOGRAPH_HARNESS_ALLOWED_ORIGINS` (実行可能ファイル内ではカンマ区切り)。
- `NEOGRAPH_HARNESS_BEARER_TOKEN` は実行可能ファイルの単一プリンシパルを有効にします
  ベアラー境界。ライブラリの埋め込みで使用できるもの
  OAuth/JWT 検証の場合は `MCPHttpServerConfig::bearer_authorizer` を返し、
  安定したプリンシパル/スコープ。
- セッションは、返された承認スコープにバインドされます。別の有効な
  プリンシパルは、リークした `Mcp-Session-Id` を再利用できません。
- `MCPHttpServer` ファクトリは、検証されたスコープを受け取り、
  `MCPHttpServerSession`オーナー。マルチテナントの埋め込みでは、スコープを使用して次のことを行う必要があります。
  分離されたハーネス レコード/チェックポイント ストアを選択します。認証なし状態になります。
  グラフ ランタイム自体。
- リクエストのペイロード、HTTP ワーカー、キュー、セッション、および応答待機の制限は次のとおりです。
  `MCPHttpServerConfig` によって制限されます。

非ループバック展開の場合は、信頼できるリバース プロキシで TLS を終了し、
OAuth/OIDC 検証または同等の `bearer_authorizer` を使用します。転送します
元の `Authorization` および `Origin` ヘッダー、平文パブリックを公開しません
リスナーを作成し、ハーネス状態ディレクトリごとに 1 つの認可ドメインを展開します。

## ホストのセットアップ

`SERVER` には絶対パスを使用します。

```bash
SERVER=/absolute/path/to/build-harness/example_harness_mcp_server
```

クロード コード、ローカル プロジェクトの範囲:

```bash
claude mcp add --scope local --transport stdio neograph-harness -- "$SERVER"
claude mcp get neograph-harness
```

コーデックス CLI:

```bash
codex mcp add neograph-harness -- "$SERVER"
codex mcp list
```

この信頼できるローカル サーバーに対する非対話型 `codex exec` の場合、次のように設定します。
`mcp_servers.neograph-harness.default_tools_approval_mode = "approve"`
コーデックス`config.toml`。これがないと、Codex は `neograph_compile` を正しくキャンセルします。
アーティファクトの保持には読み取り専用の注釈が付けられないためです。インタラクティブなセッション
代わりにデフォルトのプロンプトをそのまま使用することもできます。

OpenCode、プロジェクト `opencode.json` またはユーザー構成:

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

`opencode mcp list`で確認してください。これらのフォームは各ホストの公式 MCP に従います。
2026 年 7 月 21 日に確認された構成契約。

## PR レビューのワークフロー

通常のリポジトリ ツールを使用して PR 差分を収集するようにホストに依頼し、次を使用します。
ハーネスツール。適切なリクエストは次のとおりです。

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

### プロバイダーの予算

`budgets.provider_timeout_seconds` は、プロバイダーの完了試行を 1 回に制限します。
1 ～ 600 秒。 `budgets.max_output_tokens` は 1 つの完了を 1 ～ 128000 に制限します
生成されたトークン。どちらもオプションです。どちらかを省略すると、以前の内容が保持されます。
ハーネスの期限やプロバイダーの既存の出力制限はありません。

作業者はどちらかのフィールドをより小さい値に設定できます。ワーカーの値が
ハーネス全体の値はコンパイル時に拒否されます。締め切り間近にハーネスがキャンセル
そのプロバイダー呼び出しに提供される子キャンセル トークンのみ。そうではありません
兄弟ワーカーまたはそれを囲む実行をキャンセルします。プロバイダーはトークンを尊重する必要があります。
そのため、中断できないプロバイダーは期限後に復帰する可能性があります。

ホストは次の順序に従う必要があります。

1. `neograph_compile` を呼び出し、`ok` が false の場合は停止します。
2. 返された `artifact_id` を使用して `neograph_start` を呼び出します。
3. `run_id` で `neograph_get` をポーリングします。これは結果とカウントのみを返します。
4. 詳細が必要な場合は、同じ `run_id` と
   `neograph://runs/...` URI を `uri` として返しました。トレースをコンテキストに取り込まない
   デフォルトでは。

### 来歴を見つける

詳細アーティファクトは、スキーマ検証された各ワーカー応答を
`workers` を使用し、既存のクライアント用に確立されたフラット `findings` 配列を保持します。
`finding_sources` は同じ長さの並列配列です。各エントリには
集約 `finding_index`、ソース `worker_id`、およびそのワーカーの `local_index`。
これを使用して、`F1` などの重複したローカル ID のソースを特定します。追加しないでください
出所フィールドをワーカーの宣言された検索オブジェクトに追加します。

## ホスト仲介履歴書

ワーカーではなく MCP ホストの場合は `executor.kind: "host_brokered"` を使用します
プロセス、能力を所有します。 `executor.interaction` を `"tool_result"` に設定します
(デフォルト) または `"input"`。プロバイダーエグゼキュータは、要求された引数を検証します。
そして、次の 2 つの非ターミナル実行状態のいずれかを返します。

- `awaiting_tool_results`: ホストは指定された機能を実行する必要があります。
- `input_required`: ホストは入力値を収集する必要があります。

`neograph_get` には、一意の `call_id`、`tool_id`、
`arguments` および `result_schema` を検証しました。その呼び出しを正確に次の方法で送信してください。

```json
{
  "run_id": "run_...",
  "call_id": "hcall_...",
  "result": {"answer": "validated host result"}
}
```

`neograph_resume` は不一致のコール ID を拒否し、その結果は規則に違反します。
宣言されたスキーマ、期限切れの呼び出し、待機していない実行の遅延結果。アン
同一の重複はグラフを再実行せずに認識されます。ある
競合する重複は拒否されます。受け入れられた再開インテントは保持されます
実行がスケジュールされる前に行われるため、プロセスのクラッシュ後にポーリングすると、
成功した兄弟を繰り返さずに `NodeInterrupt` チェックポイントから再開します
労働者。

### 外部効果と調整

通常のホスト仲介契約には下位互換性があります: カタログ エントリ
`executor.effect`なし 処理後も`awaiting_tool_results`のまま
再起動し、同じ `{run_id, call_id, result}` 再開リクエストを受け入れます。

外部から見える非べき等性を実現できるホスト機能の場合
変更する場合は、そのリスクを明示的に宣言します。エフェクトメタデータは、
デフォルトの `host_brokered` `tool_result` インタラクション。入力コレクションではありません
メタデータ。

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

保留中の呼び出しには、永続的な `effect` オブジェクトが含まれます。その`effect_id`と
`idempotency_key` はハーネス実行にスコープされており、プロバイダーのとは異なります。
ツール呼び出し ID。 `status_query` と `fencing` はホストの機能を説明します。ハーネス
それらは記録されますが、プロバイダー固有のクエリや再試行プロトコルは作成されません。

`idempotency: "unsupported"` 通話中にサービスが再接続した場合
待っていると、その実行のみが `ambiguous_effect` に変更されます。これはホストを意味します
プロセスが停止する前にエフェクトを実行できた可能性がありますが、ハーネスは実行できません
どちらかの結果を証明します。コンパクトステータスには`pending`と`ambiguity`が含まれます。
ジャーナルには `host_brokered.effect.ambiguous` が記録されており、ハーネスもどちらも記録していません
ツールを再実行することも、効果が失敗したか完了したかを報告することもありません。

ホストが自身のチェックを行った後、`neograph_resume` を通じて曖昧さを解決します。
権威あるシステム:

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"completed","result":{"answer":"validated host result"}}
```

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"failed"}
```

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"unknown"}
```

`completed` は `result` を検証して消費し、チェックポイントから再開します。
`failed` は、ワーカーを再度実行せずに、ターミナル ハーネスの障害を記録します。
`unknown` は、後の調整のために実行を `ambiguous_effect` に残します。ちょうど
重複した完了、失敗、または不明な送信は冪等です。矛盾した
完了または失敗した提出は拒否されます。重複しないすべての調整
`host_brokered.effect.reconciled` としてジャーナルに記録されます。

曖昧な効果は意図的にキャンセルできず、期限切れになりません。あ
キャンセルまたはタイムアウトでは、外部効果が発生したかどうかを確認できません。提出する
権威あるシステムがまだ解決できない場合は、`unknown`。

このプロトコルは、ホストがクラッシュしても 1 回限りの配信を要求しません。ホスト
冪等性キーまたはステータスクエリをサポートするシステムは、それらのシステムを使用して、
調整を提出する前に実際の結果を決定します。

実行スナップショットには、`created_at`、`updated_at`、`expires_at`、および
`poll_after_ms`。デフォルトの TTL は 24 時間で、デフォルトのポーリング間隔は
1秒。埋め込みは、`HarnessServiceConfig` を通じて両方をオーバーライドできます。

## 実験的タスクのプロファイル

MCP タスクはコア MCP 2025-11-25 の一部ではなく、アップストリーム拡張機能はまだ残っています
それ自体に実験的なラベルを付けます。したがって、NeoGraph はデフォルトで無効のままにし、
安定した `run_id` と `neograph_get` ポーリング コントラクトとは別のものです。

サンプル サーバーでオプトインするには、永続状態も有効にする必要があります。

```bash
export NEOGRAPH_HARNESS_STATE_DIR="$PWD/.neograph-harness-state"
export NEOGRAPH_HARNESS_EXPERIMENTAL_TASKS=1
```

次に、サーバーは `io.modelcontextprotocol/tasks` をアドバタイズし、マークを付けます
オプションのタスクサポートを備えた `neograph_start`、`tasks/get` を提供します。
`tasks/update`、および`tasks/cancel`。次の場合にのみ `CreateTaskResult` を返します。
個々の `tools/call` リクエストには以下が含まれます。

```json
{
  "_meta": {
    "io.modelcontextprotocol/clientCapabilities": {
      "extensions": {"io.modelcontextprotocol/tasks": {}}
    }
  }
}
```

そのリクエストをオプトインしていないクライアントは、通常の `CallToolResult` を受け取り、
`neograph_get` のポーリングを続行します。プロファイルを有効にしても安定したものは変わりません
後退する。タスクのステータスは `working`、`input_required`、`completed`、`failed`、
そして`cancelled`。 `tasks/update.inputResponses` は保留中のキーによってキー設定されます
`call_id`、およびポーリング クライアントは `pollIntervalMs` および `ttlMs` を尊重する必要があります。

## 機能バックエンド

`make_provider_harness_executor` はあらゆる NeoGraph を通じてワーカーを駆動します
`Provider`。モデルが宣言されたツールを要求すると、エグゼキュータはそのツールを検証します。
ディスパッチ前後のカタログに対する引数と出力。

初期化されたダウンストリームには `make_mcp_harness_capability_executor` を使用します
`MCPClient` インスタンス、または A2A の場合は `a2a::make_harness_capability_executor`
エージェント。リクエストは引き続き権限を持ちます。作業者にはリストされたツール ID のみが表示されます。
`tools` 配列内。

ファイルシステム ツールの場合は、パスを含むすべての入力を `path_arguments` で宣言し、
`policy.workspace_roots`を設定します。相対パスは最初のルートの下で解決されます。
設定されたすべてのルートの外側にある正規パスはディスパッチ前に拒否されます。
既存のシンボリックリンクを介したエスケープも含まれます。正規パスが渡されます
モデルが提供するスペルではなく、機能バックエンド。下流MCP
と A2A サービスは別個の信頼境界を維持し、同じものを強制する必要があります。
ファイルシステムのチェック時間/使用時間の競合を解消するルート ポリシー。
`policy.read_only: true` を使用すると、コンパイルですべてのカタログ エントリが拒否されます。
`read_only: true`とマークされています。

## ディストリビューションとプロトコルのプロファイル

サポートされているローカル配布パスはインストール可能です。
上記の`neograph-harness-mcp`バイナリ。ソース ビルドは引き続き使用できます。
サンプル ターゲットと Python ホイールは、ライブラリ/ランタイム パッケージのままです。
リモート デーモンを暗黙的にインストールします。 MCPB および公式レジストリ出版物
リリース/ディスカバリー パッケージング オプションはそのままです。ワイヤーには必要ありません
プロトコルに追加する必要があり、署名されたリリース アーティファクトと明示的な
リモート認証展開マニフェスト。

NeoGraph は現在、日付付きの MCP `2025-11-25` プロファイルのみを公開しています。ファイナル
将来のステートレス プロトコルを記述する SEP は、新しい有線バージョンを作成しません。
MCP プロジェクトが新しいプロファイルを公開するまで、後継プロファイルは公開されません。
時代遅れの仕様。
