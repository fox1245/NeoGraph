<!-- neograph-i18n: source=docs/STRICT_RUNTIME_INTERPOSITION.md locale=ja source_sha256=a6837ba97c87a975689aac8038004f408ccde630e28e01cd8cc5173ad84663ae -->
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

アクティブにできるのは、`RuntimeGuaranteeProfile::Strict` のエポックだけです。プロファイルを `GraphEngine` に接続すると、組み込みのコンシューマーに、プロバイダーのインターポジションとライフサイクルHookの両方が組み込まれます。ただし、プロバイダー呼び出しの間にコンテキストを進めることはしません。[ホストがステップを進める契約](#host-stepped-contract)を参照してください。

<a id="host-stepped-contract"></a>
## ホストがステップを進める契約（host-stepped）

strictの経路は、プロバイダーへのリクエストをすべて、アクティブなエポックから作ります。材料は、エポックに固定されたRAWの範囲と、選択されたアーティファクトです。グラフやエージェントが保持している会話は読みません。エポックのほかにプロンプトへ入るのは、組み込みノードが埋めるホスト指示と、信頼された補足入力（trusted supplemental input）のスロットだけです。そのため、ホストが記録するまで、NeoGraphはモデルの返答もツールの結果も知ることができません。契約は次のとおりです。

**ホストは、プロバイダーを呼び出すたびに、RAWレコードを追加し、新しいエポックをアクティブにします。**

2回の呼び出しの間に、ホストは直前のターン（モデルの返答、続いてツールの結果）を `ContextStore` に追加します。そのターンまでをRAWの範囲に含むエポックを作り、`StrictRuntimeProfile::activate` でアクティブにします。最初のエポックも、ユーザーのメッセージから同じ方法で作ります。次のコードが1ステップ分です。`head` は `store.history_head(feed)` で、`epoch_sequence` と `previous_epoch_id` は直前のエポックから取ります。

```cpp
for (auto& [message, trust] : turn) {  // the reply (ModelOutput), then the tool results (ToolOutput)
    RuntimeHistoryRecordData record;
    record.feed_id = feed.feed_id;
    record.sequence = ++head.sequence;
    record.message_id = "message-" + std::to_string(record.sequence);
    record.trust = trust;
    record.message = std::move(message);
    record.predecessor_id = head.record_id;
    const auto created = RuntimeHistoryRecord::create(std::move(record));
    if (store.append_history(feed, created, head.record_id) != ContextStoreAppendResult::Appended)
        throw std::runtime_error("the context feed moved");
    head.record_id = created.id();
}
ContextEpochData next;
next.run_id = run_id;
next.sequence = ++epoch_sequence;
next.predecessor_id = previous_epoch_id;
next.feed_id = feed.feed_id;
next.raw_from_sequence = 1;
next.raw_through_sequence = head.sequence;
next.raw_window_digest = store.snapshot_history(feed, 1, head.sequence).digest;
next.guarantee_profile = RuntimeGuaranteeProfile::Strict;
auto epoch = ContextEpoch::create(std::move(next));
previous_epoch_id = epoch.id();
profile.activate(owner_id, std::move(epoch));
```

NeoGraphには、1回の実行の途中でエポックを進めるヘルパーは、まだありません。

**1回の実行の中で回るループ。** プロファイルを素の `GraphEngine` に接続しても、コンテキストは進みません。`engine.run()` 1回の中でツールループ（LLM呼び出し、ツールのディスパッチ、LLM呼び出し。`create_react_graph` が作る構成）が回ると、2回目のLLM呼び出しも同じエポックで実行されます。ツールの結果はグラフの状態にはありますが、エポックにはありません。プロバイダーは最初のターンのコンテキストをもう一度受け取り、モデルはツールの結果を見ないまま、ステップ数の上限か予算で実行が止まるまで、同じツールを要求し続けるおそれがあります。

そこでNeoGraphは、その呼び出しを拒否します。グラフがすでに進めたターンのエポックに対してプロバイダー呼び出しが届くと、`std::logic_error` を投げます。呼び出し元が提示したメッセージに、エポックの最初のディスパッチ以降に追加されたアシスタントまたはツールのメッセージがあり、呼び出しがそのメッセージをホスト指示や信頼された補足入力としても運んでいない場合に、ターンが進んだとみなします。エラーメッセージは、この契約を明示します。

```text
Strict provider dispatch is host-stepped: the host appends RAW records and activates a new epoch for every provider call. This call presents assistant or tool messages added since the active epoch's first dispatch, so the graph or agent loop advanced inside one run and the provider would not receive them. Append those messages to the context feed and activate a new epoch before this call.
```

グラフでは、失敗したノードが `NodeExecutionError` として報告します。logic errorは再試行されないので、`attempts()` は1です。`set_runtime_interposition` でコントローラーを設定した `llm::Agent` も、2回目のプロバイダー呼び出しで同じように失敗します。

次のものは拒否しません。

- 同じターンの再試行。ノードの再試行や、プロバイダーの失敗後の再呼び出しは、同じメッセージをもう一度提示します。
- エポックの最初のディスパッチの時点で、すでにアシスタントまたはツールのメッセージを含んでいた会話。それらのメッセージが基準になります。
- ユーザーのメッセージだけが増えた場合。検査するのは、アシスタントとツールのメッセージです。
- 増えていく会話を信頼された補足入力として渡す組み込みノード（plan-executeグラフのexecutorノードなど）。返答とツールの結果はプロバイダーに届き、アセンブリのレシートに結び付けられます。

エポックをアクティブにするか `clear()` を呼ぶと、提示された内容はすべて忘れられます。コントローラーは、呼び出し箇所（ノード名）ごとに最初の提示を覚えますが、1つのエポックで保持する異なる提示の数には上限があります。上限を超えた提示は追跡しないので、その場合の検査はベストエフォートです。

1つの会話の実行は、ホスト側で直列化してください。プロファイルが持つアクティブなエポックは1つで、`thread_id` は見ません。[1つの会話の実行](concurrency.md#runs-of-one-conversation)を参照してください。

## コンテキストの管理方法を選ぶ

NeoGraphには、モデル呼び出しのコンテキストを管理する方法が4つあります。プロンプトを何が作るか、何を保証するかが、それぞれ異なります。

| 方法 | プロンプトの出どころ | 保証すること |
| --- | --- | --- |
| `messages` チャネル | ノードが残したままの、グラフの状態の `messages` チャネル。`LLMCallNode` がこれを送って返答を追記し、`ToolDispatchNode` がツールの結果を追記するので、1回の実行の中のツールループが動く | グラフの状態からリクエストを作ること、チェックポイントのストアがその状態を保存できること。呼び出しごとのレシート、必須アーティファクト、コンテキストの上限はない |
| `history::compact_history` | 独立した方法ではなく、より短いメッセージ一覧を返すヘルパー。`estimate_tokens` が上限を超えると、古いテキストだけの先頭部分をモデルが書いた要約に置き換え、最近の部分は残す。結果は別の方法の入力として使う。`RuntimeInterpositionController` を受け取るオーバーロードは、要約の呼び出しをそのコントローラー経由で送る | 履歴を残したか置き換えたかと、その理由を伝えるステータス。ネイティブのリプレイの封印を含む履歴は、そのまま返す。要約はモデルの出力なので、事実や制約が要約に残った証拠はない。見積もりはヒューリスティック。エポックもレシートもない |
| Recordedプロファイル | `RuntimeInterpositionController` を通るアクティブなエポック（`RuntimeGuaranteeProfile::Recorded`、デフォルト）。`ContextStore` のRAWの範囲と、エポックのアーティファクトで構成する。ホストがステップを進める | リクエストをそのエポックから正確に組み立ててアセンブリのレシートに結び付け、ディスパッチごとにレシートを残す。ストアが永続的である必要はなく、入力の上限を0にできるので、エビデンスをどれだけ保持するかと、上限を置くかどうかは、ホストが決める |
| Strictプロファイル | Recordedプロファイルと同じで、`StrictRuntimeProfile` を通る。ホストがステップを進める | Recordedプロファイルの保証すべてに加えて、永続的なコンテキストとディスパッチのストア、呼び出しごとの必須のHook、エポックに必ず含まれていなければならない必須アーティファクト、予算を超えるターンをディスパッチ前に止める0でない入力の上限。モデルがコンテキストに注意を払った、または従ったという保証はなく、エポックを代わりに進めることもしない |

`messages` チャネルからエポックに移ると、コンテキストの持ち主が変わります。グラフの状態はプロンプトの元ではなくなり、RAWの履歴はホストが保持します。

## コンテキストの予算

strictの経路は、組み立てたリクエストごとに入力の上限（`max_input_tokens`）と比べ、予算を超えるターンをディスパッチの前に `ContextBudgetBlocked` で止めます。アセンブリのレシートには、使った見積もり（`estimated_input_tokens`）と、そのうち必須アーティファクトのテキストが占める分（`mandatory_input_tokens`）が記録されます。

見積もりは `RuntimeTurnAssembler::estimate_input_tokens` です。エンコードしたリクエスト本文のバイト長を3で割り、切り上げた値です。ヒューリスティックであり、証明された上限ではありません。どのプロバイダーのトークナイザーからも導いていないので、プロバイダーが数える値より小さいことも大きいこともあります。見えているテキストだけでなく、エンコードしたリクエスト全体を数えます。ツールのスキーマ、フィールド名、プロバイダー固有のフレーミングも、バイト数に入ります。`max_input_tokens` は、大きすぎるリクエストを防ぐための安全装置として扱ってください。プロバイダーが報告する値や請求する値についての約束ではありません。`mandatory_input_tokens` は、必須アーティファクトのテキストだけに同じ比率を適用します。

ディスパッチのトークン予約は、別のリソース契約です。実行にモデルトークンの予算がある場合、ディスパッチごとに、モデルの入力上限に出力の許容量を足した分を、行いうる試行の回数だけ予約します。この見積もりは使いません。

## プロバイダーの結果のライフサイクル

プロバイダーの境界では、変更できない2つの値を、別々に記録します。

1. `ProviderDispatchReceipt` は、ディスパッチの前に書き込まれます。
2. `ProviderDispatchOutcomeReceipt` は、試行の後に、`Succeeded`、`Failed`、`ReconciliationRequired` のいずれかを記録します。

成功した結果には、SDKの結果の観測全体のダイジェストが結び付けられます。受け付けられていないことが証明された型付きのFailure（リクエストが送信されなかった場合と、完了したHTTP 4xx応答が拒否した場合）は `Failed` を記録します。配送が不確かな場合は `ReconciliationRequired` を記録します。ディスパッチの後に例外が起きても、リモートのプロバイダーが動作したかどうかは分からないため、コントローラーは黙ってリトライすることはありません。SQLiteのスキーマv3は、最終レシートを別に保存し、再起動後に、admission済みのディスパッチとの正確な結び付きを検査します。レシートのダイジェストはエビデンスです。ネイティブな継続の保管でも、実行可能な保存済みの結果でもありません。

コントローラーは `ProviderRequest` を受け取り、変更できない、所有権付きの `sp::runtime::Result` を返します。順序付きのメッセージとパート、部分的な失敗のエビデンスも保持されます。本物の結果が得られた後で、精算やレシートの永続化が失敗した場合、`ProviderDispatchOutcomePersistenceError` は、その結果と元の原因を保持します。二次的なオブザーバーの失敗は `delivery_error()` に残ります。トークンの課金や予約は、null許容のプロバイダーの使用量レポートとは別のものです。

## ProgramのCoreのプロバイダー呼び出し（単独のStrictランタイムとは別）

Programが組み込みのCoreのLLMノードを使う場合、ホストは `RuntimeConfig::core_provider_call_resolver` と `require_core_provider_call_broker = true` を設定できます。`ProgramCoreProviderCallContext` ごとに、`<neograph/program/sqlite_provider_call_broker.h>` の `SQLiteProgramProviderCallJournal::bind(context, deployment_identity)` を返します。リンクするのは `neograph::program_sqlite` です。デプロイメントIDは、実際のプロバイダーのルート、モデルのデプロイメント、権限（認証情報のバージョンを含む）を表す、ホストが所有するSHA-256のIDでなければなりません。ブローカーは、これらを `Provider` オブジェクトから見つけ出すことはしません。再起動や再接続のときは、同じ永続データベースを再度バインドしてください。`RuntimeConfig::core_provider_calls_journaled = true` も設定してください。フォークはこのジャーナルからプロバイダー呼び出しをリプレイするので、このフラグを設定すれば、このホストでのフォークが `ProgramHandle::notices()` に `P_FORK_PROVIDER_CALLS_NOT_JOURNALED` を出すことはありません。ソースの同じ位置の呼び出しを見つけても、それを引き継ぐことも、呼び出しが異なることを示すこともできないフォークは、`RuntimeConfig::fork_may_resend_provider_calls` が true でない限り拒否されます（リファレンスを参照）。

ジャーナルは、各呼び出しを、オーナー、変更できないProgramバージョン、実行、オペレーション、Coreのスレッド／タスク／ノード、組み込みの呼び出し序数で識別します。Programの試行では識別**しません**。キーの横に、呼び出しのリクエスト内容のダイジェストとdescriptor policyのidentityを記録し、呼び出しをその内容で照合する（#543）ので、同じリクエストは、SchemaProviderのdescriptor policyのidentityが別でも同じ呼び出しです。マーカーは、トランスポートの前に、SQLiteのFULL同期でコミットされます。このマーカーが意味するのは、トランスポートが行われた*可能性がある*ことです。プロバイダーが受け取ったことや、効果がちょうど1回起きたことを意味しません。変更できないSDKのCompletionまたはFailureの結果は、そのままエンコードバージョン2で保存され、正確に結び付いたリプレイに使われます。受け付けられていないことが証明されたFailure（送信されなかった場合と、完了したHTTP 4xx応答が拒否した場合）は `Failed` を記録します。配送が不確かな場合、例外が起きた場合、精算の前にクラッシュした場合は、照合が必要で、黙って再ディスパッチされることはありません。状態を確認するには `inspect(owner, logical_call_id(context, core_identity))` を使います。`reconcile_success` を使うのは、プロバイダー側の独立して検証したエビデンスと、完全なCompletionの結果がある場合だけです。リプレイされたストリーミングの呼び出しは、取得済みの結果を返し、ストリームのイベントを合成することはありません。

出力の上限を引き上げた呼び出しは、新しいセマンティックな呼び出しです。トランスポートのリトライでも、同じジャーナルのスロットのリプレイでもありません。ネイティブリプレイの設定からは、この上限が除かれます。準備済みリクエストのダイジェストと、保守的に見積もったリソースの請求枠には、引き続きこの上限が含まれます。admission済みの呼び出しごとに、別々の決定的な序数を与え、すべての試行の結果と会計、そして元のデッドラインを保持し、同じリソースバンクからadmissionを受けてください。既存のスロットでリクエスト内容のダイジェストが変わると、拒否されます。ネイティブ履歴もカーソルも、クレジットを更新しません。配送が不確かな場合や、オブザーバーまたは精算が失敗した場合に、もう一度送信してよいという許可にもなりません。

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

## アーティファクトの配置

アーティファクトの `placement` は、そのテキストをRAWメッセージのどこに入れるかを決めます。アセンブラーは、次の順序でプロンプトを組み立てます。同じ配置の中では、`priority` が高いものが先で、次に `producer_id`、`id` の順です。

```text
host instructions
BeforeHistory artifacts
RAW messages before the latest user message
BeforeLatestUser artifacts
latest user message
AfterLatestUser artifacts
RAW messages after the latest user message
AfterHistory artifacts
trusted supplemental input
```

ターンが変わっても変わらないアーティファクトは、`BeforeHistory` に置いてください。デフォルトの配置は `BeforeLatestUser` なので、配置を指定せずに作ったアーティファクトは、ターンごとに動きます。

- `BeforeLatestUser` と `AfterLatestUser` は、最新のユーザーメッセージに固定されます。新しいユーザーメッセージが来ると、アーティファクトも一緒に動き、プロンプトは、アーティファクトの元の位置から先が前のプロンプトと異なります。この2つの配置には、RAWウィンドウにユーザーメッセージが必要です。なければ、アセンブリは失敗します。
- `AfterHistory` は、常に最後です。手前の履歴は伸びていきますが、それより前のプロンプトの部分は変わりません。
- `BeforeHistory` は、ホスト指示の直後に来て、履歴に依存しません。アーティファクトの集合と優先度が同じなら、テキストも位置も同じです。

アーティファクトの集合が変わるか、RAWウィンドウが最も古いレコードを手放すと、その箇所から先のプロンプトが変わります。プロンプトの先頭部分をキャッシュするプロバイダーは、先頭のバイトから前のリクエストと同一の部分だけを再利用できます。配置がキャッシュヒットに与える影響は、NeoGraphではまだ測定していません。

`RequiredSkill` と `HardConstraint` のアーティファクトはSystemメッセージとして、ほかの種類はUserメッセージとして描画されます。Systemメッセージの扱いは、プロバイダーのファミリーによって異なります。

- `anthropic.messages`、`google.generate`、`google.interactions` では、リクエストビルダーが、すべてのSystemメッセージを、配置にかかわらず最上位のsystemフィールドに移します（空行で区切り、組み立てた順）。そのため、Skillや制約の配置が決めるのは、会話の中での位置ではなく、systemテキスト同士の順序だけです。
- `openai.chat` と `openai.responses` では、Systemメッセージは、配置が決めた場所にそのまま残ります。

リクエストの先頭を安定させたいときは、System役割のアーティファクトのテキストを、ターンの間で変えないでください。ここまではレガシーのレイアウトの説明で、エポックが別のレイアウトを求めない限りこれが使われます（「安定性優先のレイアウト」を参照）。

## 安定性クラス、順序、プレフィックス宣言

プロバイダーのプロンプトキャッシュがリクエストの先頭を保てるかどうかは、呼び出しの間にどのアーティファクトが変わらないかに左右されます。データモデルはそれを記録し、安定性優先のレイアウト（次の節）がそれを使います。

- アーティファクトは `StabilityClass` を宣言できます。`Stable`（実行の間ずっと同じバイト列）、`Slow`（圧縮など、宣言されたプレフィックスの断絶のときだけ変わる）、`Volatile`（呼び出しごとに変わりうる）です。宣言しないアーティファクトは、読み込むときにクラスが決まります。`RequiredSkill` と `HardConstraint` は `Stable`、プロデューサーが `COMPACTION_PRODUCER_ID` の `DerivedContext` は `Slow`、それ以外はすべて `Volatile` です。System メッセージとして描画される種類の `Volatile` なアーティファクトを作ると、`ContextStabilityError` で拒否されます。
- アーティファクトは `AnchorMode` を宣言できます。`Replace`（既定）または `KeepInPlace` です。これを使うものはまだありません。
- エポックは、アーティファクトを固定された順序で並べた `ordered_artifact_ids` と `prefix_claim` を記録できます。`prefix_claim` は、`Extends`（リクエストの先頭は直前のエポックのものと同じで、末尾でだけ伸びる）か、`break_reason` を伴う `Breaks` です。`Extends` を宣言したエポックをアクティブにすると、アクティブなエポック、またはホストが `activate` に渡した直前のエポックに対して宣言が検査されます。条件は、同じ `raw_from_sequence`、より大きい `raw_through_sequence`、同じ順序の同じ `Stable` と `Slow` のアーティファクト、そしてまだ選択されていて RAW ウィンドウ内にある `KeepInPlace` のアーティファクトです。順序を記録していないエポック（このフィールドができる前に保存されたものなど）は、引き継げません。`Extends` が約束するものはレイアウトによって異なります。`Legacy` では、`Stable` と `Slow` のアーティファクトと記録された順序が変わらず、ウィンドウが伸びただけだという意味で、レガシーのアセンブラは配置、優先度、プロデューサーの順に並べ、記録された順序を読まないので、リクエストの先頭までは約束しません。`StableFirst` では、記録された順序がそのままリクエストの順序で、エポックのアーティファクトがアセンブラの受け付けるレイアウトでなければ宣言は拒否され、`Slow` のアーティファクトが終わるまでのリクエストの先頭は直前のエポックのものと同じです（ホストの指示とツールは、エポックが所有しないので除きます）。
- アセンブラが書くレシートは、プロンプトの順序でのアーティファクト、そのクラス（それぞれアーティファクトが宣言したものか導出したものか、および導出規則のバージョンとともに）、エンコーダーのファミリー、ツールのダイジェスト、エポックの断絶の理由を記録します。JSON でない本文は、ツールのないリクエストではなくエラーです。保存されたレシートは、クラスが宣言されているところでは厳密に照合し、現行の規則のバージョンのときだけクラスを導出し直して比べるので、後で規則が変わっても、保存済みのレシートは読めるままです。

## 安定性優先のレイアウト

エポックは `assembly_layout` で、アーティファクトをどう並べるかを選びます。`Legacy` は既定で、このフィールドができる前に保存されたエポックが持てる唯一のレイアウトであり、「アーティファクトの配置」で説明したアセンブリです。`StableFirst` は明示的に選ぶもので、`ordered_artifact_ids` が必要で、プロンプトを次の順に組み立てます。

```text
host instructions
Stable artifacts, in the recorded order
Slow artifacts, in the recorded order
RAW messages
Volatile artifacts, in the recorded order
trusted supplemental input
```

- **エポックのリストが順序です。** `order_context_artifacts` がアーティファクトからこのリストを作ります。安定性クラス、次に高い `priority`、次に `producer_id`、次にホストが与えるキーの順です。内容のハッシュは関与しないので、テキストが変わる兄弟のアーティファクトも位置を保ち、4 つがすべて同じ 2 つのアーティファクトはあいまいとして拒否されます。
- **配置はクラスに従います。** `Stable` と `Slow` のアーティファクトは `BeforeHistory` に、`Volatile` のものは `AfterHistory` に置き、リストは `Stable`、`Slow`、`Volatile` の順に並べます。それ以外（たとえば既定の配置 `BeforeLatestUser` で作ったアーティファクト）は、移されることなく `ContextStabilityError` で拒否されます。 アセンブラが検査するクラスは、アーティファクトが宣言したクラスで、宣言がなければ組み立てるビルドの `derive_stability_class` が返すクラスです。このレイアウトのエポックを保存するホストは、すべてのアーティファクトのクラスを宣言してください。そうしないと、導出規則が後で変わったときに、アセンブラが保存済みのエポックを拒否することがあります。
- **System テキストは先頭に留まります。** System メッセージになれるのは、ホストの指示と、`Stable` または `Slow` のアーティファクトだけです。RAW や補助入力の System メッセージは、`anthropic.messages`、`google.generate`、`google.interactions` で system フィールドに移されてリクエストの先頭を動かすため、拒否されます。移されるテキストは `Stable`、`Slow` の順です。
- **レイアウトの変更は断絶です。** 連続する 2 つのエポックでレイアウトが異なるには、後のエポックが理由 `LayoutChange` の `Breaks` を宣言しなければなりません。そうでなければ `verify_context_epoch_claim` が拒否し、レイアウトをまたぐ `Extends` の宣言も拒否します。
- **本文から読み取る 2 つの境界。** レシートは、`Stable` の部分（ホストの指示と `Stable` のアーティファクト）が終わる位置、追記だけの部分（`Volatile` のアーティファクトより前のすべて）が終わる位置、そして後者までのリクエストのダイジェストを記録します。境界は、エンコードされた本文でプレフィックスに入らない最初のメッセージとパートなので、System テキストを移すファミリーでの `{0, 0}` は、ツールと system フィールドだけのプレフィックスです。位置は、System テキストを移し、ツール結果をまとめ直した後の本文のものです。アセンブラは、プレフィックスのメッセージとその後ろの目印のメッセージだけでリクエストを 2 回、3 回とエンコードして位置を見つけ、エンコードされた本文が食い違えば `ContextStabilityError` でエポックを拒否します。`Slow` の System テキストが `Stable` のテキストと system フィールドを共有するときは、フィールドが 1 つのブロックなので、`Stable` の境界は記録されません。ダイジェストは、ストリームのフラグと出力の上限を除く本文のすべてのメンバーを含み、メッセージは境界で切られます。この 2 つは、ファミリーのディスクリプタが本文の最上位で名前を指定するメンバーで、`google.generate` は出力の上限を `generationConfig` の中に置くため、ダイジェストはそれを覆います。レガシーのエポックには境界がありません。

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
