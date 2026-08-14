<!-- neograph-i18n: source=docs/reference-en.md locale=ja source_sha256=9c7535abce2e7379b543aa224c27595799979906c32c59c01a2a6cabef43a4da -->
# NeoGraph API — ナラティブツアー
**Languages:** [English](reference-en.md) | [한국어](reference-ko.md) | [日本語](reference-ja.md) | [简体中文](reference-zh-CN.md)
この文書は NeoGraph の公開 API を順に案内する **ナラティブツアー** であり、
完全なリファレンスではありません。実際のエージェントを構築するときに出会う順に、
基礎型 → プロバイダー/ツールインターフェース → グラフ型 → エンジン →
チェックポイントストア → マルチ LLM → MCP の各モジュールを説明します。
以下の形は master HEAD に対して正確であり、
`include/neograph/` と照合済みですが、いくつかのモジュール
(`neograph::a2a`, `neograph::acp`, `neograph::async`,
`SqliteCheckpointStore`、`PostgresCheckpointStore`、
`RateLimitedProvider`、`NodeCache`、`AsyncTool`、`create_deep_research_graph`)
には、このツアーで扱っていない **ヘッダー上の公開 API があります**。
上記を含む全モジュールの型ごとの完全な API 一覧については、以下にリンクした
`include/neograph/` の公開ヘッダーを使用してください。このナラティブツアーが
推奨される入口であり、ヘッダーが正式なリファレンスです。
この分割により、ナラティブを最初から最後まで読める大きさに保ちながら、
詳細なリファレンスを `include/neograph/` の実装と一緒に維持できます。
**モジュール一覧:**
| モジュール | 名前空間 | 説明 | ツアー | ヘッダー |
|--------|-----------|-------------|------|---------|
| Core | `neograph` | 基礎型、Provider / Tool インターフェース | [§1–§3](#1-foundation-types) | [Provider](../include/neograph/provider.h) |
| Graph | `neograph::graph` | グラフエンジン、ノード、状態、チェックポイント、Store | [§4–§11](#4-graph-types) | [GraphEngine](../include/neograph/graph/engine.h) |
| LLM | `neograph::llm` | LLM プロバイダー実装と Agent | [§12](#12-llm-module) | [Agent](../include/neograph/llm/agent.h) |
| MCP | `neograph::mcp` | Model Context Protocol クライアント | [§13](#13-mcp-module) | [MCPClient](../include/neograph/mcp/client.h) |
| Util | `neograph::util` | 並行処理ユーティリティ | [§14](#14-util-module) | [RequestQueue](../include/neograph/util/request_queue.h) |
| **A2A** | `neograph::a2a` | Agent-to-Agent JSON-RPC ブリッジ (クライアント + サーバー + ストリーミング) | _ヘッダーのみ_ | [A2AClient](../include/neograph/a2a/client.h) |
| **ACP** | `neograph::acp` | Agent Client Protocol — stdio 上のエディター↔エージェント双方向 RPC | _ヘッダーのみ_ | [ACPServer](../include/neograph/acp/server.h) |
| **Async** | `neograph::async` | Asio の HTTP/SSE/WS ヘルパー、ConnPool、run_sync | _ヘッダーのみ_ | [WsClient](../include/neograph/async/ws_client.h) |
3 つの「_ヘッダーのみ_」行は、最近の監査とプロトコルブリッジ作業で新たに追加されたモジュールです。完全なヘッダーが
`include/neograph/{a2a,acp,async}/` の下にあり、ctest のテストスイートで使用されています。ただし、専用のナラティブ節の作成は
これらのヘッダーを参照する方針にして保留しています。規模が大きく、
(A2A だけでも約 5 クラス + types モジュール + caller node があります)、
新しいモジュールはあと 1〜2 リリースの間も発展し続ける傾向があるためです。
手書きツアーを保守する価値が出るのを待っています。
**便利なヘッダー:** `#include <neograph/neograph.h>` はコア API とグラフエンジン API 全体を含みます。
---
## 目次
- [1. 基礎型](#1-foundation-types)
  - [ToolCall](#toolcall)
  - [ChatMessage](#chatmessage)
  - [ChatTool](#chattool)
  - [ChatCompletion](#chatcompletion)
  - [ヘルパー関数](#helper-functions)
  - [ADL シリアライズ](#adl-serialization)
- [2. Provider インターフェース](#2-provider-interface)
  - [StreamCallback](#streamcallback)
  - [CompletionParams](#completionparams)
  - [Provider](#provider)
- [3. Tool インターフェース](#3-tool-interface)
  - [Tool](#tool)
- [4. グラフ型](#4-graph-types)
  - [ReducerType](#reducertype)
  - [ReducerFn](#reducerfn)
  - [Channel](#channel)
  - [ChannelWrite](#channelwrite)
  - [NodeInterrupt](#nodeinterrupt)
  - [Send](#send)
  - [Command](#command)
  - [RetryPolicy](#retrypolicy)
  - [StreamMode](#streammode)
  - [Edge](#edge)
  - [ConditionalEdge](#conditionaledge)
  - [NodeContext](#nodecontext)
  - [GraphEvent](#graphevent)
  - [GraphStreamCallback](#graphstreamcallback)
  - [NodeResult](#noderesult)
  - [ConditionFn](#conditionfn)
  - [Constants](#constants)
- [5. GraphState](#5-graphstate)
- [6. GraphNode](#6-graphnode)
  - [GraphNode (abstract)](#graphnode-abstract)
  - [LLMCallNode](#llmcallnode)
  - [ToolDispatchNode](#tooldispatchnode)
  - [IntentClassifierNode](#intentclassifiernode)
  - [SubgraphNode](#subgraphnode)
- [7. GraphEngine](#7-graphengine)
  - [EngineConfig と EngineResources](#engineconfig-and-engineresources)
  - [RunConfig](#runconfig)
  - [RunResult](#runresult)
  - [GraphEngine](#graphengine)
- [7b. エンジン内部](#7b-engine-internals)
  - [GraphCompiler](#graphcompiler)
  - [Scheduler](#scheduler)
  - [CheckpointCoordinator](#checkpointcoordinator)
  - [NodeExecutor](#nodeexecutor)
- [8. チェックポイント](#8-checkpoint)
  - [Checkpoint (struct)](#checkpoint-struct)
  - [CheckpointStore](#checkpointstore)
  - [InMemoryCheckpointStore](#inmemorycheckpointstore)
- [9. Store](#9-store)
  - [Namespace](#namespace)
  - [StoreItem](#storeitem)
  - [Store (abstract)](#store-abstract)
  - [InMemoryStore](#inmemorystore)
- [10. ローダー](#10-loader)
  - [ReducerRegistry](#reducerregistry)
  - [ConditionRegistry](#conditionregistry)
  - [NodeFactory](#nodefactory)
  - [Built-in Registrations](#built-in-registrations)
- [11. ReAct グラフ](#11-react-graph)
- [12. LLM モジュール](#12-llm-module)
  - [OpenAIProvider](#openaiprovider)
  - [SchemaProvider](#schemaprovider)
  - [Agent](#agent)
  - [json_path ユーティリティ](#json_path-utilities)
- [13. MCP モジュール](#13-mcp-module)
  - [MCPTool](#mcptool)
  - [MCPClient](#mcpclient)
- [14. Util モジュール](#14-util-module)
  - [RequestQueue](#requestqueue)
- [使用例](#usage-examples)
  - [最小 ReAct エージェント](#minimal-react-agent)
  - [条件付きルーティングを持つカスタムグラフ](#custom-graph-with-conditional-routing)
  - [チェックポイント付き Human-in-the-Loop](#human-in-the-loop-with-checkpointing)
  - [Send による動的ファンアウト](#dynamic-fan-out-with-send)
  - [Command によるルーティング上書き](#routing-override-with-command)
  - [SchemaProvider のマルチ LLM 対応](#schemaprovider-multi-llm-support)
  - [MCP ツール統合](#mcp-tool-integration)
---
<a id="1-foundation-types"></a>
## 1. 基礎型
**ヘッダー:** `<neograph/types.h>`
**名前空間:** `neograph`
全モジュールで共有するコアデータ型です。LLM のチャットプロトコルにおける
メッセージ、ツール呼び出し、補完結果、およびそれらの JSON シリアライズを表します。
### ToolCall
LLM が要求した 1 回のツール呼び出しを表します。
```cpp
struct ToolCall {
    std::string id;         // Unique identifier assigned by the LLM
    std::string name;       // Name of the tool to call
    std::string arguments;  // JSON-encoded string of arguments
};
```

| フィールド | 型 | 説明 |
|-------|------|-------------|
| `id` | `std::string` | LLM が割り当てる、このツール呼び出し固有の識別子 |
| `name` | `std::string` | 呼び出すツール関数の名前 |
| `arguments` | `std::string` | 呼び出し引数を含む JSON エンコード文字列 |
### ChatMessage
会話中の 1 件のメッセージです。system、user、assistant、tool のすべてのロールに対応します。
```cpp
struct ChatMessage {
    std::string role;                    // "system", "user", "assistant", or "tool"
    std::string content;                 // Text content of the message
    std::vector<ToolCall> tool_calls;    // Tool calls (assistant messages only)
    std::string tool_call_id;           // ID of the tool call this responds to (tool messages)
    std::string tool_name;              // Name of the tool (tool messages)
    std::vector<std::string> image_urls; // base64 data URLs or HTTP URLs for Vision
};
```

| フィールド | 型 | 説明 |
|-------|------|-------------|
| `role` | `std::string` | メッセージのロール: `"system"`、`"user"`、`"assistant"`、`"tool"` |
| `content` | `std::string` | メッセージのテキスト内容 |
| `tool_calls` | `std::vector<ToolCall>` | assistant が要求したツール呼び出し (assistant 以外では空) |
| `tool_call_id` | `std::string` | このツール結果を元のツール呼び出しに結び付ける ID |
| `tool_name` | `std::string` | この結果を生成したツールの名前 |
| `image_urls` | `std::vector<std::string>` | マルチモーダル/vision メッセージの画像 URL。`data:image/...;base64,...` または `https://...` を受け付ける |
### ChatTool
LLM が利用できるツールを定義します。
```cpp
struct ChatTool {
    std::string name;        // Tool name (unique identifier)
    std::string description; // Human-readable description for the LLM
    json parameters;         // JSON Schema describing the tool's parameters
};
```

| フィールド | 型 | 説明 |
|-------|------|-------------|
| `name` | `std::string` | ツール固有の名前 |
| `description` | `std::string` | ツールの目的を LLM に説明する表示用説明 |
| `parameters` | `json` | 受け付けるパラメーターを記述する JSON Schema オブジェクト |
### ChatCompletion
1 回の LLM 補完呼び出しの結果です。
```cpp
struct ChatCompletion {
    ChatMessage message;   // The assistant's response message
    std::string stop_reason = "unknown";
    struct Usage {
        int prompt_tokens = 0;      // Tokens in the prompt
        int completion_tokens = 0;  // Tokens in the completion
        int total_tokens = 0;       // Total tokens used
    } usage;
};
```

| フィールド | 型 | 説明 |
|-------|------|-------------|
| `message` | `ChatMessage` | assistant の応答 (ツール呼び出しを含む場合がある) |
| `stop_reason` | `std::string` | 正規化されたプロバイダーの完了理由: `end_turn`、`max_tokens`、`stop_sequence`、`tool_use`、`content_filter`、`refusal`、`unknown` |
| `usage.prompt_tokens` | `int` | 入力プロンプトのトークン数 |
| `usage.completion_tokens` | `int` | 生成された補完結果のトークン数 |
| `usage.total_tokens` | `int` | 消費したトークンの合計 (プロンプト + 補完結果) |
`stop_reason` が公開 C++ 構造体に追加されました。このリリースへアップグレードする際は、
共有ライブラリの利用側は、構造体のバイナリレイアウトが変わったため、このリリースへのアップグレード時に再コンパイルしてください。
<a id="helper-functions"></a>
### ヘルパー関数
#### `messages_to_json`
メッセージベクターを OpenAI 互換の JSON ワイヤー形式に変換します。ツール呼び出し
メッセージ、ツール結果メッセージ、マルチモーダル (Vision) メッセージを適切な構造で処理します。
```cpp
json messages_to_json(const std::vector<ChatMessage>& messages);
```

**戻り値:** 各要素が適切な形式のメッセージオブジェクトである `json` 配列。
#### `tools_to_json`
ツール定義を OpenAI 互換の JSON ワイヤー形式に変換し、各ツールを
`{type: "function", function: {...}}` のエンベロープに格納します。
```cpp
json tools_to_json(const std::vector<ChatTool>& tools);
```

**戻り値:** ツール定義の `json` 配列。
#### `parse_response_message`
OpenAI 形式 API のレスポンスから単一の choice オブジェクトを `ChatMessage` に解析します。
`message` フィールドから assistant の内容とツール呼び出しを抽出します。
```cpp
ChatMessage parse_response_message(const json& choice);
```

| パラメーター | 型 | 説明 |
|-----------|------|-------------|
| `choice` | `const json&` | `choices` 配列の 1 要素 (必ず `message` フィールドを含む) |
**戻り値:** role、content、ツール呼び出しが設定された `ChatMessage`。
### ADL Serialization
ADL により `json j = my_tool_call;` や `my_tool_call = j.get<ToolCall>()` のように直接利用できます。
```cpp
void to_json(json& j, const ToolCall& tc);
void from_json(const json& j, ToolCall& tc);

void to_json(json& j, const ChatMessage& msg);
void from_json(const json& j, ChatMessage& msg);
```

すべてのフィールドは空文字列をデフォルトとする `value()` を使うため、欠落したフィールドにも対応できます。
---
<a id="2-provider-interface"></a>
## 2. Provider インターフェース
**ヘッダー:** `<neograph/provider.h>`, `<neograph/completion_provider.h>`
**名前空間:** `neograph`
LLM バックエンドの抽象インターフェースです。任意の LLM API を追加するにはこれを実装します。
> **新しい Provider 実装を書く場合:** `CompletionProvider` を継承して `do_invoke()` を実装します。既存の `Provider`
> サブクラスと `complete*` 呼び出し側は削除予定なしで引き続きサポートされます。
> [`ASYNC_GUIDE.md` §9.3](ASYNC_GUIDE.md#93-provider) を参照してください。
### StreamCallback
ストリーミングされるトークンのコールバック用型エイリアスです。
```cpp
using StreamCallback = std::function<void(const std::string& chunk)>;
```

ストリーミング補完中に、トークン (またはチャンク) ごとに 1 回呼び出されます。`chunk` 引数には増分テキスト断片が入ります。
増分で届くテキスト断片が入ります。
### CompletionParams
1 回の LLM 補完リクエストのパラメーターです。
```cpp
struct CompletionParams {
    std::string model;                // Model identifier (e.g. "gpt-4o")
    std::vector<ChatMessage> messages; // Conversation history
    std::vector<ChatTool> tools;      // Available tools (empty = no tool use)
    float temperature = 0.7f;         // Sampling temperature
    int max_tokens = -1;              // Max tokens to generate (-1 = provider default)
};
```

| フィールド | 型 | デフォルト | 説明 |
|-------|------|---------|-------------|
| `model` | `std::string` | `""` | 使用するモデル。空ならプロバイダーのデフォルトモデルを使う |
| `messages` | `std::vector<ChatMessage>` | | 時系列順の会話メッセージ |
| `tools` | `std::vector<ChatTool>` | `{}` | LLM が呼び出せるツール。空ならツール利用を無効化 |
| `temperature` | `float` | `0.7f` | サンプリング温度 (0.0 = 決定的、高いほどランダム) |
| `max_tokens` | `int` | `-1` | 生成する最大トークン数。`-1` ならプロバイダーが決める |
### Provider
LLM プロバイダーの安定した互換性用基底クラスです。既存の実装と呼び出し側はこのインターフェースを引き続き利用できます。
新しい実装では、以下の `CompletionProvider` を優先してください。
```cpp
class Provider {
public:
    virtual ~Provider() = default;

    // Synchronous completion. Default body bridges to complete_async via
    // run_sync — backends that override the async peer get sync for
    // free, and vice versa. Override at least one side.
    virtual ChatCompletion complete(const CompletionParams& params);

    // Async completion (asio coroutine). Default body co_returns
    // complete(params).
    virtual asio::awaitable<ChatCompletion>
    complete_async(const CompletionParams& params);

    // Streaming completion (sync). Default emits the collected result once.
    virtual ChatCompletion complete_stream(const CompletionParams& params,
                                           const StreamCallback& on_chunk);

    // Async streaming peer. The default runs complete_stream on a worker
    // thread and delivers callbacks on the awaiting executor.
    virtual asio::awaitable<ChatCompletion>
    complete_stream_async(const CompletionParams& params,
                          const StreamCallback& on_chunk);

    // Stable callback-selected compatibility entry point.
    virtual asio::awaitable<ChatCompletion>
    invoke(const CompletionParams& params,
           StreamCallback on_chunk = nullptr);

    // Only pure virtual on this interface — every backend must name
    // itself.
    virtual std::string get_name() const = 0;
};
```

| メソッド | 説明 |
|--------|-------------|
| `complete(params)` | ブロッキング補完。`neograph::async::run_sync` 経由で `complete_async` にデフォルト転送 |
| `complete_async(params)` | コルーチン側。デフォルトは `co_return complete(params)` |
| `complete_stream(params, on_chunk)` | ストリーミング補完。チャンクごとに `on_chunk` を呼び、組み立てた `ChatCompletion` を返す |
| `complete_stream_async(params, on_chunk)` | 非同期ストリーミング側 (Round 4)。`on_chunk` の意味は同じ |
| `invoke(params, on_chunk)` | 既存エンジンコードが使う、コールバック選択式の互換エントリポイント |
| `get_name()` | 人間が読めるプロバイダー識別子 (純粋仮想なのはこれだけ) |
**少なくとも片側をオーバーライドする契約:** 各 `(sync, async)` ペアは
もう一方をデフォルトとして呼び出します。どちらもオーバーライドしないと、呼び出し時に相互の無限再帰になります。
下記の `CheckpointStore` の sync↔async ブリッジも同じ形です。
これらのメソッドには削除予定も非推奨警告もありません。互換性修正とセキュリティ修正は継続されます。
新しい機能は明示的なリクエスト API からのみ提供される場合があります。
### CompletionProvider
新しい C++ Provider 実装に推奨する基底クラスです。final アダプターを通じてすべての `Provider` エントリポイントを維持しつつ、
リクエストモードを認識したオーバーライドを 1 つ実装できます。
```cpp
class MyProvider : public neograph::CompletionProvider {
public:
    asio::awaitable<ChatCompletion>
    do_invoke(CompletionRequest request) override {
        if (request.streaming()) {
            // Use the streaming transport even when no observer is attached.
            // If present, request.on_chunk() receives incremental text.
        } else {
            // Use the collect transport.
        }
        co_return result;
    }

    std::string get_name() const override { return "my-provider"; }
};
```

新しい直接呼び出しでは、トランスポートモードを明示してください:
```cpp
auto full = co_await provider.invoke_request(
    CompletionRequest::collect(params));
auto streamed = co_await provider.invoke_request(
    CompletionRequest::stream(params, on_chunk));
```

---
<a id="3-tool-interface"></a>
## 3. Tool インターフェース
**ヘッダー:** `<neograph/tool.h>`
**名前空間:** `neograph`
LLM が呼び出せるツールの抽象インターフェースです。エージェントに関数を公開するにはこれを実装します。
> **カスタム Tool サブクラスを書く場合:** 同期の `Tool` と非同期の `AsyncTool` のどちらを継承するかは、
> [`ASYNC_GUIDE.md` §9.6](ASYNC_GUIDE.md#96-tool-vs-asynctool) で、
> 同期の `Tool` と非同期の `AsyncTool` のどちらを継承するか確認してください。2 つは
> 相互排他的なので、どちらか一方を選びます。
### Tool
```cpp
class Tool {
public:
    virtual ~Tool() = default;

    // Returns the tool's definition (name, description, parameter schema)
    virtual ChatTool get_definition() const = 0;

    // Executes the tool with the given arguments, returns result as string
    virtual std::string execute(const json& arguments) = 0;

    // Returns the tool's unique name
    virtual std::string get_name() const = 0;
};
```

| メソッド | 戻り値 | 説明 |
|--------|---------|-------------|
| `get_definition()` | `ChatTool` | パラメーター用 JSON Schema を含むツールメタデータを返す |
| `execute(arguments)` | `std::string` | 解析済み JSON 引数でツールを実行し、LLM に返す文字列結果を返す |
| `get_name()` | `std::string` | このツールの固有識別子 |
**実装例:**
```cpp
class WeatherTool : public neograph::Tool {
public:
    ChatTool get_definition() const override {
        return {"get_weather", "Get current weather for a city", json::parse(R"({
            "type": "object",
            "properties": {
                "city": {"type": "string", "description": "City name"}
            },
            "required": ["city"]
        })")};
    }

    std::string execute(const json& args) override {
        std::string city = args.at("city");
        return "Weather in " + city + ": 22C, sunny";
    }

    std::string get_name() const override { return "get_weather"; }
};
```

---
<a id="4-graph-types"></a>
## 4. グラフ型
**ヘッダー:** `<neograph/graph/types.h>`
**名前空間:** `neograph::graph`
グラフエンジンの中核型です。チャネル、エッジ、イベント、制御フローの基本要素を定義します。
### ReducerType
複数のノードが書き込んだときに、チャネル値をどのようにマージするかを決めます。
```cpp
enum class ReducerType {
    OVERWRITE,  // New value replaces old value
    APPEND,     // New value is appended (for array channels)
    CUSTOM      // User-defined reducer function
};
```

### ReducerFn
カスタムリデューサー関数のシグネチャです。
```cpp
using ReducerFn = std::function<json(const json& current, const json& incoming)>;
```

| パラメーター | 説明 |
|-----------|-------------|
| `current` | 現在のチャネル値 |
| `incoming` | 書き込まれる新しい値 |
**戻り値:** 新しいチャネル値になるマージ済みの結果。
### Channel
関連するリデューサーを持つ、名前付きでバージョン管理された状態チャネルの内部表現です。
```cpp
struct Channel {
    std::string name;                              // Channel name
    ReducerType reducer_type = ReducerType::OVERWRITE; // Merge strategy
    ReducerFn   reducer;                           // Custom reducer (when type == CUSTOM)
    json        value;                             // Current value
    uint64_t    version = 0;                       // Write counter
};
```

### ChannelWrite
名前付きチャネルを対象とする 1 回の書き込み操作です。ノードはこれらのベクターを返します。
```cpp
struct ChannelWrite {
    std::string channel;  // Target channel name
    json        value;    // Value to write (merged via the channel's reducer)
};
```

### NodeInterrupt
動的な中断点 (Human-in-the-Loop) を発生させるためにノード内から送出する例外型です。
送出されると実行が一時停止し、チェックポイントが保存され、後から中断を再開できます。
```cpp
class NodeInterrupt : public std::runtime_error {
public:
    explicit NodeInterrupt(const std::string& reason);
    NodeInterrupt(const std::string& reason, json value);   // with a payload
    const std::string& reason() const;
    const json&        value()  const;   // null when no payload was attached
    const std::string& node()   const;   // stamped by the executor
};
```

| メソッド | 戻り値 | 説明 |
|--------|---------|-------------|
| `reason()` | `const std::string&` | コンストラクターに渡された理由文字列 |
| `value()` | `const json&` | 構造化ペイロード。付与されていない場合は null |
| `node()` | `const std::string&` | 例外を投げたノード。executor がここに記録するため、ノード本体はグラフ定義上の自分の名前を知る必要がありません |
**往復処理。** 承認プロンプトの情報は双方向に移動します。ノードは *何を* 承認すべきかを示し、人間の回答は
要求したノードへ戻らなければなりません。
```cpp
asio::awaitable<NodeResult> run(NodeInput in) override {
    // The human's answer. Empty until someone has actually answered — which is
    // how you tell "nobody has looked yet" from "the answer was no".
    const auto& verdict = in.ctx.resume_value;

    if (needs_approval(in.state) && !verdict) {
        throw NodeInterrupt("shell command needs approval",
                            json{{"tool", "shell"}, {"cmd", "rm -rf build/"}});
    }
    if (verdict && !verdict->value("approved", false)) {
        co_return refused();
    }
    co_return proceed();
}
```

呼び出し側には一時停止が通常の `RunResult` として見えます。`NodeInterrupt` が呼び出し側へ再送出されることはありません:
```cpp
auto r = engine->run(cfg);
if (r.interrupted) {
    r.interrupt_node;                          // "risky"  — which node paused
    r.interrupt_value["reason"];               // the sentence, for a human
    r.interrupt_value["value"];                // the payload, to branch on
                                               //   (key absent if none attached)
    engine->resume(cfg.thread_id, json{{"approved", true}});   // the answer
}
```

`resume_value` は `messages` チャネルがある場合、そのユーザーターンとしても届きます。
これはチャット形式のグラフが従来から値を受け取ってきた方法です。
`ctx.resume_value` は一般的な経路であり、グラフのチャネル名に関係なく利用できます。
これは *動的* な中断形式です。*静的* な形式である
グラフ定義の `interrupt_before` / `interrupt_after` は、グラフ作成時に選んだノードで一時停止します。
そのため「モデルが危険なものを要求したときだけ一時停止する」といった条件は表現できません。
### Send
動的なファンアウト要求を表します。ノードは異なる入力を持つ 1 つ以上のノードへ送る `Send` オブジェクトを返し、map-reduce パターンを実現できます。
```cpp
struct Send {
    std::string target_node;  // Node to dispatch
    json        input;        // Channel writes for that invocation
};
```

エンジンは各 `Send` の対象を専用の入力で実行し、すべての Send が完了してからグラフを続行します。同じノードへの複数の Send は順番に実行されます。
### Command
ルーティングの上書きと状態更新を組み合わせたものです。ノードが `Command` を返すと、状態更新を書き込み、特定の次ノードへ実行をリダイレクトし、通常のエッジルーティングを迂回できます。
```cpp
struct Command {
    std::string               goto_node;  // Next node (overrides edge routing)
    std::vector<ChannelWrite> updates;    // State updates to apply
};
```

| フィールド | 型 | 説明 |
|-------|------|-------------|
| `goto_node` | `std::string` | 次に実行するノード名。通常のエッジ解決を上書きします |
| `updates` | `std::vector<ChannelWrite>` | ルーティング前に適用するチャネル書き込み |
### RetryPolicy
ノード実行の失敗に対する自動再試行動作を設定します。
```cpp
struct RetryPolicy {
    int   max_retries        = 0;      // 0 = no retry
    int   initial_delay_ms   = 100;    // First retry delay in milliseconds
    float backoff_multiplier = 2.0f;   // Exponential backoff factor
    int   max_delay_ms       = 5000;   // Maximum delay cap in milliseconds
    float jitter_pct         = 0.0f;   // Per-retry jitter as a fraction of
                                       // the computed delay (0.25 = ±25%).
                                       // Default 0 = back-compat. Per-thread
                                       // RNG, no global state.
};
```

再試行 `n` の遅延は `min(initial_delay_ms * backoff_multiplier^n, max_delay_ms)` で、`jitter_pct > 0` の場合は任意で `1 + uniform(-jitter_pct, +jitter_pct)` を乗じます。
### StreamMode
ストリーミング実行中にどのイベントを発行するかを制御するビットフィールドフラグです。
```cpp
enum class StreamMode : uint8_t {
    EVENTS  = 0x01,  // NODE_START, NODE_END, INTERRUPT, ERROR
    TOKENS  = 0x02,  // LLM_TOKEN (individual tokens from streaming LLM calls)
    VALUES  = 0x04,  // Full state snapshot after each step
    UPDATES = 0x08,  // Channel write deltas per node
    DEBUG   = 0x10,  // Internal debug info (retry attempts, routing decisions)
    ALL     = 0xFF   // All event types
};
```

ビット単位の OR でフラグを組み合わせます:
```cpp
StreamMode mode = StreamMode::EVENTS | StreamMode::TOKENS;
```

**Operators:**
```cpp
StreamMode operator|(StreamMode a, StreamMode b);  // Combine flags
StreamMode operator&(StreamMode a, StreamMode b);  // Mask flags
bool has_mode(StreamMode flags, StreamMode test);   // Test if flag is set
```

### Edge
2 つのノード間の静的な有向エッジです。
```cpp
struct Edge {
    std::string from;  // Source node name
    std::string to;    // Target node name
};
```

グラフの入口と出口には特殊定数 `START_NODE` と `END_NODE` を使用します。
### ConditionalEdge
実行時に名前付き条件関数が決める動的なエッジです。
```cpp
struct ConditionalEdge {
    std::string from;                              // Source node name
    std::string condition;                         // Name in ConditionRegistry
    std::map<std::string, std::string> routes;     // condition_result -> target node name
};
```

実行時にエンジンが条件関数を呼び出します (`ConditionRegistry` から名前で検索します)。
関数の戻り値を `routes` マップのキーとして使い、次のノードを決定します。
### NodeContext
ノードのコンストラクターに渡す依存性注入コンテナーです。LLM プロバイダー、ツール、設定にアクセスできます。
```cpp
struct NodeContext {
    std::shared_ptr<Provider> provider;   // LLM provider
    std::vector<Tool*>        tools;      // Available tools (non-owning)
    std::string               model;      // Model override (empty = provider default)
    std::string               instructions; // System prompt / instructions
    json                      extra_config; // Additional configuration (node-type-specific)
};
```

新しいエンジンでは、ポインターを個別に管理する代わりに `EngineResources` 経由で `ToolSet` を移動することを推奨します。
`GraphEngine::build()` は対応する非所有ビューを `NodeContext` に結び付け、
エンジンの寿命中はすべてのツールを生存させます。
### GraphEvent
ストリーミングによるグラフ実行中に発行されるイベントです。
```cpp
struct GraphEvent {
    enum class Type {
        NODE_START,     // A node is about to execute
        NODE_END,       // A node has finished executing
        LLM_TOKEN,      // A single token from a streaming LLM call
        CHANNEL_WRITE,  // A channel value was updated
        INTERRUPT,      // Execution paused (NodeInterrupt or configured breakpoint)
        ERROR           // An error occurred during execution
    };

    Type        type;       // Event type
    std::string node_name;  // Name of the node that produced this event
    json        data;       // Event payload (varies by type)
};
```

**イベントデータのペイロード:**
| 型 | `data` の内容 |
|------|-----------------|
| `NODE_START` | `{}` またはノードメタデータ |
| `NODE_END` | ノードが生成したチャネル書き込み |
| `LLM_TOKEN` | `{"token": "..."}` |
| `CHANNEL_WRITE` | `{"channel": "...", "value": ...}` |
| `INTERRUPT` | `{"reason": "...", "node": "..."}` |
| `ERROR` | `{"error": "...", "node": "..."}` |
### GraphStreamCallback
ストリーミング実行で使うグラフイベントコールバックの型エイリアスです。
```cpp
using GraphStreamCallback = std::function<void(const GraphEvent&)>;
```

`GraphEvent` は安定したコールバック形式および JSON 向け形式です。型付きペイロードが必要なコードは、
エンジンの入口を変えずに同じストリームを適応できます:
```cpp
using TypedGraphEvent = std::variant<NodeStartEvent, NodeEndEvent,
    LlmTokenEvent, ChannelWriteEvent, StateSnapshotEvent, RoutingEvent,
    SendDispatchEvent, InterruptEvent, ErrorEvent, RawGraphEvent>;

auto callback = adapt_typed_stream([](const TypedGraphEvent& event) {
    std::visit([](const auto& typed) {
        // Handle NodeStartEvent, LlmTokenEvent, and the other alternatives.
    }, event);
});
```

`to_typed_event()` が直接変換します。不正なペイロードや将来のバージョンで追加された形は、
ストリーミングコールバックから例外を送出せず `RawGraphEvent` になります。
### NodeResult
ノード実行から返す拡張型です。チャネル書き込みを、任意の `Command` と `Send` の高度な制御フロー指示で包みます。
```cpp
struct NodeResult {
    std::vector<ChannelWrite> writes;           // Channel updates
    std::optional<Command>    command;           // Routing override (if set)
    std::vector<Send>         sends;             // Dynamic fan-out targets

    NodeResult() = default;
    NodeResult(std::vector<ChannelWrite> w);     // Implicit from plain writes
};
```

`command` が設定されると通常のエッジルーティングを迂回し、
`command->goto_node` へ実行を移します。`sends` が空でない場合、エンジンは指定された対象へ
動的にファンアウトします。
### ConditionFn
条件付きエッジで使う条件関数のシグネチャです。
```cpp
using ConditionFn = std::function<std::string(const GraphState&)>;
```

関数は現在のグラフ状態を調べて文字列キーを返します。このキーを `ConditionalEdge::routes` マップで検索し、次のノードを決定します。
### Constants
```cpp
constexpr const char* START_NODE = "__start__";  // Graph entry point
constexpr const char* END_NODE   = "__end__";    // Graph termination
```

これらはエッジ定義でグラフの入口と出口を示すために使用します:
```cpp
Edge{START_NODE, "my_first_node"}
Edge{"my_last_node", END_NODE}
```

---
## 5. GraphState
**ヘッダー:** `<neograph/graph/state.h>`
**名前空間:** `neograph::graph`
グラフ用のスレッドセーフでバージョン管理されたキー値状態コンテナーです。各エントリは、値のマージ方法を制御する
関連するリデューサーによって値のマージ方法が決まる、名前付きチャネルです。
```cpp
class GraphState {
public:
    void init_channel(const std::string& name,
                      ReducerType type,
                      ReducerFn reducer,
                      const json& initial_value = json());

    json get(const std::string& channel) const;
    std::vector<ChatMessage> get_messages() const;

    void write(const std::string& channel, const json& value);
    void apply_writes(const std::vector<ChannelWrite>& writes);

    uint64_t channel_version(const std::string& channel) const;
    uint64_t global_version() const;

    json serialize() const;
    void restore(const json& data);

    std::vector<std::string> channel_names() const;
};
```

| メソッド | 説明 |
|--------|-------------|
| `init_channel(name, type, reducer, initial_value)` | reducer と任意の初期値を指定してチャネルを登録します。そのチャネルを読み書きする前に呼び出す必要があります |
| `get(channel)` | チャネルの現在値を読みます。スレッドセーフです (共有ロック) |
| `get_messages()` | 便利メソッドです。`"messages"` チャネルを読み、`std::vector<ChatMessage>` としてデシリアライズします |
| `write(channel, value)` | reducer を通じて単一チャネルに値を書き込みます。スレッドセーフです (排他ロック) |
| `apply_writes(writes)` | `ChannelWrite` 操作のまとまりをアトミックに適用します。すべての書き込みは 1 つの排他ロック下で適用されます |
| `channel_version(channel)` | 指定チャネルの書き込みカウンターを返します |
| `global_version()` | グローバルバージョンカウンターを返します (任意のチャネルへの書き込みごとに増加) |
| `serialize()` | すべてのチャネル値とバージョンを JSON にシリアライズします (チェックポイント用) |
| `restore(data)` | シリアライズ済み JSON からチャネル値とバージョンを復元します |
| `channel_names()` | 初期化済みの全チャネル名を返します |
---
## 6. GraphNode
**ヘッダー:** `<neograph/graph/node.h>`
**名前空間:** `neograph::graph`
ノードはグラフの計算単位です。ライブラリには抽象基底クラスと 4 種類の組み込みノードがあります。
<a id="graphnode-abstract"></a>
### GraphNode (抽象)
サブクラスがオーバーライドするメソッドは 1 つだけです: `run(NodeInput) -> awaitable<NodeOutput>`。
状態を読み取り、処理を決め、書き込み (必要なら `Command` / `Send` も) を返します。
```cpp
class GraphNode {
public:
    virtual ~GraphNode() = default;

    // The only custom-node dispatch entry.
    virtual asio::awaitable<NodeOutput> run(NodeInput in) = 0;

    virtual std::string get_name() const = 0;
};

struct NodeInput {
    const GraphState&          state;       // channels visible to this node
    const RunContext&          ctx;         // cancel_token, step, thread_id, ...
    const GraphStreamCallback* stream_cb;   // null when not streaming
};

using NodeOutput = NodeResult;  // writes + optional Command + optional Sends
```

| メンバー | 説明 |
|--------|-------------|
| `in.state` | 読み取り専用の `GraphState`。読み取りには `in.state.get(channel)` を使います |
| `in.ctx.cancel_token` | `provider.complete(params)` に渡すと、キャンセル時に LLM HTTP ソケットを中断できます。独自ループでは `ctx.cancel_token->is_cancelled()` をポーリングします |
| `in.ctx.step` | 現在のスーパーステップ番号 |
| `in.ctx.thread_id` | `RunConfig::thread_id` を反映します |
| `in.stream_cb` | ストリーミング出力先。null でなければここから `LLM_TOKEN` イベントを送出します。非ストリーミング実行では null です |
| 戻り値: `NodeOutput.writes` | エンジンが reducer でマージするチャネル書き込み |
| 戻り値: `NodeOutput.command` | 任意のルーティング上書き (`goto_node` + 状態更新) |
| 戻り値: `NodeOutput.sends` | 任意の動的ファンアウト。エンジンは `Send` ごとに 1 つの分岐を生成します |
| `get_name()` | グラフ内で一意なノード名を返します |
最小例:
```cpp
class CounterNode : public neograph::graph::GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto current = in.state.get("count");
        int n = current.is_number() ? current.get<int>() : 0;
        NodeOutput out;
        out.writes.push_back({"count", n + 1});
        co_return out;
    }
    std::string get_name() const override { return "counter"; }
};
```

非同期ネイティブな LLM 呼び出し:
```cpp
class ChatNode : public neograph::graph::GraphNode {
    std::shared_ptr<Provider> provider_;
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        CompletionParams params;
        params.messages    = in.state.get_messages();
        params.cancel_token = in.ctx.cancel_token;  // cancel propagates
        auto reply = co_await provider_->complete_async(params);
        NodeOutput out;
        json msg;
        to_json(msg, reply.message);
        out.writes.push_back({"messages", json::array({msg})});
        co_return out;
    }
    std::string get_name() const override { return "chat"; }
};
```

> **移行上の注意。** `GraphNode` のノードエントリポイントは
> `run(NodeInput)` の 1 つです。これは `Command` と `Send` を保持し、
> 非同期およびストリーミング実行に参加するため、サブクラスが実装する
> オーバーライドです。
### LLMCallNode
現在の会話状態で LLM を呼び出します。`"messages"` チャネルを読み、プロバイダーへ補完リクエストを送り、
assistant の応答をチャネルへ書き戻します。`run_stream` / `run_stream_async` で開始した場合は
`LLM_TOKEN` イベントをストリーミングします。
```cpp
class LLMCallNode : public GraphNode {
public:
    LLMCallNode(const std::string& name, const NodeContext& ctx);
    asio::awaitable<NodeOutput> run(NodeInput in) override;
    std::string get_name() const override;
};
```

| コンストラクターパラメーター | 説明 |
|-----------------------|-------------|
| `name` | ノード名 |
| `ctx` | LLM provider、tools、model、instructions を提供するノードコンテキスト |
(LLMCallNode、`ToolDispatchNode`、`IntentClassifierNode`、`SubgraphNode` は
すべて同じ `run(NodeInput)` 契約を実装します。)
### ToolDispatchNode
直近の assistant メッセージからツール呼び出しをディスパッチします。`"messages"` チャネルから保留中のツール呼び出しを読み、各ツールを実行して、ツール結果メッセージを書き戻します。
```cpp
class ToolDispatchNode : public GraphNode {
public:
    ToolDispatchNode(const std::string& name, const NodeContext& ctx);

    asio::awaitable<NodeOutput> run(NodeInput in) override;
    std::string get_name() const override;
};
```

| コンストラクターパラメーター | 説明 |
|-----------------------|-------------|
| `name` | ノード名 |
| `ctx` | ノードコンテキスト (`ctx.tools` でツールを検索して実行します) |
### IntentClassifierNode
LLM を使ってユーザーの意図を分類し、結果を `"__route__"` チャネルに書き込みます。
組み込み条件 `"route_channel"` と組み合わせて、意図に基づく動的なルーティングを可能にします。
```cpp
class IntentClassifierNode : public GraphNode {
public:
    IntentClassifierNode(const std::string& name, const NodeContext& ctx,
                         const std::string& prompt,
                         std::vector<std::string> valid_routes);

    asio::awaitable<NodeOutput> run(NodeInput in) override;
    std::string get_name() const override;
};
```

| コンストラクターパラメーター | 型 | 説明 |
|-----------------------|------|-------------|
| `name` | `std::string` | ノード名 |
| `ctx` | `NodeContext` | 分類用 LLM 呼び出しに使う Provider と model |
| `prompt` | `std::string` | 分類プロンプトテンプレート |
| `valid_routes` | `std::vector<std::string>` | 許可する分類値。LLM 出力はこれらに照らして検証されます |
### SubgraphNode
コンパイル済みの `GraphEngine` を 1 つのノードとしてラップし、階層的なグラフ構成
(supervisor パターン、ネストしたワークフロー) を可能にします。チャネルマッピングで親子グラフ間のデータの流れを制御します。
```cpp
class SubgraphNode : public GraphNode {
public:
    SubgraphNode(const std::string& name,
                 std::shared_ptr<GraphEngine> subgraph,
                 std::map<std::string, std::string> input_map = {},
                 std::map<std::string, std::string> output_map = {});
    asio::awaitable<NodeOutput> run(NodeInput in) override;
    std::string get_name() const override;
};
```

| コンストラクターパラメーター | 型 | 説明 |
|-----------------------|------|-------------|
| `name` | `std::string` | 親グラフ内のノード名 |
| `subgraph` | `std::shared_ptr<GraphEngine>` | コンパイル済みの子グラフエンジン |
| `input_map` | `std::map<std::string, std::string>` | `parent_channel -> child_channel` の対応付け。親から読み、子の入力へ書き込みます |
| `output_map` | `std::map<std::string, std::string>` | `child_channel -> parent_channel` の対応付け。子が生成した write delta のチャネル名を変更して親へ転送します |
マップが空の場合、チャネルは名前でマッピングされます (identity mapping)。

入力マッピングは親の現在のチャネル値を子の入力へコピーします。出力マッピングは
意図的に異なり、子の最終シリアライズ状態を新しい reducer 入力として扱わず、子が
生成した順序で `ChannelWrite` delta を転送して各 write の `Mode` を保持します。
そのため、継承した append/custom 値が二重に適用されません。出力マッピングは
snapshot replacement を推論しません。マッピング先の親値を置き換える場合、子は
`ChannelWrite::Mode::Overwrite` を明示的に送出する必要があります。

#### ランタイムコンテキストの伝播

`SubgraphNode` はエンジン境界で子の実行コンテキストを派生させます。公開
`RunContext` のレイアウトは変更しません。

| コンテキスト値 | 子での意味 |
|---------------|-----------------|
| `cancel_token` | 子操作用トークンを作成するため、親のキャンセルはすべての子と孫へ届きます。 |
| `usage`, `deadline`, `trace_id`, `stream_mode` | 継承します。`deadline` と `trace_id` は `RunMetadata` 由来で、子は親の stream mode を広げられません。 |
| `thread_id` | 親 thread ID が空でなければ、親 ID、subgraph ノード名、super-step、invocation identity から決定的に派生します。そのため sibling `Send` 呼び出しは別々の checkpoint identity を得ます。親 thread ID が空なら子もスコープなしとなり checkpointing は無効です。 |
| `step` | 子実行にローカルで、子 checkpoint または 0 から始まります。 |
| `store` | 親 Store があれば継承し、なければ子エンジンに設定された Store を維持します。 |
| Tool policy | 親 `ToolGate` が子の gate より先に実行されます。子は許可された呼び出しをさらに制限または rewrite できますが、親の deny/interrupt は回避できません。 |
| Checkpoint backend and resume value | 親 backend があれば継承し、なければ子の backend を維持します。親 resume は派生した子 checkpoint identity が存在する場合のみ子 checkpoint を resume し、null でない resume value を転送します。Checkpoint routing は公開 `RunContext` フィールドではなく内部実装です。 |

---
## 7. GraphEngine
**ヘッダー:** `<neograph/graph/engine.h>`
**名前空間:** `neograph::graph`
コア実行エンジンです。グラフ定義をコンパイルし、状態遷移を管理し、スーパーステップループでノード実行を調整します。
<a id="engineconfig-and-engineresources"></a>
### EngineConfig と EngineResources
新しいコードでは、エンジンを作成する前に構築時の依存関係とポリシーを組み立てます:
```cpp
struct EngineConfig {
    NodeContext node_context;
    std::shared_ptr<CheckpointStore> checkpoint_store;
    std::shared_ptr<Store> store;
    std::optional<RetryPolicy> retry_policy;
    std::map<std::string, RetryPolicy> node_retry_policies;
    ToolGate tool_gate;
    std::size_t worker_count = 1;
    std::set<std::string> cached_nodes;
};

struct EngineResources {
    ToolSet tools;
    std::shared_ptr<const GraphRegistry> registry;
};
```

`ToolSet` は固定されたツール集合を所有する move-only オーナーです。`GraphRegistry` はエンジンごとのリデューサー、条件、ノードファクトリーのオーバーレイです。
オーバーレイにない名前は既存のプロセス全体レジストリへフォールバックします。
`build()` または `link()` に渡す前に両方を構成してください。
実行時の変更はローカルレジストリ契約の対象外です。
### RunConfig
1 回のグラフ実行を設定します。
```cpp
struct RunConfig {
    std::string                 thread_id;
    json                        input;
    int                         max_steps    = 50;
    StreamMode                  stream_mode  = StreamMode::ALL;
    std::shared_ptr<CancelToken> cancel_token;          // v0.3+
    std::shared_ptr<UsageAccumulator> usage;             // optional accumulator
    bool                        resume_if_exists = false; // v0.3.1+
};
```

| フィールド | 型 | デフォルト | 説明 |
|-------|------|---------|-------------|
| `thread_id` | `std::string` | `""` | チェックポイント用に会話/セッションを識別します |
| `input` | `json` | `{}` | 実行開始前にチャネルへ書き込む初期値。通常は `{"messages": [...]}` です |
| `max_steps` | `int` | `50` | 強制終了までの最大スーパーステップ数 (無限ループを防ぎます) |
| `stream_mode` | `StreamMode` | `ALL` | ストリーミング中に送出するイベント種別を制御するビットフィールド |
| `cancel_token` | `std::shared_ptr<CancelToken>` | `nullptr` | 協調キャンセル用ハンドル。エンジンはこれを `RunContext` に包み、各ノードの `run(NodeInput)` 呼び出しへ `in.ctx.cancel_token` として渡します |
| `usage` | `std::shared_ptr<UsageAccumulator>` | `nullptr` | 任意のトークン集計器。省略時はエンジンが作成し、実行中の集計器を `in.ctx.usage` として公開します |
| `resume_if_exists` | `bool` | `false` | `true` かつ `thread_id` のチェックポイントが存在する場合、`input` を適用する前にそこから初期化します (複数ターンのチャット形状) |
### RunContext (v0.4 PR 1、`NodeInput.ctx` 経由でノードに公開)
エンジンが実行ごとに運ぶディスパッチメタデータです。`RunConfig`（未指定時は新しい
usage accumulator を作成）、`RunMetadata`、有効な Store、任意の resume value
から構築されます。ノードは `run(NodeInput) -> NodeOutput` のオーバーライド内で
`in.ctx` 経由で利用します。
```cpp
struct RunContext {
    std::shared_ptr<CancelToken>  cancel_token;
    std::shared_ptr<UsageAccumulator> usage;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::string                   trace_id;
    std::string                   thread_id;
    int                           step;
    StreamMode                    stream_mode;
    std::optional<json>           resume_value;
    std::shared_ptr<Store>        store;
    ToolGate                      tool_gate;
};
```

| フィールド | 説明 |
|-------|-------------|
| `cancel_token` | 実行中のトークン。`provider.complete(params)` に渡すと、キャンセル時に LLM HTTP ソケットを中断できます。独自ループでは `is_cancelled()` をポーリングします |
| `usage` | エンジンが値を入れる共有トークン集計先 |
| `deadline` | C++ `RunMetadata` から渡される任意の絶対 deadline |
| `trace_id` | C++ `RunMetadata` から渡される任意の trace correlator |
| `thread_id` | `RunConfig.thread_id` を反映します |
| `step` | 現在のスーパーステップ番号。反復ごとに更新されます |
| `stream_mode` | `RunConfig.stream_mode` を反映します |
| `resume_value` | `GraphEngine::resume()` に渡された値。新規実行では空です |
| `store` | エンジンに設定された Store。未設定の場合は `nullptr` |
| `tool_gate` | 継承された親ポリシーを含む、この invocation の有効なポリシー |
### CancelToken
呼び出し側とエンジンで共有する協調的なキャンセル基本単位です。`std::make_shared<CancelToken>()` で作成し、`RunConfig.cancel_token` に渡して、
実行中のスレッドから `cancel()` を呼ぶと実行を中断できます。
ノードが `provider.complete_async` の途中にいる場合は LLM HTTP ソケットも含まれます。
各エンジン実行は専用の操作用子をフォークするため、
1 つの親から複数の同時実行を安全にキャンセルできます。asio のキャンセルスロットを共有する必要はありません。
エンジンの操作用子は、投稿したキャンセル通知が実行されるまで自身を保持します。
自分で作ったトークンに対して直接 `bind_executor()` を呼ぶ場合、外部オブジェクトの所有権をエンジンは提供できません。
エグゼキューターが排出されるまでトークンを生存させる必要があります。外部オブジェクトの所有権をエンジンは提供できません。
これらのメソッドは公開ヘッダーに inline で定義されているため、更新された `fork()` の寿命動作を受け取るには、
既存の C++ 利用側を再コンパイルする必要があります。
`CancelToken` オブジェクトのレイアウトは 0.11.x とバイナリ互換です。
```cpp
class CancelToken {
public:
    void cancel() noexcept;                            // request cancellation
    bool is_cancelled() const noexcept;                // polling read

    std::shared_ptr<CancelToken> fork();                // v0.4: child token
    void bind_executor(asio::any_io_executor ex);
    asio::cancellation_slot slot() noexcept;
};
```

#### 階層キャンセル (v0.4 `fork()`)
各子トークンは独自の `cancellation_signal` を持ち、親の
`cancel()` は生存中のすべての子へ連鎖します。これは v0.3.x の
`add_cancel_hook` リスト (非推奨、v1.0 で削除) に代わる構造的な仕組みです。
同時にネストしたスコープ、つまり各ワーカーが同時に `provider.complete(params)` を呼ぶ
マルチ Send ファンアウトでは、各ワーカーが
`fork()` を 1 回だけ行い、互いのスロットを上書きしません。
```cpp
// Caller side: one parent token, fan it out across N concurrent runs.
auto parent = std::make_shared<neograph::graph::CancelToken>();

RunConfig cfg_a; cfg_a.thread_id = "user-1"; cfg_a.cancel_token = parent;
RunConfig cfg_b; cfg_b.thread_id = "user-2"; cfg_b.cancel_token = parent;

auto fut_a = std::async(std::launch::async, [&] { return engine->run(cfg_a); });
auto fut_b = std::async(std::launch::async, [&] { return engine->run(cfg_b); });

// User hits stop in the UI:
parent->cancel();   // cascades to every fork() child, every run aborts

// Inside a node — pass the child to provider.complete so the HTTP
// socket aborts on parent cancel without you doing any wiring:
asio::awaitable<NodeOutput> run(NodeInput in) override {
    CompletionParams params;
    params.messages    = in.state.get_messages();
    params.cancel_token = in.ctx.cancel_token;   // engine forks for you
    auto reply = co_await provider_->complete_async(params);
    NodeOutput out;
    /* ... */
    co_return out;
}
```

| メソッド | 説明 |
|--------|-------------|
| `cancel()` | 冪等でスレッドセーフ。ポーリングフラグを設定し、バインドされたエグゼキューターで asio の cancellation_signal を発行し、`fork()` 経由ですべての生存中の子へ連鎖 |
| `is_cancelled()` | ロックフリーのポーリング読み取り |
| `fork()` | **v0.4 PR 3。** 子の shared_ptr を返します。親の cancel() は連鎖し、fork() 時点ですでに親がキャンセル済みなら競合なしで子もキャンセル済みになります |
| `bind_executor(ex)` | シグナル発行を処理するエグゼキューターをエンジン内部でバインド |
| `slot()` | `co_spawn` 時に `bind_cancellation_slot` へ渡す asio `cancellation_slot` |
### RunResult
グラフ実行が完了または中断した後に返る結果です。
```cpp
struct RunResult {
    json        output;                          // Final serialized state
    bool        interrupted       = false;       // True if execution was paused (HITL)
    std::string interrupt_node;                  // Node that caused the interrupt
    json        interrupt_value;                 // Value associated with the interrupt
    std::string checkpoint_id;                   // ID of the last checkpoint saved
    std::vector<std::string> execution_trace;    // Ordered list of executed node names

    bool max_steps_exhausted() const noexcept;    // Limit stopped runnable work
    RunStatus status() const noexcept;            // Completed, Interrupted, or StepLimit

    template <typename T> T channel(const std::string& name) const;
    template <typename T> T channel(const ChannelKey<T>& key) const;
    template <typename T>
    std::optional<T> try_channel(const ChannelKey<T>& key) const;
};
```

| フィールド | 型 | 説明 |
|-------|------|-------------|
| `output` | `json` | 全チャネルの最終シリアライズ状態 |
| `interrupted` | `bool` | 中断 (HITL) により実行が一時停止した場合は `true` |
| `interrupt_node` | `std::string` | 中断を発生させたノードの名前 |
| `interrupt_value` | `json` | 中断に関連する理由またはペイロード |
| `checkpoint_id` | `std::string` | 最後に保存したチェックポイントの UUID |
| `execution_trace` | `std::vector<std::string>` | 実行順に並んだノード名の一覧 |
`max_steps_exhausted()` は、実行可能な作業が残っている状態でステップ上限に達して実行が停止した場合にのみ `true` を返します。
許可された最後のステップで `__end__` に到達したグラフは
`false` を返します。
`status()` は `RunStatus::Completed`、`RunStatus::Interrupted`、または
`RunStatus::StepLimit` のいずれかを返します。公開 `RunResult` のデータレイアウトは変わりません。
`ChannelKey<T>` は再利用可能なチャネル名を、期待する C++ 型に結び付けます:
```cpp
inline const ChannelKey<std::string> Answer{"answer"};

auto answer = result.channel(Answer);
if (auto optional = result.try_channel(Answer)) {
    std::cout << *optional << '\n';
}
```

### GraphEngine
メインのエンジンクラスです。新しいコードでは JSON 定義に `build_strict()` を使ってください。
無効なトポロジーをノード作成前に拒否します。解析、検証、検査、変換を別工程に分ける必要がある場合は `link()` と
`ValidatedTopology` を使います。寛容な `build()`、`CompiledGraph` の link オーバーロード、`compile()`、
構築後の setter は互換性のための経路として残っています。
```cpp
class GraphEngine {
public:
    // ---- Construction ----

    static std::unique_ptr<GraphEngine> build(
        const json& definition, EngineConfig config);
    static std::unique_ptr<GraphEngine> build(
        const json& definition, EngineConfig config, EngineResources resources);

    static std::unique_ptr<GraphEngine> build_strict(
        const json& definition, EngineConfig config);
    static std::unique_ptr<GraphEngine> build_strict(
        const json& definition, EngineConfig config, EngineResources resources);

    static std::unique_ptr<GraphEngine> link(
        ValidatedTopology topology, EngineConfig config = {});
    static std::unique_ptr<GraphEngine> link(
        ValidatedTopology topology, EngineConfig config, EngineResources resources);

    static std::unique_ptr<GraphEngine> link(
        CompiledGraph graph, EngineConfig config = {});
    static std::unique_ptr<GraphEngine> link(
        CompiledGraph graph, EngineConfig config, EngineResources resources);

    static std::unique_ptr<GraphEngine> compile( // compatibility facade
        const json& definition, const NodeContext& default_context,
        std::shared_ptr<CheckpointStore> store = nullptr);

    // ---- Execution (sync) ----

    RunResult run(const RunConfig& config);

    RunResult run_stream(const RunConfig& config,
                         const GraphStreamCallback& cb);

    RunResult resume(const std::string& thread_id,
                     const json& resume_value = json(),
                     const GraphStreamCallback& cb = nullptr);

    // ---- Execution (async, 3.0) ----

    asio::awaitable<RunResult> run_async(const RunConfig& config);

    asio::awaitable<RunResult> run_stream_async(
        const RunConfig& config, const GraphStreamCallback& cb);

    asio::awaitable<RunResult> resume_async(
        const std::string& thread_id,
        const json& resume_value = json(),
        const GraphStreamCallback& cb = nullptr);

    // ---- State Inspection & Manipulation ----

    GraphAdmin admin(); // borrowed facade; must not outlive this engine

    std::optional<json> get_state(const std::string& thread_id) const;

    std::vector<Checkpoint> get_state_history(const std::string& thread_id,
                                              int limit = 100) const;

    void update_state(const std::string& thread_id,
                      const json& channel_writes,
                      const std::string& as_node = "");

    std::string fork(const std::string& source_thread_id,
                     const std::string& new_thread_id,
                     const std::string& checkpoint_id = "");

    // ---- Compatibility configuration (prefer EngineConfig/EngineResources) ----

    void own_tools(std::vector<std::unique_ptr<Tool>> tools);
    void set_checkpoint_store(std::shared_ptr<CheckpointStore> store);
    void set_store(std::shared_ptr<Store> store);
    std::shared_ptr<Store> get_store() const;
    void set_retry_policy(const RetryPolicy& policy);
    void set_node_retry_policy(const std::string& node_name, const RetryPolicy& policy);

    // Fan-out worker pool. n==1 keeps the engine on the caller's
    // executor (no engine-owned thread_pool); n>=2 installs an
    // owned `asio::thread_pool` of size n. build() defaults to
    // n==1 — prefer EngineConfig::worker_count to opt into
    // real parallel fan-out. Throws `std::logic_error` if called
    // while a run is in flight (Round 3 guard — `active_runs_`
    // counter prevents tasks queued on the old pool from being
    // silently dropped on swap).
    void set_worker_count(std::size_t n);

    // Compatibility convenience: set_worker_count(hardware_concurrency()).
    void set_worker_count_auto();

    // Per-node result caching. Disabled by default; opt in per node.
    void set_node_cache_enabled(const std::string& node_name, bool enabled);
    void clear_node_cache();
    const NodeCache& node_cache() const;

    const std::string& get_graph_name() const;
};
```

#### `build` と `link`
```cpp
EngineConfig config;
config.node_context.provider = provider;
config.checkpoint_store = checkpoint_store;
config.store = store;
config.worker_count = 4;
config.cached_nodes.insert("retrieve");

std::vector<std::unique_ptr<Tool>> owned_tools;
owned_tools.push_back(std::make_unique<SearchTool>());
auto registry = std::make_shared<GraphRegistry>();
// Register engine-local reducers, conditions, or node types on registry.

EngineResources resources{
    .tools = ToolSet(std::move(owned_tools)),
    .registry = registry,
};

auto engine = GraphEngine::build(definition, std::move(config),
                                 std::move(resources));
```

`build()` はコンパイル、検証、リンク、設定を行い、完全に構成されたエンジンを返します。
`link()` は `CompiledGraph` を move で受け取り、実行時設定を適用します。手動でコンパイルする呼び出し側は、
必要とするソースから IR への往復検証を自分で行う責任があります。
#### `compile` (互換性)
```cpp
static std::unique_ptr<GraphEngine> compile(
    const json& definition,
    const NodeContext& default_context,
    std::shared_ptr<CheckpointStore> store = nullptr);
```

JSON 定義からグラフをコンパイルし、実行可能なエンジンを返します。
元のシグネチャを維持し、`build()` に委譲します。新しいコードでストア、再試行ポリシー、ワーカー設定、
キャッシュ、ツールゲートが必要な場合は `EngineConfig` を優先してください。
| パラメーター | 型 | 説明 |
|-----------|------|-------------|
| `definition` | `const json&` | JSON 形式のグラフ定義 (下記参照) |
| `default_context` | `const NodeContext&` | すべてのノードに注入するデフォルトコンテキスト |
| `store` | `std::shared_ptr<CheckpointStore>` | 任意の永続化用チェックポイントストア |
**グラフ定義 JSON スキーマ:**
```json
{
  "name": "my_graph",
  "channels": {
    "messages": {"reducer": "append"},
    "status": {"reducer": "overwrite", "initial": "idle"}
  },
  "nodes": {
    "llm": {"type": "llm_call"},
    "tools": {"type": "tool_dispatch"}
  },
  "edges": [
    {"from": "__start__", "to": "llm"},
    {"from": "tools", "to": "llm"}
  ],
  "conditional_edges": [
    {
      "from": "llm",
      "condition": "has_tool_calls",
      "routes": {"yes": "tools", "no": "__end__"}
    }
  ],
  "interrupt_before": [],
  "interrupt_after": ["tools"]
}
```

##### バリアノード (AND-join のオプトイン)
ノード宣言には、そのノードだけで AND-join の意味論を有効にする `barrier` フィールドを含められます。
デフォルトのシグナルディスパッチモデルでは、上流のいずれかがルーティングするたびにノードが発火します。
そのため、長さの異なる経路による非対称な直列 fan-in では join ノードが二重に発火します。
バリアは、記載されたすべての上流が少なくとも 1 回 (任意の数のスーパーステップをまたいで) シグナルを送るまでノードを待機させます。
```json
"join": {
  "type": "my_join",
  "barrier": {"wait_for": ["a", "s2"]}
}
```

`a` と `s2` の両方がシグナルを送ったときに 1 回発火します。バリアを通るループでは、
各ラウンドで新しいシグナルを集めるため、発火後に状態がリセットされます。
**永続化:** `CHECKPOINT_SCHEMA_VERSION = 2` なので、バリアアキュムレーターは各チェックポイントに保存され (`Checkpoint::barrier_state`、
`map<string, set<string>>`)、再開時に復元されます。蓄積途中で発生した中断も安全です。
部分的な上流集合が一時停止を越えて保持され、残りのシグナルが届くとバリアが発火します。
v1 の blob は空の `barrier_state` でデシリアライズされ、保存済みチェックポイントでは v2 より前の動作と一致します。
#### `run`
```cpp
RunResult run(const RunConfig& config);
```

グラフを同期的に (ブロッキングして) 実行します。`START_NODE` から開始し、`END_NODE` に到達するか
`max_steps` を超えるまでエッジをたどります。
#### `run_stream`
```cpp
RunResult run_stream(const RunConfig& config,
                     const GraphStreamCallback& cb);
```

ストリーミングイベント付きでグラフを実行します。コールバック `cb` は `config.stream_mode` フィルターに一致する各イベントで呼び出されます。
#### `resume`
```cpp
RunResult resume(const std::string& thread_id,
                 const json& resume_value = json(),
                 const GraphStreamCallback& cb = nullptr);
```

以前に中断されたチェックポイントから実行を再開します (Human-in-the-Loop)。
| パラメーター | 型 | 説明 |
|-----------|------|-------------|
| `thread_id` | `std::string` | 再開対象のスレッド ID |
| `resume_value` | `json` | 再開前に注入する任意の値 (例: 人間の承認) |
| `cb` | `GraphStreamCallback` | 任意のストリーミングコールバック。非ストリーミング再開では `nullptr` |
#### `get_state`
```cpp
std::optional<json> get_state(const std::string& thread_id) const;
```

スレッドの最新状態を返します。チェックポイントがなければ `std::nullopt` を返します。
#### `get_state_history`
```cpp
std::vector<Checkpoint> get_state_history(const std::string& thread_id,
                                          int limit = 100) const;
```

スレッドのチェックポイント履歴をタイムスタンプ順 (新しいものから) で返します。
#### `update_state`
```cpp
void update_state(const std::string& thread_id,
                  const json& channel_writes,
                  const std::string& as_node = "");

void update_state_writes(const std::string& thread_id,
                         const std::vector<ChannelWrite>& channel_writes,
                         const std::string& as_node = "");
```

チャネル write を適用してスレッド状態を手動更新します。JSON object 形式はチャネル名ごとに
reducer write を適用します。`ChannelWrite` vector 形式は write 順序と明示的な overwrite
mode を保持します。どちらも更新済み状態で新しい checkpoint を作成します。
| パラメーター | 型 | 説明 |
|-----------|------|-------------|
| `thread_id` | `std::string` | 対象スレッド |
| `channel_writes` | `json` | 適用する `{channel: value}` ペアのオブジェクト |
| `as_node` | `std::string` | 任意。特定ノードからの書き込みとして記録 |
#### `fork`
```cpp
std::string fork(const std::string& source_thread_id,
                 const std::string& new_thread_id,
                 const std::string& checkpoint_id = "");
```

スレッドの状態を新しいスレッドとしてコピーします。会話を分岐したり、what-if シナリオを作ったりするのに便利です。
| パラメーター | 型 | 説明 |
|-----------|------|-------------|
| `source_thread_id` | `std::string` | コピー元のスレッド |
| `new_thread_id` | `std::string` | 新しいスレッド識別子 |
| `checkpoint_id` | `std::string` | 任意。特定のチェックポイントから fork (デフォルト: 最新) |
**戻り値:** 新しく fork した状態のチェックポイント ID。
#### `own_tools`
```cpp
void own_tools(std::vector<std::unique_ptr<Tool>> tools);
```

ツールの所有権をエンジンへ移します。エンジンはツールを保持し、すべての `NodeContext.tools` 参照について
生ポインターをエンジンの寿命中有効に保ちます。
#### `set_checkpoint_store`
```cpp
void set_checkpoint_store(std::shared_ptr<CheckpointStore> store);
```

チェックポイントストアを接続します。`resume()`、`get_state()`、`fork()` およびすべての状態検査メソッドに必要です。
#### `set_store`
```cpp
void set_store(std::shared_ptr<Store> store);
```

クロススレッド共有メモリストアを接続します ([Store](#9-store) 参照)。
#### `get_store`
```cpp
std::shared_ptr<Store> get_store() const;
```

接続された共有メモリストアを返します。設定されていなければ `nullptr` です。
#### `set_retry_policy`
```cpp
void set_retry_policy(const RetryPolicy& policy);
```

すべてのノードのデフォルト再試行ポリシーを設定します。固有のポリシーがないノードはこれを使用します。
#### `set_node_retry_policy`
```cpp
void set_node_retry_policy(const std::string& node_name, const RetryPolicy& policy);
```

特定ノードの再試行ポリシーを設定し、デフォルトを上書きします。
#### `get_graph_name`
```cpp
const std::string& get_graph_name() const;
```

定義で指定されたグラフ名を返します。
---
<a id="7b-engine-internals"></a>
## 7b. エンジン内部
`GraphEngine` は 4 つの目的別クラスに委譲する薄いオーケストレーターです。通常、利用者が直接触れることはありません。これらは `GraphEngine::build()` (または互換ファサード `compile()`) 内で生成され、`execute_graph()` から駆動されますが、高度な利用者が JSON なしで構築したり、カスタムチェックポイントフローを駆動したり、テストで部品をスタブ化したりできるよう公開されています。
| クラス | ヘッダー | 役割 |
|-------|--------|----------------|
| [`GraphCompiler`](#graphcompiler) | `<neograph/graph/compiler.h>` | JSON を `CompiledGraph` に解析 |
| [`Scheduler`](#scheduler) | `<neograph/graph/scheduler.h>` | ルーティング判断 (シグナルディスパッチ + バリア) |
| [`CheckpointCoordinator`](#checkpointcoordinator) | `<neograph/graph/coordinator.h>` | 実行ごとのチェックポイント寿命管理 |
| [`NodeExecutor`](#nodeexecutor) | `<neograph/graph/executor.h>` | 再試行、並列ファンアウト、Send ディスパッチ |
### GraphCompiler
**ヘッダー:** `<neograph/graph/compiler.h>`
純粋な JSON → 値型の変換です。実行時依存性はなく、生成された `CompiledGraph` は検査したり、テストで手作業により構築したりできる move 可能なバンドルです。
```cpp
namespace neograph::graph {

struct ChannelDef {
    std::string  name;
    ReducerType  type = ReducerType::OVERWRITE;
    std::string  reducer_name = "overwrite";
    json         initial_value;
};

struct CompiledGraph {
    std::string name;
    std::vector<ChannelDef> channel_defs;
    std::map<std::string, std::unique_ptr<GraphNode>> nodes;
    std::vector<Edge> edges;
    std::vector<ConditionalEdge> conditional_edges;
    BarrierSpecs barrier_specs;
    std::set<std::string> interrupt_before;
    std::set<std::string> interrupt_after;
    std::optional<RetryPolicy> retry_policy;
};

class GraphCompiler {
public:
    static TopologySpec parse(const json& definition);
    static CompiledGraph link(TopologySpec topology,
                              const NodeContext& default_context);
    static CompiledGraph compile(const json& definition,
                                 const NodeContext& default_context);
};

} // namespace neograph::graph
```

`GraphCompiler::parse()` はノードを構築せずに `TopologySpec` を生成します。
`GraphValidator::validate()` は構造化された診断情報を返し、`GraphValidator::require_valid()` は
`ValidatedTopology` を返すか `std::runtime_error` を送出します。
実行時ノードを解決してインスタンス化するのは `GraphCompiler::link()` だけです。
`compile()` は parse と link を組み合わせる互換経路であり、`GraphEngine::build()` は寛容な警告動作を維持します。
新しいコードでは `GraphEngine::build_strict()` を使って境界全体を強制できます:
```cpp
auto spec = GraphCompiler::parse(definition);
auto validated = GraphValidator::require_valid(std::move(spec));
auto engine = GraphEngine::link(std::move(validated), config, resources);
```

### Scheduler
**ヘッダー:** `<neograph/graph/scheduler.h>`
グラフトポロジーを所有し、前のスーパーステップが発行したルーティングシグナルから各ステップの ready 集合を計算します。
スレッド、チェックポイント、再試行、HITL の知識は持ちません。これらはエンジン側に残ります。
```cpp
namespace neograph::graph {

struct StepRouting {
    std::string node_name;
    std::optional<std::string> command_goto;
};

struct NextStepPlan {
    std::vector<std::string> ready;
    bool hit_end = false;
    std::optional<std::string> winning_command_goto;
};

using BarrierSpecs = std::map<std::string, std::set<std::string>>;
using BarrierState = std::map<std::string, std::set<std::string>>;

class Scheduler {
public:
    Scheduler(const std::vector<Edge>& edges,
              const std::vector<ConditionalEdge>& conditional_edges,
              BarrierSpecs barrier_specs = {});

    std::vector<std::string> plan_start_step() const;

    NextStepPlan plan_next_step(
        const std::vector<std::string>& just_ran,
        const std::vector<NodeResult>& results,
        const GraphState& state,
        BarrierState& barrier_state) const;

    std::vector<std::string> resolve_next_nodes(
        const std::string& current,
        const GraphState& state) const;

    const BarrierSpecs& barrier_specs() const;
};

} // namespace neograph::graph
```

**意味論:**
- **シグナルディスパッチ:** ステップ S のノードが明示的にルーティングした場合に限り、ノードはスーパーステップ S+1 で ready になります。
  通常のエッジ、条件エッジの分岐、`Command::goto_node`、または Send が対象です。静的な predecessor マップはありません。
  それでは XOR ルーティングと AND fan-in を混同してしまうためです。
- **対応の不変条件:** 呼び出し側は `just_ran` と `results` を `just_ran[i] ↔ results[i]` の対応で渡す必要があります。
  2 引数オーバーロードの型シグネチャによって保証され、呼び出し側が対応をずらせないようになっています。
- **バリア:** `"barrier": {"wait_for": [...]}` を宣言したノードは、一覧にあるすべての上流がシグナルを送るまで待機します。
  シグナルは可変の `BarrierState` マップを通じてスーパーステップをまたいで蓄積されます。
  発火するとエントリがリセットされるため、バリアを通るループも正しく動きます。
### CheckpointCoordinator
**ヘッダー:** `<neograph/graph/coordinator.h>`
`(CheckpointStore, thread_id)` に対する実行単位のラッパーです。ストアが null または thread_id が空なら、各メソッドは
ストアが null または thread_id が空なら安全な no-op になるため、呼び出し側でガードする必要はありません。
```cpp
namespace neograph::graph {

struct ResumeContext {
    bool have_cp = false;
    std::string checkpoint_id;
    json channel_values;
    int start_step = 0;  // Phase-adjusted
    CheckpointPhase phase = CheckpointPhase::Completed;
    std::vector<std::string> next_nodes;
    std::unordered_map<std::string, NodeResult> replay_results;
    BarrierState barrier_state;
};

class CheckpointCoordinator {
public:
    CheckpointCoordinator(std::shared_ptr<CheckpointStore> store,
                          std::string thread_id);

    bool enabled() const noexcept;

    std::string save_super_step(
        const GraphState& state,
        const std::string& current_node,
        const std::vector<std::string>& next_nodes,
        CheckpointPhase phase,
        int step,
        const std::string& parent_id,
        const BarrierState& barrier_state) const;

    ResumeContext load_for_resume() const;

    void record_pending_write(
        const std::string& parent_cp_id,
        const std::string& task_id,
        const std::string& task_path,
        const std::string& node_name,
        const NodeResult& nr,
        int step) const;

    void clear_pending_writes(const std::string& parent_cp_id) const;
};

} // namespace neograph::graph
```

**フェーズ対応のステップオフセット:** `load_for_resume()` は最新チェックポイントの `interrupt_phase` を読み、
`start_step` を適切に設定します。
`Before` / `NodeInterrupt` は `cp.step` で再入し、`After` / `Completed` /
`Updated` は +1 進めます。エンジンの再開経路がこのロジックを重複して実行することはありません。
### NodeExecutor
**ヘッダー:** `<neograph/graph/executor.h>`
スーパーステップごとのノード呼び出しを所有します。再試行ループ、リプレイ検索、保留書き込みの記録、
保留書き込みの記録、
`asio::experimental::make_parallel_group`、および Send dispatch です。3.0
同期版 `run_one` / `run_parallel` / `run_sends` の対は削除され、呼び出し側は `_async` 版を使います。
呼び出し側は `_async` 版を使います。
```cpp
namespace neograph::graph {

class NodeExecutor {
public:
    using RetryPolicyLookup = std::function<RetryPolicy(const std::string&)>;

    NodeExecutor(
        const std::map<std::string, std::unique_ptr<GraphNode>>& nodes,
        const std::vector<ChannelDef>& channel_defs,
        RetryPolicyLookup retry_policy_for,
        asio::thread_pool* fan_out_pool = nullptr);

    asio::awaitable<NodeResult> run_one_async(
        const std::string& node_name, int step,
        GraphState& state,
        const std::unordered_map<std::string, NodeResult>& replay,
        CheckpointCoordinator& coord,
        const std::string& parent_cp_id,
        const BarrierState& barrier_state,
        std::vector<std::string>& trace,
        const GraphStreamCallback& cb, StreamMode stream_mode);

    asio::awaitable<std::vector<NodeResult>> run_parallel_async(
        const std::vector<std::string>& ready, int step,
        GraphState& state,
        const std::unordered_map<std::string, NodeResult>& replay,
        CheckpointCoordinator& coord,
        const std::string& parent_cp_id,
        const BarrierState& barrier_state,
        std::vector<std::string>& trace,
        const GraphStreamCallback& cb, StreamMode stream_mode);

    asio::awaitable<void> run_sends_async(
        const std::vector<Send>& sends, int step,
        GraphState& state,
        const std::unordered_map<std::string, NodeResult>& replay,
        CheckpointCoordinator& coord,
        const std::string& parent_cp_id,
        std::vector<std::string>& trace,
        const GraphStreamCallback& cb, StreamMode stream_mode);

    asio::awaitable<NodeResult> execute_node_with_retry_async(
        const std::string& node_name,
        GraphState& state,
        const GraphStreamCallback& cb, StreamMode stream_mode);
};

} // namespace neograph::graph
```

**不変条件:**
- `run_one_async` と `run_parallel_async` はどちらも
  割り込み元ノードに限定した `phase=NodeInterrupt` チェックポイントを保存します。
  その後 `NodeInterrupt` を再送出するため、再開時はそのノードだけに再入します。
  兄弟ノードの書き込みはすでに `pending_writes` にあり、マップ経由でリプレイされます。
- `run_parallel_async` は `ready` の順に書き込みと `Command.updates` を適用するため、
  後続の Scheduler 呼び出しでも `ready[i] ↔ results[i]` の対応が保たれます。
- `run_sends_async`: 単一 Send は共有状態で再試行付きで実行します。
  複数 Send では対象ごとに分離状態コピー (初期化 + 復元 + 入力適用) を作り、再試行なしで実行します。
  これは 3.0 より前の意味論を保ちます。
- 任意の `fan_out_pool` が並列分岐のディスパッチ先を決めます。null の場合、分岐は
  `co_await asio::this_coro::executor` 上で実行されます。単一スレッドの非同期呼び出し側には十分ですが、
  CPU バウンドのファンアウトは直列化されます。null でなければ `run_parallel_async` と
  複数 Send 分岐は `pool->get_executor()` に `co_spawn` され、実際のスレッド並列性を得ます。
  `GraphEngine::set_worker_count(N)` は同期 `run()` 呼び出し側向けにプールを設定します。
- `execute_node_with_retry_async` は内部の再試行ループです。バックオフには `asio::steady_timer` を使うため、
  再試行待ちの間もエグゼキューターは停止しません。
---
<a id="8-checkpoint"></a>
## 8. チェックポイント
**ヘッダー:** `<neograph/graph/checkpoint.h>`
**名前空間:** `neograph::graph`
チェックポイントはグラフ実行状態を保存・復元することで、永続化、タイムトラベルデバッグ、Human-in-the-Loop ワークフローを可能にします。
### Checkpoint (struct)
時点におけるグラフ実行状態をシリアライズしたスナップショットです。
```cpp
struct Checkpoint {
    std::string id;                // UUID v4
    std::string thread_id;         // Conversation/session identifier
    json        channel_values;    // Serialized channel data
    json        channel_versions;  // Per-channel version counters
    std::string parent_id;         // Previous checkpoint ID (for time-travel chain)
    std::string current_node;      // Node that was active at checkpoint time
    std::vector<std::string> next_nodes;  // Nodes to execute on resume
    CheckpointPhase interrupt_phase;  // Before | After | Completed | NodeInterrupt | Updated
    std::map<std::string, std::set<std::string>> barrier_state;  // v2+: in-flight barrier accumulators
    json        metadata;          // User-defined metadata
    int64_t     step;              // Super-step number
    int64_t     timestamp;         // Unix epoch milliseconds
    std::uint32_t schema_version = CHECKPOINT_SCHEMA_VERSION;  // Layout version

    static std::string generate_id();  // Generate UUID v4
};

// Wire-stable schema version. Bump on layout-incompatible changes.
// v2 added `barrier_state`; v3 records pending-write mode support.
// Typed `uint32_t`: schema versions are non-negative wire values.
constexpr std::uint32_t CHECKPOINT_SCHEMA_VERSION = 3;
```

| フィールド | 型 | 説明 |
|-------|------|-------------|
| `id` | `std::string` | 一意の識別子 (UUID v4) |
| `thread_id` | `std::string` | 会話/セッションごとにチェックポイントをまとめます |
| `channel_values` | `json` | 全チャネルのシリアライズ済み状態 |
| `channel_versions` | `json` | 各チャネルのバージョンカウンター |
| `parent_id` | `std::string` | 直前のチェックポイント ID (タイムトラベル用の連結リストを形成します) |
| `current_node` | `std::string` | チェックポイント取得時に実行中だったノード |
| `next_nodes` | `std::vector<std::string>` | 次のスーパーステップに予定されている全ノード (`resume()` が使用)。signal dispatch では、1 つのスーパーステップ後に複数ノードが同時に ready になることがあります (並列 fan-out や条件分岐の同時活性化)。その全てを永続化する必要があり、単一ノードだけを保存するとクラッシュをまたいで兄弟ノードが黙って失われます |
| `interrupt_phase` | `CheckpointPhase` | 列挙値: `Before` (`interrupt_before` が発火)、`After` (`interrupt_after` が発火)、`Completed` (通常のスーパーステップ周期)、`NodeInterrupt` (実行中にノードが `NodeInterrupt` を投げた)、`Updated` (外部 `update_state()` 注入)。`to_string()` と `parse_checkpoint_phase()` が安定した wire/log エンコードを提供します |
| `barrier_state` | `map<string, set<string>>` | これまで signal した上流を barrier ごとに蓄積したもの。エントリは処理中 (まだ発火していない) barrier にだけ存在し、Scheduler は barrier 発火時にそのエントリを消します。形状は `scheduler.h` の `BarrierState` と一致します。schema v2 以降に存在します。v1 blob は空 map としてデシリアライズされ、v2 以前の動作と一致します |
| `metadata` | `json` | 任意のユーザー定義データ |
| `step` | `int64_t` | スーパーステップカウンター |
| `timestamp` | `int64_t` | Unix epoch ミリ秒での作成時刻 |
| `schema_version` | `std::uint32_t` | wire 上のレイアウトバージョン (`CHECKPOINT_SCHEMA_VERSION`、現在は `3` を参照)。Round 5 で `int` から固定幅の unsigned へ広げました。schema version は非負であり、ディスクへ永続化して JSON 経由で往復する値に、プラットフォームで幅が変わる `int` を使うのは不適切だったためです。永続化する `CheckpointStore` 実装はこれをシリアライズし、デシリアライズした blob の `0` は「バージョン付け前」(例: フィールドが存在しなかった。移行は呼び出し側の責任) と扱うべきです |
### CheckpointStore
チェックポイント永続化の抽象インターフェースです。データベース、ファイルシステム、その他のバックエンドに保存するには実装します。
データベース、ファイルシステム、その他のバックエンドにチェックポイントを保存するためのインターフェースです。
> **カスタムストアを書く場合:** 新しい実装では、適用できる最小の機能を実装してください。
> 適用する capability は `CheckpointStoreCore`、必要に応じて
> `AsyncCheckpointStore` や `PendingWritesCheckpointStore` を必要に応じて実装し、
> `adapt_checkpoint_store()` を通して渡します。既存の `CheckpointStore` は互換性契約として残ります。
> async のデフォルトは sync メソッドを呼ぶため、sync 専用バックエンドも有効ですが、その async 呼び出しはブロッキングです。
> [`ASYNC_GUIDE.md` §9.4](ASYNC_GUIDE.md#94-checkpointstore) を参照してください。
```cpp
class CheckpointStore {
public:
    virtual ~CheckpointStore() = default;

    // ── Sync core (5 virtuals, non-pure with bridge defaults) ──────
    virtual void save(const Checkpoint& cp);
    virtual std::optional<Checkpoint> load_latest(const std::string& thread_id);
    virtual std::optional<Checkpoint> load_by_id(const std::string& id);
    virtual std::vector<Checkpoint>   list(const std::string& thread_id,
                                           int limit = 100);
    virtual void delete_thread(const std::string& thread_id);

    // ── Async peers (5 virtuals, default co_return the sync call) ──
    virtual asio::awaitable<void> save_async(const Checkpoint& cp);
    virtual asio::awaitable<std::optional<Checkpoint>>
        load_latest_async(const std::string& thread_id);
    virtual asio::awaitable<std::optional<Checkpoint>>
        load_by_id_async(const std::string& id);
    virtual asio::awaitable<std::vector<Checkpoint>>
        list_async(const std::string& thread_id, int limit = 100);
    virtual asio::awaitable<void>
        delete_thread_async(const std::string& thread_id);

    // ── Pending writes — fine-grained super-step progress log ──────
    //
    // Default no-ops: backends that don't support per-node durable
    // writes fall back to "full super-step replay" on resume.
    virtual void put_writes(const std::string& thread_id,
                            const std::string& parent_checkpoint_id,
                            const PendingWrite& write) {}
    virtual std::vector<PendingWrite> get_writes(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id) { return {}; }
    virtual void clear_writes(const std::string& thread_id,
                              const std::string& parent_checkpoint_id) {}

    // ── Async pending-writes peers (default-bridge to sync) ────────
    virtual asio::awaitable<void> put_writes_async(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id,
        const PendingWrite& write);
    virtual asio::awaitable<std::vector<PendingWrite>> get_writes_async(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id);
    virtual asio::awaitable<void> clear_writes_async(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id);
};
```

| メソッド | 説明 |
|--------|-------------|
| `save(cp)` / `save_async(cp)` | チェックポイントを永続化します。エンジンはスーパーステップごとに 1 つ書き込みます。 |
| `load_latest(thread_id)` / `_async` | スレッドの最新チェックポイントを読み込みます。 |
| `load_by_id(id)` / `_async` | UUID で指定したチェックポイントを読み込みます (タイムトラベル)。 |
| `list(thread_id, limit)` / `_async` | スレッドのチェックポイントを新しい順に最大 `limit` 件列挙します。 |
| `delete_thread(thread_id)` / `_async` | スレッドのすべてのチェックポイントを削除します。 |
| `put_writes(thread_id, parent_cp, write)` / `_async` | スーパーステップ途中で成功したノード実行を記録します。エンジンはノードが戻った直後、かつその書き込みを GraphState に適用する *前* にこれを呼び出します。デフォルトは no-op です。 |
| `get_writes(thread_id, parent_cp)` / `_async` | 親チェックポイントに紐づく保留中の書き込みを読み込みます。エンジンは resume 時にこれを呼び出し、完了済みタスクをスキップします。デフォルトは空です。 |
| `clear_writes(thread_id, parent_cp)` / `_async` | 後続スーパーステップのチェックポイントが永続保存された後、保留中の書き込みを破棄します。デフォルトは no-op です。 |
### InMemoryCheckpointStore
テストや単一プロセスのアプリケーションに適した、スレッドセーフなインメモリ実装です。
```cpp
class InMemoryCheckpointStore : public CheckpointStore {
public:
    void save(const Checkpoint& cp) override;
    std::optional<Checkpoint> load_latest(const std::string& thread_id) override;
    std::optional<Checkpoint> load_by_id(const std::string& id) override;
    std::vector<Checkpoint> list(const std::string& thread_id,
                                  int limit = 100) override;
    void delete_thread(const std::string& thread_id) override;

    size_t size() const;  // Total number of stored checkpoints
};
```

---
## 9. Store
**ヘッダー:** `<neograph/graph/store.h>`
**名前空間:** `neograph::graph`
スレッドをまたいで共有するメモリストアです。名前空間付きキー値ストレージを提供し、スレッドとグラフ実行をまたいで永続します。長期的なユーザー設定、共有ナレッジベース、エージェントメモリなどに使えます。
### Namespace
文字列ベクターで表す階層パスです。
```cpp
using Namespace = std::vector<std::string>;
```

例: `{"users", "user123", "preferences"}` は `users/user123/preferences` というパスを表します。
### StoreItem
ストア内の 1 件の項目です。
```cpp
struct StoreItem {
    Namespace   ns;          // Namespace path
    std::string key;         // Item key within the namespace
    json        value;       // Stored value
    int64_t     created_at;  // Creation timestamp (Unix epoch millis)
    int64_t     updated_at;  // Last update timestamp (Unix epoch millis)
};
```

<a id="store-abstract"></a>
### Store (抽象)
クロススレッド共有メモリの抽象インターフェースです。
```cpp
class Store {
public:
    virtual ~Store() = default;

    // Put a value (create or update)
    virtual void put(const Namespace& ns, const std::string& key,
                     const json& value) = 0;

    // Get a single item
    virtual std::optional<StoreItem> get(const Namespace& ns,
                                         const std::string& key) const = 0;

    // Search items under a namespace prefix
    virtual std::vector<StoreItem> search(const Namespace& ns_prefix,
                                           int limit = 100) const = 0;

    // Delete an item
    virtual void delete_item(const Namespace& ns, const std::string& key) = 0;

    // List namespaces under a prefix
    virtual std::vector<Namespace> list_namespaces(
        const Namespace& prefix = {}) const = 0;
};
```

| メソッド | 説明 |
|--------|-------------|
| `put(ns, key, value)` | 値を挿入または更新します。項目がすでに存在する場合は `updated_at` を更新します |
| `get(ns, key)` | 単一の項目を取得します。見つからない場合は `std::nullopt` を返します |
| `search(ns_prefix, limit)` | namespace が指定 prefix で始まるすべての項目を探します |
| `delete_item(ns, key)` | store から項目を削除します |
| `list_namespaces(prefix)` | 指定 prefix で始まる一意な namespace をすべて列挙します |
### InMemoryStore
テストと単一プロセス用途向けのスレッドセーフなインメモリ実装です。
```cpp
class InMemoryStore : public Store {
public:
    void put(const Namespace& ns, const std::string& key,
             const json& value) override;
    std::optional<StoreItem> get(const Namespace& ns,
                                 const std::string& key) const override;
    std::vector<StoreItem> search(const Namespace& ns_prefix,
                                   int limit = 100) const override;
    void delete_item(const Namespace& ns, const std::string& key) override;
    std::vector<Namespace> list_namespaces(
        const Namespace& prefix = {}) const override;

    size_t size() const;  // Total number of stored items
};
```

---
## 10. Loader
**ヘッダー:** `<neograph/graph/loader.h>`
**名前空間:** `neograph::graph`
リデューサー、条件、ノード型向けのレガシーなシングルトンレジストリです。
JSON 駆動のグラフ構築では、プロセス全体のフォールバックとして残っています。新しい
コードは `EngineResources` 経由で `GraphRegistry` を渡せます。ローカルの登録が優先され、
見つからない名前はここで解決されます。
### ReducerRegistry
文字列名を `ReducerFn` 実装へ対応付けるシングルトンレジストリです。
```cpp
class ReducerRegistry {
public:
    static ReducerRegistry& instance();

    void register_reducer(const std::string& name, ReducerFn fn);
    ReducerFn get(const std::string& name) const;
    std::vector<std::string> names() const;
};
```

| メソッド | 説明 |
|--------|-------------|
| `instance()` | singleton インスタンスを返します |
| `register_reducer(name, fn)` | カスタム reducer 関数を登録します |
| `get(name)` | reducer を名前で検索します。見つからない場合は例外を投げます |
| `names()` | 登録済み reducer 名のソート済みリスト (外部ツール用の introspection) |
### ConditionRegistry
文字列名を `ConditionFn` 実装へ対応付けるシングルトンレジストリです。
```cpp
class ConditionRegistry {
public:
    static ConditionRegistry& instance();

    void register_condition(const std::string& name, ConditionFn fn);
    ConditionFn get(const std::string& name) const;
    std::vector<std::string> names() const;
};
```

| メソッド | 説明 |
|--------|-------------|
| `instance()` | singleton インスタンスを返します |
| `register_condition(name, fn)` | カスタム condition 関数を登録します |
| `get(name)` | condition を名前で検索します。見つからない場合は例外を投げます |
| `names()` | 登録済み condition 名のソート済みリスト (外部ツール用の introspection) |
### NodeFactory
JSON 設定から `GraphNode` インスタンスを作成するシングルトンファクトリーです。
```cpp
using NodeFactoryFn = std::function<std::unique_ptr<GraphNode>(
    const std::string& name,
    const json& config,
    const NodeContext& ctx)>;

class NodeFactory {
public:
    static NodeFactory& instance();

    void register_type(const std::string& type, NodeFactoryFn fn);
    void register_type(const std::string& type, NodeFactoryFn fn,
                       json config_schema);
    std::unique_ptr<GraphNode> create(const std::string& type,
                                       const std::string& name,
                                       const json& config,
                                       const NodeContext& ctx) const;
    std::vector<std::string> registered_types() const;
    json export_schema() const;
};
```

| メソッド | 説明 |
|--------|-------------|
| `instance()` | singleton インスタンスを返します |
| `register_type(type, fn)` | ノード factory を登録します。config schema のデフォルトは許容的な `{"type":"object"}` です |
| `register_type(type, fn, config_schema)` | 上と同じですが、ノードの `config` 用 JSON Schema (Draft 2020-12) を宣言します。追加的な変更であり、2 引数 overload はそのまま動作します。`export_schema()` でのみ使われ、エンジンはこれに対して config を検証しません |
| `create(type, name, config, ctx)` | 指定 type のノードを作成します。type が登録されていない場合は例外を投げます |
| `registered_types()` | 登録済みノード type 名のソート済みリスト |
| `export_schema()` | このエンジンが受け付ける topology JSON の機械可読な説明 ([Topology Schema Export](#topology-schema-export-issue-56) を参照) |
<a id="built-in-registrations"></a>
### 組み込み登録
ライブラリは次のコンポーネントをあらかじめ登録します:
**Reducers:**
| 名前 | 動作 |
|------|----------|
| `"overwrite"` | 現在値を入力値で置き換えます |
| `"append"` | 入力値を現在の配列へ追加します。入力値が配列の場合は、その要素を連結します |
**Conditions:**
| 名前 | 動作 |
|------|----------|
| `"has_tool_calls"` | `"messages"` チャネルの最後のメッセージを調べます。ツール呼び出しを含む場合は `"yes"`、それ以外は `"no"` を返します |
| `"route_channel"` | `"__route__"` チャネルを読み、その文字列値を返します。`IntentClassifierNode` と一緒に使います |
**Node types:**
| 型 | クラス | 説明 |
|------|-------|-------------|
| `"llm_call"` | `LLMCallNode` | 現在の会話状態で LLM を呼び出します |
| `"tool_dispatch"` | `ToolDispatchNode` | 最新の assistant メッセージからツール呼び出しを dispatch します |
| `"intent_classifier"` | `IntentClassifierNode` | LLM による意図分類。`config` から `prompt` と `valid_routes` を読みます |
| `"subgraph"` | `SubgraphNode` | コンパイル済みサブグラフを実行します。`config` から `input_map` と `output_map` を読みます |
<a id="topology-schema-export-issue-56"></a>
### トポロジースキーマのエクスポート (issue #56)
NeoGraph は *JSON で記述された* グラフを実行します。JSON を差し替えれば、同じエンジンが別のハーネスになります。
同じエンジンが別のハーネスになります。`NodeFactory::export_schema()` は
このエンジンのバージョンが受け付けるトポロジー JSON の正確な機械可読説明を出力します。
そのため外部ツール、特にコードを書かないビジュアルブロックエディター (プライベートな関連リポジトリ NeoGraph Studio、
issue #56) はエンジンからパレットを生成でき、同期ずれを起こしません。
同期しなくなるのを避けるためです。
**3 つのアクセス経路、1 つのドキュメント:**
| 入力元 | 方法 |
|------|-----|
| C++ | `neograph::graph::NodeFactory::instance().export_schema()` → `json` |
| CLI | `./example_export_schema > schema.json` (`examples/52_export_schema.cpp`) |
| Python | `neograph_engine.export_schema()` → `dict` |
**ドキュメント形状:**
```jsonc
{
  "neograph_version": "0.9.0",
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "topology":   { /* JSON Schema for the top-level envelope:
                     name, channels, nodes (type + config + barrier),
                     edges, conditional_edges, interrupt_before,
                     interrupt_after, retry_policy */ },
  "node_types": { "<type>": { /* config JSON Schema */ }, ... },
  "reducers":   ["append", "overwrite", ...],
  "conditions": ["has_tool_calls", "route_channel", ...]
}
```

- **`neograph_version`** は唯一の真実の情報源である
  `pyproject.toml` からコンパイル時に刻印されます。ツールはキャッシュしたスキーマと比較し、
  エンジンより古いパレットなら警告します。
- **`node_types`** は呼び出し時に `NodeFactory` へ登録されているものを反映します。
  埋め込み側のカスタムノード型も現れるため、エクスポート前に登録してください。
  カスタムリデューサーや条件も同様です。3 引数の `register_type` で登録した型には
  宣言済み設定スキーマが付き、2 引数形式は寛容な `{"type":"object"}` になります。
- **往復契約。** トポロジー JSON を出力するツールはローダーを通して往復させ、構造が保たれることを表明すべきです。
  特にトップレベルの `conditional_edges` ブロックは v0.1.0〜v0.1.7 のコンパイラーで暗黙に破棄されていました (v0.1.8 で修正)。
  エンジンのテストスイート (`tests/test_schema_export.cpp`) がこの回帰を防ぎ、
  ツール側でも同様に検証すべきです。
```cpp
#include <neograph/graph/loader.h>
// register custom node types first if you want them in the palette …
auto schema = neograph::graph::NodeFactory::instance().export_schema();
std::cout << schema.dump(2) << "\n";
```

---
## 10.5. Observability — OpenTelemetry + OpenInference
**モジュール:** `neograph_engine.tracing` (OTel 形式) + `neograph_engine.openinference` (LLM 形式)
**導入時期:** OTel 層は v0.3.x、OpenInference 層は **v0.6.0**。
NeoGraph はストリーミング API と同じコールバックを通じて `GraphEvent` ストリームを出力します。2 つのヘルパーがその上に乗ります:
  - **`otel_tracer(tracer)`** — ベンダー非依存の OpenTelemetry span。
    実行ごとのルートスパン、ノードごとの子スパン、ステータス / エラー / 中断のマッピングを作ります。
    スパンは任意の OTel バックエンド (Jaeger、Tempo、Honeycomb、Datadog など) に流せます。
    すでに APM を運用しており、スパン形式のデータだけが必要な場合に便利です。
  - **`openinference_tracer(tracer)` + `OpenInferenceProvider`** —
    同じ OTel の仕組みですが、各スパンに LLM 形式の属性を追加します。
    span は `openinference.span.kind` (`"CHAIN"` / `"LLM"`) と
    LLM 固有のキー (`llm.model_name`、`llm.input_messages.{i}.…`、
    `llm.token_count.{prompt,completion,total}` など) を持たせます。
    OpenInference 規約に対応するバックエンド (Phoenix、Arize、
    Langfuse) はトレースをチャットバブル + DAG 階層 +
    呼び出しごとのトークンコスト UI (「LangSmith UX」) として表示できます。
### `otel_tracer` — OTel-shape spans
```python
from contextlib import contextmanager
from typing import Any, Callable, Iterator, Optional

@contextmanager
def otel_tracer(
    tracer: Any,
    *,
    root_name: str = "graph.run",
    node_span_prefix: str = "node.",
    attribute_prefix: str = "neograph",
    on_event: Optional[Callable[[Any], None]] = None,
) -> Iterator[Callable[[Any], None]]:
    ...
```

| ノブ | デフォルト | 目的 |
|---|---|---|
| `root_name` | `"graph.run"` | 実行ごとの root span 名 |
| `node_span_prefix` | `"node."` | 各ノード名に連結する prefix |
| `attribute_prefix` | `"neograph"` | エンジン固有属性の prefix (`neograph.node`、`neograph.next_nodes` など) |
| `on_event` | `None` | すべての raw `GraphEvent` を受け取る任意の二次 callback。logging / metrics との連鎖に便利です |
処理されるイベントは、`NODE_START` が子スパンを開き、`NODE_END` が `Status.OK` で閉じ、
`ERROR` が例外を記録して `Status.ERROR` で閉じ、`INTERRUPT` が
`{attribute_prefix}.interrupted = true` を付けて閉じます。
同時ファンアウト (複数 Send) では、各ノード名が開いたスパンのスタックを持ち、
`NODE_END` が最新を取り出します。実行が例外を送出した場合も、
コンテキストマネージャーの `finally` が開いたままのスパンを強制的に閉じます。
```python
from opentelemetry import trace
from neograph_engine.tracing import otel_tracer

tracer = trace.get_tracer("my-service")
with otel_tracer(tracer) as cb:
    engine.run_stream(cfg, cb)
```

### `openinference_tracer` — LLM 形式の属性を追加
同じ形式に加え、各スパンに `openinference.span.kind = "CHAIN"` を付け、ノードのペイロードを `input.value` / `output.value` の JSON blob としてエンコードします。Phoenix / Arize / Langfuse はトレースを UI で LLM チェーンとして扱います。
```python
@contextmanager
def openinference_tracer(
    tracer: Any,
    *,
    root_name: str = "graph.run",
    node_span_prefix: str = "node.",
    on_event: Optional[Callable[[Any], None]] = None,
) -> Iterator[Callable[[Any], None]]:
    ...
```

トレーサーは各ノードスパンを OTel の *現在のコンテキスト* としても接続します (`otel_context.attach`)。
そのためノード本体内の `Provider.complete()` 呼び出しは、そのノードの子として `llm.complete` スパンを開きます。
トレースは 3 つ以上の孤立した trace-ID ではなく、1 つの接続されたツリーになります。
(v0.6.0 の contextvar 伝播修正)。
### `OpenInferenceProvider` — 任意の `Provider` を包む
```python
class OpenInferenceProvider(Provider):
    def __init__(self, inner: Provider, tracer: Any,
                 *, span_name: str = "llm.complete"):
        ...
```

各 `complete(params)` 呼び出しで、現在の OTel コンテキスト配下に LLM 種別の子スパンを開き、
OpenInference 属性を収集して `inner.complete()` に委譲し、その後スパンを閉じます。
トレースの失敗は握りつぶされ、可観測性が LLM 呼び出しを壊すことはありません。
内側のプロバイダーの例外はスパンを ERROR として記録した後に再送出します。
LLM スパンごとに収集する属性:
| 属性 | 由来 |
|---|---|
| `openinference.span.kind` | 定数 `"LLM"` |
| `llm.model_name` | `params.model` |
| `llm.invocation_parameters` | `temperature`、`max_tokens`、`top_p`、`frequency_penalty`、`presence_penalty` の JSON blob (設定時) |
| `llm.input_messages.{i}.message.role` | `params.messages[i].role` |
| `llm.input_messages.{i}.message.content` | `params.messages[i].content` |
| `input.value` / `input.mime_type` | `params.messages` JSON / `application/json` (Langfuse 互換 blob) |
| `llm.output_messages.0.message.role` | `result.message.role` |
| `llm.output_messages.0.message.content` | `result.message.content` |
| `output.value` / `output.mime_type` | `result.message.content` / `text/plain` |
| `llm.token_count.prompt` | `result.usage.prompt_tokens` |
| `llm.token_count.completion` | `result.usage.completion_tokens` |
| `llm.token_count.total` | `result.usage.total_tokens` |
### エンドツーエンド: NeoGraph + Phoenix を 1 ブロックで
```bash
docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix:latest
pip install neograph-engine opentelemetry-exporter-otlp
```

```python
from opentelemetry import trace
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import BatchSpanProcessor
from opentelemetry.exporter.otlp.proto.grpc.trace_exporter import OTLPSpanExporter
from neograph_engine.openinference import OpenInferenceProvider, openinference_tracer
from neograph_engine.llm import OpenAIProvider
import neograph_engine as ng

provider = TracerProvider()
provider.add_span_processor(
    BatchSpanProcessor(OTLPSpanExporter(endpoint="http://localhost:4317", insecure=True)))
trace.set_tracer_provider(provider)
tracer = trace.get_tracer("my-app")

inner = OpenAIProvider(api_key="sk-...")
wrapped = OpenInferenceProvider(inner, tracer)
ctx = ng.NodeContext(provider=wrapped)
engine = ng.GraphEngine.compile(graph_def, ctx)

with openinference_tracer(tracer) as cb:
    engine.run_stream(ng.RunConfig(input={"messages": [...]}), cb)

# Open http://localhost:6006 — the trace renders as a chain with
# each LLM call expanded into prompt / response / token counts.
```

Phoenix の代わりに Langfuse セルフホストへ向けるときは、エンドポイント URL だけを変更します。
どちらも OpenInference と OTLP に対応します。
### メモ
- **オプトイン依存:** `opentelemetry-api` はベース wheel に含まれません。
  `neograph_engine.tracing` / `.openinference` の import は、パッケージがない場合に初回利用時だけ `ImportError` を送出します。
  `pip install opentelemetry-api opentelemetry-sdk opentelemetry-exporter-otlp` でインストールしてください。
- **pybind をまたぐ OTel contextvars:** v0.3.x の `otel_tracer` は
  `trace.use_span(...).__enter__()` を `__exit__()` なしで呼ぶと contextvar が漏れ、
  C++ → Python コールバック境界を確実には伝播しないと説明していました。
  現在の両トレーサーは `otel_context.attach` + `detach` トークン対を明示的に使い、
  現在のスパンの有効化を決定的に制御します。
- **`otel_tracer` と `openinference_tracer`:** バックエンドが APM 形式 (Jaeger、Datadog) で一般的なスパンを必要とするなら OTel 版を使います。Phoenix、Langfuse、Arize で LLM 形式の表示が必要なら OpenInference 版を使います。同じ実行で両者を組み合わせることはできません。エンジンのイベントストリームに対する代替コールバックです。
---
<a id="11-react-graph"></a>
## 11. ReAct グラフ
**ヘッダー:** `<neograph/graph/react_graph.h>`
**名前空間:** `neograph::graph`
標準的な ReAct (Reason + Act) エージェントを、2 ノードのグラフとして作成する便利な関数です。
`llm_call -> tool_dispatch -> (ツール呼び出しならループ、そうでなければ終了)` の構成になります。
```cpp
std::unique_ptr<GraphEngine> create_react_graph(
    std::shared_ptr<Provider> provider,
    std::vector<std::unique_ptr<Tool>> tools,
    const std::string& instructions = "",
    const std::string& model = "");
```

| パラメーター | 型 | 説明 |
|-----------|------|-------------|
| `provider` | `std::shared_ptr<Provider>` | LLM provider |
| `tools` | `std::vector<std::unique_ptr<Tool>>` | エージェントが利用できるツール (所有権は移動します) |
| `instructions` | `std::string` | システムプロンプト / 指示 |
| `model` | `std::string` | model の上書き (空なら provider のデフォルトを使用) |
**戻り値:** 実行可能な状態までコンパイルされた `GraphEngine`。
これは `Agent::run()` と機能的に同等ですがグラフエンジンとして動作し、チェックポイント、ストリーミングイベント、状態検査など
他のグラフエンジン機能にもアクセスできます。
---
## 11b. Plan-and-Execute グラフ
**ヘッダー:** `<neograph/graph/plan_execute_graph.h>`
**名前空間:** `neograph::graph`
Plan-and-Execute パターン向けの便利なファクトリーです。planner が手順の JSON 配列を出力し、executor が内部 ReAct ループで 1 件ずつ処理し、
responder が `past_steps` から最終回答を組み立てます。
```
__start__ → planner → [plan_empty? responder : executor]
                      executor → [plan_empty? responder : executor]
                      responder → __end__
```

```cpp
std::unique_ptr<GraphEngine> create_plan_execute_graph(
    std::shared_ptr<Provider> provider,
    std::vector<std::unique_ptr<Tool>> tools,
    const std::string& planner_prompt,
    const std::string& executor_prompt,
    const std::string& responder_prompt,
    const std::string& model = "",
    int max_step_iterations = 5);
```

| パラメーター | 型 | 説明 |
|-----------|------|-------------|
| `provider` | `std::shared_ptr<Provider>` | 全フェーズで共有する LLM provider |
| `tools` | `std::vector<std::unique_ptr<Tool>>` | executor が呼び出せるツール (所有権は移動します) |
| `planner_prompt` | `std::string` | planner 用の system prompt。手順の JSON 配列で応答するよう model に指示する必要があります (fenced ```json ブロックや先頭の説明文は許容されます) |
| `executor_prompt` | `std::string` | 単一ステップ executor 用の system prompt (内側の ReAct ループ) |
| `responder_prompt` | `std::string` | 最終 synthesis フェーズ用の system prompt |
| `model` | `std::string` | model の上書き (空なら provider のデフォルトを使用) |
| `max_step_iterations` | `int` | 各ステップで executor 内部のツール呼び出し反復回数の上限 |
**値が入るチャネル:** `plan`, `past_steps`, `final_response`, `messages`。
**戻り値:** 実行可能な状態までコンパイルされた `GraphEngine`。ファクトリーは初回呼び出し時に
3 つのカスタムノード型と `plan_empty` 条件を登録します (`std::call_once` により冪等)。
`examples/14_plan_executor.cpp` に、保留書き込みによるクラッシュ / 再開を扱う Send ファンアウト版があります。
---
<a id="12-llm-module"></a>
## 12. LLM モジュール
### OpenAIProvider
**ヘッダー:** `<neograph/llm/openai_provider.h>`
**名前空間:** `neograph::llm`
OpenAI API と OpenAI 互換エンドポイント向けの Provider 実装です。
```cpp
class OpenAIProvider : public Provider {
public:
    struct Config {
        std::string api_key;                          // API key
        std::string base_url = "https://api.openai.com"; // API base URL
        std::string default_model = "gpt-4o-mini";    // Default model
        int timeout_seconds = 60;                     // HTTP timeout
    };

    static std::unique_ptr<OpenAIProvider> create(const Config& config);

    ChatCompletion complete(const CompletionParams& params) override;
    ChatCompletion complete_stream(const CompletionParams& params,
                                   const StreamCallback& on_chunk) override;
    std::string get_name() const override;  // Returns "openai"
};
```

**設定フィールド:**
| フィールド | 型 | デフォルト | 説明 |
|-------|------|---------|-------------|
| `api_key` | `std::string` | | OpenAI API キー |
| `base_url` | `std::string` | `"https://api.openai.com"` | Base URL。Azure、ローカルモデル、互換 API 用に上書きします |
| `default_model` | `std::string` | `"gpt-4o-mini"` | `CompletionParams::model` が空のときに使う model |
| `timeout_seconds` | `int` | `60` | HTTP request のタイムアウト |
**使用方法:**
```cpp
auto provider = neograph::llm::OpenAIProvider::create({
    .api_key = "sk-...",
    .default_model = "gpt-4o"
});
```

### SchemaProvider
**ヘッダー:** `<neograph/llm/schema_provider.h>`
**名前空間:** `neograph::llm`
JSON 設定ファイルを通じて複数の LLM API に対応するスキーマ駆動型 Provider です。API ごとのロジックをハードコードせず、
`SchemaProvider` がリクエストの形式、レスポンスの解析、任意 API のストリーミング処理方法を記述するスキーマを読み取ります。
```cpp
class SchemaProvider : public Provider {
public:
    struct Config {
        std::string schema_path;       // Schema name or file path
        std::string api_key;           // API key (overrides env var)
        std::string default_model = "gpt-4o-mini";
        int         timeout_seconds = 60;
        std::string base_url_override;  // Overrides schema's connection.base_url
        bool        use_websocket = false;  // OpenAI Responses /v1/responses WS mode
        bool        prefer_libcurl = false; // Switch HTTP transport to libcurl HTTP/2
    };

    static std::unique_ptr<SchemaProvider> create(const Config& config);
    static std::shared_ptr<Provider>       create_shared(const Config& config);

    ChatCompletion complete(const CompletionParams& params) override;
    asio::awaitable<ChatCompletion>
    complete_async(const CompletionParams& params) override;
    ChatCompletion complete_stream(const CompletionParams& params,
                                   const StreamCallback& on_chunk) override;
    std::string get_name() const override;
};
```

**設定フィールド:**
| フィールド | 型 | デフォルト | 説明 |
|-------|------|---------|-------------|
| `schema_path` | `std::string` | | 組み込みスキーマ名、またはカスタムスキーマ JSON ファイルへのパス |
| `api_key` | `std::string` | | API キー。空の場合は schema で指定された環境変数へフォールバックします |
| `default_model` | `std::string` | `"gpt-4o-mini"` | デフォルトの model 識別子 |
| `timeout_seconds` | `int` | `60` | HTTP タイムアウト |
| `base_url_override` | `std::string` | `""` | 空でなければ schema の `connection.base_url` を上書きします。テスト用の代替実装やセルフホストの OpenAI 互換 endpoint に便利です。 |
| `use_websocket` | `bool` | `false` | HTTP/SSE ではなく `wss://` で `complete_stream` を駆動します。現在は `"openai_responses"` schema のみ対応しています (OpenAI の /v1/responses WebSocket mode と一致)。 |
| `prefer_libcurl` | `bool` | `false` | 非ストリーミング HTTP トランスポートを libcurl に切り替えます (HTTP/2 + 多重化 + Cloudflare と相性のよい fingerprint)。ビルド時の `NEOGRAPH_USE_LIBCURL` で制御されます。 |
**組み込みスキーマ:**
| 名前 | API | メモ |
|------|-----|-------|
| `"openai"` | OpenAI | `OpenAIProvider` と同じ動作 |
| `"claude"` | Anthropic Claude | SSE event ベースの streaming を使います |
| `"gemini"` | Google Gemini | function declarations 形式を使います |
**カスタムスキーマ:** `schema_path` にファイルパスを渡すと、任意の API のリクエスト/レスポンス形式を記述するカスタム JSON スキーマを読み込みます。
**使用方法:**
```cpp
// Using a built-in schema
auto claude = neograph::llm::SchemaProvider::create({
    .schema_path = "claude",
    .api_key = "sk-ant-...",
    .default_model = "claude-sonnet-4-20250514"
});

// Using a custom schema file
auto custom = neograph::llm::SchemaProvider::create({
    .schema_path = "/path/to/my_provider.json",
    .api_key = "...",
    .default_model = "my-model-v1"
});
```

**内部戦略 enum** (カスタムスキーマ作成者向け):
スキーマファイルは次の戦略を設定します:
| 戦略 | オプション | 説明 |
|----------|---------|-------------|
| System prompt | `IN_MESSAGES`, `TOP_LEVEL`, `TOP_LEVEL_PARTS` | system prompt をリクエストに配置する方法 |
| Tool calls | `TOOL_CALLS_ARRAY`, `CONTENT_ARRAY`, `PARTS_ARRAY` | ツール呼び出しが assistant メッセージに現れる形式 |
| Tool results | `FLAT`, `CONTENT_ARRAY`, `PARTS_ARRAY` | ツール結果の整形方法 |
| Tool defs | `FUNCTION`, `NONE`, `FUNCTION_DECLARATIONS` | ツール定義の包み方 |
| Response | `CHOICES_MESSAGE`, `CONTENT_ARRAY`, `CANDIDATES_PARTS` | 応答の解析方法 |
| Streaming | `SSE_DATA`, `SSE_EVENTS` | ストリーミング形式 |
### Agent
**ヘッダー:** `<neograph/llm/agent.h>`
**名前空間:** `neograph::llm`
LLM のツール使用ループを実行するシンプルなエージェントです。LLM を呼び、ツール呼び出しを実行し、
結果を戻して、LLM がテキストだけで応答するまで繰り返します。
```cpp
class Agent {
public:
    Agent(std::shared_ptr<Provider> provider,
          std::vector<std::unique_ptr<Tool>> tools,
          const std::string& instructions = "",
          const std::string& model = "");

    // Run the tool loop, returns the final text response
    std::string run(std::vector<ChatMessage>& messages,
                    int max_iterations = 10);

    // Streaming variant: streams final response tokens
    std::string run_stream(std::vector<ChatMessage>& messages,
                           const StreamCallback& on_chunk,
                           int max_iterations = 10);

    // Single completion (no tool loop)
    ChatCompletion complete(const std::vector<ChatMessage>& messages);
};
```

| コンストラクターパラメーター | 型 | 説明 |
|-----------------------|------|-------------|
| `provider` | `std::shared_ptr<Provider>` | 使用する LLM provider |
| `tools` | `std::vector<std::unique_ptr<Tool>>` | エージェントが利用できるツール (所有権は移動します) |
| `instructions` | `std::string` | messages の前に付ける system prompt |
| `model` | `std::string` | model の上書き (空なら provider のデフォルトを使用) |
| メソッド | 説明 |
|--------|-------------|
| `run(messages, max_iterations)` | ツール使用ループ全体を実行します。会話全体で `messages` をその場で変更し、最後の assistant テキスト応答を返します |
| `run_stream(messages, on_chunk, max_iterations)` | `run()` と同じですが、最終応答トークンを `on_chunk` でストリーミングします。ツール使用の反復はストリーミングされません |
| `complete(messages)` | ツールループなしの単一 LLM 呼び出し。単発 completion に便利です |
**使用方法:**
```cpp
auto provider = neograph::llm::OpenAIProvider::create({.api_key = "sk-..."});

std::vector<std::unique_ptr<neograph::Tool>> tools;
tools.push_back(std::make_unique<WeatherTool>());

neograph::llm::Agent agent(provider, std::move(tools),
                            "You are a helpful weather assistant.");

std::vector<neograph::ChatMessage> messages;
messages.push_back({"user", "What's the weather in Seoul?"});

std::string response = agent.run(messages);
```

<a id="json_path-utilities"></a>
### json_path ユーティリティ
**ヘッダー:** `<neograph/llm/json_path.h>`
**名前空間:** `neograph::llm::json_path`
ドット区切りのパス文字列を使って JSON 値を移動・操作するユーティリティです。
`SchemaProvider` が内部で使いますが、一般用途にも利用できます。
```cpp
namespace json_path {
    std::vector<std::string> split_path(const std::string& path);
    const json* at_path(const json& root, const std::string& path);
    json* at_path_mut(json& root, const std::string& path);
    bool has_path(const json& root, const std::string& path);

    template<typename T>
    T get_path(const json& root, const std::string& path, const T& default_val);

    void set_path(json& root, const std::string& path, const json& value);
}
```

| 関数 | 説明 |
|----------|-------------|
| `split_path(path)` | dot-path 文字列を segment に分割します。例: `"choices.0.message"` は `["choices", "0", "message"]` になります |
| `at_path(root, path)` | dot-path で JSON 値の内部へ移動します。数値 segment は配列 index です。path が存在しない場合は `nullptr` を返します |
| `at_path_mut(root, path)` | `at_path` の mutable 版 |
| `has_path(root, path)` | JSON 値内に dot-path が存在する場合は `true` を返します |
| `get_path<T>(root, path, default_val)` | path 上の値を型 `T` に変換して返します。path が存在しない、または変換に失敗した場合は `default_val` を返します |
| `set_path(root, path, value)` | dot-path 上に値を設定し、必要に応じて中間 object を作成します |
**例:**
```cpp
using namespace neograph::llm::json_path;

json data = json::parse(R"({"choices": [{"message": {"content": "Hello"}}]})");

// Navigate
const json* msg = at_path(data, "choices.0.message.content");
// *msg == "Hello"

// Check existence
bool exists = has_path(data, "choices.0.message.role");
// exists == false

// Get with default
std::string role = get_path<std::string>(data, "choices.0.message.role", "assistant");
// role == "assistant"

// Set value (creates intermediates)
set_path(data, "metadata.version", 2);
```

---
<a id="13-mcp-module"></a>
## 13. MCP モジュール
**ヘッダー:** `<neograph/mcp/client.h>`
**名前空間:** `neograph::mcp`
Model Context Protocol (MCP) クライアント実装です。MCP サーバーに接続し、利用可能なツールを検出して、
利用できるトランスポートは 2 つです:
- **HTTP** — `MCPClient("http://host:port")`。検出されたツールは、元の Streamable HTTP セッションを保持します。
- **stdio** — `MCPClient({"python", "server.py"})`。クライアントはサブプロセスを `fork` + `execvp` し、双方向パイプを接続して、
  子プロセスの stdin/stdout 上で通信します。サブプロセスは、
  生成元の `MCPClient` またはいずれかの `MCPTool` が生存する間だけ存在します。
  破棄時には SIGTERM を送り、`waitpid` で回収します (約 500 ms 後に SIGKILL へフォールバック)。
### MCPTool
単一の MCP サーバーツールをローカル `Tool` 実装としてラップします。トランスポートごとに 1 つずつ、2 つのコンストラクターがあります。
トランスポートごとに 1 つずつコンストラクターがあり、`MCPClient::get_tools()` が適切なものを選びます。
```cpp
class MCPTool : public AsyncTool {
public:
    // Legacy direct-construction mode. Discovered tools reuse their client session.
    MCPTool(const std::string& server_url,
            const std::string& name,
            const std::string& description,
            const json& input_schema);

    // stdio mode — tool holds a shared_ptr back-ref to the subprocess
    // session, keeping it alive as long as any tool is reachable.
    MCPTool(std::shared_ptr<detail::StdioSession> session,
            const std::string& name,
            const std::string& description,
            const json& input_schema);

    const ToolDefinition& get_mcp_definition() const noexcept;
    CallToolResult execute_result(const json& arguments);
    asio::awaitable<CallToolResult> execute_result_async(const json& arguments);
    ChatTool get_definition() const override;
    asio::awaitable<std::string> execute_async(const json& arguments) override;
    std::string get_name() const override;
};
```

通常、`MCPTool` を直接構築することはありません。`MCPClient::get_tools()` が検出してラップします。
### MCPClient
MCP サーバーに接続し、初期化ハンドシェイクを行い、ツールの検出と呼び出しのメソッドを提供するクライアントです。
> `MCPClient` はサブクラス化を想定していません。そのまま使用してください。
> `rpc_call_async()` が実装本体で、`rpc_call()` は薄い同期ファサードです。
> [`ASYNC_GUIDE.md` §9.5](ASYNC_GUIDE.md#95-mcpclient) を参照してください。
```cpp
class MCPClient {
public:
    // HTTP transport.
    explicit MCPClient(const std::string& server_url);
    MCPClient(const std::string& server_url, MCPClientConfig config);

    // stdio transport — fork+exec the subprocess.
    explicit MCPClient(std::vector<std::string> argv);

    bool initialize(const std::string& client_name = "neograph");
    bool is_initialized() const noexcept;
    InitializeResult get_initialize_result() const;
    std::vector<std::unique_ptr<Tool>> get_tools();
    ListToolsPage list_tools(std::optional<std::string> cursor = std::nullopt);
    std::vector<ToolDefinition> get_tool_definitions();
    json call_tool(const std::string& name, const json& arguments);
    CallToolResult call_tool_result(const std::string& name,
                                    const json& arguments);

    // Low-level async dispatch retained for source compatibility.
    asio::awaitable<json>
    rpc_call_async(const std::string& method, const json& params);
};
```

**ワイヤープロトコル:** NeoGraph の MCP クライアントは
`protocolVersion = "2025-11-25"` を使用します。HTTP トランスポートは
すべての JSON-RPC リクエストに `MCP-Protocol-Version` ヘッダーを送り (Round 1 +
Round 3 の仕様に整合)、stdio トランスポートは同じバージョンを `initialize` ペイロードに持たせます。
古いプロトコルバージョンで動作するサーバーはリクエストを拒否する可能性があるため、サーバー側を固定するかアップグレードしてください。
| メソッド | 説明 |
|--------|-------------|
| `MCPClient(url)` | HTTP モードのクライアントを構築 |
| `MCPClient(argv)` | サブプロセスを起動して stdio モードのクライアントを構築。`argv[0]` は `PATH` で解決 (execvp)。fork/exec 失敗時は例外。安全対策 (Round 3 強化) のため Windows の `.bat` / `.cmd` は拒否 |
| `initialize(client_name)` | MCP 初期化ハンドシェイクを 1 回実行。再呼び出しは冪等で、プロトコル/トランスポート失敗時は例外 |
| `get_initialize_result()` | ネゴシエートしたプロトコル、機能、サーバー情報、指示、未加工の結果を返す |
| `list_tools(cursor)` | カーソルを不透明な値として扱い、1 ページを取得 |
| `get_tool_definitions()` | 全ページをたどり、ツールメタデータ全体を保持 |
| `get_tools()` | 全ページを検出し、セッションを保持する `MCPTool` インスタンスを返す |
| `call_tool(name, arguments)` | 指定された引数で名前付きツールを呼び出し、未加工の JSON 応答を返す |
| `call_tool_result(name, arguments)` | content、structured content、`isError`、`_meta` を保持する型付き結果 |
| `rpc_call_async(method, params)` | コルーチン版。実装本体で、`rpc_call` は薄い同期ラッパー |
**HTTP の使用方法:**
```cpp
neograph::mcp::MCPClient client("http://localhost:8000");
client.initialize();
auto tools = client.get_tools();
```

**stdio の使用方法:**
```cpp
// argv[0] is resolved via PATH; pipe fds are closed in the child before execvp.
neograph::mcp::MCPClient client({"python", "/path/to/server.py"});
client.initialize();
auto tools = client.get_tools();   // MCPTools hold shared_ptr<StdioSession>
```

---
<a id="14-util-module"></a>
## 14. Util モジュール
**ヘッダー:** `<neograph/util/request_queue.h>`
**名前空間:** `neograph::util`
### RequestQueue
ワーカースレッドプールとバックプレッシャーに対応したロックフリーのリクエストキューです。
サーバーアプリケーションで HTTP 接続の受け付けと LLM 呼び出しの同時実行数を分離します。
```cpp
class RequestQueue {
public:
    struct Stats {
        size_t pending;        // Tasks waiting in queue
        size_t active;         // Tasks currently executing
        size_t completed;      // Total settled tasks, including cancellation
        size_t rejected;       // Tasks rejected during admission
        size_t num_workers;    // Number of worker threads
        size_t max_queue_size; // Maximum queue capacity
    };

    // num_workers must be greater than zero.
    RequestQueue(size_t num_workers = 128, size_t max_queue_size = 10000);
    ~RequestQueue();

    // Non-copyable
    RequestQueue(const RequestQueue&) = delete;
    RequestQueue& operator=(const RequestQueue&) = delete;

    // Submit a task. Concurrent callers cannot exceed max_queue_size.
    // A full queue returns {false, invalid_future}; an internal enqueue
    // failure returns {false, valid_future}, which throws on get().
    template<typename F>
    std::pair<bool, std::future<void>> submit(F&& task);

    // Idempotently reject new work, cancel queued tasks, and wait for workers.
    void close();
    bool is_closed() const noexcept;

    // Get current queue statistics
    Stats stats() const;
};
```

| コンストラクターパラメーター | 型 | デフォルト | 説明 |
|-----------------------|------|---------|-------------|
| `num_workers` | `size_t` | `128` | プール内のワーカースレッド数。0 は `std::invalid_argument` を送出 |
| `max_queue_size` | `size_t` | `10000` | 保留できるタスクの最大数。超過分は拒否 |
| メソッド | 説明 |
|--------|-------------|
| `submit(task)` | pending capacity を原子的に予約してから callable を enqueue します。受理時は `first=true`。満杯または閉じたキューは invalid future と `false` を返し、内部 enqueue 失敗はエラーを伝播する valid future と `false` を返します。受理済み future は完了時に解決するかタスク例外を伝播します。 |
| `close()` | 新しい作業を冪等に拒否します。外部呼び出しは全 worker の終了を待ち、未取得作業は `std::runtime_error("RequestQueue is closed")` で完了します。取得済み callable は完了できます。callable 自身が `close()` を呼んで終了を開始できますが、自分自身は待ちません。 |
| `is_closed()` | `close()` が新しい作業の拒否を開始したか報告します。 |
| `stats()` | 現在のキュー統計のスナップショットを返す |
キュー内部ではロックフリーの enqueue/dequeue に `moodycamel::ConcurrentQueue` を使い、
条件変数でアイドル状態のワーカーを起こします。
**使用方法:**
```cpp
neograph::util::RequestQueue queue(4, 100);  // 4 workers, max 100 pending

auto [accepted, future] = queue.submit([&] {
    // Handle an incoming HTTP request
    auto result = engine->run(config);
    send_response(result);
});

if (!accepted) {
    send_503_service_unavailable();
}
```

---
<a id="usage-examples"></a>
## 使用例
<a id="minimal-react-agent"></a>
### 最小 ReAct エージェント
NeoGraph を使う最も簡単な方法、つまりツール付き ReAct エージェントです:
```cpp
#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/graph/react_graph.h>

int main() {
    auto provider = neograph::llm::OpenAIProvider::create({
        .api_key = std::getenv("OPENAI_API_KEY"),
        .default_model = "gpt-4o"
    });

    std::vector<std::unique_ptr<neograph::Tool>> tools;
    tools.push_back(std::make_unique<WeatherTool>());

    auto engine = neograph::graph::create_react_graph(
        provider, std::move(tools),
        "You are a helpful assistant with access to weather data."
    );

    neograph::graph::RunConfig config;
    config.input = {{"messages", json::array({
        {{"role", "user"}, {"content", "What's the weather in Tokyo?"}}
    })}};

    auto result = engine->run(config);
    // result.output contains the final state with all messages
}
```

<a id="custom-graph-with-conditional-routing"></a>
### 条件付きルーティングを持つカスタムグラフ
条件エッジを持つグラフを構築します:
```cpp
#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>

using namespace neograph::graph;
using json = nlohmann::json;

int main() {
    auto provider = neograph::llm::OpenAIProvider::create({
        .api_key = std::getenv("OPENAI_API_KEY")
    });

    std::vector<std::unique_ptr<neograph::Tool>> tools;
    tools.push_back(std::make_unique<SearchTool>());
    tools.push_back(std::make_unique<CalculatorTool>());

    json definition = {
        {"name", "assistant_graph"},
        {"channels", {
            {"messages", {{"reducer", "append"}}},
            {"status",   {{"reducer", "overwrite"}, {"initial", "idle"}}}
        }},
        {"nodes", {
            {"llm",   {{"type", "llm_call"}}},
            {"tools", {{"type", "tool_dispatch"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "llm"}},
            {{"from", "tools"},     {"to", "llm"}}
        })},
        {"conditional_edges", json::array({
            {{"from", "llm"},
             {"condition", "has_tool_calls"},
             {"routes", {{"yes", "tools"}, {"no", "__end__"}}}}
        })}
    };

    auto store = std::make_shared<InMemoryCheckpointStore>();
    EngineConfig engine_config;
    engine_config.node_context.provider = provider;
    engine_config.node_context.model = "gpt-4o";
    engine_config.node_context.instructions = "You are a helpful assistant.";
    engine_config.checkpoint_store = store;
    EngineResources resources{.tools = ToolSet(std::move(tools))};
    auto engine = GraphEngine::build(definition, std::move(engine_config),
                                     std::move(resources));

    RunConfig config;
    config.thread_id = "session-1";
    config.input = {{"messages", json::array({
        {{"role", "user"}, {"content", "Search for NeoGraph C++ library"}}
    })}};

    auto result = engine->run(config);

    // Inspect execution trace
    for (const auto& node : result.execution_trace) {
        std::cout << "Executed: " << node << "\n";
    }
}
```

<a id="human-in-the-loop-with-checkpointing"></a>
### チェックポイントを使う Human-in-the-Loop
人間の承認に中断を使います:
```cpp
auto store = std::make_shared<InMemoryCheckpointStore>();
EngineConfig engine_config;
engine_config.node_context = ctx;
engine_config.checkpoint_store = store;
auto engine = GraphEngine::build(definition, std::move(engine_config));

// Configure interrupt after the "tools" node
// (set "interrupt_after": ["tools"] in the JSON definition)

RunConfig config;
config.thread_id = "approval-session";
config.input = {{"messages", json::array({
    {{"role", "user"}, {"content", "Delete all files in /tmp"}}
})}};

auto result = engine->run(config);

if (result.interrupted) {
    std::cout << "Interrupted at: " << result.interrupt_node << "\n";
    std::cout << "Reason: " << result.interrupt_value.dump() << "\n";

    // Get human input...
    std::string approval = get_human_approval();

    // Resume with the human's decision
    auto resumed = engine->resume(
        "approval-session",
        {{"approved", approval == "yes"}}
    );
}
```

<a id="dynamic-fan-out-with-send"></a>
### Send による動的ファンアウト
map-reduce パターンに `Send` を使います:
```cpp
class FanOutNode : public GraphNode {
public:
    std::string get_name() const override { return "fan_out"; }

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto items = in.state.get("items");
        NodeOutput result;
        for (const auto& item : items) {
            result.sends.push_back(Send{
                "process_item",       // target node
                {{"item", item}}      // input for that invocation
            });
        }
        co_return result;
    }
};
```

各 `Send` は異なる入力で `"process_item"` ノードをディスパッチします。エンジンはすべての Send を実行し、
<a id="routing-override-with-command"></a>
### Command によるルーティング上書き
状態を同時に更新し、ルーティングを制御するために `Command` を使います:
```cpp
class RouterNode : public GraphNode {
public:
    std::string get_name() const override { return "router"; }

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto messages = in.state.get_messages();
        auto last = messages.back().content;

        NodeOutput result;

        if (last.find("urgent") != std::string::npos) {
            result.command = Command{
                "urgent_handler",                          // goto node
                {{{"channel", "priority"}, {"value", "high"}}} // state updates
            };
        } else {
            result.command = Command{
                "normal_handler",
                {{{"channel", "priority"}, {"value", "normal"}}}
            };
        }

        co_return result;
    }
};
```

`Command` が返されると、その `updates` が状態に適用され、通常のエッジルーティングを迂回して
指定された `goto_node` へ直接移動します。
<a id="schemaprovider-multi-llm-support"></a>
### SchemaProvider によるマルチ LLM 対応
`SchemaProvider` を使って LLM プロバイダーを切り替えます:
```cpp
#include <neograph/llm/schema_provider.h>

// OpenAI
auto openai = neograph::llm::SchemaProvider::create({
    .schema_path = "openai",
    .api_key = std::getenv("OPENAI_API_KEY"),
    .default_model = "gpt-4o"
});

// Anthropic Claude
auto claude = neograph::llm::SchemaProvider::create({
    .schema_path = "claude",
    .api_key = std::getenv("ANTHROPIC_API_KEY"),
    .default_model = "claude-sonnet-4-20250514"
});

// Google Gemini
auto gemini = neograph::llm::SchemaProvider::create({
    .schema_path = "gemini",
    .api_key = std::getenv("GEMINI_API_KEY"),
    .default_model = "gemini-2.0-flash"
});

// All three implement the same Provider interface
// Use any of them interchangeably with Agent or GraphEngine
neograph::llm::Agent agent(claude, std::move(tools), "You are helpful.");
```

<a id="mcp-tool-integration"></a>
### MCP ツール統合
MCP サーバーに接続し、そのツールを使います:
```cpp
#include <neograph/mcp/client.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/llm/agent.h>

int main() {
    // Connect to MCP server
    neograph::mcp::MCPClient mcp("http://localhost:3000");
    if (!mcp.initialize()) {
        std::cerr << "Failed to connect to MCP server\n";
        return 1;
    }

    // Discover tools from server
    auto tools = mcp.get_tools();
    std::cout << "Discovered " << tools.size() << " tools\n";

    // Use discovered tools with an Agent
    auto provider = neograph::llm::OpenAIProvider::create({
        .api_key = std::getenv("OPENAI_API_KEY")
    });

    neograph::llm::Agent agent(provider, std::move(tools),
                                "You have access to remote tools via MCP.");

    std::vector<neograph::ChatMessage> messages;
    messages.push_back({"user", "Use the available tools to help me."});

    std::string response = agent.run(messages);
    std::cout << response << "\n";
}
```

---
## このツアーの対象外
`include/neograph/` 以下のヘッダーには、上で扱っていない公開 API も含まれています。
以下の各節は、その正式なソースレベルリファレンスへの短い案内です。
### `neograph::a2a` — Agent-to-Agent プロトコル
**ヘッダー:** `<neograph/a2a/{client,server,types,a2a_caller_node}.h>`
Streamable HTTP 上の JSON-RPC 2.0 です。`A2AClient` はリモートエージェントに
(`message/send`、`tasks/get`、`tasks/cancel`、AgentCard の検出、`message/stream` SSE) を呼び出します。
サーバー側は `GraphAgentAdapter` を通じて NeoGraph の `GraphEngine` を A2A エンドポイントへ適応させます。
`v0.3` / `v1` のメソッド名を二重にディスパッチします。
commit `bc675a1` を参照してください。ストリーミングには (client 側) `SseFrameSplitter` と
(server 側) httplib chunked を使います。caller node は A2A 呼び出しをグラフノードとして組み込みます。

**公開ヘッダー:** [`include/neograph/a2a/`](../include/neograph/a2a/)。
### `neograph::acp` — Agent Client Protocol
**ヘッダー:** `<neograph/acp/{server,types}.h>`
stdio 上の改行区切り JSON によるエディター↔エージェント JSON-RPC です (Zed、
Gemini CLI、Neovim CodeCompanion)。双方向で、client→agent
(`initialize`、`session/{new,prompt,cancel}`) と agent→client
(`fs/{read,write}_text_file`、`session/request_permission`) を遅延バインドされた `ACPClient` 経由で扱います。
`ACPServer::handle_message` はワーカースレッド上でプロンプトを非同期ディスパッチし、
`max_inflight_prompts=32`、セッションごとの single-flight、`-32000` のバックプレッシャーで上限を設けます。

**公開ヘッダー:** [`include/neograph/acp/`](../include/neograph/acp/)。
### `neograph::async` — HTTP/SSE/WS ヘルパー
**ヘッダー:** `<neograph/async/{conn_pool,http_client,sse_parser,ws_client,curl_h2_pool,run_sync}.h>`
コルーチンベースの HTTP/1.1 クライアント + ConnPool です。安全なメソッドだけの stale-idle retry を備え、
RFC 7231 §4.2.2 に従い、POST などは暗黙に二重適用せず再送出します。
OpenAI/Claude ストリーミングには `SseEventParser`、
OpenAI Responses WebSocket には `WsClient`、libcurl には
Cloudflare 前段のエンドポイントで HTTP/2 + 多重化を行う `CurlH2Pool`、
エンジンのデフォルトで awaitable と同期を橋渡しする `run_sync` を使います。

**公開ヘッダー:** [`include/neograph/async/`](../include/neograph/async/)。
### 永続チェックポイントバックエンド
**ヘッダー:** `<neograph/graph/postgres_checkpoint.h>`、
`<neograph/graph/sqlite_checkpoint.h>`
`PostgresCheckpointStore` — libpq ベースの 3 テーブルスキーマ (`neograph_*`) で、
チャネル blob を `(thread_id, channel, version)` をキーに重複排除します。LangGraph の `PostgresSaver` と同等です。
非同期の初回/差し替え接続は全ホストに対する 1 つのグローバル期限を使います。
正の `connect_timeout` は接続文字列へ直接書き込まれ (最小 2 秒)、それ以外は 30 秒の安全なデフォルトになります。
環境変数とサービスファイルのタイムアウト値は初回接続確立前には利用できず、そのデフォルトが使われます。
`SqliteCheckpointStore` — 同じ形の単一ファイルバックエンドで、
エッジ / 単一ホスト配置に適しています。
**公開ヘッダー:**
[`PostgresCheckpointStore`](../include/neograph/graph/postgres_checkpoint.h) ·
[`SqliteCheckpointStore`](../include/neograph/graph/sqlite_checkpoint.h)。
### このツアーにないその他の公開 API
- **`neograph::llm::RateLimitedProvider`** — 任意の `Provider` を
  429 再試行、Retry-After の尊重、上限付き指数バックオフ、最大総待ち時間ゲート付きでラップします (Round 5)。
  [ヘッダー](../include/neograph/llm/rate_limited_provider.h)。
- **`neograph::AsyncTool`** — コルーチン向き (HTTP fetch、MCP 呼び出し) の仕事に対して
  `execute_async(json)` を公開する `Tool` の対となる実装です。同期 `execute()` は
  `run_sync` を経由して `final` ルーティングされます。
- **`neograph::graph::NodeCache`** — ノード単位のメモ化です。
  構築時に `EngineConfig::cached_nodes` でオプトインします (setter は互換性のため残ります)。
- **`neograph::graph::create_deep_research_graph`** —
  `examples/25_deep_research.cpp` で使われます。Round 2 の監査で
  `BriefNode` の LLM 書き換え、`FinalReportNode` のトークン上限再試行、
  `ClarifyNode` HITL gate で触れています。
このツアーに必要な型がない場合は `include/neograph/` を直接確認してください。
すべての公開ヘッダーにリファレンス文書があります。
