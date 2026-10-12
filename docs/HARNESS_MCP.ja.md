<!-- neograph-i18n: source=docs/HARNESS_MCP.md locale=ja source_sha256=b75490c4ee1f8c84e484fcbd9515de47eb62bef484adfebc5b1e4259daa0d911 -->
# NeoGraph Harness MCP

**Languages:** [English](HARNESS_MCP.md) | [한국어](HARNESS_MCP.ko.md) | [日本語](HARNESS_MCP.ja.md) | [简体中文](HARNESS_MCP.zh-CN.md)

NeoGraph Harnessは、複数のワーカーによる上限付きのワークフローを、実行する前にコンパイルします。安定したMCPのインターフェースは、次の6つのツールのままです。

- `neograph_schema` は、インストールされているリクエストの契約とプリセットを調べます。
- `neograph_compile` は、実行せずに、コンパイルと検証だけを行います。
- `neograph_start` は、保持されているアーティファクト、またはインラインのリクエストを開始します。
- `neograph_get` は、簡易ステータスをポーリングするか、結果アーティファクトのURIを参照します。
- `neograph_resume` は、保留中のホストの結果を、正確に検証して送信します。
- `neograph_cancel` は、キューに入っている、実行中の、または待機中のワークフローを、協調的にキャンセルします。

同梱されているプリセットは、`fanout_judge`、`pr_review_panel`、`bug_triage`、`research_synthesis` です。プリセットが生成するのは、通常のstrict-Coreのグラフアーティファクトです。JavaScriptのリクエストは、自身の `ProgramSource` エンベロープとソースマップを保持します。

### JavaScriptによるオーサリングの境界

`harness.mode` が、新しい公開で受け付けるのは、`preset` または `javascript` です。JavaScriptのリクエストは、`harness.source` にソーステキストを持ち、`harness.source_id` を固定できます。

```json
{
  "harness": {
    "mode": "javascript",
    "source_id": "review:main.js",
    "source": "export function define() { const g = ng.graph('main'); /* ... */ return g; }"
  }
}
```

トランスレーターは、そのテキストを、正規の `ProgramSource` JavaScriptエンベロープ（言語は `javascript`、QuickJSエンジン、固定（freeze）されたホストAPI、import、ソースマップ）で包み、`ProgramCompiler`、`ProgramCatalog`、`ProgramRuntime` に通します。`define()` は、シール済みの `ng` バインディングを通じて、1つのグラフを構築します。省略可能なジェネレーターの `main()` が、通常の制御フローを担当し、既存の型付きのProgramコマンドをyieldします。JavaScriptがCoreノードをディスパッチしたり、プロバイダーやツールを選んだり、admission、予算、ジャーナル、リプレイを迂回したりすることはありません。

評価したモジュールは、結果の契約も決めます。`define()` だけをエクスポートするソースは、`channels.final_result.value` のラッパーを含む、Coreのルートの契約を保ちます。実行時の `main(input)` をエクスポートするソースは、代わりに、最終的な戻り値を、Harnessの結果スキーマに対して直接、宣言し、検証します。

#### 制御フローのマイグレーション例

`define()` はコンパイル時だけで動かし、実行時の効果はすべて、yieldした型付きコマンドの背後に置きます。次の完全なリクエストは、ジェネレーターに3つの操作（`ng.all` のjoinと、その2つのCore呼び出し）と、2並列を与えます。ワーカーノードは、リクエストからシールされた設定をそのまま繰り返し、最終的な戻り値はHarnessの結果の形をしています。

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

安定したソース位置の文字列は、永続的なコマンド座標の一部です。リトライや再起動をまたいで、決定的になるようにしてください。必要な数の成功のうち最初に得られたものを採用したい場合は `ng.any(...)`、最初に終了したメンバーを採用したい場合は `ng.race(...)` を使います。どちらも、構造化された並行性によって、未完了の並列メンバーをキャンセルします。環境依存のI/O、タイマー、動的ロード、`eval`、ネイティブハンドルは、引き続き使えません。

`harness.mode` は、必ず明示します。`dsl` は `H_MIGRATION_CORE_DSL`、`core` は `H_MIGRATION_CORE_JSON`、`program` / `program_json` は `H_MIGRATION_PROGRAM_JSON` を返します。いずれも `/harness/mode` を指し、リクエストのJSONの形や、欠けているフィールドから選ばれることはありません。strict CoreのJSONは、検証済みのCoreとProgramのアーティファクトのための、内部用／交換用の表現として残ります。信頼できるC++によるプロセス内での構築も、引き続きサポートされます。どちらも、Harnessの公開されたオーサリング言語ではありません。

スキーマのエクスポート、コンパイル、開始は、同じ、変更できない `HarnessAdmissionProfile` を使うようになりました。そのスコープ付きの `GraphRegistry` とマニフェストには、利用できるすべてのノード、リデューサー、条件が、実装、lowering、互換性のメタデータとともに一覧されます。プロセス全体で共有するレジストリのエントリは、このパレットに含まれず、Harnessのadmissionでは解決できません。コンパイルは、検証済みの宣言的な `TopologySpec` で止まります。そのため、拒否された入力が `GraphNode` を構築することはなく、ワーカーも効果もディスパッチされません。保持されるアーティファクトは、プロファイルIDとフィンガープリントを結び付けています。異なるプロファイルのアーティファクトや、プロファイルより前のアーティファクトは、再解釈されずに、開始時や再開時にフェイルクローズになります。

C++の組み込み側は、デフォルト以外のプロファイルを、構築時に `HarnessServiceResources` を通して渡します。このリソースの境界は追加されたもので、既存の `HarnessServiceConfig` のレイアウトは変わりません。プロファイルのフィンガープリントは、マニフェストと、スコープ付きレジストリがエクスポートする意味の射影を対象にします。各 `implementation_identity` は、信頼される宣言であり、対応する呼び出し可能なものの挙動が変わるたびに、変更しなければなりません。

これが、現在のProgramを基盤にしたHarnessの互換アダプターです。受理されたHarnessのリクエストは、これまでどおり、従来の `ProgramSource` に変換され、`ProgramCompiler` でコンパイルされ、`ProgramCatalog` でadmissionされ、`ProgramRuntime` で実行されます。ノードを実行するのは、引き続き `GraphEngine` だけです。

Harnessのワークフローは、組み込みのQuickJS上の標準的なJavaScriptで書きます。従来の `dsl`、単独の `core`、`program` のモードは、マイグレーションの診断とともに拒否されます。strict CoreのJSONは、記述用の言語ではなく、内部用の交換データです。この文書では、残されている互換動作とその診断を説明します。

## ローカルの認証済みホストワーカー

`neograph-harness-mcp` は、ワーカーの推論を、明示的に選んだ、認証済みのローカルCLI 1つに委任できます。これは**モデルの委任**であり、認証情報の継承ではありません。NeoGraphが、ホストの認証ファイルを開いたり、OAuthトークンを受け取ったり、サブスクリプションをプロバイダーのAPIキーに変換したりすることはありません。公式のホストのプロセスは、自身が保存しているログイン情報を使います。プロセスグループによる隔離のために、Linuxが必要です。それ以外のプラットフォームでは、ホストのバックエンドはフェイルクローズになります。`auto` による選択はありません。認証済みのホストが複数あっても、課金やポリシーに関する判断が、黙って行われてはならないためです。

```bash
opencode auth login                 # or claude auth login / codex login, once
neograph-harness-mcp --executor opencode --host-status
neograph-harness-mcp --executor opencode --host-model openai/YOUR_MODEL
```

ほかのCLIを使うには、`--executor claude` または `--executor codex` を選びます。`--host-model` を省略すると、そのアダプターのホストのデフォルトが要求されます。環境変数でも指定でき、`NEOGRAPH_HARNESS_EXECUTOR` と `NEOGRAPH_HARNESS_HOST_MODEL` を使います。ステータスの表示と通常の起動では、最初に、時間と範囲を限定した、秘密情報を含まないバージョンとログインの事前チェックを行います。OpenCodeでは、明示したモデル名を `opencode models` で検証します。ClaudeとCodexには、同じ形式のポータブルなモデル一覧のAPIがないため、モデルの選択に関するエラーは、実際に試したリクエストから報告されます。CLIがない、ログアウトしている、出力が未対応、クォータ、ポリシー、モデルに関する失敗は、黙ってプロバイダー呼び出しに置き換えられることはありません。正確なモデル名、または `host default`、CLIのバージョン、エグゼキューターのID、モードは、認証情報を含めずに、ステータスとホストのメタデータに表示されます。

組み込み側や、将来の共有プロセスのMCP設定でも、`<neograph/mcp/harness_host_agent.h>` から同じ境界を構築できます。まず `preflight_host_agent(config)` を呼び、そのうえで `HarnessProgramHostConfig::worker_executor = make_host_agent_executor(config)` を設定します。`HostAgentExecutorConfig` のワークスペースは、明示的に指定する正規のルートです。秘密情報を含まないエグゼキューターとモデルのIDを `provider_host_configuration` に結び付けると、保持されたアーティファクトを別のルートで再開できなくなります。この境界は、直接の `Provider` エグゼキューターを変更せず、外向きのMCP Samplingも必要としません。

サブプロセスのプロファイルは、意図的に読み取り専用で、ローカルのstdioだけを対象にしています。OpenCodeは、新しく作る一時的な設定ディレクトリから `--pure` で実行し、read / glob / grep だけを許可します。`.env` と既知のホストの認証情報のパスは、モデルのreadツールに対して引き続き拒否され、ワークスペースのルートには、読み取り専用で外部パスにアクセスする権限が与えられます。Claudeは、Read / Glob / Grep だけを許可して `-p --safe-mode --permission-mode plan` で実行します。`--bare`（サブスクリプションのログインを省略する）は使いません。Codexは `exec --ignore-user-config --ignore-rules --ephemeral --sandbox read-only` で実行します。

すべてのサブプロセスには、次のものが与えられます。環境変数は、許可リストで必要なものだけに絞ります。直接のプロバイダーの認証情報と、無関係なリポジトリやクラウドの認証情報は、除外されます。ネストしたNeoGraphのホスト委任を禁止する深さのマーカーが付きます。プロンプト、イベント、stdout、stderrの取得量には上限があります。デッドラインもあります。キャンセルすると、プロセスグループが終了します。プロンプトやモデルIDを、シェルが評価することはありません。Programレベルのスキーマ検証と、上限付きのリトライは、引き続き適用されます。CLIが生成したJSONは、ワーカーのスキーマゲートに受理されるまで、信頼されません。使用量は、ホストの機械可読な完了イベントから集計します。

これらのCLIトランスポートには、生成時にトークン数を厳密に制限する共通の仕組みはありません。予算を超えた応答は拒否され、judgeに送られることはありませんが、上流ではすでに課金されている可能性があります。ワークスペースの読み取り専用の権限は、ホストのポリシーによる境界であり、OSのマウント名前空間ではありません。信頼できないリポジトリやホストのポリシーに対して、ファイルシステムを厳格に閉じ込める必要がある場合は、隔離したOSアカウントやコンテナーを使ってください。ローカルのCLIアダプターは、Harnessのcapabilityツールを受け付けません。capabilityの豊富なワーカーには、直接のProviderエグゼキューターを使ってください。

ライブテストは、信頼できる、認証済みのプライベートなランナーでだけ、明示的に有効にします。`NEOGRAPH_HARNESS_LIVE_HOST=claude|codex|opencode` で、インストールされているCLIを1つ選び、`NEOGRAPH_HARNESS_LIVE_MODEL` で、必要ならモデルを固定します。OpenCodeのOAuthのジョブでは、さらに `NEOGRAPH_HARNESS_LIVE_OPENCODE_OAUTH=1` と、利用できる `openai/...` モデルを設定します。テストは、実行する前に、`opencode auth list` がOpenAIのOAuthを報告する（秘密情報は含まない）ことを確認します。このマーカーはテスト用のゲートであり、認証情報ではありません。ログイン、モデル、ゲートのいずれかがない場合は、推論を捏造せずに、明示的にスキップします。

MCPサーバーは、各ホストの標準的なローカルstdioの設定で登録できます。PATH上にある、インストール済みの `neograph-harness-mcp` のバイナリに置き換えてください（MCPのエントリに認証情報を貼り付けないでください）。

```bash
claude mcp add neograph-harness -- neograph-harness-mcp --executor claude
codex mcp add neograph-harness -- neograph-harness-mcp --executor codex
```

OpenCodeでは、`opencode mcp add` を実行し、対話形式の案内で、コマンドが `neograph-harness-mcp --executor opencode` のローカルサーバーを選びます。同等の、サポートされている `opencode.json` のエントリは、次のとおりです。

```json
{"mcp":{"neograph-harness":{"type":"local","command":["neograph-harness-mcp","--executor","opencode"],"enabled":true}}}
```

[OpenCode CLI](https://opencode.ai/docs/cli/)、[Claude CLI](https://code.claude.com/docs/en/cli-reference)、[Claudeの認証](https://code.claude.com/docs/en/authentication)、[Codexの非対話モード](https://learn.chatgpt.com/docs/non-interactive-mode)、[Codexの認証](https://learn.chatgpt.com/docs/auth)を参照してください。委任された呼び出しには、引き続き、ホストのサブスクリプションのクォータ、レート制限、利用できるモデル、データ保持、組織のポリシー、データの扱いが適用されます。サードパーティの製品がClaude.aiのサブスクリプションを使う場合は、Anthropicの承認が必要になることがあります。このオプトインの、インストール済みCLIを使う方法は、ホスト型のClaudeサブスクリプションのバックエンドを再配布する承認では**ありません**。あるホストにログインできたからといって、別のベンダーやモデルにアクセスできるとは考えないでください。このローカルのバックエンドは、リモートのHTTP委任もMCP Samplingも有効にしません（ポータブルなホストの基準ではサポート外）。リモートやサーバーへのデプロイでは、直接のProviderの認証情報を使ってください。

### 直接のAPIプロバイダー（スタンドアロン／サーバー）


オプトインのローカルサーバーを、ビルドしてインストールします（ホストCLIのモード自体は、NeoGraph固有のモデルキーを必要としません）。

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

同梱のOpenRouterの例では、`NEOGRAPH_HARNESS_API_KEY` が `OPENROUTER_API_KEY` より優先されます。ローカルのホストでの実行とは異なり、直接のプロバイダーのモードには、専用のAPIキーが必要です。サーバーは、プロトコルのメッセージをstdoutだけに、診断情報をstderrだけに書き込みます。

ホストとの相互運用性のスモークテストに限り、`--executor provider` とともに `NEOGRAPH_HARNESS_SMOKE=1` を設定します。決定的なプロセス内プロバイダーが、指摘ゼロの有効なレビューを返すもので、APIキーは不要です。LLMの品質テストではありません。
### 認証情報を使わないOpenCodeのグローバルMCPの取り込み

単一ユーザーのローカル環境では、OpenCodeのユーザーグローバルな `opencode.json` から、選んだ、認証情報を使わないstdioサーバーを、`<neograph/mcp/adoption.h>` を通じて、明示的に取り込めます。検出は、調査だけを行います。読むのは、通常ファイルで、ユーザーが所有する、そのグローバルファイルだけです。プロジェクトやワークスペースの設定、OAuthのストアは読まず、サーバーも起動しません。HTTPのエントリ、無効なエントリ、インポートされた環境変数やファイルへの参照、未対応のフィールド、シェルのエグゼキューター、NeoGraph自身を再帰的に指すエントリは、拒否されます。

取り込みには、認証情報を含まない `argv` であるという明示的な証明、独立して承認された起動レコード、選択したツールとスキーマのcapabilityマニフェストが必要です。`pinned` のレコードは、ソースの内容、正規化したcwd、実行ファイルとインタープリターのID、argv、特定できるスクリプトやパッケージを結び付けます。検証できない起動の形式には、明示的な `trusted_mutable` の承認が必要で、黙って格下げされることはありません。検出したからといって、いま調べたバイト列を承認したことにはなりません。

各ツールのマニフェストは、`argument_policy: "exact-arguments-v1"` と、`argument_predicate: {"allowed_arguments": [<complete approved argument objects>]}` を使う必要があります。比較の対象は、スキーマの形だけでなく、リソースを含む引数の値です。汎用的な `read-only` や、SQL／パスというラベルは拒否されます。任意の入力が安全であることを証明しないためです。Harnessの権限は、ワーカーの宣言、取り込んだマニフェスト、静的ポリシー、プロセス境界の共通部分のままです。MCPのアノテーションで、それを広げることはできません。

ホストまたはプロバイダーのワーカーを構築する前に、`HardenedMcpClientRegistry::configure_harness(host_config, provider_config)` を呼び出してください。これにより、変更できない名前空間付きのツールメタデータと、承認済みのエグゼキューターが、まとめて設定されます。対応するリクエストメタデータは、`tool_catalog()` が提供します。承認の取り消しやスキーマの変化があると、保持されているエグゼキューターが遮断され、共有クライアントも明示的にシャットダウンされます。別の所有者がまだ参照を持っていても同じです。スキーマの更新とツールのディスパッチは、呼び出し側のデッドラインとキャンセルトークンに従います。

取り込んだクライアントは、実行ファイルの絶対パス、正規化したcwd、許可リストで置き換えた環境変数、上限付きのプロトコルフレームとstderr、プロセスツリー全体のシャットダウンを使います。Windowsでの起動では、継承するハンドルの許可リストと、クローズ時に終了させるJob Objectを使います。ステータスのレコードに含まれるのは、ハッシュと名前で、argv、生の設定、stderr、認証情報は含まれません。認証情報を伴うモード（`secret_injected`、`host_brokered`）、リモートのHTTP MCP、任意のシェルやCLIの実行は、引き続きサポートされません。

インストール可能なサンプルは、プロバイダーのワーカーエグゼキューターでの取り込みだけをサポートします。別途レビューされたJSONの承認ファイルを読み込み、自動で同意する環境変数のフラグは使いません。`NEOGRAPH_HARNESS_MCP_APPROVAL` に入れるのは、ファイルのパス（最大1 MiB）であり、内容ではありません。

```bash
export NEOGRAPH_HARNESS_MCP_SOURCE="$HOME/.config/opencode/opencode.json"
export NEOGRAPH_HARNESS_MCP_APPROVAL="$PWD/approved-mcp.json"
neograph-harness-mcp --executor provider
```

承認オブジェクトには、`McpLaunchApproval` と `McpToolApproval` に対応する、`launch` と `tools` のメンバーがあります。`launch` には、`trust_mode`、明示的な真偽値の `argv_no_credentials_attested`、`source_path`、`source_content_hash`、`cwd`、`executable`、`executable_identity`、`argv_hash`、`interpreter_identity`、`package_identity` が入ります。ホスト側のヘルパー `make_mcp_launch_approval()` が、レビュー用にこれらのIDを計算します。`tools` には、`server_name`、`launch_identity`（承認された起動の `executable_identity`）、`selected_tools`、`schema_hashes`、`manifest`、`policy_version` が入ります。スキーマの承認を記録するときは、正規化した `ToolDefinition::from_json(definition).to_json()` の値を、`mcp_tool_schema_hash()` でハッシュします。

ソースを明示的に上書きする場合も、ドキュメントに書かれたユーザーグローバルなファイルを指す必要があります。プロジェクトのパスを、グローバル設定に見せかけることはできません。ソースを設定した状態で `--host-status` を実行すると、認証情報のチェックの前に、検出だけを行った、秘密情報を伏せたレコードが出力されます。下流のサーバーが起動されることはありません。

永続的なhost-brokeredの呼び出しには、レコードとチェックポイントの両方の永続化が必要です。サンプルでは、明示的に指定した1つのディレクトリで、その両方を有効にします。

```bash
export NEOGRAPH_HARNESS_STATE_DIR="$PWD/.neograph-harness-state"
```

これにより、変更できないアーティファクト、変更可能な実行レコード、追記専用の因果ジャーナルが `runs.db` に、グラフのチェックポイントが `checkpoints.db` に保存されます。ジャーナルの行と、Harnessが作成するすべてのチェックポイントは、実行を、変更できないアーティファクト、コンパイル済みのリビジョンダイジェスト、MCPのプロトコルバージョン、Harnessのプロファイルに結び付けます。ワーカーの試行には、所要時間、検証とリトライの結果、プロバイダー、capability、host-brokeredの各呼び出しを、それを発行した試行に結び付ける相関IDが含まれます。2つのSQLiteストアは、どちらもWALモードと、上限付きのbusyタイムアウトを使います。バージョン1の既存のレコードデータベースは、開いたときに、トランザクション内でバージョン3に移行されます。このディレクトリは、サーバーを再起動しても残ります。どちらかのストアがない場合、`host_brokered` のカタログエントリはコンパイル時に拒否されます。そのため、ワークフローが、実際にはない再開可能性を宣言することはありません。

独自に組み込む場合も、オプションの `neograph::mcp_sqlite` ターゲットの `SqliteHarnessRecordStore` を通じて、同じバックエンドを構築できます。デフォルトのジャーナルモードは、SQLiteに渡る前に、一般的な秘密情報や内容のフィールドを、再帰的に `[REDACTED]` に置き換えます。`METADATA_ONLY` は、イベントのペイロードをすべて破棄します。`FULL` は、プロバイダーの内容、ツールの引数、結果をそのまま保存するので、保存が承認されたデータに対してだけ有効にしてください。イベントは、`HarnessJournal::list_events(run_id, after_sequence, limit)` で、実行の順に読み出せます。アトミックなJSONファイルを使いたい環境向けに、`FileHarnessRecordStore` も引き続き使えます。ただし、ジャーナルの境界は実装していません。

### 保持（retention）

SQLiteストアは、オプションの兄弟インターフェース `HarnessRetentionStore` を実装します。安定した `HarnessRecordStore` のvtableは変わりません。アーティファクトを保持する前、または実行を開始する前に、`HarnessService` は `HarnessServiceConfig` の `max_artifacts` と `max_runs` を適用します。デフォルトは、どちらも128です。

クリーンアップが削除するのは、終了済みで、他から参照されていない（leaf）実行だけです。キューに入っている実行、実行中の実行、入力待ちの実行は保護されます。ジャーナルの終了処理が済んでいないプロセス内の実行も同様です。リプレイまたはフォークの行は `source_run_id` を記録するので、その行が保持されている間は、ソースを削除できません。空きが必要な場合は、依存している側のleafが先に削除されます。ソースが削除の対象になるのは、保持されている行が誰も参照しなくなった、後のステップです。そのため、候補がすべて、アクティブか、明示的に保護されているか、まだ参照されている場合、上限は厳密には守られません。

`runs.db` の中では、1つのトランザクションが、実行のジャーナルの行を、その実行の行より先に削除します。アーティファクトを削除するのは、どの実行からも参照されなくなってからです。そのコミットの後、Harnessは、削除した実行のチェックポイントスレッドを、別に設定されたチェックポイントストアから削除します。この2段階目でクラッシュしたり、チェックポイントのバックエンドが失敗したりすると、到達できないチェックポイントの領域が残ることがあります。ただし、保持されているリプレイやフォークが、削除済みのソースのレコードを指したままになることはありません。そうしたチェックポイントだけの残骸は、後の管理操作や、バックエンド固有の孤立データの掃除で回収される場合があります。

`FileHarnessRecordStore` は、永続的なクリーンアップを実装していません。従来の、メモリ上のアーティファクトキャッシュの追い出しと、実行数の厳格な上限の動作は、そのままです。

## デバッガービュー

`neograph_get` は、簡易な `status` をデフォルトのまま保ち、MCPツールを増やさずに、4つのデバッガービューを追加します。

| ビュー | 結果 |
|---|---|
| `attempts` | ジャーナルに記録された、ワーカーの試行の開始・完了・中断のイベント |
| `trace` | 既存の、順序付きのGraphEngineのノードトレースと、因果ジャーナルのタイムライン |
| `checkpoints` | ペイロードを含まないチェックポイントのメタデータ（ID、親、ノード、フェーズ、ステップ、チャネル名） |
| `diff` | 各チェックポイントと、その親との間で変化したチャネルの値とバージョン |

`attempts` と `trace` は、前方向の不透明なカーソルとして `after_sequence` を受け付けます。4つのビューすべてが、1〜1000の `limit` を受け付けます。返されるアーティファクトのURIは、同じページングをクエリとして持てます。たとえば、次のようになります。

```text
neograph://runs/run_123/attempts?after_sequence=17&limit=50
```

これらのURIで受け付けるのは、`after_sequence` と `limit` だけです。未知の、または形式が不正なクエリのフィールドは、無視されずに失敗します。ジャーナルを基にしたビューは、保存されたとおりのペイロードを返すので、設定した伏せ字処理のモードはそのまま保たれます。`diff` ビューはジャーナルではなくチェックポイントストアから計算され、チャネルの値をそのまま含むことがあります。このビューへのアクセスは、既存の詳細な実行結果へのアクセスと同じように扱ってください。

## リプレイのモード

`neograph_start` は、MCPツールを増やさずに、完了した実行をリプレイできます。

```json
{"replay":{"source_run_id":"run_123","mode":"recorded"}}
```

`recorded` は、コンパイラーによって固定されたグラフを、コピー元のジャーナルにある完了済みのワーカー試行の結果を使って、再実行します。設定されているワーカー、プロバイダー、MCP、A2A、capabilityのエグゼキューターは、一切呼び出しません。コピー元のアーティファクトのリビジョン、プロトコル、プロファイルは、引き続き一致している必要があり、ジャーナルは `FULL` のペイロードモードでなければなりません。`REDACTED` と `METADATA_ONLY` のジャーナルは、ワーカーの正確な出力がないため、リプレイできません。取得済みの操作座標、元のオーナー／バージョン／入力の権限、永続的なプロバイダーバンクの保管が、正確に必要です。互換性のない古い保管は拒否され、トークン合計や試行番号から再構築されることはありません。

同じ保持済みのアーティファクトを、実際のプロバイダーとツールで実行するには、`mode: "live"` を使います。スナップショットとジャーナルのライフサイクルイベントは、実行を `recorded_replay` または `live_replay` とラベル付けし、`source_run_id` を含めます。通常の開始は、引き続き `live` です。

recordedのリプレイは、コピー元の、変更できない元の呼び出し権限を、残っている支出権限とは別に認証します。コピー元の系統に対する1回のCASで、残りが保持されたままリプレイの実行に移されます。2回目のリプレイが、同じコピー元の残りを再び使うことはできません。取得済みの呼び出しは、過去の操作座標を再利用し、操作スロットは補充されません。リプレイで新たに発生するCoreと実時間の処理は、移された残りを消費します。

プロバイダーのレポート、確定している課金、不確かな予約、効果のIDは、観測専用の別のバンクに、ちょうど1回だけ展開（hydrate）されます。再生の最中に、予約、返金、精算、レポートの追加、新しい効果のディスパッチを行うことはできません。失われたリプレイの実行は、永続的なリースによって遮断され、移された、または不確かなクレジットを保持します。実際のディスパッチで修復されることはありません。実行のスナップショットには、現在の系統の残りが示されます。クレジットがリプレイに移ったコピー元も同様です。

`ChatMessage` / `ChatTool` とJSONは、持ち運び用の射影（portable projection）であり、ネイティブな権限（native authority）を持ちません。持ち運び用のフォーマットは、これまでどおり[`provider-message-v2`](../schemas/provider-message-v2.schema.json)と[`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json)です。本物のC++のチェックポイントのサイドカーは、メモリ上でネイティブシール（native seal）を保持します。ネイティブな履歴を永続化するには、ホストが所有する `sp::NativeArchive` が必要です。これは閉じたv3形式（`spna3`）で、認証付きのオーナー専用の保管（custody）と、独立した鍵を使います。archive v2は拒否します。アップグレードも解釈もしません。認証が結び付けるのは、セマンティックなディスクリプターの選択内容すべて（origin、パス、ヘッダー、ポリシー、リクエストのフィールドマッピング、usageのパス、stopのマッピング）と、オーナー、正確な保管の対応付けです。暗号化ではなく、ベンダー（発行元）の認証でもありません。archiveの本体、鍵、ネイティブなblob、生のワイヤー観測データは、公開しないでください。archiveはエビデンスの保管場所であり、金銭の付与（grant）でも、支出のリース（spending lease）でもありません。Programと外部のバンクは、これまでどおり独立したジャーナルが所有します。スナップショットをコピーしても、クレジットは作れません。

**単独バンクのジャーナルの訂正：現在の契約を改訂しました。実際に動かして得た実行時のエビデンスは後述します。** オーナーが承認したプロトコルでは、次の2点が必要です。1つは、単調増加する、信頼されたストアの名前空間に対する義務（namespace obligation）です。もう1つは、実在する不変の元のオーナー／スレッド／グラフのスコープ、上限（ceiling）、デッドライン／クロックの識別子、世代（generation）です。ホストが所有する不透明なリース（opaque lease）を発行できるのは、チェックポイント全体のコミットメントとリビジョンに対する、永続的なヘッドの正確なCASだけです。保留中のエフェクトのウィンドウは、プロバイダーI/Oの前に、正確に永続化しなければなりません。精算には、本物のSDKの結果と、実際の課金額、nullを許すレポート、ホールド、重複排除用のIDを使います。チェックポイントと次のヘッドは、同じ所有アクター／リビジョンのもとでアトミックに公開します。バンクのメタデータを削除する、チェックポイントを間引く、認証済みの古いスナップショットをリプレイする、同じIDを上書きする、アクターを失う、といった操作でクレジットが付与されてはいけません。上限を130から129に絞ったとき、すでに65のホールドがあると、さらに65を受け入れることはできません。エフェクトがなかったと証明された失敗であれば、変更のないヘッドを解放できるため、本物の130での復旧は引き続き可能です。クラッシュ、不明、リース喪失のウィンドウは、返金も再試行もフォールバックもせず、ホールドしたままにします。plain／pristineなarchive設定は、金銭もネイティブな支出リースも与えません。現在の `config.usage` で、既存の単独の義務を置き換えることもできません。Programや外部バンクのジャーナルの所有関係は変わりません。以上は要求される契約です。実際の通貨と保管のエビデンス、および計測上の制約は後述します。安定版として公開済みのAPIを保証するものではありません。

**現在の宣言。統合した実行時のエビデンスは後述します。** `<neograph/graph/checkpoint.h>` は、`owner_scope`、論理的な `thread_id`、バックエンド内部用の `storage_thread_id`、`graph_identity`、`original_ceiling`、`original_deadline_ticks`、`deadline_clock_identity` を持つ `ManagedBudgetLeaseScope` を宣言しています。`OwnedManagedBudgetLease` が公開するのは、読み取り専用の `scope()`、`actor_id()`、不変の `bank_generation()`、`revision()`、`head_checkpoint_id()`、`head_commitment()` です。権限をインポートする公開コンストラクターはありません。`ManagedBudgetEffectReceipt` は `active()`、`effect_id()`、`claim_amount()`、`request_digest()` を公開し、デフォルトのレシートは何の権限も与えません。`CheckpointStore` は、`acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`、`begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`、`settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`、`publish_managed_budget_checkpoint(lease, checkpoint)`、`release_managed_budget_lease(lease)` と、それぞれの `_async` 版を宣言しています。同期版の `CheckpointStoreCore` と `AsyncCheckpointStore` は、それぞれの版を公開します。`managed_budget_checkpoint_commitment(checkpoint)` が対象とするのは、バンクのJSONだけでなく、永続化されるチェックポイント全体です。これらの宣言だけでは、バックエンドのCAS、通貨の安全性、インストール済みABIの互換性、実行時の経路が実際に成功することは、いずれも確認できません。

**本物のInMemoryの共有バンクフォーク：維持しており、動作も確認済みです。** 元からある本物のC++のフォークは、元の1つの会計ジャーナルと、信頼された現在のブランチヘッドを使います。グラントを複製することはありません。`publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)`（と `_async` 版）には、本物の現在のコピー元とそのコミットメント全体、および同じバンクのネイティブC++ポインターの実体が必要です。永続的な単独フォークは、引き続き明示的に未対応です。`OwnedManagedBudgetLease::scope()` と、元のオーナー／スレッド／グラフ、上限、デッドライン／クロック、世代は不変のままです。ストアが発行する読み取り専用の `execution_thread_id()` / `execution_storage_thread_id()` は、実行するブランチを別に選びます。`GraphState::budget_original_thread_id()` は、元の会計バンクを示します。選択したブランチのヘッドに対する正確なCASと、グローバルなアクター／リビジョンにより、標準の現在のカウンター、保留中のエフェクト、使用済み（burned）のIDに対して、すべてのブランチが直列化されます。元のブランチもフォークしたブランチも、補充なしで使い続けられます。古いスナップショット、チェックポイントのコピー、インポートしたJSONは、エイリアスを作ることも、ヘッドを巻き戻すこともできません。元のroot30 → charge3 → original continuation6 → fork lower20 → continuation9という同一バンクの証明は、変更していない test_graph_engine.cpp:810–913 でPASSしました。保存された元の上限30は、フォーク側の有効な上限20とは別です。31への拡大と、JSONだけでの復元は、拒否されなければなりません。上限のない報告済みの観測値は事実を示すデータであり、有限のグラントではありません。変更のないヘッドを解放できるのは、エフェクトがゼロだと証明されたリースだけです。不明または保留中のエフェクトがあると、義務は保持されます。

**現在のrelease-errorの契約：実際に動かしたスイートとプローブは後述します。** `<neograph/graph/engine.h>` の `graph::ManagedBudgetLeaseReleaseError` は、`ProviderOutcomeError` から派生します。`cause()` は元の実行時例外を保持し、`release_error()` は二次的な、永続的なリース処理の失敗を返します。`outcome()` は、本物のSDKのエビデンスがあれば保持し、SDKの結果がなければnullです。releaseに失敗しても、結果を作り上げたり、再dispatchを許可したりすることはありません。閉じたメタデータ `_neograph_managed_budget_scope` には、元の論理スコープ／上限／デッドラインのクロック／世代が書かれていますが、これはデータであり、バックエンドのCASによる権限ではありません。

**archiveのオーナーと保持（retention）の契約：実際に動かしたスイートとプローブは後述します。** 省略された元のオーナーを、実際に設定されている `sp::NativeArchive::owner_scope()` から引き継ぐのは、有限の単独ルート、または認証済みの有限のコピー元だけです。上限のない場合とplainな場合のオーナーメタデータの意味は変わりません。archiveのオーナーが明示的に食い違っている場合は、リースを取得する前に拒否します。`CheckpointStore::retains_native_checkpoint() const noexcept` と、それに対応するCore／Asyncのストレージ機能は、デフォルトでfalseです。実際のInMemoryバックエンドはtrueにオーバーライドし、ラッパーは実際の保持状況を委譲しなければなりません。この読み取り専用の記述により、正当な、リースなし／plain／上限なしのC++のネイティブチェックポイントの保管が許されます。ただし、支出クレジットもネイティブリプレイの権限も与えません。リース付きの保管には、JSONのフラグや推測したストア型ではなく、ストアが実際に発行したレシートを使います。

**ネイティブ保管のI/O前ゲート：実際に動かしたスイートとプローブは後述します。** 管理対象のエフェクトを開始するには、保留中のエフェクト、スロット、ホールドしたウィンドウを変更する前に、本物の結び付けを持つNativeArchive、または実際のローカルでストアが発行したプライベートなC++の保持機能（capability）が必要です。このプライベートな機能は、JSONからインポートされることも、ワイヤー上で転送されることもありません。C++のサイドカーは境界を越えられないため、リモートのバックエンドがInMemoryであっても、gRPCにはクライアントとサーバー双方の実際のarchiveが必要です。archiveが有限のコピー元オーナーを提供しない場合、元の匿名のオーナースコープは空のままです。実際のarchiveの結び付けは、元のスコープと一致しなければなりません。財務上のヘッドとリースのエビデンスだけでは、ネイティブ保管の準備ができているとは証明できません。

`ProviderOutcomeError` は、結果を保持するホストエラーの共通の基底です。`ProviderObserverError` と `ProviderDispatchOutcomePersistenceError` は、drain済みのSDKの結果の全体と、元の `cause()` を保持します。永続化のエラーは、二次的なオブザーバーの失敗も `delivery_error()` に保持します。`ProviderFailure::outcome()` は、SDKの失敗そのものを保持します。これらはエビデンスであり、NodeやProgramが再ディスパッチしてよいという許可ではありません。トランスポートのリトライは、SDKだけが担当します。呼び出し側が選んだ `max_output_tokens` が、黙って切り詰められることもありません。上限を引き上げたセマンティックな呼び出しには、新しい準備済みダイジェスト、別の決定的な呼び出し序数、元のリソースバンクと元のデッドラインからのadmissionが必要です。ネイティブリプレイの資格があっても、クレジットが更新されることはありません。

`ProgramFailure` は、生きている `provider_outcome` と `provider_cause` を保持します。正規の事実としてのSDKwitness（canonical factual SDK witness）は、本物のarchiveの保管を、オーナー／run／バージョン／バンドル／オペレーション／試行に結び付けます。Runtimeは、復旧した失敗を公開する前に、設定済みの保管を先に復元します。公開されているデータのみの `ProgramResult::create()` は、あらかじめ入力したwitnessでこれを迂回することはできません。未解決のパース済みシールは、実行可能な結果ではありません。プロセスの再起動後は、元の例外ポインターが存在せず（`provider_cause == nullptr`）、テキストから再作成することもありません。永続化できない失敗は、シリアライズも公開もリプレイもできません。

`RecordedBindingSet` は、ソースに結び付けられた、ムーブ専用のデータです。呼び出し側が渡すdispatcherではありません。信頼されたCatalogの `recorded_capability_binder` が、実際に永続化されたソースイベントから、取得済み（captured）の機能だけを独立して実体化します。`ProgramRuntime::replay_recorded()` は、元の選択済みソースの権限を確認したうえで、実際に残っているバンクを永続的なCASで移します。引き継いだ支出は、新しいモデルのグラントではありません。以前の `start_recorded` による更新APIは削除しました。InMemory、File、SQLite、PostgreSQLの各Programストアは、実行の間じゅう、正確で不変な、所有されたリースを保持します。期限が切れても更新されません。制御されたJavaScriptも、基になる機能マニフェストを検証し、正確な完了済みコマンドの結果を消費します。外部のエフェクトを再dispatchすることはありません。

**Recorded-controlの因果的な修正：フルスイートで動作確認済み。** capturedしたコマンドのリプレイは、実行前に、新しいCPUの実時間（wall-time）とCoreの処理だけを、永続的に予約します。そして、測定した処理量と、新しく生成されたCoreチェックポイントを、結果のCASで公開します。新しいモデル、金銭、Programオペレーションの許容量は消費せず、capturedした外部エフェクトを再dispatchすることもありません。精算されていない予約は、引き落とされたままです。予約により、通常のRunning→Runningの遷移ではなく、認証済みの精算遷移が選ばれます。通常の遷移では、最初の新しいCoreチェックポイントが拒否されていました。awaitのチャネル受信、タイマーの待機／キャンセル、ハンドオフ待機の開始と解放は、それぞれを所有するexecutorとstrand上で直列化されます。既存のRecorded CPU／Memoryのawait／handoffシナリオは、フルスイートで通りました。リモートのTSanの対象範囲の限界は、後述のとおり明記します。

以下の有料の観測、ネイティブ軸の観測、統合ランタイムの観測は、過去のプロバイダー切り替えのコホートです。件数、失敗、スキップ、制約は、変更していません。残されている記録の中の「最新」は、そのコホートの最後の実行を指します。現在のPythonバインディングや、このドキュメントの変更を検証したものではありません。

**有料の観測は完了しています。これは、あらゆる条件での適格性確認（qualification）ではありません。** 元の `SPQUAL1` の基本枠630/1000000 microUSDは変わりません。ハッシュで連結された1つの `A` が、承認済みの拡張枠480/3000000を、同じ元の台帳の中に受け入れています。合計は1110/4000000で、呼び出し回数、支出、ホールド、精算は累積されており、新しいグラントID、ヘッダー、リセットはありません。宣言の正確なバイト列とファイルの同一性、および元の認可、ベースライン、カタログ、アクティベーション、台帳プレフィックスのハッシュと合計値は、固定されたままです。削除、置換、変更を行うと、フェイルクローズになります。最終的な正規の台帳は、呼び出し1110回、支出437958、ホールド1287828（いずれもmicroUSD）、イベントA1、上限1110/4000000です。支出とホールドの合計US$1.725786は、ローカルのカタログ上のメーターの値であり、請求書の金額ではありません。文書化済みの5ファミリー×60ペアのベースラインは、600リクエストを完了しました。内訳は、Chat 60/60、Responses 60/60、Messages 60/60、Generate 56/60（不正解のvision SSEが4件）、Interactions 57/60（不正解のvisionが、バッファ型で1件、SSEで2件）です。合計は293/300ペアで、300/300ではありません。ほかの旧600件の会計記録は保存してありますが、挙動を完全に証明するものではありません。以前のM5／メディアのワンショットのコホートは変わりません。以前の3ラウンドのGoogleの前提条件には、無効なツール2件と、読み取れないpositive 1件の失敗が残っています。これ以上の有料呼び出しは承認されていません。最終的なSDKのエビデンスとネイティブ軸の制約は、ベースラインの成功とは別のものです。以前のアクティベーションと再オープンのスモークテストは、2回再オープンした後の呼び出し610回、支出219159、ホールド751233として記録されています。SDKのメーター／カナリア／visionの4テストは、19.38秒で通過しました。これらは範囲を限定した以前のチェックポイントであり、最終的な台帳の合計ではありません。以前に検証したChat 60ペアのコホートは、実際の試行120件、UpperBoundの課金120件を保持しており、UnknownHoldはありません。

**ネイティブ軸の観測は、暗号学的な検証でも、ネイティブでの消費や同等性の確認でもありません。** Generateは、改変、欠落、重複を受け入れました。Interactionsは、隔離した本物のコピー元／positive control、1つのオーナーの署名の改変、thoughtキャリアの欠落、callキャリアの欠落、重複を受け入れました。thoughtと署名をすべて削除すると、汎用の400が返りました。THOUGHT項目を残したまま署名フィールドをすべて削除した場合も、汎用の400でした。最後のキャプチャには、ローカルでエンコードした元データを保持するコントロールがありましたが、同じキャプチャでのサーバー側のpositiveではありません。以前のpositiveのコホートは、本物のままです。これらの観測が確立するのは、キャリアがまとめて欠けた場合の境界だけです。発行元や署名の検証、ベンダー側での消費は確立しません。実際のレポートは、SDKの `config/qualification-extension-results.json`、`qualification-final-summary.json`、`qualification-native-axis-results.json`、`qualification-combined-omission-results.json`、`qualification-signature-presence-results.json` です。前提条件の失敗、未実行、ネガティブで結論が出ない、といった状態は、事実のまま残しています。thoughtだけ、またはキャリアだけを欠落させた場合は、別のキャリアが残っている限り受け入れられました。これは、発行元の検証やネイティブでの消費についての主張を強めるものではありません。

**実際の統合的な検証結果と、残る制約。** 直近のCoreのフル実行は、2242テスト、失敗0件、スキップ16件（RAMでのプロセス喪失のケース14件は対象外、実際の認証情報が必要なゲート2件）で、130.17秒でした。`PgNestedJsonRoundTrips` は、重複キー、順序、nullのメタデータ、blob、残余（residual）を正確に保ったまま、0.18秒で通りました。変更のない元の共有バンクフォークと、既存のRecorded CPU／Memoryのawait／handoffシナリオも通りました。ラップした実際のMemory／SQLite／PostgreSQL／gRPCに対する、finite130／hold65／lower129／strip／old-head／pruning／no-archive／importのプローブは、通常ビルドとASan+UBSanの両方で通りました。ローカルのMemory／SQLite／PostgreSQLに対するTSanの範囲は、7件が通り、警告は0件でした。システムのAbseil／Protobufを含む、gRPC混在のフルTSanは、終了コード66で終わり、依存ライブラリと生成されたRPCのスタックに、レース警告が402件出ました。これは計装とカバレッジの限界であり、誤検知だと証明されたわけではありません。リモートのTSanによる検証や、レースがないことは主張せず、警告の抑制もしていません。インストール済みのfind_packageを使う、Program C++／C ABI／dualQuickJSの3つのコンシューマーは通りました。新たにインストールしたNeoGraph／SchemaProviderの型付きコンシューマーは、次の項目を通りました。実際のHTTPリクエスト2件、コルーチン開始前のプロバイダー破棄、ネイティブ／ツールのリプレイ、拒否（refusal）、known-zeroと生データの保持、実際のLinkedMismatchの拒否です。ブラウザーでのAlice/Bobの分離と、世代2への置き換えは、目視で確認しました。PostgreSQLのProgram Chatのブラックボックステスト6件は、18.989秒で通りました。直近のSDKは26/26が通り、失敗0件、74.07秒でした。最終的なReleaseGraphは、16構成×新規プロセス3回の繰り返し（48レコード）を38.29秒で完了し、失敗0件、実際のプロトコルとowned-outcomeのチェックがすべて通りました。NeoGraphの `benchmarks/provider-cutover-final-results.json` と `benchmarks/provider-cutover-final-summary.json` に、この最終コホートを個別に保存しています。測定中にコンパイラーや有料モデルは実行していません。過去のコホートは変更しておらず、セマンティクスやリソース使用量が同等だとは主張しません。不安定なSDK／ABI3は、安定版リリースでも、より広いプラットフォームでの適格性確認でもありません。


## 互換性のあるフォーク

まず、修正したHarnessをコンパイルします。そのうえで、既存の `neograph_start` ツールを使って、正確に指定した過去のチェックポイントから、そのターゲットのアーティファクトに分岐します。

```json
{
  "fork": {
    "source_run_id": "run_123",
    "checkpoint_id": "550e8400-e29b-41d4-a716-446655440000",
    "artifact_id": "artifact_repaired"
  }
}
```

コピー元のチェックポイントは、`source_run_id` のものでなければなりません。実行を割り当てる前に、Harnessは、ターゲットのアーティファクトに対して、チェックポイントのスキーマ、コピー元のリビジョン、MCPのプロトコル、Harnessのプロファイル、復元されるすべてのチャネルとリデューサー、継続するすべてのノード、アクティブなバリアのインターフェースを検証します。互換性のない分岐では、`started: false`、`status: "incompatible_fork"`、`path` と `witness` を持つ、機械可読な `H_FORK_*` の診断が返ります。実行もフォークのチェックポイントも作成されません。

チェックポイントストアが必要です。レコードストアがない場合、フォークが参照できるのは、現在のサービスのプロセスにまだ残っているコピー元の実行とアーティファクトだけです。再起動後も残るフォークの系統が必要な場合は、両方のストアを設定してください。

互換性のある分岐には `compatible_fork` というラベルが付き、開始のレスポンス、スナップショット、ライフサイクルのジャーナルイベントに、`source_run_id` と `source_checkpoint_id` の両方が含まれます。実行は、選択したチェックポイントの `next_nodes` から再開します。すでにコミットされている前段のノードは、再実行されません。修正後のトポロジー、ワーカーの契約、ツールカタログは、ターゲットのアーティファクトから取得します。復元されるチャネルの値（元のタスクのチャネルを含む）は、コピー元のチェックポイントから取得します。タスクの入力そのものを変える必要がある場合は、フォークではなく、新しく開始してください。

コピー元の実行、アーティファクト、選択したチェックポイントは、フォークが参照するものです。互換性のチェックやフォークの実行がそれらを使いうる間は、保持されたままでなければなりません。リテンションのクリーンアップは、依存している側を先に削除するか、参照されているコピー元を残す必要があります。事前チェックから分岐の作成までの間に、コピー元のチェックポイントを削除してはいけません。

## Streamable HTTP

リモートトランスポートはオプトインです。そのため、stdioだけの既存のターゲットは小さいままで、HTTPやOpenSSLへの依存が黙って増えることはありません。

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

エンドポイントは `http://127.0.0.1:8080/mcp` です。公開されているMCP 2025-11-25のStreamable HTTPのPOSTの契約を、セッションごとのMCPライフサイクルとJSONレスポンスで実装しています。通知にはHTTP 202を返します。クライアントがキャンセルした呼び出しは、空のイベントストリームで終わるので、レスポンスは届きません。セッションは、DELETEで終わるか、30分間リクエストがないと（`session_idle_timeout`）終わります。新しいクライアントのために `max_sessions`（64）の空きが必要になった場合は、リクエストが進行中でないセッションのうち、最も長く使われていないものが終わります。そのIDには404が返り、クライアントはこれによって、あらためて初期化が必要だと分かります。オプションの、単独のGET/SSEチャネルは、意図的に実装されておらず、HTTP 405を返します。これは、トランスポートの仕様が明示的に認めています。

セキュリティのデフォルトはトランスポートのレベルにあり、認証を `GraphEngine` や `HarnessService` に結び付けません。

- デフォルトのバインド先は `127.0.0.1` です。ループバック以外へのバインドは、bearerのオーソライザーを設定しない限り、拒否されます。
- 指定された `Origin` は、`NEOGRAPH_HARNESS_ALLOWED_ORIGINS`（実行ファイルではカンマ区切り）のエントリと完全に一致しない限り、すべて拒否されます。
- `NEOGRAPH_HARNESS_BEARER_TOKEN` を設定すると、実行ファイルの、単一のプリンシパルによるbearerの境界が有効になります。ライブラリとして組み込む場合は、`MCPHttpServerConfig::bearer_authorizer` でOAuth/JWTを検証し、安定したプリンシパルとスコープを返せます。
- セッションは、返された認可スコープに結び付けられます。別の有効なプリンシパルが、漏えいした `Mcp-Session-Id` を再利用することはできません。
- `MCPHttpServer` のファクトリーは、その検証済みのスコープを受け取り、`MCPHttpServerSession` のオーナーを返します。マルチテナントで組み込む場合は、このスコープを使って、隔離されたHarnessのレコードストアとチェックポイントストアを選ぶ必要があります。認証の状態が、グラフランタイム自体に入ることはありません。オーソライザーがない場合（ループバックへのバインドのみ）、スコープは空で、実行ファイルは単一のローカルのオーナースコープを保ちます。
- リクエストのペイロード、HTTPワーカー、キュー、セッション、レスポンス待ちの上限は、`MCPHttpServerConfig` で制限されます。HTTPスレッドは、必要になったときに `max_http_threads`（64）まで起動されます。ツール呼び出しが、戻るまで1つを占有するためです。
- bearerトークンをOAuthの認可サーバーから受け取る組み込みでは、`MCPHttpServerConfig::protected_resource_metadata` を設定します。サーバーはRFC 9728のメタデータを公開し、401のチャレンジのたびに、その場所を示します。

`neograph::mcp::ScopedHarnessStore` は、公開IDと、スキーマが所有するレコードやジャーナルへの参照を、元に戻せるテナントの名前空間にマッピングします。不透明なリクエスト、結果、イベントのペイロードは、書き換えられません。FileストアとSQLiteストアは、これらの非公開IDを受け付けます。長すぎるFileのキーには、固定長のハッシュ値のファイル名が使われ、短いキーのパスは変わりません。SQLiteのリテンションは、1つのトランザクションの中で、件数と削除の候補の両方を、選択した名前空間に限定します。他のテナントと、保護されたコピー元の参照は残ります。このストレージの境界は、認証されたスコープや、アプリケーションのプロバイダー／ツール／クォータのポリシーの代わりにはなりません。

ループバック以外へのデプロイでは、信頼できるリバースプロキシでTLSを終端し、そのOAuth/OIDCの検証か、同等の `bearer_authorizer` を使ってください。元の `Authorization` ヘッダーと `Origin` ヘッダーは転送してください。平文の公開リスナーを公開してはいけません。Harnessの状態ディレクトリ1つにつき、認可ドメインは1つにしてください。

## ホストの設定

`SERVER` には、インストール済みの `neograph-harness-mcp` のバイナリの絶対パスを指定します。これらのエントリは、直接のProviderエグゼキューターを選びます。上のローカルホストのモードを使う場合は、代わりに、対応する `--executor claude|codex|opencode` を渡してください。

```bash
SERVER=/absolute/path/to/neograph-harness-mcp
```

Claude Code（ローカルのプロジェクトスコープ）：

```bash
claude mcp add --scope local --transport stdio neograph-harness -- "$SERVER" --executor provider
claude mcp get neograph-harness
```

Codex CLI：

```bash
codex mcp add neograph-harness -- "$SERVER" --executor provider
codex mcp list
```

信頼できるこのローカルサーバーに対して、対話なしで `codex exec` を使う場合は、Codexの `config.toml` に `mcp_servers.neograph-harness.default_tools_approval_mode = "approve"` を設定します。設定しないと、アーティファクトを保持する操作が読み取り専用として注釈されていないため、Codexは `neograph_compile` を、正しくキャンセルします。対話セッションでは、デフォルトのプロンプトのままでかまいません。

OpenCode（プロジェクトの `opencode.json` またはユーザー設定）：

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

`opencode mcp list` で確認します。これらの形式は、2026-07-21の時点で確認した、各ホストの公式のMCP設定の仕様に従っています。

## PRレビューのワークフロー

ホストに、通常のリポジトリ用ツールでPRのdiffを集めさせ、そのうえでHarnessのツールを使います。適切なリクエストの例は、次のとおりです。

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

`budgets.provider_timeout_seconds` は、準備済みのプロバイダー操作1回の絶対的なデッドラインを、1〜600秒に設定します。`budgets.max_output_tokens` は、その出力の上限を、1〜128000トークンに設定します。どちらも省略可能です。省略すると、その項目は未設定のままになり、admission済みのSDKのデフォルトポリシーが保たれます。時間や出力が無制限になるわけではありません。プロバイダーのリトライは、準備済みの操作のデッドラインの範囲内で、SDKが担当します。

ワーカーは、どちらのフィールドも、より小さい値に設定できます。Harness全体の値を超えるワーカーの値は、コンパイル時に拒否されます。デッドラインに達すると、Harnessは、そのプロバイダー呼び出しに渡した子のキャンセルトークンだけをキャンセルします。並列のワーカーや、それを含む実行はキャンセルしません。プロバイダーはトークンに従う必要があるため、中断できないプロバイダーは、デッドラインの後に戻ることがあります。

ホストは、次の順序で進めてください。

1. `neograph_compile` を呼び、`ok` がfalseなら中止します。
2. 返された `artifact_id` を指定して、`neograph_start` を呼びます。
3. `run_id` を指定して `neograph_get` をポーリングします。返されるのは、結果と件数だけです。
4. 詳細が必要な場合は、同じ `run_id` と、返された `neograph://runs/...` のURIを `uri` に指定して、
   `neograph_get` を呼びます。トレースは、デフォルトではコンテキストに取り込まないでください。

### 指摘の出どころ

詳細のアーティファクトは、スキーマ検証済みの各ワーカーの応答を `workers` に保持し、既存のクライアント向けに、これまでのフラットな `findings` 配列も保ちます。`finding_sources` は、同じ長さを持つ並列の配列です。各エントリには、集約後の `finding_index`、出どころの `worker_id`、そのワーカー内の `local_index` が入ります。`F1` のように、ローカルIDが重複している場合は、これで出どころを特定してください。ワーカーが宣言する指摘のオブジェクトに、出どころのフィールドを追加してはいけません。

## host-brokeredの再開

ワーカーのプロセスではなく、MCPホストがcapabilityを所有する場合は、`executor.kind: "host_brokered"` を使います。`executor.interaction` は、`"tool_result"`（デフォルト）または `"input"` に設定します。プロバイダーのエグゼキューターは、要求された引数を検証し、終了していない次の2つの実行状態のどちらかを返します。

- `awaiting_tool_results`：ホストが、指定されたcapabilityを実行する必要があります。
- `input_required`：ホストが、入力値を収集する必要があります。

`neograph_get` の結果には、一意の `call_id`、`tool_id`、検証済みの `arguments`、`result_schema` を持つ `pending` オブジェクトが含まれます。まさにその呼び出しを、次のように送信します。

```json
{
  "run_id": "run_...",
  "call_id": "hcall_...",
  "result": {"answer": "validated host result"}
}
```

`neograph_resume` は、呼び出しIDの不一致、宣言したスキーマに違反する結果、期限切れの呼び出し、待機中でない実行への遅れた結果を拒否します。完全に同一の重複は、グラフを再実行せずに受理されます。内容が食い違う重複は拒否されます。受理された再開の意図は、実行がスケジュールされる前に永続化されます。そのため、プロセスがクラッシュした後でポーリングすると、`NodeInterrupt` のチェックポイントから再開がやり直されます。成功済みの並列ワーカーが繰り返されることはありません。

### 外部への効果と照合

通常のhost-brokeredの契約は、後方互換です。`executor.effect` のないカタログエントリは、プロセスを再起動した後も `awaiting_tool_results` のままで、同じ `{run_id, call_id, result}` の再開リクエストを受け付けます。

外部から見える、冪等でない変更を行いうるホストのcapabilityでは、そのリスクを明示的に宣言します。効果のメタデータが有効なのは、デフォルトの `host_brokered` の `tool_result` のインタラクションに対してだけです。入力収集用のメタデータではありません。

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

このとき、保留中の呼び出しには、永続的な `effect` オブジェクトが含まれます。その `effect_id` と `idempotency_key` は、Harnessの実行をスコープとし、プロバイダーのツール呼び出しIDとは異なります。`status_query` と `fencing` は、ホストのcapabilityを記述するものです。Harnessはこれらを記録しますが、プロバイダー固有の問い合わせやリトライのプロトコルを作り出すことはありません。

`idempotency: "unsupported"` の呼び出しが待機中のままサービスが再接続すると、その実行だけが `ambiguous_effect` に変わります。これは、プロセスが止まる前にホストが効果を実行していた可能性があるものの、Harnessがどちらの結果も証明できないことを意味します。簡易ステータスには `pending` と `ambiguity` が含まれ、ジャーナルには `host_brokered.effect.ambiguous` が記録されます。Harnessは、ツールをリプレイしませんし、効果を失敗とも完了とも報告しません。

ホストが、自分の側の正式なシステムで確認した後で、`neograph_resume` を通じてこの曖昧さを解消します。

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"completed","result":{"answer":"validated host result"}}
```

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"failed"}
```

```json
{"run_id":"run_...","call_id":"hcall_...","resolution":"unknown"}
```

`completed` は、`result` を検証して消費し、チェックポイントから再開します。`failed` は、ワーカーを再実行せずに、Harnessの終了状態の失敗を記録します。`unknown` は、後の照合のために、実行を `ambiguous_effect` のままにします。完全に同一の重複した completed、failed、unknown の送信は冪等です。内容が食い違う completed や failed の送信は、拒否されます。重複でない照合は、すべて `host_brokered.effect.reconciled` としてジャーナルに記録されます。

曖昧な効果は、意図的に、キャンセルできず、期限切れにもなりません。キャンセルやタイムアウトでは、外部への効果が起きたかどうかを確定できないためです。正式なシステムでまだ解決できない場合は、`unknown` を送信してください。

このプロトコルは、ホストがクラッシュしても配送が1回だけ行われること（exactly-once）を主張しません。冪等性キーや状態の問い合わせをサポートするホストは、照合を送信する前に、それらを使って実際の結果を確認してください。

実行のスナップショットには、`created_at`、`updated_at`、`expires_at`、`poll_after_ms` が含まれます。デフォルトのTTLは24時間、デフォルトのポーリング間隔は1秒です。組み込み側は、どちらも `HarnessServiceConfig` で上書きできます。

## 実験的なTasksプロファイル

実装されていません。このガイドの以前のバージョンには、オプトインのMCP Tasksプロファイルが書かれていましたが、そのようなプロファイルは存在しません。Harnessサーバーは、tasksのcapabilityを宣言しません。`HarnessServiceConfig::enable_experimental_tasks` と `NEOGRAPH_HARNESS_EXPERIMENTAL_TASKS` には効果がなく、`tasks/get`、`tasks/update`、`tasks/cancel` は `-32601` を返します。長時間の実行には、安定した `run_id` と `neograph_get` のポーリングという契約を使います。

## Capabilityバックエンド

`make_provider_harness_executor` は、任意のNeoGraph `Provider` を通じてワーカーを駆動します。モデルが宣言済みのツールを要求した場合、エグゼキューターは、ディスパッチの前後に、その引数と出力をカタログに照らして検証します。

プロバイダーのエグゼキューターは、型付きの `ProviderRequest` を組み立てます。`prepare` は、I/Oなしで検証とエンコードを行い、`dispatch` は準備済みのリクエストを消費します。`invoke` は、これらをまとめて行います。エグゼキューターは、所有しているSDKのCompletionまたはFailureの結果を、順序付きのメッセージ／パートや部分的な失敗も含めて保持します。失敗を、テキストだけの成功に変えることはありません。プロバイダーのレポートでは、不明な使用量はnull許容のフィールドとして保持されます。保守的に見積もったトークンの課金や予約は、別の権限です。記録されたプロバイダーの再生では、それらの結果を再び課金したり精算したりすることはありません。リプレイで新たに発生するCPUやCoreの処理は、移された残りを引き続き消費します。

初期化済みの下流の `MCPClient` インスタンスには `make_mcp_harness_capability_executor` を、A2Aエージェントには `a2a::make_harness_capability_executor` を使います。権限を決めるのは、引き続きリクエストです。ワーカーに見えるのは、その `tools` 配列に挙げられたツールIDだけです。

ファイルシステムのツールでは、パスを持つすべての入力を `path_arguments` に宣言し、`policy.workspace_roots` を設定します。相対パスは、最初のルートの下で解決されます。設定されたどのルートの外にもある正規のパスは、ディスパッチの前に拒否されます。既存のシンボリックリンクを経由した逃げ道も同様です。capabilityのバックエンドに渡されるのは、モデルが指定した表記ではなく、正規化されたパスです。下流のMCPやA2Aのサービスは、別の信頼境界のままです。ファイルシステムのtime-of-check/time-of-useの競合を防ぐために、それらのサービスも、同じルートのポリシーを強制してください。`policy.read_only: true` の場合、`read_only: true` が付いていないカタログエントリがあると、コンパイルは拒否されます。

## 配布とプロトコルのプロファイル

サポートされているローカルの配布方法は、上で説明したインストール可能な `neograph-harness-mcp` のバイナリです。ソースからのビルドでは、引き続きサンプルのターゲットを使えます。Pythonのwheelは、引き続きライブラリとランタイムのパッケージであり、リモートのデーモンを暗黙にインストールすることはありません。MCPBと公式レジストリへの公開は、リリースと発見のためのパッケージングの選択肢のままです。ワイヤープロトコルには必須ではなく、署名付きのリリース成果物と、リモート認証を明示したデプロイのマニフェストがある場合にだけ追加してください。

NeoGraphが公開しているのは、日付付きのMCP `2025-11-25` のプロファイルです。`2025-06-18`、`2025-03-26`、`2024-11-05` を要求するクライアントには、そのリビジョンが定義するフィールドで応答します。`2025-03-26` と `2024-11-05` のクライアントからのJSON-RPCバッチも受け付けます。MCPの現行のリビジョン `2026-07-28` はステートレスです（`initialize` もセッションもなく、バージョンはすべてのリクエストの `_meta` に入ります）。これはまだ提供していません。公式の両世代対応クライアントは `2025-11-25` にフォールバックしますが、`2026-07-28` に固定したクライアントは接続できません。`2025-11-25` と並ぶ2つ目のモードとして提供する予定です。
