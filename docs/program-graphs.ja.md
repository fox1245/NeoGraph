<!-- neograph-i18n: source=docs/program-graphs.md locale=ja source_sha256=20ebf915c22aaeffcbcc7afba46518d5b2fb7ce055c18326867f940fb8670d0c -->
# Programのグラフ機能

**Languages:** [English](program-graphs.md) | [한국어](program-graphs.ko.md) | [日本語](program-graphs.ja.md) | [简体中文](program-graphs.zh-CN.md)

このページでは、`define()` を使うProgramが、ノード、エッジ、バリア、グラフ単位のリトライポリシーのほかに使えるグラフ機能を説明します。各機能は、strictなCore定義（`schema_version` 1）のキーで、`javascript_authoring_capability_manifest()["features"]` にフラグがあります。ある機能を使わない定義は、その機能が追加される前とまったく同じバイト列、`definition_hash`、バンドルIDのままです。

これらのキーを1つでも使う定義は、バリデーターの警告をバンドルにシールしません。警告は、これまでどおりコンパイルの結果として返されます。詳しくは[診断ガイド](diagnostics.md)を参照してください。

<a id="authoring-rules"></a>
## 記述ルール

Program は、ホストがコンパイルする JavaScript モジュール1つです。以下のルールはモジュールのコンパイル時または実行時に検査され、違反すると各項目に示した診断コードで報告されます（[診断ガイド](diagnostics.md)を参照）。

- **モジュールの形。** モジュールは、グラフビルダーをちょうど1つ返す同期の `define()` をエクスポートします（`P_JS_DEFINE_MISSING`、`P_JS_DEFINE_ASYNC`、`P_JS_DEFINE_VALUE`）。`main()` だけをエクスポートするモジュールは拒否されます。`main()` は省略でき、エクスポートする場合は同期ジェネレーターでなければなりません（`P_JS_CONTROL_MAIN`、`P_JS_CONTROL_ASYNC`）。`main()` がなければ、実行はグラフをそのまま実行します。トップレベルの `await` があるモジュールも拒否されます。
- **`define()` の中。** ループ、補助関数、テンプレート文字列など、ふつうの決定的な JavaScript で、`ng.graph()` を通じてグラフを組み立てます。コマンドを `yield` することはできず、ノードも実行されません。ビルダーの引数は正規の JSON データでなければならず（`P_JS_GRAPH_VALUE`）、`define()` が戻った後はビルダーを使えません（`P_JS_GRAPH_SEALED`）。
- **`main()` の中。** ループ、再帰、クロージャ、例外、ローカルの状態は通常どおり使えます。ただし `main()` が動作できるのは、`ng` で作ったコマンド（`ng.callCore`、`ng.all` など）を `yield` するときだけです。ランタイムがコマンドごとに検査してジャーナルに記録し、実行して、結果を渡してジェネレーターを再開します。ソースコードは動作を要求するだけで、自分に権限を与えることはできません。すべてのノードタイプ、条件、リデューサーはホストのレジストリにある必要があり、ホストの予算が実行を制限します。
- **拒否されるもの。** `Date`、`eval`、`Function` コンストラクターは `P_JS_AMBIENT_DENIED` で失敗します。`Math.random()` は常に0を返すので、Program に乱数はありません。グローバルの `std`、`os`、`process`、`require`、`fetch`、`crypto`、`performance`、タイマー関数、`Worker`、`SharedArrayBuffer`、`Atomics`、`WebAssembly`、`WeakRef`、`FinalizationRegistry` は未定義です。動的な `import()` と `require()` は `P_JS_IMPORT_DYNAMIC` で失敗し、静的な `import` は、ソースと一緒に封印されたモジュールを指さなければなりません（`P_JS_IMPORT_UNAVAILABLE`）。
- **ソース言語ではないもの。** Program を書く言語は JavaScript だけです。`define()` が作る strict Core JSON、バンドル、ジャーナル、レコードは保存された成果物であり、NeoGraph はそれらを Program のソースとして受け付けません。従来の Core DSL と Program の JSON 操作ツリーは削除されました。

## グラフのメタデータ

フラグ：`graph.metadata`。

```js
export function define() {
  return ng.graph("plan")
    .metadata({owner: "agent:planner", requirements: ["REQ-12"]})
    .node("draft", {type: "my.task"})
    .entry("draft").exit("draft");
}
```

`.metadata(object)` は、グラフごとに1回だけ呼び出せます。Core定義には、トップレベルの `"metadata"` オブジェクトとして保存されます。空のオブジェクトは、指定しないのと同じです。

- **上限。** 正規化したJSONで最大8 KiB、`metadata` 以下の入れ子の深さは4、オブジェクトのメンバーは合計64個まで、キーは1〜128バイトです。`neograph.` で始まるトップレベルのキーは予約されています。
- **テキスト。** キーと文字列には、不可視文字を含められません。対象は、タブ・改行・復帰以外のC0制御文字、双方向（bidi）制御文字のU+061C・U+200E・U+200F・U+202A〜U+202E・U+2066〜U+2069、ゼロ幅文字のU+200B〜U+200D・U+2060・U+FEFF、タグ文字のU+E0000〜U+E007Fです。
- **同一性。** メタデータは定義の一部なので、`definition_hash` とバンドルIDが変わります。キーの順序を変えても変わりません。
- **読み取り。** 読めるのはホストだけで、シール済みの定義から読みます。C++では `bundle.sealed_core_definitions()[0].definition["metadata"]`、Pythonでは `bundle.sealed_core_definitions[0]["definition"]["metadata"]` です。たとえば、`LocalProgramHost.compile()` が返すバンドルで使えます。ノードファクトリー、実行状態、チェックポイント、出力、イベントからは見えません。
- **権限。** メタデータに記録されるのは、作成者の主張です。権限は何も与えません。委任の上限は、ホストがadmissionした `RunBudget`（`max_child_depth`、`max_total_children`、`max_concurrency`）で決まります。
- **マイグレーション。** メタデータは `GraphSemanticMigrationAdapter` のトポロジーの形に含まれないため、変更してもマイグレーションは妨げられません。`metadata_changes()` は、追加、削除、変更されたポインターを、承認するホスト向けに一覧にします。

エラーは次のとおりです。2回目の呼び出しや、オブジェクトでない引数には `P_JS_GRAPH_ARGUMENT`。ほかに、`P_CORE_GC_FIELD_TYPE`、`P_CORE_GC_METADATA_LIMIT`、`P_CORE_GC_METADATA_TEXT`、`P_CORE_GC_FIELD_VALUE`（空のキーや予約されたキー）があります。

## any_ofによるjoinのスロット

フラグ：`barrier.any_of`。

```js
graph.conditionalEdge("B", "t.check", {pass: "D", fail: "FIX"})
  .edge("FIX", "D").edge("C", "D")
  .barrier("D", [{any_of: ["B", "FIX"]}, "C"]);
```

`barrier(node, waitFor)` の各項目は、ノード名か、スロット `{any_of: [...]}` です。joinは、これまでどおり、スロットに対するANDのjoinです。

- スロットは、選択肢のどれか1つがjoinにシグナルを送ると埋まります。すべてのスロットが埋まるとjoinが実行され、通常のバリアと同じく、すべてのシグナルがクリアされます。
- スロットに含まれないノードからのシグナルは記録されますが、数には入りません。`Command.goto` と `Send` は、従来どおりjoinを迂回します。
- 同じスーパーステップに、またはjoinが実行される前に、2つの選択肢が届いても、何も変わりません。joinが実行された後に届いた選択肢は、新しいラウンドを開始します。joinにほかのスロットがあれば、このラウンドは完了しないため、実行は `P_CORE_BARRIER_STALLED` で終わります（フェイルクローズ）。スロットが1つだけなら、遅れて届いた選択肢がすぐに新しいラウンドを満たし、joinがもう一度実行されます。したがって、選択肢どうしはルートが排他的になるようにしてください。2つが両方とも実行されうる場合は、`P_CORE_E17` が警告します。
- Core定義では、スロットは `{"any_of": [...]}` の形で出力され、名前はUTF-8のバイト順に並びます。スロットどうしは、各スロットの最小の名前の順に並びます。名前が1つだけのスロットは、その名前だけで出力されます。そのため、`[{any_of: ["B"]}, "C"]` は、`["B", "C"]` とバイト列もIDも同じになります。
- スロット付きのjoinが停止した場合、witnessにスロットが一覧されます。`wait_for` と `missing` には、名前と `{"any_of": [...]}` のオブジェクトが入ります。
- バリデーターは、スロットごとに検査します。どの選択肢もjoinへルーティングしない場合は `P_CORE_E8`、メンバーが別の場所へルーティングしうる場合は `P_CORE_E15` になります。同じスロットの別の選択肢へのルートは、迂回には当たりません。

エラーは次のとおりです。他の形の項目には `P_JS_GRAPH_ARGUMENT`。空または文字列でない `any_of`、`any_of` と並ぶキー、入れ子の `any_of`、スロット内またはスロット間で繰り返されたノードには、`P_CORE_GC_BARRIER` が出ます。

## witnessのパスとルート表

- **迂回パス。** `P_CORE_E15` のwitnessには `paths` があります。迂回ルートごとに、メンバーからそのルートを通る一連のルートが入り、joinに戻る場合は、joinで終わります。joinに戻る場合は、joinの直前のノードがメンバーの代わりになれます。そのためwitnessに `substitute` と `suggested_wait_for` が加わり、返される診断は、`wait_for` をこれに置き換えるよう提案します（適用可能性は `maybe`）。#357の形では、提案は `[{"any_of": ["B", "FIX"]}, "C"]` です。すでに別のスロットを埋めているノードが提案されることはありません。
- **停止パス。** 停止したjoinのwitness（`output["_neograph"]["barrier_stalled"]` と、Programの失敗に含まれる `stalled_barriers`）には、`paths` が加わります。これは、実行された未到着のメンバーごとに、joinにシグナルを送ったノードまでのルートで、トレースと、ルーティング済みの後続ノードのマップから求めます。提案がある場合は、同じ `suggested_wait_for` も入ります。
- **ルート表。** `ProgramCompilerConfig::echo_routes`（Python：`ProgramCompiler(..., echo_routes=True)`）を指定すると、`P_CORE_ROUTES` の注記が返ります。その `witness.routes` は、各ノードを、スケジューラーがルーティングしうる先に対応付けます。最初の条件付きエッジのルート、なければ静的エッジです。シールされることはないので、指定してもしなくても、バンドルは同じです。

## 失敗のルーティング

フラグ：`retry_policy.per_node`、`failure_edges`、`failure_exits`、`retry.attempt_timeout`。

```js
ng.graph("fix")
  .channel("failures", {reducer: "append", initial: []})
  .node("IMPLEMENT", {type: "t.task"}).node("TEST", {type: "t.task"})
  .node("ASK_USER", {type: "t.review"}).node("ABORT", {type: "t.task"})
  .entry("IMPLEMENT").edge("IMPLEMENT", "TEST").exit("TEST")
  .conditionalEdge("ASK_USER", "t.decision", {retry: "IMPLEMENT", abort: "ABORT"})
  .retryPolicy("TEST", {max_retries: 2, non_retryable: ["app.compile_error"], max_visits: 3,
                        on_failure: "ASK_USER", error_channel: "failures"})
  .interruptBefore("ASK_USER")
  .failureExit("ABORT", {code: "plan.user_abort"});
```

TESTは3回試行されます。それでも失敗した場合、実行は失敗しません。TESTはASK_USERにルーティングされ、ASK_USERは失敗レコードを添えて、人間の回答を待つために一時停止します。`retry` と答えると、IMPLEMENTとTESTがもう一度実行されます。`abort` と答えると、failure exitであるABORTが実行され、実行は `plan.user_abort` で `Failed` として終わります。`Completed` と報告されることはありません。

`retryPolicy(node, {...})` は、2つのCoreキーに変換されます。タイミングとポリシーのフィールドは `node_retry_policies` に、`on_failure` と `error_channel` は `failure_edges` に入ります。`failureExit(node, {code})` は `failure_exits` を書き込みます。使わないキーは存在しません。グラフ単位の `retryPolicy({...})` は変わりません。

```json
"node_retry_policies": {"TEST": {"max_retries": 2, "non_retryable": ["app.compile_error"], "max_visits": 3}},
"failure_edges": [{"from": "TEST", "to": "ASK_USER", "error_channel": "failures"}],
"failure_exits": {"ABORT": {"code": "plan.user_abort"}}
```

### キー

- **ノードごとのポリシー。** 書いたフィールドだけが保存されます。値の範囲は、`max_retries` が0〜10、`initial_delay_ms` が10〜60 000、`backoff_multiplier` が1.0〜10.0、`max_delay_ms` が `initial_delay_ms` から300 000まで、`non_retryable` が重複しないコードで最大16個、`max_visits` が1〜100、`attempt_timeout_ms` が100〜3 600 000です。`error_text` もあります。範囲外の値は、`{min, max, actual}` を持つ `P_CORE_GC_FIELD_VALUE` になります。
- **コード。** 作成者のコードは `[a-z][a-z0-9_]*(\.[a-z0-9_]+)+` に一致する必要があります。`NG.` の接頭辞は予約されています。`non_retryable` には、分類器のコード（`NG.TaskFailed`、`NG.Timeout`、`NG.SubgraphFailed`、およびリファレンスのノード失敗契約にあるその他のコード）も指定できます。
- **failure edge。** ノードごとに最大1つで、`from` の順に並べます。行き先には、`__start__`、`__end__`、そのノード自身、join、およびエッジもルートもないノード（failure exitを除く）を指定できません（`P_CORE_GC_FAILURE_EDGE`）。`error_channel` は、`append` リデューサーを使う必要があります。
- **failure exit。** コードのデフォルトは `NG.Aborted` です。failure exitのノードには、エッジもルートもありません。
- **検証。** 未知のノードとチャネルは `P_CORE_E3` になります。failure edgeは、ルーティング済みの後続ノードのマップに加わります。そのため、到達可能性、終了、バリアの迂回（`P_CORE_E15`、ルート `on_failure`）の検査に反映され、`error_channel` は書き込み済みとして扱われます。ノードタイプが宣言していない `retry_policy` キーがノード設定の中にあると、`P_CORE_E18` として報告されます。グラフ単位の `retryPolicy({...})` に `on_failure` を書くと、ノードごとの書き方を案内するヒント付きの `P_CORE_GC_UNKNOWN_FIELD` になります。
- **マイグレーション。** `failure_edges` と `failure_exits` は、マイグレーションアダプターのトポロジーの形に含まれます。`node_retry_policies` は含まれません。

### 実行時

- **リトライのタイミング**は、フィールドごとに決まります。まず、ホストのデフォルト（`EngineConfig::retry_policy`、`set_retry_policy`）を使い、なければグラフの `retryPolicy`、それもなければ組み込みのデフォルトを使います。そのうえで、ノード自身のフィールドで上書きします。ホストがノードごとに設定したポリシー（`EngineConfig::node_retry_policies`、`set_node_retry_policy`）は、ノードごとのポリシーを持たないノードについては、これまでどおり、この結果を置き換えます。ノードごとのポリシーを持つノードについては、ホストは各フィールドを厳しくする方向にしか変えられません。`max_retries` は小さいほう、遅延と倍率は大きいほうが採用されます。このようなノードが、10回を超えてリトライすることはありません。結果は `GraphEngine::resolved_retry_policy(node)` で確認できます。
- **何をリトライし、何をルーティングするか**は、分類器が判断します（リファレンスのノード失敗契約）。`non_retryable` に挙げたコードはリトライされません。ノードの内側で起きたProgramの診断（`ProgramDiagnosticError`、またはコンパイルやadmissionのエラー）は `NG.ConfigError` になり、リトライもルーティングもされません。
- **処理された失敗。** ノードがルーティング可能な失敗で処理を断念し、そのノードにfailure edgeがある場合、実行は続きます。ノードの失敗した試行は、何も書き込みません。`error_channel` がある場合は、そのノードのスーパーステップの中で、レコードが1件追加されます。ノードは、edgeの行き先にだけルーティングされ、自分のエッジやルートにはシグナルを送りません。同じスーパーステップの並列ノードは、通常どおり結果を反映します。そのスーパーステップの `Command.goto` は、失敗ルートを含むすべてのルートに、引き続き優先します。失敗による到着が、joinのスロットを埋めることはありません。実行されたハンドラーが、#357の形のように、メンバーの代わりになれます。ノードは、トレースと `interruptAfter` では、実行されたものとして数えられます。保留中の書き込みには、処理済みの結果が記録されるので、クラッシュ後の再開では、ノードを再実行せずにルートがリプレイされます。静的ノード、fan-outの分岐、1つまたは複数の `Send` のターゲット、サブグラフのノードは、いずれも同じように動作します。ルーティングできない失敗や、failure edgeのないノードの失敗は、これまでどおり実行を失敗させます。
- **レコード**には、構造化されたフィールドだけが入ります。
  ```json
  {"node": "TEST", "kind": "exhausted", "code": "NG.TaskFailed", "attempts": 3, "retryable": true,
   "step": 1, "routed_to": "ASK_USER", "effects": []}
  ```
  `kind` は、`exhausted`、`non_retryable`、`effect_committed`、`ceiling`、`visit_limit`、`deadline` のいずれかです。最後の1つは、バックオフが実行のデッドラインを越えてしまう場合です。`effects` には、最後の試行で仲介（mediate）されたToolの効果が、`{tool, ordinal, status}` の形で並びます。
- **自由記述のテキスト**は、ノードごとに `error_text: true` を指定して有効にします。ホストのredactorも必要です。`EngineConfig::failure_text_redactor`、`CatalogConfig::failure_text_redactor`、またはPythonの `GraphEngine.compile` と `LocalProgramHost` の `failure_text_redactor=` で渡します。テキストの出どころは、型付きの公開メッセージ（`NodeFailureError` の `public_message`、またはPythonの例外の `public_message` 属性）だけです。例外のテキストが使われることはありません。redactorは、残すテキストを返します。エンジンは、C0制御文字、双方向制御文字、ゼロ幅文字、タグ文字を取り除き、UTF-8の文字境界で512バイトに切り詰めて、`"detail": {"untrusted_text": "..."}` として保存します。redactorがなければ、テキストは書き込まれません。
- **割り込み。** 行き先に `interruptBefore` がある場合、割り込みの値には `{"reason": "failure_route", "failure": record}` が入ります。Programでは、`result.interrupt.value` と `result.interrupt.pending_input.core_interrupt_value` がこれにあたります。
- **イベント。** 処理を断念したノードの `ERROR` イベントには、`handled`（失敗がルーティングされた場合はtrue）があります。ルーティングされた場合は、`routed_to` もあります。
- **ループの上限：`max_visits`。** これを宣言したノードは、最大でその回数だけ実行されます。次の起動は実行されず、`NG.VisitLimit` で失敗します（`kind` は `visit_limit`、試行は0回）。この失敗は、ノードのfailure edgeに沿ってルーティングされます。failure edgeがなければ、実行が失敗します。カウンターは、failure edgeの行き先が完了するとリセットされます。カウンターは、チェックポイントのメタデータに `_neograph.node_visits` として保存されます。保存されるのは、宣言したノードの分だけです。ステップ予算は、引き続き全体の最後の安全装置です。
- **failure exit。** failure exitのノードが完了すると、そのスーパーステップの後に実行が終わります。このとき、`output["_neograph"]["failure_exit"] = {node, code}` と `RunStatus::FailureExit` が設定されます（Pythonでは `RunStatus.FailureExit`。`RunInvocation` は `Error` を報告します）。Programの実行は、宣言したコードで `Failed` として終わり、`core_node` にはそのノードが入ります。
- **リトライの上限。** `RunBudget::max_node_retries`（Pythonでは `ProgramRunBudget.max_node_retries`）は、実行全体でのノードのリトライ回数を制限します。対象は、すべてのCore呼び出し、ノード、分岐、`Send`、サブグラフです。上限に達すると、`kind` が `ceiling` でリトライをやめます。実際に使ったリトライ回数は `ProgramUsage::node_retries`、残りは `remaining_budget.max_node_retries` です。指定がなければ上限はありません。どちらのフィールドも、設定されている場合にだけシリアライズされます。`ng.spawn` で開始した子は、親の上限を引き継ぎます。Programの外では、`NodeRetryCeiling` を `RunMetadata::node_retry_ceiling` として渡します。
- **Programでルーティングされない失敗。** failure edgeのないノードが失敗すると、Programは `P_RUNTIME_CORE_FAILURE` ではなく、分類されたコード（`NG.TaskFailed`、作成者のコード、`NG.ProviderFailed` など）で `Failed` として終わります。`failure.witness.node_failure` には、`{node, code, kind, attempts, retryable, routable}` が入ります。オーケストレーションプランの `retry` オペレーションも、同じ分類器に問い合わせます。本体をもう一度実行するのは、リトライ可能な失敗の後だけです。コミット済みの効果の後、リトライしない種別の後、リトライの上限に達した後、デッドラインで止まった後には、再実行しません。

### 試行タイムアウトとデッドライン

フラグ：`retry.attempt_timeout`。

```js
graph.retryPolicy("TEST", {attempt_timeout_ms: 30000, max_retries: 2, on_failure: "ASK_USER"});
```

- **タイムアウトした試行。** `attempt_timeout_ms` は、ノードの試行ごとに制限をかけます。時間を過ぎると、その試行は、自分用のキャンセルトークン（ノードの `ctx.cancel_token` で、実行のトークンの子）によってキャンセルされ、`NG.Timeout` で失敗します。この失敗は、通常の失敗と同じように、リトライもルーティングもできます。最初のタイムアウトで止めたい場合は、`NG.Timeout` を `non_retryable` に挙げます。先に起きた終了イベントが優先されます。タイムアウト時刻の時点またはその後に完了した試行は、タイマーがまだ発火していなくても、タイムアウトしたものとして扱われます。
- **遅れて完了した結果は捨てられます。** タイムアウトした試行が、その後に何をしても反映されません。書き込みと結果は破棄され、その試行が発行したイベントは、実行のコールバックに届きません。その試行が開始する、仲介されたToolの呼び出しは、トークンがキャンセル済みなので拒否されます。次の試行が始まるのは、キャンセルされた試行が止まってからです。したがって、1つのノードの2つの試行が重なることはありません。
- **停止の期限。** キャンセルされた試行には、実行に与えられていたのと同じ長さ、ただし最低1秒の、停止までの猶予があります。それまでに止まらない試行は、まだ効果を実行している可能性があります。その場合、ノードは `NG.EffectUncertain` で失敗し、リトライもルーティングもされないので、実行は失敗します。その試行はバックグラウンドで動き続け、戻るまでノードを生かしたままにします。`wait_for_node_attempts()` は、そうした試行の終了を待ちます。Pythonのバインディングは、インタープリターの終了時にこれを呼びます。冪等でない効果をコミットしてから止まった試行は、従来どおり `NG.EffectCommitted` で失敗します。仲介された効果を不確かなまま残した試行は、照合のために実行を一時停止させます。
- **すべての経路で同じです。** 静的ノード、fan-outの分岐、1つまたは複数の `Send` のターゲット、サブグラフのノードは、同じ経路をたどります。サブグラフのノードの子の実行は、その試行と一緒にキャンセルされます。タイムアウトした試行の後にクラッシュした場合は、実行中だった試行から再開します。再開時に、タイムアウトした試行の結果が反映されることはありません。
- **ホスト側のタイムアウト。** `GraphEngine::set_node_attempt_timeout_ms(node, ms)`（Pythonでは `set_node_attempt_timeout_ms`、`None` で解除）は、定義にタイムアウトがないノードにタイムアウトを設定します。定義にタイムアウトがあるノードについては、ホストは短くする方向にしか変えられず、2つのうち短いほうが適用されます。結果は `resolved_attempt_timeout_ms(node)` で確認できます。
- **ノード本体が動く場所。** 実行を駆動するコルーチンは、タイムアウトのあるノード本体を実行しません。Programの実行では、どのノード本体も実行しません。そうした本体は、プロセス全体で共有するワーカープール（`NEOGRAPH_NODE_ATTEMPT_THREADS`、1〜1024、デフォルトは32）で実行されます。そのため、スレッドをブロックする本体が、自分自身のタイムアウトや、joinのメンバーと `await` のタイマーが共有するProgramスケジューラーを巻き込んで止めてしまうことはありません。これはPythonのノードだけでなく、C++のノードとCoreのサブグラフにも当てはまります。それ以外の `GraphEngine` の実行では、タイムアウトのない本体は、これまでどおり呼び出し元のスレッドで実行されます。
- **デッドラインを考慮したバックオフ。** 次の試行の前のバックオフが、実行のデッドライン（Programでは `wall_time_ms`、Programの外では `RunMetadata::deadline`）を越えてしまう場合、ノードは、予算が実行を終わらせるまで待つのではなく、ただちに `kind` を `deadline` にしてリトライをやめます。失敗は元のコード（`NG.TaskFailed`、`NG.Timeout` など）のままで、ノードのfailure edgeに沿ってルーティングされます。failure edgeがなければ、実行が失敗します。

## 健全性チェック

フラグ：`diagnostics.soundness_check`。`ProgramCompilerConfig::soundness_check`（Python：`ProgramCompiler(..., soundness_check=True)` または `LocalProgramHost(..., soundness_check=True)`）を指定すると、ルートグラフについて、ルートが許すすべての実行を探索し、見つかった内容を返します。デフォルトではオフです。ルート表と同じく、シールされることはないので、有効にしてもしなくても、バンドルは同じです。C++で `GraphValidator` を呼ぶ場合は、`ValidationOptions::soundness_check` を設定します。

このチェックは、スケジューラーと同じ方法でスーパーステップを進めます。状態は、実行可能なノードの集合、各joinが保持しているシグナル（`any_of` のスロットを含む）、`max_visits` を持つノードごとの訪問回数です。訪問回数は、実行時と同じく、そのノードのfailure edgeの行き先が完了したときにだけリセットされます。条件付きエッジは、そのどのルートも取りえます。failure edgeを持つノードは、代わりにそのedgeを取ることがあります。failure exitに達すると、実行は終わります。検出結果はどれも警告で、その `witness.trace` は、それを示す最短の実行です。スーパーステップごとに1項目で、実行されたノード（`ready`）と、それらが取ったルート（`routes`。failure edgeは `on_failure`）が入ります。

| コード | 検出内容 |
|---|---|
| `P_CORE_S1` | ある時点から先では、どの実行も完了できません。後続の実行はすべて、バリア待ちで停止して終わるか、訪問回数の上限で失敗するか、ループします。 |
| `P_CORE_S2` | joinがシグナルを保持したまま、実行が終わります。バリア待ちで停止した場合と、明示的な終了でシグナルが捨てられた場合があります。 |
| `P_CORE_S3` | どの実行でも、joinが発火しません。 |
| `P_CORE_S4` | 実行が出口のないループに入ることがあり、その場合はステップ数の上限でしか止まりません。 |
| `P_CORE_S5` | overwriteのチャネルに書き込む2つのノードが、同じスーパーステップで実行されることがあります（`P_CORE_E5` が報告するfan-outの並列ノードは除きます）。 |

`P_CORE_SOUNDNESS` の要約は、チェックがどこまで進んだかを示します。すべての実行を探索した場合は注記、そうでない場合は警告です。探索していない範囲について、何かを主張することはありません。

- `Command.goto` や `Send` を返す可能性のあるノードタイプは、effect契約に `"control": ["goto"]` または `["send"]` として、それを宣言します。そのようなノードが実行可能になった後の実行は、解析できません。要約には、該当するノードが一覧されます。`P_CORE_S1` と `P_CORE_S4` は、後続のすべての実行を探索できた箇所についてだけ報告し、`P_CORE_S3` はまったく報告しません。このチェックは契約を信頼します。したがって、宣言せずに `Command.goto` を返すノードタイプは、宣言されたルートのとおりに解析されます。
- `soundness_state_cap`（デフォルトは100 000状態）で、作業量に上限をかけます。チェックがこの上限に達すると、要約は `not_checked` になり、それまでに見つかった `P_CORE_S2` と `P_CORE_S5` の検出結果だけが返ります。

#357の形で `barrier("D", ["B", "C"])` とした場合は、次のようになります。

```python
compiler = ng.ProgramCompiler(registry, soundness_check=True)
bundle = compiler.compile(ng.ProgramSource.from_javascript("plan.js", source))
stall = [d for d in bundle.compile_diagnostics if d["code"] == "P_CORE_S2"][0]
stall["witness"]["trace"]
# [{"ready": ["A"], "routes": {}},
#  {"ready": ["B", "C"], "routes": {"B": "fail"}},
#  {"ready": ["FIX"], "routes": {}}]
stall["witness"]["missing"]             # ["B"]
stall["witness"]["suggested_wait_for"]  # [{"any_of": ["B", "FIX"]}, "C"]
```

`P_CORE_S1` は、同じグラフについて、`B` が `fail` を取った時点から報告します。`barrier("D", [{any_of: ["B", "FIX"]}, "C"])` にすると、チェックは何も検出せず、要約の注記には、すべての実行を探索したと書かれます。各witnessについては、[診断ガイド](diagnostics.md#soundness-check)で説明しています。
