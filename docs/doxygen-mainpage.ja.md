<!-- neograph-i18n: source=docs/doxygen-mainpage.md locale=ja source_sha256=8b62b237339d4244bee698f93443c1598e9ae55b631399b33d036606bab39d52 -->
# NeoGraph C++ API リファレンス {#mainpage}

**Languages:** [English](doxygen-mainpage.md) | [한국어](doxygen-mainpage.ko.md) | [日本語](doxygen-mainpage.ja.md) | [简体中文](doxygen-mainpage.zh-CN.md)

C++17 グラフ エージェント エンジン ライブラリ — LangGraph for C++ (オプションあり)
Python バインディング。このサイトは、**生成されたリファレンス**です。
`include/neograph/` のパブリック C++ ヘッダー。

## どこから始めるべきか

NeoGraph を初めて使用する場合は、**最初に説明ドキュメントをお読みください** — これ
生成された参照は、クラス シグネチャがわかったら検索するためのものです。
あなたが探しているもの。

| |の場合| に移動します。
|---|---|
| NeoGraph とは何か、なぜ、ベンチマーク | [README](https://github.com/fox1245/NeoGraph#readme) |
|メンタル モデル — チャネル、ノード、エッジ、送信、コマンド | [Core Concepts](https://github.com/fox1245/NeoGraph/blob/master/docs/concepts.md) |
|一般的な問題に対する症状優先の修正 | [Troubleshooting](https://github.com/fox1245/NeoGraph/blob/master/docs/troubleshooting.md) |
| 39 個の実行可能な C++ プログラム | [examples/](https://github.com/fox1245/NeoGraph/tree/master/examples) |
| 23 個の実行可能な Python プログラム | [bindings/python/examples/](https://github.com/fox1245/NeoGraph/tree/master/bindings/python/examples) |
|非同期/コルーチンの内部 | [ASYNC_GUIDE](https://github.com/fox1245/NeoGraph/blob/master/docs/ASYNC_GUIDE.md) |

## トップレベルのヘッダー

コンビニエンス ヘッダーは、完全なコア + グラフ エンジン API を取り込みます。

```cpp
#include <neograph/neograph.h>

using namespace neograph;
using namespace neograph::graph;
```

サブ名前空間:

- `neograph` — 基礎タイプ (`Provider`、`Tool`、`ChatMessage`)
- `neograph::graph` — エンジン、ノード、状態、チェックポイント設定
- `neograph::llm` — プロバイダー実装 (OpenAI、スキーマ駆動、エージェント ヘルパー)
- `neograph::mcp` — モデル コンテキスト プロトコル クライアント
- `neograph::async` — コルーチン + io_context インフラストラクチャ
- `neograph::util` — 同時実行プリミティブ

## 最初のプログラム

```cpp
#include <neograph/neograph.h>
#include <neograph/llm/mock_provider.h>

using namespace neograph;
using namespace neograph::graph;

int main() {
    json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes",    {{"echo",     {{"type", "llm_call"}}}}},
        {"edges",    json::array({
            {{"from", "__start__"}, {"to", "echo"}},
            {{"from", "echo"},      {"to", "__end__"}}})}
    };

    NodeContext ctx;
    ctx.provider = std::make_shared<llm::MockProvider>();
    auto engine = GraphEngine::build_strict(
        definition, EngineConfig{.node_context = std::move(ctx)});

    RunConfig cfg;
    cfg.thread_id = "demo";
    cfg.input["messages"] = json::array({{{"role","user"},{"content","hi"}}});
    auto result = engine->run(cfg);
    return 0;
}
```

実際に LLM を使用する場合は、`MockProvider` を `llm::OpenAIProvider` に交換するか、
`llm::SchemaProvider`。完全な `Provider` インターフェイスは次のとおりです。
`neograph::Provider`。

## 参照インデックス

サイドバーのクラスリスト、ファイルリスト、名前空間リストは
`include/neograph/` の下のヘッダーから生成されます。
[Class list](annotated.html) は最も便利なエントリ ポイントです。

## ソース

プロジェクトホーム: <https://github.com/fox1245/NeoGraph>

ライセンス: MIT。
