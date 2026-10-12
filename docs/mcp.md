# MCP: tools from a graph, and graphs that use tools

**Languages:** [English](mcp.md) | [한국어](mcp.ko.md) | [日本語](mcp.ja.md) | [简体中文](mcp.zh-CN.md)

The Model Context Protocol (MCP) lets a client discover and call tools on a server over JSON-RPC 2.0. The client is usually an agent host, an editor or a model application. NeoGraph can play both roles. As a server it publishes tools, most often one tool per graph. As a client it calls tools that another server publishes, from a graph node or from a model's tool calls.

Do not confuse this page with [Harness MCP](HARNESS_MCP.md). That document describes a ready-made MCP server with six tools for compiling and running multi-worker workflows. This page is about building your own server or client with the `neograph::mcp` libraries.

## Components

| Role | C++ target | CMake option (default) | Python |
|---|---|---|---|
| Client, stdio and Streamable HTTP | `neograph::mcp` | `NEOGRAPH_BUILD_MCP_CLIENT` (on) | `neograph_engine.mcp` |
| Server over stdio | `neograph::mcp_server` | `NEOGRAPH_BUILD_MCP_SERVER` (on) | none |
| Server over Streamable HTTP | `neograph::mcp_http_server` | `NEOGRAPH_BUILD_MCP_HTTP_SERVER` (**off**) | none |

`NEOGRAPH_BUILD_MCP` (on) sets the default of the client and stdio server options. The HTTP server is opt-in. The Python package has no MCP server; to serve from Python, use the official MCP Python SDK and call a compiled engine from your tool function.

## Protocol versions

The server and the client speak MCP `2025-11-25` and accept `2025-06-18`, `2025-03-26` and `2024-11-05`. The server answers a client with the revision the client asks for, and any other request with `2025-11-25`. Only the stdio and Streamable HTTP transports exist; the older HTTP+SSE transport is not spoken. The `2026-07-28` revision (no `initialize`, no sessions) is not spoken either: a server that serves only that revision refuses the handshake with `-32022`, which reaches the caller as `MCPError`. The details are in the [C++ reference](reference-en.md#13-mcp-module).

## Serve a graph as a tool over stdio

This program builds a one-node graph that counts words and publishes it as the tool `word_count`. It needs no API key. Each tool call runs the graph once.

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

How it works:

- A tool is a name, a JSON Schema for its arguments (`input_schema` must say `"type": "object"`) and a handler. The server checks every call's arguments against the schema before the handler runs. Invalid arguments come back as a result with `isError: true` and the validation message.
- Handlers run on a fixed worker pool: `MCPServerConfig::max_concurrent_calls` (4) run at once and `max_pending_calls` (64) wait; a call beyond that gets JSON-RPC error `-32000` ("MCP tool queue is full"). The engine is shared by those threads. Nodes are reused across runs, so keep per-run data in channels, not in the node object. If the engine has a checkpoint store, give each call its own `RunConfig::thread_id`.
- When the client sends `notifications/cancelled`, the call's `CancelToken` is cancelled. Passing it as `run.cancel_token` makes the graph run stop.
- `run()` reads one JSON-RPC message per line from stdin and writes to stdout. Anything else the program prints on stdout corrupts the protocol; log to stderr.

Link it with `neograph::core` and `neograph::mcp_server`:

```cmake
add_executable(word_count_server word_count_server.cpp)
target_link_libraries(word_count_server PRIVATE neograph::core neograph::mcp_server)
```

To use it from an MCP host, register the executable as a stdio server with no arguments.

## Call it from a client

Start the server as a child process and call the tool. `get_tool_definitions()` follows all result pages; `call_tool_result()` returns the content, `structuredContent` and `isError` of the reply.

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

The same in Python. The child process ends when the client object is released:

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

The C++ program, started as `./mcp_client ./word_count_server`, prints:

```text
tool: word_count - Count the words in a text by running a NeoGraph graph.
is_error: false
text: 5
```

The Python script prints:

```text
word_count - Count the words in a text by running a NeoGraph graph.
False 5
```

The 0.13.1 wheel includes this Python client, and its stdio transport works against the server above. `MCPClient.shutdown()` is not in the wheel, and in our check the wheel's HTTP client could not open a session on the HTTP server described below; use a build of the current source for HTTP.

For HTTP, `MCPClient("http://127.0.0.1:8000/mcp")` takes the server's endpoint URL and uses it as given. A discovered tool keeps its session, so it stays usable after the `MCPClient` object is destroyed.

## Call a tool from a graph node

A node can hold a client and call a tool. The run's deadline and cancel token bound the call; if the run is cancelled, the client sends `notifications/cancelled` to the server and drops the late reply.

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

Run it with the path of the server from the previous section: `./remote_word_count ./word_count_server`. It prints `count: 8`.

For model-driven tool use, `MCPClient::get_tools()` returns the server's tools as NeoGraph `Tool` objects. Put them in `NodeContext::tools` (Python: `ng.NodeContext(tools=client.get_tools())`) and a `tool_dispatch` node runs the calls a model requests. The [C++ examples](../examples/README.md) and [Python examples](../bindings/python/examples/README.md) have MCP agents; they need a model and therefore an API key.

## Serve over Streamable HTTP

Link `neograph::mcp_http_server`, build with `-DNEOGRAPH_BUILD_MCP_HTTP_SERVER=ON`, and replace `main` of the program above. `make_server` is the function from that listing, and the program also needs `#include <neograph/mcp/http_server.h>`, `<cstdlib>` and `<string_view>`.

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

- An HTTP session is one `MCPServer`. The factory runs once per session and must register the tools on a new server. Its argument is the scope that `bearer_authorizer` returned, so a program can keep sessions of different principals apart.
- `MCPHttpServerConfig::host` defaults to `127.0.0.1`. A bind to any other address throws `std::invalid_argument` unless `bearer_authorizer` is set. The authorizer receives the bearer token and returns the caller's scope; `std::nullopt` or an empty string refuses the token with `401`. A session is bound to its scope, so another principal cannot reuse a leaked `Mcp-Session-Id`.
- A request that carries an `Origin` header is refused with `403` unless the origin is listed in `allowed_origins`.
- `protected_resource_metadata` publishes RFC 9728 metadata and names it in every `401` challenge, for deployments whose tokens come from an OAuth authorization server. Without it, static bearer tokens need no discovery.
- Limits: `max_sessions` (64; the least recently used idle session is evicted), `session_idle_timeout` (30 minutes), `max_payload_bytes` (1 MiB), `response_timeout` (10 minutes; a request that runs longer gets `504` and its call is cancelled).
- This transport returns one `application/json` response per POST and answers `GET` with `405`; it does not offer a server-to-client event stream.

## Interrupts, cancellation and errors

- An MCP tool call cannot pause and resume. If your handler runs a graph that pauses (`RunResult::interrupted`), the handler decides what the client sees. A handler that throws `NodeInterrupt` tells the client that the call needs input: the client gets JSON-RPC error `-32011` with `data` `{code: "NG.Interrupted", reason, value}`.
- A handler that throws any other exception gives a result with `isError: true` and `_meta["io.neograph/failure"]` holding the stable code. Only the failure's public message reaches the client, never the exception text. Throw `NodeFailureError` to choose the code and message.
- A call the client cancels gets no response, on every transport. An unknown tool is `-32602`.
- The full table of failure classes across MCP, gRPC, A2A and ACP is in the [protocol failure mapping](reference-en.md#protocol-failure-mapping) of the reference. It is not repeated here.

## Tests and interoperability

NeoGraph's server and client are checked against the official SDKs in CI (`.github/workflows/mcp-interop.yml`): the Python SDK `mcp` 2.3.0 and 1.30.0, and the TypeScript SDK 1.32.1 and 2.3.1. The suites are in `bindings/python/tests/interop/mcp`, and the server they talk to is `tests/fixtures/mcp_interop_probe_server.cpp`, which registers tools for structured output, errors, cancellation and metadata. Reading it is a quick way to see the whole server API. A graph server like the one above also answers the official Python SDK client (`streamable_http_client`, `ClientSession`) without changes.
