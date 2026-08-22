<!-- neograph-i18n: source=examples/cookbook/byo-openai/README.md locale=ja source_sha256=812a1f340ed6b8f92ddd742cc1c8f239265b501fa010c81929825f0973738e38 -->
# 自带 OpenAI 客户端（Bring Your Own OpenAI Client）

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

大多数生产环境中的 Python 用户已经拥有一个 `openai.OpenAI()` 客户端实例，其中包含他们自己的重试逻辑、自定义传输层、可观测性钩子或 OpenRouter 路由。本食谱展示了如何将该现有客户端插入 NeoGraph 作为自定义 `Provider` —— 而不是使用 NeoGraph 内置的 `OpenAIProvider`。

技巧在于：NeoGraph 的 `Provider` 在 v0.2.3+ 中可被 Python 子类化。子类的 `complete(params)` 会在图节点内（LLMCallNode、ReAct 循环等）运行，就像内置的提供者一样。

## 何时使用哪种方式

| 你希望 | 使用 |
|---|---|
| 「ピン留めされたDeepSeekルートをOpenRouter経由で使うだけ」 | `OpenAISdkProvider` 公式の `openai` SDK を使用して |
| “我已经有一个 `openai.OpenAI()` 设置好了重试 / Azure / 代理 / 钩子” | このクックブック（サブクラス`Provider`、クライアントに委譲） |
| 「公式の`openai` SDKを通じてOpenRouter APIを使用しています」 | このピン留めされたDeepSeekモデルを使ったクックブック |
| 「テストでLLMをモックしたい」 | このクックブック(決定的スタブ付き) |

关键点：**NeoGraph 的图引擎并不关心 LLM 调用如何进行** —— 它只需要 `params -> ChatCompletion`。

## 全部代码约 60 行

[`hybrid.py`](hybrid.py) を参照してください。キーの形状:

```python
import neograph_engine as ng
from openai import OpenAI

class OpenAISdkProvider(ng.Provider):
    """NeoGraph Provider backed by the official `openai` SDK."""
    def __init__(self, client: OpenAI, model: str = "~deepseek/deepseek-v4-flash-latest"):
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

それだけです。`OpenAISdkProvider(OpenAI(api_key=...))`を`NodeContext`に渡すと、`llm_call`ノードを使用する任意のNeoGraphグラフはSDK経由でルーティングされ、SDKクライアントに接続されたすべてのリトライ/Azure/可観測性/プロキシ構成が保持されます。

## 実行

```bash
pip install neograph-engine>=0.2.3 openai
echo 'OPENROUTER_API_KEY=sk-or-...' > .env
python hybrid.py
```

出力:
```
[hybrid] using openai SDK inside NeoGraph 0.2.3 graph
[hybrid] running one llm_call through the OpenAI SDK provider
[provider] complete() call #1 (2 msgs) — model=~deepseek/deepseek-v4-flash-latest
[... user and assistant messages ...]
[hybrid] provider.complete() called 1× via openai SDK
```

組み込みの`llm_call`は共有の`NodeContext.instructions`をシステムプロンプトとして使用します。異なるグラフステージで異なるプロンプトが必要な場合は、カスタムノードタイプを使用してください。

## 保持されるもの

- `openai.OpenAI()`クライアントの`default_headers`、リトライポリシー、カスタム`http_client=httpx.Client(...)`、Azure/プロキシ構成。
- `OpenAIObservabilityCallbacks` / `langfuse` / `helicone` / `weights & biases`のSDKレベルでの統合 — すべての呼び出しをインターセプトします。
- `usage`(トークンカウント)の既存の追跡、エラー、リトライ。

## `neograph_engine.llm.OpenAIProvider`と比較して失うもの

- ネイティブHTTPパス(asio+接続プール)— SDKよりも約1.5倍高速で、GIL競合ゼロ。ボトルネックがOpenAI呼び出しならSDKで十分ですが、フレームワークオーバーヘッドが問題ならネイティブが勝ちます。

## ツール呼び出し — 動作する3つのパターン

Provider トランポリンにより、`complete()` は `tool_calls` をクリーンに返すことができます。現在**機能していない**のは、 C++ `tool_dispatch` グラフノードが Python `Tool` サブクラスにコールバックするパスです — このパスはセグメンテーションフォールトします (既存の問題。v0.3 で追跡中)。本日動作する 3 つのパターンは以下です:

### A. Agentic Provider (`byo-openai` に推奨)

ツールループを**内部**で実行してください `complete()`。ユーザーの `openai.OpenAI` クライアントはすでにツール呼び出しをサポートしています。エージェントループ（呼び出し → Pythonでのディスパッチ → 結果 → 呼び出し → テキスト）を完了させ、最終的なアシスタントメッセージのみをNeoGraphに返します。グラフは「ターン」ごとに正確に1つの `complete()` を認識し、 `tool_dispatch` ノードは必要ありません。

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
                model=params.model or "~deepseek/deepseek-v4-flash-latest",
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

トレードオフ: NeoGraphは中間ステップを見ません(ツール呼び出しごとのチェックポイントはありません)が、すべてのSDK動作を保持し、ディスパッチ境界の摩擦はありません。

### B. C++ツール + Python Provider

ディスパッチパスには組み込みのC++ツール（`MCPTool`（`neograph_engine.mcp`由来）、またはその他のC++側の`Tool`）を使用し、LLM呼び出しにはPython Providerを使用します。グラフの`tool_dispatch`ノードはC++ツールを問題なく呼び出せます。クラッシュするのは、Pythonの`Tool`サブクラスへのコールバックだけです。

### C. プロバイダーがtool_callsを返します。カスタムPythonノードがディスパッチします。

組み込みの `tool_dispatch` ノードをスキップしてください。独自の `@ng.node("dispatch")` を作成し、 `messages[-1].tool_calls`を読み取り、Python ツールを直接呼び出し、ツール結果メッセージを書き戻します。すべて Python のままです。

## A2A + カスタムProvider

このクックブックは、[ai-assembly cookbook](../ai-assembly/) と自然に組み合わせることができます — 各メンバーのプロバイダーを `OpenAISdkProvider(...)` に置き換えることで、NeoGraph の A2A bridge を引き続き使用しながら、すべての persona で SDK レベルの動作をすべて得ることができます。
