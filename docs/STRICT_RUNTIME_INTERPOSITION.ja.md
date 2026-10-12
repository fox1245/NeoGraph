<!-- neograph-i18n: source=docs/STRICT_RUNTIME_INTERPOSITION.md locale=ja source_sha256=eee79303f2ce57761c1bf62d99755e7589782385c4ffa411c4d5c6ad4ad55207 -->
# Strictランタイムインターポジション

**Languages:** [English](STRICT_RUNTIME_INTERPOSITION.md) | [한국어](STRICT_RUNTIME_INTERPOSITION.ko.md) | [日本語](STRICT_RUNTIME_INTERPOSITION.ja.md) | [简体中文](STRICT_RUNTIME_INTERPOSITION.zh-CN.md)

NeoGraphのstrictランタイムの経路では、必須のコンテキスト、ライフサイクルHook、プロバイダーのディスパッチのエビデンスが、モデルの裁量から外れます。信頼できる組み込みでは、型付きのプロバイダーを直接呼び出すこともできます。`StrictRuntimeProfile` は、strictの経路に必要な依存をまとめて組み立てます。

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

保証の対象は、コンテキストの正確な構築、必須アーティファクトの存在、リクエストのID、ディスパッチのadmission、結果が確定しているか照合が必要かというプロバイダーの結果の区別です。LLMがすべてのトークンに注意を払ったことや、従ったことまでは保証しません。

ホストが書いたカスタムのネイティブノードは、引き続き信頼されるコードです。そのようなノードに生の `Provider` を渡すと、意図的にstrictプロファイルの外に出ます。生成されたトポロジーが受け取れるのは登録済みのノードだけで、その権限を作り出すことはできません。

## Strictプロファイル

`StrictRuntimeProfileConfig` には、次のものが必要です。

- プロバイダー
- `DurableContextStore`
- 最終結果の記録に対応した `DurableProviderDispatchReceiptStore`
- `HookRuntime`
- 内容でアドレス指定されたプロバイダーのバインディングID
- 0でない入力トークンの上限
- 必須のコンテキストとSkillのアーティファクトを正確に指定するID（省略可能）

アクティブにできるのは、`RuntimeGuaranteeProfile::Strict` のエポックだけです。プロファイルを `GraphEngine` に接続すると、組み込みのコンシューマーに、プロバイダーのインターポジションとライフサイクルHookの両方が組み込まれます。

## プロバイダーの結果のライフサイクル

プロバイダーの境界では、変更できない2つの値を、別々に記録するようになりました。

1. `ProviderDispatchReceipt` は、ディスパッチの前に書き込まれます。
2. `ProviderDispatchOutcomeReceipt` は、試行の後に、`Succeeded`、`Failed`、`ReconciliationRequired` のいずれかを記録します。

成功した結果には、SDKの結果の観測全体のダイジェストが結び付けられます。送信されていないことが証明された型付きのFailureは `Failed` を記録します。配送が不確かな場合は `ReconciliationRequired` を記録します。ディスパッチの後に例外が起きても、リモートのプロバイダーが動作したかどうかは分からないため、コントローラーは黙ってリトライすることはありません。SQLiteのスキーマv3は、最終レシートを別に保存し、再起動後に、admission済みのディスパッチとの正確な結び付きを検査します。レシートのダイジェストはエビデンスです。ネイティブな継続の保管でも、実行可能な保存済みの結果でもありません。

コントローラーは `ProviderRequest` を受け取り、変更できない、所有権付きの `sp::runtime::Result` を返します。順序付きのメッセージとパート、部分的な失敗のエビデンスも保持されます。本物の結果が得られた後で、精算やレシートの永続化が失敗した場合、`ProviderDispatchOutcomePersistenceError` は、その結果と元の原因を保持します。二次的なオブザーバーの失敗は `delivery_error()` に残ります。トークンの課金や予約は、null許容のプロバイダーの使用量レポートとは別のものです。

## ProgramのCoreのプロバイダー呼び出し（単独のStrictランタイムとは別）

Programが組み込みのCoreのLLMノードを使う場合、ホストは `RuntimeConfig::core_provider_call_resolver` と `require_core_provider_call_broker = true` を設定できます。`ProgramCoreProviderCallContext` ごとに、`<neograph/program/sqlite_provider_call_broker.h>` の `SQLiteProgramProviderCallJournal::bind(context, deployment_identity)` を返します。リンクするのは `neograph::program_sqlite` です。デプロイメントIDは、実際のプロバイダーのルート、モデルのデプロイメント、権限（認証情報のバージョンを含む）を表す、ホストが所有するSHA-256のIDでなければなりません。ブローカーは、これらを `Provider` オブジェクトから見つけ出すことはしません。再起動や再接続のときは、同じ永続データベースを再度バインドしてください。

ジャーナルは、各呼び出しを、オーナー、変更できないProgramバージョン、実行、オペレーション、Coreのスレッド／タスク／ノード、組み込みの呼び出し序数で識別します。リクエストの内容やProgramの試行では識別**しません**。マーカーは、トランスポートの前に、SQLiteのFULL同期でコミットされます。このマーカーが意味するのは、トランスポートが行われた*可能性がある*ことです。プロバイダーが受け取ったことや、効果がちょうど1回起きたことを意味しません。変更できないSDKのCompletionまたはFailureの結果は、そのままエンコードバージョン2で保存され、正確に結び付いたリプレイに使われます。送信されていないことが証明されたFailureは `Failed` を記録します。配送が不確かな場合、例外が起きた場合、精算の前にクラッシュした場合は、照合が必要で、黙って再ディスパッチされることはありません。状態を確認するには `inspect(owner, logical_call_id(context, core_identity))` を使います。`reconcile_success` を使うのは、プロバイダー側の独立して検証したエビデンスと、完全なCompletionの結果がある場合だけです。リプレイされたストリーミングの呼び出しは、取得済みの結果を返し、ストリームのイベントを合成することはありません。

出力の上限を引き上げた呼び出しは、新しいセマンティックな呼び出しです。トランスポートのリトライでも、同じジャーナルのスロットのリプレイでもありません。インターフェース4で、この上限がネイティブリプレイの設定から外れるだけです。準備済みリクエストのダイジェストと、保守的に見積もったリソースの請求枠には、引き続きこの上限が含まれます。admission済みの呼び出しごとに、別々の決定的な序数を与え、すべての試行の結果と会計、そして元のデッドラインを保持し、同じリソースバンクからadmissionを受けてください。既存のスロットでダイジェストが変わると、拒否されます。ネイティブ履歴もカーソルも、クレジットを更新しません。配送が不確かな場合や、オブザーバーまたは精算が失敗した場合に、もう一度送信してよいという許可にもなりません。

順序付きのメッセージのパート、rawの観測、null許容の使用量、試行のメタデータ、ネイティブな継続は、保存される結果の一部のままです。ネイティブな結果には、ホストの `sp::NativeArchive` が必要で、`SQLiteProgramProviderCallJournal(database_path, native_archive)` に渡します。ポータブルなJSONの射影から、その権限を再作成することはできません。情報が失われる従来形式のレシートは、アップグレードされることも、黙って再ディスパッチされることもなく、拒否されます。ジャーナルは、保守的に見積もった請求枠とコミットしたトークン量を、プロバイダーのレポートとは別に保持します。データベースのパスには、永続化できるファイルシステム上のものを使ってください。空のパス、`:memory:`、`file:` URIのパスは、拒否されます。このブローカーが扱うのは、CoreにすでにあるReActのメッセージ状態であり、組み立てられた `ContextEpoch` ではありません。同じ組み込みの呼び出しに対して、エンジンのStrictランタイムのインターポジションと併用することはできません。ホストが書いたネイティブなProviderの呼び出しは、対象外です。

## ネイティブ、stdio、HTTPで動く必須のHook

`MandatoryHookRunner` は、既存のネイティブアダプターと、トランスポートに依存しない `HookExecutionBackend` のどちらも受け付けます。`RpcHookExecutionAdapter` は、`HookRpcExecutor` をそのバックエンドに結び付けます。固定のJSON-RPCメソッド `hooks/invoke` は、`StdioJsonRpcTransport` でも `HttpJsonRpcTransport` でも使えます。どちらも `MCPClient` のセッション上で動くため、エンドポイントは、`hooks/invoke` も提供するMCPサーバーです。このメソッドはNeoGraphの拡張で、MCPの一部ではありません。これを持たないMCPサーバーは、`-32601` を返します。

RPC Hookのアーティファクトは、エビデンスであり、権限ではありません。`ContextStoreHookArtifactPublisher` が受け付けるのは、次の条件を満たすアーティファクトだけです。

- 種類が `HookOutput` であること
- `source_digest` が、そのHookの呼び出しIDと正確に一致すること
- ランタイムイベントが、その呼び出しに一致すること

公開は、オーナー単位で、冪等です。外部への効果が成功したのに、そのアーティファクトを公開できなかった場合、Hookは `ReconciliationRequired` で精算されます。問題のない成功として報告されることはありません。

## 必須のコンテキストと変換

`RuntimeContextRequirements` は、必須のアーティファクトIDすべてと、そのうち `RequiredSkill` のアーティファクトでなければならないサブセットを、区別して保持します。`HardConstraint` は、専用の必須アーティファクトの種類です。必須のアーティファクトはすべて、アクティブなエポックに選択されていなければならず、`required=true` を保っていなければなりません。また、必須のトークン数に計上されます。

`ContextTransformReceipt` は、v1では意図的に保守的になっています。トランスフォーマーは、省略可能なエビデンスを置き換えたり圧縮したりできます。ただし、必須の入力アーティファクトのIDは、すべて、出力の集合にバイト単位で同一のまま現れなければなりません。言い換えは、制約が保たれたことの証明として受け付けられません。

## 実行時のデベロッパー指示

`RuntimeDeveloperInstruction` は、変更できないデベロッパーの入力であり、権限ではありません。`RuntimeInstructionController::submit_and_plan` は、次の順序で処理します。

```text
append Developer-trust history record
  -> load the exact active Program lineage/generation
  -> call the host planner
  -> validate decision against the current lineage head
  -> require an exact already-admitted target for transition decisions
  -> persist the required decision artifact
```

決定の種類は閉じており、次のとおりです。

- `SatisfiedInPlace`
- `Rejected`
- `ReplaceAtHandoff`
- `MigrateGraph`

遷移を適用するときは、既存の `ProgramRuntime::replace` または `migrate_graph` の経路に処理を渡す直前に、系統のヘッドを再確認します。古くなった決定が、権限になることはありません。

## 上限付きのProgramの合成

`ProgramSynthesisGateway` は、ホストが所有する、生成された後継を扱う経路を提供します。

```text
immutable ProgramSynthesisProposal
  -> durable host reservation receipt
  -> bounded QuickJS compilation
  -> proposal capability/effect closure check
  -> host-owned semantic contract validation
  -> ordinary ProgramCatalog admission
  -> immutable ProgramSynthesisReceipt
```

予約では、更新できない `max_dynamic_compiles` が正確に1単位だけ減っていなければならず、他の予算が増えてはいけません。予約はコンパイルの前に行われるので、拒否されたソースに、コンパイルの1単位が戻ることはありません。意味検証は必須で、コンパイルの後、admissionのリゾルバーの前に実行されます。その変更できないレシートは、提案、予約、コンパイル済みのバンドル、バリデーターのID、セマンティックな契約のID、判定、エビデンスのダイジェストを結び付けます。却下の判定は、型付きのエビデンスを公開し、`ProgramVersion` を公開することはできません。ゲートウェイが、結果をアクティブにしたり、バインドしたり、マイグレーションしたり、spawnしたりすることはありません。これらは、既存のProgram APIを通じた、ホストの別々の決定のままです。

実行時の指示のプランナーは、ゲートウェイを呼び出し、admission済みの正確なバージョンを、置き換えまたはマイグレーションの決定として返せます。これにより、次の分離が保たれます。

```text
proposal -> reserve -> compile -> semantic validate -> admit -> decide -> migrate/spawn
```

生成されたJavaScriptに、コンパイラー、Catalog、認証情報、アクティベーションの権限を公開することはありません。
