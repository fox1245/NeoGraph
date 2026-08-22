<!-- neograph-i18n: source=examples/cookbook/openrouter-provider/README.md locale=ja source_sha256=4d18b0fee54089948ca59063eb6177567e1b5db09a87123125e808bee6889add -->
# NeoGraph + OpenRouter（ピン留めされた DeepSeek）

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

OpenRouter のピン留めされた `~deepseek/deepseek-v4-flash-latest` モデルに対して NeoGraph グラフを実行します。このクックブックは、同じ API キー、モデル、および明示的なゼロデータ保持（ZDR）プロバイダー設定を持つ、等価な2つのプロバイダーサーフェスを示します:

1. 組み込みの `OpenAIProvider` をOpenRouterのOpenAI互換チャットエンドポイント（`via_openai_compat.py`）に対して使用します。
2. 正規化されたチャットリクエストを直接投稿するカスタムPython `Provider`（`via_http.py`）。


2番目のパスは、NeoGraphの`Provider`トランポリンがトランスポート非依存である一方、ワイヤー通信の契約を明示的に保つことを示しています。

## Path A — 組み込みプロバイダー

`OpenAIProvider` は `/v1/chat/completions` 自体を追加するため、ベアのOpenRouter APIベースURL（`https://openrouter.ai/api`）を渡してください。`/v1` サフィックスは渡さないでください:

```python
from neograph_engine.llm import OpenAIProvider

provider = OpenAIProvider(
    api_key=os.environ["OPENROUTER_API_KEY"],
    base_url="https://openrouter.ai/api",
    default_model="~deepseek/deepseek-v4-flash-latest",
    provider_routing={"zdr": True},
```

[`via_openai_compat.py`](via_openai_compat.py) を参照してください。

## Path B — カスタム直接 HTTP プロバイダー

[`via_http.py`](via_http.py) は `Provider` をサブクラス化し、`https://openrouter.ai/api/v1/chat/completions` に投稿し、`choices[0].message` を `ChatCompletion` にマッピングし、トークン使用量を NeoGraph のレスポンスにコピーします。また、リクエストは OpenRouter に ZDR プロバイダーの優先設定も問い合わせます。

## 実行

```bash
echo 'OPENROUTER_API_KEY=sk-or-...' > .env
pip install neograph-engine>=0.2.3 httpx
python via_openai_compat.py
python via_http.py
```

両方のデモは、厳格な単一ノードグラフを構築し、その`llm_call`を同じ固定されたDeepSeekモデル経由でルーティングします。共有の`NodeContext.instructions`がシステムプロンプトを供給します。キーが欠落している場合は、明確なメッセージとともに終了します。モックプロバイダーがライブプロバイダーのパスを静かに変更することはありません。

## 出力形状

```text
[openrouter] using ~deepseek/deepseek-v4-flash-latest via OpenAI-compatible path
[user] What's the capital of France?
  assistant: Paris is the capital of France.
```

実際の完了は異なる。直接HTTPパスは`via direct HTTP`を出力し、独自の算術問題を使用する。

## なぜ両方のパスを維持するのか？

- パスAはNeoGraphのネイティブHTTP実装と接続プーリングを使用する。
- Path Bは、アプリケーションコードが所有するカスタムヘッダー、転送ポリシー、または応答変換のための最小の例である。
- 両方のパスは、まったく同じOpenRouterエンドポイントファミリー、APIキー、および固定されたDeepSeekモデルを使用するため、プロバイダーサーフェスの比較は有意義です。

OpenRouter APIのリクエストおよびレスポンスの形状:
<https://openrouter.ai/docs/api-reference/overview>
