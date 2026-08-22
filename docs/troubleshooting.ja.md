<!-- neograph-i18n: source=docs/troubleshooting.md locale=ja source_sha256=ac341ae5a04c54a36f6e1d4e165be17f5cf789b924e015626c3a01c9c3a447b9 -->
# トラブルシューティング

**Languages:** [English](troubleshooting.md) | [한국어](troubleshooting.ko.md) | [日本語](troubleshooting.ja.md) | [简体中文](troubleshooting.zh-CN.md)

最初に症状、次に根本原因と修正。ここに載っていない問題に遭遇した場合は、symptomを添えてissueを開いてください。以後のリストに追加される可能性が高いです。

> **5秒の sanity check。** 何よりも先に、以下を確認する
> あなたは最新パッチを使用しています:
> ```bash
> pip install --upgrade neograph-engine
> python -c "import neograph_engine; print(neograph_engine.__version__)"
> ```
> 以下の問題のほとんどは特定のリリースで修正されています。まずアップグレードしてください。
> その後にデバッグです。

---

## インストール / import

### `pip install neograph-engine` は成功するが、`import` は失敗する

おそらく Python バージョン / プラットフォームの不一致です。私たちは wheel を配布しています:

| プラットフォーム | Versions |
|---|---|
| Linux x86_64 (manylinux_2_34) | Python 3.9 – 3.13 |
| Linux aarch64（manylinux_2_34） | Python 3.9 – 3.13 |
| macOS arm64（14以降） | Python 3.9 – 3.13 |
| Windows x64（MSVC） | Python 3.9 – 3.13 |

このマトリックスの外のものはすべてsdist（ソースビルド）にフォールスローします。これにはCMake 3.16+、OpenSSL、およびC++20ツールチェーンが必要です。プラットフォームがリストにない場合、source build が失敗した場合は、問題を開いてください。

### Linux 上の `ImportError: ... GLIBC_2.32 not found`

Linux ホイールは `manylinux_2_34` です — glibc ≥ 2.34 (Ubuntu 22.04+、Debian 12+、RHEL 9+) が必要です。古いディストリビューションでは、ソースからビルドしてください。

### Windows 上の `ImportError: DLL load failed`

Windows ホイールは独自の依存関係を同梱していますが、Python のインストールはホイールのアーキテクチャ（x64）と一致している必要があります。以下で確認してください:

```powershell
python -c "import platform; print(platform.architecture())"
```

`('32bit', ...)` と表示される場合、32ビット版の Python を使用しています — 64ビット版をインストールしてください。

---

## TLS / ネットワーク

### プロバイダー呼び出しが60秒間ハングし、その後 `ConnPool::async_post: timeout` でエラーになります

**影響:** `neograph-engine` ホイール v0.1.0 – v0.1.6。

**根本原因:** バンドルされた OpenSSL には、`/etc/pki/tls/...` (RHEL の慣例) を指すコンパイル済みの CA ストアパスが含まれています。Ubuntu、Debian、macOS では、CA ストアは別の場所 (`/etc/ssl/certs/...`) にあるため、ホイールの libssl はピア証明書を検証できず、TLS ハンドシェイクはエラーになる前に完全なリクエストタイムアウトまで静かに待機します。

**修正 (≥ v0.1.7):** ホイールの `__init__.py` は、インポート時に `SSL_CERT_FILE` を `certifi.where()` に自動的にポイントするようになりました。アップグレードしてください:

```bash
pip install --upgrade neograph-engine
```

**旧ホイールでの回避策:**

```bash
# Debian / Ubuntu
export SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt
# Cross-distro
export SSL_CERT_FILE=$(python -c "import certifi; print(certifi.where())")
```

**v0.1.7+ で自動修正をオプトアウトするには** (例: カスタム CA バンドルがある場合): インポートする前に `NEOGRAPH_SKIP_CERT_AUTOFIX=1` を設定してください。

### `urllib` は動作しますが、NeoGraph は動作しません

上記と同じ根本原因 — `urllib` はシステムの OpenSSL を使用しますが、ホイールは誤った CA パスを持つバンドルされた OpenSSL を使用します。同じ修正: ≥ v0.1.7 にアップグレードするか、`SSL_CERT_FILE` を設定してください。

### WebSocket レスポンス (`use_websocket=True`) が `close=1000` で即座に閉じます

理由は3つあり、頻度の順に:

1. **API キー / 組織で WebSocket アクセスが有効になっていません。** 一部の OpenAI ティア 1 アカウントでは、WebSocket モードのアクセスがまだありません。`use_websocket=False` を設定して HTTP/SSE にフォールバックしてください。
2. **特定のプロキシパスで `User-Agent` ヘッダーがありません。** コミット `d7c61d0` で修正されました。≥ v0.1.4 にアップグレードしてください。
3. **`temperature` フィールドは一部の Responses-API モデルで拒否されます。** 同じコミットで、サポートされているモデルでは WS ハンドシェイクから削除されます。

### ブラウザから WASM 経由で実行する際の CORS エラー

WASM ビルドは、ブラウザ CORS 用のバイパスヘッダーをまだ実装していません。ステータスについては [WASM/CORS のイシュー](https://github.com/fox1245/NeoGraph/issues) を追跡してください。

---

## グラフのコンパイル / 実行

### `RuntimeError: Unknown reducer: <name>`

バインディングには 2 つのリデューサーが同梱されています: `"overwrite"` と `"append"`。登録していない限り、それ以外はコンパイルに失敗します。

**カスタムリデューサーの登録 (Python から、v0.1.9 以降):**

```python
ng.ReducerRegistry.register_reducer("sum",
    lambda current, incoming: (current or 0) + incoming)
```

既存の名前を再登録すると、前のリデューサーが置き換えられます。呼び出し可能オブジェクトはGILの下で実行されます。同時のSend fan-outは、Pythonカスタムノードと同じ方法でその上で直列化されます。

`"last_value"` (一般的な LangGraph のエイリアス) と入力した場合、ここでは `"overwrite"` です。同じセマンティクス、異なる名前です。

### `RuntimeError: Unknown condition: <name>`

組み込みの条件: `has_tool_calls`、`route_channel`。その他の名前は登録する必要があります。

**カスタム条件を登録する（Pythonから、v0.1.9以降）：**

```python
def is_long(state):
    msgs = state.get("messages") or []
    return "long" if len(msgs) > 10 else "short"

ng.ConditionRegistry.register_condition("is_long", is_long)
```

呼び出し可能オブジェクトは、ライブの `GraphState` (`state.get(channel)` / `state.get_messages()` が利用可能) を受け取り、条件付きエッジの `routes` キーのいずれかに一致する文字列を返す必要があります。

### `RuntimeError: Write to unknown channel: <name>`

`ChannelWrite` 内のチャネル名が `definition["channels"]` 内のどの項目とも一致しません。チャネル名は完全一致です。`messages` と `Messages` は異なります。

### `RuntimeError: Unknown node type: <name>`

ノードの 1 つの `type` フィールドが、ファクトリレジストリにないものを参照しています。組み込み (`llm_call`、`tool_dispatch`、`intent_classifier`、`subgraph`) の場合、型名は上に明記されています。独自の型の場合、コンパイルの前に `ng.NodeFactory.register_type(type_name, factory)` を呼び出す必要があります。

### 私の ReAct ループは 1 回しか実行されません — `execution_trace == ['llm']`

**影響を受ける:** `neograph-engine` ホイール v0.1.0 – v0.1.7。

**根本原因:** グラフコンパイラがトップレベルの `conditional_edges` ブロックをサイレントにドロップしました。README のクイックスタートとすべての Python の例はこの形式を使用しているため、ReAct ループは単一の LLM 呼び出し (ツールディスパッチなし) に退化しました。

**修正（≥ v0.1.8）：** コンパイラは現在、両方の形式を受け入れます — トップレベルの `conditional_edges` 配列、または`edges` 内のインラインで `condition` フィールドを持つ形式です。アップグレードして、以下で検証してください：

```python
result = engine.run(...)
print(result.execution_trace)
# Expected for ReAct: ['llm', 'dispatch', 'llm']
```

**古いホイールでの回避策：** 条件をインラインで配置します：

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

### `result.execution_trace` が空 / 開始ノードのみが表示される

グラフがすぐに `__end__` にルーティングされました。最も一般的な原因:

1. **`__start__` からのエッジが欠落しています。** すべてのグラフには、少なくとも 1 つの `{"from": ng.START_NODE, "to": "..."}` エッジが必要です。
2. **条件が `routes` マップにない値を返しました。** 条件の戻り値がどのキーにも一致しない場合、オープンまたは未指定の条件は明示的な `"default"` ルートを使用します。それが `__end__` にマップされる場合、正常に終了します。 `"default"` がない場合、ルーティングはソースノード、条件、返されたラベルを含むエラーをスローします。クローズド条件は、宣言されたセット外のラベルを常に拒否します。
3. **`max_steps=0` または `max_steps=1`** — 実行は即座に上限に達しました。デフォルトは25です。ReActループは通常10以上必要です。

### コンパイルエラー: `RuntimeError: Cycle detected: a -> b -> a`

NeoGraphは循環を許可しますが（ReActループは循環です）、コンパイラは*無条件の*循環 — 条件付き脱出のない`a → b → a` — を検出します。`__end__`へルーティングできる条件付きエッジを追加してください。

---

## パフォーマンス

### Fan-outが予想より遅い

一般的な原因は2つあります：

1. **エンジン所有のワーカープールはありません。** `compile()`はデフォルトで`set_worker_count(1)`になります — プールなし、fan-outブランチは呼び出し元のエグゼキュータ上でインラインでディスパッチされ、直列に実行されます。`compile()`の後に一度だけプールをオプトインしてください（`run()`の前）:

   ```python
   engine.set_worker_count(N)        # exact fan-out width
   engine.set_worker_count_auto()    # hardware_concurrency()
   ```

NeoGraphはまた、プールなしでマルチSend（またはマルチ出力エッジ）fan-outが初めて実行されたときに、一度だけstderr警告を出力するため、サイレントシリアルケースが可視化されます。worker=1の高速パスが意図的な場合は、`NEOGRAPH_SUPPRESS_FANOUT_WARNING=1`で抑制してください。
2. **Pythonカスタムノードは、その本体の間GILを保持します。** あなたの`@ng.node`関数がCPUバウンドのPython処理を行う場合、fan-outは高速化されません。ONNX / PyTorch / numpy / `requests.get`はネイティブ呼び出し中にGILを解放するため、これらは並列化されます。純粋なPythonスコアリングループの場合、ワーカー数をいくつ設定しても関係ありません。

### `bench_neograph par`は200マイクロ秒超を報告します

**v1.0以前のwheel。** v0.1.4–v0.xはワーカープールのデフォルトを`hardware_concurrency()`に設定しており、fan-outティックごとにスレッド間送信コストを支払っていました。v1.0ではデフォルトを`set_worker_count(1)`（プールなし、送信コストなし）に戻しました — `par`は新しい`compile()`でフリップ前の大まかな範囲に戻っています。ワークロードのfan-outブランチが実際のスレッドプールから本当に恩恵を受ける場合（CPU-boundの本体、大規模なfan-out幅）は、`engine.set_worker_count(N)` / `engine.set_worker_count_auto()`でプールをオプトインしてください。

### ストリーミングコールバックがノードごとに2回発生します

**影響:** Python `@ng.node`書き込み専用ノード。`re-agent`コミット`2a5c5dc` / `5993935`で修正され、NeoGraph masterに複製されています。

**v1以前のリリースの根本原因:** 純粋書き込みの`GraphNode`サブクラス（`Command`なし、`Send`なし）は、結果用とストリームフック用に一度ずつ実行される可能性がありました。アップグレードして単一の`run(NodeInput)`オーバーライドを実装してください。v1はそのメソッドを一度だけ呼び出し、オプションのストリームシンクを`in.stream_cb`として公開します。

`@ng.node`デコレータを使用している場合（サブクラス化ではない）、これはすでに処理されています。

---

## チェックポイント / Postgres

### `PostgresCheckpointStore`が見つからない / インポートエラー

PyPIホイールは`PostgresCheckpointStore`を有効にして出荷されています（libpqはv0.1.3以降バンドルされています）。`import neograph_engine; neograph_engine.PostgresCheckpointStore`は直接動作するはずです。

ソースから`-DNEOGRAPH_BUILD_POSTGRES=ON`なしでビルドした場合、クラスはバインディングに存在しません。フラグを設定してCMake設定を再実行し、再ビルドしてください。

### Postgres接続: `FATAL: password authentication failed`

`PostgresCheckpointStore` 接続文字列はlibpqに従います:

```
postgresql://user:password@host:port/dbname
```

パスワードにURL特殊文字（`@`、`:`、`/`、`%`）が含まれる場合は、URLエンコードしてください。または`key=value`形式を使用してください：

```
host=localhost user=neo password=p@ss dbname=neograph
```

### 非同期 Postgres 再接続は 30 秒でタイムアウトします

非同期の初期接続/置換接続では、試行全体に対して1つの本番安全デッドラインが使用されます。接続文字列に直接記述された正の`connect_timeout=N`は、その全体予算を秒単位で設定し、`connect_timeout=1`はPostgreSQLの最小値である2秒に切り上げられます。明示的な値がない場合、ゼロの場合、または負の場合は、NeoGraphは30秒を使用します。`PGCONNECT_TIMEOUT`およびサービスファイルのタイムアウト値は解決が遅すぎて初期の非同期接続ステップを制限できないため、これらも30秒のデフォルトを使用します。非同期の期限が異なる必要がある場合は、値を直接接続文字列に記述してください。

この予算は、マルチホスト接続文字列内のすべてのホストと解決されたIPを対象とし、ホストごとに乗算されることはありません。これは、同期libpqでの`connect_timeout`が各ホストに個別に適用される点と意図的に異なります。同期の`PostgresCheckpointStore`構築および置換は変更されません。

たとえば、この例では、非同期の置換試行全体に 60 秒を与えます:

```
host=pg-a,pg-b dbname=neograph connect_timeout=60
```

### Postgres `relation "neograph_checkpoints" does not exist`

ストアは初回使用時にテーブルを作成します（`CREATE TABLE IF NOT EXISTS`）。DBユーザーにCREATE権限がない場合は、スキーマを手動で実行してください。SQLは[`include/neograph/graph/postgres_checkpoint.h`](../include/neograph/graph/postgres_checkpoint.h)の`kSchema`の下にあります。

---

## 例 / docker

### `docker compose run agent` 例えば、26はPGを見つけられず失敗します

compose ファイルは、 `db` サービスが `postgres://neograph:neograph@db:5432/neograph`として到達可能であることを期待しています。docker-compose の外部にいる場合は、 `PG_URL` を到達可能なホストに設定してください。完全な環境変数テーブルについては、[`examples/26_postgres_react_hitl/README.md`](../examples/26_postgres_react_hitl/README.md) を参照してください。

### Crawl4AI の例が起動を拒否します

Crawl4AI はオプションの Docker コンテナです:

```bash
docker run -d -p 11235:11235 --shm-size=1g --name crawl4ai \
    unclecode/crawl4ai:latest
```

例17、25、26は、`CRAWL4AI_URL`（デフォルト`http://localhost:11235`）が到達不可能な場合に正常にフォールバックします。

### `example_clay_chatbot`ビルドターゲットが見つかりません

例11では、CMake設定時に`-DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON`が必要です:

```bash
cmake -B build -DNEOGRAPH_BUILD_CLAY_EXAMPLE=ON ..
make example_clay_chatbot
```

これはClay（UIレイアウト）+ Raylib（レンダラー）を取り込むため、フラグの背後にあります。

---

## ストリーミングイベント

### `event.node`は`AttributeError`を発生させます

属性は`event.node_name`です（C++フィールド名と一致します）。同様に`event.type`（列挙型）と`event.data`（JSONディクショナリ）にも適用されます。

```python
def cb(event):
    print(f"{event.type.name} on {event.node_name}: {event.data}")
```

### 私の`StreamMode.TOKENS`コールバックが決して発火しません

プロバイダーはストリーミングをサポートする必要があります。現在：

| プロバイダー | ストリーミング？ |
|---|---|
| `OpenAIProvider` | ✓ HTTP/SSE |
| `SchemaProvider("openai_responses")` | ✓ SSE |
| `SchemaProvider("openai_responses", use_websocket=True)` | ✓ WS |
| `SchemaProvider("claude")` | ✓ SSE |
| カスタムPython `Provider`サブクラス | あなたの`complete_stream`実装に依存します |

カスタムPython `Provider`では、 `complete_stream`をオーバーライドします。Pythonサブクラスは非同期仮想オーバーライドを公開しません。新しいC++バックエンドでは、 `CompletionProvider` から派生し、 `request.streaming()` を `do_invoke()`内で処理します。既存のC++ `Provider` サブクラスは、 `complete_stream()` または `complete_stream_async()`を引き続きオーバーライドできます。ストリーミング実装がない場合、デフォルトでは収集されたレスポンスを増分トークンではなく1つのチャンクとして出力します。

---

## OpenTelemetry

### 私のOTelスパンが`parent_id=None`とともに表示されます（1つではなく4つの個別のトレース）

**影響:** `neograph_engine.tracing`、コミット`9073671`より前。

**根本原因:** `tracer.start_span` + `use_span(...).__enter__()`はcontextvarsに依存していますが、contextvarsはC++→Pythonのpybindコールバック境界を越えて伝播しません。

**修正:** `otel_tracer`ヘルパーが`set_span_in_context(root_span)`を介して親コンテキストのスナップショットを取り、それを各子ノードの`start_span`に明示的に渡すようになりました。`9073671`以降へアップグレードしてください。

独自のOTel統合を実装している場合は、同じことを行ってください。バインディング境界を越えてcontextvarsに依存しないでください。

### 私のLLMスパンが、ノードスパンとは異なるトレースIDで表示されます

**影響:** `neograph_engine.openinference` v0.6.0最終版（コミット`fa8ed50`）より前。

**根本原因:** `openinference_tracer` が `parent_ctx` (スナップショット) を設定したが、ノードスパンをOTelの現在のコンテキストとして*アタッチ*しなかった。そのため、ノード本体が `provider.complete()` と `OpenInferenceProvider` を呼び出したとき、 `llm.complete` が `tracer.start_as_current_span(...)`を介してスパンを開いたが、新しいスパンはグローバルルートにフォールバックし、トレースはLLM呼び出しごとに別々のトレースIDに断片化された。

**修正:** `openinference_tracer` は `otel_context.attach(set_span_in_context(span))` を `NODE_START` に対して実行し、結果のトークンをスパンとともに退避します。`NODE_END` / `ERROR` / `INTERRUPT` はスパンを終了する前にトークンを切り離し、以前の現在のスパンを復元します。v0.6.0でPhoenixに対して検証済み — `graph.run > node.X > llm.complete` 階層を持つ単一のトレースツリー。

v0.6.0+ を使用していて、*それでも*分割トレースが発生する場合は、プロバイダーがラップされていません。`ctx.provider = OpenInferenceProvider(inner, tracer)`が**前に**`engine.compile(...)`で実行されるようにしてください。それ以外の場合、エンジンはラップされていないプロバイダーにバインドされます。

### `pip install opentelemetry-api`は、`openinference`をインポートするとImportErrorを発生させます。

`neograph_engine.openinference`は`opentelemetry`を遅延インポートします。ImportErrorは最初の使用時のみ発生し、1行のインストールヒントが表示されます::

    pip install opentelemetry-api opentelemetry-sdk

OTLP経由でスパンをPhoenix / Langfuse / Tempoにプッシュしたい場合は、`opentelemetry-exporter-otlp`を追加してください。

### 私のカスタム`Tracer`アダプタが、`session.close()`の後にハングする／クラッシュする／ガベージを出力する（issue #24）

あなたは、スパンをメモリ内リストに記録する`neograph::observability::Tracer`アダプタ（C++）を書き、その後`OpenInferenceTracerSession::close()`を呼び出した**後に**リストを走査しました。その走査は解放済みメモリを読み取ります。

`close()`は、ルートスパン（およびノードごとのスパンスタック）上の内部`unique_ptr<Span>`をリセットします。アダプタが`start_span`から返したラッパーオブジェクトへの**生ポインタ**を配布していた場合、`close()`が戻った瞬間にそれらのポインタはダングリングになります。ラッパーは呼び出し元が所有しており、呼び出し元はそれらを解放したばかりだからです。

**修正:** アダプタは、呼び出し元が所有するラッパーへの生ポインタを追跡するだけでなく、記録したスパンデータ自体を所有しなければなりません。その形状は以下の通りです:

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

参照: `tests/test_openinference_cpp.cpp::InMemoryTracer` (正規のテストフィクスチャ) と `examples/49_openinference.cpp::PrintTracer` (stderr 出力デモ) はどちらもこの正確なパターンを使用しています。同じ警告が `@warning` ブロック内の `Tracer` と `OpenInferenceTracerSession::close()` のヘッダーにあります。

**バグの現れ方:** 観測可能な障害モードには、検査ループ内での完全なクラッシュ(最良性のケース)、スパン名の印刷中にハングする(解放されたバッファに文字列フォーマッタをループさせるものが含まれていた)、または単に属性値が誤っている、が含まれます。これら三つはすべて同じ根本原因です。

---

## ビルドエラー

### GCC 13内部コンパイラエラー: `build_special_member_call`、`cp/call.cc:11096`（issue #23）

Ubuntu 24.04の標準GCC 13(または任意のGCC 13.x)を使用している場合、ビルドは以下のように失敗します:

```
internal compiler error: in build_special_member_call, at cp/call.cc:11096
```

…コルーチン内で`co_await x.foo_async(...)`を実行する行（通常は`asio::co_spawn`へ`main()`から渡すラムダ本体）で発生します。これはコードの問題ではなくGCC 13のフロントエンドバグです。GCC 14以降、Clang 18以降、MSVC 19.40以降では同じソースを変更せずにコンパイルできます。

**3つの回避策**(推奨順):

1. **コンパイラをアップグレードしてください** — Ubuntu 24.04で`sudo apt install gcc-14 g++-14`（24.10ではデフォルトでGCC 14が付属）、その後`cmake -DCMAKE_CXX_COMPILER=g++-14 ...`。最もクリーンな修正です。自然な方法でコードを書くことができます。

2. **コルーチンを `neograph::async::run_sync`** の代わりに `asio::co_spawn` から `main()`を介して駆動します。同じ観測可能な動作で、フロントエンドのICEはありません：

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

`run_sync`は独自のプライベート`io_context`を構築し、awaitableを完了まで駆動します — 内部的には`co_spawn + io.run()`が行うことと同一ですが、呼び出しサイトはコンパイラの観点から同期であるため、ICEは決して発生しません。

3. **コルーチンを再構成**して、`co_await`がフリー関数やラムダ本体ではなく、通常のクラスのメンバー関数内で発生するようにします。これは一部のケースでは機能しますが、診断が常に正しい形状変更を指すとは限りません。オプション1または2の方が信頼性が高いです。

**このリポジトリで問題が顕在化する箇所:** CMakeListsには、例ごとのツールチェーンゲートが `example_03` (元のICE発生箇所) の周囲にあり、 `examples/50_async_tool.cpp` は、 `run_sync` の代わりに `co_spawn` を `main()`から使用することで問題を回避しています。自然な `co_spawn`-メインから の形に従う新しいコルーチンの例やテストは、同じツールチェーンで同じICEに遭遇します — オプション1または2を適用してください。

## Python型ID (v0.5.0+)

### `isinstance(params.messages, list)`はFalseを返します

**影響を受けるバージョン:** v0.5.0以降、5つのベクタープロパティサーフェス: `CompletionParams.messages`、`.tools`、`ChatMessage.tool_calls`、`NodeResult.writes`、`.sends`。

**理由:** v0.5.0 は、 `params.messages.append(...)` のサイレントな no-op を修正しました。これは、これらのベクトルを不透明型 (`PYBIND11_MAKE_OPAQUE` + `py::bind_vector`) としてバインドすることで、 `.append` がライブの C++ ベクトルを変更するようにしたためです。トレードオフとして、プロパティの型は現在、例えば `ChatMessageList` (pybind クラス) となり、プレーンな Python ではなくなりました。 `list`.

**まだ動くもの:**
- `params.messages = [m1, m2]` — `py::implicitly_convertible<py::list, …>`は代入時にPythonリストを自動変換します。
- `for m in params.messages` — 反復プロトコル。
- `len(params.messages)`、`params.messages[i]`、`params.messages[i] = m`、スライシング。
- `params.messages.append(...)`、`.extend(...)`、`.insert(...)`、`.pop(...)`、`.clear()` — すべてライブのC++ベクターに直接プッシュされます。

**壊れたもの(まれ):**
- `isinstance(x, list) → False`。プレーンなPythonリストが本当に必要な場合は、マテリアライズします: `list(params.messages)`。
- `json.dumps(params.messages)` — バインドされたクラスは直接JSONシリアライズできません。変換します: `json.dumps([{"role": m.role,
  "content": m.content} for m in params.messages])`。

`ChatMessage.image_urls`(`std::vector<std::string>`)は移行されていません — `vector<string>`は、コールサイトのスイープなしでグローバルなOPAQUEにするにはバインディングで広く使用されすぎています。`.append()`のno-opは、文書化された制限としてそこに残っています。`add_image_url()`の便利メソッドを介したv0.6+の候補です。

---

## ビルドはソースから行う

### CMake 設定: Windows 上の `Could NOT find SQLite3`

現在のWindowsホイールはSQLiteを有効化し、対応するランタイムDLLを同梱しています。独自にソースからビルドする場合は、同じvcpkg/MSVCツールチェーンでSQLiteをインストールしてください。SQLiteが意図的に不要なら`-DNEOGRAPH_BUILD_SQLITE=OFF`を渡せますが、互換性のないMSVCランタイム向けDLLを混在させないでください。

### CMake 設定：Linux 上の `Could NOT find CURL`

オプション依存です。パッケージマネージャーでインストールしてください：

```bash
# Debian / Ubuntu
sudo apt install libcurl4-openssl-dev
# RHEL / Fedora
sudo dnf install libcurl-devel
# macOS
brew install curl
```

または無効化: `-DNEOGRAPH_USE_LIBCURL=OFF`。libcurl がない場合、`SchemaProvider` の `prefer_libcurl=True` モード (HTTP/2) は利用できません — デフォルトの ConnPool (HTTP/1.1) は引き続き動作します。

### Pybindバインディングが未定義参照でリンクに失敗する

おそらく、新しいコードを取得した後に CMake を再実行せずに `make` を再実行しているのでしょう。ビルドディレクトリのコンパイル済みオブジェクトファイルが、古いヘッダーのシンボルを参照しています。`make clean && make` を行うか、ビルドディレクトリを削除して再設定してください。

### スクリプトから A2A サーバーを起動するときの `OPENAI_API_KEY not set`

`cppdotenv::auto_load_dotenv()` は、それを呼び出すバイナリ内の `.env` を読み取りますが、ランチャースクリプトからフォークされた子プロセスは、ランチャーがすでにエクスポートしていないものは**一切**継承しません。スクリプトが次のことを行う場合:

```bash
./member_server 8101 ...   # forks before any env is set up
```

…各子プロセスは空の環境を認識し、起動を拒否します。ランチャー内で最初に `.env` を source して、変数がフォークを行うシェルにエクスポートされるようにしてください:

```bash
set -a; . ./.env; set +a            # marks every assignment as exported
./member_server 8101 ... &
```

クックブックの `scripts/run_session.sh` は、兄弟の `.env` へのフォールバックを含む完全なパターンを示しています。

### マルチペルソナ／マルチプロセスA2A：OpenAIプロバイダーをどこで共有するか

`OpenAIProvider::create_shared(cfg)` (`shared_ptr<Provider>` を返す) を、`create(cfg)` (`unique_ptr` を返す) の代わりに使用してください。共有形式は `NodeFactory` ラムダにキャプチャ可能で、すべてのグラフノードと A2A リクエストにわたって再利用できます — `create()` の `unique_ptr` は、手動で `release()` して再ラップすることを強制します。

---

## C++ コンシューマー — `httplib.h` マクロの一貫性 (重要な点、issue #16)

C++アプリケーションを構築し、**NeoGraphにリンク**し、かつ `#include <httplib.h>` 自身の翻訳単位内でも `httplib::Server` SSEエンドポイントを実行する場合、`<httplib.h>` を含むすべてのTUは、インクルードの**前に** `#define CPPHTTPLIB_OPENSSL_SUPPORT` しなければなりません。1つのTUでもマクロが欠落していると、内部でSEGVが静かに発生し、 `getaddrinfo` 初めて `SchemaProvider::complete_stream` がLLMエンドポイントに到達したときに発生します。

### これが発生する理由

`cpp-httplib`はヘッダオンリーである。クラス`httplib::ClientImpl`は、`CPPHTTPLIB_OPENSSL_SUPPORT`が定義されている場合、**条件的に大きくなる**（SSL関連メンバーが追加され、レイアウトが約8バイトずれる）。ライブラリの関数はすべて`inline`であるため、リンカはインライン関数ごとに1つのインスタンスを保持し、重複を破棄する。バイナリ内の2つの翻訳単位が異なる`ClientImpl`レイアウトに対してコンパイルされる場合（一方がマクロを定義し、他方が定義しなかった場合）、リンカは1つの定義を選択する。もう一方のTUのコンパイル側は、誤ったオフセットでメンバーにアクセスする——典型的なODR違反である。破損は隣接フィールドに及ぶ（例：`proxy_host_`は実際には`path_`の末尾であるオフセットから読み取ることになり）、`httplib::ClientImpl::create_client_socket`はワイルドな`proxy_host_.c_str()`→`getaddrinfo`→`internal_strlen`→SEGVで「プロキシを使用」パスに分岐する。

### 症状

ASanの下で：

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

ASanなしの場合：gdb経由で同じスタック、ワイルドポインタ値はテキストのように見えることが*ある*（誤ったオフセットのスロットにあった任意のバイトである——ASan下では通常`0xBE`の隔離ポイズンである；ASanなしでは、JSON/UTF-8フラグメントとしてデコードされることがある未初期化のスタック内容であり、実際の原因によるメモリ破損のように*見える*——それが誤解を招く症状である）。

### 修正

`<httplib.h>` を含むすべての翻訳単位において:

```cpp
// your main.cpp / sse_handler.cpp / wherever
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
```

またはCMake内でグローバルに(推奨)—ターゲット全体で一貫性が保証される):

```cmake
target_compile_definitions(your_target PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
```

このマクロは、自身の httplib 利用が `Server` のみを必要とする場合（`SSLClient` ではない場合）でも無害です — メンバーを**追加**するだけであり、自身の側で実際に SSL を行うことを要求するものはありません。

### ASanなしでの監査方法

```bash
grep -rn 'include.*httplib\.h\|CPPHTTPLIB_OPENSSL_SUPPORT' src/
```

`<httplib.h>` の任意のインクルード箇所の**前**に `#define CPPHTTPLIB_OPENSSL_SUPPORT` が（同じ翻訳ユニット内、またはコンパイルフラグ定義経由で）存在しない場合、それがほぼ確実にバグです。

### NeoGraphがこれを修正できない理由

NeoGraph自身の.cppファイルはすべてマクロを一貫して定義している。違反は、下流のTUがマクロ*なしで*httplib.hも取り込む場合にのみ発生する。コンパイル時にそれを検出するには、(a) NeoGraphが`httplib::ClientImpl`を公開ヘッダーで公開する（httplibが`SchemaProvider.cpp`内に留まるため、意図的に公開しない）、または(b)翻訳単位間での構造体サイズのリンク時`static_assert`が必要であり、C++はそれをサポートしていない。この罠を文書化することが最善であり、このセクションがその文書である。Issue #16はクローズ済み。

---

## トポロジーJSONを構築するツール／エディターがエンジンと乖離する

### 症状

NeoGraphトポロジーJSONを出力するジェネレータ、GUI、またはビジュアルブロックエディタを作成した（または使用している）。エンジンが`compile()`で`Unknown node type:`/`Unknown reducer:`/`Unknown condition:`で拒否するノードタイプ、リデューサー、または条件を提供した——または描いたブランチが静かに決して発火しない。

### これが発生する理由

ツールのパレットは手動で維持され、実際にリンクされたNeoGraphバージョンに遅れを取った。ブランチのケースは、古典的なトップレベルの`conditional_edges`回帰である（v0.1.0–v0.1.7で静かに削除され、v0.1.8で修正）——そのブロックを出力するツールは、ローダー→コンパイルのラウンドトリップを生き残ることを検証する必要がある。

### 修正

パレットを手動で保守しないでください。エンジンは受け入れる内容を正確に示す機械可読スキーマを出力します。ツールをそれに固定してください：

- C++：`neograph::graph::NodeFactory::instance().export_schema()`。
- CLI：`./example_export_schema > schema.json`（`examples/52_export_schema.cpp`）。
- Python：`neograph_engine.export_schema()`→dict。

ドキュメントは`neograph_version`を保持している；ツールにそれをキャッシュされたスキーマと比較させ、不一致を警告させる。`node_types`は呼び出し時に`NodeFactory`に登録されているものを反映するため、`compile()`の前とまったく同じように、エクスポート*前に*カスタムノード型/リデューサー/条件を登録する。（背景：issue #56。）

---

## 厳密なトポロジー検証

### 症状

`compile()` は `strict topology validation failed (schema_version 1)` をスローし、`$: unknown or unconsumed key 'conditionnal_edges'`、`nodes.X.barrier: 'wait_for' is missing or empty`、または `translation validation failed: compiled graph does not round-trip` のようなキーを列挙します。

### これが発生する理由

あなたのトポロジーは`"schema_version": 1`を宣言しており、これにより厳密なコンパイルが選択されます。コンパイラが所有するすべてのオブジェクトのすべてのキーは、パーサーによって*消費*されなければなりません。誰も消費しなかったキーは、ほぼ常にタイプミス（`conditionnal_edges`、`max_retry`、`promt`）か、エンジンがそれ以外では**黙って破棄**する構造です。これはv0.1.0–v0.1.7の`conditional_edges`リグレッションの背後にある障害モードです。ラウンドトリップ（意味検証）エラーは、コンパイルされたグラフがJSONとして再出力されたときに、もはや入力と一致しないことを意味します。コンパイラが何かを失ったか、配線を変更したかであり、メッセージは正確に何かをリストします。

### 修正

- リスト化されたキーを修正します。各エラーには JSON パスが付いています。
- コメントとエディタのメタデータはアノテーション名前空間に属します。`_`または`x-`で始まるキー（例：`_comment`、`x-studio-pos`）は常に許可され、検証されることはありません。
- バリアには空でない`wait_for`配列が必要です。インライン条件付きエッジは`routes`を経由するため、その上の`to`はデッドです。ターゲットを`routes`に移動するか、削除してください。
- 宣言された設定スキーマ（3引数の`register_type`）を持つカスタムノードタイプは、クローズドワールドでチェックされます。スキーマに`"additionalProperties": true`を追加して、タイプをオプトアウトしてください。
- 歴史的な寛容なパースにフォールバックするには、`schema_version`を削除してください。未知のキーは再び無視され、ラウンドトリップの不一致はstderrに警告するだけです。新しいドキュメントは厳密なままにしてください。

### 互換性のタイムライン

- すべての`0.x`リリースは、寛容な互換性パス上で、欠落またはゼロの`schema_version`ドキュメントを保持します。`0.x`の更新は、それらを厳密なドキュメントとして黙って再解釈することはありません。
- 新しい定義、組み込みグラフファクトリ、および保守された例は、現在のバージョン（`TOPOLOGY_SCHEMA_VERSION`、現在は`1`）を宣言します。
- 計画された`1.0.0`境界は、欠落またはゼロのバージョンを、ルーティングやパースセマンティクスを黙って変更する代わりに、移行診断で拒否します。
- C++入力は`GraphCompiler::upgrade_to_latest()`で、Python入力は`ng.upgrade_topology()`でアップグレードしてください。無視されたレガシーデータは、衝突安全な`x-upgraded-*`アノテーションの下に保持されます。厳密なCore JSONは保持される交換アーティファクトです。JavaScriptソースは、QuickJS `define()`を通じて再コンパイルする必要があります。

---

## バグの報告

上記に該当しない場合:

1. 最初に`pip install --upgrade neograph-engine`を実行してください。多くの問題はパッチレベルの修正です。
2. 最小の再現ケースを取得してください：
   - グラフ定義
   - 使用中のノードタイプ
   - 正確な`engine.run(...)`呼び出し
   - その`result.execution_trace`と（ストリーミングの場合）あなたが見たイベント
3. プラットフォーム、Pythonバージョン、および`neograph_engine.__version__`に留意してください。
4. <https://github.com/fox1245/NeoGraph/issues>でissueを開いてください。

バグが特定のLLMエンドポイントに対してのみ発生する場合は、ワイヤーレベルの形式（OpenAI Responsesの場合は`example_responses_envelope`、該当する場合は生のHTTPトレースの`tcpdump`/`wireshark`）も含めてください。
