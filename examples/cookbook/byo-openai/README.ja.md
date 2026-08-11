<!-- neograph-i18n: source=examples/cookbook/byo-openai/README.md locale=ja source_sha256=837a72c1600b8f89e2a5600d0ea31ad8fb44f043e2e9c11d49ff18551164f306 -->
# 独自の OpenAI クライアントを導入する

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

ほとんどの実稼働 Python ユーザーはすでに `openai.OpenAI()` クライアントを持っています
独自の再試行、カスタムトランスポート、可観測性フックを備えたインスタンス、
または OpenRouter ルーティング。このクックブックは接続方法を示しています
既存のクライアントをカスタム `Provider` として NeoGraph に追加します。
NeoGraph の内蔵 `OpenAIProvider`。

秘訣: NeoGraph の `Provider` は、v0.2.3 以降では Python でサブクラス化可能です。
サブクラスの `complete(params)` はグラフ ノード (LLMCallNode、
ReAct ループなど) は組み込みプロバイダーと同様です。

## いつどれを使うか

|欲しい |使用 |
|---|---|
| 「OpenRouter 経由で固定 DeepSeek ルートを使いたい」 |公式 `openai` SDK を使う `OpenAISdkProvider` |
| 「再試行 / Azure / プロキシ / フックを使用して `openai.OpenAI()` をすでにセットアップしています。」 |このクックブック (サブクラス `Provider`、クライアントに委任) |
| 「公式 `openai` SDK 経由で OpenRouter API を使用しています」 | 固定 DeepSeek モデルを使うこのクックブック |
| 「テストで LLM をモックしたい」 |このクックブックには決定的なスタブが含まれています |

ポイント: **NeoGraph のグラフ エンジンは、LLM の呼び出し方法を気にしません。
** が起こります — 必要なのは `params -> ChatCompletion` だけです。

## 全部で60行

[`hybrid.py`](hybrid.py)を参照してください。キーの形状:

```python
import neograph_engine as ng
from openai import OpenAI

class OpenAISdkProvider(ng.Provider):
    """NeoGraph Provider backed by the official `openai` SDK."""
    def __init__(self, client: OpenAI, model: str = "deepseek/deepseek-v4-flash-0731"):
        super().__init__()
        self.client = client
        self.model  = model

    def complete(self, params: ng.CompletionParams) -> ng.ChatCompletion:
        # Translate NeoGraph params into the SDK's chat-completions shape.
        messages = [{"role": m.role, "content": m.content}
                    for m in params.messages]
        resp = self.client.chat.completions.create(
            model=params.model or self.model,
            messages=messages,
            temperature=params.temperature,
        )
        # Translate back into NeoGraph's response shape.
        out = ng.ChatCompletion()
        out.message.role    = "assistant"
        out.message.content = resp.choices[0].message.content or ""
        return out

    def get_name(self) -> str:
        return "openai-sdk"
```

それでおしまい。 `OpenAISdkProvider(OpenAI(api_key=...))` をに渡します
`NodeContext` および `llm_call` ノードを使用する NeoGraph グラフはルーティングされます。
SDK 経由 — すべての再試行/Azure/可観測性/プロキシを維持する
SDK クライアントに添付された構成。

## 走る

```bash
pip install neograph-engine>=0.2.3 openai
echo 'OPENROUTER_API_KEY=sk-or-...' > .env
python hybrid.py
```

出力：
```
[hybrid] using openai SDK inside NeoGraph 0.2.3 graph
[hybrid] running one llm_call through the OpenAI SDK provider
[provider] complete() call #1 (2 msgs) — model=deepseek/deepseek-v4-flash-0731
[... user and assistant messages ...]
[hybrid] provider.complete() called 1× via openai SDK
```

組み込み `llm_call` は、共有 `NodeContext.instructions` をその
システムプロンプト。さまざまなグラフ ステージが必要な場合は、カスタム ノード タイプを使用します。
さまざまなプロンプトが表示されます。

## 保管しているもの

- `openai.OpenAI()` クライアントの `default_headers`、再試行ポリシー、
  カスタム `http_client=httpx.Client(...)`、Azure/プロキシ構成。
- `OpenAIObservabilityCallbacks` / `langfuse` / `helicone` /
  `weights & biases` 統合は SDK レベルでアタッチされます。
  すべての通話を傍受します。
- `usage` (トークン数)、エラー、再試行の既存の追跡。

## あなたが諦めるもの vs `neograph_engine.llm.OpenAIProvider`

- ネイティブ HTTP パス (asio + 接続プール) — よりも約 1.5 倍高速
  SDK と GIL の競合はありません。ボトルネックが OpenAI 呼び出しである場合は、
  SDK は問題ありません。フレームワークのオーバーヘッドの場合は、ネイティブの方が勝ちます。

## ツール呼び出し - 3 つの作業パターン

プロバイダーのトランポリンにより、`complete()` は `tool_calls` をきれいに返すことができます。
現在**動作していない**のは、C++ `tool_dispatch` グラフ ノードです。
Python `Tool` サブクラスへのコールバック — そのパスはセグメンテーション違反です
(既存の問題; v0.3 で追跡されています)。今日は 3 つのパターンが機能します。

### A. エージェント プロバイダー (`byo-openai` に推奨)

**内側** `complete()` でツール ループを実行します。ユーザーの`openai.OpenAI`
クライアントはすでにツール呼び出しをサポートしています。エージェント ループを終了させます
(呼び出し → Python でのディスパッチ → 結果 → 呼び出し → テキスト) とリターンのみ
NeoGraph への最後のアシスタント メッセージ。グラフには正確に 1 つが表示されます
「ターン」ごとに `complete()`、`tool_dispatch` ノードは必要ありません。

```python
class AgenticOpenAIProvider(ng.Provider):
    def __init__(self, client, tools_by_name):
        super().__init__()
        self.client = client
        self.tools  = tools_by_name      # {"calc": calc_fn, ...}
    def complete(self, params):
        messages = [{"role": m.role, "content": m.content} for m in params.messages]
        sdk_tools = [{"type":"function",
                      "function":{"name":n,"description":fn.__doc__ or "",
                                  "parameters":fn.schema}}
                     for n, fn in self.tools.items()]
        for _ in range(10):  # cap loops
            r = self.client.chat.completions.create(
                model=params.model or "deepseek/deepseek-v4-flash-0731",
                messages=messages, tools=sdk_tools)
            choice = r.choices[0]
            if not choice.message.tool_calls:
                out = ng.ChatCompletion()
                out.message.role    = "assistant"
                out.message.content = choice.message.content or ""
                return out
            messages.append(choice.message.model_dump())
            for tc in choice.message.tool_calls:
                fn = self.tools[tc.function.name]
                result = fn(**stdjson.loads(tc.function.arguments))
                messages.append({"role":"tool","tool_call_id":tc.id,
                                 "content":str(result)})
```

トレードオフ: NeoGraph は中間ステップを認識しません (各ステップごとのチェックポイントはありません)。
ツール呼び出し)、ただし SDK の動作はすべて保持され、ディスパッチは行われません
境界摩擦。

### B. C++ ツール + Python プロバイダー

組み込みの C++ ツール (`neograph_engine.mcp` から `MCPTool`、
またはその他の C++ 側 `Tool`) をディスパッチ パスとして指定し、Python
LLM 呼び出しのプロバイダー。グラフの `tool_dispatch` ノード呼び出し
C++ ツールは問題ありません。 Python `Tool` サブクラスへのコールバックのみ
クラッシュします。

### C. プロバイダーはtool_callsを返します。カスタム Python ノードのディスパッチ

組み込みの `tool_dispatch` ノードをスキップします。自分で書いてください
`@ng.node("dispatch")` は `messages[-1].tool_calls` を読み取り、呼び出します
Python ツールに直接アクセスし、ツールの結果メッセージを書き込みます。
戻る。完全に Python に留まります。

## A2A + カスタムプロバイダー

このクックブックは自然に次のように構成されています。
[ai-assembly cookbook](../ai-assembly/) — 各メンバーを置き換えます
`OpenAISdkProvider(...)` を備えたプロバイダーを使用してすべての SDK レベルを取得します
NeoGraph の A2A ブリッジを使用しながら、すべてのペルソナの動作を監視します。
