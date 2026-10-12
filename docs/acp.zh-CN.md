<!-- neograph-i18n: source=docs/acp.md locale=zh-CN source_sha256=d77b934a500c3e111fd61587bca9025e698ac91bf4048ddb2d4eb56b1de80fc4 -->
# ACP：把图作为编辑器中的智能体

**Languages:** [English](acp.md) | [한국어](acp.ko.md) | [日本語](acp.ja.md) | [简体中文](acp.zh-CN.md)

Agent Client Protocol（ACP）把编辑器与编码智能体或对话智能体连接起来。编辑器（Zed、使用 CodeCompanion 的 Neovim、Gemini CLI 等）把智能体作为子进程启动，在智能体的 stdin 和 stdout 上交换 JSON-RPC 2.0 消息，每行一个 JSON 对象。编辑器打开会话并发送提示词，智能体把回答以更新的形式流式返回。智能体还可以向编辑器请求权限，或请求访问文件和终端。

`ACPServer` 把编译好的 NeoGraph 图作为这样的智能体运行。NeoGraph 实现的是智能体一侧，编辑器是客户端。`ACPClient` 是智能体向编辑器发出自己的请求时使用的句柄。

## 组件

| 角色 | C++ 目标 | CMake 选项（默认值） | Python |
|---|---|---|---|
| 智能体（服务器） | `neograph::acp` | `NEOGRAPH_BUILD_ACP`（开） | 通过官方 SDK |

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

- **取消。** `session/cancel` 取消正在运行的那一轮，这一轮以 `stopReason: "cancelled"` 结束；没有运行中的轮次时，取消会被忽略。stdin 到达文件末尾时，`run()` 会等运行中的轮次 `set_eof_grace`（1 秒）让它们结束，然后取消。
- **一轮如何结束。** 运行完成是 `end_turn`，被取消是 `cancelled`，运行达到 `RunConfig::max_steps` 是 `max_turn_requests`，超出模型令牌预算是 `max_tokens`，provider 拒绝是 `refusal`。其他失败是 JSON-RPC 错误 `-32603`，其 `data` 为 `{code, retryable}`；消息是失败的公开消息，不是异常文本。完整的对照表在参考文档的 [ACP](reference-en.md#neographacp--agent-client-protocol) 和[协议失败映射](reference-en.md#protocol-failure-mapping)中。
- **会话。** `session/new` 创建会话，`session/resume` 继续智能体已知的会话。引擎配置了检查点存储时，停在权限请求上的会话可以在智能体重启后用 `session/resume` 恢复；这时下一条提示词的文本就是对待答问题的回答。在 `initialize` 之前调用是 `-32600`，未知的会话是 `-32002`。
- **MCP 服务器。** 客户端可以在 `session/new` 中传入 stdio MCP 服务器。智能体用 NeoGraph 的 MCP 客户端，在会话的 `cwd` 中以最小环境（`PATH` 和客户端的 `env` 条目）启动它们，所以智能体环境中的 provider 密钥不会传给它们。节点通过 `ACPClient::session_tools(session_id)` 使用这些工具。HTTP 和 SSE 的 MCP 服务器会被以 `-32602` 拒绝。见 [MCP 指南](mcp.md)。
- **能力。** `ACPServer::capabilities()` 就是 `initialize` 所公布的内容。提示词接受文本和 `resource_link` 块；图像、音频和嵌入资源只有在 `capabilities().prompt` 中开启后才接受。

## 测试与互操作

`tests/fixtures/acp_probe_agent.cpp` 是用公开 API 搭建的小型 ACP 智能体。互操作测试套件让官方 SDK 客户端连接它。单元测试是 `tests/test_acp_server.cpp` 和 `tests/test_acp_types.cpp`。
