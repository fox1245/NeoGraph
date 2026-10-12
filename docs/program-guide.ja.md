<!-- neograph-i18n: source=docs/program-guide.md locale=ja source_sha256=6f69f4c493387ec533fd315d4e5a294d10e695e59765446ee310d4add2d1b624 -->
# Program ガイド: グラフを取りまとめる JavaScript

**Languages:** [English](program-guide.md) | [한국어](program-guide.ko.md) | [日本語](program-guide.ja.md) | [简体中文](program-guide.zh-CN.md)

グラフの形は固定されています。ノード、エッジ、チャネルは、コンパイルするときに決まります。Program は、ホストがコンパイルし、承認(admission)して実行する小さな JavaScript モジュールで、グラフの1段上に位置します。Program は、自分が持つグラフを1つ返す `define()` をエクスポートし、必要に応じて `main()` もエクスポートします。`main()` は、そのグラフをどう実行するかを決めるジェネレーターです。同時にいくつ動かすか、結果をどうまとめるか、予算が尽きたらどうするかを決めます。ランタイムは `main()` の各ステップをジャーナルに記録するので、実行は人を待って止まり、そのあと続きから再開できます。

このページでは、モデルを説明し、Program を1つ最初から最後まで追います。ルールは [Python バインディングガイド](python-binding.md#program-and-quickjs)に、モジュールに書けるものと書けないものは[記述ルール](program-graphs.md#authoring-rules)に、グラフの機能は [Program グラフ機能](program-graphs.md)に、エラーコードは[診断ガイド](diagnostics.md)にあります。

## Program を使う場面

1つのグラフとそのチェックポイントで足りるなら、普通のグラフを使ってください。Program はグラフの外側の仕事のためのものです。複数のコピーを並列に実行してルール(`ng.all`、`ng.race`、`ng.quorum`)でまとめる、実行全体に予算をかける、別のプロセスで再開できるように実行を記録する、モデルやユーザーが書いたコードをホストに承認させる、といった仕事です。この JavaScript は、ファイルシステム、ネットワーク、時計、乱数、`eval`、動的 `import`、`Promise`、`async` にアクセスできません。できるのは、グラフを組み立て、ランタイムにコマンドを `yield` することだけです。名前を挙げるノードタイプ、条件、リデューサーはすべて、ホストが作ったレジストリにある必要があるので、生成された Program が、ホストの登録していないコードを実行することはできません。

## 要件

Program 層には、Program と QuickJS 制御を含むビルドが必要です。`pip install neograph-engine` に含まれており、ソースからのビルドでは `-DNEOGRAPH_BUILD_PYBIND=ON -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON` が必要です(そのあと `pip install pydantic certifi` を実行し、ビルドディレクトリを `PYTHONPATH` に加えます)。

以下の例は、失敗ルーティング、`fork`、SQLite の状態ディレクトリを使います。PyPI の 0.13.1 ホイールにはこれらがないので、現在のソースからビルドしたもので実行してください。C++ では `examples/63_program_quickstart.cpp` から始めてください。

## 実践例: リリースのチェック

リリースのジョブが、3つのサービスを並列にチェックします。`payments` のチェックは失敗します。ジョブ全体を失敗させる代わりに、失敗はレビューのステップに回り、ジョブは人を待ちます。人がリリースを承認すると、ほかのサービスは再チェックされずにジョブが終わります。同じジョブが予算の範囲内で動きます。

例は Python のファイル3つです。`release_program.py` には実行ごとに変わらないものをすべて入れ、`run_release.py` と `restart.py` がそれを使います。

### 1. ノードのコード

ノードは普通の Python です。`check` は `payments` で失敗します。失敗は、例外の `code`、`public_message`、`retryable` という属性で自分自身を説明し、例外のテキストがノードの外に出ることはありません。`Review` は人が答えたあとに実行され、その答えが再開値になります。

```python
import hashlib

import neograph_engine as ng


# 1. The node code. Programs refer to these by name; they stay ordinary Python.

CHECKED = []   # which services ran their checks, to show that nothing runs twice

class ChecksFailed(Exception):
    code = "release.checks_failed"        # an author code: lower-case, dotted
    public_message = "the release checks failed"
    retryable = True


@ng.node("release.check")
def check(state):
    service = state.get("service")
    CHECKED.append(service)
    if service == "payments":
        raise ChecksFailed(f"{service}: 2 tests failed")
    return [ng.ChannelWrite("report", [f"{service}: checks passed"])]


class Review(ng.GraphNode):
    """Runs after a person answered; the answer is the resume value."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        answer = input.ctx.resume_value or {}
        return [ng.ChannelWrite("decision", answer.get("decision", "hold"))]


@ng.node("release.ship")
def ship(state):
    return [ng.ChannelWrite("report", [f"{state.get('service')}: shipped"])]


@ng.node("release.hold")
def hold(state):
    return [ng.ChannelWrite("report", [f"{state.get('service')}: held"])]


ng.NodeFactory.register_type("release.review", lambda name, config, ctx: Review(name))
ng.ConditionRegistry.register_condition(
    "release.decision", lambda state: state.get("decision") or "hold")
```

### 2. レジストリ

レジストリは、Program が使える実行可能な名前の一覧で、名前ごとにバージョンとダイジェストを持ちます。ホストは、このプロセスにすでに登録されているノード、条件、リデューサーからレジストリを作ります。

```python
# 2. The registry: the only executable names a Program may use.

def digest(label):
    return "sha256:" + hashlib.sha256(label.encode()).hexdigest()


def build_registry():
    builder = ng.ProgramRegistryBuilder()
    for node_type in ("release.check", "release.review", "release.ship", "release.hold"):
        builder.add_registered_node(node_type, "1.0.0", digest(node_type))
    builder.add_registered_condition("release.decision", "1.0.0", digest("release.decision"))
    for reducer in ("append", "overwrite"):
        builder.add_registered_reducer(reducer, "1.0.0", digest(reducer))
    return builder.build()
```

### 3. Program

`define()` はグラフを記述します。`check` は2回試します(`max_retries: 1`)。それでも失敗した場合は、実行を終わらせずに、失敗を `review` へ回し、失敗の記録を `failures` チャネルに追加します。`review` は `interruptBefore` で止められているので、実行はそこで止まって答えを待ちます。答えが来ると、条件 `release.decision` が実行を `ship` か `hold` に進めます。`hold` は失敗終了ノードで、実行をコード `release.held` つきの `Failed` で終わらせます。

`main()` は `ng.all` でサービスごとに1回ずつグラフを実行します。同時に動かすのは最大3つで、レポートをサービスの順に返します。

```python
# 3. The Program: define() describes the graph, main() coordinates runs of it.

SOURCE = r'''
export function define() {
  const g = ng.graph("release");
  g.channel("service", {reducer: "overwrite", initial: ""});
  g.channel("report", {reducer: "append", initial: []});
  g.channel("failures", {reducer: "append", initial: []});
  g.channel("decision", {reducer: "overwrite", initial: ""});
  g.node("check", {type: "release.check"});
  g.node("review", {type: "release.review"});
  g.node("ship", {type: "release.ship"});
  g.node("hold", {type: "release.hold"});
  g.entry("check");
  g.edge("check", "ship");
  g.exit("ship");
  // Two attempts. Then the failure goes to "review" instead of failing the run.
  g.retryPolicy("check", {max_retries: 1, initial_delay_ms: 10, backoff_multiplier: 1,
                          max_delay_ms: 10, on_failure: "review", error_channel: "failures"});
  g.interruptBefore("review");               // a person answers before "review" runs
  g.conditionalEdge("review", "release.decision", {approve: "ship", hold: "hold"});
  g.failureExit("hold", {code: "release.held"});
  return g;
}

export function* main(input) {
  const results = yield ng.all(
    input.services.map((name) => ng.callCore("release", {service: name}, "release:" + name)),
    {max_in_flight: 3}, "fan-out");
  return results.map((result) => result.report);
}
'''
```

覚えておくルールが2つあります。`ng.callCore` が指定できるのは、`define()` が返すグラフだけで、`ng.graph()` に付けた名前で指定します。そして、失敗したコマンドは実行を終わらせます。ジェネレーターは再開されないので、`yield` を囲む `try`/`catch` は実行されません。失敗は、ここで `retryPolicy` がしているようにグラフの中で処理するか、ホストで `result.status` と `result.failure` を読んで処理してください。

### 4. 予算とホスト

ホストは、承認するすべての Program に上限を設けます。Program は実行予算1つとともに承認され、そのバージョンのすべての実行は、まったく同じ予算を使う必要があります。上限のフィールドは、子の2つの制限を除き、モデルを呼ばない Program でも正の値でなければなりません。

```python
# 4. The budgets. The host ceiling bounds every Program; the run budget bounds one run.

def budget(**fields):
    value = ng.ProgramRunBudget()
    for name, number in fields.items():
        setattr(value, name, number)
    return value


ceiling = budget(wall_time_ms=120_000, model_tokens=1, monetary_microunits=1,
                 max_concurrency=8, max_program_operations=64, max_core_steps=2_000,
                 max_dynamic_compiles=1)
run_budget = budget(wall_time_ms=10_000, max_concurrency=6, max_program_operations=16,
                    max_core_steps=50)


def admit(host, run_budget):
    """Compile the Program, check it against the host's ceiling and return its version."""
    source = ng.ProgramSource.from_javascript("release.js", SOURCE)
    return host.compile_admit(source, run_budget)


def make_host(state_dir=None):
    host = ng.LocalProgramHost(build_registry(), "tenant:docs", ceiling,
                               sqlite_directory=state_dir)
    return host, admit(host, run_budget)
```

`compile_admit` はソースをコンパイルし、バンドルが上限を超えていないか確認して、`ProgramVersion` を返します。ソース、レジストリ、予算が同じなら、別のプロセスでもバージョン ID は同じになります。テストがプロセス間でこれを確認しています。

### 5. 実行して止める

`run_release.py` を作ります。

```python
from release_program import CHECKED, admit, budget, make_host, run_budget

host, version = make_host()

# Run: "payments" fails its checks, so the run pauses for a person.

services = {"services": ["api", "payments", "web"]}
paused = host.run(version, services, run_budget)
print(paused.status)
print(paused.interrupt.core_node, paused.interrupt.value["failure"]["code"])
```

```text
ProgramTerminalStatus.Interrupted
review release.checks_failed
```

`api` と `web` はチェックを終えました。`payments` は2回失敗したので、その失敗は `review` にルーティングされ、実行はステータス `Interrupted` で止まりました。`result.interrupt.core_node` は待っているノードを示し、`result.interrupt.value` には `{"reason": "failure_route", "failure": {...}}` が入っています。失敗の記録には `node`、`kind`(ここでは `exhausted`)、`code`、`attempts`、`retryable`、`step`、`routed_to`、`effects` があります。`api` と `web` のレポートは、すでに実行の中にあります。

### 6. フォークして別の答えを試す

```python
# Fork the paused run to see what "hold" would do. The original stays paused.
# The fork's budget is taken out of the paused run's remaining budget.

fork_budget = budget(wall_time_ms=2_000, max_concurrency=3, max_program_operations=8,
                     max_core_steps=20)
forked = host.fork(paused.run_id, {"decision": "hold"}, version, fork_budget).wait()
print(forked.status, forked.failure.code)
```

```text
ProgramTerminalStatus.Failed release.held
```

`fork` は、止まっている実行のチェックポイントから新しい実行を始め、質問に `"hold"` で答えます。フォークは、失敗終了のコードつきで `Failed` になります。元の実行はまだ止まったままです。終わった作業は、再実行されずにリプレイされます。

フォークの予算は、元の実行の残りの予算から、フィールドごとに差し引かれます。`max_concurrency` も同様です。ジョインには3が必要で、実行には6あるので、元の実行には3が残ります。フォークがある資源をすべて持っていくと、元の実行は続けられません。その場合に `P_CONCURRENCY_BUDGET` と `P_TRANSITION_CONFLICT` を確認しています。

### 7. 答えで再開する

```python
# Resume the original with "approve". The other services do not run again.

done = host.resume(paused.run_id, {"decision": "approve"}).wait()
print(done.status)
print(done.output)
print(sorted(CHECKED))
```

```text
ProgramTerminalStatus.Completed
[['api: checks passed', 'api: shipped'], ['payments: shipped'], ['web: checks passed', 'web: shipped']]
['api', 'payments', 'payments', 'web']
```

`resume` は待っていた質問に答え、同じ実行を続けます。人が承認したので、`payments` はチェックを飛ばして出荷されます。最後の行は、何も2回実行されていないことを示しています。`api` と `web` は1回ずつ、`payments` は失敗した2回の試行で2回現れます。

### 8. 小さすぎる予算

```python
# A budget that is too small refuses the join before any member starts.

small = budget(wall_time_ms=10_000, max_concurrency=6, max_program_operations=2,
               max_core_steps=50)
refused = host.run(admit(host, small), services, small)
print(refused.status, refused.failure.code)
```

```text
ProgramTerminalStatus.BudgetExhausted P_PROGRAM_OPERATION_BUDGET
```

ジョインには、この予算(`max_program_operations=2`)が許すより多くの Program 操作が必要です。ランタイムはメンバーを1つも開始する前にジョイン全体を拒否するので、どのサービスもチェックされません。予算を変えるには、`compile_admit` をやり直して新しいバージョンを作ります。承認された予算と異なる予算で `host.run` を呼ぶと、`P_START_BUDGET` が発生します。

### 9. 再起動を乗り越える

既定では、`LocalProgramHost` はすべてをメモリに置き、`resume` は実行を始めたホストオブジェクトを通してしか動きません。`sqlite_directory` を指定すると、ホストは実行、バージョン、チェックポイントを SQLite のファイルに保存するので、新しいプロセスが実行を続けられます。同じレジストリを使い、同じ Program を承認する必要があります。

`restart.py` を作ります。

```python
import sys

from release_program import make_host, run_budget

state_dir, step = sys.argv[1], sys.argv[2]
host, version = make_host(state_dir)

if step == "start":
    paused = host.run(version, {"services": ["api", "payments", "web"]}, run_budget)
    print(paused.status, paused.run_id)
else:
    done = host.resume(sys.argv[3], {"decision": "approve"}).wait()
    print(done.status, done.output)
```

```bash
python restart.py state start
python restart.py state resume run-...
```

最初のコマンドはステータス `Interrupted` と実行 ID を出力します。その ID を2番目のコマンドに渡すと、`Completed` とレポートが出力されます。ディレクトリ `state` には `program.db`、`checkpoints.db`、`provider_calls.db` が入ります。

## `define()` で表現できること

`ng.graph(name)` はビルダーを返します。すべてのメソッドはビルダーを返します。

| メソッド | 意味 |
|---|---|
| `channel(name, {reducer, initial})` | リデューサー(`overwrite`、`append`、または登録済みのもの)を持つ状態チャネル。 |
| `node(name, {type, ...config})` | 登録済みのタイプのノード。ほかのキーはその設定です。 |
| `edge(from, to)`、`conditionalEdge(from, condition, routes)` | 固定のエッジと、行き先が分かれるエッジ。条件は登録済みの名前です。 |
| `entry(node)`、`exit(node)` | `__start__` と `__end__` につなぎます。 |
| `barrier(node, waitFor)` | 列挙したすべてのノード、または `{any_of: [...]}` スロットを待つジョイン。 |
| `interruptBefore(...)`、`interruptAfter(...)` | ノードの前または後で実行を止め、答えを待ちます。 |
| `retryPolicy(node, {...})` | リトライ、タイムアウト、そして `on_failure` があれば失敗エッジ。 |
| `failureExit(node, {code})` | 実行をそのコードつきの `Failed` で終わらせるノード。 |
| `metadata({...})` | グラフについての主張。権限は与えません。 |

`ng.javascript_authoring_capability_manifest()` は、このビルドが受け付ける語彙を正確に返し、TypeScript の宣言は `dts_prelude` に入っています。あまり使わないキーは [Program グラフ機能](program-graphs.md)で説明しています。

## `main()` が `yield` できるもの

`yield` はコマンドを1つランタイムに渡し、コマンドが完了するとその結果を受け取ります。

| コマンド | 結果 |
|---|---|
| `ng.callCore(name, input, site)` | グラフを実行し、その出力を返します。出力は、チャネルごとの `value` と `version` を持つ `channels` で、各チャネルの値はチャネル名のキーでも参照できます。 |
| `ng.all(members, options, site)`(`ng.parallel` も同じ) | メンバーのコマンドを `max_in_flight` 個ずつ並行して実行し、結果を与えた順に返します。メンバーが1つでも失敗すれば、実行が失敗します。 |
| `ng.race(members, options, site)` | 最初に終わったメンバーの結果を返します。 |
| `ng.quorum(members, required, site)` | 必要な数のメンバーが成功した時点で返します。 |
| `ng.await(command, timeoutMs, site)` | コマンド1つを期限まで待ちます。期限を過ぎると、実行は `P_AWAIT_TIMEOUT` つきの `TimedOut` で終わります。 |
| `ng.spawn(binding, input, site)` | 子 Program を開始します。子バインディングのリゾルバーを持つ C++ ホストが必要で、`LocalProgramHost` にはないので `P_CHILD_BINDING` で失敗します。 |
| `ng.emit(value, site)`、`ng.checkpoint(value, site)` | `value` を実行のジャーナルに記録し、そのまま返します。それぞれ Program 操作1回として数えます。 |
| `ng.cancelScope(scope, reason, site)` | 実行をキャンセルします(`scope` は `"current"`)。実行は `P_RUNTIME_CANCELLED` つきの `Cancelled` で終わります。 |

各コマンドの最後の引数は、呼び出し箇所を示すラベルです。`ng.all` のメンバーが1つ止まると、実行が止まります。残りのメンバーは最後まで実行され、実行は `Interrupted` になり、`resume` が止まったメンバーに答えます。

## ステータス

`ProgramResult.status` は `ProgramTerminalStatus` です。`Completed` 以外のときは `result.failure`(`code`、`message`、`core_node`、`witness`)を読んでください。

| ステータス | 意味 |
|---|---|
| `Completed` | `main()` が値を返したか、グラフが終了ノードに達しました。`result.output` にその値が入ります。 |
| `Interrupted` | 実行が答えを待っています。`result.interrupt` が場所を示します。`resume` か `fork` で続けます。 |
| `Failed` | グラフがルーティングしなかった失敗、失敗終了、または `main()` が投げた例外。`result.failure.code` が内容を示します。 |
| `BudgetExhausted` | 予算のかかった資源が尽きました。例: `P_PROGRAM_OPERATION_BUDGET`、`P_CORE_STEP_BUDGET`、`P_CONCURRENCY_BUDGET`。 |
| `TimedOut` | 実行の `wall_time_ms` か `ng.await` の期限を過ぎました。 |
| `Cancelled` | `ng.cancelScope` かハンドルの `cancel()` が実行を止めました。 |

`AmbiguousEffect` と `CheckpointIncompatible` もあります。`include/neograph/program/result.h` を参照してください。

## 予算

`ProgramRunBudget` にはフィールドが10個あります。`wall_time_ms`、`model_tokens`、`monetary_microunits`、`max_concurrency`、`max_program_operations`、`max_core_steps`、`max_dynamic_compiles`、`max_child_depth`、`max_total_children`、`max_node_retries` です。`result.usage` は実行が使った量を、`result.remaining_budget` は残りを示します。0 は何も許可しないという意味です。`model_tokens = 0` の実行がモデルを呼ぶと、`BudgetExhausted` で終わります。上限、承認された予算、モデルのコストに関するルールは、[Python バインディングガイド](python-binding.md#budgets)にあります。

## 動作を確認しているテスト

例の各ステップは、`bindings/python/tests/program_e2e` のシナリオに対応しています。`test_07_main_programs.py`(`ng.all`、`race`、`quorum`、`await`)、`test_10_failure_routing.py`(失敗ルートと承認)、`test_02_hitl.py`(インタラプトと再開。メモリ上と再起動後)、`test_04_budgets.py`(予算)、`test_13_fork_replays_the_paused_step.py` と `test_20_javascript_fork.py`(フォーク)です。[そのディレクトリの README](../bindings/python/tests/program_e2e/README.md)にすべてのシナリオが載っています。例で扱わなかった Hook、provider 呼び出し、子 Program は、[Python バインディングガイド](python-binding.md)と[再帰的な Harness](PROGRAM_RECURSIVE_HARNESSES.md)で扱っています。
