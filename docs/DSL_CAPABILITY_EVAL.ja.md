<!-- neograph-i18n: source=docs/DSL_CAPABILITY_EVAL.md locale=ja source_sha256=8918e3e19d555bc582f3c36b4a9bfa63dd9b80ea655074a7214a5c5b233a5c27 -->
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

## DeepSeekで観測した結果

実モデルでの実行で、モデルは、11のcapabilityグループすべてについて、プローブで検証されたソースを生成しました。静的なHITLとリトライでは、診断に導かれた修正が1回必要でした。構造化された並行性では、修正が2回必要でした。最初はESモジュールのexportを復元するため、次に誤ったCoreのバインディングを、admission済みの正確な名前に置き換えるためです。制御フローは、正確なバインディングへの修正を1回行って合格しました。mapは、ホストがネイティブAPIのマニフェストを与え、admission済みのCoreの識別子を曖昧さなく明示した後で、1回で合格しました。

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

これは、できることの証明であり、信頼性についての統計的な主張ではありません。ケースごとの、1回で成功する割合と、修正して成功する割合には、引き続き、繰り返しの試行が必要です。その際、プロバイダーの失敗は別に報告します。

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

モデルは、ESモジュールとCommonJS、グラフ名とノード名、要求されたCoreのIDと `core` のような一般的な語を、繰り返し取り違えました。そのため、ホストは、正確なシグネチャと、admission済みの識別子を注入し、可能な限り、モジュールの骨組みを固定のまま保ち、それらしく見えるソースを、要求したトポロジーが構築されたことのエビデンスとして扱ってはいけません。

NeoGraphは、この境界を `ProgramSynthesisGateway` で強制します。すべてのゲートウェイの構成は、ホストが所有する意味検証を提供しなければなりません。検証に成功すると、Catalogのadmissionの前に、内容でアドレス指定されたレシートが生成されます。却下の判定では、`ProgramSynthesisValidationError` が送出され、正確なエビデンスが保持され、admissionのリゾルバーが呼び出されることはありません。すでに消費された動的コンパイルの予約は、消費されたままです。

`javascript_authoring_capability_manifest()` は、インストールされているグラフビルダーとコマンドの語彙、正確なシグネチャ、分類、上限、プロファイルの制約を、機械可読なデータとして公開します。適合性テストは、このマニフェストを、2つのQuickJSコンテキストに実際にインストールされているプロパティと比較します。そのため、APIがずれるとテストスイートが失敗します。

マニフェストは、さらに、次のものも公開します。オーサリングの機能ごとに1つの真偽値フラグを持つ `features`（たとえば `graph.metadata`、`barrier.any_of`、`failure_edges`）。すべてのメソッドのTypeScript用の `dts` シグネチャ（共有する宣言は `dts_prelude` にあります）。グラフビルダーのすべての引数のJSON Schema。これらを支えるスキーマファイルは2つあります。[`schemas/javascript-capability-manifest-v1.schema.json`](../schemas/javascript-capability-manifest-v1.schema.json)はマニフェスト自体を、[`schemas/core-definition-v1.schema.json`](../schemas/core-definition-v1.schema.json)はシール済みのCore定義を記述します。後者は、生成された定義を制約付きでデコードするためのものです。テストでは、マニフェストをそのスキーマに対して、ゴールデンなIDコーパスの定義をCoreのスキーマに対して、それぞれ検証します。
