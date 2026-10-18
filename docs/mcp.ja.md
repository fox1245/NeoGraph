<!-- neograph-i18n: source=docs/mcp.md locale=ja source_sha256=a8f7e349912caf07dfb404ab4028ca0be14f572ae5a92d04c088205b301b832c -->
# MCP: グラフをツールとして公開する、ツールを使うグラフを作る

**Languages:** [English](mcp.md) | [한국어](mcp.ko.md) | [日本語](mcp.ja.md) | [简体中文](mcp.zh-CN.md)

Model Context Protocol(MCP)は、クライアントがサーバーのツールを見つけて呼び出す方法を、JSON-RPC 2.0 上で定めたものです。クライアントは通常、エージェントホスト、エディター、モデルを使うアプリケーションです。NeoGraph は両方の役を担えます。サーバーとしてはツールを公開し、多くの場合グラフ1つがツール1つになります。クライアントとしては、他のサーバーが公開するツールを、グラフのノードやモデルのツール呼び出しから呼びます。

このページを [Harness MCP](HARNESS_MCP.md) と混同しないでください。Harness MCP のドキュメントは、複数ワーカーのワークフローをコンパイルして実行する6つのツールを備えた、既製の MCP サーバーを説明しています。このページは、`neograph::mcp` ライブラリで自分のサーバーやクライアントを作る方法を扱います。

## コンポーネント

| 役割 | C++ ターゲット | CMake オプション(既定値) | Python |
|---|---|---|---|
| クライアント(stdio、Streamable HTTP) | `neograph::mcp` | `NEOGRAPH_BUILD_MCP_CLIENT`(オン) | `neograph_engine.mcp` |
| stdio サーバー | `neograph::mcp_server` | `NEOGRAPH_BUILD_MCP_SERVER`(オン) | なし |
| Streamable HTTP サーバー | `neograph::mcp_http_server` | `NEOGRAPH_BUILD_MCP_HTTP_SERVER`(**オフ**) | なし |

`NEOGRAPH_BUILD_MCP`(オン)が、クライアントと stdio サーバーのオプションの既定値を決めます。HTTP サーバーは自分で有効にする必要があります。Python パッケージには MCP サーバーがありません。Python でサーバーを作るには、公式の MCP Python SDK を使い、ツール関数の中でコンパイル済みのエンジンを呼び出してください。

## プロトコルのバージョン

サーバーとクライアントは MCP `2025-11-25` を話し、`2025-06-18`、`2025-03-26`、`2024-11-05` も受け付けます。サーバーは、クライアントが求めたリビジョンで応答し、それ以外の要求には `2025-11-25` で応答します。トランスポートは stdio と Streamable HTTP だけです。古い HTTP+SSE トランスポートには対応していません。`2026-07-28` リビジョン(`initialize` もセッションもない)にもまだ対応していません。そのリビジョンしか提供しないサーバーは、ハンドシェイクを `-32022` で拒否し、呼び出し側には `MCPError` として届きます。詳細は [C++ リファレンス](reference-en.md#13-mcp-module)にあります。

## stdio でグラフをツールとして公開する

このプログラムは、単語数を数える1ノードのグラフを作り、ツール `word_count` として公開します。API キーは不要です。ツールが呼ばれるたびに、グラフを1回実行します。

```cpp
#include <neograph/neograph.h>
#include <neograph/mcp/server.h>

#include <memory>
#include <sstream>
#include <utility>

using namespace neograph;
using namespace neograph::graph;

namespace {

// A graph node: count the words in the "text" channel and write the number to "count".
class WordCountNode final : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        std::istringstream words(input.state.get(ChannelKey<std::string>{"text"}));
        int count = 0;
        for (std::string word; words >> word;) ++count;
        co_return NodeOutput{{ChannelWrite{"count", json(count)}}};
    }
    std::string get_name() const override { return "word_count"; }
};

std::shared_ptr<GraphEngine> build_engine() {
    NodeFactory::instance().register_type(
        "docs.word_count", [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<WordCountNode>();
        },
        json::object(),
        json{{"reads", json::array({"text"})},
             {"writes", json::array({"count"})},
             {"exports", json::array({"count"})}});
    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "word-count"},
        {"channels", {{"text", {{"reducer", "overwrite"}, {"initial", ""}}},
                      {"count", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"word_count", {{"type", "docs.word_count"}}}}},
        {"edges", {{{"from", "__start__"}, {"to", "word_count"}},
                   {{"from", "word_count"}, {"to", "__end__"}}}},
    };
    return std::shared_ptr<GraphEngine>(GraphEngine::build_strict(definition, EngineConfig{}));
}

std::unique_ptr<mcp::MCPServer> make_server(std::shared_ptr<GraphEngine> engine) {
    mcp::MCPServerConfig config;
    config.server_info = {{"name", "word-count-graph"}, {"version", "1.0.0"}};
    auto server = std::make_unique<mcp::MCPServer>(std::move(config));

    mcp::ToolDefinition tool;
    tool.name = "word_count";
    tool.description = "Count the words in a text by running a NeoGraph graph.";
    tool.input_schema = {{"type", "object"},
                         {"required", json::array({"text"})},
                         {"properties", {{"text", {{"type", "string"}}}}},
                         {"additionalProperties", false}};

    // One call is one graph run. Calls arrive on the server's worker threads;
    // the engine may be shared, and the client's cancellation reaches the run
    // through the call's CancelToken.
    server->register_tool(std::move(tool), [engine](const json& arguments, const auto& cancel) {
        RunConfig run;
        run.input = {{"text", arguments.at("text")}};
        run.cancel_token = cancel;
        const auto result = engine->run(run);

        mcp::CallToolResult reply;
        reply.content = json::array(
            {{{"type", "text"}, {"text", std::to_string(result.channel<int>("count"))}}});
        return reply;
    });
    return server;
}

}  // namespace

int main() {
    make_server(build_engine())->run();  // newline-delimited JSON-RPC on stdin and stdout
    return 0;
}
```

仕組みは次のとおりです。

- ツールは、名前、引数の JSON Schema(`input_schema` には `"type": "object"` が必要)、ハンドラーでできています。サーバーは、ハンドラーを実行する前に、すべての呼び出しの引数をスキーマで検査します。引数が不正な場合は、`isError: true`、検証メッセージ、コードが `NG.InvalidRequest` の `_meta["io.neograph/failure"]` を持つ結果が返ります。NeoGraph のクライアントは、これをハンドラーが実行されなかった印として読みます。
- ハンドラーは固定サイズのワーカープールで実行されます。`MCPServerConfig::max_concurrent_calls`(4)件が同時に実行され、`max_pending_calls`(64)件が待機します。それを超える呼び出しには JSON-RPC エラー `-32000`("MCP tool queue is full")が返ります。エンジンはそれらのスレッドが共有します。ノードは複数の実行で再利用されるので、実行ごとのデータはノードオブジェクトではなくチャネルに置いてください。エンジンにチェックポイントストアがある場合は、呼び出しごとに別の `RunConfig::thread_id` を指定してください。
- クライアントが `notifications/cancelled` を送ると、その呼び出しの `CancelToken` がキャンセルされます。このトークンを `run.cancel_token` に渡せば、グラフの実行も止まります。
- `run()` は stdin から1行に1つの JSON-RPC メッセージを読み、stdout に書きます。プログラムが stdout にそれ以外を出力するとプロトコルが壊れるので、ログは stderr に出してください。

`neograph::core` と `neograph::mcp_server` をリンクします。

```cmake
add_executable(word_count_server word_count_server.cpp)
target_link_libraries(word_count_server PRIVATE neograph::core neograph::mcp_server)
```

MCP ホストから使うには、この実行ファイルを引数なしの stdio サーバーとして登録します。

## クライアントから呼び出す

サーバーを子プロセスとして起動し、ツールを呼びます。`get_tool_definitions()` はすべての結果ページをたどり、`call_tool_result()` は応答の content、`structuredContent`、`isError` を返します。

```cpp
#include <neograph/neograph.h>
#include <neograph/mcp/client.h>

#include <iostream>

using namespace neograph;

int main(int argc, char** argv) {
    // Start the server as a child process and talk to it over its stdin and stdout.
    mcp::MCPClient client(std::vector<std::string>{argv[1]});
    client.initialize("docs-client");

    for (const auto& definition : client.get_tool_definitions())
        std::cout << "tool: " << definition.name << " - " << definition.description << '\n';

    const auto result =
        client.call_tool_result("word_count", {{"text", "graphs keep state in channels"}});
    std::cout << "is_error: " << std::boolalpha << result.is_error << '\n'
              << "text: " << result.content[0]["text"].get<std::string>() << '\n';
    client.shutdown();
    return 0;
}
```

Python でも同じことができます。クライアントオブジェクトが解放されると、子プロセスは終了します。

```python
from neograph_engine import mcp

client = mcp.MCPClient(["./word_count_server"])  # stdio: starts the server as a child process
# client = mcp.MCPClient("http://127.0.0.1:8000/mcp")  # Streamable HTTP: the endpoint URL
client.initialize("docs-client")

for definition in client.get_tool_definitions():
    print(definition.name, "-", definition.description)

result = client.call_tool_result("word_count", {"text": "graphs keep state in channels"})
print(result.is_error, result.content[0]["text"])
```

C++ プログラムを `./mcp_client ./word_count_server` で起動すると、次のように出力されます。

```text
tool: word_count - Count the words in a text by running a NeoGraph graph.
is_error: false
text: 5
```

Python スクリプトは次のように出力します。

```text
word_count - Count the words in a text by running a NeoGraph graph.
False 5
```

0.14.0 のホイールにもこの Python クライアントが含まれています。stdio トランスポートは上のサーバーで、HTTP クライアントは後述の HTTP サーバーで動き、`MCPClient.shutdown()` もあります。0.13.1 以前のホイールには `shutdown()` がなく、その HTTP クライアントは後述の HTTP サーバーでセッションを開けませんでした。そのようなホイールを使う場合は、現在のソースからビルドしてください。

HTTP では、`MCPClient("http://127.0.0.1:8000/mcp")` のようにサーバーのエンドポイント URL を渡し、その URL がそのまま使われます。見つけたツールは自分のセッションを保持するので、`MCPClient` オブジェクトが破棄されたあとも使えます。

## グラフのノードからツールを呼ぶ

ノードがクライアントを持ち、ツールを呼べます。実行の期限とキャンセルトークンが呼び出しを制限します。実行がキャンセルされると、クライアントはサーバーに `notifications/cancelled` を送り、遅れて届いた応答は捨てます。

```cpp
#include <neograph/mcp/client.h>
#include <neograph/neograph.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

using namespace neograph;
using namespace neograph::graph;

namespace {

// A node that calls the "word_count" tool of an MCP server and writes the reply.
class RemoteWordCount final : public GraphNode {
public:
    explicit RemoteWordCount(std::shared_ptr<mcp::MCPClient> client) : client_(std::move(client)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        const auto text = input.state.get(ChannelKey<std::string>{"text"});
        const json arguments = {{"text", text}};
        // The run's deadline and cancel token bound the call. If the run is
        // cancelled, the client sends notifications/cancelled to the server.
        const auto deadline =
            input.ctx.deadline.value_or(std::chrono::steady_clock::time_point::max());
        const auto reply = co_await client_->call_tool_result_async(
            "word_count", arguments, deadline, input.ctx.cancel_token);
        if (reply.is_error) throw std::runtime_error("word_count failed");

        NodeOutput output;
        const int count = std::stoi(reply.content[0]["text"].get<std::string>());
        output.writes.push_back(ChannelWrite{"count", json(count)});
        co_return output;
    }

    std::string get_name() const override { return "remote_word_count"; }

private:
    std::shared_ptr<mcp::MCPClient> client_;
};

}  // namespace

int main(int argc, char** argv) {
    auto client = std::make_shared<mcp::MCPClient>(std::vector<std::string>{argv[1]});
    client->initialize("docs-graph");

    NodeFactory::instance().register_type(
        "docs.remote_word_count", [client](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<RemoteWordCount>(client);
        },
        json::object(),
        json{{"reads", json::array({"text"})},
             {"writes", json::array({"count"})},
             {"exports", json::array({"count"})}});

    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "remote-word-count"},
        {"channels", {{"text", {{"reducer", "overwrite"}, {"initial", ""}}},
                      {"count", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"count_words", {{"type", "docs.remote_word_count"}}}}},
        {"edges", {{{"from", "__start__"}, {"to", "count_words"}},
                   {{"from", "count_words"}, {"to", "__end__"}}}},
    };

    auto engine = GraphEngine::build_strict(definition, EngineConfig{});
    RunConfig run;
    run.input = {{"text", "tools can be called from a graph node"}};
    std::cout << "count: " << engine->run(run).channel<int>("count") << '\n';
    return 0;
}
```

前の節のサーバーのパスを渡して `./remote_word_count ./word_count_server` と実行すると、`count: 8` と出力されます。

モデルにツールを選ばせる場合は、`MCPClient::get_tools()` でサーバーのツールを NeoGraph の `Tool` オブジェクトとして受け取ります。それを `NodeContext::tools` に入れると(Python では `ng.NodeContext(tools=client.get_tools())`)、`tool_dispatch` ノードがモデルの要求した呼び出しを実行します。[C++ サンプル](../examples/README.md)には MCP エージェント(03、20、22〜24)があり、API キーは要りません。標準ライブラリだけのデモサーバー `examples/mcp_demo_server.py` がツールを提供し、スクリプト化したモデルが LLM の役を務めます。実際のモデルを使うには `--live` を付けます。その場合は `OPENROUTER_API_KEY` が必要です。[Python サンプル 26](../bindings/python/examples/README.md) は、ローカルの MCP サーバーを自分で起動するので、こちらもキーは要りません。

## Streamable HTTP で公開する

`neograph::mcp_http_server` をリンクし、`-DNEOGRAPH_BUILD_MCP_HTTP_SERVER=ON` でビルドして、上のプログラムの `main` を差し替えます。`make_server` はそのリストにある関数です。プログラムには `#include <neograph/mcp/http_server.h>`、`<cstdlib>`、`<string_view>` も必要です。

```cpp
int main(int argc, char** argv) {
    auto engine = build_engine();

    mcp::MCPHttpServerConfig http;
    http.host = "127.0.0.1";
    http.port = argc > 1 ? std::atoi(argv[1]) : 8000;  // 0 picks a free port
    http.endpoint = "/mcp";

    // The factory runs once per HTTP session and must return a fresh server.
    mcp::MCPHttpServer server(
        [engine](std::string_view /*authorization scope*/) {
            mcp::MCPHttpServerSession session;
            session.server = make_server(engine);
            return session;
        },
        http);
    server.start();  // blocks until stop(); start_async() returns at once
    return 0;
}
```

- HTTP セッション1つが `MCPServer` 1つに対応します。ファクトリーはセッションごとに1回実行され、新しいサーバーにツールを登録する必要があります。引数は `bearer_authorizer` が返したスコープなので、プリンシパルごとにセッションを分けられます。
- `MCPHttpServerConfig::host` の既定値は `127.0.0.1` です。それ以外のアドレスにバインドすると、`bearer_authorizer` を設定していない限り `std::invalid_argument` が投げられます。認可関数はベアラートークンを受け取り、呼び出し元のスコープを返します。`std::nullopt` か空文字列を返すと、トークンを `401` で拒否します。セッションはスコープに結び付くので、別のプリンシパルが漏えいした `Mcp-Session-Id` を使い回すことはできません。
- `Origin` ヘッダーを持つリクエストは、そのオリジンが `allowed_origins` に載っていなければ `403` で拒否されます。ループバックにバインドしたサーバーは、バインドしたポートの自分自身のループバックオリジン (`http://localhost:<port>`、`http://127.0.0.1:<port>`、`http://[::1]:<port>`) も受け入れ、それぞれを初めて受け入れたときに stderr に 1 行書きます。ブラウザーのインスペクターや開発サーバーのように別のポートのページは、一覧に載せる必要があります。DNS リバインディングのページは自分のオリジンを送るため拒否されます。
- ループバックにバインドしたサーバーは `Host` も確認します。ポートにかかわらず `localhost`、`127.0.0.1`、`[::1]` と、`allowed_hosts` に載せた名前にだけ応答します。それ以外の `Host` は `Origin` の有無にかかわらず `403` で拒否されるため、DNS リバインディングのページは `Origin` を持たないリクエストでも拒否されます。`Host` のないリクエスト、または `Host` が `host[:port]` の形でないリクエストは `400` を受け取ります。エントリーは `mcp.example.com` や `[::1]` のようなポートのない名前で、大文字と小文字を区別せずに比較され、`*` はどの `Host` も受け入れます。公開名をそのまま転送するローカルのリバースプロキシやトンネルの背後では、その名前を一覧に載せてください。それ以外のバインドでは、一覧が空なら何も確認せず、空でなければ一覧の名前だけを受け入れます。
- `initialize` 以降のリクエストは、`MCP-Protocol-Version` ヘッダーに `MCP_SUPPORTED_PROTOCOL_VERSIONS` のどのリビジョンを書いてもかまいません。セッションはネゴシエートしたリビジョンのまま動作し、セッション内で別のリビジョンを書いた最初のリクエストは stderr に記録されます。サポートしない値や形式の正しくない値は `400` で拒否され、ヘッダーのないリクエストにはネゴシエートしたリビジョンが使われます。
- `protected_resource_metadata` を設定すると、RFC 9728 のメタデータを公開し、すべての `401` チャレンジでその場所を示します。トークンを OAuth 認可サーバーから受け取る構成のための機能です。設定しなければ、固定のベアラートークンにディスカバリーは不要です。
- 制限: `max_sessions`(64。最も長く使われていないアイドルセッションから退去)、`session_idle_timeout`(30分)、`max_payload_bytes`(1 MiB)、`response_timeout`(10分。それより長くかかるリクエストは `504` を受け、その呼び出しはキャンセルされます)。
- このトランスポートは POST ごとに `application/json` の応答を1つ返し、`GET` には `405` で答えます。サーバーからクライアントへのイベントストリームは提供しません。

## インタラプト、キャンセル、エラー

- MCP のツール呼び出しは、一時停止して再開することができません。ハンドラーが実行したグラフが停止した場合(`RunResult::interrupted`)、クライアントに何を見せるかはハンドラーが決めます。ハンドラーが `NodeInterrupt` を投げると、その呼び出しに入力が必要だと伝わり、クライアントは JSON-RPC エラー `-32011` と `data` `{code: "NG.Interrupted", reason, value}` を受け取ります。
- ハンドラーがそれ以外の例外を投げると、`isError: true` の結果が返り、`_meta["io.neograph/failure"]` に安定したコードと `retryable`、`retryable_by_class` が入ります([プロトコル失敗のマッピング](reference-en.md#protocol-failure-mapping))。クライアントに届くのは失敗の公開メッセージだけで、例外のテキストは届きません。コードとメッセージを自分で決めるには、`NodeFailureError` を投げてください。
- クライアントがキャンセルした呼び出しには、どのトランスポートでも応答が返りません。未知のツールは `-32602` です。
- エージェントで `MCPTool`(`client.get_tools()` で得たツール)が失敗したとき、モデルが読む内容は、サーバーがツールを実行したかどうかで変わります。サーバーが実行前に拒否したリクエストは、サーバーのメッセージを持ち、`retryable` と `effect_uncertain` がどちらも false の通常の `failed` 結果です。対象は、JSON-RPC エラー `-32700`、`-32600`、`-32601`、`-32602` と、`NG.InvalidRequest` の目印が付いた `isError` 結果で、`MCPServer` は入力スキーマに合わない引数にこの目印を付けます。何も実行されていないので、モデルは引数を直せます。それ以外の失敗は、ツールが開始した後に起きた可能性があるため(ハンドラーが返した `isError` 結果、`-32603`、サーバーエラー)、効果を除外する `ToolExecutionPolicy` がなければ、`effect_uncertain` が true の `reconciliation_required` になります。通常の `failed` にするには、読み取り専用または冪等なツールをポリシーに宣言してください。[ツール失敗の契約](reference-en.md#tool-failure-contract)を参照してください。
- MCP、gRPC、A2A、ACP 全体の失敗クラスの表は、リファレンスの[プロトコル失敗マッピング](reference-en.md#protocol-failure-mapping)にあります。ここには再掲しません。

## テストと相互運用性

NeoGraph のサーバーとクライアントは、CI(`.github/workflows/mcp-interop.yml`)で公式 SDK と突き合わせて検査しています。対象は Python SDK の `mcp` 2.3.0 と 1.30.0、TypeScript SDK の 1.32.1 と 2.3.1 です。テストスイートは `bindings/python/tests/interop/mcp` にあり、テストが接続するサーバーは `tests/fixtures/mcp_interop_probe_server.cpp` です。このサーバーは、構造化出力、エラー、キャンセル、メタデータを扱うツールを登録しているので、読めばサーバー API の全体を手早く把握できます。上のようなグラフのサーバーは、変更なしで公式 Python SDK のクライアント(`streamable_http_client`、`ClientSession`)にも応答します。
