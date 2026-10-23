<!-- neograph-i18n: source=docs/mcp.md locale=zh-CN source_sha256=b9bf9962a4940a79256307c271914555f4c900c6f3ae49155c71703f8c96c16e -->
# MCP：把图公开为工具，以及使用工具的图

**Languages:** [English](mcp.md) | [한국어](mcp.ko.md) | [日本語](mcp.ja.md) | [简体中文](mcp.zh-CN.md)

Model Context Protocol（MCP）在 JSON-RPC 2.0 之上规定了客户端如何发现并调用服务器上的工具。客户端通常是智能体宿主、编辑器或使用模型的应用。NeoGraph 两种角色都能担任。作为服务器，它发布工具，通常一张图对应一个工具。作为客户端，它调用其他服务器发布的工具，调用可以来自图的节点，也可以来自模型的工具调用。

请不要把本页与 [Harness MCP](HARNESS_MCP.md) 混淆。那份文档介绍的是一个现成的 MCP 服务器，它有 6 个工具，用于编译和运行多工作者工作流。本页讲的是如何用 `neograph::mcp` 库搭建你自己的服务器或客户端。

## 组件

| 角色 | C++ 目标 | CMake 选项（默认值） | Python |
|---|---|---|---|
| 客户端（stdio 和 Streamable HTTP） | `neograph::mcp` | `NEOGRAPH_BUILD_MCP_CLIENT`（开） | `neograph_engine.mcp` |
| stdio 服务器 | `neograph::mcp_server` | `NEOGRAPH_BUILD_MCP_SERVER`（开） | 无 |
| Streamable HTTP 服务器 | `neograph::mcp_http_server` | `NEOGRAPH_BUILD_MCP_HTTP_SERVER`（**关**） | 无 |

`NEOGRAPH_BUILD_MCP`（开）决定客户端和 stdio 服务器两个选项的默认值。HTTP 服务器需要自行开启。Python 包里没有 MCP 服务器；要在 Python 中提供服务，请使用官方 MCP Python SDK，并在工具函数里调用已编译的引擎。

## 协议版本

服务器和客户端使用 MCP `2025-11-25`，同时接受 `2025-06-18`、`2025-03-26` 和 `2024-11-05`。服务器按客户端请求的修订版本应答，其他请求则用 `2025-11-25` 应答。传输方式只有 stdio 和 Streamable HTTP，不支持较早的 HTTP+SSE 传输。`2026-07-28` 修订版（没有 `initialize`，没有会话）也暂不支持：只提供该修订版的服务器会以 `-32022` 拒绝握手，调用方会收到 `MCPError`。详情见 [C++ 参考](reference-en.md#13-mcp-module)。

## 通过 stdio 把图公开为工具

这个程序构建一张只有一个节点、用于统计单词数的图，并把它发布为工具 `word_count`。不需要 API 密钥。每次工具调用都会把图运行一次。

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

工作方式如下：

- 工具由名称、参数的 JSON Schema（`input_schema` 必须写明 `"type": "object"`）和处理函数组成。服务器在运行处理函数之前，先按 Schema 检查每次调用的参数。参数无效时，返回的结果带有 `isError: true`、校验信息，以及代码为 `NG.InvalidRequest` 的 `_meta["io.neograph/failure"]`，NeoGraph 客户端据此得知处理函数没有运行。
- 处理函数在固定大小的工作线程池中运行：`MCPServerConfig::max_concurrent_calls`（4）个同时运行，`max_pending_calls`（64）个排队等待；超出的调用会收到 JSON-RPC 错误 `-32000`（"MCP tool queue is full"）。引擎由这些线程共享。节点会在多次运行中复用，所以每次运行的数据要放在通道里，不要放在节点对象里。如果引擎配置了检查点存储，请为每次调用指定不同的 `RunConfig::thread_id`。
- 客户端发送 `notifications/cancelled` 后，该调用的 `CancelToken` 随之取消。把它作为 `run.cancel_token` 传入，图的运行也会随之停止。
- `run()` 从 stdin 每行读取一条 JSON-RPC 消息，并写到 stdout。程序在 stdout 上输出任何其他内容都会破坏协议，日志请写到 stderr。

链接 `neograph::core` 和 `neograph::mcp_server`：

```cmake
add_executable(word_count_server word_count_server.cpp)
target_link_libraries(word_count_server PRIVATE neograph::core neograph::mcp_server)
```

要在 MCP 宿主中使用，把这个可执行文件注册为不带参数的 stdio 服务器即可。

## 从客户端调用

把服务器作为子进程启动，然后调用工具。`get_tool_definitions()` 会读取所有结果页；`call_tool_result()` 返回应答中的 content、`structuredContent` 和 `isError`。

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

Python 中的写法相同。客户端对象被释放时，子进程随之结束：

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

以 `./mcp_client ./word_count_server` 启动 C++ 程序，输出如下：

```text
tool: word_count - Count the words in a text by running a NeoGraph graph.
is_error: false
text: 5
```

Python 脚本输出如下：

```text
word_count - Count the words in a text by running a NeoGraph graph.
False 5
```

0.14.0 的 wheel 包含这个 Python 客户端：stdio 传输可以配合上面的服务器工作，HTTP 客户端可以配合下文的 HTTP 服务器工作，也提供 `MCPClient.shutdown()`。0.13.1 及更早的 wheel 没有 `shutdown()`，其 HTTP 客户端也无法在该 HTTP 服务器上建立会话；使用这些 wheel 时，请从当前源码构建。

对于 HTTP，`MCPClient("http://127.0.0.1:8000/mcp")` 接收服务器的端点 URL，并原样使用。发现到的工具会保留自己的会话，因此在 `MCPClient` 对象销毁之后仍可使用。

stdio 服务器只向 stdout 写入消息。如果服务器仍在那里打印横幅或日志行，客户端会跳过不是 JSON 的行，每个会话最多跳过 `StdioClientConfig::max_stdout_text_lines`（1000）行、`max_stdout_text_bytes`（256 KiB），并在跳过第一行时向宿主的 stderr 写一次警告，不引用该文本。超过上限之后的下一行会以 `MCPFailure::protocol` 结束会话；任一上限设为 0 则什么都不跳过。可能是损坏消息的行不会被跳过：以 `{` 开头、以 `[` 后接 `{` 开头或包含 `"jsonrpc"` 的行，以及不是对象或批量的 JSON，都会立即结束会话，因为跳过它们会让它们所答复的调用一直等到超时。服务器把日志写到 stderr。只接收 `argv` 的构造函数使用默认值。

## 在图节点中调用工具

节点可以持有一个客户端并调用工具。运行的截止时间和取消令牌会限制这次调用；如果运行取消，客户端会向服务器发送 `notifications/cancelled`，并丢弃迟到的应答。

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

传入上一节服务器的路径来运行：`./remote_word_count ./word_count_server`，输出 `count: 8`。

如果要让模型自己选择工具，用 `MCPClient::get_tools()` 把服务器的工具取成 NeoGraph 的 `Tool` 对象，放入 `NodeContext::tools`（Python 中是 `ng.NodeContext(tools=client.get_tools())`），`tool_dispatch` 节点就会执行模型请求的调用。[C++ 示例](../examples/README.md)里有 MCP 智能体（03、20、22 至 24），不需要 API 密钥：仅用标准库的演示服务器 `examples/mcp_demo_server.py` 提供工具，由脚本化的模型充当 LLM。加上 `--live` 可换用真实模型，这时需要 `OPENROUTER_API_KEY`。[Python 示例 26](../bindings/python/examples/README.md) 自己启动本地 MCP 服务器，同样不需要密钥。

## 通过 Streamable HTTP 提供服务

链接 `neograph::mcp_http_server`，用 `-DNEOGRAPH_BUILD_MCP_HTTP_SERVER=ON` 构建，然后替换上面程序的 `main`。`make_server` 就是该清单中的函数，程序还需要 `#include <neograph/mcp/http_server.h>`、`<cstdlib>` 和 `<string_view>`。

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

- 一个 HTTP 会话对应一个 `MCPServer`。工厂函数每个会话运行一次，必须在新服务器上注册工具。它的参数是 `bearer_authorizer` 返回的作用域，因此程序可以把不同主体的会话隔离开。
- `MCPHttpServerConfig::host` 默认为 `127.0.0.1`。绑定到其他地址时，除非设置了 `bearer_authorizer`，否则会抛出 `std::invalid_argument`。授权函数接收 Bearer 令牌并返回调用者的作用域；返回 `std::nullopt` 或空字符串，则以 `401` 拒绝该令牌。会话与作用域绑定，所以其他主体无法复用泄露的 `Mcp-Session-Id`。
- 带有 `Origin` 头的请求，如果该来源不在 `allowed_origins` 中，会被以 `403` 拒绝。绑定到回环地址时，服务器还接受它自己在所绑定端口上的回环来源（`http://localhost:<port>`、`http://127.0.0.1:<port>`、`http://[::1]:<port>`），并在第一次接受每个来源时向 stderr 写一行。其他端口上的页面（例如浏览器检查器或开发服务器）必须列入该列表。DNS 重绑定页面发送的是它自己的来源，因此会被拒绝。
- 绑定到回环地址时，服务器还会检查 `Host`。无论端口如何，它只应答 `localhost`、`127.0.0.1`、`[::1]`，以及 `allowed_hosts` 中列出的名称。其他 `Host` 无论是否带有 `Origin` 都会被以 `403` 拒绝，因此即使请求不带 `Origin`，DNS 重绑定页面也会被拒绝。没有 `Host` 的请求，或 `Host` 不是 `host[:port]` 形式的请求，得到 `400`。条目是不带端口的名称，例如 `mcp.example.com` 或 `[::1]`，比较时不区分大小写；`*` 接受任何 `Host`。在原样转发公网名称的本地反向代理或隧道之后，请把该名称列入列表。绑定到其他地址时，列表为空则不做任何检查，非空则只接受列出的名称。
- `initialize` 之后的请求可以在 `MCP-Protocol-Version` 头中写 `MCP_SUPPORTED_PROTOCOL_VERSIONS` 里的任一修订版本。会话仍按协商的修订版本工作；一个会话中第一个写了其他修订版本的请求会记录到 stderr。不支持或格式不正确的值以 `400` 拒绝，没有该头的请求使用协商的修订版本。
- 设置 `protected_resource_metadata` 后，服务器会发布 RFC 9728 元数据，并在每个 `401` 质询中指出其位置，适用于令牌来自 OAuth 授权服务器的部署。不设置时，静态 Bearer 令牌不需要发现机制。
- 限制：`max_sessions`（64；最久未使用的空闲会话会被淘汰）、`session_idle_timeout`（30 分钟）、`max_payload_bytes`（1 MiB）、`response_timeout`（10 分钟；运行更久的请求会收到 `504`，对应的调用被取消）。
- 该传输对每个 POST 返回一个 `application/json` 响应，对 `GET` 返回 `405`，不提供服务器到客户端的事件流。

## 中断、取消和错误

- MCP 工具调用无法暂停后再恢复。如果处理函数运行的图暂停了（`RunResult::interrupted`），客户端看到什么由处理函数决定。处理函数抛出 `NodeInterrupt`，表示这次调用需要输入：客户端会收到 JSON-RPC 错误 `-32011`，其 `data` 为 `{code: "NG.Interrupted", reason, value}`。
- 处理函数抛出其他异常时，返回 `isError: true` 的结果，稳定的错误码以及 `retryable`、`retryable_by_class` 放在 `_meta["io.neograph/failure"]` 中（[协议失败映射](reference-en.md#protocol-failure-mapping)）。到达客户端的只有失败的公开消息，不包含异常文本。想自己指定错误码和消息，请抛出 `NodeFailureError`。
- 被客户端取消的调用，在任何传输上都不会收到应答。未知工具返回 `-32602`。
- 在智能体中，`MCPTool`（由 `client.get_tools()` 得到）失败时模型读到什么，取决于服务器有没有运行过这个工具。服务器在运行前拒绝的请求，是带有服务器消息、`retryable` 与 `effect_uncertain` 均为 false 的普通 `failed` 结果。这包括 JSON-RPC 错误 `-32700`、`-32600`、`-32601`、`-32602`，以及标记为 `NG.InvalidRequest` 的 `isError` 结果，`MCPServer` 用它标记不符合输入 schema 的参数。什么都没有运行，模型可以修正参数。其他失败可能发生在工具开始之后（处理函数返回的 `isError` 结果、`-32603`、服务器错误），因此没有能排除效果的 `ToolExecutionPolicy` 时，结果为 `reconciliation_required`，`effect_uncertain` 为 true。想得到普通的 `failed`，请在策略中声明只读或幂等的工具。参见[工具失败契约](reference-en.md#tool-failure-contract)。
- MCP、gRPC、A2A、ACP 各类失败的完整对照表在参考文档的[协议失败映射](reference-en.md#protocol-failure-mapping)中，这里不再重复。

## 测试与互操作

NeoGraph 的服务器和客户端在 CI（`.github/workflows/mcp-interop.yml`）中与官方 SDK 对照检查：Python SDK `mcp` 2.3.0 和 1.30.0，以及 TypeScript SDK 1.32.1 和 2.3.1。测试套件在 `bindings/python/tests/interop/mcp`，测试所连接的服务器是 `tests/fixtures/mcp_interop_probe_server.cpp`。它注册了涉及结构化输出、错误、取消和元数据的工具，读一遍就能快速了解整个服务器 API。像上面那样的图服务器，不用修改也能响应官方 Python SDK 的客户端（`streamable_http_client`、`ClientSession`）。
