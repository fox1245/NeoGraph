<!-- neograph-i18n: source=docs/migration-v0.4-to-v1.0.md locale=ja source_sha256=37d0912e7f5f8a94f97a67c362602b9b38015ab141aa56aac51c33c118750d4c -->
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

イベント駆動Provider dispatchは別のpre-v1必須再ビルド境界です。`CancelToken`は`std::stop_source`を使い、executor非依存native subscription向け`stop_token()`を提供します。既存`cancel()`、`is_cancelled()`、`fork()`、`bind_executor()`、`slot()` callsiteはsource互換ですが旧inline実装はbinary互換ではありません。全C++ consumer/extensionを一致NeoGraph header/libraryで再コンパイルし、shared libraryのみを交換しないでください。通知はSDK `join()`、FIFO所有outcome、絶対deadline、admission/権限budgetを保持します。[ABI policy](ABI_POLICY.md)と[同条件実測](../benchmarks/provider-notification-summary.json)を参照してください。

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
| `execute_async(state)` | ネイティブ非同期: `co_await provider->invoke_async(std::move(request));` |
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

```cpp
#include <neograph/graph/node.h>
#include <neograph/graph/run_context.h>
#include <neograph/provider.h>
#include <neograph/runtime_interposition_consumer.h>

class ChatNode : public neograph::graph::GraphNode,
                 public neograph::RuntimeInterpositionConsumer {
    std::shared_ptr<neograph::Provider> provider_;
    std::string model_;
public:
    ChatNode(std::shared_ptr<neograph::Provider> provider, std::string model)
        : provider_(std::move(provider)), model_(std::move(model)) {}
    asio::awaitable<neograph::graph::NodeOutput>
    run(neograph::graph::NodeInput in) override {
        auto request = neograph::make_provider_request(
            *provider_, model_, in.state.get_provider_messages());
        request.cancel_token = in.ctx.cancel_token;
        request.options.deadline = in.ctx.deadline;
        auto result = co_await neograph::graph::observe_provider_result(
            in.ctx, invoke_provider(provider_, std::move(request), {}, {},
                neograph::graph::provider_call_broker(in.ctx),
                neograph::graph::make_provider_call_identity(in.ctx, get_name())));
        neograph::graph::record_usage(in.ctx, result);
        neograph::outcome_or_throw(result);
        neograph::graph::NodeOutput out;
        out.writes.push_back(neograph::graph::provider_messages_write(result));
        co_return out;
    }
    std::string get_name() const override { return "chat"; }
};
```

### ケース 3 — ストリーミングノード (`execute_stream` の移行)

公開契約は所有 typed 準備/dispatch であり、同期・非同期の virtual completion 対ではありません。`ProviderRequest.payload` は Chat、Messages、Responses、Gemini、Interactions の SDK リクエスト variant です。`ProviderMode::Collect` / `Stream` は観測者の有無と独立に転送を選択します。`on_event` は借用 typed `sp::Event` view を受け取ります。コールバック後に必要なデータだけコピーします。raw JSON override や portable projection による native 権限のインポートは認めません。

```cpp
#include <neograph/graph/node.h>
#include <neograph/graph/run_context.h>
#include <neograph/provider.h>
#include <neograph/runtime_interposition_consumer.h>

class StreamingChatNode : public neograph::graph::GraphNode,
                 public neograph::RuntimeInterpositionConsumer {
    std::shared_ptr<neograph::Provider> provider_;
    std::string model_;
public:
    StreamingChatNode(std::shared_ptr<neograph::Provider> provider, std::string model)
        : provider_(std::move(provider)), model_(std::move(model)) {}
    asio::awaitable<neograph::graph::NodeOutput>
    run(neograph::graph::NodeInput in) override {
        auto request = neograph::make_provider_request(
            *provider_, model_, in.state.get_provider_messages(), {}, {},
            neograph::ProviderMode::Stream);
        request.on_event = [sink = in.stream_cb](const sp::Event& event) {
            if (!sink) return;
            const auto* delta = std::get_if<sp::PartDelta>(&event);
            if (delta && delta->payload.kind == sp::PartKind::Text &&
                delta->payload.channel == sp::DeltaChannel::Content)
                (*sink)({neograph::graph::GraphEvent::Type::LLM_TOKEN,
                         "chat", neograph::json(std::string(delta->payload.bytes))});
        };
        request.cancel_token = in.ctx.cancel_token;
        request.options.deadline = in.ctx.deadline;
        auto result = co_await neograph::graph::observe_provider_result(
            in.ctx, invoke_provider(provider_, std::move(request), {}, {},
                neograph::graph::provider_call_broker(in.ctx),
                neograph::graph::make_provider_call_identity(in.ctx, get_name())));
        neograph::graph::record_usage(in.ctx, result);
        neograph::outcome_or_throw(result);
        neograph::graph::NodeOutput out;
        out.writes.push_back(neograph::graph::provider_messages_write(result));
        co_return out;
    }
    std::string get_name() const override { return "chat"; }
};
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

# 移行 2: typed lossless Provider 切り替え (再コンパイル必須)

ソースとバイナリの破壊的変更です。全 C++ 利用者とカスタムプロバイダーを新しい一致したヘッダー/ライブラリで再コンパイルします。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、descriptor interpreter、Responses WebSocket は alias/互換 bridge なしで削除されました。SDK は alpha `0.1.1`、interface revision 4 / shared ABI 4、out-of-line capability check を使用し、安定リリースの宣言ではありません。記録された interface-3 SDK runtime/archive 検証は Linux/POSIX の範囲で、interface 4 の資格検証ではありません。Windows NTFS と macOS の実装はありますが、新 platform の検証には runtime 証拠が必要です。WASM provider runtime の検証は確立していません。

削除済み `Provider::complete`、`complete_async`、`complete_stream`、`complete_stream_async` 呼び出しは明示 mode request と `invoke` / `dispatch`（または C++ async peer）へ移行します。`Agent::complete` は所有結果を返す別の one-turn API として残ります。

CMake 3.20 以上が必要です。Core は所有 typed provider 契約を公開するため `NEOGRAPH_BUILD_LLM=OFF` でも SchemaProvider runtime は必須です。明示的な `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` が優先され、それがなければ設置済み `SchemaProvider` runtime package を探します。なければ `NEOGRAPH_FETCH_SCHEMAPROVIDER=ON`（既定）の時に `cmake/NeoGraphSchemaProvider.cmake` が固定した不変 GitHub archive を取得します。Offline build は SDK を設置し、`CMAKE_PREFIX_PATH` に prefix を設定して `-DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` を渡します。Sibling checkout は推測せず、削除済み bundled interpreter も選択しません。NeoGraph の任意 HTTP module を無効にしても SDK runtime の transport 依存は必要です。記録された SDK runtime/archive 検証は Linux/POSIX の範囲です。Windows NTFS と macOS の実装はありますが、新 platform の検証には runtime 証拠が必要です。WASM provider runtime の検証は確立していません。

Python も C++ と同じ所有 request/outcome 境界を公開します: `make_provider_request`、`Provider.prepare`、`dispatch`、`invoke`。Provider 履歴には typed part を持つ `ProviderMessage` を使い、`ChatMessage` はグラフ用の便宜的 projection として残ります。SDK 失敗は `ProviderOutcome.failure` で読み、host observer/settlement 例外は `outcome` と `cause` を保持します。コンストラクターと GIL/コールバック動作は [Python binding ガイド](python-binding.md)を参照してください。

Interface 4 より前の installed find_package Program C++/C ABI/dualQuickJS consumer と NeoGraph/SchemaProvider typed2-request lifetime/native/raw/mismatch consumer は pass しました。これは過去の package 結果で、interface-4 pass の主張ではありません。より広い platform や安定 release は主張しません。

公開契約は所有 typed 準備/dispatch であり、同期・非同期の virtual completion 対ではありません。`ProviderRequest.payload` は Chat、Messages、Responses、Gemini、Interactions の SDK リクエスト variant です。`ProviderMode::Collect` / `Stream` は観測者の有無と独立に転送を選択します。`on_event` は借用 typed `sp::Event` view を受け取ります。コールバック後に必要なデータだけコピーします。raw JSON override や portable projection による native 権限のインポートは認めません。

```cpp
#include <neograph/provider.h>
#include <neograph/runtime_interposition_consumer.h>
#include <neograph/controlled_provider.h>

// Selected public declarations from neograph::Provider.
class Provider {
public:
    virtual ~Provider() = default;
    virtual std::string get_name() const = 0;
    virtual std::string_view family() const noexcept = 0;
    virtual PreparedProviderRequest prepare(ProviderRequest request) = 0;
    sp::runtime::Result dispatch(PreparedProviderRequest request);
    asio::awaitable<sp::runtime::Result> dispatch_async(PreparedProviderRequest request);
    sp::runtime::Result invoke(ProviderRequest request);
    asio::awaitable<sp::runtime::Result> invoke_async(ProviderRequest request);
    static std::string request_digest(const PreparedProviderRequest& request);
    static std::optional<std::uint64_t> conservative_token_upper_bound(
        const PreparedProviderRequest& request);
};
```

### ProviderRequest / ProviderControls

```cpp
#include <neograph/llm/schema_provider.h>
#include <neograph/types.h>

sp::runtime::Result call_provider(
    neograph::Provider& provider, std::string model,
    std::vector<sp::Message> history,
    std::function<void(const sp::Event&)> observer) {
    neograph::ProviderControls controls;
    controls.max_output_tokens = 128;  // optional caller-selected wire cap
    auto request = neograph::make_provider_request(
        provider, std::move(model), std::move(history), {},
        std::move(controls), neograph::ProviderMode::Stream);
    request.on_event = std::move(observer);
    auto prepared = provider.prepare(std::move(request));
    return provider.dispatch(std::move(prepared));  // owns Completion or Failure
}
```

Interface 4 は retained per-call control を `extra_fields` dictionary ではなく typed field で保持します:

| Family | 追加の `ProviderControls` |
|---|---|
| Chat | `chat_reasoning`、`include_reasoning`、`usage_include`、`models`。宣言済み OpenRouter origin が必要です。Scalar `reasoning_effort` は別です。 |
| Responses | `previous_response_id`、`previous_response_history`、`parallel_tool_calls`、`verbosity`、`truncation`、`responses_include`。 |
| Messages | `thinking_mode`、`output_effort`、`cache_control`、`messages_tool_choice`、宣言済み origin の OpenRouter `provider` routing。 |
| Gemini Generate | `gemini_history_mode`、`gemini_thinking_level`、`temperature`、`safety_settings`、`gemini_tool_choice`。 |

Messages manual thinking は policy が認める output cap 未満の budget を要求します。Adaptive/disabled mode は thinking budget を禁止します。Manual/adaptive thinking は有効な temperature を省略しますが、不正な値と model が禁止する temperature は省略前に拒否します。Disabled thinking は承認済み temperature を出力します。Model-prefix 制限は承認済み policy の事実で、全 model または最後の `/` 以後の suffix を ASCII 大小文字を区別せず照合します。Gemini thinking level と thinking budget、および typed tool choice と `required_tool` はそれぞれ同時に指定できません。非対応 family/origin/value の組合せは I/O 前に拒否します。

### PreparedProviderRequest / ProviderBudgetClaim
`prepare()` は検証とエンコードを正確に一度行い、元の deadline とキャンセル状態を持つ移動専用 `PreparedProviderRequest` を生成します。永続呼び出し元は `Provider::request_digest()` を assembly に結び付け、承認された budget claim を予約し、dispatch receipt を記録してから、同じハンドルを `ControlledProvider::dispatch_prepared(_async)` で消費します。gate 後の再生成はありません。重複 receipt は再送しません。カスタム実装は `get_name()`、`family()`、`prepare()` を実装し `prepare_runtime()` または `prepare_local()` を使います。local callback は `this` ではなく所有 shared 状態をキャプチャします。

任意の `ProviderControls` は呼び出し元の選択であり、強制デフォルトや黙った cap clamp ではありません。非対応 family 制御は dispatch 前に拒否します。有界呼び出しには承認された実際のモデル input/output 上限が必要で、欠落は `LimitUnknown` です。予約は保守的な支出権限であり、報告使用量・予測・請求書ではありません。不明/部分/delivery-unknown の結果は hold を維持し、実際の最終報告で精算し、超過報告も全量を計上します。retry は明示的な単一層で、既定 off、有界 window と unknown-prior hold を使います。隠れた再送はありません。


プロバイダー呼び出しは `sp::runtime::Result`、すなわち `sp::Completion` または `sp::Failure` を保持する不変の所有 `std::shared_ptr<const sp::Outcome>` を返します。表示テキストだけでなく結果全体を保持してください。順序付きメッセージ/パート、保持された native continuation と family が提供する wire 証拠、順序付き raw 観測、停止の根拠と実際の試行メタデータは呼び出しとクライアント破棄後も残ります。`input_total`、`output_total`、`total` などの使用量カウンターは `std::optional<sp::Count>` で、存在する count は `uint64_t value` と `Evidence` を持ちます。`Usage` は stage、quality、conflict も記録します。欠落は不明であり、ゼロを作りません。失敗も元の部分結果を保持します。`ProviderFailure::outcome()` と `ProviderObserverError::outcome()` は実際の結果を保持し、後者の `cause()` は観測者の例外を保持します。

Wire 証拠は family が提供する任意の情報です。`sp::Completion::wire_envelope` は null の場合があります（Python の `ProviderCompletion.wire_envelope` は `None`）。現在の buffered Chat は応答 JSON 全体を `raw_events` 内の `RawWire` に保持します。`type == "chat.completion"` で、文書は `payload` にあり、`wire_envelope` は null のままです。Family が実際に保持する場所から証拠を読み、fallback envelope は捏造しません。Native continuation と raw buffer は保護された証拠として保持され、trace payload から除外されます。

`UsageAccumulator::snapshot()` は累積報告を返します。`total_tokens_wide()` は計上済みトークンと未解決予約の合計で、報告使用量として表示してはいけません。精算には input/output count のある final・consistent 報告が必要で、根拠のある最大 total を計上し、超過使用量も clamp しません。累積対象の一つでも counter が欠落すれば集計も不明です。予約、ローカル計上、vendor 請求書は別の記録です。

実結果の後に post-effect 精算や terminal receipt 永続化が失敗すると、`ProviderDispatchOutcomePersistenceError::outcome()` は元の不変結果、`cause()` は元の永続例外を保持します。delivery も失敗した場合は `delivery_error()` が元の観測者例外を保持します。永続化成功後の観測者失敗は元の例外を変更せず再送出し、不明/結果なし transport 失敗では outcome を捏造しません。
### SchemaProvider

`SchemaProvider` は承認済み `sp::descriptor::ValidatedDescriptor`、`sp::runtime::Options`、任意の `SchemaProvider::Defaults` を受け取ります。descriptor は closed/versioned データ admission であり、要求/応答 interpreter や任意 primitive registry ではありません。credential は公開 descriptor でなく runtime options に置きます。Defaults は typed OpenRouter routing と Responses 保持 (`responses_store`) のみで、後者は Responses 専用です。Hosted OpenRouter routing・retention・JSON 形式は宣言済み typed 制御です。Images、Veo、Decisions は別の NeoGraph typed client と別の承認を使い SDK chat grant を継承しません。

```cpp
#include <neograph/llm/schema_provider.h>
#include <descriptor/descriptor.h>
#include <stdexcept>
#include <variant>

std::shared_ptr<neograph::llm::SchemaProvider> admitted_provider(
    std::string_view descriptor_json, std::string api_key) {
    auto loaded = sp::descriptor::load(descriptor_json);
    if (const auto* error = std::get_if<sp::descriptor::ConfigError>(&loaded))
        throw std::invalid_argument(error->message);
    sp::runtime::Options options;
    options.api_key = std::move(api_key);
    neograph::llm::SchemaProvider::Defaults defaults;
    return std::make_shared<neograph::llm::SchemaProvider>(
        std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded)),
        std::move(options), std::move(defaults));
}
```

通常の `sp::descriptor::load` は header を literal として扱い、`${VAR}` を展開しません。Admission 前の明示的 host preprocessing には `sp::descriptor::load_with_environment_headers(source, overrides, policy)`、または `DeploymentHeaderEnvironment` を受け取る決定的 `load_with_deployment_headers(source, overrides, environment, policy)` を使います。Environment helper は Messages の任意 `ANTHROPIC_WORKSPACE_ID` / `ANTHROPIC_BETA` を読み、未設定または空の値は省略します。Literal descriptor header が environment に優先し、explicit override が両方に優先します。名前は大小文字を区別しません。重複 override、不正・reserved header は最終 admission で拒否します。Encoder は template を評価せず、承認済み descriptor も変更しません。

### Native 履歴 / 予算

`ChatMessage` / `ChatTool` と JSON は portable projection であり native 権限ではありません。Portable 形式は [`provider-message-v2`](../schemas/provider-message-v2.schema.json)、[`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json) のままです。真正な C++ checkpoint sidecar はメモリ内の native seal を保持します。永続 native 履歴には host-owned `sp::NativeArchive` が必要です。closed v3 / `spna3` は独立キーによる認証済み owner-private custody で、archive v2 は更新・解釈せず拒否します。認証は全 semantic descriptor 選択（origin/path/header、policy、要求 field mapping、usage path、stop mapping）、owner と正確な custody binding を結び付けます。暗号化や vendor-issuer 認証ではありません。archive 本文・キー・native blob・raw wire 観測は公開しません。Archive は証拠保存であり、金銭 grant や spending lease ではありません。Program/external bank は独立 journal が所有し、snapshot コピーで credit は作れません。

Interface 4 は native replay の configuration digest から output-generation cap だけを除きます。Content、origin、route、policy identity、prefix、tool、reasoning control は引き続き結び付けられます。実際の cap は encoded request と prepared-request digest に残ります。Cap を増やす semantic call ごとに新しい resource-bank admission、固有の決定的 call ordinal/effect identity、元の deadline が必要です。精算済み call slot の再利用、credit 更新、seal 修復、不確実な effect の再送はできません。Archive v3 と portable JSON v2 は変更されません。Old-policy native archive はその policy に結び付いたままで、異なる policy では拒否し、migration しません。

Same-route native continuation と明示的 foreign projection は異なる契約です。Gemini の既定は `sp::gemini::HistoryMode::NativeOnly`。`PortableForeign` は native seal、wire output、signature のない caller-created assistant Text/ToolCall history を認めます。各 foreign assistant turn の最初の function call だけに `skip_thought_signature_validator` を付け、text-only history に signature は作りません。真正な native group は厳密に検証し、失敗・不一致 seal を除去して portable history に降格させません。Responses `previous_response_id` は provider-held state を選ぶため、`messages` には新 input だけを送ります。`previous_response_history` は送信しない local ownership 証拠です。Client-tool ownership に必要なら全 original prefix と、ID が cursor と一致する真正な terminal assistant を渡します。以後の in-process cursor 結果は private completed ownership を保持し、full `NativeReplay` や archive authority にはなりません。Cursor は native archive や portable import grant ではありません。

**Standalone bank journal 修正 — 現在の契約を改訂；実際の runtime 証拠は下記。** Owner-approved protocol は単調 trusted-store namespace obligation と、実際の不変 original owner/thread/graph scope、ceiling、deadline/clock identity、generation を要求します。全 checkpoint commitment/revision に対する正確な durable head CAS だけが host-owned opaque lease を発行できます。正確な pending effect window を provider I/O 前に永続化し、真正な SDK outcome と実際の charge、nullable report、hold、dedup identity で精算しなければなりません。Checkpoint/next head は同じ owned actor/revision 下で原子的に publish します。Bank metadata 削除、checkpoint pruning、old authenticated snapshot replay、同一 ID overwrite、actor 喪失で credit を与えてはなりません。既存 65 hold がある ceiling 130 を 129 に下げると別の 65 は許可できません。証明済み no-effect 失敗は unchanged head を release し authentic 130 復旧を可能にできます。Crash/unknown/lost-lease window は refund/retry/fallback なしで hold を保持します。Plain/pristine archive 設定は money/native spending lease を与えず、現在の `config.usage` は既存 standalone obligation を置換できません。Program/external-bank journal 所有は不変です。これは要求契約です。実際の currency/custody 証拠と instrumentation 制約は下記であり、安定 released API 保証ではありません。

**現在の宣言；統合 runtime 証拠は下記:** `<neograph/graph/checkpoint.h>` は `owner_scope`、logical `thread_id`、private backend `storage_thread_id`、`graph_identity`、`original_ceiling`、`original_deadline_ticks`、`deadline_clock_identity` を持つ `ManagedBudgetLeaseScope` を宣言します。`OwnedManagedBudgetLease` は read-only `scope()`、`actor_id()`、不変 `bank_generation()`、`revision()`、`head_checkpoint_id()`、`head_commitment()` を公開し、公開 authority-import constructor はありません。`ManagedBudgetEffectReceipt` は `active()`、`effect_id()`、`claim_amount()`、`request_digest()` を公開し、default receipt は権限を与えません。`CheckpointStore` は `acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`、`begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`、`settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`、`publish_managed_budget_checkpoint(lease, checkpoint)`、`release_managed_budget_lease(lease)` と `_async` counterpart を宣言します。Sync `CheckpointStoreCore` と `AsyncCheckpointStore` はそれぞれの variant を公開します。`managed_budget_checkpoint_commitment(checkpoint)` は bank JSON だけでなく全永続 checkpoint を結び付けます。これらの宣言は backend CAS、currency 安全性、installed ABI 互換性、実際に成功した runtime 経路を証明しません。

**真正な InMemory shared-bank fork は保持・実証済み。** 元の真正な C++ fork は ONE original financial journal と trusted current branch head を使い、grant を複製しません。`publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)`（および `_async`）は authentic current source/full commitment と実際の same-bank native C++ pointer を要求し、durable standalone fork は明示的に unsupported のままです。`OwnedManagedBudgetLease::scope()` と original owner/thread/graph、ceiling、deadline/clock、generation は不変です。Read-only store-issued `execution_thread_id()` / `execution_storage_thread_id()` は execution branch を別に選び、`GraphState::budget_original_thread_id()` は元の financial bank を示します。正確な selected-branch head CAS と global actor/revision は canonical current counter、pending effect、burned identity に対して全 branch を直列化します。Original/fork branch は補充なしで使用可能なままです。Stale snapshot、checkpoint copy、imported JSON は alias を発行したり head を巻き戻したりできません。元の root30 → charge3 → original continuation6 → fork lower20 → continuation9 の same-bank 証明は未変更 test_graph_engine.cpp:810–913 で PASSED です。Saved original ceiling30 は effective fork ceiling20 と別です。Widening31 と JSON-only restore は拒否必須です。Unbounded reported observation は factual data で finite grant ではありません。証明済み zero-effect lease だけが unchanged head を release でき、unknown/pending effect は obligation を保持します。

**現在の release-error 契約；実際の suite/probe は下記。** `<neograph/graph/engine.h>` の `graph::ManagedBudgetLeaseReleaseError` は `ProviderOutcomeError` を継承します。`cause()` は元の execution exception を保持し、`release_error()` は二次 durable lease-disposition 失敗を公開します。`outcome()` は真正な SDK 証拠があれば保持し、SDK outcome がなければ null です。Release 失敗は結果を捏造せず再 dispatch も許可しません。Closed `_neograph_managed_budget_scope` metadata は元の logical scope/cap/deadline clock/generation を記述しますが、backend CAS 権限ではなく data です。

**Archive-owner/retention 契約；実際の suite/probe は下記。** Finite standalone root または authenticated finite source だけが、実際に設定された `sp::NativeArchive::owner_scope()` から省略された original owner を継承します。Unbounded/plain owner metadata の意味は不変です。明示的に矛盾する archive owner は lease acquire 前に拒否します。`CheckpointStore::retains_native_checkpoint() const noexcept` と対応する Core/Async storage capability は既定 false で、実際の InMemory backend は true に override し、wrapper は実際の retention を委譲します。この read-only 記述は正当な unleased/plain/unbounded C++ native checkpoint custody を許しますが、spending credit や native replay authority は与えません。Leased custody は JSON flag や推測した store type ではなく実際の store-issued receipt を使います。

**Native-custody pre-I/O gate；実際の suite/probe は下記。** Managed effect begin は pending-effect/slot/held-window 変更前に、真正に結び付けた NativeArchive または実際の local store-issued private C++ retention capability を要求します。Private capability は JSON から import せず wire にも転送しません。C++ sidecar は境界を越えられないため、remote backend が InMemory でも gRPC は実際の client/server archive を要求します。Archive が finite source owner を提供しなければ元の anonymous owner scope は空のままで、実際の archive binding は original scope に一致しなければなりません。Financial head/lease 証拠だけでは native-custody readiness を証明しません。

診断 JSON は構文上有効な duplicate-key 文書も含め元の raw byte を保持しますが、実行可能な要求/config admission は重複を拒否します。元の non-2xx 応答 JSON は二度目の損失 parse なしで `http.error` 証拠に残ります。named SSE error は後続の正常 stream close より優先します。診断/provider metadata の上限は承認済み source extent であり、無関係な小さい error-text cap ではありません。

`ProviderRequest::observer_limits` は host-only です。明示した `max_events`・`max_bytes` は正数で、承認済み SDK 配信上限を下げることしかできません。`provider-request/v3` digest は実効 limit、mode、encoded body、retry policy と全 semantic descriptor binding を結び付けます。Bridge は queued/draining batch を通じて実際の PMR vector/map capacity と所有 event/document byte を計上し、queue mutex 外で cancellation を要求します。`messages` という名の Generic channel を chat に強制変換しません。native `history` channel を `messages` に mapping すると C++ sidecar が保持され、JSON から native 権限を作りません。

`ProviderOutcomeError` は結果を保持する共通 host-error base です。`ProviderObserverError` と `ProviderDispatchOutcomePersistenceError` は完全に drain した SDK 結果と元の `cause()` を保持し、後者は二次 observer 失敗も `delivery_error()` に保持します。`ProviderFailure::outcome()` は SDK 失敗自体を保持します。これは Node/Program の再 dispatch 権限ではありません。Transport retry の唯一の所有者は SDK で、caller が選んだ `max_output_tokens` を黙って clamp しません。

`ProgramFailure` は live `provider_outcome`・`provider_cause` を保持します。Canonical factual SDK witness は真正な archive custody を owner/run/version/bundle/operation/attempt に結び付け、Runtime は復旧失敗を公開する前に設定済み custody を eager に復元します。公開 data-only `ProgramResult::create()` は事前入力 witness で迂回できず、未解決の parsed seal は実行結果ではありません。プロセス再起動後は元の exception pointer がなく `provider_cause == nullptr` であり、text から再作成しません。永続化できない失敗は serialize/publish/replay できません。

`RecordedBindingSet` は source-bound の move-only data で、caller 提供 dispatcher ではありません。信頼された Catalog の `recorded_capability_binder` は実際の永続 source event を独立に読み、captured-only capability を materialize します。`ProgramRuntime::replay_recorded()` は元の selected-source permission を検証し、実際の残存 bank を durable CAS で移します。inherited spend は新しい model grant ではありません。旧 `start_recorded` 更新 API は削除されました。InMemory/File/SQLite/PostgreSQL Program store は実行全体で正確で不変の owned lease を保持し、expiry による更新をしません。Controlled JavaScript も underlying capability manifest を検証し、正確な completed command 結果を消費して external effect を再 dispatch しません。

**Recorded-control causal fix は full suite で実証済み。** Captured command replay は実行前に新しい CPU wall-time/Core work だけを durable に reserve し、測定済み work と新しく生成した Core checkpoint を result CAS で publish します。新しい model、money、Program-operation allowance を消費せず、captured external effect を再 dispatch しません。未精算 reservation は debit を保持します。Reservation により、最初の新しい Core checkpoint を拒否した通常の Running→Running transition ではなく認証済み settlement transition を選びます。Await channel receive、timer wait/cancel、handoff wait の開始/release は owning executor/strand 上で直列化します。既存 Recorded CPU/Memory await/handoff scenario は full suite で pass しました。Remote TSan coverage 制約は下記に明記します。

以下の観測はこの文書整備より前に記録されたものです。歴史的証拠であり、新 test 実行や全 platform・transport・security 性質の保証ではありません。

**有料観測は完了；普遍的な qualification ではありません。** 元の `SPQUAL1` base630/1000000 microUSD は不変です。同じ元 ledger の ONE hash-chained `A` が承認済み extension480/3000000 を受け入れ、aggregate1110/4000000 になります。Calls/spent/hold/settlement は累積で新 grant ID/header/reset はありません。正確な declaration byte/file identity と original authorization/baseline/catalog/activation/ledger-prefix の hash/totals は固定され、削除・置換・変更は fail closed です。最終 canonical ledger は calls1110/spent437958/held1287828 microUSD、eventA1、limits1110/4000000；spent+held US$1.725786 は LOCAL catalogue meter で invoice ではありません。記録済み five-family60-pair baseline は600 request 完了：Chat60/60、Responses60/60、Messages60/60、Generate56/60（incorrect-vision SSE4件）、Interactions57/60（incorrect-vision buffered1件/SSE2件）；合計293/300 pair で300/300ではありません。他の old600 financial record は保持しますが完全な behavioral proof ではありません。以前の M5/media one-shot cohort は不変です。以前の Google3-round prerequisite は invalid-tool2件/unreadable-positive1件の失敗状態を保持します。追加有料呼出しは承認されません。最終 SDK 証拠と native-axis 制約は baseline 成功とは別です。 以前の activation/reopen smoke は2回 reopen 後 calls610/spent219159/held751233、SDK meter/canary/vision4-test19.38秒 pass として保持します。これは限定された以前の checkpoint で最終 ledger totals ではありません。以前の検証済み Chat60-pair cohort は実際の attempt120、UpperBound charge120、UnknownHold なしを保持します。

**Native-axis 観測は cryptographic 検証・native consumption/equivalence ではありません。** Generate は mutation/omission/duplication を受け入れました。Interactions は isolated genuine source/positive control、one-owner signature mutation、thought-carrier omission、call-carrier omission、duplication を受け入れました。全 thought/signature 削除は generic400、THOUGHT item を保持して全 signature field を削除した場合も generic400 でした。最後の capture は local encoded-original retention control で、same-capture server positive ではありません。以前の positive cohort は真正です。観測は aggregate-carrier-absence boundary のみを示し、issuer/signature 検証や vendor consumption を証明しません。実際の report：SDK `config/qualification-extension-results.json`、`qualification-final-summary.json`、`qualification-native-axis-results.json`、`qualification-combined-omission-results.json`、`qualification-signature-presence-results.json`。Prerequisite-failed/not-run/negative-inconclusive の状態は事実のままです。 Thought-only/carrier-only omission は別 carrier が残る状態で受け入れられました。Issuer-validation/native-consumption の主張を強めません。

**実際の統合証明と残る制約。** 最新 Core full run は2242 test、失敗0、skip16（RAM process-loss 非適用14件/live-credential gate2件）、130.17秒です。`PgNestedJsonRoundTrips` は duplicate key/order/null metadata、blob、residual を正確に保持し0.18秒で pass。未変更の元 shared-bank fork と既存 Recorded CPU/Memory await/handoff scenario も pass。実際の wrappedMemory/SQLite/PostgreSQL/gRPC finite130/hold65/lower129/strip/old-head/pruning/no-archive/import probe は plain と ASan+UBSan で pass。LOCAL Memory/SQLite/PostgreSQL TSan scope は7件 pass、warning0。System Abseil/Protobuf を含む full mixed gRPC TSan は exit66、dependency/generated-RPC stack に race warning402件。これは instrumentation/coverage 制約で proven false positive ではありません。Remote TSan/race-free は主張せず warning を suppress しません。Installed find_package Program C++/C ABI/dualQuickJS の3 consumer は pass。Fresh installed NeoGraph/SchemaProvider typed consumer は実際の HTTP request2件、coroutine 開始前の provider 破棄、native/tool replay、refusal、known-zero/raw 保持、実際の LinkedMismatch 拒否で pass。Browser Alice/Bob isolation と generation2 replacement を目視検証し、PostgreSQL Program Chat black-box6件は18.989秒で pass。最新 SDK26/26 は失敗0、74.07秒で pass。最終 ReleaseGraph16設定 ×fresh process3回/48記録は38.29秒、失敗0、全 actual protocol/owned-outcome check pass で完了しました。NeoGraph `benchmarks/provider-cutover-final-results.json` と `benchmarks/provider-cutover-final-summary.json` は独立した最終 cohort を保持します。測定中 compiler/有料 model は実行せず、歴史 cohort は不変で semantic/resource equivalence は主張しません。Unstable SDK/ABI3 は安定 release や広い platform qualification ではありません。

**最小有料証拠（2026-10-03）は広範な qualification ではありません。** 個別承認の one-shot 3 呼出しの結果：Images—JPEG 1 個、1024×1024、360685 byte、input/output/total token 19/1408/1427、実際に目視確認；Veo—MP4 1 個、1280×720、4 秒、437737 byte、generation 1 回と status GET 3 回、usage nullable、Chromium で decode・目視確認；Decisions—`typesafe/jev-1.13`、probability 0.93、input/output token 283/21、total 不明、API 報告費用 USD 0.000011886。Image USD 0.0336 base + text/thinking、Veo USD 0.20 は catalog 予測で invoice ではなく、最小 image smoke は価格帯内訳を取得していません。結果は one-shot 権限を更新せず再実行も許可しません。

完了した chat pair は downstream vendor の native-continuation 消費を証明しません。

Stage 3 (2026-04) の設計と当時の測定テスト数は歴史として保持します。provider 互換/crossover の決定は以下の typed lossless 移行に置き換わり、旧設計台帳は現在の provider API ではありません。

- [ABI_POLICY.md](ABI_POLICY.md)
- [ASYNC_GUIDE.md](ASYNC_GUIDE.md)
- [Issue #5](https://github.com/fox1245/NeoGraph/issues/5) — historical decision; superseded provider compatibility policy.

---

# 移行 3: `compile()` ワーカープールデフォルトが 1 (v0.1.4 回帰復元)

## 変更点

`GraphEngine::compile(def, ctx)` のデフォルトワーカー数は
v0.1.4 (`b59444f`) から `std::thread::hardware_concurrency()` でしたが、
現在の pre-v1 API で **`1` (= エンジン所有 thread_pool なし)** に復元されました。

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

Worker pool を選ばなければ CPU-bound fan-out は呼び出し元 executor 上で動き、非同期 I/O は重なる場合があります。別の実行 thread が必要なら `set_worker_count_auto()` または明示 worker count を使います。Worker count だけで一貫性や高速化は保証しません。

## 影響を受ける NeoGraph 内部サンプル

この変更と共に追加されたファンアウト可視化パッチ — 意図を保持するために明示的呼出を
追加。ユーザーコードが一致する場合、同じパターンを適用:

- `examples/10_send_command.cpp` — 同期 `sleep_for` ResearcherNode が Send 経由で
  ファンアウト、`engine->set_worker_count_auto()` 追加
- `examples/14_plan_executor.cpp` — 5 サブトピック Send ファンアウト (同期 sleep_for)、
  同追加
- `examples/21_mcp_fanout.cpp` — 3 MCP ツール呼出を同時発行、同追加
- `examples/36_classifier_fanout.cpp` — 既に `set_worker_count(5)` が明示的。
  偽のデフォルト (現在の既定は 1、engine-owned pool なし) を述べていたコメント修正
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

> Stage 3 (2026-04) の設計と当時の測定テスト数は歴史として保持します。provider 互換/crossover の決定は以下の typed lossless 移行に置き換わり、旧設計台帳は現在の provider API ではありません。

---

# 移行 4: C++ ABI と必須再ビルド

NeoGraph は全ての公開バイナリライブラリにプロジェクト `VERSION` と
メジャー `SOVERSION` を設定します。v1 前は ABI 世代 0 ですが、`0.x`
間のバイナリ互換性は保証されません。changelog が境界を告知した場合は
全ての C++ コンシューマーを再ビルドしてください。特に `0.11.1` 以下から
bounded `NodeCache` を含むリリースへの更新では、`NodeCache` と
`EngineConfig` のオブジェクトレイアウト変更により再ビルドが必須です。

Typed Provider 移行は virtual 契約を `get_name`、`family`、`prepare` に変え、旧 completion virtual を削除します。既存 Provider binary は互換でなく、カスタム provider と全依存 C++ 利用者を一致する header/library で再ビルドします。Core も SchemaProvider 型を公開するため LLM 無効 build にも SDK runtime が必要です。将来の安定 interface は安定 layout の変更より別の capability interface と adapter を優先すべきであり、現在の pre-v1 interface は binary 互換を約束しません。

プラットフォーム別の名前、既知の境界、CI 検証については
[バイナリ互換性ポリシー](ABI_POLICY.md)を参照してください。
