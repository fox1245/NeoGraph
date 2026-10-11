<!-- neograph-i18n: source=docs/program-graphs.md locale=ja source_sha256=8082d68c638414c879b2854352e5fd4c95bb82f0ac014d1efa11b852de5991e1 -->
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
