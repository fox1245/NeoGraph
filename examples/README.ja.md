<!-- neograph-i18n: source=examples/README.md locale=ja source_sha256=d3155f01f10377484a3a7e1107f4c84adb87543e48a273c9532581b532414a86 -->
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

バイナリはビルドディレクトリに `example_<name>` として作られます。多くのソースファイルは、先頭の `Usage:` 行に正確なコマンドを書いています。Program を使うサンプル(63〜69)には `-DNEOGRAPH_BUILD_PROGRAM=ON` が必要です。そのうち JavaScript のサンプル(65〜69)には `-DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON` も必要で、69 には SQLite も必要です。サンプル 76 には `-DNEOGRAPH_BUILD_MCP_HTTP_SERVER=ON` が必要です。gRPC のサンプル(45、53、54、55、57)には `-DNEOGRAPH_BUILD_GRPC=ON` と `grpc++`/`protoc` が必要です。サンプル 11 には `-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON` が必要です。コンポーネントを無効にしたサンプルはビルドから外れるだけで、ビルドを失敗させません。

## ここから始める

順番に読んでください。1 つ進むごとに考え方が 1 つ増えます。どれもオフラインで動きます。

| 手順 | サンプル | Offline | 学ぶこと |
|---|---|---|---|
| 1 | [`62_core_quickstart.cpp`](62_core_quickstart.cpp) | yes | インストール済みの `neograph::core` ターゲット。ノード 1 つ、strict なグラフ 1 つ、型付きチャネル 1 つ。`HELLO` を出力する。 |
| 2 | [`51_minimal.cpp`](51_minimal.cpp) | yes | チャネル、つまりグラフが実行時に使う状態。ノード 1 つが 1 つのチャネルを読んで 2 つに書き込み、呼び出し側が型付きの `ChannelKey` で両方を読み戻す。 |
| 3 | [`105_conditional_edges.cpp`](105_conditional_edges.cpp) | yes | 条件付きエッジ。`route_channel` がラベルから次のノードを選び、`default` ルートがマップに載っていないラベルをすべて受ける。 |
| 4 | [`05_parallel_fanout.cpp`](05_parallel_fanout.cpp) | yes | fan-out と fan-in。3 つのノードが同じスーパーステップで動き、`barrier` が合流させて後続のノードに渡す。 |
| 5 | [`01_react_agent.cpp`](01_react_agent.cpp) | yes | モデル、ツール、ループ。`llm::Agent` が、スクリプト化したモデルと本物の電卓ツールで ReAct エージェントを実行する。`--live` では実際のモデルを呼ぶ。 |
| 6 | [`102_node_interrupt.cpp`](102_node_interrupt.cpp) | yes | Human-in-the-Loop。ノードが実行を止め、チェックポイントがその状態を保ち、`resume` が人の答えをノードに返す。 |
| 7 | [`65_program_javascript.cpp`](65_program_javascript.cpp) | yes | Program。JavaScript の `define()` と `main()` が C++ ホストから Core グラフを動かす(`-DNEOGRAPH_BUILD_PROGRAM=ON` と `-DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON` が必要)。 |

手順 7 のあとは、次の表で目的から選ぶか、段階別の索引をたどってください。

## やりたいことから探す

目的を選んで、そのサンプルを読んでください。行は基本からプロトコルへと並んでいます。

| やりたいこと | 読むサンプル | Offline |
|---|---|---|
| 最初のグラフを動かす | [`62_core_quickstart.cpp`](62_core_quickstart.cpp) | yes |
| 名前付きチャネルに状態を持たせ、型付きで読み戻す | [`51_minimal.cpp`](51_minimal.cpp) | yes |
| コルーチンなしで短いノードを書く | [`61_sync_node.cpp`](61_sync_node.cpp) | yes |
| ラベルでリクエストをルーティングする(フォールバック付き) | [`105_conditional_edges.cpp`](105_conditional_edges.cpp) | yes |
| リクエストを適切な専門家へルーティングする | [`07_intent_routing.cpp`](07_intent_routing.cpp) | yes |
| ステップを並列に実行して結果を合流させる | [`05_parallel_fanout.cpp`](05_parallel_fanout.cpp) | yes |
| 実行時に作業を作り、ループして戻る | [`10_send_command.cpp`](10_send_command.cpp) | yes |
| 独自のリデューサーやエッジ条件を登録する | [`42_custom_reducer_condition.cpp`](42_custom_reducer_condition.cpp) | yes |
| チャネルが書き込みをまとめる方法(overwrite か append)を選ぶ | [`104_channels_reducers.cpp`](104_channels_reducers.cpp) | yes |
| グラフの中に別のグラフを入れる | [`06_subgraph.cpp`](06_subgraph.cpp) | yes |
| モデルにツールを渡し、答えが出るまでループさせる | [`01_react_agent.cpp`](01_react_agent.cpp) | yes |
| 1 回の呼び出しで ReAct グラフを作る | [`02_custom_graph.cpp`](02_custom_graph.cpp) | yes |
| 実行をリアルタイムに観察する | [`100_stream_modes.cpp`](100_stream_modes.cpp) | yes |
| 失敗するノードをバックオフ付きでリトライする | [`101_retry_policy.cpp`](101_retry_policy.cpp) | yes |
| ノードが先に進む前に、人に尋ねられるようにする | [`102_node_interrupt.cpp`](102_node_interrupt.cpp) | yes |
| ノードの手前でグラフを止め、再開する | [`04_checkpoint_hitl.cpp`](04_checkpoint_hitl.cpp) | yes |
| 暴走したループを止める | [`103_max_steps.cpp`](103_max_steps.cpp) | yes |
| 保存した状態を読み、編集し、フォークする | [`08_state_management.cpp`](08_state_management.cpp) | yes |
| クラッシュ後に完了済みの作業をやり直さず復旧する | [`14_plan_executor.cpp`](14_plan_executor.cpp) | yes |
| 呼び出しをまたいでチャットを続ける | [`41_resume_if_exists_chat.cpp`](41_resume_if_exists_chat.cpp) | yes |
| スレッド間でメモリを共有する | [`43_store_personalization.cpp`](43_store_personalization.cpp) | yes |
| 長い履歴を予算内に収める | [`56_history_compaction.cpp`](56_history_compaction.cpp) | yes |
| 別スレッドから実行をキャンセルする | [`46_cancel_token.cpp`](46_cancel_token.cpp) | yes |
| ノードの再計算を省く | [`47_node_cache.cpp`](47_node_cache.cpp) | yes |
| OpenInference バックエンドへトレースを送る | [`49_openinference.cpp`](49_openinference.cpp) | yes |
| Program をコンパイルして実行する | [`63_program_quickstart.cpp`](63_program_quickstart.cpp) | yes |
| JavaScript で Program を書く | [`65_program_javascript.cpp`](65_program_javascript.cpp) | yes |
| 失敗した Program のノードをオペレーターに回す | [`66_program_failure_routing.cpp`](66_program_failure_routing.cpp) | yes |
| Program が使える量に上限を設ける | [`67_program_budgets.cpp`](67_program_budgets.cpp) | yes |
| Program の実行を一時停止、再開、フォークする | [`68_program_pause_resume_fork.cpp`](68_program_pause_resume_fork.cpp) | yes |
| クラッシュ後に Program の実行を復旧する | [`69_program_crash_recovery.cpp`](69_program_crash_recovery.cpp) | yes |
| 実行中の Program をより良いトポロジーへ移す | [`64_topology_retrieval.cpp`](64_topology_retrieval.cpp) | yes |
| シードから harness を進化させる | [`32_evolution.cpp`](32_evolution.cpp) | yes |
| MCP サーバーとしてツールを提供する | [`59_mcp_server.cpp`](59_mcp_server.cpp) | yes |
| ベアラートークン付きで MCP を HTTP で公開する | [`76_mcp_http_server.cpp`](76_mcp_http_server.cpp) | yes |
| ReAct ループの中で MCP ツールを使う | [`22_mcp_stdio.cpp`](22_mcp_stdio.cpp) | yes |
| 失敗する MCP ツールを扱う | [`75_mcp_tool_errors.cpp`](75_mcp_tool_errors.cpp) | yes |
| MCP ツールの呼び出しを、実行前に承認する | [`20_mcp_hitl.cpp`](20_mcp_hitl.cpp) | yes |
| A2A でグラフを他のエージェントへ公開する | [`38_a2a_server.cpp`](38_a2a_server.cpp) | yes |
| リモートの A2A エージェントを呼び出す | [`37_a2a_client.cpp`](37_a2a_client.cpp) | yes |
| 止まったグラフを、入力を待つ A2A タスクにする | [`78_a2a_input_required.cpp`](78_a2a_input_required.cpp) | yes |
| ACP でグラフをエディターへ公開する | [`39_acp_server.cpp`](39_acp_server.cpp) | yes |
| 危険なステップの前にエディターへ許可を求める | [`80_acp_permission.cpp`](80_acp_permission.cpp) | yes |
| gRPC でグラフを公開する | [`45_grpc_server.cpp`](45_grpc_server.cpp) | yes |
| gRPC で別プロセスのツールを呼び出す | [`57_grpc_remote_tool.cpp`](57_grpc_remote_tool.cpp) | yes |
| Reflexion などの推論パターンを試す | [`15_reflexion.cpp`](15_reflexion.cpp) | key |
| ローカルのモデルサーバーに対して実行する | [`31_local_transformer.cpp`](31_local_transformer.cpp) | service |

## 段階別の索引

### Core エンジン

ノード、チャネル、ルーティング、fan-out、ストリーミング、リトライ、割り込みを扱います。`neograph::core` だけをリンクしますが、スクリプト化したプロバイダーを使う 02、06、07 は `neograph::llm` もリンクします。

| # | サンプル | Offline | 示すもの |
|---|---|---|---|
| 62 | [`62_core_quickstart.cpp`](62_core_quickstart.cpp) | yes | インストール済みの `neograph::core` ターゲット。ノード 1 つ、strict なグラフ 1 つ、型付きチャネル 1 つ。 |
| 02 | [`02_custom_graph.cpp`](02_custom_graph.cpp) | yes | `create_react_graph()` が 1 回の呼び出しで llm、tools、llm の ReAct グラフを作り、`run_stream` がノードイベントを出力する。 |
| 05 | [`05_parallel_fanout.cpp`](05_parallel_fanout.cpp) | yes | fan-out と fan-in。3 つの researcher ノードが同時にタイマーを待ち(カウンターで確かめる)、barrier が合流させて summarizer に渡す。 |
| 06 | [`06_subgraph.cpp`](06_subgraph.cpp) | yes | サブグラフ。外側の supervisor グラフが内側の ReAct グラフに処理を任せる。 |
| 07 | [`07_intent_routing.cpp`](07_intent_routing.cpp) | yes | 分類ノードと条件付きエッジ(`route_channel`)が、各リクエストを math、translate、general のいずれかの専門家へ送る。 |
| 09 | [`09_all_features.cpp`](09_all_features.cpp) | yes | エンジンの 5 つの機能(`NodeInterrupt`、`RetryPolicy`、`StreamMode`、`Command`、`Store`)を数行ずつ見て回る。それぞれ、その機能を扱うサンプルへ案内する。 |
| 10 | [`10_send_command.cpp`](10_send_command.cpp) | yes | `Send` による fan-out と `Command` による経路の上書き。十分な結果が揃うまで、planner、researcher、evaluator がループする。 |
| 42 | [`42_custom_reducer_condition.cpp`](42_custom_reducer_condition.cpp) | yes | C++ から独自のチャネルリデューサーとエッジ条件を登録する。 |
| 51 | [`51_minimal.cpp`](51_minimal.cpp) | yes | チャネル。ノード 1 つが 1 つのチャネルを読んで 2 つに書き込み、呼び出し側は型付きの `ChannelKey` で両方を読み戻す(`result.channel(key)`、`try_channel`)。 |
| 52 | [`52_export_schema.cpp`](52_export_schema.cpp) | yes | `NodeFactory::export_schema()` が、ビジュアルエディターがパレットを作るための、トポロジーの JSON Schema を出力する。 |
| 61 | [`61_sync_node.cpp`](61_sync_node.cpp) | yes | `SyncGraphNode`。コルーチンの定型コードなしで、短いノード 2 つ(空白の除去、続けて大文字化)を書く。ブロッキング I/O は引き続き `GraphNode::run()` に書く。 |
| 100 | [`100_stream_modes.cpp`](100_stream_modes.cpp) | yes | `StreamMode`。同じ実行を VALUES、UPDATES、EVENTS、DEBUG の各モードで観察し、モードごとに届くイベントを数える。 |
| 101 | [`101_retry_policy.cpp`](101_retry_policy.cpp) | yes | `RetryPolicy`。ノードが 2 回失敗し、エンジンが指数バックオフでリトライする。上限を小さくすると、リトライを使い切ったときに呼び出し側が何を受け取るかが分かる。 |
| 102 | [`102_node_interrupt.cpp`](102_node_interrupt.cpp) | yes | `NodeInterrupt`。ノード自身が実行を止め、ペイロードが呼び出し側に尋ねる内容を伝え、`resume` が答え(承認または拒否)をノードに返す。 |
| 103 | [`103_max_steps.cpp`](103_max_steps.cpp) | yes | `max_steps`。ループがステップ上限で止まり、結果がそれを知らせる(`max_steps_exhausted()`、`RunStatus::StepLimit`)。最後のステップで終わった実行には印が付かない。 |
| 104 | [`104_channels_reducers.cpp`](104_channels_reducers.cpp) | yes | チャネルとリデューサー。`overwrite` と `append` の違い、`initial` の値、チャネルのバージョン(全チャネルで 1 つの書き込みカウンターを共有)。 |
| 105 | [`105_conditional_edges.cpp`](105_conditional_edges.cpp) | yes | 条件付きエッジ。`route_channel` がラベルから次のノードを選び、`default` ルートがマップに載っていないラベルをすべて受ける。 |

### エージェントパターン

モデルとツールをループで組み合わせた構成と、その上に作った公開済みの推論パターンです。`key` の行は実際のモデルを呼び出します。

| # | サンプル | Offline | 示すもの |
|---|---|---|---|
| 01 | [`01_react_agent.cpp`](01_react_agent.cpp) | yes | ReAct。Thought の 1 行、ツール呼び出し、Observation、最終回答。`--live` では実際のモデルを呼ぶ。 |
| 36 | [`36_classifier_fanout.cpp`](36_classifier_fanout.cpp) | yes | 5 つの小さな分類器を `Send` で fan-out する。所要時間は合計ではなく、最も遅い 1 つ分になる。判定結果は固定値。 |
| 15 | [`15_reflexion.cpp`](15_reflexion.cpp) | key | Reflexion。critic が受け入れるまで、actor と critic がループする(Shinn et al. 2023)。 |
| 16 | [`16_tree_of_thoughts.cpp`](16_tree_of_thoughts.cpp) | key | Tree of Thoughts。候補の思考を生成し、採点し、残し、展開する。 |
| 17 | [`17_self_ask.cpp`](17_self_ask.cpp) | key | Self-Ask。マルチホップ推論のための、明示的な追加質問(Press et al. 2022)。 |
| 18 | [`18_multi_agent_debate.cpp`](18_multi_agent_debate.cpp) | key | researcher、skeptic、judge。共有する 1 つのトランスクリプトに対して、3 つのシステムプロンプトが働く。 |
| 19 | [`19_rewoo.cpp`](19_rewoo.cpp) | key | REWOO。planner がプレースホルダーを書き、worker がツールを並列に実行し、solver が証拠をまとめる。 |
| 12 | [`12_rag_agent.cpp`](12_rag_agent.cpp) | key | 実際の埋め込みとメモリ内のコサイン検索を使う、検索拡張生成。 |
| 28 | [`28_corrective_rag.cpp`](28_corrective_rag.cpp) | key | Corrective RAG。検索結果を評価し、洗練するか、Web 検索を足すか、Web 検索だけにする(Yan et al. 2024)。 |
| 25 | [`25_deep_research.cpp`](25_deep_research.cpp) | key | Deep research。supervisor が、それぞれ ReAct ループを回すサブ researcher を fan-out し、レポートを書く。`OPENROUTER_API_KEY` と Crawl4AI サーバーが必要。 |
| 40 | [`40_react_async_streaming.cpp`](40_react_async_streaming.cpp) | key | 非同期 ReAct。外側のコルーチンが `run_stream_async` を待ち、型付きのテキスト差分を出力する。 |
| 13 | [`13_openrouter_responses_sse.cpp`](13_openrouter_responses_sse.cpp) | key | 型付きの Responses SSE リクエスト。順序付きの `sp::Event` の観測と、所有権を持つ `sp::Outcome` を扱う。 |
| 29 | [`29_responses_envelope.cpp`](29_responses_envelope.cpp) | key | デバッグ補助。1 つのツール呼び出しリクエストについて、Responses のエンベロープ全体と、順序付きの出力項目をすべてダンプする。 |
| 30 | [`30_reasoning_effort.cpp`](30_reasoning_effort.cpp) | key | 1 つのプロンプトで推論強度 `none`、`low`、`medium`、`high` を順に試し、レイテンシと推論トークンを比べる。 |
| 34 | [`34_openrouter_responses_tools_sse.cpp`](34_openrouter_responses_tools_sse.cpp) | key | 型付きのホスト型ツール 7 セクションすべてを SSE で実行する。完全な outcome と、順序付きのワイヤー観測を残す。 |
| 31 | [`31_local_transformer.cpp`](31_local_transformer.cpp) | service | `127.0.0.1:8090` のローカル OpenAI 互換サーバー(llama.cpp、vLLM)を指す、型付き Chat クライアント。 |

### Program API

Program は Core グラフを、永続的な識別子、予算、admission を備えたライフサイクルで包みます。JavaScript の Program API(join、失敗ルーティング、予算、一時停止と再開、fork、クラッシュ復旧)は、[`bindings/python/tests/program_e2e/`](../bindings/python/tests/program_e2e/README.md) の Python エンドツーエンドシナリオで動作を確認しています。65〜69 番のサンプルは C++ ホストから JavaScript の Program を実行するもので、Python のサンプル 29〜36 番に対応します。どれも `-DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON` が必要です。これらが共有するレジストリ、admission、ランタイムの設定は `program_support.h` にまとめてあります。63 番は、C++ のビルダー JSON で書いた Program にこの設定を使います。

| # | サンプル | Offline | 示すもの |
|---|---|---|---|
| 63 | [`63_program_quickstart.cpp`](63_program_quickstart.cpp) | yes | `call_core` 操作を 1 つ持つ `ProgramSource` をコンパイルし、admission を通して `ProgramRuntime` で実行する。 |
| 64 | [`64_topology_retrieval.cpp`](64_topology_retrieval.cpp) | yes | 検索で得たトポロジーの候補が、永続的なセーフポイントで、実行中の 1 つの系統の検証済みマイグレーションになる。 |
| 65 | [`65_program_javascript.cpp`](65_program_javascript.cpp) | yes | C++ ホストが、`define()` と `main()` を持つ JavaScript の Program をコンパイルして実行する。`main()` が `ng.callCore` でグラフを 2 回呼び、21 が 42 に、さらに 84 になる。 |
| 66 | [`66_program_failure_routing.cpp`](66_program_failure_routing.cpp) | yes | Python 32 番の C++ 版。TEST が作成者コード付きの `NodeFailureError` を投げ、2 回リトライされたあと、`interruptBefore` 付きのハンドラーにルーティングされる。`retry` で `resume` すると Program は完了し、`abort` なら `plan.user_abort` 付きの `failed` で終わる。 |
| 67 | [`67_program_budgets.cpp`](67_program_budgets.cpp) | yes | C++ での 3 つの予算制限。ホストの `BudgetLimits` の上限を超える要求をした Program は admission で拒否され(`P_ADMIT_POLICY`)、別の `RunBudget` での実行は開始時に拒否され(`P_START_BUDGET`)、`max_core_steps` を使い切ったループは `budget_exhausted` で終わる。 |
| 68 | [`68_program_pause_resume_fork.cpp`](68_program_pause_resume_fork.cpp) | yes | Python 34 番と 35 番の C++ 版。`interruptBefore` が実行を止め、`resume` がオペレーターの答えで続きを実行し、`fork` は止まった同じ実行に、チェックポイントから別の答えを与える。終わったノードの本体は、合計で 1 回しか動かない。 |
| 69 | [`69_program_crash_recovery.cpp`](69_program_crash_recovery.cpp) | yes | SQLite ファイルを使う Python 36 番の C++ 版。プロセス A が質問に答え、`charge` を実行して異常終了する。プロセス B が同じディレクトリを開いて `reconnect` を呼ぶと、実行は 2 回目の試行で完了する。`charge` は 2 回動くため、その効果は台帳で守る。対象は Linux と macOS。 |
| 70 | [`70_program_plan_intake.cpp`](70_program_plan_intake.cpp) | yes | スクリプト化した計画役が、フラットな `step-plan/v1` で答えます。受け付けノードは最初の答えを 3 件の違反とともに拒否し、計画役は違反を受けて答え直します。2 回目の答えはタスクグラフに変換され、2 つの合計を求め、それらを足し、単語を 1 つ大文字にします。 |
| 32 | [`32_evolution.cpp`](32_evolution.cpp) | yes | Harness の進化ループ。strict な Core のシードをタスクに対して変異させ、系統を JSON で出力する(`--smoke` は組み込みのフィクスチャを使う)。 |
| 58 | [`58_harness_bench.cpp`](58_harness_bench.cpp) | yes | 3 つのフィードバックモードに対する harness の収束ベンチマーク。`--smoke` は組み込みのタスクを実行する。 |
| 60 | [`60_harness_mcp_server.cpp`](60_harness_mcp_server.cpp) | yes | Harness MCP サーバー。6 つのツールで、範囲を限定したマルチ worker ワークフローをコンパイルして実行する。`NEOGRAPH_HARNESS_SMOKE=1` でオフラインのフィクスチャプロバイダーに差し替わる。 |

### プロトコル: MCP、A2A、ACP、gRPC

どのサンプルもオフラインで動きます。MCP クライアントのサンプルは、標準ライブラリだけのサーバー(`mcp_demo_server.py`、インストール不要)を起動し、スクリプト化したモデルを使います。A2A と ACP のサンプルは、クライアントとサーバーを 1 つのプロセスに置くか、補助プロセスを起動します。有効化が必要な CMake オプション(`-DNEOGRAPH_BUILD_GRPC=ON` など)があるサンプルは、その行にオプションを書いてあります。

| # | サンプル | Offline | 示すもの |
|---|---|---|---|
| 59 | [`59_mcp_server.cpp`](59_mcp_server.cpp) | yes | stdio 上の MCP サーバー。`echo` ツール 1 つ、入力と出力のスキーマ、構造化コンテンツ。 |
| 76 | [`76_mcp_http_server.cpp`](76_mcp_http_server.cpp) | yes | streamable HTTP 上の MCP サーバー。空きポートでベアラートークンを検査し、NeoGraph 自身のクライアントが呼び出す。トークンなしの呼び出しは HTTP 401 になる(`-DNEOGRAPH_BUILD_MCP_HTTP_SERVER=ON` が必要)。 |
| 22 | [`22_mcp_stdio.cpp`](22_mcp_stdio.cpp) | yes | stdio 上の MCP。サーバーはネットワークスタックを持たない子プロセスで、ReAct ループがそのツールを呼び出す。スクリプト化したモデルと、標準ライブラリだけのサーバーを使う。 |
| 03 | [`03_mcp_agent.cpp`](03_mcp_agent.cpp) | yes | streamable HTTP 上の MCP。サーバーのツールを検出して ReAct ループに渡す。デモサーバーは標準ライブラリだけの Python で、モデルはスクリプト化している。 |
| 20 | [`20_mcp_hitl.cpp`](20_mcp_hitl.cpp) | yes | 人の承認付きの MCP。`interrupt_before` が MCP ツール呼び出しの手前でグラフを止め、保留中の呼び出しを表示し、`resume()` がモデル呼び出しを繰り返さずに続行する。 |
| 21 | [`21_mcp_fanout.cpp`](21_mcp_fanout.cpp) | yes | `Send` を使う MCP。planner がツールごとに呼び出しを 1 つ作り、3 つの呼び出しが 1 つのスーパーステップの中で重なって実行される。モデルは使わない。 |
| 23 | [`23_mcp_multi.cpp`](23_mcp_multi.cpp) | yes | 1 つのエージェントが、2 つの MCP サーバー(HTTP と stdio)のツールを 1 つの一覧にまとめて使い、2 回の呼び出しはそれぞれ別のサーバーに届く。 |
| 24 | [`24_mcp_feedback.cpp`](24_mcp_feedback.cpp) | yes | MCP 上のオペレーターフィードバック。最初の答えは推測で、オペレーターの返信が履歴に加わり、2 回目のラウンドでツールを呼び出す。 |
| 75 | [`75_mcp_tool_errors.cpp`](75_mcp_tool_errors.cpp) | yes | MCP の失敗。`isError` の結果と JSON-RPC エラーは別の応答で、エージェントは両方のメッセージを読んで自分で直す。 |
| 35 | [`35_re_agent.cpp`](35_re_agent.cpp) | yes | リバースエンジニアリングエージェント。stdio 上の GhidraMCP で、ストリップ済みのバイナリの関数に名前を付ける。代役のブリッジとスクリプト化したモデルでオフライン実行でき、`--live` では Ghidra と実際のモデルを使う。 |
| 38 | [`38_a2a_server.cpp`](38_a2a_server.cpp) | yes | A2A サーバー。グラフを SSE ストリーミング付きで A2A 0.3 と 1.0 で公開し、自前のクライアントで呼び出す。`serve` を渡すと待ち受けを続ける。 |
| 77 | [`77_a2a_dialects.cpp`](77_a2a_dialects.cpp) | yes | A2A のダイアレクト。エージェントカードが JSON-RPC の 1.0 と 0.3 を挙げ、1 つ目のクライアントはそこから 1.0 を選ぶ。0.3 だけを載せたカードを渡された 2 つ目のクライアントは、0.3 を選ぶ。 |
| 78 | [`78_a2a_input_required.cpp`](78_a2a_input_required.cpp) | yes | A2A の `input-required`。`interrupt_before` で止まったグラフが待機するタスクになり、クライアントが同じタスクに答えると、結果をアーティファクトとして受け取る。 |
| 79 | [`79_a2a_collaboration.cpp`](79_a2a_collaboration.cpp) | yes | A2A のコラボレーション。2 つのエージェントがリンクを結び(提案、次に承認)、メールボックスを通して連番付きのエンベロープをやり取りする。再配送されたものは 1 回だけ適用される。 |
| 37 | [`37_a2a_client.cpp`](37_a2a_client.cpp) | yes | A2A クライアント。エージェントカードを取得してメッセージを送り、`A2ACallerNode` でグラフからリモートエージェントを呼び出す。 |
| 39 | [`39_acp_server.cpp`](39_acp_server.cpp) | yes | stdio 上の ACP エージェント。エディターが駆動する形。`--self-test` は initialize、session/new、session/prompt をプロセス内で実行する。 |
| 80 | [`80_acp_permission.cpp`](80_acp_permission.cpp) | yes | ACP の許可確認。ノードが削除の前に `session/request_permission` でエディターに尋ねる。スクリプト化したエディターが 1 回目は許可し、次は拒否する。 |
| 81 | [`81_acp_elicitation.cpp`](81_acp_elicitation.cpp) | yes | ACP の elicitation。クライアントがその capability を宣言していれば、ノードが `elicitation/create` でユーザーにフォーム形式の質問をし、宣言していなければ既定値に切り替える。 |
| 45 | [`45_grpc_server.cpp`](45_grpc_server.cpp) | yes | gRPC。`GraphEngine` を `GraphService` で公開する。`--self-test` はサーバーとクライアントを 1 つのプロセスで空きポートに立てて実行し、サンプル 53 がプロセス間のクライアントになる(`-DNEOGRAPH_BUILD_GRPC=ON` が必要)。 |
| 53 | [`53_grpc_client.cpp`](53_grpc_client.cpp) | yes | gRPC。通常のクライアントから `GraphService`(`Health`、`RunGraph`、ストリーミング)を呼び出す(`-DNEOGRAPH_BUILD_GRPC=ON` が必要)。 |
| 54 | [`54_grpc_checkpoint.cpp`](54_grpc_checkpoint.cpp) | yes | gRPC。リモートの `CheckpointStore` である `GrpcCheckpointStore` と、測定したレイテンシ(`-DNEOGRAPH_BUILD_GRPC=ON` が必要)。 |
| 55 | [`55_grpc_vs_jsonrpc_toolcall.cpp`](55_grpc_vs_jsonrpc_toolcall.cpp) | yes | ツール呼び出しで gRPC と JSON-RPC をループバック上で測って比べる。測定であり、合否の検査ではない(`-DNEOGRAPH_BUILD_GRPC=ON` が必要)。 |
| 57 | [`57_grpc_remote_tool.cpp`](57_grpc_remote_tool.cpp) | yes | gRPC。別プロセスのツールを、ローカルの `neograph::Tool` として使う(`-DNEOGRAPH_BUILD_GRPC=ON` が必要)。 |

### 永続化と運用

チェックポイント、状態、メモリ、並行性、可観測性を扱います。

| # | サンプル | Offline | 示すもの |
|---|---|---|---|
| 04 | [`04_checkpoint_hitl.cpp`](04_checkpoint_hitl.cpp) | yes | Human-in-the-Loop。支払いノードの手前で `interrupt_before` により止め、チェックポイントを保存し、承認後に再開する。 |
| 08 | [`08_state_management.cpp`](08_state_management.cpp) | yes | 保存したチェックポイントに対する `get_state`、`update_state`、`fork` と、チェックポイントの履歴。 |
| 14 | [`14_plan_executor.cpp`](14_plan_executor.cpp) | yes | fan-out の途中で失敗を模擬した Plan and execute。再開すると、失敗した兄弟ノードだけが再実行される(pending writes)。 |
| 26 | [`26_postgres_react_hitl/`](26_postgres_react_hitl/) | key | PostgreSQL を使う deep-research の HITL。レポートの出力と再開の間にプロセスが終了しても、状態が残る。キー、PostgreSQL、Crawl4AI(Docker Compose)が必要。 |
| 41 | [`41_resume_if_exists_chat.cpp`](41_resume_if_exists_chat.cpp) | yes | `resume_if_exists` を使うマルチターンのチャット。次のターンが保存済みのチェックポイントを読み込み、続きを追記する。 |
| 43 | [`43_store_personalization.cpp`](43_store_personalization.cpp) | yes | ノード内から読み書きする、スレッド横断の `Store`。ユーザーごとに応答を個別化する。 |
| 48 | [`48_sqlite_checkpoint.cpp`](48_sqlite_checkpoint.cpp) | yes | `:memory:` 上の `SqliteCheckpointStore`。チェックポイント、再開、スレッドの分離(ファイルへの永続化は示さない)。 |
| 56 | [`56_history_compaction.cpp`](56_history_compaction.cpp) | yes | 会話履歴の圧縮。要約が正常な場合に限り、古いテキストのターンが要約に置き換わり、ネイティブのグループは丸ごと残る。 |
| 27 | [`27_async_concurrent_runs.cpp`](27_async_concurrent_runs.cpp) | yes | 3 つのエージェント実行が、`engine->run_async()` を通じて 1 つの `io_context` スレッド上で交互に進む。 |
| 44 | [`44_request_queue_backpressure.cpp`](44_request_queue_backpressure.cpp) | yes | バックプレッシャー付きの固定ワーカープール(`RequestQueue`)。処理中の作業数に上限を設け、溢れた分は拒否する。 |
| 46 | [`46_cancel_token.cpp`](46_cancel_token.cpp) | yes | 協調的キャンセル。子ごとに `CancelToken::fork()` を使い、親の `cancel()` がすべてを止める。 |
| 47 | [`47_node_cache.cpp`](47_node_cache.cpp) | yes | ノード単位の結果キャッシュ。同じ入力での 2 回目の実行はキャッシュに当たり、新しい入力は外れる。 |
| 50 | [`50_async_tool.cpp`](50_async_tool.cpp) | yes | `AsyncTool`。コルーチン形式のツールで、同期の `Tool::execute` の契約もそのまま受け付ける。 |
| 49 | [`49_openinference.cpp`](49_openinference.cpp) | yes | OpenInference トレーシング。モックプロバイダーを使い、`graph.run > node.* > llm.complete` を 1 つの span ツリーにする。 |

### ショーケースとクックブック

複数の機能を組み合わせた、より大きなプログラムです。クックブックの各プロジェクトには専用の README があります。[クックブックの索引](cookbook/README.md)を参照してください。

| # | サンプル | Offline | 示すもの |
|---|---|---|---|
| 11 | [`11_clay_chatbot.cpp`](11_clay_chatbot.cpp) | service | C++ で作ったデスクトップチャット。Clay がウィンドウを配置し、Raylib が描画し、エージェントの応答がストリーミングされる。既定はモックで、`--live` で OpenRouter を使う。`-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON` とディスプレイが必要。 |

| プロジェクト | Offline | 示すもの |
|---|---|---|
| [`cookbook/the-beast/`](cookbook/the-beast/) | yes | strict な Core JSON を書き、コンパイラーのゲートを通し、進化させ、チェックポイントでロールバックする自己進化エージェント。オフラインのバイナリはスタブの作成者を使い、ライブ版にはキーが必要。 |
| [`cookbook/ai-assembly/`](cookbook/ai-assembly/) | yes | A2A のメンバーサーバー 4 つと、法案を一斉に配信して投票を集計する Speaker。`scripts/run_session.sh --mock` でオフライン実行でき、`NEOGRAPH_BUILD_DIR` にビルドディレクトリを指定する。 |
| [`cookbook/jarvis/`](cookbook/jarvis/) | yes | 4 方向ルーター、MCP ツール、A2A の専門家、保存される会話メモリを備えたメタオーケストレーター。モックビルド: `-DNEOGRAPH_BUILD_COOKBOOK_JARVIS=ON -DNEOGRAPH_JARVIS_FORCE_MOCK=ON`。 |
| [`cookbook/minimal-mcp/`](cookbook/minimal-mcp/) | yes | 標準ライブラリだけの 60 行の Python サーバーを相手にした、MCP クライアントの往復。モデルは使わない。 |
| [`cookbook/multi_tenant_chatbot/`](cookbook/multi_tenant_chatbot/) | yes | コンパイルキャッシュを通して、1 つのプロセスが多数の顧客のトポロジーを処理する。モックのバイナリには何も要らず、ライブ版にはキーが必要。 |
| [`cookbook/self_evolving_chatbot/`](cookbook/self_evolving_chatbot/) | key | 顧客ごとにトポロジーを選び、進化させるチャットサーバー。ProgramChat には `--mock` モードがあり、Core のサーバーにはキーが必要。 |
| [`cookbook/topology-retrieval/`](cookbook/topology-retrieval/) | yes | サンプル 64 に入力を渡す候補の選択(dense、BM25、順位融合、再ランキング)。`--mock` には `pip install faiss-cpu numpy` が必要。 |
| [`cookbook/program-coding-swarm/`](cookbook/program-coding-swarm/) | yes | Program API 上で階層型マルチモジュール・コーディングワークフローを実行する実験的な概念実証: 並列子ハーネス、モジュールごとの予算とモデルロール、ホスト所有の検証・レビュー判定ゲート、孫セカンドオピニオンハーネス、SQLite 世代置換とロールバック、`central` と `delegated` の合成モード比較（既定は `--mock`、JSON レポート付きの `--live`）。 |
| [`cookbook/byo-openai/`](cookbook/byo-openai/) と [`cookbook/openrouter-provider/`](cookbook/openrouter-provider/) | service | Python のプロバイダーレシピ。ノードの中で自前の OpenAI クライアントを使う方法と、型付きの OpenRouter リクエスト。Chat エンドポイントが必要(キー、または `bindings/python/examples/_scripted.py` のスクリプト化したモデルのようなローカルのピア)。 |

この表の `yes` は、プロジェクトの既定の実行にキーが要らないという意味です。スモークテストの対象になるバイナリは、`ctest -L examples-offline` に `cookbook.*` として現れます。

### サンプル 11 と 35 についての補足

サンプル 11 は Raylib をソースからビルドするため、Linux では X11 と OpenGL の開発ヘッダーが必要です(例: `libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev`)。テキストの表示には、fontconfig か一般的なインストール先で見つかったシステムの TrueType/OpenType フォントを使います。フォントのファイルは `NEOGRAPH_CHAT_FONT` で選び、必要なら `NEOGRAPH_CHAT_BOLD_FONT` と `NEOGRAPH_CHAT_MONO_FONT` も設定できます。`font.ttc#N` は、コレクションの N 番目の face を選びます。キー操作は次のとおりです。Enter で送信、Esc で入力の消去、Ctrl+V で貼り付け、Ctrl+Q で終了。応答のストリーミング中は、Send ボタンが Stop に変わります。

サンプル 35 は、`--bridge examples/ghidra_stand_in.py`(GhidraMCP ブリッジの代役)とスクリプト化したモデルを使って、オフラインで動きます。`--live` で実行するときは、`GHIDRA_MCP_BRIDGE`(または `--bridge`)で、自分の `bridge_mcp_ghidra.py` スクリプトを指定する必要があります。`GHIDRA_MCP_PYTHON` はそのインタープリターを選び(既定は `python3`)、`GHIDRA_SERVER_URL` は GhidraMCP バックエンドのエンドポイントを選びます(既定は `http://127.0.0.1:18080/`)。ライブ実行の前に、GhidraMCP バックエンドを有効にし、プログラムを開いた状態で Ghidra を起動してください。このサンプルは、モデルを呼び出す前にツールを検出し、`list_methods` を 1 回呼び出します。バックエンドが応答しなければ、終了ステータス 2 で終了します。GhidraMCP のリリースによっては、バックエンドが停止していてもすべてのツールを列挙するためです。ライブ実行には、空でない `OPENROUTER_API_KEY` も必要です(確認はそのあとです)。有料のモデル呼び出しも行います。

## オフライン、スクリプト、ライブ

モデルで動くサンプルにはモデルが必要です。オフラインのサンプルは `scripted_provider.h` を使います。これは `neograph::Provider` の一種で、応答(テキスト、ツール呼び出し、ストリーミングの差分、使用量、型付きの失敗)をサンプルの中に書きます。実際のプロバイダーと同じ prepare と dispatch の経路を通るので、グラフ、ツール、チェックポイントのコードは、そのまま本番に出せるコードです。これはモデルではありません。何を返すかはスクリプトが決めます。

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

番号は再利用しません。既存のどの番号よりも 1 大きい番号を使ってください。新しいサンプルは、ソースファイル 1 つ、CMake の 1 行、表の 1 行で構成されます。

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
native リプレイ権限ではありません。prepare は一度だけ行い、durable 呼出元は
`Provider::request_digest(prepared)` に claim/receipt を結び付け、同じ handle を dispatch します。
出力 JSON から history を再構築したり、failure を最終テキストに縮約しないでください。

canonical persistence は `provider-message-v3`/`runtime-history-record-v3` を使用し、
portable 要約は native record を置換しません。optional control は呼出元が選び、暗黙に clamp しません。
bounded call には真正 model fact が必要で、欠落は `LimitUnknown` です。
reservation/charged/held は nullable provider usage と別で、budget 更新、価格、forecast、invoice ではありません。

header-only `examples/provider_example_support.h` は実際の SDK runtime を使用します。
`NEOGRAPH_BUILD_LLM=OFF` を含む全 Core ビルドに `SchemaProvider::runtime` が必要です。
CMake 3.20+ は明示 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`、インストール済み runtime package、
固定 public GitHub source archive の順に SDK を選択します。download fallback は既定で有効です。
インストール済み package/明示 source の offline ビルドでは `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` にしてください。
インストール include root は `include/SchemaProvider` です。interface/capability 検査を行い、
SDK package は alpha `0.4.0`（interface revision/shared ABI 7）です。

保存済み interface-3 model-free C++ E2E 実行では番号付き target39個を検証しました。finite offline29個と、
実際の MCP/ACP/A2A/Harness および gRPC graph/チェックポイント/tool 経路です。gRPC-vs-JSON-RPC
測定例も実行しましたが返却値を検証せず、behavioral E2E pass には数えません。
live/外部 model 経路22個と無効な Clay GUI は未検証で、公開 vendor 要求や新 grant はありません。
過去の測定は新移行の qualification ではありません。live には鍵、network、model access と費用が必要です。
鍵、prompt、artifact は非公開に保ち、機密 envelope/native 出力を公開 log に送らないでください。
native archive は owner-private 認証 custody で、暗号化や vendor issuer 認証ではありません。

以前の interface-4 証拠は別です。Linux x86_64 で native research recovery、
ToT/Forge/rewrite helper と A2A 0.3/1.0 peer を実行し、インストール済み Python application
11個が分割した実行群で credential-free localhost 要求48個を行いました。
追跡済み evolution file mode と Plan resume も以下のとおり実行しました。
保存済み C++ full suite の再実行ではなく、範囲を限定した local 実行です。
[SDK interface-4 実行記録](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/CONFORMANCE.md#interface-4-execution-record)を参照してください。
新しい hosted vendor、Windows/macOS/ARM64、HTTP/3、sanitizer 検証は主張せず、
remote CI と公開は未完了です。

## 保持された例の契約

このソース契約は SDK `0.4.0`（interface 7）に対してビルドします。上の C++ 実行記録は
それより古い SDK interface で行ったもので、過去の証拠のままです。現在の SDK の検証でも、
新しい live 呼出しでもありません。family ごとの制御は
`ProviderControls` の閉じた型付きフィールドで、非対応 family/origin/model は I/O 前に拒否します。
reasoning/sampling/tool 制御、Responses のサーバー保持 cursor、deployment header、
明示的 portable Gemini history は [provider reference](../docs/reference-en.md) を参照してください。
cursor と portable history は native リプレイ権限を与えません。

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

この五つのコマンドはモデル呼び出し 3 + 4 + 14 + 1 + 4 = 26 回、設定出力トークン 212,992 に制限され、入力トークンは別です。ドル価格の証明にも支出の承認にもなりません。支出が許可されているモデルと経路を使い、ログは非公開にしてください。元の作業量の qualification では shape override だけを外し、明示した cap/再要求の選択は記録したままにします。この明示設定では ToT + Baldwin + サーバー全体で最大 175 呼び出し / 設定出力トークン 1,433,600 です。

ToT は空でない最終 `EXPR`/`CHECK` を出力しなければなりません。実際の式を独立に評価し、4 を一つ、7 を一つ、8 を二つ正確に使い、結果が 24 であることを確認してください。モデルのスコアは算術 oracle ではありません。Baldwin は両モードで実際の pipeline fitness と hit 数を示す必要があります。特定の進化 trace を強制したり、live learner を offline oracle に置き換えたりしないでください。サーバーは空でないモデル/judge 出力で、シリアライズ/provider failure なしに選択した第 4 ターンへ到達する必要があります。30 は四つの effort を観測する sweep であり、reasoning トークン、実行時間、正確性の単調性は約束されません。空の回答は Outcome を保持したまま exit 2 になります。29 は回答テキストなしで tool call を返すことが正当にあり得ます。これは envelope の inspection であり、天気回答の benchmark ではありません。Provider failure は引き続き nonzero です。

### チェックポイントと進化

例08は terminal チェックポイントを fork して新しい user turn を始める既存の流れを保持します。
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
SSE 配信後はリプレイしません。例38は両 dialect を広告し、初期/更新 task snapshot を表示します。
初期 task は完了した回答ではありません。server の応答 encoding は method 表記とは別に
`A2A-Version` header が選択します。1.0 は PascalCase method、flat part、
`returnImmediately` を使い、stream の status/artifact update を累積します。caller の回答は
完了/中断 agent status text、最初の artifact text、最後の agent history text の順に選びます。

## メンタルモデル — 3 層、真ん中に JSON

各サンプルは、次の 3 つの構成のいずれかです。

1. **組み込みノードだけ**(02、04、07、14): `llm_call`、`tool_dispatch`、モックプロバイダーのノード。
   グラフ全体を JSON で組み立て、サブクラスは作りません。`create_react_graph()` が生成するものに最も近い形です。
2. **カスタムの `GraphNode` サブクラス**(05、09、10、25): `run(NodeInput)` の本体を自分で書き、
   `NodeOutput` を通して `ChannelWrite`、`Send`、`Command` を出力します。Send による fan-out と、
   Command による経路の上書きは、この形で書きます。
3. **型付き provider リクエストと Outcome**(13、15、16、17): SDK admission、順序付きイベント、不変 Outcome を使用します。descriptor interpreter や WebSocket adapter はありません。

グラフ定義は JSON 形式(`std::map<std::string, json>`)です。
[Python のサンプル](../bindings/python/examples/)も同じトポロジー形式を使います。
provider リクエストと Outcome は、トポロジー JSON とは別の型付きオブジェクトです。

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
画像、動画、decisions は SchemaProvider のエンドポイントで、`Provider::complete_endpoint` で呼び出します
（リファレンスの「エンドポイント呼び出し」を参照）。NeoGraph 0.16.0 は型付きクライアント
`ImagesClient`、`VeoClient`、`DecisionsClient` を削除しました。CHANGELOG を参照してください。
