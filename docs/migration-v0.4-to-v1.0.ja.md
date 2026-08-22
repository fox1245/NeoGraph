<!-- neograph-i18n: source=docs/migration-v0.4-to-v1.0.md locale=ja source_sha256=d2b0e6c51e07b5877e07c5768ade91ce2129fec5f6441f92c7f7b8feae6026b9 -->
# 移行ガイド: レガシー 8 仮想メソッド → `run(NodeInput)` (v0.4.x → v0.9+)

**Languages:** [English](migration-v0.4-to-v1.0.md) | [한국어](migration-v0.4-to-v1.0.ko.md) | [日本語](migration-v0.4-to-v1.0.ja.md) | [简体中文](migration-v0.4-to-v1.0.zh-CN.md)

NeoGraph v0.4 はノードエントリポイントを単一の `run(NodeInput) ->
awaitable<NodeOutput>` に集約しました。レガシーな 8 つの仮想メソッド (`execute` /
`execute_async` / `execute_stream` / `execute_stream_async` とそれらの `_full`
対応版) は v0.4.x で非推奨となり、v1 準備リリースである v0.9.0 で削除されました。
本書はレガシーノードを現在の API に移行する手順を概説します。

> v0.9.0 以降、`run(NodeInput)` を実装していない C++ サブクラスは抽象クラスとして
> コンパイルエラーになります。Python サブクラスも `run(self, input)` を実装する
> 必要があります。

## 移行する理由

旧パターン — `(sync/async) × (writes/full) × (stream/non-stream)` = 8 仮想
直積。いずれか 1 つをオーバーライドすると、他の 7 つがデフォルトチェーンに
フォールバックします。一部の組み合わせは安全ですが、実行時の落とし穴があります
(例: 同期 `execute_full` + 非同期ディスパッチ → ネストされた `run_sync` 競合)。
これにより、ユーザーがどの関数をオーバーライドすべきか不明瞭でした。

新パターン — 単一の `run(NodeInput) -> awaitable<NodeOutput>`。
1 つだけオーバーライド。同期 vs 非同期の区別は呼出元の関心事です
(ユーザーはコルーチン内で `co_await` を使用するか、プレーンな同期コードを
自由に使用可能)。Command / Send は `NodeOutput` に含まれるため、追加の
仮想メソッドは不要。ストリーミングコールバックは
`NodeInput::stream_cb` (nullable ポインタ) 経由で到着。

## 8 仮想メソッド → 新 `run()` マッピング

| レガシー仮想メソッド | 移行後の形式 |
|---|---|
| `execute(state)` | `NodeOutput out; out.writes = {...}; co_return out;` (同期本体) |
| `execute_async(state)` | ネイティブ非同期: `co_return co_await provider->complete_async(...);` |
| `execute_stream(state, cb)` | `if (in.stream_cb) (*in.stream_cb)(event); co_return NodeOutput{...};` |
| `execute_stream_async(state, cb)` | 上記 + ネイティブ非同期 (`co_await ...`) |
| `execute_full(state)` | `NodeOutput out; out.writes=...; out.command=...; co_return out;` |
| `execute_full_async(state)` | 上記 + ネイティブ非同期 |
| `execute_full_stream(state, cb)` | `execute_full` + `in.stream_cb` 使用 |
| `execute_full_stream_async(state, cb)` | 上記 + ネイティブ非同期 |

キーポイント: **8 つのバリアントは、どの `NodeOutput` フィールドが設定されるか +
`in.stream_cb` が使用されるか + `co_await` が使用されるかの組み合わせとして
表現可能**。残る仮想メソッドは 1 つだけ。

### 最も一般的な Python 移行

**旧コード:**

```python
class CounterNode(ng.GraphNode):
    def execute(self, state):
        current = state.get("count") or 0
        return [ng.ChannelWrite("count", current + 1)]
```

**現在のコード:**

```python
class CounterNode(ng.GraphNode):
    def run(self, input):
        current = input.state.get("count") or 0
        return [ng.ChannelWrite("count", current + 1)]
```

Python の `run` は通常の `def` であり、`async def` ではありません。
ストリーミング実行では `input.stream_cb` がイベントを受け取る関数になり、
通常実行では `None` になります。

## ケースバイケース変換例

### ケース 1 — 最も単純な同期ノード

**旧:**
```cpp
class MyNode : public GraphNode {
public:
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        int n = state.get("counter").get<int>();
        return {ChannelWrite{"counter", json(n + 1)}};
    }
    std::string get_name() const override { return "my_node"; }
};
```

**新:**
```cpp
class MyNode : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        int n = in.state.get("counter").get<int>();
        NodeOutput out;
        out.writes.push_back({"counter", json(n + 1)});
        co_return out;
    }
    std::string get_name() const override { return "my_node"; }
};
```

違い:
- `state` → `in.state`
- 戻り値が `NodeOutput` にラップされる (`writes` フィールド)
- 関数が `asio::awaitable<NodeOutput>` で `co_return` で終了

### ケース 2 — 非同期 LLM ノード (`execute_async` の移行)

**旧:**
```cpp
class TalkNode : public GraphNode {
    std::shared_ptr<Provider> prov_;
public:
    asio::awaitable<std::vector<ChannelWrite>>
    execute_async(const GraphState& state) override {
        auto reply = co_await prov_->complete_async({
            .messages = state.get_messages(),
            .model    = "gpt-mock",
        });
        co_return std::vector<ChannelWrite>{
            {"reply", json(reply.message.content)}
        };
    }
    std::string get_name() const override { return "talk"; }
};
```

**新:**
```cpp
class TalkNode : public GraphNode {
    std::shared_ptr<Provider> prov_;
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto reply = co_await prov_->complete_async({
            .messages = in.state.get_messages(),
            .model    = "gpt-mock",
        });
        NodeOutput out;
        out.writes.push_back({"reply", json(reply.message.content)});
        co_return out;
    }
    std::string get_name() const override { return "talk"; }
};
```

### ケース 3 — ストリーミングノード (`execute_stream` の移行)

**旧:**
```cpp
std::vector<ChannelWrite>
execute_stream(const GraphState& state, const GraphStreamCallback& cb) override {
    auto reply = prov_->complete_stream(params, [&](const std::string& chunk) {
        cb({GraphEvent::Type::LLM_TOKEN, "talk", json(chunk)});
    });
    return {ChannelWrite{"reply", json(reply.message.content)}};
}
```

**新:**
```cpp
asio::awaitable<NodeOutput> run(NodeInput in) override {
    // in.stream_cb is a pointer — null means the caller does not want streaming.
    auto on_chunk = [&](const std::string& chunk) {
        if (in.stream_cb) {
            (*in.stream_cb)({GraphEvent::Type::LLM_TOKEN, "talk", json(chunk)});
        }
    };
    auto reply = prov_->complete_stream(params, on_chunk);
    NodeOutput out;
    out.writes.push_back({"reply", json(reply.message.content)});
    co_return out;
}
```

### ケース 4 — Command / Send を使用するノード (`execute_full` の移行)

**旧:**
```cpp
NodeResult execute_full(const GraphState& state) override {
    NodeResult r;
    r.writes.push_back({"step", json("dispatched")});
    Command command;
    command.goto_node = "next_router";
    r.command = command;   // Force routing
    return r;
}
```

**新:**
```cpp
asio::awaitable<NodeOutput> run(NodeInput in) override {
    NodeOutput out;   // NodeOutput == NodeResult — alias of the same type
    out.writes.push_back({"step", json("dispatched")});
    Command command;
    command.goto_node = "next_router";
    out.command = command;
    co_return out;
}
```

`NodeOutput` は `NodeResult` のエイリアスです — レガシーな `NodeResult` コードも
そのままコンパイルされます。

## よくある間違い

### `NodeInput in` は値渡し

```cpp
// ❌ Wrong — coroutine ref-param UAF, SEGV in pybind async path
asio::awaitable<NodeOutput> run(const NodeInput& in) override { ... }

// ✅ Correct
asio::awaitable<NodeOutput> run(NodeInput in) override { ... }
```

理由: コルーチンフレームは安全性のため引数のコピーを取らなければならない。
参照で受け取ると、呼出元のスタックフレームが消えた後に `in.state` が
ダングリングになる。PR 2 作業中に実際に発生したバグ。

### cancel / store / stream_cb はすべて `in.ctx` から取得

レガシーノードは `state.run_cancel_token_` のような密輸チャネル経由で
キャンセルトークンを受け取っていましたが、v0.4 で `RunContext` が正式な配管として
導入されました:

```cpp
asio::awaitable<NodeOutput> run(NodeInput in) override {
    // Check cancellation signal
    if (in.ctx.cancel_token && in.ctx.cancel_token->is_cancelled()) {
        throw CancelledException("user cancelled");
    }

    // Store access (issue #27)
    if (in.ctx.store) {
        auto user_pref = in.ctx.store->get({"users", in.ctx.thread_id}, "lang");
        // ...
    }

    // Streaming sink (nullable)
    if (in.stream_cb) {
        (*in.stream_cb)({GraphEvent::Type::NODE_END, "my_node", json(...)});
    }

    co_return NodeOutput{};
}
```

ノードが利用可能な `in.ctx` のフィールド: `cancel_token`、`usage`、
`thread_id`、`step`、`stream_mode`、`store`、`resume_value`、`deadline`、
`trace_id`。最後の 2 つは `RunMetadata` で設定し、エンジンはネストした
subgraph まで保持します。チェックポイントの経路はエンジン内部であり、公開
`RunContext` フィールドではありません。Python は `trace_id`、`run_id`、
`model_token_budget`、`has_deadline`、`deadline_remaining_ms` を公開します。
生のC++ steady-clockデッドラインは意図的に非公開のままです。

### `_full` 仮想メソッドの移行 — 1 行で `co_return out;` で終了

レガシー `execute_full` ユーザーの最も一般的な混乱:
「`NodeResult` は古い型だが、`NodeOutput` を返さなければならないのか？」
→ これらは同じ型のエイリアスです。単に `NodeOutput out;
out.writes=...; out.command=...; out.sends=...; co_return out;` とするだけ。

## 移行しないとどうなるか

v0.9.0 以降、レガシー 8 仮想メソッドは存在しません。

- C++ レガシー `override` は `'execute' marked override but does not override`
  のようなコンパイルエラーを生成。
- `execute()` のみを実装する Python ノードは `run(input)` を要求する
  `NotImplementedError` を発生。

古いメソッド名を残したままの移行パターンを使用しないでください。エンジンは
`run(NodeInput)` のみを呼び出すため、古い本体は決して実行されません。

## 一括移行スクリプトはあるか？

いいえ — 仮想メソッドのシグネチャが 8 形式にわたって変化するため、正規表現ベースの
変換は非実用的です。ユーザーはケースバイケースの例 (上記 4 例) を読み、
手動で移行する必要があります。

最も一般的なパターン (`execute(state)` のみのオーバーライド) については、以下の
sed/awk ワンライナーが初回パスの補助になる可能性があります — 人間による
レビューが必要です:

```bash
# Very rough initial pass — nodes with single-line execute override only.
# Always dry-run without -i first.
grep -lE 'execute\(const GraphState' src/**/*.cpp
# Manually edit each resulting file to the new pattern.
```

複雑なノード (`execute_full`、`execute_stream_async` など) は手動で
編集する必要があります。ショートカットはありません。

---

# 移行 2: `Provider` 互換性ポリシーと新しい明示的リクエスト API (v0.9+)

既存の `Provider::complete*` 4 メソッドとコールバックベースの `invoke()` は
削除計画のない安定 API です。既存の実装と呼出元は移行不要。ただし、新しい
`Provider` 実装は `CompletionProvider::do_invoke()` のみをオーバーライドし、
新しい直接呼出は `invoke_request(CompletionRequest)` を使用すべきです。

## 既存 vs 推奨新呼出パターン

| 安定互換 API | `CompletionProvider` を直接使用する場合の推奨 API |
|---|---|
| `complete(params)` | `run_sync(invoke_request(CompletionRequest::collect(params)))` |
| `complete_async(params)` | `co_await invoke_request(CompletionRequest::collect(params))` |
| `complete_stream(params, on_chunk)` | `run_sync(invoke_request(CompletionRequest::stream(params, on_chunk)))` |
| `complete_stream_async(params, on_chunk)` | `co_await invoke_request(CompletionRequest::stream(params, on_chunk))` |

`CompletionRequest` はコールバックの存在とトランスポートモードを分離します。
したがって、コールバックなしでも `CompletionRequest::stream(params)` が
明示的にストリーミングトランスポートを要求します。`Provider&` または `Provider*`
のみを保持するコードは既存の `complete*` メソッドを変更なく使用可能。

## ケースバイケース変換

### Provider を呼び出すユーザーコード

```cpp
// Stable compatible API — continues to be supported
auto completion = co_await provider->complete_async(params);
```

```cpp
// New code using `CompletionProvider` directly — mode is explicit
auto completion = co_await provider.invoke_request(
    CompletionRequest::collect(params));
```

### カスタム Provider サブクラス

既存の `Provider` サブクラスはそのまま動作し続けます。新しい実装は
`CompletionProvider` から継承し、`do_invoke()` のみを実装すべきです。
既存の `Provider` vtable と Python `complete()` 契約は変更されません。

```cpp
// Old pattern — async-native provider
class MyProvider : public neograph::Provider {
public:
    asio::awaitable<ChatCompletion>
    complete_async(const CompletionParams& params) override {
        // ... HTTP call ...
        co_return result;
    }

    ChatCompletion complete_stream(const CompletionParams& params,
                                   const StreamCallback& on_chunk) override {
        // ... SSE call ...
        return result;
    }

    std::string get_name() const override { return "my"; }
};
```

```cpp
// New pattern — transport mode is decoupled from callback presence
class MyProvider : public neograph::CompletionProvider {
public:
    asio::awaitable<ChatCompletion>
    do_invoke(CompletionRequest request) override {
        if (request.streaming()) {
            // Uses SSE/WS transport even without callback.
            // With callback, invokes request.on_chunk()(token) per token.
        } else {
            // ... HTTP call ...
        }
        co_return result;
    }

    std::string get_name() const override { return "my"; }
};
```

新しい呼出元はモードを明示的に指定。

```cpp
auto full = co_await provider.invoke_request(
    CompletionRequest::collect(params));

auto streamed = co_await provider.invoke_request(
    CompletionRequest::stream(params, on_chunk));

// Can explicitly request streaming transport even without observing tokens.
auto streamed_without_observer = co_await provider.invoke_request(
    CompletionRequest::stream(params));
```

レガシー 4 仮想メソッドと `invoke(params, on_chunk)` は引き続きサポート。
`CompletionProvider` final アダプタがすべてのレガシーエントリポイントを 1 度だけ
`do_invoke()` に接続し、相互再帰を防止。

## 自動キャンセル伝播

キャンセルが必要な場合は `CompletionParams::cancel_token` を指定。内部エンジン
ノードは params 内の `RunContext` トークンをプロバイダに渡します。スレッドローカル
暗黙伝播パスはもはや使用されません。

```cpp
// Node body inside engine — both cancel identically
co_await provider->invoke(params, nullptr);                    // OK
neograph::async::run_sync(provider->invoke(params, nullptr));  // OK
```

グラフ外の直接呼出元 (例: `Agent` ユーザーコード) も同じパターンに従います —
明示的な cancel_token がない場合、キャンセルは受信できません (以前と同じ)。

## 移行しないとどうなるか

対応不要:
- レガシー仮想メソッドのオーバーライドと直接呼出は引き続き動作。
- Provider 関連の `-Wdeprecated-declarations` 警告は発生しなくなった。
- 互換性とセキュリティ修正はレガシー API にも適用。

レガシー API の削除計画はありません。ただし、新機能は明示的リクエスト契約に
のみ追加される可能性があるため、`CompletionProvider` で実装する方が望ましい。

## 自動変換は可能か？

呼出サイトは単純なパターンに従う:

```bash
# Review with dry-run
grep -rnE '->complete(_async|_stream|_stream_async)?\(' your/code

# Then manually edit case-by-case (refer to the mapping table above).
```

レガシー `Provider` サブクラスの単一 `CompletionProvider::do_invoke()` への
統合はオプションであり、交錯したロジックのため手動編集が必要。

## 関連ドキュメント / 課題

- [`include/neograph/graph/node.h`](../include/neograph/graph/node.h) — 新しい
  `run(NodeInput)` 仮想メソッドのインラインドキュメント (例を含む)
- [ROADMAP_v1.md](../ROADMAP_v1.md) — Candidate 1 (GraphNode 8 仮想メソッド
  平坦化) の詳細設計ノート
- [troubleshooting.md](troubleshooting.md) — 実際の移行中に遭遇した
  コンパイルエラーと実行時の違い
- [Issue #5](https://github.com/fox1245/NeoGraph/issues/5) — Provider メソッド
  実装パスと恒久的互換性ポリシーの決定記録

---

# 移行 3: `compile()` ワーカープールデフォルトが 1 (v0.1.4 回帰復元)

## 変更点

`GraphEngine::compile(def, ctx)` のデフォルトワーカー数は
v0.1.4 (`b59444f`) から `std::thread::hardware_concurrency()` でしたが、
v1.0 で **`1` (= エンジン所有 thread_pool なし)** に復元されました。

## 理由

`hardware_concurrency` デフォルトはすべてのファンアウトノードに
スレッド間サブミットオーバーヘッド (~6-7 µs/task) を課します —
bench par 測定 (5 ワーカー + サマライザ) が 11.6 µs から 44 µs に回帰 (4× 減速)。
測定環境を bisect した結果、v0.1.4 の `b59444f` が原因と特定。

実運用ワークロード (LLM 呼出が ms~s 範囲) ではサブミットオーバーヘッドを
無視できますが:
- **単純なグラフ (ファンアウトなし)** もプールオーバーヘッドを支払う — 無意味
- **スレッドセーフでないノード状態** がデフォルトでマルチワーカーに露出 —
  実際の危険

したがって、デフォルトは安全に 1 に設定され、ユーザーは実際のファンアウト
並列化を明示的にオプトインする必要があります。

## 移行

ファンアウト並列化を必要とするグラフ (例: 複数の `Send` ディスパッチ、
`parallel_group`、deep_research の 5 研究者ファンアウト) は
`compile()` の後に明示的に呼び出す必要があります:

```cpp
auto engine = GraphEngine::compile(def, ctx);
engine->set_worker_count_auto();  // hardware_concurrency()
// or
engine->set_worker_count(4);  // specify exact N
```

```python
engine = ng.GraphEngine.compile(def, ctx)
engine.set_worker_count_auto()
```

単純なグラフ (ファンアウトなし) または軽量ファンアウトグラフ (LLM 呼出が支配的)
はデフォルトのまま — プールオーバーヘッド 0。

## 移行しないとどうなるか

- ファンアウトのあるユーザーグラフは単一スレッドで逐次実行 (一貫性保証)
- 実際の wallclock 回復は達成されない — 明示的な
  `set_worker_count_auto()` が必要

## 影響を受ける NeoGraph 内部サンプル

この変更と共に追加されたファンアウト可視化パッチ — 意図を保持するために明示的呼出を
追加。ユーザーコードが一致する場合、同じパターンを適用:

- `examples/10_send_command.cpp` — 同期 `sleep_for` ResearcherNode が Send 経由で
  ファンアウト、`engine->set_worker_count_auto()` 追加
- `examples/14_plan_executor.cpp` — 5 サブトピック Send ファンアウト (同期 sleep_for)、
  同追加
- `examples/21_mcp_fanout.cpp` — 3 MCP ツール呼出を同時発行、同追加
- `examples/36_classifier_fanout.cpp` — 既に `set_worker_count(5)` が明示的。
  偽のデフォルト (現在のデフォルトは hardware_concurrency) を述べていたコメント修正
- `src/core/deep_research_graph.cpp` `create_deep_research_graph()` ビルダー —
  `compile()` 直後に `set_worker_count_auto()` を呼出し、スーパーバイザの
  N 研究者が真に同時実行されるように

`examples/05_parallel_fanout.cpp` は `io_context` 上でコルーチンタイマーオーバーラップ
を使用 (同期 sleep なし) のため、ワーカープールは効果なし — 変更不要。

ユーザーコードに同じパターンが存在する場合:

```cpp
auto engine = GraphEngine::compile(def, ctx);
engine->set_worker_count_auto();   // ← add this line
```

詳細な測定については ROADMAP_v1.md の perf セクションを参照 (別途追加)。

---

# 移行 4: C++ ABI と必須再ビルド

NeoGraph は全ての公開バイナリライブラリにプロジェクト `VERSION` と
メジャー `SOVERSION` を設定します。v1 前は ABI 世代 0 ですが、`0.x`
間のバイナリ互換性は保証されません。changelog が境界を告知した場合は
全ての C++ コンシューマーを再ビルドしてください。特に `0.11.1` 以下から
bounded `NodeCache` を含むリリースへの更新では、`NodeCache` と
`EngineConfig` のオブジェクトレイアウト変更により再ビルドが必須です。

前述の Provider 移行は既存 `Provider` vtable を変更しません。将来の
`CheckpointStore` 非同期移行も同じポリシーに従い、v1 後は安定した
レイアウトの変更より別の capability interface と adapter を優先します。

プラットフォーム別の名前、既知の境界、CI 検証については
[バイナリ互換性ポリシー](ABI_POLICY.md)を参照してください。
