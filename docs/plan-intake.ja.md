<!-- neograph-i18n: source=docs/plan-intake.md locale=ja source_sha256=69600014da30bc764c1fde85c2622f45ee13bd5f0a98c35f647aeff50dc58cc9 -->
# Plan intake と plan repair

**Languages:** [English](plan-intake.md) | [한국어](plan-intake.ko.md) | [日本語](plan-intake.ja.md) | [简体中文](plan-intake.zh-CN.md)

task graph を作る planner モデルは、閉じた形式で型のある文書を書かなければなりません。step plan は同じ仕事のためのもっと小さな文書で、順序のある平らな step のリストです。純粋関数がホストのテンプレート allow-list に照らして step plan を検査し、モデルが直せる形ですべての問題を報告し、正しい計画を `TaskGraphProposal` に変換します。その後の proposal は、既存の信頼経路をそのまま通ります。`TaskGraphProposal::parse`、`TaskGraphFragmentCompiler::compile`、公開、`expand_task_graph` の順です。

step plan はデータで、権限を持ちません。intake を通ったことは、計画の形式が正しくポリシーの範囲内にあることを意味します。計画が要求を解決することは意味しません。すべての検査を通っても、見当違いのツールや誤った順序を選んだ計画はありえます。

```
planner text --intake_step_plan--> violations | TaskGraphProposal JSON
TaskGraphProposal JSON --parse--> --compile--> --publish--> expand_task_graph
```

このページでは、step plan、その violation、step plan の変換先であるスキーマ 2 の proposal（明示的な予算、リテラル入力、試行回数）、task の再試行、plan repair ループを扱います。すでに実行中のグラフを変更する機能は、このリリースにはありません。

## step plan

```json
{
  "schema": "step-plan/v1",
  "steps": [
    {"id": "s1", "use": "web.search/v2",
     "params": {"query": "weather in Seoul tomorrow", "limit": 3},
     "retry": {"max_attempts": 2}},
    {"id": "s2", "use": "text.summarize/v1",
     "params": {"text": {"from_step": "s1", "field": "items/0/snippet"}, "style": "one sentence"}},
    {"id": "s3", "use": "notes.save/v1",
     "params": {"title": "Seoul weather", "body": {"from_step": "s2", "output": "summary", "field": "text"}},
     "after": ["s1"]}
  ]
}
```

形式は閉じています。下の表にないキーはすべて violation です。

| キー | 型 | ルール |
|---|---|---|
| `schema` | string | `step-plan/v1` であること |
| `steps` | array | step が 1 個以上、`StepPlanLimits::max_steps`（既定 32）以下 |
| `steps[].id` | string | `[a-z][a-z0-9_]{0,31}`、一意 |
| `steps[].use` | string | ホストの allow-list にあるテンプレート id。動作を選ぶのはこれだけ |
| `steps[].params` | object | 任意。キーはテンプレートの入力名で、`input_fields` の項目から先頭の `/` を除いたもの（ネストした入力 `/filter/key` は `filter/key` と書く）。値はリテラルか参照 |
| 参照 | object | `from_step`（前にある step の id）を持つ。`output` は artifact 名で、既定は生産者の `out`、なければ唯一の artifact。`field` は先頭の `/` を除いた出力フィールドで、既定は artifact の唯一のフィールド |
| `steps[].out` | string | 任意。step が生成する artifact の名前。後の参照の `output` の既定値になる。テンプレートに宣言された artifact であること |
| `steps[].after` | id の配列 | 任意。前にある step だけに張る追加の順序エッジ |
| `steps[].retry` | object | 任意。`{"max_attempts": n}`。n は 1 から、`StepPlanLimits::max_retry_attempts`（3）と `TaskGraphProposalLimits::max_attempts` の小さいほうまで |

リテラルは任意の JSON 値です。文字列、有限の数値、boolean、null、配列、オブジェクトが該当します。リテラル 1 つは正規化した JSON で `max_literal_bytes`（4096）以下、計画全体のリテラルは合計で `max_total_literal_bytes`（32768）以下、ネストは `max_literal_depth`（6）以下です。ホストの `TaskGraphProposalLimits::max_literal_bytes` と `max_total_literal_bytes` も超えられず、リテラルの中のどのオブジェクトも `from_step` キーを持てません。

参照と `after` が指せるのは、前に並べた step だけです。そのため依存グラフは構造上、非循環になります。形式に循環のコードはなく、モデルが守るルールも 1 つだけです。互いに独立な step は並列のままです。エッジは、参照か `after` が作る場合にだけ生まれます。

### step plan で言えないこと

| 形式にないもの | 理由 |
|---|---|
| プロバイダー、モデル、endpoint、URL、認証情報、ヘッダー | 信頼と権限はホストのポリシーです。選択子はテンプレート id だけです。 |
| コード、スクリプト、コマンド、式、テンプレート言語 | 計画はデータです。参照はフィールドを選ぶだけで、何も評価しません。 |
| 予算、タイムアウト、capability と effect の一覧、child binding | ホストがテンプレート契約とポリシーから決めます。 |
| join ポリシー | task graph は all-of で join します。 |
| 条件分岐、ループ、入れ子の計画 | 形式を小さく保ちます。分岐は、計画を実行する Program が担当します。 |

権限を指すキー（`provider`、`endpoint`、`url`、`api_key`、`token`、`secret`、`credential`、`command`、`code`、`script`、`model`、`budget`、`timeout`）には、コード `STEP_FORBIDDEN_FIELD` と、ホストが決めるものだというヒントが付きます。この一覧はヒントを良くするだけです。権限が計画に入らないようにしているのは、閉じたキー集合です。

## 計画の検査

```cpp
StepPlanIntakeOptions options;
options.source_id          = "planner:turn-1";
options.proposal_limits    = policy.limits;              // the limits the host gives TaskGraphProposal::parse
options.template_allowlist = policy.template_allowlist;  // the same trusted list

const auto result = intake_step_plan(model_text, options);
if (result.ok()) {
    const json& proposal = *result.proposal;             // schema 2, canonical; result.proposal_hash is its identity
} else {
    const auto feedback = format_violations_for_model(result.violations, 2048);
}
```

`intake_step_plan` は、planner のテキストか、解析済みの `json` を受け取ります。テキストからは最初に現れる最上位の JSON オブジェクトを使います。コードフェンスの中でも、説明文の後ろでもかまいません。`result.envelope` は、オブジェクトをどう見つけたかを示します。テキスト全体がオブジェクトなら `Strict`、そうでなければ `Recovered` です。1 つのオブジェクトでキーが 2 回現れると `STEP_DUPLICATE_KEY` になります。解析済みの `json` を受け取るオーバーロードは、重複キーを検出できません。

結果には、ソート済みの violation、`plan_digest`（正規化した計画の sha256 identity）、変換後の `proposal` と `proposal_hash`、変換後の proposal のポインターを計画の step に戻す `source_map`、そして `steps`、`edges`、`depth`、`width`、`literal_bytes` の数値を持つ `report` が入ります。intake は決定的です。同じ文書の値（キーの順序と空白は無関係）と同じオプションなら、violation も proposal のバイト列も同じです。計画の内容のために例外を投げることはなく、オプション自体が不正なときだけ `std::invalid_argument` を投げます。

`StepPlanIntakeOptions::template_info` は参考情報で、拒否を増やすことしかできません。`required_params` は欠けた入力を violation にし、`summary` は planner のプロンプトに使うテキストです。`step_plan_json_schema(templates, limits)` は、カタログに合わせた JSON Schema を生成します。`use` は許可された id の enum、`params` はテンプレートごとの入力名の一覧です。構造化出力を強制するプロバイダー向けの補助で、最終的な権限は検証器にあります。[`schemas/step-plan-v1.schema.json`](../schemas/step-plan-v1.schema.json) は、カタログに依存しない形です。

## violation

violation は `{path, code, message, hint, detail}` です。`path` は、提出された文書を指す JSON Pointer です（`""` は文書全体）。メッセージに入るのは id、テンプレート名、コードだけです。計画から取った文字列は、64 バイトに切り、表示可能な文字だけにしてから出し、リテラルの値は決して出しません。そのため、モデルが書いた指示がフィードバックに載って次のプロンプトへ戻ることはありません。ヒントは命令文 1 文で、有効な名前を最大 5 個挙げます。

一覧は path 順に、セグメントごとに比較してソートします。数値のセグメント同士は整数として比較します（`/steps/2` は `/steps/10` の前）。次に code、detail の順です。`max_violations`（32）で打ち切り、漏れがあれば `STEP_VIOLATIONS_TRUNCATED` で終わります。構造に violation が 1 つでも見つかった場合、計画単位の上限（`STEP_LIMIT_EDGES`、`STEP_LIMIT_DEPTH`、`STEP_LIMIT_BUDGET`）は計算しません。健全なグラフがないと計算できないためです。

| コード | path | 意味 |
|---|---|---|
| `STEP_NOT_JSON` | `""` | テキストに、完全で正しい JSON オブジェクトがない |
| `STEP_DUPLICATE_KEY` | 2 つ目のキー | オブジェクトでキーが繰り返された |
| `STEP_TYPE` | 該当の値 | JSON の型が違う。`detail.expected` が正しい型を示す |
| `STEP_SCHEMA` | `/schema` | ないか、`step-plan/v1` ではない |
| `STEP_UNKNOWN_FIELD` | 該当のキー | 閉じた集合にないキー |
| `STEP_FORBIDDEN_FIELD` | 該当のキー | 権限を指す語。ホストが決める |
| `STEP_EMPTY`、`STEP_LIMIT_STEPS` | `/steps` | step がない、または多すぎる |
| `STEP_LIMIT_PARAMS`、`STEP_LIMIT_AFTER`、`STEP_LIMIT_DOCUMENT` | 該当の一覧または `""` | step の入力や `after` の項目が上限より多い、文書が `max_document_bytes` より大きい、またはネストが深すぎる |
| `STEP_ID_REQUIRED`、`STEP_ID_FORMAT`、`STEP_ID_DUPLICATE` | `/steps/i/id` | id の問題 |
| `STEP_USE_REQUIRED`、`STEP_USE_UNKNOWN` | `/steps/i/use` | ない、または allow-list にない。ヒントが最も近い id を示す |
| `STEP_PARAM_UNKNOWN`、`STEP_PARAM_MISSING` | `/steps/i/params/key` | テンプレートにその入力がない、または必須の入力がない |
| `STEP_LITERAL_TOO_LARGE`、`STEP_LITERAL_DEPTH`、`STEP_LITERAL_RESERVED_KEY`、`STEP_LITERAL_NUMBER` | 該当の param | リテラルの上限（リテラルを受け付けないホストを含む）、リテラル内の `from_step` キー、有限でない数値 |
| `STEP_REF_SYNTAX` | 該当の参照 | 参照オブジェクトの形が不正 |
| `STEP_REF_UNKNOWN_STEP`、`STEP_REF_FORWARD` | `.../from_step` | step ではない、または前にある step ではない（step は自分自身を指せない） |
| `STEP_REF_OUTPUT_UNKNOWN`、`STEP_REF_FIELD_UNKNOWN`、`STEP_REF_FIELD_AMBIGUOUS`、`STEP_REF_OUT_MISMATCH` | `.../output`、`.../field` または該当の param | 出力やフィールドが生産者の契約にない。複数あるのに指定がない（ヒントが一覧する）。`output` が生産者の `out` と違う |
| `STEP_OUT_UNKNOWN` | `/steps/i/out` | テンプレートに宣言された artifact ではない |
| `STEP_AFTER_UNKNOWN`、`STEP_AFTER_FORWARD`、`STEP_AFTER_DUPLICATE` | `/steps/i/after/j` | 順序エッジの問題 |
| `STEP_RETRY_FIELD`、`STEP_RETRY_RANGE` | `/steps/i/retry/...` | `max_attempts` 以外のキー、または許可範囲外の値 |
| `STEP_LIMIT_EDGES`、`STEP_LIMIT_DEPTH`、`STEP_LIMIT_BUDGET` | `/steps` | 計画単位の上限。依存の数、最長の連鎖、step ごとの予約予算の合計（再試行は試行ごとに 1 回数える） |
| `STEP_LOWERING_REJECTED` | 対応する step | `TaskGraphProposal::parse` が変換結果を拒否した。`detail.proposal_code` に `P_PROPOSAL_*` のコードがある。intake が検査する上限とホストのポリシーが食い違っている、または intake に抜けがあることを意味し、最終的な権限は parse にある |
| `STEP_VIOLATIONS_TRUNCATED` | `""` | 一覧が打ち切られた |

モデルが受け取る内容の例です。

```json
[
  {"path": "/steps/2/use", "code": "STEP_USE_UNKNOWN",
   "message": "Template 'web.serach' is not available.",
   "hint": "Use one of: text.summarize/v1, web.search/v2. Closest: web.search/v2.",
   "detail": {"given": "web.serach", "choices": ["text.summarize/v1", "web.search/v2"]}},
  {"path": "/steps/3/params/text/from_step", "code": "STEP_REF_FORWARD",
   "message": "Step 's4' does not come before this step.",
   "hint": "Reference only steps listed earlier in 'steps'; reorder the steps or drop the reference."}
]
```

## 変換（lowering）

変換は、文書とオプションだけで決まる純粋関数です。

- task は step の順序に従います。参照は入力 binding に、リテラルはスキーマ 2 のリテラルに、`after` と参照の生産者は `depends_on`（ソート済み、重複なし）になります。
- planner は予算を選びません。各 step は、すべての次元で、テンプレートの上限とホストの task ごとの上限のうち小さいほうを予約し、4 つの次元がすべて明示されます。`n` 回試行する再試行は、これを `n` 回予約します。
- proposal は常にスキーマ 2 なので、ホストの起動時の予算は正確です（下記）。intake が返す前に `TaskGraphProposal::parse` で解析します。返される `proposal` はすでに正規形です。

## スキーマ 2 の proposal

`TaskGraphProposal` のスキーマ 2 は、step plan の変換先であり、planner が直接書いてもかまいません（[`schemas/task-graph-proposal-v2.schema.json`](../schemas/task-graph-proposal-v2.schema.json) を参照）。スキーマ 1 の文書は、正規バイト列と identity が変わりません。スキーマ 1 と宣言しながらスキーマ 2 のメンバーを使う文書は、`P_PROPOSAL_SCHEMA_VERSION` です。

### 明示的な予算（#412）

スキーマ 1 では、予算の次元を 0 のままにした task は、proposal と fragment の上限では 0 と数えられましたが、子はその次元について link の上限全体を受け取って起動し、空の `output_bytes` はどんな出力も許していました。スキーマ 2 はこの隙間を塞ぎます。

- task は、テンプレートの上限が 0 でないすべての次元に、0 でない値を明示する必要があります（`P_PROPOSAL_BUDGET_ZERO`）。0 のままにできるのは、テンプレートの上限が 0 の次元だけです。
- スキーマ 2 の fragment は、各 task を予約した値どおりに起動します。0 の次元は `RunBudget` と同じく何も許さず、出力は明示した `output_bytes` に制限されます。

| | スキーマ 1 | スキーマ 2 |
|---|---|---|
| 空けた予算の次元 | 許容。0 と数える。子は link の上限を受け取る | テンプレートが使える次元なら `P_PROPOSAL_BUDGET_ZERO` |
| `output_bytes` が 0 | どんな出力も通る | どんな出力も収まらない |
| 既存の文書の正規バイト列と identity | 変わらない | 該当なし |

planner から proposal を受け取るホストは、`TaskGraphProposalLimits::min_schema_version` を 2 にしてスキーマ 1 を拒否します。

### リテラル入力

task は `literals` を持てます。`[{"to": {"field": "/question"}, "value": ...}]` の形です。リテラルは、テンプレートに宣言された入力フィールド 1 つに JSON の値を代入し、子の入力で binding の後に適用されます。対象はテンプレートの入力であり（`P_PROPOSAL_LITERAL_TARGET`）、ほかの binding やリテラルを受け取らないことが条件です（`P_PROPOSAL_LITERAL_DUPLICATE_TARGET`）。リテラルはデータで、評価されません。正規形では対象の順に並びます。

### 試行回数

task は `max_attempts` を宣言できます（1 からホストの `TaskGraphProposalLimits::max_attempts` まで。`P_PROPOSAL_ATTEMPTS`）。試行のたびに、task の予算全体を持つ新しい子を起動するので、集計は予算を試行回数分だけ予約します。task の `reserved_budget` は `budget` に `max_attempts` を掛けた値で、proposal も fragment も、その合計を上限と比べます。

### ホストの上限

| `TaskGraphProposalLimits` | 既定値 | 意味 |
|---|---|---|
| `min_schema_version` | 1 | 受け付ける最も低いスキーマ |
| `max_attempts` | 1 | task が宣言できる最大の試行回数。1 なら再試行なし |
| `max_literal_bytes` | 0 | リテラル 1 つの正規 JSON のバイト数。0 ならリテラルを受け付けない |
| `max_total_literal_bytes` | 0 | proposal のすべてのリテラルの正規 JSON のバイト数の合計 |

拒否された expansion は、最初のいくつかの diagnostic を `failure.witness.diagnostics` に `{code, pointer}` として報告します。

## task の再試行

task のまとまりが終わった後、子がノード失敗の分類器によって再試行可能とされた失敗で終わり（`failure.witness.node_failure.retryable` が true で、`kind` が `effect_committed`、`ceiling`、`deadline` のいずれでもない。plan レベルの `retry` 演算と同じ規則）、試行の残りがある task は、次の試行番号で再びディスパッチされます。キャンセルされた task やタイムアウトした task、ノード失敗ではない失敗（存在しない入力、契約を外れた出力）は、再試行しません。

- 終端状態は一度決まったら変わらないため、再試行する試行については `Failed` を書き込みません。次のディスパッチが、task を次の試行番号と新しい試行 identity で `Active` として書き込み、前の試行の失敗は、次の結果が置き換えるまでレコードに残します。試行番号は減りません。
- 試行ごとに別の子があり、run id は試行 identity から導かれ、task の予算全体を受け取ります。
- 再試行は、グラフを次にスキャンするときに起こります。新しく準備ができた task と同じ扱いです。再試行される task に依存する task は待ち、`fail_fast` ポリシーでも、再試行可能な失敗はほかの task をキャンセルしません。
- 次の試行が始まる前にクラッシュしたときは、同じ試行を再起動します。その子はすでに失敗を持っているので、同じ判定が再び下されます。

## plan repair

モデルの最初の計画は、正しくないことがよくあります。plan repair は、violation をプロンプトに入れて、決まった回数まで聞き直します。新しい plan 演算ではなく、既存の仕組みの上に作るパターンです。Core グラフ `plan -> intake -> (plan again)` で、`intake` は登録された純粋ノードです。run の予算、プロバイダー呼び出しのジャーナル、Core のチェックポイントが、ループに必要なものを与えます。回数の制限、トークンと費用の上限、締め切り、記録された状態です。そのため、再開した run が、すでに受け取った答えをモデルに聞き直すことはありません。

```json
{
  "schema_version": 1,
  "name": "main",
  "channels": {
    "messages": {"reducer": "overwrite", "initial": [{"role": "user", "content": "..."}]},
    "proposal": {"reducer": "overwrite", "initial": null},
    "repair": {"reducer": "overwrite", "initial": {}}
  },
  "nodes": {
    "plan": {"type": "llm_call"},
    "intake": {"type": "neograph.step_plan_intake/v1", "max_attempts": 3, "feedback": "replace"}
  },
  "edges": [
    {"from": "__start__", "to": "plan"},
    {"from": "plan", "to": "intake"}
  ],
  "conditional_edges": [
    {"from": "intake", "condition": "neograph.step_plan_route/v1",
     "routes": {"ok": "__end__", "retry": "plan", "give_up": "__end__"}}
  ],
  "node_retry_policies": {"plan": {"max_visits": 3}}
}
```

Program の中では、Core 定義の `name` が、それを埋め込むルート演算の名前（ここでは `main`）と一致している必要があります。`plan` は、`messages` を読み、モデルの答えを assistant メッセージとして追加するノードなら何でもかまいません。その後、Program は `"proposal_source": {"artifact": "input", "field": "/channels/proposal/value"}` により、planner の出力から受理された proposal を expand します。[`examples/65_program_plan_intake.cpp`](../examples/65_program_plan_intake.cpp) は、この流れをスクリプト化した planner でオフラインのまま実行します。誤りが 3 つある最初の応答、修正のターン 1 回、受理された計画が子 Program に expand されるところまでです。

### 登録される実行体

`add_step_plan_repair(builder, options)` は、`RegistrySnapshotBuilder` に実行体を 3 つ登録します。`StepPlanRepairNodeOptions::intake` が、信頼する allow-list と上限を持ちます。封印されたグラフはこれを変更できず、実行体の identity がこれを含むので、ある allow-list で承認された Program が別の allow-list で動くことはありません。

| 実行体 | 役割 |
|---|---|
| `neograph.step_plan_intake/v1`（ノード） | `messages` の最後の assistant メッセージを読み、`intake_step_plan` を実行して、`proposal`（変換後の proposal か null）と `repair` を書き、次の試行があるときは `messages` も書く。設定は `max_attempts`（1 から `max_attempts_ceiling` まで、既定 3）と `feedback` |
| `neograph.step_plan_route/v1`（条件） | `repair.status` に応じて `ok`、`retry`、`give_up` を返す。それ以外の値は `give_up` |
| `neograph.step_plan_report/v1`（ノード） | `repair.status` を `exhausted` に書き直し、ほかには何もしない。ループを failure exit で終えるグラフ向け |

`feedback: "replace"`（既定）は、試行ごとに `messages` を、system メッセージ、最初の user メッセージ、却下された計画、violation で作り直します。コンテキストは増えず、前の間違いを読み直すこともありません。`"append"` は会話全体を残して violation を追加します。ノードは `messages` を丸ごと書くので、チャネルの reducer は関係ありません。violation は `format_violations_for_model` でテキストにし、`max_feedback_bytes`（2048）で制限します。

### レポート

`repair` チャネルにはレポートが入ります。`repair_step_plan` が返す値と同じです。

```json
{"status": "exhausted", "attempts": 3, "max_attempts": 3,
 "last_violations": [{"path": "...", "code": "STEP_USE_UNKNOWN", "message": "...", "hint": "...", "detail": {}}],
 "violation_code_histogram": {"STEP_USE_UNKNOWN": 3, "STEP_REF_FORWARD": 1},
 "plan_digests": ["sha256:...", "sha256:...", "sha256:..."],
 "envelope": ["strict", "recovered", "strict"]}
```

`status` は `ok`、`retry`、`exhausted` のいずれかです。`last_violations` は 8 KiB で打ち切られます。

### 上限と最終的な失敗

| 上限 | 仕組み |
|---|---|
| 試行回数 | ホストの上限内での intake ノードの `max_attempts`。`plan` の `max_visits` は、上のロジックが誤っていたときに run を失敗させる安全網 |
| トークンと費用 | run の予算。planner グラフを子 Program（`spawn` を `await` で join）として実行すれば、計画の段階に別の上限を与えられる |
| 経過時間 | run の `wall_time_ms` と、モデル呼び出しごとの `attempt_timeout_ms` |
| モデル呼び出しの再試行 | `plan` ノードの `max_retries`。上の試行回数とは別 |

最後の試行の後に終える方法は 2 つあります。

- フォールバック（ユーザーに向き合うフローで推奨）。`give_up` の経路を `__end__` に向けます。レポートは `exhausted` を示し、親の Program が `/channels/repair/value/status` で分岐して、直接答えるかユーザーに尋ねます。fragment は公開されません。
- 失敗。`give_up` の経路を、コード `plan.repair_exhausted` の failure exit として宣言した `neograph.step_plan_report/v1` ノードに向けます。run はそのコードで `Failed` になり、出力にはレポートが残ります。

予算による停止は、既存の `BudgetExhausted` 状態とコードになるので、ホストは「モデルが直せなかった」と「お金が尽きた」を区別できます。

### run の前に計画する

run を始める前に計画するホストは、`repair_step_plan` を使います。

```cpp
PlanRepairOptions options;
options.intake       = intake_options;
options.max_attempts = 3;
const auto result = repair_step_plan(options, request_text,
    [&](const std::vector<PlannerMessage>& messages) {
        return PlannerReply{call_model(messages), prompt_tokens, completion_tokens};
    });
if (result.ok) start_run(result.proposal);   // the proposal becomes the run input
```

受理された proposal が run の入力になり、run レコードがそれを保存するので、再開しても再び計画することはありません。費用の集計はホスト自身の役目です。run の予算には計画が含まれません。計画の費用を run に計上したいときは、Core グラフを使ってください。

## 互換性とロールアウト

このツリーは v1 より前です。これらの変更は公開構造体にメンバーを追加するので、NeoGraph、すべての C++ の利用側、Python 拡張をまとめて再ビルドしてください（[`docs/ABI_POLICY.md`](ABI_POLICY.md) と変更履歴のエントリを参照）。

| レコード | ルール |
|---|---|
| スキーマ 1 の proposal | 正規バイト列と identity は変わらない |
| スキーマ 2 の proposal | 新規。古いバイナリは `P_PROPOSAL_SCHEMA_VERSION` で拒否する |
| スキーマ 1 の fragment と、その保存レコード | 変わらず、従来どおり起動される（空けた次元は link の上限を受け取る） |
| スキーマ 2 の fragment | 新規。古いバイナリは解析を拒否する |

保存済みデータの移行は要りません。書き込み側がスキーマ 2 の proposal、リテラル、再試行を作り始める前に、run を復旧できるすべてのプロセスをアップグレードしてください。スキーマ 2 の fragment ができた後にロールバックするには、先にその expansion をすべて終わらせる必要があります。

このリリースに含まれないものは、実行中のグラフの変更、SQLite や PostgreSQL 向けの耐久的な fragment ストア、planner のためのテンプレート費用カタログビューです。
