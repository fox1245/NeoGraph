<!-- neograph-i18n: source=docs/diagnostics.md locale=ja source_sha256=c4b7c36f1ce541c78785e7cd73889179b47e0c2fac4207ecf4eff50932331aa6 -->
# Program 診断

**Languages:** [English](diagnostics.md) | [한국어](diagnostics.ko.md) | [日本語](diagnostics.ja.md) | [简体中文](diagnostics.zh-CN.md)

Program のコンパイルは診断を返します。失敗時は `ProgramCompileError::diagnostics()`、成功時は `ProgramBundle::compile_diagnostics()`、Python の例外では `.diagnostics` で受け取ります。このページでは各コードの意味と修正方法を説明します。返されるすべての診断は `help.doc` でこのページの該当項目を指します。

- [help オブジェクト](#the-help-object)
- [Program ドキュメント](#program-document)
- [JavaScript フロントエンド](#javascript-frontend)
- [Core パーサー](#core-parser)
- [Core バリデーター](#core-validator)
- [レジストリ](#registry)
- [ランタイムとアドミッション](#runtime-and-admission)

<a id="the-help-object"></a>
## help オブジェクト

返される診断には `help` があり、ツールやモデルはメッセージを解析せずにそのまま対処できます。

| フィールド | 内容 |
|---|---|
| `doc` | このページにあるコードの項目、またはそのコード系列のセクションへのリンク。 |
| `related` | 同じ根本原因が現れる他の位置。それぞれに 1 行の `label` が付きます。 |
| `suggestions` | 修正案。それぞれ `message`、`edits`、`applicability` を持ちます。 |
| `candidates` | 未知の名前から編集 2 回以内にある宣言済みの名前。近い順です。 |

`edits` は、コンパイラーが解析した Program ドキュメントに対する RFC 6902 JSON Patch 操作（`add`、`replace`、`remove`、`move`）です。JavaScript ソースの場合は `define()` が生成したドキュメントで、ビルダー呼び出しの位置は診断の span が示します。`applicability` は `machine`（そのまま適用）、`maybe`（先に確認する。既定値）、`placeholder`（`<node>` などの値を埋める）のいずれかです。

バンドルに封印される診断には `help` が含まれないため、バンドル id は `help` に依存しません。コンパイルは根本原因ごとに診断を 1 つ返します。複数の診断が同じ未知の名前を報告した場合は最初の 1 つだけを残し、残りはその診断の `related` 項目になります。返される診断はエラーを先頭にして最大 20 件で、残りの件数は `P_DIAGNOSTICS_TRUNCATED` ノートが示します。

メッセージは書かれたとおりの形で名前を示します。JavaScript ソースでは `graph.barrier("D", [...])` のようなビルダー呼び出し、JSON または C++ ビルダーのドキュメントでは Core のキーで表します。

<a id="program-document"></a>
## Program ドキュメント

| コード | 意味と修正方法 |
|---|---|
| <a id="p_schema_version"></a>`P_SCHEMA_VERSION` | Program、契約、埋め込み Core 定義のスキーマバージョンがないか、サポートされていません。witness に挙げられたバージョンを使ってください。 |
| <a id="p_schema_unknown_field"></a>`P_SCHEMA_UNKNOWN_FIELD` | 閉じた Program オブジェクトに定義されていないフィールドがあります。削除するか候補名を使ってください。 |
| <a id="p_schema_required"></a>`P_SCHEMA_REQUIRED` | 必須の Program フィールドがありません。追加してください。 |
| <a id="p_schema_type"></a>`P_SCHEMA_TYPE` | Program フィールドの JSON 型が誤っているか、ソースマップでポインターが重複しています。witness の型を使ってください。 |
| <a id="p_schema_operation"></a>`P_SCHEMA_OPERATION` | この操作にはより新しい Program スキーマバージョンが必要です。`program_schema_version` を上げるか、別の操作を使ってください。 |
| <a id="p_contract_schema"></a>`P_CONTRACT_SCHEMA` | 入力または出力の契約が無効な、またはサポートされていない JSON Schema キーワードを使っています。 |
| <a id="p_root_name"></a>`P_ROOT_NAME` | ルート名がないか、空か、Core 定義の `name` と異なります。両者を一致させてください。 |
| <a id="p_root_operation"></a>`P_ROOT_OPERATION` | `call_core` ルートに Core ノードがありません。ノードを 1 つ以上宣言してください。 |
| <a id="p_plan_operation"></a>`P_PLAN_OPERATION` | サポートされていない操作です。 |
| <a id="p_plan_core"></a>`P_PLAN_CORE` | プランが封印済みルート以外の Core を呼び出しているか、Core を呼ばずに子もディスパッチしていません。 |
| <a id="p_plan_bound"></a>`P_PLAN_BOUND` | loop、retry、timeout の上限が範囲外です。 |
| <a id="p_plan_condition"></a>`P_PLAN_CONDITION` | プラン条件の述語がちょうど 1 つではないか、パスが JSON Pointer ではありません。 |
| <a id="p_plan_race_arity"></a>`P_PLAN_RACE_ARITY` | race の分岐が 2 つではありません。 |
| <a id="p_plan_quorum"></a>`P_PLAN_QUORUM` | `min_success` が 0 か、分岐数より大きいです。 |
| <a id="p_plan_spawn_shape"></a>`P_PLAN_SPAWN_SHAPE` | spawn にインライン本体があるか、await の直接の本体ではありません。 |
| <a id="p_plan_cancel_scope"></a>`P_PLAN_CANCEL_SCOPE` | cancel は `run` スコープのみをサポートします。 |
| <a id="p_plan_reference"></a>`P_PLAN_REFERENCE` | 封印済みの操作が存在しない操作を参照しています。コンパイラーの不具合なので報告してください。 |
| <a id="p_plan_source_pointer"></a>`P_PLAN_SOURCE_POINTER` | 封印済みの操作のソースポインターが不正です。コンパイラーの不具合なので報告してください。 |
| <a id="p_plan_typed"></a>`P_PLAN_TYPED` | プランを型付きの操作として封印できませんでした。詳細は witness にあります。 |
| <a id="p_parallel_map_source"></a>`P_PARALLEL_MAP_SOURCE` | `item_source` には上限付きのリテラル配列か入力アーティファクトのフィールドが必要です。 |
| <a id="p_parallel_map_binding"></a>`P_PARALLEL_MAP_BINDING` | `parallel_map` のバインディングやエンドポイントがないか、JSON Pointer ではありません。 |
| <a id="p_parallel_map_bound"></a>`P_PARALLEL_MAP_BOUND` | `parallel_map` の上限が範囲外か、`max_in_flight` が `max_items` を超えています。 |
| <a id="p_parallel_map_failure"></a>`P_PARALLEL_MAP_FAILURE` | `failure_policy` は `fail_fast` か `collect` でなければなりません。 |
| <a id="p_expand_source"></a>`P_EXPAND_SOURCE` | `proposal_source` にはインラインオブジェクトか入力の JSON Pointer が必要です。 |
| <a id="p_expand_bound"></a>`P_EXPAND_BOUND` | `expand_task_graph` の上限が範囲外です。 |
| <a id="p_expand_failure"></a>`P_EXPAND_FAILURE` | `failure_policy` は `fail_fast` か `collect` でなければなりません。 |
| <a id="p_budget_invalid"></a>`P_BUDGET_INVALID` | 予算レコードが欠けている、重複している、最小と最大が逆転している、または構造上の下限を下回っています。9 種類のリソースをそれぞれ 1 回宣言してください。 |
| <a id="p_budget_v1"></a>`P_BUDGET_V1` | Program スキーマ 1 では `max_program_operations` がちょうど 1 に固定されます。 |
| <a id="p_budget_static"></a>`P_BUDGET_STATIC` | 宣言した最大値ではプランが常に実行する操作をまかなえません。値を上げてください。 |
| <a id="p_migration_program_json"></a>`P_MIGRATION_PROGRAM_JSON` | Program JSON の操作ツリーはもう受け付けません。JavaScript か信頼済みの C++ ビルダーで記述してください。 |
| <a id="p_compiler_internal"></a>`P_COMPILER_INTERNAL` | コンパイラーが内部エラーで安全側に失敗しました。ソースを添えて報告してください。 |
| <a id="p_diagnostics_truncated"></a>`P_DIAGNOSTICS_TRUNCATED` | 診断が 20 件を超えたため、残りは `witness.omitted` に件数だけが入っています。表示された診断を直してから再度コンパイルしてください。 |

<a id="javascript-frontend"></a>
## JavaScript フロントエンド

| コード | 意味と修正方法 |
|---|---|
| <a id="p_js_evaluation"></a>`P_JS_EVALUATION` | ソースが例外を投げたか、解析できませんでした。span が失敗した行を指します。 |
| <a id="p_js_timeout"></a>`P_JS_TIMEOUT` | 評価がコンパイル時間の上限か予算の `wall_time_ms` を超えました。 |
| <a id="p_js_resource_limit"></a>`P_JS_RESOURCE_LIMIT` | 評価がメモリまたはスタックの上限を超えました。 |
| <a id="p_js_cancelled"></a>`P_JS_CANCELLED` | 評価がキャンセルされました。 |
| <a id="p_js_runtime"></a>`P_JS_RUNTIME` | QuickJS ランタイムかビルダー呼び出しが内部で失敗しました。 |
| <a id="p_js_runtime_identity"></a>`P_JS_RUNTIME_IDENTITY` | ソースがコンパイラーとは異なる QuickJS ビルドまたはプロファイル向けに作られています。 |
| <a id="p_js_unavailable"></a>`P_JS_UNAVAILABLE` | このビルドには QuickJS フロントエンドがありません。 |
| <a id="p_js_ambient_denied"></a>`P_JS_AMBIENT_DENIED` | ソースがプロファイルで禁止されている環境権限（時計、乱数、eval など）を使いました。 |
| <a id="p_js_define_missing"></a>`P_JS_DEFINE_MISSING` | モジュールが `define()` を export していません。 |
| <a id="p_js_define_async"></a>`P_JS_DEFINE_ASYNC` | `define()` は同期関数でなければなりません。 |
| <a id="p_js_define_value"></a>`P_JS_DEFINE_VALUE` | `define()` は開いたグラフビルダーをちょうど 1 つ返さなければなりません。 |
| <a id="p_js_graph_argument"></a>`P_JS_GRAPH_ARGUMENT` | ビルダーメソッドが誤った形の引数、予約済みまたは重複した名前、宣言されていないノードを受け取りました。メッセージにメソッド名が含まれます。 |
| <a id="p_js_graph_value"></a>`P_JS_GRAPH_VALUE` | ビルダーの引数が正規の JSON データではありません（関数、クラスのインスタンス、有限でない数値）。 |
| <a id="p_js_graph_limit"></a>`P_JS_GRAPH_LIMIT` | 生成されたグラフがバイト数の上限を超えました。 |
| <a id="p_js_graph_owner"></a>`P_JS_GRAPH_OWNER` | この定義のビルダーではないオブジェクトでビルダーメソッドを呼び出しました。 |
| <a id="p_js_graph_sealed"></a>`P_JS_GRAPH_SEALED` | `define()` が戻った後にビルダーメソッドを呼び出しました。 |
| <a id="p_js_graph_entry"></a>`P_JS_GRAPH_ENTRY` | グラフにノードはあるものの `.entry(node)` がないため、何も実行されません。 |
| <a id="p_js_control_character"></a>`P_JS_CONTROL_CHARACTER` | 設定文字列に制御文字があります。正規表現を普通の文字列で書いた場合によく起きます。`/.../.source` か ``String.raw`...` `` を使ってください。 |
| <a id="p_js_control_main"></a>`P_JS_CONTROL_MAIN` | export された `main` が関数ではありません。 |
| <a id="p_js_control_async"></a>`P_JS_CONTROL_ASYNC` | `main` は同期ジェネレーターでなければなりません。 |
| <a id="p_js_control_value"></a>`P_JS_CONTROL_VALUE` | `main` が封印済みのコマンドでも正規の JSON でもない値を yield したか返しました。 |
| <a id="p_js_control_command"></a>`P_JS_CONTROL_COMMAND` | 不正な引数でコマンドを作りました。 |
| <a id="p_js_control_finished"></a>`P_JS_CONTROL_FINISHED` | 終了した制御ジェネレーターを再開しました。 |
| <a id="p_js_import_dynamic"></a>`P_JS_IMPORT_DYNAMIC` | 動的な `import()` と `require()` は使えません。 |
| <a id="p_js_import_invalid"></a>`P_JS_IMPORT_INVALID` | import 指定子の形式が不正です。 |
| <a id="p_js_import_unavailable"></a>`P_JS_IMPORT_UNAVAILABLE` | import が封印済みのモジュールクロージャーに含まれていません。 |
| <a id="p_import_conflict"></a>`P_IMPORT_CONFLICT` | 1 つの import ソースが 2 つの異なるコンテンツ識別子で現れています。 |
| <a id="p_import_dynamic"></a>`P_IMPORT_DYNAMIC` | ソースまたは封印済みのモジュールがモジュールを動的に読み込んでいます。 |
| <a id="p_import_unresolved"></a>`P_IMPORT_UNRESOLVED` | import が検証済みの正確なモジュールレシートに結び付いていません。モジュール解決の結果とともにコンパイルしてください。 |

<a id="core-parser"></a>
## Core パーサー

Core 定義のパーサーが出す診断です。witness の `core_code` に Core のコードが入っています。

| コード | 意味と修正方法 |
|---|---|
| <a id="p_core_gc_schema_version"></a>`P_CORE_GC_SCHEMA_VERSION` | Core の `schema_version` がサポートされる非負の整数ではありません。 |
| <a id="p_core_gc_unknown_field"></a>`P_CORE_GC_UNKNOWN_FIELD` | 厳格な Core オブジェクトにエンジンが読まないキーがあります。削除するか候補名を使うか、メモなら `_` または `x-` の注釈キーへ移してください。 |
| <a id="p_core_gc_field_type"></a>`P_CORE_GC_FIELD_TYPE` | Core フィールドの JSON 型が誤っています。 |
| <a id="p_core_gc_field_value"></a>`P_CORE_GC_FIELD_VALUE` | Core フィールドの値が不正です。空の名前、範囲外の数値、未知のオプションなどです。 |
| <a id="p_core_gc_field_required"></a>`P_CORE_GC_FIELD_REQUIRED` | `type`、`from`、`to` などの必須 Core フィールドがありません。 |
| <a id="p_core_gc_edge"></a>`P_CORE_GC_EDGE` | edge や conditional edge がオブジェクトではないか、routes がオブジェクトではありません。 |
| <a id="p_core_gc_barrier"></a>`P_CORE_GC_BARRIER` | barrier の `wait_for` がないか、空か、形式が不正です。 |
| <a id="p_core_gc_node_config"></a>`P_CORE_GC_NODE_CONFIG` | ノードの設定が型スキーマの `required`、`type`、`enum` の規則に合いません。 |
| <a id="p_core_gc_reserved_node"></a>`P_CORE_GC_RESERVED_NODE` | `__start__` と `__end__` は予約されているため、ノード名に使えません。 |
| <a id="p_core_gc_no_entry"></a>`P_CORE_GC_NO_ENTRY` | `__start__` から出る edge がないため、何も実行されません。`.entry(node)` を追加してください。 |
| <a id="p_core_gc_registry_miss"></a>`P_CORE_GC_REGISTRY_MISS` | ノード型、reducer、condition がレジストリのスナップショットにありません。登録するか候補名を使ってください。 |
| <a id="p_core_gc_metadata_limit"></a>`P_CORE_GC_METADATA_LIMIT` | グラフのメタデータが上限を超えています。上限は正規 JSON で 8 KiB、入れ子の深さ 4、オブジェクトメンバー合計 64、キー 128 バイトです。witness に `kind`、`limit`、`actual` があります。 |
| <a id="p_core_gc_metadata_text"></a>`P_CORE_GC_METADATA_TEXT` | メタデータのキーや文字列に不可視文字（タブ、改行、復帰以外の C0 制御文字、bidi 制御文字、ゼロ幅文字、タグ文字）や不正な UTF-8 が含まれています。witness にコードポイントが挙げられます。 |
| <a id="p_core_roundtrip"></a>`P_CORE_ROUNDTRIP` | 解析したグラフが定義をそのまま再出力できません。何かが欠落したか、つなぎ替えられています。コンパイラーの不具合なので報告してください。 |

<a id="core-validator"></a>
## Core バリデーター

グラフの意味を分析した結果です。警告は返されるだけで、バンドルは最初に封印されたときの規則による警告を封印します。

| コード | 意味と修正方法 |
|---|---|
| <a id="p_core_e3"></a>`P_CORE_E3` | edge、route、interrupt、barrier が未知のノードを指しているか、barrier が自分自身を待っています。候補名を使ってください。 |
| <a id="p_core_e4"></a>`P_CORE_E4` | ノードの effect 契約が宣言されていないチャネルに書き込む（エラー）か読み取ります（警告）。チャネルを宣言してください。 |
| <a id="p_core_e5"></a>`P_CORE_E5` | fan-out の兄弟ノードが同じ overwrite チャネルに書き込み、最後の書き込みが非決定的に勝ちます。append reducer を使ってください。 |
| <a id="p_core_e6"></a>`P_CORE_E6` | チャネルが読み書きされないか、書かれるだけで読まれないか、読まれるだけで初期値がありません。 |
| <a id="p_core_e7"></a>`P_CORE_E7` | edge と route で `__start__` から到達できないノードがあります。 |
| <a id="p_core_e8"></a>`P_CORE_E8` | barrier が自分へルーティングしないノード、またはどの選択肢も自分へルーティングしない `any_of` スロットを待っているため、join は決して発火しません。 |
| <a id="p_core_e9"></a>`P_CORE_E9` | ノードに通常の in-edge が複数ありますが barrier がありません。到着する super-step ごとに 1 回実行されます。AND join なら barrier を追加し、XOR マージなら無視してください。 |
| <a id="p_core_e10"></a>`P_CORE_E10` | conditional edge に route がないか、condition が返さないラベルをルーティングしているか、ラベルをルーティングしないまま残しています。 |
| <a id="p_core_e11"></a>`P_CORE_E11` | ノードが `__end__` への経路のないサイクルに閉じ込められています。 |
| <a id="p_core_e12"></a>`P_CORE_E12` | ローカルレジストリに reducer、ノード型、condition がありません。 |
| <a id="p_core_e13"></a>`P_CORE_E13` | conditional edge でルーティングするノードから静的な edge が出ています。この edge は決して発火しません。削除するか route として追加してください。 |
| <a id="p_core_e14"></a>`P_CORE_E14` | ノードに 2 つ目の conditional edge があります。ルーティングするのは最初の 1 つだけです。まとめてください。 |
| <a id="p_core_e15"></a>`P_CORE_E15` | barrier のメンバーが自分の join 以外へルーティングし得ます。そうなると実行は barrier-stalled で終わります。同じ `any_of` スロットの別の選択肢へのルートは迂回ではありません。 |
| <a id="p_core_e16"></a>`P_CORE_E16` | グラフにサイクルがありますが、このコンパイルは非巡回グラフを要求しています。 |
| <a id="p_core_e17"></a>`P_CORE_E17` | 1 つの `any_of` スロットの 2 つの選択肢が両方とも実行され得ます。fan-out が join を通らずに両方へ届くか、ある選択肢が join と別の選択肢の両方へルーティングします。2 番目の到着は終わらない新しいラウンドを開き、実行は barrier-stalled で終わります。例えば conditional edge を使って選択肢どうしを排他的にルーティングしてください。witness に fan-out ノードと 2 つの経路があります。 |

<a id="registry"></a>
## レジストリ

| コード | 意味と修正方法 |
|---|---|
| <a id="p_registry_missing"></a>`P_REGISTRY_MISSING` | 参照した実行単位がレジストリのスナップショットにありません。登録するか候補名を使ってください。 |
| <a id="p_registry_dependency_missing"></a>`P_REGISTRY_DEPENDENCY_MISSING` | 実行単位がレジストリのスナップショットにない依存関係を必要としています。 |
| <a id="p_registry_identity_mismatch"></a>`P_REGISTRY_IDENTITY_MISMATCH` | 2 つの参照が 1 つの実行単位の異なる正確な識別子に解決されます。 |
| <a id="p_registry_requirement_kind"></a>`P_REGISTRY_REQUIREMENT_KIND` | ノードの要件リゾルバーがサポートされていない実行単位の種類を返しました。 |
| <a id="p_registry_requirement_resolver"></a>`P_REGISTRY_REQUIREMENT_RESOLVER` | ノードの要件リゾルバーが例外を投げました。 |

<a id="runtime-and-admission"></a>
## ランタイムとアドミッション

アドミッションと実行の失敗は Python に `ng.ProgramAdmissionError` と `ng.ProgramDiagnosticError` として届き、実行結果にはコード付きの `failure` が入ります。コードに専用の項目がない場合、`help.doc` はこのセクションを指します。

| コード | 意味と修正方法 |
|---|---|
| <a id="p_core_barrier_stalled"></a>`P_CORE_BARRIER_STALLED` | もう届かないシグナルを join が待ったまま Core の実行が止まりました。witness は止まった join ごとに `received` と `missing` を挙げます。 |
