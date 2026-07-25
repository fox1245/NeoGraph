<!-- neograph-i18n: source=docs/concepts.md locale=ja source_sha256=a7d9bae682dca57211d6c7ad0795977dc811bd5123934d73e9187a13e89e25f1 -->
# NeoGraph のコアコンセプト — ナラティブガイド

**Languages:** [English](concepts.md) | [한국어](concepts.ko.md) | [日本語](concepts.ja.md) | [简体中文](concepts.zh-CN.md)

例に入る前に、これを一度読んでください。それは、
メンタル モデルを自分で構築する順序で作成します: グラフ →
チャネル → ノード → エッジ → ファンアウト → ルーティング オーバーライド →
チェックポイント→ストリーミング。

コードサンプルは簡潔であるため、Python 側です。すべての地図
C++ API に対して 1:1 (クラスについては [`reference-en.md`](reference-en.md) を参照)
署名または [Doxygen](https://fox1245.github.io/NeoGraph/)
生成された参照)。

> **以前に LangGraph を使用したことがある場合:** プリミティブは意図的に
> 同じ — リデューサーを備えたチャネル、書き込みを発行するノード、条件付き
> エッジ、`Send`、`Command`、チェックポイント。違いについては、次のとおりです。
> [Comparison with LangGraph](../README.md#vs-langgraph)
> README。以下の説明は何も仮定していません。

---

## 目次

(セクション 8.5 は v0.6.0 で追加 — `Tracing — OpenTelemetry + Phoenix / Langfuse`。
外部ドキュメントのリンクを安定させるため、番号付きの見出しは 1 ～ 9 のままです。
8.5 はストリーミングと一般的な落とし穴の間に位置します。)


1. [The big picture](#1-the-big-picture)
2. [Channels & reducers](#2-channels--reducers)
3. [Nodes](#3-nodes)
4. [Edges & conditional routing](#4-edges--conditional-routing)
5. [Send — dynamic fan-out](#5-send--dynamic-fan-out)
6. [Command — routing override + state patch](#6-command--routing-override--state-patch)
7. [Checkpoints, interrupts, HITL](#7-checkpoints-interrupts-hitl)
8. [Streaming events](#8-streaming-events)
9. [Common pitfalls](#9-common-pitfalls)

---

<a id="1-the-big-picture"></a>
## 1. 全体像

NeoGraph **グラフ** には次の 4 つの要素があります。

|もの |それは何ですか |定義元 |
|---|---|---|
| **チャンネル** |共有状態の名前付きスロット。それぞれに、新しい書き込みを既存の値と組み合わせる方法を定義するリデューサーがあります。 | `definition["channels"]` |
| **ノード** |状態を読み取り、書き込みを発行する関数 (およびオプションで `Send` / `Command`)。 | `definition["nodes"]` |
| **エッジ** |静的な次ノード ポインター。 | `definition["edges"]` |
| **条件付きエッジ** |述語主導のルーティング — 状態に基づいて、いくつかの次のノードの 1 つを選択します。 | `definition["conditional_edges"]` |

実行は **スーパーステップ ループ** です。

```
1. ready_set = nodes routed from __start__
2. while ready_set is not empty:
   a. run all nodes in ready_set (in parallel if the executor allows)
   b. apply each node's writes to state
   c. collect their Send / Command / outgoing-edge signals
   d. plan_next_step → new ready_set
```

スーパー ステップは、並列処理、チェックポイント処理、および
ストリーミングイベント。どちらも「現在」実行できる 2 つのノードは同じです
スーパーステップ。彼らは同じ入力状態と書き込みを観察します。
ステップ終了時にリデューサーを介して結合します。

---

<a id="2-channels--reducers"></a>
## 2. チャネルとリデューサー

すべての状態は名前付きチャネルに存在します。チャネルは複数にわたって持続します
ノードとスーパーステップ間。ノードは書き込みによって通信します。

### チャネルの定義

```python
"channels": {
    "messages":  {"reducer": "append"},     # conversation history
    "counter":   {"reducer": "overwrite"},  # latest value wins
    "summary":   {"reducer": "overwrite"},
}
```

### 減速機内蔵

|減速機 |新しい書き込みセマンティクス |一般的な使用法 |
|---|---|---|
| `"overwrite"` |新しい値が古い値に置き換わります。並列書き込みでは最後の書き込み者が優先されます。 |単一値のスクラッチ (現在のノード、現在の質問、ルート ヒント)。 |
| `"append"` |新しいリスト (リストである必要があります) は既存のリストに連結されます。順序: 前のステップの値が最初に、このステップの書き込みがノードの実行順序で追加されます。 |会話メッセージ、検索結果、ファンアウトコレクション。 |

> 両方の減速機は `ReducerRegistry::ReducerRegistry()` に登録されています
> エンジン始動時（[`src/core/graph_loader.cpp`](../src/core/graph_loader.cpp)）。
> カスタム リデューサーは `ReducerRegistry::register_reducer(name, fn)` 経由で C++ から登録します
> または Python から (v0.1.9 以降):
>
> ```python
> ng.ReducerRegistry.register_reducer("sum",
>     lambda current, incoming: (current or 0) + incoming)
> ```
>
> Python 呼び出し可能ファイルは GIL の下で実行されます。同時送信ファンアウト
> Python カスタム ノードと同じ方法でシリアル化します。再登録する
> 以前のリデューサーは名前に置き換えられます。

### チャネルへの書き込み

ノードは `ChannelWrite` のリストを返します。

```python
return [
    ng.ChannelWrite("messages", [{"role": "assistant", "content": "Hi!"}]),
    ng.ChannelWrite("counter",  state.get("counter", 0) + 1),
]
```

値の形状はリデューサーと一致する必要があります。
- `"append"` → リストである必要があります (連結されます)。
- `"overwrite"` → 任意の JSON シリアル化可能な値。

### ノードから状態を読み取る

```python
def run(self, input):
    msgs    = input.state.get("messages") or []  # list of message dicts
    counter = input.state.get("counter") or 0
    ...
```

`state.get(channel)` はチャネルの現在値を返します。または、次の場合は `None` を返します。
チャネルは存在しますが、まだ書き込まれていません。入力されたアクセスの場合
チャット メッセージ、`state.get_messages()` は `list[ChatMessage]` を返します
(`messages` チャネルから解析) — `llm_call` によって内部的に使用されます。

### バージョン

各チャネルは単調な `version` 番号を伝送します。エンジンが使用するのは、
これはチェックポイントの差分と `state.channel_version(name)` 用です
検査API。通常は直接読むことはありません。

---

<a id="3-nodes"></a>
## 3. ノード

ノード タイプを登録する 3 つの方法 (制御の昇順):

### 3.1 組み込みノード

| `type` (JSON 形式) |何をするのか |構成 |
|---|---|---|
| `llm_call` | `provider->complete_async(messages, tools)` を呼び出し、アシスタント メッセージを `messages` に追加します。 | `NodeContext` から `provider`、`model`、`instructions`、`tools` を読み取ります。 |
| `tool_dispatch` |最新のアシスタント メッセージの `tool_calls` を確認し、`Tool::execute` 経由でそれぞれを実行し、`{role: "tool", tool_call_id, content}` の結果を追加します。 | `NodeContext` から `tools` を読み取ります。 |
| `intent_classifier` | LLM はユーザーの意図を N 個のラベルの 1 つに分類し、選択したラベルを `__route__` に書き込みます。 `route_channel` 条件付きでペアリングします。 | |
| `subgraph` |別のグラフを単一のノードとして埋め込みます。内部状態は、構成されたキーの再マッピングを通じてマッピングされます。 | `extra_config: {graph_def, input_keys, output_keys}` |

### 3.2 `@ng.node` デコレーター (Python のみ)

書き込み専用ノードを定義する最短の方法:

```python
@ng.node("greet")
def greet_node(state):
    name = state.get("name") or "world"
    return [ng.ChannelWrite("messages",
        [{"role": "assistant", "content": f"Hello, {name}!"}])]
```

装飾された関数は `list[ChannelWrite]` (または `None`、
`[]`として扱われます)。 `Send` または `Command` を発行することはできません。
サブクラス `GraphNode`。

### 3.3 完全な `GraphNode` サブクラス

完全に制御するには、`run(input)` をオーバーライドします。これは v0.4.0 で導入され、
v0.9.0 以降の唯一のカスタム ノード エントリ ポイント — 1 つのメソッド、1 つの
サイン：

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

Python は `input.ctx` で `cancel_token`、`thread_id`、`step`、`stream_mode`、
`store`、`resume_value` を公開します。C++ 呼び出し元は `RunMetadata` に
`deadline` と `trace_id` を設定でき、エンジンはそれらをネストした subgraph
まで伝播します。この 2 フィールドはまだ Python バインディングには公開されません。

必要ない場合は、裸の `list[ChannelWrite]` を返却することもできます。
`Send` または `Command` — バインディングにより `NodeResult` に持ち上げられます。
自動的に。

> **v0.3.x からの移行:** 8 仮想チェーン (`execute`、
> `execute_async`、`execute_full`、`execute_full_async`、
> `execute_stream`、`execute_stream_async`、`execute_full_stream`、
> `execute_full_stream_async`) は v0.4.x で非推奨となり、 で削除されました。
> v1 準備中の v0.9.0。シングルに交換します
> `run(input)` オーバーライド。 `input.state` から状態を読み取り、トークンを発行します
> None 以外の場合は、`input.stream_cb` 経由でキャンセル トークンを読み取ります。
> `input.ctx.cancel_token`。

JSON ローダーがインスタンス化できるように型を登録します。

```python
ng.NodeFactory.register_type(
    "researcher",
    lambda name, config, ctx: Researcher(name),
)
```

工場では`(name, per-node config, NodeContext)`を認識しているので同じです
クラスは、異なる構成を使用して複数の名前でインスタンス化できます。

### 3.4 ツール (別の概念、`tool_dispatch` で使用)

`Tool` はノードではありません。`tool_dispatch` が呼び出すものです。サブクラス
`ng.Tool`、3 つのメソッドをオーバーライドし、インスタンスをに渡します。
`NodeContext(tools=[…])`:

```python
class CalcTool(ng.Tool):
    def get_name(self):       return "calc"
    def get_definition(self): return ng.ChatTool(name="calc", ...)
    def execute(self, args):  return str(args["x"] * 2)
```

エンジンはコンパイル時にツール リストの所有権を取得します。
ローカル参照は後で削除される可能性があります。

---

<a id="4-edges--conditional-routing"></a>
## 4. エッジと条件付きルーティング

### 静的エッジ

```python
"edges": [
    {"from": ng.START_NODE, "to": "llm"},
    {"from": "dispatch",    "to": "llm"},
    {"from": "summarizer",  "to": ng.END_NODE},
]
```

同じ送信元ノードからの複数のエッジがファンアウトします (すべての後続ノードが停止します)
次のスーパーステップの準備完了セットに組み込まれます)。同じターゲットへの 2 つのエッジ
1 回のスーパーステップ重複排除から 1 回のターゲット実行まで。

### 条件付きエッジ

条件付きエッジは **名前付き条件** を実行し、次のノードを選択します
`routes` マップから:

```python
"conditional_edges": [
    {
        "from": "llm",
        "condition": "has_tool_calls",
        "routes": {"true": "dispatch", "false": ng.END_NODE},
    }
]
```

条件名は、
エンジン。 2 つは組み込みとして出荷されます。

|状態 |返品 |いつ使用するか |
|---|---|---|
| `has_tool_calls` |最新のアシスタント メッセージに空ではない `tool_calls` がある場合は `"true"`。それ以外の場合は`"false"`。 | ReAct ループ — LLM が要求を停止するまでツールをディスパッチし続けます。 |
| `route_channel` | `__route__` チャネルにある文字列は何でも。 `"default"` に戻ります。 |明示的インテント ルーティングには、`intent_classifier` と組み合わせます。 |

`ConditionRegistry::register_condition(name, fn)` 経由で C++ からカスタム条件を登録
または Python から (v0.1.9 以降):

```python
def is_long(state):
    msgs = state.get("messages") or []
    return "long" if len(msgs) > 10 else "short"

ng.ConditionRegistry.register_condition("is_long", is_long)
```

呼び出し可能関数はライブ `GraphState` を受信します (つまり、`state.get(channel)` と
`state.get_messages()` は機能します)、次のいずれかに一致する文字列を返す必要があります。
条件付きエッジの `routes` キー。

### 2 つの同等の形式 - どちらも v0.1.8 以降で動作します

条件付きエッジは、`edges` 配列内に存在する可能性があります (
`condition` フィールド) **または** 別の `conditional_edges` ブロック。
どちらの形式も受け入れられます。より明確なものを選択してください:

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

> **履歴:** フォーム A は、以前にグラフ コンパイラによってサイレントに削除されました。
> v0.1.8 — README とすべての Python サンプルで使用されているため、ReAct ループが発生します
> 単一の LLM 呼び出しに縮退します。コミット`e23a523`で修正されました。もしあなたが
> ホイール ≤ 0.1.7 の場合は、アップグレードしてください。

---

<a id="5-send--dynamic-fan-out"></a>
## 5. 送信 — 動的ファンアウト

`Send` は、次のステップのノードの数が依存する場合に使用します。
州。従来の使用法: 検索トピックのリストを N 並列に分割します。
研究者の呼び出し。

```python
class Planner(ng.GraphNode):
    def execute_full(self, state):
        topics = decide_topics(state)                  # e.g. 5 strings
        return ng.NodeResult(
            writes=[],
            sends=[ng.Send("researcher", {"topic": t}) for t in topics],
        )
```

エンジンの `run_sends_async` は、ごとに 1 回 `researcher` をインスタンス化します。
`Send`、それぞれに独自の `state.get("topic")` があり、それらを実行します
`asio::experimental::make_parallel_group`経由でパラレル。

### メンタルモデル

`Send(target, payload)` は「この状態で `target` をインスタンス化します」
パッチを適用してレディセットに追加します。ペイロードは
ターゲットが `state` を認識する前に状態を書き込みます。

並列グループが終了すると、次のスーパーステップのルーティングが始まります。
Send で生成された各タスクの発信エッジ (または `Command.goto`、
発した場合）。

### 一般的な形状: ファンアウト 5、サマライザーへのファンイン

```
planner ─┬─ Send("researcher", {topic: "A"})  ─┐
         ├─ Send("researcher", {topic: "B"})  ─┤
         ├─ Send("researcher", {topic: "C"})  ─┼─→ summarizer
         ├─ Send("researcher", {topic: "D"})  ─┤
         └─ Send("researcher", {topic: "E"})  ─┘
```

`researcher` の発信エッジはちょうど `{"from": "researcher", "to": "summarizer"}` です
— 静的エッジと同じ重複排除ルールなので、サマライザーは 1 回実行されます。

### ワーカー数の調整

`build()` のデフォルトは `EngineConfig::worker_count == 1` — エンジン所有のスレッドなし
プール、ファンアウト ブランチはコルーチン自体でインラインでディスパッチされます
執行者。これは、シーケンシャルに安価な割り当てなしの高速パスです。
非スレッドセーフ状態を保持するノードに対しても安全です。

実際の並列処理を行うには、プールを明示的に選択します。正確に N を選択してください
ファンアウト幅に一致させるか、`set_worker_count_auto()` を使用してください。
`hardware_concurrency()` (フォールバック 4 を使用):

```python
engine.set_worker_count(5)           # match a 5-way Send
# or
engine.set_worker_count_auto()       # hardware_concurrency()
```

マルチ送信 (またはマルチ発信エッジ) ファンアウトが、
オプトインされたプールでは、NeoGraph はワンショットの標準エラー出力警告を発行するため、
サイレントシリアルのケースは目立たない。で抑制する
意図的に運転する場合は`NEOGRAPH_SUPPRESS_FANOUT_WARNING=1`
シリアル ファンアウト (たとえば、worker=1 高速パスのベンチマーク)。

---

<a id="6-command--routing-override--state-patch"></a>
## 6. コマンド — ルーティングオーバーライド + ステートパッチ

`Command` は、ノードが次にどこに行くかを決定し、その状態で状態を変化させます。
同じ戻り値。通常の発信エッジをバイパスします。

```python
class Evaluator(ng.GraphNode):
    def execute_full(self, state):
        if score(state) >= 0.8:
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
                    updates=[ng.ChannelWrite("retries",  state.get("retries", 0) + 1)],
                ),
            )
```

### コマンドと条件付きエッジをいつ使用するか

- **条件付きエッジ**: ルーティングは状態述語に依存します。
  ノードロジックは必要ありません。よりクリーンで宣言的。
- **コマンド**: ルーティングは、作成するのが最も自然なロジックに依存します。
  ノード内 - 複数基準のスコアリング、コンテンツ検査、再試行
  決断。また、状態をアトミックに更新し、選択する唯一の方法です
  次のノード。

### ファンインで最後のライターが勝利

複数のコマンドが同じスーパーステップで起動された場合 (まれですが、のみ)
複数の並列グループの兄弟がそれらを発行する場合に可能)、最後の
一人が勝ちます。順序は並列グループの補完によって決定されます。
は非決定的です — 多くても 1 つを確保することで、これを考慮した設計を行ってください。
兄弟は `Command` を発行します。

---

<a id="7-checkpoints-interrupts-hitl"></a>
## 7. チェックポイント、割り込み、HITL

### チェックポイント ストアのセットアップ

```python
engine.set_checkpoint_store(ng.InMemoryCheckpointStore())
# or: engine.set_checkpoint_store(ng.PostgresCheckpointStore(...))   # if built with PG
```

ストアが接続されている場合、すべてのスーパーステップはチェックポイントを
`(thread_id, checkpoint_id)` にキーを付けて保存します。 `RunResult.checkpoint_id`
フィールドは最新のものです。

### 静的割り込みポイント

```python
"interrupt_before": ["payment"],   # pause before this node runs
"interrupt_after":  ["llm"],       # pause after, before routing
```

エンジンは `interrupted=True` を含む `RunResult` を返します。
`interrupt_node`セット。再開するには:

```python
result = await engine.resume_async(thread_id="t1",
                                   checkpoint_id=result.checkpoint_id,
                                   new_input={...})  # optional
```

### `NodeInterrupt` による動的割り込み

ノード本体内からスローする (Python: `raise ng.NodeInterrupt(reason)`、
C++: `throw NodeInterrupt(...)`)。エンジンが状態をキャッチし、維持し、
スローノードで中断された`RunResult`を返します - 同じ
APIを再開します。

一時停止の決定が中間ノードの出力に依存する場合に便利です
(例: 「LLM は人間に見せる価値のあるものを生み出しましたか?」)。

### タイムトラベル

`engine.fork(thread_id, from_checkpoint_id)` は新しいスレッドを返します。
過去のチェックポイントからスタートします。 「もし私が答えていたらどうなるか」に便利
違う」分岐。

---

<a id="8-streaming-events"></a>
## 8. ストリーミングイベント

`run_stream` / `run_stream_async` は、イベントの発生時にコールバックを呼び出します。
モードは OR 可能なビットマスクです。

|モード |排出 |
|---|---|
| `EVENTS` | `NODE_START`、`NODE_END`、`INTERRUPT` |
| `TOKENS` | `Provider` からのストリーミングされたトークンごとに `LLM_TOKEN` |
| `DEBUG` |次に使用できるセットを示す `__routing__` イベント |
| `VALUES` |各スーパーステップ後の完全な状態の `__state__` イベント |
| `UPDATES` | `ChannelWrite` ごとの `CHANNEL_WRITE` イベント |
| `ALL` |上記のすべて |

```python
def cb(event):
    print(event.type, event.node_name, event.data)

engine.run_stream(
    ng.RunConfig(thread_id="t", input={...},
                 stream_mode=ng.StreamMode.EVENTS),
    cb)
```

> **注:** `event.node_name` (`event.node` ではありません)。 C++ 構造体フィールド
> `node_name`です。 pybind は元の名前を保持します。

チャット形式のストリーミングの場合 (LangChain 互換のメッセージ辞書
インクリメンタル `content_so_far`)、ヘルパーを使用します。

```python
from neograph_engine import message_stream

engine.run_stream(
    ng.RunConfig(thread_id="t", input={...},
                 stream_mode=ng.StreamMode.TOKENS),
    message_stream(lambda chunk: print(chunk["content"], end="", flush=True)))
```

### `asio::io_context.run()` 配置 (C++)

C++ から `engine.run_stream_async()` を駆動する場合、外側の
`asio::io_context.run()` はアプリケーションのメインから呼び出す必要があります
スレッド (または、
通常のプロセスの起動パス)。テスト済みの良好な形状:

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

> **既知の制限 - HTTP サーバー ワーカー内のネストされた `io.run()`
> callback** (問題 #16): `asio::io_context.run()` を内部にネストする
> `httplib::Server::set_chunked_content_provider` (または同等のもの)
> リクエストごとのワーカー コールバック自体が子スレッドを生成します。
> `Provider::complete_stream_async` のデフォルトブリッジ) が観察されました
> 一部の glibc / OpenSSL の組み合わせでは、`getaddrinfo` の SEGV に変換されます。の
> ツリー内テスト
> ([`tests/test_schema_provider_stream_async_nested_thread.cpp`](../tests/test_schema_provider_stream_async_nested_thread.cpp))
> 構造形状をカバーしてきれいに通過しますが、下流側
> 環境 (HTTPS 経由の実際の `api.openai.com`、glibc リゾルバー)
> TSan / ASan では、同時リクエスト負荷) は完全ではありません
> テストスイートから再現可能。 **回避策:**
>
> 1. **代わりに `co_await provider->complete_async(...)` を使用します。
>    HTTP サーバー コールバック** 内からの `complete_stream_async`、および
>    組み立てられた応答を 1 つの `LLM_TOKEN` イベントとして
>    ヘルパー。トークンタイプの UX は失われます。エンジン + ノード + ツールループの作業
>    端から端まで。これは ProjectDatePop のダウンストリーム `cpp_backend` です
>    今日は使います。
> 2. **`io.run()` をリクエストごとのコールバックから移動**: 1 つを実行します。
>    専用のワーカー スレッド上の長命 `asio::io_context`
>    エンジン、リクエストごとの作業をエンジンにキューイングし、結果をポストバックします。
>    HTTP サーバーの応答シンクに送信されます。リクエストごとの回避
>    SEGV が相関するネストされた `std::thread` スポーン。

---

## 8.5。トレース — OpenTelemetry + Phoenix / Langfuse

ストリーミングと同じコールバック形状、異なるコンシューマ。 OTelを通過
`engine.run_stream(cfg, cb)` およびあらゆるものへのトレーサー発行コールバック
`NODE_START` / `NODE_END` / `ERROR` / `INTERRUPT` イベントは
スパン。

2 つのレイヤーがツリー内で出荷されます。

  - `neograph_engine.tracing.otel_tracer` — ベンダー中立の OTel
    スパン。スパンは任意の OTel バックエンド (Jaeger、Tempo、Honeycomb、
    データドッグ）。
  - `neograph_engine.openinference` — LLM 形状の属性レイヤー
    同じスパンを *LangSmith スタイルのチャットバブルに変える
    フェニックス / アライズ / ラングフューズのトレース*:

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

Phoenix を 1 回スピンアップします: `docker run -d -p 6006:6006 -p 4317:4317
アライズフェニックス/フェニックス`。 http://localhost:6006 を開く — トレース
チェーン (`graph.run` → `node.X` → `llm.complete`) としてレンダリングします。
プロンプト/応答/トークン数が LLM 詳細ペインに表示されます。
同じコードで、OTLP エンドポイント URL を Langfuse セルフホストに交換し、
トレースは同じ形状でそこに表示されます。

これは *「NeoGraph には LangSmith がありません」* に対する答えです — あなた
LangSmith UX (チャットバブル、DAG 階層、トークンコスト) を取得します。
1 つの Docker コマンドで Phoenix または Langfuse をローカルで実行します。いいえ
SaaS 契約。トレースごとの料金設定はありません。

属性キーのスキーマについては、`docs/reference-en.md` §10.5 を参照してください。
`otel_tracer` と `openinference_tracer` の間のトレードオフのメモ。

---

<a id="9-common-pitfalls"></a>
## 9. よくある落とし穴

これらはすべて実際のユーザーによってヒットされました。相互参照元
[`docs/troubleshooting.md`](troubleshooting.md)。

### 「私の ReAct ループは 1 回しか実行されません」

ホイール ≤ 0.1.7 を使用しています。グラフ コンパイラは、
`conditional_edges`は黙ってブロックします。 0.1.8 以上にアップグレードしてください。で確認してください
`result.execution_trace == ['llm', 'dispatch', 'llm']` (だけではありません)
`['llm']`）。

### 「プロバイダーの呼び出しが 60 秒間ハングし、その後エラーが発生します」

ホイール ≤ 0.1.6 を使用しています。バンドルされた OpenSSL ハードコード RHEL CA パス
Ubuntu / Debian / macOS には存在しません。 0.1.7 以上にアップグレードしてください
(インポート時に `SSL_CERT_FILE` を証明書のバンドルに自動設定します) または設定
手動で`SSL_CERT_FILE`。

### 「ファンアウトが予想よりも遅い」

`compile()` のデフォルトは `set_worker_count(1)` (エンジン所有のスレッドなし)
プール - ファンアウト ブランチは呼び出し元のエグゼキュータ上でシリアルに実行されます)。のために
N が一致する実並列処理呼び出し `engine.set_worker_count(N)`
送信ファンアウト幅、または `engine.set_worker_count_auto()`
`hardware_concurrency()`。 NeoGraph はワンショットの標準エラー出力も出力します
オプトインなしでマルチ送信ファンアウトを初めて実行するときに警告する
プール — これはエラーではなくヒントです。 Python カスタム ノードについては、「GIL」を参照してください。
小さなファンアウトで競合が発生するため、1 と N の両方でベンチを設定します。

### 「Python RunResult には .status / .final_state 属性がありません」

Python バインディングはこれらの属性を公開しません。 `result.output`を使用し、
`result.interrupted`、`result.max_steps_exhausted`、および
`result.execution_trace`。 C++ 呼び出し元は `RunResult::status()` を使用して、
`Completed` / `Interrupted` / `StepLimit` ビューを入力しました。の表を参照してください。
README の「出力の読み方」セクション。

### 「不明な減速機: <name>」

`overwrite` と `append` の 2 つの減速機が同梱されています。カスタム減速機に必要なもの
C++ の `ReducerRegistry::register_reducer` (Python フックはまだありません)。

### 「条件は登録されていますが、条件付きエッジが起動しません」

フォームがローダーが受け入れるフォームであることを確認します (フォーム A またはフォーム B
[§4](#4-edges--conditional-routing)) — どちらも v0.1.8 以降で動作します。の上
古いホイールではフォーム B のみが機能します。

### 「execution_trace は開始ノードのみを表示します」

ルーティングは `__end__` に失敗しました。おそらくエッジが欠落している可能性があります
開始ノード、または条件式がフィールドにない値を返しました。
`routes` マップ (この場合、エンジンは
辞書順に最後のルートをフォールバックとして使用する — 意外な要因)。

---

## 次はどこへ

- [Python examples](../bindings/python/examples/) — 21 自己完結型
  上記のすべての概念をカバーするスクリプト。
- [C++ examples](../examples/) — 同じ構造を持つ 36 個のプログラム。
- [`reference-en.md`](reference-en.md) — 網羅的なクラスごとの API。
- [Doxygen](https://fox1245.github.io/NeoGraph/) — 生成された参照
  C++ ヘッダーの場合。
- [`ASYNC_GUIDE.md`](ASYNC_GUIDE.md) — 非同期 / コルーチンの詳細
  層。
