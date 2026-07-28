<!-- neograph-i18n: source=docs/troubleshooting.md locale=ja source_sha256=6d6541f3291390674910938c7a45100ea84f62b1b7f7dd4e864afee8507ca154 -->
# トラブルシューティング

**Languages:** [English](troubleshooting.md) | [한국어](troubleshooting.ko.md) | [日本語](troubleshooting.ja.md) | [简体中文](troubleshooting.zh-CN.md)

最初に症状を示し、その後に根本原因と修正を行います。何かに当たってしまったら
それはここにはありません。症状に関する問題を開いてください - おそらく
その後このリストに載せてください。

> **5 秒間の健全性チェック。** 何よりもまず確認してください。
> 最新のパッチを適用しています:
> ```bash
> pip install --upgrade neograph-engine
> python -c "import neograph_engine; print(neograph_engine.__version__)"
> ```
> 以下の問題のほとんどは特定のリリースで修正されています。まずアップグレードして、
> 2番目にデバッグします。

---

## インストール/インポート

### `pip install neograph-engine` は成功しますが、`import` は失敗します

Python のバージョンとプラットフォームが一致していない可能性があります。以下のホイールを出荷します。

|プラットフォーム |バージョン |
|---|---|
| Linux x86_64 (manylinux_2_34) | Python 3.9 – 3.13 |
| Linux aarch64 (manylinux_2_34) | Python 3.9 – 3.13 |
| macOS arm64 (14+) | Python 3.9 – 3.13 |
| Windows x64 (MSVC) | Python 3.9 – 3.13 |

この行列の外側にあるものはすべて sdist に送られます (ソース
build)、CMake 3.16+、OpenSSL、および C++20 ツールチェーンが必要です。もし
使用しているプラ​​ットフォームがリストになく、ソースのビルドが失敗します。
問題。

### Linux 上の `ImportError: ... GLIBC_2.32 not found`

Linux ホイールは `manylinux_2_34` です - glibc ≥ 2.34 (Ubuntu 22.04+、
Debian 12 以降、RHEL 9 以降）。古いディストリビューションでは、ソースからビルドします。

### Windows の `ImportError: DLL load failed`

Windows ホイールには独自の依存関係が同梱されていますが、Python のインストールは
ホイール アーキテクチャ (x64) と一致する必要があります。次のように確認します。

```powershell
python -c "import platform; print(platform.architecture())"
```

`('32bit', ...)` と出力される場合は、32 ビット Python を使用しています。
64ビットのもの。

---

## TLS / ネットワーク

### プロバイダー呼び出しが 60 秒間ハングし、その後 `ConnPool::async_post: timeout` のエラーが発生する

**影響を受ける:** `neograph-engine` ホイール v0.1.0 ～ v0.1.6。

**根本原因:** バンドルされた OpenSSL にはコンパイル済みの CA ストア パスが含まれています
`/etc/pki/tls/...` (RHEL 規約) を指します。 Ubuntu、Debian、
macOS では、CA ストアは別の場所 (`/etc/ssl/certs/...`) に存在するため、
Wheel の libssl はピア証明書と TLS ハンドシェイクを検証できません
エラーが発生する前に、要求が完全にタイムアウトになるまで静かに待機します。

**修正 (v0.1.7 以上):** ホイールの `__init__.py` が自動ポイントになるようになりました。
インポート時は `certifi.where()` の `SSL_CERT_FILE`。アップグレード:

```bash
pip install --upgrade neograph-engine
```

**古いホイールの回避策:**

```bash
# Debian / Ubuntu
export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt
# Cross-distro
export SSL_CERT_FILE=$(python -c "import certifi; print(certifi.where())")
```

**v0.1.7+ で自動修正をオプトアウトするには** (例: カスタム CA を使用している場合)
バンドル): インポートする前に `NEOGRAPH_SKIP_CERT_AUTOFIX=1` を設定します。

### `urllib` は機能しますが、NeoGraph は機能しません

上記と同じ根本原因 — `urllib` はシステム OpenSSL を使用しますが、
Wheel は、バンドルされている OpenSSL を間違った CA パスで使用します。同じ修正:
v0.1.7 以上にアップグレードするか、`SSL_CERT_FILE` を設定してください。

### WebSocket 応答 (`use_websocket=True`) は `close=1000` ですぐに閉じます

よくある 3 つの原因を頻度順に示します。

1. **WebSocket アクセスが API キー/組織で有効になっていません。** 一部の OpenAI
   Tier 1 アカウントにはまだ WebSocket モード アクセスがありません。フォールバック
   `use_websocket=False` を設定することによる HTTP/SSE。
2. **特定のプロキシ パスに `User-Agent` ヘッダーがありません。** で修正されました。
   `d7c61d0`をコミットします。 v0.1.4 以上にアップグレードしてください。
3. **`temperature` フィールドは一部の Responses-API モデルで拒否されました。** 同じ
   commit により、サポートされているモデルの WS ハンドシェイクから削除されます。

### WASM 経由でブラウザから実行すると CORS エラーが発生する

WASM ビルドは、ブラウザー CORS のバイパス ヘッダーをまだ実装していません。
ステータスについては [Issue #wasm-cors](../../issues) を追跡します。

---

## グラフのコンパイル/実行

### `RuntimeError: Unknown reducer: <name>`

バインディングには `"overwrite"` と `"append"` の 2 つのレデューサーが付属しています。
それ以外のものは、登録しない限りコンパイルに失敗します。

**カスタム リデューサーを登録します (v0.1.9 以降、Python から):**

```python
ng.ReducerRegistry.register_reducer("sum",
    lambda current, incoming: (current or 0) + incoming)
```

既存の名前を再登録すると、以前のリデューサーが置き換えられます。の
callable は GIL の下で実行されます。同時送信ファンアウトのシリアル化
Python カスタム ノードと同じ方法です。

`"last_value"` (一般的な LangGraph エイリアス) と入力した場合 — それは
`"overwrite"`はこちら。セマンティクスは同じですが、名前は異なります。

### `RuntimeError: Unknown condition: <name>`

組み込み条件: `has_tool_calls`、`route_channel`。別の名前
登録する必要があります。

**カスタム条件を登録します (v0.1.9 以降、Python から):**

```python
def is_long(state):
    msgs = state.get("messages") or []
    return "long" if len(msgs) > 10 else "short"

ng.ConditionRegistry.register_condition("is_long", is_long)
```

呼び出し可能関数はライブ `GraphState` を受信します (`state.get(channel)` / を使用)
`state.get_messages()` が利用可能）、一致する文字列を返す必要があります
条件付きエッジの `routes` キーの 1 つ。

### `RuntimeError: Write to unknown channel: <name>`

`ChannelWrite` のチャンネル名が次のものと一致しません。
`definition["channels"]`。チャンネル名は正確です。 `messages`と
`Messages`は異なります。

### `RuntimeError: Unknown node type: <name>`

いずれかのノードの `type` フィールドが、
工場レジストリ。ビルトイン用（`llm_call`、`tool_dispatch`、
`intent_classifier`、`subgraph`) タイプ名は上に詳しく記載されています。
独自のタイプについては、次のように呼び出す必要があります。
コンパイル前に `ng.NodeFactory.register_type(type_name, factory)`。

### 私の ReAct ループは 1 回だけ実行されます — `execution_trace == ['llm']`

**影響を受ける:** `neograph-engine` ホイール v0.1.0 ～ v0.1.7。

**根本原因:** グラフ コンパイラがトップレベルを削除しました
`conditional_edges`は黙ってブロックします。 README クイックスタートと
すべての Python サンプルはこの形式を使用するため、ReAct ループは次のように縮退します。
単一の LLM 呼び出し (ツールのディスパッチなし)。

**修正 (v0.1.8 以上):** コンパイラは両方の形式 (トップレベル) を受け入れるようになりました。
`conditional_edges` 配列、または `condition` を使用したインラインイン `edges`
分野。次のようにアップグレードして確認します。

```python
result = engine.run(...)
print(result.execution_trace)
# Expected for ReAct: ['llm', 'dispatch', 'llm']
```

**古いホイールでの回避策:** 条件をインラインに置きます:

```python
"edges": [
    {"from": ng.START_NODE, "to": "llm"},
    {"from": "dispatch",    "to": "llm"},
    {"from": "llm",
     "condition": "has_tool_calls",
     "routes": {"true": "dispatch", "false": ng.END_NODE}},
]
# (no separate conditional_edges block)
```

### `result.execution_trace` は空です / 開始ノードのみを表示します

グラフはすぐに `__end__` にルーティングされました。最も一般的な原因:

1. **`__start__` のエッジがありません。** すべてのグラフには少なくとも 1 つは必要です
   `{"from": ng.START_NODE, "to": "..."}`エッジ。
2. **条件付きで `routes` マップにない値が返されました。**
   条件の戻り値がどのキーにも一致しない場合、open または出力契約のない
   条件は明示的な `"default"` ルートを使います。それが `__end__` を指すと
   正常終了します。`"default"` がなければ source node、条件名、返された
   label を含むエラーになります。closed 条件は宣言外の label を必ず拒否します。
3. **`max_steps=0` または `max_steps=1`** — ランは天井に達しました
   すぐに。デフォルトは 25 です。 ReAct ループには通常 10 以上が必要です。

### コンパイル エラー: `RuntimeError: Cycle detected: a -> b -> a`

NeoGraph はサイクルを許可しますが (ReAct ループはサイクルです)、コンパイラは
*無条件* サイクルをキャッチします — 条件なしの `a → b → a`
逃げる。 `__end__` にルーティングできる条件付きエッジを追加します。

---

## パフォーマンス

### ファンアウトが予想よりも遅い

よくある 2 つの原因:

1. **エンジン所有のワーカー プールはありません。** `compile()` のデフォルトは
   `set_worker_count(1)` — プールなし、ファンアウト ブランチはインラインでディスパッチされます
   呼び出し側のエグゼキュータ上で実行され、シリアルに実行されます。プールに一度オプトインします
   `compile()` 後 (および `run()` 前):

   ```python
   engine.set_worker_count(N)        # exact fan-out width
   engine.set_worker_count_auto()    # hardware_concurrency()
   ```

   NeoGraph は、初めての実行時にワンショットの標準エラー出力警告も出力します。
   マルチ送信 (またはマルチ送信エッジ) ファンアウトはプールなしで実行されます。
   したがって、サイレントシリアルのケースが表示されます。で抑制する
   `NEOGRAPH_SUPPRESS_FANOUT_WARNING=1` (worker=1 高速パスの場合)
   意図的な。
2. **Python カスタム ノードは、本体中に GIL** を保持します。もしあなたの
   `@ng.node` 関数は CPU に依存した Python を動作させるため、ファンアウトは高速化されません
   上。 ONNX / PyTorch / numpy / `requests.get` は、実行中に GIL を解放します。
   ネイティブ呼び出しなので、並列化されます。純粋な Python スコアリング ループの場合、
   設定したワーカーの数は関係ありません。

### `bench_neograph par` は 200 マイクロ秒以上を報告します

**v1.0 以前のホイール** v0.1.4 ～ v0.x では、ワーカー プールのデフォルトが維持されていました。
`hardware_concurrency()`、クロススレッド送信コストを支払いました
すべてのファンアウトティック。 v1.0 はデフォルトを `set_worker_count(1)` に戻しました
(プールなし、送信コストなし) — `par` がプレフリップ球場に戻ってきました。
新鮮な`compile()`。プールにオプトインする
`engine.set_worker_count(N)` / `engine.set_worker_count_auto()`のとき
ワークロードのファンアウト ブランチは実際に実際のスレッドの恩恵を受けます
プール (CPU バウンドのボディ、大きなファンアウト幅)。

### ストリーミング コールバックがノードごとに 2 回起動します

**影響:** Python `@ng.node` 書き込み専用ノード。で修正されました
`re-agent` は `2a5c5dc` / `5993935` をコミットし、NeoGraph にレプリケートされます
マスター。

**v1 より前のリリースの根本原因:** 純粋な書き込み `GraphNode` サブクラス (いいえ
`Command`、`Send` はありません) 結果に対して 1 回、ストリームに対して 1 回実行できます
フック。単一の `run(NodeInput)` オーバーライドをアップグレードして実装します。 v1 が呼び出す
このメソッドを 1 回実行すると、オプションのストリーム シンクが `in.stream_cb` として公開されます。

`@ng.node` デコレーター (サブクラス化ではない) を使用している場合、これは次のようになります。
すでに扱われています。

---

## チェックポイント / Postgres

### `PostgresCheckpointStore` が見つからない / インポート エラー

PyPI ホイールは、`PostgresCheckpointStore` が有効になった状態で出荷されます (libpq は
v0.1.3 以降バンドルされています)。 `import neograph_engine; neograph_engine.PostgresCheckpointStore`
直接動作するはずです。

`-DNEOGRAPH_BUILD_POSTGRES=ON` を使用せずにソースからビルドした場合、
クラスはバインディング内に存在しません。フラグを指定して CMake config を再実行します
設定してから再構築します。

### Postgres 接続: `FATAL: password authentication failed`

`PostgresCheckpointStore` 接続文字列は libpq の後に続きます。

```
postgresql://user:password@host:port/dbname
```

パスワードに URL 特殊文字 (`@`、`:`、`/`、`%`) が含まれている場合、URL エンコード
または、`key=value` フォームを使用します。

```
host=localhost user=neo password=p@ss dbname=neograph
```

### 非同期 Postgres の再接続は 30 秒後にタイムアウトになります

非同期の初期接続/置換接続では、1 つの実稼働安全期限が使用されます。
試み全体。に直接書き込まれた正の `connect_timeout=N`
接続文字列は、`connect_timeout=1` を使用して、グローバル バジェットを秒単位で設定します。
PostgreSQL の最小値の 2 秒に切り上げられます。明示的な値が
存在しない、ゼロ、または負の場合、NeoGraph は 30 秒を使用します。 `PGCONNECT_TIMEOUT`と
サービスファイルのタイムアウト値の解決が遅すぎて、最初の非同期を制限できない
接続ステップなので、デフォルトの 30 秒も使用します。値を直接入れる
非同期期限が異なる必要がある場合は、接続文字列内で指定します。

バジェットは、マルチホスト接続文字列内のすべてのホストと解決された IP に及びます。
ホストごとに乗算されません。これは同期とは意図的に異なります
libpq。`connect_timeout` は各ホストに個別に適用されます。同期
`PostgresCheckpointStore`の構築と交換は変更ありません。

たとえば、これにより、完全な非同期置換の試行には 60 秒かかります。

```
host=pg-a,pg-b dbname=neograph connect_timeout=60
```

### ポストグレ `relation "neograph_checkpoints" does not exist`

ストアは、最初の使用時にテーブルを作成します (`CREATE TABLE IF NOT EXISTS`)。
DB ユーザーに CREATE 権限がない場合は、スキーマを手動で実行します。
SQLは[`include/neograph/graph/postgres_checkpoint.h`](../include/neograph/graph/postgres_checkpoint.h)にあります
`kSchema`の下にあります。

---

## 例 / ドッカー

### `docker compose run agent` (例: 26) は PG を見つけることができません

構成ファイルは、`db` サービスが次のように到達可能であることを期待しています。
`postgres://neograph:neograph@db:5432/neograph`。外にいる場合
docker-compose では、代わりに `PG_URL` を到達可能なホストに設定します。見る
[`examples/26_postgres_react_hitl/README.md`](../examples/26_postgres_react_hitl/README.md)
完全な環境テーブルの場合。

### Crawl4AI の例が起動を拒否する

Crawl4AI はオプションの Docker コンテナです。

```bash
docker run -d -p 11235:11235 --shm-size=1g --name crawl4ai \
    unclecode/crawl4ai:latest
```

例 17、25、26 は、`CRAWL4AI_URL` (デフォルト) の場合に正常にフォールバックします。
`http://localhost:11235`) に到達できません。

### `example_clay_chatbot` ビルド ターゲットが見つかりません

例 11 では CMake で `-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON` が必要です
時間を設定します:

```bash
cmake -B build -DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON ..
make example_clay_chatbot
```

Clay (UI レイアウト) + Raylib (レンダラー) をプルします - それが遅れている理由です
旗。

---

## ストリーミングイベント

### `event.node` は `AttributeError` をレイズします

属性は `event.node_name` (C++ フィールド名と一致します) です。同じ
`event.type` (列挙型) と `event.data` (JSON dict) の場合。

```python
def cb(event):
    print(f"{event.type.name} on {event.node_name}: {event.data}")
```

### `StreamMode.TOKENS` コールバックが起動しない

プロバイダーはストリーミングをサポートする必要があります。現在：

|プロバイダー |ストリーミング？ |
|---|---|
| `OpenAIProvider` | ✓ HTTP/SSE |
| `SchemaProvider("openai_responses")` | ✓ SSE |
| `SchemaProvider("openai_responses", use_websocket=True)` | ✓ WS |
| `SchemaProvider("claude")` | ✓ SSE |
|カスタム Python `Provider` サブクラス | `complete_stream` の実装に依存します。 |

カスタム Python `Provider` の場合は、`complete_stream` をオーバーライドします。 Python のサブクラス
非同期仮想オーバーライドを公開しないでください。新しい C++ バックエンドの場合は、次から派生します。
`CompletionProvider` と `do_invoke()` の `request.streaming()` を処理します。既存
C++ `Provider` サブクラスは引き続き `complete_stream()` をオーバーライドする可能性があります。
`complete_stream_async()`。ストリーミング実装がない場合、デフォルトでは次のように出力されます。
収集された応答は増分トークンではなく 1 つのチャンクとして収集されます。

---

## オープンテレメトリ

### OTel スパンが `parent_id=None` で表示されます (1 つではなく 4 つの別個のトレース)

**影響を受けます:** `9073671` をコミットする前の `neograph_engine.tracing`。

**根本原因:** `tracer.start_span` + `use_span(...).__enter__()`
contextvars に依存しているため、
C++ → Python pybind コールバック境界。

**修正:** `otel_tracer` ヘルパーは、次を介して親コンテキストのスナップショットを作成するようになりました。
`set_span_in_context(root_span)` をそれぞれに明示的に渡します
子ノードの `start_span`。 `9073671` 以降にアップグレードします。

独自の OTel 統合を展開している場合も、同じことを行ってください。依存しないでください。
バインディング境界を越える contextvars 上で。

### LLM スパンがノード スパンとは異なるトレース ID で表示される

**影響を受けるもの:** v0.6.0 最終版より前の `neograph_engine.openinference`
(`fa8ed50`をコミット)。

**根本原因:** `openinference_tracer` が `parent_ctx` を設定しました (a
スナップショット) ですが、ノード スパンを現在の OTel として *接続* したことはありません
コンテクスト。したがって、ノード本体が `provider.complete()` を呼び出したとき、
`OpenInferenceProvider` は `llm.complete` スパンを次のようにオープンしました
`tracer.start_as_current_span(...)`、新しいスパンは に戻りました。
グローバル ルートとトレースは、ごとに個別のトレース ID に断片化されます。
LLMコール。

**修正:** `openinference_tracer` は現在、
`otel_context.attach(set_span_in_context(span))` 上の `NODE_START`
そして、結果のトークンをスパンの横に隠します。 `NODE_END` /
`ERROR` / `INTERRUPT` スパンを終了する前にトークンを切り離します。
以前の現在のスパンを復元します。 v0.6.0 で検証済み
Phoenix — `graph.run > node.X > llm.complete` を含む単一のトレース ツリー
階層。

v0.6.0 以降を使用していて、*まだ* 分割トレースが表示される場合は、プロバイダー
ラップされていません - `ctx.provider = OpenInferenceProvider(inner, tracer)` であることを確認してください
`engine.compile(...)` より前に実行されます。それ以外の場合はエンジンがバインドされます。
ラップされていないプロバイダーに送信します。

### `openinference` をインポートすると、`pip install opentelemetry-api` で ImportError が発生します

`neograph_engine.openinference` `opentelemetry` を遅延インポートします。の
ImportError は初回使用時にのみ発生し、1 行のインストール ヒントが表示されます::

    pip インストール opentelemetry-api opentelemetry-sdk

スパンをプッシュする場合は、`opentelemetry-exporter-otlp` を追加します。
OTLP経由のフェニックス/ラングフューズ/テンポ。

### `session.close()` の後にカスタム `Tracer` アダプターがハング/クラッシュ/ガベージを出力する (問題 #24)

`neograph::observability::Tracer` アダプター (C++) を作成しました。
レコードはメモリ内のリストにまたがり、**後**リストを調べます
`OpenInferenceTracerSession::close()` に電話をかけます。散歩にはこう書かれています
メモリを解放しました。

`close()` はルート経由で内部 `unique_ptr<Span>` をリセットします
スパン (およびノー​​ドごとのスパン スタック)。アダプターが配布された場合
返されたラッパー オブジェクトへの **生のポインター**
`start_span`、そのポインタは `close()` の瞬間にぶら下がっています
戻り値 — ラッパーは呼び出し側が所有しており、呼び出し側
ちょうど彼らを解放したばかりです。

**修正:** アダプターは、記録されたスパン データ自体を所有する必要があります。
呼び出し元が所有するラッパーへの生のポインターを追跡するだけです。形状:

```cpp
// Owned by the tracer (lives until tracer drops):
struct RecordedSpan {
    std::string name;
    RecordedSpan* parent = nullptr;
    std::map<std::string, std::string> attrs;
    // ...status, events, ended flag...
};

// Owned by the OpenInference layer (may be reset on close):
class WrapperSpan : public obs::Span {
    RecordedSpan* rec_;        // pointer into the tracer-owned data
public:
    void set_attribute(...) override { rec_->attrs[...] = ...; }
    // ...
};

class MyTracer : public obs::Tracer {
    std::vector<std::unique_ptr<RecordedSpan>> records_;  // ← owns data
    // start_span builds a fresh RecordedSpan, returns a Wrapper
    // pointing at it. Walk records_ for inspection — never the
    // wrappers.
};
```

参照: `tests/test_openinference_cpp.cpp::InMemoryTracer`
(標準テストフィクスチャ) および `examples/49_openinference.cpp::PrintTracer`
(stderr-printing デモ) どちらもこの正確なパターンを使用します。同じ
警告は `Tracer` の `@warning` ブロックにあり、
ヘッダーに`OpenInferenceTracerSession::close()`。

**バグの発生方法:** 観察可能な障害モードには、クリーンな障害が含まれます。
検査ループ内でクラッシュ (最良の場合)、途中でハングアップ
スパン名の出力 (解放されたバッファにはたまたま次のものが含まれていました)
文字列フォーマッタをループするもの）、または単に間違っている
属性値。 3 つはすべて同じ根本原因です。

---

## ビルドエラー

### GCC 13 内部コンパイラ エラー: `build_special_member_call`、`cp/call.cc:11096` (問題 #23)

Ubuntu 24.04 の標準 GCC 13 (または任意の GCC 13.x) を使用しています。ビルド
死ぬ:

```
internal compiler error: in build_special_member_call, at cp/call.cc:11096
```

…コルーチン内で `co_await x.foo_async(...)` を実行する行上
(通常、ラムダ本体は `main()` から `asio::co_spawn` に渡されます)。
これは GCC 13 フロントエンドのバグであり、コードではありません。 GCC 14+、Clang 18+、
および MSVC 19.40 以降はすべて、同じソースを変更せずにコンパイルします。

**3 つのエスケープ** (優先順):

1. **コンパイラをアップグレードします** — `sudo apt install gcc-14 g++-14` 上
   Ubuntu 24.04 (24.10 にはデフォルトで GCC 14 が同梱されます)、その後
   `cmake -DCMAKE_CXX_COMPILER=g++-14 ...`。最もクリーンな修正。しましょう
   自然な方法でコードを記述します。

2. **代わりに `neograph::async::run_sync`** を介してコルーチンを駆動します。
   `main()`からの`asio::co_spawn`。同じ観察可能な動作、いいえ
   フロントエンドICE:

   ```cpp
   // Instead of:
   asio::co_spawn(io,
       [&]() -> asio::awaitable<void> {
           result = co_await tool.execute_async(args);   // ← GCC 13 ICEs here
       },
       asio::detached);
   io.run();

   // Do:
   #include <neograph/async/run_sync.h>
   result = neograph::async::run_sync(tool.execute_async(args));
   ```

   `run_sync` は独自のプライベート `io_context` を構築し、
   完了まで待機可能 — 内部的には同じです
   `co_spawn + io.run()` は可能ですが、通話サイトは
   コンパイラの観点から見て、ICE は決して起動しません。

3. **コルーチンを再構築**して、`co_await` が内部で発生するようにします。
   無料関数やラムダではなく、通常のクラスのメンバー関数
   体。これは場合によっては機能しますが、診断が常に機能するとは限りません。
   適切な形状変化を指摘します。オプション 1 または 2 の方が信頼性が高くなります。

**このリポジトリのどこに食い込むか:** CMakeLists には例ごとの
`example_03` (元の ICE サイト) 周辺のツールチェーン ゲート、および
`examples/50_async_tool.cpp` は次を使用して問題を回避します。
`main()` の `co_spawn` ではなく、`run_sync`。新しいコルーチン
自然な `co_spawn`-from-main 形状に従った例/テスト
同じツールチェーン上の同じ ICE にヒットします — オプション 1 を適用するだけです
または2。

## Python タイプ ID (v0.5.0+)

### `isinstance(params.messages, list)` は False を返します

**影響:** v0.5.0 以降、5 つのベクター プロパティ サーフェス:
`CompletionParams.messages`、`.tools`、`ChatMessage.tool_calls`、
`NodeResult.writes`、`.sends`。

**理由:** v0.5.0 は、`params.messages.append(...)` でのサイレント no-op を修正しました。
これらのベクトルを不透明型としてバインドすることによって
(`PYBIND11_MAKE_OPAQUE` + `py::bind_vector`) したがって、`.append` は変異します
ライブ C++ ベクトル。トレードオフ: プロパティのタイプは現在、
例えば`ChatMessageList` (pybind クラス)。プレーンな Python `list` ではありません。

**まだ機能するもの:**
- `params.messages = [m1, m2]` — `py::implicitly_convertible<py::list, …>`
  割り当て時に Python リストを自動変換します。
- `for m in params.messages` — 反復プロトコル。
- `len(params.messages)`、`params.messages[i]`、`params.messages[i] = m`、
  スライス。
- `params.messages.append(...)`、`.extend(...)`、`.insert(...)`、
  `.pop(...)`、`.clear()` — すべてライブで C++ ベクトルにプッシュスルーされます。

**壊れたもの (まれ):**
- `isinstance(x, list) → False`。本当にプレーンな Python が必要な場合
  リスト、実体化: `list(params.messages)`。
- `json.dumps(params.messages)` — バインドされたクラスは直接バインドされていません
  JSON シリアル化可能。変換: `json.dumps([{"role": m.role,
  "content": m.content} for m in params.messages])`。

`ChatMessage.image_urls` (`std::vector<std::string>`) は *ではありません*
移行済み — `vector<string>` は、
コールサイト スイープなしのグローバル OPAQUE。 `.append()` ノーオペ
文書化された制限として残っています。 v0.6+ 候補 (経由)
`add_image_url()` の便利なメソッド。

---

## ソースからビルドする

### CMake 構成: Windows の `Could NOT find SQLite3`

Windows ホイール ビルドは `-DNEOGRAPH_BUILD_SQLITE=OFF` を設定します。
SQLite は、MSVC ランタイム全体で ABI 互換ではありません。構築している場合
自分で使用するために Windows 上のソースから SQLite をインストールするか、
vcpkg を使用するか、`-DNEOGRAPH_BUILD_SQLITE=OFF` を明示的に渡します。

### CMake 構成: Linux 上の `Could NOT find CURL`

オプションの依存関係。パッケージマネージャー経由でインストールします。

```bash
# Debian / Ubuntu
sudo apt install libcurl4-openssl-dev
# RHEL / Fedora
sudo dnf install libcurl-devel
# macOS
brew install curl
```

または無効にします: `-DNEOGRAPH_USE_LIBCURL=OFF`。 libcurl がないと、
`SchemaProvider` の `prefer_libcurl=True` モード (HTTP/2) は使用できません
— デフォルトの ConnPool (HTTP/1.1) は引き続き機能します。

### Pybind バインディングが未定義の参照とリンクできない

新しいコードを再実行せずにプルした後、`make` を再実行している可能性があります。
CMake。ビルド ディレクトリのコンパイルされたオブジェクト ファイルは、次のシンボルを参照します。
古いヘッダー。 `make clean && make` または削除して再構成する
ビルドディレクトリ。

### スクリプトから A2A サーバーを起動する場合の `OPENAI_API_KEY not set`

`cppdotenv::auto_load_dotenv()` は、バイナリ内の `.env` を読み取ります。
呼び出しますが、ランチャー スクリプトからフォークされた子プロセスが呼び出します。
**まだ**、ランチャーがまだエクスポートしていないものは継承しません。もし
あなたのスクリプトは次のことを行います:

```bash
./member_server 8101 ...   # forks before any env is set up
```

…どの子供も空の環境を見て、始めることを拒否します。ソース
ランチャーで最初に `.env` を実行するため、変数は次のようにエクスポートされます。
フォークを実行するシェル:

```bash
set -a; . ./.env; set +a            # marks every assignment as exported
./member_server 8101 ... &
```

クックブックの `scripts/run_session.sh` には、完全なパターンが示されています。
兄弟 `.env` にフォールバックします。

### マルチペルソナ/マルチプロセス A2A: OpenAI プロバイダーを共有する場所

`OpenAIProvider::create_shared(cfg)` を使用します (`shared_ptr<Provider>` を返します)
`create(cfg)` の代わりに (`unique_ptr` を返します)。共有フォームは、
`NodeFactory` ラムダにキャプチャ可能で、すべてのシステムで再利用可能
グラフ ノードと A2A リクエスト — `create()` の `unique_ptr` は強制します
手動で `release()` して再ラップする必要があります。

---

## C++ コンシューマー — `httplib.h` マクロの一貫性 (耐荷重、問題 #16)

**NeoGraph にリンクする C++ アプリケーションを構築する場合**、かつ
独自の翻訳単位の `#include <httplib.h>` (例:
独自の `httplib::Server` SSE エンドポイントを実行します)、以下を含むすべての TU
`<httplib.h>` は `#define CPPHTTPLIB_OPENSSL_SUPPORT` **前**でなければなりません
含まれます。 TU が 1 つでもあるマクロが見つからないと、警告なしでエラーが生成されます。
`getaddrinfo` 内の SEGV 初回
`SchemaProvider::complete_stream` は LLM エンドポイントにヒットします。

### なぜこれが起こるのか

`cpp-httplib` はヘッダーのみです。クラス`httplib::ClientImpl`は
**条件付きで大きくなります** (`CPPHTTPLIB_OPENSSL_SUPPORT` が定義されている場合)
(SSL 関連のメンバーが追加され、レイアウトが最大 8 バイトシフトします)。なぜなら
ライブラリの関数はすべて `inline` ですが、リンカは 1 つを保持します。
インライン関数ごとにインスタンス化され、重複は破棄されます。 2つなら
バイナリ内の TU は、さまざまな `ClientImpl` レイアウトに対してコンパイルされます
(一方はマクロを定義し、もう一方はマクロを定義していないため)、リンカは
1 つの定義。 *他の* TU のコンパイル側は次のメンバーにアクセスします。
間違ったオフセット - 古典的な ODR 違反。汚職が蔓延する
隣接するフィールド (例: `proxy_host_` は最終的にオフセットから読み取ることになります)
それは実際には `path_` の尻尾です)、そして `httplib::ClientImpl::create_client_socket`
ワイルド `proxy_host_.c_str()` を使用して「プロキシを使用」パスに分岐します
→ `getaddrinfo` → `internal_strlen` → SEGV。

### 症状

ASan の下:

```
==NNNN==ERROR: AddressSanitizer: SEGV on unknown address
    #0 internal_strlen (...)
    #1 getaddrinfo
    #2 httplib::detail::create_socket
    #3 httplib::detail::create_client_socket
    #4 httplib::ClientImpl::create_client_socket
    #5 httplib::SSLClient::create_and_connect_socket
    ...
    #N neograph::llm::SchemaProvider::complete_stream
```

ASan なし: gdb 経由で同じスタックを使用し、ワイルド ポインター値を使用します。
テキストのように見える*可能性があります(間違ったオフセットにあったバイトは何でもです)
スロット — ASan では通常、`0xBE` 隔離ポイズンです。それなし
ASan としてデコードされる、初期化されていないスタック コンテンツである可能性があります。
JSON / UTF-8 が断片化されており、メモリ破損のように *見えます*
真犯人 — それが誤解を招く症状です）。

### 修理

`<httplib.h>` を含むすべての TU:

```cpp
// your main.cpp / sse_handler.cpp / wherever
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
```

または、CMake 内でグローバルに (推奨 - すべてのシステム全体で一貫性を保証します)
ターゲット全体):

```cmake
target_compile_definitions(your_target PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
```

独自の httplib の使用が必要な場合でも、このマクロは無害です。
`Server` (`SSLClient` ではありません) — メンバーを**追加**するだけです。何もない
実際に自分側で SSL を実行する必要があります。

### ASan を使用せずに監査する方法

```bash
grep -rn 'include.*httplib\.h\|CPPHTTPLIB_OPENSSL_SUPPORT' src/
```

`<httplib.h>` のインクルード サイトの前に**がない**場合、
`#define CPPHTTPLIB_OPENSSL_SUPPORT` (同じ TU 内、または
コンパイルフラグ定義)、それはほぼ間違いなくバグです。

### NeoGraph がこれを解決できない理由

NeoGraph 独自の .cpp ファイルはすべて一貫してマクロを定義します。の
違反は、ダウンストリーム TU も httplib.h をプルした場合にのみ発生します。
マクロなしで。コンパイル時にそれを検出するには、
(a) パブリックヘッダーで `httplib::ClientImpl` を公開する NeoGraph
(意図的にそうしません - httplib は `SchemaProvider.cpp` 内に残ります)、
または (b) 変換後の構造体サイズのリンク時 `static_assert`
単位は C++ ではサポートされていません。罠を記録するのが最善です
私たちにはできることがある。このセクションはドキュメントです。第16号は終了しました。

---

## エンジンからトポロジ JSON ドリフトを構築するツール/エディター

### 症状

以下を出力するジェネレーター、GUI、またはビジュアル ブロック エディターを作成 (または使用) しました。
NeoGraph トポロジ JSON。ノード タイプ、リデューサ、または条件を提供しました
その後、エンジンは `compile()` で `Unknown node type:` を拒否します。
/ `Unknown reducer:` / `Unknown condition:` — またはあなたが描いた枝
静かに発砲することはありません。

### なぜこれが起こるのか

ツールのパレットは手作業で管理されており、NeoGraph に後れを取っていました。
実際にリンクされているバージョン。ブランチケースは古典的なトップレベルです
`conditional_edges` 回帰 (v0.1.0 ～ v0.1.7 でサイレントに削除されました。
修正済み v0.1.8) — そのブロックを発行するツールは、ブロックが存続することを確認する必要があります。
ローダー→コンパイルの往復。

### 修理

パレットを手作業でメンテナンスしないでください。エンジンは機械可読な情報を出力します。
受け入れられる正確なスキーマ — ツールをそれに固定します。

- C++: `neograph::graph::NodeFactory::instance().export_schema()`。
- CLI: `./example_export_schema > schema.json`
  (`examples/52_export_schema.cpp`)。
- Python: `neograph_engine.export_schema()` → dict.

文書には `neograph_version` が記載されています。ツールにそれを比較してもらいます
キャッシュされたスキーマを確認し、不一致について警告します。 `node_types` は何でも反映します
通話時に`NodeFactory`に登録されるので、カスタムを登録してください
エクスポートの *前* のノード タイプ/リデューサー/条件 (実行時とまったく同じ)
`compile()` より前。 (背景: 問題 #56)

---

## 厳密なトポロジ検証

### 症状

`compile()` は `strict topology validation failed (schema_version 1)` をスローします
`$: unknown or unconsumed key 'conditionnal_edges'` のようなキーのリスト、
`nodes.X.barrier: 'wait_for' is missing or empty`、または
`translation validation failed: compiled graph does not round-trip`。

### なぜこれが起こるのか

トポロジは `"schema_version": 1` を宣言しており、厳密にオプトインされています。
コンパイル: コンパイラーが所有するすべてのオブジェクトのすべてのキーは、
パーサーによって *消費*されます。誰も使用しないキーはほとんどの場合タイプミスです
(`conditionnal_edges`、`max_retry`、`promt`) またはエンジンを構築する
そうしないと、**サイレント ドロップ** — の背後にある障害モードが削除されます。
v0.1.0 ～ v0.1.7 `conditional_edges` 回帰。往復
(翻訳検証) エラーは、コンパイルされたグラフが次のように再出力されることを意味します。
JSON は入力と一致しなくなりました: コンパイラーが失われたか、再配線されました
メッセージには正確にその内容がリストされています。

### 修理

- リストされたキーを修正します。各エラーには JSON パスが含まれます。
- コメントとエディターのメタデータは、アノテーション名前空間に属します。
  `_` または `x-` で始まるキー (例: `_comment`、`x-studio-pos`)
  常に許可され、決して検証されません。
- バリアには空ではない `wait_for` 配列が必要です。インライン条件文
  エッジは `routes` を経由するため、その上の `to` は無効になります。
  ターゲットを `routes` に移動するか、ドロップします。
- 宣言された構成スキーマを使用して登録されたカスタム ノード タイプ
  (3-arg `register_type`) はクローズドワールドでチェックされます。追加
  タイプをオプトアウトするには、スキーマに `"additionalProperties": true` を追加します。
- 過去の寛大な解析に戻すには、次のコマンドを削除します。
  `schema_version` — 未知のキーは再度無視され、ラウンドトリップされます。
  不一致がある場合は標準エラー出力でのみ警告されます。新しい文書は厳格であるべきです。

### 互換性タイムライン

- すべての `0.x` リリースでは、`schema_version` がない文書または 0 の文書を
  寛容な互換パスに維持します。`0.x` 更新で暗黙に厳格文書へ再解釈しません。
- 新しい定義、組み込みグラフファクトリー、保守対象の例は現在のバージョン
  (`TOPOLOGY_SCHEMA_VERSION`、現在は `1`) を宣言します。
- 予定されている `1.0.0` 境界では、バージョンがない入力または 0 の入力の
  ルーティングや解析の意味を暗黙に変えず、移行診断とともに拒否します。
- C++ 入力は `GraphCompiler::upgrade_to_latest()`、Python 入力は
  `ng.upgrade_topology()` で更新します。無視されていたレガシーデータは衝突を
  避ける `x-upgraded-*` 注釈として保持されます。DSL ソースは elaborator を
  再実行し、lockfile と source map を一緒に再生成してください。

---

## バグの報告

症状が上記に当てはまらない場合:

1. 最初に `pip install --upgrade neograph-engine` を実行します。多くの問題が発生します。
   パッチレベルの修正。
2. 最小の再現子をキャプチャします。
   - グラフの定義
   - 使用中のノードタイプ
   - 正確な `engine.run(...)` 呼び出し
   - `result.execution_trace` および (ストリーミングの場合) 見たイベント
3. プラットフォーム、Python バージョン、`neograph_engine.__version__` をメモします。
4. <https://github.com/fox1245/NeoGraph/issues> で問題をオープンしてください。

バグが特定の LLM エンドポイントに対してのみ発生する場合は、次のことも行ってください。
ワイヤレベルの形状を含めます (OpenAI の場合は `example_responses_envelope`)
回答;該当する場合、生の HTTP トレースの場合は `tcpdump`/`wireshark`)。
