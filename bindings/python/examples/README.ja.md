<!-- neograph-i18n: source=bindings/python/examples/README.md locale=ja source_sha256=f83696b352140f2c77392207424b16e3d297e7b183c67f5d1fd26347c1d7e911 -->
# Python API の例

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

バインディング面をエンドツーエンドでカバーする28個のスクリプト。

## セットアップ

```bash
pip install neograph-engine python-dotenv
cp .env.example .env  # edit OPENAI_API_KEY for examples that hit a real LLM
```

`_common.py`は、このディレクトリまたは任意の親ディレクトリから`.env`を自動ロードします。APIキーを必要とする例は、キーが存在しない場合にクラッシュせずにきれいにスキップされます。

## 索引

| # | ファイル | ネットワーク | パターン |
|---|------|---------|---------|
| 01 | [`01_minimal.py`](01_minimal.py) | オフライン | `GraphNode`のサブクラス + `engine.run()`。最小限の有用なグラフ。 |
| 02 | [`02_tool_dispatch.py`](02_tool_dispatch.py) | オフライン | `Tool`のサブクラス + 組み込みの`tool_dispatch`。手作りツールコール（実LLMなし）。 |
| 03 | [`03_send_fanout.py`](03_send_fanout.py) | オフライン | `run(input)`が`NodeResult`を`Send`リスト付きで返す + `set_worker_count(4)`。マップリデュース。 |
| 04 | [`04_async_concurrent.py`](04_async_concurrent.py) | オフライン | `engine.run_async` + 8個の並行実行による`asyncio.gather` + `run_stream_async`。 |
| 05 | [`05_openai_provider.py`](05_openai_provider.py) | **OpenAI** | `OpenAIProvider` + 組み込み `llm_call` ノード。一回限りの完了処理。 |
| 06 | [`06_react_agent.py`](06_react_agent.py) | **OpenAI** | ReAct ループ: `llm_call` ↔ `tool_dispatch` と `has_tool_calls` 条件付き。 |
| 07 | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) | オフライン | モック LLM エミッターを使用した propose/approve の2段階ワークフロー。 |
| 08 | [`08_intent_routing.py`](08_intent_routing.py) | **OpenAI** | 分類器ノード + 条件エッジ → 数学 / 翻訳 / 一般専門家 に振り分け。 |
| 09 | [`09_state_management.py`](09_state_management.py) | オフライン | `set_checkpoint_store(InMemoryCheckpointStore())` + `get_state` + `fork`. |
| 10 | [`10_command_routing.py`](10_command_routing.py) | オフライン | `run(input)` が `Command(goto_node=…, updates=[…])` を返す。 |
| 11 | [`11_reflexion.py`](11_reflexion.py) | **OpenAI** | Actor + critic ループ（反射プロンプトを使用したループ）（Shinn et al. 2023）。 |
| 12 | [`12_self_ask.py`](12_self_ask.py) | **OpenAI** | Self-Ask 追随質問分解 (Press et al. 2022)。 |
| 13 | [`13_multi_agent_debate.py`](13_multi_agent_debate.py) | **OpenAI** | 二人のディベーター＋ジャッジ。ディベーターは`Send`を介してfan-outする。 |
| 14 | [`14_graph_to_json.py`](14_graph_to_json.py) | オフライン | グラフ定義を`.json`ファイルにシリアライズします。 |
| 15 | [`15_graph_from_json.py`](15_graph_from_json.py) | オフライン | `.json` グラフをロードして実行します（14の関連項目）。 |
| 16 | [`16_deep_research_chat.py`](16_deep_research_chat.py) | **Responses（WS/HTTP）** | `조사해줘 / research / investigate`で3-way並列ディープリサーチサブグラフへ切り替わるマルチターンGradioチャットです。公式OpenAIは既定でWebSocketを使用し、互換ゲートウェイはビルド済みのHTTP/2またはHTTP/1.1を自動選択します。`NG_RESPONSES_TRANSPORT`で上書きできます。`pip install gradio`が必要です。 |
| 17 | [`17_deep_research_crawl4ai.py`](17_deep_research_crawl4ai.py) | **Responses + Crawl4AI + Postgres** | 16と同じトランスポート対応チャットですが、研究者はローカルのCrawl4AIコンテナ（`docker run unclecode/crawl4ai`）を介して実際にウェブを検索し、状態はPostgres（`PostgresCheckpointStore`）で永続化されます。両方とも環境変数で任意に設定でき、未設定時は正常にフォールバックします。Postgresパスには `-DNEOGRAPH_BUILD_POSTGRES=ON` のソースビルドが必要です。 |
| 18 | [`18_node_cache.py`](18_node_cache.py) | **OpenAI** | `engine.set_node_cache_enabled("ask", True, CacheScope.Reusable)` — 以降、同じ入力の実行ではキャッシュ済みの `NodeResult` が0msで再生され、LLM呼び出しは行われません。統計は `engine.node_cache_stats()` で確認できます。 |
| 19 | [`19_streaming_messages.py`](19_streaming_messages.py) | オフライン | `from neograph_engine import message_stream` — コールバックをラップして、`LLM_TOKEN`イベントがLangChain形式のメッセージ辞書（`{role, content, content_so_far, node, metadata}`）として届くようにします。 |
| 20 | [`20_otel_tracing.py`](20_otel_tracing.py) | オフライン | `from neograph_engine.tracing import otel_tracer` — エンジンのイベントをOpenTelemetryスパンにブリッジします。ConsoleSpanExporterが同梱されています。OTLPに交換してJaeger / Tempo / Honeycomb / Datadogに送信できます。 |
| 21 | [`21_http2_transport.py`](21_http2_transport.py) | **OpenAI** | `SchemaProvider(..., prefer_libcurl=True)` — オプトインのHTTP/2（libcurl）と既定のConnPool（HTTP/1.1 keep-alive）を比較します。libcurlが組み込まれている場合は5-way並列バーストでA/Bテストし、バックエンドのないwheelではHTTP/1.1のsmoke呼び出しを1回実行してHTTP/2のソースビルド方法を案内します。 |
| 22 | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | **OpenAI** | 目標指向の自己進化: エージェントが実行され、出力をJSON-shape目標に対して採点し、LLMに改definitions. Loop closes when score ≥ 1.0 or max_iters hit. JSON-as-program validates、 where the selectorは新しいgraph spec. |
| 23 | [`23_evolving_chat_agent.py`](23_evolving_chat_agent.py) | **OpenAI** | スレッド単位で進化するチャットエージェント: 永続的なマルチターン会話。ターン間では、エージェントのJSON定義が蓄積された履歴に基づいて書き換えられます。進化をまたぐチェックポイント再開（以前のメッセージは存続）、`__graph_meta__`監査チャネルパターン、およびバリデータ境界（ノードタイプのホワイトリスト、必須チャネル、エッジ接続）を示します。`OPENAI_API_KEY`を要求します。なければクリーンに終了します。 |
| 24 | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) | オフライン | ツールゲート（#89）: すべてのツールコールに対して`engine.set_tool_gate(...)`が、**どのツールの実行前にも**参照され、Allow / Allow-with-rewritten-args / Deny / Interrupt を返します。標準の承認プロンプトを表示します。 *「エージェントが`rm -rf build/`を実行しようとしています。許可しますか？」* — そして重要なのは、人間が決定している間に無害な兄弟呼び出しが**実行されていない**ことです。したがって、拒否は実際には何も起こらなかったことを意味し、承認しても再実行はされません。 |
| 25 | [`25_async_tools.py`](25_async_tools.py) | オフライン | Concurrent tools (#96): `ng.AsyncTool` の代わりに `ng.Tool`、3つの300 msツールは0.90秒ではなく0.30秒かかります。また、同じ実行で境界を測定します — 3つの*CPU-bound*ツールは1つのツールの3.2倍の時間がかかります。これは、Python関数が実行中にGILを保持し、スレッドの数に関係なくそれが変わらないためです。並行性はオプトインであるため、既存のステートフルツールが突然自分自身と競合することはありません。 |
| 26 | [`26_mcp_tools.py`](26_mcp_tools.py) | オフライン | MCP (#95): `ng.mcp.MCPClient(url).get_tools()` リモートツールカタログを取得し、それをそのまま `NodeContext`に渡す。独自のMCPサーバーを起動するため、ネットワークなしで動作する。名前付き呼び出しの繰り返しはデフォルトで直列のままである。スレッド化されたデモは明示的に `fetch` を `ToolExecutionPolicyRegistry`を通じてReentrantとしてマークし、その後、3回の0.4秒のHTTP呼び出しを0.41秒で測定する。stdioもJSON-RPC IDを多重化するが、オーバーラップにはそのホストポリシーと並行サーバーの両方が必要である。 |
| 27 | [`27_a2a_server.py`](27_a2a_server.py) | localhost | A2Aホスティング（#120）：公式の `a2a-sdk` がJSON-RPC、タスク状態、エージェントカード、およびキャンセレーションを所有します。 `ProtocolHostAdapter.stream()` がチェックポイントコンテキストを保持しながら、エンジンのトークンイベントをチャンク化されたA2Aアーティファクトにマッピングします。Python 3.10以上と `pip install "neograph-engine[a2a]"`. |
| 28 | [`28_acp_agent.py`](28_acp_agent.py) | stdio | ACPホスティング（#120）：トークン更新をストリーミングし、グラフ用のテキスト/画像/オーディオ/リソースコンテンツブロックを保持し、永続的な `session/load` をサポートします（`NEOGRAPH_ACP_POSTGRES_URL` または `NEOGRAPH_ACP_SQLITE_PATH` が設定されている場合）。Python 3.10以上と `pip install "neograph-engine[acp]"` が必要です。 |

## ホスティングが公式SDKを使用する理由

C++ライブラリには独自の`A2AServer`と`ACPServer`があるが、それらのクラスを直接公開すると、Pythonユーザーには公式SDKよりも統合が弱い第二のプロトコル実装が提供されることになる。特に、公式SDKはすでに現在のワイヤーフォーマット互換性、サーバートランスポート、タスクまたはセッションのライフサイクル、asyncioキャンセルを所有している。NeoGraphが提供するのは、それらのSDKが提供できない部分、つまりチェックポイント対応のC++グラフエンジンへの呼び出しのみである。

| 項目 | 決定 |
|------|----------|
| 欠落していると思われるC++機能 | `A2AServer`、`ACPServer`、およびそれらのライフサイクルメソッドはPythonクラスとしてミラーリングされていない。 |
| Pythonの代替案 | 公式の`a2a-sdk` 1.xおよび`agent-client-protocol` 0.11.xサーバーランタイム。 |
| NeoGraph統合 | `ProtocolHostAdapter` はプロトコル会話IDを`RunConfig.thread_id`にマッピングし、`resume_if_exists`を有効にし、`LLM_TOKEN`イベントをストリーミングし、カスタムのJSON安全な入力ペイロードを受け入れ、アクティブなasyncioタスクをキャンセルする。 |
| 依存関係ポリシー | 両SDKはPython 3.10+が必要なためオプションであり、`neograph-engine`はPython 3.9をサポートしている。`neograph-engine[a2a]`、`neograph-engine[acp]`、または`neograph-engine[protocols]`をインストールする。 |
| 永続的なACPセッション | ホイール対応の永続的バックエンドには`NEOGRAPH_ACP_POSTGRES_URL`を設定する。`NEOGRAPH_BUILD_SQLITE=ON`を使ったソースビルドは`NEOGRAPH_ACP_SQLITE_PATH`を設定することができる。エージェントは、構成済みの場合にのみ`session/load`をアドバタイズする。最初の完了したプロンプトがチェックポイントを生成した後、新しいセッションはロード可能になる。セッションIDはサーバー生成の機能であり、チェックポイントはプライベートな`acp:`スレッド名前空間を使用する。セッションごとにアクティブなエージェントプロセスを1つ維持すること。チェックポイントストアは、複数のプロセスにまたがる同時書き込みをシリアライズしない。 |
| 現在の制限 | ACPエディタコールバック（`fs/read_text_file`、ターミナル呼び出し、権限プロンプト）は、共有NeoGraph Pythonツールからまだ安全に呼び出せません。現在の`AsyncTool`はワーカースレッドで同期関数を実行し、現在のプロトコルセッションIDを保持しません。偽のブリッジは、誤ったエディタセッションを呼び出すリスクがあります。 |
| 直接バインディングを再検討する際 | ユーザーは正確なC++サーバーをPythonに埋め込む必要があります。そうでなければ、公式SDKパスは必要なNeoGraphのキャンセル、チェックポイント、トレーシング、またはツール呼び出しの動作を保持できません。 |

`ProtocolHostAdapter.run_payload()`は、設定された`input_builder`を通じて、JSONセーフな任意の値を渡します。デフォルトの`message_input`は、リッチコンテンツブロックをユーザーメッセージの`content`として保持します。別の形状を期待するプロバイダーを持つグラフは、カスタムビルダーを渡すべきです。`ProtocolHostAdapter.stream()`は、`ProtocolStreamEvent(kind="token", ...)`の値と、それに続く正確に1つの最終イベントを生成します。ライブトークンは、`stream_node`が最終回答を正確に構成するトークンのグラフノードを指定する場合を除き、無効です。これにより、プランナー/ツールノードの出力がプロトコル応答を介して漏れるのを防ぎます。asyncioコンシューマーキューは制限されています（デフォルトで1,024チャンク）。オーバーフロー時にはエンジン実行がキャンセルされます。ネイティブストリームイベントは最初にasyncioループにスケジュールされるため、このキューは遅いプロトコル転送に対する背圧であり、無制限のネイティブプロデューサーに対するプロセス全体のハードメモリキャップではありません。

次のいずれかで実行します。

```bash
python 01_minimal.py
```

## メンタルモデル

PythonからのNeoGraphは、PythonからのLangGraphのように見えます：ノードのグラフ、リデューサー付きチャネル、`Send`による動的fan-out、`Command`によるルーティングオーバーライド、名前付き条件（`route_channel`、`has_tool_calls`など）による条件付きエッジ。同じプリミティブ、同じJSON形状のグラフ定義です。違いは、それを実行しているもの——スーパーステップループ、スケジューリング、チェックポイントを、LangGraphの約600µsではなく、ステップごとにマイクロ秒単位で行うC++エンジンです。

例には3つのパターンが見られます：

1. **Pythonカスタムノード**（01、03、04、07、09、10、11、12、13）は`neograph_engine.GraphNode`をサブクラス化し、`run(input)`を実装します。`input.state`からチャネルを読み取り、存在する場合`input.stream_cb`を使用し、書き込み、`Command`、`Send`、または`NodeResult`を返します。エンジンはGIL処理の下でPythonにディスパッチするため、並行するカスタムノードがデッドロックすることはありません。

2. **Pythonツール** (02, 06, 07) は`neograph_engine.Tool`をサブクラス化し、インスタンスを`NodeContext(tools=[…])`に渡します。エンジンはコンパイル時に所有権を取得します。Pythonの参照はその後ドロップできます。

3. **Async** (04) — すべての`*_async`バインディングは、呼び出しスレッドの実行ループにバインドされた`asyncio.Future`を返します。ストリームコールバックは`loop.call_soon_threadsafe`を介してループスレッドにホップされるため、`cb(ev)`はasyncioが期待する場所で実行されます。

## グラフ定義はJSONです。

`GraphEngine.compile(definition, ctx)` は、コードで構築するPythonの `dict` または、ファイルから `dict` する `json.loads()` のいずれかを受け入れます — 同じ形状です。例14と15は往復を示しています。カスタムノードの*型*は、依然としてコード内で登録する必要があります（PythonクラスはJSONにエンコードできません）が、配線 — チャンネル、型別のノード、エッジ、条件付きエッジ — はデータです。

## ディストリビューション名とインポート名

PyPIパッケージは **`neograph-engine`** です（裸の `neograph` という名前は、PyPI上で既に無関係のプロジェクトによって取得されていました）。Pythonのインポート名は `neograph_engine` です：

```python
import neograph_engine as ng
from neograph_engine.llm import OpenAIProvider, SchemaProvider
```
