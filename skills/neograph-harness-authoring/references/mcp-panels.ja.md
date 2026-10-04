<!-- neograph-i18n: source=skills/neograph-harness-authoring/references/mcp-panels.md locale=ja source_sha256=ce9166dfce50a6fe8b400d1e21c0f1eba65d4af1e7f990ee9a5d4b03a7f68e8a -->
# MCP パネルの手順

**Languages:** [English](mcp-panels.md) | [한국어](mcp-panels.ko.md) | [日本語](mcp-panels.ja.md) | [简体中文](mcp-panels.zh-CN.md)

1. `neograph_schema` を呼び出し、このビルドが返したプリセットとフィールドだけを使います。
2. 正確な目的、受け入れ基準、上限を定めた予算、各ワーカーの JSON 出力スキーマを含む
   一つのリクエストを作成します。
3. レビュー作業では `pr_review_panel` を使い、`policy.read_only` を true に設定し、
   `policy.evidence_required` に、すべての指摘スキーマで必須とする証拠フィールドを設定します。
4. 各ワーカーには必要なツール ID だけを渡します。読み取り専用ツールを明示し、パスを持つ
   文字列引数を `path_arguments` に列挙します。そのような引数がある場合は、
   必ず `policy.workspace_roots` を明示的に設定します。
5. `neograph_compile` を呼び出します。`ok` が false の場合、`phase`、`path`、`source` に
   基づいて診断を修正します。拒否されたリクエストで start を呼び出してはいけません。
6. 保持した `artifact_id` を指定して `neograph_start` を呼び出します。
7. `run_id` を指定して `neograph_get` をポーリングします。ステータスが `awaiting_tool_results`
   または `input_required` なら、返された `pending` 呼び出しだけを処理し、同じ `run_id`、
   正確な `call_id`、`result_schema` に適合する結果で `neograph_resume` を呼び出します。
   同一内容の重複は受領済みとして扱い、別の呼び出し ID に置き換えてはいけません。
8. ステータスが終端になるまでポーリングを続けます。簡潔な結果をメインコンテキストに保持します。
9. 最終回答にワーカーの詳細や実行トレースが必要な場合に限り、返された `neograph://runs/...`
   URI を、その `run_id` を指定した `neograph_get` で参照します。
10. 部分的な結果、指摘ゼロ、タイムアウト、キャンセル、期限切れ、最大ステップ到達、失敗の
    各結果を正確に報告します。一括して成功に置き換えてはいけません。

## アンチパターン

- インラインリクエストでも `neograph_compile` を省略してはいけません。
- すべてのワーカーに広範なツールカタログを付けてはいけません。
- ワークスペースルートなしで、パスを持つツールを設定してはいけません。
- 不正または空のワーカー出力を、空の指摘リストとして扱ってはいけません。
- 簡潔な結果から必要だと分かる前に、詳細なトレースを取得してはいけません。
- 読み取り専用レビューに書き込み可能なツールを追加してはいけません。
- 呼び出し ID が消費された後に、データを変更してホストへの結果送信を再試行してはいけません。
- MCP Tasks がコアプロトコルでサポートされていると仮定してはいけません。サーバーと個別の
  リクエストが実験的な `io.modelcontextprotocol/tasks` 拡張を明示的に有効にしている場合を除き、
  安定版の `neograph_get` ポーリングを使います。

## 例

PR レビューでは、ホストのリポジトリツールで差分を取得し、タスクの目的に含めます。
正確性とセキュリティについてそれぞれ異なる指示を与えた、二つのワーカーを使います。
各指摘で `file`、`line`、`evidence` を必須にしてください。完全なリクエストとホストの
セットアップコマンドは、リポジトリの [HARNESS_MCP.md](../../../docs/HARNESS_MCP.md) を参照してください。
