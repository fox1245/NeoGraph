<!-- neograph-i18n: source=docs/STRICT_RUNTIME_INTERPOSITION.md locale=ja source_sha256=59193d3d0f34fd9e49284edb43ce62f5ec0352c27fdcfce47bf1dceec7f9454a -->
# 厳密なランタイムインター ポジション

**Languages:** [English](STRICT_RUNTIME_INTERPOSITION.md) | [한국어](STRICT_RUNTIME_INTERPOSITION.ko.md) | [日本語](STRICT_RUNTIME_INTERPOSITION.ja.md) | [简体中文](STRICT_RUNTIME_INTERPOSITION.zh-CN.md)

NeoGraphの厳密なランタイムパスは、必須コンテキスト、ライフサイクルHooks、およびプロバイダーディスパッチの証跡をモデルの裁量から排除する。これは追加的なものである。従来の直接プロバイダー呼び出しは信頼された埋め込みのために依然として存在する一方、`StrictRuntimeProfile`が厳密なパスに必要な依存関係をアセンブルする。

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

成功した結果は正規化された完了のダイジェストをバインドします。ディスパッチ後の例外はリモートプロバイダーが動作したかどうかを証明できないため、コントローラーはリトライではなく`ReconciliationRequired`を記録します。SQLiteスキーマv3は結果を別々に保存し、各結果が再起動後も許可されたディスパッチ受領書を正確にバインドしていることを検証します。

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
