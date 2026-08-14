<!-- neograph-i18n: source=wasm/README.md locale=ja source_sha256=19a1e742b349014d3c9115255a5966a692c37f11c52cdfc3f65a9e747a507a89 -->
# NeoGraph WASM — 実現可能性の急上昇

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

WebAssembly にコンパイルされたグラフ エンジン。このディレクトリは
**フェーズ 1 スパイク** — エンジン層 (コンパイル、実行、エグゼキュータ、
スケジューラ、コーディネーター、状態、チャネル、NodeCache) を構築して実行します
Emscripten では未修正。

## 過去の実行結果

|メトリック |値 |
|---|---|
| WASM バイナリ (-O3 + LTO) | **712 KB** |
| Emscripten JS ランタイム | 92KB |
| JavaScript + WASM の合計 | **~800 KB** |
|エンジンソースの差分 | 0 行 |
|最初の実行出力 | `doubled = 42, trace = d` ✓ |

比較のために: ネイティブ NG は合計 5.5 MB です。 LangGraph スタック
(langgraph + langchain + openai + httpx + pydantic + langsmith) は 31 MB
ブラウザへの出荷すら試みない純粋な Python です。 NG
L3 キャッシュ内に 2 倍以上収まり、一般的な SaaS に十分な大きさです。
ランディング ページには、このエンジンが必要とする量を超える JS がすでに読み込まれています。

## 今日実行される内容 (フェーズ 1)

- `GraphEngine::compile(json)` — JSON 定義 → 実行可能エンジン。
- `engine->run(cfg)` — InMemoryCheckpointStore との同期実行。
- `NodeFactory::register_type` 経由で登録されたカスタム ノード — リーフ
  セマンティクスは C++ / Python パスから引き継がれます。
- v0.1.6 のすべての機能はクリーンにコンパイルされます: `set_worker_count`、
  `set_node_cache_enabled`、リデューサー付きチャンネル、条件付きエッジ、
  ファンアウト、コマンドルーティング、割り込みを送信します。
- C++20 コルーチン (asio のヘッダーのみの `awaitable` 部分) は以下で動作します。
  エムスクリプテン5.0。

## 意図的にまだ出荷されていないもの

|サブシステム |延期される理由 |フェーズ |
|---|---|---|
| `neograph_async` (ASIO 経由の HTTP/WebSocket) |ブラウザは生のソケットではなく、`fetch` / ネイティブ WebSocket を使用します。 | 2 |
| `neograph_llm` (スキーマプロバイダー、OpenAIProvider) |上記の非同期トランスポートに依存します。 | 2 |
| `neograph_postgres` |ブラウザは関係ありません | — |
| `neograph_mcp` |サブプロセスベース、ブラウザは無関係 | — |
| JS バインディングを埋め込む | JS でノード実装をコールバックとして定義しましょう | 2-A |

## ビルドと実行

```bash
source /opt/emsdk/emsdk_env.sh

emcmake cmake -S . -B build-wasm \
  -DCMAKE_BUILD_TYPE=Release \
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

このターゲットは `neograph_core` を直接リンクするため、ソース一覧はこの文書に
複製せず、メインの CMake ビルドで管理します。成功すると `doubled = 42` と
ノードの実行トレースが出力されます。

`compile()` のデフォルトは `worker_count=1` なので、エンジン所有の
スレッドプールは作成されません。このスモークコマンドは、呼び出し側が
`set_worker_count(N >= 2)` で並列ファンアウトを選べるように Emscripten の
4 スレッドを有効にしますが、スモーク自体は 1 ワーカーのデフォルトを使います。
シングルスレッドビルドでは `-sPTHREAD_POOL_SIZE=0` を渡せばよく、
`set_worker_count(1)` の呼び出しは不要です。

## ブラウザー対応状況

現在、このリポジトリにはブラウザーローダー、npm パッケージ、Embind API はありません。
そのため、現在のターゲットは Node.js 専用です。Emscripten pthreads を使うブラウザー
ビルドには、クロスオリジン分離ヘッダー
(`Cross-Origin-Opener-Policy: same-origin`、`Cross-Origin-Embedder-Policy: require-corp`)
と生成されたワーカーアセットを配信する Web サーバーも必要です。

## フェーズ 2 スケッチ

1. **2-A — JS バインディングを埋め込みます。** `GraphEngine`、`RunConfig`、を公開します。
   `ChannelWrite`、`Send`、`Command` を JS に変換します。 JS関数が登録できる
   それ自体がノード実装として機能します。エンジンは JS を呼び出します。
   各ノードの実行。推定 1 ～ 2 日。

2. **2-B — フェッチベースの HTTP トランスポート。** トランスポートを提供します。
   `SchemaProvider` が使用するインターフェイス。 WASM ビルドがそれを接続します
   `fetch()`まで。同じプロバイダー コードがいずれかのバックエンドをターゲットにします。推定
   3～5日。

3. **2-C — npm パッケージ。** アプリができるように `@neograph/wasm` として公開します。
   `npm install` 独自のビルドを持たないエンジン + JS バインディング。
   推定 1 ～ 2 日。

フェーズ 2 の後、エンジンはオリジネーターが発行したグラフを完全に実行できます。
ブラウザ タブ — BYOK Anthropic / OpenAI / Bedrock キーを呼び出したままにします
`fetch()`、transformers.js / ローカル推論用の組み込み AI、および
結果はチャネルを通って結果エンベロープに戻ります。それが
ランタイム側
[NeoProtocol](https://github.com/fox1245/NeoProtocol) 実行者の役割。
