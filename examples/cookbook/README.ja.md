<!-- neograph-i18n: source=examples/cookbook/README.md locale=ja source_sha256=2b960566263f063bf11a97a63b315005e7ab13700b5839294441f20eb52f6256 -->
# NeoGraph Cookbooks

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

複数のNeoGraph機能を実際の動作シナリオに組み合わせるエンドツーエンドのレシピ集。各レシピは自己完結型です。フォルダをコピーし、READMEに従い、実行するだけです。

| クックブック | 内容 |
|---|---|
| [`the-beast/`](the-beast/) | **自己進化型エージェント：生成・進化・ロールバック。** Beastは厳密なCore JSONを作成し、実行前に検証し、`evolve()`で制約付きCoreトポロジーを進化させ、チェックポイントを通じてロールバックします。live、apex、forge、script、arithmetic-evolutionの各変種は同じコンパイラ/検証境界を保持します。JavaScriptまたは信頼済みC++がソースの作成を担い、厳密なCore JSONは交換データのままです。 |
| [`ai-assembly/`](ai-assembly/) | マルチペルソナA2A：国民議会の4名の議員（それぞれ独自のA2Aエンドポイントを持つ）+ 法案を並列で配信し票を集計する議長。クロスランゲージ対応：C++メンバーサーバー + PythonまたはC++の議長。 |
| [`byo-openai/`](byo-openai/) | 独自の`openai.OpenAI()`クライアントを持ち込む：NeoGraphの`Provider`をサブクラス化して、すべてのLLM呼び出しをSDKに委譲し、リトライ/Azure/オブザーバビリティ設定をすべて維持します。また、agenticプロバイダーパターンによるツール呼び出しも含みます。 |
| [`jarvis/`](jarvis/) | **音声駆動メタオーケストレーター（スケルトン）。** マイク → whisper.cpp（言語自動検出） → ルーター（直接／委譲／並列の3方向） → MCPツールまたはA2Aスペシャリスト → ユーザーが検出した言語でのオンデバイスTTS（上位音）。JSON駆動のツール＋エージェントカタログ、A2A双方向（JARVIS自体も到達可能）。オンデバイスで、クラウド不要。 |
| [`minimal-mcp/`](minimal-mcp/) | MCPクライアントのラウンドトリップ： **LLMなし、APIキーなし、fastmcpなし**。約60行のstdlib stdioサーバー＋`initialize` → `tools/list` → `tools/call`を実行するC++ハーネスです。NeoGraphのMCPクライアントが必要とするのはワイヤープロトコルを話すプロセスだけで、通信相手は何であってもよいことを示します。 |
| [`openrouter-provider/`](openrouter-provider/) | OpenRouter修正済みDeepSeekプロバイダーサーフェス: 互換性エンドポイントに対する組み込み`OpenAIProvider`と、直接HTTPを使用したカスタムPython `Provider`。 |

各クックブックは、表面化した摩擦も文書化します。これは、公開APIの粗いエッジを見つけるのに役立ちます。
