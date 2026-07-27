<!-- neograph-i18n: source=docs/concurrency.md locale=ja source_sha256=53d5843f1b9147b72df94827ed3d1463c1a60be53f9edff281267ba5b82653f8 -->
# 同時実行性と非同期性

**Languages:** [English](concurrency.md) | [한국어](concurrency.ko.md) | [日本語](concurrency.ja.md) | [简体中文](concurrency.zh-CN.md)

NeoGraph は、すぐに使用できる 2 つの同時実行モデルをサポートしています。
ホスティング パターンに適合するもの:

* **エージェントごとのスレッド (同期)** — `run()` / `run_stream()` / `resume()`
  すでに使用しているエグゼキュータにディスパッチされます。およそ 1 秒までは安全
  千人の同時エージェント。 1 回の呼び出しあたり最大 5 μs のエンジン オーバーヘッド
  `-O3 -DNDEBUG` ビルドをリリース (スーパーステップ ループは
  `run_sync(execute_graph_async)` なので、両方のエントリ ポイントが 1 つを共有します
  コルーチン パス)。
* **コルーチンベースの非同期** — `run_async()` / `run_stream_async()` /
  `resume_async()` は `asio::awaitable<RunResult>` を返します。 1つ
  `asio::io_context` は、何千もの同時エージェントをホストします。
  実行ごとのスレッド。すべてのプロバイダー/MCP/チェックポイント I/O ポイントは
  ボンネットの下にはノンブロッキングの`co_await`が入っています。完全な移行ガイドは次のとおりです。
  [`ASYNC_GUIDE.md`](ASYNC_GUIDE.md)。

## 非同期 (ステージ 3)

```cpp
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

asio::io_context io;
for (const auto& user : users) {
    asio::co_spawn(
        io,
        [&, user]() -> asio::awaitable<void> {
            RunConfig cfg;
            cfg.thread_id = user.session_id;
            cfg.input     = {{"messages", user.history}};
            auto result = co_await engine->run_async(cfg);
            handle(result);
        },
        asio::detached);
}
io.run();  // drives all agents on this thread
```

`engine->run_async()` は呼び出し元のエグゼキューターにエンドツーエンドで留まります —
すべてのスーパーステップの一時停止ポイント (ノードのディスパッチ、チェックポイント I/O、
並列ファンアウト、再試行バックオフ）は実際の `co_await` です。 3人
したがって、上記の 50 ミリ秒のステップは 1 つの io_context スレッドに重複し、
ウォールタイムは 3 × 50 ミリ秒ではなく、約 50 ミリ秒に達します。 1 つのスレッド、N 個の同時実行
エージェント。コア全体にわたる CPU バウンドのファンアウトの場合は、ドライバーを
共有 `asio::thread_pool` — それが次のパターンです
[`benchmarks/concurrent/CONCURRENT.md`](../benchmarks/concurrent/CONCURRENT.md)
ここで、N = 10,000 は 52 ミリ秒で終了します。 1 回の実行内で、
`make_parallel_group` ファンアウトも重複: 3 つの並列ファンアウト
研究者は連続 370 ミリ秒から 150 ミリ秒まで崩壊します。

カスタム ノードは、`asio::awaitable` を返すことで非同期パスに参加します。
統合 `run(NodeInput)` エントリ ポイントから (v0.4.0 で導入;
従来の 8 仮想チェーンは v0.9.0 で削除されました):

```cpp
class FetchNode : public GraphNode {
  public:
    asio::awaitable<NodeOutput>
    run(NodeInput in) override {
        auto ex = co_await asio::this_coro::executor;
        auto res = co_await neograph::async::async_post(ex, /*...*/);
        // in.ctx.cancel_token, in.state, in.stream_cb available.
        co_return NodeOutput{ {ChannelWrite{"out", res}} };
    }
    std::string get_name() const override { return "fetch"; }
};
```

非同期形状のツールは `AsyncTool` から派生します。

```cpp
class FetchTool : public neograph::AsyncTool {
  public:
    asio::awaitable<std::string>
    execute_async(const json& args) override { /* co_await HTTP */ }
    // sync execute() is final, routes through run_sync automatically.
};
```

マルチエージェントについては、`examples/27_async_concurrent_runs.cpp` を参照してください。
パターンと `examples/05_parallel_fanout.cpp` 内のファンアウト用
1回の実行。

## 同期 (エージェントごとのスレッド)

NeoGraph は独自の非同期ランタイムを同梱していません。同期ランタイムを公開しています。
`run()` / `run_stream()` / `resume()` と実行者を選択できます。
コンパイルされた単一の `GraphEngine` は、次のスレッド間で安全に共有できます。
`run()` を **別の `thread_id`** と同時に呼び出すため、ホスティング
マルチテナントエージェントのワークロードは、何にでもディスパッチするかどうかの問題です
すでに使用しているエグゼキュータ。

```cpp
// One engine, many concurrent sessions — no external runtime required.
EngineConfig engine_config;
engine_config.node_context = ctx;
engine_config.checkpoint_store = std::make_shared<InMemoryCheckpointStore>();
auto engine = GraphEngine::build(def, std::move(engine_config));

std::vector<std::future<RunResult>> sessions;
for (const auto& user : users) {
    sessions.push_back(std::async(std::launch::async, [&engine, user]() {
        RunConfig cfg;
        cfg.thread_id = user.session_id;
        cfg.input = {{"messages", user.history}};
        return engine->run(cfg);
    }));
}
for (auto& f : sessions) handle(f.get());
```

`std::async` をサポートする `asio::thread_pool` でも同様に機能します。
タスク システム、または Web フレームワークのワーカー プール — NeoGraph は除外されます
執行者の決定のこと。 CPU 並列ファンアウト *内部* が必要な場合
単一の同期 `run()` 呼び出し (N スレッド上の N 同期 `run()` ではなく)、
インストールする `build()` の前に `EngineConfig::worker_count` を設定してください
エンジン所有の `asio::thread_pool` と `run_parallel_async`
multi-ブランチディスパッチを送信します。

## バンドルされている`RequestQueue`を使用する

固定ワーカー プールが必要なマルチテナント サーバーの場合、
バックプレッシャー (キューが飽和した場合の新しいセッションの拒否)
無制限のメモリ増加の代わりに)、`neograph::util` をリンクして使用します
組み込みのロックフリーキュー — 外部エグゼキュータは必要ありません:

```cpp
#include <neograph/util/request_queue.h>
using namespace neograph::util;

RequestQueue pool(16, 1000);           // 16 workers, max 1000 pending sessions
EngineConfig engine_config;
engine_config.node_context = ctx;
engine_config.checkpoint_store = std::make_shared<InMemoryCheckpointStore>();
auto engine = GraphEngine::build(def, std::move(engine_config));

std::vector<RunResult>          results(users.size());
std::vector<std::future<void>>  futs;

for (size_t i = 0; i < users.size(); ++i) {
    auto [accepted, fut] = pool.submit([&, i]() {
        RunConfig cfg;
        cfg.thread_id = users[i].session_id;
        cfg.input     = {{"messages", users[i].history}};
        results[i]    = engine->run(cfg);
    });
    if (!accepted) {
        // Backpressure: queue is full — shed load, return 503, retry later, …
        reject(users[i]);
        continue;
    }
    futs.push_back(std::move(fut));
}

for (auto& f : futs) f.get();           // propagates exceptions from run()

auto s = pool.stats();
log("pending={} active={} completed={} rejected={}",
    s.pending, s.active, s.completed, s.rejected);
```

`submit()` は `{accepted, std::future<void>}` を返します。`RunResult` は共有出力
スロット（上記）またはタスクごとの `std::promise<RunResult>` で受け渡せます。
キューはロックフリーの `moodycamel::ConcurrentQueue` を使用し、アイドル中の
ワーカーは condvar で待機するため busy-spin しません。admission は pending
スロットを原子的に予約するため、同時呼び出しでも `max_queue_size` を超えません。
満杯による通常の backpressure は `{false, invalid_future}` を返します。内部の
enqueue 失敗は代わりに `{false, valid_future}` を返し、その future を監視すると
`std::runtime_error` が送出されます。

キューは 1 つ以上のワーカーで構築してください。`close()` は冪等で、以後の投入を
拒否し、ワーカーの終了を待ち、すでにワーカーが取得した callable は完了させ、未取得の
future はすべて `std::runtime_error("RequestQueue is closed")` で完了させます。
callable 自身が `close()` を呼んで終了を開始することもできますが、そのワーカーは
自分自身を待たずに戻ります。デストラクターも同じ close 経路を使うため、teardown 中に
受理済み future が暗黙に取り残されることはありません。

## 安全な同時使用のためのルール

- 構成ミューテーター (`set_retry_policy`、`set_checkpoint_store`、
  `set_store`、`own_tools`、…) は同時実行の **前** に呼び出す必要があります
  `run()`。最初のディスパッチの後、エンジンを凍結したものとして扱います。
- **同じ** `thread_id` を共有する同時 `run()` 呼び出しはクラッシュしない
  ただし、指定されていないチェックポイント インターリーブが生成されます。セッションごとにシリアル化する
  確定的な履歴が必要な場合は、自分自身にアクセスしてください。
- カスタム `GraphNode` サブクラスは **ステートレスまたは自己同期**である必要があります。
  ノード インスタンスはエンジンによって所有され、実行されるたびに再利用されます。
  すべてのスレッド - 実行ごとのスクラッチ データは、グラフ チャネルではなくグラフ チャネルに属します。
  ノードのメンバー変数。
- ユーザー指定の `CheckpointStore`、`Store`、`Provider`、および `Tool`
  実装はスレッドセーフである必要があります。バンドルされている`InMemoryCheckpointStore`
  と `InMemoryStore` はすでにそうです。

## PostgreSQL による永続的なチェックポイント設定

マルチプロセス展開の場合、またはチェックポイントが再起動後も存続する必要がある場合、
`neograph::postgres` をリンクし、`InMemoryCheckpointStore` を交換します
`PostgresCheckpointStore`:

```cpp
#include <neograph/graph/postgres_checkpoint.h>

auto store = std::make_shared<PostgresCheckpointStore>(
    "postgresql://user:pass@host:5432/dbname");
EngineConfig engine_config;
engine_config.node_context = ctx;
engine_config.checkpoint_store = store;
auto engine = GraphEngine::build(def, std::move(engine_config));
```

スキーマは、LangGraph の `PostgresSaver` (接頭辞が 3 つのテーブル) を反映しています。
`neograph_*` は同じデータベース内で LangGraph 状態と共存します)、および
`(thread_id, channel, version)` によってチャネル値の重複を排除します。あ
スーパーステップごとに 1 つのチャンネルに触れる 1000 ステップのセッションのおおよそのコスト
`O(steps × channels)` の代わりに `O(steps + channels)` BLOB 行。

**ビルド フラグ**: `-DNEOGRAPH_BUILD_POSTGRES=ON` (デフォルト)。必要
`libpq-dev` (apt) / `libpq-devel` (rpm)。フラグ `OFF` をスキップするように設定します。
完全に依存関係。

**統合テストの実行**: 使い捨てのローカル PG をスピンアップし、
テスト バイナリをそこに指定します。

```bash
docker run -d --rm --name neograph-pg-test \
    -e POSTGRES_PASSWORD=test -e POSTGRES_DB=neograph_test \
    -p 55432:5432 postgres:16-alpine

NEOGRAPH_TEST_POSTGRES_URL='postgresql://postgres:test@localhost:55432/neograph_test' \
    ctest --test-dir build -R PostgresCheckpoint --output-on-failure
```

環境変数がないと、PG テストは `GTEST_SKIP` されるため、残りの部分は
Postgres が手元にないマシンではスイートは緑色のままです。

対象範囲: `tests/test_graph_engine.cpp` に含まれるもの
`ConcurrentRunDifferentThreadIds` (16 スレッド × 25 実行 = 400 並列
実行、セッションごとの出力の検証 + チェックポイント分離)、および
`ConcurrentRunSameThreadIdNoCrash` (8 スレッド × 50 が 1 つの共有上で実行される)
`thread_id`、クラッシュのない動作を検証します)。
