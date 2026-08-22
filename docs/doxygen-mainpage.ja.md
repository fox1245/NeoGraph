<!-- neograph-i18n: source=docs/doxygen-mainpage.md locale=ja source_sha256=a2fa88f0d8ef08b3821d8ffe9810bc53b0ed7cd56251bd719b552628036a5b0d -->
# NeoGraph C++ APIリファレンス {#mainpage}

**Languages:** [English](doxygen-mainpage.md) | [한국어](doxygen-mainpage.ko.md) | [日本語](doxygen-mainpage.ja.md) | [简体中文](doxygen-mainpage.zh-CN.md)

C++20グラフエージェントエンジンライブラリ — C++向けのLangGraphで、オプションのPythonバインディング付きです。このサイトは`include/neograph/`内の公開C++ヘッダーに対する**生成されたリファレンス**です。

## 開始する場所

NeoGraphを初めてお使いの場合は、**まずナラティブドキュメントをお読みください** — この生成されたリファレンスは、探しているものが分かった時点でクラスシグネチャを調べるためのものです。

| 対象 | 移動先 |
|---|---|
| NeoGraphとは何か、その理由、ベンチマーク | [README](https://github.com/fox1245/NeoGraph#readme) |
| メンタルモデル — チャネル、ノード、エッジ、Send、Command | [Core Concepts](https://github.com/fox1245/NeoGraph/blob/master/docs/concepts.md) |
| 一般的な問題の症状起点での修正 | [Troubleshooting](https://github.com/fox1245/NeoGraph/blob/master/docs/troubleshooting.md) |
| 39個の実行可能なC++プログラム | [examples/](https://github.com/fox1245/NeoGraph/tree/master/examples) |
| 実行可能なPythonプログラム23件 | [bindings/python/examples/](https://github.com/fox1245/NeoGraph/tree/master/bindings/python/examples) |
| Async / コルーチン内部構造 | [ASYNC_GUIDE](https://github.com/fox1245/NeoGraph/blob/master/docs/ASYNC_GUIDE.md) |

## トップレベルヘッダ

便利ヘッダは、完全なCore + GraphEngine APIを取り込みます:

```cpp
#include <neograph/neograph.h>

using namespace neograph;
using namespace neograph::graph;
```

サブ名前空間:

- `neograph`           — 基盤型 (`Provider`、`Tool`、`ChatMessage`)
- `neograph::graph`    — エンジン、ノード、状態、チェックポイント
- `neograph::llm`      — プロバイダ実装 (OpenAI、スキーマ駆動、エージェントヘルパー)
- `neograph::mcp`      — Model Context Protocol クライアント
- `neograph::async`    — コルーチン + io_context インフラストラクチャ
- `neograph::util`     — 並行性プリミティブ

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

実際のLLM利用では、`MockProvider`を`llm::OpenAIProvider`または`llm::SchemaProvider`に置き換えてください。完全な`Provider`インターフェースは`neograph::Provider`にあります。

## リファレンス索引

サイドバーのクラスリスト、ファイルリスト、名前空間リストは `include/neograph/` 配下のヘッダから生成されています。[クラスリスト](annotated.html) が最も有用なエントリポイントです。

## ソース

プロジェクトホーム: <https://github.com/fox1245/NeoGraph>

ライセンス: MIT.
