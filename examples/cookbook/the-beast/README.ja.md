<!-- neograph-i18n: source=examples/cookbook/the-beast/README.md locale=ja source_sha256=c70c7b805d11a43a76fb1402e0b7ab7160eea9d0b9137fc779776b717d66c453 -->
# The Beast — 生成・進化・ロールバック

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

> 自己進化するエージェント。厳密なCore JSONとして自身のハーネスを書き、Coreコンパイラの下でそれを進化させ、チェックポイント機構を通じて実行を巻き戻す。**生成された。進化した。巻き戻された。野獣は残る。**

ほとんどの「エージェントフレームワーク」はグラフを*構築*させるだけだ。The Beastは静的ハーネスにはできない3つのことを行う。そしてそのすべては、この単一のプログラム内で**現実的で、オフラインで、決定論的**である（APIキーは不要）。

1. **生成する** — ランタイムに新しいハーネスを生成し、単一のノードが実行される前に結束していることを証明する。
2. **進化させる** — 実際のミューテーション演算子で進化させ、コンパイラ自体をフィットネスゲートとして使う。
3. **実行中のハーネスを**チェックポイント経由で任意の以前のスーパーステップに**ロールバック**します — リプレイではなく、真のタイムトラベルです。

それが安全なのは、NeoGraphにおいてハーネスは**データ**であり — 厳密なCore JSON (issue #56) で記述されたトポロジーであり — Coreコンパイラが実行前にハーネスの整合性を*証明できる*からにすぎない。厳密なCore JSONは交換成果物であり、第二のソース言語ではない。コンパイラこそが、そのモンスターを負債からカテゴリへと変えるものである。

## 実行してください

```console
$ cmake --build build --target cookbook_the_beast
$ ./build/cookbook_the_beast
```

```
── ACT I · generate a harness, prove it coherent ──
  ACCEPTED — strict compile and validation gates passed. Core nodes: s1_n s2_n s3_n
  (strict Core JSON is already the canonical interchange representation.)

── ACT II · evolve the harness (compiler = fitness) ──
  generations: 4 · offspring: 36 · survived compile gate: 36 · rejected (invalid, never run): 0
  sample mutations that produced offspring:
    gen 1: remove_edge: removed edges[0]        →  3 nodes
    gen 1: toggle_ce: added conditional edge from s2_n  →  3 nodes
    gen 1: toggle_barrier: added barrier on s3_n →  3 nodes
  (full diffable lineage via to_json(result) — the evolutionary rollback surface.)

── ACT III · spawn + roll back through the checkpointer ──
  ran to completion, trail = ["s1_n","s2_n","s3_n"]
  checkpoint timeline (3 snapshots):
    step 0  id=aac922ed  trail=["s1_n"]
    step 1  id=4b74daa9  trail=["s1_n","s2_n"]
    step 2  id=a528eb9d  trail=["s1_n","s2_n","s3_n"]
  >> ROLLBACK to step 1 (id=4b74daa9)
     final trail was ["s1_n","s2_n","s3_n"]; restored trail = ["s1_n","s2_n"]  (later steps gone)

Generated. Evolved. Rewound. The Beast remains.
```

## 第1幕 — 生成＋ゲート

The Beastはハーネスを厳密なCore JSONとして直接作成し、それを順番にコンパイラと検証ゲートに通す。いずれかのゲートで不合格になったハーネスは**破棄される**。

| ゲート | API | 捕捉するもの |
|---|---|---|
| 1. コンパイル＋TV | `GraphCompiler::compile` (strict, `schema_version: 1`) + `verify_roundtrip` | タイプミスまたは未サポートのキーは、黙って破棄されるのではなく、*ハードエラー*（消費キー会計）となる。翻訳検証は`canon(source) == canon(compile(source).to_json())`を検証する。 |
| **2. 验证** | `GraphValidator::validate` | 图**含义**：悬挂边（E3）、永远无法触发的屏障（E8）、未完成的路线图（E10）、信道效应违规（E4/E6）。 |

シードには、コアチェーン`s1_n → s2_n → s3_n`として配線された3つの明示的なノードが含まれている。

## 第二幕 — 进化 (编译器即适配度函数)

`neograph::graph::evolve()` (issue #80) は、シードに対して**実際の突然変異演算子**を実行する — `toggle_conditional_edge`、`toggle_barrier`、`add_edge`、および`remove_edge`。すべての子孫はまず**コンパイルゲートを通過する**：無効な子孫は実行されることなく、無料で死ぬ。棄却率自体が演算子の健全性指標となる。

突然変異空間は有界な厳密なCoreトポロジーであり、ソース言語ではない。子孫は標準の交換表現に留まり、コンパイラゲートはすべての子に対しても有効な状態を維持する。

各実行は、`to_json(result)`を介して差分可能な系譜を出力する：各個体の親、世代、突然変異、およびコアロックファイル。その系統**こそが**進化スケールでのロールバック面である — それをコミットし、差分を取り、世代全体を元に戻せ。

## 第III幕 — ロールバック（チェックポイントによるタイムトラベル）

生存したハーネスは、`InMemoryCheckpointStore` が `EngineConfig::checkpoint_store` を通じて接続された状態で生成される。エンジンは各スーパーステップの終了時に状態のスナップショットを取得する。その後:

- `store->list("beast-run")`は完全なタイムラインを返す — `trail`がステップごとに1ノードずつ成長するのを*見る*ことができる。
- `store->load_by_id(earlier.id)`は、以前のステップでの正確なチャネル状態を**復元**する。デモは`["s1_n","s2_n","s3_n"]`から`["s1_n","s2_n"]`へロールバックする — 後のステップは本当に消えている。これは`load_by_id` / `load_latest`のタイムトラベルであり、HITL割り込み/再開とスレッド分岐が構築されているのと同じ機構である。

## 本番稼働 — モデルが実際にハーネスを書く

`the_beast.cpp`はオフラインである（スタブ作成者）。[`the_beast_live.cpp`](the_beast_live.cpp)が本物である：ライブLLMに`NodeFactory::export_schema()`（このエンジンビルドが受け入れる正確なパレット — エンジンのスキーマそのものであるため、ドリフトできない。[`../../52_export_schema.cpp`](../../52_export_schema.cpp)を参照）が渡され、厳密なCore JSONとしてハーネスを作成するよう求められる。返されたものは何であれ、同じコンパイラと検証ゲートを通過する。棄却時には、診断情報が会話に直接フィードバックされ、モデルが書き直す — 真の自己修復ループである。

```console
$ echo 'OPENROUTER_API_KEY=sk-or-...' >> .env      # DeepSeek V4 Flash 0731 via OpenRouter
$ cmake --build build --target cookbook_the_beast_live
$ ./build/cookbook_the_beast_live                  # optional: pass a task string as argv[1]
```

`the_beast_live.cpp`は`~deepseek/deepseek-v4-flash-latest`を`provider: {"zdr": true, "only": ["morph"], "allow_fallbacks": false}`に固定する。検証時点で、OpenRouterはMorphのデータセンターを米国としてリストし、そのモデル/プロバイダエンドポイントをZDR対応としてリストしていた。これは厳密なプロバイダ選択であり、OpenRouterのリージョン内常駐保証ではない：その文書化されたリージョン内保証は現在、エンタープライズEUルートである。Morphの適格エンドポイントが利用できない場合、プロンプトを別のプロバイダに送信するのではなく、リクエストは失敗する。

ライブクックブックはプロバイダーのタイムアウトを180秒に設定しています。この推論モデルの4,000トークン生成予算は、一般的な60秒のデフォルトを正当に超える可能性があります。



```
── Attempt #1: asking the model to write a harness ──
  model returned 663 chars of JSON.
  ACCEPTED — strict compile and validation gates passed.
  Core lockfile nodes: r_stage c_stage s_stage

── Spawning the model's harness (checkpointed) ──
  ran to completion, trail = ["r_stage","c_stage","s_stage"]
  checkpoint timeline (3 snapshots): ...
  >> ROLLBACK to step 1: restored trail = ["r_stage","c_stage"]

The model wrote it. The compiler proved it. The Beast ran it.
```

**ライブ実行で示されたもの**（DeepSeek v4 flash）：線形パイプライン、ダイヤモンド型fan-out / バリア型fan-in、条件付きルーターにわたって、初回で*首尾一貫した*ハーネスを作成しました。自己修復ループは武装されていますが、有能なモデルがそれを引き起こすことはめったにありません。それでもゲートはlintersとして価値を発揮しました꞉ diamond のバリア欠落（E9）とルーターの到達不能ハンド imperative（E7）を警告としてフラグ付けしました。

ここでのノードは決定的な`beast_node`ワーカーであるため、ライブ実行は1回のLLM呼び出し（作成）で済み、実行は無料である。それらを`llm_call`に置き換えると、各ノードもライブ呼び出しになる。

## Copy Ninja — 検証済みのローカル機能はグラフノードになります

[`the_beast_copy_ninja.cpp`](the_beast_copy_ninja.cpp) は、1つの狭いcapability-to-harnessパスを実行可能にする。これはA2Aカードをコードに変えるものでは**ない**：

1. 合成されたloopbackバックサーバーは、ある既知のエージェントカードを公開するだけです；collectionはまさにそのGETを実行し、カードの宣伝されたRPC URLを決してフォローしません；
2. `AgentCardCandidateCompiler` は、自由形式のカードテキスト、エンドポイント、認証情報、実行可能ソースを除外した、不変かつ**unadmitted**な記述子を生成する；
3. 独立して供給されるダイジェスト固定の行動プロファイルが、唯一の`copy-ninja.hello-world-echo.v1`テンプレートを検証し、それをローカルの`CopyNinjaNode`として具体化する；そして
4. ライブBeastは、2チャンネル・1ノードのトポロジーのみをオーサリングする。通常の厳格なコンパイル/ラウンドトリップ→検証ゲートが最初に実行される。その後、4番目のローカルバインディングゲートが、正確に `copy_ninja_local` の間で `__start__` と `__end__`.

呼び出し元のプロンプトはLLMメッセージから意図的に除外されています。モデルはトポロジーを作成し、ローカルグラフだけがプロンプトを消費します。また、合成ソースサーバーがRPCを観測した場合も実行は失敗します。これは、固定されたローカル動作の証拠であり、ソースコード転送、委任、admission、または一般的な動作等価性の証拠ではありません。

```console
$ cmake -S . -B build -DNEOGRAPH_BUILD_LLM=ON -DNEOGRAPH_BUILD_A2A=ON
$ cmake --build build --target cookbook_the_beast_copy_ninja
$ ./build/cookbook_the_beast_copy_ninja "Grace"
```

2026-08-08に観測されたライブ結果：作成モデルは初回試行で4つのゲートすべてを通過した；グラフは、1回のディスカバリGETとゼロ回のソースエージェントRPCで`Hello, World! I have received your request (Grace)`を返した。

## Apex — ハーネスはツールを食い尽くす

スタブワーカーのデモは、生成されたハーネスが*コヒーレント*であることを証明するが、ハーネスは決して動作しない。[`the_beast_apex.cpp`](the_beast_apex.cpp) が怪物である：モデルは**ツールカタログ**を渡され、ReActエージェントを作成するよう求められる — `llm_call` ⇄ `tool_dispatch` が`has_tool_calls`上でループする。それが書くハーネスはコヒーレンスのためにゲートされ、その後**ツールをバインドした状態でスポーンされる**（`ctx.tools` + `engine->own_tools`）。スポーンされたエージェントは、どのツールをいつ呼ぶかを独自に決定する。

```console
$ cmake --build build --target cookbook_the_beast_apex
$ ./build/cookbook_the_beast_apex "What is 23 * 19, and the weather in Seoul?"
```

実際の実行 — セルフリペアループが実際に作動し、その後自律的なツール呼び出しが行われる：

```
Tool catalog offered: calculator get_weather

── Attempt #1: model authors a tool-calling agent ──
  REJECTED at 'compile': ... unknown or unconsumed key 'id'    (strict, schema_version 1)
  → feeding diagnostics back for self-repair.
── Attempt #2: model authors a tool-calling agent ──
  REJECTED at 'compile': ... unknown or unconsumed key 'name'
  → feeding diagnostics back for self-repair.
── Attempt #3: model authors a tool-calling agent ──
  ACCEPTED — coherent tool-calling agent. Nodes: agent(llm_call) tools(tool_dispatch)

── Spawning the agent it wrote — live, tools bound ──
  user task: What is 23 multiplied by 19, and what's the weather in Seoul?
  [the harness is calling tools autonomously]
    tool → {"result":437.0}
    tool → {"weather":"19C, clear"}
  tool calls executed by the harness: 2
  final answer: 23 × 19 = 437; Weather in Seoul: 19°C, clear.

The model wrote the agent. The compiler proved it. The agent ate the tools.
```

これが1回の実行における論文全体である：モデルは`nodes`スキーマを2回幻覚し（`id`、次に`name`キーを追加）、厳格なコンパイラの**消費キー会計が両方を拒否した** — 診断は会話に戻され、3回目の試行で自己修復した。その後、機械作成・コンパイラ検証済みのエージェントがライブのReActループを実行し、2つのツールを自律的に呼び出した。創造性は無制限、ツール使用は自律的、**コヒーレンスは交渉の余地がない。**

## Forge — ツールがないときは、それを書く

[`the_beast_forge.cpp`](the_beast_forge.cpp) は頂点にツールサプライチェーンを加えたものである。タスクが与えられると、次のことを行う：

1. **DISCOVER** — 標準のMCP stdioサーバーをスポーンし、実際のMCPプロトコル上でそのツールを一覧表示する（`MCPClient::get_tools`）。
2. **FORGE** — タスクに必要な機能がカタログにない場合、アーキテクトLLMがそれを実装する**Python MCPサーバーを記述**します。それをディスクに具体化し、起動し、MCP経由で新しいツールを**再ディスカバー**します。(生成したサーバーの初期化に失敗した場合は自己修復します。)
3. **AUTHOR** — *結合された*カタログ上でReActエージェントを記述します。3つのゲート+自己修復は常に適用されます。
4. **SPAWN** — ディスカバー*および*フォージされたすべてのツールをバインドし、エージェントを実行します。エージェントはそれらを自律的に呼び出します。

実際の実行 — モデルが不足しているツールを記述し、エージェントがそれを使用しました:

```
── DISCOVER · stock MCP server ──
  tools: get_current_time calculate get_weather

── FORGE · the model writes a Python MCP server for what's missing ──
  attempt #1: wrote 5225 bytes → /tmp/beast_forged_server.py
  FORGED + re-discovered over MCP: reverse_string

── AUTHOR · the model writes a ReAct agent over the full catalog ──
  #1 REJECTED at 'compile': ... unknown or unconsumed key 'id' → self-repair.
  ACCEPTED — coherent agent: agent(llm_call) tools(tool_dispatch)

── SPAWN · run the agent it wrote, tools bound ──
  [harness dispatching tools autonomously]
    tool → retsnom                         # the forged reverse_string
    tool → 2026-07-10 06:13:21 (UTC)       # the discovered get_current_time
  final answer: Reversed 'monster' → retsnom; current UTC time is 2026-07-10 06:13:21.

It discovered tools, forged the missing one, and used them all.
```

2つのライブMCPサブプロセス（1つは標準、もう1つはBeastが*この実行で*書いたもの）、それぞれに実際の`tools/list`、実際のReActループ。リモートなのは作成モデルのみである。

### カスタム*ノード*も定義できますか?

正直に言うと：NeoGraphノード**タイプ**は`NodeFactory::register_type`を通じて登録されるC++クラスである — ランタイムに真新しいアトミックC++ノードタイプをJITコンパイルすることはできない。しかし、その意図はBeastがデータから駆動できる3つの方法でカバーされている：

- **複合ノード** — 明示的なCoreノードとエッジにより、モデルは再利用可能なトポロジー単位を純粋にデータで定義できる；これはまさに`the_beast.cpp`のシードが行うことである。
- **再帰** — `subgraph`ノードはハーネス全体を1つのノードとして埋め込むため、Beastが作成したハーネスはBeastが作成したサブハーネスを含むことができる（Nレベルの自己増殖）。
- **コードによるカスタム動作** — 上記のforgeパターンは、モデルが作成したランタイム動作そのものである：モデルが書いたツールがディスパッチ可能なユニットになる。同じトリックは、汎用の`script_node`型（モデルが書いたコードを実行する事前登録済みC++ノード）に一般化され、これが「LLMがロジックを定義した新しいアトミックノード」を得るための正直な方法である。

真に不可能なのは、実行時に新しい*コンパイル済みのC++ノードクラス*を発行することだけです。モデルが動作を特化するために必要なものはすべて、コンパイラが既にゲートをかけるシード/スクリプト/サブグラフ表面に含まれています。

## スクリプト — ユニバーサルカートリッジ(モデルが書いたノードロジック+フロー)

上記のすべてのバリアントは、モデルが*ツール*（リーフ機能）を作成することを可能にする。[`the_beast_script.cpp`](the_beast_script.cpp)は、モデルが**ノードロジック — ツールが決して表現できない制御フロー（`goto`）を含む —** を作成することを可能にする。`script_node`は、設定にモデルが書いたPythonを保持する1つの事前コンパイル済みC++ノードであり、`run()`でチャネル状態をノードに渡し、コードが返すもの（`{writes, goto, sends}`）をグラフに適用する。モデルはノードの動作*と*グラフのフローを、再コンパイルなしでデータとして定義する。

コヒーレンスは交渉の余地がないままである。スクリプトは設定でその契約を宣言する（`reads` / `writes` / `goto_targets`）；ハーネスは厳格なCoreコンパイラ/検証ゲートに加えて、Beast層の**契約チェック**（宣言された書き込みは宣言されたチャネルでなければならず、gotoターゲットは実在のノードでなければならない）に加えて、宣言外の書き込み/gotoを拒否する**ランタイムラッパー**を通過させる。これにより、**NeoGraphコアへの変更ゼロ**でBeast層の効果/ルート保証が復元される — 追加的かつ後方互換性がある。

```console
$ cmake --build build --target cookbook_the_beast_script
$ ./build/cookbook_the_beast_script --selftest   # offline, no API key
$ ./build/cookbook_the_beast_script              # live: DeepSeek writes the node logic
```

ライブ実行 — モデルは、その制御フローが独自の`goto`であるカウンターループを書いた：

```
── Attempt #1: model writes node logic ──
  ACCEPTED — coherent, and the script's write/goto surface is contract-checked.

── Spawning — the node's own code drives the loop via goto ──
  [tick #1 — script decides: continue or exit]
  [tick #2 — script decides: continue or exit]
  [tick #3 — script decides: continue or exit]
  trace: tick -> tick -> tick -> END
  final counter = 3  (the model's goto logic ran the loop, contract-enforced)
```

`tick`からの静的エッジは存在しない：ループは、カウンタが3に達するまでモデルのPythonが`{"goto": "tick"}`を返し、その後`{"goto": "__end__"}`を返すためにのみ存在する。`--selftest`は、APIキーなしで缶詰ハーネスから同一のメカニズムを実行するため、CIはオフラインでそれを実行できる。

**境界（正直に）。** コンパイラはグラフの*形状*を証明する；契約はノードの*表面*（触れる可能性のあるチャネル/ターゲット）を証明する；スクリプトの*内部ロジック*だけが未証明であり、サブプロセスの`timeout`と実行の`max_steps`によって制限される。モデルが書いたコードの実行は任意コード実行である：ローカルでユーザー駆動のクックブックには問題ないが、本番環境ではインタープリタの周囲にサンドボックスが必要である。これは**ビルドオプション**であり、デフォルトではオフである：

Sandboxed-apiはFetchContent経由での埋め込みがうまくいかないため、事前構築されたツリーをリンクする（ビルドレシピはオプションの上のCMakeコメントに記載）。

```console
$ cmake -S . -B build -DNEOGRAPH_BEAST_SANDBOX=ON -DSANDBOX2_SRC=/path/to/sandboxed-api
$ cmake --build build --target cookbook_the_beast_script
```

オンにすると、pythonはGoogle **Sandbox2** の下で実行される — 独自のユーザー/pid/mount/net名前空間、インタープリタと2つの作業ファイルに限定された読み取り専用FSビュー、およびCPU/ウォール/ファイルrlimit。`libcap-dev`、`libunwind-dev`、C++20ツールチェーンが必要；Linux/WSL2で検証済み。

**効果契約から合成されたSeccompポリシー。** Pythonのsyscallフットプリントは安全に許可リスト化するには大きすぎるため、デフォルトのアクションは許可のままである — しかし、ノードの宣言された*機能*はsyscallを差し引く：`"net"`機能を宣言しないノードは、`socket`/`connect`/`bind`/…がseccompブロックされる（EPERM）；`"exec"`機能がない場合は`execve`/`execveat`がブロックされる。ポリシーは*宣言された契約から導出*され、手書きではない。これはネガティブテストで検証された — 宣言されたcapのみが異なる、**同じ**サンドボックス下の**同じ**python：

```
caps=[]     (no net cap): {"socket": "SOCKET_BLOCKED:EPERM"}   # seccomp denies the syscall itself
caps=[net]  (net cap):    {"socket": "SOCKET_CREATED"}         # capability grants it
```

したがって、これはネットワーク名前空間以上のものである：`net` capがない場合、`socket()` *syscall*は失敗する（netnsの上での多層防御）。正直な範囲：これは**コンテナグレード + 契約導出のseccompブロックリスト**であり、完全なsyscall許可リストではない — ブロックされていないsyscallを介したカーネルエクスプロイトは依然として封じ込められない。より厳格なノードごとの許可リスト（および機能ベースのシークレット仲介）が、文書化された次のステップである。

## Evolve — memetic（ダーウィンの想定 + ラマルク的）

オフラインの`the_beast.cpp`は意図的に`run_evaluation=false`を設定するため、その選択は構造のみである。汎用進化APIは、タスクを実行し、期待される正確なチャネル値をスコアリングすることもできる。

[`the_beast_evolve.cpp`](the_beast_evolve.cpp)は代わりにカスタム連続距離メトリックを使用する：ニアミスは、汎用スコアラーの単一の出力不一致クラスを受け取るのではなく、数値ターゲットに向かって改善できる。

- **タスク**（本物であり、出力でスコアリングされる — 構造的な代理ではない）: 目標数を計算する算術パイプラインを組み立てる。5つの演算ノードが存在し — `add2(+2) add3(+3) mul5(*5) mul2(*2) sub1(-1)` — 各ノードは`acc`チャネル（初期値0）を読み取り、自身の演算を適用し、書き戻す。ハーネスの答えは実行後に`acc`が保持する値であり、**fitness = `-(|acc - 20|)`**。*トポロジー*（どの演算がどの順序で実行されるか）が数値を決定するため、配線を進化させることは計算を進化させることになる。
- **ダーウィン的**: ランダムな再配線（`all_operators()`）+ 測定された出力による選択 — 20に向かってよろめきながら進む。
- **ラマルキアン**: LLMが算術を行い、20に正確に到達するチェーンを配線し、その獲得した解決策を遺伝可能なシードとして注入する。

```console
$ ./build/cookbook_the_beast_evolve --darwin-only   # offline, deterministic
gen 0  seed acc=5   fitness -15
gen 1  best acc=10  fitness -10  (mut)
gen 2  best acc=24  fitness -4   (mut)   # overshoot
gen 6  best acc=19  fitness -1   (mut)
gen 9  best acc=20  fitness -0   (mut)   → Solved
champion: acc=20, origin 'mut'. Pure Darwinian mutation + selection.

$ ./build/cookbook_the_beast_evolve                 # + Lamarckian (needs OPENROUTER_API_KEY)
gen 2  best acc=24  fitness -4  (mut)
gen 3  [Lamarckian] LLM refinement acc=20  fitness -0  → injected (heritable)
       Solved via Lamarckian injection.
champion: acc=20, origin 'LLM'. The winner is a Lamarckian acquired trait ...
```

対比こそが要点である: **ブラインド突然変異は目標に向かって試行錯誤しながら進む**（第9世代までに5→10→24→19→20、試行によって数を計算する）; **LLMは算術を行う** — `(0+2)*5*2 = 20` — そして注入されたときに直接答えに到達する。獲得された解は遺伝可能なチャンピオン（`origin 'LLM'`）になるため、これはラマルク的である; ブラインド変異 + 選択はダーウィン的であり、両方を実行することはミーム的アルゴリズムである。

正直な注記: 純粋なダーウィン的アプローチはオフラインで検証され、決定的である。ラマルク的LLM呼び出し（deepseek-v4-flash）は**時折不安定である** — ストリームされた応答が解析不能で返ることがあり、その場合、実行は`[Lamarckian] LLM returned no parseable harness`を記録し、ダーウィン的アプローチがラウンドを担う; 最終行はチャンピオンの*実際の*起源を報告し、実際には起こらなかったラマルク的勝利を決して報告しない。

## Gate-eval — コヒーレンスゲートは実際に健全か？

ビーストの安全論全体の主張は、静的検証器が*健全*なコヒーレンスのオラクルであるということである：ERRORはハーネスが自らランタイムに障害を惹起することを意味し、エラーなしが実行を意味する、それは*主張されたが、測定されなかった*。——レビューアーが最初に尋ねたことである。

[`the_beast_gate_eval.cpp`](the_beast_gate_eval.cpp)がそれを測定する。ラベル付きトポロジーのコーパスをバリデータ（予測判定）とエンジン（グラウンドトゥルース）の両方に通し、相互検証する。オフライン、決定的、APIキー不要 — すべての判定が実行と一致した場合に限り`exit 0`、したがって**CIは健全性でゲートを掛けられる**。

```console
$ ./build/cookbook_the_beast_gate_eval
case                     | validator      | runtime | sound?
coherent                 | ok             | CLEAN   | yes
E4-undeclared-write      | ERROR:E4       | FAULT   | yes   # reject ⇒ genuine runtime throw
E3-dangling-edge         | ERROR:E3       | FAULT   | yes
E7-unreachable(warn)     | ok             | CLEAN   | yes   # a warning does NOT reject a correct graph
E10-empty-routes         | ERROR:E10      | not run | yes   # dispatch is UB by design — the gate stops it
runtime cross-check: 4/4 cases where the validator's verdict matched execution.
```

テスト対象の性質:

> バリデータがERRORを報告する ⟹ グラフは実行時にフォールトが発生する; バリデータがエラーを報告しない ⟹ グラフはクリーンに実行される。

最初の行は*健全性*である（エラーがフラグされたグラフがクリーンに実行されたなら、それは健全性の穴になる）; 警告がフラグされたグラフがクリーンに実行されることは、ゲートが*過剰に*拒否しないことを示す。E10/E8クラスのエラーは判定のみである — 空のルートマップを実行すると`rend()`（UB）を参照外しすることになり、これはまさにゲートが防ぐために存在するフォールトであるため、チェックされるが実行はされない。これはデモンストレーション用コーパスであり、すべての診断を網羅するものではない — しかし「ゲートは健全である」をスローガンから、測定されCIで強制される4/4へと変える。

## ゲートファズ — 保証とその境界、大規模に

[`the_beast_gate_fuzz.cpp`](the_beast_gate_fuzz.cpp)はgate_evalを5つの手動ラベル付きケースから数千のファズ化ケースへと押し上げる — しかし正直に。素朴な手法（N個のグラフをファズし、精度1.0を出力する）は見せかけになるだろう: **エンジンはコンパイル時にバリデータを再実行し、任意のエラーでスローする**ため、「バリデータエラー ⟹ エンジンフォールト」は*構造上*真である。したがって、このプログラムは実際に有益な2つのことを測定する:

```console
$ ./build/cookbook_the_beast_gate_fuzz 2>/dev/null   # lint → stderr
LAYER 1 — static gate vs engine over 2000 honest-contract mutants:
  gate rejected 1586, gate passed 414;  agreements 2000, DISAGREEMENTS 0
  runtime faults AFTER the gate passed (soundness holes): 0
LAYER 2 — a node that LIES about its effect contract (500 mutants):
  static gate PASSED (blind to the lie): 500/500
  runtime GraphState guard FAULTED (backstop caught it): 500/500
CI gate (Layer 1: 0 disagreements over 2000; Layer 2: runtime backstops 100%): PASS
```

- **レイヤー1 — 大規模での一貫性。** ランダムな構造ミューテーター（ぶら下がりエッジ→E3、未宣言書き込み→E4、孤立ライター、ドロップされたエッジ→E7 *警告*、追加の有効エッジ）で一貫したシードをファズします。2000以上のミュータントにわたって、コンパイラゲートとエンジンは決して不一致になりません。これは健全性の*発見*ではありません（部分的に構造上そうなっています）— これは**回帰保証**です：将来の変更が静的ゲートとランタイムを乖離させた場合、これは失敗します。
- **レイヤー2 — 境界。** ゲートは各ノードの宣言された**エフェクト契約**を信頼する。*嘘をつく*ノード — `writes:["out"]`を宣言するが、実際にはランタイムで未宣言の`phantom`チャネルに書き込む — は静的ゲートを容易に通過し（500/500）、**ランタイム`GraphState`書き込みガード**がすべてを捕捉する（500/500）。これはゲートのバグではなく、設計された分業である。

結果は保証の*正確な*記述であり、疑わしいほど完璧な混同行列よりも誠実です：**静的ゲートは誠実な契約に対して健全であり、不誠実な契約にはランタイムのバックストップがある** — レイヤー1とレイヤー2はそれぞれCIで強制されます。

形式的な伴走文書、[`SOUNDNESS.md`](SOUNDNESS.md)はこれを*証明*する: スーパーステップ実行の小ステップ意味論、エフェクト束`(𝒫(Chan), ⊆)`、整形式性判定`⊢ G ok`としてのゲート、および（正直な契約のもとでゲートを通過するグラフが決してフォールトしないという）Progress定理。正直性の仮定が必要条件であることが証明され、ランタイム書き込みガードがそのフェイルクローズのバックストップとなる。すべての前提はエンジンソースに対してチェックされ、`gate_eval`/`gate_fuzz`がモデルの忠実度チェックである。ここでの2つのハーネスは、その文書の系6.4と命題6.5を実行したものである。

## バルドウィン — ミーム的適応は盲目的適応に勝るか、そして継承は重要か？

`evolve`変種はダーウィン的突然変異 + ラマルク的LLM注入を示した。すべてのレビュアーが提起したより鋭い研究課題: **ブラインド進化とワンショットソルバーの両方が停滞するが、ミーム的アルゴリズムの組み合わせが勝つタスクは存在するか — そして学習された形質を*どのように*継承するかが、文献が予測するように結果を変えるか？**（Whitley 1994; Hinton & Nowlan 1987。）

[`the_beast_baldwin.cpp`](the_beast_baldwin.cpp)がその実験であり、実際のNeoGraphハーネス上で実行される。ゲノムはアフィンパイプラインの配線であり、各ステージは演算にコミットされる**または生涯学習が解決するために可塑的なまま残される（`?`）**。適合度は**実行時の組み立てられたハーネス**のシグネチャであり — 起動時の相互検証は、高速な解析的適合度が200のトポロジーでコンパイル済みエンジンのものと等しいことを証明する（gate-evalと同じ規律）。ランドスケープは**欺瞞的**である: どこにでも見える広いデコイ丘（0.85）と、可塑性遺伝子が張る近傍を探索する*学習*だけが見つけられる、狭く**勾配のない**大域的高原（1.0）。

```console
$ ./build/cookbook_the_beast_baldwin          # offline, deterministic, no key
engine/analytic cross-check: 200/200 topologies execute exactly as modeled → real harness.
  Darwinian  | assimilated global  6/24 | mean committed → global  25%  decoy  70%
  Baldwinian | assimilated global 21/24 | mean committed → global  74%  decoy  17%
  Lamarckian | assimilated global 23/24 | mean committed → global  78%  decoy   9%
CI gate (blind Darwin near the 25% chance floor, learners assimilate >65% by a
  >25-pt margin, faithful fitness): PASS
```

適合度 fitness が何であるかについての注記: 各ゲノムは実際の NeoGraph トポロジーにコンパイルされ、クロスチェックにより、エンジンがそれらのうち200個を解析モデルが予測するとおり正確に実行することが証明される — 基質 substrate は、本物で忠実に実行されるハーネスである。GA が最適化する目的 objective は、配線上の欺瞞的なハミングランドスケープ (ダイナミクスを制御されたテストベッド) であり、生の実行出力ではない。両方の事実は、曖昧にされるのではなく、明確に述べられている。

2つの所見は、異なる基準で保持されます。

1. **メメティックは盲目的進化を上回る（頑健 — CIでゲート制御）。** 盲目的ダーウィン進化は大域を約25%しか獲得しない — これは偶然の下限である — というのも台地にはコミット済み空間の勾配がないため、選択はデコイを追従して閉じ込められるからである。学習は台地を露呈させ、それを約75%獲得する。このゲートは*margin*を主張する（24シードにわたる平均）ものであり、per-runしきい値カウントではない — per-runカウントは初期運によって動かされ、25%対75%という差（margin）が安定したシグナルである。
2. **バルドウィン制御（測定のみ — 決してゲートしない）。** バルドウィン（学習した形質を継承しない）対 ラマルク（それをゲノムに書き込む）：ここではグローバルに **74% 対 78%** — ラマルク方式がわずかに先行、これは **deceptive だが adversarial ではない**ランドスケープにおける *予想される* 結果である（write-backの速度がその多様性コストを上回る）。Whitleyの**逆転**（Baldwin > Lamarck）には特別に adversarial なランドスケープが必要であり、この単純な二峰性写出構造manualでは堅牢にそれを示すことができない。「loose」という整合的に報告されており、人為的な偶然に合わせて調整されたわけではない。（これは本当に繊細な問題である：インデックスベースのタイブレークを用いた初期バージョンは逆転を *見かけ上* 示したが、それこれは artifacts であり。選択境界での同率は now seed per-seed の随机抽選によって break され、スイープ全体で average され、その ~74 対 78 の順序は seed series を通じて stable で、見かけの逆転はその修正をただすことがてきなませんでした。）

This is the honest shape honest shape of the result the reviews ask for: the robust claim(l langavec mediating evolution(teache decides) が解決実際 cannot; which is core, measured, CI-enforced; 99 delicate claim (non-inheritance beats inheritance, inherited concept) は測 exhibits 、報告報告("measured", 報告"as-is") with negative outcome. outcome named 明記され.

## Baldwin-adv — 敵対的ランドスケープ + 実際のヒルクライム学習

[`the_beast_baldwin_adv.cpp`](the_beast_baldwin_adv.cpp)は、前回の実験の両側面を鋭くする。学習は今や**真の局所探索**（可塑性遺伝子に対するマルチリスタート山登り法で局所最適解へ到達する——リファイナの離散的類似物であり、LLMが差し込まれるスロットである）であり、ランドスケープは真に**敵対的**である：広いデコイ丘は、その勾配が小さく急峻な大域的な球から*遠ざかる*方向を指す。盲目的なコミット空間探索は偶然の下限にはない——それはデコイ勾配に沿って能動的に**欺かれている**。

```console
$ ./build/cookbook_the_beast_baldwin_adv        # offline, deterministic, no key
  Darwinian  | committed → global   5%   decoy  92%
  Baldwinian | committed → global  76%   decoy  19%
  Lamarckian | committed → global  98%   decoy   1%
CI gate (blind deceived onto decoy >50%, both learners solve >60%, faithful): PASS
```

- **Memetic ≫ blind（ロバスト、CIゲート付き）。** Darwinはデコイに騙される（グローバル約5% / デコイ約92%）。学習により、単一ゲノムでは到達できないグローバルボールに到達する（76〜98%）。これはプラトーよりも強い分離である——ブラインドベースラインは誤誘導されており、単に盲目なわけではない。シードベース全体で安定（Darwin 2〜5%、学習者76〜98%、生物分母：あり）。
- **Baldwin vs Lamarck：逆転は再現されません。** Lamarck的書き戻しが安定したマージン（98% vs 76%）で勝ちます。到達可能/到達不可能の境界全体にわたるパラメータスイープ（30以上の構成、3回のスイープ）では、非継承が書き戻しを堅牢に上回る**レジームは見つかりませんでした**：グローバルが到達可能な場合、書き戻しの速度が支配します；到達不可能な場合、両方とも失敗し、Baldwinの多様性の優位性はわずか（約3〜4ポイント）です。これは「WhitleyのBaldwin > Lamarck逆転はハーネストポロジー上で再現するか」という問いに対する誠実な経験的答えです — **いいえ**、この離散レジームでは、プログラムはメカニズムを名前付きでそう述べています。（Whitleyの逆転は、実数値の局所探索を持つ*連続*多峰関数で確立されました；ここでの離散トポロジーGAはそれを示しません。）

## Baldwin-llm — モデル自身が学習オペレーターである

上記の機械的学習者（ランダム推測、山登り法）は常に、LLMリファイナが差し込まれる*スロット*であった。[`the_beast_baldwin_llm.cpp`](the_beast_baldwin_llm.cpp)は、モデルが実際に推論できるタスクでそれを差し込む：算術パイプラインの`?`段階を埋めて、`acc`がターゲットに到達するようにする。**学習演算子はモデルである**（`?`段階の演算を選択する）；適合度は組み立てられたハーネスの*実行*である。Baldwin/Lamarckの切り替えは文字通りになる：

- **Baldwin的**はモデルの埋め込みをスコアリングするが、遺伝子`?`は保持する——モデルは次世代で**再び**参照されなければならない。学習は継承されない。
- **Lamarck的**は埋め込みをゲノムに書き込む——`?`はコミットされる。獲得形質は**遺伝可能**である；モデルは再び参照される必要はない。

```console
$ ./build/cookbook_the_beast_baldwin_llm       # oracle learner (default, offline)
  Baldwinian (learner = oracle):
    gen 0: … committed genes 16/24 | learner calls 5
    gen 3: … committed genes 10/24 | learner calls 6      # re-learns every gen
  Lamarckian (learner = oracle):
    gen 0: … committed genes 24/24 | learner calls 5
    gen 3: … committed genes 24/24 | learner calls 0      # banked; no re-learning
total learner invocations: Baldwin 23 vs Lamarck 5  (Lamarck banked its way to fewer)
```

観察可能な違いは適合度ではなく（両方ともターゲットに到達する）、**ゲノム経済**である：Lamarckは学習者の作業を遺伝に銀行化する（遺伝子はコミットし、呼び出しはゼロに落ちる）；Baldwinは毎世代再学習する（遺伝子は可塑性のままで、呼び出しは高いまま）。デフォルトでは決定的な**オラクル**学習者でオフライン実行するか、`--llm`と`OPENROUTER_API_KEY`で**モデル**を学習者にする——その場合、それらの呼び出しは実際のAPI呼び出しであり、遺伝は文字通り、モデルに一度支払うことと毎世代支払うことの違いである。これが「モデルの修正は遺伝可能になるか？」の具体的な意味である——主張ではなくトレースとして示される。（`--llm`パスはネットワークを必要とし、呼び出し/パース失敗時にはオラクルにフォールバックしてログを取るため、デモは常に完了する。）

## Novelist — 前提を入れると、ライトノベル長の`.txt`が出てくる

最も単純で真に有用なライティングハーネスであり、「NovelWriter」アイデアの正直な形である：前提を与えると、プレーンテキストとしてライトノベルサイズの原稿全体が返ってくる。[`the_beast_novelist.cpp`](the_beast_novelist.cpp)は**lost-in-the-middle**の具体的な治療法である——長い物語は*一つの巨大なコンテキストで書かれるわけではない*。それは**明示的なストーリー状態**上の小さなグラフである：

    channels:  premise · outline · bible · summary · book · idx · total

各章は**コンパクトな外部化状態に対して新たに生成される**（アウトラインのビート、ストーリーバイブル、実行中の要約）ので、60k文字を再読する代わりになる。モデルは小説全体にわたってキャラクターが誰かを*覚えている*必要はない——`bible`チャンネルを読むだけである。

```console
$ ./build/cookbook_the_beast_novelist "a librarian's returned books whisper futures" 12
harness passed the coherence gate. writing (live — this takes a few minutes)…
  … chapter 1/12 written (4180 chars)
  …
done — 51k characters across 12 chapters.
manuscript: /abs/path/novel_12ch.txt
```

グラフは`__start__ → planner → writer ⟲`である：`planner`は前提をアウトライン＋初期バイブルに変える；`writer`は章`idx`を`book`に書き込み、**`summary`と`bible`を更新**して次の反復が接地されたままになるようにし、その後**`Command` gotoで自己ループ**して`idx+1 == total`まで続く。エフェクト契約が宣言されているので、**コヒーレンスゲートは一言書かれる前に配線を証明する**——すべてのストーリー状態チャンネルが実際に消費され、ぶら下がった段階はない。

**バッチ生成、バッチ進化——章ごとに異なる感触。** 各章は事実上、孤立したサブエージェントである（共有ストーリー状態によってのみ接地された新しい`writer`呼び出し）。それらが同じものを読まないようにするため、ライターは章ごとに**スタイルゲノム**を進化させる——5次元スタイル空間の点（視点・時制・ムード・レンズ・ペーシング、480の組み合わせ）。それはミニGA（`baldwin` memeticループ、ターゲットではなく*多様性*を狙う）を実行する：候補ゲノムのバッチが**新規性**——すでに使用されたスタイルからの距離——を最大化するように進化され、その後勝者がコミットされて`styles_used`にプッシュされ、次の章がそれから遠ざかるように圧力を受ける。オフラインではスタイルトレースは決定的で、目に見えて多様である：

```console
  … chapter 1/8  [style: epistolary/journal, present tense, melancholic, dialogue-driven, brisk]
  … chapter 2/8  [style: omniscient third, past tense, wry and whimsical, atmospheric, slow-burn]
  … chapter 4/8  [style: first-person, past tense, melancholic, kinetic action, staccato]
  … chapter 6/8  [style: close third-person, present tense, cold and clinical, kinetic, slow-burn]
```

新奇性探索は独自性 (distinctiveness) を最大化します（すべての次元が異なることを保証するわけではありません)— 正直であり、単一モデルの長文における単調な声の失敗を打破するのに十分です。

オフライン（キーなし）では**決定的スタブ**プランナー/ライターが*まったく同じグラフ*を実行するので、パイプライン——状態スレッディング、gotoループ、蓄積、`.txt`出力——はネットワークなしで検証可能である；`OPENROUTER_API_KEY`は実際の散文にモデルを差し込む。正直な範囲：ゲートは*配管*（ストーリー状態が配線されスレッド化されていること）を証明し、*散文*は証明しない——物語の品質はモデルの仕事であり、構造を超えた連続性はチェッカーノード（ランタイムバックストップパターン）であり、追加すべき明らかな次のノードとして残される。

## 摩擦が表面化し. sign

- **`trail`上のE6「書かれたが読まれない」**はlintとして出力される——そしてそれは*正しい*：`trail`は下流の*ノード*が消費しないターミナル出力チャンネルであり、ホストだけが`RunResult::channel`を介してそれを読み戻す。バリデータはグラフのチャンネル表面について正確であり、間違っていない。エフェクト分析が機能していることを示すために意図的に表示されたままにされている。
- **シリアライズされたチェックポイント状態はチャネルラップされている**（`channel_values["channels"]["trail"]["value"]`）— フラットではない。デモの`channel_of()`ヘルパーがそれをアンラップする。`RunResult::channel`は同じ形状を読み取る。
- コアのロックファイルは、検証全体を通じて`schema_version: 1`を維持する。これにより、標準的な相互交換表現が厳密に保たれ、進化ループがそのコヒーレンス保証を黙ってダウングレードするのを防ぐ。
