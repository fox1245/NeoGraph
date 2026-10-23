<!-- neograph-i18n: source=bindings/python/examples/README.md locale=ja source_sha256=2769be0d70b9216a7b558b1115895f41285a6c9937abefc5e2449c5f8da761a3 -->
# Python API の例

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

この 47 本のスクリプトは、グラフの状態、ルーティング、ツール、プロバイダーリクエスト、Program API、プロトコルのホスティングを示します。各スクリプトが示すのは 1 つの事柄です。ほとんどはオフラインで動きます。プロバイダーの代わりにスクリプト化したモデルが応答するため、API キーもネットワークも不要です。実際のモデルも呼べるサンプルは `--live` を受け付けます。

**Offline** 列は、各行が必要とするものを示します。`yes`: キーもネットワークも不要で、`bindings/python/tests/test_examples_offline.py` が自動でテストします。`key`: プロバイダーの API キーが必要です。`service`: 別のプロセス、サーバー、または追加のパッケージが必要です。`paired`: その行に書かれたサンプルと一緒に、オフラインで動きます。

## セットアップ

```bash
pip install neograph-engine
python 01_minimal.py
```

このディレクトリでコマンドを実行してください。オフラインのサンプルにはこれ以外に必要なものはありません。wheel ではなくソースビルドで実行するには、`-DNEOGRAPH_BUILD_PYBIND=ON` で構成し、`PYTHONPATH` をビルドディレクトリと `bindings/python` に設定します。ビルドツリーはパッケージの Python 依存関係をインストールしないので、先に `pip install 'pydantic>=2' certifi` を実行してください。`import neograph_engine` には pydantic が必要で、certifi がない場合 TLS はホストの信頼ストアを使います。

## ここから始める

順番に読んでください。1 つ進むごとに考え方が 1 つ増えます。どれもオフラインで動きます。

| 手順 | サンプル | Offline | 学ぶこと |
|---|---|---|---|
| 1 | [`01_minimal.py`](01_minimal.py) | yes | 最初のグラフ。`@ng.node` で登録した関数が 1 つのチャネルを読んで別のチャネルに書き込み、21 が 42 になる。 |
| 2 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | yes | Python の `Tool` と組み込みの `tool_dispatch` ノード。スクリプト化したツール呼び出しが `42` として返る。 |
| 3 | [`03_send_fanout.py`](03_send_fanout.py) | yes | `Send` が項目ごとに 1 つの分岐を実行時に作り、分岐の結果が合流する。8 つの二乗値が順不同で揃う。 |
| 4 | [`05_openai_provider.py`](05_openai_provider.py) | yes | 組み込みの `llm_call` ノードによるモデル呼び出し 1 回。スクリプト化したモデルが `hello world` と答え、`--live` ではホスト型のモデルを呼ぶ。 |
| 5 | [`06_react_agent.py`](06_react_agent.py) | yes | ReAct ループ。`llm_call`、`tool_dispatch`、`has_tool_calls` の条件付きエッジ。`calc` が `4053` を返し、そのあとモデルが答える。 |
| 6 | [`77_dynamic_interrupt.py`](77_dynamic_interrupt.py) | yes | Human-in-the-Loop。ノードが `NodeInterrupt` を送出し、チェックポイントが実行を保ち、`resume` が人の答えをノードに返す。 |
| 7 | [`29_program_hello.py`](29_program_hello.py) | yes | Program。短い JavaScript の Program が、Python ノードを持つグラフを 1 つ定義し、`LocalProgramHost` で実行する。 |

手順 7 のあとは、次の表で目的から選ぶか、段階別の索引をたどってください。

## やりたいことから探す

目的を選んで、そのサンプルを読んでください。行は基本からプロトコルへと並んでいます。

| やりたいこと | 読むサンプル | Offline |
|---|---|---|
| 最初のグラフを動かす | [`01_minimal.py`](01_minimal.py) | yes |
| グラフに Python のツールを追加する | [`02_tool_dispatch.py`](02_tool_dispatch.py) | yes |
| Send で分岐を fan-out する | [`03_send_fanout.py`](03_send_fanout.py) | yes |
| asyncio で複数のグラフを同時に実行する | [`04_async_concurrent.py`](04_async_concurrent.py) | yes |
| グラフからモデルを呼び出す | [`05_openai_provider.py`](05_openai_provider.py) | yes |
| ツールを使うエージェントループを作る | [`06_react_agent.py`](06_react_agent.py) | yes |
| 意図に応じてルーティングする | [`08_intent_routing.py`](08_intent_routing.py) | yes |
| 次のノードの選択と状態の書き込みを 1 ステップで行う | [`10_command_routing.py`](10_command_routing.py) | yes |
| チャネルが書き込みをまとめる方法(overwrite か append)を選ぶ | [`79_channels.py`](79_channels.py) | yes |
| 独自のリデューサーやエッジ条件を登録する | [`80_custom_registries.py`](80_custom_registries.py) | yes |
| 実行をリアルタイムに観察する | [`75_stream_modes.py`](75_stream_modes.py) | yes |
| 失敗するノードをバックオフ付きでリトライする | [`76_retry_policy.py`](76_retry_policy.py) | yes |
| ノードが先に進む前に、人に尋ねられるようにする | [`77_dynamic_interrupt.py`](77_dynamic_interrupt.py) | yes |
| ステップ上限でループを止める | [`78_max_steps.py`](78_max_steps.py) | yes |
| 承認を待って止め、再開する | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | yes |
| ツール呼び出しを許可、拒否、割り込みする | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) | yes |
| 保存した状態を読み、フォークする | [`09_state_management.py`](09_state_management.py) | yes |
| グラフを JSON として保存、読み込みする | [`14_graph_to_json.py`](14_graph_to_json.py) | yes |
| トークンを LangChain 形式のチャンクとしてストリーミングする | [`19_streaming_messages.py`](19_streaming_messages.py) | yes |
| OpenTelemetry で実行をトレースする | [`20_otel_tracing.py`](20_otel_tracing.py) | yes |
| I/O バウンドのツールを並行して実行する | [`25_async_tools.py`](25_async_tools.py) | yes |
| JavaScript の Program を実行する | [`29_program_hello.py`](29_program_hello.py) | yes |
| 並列の分岐を join し、join が止まる理由を調べる | [`30_program_barrier_join.py`](30_program_barrier_join.py) | yes |
| 冗長な 2 つのソースのうち、先に届いたほうを待つ | [`31_program_any_of_join.py`](31_program_any_of_join.py) | yes |
| 失敗した Program のノードをオペレーターに回す | [`32_program_failure_routing.py`](32_program_failure_routing.py) | yes |
| Program が使える量に上限を設ける | [`33_program_budgets.py`](33_program_budgets.py) | yes |
| Program の実行を一時停止して再開する | [`34_program_pause_resume.py`](34_program_pause_resume.py) | yes |
| モデル呼び出しを繰り返さずに Program の実行をフォークする | [`35_program_fork.py`](35_program_fork.py) | yes |
| クラッシュ後に Program の実行を復旧する | [`36_program_crash_recovery.py`](36_program_crash_recovery.py) | yes |
| Program のノードにモデルを結び付け、使用量を読む | [`37_program_model_node.py`](37_program_model_node.py) | yes |
| コンパイラーの診断から Program を修復する | [`38_program_diagnostics.py`](38_program_diagnostics.py) | yes |
| 2 つの実行を競わせ、遅いほうをキャンセルする | [`39_program_main_race.py`](39_program_main_race.py) | yes |
| 遅いノードの試行をタイムアウトさせてリトライする | [`40_program_attempt_timeout.py`](40_program_attempt_timeout.py) | yes |
| MCP サーバーのツールを使う | [`26_mcp_tools.py`](26_mcp_tools.py) | yes |
| エージェントを A2A でホストする | [`27_a2a_server.py`](27_a2a_server.py) | yes |
| Python から A2A エージェントを呼び出す | [`45_a2a_client.py`](45_a2a_client.py) | yes |
| エージェントを ACP でホストする | [`28_acp_agent.py`](28_acp_agent.py) | yes |
| エージェントに自分のグラフを書き換えさせる | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | yes |
| Reflexion、Self-Ask、ディベートを試す | [`11_reflexion.py`](11_reflexion.py) | key |

## 段階別の索引

### Core エンジン

ノード、チャネル、ルーティング、ストリーミング、リトライ、割り込みを、モデルを使わずに扱います。サンプル 14 はスクリプトの隣に `my_graph.json` を書き、15 がそれを読むので、この順に実行してください。

| # | サンプル | Offline | 示すもの |
|---|---|---|---|
| 01 | [`01_minimal.py`](01_minimal.py) | yes | `@ng.node` で登録した関数がチャネルを読み、2 倍にして書き戻す。21 が 42 になる。 |
| 02 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | yes | Python の `Tool` と組み込みの `tool_dispatch` ノード。スクリプト化したツール呼び出しが `42` として返る。 |
| 03 | [`03_send_fanout.py`](03_send_fanout.py) | yes | 8 つの `Send` 分岐が二乗値 `[0, 1, 4, 9, 16, 25, 36, 49]` をまとめる。append の順序は前提にしない。 |
| 04 | [`04_async_concurrent.py`](04_async_concurrent.py) | yes | `asyncio.gather` で 8 つの非同期実行を回すと `[0, 2, 4, 6, 8, 10, 12, 14]` になる。ストリーミング実行はノードイベントを出す。 |
| 10 | [`10_command_routing.py`](10_command_routing.py) | yes | `Command` が次のノードを選び、状態を書く。入力 200、50、-10 が、それぞれ accept、manual、reject に進む。 |
| 14 | [`14_graph_to_json.py`](14_graph_to_json.py) | yes | グラフ定義は素の JSON。dict から実行し、`my_graph.json` として保存する。 |
| 15 | [`15_graph_from_json.py`](15_graph_from_json.py) | yes | サンプル 14 の `my_graph.json` を読み込み、5 を 10 に、100 を 200 に 2 倍にする。先に 14 を実行する必要がある。カスタムのノードタイプはコードで登録する。 |
| 19 | [`19_streaming_messages.py`](19_streaming_messages.py) | yes | `message_stream` がトークンイベントを LangChain 形式のチャンクに変換する。スクリプト化した 5 つのトークンが `Octopuses have three hearts.` になる。 |
| 75 | [`75_stream_modes.py`](75_stream_modes.py) | yes | ストリームモード。同じ実行を VALUES、UPDATES、EVENTS、DEBUG の各モードで観察し、モードごとに届くイベントを数える。 |
| 76 | [`76_retry_policy.py`](76_retry_policy.py) | yes | `RetryPolicy`。ノードが 2 回失敗し、エンジンが指数バックオフでリトライする。上限を小さくすると、リトライを使い切ったときに呼び出し側が何を受け取るかが分かる。 |
| 77 | [`77_dynamic_interrupt.py`](77_dynamic_interrupt.py) | yes | `NodeInterrupt`。ノードがこれを送出し、ペイロードが呼び出し側に尋ねる内容を伝え、`resume` が答え(承認または拒否)をノードに返す。 |
| 78 | [`78_max_steps.py`](78_max_steps.py) | yes | `max_steps`。ループがステップ上限で止まり、結果がそれを知らせる(`max_steps_exhausted`、`RunStatus.StepLimit`)。最後のステップで終わった実行には印が付かない。 |
| 79 | [`79_channels.py`](79_channels.py) | yes | チャネルとリデューサー。`overwrite` と `append` の違い、`initial` の値、チャネルのバージョン(全チャネルで 1 つの書き込みカウンターを共有)。 |
| 80 | [`80_custom_registries.py`](80_custom_registries.py) | yes | 独自のリデューサーと条件。`ReducerRegistry.register_reducer` で 3 人の judge のスコアの最大値を順序に関係なく残し、`ConditionRegistry.register_condition` でその値を使ってルーティングする。 |

### エージェントパターン

グラフの中のモデル(ツールの有無は問いません)です。`key` の行はホスト型のモデルを呼び出します。

| # | サンプル | Offline | 示すもの |
|---|---|---|---|
| 05 | [`05_openai_provider.py`](05_openai_provider.py) | yes | `llm_call` によるモデル呼び出し 1 回。スクリプト化したモデルが `hello world` と答え、`--live` ではホスト型のモデルを呼ぶ。 |
| 06 | [`06_react_agent.py`](06_react_agent.py) | yes | Responses プロトコル上の ReAct。モデルが出した `calc` 呼び出しが `4053` を返し、そのあとモデルが答える。 |
| 08 | [`08_intent_routing.py`](08_intent_routing.py) | yes | 3 つの質問が `route_channel` を通って math、translate、general の専門家へ進み、各専門家が答えを書く。 |
| 11 | [`11_reflexion.py`](11_reflexion.py) | key | Reflexion。actor と critic の呼び出しが、振り返りを次の試行へ引き継ぐ。範囲を絞った学習用のバージョン。 |
| 12 | [`12_self_ask.py`](12_self_ask.py) | key | Self-Ask。中間の質問と回答がスクラッチパッドに溜まり、そのあとで最終回答が出る。検索サービスは接続していない。 |
| 13 | [`13_multi_agent_debate.py`](13_multi_agent_debate.py) | key | 2 つの `Send` 分岐が反対の立場で主張し、1 人の judge が統合された議論を読む。 |
| 16 | [`16_deep_research_chat.py`](16_deep_research_chat.py) | key | 調査モード付きの Gradio チャット。Web 検索ではなくモデルの知識を使う並列の researcher が、3 つの質問からなるレポートを作る。`gradio` が必要。 |
| 17 | [`17_deep_research_crawl4ai.py`](17_deep_research_crawl4ai.py) | key | 同じ調査グラフに、Crawl4AI による実際の Web 検索と、任意の PostgreSQL チェックポイントを加えたもの。`gradio`、`requests`、Crawl4AI サーバーが必要。 |
| 22 | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | yes | グラフが自分の出力を採点し、失敗すると改訂版の定義をモデルに求める。未登録のノードタイプを使った書き換えは理由付きで拒否され、admission を通った書き換えがコンパイルされて、2 回目の反復で目標を達成する。`--live` ではホスト型のモデルを呼ぶ。 |
| 23 | [`23_evolving_chat_agent.py`](23_evolving_chat_agent.py) | yes | 数回のチャットのあと、evolver モデルが新しいエージェント定義を提案する。`validate_agent` が admission を通し、新しいエンジンがチェックポイントストアを共有するので前の 4 件のメッセージが残り、`__graph_meta__` にバージョンとハッシュが記録される(アプリケーションのメタデータで、リプレイの証拠ではない)。 |

### Program API

制限付きの JavaScript Program API は、Core グラフの上で動きます(`ProgramRegistryBuilder`、`ProgramCompiler`、`LocalProgramHost`)。説明は [Program と QuickJS](../../../docs/python-binding.md#program-and-quickjs) にあります。join、失敗ルーティング、予算、一時停止と再開、fork、クラッシュ復旧については、[`bindings/python/tests/program_e2e/`](../tests/program_e2e/README.md) のエンドツーエンドシナリオで動作を確認しています。以下のサンプルは、それぞれこのうちの 1 つを `LocalProgramHost` で示します。どれもオフラインで動き、ノードがモデルを呼ぶ場面ではスクリプト化したモデルが答えます。共通の設定は `_program.py` にまとめてあり、サンプル 29 がその設定を 1 回だけ展開して見せます。

| # | サンプル | Offline | 示すもの |
|---|---|---|---|
| 29 | [`29_program_hello.py`](29_program_hello.py) | yes | JavaScript の Program が Python ノードを持つグラフを 1 つ定義し、`LocalProgramHost` で実行する。`status: Completed`、`value: 1`、`trace: ['work']` を出力する。 |
| 30 | [`30_program_barrier_join.py`](30_program_barrier_join.py) | yes | レビュアー 3 人が並列に動き、`barrier` が全員を待つ。1 人が別のノードへルーティングすると、Program は `Failed` で終わり、`P_CORE_BARRIER_STALLED` と、来なかったノードを示す witness が残る。 |
| 31 | [`31_program_any_of_join.py`](31_program_any_of_join.py) | yes | 通常のメンバー 2 つを待つ join は、片方を肩代わりする修正ノードが入ると止まる。コンパイラー自身の提案である `{any_of: [...]}` スロットをソースに適用すると、直した Program は完了する。 |
| 32 | [`32_program_failure_routing.py`](32_program_failure_routing.py) | yes | TEST が失敗し、2 回リトライされ、最後の失敗が `interruptBefore` 付きのハンドラーにルーティングされる。オペレーターが `retry` で再開すると Program は完了し、`abort` で再開すると、失敗終了ノードが Program を `plan.user_abort` 付きの `Failed` で終わらせる。 |
| 33 | [`33_program_budgets.py`](33_program_budgets.py) | yes | モデルに結び付けた `llm_call` ノード 2 つがそれぞれ 600 トークンを報告し、予算は 1,000 トークンなので、実行は `BudgetExhausted` で終わる。admission を通った予算と異なる予算での実行は、`P_START_BUDGET` で拒否される。 |
| 34 | [`34_program_pause_resume.py`](34_program_pause_resume.py) | yes | `interruptBefore` が b ノードの手前で実行を止め、`host.resume(run_id, value)` が続きを実行する。止まった時点の trace は `['a']`、再開後は `['b', 'c']` で、`main()` はオペレーターの答えを返して終わる。 |
| 35 | [`35_program_fork.py`](35_program_fork.py) | yes | 終わったモデル呼び出しの隣で止まった実行を、別の答えでフォークする。終わった呼び出しはチェックポイントからリプレイされるので、スクリプト化したモデルが受け取るリクエストは合計 1 回で、フォークにトークンは課金されない。 |
| 36 | [`36_program_crash_recovery.py`](36_program_crash_recovery.py) | yes | プロセス A は Program を質問まで実行し、答えを受けてモデルを呼んだあと、異常終了する。プロセス B が同じ `sqlite_directory` を開いて `reconnect` を呼ぶと、実行が完了し、ジャーナルに残った応答がリプレイされ、呼び出しの課金は 1 回だけになる。対象は Linux と macOS。 |
| 37 | [`37_program_model_node.py`](37_program_model_node.py) | yes | Program の `llm_call` ノードは、ホストが `node_context` でプロバイダーに結び付ける。スクリプト化したモデルが報告した 30 + 12 トークンは、`usage.model_tokens: 42`、`remaining_budget`、価格付きの金額記録に反映される。この結び付けのないホストは、`P_ADMIT_PROVIDER` で Program を拒否する。 |
| 38 | [`38_program_diagnostics.py`](38_program_diagnostics.py) | yes | スクリプト化したモデルが、エッジのノード名を書き間違える。コンパイルは `P_CORE_E3`、修正案、候補とともに失敗する。修復ループが修正案を適用し、`soundness_check=True` で再コンパイルして、Program を admission に通す。モデルの呼び出しは 1 回だけ。 |
| 39 | [`39_program_main_race.py`](39_program_main_race.py) | yes | `main()` が `ng.race`、`ng.quorum`、`ng.await` の下で Core の実行を複数開始する。先に終わった実行が勝ち、遅い実行はキャンセルトークンの変化を検知し、期限を過ぎた `ng.await` は Program を `P_AWAIT_TIMEOUT` 付きの `TimedOut` で終わらせる。 |
| 40 | [`40_program_attempt_timeout.py`](40_program_attempt_timeout.py) | yes | 5 秒かかるノードは `attempt_timeout_ms` で止められ、`NG.Timeout` で失敗し、リトライで成功する。`usage.node_retries` は 1。常に遅いノードは、Program を `NG.Timeout` 付きの `Failed` で終わらせる。 |

### プロトコル: MCP、A2A、ACP

ここでの MCP と A2A はクライアント(`ng.mcp`、`ng.a2a`)です。A2A と ACP のエージェントは、公式の Python SDK と `ProtocolHostAdapter` でホストします。この階層のサンプルはすべてオフラインで動きます。空きポートに自前のサーバーを起動するか、エージェントを子プロセスとして実行します。SDK を使うサンプルには `neograph-engine[a2a]` または `neograph-engine[acp]` が必要で、入っていなければスキップされます。サーバーについては、`bindings/python/tests/test_protocol_sdk_e2e.py` がさらに詳しく検証します。

| # | サンプル | Offline | 示すもの |
|---|---|---|---|
| 26 | [`26_mcp_tools.py`](26_mcp_tools.py) | yes | ローカルの MCP サーバーを起動して `fetch` を検出し、明示的な再入ポリシーで 3 回呼び出す。MCP を有効にしたビルドが必要で、スモークテストは Linux で行う。 |
| 27 | [`27_a2a_server.py`](27_a2a_server.py) | yes | 公式の A2A SDK でエージェントをホストする。`127.0.0.1:9999` のエージェントカードと JSON-RPC、ストリーミングされるアーティファクト。`approve:` で始まるプロンプトはグラフを停止させる。タスクは `input-required` になり、そのタスクへの次のメッセージが実行を再開する。`--self-check` は空きポートでサーバーを起動し、SDK 自身のクライアントで 2 ターンと 1 回の承認を実行する。`neograph-engine[a2a]` が必要。 |
| 28 | [`28_acp_agent.py`](28_acp_agent.py) | yes | 公式の ACP SDK で stdio 上にエージェントをホストする。initialize、セッションの作成、prompt、トークン更新、`end_turn`。`approve:` で始まるプロンプトはグラフを停止させる。エージェントは `session/request_permission` を送り、クライアントが選んだ選択肢で再開する。`--self-check` はエージェントを子プロセスとして実行し、SDK のクライアントで 2 ターンと 1 回の承認を実行する。`neograph-engine[acp]` が必要。 |
| 45 | [`45_a2a_client.py`](45_a2a_client.py) | yes | A2A クライアント。`ng.a2a.A2AClient` がエージェントカードを取得してメッセージを送り、サンプル 27 の SDK でホストしたエージェントのストリームを読む。そのエージェントは、このスクリプトが空きポートでスレッドとして起動する。`neograph-engine[a2a]` が必要。 |

### 永続化と運用

チェックポイント、状態、キャッシュ、トレーシング、トランスポート、ツールの並行実行を扱います。

| # | サンプル | Offline | 示すもの |
|---|---|---|---|
| 07 | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | yes | チェックポイントが支払いのディスパッチの手前で実行を止め、承認すると、模擬の支払いがちょうど 1 件だけ実行される。 |
| 09 | [`09_state_management.py`](09_state_management.py) | yes | `get_state` と `fork`。alpha は count 11 に達し、beta がその値をフォークし、未知のスレッドにはチェックポイントがない。 |
| 18 | [`18_node_cache.py`](18_node_cache.py) | key | 2 つのトピックにまたがる 5 回の実行で、プロバイダーを使うノードは 2 回だけ実行され、繰り返された入力はキャッシュした書き込みをリプレイする。 |
| 20 | [`20_otel_tracing.py`](20_otel_tracing.py) | yes | 実行と 3 つのノードの OpenTelemetry span をコンソールに出力する。最終的な経路は `['A', 'B', 'C']`。`opentelemetry-api` と `opentelemetry-sdk` が必要。 |
| 21 | [`21_http2_transport.py`](21_http2_transport.py) | key | 同じ SDK で HTTP/1.1 と HTTP/2 を比べる。TLS の Chat エンドポイントと、HTTP/2 対応の libcurl が必要。結果はこのエンドポイントについてのもので、順位付けではない。 |
| 24 | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) | yes | ツールゲートが、どちらの兄弟ツールが動く前にも実行を止める。拒否すると `list_files` だけ、承認すると両方が 1 回ずつ実行される。シェル操作は模擬。 |
| 25 | [`25_async_tools.py`](25_async_tools.py) | yes | I/O バウンドの作業で、同期の `Tool` と、処理が重なる `AsyncTool` を比べ、続いて CPU バウンドの作業で GIL の境界を示す。時間の比率は観測値で、スモークテストは Linux で行う。 |

## オフライン、スクリプト、ライブ

モデルで動くサンプルは、`_common.py` の `example_provider(script)` でプロバイダーを作ります。`--live` を付けない限り、`_scripted.ScriptedModel` が応答します。これは `127.0.0.1` で動く小さなサーバーで、スクリプトに従って OpenAI の Chat または Responses プロトコルで応答します(テキスト、ツール呼び出し、ストリーミング、使用量、HTTP の失敗)。実際の `SchemaProvider` によるリクエストの構築、SSE の解析、outcome の処理が、このサーバーを相手に動きます。これはモデルではありません。何を返すかはスクリプトが決めます。

```python
from _common import example_provider
from _scripted import call_tool, say

script = [call_tool("calc", expression="2 + 3"), say("5")]   # call 1 asks for a tool, call 2 answers
with example_provider(script) as chosen:
    provider = chosen.provider        # a SchemaProvider; chosen.scripted is None with --live
```

スクリプトは、`turn.last_user_text`、`turn.system_text`、`turn.last_tool_result` を読む関数 `(turn) -> Reply` にもできます。`chosen.scripted.call_count` で、モデル呼び出しが何回あったかを検証できます。規則は `_scripted.py` の docstring にあります。

`--live`、`NG_EXAMPLE_LIVE=1`、`OPENAI_API_BASE`、`NG_PROVIDER_DESCRIPTOR` のいずれかを使うと、代わりに設定済みのエンドポイントが選ばれます。ホスト型のエンドポイントには `OPENAI_API_KEY` が必要で、任意で `OPENAI_MODEL`(既定は `gpt-6-luna`)を使います。どちらもプロセスの環境からのみ読みます。`_common.py` は `.env` ファイルを読み込まず、キーがない場合は成功した実行ではなくエラーです。`_common.py` は、closed な OpenAI の Chat または Responses ディスクリプターを admission に通してから `SchemaProvider` を構築します。型付き SDK は libcurl の HTTP を使い、削除済みのプロバイダークラス、completion パラメーター、WebSocket 経路、トランスポートバックエンドの選択子は使えません。`OPENAI_API_BASE` は origin またはゲートウェイのプレフィックスであり、完全な `/v1/chat/completions` 経路ではありません。CA がシステムの信頼ストアにない HTTPS ピアには、`NG_EXAMPLE_CA_FILE` も必要です。代わりに `NG_PROVIDER_DESCRIPTOR` で、admission を通る完全なディスクリプター JSON ファイルを指定することもできます。型付きリクエストと outcome の契約は、[Python バインディングガイド](../../../docs/python-binding.md)を参照してください。

## 状態とスケジューリング

ノードは現在のスーパーステップの状態を読み、チャネルへの書き込みを返します。エンジンは、次のスーパーステップの前に、宣言されたリデューサーでその書き込みをマージします。`Send` は分岐ごとの入力を渡し、`Command` は状態を更新して次のノードを選びます。ワーカープールを使っても、CPython の GIL のもとでは、CPU バウンドの Python コールバックは並列に実行されません。

`GraphEngine.compile()` は、Python で書いた辞書のほか、JSON から読み込んだ辞書も受け付けます。定義が保存するのは接続関係で、実行可能な Python クラスではありません。保存した定義をコンパイルする前に、カスタムノードのファクトリーを登録してください。サンプル 14 と 15 は、スクリプトの隣にある `my_graph.json` をそれぞれ書き込み、読み込みます。この 2 つを検証するときは、使い捨てのチェックアウトか、このディレクトリのコピーを使ってください。

## 対話とプロトコルのシナリオ

16 と 17 では `gradio` をインストールしてスクリプトを起動し、表示されたローカル URL を開きます。`hello` を送信し、続いて `research apples` を送信します。17 では、実際の Crawl4AI の `/md` サービス、またはそれを忠実に再現したローカルのピアが、`{"success": true, "markdown": "..."}` を返す必要があります。Postgres は、実行中のビルドが対応していて、データベースに接続できる場合にだけ設定してください。エンジンの状態と UI の履歴は、別々に確認してください。

サンプル 27 と 28 は、ワイヤーの処理に公式の Python SDK を、チェックポイント対応のエンジン実行に `ProtocolHostAdapter` を使います。A2A の検証には、エージェントカードを取得し、メッセージを送り、ストリームを受け取る SDK クライアントが必要です。ACP の検証には、`initialize`、`session/new`、`session/prompt` を送るための、クライアントのサブプロセス接続が必要です。stdout はプロトコルメッセージ専用です。同じコンテキスト/セッションで 2 つ目のプロンプトを送り、`(turn 2)` が返ることを確かめてください。`approve: deploy` を送ってみてください。A2A はステータスメッセージが ``Run `deploy`?`` の `input-required` タスクで答え、同じタスク ID でテキスト `approve` を送ると `NeoGraph ran: deploy` で完了します。ACP は権限リクエストを送り、選択肢 `approve` が同じテキストでターンを終わらせます。キャンセルは、成功したように見える代替結果を作らず、実行中のリクエストを止める必要があります。

永続的な ACP セッションには、`NEOGRAPH_ACP_POSTGRES_URL` と `NEOGRAPH_ACP_SQLITE_PATH` のどちらか 1 つだけを設定してください。最初のプロンプトが完了すると、`session/load` に必要なチェックポイントが作られます。セッションごとにアクティブなエージェントプロセスは 1 つにしてください。チェックポイントストアは、プロセスをまたぐ同時書き込みを直列化しません。このサンプルはリッチコンテンツブロックをそのまま返しますが、画像や音声の内容は解釈せず、エディターのファイルシステムやターミナルのコールバックも提供しません。

## ディストリビューション名とインポート名

```python
import neograph_engine as ng
from neograph_engine.llm import SchemaProvider
```

ディストリビューション名は `neograph-engine` です。PyPI の `neograph` は無関係のプロジェクトです。

## サンプルを追加する

番号は再利用しません。既存のどの番号よりも 1 大きい番号を使ってください。新しいサンプルは、ファイル 1 つと表の 1 行で構成されます。

1. ファイル名を `NN_short_name.py` とし、モジュールの docstring で始めます。数行の説明、`Run:` のコマンド、続いてテスト用の注釈を書きます。
2. その docstring にテスト方法を書きます。オフラインのサンプルは必ず出力する行を書き、キーやサービスが必要なサンプルはオフラインで動かせない理由を書きます(注釈の文法は `examples/run_example_smoke.py` の先頭にあります)。
3. モデルで動くプロバイダーは `example_provider(script)` で作り、既定でオフラインになり `--live` を受け付けるようにします。
4. 該当する段階の表に、Offline の値付きで行を追加します。
5. `python -m pytest bindings/python/tests/test_examples_offline.py` と `python scripts/check_examples.py` を実行します。後者は、注釈のないサンプル、重複した番号、注釈と食い違う表があると失敗します。

```python
"""NN — one line saying what this shows.

Run:
    python NN_short_name.py           # scripted model, no key
    python NN_short_name.py --live    # hosted model; OPENAI_API_KEY in the environment

smoke-expect: provider: scripted
smoke-expect: final answer: 5
"""
```

サンプルをオフラインで動かせない場合は、`smoke-expect` の行の代わりに `smoke-skip: <reason>` を書きます。`smoke-requires: module1, module2` は、モジュールがないときにテストをスキップします。
