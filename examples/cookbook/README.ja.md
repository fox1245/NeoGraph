<!-- neograph-i18n: source=examples/cookbook/README.md locale=ja source_sha256=b668003b55bbf84e6463dc6dbc7c708f77d62a9face15528b6fc7e32caac0182 -->
# NeoGraph クックブック

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

P8 処理一覧: [`spec/neograph-example-disposition-v1.json`](../../spec/neograph-example-disposition-v1.json)。

複数の NeoGraph 機能を 1 つにまとめたエンドツーエンドのレシピ
実際の作業シナリオ。それぞれが自己完結型です。フォルダーをコピーし、
READMEに従って実行してください。

|料理本 |それが示すもの |
|---|---|
| [`the-beast/`](the-beast/) | **自己進化エージェント: 生成、進化、ロールバック。** Beast は strict Core JSON を作成し、実行前にコンパイルと検証を通し、`evolve()` で Core トポロジを進化させ、チェックポイントでロールバックします。ライブ、apex、forge、script、evolve の各変種も同じ境界を使い、ソース作成は JavaScript または信頼された C++、strict Core JSON は交換データとして保持します。 |
| [`ai-assembly/`](ai-assembly/) |マルチペルソナ A2A: 国会議員 4 名 (それぞれ独自の A2A エンドポイント) + 法案を並行してブロードキャストし、投票を集計する議長。クロス言語: C++ メンバー サーバー + Python または C++ スピーカー。 |
| [`byo-openai/`](byo-openai/) |独自の `openai.OpenAI()` クライアントを導入します。NeoGraph の `Provider` をサブクラス化して、すべての LLM 呼び出しを SDK に委任し、すべての再試行 / Azure / オブザーバビリティ構成を維持します。また: エージェント プロバイダー パターンを介したツール呼び出し。 |
| [`jarvis/`](jarvis/) | **音声駆動のメタ オーケストレーター (スケルトン)。** マイク → whisper.cpp (言語自動検出) → ルーター (ダイレクト / デリゲート / パラレル 3 ウェイ) → MCP ツールまたは A2A スペシャリスト → ユーザーが検出した言語でのスーパートニック オンデバイス TTS。 JSON 駆動のツール + エージェント カタログ、A2A 双方向 (JARVIS 自体に到達可能)。オンデバイス、クラウド不要。 |
| [`minimal-mcp/`](minimal-mcp/) | **LLM なし、API キーなし、fastmcp なし**の MCP クライアント ラウンドトリップ: ~60 行の stdlib stdio サーバー + `initialize` → `tools/list` → `tools/call` を実行する C++ ハーネス。 NeoGraph の MCP クライアントにはワイヤ プロトコルを話すプロセスのみが必要であることを示します。ピアは何でもかまいません。 |
| [`openrouter-provider/`](openrouter-provider/) | OpenRouter の固定 DeepSeek provider。互換 endpoint 用の組み込み `OpenAIProvider` と直接 HTTP のカスタム Python `Provider` を比較します。 |

各クックブックには、それが表面化した摩擦についても記録されています。
パブリック API の大まかな部分を見つけます。
