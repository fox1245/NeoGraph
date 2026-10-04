<!-- neograph-i18n: source=docs/concepts.md locale=ja source_sha256=0f718bca31f68497ef00b56cb3dd01cd534f53f3dfd2a42741524fae51a18a36 -->
# NeoGraphのコアコンセプト— 解説ガイド

**Languages:** [English](concepts.md) | [한국어](concepts.ko.md) | [日本語](concepts.ja.md) | [简体中文](concepts.zh-CN.md)

例の前にこの文書を読んでください。グラフを作る順序に沿って、チャネル、ノード、エッジ、fan-out、経路、チェックポイント、ストリーミングを説明します。

LangGraph を使ったことがあれば、reducer 付きチャネル、`Send`、`Command`、チェックポイントは馴染みのあるものです。NeoGraph の [Core と ProgramRuntime](../README.md#core-and-programruntime) は役割が異なり、このガイドは Core のグラフ実行から始めます。Python provider 呼び出しには削除済み completion クラスではなく typed [binding 契約](python-binding.md)を使います。

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
   a. run the ready batch against its pre-update channel state
   b. buffer returned writes, then fold them through channel reducers
   c. execute emitted Sends after ordinary writes; fold their results
   d. combine routing signals and evaluate updated state → new ready_set
```

通常の ready batch はその batch の update 前のチャネル状態を読みます。実行中の兄弟ノードは、他の兄弟が返した書き込みを読めません。エンジンは書き込みを buffer に集め、batch 後に reducer で結合し、更新済み状態で経路を評価します。これはグラフのスケジューリングであり、モデル内部の計算を同期するものではありません。

例えば `counter` が 0 で二つの ready ノードがそれぞれ `counter + 1` を返すと、両方が 0 を読みます。overwrite reducer の結果は 2 ではなく 1 です。カスタム sum reducer に増分 1 をそれぞれ書けば 2 に結合できます。複数分岐の `Send` は各 payload を適用した隔離状態コピーを使い、単一 `Send` は共有状態に payload を適用します。Reducer の順序だけではモデル応答や外部効果の再現性は得られません。

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

### チャネル lifecycle と checkpoint 契約

Reducer は書き込みを結合します。配列 retention は別の政策で、`unbounded`（既定）、`latest`、正の `retention_limit` を持つ `bounded` を選びます。Retention は `ChannelWrite.Mode.Overwrite` を含む各書き込み後に配列を切り詰めます。`latest` は次の書き込みまで最後の要素を保持します。Persistence は独立に `checkpoint`（既定、materialized 値と version）または `ephemeral`（両方を永続 checkpoint から省略）を選びます。Bounded retention は保存量だけでなく観測できる履歴を変えます。

エンジンはノードの返した書き込み順、static batch の scheduler-ready 順、複数 `Send` の呼び出し順で結合し、完了順は使いません。Pending write は同じ task slot に再生します。Overwrite は順序付き last-writer-wins、append は要素順を保持します。カスタム reducer は replay で純粋かつ安定しているべきです。Regrouping で結果を保つには結合律、順序独立には交換律が必要です。明示的 overwrite は reducer を迂回してから retention を適用します。これらの規則はモデル応答や外部効果の再現性を保証しません。

Ephemeral 値は superstep 間でも生き続け、各 step で reset しません。Checkpoint は宣言された ephemeral 名と書き込み済みかを記録しますが値は保存しません。Resume、`resume_if_exists`、exact-ID resume、state update は書き込み済み ephemeral 状態、旧 checkpoint の guard 欠落、ephemeral チャネル集合の変更を拒否します。最初の ephemeral 書き込み前の checkpoint は文書化された順に pending write を再生して resume できます。`update_state` は ephemeral 書き込みを拒否します。複数 `Send` の in-process worker は隔離コピー内に live ephemeral 値を継承します。正しさに必要な状態は checkpoint に残すか、新 run で永続入力から再構成してください。

`GraphState::restore` は ephemeral チャネルのあるグラフを拒否します。一致する guard と `restore_checkpoint` を使うか、全 live 値と version を含む同一 process snapshot に `restore_runtime` を使います。Guard は checkpoint metadata を使い、channel blob layout や store schema を変えません。Ephemeral チャネルのないグラフの旧 full-value checkpoint は引き続き動作します。Guard のない binary に downgrade する前に、ephemeral thread と fork を drain するか永続入力から再開始してください。旧 reader は追加 guard を強制できません。

Checkpoint チャネルは full materialized snapshot を使います。Memory、SQLite、PostgreSQL は不変の `(thread, channel, version)` 値を重複排除しますが、append 履歴は書き込みごとに version が変わり snapshot も増えます。Pending write は未完了 superstep の成功 task を記録し、一般 channel delta ではありません。Per-step reset 政策は提供しません。安全な設計には書き込み・経路決定後の reset、interrupt、replay、Send の定義が必要で、ephemeral persistence と混同してはいけません。

Delta-backed checkpoint は設計でありチャネル設定ではありません。この形式は full snapshot から順序付き `{channel, version, write mode, value}` delta を最大 *K* 個（任意の byte 閾値）再生するものです。Overwrite、retention、version、reducer identity を保ち、pending write の削除前に snapshot/delta と checkpoint pointer を原子的 publish する必要があります。欠落 link、version gap、未知 reducer、replay 失敗を拒否しなければなりません。採用には新 schema version と測定した利益が必要です。既存 snapshot は架空 delta なしで base に移し、可逆 rollout では old-reader full snapshot を維持します。Delta-only record があれば元 reducer registry で materialize しない限り downgrade を拒否します。現在の store は full-snapshot 方式です。

Baseline は `bench_checkpoint_store --threads 1 --iters 1 --history-steps 256 --payload 512 --backends memory,sqlite` で測ります。隔離 local DB のみに `postgres`、`--pg-url` を加えます。行は logical serialized byte、save/load p50/p95、reconstruction depth を報告し、legacy 行は blob count を報告します。Allocation request には `heaptrack bench_checkpoint_store --threads 1 --iters 1 --history-steps 256 --payload 512 --backends memory` を使います。Native JSON/SQL allocator は C++ `operator new` ですべて測定できません。同じ payload、history、backend で測定値を比較します。Logical byte と durable physical byte は異なります。SQLite は終了時に削除する固有 temporary DB を使い、`--sqlite-path` は新 file を残して既存 path を拒否します。

記録された Linux x86-64 Debug baseline は thread 1、history step 256、512-byte message、iteration 1 でした。歴史的測定であり性能目標ではありません:

| Backend | Logical checkpoint bytes | Save p50/p95 (µs) | Load p50/p95 (µs) | Replay depth |
| --- | ---: | ---: | ---: | ---: |
| Memory | 17,814,952 | 54 / 138 | 141 / 382 | 1 |
| SQLite | 17,814,952 | 289 / 1,589 | 176 / 474 | 1 |

削除前の別 repeat で SQLite DB/WAL は 14,811,136 / 4,210,672 byte（合計 19,021,808）でした。構築・JSON parse を含む process-wide `malloc`、`calloc`、非ゼロ `realloc` request を数えた Linux `LD_PRELOAD` shim は `--history-steps 0` に対して Memory 追加 88,277 request / 605,289,027 requested byte、SQLite 114,295 / 867,319,964 を測りました。これは累積 request で、live memory や store のみの allocation ではありません。Aligned/internal allocation は捕捉しませんでした。Shim は依存ではなく、結論の前に対応 profiler と複数 warm run を使ってください。

### チャネルへの書き込み

ノードは`ChannelWrite`のリストを返します：

```python
return [
    ng.ChannelWrite("messages", [{"role": "assistant", "content": "Hi!"}]),
    ng.ChannelWrite("counter",  (state.get("counter") or 0) + 1),
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
| `llm_call` | 所有 typed 要求を一度準備し gate/reserve/receipt 後に同じハンドルを dispatch、全順序メッセージと結果全体を保持します。 | 読み取り `provider`, `model`, `instructions`, `tools` から `NodeContext`. |
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

Python は `input.ctx` に `cancel_token`、`usage`、`thread_id`、`step`、`stream_mode`、`store`、`resume_value`、`trace_id`、`run_id`、`model_token_budget` と typed provider 証拠を公開します。Deadline は `has_deadline` と `deadline_remaining_ms` で確認し、生の C++ steady-clock 値は非公開です。C++ 呼び出し元は `RunMetadata` で deadline と trace metadata を渡し、ネストした subgraph に伝播します。

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
    def get_definition(self): return ng.ChatTool("calc", "Double x", {"type": "object", "properties": {"x": {"type": "number"}}, "required": ["x"]})
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

`Send` は topic ごとに researcher を一つ呼ぶなど、実行中に target 呼び出し数を決めます。エンジンは通常 ready batch が返って書き込みを適用した後、同じ番号の superstep 内で生成された Send を実行します。

```python
class Planner(ng.GraphNode):
    def run(self, input):
        topics = decide_topics(input.state)            # e.g. 5 strings
        return ng.NodeResult(
            writes=[],
            sends=[ng.Send("researcher", {"topic": t}) for t in topics],
        )
```

### メンタルモデル

エンジンは `Send` ごとに compiled target を呼び、新しい node object は保証しません。同時呼び出しでは target の member state を安全に扱う必要があります。Payload は target がチャネルを読む前に適用します。単一 Send は共有状態、複数 Send は ready batch 後状態の隔離コピーを使い、全分岐の終了後に返却書き込みを呼び出し順で結合します。次の ready batch の経路は通常 node と Send target の信号を組み合わせます。

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

`build()` の既定は `EngineConfig::worker_count == 1` で engine-owned thread pool はなく、呼び出し元 coroutine executor に分岐を dispatch します。Coroutine I/O は重なりますが、単一 thread executor の CPU-bound 作業は直列化する場合があります。Multi-thread caller executor や同時 run では node member state を安全に扱う必要があります。

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
                    updates=[ng.ChannelWrite("retries",  (input.state.get("retries") or 0) + 1)],
                ),
            )
```

### Command と conditional edge の使い分け方

- **Conditional edge (条件付きエッジ)** : ルーティングは、ノード logic を必要としない state の predicateに依存します。よりクリーンで宣言的です。
- **コマンド**: ルーティングはノード内に記述するのが最も自然なロジックに依存する — 複数基準のスコアリング、コンテンツ検査、再試行判断。また、状態を原子的に更新し、かつ次ノードを選定する唯一の方法でもある。

### fan-in 下でのラストライター勝ち

複数の兄弟が空でない `Command.goto_node` を返すと、渡された経路順序の最後の command が通常エッジと barrier を上書きします。Static batch は ready 順、複数 `Send` は完了順でなく呼び出し順を渡します。返された command update はすべて書き込み pipeline で結合します。競合 command が workflow を変えるなら、経路決定ノードを一つにしてください。

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

`engine.fork(source_thread_id, new_thread_id, checkpoint_id="")` はチェックポイントを呼び出し側が指定した宛先スレッドへコピーし、新しいチェックポイント ID を返します。チェックポイント ID を省略すると元スレッドの最新チェックポイントを選びます。コピーは保留中の continuation を保持し、状態の編集だけでは新しい処理を予約しません。

`next_nodes == ["__end__"]` の完了済み continuation を resume すると、ノードを実行せず保存済み結果を復元します。編集した状態で停止中の処理を続けるには、`get_state_history()` から保留ノードが残る正確な過去のチェックポイント ID を選んで fork し、コピーを編集して resume します。過去の空の `next_nodes` ベクトルは別です。正確な ID を指定しない最新状態の resume は新しい実行を開始する従来の動作を保持し、exact-ID resume は指定したスナップショットに固定されます。

[Example 08](../examples/08_state_management.cpp) は新しい turn の流れを保持します。完了済みチェックポイントを fork し、ユーザーメッセージを編集してから `resume_if_exists=true` で `run()` を呼びます。その新しい実行が停止した場合だけ resume します。停止中の fork の resume を示す例ではありません。

`ChatMessage` / `ChatTool` と JSON は portable projection であり native 権限ではありません。Portable 形式は [`provider-message-v2`](../schemas/provider-message-v2.schema.json)、[`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json) のままです。真正な C++ checkpoint sidecar はメモリ内の native seal を保持します。永続 native 履歴には host-owned `sp::NativeArchive` が必要です。closed v3 / `spna3` は独立キーによる認証済み owner-private custody で、archive v2 は更新・解釈せず拒否します。認証は全 semantic descriptor 選択（origin/path/header、policy、要求 field mapping、usage path、stop mapping）、owner と正確な custody binding を結び付けます。暗号化や vendor-issuer 認証ではありません。archive 本文・キー・native blob・raw wire 観測は公開しません。Archive は証拠保存であり、金銭 grant や spending lease ではありません。Program/external bank は独立 journal が所有し、snapshot コピーで credit は作れません。

Provider 履歴には異なるモードがあります。同じ経路の native continuation は真正な reasoning、signature、順序付き tool group を元の binding の下で保持します。Gemini の既定値は `NativeOnly` です。明示的な `PortableForeign` は native seal、wire output、signature のない呼び出し側作成の assistant text と tool call を受け入れます。最初の外部 function call だけに Google が文書化した bypass marker を付け、text-only turn には signature を付けません。この projection は native 権限を与えず、失敗した native seal を修復したり portable に降格したりしません。任意の複数 vendor の履歴が native として移植可能になるわけではありません。

Responses の `previous_response_id` は provider が保持する会話状態を選び、要求には新しい入力だけを含めます。Client-tool の所有権に local 証拠が必要な場合、`previous_response_history` は真正な過去の所有権証拠を提供し、繰り返し入力として送信されません。Cursor は完全な native replay seal でも archive 権限でもなく、origin、route、model、configuration、完了状態の検査を受けます。

現在の SDK interface revision と shared-library generation は 4 であり、利用側は一致する header と library で再ビルドする必要があります。Output generation cap は native replay configuration と別に、呼び出しごとに admission と accounting の対象になります。新しい semantic call の cap を上げても元の bank、grant、deadline は更新されません。明示的に文書化された per-turn 選択を除き、content、prefix、origin、route、policy、tools、reasoning controls の binding は保持されます。Portable JSON v2 と native archive v3 / `spna3` は不変で、以下の過去の ABI3 測定は interface4 の結果ではありません。

Python も C++ と同じ所有 request/outcome 境界を公開します: `make_provider_request`、`Provider.prepare`、`dispatch`、`invoke`。Provider 履歴には typed part を持つ `ProviderMessage` を使い、`ChatMessage` はグラフ用の便宜的 projection として残ります。SDK 失敗は `ProviderOutcome.failure` で読み、host observer/settlement 例外は `outcome` と `cause` を保持します。コンストラクターと GIL/コールバック動作は [Python binding ガイド](python-binding.md)を参照してください。

`input_total`、`output_total`、`total` などの使用量カウンターは `std::optional<sp::Count>` で、存在する count は `uint64_t value` と `Evidence` を持ちます。`Usage` は stage、quality、conflict も記録します。欠落は不明であり、ゼロを作りません。

`UsageAccumulator::snapshot()` は累積報告を返します。`total_tokens_wide()` は計上済みトークンと未解決予約の合計で、報告使用量として表示してはいけません。精算には input/output count のある final・consistent 報告が必要で、根拠のある最大 total を計上し、超過使用量も clamp しません。累積対象の一つでも counter が欠落すれば集計も不明です。予約、ローカル計上、vendor 請求書は別の記録です。

**Standalone bank journal 修正 — 現在の契約を改訂；実際の runtime 証拠は下記。** Owner-approved protocol は単調 trusted-store namespace obligation と、実際の不変 original owner/thread/graph scope、ceiling、deadline/clock identity、generation を要求します。全 checkpoint commitment/revision に対する正確な durable head CAS だけが host-owned opaque lease を発行できます。正確な pending effect window を provider I/O 前に永続化し、真正な SDK outcome と実際の charge、nullable report、hold、dedup identity で精算しなければなりません。Checkpoint/next head は同じ owned actor/revision 下で原子的に publish します。Bank metadata 削除、checkpoint pruning、old authenticated snapshot replay、同一 ID overwrite、actor 喪失で credit を与えてはなりません。既存 65 hold がある ceiling 130 を 129 に下げると別の 65 は許可できません。証明済み no-effect 失敗は unchanged head を release し authentic 130 復旧を可能にできます。Crash/unknown/lost-lease window は refund/retry/fallback なしで hold を保持します。Plain/pristine archive 設定は money/native spending lease を与えず、現在の `config.usage` は既存 standalone obligation を置換できません。Program/external-bank journal 所有は不変です。これは要求契約です。実際の currency/custody 証拠と instrumentation 制約は下記であり、安定 released API 保証ではありません。

**現在の宣言；統合 runtime 証拠は下記:** `<neograph/graph/checkpoint.h>` は `owner_scope`、logical `thread_id`、private backend `storage_thread_id`、`graph_identity`、`original_ceiling`、`original_deadline_ticks`、`deadline_clock_identity` を持つ `ManagedBudgetLeaseScope` を宣言します。`OwnedManagedBudgetLease` は read-only `scope()`、`actor_id()`、不変 `bank_generation()`、`revision()`、`head_checkpoint_id()`、`head_commitment()` を公開し、公開 authority-import constructor はありません。`ManagedBudgetEffectReceipt` は `active()`、`effect_id()`、`claim_amount()`、`request_digest()` を公開し、default receipt は権限を与えません。`CheckpointStore` は `acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`、`begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`、`settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`、`publish_managed_budget_checkpoint(lease, checkpoint)`、`release_managed_budget_lease(lease)` と `_async` counterpart を宣言します。Sync `CheckpointStoreCore` と `AsyncCheckpointStore` はそれぞれの variant を公開します。`managed_budget_checkpoint_commitment(checkpoint)` は bank JSON だけでなく全永続 checkpoint を結び付けます。これらの宣言は backend CAS、currency 安全性、installed ABI 互換性、実際に成功した runtime 経路を証明しません。

**真正な InMemory shared-bank fork は保持・実証済み。** 元の真正な C++ fork は ONE original financial journal と trusted current branch head を使い、grant を複製しません。`publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)`（および `_async`）は authentic current source/full commitment と実際の same-bank native C++ pointer を要求し、durable standalone fork は明示的に unsupported のままです。`OwnedManagedBudgetLease::scope()` と original owner/thread/graph、ceiling、deadline/clock、generation は不変です。Read-only store-issued `execution_thread_id()` / `execution_storage_thread_id()` は execution branch を別に選び、`GraphState::budget_original_thread_id()` は元の financial bank を示します。正確な selected-branch head CAS と global actor/revision は canonical current counter、pending effect、burned identity に対して全 branch を直列化します。Original/fork branch は補充なしで使用可能なままです。Stale snapshot、checkpoint copy、imported JSON は alias を発行したり head を巻き戻したりできません。元の root30 → charge3 → original continuation6 → fork lower20 → continuation9 の same-bank 証明は未変更 test_graph_engine.cpp:810–913 で PASSED です。Saved original ceiling30 は effective fork ceiling20 と別です。Widening31 と JSON-only restore は拒否必須です。Unbounded reported observation は factual data で finite grant ではありません。証明済み zero-effect lease だけが unchanged head を release でき、unknown/pending effect は obligation を保持します。

**現在の release-error 契約；実際の suite/probe は下記。** `<neograph/graph/engine.h>` の `graph::ManagedBudgetLeaseReleaseError` は `ProviderOutcomeError` を継承します。`cause()` は元の execution exception を保持し、`release_error()` は二次 durable lease-disposition 失敗を公開します。`outcome()` は真正な SDK 証拠があれば保持し、SDK outcome がなければ null です。Release 失敗は結果を捏造せず再 dispatch も許可しません。Closed `_neograph_managed_budget_scope` metadata は元の logical scope/cap/deadline clock/generation を記述しますが、backend CAS 権限ではなく data です。

**Archive-owner/retention 契約；実際の suite/probe は下記。** Finite standalone root または authenticated finite source だけが、実際に設定された `sp::NativeArchive::owner_scope()` から省略された original owner を継承します。Unbounded/plain owner metadata の意味は不変です。明示的に矛盾する archive owner は lease acquire 前に拒否します。`CheckpointStore::retains_native_checkpoint() const noexcept` と対応する Core/Async storage capability は既定 false で、実際の InMemory backend は true に override し、wrapper は実際の retention を委譲します。この read-only 記述は正当な unleased/plain/unbounded C++ native checkpoint custody を許しますが、spending credit や native replay authority は与えません。Leased custody は JSON flag や推測した store type ではなく実際の store-issued receipt を使います。

**Native-custody pre-I/O gate；実際の suite/probe は下記。** Managed effect begin は pending-effect/slot/held-window 変更前に、真正に結び付けた NativeArchive または実際の local store-issued private C++ retention capability を要求します。Private capability は JSON から import せず wire にも転送しません。C++ sidecar は境界を越えられないため、remote backend が InMemory でも gRPC は実際の client/server archive を要求します。Archive が finite source owner を提供しなければ元の anonymous owner scope は空のままで、実際の archive binding は original scope に一致しなければなりません。Financial head/lease 証拠だけでは native-custody readiness を証明しません。

`ProgramFailure` は live `provider_outcome`・`provider_cause` を保持します。Canonical factual SDK witness は真正な archive custody を owner/run/version/bundle/operation/attempt に結び付け、Runtime は復旧失敗を公開する前に設定済み custody を eager に復元します。公開 data-only `ProgramResult::create()` は事前入力 witness で迂回できず、未解決の parsed seal は実行結果ではありません。プロセス再起動後は元の exception pointer がなく `provider_cause == nullptr` であり、text から再作成しません。永続化できない失敗は serialize/publish/replay できません。

`RecordedBindingSet` は source-bound の move-only data で、caller 提供 dispatcher ではありません。信頼された Catalog の `recorded_capability_binder` は実際の永続 source event を独立に読み、captured-only capability を materialize します。`ProgramRuntime::replay_recorded()` は元の selected-source permission を検証し、実際の残存 bank を durable CAS で移します。inherited spend は新しい model grant ではありません。旧 `start_recorded` 更新 API は削除されました。InMemory/File/SQLite/PostgreSQL Program store は実行全体で正確で不変の owned lease を保持し、expiry による更新をしません。Controlled JavaScript も underlying capability manifest を検証し、正確な completed command 結果を消費して external effect を再 dispatch しません。

**Recorded-control causal fix は full suite で実証済み。** Captured command replay は実行前に新しい CPU wall-time/Core work だけを durable に reserve し、測定済み work と新しく生成した Core checkpoint を result CAS で publish します。新しい model、money、Program-operation allowance を消費せず、captured external effect を再 dispatch しません。未精算 reservation は debit を保持します。Reservation により、最初の新しい Core checkpoint を拒否した通常の Running→Running transition ではなく認証済み settlement transition を選びます。Await channel receive、timer wait/cancel、handoff wait の開始/release は owning executor/strand 上で直列化します。既存 Recorded CPU/Memory await/handoff scenario は full suite で pass しました。Remote TSan coverage 制約は下記に明記します。

以下の観測はこの文書整備より前に記録されたものです。歴史的証拠であり、新 test 実行や全 platform・transport・security 性質の保証ではありません。

**有料観測は完了；普遍的な qualification ではありません。** 元の `SPQUAL1` base630/1000000 microUSD は不変です。同じ元 ledger の ONE hash-chained `A` が承認済み extension480/3000000 を受け入れ、aggregate1110/4000000 になります。Calls/spent/hold/settlement は累積で新 grant ID/header/reset はありません。正確な declaration byte/file identity と original authorization/baseline/catalog/activation/ledger-prefix の hash/totals は固定され、削除・置換・変更は fail closed です。最終 canonical ledger は calls1110/spent437958/held1287828 microUSD、eventA1、limits1110/4000000；spent+held US$1.725786 は LOCAL catalogue meter で invoice ではありません。記録済み five-family60-pair baseline は600 request 完了：Chat60/60、Responses60/60、Messages60/60、Generate56/60（incorrect-vision SSE4件）、Interactions57/60（incorrect-vision buffered1件/SSE2件）；合計293/300 pair で300/300ではありません。他の old600 financial record は保持しますが完全な behavioral proof ではありません。以前の M5/media one-shot cohort は不変です。以前の Google3-round prerequisite は invalid-tool2件/unreadable-positive1件の失敗状態を保持します。追加有料呼出しは承認されません。最終 SDK 証拠と native-axis 制約は baseline 成功とは別です。 以前の activation/reopen smoke は2回 reopen 後 calls610/spent219159/held751233、SDK meter/canary/vision4-test19.38秒 pass として保持します。これは限定された以前の checkpoint で最終 ledger totals ではありません。以前の検証済み Chat60-pair cohort は実際の attempt120、UpperBound charge120、UnknownHold なしを保持します。

**Native-axis 観測は cryptographic 検証・native consumption/equivalence ではありません。** Generate は mutation/omission/duplication を受け入れました。Interactions は isolated genuine source/positive control、one-owner signature mutation、thought-carrier omission、call-carrier omission、duplication を受け入れました。全 thought/signature 削除は generic400、THOUGHT item を保持して全 signature field を削除した場合も generic400 でした。最後の capture は local encoded-original retention control で、same-capture server positive ではありません。以前の positive cohort は真正です。観測は aggregate-carrier-absence boundary のみを示し、issuer/signature 検証や vendor consumption を証明しません。実際の report：SDK `config/qualification-extension-results.json`、`qualification-final-summary.json`、`qualification-native-axis-results.json`、`qualification-combined-omission-results.json`、`qualification-signature-presence-results.json`。Prerequisite-failed/not-run/negative-inconclusive の状態は事実のままです。 Thought-only/carrier-only omission は別 carrier が残る状態で受け入れられました。Issuer-validation/native-consumption の主張を強めません。

**実際の統合証明と残る制約。** 最新 Core full run は2242 test、失敗0、skip16（RAM process-loss 非適用14件/live-credential gate2件）、130.17秒です。`PgNestedJsonRoundTrips` は duplicate key/order/null metadata、blob、residual を正確に保持し0.18秒で pass。未変更の元 shared-bank fork と既存 Recorded CPU/Memory await/handoff scenario も pass。実際の wrappedMemory/SQLite/PostgreSQL/gRPC finite130/hold65/lower129/strip/old-head/pruning/no-archive/import probe は plain と ASan+UBSan で pass。LOCAL Memory/SQLite/PostgreSQL TSan scope は7件 pass、warning0。System Abseil/Protobuf を含む full mixed gRPC TSan は exit66、dependency/generated-RPC stack に race warning402件。これは instrumentation/coverage 制約で proven false positive ではありません。Remote TSan/race-free は主張せず warning を suppress しません。Installed find_package Program C++/C ABI/dualQuickJS の3 consumer は pass。Fresh installed NeoGraph/SchemaProvider typed consumer は実際の HTTP request2件、coroutine 開始前の provider 破棄、native/tool replay、refusal、known-zero/raw 保持、実際の LinkedMismatch 拒否で pass。Browser Alice/Bob isolation と generation2 replacement を目視検証し、PostgreSQL Program Chat black-box6件は18.989秒で pass。最新 SDK26/26 は失敗0、74.07秒で pass。最終 ReleaseGraph16設定 ×fresh process3回/48記録は38.29秒、失敗0、全 actual protocol/owned-outcome check pass で完了しました。NeoGraph `benchmarks/provider-cutover-final-results.json` と `benchmarks/provider-cutover-final-summary.json` は独立した最終 cohort を保持します。測定中 compiler/有料 model は実行せず、歴史 cohort は不変で semantic/resource equivalence は主張しません。Unstable SDK/ABI3 は安定 release や広い platform qualification ではありません。

Host 配信 limit、extent-bounded 診断/raw 証拠、共通 provider error と最小 media 証拠は [typed provider reference](reference-en.md#owned-outcome) に記載します。

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

> 旧 issue #16 は一部 glibc/OpenSSL 組合せで request ごとの入れ子 `io.run()` と旧 child-thread provider streaming bridge 使用時の `getaddrinfo` SEGV を観測しました。この bridge は typed 移行で削除されました。当時の構造テストは downstream HTTPS/sanitizer/load 条件を完全再現しておらず、現資格検証ではありません。現呼び出し元は明示的 `ProviderMode` と `invoke_async` / `dispatch_async` を使い、request ごとの loop 入れ子でなく既存の長寿命 executor で駆動します。偽の単一 token や completion 再送を回避策として作りません。

---

## 8.5. Tracing — OpenTelemetry + Phoenix / Langfuse

`neograph_engine.tracing.otel_tracer` と `neograph_engine.openinference.openinference_tracer` は graph event を run/node span に変えます。後者は `CHAIN` タグと node payload projection を記録します。Run ごとに graph callback を一つ選びます。モデル呼び出しを `LLM` span として記録するには graph compile 前に typed provider を `OpenInferenceProvider(inner, tracer, *, span_name="llm.complete")` で包みます。Wrapper は native C++ observer と継承した `prepare`/一度限りの `dispatch` または `invoke` を使います。Request の準備や破棄は span を開かず、承認済み dispatch は owned outcome、キャンセル、deadline、typed event を変更せず span を開きます。Tracer 失敗は provider 結果や例外を置換しません。

```python
from opentelemetry import context as otel_context
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import BatchSpanProcessor
from opentelemetry.exporter.otlp.proto.grpc.trace_exporter import OTLPSpanExporter
from neograph_engine import GraphEngine, NodeContext
from neograph_engine.openinference import OpenInferenceProvider, openinference_tracer


class ParentContextTracer:
    def __init__(self, tracer, parent_context):
        self.tracer, self.parent_context = tracer, parent_context

    def start_span(self, name):
        return self.tracer.start_span(name, context=self.parent_context)


def trace_graph(graph_spec, inner_provider, model, cfg):
    provider = TracerProvider()
    provider.add_span_processor(BatchSpanProcessor(
        OTLPSpanExporter(endpoint="http://localhost:4317", insecure=True)))
    tracer = provider.get_tracer("my-app")
    try:
        with openinference_tracer(tracer) as cb:
            parent = ParentContextTracer(tracer, otel_context.get_current())
            observed = OpenInferenceProvider(inner_provider, parent)
            engine = GraphEngine.compile(
                graph_spec, NodeContext(provider=observed, model=model))
            return engine.run_stream(cfg, cb)
    finally:
        provider.shutdown()
```

Local Phoenix endpoint は `docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix:latest` を起動し、`opentelemetry-api opentelemetry-sdk opentelemetry-exporter-otlp` を設置します。Graph specification、既存 provider、明示的 model と `RunConfig` を `trace_graph` に渡します。`ParentContextTracer` は run root を worker dispatch に明示的に渡し、cross-thread や node 別の自動 parent 伝播は保証しません。Python は dispatch 時の active OTel context を使い、prepared operation は寿命中 tracer adapter を保持します。

LLM span は公開 role/text projection、宣言済み scalar、既知 usage count のみを含みます。既知ゼロは記録し、不明は省略します。Native replay/reasoning、raw wire envelope/event と encoded request body は trace に入れず、request/outcome の本来の custody を維持します。失敗時の partial report を含む usage 属性は vendor charge や budget authority の証明ではありません。Charged/reserved accounting は `UsageAccumulator.authority_snapshot()` と Program の `provider_budget_authority` が扱います。

公開 text、例外メッセージ、graph payload にも application secret があり得ます。Exporter に渡すデータを選択または redact し、[OpenTelemetry の機密データ指針](https://opentelemetry.io/docs/security/handling-sensitive-data/)を参照してください。[OpenInference convention](https://github.com/Arize-ai/openinference/blob/main/spec/semantic_conventions.md) は `CHAIN` と `LLM` を定義します。[参照](reference-en.md#105-observability--opentelemetry--openinference)は NeoGraph の属性 subset、token event、Python typed 呼び出し例と C++ の寿命要件を説明します。

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

### Python RunResult の status と state を読む

`result.status` は typed `Completed`、`Interrupted`、`StepLimit`、`SafePoint` 状態を公開します。`result.output` は portable 最終 state、`result.interrupted`、`result.max_steps_exhausted`、`result.execution_trace` は run の観測です。`result.native_messages`、`result.provider_outcomes` は全 typed provider 証拠を保持します。[Python binding ガイド](python-binding.md#hitl-and-state)を参照してください。

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
