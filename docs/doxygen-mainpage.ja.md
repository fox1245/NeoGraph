<!-- neograph-i18n: source=docs/doxygen-mainpage.md locale=ja source_sha256=e7253d2aacfe88e0cb5c6ef9f02834b805e0ddcfb739e7c9882235f90eba9b52 -->
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
| Python typed provider とグラフの例 | [bindings/python/examples/](https://github.com/fox1245/NeoGraph/tree/master/bindings/python/examples) |
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

プロバイダー呼び出しは `sp::runtime::Result`、すなわち `sp::Completion` または `sp::Failure` を保持する不変の所有 `std::shared_ptr<const sp::Outcome>` を返します。表示テキストだけでなく結果全体を保持してください。順序付きメッセージ/パート、保持された native continuation と family が提供する wire 証拠、順序付き raw 観測、停止の根拠と実際の試行メタデータは呼び出しとクライアント破棄後も残ります。使用量は根拠・段階・品質付きの nullable `uint64_t` であり、欠落はゼロではなく不明です。失敗も元の部分結果を保持します。`ProviderFailure::outcome()` と `ProviderObserverError::outcome()` は実際の結果を保持し、後者の `cause()` は観測者の例外を保持します。

Wire 証拠は family が提供する任意の情報です。`sp::Completion::wire_envelope` は null の場合があります（Python の `ProviderCompletion.wire_envelope` は `None`）。現在の buffered Chat は応答 JSON 全体を `raw_events` 内の `RawWire` に保持します。`type == "chat.completion"` で、文書は `payload` にあり、`wire_envelope` は null のままです。Family が実際に保持する場所から証拠を読み、fallback envelope は捏造しません。Native continuation と raw buffer は保護された証拠として保持され、trace payload から除外されます。

ソースとバイナリの破壊的変更です。全 C++ 利用者とカスタムプロバイダーを新しい一致したヘッダー/ライブラリで再コンパイルします。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、descriptor interpreter、Responses WebSocket は alias/互換 bridge なしで削除されました。リリース対象は SDK `0.1.1` alpha、interface revision 4 / shared ABI 4 で、out-of-line capability check を使用します。Alpha は安定 API の保証ではありません。記録された interface-3 runtime/archive 資格検証は Linux/POSIX の範囲であり、interface 4 や Windows・macOS・WASM runtime の資格検証を意味しません。

Python は `SchemaProvider(ValidatedDescriptor, ProviderRuntimeOptions,
SchemaProviderDefaults)` と、typed `ProviderMessage` の part を受け取る
`make_provider_request` を使います。`prepare` は `PreparedProviderRequest` を
返し、`dispatch` はそれを一度消費します。`invoke` はリクエストを準備して
dispatch します。`ProviderOutcome` は不変の completion/failure/partial view を
保持し、不明な使用量は `None` です。グラフの `ChatMessage` は別の便宜型であり、
provider message の alias ではありません。Blocking provider 呼び出しは GIL を
解放し、Python async 呼び出し側は `asyncio.to_thread` を使えます。
コンストラクタ、エラー、制限は [`Python binding guide`](python-binding.md) を参照してください。


実結果の後に post-effect 精算や terminal receipt 永続化が失敗すると、`ProviderDispatchOutcomePersistenceError::outcome()` は元の不変結果、`cause()` は元の永続例外を保持します。delivery も失敗した場合は `delivery_error()` が元の観測者例外を保持します。永続化成功後の観測者失敗は元の例外を変更せず再送出し、不明/結果なし transport 失敗では outcome を捏造しません。
## リファレンス索引

サイドバーのクラスリスト、ファイルリスト、名前空間リストは `include/neograph/` 配下のヘッダから生成されています。[クラスリスト](annotated.html) が最も有用なエントリポイントです。

## ソース

プロジェクトホーム: <https://github.com/fox1245/NeoGraph>

ライセンス: MIT.
