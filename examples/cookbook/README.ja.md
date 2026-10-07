<!-- neograph-i18n: source=examples/cookbook/README.md locale=ja source_sha256=4d090ff982d9da4ec65a9f2e24855eec4c18d22b530bebaf720905e944355fda -->
# NeoGraph Cookbooks

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

## 型付き C++ 移行状況

現在の C++ recipe は五つの型付き SDK family の所有された `ProviderRequest`、
順序付き `sp::Event`、不変 `std::shared_ptr<const sp::Outcome>`
（`Completion`/`Failure`）を使用します。`ChatMessage`/`ChatTool` は portable 投影であり、
native replay 権限ではありません。prepare は一度だけ行い、durable 呼出元は
`Provider::request_digest(prepared)` に claim/receipt を結び付け、同じ handle を dispatch します。
local wait の終了は remote model が停止した、または請求しない証拠ではありません。durable receipt は自動 redispatch を防ぎますが external effect の exactly-once は保証しません。
出力 JSON から history を再構築したり、failure を最終テキストに縮約しないでください。

canonical persistence は `provider-message-v2`/`runtime-history-record-v2` を使用し、
portable 要約は native record を置換しません。optional control は呼出元が選び、暗黙に clamp しません。
bounded call には真正 model fact が必要で、欠落は `LimitUnknown` です。
reservation/charged/held は nullable provider usage と別で、budget 更新、価格、forecast、invoice ではありません。

header-only `examples/provider_example_support.h` は実際の SDK runtime を使用します。
Core-only を含むすべての native ビルドには `find_package(SchemaProvider CONFIG REQUIRED COMPONENTS runtime)` の `SchemaProvider::runtime` または
明示的な `-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=<sdk-source>` または immutable public SDK archive fallback を使います。offline installed/source SDK では `-DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` で fetching を止めます。
インストール include root は `include/SchemaProvider`。interface/capability 検査を行い、
SDK `0.1.1` alpha の interface/shared ABI は4です。

保存済み interface-3 model-free 実行では Assembly の実際のローカル A2A member サーバー四つと C++ speaker、
JARVIS CLI synthetic turn とメモリ永続化、Beast strict Core コンパイル・進化・checkpoint rollback、
専用 mock topology load と retrieval index 再使用・admission、ProgramChat ブラウザー tenant 分離・
generation 置換と SQLite六件・PostgreSQL六件の black-box scenario を確認しました。
下の一覧はこの過去の実行を、インストール済み Python application、A2A peer、Forge helper を含む
[現在の interface-4 local 証拠](../README.md#typed-c-cutover-status)と区別します。
vendor inference、音声、全 Beast live 変種や専用 live multitenant 1,000/32 load は未検証です。
過去の測定は新移行の qualification ではありません。live 実行には鍵、ネットワーク、モデルアクセスと費用が必要です。
鍵、prompt、artifact は非公開に保ち、機密 envelope/native inspection 出力を公開 log に送らないでください。
native archive は認証された owner-private custody であり、暗号化や vendor issuer 認証ではありません。


複数の NeoGraph 機能を組み合わせる recipe です。C++ target は必要な SDK package を指定した NeoGraph ツリーでビルドします。フォルダーのコピーだけでは standalone ビルドになりません。

| クックブック | 内容 |
|---|---|
| [`the-beast/`](the-beast/) | **自己進化型エージェント：生成・進化・ロールバック。** Beastは厳密なCore JSONを作成し、実行前に検証し、`evolve()`で制約付きCoreトポロジーを進化させ、チェックポイントを通じてロールバックします。live、apex、forge、script、arithmetic-evolutionの各変種は同じコンパイラ/検証境界を保持します。JavaScriptまたは信頼済みC++がソースの作成を担い、厳密なCore JSONは交換データのままです。 |
| [`ai-assembly/`](ai-assembly/) | マルチペルソナA2A：国民議会の4名の議員（それぞれ独自のA2Aエンドポイントを持つ）+ 法案を並列で配信し票を集計する議長。クロスランゲージ対応：C++メンバーサーバー + PythonまたはC++の議長。 |
| [`byo-openai/`](byo-openai/) | 型付き Python provider request と真正 prepared handle。互換性と実行証拠は各 recipe を参照してください。 |
| [`jarvis/`](jarvis/) | 音声駆動メタオーケストレーター。任意のローカル ASR/TTS、chat/direct/delegate/parallel の四方向 router、MCP tools、A2A specialists と会話 memory を組み合わせます。ローカル/mock は cloud-free、live 推論は OpenRouter を使います。 |
| [`minimal-mcp/`](minimal-mcp/) | MCPクライアントのラウンドトリップ： **LLMなし、APIキーなし、fastmcpなし**。約60行のstdlib stdioサーバー＋`initialize` → `tools/list` → `tools/call`を実行するC++ハーネスです。NeoGraphのMCPクライアントが必要とするのはワイヤープロトコルを話すプロセスだけで、通信相手は何であってもよいことを示します。 |
| [`openrouter-provider/`](openrouter-provider/) | 型付き Python provider request と真正 prepared handle。互換性と実行証拠は各 recipe を参照してください。 |

各クックブックは、表面化した摩擦も文書化します。これは、公開APIの粗いエッジを見つけるのに役立ちます。

## 全 recipe 一覧と証拠の範囲

| Recipe | Status |
|---|---|
| [`ai-assembly/`](ai-assembly/) | 保存済み interface-3 四サーバー/C++ session; 現在のインストール済み Python Speaker を local A2A 0.3/1.0 peer で実行; synthetic abstention はモデル判断ではない |
| [`byo-openai/`](byo-openai/) | Installed SDK4 wheel と公式 OpenAI client を local Chat peer で実行：known/unknown usage、実 tool 関数、八回呼出し上限、hosted-call guard を検証；vendor inference なし |
| [`jarvis/`](jarvis/) | 保存済み interface-3 CLI 永続化/EOF 証拠；現在の Python MCP tool、LangGraph twin/driver、pybind per-turn/startup 正確性を検証；新しい性能・音声 qualification ではない |
| [`minimal-mcp/`](minimal-mcp/) | 実際の Python stdlib server を公式 MCP client で実行：handshake/discovery と計算・時刻・demo-weather 呼出しを検証；LLM や実気象サービスなし |
| [`multi_tenant_chatbot/`](multi_tenant_chatbot/) | 専用 mock1,000要求・error0・compiled topology3・cache hit997; isolated host は reference metadata のみ; live1,000/32未検証 |
| [`openrouter-provider/`](openrouter-provider/) | Installed SDK4 native provider と custom HTTP node を local Chat peer で実行：known/unknown usage、保持 history、hosted-call guard を検証；vendor inference なし |
| [`self_evolving_chatbot/`](self_evolving_chatbot/) | 実ブラウザー tenant 分離・generation 置換、SQLite六件・PostgreSQL六件 black-box と明示 host model policy を検証; vendor inferenceなし |
| [`the-beast/`](the-beast/) | 型付き C++ 移行; 実際の strict Core コンパイル・進化・checkpoint rollback を実行; 全 live 変種の pass 主張ではない |
| [`topology-retrieval/`](topology-retrieval/) | mock Python ranking/index 再使用と実 C++ registry admission/migration・unknown-key拒否を検証; 外部 pointer は権限ではない |

Python MCP server、Jarvis CLI/REPL driver、retrieval HTTP client は protocol client で provider binding 実装ではありません。Assembly Python speaker は A2A binding、Jarvis pybind benchmark は移行済み native binding を使います。その実行証拠は上の C++ 実行とは別です。SDK runtime の現在の検証対象は Linux/POSIX であり、macOS/Windows transport の検証は主張しません。live multitenant 1,000/32 は smoke ではありません。

## Interface-4 制御と保持された呼出し制限

`ProviderControls` は family ごとの reasoning、sampling、tool selection を保持します。
Chat は admitted OpenRouter reasoning object、usage/include 制御、代替 model、
Responses は明示的サーバー保持 continuation、verbosity、truncation、include 選択を提供します。
Messages は manual/adaptive/disabled thinking、effort、cache 制御、tool choice、
Gemini は明示的 portable foreign history、thinking level、safety setting、sampling、tool choice を
提供します。非対応 family/origin/model は I/O 前に拒否します。native message には真正 custody が
必要であり、出力 JSON、cursor、portable 投影では作れません。
正確な型は [provider reference](../../docs/reference-en.md) を参照してください。

Forge は low reasoning effort を要求し、ask ごとに300秒 deadline を固定します。完了した空の
`MaxTokens` 応答に text と有効/無効 client call がない場合だけ、現在の出力 cap の二倍で
一回追加呼出しできます。両 outcome と usage を保持し、failure、observer/settlement エラーは
再試行しません。既存 live chatbot 経路 `multi_tenant_chatbot/server_live_llm.cpp` と
`self_evolving_chatbot/server_multi.cpp` の provider timeout は180秒です。
ProgramChat の別の CLI 制限や共有 factory の既定値は変えません。

[番号付き例の契約](../README.md#retained-example-contracts) は Deep Research の制限付き cap ladder と
deadline discovery、例16/28、fork/new-turn、計算した replay 節約数、evolution file モード、
A2A 0.x/1.0 snapshot を説明します。このソース更新は上の実行を interface-4 や
live-provider pass に読み替えるものではありません。
