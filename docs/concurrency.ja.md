<!-- neograph-i18n: source=docs/concurrency.md locale=ja source_sha256=889743688862b981a4a8e8d8de0c0f3bc287693712453d430183d4dcdd7a030a -->
# 並行実行と非同期

**Languages:** [English](concurrency.md) | [한국어](concurrency.ko.md) | [日本語](concurrency.ja.md) | [简体中文](concurrency.zh-CN.md)

## executorの選択

同期の `run`、`run_stream`、`resume` は、非同期版と同じコルーチンの実装を動かしている間、呼び出し元のスレッドをブロックします。ホスト側のワーカープールを使えば、互いに独立したセッションを並行して実行できます。非同期版は `asio::awaitable<RunResult>` を返すので、自分で用意したexecutorで駆動してください。awaitableにしても、任意のユーザーコードがノンブロッキングになるわけではありません。

`EngineConfig::worker_count = 1` がデフォルトで、エンジンが所有するfan-out用のプールは作られません。中断中のI/O分岐は、1つのスレッド上でも重なって進められます。そのスレッド上のCPU処理は、直列に実行されます。呼び出し側のexecutorがマルチスレッドの場合や、明示的に有効にしたエンジンのワーカープールを使う場合は、CPUを使う分岐を複数のコアで実行できます。プールの設定は、エンジンを公開する前に済ませてください。

```cpp
#include <neograph/async/run_sync.h>

EngineConfig options;
options.node_context = ctx;
options.checkpoint_store = std::make_shared<InMemoryCheckpointStore>();
options.worker_count = 4;
auto engine = GraphEngine::build(def, std::move(options));
RunConfig run;
run.thread_id = "session-1";
run.input = {{"count", 0}};
auto result = neograph::async::run_sync(engine->run_async(run));
```

サンプル `27_async_concurrent_runs.cpp` は、1つのio_contextで複数のセッションを動かす例です。`05_parallel_fanout.cpp` は、1回の実行の中で分岐を動かす例です。過去のスループットとメモリの測定結果は、[性能の詳細解説](performance-deep-dive.md)にあります。これは、安全に扱えるセッション数の上限を保証するものではありません。

## 共有エンジンの規則

- 独立したセッションには、それぞれ別の `thread_id` を使います。同じIDで同時に実行すると、チェックポイントが書き込まれる順序は不定です。履歴の順序が重要な場合は、ホスト側で直列化してください。
- 設定用のセッターの呼び出しとツールのバインドは、エンジンを実行用または管理用のスレッドに公開する前に済ませます。実行中にワーカープールのサイズを変えることはできず、エラーになります。
- 1つのエンジンでは、管理操作と実行は同時に行えません。実行（run / resume）が動いている間は、状態や履歴の読み取り、更新、フォークが `std::logic_error` で拒否されます。管理操作が動いている間は、実行が拒否されます。管理操作をやり直す前に、キャンセルと処理のdrainを行い、完了を待ってください。
- ストアを共有する別々のエンジンは、このadmissionの境界の外にあります。ホスト側で調整してください。
- ノードのインスタンスは、複数の実行で使い回されます。実行ごとの一時的な状態はチャネルに置いてください。カスタムのノード、プロバイダー、ツール、ストアは、状態を持たないか、同期されている必要があります。同梱のインメモリストアは、ミューテックスを使います。

プロバイダー呼び出しは、準備済みのリクエストとランタイムのクライアントを自分で所有します。C++のイベントビューは、コールバックの間だけ有効です。保持したいデータはコピーしてください。ネイティブリプレイの状態と会計の権限には、本物の保管が必要で、ポータブルなJSONからの再構成では代わりになりません。詳しくは[非同期ガイド](ASYNC_GUIDE.md)を参照してください。

## 上限付きの同期admission

`RequestQueue` を使うには、`neograph::util` をリンクします。内部では `moodycamel::ConcurrentQueue` を使い、空いているワーカーは条件変数で待機します。待機スロットの上限が制限するのは、キューに入っているセッションの数です。実行中のセッションが消費するメモリは、制限しません。

```cpp
#include <neograph/util/request_queue.h>

neograph::util::RequestQueue queue(16, 1000);
auto [accepted, future] = queue.submit([engine, config] {
    auto result = engine->run(config);
    handle(result);
});
if (future.valid()) future.get();
if (!accepted) reject_request();
```

キューがいっぱいのときは、`accepted=false` と無効なfutureが返ります。内部でエンキューに失敗したときは、`false` と、`std::runtime_error` を保持する有効なfutureが返ります。拒否をすべて通常の飽和として扱わず、このfutureを確認してください。構築には、1つ以上のワーカーが必要です。

`close()` は冪等です。新しい投入を拒否し、すでに取り出された処理は完了させ、まだ取り出されていないfutureは、`std::runtime_error("RequestQueue is closed")` で完了させます。ワーカーがcloseを呼んだ場合は、自分自身の終了を待たずにシャットダウンを開始します。デストラクターも同じ経路を使います。受理されたfutureが、黙って取り残されることはありません。

## チェックポイントのI/OとPython

インメモリのチェックポイント操作は、ミューテックスの下で、呼び出し元のスレッド上で実行されます。SQLiteと、同期のカスタムバックエンドは、ブロックする処理を、上限のあるワーカーに移します。PostgreSQLは、ノンブロッキングのlibpq I/Oを使い、パイプラインのバッチ処理は行いません。`NEOGRAPH_BUILD_POSTGRES=ON` にすると、このオプションのターゲットが有効になり、libpqの開発用ファイルが必要になります。`OFF` にすると、このオプションの依存だけがなくなります。

Pythonのコールバックは、GILの下で実行されます。CPUを使うPythonのノードやリデューサーは、worker_countを増やしただけでは並列になりません。ネイティブの呼び出しが重なって動くのは、その実装自身がGILを解放する場合だけです。型付きプロバイダーのinvokeとdispatchはGILを解放するので、`asyncio.to_thread` 経由で呼び出せます。これは、ネイティブのプロバイダーのasyncio awaitableを公開するものではありません。

## ランタイム依存とプラットフォームの適格性確認

Coreは、`NEOGRAPH_BUILD_LLM=OFF` の場合も含め、常に外部の `SchemaProvider::runtime` をリンクします。PostgreSQL、LLMノード、NeoGraphのオプションのCurlH2Poolを無効にしても、このランタイムが必要とするlibcurlはなくなりません。対応するインストール済みのSDKか、`NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` を用意してください。

ソースの解決順は、明示したSDKのソースディレクトリ、インストール済みのパッケージ、リビジョンを固定した公開archiveのフォールバックの順です（`NEOGRAPH_FETCH_SCHEMAPROVIDER=ON` がデフォルト）。インストール済みのSDKを使ってオフラインでビルドするには、このフラグを `OFF` にし、`CMAKE_PREFIX_PATH` にSDKのプレフィックスを指定します。NeoGraphとSDKのソースの構成には、CMake 3.20以降が必要です。

現在のSDKのランタイムとarchiveは、Linux/POSIXで適格性確認が済んでいます。NeoGraphの既存のLinux/macOS/Windows向けパッケージメタデータは、この新しい依存がそれらすべてのプラットフォームで動く証拠にはなりません。macOS、Windows、WASMには、それぞれランタイムとビルドの適格性確認が必要です。ポータブルなexecutor APIがあるだけでは、確認したことにはなりません。
