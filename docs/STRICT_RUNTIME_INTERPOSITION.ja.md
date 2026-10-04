<!-- neograph-i18n: source=docs/STRICT_RUNTIME_INTERPOSITION.md locale=ja source_sha256=9b872c2d049d049aa2c5e7393b485bc79c3b89a5268878b1fff5a263c347208f -->
# 厳密なランタイムインター ポジション

**Languages:** [English](STRICT_RUNTIME_INTERPOSITION.md) | [한국어](STRICT_RUNTIME_INTERPOSITION.ko.md) | [日本語](STRICT_RUNTIME_INTERPOSITION.ja.md) | [简体中文](STRICT_RUNTIME_INTERPOSITION.zh-CN.md)

NeoGraph の厳密なランタイムパスは、必須コンテキスト、ライフサイクル Hook、provider dispatch の証拠をモデルの裁量から分離します。信頼された埋め込みでは直接 typed provider 呼び出しを使え、`StrictRuntimeProfile` は厳密なパスの依存関係を組み立てます。

## 保証の境界

```text
durable RAW history + admitted artifacts + required Skills/constraints
  -> immutable ContextEpoch
  -> RuntimeTurnAssembler
  -> ContextAssemblyReceipt
  -> mandatory BeforeProviderRequest Hooks
  -> durable ProviderDispatchReceipt
  -> provider
  -> ProviderDispatchOutcomeReceipt
  -> mandatory AfterProviderResponse Hooks
```

この保証は、正確なコンテキスト構築、必須アーティファクトの存在、リクエストアイデンティティ、ディスパッチadmission、および既知・照合要求のあるプロバイダー成果をカバーする。LLMがすべてのトークンに注意を払ったことや従ったことを主張するものではない。

ホストが作成したカスタムネイティブノードは信頼されたコードのままである。そのようなノードに生の`Provider`を渡すことは意図的に厳密なプロファイルから外れるものとなり、生成されたトポロジーは登録されたノードのみを受け取り、その権限を発明することはできない。

## 厳密なプロファイル

`StrictRuntimeProfileConfig`は以下を要求する:

- プロバイダー;
- `DurableContextStore`;
- 末端の成果のサポートを備えた`DurableProviderDispatchReceiptStore`;
- `HookRuntime`;
- コンテンツアドレス方式のプロバイダーバインディングアイデンティティ;
- ゼロより大きい入力トークンの上限; および
- 省略可能な正確な必須コンテキストとSkillアーティファクトのアイデンティティ。

`RuntimeGuaranteeProfile::Strict`のエポックのみがアクティブ化され得る。プロファイルを`GraphEngine`にアタッチすると、ビルトインコンシューマーにプロバイダーインターセッションとライフサイクルHooksの両方がインストールされる。

## Providerの成果ライフサイクル

プロバイダ境界は現在、2つの独立した不変値を記録します：

1. `ProviderDispatchReceipt`はディスパッチ前に書き込まれる。
2. `ProviderDispatchOutcomeReceipt`は試行後に`Succeeded`、`Failed`、または`ReconciliationRequired`を記録します。

成功した結果は SDK outcome 全体の観測の digest を結び付けます。未送信が立証された typed Failure は `Failed`、不確実な配信は `ReconciliationRequired` を記録します。dispatch 後の例外だけではリモート provider の実行を判断できないため、暗黙に再試行しません。SQLite schema v3 は terminal receipt を別に保存し、再起動後に正確な admitted dispatch binding を検査します。Receipt digest は証拠であり、native continuation custody や実行可能な保存 outcome ではありません。

コントローラーは `ProviderRequest` を受け取り、不変の所有 `sp::runtime::Result` を返して、順序付き message/part と部分失敗の証拠を保持します。実際の結果の後で精算や receipt 永続化に失敗すると、`ProviderDispatchOutcomePersistenceError` は結果と元の cause を保持し、二次的 observer 失敗は `delivery_error()` に残ります。Token charge/reservation は nullable provider 使用量 report と別です。

## Program Core provider 呼び出し（独立した Strict Runtime とは別）

Program が組み込み Core LLM node を使う場合、ホストは
`RuntimeConfig::core_provider_call_resolver` と
`require_core_provider_call_broker = true` を設定できます。正確な
`ProgramCoreProviderCallContext` ごとに
`SQLiteProgramProviderCallJournal::bind(context, deployment_identity)` を返します。
ヘッダーは `<neograph/program/sqlite_provider_call_broker.h>`、リンク対象は
`neograph::program_sqlite` です。Deployment identity は実際の provider route、
model deployment、credential version を含む権限のホスト所有 SHA-256 identity
です。Broker はこれを `Provider` から推測しません。再起動/reconnect では
同じ durable database を再び結び付けます。

Journal のキーは owner、不変 Program version、run、operation、Core
thread/task/node、組み込み call ordinal です。Request 内容や Program attempt
はキーではありません。転送前に SQLite FULL 同期で marker を commit します。
Marker は転送が起きた可能性を示し、provider 受信や exactly-once 効果を
証明しません。SDK Completion/Failure outcome 全体を不変のまま encoding
version 2 で保存し、正確に結び付けた replay に使います。未送信が立証された
Failure は `Failed`、不確実な配信・例外・精算前の crash は reconciliation が
必要で、暗黙に再dispatchしません。状態は
`inspect(owner, logical_call_id(context, core_identity))` で確認します。
`reconcile_success` は独立に確認した provider-side 証拠と完全な Completion
outcome がある場合のみ使います。Streaming replay は captured outcome を返し、
stream event を作りません。

増やした output cap は新しい semantic call であり、同じ journal slot の transport retry や replay ではありません。Interface 4 は native replay configuration だけから cap を除き、prepared-request digest と保守的 resource claim には残します。承認する各 call に固有の決定的 ordinal を与え、全 attempt の outcome/accounting と元の deadline を保ち、同じ resource bank から admission を得ます。既存 slot の digest 変更は拒否されます。Native history や cursor は credit を更新せず、uncertain delivery や observer/settlement 失敗後の再送も許可しません。

順序付き message part、raw 観測、nullable 使用量、attempt metadata、
native continuation は保存結果に残ります。Native outcome には
`SQLiteProgramProviderCallJournal(database_path, native_archive)` に渡した
ホストの `sp::NativeArchive` が必要です。Portable JSON projection はその権限を
再作成できません。旧 lossy receipt は upgrade や暗黙の再dispatchではなく
拒否します。Journal は保守的 claim/committed token 量を provider report と
分けて保持します。Durable filesystem database path を使ってください。
空の path、`:memory:`、`file:` URI は拒否されます。

この broker は assembled `ContextEpoch` ではなく Core の既存 ReAct message
state を使います。同じ組み込み呼び出しで engine Strict Runtime interposition
と併用できません。ホストが書いた native Provider 呼び出しは範囲外です。


## 必須Hooks（native、stdio、またはHTTP）

`MandatoryHookRunner`は既存のネイティブアダプターまたはトランスポート非依存の`HookExecutionBackend`を受け入れます。`RpcHookExecutionAdapter`は`HookRpcExecutor`をそのバックエンドにバインドします。同じ固定`hooks/invoke` JSON-RPCメソッドは`StdioJsonRpcTransport`または`HttpJsonRpcTransport`を使用できます。

RPC Hook成果物は証拠であり、権限ではありません。`ContextStoreHookArtifactPublisher`は次の条件を満たす成果物のみを受け入れます：

- 種類は`HookOutput`であり、
- `source_digest`は正確なHook呼び出しIDと一致し、
- ランタイムイベントは呼び出しと一致します。

公開は所有者スコープかつ冪等です。外部効果が成功したが、その成果物を公開できない場合、Hookは`ReconciliationRequired`に解決されます。クリーンな成功として報告されることはありません。

## 必須コンテキストと変換

`RuntimeContextRequirements` は、すべての必須成果物IDを、`RequiredSkill` 成果物でなければならないサブセットから分離します。`HardConstraint` は専用の必須成果物種別です。すべての必須成果物は、アクティブなエポックによって選択され、`required=true` を保持しなければならず、必須トークン数に寄与します。

`ContextTransformReceipt` はv1では意図的に保守的です。トランスフォーマーは任意の証拠を置換または圧縮しても構いませんが、すべての必須入力成果物IDは出力セットにバイト単位で同一に出現しなければなりません。言い換えは制約維持の証明として受け入れられません。

## ランタイム開発者向け指示

`RuntimeDeveloperInstruction` は不変の開発者入力であり、権限ではありません。`RuntimeInstructionController::submit_and_plan` は次の順序で実行します:

```text
append Developer-trust history record
  -> load the exact active Program lineage/generation
  -> call the host planner
  -> validate decision against the current lineage head
  -> require an exact already-admitted target for transition decisions
  -> persist the required decision artifact
```

決定済みの事項は以下のとおりです。

- `SatisfiedInPlace`;
- `Rejected`;
- `ReplaceAtHandoff`、および
- `MigrateGraph`.

遷移を適用すると、既存の`ProgramRuntime::replace`または`migrate_graph`パスに委任する直前に、系統先頭が再チェックされます。古い決定が権威になることはありません。

## 境界付きProgram合成

`ProgramSynthesisGateway`は、ホスト所有の生成された後続パスを提供します。

```text
immutable ProgramSynthesisProposal
  -> durable host reservation receipt
  -> bounded QuickJS compilation
  -> proposal capability/effect closure check
  -> host-owned semantic contract validation
  -> ordinary ProgramCatalog admission
  -> immutable ProgramSynthesisReceipt
```

予約は、非再生可能な`max_dynamic_compiles`単位を正確に1つ減らすことを示し、他の予算を増やしてはなりません。予約はコンパイル前に行われるため、拒否されたソースはコンパイル単位を返却されません。意味検証は必須であり、コンパイル後かつadmissionリゾルバの前に実行されます。その不変のレシートは、提案、予約、コンパイル済みバンドル、バリデータID、意味コントラクトID、判定、エビデンスダイジェストを結び付けます。拒否された判定は型付きエビデンスを公開し、`ProgramVersion`を公開できません。ゲートウェイはその結果をアクティブ化、バインド、マイグレート、またはスパウンすることはありません。これらは、既存のProgram APIを通じた別個のホスト決定のままです。

ランタイム命令プランナーはゲートウェイを呼び出し、その後、交換またはマイグレーション決定で正確に承認されたバージョンを返すことができます。これにより以下が維持されます。

```text
proposal -> reserve -> compile -> semantic validate -> admit -> decide -> migrate/spawn
```

コンパイラ、Catalog、資格情報、またはアクティブ化権限を生成されたJavaScriptに公開することなく。
