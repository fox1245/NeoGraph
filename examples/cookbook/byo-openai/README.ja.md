<!-- neograph-i18n: source=examples/cookbook/byo-openai/README.md locale=ja source_sha256=acf353dedaa0142260f857bf89a32b1c5cbee7fb7c2b31df5fb67fa7ef131dd3 -->
# 自分の OpenAI クライアントを使う

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

既存の `openai.OpenAI()` クライアントを NeoGraph のカスタム `GraphNode` 内で
使います。公式 SDK が HTTP クライアント、再試行ポリシー、ヘッダー、SDK レベルの
計測を維持します。NeoGraph はノードをスケジュールし、`ChannelWrite` の結果を
グラフの状態に適用します。

[`hybrid.py`](hybrid.py) は SDK を一度呼び出し、アシスタントの返信を追加します。
[`hybrid_with_tools.py`](hybrid_with_tools.py) は一つのノード内で SDK のツール呼び出し
ループを実行し、三つの Python 関数を呼び出して最終返信を追加します。
両例は OpenRouter を使い、既定モデルは `~deepseek/deepseek-v4-flash-latest` です。
`provider={"zdr": true}` を送り、OpenRouter のゼロデータ保持ルーティングを
要求します。この設定は地理的なデータ所在地ポリシーを保証しません。

## 前提条件とローカル実行

現在の型付きプロバイダーへの移行を含むチェックアウトからビルドした wheel と
`openai` パッケージをインストールしてください。削除済みの完了 API を持つ古い
リリースでは実行できません。このディレクトリで以下のコマンドを実行する前に、
ローカル Chat Completions プロトコルサーバーを起動してください。以下は検証手順で
あり、成功した実行の記録ではありません。

```bash
python -m pip install openai
OPENROUTER_BASE_URL=http://127.0.0.1:8765/v1 OPENROUTER_MODEL=fixture-model python hybrid.py
OPENROUTER_BASE_URL=http://127.0.0.1:8765/v1 OPENROUTER_MODEL=fixture-model python hybrid_with_tools.py
```

`OPENROUTER_BASE_URL` は API プレフィックスを含みます。ローカルサーバーでは
`/v1`、OpenRouter では `/api/v1` です。SDK が `/chat/completions` を追加します。
正規のループバックホスト `127.0.0.1` と `::1` では、環境にホスト用の鍵があっても
固定のダミー認証情報 `local-smoke` を使います。他のホストには HTTPS、
`NG_ALLOW_HOSTED_CALLS=1`、`OPENROUTER_API_KEY` が必要です。明示的な許可が
なければ、リクエスト前に終了コード 2 で終了します。ホストへの呼び出しは課金される
場合があります。既存の `.env` を読み込めますが、エクスポート済みの変数は
上書きしません。鍵をコミットしたり Authorization ヘッダーを記録したりしないでください。

## グラフ状態と SDK リクエスト

各グラフには `START_NODE` と `END_NODE` の間にカスタムノードが一つあります。
スコープ付き `GraphRegistry` にノード型を登録します。グローバルなプロバイダー
サブクラスや完了トランポリンは使いません。ノードは `messages` チャネルを読み、
SDK リクエストの先頭にシステム指示を加えてチャネル書き込みを返します。append
リデューサーは入力ユーザーメッセージと、その後のアシスタントメッセージを保持します。
システムメッセージはリクエスト内だけに残ります。

`hybrid.py` のサーバーは `model="fixture-model"`、システムメッセージ、ユーザー
メッセージ、`temperature=0.7`、`provider={"zdr": true}` を含むバッファリング方式の
`POST /v1/chat/completions` を一度受け取ります。`id`、
`object="chat.completion"`、`created`、`model` と、`index=0`、アシスタント
メッセージ、`finish_reason="stop"` を持つ一件の `choices` を含む標準 Chat
Completion JSON を返してください。`usage` は省略できます。期待する状態には二つの
メッセージがあり、`sdk_usage` は SDK の使用量辞書または `None` です。欠落した
カウンターを 0 にしません。ツール呼び出しを受けた場合、このテキスト専用ノードは
呼び出しを捨てずに失敗します。

## ツールループ

`hybrid_with_tools.py` の最初のリクエストには関数ツール `reverse_string`、
`word_count`、`calc` も宣言されます。決定的なサーバーは、それぞれ異なる id と
JSON 引数文字列 `{"s":"NeoGraph"}`、`{"text":"the quick brown fox"}`、
`{"expr":"17*23+5"}` を持つ三つのアシスタントツール呼び出しを返せます。
`finish_reason="tool_calls"` を設定してください。

ノードは SDK 内部の履歴にアシスタントのツール呼び出しメッセージを追加し、各関数を
実行し、一致する `tool_call_id` とともに結果を追加します。二回目のリクエストは
`hparGoeN`、`4`、`396` の結果を含む必要があります。ツール呼び出しを含まない
`finish_reason="stop"` のアシスタントテキストを返してください。期待するグラフ状態は、
元のユーザーと最終アシスタントのメッセージだけを含み、`tool_calls=3`、
`sdk_usage` は二件のリストになります。各項目は対応する返信の使用量辞書または
`None` です。最終呼び出しの使用量はループ全体の使用量ではありません。この交換では
プログラムが三回のツール実行と二回の SDK 呼び出しを表示します。

ツールの例外は、モデルが応答できるようツール結果のエラーテキストになります。
ツール呼び出し応答が八回連続すると上限に達し、架空の最終回答を書かずにエラーを
送出します。`calc` はこの算術デモ用に Python 式を評価します。信頼できない式を
実行するためのサンドボックスではありません。

## プロバイダー証拠の境界

これらのノードはアプリケーション所有の JSON 状態を書き込みます。
`ProviderOutcome`、ネイティブ再生権限、プロバイダー受領記録、ツールごとの
チェックポイントは生成しません。SDK の再試行と途中のツール呼び出しはノード内に
留まるため、グラフのチェックポイントはそれらの要求の永続的な受領記録には
なりません。実行後にクライアントを閉じます。

NeoGraph の型付きプロバイダー結果とネイティブ SDK 通信が必要な場合は、
[OpenRouter SchemaProvider の例](../openrouter-provider/README.md) を使ってください。
既存の SDK クライアントをこれらのカスタムノードに渡すとクライアント設定を保持しますが、
そのクライアントが `SchemaProvider` になるわけではありません。
