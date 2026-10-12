<!-- neograph-i18n: source=docs/ASYNC_GUIDE.md locale=ja source_sha256=a48e529a151529d9711ee297aed8bc4b7e603c0198c596188608cb6a8565dc13 -->
# NeoGraph非同期ガイド

**Languages:** [English](ASYNC_GUIDE.md) | [한국어](ASYNC_GUIDE.ko.md) | [日本語](ASYNC_GUIDE.ja.md) | [简体中文](ASYNC_GUIDE.zh-CN.md)

## 実行のエントリポイント

`GraphEngine::run`、`run_stream`、`resume` は、対応するコルーチン版を、同期のブリッジ経由で動かします。`run_async`、`run_stream_async`、`resume_async` は `asio::awaitable<RunResult>` を返し、呼び出し側のexecutorを使います。操作が終わるまで、そのexecutorを動かし続けてください。コールバックがまだ使っているリソースを、破棄してはいけません。

`EngineConfig::worker_count` のデフォルトは `1` で、エンジンが所有するfan-out用のプールはありません。I/Oを待つ分岐は、中断している間は重なって進められます。CPUを使う処理は、executorのスレッドが1つだけだと、直列に実行されます。それらの処理で複数のコアが必要な場合は、エンジンを公開する前にワーカープールを設定してください。詳しくは[並行性](concurrency.md)と `examples/27_async_concurrent_runs.cpp` を参照してください。

## 準備済みのプロバイダー呼び出し

`SchemaProvider` は、closedな `sp::descriptor::ValidatedDescriptor`、`sp::runtime::Options`、省略可能な型付きの `SchemaProvider::Defaults` から作ります。ランタイムはlibcurlを使います。古いディスクリプターのインタープリター、`prefer_libcurl` セレクター、ResponsesのWebSocket経路は、削除されました。

`make_provider_request` は、ファミリー固有の型付きペイロードを組み立てます。デフォルトは `ProviderMode::Collect` です。`Stream` は、`on_event` とは別に、明示的に選びます。`prepare` は、検証とエンコードを1回だけ行います。`dispatch(_async)` は、ムーブ専用のハンドルを1回だけ消費します。`invoke(_async)` は、準備とディスパッチをまとめて行います。返されるawaitableがリクエストとクライアントの状態を所有するので、元のリクエストとProviderは、スケジュールされた後まで生きている必要はありません。

```cpp
#include <neograph/async/run_sync.h>
#include <neograph/provider.h>

// provider owns a validated descriptor and SDK runtime policy.
auto request = neograph::make_provider_request(
    *provider, model, messages, {}, {}, neograph::ProviderMode::Collect);
request.cancel_token = cancel_token;
request.options.deadline = deadline;
auto prepared = provider->prepare(std::move(request));
auto result = neograph::async::run_sync(
    provider->dispatch_async(std::move(prepared)));
```

C++の `on_event` が受け取るのは、借用された `sp::Event` のビューです。コールバックの後に必要なバイト列は、コピーしてください。イベントには、テキストだけでなく、使用量、reasoning、ツール、rawワイヤーの観測も含まれます。変更できない `sp::runtime::Result` は、`sp::Completion` または `sp::Failure`（部分的な失敗のエビデンスを含む）を所有します。オブザーバーが失敗した場合、その結果は `ProviderObserverError::outcome()` に、コールバックの例外は `cause()` に保持されます。

ディスパッチは、容量1にまとめられた通知を待ちます。イベントと結果は、上限付きのBridgeが保持します。通知はイベントのキューではありません。キャンセルすると、停止の要求がSDKに転送されます。`operation.join()` は、コールバックの復帰とadmissionスロットの解放を待ちます。これは、オブザーバーやリソースの失敗があった場合も同じです。デッドラインは操作の時間を制限します。呼び出し側を止めても、サーバーがリクエストを受け取らなかったことの証明にはなりません。

永続的なディスパッチを行う場合は、`Provider::request_digest()` を組み立てた内容に結び付け、admission済みの請求枠を予約して、レシートを永続化します。そのうえで、同じハンドルを `ControlledProvider::dispatch_prepared(_async)` で消費します。重複したレシートがあっても、再ディスパッチは許可されません。予約は、すでにadmission済みの支出の権限を引き落とすか保留するものです。プロバイダーが報告した使用量でも、請求書でもありません。欠けているカウンターは不明のままです。配送が不明なエビデンスや部分的なエビデンスがあっても、未解決のホールドを解放する根拠にはなりません。

完全なメッセージは、メモリ上で、本物のネイティブな継続を保持します。ポータブルなJSONの射影が持つのは観測内容で、ネイティブリプレイや金銭に関する権限ではありません。ネイティブな継続を永続化するのは、admission済みの `sp::NativeArchive` を通す場合だけにしてください。テキストや生のJSONから再構成してはいけません。詳しくは[マイグレーションガイド](migration-v0.4-to-v1.0.md)を参照してください。

## カスタムのプロバイダーとノード

Providerのサブクラスが実装するのは、`get_name`、`family`、`prepare` だけです。共通の invoke / dispatch メソッドは、completion用の仮想フックではありません。

```cpp
#include <neograph/provider.h>

class MyProvider final : public neograph::Provider {
    std::string family_;
    std::shared_ptr<sp::runtime::Client> client_;
public:
    MyProvider(sp::descriptor::ValidatedDescriptor descriptor,
               sp::runtime::Options options)
        : family_(descriptor.family()),
          client_(std::make_shared<sp::runtime::Client>(
              std::move(descriptor), std::move(options))) {}
    std::string get_name() const override { return "my-provider"; }
    std::string_view family() const noexcept override { return family_; }
    neograph::PreparedProviderRequest prepare(neograph::ProviderRequest request) override {
        return prepare_runtime(client_, std::move(request));
    }
};
```

`prepare_local` は、本物のローカルディスパッチの状態を所有するC++のアダプターが使えます。借用した `this` ではなく、所有する共有状態をキャプチャしてください。Pythonのサブクラスは、準備を本物のプロバイダーに委譲します。結果やローカルディスパッチの権限を偽造することはできません。

カスタムのグラフノードは、`run(NodeInput) -> asio::awaitable<NodeOutput>` をオーバーライドします。`in.state` と `in.ctx` を読み、出力は、nullでない `in.stream_cb` を通してだけ行います。書き込み、Command、Sendは、1つの `NodeOutput` にまとめて返します。エンジンは、同期のストリーミングでも非同期のストリーミングでも、ディスパッチ1回につきノードを1回だけ呼び出します。完全なプロバイダー履歴には `RunConfig::provider_messages` を使ってください。`ChatMessage` はグラフ向けの便宜的な型で、SDKのメッセージモデルではありません。

<a id="94-checkpointstore"></a>
## CheckpointStoreのアダプター

`CheckpointStoreCore` には、5つの同期操作（save、load_latest、load_by_id、list、delete_thread）が必要です。`adapt_checkpoint_store` は、エンジンが求める契約を提供し、ブロックする処理を上限付きのワーカーに移します。`AsyncCheckpointStore` には、それらに対応する5つの非同期操作が必要です。`adapt_async_checkpoint_store` は、明示的な同期の管理用ファサードを提供します。

従来の `CheckpointStore` の同期のデフォルト実装は、`std::logic_error` で失敗します。非同期のデフォルト実装は、同期のオーバーライドをワーカーに移します。非同期だけを実装した従来のサブクラスは、明示的な非同期のcapabilityとアダプターに移行する必要があります。インメモリの操作は、ミューテックスで保護されたストレージを呼び出し元のスレッドで使い、SQLiteはブロックする処理をワーカーに移し、PostgreSQLはパイプラインのバッチ処理なしで、ノンブロッキングのlibpq I/Oを使います。

保留中の書き込みの永続化は、`PendingWritesCheckpointStore` という別のcapabilityです。これがない場合、再開ではスーパーステップ全体がリプレイされます。何もしないメソッドで、外部への効果が重複排除されることはありません。Pythonのチェックポイントのサブクラスは、同期メソッドを実装します。非同期ネイティブのバックエンドは、C++のアダプターがホストします。

<a id="95-mcpclient"></a>
## MCPClient

`rpc_call` は、`rpc_call_async` を同期的に動かします。MCPClientは、カスタムトランスポートのサブクラスを作るためのインターフェースではありません。HTTP呼び出しは重なって進みます。1つのstdioセッションでは、フレームの書き込みは直列になり、1つのリーダーが、JSON-RPCのIDによって応答を振り分けます。待機者を1つキャンセルしても、共有しているトランスポートは閉じません。直列のサブプロセスが、スループットを制限する点は変わりません。

<a id="96-tool-vs-asynctool"></a>
## ToolとAsyncTool

同期のToolが実装するのは、`execute`、`get_definition`、`get_name` です。AsyncToolが実装するのは、`execute_async`、`get_definition`、`get_name` です。AsyncToolの同期版 `execute` はfinalで、内部の `run_sync` コンテキストを動かします。両方の面をオーバーライドしてはいけません。また、共有しているイベントループのスレッドを、長い同期処理でブロックしてはいけません。

## 汎用のHTTPストリーミング

NeoGraphの `async_post_stream` は、引き続き、残されているコンシューマーが使う、独立した汎用のHTTPユーティリティです。SDKのlibcurlによるディスパッチが、これを置き換えることはありません。チャンク転送、上限付きの `Content-Length`、接続のクローズで終わる応答本体に対応しています。空でない固定長の本体は、コールバックに1回だけ渡されます。長さ0の本体では、コールバックは呼ばれません。返される `HttpStreamResponse.status` は、200と2xx以外のステータスをそのまま保持します。そのため、呼び出し側は、SSEでない応答を空の成功として扱わずに、JSONのエラー本体を解釈できます。

`RequestOptions` のデフォルトは、ステータスとヘッダーが64 KiB、デコード後の本体が16 MiB、転送チャンクが1 MiBの上限です。0にすると、対応する上限が無効になります。固定長の場合は、メモリを確保する前に、本体の上限を確認します。無効または曖昧なフレーミング、途中でのEOF、すでにバッファに入っている余分なバイトは、拒否されます。ただし、関数が戻った後に届くバイトを検出することは、約束していません。リダイレクトの本体は、別に扱われます。ホップごとのタイムアウトのデフォルトは0で、リダイレクトは無効です。これらの汎用のオプションは、SDKのポリシーでも、モデルの支出のグラントでもありません。

## Pythonと寿命の境界

Pythonは、`prepare`、`dispatch`、`invoke` を、型付きの `ProviderRequest`、`PreparedProviderRequest`、変更できない `ProviderOutcome` とともに公開します。invoke / dispatch はGILを解放します。コールバックとPythonオブジェクトの破棄では、GILを取得します。asyncioとの境界では、`asyncio.to_thread(provider.invoke, request)` を使ってください。C++のプロバイダーのawaitableが、asyncioのawaitableに自動で変換されることはありません。詳しくは[Pythonバインディング](python-binding.md)を参照してください。

`run_sync` は、呼び出しごとに専用のシングルスレッドのコンテキストを作ります。`run_sync_pool` は、呼び出しごとにプールを作ります。これらのexecutorに結び付いたハンドルを、呼び出しの外に持ち出してはいけません。コルーチンは、通常の関数として別のawaitableをそのまま返すか、コルーチン本体で `co_return co_await` を使います。その本体に、普通の `return` を混ぜてはいけません。

## ビルドと過去のエビデンス

`neograph::core` は、`NEOGRAPH_BUILD_LLM=OFF` の場合でも、外部の `SchemaProvider::runtime` を必要とします。インストール済みのSDKのプレフィックスか、`NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` を使ってください。SDKのビルドには、CMake 3.20以降、C++20、Python、スタンドアロンのAsio、yyjson、libcurl 7.88以降、OpenSSL Cryptoが必要です。記録されている、interface 4より前のランタイムとarchiveの適格性確認は、Linux/POSIXを対象にしています。interface 4の適格性確認にはなりません。既存のmacOS/Windows向けメタデータは、この依存の適格性確認にはなりません。WASMでのランタイム統合は、確立されていません。

NeoGraphの構成には、CMake 3.20以降が必要です。解決順は、まず明示したSDKのソースディレクトリ、なければインストール済みのパッケージ、最後にリビジョンを固定した公開archiveの既定のフォールバックです。インストール済みのSDKを使ってオフラインで構成するには、`NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` を設定し、`CMAKE_PREFIX_PATH` にそのSDKのプレフィックスを指定します。取得したSDKやソースのSDKは、リポジトリに含まれているNeoGraphのAsioとyyjsonを使いますが、システムの開発用の依存は、引き続き必要です。

[Stage 3の設計](ASYNC_STAGE3_DESIGN.md)は、2026年4月の提案をアーカイブしたものです。その中の2.0/3.0というラベル、completionの相互呼び出し、テストやベンチマークの件数は、過去のマイルストーンです。現在のリリース番号でも、サポートされるシグネチャでも、新たな合格の主張でもありません。現在のリリース番号は `pyproject.toml` にあります。過去の測定結果は[性能の詳細解説](performance-deep-dive.md)にあります。

元のStage 3のガイドには、次の値が記録されていました。同期パスのテストは、当時存在していた276件以上です。エンジンのオーバーヘッドは、seqが約30 µs、parが約205 µsです。HTTPのasync_poolは17834 ops/sで、これに対してStage 2のasyncは8401/s、syncは6064/sです。50000個のタイマーによるfan-outは541K ops/sで、RSSは67 MBです。ほかに、50 msのエージェント3つが50 msで完了したこと、3つのresearcherが150 msで完了したこと（逐次では370 ms）も記録されていました。これらは、それらのワークロードに対する過去の観測であり、現在のプロバイダー切り替え（typed cutover）後の結果ではありません。
