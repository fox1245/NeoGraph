<!-- neograph-i18n: source=examples/cookbook/openrouter-provider/README.md locale=ja source_sha256=2ae7c3fb71401e8c8db0b7c5a6116f56d8076f1c963b768aa55f98931801bc16 -->
# NeoGraph + OpenRouter

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

このクックブックは OpenRouter の Chat Completions API への二つのリクエスト経路を
維持します。両方とも既定モデルは `~deepseek/deepseek-v4-flash-latest` で、
`provider={"zdr": true}` を送りゼロデータ保持ルーティングを要求します。
この設定は地理的なデータ所在地の保証ではありません。

[`via_openai_compat.py`](via_openai_compat.py) はネイティブ `SchemaProvider` と
組み込みの `llm_call` ノードを使います。[`via_http.py`](via_http.py) は `httpx` で
HTTP リクエストと応答の変換を所有するカスタム Python `GraphNode` を使います。
エンドポイントと認証情報のヘルパーは共有しますが、通信実装と結果の表現は
共有しません。互換性の例に `httpx` は不要です。

## 経路 A: 型付き SchemaProvider

互換性の経路は、閉じたバージョン付き `openai.chat` ディスクリプターで
`load_provider_descriptor` を呼び出します。`connection.base_url` にはオリジン、
`connection.paths` には完全な API パスを指定します。OpenRouter ではそれぞれ
`https://openrouter.ai` と `/api/v1/chat/completions` です。不明なフィールドは
拒否され、任意の JSON でコーデックの動作を注入することはできません。

型付き `OpenRouterRouting` が `SchemaProviderDefaults` の `zdr=True` を
設定します。`ProviderRuntimeOptions` は API キー、120 秒のタイムアウト、
任意の `OPENROUTER_CA_FILE` を指定します。ランタイムは libcurl を使い、HTTP2
や WebSocket のトランスポート選択機能はありません。`NodeContext` は明示的な
モデルとシステム指示を指定します。`RunConfig.provider_messages` は `Text` 部分を
持つ型付き `ProviderMessage` を指定します。組み込みノードは型付き要求を作成し、
準備し、プロバイダー経由でディスパッチします。

ループバック検証では `provider_policy_json()` を読み、`openai.chat` ファミリーの
`openrouter_origins` に正確なループバックオリジンを追加します。ディスクリプターを
ロードする前に、ファミリーとコーデック資源の全データを `load_provider_policy` に
渡して承認します。検証サーバーのオリジンは宣言されたポリシーデータであり、
実行時のエンドポイント上書きや ZDR 検証の省略ではありません。

成功した結果は不変のプロバイダー結果と型付きメッセージ履歴を保持します。
`outcome.completion` は完了を保持し、失敗の場合は `outcome.failure` が失敗を
保持します。欠落した使用量カウンターは `None`、観測されたゼロは `value=0` の
`UsageCount` です。例は生のペイロードや鍵をダンプせず、アシスタントのテキストと
既知の入出力カウンターを表示します。プロバイダーの失敗を成功した回答にはしません。

## 経路 B: カスタム HTTP ノード

`OpenRouterHttpNode` はグラフのメッセージを読み、システム指示を先頭に追加して、
選択したエンドポイントに JSON を送り、`choices[0].message` を通常の
`ChannelWrite` に変換します。カスタム HTTP ヘッダー、タイムアウトポリシー、
応答の変換はアプリケーションコードが所有します。append リデューサーはユーザーと
その後のアシスタントメッセージを保持します。`http_usage` は応答の使用量辞書、
または省略時の `None` を保存します。実行後に `httpx` クライアントを閉じます。

この経路は `ProviderOutcome`、準備済み要求の権限、ネイティブ再生履歴、
型付きプロバイダー使用量を生成しません。HTTP エラーはノードを失敗させ、
ツール呼び出しを受けた場合もこのテキスト専用の例は呼び出しを捨てずに失敗します。
既存の公式 SDK クライアントに呼び出しとツールループを任せる場合は、
[BYO OpenAI SDK クックブック](../byo-openai/README.md) を使ってください。

## 前提条件とローカル実行

現在の型付きプロバイダーへの移行を含むチェックアウトからビルドした wheel を
インストールしてください。古い完了 API のリリースでは実行できません。経路 B 用に
`httpx` をインストールし、このディレクトリで以下のコマンドを実行する前にローカル
Chat Completions サーバーを起動してください。以下は検証手順であり、成功した
実行の記録ではありません。

```bash
python -m pip install httpx
OPENROUTER_BASE_URL=http://127.0.0.1:8765/v1 OPENROUTER_MODEL=fixture-model python via_openai_compat.py
OPENROUTER_BASE_URL=http://127.0.0.1:8765/v1 OPENROUTER_MODEL=fixture-model python via_http.py
```

`OPENROUTER_BASE_URL` は API プレフィックスを含み、両経路とも
`/chat/completions` を追加します。正規のループバックホスト `127.0.0.1` と `::1` は、
環境にホスト用の鍵があっても固定のダミー認証情報 `local-smoke` を使います。
他のホストには HTTPS、`NG_ALLOW_HOSTED_CALLS=1`、`OPENROUTER_API_KEY` が
必要です。明示的な許可がなければ、各プログラムは要求前に終了コード 2 で終了します。
ホストへの呼び出しは課金される場合があります。任意の既存 `.env` はエクスポート済み
変数を上書きしません。鍵をコミットしたり Authorization ヘッダーを記録したりしないで
ください。経路 A では OpenRouter 以外のホストについても、明示的なルーティング
ポリシー承認が必要です。呼び出し許可だけでは OpenRouter の意味を付与しません。

## ローカルプロトコルと期待する状態

サーバーはシステムメッセージ、ユーザーメッセージ、`model="fixture-model"`、
`provider={"zdr": true}` を含むバッファリング方式の `POST /v1/chat/completions` を
受け取ります。経路 B は `temperature=0.7` も送ります。経路 A はフランスの首都、
経路 B は `17 * 23` を質問します。経路 A 用のサーバー応答例:

```json
{
  "id": "fixture-chat-1",
  "object": "chat.completion",
  "created": 0,
  "model": "fixture-model",
  "choices": [{
    "index": 0,
    "message": {"role": "assistant", "content": "Paris."},
    "finish_reason": "stop"
  }],
  "usage": {"prompt_tokens": 8, "completion_tokens": 2, "total_tokens": 10}
}
```

経路 A では、実際の成功したプロバイダー結果が一つ、`result.provider_messages` に
ユーザーとアシスタント、グラフ状態にアシスタントの回答があることを期待します。
この応答の使用量は入力 8、出力 2 です。不明なカウンターを検証するには `usage` を
省略してください。架空のゼロを期待しないでください。経路 B ではアシスタントの
内容として `"391"` を返し、二件のグラフメッセージと応答の `http_usage` 辞書を
期待します。どちらも暗黙にモックプロバイダーへ切り替えません。

OpenRouter のリクエストと応答のリファレンス:
<https://openrouter.ai/docs/api-reference/overview>
