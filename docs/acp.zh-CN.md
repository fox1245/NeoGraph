<!-- neograph-i18n: source=docs/acp.md locale=zh-CN source_sha256=d6c0969438c8807fc2a0db7bad4789dbf7f7110c6b864b32417f0715f51bd282 -->
# ACP：把图作为编辑器中的智能体

**Languages:** [English](acp.md) | [한국어](acp.ko.md) | [日本語](acp.ja.md) | [简体中文](acp.zh-CN.md)

Agent Client Protocol（ACP）把编辑器与编码智能体或对话智能体连接起来。编辑器（Zed、使用 CodeCompanion 的 Neovim、Gemini CLI 等）把智能体作为子进程启动，在智能体的 stdin 和 stdout 上交换 JSON-RPC 2.0 消息，每行一个 JSON 对象。编辑器打开会话并发送提示词，智能体把回答以更新的形式流式返回。智能体还可以向编辑器请求权限，或请求访问文件和终端。

`ACPServer` 把编译好的 NeoGraph 图作为这样的智能体运行。NeoGraph 实现的是智能体一侧，编辑器是客户端。`ACPClient` 是智能体向编辑器发出自己的请求时使用的句柄。编辑器以错误应答时会抛出 `acp::ACPClientError`。如果代码表明请求不可能成功，例如对不存在的文件返回 `-32002`，它就是 `acp::ACPClientRefusal`，节点不会重试。

## 组件

| 角色 | C++ 目标 | CMake 选项（默认值） | Python |
|---|---|---|---|
| 智能体（服务器） | `neograph::acp` | `NEOGRAPH_BUILD_ACP`（开） | 通过官方 SDK |
| 通过 Streamable HTTP 和 WebSocket 提供的智能体（实验性） | `neograph::acp_http_server` | `NEOGRAPH_BUILD_ACP_HTTP_SERVER`（**关**） | 无 |

Python 包中没有原生的 ACP 服务器。要用 Python 编写智能体，请安装官方 SDK（`pip install "neograph-engine[acp]"`），并在它背后用 `neograph_engine.ProtocolHostAdapter` 运行引擎。该适配器把 ACP 会话 ID 对应到 NeoGraph 的 `thread_id`，从检查点恢复，转发取消，并流式输出令牌。[Python 示例](../bindings/python/examples/README.md)里有这样的智能体。ACP 需要 Python 3.10 或更高版本。

## 版本

ACP 协议版本 1。消息按官方 ACP 模式 v1.25.0 做检查，服务器在 CI（`.github/workflows/protocol-interop.yml`，测试套件在 `bindings/python/tests/interop/acp_suite`）中与官方 Python SDK（`agent-client-protocol` 0.12.1，以及预发布版 1.0.0rc2）和 TypeScript SDK（`@agentclientprotocol/sdk` 1.8.0）一起测试。

## 把图发布为智能体

这个程序就是 [A2A 指南](a2a.md)中的发布说明图：`draft` 根据提示词写发布说明，`publish` 只在有人批准之后才运行。不需要 API 密钥。

```cpp
#include <neograph/acp/server.h>
#include <neograph/neograph.h>

#include <memory>
#include <string>

using namespace neograph;
using namespace neograph::graph;

namespace {

// draft: write a draft from the incoming prompt.
class DraftNode final : public SyncGraphNode {
public:
    DraftNode() : SyncGraphNode("draft") {}

protected:
    NodeOutput run_sync(NodeInput input) override {
        const auto topic = input.state.get(ChannelKey<std::string>{"prompt"});
        return NodeOutput{{ChannelWrite{"draft", json("Release notes: " + topic)}}};
    }
};

// publish: runs only after a person has answered; the answer is the resume value.
class PublishNode final : public SyncGraphNode {
public:
    PublishNode() : SyncGraphNode("publish") {}

protected:
    NodeOutput run_sync(NodeInput input) override {
        const auto draft = input.state.get(ChannelKey<std::string>{"draft"});
        const auto answer = input.ctx.resume_value ? input.ctx.resume_value->get<std::string>() : "";
        const bool approved = answer.rfind("approve", 0) == 0;  // "approve" or "approved"
        const auto reply = approved ? "Published: " + draft : "Not published (" + answer + "): " + draft;
        return NodeOutput{{ChannelWrite{"response", json(reply)}}};
    }
};

}  // namespace

int main() {
    NodeFactory::instance().register_type(
        "docs.draft", [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<DraftNode>();
        });
    NodeFactory::instance().register_type(
        "docs.publish", [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<PublishNode>();
        });

    // The ACP server reads the user's text from the "prompt" channel and the
    // agent's answer from the "response" channel.
    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "release-notes"},
        {"channels", {{"prompt", {{"reducer", "overwrite"}, {"initial", ""}}},
                      {"draft", {{"reducer", "overwrite"}, {"initial", ""}}},
                      {"response", {{"reducer", "overwrite"}, {"initial", ""}}}}},
        {"nodes", {{"draft", {{"type", "docs.draft"}}},
                   {"publish", {{"type", "docs.publish"}}}}},
        {"edges", {{{"from", "__start__"}, {"to", "draft"}},
                   {{"from", "draft"}, {"to", "publish"}},
                   {{"from", "publish"}, {"to", "__end__"}}}},
        {"interrupt_before", json::array({"publish"})},
    };

    // A checkpoint store is what lets a turn wait for an answer and resume.
    EngineConfig config;
    config.checkpoint_store = std::make_shared<InMemoryCheckpointStore>();
    std::shared_ptr<GraphEngine> engine = GraphEngine::build_strict(definition, std::move(config));

    // Anything the agent prints on stdout is protocol traffic; log to stderr.
    acp::ACPServer server(engine, json{{"name", "release-notes"}, {"version", "1.0.0"}});
    server.run();  // newline-delimited JSON-RPC on stdin and stdout, until EOF
    return 0;
}
```

工作方式如下：

- `session/prompt` 把提示词的文本放进图的 `prompt` 通道。`response` 通道的文本作为一条 `agent_message_chunk` 更新返回，这一轮以 `stopReason: "end_turn"` 结束。要改变这一点，请在 `ACPGraphAdapter` 中重写 `input_channel()`、`output_channel()`、`build_session_state()`（用于给工作目录等通道赋初值）或下面的中断钩子，并把适配器作为构造函数的第三个参数传入。
- 图中断会在同一轮里变成权限请求。发生 `interrupt_before` 或 `NodeInterrupt` 时，服务器发送 `session/request_permission`，选项为 `approve`（`allow_once`）和 `reject`（`reject_once`）。编辑器所选选项的 ID 就是恢复值，所以上面的 `publish` 收到的是 `"approve"` 或 `"reject"`。要改变选项或恢复值，请重写 `interrupt_permission()` 和 `interrupt_resume_value()`。如果编辑器回答 `cancelled`，这一轮以 `cancelled` 结束。
- `run()` 一直读取 stdin，直到它关闭。程序在 stdout 上写入其他内容会破坏协议，日志请写到 stderr。
- 服务器同时最多处理 32 个提示词，每个会话一个（`set_max_inflight_prompts`）。被拒绝的提示词会收到 JSON-RPC 错误 `-32603`，其 `data.reason` 为 `"overloaded"` 或 `"session_busy"`。

链接 `neograph::core` 和 `neograph::acp`：

```cmake
add_executable(release_notes_agent release_notes_agent.cpp)
target_link_libraries(release_notes_agent PRIVATE neograph::core neograph::acp)
```

编辑器启动这个可执行文件，并在它的 stdin 和 stdout 上使用 ACP。具体如何注册，取决于编辑器。

## 不用编辑器试一试

这个脚本扮演编辑器的角色。它启动智能体、打开会话、发送提示词、批准权限请求，并打印发生的事情。

```python
import json
import subprocess
import sys

# Start the agent the way an editor does: a child process, JSON-RPC on stdin/stdout.
agent = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)


def send(message):
    agent.stdin.write(json.dumps(message) + "\n")
    agent.stdin.flush()


def call(request_id, method, params):
    """Send a request and return its result, answering the agent's own requests on the way."""
    send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
    while True:
        message = json.loads(agent.stdout.readline())
        if message.get("method") == "session/request_permission":
            options = [option["optionId"] for option in message["params"]["options"]]
            print("agent asks:", message["params"]["toolCall"]["title"], options)
            send({"jsonrpc": "2.0", "id": message["id"],
                  "result": {"outcome": {"outcome": "selected", "optionId": "approve"}}})
        elif message.get("method") == "session/update":
            print("update:", message["params"]["update"]["content"]["text"])
        elif "method" not in message and message.get("id") == request_id:
            return message["result"]


info = call(1, "initialize", {"protocolVersion": 1, "clientCapabilities": {}})["agentInfo"]
print("agent:", info["name"], info["version"])
session = call(2, "session/new", {"cwd": "/tmp", "mcpServers": []})["sessionId"]
result = call(3, "session/prompt",
              {"sessionId": session, "prompt": [{"type": "text", "text": "version 2.0"}]})
print("stop reason:", result["stopReason"])
agent.stdin.close()
agent.wait()
```

传入智能体的路径来运行：`python acp_client.py ./release_notes_agent`。输出如下：

```text
agent: release-notes 1.0.0
agent asks: Interrupt before node: publish ['approve', 'reject']
update: Published: Release notes: version 2.0
stop reason: end_turn
```

把 `"approve"` 改成 `"reject"`，更新就变成 `Not published (reject): Release notes: version 2.0`。

## 取消、会话和错误

- **取消。** `session/cancel` 取消正在运行的那一轮，这一轮以 `stopReason: "cancelled"` 结束；没有运行中的轮次时，取消请求不起作用。按请求 id 指向正在运行的 `session/prompt` 的 `$/cancel_request` 也以同样的方式取消该轮；指向其他请求的则被忽略。stdin 到达文件末尾时，`run()` 会等运行中的轮次 `set_eof_grace`（1 秒）让它们结束，然后取消。
- **一轮如何结束。** 运行完成是 `end_turn`，取消则是 `cancelled`，运行达到 `RunConfig::max_steps` 是 `max_turn_requests`，超出模型令牌预算是 `max_tokens`，provider 拒绝是 `refusal`。其他失败是 JSON-RPC 错误 `-32603`，其 `data` 为 `{code, retryable, retryable_by_class}`：引擎用完节点的尝试次数后 `retryable` 为 `false`，`retryable_by_class` 表示失败的类别是否允许重试；消息是失败的公开消息，不是异常文本。完整的对照表在参考文档的 [ACP](reference-en.md#neographacp--agent-client-protocol) 和[协议失败映射](reference-en.md#protocol-failure-mapping)中。
- **会话。** `session/new` 创建会话，`session/resume` 继续智能体已知的会话。引擎配置了检查点存储时，停在权限请求上的会话可以在智能体重启后用 `session/resume` 恢复；这时下一条提示词的文本就是对待答问题的回答。在 `initialize` 之前调用是 `-32600`，未知的会话是 `-32002`。
- **MCP 服务器。** 客户端可以在 `session/new` 中传入 stdio MCP 服务器。智能体用 NeoGraph 的 MCP 客户端，在会话的 `cwd` 中以最小环境（`PATH` 和客户端的 `env` 条目）启动它们，所以智能体环境中的 provider 密钥不会传给它们。节点通过 `ACPClient::session_tools(session_id)` 使用这些工具。HTTP 和 SSE 的 MCP 服务器会以 `-32602` 拒绝。见 [MCP 指南](mcp.md)。
- **能力。** `ACPServer::capabilities()` 就是 `initialize` 所公布的内容。提示词接受文本和 `resource_link` 块；图像、音频和嵌入资源只有在 `capabilities().prompt` 中开启后才接受。

## 远程传输（实验性）

stdio 是 ACP 定义、所有编辑器都实现的传输。远程传输，即在同一端点上同时支持 WebSocket 升级的 Streamable HTTP，目前只是 ACP 项目的草案提案。`neograph::acp_http_server` 通过这两种方式提供智能体，并在草案写得明确的地方遵循它，所以草案变化时，线上格式的细节可能随之变化。它是**实验性的，默认关闭**：一个单独的库（`NEOGRAPH_BUILD_ACP_HTTP_SERVER`，默认关），不要求就不会被链接；服务器只有在 `start()` 之后才会监听。stdio 上的 `ACPServer::run()` 不变。

```cpp
#include <neograph/acp/http_server.h>

// One ACPServer per connection: the factory builds it, the transport drives it.
acp::ACPHttpServer server([engine](std::string_view /*principal*/) {
    acp::ACPHttpConnection connection;
    connection.server = std::make_unique<acp::ACPServer>(
        engine, json{{"name", "release-notes"}, {"version", "1.0.0"}});
    return connection;
});
server.start_async();  // 127.0.0.1 and a free port, see server.port()
```

链接 `neograph::acp_http_server`，它会带上 `neograph::acp`。工厂对每个连接调用一次，参数是可选的 `authenticator` 返回的主体（principal），没有则为空字符串。

- **连接。** 一个 ACP 连接就是一个 `ACPServer`，驱动方式与 stdio 完全相同：消息按到达顺序处理，连接结束就是输入结束，所以它正在运行的提示词会被取消。连接彼此独立，可以在同一个引擎上同时运行提示词，`max_connections`（16）限制其数量。会话 ID 属于一个存活的连接：对另一个存活连接持有的 ID 执行 `session/resume`，会被以 `-32603` 和 `data.reason` 为 `"session_busy"` 拒绝。连接的会话随连接结束；重新连接之后，`session/resume` 从引擎的检查点存储恢复会话。调用编辑器的节点捕获 `ACPHttpServer::client()`（或把未绑定的 `ACPClient` 传给 `attach_client()`），每次调用都会到达拥有该会话的连接。
- **Streamable HTTP** 在 `ACPHttpConfig::endpoint`（`/acp`）上提供。不带 `Acp-Connection-Id` 的 `initialize` `POST` 会创建连接，以 `200` 应答，响应作为正文，ID 放在 `Acp-Connection-Id` 头中。其他 `POST` 都带这个头，以没有正文的 `202` 应答，真正的答复通过流送达。带 `Accept: text/event-stream` 的 `GET` 打开一个 server-sent-event 流：只带这个头的是连接流（`session/new` 和 `session/resume` 的答复），再带 `Acp-Session-Id` 的是会话流（`session/update`、`session/request_permission` 和 `fs/*` 这类智能体的请求，以及该会话请求的答复）。由 `session/resume` 恢复的会话在客户端打开流之前没有自己的流，其间它的消息走连接流。`DELETE` 结束连接。在 `params.sessionId` 中指明会话的请求必须发送相同的 `Acp-Session-Id`，且该会话必须属于这个连接。批量请求返回 `501`。没有打开的流时，消息在有上限的队列中等待；连接丢失后不会重发任何内容；既没有流也没有请求的时间达到 `connection_idle_timeout`（60 秒）的连接会被关闭，其提示词被取消。
- **WebSocket** 在同一端点上提供（带 `Upgrade: websocket` 的 `GET`）。每个文本帧是一条 JSON-RPC 消息，两个方向都是如此，第一条是 `initialize`；二进制帧会被忽略。帧遵循 RFC 6455：客户端帧必须带掩码，ping 会得到应答，消息可以分片。超过 `max_payload_bytes` 的消息会以代码 1009 关闭连接，关闭套接字即结束连接。
- **与草案的不同。** 草案要求 HTTP/2；本服务器使用 HTTP/1.1，因此每个流和每个 `POST` 都是独立的 TCP 连接，多路复用由反向代理提供。WebSocket 升级的 101 响应不带 `Acp-Connection-Id`。不设置 cookie，不检查 `Acp-Protocol-Version`，流也没有事件 ID 和续传。
- **安全。** 除非指定其他主机，否则绑定 `127.0.0.1`；非回环地址的绑定在没有设置 `authenticator` 时会被拒绝。每个请求、流和升级请求，以及服务器不提供的路径，都在读取正文之前检查：先是 Host 和 Origin（`allowed_hosts` 和 `allowed_origins`，规则与 [MCP](mcp.md) 和 [A2A](a2a.md) 服务器相同），然后是 `authenticator`（HTTP 401 和 `WWW-Authenticate: Bearer`），最后是 `max_payload_bytes`（HTTP 413，默认 4 MiB）。连接只对创建它的主体应答。智能体以编辑器的名义读写文件，所以要把远程绑定看作是把这种能力暴露出去。浏览器无法在 WebSocket 上设置 `Authorization`，这类连接请在反向代理中认证。

## 测试与互操作

`tests/fixtures/acp_probe_agent.cpp` 是用公开 API 搭建的小型 ACP 智能体。互操作测试套件让官方 SDK 客户端连接它。单元测试是 `tests/test_acp_server.cpp` 和 `tests/test_acp_types.cpp`；远程传输另有 `tests/test_acp_http_server.cpp`。`bindings/python/tests/interop/acp_suite/test_acp_sdk_remote_interop.py` 用官方 Python SDK（`agent-client-protocol` 0.12）为草案传输提供的客户端，在两种传输上对 `neograph_acp_probe_agent --http` 运行；TypeScript SDK 没有针对这些传输运行。
