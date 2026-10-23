<!-- neograph-i18n: source=docs/a2a.md locale=zh-CN source_sha256=e5eb8cd811611adb812540ab84e709c596588f66572bc85b5769c4cbf3f3006c -->
# A2A：智能体调用智能体

**Languages:** [English](a2a.md) | [한국어](a2a.ko.md) | [日本語](a2a.ja.md) | [简体中文](a2a.zh-CN.md)

Agent2Agent（A2A）协议让一个智能体发现另一个智能体，并把工作交给它。服务器在 `/.well-known/agent-card.json` 发布智能体卡片，说明自己是谁、能做什么。客户端通过 HTTP 上的 JSON-RPC 发送消息，服务器把这项工作作为任务来跟踪。任务会经历 working、input-required、completed、canceled、failed 等状态。客户端还可以把任务当作 Server-Sent Events 流来跟踪。

NeoGraph 两端都支持。`A2AServer` 把编译好的图发布为智能体。`A2AClient` 调用远程智能体，`A2ACallerNode` 则把远程智能体变成图中的一个节点。

## 组件

| 角色 | C++ 目标 | CMake 选项（默认值） | Python |
|---|---|---|---|
| 服务器和客户端 | `neograph::a2a` | `NEOGRAPH_BUILD_A2A`（开） | `neograph_engine.a2a` 中的客户端 |

Python 包里只有客户端。要在 Python 中提供智能体，请使用官方 A2A Python SDK（`pip install "neograph-engine[a2a]"` 会装上它），并在它背后用 `neograph_engine.ProtocolHostAdapter` 运行引擎。该适配器把 A2A 上下文 ID 对应到 NeoGraph 的 `thread_id`，从检查点恢复，转发取消，流式输出令牌，并把图中断变成 `input-required` 任务（见[在 Python 中托管时的中断](#interrupts-when-you-host-from-python)）。[Python 示例](../bindings/python/examples/README.md)里有这样的服务器。

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
- 卡片通过 `GET /.well-known/agent-card.json` 提供，JSON-RPC 端点是 `POST /`。没有设置 `AgentCard::url` 时，卡片公布的是客户端所访问的地址，使用 `http` 协议。位于终止 TLS 的反向代理之后时，请把 `AgentCard::url` 设为公开 URL，或者调用 `A2AServer::set_trust_forwarded_headers(true)`，让协议和主机取自代理发送的 `Forwarded`（RFC 7239）或 `X-Forwarded-Proto` / `X-Forwarded-Host` 头。客户端自己也能设置这些头，所以默认关闭；只有当你掌控的代理会覆盖这些头时才打开。绑定在回环地址上的服务器只有在 `A2AHttpConfig::allowed_hosts` 列出了代理的公开 `Host` 时才会接受它（见下一条）。没有 `url` 的卡片序列化时会省略这个成员（如果有首选传输的第一个接口，则使用它的 URL），而不是写入空字符串。
- 这个服务器不对请求做认证；只有基于 Program 的构造函数才接受认证器。请绑定到回环地址，或在前面放一个负责认证的代理。
- **Host、Origin 与请求大小。** 其他来源的网页只要把自己的主机名重新指向这台机器，就能访问绑定在回环地址上的服务器（DNS 重绑定）。因此回环绑定（`127.0.0.1`、`::1`、`localhost`）只应答 `Host` 为 `localhost`、`127.0.0.1`、`[::1]`（不限端口）或列在 `A2AHttpConfig::allowed_hosts` 中的名称的请求；其他 `Host` 返回 `403`，缺失或不是 `host[:port]` 形式的 `Host` 返回 `400`。带 `Origin` 的请求，除非该来源列在 `allowed_origins` 中，或在回环绑定上是服务器自己的来源（`http://localhost:<port>`、`http://127.0.0.1:<port>`、`http://[::1]:<port>`），否则返回 `403`（每个来源第一次被接受时会向 stderr 写一行）。要加入更多名称，请在 `start()` 之前调用 `A2AServer::set_http_config()`。`allowed_hosts` 的条目是不带端口的名称，如 `agent.example.com` 或 `[::1]`，比较时忽略大小写，`*` 接受任何 `Host`。在非回环绑定上，`allowed_hosts` 为空时不检查 `Host`，非空时只接受所列名称。检查覆盖每条路由（卡片、含事件流的 JSON-RPC 端点以及其他任何路径），并且在读取请求体之前进行。超过 `max_payload_bytes` 的请求体返回 `413`；默认值为 4 MiB，足够一条长消息或一个略小于 3 MiB 的内联文件（更大的文件请用 `uri` 部分传送）。位于转发公开 `Host` 的本地反向代理或隧道之后时，请列出该名称（或 `*`）。

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

只有当响应的 `Content-Type` 为 `text/event-stream` 时，才把它当作服务器发送事件来读，此时 JSON-RPC 消息是第一个 `data:` 事件；其余情况一律按 JSON 读取。响应里的文本即使含有 `data: `，也不会影响判断。

调用本身没有期限：`send_message_sync` 会一直等到代理答复，流会一直读到代理结束它，因为任务可能运行数分钟。连接有上限：域名解析、TCP 连接和 TLS 握手合计 30 秒（已经在进行的域名解析无法被中断，可能超出这个时间），可用 `set_connect_timeout(seconds)` 修改。`set_timeout(seconds)` 限制每个请求的整个交换（包括读完答复或流），超出时调用会抛出异常。两者设为 0 都表示取消限制。Python 的 `set_timeout` 接收秒数并截断为整数秒，因此小于 1 的值会取消限制；Python 没有 `set_connect_timeout`。调用方在 `on_event` 中返回 `false` 来结束的流会立即关闭，而不是读到结尾。客户端没有设置期限的调用，仍会随它所属的工作一起结束：`A2ACallerNode` 的调用随运行的 cancel token 或节点的 `attempt_timeout_ms` 结束，`make_harness_capability_executor` 发出的调用随 Harness 作业的 cancel token 或其 `call_timeout`（默认 30 秒，0 表示不限制）结束，`send_message_sync` 把这样的 token 作为最后一个参数。

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
- **失败。** 失败的运行是一个 `failed` 任务，状态消息是失败的公开消息，`metadata["neograph/failure"]` 为 `{code, retryable, retryable_by_class}`。引擎用完节点的尝试次数后 `retryable` 为 `false`，`retryable_by_class` 表示失败的类别是否允许重试。异常文本不会出现在网络上。如果在任何节点完成之前 Hook 就拦下了运行，结果是 `rejected`。达到 `max_steps` 的图会以错误码 `NG.StepLimit` 失败。
- **请求错误**（未知任务 `-32001`，向已终止的任务发消息 `-32004`，与任务不一致的 `contextId` `-32602`，任务仍在运行 `-32603` 且原因为 `TASK_BUSY`）列在参考文档的 [A2A 错误映射](reference-en.md#a2a-error-mapping)中，所有协议的失败类别则在[协议失败映射](reference-en.md#protocol-failure-mapping)中。这里不再重复。
- **Program。** 基于 Program 的智能体（`A2AServer` 背后的已准入 `ProgramVersion`）还接受 NeoGraph 协作信封，见 [A2A 协作扩展](reference-en.md#a2a-collaboration-extension)。等待输入的 Program 任务目前还不能通过 A2A 回答：消息会收到 `-32004`，任务继续等待。服务器重启后，会从 `CollaborationMailbox` 恢复 Program 任务。对仍在运行的已恢复任务调用 `SubscribeToTask`，会先发送 Task，再跟随持久 run 直到它结束，并以最终状态收尾；无法跟随 run 时，不会空着关闭，而是以错误事件结束。

<a id="interrupts-when-you-host-from-python"></a>
## 在 Python 中托管时的中断

`ProtocolHostAdapter` 让暂停的图（`interrupt_before`、`NodeInterrupt`、工具闸门）具有与原生服务器相同的形态：一个由后续消息恢复的 `input-required` 任务。[示例 27](../bindings/python/examples/27_a2a_server.py) 就是这个执行器。

- **提问。** 图暂停时，`adapter.stream(text, thread_id=..., request_id=..., interrupts=True)` 以一个 `interrupt` 事件结束，而不是最终事件。该事件的 `ProtocolInterrupt` 含有 `reason`、`payload`（节点传给 `NodeInterrupt` 的值）、`node` 和 `checkpoint_id`。执行器发布一个转到 `TASK_STATE_INPUT_REQUIRED` 的状态更新，其消息中原因是文本，负载是数据部分，`metadata["neograph/interrupt"]` 则带有节点、检查点和中断 ID，与原生服务器一致。
- **回答。** 发给该任务的下一条消息会在任务仍为 `input-required` 时到达 `execute()`。执行器把它的文本连同任务自身元数据所指的暂停一起交给 `adapter.resume_stream(text, thread_id=..., request_id=..., interrupt=...)`，它从运行暂停时的检查点恢复，并像其他轮次一样流式输出该轮的其余部分。任务以 `completed` 结束；如果图再次提问，则再次进入 `input-required`。这段文本就是暂停的节点从 `input.ctx.resume_value` 读到的值，原生服务器也是如此。如果节点期望别的形态（例如 dict），请给 `ProtocolHostAdapter` 传入 `resume_value=lambda answer, interrupt: ...`。
- **状态。** 适配器按线程在内存中记住暂停，引擎需要有检查点存储才能恢复它。示例根据任务自身元数据所指的暂停来回答该任务：它用 `metadata["neograph/interrupt"]` 重新构造 `ProtocolInterrupt(thread_id=..., checkpoint_id=..., interrupt_id=...)` 并作为 `interrupt=` 传入，因此重启之后同样有效。既没有记录的暂停、又没有 `interrupt=` 时，`resume_stream()` 会抛出 `LookupError`：恢复一个并未暂停的线程，返回的会是它上一轮的文本。任务被取消时，请调用 `adapter.discard_interrupt(thread_id)`。对等待中的任务执行 `CancelTask`，会像前文所述那样让它以 `canceled` 结束。适配器按 A2A 上下文区分线程，而原生服务器按任务区分：同一上下文中的新任务从先前任务留下的状态开始。适配器每个线程只记住一个问题，所以更新的暂停会取代更早暂停的记录，因此示例从任务而不是从适配器取得暂停。无人回答的暂停会作为未结束状态留在检查点存储中，引擎无法恢复紧随其后的暂停（`NodeInterrupt ancestry crosses super-step boundaries`）：对等待中的任务执行 `CancelTask` 之后，或在另一个任务等待时新任务又暂停，该上下文的下一次暂停在被回答时会失败，直到该上下文的某一轮完成为止。
- **没有 `interrupts=True` 时。** 图暂停时，`stream()`、`run()` 和 `run_payload()` 会抛出 `ProtocolInterrupted`。此前它们会返回状态里的最后一条消息，也就是用户自己的文本，并把它当作智能体的回答。
- **未覆盖的部分。** NeoGraph 协作信封和基于 Program 的任务是原生服务器的功能；SDK 只提供标准方法。

## 测试与互操作

`tests/fixtures/a2a_probe_server.cpp` 是用公开 API 搭建的小型 A2A 服务器。互操作测试套件让官方 SDK 客户端连接它，也让 NeoGraph 的客户端连接 SDK 的服务器。单元测试是 `tests/test_a2a_server.cpp`、`tests/test_a2a_client.cpp` 和 `tests/test_a2a_conformance.cpp`。
