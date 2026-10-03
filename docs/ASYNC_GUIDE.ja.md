<!-- neograph-i18n: source=docs/ASYNC_GUIDE.md locale=ja source_sha256=3d01320c4796b2b8fae399c9660352bcb3eaadefa460b54c1e15664cb04bd537 -->
# NeoGraph 非同期ガイド


> Stage 3 (2026-04) の設計と当時の測定テスト数は歴史として保持します。provider 互換/crossover の決定は以下の typed lossless 移行に置き換わり、旧設計台帳は現在の provider API ではありません。

Stage 3 / 2026-04 リリース。対象読者: 既存の NeoGraph コードを非同期 API に
移行するユーザー、または非同期 API に対して新規コードを書くユーザー。

本ガイドは **何が** 変わったか、**なぜ** その形状なのか、**どうやって**
段階的に移行するかをカバーします。個々の学期の設計根拠については
[`ASYNC_STAGE3_DESIGN.md`](ASYNC_STAGE3_DESIGN.md) を参照。分単位の
コミット台帳については `feat/async-api` ブランチの git log を参照。

---

## 1. 新機能

エンジン内のすべての同期 I/O ポイントに awaitable な対応版が追加されました:

| 層 | 同期 | 非同期 |
|---|---|---|
| Provider | `invoke` / `dispatch` | `invoke_async` / `dispatch_async` |
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

旧 Stage 3 報告は既存 276+ テストが当時の sync 経路を通過したと記録しています。現移行の検証結果ではありません。

---

<a id="2-the-crossover-default-pattern"></a>
## 2. 準備済み provider dispatch (crossover 削除)

公開契約は所有 typed 準備/dispatch であり、同期・非同期の virtual completion 対ではありません。`ProviderRequest.payload` は Chat、Messages、Responses、Gemini、Interactions の SDK リクエスト variant です。`ProviderMode::Collect` / `Stream` は観測者の有無と独立に転送を選択します。`on_event` は借用 typed `sp::Event` view を受け取ります。コールバック後に必要なデータだけコピーします。raw JSON override や portable projection による native 権限のインポートは認めません。

`prepare()` は検証とエンコードを正確に一度行い、元の deadline とキャンセル状態を持つ移動専用 `PreparedProviderRequest` を生成します。永続呼び出し元は `Provider::request_digest()` を assembly に結び付け、承認された budget claim を予約し、dispatch receipt を記録してから、同じハンドルを `ControlledProvider::dispatch_prepared(_async)` で消費します。gate 後の再生成はありません。重複 receipt は再送しません。カスタム実装は `get_name()`、`family()`、`prepare()` を実装し `prepare_runtime()` または `prepare_local()` を使います。local callback は `this` ではなく所有 shared 状態をキャプチャします。

ソースとバイナリの破壊的変更です。全 C++ 利用者とカスタムプロバイダーを新しい一致したヘッダー/ライブラリで再コンパイルします。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、descriptor interpreter、Responses WebSocket は alias/互換 bridge なしで削除されました。SDK は不安定 `0.0.0`、interface revision 3 / shared ABI 3、out-of-line capability check を使用し、安定リリースの宣言ではありません。現 runtime/archive は Linux/POSIX で、Windows・macOS・WASM runtime の資格検証を意味しません。Python provider binding/wrapper は延期され、この C++ 変更では移植されません。

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

`prepare()` は検証とエンコードを正確に一度行い、元の deadline とキャンセル状態を持つ移動専用 `PreparedProviderRequest` を生成します。永続呼び出し元は `Provider::request_digest()` を assembly に結び付け、承認された budget claim を予約し、dispatch receipt を記録してから、同じハンドルを `ControlledProvider::dispatch_prepared(_async)` で消費します。gate 後の再生成はありません。重複 receipt は再送しません。カスタム実装は `get_name()`、`family()`、`prepare()` を実装し `prepare_runtime()` または `prepare_local()` を使います。local callback は `this` ではなく所有 shared 状態をキャプチャします。

```cpp
#include <neograph/provider.h>
#include <neograph/runtime_interposition_consumer.h>
#include <runtime/client.h>

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
    neograph::PreparedProviderRequest
    prepare(neograph::ProviderRequest request) override {
        return prepare_runtime(client_, std::move(request));
    }
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
#include <neograph/graph/node.h>
#include <neograph/graph/run_context.h>
#include <neograph/provider.h>
#include <neograph/runtime_interposition_consumer.h>

class ChatNode : public neograph::graph::GraphNode,
                 public neograph::RuntimeInterpositionConsumer {
    std::shared_ptr<neograph::Provider> provider_;
    std::string model_;
public:
    ChatNode(std::shared_ptr<neograph::Provider> provider, std::string model)
        : provider_(std::move(provider)), model_(std::move(model)) {}
    asio::awaitable<neograph::graph::NodeOutput>
    run(neograph::graph::NodeInput in) override {
        auto request = neograph::make_provider_request(
            *provider_, model_, in.state.get_provider_messages());
        request.cancel_token = in.ctx.cancel_token;
        request.options.deadline = in.ctx.deadline;
        auto result = co_await neograph::graph::observe_provider_result(
            in.ctx, invoke_provider(provider_, std::move(request), {}, {},
                neograph::graph::provider_call_broker(in.ctx),
                neograph::graph::make_provider_call_identity(in.ctx, get_name())));
        neograph::graph::record_usage(in.ctx, result);
        neograph::outcome_or_throw(result);
        neograph::graph::NodeOutput out;
        out.writes.push_back(neograph::graph::provider_messages_write(result));
        co_return out;
    }
    std::string get_name() const override { return "chat"; }
};
```

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
旧来の二重実行フォールバックはもはや存在しません。

### 4.5 MCP stdio 単一セッションの並行性

単一の stdio トランスポートが、自身の `io_context`、子プロセスのパイプ、
書き込みセマフォ、および応答 ID を振り分けるリーダーを所有します。
**同じ** セッションの `rpc_call_async` はフレームの書き込み中だけロックを
共有します。読み取りは並行して待機でき、リーダーは JSON-RPC ID によって
各応答を対応する呼び出しへ渡します。呼び出し元は別々の executor を利用でき、
連続した `run_sync` グラフ実行でも同じセッションを使用できます。
一つの呼び出しのキャンセルやタイムアウトは共有子プロセスを閉じず、遅れて
届いた応答が別の呼び出しに誤配送されることもありません。

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

公開契約は所有 typed 準備/dispatch であり、同期・非同期の virtual completion 対ではありません。`ProviderRequest.payload` は Chat、Messages、Responses、Gemini、Interactions の SDK リクエスト variant です。`ProviderMode::Collect` / `Stream` は観測者の有無と独立に転送を選択します。`on_event` は借用 typed `sp::Event` view を受け取ります。コールバック後に必要なデータだけコピーします。raw JSON override や portable projection による native 権限のインポートは認めません。

### 9.1 2 分バージョン

| 作成するもの | オーバーライド | そのまま継承 |
|---|---|---|
| 任意のカスタム `GraphNode` | `run(NodeInput)` | 他に必要な仮想メソッドは `get_name()` のみ |
| Provider | `get_name()`, `family()`, `prepare(ProviderRequest)` | `invoke(_async)`, `dispatch(_async)` |
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

公開契約は所有 typed 準備/dispatch であり、同期・非同期の virtual completion 対ではありません。`ProviderRequest.payload` は Chat、Messages、Responses、Gemini、Interactions の SDK リクエスト variant です。`ProviderMode::Collect` / `Stream` は観測者の有無と独立に転送を選択します。`on_event` は借用 typed `sp::Event` view を受け取ります。コールバック後に必要なデータだけコピーします。raw JSON override や portable projection による native 権限のインポートは認めません。

`prepare()` は検証とエンコードを正確に一度行い、元の deadline とキャンセル状態を持つ移動専用 `PreparedProviderRequest` を生成します。永続呼び出し元は `Provider::request_digest()` を assembly に結び付け、承認された budget claim を予約し、dispatch receipt を記録してから、同じハンドルを `ControlledProvider::dispatch_prepared(_async)` で消費します。gate 後の再生成はありません。重複 receipt は再送しません。カスタム実装は `get_name()`、`family()`、`prepare()` を実装し `prepare_runtime()` または `prepare_local()` を使います。local callback は `this` ではなく所有 shared 状態をキャプチャします。

任意の `ProviderControls` は呼び出し元の選択であり、強制デフォルトや黙った cap clamp ではありません。非対応 family 制御は dispatch 前に拒否します。有界呼び出しには承認された実際のモデル input/output 上限が必要で、欠落は `LimitUnknown` です。予約は保守的な支出権限であり、報告使用量・予測・請求書ではありません。不明/部分/delivery-unknown の結果は hold を維持し、実際の最終報告で精算し、超過報告も全量を計上します。retry は明示的な単一層で、既定 off、有界 window と unknown-prior hold を使います。隠れた再送はありません。


プロバイダー呼び出しは `sp::runtime::Result`、すなわち `sp::Completion` または `sp::Failure` を保持する不変の所有 `std::shared_ptr<const sp::Outcome>` を返します。表示テキストだけでなく結果全体を保持してください。順序付きメッセージ/パート、native continuation、完全な wire envelope、順序付き raw 観測、停止の根拠と実際の試行メタデータは呼び出しとクライアント破棄後も残ります。使用量は根拠・段階・品質付きの nullable `uint64_t` であり、欠落はゼロではなく不明です。失敗も元の部分結果を保持します。`ProviderFailure::outcome()` と `ProviderObserverError::outcome()` は実際の結果を保持し、後者の `cause()` は観測者の例外を保持します。

実結果の後に post-effect 精算や terminal receipt 永続化が失敗すると、`ProviderDispatchOutcomePersistenceError::outcome()` は元の不変結果、`cause()` は元の永続例外を保持します。delivery も失敗した場合は `delivery_error()` が元の観測者例外を保持します。永続化成功後の観測者失敗は元の例外を変更せず再送出し、不明/結果なし transport 失敗では outcome を捏造しません。
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
