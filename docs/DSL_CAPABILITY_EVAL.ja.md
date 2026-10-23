<!-- neograph-i18n: source=docs/DSL_CAPABILITY_EVAL.md locale=ja source_sha256=8629673a4ffa2bad19d7820ef967e71430cbd2f853599858f6a6d053e70af8af -->
# QuickJS DSLのcapabilityとモデルによる合成の評価

**Languages:** [English](DSL_CAPABILITY_EVAL.md) | [한국어](DSL_CAPABILITY_EVAL.ko.md) | [日本語](DSL_CAPABILITY_EVAL.ja.md) | [简体中文](DSL_CAPABILITY_EVAL.zh-CN.md)

## 問い

NeoGraphは、次の2つの主張を区別する必要があります。

1. admission済みのQuickJS DSLが、あるcapabilityを表現できること。
2. LLMが、そのcapabilityを実際に正しく使うソースを合成できること。

2つ目の主張は、1つ目から導かれるものではありません。ソースがこの評価に合格するのは、本物の `ProgramCompiler` が受理し、さらに、ケースごとの意味検証が、ローワリングされたCore IR、またはシール済みのJavaScriptコマンドツリーを確認した場合だけです。ソーステキストのキーワードを照合するだけでは、十分ではありません。

## Capabilityマニフェスト

[`tests/fixtures/dsl_capabilities/cases.json`](../tests/fixtures/dsl_capabilities/cases.json)は、機械可読な評価対象の一覧です。現在、次のものを対象にしています。

- graph / channel / node / entry / edge / exit と、通常のJavaScriptによる構築
- 登録済みの条件付きルーティング
- 静的なfan-out、fan-in、バリア
- 静的なHITLの割り込みと、グラフのリトライポリシー
- レジストリを介した動的な `Send` と `NodeInterrupt` の挙動
- 通常のJavaScriptの分岐とループの制御を伴う `callCore`
- 上限付きの `ng.all` にローワリングされるJavaScriptのmap
- `all`、`parallel`、汎用の `join`、`race`、`quorum`
- `await` の中に入れ子にした、子の `spawn`
- `emit`、`checkpoint`、`cancelScope`
- admission済みのネイティブな `hostCapability` のimportスロット

リポジトリに含まれるJavaScriptのフィクスチャは、それぞれ `program_dsl_capability_probe` でコンパイルされ、意味検証されます。これらは決定的なCTestのテストで、モデルもネットワークも必要としません。

```powershell
cmake --build build --config Release --target program_dsl_capability_probe
ctest --test-dir build -C Release --output-on-failure `
  -R '^Program\.DslCapability\.'
```

グラフビルダーに絞ったリグレッションテストでは、ミューテーターを繰り返し呼び出しても、正しく積み上がることと、バリア、割り込み、リトライの宣言がローワリングの後も残ることも確認します。

```powershell
build\tests\Release\neograph_program_tests.exe `
  --gtest_filter=ProgramCompilerTest.JavaScriptGraphBuilderLowersEveryDeclaredPrimitiveAndAccumulatesCalls
```

## 実モデルでの評価

オプトインのランナーは、自然言語での意味の説明と、公開APIのシグネチャを使って、ソースを求めます。リポジトリに含まれる正解は、モデルに渡しません。すべての応答は、決定的なCTestと同じネイティブのプローブに送られます。ランナーは、`skills/neograph-harness-authoring/SKILL.md` と、その `references/quickjs-authoring.md` を、モデルのコンテキストに読み込み、ガイダンスのダイジェストをレポートに記録します。`--skill` で、その付随するリファレンスを持つ別のエントリポイントを選べます。ホストが実際のコンパイラーブリッジを呼び出します。モデルはソースを返し、診断を受け取って、回数に上限のある修正を行います。skillが、ランタイムの権限を与えることはありません。

```powershell
bun --env-file=C:\path\to\.env run scripts/run_dsl_capability_eval.ts `
  --probe build\tests\Release\program_dsl_capability_probe.exe `
  --model z-ai/glm-5.3-flash `
  --repair-attempts 2 `
  --output dsl-capability-evidence.json
```

`--case` は、カンマ区切りのサブセットを受け付けます。`--attempts` は、独立した1回限りの試行を繰り返します。`--repair-attempts` は、却下された完全なソースとともに、プローブが出した確定的な診断をモデルに返します。プロバイダーやレスポンスの失敗は、コンパイルや意味検証による却下とは別に扱われます。デフォルトのモデルは `z-ai/glm-5.3-flash` です。1回の生成は、補完トークン4,096個までに制限され、cookbookのZDRのプロバイダールーティング設定を使います。以下のDeepSeekの結果は、ランナーがskillを読み込む前に記録したものです。

`--provider anthropic` を指定すると、リクエストは Anthropic Messages API に送られ、`ANTHROPIC_API_KEY` が使われます。デフォルトは引き続き `openrouter` です。現在のモデルの一部は `temperature` パラメーターを拒否するため、`--temperature` を指定しない限り送りません。また、OpenRouter の設定である `--reasoning-effort` は受け付けません。`--dry-run` は、キーもネットワーク呼び出しも使わずに、最初のターンと修正ターンのリクエストを、秘密のヘッダー値を伏せた状態でそのまま出力します（ネイティブのマニフェストを読むために `--probe` は引き続き実行します）。使えるレスポンスが得られない失敗（ネットワーク、レート制限、過負荷、拒否されたリクエスト）は `failureKind: "provider"` として記録され、`--provider-retries` で再試行できます。ソースの封筒として使えないレスポンスは `model_output` です。レポートの `summary.usage` は、修正ターンを含むすべてのレスポンスの入力トークンと出力トークンを合計します。

## 指示を別に評価する

ソースのケースが合格したときに測れるのは、skill、与えたネイティブAPIのリファレンス、ケースの契約、モデルの構成を、組み合わせた結果です。skill単独で十分であることの証明にはなりません。ランナーは、システムプロンプト、APIリファレンス、モデルが返した生の内容、使用量、停止理由、strictエンベロープの結果を、エビデンスとして保持します。従来のソース抽出は、コードフェンスで囲まれたJSONを復元することがあります。strictEnvelopeは、それと、求めた生のJSONエンベロープに従っていることを区別します。

--reasoning-effortを使うのは、別の比較用プロファイルを明示的に選ぶ場合だけです。省略すると、プロバイダーのデフォルトが保たれます。変更前後の比較ごとに、この値、モデル、サンプリング、出力の上限、タスク、コンパイラーを固定してください。

チャットボットの内部のHarnessセレクターには、対になる評価ツールがあります。

~~~powershell
bun run scripts/run_harness_skill_ab.ts --before saved-skill/SKILL.md `
  --after skills/neograph-harness-authoring/SKILL.md `
  --provider relace --repeats 2 --output skill-comparison.json
~~~

どちらのディレクトリにも、references/chat-template-proposals.md が必要です。デフォルトのフィクスチャには、挨拶、独立したレビューを求める継続中のリクエスト、記録済みのレビューのシナリオが含まれます。期待されるプランは、モデルの入力には含まれません。評価ツールは、実際のチャットのプロファイルを固定し、AB/BAの順序を交互にし、生の応答を保持して、JSONの形と、期待されるプランおよび確信度とを、別々に採点します。--providerは、フォールバックを無効にして、バックエンドを固定します。指定しない場合、ルーティングのポリシーは共通ですが、実際のプロバイダーは異なることがあります。返されたプロバイダー名とモデル名は記録されます。ケース数と繰り返し回数には上限があり、修正や黙ってのリトライは行いません。

これはプロンプトのレベルの評価です。ランタイムのadmissionでも、回答の品質についての統計的な主張でもありません。対象を絞ったケースを少数回繰り返したサンプルは、リグレッションのエビデンスであり、モデル一般の信頼性の推定値ではありません。

## 現在の結果（2026-10-23）

合格率ではアームを区別できませんでした。ランナーの API リファレンスとケースの文面があれば、`claude-haiku-5-5` は、以前の skill、書き直した skill、skill のテキストがまったくない場合のいずれでも、110試行中110試行で、最初の試行からコンパイラが受理しケースの検証器が確認したソースを書きました。この測定は上限（ceiling）に達しているため、skill が役に立つことも、書き直しが役に立ったことも示せません。

条件: Anthropic Messages API 経由の `claude-haiku-5-5`、`cases.json` の11ケース、アームごとに2回の実行、各実行でケースごとに独立した1回限りの試行を5回（アームあたり110試行）、`--repair-attempts 2`、生成ごとに思考を含めて `max_tokens` 4,096。このモデルでは API が `temperature` を拒否するため、サンプリングのパラメーターは送っていません。モデルはプロバイダーのデフォルトで適応的な思考を行いながらサンプリングしており、各試行は本物の標本です。プロトコル、ホールドアウトのケース（ケース ID の先頭、中央、末尾である `graph_basics`、`program_durability`、`registry_mediated`）、分析方法は、最初の試行の前に、リポジトリに含まれないローカルのファイルに書きました。skill なしの対照と各アームの2回目の実行は、最初の実行が上限に達していると分かった後に書いた補遺として加えました。ホールドアウトのソースと診断は、改訂が確定するまで読んでいません。再試行が必要になったリクエストはなく、プロバイダーの失敗で終わった試行もありませんでした。

1回の実行をやり直すには次のようにします。

```powershell
bun --env-file=C:\path\to\.env run scripts/run_dsl_capability_eval.ts `
  --probe build\tests\Release\program_dsl_capability_probe.exe `
  --provider anthropic --model claude-haiku-5-5 `
  --attempts 5 --repair-attempts 2 --provider-retries 5 `
  --skill path\to\SKILL.md `
  --output evidence.json
```

キーは環境の `ANTHROPIC_API_KEY` から読み込み（ここでは env ファイルが設定します）、どの出力にも現れません。「この変更より前の skill」はこの変更の基準コミットにあった skill で、skill なしの対照は、空のエントリーファイルとその隣の空の `references/quickjs-authoring.md` です。55試行の1回の実行は約1分かかり、入力トークンは skill のテキストがあると23万〜27万個、ないと6万個でした。8回の実行とスモーク試行1回で、入力160万個、出力31万個のトークンを使いました。実行ごとのレポートはリポジトリに含めません。

| アーム | Skill のダイジェスト | 1回で成功 | 修正後 | 平均修正回数 | フェンス付き応答 | 最初のターンの入力トークン（平均） |
|---|---|---|---|---|---|---|
| この変更より前の skill | `b59a0a379690` | 110/110 | 110/110 | 0.00 | 7/110 | 4890 |
| 書き直した skill（測定したテキスト） | `1ec37bcd85e1` | 110/110 | 110/110 | 0.00 | 3/110 | 4306 |
| 中間の改訂（採用しない） | `34509f71ec3c` | 107/110 | 110/110 | 0.03 | 3/110 | 4154 |
| skill のテキストなし（対照） | `75a11da44c80` | 110/110 | 110/110 | 0.00 | 0/110 | 1087 |

「より前」、書き直した skill、対照のアームでは、すべてのケースが1回で10/10合格しました。中間の改訂は `program_control_flow`、`program_durability`、`program_host_capability` でそれぞれ1回ずつ外し、いずれも最初の修正で直りました。ホールドアウトの3ケース（アームあたり30試行）では、「より前」、書き直した skill、対照が1回で30/30、中間の改訂が29/30でした。

失敗のクラスは、却下された、または規則から外れたすべての応答を数えたものです。

- 却下されたソース: 「より前」も書き直した skill も、110試行中なし。
- フェンス付き応答: プロンプトが禁じているにもかかわらず、JSON の封筒が Markdown のフェンスで囲まれて返ってきました。抽出器が復元するのでこれらの試行は合格しますが、要求した生の封筒の形式には反します。前が7/110（2回の実行で3/55と4/55、うち4件は `program_structured_concurrency`）、書き直した skill が3/110、skill のテキストなしでは0/110。
- 中間の改訂、最初のターンの却下3件: 与えられた名前ではなくケース名にちなんだグラフ名（2件、`graph name must be capability`）と、プレーンなオブジェクトだった応答から `channels` ラッパーを読んだもの（1件、`TypeError: cannot read property 'accepted' of undefined`）。その改訂は、これらを防ぐ2つの規則を落としており、書き直した skill は両方を戻しています。

データが裏づけること: ランナーの API リファレンスとケースの文面があれば、モデルは skill があってもなくても11の capability グループすべてを最初の試行で書け（110/110）、この測定では skill が合格率を上げることは示せず、以前の skill、書き直し、skill なしを区別することもできませんでした。違ったのはコストと応答の形式です。以前の skill のテキストは1回の呼び出しあたり入力トークンを約3,800個増やし（対照の入力の4.5倍）、書き直したテキストは約3,200個で、最初のターンの入力は12%減りました。フェンス付き応答は、以前のテキストのほうがテキストなしより多かったものの（7/110 対 0/110、Fisher の p = 0.014）、実行ごとに見ると有意ではなく（3/55 と 4/55 対 0/55、p = 0.24 と 0.12）、p 値は複数の比較について補正しておらず、7件のうち4件は1つのケースに集中しており、以前のテキストのどこが原因かは調べていません。書き直したテキストはどちらとも区別できません（3/110、「より前」とは p = 0.33、対照とは 0.25）。中間の改訂の3/110の取りこぼしは有意ではありません（p = 0.25）が、それぞれテキストの中に原因が見えます。書き直したテキストは、ホールドアウトでない8ケースで中間の改訂の失敗を読んだ後に書いたため、そこでの結果は独立した検証ではありません。手つかずの比較はホールドアウトのケースだけで、30/30 対 30/30 ではアームを分けられません。

測定後の修正。「書き直した skill」として測定したテキストのダイジェストは `1ec37bcd85e1`、この変更が出荷するテキストのダイジェストは `753b82da7b2d` です。実行の後でリファレンスをコンパイラと突き合わせ、6つの記述を直しました。`import` はないという記述、「未知のキーは拒否される」の適用範囲、`exit` が必須で `__end__` はエッジの行き先にならないという記述、`yield*` は拒否されるという記述（拒否されるのはコマンドに対するものだけ）、`cancelScope` が受け取るスコープ、拒否が返ってくる形です。出荷するテキストは688バイト（約230トークン）長くなり、最初のターンの入力は約4,540トークンで、以前の skill より7%少なくなります。110試行では測定していません。3つのケース（`program_durability`、`program_map`、`graph_routing`）をケースごとに3回ずつ回したスモーク実行は、1回で9/9、生の封筒9/9で合格し、プロバイダーの失敗はありませんでした（入力40,761個、出力5,212個のトークン）。この実行は回帰の確認であり、測定ではありません。

これは、1つのモデル、1つのプロバイダー、1つのサンプリング設定について、契約をプロンプトが明示しているケースで行った、小標本の capability の測定であり、信頼性についての主張ではありません。skill の効果を示すには、助けのないモデルが時々失敗するタスクが必要です。skill のチャットテンプレートと Harness MCP の面は測定していません。

## 以前の結果（DeepSeek）

この以前の実行は、別のモデル、以前の skill、以前の Program 実装を使い、ランナーが skill を読み込む前に記録したものです。実モデルでの実行で、モデルは、11のcapabilityグループすべてについて、プローブで検証されたソースを生成しました。静的なHITLとリトライでは、診断に導かれた修正が1回必要でした。構造化された並行性では、修正が2回必要でした。最初はESモジュールのexportを復元するため、次に誤ったCoreのバインディングを、admission済みの正確な名前に置き換えるためです。制御フローは、正確なバインディングへの修正を1回行って合格しました。mapは、ホストがネイティブAPIのマニフェストを与え、admission済みのCoreの識別子を曖昧さなく明示した後で、1回で合格しました。

| Capabilityケース | モデルのエビデンス | 重要な観測 |
|---|---|---|
| `graph_basics` | 合格 | ループで組み立てたノードが、正しくローワリングされました |
| `graph_routing` | 繰り返しの試行で合格 | 以前の出力は、禁止されているCommonJSと `require` を使っていました |
| `graph_fanout_barrier` | 合格 | fan-outのエッジとバリアのメンバーが正確でした |
| `graph_hitl_retry` | 修正1回で合格 | 最初の出力は、誤ったノード名を使っていました |
| `registry_mediated` | 合格 | モデルは、ホストがadmissionした動的ノードを、正しく参照しました |
| `program_control_flow` | 修正1回で合格 | 以前の試行では、admission済みのCore `capability` ではなく、`core` や `Core` を繰り返し呼び出していました |
| `program_map` | 識別子を正確に与えた後で合格 | 以前の試行では、CommonJS、`yield*`、誤ったCore名を使っていました |
| `program_structured_concurrency` | 修正2回で合格 | 入れ子のコマンドとCoreのバインディングが、正確であることを確認しました |
| `program_spawn_await` | 合格 | `Await(Spawn(...))` とタイムアウトの構造を確認しました |
| `program_durability` | 合格 | emit、checkpoint、キャンセルのコマンドが正確でした |
| `program_host_capability` | 合格 | importスロットと正規の入力が一致しました |

バリデーターは、コマンドの種類と入力に加えて、Coreの正確な名前も確認し、入れ子のすべての `callCore` に `capability` を要求します。`core`、`Core`、ノード名 `work` を使うモデルのソースは却下されます。

これは、できることの証明であり、信頼性についての統計的な主張ではありません。現在のモデルでの繰り返し試行と、別に報告したプロバイダーの失敗は、上の「現在の結果」にあります。

## 生成されたProgramへの影響

生のソースを1回で生成するだけでは、プロダクトとしての保証には十分ではありません。最低限の安全な合成の経路は、次のとおりです。

```text
capability manifest + exact admitted identifiers
  -> model source proposal
  -> bounded QuickJS compilation
  -> semantic capability probe
  -> diagnostic-guided repair within a fixed budget
  -> ordinary admission and publication
```

以前の実行では、モデルは、ESモジュールとCommonJS、グラフ名とノード名、要求されたCoreのIDと `core` のような一般的な語を、繰り返し取り違えました。そのため、ホストは、正確なシグネチャと、admission済みの識別子を注入し、可能な限り、モジュールの骨組みを固定のまま保ち、それらしく見えるソースを、要求したトポロジーが構築されたことのエビデンスとして扱ってはいけません。

NeoGraphは、この境界を `ProgramSynthesisGateway` で強制します。すべてのゲートウェイの構成は、ホストが所有する意味検証を提供しなければなりません。検証に成功すると、Catalogのadmissionの前に、内容でアドレス指定されたレシートが生成されます。却下の判定では、`ProgramSynthesisValidationError` が送出され、正確なエビデンスが保持され、admissionのリゾルバーが呼び出されることはありません。すでに消費された動的コンパイルの予約は、消費されたままです。

`javascript_authoring_capability_manifest()` は、インストールされているグラフビルダーとコマンドの語彙、正確なシグネチャ、分類、上限、プロファイルの制約を、機械可読なデータとして公開します。適合性テストは、このマニフェストを、2つのQuickJSコンテキストに実際にインストールされているプロパティと比較します。そのため、APIがずれるとテストスイートが失敗します。

マニフェストは、さらに、次のものも公開します。オーサリングの機能ごとに1つの真偽値フラグを持つ `features`（たとえば `graph.metadata`、`barrier.any_of`、`failure_edges`）。すべてのメソッドのTypeScript用の `dts` シグネチャ（共有する宣言は `dts_prelude` にあります）。グラフビルダーのすべての引数のJSON Schema。これらを支えるスキーマファイルは2つあります。[`schemas/javascript-capability-manifest-v1.schema.json`](../schemas/javascript-capability-manifest-v1.schema.json)はマニフェスト自体を、[`schemas/core-definition-v1.schema.json`](../schemas/core-definition-v1.schema.json)はシール済みのCore定義を記述します。後者は、生成された定義を制約付きでデコードするためのものです。テストでは、マニフェストをそのスキーマに対して、ゴールデンなIDコーパスの定義をCoreのスキーマに対して、それぞれ検証します。
