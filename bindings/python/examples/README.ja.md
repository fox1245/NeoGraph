<!-- neograph-i18n: source=bindings/python/examples/README.md locale=ja source_sha256=a1ffbe746f41909d860beac33ef1f3ea473e10ad6162985e4fba7b3810b4dd22 -->
# Python API の例

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

この28個のスクリプトは、グラフの状態、ルーティング、ツール、プロバイダーへのリクエスト、プロトコルのホスティングを示します。まずオフラインの例から始めてください。ホスト型モデルへの呼び出しには認証情報の明示的な設定が必要で、プロバイダーの利用料金が発生します。

## セットアップ

```bash
pip install neograph-engine
python 01_minimal.py
```

コマンドはこのディレクトリで実行してください。`python-dotenv` は任意です。`_common.py` で例またはリポジトリ内の最も近い `.env` を読み込む場合にインストールしてください。エクスポート済みの環境変数が優先されます。ホスト型サービスの認証情報が欠けている場合はエラーであり、検証の成功にはなりません。

ホスト型サービスを使う例では、`OPENAI_API_KEY` を設定し、必要に応じて `OPENAI_MODEL`（既定値は `gpt-4.1-mini`）も設定してください。`_common.py` は閉じた形式の OpenAI Chat または Responses 記述子を受け入れ、`SchemaProvider` を構築します。型付き SDK は libcurl HTTP を使います。削除されたプロバイダークラス、完了パラメーター、WebSocket 経路、トランスポートバックエンドの選択機能は利用できません。

キーを使わずにプロトコルを検証するには、`OPENAI_API_BASE` を Chat/Responses プロトコルを忠実に実装したループバックの接続先に設定してください。その接続先は、Python のモックを返すのではなく、例が使うルートとレスポンス形式を実装する必要があります。ローカル TLS CA には `NG_EXAMPLE_CA_FILE` を使ってください。代わりに `NG_PROVIDER_DESCRIPTOR` で、ゲートウェイのプレフィックス、ルート、許可するヘッダーを含む、完全な受け入れ済み記述子の JSON ファイルを指定できます。ベース URL は完全な `/v1/chat/completions` ルートではありません。型付きリクエストと outcome の契約については、[Python バインディングガイド](../../../docs/python-binding.md)を参照してください。

## 索引と期待される動作

各項目は `python <file>` で実行してください。Linux x86_64 のスモーク実行では、両方の調査アプリを含む一部の型付き provider アプリケーションを、認証情報不要の localhost ピアで実行しました。すべてのスクリプト、ホスト型サービス、リリース対象プラットフォームの検証ではなく、リモート CI と公開はまだ完了していません。範囲は[現在のリリースの実行記録](../../../CHANGELOG.md#unreleased)を参照してください。表は期待される動作を示し、モデルの文言は決定的ではありません。

| # | ファイル | 前提条件 | 期待される動作 |
|---|------|---------------|-------------------|
| 01 | [`01_minimal.py`](01_minimal.py) | なし | カスタムノードが21を2倍して42にします。 |
| 02 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | なし | スクリプトで指定したツール呼び出しが `tool_dispatch` を通り、計算機が `42` を返します。 |
| 03 | [`03_send_fanout.py`](03_send_fanout.py) | なし | 8本の `Send` 分岐が平方数 `[0, 1, 4, 9, 16, 25, 36, 49]` をマージします。追加順序は仮定しません。 |
| 04 | [`04_async_concurrent.py`](04_async_concurrent.py) | なし | 8回の非同期実行が `[0, 2, 4, 6, 8, 10, 12, 14]` を生成し、ストリーミング実行がノードイベントを出力します。 |
| 05 | [`05_openai_provider.py`](05_openai_provider.py) | Chat 接続先またはホスト型サービスのキー | `llm_call` が `hello world` と述べるアシスタントメッセージを書き込みます。 |
| 06 | [`06_react_agent.py`](06_react_agent.py) | Responses 接続先またはホスト型サービスのキー | モデルが発行する `calc` 呼び出しが `4053` を返し、その後モデルが最終回答を示します。 |
| 07 | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | なし | チェックポイントが支払いのディスパッチ前に一時停止し、承認すると模擬支払いをちょうど1回再開します。金銭は請求されません。 |
| 08 | [`08_intent_routing.py`](08_intent_routing.py) | Chat 接続先またはホスト型サービスのキー | 3つの質問を数学、翻訳、一般の専門家へ振り分け、各専門家が回答を書き込みます。 |
| 09 | [`09_state_management.py`](09_state_management.py) | なし | Alpha のカウントが11に達し、beta がその値から分岐します。未知のスレッドにはチェックポイントがありません。 |
| 10 | [`10_command_routing.py`](10_command_routing.py) | なし | 入力200、50、-10が `Command` を通じて accept、manual、reject ノードを選びます。 |
| 11 | [`11_reflexion.py`](11_reflexion.py) | Chat 接続先またはホスト型サービスのキー | Actor/critic の呼び出しが振り返りを次の試行へ引き継ぎ、`ok` またはスーパーステップの上限で停止します。これは回数を制限した学習用の実装です。 |
| 12 | [`12_self_ask.py`](12_self_ask.py) | Chat 接続先またはホスト型サービスのキー | 最終回答の前に、中間の質問と回答をスクラッチパッドへ蓄積します。検索サービスは接続していません。 |
| 13 | [`13_multi_agent_debate.py`](13_multi_agent_debate.py) | Chat 接続先またはホスト型サービスのキー | 2本の `Send` 分岐が対立する議論を生成し、1人の判定役がマージされた議論を読みます。 |
| 14 | [`14_graph_to_json.py`](14_graph_to_json.py) | なし | 2倍した結果42と、保存された `my_graph.json` 定義を得ます。 |
| 15 | [`15_graph_from_json.py`](15_graph_from_json.py) | 先に14を実行 | 保存した定義が5を10に、100を200にします。カスタムノード型は別途登録します。 |
| 16 | [`16_deep_research_chat.py`](16_deep_research_chat.py) | Responses 接続先/キー、`gradio` | 通常のチャット、または3つの質問に基づく調査レポートを生成します。調査役はウェブ検索ではなくモデルの知識を使います。 |
| 17 | [`17_deep_research_crawl4ai.py`](17_deep_research_crawl4ai.py) | Responses 接続先/キー、`gradio`、`requests`、任意で Crawl4AI/Postgres | `CRAWL4AI_URL` で実際の `/md` 検索を、`NEOGRAPH_PG_DSN` で永続的なエンジンチェックポイントを有効にします。設定済みサービスの失敗を黙って別の処理に置き換えることはありません。両方とも未設定の場合は、モデルのみの調査とインメモリ状態であることを明示します。Gradio の履歴は自動復元されません。 |
| 18 | [`18_node_cache.py`](18_node_cache.py) | Chat 接続先またはホスト型サービスのキー | 2つのトピックに対する5回の実行で、プロバイダーを使うノードは2回実行されます。同じ入力ではキャッシュ済みの書き込みを再生します。固定の遅延時間は保証しません。 |
| 19 | [`19_streaming_messages.py`](19_streaming_messages.py) | なし | スクリプトで指定した5つのトークンイベントが `Octopuses have three hearts.` とメッセージストリームのチャンクを構成します。 |
| 20 | [`20_otel_tracing.py`](20_otel_tracing.py) | `opentelemetry-api`、`opentelemetry-sdk` | コンソールのスパンが実行全体と3つのノードを記録し、最終的な経路は `['A', 'B', 'C']` になります。 |
| 21 | [`21_http2_transport.py`](21_http2_transport.py) | TLS Chat 接続先/キー、libcurl の HTTP/2 対応 | 同じ SDK を通じて HTTP/1.1 と HTTP/2 のリクエストを比較します。ネゴシエートされたプロトコルは別途確認してください。所要時間はエンドポイントに依存します。 |
| 22 | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | Chat 接続先またはホスト型サービスのキー | プロファイル JSON を採点し、失敗後に修正したグラフを要求し、再コンパイルして再試行します。登録済みノード型だけを受け入れます。モデルが成功しないまま反復回数の上限に達する場合があります。 |
| 23 | [`23_evolving_chat_agent.py`](23_evolving_chat_agent.py) | Chat 接続先またはホスト型サービスのキー | 受け入れたグラフの書き換えをまたいで会話のチェックポイントを保持し、バージョンとハッシュを `__graph_meta__` に記録します。このメタデータはアプリケーションレベルのもので、権威あるリプレイの証拠ではありません。 |
| 24 | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) | なし | 兄弟関係にあるいずれかのツールが実行される前に停止します。拒否では `list_files` だけを、承認では両方を1回ずつ実行します。シェル操作は模擬処理です。 |
| 25 | [`25_async_tools.py`](25_async_tools.py) | なし | 直列の `Tool` と I/O 待ちを重ねて実行する `AsyncTool` を比較し、CPython GIL の境界を示します。時間比は観測値であり、合格基準ではありません。 |
| 26 | [`26_mcp_tools.py`](26_mcp_tools.py) | MCP を有効にしたビルド | 実際のローカル JSON-RPC MCP 接続先を起動し、`fetch` を検出し、明示的な再入可能ポリシーで3回の呼び出しを実行します。外部ネットワークもキーも使いません。 |
| 27 | [`27_a2a_server.py`](27_a2a_server.py) | Python 3.10+、`neograph-engine[a2a]` | `127.0.0.1:9999` でエージェントカードと A2A JSON-RPC を提供します。ストリーミングされるアーティファクトは `NeoGraph received: hello (turn 1)` を構成します。 |
| 28 | [`28_acp_agent.py`](28_acp_agent.py) | Python 3.10+、`neograph-engine[acp]` | stdio で ACP を提供します。初期化、セッション作成、プロンプト送信を行い、トークン更新と `end_turn` を受け取ります。永続的な `session/load` には設定済みの Postgres または SQLite バックエンドが必要です。 |

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
