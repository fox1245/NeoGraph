<!-- neograph-i18n: source=docs/program-graphs.md locale=ja source_sha256=ba8de002d040f5f18052d1e8d7971509fa764fcf24fb3859a39cf510c181c2c2 -->
# Program グラフ機能

**Languages:** [English](program-graphs.md) | [한국어](program-graphs.ko.md) | [日本語](program-graphs.ja.md) | [简体中文](program-graphs.zh-CN.md)

このページでは、`define()` プログラムがノード、エッジ、barrier、グラフ単位のリトライポリシー以外に使えるグラフ機能を説明します。各機能は厳格な Core 定義（`schema_version` 1）のキーで、`javascript_authoring_capability_manifest()["features"]` にフラグがあります。機能を使わない定義は、その機能ができる前とまったく同じバイト列、`definition_hash`、バンドル id を保ちます。

これらのキーを 1 つでも使う定義は、バンドルにバリデーターの警告を封印しません。警告はコンパイル結果として引き続き返されます。[診断ガイド](diagnostics.md)を参照してください。

## グラフメタデータ

フラグ: `graph.metadata`。

```js
export function define() {
  return ng.graph("plan")
    .metadata({owner: "agent:planner", requirements: ["REQ-12"]})
    .node("draft", {type: "my.task"})
    .entry("draft").exit("draft");
}
```

`.metadata(object)` はグラフごとに 1 回だけ呼び出せます。Core 定義はこれを最上位の `"metadata"` オブジェクトとして保存し、空のオブジェクトは指定なしと同じです。

- **上限。** 正規 JSON で最大 8 KiB、`metadata` 以下の入れ子の深さ 4、オブジェクトメンバー合計 64、キーは 1〜128 バイトです。`neograph.` で始まる最上位キーは予約されています。
- **テキスト。** キーと文字列に不可視文字は使えません。タブ、改行、復帰以外の C0 制御文字、bidi 制御文字 U+061C、U+200E、U+200F、U+202A〜U+202E、U+2066〜U+2069、ゼロ幅文字 U+200B〜U+200D、U+2060、U+FEFF、そしてタグ文字 U+E0000〜U+E007F が該当します。
- **識別子。** メタデータは定義の一部なので、`definition_hash` とバンドル id が変わります。キーの順序は影響しません。
- **読み手。** 封印済みの定義からホストだけが読みます。C++ では `bundle.sealed_core_definitions()[0].definition["metadata"]`、Python では `bundle.sealed_core_definitions[0]["definition"]["metadata"]` で、例えば `LocalProgramHost.compile()` が返すバンドルから読みます。ノードファクトリー、実行状態、チェックポイント、出力、イベントはメタデータを見ません。
- **権限。** メタデータは作成者の主張を記録するだけで、何の権限も与えません。委任の上限はホストが承認する `RunBudget`（`max_child_depth`、`max_total_children`、`max_concurrency`）にあります。
- **マイグレーション。** メタデータは `GraphSemanticMigrationAdapter` のトポロジー形状に含まれないため、変更してもマイグレーションを妨げません。`metadata_changes()` は、承認するホストのために追加、削除、変更されたポインターを挙げます。

エラー: 2 回目の呼び出しやオブジェクト以外の引数には `P_JS_GRAPH_ARGUMENT`、ほかに `P_CORE_GC_FIELD_TYPE`、`P_CORE_GC_METADATA_LIMIT`、`P_CORE_GC_METADATA_TEXT`、`P_CORE_GC_FIELD_VALUE`（空のキーや予約されたキー）があります。

## any_of による join スロット

フラグ: `barrier.any_of`。

```js
graph.conditionalEdge("B", "t.check", {pass: "D", fail: "FIX"})
  .edge("FIX", "D").edge("C", "D")
  .barrier("D", [{any_of: ["B", "FIX"]}, "C"]);
```

`barrier(node, waitFor)` の各項目はノード名か、スロット `{any_of: [...]}` です。join は引き続きスロット単位の AND join です。

- スロットは、選択肢のいずれかが join にシグナルを送ると満たされます。すべてのスロットが満たされると join が実行され、通常の barrier と同じくすべてのシグナルがクリアされます。
- スロットに属さないノードのシグナルは記録されますが、数には入りません。`Command.goto` と `Send` は従来どおり join を迂回します。
- 2 つの選択肢が同じ super-step に、または join の実行前に到着しても何も変わりません。join の実行後に到着した選択肢は決して終わらない新しいラウンドを開くため、実行は `P_CORE_BARRIER_STALLED` で終わります（安全側の失敗）。したがって選択肢どうしは排他的にルーティングされるべきで、2 つが両方とも実行され得る場合は `P_CORE_E17` が警告します。
- Core 定義ではスロットは名前を UTF-8 のバイト順に並べた `{"any_of": [...]}` として出力され、スロットは最小の名前の順に並びます。名前が 1 つのスロットはその名前として出力されるため、`[{any_of: ["B"]}, "C"]` は `["B", "C"]` とバイト列も id も同じです。
- 止まったスロット join の witness はスロットを挙げます。`wait_for` と `missing` には名前と `{"any_of": [...]}` オブジェクトが入ります。
- バリデーターはスロットごとに検査します。どの選択肢も join へルーティングしなければ `P_CORE_E8`、メンバーが別の場所へルーティングし得るなら `P_CORE_E15` を出します。同じスロットの別の選択肢へのルートは迂回ではありません。

エラー: 他の形の項目には `P_JS_GRAPH_ARGUMENT`、空または文字列でない `any_of`、`any_of` と並ぶ別のキー、入れ子の `any_of`、スロット内やスロット間で繰り返されたノードには `P_CORE_GC_BARRIER` が出ます。

## witness の経路とルート表

- **迂回経路。** `P_CORE_E15` の witness には `paths` があります。迂回ルートごとに、メンバーからそのルートを通るルートの並びで、join に戻る場合は join で終わります。戻る場合は join の直前のノードがメンバーの代わりになれるため、witness に `substitute` と `suggested_wait_for` が加わり、返される診断は `wait_for` をそれに置き換えるよう提案します（適用可能性 `maybe`）。#357 の形なら提案は `[{"any_of": ["B", "FIX"]}, "C"]` です。すでに別のスロットを満たしているノードは提案しません。
- **停止経路。** 止まった join の witness（`output["_neograph"]["barrier_stalled"]`、および Program の失敗の `stalled_barriers`）に `paths` が加わります。実行された欠けたメンバーごとに、join にシグナルを送ったノードまでのルートで、トレースとルーティング済みの後続マップから求めます。該当する場合は同じ `suggested_wait_for` も入ります。
- **ルート表。** `ProgramCompilerConfig::echo_routes`（Python: `ProgramCompiler(..., echo_routes=True)`）は `P_CORE_ROUTES` ノートを返します。その `witness.routes` は各ノードを、スケジューラーがルーティングし得る先、つまり最初の conditional edge のルート、なければ静的な edge に対応付けます。封印されないため、どちらでもバンドルは同じです。

## 失敗のルーティング

フラグ: `retry_policy.per_node`、`failure_edges`、`failure_exits`。

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

TEST は 3 回試行されます。それでも失敗すると、実行は失敗しません。TEST は ASK_USER にルーティングされ、ASK_USER は失敗レコードを持って人の回答を待つために停止します。`retry` と答えると IMPLEMENT と TEST が再び実行され、`abort` と答えると failure exit である ABORT が実行されるため、実行は `plan.user_abort` で `Failed` として終わり、決して `Completed` を報告しません。

`retryPolicy(node, {...})` は 2 つの Core キーに変換されます。タイミングとポリシーのフィールドは `node_retry_policies` に、`on_failure` と `error_channel` は `failure_edges` に入ります。`failureExit(node, {code})` は `failure_exits` を書きます。各キーは使わなければ存在せず、グラフ単位の `retryPolicy({...})` は変わりません。

```json
"node_retry_policies": {"TEST": {"max_retries": 2, "non_retryable": ["app.compile_error"], "max_visits": 3}},
"failure_edges": [{"from": "TEST", "to": "ASK_USER", "error_channel": "failures"}],
"failure_exits": {"ABORT": {"code": "plan.user_abort"}}
```

### キー

- **ノードごとのポリシー。** 書いたフィールドだけが保存されます。`max_retries` 0〜10、`initial_delay_ms` 10〜60 000、`backoff_multiplier` 1.0〜10.0、`max_delay_ms` は `initial_delay_ms` から 300 000 まで、`non_retryable`（一意なコード最大 16 個）、`max_visits` 1〜100、`error_text` です。`attempt_timeout_ms` はまだ受け付けません。範囲外は `{min, max, actual}` を持つ `P_CORE_GC_FIELD_VALUE` です。
- **コード。** 作成者のコードは `[a-z][a-z0-9_]*(\.[a-z0-9_]+)+` に一致する必要があり、`NG.` 接頭辞は予約されています。`non_retryable` には分類器のコード（`NG.TaskFailed`、`NG.Timeout`、`NG.SubgraphFailed`、およびリファレンスのノード失敗契約にあるその他）も書けます。
- **failure edge。** ノードごとに最大 1 つで、`from` の順に並べられます。対象は `__start__`、`__end__`、ノード自身、join、そして failure exit ではないのに edge もルートもないノードであってはなりません（`P_CORE_GC_FAILURE_EDGE`）。`error_channel` は `append` reducer を使う必要があります。
- **failure exit。** コードの既定値は `NG.Aborted` です。failure exit のノードには edge もルートもありません。
- **検証。** 未知のノードとチャネルは `P_CORE_E3` です。failure edge はルーティング済みの後続マップに入るため、到達可能性、終了、barrier の迂回（`P_CORE_E15`、ルート `on_failure`）の検査がこれを見ます。また `error_channel` は書き込まれたものとして数えます。ノード型が宣言していないノード設定の `retry_policy` キーは `P_CORE_E18` として報告されます。グラフ単位の `retryPolicy({...})` に `on_failure` を書くと、ノードごとの形を示すヒント付きの `P_CORE_GC_UNKNOWN_FIELD` になります。
- **マイグレーション。** `failure_edges` と `failure_exits` はマイグレーションアダプターのトポロジー形状に含まれ、`node_retry_policies` は含まれません。

### 実行時

- **再試行のタイミング**はフィールドごとに決まります。ホストの既定値（`EngineConfig::retry_policy`、`set_retry_policy`）、なければグラフの `retryPolicy`、それもなければ組み込みの既定値から始め、ノード自身のフィールドで上書きします。ホストのノードごとのポリシー（`EngineConfig::node_retry_policies`、`set_node_retry_policy`）は、ノードごとのポリシーを持たないノードではこれまでどおり結果を置き換えます。持つノードでは各フィールドを厳しくすることしかできません。`max_retries` は小さい方、遅延と倍率は大きい方を使い、そのようなノードは 10 回を超えて再試行しません。結果は `GraphEngine::resolved_retry_policy(node)` で確認できます。
- **何を再試行し、何をルーティングするか**は分類器（リファレンスのノード失敗契約）が決めます。`non_retryable` に書かれたコードは再試行しません。ノードの中で発生した Program の診断（`ProgramDiagnosticError`、またはコンパイルや承認のエラー）は `NG.ConfigError` で、再試行もルーティングもされません。
- **処理された失敗。** ノードがルーティング可能な失敗であきらめ、failure edge を持つ場合、実行は続きます。失敗した試行は何も書かず、`error_channel` があればノードの super-step でそこにレコードが 1 つ追加され、ノードは自分の edge やルートには信号を送らず、edge の対象にだけルーティングされます。同じ super-step の兄弟ノードは通常どおり結果を適用し、その super-step の `Command.goto` は失敗ルートを含むすべてのルートより引き続き優先されます。失敗による到着は join のスロットを満たしません。#357 の形のように、実行されたハンドラーがメンバーの代わりになれます。ノードはトレースと `interruptAfter` では実行されたものとして数え、ノードの pending write が処理された結果を記録するため、クラッシュ後の再開はノードを再実行せずにルートを再生します。静的ノード、ファンアウトの分岐、1 つまたは複数の `Send` の対象、サブグラフのノードはすべて同じように動きます。ルーティングできない失敗や、failure edge のないノードの失敗は、これまでどおり実行を失敗させます。
- **レコード**には構造化されたフィールドだけが入ります。
  ```json
  {"node": "TEST", "kind": "exhausted", "code": "NG.TaskFailed", "attempts": 3, "retryable": true,
   "step": 1, "routed_to": "ASK_USER", "effects": []}
  ```
  `kind` は `exhausted`、`non_retryable`、`effect_committed`、`ceiling`、`visit_limit`、`deadline`（試行タイムアウト用に予約）のいずれかです。`effects` は最後の試行の仲介された Tool の効果を `{tool, ordinal, status}` で並べます。
- **自由テキスト**はノードごとに `error_text: true` で選び、ホストの redactor が必要です。`EngineConfig::failure_text_redactor`、`CatalogConfig::failure_text_redactor`、または Python の `GraphEngine.compile` と `LocalProgramHost` の `failure_text_redactor=` です。出所は型付きの公開メッセージ（`NodeFailureError` の `public_message` や Python の例外の `public_message` 属性）だけで、例外のテキストは決して使いません。redactor が残すテキストを返すと、エンジンは C0、双方向、ゼロ幅、タグの文字を取り除き、UTF-8 の境界で 512 バイトに切り詰め、`"detail": {"untrusted_text": "..."}` として保存します。redactor がなければテキストは書きません。
- **割り込み。** 対象に `interruptBefore` があると、割り込みの値に `{"reason": "failure_route", "failure": record}` が入ります。Program では `result.interrupt.value` と `result.interrupt.pending_input.core_interrupt_value` です。
- **イベント。** あきらめたノードの `ERROR` イベントには `handled`（失敗がルーティングされたら true）と、ルーティングされた場合は `routed_to` があります。
- **ループの上限: `max_visits`。** これを宣言したノードは最大でその回数だけ実行されます。次の活性化は実行されず `NG.VisitLimit`（`kind` `visit_limit`、試行 0 回）で失敗し、ノードの failure edge に沿ってルーティングされるか、edge がなければ実行を失敗させます。カウンターは failure edge の対象が完了するとリセットされます。カウンターは宣言したノードについてだけ、チェックポイントのメタデータの `_neograph.node_visits` にあります。ステップ予算は引き続き全体の安全装置です。
- **failure exit。** failure exit のノードが完了すると、実行はその super-step の後に `output["_neograph"]["failure_exit"] = {node, code}` と `RunStatus::FailureExit`（Python は `RunStatus.FailureExit`、`RunInvocation` は `Error`）で終わります。Program の実行は宣言されたコードで `Failed` になり、`core_node` はそのノードです。
- **再試行の上限。** `RunBudget::max_node_retries`（Python は `ProgramRunBudget.max_node_retries`）は実行全体、つまりすべての Core 呼び出し、ノード、分岐、`Send`、サブグラフのノード再試行を制限します。上限に達すると `kind` `ceiling` で再試行をやめます。使った再試行は `ProgramUsage::node_retries`、残りは `remaining_budget.max_node_retries` です。なければ上限はなく、2 つのフィールドは設定されたときだけシリアライズされます。`ng.spawn` で始めた子は親の上限を受け継ぎます。Program の外では `NodeRetryCeiling` を `RunMetadata::node_retry_ceiling` として渡します。
- **Program でルーティングされない失敗。** failure edge のないノードの失敗は、Program を `P_RUNTIME_CORE_FAILURE` ではなく分類されたコード（`NG.TaskFailed`、作成者のコード、`NG.ProviderFailed` など）で `Failed` にし、`failure.witness.node_failure` には `{node, code, kind, attempts, retryable, routable}` が入ります。オーケストレーション計画の `retry` 操作も同じ分類器に問い合わせます。再試行可能な失敗の後でだけ本体を再実行し、コミットされた効果、再試行しないクラス、到達した再試行上限の後には再実行しません。

## 健全性チェック

フラグ: `diagnostics.soundness_check`。`ProgramCompilerConfig::soundness_check`（Python: `ProgramCompiler(..., soundness_check=True)` または `LocalProgramHost(..., soundness_check=True)`）は、ルートグラフのルートが許すすべての実行を探索し、見つけたものを返します。既定ではオフで、ルート表と同じく封印されないため、どちらでもバンドルは同じです。`GraphValidator` を使う C++ の呼び出し側は `ValidationOptions::soundness_check` を設定します。

チェックはスケジューラーと同じように super-step を進めます。状態は、実行準備のできたノードの集合、各 join が保持するシグナル（`any_of` スロットを含む）、そして `max_visits` を持つノードごとの訪問回数です。訪問回数は実行時と同じく、そのノードの failure edge の対象が完了したときだけリセットされます。conditional edge はどのルートも取り得て、失敗 edge を持つノードは代わりにその edge を取り得て、failure exit は実行を終えます。検出結果はすべて警告で、`witness.trace` はそれを示す最短の実行を super-step ごとに 1 項目ずつ並べたものです。実行されたノード（`ready`）と、それらが取ったルート（`routes`、失敗 edge は `on_failure`）が入ります。

| コード | 検出結果 |
|---|---|
| `P_CORE_S1` | ある時点から、どの実行も完了できません。後続の実行はすべて barrier-stalled で終わるか、訪問上限で失敗するか、ループします。 |
| `P_CORE_S2` | join がまだシグナルを保持したまま実行が終わります。barrier-stalled で終わるか、明示的な終了がシグナルを捨てます。 |
| `P_CORE_S3` | join がどの実行でも発火しません。 |
| `P_CORE_S4` | 実行が出口のないループに入り得て、その場合は step 上限でしか止まりません。 |
| `P_CORE_S5` | 1 つの overwrite チャネルに書く 2 つのノードが同じ super-step で実行され得ます（`P_CORE_E5` が報告する fan-out の兄弟以外）。 |

`P_CORE_SOUNDNESS` の要約は、チェックがどこまで行ったかを示します。すべての実行を探索した場合はノート、そうでない場合は警告です。探索していないことは決して主張しません。

- `Command.goto` または `Send` を返し得るノードタイプは、effect 契約に `"control": ["goto"]` または `["send"]` としてそれを宣言します。そのようなノードが準備された後の実行は解析できません。要約がそれらのノードを列挙し、`P_CORE_S1` と `P_CORE_S4` はすべての後続の実行を探索した箇所でのみ報告し、`P_CORE_S3` はまったく報告しません。チェックは契約を信頼するため、宣言せずに `Command.goto` を返すノードタイプは宣言されたルートどおりに解析されます。
- `soundness_state_cap`（既定 100 000 状態）が作業量を制限します。チェックがそこに達すると要約は `not_checked` となり、それまでに見つかった `P_CORE_S2` と `P_CORE_S5` の検出結果だけが返されます。

`barrier("D", ["B", "C"])` の #357 の形の場合:

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

`P_CORE_S1` は同じグラフを、`B` が `fail` を取った時点から報告します。`barrier("D", [{any_of: ["B", "FIX"]}, "C"])` ならチェックは何も見つけず、要約のノートはすべての実行を探索したことを示します。各 witness は[診断ガイド](diagnostics.md#soundness-check)で説明しています。
