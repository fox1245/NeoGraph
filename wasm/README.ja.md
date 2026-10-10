<!-- neograph-i18n: source=wasm/README.md locale=ja source_sha256=000ed65af24d7a4009772586f8895758fc873841f13515a94b4ab0cfd16866b7 -->
# NeoGraph WASM smoke プログラム

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`smoke.cpp` は `DoubleNode` 一つのグラフをコンパイルし、`doubled = seed * 2` を書き込み、`InMemoryCheckpointStore` で実行します。`seed = 21` の期待出力には `doubled = 42` と `trace = d` が含まれます。ネットワークやモデル呼び出しはありません。同じ生成ターゲットを Node.js または以下のブラウザー harness で実行できますが、ブラウザー SDK ではありません。

## 現在のビルド境界

型付き provider 移行後は `NEOGRAPH_BUILD_LLM=OFF` でも Core が `SchemaProvider::runtime` に依存します。
CMake 3.20+ は明示 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`、インストール済み runtime、固定 public GitHub source archive の順に SDK を選びます。
`NEOGRAPH_FETCH_SCHEMAPROVIDER` の download fallback は既定 ON で、offline package/source ビルドでは OFF にしてください。
Core は SDK runtime にリンクし、libcurl transport にはリンクしません。調査した SDK pin の最上位 CMake は runtime ターゲットが CURL にリンクしなくても構成時に CURL を要求します。OpenSSL は runtime の直接依存ではありません。native SDK library を WebAssembly にリンクすることはできません。Emscripten 向け runtime/archive と C++ 標準ライブラリーの対応を検証する必要があり、以下の過去の smoke は現在のビルド成功の証拠ではありません。

現在のビルドには同じ target 向け SDK `0.3.0`、interface revision/shared generation 6、マージ済み SchemaProvider PR #16（`3b88e4ba020c3a4d39ff0660014e7292b516b7cb`）が必要です。真正な Emscripten SDK runtime の検証は依然として前提条件です。本書は WASM 対応の削除も現在のビルドの保証もしません。Archive v3 / `spna3` と portable JSON v2 は不変です。

### 対応ターゲットの要件

Emscripten ツールチェーンと C++ 標準ライブラリーは、`std::bit_cast` と `std::stop_token` を含む SDK の C++20 契約をサポートする必要があります。native archive custody にはターゲットで対応する atomic no-replace filesystem backend が必要で、ビルドのために custody を無効化してはいけません。

新しいビルドディレクトリーと真正な同一 target の依存を使用してください。SDK 構成で必要な CURL 依存も含みます。native library や偽装した依存の可用性では WebAssembly ビルドを検証できません。現在の Node.js とブラウザー実行は未検証です。

## 過去の結果

値は型付き SDK 移行前の過去の smoke 実行によるものです。生成された `.wasm`/`.js` はコミットせず、CI も WASM サイズ artifact を公開していません。サイズは当時のビルドの値であり、現在の配布コストではありません。

| 指標 | 値 |
|---|---|
| WASM binary (-O3 + LTO) | 712 KB |
| Emscripten JS runtime | 92 KB |
| JavaScript + WASM 合計 | ~800 KB |
| 当時のエンジンソース変更 | 0 lines |
| 初回出力 | `doubled = 42, trace = d` |

## ターゲットと前提条件

ターゲットは `neograph_core` に直接リンクし、Core のソース一覧、C++20 coroutine、exception、Emscripten pthread を使います。CMake は distro Emscripten 3.1.x の Asio coroutine を有効にし、WASM ターゲットだけで未対応の stack-protector symbol を無効にします。native hardening は変わりません。ターゲットは `PTHREAD_POOL_SIZE=4`、グラフは既定の `worker_count=1` を使用します。単一スレッド WASM 用の CMake オプションはありません。

動作する Emscripten SDK runtime ビルドが用意できた後の configure/build/run 手順は次のとおりです。

```bash
# For an emsdk install only: source /opt/emsdk/emsdk_env.sh
# A distro installation with emcmake/em++ on PATH needs no emsdk script.

emcmake cmake -S . -B build-wasm \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR="$SCHEMAPROVIDER_SOURCE_DIR" \
  -DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF \
  -DNEOGRAPH_BUILD_WASM=ON \
  -DNEOGRAPH_BUILD_ASYNC=OFF \
  -DNEOGRAPH_BUILD_LLM=OFF \
  -DNEOGRAPH_BUILD_MCP=OFF \
  -DNEOGRAPH_BUILD_MCP_CLIENT=OFF \
  -DNEOGRAPH_BUILD_MCP_SERVER=OFF \
  -DNEOGRAPH_BUILD_MCP_HTTP_SERVER=OFF \
  -DNEOGRAPH_BUILD_A2A=OFF \
  -DNEOGRAPH_BUILD_ACP=OFF \
  -DNEOGRAPH_BUILD_GRPC=OFF \
  -DNEOGRAPH_BUILD_POSTGRES=OFF \
  -DNEOGRAPH_BUILD_SQLITE=OFF \
  -DNEOGRAPH_BUILD_UTIL=OFF \
  -DNEOGRAPH_BUILD_EXAMPLES=OFF \
  -DNEOGRAPH_BUILD_TESTS=OFF \
  -DNEOGRAPH_BUILD_BENCHMARKS=OFF \
  -DNEOGRAPH_USE_LIBCURL=OFF
cmake --build build-wasm --target neograph_wasm_smoke -j
node build-wasm/wasm/smoke.js
```

`NEOGRAPH_USE_LIBCURL=OFF` は NeoGraph のオプション HTTP/2 backend を無効にするだけです。SDK 最上位の CURL 検出は除去しません。真正な Emscripten 依存が必要で、現在検証済みのビルド手順ではありません。

生成 loader が filesystem path を `fetch` に渡す Node.js で使った過去の回避手順は次のとおりです。

```bash
node -e 'const fs=require("fs"); WebAssembly.instantiateStreaming=undefined; global.fetch=async p=>({ok:true,arrayBuffer:async()=>fs.promises.readFile(p)}); require("./build-wasm/wasm/smoke.js");'
```


## ブラウザー smoke

真正なターゲットのビルド後、標準ライブラリーのみの loopback サーバーで JS、WASM、生成 pthread worker asset を配信します。

```sh
python3 wasm/serve_smoke.py build-wasm/wasm --port 8765
# Open http://127.0.0.1:8765/smoke.html in a browser.
```

サーバーは `Cross-Origin-Opener-Policy: same-origin` と `Cross-Origin-Embedder-Policy: require-corp` を送信します。harness は cross-origin isolation / `SharedArrayBuffer` がなければ実行を拒否し、代替 runtime ではなく実際に生成された `smoke.js` を読み込みます。合格には出力 `doubled = 42`、`trace = d`、終了コード 0 が必要です。`document.body.dataset.result === "pass"` と `dataset.exitCode === "0"` に結果を公開します。ブラウザー/バージョン、console エラー、観測した出力を記録してください。Node.js の成功だけではブラウザーの証拠になりません。

この harness は npm package、Embind API、JS graph callback、provider transport を提供しません。hosted model access、local inference、NeoProtocol Executor 統合は、このモデルなし smoke では検証されません。
