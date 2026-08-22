<!-- neograph-i18n: source=README.md locale=ja source_sha256=6ba467cfa403c387e0a433c35a7d0002d1579850b8820d50544b399c8cadb239 -->
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

```bash
git clone https://github.com/fox1245/NeoGraph.git
cd NeoGraph
cmake -S . -B build -DNEOGRAPH_BUILD_EXAMPLES=ON
cmake --build build --parallel
./build/example_core_quickstart
```

完全なソースは[examples/62_core_quickstart.cpp](examples/62_core_quickstart.cpp)にあります。これは1つのC++ノードを登録し、厳格なグラフをコンパイルし、それを実行し、型付きチャネルを読み取ります。

必要に応じてプログラム可能な制御プレーンを有効にする：

```bash
cmake -S . -B build-program \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON \
  -DNEOGRAPH_BUILD_EXAMPLES=ON
cmake --build build-program --parallel
./build-program/example_program_quickstart
```

[examples/63_program_quickstart.cpp](examples/63_program_quickstart.cpp)および[QuickJSオーサリング境界](docs/QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)を参照してください。

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

## Python

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

Core専用ユーザーはProgramやQuickJSの費用を負担しません：

```bash
cmake -S . -B build-core \
  -DNEOGRAPH_BUILD_PROGRAM=OFF \
  -DNEOGRAPH_BUILD_LLM=OFF \
  -DNEOGRAPH_BUILD_MCP=OFF
```

重要なオプション：

| オプション | 目的 |
|---|---|
| `NEOGRAPH_BUILD_PROGRAM` | 永続的なProgram値、カタログ、ランタイム、系統、移行 |
| `NEOGRAPH_BUILD_QUICKJS_CONTROL` | QuickJS Programの作成およびジェネレーターコマンド |
| `NEOGRAPH_BUILD_PYBIND` | `neograph-engine` Python拡張 |
| `NEOGRAPH_BUILD_SQLITE` | SQLiteチェックポイント、コンテキスト、Hookおよびプロバイダーレシートストア |
| `NEOGRAPH_BUILD_POSTGRES` | PostgreSQLチェックポイントおよびProgram永続化コンポーネント |
| `NEOGRAPH_BUILD_MCP_CLIENT` / `SERVER` | MCPクライアントおよびサーバーの役割 |
| `NEOGRAPH_BUILD_A2A` / `ACP` / `GRPC` | オプションのプロトコル統合 |

デプロイメントに一致する狭いCMakeターゲットを使用してください: `neograph::core`、`neograph::llm`、`neograph::program`、`neograph::mcp`、`neograph::a2a`、またはその他の有効なコンポーネント。

## 検証

リポジトリは、決定的なC++およびPythonスイート、Programリプレイ/移行プローブ、DSL機能フィクスチャ、ドキュメント/i18nチェック、サニタイザー、およびオプションのライブモデル評価を実行します。ベンチマークの主張は、時代を超えたAPI保証ではなく、[benchmarks](benchmarks/README.md)および日付入りの[performance report](docs/performance-deep-dive.md)に属します。

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
