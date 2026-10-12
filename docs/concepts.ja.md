<!-- neograph-i18n: source=docs/concepts.md locale=ja source_sha256=3ddbf9b3903df0b61f2694ecfbb12af0682b96f25f81d50b4efc4865c4c576a1 -->
# NeoGraphのコアコンセプト — 解説ガイド

**Languages:** [English](concepts.md) | [한국어](concepts.ko.md) | [日本語](concepts.ja.md) | [简体中文](concepts.zh-CN.md)

サンプルを読む前に、このガイドに目を通してください。グラフを組み立てる順に、チャネル、ノード、エッジ、fan-out、ルーティング、チェックポイント、ストリーミングを説明します。

LangGraphを使ったことがあれば、リデューサー付きのチャネル、`Send`、`Command`、チェックポイントはおなじみのはずです。NeoGraphでは[CoreとProgramRuntime](../README.md#core-and-programruntime)の役割が分かれており、このガイドはCoreによるグラフ実行から始めます。Pythonからプロバイダーを呼び出すときは、削除済みのcompletionクラスではなく、型付きの[バインディング契約](python-binding.md)を使います。

---

## 目次

（セクション8.5 `Tracing — OpenTelemetry + Phoenix / Langfuse` はv0.6.0で追加しました。外部ドキュメントからのリンクを壊さないよう、番号付きの見出しは1〜9のままにしてあり、8.5はStreamingとCommon pitfallsの間にあります。）


1. [全体像](#1-the-big-picture)
2. [チャネルとリデューサー](#2-channels--reducers)
3. [ノード](#3-nodes)
4. [エッジと条件付きルーティング](#4-edges--conditional-routing)
5. [Send — 動的なfan-out](#5-send--dynamic-fan-out)
6. [Command — ルーティングの上書きと状態パッチ](#6-command--routing-override--state-patch)
7. [チェックポイント、割り込み、HITL](#7-checkpoints-interrupts-hitl)
8. [ストリーミングイベント](#8-streaming-events)
9. [よくある落とし穴](#9-common-pitfalls)

---

<a id="1-the-big-picture"></a>
## 1. 全体像

NeoGraphの**グラフ**は、次の4つで構成されます。

| 要素 | 内容 | 定義する場所 |
|---|---|---|
| **チャネル** | 共有状態の中にある、名前付きのスロット。各チャネルにはリデューサーがあり、新しい書き込みを既存の値とどう組み合わせるかを決めます。 | `definition["channels"]` |
| **ノード** | 状態を読み、書き込みを返す関数。必要に応じて `Send` や `Command` も返します。 | `definition["nodes"]` |
| **エッジ** | 次に実行するノードを固定で指す定義。 | `definition["edges"]` |
| **条件付きエッジ** | 述語でルーティングを決めるエッジ。状態に応じて、複数の次ノードから1つを選びます。 | `definition["conditional_edges"]` |

実行は**スーパーステップのループ**で進みます。

```
1. ready_set = nodes routed from __start__
2. while ready_set is not empty:
   a. run the ready batch against its pre-update channel state
   b. buffer returned writes, then fold them through channel reducers
   c. execute emitted Sends after ordinary writes; fold their results
   d. combine routing signals and evaluate updated state → new ready_set
```

通常の実行可能バッチ（ready batch）は、そのバッチの更新が入る前のチャネル状態を読みます。同じバッチのノードが返した書き込みは、実行中には読めません。エンジンは書き込みをいったんバッファに貯め、バッチの終了後にリデューサーで既存の値と結合し、更新後の状態でルーティングを評価します。これはグラフのスケジューリングの話で、モデル内部の計算を同期するわけではありません。

たとえば `counter` の初期値が0で、実行可能な2つのノードがどちらも `counter + 1` を返すと、2つとも0を読みます。overwriteリデューサーの結果は、2ではなく1です。カスタムの合計リデューサーを使えば、増分1の書き込み2つを合算して2にできます。`Send` は1つでも複数でも、それぞれがペイロードを適用した隔離済みの状態コピー上で実行されます。そのため、ペイロードが共有状態に届くことはありません（#389）。リデューサーの順序を決めても、モデルの応答や外部への副作用が再現できるようになるわけではありません。

---

<a id="2-channels--reducers"></a>
## 2. チャネルとリデューサー

状態はすべて、名前付きのチャネルに入ります。チャネルはデフォルトで、スーパーステップやチェックポイントをまたいで値を保持します。ノードは、チャネルに書き込むことで互いにやり取りします。値の結合方法、配列の保持方法、チェックポイントに含めるかどうかは、それぞれ独立して決めます。

### チャネルの定義

```python
"channels": {
    "messages":  {"reducer": "append"},     # conversation history
    "counter":   {"reducer": "overwrite"},  # latest value wins
    "summary":   {"reducer": "overwrite"},
}
```

### 組み込みのリデューサー

| リデューサー | 新しい書き込みの扱い | 主な用途 |
|---|---|---|
| `"overwrite"` | 新しい値が古い値を置き換えます。並列に書き込んだ場合は、最後に書いたものが残ります。 | 単一値の作業用領域（現在のノード、現在の質問、ルートのヒントなど）。 |
| `"append"` | 新しいリスト（必ずリストにすること）を、既存のリストの末尾に連結します。前のステップの値が先頭に並びます。並列ノードの書き込みは、完了順ではなくスケジューラーの実行可能順に、各ノード内では返した順に続きます。 | 会話メッセージ、検索結果、fan-outの収集結果。 |

> この2つのリデューサーは、エンジンの起動時に `ReducerRegistry::ReducerRegistry()` で登録されます（[`src/core/graph_loader.cpp`](../src/core/graph_loader.cpp)）。カスタムリデューサーは、C++では `ReducerRegistry::register_reducer(name, fn)` で、Pythonでは（v0.1.9以降）次のように登録します。
>
> ```python
> ng.ReducerRegistry.register_reducer("sum",
>     lambda current, incoming: (current or 0) + incoming)
> ```
>
> Pythonのcallableは GIL を保持して実行されます。そのため、並行する Send のfan-outは、Pythonのカスタムノードと同じように、ここで直列になります。同じ名前で再登録すると、以前のリデューサーが置き換わります。

### チャネルのライフサイクルとチェックポイントの契約

リデューサーが担当するのは、書き込みの結合です。配列をどこまで保持するかは別の設定（retention）で、`unbounded`（デフォルト）、`latest`、正の `retention_limit` を指定する `bounded` から選びます。retentionは、`ChannelWrite.Mode.Overwrite` を含むすべての書き込みの後で配列を切り詰めます。`latest` は、次の書き込みで置き換わるまで最後の要素を保持します。永続化の方法もこれらとは独立で、`checkpoint`（デフォルト。値とバージョンを保存）か `ephemeral`（値もバージョンも永続チェックポイントに含めない）を選びます。bounded指定のretentionは、保存サイズが変わるだけでなく、見える履歴そのものを変えます。

エンジンは、各ノードの書き込みを返された順に、静的なバッチの書き込みをスケジューラーの実行可能順に、複数の `Send` の結果を呼び出し順に結合します。完了した順は使いません。保留中の書き込み（pending write）は、これと同じタスクスロットにリプレイされます。overwriteは順序どおりに適用されて最後の書き込みが残り、appendは要素の順序を保ちます。カスタムリデューサーは副作用のない純粋な関数にし、リプレイしても結果が変わらないようにしてください。結合する単位を変えても結果を保つには結合法則が、順序に依存させないためには交換法則が必要です。明示的なoverwriteはリデューサーを通さず、その後でretentionだけを適用します。これらの規則は、モデルの出力や外部への副作用が再現できることまでは保証しません。

ephemeralな値は、スーパーステップをまたいで生き続け、ステップごとにリセットされることはありません。チェックポイントには、宣言されたephemeralチャネルの名前と、書き込み済みかどうかだけを記録し、値は保存しません。resume、`resume_if_exists`、ID指定のresume、状態の更新は、次のいずれかの場合に拒否されます。書き込み済みのephemeral状態がある場合、古いチェックポイントにguard（ephemeralの宣言を記録するメタデータ）がない場合、ephemeralチャネルの集合が変わっている場合です。最初のephemeral書き込みより前のチェックポイントなら、保留中の書き込みを所定の順序でリプレイしながら再開できます。`update_state` は、ephemeralへの書き込みを拒否します。複数の `Send` を同一プロセス内のワーカーで実行する場合、各ワーカーの隔離コピーは、その時点のephemeralの値を引き継ぎます。正しさに関わる状態はチェックポイントに残すか、新しい実行で永続的な入力から再構築してください。

`GraphState::restore` は、ephemeralチャネルを持つグラフを拒否します。その場合は、対応するguardを渡して `restore_checkpoint` を使うか、同一プロセス内のスナップショット（すべての現在値とバージョンを含む）に対して `restore_runtime` を使ってください。guardにはチェックポイントのメタデータを使うので、チャネルのblobレイアウトやストアのスキーマは変わりません。ephemeralチャネルのないグラフであれば、古い（値をすべて持つ）チェックポイントも、そのまま使えます。guardに対応していないバイナリへダウングレードする場合は、先にephemeralなスレッドとフォークを完了させるか、永続的な入力から再起動してください。古いリーダーは、後から追加されたguardを強制できません。

チェックポイント対象のチャネルは、値をすべて含む完全なスナップショットとして保存されます。Memory、SQLite、PostgreSQLの各ストアは、変化のない `(thread, channel, version)` の値を重複排除しますが、append履歴は書き込みのたびにバージョンが変わるので、スナップショットは大きくなり続けます。保留中の書き込みに記録されるのは、未完了のスーパーステップで成功したタスクです。チャネルの差分一般を表すものではありません。ステップごとにリセットするポリシーは、用意していません。安全に設計するには、書き込みとルーティングの後でいつリセットするか、さらに割り込み、リプレイ、Send の挙動まで決める必要があるためです。ephemeralな永続化とは別のものなので、混同しないでください。

差分ベースのチェックポイントは設計案であり、チャネルの設定ではありません。この方式では、完全なスナップショットから、順序付きの `{channel, version, write mode, value}` 差分を最大 *K* 個（バイト数のしきい値を併用する場合もあります）リプレイして状態を復元します。overwrite、retention、バージョン、リデューサーの同一性は、保たなければなりません。保留中の書き込みを消す前に、スナップショットと差分、チェックポイントのポインターをアトミックに公開する必要もあります。リンクの欠落、バージョンの飛び、未知のリデューサー、リプレイの失敗は拒否します。採用するには、新しいスキーマバージョンと、測定で確かめた効果が必要です。既存のスナップショットは、差分を捏造せずにベースへ移行します。段階的に切り戻せるよう、ロールアウト中は古いリーダー向けに完全なスナップショットも残します。差分だけのレコードがある場合、元のリデューサーレジストリで復元できない限り、ダウングレードは拒否します。現在のストアは、すべて完全スナップショット方式です。

ベースラインは `bench_checkpoint_store --threads 1 --iters 1 --history-steps 256 --payload 512 --backends memory,sqlite` で測ります。`postgres` と `--pg-url` を加えるのは、隔離したローカルのデータベースを使う場合に限ります。出力の各行には、シリアライズ後の論理バイト数、保存と読み込みのp50/p95、復元の深さが出ます。legacy方式の行ではblob数が出ます。メモリ確保の回数を調べるには、`heaptrack bench_checkpoint_store --threads 1 --iters 1 --history-steps 256 --payload 512 --backends memory` を実行します。JSONやSQLのネイティブなアロケーターは、すべてがC++の `operator new` で捕捉されるわけではありません。比較するときは、ペイロード、履歴、バックエンドの構成を揃え、推定値ではなく実測した削減量を使ってください。論理バイト数は、永続化後の物理バイト数とは異なります。SQLiteは一意な一時データベースを使い、終了時に削除します。`--sqlite-path` を指定すると新しいファイルを残し、既存のパスは拒否します。

記録済みのLinux x86-64 Debugビルドでのベースラインは、スレッド数1、履歴256ステップ、メッセージ512バイト、反復1回の条件で測ったものです。過去の測定結果であり、性能目標ではありません。

| バックエンド | 論理チェックポイントバイト数 | 保存 p50/p95 (µs) | 読み込み p50/p95 (µs) | リプレイの深さ |
| --- | ---: | ---: | ---: | ---: |
| Memory | 17,814,952 | 54 / 138 | 141 / 382 | 1 |
| SQLite | 17,814,952 | 289 / 1,589 | 176 / 474 | 1 |

削除する前に別途繰り返した測定では、SQLiteのデータベースとWALのサイズは14,811,136 / 4,210,672バイト（合計19,021,808バイト）でした。さらに、Linuxの `LD_PRELOAD` シムで、プロセス全体の `malloc`、`calloc`、0以外の `realloc` の要求回数を数えました。構築とJSONのパースも含みます。`--history-steps 0` と比べた増加分は、Memoryが88,277回・605,289,027バイト、SQLiteが114,295回・867,319,964バイトでした。これらは累積の要求であり、実際に使用中のメモリ量でも、ストアだけの確保量でもありません。アラインメント付きの確保と内部的な確保は捕捉していません。このシムは依存ではありません。結論を出す前に、サポート対象のプロファイラーを使い、ウォームアップ後の測定を複数回行ってください。

### チャネルへの書き込み

ノードは `ChannelWrite` のリストを返します。

```python
return [
    ng.ChannelWrite("messages", [{"role": "assistant", "content": "Hi!"}]),
    ng.ChannelWrite("counter",  (state.get("counter") or 0) + 1),
]
```

値の形は、リデューサーに合わせる必要があります。
- `"append"`：リストにします（リストが連結されます）。
- `"overwrite"`：JSONにシリアライズできる値なら、何でも使えます。

### ノードから状態を読む

```python
def run(self, input):
    msgs    = input.state.get("messages") or []  # list of message dicts
    counter = input.state.get("counter") or 0
    ...
```

`state.get(channel)` は、チャネルの現在の値を返します。チャネルは存在するもののまだ書き込まれていない場合は、`None` を返します。チャットメッセージを型付きで読むには、`state.get_messages()` を使います。`messages` チャネルを解析して `list[ChatMessage]` を返すもので、`llm_call` も内部でこれを使っています。

### バージョン

各チャネルは、単調に増えるバージョン番号 `version` を持ちます。エンジンはこれを、チェックポイントの差分比較と、確認用のAPI `state.channel_version(name)` に使います。通常、直接読む必要はありません。

---

<a id="3-nodes"></a>
## 3. ノード

ノードタイプを登録する方法は3つあります。自由度が低いものから順に説明します。

### 3.1 組み込みノード

| `type`（JSON内） | 動作 | 設定 |
|---|---|---|
| `llm_call` | 型付きのリクエストを1つ作って所有し、ゲート処理、予約、レシートの記録を行ったうえで、同じハンドルをdispatchします。順序付きのメッセージすべてと、結果全体を保持します。 | `provider`、`model`、`instructions`、`tools` を `NodeContext` から読みます。 |
| `tool_dispatch` | 最新のアシスタントメッセージの `tool_calls` を見て、それぞれを `Tool::execute` で実行し、`{role: "tool", tool_call_id, content}` の形で結果を追加します。 | `tools` を `NodeContext` から読みます。 |
| `intent_classifier` | LLMでユーザーの意図をN個のラベルのどれかに分類し、選んだラベルを `__route__` に書き込みます。`route_channel` 条件と組み合わせて使います。 | `extra_config: {labels, prompt_template}` |
| `subgraph` | 別のグラフを1つのノードとして埋め込みます。内側の状態とは、設定したキーの再マッピングを通してやり取りします。 | `extra_config: {graph_def, input_keys, output_keys}` |

### 3.2 `@ng.node` デコレーター（Pythonのみ）

書き込みだけを行うノードを、最も簡単に定義する方法です。

```python
@ng.node("greet")
def greet_node(state):
    name = state.get("name") or "world"
    return [ng.ChannelWrite("messages",
        [{"role": "assistant", "content": f"Hello, {name}!"}])]
```

デコレートする関数は、`list[ChannelWrite]` を返します（`None` を返すと `[]` として扱います）。`Send` や `Command` は返せないので、必要な場合は `GraphNode` を継承してください。

### 3.3 `GraphNode` を継承する完全な形

`run(input)` をオーバーライドすると、すべてを制御できます。v0.4.0で導入され、v0.9.0以降はカスタムノードの唯一のエントリポイントです。メソッドは1つ、シグネチャも1つです。

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

Pythonでは、`input.ctx` から `cancel_token`、`usage`、`thread_id`、`step`、`stream_mode`、`store`、`resume_value`、`trace_id`、`run_id`、`model_token_budget`、型付きのプロバイダーのエビデンスを参照できます。デッドラインは `has_deadline` と `deadline_remaining_ms` で確認します。C++のsteady clockの生の値は公開されていません。C++の呼び出し側は、デッドラインとトレースのメタデータを `RunMetadata` で渡します。これはネストしたサブグラフにも伝わります。

`Send` や `Command` が不要なら、`list[ChannelWrite]` をそのまま返すこともできます。バインディングが自動で `NodeResult` に変換します。

> **v0.3.xからの移行：** v0.4より前にあった、複数のエントリポイントを持つノードAPIは削除されました。代わりに `run(input)` をオーバーライドします。状態は `input.state` から読み、`input.stream_cb` がNoneでなければそれを通してトークンを出力し、キャンセルトークンは `input.ctx.cancel_token` から読みます。

JSONローダーがインスタンス化できるように、型を登録します。

```python
ng.NodeFactory.register_type(
    "researcher",
    lambda name, config, ctx: Researcher(name),
)
```

ファクトリには `(name, per-node config, NodeContext)` が渡されます。そのため、同じクラスを別の名前・別の設定で、複数登録できます。

### 3.4 ツール（`tool_dispatch` が使う、ノードとは別の概念）

`Tool` はノードではなく、`tool_dispatch` が呼び出す対象です。`ng.Tool` を継承して3つのメソッドをオーバーライドし、そのインスタンスを `NodeContext(tools=[…])` に渡します。

```python
class CalcTool(ng.Tool):
    def get_name(self):       return "calc"
    def get_definition(self): return ng.ChatTool("calc", "Double x", {"type": "object", "properties": {"x": {"type": "number"}}, "required": ["x"]})
    def execute(self, args):  return str(args["x"] * 2)
```

ツールのリストはコンパイル時にエンジンが所有するので、手元の参照はその後で破棄して構いません。

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

同じノードから複数のエッジを出すと、fan-outします（後続のノードがすべて、次のスーパーステップの実行可能な集合に入ります）。1つのスーパーステップから同じターゲットへ2本のエッジがあっても、ターゲットは1回しか実行されません。

### 条件付きエッジ

条件付きエッジは**名前付きの条件**を実行し、その結果で `routes` マップから次のノードを選びます。

```python
"conditional_edges": [
    {
        "from": "llm",
        "condition": "has_tool_calls",
        "routes": {"true": "dispatch", "false": ng.END_NODE},
    }
]
```

条件名は、エンジンに登録された `ConditionFn` に解決されます。組み込みで用意しているのは、次の2つです。

| 条件 | 戻り値 | 使いどころ |
|---|---|---|
| `has_tool_calls` | 最新のアシスタントメッセージの `tool_calls` が空でなければ `"true"`、そうでなければ `"false"`。 | ReActループ。LLMがツールを要求しなくなるまで、ツールのdispatchを繰り返します。 |
| `route_channel` | `__route__` チャネルに入っている文字列をそのまま返します。なければ `"default"` になります。 | `intent_classifier` と組み合わせて、意図に応じたルーティングを明示的に行うときに使います。 |

カスタム条件は、C++では `ConditionRegistry::register_condition(name, fn)` で、Pythonでは（v0.1.9以降）次のように登録します。

```python
def is_long(state):
    msgs = state.get("messages") or []
    return "long" if len(msgs) > 10 else "short"

ng.ConditionRegistry.register_condition("is_long", is_long)
```

この関数には現在の `GraphState` が渡されるので、`state.get(channel)` や `state.get_messages()` が使えます。戻り値は、条件付きエッジの `routes` のキーのどれかに一致する文字列にしてください。

### 同等の2つの書き方 — どちらもv0.1.8以降で使えます

条件付きエッジは、`edges` 配列の中（`condition` フィールド付き）に書いても、別の `conditional_edges` ブロックに書いてもかまいません。どちらの形式も受け付けるので、読みやすいほうを選んでください。

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

> **経緯：** v0.1.8より前は、形式Aがグラフコンパイラに黙って無視されていました。READMEとPythonのサンプルがすべてこの形式で書かれていたため、ReActループが1回のLLM呼び出しで終わってしまっていました。コミット `e23a523` で修正済みです。wheel 0.1.7以前でこの現象が起きた場合は、アップグレードしてください。

---

<a id="5-send--dynamic-fan-out"></a>
## 5. Send — 動的なfan-out

`Send` を使うと、ノードが実行時に呼び出し回数を決められます。たとえば、トピックごとにresearcherを1つ呼び出す、といった使い方です。エンジンは、通常の実行可能バッチが返って書き込みが反映された後に、出力された Send を同じ番号のスーパーステップの中で実行します。

```python
class Planner(ng.GraphNode):
    def run(self, input):
        topics = decide_topics(input.state)            # e.g. 5 strings
        return ng.NodeResult(
            writes=[],
            sends=[ng.Send("researcher", {"topic": t}) for t in topics],
        )
```

### 考え方

エンジンは、コンパイル済みのターゲットを `Send` 1つにつき1回呼び出します。呼び出しのたびに新しいノードオブジェクトを作るとは限りません。そのため、Send のターゲットは、並行して呼び出されてもメンバー状態を安全に扱えるように作る必要があります。ペイロードは、ターゲットがチャネルを読む前に適用されます。ステップが送った Send が1つでも複数でも、各 Send は、実行可能バッチ後の状態を隔離してコピーし、そこに自分のペイロードを適用して実行されます。したがってペイロードが見えるのは、その Send の実行（再試行の各回を含む）の中だけで、次のステップにも最終状態にも見えません（#389）。返された書き込みは、すべての Send が終わった後に、呼び出し順で結合されます。次の実行可能バッチは、通常のノードと Send ターゲットの両方のシグナルを合わせてルーティングされます。

`NodeInterrupt` を送出した Send のターゲットは、実行可能バッチのノードと同じように、実行を一時停止させます。その `NodeInterrupt` のチェックポイントは、スーパーステップ全体をやり直す起点になります。ステップ開始時点の状態を保持し、そのステップで実行可能だったノードを次のノードとして記録します。一時停止した結果が返す状態も、この開始時点の状態です。再開すると、ステップをもう一度実行します。このとき、終了済みのノードと Send は、記録済みの結果がリプレイされるだけで、再実行も再課金もされません。一時停止した Send と、まだ終了していない他の Send は、同じタスクIDで再開値を受け取って実行されます。そのため、前回の試行がジャーナルに記録したプロバイダー呼び出しは、リプレイされます。入口にあるmapノードのように、ステップより前にチェックポイントがない場合は、終了した結果を、割り込みのチェックポイント自体に記録します。

### よくある形：5個のfan-outからsummarizerへfan-in

```
planner ─┬─ Send("researcher", {topic: "A"})  ─┐
         ├─ Send("researcher", {topic: "B"})  ─┤
         ├─ Send("researcher", {topic: "C"})  ─┼─→ summarizer
         ├─ Send("researcher", {topic: "D"})  ─┤
         └─ Send("researcher", {topic: "E"})  ─┘
```

`researcher` から出るエッジは、`{"from": "researcher", "to": "summarizer"}` の1本だけです。静的エッジと同じ重複排除のルールが働くので、summarizerは1回だけ実行されます。

### ワーカー数の調整

`build()` のデフォルトは `EngineConfig::worker_count == 1` で、エンジンが所有するスレッドプールは作りません。分岐は、呼び出し側のコルーチンexecutor上でdispatchされます。コルーチンのI/Oは重ねて進められますが、CPUを使う処理は、スレッドが1つのexecutorでは直列になることがあります。呼び出し側のexecutorがマルチスレッドの場合や、複数の実行を同時に動かす場合も、ノードのメンバー状態は安全に扱う必要があります。

本当に並列で実行したいときは、プールを明示的に有効にします。fan-outの幅に合わせてNを指定するか、`set_worker_count_auto()` で `hardware_concurrency()` の値を使います（フォールバック値は4）。

```python
engine.set_worker_count(5)           # match a 5-way Send
# or
engine.set_worker_count_auto()       # hardware_concurrency()
```

プールを有効にしないまま、複数の Send（または複数の出力エッジ）によるfan-outを実行すると、NeoGraphは標準エラー出力に警告を1回だけ出します。気づかないうちに直列で動いている状態を、見逃さないためです。意図的に直列のfan-outを動かす場合（たとえばworker=1の高速パスのベンチマーク）は、`NEOGRAPH_SUPPRESS_FANOUT_WARNING=1` で警告を止められます。

---

<a id="6-command--routing-override--state-patch"></a>
## 6. Command — ルーティングの上書きと状態パッチ

`Command` を使うと、ノードは次の遷移先の決定と状態の更新を、1つの戻り値でまとめて行えます。通常の出力エッジは経由しません。

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

### Commandと条件付きエッジの使い分け

- **条件付きエッジ**：ルーティングが状態に対する条件だけで決まり、ノード内のロジックを必要としない場合に使います。宣言的で、すっきり書けます。
- **Command**：ルーティングの判断が、ノードの中に書くのが自然なロジックに依存する場合に使います。たとえば、複数の基準によるスコアリング、内容の検査、再試行の判断などです。状態の更新と次のノードの選択を、アトミックに行える唯一の方法でもあります。

### fan-in時のlast-writer-wins

複数の並列ノードが、空でない `Command.goto_node` を返した場合は、ルーティング順で最後のCommandが採用され、通常のエッジとバリアを上書きします。ルーティング順は、静的なバッチでは実行可能順、複数の `Send` では完了順ではなく呼び出し順です。返されたCommandの更新は、いずれも書き込みパイプラインを通じて、すべて結合されます。競合するCommandによってワークフローが変わってしまう場合は、ルーティングを決めるノードを1つにしてください。

---

<a id="7-checkpoints-interrupts-hitl"></a>
## 7. チェックポイント、割り込み、HITL

### チェックポイントストアの設定

```python
engine.set_checkpoint_store(ng.InMemoryCheckpointStore())
# or: engine.set_checkpoint_store(ng.PostgresCheckpointStore(...))   # if built with PG
```

ストアを接続すると、スーパーステップごとに、`(thread_id, checkpoint_id)` をキーとしてチェックポイントがストアに書き込まれます。`RunResult.checkpoint_id` は、そのうち最新のものです。

### 静的な割り込みポイント

```python
"interrupt_before": ["payment"],   # pause before this node runs
"interrupt_after":  ["llm"],       # pause after, before routing
```

エンジンは、`interrupted=True` を立て、`interrupt_node` を設定した `RunResult` を返します。再開するには、次のようにします。

```python
result = await engine.resume_async(thread_id="t1",
                                   checkpoint_id=result.checkpoint_id,
                                   new_input={...})  # optional
```

再開で通過するのは、答えた割り込み自身のゲートだけです。`interrupt_before` の一時停止から再開すると、止めていたノードが実行されます。`NodeInterrupt` から再開すると、そのステップが続きます。これ以外の再開、たとえば `interrupt_after` の一時停止後の再開や、プロセス停止後にステップのチェックポイントから復旧する場合は、これから実行する `interrupt_before` のノードの手前で、あらためて停止します。再開値が渡されるのは、`interrupt_before` の一時停止が止めていたノードだけです。`interrupt_after` の一時停止後の再開や、それ以外のチェックポイントからの再開では、どのノードにも値は渡されません（#393）。

1つのステップで同時に実行可能になった複数のゲート付きノードには、1つずつ答えます（#396）。再開は、実行が止まったゲートに答えます。ステップを実行する前に、そのステップの他のゲート付きノードのうち、まだ答えていないものがあれば、それぞれの `interrupt_before` の一時停止で、実行がもう一度止まります。ステップのゲートすべてに答えると、ステップが実行され、各ゲート付きノードは、自分のゲートへの答えを受け取ります。

### `NodeInterrupt` による動的な割り込み

ノード本体の中から送出します（Python：`raise ng.NodeInterrupt(reason)`、C++：`throw NodeInterrupt(...)`）。エンジンはこれを捕捉して状態を永続化し、送出したノードで中断した `RunResult` を返します。再開のAPIは同じです。実行可能バッチのノードでも `Send` のターゲットでも、同期・非同期・ストリーミングのどの入口からでも、同じように動作します。一時停止したステップで終了済みの並列ノードは、再開時に再実行されず、記録済みの結果がリプレイされます。ステップより前にチェックポイントがない場合も同じです。

1つのステップで複数の割り込みが起きた場合は、1つずつ答えます（#388）。1つのスーパーステップの中で複数のノードや Send が `NodeInterrupt` を送出すると、実行はそのすべてで一時停止します。`RunResult.interrupts` には、それぞれが `{id, node, value}` として並びます。id は、一時停止した呼び出しのタスクIDです（`s3:review`。Send の場合は `s3:send[1]:worker:<hash>`）。`interrupt_node`、`interrupt_value`、`interrupt_id` には、最初の割り込みが入ります。

再開すると、`interrupt_id=` で指定した割り込み（C++では `RunMetadata::resume_interrupt_id`）に答えます。指定がなければ、最初の割り込みに答えます。そのステップで一時停止していた他のノードや Send は、値なしでもう一度実行され、あらためて自分の質問を出します。それには次の再開で答えます。保留中でない割り込みを指定すると、何かが実行される前に `ValueError`（C++では `std::invalid_argument`）が発生します。複数の割り込みで一時停止したチェックポイントは、メタデータ（`_neograph.interrupts`）にその一覧を持ちます。割り込みが1つだけのチェックポイントは、従来と変わりません。

値が届くのは、一時停止したステップの中で答えを受けた呼び出しだけです（#393）。再開した実行の後のステップにあるノードには、`ctx.resume_value` が渡されません。そこで質問を出すノードやツールのゲートは、別の質問への答えを受け取るのではなく、実行をもう一度一時停止させます。親を通して再開したサブグラフは、親側で答えを受けたノードを通して、自分の答えを受け取ります。`messages` チャネルを持つグラフは、これまでどおり、その値をユーザーメッセージとして受け取ります。JavaScriptのjoinと、プランレベルの `parallel`・`quorum` も同じ規則です。再開のたびに、一時停止しているメンバーの1つに答え、残りはもう一度質問を出します。

並列ノードが異なる終わり方をしたステップは、並列ノードの順番や完了順にかかわらず、1つの規則で結末が決まります（#392）。この規則は、fan-outの分岐にも、ステップ内の Send にも、ワーカーが1つでも複数でも、同じように適用されます。まず、実行がキャンセルされた場合は、そこで止まります。キャンセルされていなければ、不確かな効果（payloadの `kind` が `tool_effect_reconciliation` である `NodeInterrupt`）があるかを見ます。ある場合は、並列ノードが失敗していても、照合（reconciliation）のためにステップを一時停止します。ここで実行を終えると、結果を誰も知らない効果を放棄することになるからです。この一時停止で報告されるのは、該当する割り込みだけです。結果を記録していない失敗した並列ノードは、ステップを再開したときに、もう一度実行されます。

不確かな効果がなければ、失敗があるかを見ます。失敗がある場合は、ディスパッチ順（実行可能順、その次に Send の順）で最初の失敗によって、実行全体が失敗します。そのステップの通常の割り込みは報告されず、そのためのチェックポイントも保存されません。失敗もなければ、ステップは上で説明したとおり、すべての割り込みで一時停止します。failure edgeに沿ってルーティングされる失敗は、完了と同じく、そのノードの結果として扱います。並列ノードの割り込みは引き続きステップを一時停止させ、ルートはステップが完了したときにたどられます。

JavaScriptのjoinと、プランレベルの `parallel`・`quorum` も、メンバーを同じ順序で判定します（#397）。不確かな効果で一時停止したメンバーがいると、失敗したメンバーがいても、joinは一時停止します。そのとき止まるのは、そうしたメンバーのうち最初の1つです。ただし、成功によってすでに結果が決まっている場合（raceの勝者が出た、quorumを満たしたなど）は除きます。失敗したメンバーは記録され、再開時にjoinを終わらせます。それは、再開が答えたメンバーが最後まで実行され、その効果が確定した後です（#400）。joinが一時停止するときは、宣言順にかかわらず、不確かな効果で一時停止したメンバーが、通常の質問で一時停止したメンバーより先に問われます（#401）。収集型の `ng.all`（`collect: true`）は、上の規則の意図的な例外です。一時停止したメンバーを含め、すべてのメンバーを待ってから報告します。そのため、別のメンバーが通常の質問で一時停止している間は、メンバーが失敗してもjoinは終わらず、その場で一時停止します。失敗は、すべてのメンバーが確定してから報告します（#401）。

一時停止するかどうかが、ノードの途中の出力に依存する場合に便利です（たとえば「LLMが、人に見せる価値のある内容を生成したか」）。

### タイムトラベル

`engine.fork(source_thread_id, new_thread_id, checkpoint_id="")` は、チェックポイントを呼び出し側が指定した宛先スレッドにコピーし、新しいチェックポイントIDを返します。チェックポイントIDを省略すると、コピー元の最新のチェックポイントが選ばれます。コピーは保留中の継続（continuation）を引き継ぎます。状態を編集しても、新しい処理が予約されることはありません。`NodeInterrupt` のチェックポイントをフォークすると、一時停止したスーパーステップの保留中の書き込みも一緒に引き継がれます。そのため、フォークを再開しても、コピー元を再開したときと同じように、そのステップで終了したノードと Send は再実行されず、記録済みの結果がリプレイされます。プロバイダー呼び出しも繰り返されません（#390）。コピー元の書き込みは、コピー元に残ります。

`next_nodes == ["__end__"]` の完了済みの継続を再開すると、ノードは実行されず、保存されている結果が復元されます。編集した状態で、一時停止していた処理を続けたい場合は、`get_state_history()` から、まだ保留中のノードが残っている過去のチェックポイントをIDで正確に選んでフォークし、コピーを編集してから再開します。過去のチェックポイントで `next_nodes` が空のベクトルだった場合は、扱いが異なります。IDを指定しない最新状態の再開は、従来どおり新しい実行として始まります。IDを指定した再開は、そのスナップショットに固定されます。

[サンプル08](../examples/08_state_management.cpp)は、新しいターンを始める流れのままです。完了済みのチェックポイントをフォークし、ユーザーメッセージを編集してから、`resume_if_exists=true` で `run()` を呼びます。再開するのは、その新しい実行が割り込みで止まった場合だけです。一時停止したフォークを再開する例ではありません。

`ChatMessage` / `ChatTool` とJSONは、持ち運び用の射影（portable projection）であり、ネイティブな権限（native authority）を持ちません。持ち運び用のフォーマットは、これまでどおり[`provider-message-v2`](../schemas/provider-message-v2.schema.json)と[`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json)です。本物のC++のチェックポイントのサイドカーは、メモリ上でネイティブシール（native seal）を保持します。ネイティブな履歴を永続化するには、ホストが所有する `sp::NativeArchive` が必要です。これは閉じたv3形式（`spna3`）で、認証付きのオーナー専用の保管（custody）と、独立した鍵を使います。archive v2は拒否します。アップグレードも解釈もしません。認証が結び付けるのは、セマンティックなディスクリプターの選択内容すべて（origin、パス、ヘッダー、ポリシー、リクエストのフィールドマッピング、usageのパス、stopのマッピング）と、オーナー、正確な保管の対応付けです。暗号化ではなく、ベンダー（発行元）の認証でもありません。archiveの本体、鍵、ネイティブなblob、生のワイヤー観測データは、公開しないでください。archiveはエビデンスの保管場所であり、金銭の付与（grant）でも、支出のリース（spending lease）でもありません。Programと外部のバンクは、これまでどおり独立したジャーナルが所有します。スナップショットをコピーしても、クレジットは作れません。

プロバイダーの履歴には、いくつかのモードがあります。同じルートでネイティブに継続する場合は、本物のreasoning、署名、順序付きのツールグループを、元の結び付けのまま保持します。Geminiのデフォルトは `NativeOnly` です。`PortableForeign` を明示すると、呼び出し側が作成したアシスタントのテキストとツール呼び出しを受け付けます。これらには、ネイティブシールも、ワイヤー出力も、署名もありません。Googleが文書化しているバイパス用マーカーを付けるのは、最初の外部（foreign）の関数呼び出しだけです。テキストだけのターンには、署名を付けません。この射影はネイティブな権限を与えません。失敗したネイティブシールを修復することも、ポータブルな形式に格下げすることもしません。複数ベンダーの任意の履歴が、ネイティブのまま持ち運べるようになるわけでもありません。

Responsesの `previous_response_id` は、プロバイダー側が保持する会話状態を指定するものです。リクエストに含めるのは、新しい入力だけです。クライアント側ツールの所有権をローカルのエビデンスで確かめる必要があるときは、`previous_response_history` が、過去の所有権の本物のエビデンスを提供します。これは繰り返しの入力としては送信されません。カーソルは、完全なネイティブリプレイシールではなく、archiveの権限でもありません。origin、ルート、モデル、設定、完了状態の検査は、引き続き受けます。

現在のSDKのインターフェースリビジョンと、共有ライブラリのジェネレーションは4です。利用側は、対応するヘッダーとライブラリで再ビルドする必要があります。出力生成の上限（cap）は、ネイティブリプレイの設定とは別に、呼び出しのたびにadmissionと会計処理の対象になります。新しいセマンティックな呼び出しのために上限を引き上げても、元のバンク、グラント、デッドラインは更新されません。コンテンツ、プレフィックス、origin、ルート、ポリシー、ツール、reasoningの制御は、ターンごとに選べると明記されているものを除き、結び付けられたままです。ポータブルJSON v2とネイティブarchive v3（`spna3`）は変わりません。以下の過去のABI3の測定結果は、interface4の結果ではありません。

Pythonも、C++と同じ、所有権を持つリクエストと結果の境界を公開しています。`make_provider_request`、`Provider.prepare`、`dispatch`、`invoke` です。プロバイダーの履歴には、型付きのパートを持つ `ProviderMessage` を使います。`ChatMessage` は、グラフ向けの簡易的な射影として残っています。SDKが返した失敗は、`ProviderOutcome.failure` から参照できます。ホストのオブザーバーや精算（settlement）で起きた例外は、`outcome` と `cause` を保持します。コンストラクターや、GILとコールバックの動作は、[Pythonバインディングガイド](python-binding.md)を参照してください。

`input_total`、`output_total`、`total` などの使用量カウンターは、`std::optional<sp::Count>` です。値がある場合は、`uint64_t value` と `Evidence` を持ちます。`Usage` には、ステージ、品質、競合（conflict）も記録されます。値がないのは「不明」を意味し、ゼロとして補うことはありません。

`UsageAccumulator::snapshot()` は、累積したレポートを返します。`total_tokens_wide()` が返すのは、計上済みのトークン数と、未解決の予約の合計です。報告された使用量として表示してはいけません。精算には、入力と出力の件数を含む、最終的で整合したレポートが必要です。精算では、裏付けのある最大の合計を計上し、上限を超えた使用量でも切り詰めません。累積したレポートのどれか1つでもカウンターが欠けていれば、集計結果でもそのカウンターは不明のままです。予約、ローカルでの計上、ベンダーの請求書は、それぞれ別の記録です。

**単独バンクのジャーナルの訂正：現在の契約を改訂しました。実際に動かして得た実行時のエビデンスは後述します。** オーナーが承認したプロトコルでは、次の2点が必要です。1つは、単調増加する、信頼されたストアの名前空間に対する義務（namespace obligation）です。もう1つは、実在する不変の元のオーナー／スレッド／グラフのスコープ、上限（ceiling）、デッドライン／クロックの識別子、世代（generation）です。ホストが所有する不透明なリース（opaque lease）を発行できるのは、チェックポイント全体のコミットメントとリビジョンに対する、永続的なヘッドの正確なCASだけです。保留中のエフェクトのウィンドウは、プロバイダーI/Oの前に、正確に永続化しなければなりません。精算には、本物のSDKの結果と、実際の課金額、nullを許すレポート、ホールド、重複排除用のIDを使います。チェックポイントと次のヘッドは、同じ所有アクター／リビジョンのもとでアトミックに公開します。バンクのメタデータを削除する、チェックポイントを間引く、認証済みの古いスナップショットをリプレイする、同じIDを上書きする、アクターを失う、といった操作でクレジットが付与されてはいけません。上限を130から129に絞ったとき、すでに65のホールドがあると、さらに65を受け入れることはできません。エフェクトがなかったと証明された失敗であれば、変更のないヘッドを解放できるため、本物の130での復旧は引き続き可能です。クラッシュ、不明、リース喪失のウィンドウは、返金も再試行もフォールバックもせず、ホールドしたままにします。plain／pristineなarchive設定は、金銭もネイティブな支出リースも与えません。現在の `config.usage` で、既存の単独の義務を置き換えることもできません。Programや外部バンクのジャーナルの所有関係は変わりません。以上は要求される契約です。実際の通貨と保管のエビデンス、および計測上の制約は後述します。安定版として公開済みのAPIを保証するものではありません。

**現在の宣言。統合した実行時のエビデンスは後述します。** `<neograph/graph/checkpoint.h>` は、`owner_scope`、論理的な `thread_id`、バックエンド内部用の `storage_thread_id`、`graph_identity`、`original_ceiling`、`original_deadline_ticks`、`deadline_clock_identity` を持つ `ManagedBudgetLeaseScope` を宣言しています。`OwnedManagedBudgetLease` が公開するのは、読み取り専用の `scope()`、`actor_id()`、不変の `bank_generation()`、`revision()`、`head_checkpoint_id()`、`head_commitment()` です。権限をインポートする公開コンストラクターはありません。`ManagedBudgetEffectReceipt` は `active()`、`effect_id()`、`claim_amount()`、`request_digest()` を公開し、デフォルトのレシートは何の権限も与えません。`CheckpointStore` は、`acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`、`begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`、`settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`、`publish_managed_budget_checkpoint(lease, checkpoint)`、`release_managed_budget_lease(lease)` と、それぞれの `_async` 版を宣言しています。同期版の `CheckpointStoreCore` と `AsyncCheckpointStore` は、それぞれの版を公開します。`managed_budget_checkpoint_commitment(checkpoint)` が対象とするのは、バンクのJSONだけでなく、永続化されるチェックポイント全体です。これらの宣言だけでは、バックエンドのCAS、通貨の安全性、インストール済みABIの互換性、実行時の経路が実際に成功することは、いずれも確認できません。

**本物のInMemoryの共有バンクフォーク：維持しており、動作も確認済みです。** 元からある本物のC++のフォークは、元の1つの会計ジャーナルと、信頼された現在のブランチヘッドを使います。グラントを複製することはありません。`publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)`（と `_async` 版）には、本物の現在のコピー元とそのコミットメント全体、および同じバンクのネイティブC++ポインターの実体が必要です。永続的な単独フォークは、引き続き明示的に未対応です。`OwnedManagedBudgetLease::scope()` と、元のオーナー／スレッド／グラフ、上限、デッドライン／クロック、世代は不変のままです。ストアが発行する読み取り専用の `execution_thread_id()` / `execution_storage_thread_id()` は、実行するブランチを別に選びます。`GraphState::budget_original_thread_id()` は、元の会計バンクを示します。選択したブランチのヘッドに対する正確なCASと、グローバルなアクター／リビジョンにより、標準の現在のカウンター、保留中のエフェクト、使用済み（burned）のIDに対して、すべてのブランチが直列化されます。元のブランチもフォークしたブランチも、補充なしで使い続けられます。古いスナップショット、チェックポイントのコピー、インポートしたJSONは、エイリアスを作ることも、ヘッドを巻き戻すこともできません。元のroot30 → charge3 → original continuation6 → fork lower20 → continuation9という同一バンクの証明は、変更していない test_graph_engine.cpp:810–913 でPASSしました。保存された元の上限30は、フォーク側の有効な上限20とは別です。31への拡大と、JSONだけでの復元は、拒否されなければなりません。上限のない報告済みの観測値は事実を示すデータであり、有限のグラントではありません。変更のないヘッドを解放できるのは、エフェクトがゼロだと証明されたリースだけです。不明または保留中のエフェクトがあると、義務は保持されます。

**現在のrelease-errorの契約：実際に動かしたスイートとプローブは後述します。** `<neograph/graph/engine.h>` の `graph::ManagedBudgetLeaseReleaseError` は、`ProviderOutcomeError` から派生します。`cause()` は元の実行時例外を保持し、`release_error()` は二次的な、永続的なリース処理の失敗を返します。`outcome()` は、本物のSDKのエビデンスがあれば保持し、SDKの結果がなければnullです。releaseに失敗しても、結果を作り上げたり、再dispatchを許可したりすることはありません。閉じたメタデータ `_neograph_managed_budget_scope` には、元の論理スコープ／上限／デッドラインのクロック／世代が書かれていますが、これはデータであり、バックエンドのCASによる権限ではありません。

**archiveのオーナーと保持（retention）の契約：実際に動かしたスイートとプローブは後述します。** 省略された元のオーナーを、実際に設定されている `sp::NativeArchive::owner_scope()` から引き継ぐのは、有限の単独ルート、または認証済みの有限のコピー元だけです。上限のない場合とplainな場合のオーナーメタデータの意味は変わりません。archiveのオーナーが明示的に食い違っている場合は、リースを取得する前に拒否します。`CheckpointStore::retains_native_checkpoint() const noexcept` と、それに対応するCore／Asyncのストレージ機能は、デフォルトでfalseです。実際のInMemoryバックエンドはtrueにオーバーライドし、ラッパーは実際の保持状況を委譲しなければなりません。この読み取り専用の記述により、正当な、リースなし／plain／上限なしのC++のネイティブチェックポイントの保管が許されます。ただし、支出クレジットもネイティブリプレイの権限も与えません。リース付きの保管には、JSONのフラグや推測したストア型ではなく、ストアが実際に発行したレシートを使います。

**ネイティブ保管のI/O前ゲート：実際に動かしたスイートとプローブは後述します。** 管理対象のエフェクトを開始するには、保留中のエフェクト、スロット、ホールドしたウィンドウを変更する前に、本物の結び付けを持つNativeArchive、または実際のローカルでストアが発行したプライベートなC++の保持機能（capability）が必要です。このプライベートな機能は、JSONからインポートされることも、ワイヤー上で転送されることもありません。C++のサイドカーは境界を越えられないため、リモートのバックエンドがInMemoryであっても、gRPCにはクライアントとサーバー双方の実際のarchiveが必要です。archiveが有限のコピー元オーナーを提供しない場合、元の匿名のオーナースコープは空のままです。実際のarchiveの結び付けは、元のスコープと一致しなければなりません。財務上のヘッドとリースのエビデンスだけでは、ネイティブ保管の準備ができているとは証明できません。

`ProgramFailure` は、生きている `provider_outcome` と `provider_cause` を保持します。正規の事実としてのSDKwitness（canonical factual SDK witness）は、本物のarchiveの保管を、オーナー／run／バージョン／バンドル／オペレーション／試行に結び付けます。Runtimeは、復旧した失敗を公開する前に、設定済みの保管を先に復元します。公開されているデータのみの `ProgramResult::create()` は、あらかじめ入力したwitnessでこれを迂回することはできません。未解決のパース済みシールは、実行可能な結果ではありません。プロセスの再起動後は、元の例外ポインターが存在せず（`provider_cause == nullptr`）、テキストから再作成することもありません。永続化できない失敗は、シリアライズも公開もリプレイもできません。

`RecordedBindingSet` は、ソースに結び付けられた、ムーブ専用のデータです。呼び出し側が渡すdispatcherではありません。信頼されたCatalogの `recorded_capability_binder` が、実際に永続化されたソースイベントから、取得済み（captured）の機能だけを独立して実体化します。`ProgramRuntime::replay_recorded()` は、元の選択済みソースの権限を確認したうえで、実際に残っているバンクを永続的なCASで移します。引き継いだ支出は、新しいモデルのグラントではありません。以前の `start_recorded` による更新APIは削除しました。InMemory、File、SQLite、PostgreSQLの各Programストアは、実行の間じゅう、正確で不変な、所有されたリースを保持します。期限が切れても更新されません。制御されたJavaScriptも、基になる機能マニフェストを検証し、正確な完了済みコマンドの結果を消費します。外部のエフェクトを再dispatchすることはありません。

**Recorded-controlの因果的な修正：フルスイートで動作確認済み。** capturedしたコマンドのリプレイは、実行前に、新しいCPUの実時間（wall-time）とCoreの処理だけを、永続的に予約します。そして、測定した処理量と、新しく生成されたCoreチェックポイントを、結果のCASで公開します。新しいモデル、金銭、Programオペレーションの許容量は消費せず、capturedした外部エフェクトを再dispatchすることもありません。精算されていない予約は、引き落とされたままです。予約により、通常のRunning→Runningの遷移ではなく、認証済みの精算遷移が選ばれます。通常の遷移では、最初の新しいCoreチェックポイントが拒否されていました。awaitのチャネル受信、タイマーの待機／キャンセル、ハンドオフ待機の開始と解放は、それぞれを所有するexecutorとstrand上で直列化されます。既存のRecorded CPU／Memoryのawait／handoffシナリオは、フルスイートで通りました。リモートのTSanの対象範囲の限界は、後述のとおり明記します。

以下の観測結果は、このドキュメントを整理する前に記録したものです。過去のエビデンスであり、新たに実行したテストの結果でも、すべてのプラットフォーム・トランスポート・セキュリティ特性についての保証でもありません。

**有料の観測は完了しています。これは、あらゆる条件での適格性確認（qualification）ではありません。** 元の `SPQUAL1` の基本枠630/1000000 microUSDは変わりません。ハッシュで連結された1つの `A` が、承認済みの拡張枠480/3000000を、同じ元の台帳の中に受け入れています。合計は1110/4000000で、呼び出し回数、支出、ホールド、精算は累積されており、新しいグラントID、ヘッダー、リセットはありません。宣言の正確なバイト列とファイルの同一性、および元の認可、ベースライン、カタログ、アクティベーション、台帳プレフィックスのハッシュと合計値は、固定されたままです。削除、置換、変更を行うと、フェイルクローズになります。最終的な正規の台帳は、呼び出し1110回、支出437958、ホールド1287828（いずれもmicroUSD）、イベントA1、上限1110/4000000です。支出とホールドの合計US$1.725786は、ローカルのカタログ上のメーターの値であり、請求書の金額ではありません。文書化済みの5ファミリー×60ペアのベースラインは、600リクエストを完了しました。内訳は、Chat 60/60、Responses 60/60、Messages 60/60、Generate 56/60（不正解のvision SSEが4件）、Interactions 57/60（不正解のvisionが、バッファ型で1件、SSEで2件）です。合計は293/300ペアで、300/300ではありません。ほかの旧600件の会計記録は保存してありますが、挙動を完全に証明するものではありません。以前のM5／メディアのワンショットのコホートは変わりません。以前の3ラウンドのGoogleの前提条件には、無効なツール2件と、読み取れないpositive 1件の失敗が残っています。これ以上の有料呼び出しは承認されていません。最終的なSDKのエビデンスとネイティブ軸の制約は、ベースラインの成功とは別のものです。以前のアクティベーションと再オープンのスモークテストは、2回再オープンした後の呼び出し610回、支出219159、ホールド751233として記録されています。SDKのメーター／カナリア／visionの4テストは、19.38秒で通過しました。これらは範囲を限定した以前のチェックポイントであり、最終的な台帳の合計ではありません。以前に検証したChat 60ペアのコホートは、実際の試行120件、UpperBoundの課金120件を保持しており、UnknownHoldはありません。

**ネイティブ軸の観測は、暗号学的な検証でも、ネイティブでの消費や同等性の確認でもありません。** Generateは、改変、欠落、重複を受け入れました。Interactionsは、隔離した本物のコピー元／positive control、1つのオーナーの署名の改変、thoughtキャリアの欠落、callキャリアの欠落、重複を受け入れました。thoughtと署名をすべて削除すると、汎用の400が返りました。THOUGHT項目を残したまま署名フィールドをすべて削除した場合も、汎用の400でした。最後のキャプチャには、ローカルでエンコードした元データを保持するコントロールがありましたが、同じキャプチャでのサーバー側のpositiveではありません。以前のpositiveのコホートは、本物のままです。これらの観測が確立するのは、キャリアがまとめて欠けた場合の境界だけです。発行元や署名の検証、ベンダー側での消費は確立しません。実際のレポートは、SDKの `config/qualification-extension-results.json`、`qualification-final-summary.json`、`qualification-native-axis-results.json`、`qualification-combined-omission-results.json`、`qualification-signature-presence-results.json` です。前提条件の失敗、未実行、ネガティブで結論が出ない、といった状態は、事実のまま残しています。thoughtだけ、またはキャリアだけを欠落させた場合は、別のキャリアが残っている限り受け入れられました。これは、発行元の検証やネイティブでの消費についての主張を強めるものではありません。

**実際の統合的な検証結果と、残る制約。** 直近のCoreのフル実行は、2242テスト、失敗0件、スキップ16件（RAMでのプロセス喪失のケース14件は対象外、実際の認証情報が必要なゲート2件）で、130.17秒でした。`PgNestedJsonRoundTrips` は、重複キー、順序、nullのメタデータ、blob、残余（residual）を正確に保ったまま、0.18秒で通りました。変更のない元の共有バンクフォークと、既存のRecorded CPU／Memoryのawait／handoffシナリオも通りました。ラップした実際のMemory／SQLite／PostgreSQL／gRPCに対する、finite130／hold65／lower129／strip／old-head／pruning／no-archive／importのプローブは、通常ビルドとASan+UBSanの両方で通りました。ローカルのMemory／SQLite／PostgreSQLに対するTSanの範囲は、7件が通り、警告は0件でした。システムのAbseil／Protobufを含む、gRPC混在のフルTSanは、終了コード66で終わり、依存ライブラリと生成されたRPCのスタックに、レース警告が402件出ました。これは計装とカバレッジの限界であり、誤検知だと証明されたわけではありません。リモートのTSanによる検証や、レースがないことは主張せず、警告の抑制もしていません。インストール済みのfind_packageを使う、Program C++／C ABI／dualQuickJSの3つのコンシューマーは通りました。新たにインストールしたNeoGraph／SchemaProviderの型付きコンシューマーは、次の項目を通りました。実際のHTTPリクエスト2件、コルーチン開始前のプロバイダー破棄、ネイティブ／ツールのリプレイ、拒否（refusal）、known-zeroと生データの保持、実際のLinkedMismatchの拒否です。ブラウザーでのAlice/Bobの分離と、世代2への置き換えは、目視で確認しました。PostgreSQLのProgram Chatのブラックボックステスト6件は、18.989秒で通りました。直近のSDKは26/26が通り、失敗0件、74.07秒でした。最終的なReleaseGraphは、16構成×新規プロセス3回の繰り返し（48レコード）を38.29秒で完了し、失敗0件、実際のプロトコルとowned-outcomeのチェックがすべて通りました。NeoGraphの `benchmarks/provider-cutover-final-results.json` と `benchmarks/provider-cutover-final-summary.json` に、この最終コホートを個別に保存しています。測定中にコンパイラーや有料モデルは実行していません。過去のコホートは変更しておらず、セマンティクスやリソース使用量が同等だとは主張しません。不安定なSDK／ABI3は、安定版リリースでも、より広いプラットフォームでの適格性確認でもありません。

ホスト側の配信の制限、範囲（extent）を限定した診断用／生のエビデンス、共通のプロバイダーエラー、最小限のメディアエビデンスは、[型付きプロバイダーのリファレンス](reference-en.md#owned-outcome)に記載しています。

---

<a id="8-streaming-events"></a>
## 8. ストリーミングイベント

`run_stream` / `run_stream_async` は、イベントが発生するたびにコールバックを呼び出します。モードはビットマスクで、ORで組み合わせられます。

| モード | 発行されるイベント |
|---|---|
| `EVENTS` | `NODE_START`、`NODE_END`、`INTERRUPT` |
| `TOKENS` | `Provider` からストリーミングされるトークンごとの `LLM_TOKEN` |
| `DEBUG` | 次に実行可能になるノードの集合を示す `__routing__` イベント |
| `VALUES` | スーパーステップごとに、完全な状態を持つ `__state__` イベント |
| `UPDATES` | `ChannelWrite` ごとの `CHANNEL_WRITE` イベント |
| `ALL` | 上記すべて |

```python
def cb(event):
    print(event.type, event.node_name, event.data)

engine.run_stream(
    ng.RunConfig(thread_id="t", input={...},
                 stream_mode=ng.StreamMode.EVENTS),
    cb)
```

> **注意：** `event.node` ではなく、`event.node_name` を使います。C++の構造体のフィールド名が `node_name` で、pybindはその名前をそのまま公開しています。

チャット形式のストリーミング（`content_so_far` が増えていく、LangChain互換のメッセージdict）には、ヘルパーを使います。

```python
from neograph_engine import message_stream

engine.run_stream(
    ng.RunConfig(thread_id="t", input={...},
                 stream_mode=ng.StreamMode.TOKENS),
    message_stream(lambda chunk: print(chunk["content"], end="", flush=True)))
```

### `asio::io_context.run()` の置き場所（C++）

C++から `engine.run_stream_async()` を動かす場合、外側の `asio::io_context.run()` は、アプリケーションのメインスレッド（または、通常のプロセス起動の流れで初期化された、寿命の長い任意のスレッド）から呼び出してください。動作確認済みの形は、次のとおりです。

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

> 過去のissue #16では、一部のglibc／OpenSSLの組み合わせで、リクエストごとに `io.run()` を入れ子にし、かつ旧来の子スレッドによるプロバイダーのストリーミングブリッジを使うと、`getaddrinfo` でSEGVが発生しました。このブリッジは、型付きインターフェースへの切り替え（typed cutover）で削除済みです。当時の構造テストの結果は、下流のHTTPS、サニタイザー、負荷の各条件を網羅的に再現したものではなく、現時点の適格性確認でもありません。現在の呼び出し側は、`ProviderMode` を明示して `invoke_async` / `dispatch_async` を使い、リクエストごとにイベントループを入れ子にするのではなく、既存の寿命の長いexecutorから処理を駆動してください。回避策として、偽のトークンを1つ作ったり、completionを再送したりしてはいけません。

---

## 8.5. Tracing — OpenTelemetry + Phoenix / Langfuse

`neograph_engine.tracing.otel_tracer` と `neograph_engine.openinference.openinference_tracer` は、グラフのイベントを、実行（run）とノードのスパンに変換します。後者は、スパンに `CHAIN` のタグを付け、ノードのペイロードの射影を記録します。グラフのコールバックは、実行ごとにどちらか1つを選んでください。モデル呼び出しを `LLM` スパンとして記録するには、グラフをコンパイルする前に、型付きプロバイダーを `OpenInferenceProvider(inner, tracer, *, span_name="llm.complete")` で包みます。このラッパーは、ネイティブのC++オブザーバーを使い、`prepare` と、一度だけ使える `dispatch` または `invoke` を継承します。リクエストを準備しただけ、あるいは破棄しただけでは、スパンは開きません。admission済みのdispatchは、スパンを開きますが、所有された結果、キャンセル、デッドライン、型付きイベントは変更しません。トレーサーが失敗しても、プロバイダーの結果や例外は置き換わりません。

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

ローカルのPhoenixエンドポイントを使うには、`docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix:latest` を実行し、`opentelemetry-api opentelemetry-sdk opentelemetry-exporter-otlp` をインストールします。`trace_graph` には、グラフ仕様、既存のプロバイダー、明示したモデル、`RunConfig` を渡します。`ParentContextTracer` は、実行のルートをワーカーのdispatchへ明示的に引き渡します。スレッドをまたぐ親の自動伝播や、ノードごとの親の自動伝播は保証していません。Pythonはdispatch時点でアクティブなOTelコンテキストを使い、準備済みのオペレーションは、寿命の間トレーサーのアダプターを保持します。

LLMスパンに含まれるのは、公開されているロールとテキストの射影、宣言済みのスカラー値、既知の使用量の件数です。既知のゼロは記録し、不明な値は省略します。ネイティブのリプレイやreasoning、生のワイヤーエンベロープとイベント、エンコード済みのリクエスト本体は、トレースに含めません。これらは、リクエストと結果の中に、本来の保管のまま残ります。失敗時の部分的なレポートも含め、使用量の属性は、ベンダーの課金やバジェットの権限を証明するものではありません。課金済みと予約済みの会計は、`UsageAccumulator.authority_snapshot()` とProgramの `provider_budget_authority` が扱います。

公開されるテキスト、例外メッセージ、グラフのペイロードにも、アプリケーションのシークレットが含まれることがあります。エクスポーターに渡す内容は、選別するかマスクしてください。詳しくは[OpenTelemetryの機微なデータの扱いに関するガイダンス](https://opentelemetry.io/docs/security/handling-sensitive-data/)を参照してください。`CHAIN` と `LLM` は、[OpenInferenceの規約](https://github.com/Arize-ai/openinference/blob/main/spec/semantic_conventions.md)で定義されています。NeoGraphが使う属性のサブセット、トークンイベント、Pythonでの型付き呼び出しの例、C++のライフタイムの要件は、[リファレンス](reference-en.md#105-observability--opentelemetry--openinference)にまとめています。

---

<a id="9-common-pitfalls"></a>
## 9. よくある落とし穴

どれも実際のユーザーが遭遇したものです。[`docs/troubleshooting.md`](troubleshooting.md)からも参照されています。

### 「ReActループが1回しか実行されない」

wheelのバージョンが0.1.7以下です。このバージョンのグラフコンパイラは、`conditional_edges` ブロックを黙って捨てていました。0.1.8以上にアップグレードしてください。`result.execution_trace == ['llm', 'dispatch', 'llm']` になれば直っています（`['llm']` で終わる場合は、まだ直っていません）。

### 「プロバイダー呼び出しが60秒止まったあと、エラーになる」

wheelのバージョンが0.1.6以下です。同梱されているOpenSSLに、RHELのCAパスがハードコードされており、Ubuntu／Debian／macOSにはそのパスがありません。0.1.7以上にアップグレードしてください（importの時点で `SSL_CERT_FILE` をcertifiのバンドルに自動設定します）。または、`SSL_CERT_FILE` を手動で設定してください。

### 「fan-outが想定より遅い」

`compile()` のデフォルトは `set_worker_count(1)` です。エンジンが所有するスレッドプールはなく、fan-outの分岐は、呼び出し側のexecutor上で直列に実行されます。本当に並列で実行するには、`engine.set_worker_count(N)` を呼び、Nを Send のfan-outの幅に合わせます。`hardware_concurrency()` の値でよければ、`engine.set_worker_count_auto()` を使います。プールを有効にしないまま、複数の Send によるfan-outを初めて実行すると、NeoGraphは標準エラー出力に警告を1回だけ出力します。これはヒントであり、エラーではありません。Pythonのカスタムノードは、小さなfan-outではGILの競合が起きるので、1とNの両方でベンチマークしてください。

### PythonのRunResultのstatusとstateを読む

`result.status` は、型付きのステータス `Completed`、`Interrupted`、`StepLimit`、`SafePoint`、`BarrierStalled`、`FailureExit` のどれかを返します。`BarrierStalled` は、バリアがもう発火できなくなり、実行が `__end__` に到達しないまま止まったことを、`FailureExit` は、failure-exitノードが完了して、実行が失敗として終わったことを表します。`result.output` は、これまでどおり、持ち運び用の最終状態です。`result.interrupted`、`result.max_steps_exhausted`、`result.execution_trace` は、その実行の様子を表します。`result.native_messages` と `result.provider_outcomes` は、型付きのプロバイダーのエビデンスをすべて保持します。詳しくは[Pythonバインディングガイド](python-binding.md#hitl-and-state)を参照してください。

### 「Unknown reducer: <name>」

組み込みのリデューサーは、`overwrite` と `append` の2つです。カスタムリデューサーは、コンパイルの前に、C++では `ReducerRegistry::register_reducer` で、Pythonでは `ng.ReducerRegistry.register_reducer` で登録してください。

### 「条件は登録したのに、条件付きエッジが発火しない」

書き方が、ローダーの受け付ける形式（[§4](#4-edges--conditional-routing)の形式Aか形式B）になっているか確認してください。どちらもv0.1.8以降で動作します。それより古いwheelでは、形式Bだけが動作します。

### 「execution_traceに開始ノードしか出ない」

ルーティングが `__end__` に抜けています。原因として最も多いのは、開始ノードから出るエッジがないことです。そうでなければ、条件が `routes` マップにない値を返し、明示した `"default"` ルートが `__end__` を指しています。strictなグラフは、マップの並び順でルートを選ぶことはなくなりました。openまたは未指定の条件では、`"default"` が宣言されていればそれを使い、宣言されていなければ、エンジンが、元のノード、条件、返されたラベルを添えて例外を投げます。closedな条件が、宣言されたラベル以外を返した場合は、必ず例外を投げます。

---

## 次に読むもの

- [Pythonのサンプル](../bindings/python/examples/)：ここまでの概念をすべて扱う、21本の自己完結したスクリプト。
- [C++のサンプル](../examples/)：同じ構成の36本のプログラム。
- [`reference-en.md`](reference-en.md)：クラスごとにAPIをすべて説明するリファレンス。
- [`ASYNC_GUIDE.md`](ASYNC_GUIDE.md)：非同期／コルーチン層の詳細な解説。
