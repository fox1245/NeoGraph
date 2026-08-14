# NeoGraph + OpenRouter（固定 DeepSeek）

**言語:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

NeoGraph のグラフを OpenRouter の固定モデル
`~deepseek/deepseek-v4-flash-latest` に接続します。同じ API キー、モデル、
明示的な zero-data-retention (ZDR) provider 優先設定を使う 2 つの
Provider サーフェスを示します。

1. 組み込み `OpenAIProvider` で OpenRouter の OpenAI 互換 chat endpoint
   を呼ぶ (`via_openai_compat.py`)
2. 正規化された chat リクエストを直接送るカスタム Python `Provider`
   (`via_http.py`)

後者は wire contract を明示しながら、NeoGraph の `Provider` trampoline
が transport に依存しないことを示します。

## パス A — 組み込み Provider

`OpenAIProvider` は `/v1/chat/completions` を自動的に追加するため、
`/v1` を含めない OpenRouter の API ベース URL を渡します。

```python
from neograph_engine.llm import OpenAIProvider

provider = OpenAIProvider(
    api_key=os.environ["OPENROUTER_API_KEY"],
    base_url="https://openrouter.ai/api",
    default_model="~deepseek/deepseek-v4-flash-latest",
    provider_routing={"zdr": True},
```

コード全体は [`via_openai_compat.py`](via_openai_compat.py) にあります。

## パス B — カスタム直接 HTTP Provider

[`via_http.py`](via_http.py) は `Provider` をサブクラス化し、
`https://openrouter.ai/api/v1/chat/completions` に POST します。
`choices[0].message` と token usage を NeoGraph の応答へ写し、
OpenRouter の ZDR provider 優先設定もリクエストに含めます。

## 実行

```bash
echo 'OPENROUTER_API_KEY=sk-or-...' > .env
pip install neograph-engine>=0.2.3 httpx
python via_openai_compat.py
python via_http.py
```

どちらも厳密な 1 ノードグラフを構築し、同じ固定 DeepSeek モデルへ
`llm_call` をルーティングします。system prompt は共有
`NodeContext.instructions` から供給されます。キーがない場合は明確な
エラーで終了し、live 経路が暗黙に mock へ切り替わることはありません。

## 出力形式

```text
[openrouter] using ~deepseek/deepseek-v4-flash-latest via OpenAI-compatible path
[user] What's the capital of France?
  assistant: Paris is the capital of France.
```

実際の応答は変わります。直接 HTTP 経路は `via direct HTTP` を表示し、
別の算術質問を使います。

## 2 つの経路を残す理由

- パス A は NeoGraph の native HTTP 実装と connection pool を使います。
- パス B は custom header、transport ポリシー、応答変換をアプリ側で
  所有する場合の最小例です。
- 両方が同じ OpenRouter endpoint 系列、API キー、固定 DeepSeek モデルを
  使うため、Provider サーフェスの比較が意味を持ちます。

OpenRouter API のリクエスト/レスポンス形式:
<https://openrouter.ai/docs/api-reference/overview>
