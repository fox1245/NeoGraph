<!-- neograph-i18n: source=examples/cookbook/the-beast/README.md locale=ja source_sha256=aa9675ba1cbeeb80c64724416d97b82171a94f2261f16551966e181ee742405d -->
# ビースト — 生成、進化、ロールバック

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

> 独自のハーネスを作成し、それを
> DSL コンパイラを実行し、チェックポインタを通じてその実行を巻き戻します。
> **生成されました。進化した。巻き戻し。野獣は残ります。**

ほとんどの「エージェント フレームワーク」では、グラフを *構築* できます。野獣は3つやる
静的ハーネスではできないこと — そして 3 つすべてが ** 現実的で、オフラインであり、
この 1 つのプログラムでは deterministic** (API キーなし):

1. **実行時に新しいハーネスを生成**し、実行前にそれが一貫していることを証明します。
   単一ノードが実行されます。
2. コンパイラ自体を使用して、実際の突然変異演算子で **進化**
   フィットネスゲートとして。
3. **実行中のハーネスを以前のスーパーステップにロールバック**します。
   チェックポインター — リプレイではなく、本物のタイムトラベル。

NeoGraph ではハーネスは **データ**、つまりトポロジであるため、これが安全であるだけです。
JSON (問題 #56) で説明されています。DSL コンパイラ (問題 #75) では、
*実行前にハーネスが一貫していることを証明します*。それを取り去って「エージェント」
独自のグラフを作成する」は、壊れたグラフを作成するための単なるマシンです。
コンパイラは、モンスターを負債からカテゴリーに変えるものです。

## 実行してください

```console
$ cmake --build build --target cookbook_the_beast
$ ./build/cookbook_the_beast
```

```
── ACT I · generate a harness, prove it coherent ──
  ACCEPTED — 3 gates passed. Core lockfile nodes: s1_n s2_n s3_n
  (DSL surface expanded away: vars/templates/use gone.)

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

## 第 1 幕 — 生成 + ゲート

Beast は DSL **サーフェス** (`vars` / `templates` /) でハーネスを作成します
`use`) を実行し、3 つのコヒーレンス ゲートを順番に通過させます。ハーネス
いずれかのゲートに失敗したものは**破棄**されます。

|ゲート | API |キャッチ |
|---|---|---|
| **1.詳細** | `Elaborator::elaborate` | DSL 座標に対するサーフェス エラー — 不明なテンプレート、欠落または余分な `use` 引数、変数サイクル、ノード名の衝突。全体的かつ決定的: 同じ DSL は常にバイト同一のコアを生成するため、ゲート 2 ～ 3 は固定アーティファクトについて推論します。 |
| **2.コンパイル + TV** | `GraphCompiler::compile` (厳密、`schema_version: 1`) + `verify_roundtrip` |タイプミスまたはサポートされていないキーは、サイレント ドロップではなく *ハード エラー* (消費キー アカウンティング) です。次に、変換検証で `canon(source) == canon(compile(source).to_json())` がアサートされます。コンパイラーが何もかも静かに再配線することはできません。 |
| **3.検証** | `GraphValidator::validate` |グラフの**意味**: ぶら下がりエッジ (E3)、決して発射できないバリア (E8)、不完全なルート マップ (E10)、チャネル効果違反 (E4/E6)。エラーは、決して正しいとは言えない構成要素のみに発生します。残りは糸くずです。 |

シードは、`use` 経由で 3 回インスタンス化された 1 つの `stage` テンプレートです。
詳細化により、コア チェーン `s1_n → s2_n → s3_n` に拡張されます。

## 第 2 幕 — 進化 (コンパイラはフィットネス関数です)

`neograph::graph::evolve()` (問題 #80) は **実際の突然変異演算子** を実行します
シード上 — `swap_template`、`add_use`、`remove_use`、`tune_param`、
`toggle_conditional_edge`、`toggle_barrier`、`add_edge`、`remove_edge`。
すべての子は **コンパイル ゲートを最初に通過します**: 無効な子は消滅します
実行することなく、無料で。拒否率自体が健康状態
演算子のメトリック。

重要な設計上の選択: 突然変異スペースは raw ではなく **DSL (M4) です
JSON** であるため、子孫は *構造上* 構造的に有効です。つまり、
ここで拒否数が 0 になっているのはなぜですか。ゲートは安全策です。
制約のない進化は安全であり、すべての子供たちに武装し続けます。

各実行は、`to_json(result)` 経由で相違可能な系図を出力します。
個人の親、世代、突然変異、およびコア ロックファイル。それ
リネージュ ** は** 進化的スケールでのロールバック サーフェス — コミット
それは違います、世代全体を元に戻します。

## 第 3 幕 — ロールバック (チェックポインタのタイムトラベル)

生き残ったハーネスは `InMemoryCheckpointStore` で生成されます
`EngineConfig::checkpoint_store` を通じて接続されています。エンジンのスナップショット
各スーパーステップの終了時の状態。その後：

- `store->list("beast-run")` は完全なタイムラインを返します — *見ることができます*
  `trail` はステップごとに 1 つのノードを成長させます。
- `store->load_by_id(earlier.id)` は、正確なチャネル状態を **復元**します。
  もっと前のステップ。デモは `["s1_n","s2_n","s3_n"]` から
  `["s1_n","s2_n"]` — 後のステップは完全になくなりました。これは
  `load_by_id` / `load_latest` タイムトラベル、同じ機械の HITL
  割り込み/再開とスレッドフォークが構築されています。

## ライブ開始 – モデルが実際にハーネスを作成します

`the_beast.cpp` はオフラインです (スタブ作成者)。 [`the_beast_live.cpp`](the_beast_live.cpp)
本物です: ライブ LLM が渡されます `NodeFactory::export_schema()`
(このエンジンのビルドが受け入れる正確なパレット — ドリフトすることはできません。
*は* エンジンのスキーマです。[`../../52_export_schema.cpp`](../../52_export_schema.cpp)を参照してください)
そして、DSL サーフェスでハーネスを作成するように依頼されました。それが何を返しても
同じ 3 つの門を通過します。拒否された場合のゲートの診断
会話に直接フィードバックされ、モデルが書き換えられます。
本物の自己修復ループ。

```console
$ echo 'OPENROUTER_API_KEY=sk-or-...' >> .env      # DeepSeek v4 flash via OpenRouter
$ cmake --build build --target cookbook_the_beast_live
$ ./build/cookbook_the_beast_live                  # optional: pass a task string as argv[1]
```

```
── Attempt #1: asking the model to write a harness ──
  model returned 663 chars of JSON.
  ACCEPTED — all three gates passed.
  Core lockfile nodes: r_stage c_stage s_stage

── Spawning the model's harness (checkpointed) ──
  ran to completion, trail = ["r_stage","c_stage","s_stage"]
  checkpoint timeline (3 snapshots): ...
  >> ROLLBACK to step 1: restored trail = ["r_stage","c_stage"]

The model wrote it. The compiler proved it. The Beast ran it.
```

**ライブ実行で示されたこと** (DeepSeek v4 フラッシュ): *一貫性のある* が作成されました
線形パイプライン、ダイヤモンド ファンアウトの最初の試行でハーネスを実現 /
バリア ファンイン、および条件付きルーター — 自己修復ループが装備されています
しかし、有能なモデルであれば、それが起こることはめったにありません。門は依然としてその地位を保っていた
lint: ダイヤモンド (E9) のバリアが失われており、到達不能であるとフラグが立てられました。
ルーター上のハンドラー (E7) を警告として表示します。重要なのはモデルではない
頻繁に失敗します。それは**そうなると、壊れたハーネスを取り出すことができないということです
コンパイラを超えて** — 創造性は無限であり、一貫性は証明されています。

ここのノードは決定論的な `beast_node` ワーカーであるため、ライブ実行にはコストがかかります
1 回の LLM 呼び出し (オーサリング) は無料で実行されます。それらを交換してください
`llm_call` と各ノードもライブ コールになります。

## Apex — ハーネスがツールを食い荒らす

スタブワーカーのデモは、生成されたハーネスが *一貫性* であることを証明していますが、
ハーネスは決して機能しません。 [`the_beast_apex.cpp`](the_beast_apex.cpp) は
モンスター: モデルには **ツール カタログ**が渡され、ツール カタログを作成するよう求められます。
ReAct エージェント — `llm_call` ⇄ `tool_dispatch` が `has_tool_calls` でループします。
書き込むハーネスは一貫性のためにゲートされ、**
ツールバインド** (`ctx.tools` + `engine->own_tools`)。生成されたエージェントは、
どのツールをいつ呼び出すかを独自に決定します。

```console
$ cmake --build build --target cookbook_the_beast_apex
$ ./build/cookbook_the_beast_apex "What is 23 * 19, and the weather in Seoul?"
```

実際の実行 — 自己修復ループが実際に起動し、その後自律的に実行されます
ツール呼び出し:

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

これが 1 回の実行で論文全体です: モデルは `nodes` を幻覚しました
スキーマを 2 回 (`id` キーを追加し、次に `name` キーを追加)、および厳密なコンパイラーの
**消費キー アカウンティングが両方とも拒否されました** - 診断は元に戻りました
会話を続けたところ、3回目の試行で自動的に修復されました。それから、
マシンで作成され、コンパイラで実証されたエージェントがライブ ReAct ループを実行し、
2 つのツールが自律的に動作します。創造性には制限がなく、ツールの使用は自律的であり、
**一貫性は交渉の余地のないものです。**

## Forge — ツールが不足している場合、ツールを作成します

[`the_beast_forge.cpp`](the_beast_forge.cpp) は頂点とツールです
サプライチェーン。タスクが与えられると、次のことが行われます。

1. **DISCOVER** — ストック MCP stdio サーバーを生成し、そのツールをリストします。
   実際の MCP プロトコル (`MCPClient::get_tools`)。
2. **FORGE** — タスクに必要だがカタログにない機能、
   アーキテクト LLM **それを実装する Python MCP サーバー**を作成します。私たちは
   それをディスクに具体化して起動し、新しいツールを**再発見**します
   MCP以上。 (生成されたサーバーの初期化に失敗した場合は自己修復します。)
3. **AUTHOR** — *結合された* カタログ上に ReAct エージェントを書き込みます。三つ
   いつものようにゲート+自己修復。
4. **SPAWN** — 検出されたすべての * および * 偽造ツールをバインドし、
   エージェントは自律的にそれらを呼び出します。

実際の実行 - モデルが不足しているツールを作成し、エージェントがそれを使用しました。

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

2 つのライブ MCP サブプロセス (1 つのストック、1 つは Beast が *この実行* で作成したもの)、
それぞれに実際の `tools/list`、実際の ReAct ループ。オーサリングモデルのみが
リモート。

### カスタム *ノード* も定義できますか?

正直に言うと、NeoGraph ノード **タイプ** は、次の方法で登録された C++ クラスです。
`NodeFactory::register_type` — 新しいアトミックを JIT コンパイルすることはできません
実行時の C++ ノード タイプ。しかし、その意図は 3 つの方法でカバーされています。
Beast はデータから運転*できます:

- **複合ノード** — DSL の `templates` / `use` (M4) はモデルを
  再利用可能なノード/トポロジ ユニットを純粋にデータ内で定義します。それはまさに
  `the_beast.cpp` のシードが何をするか。
- **再帰** — `subgraph` ノードはハーネス全体を 1 つのノードとして埋め込みます。
  そのため、Beast が作成したハーネスには Beast が作成したサブハーネスを含めることができます
  (Nレベルの自己増殖)。
- **コードによるカスタム動作** — 上記の forge パターンは * ランタイムです
  モデルによって作成された動作: モデルが作成したツールはディスパッチ可能になります
  ユニット。同じトリックが汎用の `script_node` 型 (
  モデルで記述されたコードを実行する事前登録された C++ ノード)。
  「LLM がロジックを定義した新しいアトミック ノード」を取得する正直な方法。

本当にテーブルから外れている 1 つのことは、新しい *コンパイルされたものを出力することです
実行時の C++ ノード クラス*。モデルが特化する必要があるものすべて
動作はデータ/スクリプト/サブグラフの表面に存在しており、コンパイラはすでに
門。

## スクリプト — ユニバーサル カートリッジ (モデルが作成したノード ロジック + フロー)

上記のすべてのバリアントでは、モデルに *ツール* (リーフ機能) を作成できます。
[`the_beast_script.cpp`](the_beast_script.cpp) により **ノード ロジックを作成できるようになります
— ツールが絶対に不可能な制御フロー (`goto`) を含む
Express.** `script_node` は、コンパイル済みの 1 つの C++ ノードであり、その構成は
モデルで書かれた Python。 `run()` では、ノードにチャネル状態を渡し、
コードが返すものすべて (`{writes, goto, sends}`) を
グラフ。モデルは、ノードの動作 * および* グラフのフローを定義します。
データを再コンパイルせずに保存します。

一貫性は交渉の余地のないものです。スクリプトは設定でコントラクトを宣言します
(`reads` / `writes` / `goto_targets`);ハーネスは 3 つの DSL を通過します
ゲートに加えて、Beast-layer **コントラクト チェック** (宣言された書き込みは
宣言されたチャネル。 goto ターゲットは実際のノードである必要があります) プラス ** ランタイム
Wrapper** は、宣言外の write/goto を拒否します。それ
**ゼロ変化で獣層の効果/ルート保証を回復します
NeoGraph コア** へ — 追加的で下位互換性があります。

```console
$ cmake --build build --target cookbook_the_beast_script
$ ./build/cookbook_the_beast_script --selftest   # offline, no API key
$ ./build/cookbook_the_beast_script              # live: DeepSeek writes the node logic
```

ライブ実行 - モデルは、独自の制御フローを持つカウンター ループを作成しました。
`goto`:

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

`tick` には静的なエッジはありません。ループが存在するのは、
モデルの Python は、カウンターが 3 に達するまで `{"goto": "tick"}` を返し、その後
`{"goto": "__end__"}`。 `--selftest` は、同じメカニズムを
API キーのない缶詰ハーネスなので、CI はオフラインで実行できます。

**境界 (正直)。** コンパイラはグラフの *形状* を証明します。の
コントラクトは、ノードの *表面* (ノードがどのチャネル/ターゲットに到達する可能性があるかを証明します)
触る）;スクリプトの *内部ロジック* だけが証明されていません。
サブプロセスでは `timeout` 、実行では `max_steps` です。ランニング
モデルで記述されたコードは任意のコード実行です。ローカルでは問題ありませんが、
ユーザー主導のクックブックですが、本番環境では、その周囲にサンドボックスが必要です。
通訳者。これは **ビルド オプション** で、デフォルトではオフになっています。

サンドボックス API は FetchContent 経由でうまく埋め込まれないため、事前に構築されたツリーをリンクします
(オプションの上の CMake コメントでレシピをビルドします):

```console
$ cmake -S . -B build -DNEOGRAPH_BEAST_SANDBOX=ON -DSANDBOX2_SRC=/path/to/sandboxed-api
$ cmake --build build --target cookbook_the_beast_script
```

これをオンにすると、Python は独自の Google **Sandbox2** の下で実行されます。
user/pid/mount/net 名前空間、以下に限定された読み取り専用 FS ビュー
インタプリタ + 2 つの作業ファイル、および CPU/ウォール/ファイルの制限。ニーズ
`libcap-dev`、`libunwind-dev`、C++20 ツールチェーン。 Linux/WSL2で検証済み。

**エフェクト コントラクトから合成された Seccomp ポリシー。** Python のシステムコール
フットプリントが大きすぎて安全にホワイトリストに登録できないため、デフォルトのアクションのままになります
permissive — ただし、ノードの宣言された *capabilities* により syscall が減算されます。
`"net"` 機能を宣言していないノードには、`socket`/`connect`/`bind`/… があります。
seccomp ブロック (EPERM); `"exec"` 機能がない場合は、`execve`/`execveat` をブロックします。
ポリシーは*宣言された契約*に基づいており、手書きではありません。これ
陰性テストで検証されました — **同じ** 下の **同じ** Python
サンドボックス。宣言された上限のみが異なります。

```
caps=[]     (no net cap): {"socket": "SOCKET_BLOCKED:EPERM"}   # seccomp denies the syscall itself
caps=[net]  (net cap):    {"socket": "SOCKET_CREATED"}         # capability grants it
```

したがって、これはネットワーク名前空間以上のものです。`net` キャップがない場合、
`socket()` *syscall* が失敗します (netns 上の多層防御)。正直
スコープ: これは **コンテナ グレード + 契約由来の seccomp ブロックリスト **、
完全な syscall ホワイトリストではありません。ブロックされていない syscall を介したカーネルエクスプロイトは、
まだ封じ込められていない。より厳密なノードごとの許可リスト (および機能ベース)
秘密調停) が次のステップとして文書化されています。

## 進化 — ミーム的 (ダーウィン + ラマルク)

オフラインの `the_beast.cpp` は意図的に `run_evaluation=false` を設定するため、
構造的な妥当性だけで選択します。汎用 evolution API はタスクを実行し、期待する
チャネル値との完全一致でもスコアリングできます。

[`the_beast_evolve.cpp`](the_beast_evolve.cpp) は代わりに連続的な距離指標を
使用します。これにより、近似解は汎用スコアラーの単一の出力不一致クラスに
留まらず、数値目標へ向けて改善できます。

- **タスク** (構造的なプロキシではなく、出力スコアが付けられた本物のタスク):
  ターゲット数値を計算する ARITHMETIC PIPELINE をアセンブルします。ファイブオプ
  ノードが存在します — `add2(+2) add3(+3) mul5(*5) mul2(*2) sub1(-1)` — 各読み取り
  `acc` チャネル (初期化 0)、その演算を適用し、それを書き戻します。ハーネスの
  答えは、実行後に `acc` が保持するものです。 **フィットネス =
  `-(|acc - 20|)`**。 *トポロジ* (どの操作がどのような順序で実行されるか)
  が数値を決定するため、配線を進化させると計算も進化します。
- **ダーウィン主義**: ランダムな再配線 (`all_operators()`) + による選択
  測定された出力 — 20 に向かってつまずきます。
- **ラマルキアン**: LLM が算術演算を行い、20 に達するチェーンを配線します。
  正確に取得し、その取得したソリューションを継承可能なシードとして注入します。

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

コントラストがすべてのポイントです: **盲目の突然変異は、
target** (世代 9 までに 5→10→24→19→20、試行によって数値を計算);
**LLM は算術演算を行います** — `(0+2)*5*2 = 20` — に直接ジャンプします
注射した時の答え。なぜなら、その得られた解決策が
伝統的なチャンピオン (`origin 'LLM'`)、それはラマルキアンです。ブラインドバリエーション+
選択はダーウィン的です。両方を実行するのはミームアルゴリズムです。

正直なメモ: 純粋なダーウィンニアンはオフラインで検証され、決定的です。の
Lamarckian LLM 呼び出し (deepseek-v4-flash) は **不安定な場合があります** -
ストリーミングされた応答が解析不能に返される場合があります。その場合、実行
`[Lamarckian] LLM returned no parseable harness` とダーウィンのログ
ラウンドを運ぶ。最後の行はチャンピオンの *実際の* 出身地を報告しています。
ラマルクの勝利は決して起こらなかった。

## Gate-eval — コヒーレンス ゲートは実際に健全ですか?

Beast の安全性に関する議論全体は、静的バリデータは *サウンド* であるということです。
coherence oracle: エラーは、ハーネスに本当に障害があることを意味します。
ランタイム;エラーがない場合は実行されることを意味します。それは**主張されたものであり、測定されたものではありません**
すべての査読者が最初に尋ねること。

[`the_beast_gate_eval.cpp`](the_beast_gate_eval.cpp) で計測します。それは、
バリデーターによるトポロジーのラベル付きコーパス (予測された判定) かつ
エンジン (グラウンド トゥルース) とクロスチェックを介して。オフライン、確定的、
キーなし — すべての判定が実行と一致した場合は `exit 0` となるため、**CI はゲートオンできます
健全性**。

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

テスト対象のプロパティ:

> バリデーターはエラーを報告します ⟹ 実行時にグラフに障害が発生します。
> バリデータはエラーを報告しません ⟹ グラフは正常に実行されます。

最初の行は *健全性* (クリーンに実行されたエラーフラグの付いたグラフは次のようになります)
健全性の穴）;正常に実行される警告フラグ付きのグラフはゲートを示しています
*過度に*拒否しません。 E10/E8 クラスのエラーは判定のみです。つまり、
空のルート マップは `rend()` (UB) を逆参照します。これがまさに障害です。
ゲートはそれを防ぐために存在するため、チェックはされますが実行されません。これは
デモンストレーション コーパス。すべての診断を網羅しているわけではありませんが、
「ゲートは健全です」というスローガンを、測定された CI 適用の 4/4 に変えます。

## ゲートファズ — 大規模な保証とその境界

[`the_beast_gate_fuzz.cpp`](the_beast_gate_fuzz.cpp) は、gate_eval を 5 からプッシュします
手作業でラベル付けされたケースから数千件のあいまいなケースまで、正直に言って。素朴な動き
(ファズ N グラフ、印刷精度 1.0) は劇場になります: **エンジンは
validator はコンパイル時に実行され、エラーが発生するとスロー**されるため、「validator-error ⟹」
「engine-faults」は *構造上* true です。したがって、プログラムは 2 つのことを測定します
実際に有益なものは次のとおりです。

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

- **レイヤー 1 — 大規模な一貫性。** 一貫したシードをランダムでファジングします。
  構造ミューテーター (ダングリング エッジ → E3、未宣言書き込み → E4、オーファン ライター、
  ドロップされたエッジ → E7 *警告*、追加の有効なエッジ)。 2000 を超えるミュータントのコンパイラ
  ゲートとエンジンは決して一致しません。これは健全性 *発見 * ではありません (
  部分的には構造による） — これは **回帰保証** です: 将来の変更があった場合
  静的ゲートとランタイムを分岐させると、これは失敗します。
- **レイヤー 2 — 境界。** ゲートは各ノードの宣言された**効果を信頼します。
  契約**。 *嘘*をするノード — `writes:["out"]` を宣言しますが、実際には次のように書き込みます
  実行時に宣言されていない `phantom` チャネル — 静的ゲートを通過します
  (500/500)、**ランタイム `GraphState` 書き込みガード** がすべてをキャッチします
  (500/500)。それはゲートのバグではありません。それは計画された分業です。

その結果、保証についての *正確な* 記述が得られ、これは保証に関する記述よりも誠実なものとなります。
疑わしいほど完璧な混同行列: **静的ゲートは、以下に比べて健全です。
正直な契約と、不正な契約に対する実行時のバックストップ** - レイヤ 1 と
レイヤ 2 それぞれに CI が適用されます。

正式なコンパニオンである [`SOUNDNESS.md`](SOUNDNESS.md) は、これを *証明* しています: 小さな一歩
スーパーステップ実行のセマンティクス、エフェクトラティス `(𝒫(Chan), ⊆)`、ゲートとして
整形式判定 `⊢ G ok` とプログレス定理 (ゲート通過グラフ)
正直な契約の下では決して過失はありません）正直仮説が必要であることが判明しました
そしてそのフェイルストップバックストップとしてのランタイム書き込みガード。あらゆる前提条件がチェックされます
エンジンのソースに対して。 `gate_eval`/`gate_fuzz` はモデルの忠実度です
小切手。ここでの 2 つのハーネスは、そのドキュメントの Cor 6.4 と Prop 6.5 です。

## ボールドウィン — ミームは盲目的に勝つのか、そして継承は重要なのか?

`evolve` バリアントは、ダーウィン変異 + ラマルク LLM インジェクションを示しました。
すべての査読者が提起したより鋭い研究上の質問: **次のようなタスクはありますか
ブラインド進化とワンショットソルバーは両方とも失速しますが、ミームの組み合わせです
勝利 — そして、学習した特性を「どのように」継承するかによって結果が変わります
文献は予測していますか?** (Whitley 1994; Hinton & Nowlan 1987.)

[`the_beast_baldwin.cpp`](the_beast_baldwin.cpp) はその実験です、轢いてください
本物の NeoGraph ハーネス。ゲノムはアフィン パイプラインの配線です。それぞれ
ステージは、生涯学習のために、オペ **または左プラスチック (`?`)** に取り組んでいます。
解決する。フィットネスは、**走行時の組み立てられたハーネス**の特徴です。
起動時のクロスチェックにより、高速分析の適合性がコンパイル済みの適合性と等しいことが証明されます。
エンジンは 200 のトポロジに対応しています (gate-eval と同じ分野)。風景というのは、
**欺瞞的**: どこにでも見える広いおとりの丘 (0.85) と、狭い
**勾配のない**グローバル プラトー (1.0)、*学習* のみ — を検索します。
可塑性遺伝子によって広がる近隣 — を見つけることができます。

```console
$ ./build/cookbook_the_beast_baldwin          # offline, deterministic, no key
engine/analytic cross-check: 200/200 topologies execute exactly as modeled → real harness.
  Darwinian  | assimilated global  6/24 | mean committed → global  25%  decoy  70%
  Baldwinian | assimilated global 21/24 | mean committed → global  74%  decoy  17%
  Lamarckian | assimilated global 23/24 | mean committed → global  78%  decoy   9%
CI gate (blind Darwin near the 25% chance floor, learners assimilate >65% by a
  >25-pt margin, faithful fitness): PASS
```

適応度*とは*に関するメモ: 各ゲノムは実際の NeoGraph にコンパイルされます。
トポロジとクロスチェックにより、エンジンがそのうち 200 個を正確に実行することが証明されます。
分析モデルは予測します - *基材*は本物で、忠実に実行されたものです
ハーネス。 GA が最適化する「目的」は、欺瞞的なハミング景観です。
生の実行ではなく、配線 (ダイナミクスの制御されたテストベッド)
出力。どちらの事実も曖昧ではなく、明確に述べられています。

異なる基準に基づく 2 つの調査結果:

1. **ミームは盲目に勝つ (堅牢 — CI ゲート)。** 盲目的なダーウィン進化論
   プラトーは
   コミットされた空間の勾配がないため、選択はおとりに従ってトラップされます。
   学習によりプラトーが露出し、それが最大 75% 吸収されます。ゲートは、
   *マージン* (24 シードを超えることを意味します)。実行ごとのしきい値数ではありません。
   実行ごとのカウントは初期化の運によって左右されます。 25% 対 75% のマージンが安定しています
   信号。
2. **ボールドウィン コントロール (測定 - ゲートなし)。** ボールドウィン (継承しない)
   学習した形質) vs ラマルク (ゲノムに書き込む): こちら **74% vs 78%**
   グローバル — ラマルキアンがわずかに上回っており、景観に関する *予想される*結果
   これは欺瞞的ではありますが、敵対的ではありません (ライトバックの速度がライトバックの速度を上回ります)
   多様性コスト）。ホイットリーの **逆転** (ボールドウィン > ラマルク) には、
   特に敵対的な状況。この単純な 2 つのピーク構造は、
   それをしっかりと示しており、それは**まぐれに合わせず、正直に報告されています**。
   (これは本当にデリケートです: インデックスベースのタイブレークを備えた初期のバージョン
   逆転を示すために * 現れた * アーティファクト。選択範囲の境界で結びます
   シードごとのランダムな抽選によって分割され、**スイープ全体で平均化**されるようになりました。
   ~74 対 78 の順序はシード ベース全体で安定しています。明らかな逆転が起こった
   その修正は生き残れません。)

これは、査読者が求めた結果の正直な形状です。つまり、堅牢な主張です。
（学習に導かれた進化は、盲目的な進化では解決できないことを解決します）が測定され、
CI が適用されます。デリケートな主張（非相続が相続に勝つ）が評価される
否定的な結果を非表示にするのではなく名前を付けて、そのまま報告します。

## Baldwin-adv — 敵対的な風景 + 実際のヒルクライム学習

[`the_beast_baldwin_adv.cpp`](the_beast_baldwin_adv.cpp) は両側をシャープにします
前回の実験。学習は **本当のローカル検索** (複数回の再起動) になりました
可塑性遺伝子を山登りして局所最適値を求める - 離散的アナログ
リファイナーの、LLM が差し込まれるスロット）、そしてその風景はまさに
**敵対的**: 小さな丘から*遠ざかる*方向を向いた広いおとりの丘。
急勾配のグローバルボール。ブラインド専用スペース検索はチャンスの場ではありません。
おとりの勾配の下で積極的に**騙され**ます。

```console
$ ./build/cookbook_the_beast_baldwin_adv        # offline, deterministic, no key
  Darwinian  | committed → global   5%   decoy  92%
  Baldwinian | committed → global  76%   decoy  19%
  Lamarckian | committed → global  98%   decoy   1%
CI gate (blind deceived onto decoy >50%, both learners solve >60%, faithful): PASS
```

- **ミーム ≫ 盲目 (堅牢、CI ゲート)。** ダーウィンはおとりに騙される
  (~5% グローバル / ~92% おとり);学習は単一のゲノムからグローバルなボールを見つける
  できません (76 ～ 98%)。これはプラトー、つまりブラインドよりも「強い」分離です。
  ベースラインは単に盲目であるだけでなく、誤解されています。シードベース全体で安定 (ダーウィン 2-5%、
  学習者は 76 ～ 98%)。
- **ボールドウィン対ラマルク: 逆転は再現されません。** ラマルクアン
  ライトバックが安定したマージンで勝利します (98% 対 76%)。全体にわたるパラメータスイープ
  到達可能/到達不能境界全体 (30 個以上の構成、3 回のスイープ) が見つかりました ** なし
  非継承がライトバックを強力に上回る体制**: グローバルが
  到達可能な場合、ライトバックの速度が優先されます。そうでない場合は、両方とも失敗します。
  限界 (~3 ～ 4 ポイント) ボールドウィンの多様性のエッジ。これが正直な経験的な答えです
  「ホイットリーのボールドウィン > ラマルクの逆転はハーネス上で再現されますか?」
  トポロジー?」 — **いいえ**、この離散体制では、プログラムは次のように言っています。
  という名前のメカニズム。 (ホイットリーの逆転は *継続* で確立されました
  実数値のローカル検索を備えたマルチモーダル関数。離散トポロジ - GA
  こちらでは出品しておりません。）

## Baldwin-llm — モデルは学習演算子です

上記の機械学習 (ランダムな推測、山登り) は常に *スロット* でした
LLM リファイナーが接続されています。 [`the_beast_baldwin_llm.cpp`](the_beast_baldwin_llm.cpp)
それを接続すると、モデルが実際に推論できるタスクに基づいて、`?` ステージを埋めることができます。
`acc` がターゲットに到達するように算術パイプラインを調整します。 **学習演算子は次のとおりです。
モデル** (`?` ステージの操作を選択します);フィットネスは組み立てられたものです
ハーネスを*実行*します。ボールドウィン/ラマルクの切り替えは文字通りになります。

- **Baldwinian** はモデルのフィルをスコア付けしますが、遺伝子 `?` を保持します。モデルは次のことを行う必要があります。
  次世代でも**もう一度**相談してください。学習は継承されません。
- **Lamarckian** はフィルをゲノムに書き込みます。`?` はコミットされます。
  獲得された形質は**遺伝性**です。モデルを再度調べる必要はありません。

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

観察可能な違いは適応度 (両方とも目標に到達) ではなく、 ** ゲノムです。
経済**: ラマルクは学習者の成果を遺伝に預けます (遺伝子はコミットし、呼び出します)
ゼロに落ちる);ボールドウィンは世代ごとに再学習する（遺伝子は可塑性を保つ、と呼びかける）
ハイを維持してください）。決定論的な **oracle** 学習器を使用してオフラインで実行する (デフォルト)、または
`--llm` と `OPENROUTER_API_KEY` を組み合わせて **モデル** を学習者にします。
これらの呼び出しが実際の API 呼び出しである場合、継承は文字通り
モデルに一度支払うか、世代ごとに支払うかの違い。これは
「モデルの修正は継承可能か?」の具体的な意味— として示されています
トレースしますが、アサートされません。 (`--llm` パスにはネットワークが必要です。これは、
oracle は呼び出し/解析の失敗をログに記録するため、デモは常に完了します。)

## Novelist — 前提はイン、ライトノベル長さの `.txt` アウト

最もシンプルで本当に使えるライティングハーネス、そして誠実なフォルム
「NovelWriter」のアイデア: 前提を与えて、全体をライトノベルサイズに戻す
プレーンテキストとしての原稿。 [`the_beast_novelist.cpp`](the_beast_novelist.cpp)は
**途中で迷った**の治療法が具体化 — 長い物語は*書かれていない*
一つの巨大な文脈の中で。これは、**明示的なストーリーの状態**に関する小さなグラフです。

    チャンネル: 前提、概要、聖書、要約、本、idx、合計

したがって、各章は **コンパクトな外部化された状態に対して新鮮に生成されます**
(アウトラインビート、ストーリーバイブル、実行中の要約) 60k を再読する代わりに
文字。モデルは、小説の登場人物が誰なのかを *思い出す*必要はありません —
`bible` チャネルを読み取ります。

```console
$ ./build/cookbook_the_beast_novelist "a librarian's returned books whisper futures" 12
harness passed the coherence gate. writing (live — this takes a few minutes)…
  … chapter 1/12 written (4180 chars)
  …
done — 51k characters across 12 chapters.
manuscript: /abs/path/novel_12ch.txt
```

グラフは `__start__ → planner → writer ⟲`: `planner` は前提を次のようにします。
概要と最初の聖書。 `writer` は `idx` 章を `book` に書き込み、
**`summary` と `bible`** を更新して、次の反復が接地されたままになるようにします。
**`Command` goto による自己ループ** は、`idx+1 == total` までです。効果契約は、
宣言されているため、**コヒーレンス ゲートは単語が書き込まれる前に配線を証明します** —
すべてのストーリー状態チャネルが実際に消費され、ぶら下がりステージはありません。

**バッチ生成、バッチ進化 - チャプターごとに独特の感触。** 各チャプターは
事実上、分離されたサブエージェント (によってのみ接地される新しい `writer` 呼び出し)
共有されたストーリー状態)。彼らが同じように読まれないようにするために、作家は
章ごとの **スタイル ゲノム** — 5 次元スタイル空間内のポイント (POV、時制、ムード、
レンズ・ペーシング、480通りの組み合わせ）。ミニ GA (`baldwin` ミーム ループ、
ターゲットではなく*多様性*を目指した): 候補ゲノムのバッチが進化して
**斬新さ**を最大化する — すでに使用されているスタイルから遠ざける — そうすれば勝者は
コミットされて `styles_used` にプッシュされるため、次の章は圧迫されます。
それ。オフラインでは、スタイル トレースは決定的であり、明らかに変化します。

```console
  … chapter 1/8  [style: epistolary/journal, present tense, melancholic, dialogue-driven, brisk]
  … chapter 2/8  [style: omniscient third, past tense, wry and whimsical, atmospheric, slow-burn]
  … chapter 4/8  [style: first-person, past tense, melancholic, kinetic action, staccato]
  … chapter 6/8  [style: close third-person, present tense, cold and clinical, kinetic, slow-burn]
```

新規性検索は独自性を最大化します (すべての側面を *保証するものではありません)
異なります） — 正直で、単一モデルの単調な音声の失敗を打ち破るのに十分です
長い形式。

オフライン (キーなし) **決定性スタブ** プランナー/ライターは *まったく同じものを実行します
グラフ* なので、パイプライン - 状態スレッド、goto ループ、累積、
`.txt` 出力 — ネットワークなしで検証可能です。 `OPENROUTER_API_KEY` がスワップイン
本物の散文のモデル。正直な範囲: ゲートは *配管* を証明します
(ストーリーの状態はワイヤーとスレッドで構成されています)、*散文* ではありません — 物語の質は
モデルのジョブ、および構造を超えた連続性はチェッカー ノードになります (
runtime-backstop パターン)、追加する明らかな次のノードとして残されます。

## 摩擦が表面化した

- **`trail`** 上の E6「書き込まれたが読み取られなかった」は lint として出力されます。
  *正しい*: `trail` は、ダウンストリームがない端末出力チャネルです。
  *ノード* が消費します。ホストのみが `RunResult::channel` 経由でそれを読み取ります。
  バリデーターは、グラフのチャネル表面について正確であるのではなく、
  間違っている。効果分析が機能していることを示すために、意図的に表示されたままになっています。
- **シリアル化されたチェックポイント状態はチャネルラップされています**
  (`channel_values["channels"]["trail"]["value"]`)、フラットではありません — デモの
  `channel_of()` ヘルパーがそれをアンラップします。同形状 `RunResult::channel`
  読みます。
- コアのロックファイルは、エラボレーションを通じて `schema_version: 1` を保持します。
  これは、ゲート 2 を厳密モードにオプトインするものです — DSL サーフェスでのオーサリング
  黙ってダウングレードすることはありません。一貫性は進化ループを保証します。
  に依存します。
