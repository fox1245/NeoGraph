<!-- neograph-i18n: source=bindings/python/examples/README.md locale=ja source_sha256=1c9ad12b9098111ceefe6e550a72390df5e35292924c7ad4bd899bac79e9519f -->
# Python API の例

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

バインディング サーフェスをエンドツーエンドでカバーする 28 個のスクリプト。

## 設定

```bash
pip install neograph-engine python-dotenv
cp .env.example .env  # edit OPENAI_API_KEY for examples that hit a real LLM
```

`_common.py` は、このディレクトリまたは任意の親から `.env` を自動ロードします。
API キーが必要な例は、次の場合に (クラッシュせずに) きれいにスキップされます。
鍵がありません。

## 索引

| # |ファイル |ネットワーク |パターン |
|---|------|---------|---------|
| 01 | [`01_minimal.py`](01_minimal.py) |オフライン | `GraphNode` サブクラス + `engine.run()`。有用な最小のグラフ。 |
| 02 | [`02_tool_dispatch.py`](02_tool_dispatch.py) |オフライン | `Tool` サブクラス + 組み込み `tool_dispatch`。手作りのtool_call (実際のLLMはありません)。 |
| 03 | [`03_send_fanout.py`](03_send_fanout.py) |オフライン | `run(input)` は、`Send` リスト + `set_worker_count(4)` を含む `NodeResult` を返します。マップリデュース。 |
| 04 | [`04_async_concurrent.py`](04_async_concurrent.py) |オフライン | `engine.run_async` + 8 同時実行の `asyncio.gather` + `run_stream_async`。 |
| 05 | [`05_openai_provider.py`](05_openai_provider.py) | **OpenAI** | `OpenAIProvider` + 組み込み `llm_call` ノード。一発で完成。 |
| 06 | [`06_react_agent.py`](06_react_agent.py) | **OpenAI** | ReAct ループ: `llm_call` ↔ `tool_dispatch` (条件付き `has_tool_calls`)。 |
| 07 | [`07_checkpoint_hitl.py`](07_checkpoint_hitl.py) |オフライン |モック LLM エミッターを使用した 2 段階の提案/承認ワークフロー。 |
| 08 | [`08_intent_routing.py`](08_intent_routing.py) | **OpenAI** |分類子ノード + 条件付きエッジ → 数学 / 翻訳 / 一般的な専門家。 |
| 09 | [`09_state_management.py`](09_state_management.py) |オフライン | `set_checkpoint_store(InMemoryCheckpointStore())` + `get_state` + `fork`。 |
| 10 | [`10_command_routing.py`](10_command_routing.py) |オフライン | `run(input)` は `Command(goto_node=…, updates=[…])` を返します。 |
| 11 | [`11_reflexion.py`](11_reflexion.py) | **OpenAI** |反省プロンプトを備えた俳優 + 批評家のループ (Shinn et al. 2023)。 |
| 12 | [`12_self_ask.py`](12_self_ask.py) | **OpenAI** | Self-Ask のフォローアップ質問の分解 (Press et al. 2022)。 |
| 13 | [`13_multi_agent_debate.py`](13_multi_agent_debate.py) | **OpenAI** | 2人のディベーター+ジャッジ。討論者は `Send` を介してファンアウトします。 |
| 14 | [`14_graph_to_json.py`](14_graph_to_json.py) |オフライン |グラフ定義を `.json` ファイルにシリアル化します。 |
| 15 | [`15_graph_from_json.py`](15_graph_from_json.py) |オフライン | `.json` グラフをロードして実行します (14 に関連)。 |
| 16 | [`16_deep_research_chat.py`](16_deep_research_chat.py) | **OpenAI WS** | `조사해줘 / research / investigate` の並行ディープリサーチサブグラフに切り替わるマルチターン Gradio チャット。 `SchemaProvider("openai_responses", use_websocket=True)`を使用します。 `pip install gradio`が必要です。 |
| 17 | [`17_deep_research_crawl4ai.py`](17_deep_research_crawl4ai.py) | **OpenAI WS + Crawl4AI + Postgres** | 16 と同じチャット形式ですが、研究者は実際にローカルの Crawl4AI コンテナー (`docker run unclecode/crawl4ai`) を介して Web を検索し、状態は Postgres で永続的です (`PostgresCheckpointStore`)。どちらも環境変数を介してオプションです。不在の場合は正常に元に戻ります。 Postgres パスの `-DNEOGRAPH_BUILD_POSTGRES=ON` を使用してソース ビルドします。 |
| 18 | [`18_node_cache.py`](18_node_cache.py) | **OpenAI** | `engine.set_node_cache_enabled("ask", True)` — 同じ入力に対する 2 回目の実行では、キャッシュされた `NodeResult` が 0 ミリ秒で再実行され、LLM 呼び出しは行われません。 `engine.node_cache_stats()` 経由の統計。 |
| 19 | [`19_streaming_messages.py`](19_streaming_messages.py) |オフライン | `from neograph_engine import message_stream` — コールバックをラップして、`LLM_TOKEN` イベントが LangChain 形状のメッセージ辞書 (`{role, content, content_so_far, node, metadata}`) として到着するようにします。 |
| 20 | [`20_otel_tracing.py`](20_otel_tracing.py) |オフライン | `from neograph_engine.tracing import otel_tracer` — エンジン イベントを OpenTelemetry スパンにブリッジします。 ConsoleSpanExporter を出荷します。 OTLP に交換して、Jaeger / Tempo / Honeycomb / Datadog に送信します。 |
| 21 | [`21_http2_transport.py`](21_http2_transport.py) | **OpenAI** | `SchemaProvider(..., prefer_libcurl=True)` — オプトイン HTTP/2 (libcurl) トランスポートとデフォルトの ConnPool (HTTP/1.1 キープアライブ)。 5 方向の並列バーストと印刷の両方で A/B を実行し、エンドポイントでの方が高速になります。デフォルトの ConnPool は、api.openai.com では高速です。 CF-WAF 互換性、企業プロキシ経由の TCP ファンアウトの低減、または HTTP/3 が必要な場合は反転してください。 |
| 22 | [`22_self_evolving_graph.py`](22_self_evolving_graph.py) | **OpenAI** |目標主導型の自己進化: エージェントが実行され、JSON 形状の目標に対して出力をスコアリングし、修正されたグラフ定義を提案するように LLM に要求します。スコア ≥ 1.0 または max_iters がヒットすると、ループが閉じます。修飾子の唯一の出力が新しいグラフ仕様であるプログラムとしての JSON を示します。 |
| 23 | [`23_evolving_chat_agent.py`](23_evolving_chat_agent.py) |オフライン (モック) / **OpenAI** |スレッドごとに進化するチャット エージェント: 永続的な複数ターンの会話。ターンの間に、蓄積された履歴に基づいてエージェントの JSON 定義が書き換えられます。エボリューション全体にわたるチェックポイント再開 (以前のメッセージは存続)、`__graph_meta__` 監査チャネル パターン、およびバリデーター境界 (ホワイトリスト ノード タイプ、必要なチャネル、エッジ接続) を示します。決定論的モックプロバイダーとヒューリスティックモックエボルバーを介して、API キーなしでエンドツーエンドで実行されます。 |
| 24 | [`24_tool_approval_gate.py`](24_tool_approval_gate.py) |オフライン |ツール ゲート (#89): `engine.set_tool_gate(...)` は、**ツールを実行する前に**、すべてのツール呼び出しに対して参照され、Allow/Allow-with-rewrite-args/Deny/Interrupt を返します。正規の承認プロンプト (*「エージェントは `rm -rf build/` を実行したいと考えています。許可しますか?」*) を表示します。そして重要なことに、人間が決定している間は無害な兄弟呼び出しは**実行されない**ため、拒否は実際には何も起こらなかったことを意味し、承認によって再実行されることはありません。 |
| 25 | [`25_async_tools.py`](25_async_tools.py) |オフライン |同時実行ツール (#96): `ng.Tool` の代わりに `ng.AsyncTool`、および 3 つの 300 ミリ秒ツールでは、0.90 秒ではなく 0.30 秒かかります。また、同じ実行で境界も測定します。Python 関数は実行中に GIL を保持し、スレッドの数が GIL を変更しないため、3 つの *CPU バウンド* ツールは 1 つのツールの 3.2 倍の時間がかかります。同時実行はオプトインであるため、既存のステートフル ツールが突然競合することはありません。 |
| 26 | [`26_mcp_tools.py`](26_mcp_tools.py) |オフライン | MCP (#95): `ng.mcp.MCPClient(url).get_tools()` はリモート ツール カタログを取得し、それを `NodeContext` に直接渡します。独自の MCP サーバーを起動するため、ネットワークなしで実行されます。重要なこと (HTTP 経由で 0.41 秒に 3 回の 0.4 秒の MCP 呼び出し) を測定し、それが「当てはまらない」部分を大声で言います。stdio には 1 つのパイプがあるため、これらの呼び出しはシリアル化されます。 |
| 27 | [`27_a2a_server.py`](27_a2a_server.py) |ローカルホスト | A2A ホスティング (#120): 公式 `a2a-sdk` は、JSON-RPC、タスク状態、エージェント カード、およびキャンセルを所有します。 `ProtocolHostAdapter.stream()` は、チェックポイント コンテキストを保持しながら、エンジン トークン イベントをチャンク化された A2A アーティファクトにマップします。 Python 3.10 以降と `pip install "neograph-engine[a2a]"` が必要です。 |
| 28 | [`28_acp_agent.py`](28_acp_agent.py) | stdio | ACP ホスティング (#120): トークン更新をストリーミングし、グラフのテキスト/画像/オーディオ/リソース コンテンツ ブロックを保存し、`NEOGRAPH_ACP_POSTGRES_URL` または `NEOGRAPH_ACP_SQLITE_PATH` が設定されている場合に耐久性のある `session/load` をサポートします。 Python 3.10 以降と `pip install "neograph-engine[acp]"` が必要です。 |

## ホスティングで公式 SDK を使用する理由

C++ ライブラリには独自の `A2AServer` と `ACPServer` がありますが、それらを公開します
クラスを直接使用すると、Python ユーザーに 2 番目のプロトコル実装が提供されます。
Python の公式 SDK よりも統合が弱い。特に公式は、
SDK は、現在のワイヤ形式の互換性、サーバー トランスポート、タスク、または
セッションのライフサイクルと非同期キャンセル。 NeoGraph は部品のみを供給します
これらの SDK では、C++ グラフ エンジンへのチェックポイントを認識した呼び出しはできません。

|アイテム |決定 |
|------|----------|
|不足していると思われる C++ 機能 | `A2AServer`、`ACPServer`、およびそれらのライフサイクル メソッドは、Python クラスとしてミラーリングされません。 |
| Python の代替案 |公式 `a2a-sdk` 1.x および `agent-client-protocol` 0.11.x サーバー ランタイム。 |
| NeoGraph の統合 | `ProtocolHostAdapter` は、プロトコル会話 ID を `RunConfig.thread_id` にマップし、`resume_if_exists` を有効にし、`LLM_TOKEN` イベントをストリーミングし、カスタム JSON セーフ入力ペイロードを受け入れ、アクティブな asyncio タスクをキャンセルします。 |
|依存関係ポリシー |どちらの SDK も Python 3.10 以降が必要であるためオプションですが、`neograph-engine` は Python 3.9 をサポートしています。 `neograph-engine[a2a]`、`neograph-engine[acp]`、または `neograph-engine[protocols]` をインストールします。 |
|耐久性のある ACP セッション |ホイールサポートの耐久性のあるバックエンドには `NEOGRAPH_ACP_POSTGRES_URL` を設定します。 `NEOGRAPH_BUILD_SQLITE=ON` を使用したソース ビルドでは、`NEOGRAPH_ACP_SQLITE_PATH` が設定される場合があります。エージェントは、`session/load` が設定されている場合にのみ `session/load` をアドバタイズします。新しいセッションは、最初に完了したプロンプトがチェックポイントを作成した後にロード可能になります。セッション ID はサーバーによって生成される機能であり、チェックポイントはプライベート `acp:` スレッド名前空間を使用します。セッションごとに 1 つのアクティブなエージェント プロセスを維持します。チェックポイント ストアは、プロセス間で同時ライターをシリアル化しません。 |
|電流制限 | ACP エディターのコールバック (`fs/read_text_file`、ターミナル呼び出し、許可プロンプト) は、共有 NeoGraph Python ツールからまだ安全に呼び出すことができません。現在の `AsyncTool` はワーカー スレッドで同期関数を実行し、現在のプロトコル セッション ID を伝えません。偽のブリッジを使用すると、間違ったエディター セッションを呼び出す危険があります。 |
| | の場合は、直接バインディングを再検討してください。ユーザーは、正確な C++ サーバーを Python に埋め込む必要があります。そうしないと、公式 SDK パスは、必要な NeoGraph のキャンセル、チェックポイント、トレース、またはツール呼び出しの動作を保存できません。 |

`ProtocolHostAdapter.run_payload()` は、JSON セーフな値を
`input_builder`を構成しました。デフォルトの `message_input` はリッチコンテンツを保持します
ユーザーメッセージの `content` としてブロックします。プロバイダーが別のグラフを期待しているグラフ
形状はカスタム ビルダーを渡す必要があります。 `ProtocolHostAdapter.stream()` の利回り
`ProtocolStreamEvent(kind="token", ...)` 値の後に最後の 1 つだけが続く
イベント。 `stream_node` がグラフ ノードを指定しない限り、ライブ トークンは無効になります。
トークンはまさに最終的な答えを形成します。これにより、プランナー/ツールノードの出力が妨げられます。
プロトコル応答を介した漏洩を防ぎます。 asyncio コンシューマ キューは制限されています
(デフォルトでは 1,024 チャンク);オーバーフローするとエンジンの運転が中止されます。ネイティブストリームイベント
は最初に asyncio ループにスケジュールされるため、このキューは
プロトコル転送が遅く、プロセス全体のメモリ制限が厳しくない
無制限のネイティブプロデューサー。

次のいずれかを実行します。

```bash
python 01_minimal.py
```

## メンタルモデル

Python の NeoGraph は Python の LangGraph に似ています。
ノード、リデューサー付きチャネル、`Send` 経由の動的ファンアウト、ルーティング
`Command` によるオーバーライド、名前付き条件による条件付きエッジ
(`route_channel`、`has_tool_calls`など)。同じプリミティブでも、
同じ JSON 形状のグラフ定義。違いは何が実行されているかです
それ - スーパーステップ ループ、スケジューリング、および
LangGraph の代わりにステップごとにマイクロ秒単位でチェックポイントを作成する
～600μs。

例全体で 3 つのパターンが表示されます。

1. **Python カスタム ノード** (01、03、04、07、09、10、11、12、13)
   `neograph_engine.GraphNode` をサブクラス化し、`run(input)` を実装します。
   `input.state` からチャネルを読み取り、存在する場合は `input.stream_cb` を使用します。
   書き込み、`Command`、`Send`、または `NodeResult` を返します。エンジン
   GIL 処理の下で Python にディスパッチされるため、カスタム ノードが同時に実行されます
   デッドロックしないでください。

2. **Python ツール** (02、06、07) サブクラス `neograph_engine.Tool` および
   インスタンスを `NodeContext(tools=[…])` に渡します。エンジンはかかります
   コンパイル時の所有権。 Python 参照は後で削除される可能性があります。

3. **非同期** (04) — すべての `*_async` バインディングは、
   `asyncio.Future` は、呼び出しスレッドの実行ループにバインドされます。
   ストリーム コールバックは、次を介してループ スレッドにホップされます。
   `loop.call_soon_threadsafe`、つまり `cb(ev)` は asyncio で実行されます
   期待しています。

## グラフ定義はJSONです

`GraphEngine.compile(definition, ctx)` は Python のいずれかを受け入れます
`dict` コードでビルドするか、`dict` ファイルから `json.loads()` をビルドします
— 同じ形。例 14 + 15 は往復を示しています。カスタムノード
*types* をコード内で登録する必要があります (Python クラスは登録できません)
JSON にエンコードされます）が、配線 - チャネル、タイプごとのノード、
エッジ、条件付きエッジ — データです。

## ディストリビューション名とインポート名

PyPI パッケージは **`neograph-engine`** (ベア `neograph` 名) です。
すでに無関係なプロジェクトによって PyPI が採用されていました)。パイソン
インポート名は `neograph_engine` です。

```python
import neograph_engine as ng
from neograph_engine.llm import OpenAIProvider, SchemaProvider
```
