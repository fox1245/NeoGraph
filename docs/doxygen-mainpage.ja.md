<!-- neograph-i18n: source=docs/doxygen-mainpage.md locale=ja source_sha256=f92342d3e71ccd35386584d6ec5ccdd2d61c6e6241fc26c74f884f70b4f3e934 -->
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
| C++ 例 (検証は別途報告) | [examples/](https://github.com/fox1245/NeoGraph/tree/master/examples) |
| Python 例 (provider 移植延期) | [bindings/python/examples/](https://github.com/fox1245/NeoGraph/tree/master/bindings/python/examples) |
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
- `neograph::llm` — `SchemaProvider`, `Agent`; typed SDK runtime
- `neograph::mcp`      — Model Context Protocol クライアント
- `neograph::async`    — コルーチン + io_context インフラストラクチャ
- `neograph::util`     — 並行性プリミティブ

## 最初のプログラム

```cpp
#include <neograph/llm/schema_provider.h>
#include <neograph/types.h>

sp::runtime::Result first_call(
    sp::descriptor::ValidatedDescriptor descriptor, sp::runtime::Options options,
    std::string model) {
    neograph::llm::SchemaProvider provider(
        std::move(descriptor), std::move(options), {});
    std::vector<sp::Message> history{
        {.role = sp::Role::User, .parts = {sp::Text{"hi"}}}};
    auto request = neograph::make_provider_request(
        provider, std::move(model), std::move(history));
    auto prepared = provider.prepare(std::move(request));
    return provider.dispatch(std::move(prepared));
}
```

`SchemaProvider` は承認済み `sp::descriptor::ValidatedDescriptor`、`sp::runtime::Options`、任意の `SchemaProvider::Defaults` を受け取ります。descriptor は closed/versioned データ admission であり、要求/応答 interpreter や任意 primitive registry ではありません。credential は公開 descriptor でなく runtime options に置きます。Defaults は typed OpenRouter routing と Responses 保持 (`responses_store`) のみで、後者は Responses 専用です。Hosted OpenRouter routing・retention・JSON 形式は宣言済み typed 制御です。Images、Veo、Decisions は別の NeoGraph typed client と別の承認を使い SDK chat grant を継承しません。

プロバイダー呼び出しは `sp::runtime::Result`、すなわち `sp::Completion` または `sp::Failure` を保持する不変の所有 `std::shared_ptr<const sp::Outcome>` を返します。表示テキストだけでなく結果全体を保持してください。順序付きメッセージ/パート、native continuation、完全な wire envelope、順序付き raw 観測、停止の根拠と実際の試行メタデータは呼び出しとクライアント破棄後も残ります。使用量は根拠・段階・品質付きの nullable `uint64_t` であり、欠落はゼロではなく不明です。失敗も元の部分結果を保持します。`ProviderFailure::outcome()` と `ProviderObserverError::outcome()` は実際の結果を保持し、後者の `cause()` は観測者の例外を保持します。

ソースとバイナリの破壊的変更です。全 C++ 利用者とカスタムプロバイダーを新しい一致したヘッダー/ライブラリで再コンパイルします。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、descriptor interpreter、Responses WebSocket は alias/互換 bridge なしで削除されました。SDK は不安定 `0.0.0`、interface revision 3 / shared ABI 3、out-of-line capability check を使用し、安定リリースの宣言ではありません。現 runtime/archive は Linux/POSIX で、Windows・macOS・WASM runtime の資格検証を意味しません。Python provider binding/wrapper は延期され、この C++ 変更では移植されません。


実結果の後に post-effect 精算や terminal receipt 永続化が失敗すると、`ProviderDispatchOutcomePersistenceError::outcome()` は元の不変結果、`cause()` は元の永続例外を保持します。delivery も失敗した場合は `delivery_error()` が元の観測者例外を保持します。永続化成功後の観測者失敗は元の例外を変更せず再送出し、不明/結果なし transport 失敗では outcome を捏造しません。
## リファレンス索引

サイドバーのクラスリスト、ファイルリスト、名前空間リストは `include/neograph/` 配下のヘッダから生成されています。[クラスリスト](annotated.html) が最も有用なエントリポイントです。

## ソース

プロジェクトホーム: <https://github.com/fox1245/NeoGraph>

ライセンス: MIT.
