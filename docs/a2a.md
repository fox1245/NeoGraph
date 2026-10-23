# A2A: agents that call agents

**Languages:** [English](a2a.md) | [한국어](a2a.ko.md) | [日本語](a2a.ja.md) | [简体中文](a2a.zh-CN.md)

The Agent2Agent (A2A) protocol lets one agent find another and hand it work. The server publishes an agent card at `/.well-known/agent-card.json` that says who it is and what it can do. A client sends it a message over JSON-RPC on HTTP, and the server tracks the work as a task that moves through states such as working, input-required, completed, canceled and failed. A client can follow a task as a stream of Server-Sent Events.

NeoGraph covers both sides. `A2AServer` publishes a compiled graph as an agent. `A2AClient` calls a remote agent, and `A2ACallerNode` makes a remote agent a node of a graph.

## Components

| Role | C++ target | CMake option (default) | Python |
|---|---|---|---|
| Server and client | `neograph::a2a` | `NEOGRAPH_BUILD_A2A` (on) | client in `neograph_engine.a2a` |

The Python package has the client only. To serve an agent from Python, use the official A2A Python SDK (`pip install "neograph-engine[a2a]"` adds it) and run your engine behind it with `neograph_engine.ProtocolHostAdapter`, which maps an A2A context id to a NeoGraph `thread_id`, resumes from checkpoints, forwards cancellation and streams tokens. The [Python examples](../bindings/python/examples/README.md) include such a server.

## Versions

A2A 1.0 (v1.0.1) is the primary version; 0.3 is kept as a compatibility dialect with the same semantics.

- **Server.** It answers each request in the dialect named by the `A2A-Version` header or the `A2A-Version` query parameter. A request with no version is treated as 0.3. Any version other than `1.0` and `0.3` gets error `-32009` that lists the supported versions. Both method spellings (`SendMessage` and `message/send`) work with either version. The agent card is served per version: `A2A-Version: 1.0` gets a pure 1.0 card, no header gets the 0.3 card, and both list the 1.0 and 0.3 interfaces.
- **Client.** Before its first call the client fetches the card and uses the first JSON-RPC interface whose version starts with `1.` or `0.`. Only when no card can be fetched does it probe: 1.0 first, then 0.3. `wire_dialect()` reports the result. A card cannot send the client to another origin unless you call `set_allow_cross_origin_interfaces(true)`.

The official SDKs are tested in both directions in CI (`.github/workflows/protocol-interop.yml`, suite in `bindings/python/tests/interop/a2a_suite`): `a2a-sdk` 1.2.2 for Python and `@a2a-js/sdk` 1.3.0 and 0.3.14.

## Serve a graph as an agent

This program publishes a graph with two nodes: `draft` writes release notes from the prompt, and `publish` runs only after a person has approved. It needs no API key.

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

How it works:

- The server puts the text of the user's message into the graph's `prompt` channel and reads the agent's answer from the `response` channel. A `GraphAgentAdapter` changes that: override `input_channel()`, `output_channel()`, `build_initial_state()`, `build_output_artifact()` (to return an artifact) or `build_resume_value()`, and pass it as the third constructor argument.
- The default adapter puts the final answer only in the status message of the completed task; the task has no artifact, so a client that reads artifacts finds none. To return the answer as an artifact, override `build_output_artifact()`. [Example 78](../examples/78_a2a_input_required.cpp) promotes the `response` channel to an artifact this way.
- A graph run is a task, and its id is the run's `thread_id`. Every task runs on its own worker thread, so a task outlives a dropped connection. The server accepts 32 tasks in flight; more get JSON-RPC error `-32603` with reason `RESOURCE_EXHAUSTED`.
- `interrupt_before` (or a `NodeInterrupt` thrown by a node) puts the task in `input-required`. The status message holds the reason, `metadata["neograph/interrupt"]` holds the node and checkpoint, and a follow-up message with the same task id resumes the run from that checkpoint. The text of the follow-up message is the resume value. Without a checkpoint store the graph runs again from the start with the follow-up as its input, so give the engine one if the graph can pause.
- The card is served at `GET /.well-known/agent-card.json` and the JSON-RPC endpoint is `POST /`. Without `AgentCard::url`, the card advertises the address the client used, with the `http` scheme. Behind a TLS-terminating reverse proxy, set `AgentCard::url` to the public URL, or call `A2AServer::set_trust_forwarded_headers(true)` so that the scheme and host come from the proxy's `Forwarded` (RFC 7239) or `X-Forwarded-Proto` / `X-Forwarded-Host` headers. It is off by default because a client can set those headers itself; turn it on only when a proxy you control overwrites them. A server bound to a loopback address answers the proxy's public `Host` only when `A2AHttpConfig::allowed_hosts` lists it (see the next bullets). A card serialized without a `url` leaves the member out (or takes the first interface of its preferred transport) instead of writing an empty string.
- This server does not authenticate requests; only the Program-backed constructors take an authenticator. Bind it to a loopback address, or put an authenticating proxy in front of it.
- **Host, Origin and request size.** A web page on another origin can reach a server bound to a loopback address when its host name is re-pointed at this machine (DNS rebinding). A loopback bind (`127.0.0.1`, `::1`, `localhost`) therefore answers only the `Host` names `localhost`, `127.0.0.1` and `[::1]` on any port, plus those in `A2AHttpConfig::allowed_hosts`; another `Host` gets `403`, and a missing `Host`, or one that is not `host[:port]`, gets `400`. A request that carries an `Origin` gets `403` unless the origin is listed in `allowed_origins` or, on a loopback bind, is the server's own: `http://localhost:<port>`, `http://127.0.0.1:<port>` or `http://[::1]:<port>` (a line on stderr the first time each is admitted). To list more, call `A2AServer::set_http_config()` before `start()`. `allowed_hosts` entries are host names without a port, such as `agent.example.com` or `[::1]`, compared ignoring case, and `*` admits any `Host`. On a non-loopback bind an empty `allowed_hosts` checks no `Host`, and a non-empty one admits only the listed names. The checks cover every route (the card, the JSON-RPC endpoint with its event streams, and any other path) and run before the body is read. A body over `max_payload_bytes` gets `413`; the default is 4 MiB, enough for a long message or one inline file of a little under 3 MiB (a larger file travels as a `uri` part). Behind a local reverse proxy or a tunnel that forwards the public `Host`, list that name (or `*`).

Link it with `neograph::core` and `neograph::a2a`:

```cmake
add_executable(release_notes_agent release_notes_agent.cpp)
target_link_libraries(release_notes_agent PRIVATE neograph::core neograph::a2a)
```

## Call an agent

Start the program above, then run this client against it. It discovers the agent, sends a message, sees that the task waits for input, and answers.

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

The same in Python:

```python
from neograph_engine import a2a

agent = a2a.A2AClient("http://127.0.0.1:8087")
print(agent.fetch_agent_card().name)

task = agent.send_message("version 2.0")  # returns when the task finishes or needs input
print(task.state)
task = agent.send_message("approved", task.id, task.context_id)  # answer on the same task
print(task.state)
```

The C++ client prints:

```text
agent: release-notes
first: input-required - Interrupt before node: publish
second: completed - Published: Release notes: version 2.0
```

The Python script prints:

```text
release-notes
TaskState.InputRequired
TaskState.Completed
```

Other calls on the client: `send_message_stream(text, on_event)` delivers status and artifact updates as they arrive and returns the assembled task; `get_task(id)` reads a task; `cancel_task(id)` cancels one, including a task that waits for input. A message to a task that is already terminal fails with `-32004`. The Python client is in the 0.14.0 wheel, and the Python script above works against this server with it.

A reply is read as JSON unless its `Content-Type` is `text/event-stream`, in which case the JSON-RPC message is its first `data:` event. The text a reply holds never decides, `data: ` included.

A call has no deadline of its own: `send_message_sync` waits until the agent answers and a stream until the agent ends it, since a task may run for minutes. Connecting is bounded: name resolution, the TCP connect and the TLS handshake have 30 s together, and `set_connect_timeout(seconds)` changes that. `set_timeout(seconds)` bounds the whole exchange of each request, the reply or the stream read to its end included, and the call throws when it runs over. For both, 0 removes the bound. The Python `set_timeout` takes seconds and truncates them to whole seconds, so a value below 1 removes the bound; Python has no `set_connect_timeout`. A stream that its caller ends by returning `false` from `on_event` is closed at once instead of being read to its end.

## Use a remote agent as a node

`A2ACallerNode` reads a channel, sends its text to the remote agent and writes the reply into another channel. Several nodes can share one client; their calls run concurrently.

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

With the default keys it writes `response`, `response_context_id` (to continue the conversation) and `response_task_id` (only while the task waits for input, so the next call resumes it). The program above prints the agent's status text and that task id.

## Interrupts, cancellation and errors

- **Interrupts.** A paused graph is an `input-required` task, as above. While the task waits no run is active; its state is in the engine's checkpoint store.
- **Cancellation.** `CancelTask` cancels the run's token and the task ends `canceled`. A cancel of a terminal task is `-32002`.
- **Failures.** A failed run is a `failed` task whose status message is the failure's public message and whose `metadata["neograph/failure"]` is `{code, retryable, retryable_by_class}`. `retryable` is `false` once the engine has used up the node's attempts, and `retryable_by_class` says whether the failure's class allows a retry at all. Exception text never crosses the wire. A hook that blocks before any node finished gives `rejected`. A graph that reaches `max_steps` fails with code `NG.StepLimit`.
- **Request errors** (unknown task `-32001`, message to a terminal task `-32004`, a `contextId` that differs from the task's `-32602`, a task still running `-32603` with reason `TASK_BUSY`) are listed in the reference under [A2A error mapping](reference-en.md#a2a-error-mapping), and every protocol's failure classes are in the [protocol failure mapping](reference-en.md#protocol-failure-mapping). They are not repeated here.
- **Programs.** A Program-backed agent (an admitted `ProgramVersion` behind `A2AServer`) also accepts NeoGraph collaboration envelopes; see [A2A collaboration extension](reference-en.md#a2a-collaboration-extension). A Program task that waits for input cannot yet be answered over A2A: the message gets `-32004` and the task keeps waiting. After a restart the server recovers its Program tasks from the `CollaborationMailbox`. `SubscribeToTask` on a recovered task that is still running streams the Task, follows the durable run until it settles and ends with the final status; if the run cannot be followed, it ends with an error event instead of closing empty.

## Tests and interoperability

`tests/fixtures/a2a_probe_server.cpp` is a small A2A server built on the public API; the interop suites run the official SDK clients against it and NeoGraph's client against SDK servers. The unit tests are `tests/test_a2a_server.cpp`, `tests/test_a2a_client.cpp` and `tests/test_a2a_conformance.cpp`.
