<!-- neograph-i18n: source=docs/ABI_POLICY.md locale=ja source_sha256=03f09d9ead6f5529e387139eb3c5ff330eeea4e551e13bb01c8d33e13ef9d89e -->
# バイナリ互換性ポリシー

**Languages:** [English](ABI_POLICY.md) | [한국어](ABI_POLICY.ko.md) | [日本語](ABI_POLICY.ja.md) | [简体中文](ABI_POLICY.zh-CN.md)

## バージョンとローダーの契約

CMakeは、NeoGraphのバージョンを `pyproject.toml` から読み取り、コンパイルされる公開ライブラリの `VERSION` にそのバージョンを、`SOVERSION` にそのメジャー番号を設定します。

| リリースライン | ローダーの世代 | 契約 |
|---|---:|---|
| `0.x` | `0` | v1より前です。告知された再ビルドの境界で、バイナリ互換性が壊れることがあります。 |
| `1.x` | `1` | 安定版v1に予定しているポリシーです。v1がリリース済みだという主張ではありません。 |
| `N.x`（`N >= 2`） | `N` | メジャーなABIの境界です。コンシューマーは再ビルドしてください。 |

`SOVERSION 0` がv1より前のパッケージに与えるのは、ローダー用の名前であり、互換性のあるレイアウトではありません。ローダーは、互換性のない `0.x` への置き換えをすべて拒否できるわけではありません。更新先のリリースノートを読み、ヘッダーとライブラリはまとめて置き換え、告知されたすべての境界で再ビルドしてください。v1より前のライブラリを、動作中のプロセスの中で1つだけ差し替える（ホットスワップする）ことはしないでください。

## 再ビルドが必須になる境界

- `0.9.0` より前から `0.9.0+` へ：GraphNodeから、従来の実行用の仮想関数8個が削除されました。カスタムノードは `run(NodeInput)` を実装します。SyncGraphNodeは、追加のアダプターです。
- その後のv1より前の、リソース上限、UsageAccumulatorの予約、issue #216に関する変更：公開レイアウトに、会計、admission、キャッシュ、ランタイムのインターポジションの状態が加わりました。対応するヘッダーで再ビルドしてください。
- 型付きプロバイダーへの切り替え（typed cutover）：すべてのC++コンシューマーとカスタムプロバイダーを、対応するNeoGraphとSchemaProvider SDKのヘッダーおよびライブラリで再ビルドしてください。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、古いディスクリプターのインタープリター、ResponsesのWebSocket経路は、エイリアスなしで削除されました。
- イベント駆動のプロバイダーディスパッチ：CancelTokenは `std::stop_source` を使い、`stop_token()` を公開します。`cancel`、`fork`、Asioスロットのシグネチャが残っているからといって、古いインラインのキャンセルのコードに互換性があるわけではありません。
- drain済みのキャンセルコンテキストのdetach：CancelTokenの公開レイアウトに、世代で区切られた発行状態が加わりました。NeoGraphと、すべてのネイティブコンシューマーをまとめて再ビルドしてください。detachは、コンテキストを破棄する前に、完了したスロットのハンドラーを解放します。ただし、操作がdrainされていることが引き続き必要で、キャンセルの状態はリセットされません。
- `ChatTool::strict`：ポータブルなツール定義にメンバーが追加されました。`ChatTool` は、`Tool::get_definition()` と `make_provider_request` で、値渡しでライブラリの境界を越えます。そのため、NeoGraph、すべてのネイティブコンシューマー、カスタムの `Tool`、Pythonの拡張を、まとめて再ビルドしてください。
- 将来の `1.0.0` の境界では、ローダーの世代が `1` になり、再ビルドが必要になります。世代1での固定は、そのリリースに向けたポリシーです。

## エクスポートされるインターフェース

Providerのサブクラス用のフックは、`get_name`、`family`、`prepare(ProviderRequest)` の3つです。共通の `invoke(_async)` と `dispatch(_async)` は、所有権を持つ型付きのリクエストを消費し、変更できない `sp::runtime::Result` を返します。対になった、completion用の仮想関数のオーバーライドではありません。

CheckpointStoreは、アダプターへの明示的な移行のために、従来のレイアウトを保っています。同期のデフォルト実装は、非同期のオーバーライドを呼び出さずに失敗します。非同期のデフォルト実装は、同期のオーバーライドをワーカーに移します。非同期だけの新しいバックエンドは、AsyncCheckpointStoreを実装して `adapt_async_checkpoint_store` を使います。同期のcapabilityを持つバックエンドは、CheckpointStoreCoreを実装して `adapt_checkpoint_store` を使います。v1より前の告知された境界では、再ビルドしてください。このアダプターの設計は、共有ライブラリだけを置き換えてよいという許可ではありません。

## SDKとPythonの境界

Coreは、LLMノードを無効にしても、外部の `SchemaProvider::runtime` を必要とします。採用しているSDKのリリースは `0.4.0` alphaで、公開されているSDKの `main` ラインのうち `cmake/NeoGraphSchemaProvider.cmake` が固定するコミット、インターフェースリビジョン `7`、共有ライブラリのABIリビジョン `7` です。capabilityのチェックはアウトオブラインです。これは、安定したインターフェースだという主張ではありません。対応するSDKのコンポーネントは、まとめてインストールしてください。`libsp_*.so.7` の世代は、NeoGraphのローダーの世代とも、Pythonの `abi3` wheelタグとも別のものです。

インターフェース7では、メディアが型付きになります。メッセージ、結果、イベントで、`sp::Media` が `sp::Image` に代わります。また、リクエストの制御が各familyに固有の型へ拡張され、チャット以外のエンドポイントとリアルタイムセッションがクライアントに加わりました。公開されているレイアウトは全体にわたって変わり、NeoGraphのハンドシェイクは、`RequiredProvider` のcapabilityを含めて、インターフェース7を要求します。SDKのコンシューマー、NeoGraph、Pythonの拡張は、まとめて再ビルドしてください。これより前のヘッダーやライブラリは、インターフェース7とは互換性がありません。ブロッキングのラッパーは、SDKのイベントループとコールバックのワーカーへの再入を、引き続き拒否します。ネイティブarchive v3（`spna3`）は変わりません。ポータブルJSONは独立したバージョンを持つ別の形式で、NeoGraphはプロバイダーのメッセージ、プロバイダーの結果、runtime history recordをv3で書き出し、v2のレコードも引き続き読み込みます。v2のレコードのバイト列は保たれます。v3のプロバイダーの結果には、メディアパート、引用、型付きのツール結果の内容、エラーの理由、実際に応答したモデルが加わります。正規の結果の使用量に、省略可能な `provider_cost` が加わるのは、金額のメタデータがある場合だけです。メタデータがないだけの過去のレコードは、元のバイト列を保ち、コストを捏造することはありません。

Pythonは、準備済みのハンドルや変更できない結果を含む、型付きのプロバイダー契約を公開します。従来のcompletionの互換シムはありません。拡張と、それに対応するライブラリは、1つのwheelとしてインストールしてください。同梱されているNeoGraphやSDKのライブラリを、1つだけ置き換えてはいけません。グラフ向けの便宜的な値であるChatMessageは、ネイティブなProviderMessageの保管の代わりにはなりません。詳しくは[Pythonバインディング](python-binding.md)を参照してください。

wheelに同梱されるのは、対応するSDKのランタイムの共有ライブラリ7個（curlを使わないワイヤー層を含む）です。SDKのC++ヘッダーとCMakeパッケージは含まれません。C++のコンシューマーは、SDKを別にインストールします。ソースの解決順は、明示した `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`、インストール済みのパッケージ、リビジョンを固定した公開archiveのフォールバックの順です。インストール済みのSDKを使ってオフラインでビルドするには、`NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` を設定し、`CMAKE_PREFIX_PATH` でそのプレフィックスを指定します。

## インストールされる名前とプラットフォームの制約

Linuxの共有ライブラリには、バージョン付きのファイル、メジャーの世代を表すSONAMEのリンク、バージョンなしのリンカー用の名前があります。NeoGraphの共有ライブラリは、同じディレクトリにある依存先を `$ORIGIN` で参照します。macOSの `.dylib` / `@loader_path` と、Windowsの接尾辞なしの `.dll` という命名は、パッケージングの慣習のままです。静的archiveにはSONAMEがなく、推移的なリンク要件もなくなりません。

WASMでのランタイム統合は確立されていません。wheelタグが表すのは、Python/ABI/プラットフォームの互換性だけです。詳しくは[PyPAのタグ仕様](https://packaging.python.org/en/latest/specifications/platform-compatibility-tags/)を参照してください。

## インストールの検証

`python scripts/verify_ci.py install --work-dir <fresh-output> --jobs 2` は、インストール済みコンシューマーのチェックを実行します。プラットフォームが対応しているコンシューマーの行に合わせて、`--shared`、`--core-only`、または `--program --quickjs` を使ってください。このドライバーは、チェックアウトした状態でも、展開したsdistからでも、GitやBashなしで動作します。
