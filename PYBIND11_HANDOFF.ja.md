<!-- neograph-i18n: source=PYBIND11_HANDOFF.md locale=ja source_sha256=ef50f7757126e94faa6a671e4cc22a02c5c49481f94e6d0652d812d1c4734b07 -->
# Pybind11 バインディング — 次回セッション引き継ぎ

**Languages:** [English](PYBIND11_HANDOFF.md) | [한국어](PYBIND11_HANDOFF.ko.md) | [日本語](PYBIND11_HANDOFF.ja.md) | [简体中文](PYBIND11_HANDOFF.zh-CN.md)

> **履歴引き継ぎ:** ここで記述されたバインディングは出荷済みです。現在の
> カスタム Python ノードは `run(input)` を実装し、レガシーな `execute*` ノード
> インターフェースは v0.9.0 で削除されました。サポートされる API については
> `docs/python-binding.md` と `docs/migration-v0.4-to-v1.0.md` を参照してください。

次回 NeoGraph セッションのための進行中計画。次回セッションで読むべき姉妹ドキュメント/
コンテキスト:

- `README.md` — アーキテクチャ、ビルドオプション、BUILD_SHARED_LIBS セクション
  (2026-04-25 追加)
- メモリ: `~/.claude/projects/-root-Coding-NeoGraph/memory/project_neograph.md`
  (プロジェクト概要)、`plan_pybind11_binding.md` (設計ノート)、
  および広域の MEMORY.md インデックス。

## TL;DR — 引き継ぎ手順

1. **設計 + 最初のコミット (~2-4 h)**: 最小限の pybind11 モジュールで
   `Provider`、`Tool`、`GraphEngine::compile`、`RunConfig`、`RunResult` を公開。
   目標: Python スクリプトが `compile(json_def, ctx).run(config)` を呼び出し、
   JSON 状態を往復できること。
2. **カスタム Python ノード (~2-4 h)**: pybind11 トランポリンにより
   `neograph.GraphNode` の Python サブクラスが `run(input)` を実装でき、
   エンジンが適切な GIL 処理の下で Python にディスパッチする。
3. **Wheel パッケージング (~1-2 d)**: manylinux + macOS arm64 + Win wheels が
   `libneograph_*.so` とバインディングを同梱。先に着地した共有ライブラリ作業
   (commit 85619e6) と相乗効果 — wheel インストールは本質的に動的リンクのため、
   SHARED が正しいモード。

## なぜこれが重要か

売り文句は *「LangGraph 級の Python エルゴノミクス + NeoGraph の µs レイテンシを
1 つの import で」* です。現在 NeoGraph ユーザーには 2 つの選択肢があります:
C++ でエンドツーエンドに書くか (Python に習熟した大多数のエージェント開発者には
現実的な障壁)、NeoGraph を完全に無視するかです。Pybind11 はエンジンをフォーク
することなくこのギャップを埋めます。

純粋な LangGraph と比較した具体的な利点:
- **エンジンオーバーヘッドが 130~640 倍削減** — README.md の「エンジンオーバーヘッド
  vs Python グラフ/パイプラインフレームワーク」ベンチマークを参照。
  NeoGraph の 5 µs/スーパーステップ vs LangGraph の 656 µs はバインディング層を
  通しても損なわれません (pybind11 ディスパッチは sub-µs)。
- **`pip install neograph`** — Docker 不要、venv 地獄なし。.so をバンドルした
  自己完結型 wheel。
- **LangChain ユーザーが既に知っている Python プリミティブ** —
  `state.get("messages")`、`ChannelWrite("findings", ...)` など。
- **移行パス** — LangGraph ユーザーはツール統合や LLM クライアントコードを
  書き直すことなくエンジンを交換可能。

## 本日のコンテキスト (2026-04-25)

このセッションで着地し、バインディングが基盤とするもの:

- **BUILD_SHARED_LIBS サポート** (commit `85619e6`、フォローアップ `110a5fb`):
  NeoGraph が Linux/macOS で `.so`/`.dylib` としてビルド可能に。RPATH
  (`$ORIGIN`/`@loader_path`) が適切に配線。330/330 ctest パス。
  下流の re-agent 検証済み。
- **`re-agent` リポジトリ** (`fox1245/re-agent`、プライベート、Phase 3 クローズ):
  並列ファンアウト + チェックポイント + 再開 + デュアルバックエンド、~750 LOC
  単一ファイル C++。pin を NeoGraph `110a5fb` に更新 (commit `2d57787`)。
- **`re-agent-reimpl` リポジトリ** (`fox1245/re-agent-reimpl`、プライベート、
  Phase 4 クローズ): クリーンルーム再実装エージェント。テスト駆動ループが
  crackme01 で 10/10 に 4 イテレーション / $0.025 で収束。

両下流リポジトリはプライベート — 個人用 RE ツールです。
Pybind11 作業は厳密に NeoGraph (公開) に属します。

## Pybind11 設計

### 公開するインターフェース (コミット 1)

薄いマーシャリングによる読み取り専用マッピング。JSON → `py::dict` は境界で発生し、
エンジン内部では発生しません。

```python
import neograph
from neograph.llm import OpenAIProvider

provider = OpenAIProvider(api_key="sk-...", default_model="gpt-4o-mini")

definition = {
    "name": "demo",
    "channels": {"messages": {"reducer": "append"}},
    "nodes": {"llm": {"type": "llm_call"}},
    "edges": [
        {"from": "__start__", "to": "llm"},
        {"from": "llm", "to": "__end__"},
    ],
}

ctx = neograph.NodeContext(provider=provider)
engine = neograph.GraphEngine.compile(definition, ctx)

result = engine.run({"messages": [{"role": "user", "content": "Hi"}]})
print(result.output["channels"]["messages"]["value"])
```

ラップするシンボル:

| C++ 型 / 関数 | Python | 備考 |
|---|---|---|
| `neograph::Provider` | `neograph.Provider` (抽象) | 基底クラス。サブクラスの `OpenAIProvider`、`SchemaProvider` は `neograph.llm` で公開。 |
| `neograph::CompletionParams` | `neograph.CompletionParams` | プレーンデータクラス。 |
| `neograph::ChatCompletion` | `neograph.ChatCompletion` | プレーンデータクラス。 |
| `neograph::Tool` | `neograph.Tool` (抽象) | 純粋 Python ツールサブクラスにはトランポリンを使用 (コミット 2)。 |
| `neograph::graph::NodeContext` | `neograph.NodeContext` | kwargs (`provider=`、`tools=`、`model=`、`instructions=`) から構築可能。 |
| `neograph::graph::GraphEngine` | `neograph.GraphEngine` | 静的 `compile(definition, ctx, store=None)` + インスタンス `run(input)` / `run_async(input)`。 |
| `neograph::graph::RunConfig` | `neograph.RunConfig` | 構築可能、`input` は辞書を受け付ける。 |
| `neograph::graph::RunResult` | `neograph.RunResult` | `.output` はネストされた辞書を公開。 |
| `neograph::graph::ChannelWrite` | `neograph.ChannelWrite` | プレーンデータクラス。 |
| `neograph::graph::Send` | `neograph.Send` | プレーンデータクラス。 |
| `neograph::graph::Command` | `neograph.Command` | プレーンデータクラス。 |
| `neograph::graph::GraphState` | `neograph.GraphState` | カスタムノード内部からの読み取り専用ビュー。`.get(channel)` は JSON 型に応じて `dict`/`list`/`str`/`int` を返す。 |

### カスタム Python ノード (コミット 2)

`neograph.GraphNode` の Python サブクラスを C++ スケジューラに接続するトランポリン:

```python
class MyAnalyzeNode(neograph.GraphNode):
    def __init__(self, provider):
        super().__init__()
        self.provider = provider

    def run(self, input):
        target = input.state.get("target_function")
        # ...do work in Python, possibly calling self.provider.complete(...)
        return [neograph.ChannelWrite("findings", [proposal])]

# Register so the JSON definition can reference it by type name
neograph.NodeFactory.instance().register_type(
    "analyze",
    lambda name, json, ctx: MyAnalyzeNode(ctx.provider),
)
```

GIL 処理 — *これが難しい部分*。2 つのルール:

1. **Python を呼び出さない C++ は GIL を解放する**:
   `engine->run(...)` と `engine->run_async(...)` は
   `py::gil_scoped_release` でラップし、エンジンが稼働中も他の Python
   スレッドが実行を継続できるようにする。
2. **Python ノードを呼び出す C++ は GIL を取得する**:
   ノードトランポリンは Python の `run` メソッドを呼び出す前に
   `py::gil_scoped_acquire` ですべてのディスパッチをラップし、戻り時に解放する。

エンジンは既に Send 分岐を `fan_out_pool_` ワーカースレッドで実行します
(re-agent の `set_worker_count` 作業で追加 — 契約は `graph_engine.cpp:60-62` を参照)。
各ワーカースレッドは Python を呼び出し、独自の GIL 取得が必要になります。
Pybind11 の `gil_scoped_acquire` はそれを再入可能に処理します。

### Wheel パッケージング (コミット 3+)

- `pyproject.toml` に `cmake-build-extension` または `scikit-build-core` を
  使用して CMake 呼び出しを駆動。`scikit-build-core` が現代的なデフォルトで、
  ~30 行の設定。
- Wheel ビルドで `BUILD_SHARED_LIBS=ON` を強制し、バンドルされた
  `libneograph_*.so` 兄弟ファイルがバインディング `.so` と共存するように。
  RPATH `$ORIGIN` を設定 (NeoGraph CMakeLists で既に設定済み)。
- CI で `cibuildwheel` マトリックス: manylinux2014 (x86_64, aarch64)、
  macOS (universal2)、Windows。注意: Windows DLL エクスポートは未配線
  (configure で警告) — Win 行は `NEOGRAPH_API` マクロパスが完了するまで
  **延期**。

## 最初のコミット形状 — 最小限の pybind11 モジュール

コミット 1 で着地するターゲット:

```
bindings/
  python/
    CMakeLists.txt         ← pybind11_add_module(...)
    src/
      module.cpp           ← root PYBIND11_MODULE
      bind_provider.cpp
      bind_graph.cpp
      bind_state.cpp
    pyneograph/
      __init__.py          ← re-exports + version
    tests/
      test_smoke.py        ← compile + run a 2-node graph end-to-end
```

ルート `CMakeLists.txt` での CMake 統合:

```cmake
option(NEOGRAPH_BUILD_PYBIND "Build Python bindings (pybind11)" OFF)
if(NEOGRAPH_BUILD_PYBIND)
    add_subdirectory(bindings/python)
endif()
```

デフォルト OFF により既存の C++ ビルドに影響なし。Wheel ビルドは
scikit-build 設定で ON に切り替え。

## ユーザーへの未解決の質問

コミット 1 が着地する前に確認すべき決定事項:

1. **リポジトリの場所** — バインディングは NeoGraph 内 (`bindings/python/`)
   か、別の姉妹リポジトリ (`pyneograph`) か？
   - リポジトリ内: フィードバックループが緊密、単一の真実源、バージョンが
     常にエンジンと一致。
   - 別リポジトリ: NeoGraph のタグカットと独立した PyPI リリース。
   - 推奨: **リポジトリ内**。リリース面が管理しやすい。

2. **PyPI でのパッケージ名** — `neograph` (クリーンだが名前空間の占有リスクが
   ある) か `neograph-engine` / `pyneograph` (より安全) か？
   - 推奨: まず PyPI を確認し、空いていれば `neograph` を取得。

3. **非同期 API の公開** — `engine.run_async()` を Python で `async def` として
   公開するか (awaitable を返す)、同期の `run()` のみか？
   - 非同期には asio↔asyncio ブリッジが必要 → より複雑。コミット 1 では
     同期優先で十分。

4. **カスタムノード API のエルゴノミクス** — `class MyNode(GraphNode)` の
   トランポリンパターンか、通常の関数をラップするデコレータ (`@neograph.node`) か？
   デコレータはより Pythonic だが `Command` / `Send` の送出が失われる。
   - 推奨: **両方**。トランポリンをプライマリに、デコレータを一般的な
     書き込み専用ケースの糖衣構文として。

## pybind11 コミットの対象外

- **Anthropic / Gemini プロバイダ** — NeoGraph 側の作業であり、
  バインディング作業ではない。バインディング着地時に C++ にまだ存在しなければ、
  存在するまで `neograph.llm` に表示されないだけ。
- **MCP クライアントバインディング** — `neograph::mcp::MCPClient` は
  Python から有用だが、サブプロセス生成の癖により慎重な GIL 処理が必要。
  コミット 4+ に延期。
- **Postgres チェックポイントバインディング** — Python ユーザーには既に
  `psycopg2` / `asyncpg` がある。`PostgresCheckpointStore` のラップは重複。
  SQLite チェックポイントバインディングは価値がある (ワイヤ形式の重複排除が
  重要で、同じスキーマを持つ Python の同等物がないため)。

## 推定総規模

- コミット 1 (基本インターフェース): 2~4 時間
- コミット 2 (Python カスタムノード + GIL): 2~4 時間
- コミット 3 (wheel パッケージング、Linux のみ): 4~8 時間
- コミット 4+ (cibuildwheel マトリックス、MCP バインディング、非同期): 1~2 日
- **Linux で「pip install neograph」が動作するまでの合計**: 1~2 日
- **マルチプラットフォーム wheel までの合計**: 3~5 日

## 引き継ぎの検証

次回セッションで引き継ぐ際、上流状態がドリフトしていないことを健全性チェック:

```bash
cd /root/Coding/NeoGraph
git log --oneline -5
# expect: 110a5fb cleanup, 85619e6 BUILD_SHARED_LIBS, c7ee23e split, ...
git status                  # should be clean
ls build-shared-test/lib*.so 2>/dev/null && echo "shared build cached" \
                              || echo "rebuild needed: cmake -S . -B build-shared-test -DBUILD_SHARED_LIBS=ON -DNEOGRAPH_BUILD_TESTS=ON && cmake --build build-shared-test -j"
```

その後、上記の設計質問から始めてコミット 1 を着地させる。