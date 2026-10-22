<!-- neograph-i18n: source=docs/program-guide.md locale=ja source_sha256=faed88529c59a389348b12b563d87661b14d1256ca7667e0f728c3529e6f7124 -->
# Program ガイド: グラフを取りまとめる JavaScript

**Languages:** [English](program-guide.md) | [한국어](program-guide.ko.md) | [日本語](program-guide.ja.md) | [简体中文](program-guide.zh-CN.md)

グラフの形は固定されています。ノード、エッジ、チャネルは、コンパイルするときに決まります。Program は、ホストがコンパイルし、承認(admission)して実行する小さな JavaScript モジュールで、グラフの1段上に位置します。Program は、自分が持つグラフを1つ返す `define()` をエクスポートし、必要に応じて `main()` もエクスポートします。`main()` は、そのグラフをどう実行するかを決めるジェネレーターです。同時にいくつ動かすか、結果をどうまとめるか、予算が尽きたらどうするかを決めます。ランタイムは `main()` の各ステップをジャーナルに記録するので、実行は人を待って止まり、そのあと続きから再開できます。

このページでは、モデルを説明し、Program を1つ最初から最後まで追います。ルールは [Python バインディングガイド](python-binding.md#program-and-quickjs)に、モジュールに書けるものと書けないものは[記述ルール](program-graphs.md#authoring-rules)に、グラフの機能は [Program グラフ機能](program-graphs.md)に、エラーコードは[診断ガイド](diagnostics.md)にあります。

## Program を使う場面

1つのグラフとそのチェックポイントで足りるなら、普通のグラフを使ってください。Program はグラフの外側の仕事のためのものです。複数のコピーを並列に実行してルール(`ng.all`、`ng.race`、`ng.quorum`)でまとめる、実行全体に予算をかける、別のプロセスで再開できるように実行を記録する、モデルやユーザーが書いたコードをホストに承認させる、といった仕事です。この JavaScript は、ファイルシステム、ネットワーク、時計、乱数、`eval`、動的 `import`、`Promise`、`async` にアクセスできません。できるのは、グラフを組み立て、ランタイムにコマンドを `yield` することだけです。名前を挙げるノードタイプ、条件、リデューサーはすべて、ホストが作ったレジストリにある必要があるので、生成された Program が、ホストの登録していないコードを実行することはできません。

## 要件

Program 層には、Program と QuickJS 制御を含むビルドが必要です。`pip install neograph-engine` に含まれており、ソースからのビルドでは `-DNEOGRAPH_BUILD_PYBIND=ON -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON` が必要です(そのあと `pip install pydantic certifi` を実行し、ビルドディレクトリを `PYTHONPATH` に加えます)。

以下の例は、失敗ルーティング、`fork`、SQLite の状態ディレクトリを使います。PyPI の 0.14.0 ホイールにはこれらがあり、0.13.1 ホイールにはないので、古いホイールを使う場合は現在のソースからビルドしたもので実行してください。これらを使う例(Python の 32、35、36)も同じです。もっと小さな例から始めるなら、[`29_program_hello.py`](../bindings/python/examples/29_program_hello.py) を実行してください。Program 1つのコンパイル、admission、実行を100行足らずで示します。C++ では [`65_program_javascript.cpp`](../examples/65_program_javascript.cpp) から始めてください。

## 実践例: リリースのチェック

リリースのジョブが、3つのサービスを並列にチェックします。`payments` のチェックは失敗します。ジョブ全体を失敗させる代わりに、失敗はレビューのステップに回り、ジョブは人を待ちます。人がリリースを承認すると、ほかのサービスは再チェックされずにジョブが終わります。同じジョブが予算の範囲内で動きます。

例は Python のファイル3つです。`release_program.py` には実行ごとに変わらないものをすべて入れ、`run_release.py` と `restart.py` がそれを使います。

以下のコードは、失敗ルーティング、レビューのための一時停止、予算、再起動を1つの流れにまとめています。リポジトリには、これらを1つずつ単独で示す実行可能な例があります。[`32_program_failure_routing.py`](../bindings/python/examples/32_program_failure_routing.py)(失敗ルートと、オペレーターの `retry` または `abort`)、[`34_program_pause_resume.py`](../bindings/python/examples/34_program_pause_resume.py)(`interruptBefore` と `host.resume`)、[`33_program_budgets.py`](../bindings/python/examples/33_program_budgets.py)、[`35_program_fork.py`](../bindings/python/examples/35_program_fork.py)、[`36_program_crash_recovery.py`](../bindings/python/examples/36_program_crash_recovery.py) です。C++ で対になる例は、[66](../examples/66_program_failure_routing.cpp) から [69](../examples/69_program_crash_recovery.cpp) までです。

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
# The fork takes a share of the paused run's remaining budget.

fork_budget = budget(wall_time_ms=2_000, max_concurrency=3, max_program_operations=8,
                     max_core_steps=20)
forked = host.fork(paused.run_id, {"decision": "hold"}, version, fork_budget).wait()
print(forked.status, forked.failure.code)
```

```text
ProgramTerminalStatus.Failed release.held
```

`fork` は、止まっている実行のチェックポイントから新しい実行を始め、質問に `"hold"` で答えます。フォークは、失敗終了のコードつきで `Failed` になります。元の実行はまだ止まったままです。終わった作業は、再実行されずにリプレイされます。

フォークの予算は、元の実行の残りの予算から切り分けた取り分です。`wall_time_ms`、`model_tokens`、`monetary_microunits`、`max_program_operations`、`max_core_steps`、`max_dynamic_compiles` は量です。フォークは元の実行に残っている量から自分の取り分を取り、残りは元の実行がそのまま持ちます。この例では、元の実行には、残りの `wall_time_ms` から 2,000 を引いた値と、残りの `max_core_steps` から 20 を引いた値が残ります。`max_concurrency` は量ではなく上限です。フォークは元の実行の上限以下で自分の上限を持ち、元の実行は自分の上限をそのまま保つので、フォークの 3 は元の実行から何も奪いません。フォークのせいで元の実行の `wall_time_ms` または `max_core_steps` が 0 になる場合、`fork` の呼び出しは `P_FORK_SOURCE_BUDGET` で拒否されます。元の実行に残っている量より大きいフォークは `P_FORK_BUDGET` で拒否されます。どちらの場合も、元の実行は変わりません。

フォークで再び実行されるノードは、質問を出したノードのように、質問を出す前に行ったプロバイダー呼び出しを繰り返します。永続的なプロバイダー呼び出しのジャーナルがあれば、フォークはそれらをリプレイするので、再送も再課金もされません。`LocalProgramHost` は、`sqlite_directory` を指定するとこのジャーナルを持ちます。すべてをメモリ上に置くホストにはジャーナルがないため、そこでのフォークはそれらの呼び出しを再送し、再課金します。これはエラーではなく、フォークが送るものも課金するものも変わりませんが、フォークはそのことを知らせます。一時停止した試行が、止まっているオペレーションでプロバイダー呼び出しを記録していた場合、`fork` が返したハンドルの `handle.notices` に、コードが `P_FORK_PROVIDER_CALLS_NOT_JOURNALED` の通知が1つ入ります。`witness` には、それらの呼び出し（`call_id`、`node`、`model`、`microunits`）と合計が並びます。そのうちフォークがどれを再実行するかは、通知からは分かりません。一時停止したステップで終わったノードの呼び出しは、そのステップに保存された書き込みからリプレイされ、再送されないからです。ジャーナルのあるホストでのフォークや、一時停止した試行がそのような呼び出しを記録していないフォークには、通知はありません。通知は実行とともには保存されないので、`reconnect` が返したハンドルにはありません。詳しくは、[Python バインディングガイド](python-binding.md#in-memory-state-and-resume)と[価格の証拠とモデルカタログ](reference-en.md#pricing-evidence-and-the-model-catalogue)を参照してください。

ジャーナルは、呼び出しを、リクエストが求める内容で識別し、それを承認したpolicyでは識別しないので、フォークは、SchemaProviderのdescriptor policyが変わった後でも呼び出しをリプレイします。その場合、ハンドルに `P_PROVIDER_CALL_POLICY_CHANGED` の通知が付き、`wait()` の後に `handle.notices` で見られます。ジャーナルにソースの呼び出しがあっても、0.15が書いたジャーナルのようにそれを照合できない場合、フォークは呼び出しが異なることを示せません。呼び出しを再送して再課金する代わりに、`NG.ConfigError` で失敗します。`LocalProgramHost(fork_may_resend_provider_calls=True)` は再送を許可し、その場合ハンドルに `P_FORK_PROVIDER_CALL_RESENT` が付きます。

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

プロセスが一時停止ではなく実行中のまま残した実行は、`host.reconnect(run_id)` で続行します。どう続行されるかは Program によって違います。子の予算なしで始めた `define()` だけの Program（`max_child_depth` と `max_total_children` がどちらも 0 の、デフォルトの状態）は、`wall_time_ms` の間、実行リースを保持します。所有者がまだ生きているかもしれない実行を、別のプロセスが続行しないようにするためです。リースが切れるまで `reconnect` は `P_RUN_LEASE_HELD` を送出し、切れた後は実行を続行せず `TimedOut` で終わらせます。`main()` を持つ Program や、子の予算（どちらも 0 より大きい）付きで始めた Program はリースを持たず、`reconnect` は最後のチェックポイントからすぐに続行します。そのとき、ジャーナルに記録済みのモデル呼び出しはリプレイされ、課金は1回だけです（[`36_program_crash_recovery.py`](../bindings/python/examples/36_program_crash_recovery.py)）。

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
| `BudgetExhausted` | 予算のかかった資源が尽きました。例: `P_PROGRAM_OPERATION_BUDGET`、`P_CORE_STEP_BUDGET`、`P_CONCURRENCY_BUDGET`。モデルトークンは、応答が予算を超えた場合も、次の呼び出しが送信前に拒否された場合も、`NG.BudgetExhausted` で終わります。金額とCoreのステップは、実行がどれだけ超えたかを示す記録とともに終わります（[予算](#budgets)）。実行が予算を超えたまま終わった場合も同じです。 |
| `TimedOut` | 実行の `wall_time_ms` か `ng.await` の期限を過ぎました。`wall_time_ms` を過ぎてから完了した実行も `TimedOut` です（[予算](#budgets)）。 |
| `Cancelled` | `ng.cancelScope` かハンドルの `cancel()` が実行を止めました。 |

`AmbiguousEffect` と `CheckpointIncompatible` もあります。`include/neograph/program/result.h` を参照してください。

<a id="budgets"></a>
## 予算

`ProgramRunBudget` にはフィールドが10個あります。`wall_time_ms`、`model_tokens`、`monetary_microunits`、`max_concurrency`、`max_program_operations`、`max_core_steps`、`max_dynamic_compiles`、`max_child_depth`、`max_total_children`、`max_node_retries` です。`result.usage` は実行が使った量を、`result.remaining_budget` は残りを示します。0 は何も許可しないという意味です。`model_tokens = 0` の実行がモデルを呼ぶと、`BudgetExhausted` で終わります。上限、承認された予算、モデルのコストに関するルールは、[Python バインディングガイド](python-binding.md#budgets)にあります。

予算を超えて終わった実行には、どの予算かを示す failure の記録が付き、witness がどれだけ超えたかを示します。`failure.witness` には、予算のキーの下に `granted`、`used`、`over_by` が入ります。実行が許可量のところで止まった場合や、モデル呼び出しが送信前に拒否された場合、`over_by` は 0 です。

| 予算 | ステータス | `failure.code` | witness のキー |
|---|---|---|---|
| `wall_time_ms` | `TimedOut` | `P_RUNTIME_TIMEOUT` | `wall_time_ms` |
| `model_tokens` | `BudgetExhausted` | `NG.BudgetExhausted` | `model_tokens` |
| `monetary_microunits` | `BudgetExhausted` | `P_MONEY_BUDGET` | `monetary` |
| `max_core_steps` | `BudgetExhausted` | `P_CORE_STEP_BUDGET` | `core_steps` |

予算が実行を途中で止めた場合も、実行が予算を超えたまま終わった場合も、記録は同じです。使用量は plan の完了後に精算されるため、plan が許可量を超えて終わることがあります。応答のコストが、その呼び出しに予約した額を上回ることがあります。キャンセルを無視するノードが期限の後に戻ることがあります。サブグラフノードのステップは、そのノードを含むグラフを止めずに、実行の Core ステップに加算されます。実行が複数の予算を超えた場合、witness にはそれぞれの項目が入り、ステータスとコードは表で最初に来る予算（予算フィールドの並び順）のものになります。期限は、ほかに何を使っていても実行を `TimedOut` で終わらせるため、wall time が先頭です。料金のかかる呼び出しが送信前に拒否された場合、拒否の証拠は `monetary` の下に残ります。予約額、`ceiling`、`used` です。

料金のかかる呼び出しの予約額は、呼び出しの出力 cap を最も高い出力レートで見積もった額です。モデルの upstream が cap で止まる場合にだけ、これが支出の上限になります。NeoGraph は cap を送りますが、upstream がそれを適用するかは確認できないため、cap を無視する upstream の応答は予約額より高くなることがあります。その呼び出しは応答が報告した額で請求され、実行に残る `monetary_microunits` はその分だけ減り、請求額が許可量を超えた実行は、上と同じく `P_MONEY_BUDGET` とともに `BudgetExhausted` で終わります。許可量の範囲内なら実行は続きます。NeoGraph はそのような呼び出しごとに、実行のハンドルへ `P_MONEY_OVER_HOLD` notice を出し（`handle.notices`、`wait()` の後に読みます）、`result.monetary_records` にあるその呼び出しの記録は `microunits` が `reserved_microunits` を上回ります。どちらも、応答が名前を示す場合は、呼び出しを処理したアップストリーム（`served_by`）を持ちます。notice はほかに何も変えず、NeoGraph は notice の後で route を拒否しません。それはホストのポリシーです。cap に頼るホストは、OpenRouter の呼び出しを cap を守ると分かっている upstream にだけ向け（provider の routing の `only` または `order`）、実行のたびに notice と記録に対応し、失ってもよい額より少なく付与できます。詳細は [価格の証拠とモデルカタログ](reference-en.md#pricing-evidence-and-the-model-catalogue) の出力 cap の段落にあります。

## Program の作業が動く場所（C++ ホスト）

C++ ホストは `RuntimeConfig` から `ProgramRuntime` を作ります。ホストが別に指定しなければ、ランタイムは attempt を
`scheduler_threads` 本のスレッドのプールで、デッドラインタイマーをもう 1 本のスレッドで実行します。
`LocalProgramHost(scheduler_threads=...)` が設定するのがこれです。独自の executor を持つホストは
`RuntimeConfig::executor` を設定します。シングルスレッドの `io_context`、ほかの作業と共有する `thread_pool`、
WebAssembly ホストなら `async::HostLoop::executor()` です。するとランタイムはスレッドを起動せず、すべての attempt、
デッドラインタイマー、待機者がその executor で実行され、開始した呼び出しの中でインライン実行されるものはありません。

- **すべての操作に awaitable の対があります。** `start_async`、`reconnect_async`、`resume_handle_async`、`fork_async`、
  `replace_async`、`migrate_graph_async` などは、ランタイムの executor で実行され、待機しているコルーチンの executor で
  完了します。`run_async` と `resume_async` は結果を待ちます。
- **同期呼び出しと `ProgramHandle::wait` は拒否されることがあります。** ホストのイベントループが駆動する executor や、
  スレッドのないビルドでは、Program を動かすべきスレッドをブロックしてしまうため、何かをする前に
  `ProgramBlockingCallRefused` を投げます。`operation()` は呼び出しを、`alternative()` は awaitable 形式を示します。
  注入された executor を動かしているスレッドでの待機も拒否されます。executor のないネイティブのランタイムは変わりません。
- **ホストが executor を渡したときは、ノード本体は自分のスレッドをブロックしてはいけません。** 専用のワーカーが必要な
  ノードの attempt はその executor の strand で実行され、ブロックする本体は、その executor を共有するすべての run を
  止めてしまいます。
- **ランタイムを破棄すると** 実行中の run はキャンセルされ、注入された executor では終了を待ちません。ハンドルが
  終了状態になるまで executor を回し続けてください。

実行できるチェックのある WebAssembly 側は [`wasm/README.md`](../wasm/README.md) にあります。

## Program の状態が保存される場所（C++ ホスト）

`ProgramRuntime` は run の記録を `ProgramTransitionStore` に、バージョンを `ProgramStore` に保存し、ホストが使う場合は、
タスクグラフ断片、リンクされたモジュール、Core ツールの grant を、それぞれ `TaskGraphFragmentStore`、`ModuleStore`、
`ProgramCoreToolGrantStore` に保存します。これらのインターフェースは `asio::awaitable` を返すので、ホストは呼び出しが
あとで完了するデータベースの上にストアを置くことができ、ランタイムはスレッドを握る代わりにそれを await します。

- **同期バックエンドは、対応する `Sync...Store` クラスから派生します**（`SyncProgramTransitionStore` ほか 4 つ）。そして
  `..._sync` 操作（`load_sync`、`compare_publish_sync` など）を実装します。基底クラスはそれらから各 awaitable 操作を完了
  させます。呼び出したスレッドですぐに完了するか、ストアが `offloads()` を true にオーバーライドしたときは有限のプールの
  スレッドで完了します。インメモリと SQLite のストアはすぐに完了し、PostgreSQL はプールに回し、待っているコルーチンは
  自分の executor で再開します。await する呼び出しの引数は、string view を含め、呼び出しが完了するまで生きている必要があります。
- **中断できない呼び出し元はブロッキングビューを使います。** `BlockingProgramTransitionStore` と残りの 4 つは、インターフェースが
  かつて持っていたシグネチャを持ちます。同期ストアに対するビューは、その `..._sync` 操作を呼びます。それ以外のストアでは、
  呼び出したスレッドの専用 `io_context` で awaitable を回して待ちます。ただし `BlockingStorePolicy::refuse` が、待つと呼び出しを
  完了させるべきスレッドが止まると言うときは拒否します。ランタイムのポリシーは、ホストが駆動する executor と、自分の
  executor を回しているスレッドで拒否し、呼び出しはストアに触れる前に `ProgramBlockingCallRefused` を投げます。
- **何が await し、何が待つのか。** ランタイムのすべての操作は、自分のストアを await する 1 つのコルーチンです。エントリーポイント
  （`start`、`reconnect`、`resume`、`fork`、`replace` など）、run の終端の公開、子の起動とその復旧、awaitable のときの Core ツール grant
  リゾルバー（`RuntimeConfig::core_tool_grant_awaitable_resolver`。ストアから `make_durable_core_tool_grant_awaitable_resolver`
  で作ります）、`ProgramCatalog`（`resolve_version_async`、`admit_async` などの `_async` 形式）、`ModuleResolver::resolve_async` が
  そうです。`_async` 形式はその本体をランタイムの executor で実行します。同期形式は同じ本体を呼び出したスレッドで最後まで実行し、
  そのスレッドが止まってしまう場所では拒否されます。したがって、ホストのイベントループの後のターンでしか完了しないストアが、
  ホストが駆動する executor で `_async` 形式を通して動き（`ProgramHandle::cancel_async`、`snapshot_async`、`latest_handoff_async`、
  `next_handoff_async` を含みます）、そのループのターンの中では同期形式が拒否されます。まだ呼び出し側をブロックするのは
  `ProgramHandoff::reference()`、`RuntimeInstructionController`、`inspect_program_replacement_chain`、`hook_runtime` で、
  ランタイムは `graph::CheckpointStore` をそのストアの同期呼び出しで読みます。
- **awaitable を捨てる呼び出しは何もしません。** 操作は `[[nodiscard]]` です。`co_await` するか、ブロッキングビューを使ってください。
- **契約**（アトミックな公開、ヘッドの compare-and-swap、fail-closed）はすべてのストアが満たすもので、
  `tests/test_program_transition_store.cpp` と `tests/test_program_catalog.cpp` の適合性（conformance）スイートが、インメモリ、
  SQLite、PostgreSQL のストアに対して、それぞれの形式で 1 回ずつ実行します。

## 動作を確認しているテスト

例の各ステップは、`bindings/python/tests/program_e2e` のシナリオに対応しています。`test_07_main_programs.py`(`ng.all`、`race`、`quorum`、`await`)、`test_10_failure_routing.py`(失敗ルートと承認)、`test_02_hitl.py`(インタラプトと再開。メモリ上と再起動後)、`test_04_budgets.py`(予算)、`test_13_fork_replays_the_paused_step.py` と `test_20_javascript_fork.py`(フォーク)です。[そのディレクトリの README](../bindings/python/tests/program_e2e/README.md)にすべてのシナリオが載っています。例で扱わなかった Hook、provider 呼び出し、子 Program は、[Python バインディングガイド](python-binding.md)と[再帰的な Harness](PROGRAM_RECURSIVE_HARNESSES.md)で扱っています。
