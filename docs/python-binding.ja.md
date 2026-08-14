<!-- neograph-i18n: source=docs/python-binding.md locale=ja source_sha256=7aebcfae1d0d7f27b5e78cdb23be69a84006759e67f4895488a182add12533ef -->
# Python バインディング

**Languages:** [English](python-binding.md) | [한국어](python-binding.ko.md) | [日本語](python-binding.ja.md) | [简体中文](python-binding.zh-CN.md)

NeoGraph は、`pip` でインストール可能な Python パッケージとして出荷されるため、同じ
C++ エンジンは、Jupyter から LangGraph スタイルのワークフローを駆動できます
ノートブック、Gradio アプリ、または FastAPI サービス:

```bash
pip install neograph-engine
```

## 5 秒のデモ (API キーなし)

インストールが機能したことを証明する最も短いもの - 1 つのデコレータ定義
ノードを実行し、出力を読み取ります。

```python
import neograph_engine as ng

@ng.node("greet")
def greet(state):
    return [ng.ChannelWrite("messages",
        [{"role": "assistant", "content": f"Hello, {state.get('name')}!"}])]

definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "demo",
    "channels": {"name":     {"reducer": "overwrite"},
                 "messages": {"reducer": "append"}},
    "nodes":    {"greet": {"type": "greet"}},
    "edges":    [{"from": ng.START_NODE, "to": "greet"},
                 {"from": "greet",       "to": ng.END_NODE}],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())
result = engine.run(ng.RunConfig(thread_id="t1", input={"name": "NeoGraph"}))

print(result.output["channels"]["messages"]["value"])
# [{'role': 'assistant', 'content': 'Hello, NeoGraph!'}]
```

## 実際の LLM を使用した ReAct エージェント

```python
import neograph_engine as ng
from neograph_engine.llm import OpenAIProvider

class CalcTool(ng.Tool):
    def get_name(self):       return "calc"
    def get_definition(self): return ng.ChatTool(name="calc", description="multiply by 2",
        parameters={"type":"object","properties":{"x":{"type":"number"}}})
    def execute(self, args):  return str(args["x"] * 2)

ctx = ng.NodeContext(
    provider=OpenAIProvider(api_key="sk-..."),
    tools=[CalcTool()],
    instructions="Use `calc` for arithmetic.",
)

definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "react",
    "channels": {"messages": {"reducer": "append"}},
    "nodes":    {"llm": {"type": "llm_call"}, "dispatch": {"type": "tool_dispatch"}},
    "edges":    [{"from": ng.START_NODE, "to": "llm"}, {"from": "dispatch", "to": "llm"}],
    "conditional_edges": [{"from": "llm", "condition": "has_tool_calls",
                           "routes": {"true": "dispatch", "false": ng.END_NODE}}],
}
engine = ng.GraphEngine.compile(definition, ctx)
result = engine.run(ng.RunConfig(thread_id="t1",
    input={"messages": [{"role": "user", "content": "What is 21 * 2?"}]},
    max_steps=10))
```

## 出力の読み取り

`engine.run(...)` は、次のフィールドを含む `RunResult` を返します。

|フィールド |タイプ |意味 |
|---|---|---|
| `output` | `dict` |最終状態 — `{"channels": {...}, "global_version": int}`。 `output["channels"][name]["value"]` を使用してチャネルを読み取ります。 |
| `max_steps_exhausted` | `bool` | `True` は、実行可能な作業が残っている間にステップ天井が実行を停止した場合のみ。 |
| `interrupted` | `bool` |実行が `interrupt_before` / `interrupt_after` / `NodeInterrupt` で一時停止された場合は、`True`。 |
| `interrupt_node` | `str` |割り込みをトリガーしたノードの名前 (`interrupted` の場合)。 |
| `interrupt_value` | `dict` |動的割り込みの場合は `{"reason": str, "type": "NodeInterrupt", "value": ...}` (ノードがペイロードをアタッチした場合にのみ `"value"` が存在します)、静的 `interrupt_before` / `interrupt_after` の場合は `{"message": ...}`。 |
| `checkpoint_id` | `str` |実行中に保存された最新チェックポイントの ID。参考情報であり、`resume_async()` はチェックポイント ID ではなく `thread_id` で再開します。 |
| `execution_trace` | `list[str]` |実行された順序でのノード名 - ルーティングのデバッグに役立ちます。 |

`RunConfig` は LangGraph の `RunnableConfig` の考え方に対応します。

中断した実行を非同期で再開するには、スレッド ID と、必要に応じて人間の回答を渡します。

```python
result = await engine.resume_async(thread_id="t1", resume_value=answer)
```

|フィールド |デフォルト |意味 |
|---|---|---|
| `thread_id` |必須 |会話 / セッション ID — チェックポイント ストリームを分離します。 |
| `input` | `{}` |初期チャネル値 — キーはグラフの `channels` 定義と一致する必要があります。 |
| `max_steps` | 50 |スーパーステップ天井。 ReAct ループには通常 10 以上が必要です。 |
| `stream_mode` | `StreamMode.OFF` |ビットマスク: `EVENTS \| TOKENS \| DEBUG \| VALUES \| UPDATES \| ALL`。 `run_stream` / `run_stream_async` によってのみ参照されます。 |
| `resume_if_exists` | `False` | `True` とチェックポイント ストアが構成されている場合、実行により `thread_id` (存在する場合) の最新のチェックポイントがロードされ、チャネル リデューサーを介して `input` が上に適用されます。これは、`input` を通じて以前の状態を手動でスレッドすることなく、マルチターン チャットを実行します。デフォルトでは、後方互換のためのフレッシュスタートセマンティクスが維持されます。中断された実行から HITL を再開する場合は、代わりに `engine.resume_async()` を使用してください。 |
| `cancel_token` | `None` |連携キャンセル用のオプションの`CancelToken`。 `engine.run()` の前に 1 つを割り当ててから、別の Python スレッドから `token.cancel()` を呼び出します。エンジンは次のスーパーステップ境界で停止します。長時間の作業を行うノードは、`input.ctx.cancel_token` をポーリングする必要があります。 |

```python
token = ng.CancelToken()
config = ng.RunConfig(thread_id="job-42", input={"query": "..."})
config.cancel_token = token

# Run engine.run(config) in a worker thread, then request cancellation from
# the caller thread when the request disconnects or the user presses Stop.
token.cancel()
```

## Python ノードから人間のために一時停止する

グラフ定義の `interrupt_before` は、選択したノードで一時停止します。
グラフを書きました。それは人間参加者が実際に存在する場合を表現することはできません
なぜなら、その一歩が危険かどうかは、モデルが何を求めたかによって決まるからです。
のために：

> *「エージェントは `rm -rf build/` を実行したいと考えています。許可しますか?」*

そのために、ノード自体が決定します。`NodeInterrupt` を発生させ、何をアタッチしますか
承認が必要です。エンジンはチェックポイントをチェックし、通常の `RunResult` を返します。
(あなたには何も提起されません)、あなたの答えは質問したノードに戻ります。

```python
import neograph_engine as ng

class ApprovalNode(ng.GraphNode):
    def run(self, input):
        # The human's answer. None until someone has actually answered — which
        # is how you tell "nobody has looked yet" from "the answer was no".
        verdict = input.ctx.resume_value

        if verdict is None:
            raise ng.NodeInterrupt(
                {"tool": "shell", "cmd": "rm -rf build/"},
                reason="shell command needs approval")

        if not verdict.get("approved"):
            return [ng.ChannelWrite("result", "refused")]
        return [ng.ChannelWrite("result", "done")]

    def get_name(self):
        return "risky"
```

```python
result = engine.run(cfg)

if result.interrupted:
    print(result.interrupt_node)               # "risky"      — which node paused
    print(result.interrupt_value["reason"])    # for a human to read
    print(result.interrupt_value["value"])     # for your code to branch on

    result = engine.resume(cfg.thread_id, {"approved": True})   # the answer
```

プレーン文字列を使用した `NodeInterrupt(reason)` も機能し、`"value"` は省略されます。
鍵。それ以外に発生させたものはエラーのままです。ノードのバグにより実行は失敗します。
人間への質問のように見えるのではなく、大きな声で。

チェックポイント ストアが必要です。それ以外の場合は再開するものがありません。

## 会話を通じてユーザーを思い出す - ストア

チェックポイントは **1 つの会話**を記憶します。ストアは**ユーザー**を記憶しており、
それらすべてにわたって。

```python
store = ng.InMemoryStore()
engine.set_store(store)

class Greet(ng.GraphNode):
    def run(self, input):
        seen = input.ctx.store.get(["users", "u1"], "visits")
        n = (seen.value["n"] if seen else 0) + 1
        input.ctx.store.put(["users", "u1"], "visits", {"n": n})
        return [ng.ChannelWrite("greeting", f"visit #{n}")]
```

名前空間は階層リストであるため、`store.search(["users"])` はすべてを検索します
すべてのユーザーの下で、`store.search(["users", "u1"])` は 1 人のユーザーのアイテムを検索します。
欠席の場合、`get()` は `None` を返します。欠席はエラーではなく回答です。

代わりに `ng.Store` をサブクラス化してデータベースに置きます。

カスタム チェックポイントの永続性も同様に機能します: サブクラス
`ng.CheckpointStore` と `save`、`load_latest`、`load_by_id`、`list` を実装します。
そして`delete_thread`。オプションの `put_writes`、`get_writes`、
`clear_writes` メソッドは、デフォルトで no-op/フルスーパーステップ再生動作になります。価値観
`StoreItem`、`Checkpoint`、および `PendingWrite` の内部は通常の Python JSON です
形状 (`dict`、`list`、文字列、数値、ブール値、および `None`)。

## 429 からバックオフする — RateLimitedProvider

```python
from neograph_engine.llm import RateLimitedProvider, OpenAIProvider

provider = RateLimitedProvider(OpenAIProvider(...), max_retries=5)
engine = ng.GraphEngine.compile(definition, ng.NodeContext(provider=provider))
```

これがないと、`engine.run()` を独自の再試行ループでラップすることになります。
**グラフ全体**を再試行します - すでに成功したすべてのノードを再実行します。これ
失敗した 1 つの HTTP リクエストを再試行します。

アップストリームの `Retry-After` が存在する場合はそれを尊重し、それにフォールバックします。
`default_wait_seconds` がない場合、1 回のスリープの上限は次のとおりです。
`max_wait_seconds`、睡眠の`max_total_wait_seconds`が終わったら諦める
蓄積されます (`0` = 合計上限なし)。

独自のプロバイダーは、適切な例外を発生させることでオプトインします。

```python
class MyProvider(ng.Provider):
    def complete(self, params):
        r = requests.post(...)
        if r.status_code == 429:
            raise ng.RateLimitError(
                "rate limited",
                retry_after_seconds=int(r.headers.get("Retry-After", -1)))
```

それ以外に発生させたものはすべてエラーのままです。

## 実行前にグラフを確認する — `validate`

```python
report = ng.validate(definition)
if report.has_errors():
    print(report.summary())
    for d in report.errors():
        print(d.code, d.path, d.message)
```

ぶら下がったエッジ、到達不能なノード、デッドバリア - むしろ読むことができるレポート
`compile()` がいつスローされるかを知るよりも。

知っておくべきエッジの 1 つ: `validate()` は最初に定義をコンパイルするため、ノード
「誰もサーフェスを診断ではなく **例外**として登録しました」と入力します。登録する
コンパイル前とまったく同じように、検証する前にノードの型を指定します。

**ノード レベルでの再試行にはクラスは必要ありません。** `"retry_policy": {...}` を
グラフ定義とエンジンはそれを尊重します。これは常に Python から機能します。

```python
definition["retry_policy"] = {"max_retries": 5, "initial_delay_ms": 100}
```

## MCP — リモートツールサーバーの使用

```python
client = ng.mcp.MCPClient("http://localhost:8931")     # or ["python", "server.py"]
client.initialize()

engine = ng.GraphEngine.compile(
    definition, ng.NodeContext(tools=client.get_tools()))
```

それが全体の統合です。 `get_tools()` はサーバーのカタログを次のように返します。
グラフがディスパッチできるツール、およびそれらを独自の Python と自由に組み合わせることができます
同じ `NodeContext` 内のツール。

`client.call_tool(name, args)` は、グラフの外で直接呼び出します。

**サーバーが重複する場合は重複します。** MCP ツールはネットワークの往復であり、
これは同時ディスパッチが有効な場合であり、`MCPTool` は実際の C++ です
`AsyncTool`。 HTTP は同時リクエストを使用します。 stdio フレームの書き込みと関連付け
JSON-RPC ID による順不同の応答:

|輸送 | 3 コール × 0.4 秒 |
|---|---|
| HTTP | **0.41 s** — 各呼び出しは独自のリクエストです |
| stdio | **~0.4 s** (同時サーバー使用) - 1 つのパイプ、リクエスト ID 多重化 |

リクエストをシリアルに処理するサブプロセスには、依然として約 1.2 秒かかります。多重化
クライアント側のボトルネックを解消します。サーバー側の同時実行性を作成することはできません。

`get_tools()` および `call_tool()` の場合、初期化は自動的に行われます。
`initialize()` は有効かつ冪等のままです。 `get_initialize_result()` が公開します
ネゴシエートされたプロトコル、機能、サーバー情報、および指示。
`get_tool_definitions()` はすべてのページネーション カーソルを追跡し、完全な MCP を保持します
メタデータ。必要な場合は、`call_tool_result()` または `MCPTool.execute_result()` を使用してください。
`structured_content`、非テキスト ブロック、`is_error`、または `_meta`。 `call_tool()`は
ソース互換性のある生の JSON ファサード。

stdio サブプロセスは、セッションへの最後の参照が終了すると終了します。
ドロップされた — クライアント、またはクライアントが作成したツール。

実行可能、オフライン (独自の MCP サーバーを起動します): [`examples/26_mcp_tools.py`](../bindings/python/examples/26_mcp_tools.py)。

## ツールを同時に実行する

モデルが 1 回のターンで複数のツールを要求すると、NeoGraph はそれらをディスパッチします。
一緒に。実際に「重なり合う」かどうかはツールが選択します。

```python
class Fetch(ng.AsyncTool):          # ng.Tool -> serial;  ng.AsyncTool -> overlaps
    def execute(self, arguments):
        return requests.get(arguments["url"]).text
    ...
```

それぞれ 300 ミリ秒待機する 20 個のツールを測定:

|ツール基本クラス |壁掛け時計 |
|---|---|
| `ng.Tool` | 6.0秒 |
| `ng.AsyncTool` | **0.30 秒** (19.9×) |

**オプトインの理由** 同期 `Tool` は、次の同期の前に完了するまで実行されます。
が開始されるため、状態を保持する既存のツールが突然自分自身を見つけることはできません
自分自身のコピーをレースします。同時実行性は宣言するものであり、何かを宣言するものではありません
それはあなたに起こります。裏返し: 同じ `AsyncTool` への 2 つの呼び出しは可能です。
一度に飛行します (モデルは 1 回のターンに 2 回飛行を要求する場合があります)。したがって、呼び出しごとに続けてください。
状態は `self` ではなく、スタック上にあります。

**明確に述べた境界。** Python 関数は、実行中に GIL を保持します。
ツールがその兄弟と重なるのは、ツールがそれを保持していない間だけです。
これは、I/O がブロックされている間です。それは、CPython が解放されるときだからです。 HTTP
呼び出し、ソケット読み取り、データベース クエリ、`time.sleep`: すべてが解放され、すべてが重複します。

**Python** で CPU を焼き付けるツールは、本体全体の GIL を保持しており、
どれだけ多くのスレッドが渡されても重複しません。

| 3 CPU バウンドの `AsyncTool` | 1 回の 3.1 倍 |
|---|---|

このようなツール `AsyncTool` を宣言しても何も意味がありません。 （重労働が発生した場合
numpy、C 拡張機能、またはサブプロセス内では、GIL がそこで解放され、
）これはテストによって固定されているため、主張が静かに漂流することはできません。

同時実行性は内部ワーカー プールによって制限されます (デフォルトでは 32 スレッド、または
`NEOGRAPH_TOOL_THREADS`。彼らは I/O のブロックに時間を費やしているため、寛大な
プールの費用はほとんどかかりません。

実行可能、オフライン: [`examples/25_async_tools.py`](../bindings/python/examples/25_async_tools.py)。

## ゲート ツールの呼び出し - 「エージェントは `rm -rf build/` を実行したいと考えています。許可しますか?」

*モデルがツール X を要求した* と *ツール X が実行された * の間には 1 つのフックがあり、
次の 3 つの評決のうち 1 つを返します。

```python
def gate(call, gctx):
    if call.name not in DANGEROUS:
        return ng.ToolDecision.allow()

    # None until a human has actually answered — which is how the gate tells
    # "nobody has been asked yet" from "the answer was no", and so avoids
    # asking the same question forever.
    if gctx.resume_value is None:
        return ng.ToolDecision.interrupt(
            f"{call.name} needs approval",
            {"tool": call.name, "arguments": call.arguments})

    if gctx.resume_value.get("approved"):
        return ng.ToolDecision.allow()
    return ng.ToolDecision.deny("the operator refused this command")

engine.set_tool_gate(gate)
```

```python
result = engine.run(cfg)
if result.interrupted:
    print(result.interrupt_value["reason"])   # "shell needs approval"
    print(result.interrupt_value["value"])    # {"tool": ..., "arguments": ...}
    result = engine.resume(cfg.thread_id, {"approved": True})
```

|評決 |効果 |
|---|---|
| `ToolDecision.allow()` |実行してください。 |
| `ToolDecision.allow({...})` |代わりにこれらの引数を指定して実行してください。これは、すべてのツールがアンビエント値 (テナント、スレッド、資格情報) を認識するのではなく、アンビエント値 (テナント、スレッド、資格情報) が挿入される場所です。 |
| `ToolDecision.deny(reason)` |実行しないでください。理由はツールの結果としてのモデルに遡るため、次のターンに同じツールを再度要求するのではなく適応できるようになります。 |
| `ToolDecision.interrupt(reason, payload)` |実行せずに、実行全体を一時停止してください。ペイロードは `RunResult.interrupt_value["value"]` に現れます。 |

許可、監査、引数の書き換え、および呼び出しごとの割り込みは 4 つではありません
特徴;彼らは四つの帽子をかぶった一人の原始人です。

**ゲートはツールを実行する前にすべての呼び出しを確認し、その順序付けは
design.** モデルが `list_files` と `shell` を同時に要求し、
`shell` には承認が必要です。実行が一時停止すると、`list_files` は**実行されません**
――たとえ門がそれを許していたとしても。

それは見落としではありません。 `resume()` は先頭からノードに再入力します。
中断したノードは書き込みを記録しませんでした。 `list_files` がすでに実行されている場合、
承認すると *2 回目* 実行されます。これを `git commit` とプロンプトに置き換えます。
`rm -rf` はダブルコミットしました。そして人間が**ノー**と言ったとしても、何でも
すでに実行されたものを元に戻すことはできません。 「拒否」が許可される許可システム
「何も起こらなかった」という意味ではなく、許可システムではありません。

2 つの実用的な注意事項:

- **チェックポイント ストアが必要です。** 割り込みは再開可能である必要があります。それなし
  再開できるものが何もない店。
- **ゲートは `RunConfig` ではなく、エンジン上に存在します。** `resume()` は独自のゲートを構築します。
  内部的には `RunConfig` なので、実行ごとのゲートは人間が操作した瞬間に消えてしまいます。
  表示されたプロンプトに応答すると、危険なツールが実行されます。
  チェックされていない。エンジンに一度設定すると、実行および再開するたびに保持されます。

エンドツーエンドで実行可能、API キーなし: [`examples/24_tool_approval_gate.py`](../bindings/python/examples/24_tool_approval_gate.py)。

## 減速機内蔵

チャネルには、新しい書き込みが既存の値とどのように結合されるかというリデューサーが必要です。
2 つの組み込み機能が本日出荷されます。

|減速機 |行動 |一般的な使用法 |
|---|---|---|
| `"overwrite"` |新しい値が古い値に置き換わります。 |単一値チャネル: `name`、`current_question`、中間スクラッチ。 |
| `"append"` |新しいリストが既存のリストに連結されます。 |会話履歴、中間結果、ノード間で蓄積したいもの。 |

カスタム リデューサーは Python から登録します (v0.1.9 以降):

```python
ng.ReducerRegistry.register_reducer("sum",
    lambda current, incoming: (current or 0) + incoming)

# Now `"reducer": "sum"` works in your channel definitions.
```

条件付きルーティングの同じパターン — `ng.ConditionRegistry.register_condition("name", fn)`
ここで、`fn(state) -> str` はルート キーの 1 つを返します。

## バインディングでカバーされる内容

- **エンジン表面** — `GraphEngine.compile / run / run_stream / run_async / run_stream_async / resume / resume_async / get_state / get_state_history / update_state / fork`、`RunConfig`、`RunResult`、`set_worker_count`、`set_checkpoint_store`、`set_node_cache_enabled`。
- **カスタム Python ノード** — サブクラス `neograph_engine.GraphNode`、`NodeFactory.register_type` または `@neograph_engine.node` デコレーター経由で登録します。エンジンは、ファンアウト ワーカー スレッドからなど、適切な GIL 処理の下でディスパッチされます。
- **カスタム Python ツール** — サブクラス `neograph_engine.Tool`、`NodeContext(tools=[...])` に渡します。エンジンはコンパイル時に所有権を取得します。
- **非同期** — すべての `*_async` バインディングは、呼び出しスレッドの実行ループにバインドされた `asyncio.Future` を返します。ストリーム コールバックは `loop.call_soon_threadsafe` 経由でループ スレッドにホップされるため、コールバックは asyncio が期待する場所で実行されます。
- **チェックポイント** — Python バックエンドでは `CheckpointStore` をサブクラス化するか、`InMemoryCheckpointStore` を直接使用できます。`-DNEOGRAPH_BUILD_POSTGRES=ON` でバインディングをビルドすれば `PostgresCheckpointStore` も利用でき、対応するホイールには libpq が同梱されます。
- **WebSocket を介した OpenAI 応答** — `SchemaProvider(schema="openai_responses", use_websocket=True)`。

ホイール: Linux x86_64 (manylinux_2_34)、Linux aarch64 (manylinux_2_34)、
macOS arm64 (14+)、Windows x64 (MSVC)、Python 3.9 → 3.13 用。 **ホイール20個
+ リリースごとの sdist ** cibuildwheel 経由。

詳細については、[`bindings/python/examples/`](../bindings/python/examples/) を参照してください。
完全な例のインデックス — 最小限のグラフ、ReAct、HITL、インテント ルーティング、非同期、
マルチエージェントのディベート、JSON グラフの往復、および Gradio チャット
ディープリサーチサブグラフ (Crawl4AI + Postgres オプション)。

## LangGraph（Pythonバインディング）との違い

売り文句は「LangGraph for C++」ですが、セマンティクスがいくつか異なります。
LangGraph Python — ポートの途中でヒットしないように、ここに表示されます。

- **マルチターン `thread_id` はオプトインです** — `engine.run(cfg)` と
  同じ `thread_id` は前のターンの自動ロードを**しません**
  デフォルトではチェックポイント。すべての実行は `cfg.input` から新たに開始されます。
  LangGraph スタイルの「ロード」に `cfg.resume_if_exists = True` を設定します
  最新の、入力を一番上に適用する」動作。デフォルトは `False` です。
  すでに `input` を通じて状態をスレッド化している呼び出し元自体は、
  影響を受けません。上記の `RunConfig` 表を参照してください。
- **`update_state` は `ChannelWrite` の辞書またはリストを受け入れます** —
  `update_state(thread_id, channel_writes, as_node='')` かかります
  `channel_writes` の 2 つの形状のいずれか:
  - dict: `{"messages": [...]}` — 直接キー入力されたフォーム、最も近いもの
    LangGraph の `values={...}` に (kwarg 名は異なります)。
  - リスト: `[ChannelWrite("messages", [...]), ...]` — と対称
    すべてのノード本体が発するもの。

  リスト項目は重複チャネルを含めて順番に適用され、
  `ChannelWrite.Mode.OVERWRITE` を保持します。dict フォームは各キー値を
  reducer に通します。他の型はサイレント no-op ではなく `TypeError` を発生させます
  (項目 #5 によって閉じられた v0.3.2 より前のトラップ)。
- **`get_state(thread_id)` はネストされた辞書を返します — `get_state_view`
  フラットヘルパー** — `state["channels"]["messages"]["value"]`
  正規の生の形状です (バージョン間で安定しています)。のために
  人間工学に基づいたドット アクセス、使用
  `view = engine.get_state_view(thread_id)` と読み取り、`view.messages`、
  `view.scratch`などを直接入力してください。 `view.raw` はフラット化されていないものを公開します
  バージョン/メタデータを必要とする呼び出し元の辞書。サブクラス `StateView`
  型付きアクセス用に宣言されたフィールド (Pydantic v2) を使用:
  `class ChatState(ng.StateView): messages: list[dict] = []`その後
  `engine.get_state_view(thread_id, model=ChatState)`。
- **Python `Provider` サブクラスは、同期 `complete` のみをバインドし、
  `complete_stream` メソッド** — 非同期仮想は Python にバインドされません
  ユーザー定義の Provider サブクラスにより、カスタム Python プロバイダーがサービスを提供します。
  エントリを同期します。非同期ネイティブプロバイダー統合の場合 (HTTP/2 多重化、
  他のコルーチンとの真の重複)、C++ に留まり、次から派生します。
  `neograph::CompletionProvider`があります。
- **一行トークンの発行** — `neograph_engine.streaming import より
  内部の Emit_token`, then `emit_token(cb, self._name, token)`
  ストリーミングノード。 4 行の `GraphEvent` 構造を置き換えます。
  儀式。
- **可観測性は個別の SaaS としてではなく、ツリー内で提供されます** — ペア
  `neograph_engine.tracing.otel_tracer` (ベンダー中立の OTel スパン)
  `neograph_engine.openinference.OpenInferenceProvider`+で
  `openinference_tracer` (LLM 形状の属性キー)、ポイント
  OpenInference 対応バックエンド (Phoenix、Arize、
  Langfuse — すべて OSS、すべて自己ホスト可能）、LangSmith を入手できます。
  UX (ターンごとのチャットバブル、DAG 階層、プロンプト/応答キャプチャ、
  ベンダーの SaaS 契約なしで、通話ごとのトークン数とコストがかかります。

  ```bash
  docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix
  pip install neograph-engine opentelemetry-exporter-otlp
  ```
  ```python
  from opentelemetry import trace
  from opentelemetry.sdk.trace import TracerProvider
  from opentelemetry.sdk.trace.export import BatchSpanProcessor
  from opentelemetry.exporter.otlp.proto.grpc.trace_exporter import OTLPSpanExporter
  from neograph_engine.openinference import OpenInferenceProvider, openinference_tracer

  trace.set_tracer_provider(TracerProvider())
  trace.get_tracer_provider().add_span_processor(
      BatchSpanProcessor(OTLPSpanExporter(endpoint="http://localhost:4317", insecure=True)))
  tracer = trace.get_tracer("my-app")

  wrapped = OpenInferenceProvider(OpenAIProvider(api_key=...), tracer)
  ctx = ng.NodeContext(provider=wrapped)
  engine = ng.GraphEngine.compile(graph, ctx)
  with openinference_tracer(tracer) as cb:
      engine.run_stream(cfg, cb)
  # → http://localhost:6006 renders the trace as a LangSmith-style chain.
  ```

  LangGraph がホストする LangSmith は典型的な可観測性パスです
  そのエコシステムの中で。 LangFuse / Phoenix は OSS の代替品です
  ただし、統合接着剤が必要です。 NeoGraphの`OpenInferenceProvider`
  *は* 統合接着剤です - すべての `Provider.complete()` にドロップインしてください
  は自動的に LLM スパンになります。
- **1 ノード方式** — `def run(self, input)` は で導入されました。
  **v0.4.0** であり、**v0.9.0** 以降の唯一のカスタム ノード オーバーライドです。
  `input.state` から状態を読み取り、ライブ
  `input.ctx.cancel_token` (ストリーミング シンク) からのハンドルをキャンセルします
  (または `None`) `input.stream_cb` から。 `list[ChannelWrite]` を返します。
  `list[Send]`、`Command`、または `NodeResult`。見る
  移動時は[`migration-v0.4-to-v1.0.md`](migration-v0.4-to-v1.0.md)
  古い `execute*` ノード。
- **2 つの Python デプス、ピリオド** — `pip install neograph-engine`
  `certifi` と `pydantic>=2.0` をプルし、それがランタイム全体です
  依存関係ツリー。グラフ エンジン、スケジューラー、チェックポイント ストア、
  HTTP/WebSocket クライアント、MCP/A2A/ACP トランスポート、OpenAI 互換
  プロバイダーと Postgres/SQLite チェックポイント バックエンドはすべてネイティブです
  ホイールに C++ が焼き付けられています。
  LangGraph の推移的ランタイムを比較: `langgraph` →
  `langchain-core` → `langchain` → `langchain-community` (それぞれ
  動きの速いパッケージ）、および統合ごとのパッケージ（`langchain-openai`、
  `langchain-anthropic`、`langchain-postgres`、`langchain-chroma`、…)。
  これが、動作していた LangGraph スクリプトが 6 か月後に壊れる理由です。
  Pydantic v1→v2 は 2024 年に世界をブレイクさせ、輸入経路は各地を漂っています
  すべてのマイナーリリース。
  NeoGraph の Python サーフェスは、凍結されたレイヤー上の薄い pybind11 レイヤーです。
  セマンティック バージョニング下の C++ ABI。に対して書かれたカスタム ノード
  v0.4.x `execute*` 互換性ウィンドウは `run(input)` に移行する必要があります。
  v0.9.0 では、v1 の準備中にレガシー ノード サーフェスが削除されました。
- **デプロイメントに Docker は必要ありません** — これは次の直接的な結果です。
  上の単一階層ツリー。本番環境の LangChain のデプロイメント
  事実上、Docker + 完全に固定された `requirements.txt` が *必要です。
  これがないと、推移的パッケージのサイレントマイナーバンプが次のパッケージに発生します。
  デプロイすると実行時にサーバーがダウンする可能性があります。 NeoGraph の車輪船
  完全なネイティブ ランタイムが組み込まれているため、次のようになります。

  - ベアメタル/VPS/a上の`pip install neograph-engine`
    サーバーレス機能は動作します - ホストの他の Python パッケージ
    NeoGraph の C++ エンジンにアクセスできません。
  - コンテナー イメージは **alpine + musl + ~20 MB** (engine .so +
    Python インタープリター + 2 deps)、または静的にリンクされた C++ バイナリ
    **~1.2 MB** (`libc.so.6` を唯一の動的展開とした場合)
  - サーバーレス (Lambda、Cloud Run) でのコールド スタートは ms クラスであり、そうではありません。
    秒 — 歩くべき LangChain インポート グラフはありません。
  - ロックファイルのメンテナンスの負担はほぼゼロです。 `pydantic>=2.0`は
    ドリフトする可能性がある唯一の制約です。
    本番環境の午前 3 時ではなく、インストール時間です。
