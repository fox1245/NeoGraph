<!-- neograph-i18n: source=wasm/README.md locale=ja source_sha256=4e62534e14c092fd8f32cd6e52366b4f1ae493d69de08a965bff04e45f5165ff -->
# WebAssembly 上の NeoGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

このディレクトリには、CI がビルドして実行する WebAssembly プログラムがあります。`smoke.cpp` は `DoubleNode` 1 つだけのグラフを
コンパイルし、`doubled = seed * 2` を書き込み、`InMemoryCheckpointStore` で実行します。`seed = 21` なら
`doubled = 42` と `trace = d` を出力します。`checks.cpp` は、スレッドのない 32 ビットホストで変わる点（下記）を
確認します。どちらもネットワークやモデルの呼び出しは行いません。今のところ WebAssembly ホストで動くのはグラフエンジンです。
モデル呼び出し、Program、ストアはまだ動きません。計画は [#535](https://github.com/fox1245/NeoGraph/issues/535) にあり、
これはブラウザー向け SDK ではありません。

## 現状

| 項目 | 状態 |
|---|---|
| ツールチェーン | Emscripten 6.0.13。CI では emsdk のリリースコミットに固定しています。 |
| ビルドモード | シングルスレッド（WebAssembly の既定）と pthread。どちらも CI でビルドして実行します。 |
| ランタイム | CI の Node.js 22 と 24。ブラウザー、Cloudflare Workers、Deno、WASI は未検証です。 |
| SDK | SchemaProvider 0.4.0、インターフェースのリビジョン 7。`cmake/NeoGraphSchemaProvider.cmake` が固定するコミットを使います。libcurl もスレッドも使わずにビルドされます。 |
| コンポーネント | Core、および任意で Program。 |

## ビルド

ビルドには Emscripten 6.0.13（C++20 のコルーチン、`<stop_token>`、`std::bit_cast` が libc++ にあります）、CMake 3.20 以上、
Ninja が必要です。LLM、ASYNC、MCP クライアント、A2A、gRPC、SQLite と PostgreSQL のストアは、WebAssembly ホストが提供しない
ソケット、TLS、システムライブラリを必要とするため CMake が拒否します。以下ではオフにしています。

シングルスレッド（WebAssembly の既定）:

```bash
source /path/to/emsdk/emsdk_env.sh

emcmake cmake -S . -B build-wasm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_WASM=ON \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_ASYNC=OFF -DNEOGRAPH_BUILD_LLM=OFF -DNEOGRAPH_BUILD_MCP=OFF \
  -DNEOGRAPH_BUILD_MCP_CLIENT=OFF -DNEOGRAPH_BUILD_MCP_SERVER=OFF -DNEOGRAPH_BUILD_MCP_HTTP_SERVER=OFF \
  -DNEOGRAPH_BUILD_A2A=OFF -DNEOGRAPH_BUILD_ACP=OFF -DNEOGRAPH_BUILD_GRPC=OFF \
  -DNEOGRAPH_BUILD_POSTGRES=OFF -DNEOGRAPH_BUILD_SQLITE=OFF -DNEOGRAPH_BUILD_UTIL=OFF \
  -DNEOGRAPH_BUILD_EXAMPLES=OFF -DNEOGRAPH_BUILD_TESTS=OFF -DNEOGRAPH_BUILD_BENCHMARKS=OFF \
  -DNEOGRAPH_USE_LIBCURL=OFF
cmake --build build-wasm --target neograph_wasm_smoke neograph_wasm_checks
node build-wasm/wasm/smoke.js
node build-wasm/wasm/checks.js
```

pthread: configure の行に `-DNEOGRAPH_THREADS=ON` を足し、別のビルドディレクトリを使います。
`NEOGRAPH_BUILD_PROGRAM=ON` が必要なのはネイティブプラグインのチェックだけです。グラフ smoke だけをビルドするならオフにしてください。

2 つのモードを切り替えるスイッチは `NEOGRAPH_THREADS` だけです。Emscripten と WASI では `OFF`、それ以外では `ON` です。

- **シングルスレッド（`OFF`）。** `-pthread` も `Threads::Threads` もありません。`asio` ターゲットが `ASIO_DISABLE_THREADS` を
  エクスポートし、SDK も同じ方法でビルドされます（`SP_THREADS=OFF`、libcurl トランスポートなし）。処理はホストが
  提供する executor で実行されます。
- **pthread（`ON`）。** すべてに `-pthread` を付け、smoke プログラムにはワーカー 4 つのプールを置きます。SDK は
  `SP_THREADS=ON` でビルドされ、WebAssembly では libcurl トランスポートは引き続きありません。
- **例外。** `-fexceptions`（JavaScript ベースの例外）を、C ソースを含むすべての NeoGraph ターゲットに設定し、SDK も同じフラグを
  使います。すでに `-fexceptions` または `-fwasm-exceptions` を指定した設定はそのままにし、例外が無効なら configure が
  失敗します。
- **プログラムごとに設定は 1 つ。** Asio はヘッダーオンリーなので、スレッドの設定が食い違う翻訳単位は、何も検出してくれない
  ODR 違反になります。生成される `neograph/build_config.h` が設定を記録し、すべての公開ヘッダーが `neograph/api.h` を通して
  このヘッダーに届き、設定が食い違う翻訳単位はコンパイルされません。SDK も自分の設定を同じ方法で記録しており
  （`sp/build_config.h`）、両者が違えば configure が失敗します。

## 測定したサイズ

Release、`-O3 -flto`、Emscripten 6.0.13。グラフ smoke には Core エンジンと SDK ランタイムが含まれます。

| ビルド | `.wasm` | `.wasm` gzip -9 | `.js` |
|---|---:|---:|---:|
| グラフ smoke、シングルスレッド | 5,484,652 B (5.2 MiB) | 1,601,090 B (1.5 MiB) | 83,223 B |
| グラフ smoke、pthread | 5,432,156 B (5.2 MiB) | 1,655,480 B (1.6 MiB) | 103,691 B |
| Program を含む checks、シングルスレッド | 5,017,721 B (4.8 MiB) | 1,554,991 B (1.5 MiB) | 83,031 B |

CI はシングルスレッドの smoke を `.wasm` 6,000,000 バイト、gzip 後 1,750,000 バイト（測定したサイズに約 9% を足した値）で制限し、サイズをジョブのサマリーに報告します。
このページの以前の版にあった 712 KB は、型付き SDK より前に測った値で、今は当てはまりません。

## これらのホストで使えないもの

NeoGraph が呼び出しを所有している場合は型付きのエラーで失敗し、そうでなければビルドされません。

- **モデル呼び出し。** LLM、ASYNC、MCP クライアント、A2A にはソケットと TLS が必要です。ホストの `fetch` を通すモデル呼び出しは
  #535 の M2 で入ります。
- **Program。** Program コンポーネントはコンパイルできますが、`ProgramRuntime` がスレッドを起動するため、シングルスレッドの
  ビルドでは、その生成、ノードの attempt タイムアウト、`n > 1` の `set_worker_count(n)` が `thread: Not supported` で
  失敗します（#557）。ホスト上の Program ストアは #556 で扱います。
- **ネイティブ Program コントロール。** C v1 プラグイン ABI は 64 ビットのレイアウトなので、wasm32 では
  `NativeControlBinding::create` が `NativeControlUnavailable` で拒否します。
- **同期のチェックポイントストア。** 非同期でないストアは、各操作をワーカースレッドのプールで実行します。
  シングルスレッドのビルドにはプールがないため、スレッドを待たずに `SynchronousCheckpointStoreUnavailable` で操作を
  拒否します。`AsyncCheckpointStore` を実装してください。組み込みのインメモリストアは、チェックポイントについては
  非同期ですが managed 予算の操作についてはそうではないため、ストアが awaitable になるまで（#556）、
  `model_token_budget` を指定した実行も拒否されます。
- **期限付きの managed 予算。** 期限は、再起動後も回復できるクロックの原点に結び付けられますが、WebAssembly ホストには
  それがありません。実行は、ストアに問い合わせる前に `ManagedBudgetDeadlineClockUnavailable` で拒否されます。
- **ネイティブアーカイブ。** ファイルの保管（custody）バックエンドがなく、SDK は `Unsupported` エラーで拒否します。
- **サブプロセスへの委譲**（Harness CLI、stdio の MCP サーバー）、**待ち受けソケット**、**gRPC**、**ファイルパスによるアップロード**、**Python**。

## ブラウザー

ブラウザー用のハーネスはツリーにありません。`serve_smoke.py` は存在しない `smoke.html` を前提としており、ブラウザーでの
実行記録もありません（#435）。Node.js での実行は、ブラウザーでの動作の証拠にはなりません。

## マイルストーン

ツールチェーン調査後の計画は [#535](https://github.com/fox1245/NeoGraph/issues/535) にあります。

- **M0 と M1（このページ）。** wasm32 の修正、シングルスレッドビルド、CI での Node smoke とサイズゲート。
- **M2.** ホストの `fetch` を通すモデル呼び出し。ブラウザーと Workers で。
- **M3.** ホストのストア上の Program（[#557](https://github.com/fox1245/NeoGraph/issues/557)、
  [#556](https://github.com/fox1245/NeoGraph/issues/556)）。
- **M4.** リアルタイムセッション、WASI、ネイティブアーカイブ用の保管バックエンド、リモート Harness。
