# ACP: a graph as an editor agent

**Languages:** [English](acp.md) | [한국어](acp.ko.md) | [日本語](acp.ja.md) | [简体中文](acp.zh-CN.md)

The Agent Client Protocol (ACP) connects an editor to a coding or chat agent. The editor (Zed, Neovim with CodeCompanion, Gemini CLI and others) starts the agent as a child process and exchanges JSON-RPC 2.0 messages with it, one JSON object per line, on the agent's stdin and stdout. The editor opens a session, sends prompts, and the agent streams its answer back as updates. The agent can also ask the editor for permission, or for access to files and a terminal.

`ACPServer` runs a compiled NeoGraph graph as such an agent. NeoGraph implements the agent side; the editor is the client. `ACPClient` is the handle through which the agent sends its own requests back to the editor. An error the editor answers is thrown as `acp::ACPClientError`. When its code says the request cannot succeed, such as `-32002` for a file that does not exist, it is an `acp::ACPClientRefusal`, and the node is not retried.

## Components

| Role | C++ target | CMake option (default) | Python |
|---|---|---|---|
| Agent (server) | `neograph::acp` | `NEOGRAPH_BUILD_ACP` (on) | through the official SDK |
| Agent over Streamable HTTP and WebSocket (experimental) | `neograph::acp_http_server` | `NEOGRAPH_BUILD_ACP_HTTP_SERVER` (**off**) | none |

The Python package does not contain a native ACP server. To write the agent in Python, install the official SDK (`pip install "neograph-engine[acp]"`) and run your engine behind it with `neograph_engine.ProtocolHostAdapter`, which maps an ACP session id to a NeoGraph `thread_id`, resumes from checkpoints, forwards cancellation, streams tokens and turns a graph interrupt into a permission request (see [Permission requests when you host from Python](#permission-requests-when-you-host-from-python)). The [Python examples](../bindings/python/examples/README.md) include such an agent. ACP needs Python 3.10 or newer.

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

- **Cancellation.** `session/cancel` cancels the turn that is running and the turn ends with `stopReason: "cancelled"`; a cancel with no turn running is ignored. A `$/cancel_request` that names a running `session/prompt` by its request id cancels that turn the same way; one that names any other request is ignored. When stdin reaches end of file, `run()` gives running turns `set_eof_grace` (one second) to finish and then cancels them.
- **How a turn ends.** `end_turn` for a completed run, `cancelled`, `max_turn_requests` when the run reaches `RunConfig::max_steps`, `max_tokens` when the model-token budget is crossed, and `refusal` for a provider refusal. Any other failure is JSON-RPC error `-32603` whose `data` is `{code, retryable, retryable_by_class}`: `retryable` is `false` once the engine has used up the node's attempts, and `retryable_by_class` says whether the failure's class allows a retry at all; the message is the failure's public message and never the exception text. The complete table is in the reference under [ACP](reference-en.md#neographacp--agent-client-protocol) and in the [protocol failure mapping](reference-en.md#protocol-failure-mapping).
- **Sessions.** `session/new` creates a session, and `session/resume` continues one the agent knows. With a checkpoint store on the engine, a session paused on a permission request can be restored by `session/resume` after the agent restarts; the next prompt's text then answers the pending question. A call before `initialize` is `-32600`, and an unknown session is `-32002`.
- **MCP servers.** A client may pass stdio MCP servers in `session/new`. The agent starts them with NeoGraph's MCP client in the session's `cwd` with a minimal environment (`PATH` and the client's `env` entries), so provider keys in the agent's environment do not reach them. Nodes reach their tools through `ACPClient::session_tools(session_id)`. HTTP and SSE MCP servers are refused with `-32602`. See the [MCP guide](mcp.md).
- **Capabilities.** `ACPServer::capabilities()` is what `initialize` advertises. Prompts accept text and `resource_link` blocks; image, audio and embedded resources only when you turn them on in `capabilities().prompt`.

## Remote transports (experimental)

stdio is the transport that ACP defines and that every editor implements. A remote transport, Streamable HTTP with a WebSocket upgrade on the same endpoint, exists only as a draft proposal of the ACP project. `neograph::acp_http_server` serves an agent over both and follows that draft where it is explicit, so its wire details may change when the draft does. It is **experimental and off by default**: a separate library (`NEOGRAPH_BUILD_ACP_HTTP_SERVER`, default off) that nothing links unless asked, and a server that listens only after `start()`. `ACPServer::run()` on stdio does not change.

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

Link `neograph::acp_http_server`, which brings `neograph::acp`. The factory is called once per connection, with the principal that the optional `authenticator` returned (empty without one).

- **Connections.** One ACP connection is one `ACPServer`, driven exactly as on stdio: messages are handled in the order they arrive, and a connection that ends is an end of input, so its running prompts are cancelled. Connections are independent and may run prompts at the same time on the same engine; `max_connections` (16) limits them. A session id belongs to one live connection: `session/resume` of an id that another live connection holds is refused with `-32603` and `data.reason` `"session_busy"`. A connection's sessions end with it; after a reconnect, `session/resume` restores a session from the engine's checkpoint store. While a prompt is running on the engine (on any connection), `session/resume` of a session that this connection did not create waits up to two seconds, during which the connection reads no other message, and then answers `-32603` with `data.reason` `"engine_busy"`; retry it. Nodes that call the editor capture `ACPHttpServer::client()` (or pass an unbound `ACPClient` to `attach_client()`), and each call reaches the connection that owns the session.
- **Streamable HTTP** on `ACPHttpConfig::endpoint` (`/acp`). A `POST` of `initialize` without `Acp-Connection-Id` creates the connection and is answered `200` with the response as the body and the id in the `Acp-Connection-Id` header. Every other `POST` carries that header, is answered `202` with no body, and its answer comes on a stream. A `GET` with `Accept: text/event-stream` opens a server-sent-event stream: the connection stream with the header alone (the answers to `session/new` and `session/resume`), the session stream with `Acp-Session-Id` too (`session/update`, the agent's requests such as `session/request_permission` and `fs/*`, and the answers to that session's requests). A session that `session/resume` brought back has no stream of its own until the client opens one, and its messages use the connection stream meanwhile. A `DELETE` ends the connection. A request that names a session in `params.sessionId` must send the same `Acp-Session-Id`, and the session must be one of the connection (`session/resume` is the exception: it introduces the session). A batch is `501`. A message with no stream open waits in a bounded queue; nothing is replayed after a connection is lost, and a connection with no stream and no request for `connection_idle_timeout` (60 s) is closed, cancelling its prompts.
- **WebSocket** on the same endpoint (`GET` with `Upgrade: websocket`). Each text frame is one JSON-RPC message in either direction, `initialize` first; binary frames are ignored. Frames follow RFC 6455: client frames must be masked, pings are answered, messages may be fragmented. A message over `max_payload_bytes` closes the connection with code 1009, and closing the socket ends the connection.
- **Where it differs from the draft.** The draft requires HTTP/2; this server speaks HTTP/1.1, so each stream and each `POST` is its own TCP connection, and multiplexing is for a reverse proxy to provide. The 101 response of a WebSocket upgrade does not carry `Acp-Connection-Id`. No cookies are set, `Acp-Protocol-Version` is not checked, and streams have no event ids or resumption. The `initialize` answer carries the id in the header only, not as `connectionId` in the body. `ACPServer` has no `session/load`, so the draft's reconnect flow is `session/resume`, and a session stream opens only after `session/new` or `session/resume` has answered. The `Acp-Session-Id` header is optional on a `POST` that is the client's answer to a request of the agent. A second stream on the same scope replaces the first.
- **Security.** The server binds `127.0.0.1` unless you name another host, and a non-loopback bind is refused unless `authenticator` is set. Every request, stream and upgrade, and any path the server does not serve, is checked before its body is read: Host and Origin (`allowed_hosts` and `allowed_origins`, with the rules of the [MCP](mcp.md) and [A2A](a2a.md) servers), then `authenticator` (HTTP 401 with `WWW-Authenticate: Bearer`), then `max_payload_bytes` (HTTP 413, 4 MiB by default). A connection answers only to the principal that created it. A client cannot make the server start processes: a `session/new` or `session/resume` that lists `mcpServers` is refused with `-32602` unless `allow_client_mcp_servers` is set (ACP expects an agent to start the stdio MCP servers that a client lists; here they would run on the server's host, not the client's), and the `cwd` a client names reaches the graph as `_acp_cwd`, which a node that uses it on the server has to check. The messages that wait for the agent are bounded (two messages of `max_payload_bytes`, at least 1 MiB): a `POST` beyond that is answered `503` with `Retry-After`, a WebSocket message beyond it with a JSON-RPC error. Slow and idle TCP connections, and the share of `max_connections` and of the stream slots that one principal may take, are not limited per peer; put a server that faces the internet behind a reverse proxy that limits them. The agent reads and writes files in the editor's name, so treat a remote bind as exposing that. A browser cannot set `Authorization` on a WebSocket; authenticate those in a reverse proxy.
## Permission requests when you host from Python

`ProtocolHostAdapter` gives a graph that pauses (`interrupt_before`, a `NodeInterrupt`, a tool gate) the same shape as the native server: a `session/request_permission` in the same turn. [Example 28](../bindings/python/examples/28_acp_agent.py) is the agent.

- **The question.** `adapter.stream(payload, thread_id=..., request_id=..., interrupts=True)` ends with an `interrupt` event instead of the final one when the graph pauses. Its `ProtocolInterrupt` has `reason`, `payload` (the value the node passed to `NodeInterrupt`), `node` and `checkpoint_id`. `prompt()` sends `request_permission` on the connection with a `ToolCallUpdate` whose title is the reason and whose `rawInput` is the payload, and the options `approve` (`allow_once`) and `reject` (`reject_once`), as the native server does.
- **The answer.** When the client selects an option, `prompt()` calls `adapter.resume_stream(option_id, thread_id=..., request_id=..., interrupt=interrupt)`. That resumes the checkpoint the run paused at and streams the rest of the turn as `agent_message_chunk` updates; the option id is the value the paused node reads from `input.ctx.resume_value`, so a node sees `"approve"` or `"reject"`. If the graph asks another question, the loop asks again. Pass `resume_value=lambda answer, interrupt: ...` to `ProtocolHostAdapter` when the node expects another shape.
- **Cancellation.** A client that answers `cancelled` ends the turn with `stopReason: "cancelled"`, and so does `session/cancel` while the request waits or while the resumed turn runs. Either way the agent calls `adapter.discard_interrupt(thread_id)`, so the question does not linger. The next prompt of the session starts a new turn; it is not taken for the answer. The abandoned pause stays open in the checkpoint store, and the engine cannot resume a pause that follows it directly (`NodeInterrupt ancestry crosses super-step boundaries`): if the next prompt asks for permission again, answering it fails until a turn of the session has completed.
- **Without `interrupts=True`.** `stream()`, `run()` and `run_payload()` raise `ProtocolInterrupted` when the graph pauses. Before, they returned the last message of the state, which is the user's own text, as if it were the agent's answer.
- **Checkpoint store.** The engine needs one to resume the paused run; the example uses an in-memory store unless it is given a durable one.
- **Restarts.** The question lives in the turn. If the agent restarts while it waits, the turn ends with the connection and the next prompt starts a new turn; the native server can restore such a session with `session/resume`, the adapter cannot.
- **Versions.** The example is tested with `agent-client-protocol` 0.12.1.

## Tests and interoperability

`tests/fixtures/acp_probe_agent.cpp` is a small ACP agent built on the public API; the interop suites run the official SDK clients against it. The unit tests are `tests/test_acp_server.cpp` and `tests/test_acp_types.cpp`; the remote transports have their own, `tests/test_acp_http_server.cpp`. `bindings/python/tests/interop/acp_suite/test_acp_sdk_remote_interop.py` runs the client that the official Python SDK (`agent-client-protocol` 0.12) ships for the draft transport against `neograph_acp_probe_agent --http`, over both transports; the TypeScript SDK is not run against them.
