<!-- neograph-i18n: source=docs/a2a.md locale=zh-CN source_sha256=ae2e03b8b87d0da548b07723e235ce213559d2ccc8a4060ef5b28e72d67a5799 -->
# A2A：智能体调用智能体

**Languages:** [English](a2a.md) | [한국어](a2a.ko.md) | [日本語](a2a.ja.md) | [简体中文](a2a.zh-CN.md)

Agent2Agent（A2A）协议让一个智能体发现另一个智能体，并把工作交给它。服务器在 `/.well-known/agent-card.json` 发布智能体卡片，说明自己是谁、能做什么。客户端通过 HTTP 上的 JSON-RPC 发送消息，服务器把这项工作作为任务来跟踪。任务会经历 working、input-required、completed、canceled、failed 等状态。客户端还可以把任务当作 Server-Sent Events 流来跟踪。

NeoGraph 两端都支持。`A2AServer` 把编译好的图发布为智能体。`A2AClient` 调用远程智能体，`A2ACallerNode` 则把远程智能体变成图中的一个节点。

## 组件

| 角色 | C++ 目标 | CMake 选项（默认值） | Python |
|---|---|---|---|
| 服务器和客户端 | `neograph::a2a` | `NEOGRAPH_BUILD_A2A`（开） | `neograph_engine.a2a` 中的客户端 |

Python 包里只有客户端。要在 Python 中提供智能体，请使用官方 A2A Python SDK（`pip install "neograph-engine[a2a]"` 会装上它），并在它背后用 `neograph_engine.ProtocolHostAdapter` 运行引擎。该适配器把 A2A 上下文 ID 对应到 NeoGraph 的 `thread_id`，从检查点恢复，转发取消，并流式输出令牌。[Python 示例](../bindings/python/examples/README.md)里有这样的服务器。

## 版本

A2A 1.0（v1.0.1）是主要版本，0.3 作为语义相同的兼容方言保留。

- **服务器。** 每个请求按 `A2A-Version` 头或 `A2A-Version` 查询参数指定的方言应答。没有版本的请求按 0.3 处理。`1.0` 和 `0.3` 之外的版本会收到错误 `-32009`，其中列出受支持的版本。两种方法写法（`SendMessage` 和 `message/send`）在任一版本下都可用。智能体卡片按版本提供：`A2A-Version: 1.0` 得到纯 1.0 卡片，没有该头则得到 0.3 卡片，两种卡片都会列出 1.0 和 0.3 的接口。
- **客户端。** 首次调用之前，客户端先获取卡片，并使用第一个版本以 `1.` 或 `0.` 开头的 JSON-RPC 接口。只有在拿不到卡片时才会探测：先试 1.0，再试 0.3。`wire_dialect()` 会报告结果。卡片不能把客户端引到其他源，除非调用 `set_allow_cross_origin_interfaces(true)`。

官方 SDK 在 CI 中做双向测试（`.github/workflows/protocol-interop.yml`，测试套件在 `bindings/python/tests/interop/a2a_suite`）：Python 的 `a2a-sdk` 1.2.2，以及 `@a2a-js/sdk` 的 1.3.0 和 0.3.14。

## 把图发布为智能体

这个程序发布一张有两个节点的图：`draft` 根据提示词写发布说明，`publish` 只在有人批准之后才运行。不需要 API 密钥。

```cpp
#include <neograph/a2a/server.h>
#include <neograph/neograph.h>

#include <iostream>
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

    // The A2A server reads the user's text from the "prompt" channel and the
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

    // A checkpoint store is what lets the task wait for an answer and resume.
    EngineConfig config;
    config.checkpoint_store = std::make_shared<InMemoryCheckpointStore>();
    std::shared_ptr<GraphEngine> engine = GraphEngine::build_strict(definition, std::move(config));

    a2a::AgentCard card;
    card.name = "release-notes";
    card.description = "Drafts release notes and publishes them after approval.";
    card.version = "1.0.0";
    card.default_input_modes = {"text/plain"};
    card.default_output_modes = {"text/plain"};
    card.skill_names = {"release-notes"};

    a2a::A2AServer server(engine, card);
    std::cout << "A2A agent on http://127.0.0.1:8087\n" << std::flush;
    server.start("127.0.0.1", 8087);  // blocks until server.stop()
    return 0;
}
```

工作方式如下：

- 服务器把用户消息的文本放进图的 `prompt` 通道，并从 `response` 通道读取智能体的回答。要改变这一点，请在 `GraphAgentAdapter` 中重写 `input_channel()`、`output_channel()`、`build_initial_state()`、`build_output_artifact()`（用于返回工件）或 `build_resume_value()`，并把适配器作为构造函数的第三个参数传入。
- 默认适配器只把最终回答放在已完成任务的状态消息里。任务没有工件，因此读取工件的客户端什么也拿不到。要把回答作为工件返回，请重写 `build_output_artifact()`。[示例 78](../examples/78_a2a_input_required.cpp) 就用这种方式把 `response` 通道提升为工件。
- 一次图运行就是一个任务，任务 ID 就是这次运行的 `thread_id`。每个任务在自己的工作线程上运行，所以连接断开后任务仍会继续。服务器同时最多接收 32 个任务，更多的会收到 JSON-RPC 错误 `-32603`，原因为 `RESOURCE_EXHAUSTED`。
- `interrupt_before`（或节点抛出的 `NodeInterrupt`）会让任务进入 `input-required`。状态消息中是原因，`metadata["neograph/interrupt"]` 中是节点和检查点；用同一个任务 ID 发送后续消息，就会从该检查点恢复运行。后续消息的文本就是恢复值。如果没有检查点存储，图会以后续消息为输入从头再运行一遍，所以图可能暂停时，请给引擎配置一个存储。
- 卡片通过 `GET /.well-known/agent-card.json` 提供，JSON-RPC 端点是 `POST /`。没有设置 `AgentCard::url` 时，卡片公布的是客户端所访问的地址。
- 这个服务器不对请求做认证；只有基于 Program 的构造函数才接受认证器。请绑定到回环地址，或在前面放一个负责认证的代理。

链接 `neograph::core` 和 `neograph::a2a`：

```cmake
add_executable(release_notes_agent release_notes_agent.cpp)
target_link_libraries(release_notes_agent PRIVATE neograph::core neograph::a2a)
```

## 调用智能体

先启动上面的程序，再用这个客户端去连接。它发现智能体、发送消息、确认任务在等待输入，然后作答。

```cpp
#include <neograph/a2a/client.h>
#include <neograph/neograph.h>

#include <iostream>
#include <string>

using namespace neograph;

namespace {

void print(const char* label, const a2a::Task& task) {
    std::cout << label << ": " << a2a::task_state_to_string(task.status.state);
    if (task.status.message && !task.status.message->parts.empty())
        std::cout << " - " << task.status.message->parts[0].text;
    std::cout << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    a2a::A2AClient client(argc > 1 ? argv[1] : "http://127.0.0.1:8087");

    const auto card = client.fetch_agent_card();  // GET /.well-known/agent-card.json
    std::cout << "agent: " << card.name << '\n';

    // The graph stops before "publish", so the task waits for input.
    auto task = client.send_message_sync("version 2.0");
    print("first", task);

    // Answer on the same task. The server resumes the run from its checkpoint.
    task = client.send_message_sync("approved", task.id, task.context_id);
    print("second", task);
    return 0;
}
```

Python 中的写法相同：

```python
from neograph_engine import a2a

agent = a2a.A2AClient("http://127.0.0.1:8087")
print(agent.fetch_agent_card().name)

task = agent.send_message("version 2.0")  # returns when the task finishes or needs input
print(task.state)
task = agent.send_message("approved", task.id, task.context_id)  # answer on the same task
print(task.state)
```

C++ 客户端输出：

```text
agent: release-notes
first: input-required - Interrupt before node: publish
second: completed - Published: Release notes: version 2.0
```

Python 脚本输出：

```text
release-notes
TaskState.InputRequired
TaskState.Completed
```

客户端的其他调用：`send_message_stream(text, on_event)` 会在状态和工件更新到达时逐个交给回调，并返回组装好的任务；`get_task(id)` 读取任务；`cancel_task(id)` 取消任务，包括正在等待输入的任务。向已经终止的任务发送消息会以 `-32004` 失败。这个 Python 客户端在 0.14.0 的 wheel 中也有，上面的 Python 脚本用它同样能配合这个服务器工作。

## 把远程智能体用作节点

`A2ACallerNode` 读取一个通道，把其中的文本发给远程智能体，并把回复写入另一个通道。多个节点可以共用一个客户端，它们的调用会并发执行。

```cpp
#include <neograph/a2a/a2a_caller_node.h>
#include <neograph/a2a/client.h>
#include <neograph/neograph.h>

#include <iostream>
#include <memory>

using namespace neograph;
using namespace neograph::graph;

int main(int argc, char** argv) {
    auto client = std::make_shared<a2a::A2AClient>(argc > 1 ? argv[1] : "http://127.0.0.1:8087");

    // The node reads "prompt", sends it to the remote agent and writes the
    // agent's text to "response". It also writes "response_task_id" while the
    // task waits for input, and "response_context_id".
    NodeFactory::instance().register_type(
        "docs.remote_agent", [client](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<a2a::A2ACallerNode>(name, client, "prompt", "response");
        });

    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "ask-remote-agent"},
        {"channels", {{"prompt", {{"reducer", "overwrite"}, {"initial", ""}}},
                      {"response", {{"reducer", "overwrite"}}},
                      {"response_task_id", {{"reducer", "overwrite"}}},
                      {"response_context_id", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"ask", {{"type", "docs.remote_agent"}}}}},
        {"edges", {{{"from", "__start__"}, {"to", "ask"}},
                   {{"from", "ask"}, {"to", "__end__"}}}},
    };

    auto engine = GraphEngine::build_strict(definition, EngineConfig{});
    RunConfig run;
    run.input = {{"prompt", "version 2.0"}};
    const auto result = engine->run(run);
    std::cout << "response: " << result.channel<std::string>("response") << '\n'
              << "waiting task: " << result.channel<std::string>("response_task_id") << '\n';
    return 0;
}
```

使用默认键时，它写入 `response`、`response_context_id`（用于延续对话）和 `response_task_id`（只在任务等待输入时才有值，这样下一次调用会恢复该任务）。上面的程序会打印智能体的状态文本和这个任务 ID。

## 中断、取消和错误

- **中断。** 暂停的图就是前面所说的 `input-required` 任务。任务等待期间没有活动的运行，它的状态在引擎的检查点存储中。
- **取消。** `CancelTask` 取消这次运行的令牌，任务以 `canceled` 结束。取消已终止的任务会得到 `-32002`。
- **失败。** 失败的运行是一个 `failed` 任务，状态消息是失败的公开消息，`metadata["neograph/failure"]` 为 `{code, retryable}`。异常文本不会出现在网络上。如果在任何节点完成之前 Hook 就拦下了运行，结果是 `rejected`。达到 `max_steps` 的图会以错误码 `NG.StepLimit` 失败。
- **请求错误**（未知任务 `-32001`，向已终止的任务发消息 `-32004`，与任务不一致的 `contextId` `-32602`，任务仍在运行 `-32603` 且原因为 `TASK_BUSY`）列在参考文档的 [A2A 错误映射](reference-en.md#a2a-error-mapping)中，所有协议的失败类别则在[协议失败映射](reference-en.md#protocol-failure-mapping)中。这里不再重复。
- **Program。** 基于 Program 的智能体（`A2AServer` 背后的已准入 `ProgramVersion`）还接受 NeoGraph 协作信封，见 [A2A 协作扩展](reference-en.md#a2a-collaboration-extension)。等待输入的 Program 任务目前还不能通过 A2A 回答：消息会收到 `-32004`，任务继续等待。服务器重启后，会从 `CollaborationMailbox` 恢复 Program 任务。对仍在运行的已恢复任务调用 `SubscribeToTask`，会先发送 Task，再跟随持久 run 直到它结束，并以最终状态收尾；无法跟随 run 时，不会空着关闭，而是以错误事件结束。

## 测试与互操作

`tests/fixtures/a2a_probe_server.cpp` 是用公开 API 搭建的小型 A2A 服务器。互操作测试套件让官方 SDK 客户端连接它，也让 NeoGraph 的客户端连接 SDK 的服务器。单元测试是 `tests/test_a2a_server.cpp`、`tests/test_a2a_client.cpp` 和 `tests/test_a2a_conformance.cpp`。
