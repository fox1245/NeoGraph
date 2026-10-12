<!-- neograph-i18n: source=docs/migration-v0.4-to-v1.0.md locale=ja source_sha256=37d0912e7f5f8a94f97a67c362602b9b38015ab141aa56aac51c33c118750d4c -->
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

これは、ソースとバイナリの両方の互換性を壊す変更です。すべてのC++コンシューマーとカスタムプロバイダーを、新しい対応するヘッダーとライブラリで再コンパイルしてください。`CompletionParams`、`ChatCompletion`、`CompletionProvider`、`OpenAIProvider`、`RateLimitedProvider`、`SchemaPrimitiveRegistry`、ディスクリプターのインタープリター、ResponsesのWebSocket経路は削除されました。エイリアスも互換用のブリッジもありません。SDKはalphaの `0.1.1` で、インターフェースリビジョン4／共有ABI 4、capabilityのチェックはアウトオブラインです。これは、安定版リリースだという主張ではありません。記録されている、インターフェース3でのSDKランタイムとarchiveの適格性確認は、Linux/POSIXを対象としていて、インターフェース4の適格性確認にはなりません。Windows NTFSとmacOSの実装はありますが、新しいプラットフォームでの適格性確認には、ランタイムでのエビデンスが必要です。WASMでのプロバイダーランタイムは、適格性確認されていません。

削除された `Provider::complete`、`complete_async`、`complete_stream`、`complete_stream_async` の呼び出し箇所は、モードを明示したリクエストと、`invoke` / `dispatch`（またはC++の非同期版）に置き換えてください。`Agent::complete` は、所有権を持つ結果を返す、1ターン用の別のAPIとして残ります。

CMake 3.20以降が必要です。`NEOGRAPH_BUILD_LLM=OFF` の場合も、Coreが所有権を持つ型付きのプロバイダー契約をエクスポートするため、SchemaProviderランタイムは必須です。明示した `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` が最優先されます。なければ、CMakeはインストール済みの `SchemaProvider` ランタイムパッケージを優先し、それもなければ（`NEOGRAPH_FETCH_SCHEMAPROVIDER=ON` がデフォルト）`cmake/NeoGraphSchemaProvider.cmake` で固定された、変更されないGitHubのarchiveをダウンロードします。オフラインでビルドする場合は、SDKをインストールし、`CMAKE_PREFIX_PATH` にそのプレフィックスを設定して、`-DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF` を渡します。CMakeが、隣のチェックアウトを推測したり、削除された同梱のインタープリターを選んだりすることはありません。NeoGraphのオプションのHTTPモジュールを無効にしても、SDKランタイムはトランスポートの依存を必要とします。記録されているSDKのランタイムとarchiveの適格性確認は、Linux/POSIXを対象としています。Windows NTFSとmacOSの実装はありますが、新しいプラットフォームでの適格性確認には、ランタイムでのエビデンスが必要です。WASMでのプロバイダーランタイムは、適格性確認されていません。

Pythonも、C++と同じ、所有権を持つリクエストと結果の境界を公開しています。`make_provider_request`、`Provider.prepare`、`dispatch`、`invoke` です。プロバイダーの履歴には、型付きのパートを持つ `ProviderMessage` を使います。`ChatMessage` は、グラフ向けの簡易的な射影として残っています。SDKが返した失敗は、`ProviderOutcome.failure` から参照できます。ホストのオブザーバーや精算（settlement）で起きた例外は、`outcome` と `cause` を保持します。コンストラクターや、GILとコールバックの動作は、[Pythonバインディングガイド](python-binding.md)を参照してください。

インターフェース4より前に、インストール済みの find_package による Program C++／C ABI／dualQuickJS のコンシューマーと、NeoGraph／SchemaProviderの型付きのコンシューマー（2リクエストでの寿命、ネイティブ、raw、不一致の確認）が通りました。これらは過去のパッケージの結果であり、インターフェース4の合格の主張ではありません。より広いプラットフォームや、安定版リリースも主張していません。

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

インターフェース4は、残されている呼び出しごとのコントロールを、`extra_fields` の辞書ではなく、型付きのフィールドで保持します。

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

### ネイティブ履歴と予算

`ChatMessage` / `ChatTool` とJSONは、持ち運び用の射影（portable projection）であり、ネイティブな権限（native authority）を持ちません。持ち運び用のフォーマットは、これまでどおり[`provider-message-v2`](../schemas/provider-message-v2.schema.json)と[`runtime-history-record-v2`](../schemas/runtime-history-record-v2.schema.json)です。本物のC++のチェックポイントのサイドカーは、メモリ上でネイティブシール（native seal）を保持します。ネイティブな履歴を永続化するには、ホストが所有する `sp::NativeArchive` が必要です。これは閉じたv3形式（`spna3`）で、認証付きのオーナー専用の保管（custody）と、独立した鍵を使います。archive v2は拒否します。アップグレードも解釈もしません。認証が結び付けるのは、セマンティックなディスクリプターの選択内容すべて（origin、パス、ヘッダー、ポリシー、リクエストのフィールドマッピング、usageのパス、stopのマッピング）と、オーナー、正確な保管の対応付けです。暗号化ではなく、ベンダー（発行元）の認証でもありません。archiveの本体、鍵、ネイティブなblob、生のワイヤー観測データは、公開しないでください。archiveはエビデンスの保管場所であり、金銭の付与（grant）でも、支出のリース（spending lease）でもありません。Programと外部のバンクは、これまでどおり独立したジャーナルが所有します。スナップショットをコピーしても、クレジットは作れません。

インターフェース4で、ネイティブリプレイの設定ダイジェストから外れるのは、出力生成の上限だけです。コンテンツ、origin、ルート、ポリシーのID、先頭部分、ツール、reasoningのコントロールは、引き続き結び付けられています。実際の上限は、エンコードされたリクエストと、準備済みリクエストのダイジェストに、引き続き入ります。上限を引き上げたセマンティックな呼び出しには、そのたびに、新しいリソースバンクのadmission、固有の決定的な呼び出し序数と効果のID、元のデッドラインが必要です。精算済みの呼び出しスロットを再利用することも、クレジットを更新することも、シールを修復することも、不確かな効果を再送することもできません。archive v3とポータブルJSON v2は変わりません。古いポリシーのネイティブarchiveは、そのポリシーに結び付いたままで、ポリシーが一致しないと、移行されずに拒否されます。

同じルートでのネイティブな継続と、明示的な外部への射影は、契約が異なります。Geminiのデフォルトは `sp::gemini::HistoryMode::NativeOnly` です。`PortableForeign` は、呼び出し側が作成したアシスタントのText / ToolCallの履歴を、ネイティブシール、ワイヤー出力、署名なしで受け付けます。`skip_thought_signature_validator` が付くのは、外部のアシスタントのターンごとの、最初の関数呼び出しだけです。テキストだけの履歴には、署名が付きません。本物のネイティブグループは、引き続き厳密に検証され、失敗したシールや不一致のシールが、取り除かれたり、ポータブルな履歴に格下げされたりすることはありません。Responsesの `previous_response_id` は、プロバイダー側が保持する状態を指定します。`messages` には新しい入力だけを送ります。`previous_response_history` は、ローカルの所有権のエビデンスで、送信されることはありません。クライアント側ツールの所有権のためにこれが必要な場合は、元の先頭部分全体と、IDがカーソルと等しい本物の終端アシスタントを渡します。その後にプロセス内で得たカーソルの結果は、非公開の完了済みの所有権を保持しますが、完全な `NativeReplay` やarchiveの権限は持ちません。カーソルは、ネイティブarchiveでも、ポータブルな取り込みの許可でもありません。

**単独バンクのジャーナルの訂正：現在の契約を改訂しました。実際に動かして得た実行時のエビデンスは後述します。** オーナーが承認したプロトコルでは、次の2点が必要です。1つは、単調増加する、信頼されたストアの名前空間に対する義務（namespace obligation）です。もう1つは、実在する不変の元のオーナー／スレッド／グラフのスコープ、上限（ceiling）、デッドライン／クロックの識別子、世代（generation）です。ホストが所有する不透明なリース（opaque lease）を発行できるのは、チェックポイント全体のコミットメントとリビジョンに対する、永続的なヘッドの正確なCASだけです。保留中のエフェクトのウィンドウは、プロバイダーI/Oの前に、正確に永続化しなければなりません。精算には、本物のSDKの結果と、実際の課金額、nullを許すレポート、ホールド、重複排除用のIDを使います。チェックポイントと次のヘッドは、同じ所有アクター／リビジョンのもとでアトミックに公開します。バンクのメタデータを削除する、チェックポイントを間引く、認証済みの古いスナップショットをリプレイする、同じIDを上書きする、アクターを失う、といった操作でクレジットが付与されてはいけません。上限を130から129に絞ったとき、すでに65のホールドがあると、さらに65を受け入れることはできません。エフェクトがなかったと証明された失敗であれば、変更のないヘッドを解放できるため、本物の130での復旧は引き続き可能です。クラッシュ、不明、リース喪失のウィンドウは、返金も再試行もフォールバックもせず、ホールドしたままにします。plain／pristineなarchive設定は、金銭もネイティブな支出リースも与えません。現在の `config.usage` で、既存の単独の義務を置き換えることもできません。Programや外部バンクのジャーナルの所有関係は変わりません。以上は要求される契約です。実際の通貨と保管のエビデンス、および計測上の制約は後述します。安定版として公開済みのAPIを保証するものではありません。

**現在の宣言。統合した実行時のエビデンスは後述します。** `<neograph/graph/checkpoint.h>` は、`owner_scope`、論理的な `thread_id`、バックエンド内部用の `storage_thread_id`、`graph_identity`、`original_ceiling`、`original_deadline_ticks`、`deadline_clock_identity` を持つ `ManagedBudgetLeaseScope` を宣言しています。`OwnedManagedBudgetLease` が公開するのは、読み取り専用の `scope()`、`actor_id()`、不変の `bank_generation()`、`revision()`、`head_checkpoint_id()`、`head_commitment()` です。権限をインポートする公開コンストラクターはありません。`ManagedBudgetEffectReceipt` は `active()`、`effect_id()`、`claim_amount()`、`request_digest()` を公開し、デフォルトのレシートは何の権限も与えません。`CheckpointStore` は、`acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment)`、`begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest)`、`settle_managed_budget_effect(lease, effect, genuine_outcome, authority)`、`publish_managed_budget_checkpoint(lease, checkpoint)`、`release_managed_budget_lease(lease)` と、それぞれの `_async` 版を宣言しています。同期版の `CheckpointStoreCore` と `AsyncCheckpointStore` は、それぞれの版を公開します。`managed_budget_checkpoint_commitment(checkpoint)` が対象とするのは、バンクのJSONだけでなく、永続化されるチェックポイント全体です。これらの宣言だけでは、バックエンドのCAS、通貨の安全性、インストール済みABIの互換性、実行時の経路が実際に成功することは、いずれも確認できません。

**本物のInMemoryの共有バンクフォーク：維持しており、動作も確認済みです。** 元からある本物のC++のフォークは、元の1つの会計ジャーナルと、信頼された現在のブランチヘッドを使います。グラントを複製することはありません。`publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)`（と `_async` 版）には、本物の現在のコピー元とそのコミットメント全体、および同じバンクのネイティブC++ポインターの実体が必要です。永続的な単独フォークは、引き続き明示的に未対応です。`OwnedManagedBudgetLease::scope()` と、元のオーナー／スレッド／グラフ、上限、デッドライン／クロック、世代は不変のままです。ストアが発行する読み取り専用の `execution_thread_id()` / `execution_storage_thread_id()` は、実行するブランチを別に選びます。`GraphState::budget_original_thread_id()` は、元の会計バンクを示します。選択したブランチのヘッドに対する正確なCASと、グローバルなアクター／リビジョンにより、標準の現在のカウンター、保留中のエフェクト、使用済み（burned）のIDに対して、すべてのブランチが直列化されます。元のブランチもフォークしたブランチも、補充なしで使い続けられます。古いスナップショット、チェックポイントのコピー、インポートしたJSONは、エイリアスを作ることも、ヘッドを巻き戻すこともできません。元のroot30 → charge3 → original continuation6 → fork lower20 → continuation9という同一バンクの証明は、変更していない test_graph_engine.cpp:810–913 でPASSしました。保存された元の上限30は、フォーク側の有効な上限20とは別です。31への拡大と、JSONだけでの復元は、拒否されなければなりません。上限のない報告済みの観測値は事実を示すデータであり、有限のグラントではありません。変更のないヘッドを解放できるのは、エフェクトがゼロだと証明されたリースだけです。不明または保留中のエフェクトがあると、義務は保持されます。

**現在のrelease-errorの契約：実際に動かしたスイートとプローブは後述します。** `<neograph/graph/engine.h>` の `graph::ManagedBudgetLeaseReleaseError` は、`ProviderOutcomeError` から派生します。`cause()` は元の実行時例外を保持し、`release_error()` は二次的な、永続的なリース処理の失敗を返します。`outcome()` は、本物のSDKのエビデンスがあれば保持し、SDKの結果がなければnullです。releaseに失敗しても、結果を作り上げたり、再dispatchを許可したりすることはありません。閉じたメタデータ `_neograph_managed_budget_scope` には、元の論理スコープ／上限／デッドラインのクロック／世代が書かれていますが、これはデータであり、バックエンドのCASによる権限ではありません。

**archiveのオーナーと保持（retention）の契約：実際に動かしたスイートとプローブは後述します。** 省略された元のオーナーを、実際に設定されている `sp::NativeArchive::owner_scope()` から引き継ぐのは、有限の単独ルート、または認証済みの有限のコピー元だけです。上限のない場合とplainな場合のオーナーメタデータの意味は変わりません。archiveのオーナーが明示的に食い違っている場合は、リースを取得する前に拒否します。`CheckpointStore::retains_native_checkpoint() const noexcept` と、それに対応するCore／Asyncのストレージ機能は、デフォルトでfalseです。実際のInMemoryバックエンドはtrueにオーバーライドし、ラッパーは実際の保持状況を委譲しなければなりません。この読み取り専用の記述により、正当な、リースなし／plain／上限なしのC++のネイティブチェックポイントの保管が許されます。ただし、支出クレジットもネイティブリプレイの権限も与えません。リース付きの保管には、JSONのフラグや推測したストア型ではなく、ストアが実際に発行したレシートを使います。

**ネイティブ保管のI/O前ゲート：実際に動かしたスイートとプローブは後述します。** 管理対象のエフェクトを開始するには、保留中のエフェクト、スロット、ホールドしたウィンドウを変更する前に、本物の結び付けを持つNativeArchive、または実際のローカルでストアが発行したプライベートなC++の保持機能（capability）が必要です。このプライベートな機能は、JSONからインポートされることも、ワイヤー上で転送されることもありません。C++のサイドカーは境界を越えられないため、リモートのバックエンドがInMemoryであっても、gRPCにはクライアントとサーバー双方の実際のarchiveが必要です。archiveが有限のコピー元オーナーを提供しない場合、元の匿名のオーナースコープは空のままです。実際のarchiveの結び付けは、元のスコープと一致しなければなりません。財務上のヘッドとリースのエビデンスだけでは、ネイティブ保管の準備ができているとは証明できません。

診断用のJSONは、構文上は有効な、キーが重複した文書も含めて、元のrawバイト列を保ちます。実行可能なリクエストと設定のadmissionでは、引き続き重複が拒否されます。元の2xx以外のレスポンスのJSONは、2回目の、情報が失われるパースをせずに、`http.error` のエビデンスに残ります。名前付きのSSEエラーは、その後の通常のストリームの終了よりも優先されます。診断用やプロバイダーのメタデータは、admission済みの元の範囲で制限され、無関係な小さなエラーテキストの上限では制限されません。

`ProviderRequest::observer_limits` は、ホスト専用です。明示的に指定する `max_events` と `max_bytes` は正の値でなければならず、admission済みのSDKの配送上限を、下げる方向にしか変えられません。`provider-request/v3` は、有効な上限、モード、エンコードされた本体、リトライのポリシー、セマンティックなディスクリプターのバインディングのすべてを、ダイジェストの対象にします。ブリッジは、キューに入っているバッチとdrain中のバッチの両方について、PMRのvectorやmapの実際の容量に、所有するイベントや文書のバイト数を加えて計上します。キャンセルは、キューのミューテックスの外で要求されます。`messages` という名前の汎用チャネルが、chatとして扱われることはありません。ネイティブな `history` チャネルを `messages` にマッピングすると、そのC++サイドカーが保持されます。JSONからネイティブの権限が作り出されることはありません。

`ProviderOutcomeError` は、結果を保持するホストエラーの共通の基底です。`ProviderObserverError` と `ProviderDispatchOutcomePersistenceError` は、drain済みのSDKの結果の全体と、元の `cause()` を保持します。永続化のエラーは、二次的なオブザーバーの失敗も `delivery_error()` に保持します。`ProviderFailure::outcome()` は、SDKの失敗そのものを保持します。これらはエビデンスであり、NodeやProgramが再ディスパッチしてよいという許可ではありません。トランスポートのリトライは、SDKだけが担当します。呼び出し側が選んだ `max_output_tokens` が、黙って切り詰められることもありません。

`ProgramFailure` は、生きている `provider_outcome` と `provider_cause` を保持します。正規の事実としてのSDKwitness（canonical factual SDK witness）は、本物のarchiveの保管を、オーナー／run／バージョン／バンドル／オペレーション／試行に結び付けます。Runtimeは、復旧した失敗を公開する前に、設定済みの保管を先に復元します。公開されているデータのみの `ProgramResult::create()` は、あらかじめ入力したwitnessでこれを迂回することはできません。未解決のパース済みシールは、実行可能な結果ではありません。プロセスの再起動後は、元の例外ポインターが存在せず（`provider_cause == nullptr`）、テキストから再作成することもありません。永続化できない失敗は、シリアライズも公開もリプレイもできません。

`RecordedBindingSet` は、ソースに結び付けられた、ムーブ専用のデータです。呼び出し側が渡すdispatcherではありません。信頼されたCatalogの `recorded_capability_binder` が、実際に永続化されたソースイベントから、取得済み（captured）の機能だけを独立して実体化します。`ProgramRuntime::replay_recorded()` は、元の選択済みソースの権限を確認したうえで、実際に残っているバンクを永続的なCASで移します。引き継いだ支出は、新しいモデルのグラントではありません。以前の `start_recorded` による更新APIは削除しました。InMemory、File、SQLite、PostgreSQLの各Programストアは、実行の間じゅう、正確で不変な、所有されたリースを保持します。期限が切れても更新されません。制御されたJavaScriptも、基になる機能マニフェストを検証し、正確な完了済みコマンドの結果を消費します。外部のエフェクトを再dispatchすることはありません。

**Recorded-controlの因果的な修正：フルスイートで動作確認済み。** capturedしたコマンドのリプレイは、実行前に、新しいCPUの実時間（wall-time）とCoreの処理だけを、永続的に予約します。そして、測定した処理量と、新しく生成されたCoreチェックポイントを、結果のCASで公開します。新しいモデル、金銭、Programオペレーションの許容量は消費せず、capturedした外部エフェクトを再dispatchすることもありません。精算されていない予約は、引き落とされたままです。予約により、通常のRunning→Runningの遷移ではなく、認証済みの精算遷移が選ばれます。通常の遷移では、最初の新しいCoreチェックポイントが拒否されていました。awaitのチャネル受信、タイマーの待機／キャンセル、ハンドオフ待機の開始と解放は、それぞれを所有するexecutorとstrand上で直列化されます。既存のRecorded CPU／Memoryのawait／handoffシナリオは、フルスイートで通りました。リモートのTSanの対象範囲の限界は、後述のとおり明記します。

以下の観測結果は、このドキュメントを整理する前に記録したものです。過去のエビデンスであり、新たに実行したテストの結果でも、すべてのプラットフォーム・トランスポート・セキュリティ特性についての保証でもありません。

**有料の観測は完了しています。これは、あらゆる条件での適格性確認（qualification）ではありません。** 元の `SPQUAL1` の基本枠630/1000000 microUSDは変わりません。ハッシュで連結された1つの `A` が、承認済みの拡張枠480/3000000を、同じ元の台帳の中に受け入れています。合計は1110/4000000で、呼び出し回数、支出、ホールド、精算は累積されており、新しいグラントID、ヘッダー、リセットはありません。宣言の正確なバイト列とファイルの同一性、および元の認可、ベースライン、カタログ、アクティベーション、台帳プレフィックスのハッシュと合計値は、固定されたままです。削除、置換、変更を行うと、フェイルクローズになります。最終的な正規の台帳は、呼び出し1110回、支出437958、ホールド1287828（いずれもmicroUSD）、イベントA1、上限1110/4000000です。支出とホールドの合計US$1.725786は、ローカルのカタログ上のメーターの値であり、請求書の金額ではありません。文書化済みの5ファミリー×60ペアのベースラインは、600リクエストを完了しました。内訳は、Chat 60/60、Responses 60/60、Messages 60/60、Generate 56/60（不正解のvision SSEが4件）、Interactions 57/60（不正解のvisionが、バッファ型で1件、SSEで2件）です。合計は293/300ペアで、300/300ではありません。ほかの旧600件の会計記録は保存してありますが、挙動を完全に証明するものではありません。以前のM5／メディアのワンショットのコホートは変わりません。以前の3ラウンドのGoogleの前提条件には、無効なツール2件と、読み取れないpositive 1件の失敗が残っています。これ以上の有料呼び出しは承認されていません。最終的なSDKのエビデンスとネイティブ軸の制約は、ベースラインの成功とは別のものです。以前のアクティベーションと再オープンのスモークテストは、2回再オープンした後の呼び出し610回、支出219159、ホールド751233として記録されています。SDKのメーター／カナリア／visionの4テストは、19.38秒で通過しました。これらは範囲を限定した以前のチェックポイントであり、最終的な台帳の合計ではありません。以前に検証したChat 60ペアのコホートは、実際の試行120件、UpperBoundの課金120件を保持しており、UnknownHoldはありません。

**ネイティブ軸の観測は、暗号学的な検証でも、ネイティブでの消費や同等性の確認でもありません。** Generateは、改変、欠落、重複を受け入れました。Interactionsは、隔離した本物のコピー元／positive control、1つのオーナーの署名の改変、thoughtキャリアの欠落、callキャリアの欠落、重複を受け入れました。thoughtと署名をすべて削除すると、汎用の400が返りました。THOUGHT項目を残したまま署名フィールドをすべて削除した場合も、汎用の400でした。最後のキャプチャには、ローカルでエンコードした元データを保持するコントロールがありましたが、同じキャプチャでのサーバー側のpositiveではありません。以前のpositiveのコホートは、本物のままです。これらの観測が確立するのは、キャリアがまとめて欠けた場合の境界だけです。発行元や署名の検証、ベンダー側での消費は確立しません。実際のレポートは、SDKの `config/qualification-extension-results.json`、`qualification-final-summary.json`、`qualification-native-axis-results.json`、`qualification-combined-omission-results.json`、`qualification-signature-presence-results.json` です。前提条件の失敗、未実行、ネガティブで結論が出ない、といった状態は、事実のまま残しています。thoughtだけ、またはキャリアだけを欠落させた場合は、別のキャリアが残っている限り受け入れられました。これは、発行元の検証やネイティブでの消費についての主張を強めるものではありません。

**実際の統合的な検証結果と、残る制約。** 直近のCoreのフル実行は、2242テスト、失敗0件、スキップ16件（RAMでのプロセス喪失のケース14件は対象外、実際の認証情報が必要なゲート2件）で、130.17秒でした。`PgNestedJsonRoundTrips` は、重複キー、順序、nullのメタデータ、blob、残余（residual）を正確に保ったまま、0.18秒で通りました。変更のない元の共有バンクフォークと、既存のRecorded CPU／Memoryのawait／handoffシナリオも通りました。ラップした実際のMemory／SQLite／PostgreSQL／gRPCに対する、finite130／hold65／lower129／strip／old-head／pruning／no-archive／importのプローブは、通常ビルドとASan+UBSanの両方で通りました。ローカルのMemory／SQLite／PostgreSQLに対するTSanの範囲は、7件が通り、警告は0件でした。システムのAbseil／Protobufを含む、gRPC混在のフルTSanは、終了コード66で終わり、依存ライブラリと生成されたRPCのスタックに、レース警告が402件出ました。これは計装とカバレッジの限界であり、誤検知だと証明されたわけではありません。リモートのTSanによる検証や、レースがないことは主張せず、警告の抑制もしていません。インストール済みのfind_packageを使う、Program C++／C ABI／dualQuickJSの3つのコンシューマーは通りました。新たにインストールしたNeoGraph／SchemaProviderの型付きコンシューマーは、次の項目を通りました。実際のHTTPリクエスト2件、コルーチン開始前のプロバイダー破棄、ネイティブ／ツールのリプレイ、拒否（refusal）、known-zeroと生データの保持、実際のLinkedMismatchの拒否です。ブラウザーでのAlice/Bobの分離と、世代2への置き換えは、目視で確認しました。PostgreSQLのProgram Chatのブラックボックステスト6件は、18.989秒で通りました。直近のSDKは26/26が通り、失敗0件、74.07秒でした。最終的なReleaseGraphは、16構成×新規プロセス3回の繰り返し（48レコード）を38.29秒で完了し、失敗0件、実際のプロトコルとowned-outcomeのチェックがすべて通りました。NeoGraphの `benchmarks/provider-cutover-final-results.json` と `benchmarks/provider-cutover-final-summary.json` に、この最終コホートを個別に保存しています。測定中にコンパイラーや有料モデルは実行していません。過去のコホートは変更しておらず、セマンティクスやリソース使用量が同等だとは主張しません。不安定なSDK／ABI3は、安定版リリースでも、より広いプラットフォームでの適格性確認でもありません。

**最小限の有料エビデンス（2026-10-03）。広範な適格性確認ではありません。** 個別に承認された、1回限りの呼び出し3件の結果は、次のとおりです。Images：JPEG 1枚、1024×1024、360685バイト、入力／出力／合計のトークンは19/1408/1427で、目視で確認しました。Veo：MP4 1本、1280×720、4秒、437737バイト、生成1回とステータスのGET 3回、使用量はnull許容で、デコードしてChromiumで目視確認しました。Decisions：`typesafe/jev-1.13`、確率0.93、入力／出力のトークンは283/21、合計は不明、APIが報告したコストはUSD 0.000011886です。Imagesの基本料金USD 0.0336にテキストとthinkingを加えた額と、VeoのUSD 0.20は、カタログ上の想定であり、請求書ではありません。最小限のImagesのスモークテストでは、価格帯ごとの内訳は取得していません。どの結果も、1回限りの権限を更新せず、再実行を認めるものでもありません。

完了したchatのペアは、下流のベンダーがネイティブな継続を消費したことを証明するものではありません。

過去のStage 3（2026-04）の設計と、測定されたテスト件数は、履歴として残してあります。そのプロバイダー互換性やクロスオーバーに関する判断は、以下の、型付きでロスのない切り替えに置き換えられました。この設計の台帳は、現在のプロバイダーAPIではありません。

- [ABI_POLICY.md](ABI_POLICY.md)
- [ASYNC_GUIDE.md](ASYNC_GUIDE.md)
- [Issue #5](https://github.com/fox1245/NeoGraph/issues/5)：過去の決定。プロバイダー互換性のポリシーは、置き換えられました。

---

# マイグレーション3：`compile()` のワーカープールのデフォルトが1に（v0.1.4のリグレッションの修正）

## 変更内容

`GraphEngine::compile(def, ctx)` のデフォルトのワーカー数は、v0.1.4（`b59444f`）から `std::thread::hardware_concurrency()` でしたが、現在のv1より前のAPIでは、**`1`（エンジンが所有するthread_poolなし）**に戻されました。

## 理由

`hardware_concurrency` をデフォルトにすると、すべてのfan-outノードに、スレッド間のsubmitのオーバーヘッド（1タスクあたり約6〜7 µs）が加わります。benchのparの測定（5ワーカーとsummarizer）は、11.6 µsから44 µsへ、4倍遅くなりました。測定環境でbisectした結果、原因はv0.1.4の `b59444f` だと特定できました。

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

## 影響を受けたNeoGraph内部のサンプル

この変更とあわせて、fan-outを分かりやすくするパッチを入れました。意図を保つために、明示的な呼び出しを追加しています。利用者のコードが同じパターンに当てはまる場合は、同じ対応をしてください。

- `examples/10_send_command.cpp`：同期の `sleep_for` を使うResearcherNodeが、Sendでfan-outします。`engine->set_worker_count_auto()` を追加しました
- `examples/14_plan_executor.cpp`：5つのサブトピックへのSendによるfan-out（同期のsleep_for）。同じ追加をしました
- `examples/21_mcp_fanout.cpp`：3つのMCPツール呼び出しを同時に発行します。同じ追加をしました
- `examples/36_classifier_fanout.cpp`：すでに `set_worker_count(5)` を明示していました。誤ったデフォルトを書いていたコメントを修正しました（現在のデフォルトは1で、エンジンが所有するプールはありません）
- `src/core/deep_research_graph.cpp` の `create_deep_research_graph()` ビルダー：`compile()` の直後に `set_worker_count_auto()` を呼び、supervisorのN個のresearcherが、実際に並行して動くようにしました

`examples/05_parallel_fanout.cpp` は、`io_context` 上のコルーチンのタイマーの重なりを使っていて（同期のスリープはありません）、ワーカープールの効果がないため、変更していません。

利用者のコードに同じパターンがある場合は、次のようにします。

```cpp
auto engine = GraphEngine::compile(def, ctx);
engine->set_worker_count_auto();   // ← add this line
```

詳しい測定結果は、ROADMAP_v1.mdのperfセクションを参照してください（別途追加）。

> 過去のStage 3（2026-04）の設計と、測定されたテスト件数は、履歴として残してあります。そのプロバイダー互換性やクロスオーバーに関する判断は、以下の、型付きでロスのない切り替えに置き換えられました。この設計の台帳は、現在のプロバイダーAPIではありません。

---

# マイグレーション4：C++のABIと、必須の再ビルド

NeoGraphは、コンパイルされるすべての公開ライブラリに、プロジェクトの `VERSION` と、メジャー番号の `SOVERSION` を割り当てるようになりました。v1より前のリリースは、すべてABI世代0を使いますが、`0.x` のバイナリ互換性は保証されません。変更履歴（changelog）が境界を告知したときは、すべてのC++コンシューマーを再ビルドしてください。特に、`0.11.1` 以前から、上限付きの `NodeCache` を含むリリースへ移行する場合は、`NodeCache` と `EngineConfig` のオブジェクトレイアウトが変わったため、再ビルドが必要です。

型付きのProviderへの移行で、仮想関数の契約は `get_name`、`family`、`prepare` に変わり、古いcompletion用の仮想関数は削除されました。既存のProviderのバイナリには互換性がありません。カスタムプロバイダーと、それに依存するすべてのC++コンシューマーを、対応するヘッダーとライブラリで再ビルドしてください。CoreもSchemaProviderの型を公開するため、LLMを無効にしたビルドでも、SDKランタイムが必要です。将来の安定したインターフェースでは、安定したレイアウトを変更するのではなく、別々のcapabilityインターフェースとアダプターを使うほうが望ましいです。現在のv1より前のインターフェースは、バイナリ互換性を約束しません。

プラットフォームごとのライブラリ名、既知の再ビルドの境界すべて、CIでの検証については、[バイナリ互換性ポリシー](ABI_POLICY.md)を参照してください。
