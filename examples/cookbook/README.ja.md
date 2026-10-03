<!-- neograph-i18n: source=examples/cookbook/README.md locale=ja source_sha256=01f0466eb43755a45f571b1f6bcdd22e270983bc426fb68d843fdc881e63aa3e -->
# NeoGraph Cookbooks


## 型付き C++ 移行状況

現在の C++ recipe は五つの型付き SDK family の所有された `ProviderRequest`、
順序付き `sp::Event`、不変 `std::shared_ptr<const sp::Outcome>`
（`Completion`/`Failure`）を使用します。`ChatMessage`/`ChatTool` は portable 投影であり、
native replay 権限ではありません。prepare は一度だけ行い、durable 呼出元は
`Provider::request_digest(prepared)` に claim/receipt を結び付け、同じ handle を dispatch します。
出力 JSON から history を再構築したり、failure を最終テキストに縮約しないでください。

canonical persistence は `provider-message-v2`/`runtime-history-record-v2` を使用し、
portable 要約は native record を置換しません。optional control は呼出元が選び、暗黙に clamp しません。
bounded call には真正 model fact が必要で、欠落は `LimitUnknown` です。
reservation/charged/held は nullable provider usage と別で、budget 更新、価格、forecast、invoice ではありません。

header-only `examples/provider_example_support.h` は実際の SDK runtime を使用します。
LLM ビルドには `find_package(SchemaProvider CONFIG REQUIRED COMPONENTS runtime)` の `SchemaProvider::runtime` または
明示的な `-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=<sdk-source>` が必要です。
インストール include root は `include/SchemaProvider`。interface/capability 検査を行い、
SDK package は unstable `0.0.0`（interface 3）です。

現在の model-free 実行では Assembly の実際のローカル A2A member サーバー四つと C++ speaker、
JARVIS CLI synthetic turn とメモリ永続化、Beast strict Core コンパイル・進化・checkpoint rollback、
ProgramChat のブラウザー tenant 分離・generation 置換と PostgreSQL black-box 六シナリオを確認しました。
下の一覧はこの実行範囲と未検証 surface を区別します。vendor inference、音声、延期 Python binding、
全 Beast live 変種や専用 multitenant server/load の pass は主張しません。
過去の測定は新移行の qualification ではありません。live 実行には鍵、ネットワーク、モデルアクセスと費用が必要です。
鍵、prompt、artifact は非公開に保ち、機密 envelope/native inspection 出力を公開 log に送らないでください。
native archive は認証された owner-private custody であり、暗号化や vendor issuer 認証ではありません。

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

複数のNeoGraph機能を実際の動作シナリオに組み合わせるエンドツーエンドのレシピ集。各レシピは自己完結型です。フォルダをコピーし、READMEに従い、実行するだけです。

| クックブック | 内容 |
|---|---|
| [`the-beast/`](the-beast/) | **自己進化型エージェント：生成・進化・ロールバック。** Beastは厳密なCore JSONを作成し、実行前に検証し、`evolve()`で制約付きCoreトポロジーを進化させ、チェックポイントを通じてロールバックします。live、apex、forge、script、arithmetic-evolutionの各変種は同じコンパイラ/検証境界を保持します。JavaScriptまたは信頼済みC++がソースの作成を担い、厳密なCore JSONは交換データのままです。 |
| [`ai-assembly/`](ai-assembly/) | マルチペルソナA2A：国民議会の4名の議員（それぞれ独自のA2Aエンドポイントを持つ）+ 法案を並列で配信し票を集計する議長。クロスランゲージ対応：C++メンバーサーバー + PythonまたはC++の議長。 |
| [`byo-openai/`](byo-openai/) | 過去の provider recipe; Python binding 移行は延期. |
| [`jarvis/`](jarvis/) | **音声駆動メタオーケストレーター（スケルトン）。** マイク → whisper.cpp（言語自動検出） → ルーター（直接／委譲／並列の3方向） → MCPツールまたはA2Aスペシャリスト → ユーザーが検出した言語でのオンデバイスTTS（上位音）。JSON駆動のツール＋エージェントカタログ、A2A双方向（JARVIS自体も到達可能）。オンデバイスで、クラウド不要。 |
| [`minimal-mcp/`](minimal-mcp/) | MCPクライアントのラウンドトリップ： **LLMなし、APIキーなし、fastmcpなし**。約60行のstdlib stdioサーバー＋`initialize` → `tools/list` → `tools/call`を実行するC++ハーネスです。NeoGraphのMCPクライアントが必要とするのはワイヤープロトコルを話すプロセスだけで、通信相手は何であってもよいことを示します。 |
| [`openrouter-provider/`](openrouter-provider/) | 過去の provider recipe; Python binding 移行は延期. |

各クックブックは、表面化した摩擦も文書化します。これは、公開APIの粗いエッジを見つけるのに役立ちます。

## 全 recipe と延期 surface の一覧

| Recipe | Status |
|---|---|
| [`ai-assembly/`](ai-assembly/) | 型付き C++ 移行; 実際のローカル A2A member サーバー四つと C++ speaker を offline 実行; synthetic abstention はモデル判断ではない |
| [`byo-openai/`](byo-openai/) | 過去の provider recipe; Python binding 移行は延期 |
| [`jarvis/`](jarvis/) | 型付き C++ 移行; CLI synthetic turn・メモリ永続化・正常 EOF を実行; 音声/Python surface は未検証 |
| [`minimal-mcp/`](minimal-mcp/) | protocol-only client/server; 意図的に不変 |
| [`multi_tenant_chatbot/`](multi_tenant_chatbot/) | 型付き C++ 移行; 専用 server 実行と live 1,000/32 load は未検証 |
| [`openrouter-provider/`](openrouter-provider/) | 過去の provider recipe; Python binding 移行は延期 |
| [`self_evolving_chatbot/`](self_evolving_chatbot/) | 型付き C++ 移行; vendor inference なしで ProgramChat ブラウザー tenant 分離・generation 置換と PostgreSQL black-box 六シナリオを実行 |
| [`the-beast/`](the-beast/) | 型付き C++ 移行; 実際の strict Core コンパイル・進化・checkpoint rollback を実行; 全 live 変種の pass 主張ではない |
| [`topology-retrieval/`](topology-retrieval/) | protocol-only client/server; 意図的に不変 |

Python MCP server、Jarvis CLI/REPL driver、retrieval HTTP client は protocol client で provider binding 実装ではありません。Assembly Python speaker と Jarvis pybind benchmark は延期 binding に依存します。live multitenant 1,000/32 は smoke ではありません。
