<!-- neograph-i18n: source=docs/concurrency.md locale=ja source_sha256=889743688862b981a4a8e8d8de0c0f3bc287693712453d430183d4dcdd7a030a -->
# 並行実行と非同期

**Languages:** [English](concurrency.md) | [한국어](concurrency.ko.md) | [日本語](concurrency.ja.md) | [简体中文](concurrency.zh-CN.md)

## executor の選択

sync `run`、`run_stream`、`resume` は async API と同じ coroutine 実装を駆動し、呼び出し thread を占有する。host worker pool は独立 session を並行実行できる。async API は `asio::awaitable<RunResult>` を返すので、自分で所有する executor で駆動する。awaitable は任意の user code を nonblocking にしない。

`EngineConfig::worker_count = 1` が既定値で、engine 所有の fan-out pool はない。中断する I/O 分岐は一つの thread でも重なるが、CPU 処理は直列になる。multithread caller executor または任意の engine pool で CPU 分岐を複数 core に割り当てる。engine 公開前に pool を設定する。

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

`27_async_concurrent_runs.cpp` は一つの io_context の複数 session を、`05_parallel_fanout.cpp` は一回の run 内の分岐を示す。歴史的 throughput/memory 測定は[性能詳細](performance-deep-dive.md)にあり、安全な session 数上限を保証しない。

## shared engine の規則

- 独立 session は異なる `thread_id` を使う。同じ id の並行実行は checkpoint 順序が未規定なので、履歴順が必要なら host で直列化する。
- execution/admin thread に公開する前に setter と tool binding を済ませる。実行中の worker pool resize はエラーである。
- 一つの engine では管理と実行は相互排他的である。run/resume 中の state/history read、update、fork は `std::logic_error` で拒否し、管理中の実行も拒否する。cancel/drain して完了を待ってから管理を再試行する。
- 同じ store を使う別の engine はこの admission 境界の外にある。host で調整する。
- node instance は run 間で再利用する。run ごとの scratch state は channel に置き、custom node/provider/tool/store は stateless または同期化する。bundled in-memory store は mutex を使う。

Provider 呼び出しは prepared request と runtime client を所有する。C++ event view は callback 内だけ有効なので保持する data はコピーする。native replay と accounting 権限は本物の custody が必要で、portable JSON の復元では得られない。[非同期ガイド](ASYNC_GUIDE.md)を参照。

## bounded sync admission

`RequestQueue` は `neograph::util` を link して使う。`moodycamel::ConcurrentQueue` を使い、idle worker は condition variable で待機する。pending-slot 上限は queued session 数を制限し、実行中 session の memory を制限しない。

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

queue 満杯では `accepted=false` と invalid future を返す。内部 enqueue 失敗では `false` と `std::runtime_error` を持つ valid future を返す。すべての拒否を通常の飽和とせず future を確認する。worker は最低一つ必要である。

`close()` は冪等である。新しい submit を拒否し、claimed work は完了させ、unclaimed future は `std::runtime_error("RequestQueue is closed")` で完了する。worker が close を呼ぶと、自分を待たずに shutdown を始める。destructor も同じ経路を使い、accepted future を取り残さない。

## checkpoint I/O と Python

in-memory checkpoint は caller で mutex を使う。SQLite と sync custom backend は blocking work を bounded worker に移す。PostgreSQL は pipeline batching なしの nonblocking libpq I/O を使う。`NEOGRAPH_BUILD_POSTGRES=ON` は libpq 開発 files を必要とする optional target を有効にし、`OFF` はその依存だけを除く。

Python callback は GIL 下で実行する。CPU-bound Python node/reducer は worker_count を増やすだけでは並列にならない。native call は自分の実装が GIL を解放する場合だけ重なれる。typed provider invoke/dispatch は GIL を解放し、`asyncio.to_thread` で呼べる。native provider asyncio awaitable の公開ではない。

## runtime 依存と platform 検証

Core は `NEOGRAPH_BUILD_LLM=OFF` でも外部 `SchemaProvider::runtime` を link する。PostgreSQL、LLM node、NeoGraph の optional CurlH2Pool を無効にしても SDK runtime の libcurl 要件はなくならない。対応する installed SDK または `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` を指定する。

source resolution は明示 SDK source directory、installed package、revision-pinned 公開 archive fallback の順（既定 `NEOGRAPH_FETCH_SCHEMAPROVIDER=ON`）。installed SDK の offline build は flag を `OFF` とし `CMAKE_PREFIX_PATH` に prefix を指定する。NeoGraph と SDK source 設定は CMake 3.20+ を要する。

現在の SDK runtime/archive の検証範囲は Linux/POSIX である。既存 Linux/macOS/Windows package metadata は新依存がすべての platform で動く証拠ではない。macOS、Windows、WASM はそれぞれ runtime/build 検証を要し、portable executor API だけでは代替できない。
