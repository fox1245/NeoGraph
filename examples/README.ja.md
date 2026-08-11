<!-- neograph-i18n: source=examples/README.md locale=ja source_sha256=bdd60f74da6b396e20b442bafa8e8479ebeeced9e8bef17caf8366a89ae4bf7e -->
# C++ API の例

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

P8 カットオーバー一覧: [`spec/neograph-example-disposition-v1.json`](../spec/neograph-example-disposition-v1.json)。

NeoGraph エンジンの表面をカバーする 56 個の実行可能な C++ プログラム。
それぞれがこのディレクトリ内の 1 つのファイルです (Docker-Compose が 1 つあります)
例外、[`26_postgres_react_hitl/`](26_postgres_react_hitl/)) — コピー
1 つを自分のプロジェクトに追加し、`neograph::core` + にリンクします
`neograph::llm`、これで出発点が決まりました。

## 建てる

デフォルトの CMake 構成では、有効なコンポーネントがサポートする
例をビルドします。Program quickstart と Program ベースの例には
`-DNEOGRAPH_BUILD_PROGRAM=ON` が必要です。gRPC と Python バインディングは
オプションで、対応するオプションを有効にしない限り該当例は省略されます。

```bash
cmake -S . -B build -DNEOGRAPH_BUILD_EXAMPLES=ON
cmake --build build -j$(nproc)
```

完全な C++ 例セットをビルドするには、Program と A2A も有効にします。

```bash
cmake -S . -B build \
  -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_A2A=ON
cmake --build build -j$(nproc)
```

例をスキップするには `-DNEOGRAPH_BUILD_EXAMPLES=OFF` を渡します。追加の
依存関係 (Crawl4AI Docker、Postgres、MCP サーバー、Clay+Raylib) が必要な例は、
明示的な CMake オプションまたはランタイムプローブで制御されます。
以下の「セットアップ」列を参照してください。

## 設定

実際の LLM にアクセスする例は cppdotenv 経由で cwd (または任意の親ディレクトリ) から
`.env` を自動ロードします。ライブ例は 1 つのキーを使います。

```
OPENROUTER_API_KEY=sk-or-...
```

以下の「Setup」エントリのない例では API キーは必要ありません。
インプロセス `MockProvider` または純粋なモック ノード。

## ここから始めましょう

初めての場合:

|最初 |学ぶこと |
|---|---|
| [`62_core_quickstart.cpp`](62_core_quickstart.cpp) | **Core クイックスタート** — インストール済み `neograph::core` ターゲットで厳密なグラフと型付きチャンネルを実行します。オプション コンポーネント/API キー不要。 |
| [`63_program_quickstart.cpp`](63_program_quickstart.cpp) | **Program クイックスタート** — インストール済み `neograph::program` ターゲットで `call_core` Program をコンパイル、admission、実行します。`-DNEOGRAPH_BUILD_PROGRAM=ON` が必要です。 |
| [`51_minimal.cpp`](51_minimal.cpp) |最小の動作プログラム — `result.channel<T>("name")` をビルド、実行、読み取ります。 APIキーがありません。 |
| [`02_custom_graph.cpp`](02_custom_graph.cpp) | JSON グラフ定義を構築し、実行します。 APIキーがありません。 |
| [`05_parallel_fanout.cpp`](05_parallel_fanout.cpp) | `make_parallel_group` による非同期ファンアウト。 APIキーがありません。 |
| [`10_send_command.cpp`](10_send_command.cpp) | `Send` (動的ファンアウト) + `Command` (ルーティング オーバーライド)。 APIキーがありません。 |
| [`01_react_agent.cpp`](01_react_agent.cpp) |実際の LLM + 計算ツールを使用した ReAct ループ。 **`OPENROUTER_API_KEY` が必要です。** |
| [`14_plan_executor.cpp`](14_plan_executor.cpp) |計画→並列サブタスク→ソルバー、チェックポイント ストアによるクラッシュ回復。 APIキーがありません。 |

これらが意味をなすと、以下の残りは内容ごとにグループ化されます。
ファイル番号ではなく、実演してください。

## 索引

### コア エンジン — グラフ、状態、ルーティング

| # |ファイル |セットアップ |それが示すもの |
|---|------|-------|---------------|
| 02 | [`02_custom_graph.cpp`](02_custom_graph.cpp) |オフライン | JSON グラフを構築して実行します。このリポジトリで最も短い便利なプログラム。 |
| 05 | [`05_parallel_fanout.cpp`](05_parallel_fanout.cpp) |オフライン |非同期ファンアウト — 3 つの「researcher」ノードが 1 つの io_context 上で同時実行され、サマライザがそれらをファンインします。 |
| 06 | [`06_subgraph.cpp`](06_subgraph.cpp) |オフライン |階層構成 — 外側のスーパーバイザー グラフは内側の ReAct サブグラフに委譲します。 |
| 07 | [`07_intent_routing.cpp`](07_intent_routing.cpp) |オフライン |分類器→条件付きエッジ→数学/翻訳/一般的な専門家。 |
| 08 | [`08_state_management.cpp`](08_state_management.cpp) |オフライン | `get_state` / `update_state` / `fork` — C++ にマップされた LangGraph のチェックポインター API。 |
| 09 | [`09_all_features.cpp`](09_all_features.cpp) |オフライン | 1 つのデモに 6 つの機能 — `NodeInterrupt`、`RetryPolicy`、`StreamMode`、`Send`、`Command`、`Store`。 |
| 10 | [`10_send_command.cpp`](10_send_command.cpp) |オフライン | Planner→Send→researcher→Command(loop|finish) — 標準的な Send+Command パターン。 |
| 42 | [`42_custom_reducer_condition.cpp`](42_custom_reducer_condition.cpp) |オフライン | C++ からカスタム チャネル リデューサーとエッジ条件を登録します。エンジンに触れることなく、JSON ボキャブラリーを拡張します。 |
| 43 | [`43_store_personalization.cpp`](43_store_personalization.cpp) |オフライン |クロススレッド `Store` は、`in.ctx.store` を介してノード内から到達しました。共有名前空間メモリからのユーザーごとのノードの動作です。 |
| 51 | [`51_minimal.cpp`](51_minimal.cpp) |オフライン |最も短い動作プログラム — ビルド、実行、`result.channel<T>("name")`。新しいユーザーのテンプレート。 |
| 52 | [`52_export_schema.cpp`](52_export_schema.cpp) |オフライン | `NodeFactory::export_schema()` → トポロジ JSON スキーマ ダンプ。コードレスビジュアルエディターがパレットを構築する、バージョンがロックされた信頼できる情報源。 |
| 56 | [`56_history_compaction.cpp`](56_history_compaction.cpp) |オフライン (オプションの OpenRouter) |境界付きメッセージ ウィンドウ - 履歴が予算を超えると、削除されたプレフィックスが LLM によって作成された概要に置き換えられます。デフォルトではモックプロバイダー。 |

### 本物の LLM — プロバイダー、ツール、ReAct

| # |ファイル |セットアップ |それが示すもの |
|---|------|-------|---------------|
| 01 | [`01_react_agent.cpp`](01_react_agent.cpp) | OpenRouter | ReAct ループ: `llm_call` ↔ `tool_dispatch` (`has_tool_calls` 条件付き)。計算ツール。 |
| 12 | [`12_rag_agent.cpp`](12_rag_agent.cpp) | OpenRouter | OpenRouter 互換 embedding + メモリ内コサイン検索を備えた RAG。 |
| 13 | [`13_openai_responses.cpp`](13_openai_responses.cpp) | OpenRouter | 同じ ReAct ループですが、OpenRouter の `/api/v1/responses` に `SchemaProvider("openai_responses")` 経由で接続します。 |
| 33 | [`33_openai_responses_ws.cpp`](33_openai_responses_ws.cpp) | OpenRouter | Responses 互換 transport の切り替え例。再現可能な実行経路は HTTP/SSE です。 |
| 34 | [`34_openai_responses_ws_tools.cpp`](34_openai_responses_ws_tools.cpp) | OpenRouter | Responses 互換の組み込みツール wire shape を紹介します。 |
| 29 | [`29_responses_envelope.cpp`](29_responses_envelope.cpp) | OpenRouter | デバッグ支援: 1 つのツール呼び出しリクエストの生の `/api/v1/responses` JSON envelope をダンプします。 |
| 30 | [`30_reasoning_effort.cpp`](30_reasoning_effort.cpp) | OpenRouter | 固定 DeepSeek モデルで reasoning effort のレイテンシー / reasoning token のトレードオフを確認します。 |

### 推論パターン

| # |ファイル |セットアップ |パターン |
|---|------|-------|---------|
| 15 | [`15_reflexion.cpp`](15_reflexion.cpp) |人類 |反射 — 批評家が「受け入れます」と言うまで生成者 ↔ 批評家がループします (Shinn et al. 2023)。俳句制約タスク。 |
| 16 | [`16_tree_of_thoughts.cpp`](16_tree_of_thoughts.cpp) |人類 |思考のツリー — 各深さで、N 個の候補の思考を生成し、それらをスコア化し、上位 K を維持し、展開します。 24 のゲーム。 |
| 17 | [`17_self_ask.cpp`](17_self_ask.cpp) |人類 |自問 - 明確な「フォローアップの質問は必要ですか?」マルチホップ推論のための分解 (Press et al. 2022)。 |
| 18 | [`18_multi_agent_debate.cpp`](18_multi_agent_debate.cpp) |人類 |研究者 / 懐疑論者 / 裁判官 — 3 つのシステム プロンプト、共有記録証明書、裁判官の判決。 |
| 19 | [`19_rewoo.cpp`](19_rewoo.cpp) |人類 | REWOO — プランナーは `#E1 / #E2` プレースホルダーを使用して完全なプランをコミットし、ワーカーはツールを並行してファンアウトし、ソルバーが合成します。 |

### 永続性と HITL

| # |ファイル |セットアップ |それが示すもの |
|---|------|-------|---------------|
| 04 | [`04_checkpoint_hitl.cpp`](04_checkpoint_hitl.cpp) |オフライン | `interrupt_before` 支払いノード、チェックポイントを保持し、オペレーターの承認後に再開します。モックプロバイダー。 |
| 14 | [`14_plan_executor.cpp`](14_plan_executor.cpp) |オフライン |ファンアウト中障害をシミュレートした Plan-and-Executor - チェックポイントの再実行は、障害が発生した兄弟のみを再実行します。保留中 - 書き込み機構が動作中。 |
| 26 | [`26_postgres_react_hitl/`](26_postgres_react_hitl/) | OpenRouter + Postgres + Crawl4AI | プロセス不連続ディープリサーチ HITL — PG でバックアップされたチェックポイントは、レポートと再開の間の `exit` まで存続します。 Docker-Compose 駆動。 |
| 41 | [`41_resume_if_exists_chat.cpp`](41_resume_if_exists_chat.cpp) |オフライン | LangGraph スタイルのマルチターン チャット — `resume_if_exists` は前のチェックポイントをリロードし、新しいターンを追加します。モックプロバイダー。 |
| 48 | [`48_sqlite_checkpoint.cpp`](48_sqlite_checkpoint.cpp) |オフライン | `SqliteCheckpointStore` — 単一ファイルの永続実行、サーバーなし。 InMemory/Postgres と同じ `CheckpointStore` インターフェイス。 |

### MCP (モデル コンテキスト プロトコル)

| # |ファイル |セットアップ |それが示すもの |
|---|------|-------|---------------|
| 03 | [`03_mcp_agent.cpp`](03_mcp_agent.cpp) | OpenRouter + MCP HTTP サーバー | ストリーミング可能な HTTP MCP サーバーからツールを検出し、ReAct ループを駆動します。 |
| 22 | [`22_mcp_stdio.cpp`](22_mcp_stdio.cpp) | OpenRouter + Python stdio スクリプト | 03 と同じですが、MCP サーバーは stdin/stdout 上の子サブプロセスで、ネットワークスタックはありません。 |
| 23 | [`23_mcp_multi.cpp`](23_mcp_multi.cpp) | OpenRouter + 2 サーバー | 1 つのエージェント、2 つの MCP サーバー (HTTP + stdio)、ツールを 1 つのリストへ統合し、LLM が両方から透過的に選択します。 |
| 21 | [`21_mcp_fanout.cpp`](21_mcp_fanout.cpp) | MCP HTTP サーバー (LLM なし) | Planner は MCP 呼び出しごとに 1 つの送信を発行します。 `make_parallel_group` はそれらを同時に実行します。決定的 — LLM は、デモが LLM 軸上でオフラインのままとなるようにツールを厳選します。 |
| 20 | [`20_mcp_hitl.cpp`](20_mcp_hitl.cpp) | OpenRouter + MCP HTTP サーバー | `interrupt_before` で任意の MCP ツール呼び出しを止め、オペレーターが確認・承認して再開します。 |
| 24 | [`24_mcp_feedback.cpp`](24_mcp_feedback.cpp) | OpenRouter + MCP HTTP サーバー | オペレーターが回答草案を読み、フィードバックを入力します。2 回目の実行はそれを会話コンテキストへ取り込みます。 |

### 非同期、同時実行、パフォーマンス

| # |ファイル |セットアップ |それが示すもの |
|---|------|-------|---------------|
| 27 | [`27_async_concurrent_runs.cpp`](27_async_concurrent_runs.cpp) |オフライン | 3 つのエージェントは、`engine->run_async()` を介して 1 つの `io_context` スレッドでインターリーブ実行されます (3×50 ミリ秒ではなく、ほぼ 50 ミリ秒)。ステージ 4 非同期エンドツーエンド。 |
| 40 | [`40_react_async_streaming.cpp`](40_react_async_streaming.cpp) | OpenRouter | 外側の `asio::io_context` + `co_spawn` + `co_await engine->run_stream_async(...)` で ReAct ループを駆動し、`SchemaProvider("openai_responses")` 経由のトークンを stdout へストリーミングします。 |
| 44 | [`44_request_queue_backpressure.cpp`](44_request_queue_backpressure.cpp) |オフライン |バックプレッシャー付きの固定ワーカー プール (`neograph::util::RequestQueue`) — 実行中の作業は制限されており、負荷がかかっても無制限に増加することはありません。 |
| 46 | [`46_cancel_token.cpp`](46_cancel_token.cpp) |オフライン |協調キャンセル — 子ごとに `CancelToken::fork()`、親 `cancel()` が飛行中のすべての子にカスケードされます。 |
| 47 | [`47_node_cache.cpp`](47_node_cache.cpp) |オフライン |ノード + 入力をキーとしたノードごとの結果キャッシュ — 実行全体で同一の入力に対する再計算をスキップします。 |
| 50 | [`50_async_tool.cpp`](50_async_tool.cpp) |オフライン | `AsyncTool` — コルーチン形状のツール実行アダプター。ツールは io_context をブロックせずに `co_await` できます。 |

### エージェントの相互運用性 — A2A および ACP

| # |ファイル |セットアップ |それが示すもの |
|---|------|-------|---------------|
| 38 | [`38_a2a_server.cpp`](38_a2a_server.cpp) |オフライン |コンパイルされた NeoGraph をエージェント間エンドポイント (HTTP、ストリーミング SSE) として公開します。まずこれを実行してください。 |
| 37 | [`37_a2a_client.cpp`](37_a2a_client.cpp) |オフライン (サンプル 38 を実行する必要があります) | *リモート* A2A エージェントを駆動する — `A2ACallerNode` は、リモート エージェントをローカル ノードのように見せます。 |
| 39 | [`39_acp_server.cpp`](39_acp_server.cpp) |オフライン | Agent Client Protocol を介して NeoGraph を公開します。標準入出力を介した双方向 JSON-RPC、エディター (Zed スタイル) が駆動する形状です。 |

### 分散 — gRPC サービスとリモート チェックポイント/ツール

`-DNEOGRAPH_BUILD_GRPC=ON` のみでビルドされます (`grpc++` / `protoc` が必要)。

| # |ファイル |セットアップ |それが示すもの |
|---|------|-------|---------------|
| 52 | [`52_grpc_server.cpp`](52_grpc_server.cpp) |オフライン (grpc++) | gRPC 経由で `GraphEngine` を公開します。遅延コンパイルされキャッシュされた個別グラフ エンジンごとに実行されます。 |
| 53 | [`53_grpc_client.cpp`](53_grpc_client.cpp) |オフライン (grpc++) | C++ クライアントから NeoGraph gRPC `GraphService` を呼び出します。 |
| 54 | [`54_grpc_checkpoint.cpp`](54_grpc_checkpoint.cpp) |オフライン (grpc++) | `GrpcCheckpointStore` — ネットワーク境界を越えたリモート `CheckpointStore`、正確な遅延測定。 |
| 55 | [`55_grpc_vs_jsonrpc_toolcall.cpp`](55_grpc_vs_jsonrpc_toolcall.cpp) |オフライン (grpc++) |直接対決: JSON-RPC と gRPC を介したツール呼び出し — 「70× は Nagle アーティファクト」の背後にあるマイクロベンチ。 |
| 57 | [`57_grpc_remote_tool.cpp`](57_grpc_remote_tool.cpp) |オフライン (grpc++) |ローカル `neograph::Tool` として公開される別のプロセスに存在するツール。 |

### 可観測性

| # |ファイル |セットアップ |それが示すもの |
|---|------|-------|---------------|
| 49 | [`49_openinference.cpp`](49_openinference.cpp) |オフライン | OpenInference トレーサー アダプター — `graph.run > node.* > llm.complete` は 1 つのトレース ツリー (12 属性) として配置されます。フェニックス認証済み。モックプロバイダー。 |

### 綿密な研究 / RAG バリアント

| # |ファイル |セットアップ |それが示すもの |
|---|------|-------|---------------|
| 25 | [`25_deep_research.cpp`](25_deep_research.cpp) | OpenRouter DeepSeek + Crawl4AI Docker | `langchain-ai/open_deep_research` の C++ ポート。スーパーバイザーは計画を立て、並行するサブ研究者を展開し、Markdown レポートを統合します。 |
| 28 | [`28_corrective_rag.cpp`](28_corrective_rag.cpp) | OpenRouter | CRAG (Yan et al. 2024)。取得→グレード→関連性に応じて refine(KB) / refine+Web / Web のみにルーティングします。`/api/v1/responses` の組み込みツールで Web 検索します。 |

### ローカル/ハイブリッド LLM バックエンド

| # |ファイル |セットアップ |それが示すもの |
|---|------|-------|---------------|
| 31 | [`31_local_transformer.cpp`](31_local_transformer.cpp) |ローカルサーバー (llama.cpp / vLLM) | `OpenAIProvider` を `http://localhost:8090` に向けます。 2 つのプロセスの分割により、モデルの重みがエージェントのアドレス空間から外されます。 |

### ショーケース

| # |ファイル |セットアップ |それが示すもの |
|---|------|-------|---------------|
| 11 | [`11_clay_chatbot.cpp`](11_clay_chatbot.cpp) |クレイ+レイリブ (`-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON`) | Clay/Raylib UI を使用したマルチターン チャット。 Pure-C++ デスクトップ アプリ、NeoGraph バックエンド。モックまたは`--live`。 |
| 35 | [`35_re_agent.cpp`](35_re_agent.cpp) | OpenRouter + ギドラ + ギドラ-mcp | リバースエンジニアリング エージェント — Ghidra を介して、ストリップされたバイナリから関数名と概要を復元します。エンドツーエンドで検証済み (matched_score 0.92、6-fn crackme)。 |
| 36 | [`36_classifier_fanout.cpp`](36_classifier_fanout.cpp) |オフライン | 5 つの小さな「分類子」 (感情 / 毒性 / 言語 / トピック / 意図) が Send を介して展開され、並行して実行されます。経過時間 ≈ 合計ではなく最大 (分類子ごと) — 小規模モデルのエッジ ストーリー。 DistilBERT/MiniLM パスの 5 ms レイテンシの代用を模擬します。インライン `[ONNX SWAP-IN]` ブロックは、`Ort::Session` を使用した 30 行の置換を示します。推論ランタイムの依存関係はありません。 |

## メンタル モデル — 3 層、真ん中に JSON

各例は、次の 3 つのセットアップの 1 つです。

1. **組み込みノードのみ** (02、04、07、14): `llm_call` / `tool_dispatch`
   / モックプロバイダーノード — グラフ全体が JSON から接続されています。
   サブクラス化。 `create_react_graph()` が生成するものに最も近い。
2. **カスタム `GraphNode` サブクラス** (05、09、10、25): を制御します。
   正確な `run(NodeInput)` ボディ — `ChannelWrite`、`Send`、または
   `Command` から `NodeOutput`。ここでファンアウトを送信し、
   コマンド ルーティングはライブでオーバーライドされます。
3. **Schema-driven response shapes** (13、15、16、17、33):
   1 つの JSON スキーマがワイヤー形状を記述し、ライブ例はすべて
   同じ OpenRouter provider と固定 DeepSeek モデルを使用します。

グラフ定義はJSON形式(`std::map<std::string, json>`)
どちらの方法でも — [Python examples](../bindings/python/examples/) の例 14 と 15
同じ定義が `json.dumps` を往復する様子を示します。

## APIキーエコノミー

|プロバイダー |例 |
|---|---|
| `OPENROUTER_API_KEY` | 01、03、12、13、15、16、17、18、19、20、22、23、24、25、28、29、30、33、34、35、40 |
| ローカルサーバー (キーなし) | 31 |
| **なし** | 02、04、05、06、07、08、09、10、14、21、27、36、37、38、39、41、42、43、44、46、47、48、49、50、51、52、53、54、55、 56、57 |

31 個のサンプルは API キーなしで実行されます。つまり、「タイヤを蹴る」です。
床。例 21 (MCP ファンアウト、決定的プランナー) および 27 (非同期)
同時実行性、LLM レイテンシの代用 `steady_timer`)、特に
トークンを使わずにエンジン配管のデモンストレーションを行います。 gRPC スイート
(52–55、57) もキー不要ですが、`-DNEOGRAPH_BUILD_GRPC=ON` が必要です
(`grpc++` / `protoc`); 56 (`history_compaction`) のデフォルトはモックです
プロバイダーであり、キーが存在する場合にのみ OpenRouter にアクセスします。

## CMake 構成後の再実行

ビルドされたバイナリは、ビルド ディレクトリのルートに配置されます。
`example_<short_name>` (例: `example_react_agent`、
`example_custom_graph`）。正確な名前は各 `.cpp` の上部にあります
`Usage:` の下にコメントします。
