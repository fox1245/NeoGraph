<!-- neograph-i18n: source=README.md locale=ja source_sha256=9e3cece3555cbcb547daaaf29d7c6ce6aa422e229bc3899ce0fad78eac0400c9 -->
# NeoGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

NeoGraphは、グラフで記述された状態を持つワークフローを実行するC++20ランタイムです。グラフでは、実行可能なノード、名前付きの状態チャネル、書き込みを統合する規則、次に実行する処理を決めるエッジを定義します。ノードは通常の計算、ツール呼び出し、モデルへの出力要求を実行できます。ランタイムはノードの実行をスケジュールし、書き込みを適用します。チェックポイントストアを設定すると、中断と再開に備えて進行状況も保存します。Pythonバインディングも同じC++エンジンを公開します。

調査ワークフローを例に考えます。文書を検索し、複数の文書から知見を抽出し、それらをまとめ、さらに検索する必要があるかをレビュー担当者に確認します。文書や知見は状態チャネルに格納し、検索、抽出、レビューはノードとして定義します。エッジは次の段階を選ぶか、検索段階に戻します。グラフを使うと、一連のモデルプロンプトの中に埋もれがちな遷移を明示できます。調査、ツール利用、人によるレビュー、マルチエージェントのワークフローは[サンプル](examples/README.md)を参照してください。

## グラフによる状態の更新

`count`というチャネルに`2`が格納されているとします。インクリメントノードは`2`を読み、`3`への更新を提案する書き込みを返します。ランタイムは、スケジュールされたノードのバッチが終了した後、チャネルのリデューサーを通じてその書き込みを適用します。次のバッチで実行される後続ノードは`3`を読みます。

```text
Committed state       Node computation          Reduced state
count = 2       ->    read 2; propose 3     ->    count = 3
                                                  |
                                            next node reads 3
```

この実行の流れに登場する用語は、実行モデルの各要素を表します。

| 用語 | NeoGraphでの意味 |
|---|---|
| ノード（Node） | ホストに登録された実行可能な処理。入力状態を読み、チャネルへの書き込みと、必要に応じてルーティングコマンドを返します。 |
| 状態（State） | 現在の実行ステップから見えるチャネル値。設定に応じて、ランタイムが管理する履歴や会計情報も含みます。 |
| チャネル（Channel） | リデューサーを持つ名前付きの値。必要に応じて、保持ポリシーやチェックポイントへの永続化ポリシーを設定します。 |
| リデューサー（Reducer） | 現在のチャネル値と新たな書き込みを組み合わせる関数。`overwrite`は値を置き換え、`append`は配列要素を蓄積します。独自のリデューサーでは別の統合方法を定義できます。 |
| エッジ（Edge） | ノード間のスケジューリング規則。無条件のエッジ、条件付きのエッジ、複数の先行ノードの完了を待つバリアがあります。サイクルを使うと同じ段階を繰り返せます。 |
| スーパーステップ（Superstep） | 実行準備が整ったノードをまとめて実行し、その書き込みを適用してからスケジュールを進める単位。 |

通常のバッチでは、実行準備が整ったノードはバッチ開始前のチャネル状態を読みます。あるノードが`ChannelWrite`を返しても、同じバッチ内の別のノードが読む値は直ちには変わりません。処理が正常に完了すると、エグゼキューターが結果を適用し、後のステップから更新後の状態が見えるようになります。複数分岐の`Send`バッチでは、各分岐は分離された状態のコピーで入力を受け取り、後で出力を統合します。これはPregel型の処理構成ですが、チャネルとリデューサーの規則はNeoGraph独自の契約です。Pregelの全機能を実装しているという意味ではありません。

並行実行しても、すべてのリデューサーが書き込み順序に依存しなくなるわけではありません。2つのノードが同じチャネルにテキストを追加したり値を上書きしたりすると、順序が結果に影響します。ワークフローで順序に依存しない結果が必要な場合は、別々のチャネルか、順序に依存しないリデューサーを使ってください。また、チャネルの保持と値の統合は別の規則です。追記型のチャネルでも、末尾の一定範囲だけを保持できます。スケジューリング、リデューサー、バリア、キャンセルについては、[概念](docs/concepts.md)と[並行処理](docs/concurrency.md)を参照してください。

## Coreの具体例

[完全なC++クイックスタート](examples/62_core_quickstart.cpp)では、文字列を大文字に変換するノードを登録し、次のトポロジーをコンパイルします。

```text
__start__ -> upper -> __end__

Input channel:   text = "hello"
Node reads:      "hello"
Node returns:    ChannelWrite{"text", "HELLO"}
Reducer:         overwrite
Output channel:  text = "HELLO"
```

ノードの計算処理は通常のC++コードです。

```cpp
class UpperNode final : public neograph::graph::GraphNode {
public:
    asio::awaitable<neograph::graph::NodeOutput> run(
        neograph::graph::NodeInput input) override {
        auto text = input.state.get(neograph::graph::ChannelKey<std::string>{"text"});
        for (auto& character : text)
            character = static_cast<char>(
                std::toupper(static_cast<unsigned char>(character)));
        co_return neograph::graph::NodeOutput{{
            neograph::graph::ChannelWrite{"text", neograph::json(std::move(text))}}};
    }
    std::string get_name() const override { return "upper"; }
};
```

完全なソースには、ヘッダー、読み取りと書き込みを宣言したノード登録、トポロジー、`GraphEngine::build_strict`、実行入力、型付きの出力アクセスが含まれます。モデル呼び出しもAPIキーも不要です。期待される出力は`HELLO`です。

### ビルドと実行

Coreが型付きプロバイダー契約を公開するため、`NEOGRAPH_BUILD_LLM=OFF`でも外部SDKのSchemaProviderは必須です。以下のコマンドでは、インストール済みの[SchemaProviderランタイムパッケージ](https://github.com/fox1245/SchemaProvider)を使用します。`SCHEMAPROVIDER_PREFIX`にそのインストール先プレフィックスを設定してください。`-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=../SchemaProvider`でチェックアウト先を明示した場合は、それを優先します。指定がなければ、CMakeはインストール済みパッケージを優先し、見つからない場合はバージョンを固定した公開SDKアーカイブを取得します。インストール済みパッケージまたは明示的なチェックアウトを使ってオフラインでビルドする場合は、`NEOGRAPH_FETCH_SCHEMAPROVIDER=OFF`を設定してください。CMakeは兄弟ディレクトリのチェックアウト先を推測せず、削除済みの同梱インタープリターも使用しません。

C++20コンパイラー、CMake 3.20以降、SDKランタイムの依存ライブラリが必要です。SDKの依存関係にはOpenSSLとlibcurl 7.88以降が含まれます。NeoGraphのHTTPSコンポーネントを含むフルビルドにはOpenSSL 3が必要です。既定のビルドでは、SQLiteとPostgreSQLの統合も有効になります。以下のコマンドでは、SDKの依存関係を維持したまま、不要なNeoGraphコンポーネントを無効にしています。記録されたSDKインターフェース4の検証は、Linux x86_64とローカルのプロトコル・状態ピアを対象としています。正確な範囲は[SDK検証記録](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/CONFORMANCE.md#interface-4-execution-record)にあります。Windows、macOS、ARM64、HTTP/3、ホストされたベンダー、WASMに対する新しい検証を示すものではありません。プラットフォームとビルドの制約は[トラブルシューティング](docs/troubleshooting.md)を参照してください。

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

## CoreとProgramRuntime

`GraphEngine`はコンパイル済みグラフを実行します。ノードのスケジューリング、状態更新、ルーティング、再試行、ストリーミング、キャンセル、グラフのチェックポイント保存と再開を担当します。コンパイル済みのトポロジーは不変です。サポートされる世代間の移行は、ノードの実行中に任意の変更を加える形ではなく、管理された安全なタイミングで行います。

`ProgramRuntime`は、Coreグラフの呼び出しや子Programの管理ができる、受け入れ済みのProgramを統括します。不変のProgramバージョン、カタログとポリシースナップショット、コマンドジャーナル、子Programの系譜、予算、リプレイ、承認された置き換えや移行を提供します。ホストは実行可能なケイパビリティを登録し、Programをコンパイルして受け入れ、呼び出しを開始します。グラフのノードを実行するのは引き続きCoreです。

調査ワークフローでは、1つのCoreグラフで検索とレビューを実行できます。Programはそのグラフを呼び出し、別々のタスクを担当する子Programを開始し、その結果を待ち、ライフサイクルの遷移を記録できます。プロセス終了後にも復旧するには、ストアの設定と、関連する保管・管理契約を満たす必要があります。インメモリストアはプロセス終了後には残りません。また、ジャーナルだけでは外部ツールの作用が厳密に1回だけ実行されることを保証できません。

QuickJSはProgramを記述するための任意のインターフェースです。Programは制限付きのJavaScript計算と、`callCore`、`spawn`、`await`、`all`、`parallel`、`race`、`quorum`、`emit`、`checkpoint`、`cancelScope`などのジェネレーターコマンドを使用します。ホストはケイパビリティを受け入れ、生成されたソースを公開前に検証します。モデルが生成した提案には、コンパイラー、認証情報、カタログ、権限付与機能へのアクセスを与えません。

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

[Programクイックスタート](examples/63_program_quickstart.cpp)では、インクリメントグラフを呼び出すProgramをコンパイルして受け入れます。期待される出力は`1`です。この例はインメモリストアとC++のProgramビルダーを使用します。JavaScriptによる記述と永続的な実行については、[記述時の境界](docs/QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)、[再帰的なProgram](docs/PROGRAM_RECURSIVE_HARNESSES.md)、[厳密なランタイム契約](docs/STRICT_RUNTIME_INTERPOSITION.md)から読み進めてください。

## 型付きプロバイダー呼び出し

モデル呼び出しでは、検証済みの記述子、ランタイムオプション、型付きリクエストを使用します。記述子の受け入れ対象は、定義済みの項目だけからなるバージョン付きデータです。リクエストとレスポンスを処理するインタープリターは実行しません。認証情報は公開の記述子ファイルではなく、ランタイムオプションに設定します。`SchemaProvider::Defaults`には、型付きのOpenRouterルーティング設定とResponsesの保持設定が含まれます。Images、Veo、Decisionsは、それぞれ専用の型付きクライアントと認可を使用します。

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

準備済みリクエストは1回だけ消費できます。`sp::runtime::Result`は、`Completion`または`Failure`を含む不変の`sp::Outcome`を所有します。順序付きのメッセージとパート、ネイティブの継続情報、生の観測データ、停止の根拠、試行メタデータ、失敗時の部分結果が必要な場合は、その結果オブジェクトを保持してください。使用量カウンターは値がない場合もあります。値がないことは不明を表し、観測されたゼロはゼロのままです。使用量の報告と予算への計上は別々の記録です。可搬な報告データから支出権限を取得することはできません。

`first_call`が戻った後は、`std::get_if<sp::Completion>(result.get())`を使って、完了結果の`messages`、`stop`、`usage`を確認します。完了結果でなければ、`std::get<sp::Failure>(*result)`から`error.kind`、`error.safe_message`、再試行の根拠、`partial`に含まれる部分的なメッセージと使用量を取得できます。たとえば、`completion.usage.output_total`がなければ、出力トークン数は不明です。カウンターがあり、その`value`が`0`なら、ゼロが報告されています。表示用テキストは、保持された結果の一つの表現にすぎません。

`ChatMessage`、`ChatTool`、JSONは可搬な表現です。真正なネイティブ履歴は、ネイティブチェックポイントのサイドカーとともにメモリ内に保持できます。ネイティブ履歴を永続化するには、実物の`sp::NativeArchive`と、所有者専用の非公開領域で保護された保管・管理が必要です。可搬なJSONでは、この権限を再構成できません。アーカイブは独立した鍵で保管・管理の真正性を検証します。暗号化でも、ベンダー発行元の認証でもありません。アーカイブ本体、鍵、ネイティブblob、生の通信観測データを公開しないでください。永続化の失敗、オブザーバー、管理対象の予算バンク、リプレイの境界については、[プロバイダーリファレンス](docs/reference-en.md)と[移行ガイド](docs/migration-v0.4-to-v1.0.md)を参照してください。

型付きAPIへの切り替えに伴い、`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、記述子インタープリター、ResponsesのWebSocket経路は削除されました。C++側の利用コードを再コンパイルし、独自のプロバイダーを移行してください。互換エイリアスはありません。SDKパッケージは`0.1.0`で、インターフェースはアルファ版、インターフェースリビジョンは4、共有ABIは4です。対応するリビジョンが一致している必要があり、これらの番号はSDKインターフェースが安定版であることを示しません。

## Python

```bash
pip install neograph-engine
```

ここで説明する型付きプロバイダーAPIはNeoGraph `0.13.0`を対象としています。過去のwheelは旧インターフェースを公開しています。[Pythonバインディングガイド](docs/python-binding.md)では、ソース側のAPIとビルドの前提条件を説明しています。

次のグラフにはAPIキーが不要です。

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

期待されるメッセージ内容は`Hello, NeoGraph!`です。Pythonでは、Programのコンパイルと実行、Hooks、ランタイムコンテキスト要件、厳密なプロファイル、SQLiteによる永続化、保存したチェックポイントからの正確な再開も公開しています。型付きプロバイダーでは`ProviderMessage`と`make_provider_request`を使い、続いて`prepare`/`dispatch`または`invoke`を呼び出します。結果は、ネイティブ側が所有する完了または失敗の根拠を保持します。これらのメッセージは、グラフで扱いやすくするための`ChatMessage`値とは異なります。ブロッキングするプロバイダー呼び出しはGILを解放します。`asyncio.to_thread`を使うと、イベントループのスレッド以外で実行できます。プロバイダーとProgramの完全な入力例は[Pythonサンプル](bindings/python/examples/README.md)を参照してください。

## 適したワークロードと制約

NeoGraphは、明示的な状態遷移、分岐やループ、並列タスク、チェックポイント、人によるレビューを持つワークフローに適しています。小さな固定グラフをC++アプリケーションに組み込むこともできます。ノード内部の計算処理はアプリケーションが担当します。グラフランタイムはモデルの学習を行わず、数値計算ライブラリを置き換えるものでもありません。モデル呼び出しには、引き続きプロバイダーの遅延、可用性、費用の制約があります。

ランタイムは、グラフ全体とノードごとの再試行ポリシー、容量制限付きの再利用可能なノードキャッシュ、`Send`によるファンアウト、`Command`によるルーティング、サブグラフ、状態履歴、フォーク、HITL、`NodeInterrupt`をサポートします。MCP、A2A、ACP、gRPC、オブザーバビリティ統合は任意のコンポーネントです。ランタイムコンテキストとHooksでは、特定のディスパッチ入力を必須にしたり、配信の証拠を記録したりできます。ただし、その検査はモデルがすべてのトークンに注意を向けたことを証明しません。永続化されたネイティブ状態の復旧や、予算制約付きのフォークには、元の共有会計権限が必要です。スナップショットをコピーしても、予算は再付与されません。

ワークロードは、実際に使うノード、プロバイダー、ストア、並行度、ビルドプロファイルで測定してください。[ベンチマーク](benchmarks/README.md)と[性能ガイド](docs/performance-deep-dive.md)は、測定した構成と制約を説明しており、あらゆる条件での高速性を主張するものではありません。単一構成のビルドで最適化された実行を測定する場合は、`CMAKE_BUILD_TYPE=Release`を指定してください。`NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION=ON`は、対応するコンパイラーでホスト固有の最適化を追加します。配布するバイナリでは無効にしておいてください。

## ビルド設定と関連資料

| オプション | 用途 |
|---|---|
| `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` | SDKのチェックアウト先を明示的に指定します。インストール済みパッケージの検索やアーカイブ取得より優先されます。 |
| `NEOGRAPH_FETCH_SCHEMAPROVIDER` | インストール済みパッケージがない場合に、バージョンを固定した公開SDKアーカイブを取得します。既定では有効です。オフラインビルドでは無効にしてください。 |
| `NEOGRAPH_BUILD_PROGRAM` | Programランタイム、カタログ、系譜、移行。既定では無効です。 |
| `NEOGRAPH_BUILD_QUICKJS_CONTROL` | 組み込みQuickJSによるProgram記述。既定では無効です。 |
| `NEOGRAPH_BUILD_PYBIND` | Python拡張。既定では無効です。 |
| `NEOGRAPH_BUILD_LLM` | NeoGraphのモデル呼び出しアダプター。無効にしてもSDKへの依存はなくなりません。 |
| `NEOGRAPH_BUILD_SQLITE` / `NEOGRAPH_BUILD_POSTGRES` | 任意の永続ストア。両方とも既定では有効です。 |
| `NEOGRAPH_BUILD_MCP_CLIENT` / `NEOGRAPH_BUILD_MCP_SERVER` | MCPクライアントとサーバーのコンポーネント。 |
| `NEOGRAPH_BUILD_A2A` / `NEOGRAPH_BUILD_ACP` / `NEOGRAPH_BUILD_GRPC` | プロトコル統合。gRPCは既定では無効です。 |
| `NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION` | 最適化構成でホスト固有の調整を行います。可搬性はありません。既定では無効です。 |

インストール済みパッケージを使う側は、有効になっている必要なコンポーネントだけをリンクします。

```cmake
find_package(SchemaProvider 0.1.0 CONFIG REQUIRED COMPONENTS runtime)
find_package(NeoGraph CONFIG REQUIRED)
target_link_libraries(app PRIVATE neograph::core neograph::llm SchemaProvider::runtime)
```

- [概念とグラフの意味論](docs/concepts.md)
- [C++リファレンス](docs/reference-en.md)と[Pythonバインディングガイド](docs/python-binding.md)
- [非同期ガイド](docs/ASYNC_GUIDE.md)と[並行処理・キャンセル](docs/concurrency.md)
- [ランタイムコンテキストと厳密なインターポジション](docs/STRICT_RUNTIME_INTERPOSITION.md)
- [Harness MCP](docs/HARNESS_MCP.md)と[QuickJSによる記述](docs/QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)
- [移行ガイド](docs/migration-v0.4-to-v1.0.md)と[トラブルシューティング](docs/troubleshooting.md)
- [C++サンプル](examples/README.md)、[Pythonサンプル](bindings/python/examples/README.md)、[ベンチマークの測定方法](benchmarks/README.md)

## ライセンス

MITです。[LICENSE](LICENSE)を参照してください。第三者に関する告知は[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)にあります。
