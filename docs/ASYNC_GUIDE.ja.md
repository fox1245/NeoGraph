<!-- neograph-i18n: source=docs/ASYNC_GUIDE.md locale=ja source_sha256=a48e529a151529d9711ee297aed8bc4b7e603c0198c596188608cb6a8565dc13 -->
# NeoGraph 非同期ガイド

**Languages:** [English](ASYNC_GUIDE.md) | [한국어](ASYNC_GUIDE.ko.md) | [日本語](ASYNC_GUIDE.ja.md) | [简体中文](ASYNC_GUIDE.zh-CN.md)

## 実行エントリポイント

`GraphEngine::run`、`run_stream`、`resume` は同期 bridge で coroutine 実装を駆動する。`run_async`、`run_stream_async`、`resume_async` は `asio::awaitable<RunResult>` を返し、呼び出し元の executor を使う。完了まで executor を駆動し、callback が使う資源を先に破棄しない。

`EngineConfig::worker_count` の既定値は `1` で、engine 所有の fan-out pool はない。I/O 分岐は中断時に重なって進むが、単一 executor thread の CPU 処理は直列になる。複数 core が必要なら engine を公開する前に pool を設定する。[並行実行](concurrency.md)と `examples/27_async_concurrent_runs.cpp` を参照。

## 準備済み provider 呼び出し

`SchemaProvider` は closed な `sp::descriptor::ValidatedDescriptor`、`sp::runtime::Options`、任意の typed `SchemaProvider::Defaults` から構築する。runtime は libcurl を使う。旧 descriptor interpreter、`prefer_libcurl` selector、Responses WebSocket 経路は削除された。

`make_provider_request` は family ごとの typed payload を作る。既定値は `ProviderMode::Collect` で、`Stream` は `on_event` と独立に指定する。`prepare` は一度検証・encode し、`dispatch(_async)` は move-only handle を一度消費する。`invoke(_async)` は両段階をまとめる。返された awaitable が request/client 状態を所有するため、元の request と Provider は scheduling 後まで生存する必要がない。

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

C++ `on_event` は借用 `sp::Event` view を受け取る。callback 後に必要な bytes はコピーする。event は text のほか usage、reasoning、tool、raw wire 観測も含む。不変 `sp::runtime::Result` は `sp::Completion` または部分失敗の根拠を含む `sp::Failure` を所有する。observer 失敗は `ProviderObserverError::outcome()` に結果、`cause()` に callback 例外を残す。

dispatch は capacity-one の合流通知を待ち、bounded Bridge が event と結果を保持する。通知自体は event queue ではない。cancel は SDK に stop を渡し、`operation.join()` は observer/resource 失敗時も callback の返却と admission slot 解放を待つ。deadline は操作を制限するが、呼び出し元の停止は server 未受信の証明にはならない。

永続 dispatch では `Provider::request_digest()` を assembly に結び付け、承認された claim を予約して receipt を保存し、同じ handle を `ControlledProvider::dispatch_prepared(_async)` で消費する。重複 receipt は再送の権限ではない。予約は承認済み支出権限を引き落とすか保留し、provider 報告 usage や invoice ではない。欠落 counter は unknown のままにし、部分/配送不明の根拠で未解決 hold を解除しない。

完全な message はメモリ内で本物の native continuation を保持する。portable JSON projection は観測値であり、native replay や財務権限ではない。native continuation の保存は承認された `sp::NativeArchive` だけを通し、text や raw JSON から復元しない。[移行ガイド](migration-v0.4-to-v1.0.md)を参照。

## custom provider と node

Provider subclass は `get_name`、`family`、`prepare` だけを実装する。共通 invoke/dispatch は virtual completion hook ではない。

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

C++ adapter は本物の local-dispatch 状態を所有するとき `prepare_local` を使える。借用 `this` ではなく所有 shared state を capture する。Python subclass は本物の provider に準備を委譲し、outcome や local-dispatch 権限を捏造できない。

custom graph node は `run(NodeInput) -> asio::awaitable<NodeOutput>` を override する。`in.state`、`in.ctx` を読み、非 null の `in.stream_cb` だけに通知し、一つの `NodeOutput` に writes/Command/Send を返す。sync/async streaming とも dispatch ごとに一度呼ぶ。完全な provider 履歴は `RunConfig::provider_messages` を使う。`ChatMessage` は graph の便宜型で、SDK message model ではない。

<a id="94-checkpointstore"></a>
## CheckpointStore adapter

`CheckpointStoreCore` は save、load_latest、load_by_id、list、delete_thread の五つの sync 操作を要求する。`adapt_checkpoint_store` は engine 契約を提供し、blocking 処理を bounded worker に移す。`AsyncCheckpointStore` は五つの async 操作を要求し、`adapt_async_checkpoint_store` は明示的な sync 管理 facade を提供する。

legacy `CheckpointStore` の sync 既定実装は `std::logic_error` で失敗し、async 既定実装は sync override を offload する。async-only subclass は明示的な async capability/adapter へ移行する。in-memory 操作は呼び出し元で mutex を使い、SQLite は blocking 処理を offload し、PostgreSQL は pipeline batching なしの nonblocking libpq I/O を使う。

pending-write の耐久性は別の `PendingWritesCheckpointStore` capability である。なければ resume は super-step 全体を再実行する。no-op method は外部効果を重複排除しない。Python checkpoint subclass は sync method を実装し、async-native backend は C++ adapter が担う。

<a id="95-mcpclient"></a>
## MCPClient

`rpc_call` は `rpc_call_async` を同期駆動する。MCPClient は custom transport の継承 interface ではない。HTTP 呼び出しは重なって進む。stdio session は frame write を直列化し、一つの reader が JSON-RPC id で reply を振り分ける。一つの waiter の cancel で shared transport を閉じない。直列 subprocess の throughput 上限は残る。

<a id="96-tool-vs-asynctool"></a>
## Tool と AsyncTool

sync Tool は `execute`、`get_definition`、`get_name` を実装する。AsyncTool は `execute_async`、`get_definition`、`get_name` を実装する。sync `execute` は final で、private `run_sync` context を駆動する。両実行 interface を override せず、shared event-loop thread を長い sync 処理で塞がない。

## Generic HTTP streaming

NeoGraph の `async_post_stream` は retained consumer が使う別の generic HTTP utility として残ります。SDK libcurl dispatch はこれを置き換えません。Chunked、有界 `Content-Length`、close-delimited response body を扱います。空でない fixed-length body は callback に一度渡し、長さゼロの body は callback を出しません。返される `HttpStreamResponse.status` は 200 と non-2xx status を保持し、caller は non-SSE reply を空の成功とせず JSON error body を解釈できます。

`RequestOptions` の既定制限は status/header 64 KiB、decoded body 16 MiB、transfer chunk 1 MiB です。各値ゼロでその制限を無効にします。Fixed-length 分岐は割り当て前に body limit を検査します。不正・曖昧な framing、premature EOF、既に buffer にある surplus byte は拒否します。返却後に到着する byte の検出を保証しません。Redirect body は別に扱います。既定 per-hop timeout はゼロで redirect は無効です。これらの generic option は SDK policy や model spending grant ではありません。

## Python と寿命境界

Python は typed `ProviderRequest`、`PreparedProviderRequest`、不変 `ProviderOutcome` を使う `prepare`、`dispatch`、`invoke` を公開する。invoke/dispatch は GIL を解放し、callback と Python object の破棄は GIL を取得する。asyncio 境界では `asyncio.to_thread(provider.invoke, request)` を使う。C++ provider awaitable を asyncio awaitable に自動変換しない。[Python binding](python-binding.md)を参照。

`run_sync` は呼び出しごとに private single-thread context を、`run_sync_pool` は呼び出しごとの pool を作る。その executor に結び付いた handle を外へ出さない。通常関数は別の awaitable を直接返せる。coroutine 本文では `co_return co_await` を使え、通常の `return` を混ぜない。

## build と歴史的根拠

外部 `SchemaProvider::runtime` は `NEOGRAPH_BUILD_LLM=OFF` でも `neograph::core` に必須である。installed SDK prefix または `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` を使う。SDK build は CMake 3.20+、C++20、Python、standalone Asio、yyjson、libcurl 7.88+、OpenSSL Crypto を要求する。記録された interface-4 より前の runtime/archive 検証は Linux/POSIX の範囲で、interface 4 の資格検証ではない。既存 macOS/Windows metadata は新依存の検証ではなく、WASM 統合も確立していない。

NeoGraph 設定は CMake 3.20+ を要する。明示 SDK source directory を優先し、なければ installed package、既定の revision-pinned 公開 archive fallback の順に解決する。installed SDK の offline 設定では `NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` とし、`CMAKE_PREFIX_PATH` に SDK prefix を指定する。fetched/source SDK は NeoGraph 同梱 Asio/yyjson を使うが system 開発依存は依然必要。

[Stage 3 設計](ASYNC_STAGE3_DESIGN.md)は 2026 年 4 月の提案記録である。2.0/3.0 の呼称、completion crossover、test/benchmark 数は歴史的 milestone で、現在の版・API・新しい pass 主張ではない。版は `pyproject.toml` から読み、歴史的測定は[性能詳細](performance-deep-dive.md)にある。

元の Stage 3 guide は当時の sync 経路の既存 test 276+、engine seq ~30 µs/par ~205 µs、HTTP async_pool 17834 ops/s（Stage 2 async 8401/s、sync 6064/s）、50000 timer fan-out 541K ops/s・RSS 67 MB を記録した。また 50 ms agent 三つが 50 ms、researcher 三つが 150 ms（直列 370 ms）で完了したと記録した。その workload の歴史的観測であり、現在の provider 移行結果ではない。
