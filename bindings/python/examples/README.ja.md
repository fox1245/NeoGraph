<!-- neograph-i18n: source=bindings/python/examples/README.md locale=ja source_sha256=5ec25b467bef853ac9df52be725f8c16b6c63aef9ab5f6f5b64c974749579bee -->
# Python APIのサンプル

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

この28本のスクリプトで、グラフのステート、ルーティング、ツール、プロバイダーへのリクエスト、プロトコルのホスティングを確認できます。まずはオフラインで動くサンプルから始めてください。ホスト型のモデルを呼び出すサンプルには、認証情報の明示的な設定が必要で、プロバイダーの料金がかかります。

## セットアップ

```bash
pip install neograph-engine
python 01_minimal.py
```

コマンドは、このディレクトリで実行します。`python-dotenv` は必須ではありません。`_common.py` に、最も近いサンプルまたはリポジトリの `.env` を読み込ませたい場合にインストールしてください。エクスポート済みの環境変数が優先されます。ホスト型の認証情報が不足している場合はエラーになります。検証が成功したことにはなりません。

ソースからビルドしたものを使う場合は、`-DNEOGRAPH_BUILD_PYBIND=ON` を指定して構成し、`PYTHONPATH` にビルドディレクトリーを設定します。ビルドツリーでは、パッケージのPython依存関係はインストールされません。先に `pip install 'pydantic>=2' certifi` を実行してください。`import neograph_engine` にはpydanticが必要です。certifiがない場合、TLSにはホストの信頼ストアが使われます。

ホスト型のサンプルでは、`OPENAI_API_KEY` を設定します。`OPENAI_MODEL` は任意です（デフォルトは `gpt-4.1-mini`）。`_common.py` は、クローズドなOpenAIのChatまたはResponsesのディスクリプターをadmissionに通してから、`SchemaProvider` を作ります。型付きSDKはlibcurlのHTTPを使います。削除済みのプロバイダークラス、completionパラメーター、WebSocketの経路、トランスポートバックエンドのセレクターは、使えません。

キーなしでプロトコルの適格性確認を行う場合は、`OPENAI_API_BASE` に、実物に忠実なループバックのChat／Responses peerを指定します。このpeerは、サンプルが使うルートとレスポンスの形を実装している必要があります。Pythonのモックを返すものではいけません。ローカルのTLS CAには、`NG_EXAMPLE_CA_FILE` を使います。別の方法として、`NG_PROVIDER_DESCRIPTOR` に、admission済みの完全なディスクリプターのJSONファイルを指定することもできます。ゲートウェイのプレフィックス、ルート、許可するヘッダーも、そこに含めます。ベースURLは、`/v1/chat/completions` までを含む完全なルートではありません。型付きのリクエストと結果の契約については、[Pythonバインディングガイド](../../../docs/python-binding.md)を参照してください。

## 一覧と期待される動作

各サンプルは、`python <file>` で実行します。Linux x86_64でのスモークテストでは、認証情報が不要なlocalhostのpeerに対して、2つのリサーチアプリを含む、型付きプロバイダーを使ういくつかのアプリケーションを動かしました。すべてのスクリプト、ホスト型のベンダー、リリース対象のプラットフォームの適格性確認が済んだわけではありません。リモートのCIと公開も、まだ済んでいません。対象の範囲は、[現在のリリースのエビデンス](../../../CHANGELOG.md#unreleased)を参照してください。表に示すのは期待される動作で、モデルの出力する文言が決まっているわけではありません。

| # | ファイル | 前提条件 | 期待される動作 |
|---|------|---------------|-------------------|
| 01 | [`01_minimal.py`](01_minimal.py) | なし | カスタムノードが21を2倍にして42にする。 |
| 02 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | なし | スクリプト化されたツール呼び出しが `tool_dispatch` を通り、calculatorが `42` を返す。 |
| 03 | [`03_send_fanout.py`](03_send_fanout.py) | なし | 8つの `Send` の分岐が、平方数 `[0, 1, 4, 9, 16, 25, 36, 49]` をマージする。追加される順序は前提にしない。 |
| 04 | [`04_async_concurrent.py`](04_async_concurrent.py) | なし | 非同期の実行8回が `[0, 2, 4, 6, 8, 10, 12, 14]` を出力し、ストリーミング実行ではノードのイベントが出る。 |
| 05 | [`05_openai_provider.py`](05_openai_provider.py) | Chatのpeerまたはホスト型のキー | `llm_call` が、`hello world` というアシスタントのメッセージを書き込む。 |
| 06 | [`06_react_agent.py`](06_react_agent.py) | Responsesのpeerまたはホスト型のキー | モデルが発行した `calc` の呼び出しが `4053` を返し、そのあとモデルが最終的な回答を出す。 |
| 07 | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | なし | 支払いのディスパッチの前でチェックポイントが一時停止し、承認すると、模擬の支払いがちょうど1回だけ再開される。実際の課金はない。 |
| 08 | [`08_intent_routing.py`](08_intent_routing.py) | Chatのpeerまたはホスト型のキー | 3つの質問が、数学、翻訳、一般の各エキスパートにルーティングされ、それぞれが回答を書き込む。 |
| 09 | [`09_state_management.py`](09_state_management.py) | なし | alphaがカウント11に達し、betaがその値をフォークする。未知のスレッドにはチェックポイントがない。 |
| 10 | [`10_command_routing.py`](10_command_routing.py) | なし | 入力200、50、-10が、`Command` によって、それぞれaccept、manual、rejectのノードに振り分けられる。 |
| 11 | [`11_reflexion.py`](11_reflexion.py) | Chatのpeerまたはホスト型のキー | ActorとCriticの呼び出しで、振り返りを次の試行に引き継ぎ、`ok` かスーパーステップの上限で止まる。上限付きの学習用の簡易版。 |
| 12 | [`12_self_ask.py`](12_self_ask.py) | Chatのpeerまたはホスト型のキー | 途中の質問と回答がスクラッチパッドにたまり、最後に最終回答を出す。検索サービスは接続していない。 |
| 13 | [`13_multi_agent_debate.py`](13_multi_agent_debate.py) | Chatのpeerまたはホスト型のキー | 2つの `Send` の分岐が反対の議論を作り、1人の審判がマージされた議論を読む。 |
| 14 | [`14_graph_to_json.py`](14_graph_to_json.py) | なし | 2倍にした結果の42と、保存された `my_graph.json` の定義。 |
| 15 | [`15_graph_from_json.py`](15_graph_from_json.py) | 先に14を実行 | 保存した定義が、5を10に、100を200にする。カスタムのノードタイプは別に登録する。 |
| 16 | [`16_deep_research_chat.py`](16_deep_research_chat.py) | Responsesのpeerまたはキー、`gradio` | 通常のチャットか、3つの質問によるリサーチレポート。リサーチャーは、Web検索ではなく、モデルの知識を使う。 |
| 17 | [`17_deep_research_crawl4ai.py`](17_deep_research_crawl4ai.py) | Responsesのpeerまたはキー、`gradio`、`requests`、任意でCrawl4AIとPostgres | `CRAWL4AI_URL` を設定すると、実際の `/md` 検索が有効になり、`NEOGRAPH_PG_DSN` を設定すると、エンジンのチェックポイントが永続化される。設定したサービスが失敗した場合に、黙って代替されることはない。どちらも設定しない場合、スクリプトは、モデルだけのリサーチとインメモリのステートであることを明示する。Gradioの履歴は、自動では復元されない。 |
| 18 | [`18_node_cache.py`](18_node_cache.py) | Chatのpeerまたはホスト型のキー | 2つのトピックにまたがる5回の実行で、プロバイダーを使うノードが2回実行され、同じ入力の実行ではキャッシュした書き込みがリプレイされる。レイテンシーの固定値は保証しない。 |
| 19 | [`19_streaming_messages.py`](19_streaming_messages.py) | なし | スクリプト化した5つのトークンイベントが、`Octopuses have three hearts.` と、メッセージストリームのチャンクになる。 |
| 20 | [`20_otel_tracing.py`](20_otel_tracing.py) | `opentelemetry-api`、`opentelemetry-sdk` | コンソールに出力されるスパンが、実行全体と3つのノードを表し、最終的な履歴は `['A', 'B', 'C']` になる。 |
| 21 | [`21_http2_transport.py`](21_http2_transport.py) | TLSのChatのpeerまたはキー、libcurlのHTTP/2サポート | 同じSDKで、HTTP/1.1とHTTP/2のリクエストを比べる。ネゴシエーションされたプロトコルは、別に確認すること。所要時間はエンドポイントによって変わる。 |
| 22 | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | Chatのpeerまたはホスト型のキー | プロファイルのJSONを採点し、失敗したら修正版のグラフを要求し、再コンパイルして再試行する。受け付けるのは、登録済みのノードタイプだけ。モデルによっては、成功しないまま反復の上限に達することがある。 |
| 23 | [`23_evolving_chat_agent.py`](23_evolving_chat_agent.py) | Chatのpeerまたはホスト型のキー | admission済みのグラフの書き換えをまたいで、会話のチェックポイントを保持し、バージョンとハッシュを `__graph_meta__` に記録する。このメタデータはアプリケーションレベルのもので、リプレイの正式なエビデンスではない。 |
| 24 | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) | なし | 並列の2つのツールのどちらも実行する前に一時停止する。拒否すると `list_files` だけが実行され、承認すると両方が1回ずつ実行される。シェル操作は模擬。 |
| 25 | [`25_async_tools.py`](25_async_tools.py) | なし | 直列の `Tool` と、I/O待ちが重なる `AsyncTool` を比べ、そのあとCPythonのGILの境界を示す。所要時間の比は観測値であり、合否の基準ではない。 |
| 26 | [`26_mcp_tools.py`](26_mcp_tools.py) | MCPを有効にしたビルド | 実際のローカルのJSON-RPCのMCP peerを起動し、`fetch` を見つけて、再入可能であることを明示したポリシーで3回呼び出す。外部のネットワークもキーも不要。 |
| 27 | [`27_a2a_server.py`](27_a2a_server.py) | Python 3.10以上、`neograph-engine[a2a]` | `127.0.0.1:9999` で、エージェントカードとA2AのJSON-RPCを提供する。ストリーミングされるアーティファクトが `NeoGraph received: hello (turn 1)` になる。 |
| 28 | [`28_acp_agent.py`](28_acp_agent.py) | Python 3.10以上、`neograph-engine[acp]` | stdioでACPを提供する。初期化し、セッションを作成してプロンプトを送ると、トークンの更新と `end_turn` を受け取る。永続的な `session/load` には、設定済みのPostgresかSQLiteのバックエンドが必要。 |

## ステートとスケジューリング

ノードは、現在のスーパーステップのステートを読み、チャネルへの書き込みを返します。エンジンは、次のスーパーステップに進む前に、宣言されたリデューサーでその書き込みをマージします。`Send` は分岐ごとの入力を渡し、`Command` はステートを更新して次のノードを選びます。ワーカープールを使っても、CPUを使うPythonのコールバックが、CPythonのGILのもとで並列に動くようにはなりません。

`GraphEngine.compile()` は、Pythonで書いた辞書だけでなく、JSONから読み込んだ辞書も受け付けます。定義に保存されるのは配線で、実行可能なPythonのクラスではありません。保存した定義をコンパイルする前に、カスタムのノードファクトリーを登録してください。サンプルの14と15は、スクリプトと同じ場所の `my_graph.json` に書き込み、そこから読み込みます。この2本の適格性確認を行うときは、使い捨てのチェックアウトか、このディレクトリのコピーを使ってください。

## 対話型とプロトコルのシナリオ

16と17では、`gradio` をインストールし、スクリプトを起動して、表示されたローカルのURLを開きます。`hello` を送信し、続いて `research apples` を送信してください。17では、実際のCrawl4AIの `/md` サービスか、実物に忠実なローカルのpeerが、`{"success": true, "markdown": "..."}` を返す必要があります。Postgresを設定するのは、実行中のビルドがそれをサポートしていて、データベースに接続できる場合だけにしてください。エンジンのステートとUIの履歴は、別々に確認してください。

27と28は、ワイヤーの処理に公式のPython SDKを使い、チェックポイントを考慮したエンジンの実行に `ProtocolHostAdapter` を使います。A2Aを検証するには、SDKのクライアントで、エージェントカードの取得、メッセージの送信、ストリームの受信を行います。ACPを検証するには、クライアントのサブプロセスから接続して、`initialize`、`session/new`、`session/prompt` を呼び出します。標準出力は、プロトコルのメッセージ専用です。同じコンテキストまたはセッションで2回目のプロンプトを送ると、`(turn 2)` が返るはずです。キャンセルすると、実行中のリクエストが止まるはずです。成功したように見えるフォールバックを作ってはいけません。

ACPのセッションを永続化するには、`NEOGRAPH_ACP_POSTGRES_URL` と `NEOGRAPH_ACP_SQLITE_PATH` のどちらか一方だけを設定します。`session/load` に必要なチェックポイントは、最初のプロンプトが完了した時点で作られます。1つのセッションにつき、アクティブなエージェントのプロセスは1つだけにしてください。チェックポイントストアは、複数のプロセスからの同時書き込みを直列化しません。このサンプルは、リッチなコンテンツブロックをそのまま返しますが、画像や音声のコンテンツは解釈せず、エディターのファイルシステムやターミナルのコールバックも提供しません。

## ディストリビューション名とインポート名

```python
import neograph_engine as ng
from neograph_engine.llm import SchemaProvider
```

ディストリビューション名は `neograph-engine` です。PyPIにある `neograph` は、無関係なプロジェクトです。
