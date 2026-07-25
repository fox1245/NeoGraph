<!-- neograph-i18n: source=wasm/README.md locale=ja source_sha256=9126f4bfed65128d8b676e182ae6e0b13f5d544de24e73018ce2ba8d44fb8093 -->
# NeoGraph WASM — 実現可能性の急上昇

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

WebAssembly にコンパイルされたグラフ エンジン。このディレクトリは
**フェーズ 1 スパイク** — エンジン層 (コンパイル、実行、エグゼキュータ、
スケジューラ、コーディネーター、状態、チャネル、NodeCache) を構築して実行します
Emscripten では未修正。

## 結果

|メトリック |値 |
|---|---|
| WASM バイナリ (-O3 + LTO) | **712 KB** |
| Emscripten JS ランタイム | 92KB |
|船の合計サイズ | **~800 KB** |
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

## 建てる

```bash
source /opt/emsdk/emsdk_env.sh

em++ -std=c++20 -O3 -flto -fexceptions -pthread \
  -sALLOW_MEMORY_GROWTH=1 -sPTHREAD_POOL_SIZE=4 \
  -DASIO_STANDALONE -DASIO_NO_DEPRECATED \
  -I include -I deps/asio/include -I deps/yyjson \
  wasm/smoke.cpp \
  src/core/json.cpp deps/yyjson/yyjson.c \
  src/core/graph_engine.cpp src/core/graph_compiler.cpp \
  src/core/graph_validator.cpp src/core/tool_dispatch.cpp \
  src/core/graph_coordinator.cpp src/core/graph_executor.cpp \
  src/core/scheduler.cpp src/core/graph_state.cpp \
  src/core/graph_node.cpp src/core/graph_loader.cpp \
  src/core/graph_checkpoint.cpp src/core/store.cpp \
  src/core/provider.cpp src/core/tool.cpp \
  src/core/react_graph.cpp src/core/plan_execute_graph.cpp \
  src/core/deep_research_graph.cpp src/core/node_cache.cpp \
  -o wasm/smoke.js
```

`node wasm/smoke.js` で実行します。ブラウザーのフラグは必要ありません。

`compile()` はデフォルトをプロビジョニングするため、`-pthread` が必要です
thread_pool のサイズは `hardware_concurrency()` です。シングルスレッド WASM は
も可能です - `-sPTHREAD_POOL_SIZE=0` を渡して呼び出します
`run()` の前の `engine->set_worker_count(1)`。

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
