<!-- neograph-i18n: source=docs/README.md locale=ja source_sha256=fe0e366626160489a53106532e2564d08cf01b64769c541064053306586d8196 -->
# NeoGraphドキュメント

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

このページは、このディレクトリにあるドキュメントと、リポジトリ内の主な README を一覧にし、それぞれ誰が読むとよいかを示します。初めての方は、最初の表を上から順に読んでください。クラスや関数を探しているときは、リファレンスを見てください。最後の2つの表は Program の契約の詳細と計測資料です。より深く知りたいときに読むもので、使い始めるのに必要なものではありません。

## はじめに読むもの

| ドキュメント | 読む目的 |
|---|---|
| [README](../README.md) | NeoGraph とは何か、API キーなしで動く Python と C++ のクイックスタート、ビルドオプション。 |
| [Concepts](concepts.md) | チャネル、リデューサー、ノード、エッジ、`Send`、`Command`、チェックポイント、インタラプト、ストリーミング。グラフを組み立てる順に説明します。 |
| [C++ サンプル](../examples/README.md)と [Python サンプル](../bindings/python/examples/README.md) | そのまま実行できるプログラム。多くは API キー不要で、必要なものは各一覧に記載されています。 |
| [トラブルシューティング](troubleshooting.md) | ビルド、インストール、実行時、Python のエラーと、それぞれの原因と対処。 |

## ガイド

| ドキュメント | こんなときに読む |
|---|---|
| [Python バインディング](python-binding.md) | Python から NeoGraph を使う。グラフ、型付き provider リクエスト、Program、Hook、チェックポイント、プロトコルクライアント。 |
| [Program ガイド](program-guide.md) | グラフを並列に実行し、失敗したノードを処理し、人の承認を待って再開し、予算内で動く、制限付き JavaScript プログラムを書く。 |
| [Program グラフ機能](program-graphs.md) | Program の `define()` グラフで表現できることを調べる。メタデータ、`any_of` ジョイン、ノードごとのリトライ、失敗エッジと失敗終了、試行ごとのタイムアウト、健全性チェック。 |
| [診断](diagnostics.md) | コンパイラやランタイムの診断コード(`P_JS_*`、`P_CORE_*`、`P_RUNTIME_*`)と対処を調べる。 |
| [MCP](mcp.md) | グラフを MCP ツールとして公開する、またはグラフから MCP サーバーのツールを呼び出す。 |
| [A2A](a2a.md) | グラフを Agent2Agent のエージェントとして公開する、またはリモートの A2A エージェントを呼び出す。 |
| [ACP](acp.md) | エディターが stdio で起動できる Agent Client Protocol エージェントとしてグラフを動かす。 |
| [Harness MCP](HARNESS_MCP.md) | Harness を使う。制限付きの複数ワーカーのワークフローをコンパイルして実行し、リプレイとレビューに対応する6つの MCP ツール。 |
| [非同期ガイド](ASYNC_GUIDE.md)と[並行実行](concurrency.md) | エグゼキューターを選ぶ、1つのエンジンを複数のセッションで共有する、非同期の provider とツールを書く、実行をキャンセルする。 |
| [ランタイムコンテキストと厳密なインターポジション](STRICT_RUNTIME_INTERPOSITION.md) | 必須コンテキスト、Hook、provider ディスパッチのレシートを、モデルの裁量に任せず強制する。 |
| [移行ガイド](migration-v0.4-to-v1.0.md) | 旧ノード API(`execute` とその派生)で書いたコードを `run(NodeInput)` へ、`CompletionParams` と `complete` で書いたコードを型付き provider リクエストへ移す。 |
| [Valgrind とサニタイザー](VALGRIND.md) | NeoGraph のメモリ検査とサニタイザー検査を実行する。 |

## リファレンス

| ドキュメント | こんなときに読む |
|---|---|
| [C++ リファレンス](reference-en.md) | 型、関数、失敗コードを調べる。MCP、gRPC、A2A、ACP のプロトコル失敗マッピング表もここにあります。 |
| [生成された API リファレンス](doxygen-mainpage.md) | `include/neograph/` の公開ヘッダーを Doxygen のメインページから見ていく。 |
| [ABI ポリシー](ABI_POLICY.md) | NeoGraph のアップグレードで自分のコードの再ビルドが必要になる場合を知る。 |
| [スキーマ](../schemas) | NeoGraph がやり取りする JSON を検証する。Core 定義、Hook、コンテキストアーティファクト、レシート。 |
| [変更履歴](../CHANGELOG.md)と[セキュリティポリシー](../SECURITY.md) | 何が変わったかを見る、脆弱性を報告する。 |

## Program の契約

Program 層の各部分を詳しく説明するドキュメントです。ランタイムを拡張するときや、正確な契約が必要なときに読んでください。使うだけなら読む必要はありません。

| ドキュメント | 主題 |
|---|---|
| [Program のケイパビリティコンテキスト](PROGRAM_CAPABILITY_CONTEXT.md) | ノードに、承認済みの設定が宣言したケイパビリティだけを見せる方法。 |
| [再帰的な Harness](PROGRAM_RECURSIVE_HARNESSES.md) | 親、子、孫のエージェントツリー。各エージェントが独自の Program バージョンと予算を持ちます。 |
| 子の合成: [契約](PROGRAM_CHILD_SYNTHESIS_CONTRACT.md)、[永続化](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md)、[復旧](PROGRAM_CHILD_SYNTHESIS_RECOVERY.md) | Program が生成した子のソースの承認をホストに求める方法と、再起動後もその状態が保たれる仕組み。 |
| [DSL ケイパビリティ評価](DSL_CAPABILITY_EVAL.md) | モデルが正しい Program を書けるかを測るテスト。 |

## 計測資料

| ドキュメント | 主題 |
|---|---|
| [性能の詳細](performance-deep-dive.md)と[ベンチマーク](../benchmarks/README.md) | エンジンのオーバーヘッドと並行実行の計測値、各実行の構成。保証する値ではありません。 |
| [Program コスト計測](PROGRAM_COST_MEASUREMENT.md) | Program の帳簿処理とストアのコストを測る方法。 |
