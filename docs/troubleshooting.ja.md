<!-- neograph-i18n: source=docs/troubleshooting.md locale=ja source_sha256=7f1eeda183c21890031bb9b18196a7377f140553c17f4362974f8d7341588208 -->
# トラブルシューティング

**Languages:** [English](troubleshooting.md) | [한국어](troubleshooting.ko.md) | [日本語](troubleshooting.ja.md) | [简体中文](troubleshooting.zh-CN.md)

## installed artifact の確認

`neograph_engine.__version__`、Python version、OS/architecture、install 方法、最初の error を記録する。checkout と旧 wheel が同じ API を持つと仮定せず、その release の wheel file を確認する。typed 移行は legacy provider 名を削除する。旧 wheel の import は新 binding の検査ではない。

公開 NeoGraph platform metadata は Linux/macOS/Windows を含む。現在の SchemaProvider runtime/archive 検証は Linux/POSIX。macOS/Windows port は新 wheel を使用可能と主張する前に別検証が必要。wheel tag/compiler だけでは runtime 統合を証明しない。WASM provider runtime は確立していない。

GLIBC symbol 不足では wheel の manylinux tag と host glibc を比較する。`manylinux_2_34` artifact は glibc 2.34 以降を要する。Windows DLL error では x64 Python と不足 dependency DLL を確認する。architecture だけが原因ではない。bundled library を個別交換しない。

## source 設定と不足依存

Core は `NEOGRAPH_BUILD_LLM=OFF` でも `SchemaProvider::runtime` を要する。NeoGraph 設定は CMake 3.20+。明示 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`、installed package、既定 revision-pinned 公開 archive fallback の順に解決する。offline 設定は対応 SDK を install し、`CMAKE_PREFIX_PATH` に prefix、`NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` を指定する。SDK source build は C++20、Python、standalone Asio、yyjson、libcurl 7.88+、OpenSSL Crypto が必要で、fetched/source 統合には NeoGraph 同梱 Asio/yyjson を使う。

SDK 単体は OpenSSL Crypto の最小 version を宣言しない。NeoGraph の full HTTPS 設定と wheel/sdist 経路は async/MCP HTTP 依存で OpenSSL 3 を要する。その build に Crypto header だけでは足りない。

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="$SDK_PREFIX" \
  -DNEOGRAPH_BUILD_PYBIND=ON
cmake --build build -j
```

`Could NOT find CURL` は `NEOGRAPH_USE_LIBCURL` 無効化では直らない。flag は NeoGraph の任意 CurlH2Pool を制御し、SDK 必須 transport ではない。libcurl 開発 file（Debian/Ubuntu は `libcurl4-openssl-dev`、Fedora は `libcurl-devel`）と一貫した toolchain/prefix を用意する。

SQLite/PostgreSQL は NeoGraph の optional component。開発 package を用意するか `NEOGRAPH_BUILD_SQLITE`/`NEOGRAPH_BUILD_POSTGRES` を明示的に無効にする。SDK runtime は除かれない。header/API 変更後に binding symbol が未解決なら fresh build directory で CMake を再設定し、対応 header/library で全 consumer を rebuild する。

GCC 13 coroutine ICE は Stage 3 で報告された。適切な compiler に更新するか対象式を変更する。旧 workaround は全 SDK/platform の検証ではない。catch handler 内で `co_await` しない。C++ 自体が禁止する。error を保存し handler の外で recovery を await する。

## 削除 provider API と typed failure

`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、descriptor-interpreter 設定、`prefer_libcurl`、Responses WebSocket 選択はなくなった。`SchemaProvider(load_provider_descriptor(descriptor_json), ProviderRuntimeOptions(...), SchemaProviderDefaults(...))` と `make_provider_request` を使う。constructor は [Python binding](python-binding.md)、C++ は[移行](migration-v0.4-to-v1.0.md)を参照。

`prepare` は single-use `PreparedProviderRequest` を返し `dispatch` が消費する。`invoke` は両段階をまとめる。dispatch receipt 保存後に admitted request を再構成したり consumed handle を再試行したりしない。failure outcome は本物の error/partial 根拠を持つ data。成功 completion として読まず `outcome.failure` を確認する。

observer exception は `ProviderObserverError` に typed outcome/cause を残す。budget settlement と terminal receipt persistence error も本物の outcome を保持するが、追加 send の権限にはならない。配送不明は blind retry でなく reconciliation が必要。

## streaming と Python async 境界

`ProviderMode.Stream` を明示する。observer は streaming を選ばない。token display は空でない content-text delta を選ぶ。usage、reasoning、tool、raw-wire、envelope は token ではない。C++ event view は callback 中の借用で、Python ProviderEvent は借用 data をコピーして所有する。遅い observer は host delivery capacity を消費する。

```python
import asyncio
import neograph_engine as ng

text = ng.Text()
text.value = "Hello"
request = ng.make_provider_request(
    provider, model, [ng.ProviderMessage(ng.ProviderRole.User, [text])],
    mode=ng.ProviderMode.Stream,
)
request.on_event = observe
outcome = await asyncio.to_thread(provider.invoke, request)
if outcome.failure is not None:
    handle_failure(outcome.failure)
else:
    handle_completion(outcome.completion)
```

Provider invoke/dispatch は GIL を解放する sync Python method。`asyncio.to_thread` を使い、native asyncio awaitable ではない。callback の呼び出し・copy・破棄は GIL を取得する。cancel は graph CancelToken で SDK に stop を渡す。timeout/cancel waiter は remote effect 不存在を証明しない。

continuation は tool/refusal/reasoning part を含む完全な `ProviderMessage` を保つ。`ChatMessage`/`ToolCall` は graph の便宜値で完全な provider history の alias ではない。usage 欠落は `None` で zero ではない。known-zero counter は根拠付き UsageCount。portable projection は native replay/financial authority を import できない。native continuation は admitted NativeArchive custody だけで保存する。

## TLS と local endpoint

明示 trust bundle は `ProviderRuntimeOptions.ca_file` に指定する。Python import は既存 `SSL_CERT_FILE` を保持し、なければ certifi があるとき選ぶ。runtime option は選んだ CA file を SDK libcurl に渡す。`NEOGRAPH_SKIP_CERT_AUTOFIX=1` は host 設定を変えない。endpoint/trust-store error を隠すため certificate verification を無効にしない。

v0.1.0–v0.1.6 wheel CA path の ConnPool timeout は歴史的問題で、v0.1.7 に CA 自動選択が追加された。現在の provider は旧経路でなく SDK libcurl を使う。local HTTP は raw URL override でなく validated descriptor の admitted data。[例 31](../examples/31_local_transformer.cpp)に従う。Responses WebSocket close=1000 手順は削除した旧 transport だけに適用する。

## graph 定義と登録

unknown reducer/condition/node-type は compile に使う registry に名前がないことを示す。built-in reducer は `overwrite`/`append`。custom reducer/condition/node factory は compile 前に登録する。`has_tool_calls`/`route_channel` は built-in condition。Python callback は GIL 下で実行する。

```python
import neograph_engine as ng

ng.ReducerRegistry.register_reducer("sum",
    lambda current, incoming: (current or 0) + incoming)
ng.ConditionRegistry.register_condition("is_long",
    lambda state: "long" if len(state.get("messages") or []) > 10 else "short")
```

write の channel 名は正確に存在する必要がある。condition は route label を返す。open condition は明示 `default` を使えるが、なければ unmatched label は throw し、closed condition は宣言外 label を拒否する。`__start__` edge と loop escape を確認する。`RunConfig.max_steps` 既定値は `50`。打ち切りを通常完了とせず step-limit status を確認する。

`schema_version: 1` の strict parsing は unknown/unconsumed key と round-trip loss を拒否する。metadata は `_`/`x-` annotation、barrier は非空 `wait_for`、conditional edge は `routes` を使う。custom 登録後 `ng.export_schema()` で live schema を出し、独自 editor palette を保守しない。absent/zero version は `0.x` で lenient、`ng.upgrade_topology()` は無視 data を衝突しない annotation に残す。

## fan-out と管理

既定 worker_count `1` は engine pool を作らない。I/O branch は suspend 時に重なり、単一 caller thread の CPU body は直列。pool が有効なら並行実行前に `set_worker_count(N)`/`set_worker_count_auto()` を設定する。native operation が GIL を解放しなければ Python CPU callback は直列のまま。

同じ engine の run/resume 中に admin state/history/update/fork を呼ぶと `std::logic_error`。cancel/drain して完了を待ち、例外を隠さない。同じ thread_id の並行実行は checkpoint 順序が未規定で、store を共有する別 engine は host 調整が必要。[並行実行](concurrency.md)を参照。

## checkpoint と PostgreSQL failure

PostgresCheckpointStore export 不足は wheel/source build が component を無効にした可能性がある。旧 wheel feature 一覧でなくその artifact 設定を見る。optional target build に対応 libpq を install する。URI password の特殊文字は percent-encode または libpq key=value 形式を使い、本物の credential を公開しない。

async connect/reconnect は全 host/IP で一 deadline。明示的な正 `connect_timeout=N` は秒で、`1` は二秒に切り上げる。absent/zero/negative、および PGCONNECT_TIMEOUT/service file だけの値は async 既定 30 秒。sync libpq の host ごとの timeout と異なる。

権限があれば store は table を作る。CREATE 権限がなければ [PostgreSQL header](../include/neograph/graph/postgres_checkpoint.h) の schema を適用する。pending-write capability 不足は super-step 全体の replay で、外部効果 exactly-once ではない。async-only backend は AsyncCheckpointStore と `adapt_async_checkpoint_store` を使い、旧相互 sync/async crossover は使わない。

## tracing adapter と歴史的修正

Tracer adapter は session close で壊れる Span wrapper への raw pointer でなく記録 data を所有する。[C++ tracing 例](../examples/49_openinference.cpp)を参照。Python contextvars は C++ callback 境界を自動で越えない。parent context を明示的に渡す/attach し、engine compile 前に wrapper を設置する。

歴史的修正は v0.1.8 の top-level conditional_edges 受理、node 二重実行 fallback 削除、移行前 httplib macro layout 不一致（issue #16）を含む。削除 provider signature を復活させない。複数 translation unit で header-only httplib を使う consumer は CPPHTTPLIB_OPENSSL_SUPPORT を一貫して定義する。現在の typed-provider HTTP は SDK libcurl。

opaque convenience vector property は Python list とは限らない。iteration/`list(value)` で確認し、ChatMessage.image_urls のような copy sequence は構築後に代入する。返された copy の変更が C++ request を変えると仮定しない。新 typed request/outcome は旧 CompletionParams 例と別。

## 安全な bug report

version/platform、最小 topology/call、execution_trace/status、機密を除いた typed failure/stop/usage 根拠を提供する。installed wheel か rebuilt checkout かを記す。credential、private prompt、encoded native request body、未除去 packet capture を含めない。<https://github.com/fox1245/NeoGraph/issues>へ報告する。
