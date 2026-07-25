<!-- neograph-i18n: source=docs/ASYNC_GUIDE.md locale=ja source_sha256=867535aebac0ee2f3fd44ffbb7b05c6f71b27535a3417f62efdd74cdc9aa14cf -->
# NeoGraph 非同期ガイド

**Languages:** [English](ASYNC_GUIDE.md) | [한국어](ASYNC_GUIDE.ko.md) | [日本語](ASYNC_GUIDE.ja.md) | [简体中文](ASYNC_GUIDE.zh-CN.md)

Stage 3 / 2026-04 リリース。対象読者: 既存の NeoGraph コードを非同期 API に
移行するユーザー、または非同期 API に対して新規コードを書くユーザー。

本ガイドは **何が** 変わったか、**なぜ** その形状なのか、**どうやって**
段階的に移行するかをカバーします。個々の学期の設計根拠については
[`ASYNC_STAGE3_DESIGN.md`](ASYNC_STAGE3_DESIGN.md) を参照。分単位の
コミット台帳については `feat/async-api` ブランチの git log を参照。

---

## 1. 新機能

エンジン内のすべての同期 I/O ポイントに awaitable な対応版が追加されました:

| 層 | 同期 (変更なし) | 非同期対応版 |
|---|---|---|
| Provider | `complete` / `complete_stream` | `complete_async` / `complete_stream_async` |
| CheckpointStore | `save` / `load_latest` / `load_by_id` / `list` / `delete_thread` / `put_writes` / `get_writes` / `clear_writes` | 各メソッドの `*_async` |
| GraphNode | — | `run(NodeInput) -> asio::awaitable<NodeOutput>` が唯一の正規オーバーライド |
| GraphEngine | `run` / `run_stream` / `resume` | `run_async` / `run_stream_async` / `resume_async` |
| MCPClient | `rpc_call` | `rpc_call_async` |
| Tool | `execute` (ユーザーインターフェース — 凍結) | `AsyncTool` アダプタでラップ |

非同期対応版は `asio::awaitable<T>` を返します。任意の `asio::io_context`
(strand、または `any_io_executor` を持つスレッドプール) 上で駆動します。
1 つの `io_context` が実行ごとに OS スレッドを専有することなく数千の同時
`run_async` 呼び出しをホストできます — このリファクタ全体の動機となった
並行モデルです。

同期インターフェースは維持されます。`engine->run(cfg)` や任意の
`provider->complete*` エントリポイントを呼び出す既存コードは引き続き
サポートされます。Stage 3 以前に存在した 276+ のテストケースは同期パスで
依然としてパスします。

---

## 2. クロスオーバーデフォルトパターン

Provider と永続化抽象に残るすべての同期/非同期ペアは、各方向をブリッジする
一対のデフォルト実装によって接続されています:

```cpp
class Provider {
  public:
    // Sync default: drive the async peer on a private io_context.
    virtual ChatCompletion complete(const CompletionParams& params);

    // Async default: co_return the sync peer (single-threaded on
    // the resuming coroutine).
    virtual asio::awaitable<ChatCompletion>
    complete_async(const CompletionParams& params);

    // ...
};
```

**契約: 2 つのうち少なくとも 1 つをオーバーライドすること。** どちらも
オーバーライドしない場合、どちらのメソッドを呼び出しても 2 つのデフォルト間で
無限再帰し、スタックオーバーフローに至ります。文書化済み。実行時ガードなし
(すべての実装者のすべての呼び出しを遅くするため)。

### どちら側をオーバーライドすべきか

| コードの形状 | オーバーライド |
|---|---|
| 実ノンブロッキング I/O を発行 (HTTP、MCP、DB、タイマー) | **非同期対応版** — 同期ファサードを継承 |
| 純粋な CPU 作業、または同期ライブラリで短時間ブロック | **同期対応版** — 非同期ブリッジを継承 |
| カスタム `GraphNode` | `run(NodeInput)` をオーバーライド。書き込み、`Command`、`Send` を 1 つの `NodeOutput` で返す |

### なぜ単一の統一 API ではないのか？

すべての公開抽象を非同期に集約すると、既存のすべての Tool とすべての
CheckpointStore サブクラスが非同期機構を認識することを強制されます —
2 つの数値を加算するだけのツールのような、何の利益もないケースを含みます。
クロスオーバーペアはこれらの抽象に対する移行コストゼロのパスであり続けます。
`GraphNode` は v1.0 で意図的に 1 つのコルーチンオーバーライドに集約されました。

---

## 3. 移行レシピ

### 3.1 同期呼出元の非同期移行

**Before:**

```cpp
auto result = engine->run_stream(config, event_cb);
```

**After:**

```cpp
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

asio::io_context io;
RunResult result;
asio::co_spawn(
    io,
    [&]() -> asio::awaitable<void> {
        result = co_await engine->run_stream_async(config, event_cb);
    },
    asio::detached);
io.run();
```

コルーチン完了時に `io.run()` が返ります。多数の同時実行では、
`io.run()` を呼ぶ前に同じ `io_context` に各実行を co_spawn します —
`examples/27_async_concurrent_runs.cpp` を参照。

### 3.2 新しい非同期プロバイダの作成

`CompletionProvider` から派生し、`do_invoke()` のみを実装します。
その final アダプタが既存のすべての `Provider` エントリポイントを動作させ続け、
`CompletionRequest` が collect モードと stream モードを明示的にします。

```cpp
class MyProvider : public CompletionProvider {
  public:
    asio::awaitable<ChatCompletion>
    do_invoke(CompletionRequest request) override {
        auto ex = co_await asio::this_coro::executor;
        const auto& params = request.params();
        auto res = co_await neograph::async::async_post(
            ex, host, port, path, body, headers, /*tls=*/true);
        if (request.streaming() && request.on_chunk()) {
            // Deliver parsed chunks through request.on_chunk().
        }
        co_return parse_response(res);
    }

    std::string get_name() const override { return "my-provider"; }
};
```

### 3.3 非同期 Tool の作成

`Tool` インターフェースは設計上同期です (Stage 3 は既存ユーザーツールの
移行コストをゼロ近くに保つため凍結)。内部でコルーチン形状の作業が必要な場合は
`AsyncTool` を使用:

```cpp
class FetchTool : public neograph::AsyncTool {
  public:
    ChatTool get_definition() const override { ... }
    std::string get_name() const override { return "fetch"; }

    asio::awaitable<std::string>
    execute_async(const json& args) override {
        auto ex = co_await asio::this_coro::executor;
        auto res = co_await neograph::async::async_post(
            ex, /*host*/, /*port*/, /*path*/, /*body*/);
        co_return res.body;
    }
};
```

`AsyncTool::execute` は `final` — `execute_async` を駆動するためにプライベート
`io_context` を生成する同期ファサードです。両方をオーバーライドすることは
契約違反です。

### 3.4 非同期プロバイダを使用するグラフノードの作成

```cpp
class MyNode : public GraphNode {
  public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        CompletionParams params = build_params(in.state);
        params.cancel_token = in.ctx.cancel_token;
        auto completion = co_await provider_->complete_async(params);

        neograph::json msg;
        to_json(msg, completion.message);
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"messages", json::array({msg})});
        co_return out;
    }

    std::string get_name() const override { return name_; }
  private:
    std::shared_ptr<Provider> provider_;
    std::string name_;
};
```

エンジンは同期と非同期のエントリポイントからこの同じコルーチンを駆動します。
`engine->run_async()` を使用すると、ノードは実行ごとに OS スレッドを専有することなく
io_context オーバーラップに参加します。

---

## 4. 注意点と落とし穴

### 4.1 GCC 13 コルーチン ICE

2 つの特定の C++20 コルーチン形状が GCC 13 の
`build_special_member_call` ICE を引き起こします (GCC 13.3 現在):

**形状 1 — `catch` ブロック内の `co_await`:**

```cpp
try { ... }
catch (const MyError& e) {
    co_await something();  // ICE
}
```

**回避策 — エラーを外部で捕捉し、後で処理:**

```cpp
std::optional<MyError> err;
std::optional<Result> ok;
try { ok.emplace(co_await op()); }
catch (const MyError& e) { err.emplace(e); }

if (err) {
    co_await recover();
    throw *err;
}
```

**形状 2 — コルーチン本体内のネストされた brace-init:**

```cpp
co_await fn(std::vector<std::string>{name},    // ICE
            json{{"key", "value"}});
```

**回避策 — 外部で構築し、参照渡し:**

```cpp
std::vector<std::string> v;
v.push_back(name);
json j;
j["key"] = "value";
co_await fn(v, j);
```

両方の形状が Stage 3 中に複数回表面化し、回避策は安定しています。
Clang 18+ と GCC 14+ は「自然な」形式を問題なくコンパイルしますが、
NeoGraph は GCC 13 をベースラインとしています。

### 4.2 `run_sync` ライフタイムハザード

`neograph::async::run_sync<T>(asio::awaitable<T>)` は呼び出しごとに
新しいシングルスレッド `io_context` を作成します。そのエグゼキュータに
バインドされた長寿命の asio ハンドル — プール内のソケット、タイマー、
ファイルディスクリプタ — は `run_sync` が返るとダングリングになります。
これは初期の ConnPool 作業で問題になり、現在のアーキテクチャは同期ファサード
を通じて意図的にプーリングしないことで回避しています。

ルール: 単一の呼び出しより長く生存する必要があるリソース (接続プール、
長時間実行ストリームディスクリプタ) は、プロセス生存期間中所有する
エグゼキュータにのみバインドすること。同期ファサードパスはリクエストごとに
新しい接続を作成します。

### 4.3 `co_return co_await x`、`return x` ではない

`asio::awaitable<T>` を返すコルーチン関数は、本体内のどこかで `co_return`
(または `co_await`) を使用する必要があります。プレーンな
`return other_awaitable()` はコンパイルされるように見えますが、実行時に
ラップされた `T` をデフォルト構築します。常に `co_return co_await` で
チェーンすること:

```cpp
asio::awaitable<RunResult>
GraphEngine::run_async(const RunConfig& config) {
    co_return co_await execute_graph_async(config, nullptr);
}
```

### 4.4 カスタムノードからのストリーミング

`GraphNode::run(NodeInput)` はディスパッチごとに 1 回実行されます。
`in.stream_cb` が非 null の場合のみイベントを送出し、呼出元がストリーミング
エンジンエントリポイントを使用したかどうかに関わらず同じ `NodeOutput` を返します。
レガシーな `execute_full_stream_async` の二重実行フォールバックはもはや存在しません。

### 4.5 MCP stdio 単一セッションの並行性

`StdioSession::rpc_call_async` は `std::mutex` で同時呼び出しを直列化します。
**同じ** シングルスレッド `io_context` 上で **同じ** セッションを呼び出す
2 つのコルーチンはデッドロックします — 2 番目のコルーチンの `lock_guard` が
1 番目の I/O 完了を駆動するために必要なワーカーをブロックするため。
典型的な使用法 (セッションごとに 1 つの論理呼出元、*異なる* セッション間での
非同期ファンアウト) は影響を受けません。awaitable-mutex 版は将来の作業として
追跡中。

---

## 5. パフォーマンスノート

非同期ワイヤは単一エージェントを高速化しません — `bench_neograph` は
Stage 3 以前と同じ seq (~30 µs) と par (~205 µs) の数値を報告します。
価値軸はエンジンレイテンシではなく **並行ロバスト性** です。

実形状ベンチマークでの測定改善:

* `bench_async_http --mode async_pool --concur 1000` — 17834 ops/s、
  vs. Stage 2 async (8401 ops/s) および sync (6064 ops/s)。
* `bench_async_fanout --concur 50000` — 541K ops/s、67 MB RSS。
  スレッド毎エージェントのベースラインは ~1000 同時を超えてスケールできず。
  50K は今や午後の作業です。
* `examples/27_async_concurrent_runs` — 1 io_context 上で 3 エージェント × 50ms 作業:
  50ms 合計 (vs. 150ms 逐次)。
* `examples/05_parallel_fanout` — 1 io_context 上で 3 並列研究者:
  150ms 合計 (vs. 370ms 逐次)。

### 同期 API を使い続けるべき場合

ワークロードが ≤ 1000 同時エージェントで、各エージェントが専用 OS スレッドで
動作する場合、同期 API は完全に合理的な選択肢であり続けます。その規模では
スレッドは十分安価で、同期コードの方が推論が簡単です。非同期 API は同期形状が
扱えないワークロードのために存在します — 数百の長時間実行エージェントが
プロセスを共有する、単一イベントループから多数のユーザーをホストする、など。

---

## 6. クリーンな移行のためのチェックリスト

- [ ] エージェントホストパターンを特定: 単一エージェントプロセス、プール、
      または共有イベントループ？
- [ ] 共有イベントループの場合 → 呼出サイトを `run_async` /
      `run_stream_async` に移行。
- [ ] カスタムノードは `run(NodeInput)` を実装し、実 I/O を直接 `co_await`。
- [ ] ツールが実 I/O を行う場合 → `AsyncTool` から派生し、
      `execute_async` をオーバーライド。
- [ ] Postgres チェックポイントストアを使用する場合 → 共有イベントループ上で
      その `*_async` メソッドを使用。libpq のノンブロッキングワイヤプロトコルと
      コルーチンフレンドリーな接続プールを使用。
- [ ] 測定する。価値軸は並行性。ワークロードが並行性バウンドでなければ
      移行しない。

---

## 7. まだカバーされていないもの

* **Postgres パイプラインモード** — 非同期チェックポイントメソッドは既に
  ノンブロッキング libpq I/O を使用しているが、まだ libpq パイプラインモードで
  複数コマンドをバッチ処理していない。
* **`async::HttpResponse` headers map** — レスポンスインターフェースは
  status / body / retry_after / location のみを公開。任意のヘッダアクセス
  (例: MCP セッション ID ヘッダ追跡) は Sem 1 のフォローアップ。

---

## 8. 3.0 での変更点

3.0 (`feat/taskflow-removal`) は Taskflow を削除し、同期エントリポイントを
`run_sync(execute_graph_async)` 経由でルーティングすることで同期と非同期を
1 つのコルーチンランタイムに集約しました。2.0 非同期 API 形状は変更なし —
違いはデフォルトと新しいオプトインにあります。

### 8.1 `GraphNode::run(NodeInput)` がレガシーオーバーライドチェーンを置換

v0.9.0 v1 準備リリースが 8 つの `execute*` 仮想メソッドを削除しました。
カスタムノードは同期・非同期エンジンエントリポイント、ストリーミング、
制御フローに対して 1 つのオーバーライドを持つようになりました:

```cpp
asio::awaitable<NodeOutput> run(NodeInput in) override {
    NodeOutput out;
    out.writes.push_back({"answer", co_await fetch_answer(in)});
    Command command;
    command.goto_node = "review";
    out.command = command;
    if (in.stream_cb) {
        (*in.stream_cb)({GraphEvent::Type::LLM_TOKEN, get_name(), json("done")});
    }
    co_return out;
}
```

以前のリリースから移行するコードは、状態読み取りを `in.state` に、
実行メタデータを `in.ctx` に、ストリーミングシンクを `in.stream_cb` に、
書き込み/`Command`/`Send` 値を返り値の `NodeOutput` に移動する必要があります。

### 8.2 `GraphEngine::set_worker_count(N)` — オプトイン CPU 並列ファンアウト

デフォルト: `run_parallel_async` と `run_sends_async` のマルチ Send 分岐は、
現在のコルーチンを駆動するエグゼキュータ上で分岐をディスパッチします。
同期 `run()` の場合、それはシングルスレッド io_context — I/O バウンド分岐は
co_await サスペンションを通じて依然オーバーラップしますが、CPU バウンド分岐は
直列化されます。

```cpp
EngineConfig engine_config;
engine_config.node_context = ctx;
engine_config.checkpoint_store = store;
engine_config.worker_count = std::thread::hardware_concurrency();
auto engine = GraphEngine::build(def, std::move(engine_config));
// Now run_parallel_async dispatches branches to an engine-owned
// asio::thread_pool of that size.
```

可能であれば構築前に設定すること。互換性セッターは任意の同時 `run()` の前に
呼び出す必要があります。実行中の実行をまたいでプールを再構築することは
安全ではありません。マルチスレッド `asio::thread_pool` を自ら駆動する
`run_async` 呼出元はこれを必要としません — 呼出元側のエグゼキュータが
既に分岐を並列化します。

### 8.3 `neograph::async::run_sync_pool(aw, n_threads)` — N ワーカー同期ブリッジ

```cpp
#include <neograph/async/run_sync.h>

int result = neograph::async::run_sync_pool(
    my_coroutine_that_uses_make_parallel_group(), /*n_threads=*/4);
```

既存のシングルスレッド `run_sync` のコンパニオン。呼び出し用に新しい
`asio::thread_pool` を生成し、内部の `make_parallel_group` 分岐が別々の
ワーカーで実行されるようにします。呼び出しごとのプール構築はワーカーごとに
1 つの `std::thread` を生成 — コストはホットパスでは無視できないため、
これは境界での時折の同期ブリッジ用であり、リクエストごとのコード用では
ありません。

### 8.4 削除されたインターフェース

- `NodeExecutor::run_one` / `run_parallel` / `run_sends` (同期) — `_async` 対応版を使用。
- `GraphEngine::execute_graph` (同期) — 削除。`run()` /
  `run_stream()` / `resume()` は `run_sync` 経由で非同期対応版にルーティング。
- `tf::Executor`、`tf::Taskflow`、`deps/taskflow/` ディレクトリ — 消滅。
  Taskflow を呼出元側ドライバとして使用していたベンチマーク
  (`bench_concurrent_neograph.cpp`) は `asio::thread_pool` + `asio::post` に切替。

---

## 9. オーバーライド判断ガイド

`GraphNode` は 1 つの正規オーバーライドを持つ。Provider と永続化
インターフェースは互換性のため別々の同期/非同期対応を保持。

### 9.1 2 分バージョン

| 作成するもの | オーバーライド | そのまま継承 |
|---|---|---|
| 任意のカスタム `GraphNode` | `run(NodeInput)` | 他に必要な仮想メソッドは `get_name()` のみ |
| 新しいカスタム LLM バックエンド | `CompletionProvider` を継承、`do_invoke()` をオーバーライド | 既存の全 `Provider` エントリポイントは final アダプタ |
| カスタム `CheckpointStore`、非同期対応バックエンド | 8 つすべての `*_async` 対応版 | 同期対応版は `run_sync` 経由でブリッジ |
| カスタム `CheckpointStore`、同期専用バックエンド | 8 つすべての同期対応版 | 非同期対応版は `run_sync` 経由でブリッジ |
| カスタム同期 `Tool` | `Tool` を継承、`execute()` をオーバーライド | — |
| カスタム非同期 `Tool` | `AsyncTool` を継承、`execute_async()` をオーバーライド | 同期 `execute()` は `final`、ブリッジ |

### 9.2 `GraphNode`

常に `run(NodeInput)` をオーバーライド。CPU 専用作業は `co_return` の前に
直接実行可能。実際の非同期 I/O は `co_await` する。エンジンは `run`、
`run_async`、ストリーミング、再開、Send ファンアウトから同じメソッドを
呼び出すため、オーバーライド選択マトリックスも同期/非同期フォールバック
再帰も存在しない。

共有シングルスレッド `io_context` を長時間ブロックしないこと。ブロッキング
作業はエグゼキュータに移動するか、コルーチンフレンドリーな I/O を使用。
`EngineConfig::worker_count` が並列ファンアウトを必要とする同期呼出元が
使用するエンジン所有プールを制御。

### 9.3 `Provider`

既存の `Provider` サブクラスは 4 つの同期/非同期 collect/stream 仮想メソッドを
使い続けてよい。それらは安定した互換性 API であり、削除計画も非推奨警告もない。
各ペアは依然として少なくとも 1 つのオーバーライドを必要とする:

| オーバーライド | 動作 |
|---|---|
| `complete()` のみ | 同期が直接動作。非同期 `complete_async` は基底クラスのデフォルト `co_return complete()` でブリッジ。CPU 専用モックプロバイダに十分。 |
| `complete_async()` のみ | 非同期が直接動作。同期 `complete` は `run_sync(complete_async())` でブリッジ。 |
| `complete_stream()` のみ | 同期ストリーミングが直接動作。非同期対応版はワーカースレッドで実行し、コールバックを待機中エグゼキュータに配信。 |
| `complete_stream_async()` のみ | ネイティブ非同期ストリーミングが直接動作。直接同期ストリーミング呼出がデフォルト collect フォールバックを避けなければならない場合は同期対応版も実装。 |

**新しい** バックエンドの場合、これらのペアから選択しないこと。
`CompletionProvider` から派生し、`do_invoke(CompletionRequest)` を実装し、
`request.streaming()` でトランスポートを選択。新しい直接呼出元は
`invoke_request()` を `CompletionRequest::collect(...)` または
`CompletionRequest::stream(...)` と共に使用すべき。互換性とセキュリティ修正は
古いエントリポイントにも継続適用されるが、新機能は明示的リクエスト専用の
可能性がある。

### 9.4 `CheckpointStore`

8 つの同期メソッド、8 つの非同期対応版、1:1 対応。出荷済みストア
(`InMemoryCheckpointStore`、`SqliteCheckpointStore`、
`PostgresCheckpointStore`) はすべて非同期側を実装し、同期側を
基底クラスのデフォルトでブリッジ。

- **非同期対応バックエンド** (libpq ノンブロッキング、非同期 MongoDB
  ドライバ等): 8 つすべての `*_async` 対応版をオーバーライド。同期呼出
  パスは呼出ごとに 1 つの `run_sync` を支払う — `get_state` /
  `update_state` 管理呼出には十分だがホットループでは不可
  (ただしエンジンは同期チェックポイントメソッドを決して呼ばない。
  ユーザーツールのみが行う)。
- **ブロッキング専用バックエンド** (古いファイル I/O、一部の ODBC ラッパー):
  8 つの同期メソッドをオーバーライド。非同期呼出元は各呼出で `run_sync`
  を通じてコルーチンスレッドをブロックするが、チェックポイント書き込みは
  ノードディスパッチに比べて頻度が低いため通常許容範囲。
- **混在させないこと**: `save()` をオーバーライドして `save_async()` を
  デフォルトのままにすると、非同期対応版は基底クラスのデフォルトを通じて
  同期に戻る — 正しいが非同期 I/O の利点を失う。インターフェースごとに
  全同期または全非同期で統一。

### 9.5 `MCPClient`

`rpc_call_async()` が実際の実装。`rpc_call()` は薄い
`run_sync(rpc_call_async(...))` ファサード。**ユーザー拡張不可** —
`MCPClient` はサブクラス化を意図しておらず、そのまま使用する。
カスタム MCP トランスポートが必要な場合は新しいクラスを書くこと。
継承しない。

HTTP リクエストは通常通りオーバーラップ。stdio 書き込みは短い書き込みロックの下で
JSON 行を完了し、単一のリーダーが JSON-RPC id で順序不同の応答を関連付ける。
したがって stdio 呼出もサブプロセスがリクエストを並行処理する場合に
オーバーラップする。逐次サブプロセスはスループットの下限のまま。

### 9.6 `Tool` vs `AsyncTool`

設計上非対称。クラス宣言時に一方を選択:

```cpp
class MyCpuTool : public Tool {
  public:
    std::string execute(const json& args) override { /* sync */ }
    ChatTool get_definition() const override { /* ... */ }
    std::string get_name() const override { return "cpu-tool"; }
};

class MyHttpTool : public AsyncTool {
  public:
    asio::awaitable<std::string> execute_async(const json& args) override {
        auto ex = co_await asio::this_coro::executor;
        auto r = co_await neograph::async::async_post(ex, /* ... */);
        co_return r.body;
    }
    // sync execute() is final and routes through run_sync automatically.
    ChatTool get_definition() const override { /* ... */ }
    std::string get_name() const override { return "http-tool"; }
};
```

両方から継承したり、1 つのクラスの両インターフェースをオーバーライドしようと
**しない** こと — `AsyncTool::execute` はまさにそれを防ぐために `final`。