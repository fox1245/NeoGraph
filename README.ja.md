<!-- neograph-i18n: source=README.md locale=ja source_sha256=95dcfaea2d1e943caf027838dec4ec2e1463e50b96b75736e25814569468af80 -->
# NeoGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

[![CI](https://github.com/fox1245/NeoGraph/actions/workflows/ci.yml/badge.svg)](https://github.com/fox1245/NeoGraph/actions/workflows/ci.yml)
[![PyPI](https://img.shields.io/pypi/v/neograph-engine)](https://pypi.org/project/neograph-engine/)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

NeoGraph は、グラフとして記述したワークフローを実行する C++20 ライブラリです。ノードが仕事をし、名前の付いた状態チャネルがデータを保持し、エッジが次に何を実行するかを決めます。ランタイムはノードをスケジュールし、ノードの書き込みを状態にマージします。チェックポイントストアを設定していればステップごとにチェックポイントを保存し、人の入力を待つために実行を止めて、あとで再開することもできます。

同じエンジンを C++ からも Python(`pip install neograph-engine`)からも使えます。オプションのコンポーネントとして、SchemaProvider SDK 経由のモデル呼び出し、MCP・A2A・ACP・gRPC のサーバーとクライアント、SQLite と PostgreSQL のチェックポイントストア、そして制限付きの JavaScript プログラムが予算の範囲内で複数のグラフを取りまとめる Program 層があります。

## 想定する読者

- ループ、分岐、並列 fan-out、リトライ、チェックポイント、人の承認ステップなど、状態が明示されたエージェントやパイプラインのワークフローを作る Python / C++ の開発者。
- そうしたワークフローを C++ のサービス内で動かす、または MCP・A2A・ACP で他のエージェントやツールに公開するチーム。
- LangGraph を知っている読者。リデューサー付きのチャネル、`Send`、`Command`、チェックポイントは同じ考え方なので、[コンセプトガイド](docs/concepts.md)は早く読み進められるはずです。

NeoGraph はアルファ版のソフトウェアです(バージョン 0.16.0)。C++ のバイナリインターフェースは 0.x のリリース間で変わることがあります。[ABI ポリシー](docs/ABI_POLICY.md)を参照してください。モデルの学習や数値計算ライブラリの代わりはしません。モデル呼び出しは、ノードがすることの1つにすぎません。

## クイックスタート

どちらの例も、ノードが1つのグラフを実行します。API キーは不要で、実行時にネットワーク呼び出しもしません。

### Python

Python 3.9 以降が必要です。

```bash
pip install neograph-engine
```

次の内容を `hello.py` として保存します。

```python
import neograph_engine as ng

@ng.node("greet")
def greet(state):
    return [ng.ChannelWrite(
        "messages",
        [{"role": "assistant", "content": f"Hello, {state.get('name')}!"}],
    )]

definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "demo",
    "channels": {
        "name": {"reducer": "overwrite"},
        "messages": {"reducer": "append"},
    },
    "nodes": {"greet": {"type": "greet"}},
    "edges": [
        {"from": ng.START_NODE, "to": "greet"},
        {"from": "greet", "to": ng.END_NODE},
    ],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())
result = engine.run(ng.RunConfig(thread_id="t1", input={"name": "NeoGraph"}))
print(result.output["channels"]["messages"]["value"])
```

```bash
python hello.py
```

```text
[{'role': 'assistant', 'content': 'Hello, NeoGraph!'}]
```

PyPI のホイールは、リリース 0.16.0 からビルドされたものです。[変更履歴](CHANGELOG.md)の *Unreleased* に載っている変更は含まれていません。それらを使うには、ソースからビルドしてください([ビルド設定](#build-configuration-and-further-reading)を参照)。

### C++

C++20 コンパイラ、CMake 3.20 以降、git、libcurl の開発ファイル(libcurl 7.88 以降。Debian と Ubuntu では `libcurl4-openssl-dev`)が必要です。最初の configure で、固定されたバージョンの SchemaProvider SDK をダウンロードします。モデル呼び出しをしない場合でも、Core にはこの SDK が必要です。オフラインの環境については、下の[ビルドと実行](#build-and-run)を参照してください。

このドキュメントに対応するタグ付きリリースはまだないため、以下のプロジェクトは `master` ブランチを取得します。ビルドを再現可能にするには、`master` をコミットハッシュに置き換えてください。

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(hello_neograph CXX)

include(FetchContent)
# Build only Core: no model calls, protocols, databases, examples or tests.
set(NEOGRAPH_BUILD_EXAMPLES OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_LLM OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_ASYNC OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_MCP OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_A2A OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_ACP OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_POSTGRES OFF CACHE BOOL "")
set(NEOGRAPH_BUILD_SQLITE OFF CACHE BOOL "")
FetchContent_Declare(NeoGraph
  GIT_REPOSITORY https://github.com/fox1245/NeoGraph.git
  GIT_TAG master)
FetchContent_MakeAvailable(NeoGraph)

add_executable(hello main.cpp)
target_compile_features(hello PRIVATE cxx_std_20)
target_link_libraries(hello PRIVATE neograph::core)
```

`main.cpp`:

```cpp
#include <neograph/neograph.h>

#include <cctype>
#include <iostream>
#include <memory>
#include <utility>

using namespace neograph;
using namespace neograph::graph;

namespace {

const ChannelKey<std::string> text_channel{"text"};

class UpperNode final : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        auto text = input.state.get(text_channel);
        for (auto& character : text)
            character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        co_return NodeOutput{{ChannelWrite{"text", json(std::move(text))}}};
    }

    std::string get_name() const override { return "upper"; }
};

}  // namespace

int main() {
    NodeFactory::instance().register_type(
        "quickstart.upper", [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<UpperNode>();
        },
        json::object(),
        json{{"reads", json::array({"text"})},
             {"writes", json::array({"text"})},
             {"exports", json::array({"text"})}});

    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "hello"},
        {"channels", {{"text", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"upper", {{"type", "quickstart.upper"}}}}},
        {"edges",
         {{{"from", "__start__"}, {"to", "upper"}},
          {{"from", "upper"}, {"to", "__end__"}}}},
    };

    auto engine = GraphEngine::build_strict(definition, EngineConfig{});
    RunConfig config;
    config.input = {{"text", "hello"}};
    const auto result = engine->run(config);
    std::cout << result.channel(text_channel) << '\n';
    return 0;
}
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/hello
```

```text
HELLO
```

同じグラフが [examples/62_core_quickstart.cpp](examples/62_core_quickstart.cpp) です。代わりに `cmake --install` でインストールした NeoGraph を使うには、`find_package(NeoGraph CONFIG REQUIRED)` を呼び出して `neograph::core` をリンクします。[ビルド設定](#build-configuration-and-further-reading)を参照してください。

### 次のステップ

- [ドキュメント目次](docs/README.md): すべてのガイドと、想定する読者。
- [コンセプト](docs/concepts.md): チャネル、ノード、エッジ、`Send`、`Command`、チェックポイント、ストリーミング。
- [Python バインディングガイド](docs/python-binding.md)と [C++ サンプル](examples/README.md)。
- サーバーとクライアント: [MCP](docs/mcp.md)、[A2A](docs/a2a.md)、[ACP](docs/acp.md)。
- [Program ガイド](docs/program-guide.md): fan-out、失敗ルーティング、承認、予算を備えた制限付き JavaScript プログラム。

## グラフが状態を変える仕組み

`count` というチャネルに `2` が入っているとします。インクリメントのノードは `2` を読み、`3` を提案する書き込みを返します。ランタイムは、スケジュールされたノードのバッチが終わったあとで、チャネルのリデューサーを通してその書き込みを適用します。次のバッチで動く後続のノードは、`3` を読みます。

```text
Committed state       Node computation          Reduced state
count = 2       ->    read 2; propose 3     ->    count = 3
                                                  |
                                            next node reads 3
```

この流れに出てきた用語は、実行モデルを説明するものです。

| 用語 | NeoGraph での意味 |
|---|---|
| ノード(Node) | ホストに登録された実行単位。入力の状態を読み、チャネルへの書き込みと、任意のルーティングコマンドを返します。 |
| 状態(State) | 現在の実行ステップから見えるチャネルの値。設定によっては、ランタイムが管理する履歴と使用量の集計も含みます。 |
| チャネル(Channel) | リデューサーを持つ名前付きの値。保持ポリシーとチェックポイント永続化ポリシーを任意で指定できます。 |
| リデューサー(Reducer) | チャネルの現在の値と、新しく来た書き込みを合成する関数。`overwrite` は値を置き換え、`append` は配列要素を積み上げ、カスタムリデューサーはそれ以外の合成方法を定義します。 |
| エッジ(Edge) | ノード間のスケジューリング規則。無条件、条件付き、複数の先行ノードを待つバリアがあります。サイクルを作れば、同じステージを繰り返せます。 |
| スーパーステップ(Superstep) | 実行可能なノードを1つのバッチとして実行し、その書き込みを適用して、次の予定へ進める単位。 |

通常のバッチでは、実行可能なノードはバッチ開始前のチャネル状態を読みます。ノードが `ChannelWrite` を返しても、同じバッチの別のノードが読む値はすぐには変わりません。バッチが正常に終わると、エグゼキューターが結果を適用し、以降のステップは更新後の状態を見ます。複数の分岐を持つ `Send` のバッチでは、各分岐が隔離された状態のコピーで入力を受け取り、終わったあとで出力をマージします。これは Pregel 方式の仕事の組み立て方です。ただし、チャネルとリデューサーの規則は NeoGraph 独自の契約であり、Pregel のすべての機能を実装していると主張するものではありません。

並行して実行しても、すべてのリデューサーが順序に依存しなくなるわけではありません。2つのノードがテキストを追加したり、同じチャネルを上書きしたりすると、書き込みの順序が結果に影響します。ワークフローが順序に依存しないことを求めるなら、別々のチャネルか、順序に依存しないリデューサーを使ってください。チャネルの保持も、合成とは別の話です。append チャネルは、末尾の一部だけを残すように制限できます。スケジューリング、リデューサー、バリア、キャンセルについては、[コンセプト](docs/concepts.md)と[並行実行](docs/concurrency.md)を参照してください。

## Core の例を順に見る

前掲の [C++ クイックスタート](examples/62_core_quickstart.cpp)は、文字列を大文字にするノードを登録し、次のトポロジーをコンパイルします。

```text
__start__ -> upper -> __end__

Input channel:   text = "hello"
Node reads:      "hello"
Node returns:    ChannelWrite{"text", "HELLO"}
Reducer:         overwrite
Output channel:  text = "HELLO"
```

ソースにはノードが読み書きするチャネルが宣言されているので、`GraphEngine::build_strict` は実行前にトポロジーを検査できます。モデル呼び出しも API キーも不要です。期待される出力は `HELLO` です。

<a id="build-and-run"></a>
### ビルドと実行

`NEOGRAPH_BUILD_LLM=OFF` の場合でも、SchemaProvider は必須の外部 SDK です。Core が型付き provider の契約をエクスポートしているためです。以下のコマンドは、インストール済みの [SchemaProvider ランタイムパッケージ](https://github.com/fox1245/SchemaProvider)を使うもので、`SCHEMAPROVIDER_PREFIX` にそのインストール先を指定します。`-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=../SchemaProvider` で指定したチェックアウトが最優先されます。それ以外の場合、CMake はまずインストール済みのパッケージを探し、見つからなければ固定された公開 SDK のアーカイブをダウンロードします。インストール済みのパッケージや指定したチェックアウトでオフラインビルドをするには、`NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` を指定します。CMake は隣のディレクトリのチェックアウトを推測せず、削除されたバンドル版インタープリターも使いません。

必要なのは、C++20 コンパイラ、CMake 3.20 以降、そして SDK のランタイム依存である libcurl 7.88 以降です。SDK の TLS は libcurl のバックエンドを使うため、SDK 自体に OpenSSL は不要です。NeoGraph の HTTPS コンポーネントを含む完全なビルドには、OpenSSL 3 が必要です。既定のビルドでは SQLite と PostgreSQL の連携も有効になるため、下のコマンドは不要な NeoGraph のコンポーネントを無効にします。SDK の依存は残ります。プラットフォームとビルドの制約は[トラブルシューティング](docs/troubleshooting.md)を参照してください。

```bash
git clone https://github.com/fox1245/NeoGraph.git
cd NeoGraph
cmake -S . -B build-core \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_EXAMPLES=ON \
  -DNEOGRAPH_BUILD_PROGRAM=OFF \
  -DNEOGRAPH_BUILD_LLM=OFF \
  -DNEOGRAPH_BUILD_ASYNC=OFF \
  -DNEOGRAPH_BUILD_MCP=OFF \
  -DNEOGRAPH_BUILD_A2A=OFF \
  -DNEOGRAPH_BUILD_ACP=OFF \
  -DNEOGRAPH_BUILD_POSTGRES=OFF \
  -DNEOGRAPH_BUILD_SQLITE=OFF
cmake --build build-core --parallel --target example_core_quickstart
./build-core/example_core_quickstart
```

## Core と ProgramRuntime

`GraphEngine` は、コンパイル済みのグラフを実行します。ノードのスケジューリング、状態の更新、ルーティング、リトライ、ストリーミング、キャンセル、グラフのチェックポイントと再開を担当します。コンパイル済みのトポロジーは変更できません。サポートされている世代の移行は、ノードの実行中に任意に書き換えるのではなく、制御された安全な地点で行われます。

`ProgramRuntime` は、承認(admission)された Program を取りまとめます。Program は Core のグラフを呼び出し、子 Program を管理できます。不変の Program バージョン、カタログとポリシーのスナップショット、コマンドジャーナル、子の系譜、予算、リプレイ、承認済みの置き換えと移行を加えます。ホストは実行可能なケイパビリティを登録し、Program をコンパイルして承認し、呼び出しを開始します。グラフのノードを実行するのは、引き続き Core です。

先ほどの調査ワークフローでは、1つの Core グラフが検索とレビューを担当できます。Program はそのグラフを呼び出し、別々のタスクのために子 Program を起動し、その結果を待ち、ライフサイクルの遷移を記録できます。永続的な復旧には、設定されたストアと、対応する保管(custody)契約が必要です。メモリ上のストアはプロセスが終わると消えますし、ジャーナルがあるだけでは外部ツールの副作用が厳密に1回になるわけではありません。

QuickJS は、オプションの Program 記述インターフェースです。Program は、範囲が限定された JavaScript の計算と、`callCore`、`spawn`、`await`、`all`、`parallel`、`race`、`quorum`、`emit`、`checkpoint`、`cancelScope` といったジェネレーターコマンドを使います。ホストはケイパビリティを承認し、生成されたソースを公開前に検証します。モデルが生成した提案が、コンパイラ、認証情報、カタログ、権限付与にアクセスすることはありません。

```bash
cmake -S . -B build-program \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON \
  -DNEOGRAPH_BUILD_EXAMPLES=ON
cmake --build build-program --parallel --target example_program_quickstart
./build-program/example_program_quickstart
```

[Program クイックスタート](examples/63_program_quickstart.cpp)は、インクリメントのグラフを呼び出す Program をコンパイルして承認します。期待される出力は `1` です。メモリ上のストアと C++ の Program ビルダーを使います。JavaScript による記述と永続的な実行については、[Program ガイド](docs/program-guide.md)、[再帰的な Program](docs/PROGRAM_RECURSIVE_HARNESSES.md)、[厳密なランタイム契約](docs/STRICT_RUNTIME_INTERPOSITION.md)から読み始めてください。

<a id="typed-provider-calls"></a>
## 型付き provider 呼び出し

モデル呼び出しには、検証済みのディスクリプター、ランタイムオプション、型付きのリクエストを使います。ディスクリプターの承認は、閉じたバージョン付きデータだけを受け付け、リクエスト/レスポンスのインタープリターは実行しません。認証情報は、公開するディスクリプターファイルではなく、ランタイムオプションに置きます。`SchemaProvider::Defaults` には、型付きの OpenRouter ルーティングと Responses の保持設定が入っています。画像、Veo、Decisions は、別の型付きクライアントと権限を使います。

```cpp
#include <neograph/llm/schema_provider.h>
#include <neograph/types.h>

sp::runtime::Result first_call(
    sp::descriptor::ValidatedDescriptor descriptor,
    sp::runtime::Options options, std::string model) {
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

準備済みのリクエストは1回だけ消費されます。`sp::runtime::Result` は、`Completion` または `Failure` を含む不変の `sp::Outcome` を所有します。順序付きのメッセージとパート、ネイティブの継続情報、生の観測値、停止の根拠、試行のメタデータ、失敗時の部分的な結果が必要なら、この結果をそのまま保持してください。使用量カウンターは null になりえます。ないときは不明という意味で、観測された 0 は 0 です。使用量の報告と予算の差し引きは別の記録であり、持ち運べる報告から支出の権限を与えることはできません。

`usage.provider_cost` は、provider が報告した nanoUSD 単位の金額、フィールドごとの状態、出所、品質、BYOK の根拠、パースした binary64 値の切り上げを保持します。請求書でも、トークン数でも、見積もりでも、支出の権限でもありません。欠けている金額のメタデータは、正規形の JSON でも欠けたままです。報告された 0 や `is_byok=false` は、それ自体が根拠です。不明な通貨や、矛盾した、または形式の誤った報告は、USD やトークンに変換せず、そのまま保持します。ブロッキングの provider 呼び出しも、単一の非同期 SDK パイプラインを駆動し、SDK のイベントループやコールバックワーカーからの再入は拒否されます。

`first_call` が返ったあとは、`std::get_if<sp::Completion>(result.get())` で完了結果の `messages`、`stop`、`usage` を調べます。完了でなければ、`std::get<sp::Failure>(*result)` から `error.kind`、`error.safe_message`、リトライの根拠、そして `partial` 内の部分的なメッセージと使用量が得られます。たとえば `completion.usage.output_total` がなければ出力トークン数は不明で、あって `value` が `0` なら 0 を報告したことになります。表示用のテキストは、保持された結果を見る方法の1つにすぎません。

`ChatMessage`、`ChatTool`、JSON は、持ち運べる射影です。本物のネイティブ履歴は、ネイティブのチェックポイントサイドカーとともにメモリ上に置けます。永続的なネイティブ履歴には、本物の `sp::NativeArchive` と、所有者専用の保護された保管が必要です。持ち運べる JSON から、その権限を再現することはできません。アーカイブは独立した鍵で保管状態を認証します。暗号化でも、ベンダー発行者の認証でもありません。アーカイブの本体、鍵、ネイティブの blob、生のワイヤ観測値は公開しないでください。永続化の失敗、オブザーバー、管理される予算バンク、リプレイの境界については、[provider リファレンス](docs/reference-en.md)と[移行ガイド](docs/migration-v0.4-to-v1.0.md)を参照してください。

型付きへの切り替えで、`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、ディスクリプターのインタープリター、Responses の WebSocket 経路が削除されました。C++ の利用側を再コンパイルし、カスタム provider を移行してください。互換用のエイリアスはありません。SDK パッケージは `0.4.0` アルファで、`cmake/NeoGraphSchemaProvider.cmake` が固定する公開コミット(`main` ライン)を使い、インターフェースのリビジョンは 7、共有 ABI は 7 です。型付きのメディアと新しいリクエスト制御が公開レイアウトを変えるため、SDK の利用側、NeoGraph、Python 拡張はあわせて再ビルドしてください。ネイティブアーカイブ v3（`spna3`）は変わりません。持ち運べる JSON は `provider-message-v3`、`provider-outcome-v3`、`runtime-history-record-v3` として書き出し、v2 形式のレコードも引き続き読み込みます。この世代は、安定した SDK インターフェースを宣言していません。

## Python

```bash
pip install neograph-engine
```

ここで説明している型付き provider API は、NeoGraph `0.14.0` が対象です。それ以前のホイールは古いインターフェースを公開しています。ソース API とビルドの前提条件は [Python バインディングガイド](docs/python-binding.md)にあります。[クイックスタート](#python)のグラフには API キーは不要です。

Python は、Program のコンパイルと実行、Hook、ランタイムコンテキストの要件、厳密プロファイル、SQLite による永続化、正確なチェックポイントの再開も提供します。型付き provider は `ProviderMessage` と `make_provider_request` を使い、`prepare`/`dispatch` または `invoke` を呼びます。結果には、ネイティブが所有する完了または失敗の根拠が残ります。これらのメッセージは、グラフ用の簡易型 `ChatMessage` とは別物です。ブロッキングの provider 呼び出しは GIL を解放するので、`asyncio.to_thread` でイベントループのスレッドの外に出せます。provider 入力の完全な例は [Python サンプル](bindings/python/examples/README.md)を、実行できる Program の例とそのルールは [Python バインディングガイド](docs/python-binding.md#program-and-quickjs)を参照してください。

## 向いている用途と限界

NeoGraph は、状態遷移が明示されていて、分岐やループ、並列タスク、チェックポイント、人によるレビューのあるワークフローに向いています。小さな固定のグラフを C++ アプリケーションに組み込むこともできます。ノード内の計算はアプリケーションの責任です。グラフランタイムはモデルを学習せず、数値計算ライブラリの代わりにもなりません。モデル呼び出しには、provider のレイテンシー、可用性、コストがそのままかかります。

ランタイムは、グラフ全体とノードごとのリトライポリシー、範囲が限定された再利用可能なノードキャッシュ、`Send` の fan-out、`Command` によるルーティング、サブグラフ、状態履歴、フォーク、HITL、`NodeInterrupt` をサポートします。MCP、A2A、ACP、gRPC、可観測性の連携は、オプションのコンポーネントです。ランタイムコンテキストと Hook は、特定のディスパッチ入力を要求し、配信の根拠を記録できますが、これらの検査は、モデルがすべてのトークンに注意を払ったことを証明するものではありません。永続的なネイティブ復旧と、範囲が限定されたフォークには、元の共有された会計上の権限が必要です。スナップショットをコピーしても、予算は新しくなりません。

ワークロードは、実際のノード、provider、ストア、並行度、ビルドプロファイルで測定してください。[ベンチマーク](benchmarks/README.md)と[性能ガイド](docs/performance-deep-dive.md)は、測定した構成と限界を説明するもので、どこでも通用する速度の主張ではありません。最適化した実行を測るときは、単一構成のビルドで `CMAKE_BUILD_TYPE=Release` を指定してください。`NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION=ON` は、対応するコンパイラでホスト固有のチューニングを加えるので、配布用のバイナリではオフにしてください。

<a id="build-configuration-and-further-reading"></a>
## ビルド設定と関連ドキュメント

| オプション | 用途 |
|---|---|
| `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` | 指定した SDK のチェックアウト。インストール済みパッケージの探索とダウンロードより優先されます。 |
| `NEOGRAPH_FETCH_SCHEMAPROVIDER` | インストール済みのパッケージがないとき、固定された公開 SDK のアーカイブをダウンロードします。既定はオンで、オフラインビルドではオフにします。 |
| `NEOGRAPH_BUILD_PROGRAM` | Program ランタイム、カタログ、系譜、移行。既定はオフ。 |
| `NEOGRAPH_BUILD_QUICKJS_CONTROL` | 組み込みの QuickJS による Program 記述。既定はオフ。 |
| `NEOGRAPH_BUILD_PYBIND` | Python 拡張。既定はオフ。 |
| `NEOGRAPH_BUILD_LLM` | NeoGraph のモデル呼び出しアダプター。オフにしても SDK への依存はなくなりません。 |
| `NEOGRAPH_BUILD_SQLITE` / `NEOGRAPH_BUILD_POSTGRES` | オプションの永続ストア。どちらも既定はオン。 |
| `NEOGRAPH_BUILD_MCP_CLIENT` / `NEOGRAPH_BUILD_MCP_SERVER` | MCP クライアントと stdio サーバーのコンポーネント。どちらも既定はオン(`NEOGRAPH_BUILD_MCP` が既定値を決めます)。 |
| `NEOGRAPH_BUILD_MCP_HTTP_SERVER` | MCP の Streamable HTTP サーバートランスポート。既定はオフ。 |
| `NEOGRAPH_BUILD_HARNESS_MCP_BINARY` | `neograph-harness-mcp` の実行ファイルをビルドしてインストールします。既定はオフ。 |
| `NEOGRAPH_BUILD_A2A` / `NEOGRAPH_BUILD_ACP` / `NEOGRAPH_BUILD_GRPC` | プロトコル連携。A2A と ACP は既定でオン、gRPC は既定でオフ。 |
| `NEOGRAPH_BUILD_ASYNC` | HTTP、SSE、WebSocket のヘルパー。既定はオン。 |
| `NEOGRAPH_BUILD_EXAMPLES` / `NEOGRAPH_BUILD_TESTS` | サンプルプログラム(既定はオン)と単体テスト(既定はオフ)。 |
| `NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION` | 最適化構成向けのホスト固有のチューニング。移植性はありません。既定はオフ。 |

Core を取得せずにインストールして別のプロジェクトから使うには、一度ビルドしてインストールします。CMake が SDK をダウンロードした場合は、`cmake --install` が SDK も同じ接頭辞にインストールするので、`CMAKE_PREFIX_PATH` には1つ指定すれば足ります。

```bash
git clone https://github.com/fox1245/NeoGraph.git
cmake -S NeoGraph -B NeoGraph/build-install -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_EXAMPLES=OFF -DNEOGRAPH_BUILD_LLM=OFF -DNEOGRAPH_BUILD_ASYNC=OFF \
  -DNEOGRAPH_BUILD_MCP=OFF -DNEOGRAPH_BUILD_A2A=OFF -DNEOGRAPH_BUILD_ACP=OFF \
  -DNEOGRAPH_BUILD_POSTGRES=OFF -DNEOGRAPH_BUILD_SQLITE=OFF
cmake --build NeoGraph/build-install --parallel
cmake --install NeoGraph/build-install --prefix "$HOME/neograph"
```

別のプロジェクトでは、クイックスタートの `FetchContent` の行を `find_package(NeoGraph CONFIG REQUIRED)` に置き換え、`-DCMAKE_PREFIX_PATH="$HOME/neograph"` を付けて configure します。インストール済みのものを使う側は、必要なコンポーネントだけをリンクします。

```cmake
find_package(SchemaProvider 0.4.0 EXACT CONFIG REQUIRED COMPONENTS runtime transport)
find_package(NeoGraph CONFIG REQUIRED)
target_link_libraries(app PRIVATE neograph::core neograph::llm SchemaProvider::runtime)
```

### ローカルでの CI 検証(Windows と WSL)

ソースのルートで実行します。Python 3.10 以降、CMake/CTest、そしてプロファイルに必要なネイティブのツールチェーンと依存関係が、あらかじめ用意されている必要があります。Windows では VS2022 の x64 開発者向け PowerShell を使い、`CMAKE_TOOLCHAIN_FILE` で vcpkg のトランスポート用ツールチェーンを、専用の `VCPKG_INSTALLED_DIR` を、`PATH` に OpenSSL/curl のランタイムツールを指定してください。WSL では、C++20 コンパイラ、pkg-config、libpq、SQLite、OpenSSL、HTTP/2 対応の curl の開発パッケージを用意します。Linux の `native-linux` と `asan` には、`psql` と、到達可能な**破壊的テスト専用**データベースを指す `NEOGRAPH_TEST_POSTGRES_URL` も必要です。Python の pytest/pydantic/certifi を用意してください。`native-linux` には、選んだインタープリターに `a2a-sdk[http-server]>=1.1,<2`、`agent-client-protocol==0.12.1`、uvicorn、httpx も必要です。

```powershell
# Windows: choose a new output path for each invocation.
python scripts/verify_ci.py native-windows --work-dir build/local-windows-01 --jobs 4
python scripts/verify_ci.py install --work-dir build/local-install-01 --jobs 4 --shared --program
```

```sh
# WSL/Linux: provision the test database and dependencies before running.
python scripts/verify_ci.py native-linux --work-dir build/local-linux-01 --jobs 4
python scripts/verify_ci.py quickjs-performance --work-dir build/local-quickjs-01 --jobs 4
```

`--work-dir` は、ソースのルートやその祖先ではない、自分が所有する新しい(存在しない)出力パスでなければなりません。`--jobs` は正の数か `auto`(CPU 数)にします。既存の出力は拒否して保持し、自動では削除しません。ランナーは依存関係をインストールせず、ホストのポリシーも変更しません。固定された依存関係のダウンロードと、cibuildwheel が宣言しているブートストラップと修復は、そのまま適用されます。

| プロファイル | 維持している目的 / 前提条件 |
|---|---|
| `native-linux` | PostgreSQL を含む完全なネイティブゲート。続いて、直列で実行する完全な Python/プロトコルのテストと、DB なしの ACP 永続化の再実行。 |
| `native-posix` | Linux ARM/macOS のネイティブテスト。テスト用サービスなしで、PostgreSQL のビルドとリンクのみ確認。 |
| `native-windows` | MSVC による DB なしのネイティブ/Program/QuickJS テストと、別枠の C 組み込み ABI スモークテスト。x64 MSVC 環境で Ninja を使ってビルドします。`--generator "Visual Studio 17 2022"` を渡すこともできます。 |
| `asan` | Linux の ASan/UBSan/LSan、サンプル11個、Python テスト一式。GCC の libasan/libstdc++ が必要。 |
| `tsan` | 別枠の Linux TSan テストとサンプル5個。プロセス内限定の `setarch -R` を許可し、抑制リストは変更なし。 |
| `msvc-asan` | Windows で直列に実行する Program/QuickJS のカナリア。有効化した `cl >=19.50`(VS2026)が必要で、MSVC 19.44 では動きません。 |
| `grpc` | gRPC のグラフ契約。gRPC/Protobuf のコンパイラとライブラリが必要。 |
| `benchmark` | Linux Release で4つのワークロードを見るリグレッションゲート。スループット、レイテンシー、目標 RSS の元の上限を適用。 |
| `quickjs-performance` | Linux で、有効と無効のビルドを揃えて比較。不変の来歴情報のため、実際のソースルートの Git チェックアウトが必要。上限は CI の hosted x86-64 ランナー基準のため、より遅いハードウェアでは回帰がなくても `javascript_wrapped_core` が失敗することがある。`--base <ref>` を指定すると、そのコミットもビルドし、同じマシンでケースごとに比較する。 |
| `fuzz` | Linux の Clang/libFuzzer による60秒のカナリア。コーパスは自分が所有する出力にコピーします。 |
| `install` | 隔離したエクスポート先で ABI・シンボル・C++・C11・衝突・再配置を検査するコンシューマー。Git/Bash は不要。任意の `--shared`、`--core-only`、`--program`。`--quickjs` には `--program` と ELF/Mach-O の検査が必要で、Windows の行にはありません。 |
| `sdist` | ソースアーカイブと Twine。build/twine/scikit-build-core>=1.0/pybind11==2.13.6/ninja>=1.10 が必要。任意の `--release-tag v0.16.0`。 |
| `wheel` | 修復を経たインストール済みホイール。cibuildwheel==2.23.0 とネイティブ/コンテナの provider が必要。`--arch x86_64\|aarch64\|arm64\|AMD64` は必須。任意の `--python cp312` を付けると、すべての CPython ではなく1つだけビルドします。 |
| `runtime-archive` | 共有 SDK のネイティブアーカイブの移植性を見るターゲット/CTest。Ninja とプラットフォームのランタイム依存が必要。 |

ローカルの Windows/WSL の結果は、ネイティブの ARM/macOS や VS2026 のサニタイザー行の代わりにはなりません。push と pull request のたびに `ci.yml` が実行されます。Linux、macOS、Windows の完全なネイティブテストに、インストール済みコンシューマーの行が2つ加わります。ドキュメントだけの変更では実行されません。`ci-extended.yml` は夜間と要求時に実行されます。サニタイザー、ファズのカナリア、性能ゲート、ネイティブ ARM64、gRPC、Visual Studio ジェネレーター、残り8つのインストール済みコンシューマーの行を扱います。リリース候補では、タグを付ける前に実行してください。`wheels.yml` は、パッケージングの変更ではプラットフォームごとに CPython 3.12 のホイールを1つビルドします。リリースタグでは、Linux（x86_64 と aarch64）、macOS（arm64）、Windows（AMD64）向けに CPython 3.9–3.13 のホイールをビルドし、最低のプラットフォームバージョンは glibc 2.34 と macOS 14 です。

- [ドキュメント目次](docs/README.md)
- [コンセプトとグラフの意味論](docs/concepts.md)
- [C++ リファレンス](docs/reference-en.md)と [Python バインディングガイド](docs/python-binding.md)
- [MCP](docs/mcp.md)、[A2A](docs/a2a.md)、[ACP](docs/acp.md) のガイドと [Program ガイド](docs/program-guide.md)
- [非同期ガイド](docs/ASYNC_GUIDE.md)と[並行実行・キャンセル](docs/concurrency.md)
- [ランタイムコンテキストと厳密なインターポジション](docs/STRICT_RUNTIME_INTERPOSITION.md)
- [Harness MCP](docs/HARNESS_MCP.md)
- [移行ガイド](docs/migration-v0.4-to-v1.0.md)と[トラブルシューティング](docs/troubleshooting.md)
- [C++ サンプル](examples/README.md)、[Python サンプル](bindings/python/examples/README.md)、[ベンチマークの方法](benchmarks/README.md)

## ライセンス

MIT です。[LICENSE](LICENSE) を参照してください。サードパーティの通知は [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) にあります。
