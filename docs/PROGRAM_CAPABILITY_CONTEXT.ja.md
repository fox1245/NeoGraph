<!-- neograph-i18n: source=docs/PROGRAM_CAPABILITY_CONTEXT.md locale=ja source_sha256=19977a5adc5fc8fac5dafb2f604519c6974ccafa509aa01ccafd393cf0c7c962 -->
# Program ノードの能力と仲介された副作用

**Languages:** [English](PROGRAM_CAPABILITY_CONTEXT.md) | [한국어](PROGRAM_CAPABILITY_CONTEXT.ko.md) | [日本語](PROGRAM_CAPABILITY_CONTEXT.ja.md) | [简体中文](PROGRAM_CAPABILITY_CONTEXT.zh-CN.md)

`ProgramCatalog` は実行可能要素の閉包全体を一度だけバインドする。ただし、ノードファクトリが参照できる能力は、そのノードの承認済み構成で宣言されたものに限るべきである。このため `RegistrySnapshotBuilder` は、ノードマニフェストの直接の実行要件と構成別の要件リゾルバを使い、構築時の `NodeContext` を絞り込む。別のノードが使うバインド済み Provider や Tool は、このノードの暗黙の能力にはならない。

| 登録されたノード | Provider ポインタ | Tool ポインタ | 安全なメタデータ |
| --- | --- | --- | --- |
| 任意の `Brokered` C++ ファクトリ | なし | なし | 正確な `provider_name` と `tool_definitions` |
| ホストがレビューした `add_host_brokered_node` | 宣言された Provider のみ | 宣言された Tool のみ | 正確なメタデータ |
| 固定の `add_core_llm_call` | 宣言された Provider のみ | モデル定義用に宣言された Tool のみ | 正確なメタデータ |
| 固定の `add_core_tool_dispatch` | なし | 宣言された Tool のみ | 正確なメタデータ |
| `TrustedNative` C++ ファクトリ | 宣言された Provider のみ | 宣言された Tool のみ | 正確なメタデータ |

二つの固定 Core 登録は NeoGraph 自身の `LLMCallNode` と `ToolDispatchNode` を構築する。呼び出し元のファクトリは受け付けない。正確で不変な要件リゾルバを使って登録すること。モデルが作成したトポロジは、承認された型とノード構成を選べるが、任意の brokered ファクトリに生の Tool ポインタを渡すことはできない。標準の dispatch ノードは引き続き `dispatch_tool_calls` を呼び、バインド済み Tool を実行する前に実行単位の `ToolGate` と `ToolExecutionController` を参照する。

`TrustedNative` は、ホストの証明を要する独立した境界である。この副作用モードを Program に承認するには、すでに `TrustedEmbedding` モードと、それに一致する認証済み Catalog ホスト識別情報が必要である。ネイティブ C++ コードは `NodeContext` の外で取得したリソースを保持したり、独自に副作用を発生させたりできる。ここでの規則は能力の縮小であり、プロセスサンドボックスでも、任意のネイティブコードが純粋であることの証明でもない。ホストはネイティブファクトリをレビューして固定し、その副作用を正確に宣言しなければならない。

Program の Core 操作が仲介された Tool dispatch を行うには、ホスト所有の `ProgramCoreToolGrant` が必要である。ランタイムは owner、Program version、run、operation、attempt、**承認済み実行バインディングのフィンガープリント**、空でない grant ID、gate、controller を検査する。grant がない場合や古い場合は、再接続後も含め Tool 呼び出しを拒否する。拒否された Tool 呼び出しも Tool 結果として扱われ、Program 自体は `Completed` になる場合がある。要求した作業が成功したかを判断する際は、副作用の受領記録を確認すること。

永続的な grant 識別情報が必要なホストは、明示的な run ID で実行を開始する前に、正確な `ProgramCoreToolGrantRecord` を `SQLiteProgramCoreToolGrantStore` に承認できる。バインディングのフィンガープリントは `capability_binding_receipt_root(version.core_materialization_receipt().capability_bindings)` である。`make_durable_core_tool_grant_resolver(store, policy_factory)` を `RuntimeConfig.core_tool_grant_resolver` に設定する。このリゾルバは再接続を含む操作ごとにレコードを再読み込みし、grant ID、owner、version、run、operation、attempt、binding がすべて同一の、新たに作成されたホストポリシーだけを受け入れる。承認が冪等なのは同一の有効レコードに限り、競合する ID や binding で置き換えることはできない。Program の再開時には attempt が進むため、ホストは再開前に、その attempt に対して引き続き許可する grant を明示的に承認しなければならない。同じ grant ID が複数の attempt を対象にできるのは、owner/version/run/operation/binding が同じ場合だけであり、失効させるとその grant で承認したすべての attempt が無効になる。保存レコードに認証情報、Tool ポインタ、コールバックは含まれない。

承認するかどうか、および Tool ごとの gate と controller ポリシーをどう再構築するかは、ホストだけが決定する。レコードは権限の証拠であり、**副作用の結果ではない**。ホストは呼び出しごとの effect broker を通じ、各 Tool dispatch、完了、不確実な結果を引き続きジャーナルに記録しなければならない。これは外部処理の exactly-once 実行を保証せず、仲介 dispatch の外で Tool を呼ぶ任意の信頼済みネイティブコードも制約しない。

`neograph::sqlite` の `SQLiteToolEffectBroker` は、`dispatch_tool_calls` 向けに永続的な write-ahead dispatch を実装する。永続的な SQLite パスと、ホスト登録済みの `{Tool*, executable_id}` バインディングを渡して構築する。実行可能要素の ID は、モデルに見える Tool 名だけでなく、実際のコード、構成、リモート送信先、認証情報に基づく権限を固定しなければならない。この broker を Program grant、または単独 Core の `RunResources::tool_effect_broker` に渡す。単独 Core では対応する `tool_effect_grant` も設定する。`make_tool_execution_context` を使う組み込みノードとホスト登録ノードは、共有エンジンを変更せずにこれらの呼び出し単位のリソースを継承する。単独の `llm::Agent::run`/`run_stream` は、同じ broker、安定した owner/run/thread 識別情報、operation、ホスト grant を含む実行ごとの `ToolExecutionContext` を受け取る。再接続時に保留中のバッチを再現するには、呼び出し元が native/tool 部分を含む完全で順序付きの `sp::Message` 履歴を保持しなければならない。表示専用の `ChatMessage` 射影では、ネイティブ継続の権限を再構築できない。永続的なネイティブ保管には、さらにホストの `sp::NativeArchive` が必要である。従来の broker を使わない Agent/Core 経路は、保証の弱い経路として明示的に残る。

Core task と batch ordinal ごとに、broker は controller を呼ぶ前に SQLite FULL-sync マーカーをコミットする。マーカーの書き込みが失敗した場合、Tool は実行しない。受領記録は owner/run/thread/task/ordinal、Program version/binding、operation/grant、正確な登録済み実行可能要素、書き換え後の正規引数を結び付ける。モデルの `ToolCall.id` は相関付け専用である。確認済みの受領記録は dispatch せず再利用し、識別情報、権限、引数の競合は拒否する。保留中マーカー、タイムアウト、キャンセル、dispatch 後の実行失敗、受領記録のコミット失敗には外部照合が必要であり、再起動をまたいで再 dispatch を禁止する。attempt の変更は来歴として記録し、新しい呼び出しスロットにはしない。外部結果の exactly-once を主張してはならない。永続マーカーがあっても、クラッシュ前に外部サービスがコミットしたかは判断できない。

`NodeContext.provider` や `NodeContext.tools` を使っていた既存の brokered カスタムファクトリは、実行を固定 Core ノードへ移すか、別途レビューしたホスト broker を使うか、ホストの trusted embedding ポリシーで `TrustedNative` として承認する必要がある。`add_host_brokered_node` は Harness worker が使う、明示的なネイティブ host-broker 登録経路である。呼び出し元が提供するファクトリの安全性を保証するものではない。ホストは副作用をレビューして固定し、各操作を承認済み provider/tool 閉包の範囲に収めなければならない。通常の `add_node` はメタデータだけを渡す境界を維持する。Program 外の直接 Core graph は、既存の `NodeContext` の挙動を維持する。残る永続性とネイティブコードの境界については [#291](https://github.com/fox1245/NeoGraph/issues/291)、[#292](https://github.com/fox1245/NeoGraph/issues/292)、[#293](https://github.com/fox1245/NeoGraph/issues/293) を参照。
