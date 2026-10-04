<!-- neograph-i18n: source=examples/README.md locale=ja source_sha256=c83cf49f9761e11401ae84366e9dce10d62a223481589269b3f729c4b68cc582 -->
# C++ API の例

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

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
`NEOGRAPH_BUILD_LLM=OFF` を含む全 Core ビルドに `SchemaProvider::runtime` が必要です。
CMake 3.20+ は明示 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`、インストール済み runtime package、
固定 public GitHub source archive の順に SDK を選択します。download fallback は既定で有効です。
インストール済み package/明示 source の offline ビルドでは `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` にしてください。
インストール include root は `include/SchemaProvider`。interface/capability 検査を行い、
SDK package は alpha `0.1.0`（interface/shared ABI 4）です。

保存済み interface-3 model-free C++ E2E 実行では番号付き target39個を検証しました。finite offline29個と、
実際の MCP/ACP/A2A/Harness および gRPC graph/checkpoint/tool 経路です。gRPC-vs-JSON-RPC
測定例も実行しましたが返却値を検証せず、behavioral E2E pass には数えません。
live/外部 model 経路22個と無効な Clay GUI は未検証で、公開 vendor 要求や新 grant はありません。
過去の測定は新移行の qualification ではありません。live には鍵、network、model access と費用が必要です。
鍵、prompt、artifact は非公開に保ち、機密 envelope/native 出力を公開 log に送らないでください。
native archive は owner-private 認証 custody で、暗号化や vendor issuer 認証ではありません。

現在の interface-4 証拠は別です。Linux x86_64 で native research recovery、
ToT/Forge/rewrite helper と A2A 0.3/1.0 peer を実行し、インストール済み Python application
11個が分割した実行群で credential-free localhost 要求48個を行いました。
追跡済み evolution file mode と Plan resume も以下のとおり実行しました。
保存済み C++ full suite の再実行ではなく、範囲を限定した local 実行です。
[SDK interface-4 実行記録](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/CONFORMANCE.md#interface-4-execution-record)を参照してください。
新しい hosted vendor、Windows/macOS/ARM64、HTTP/3、sanitizer 検証は主張せず、
remote CI と公開は未完了です。



番号付き例は NeoGraph のエンジン API を扱い、Core と Program の quickstart も含みます。
ほとんどはこのディレクトリの単一ファイルです。Docker Compose を使う
[`26_postgres_react_hitl/`](26_postgres_react_hitl/) もあります。
例をプロジェクトにコピーし、`neograph::core` と必要なコンポーネントにリンクしてください。

## 建てる

デフォルトの CMake 構成では、有効なコンポーネントがサポートする
例をビルドします。Program quickstart と Program ベースの例には
`-DNEOGRAPH_BUILD_PROGRAM=ON` が必要です。gRPC と Python バインディングは
オプションで、対応するオプションを有効にしない限り該当例は省略されます。

```bash
cmake -S . -B build -DNEOGRAPH_BUILD_EXAMPLES=ON
cmake --build build -j$(nproc)
```

Program ベースと A2A の例を含めるには、次のコンポーネントも有効にします。

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
| 13 | [`13_openrouter_responses_sse.cpp`](13_openrouter_responses_sse.cpp) | OpenRouter | 型付き Responses SSE リクエスト、順序付き `sp::Event` 観測と所有された `sp::Outcome`。 |
| 34 | [`34_openrouter_responses_tools_sse.cpp`](34_openrouter_responses_tools_sse.cpp) | OpenRouter | 全七 hosted-tool セクションを型付き SSE で実行し、完全な Outcome と順序付き wire 観測を保持します。 |
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
| 48 | [`48_sqlite_checkpoint.cpp`](48_sqlite_checkpoint.cpp) |オフライン | SQLite `:memory:` checkpoint/resume と thread 分離; file/process-restart 永続性の証拠ではありません。 |

### MCP (モデル コンテキスト プロトコル)

| # |ファイル |セットアップ |それが示すもの |
|---|------|-------|---------------|
| 03 | [`03_mcp_agent.cpp`](03_mcp_agent.cpp) | OpenRouter + MCP HTTP サーバー | ストリーミング可能な HTTP MCP サーバーからツールを検出し、ReAct ループを駆動します。 |
| 22 | [`22_mcp_stdio.cpp`](22_mcp_stdio.cpp) | OpenRouter + Python stdio スクリプト | 03 と同じですが、MCP サーバーは stdin/stdout 上の子サブプロセスで、ネットワークスタックはありません。 |
| 23 | [`23_mcp_multi.cpp`](23_mcp_multi.cpp) | OpenRouter + 2 サーバー | 1 つのエージェント、2 つの MCP サーバー (HTTP + stdio)、ツールを 1 つのリストへ統合し、LLM が両方から透過的に選択します。 |
| 21 | [`21_mcp_fanout.cpp`](21_mcp_fanout.cpp) | MCP HTTP サーバー (LLM なし) | 固定 planner が MCP 呼び出しごとに Send を作り、`make_parallel_group` で同時実行します。モデル呼び出しはありませんが、MCP サーバーへの接続は必要です。 |
| 20 | [`20_mcp_hitl.cpp`](20_mcp_hitl.cpp) | OpenRouter + MCP HTTP サーバー | `interrupt_before` で任意の MCP ツール呼び出しを止め、オペレーターが確認・承認して再開します。 |
| 24 | [`24_mcp_feedback.cpp`](24_mcp_feedback.cpp) | OpenRouter + MCP HTTP サーバー | オペレーターが回答草案を読み、フィードバックを入力します。2 回目の実行はそれを会話コンテキストへ取り込みます。 |

### 非同期、同時実行、パフォーマンス

| # |ファイル |セットアップ |それが示すもの |
|---|------|-------|---------------|
| 27 | [`27_async_concurrent_runs.cpp`](27_async_concurrent_runs.cpp) |オフライン | 3 つのエージェントは、`engine->run_async()` を介して 1 つの `io_context` スレッドでインターリーブ実行されます (3×50 ミリ秒ではなく、ほぼ 50 ミリ秒)。ステージ 4 非同期エンドツーエンド。 |
| 40 | [`40_react_async_streaming.cpp`](40_react_async_streaming.cpp) | OpenRouter | 型付き provider イベントによる非同期 ReAct。text delta は表示用投影であり native history ではありません。 |
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
| 31 | [`31_local_transformer.cpp`](31_local_transformer.cpp) | llama.cpp / vLLM | `http://localhost:8090` の型付き Chat クライアント。モデル重みはエージェントプロセスの外部に置きます。 |

### ショーケース

| # |ファイル |セットアップ |それが示すもの |
|---|------|-------|---------------|
| 11 | [`11_clay_chatbot.cpp`](11_clay_chatbot.cpp) |クレイ+レイリブ (`-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON`) | Clay/Raylib UI を使用したマルチターン チャット。 Pure-C++ デスクトップ アプリ、NeoGraph バックエンド。モックまたは`--live`。 |
| 35 | [`35_re_agent.cpp`](35_re_agent.cpp) | OpenRouter + Ghidra + ghidra-mcp | リバースエンジニアリングエージェント。Ghidra でシンボルを除去したバイナリの関数名と概要を復元します。過去のエンドツーエンド結果は、6 関数の crackme で matched_score 0.92 でした。完全なパイプラインは別の非公開リポジトリ `fox1245/re-agent` で管理されています。 |
| 36 | [`36_classifier_fanout.cpp`](36_classifier_fanout.cpp) |オフライン | 5 つの小さな「分類子」 (感情 / 毒性 / 言語 / トピック / 意図) が Send を介して展開され、並行して実行されます。経過時間 ≈ 合計ではなく最大 (分類子ごと) — 小規模モデルのエッジ ストーリー。 DistilBERT/MiniLM パスの 5 ms レイテンシの代用を模擬します。インライン `[ONNX SWAP-IN]` ブロックは、`Ort::Session` を使用した 30 行の置換を示します。推論ランタイムの依存関係はありません。 |

例 35 には、`bridge_mcp_ghidra.py` スクリプトのパスを指定する `GHIDRA_MCP_BRIDGE` が必要です。`GHIDRA_MCP_PYTHON` はインタープリターを選択し、既定値は `python3` です。`GHIDRA_SERVER_URL` はプラグインのエンドポイントを選択し、既定値は `http://127.0.0.1:18080/` です。実行前に Ghidra と MCP プラグインを起動してください。この例には `OPENROUTER_API_KEY` も必要で、有料モデルを呼び出します。上のスコアは過去の観測値であり、新しい実行結果や一般的な精度保証ではありません。

## 保持された例の契約

このソース契約は SDK interface 4 を使います。上の C++ 実行記録は過去の証拠であり、
interface-4 の検証や新しい live 呼出しではありません。family ごとの制御は
`ProviderControls` の閉じた型付きフィールドで、非対応 family/origin/model は I/O 前に拒否します。
reasoning/sampling/tool 制御、Responses のサーバー保持 cursor、deployment header、
明示的 portable Gemini history は [provider reference](../docs/reference-en.md) を参照してください。
cursor と portable history は native replay 権限を与えません。

### 研究と遅い推論経路

Deep Research (25 / 26) は supervisor、researcher、compression、final-report の各要求で、
完了した空の `MaxTokens` outcome に visible text と有効/無効な client tool call がない場合だけ、
最大二回の追加 semantic call を許可します。出力 cap は倍増し、16,384 を超えません。
research-brief 呼出しはこの ladder の対象外です。追加呼出しは新しい ordinal を使い、
元の bank の admission を通り、outcome と usage を保持します。grant、hold、deadline は更新しません。
明示 deadline がなければ、effect のない一回の preparation で設定済み deadline を取得し、
mediated invoke 前に解放して deadline を固定します。明示 deadline はこの取得を省略します。
Failure、observer/settlement エラー、既に配信した streaming part は追加呼出しを起こしません。

空の最終報告はエラーです。内容のある `MaxTokens` 報告は public 投影だけに `Incomplete` を
付け、不変 outcome は変更せず、部分テキストも再試行しません。空の compression が ladder を
使い切ると diagnostic を返し、成功した provider result を捏造しません。

例16は完了した空応答だけで最大三回呼び、cap 8,192 と ask ごとの 300秒 deadline を維持します。
cap を倍増せず、failure も再試行しません。例28の rewrite は low effort と出力512 token を要求し、
空/空白応答なら元の質問をそのまま返します。provider timeout は180秒です。
この経路別設定は共有 factory の既定値を変えません。

### チェックポイントと進化

例08は terminal checkpoint を fork して新しい user turn を始める既存の流れを保持します。
一時停止した reviewer の resume 例ではありません。例14は実際の executor 回数から節約数を
計算します。最初の五回の後、失敗 sibling 一つだけを再実行すると四回を節約します。

例54は smoke/file モードを選ぶ前に `pnoop` を登録します。repository root から
追跡済み seed/task ファイルを使ってください:

```bash
./build/example_evolution --smoke
./build/example_evolution examples/54_evolution_seed.json examples/54_evolution_task.json
```

実際の JSON の `best.compiled`、`best.validated`、`best.executed`、`best.correct` を確認してください。
`compile_passed` だけでは正しい実行の証拠になりません。file モードにも built-in node type と
この demo の `pnoop` があり、custom type には host 登録が必要です。
現在の Linux x86_64 file mode 実行では、この seed/task pair の四つの `best` flag がすべて true でした。
未登録 node type は成功した compile/execute/correct 結果なしに失敗しました。

### A2A dialect と task snapshot

例37は card の interface と最初の RPC で選んだ dialect を表示します。client は互換性のある
JSON-RPC 0.x/1.0 card interface を選び、card URL は設定済み RPC endpoint を変更しません。
card を fetch していなければ数値 `-32601` の場合だけ初期 dialect probe が可能で、
SSE 配信後は再送しません。例38は両 dialect を広告し、初期/更新 task snapshot を表示します。
初期 task は完了した回答ではありません。server の応答 encoding は method 表記とは別に
`A2A-Version` header が選択します。1.0 は PascalCase method、flat part、
`returnImmediately` を使い、stream の status/artifact update を累積します。caller の回答は
完了/中断 agent status text、最初の artifact text、最後の agent history text の順に選びます。

## メンタル モデル — 3 層、真ん中に JSON

各例は、次の 3 つのセットアップの 1 つです。

1. **組み込みノードのみ** (02、04、07、14): `llm_call` / `tool_dispatch`
   / モックプロバイダーノード — グラフ全体が JSON から接続されています。
   サブクラス化。 `create_react_graph()` が生成するものに最も近い。
2. **カスタム `GraphNode` サブクラス** (05、09、10、25): を制御します。
   正確な `run(NodeInput)` ボディ — `ChannelWrite`、`Send`、または
   `Command` から `NodeOutput`。ここでファンアウトを送信し、
   コマンド ルーティングはライブでオーバーライドされます。
3. **型付き provider リクエストと Outcome** (13, 15, 16, 17): SDK admission、順序付きイベント、不変 Outcome を使用します。descriptor interpreter や WebSocket adapter はありません。

グラフ定義は JSON 形式 (`std::map<std::string, json>`) です。
[Python examples](../bindings/python/examples/) も同じ topology 形式を使います。
provider リクエストと Outcome は topology JSON とは別の型付きオブジェクトです。

## APIキーエコノミー

|プロバイダー |例 |
|---|---|
| `OPENROUTER_API_KEY` | 01、03、12、13、15、16、17、18、19、20、22、23、24、25、28、29、30、34、35、40 |
| ローカルサーバー (キーなし) | 31 |
| **なし** | 02、04、05、06、07、08、09、10、14、21、27、36、37、38、39、41、42、43、44、46、47、48、49、50、51、52、53、54、55、 56、57 |

例 25 と 26 は local Crawl4AI も使用します。現在の secure Docker image は
空でない `CRAWL4AI_API_TOKEN` を必要とします。例 26 の `.env.example` を参照してください。

31 例は API key なしで実行できます。例 21 は固定 MCP planner、
27 は `steady_timer` で model latency を代用し、token を使わず engine 動作を示します。
gRPC 例 (52–55, 57) も key 不要ですが、`-DNEOGRAPH_BUILD_GRPC=ON` と `grpc++`/`protoc` が必要です。
56 (`history_compaction`) は既定で mock provider を使い、key がある場合のみ OpenRouter に接続します。

## CMake 構成後の再実行

ビルドされたバイナリは、ビルド ディレクトリのルートに配置されます。
`example_<short_name>` (例: `example_react_agent`、
`example_custom_graph`）。正確な名前は各 `.cpp` の上部にあります
`Usage:` の下にコメントします。

## Responses inspection 契約 (13 / 29 / 30 / 34)

13 は型付き Responses streaming request で、イベントは文字列 callback ではありません。
29 は成功/失敗の完全な Outcome、`wire_envelope`、順序付き全 `wire_output` item と型付き part、
function argument、citation、reasoning、opaque hosted output、artifact を保持します。
raw inspection 出力は機密であり、安全な telemetry/export 形式ではありません。
30 は `none`、`low`、`medium`、`high` を sweep し完全な Outcome を保持します。
reasoning/input/output/total/provider-reported-total と extra count は nullable 64-bit evidence で、
stage/quality/conflict を含みます。欠落は zero ではなく、visible text や予約量は usage ではありません。
34 の七セクションは function(calculator)、web search、image generation、file search、
tool search、`shell.environment` の skills、shell(`container_auto`) です。
`OPENROUTER_VECTOR_STORE_ID` は file search 条件、`OPENROUTER_SKILL_ID` は既定
`openai-spreadsheets` skill を置換します。この inspection demo は function tool を宣言するだけでローカル実行しません。
順序付き型付き/raw event と完全な terminal を保持します。hosted tool は route によって未対応または追加費用となり、
型付き admission は live 互換性保証ではありません。WebSocket/primitive 実行例は残しません。
Images/Veo/Decisions は別の型付き NeoGraph client で、chat spending grant を継承しません。
