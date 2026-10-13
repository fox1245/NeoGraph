<!-- neograph-i18n: source=docs/doxygen-mainpage.md locale=ja source_sha256=e76f02f95e2c48a4209505045ee0773301a7dbc6100f48105d1299237fd5d6fa -->
# NeoGraph C++ APIリファレンス {#mainpage}

**Languages:** [English](doxygen-mainpage.md) | [한국어](doxygen-mainpage.ko.md) | [日本語](doxygen-mainpage.ja.md) | [简体中文](doxygen-mainpage.zh-CN.md)

C++20のグラフエージェントエンジンライブラリです。C++向けのLangGraphにあたり、オプションでPythonバインディングも使えます。このサイトは、`include/neograph/` にある公開C++ヘッダーの、**自動生成されたリファレンス**です。

## 最初に読むもの

NeoGraphを初めて使う方は、**先に解説のドキュメントを読んでください**。この自動生成のリファレンスは、探したいものが分かっているときに、クラスのシグネチャを調べるためのものです。

| 知りたいこと | 参照先 |
|---|---|
| NeoGraphとは何か、なぜ作ったか、ベンチマーク | [README](https://github.com/fox1245/NeoGraph#readme) |
| 考え方：チャネル、ノード、エッジ、Send、Command | [コアコンセプト](https://github.com/fox1245/NeoGraph/blob/master/docs/concepts.md) |
| よくある問題の、症状から引ける対処法 | [トラブルシューティング](https://github.com/fox1245/NeoGraph/blob/master/docs/troubleshooting.md) |
| C++のサンプル（検証結果は別に報告） | [examples/](https://github.com/fox1245/NeoGraph/tree/master/examples) |
| Pythonの型付きプロバイダーとグラフのサンプル | [bindings/python/examples/](https://github.com/fox1245/NeoGraph/tree/master/bindings/python/examples) |
| 非同期／コルーチンの内部 | [ASYNC_GUIDE](https://github.com/fox1245/NeoGraph/blob/master/docs/ASYNC_GUIDE.md) |

## トップレベルのヘッダー

便利なヘッダーを1つインクルードすると、コアとグラフエンジンのAPI全体が読み込まれます。

```cpp
#include <neograph/neograph.h>

using namespace neograph;
using namespace neograph::graph;
```

サブ名前空間は次のとおりです。

- `neograph`：基盤となる型（`Provider`、`Tool`、`ChatMessage`）
- `neograph::graph`：エンジン、ノード、状態、チェックポイント
- `neograph::llm`：`SchemaProvider`、`Agent`、型付きのSDKランタイム
- `neograph::mcp`：Model Context Protocolのクライアント
- `neograph::async`：コルーチンとio_contextの基盤
- `neograph::util`：並行処理のプリミティブ

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

`SchemaProvider` は、admission済みの `sp::descriptor::ValidatedDescriptor`、`sp::runtime::Options`、省略可能な `SchemaProvider::Defaults` を受け取ります。ディスクリプターの読み込みは、closedでバージョン付きのデータのadmissionです。リクエストとレスポンスのインタープリターでも、任意のプリミティブのレジストリでもありません。認証情報は、公開されるディスクリプターのファイルではなく、ランタイムオプションに置きます。Defaultsに含まれるのは、型付きのOpenRouterルーティングと、Responsesの保持（`responses_store`）だけです。後者が有効なのは、Responsesに対してだけです。ホスト型のOpenRouterのルーティング、保持、JSONフォーマットは、引き続き、宣言する型付きのコントロールです。Images、Veo、Decisionsは、NeoGraphの別の型付きクライアントと、別の認可を使います。SDKのchatのグラントを引き継ぐことはありません。

プロバイダー呼び出しが返すのは `sp::runtime::Result` です。これは、変更できない、所有権付きの `std::shared_ptr<const sp::Outcome>` で、`sp::Completion` または `sp::Failure` を含みます。表示用のテキストだけでなく、結果の全体を保持してください。順序付きのメッセージとパート、保持されているネイティブな継続とファミリーが提供するワイヤーのエビデンス、順序付きのrawの観測、停止のエビデンス、本物の試行メタデータは、呼び出しが終わった後も、クライアントが破棄された後も残ります。使用量カウンターは、エビデンス、ステージ、品質を伴う、null許容の `uint64_t` の値です。値がないのは「不明」を意味し、ゼロではありません。失敗は、元の部分的な結果を保持します。`ProviderFailure::outcome()` と `ProviderObserverError::outcome()` は、その結果を保持します。後者は、オブザーバーの例外も `cause()` に保持します。

ワイヤーのエビデンスは、ファミリーが提供する省略可能なものです。`sp::Completion::wire_envelope` はnullになることがあります（Pythonでは `ProviderCompletion.wire_envelope` が `None`）。バッファ型のChatは、現在、レスポンス全体のJSONを `raw_events` に `RawWire` として保持します。`type == "chat.completion"` で、文書は `payload` にあります。`wire_envelope` はnullのままです。エビデンスは、ファミリーが保持している場所から読んでください。代わりのエンベロープが捏造されることはありません。ネイティブな継続とrawバッファは、保護されたエビデンスのままで、トレースのペイロードには含まれません。

これは、ソースとバイナリの両方の互換性を壊す変更です。すべてのC++コンシューマーとカスタムプロバイダーを、新しい対応するヘッダーとライブラリで再コンパイルしてください。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、ディスクリプターのインタープリター、ResponsesのWebSocket経路は削除されました。エイリアスも互換用のブリッジもありません。リリースの対象は、SDK `0.3.1` alpha、インターフェースリビジョン6／共有ABI 6で、capabilityのチェックはアウトオブラインです。alphaは、安定したAPIの約束ではありません。

Pythonでは、`SchemaProvider(ValidatedDescriptor, ProviderRuntimeOptions,
SchemaProviderDefaults)` と `make_provider_request` を、型付きの `ProviderMessage` パートとともに使います。`prepare` は `PreparedProviderRequest` を返し、`dispatch` はそれを1回だけ消費します。`invoke` は、リクエストの準備とディスパッチをまとめて行います。`ProviderOutcome` は、変更できないcompletion／failure／partialのビューを保持し、不明な使用量は `None` になります。グラフの `ChatMessage` は別の便宜的な型で、プロバイダーのメッセージの別名ではありません。ブロックするプロバイダー呼び出しはGILを解放するので、Pythonの非同期の呼び出し側は `asyncio.to_thread` を使えます。コンストラクター、エラー、制約については、[Pythonバインディングガイド](python-binding.md)を参照してください。


本物の結果が存在するのに、効果の後の会計処理や、最終レシートの永続化が失敗した場合、`ProviderDispatchOutcomePersistenceError` は、元の変更できない結果を `outcome()` に、元の永続化の例外を `cause()` に保持します。配送にも失敗していた場合は、`delivery_error()` が元のオブザーバーの例外を保持します。永続化に成功した後でオブザーバーが失敗した場合は、その元のオブザーバーの例外が、そのまま再送出されます。不明な、または結果のないトランスポートの失敗では、結果が捏造されることはありません。

## リファレンスの索引

サイドバーのクラス一覧、ファイル一覧、名前空間一覧は、`include/neograph/` にあるヘッダーから自動生成されています。最も使いやすい入口は、[クラス一覧](annotated.html)です。

## ソース

プロジェクトのホーム：<https://github.com/fox1245/NeoGraph>

ライセンス：MIT
