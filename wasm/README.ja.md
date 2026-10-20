<!-- neograph-i18n: source=wasm/README.md locale=ja source_sha256=677a943827a8ac2e6d20cd23d1b1071ad0c3cade39c959acfc68762b7cad4abc -->
# NeoGraphのWASMスモークプログラム

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`smoke.cpp` は、`DoubleNode` が1つだけのグラフをコンパイルし、`InMemoryCheckpointStore` を使って実行します。ノードは `doubled = seed * 2` を書き込みます。`seed = 21` のときの期待される出力には、`doubled = 42` と `trace = d` が含まれます。ネットワークやモデルの呼び出しは行いません。生成されるターゲットは同じで、Node.jsでも、下のブラウザー用ハーネスでも実行できます。ブラウザー向けのSDKではありません。

## 現在のビルドの前提と限界

`NEOGRAPH_BUILD_LLM=OFF` にしても、Coreは `SchemaProvider::runtime` に依存します。CMake 3.20以降は、明示した `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`、インストール済みのランタイム、リビジョンを固定した公開GitHubのソースアーカイブの順に選びます。ダウンロードへのフォールバックは、`NEOGRAPH_FETCH_SCHEMAPROVIDER` によってデフォルトでONです。パッケージやソースを使ってオフラインでビルドするときは、OFFにしてください。
CoreがリンクするのはSDKのランタイムで、そのlibcurlトランスポートはリンクしません。確認したSDKの固定リビジョンでは、ランタイムのターゲットがCURLをリンクしないにもかかわらず、SDKのトップレベルのCMakeが、構成時にCURLを要求します。OpenSSLは、ランタイムが直接依存するものではありません。
ネイティブのSDKライブラリーを、WebAssemblyにリンクすることはできません。Emscriptenのターゲットでは、SDKのランタイム、アーカイブのサポート、C++標準ライブラリーが動作する必要があります。下の過去のスモークテストは、現在のコードがビルドできることを示すものではありません。

現在のビルドには、同じターゲット向けにビルドした、SDK `0.4.0`（インターフェースのリビジョンと共有世代は7、`cmake/NeoGraphSchemaProvider.cmake` が固定するSchemaProviderのコミット）が必要です（WebAssemblyの作業は#535で追跡しています）。本物のEmscripten向けのSDKランタイムのビルドが前提条件であり、このページは、現在のビルドが成功することを保証するものではありません。

### サポート対象のターゲットの要件

Emscriptenのツールチェーンと、C++標準ライブラリーが、SDKのC++20の契約（`std::bit_cast` と `std::stop_token` を含む）をサポートしている必要があります。ネイティブアーカイブの保管（custody）には、ターゲットでサポートされている、上書きしないアトミックなファイルシステムのバックエンドが必要です。ビルドを通すために、保管を無効にしてはいけません。

ビルドディレクトリーは新しく作り、同じターゲット向けの本物の依存を使ってください。SDKの構成時に必要になるCURLの依存も同様です。ネイティブのライブラリーや、依存があるかのように見せかけたものでは、有効なWebAssemblyのビルドにはなりません。Node.jsとブラウザーでの現在の実行は、まだ確認していません。

## 過去の結果

以下の値は、型付きSDKへの切り替え前に行った、以前のスモークテストで得たものです。生成された `.wasm` や `.js` はコミットしておらず、CIもWASMのサイズの成果物を公開していません。サイズは当時のビルドのものです。現在のデプロイに必要なコストではありません。

| 指標 | 値 |
|---|---|
| WASMバイナリー（-O3＋LTO） | 712 KB |
| EmscriptenのJSランタイム | 92 KB |
| JavaScript＋WASMの合計 | 約800 KB |
| この実行でのエンジンのソースの差分 | 0行 |
| 初回の出力 | `doubled = 42, trace = d` |

## ターゲットと前提条件

このターゲットは、`neograph_core` に直接リンクします。Coreの主要なソースリスト、C++20のコルーチン、例外、Emscriptenのpthreadsを使います。CMakeは、ディストリビューション版のEmscripten 3.1.xに対して、Asioのコルーチン機能を有効にします。また、サポートされていないスタックプロテクターのシンボルを無効にするのは、WASMのターゲットだけで、ネイティブのハードニングは変わりません。ターゲットは `PTHREAD_POOL_SIZE=4` を設定していますが、グラフはデフォルトの `worker_count=1` で動きます。ここには、シングルスレッド版のWASMを作るためのCMakeオプションはありません。

Emscripten向けのSDKランタイムのビルドが用意できたら、このターゲットの構成、ビルド、実行の手順は、次のとおりです。

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

`NEOGRAPH_USE_LIBCURL=OFF` で無効になるのは、NeoGraphのオプションのHTTP/2バックエンドだけです。SDKのトップレベルで行うCURLの検出は、なくなりません。この手順にも、本物のEmscripten向けの依存が必要です。現在のビルドを検証した手順ではありません。

生成されたローダーがファイルシステムのパスを `fetch` に渡すバージョンのNode.jsでは、過去に次の回避策を使いました。

```bash
node -e 'const fs=require("fs"); WebAssembly.instantiateStreaming=undefined; global.fetch=async p=>({ok:true,arrayBuffer:async()=>fs.promises.readFile(p)}); require("./build-wasm/wasm/smoke.js");'
```

## ブラウザーでのスモークテスト

本物のターゲットがビルドできたら、標準ライブラリーだけで動くループバックのサーバーで、JS、WASM、生成されたpthreadのワーカー用アセットを配信します。

```sh
python3 wasm/serve_smoke.py build-wasm/wasm --port 8765
# Open http://127.0.0.1:8765/smoke.html in a browser.
```

サーバーは `Cross-Origin-Opener-Policy: same-origin` と `Cross-Origin-Embedder-Policy: require-corp` を送ります。ハーネスは、クロスオリジン分離と `SharedArrayBuffer` が使えないと、実行を拒否します。読み込むのは、実際に生成された `smoke.js` で、代わりのランタイムではありません。合格の条件は、出力が `doubled = 42` と `trace = d` で、終了コードが0であることです。これは、`document.body.dataset.result === "pass"` と `dataset.exitCode === "0"` として公開されます。ブラウザーの種類とバージョン、コンソールのエラー、実際の出力を記録してください。Node.jsでの実行が成功しただけでは、ブラウザーでの動作のエビデンスにはなりません。

このハーネスは、npmパッケージ、Embind API、JSによるグラフのコールバック、プロバイダーのトランスポートを提供しません。ホスト型モデルへのアクセス、ローカル推論、NeoProtocol Executorとの統合については、モデルを使わないこのスモークテストの対象外です。
