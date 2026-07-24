<!-- neograph-i18n: source=examples/cookbook/ollama-provider/README.md locale=ja source_sha256=e9208ffec69b9c3d4b86998e648ea60f8b2f9c217008241cd11b71af7c346bed -->
# NeoGraph + Ollama (ローカル LLM)

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

NeoGraph グラフをローカルで提供されるモデルに対して実行します。
[Ollama](https://ollama.com) — API キーなし、ネットワーク下りなし、フル
プライバシー。 2 つのパス、どちらも NeoGraph 0.2.3 以降で動作します。

## パス A — Ollama の互換エンドポイントに対する `OpenAIProvider` *(新しいコードはゼロ)*

Ollama は OpenAI リクエスト/レスポンスで `/v1/chat/completions` を公開します
形。 NeoGraph の内蔵 `OpenAIProvider` をそれに向けると、
完了 — 同じネイティブ ASIO HTTP パス、同じ接続プール、同じ速度。

```python
from neograph_engine.llm import OpenAIProvider
# OpenAIProvider appends `/v1/chat/completions` itself, so base_url
# is the bare host — NOT host + "/v1". Ollama returns 404 otherwise.
provider = OpenAIProvider(
    api_key="sk-anything",                       # required field, value ignored
    base_url="http://127.0.0.1:11434",            # NOT ".../v1"
    default_model="qwen2.5:0.5b",
)
```

それが全体の統合です。 [`via_openai_compat.py`](via_openai_compat.py)を参照してください。

**パス A を選択する場合 **: スピードが欲しいなら、オラマは気にしない -
特定の機能 (モデルのプル/ロード、ネイティブ フィールドによるストリーミング、
`keep_alive` ブロックと `options` ブロック)。ユーザーの95%。

## パス B — カスタム Python `Provider` 経由のネイティブ Ollama API

v0.2.3 `Provider` トランポリンが本物であることを示します。
ベンダーに依存しない。 Ollama のネイティブ `/api/chat` をラップします (より豊富なパラメーター、
互換レイヤーの翻訳はありません）、次のように出荷されます。
[`via_native_api.py`](via_native_api.py)。

```python
class OllamaProvider(ng.Provider):
    def __init__(self, model="qwen2.5:0.5b", host="http://127.0.0.1:11434"):
        super().__init__()
        self.model = model
        self.host = host
    def complete(self, params):
        r = httpx.post(f"{self.host}/api/chat", json={
            "model": params.model or self.model,
            "messages": [{"role": m.role, "content": m.content}
                         for m in params.messages],
            "stream": False,
            "options": {"temperature": params.temperature},
        }, timeout=120)
        r.raise_for_status()
        body = r.json()
        out = ng.ChatCompletion()
        out.message.role    = "assistant"
        out.message.content = body["message"]["content"]
        return out
    def get_name(self): return "ollama"
```

**パス B を選択する場合**: Ollama ネイティブのフィールド (`options`、
`keep_alive`、`format=json` 制約)、またはチームがすでに
`ollama` Python SDK をクライアント サーフェスとして保持したいとします。

## 走る

```bash
# 1. start Ollama (separate terminal)
ollama serve

# 2. pull a small model so it fits in CI / laptop RAM
ollama pull qwen2.5:0.5b      # ~400 MB

# 3. install NeoGraph + httpx (Path B uses httpx)
pip install neograph-engine>=0.2.3 httpx

# 4. run either path
python via_openai_compat.py
python via_native_api.py
```

どちらのデモも厳密な 1 ノード グラフを構築し、その `llm_call` を
ローカルモデル。システム プロンプトは共有ファイルから取得されます。
`NodeContext.instructions`。外部 API キーは必要ありません。

## 出力

```
[ollama] using qwen2.5:0.5b at http://127.0.0.1:11434 (OpenAI-compat path)
[user] What's the capital of France?
       user: What's the capital of France?
  assistant: Paris is the capital of France.
```

(実際の補完はモデルによって異なります。ネイティブ パスが出力されます)
`(native /api/chat path)` と独自のサンプル質問を使用します。)

## 注意事項

- **`qwen2.5:0.5b` を選ぶ理由** 対応する最小の主流モデル
  英語＋簡単な推理。コールドスタートでは 2 ～ 3 秒で負荷がかかり、その後
  CPU 上で ~100 ミリ秒/完了。クックブックのデモに適しています。に交換
  `llama3.2:3b` / `qwen2.5:7b` / `phi4:14b` を確認したら
  ループが機能します。
- **最初の呼び出しが遅い** — Ollama はモデルをメモリに遅延してロードします
  最初のリクエスト時 (サブ 1B の場合は約 2 ～ 5 秒)。その後の通話は温かいものになります。
  `keep_alive` パラメータ (パス B) は、モデルの長さを制御します。
  アイドル後もロードされたままになります。
- **ツール呼び出し**: Ollama のツール呼び出しサポートはモデルに依存します。
  `/v1/chat/completions` 経由で OpenAI 互換シェイプを使用します
  終点。パス A + からのエージェント プロバイダー パターンを使用します。
  [`../byo-openai/hybrid_with_tools.py`](../byo-openai/hybrid_with_tools.py)
  ツール対応の Ollama モデル (`qwen2.5:7b` など) を使用します。
- **ストリーミング**: パス A は、NeoGraph のネイティブ ストリーミングを継承します。
  `OpenAIProvider.complete_stream`。パス B の例はストリーミングしません。
  必要に応じて、`"stream": True` + チャンク ループを追加します。

## なぜこれが重要なのか

NeoGraph (5 μs のエンジン オーバーヘッド) とローカルの Ollama モデルの組み合わせ
**外部依存関係がゼロ**のエージェント スタック全体が得られます。

- 13 MB ネイティブ バイナリ + ~400 MB モデル = **Raspberry Pi 5 に適合**
- API キーなし、レート制限なし、ネットワーク下りなし、完全なデータ プライバシー
- グラフセマンティクスにおける LangGraph のパリティ。 Ollama 品質のモデル
  LLM コール
- エンジン層での反復ごとのオーバーヘッドがノイズの中に含まれます (
  500 ～ 2000 ミリ秒のモデル推論が優勢ですが、フレームワークのフットプリントは
  LangGraph + Python ランタイムより大幅に小さい

適した用途: エッジ AI、オンデバイス エージェント、プライバシーが必要な展開、
自己ホスト型アシスタント、および OpenAI の早期請求にうんざりしている人
プロトタイピング。
