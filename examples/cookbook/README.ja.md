<!-- neograph-i18n: source=examples/cookbook/README.md locale=ja source_sha256=2b1edd2874749ce5d0bc9d01ffd13e06e8bb22a499b2572cca9079e67bbc2272 -->
# NeoGraph Cookbooks

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

## 型付き C++ 移行状況

現在の C++ recipe は五つの型付き SDK family の所有された `ProviderRequest`、
順序付き `sp::Event`、不変 `std::shared_ptr<const sp::Outcome>`
（`Completion`/`Failure`）を使用します。`ChatMessage`/`ChatTool` は portable 投影であり、
native リプレイ権限ではありません。prepare は一度だけ行い、durable 呼出元は
`Provider::request_digest(prepared)` に claim/receipt を結び付け、同じ handle を dispatch します。
local wait の終了は remote model が停止した、または請求しない証拠ではありません。durable receipt は自動 redispatch を防ぎますが external effect の exactly-once は保証しません。
出力 JSON から history を再構築したり、failure を最終テキストに縮約しないでください。

canonical persistence は `provider-message-v3`/`runtime-history-record-v3` を使用し、
portable 要約は native record を置換しません。optional control は呼出元が選び、暗黙に clamp しません。
bounded call には真正 model fact が必要で、欠落は `LimitUnknown` です。
reservation/charged/held は nullable provider usage と別で、budget 更新、価格、forecast、invoice ではありません。

header-only `examples/provider_example_support.h` は実際の SDK runtime を使用します。
Core-only を含むすべての native ビルドには `SchemaProvider::runtime` が必要です。CMake は、
インストール済み package、明示的な `-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=<sdk-source>`、
または immutable public SDK archive fallback を使います。offline のインストール済み/source SDK では
`-DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` で fetching を止めます。
インストール include root は `include/SchemaProvider` です。interface/capability 検査を行い、
SDK `0.4.0` alpha の interface revision/shared ABI は7です。

保存済み interface-3 model-free 実行では Assembly の実際のローカル A2A member サーバー四つと C++ speaker、
JARVIS CLI synthetic turn とメモリ永続化、Beast strict Core コンパイル・進化・チェックポイント rollback、
専用 mock トポロジー load と retrieval index 再使用・admission、ProgramChat ブラウザー tenant 分離・
generation 置換と SQLite六件・PostgreSQL六件の black-box scenario を確認しました。
下の一覧はこの過去の実行を、インストール済み Python application、A2A peer、Forge helper を含む
[以前の interface-4 local 証拠](../README.md#typed-c-cutover-status)と区別します。
vendor inference、音声、全 Beast live 変種や専用 live multitenant 1,000/32 load は未検証です。
過去の測定は新移行の qualification ではありません。live 実行には鍵、ネットワーク、モデルアクセスと費用が必要です。
鍵、prompt、artifact は非公開に保ち、機密 envelope/native inspection 出力を公開 log に送らないでください。
native archive は認証された owner-private custody であり、暗号化や vendor issuer 認証ではありません。


複数の NeoGraph 機能を組み合わせる recipe です。C++ target は必要な SDK package を指定した NeoGraph ツリーでビルドします。フォルダーのコピーだけでは standalone ビルドになりません。

| クックブック | 内容 |
|---|---|
| [`the-beast/`](the-beast/) | **自己進化型エージェント: 生成・進化・ロールバック。** The Beast は strict な Core JSON を書き、実行前に検証し、`evolve()` で範囲を限定した Core のトポロジーを進化させ、チェックポイントでロールバックする。live、apex、forge、script、arithmetic-evolution の各変種も、同じコンパイラー/検証の境界を保つ。ソースの作成は JavaScript または信頼できる C++ が担い、strict な Core JSON は交換データのまま。 |
| [`ai-assembly/`](ai-assembly/) | マルチペルソナの A2A。国会議員 4 名(それぞれ独自の A2A エンドポイントを持つ)と、法案を並列に配信して票を集計する Speaker。言語をまたぐ構成: C++ のメンバーサーバーと、Python または C++ の Speaker。 |
| [`byo-openai/`](byo-openai/) | 真正な型付き SDK 経路による、Python のプロバイダー準備。互換性と実行証拠は recipe を参照。 |
| [`jarvis/`](jarvis/) | 音声駆動のメタオーケストレーター。任意のローカル ASR/TTS が、4 方向ルーター(chat/direct/delegate/parallel)、MCP ツール、A2A の専門家、保存された会話メモリを包む。ローカルまたはモックでの動作はクラウド不要で、ライブ推論には OpenRouter を使う。 |
| [`minimal-mcp/`](minimal-mcp/) | **モデルも API キーも使わない** MCP クライアントの往復。約 60 行の標準ライブラリだけの stdio サーバーと、`initialize` → `tools/list` → `tools/call` を実行する C++ のハーネス。NeoGraph の MCP クライアントに必要なのはワイヤープロトコルを話すプロセスだけで、相手はどんなものでもよいことを示す。 |
| [`openrouter-provider/`](openrouter-provider/) | 型付きの Python OpenRouter プロバイダーリクエストと、不変の outcome。互換性と実行証拠は recipe を参照。 |
| [`multi_tenant_chatbot/`](multi_tenant_chatbot/) | コンパイルキャッシュを通して、1 つのプロセスが多数の顧客のトポロジーを処理する。モックのバイナリには何も要らず、ライブ版にはキーが必要。 |
| [`self_evolving_chatbot/`](self_evolving_chatbot/) | 顧客ごとにトポロジーを選び、進化させるチャットサーバー。ProgramChat には `--mock` モードがあり、Core のサーバーにはキーが必要。 |
| [`topology-retrieval/`](topology-retrieval/) | サンプル 64 に入力を渡す候補の選択(dense、BM25、順位融合、再ランキング)。`--mock` には `pip install faiss-cpu numpy` が必要。 |
| [`program-coding-swarm/`](program-coding-swarm/) | Program API 上で階層型マルチモジュール・コーディングワークフローを実行する実験的な概念実証（experimental proof of concept）: 並列子ハーネス（`ng.all`）、モジュールごとの予算配分とモデルロール、ホスト所有の検証・レビュー判定ゲート、孫セカンドオピニオンハーネス、SQLite 世代置換とロールバック、`central` と `delegated` の合成モード比較（既定は `--mock`、JSON レポート付きの `--live`）。 |

各クックブックには、作業中に見つかった引っかかりも記録してあります。公開 API の粗い部分を探すのに役立ちます。

## 全 recipe 一覧と証拠の範囲

| Recipe | Status |
|---|---|
| [`ai-assembly/`](ai-assembly/) | 保存済み interface-3 四サーバー/C++ session; 現在のインストール済み Python Speaker を local A2A 0.3/1.0 peer で実行; synthetic abstention はモデル判断ではない |
| [`byo-openai/`](byo-openai/) | Installed SDK4 wheel と公式 OpenAI client を local Chat peer で実行: known/unknown usage、実 tool 関数、八回呼出し上限、hosted-call guard を検証; vendor inference なし |
| [`jarvis/`](jarvis/) | 保存済み interface-3 CLI 永続化/EOF 証拠; 現在の Python MCP tool、LangGraph twin/driver、pybind per-turn/startup 正確性を検証; 新しい性能・音声 qualification ではない |
| [`minimal-mcp/`](minimal-mcp/) | 実際の Python stdlib server を公式 MCP client で実行: handshake/discovery と計算・時刻・demo-weather 呼出しを検証; LLM や実気象サービスなし |
| [`multi_tenant_chatbot/`](multi_tenant_chatbot/) | 専用 mock 1,000 要求・error 0・コンパイル済みトポロジー 3・cache hit 997; isolated host は reference metadata のみ; live 1,000/32 は未検証 |
| [`openrouter-provider/`](openrouter-provider/) | Installed SDK4 native provider と custom HTTP node を local Chat peer で実行: known/unknown usage、保持 history、hosted-call guard を検証; vendor inference なし |
| [`self_evolving_chatbot/`](self_evolving_chatbot/) | 実ブラウザー tenant 分離・generation 置換、SQLite六件・PostgreSQL六件 black-box と明示 host model policy を検証; vendor inference なし |
| [`the-beast/`](the-beast/) | 型付き C++ 移行; 実際の strict Core コンパイル・進化・チェックポイント rollback を実行; 全 live 変種の pass 主張ではない |
| [`topology-retrieval/`](topology-retrieval/) | mock Python ranking/index 再使用と実 C++ registry admission/migration・unknown-key拒否を検証; 外部 pointer は権限ではない |
| [`program-coding-swarm/`](program-coding-swarm/) | 実験的な概念実証（experimental proof of concept）; オフラインのスクリプトプロバイダーで並列子ハーネス、非更新型の予算配分、ホスト検証・判定ゲート、孫セカンドオピニオンハーネス、SQLite 世代置換・ロールバック、central/delegated 合成を検証; live モードには `ANTHROPIC_API_KEY` と `OPENROUTER_API_KEY` が必要 |

Python MCP server、Jarvis CLI/REPL driver、retrieval HTTP client は protocol client で provider binding 実装ではありません。Assembly Python speaker は A2A binding、Jarvis pybind benchmark は移行済み native binding を使います。その実行証拠は上の C++ 実行とは別です。SDK runtime の現在の検証対象は Linux/POSIX であり、macOS/Windows transport の検証は主張しません。live multitenant 1,000/32 は smoke ではありません。

## プロバイダー制御と保持された呼出し制限

`ProviderControls` は family ごとの reasoning、sampling、tool selection を保持します。
Chat は admitted OpenRouter reasoning object、usage/include 制御、代替 model、
Responses は明示的サーバー保持 continuation、verbosity、truncation、include 選択を提供します。
Messages は manual/adaptive/disabled thinking、effort、cache 制御、tool choice、
Gemini は明示的 portable foreign history、thinking level、safety setting、sampling、tool choice を
提供します。admission は非対応 family/origin/model を I/O 前に拒否します。native message には真正 custody が
必要であり、出力 JSON、cursor、portable 投影では作れません。
正確な型は [provider reference](../../docs/reference-en.md) を参照してください。

Forge は low reasoning effort を要求し、ask ごとに300秒 deadline を固定します。完了した空の
`MaxTokens` 応答に text と有効/無効 client call がない場合だけ、現在の出力 cap の二倍で
一回追加呼出しできます。両 outcome と usage を保持し、failure、observer/settlement エラーは
再試行しません。既存 live chatbot 経路 `multi_tenant_chatbot/server_live_llm.cpp` と
`self_evolving_chatbot/server_multi.cpp` の provider timeout は180秒です。
ProgramChat の別の CLI 制限や共有 factory の既定値は変えません。

[番号付き例の契約](../README.md#retained-example-contracts) は Deep Research の制限付き cap ladder と
deadline discovery、例16/28、fork/new-turn、計算したリプレイ節約数、evolution file モード、
A2A 0.x/1.0 snapshot を説明します。このソース更新は、上の保存済み実行を現在の SDK の検証や
live-provider pass に読み替えるものではありません。
