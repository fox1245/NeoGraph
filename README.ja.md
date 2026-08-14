<!-- neograph-i18n: source=README.md locale=ja source_sha256=6c67c286aae76e1f4dcc6a25b9e04af02b9d362083721bce943f3fddb381b168 -->
<p align="center">
  <h1 align="center">NeoGraph</h1>
  <p align="center">
    <strong>C++ グラフエージェントエンジン — Python バインディング付き。</strong><br>
    LangGraph 級の機能 · 5&nbsp;µs エンジンオーバーヘッド · Raspberry&nbsp;Pi に収まる静的バイナリ 1 つ。
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
  <a href="#use-from-a-cmake-project">CMake</a> &middot;
  <a href="#python">Python</a> &middot;
  <a href="docs/concepts.md">コンセプト</a> &middot;
  <a href="examples/README.md">サンプル</a> &middot;
  <a href="docs/troubleshooting.md">トラブルシューティング</a> &middot;
  <a href="docs/reference-en.md">API リファレンス</a> &middot;
  <a href="#vs-langgraph">vs LangGraph</a>
</p>

---

<p align="center">
  <a href="docs/videos/neograph-promo.mp4">
    <img src="docs/images/neograph-promo.gif" alt="NeoGraph promo — 5µs engine overhead, 5.5MB RSS at 10K concurrent, 1.2MB static binary, fits Raspberry Pi" width="900">
  </a>
</p>

## NeoGraph とは？

NeoGraph は **C++20 グラフベースエージェントオーケストレーションエンジン**です。
LangGraph 級の機能を C++ にもたらします。エージェントワークフローを JSON で定義し、
並列ファンアウトで実行し、タイムトラベルデバッグや Human-in-the-Loop のために
状態をチェックポイントし、任意の LLM プロバイダを接続 — すべて Python 不要で。

```cpp
#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/graph/react_graph.h>

auto provider = neograph::llm::OpenAIProvider::create({
    .api_key = "sk-...", .default_model = "gpt-4o-mini"
});
auto engine = neograph::graph::create_react_graph(provider, std::move(tools));

neograph::graph::RunConfig config;
config.input = {{"messages", json::array({{{"role","user"},{"content","Hello!"}}})}};
auto result = engine->run(config);
```

上記のエージェントは実際にはエンジンが実行する JSON に過ぎません — JSON を
入れ替えれば別のエージェントになります (参照: [`docs/concepts.md`](docs/concepts.md)):

```json
{
  "schema_version": 1,
  "channels": { "messages": {"reducer": "append"}, "__route__": {"reducer": "overwrite"} },
  "nodes": {
    "planner":    {"type": "llm_call"},
    "researcher": {"type": "tool_dispatch"},
    "classifier": {"type": "intent_classifier", "routes": ["deep_dive", "summarize"]}
  },
  "edges": [
    {"from": "__start__", "to": "planner"},
    {"from": "planner", "condition": "has_tool_calls",
     "routes": {"true": "researcher", "false": "classifier"}},
    {"from": "researcher", "to": "planner"},
    {"from": "classifier", "condition": "route_channel",
     "routes": {"deep_dive": "__end__", "summarize": "__end__"}}
  ]
}
```

**NeoGraph は C++ 向け唯一のグラフエージェントエンジンです。** ロボティクス、
組み込みシステム、ゲーム、高頻度取引、あるいは Python が使えないあらゆる場面で
エージェントを構築するなら — これがそのエンジンです。

## 4 つの軸

各行は 1 コマンドで試せます — セットアップ不要、実 LLM バリアント以外は API キー不要。

|   | 軸 | 実測値 | 詳細 |
|---|---|---|---|
| ⚡ | **パフォーマンス** | 5 µs エンジンオーバーヘッド · 10 K 同時 5.5 MB · p99 7 µs @ 10 K (1 CPU sandbox) | [パフォーマンス詳細](docs/performance-deep-dive.md) |
| 🧬 | **自己進化** | LLM 判定 → `graph_def` ホットスワップ · 5 顧客 → 3 創発的トポロジクラスタ | [self_evolving_chatbot](examples/cookbook/self_evolving_chatbot/) |
| 🔌 | **組み込み対応** | 1.2 MB 削除済み静的バイナリ · `libc.so.6` のみ · RPi Zero 2W で動作 | [組み込み / ロボティクス](docs/performance-deep-dive.md#what-the-numbers-mean-for-embedded--robotics) |
| 🪶 | **軽量** | 2 直接 wheel 依存 · 1 K 顧客マルチテナント → 29 MB · t2.micro 対応 | [multi_tenant_chatbot](examples/cookbook/multi_tenant_chatbot/) |

### ベンチマーク

同一トポロジ、ゼロ I/O エンジンオーバーヘッド — ノードディスパッチ + 状態書き込み +
リデューサ呼び出しのみ (µs/iter、低いほど良い):

| フレームワーク | `seq` (3 ノード) | `par` (ファンアウト 5) | vs. NeoGraph |
|---|--:|--:|--:|
| **NeoGraph master** | **5.0 µs** | **11.8 µs** | 1× |
| Haystack 2.28 | 144 µs | 290 µs | 29× |
| pydantic-graph 1.85 | 236 µs | 286 µs | 47× |
| LangGraph 1.1.9 | 657 µs | 2,349 µs | 131× |
| LlamaIndex 0.14 | 1,780 µs | 4,684 µs | 356× |
| AutoGen 0.7.5 | 3,209 µs | 7,293 µs | 642× |

N=10,000 同時 (1 CPU / 512 MB sandbox): NeoGraph 52 ms / 7 µs p99 /
5.5 MB · LangGraph 23.4 s / 416 MB · LlamaIndex & AutoGen OOM キル。
完全なマトリックス + 手法: [`docs/performance-deep-dive.md`](docs/performance-deep-dive.md)
· [`benchmarks/README.md`](benchmarks/README.md)。

<a id="quick-start"></a>
## クイックスタート

**要件** — C++20 コンパイラ (GCC 13.3 コアグリーン; GCC 14.2+ / Clang 18+ /
MSVC 2022 は全機能対応)、CMake 3.16+、Python 3 (ビルド時コード生成)。デフォルト
オプションでは configure ステップに OpenSSL、SQLite3、libpq、libcurl の
**開発用** パッケージが必要です (ランタイム `.so` のみでは `find_package` を
満たせません):

```bash
# Ubuntu / Debian
sudo apt install libssl-dev libsqlite3-dev libpq-dev libcurl4-openssl-dev
# macOS (SQLite ships with the system)
brew install openssl libpq curl
```

Postgres / SQLite チェックポイントや HTTP/2 バックエンドが不要な場合は、
パッケージをスキップして代わりに `-DNEOGRAPH_BUILD_POSTGRES=OFF
-DNEOGRAPH_BUILD_SQLITE=OFF -DNEOGRAPH_USE_LIBCURL=OFF` で configure。

**プラットフォーム** — Linux x86_64 **GA** (リファレンス、429/429 ctest、サニタイザクリーン);
macOS arm64、Linux ARM64、Windows MSVC 2022 **ベータ**。プラットフォームごとの根拠は
[`CHANGELOG.md`](CHANGELOG.md) を参照。

```bash
git clone https://github.com/fox1245/NeoGraph.git
cd NeoGraph
cmake -S . -B build
cmake --build build -j$(nproc)

# Run an example — no API key needed:
./build/example_custom_graph      # mock ReAct agent
./build/example_parallel_fanout   # parallel fan-out/fan-in
./build/example_send_command      # dynamic Send + Command routing
```

実 LLM に対して実行 — API を使用する全サンプルは cwd から `.env` を自動読み込み
(バンドル `cppdotenv`):

```bash
echo "OPENAI_API_KEY=sk-..." > .env
./build/example_react_agent
```

<a id="use-from-a-cmake-project"></a>
## CMake プロジェクトから使用

`pip install` は Python 専用 (C++ ヘッダなし)。C++ の場合、`FetchContent` は
CMake における `pip install` のように動作します:

```cmake
include(FetchContent)
FetchContent_Declare(NeoGraph
    GIT_REPOSITORY https://github.com/fox1245/NeoGraph.git
    GIT_TAG        master)
# Optional: trim heavy components you don't need.
set(NEOGRAPH_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(NEOGRAPH_BUILD_PYBIND   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(NeoGraph)

add_executable(my_agent main.cpp)
target_link_libraries(my_agent PRIVATE neograph::core neograph::llm neograph::a2a)
```

以上で統合完了。初めての場合、
[**最初の 30 分でハマる 5 つの落とし穴**](docs/troubleshooting.md) (チャネルアクセサ形状、
`neograph::graph::` サブ名前空間、`<httplib.h>` OpenSSL マクロ、
GCC 13 コルーチン ICE、…) を読めばデバッグ時間を節約できます。全ビルド
オプションと CMake ターゲット: [`docs/reference-en.md`](docs/reference-en.md)。

## Python

同じ C++ エンジン、`pip` インストール可能でノートブック、Gradio、FastAPI
サービスから駆動:

```bash
pip install neograph-engine
```

```python
import neograph_engine as ng

definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "demo",
    "channels": {"messages": {"reducer": "append"}},
    "nodes":    {"llm": {"type": "llm_call"}},
    "edges":    [{"from": ng.START_NODE, "to": "llm"},
                 {"from": "llm", "to": ng.END_NODE}],
}
engine = ng.GraphEngine.compile(definition, ng.NodeContext())
result = engine.run(ng.RunConfig(thread_id="t1", input={"messages": [...]}))
```

リリースごとに 20 wheel + sdist (Linux x86_64/aarch64、macOS arm64、Windows x64 ·
Python 3.9–3.13)。完全ガイド — 実 LLM での ReAct、非同期、カスタムリデューサ、
LangGraph 差分リスト、可観測性、Docker 不要デプロイ:
[`docs/python-binding.md`](docs/python-binding.md)。

## 機能

**コアエンジン (`neograph::core`)** — JSON 定義グラフ (ワークフロー変更に再コンパイル不要) ·
Pregel スーパーステップ実行 (サイクル対応) · 並列ファンアウト/ファンイン ·
`Send` (動的ファンアウト) + `Command` (ルーティング+状態上書き) · チェックポイント +
HITL (`interrupt_before/after`、`resume()`、`NodeInterrupt`) · `get_state` /
`update_state` / `fork` / タイムトラベル · リトライポリシー · ストリームモード · サブグラフ ·
インテントルーティング · スレッド間 `Store` · `NodeFactory` によるカスタムノード ·
非同期ネイティブ (`run_async` / `run_stream_async`) · 協調的 `CancelToken` ·
履歴圧縮 · ノード毎キャッシュ · `NodeFactory::export_schema()` (バージョン固定
ビジュアルエディタを駆動)。ビルトイン **OpenInference トレーサー**、追加リンク不要。

**LLM プロバイダ (`neograph::llm`)** — `OpenAIProvider` (OpenAI/Groq/Together/
vLLM/Ollama — あらゆる OpenAI 互換 API) · `SchemaProvider` (Claude、Gemini、または
JSON スキーマによる任意のカスタムベンダー) · ストリーミング付き ReAct `Agent` ループ。

**統合** — MCP クライアント (`neograph::mcp`、HTTP + stdio) · ローカル MCP サーバー
(`neograph::mcp_server`、stdio) · オプトイン Streamable HTTP サーバー
(`neograph::mcp_http_server`) · SQLite Harness レコード
(`neograph::mcp_sqlite`) · コンパイラ支援マルチワーカー
[Harness MCP](docs/HARNESS_MCP.md) · Agent-to-Agent
(`neograph::a2a`、サーバー + クライアント + 呼出ノード) · Agent Client Protocol
(`neograph::acp`、エディタ駆動) · gRPC サービス (`neograph::grpc`、オプトイン) ·
非同期 HTTP/HTTPS/WS + SSE (`neograph::async`)。

**永続状態** — `PostgresCheckpointStore`、`SqliteCheckpointStore`、
`InMemoryCheckpointStore` を 1 つの `CheckpointStore` インターフェース背後に
(すべて Python バインディング済み)、加えて不変 Harness アーティファクトと
再起動安全な実行レコード用の `SqliteHarnessRecordStore`。
`neograph::util` にロックフリー `RequestQueue` + `AsyncTool`。

`NEOGRAPH_BUILD_MCP` は両方の MCP ロールの互換性アンブレラとして維持。
狭いビルドには `NEOGRAPH_BUILD_MCP_CLIENT` または `NEOGRAPH_BUILD_MCP_SERVER` を
使用。stdio サーバー専用ターゲットは `neograph::async` や OpenSSL を要求しません。
リモート HTTP には `NEOGRAPH_BUILD_MCP_HTTP_SERVER` を明示的に有効化。

全機能リストと 55+ 実行可能サンプル:
[`examples/README.md`](examples/README.md)。

## アーキテクチャ

`GraphEngine` は 4 つの専用・独立単体テスト済みクラスに委譲する薄い
スーパーステップオーケストレータです:

- **`GraphCompiler`** — 純粋な `JSON → CompiledGraph` パーサー。
- **`Scheduler`** — シグナルディスパッチルーティング + バリア蓄積。
- **`NodeExecutor`** — リトライループ、並列ファンアウト (`asio::make_parallel_group`)、`Send` ディスパッチ。
- **`CheckpointCoordinator`** — `(store, thread_id)` ファサード背後での保存/再開/保留書き込み。

`neograph::core` はネットワーク依存ゼロ (`yyjson` + ヘッダオンリー `asio`)。
`httplib` は `llm`/`mcp` に PRIVATE であり、あなたのコードに露出することは決してありません。
2 つの並行モデルを同梱 — スレッド毎エージェント (同期) と
コルーチン非同期 (1 つの `asio::io_context` で数千エージェント)。詳細:
[`docs/reference-en.md` §7b](docs/reference-en.md#7b-engine-internals) ·
[`docs/concurrency.md`](docs/concurrency.md) · [`docs/ASYNC_GUIDE.md`](docs/ASYNC_GUIDE.md)。

## vs LangGraph

| | LangGraph (Python) | NeoGraph (C++) |
|---|---|---|
| エンジン | StateGraph | GraphEngine |
| チェックポイント / HITL / fork / タイムトラベル | あり | あり (+ `NodeInterrupt`) |
| 並列ファンアウト | 静的 | `make_parallel_group` (+ オプトイン `asio::thread_pool`) |
| Send / Command | あり | `NodeResult::sends` / `::command` |
| マルチ LLM | LangChain 必須 | `SchemaProvider` ビルトイン (3 ベンダー) |
| MCP | 別実装 | ビルトイン |
| ランタイム / メモリ | Python GIL · ~300 MB+ | C++20 コルーチン + asio · ~10 MB |
| エッジ / 組み込み | 不可 | Raspberry Pi、Jetson、IoT |

LangGraph が顧客ごとにプロセスを必要とするマルチテナント形状 (StateGraph
は Python オブジェクト) を、NeoGraph は graph-as-JSON として 1 プロセスで提供 —
[マルチテナント](examples/cookbook/multi_tenant_chatbot/) と
[自己進化](examples/cookbook/self_evolving_chatbot/) クックブックがその理由を示します。

## 謝辞

[LangGraph](https://github.com/langchain-ai/langgraph)、
[agent.cpp](https://github.com/mozilla-ai/agent.cpp)、
[asio](https://think-async.com/Asio/) (3.0 エンジンランタイム)、
[Clay](https://github.com/nicbarker/clay) に触発されました。

## ライセンス

MIT — [LICENSE](LICENSE) 参照。サードパーティ: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)。
