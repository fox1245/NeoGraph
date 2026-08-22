<!-- neograph-i18n: source=docs/concepts.md locale=ja source_sha256=3d95cddd2a9d9ff0c7b8028968a5bfab4c44b404af3eff0115f8edfb25a7f1cc -->
# NeoGraphのコアコンセプト— 解説ガイド

**Languages:** [English](concepts.md) | [한국어](concepts.ko.md) | [日本語](concepts.ja.md) | [简体中文](concepts.zh-CN.md)

例に飛び込む前に、これを一度読んでください。これは、あなた自身が構築する順序でメンタルモデルを構築します：グラフ → チャネル → ノード → エッジ → fan-out → ルーティングオーバーライド → チェックポイント → ストリーミング。

コードサンプルは簡潔さのためPython側で記載しています。すべてはC++ APIに1:1で対応します（クラスシグネチャは[`reference-en.md`](reference-en.md)、公開ヘッダは`include/neograph/`以下を参照）。

> **LangGraphを以前使ったことがある場合:** プリミティブは意図的に同じです — リデューサー付きチャンネル、書き込みを発行するノード、条件付きエッジ、`Send`、`Command`、チェックポイント。READMEはNeoGraphの[2つのランタイムレイヤー](../README.md#two-runtime-layers)を要約しています。以下の説明は何も前提としません。

---

## 目次

(セクション8.5はv0.6.0で追加 — `Tracing — OpenTelemetry + Phoenix / Langfuse`。番号付き見出しは1〜9のままで、外部ドキュメントのリンクを安定させています。8.5はStreamingとCommon pitfallsの間に位置します。)


1. [全体像](#1-the-big-picture)
2. [チャンネルとリデューサー](#2-channels--reducers)
3. [ノード](#3-nodes)
4. [エッジと条件付きルーティング](#4-edges--conditional-routing)
5. [Send — 動的fan-out](#5-send--dynamic-fan-out)
6. [Command — ルーティング上書き + 状態パッチ](#6-command--routing-override--state-patch)
7. [チェックポイント、割り込み、HITL](#7-checkpoints-interrupts-hitl)
8. [ストリーミングイベント](#8-streaming-events)
9. [よくある落とし穴](#9-common-pitfalls)

---

<a id="1-the-big-picture"></a>
## 1. 全体像

NeoGraph **グラフ**は以下の4つの要素です:

| 要素 | 説明 | 定義元 |
|---|---|---|
| **チャンネル** | 共有状態における名前付きスロット。それぞれにリデューサーがあり、新しい書き込みを既存の値とどのように結合するかを定義します。 | `definition["channels"]` |
| **ノード** | 状態を読み取り、書き込みを発行する関数（オプションで`Send` / `Command`）。 | `definition["nodes"]` |
| **エッジ** | 静的で次のノードを指すポインタ。 | `definition["edges"]` |
| **条件付きエッジ** | 述語駆動のルーティング — 状態に基づいて複数の次のノードから1つを選択します。 | `definition["conditional_edges"]` |

実行は**スーパーステップループ**です：

```
1. ready_set = nodes routed from __start__
2. while ready_set is not empty:
   a. run all nodes in ready_set (in parallel if the executor allows)
   b. apply each node's writes to state
   c. collect their Send / Command / outgoing-edge signals
   d. plan_next_step → new ready_set
```

スーパーステップは、並列性、チェックポイント、およびストリーミングイベントの単位です。「今」実行できる2つのノードは同じスーパースステップであり、同じ入力状態を観測し、ステップ終了時にその書き込みは reducers を介して結合されます。

---

<a id="2-channels--reducers"></a>
## 2. チャンネルとReducer

すべての状態は名前付きチャネルに格納される。チャネルはノードをまたいで、またスーパーステップをまたいで持続し、ノードはチャネルへの書き込みによって通信する。

### チャンネルの定義

```python
"channels": {
    "messages":  {"reducer": "append"},     # conversation history
    "counter":   {"reducer": "overwrite"},  # latest value wins
    "summary":   {"reducer": "overwrite"},
}
```

### ビルトインレデューサー

| レデューサー | 新規書き込みセマンティクス | 典型的な用途 |
|---|---|---|
| `"overwrite"` | 新しい値が古い値に置き換わる。並列書き込み時は最後の書き込みが優先される。 | 単一値スクラッチ（現在のノード、現在の質問、ルートヒント）。 |
| `"append"` | 新しいリスト（リストである必要があります！）は既存のリストに連結されます。順序：前のステップの値が先、このステップの書き込みはnode-execution orderで後に追加されます。 | 会話メッセージ、検索結果、fan-outコレクション。 |

> 両方のリデューサは、エンジン起動時に`ReducerRegistry::ReducerRegistry()`へ登録されます（[`src/core/graph_loader.cpp`](../src/core/graph_loader.cpp)）。カスタムリデューサは、C++から`ReducerRegistry::register_reducer(name, fn)`を介して、または（v0.1.9以降）Pythonから登録します：
>
> ```python
> ng.ReducerRegistry.register_reducer("sum",
>     lambda current, incoming: (current or 0) + incoming)
> ```
>
> Pythonの呼び出し可能オブジェクトはGILの下で実行されます。並行するSend fan-outは、Pythonカスタムノードと同じ方法でその上で直列化されます。名前の再登録は、以前のリデューサを置き換えます。

### チャネルへの書き込み

ノードは`ChannelWrite`のリストを返します：

```python
return [
    ng.ChannelWrite("messages", [{"role": "assistant", "content": "Hi!"}]),
    ng.ChannelWrite("counter",  state.get("counter", 0) + 1),
]
```

値の形状はreducerと一致する必要があります：
- `"append"` → リストでなければなりません（連結されます）。
- `"overwrite"` → JSONシリアライズ可能な任意の値。

### ノードからの状態の読み取り

```python
def run(self, input):
    msgs    = input.state.get("messages") or []  # list of message dicts
    counter = input.state.get("counter") or 0
    ...
```

`state.get(channel)`はチャンネルの現在の値を返します。チャンネルが存在するがまだ書き込まれていない場合は`None`を返します。チャットメッセージへの型付きアクセスの場合、`state.get_messages()`は`list[ChatMessage]`を返します（`messages`チャンネルから解析されます）— これは`llm_call`によって内部的に使用されます。

### Versions

各チャンネルは単調増加する`version`番号を保持します。エンジンはこれをチェックポイントの差分比較と`state.channel_version(name)`検査APIに使用します。通常、これを直接読み取ることはありません。

---

<a id="3-nodes"></a>
## 3. ノード

ノードタイプを登録する3つの方法を、制御の度合いが増す順に示します。

### 3.1 組み込みノード

| `type`（JSON内） | 動作 | 設定 |
|---|---|---|
| `llm_call` | `provider->complete_async(messages, tools)`を呼び出し、アシスタントメッセージを`messages`に追加します。 | 読み取り `provider`, `model`, `instructions`, `tools` から `NodeContext`. |
| `tool_dispatch` | 最新のアシスタントメッセージの`tool_calls`を確認し、`Tool::execute`を介してそれぞれを実行し、`{role: "tool", tool_call_id, content}`の結果を追加します。 | 読み取り `tools` から `NodeContext`. |
| `intent_classifier` | LLMはユーザーの意図をN個のラベルのいずれかに分類し、選択したラベルを`__route__`に書き込みます。`route_channel`条件と組み合わせて使用します。 | `extra_config: {labels, prompt_template}` |
| `subgraph` | 別のグラフを単一ノードとして埋め込みます。内部状態は設定されたキー再マッピングを通じてマッピングされます。 | `extra_config: {graph_def, input_keys, output_keys}` |

### 3.2 `@ng.node`デコレータ（Pythonのみ）

書き込み専用ノードを定義する最も短い方法：

```python
@ng.node("greet")
def greet_node(state):
    name = state.get("name") or "world"
    return [ng.ChannelWrite("messages",
        [{"role": "assistant", "content": f"Hello, {name}!"}])]
```

デコレートされた関数は`list[ChannelWrite]`（または`None`、`[]`として扱われる）を返さなければなりません。`Send`や`Command`を出力することはできません。それらについては、`GraphNode`をサブクラス化してください。

### 3.3 完全な`GraphNode`サブクラス

完全な制御のために`run(input)`をオーバーライドします。これはv0.4.0で導入され、v0.9.0以降の唯一のカスタムノードエントリポイントです。1つのメソッド、1つのシグネチャです。

```python
class Researcher(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        # input.state    — read channels via input.state.get(...)
        # input.ctx      — RunContext (cancel_token, thread_id, step, ...)
        # input.stream_cb — non-None when running in streaming mode
        topic = input.state.get("topic")
        result = await_llm(topic, cancel_token=input.ctx.cancel_token)
        return ng.NodeResult(
            writes=[ng.ChannelWrite("findings", [result])],
            command=ng.Command(goto_node="evaluator"),  # optional
            sends=[],                                    # optional
        )
```

Pythonは `cancel_token`, `thread_id`, `step`, `stream_mode`, `store`、および `resume_value` を `input.ctx`上に公開します。C++の呼び出し元は `deadline` と `trace_id` を `RunMetadata`上に設定できます。エンジンはそれらをネストされたサブグラフを通じて伝播します。これら2つのフィールドは、Pythonバインディングではまだ公開されていません。

また、裸の `list[ChannelWrite]` を返すこともできます。`Send` や `Command` が不要な場合、バインディングはそれを `NodeResult` に自動的にリフトします。

> **v0.3.xからの移行:** 削除されたv0.4以前のマルチエントリノードAPIには1つの置き換えがあります。`run(input)`をオーバーライドします。`input.state`から状態を読み取り、非Noneの場合は`input.stream_cb`を通じてトークンを出力し、`input.ctx.cancel_token`からキャンセルトークンを読み取ります。

型を登録して、JSONローダーがインスタンス化できるようにします：

```python
ng.NodeFactory.register_type(
    "researcher",
    lambda name, config, ctx: Researcher(name),
)
```

ファクトリは`(name, per-node config, NodeContext)`を認識するため、同じクラスを異なる設定で複数の名前の下でインスタンス化できます。

### 3.4 ツール（別の概念。`tool_dispatch`が使用）

`Tool`はノードではありません。`tool_dispatch`が呼び出すものです。`ng.Tool`をサブクラス化し、3つのメソッドをオーバーライドし、インスタンスを`NodeContext(tools=[…])`に渡します。

```python
class CalcTool(ng.Tool):
    def get_name(self):       return "calc"
    def get_definition(self): return ng.ChatTool(name="calc", ...)
    def execute(self, args):  return str(args["x"] * 2)
```

エンジンはコンパイル時にツールリストのオーナーシップを取得します — ローカル参照はその後ドロップできます。

---

<a id="4-edges--conditional-routing"></a>
## 4. エッジ & 条件付きルーティング

### 静的エッジ

```python
"edges": [
    {"from": ng.START_NODE, "to": "llm"},
    {"from": "dispatch",    "to": "llm"},
    {"from": "summarizer",  "to": ng.END_NODE},
]
```

同じソースノードからの複数エッジはfan-outします(すべての後続ノードが次のスーパーステップの準備セットに入ります)。1つのスーパーステップから同じターゲットへの2つのエッジは、ターゲットの1回の実行に重複排除されます。

### 条件付きエッジ

条件付きエッジは**名前付き条件**を実行し、`routes`マップから次のノードを選択します。

```python
"conditional_edges": [
    {
        "from": "llm",
        "condition": "has_tool_calls",
        "routes": {"true": "dispatch", "false": ng.END_NODE},
    }
]
```

条件名は、エンジンに登録された`ConditionFn`に解決されます。2つが組み込みとして出荷されています：

| Condition | 戻り値 | 使用タイミング |
|---|---|---|
| `has_tool_calls` | `"true"` 最新のアシスタントメッセージが空でない`tool_calls`を持つ場合; それ以外の場合は`"false"`。 | ReActループ — LLMが要求をやめるまでツールのディスパッチを続けます。 |
| `route_channel` | `__route__`チャネルにある任意の文字列である場合、`"default"`にフォールバックします。 | 明示的なインテントルーティングのために`intent_classifier`とペアにしてください。 |

カスタム条件は、C++から`ConditionRegistry::register_condition(name, fn)`経由で、またはPythonから（v0.1.9以降）登録します：

```python
def is_long(state):
    msgs = state.get("messages") or []
    return "long" if len(msgs) > 10 else "short"

ng.ConditionRegistry.register_condition("is_long", is_long)
```

呼び出し可能オブジェクトは、ライブの`GraphState`を受け取り（そのため`state.get(channel)`と`state.get_messages()`が機能する）、条件付きエッジの`routes`キーの1つに一致する文字列を返す必要があります。

### 2つの同等な形式 — どちらも v0.1.8 以降で動作します

条件付きエッジは、`edges`配列内（`condition`フィールド付き）**または**別の`conditional_edges`ブロック内に存在できます。両方の形式が受け入れられます。どちらか明確な方を選択してください：

```python
# Form A — top-level (LangGraph parity, recommended for Python)
"edges":             [{"from": "__start__", "to": "llm"}, ...],
"conditional_edges": [{"from": "llm", "condition": "...", "routes": {...}}]

# Form B — inline (used by every C++ example)
"edges": [
    {"from": "__start__", "to": "llm"},
    {"from": "llm", "condition": "...", "routes": {...}},
]
```

> **履歴：** 形式Aはv0.1.8より前のグラフコンパイラによって黙って破棄されていました — READMEとすべてのPythonの例がそれを使用していたため、ReActループは単一のLLM呼び出しに退化していました。コミット`e23a523`で修正されました。0.1.7以下のホイールでこれが見られる場合は、アップグレードしてください。

---

<a id="5-send--dynamic-fan-out"></a>
## 5. 送信 — 動的 fan-out

`Send` は、次のステップのノード数が状態に依存するケース向けである。典型的な使用法: 検索トピックのリストをN個の並列リサーチャー呼び出しに分割する。

```python
class Planner(ng.GraphNode):
    def run(self, input):
        topics = decide_topics(input.state)            # e.g. 5 strings
        return ng.NodeResult(
            writes=[],
            sends=[ng.Send("researcher", {"topic": t}) for t in topics],
        )
```

エンジンの`run_sends_async`は、`researcher`を`Send`ごとに1回インスタンス化し、それぞれが独自の`state.get("topic")`を持ち、`asio::experimental::make_parallel_group`を介して並列に実行します。

### メンタルモデル

`Send(target, payload)`は「この状態パッチで`target`をインスタンス化し、それを準備完了セットに追加する」ことです。ペイロードは、ターゲットが`state`を見る前に状態書き込みとして適用されます。

並列グループが終了した後、次のスーパーステップのルーティングは、各Sendが生成したタスクの出力エッジ（または、それを発行した場合はその`Command.goto`）から来ます。

### 一般的な形: fan-out 5、fan-in to summarizer (要約)

```
planner ─┬─ Send("researcher", {topic: "A"})  ─┐
         ├─ Send("researcher", {topic: "B"})  ─┤
         ├─ Send("researcher", {topic: "C"})  ─┼─→ summarizer
         ├─ Send("researcher", {topic: "D"})  ─┤
         └─ Send("researcher", {topic: "E"})  ─┘
```

`researcher`の出力エッジは`{"from": "researcher", "to": "summarizer"}`のみです — 静的エッジと同じ重複排除ルールなので、サマライザは一度だけ実行されます。

### ワーカー数のチューニング

`build()`はデフォルトで`EngineConfig::worker_count == 1`になります — エンジン所有のスレッドプールはなく、fan-outブランチはコルーチン自身のエグゼキュータ上でインラインにディスパッチされます。これはアロケーションなしの高速パスであり、シーケンシャルなグラフには安価で、非スレッドセーフな状態を保持するノードにも安全です。

実際の並列処理を行うには、プールを明示的に選択してください。fan-out幅に合わせて正確にNを選ぶか、`set_worker_count_auto()` を `hardware_concurrency()` に使用します（フォールバックは4）:

```python
engine.set_worker_count(5)           # match a 5-way Send
# or
engine.set_worker_count_auto()       # hardware_concurrency()
```

マルチSend（またはマルチ出力エッジ）のfan-outがオプトインされたプールなしで実行される場合、NeoGraphはワンショットのstderr警告を発行し、サイレントなシリアル実行がレーダーの下を通過しないようにします。意図的にシリアルfan-outを駆動する場合（例: worker=1高速パスのベンチマーク）は、`NEOGRAPH_SUPPRESS_FANOUT_WARNING=1`で抑制します。

---

<a id="6-command--routing-override--state-patch"></a>
## 6. Command — ルーティングオーバーライド + 状態パッチ

`Command`により、ノードは次にどこへ進むかを決定し、同じ戻り値で状態を変更できます。これは通常の出力エッジをバイパスします。

```python
class Evaluator(ng.GraphNode):
    def run(self, input):
        if score(input.state) >= 0.8:
            return ng.NodeResult(
                writes=[],
                command=ng.Command(
                    goto_node="summarizer",
                    updates=[ng.ChannelWrite("verdict", "accepted")],
                ),
            )
        else:
            return ng.NodeResult(
                writes=[],
                command=ng.Command(
                    goto_node="planner",                  # loop back
                    updates=[ng.ChannelWrite("retries",  input.state.get("retries", 0) + 1)],
                ),
            )
```

### Command と conditional edge の使い分け方

- **Conditional edge (条件付きエッジ)** : ルーティングは、ノード logic を必要としない state の predicateに依存します。よりクリーンで宣言的です。
- **コマンド**: ルーティングはノード内に記述するのが最も自然なロジックに依存する — 複数基準のスコアリング、コンテンツ検査、再試行判断。また、状態を原子的に更新し、かつ次ノードを選定する唯一の方法でもある。

### fan-in 下でのラストライター勝ち

同じスーパーステップで複数のCommandが発火する場合（稀 — 複数の並列グループの兄弟がそれらを発行する場合のみ可能）、最後のものが優先されます。順序は並列グループの完了によって決定され、これは非決定的です — 最大1つの兄弟が`Command`を発行することを保証して、これに対応して設計してください。

---

<a id="7-checkpoints-interrupts-hitl"></a>
## 7. チェックポイント、割り込み、HITL

### チェックポイントストアの設定

```python
engine.set_checkpoint_store(ng.InMemoryCheckpointStore())
# or: engine.set_checkpoint_store(ng.PostgresCheckpointStore(...))   # if built with PG
```

ストアが接続されている場合、すべてのスーパーステップは`(thread_id, checkpoint_id)`をキーとしてチェックポイントをストアに書き込みます。`RunResult.checkpoint_id`フィールドが最新のものです。

### 静的割り込みポイント

```python
"interrupt_before": ["payment"],   # pause before this node runs
"interrupt_after":  ["llm"],       # pause after, before routing
```

エンジンは `RunResult` を `interrupted=True` と `interrupt_node` を設定して返します。再開するには：

```python
result = await engine.resume_async(thread_id="t1",
                                   checkpoint_id=result.checkpoint_id,
                                   new_input={...})  # optional
```

### `NodeInterrupt`による動的割り込み

ノード本体の内部からスローします（Python: `raise ng.NodeInterrupt(reason)`、C++: `throw NodeInterrupt(...)`）。エンジンはキャッチし、状態を永続化し、スローしたノードで中断された`RunResult`を返します — 同じ再開APIです。

一時停止の決定が中間ノード出力に依存する場合に便利です（例:「LLM が人間に見せる価値のあるものを生成したか?」）。

### タイムトラベル

`engine.fork(thread_id, from_checkpoint_id)`は過去のチェックポイントから開始する新しいスレッドを返します。「別の答え方をしていたらどうなっていたか」という分岐に役立ちます。

---

<a id="8-streaming-events"></a>
## 8. ストリーミングイベント

`run_stream` / `run_stream_async`はイベントが発生する際にコールバックを呼び出します。モードはOR可能なビットマスクです:

| モード | 出力を発火する |
|---|---|
| `EVENTS` | `NODE_START`, `NODE_END`, `INTERRUPT` |
| `TOKENS` | `LLM_TOKEN` — `Provider`からストリーミングされた各トークン |
| `DEBUG` | `__routing__` 次の準備完了セットを示すイベント |
| `VALUES` | `__state__` 各スーパーステップ後の完全な状態を含むイベント |
| `UPDATES` | `CHANNEL_WRITE` ごとの`ChannelWrite`イベント |
| `ALL` | 上記のすべて |

```python
def cb(event):
    print(event.type, event.node_name, event.data)

engine.run_stream(
    ng.RunConfig(thread_id="t", input={...},
                 stream_mode=ng.StreamMode.EVENTS),
    cb)
```

> **注:** `event.node_name`（`event.node`ではない）。C++構造体フィールドは`node_name`です。pybindは元の名前を保持します。

チャット形式のストリーミング（増分`content_so_far`を含むLangChain互換メッセージ辞書）には、ヘルパーを使用します：

```python
from neograph_engine import message_stream

engine.run_stream(
    ng.RunConfig(thread_id="t", input={...},
                 stream_mode=ng.StreamMode.TOKENS),
    message_stream(lambda chunk: print(chunk["content"], end="", flush=True)))
```

### `asio::io_context.run()` 配置（C++）

C++から`engine.run_stream_async()`を駆動する場合、外側の`asio::io_context.run()`はアプリケーションのメインスレッド（または通常のプロセス起動パスを通じて初期化された任意の長命スレッド）から呼び出す必要があります。テスト済みの良好な形状：

```cpp
// Main-thread driver — what examples/40 and the SchemaProvider tests use.
asio::io_context io;
asio::co_spawn(io, [&]() -> asio::awaitable<void> {
    result = co_await engine->run_stream_async(cfg, cb);
}, asio::detached);
io.run();
```

```cpp
// Dedicated worker thread driver — also fine.
std::thread t([&]() {
    asio::io_context io;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        result = co_await engine->run_stream_async(cfg, cb);
    }, asio::detached);
    io.run();
});
t.join();
```

> **既知の制限 — ネストされた `io.run()` HTTPサーバーワーカーコールバック内** (issue #16): `asio::io_context.run()` を `httplib::Server::set_chunked_content_provider` (またはそれに相当する、リクエストごとのワーカーコールバックで、それ自体が `Provider::complete_stream_async`のデフォルトブリッジを介して子スレッドを生成するもの) 内にネストすると、一部の glibc / OpenSSL の組み合わせで `getaddrinfo` においてSEGVが発生することが観察されています。ツリー内のテスト ([`tests/test_schema_provider_stream_async_nested_thread.cpp`](../tests/test_schema_provider_stream_async_nested_thread.cpp)) は構造的な形状をカバーしており、問題なくパスしますが、ダウンストリーム環境 (HTTPS経由の実際の `api.openai.com` 、TSan / ASan 下の glibc リゾルバ、同時リクエスト負荷) はテストスイートから完全に再現可能ではありません。**回避策:**
>
> 1. **`co_await provider->complete_async(...)` を使用し、`complete_stream_async` の代わりにHTTPサーバーコールバック内から使用し**、組み立てた応答を1つの `LLM_TOKEN` イベントとしてヘルパーから発行します。トークン型付けのUXは失われますが、エンジン＋ノード＋ツールループはエンドツーエンドで動作します。これはProjectDatePopの下流の `cpp_backend` が現在使用しているものです。
> 2. **`io.run()`をリクエストごとのコールバックの外に移動します**：エンジン用の専用ワーカースレッド上で1つの長命の`asio::io_context`を実行し、リクエストごとの作業をキューに入れ、結果をHTTPサーバーの応答シンクにポストバックします。SEGVと相関するリクエストごとのネストされた`std::thread`スパウンを回避します。

---

## 8.5 トレーシング — OpenTelemetry + Phoenix / Langfuse

ストリーミングと同じコールバック形状、異なるコンシューマー。OTelトレーサー発行コールバックを`engine.run_stream(cfg, cb)`に渡すと、すべての`NODE_START` / `NODE_END` / `ERROR` / `INTERRUPT`イベントがスパンになります。

2つのレイヤーがツリーに同梱されている:

  - `neograph_engine.tracing.otel_tracer` — ベンダーニュートラルなOTelスパン。スパンは任意のOTelバックエンド（Jaeger、Tempo、Honeycomb、Datadog）に流れます。
  - `neograph_engine.openinference` — LLM形状の属性レイヤで、同じスパンをPhoenix / Arize / Langfuseで*LangSmithスタイルのチャットバブルトレース*に変換します：

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

# Wrap the provider — every Provider.complete() now emits an LLM-kind span.
wrapped = OpenInferenceProvider(real_provider, tracer)
ctx = ng.NodeContext(provider=wrapped)
engine = ng.GraphEngine.compile(graph_def, ctx)

with openinference_tracer(tracer) as cb:
    engine.run_stream(ng.RunConfig(input={"messages": [...]}), cb)
```

Phoenixを一度起動します：`docker run -d -p 6006:6006 -p 4317:4317
arizephoenix/phoenix`。http://localhost:6006を開くと、トレースがチェーン（`graph.run` → `node.X` → `llm.complete`）としてレンダリングされ、プロンプト / レスポンス / トークン数がLLM詳細ペインに表示されます。同じコードで、OTLPエンドポイントURLをLangfuseセルフホストに切り替えると、トレースは同じ形状でそこに表示されます。

これは *「NeoGraphにはLangSmithがない」* に対する答えです — PhoenixまたはLangfuseを1つのDockerコマンドでローカルに実行することで、LangSmith UX（チャットバブル、DAG階層、トークンコスト）を取得できます。SaaS契約も、トレースごとの価格設定もありません。

`docs/reference-en.md` §10.5で、属性キースキーマと`otel_tracer`、`openinference_tracer`間のトレードオフを確認してください。

---

<a id="9-common-pitfalls"></a>
## 9.よくある落とし穴

これらはすべて実際のユーザーが遭遇したものです。[`docs/troubleshooting.md`](troubleshooting.md)から相互参照されています。

### 「私のReActループが一度だけしか実行されない」

あなたはwheel ≤ 0.1.7を使用しています。グラフコンパイラが`conditional_edges`ブロックを静かに削除しました。≥ 0.1.8にアップグレードしてください。`result.execution_trace == ['llm', 'dispatch', 'llm']`で検証してください（`['llm']`だけではありません）。

### 「プロバイダーコールが60秒間ハングし、それからエラーになる」

あなたはwheel ≤ 0.1.6を使用しています。バンドルされたOpenSSLは、Ubuntu / Debian / macOSには存在しないRHEL CAパスをハードコードしています。≥ 0.1.7にアップグレードするか（インポート時に`SSL_CERT_FILE`をcertifiのバンドルに自動設定）、`SSL_CERT_FILE`を手動で設定してください。

### 私のfan-outは期待していたよりも遅いです

`compile()` デフォルトは `set_worker_count(1)` （エンジン所有のスレッドプールなし — fan-out ブランチは呼び出し元のエグゼキュータ上で直列に実行される）。実際の並列処理には `engine.set_worker_count(N)` を呼び出し、N を Send の fan-out 幅に合わせるか、 `engine.set_worker_count_auto()` を `hardware_concurrency()`に使用する。NeoGraph はまた、オプトインしたプールなしでマルチ Send の fan-out が初めて実行されたときに、一度だけ stderr 警告を出力する — これはヒントであり、エラーではない。Python カスタムノードは小さな fan-out で GIL の競合が発生するため、1 と N の両方でベンチマークを行うこと。

### "Python RunResult に .status / .final_state 属性がない"

Pythonバインディングはそれらの属性を公開していません。`result.output`、`result.interrupted`、`result.max_steps_exhausted`、`result.execution_trace`を使用してください。C++呼び出し元は、型付きの`Completed` / `Interrupted` / `StepLimit`ビューに`RunResult::status()`を使用できます。[Pythonバインディングガイド](python-binding.md#hitl-and-state)を参照してください。

### 「不明なリデューサー：<name>」

`overwrite`と`append`の2つのリデューサーが同梱されています。コンパイル前に、C++では`ReducerRegistry::register_reducer`、Pythonでは`ng.ReducerRegistry.register_reducer`を使用してカスタムリデューサーを登録してください。

### "条件が登録されているのに、条件付きエッジが発火しない"

フォームがローダーが受け入れるもの（[§4](#4-edges--conditional-routing)のフォームAまたはフォームB）であることを確認してください — 両方ともv0.1.8以降で動作します。古いwheelでは、フォームBのみが動作します。

### "execution_trace が開始ノードのみを表示する"

ルーティングが`__end__`にフォールスルーしました。最も可能性が高いのは、開始ノードからのエッジが欠落しているか、条件が`routes`マップにない値を返し、明示的な`"default"`ルートが`__end__`を指していることです。厳密なグラフでは、マップの順序によってルートを選択しなくなりました。オープンまたは未指定の条件は、宣言されている場合は`"default"`を使用し、それ以外の場合はエンジンがソースノード、条件、および返されたラベルとともに例外をスローします。クローズド条件は、宣言されたラベルの外側を返す場合、常に例外をスローします。

---

## 次のステップ

- [Pythonの例](../bindings/python/examples/) — 上記のすべての概念を網羅する21の自己完結型スクリプト。
- [C++の例](../examples/) — 同じ構造を持つ36個のプログラム。
- [`reference-en.md`](reference-en.md) — クラスごとの網羅的なAPI。
- [`ASYNC_GUIDE.md`](ASYNC_GUIDE.md) — 非同期/コルーチンレイヤーに関する詳細な解説。
