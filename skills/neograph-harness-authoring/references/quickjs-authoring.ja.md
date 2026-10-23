<!-- neograph-i18n: source=skills/neograph-harness-authoring/references/quickjs-authoring.md locale=ja source_sha256=2b43b221e43adde43479b04affc783981618f19917ad94444136151aaf21275e -->
# QuickJS ソースの作成

**Languages:** [English](quickjs-authoring.md) | [한국어](quickjs-authoring.ko.md) | [日本語](quickjs-authoring.ja.md) | [简体中文](quickjs-authoring.zh-CN.md)

## 名前はホストから受け取る

リクエストと渡されたマニフェストが、使ってよいすべての名前を与えます。ノード型、
リデューサ、条件、グラフ（Core）名、子バインディング、インポートスロット、入力の形、
そして `main` が返すべき値です。マニフェストが列挙するのは構文（シグネチャ、
オプションキー、上限）だけで、アプリケーションのノードは含みません。必要な名前が
ない場合は、どの名前がないのかを述べてください。もっともらしいだけの名前は誤りです。

グラフ名もその一つです。ホストが名前を与えたら、その文字列をそのまま `ng.graph(...)` と
すべての `ng.callCore(...)` に渡します。タスクの内容から名前を作らないでください。

## モジュール

グローバルの `ng` を使う ES モジュールです。`define` をエクスポートし、タスクが
ランタイム制御を必要とするときはジェネレータ `main` もエクスポートします。
`require`、`module.exports`、`export default` はなく、動的な `import()` は拒否されます。
静的な `import` は、ホストが封印してタスクで名前を伝えたモジュールに対してだけ動作し、
それ以外の `import` は拒否されます。

    // GRAPH、REDUCER、NODE_TYPE はホストが与えた名前を表すプレースホルダです。
    export function define() {
      const g = ng.graph("GRAPH");
      g.channel("value", {reducer: "REDUCER", initial: 0});
      g.node("step", {type: "NODE_TYPE"});
      g.entry("step");
      g.exit("step");
      return g;
    }

    export function* main(input) {
      const result = yield ng.callCore("GRAPH", {value: input.value}, "run");
      return {value: result.channels.value.value};  // グラフ実行の出力
    }

`async`、Promise、タイマー、`eval`、`Date`、I/O は使わないでください。`Date` と `eval` は
例外を投げ、`setTimeout` は存在せず、`Math.random()` は常に 0 を返します。返す値と
すべてのコマンド入力はプレーンな JSON でなければなりません。`undefined`、関数、
`Date`、`Map` は使えません。

## define(): グラフ

`define()` は同期関数で、`ng.graph(name)` が返すビルダーを返します。`ng.callCore` のような
コマンドはここにはありません。

- ビルダーのメソッドはすべて同じビルダーを返し、ノードハンドルは返しません。
  マニフェストのシグネチャとオプションキーをそのまま使ってください。キーは
  snake_case です（`initial_delay_ms` であって `initialDelay` ではありません）。未知の
  キーは `channel`、`retryPolicy`、コマンドのオプションで拒否されます。`node` の設定の
  追加キーはノード型に渡され、受け付けるかどうかはホストのレジストリが決めます。
- グラフには `entry` が必要です。なければ何も実行されないため、コンパイラは拒否します。
  `exit(node)` は実行が終わる場所を示します（出ていくエッジのないノードも実行を終えます）。
  すべてのエッジ、バリア、インタラプト、リトライは宣言済みのノードを指さなければなりません。
  `__start__` と `__end__` はノード名になりませんが、エッジやルートの行き先は `__end__` に
  できます。
- `node(name, {type, ...config})`: `type` は必須で、登録済みでなければなりません。
  `channel(name, {reducer, initial})`: 登録済みのリデューサを指定してください。
- `conditionalEdge(from, condition, routes)`: `routes` は、登録済みの条件が返しうる
  すべてのラベルを、そしてそのラベルだけを、ノードに対応づけます。
- ファンアウトは、一つのノードから出る複数のエッジです。すべての分岐を待つ必要が
  あるジョインには `barrier(join, [branchNodes])` も必要です。一つのチャネルへの
  同時書き込みには、それをマージするリデューサが必要です。

## main(): ランタイムコマンド

`main` は同期ジェネレータです。各 `yield` は `ng` のコンストラクタが作ったコマンドを
ちょうど一つランタイムに渡し、そのコマンドの結果を受け取ります。生の配列、
プレーンなオブジェクト、Promise、コマンドに対する `yield*` は拒否されます。分岐、ループ、`map` などの
通常の JavaScript でコマンドやコマンドの配列を作り、複数を同時に実行するには
`ng.all(commands, {max_in_flight: N})` を一度だけ `yield` してください。

| コンストラクタ | 意味 |
|---|---|
| `ng.callCore(name, input, site)` | グラフを実行します。`name` は `ng.graph` に渡した文字列で、`input` はチャネル名を値に対応づけます。グラフ実行の出力は `result.channels.NAME.value` で読みます。 |
| `ng.all(commands, options, site)` | コマンドを `max_in_flight` 個ずつ同時に実行し、結果を順番どおりに返します。`ng.parallel` も同じです。 |
| `ng.race(commands, options, site)` | 最初に出た結果が勝ちます。コマンドは二つ以上必要です。 |
| `ng.quorum(commands, k, site)` | `k` 個が成功したら終わります。`1 <= k <= commands.length`。 |
| `ng.join(commands, mode, options, site)` | 一般形です。`mode` は `"all"`、`"race"`、`"quorum"`（`required_successes` を伴う）です。`failure_policy` は `"fail_fast"` または `"collect"` です。 |
| `ng.spawn(binding, input, site)` | ホストが承認した子を開始します。結果を得るには `ng.await(spawnCommand, timeoutMs, site)` で包みます。 |
| `ng.emit(value, site)`, `ng.checkpoint(value, site)` | JSON 値を実行ジャーナルに記録します。 |
| `ng.cancelScope(scope, reason, site)` | `"current"`、`"nested"`、`"root"`、`"run"` をキャンセルします。ほかの名前は実行を失敗させます。`main` の最上位では実行をキャンセル状態で終えます。 |
| `ng.hostCapability(slot, input, site)` | ホストが承認したインポートスロット（整数）を呼び出します。 |

- `site` は省略可能な最後の引数で、`"item:" + id` のような安定したラベルです。id や
  インデックスから作り、時刻や乱数からは作らないでください。復旧した実行は `main` を
  再実行するので、同じ結果に対して同じコマンドを出さなければなりません。
- オプションキーは snake_case です。`max_in_flight`、`required_successes`、await の
  タイムアウトは正の整数です。
- コマンドが一つでも失敗すると（`ng.all` のメンバー一つでも）実行は終わり、`main` は
  再開されないので、`yield` を囲む `try`/`catch` では回復できません。論理的な
  リトライは、結果のフィールド（例: `done` フラグ）を見てループします。失敗する
  ノードのリトライは `define()` の `retryPolicy` の役目です。
- 結果はタスクが述べる形で読んでください。応答が `status` のようなフィールドを持つと
  あれば、そのフィールドは `result.status` です。`channels` ラッパーはグラフ実行の
  出力にだけ属するので、タスクが別の形で説明している応答には付けないでください。
- `define()` だけのモジュールは Core の結果を返します。`main` がある場合、返す値は
  ホストが承認した出力契約と一致しなければなりません。

## 応答と修正

ソース評価では、文字列フィールド `source` だけを持つ JSON オブジェクトを一つ返し、
そこにモジュール全体を入れます。JSON としてエスケープし（`\n`、`\"`）、波括弧の前後には
何も置かないでください。Markdown フェンス、説明文、パッチ、Program JSON は不可です。

    {"source": "export function define() {\n  ...\n}\n"}

Harness MCP では、まず `neograph_schema` を呼び、モジュールを `harness.mode:
"javascript"`、`source_id`、`source`、および必要なすべてのタスク、ワーカー、予算、
ポリシーのフィールドとともに送ります。ソースだけではリクエストになりません。
`neograph_compile` は、そのツールがあるときだけ使ってください。

コンパイラに拒否されると、`code`、`message`、コンパイル後の定義の中を指す JSON
`pointer`、`witness` を持つ診断が返ります。ホストが結果に対して自前で行う検査は、
代わりにただのエラーメッセージを返すことがあります。拒否が示す原因を直し、タスクが
定めたことはそのままにして、同じ封筒で完全なモジュールを返してください。権限を広げたり、無関係な操作を
加えたり、ホストの検査が受理する前に成功したと主張したりしないでください。
コンパイルできるソースでも、誤ったトポロジーを作ることがあります。
