<!-- neograph-i18n: source=examples/README.md locale=ja source_sha256=8d5870bdce1ab6c9f3b5e508b7487efc8c8478ee8cb58e24807b494414025413 -->
# C++ API の例

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

各サンプルは 1 ファイルで 1 つの事柄を示し、上から下まで数分で読めます。多くはオフラインで動きます。プロバイダーの代わりにスクリプト化したモデルが応答するため、API キーもネットワークも不要です。実際のモデルも呼べるサンプルは `--live` を受け付けます。1 つを自分のプロジェクトにコピーし、`neograph::core`(モデルを使うものは `neograph::llm` も)をリンクすれば、動くコードから始められます。

**Offline** 列は、各行が必要とするものを示します。`yes`: キーもネットワークも不要で、自動のスモークテスト(`ctest -L examples-offline`)があります。`key`: プロバイダーの API キーが必要です。`service`: 別のプロセス、サーバー、データベース、ディスプレイ、またはモデルが必要です。`paired`: その行に書かれたサンプルと一緒に、オフラインで動きます。

## ビルドと実行

```bash
cmake -S . -B build -DNEOGRAPH_BUILD_EXAMPLES=ON -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_TESTS=ON
cmake --build build -j$(nproc)
./build/example_react_agent            # scripted model: no key, no network
./build/example_react_agent --live     # real model: OPENROUTER_API_KEY must be set in the environment
ctest --test-dir build -L examples-offline --output-on-failure   # run every offline example and check its output
```

バイナリはビルドディレクトリに `example_<名前>` として作られます。多くのソースファイルは、先頭の `Usage:` 行に正確なコマンドを書いています。Program を使うサンプル(63、64)には `-DNEOGRAPH_BUILD_PROGRAM=ON` が必要です。gRPC のサンプル(45、53、54、55、57)には `-DNEOGRAPH_BUILD_GRPC=ON` と `grpc++`/`protoc` が必要です。サンプル 11 には `-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON` が必要です。コンポーネントを無効にしたサンプルはビルドから外れるだけで、ビルドを失敗させません。

## ここから始める

順番に読んでください。1 つ進むごとに考え方が 1 つ増えます。

| 手順 | サンプル | Offline | 学ぶこと |
|---|---|---|---|
| 1 | [`62_core_quickstart.cpp`](62_core_quickstart.cpp) | yes | インストール済みの `neograph::core` ターゲット。ノード 1 つ、厳密なグラフ 1 つ、型付きチャンネル 1 つ。 |
| 2 | [`01_react_agent.cpp`](01_react_agent.cpp) | yes | モデル、ツール、ループ。`llm::Agent` が電卓ツール付きの ReAct エージェントを実行し、テキストをストリーミングします。 |
| 3 | [`05_parallel_fanout.cpp`](05_parallel_fanout.cpp) | yes | `make_parallel_group` で 1 つの `io_context` 上に並列の fan-out と fan-in を行います。 |
| 4 | [`10_send_command.cpp`](10_send_command.cpp) | yes | `Send` で実行時に作業を作り、`Command` で経路を上書きします。planner、researcher、evaluator がループします。 |
| 5 | [`04_checkpoint_hitl.cpp`](04_checkpoint_hitl.cpp) | yes | `interrupt_before` で支払いノードの手前で止め、チェックポイントを保存し、承認後に再開します。 |
| 6 | [`63_program_quickstart.cpp`](63_program_quickstart.cpp) | yes | `neograph::program` で `call_core` Program を 1 つコンパイルし、admission を通して実行します(`-DNEOGRAPH_BUILD_PROGRAM=ON` が必要)。 |

## やりたいことから探す

目的を選んで、そのサンプルを読んでください。

| やりたいこと | 読むサンプル | Offline |
|---|---|---|
| 最初のグラフを動かす | [`62_core_quickstart.cpp`](62_core_quickstart.cpp) | yes |
| モデルにツールを渡し、答えが出るまでループさせる | [`01_react_agent.cpp`](01_react_agent.cpp) | yes |
| ステップを並列に実行して結果を合流させる | [`05_parallel_fanout.cpp`](05_parallel_fanout.cpp) | yes |
| 実行時に作業を作り、元へ戻る | [`10_send_command.cpp`](10_send_command.cpp) | yes |
| リクエストを適切な専門家へ振り分ける | [`07_intent_routing.cpp`](07_intent_routing.cpp) | yes |
| グラフの中に別のグラフを入れる | [`06_subgraph.cpp`](06_subgraph.cpp) | yes |
| 人の判断を待って止め、再開する | [`04_checkpoint_hitl.cpp`](04_checkpoint_hitl.cpp) | yes |
| 保存した状態を読み、編集し、フォークする | [`08_state_management.cpp`](08_state_management.cpp) | yes |
| クラッシュ後に完了済みの作業をやり直さず復旧する | [`14_plan_executor.cpp`](14_plan_executor.cpp) | yes |
| 呼び出しをまたいでチャットを続ける | [`41_resume_if_exists_chat.cpp`](41_resume_if_exists_chat.cpp) | yes |
| スレッド間でメモリを共有する | [`43_store_personalization.cpp`](43_store_personalization.cpp) | yes |
| 長い履歴を予算内に収める | [`56_history_compaction.cpp`](56_history_compaction.cpp) | yes |
| 別スレッドから実行をキャンセルする | [`46_cancel_token.cpp`](46_cancel_token.cpp) | yes |
| ノードの再計算を省く | [`47_node_cache.cpp`](47_node_cache.cpp) | yes |
| OpenInference バックエンドへトレースを送る | [`49_openinference.cpp`](49_openinference.cpp) | yes |
| Program をコンパイルして実行する | [`63_program_quickstart.cpp`](63_program_quickstart.cpp) | yes |
| 実行中の Program をより良いトポロジーへ移す | [`64_topology_retrieval.cpp`](64_topology_retrieval.cpp) | yes |
| シードから harness を進化させる | [`32_evolution.cpp`](32_evolution.cpp) | yes |
| A2A でグラフを他のエージェントへ公開する | [`38_a2a_server.cpp`](38_a2a_server.cpp) | yes |
| リモートの A2A エージェントを呼び出す | [`37_a2a_client.cpp`](37_a2a_client.cpp) | yes |
| ACP でグラフをエディターへ公開する | [`39_acp_server.cpp`](39_acp_server.cpp) | yes |
| MCP サーバーとしてツールを提供する | [`59_mcp_server.cpp`](59_mcp_server.cpp) | yes |
| ReAct ループの中で MCP ツールを使う | [`22_mcp_stdio.cpp`](22_mcp_stdio.cpp) | key |
| gRPC で別プロセスのツールを呼び出す | [`57_grpc_remote_tool.cpp`](57_grpc_remote_tool.cpp) | yes |
| Reflexion などの推論パターンを試す | [`15_reflexion.cpp`](15_reflexion.cpp) | key |
| ローカルのモデルサーバーに対して実行する | [`31_local_transformer.cpp`](31_local_transformer.cpp) | service |

## 段階別の索引

### はじめにと Core エンジン

残りの Core 機能です。`neograph::core` だけをリンクしますが、スクリプト化したプロバイダーを使う 02、06、07、09 は `neograph::llm` もリンクします。

| # | サンプル | Offline | 示すもの |
|---|---|---|---|
| 02 | [`02_custom_graph.cpp`](02_custom_graph.cpp) | yes | `create_react_graph()` の 1 回の呼び出しで llm、tools、llm の ReAct グラフを作り、`run_stream` がノードイベントを出力します。 |
| 05 | [`05_parallel_fanout.cpp`](05_parallel_fanout.cpp) | yes | fan-out と fan-in。3 つの researcher ノードが同時にタイマーを待ち（カウンターで確認します）、barrier が合流させて summarizer に渡します。 |
| 06 | [`06_subgraph.cpp`](06_subgraph.cpp) | yes | サブグラフ。外側の supervisor グラフが内側の ReAct グラフに処理を任せます。 |
| 07 | [`07_intent_routing.cpp`](07_intent_routing.cpp) | yes | 分類ノードと条件付きエッジ(`route_channel`)が、各リクエストを math、translate、general の専門家へ振り分けます。 |
| 09 | [`09_all_features.cpp`](09_all_features.cpp) | yes | 1 ファイルに 6 つの機能: `NodeInterrupt`、`RetryPolicy`、`StreamMode`、`Send`、`Command`、`Store`。 |
| 10 | [`10_send_command.cpp`](10_send_command.cpp) | yes | `Send` による fan-out と `Command` による経路の上書き。十分な結果が揃うまで planner、researcher、evaluator が繰り返します。 |
| 42 | [`42_custom_reducer_condition.cpp`](42_custom_reducer_condition.cpp) | yes | C++ から独自のチャンネル reducer とエッジ条件を登録します。 |
| 51 | [`51_minimal.cpp`](51_minimal.cpp) | yes | チャンネル。ノード 1 つが 1 つのチャンネルを読んで 2 つに書き込み、呼び出し側は型付きの `ChannelKey` で両方を読み戻します（`result.channel(key)`、`try_channel`）。 |
| 52 | [`52_export_schema.cpp`](52_export_schema.cpp) | yes | `NodeFactory::export_schema()` が、ビジュアルエディターがパレットを作るためのトポロジー JSON Schema を出力します。 |
| 61 | [`61_sync_node.cpp`](61_sync_node.cpp) | yes | `SyncGraphNode`。コルーチンの定型コードが要らない短いノード 2 つ（空白の除去、大文字化）。ブロッキング I/O は引き続き `GraphNode::run()` に書きます。 |
| 100 | [`100_stream_modes.cpp`](100_stream_modes.cpp) | yes | `StreamMode`。同じ実行を VALUES、UPDATES、EVENTS、DEBUG の各モードで観察し、モードごとに届くイベントを数えます。 |
| 101 | [`101_retry_policy.cpp`](101_retry_policy.cpp) | yes | `RetryPolicy`。ノードが 2 回失敗すると、エンジンが指数バックオフで再試行します。回数を減らすと、再試行を使い切ったときに呼び出し側が受け取るものが分かります。 |
| 102 | [`102_node_interrupt.cpp`](102_node_interrupt.cpp) | yes | `NodeInterrupt`。ノード自身が実行を止め、ペイロードが呼び出し側に尋ねる内容を伝え、`resume` が答え（承認または拒否）をノードに返します。 |
| 103 | [`103_max_steps.cpp`](103_max_steps.cpp) | yes | `max_steps`。ループがステップ上限で止まり、結果がそれを知らせます（`max_steps_exhausted()`、`RunStatus::StepLimit`）。最後のステップで自然に終わった実行には印が付きません。 |

### エージェントパターン

モデルとツールをループで組み合わせた構成と、その上に作った公開済みの推論パターンです。`key` の行は実際のモデルを呼び出します。

| # | サンプル | Offline | 示すもの |
|---|---|---|---|
| 01 | [`01_react_agent.cpp`](01_react_agent.cpp) | yes | ReAct。Thought の 1 行、ツール呼び出し、Observation、最終回答。`--live` で実際のモデルを呼びます。 |
| 36 | [`36_classifier_fanout.cpp`](36_classifier_fanout.cpp) | yes | 5 つの小さな分類器を `Send` で fan-out します。所要時間は合計ではなく最も遅い 1 つ分です。判定結果は固定値です。 |
| 15 | [`15_reflexion.cpp`](15_reflexion.cpp) | key | Reflexion。critic が受け入れるまで actor と critic が繰り返します(Shinn et al. 2023)。 |
| 16 | [`16_tree_of_thoughts.cpp`](16_tree_of_thoughts.cpp) | key | Tree of Thoughts。候補となる思考を生成し、採点し、残し、展開します。 |
| 17 | [`17_self_ask.cpp`](17_self_ask.cpp) | key | Self-Ask。マルチホップ推論のための明示的な追加質問(Press et al. 2022)。 |
| 18 | [`18_multi_agent_debate.cpp`](18_multi_agent_debate.cpp) | key | researcher、skeptic、judge。1 つの共有トランスクリプトに対して 3 つのシステムプロンプトが働きます。 |
| 19 | [`19_rewoo.cpp`](19_rewoo.cpp) | key | REWOO。planner がプレースホルダーを書き、worker がツールを並列実行し、solver が証拠をまとめます。 |
| 12 | [`12_rag_agent.cpp`](12_rag_agent.cpp) | key | 実際の埋め込みとメモリ内コサイン検索を使う検索拡張生成。 |
| 28 | [`28_corrective_rag.cpp`](28_corrective_rag.cpp) | key | Corrective RAG。検索結果を評価し、洗練する、Web 検索を足す、Web 検索のみにする、のいずれかを選びます(Yan et al. 2024)。 |
| 25 | [`25_deep_research.cpp`](25_deep_research.cpp) | key | Deep research。supervisor が、それぞれ ReAct ループを回す下位 researcher を fan-out し、レポートを書きます。`OPENROUTER_API_KEY` と Crawl4AI サーバーが必要です。 |
| 40 | [`40_react_async_streaming.cpp`](40_react_async_streaming.cpp) | key | 非同期 ReAct。外側のコルーチンが `run_stream_async` を待ち、型付きのテキスト差分を出力します。 |
| 13 | [`13_openrouter_responses_sse.cpp`](13_openrouter_responses_sse.cpp) | key | 順序付きの `sp::Event` 観測と所有された `sp::Outcome` を伴う、型付き Responses SSE リクエスト。 |
| 29 | [`29_responses_envelope.cpp`](29_responses_envelope.cpp) | key | デバッグ補助。1 つのツール呼び出しリクエストについて、Responses エンベロープ全体と順序付きの出力項目をすべてダンプします。 |
| 30 | [`30_reasoning_effort.cpp`](30_reasoning_effort.cpp) | key | 1 つのプロンプトで推論強度 `none`、`low`、`medium`、`high` を順に試し、レイテンシと推論トークンを比べます。 |
| 34 | [`34_openrouter_responses_tools_sse.cpp`](34_openrouter_responses_tools_sse.cpp) | key | 型付きのホスト型ツール 7 セクションをすべて SSE で実行し、完全な outcome と順序付きの wire 観測を残します。 |
| 31 | [`31_local_transformer.cpp`](31_local_transformer.cpp) | service | `127.0.0.1:8090` のローカル OpenAI 互換サーバー(llama.cpp、vLLM)を指す、型付き Chat クライアント。 |

### Program API

Program は Core グラフを、永続的な識別子、予算、admission を備えたライフサイクルで包みます。JavaScript Program API(join、失敗ルーティング、予算、一時停止と再開、fork、クラッシュ復旧)は、[`bindings/python/tests/program_e2e/`](../bindings/python/tests/program_e2e/README.md) の Python エンドツーエンドシナリオが扱っています。

| # | サンプル | Offline | 示すもの |
|---|---|---|---|
| 63 | [`63_program_quickstart.cpp`](63_program_quickstart.cpp) | yes | `call_core` 操作を 1 つ持つ `ProgramSource` をコンパイルし、admission を通して `ProgramRuntime` で実行します。 |
| 64 | [`64_topology_retrieval.cpp`](64_topology_retrieval.cpp) | yes | 検索で得たトポロジー候補を、永続的なセーフポイントで 1 つの実行中 lineage の検証済みマイグレーションにします。 |
| 32 | [`32_evolution.cpp`](32_evolution.cpp) | yes | Harness 進化ループ。厳密な Core のシードをタスクに合わせて変異させ、系譜を JSON で出力します(`--smoke` は組み込みフィクスチャを使用)。 |
| 58 | [`58_harness_bench.cpp`](58_harness_bench.cpp) | yes | 3 つのフィードバックモードに対する harness 収束ベンチマーク。`--smoke` は組み込みタスクを実行します。 |
| 60 | [`60_harness_mcp_server.cpp`](60_harness_mcp_server.cpp) | yes | Harness MCP サーバー。6 つのツールで、範囲を限定したマルチ worker ワークフローをコンパイルして実行します。`NEOGRAPH_HARNESS_SMOKE=1` でオフラインのフィクスチャプロバイダーに切り替わります。 |

### プロトコル: MCP、A2A、ACP、gRPC

モデルやサーバーを使う MCP サンプルには、それぞれが必要です。59 は手で操作できるサーバーです。A2A と ACP のサンプルはオフラインで動きます。gRPC のサンプルは `-DNEOGRAPH_BUILD_GRPC=ON` のときだけビルドされます。

| # | サンプル | Offline | 示すもの |
|---|---|---|---|
| 59 | [`59_mcp_server.cpp`](59_mcp_server.cpp) | yes | stdio 上の MCP サーバー。`echo` ツール 1 つ、入出力スキーマ、構造化コンテンツ。 |
| 22 | [`22_mcp_stdio.cpp`](22_mcp_stdio.cpp) | key | stdio 上の MCP。サーバーは子プロセスで、ネットワークスタックなしに ReAct ループが駆動します。 |
| 03 | [`03_mcp_agent.cpp`](03_mcp_agent.cpp) | key | streamable HTTP 上の MCP。サーバーのツールを検出して ReAct ループを駆動します。実行中の MCP サーバーが必要です。 |
| 20 | [`20_mcp_hitl.cpp`](20_mcp_hitl.cpp) | key | 人の承認付き MCP。すべての MCP ツール呼び出しの前で `interrupt_before` により止め、保留中の呼び出しを表示して再開します。 |
| 21 | [`21_mcp_fanout.cpp`](21_mcp_fanout.cpp) | service | `Send` を使う MCP。1 つの planner が MCP ツールごとに呼び出しを作り、並列に実行します。モデルは不要ですが、MCP サーバーが動いている必要があります。 |
| 23 | [`23_mcp_multi.cpp`](23_mcp_multi.cpp) | key | 1 つのエージェントが 2 つの MCP サーバー(HTTP と stdio)を 1 つのツール一覧にまとめて使います。 |
| 24 | [`24_mcp_feedback.cpp`](24_mcp_feedback.cpp) | key | MCP 上のオペレーターフィードバック。スクリプト化した 2 回目のターンが下書きを批評し、次の実行がそれを取り込みます。 |
| 35 | [`35_re_agent.cpp`](35_re_agent.cpp) | service | リバースエンジニアリングエージェント。stdio 上の GhidraMCP でストリップされたバイナリの関数名を復元します。Ghidra とキーが必要です。 |
| 38 | [`38_a2a_server.cpp`](38_a2a_server.cpp) | yes | A2A サーバー。グラフを SSE ストリーミング付きで A2A 0.3 と 1.0 で公開し、自前のクライアントで呼び出します。`serve` を渡すと待ち受けを続けます。 |
| 37 | [`37_a2a_client.cpp`](37_a2a_client.cpp) | yes | A2A クライアント。エージェントカードを取得してメッセージを送り、`A2ACallerNode` でグラフからリモートエージェントを呼び出します。 |
| 39 | [`39_acp_server.cpp`](39_acp_server.cpp) | yes | stdio 上の ACP エージェント。エディターが駆動する形です。`--self-test` は initialize、session/new、session/prompt をプロセス内で実行します。 |
| 45 | [`45_grpc_server.cpp`](45_grpc_server.cpp) | paired | gRPC。`GraphEngine` を `GraphService` で公開します。サンプル 53 がこれを補助プロセスとして起動します(`-DNEOGRAPH_BUILD_GRPC=ON` が必要)。 |
| 53 | [`53_grpc_client.cpp`](53_grpc_client.cpp) | yes | gRPC。通常のクライアントから `GraphService`(`Health`、`RunGraph`、ストリーミング)を呼び出します(`-DNEOGRAPH_BUILD_GRPC=ON` が必要)。 |
| 54 | [`54_grpc_checkpoint.cpp`](54_grpc_checkpoint.cpp) | yes | gRPC。リモートの `CheckpointStore` である `GrpcCheckpointStore` と、測定したレイテンシ(`-DNEOGRAPH_BUILD_GRPC=ON` が必要)。 |
| 55 | [`55_grpc_vs_jsonrpc_toolcall.cpp`](55_grpc_vs_jsonrpc_toolcall.cpp) | yes | ツール呼び出しにおける gRPC と JSON-RPC をループバックで測定して比べます。合否の検査ではなく測定です(`-DNEOGRAPH_BUILD_GRPC=ON` が必要)。 |
| 57 | [`57_grpc_remote_tool.cpp`](57_grpc_remote_tool.cpp) | yes | gRPC。別プロセスのツールをローカルの `neograph::Tool` として使います(`-DNEOGRAPH_BUILD_GRPC=ON` が必要)。 |

### 永続化と運用

チェックポイント、状態、メモリ、並行性、可観測性。

| # | サンプル | Offline | 示すもの |
|---|---|---|---|
| 04 | [`04_checkpoint_hitl.cpp`](04_checkpoint_hitl.cpp) | yes | Human-in-the-Loop。支払いノードの手前で `interrupt_before` により止め、チェックポイントを保存し、承認後に再開します。 |
| 08 | [`08_state_management.cpp`](08_state_management.cpp) | yes | 保存したチェックポイントに対する `get_state`、`update_state`、`fork` と、チェックポイント履歴。 |
| 14 | [`14_plan_executor.cpp`](14_plan_executor.cpp) | yes | fan-out の途中で失敗を模擬した Plan and execute。再開すると失敗した兄弟ノードだけが再実行されます(pending writes)。 |
| 26 | [`26_postgres_react_hitl/`](26_postgres_react_hitl/) | key | PostgreSQL を使う deep-research HITL。レポートと再開の間にプロセスが終了しても保持されます。キー、PostgreSQL、Crawl4AI が必要です(Docker Compose)。 |
| 41 | [`41_resume_if_exists_chat.cpp`](41_resume_if_exists_chat.cpp) | yes | `resume_if_exists` を使うマルチターンのチャット。次のターンが保存済みのチェックポイントを読み込み、続きを追記します。 |
| 43 | [`43_store_personalization.cpp`](43_store_personalization.cpp) | yes | ノード内から読み書きするスレッド横断の `Store` で、ユーザーごとに応答を個別化します。 |
| 48 | [`48_sqlite_checkpoint.cpp`](48_sqlite_checkpoint.cpp) | yes | `:memory:` 上の `SqliteCheckpointStore`。チェックポイント、再開、スレッドの分離(ファイルへの永続化は示しません)。 |
| 56 | [`56_history_compaction.cpp`](56_history_compaction.cpp) | yes | 会話履歴の圧縮。要約が正常な場合に限り古いテキストターンが要約に置き換わり、ネイティブのグループは丸ごと保持されます。 |
| 27 | [`27_async_concurrent_runs.cpp`](27_async_concurrent_runs.cpp) | yes | 3 つのエージェント実行が `engine->run_async()` で 1 つの `io_context` スレッド上に交互に載ります。 |
| 44 | [`44_request_queue_backpressure.cpp`](44_request_queue_backpressure.cpp) | yes | バックプレッシャー付きの固定ワーカープール(`RequestQueue`)。処理中の作業を制限し、溢れた分は拒否します。 |
| 46 | [`46_cancel_token.cpp`](46_cancel_token.cpp) | yes | 協調的キャンセル。子ごとに `CancelToken::fork()` を使い、親の `cancel()` がすべてを止めます。 |
| 47 | [`47_node_cache.cpp`](47_node_cache.cpp) | yes | ノード単位の結果キャッシュ。同じ入力での 2 回目の実行はキャッシュに当たり、新しい入力は外れます。 |
| 50 | [`50_async_tool.cpp`](50_async_tool.cpp) | yes | `AsyncTool`。同期の `Tool::execute` 契約でもそのまま受け付けられる、コルーチン形式のツール。 |
| 49 | [`49_openinference.cpp`](49_openinference.cpp) | yes | OpenInference トレーシング。`graph.run > node.* > llm.complete` が、モックプロバイダーとともに 1 つの span ツリーになります。 |

### ショーケースとクックブック

複数の機能を組み合わせた、より大きなプログラムです。クックブックの各プロジェクトには専用の README があります。[クックブックの索引](cookbook/README.md)を参照してください。

| # | サンプル | Offline | 示すもの |
|---|---|---|---|
| 11 | [`11_clay_chatbot.cpp`](11_clay_chatbot.cpp) | service | C++ で作ったデスクトップチャット。Clay がウィンドウを配置し、Raylib が描画し、エージェントの応答がストリーミングされます。既定はモック、`--live` で OpenRouter。`-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON` とディスプレイが必要です。 |

| プロジェクト | Offline | 示すもの |
|---|---|---|
| [`cookbook/the-beast/`](cookbook/the-beast/) | yes | 厳密な Core JSON を書き、コンパイラで検証し、進化させ、チェックポイントでロールバックする自己進化エージェント。オフラインのバイナリはスタブの作成者を使い、ライブ版にはキーが必要です。 |
| [`cookbook/ai-assembly/`](cookbook/ai-assembly/) | yes | A2A のメンバーサーバー 4 つと、議案を配信して投票を集計する Speaker。`scripts/run_session.sh --mock` でオフライン実行でき、`NEOGRAPH_BUILD_DIR` にビルドディレクトリを指定します。 |
| [`cookbook/jarvis/`](cookbook/jarvis/) | yes | 4 方向ルーター、MCP ツール、A2A の専門家、保存されるメモリを備えたメタオーケストレーター。モックビルド: `-DNEOGRAPH_BUILD_COOKBOOK_JARVIS=ON -DNEOGRAPH_JARVIS_FORCE_MOCK=ON`。 |
| [`cookbook/minimal-mcp/`](cookbook/minimal-mcp/) | yes | 標準ライブラリだけの 60 行の Python サーバーを相手にした MCP クライアントの往復。モデルは使いません。 |
| [`cookbook/multi_tenant_chatbot/`](cookbook/multi_tenant_chatbot/) | yes | コンパイルキャッシュを通じて、1 つのプロセスが多数の顧客のトポロジーを処理します。モックのバイナリは何も要らず、ライブ版にはキーが必要です。 |
| [`cookbook/self_evolving_chatbot/`](cookbook/self_evolving_chatbot/) | key | 顧客ごとにトポロジーを選び、進化させるチャットサーバー。ProgramChat には `--mock` モードがあり、Core のサーバーにはキーが必要です。 |
| [`cookbook/topology-retrieval/`](cookbook/topology-retrieval/) | yes | サンプル 64 へつながる候補選択(dense、BM25、順位融合、再ランキング)。`--mock` には `pip install faiss-cpu numpy` が必要です。 |
| [`cookbook/byo-openai/`](cookbook/byo-openai/) and [`cookbook/openrouter-provider/`](cookbook/openrouter-provider/) | service | Python のプロバイダーレシピ。ノード内で自前の OpenAI クライアントを使う方法と、型付きの OpenRouter リクエスト。Chat エンドポイント(キー、または `bindings/python/examples/_scripted.py` のスクリプト化モデルのようなローカルピア)が必要です。 |

この表の `yes` は、プロジェクトの既定の実行にキーが要らないという意味です。スモークテストされるバイナリは `ctest -L examples-offline` に `cookbook.*` として現れます。

### サンプル 11 と 35 についての補足

例 11 は Raylib をソースからビルドするため、Linux では X11 と OpenGL の開発ヘッダーが必要です(例: `libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev`)。テキストには fontconfig または一般的なインストール パスで見つかったシステムの TrueType/OpenType フォントを使います。`NEOGRAPH_CHAT_FONT` と、必要に応じて `NEOGRAPH_CHAT_BOLD_FONT`、`NEOGRAPH_CHAT_MONO_FONT` でファイルを指定でき、`font.ttc#N` はフォント コレクションの N 番目の face を選びます。キー: Enter で送信、Esc で入力を消去、Ctrl+V で貼り付け、Ctrl+Q で終了。応答のストリーミング中は Send ボタンが Stop になります。

例 35 には、`bridge_mcp_ghidra.py` スクリプトのパスを指定する `GHIDRA_MCP_BRIDGE` が必要です。`GHIDRA_MCP_PYTHON` はインタープリターを選択し、既定値は `python3` です。`GHIDRA_SERVER_URL` は GhidraMCP バックエンドのエンドポイントを選択し、既定値は `http://127.0.0.1:18080/` です。実行前に、GhidraMCP バックエンドを有効にしてプログラムを開いた Ghidra を起動してください。この例はモデルを呼び出す前にツールを検出して `list_methods` を一度呼び出し、バックエンドが応答しなければ終了ステータス 2 で終了します。一部の GhidraMCP リリースはバックエンドが停止していてもすべてのツールを列挙するためです。この例にはその後に確認される空でない `OPENROUTER_API_KEY` も必要で、有料モデルを呼び出します。

## オフライン、スクリプト、ライブ

モデルで動くサンプルにはモデルが必要です。オフラインのサンプルは `scripted_provider.h` を使います。これは返答をサンプル自身に書く `neograph::Provider` で、テキスト、ツール呼び出し、ストリーミングの差分、使用量、型付きの失敗を表現できます。実際のプロバイダーと同じ prepare と dispatch の経路を通るので、グラフ、ツール、チェックポイントのコードは、そのまま本番に出せるコードです。モデルではないため、何を返すかはスクリプトが決めます。

```cpp
#include "scripted_provider.h"

auto scripted = std::make_shared<examples::ScriptedProvider>(std::vector<examples::Reply>{
    examples::call_tool("calculator", {{"expression", "2 + 3"}}),   // call 1: ask for a tool
    examples::say("The answer is 5."),                              // call 2: the final answer
});
auto provider = examples::choose_provider(argc, argv, scripted);    // --live switches to OpenRouter
```

スクリプトはリクエストの関数にもできます。`ScriptedProvider([](const examples::ScriptedTurn& turn) { ... })` は履歴、最後のユーザーテキスト、ツール結果を参照でき、ループや並列 fan-out にはこちらが必要です。規則は `scripted_provider.h` 先頭のコメントにあります。

`--live`(または `NG_EXAMPLE_LIVE=1`)は `OPENROUTER_API_KEY` から実際のプロバイダーを作ります。キーはプロセスの環境からのみ読み、ヘルパーはファイルを読みません。ライブ呼び出しはプロバイダーが課金します。古いライブ専用のサンプルはまだ `cppdotenv` で `.env` ファイルを読み込んでおり、書き直す際にこのヘルパーへ移します。サンプル 25 と 26 はローカルの Crawl4AI サーバーも使います。現在のセキュアな Crawl4AI イメージには空でない `CRAWL4AI_API_TOKEN` が必要です。サンプル 26 の `.env.example` を参照してください。

## サンプルを追加する

番号は再利用しません。次に空いているのは 33 と 65 以降です。新しいサンプルは、ソースファイル 1 つ、CMake の 1 行、表の 1 行で構成されます。

1. ファイル名を `NN_short_name.cpp` とし、ヘッダーコメントで始めます。数行の説明、`Usage:` 行、続いてテスト用の注釈を書きます。
2. そのコメントブロックにテスト方法を書きます。オフラインのサンプルは必ず出力する行を書き、キーやサービスが必要なサンプルはオフラインで動かせない理由を書きます(注釈の文法は `examples/run_example_smoke.py` の先頭にあります)。
3. `examples/cmake/` 配下のファイルで `neograph_add_example(short_name NN_short_name.cpp LINK neograph::core neograph::llm)` を使ってターゲットを登録します。ctest の項目は注釈から作られます。
4. 該当する段階の表に、Offline の値付きで行を追加します。
5. `ctest -L examples-offline` と `python scripts/check_examples.py` を実行します。後者は、注釈のないサンプル、重複した番号、注釈と食い違う表があると失敗します。

```cpp
// smoke-expect: Final answer: 427
// smoke-skip: needs OPENROUTER_API_KEY (live model call)
```

上の 2 つの注釈はどちらか一方です。サンプルには `smoke-expect` の行か `smoke-skip` の 1 行のどちらかを書き、両方は書きません。

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
SDK package は alpha `0.1.1`（interface/shared ABI 4）です。

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

例16は既定 cap 8,192 と ask ごとの300秒 deadlineを維持し、完了した空応答だけで最大三回呼びます。
cap を倍増せず failure も再試行しません。空の中間応答や無効な evaluator score はゼロにせず失敗し、切れた最終回答は不完全として1で終了します。
例28の rewrite は low effort と出力512 token を要求し、
空/空白応答なら元の質問をそのまま返します。provider timeout は180秒です。
この経路別設定は共有 factory の既定値を変えません。

複数顧客の self-evolving chatbot は judge 履歴の接頭辞を有効な UTF-8 境界で
200バイト以内に切り、例外をエラーと終了コード1で報告します。The Beast の
`baldwin_llm --llm` はキーが必須で offline oracle に置き換えません。型付き要求は
low effort と既定 cap 1,824 を使い、完了した空の `MaxTokens` 応答だけを既定 cap 3,648 で
一度再要求します。元の Outcome と実際の各呼び出しの usage は保持します。Provider failure、
空応答のままの終了や解析不能な演算名は、完了した learner を装わずに1で終了します。ソース修正はライブ検証やパズル成功の主張ではありません。

### issue #190 / #317 の有限診断

`NG_EXAMPLE_MAX_TOKENS` は ToT、Baldwin、Jarvis、`server_multi`、inspection 例 29/30 の明示的な正の呼び出しごとの出力上限です。隠れた reasoning を含み、明示した場合は決して倍増しません。未設定なら元のレシピ既定値はそのままです。`NG_EXAMPLE_EMPTY_REASKS=0` は既存の完了した空 `MaxTokens` 再要求を無効にします。最大/既定は ToT が 2、Baldwin/Jarvis が 1 です。これらは新しい意味上の呼び出しであり、failure や配信済み tool-call 出力の再試行ではありません。Jarvis は別個の broker ordinal、元の admission bank/deadline、実際のすべての Outcome/report を保持します。合格のために thinking 設定を無効にせず、モデル上限の情報を捏造せず、金銭的 grant を更新しません。

次の制御は元の例の作業量を減らすだけです。

| レシピ | 制御と元の既定値 | 再要求無効時の最大呼び出し数 |
|---|---|---|
| ToT | `NG_TOT_DEPTH=3` (1..3), `NG_TOT_BRANCHING=3` (1..3), `NG_TOT_BEAM_WIDTH=5` (1..5) | 元は 37、1/1/1 で 3 |
| Baldwin（両モード） | `NG_BALDWIN_POPULATION=6` (2/4/6), `NG_BALDWIN_GENERATIONS=4` (1..4) | 元は 48、2/1 で 4 |
| 複数顧客サーバー | `NG_MULTI_CUSTOMERS=5`, `NG_MULTI_TURNS=5` (いずれも 1..5) | 元は 90、顧客 1/4 ターンで 14 |

**別途ホストの支出予約**を行った後、リポジトリー root から実行します。

```sh
# Representative canaries, not substitutes for the unchanged original workload.
NG_EXAMPLE_MAX_TOKENS=8192 NG_EXAMPLE_EMPTY_REASKS=0 \
  NG_TOT_DEPTH=1 NG_TOT_BRANCHING=1 NG_TOT_BEAM_WIDTH=1 \
  ./build/example_tree_of_thoughts
NG_EXAMPLE_MAX_TOKENS=8192 NG_EXAMPLE_EMPTY_REASKS=0 \
  NG_BALDWIN_POPULATION=2 NG_BALDWIN_GENERATIONS=1 \
  ./build/cookbook_the_beast_baldwin_llm --llm
NG_EXAMPLE_MAX_TOKENS=8192 NG_MULTI_CUSTOMERS=1 NG_MULTI_TURNS=4 \
  ./build/cookbook_self_evolving_chatbot_multi
# These inspection commands retain sensitive native/provider payloads.
NG_EXAMPLE_MAX_TOKENS=8192 ./build/example_responses_envelope "Reply briefly to hello."
NG_EXAMPLE_MAX_TOKENS=8192 ./build/example_reasoning_effort
```

この五つのコマンドはモデル呼び出し 3 + 4 + 14 + 1 + 4 = 26 回、設定出力トークン 212,992 に制限され、入力トークンは別です。ドル価格の証明にも支出の承認にもなりません。所有者が承認したモデル/経路を使い、ログは非公開にしてください。元の作業量の qualification では shape override だけを外し、明示した cap/再要求の選択は記録したままにします。この明示設定では ToT + Baldwin + サーバー全体で最大 175 呼び出し / 設定出力トークン 1,433,600 です。

ToT は空でない最終 `EXPR`/`CHECK` を出力しなければなりません。実際の式を独立に評価し、4 を一つ、7 を一つ、8 を二つ正確に使い、結果が 24 であることを確認してください。モデルのスコアは算術 oracle ではありません。Baldwin は両モードで実際の pipeline fitness と hit 数を示す必要があります。特定の進化 trace を強制したり、live learner を offline oracle に置き換えたりしないでください。サーバーは空でないモデル/judge 出力で、シリアライズ/provider failure なしに選択した第 4 ターンへ到達する必要があります。30 は四つの effort を観測する sweep であり、reasoning トークン、実行時間、正確性の単調性は約束されません。空の回答は Outcome を保持したまま exit 2 になります。29 は回答テキストなしで tool call を返すことが正当にあり得ます。これは envelope の inspection であり、天気回答の benchmark ではありません。Provider failure は引き続き nonzero です。

### チェックポイントと進化

例08は terminal checkpoint を fork して新しい user turn を始める既存の流れを保持します。
一時停止した reviewer の resume 例ではありません。例14は実際の executor 回数から節約数を
計算します。最初の五回の後、失敗 sibling 一つだけを再実行すると四回を節約します。

例32は smoke/file モードを選ぶ前に `pnoop` を登録します。repository root から
追跡済み seed/task ファイルを使ってください:

```bash
./build/example_evolution --smoke
./build/example_evolution examples/32_evolution_seed.json examples/32_evolution_task.json
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

Decisions の choice criteria は文字列配列ではなく、choice をキーとするオブジェクトです。
現在の別個の typed client は、raw `SchemaProvider::request_json()` body を使わずに正しい形を表現します。

```cpp
#include <neograph/llm/decisions_client.h>
neograph::llm::DecisionsChoiceQuestion question;
question.instructions = "Choose whether the supplied answer addresses the question.";
question.criteria = {{"accept", "The answer addresses the question."},
                     {"reject", "The answer does not address the question."}};
neograph::llm::DecisionsRequest request;
request.state = {{"question", "What is 2 + 2?"}, {"answer", "4"}};
request.questions.emplace("decision", std::move(question));
```

これは要求の構築であり、新しい有料要求や vendor 精度の証明ではありません。
