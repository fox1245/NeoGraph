<!-- neograph-i18n: source=README.md locale=ja source_sha256=73d8153a6b0ecc5957982362ae8429663ac2730be7c65242cb1c4b1031fc170c -->
<p align="center">
<h1 align="center">NeoGraph</h1>
  <p align="center">
<strong>高速なC++グラフランタイムと、永続的なプログラマブルエージェント制御プレーンを備えています。</strong><br>
レイテンシが重要になる場合の静的Core実行。制御が重要になる場合のQuickJS Program、サブエージェント、Hook、ランタイムコンテキスト、検証済みトポロジー進化。
  </p>
</p>

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

<p align="center">
  <a href="https://pypi.org/project/neograph-engine/"><img alt="PyPI" src="https://img.shields.io/pypi/v/neograph-engine?label=pip%20install%20neograph-engine&color=blue"></a>
  <a href="https://pypi.org/project/neograph-engine/"><img alt="Python versions" src="https://img.shields.io/pypi/pyversions/neograph-engine"></a>
  <a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-MIT-green.svg"></a>
</p>

<p align="center">
<a href="#quick-start">クイックスタート</a> &middot;
<a href="#two-runtime-layers">アーキテクチャ</a> &middot;
<a href="#python">Python</a> &middot;
<a href="examples/README.md">例</a> &middot;
<a href="docs/reference-en.md">C++リファレンス</a> &middot;
<a href="docs/python-binding.md">Pythonリファレンス</a>
</p>

---

<p align="center">
  <a href="docs/videos/neograph-promo-v3.mp4">
    <img src="docs/images/neograph-promo-v3.gif" alt="NeoGraph — generated Programs, semantic admission, runtime topology, Hooks, context and Python parity" width="900">
  </a>
</p>

## 今日のNeoGraphとは

NeoGraphには、意図的に分離された2つの実行レイヤーがあります：

| レイヤー | それを使用する | コントラクト |
|---|---|---|
| **GraphEngine / Core** | 固定またはホスト選択のグラフ、低オーバーヘッド、組み込みデプロイメント | 不変のコンパイル済みトポロジー；C++ノードはPregelスタイルのスーパーステップを通じて実行される |
| **ProgramRuntime / QuickJS** | ランタイム制御、子Program、構造化並行性、トポロジー置換と移行 | 不変のProgram世代；永続的な型付きコマンド；ジャーナル化された遷移とリプレイ |

モデルはコンパイラ、カタログ、資格情報、移行、または権限付与を受けるアクセスを一切受け取らない。生成されたソースは以下の通り：

```text
proposal → reserve → compile → semantic validate → admit → publish → migrate or spawn
```

拒否された提案は`ProgramVersion`を公開できず、その動的コンパイル予算も復元されません。[厳格なランタイムインターセプション](docs/STRICT_RUNTIME_INTERPOSITION.md)および[DSL能力評価](docs/DSL_CAPABILITY_EVAL.md)を参照してください。

<a id="quick-start"></a>
## クイックスタート

### C++ Core

SchemaProvider は `NEOGRAPH_BUILD_LLM=OFF` でも必須の外部 C++ 依存です。Core も所有 typed provider 契約を公開します。SDK runtime package を設置し、その prefix を `SCHEMAPROVIDER_PREFIX` に指定します。以下の configure は `-DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX"` を使います。代わりに `-DNEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR=../SchemaProvider` で checkout を明示できます。推測した sibling checkout や旧 bundled interpreter は自動選択しません。現 SDK runtime/archive は Linux/POSIX で、依存なし・OpenSSL 不要・native Windows/macOS・WASM runtime は約束しません。

```bash
git clone https://github.com/fox1245/NeoGraph.git
cd NeoGraph
cmake -S . -B build -DNEOGRAPH_BUILD_EXAMPLES=ON -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX"
cmake --build build --parallel
./build/example_core_quickstart
```

完全なソースは[examples/62_core_quickstart.cpp](examples/62_core_quickstart.cpp)にあります。これは1つのC++ノードを登録し、厳格なグラフをコンパイルし、それを実行し、型付きチャネルを読み取ります。

必要に応じてプログラム可能な制御プレーンを有効にする：

```bash
cmake -S . -B build-program \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON \
  -DNEOGRAPH_BUILD_EXAMPLES=ON
cmake --build build-program --parallel
./build-program/example_program_quickstart
```

[examples/63_program_quickstart.cpp](examples/63_program_quickstart.cpp)および[QuickJSオーサリング境界](docs/QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)を参照してください。

### パフォーマンスビルド

Ninja や Unix Makefiles などの単一構成ジェネレーターは、
`CMAKE_BUILD_TYPE` が空の場合に最適化レベルを選択しません。NeoGraph は
この構成を警告します。GCC/Clang では QuickJS と NeoGraph が Release の
`-O3 -DNDEBUG` フラグなしでコンパイルされるためです。

GCC または Clang でローカルホスト向けのパフォーマンスビルドを行う場合：

```bash
cmake -S . -B build-performance -G Ninja \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_ENABLE_NATIVE_OPTIMIZATION=ON \
  -DNEOGRAPH_BUILD_BENCHMARKS=ON \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON
cmake --build build-performance --parallel
```

`NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION=ON` は最適化構成に
`-march=native -mtune=native` を追加します。ローカルのスループットは
向上しますが、成果物は非移植になるため、配布用バイナリでは無効にして
ください。Release のハードニングは既定で有効です。

GCC/Clang の最終的な Release プロファイルは、QuickJS に C11、NeoGraph に
C++20、`-O3 -DNDEBUG` を使用します。既定のハードニングには
`-D_GLIBCXX_ASSERTIONS`、`-fstack-protector-strong`、
`-fcf-protection=full`、Linux の `-D_FORTIFY_SOURCE=2`、
および RELRO/NOW リンクが含まれます。LTO とホスト固有のチューニングは
既定では有効になりません。

<a id="two-runtime-layers"></a>
## 2つのランタイム層

### GraphEngine / Core

- 静的および条件付きエッジ、サイクル、バリア、`Send` fan-outおよび`Command`ルーティング;
- チェックポイント/再開、正確なチェックポイント再開、フォーク、状態履歴、HITLおよび`NodeInterrupt`;
- 同期およびコルーチン API、ストリーミング、キャンセルとトークン会計;
- グラフ全体およびノード単位の再試行ポリシー、ジッタ、および境界付き再利用可能ノードキャッシュ;
- カスタムレジストリ、プロバイダー、ツール、MCP、A2A および ACP 統合;
- セーフポイントキャプチャと形状保持型 GraphEngine 生成マイグレーション。

### ProgramRuntime / QuickJS

- 制限付きQuickJS `define()`およびジェネレータ`main(input)`内での標準JavaScript計算
- 封印されたコマンド: `callCore`、`spawn`、`await`、`all`、`parallel`、`race`、`quorum`、`emit`、`checkpoint`、`cancelScope`、および許可されたホスト能力;
- 不変の Program バンドル、バージョン、カタログ、admission プロファイル、およびポリシースナップショット;
- 永続的なコマンドジャーナル、完全一致リプレイ、子の系統、非更新可能な予算、およびプロセスリカバリ;
- チェックポイント置換と制限付きライブ GraphEngine トポロジーマイグレーション;
- 生成された Program の admission 前におけるホスト所有の意味検証。

インストールされたJavaScriptサーフェスは`javascript_authoring_capability_manifest()`を通じて機械可読であり、CIで実際のQuickJSバインディングに対してチェックされます。

## ランタイムの安全性とコンテキスト

NeoGraphは、重要な動作をモデルの裁量の外に移します：

- 不変のRAWメッセージ履歴および`ContextEpoch`選択;
- 派生コンテキスト、必須のSkill、およびハード制約；
- 必須のアーティファクトを正確に保持する保守的な変換レシート；
- ネイティブ、stdio、またはHTTP実行バックエンド上の必須ライフサイクルHook；
- プロバイダーディスパッチと終端結果レシート；
- 永続的なランタイム開発者指示と許可されたトポロジー遷移。

NeoGraphは、構築、admission、ディスパッチ、および証拠の境界を保証します。LLMがすべてのトークンに注意を払ったとは主張しません。
## Typed C++ provider 呼び出し

`SchemaProvider` は承認済み `sp::descriptor::ValidatedDescriptor`、`sp::runtime::Options`、任意の `SchemaProvider::Defaults` を受け取ります。descriptor は closed/versioned データ admission であり、要求/応答 interpreter や任意 primitive registry ではありません。credential は公開 descriptor でなく runtime options に置きます。Defaults は typed OpenRouter routing と Responses 保持 (`responses_store`) のみで、後者は Responses 専用です。Hosted OpenRouter routing・retention・JSON 形式は宣言済み typed 制御です。Images、Veo、Decisions は別の NeoGraph typed client と別の承認を使い SDK chat grant を継承しません。

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

プロバイダー呼び出しは `sp::runtime::Result`、すなわち `sp::Completion` または `sp::Failure` を保持する不変の所有 `std::shared_ptr<const sp::Outcome>` を返します。表示テキストだけでなく結果全体を保持してください。順序付きメッセージ/パート、native continuation、完全な wire envelope、順序付き raw 観測、停止の根拠と実際の試行メタデータは呼び出しとクライアント破棄後も残ります。使用量は根拠・段階・品質付きの nullable `uint64_t` であり、欠落はゼロではなく不明です。失敗も元の部分結果を保持します。`ProviderFailure::outcome()` と `ProviderObserverError::outcome()` は実際の結果を保持し、後者の `cause()` は観測者の例外を保持します。

`ChatMessage` / `ChatTool` と JSON は portable projection であり native 権限ではありません。現在の形式は [`provider-message-v2`](schemas/provider-message-v2.schema.json)、[`runtime-history-record-v2`](schemas/runtime-history-record-v2.schema.json) です。真正の C++ メモリ内 checkpoint sidecar は archive なしで native seal を保持します。永続 native 履歴と bank 参照には実際の `sp::NativeArchive` が必要です。closed v2 / `spna2` は独立キーを使う認証済み owner-private 保護 custody であり、暗号化や vendor-issuer 認証ではありません。archive 本文・キー・native blob・raw wire 観測は公開しません。managed 復旧/fork は charged/reserved/report/dedup の canonical bank を共有し、予算を更新しません。汎用有界永続 fork には外部 host-shared bank/journal が必要で、snapshot コピーから独立支出権限は得られません。


実結果の後に post-effect 精算や terminal receipt 永続化が失敗すると、`ProviderDispatchOutcomePersistenceError::outcome()` は元の不変結果、`cause()` は元の永続例外を保持します。delivery も失敗した場合は `delivery_error()` が元の観測者例外を保持します。永続化成功後の観測者失敗は元の例外を変更せず再送出し、不明/結果なし transport 失敗では outcome を捏造しません。
ソースとバイナリの破壊的変更です。全 C++ 利用者とカスタムプロバイダーを新しい一致したヘッダー/ライブラリで再コンパイルします。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、descriptor interpreter、Responses WebSocket は alias/互換 bridge なしで削除されました。SDK は不安定 `0.0.0`、interface revision 3 / shared ABI 3、out-of-line capability check を使用し、安定リリースの宣言ではありません。現 runtime/archive は Linux/POSIX で、Windows・macOS・WASM runtime の資格検証を意味しません。Python provider binding/wrapper は延期され、この C++ 変更では移植されません。

## Python

> 以下の Python 資料は既存 binding の説明です。provider binding/wrapper は明示的に延期され、typed lossless C++ 移行として移植/実行されていません。旧 wheel の設置から新 C++ provider API は得られません。
Pythonパッケージは同じC++エンジンを使用し、現在はProgram、Hook、厳密コンテキスト、ランタイムポリシー、およびSQLite永続化サーフェスを含みます：

```bash
pip install neograph-engine
```

### 5秒デモ（APIキー不要）

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

Pythonはさらに以下を公開します：

- `RetryPolicy`、ノードごとのランタイムオーバーライド、`RunMetadata`、正確な`resume_from`、および再利用可能なキャッシュスコープ;
- `ProgramSource`、`ProgramRegistryBuilder`、`ProgramCompiler`、`LocalProgramHost`、ハンドルと結果。
- 必須の`HookRuntime`コールバックとフェイルクローズのライフサイクル配信。
- `RuntimeContextRequirements`、`ContextTransformReceipt`、SQLite永続コンテキスト/ディスパッチストア、および`StrictRuntimeProfile`。

[Pythonバインディングガイド](docs/python-binding.md)および[Pythonの例](bindings/python/examples/README.md)を参照してください。

## ビルド設定

Core 専用 build は Program/QuickJS を省きますが SchemaProvider runtime は省けません:

```bash
cmake -S . -B build-core \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DNEOGRAPH_BUILD_PROGRAM=OFF \
  -DNEOGRAPH_BUILD_LLM=OFF \
  -DNEOGRAPH_BUILD_MCP=OFF
```

重要なオプション：

| オプション | 目的 |
|---|---|
| `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` | 明示的な SDK source checkout。未指定時は runtime package が必須。 |
| `NEOGRAPH_BUILD_PROGRAM` | 永続的なProgram値、カタログ、ランタイム、系統、移行 |
| `NEOGRAPH_BUILD_QUICKJS_CONTROL` | QuickJS Programの作成およびジェネレーターコマンド |
| `NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION` | 最適化構成で非移植のホスト固有命令チューニングを有効化 |
| `NEOGRAPH_WARN_ON_UNOPTIMIZED_SINGLE_CONFIG` | 単一構成ビルドで `CMAKE_BUILD_TYPE` がなく、Release 最適化フラグを逃す場合に警告 |
| `NEOGRAPH_BUILD_PYBIND` | `neograph-engine` Python拡張 |
| `NEOGRAPH_BUILD_SQLITE` | SQLiteチェックポイント、コンテキスト、Hookおよびプロバイダーレシートストア |
| `NEOGRAPH_BUILD_POSTGRES` | PostgreSQLチェックポイントおよびProgram永続化コンポーネント |
| `NEOGRAPH_BUILD_MCP_CLIENT` / `SERVER` | MCPクライアントおよびサーバーの役割 |
| `NEOGRAPH_BUILD_A2A` / `ACP` / `GRPC` | オプションのプロトコル統合 |

デプロイメントに一致する狭いCMakeターゲットを使用してください: `neograph::core`、`neograph::llm`、`neograph::program`、`neograph::mcp`、`neograph::a2a`、またはその他の有効なコンポーネント。

SDK imported target は `include/SchemaProvider` include root を提供します。公開例は recipe 専用 helper なしで `<descriptor/descriptor.h>`、`<runtime/client.h>`、`<neograph/llm/schema_provider.h>` を直接使います。

```cmake
find_package(SchemaProvider CONFIG REQUIRED COMPONENTS runtime)
find_package(NeoGraph CONFIG REQUIRED)
target_link_libraries(app PRIVATE neograph::core neograph::llm SchemaProvider::runtime)
```

## 検証

`scripts/test_find_package.sh` は installed-consumer 検査手順であり、存在するだけで現 pass を主張しません。現 SDK ABI3 全再ビルド/CTest は 26/26 pass、shared 設置 consumer は実際の local HTTP 二 turn typed 要求、tool/native/refusal/known-zero 結果と mismatch 拒否を実行しました。NeoGraph・Python・Windows・macOS・WASM・有料 live-provider 互換の資格検証ではありません。NeoGraph 統合検証は別途報告します。

## ドキュメント

- [Concepts](docs/concepts.md)
- [C++リファレンス](docs/reference-en.md)
- [Pythonバインディング](docs/python-binding.md)
- [並行性とキャンセル](docs/concurrency.md)
- [非同期ガイド](docs/ASYNC_GUIDE.md)
- [Harness MCP](docs/HARNESS_MCP.md)
- [QuickJS公開オーサリング境界](docs/QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)
- [厳格なランタイムインターセジション](docs/STRICT_RUNTIME_INTERPOSITION.md)
- [Troubleshooting](docs/troubleshooting.md)
- [例](examples/README.md)

## ライセンス

MIT — [LICENSE](LICENSE) を参照してください。サードパーティの通知: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)。
