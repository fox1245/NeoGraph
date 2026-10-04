<!-- neograph-i18n: source=wasm/README.md locale=ja source_sha256=998352566107627e9cf22b256b5d1e2648275aa3c8ad5bd60d513c6fc59a7314 -->
# NeoGraph WASM smoke プログラム

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`smoke.cpp` は `DoubleNode` 一つのグラフをコンパイルし、`doubled = seed * 2` を書き込み、`InMemoryCheckpointStore` で実行します。`seed = 21` の期待出力には `doubled = 42` と `trace = d` が含まれます。ネットワークやモデル呼び出しはありません。ブラウザー SDK ではなく Node.js smoke ターゲットです。

## 現在のビルド境界

型付き provider 移行後は `NEOGRAPH_BUILD_LLM=OFF` でも Core が `SchemaProvider::runtime` に依存します。
CMake 3.20+ は明示 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`、インストール済み runtime、固定 public GitHub source archive の順に SDK を選びます。
`NEOGRAPH_FETCH_SCHEMAPROVIDER` の download fallback は既定 ON で、offline package/source ビルドでは OFF にしてください。
SDK runtime は libcurl、OpenSSL、threads も必要とします。現在の transport と archive にはプラットフォーム固有の依存があり、この worktree では Emscripten SDK runtime ビルドを検証していません。native SDK を WebAssembly にリンクすることはできません。以下の過去の smoke は現在のソースが Emscripten でビルドできる証拠ではありません。

NeoGraph `0.13.0` には同じ target 向けの alpha SDK `0.1.0`、interface revision/shared generation 4 が必要です。真正な Emscripten SDK runtime の検証は依然として前提条件です。本書は WASM 対応の削除も現在のビルドの保証もしません。Archive v3 / `spna3` と portable JSON v2 は不変です。

## 過去の結果

値は型付き SDK 移行前のローカル smoke 実行によるものです。生成された `.wasm`/`.js` はコミットせず、CI も WASM サイズ artifact を公開していません。サイズは当時のビルドの値であり、現在の配布コストではありません。

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
source /opt/emsdk/emsdk_env.sh

emcmake cmake -S . -B build-wasm \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR="$SCHEMAPROVIDER_SOURCE_DIR" \
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

`NEOGRAPH_USE_LIBCURL=OFF` は NeoGraph のオプション HTTP/2 backend を無効にするだけで、SDK runtime の libcurl 依存は無効にしません。現在検証済みのビルド手順ではありません。

生成 loader が filesystem path を `fetch` に渡す Node.js で使った過去の回避手順は次のとおりです。

```bash
node -e 'const fs=require("fs"); WebAssembly.instantiateStreaming=undefined; global.fetch=async p=>({ok:true,arrayBuffer:async()=>fs.promises.readFile(p)}); require("./build-wasm/wasm/smoke.js");'
```


## ブラウザーの状況と提案作業

このリポジトリに browser loader、npm package、Embind API はありません。browser pthread ビルドには cross-origin isolation header（`Cross-Origin-Opener-Policy: same-origin`、`Cross-Origin-Embedder-Policy: require-corp`）と生成 worker asset が必要です。これらだけで SDK runtime がブラウザー対応になるわけではありません。

ブラウザーポートには先に SDK 対応 transport/archive 設計が必要で、その後 JS node callback と packaging を実装する必要があります。提案されている `fetch()` adapter、hosted model access、local browser inference、NeoProtocol Executor 統合は、この smoke プログラムでは提供も検証もしていません。
