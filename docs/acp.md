# ACP: a graph as an editor agent

**Languages:** [English](acp.md) | [한국어](acp.ko.md) | [日本語](acp.ja.md) | [简体中文](acp.zh-CN.md)

The Agent Client Protocol (ACP) connects an editor to a coding or chat agent. The editor (Zed, Neovim with CodeCompanion, Gemini CLI and others) starts the agent as a child process and exchanges JSON-RPC 2.0 messages with it, one JSON object per line, on the agent's stdin and stdout. The editor opens a session, sends prompts, and the agent streams its answer back as updates. The agent can also ask the editor for permission, or for access to files and a terminal.

`ACPServer` runs a compiled NeoGraph graph as such an agent. NeoGraph implements the agent side; the editor is the client. `ACPClient` is the handle through which the agent sends its own requests back to the editor.

## Components

| Role | C++ target | CMake option (default) | Python |
|---|---|---|---|
| Agent (server) | `neograph::acp` | `NEOGRAPH_BUILD_ACP` (on) | through the official SDK |

The Python package does not contain a native ACP server. To write the agent in Python, install the official SDK (`pip install "neograph-engine[acp]"`) and run your engine behind it with `neograph_engine.ProtocolHostAdapter`, which maps an ACP session id to a NeoGraph `thread_id`, resumes from checkpoints, forwards cancellation and streams tokens. The [Python examples](../bindings/python/examples/README.md) include such an agent. ACP needs Python 3.10 or newer.

## Version

ACP protocol version 1. The messages are checked against the official ACP schema v1.25.0, and the server is tested against the official Python SDK (`agent-client-protocol` 0.12.1, and 1.0.0rc2 as a pre-release) and TypeScript SDK (`@agentclientprotocol/sdk` 1.8.0) in CI (`.github/workflows/protocol-interop.yml`, suite in `bindings/python/tests/interop/acp_suite`).

## Serve a graph as an agent

This program is the release-notes graph from the [A2A guide](a2a.md): `draft` writes release notes from the prompt, and `publish` runs only after a person has approved. It needs no API key.

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

How it works:

- `session/prompt` puts the text of the prompt into the graph's `prompt` channel. The text of the `response` channel goes back as one `agent_message_chunk` update, and the turn ends with `stopReason: "end_turn"`. An `ACPGraphAdapter` changes that: override `input_channel()`, `output_channel()`, `build_session_state()` (to seed channels such as the working directory) or the interrupt hooks below, and pass it as the third constructor argument.
- A graph interrupt becomes a permission request in the same turn. `interrupt_before` or a `NodeInterrupt` makes the server send `session/request_permission` with the options `approve` (`allow_once`) and `reject` (`reject_once`). The id of the option the editor selects is the resume value, so `publish` above sees `"approve"` or `"reject"`. Override `interrupt_permission()` and `interrupt_resume_value()` to change the options or the value. If the editor answers `cancelled`, the turn ends `cancelled`.
- `run()` reads stdin until it closes. Anything else the program writes to stdout corrupts the protocol; log to stderr.
- The server handles at most 32 prompts at once and one per session (`set_max_inflight_prompts`). A refused prompt is JSON-RPC error `-32603` with `data.reason` `"overloaded"` or `"session_busy"`.

Link it with `neograph::core` and `neograph::acp`:

```cmake
add_executable(release_notes_agent release_notes_agent.cpp)
target_link_libraries(release_notes_agent PRIVATE neograph::core neograph::acp)
```

An editor starts the executable and speaks ACP on its stdin and stdout; how you register it depends on the editor.

## Try it without an editor

This script plays the editor's part. It starts the agent, opens a session, sends a prompt, approves the permission request and prints what happens.

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

Run it with the path of the agent: `python acp_client.py ./release_notes_agent`. It prints:

```text
agent: release-notes 1.0.0
agent asks: Interrupt before node: publish ['approve', 'reject']
update: Published: Release notes: version 2.0
stop reason: end_turn
```

Change `"approve"` to `"reject"` and the update reads `Not published (reject): Release notes: version 2.0`.

## Cancellation, sessions and errors

- **Cancellation.** `session/cancel` cancels the turn that is running and the turn ends with `stopReason: "cancelled"`; a cancel with no turn running is ignored. When stdin reaches end of file, `run()` gives running turns `set_eof_grace` (one second) to finish and then cancels them.
- **How a turn ends.** `end_turn` for a completed run, `cancelled`, `max_turn_requests` when the run reaches `RunConfig::max_steps`, `max_tokens` when the model-token budget is crossed, and `refusal` for a provider refusal. Any other failure is JSON-RPC error `-32603` whose `data` is `{code, retryable}`; the message is the failure's public message and never the exception text. The complete table is in the reference under [ACP](reference-en.md#neographacp--agent-client-protocol) and in the [protocol failure mapping](reference-en.md#protocol-failure-mapping).
- **Sessions.** `session/new` creates a session, and `session/resume` continues one the agent knows. With a checkpoint store on the engine, a session paused on a permission request can be restored by `session/resume` after the agent restarts; the next prompt's text then answers the pending question. A call before `initialize` is `-32600`, and an unknown session is `-32002`.
- **MCP servers.** A client may pass stdio MCP servers in `session/new`. The agent starts them with NeoGraph's MCP client in the session's `cwd` with a minimal environment (`PATH` and the client's `env` entries), so provider keys in the agent's environment do not reach them. Nodes reach their tools through `ACPClient::session_tools(session_id)`. HTTP and SSE MCP servers are refused with `-32602`. See the [MCP guide](mcp.md).
- **Capabilities.** `ACPServer::capabilities()` is what `initialize` advertises. Prompts accept text and `resource_link` blocks; image, audio and embedded resources only when you turn them on in `capabilities().prompt`.

## Tests and interoperability

`tests/fixtures/acp_probe_agent.cpp` is a small ACP agent built on the public API; the interop suites run the official SDK clients against it. The unit tests are `tests/test_acp_server.cpp` and `tests/test_acp_types.cpp`.
