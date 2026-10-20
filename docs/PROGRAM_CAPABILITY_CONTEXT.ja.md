<!-- neograph-i18n: source=docs/PROGRAM_CAPABILITY_CONTEXT.md locale=ja source_sha256=05fbc110d09704e01a8f8dd64767d56b589b973a31fb3fbda55f9077f2686930 -->
# Programのノードのcapabilityと、仲介される効果

**Languages:** [English](PROGRAM_CAPABILITY_CONTEXT.md) | [한국어](PROGRAM_CAPABILITY_CONTEXT.ko.md) | [日本語](PROGRAM_CAPABILITY_CONTEXT.ja.md) | [简体中文](PROGRAM_CAPABILITY_CONTEXT.zh-CN.md)

`ProgramCatalog` は、実行可能なものの全体のクロージャーを1回だけバインドします。しかし、ノードファクトリーから見えるのは、そのノードのadmission済みの設定が宣言したcapabilityだけであるべきです。そのため、`RegistrySnapshotBuilder` は、ノードのマニフェストが直接要求する実行可能なものと、その設定ごとの要件リゾルバーを使って、構築時の `NodeContext` を絞り込みます。別のノードが使うバインド済みのProviderやToolは、このノードに暗黙に与えられるcapabilityではありません。

| 登録されたノード | Providerポインター | Toolポインター | 安全なメタデータ |
| --- | --- | --- | --- |
| 任意の `Brokered` のC++ファクトリー | なし | なし | 正確な `provider_name` と `tool_definitions` |
| ホストがレビューした `add_host_brokered_node` | 宣言されたProviderそのもの | 宣言されたToolそのもの | 正確なメタデータ |
| 固定の `add_core_llm_call` | 宣言されたProviderそのもの | モデルの定義用に、宣言されたToolそのもの | 正確なメタデータ |
| 固定の `add_core_tool_dispatch` | なし | 宣言されたToolそのもの | 正確なメタデータ |
| 固定の `add_core_endpoint_node` | 宣言されたProviderそのもの | なし | 正確なメタデータ |
| `TrustedNative` のC++ファクトリー | 宣言されたProviderそのもの | 宣言されたToolそのもの | 正確なメタデータ |

固定のCore登録は、NeoGraph自身の `LLMCallNode`、`ToolDispatchNode`、SchemaProviderのエンドポイントノード（`add_core_endpoint_node`。そのマニフェストがノードタイプを指定します：`embed`、`image_generate`、...、`job_collect`、#531）を構築します。呼び出し側が渡したファクトリーは受け付けません。エンドポイントノードの呼び出しは、`llm_call` の呼び出しと同様に、Coreのプロバイダー経路をたどります。ホストのプロバイダー呼び出しブローカーがこれらをジャーナルに記録してリプレイし、モデルのトークン予算は出力の上限がないためこれらを拒否し、価格付きバインディングは、モデルカタログがエンドポイント呼び出しの上限を決められるようになるまで、`P_MONEY_UNMODELED` で拒否します。固定のCoreノードはすべて、正確で変更できない要件リゾルバーとともに登録してください。モデルが書いたトポロジーは、admission済みのタイプとノードの設定を選べますが、任意のbrokeredファクトリーに、生のToolポインターを受け取らせることはできません。標準のディスパッチノードは、引き続き `dispatch_tool_calls` を呼び出します。これは、バインド済みのToolを呼び出す前に、その実行の `ToolGate` と `ToolExecutionController` に問い合わせます。

`TrustedNative` は、ホストが証明する、別の境界です。Programのadmissionは、すでに、`TrustedEmbedding` モードと、その効果モードに対応する、認証済みのCatalogホストIDを要求します。ネイティブのC++コードは、`NodeContext` の外で取得したリソースを保持したり、自分で効果を実行したりできます。これらの規則はcapabilityの減衰であり、プロセスのサンドボックスでも、任意のネイティブコードが副作用を持たないことの証明でもありません。ホストは、ネイティブファクトリーをレビューして固定し、その効果を正直に宣言する必要があります。

ProgramのCoreのオペレーションが、仲介されたToolディスパッチを行うには、ホストが所有する `ProgramCoreToolGrant` が必要です。ランタイムは、オーナー、Programバージョン、実行、オペレーション、試行、**admission済みの実行可能バインディングのフィンガープリント**、空でないグラントID、ゲート、コントローラーを確認します。グラントがない、または古い場合は、再接続の後も含めて、Tool呼び出しが拒否されます。拒否されたTool呼び出しは、Toolの結果の1つであり、Program自体は `Completed` になることがあります。要求した処理が成功したかどうかは、効果のレシートを確認して判断してください。

永続的なグラントのIDが必要なホストは、実行IDを明示して実行を開始する前に、`SQLiteProgramCoreToolGrantStore` に、正確な `ProgramCoreToolGrantRecord` をadmissionできます。バインディングのフィンガープリントは、`capability_binding_receipt_root(version.core_materialization_receipt().capability_bindings)` です。`make_durable_core_tool_grant_resolver(store, policy_factory)` を、`RuntimeConfig.core_tool_grant_resolver` として設定してください。このリゾルバーは、再接続を含め、オペレーションごとにレコードを読み込み直し、グラントID、オーナー、バージョン、実行、オペレーション、試行、バインディングがすべて同一である、新しいホストのポリシーだけを受け付けます。admissionが冪等なのは、同一のアクティブなレコードの場合だけです。IDやバインディングが食い違うレコードで、置き換えることはできません。再開したProgramは、試行を進めます。そのため、ホストは、再開する前に、その試行について、まだ許可されているグラントを、明示的にadmissionしなければなりません。同じグラントIDが、それらの試行をカバーできるのは、オーナー、バージョン、実行、オペレーション、バインディングが同じ場合に限られます。取り消すと、admission済みのすべての試行が無効になります。保存されるレコードには、認証情報、Toolポインター、コールバックは含まれません。

admissionするかどうか、Toolごとのゲートとコントローラーのポリシーをどう組み直すかは、ホストだけが決めます。このレコードは権限のエビデンスであり、効果の結果では**ありません**。ホストは、呼び出しごとの効果ブローカーを通じて、Toolのディスパッチ、完了、不確かな結果を、引き続きジャーナルに記録する必要があります。外部での実行がちょうど1回であることは約束せず、仲介されたディスパッチの外でToolを呼び出す、信頼された任意のネイティブコードを制約することもありません。

`neograph::sqlite` の `SQLiteToolEffectBroker` は、`dispatch_tool_calls` のための、永続的な先行書き込み（write-ahead）ディスパッチを実装します。永続的なSQLiteのパスと、ホストが登録した `{Tool*, executable_id}` のバインディングを指定して構築します。実行可能なIDは、モデルから見えるTool名だけでなく、実際のコード、設定、リモートの宛先、認証情報の権限を固定するものでなければなりません。このブローカーは、Programのグラントに渡すか、単独のCoreでは `RunResources::tool_effect_broker` に渡します。単独のCoreでは、対応する `tool_effect_grant` も設定します。`make_tool_execution_context` を使う組み込みノードとホストが登録したノードは、共有しているエンジンを変更せずに、これらの呼び出し単位のリソースを引き継ぎます。単独の `llm::Agent::run` / `run_stream` は、同じブローカー、安定したオーナー／実行／スレッドのID、オペレーション、ホストのグラントを持つ、実行ごとの `ToolExecutionContext` を受け付けます。再接続のときに、保留中のバッチをリプレイするには、呼び出し側が、ネイティブやツールのパートを含む、順序付きの完全な `sp::Message` の履歴を保持する必要があります。表示専用の `ChatMessage` の射影から、ネイティブな継続の権限を再作成することはできません。ネイティブな保管を永続化するには、さらに、ホストの `sp::NativeArchive` が必要です。ブローカーを使わないAgent／Coreの経路は、保証の水準が低くなります。

ブローカーは、Coreのタスクとバッチの序数ごとに、コントローラーを呼び出す前に、SQLiteのFULL同期のマーカーをコミットします。マーカーのコミットに失敗した場合、Toolは実行されません。レシートは、オーナー／実行／スレッド／タスク／序数、Programのバージョンとバインディング、オペレーションとグラント、登録された実行可能なもの、正規化して書き換えられた引数を結び付けます。モデルの `ToolCall.id` は、対応付けのためだけに使われます。確認済みのレシートは、ディスパッチせずにリプレイされます。ID、権限、引数が食い違う場合は拒否されます。保留中のマーカー、タイムアウト、キャンセル、ディスパッチ後の実行の失敗、レシートのコミットの失敗は、外部での照合が必要で、再起動をまたいで再ディスパッチをブロックします。試行が変わっても、新しい呼び出しスロットにはならず、来歴として記録されます。外部での結果がちょうど1回であると主張してはいけません。永続的なマーカーがあっても、クラッシュの前に外部サービスがコミットしたかどうかは、判定できません。

brokeredなカスタムファクトリーは、`NodeContext.provider` や `NodeContext.tools` を使えません。実行を固定のCoreノードに移すか、別途レビューされたホストのブローカーを使うか、ホストの信頼された組み込みのポリシーのもとで `TrustedNative` としてadmissionしてください。`add_host_brokered_node` は、Harnessのワーカーが使う、ネイティブのホストブローカーを登録するための、明示的な経路です。これによって、呼び出し側が渡したファクトリーが安全になるわけではありません。ホストは、その効果をレビューして固定し、各オペレーションを、admission済みのプロバイダーとツールのクロージャーの範囲内にとどめる必要があります。通常の `add_node` は、メタデータだけの境界を保ちます。Programの外にある、直接のCoreグラフは、絞り込まれず、`NodeContext` 全体を受け取ります。永続性とネイティブコードの境界については、[#291](https://github.com/fox1245/NeoGraph/issues/291)、[#292](https://github.com/fox1245/NeoGraph/issues/292)、[#293](https://github.com/fox1245/NeoGraph/issues/293)を参照してください。
