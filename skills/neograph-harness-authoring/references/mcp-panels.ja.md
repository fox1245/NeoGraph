<!-- neograph-i18n: source=skills/neograph-harness-authoring/references/mcp-panels.md locale=ja source_sha256=ce9166dfce50a6fe8b400d1e21c0f1eba65d4af1e7f990ee9a5d4b03a7f68e8a -->
# MCPパネルの手順

**Languages:** [English](mcp-panels.md) | [한국어](mcp-panels.ko.md) | [日本語](mcp-panels.ja.md) | [简体中文](mcp-panels.zh-CN.md)

1. `neograph_schema` を呼び出します。このビルドが返したプリセットとフィールドだけを使います。
2. 次の内容を含むリクエストを1つ作ります。明確な目的、受け入れ基準、上限を決めた予算、各ワーカーのJSON出力スキーマです。
3. レビュー作業では `pr_review_panel` を使います。`policy.read_only` をtrueにし、`policy.evidence_required` には、すべての指摘のスキーマで必須にするエビデンスのフィールドを設定します。
4. 各ワーカーには、必要なツールIDだけを渡します。読み取り専用のツールにはその旨を示し、パスを受け取る文字列引数は `path_arguments` に列挙します。そのような引数がある場合は、必ず `policy.workspace_roots` を明示的に設定します。
5. `neograph_compile` を呼び出します。`ok` がfalseなら、`phase`、`path`、`source` を手がかりに、診断された問題を修正します。拒否されたリクエストで、startを呼び出してはいけません。
6. 保持しておいた `artifact_id` を指定して、`neograph_start` を呼び出します。
7. `run_id` を指定して `neograph_get` をポーリングします。ステータスが `awaiting_tool_results` または `input_required` の場合は、返された `pending` の呼び出しにだけ応答します。そのあと、同じ `run_id`、正確な `call_id`、`result_schema` に合う結果を指定して、`neograph_resume` を呼び出します。まったく同じ内容の重複は、受領済みとして扱います。別の呼び出しIDを代わりに使ってはいけません。
8. ステータスが終了状態になるまで、ポーリングを続けます。簡潔な結果は、メインのコンテキストに保持しておきます。
9. 返された `neograph://runs/...` のURIは、最終的な回答にワーカーの詳細や実行トレースが必要なときにだけ、その `run_id` を指定した `neograph_get` で参照します。
10. 部分的な結果、指摘ゼロ、タイムアウト、キャンセル、期限切れ、最大ステップへの到達、失敗は、そのとおりに報告します。ひとまとめに「成功」として扱ってはいけません。

## アンチパターン

- インラインのリクエストでも、`neograph_compile` を省略してはいけません。
- すべてのワーカーに、幅広いツールカタログを付けてはいけません。
- ワークスペースのルートを指定せずに、パスを受け取るツールを設定してはいけません。
- 形式が不正なワーカーの出力や、空の出力を、指摘が空のリストとして扱ってはいけません。
- 簡潔な結果から必要だと分かる前に、詳細なトレースを取得してはいけません。
- 読み取り専用のレビューに、書き込みできるツールを追加してはいけません。
- 呼び出しIDが消費されたあとに、データを変えてホストの結果を再送してはいけません。
- MCP Tasksがコアプロトコルでサポートされていると考えてはいけません。サーバーと個々のリクエストの両方が、実験的な `io.modelcontextprotocol/tasks` 拡張を明示的に有効にしている場合を除いて、安定版の `neograph_get` のポーリングを使います。

## 例

PRのレビューでは、ホストのリポジトリツールで差分を取得し、タスクの目的に入れます。ワーカーは2つ使い、それぞれに、正確性とセキュリティという別々の指示を与えます。各指摘には、`file`、`line`、`evidence` を必須にします。完全なリクエストと、ホストのセットアップのコマンドは、リポジトリの[HARNESS_MCP.md](../../../docs/HARNESS_MCP.md)を参照してください。
