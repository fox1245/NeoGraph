<!-- neograph-i18n: source=docs/migration-v0.4-to-v1.0.md locale=ja source_sha256=6dd79d67e013f5ea48fb53ece7b2a02345a827205a29cc1294b62a7bffdba92d -->
# マイグレーションガイド：従来の8個の仮想関数 → `run(NodeInput)`（v0.4.x → v0.9+）

**Languages:** [English](migration-v0.4-to-v1.0.md) | [한국어](migration-v0.4-to-v1.0.ko.md) | [日本語](migration-v0.4-to-v1.0.ja.md) | [简体中文](migration-v0.4-to-v1.0.zh-CN.md)

NeoGraph v0.4で、ノードのエントリポイントが、`run(NodeInput) ->
awaitable<NodeOutput>` の1つに統合されました。従来の8個の仮想関数（`execute` / `execute_async` /
`execute_stream` / `execute_stream_async` と、それぞれの `_full` 版）は、v0.4.xで非推奨になり、v1への準備リリースであるv0.9.0で削除されました。この文書では、従来のノードを現在のAPIへ移行する手順を説明します。

> v0.9.0以降では、`run(NodeInput)` を実装していないC++のサブクラスは、抽象クラスとしてコンパイルに失敗します。Pythonのサブクラスも、`run(self, input)` を実装する必要があります。

イベント駆動のProviderディスパッチにより、v1より前の再ビルド必須の境界が、もう1つ加わりました。`CancelToken` は `std::stop_source` を使うようになり、executorに依存しないネイティブの購読のために、`stop_token()` を公開します。既存の `cancel()`、`is_cancelled()`、`fork()`、`bind_executor()`、`slot()` の呼び出し箇所は、ソースの互換性を保ちます。ただし、古いインライン実装とは、バイナリ互換ではありません。すべてのC++コンシューマーと拡張を、対応するNeoGraphのヘッダーとライブラリで再コンパイルしてください。共有ライブラリだけを置き換えても足りません。プロバイダーの通知は、SDKの `join()`、FIFOで所有される結果、絶対的なデッドライン、admissionと権限の予算を保ちます。[ABIポリシー](ABI_POLICY.md)と[対応する測定結果](../benchmarks/provider-notification-summary.json)を参照してください。

## 移行する理由

従来のパターンでは、`(sync/async) × (writes/full) × (stream/non-stream)` の直積で、仮想関数が8個ありました。どれか1つをオーバーライドすると、残りの7個はデフォルトの連鎖にフォールバックします。安全な組み合わせもありますが、実行時の落とし穴があります（たとえば、同期の `execute_full` と非同期ディスパッチの組み合わせでは、入れ子の `run_sync` で競合が起きます）。そのため、利用者がどの関数をオーバーライドすべきか、分かりにくくなっていました。

新しいパターンでは、`run(NodeInput) -> awaitable<NodeOutput>` が1つあるだけで、オーバーライドするのもそれだけです。同期か非同期かは、呼び出し側が考えることです（コルーチンの中では `co_await` を、通常の同期コードでは普通のコードを、自由に使えます）。Command と Send は `NodeOutput` に含まれるので、仮想関数を増やす必要はありません。ストリーミングのコールバックは、`NodeInput::stream_cb`（nullになりうるポインター）経由で渡されます。

## 8個の仮想関数から新しい `run()` への対応

| 従来の仮想関数 | 移行後の形 |
|---|---|
| `execute(state)` | `NodeOutput out; out.writes = {...}; co_return out;`（同期の本体） |
| `execute_async(state)` | `co_await provider->invoke_async(std::move(request));` のような、ネイティブの非同期 |
| `execute_stream(state, cb)` | `if (in.stream_cb) (*in.stream_cb)(event); co_return NodeOutput{...};` |
| `execute_stream_async(state, cb)` | 上と同じ形に、ネイティブの非同期（`co_await ...`）を加えたもの |
| `execute_full(state)` | `NodeOutput out; out.writes=...; out.command=...; co_return out;` |
| `execute_full_async(state)` | 上と同じ形に、ネイティブの非同期を加えたもの |
| `execute_full_stream(state, cb)` | `execute_full` の形に、`in.stream_cb` の利用を加えたもの |
| `execute_full_stream_async(state, cb)` | 上と同じ形に、ネイティブの非同期を加えたもの |

要点：**8つの種類は、`NodeOutput` のどのフィールドを設定するか、`in.stream_cb` を使うか、`co_await` を使うか、の組み合わせで表せます**。仮想関数は1つだけが残ります。

### Pythonで最も多い移行

**従来のコード：**

```python
class CounterNode(ng.GraphNode):
    def execute(self, state):
        current = state.get("count") or 0
        return [ng.ChannelWrite("count", current + 1)]
```

**現在のコード：**

```python
class CounterNode(ng.GraphNode):
    def run(self, input):
        current = input.state.get("count") or 0
        return [ng.ChannelWrite("count", current + 1)]
```

Pythonの `run` は、`async def` ではなく、通常の `def` です。ストリーミング実行では、`input.stream_cb` はイベントを受け取る関数です。通常の実行では `None` になります。

## ケース別の変換例

### ケース1 — 最も単純な同期ノード

**従来：**
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

**新しい形：**
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

違いは次のとおりです。
- `state` → `in.state`
- 戻り値を `NodeOutput`（`writes` フィールド）に包む
- 関数は `asio::awaitable<NodeOutput>` を返し、`co_return` で終わる

### ケース2 — 非同期のLLMノード（`execute_async` の移行）

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

### ケース3 — ストリーミングノード（`execute_stream` の移行）

公開されている契約は、所有権を持つ型付きの準備とディスパッチであり、対になったcompletion用の仮想メソッドではありません。`ProviderRequest.payload` は、Chat、Messages、Responses、Gemini、Interactionsのリクエストを表す、SDKのvariantです。`ProviderMode::Collect` / `Stream` は、オブザーバーとは独立に、トランスポートを選びます。`on_event` が受け取るのは、借用された、型付きの `sp::Event` のビューです。コールバックの後に必要なデータだけを、コピーしてください。生のJSONによる上書きや、ポータブルな射影を通じたネイティブ状態の取り込みは、受け付けられません。

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

### ケース4 — CommandまたはSendを使うノード（`execute_full` の移行）

**従来：**
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

**新しい形：**
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

`NodeOutput` は `NodeResult` の別名なので、従来の `NodeResult` のコードも、そのままコンパイルできます。

## よくある間違い

### `NodeInput in` は値渡しにする

```cpp
// ❌ Wrong — coroutine ref-param UAF, SEGV in pybind async path
asio::awaitable<NodeOutput> run(const NodeInput& in) override { ... }

// ✅ Correct
asio::awaitable<NodeOutput> run(NodeInput in) override { ... }
```

理由：安全のため、コルーチンフレームは引数のコピーを持つ必要があります。参照で受け取ると、呼び出し元のスタックフレームが消えた後で、`in.state` がぶら下がった参照になります。PR 2の作業中に、実際に起きたバグです。

### cancel / store / stream_cb は、すべて `in.ctx` から取得する

従来のノードは、`state.run_cancel_token_` のような裏口の経路でキャンセルトークンを受け取っていましたが、v0.4で、正式な受け渡しの仕組みとして `RunContext` が導入されました。

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

C++のノードが使える `in.ctx` のフィールドは、`cancel_token`、`usage`、`thread_id`、`step`、`stream_mode`、`store`、`resume_value`、`deadline`、`trace_id` です。最後の2つは `RunMetadata` に設定します。エンジンは、ネストしたサブグラフにもこれらを引き継ぎます。チェックポイントのルーティングは、エンジンの内部にとどまり、`RunContext` の公開フィールドではありません。Pythonが公開するのは、`trace_id`、`run_id`、`model_token_budget`、`has_deadline`、`deadline_remaining_ms` です。C++のsteady clockの生のデッドラインは、意図的に公開されていません。

### `_full` の仮想関数の移行 — 最後は `co_return out;` の1行で終える

従来の `execute_full` の利用者が最も混乱するのは、「`NodeResult` は古い型なのに、`NodeOutput` を返さなければならないのか」という点です。
→ この2つは、同じ型の別名です。`NodeOutput out;
out.writes=...; out.command=...; out.sends=...; co_return out;` とするだけです。

## 移行しなかった場合

v0.9.0以降では、従来の8個の仮想関数は、存在しません。

- 従来のC++の `override` は、`'execute' marked
  override but does not override` のようなコンパイルエラーになります。
- `execute()` だけを実装したPythonのノードは、`run(input)` を要求する `NotImplementedError` を送出します。

古いメソッド名を残したままにする、移行用のパターンは使わないでください。エンジンが呼び出すのは `run(NodeInput)` だけなので、古い本体が実行されることはありません。

## 一括で移行するスクリプトはありますか

ありません。仮想関数のシグネチャが8つの形に分かれているため、正規表現による変換は現実的ではありません。上のケース別の例（4つ）を読んで、手作業で移行してください。

最も多いパターン（`execute(state)` だけをオーバーライドしている場合）では、次のsed/awkのワンライナーが、最初の変換の助けになるかもしれません。ただし、人間によるレビューが必要です。

```bash
# Very rough initial pass — nodes with single-line execute override only.
# Always dry-run without -i first.
grep -lE 'execute\(const GraphState' src/**/*.cpp
# Manually edit each resulting file to the new pattern.
```

複雑なノード（`execute_full`、`execute_stream_async` など）は、手作業で編集する必要があります。近道はありません。

---

# マイグレーション2：型付きでロスのないProviderへの切り替え（再コンパイル必須）

これは、ソースとバイナリの両方の互換性を壊す変更です。すべてのC++コンシューマーとカスタムプロバイダーを、新しい対応するヘッダーとライブラリで再コンパイルしてください。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、ディスクリプターのインタープリター、ResponsesのWebSocket経路は削除されました。エイリアスも互換用のブリッジもありません。SDKはalphaの `0.4.0` で、インターフェースリビジョン7／共有ABI 7、capabilityのチェックはアウトオブラインです。これは、安定版リリースだという主張ではありません。

削除された `Provider::complete`、`complete_async`、`complete_stream`、`complete_stream_async` の呼び出し箇所は、モードを明示したリクエストと、`invoke` / `dispatch`（またはC++の非同期版）に置き換えてください。`Agent::complete` は、所有権を持つ結果を返す、1ターン用の別のAPIとして残ります。

CMake 3.20以降が必要です。`NEOGRAPH_BUILD_LLM=OFF` の場合も、Coreが所有権を持つ型付きのプロバイダー契約をエクスポートするため、SchemaProviderランタイムは必須です。明示した `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` が最優先されます。なければ、CMakeはインストール済みの `SchemaProvider` ランタイムパッケージを優先し、それもなければ（`NEOGRAPH_FETCH_SCHEMAPROVIDER=ON` がデフォルト）`cmake/NeoGraphSchemaProvider.cmake` で固定された、変更されないGitHubのarchiveをダウンロードします。オフラインでビルドする場合は、SDKをインストールし、`CMAKE_PREFIX_PATH` にそのプレフィックスを設定して、`-DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` を渡します。CMakeが、隣のチェックアウトを推測したり、削除された同梱のインタープリターを選んだりすることはありません。NeoGraphのオプションのHTTPモジュールを無効にしても、SDKランタイムはトランスポートの依存を必要とします。SDKランタイムは、WebAssemblyのビルドでは使えません。

Pythonも、C++と同じ、所有権を持つリクエストと結果の境界を公開しています。`make_provider_request`、`Provider.prepare`、`dispatch`、`invoke` です。プロバイダーの履歴には、型付きのパートを持つ `ProviderMessage` を使います。`ChatMessage` は、グラフ向けの簡易的な射影として残っています。SDKが返した失敗は、`ProviderOutcome.failure` から参照できます。ホストのオブザーバーや精算（settlement）で起きた例外は、`outcome` と `cause` を保持します。コンストラクターや、GILとコールバックの動作は、[Pythonバインディングガイド](python-binding.md)を参照してください。

公開されている契約は、所有権を持つ型付きの準備とディスパッチであり、対になったcompletion用の仮想メソッドではありません。`ProviderRequest.payload` は、Chat、Messages、Responses、Gemini、Interactionsのリクエストを表す、SDKのvariantです。`ProviderMode::Collect` / `Stream` は、オブザーバーとは独立に、トランスポートを選びます。`on_event` が受け取るのは、借用された、型付きの `sp::Event` のビューです。コールバックの後に必要なデータだけを、コピーしてください。生のJSONによる上書きや、ポータブルな射影を通じたネイティブ状態の取り込みは、受け付けられません。

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

残されている呼び出しごとのコントロールは、`extra_fields` の辞書ではなく、型付きのフィールドです。

| ファミリー | 追加される `ProviderControls` |
|---|---|
| Chat | `chat_reasoning`、`include_reasoning`、`usage_include`、`models`。これらには、宣言済みのOpenRouterのoriginが必要です。スカラーの `reasoning_effort` は別です。 |
| Responses | `previous_response_id`、`previous_response_history`、`parallel_tool_calls`、`verbosity`、`truncation`、`responses_include`。 |
| Messages | `thinking_mode`、`output_effort`、`cache_control`、`messages_tool_choice`、宣言済みのoriginのOpenRouterの `provider` ルーティング。 |
| Gemini Generate | `gemini_history_mode`、`gemini_thinking_level`、`temperature`、`safety_settings`、`gemini_tool_choice`。 |

Messagesの手動のthinkingには、ポリシーでadmissionされた、出力上限を下回るbudgetが必要です。adaptiveとdisabledのモードでは、thinkingのbudgetを指定できません。手動またはadaptiveのthinkingでは、有効なtemperatureが省かれます。ただし、不正な値や、モデルで禁止されているtemperatureは、省かれる前に拒否されます。disabledのthinkingでは、admissionされたtemperatureが送出されます。モデルの接頭辞による制限は、ポリシーとしてadmissionされた事実で、モデル名全体、または最後の `/` より後ろの部分に対して、ASCIIの大文字小文字を区別せずに照合されます。Geminiのthinking levelとthinking budgetは、同時に指定できません。型付きのtool choiceと `required_tool` も、同時に指定できません。サポートされていないファミリー、origin、値の組み合わせは、I/Oの前に拒否されます。

### PreparedProviderRequest / ProviderBudgetClaim
`prepare()` は、検証とエンコードをちょうど1回だけ行い、元のデッドラインとキャンセルの状態を持つ、ムーブ専用の `PreparedProviderRequest` を作ります。永続的な処理を行う呼び出し側は、その `Provider::request_digest()` を、組み立てた内容に結び付け、admission済みの予算の請求枠を予約し、ディスパッチのレシートを書き込んでから、同じハンドルを `ControlledProvider::dispatch_prepared(_async)` で消費します。ゲートを通った後で、リクエストを組み立て直してはいけません。重複したレシートがあっても、再ディスパッチされることはありません。カスタムプロバイダーは、`prepare_runtime()` または `prepare_local()` を使って、`get_name()`、`family()`、`prepare()` を実装します。ローカルのコールバックは、`this` ではなく、所有する共有状態をキャプチャしてください。

省略可能な `ProviderControls` は、呼び出し側の選択であり、必須のデフォルト値でも、黙って切り詰められる上限でもありません。ファミリーがサポートしていないコントロールは、ディスパッチの前に失敗します。上限付きの呼び出しには、admission済みの、本物のモデルの入力と出力に関する事実が必要です。事実が欠けている場合は `LimitUnknown` で失敗します。予約は、保守的に見積もった支出の権限です。報告された使用量でも、予測でも、請求書でもありません。不明、部分的、配送不明の結果は、ホールドを保持します。本物の最終レポートがあれば、上限を超えた使用量も含めて、そのホールドを精算します。リトライは、明示的な1つの層で、デフォルトではオフです。時間枠に上限があり、以前の結果が不明な場合はホールドを保持します。隠れた再送はありません。


プロバイダー呼び出しが返すのは `sp::runtime::Result` です。これは、変更できない、所有権付きの `std::shared_ptr<const sp::Outcome>` で、`sp::Completion` または `sp::Failure` を含みます。表示用のテキストだけでなく、結果の全体を保持してください。順序付きのメッセージとパート、保持されているネイティブな継続とファミリーが提供するワイヤーのエビデンス、順序付きのrawの観測、停止のエビデンス、本物の試行メタデータは、呼び出しが終わった後も、クライアントが破棄された後も残ります。`input_total`、`output_total`、`total` などの使用量カウンターは、`std::optional<sp::Count>` です。値がある場合は、`uint64_t value` と `Evidence` を持ちます。`Usage` には、ステージ、品質、競合（conflict）も記録されます。値がないのは「不明」を意味し、ゼロとして補うことはありません。失敗は、元の部分的な結果を保持します。`ProviderFailure::outcome()` と `ProviderObserverError::outcome()` は、その結果を保持します。後者は、オブザーバーの例外も `cause()` に保持します。

ワイヤーのエビデンスは、ファミリーが提供する省略可能なものです。`sp::Completion::wire_envelope` はnullになることがあります（Pythonでは `ProviderCompletion.wire_envelope` が `None`）。バッファ型のChatは、現在、レスポンス全体のJSONを `raw_events` に `RawWire` として保持します。`type == "chat.completion"` で、文書は `payload` にあります。`wire_envelope` はnullのままです。エビデンスは、ファミリーが保持している場所から読んでください。代わりのエンベロープが捏造されることはありません。ネイティブな継続とrawバッファは、保護されたエビデンスのままで、トレースのペイロードには含まれません。

`UsageAccumulator::snapshot()` は、累積したレポートを返します。`total_tokens_wide()` が返すのは、計上済みのトークン数と、未解決の予約の合計です。報告された使用量として表示してはいけません。精算には、入力と出力の件数を含む、最終的で整合したレポートが必要です。精算では、裏付けのある最大の合計を計上し、上限を超えた使用量でも切り詰めません。累積したレポートのどれか1つでもカウンターが欠けていれば、集計結果でもそのカウンターは不明のままです。予約、ローカルでの計上、ベンダーの請求書は、それぞれ別の記録です。

本物の結果が存在するのに、効果の後の会計処理や、最終レシートの永続化が失敗した場合、`ProviderDispatchOutcomePersistenceError` は、元の変更できない結果を `outcome()` に、元の永続化の例外を `cause()` に保持します。配送にも失敗していた場合は、`delivery_error()` が元のオブザーバーの例外を保持します。永続化に成功した後でオブザーバーが失敗した場合は、その元のオブザーバーの例外が、そのまま再送出されます。不明な、または結果のないトランスポートの失敗では、結果が捏造されることはありません。

### SchemaProvider

`SchemaProvider` は、admission済みの `sp::descriptor::ValidatedDescriptor`、`sp::runtime::Options`、省略可能な `SchemaProvider::Defaults` を受け取ります。ディスクリプターの読み込みは、closedでバージョン付きのデータのadmissionです。リクエストとレスポンスのインタープリターでも、任意のプリミティブのレジストリでもありません。認証情報は、公開されるディスクリプターのファイルではなく、ランタイムオプションに置きます。Defaultsに含まれるのは、型付きのOpenRouterルーティングと、Responsesの保持（`responses_store`）だけです。後者が有効なのは、Responsesに対してだけです。ホスト型のOpenRouterのルーティング、保持、JSONフォーマットは、引き続き、宣言する型付きのコントロールです。Images、Veo、Decisionsは、NeoGraphの別の型付きクライアントと、別の認可を使います。SDKのchatのグラントを引き継ぐことはありません。

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

通常の `sp::descriptor::load` は、ヘッダーをリテラルとして扱います。`${VAR}` は展開されません。admissionの前に、ホスト側で明示的に前処理したい場合は、`sp::descriptor::load_with_environment_headers(source, overrides, policy)` を使うか、決定的な `load_with_deployment_headers(source, overrides, environment, policy)` を `DeploymentHeaderEnvironment` とともに使います。環境変数を読むヘルパーは、Messages用に、省略可能な `ANTHROPIC_WORKSPACE_ID` / `ANTHROPIC_BETA` を読みます。未設定または空の値は省かれます。ディスクリプターにリテラルで書いたヘッダーは環境変数の値より優先され、明示的なoverridesはその両方に優先します。いずれも大文字小文字を区別しません。overridesの重複、不正なヘッダー、予約されたヘッダーは、最終的なadmissionで失敗します。エンコーダーがテンプレートを評価することも、admission済みのディスクリプターを変更することもありません。

### ネイティブ履歴

チェックポイントがプロバイダーのネイティブな履歴（推論内容、署名、順序付きのツール呼び出し）を保持するのは、それを生成したプロセス内の C++ のサイドカーとしてだけです。`ChatMessage`、`ChatTool`、JSON は持ち運び用のコピーで、ネイティブな権限を持ちません。再起動後もネイティブな履歴を保持する必要があるホストは、`sp::NativeArchive` を設定します。これらの契約は[型付きプロバイダーのリファレンス](reference-en.md#portable-projections)で説明しています。

関連ドキュメント：
- [ABI_POLICY.md](ABI_POLICY.md)
- [ASYNC_GUIDE.md](ASYNC_GUIDE.md)

---

# マイグレーション3：`compile()` のワーカープールのデフォルトが1に（v0.1.4のリグレッションの修正）

## 変更内容

`GraphEngine::compile(def, ctx)` のデフォルトのワーカー数は、v0.1.4（`b59444f`）から `std::thread::hardware_concurrency()` でしたが、現在のv1より前のAPIでは、**`1`（エンジンが所有するthread_poolなし）**に戻されました。

## 理由

`hardware_concurrency` をデフォルトにすると、すべてのfan-outノードに、スレッド間のsubmitのオーバーヘッド（1タスクあたり約6〜7 µs）が加わります。benchのparの測定（5ワーカーとsummarizer）は、11.6 µsから44 µsへ、4倍遅くなりました。

実際の本番のワークロード（ミリ秒から秒の範囲のLLM呼び出し）では、submitのオーバーヘッドは無視できます。しかし、次の問題があります。
- **単純なグラフ（fan-outなし）**でも、プールのオーバーヘッドを払うことになり、無意味です
- **スレッドセーフでないノードの状態**が、デフォルトでマルチワーカーにさらされます。実際に踏みやすい落とし穴です

そのため、デフォルトは安全側の1にし、fan-outを実際に並列化したい場合は、利用者が明示的にオプトインする方式にしました。

## 移行

fan-outの並列化が必要なグラフ（たとえば、複数の `Send` のディスパッチ、`parallel_group`、deep_researchの5 researcherのfan-out）では、`compile()` の後に、明示的に次を呼ぶ必要があります。

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

単純なグラフ（fan-outなし）や、軽いfan-outのグラフ（LLM呼び出しが支配的）は、デフォルトのままにします。プールのオーバーヘッドはゼロです。

## 移行しなかった場合

ワーカープールをオプトインしない場合、CPUを使うfan-outは、呼び出し側のexecutorで実行されます。非同期I/Oは、それでも重なって進められます。別々の実行スレッドが必要な場合は、`set_worker_count_auto()` か、明示的なワーカー数を使ってください。ワーカー数だけでは、一貫性も高速化も保証されません。

---

# マイグレーション4：C++のABIと、必須の再ビルド

NeoGraphは、コンパイルされるすべての公開ライブラリに、プロジェクトの `VERSION` と、メジャー番号の `SOVERSION` を割り当てるようになりました。v1より前のリリースは、すべてABI世代0を使いますが、`0.x` のバイナリ互換性は保証されません。変更履歴（changelog）が境界を告知したときは、すべてのC++コンシューマーを再ビルドしてください。特に、`0.11.1` 以前から、上限付きの `NodeCache` を含むリリースへ移行する場合は、`NodeCache` と `EngineConfig` のオブジェクトレイアウトが変わったため、再ビルドが必要です。

型付きのProviderへの移行で、仮想関数の契約は `get_name`、`family`、`prepare` に変わり、古いcompletion用の仮想関数は削除されました。既存のProviderのバイナリには互換性がありません。カスタムプロバイダーと、それに依存するすべてのC++コンシューマーを、対応するヘッダーとライブラリで再ビルドしてください。CoreもSchemaProviderの型を公開するため、LLMを無効にしたビルドでも、SDKランタイムが必要です。将来の安定したインターフェースでは、安定したレイアウトを変更するのではなく、別々のcapabilityインターフェースとアダプターを使うほうが望ましいです。現在のv1より前のインターフェースは、バイナリ互換性を約束しません。

プラットフォームごとのライブラリ名、既知の再ビルドの境界すべて、CIでの検証については、[バイナリ互換性ポリシー](ABI_POLICY.md)を参照してください。
