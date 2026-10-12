<!-- neograph-i18n: source=bindings/python/examples/README.md locale=ja source_sha256=a56e3bfe852d264dcadce3f7ea680e0c8b6f8dca978c16eb8d21270ed2254900 -->
# Python API の例

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

この 28 本のスクリプトは、グラフの状態、ルーティング、ツール、プロバイダーへのリクエスト、プロトコルのホスティングを示します。各スクリプトが示すのは 1 つの事柄です。ほとんどはオフラインで動きます。プロバイダーの代わりにスクリプト化したモデルが応答するため、API キーもネットワークも不要です。実際のモデルも呼べるサンプルは `--live` を受け付けます。

**Offline** 列は、各行が必要とするものを示します。`yes`: キーもネットワークも不要で、`bindings/python/tests/test_examples_offline.py` が自動でテストします。`key`: プロバイダーの API キーが必要です。`service`: 別のプロセス、サーバー、または追加のパッケージが必要です。`paired`: その行に書かれたサンプルと一緒に、オフラインで動きます。

## セットアップ

```bash
pip install neograph-engine
python 01_minimal.py
```

このディレクトリでコマンドを実行してください。オフラインのサンプルにはこれ以外に必要なものはありません。wheel ではなくソースビルドで実行するには、`-DNEOGRAPH_BUILD_PYBIND=ON` で構成し、`PYTHONPATH` をビルドディレクトリと `bindings/python` に設定します。ビルドツリーはパッケージの Python 依存関係をインストールしないので、先に `pip install 'pydantic>=2' certifi` を実行してください。`import neograph_engine` には pydantic が必要で、certifi がない場合 TLS はホストの信頼ストアを使います。

## ここから始める

順番に読んでください。1 つ進むごとに考え方が 1 つ増えます。

| 手順 | サンプル | Offline | 学ぶこと |
|---|---|---|---|
| 1 | [`01_minimal.py`](01_minimal.py) | yes | カスタムノードがチャンネルを読み、2 倍にして書き戻します。21 が 42 になります。 |
| 2 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | yes | Python の `Tool` と組み込みの `tool_dispatch` ノード。スクリプト化したツール呼び出しが `42` として返ります。 |
| 3 | [`05_openai_provider.py`](05_openai_provider.py) | yes | 組み込みの `llm_call` ノードによるモデル呼び出し 1 回。スクリプト化したモデルが `hello world` と答えます。 |
| 4 | [`06_react_agent.py`](06_react_agent.py) | yes | ReAct ループ。`llm_call`、`tool_dispatch`、`has_tool_calls` の条件付きエッジ。`calc` が `4053` を返し、モデルが答えます。 |
| 5 | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | yes | チェックポイントが支払いのディスパッチ前で止まり、承認すると模擬の支払いがちょうど 1 回だけ再開します。実際のお金は動きません。 |
| 6 | [`14_graph_to_json.py`](14_graph_to_json.py) | yes | グラフ定義は素の JSON です。dict から実行し、`my_graph.json` として保存します。サンプル 15 がそれを読み込みます。 |

## やりたいことから探す

目的を選んで、そのサンプルを読んでください。

| やりたいこと | 読むサンプル | Offline |
|---|---|---|
| 最初のグラフを動かす | [`01_minimal.py`](01_minimal.py) | yes |
| グラフに Python のツールを追加する | [`02_tool_dispatch.py`](02_tool_dispatch.py) | yes |
| Send で分岐を fan-out する | [`03_send_fanout.py`](03_send_fanout.py) | yes |
| asyncio で複数のグラフを同時に実行する | [`04_async_concurrent.py`](04_async_concurrent.py) | yes |
| グラフからモデルを呼び出す | [`05_openai_provider.py`](05_openai_provider.py) | yes |
| ツールを使うエージェントループを作る | [`06_react_agent.py`](06_react_agent.py) | yes |
| 意図に応じてルーティングする | [`08_intent_routing.py`](08_intent_routing.py) | yes |
| 次のノードを選びつつ 1 ステップで状態を書く | [`10_command_routing.py`](10_command_routing.py) | yes |
| 承認を待って止め、再開する | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | yes |
| ツール呼び出しを許可、拒否、割り込みする | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) | yes |
| 保存した状態を読み、フォークする | [`09_state_management.py`](09_state_management.py) | yes |
| グラフを JSON として保存、読み込みする | [`14_graph_to_json.py`](14_graph_to_json.py) | yes |
| トークンを LangChain 形式のチャンクとしてストリーミングする | [`19_streaming_messages.py`](19_streaming_messages.py) | yes |
| OpenTelemetry で実行をトレースする | [`20_otel_tracing.py`](20_otel_tracing.py) | yes |
| I/O 中心のツールを並行して実行する | [`25_async_tools.py`](25_async_tools.py) | yes |
| MCP サーバーのツールを使う | [`26_mcp_tools.py`](26_mcp_tools.py) | yes |
| エージェントを A2A で公開する | [`27_a2a_server.py`](27_a2a_server.py) | service |
| エージェントを ACP で公開する | [`28_acp_agent.py`](28_acp_agent.py) | service |
| Reflexion、Self-Ask、ディベートを試す | [`11_reflexion.py`](11_reflexion.py) | key |
| エージェントに自分のグラフを書き換えさせる | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | key |

## 段階別の索引

### はじめにと Core エンジン

モデルを使わない残りの Core 機能です。サンプル 14 はスクリプトの隣に `my_graph.json` を書き、15 がそれを読むので、この順に実行してください。

| # | サンプル | Offline | 示すもの |
|---|---|---|---|
| 03 | [`03_send_fanout.py`](03_send_fanout.py) | yes | 8 つの `Send` 分岐が二乗値 `[0, 1, 4, 9, 16, 25, 36, 49]` をまとめます。append の順序は前提にしません。 |
| 04 | [`04_async_concurrent.py`](04_async_concurrent.py) | yes | `asyncio.gather` で 8 つの非同期実行を回すと `[0, 2, 4, 6, 8, 10, 12, 14]` になり、ストリーミング実行はノードイベントを出します。 |
| 10 | [`10_command_routing.py`](10_command_routing.py) | yes | `Command` が次のノードを選び、状態を書きます。入力 200、50、-10 がそれぞれ accept、manual、reject に進みます。 |
| 14 | [`14_graph_to_json.py`](14_graph_to_json.py) | yes | グラフ定義は素の JSON です。dict から実行し、`my_graph.json` として保存します。 |
| 15 | [`15_graph_from_json.py`](15_graph_from_json.py) | yes | サンプル 14 の `my_graph.json` を読み込み、5 を 10、100 を 200 に 2 倍にします。先に 14 を実行してください。カスタムノード型はコードで登録します。 |
| 19 | [`19_streaming_messages.py`](19_streaming_messages.py) | yes | `message_stream` がトークンイベントを LangChain 形式のチャンクに変換します。スクリプト化した 5 つのトークンが `Octopuses have three hearts.` になります。 |
| 75 | [`75_stream_modes.py`](75_stream_modes.py) | yes | ストリームモード。同じ実行を VALUES、UPDATES、EVENTS、DEBUG の各モードで観察し、モードごとに届くイベントを数えます。 |
| 76 | [`76_retry_policy.py`](76_retry_policy.py) | yes | `RetryPolicy`。ノードが 2 回失敗すると、エンジンが指数バックオフで再試行します。回数を減らすと、再試行を使い切ったときに呼び出し側が受け取るものが分かります。 |
| 77 | [`77_dynamic_interrupt.py`](77_dynamic_interrupt.py) | yes | `NodeInterrupt`。ノードがこれを送出すると、ペイロードが呼び出し側に尋ねる内容を伝え、`resume` が答え（承認または拒否）をノードに返します。 |
| 78 | [`78_max_steps.py`](78_max_steps.py) | yes | `max_steps`。ループがステップ上限で止まり、結果がそれを知らせます（`max_steps_exhausted`、`RunStatus.StepLimit`）。最後のステップで自然に終わった実行には印が付きません。 |
| 79 | [`79_channels.py`](79_channels.py) | yes | チャンネルと reducer。`overwrite` と `append` の違い、`initial` の値、そして全チャンネルが 1 つの書き込みカウンターを共有するチャンネルバージョン。 |
| 80 | [`80_custom_registries.py`](80_custom_registries.py) | yes | 独自の reducer と条件。`ReducerRegistry.register_reducer` で審査員 3 人のスコアの最大値を順序に関係なく残し、`ConditionRegistry.register_condition` でその値によって経路を選びます。 |

### エージェントパターン

グラフの中のモデル(ツールの有無は問いません)です。`key` の行はホスト型モデルを呼び出します。

| # | サンプル | Offline | 示すもの |
|---|---|---|---|
| 05 | [`05_openai_provider.py`](05_openai_provider.py) | yes | `llm_call` によるモデル呼び出し 1 回。スクリプト化したモデルが `hello world` と答え、`--live` ではホスト型モデルを呼びます。 |
| 06 | [`06_react_agent.py`](06_react_agent.py) | yes | Responses プロトコル上の ReAct。モデルが出した `calc` 呼び出しが `4053` を返し、モデルが答えます。 |
| 08 | [`08_intent_routing.py`](08_intent_routing.py) | yes | 3 つの質問が `route_channel` を通って math、translate、general の専門家に振り分けられ、各専門家が答えを書きます。 |
| 11 | [`11_reflexion.py`](11_reflexion.py) | key | Reflexion。actor と critic の呼び出しが振り返りを次の試行へ引き継ぎます。範囲を絞った学習用のバージョンです。 |
| 12 | [`12_self_ask.py`](12_self_ask.py) | key | Self-Ask。中間の質問と回答がスクラッチパッドに溜まってから最終回答が出ます。検索サービスは接続されていません。 |
| 13 | [`13_multi_agent_debate.py`](13_multi_agent_debate.py) | key | 2 つの `Send` 分岐が反対の立場で主張し、1 人の審判が統合された議論を読みます。 |
| 16 | [`16_deep_research_chat.py`](16_deep_research_chat.py) | key | 調査モード付きの Gradio チャット。Web 検索ではなくモデルの知識を使う並列 researcher が、3 つの質問からなるレポートを作ります。`gradio` が必要です。 |
| 17 | [`17_deep_research_crawl4ai.py`](17_deep_research_crawl4ai.py) | key | 同じ調査グラフに、Crawl4AI による実際の Web 検索と、任意の PostgreSQL チェックポイントを加えたものです。`gradio`、`requests`、Crawl4AI サーバーが必要です。 |
| 22 | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | key | グラフが自分の出力を採点し、失敗したらモデルに修正版の定義を求め、再コンパイルして再試行します。登録済みのノード型だけが許可されます。 |
| 23 | [`23_evolving_chat_agent.py`](23_evolving_chat_agent.py) | key | 許可されたグラフの書き換え後も会話のチェックポイントが残ります。バージョンとハッシュは `__graph_meta__` に記録されます(アプリケーションレベルのメタデータであり、リプレイの証拠ではありません)。 |

### Program API

制限付きの JavaScript Program API は Core グラフの上で動きます: `ProgramRegistryBuilder`、`ProgramCompiler`、`LocalProgramHost`。[Program and QuickJS](../../../docs/python-binding.md#program-and-quickjs) に説明があり、[`bindings/python/tests/program_e2e/`](../tests/program_e2e/README.md) のエンドツーエンドシナリオが join、失敗ルーティング、予算、一時停止と再開、fork、クラッシュ復旧を扱っています。 以下のサンプルは、それぞれこの中の 1 つを `LocalProgramHost` で示します。すべてオフラインで動き、ノードがモデルを呼ぶ場面ではスクリプト化したモデルが答えます。共通の設定は `_program.py` にあり、29 番のサンプルがその設定を 1 回だけ展開して見せます。

| # | サンプル | Offline | 示すもの |
|---|---|---|---|
| 29 | [`29_program_hello.py`](29_program_hello.py) | yes | JavaScript の Program が Python ノードを持つグラフを 1 つ定義し、`LocalProgramHost` で実行します。`status: Completed`、`value: 1`、`trace: ['work']` を出力します。 |
| 30 | [`30_program_barrier_join.py`](30_program_barrier_join.py) | yes | レビュアー 3 人が並列に動き、`barrier` が全員を待ちます。1 人が別のノードへルーティングすると、Program は `Failed` で終わり、`P_CORE_BARRIER_STALLED` と、来なかったノードを示す witness が残ります。 |
| 31 | [`31_program_any_of_join.py`](31_program_any_of_join.py) | yes | 通常のメンバー 2 つを待つ join は、片方を肩代わりする修正ノードが入ると止まります。コンパイラ自身の提案である `{any_of: [...]}` スロットをソースに適用すると、直した Program は完了します。 |
| 32 | [`32_program_failure_routing.py`](32_program_failure_routing.py) | yes | TEST が失敗して 2 回再試行され、最後の失敗は `interruptBefore` 付きのハンドラーへルーティングされます。オペレーターが `retry` で再開すると Program は完了し、`abort` で再開すると失敗終了ノードが `plan.user_abort` を付けて `Failed` で終わらせます。 |
| 33 | [`33_program_budgets.py`](33_program_budgets.py) | yes | バインドされた `llm_call` ノード 2 つが各 600 トークンを報告し、予算は 1,000 トークンなので、実行は `BudgetExhausted` で終わります。承認された予算と異なる予算での実行は `P_START_BUDGET` で拒否されます。 |
| 34 | [`34_program_pause_resume.py`](34_program_pause_resume.py) | yes | `interruptBefore` が b ノードの前で実行を止め、`host.resume(run_id, value)` が続きを実行します。停止時の trace は `['a']`、再開後は `['b', 'c']` で、`main()` はオペレーターの答えを返して終わります。 |
| 35 | [`35_program_fork.py`](35_program_fork.py) | yes | 終わったモデル呼び出しの隣で止まった実行を、別の答えで fork します。終わった呼び出しはチェックポイントからリプレイされるので、スクリプト化したモデルが受け取るリクエストは合計 1 回で、fork にトークンは課金されません。 |
| 36 | [`36_program_crash_recovery.py`](36_program_crash_recovery.py) | yes | プロセス A は Program を質問まで実行し、答えを受けてモデルを呼んだあと終了します。プロセス B が同じ `sqlite_directory` を開いて `reconnect` を呼ぶと実行が完了し、ジャーナルの応答はリプレイされ、呼び出しの課金は 1 回だけです。Linux と macOS のみ。 |
| 37 | [`37_program_model_node.py`](37_program_model_node.py) | yes | Program の `llm_call` ノードは、ホストが `node_context` でプロバイダーにバインドします。スクリプト化したモデルが報告した 30 + 12 トークンは `usage.model_tokens: 42`、`remaining_budget`、価格付きの課金記録に反映されます。バインドのないホストは `P_ADMIT_PROVIDER` で Program を拒否します。 |
| 38 | [`38_program_diagnostics.py`](38_program_diagnostics.py) | yes | スクリプト化したモデルがエッジのノード名を書き間違えます。コンパイルは `P_CORE_E3`、修正案、候補とともに失敗します。修復ループが修正案を適用し、`soundness_check=True` で再コンパイルして Program を承認します。モデルの呼び出しは 1 回だけです。 |
| 39 | [`39_program_main_race.py`](39_program_main_race.py) | yes | `main()` が `ng.race`、`ng.quorum`、`ng.await` の下で Core の実行を複数開始します。先に終わった実行が勝ち、遅い実行はキャンセルトークンの変化を検知し、期限を過ぎた `ng.await` は Program を `P_AWAIT_TIMEOUT` 付きの `TimedOut` で終わらせます。 |

### プロトコル: MCP、A2A、ACP

ここでの MCP はクライアント(`ng.mcp`)です。A2A と ACP は公式の Python SDK と `ProtocolHostAdapter` でホストします。停止するまで動き続けるため、スモーク検証は `bindings/python/tests/test_protocol_sdk_e2e.py` が受け持ちます。

| # | サンプル | Offline | 示すもの |
|---|---|---|---|
| 26 | [`26_mcp_tools.py`](26_mcp_tools.py) | yes | ローカルの MCP サーバーを起動して `fetch` を検出し、明示的な再入ポリシーで 3 回呼び出します。MCP を有効にしたビルドが必要で、スモークテストは Linux で行います。 |
| 27 | [`27_a2a_server.py`](27_a2a_server.py) | service | 公式の A2A SDK でエージェントをホストします。`127.0.0.1:9999` のエージェントカードと JSON-RPC、ストリーミングされるアーティファクト。`neograph-engine[a2a]` が必要です。 |
| 28 | [`28_acp_agent.py`](28_acp_agent.py) | service | 公式の ACP SDK で stdio 上にエージェントをホストします。initialize、セッション作成、prompt、トークン更新、`end_turn`。`neograph-engine[acp]` が必要です。 |

### 永続化と運用

チェックポイント、状態、キャッシュ、トレーシング、トランスポート、ツールの並行実行。

| # | サンプル | Offline | 示すもの |
|---|---|---|---|
| 07 | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | yes | チェックポイントが支払いのディスパッチ前で止まり、承認すると模擬の支払いがちょうど 1 回だけ再開します。 |
| 09 | [`09_state_management.py`](09_state_management.py) | yes | `get_state` と `fork`。alpha は count 11 に達し、beta がその値をフォークし、未知のスレッドにはチェックポイントがありません。 |
| 18 | [`18_node_cache.py`](18_node_cache.py) | key | 2 つのトピックにまたがる 5 回の実行で、プロバイダーを使うノードは 2 回だけ実行され、繰り返された入力はキャッシュされた書き込みを再生します。 |
| 20 | [`20_otel_tracing.py`](20_otel_tracing.py) | yes | 実行と 3 つのノードの OpenTelemetry span がコンソールに出力され、最終的な経路は `['A', 'B', 'C']` です。`opentelemetry-api` と `opentelemetry-sdk` が必要です。 |
| 21 | [`21_http2_transport.py`](21_http2_transport.py) | key | 同じ SDK で HTTP/1.1 と HTTP/2 を比べます。TLS の Chat エンドポイントと、HTTP/2 対応の libcurl が必要です。結果はこのエンドポイントについてのもので、順位付けではありません。 |
| 24 | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) | yes | ツールゲートが、どちらの兄弟ツールも実行される前に止まります。拒否すると `list_files` だけ、承認すると両方が 1 回ずつ実行されます。シェル操作は模擬です。 |
| 25 | [`25_async_tools.py`](25_async_tools.py) | yes | I/O 中心の作業で同期の `Tool` と重なって実行される `AsyncTool` を比べ、CPU 中心の作業では GIL の境界を示します。時間の比率は観測値で、スモークテストは Linux で行います。 |

## オフライン、スクリプト、ライブ

モデルで動くサンプルは、`_common.py` の `example_provider(script)` でプロバイダーを作ります。`--live` がなければ `_scripted.ScriptedModel` が応答します。これは `127.0.0.1` で動く小さなサーバーで、スクリプトに従って OpenAI の Chat または Responses プロトコルで応答します(テキスト、ツール呼び出し、ストリーミング、使用量、HTTP の失敗)。実際の `SchemaProvider` によるリクエスト構築、SSE の解析、outcome の処理がこのサーバーに対して動きます。モデルではないため、何を返すかはスクリプトが決めます。

```python
from _common import example_provider
from _scripted import call_tool, say

script = [call_tool("calc", expression="2 + 3"), say("5")]   # call 1 asks for a tool, call 2 answers
with example_provider(script) as chosen:
    provider = chosen.provider        # a SchemaProvider; chosen.scripted is None with --live
```

スクリプトは、`turn.last_user_text`、`turn.system_text`、`turn.last_tool_result` を読む関数 `(turn) -> Reply` にもできます。`chosen.scripted.call_count` で、モデル呼び出しが何回あったかを検証できます。規則は `_scripted.py` の docstring にあります。

`--live`、`NG_EXAMPLE_LIVE=1`、`OPENAI_API_BASE`、`NG_PROVIDER_DESCRIPTOR` のいずれかを使うと、代わりに設定済みのエンドポイントが選ばれます。ホスト型エンドポイントには `OPENAI_API_KEY` が必要で、任意で `OPENAI_MODEL`(既定は `gpt-4.1-mini`)を使います。どちらもプロセスの環境からのみ読みます。`_common.py` は `.env` ファイルを読み込まず、キーがない場合は成功した実行ではなくエラーです。`_common.py` は閉じた OpenAI の Chat または Responses ディスクリプターを承認してから `SchemaProvider` を構築します。型付き SDK は libcurl の HTTP を使い、削除済みのプロバイダークラス、completion パラメーター、WebSocket 経路、トランスポートバックエンドの選択子は使えません。`OPENAI_API_BASE` は origin またはゲートウェイのプレフィックスであり、完全な `/v1/chat/completions` 経路ではありません。システムの信頼ストアに CA がない HTTPS ピアには `NG_EXAMPLE_CA_FILE` も必要です。代わりに `NG_PROVIDER_DESCRIPTOR` で、承認済みの完全なディスクリプター JSON ファイルを指定することもできます。型付きリクエストと outcome の契約は [Python バインディングガイド](../../../docs/python-binding.md)を参照してください。

## 状態とスケジューリング

ノードは現在のスーパーステップの状態を読み、チャネルへの書き込みを返します。エンジンは、次のスーパーステップの前に、宣言されたリデューサーを通じてそれらの書き込みをマージします。`Send` は分岐ごとの入力を渡し、`Command` は状態を更新して次のノードを選びます。ワーカープールを使っても、CPython GIL の下では CPU 負荷の高い Python コールバックが並列に実行されるわけではありません。

`GraphEngine.compile()` は、Python で記述した辞書に加え、JSON から読み込んだ辞書も受け入れます。定義が保存するのは接続関係であり、実行可能な Python クラスではありません。保存した定義をコンパイルする前に、カスタムノードのファクトリーを登録してください。例14と15はスクリプトと同じ場所にある `my_graph.json` を書き込み/読み込みします。この組み合わせを検証する際は、使い捨てのチェックアウトまたはこのディレクトリのコピーを使ってください。

## 対話とプロトコルのシナリオ

16/17では `gradio` をインストールしてスクリプトを起動し、表示されたローカル URL を開いてください。`hello` を送信し、続いて `research apples` を送信します。17では、実際の Crawl4AI `/md` サービスまたは忠実なローカル接続先が `{"success": true, "markdown": "..."}` を返す必要があります。実行中のビルドが Postgres に対応し、データベースへ接続できる場合だけ Postgres を設定してください。エンジンの状態と UI の履歴は別々に確認してください。

例27/28は、ワイヤー処理に公式 Python SDK を、チェックポイントを考慮したエンジン実行に `ProtocolHostAdapter` を使います。A2A の検証には、エージェントカードを取得し、メッセージを送信し、ストリームを受信する SDK クライアントが必要です。ACP の検証には、`initialize`、`session/new`、`session/prompt` のためのクライアントからサブプロセスへの接続が必要です。stdout はプロトコルメッセージ専用です。同じコンテキスト/セッションで2つ目のプロンプトを送信し、`(turn 2)` を期待してください。キャンセルは、成功したように見える代替結果を作るのではなく、実行中のリクエストを停止する必要があります。

永続的な ACP セッションには、`NEOGRAPH_ACP_POSTGRES_URL` または `NEOGRAPH_ACP_SQLITE_PATH` のどちらか一方だけを設定してください。最初のプロンプトが完了すると、`session/load` に必要なチェックポイントが作成されます。セッションごとにアクティブなエージェントプロセスを1つにしてください。チェックポイントストアは、プロセス間の並行書き込みを直列化しません。この例はリッチコンテンツブロックをエコーしますが、画像/音声の内容を解釈せず、エディターのファイルシステム/ターミナルコールバックも提供しません。

## ディストリビューション名とインポート名

```python
import neograph_engine as ng
from neograph_engine.llm import SchemaProvider
```

ディストリビューション名は `neograph-engine` です。PyPI の `neograph` は無関係のプロジェクトです。

## サンプルを追加する

番号は再利用しません。次に空いているのは 29 です。新しいサンプルは、ファイル 1 つと表の 1 行で構成されます。

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

サンプルをオフラインで動かせない場合は、`smoke-expect` の行の代わりに `smoke-skip: <理由>` を書きます。`smoke-requires: module1, module2` は、モジュールがないときにテストをスキップします。
