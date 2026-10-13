<!-- neograph-i18n: source=docs/diagnostics.md locale=ja source_sha256=05a405c99f388774993482b2cb175a7c22343d727442659756368bae50402791 -->
# Programの診断

**Languages:** [English](diagnostics.md) | [한국어](diagnostics.ko.md) | [日本語](diagnostics.ja.md) | [简体中文](diagnostics.zh-CN.md)

Programをコンパイルすると、診断が返されます。失敗した場合は `ProgramCompileError::diagnostics()`、成功した場合は `ProgramBundle::compile_diagnostics()`、Pythonの例外では `.diagnostics` から取得できます。このページでは、各コードの意味と直し方を説明します。返されるすべての診断は、`help.doc` を通じて、このページの該当項目にリンクします。

- [helpオブジェクト](#the-help-object)
- [Programドキュメント](#program-document)
- [JavaScriptフロントエンド](#javascript-frontend)
- [Coreパーサー](#core-parser)
- [Coreバリデーター](#core-validator)
- [健全性チェック](#soundness-check)
- [レジストリ](#registry)
- [ランタイムとadmission](#runtime-and-admission)

<a id="the-help-object"></a>
## helpオブジェクト

返される診断には `help` が付いています。ツールやモデルは、メッセージを解析しなくても、これをもとに動作できます。

| フィールド | 内容 |
|---|---|
| `doc` | このページにある、そのコードの項目へのリンクです。項目がない場合は、そのコードが属するファミリーの節へのリンクです。 |
| `related` | 同じ根本原因による、ほかの箇所です。それぞれに1行の `label` が付きます。 |
| `suggestions` | 修正案です。それぞれに `message`、`edits`、`applicability` があります。 |
| `candidates` | 未知の名前から編集距離2以内にある、宣言済みの名前です。近い順に並びます。 |

`edits` は、コンパイラーがパースしたProgramドキュメントに対する、RFC 6902のJSON Patch操作（`add`、`replace`、`remove`、`move`）です。JavaScriptのソースでは、`define()` が生成したドキュメントが対象になり、診断のspanはビルダーの呼び出し位置を指します。`applicability` は、`machine`（そのまま適用できる）、`maybe`（先に確認する。デフォルト）、`placeholder`（`<node>` のような値を埋める）のいずれかです。

バンドルにシールされる診断には、`help` は付きません。そのため、バンドルIDがこれに左右されることはありません。コンパイルは、根本原因ごとに診断を1件返します。複数の診断が同じ未知の名前を報告する場合は、最初の1件が残り、残りはその `related` の項目になります。返される診断は最大20件で、エラーが先に並びます。残りの件数は、`P_DIAGNOSTICS_TRUNCATED` の注記が示します。

メッセージには、書いた内容がそのまま使われます。JavaScriptのソースでは `graph.barrier("D", [...])` のようなビルダーの呼び出し、JSONやC++ビルダーのドキュメントではCoreのキーです。

<a id="program-document"></a>
## Programドキュメント

| コード | 意味と対処 |
|---|---|
| <a id="p_schema_version"></a>`P_SCHEMA_VERSION` | Program、契約、または埋め込まれたCore定義のスキーマバージョンが、サポート外か、指定されていません。witnessにコンパイラーが挙げているバージョンを使ってください。 |
| <a id="p_schema_unknown_field"></a>`P_SCHEMA_UNKNOWN_FIELD` | closedなProgramオブジェクトに、定義されていないフィールドがあります。削除するか、候補の名前を使ってください。 |
| <a id="p_schema_required"></a>`P_SCHEMA_REQUIRED` | Programの必須フィールドがありません。追加してください。 |
| <a id="p_schema_type"></a>`P_SCHEMA_TYPE` | ProgramのフィールドのJSON型が違っているか、ソースマップでポインターが重複しています。witnessにある型を使ってください。 |
| <a id="p_schema_operation"></a>`P_SCHEMA_OPERATION` | この操作には、より新しいProgramスキーマバージョンが必要です。`program_schema_version` を上げるか、別の操作を使ってください。 |
| <a id="p_contract_schema"></a>`P_CONTRACT_SCHEMA` | 入力または出力の契約が、無効な、またはサポートされていないJSON Schemaのキーワードを使っています。 |
| <a id="p_root_name"></a>`P_ROOT_NAME` | ルート名がないか、空か、Core定義の `name` と異なっています。両者を一致させてください。 |
| <a id="p_root_operation"></a>`P_ROOT_OPERATION` | `call_core` のルートに、Coreノードがありません。ノードを1つ以上宣言してください。 |
| <a id="p_plan_operation"></a>`P_PLAN_OPERATION` | この操作はサポートされていません。 |
| <a id="p_plan_core"></a>`P_PLAN_CORE` | プランが、シールされたルート以外のCoreを呼び出しているか、Coreを1つも呼び出さず、子もディスパッチしていません。 |
| <a id="p_plan_bound"></a>`P_PLAN_BOUND` | ループ、リトライ、タイムアウトの上限が、範囲外です。 |
| <a id="p_plan_condition"></a>`P_PLAN_CONDITION` | プランの条件が、述語をちょうど1つ持っていないか、そのパスがJSON Pointerではありません。 |
| <a id="p_plan_race_arity"></a>`P_PLAN_RACE_ARITY` | raceの分岐が2つではありません。 |
| <a id="p_plan_quorum"></a>`P_PLAN_QUORUM` | `min_success` が0か、分岐数より大きくなっています。 |
| <a id="p_plan_spawn_shape"></a>`P_PLAN_SPAWN_SHAPE` | spawnがインラインの本体を持っているか、awaitの直接の本体になっていません。 |
| <a id="p_plan_cancel_scope"></a>`P_PLAN_CANCEL_SCOPE` | cancelがサポートするスコープは、`run` だけです。 |
| <a id="p_plan_reference"></a>`P_PLAN_REFERENCE` | シールされた操作が、存在しない操作を参照しています。これはコンパイラーの不具合です。報告してください。 |
| <a id="p_plan_source_pointer"></a>`P_PLAN_SOURCE_POINTER` | シールされた操作のソースポインターが無効です。これはコンパイラーの不具合です。報告してください。 |
| <a id="p_plan_typed"></a>`P_PLAN_TYPED` | プランを、型付きの操作としてシールできませんでした。詳細はwitnessにあります。 |
| <a id="p_parallel_map_source"></a>`P_PARALLEL_MAP_SOURCE` | `item_source` には、上限のあるリテラルの配列か、入力アーティファクトのフィールドのどちらかが必要です。 |
| <a id="p_parallel_map_binding"></a>`P_PARALLEL_MAP_BINDING` | `parallel_map` のバインディングまたはエンドポイントがないか、JSON Pointerではありません。 |
| <a id="p_parallel_map_bound"></a>`P_PARALLEL_MAP_BOUND` | `parallel_map` の上限が範囲外か、`max_in_flight` が `max_items` を超えています。 |
| <a id="p_parallel_map_failure"></a>`P_PARALLEL_MAP_FAILURE` | `failure_policy` は、`fail_fast` か `collect` にしてください。 |
| <a id="p_expand_source"></a>`P_EXPAND_SOURCE` | `proposal_source` には、インラインのオブジェクトか、入力のJSON Pointerが必要です。 |
| <a id="p_expand_bound"></a>`P_EXPAND_BOUND` | `expand_task_graph` の上限が範囲外です。 |
| <a id="p_expand_failure"></a>`P_EXPAND_FAILURE` | `failure_policy` は、`fail_fast` か `collect` にしてください。 |
| <a id="p_budget_invalid"></a>`P_BUDGET_INVALID` | 予算のレコードが、不完全、重複、大小の逆転、または構造上の下限未満のいずれかです。9つのリソースをすべて、1回ずつ宣言してください。 |
| <a id="p_budget_v1"></a>`P_BUDGET_V1` | Programスキーマ1では、`max_program_operations` は必ず1に固定されます。 |
| <a id="p_budget_static"></a>`P_BUDGET_STATIC` | 宣言した最大値が、プランが必ず実行する操作をまかなえていません。最大値を上げてください。 |
| <a id="p_migration_program_json"></a>`P_MIGRATION_PROGRAM_JSON` | ProgramのJSONによる操作ツリーは、受け付けなくなりました。JavaScriptか、信頼できるC++ビルダーで記述してください。 |
| <a id="p_compiler_internal"></a>`P_COMPILER_INTERNAL` | コンパイラーが内部エラーでフェイルクローズしました。ソースを添えて報告してください。 |
| <a id="p_diagnostics_truncated"></a>`P_DIAGNOSTICS_TRUNCATED` | 20件を超える診断が見つかりました。残りの件数は `witness.omitted` にあります。表示された診断を直してから、もう一度コンパイルしてください。 |

<a id="javascript-frontend"></a>
## JavaScriptフロントエンド

| コード | 意味と対処 |
|---|---|
| <a id="p_js_evaluation"></a>`P_JS_EVALUATION` | ソースが例外を送出したか、パースできませんでした。spanは失敗した行を指します。 |
| <a id="p_js_timeout"></a>`P_JS_TIMEOUT` | 評価が、コンパイルの時間制限か、予算の `wall_time_ms` を超えました。 |
| <a id="p_js_resource_limit"></a>`P_JS_RESOURCE_LIMIT` | 評価が、メモリまたはスタックの上限を超えました。 |
| <a id="p_js_cancelled"></a>`P_JS_CANCELLED` | 評価がキャンセルされました。 |
| <a id="p_js_runtime"></a>`P_JS_RUNTIME` | QuickJSランタイムまたはビルダーの呼び出しが、内部で失敗しました。 |
| <a id="p_js_runtime_identity"></a>`P_JS_RUNTIME_IDENTITY` | ソースが、コンパイラーとは異なるQuickJSのビルドまたはプロファイル向けに作られています。 |
| <a id="p_js_unavailable"></a>`P_JS_UNAVAILABLE` | このビルドには、QuickJSフロントエンドがありません。 |
| <a id="p_js_ambient_denied"></a>`P_JS_AMBIENT_DENIED` | ソースが、プロファイルで禁止されている環境依存の権限（時計、乱数、evalなど）を使いました。 |
| <a id="p_js_define_missing"></a>`P_JS_DEFINE_MISSING` | モジュールが `define()` をエクスポートしていません。 |
| <a id="p_js_define_async"></a>`P_JS_DEFINE_ASYNC` | `define()` は同期関数にしてください。 |
| <a id="p_js_define_value"></a>`P_JS_DEFINE_VALUE` | `define()` は、開いたグラフビルダーをちょうど1つ返す必要があります。 |
| <a id="p_js_graph_argument"></a>`P_JS_GRAPH_ARGUMENT` | ビルダーのメソッドが、形の違う引数、予約済みまたは重複した名前、宣言されていないノードを受け取りました。メッセージにメソッド名が書かれています。 |
| <a id="p_js_graph_value"></a>`P_JS_GRAPH_VALUE` | ビルダーの引数が、正規のJSONデータではありません（関数、クラスのインスタンス、有限でない数値など）。 |
| <a id="p_js_graph_limit"></a>`P_JS_GRAPH_LIMIT` | 生成されたグラフが、バイト数の上限を超えています。 |
| <a id="p_js_graph_owner"></a>`P_JS_GRAPH_OWNER` | ビルダーのメソッドが、この定義のビルダーではないオブジェクトに対して呼び出されました。 |
| <a id="p_js_graph_sealed"></a>`P_JS_GRAPH_SEALED` | `define()` が値を返した後で、ビルダーのメソッドが呼び出されました。 |
| <a id="p_js_graph_entry"></a>`P_JS_GRAPH_ENTRY` | グラフにノードはありますが `.entry(node)` がなく、何も実行されません。 |
| <a id="p_js_control_character"></a>`P_JS_CONTROL_CHARACTER` | 設定の文字列に制御文字が含まれています。多くの場合、正規表現を普通の文字列として書いたのが原因です。`/.../.source` か ``String.raw`...` `` を使ってください。 |
| <a id="p_js_control_main"></a>`P_JS_CONTROL_MAIN` | エクスポートされた `main` が関数ではありません。 |
| <a id="p_js_control_async"></a>`P_JS_CONTROL_ASYNC` | `main` は同期ジェネレーターにしてください。 |
| <a id="p_js_control_value"></a>`P_JS_CONTROL_VALUE` | `main` が、シール済みのコマンドでも正規のJSONでもない値をyieldまたはreturnしました。 |
| <a id="p_js_control_command"></a>`P_JS_CONTROL_COMMAND` | コマンドが、無効な引数で作られました。 |
| <a id="p_js_control_finished"></a>`P_JS_CONTROL_FINISHED` | 制御ジェネレーターが、終了した後で再開されました。 |
| <a id="p_js_import_dynamic"></a>`P_JS_IMPORT_DYNAMIC` | 動的な `import()` と `require()` は使えません。 |
| <a id="p_js_import_invalid"></a>`P_JS_IMPORT_INVALID` | importの指定子の形式が不正です。 |
| <a id="p_js_import_unavailable"></a>`P_JS_IMPORT_UNAVAILABLE` | importされたものが、シールされたモジュールクロージャーに含まれていません。 |
| <a id="p_import_conflict"></a>`P_IMPORT_CONFLICT` | 同じimportのソースが、2つの異なるコンテンツIDで現れています。 |
| <a id="p_import_dynamic"></a>`P_IMPORT_DYNAMIC` | ソースまたはシール済みのモジュールが、モジュールを動的にロードしています。 |
| <a id="p_import_unresolved"></a>`P_IMPORT_UNRESOLVED` | importが、検証済みの特定のモジュールレシートに結び付けられていません。モジュール解決を指定してコンパイルしてください。 |

<a id="core-parser"></a>
## Coreパーサー

これらはCore定義のパーサーが出す診断で、witnessにはCoreのコードが `core_code` として入っています。

| コード | 意味と対処 |
|---|---|
| <a id="p_core_gc_schema_version"></a>`P_CORE_GC_SCHEMA_VERSION` | Coreの `schema_version` が、サポートされている0以上の整数ではありません。 |
| <a id="p_core_gc_unknown_field"></a>`P_CORE_GC_UNKNOWN_FIELD` | strictなCoreオブジェクトに、エンジンが読まないキーがあります。削除するか、候補の名前を使うか、メモなら `_` または `x-` で始まるアノテーションのキーに移してください。 |
| <a id="p_core_gc_field_type"></a>`P_CORE_GC_FIELD_TYPE` | CoreのフィールドのJSON型が違っています。 |
| <a id="p_core_gc_field_value"></a>`P_CORE_GC_FIELD_VALUE` | Coreのフィールドの値が無効です。空の名前、範囲外の数値、未知のオプションなどが該当します。 |
| <a id="p_core_gc_field_required"></a>`P_CORE_GC_FIELD_REQUIRED` | `type`、`from`、`to` など、必須のCoreフィールドがありません。 |
| <a id="p_core_gc_edge"></a>`P_CORE_GC_EDGE` | エッジまたは条件付きエッジがオブジェクトではないか、そのルートがオブジェクトではありません。 |
| <a id="p_core_gc_barrier"></a>`P_CORE_GC_BARRIER` | バリアの `wait_for` がないか、空か、形式が不正です。 |
| <a id="p_core_gc_node_config"></a>`P_CORE_GC_NODE_CONFIG` | ノードの設定が、そのタイプのスキーマの `required`、`type`、`enum` の規則に合っていません。 |
| <a id="p_core_gc_reserved_node"></a>`P_CORE_GC_RESERVED_NODE` | `__start__` と `__end__` は予約されており、ノードの名前には使えません。 |
| <a id="p_core_gc_no_entry"></a>`P_CORE_GC_NO_ENTRY` | `__start__` から出るエッジがなく、何も実行されません。`.entry(node)` を追加してください。 |
| <a id="p_core_gc_registry_miss"></a>`P_CORE_GC_REGISTRY_MISS` | ノードタイプ、リデューサー、条件が、レジストリのスナップショットにありません。登録するか、候補の名前を使ってください。 |
| <a id="p_core_gc_metadata_limit"></a>`P_CORE_GC_METADATA_LIMIT` | グラフのメタデータが上限を超えています。上限は、正規化したJSONで8 KiB、入れ子の深さ4、オブジェクトのメンバー合計64個、キー128バイトです。witnessには `kind`、`limit`、`actual` が入ります。 |
| <a id="p_core_gc_metadata_text"></a>`P_CORE_GC_METADATA_TEXT` | メタデータのキーまたは文字列に、不可視文字（タブ・改行・復帰以外のC0制御文字、双方向制御文字、ゼロ幅文字、タグ文字）か、不正なUTF-8が含まれています。witnessにコードポイントが一覧されます。 |
| <a id="p_core_gc_failure_edge"></a>`P_CORE_GC_FAILURE_EDGE` | failure edgeが、行き先に関する規則に違反しています。`witness.reason` は次のいずれかです。`sentinel_source`、`duplicate_from`（ノードごとにfailure edgeは1つ）、`sentinel_target`（代わりにfailure exitで実行を終わらせる）、`self_target`、`join_target`（失敗による到着は、joinのスロットを埋めない）、`implicit_end_target`（行き先にエッジかルートを持たせるか、failure exitにする）、`error_channel_reducer`（エラーチャネルは `append` リデューサーを使う必要がある）。 |
| <a id="p_core_roundtrip"></a>`P_CORE_ROUNDTRIP` | パースしたグラフから、元の定義を再出力できません。何かが落ちたか、つなぎ替えられています。これはコンパイラーの不具合です。報告してください。 |

<a id="core-validator"></a>
## Coreバリデーター

これらは、グラフの意味を解析する診断です。警告は返されるだけです。バンドルには、最初にシールされたときのルールによる警告がシールされます。

| コード | 意味と対処 |
|---|---|
| <a id="p_core_e3"></a>`P_CORE_E3` | エッジ、ルート、割り込み、バリアが未知のノードを指しているか、バリアが自分自身を待っています。候補の名前を使ってください。 |
| <a id="p_core_e4"></a>`P_CORE_E4` | ノードのeffect契約が、宣言されていないチャネルに書き込む（エラー）か、読み取る（警告）ようになっています。チャネルを宣言してください。 |
| <a id="p_core_e5"></a>`P_CORE_E5` | fan-outの並列ノードが、同じoverwriteチャネルに書き込んでいます。最後に書いたものが残りますが、どれが最後になるかは決まっていません。appendリデューサーを使ってください。 |
| <a id="p_core_e6"></a>`P_CORE_E6` | チャネルが、どこからも読み書きされていない、書き込まれるだけで読まれていない、または読まれるだけで書き込まれておらず初期値もない、のいずれかです。 |
| <a id="p_core_e7"></a>`P_CORE_E7` | `__start__` からエッジとルートをたどっても、到達できないノードがあります。`Send` または `Command.goto` だけが開始するノードも対象です。グラフ定義に動的な対象を宣言する場所がないためで、この場合は警告が想定どおりです。警告を消すためにエッジを足さないでください。ノードがもう一度実行されます。 |
| <a id="p_core_e8"></a>`P_CORE_E8` | バリアが待っているノード、または `any_of` のスロットで、どの選択肢もそのバリアへルーティングされません。そのため、joinは発火しません。 |
| <a id="p_core_e9"></a>`P_CORE_E9` | ノードに、通常の入りエッジが複数あるのにバリアがありません。到着したスーパーステップごとに、1回ずつ実行されます。ANDのjoinにしたい場合はバリアを追加してください。XOR（どれか1つで進む）のマージなら、無視してかまいません。 |
| <a id="p_core_e10"></a>`P_CORE_E10` | 条件付きエッジに、ルートがない、条件が返さないラベルへのルートがある、またはルーティングされていないラベルがあります。 |
| <a id="p_core_e11"></a>`P_CORE_E11` | `__end__` に至る経路がない循環に、ノードが閉じ込められています。 |
| <a id="p_core_e12"></a>`P_CORE_E12` | ローカルのレジストリに、リデューサー、ノードタイプ、条件のどれかが登録されていません。 |
| <a id="p_core_e13"></a>`P_CORE_E13` | 条件付きエッジでルーティングされるノードから、静的エッジが出ています。このエッジは発火しません。削除するか、ルートとして追加してください。 |
| <a id="p_core_e14"></a>`P_CORE_E14` | ノードに2本目の条件付きエッジがあります。ルーティングに使われるのは最初の1本だけです。1本にまとめてください。 |
| <a id="p_core_e15"></a>`P_CORE_E15` | バリアのメンバーが、自分のjoin以外の場所にルーティングされることがあります。そうなると、実行はバリア待ちで停止して終わります。同じ `any_of` スロットの別の選択肢へのルートは、迂回には当たりません。`witness.paths` に、迂回ルートが1本ずつ挙げられます。迂回したルートがjoinに戻ってくる場合は、`suggested_wait_for` と、返される提案によって、joinの直前のノードがメンバーの代わりになれます。 |
| <a id="p_core_e16"></a>`P_CORE_E16` | グラフに循環がありますが、このコンパイルは非循環のグラフを要求しています。 |
| <a id="p_core_e17"></a>`P_CORE_E17` | 1つの `any_of` スロットの2つの選択肢が、両方とも実行されうる状態です。fan-outがjoinを通らずに両方に到達する場合と、片方の選択肢がjoinへも、もう片方の選択肢へもルーティングする場合があります。2回目の到着は、完了することのないラウンドを開始し、実行はバリア待ちで停止して終わります。条件付きエッジなどを使って、選択肢どうしのルートが排他的になるようにしてください。witnessには、fan-outのノードと、2本のパスが入ります。 |
| <a id="p_core_e18"></a>`P_CORE_E18` | ノード設定に、そのノードタイプが宣言していない `retry_policy` キーがあり、何も読まれません。ノードのポリシーは `graph.retryPolicy(node, {...})` で設定してください（Coreでは `node_retry_policies`）。 |
| <a id="p_core_routes"></a>`P_CORE_ROUTES` | `ProgramCompilerConfig::echo_routes`（Pythonでは `echo_routes=True`）を設定したときに返される注記です。`witness.routes` は、各ノードと `__start__` を、スケジューラーがルーティングしうる先に対応付けます。シールされることはありません。 |

<a id="soundness-check"></a>
## 健全性チェック

これらは、オプトインの健全性チェック `ProgramCompilerConfig::soundness_check`（Pythonでは `soundness_check=True`）が出す診断です。詳しくは[Programのグラフ機能](program-graphs.md#soundness-check)を参照してください。返されるだけで、シールされることはありません。`witness.trace` には、検出内容を示す最短の実行のスーパーステップが並びます。`ready` は実行されたノードで、`routes` は、選択肢のあるノードがそれぞれ取ったルートです（failure edgeなら `on_failure`、`max_visits` によって強制されたなら `visit_limit`）。`witness.choices` は、それらのルートの選択を、各選択を宣言したエッジのCoreポインターとともに一覧にしたものです。`help.related` は、それらを指します。

| コード | 意味と対処 |
|---|---|
| <a id="p_core_s1"></a>`P_CORE_S1` | Option to complete：トレースが到達する状態（`witness.state`）から先では、どの実行も完了できません。後続のすべての実行が、`stalls_at` にあるjoinでバリア待ちで停止して終わるか、訪問回数の上限で失敗するか、ループします。どれになるかは `ends` に書かれています。どのルートからも必ず届くものをjoinに待たせてください（たとえば `any_of` のスロット）。または、その分岐をjoinへ戻すようにルーティングしてください。 |
| <a id="p_core_s2"></a>`P_CORE_S2` | Proper completion：joinがまだシグナルを保持したまま、実行が終わっています。`kind` は、`stalled`（実行が `P_CORE_BARRIER_STALLED` で失敗する）か、`completed`（明示的な終了がシグナルを捨て、実行が完了と報告される）です。witnessには `received` と `missing` が入ります。スロットが1つ足りず、スロット外のノードが1つシグナルを送っているために停止している場合は、`suggested_wait_for` によって、そのノードがスロットを埋められるようになります。返される提案が、それを適用します。 |
| <a id="p_core_s3"></a>`P_CORE_S3` | どの実行でも、joinが発火しません。`kind` は、`dead`（シグナルを送るものがない）か、`unfireable`（シグナルは届くが、すべてのスロットが同時に埋まることがない）です。`slots_never_filled` には、どの実行もシグナルを送らないスロットが一覧されます。すべての実行を探索できた場合にだけ報告されます。 |
| <a id="p_core_s4"></a>`P_CORE_S4` | 実行が、抜け出せないループに入ることがあり、その場合はステップ数の上限でしか止まりません。`nodes` はループを構成するノード、`trace` はそこに至る経路、`cycle` はループを1周する経路です。ループの中の条件付きノードに、ループから出るルートを追加してください。返される提案はプレースホルダーです。 |
| <a id="p_core_s5"></a>`P_CORE_S5` | 同じoverwriteチャネルに書き込む2つのノードが、同じスーパーステップで実行されることがあります（`P_CORE_E5` が報告するfan-outの並列ノードは除きます）。最後に書いたものが残りますが、どれが最後になるかは決まっていません。appendリデューサーを使うか（返される提案）、書き込むノードを直列にしてください。すべてのノードタイプにeffect契約がある場合にだけ報告されます。 |
| <a id="p_core_soundness"></a>`P_CORE_SOUNDNESS` | 要約です。`status` は、すべての実行を探索した場合は `checked`（注記）、そうでない場合は `partial` または `not_checked`（警告）です。witnessには、`states`、`transitions`、`state_cap`、探索した実行が到達する `outcomes`、コードごとの `findings` が入ります。`partial` の場合は、`Command.goto` または `Send` を宣言するタイプのノードが `not_analyzable` に一覧され、それぞれに至るトレースが付きます。それらのノードの後の実行は探索されていません。`not_checked` は、`soundness_state_cap` に達したことを意味します。 |

<a id="registry"></a>
## レジストリ

| コード | 意味と対処 |
|---|---|
| <a id="p_registry_missing"></a>`P_REGISTRY_MISSING` | 参照されている実行可能なものが、レジストリのスナップショットにありません。登録するか、候補の名前を使ってください。 |
| <a id="p_registry_dependency_missing"></a>`P_REGISTRY_DEPENDENCY_MISSING` | 実行可能なものが必要とする依存先が、レジストリのスナップショットにありません。 |
| <a id="p_registry_identity_mismatch"></a>`P_REGISTRY_IDENTITY_MISMATCH` | 2つの参照が、1つの実行可能なものの、異なる正確なIDに解決されています。 |
| <a id="p_registry_requirement_kind"></a>`P_REGISTRY_REQUIREMENT_KIND` | ノードの要件リゾルバーが、サポートされていない種類の実行可能なものを返しました。 |
| <a id="p_registry_requirement_resolver"></a>`P_REGISTRY_REQUIREMENT_RESOLVER` | ノードの要件リゾルバーが例外を送出しました。 |

<a id="runtime-and-admission"></a>
## ランタイムとadmission

admissionと実行の失敗は、Pythonでは `ng.ProgramAdmissionError` と `ng.ProgramDiagnosticError` として届きます。実行結果には、コード付きの `failure` が入ります。そのコードに専用の項目がない場合、`help.doc` はこの節にリンクします。

Coreノードの失敗は、診断ではありません。#364以降、その `failure.code` には、ノード失敗の分類器が返すコード（`NG.TaskFailed`、`app.compile_error` のような作成者のコード、`NG.ProviderFailed` など）が入り、`failure.witness.node_failure` にレコードが入ります。failure exitは、宣言されたコードで実行を終わらせます（[Programのグラフ機能](program-graphs.md#failure-routing)を参照）。`P_RUNTIME_CORE_FAILURE` は、ノード失敗のレコードを持たないCoreの失敗に対して残っています。

| コード | 意味と対処 |
|---|---|
| <a id="p_core_barrier_stalled"></a>`P_CORE_BARRIER_STALLED` | Coreの実行が、届くことのないシグナルをjoinが待ったまま止まりました。witnessには、止まったjoinごとに `received` と `missing` が一覧されます。実行された未到着のメンバーからの `paths` も入ります。トレースから代役が見つかる場合は、`suggested_wait_for` も入ります。 |
