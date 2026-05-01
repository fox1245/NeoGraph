/**
 * @file acp/server.h
 * @brief ACP server — exposes a NeoGraph as an Agent Client Protocol agent.
 *
 * Speaks JSON-RPC 2.0 over a transport. The default transport is
 * **newline-delimited JSON over stdio** (one message per line on stdin,
 * one per line on stdout) — the form used by Zed's agent-client-protocol
 * Rust crate and by every editor that launches an agent as a sub-process.
 *
 * Methods served (client → agent):
 *   - initialize
 *   - session/new
 *   - session/prompt
 *   - session/cancel  (notification — no response)
 *
 * Methods emitted (agent → client):
 *   - session/update  (streaming notification while a prompt is in flight)
 *
 * Deferred (return JSON-RPC -32601 Method not found until added):
 *   authenticate, session/load, session/resume, session/list, session/close,
 *   session/set_config_option, session/set_mode.
 *
 * Adapter pattern mirrors neograph::a2a — by default the inbound prompt
 * text is dropped into the `prompt` channel and the `response` channel
 * is sent back. Subclass @ref ACPGraphAdapter to plug in a richer
 * mapping (multi-modal blocks, system prompts, conversation history).
 */
#pragma once

#include <neograph/api.h>
#include <neograph/acp/types.h>
#include <neograph/graph/engine.h>

#include <chrono>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace neograph::acp {

class ACPServer;

/**
 * @brief Map between ACP `ContentBlock[]` and the engine's channels.
 *
 * The default does the simplest possible thing: concatenate all text
 * blocks into the input channel; read a string out of the output
 * channel; emit that string as a single `agent_message_chunk` followed
 * by StopReason::Completed.
 *
 * Override to handle images/audio/embedded resources, to wire up a
 * conversation-history channel, or to emit per-super-step thought
 * chunks during the run (see @ref emit_chunks_from_state below).
 */
class NEOGRAPH_API ACPGraphAdapter {
  public:
    virtual ~ACPGraphAdapter() = default;

    /// Channel name to write the inbound user text into (default "prompt").
    virtual std::string input_channel() const { return "prompt"; }

    /// Channel name to read the agent's text reply from (default "response").
    virtual std::string output_channel() const { return "response"; }

    /// Concatenate every text-typed ContentBlock with single-space joins;
    /// non-text blocks are ignored unless the adapter overrides this.
    virtual std::string extract_user_text(
        const std::vector<ContentBlock>& blocks) const;

    /// Build the initial graph state for a turn. Default behaviour:
    /// writes the user text to @ref input_channel() AND seeds the
    /// `_acp_session_id` channel with the active session id (so
    /// nodes can locate their session for fs/* / permission calls).
    /// Subclasses overriding this should preserve `_acp_session_id`
    /// if they want @ref ACPClient calls to keep working.
    virtual neograph::json build_initial_state(
        const std::vector<ContentBlock>& blocks,
        const std::string& session_id) const;

    /// Pull the agent's text reply out of the graph's final state.
    /// Default looks at `channels[<output_channel()>].value`, then falls
    /// back to `channels.messages[-1].content` (matches ReAct examples).
    virtual std::string extract_agent_text(
        const neograph::json& output) const;
};

/**
 * @brief Agent-side handle for issuing requests back to the editor.
 *
 * ACP is bidirectional — the client (editor) drives the agent with
 * session/prompt etc., but the agent talks back not just via
 * `session/update` notifications but also via *requests* like
 * `fs/read_text_file` (read a workspace file) and
 * `fs/write_text_file` (write a workspace file). These calls block
 * until the editor responds.
 *
 * Obtain an instance via @ref ACPServer::client(). Capture it inside a
 * node's lambda; pull `session_id` out of the graph state (the default
 * @ref ACPGraphAdapter writes it to the `_acp_session_id` channel).
 *
 * @code
 * auto client = server.client();
 * NodeFactory::instance().register_type("read_workspace_file",
 *     [client](const std::string& n, const json&, const NodeContext&) {
 *         return std::make_unique<MyNode>(n, client);
 *     });
 *
 * // inside MyNode::execute(state):
 * auto sid  = state.get("_acp_session_id").get<std::string>();
 * auto path = state.get("path").get<std::string>();
 * auto src  = client->read_text_file(sid, path);
 * @endcode
 *
 * Calls throw `std::runtime_error` on transport failure, on
 * JSON-RPC error envelopes, and on timeout.
 */
class NEOGRAPH_API ACPClient {
  public:
    /// Construct an unbound client. Bind it to a server later via
    /// @ref bind or @ref ACPServer::attach_client. This is the form
    /// you want when nodes capture the client during compile() — long
    /// before the ACPServer instance exists.
    ACPClient();

    /// Construct a client already bound to `server`.
    explicit ACPClient(ACPServer* server);

    /// Late-binding hook — set the back-pointer to the server. Idempotent.
    void bind(ACPServer* server);

    /// True once @ref bind has been called with a non-null server.
    bool bound() const noexcept;

    /// Default per-call timeout. Editors usually respond fast, but
    /// fs/* may be slow for large files; tune via @ref set_timeout.
    void set_timeout(std::chrono::milliseconds t);

    /// Read a workspace file via the editor. `line` is 1-based;
    /// omitted = from start. `limit` is the max line count;
    /// omitted = whole file. Throws if not yet bound to a server.
    std::string read_text_file(std::string_view session_id,
                               std::string_view path,
                               std::optional<int> line  = {},
                               std::optional<int> limit = {});

    /// Write (create or overwrite) a workspace file via the editor.
    /// Throws if not yet bound to a server.
    void write_text_file(std::string_view session_id,
                         std::string_view path,
                         std::string_view content);

    /// Ask the editor to surface a permission prompt for an upcoming
    /// tool call. Blocks until the user picks an option or the prompt
    /// is cancelled. The returned outcome tells the caller whether to
    /// proceed (kind == Selected, inspect optionId/kind) or abort
    /// (kind == Cancelled — the prompt turn itself was cancelled).
    /// Throws if not yet bound to a server.
    RequestPermissionOutcome request_permission(
        std::string_view session_id,
        const ToolCall&  tool_call,
        const std::vector<PermissionOption>& options);

  private:
    ACPServer*               server_ = nullptr;
    std::chrono::milliseconds timeout_{std::chrono::seconds(30)};
};

/**
 * @brief HTTP-less, stdio-first ACP server hosting a NeoGraph engine.
 *
 * Lifecycle:
 *
 *   ACPServer server(engine, info);
 *   server.run();                    // blocks: reads std::cin, writes std::cout
 *
 * The single-shot @ref handle_message form is provided for testing and
 * for embedding inside a custom transport (HTTP, WebSocket, in-process
 * pipe). It takes one parsed JSON-RPC envelope and returns the
 * response envelope (or a null json for notifications); side-effect
 * notifications (session/update) are delivered through the @ref
 * NotificationSink set on the server.
 */
class NEOGRAPH_API ACPServer {
  public:
    /// Sink for agent→client notifications (session/update). Receives
    /// the fully-formed JSON-RPC envelope ready to be written to the
    /// transport.
    using NotificationSink = std::function<void(const neograph::json&)>;

    /// @param engine   The graph engine that handles incoming prompts.
    /// @param info     Free-form agent_info bag returned in initialize.
    ///                 Typically `{"name":"my-agent","version":"0.1.0"}`.
    /// @param adapter  Optional input/output mapping override.
    ACPServer(std::shared_ptr<neograph::graph::GraphEngine> engine,
              neograph::json info,
              std::shared_ptr<ACPGraphAdapter> adapter = {});

    ~ACPServer();

    /// Mutable access to the capabilities advertised in initialize.
    /// Defaults: load_session=false, no prompt extras, no MCP transports
    /// (stdio is required by spec — its presence is implicit).
    AgentCapabilities&       capabilities();
    const AgentCapabilities& capabilities() const;

    /// Drive the server loop reading newline-delimited JSON-RPC envelopes
    /// from `in` and writing them to `out`. Returns when `in` reaches EOF
    /// or @ref stop() is called from another thread.
    void run(std::istream& in, std::ostream& out);

    /// Convenience overload — uses std::cin / std::cout.
    void run();

    /// Process exactly one parsed JSON-RPC envelope. Returns:
    ///   - the response envelope for requests (`{ "jsonrpc","id","result"|"error" }`)
    ///   - a null json (no value) for notifications.
    /// Side-effect notifications go to the sink set via @ref
    /// set_notification_sink.
    neograph::json handle_message(const neograph::json& envelope);

    /// Where session/update notifications go. Default: dropped on the
    /// floor (test-friendly). When run() is driving, the sink is set to
    /// write to the output stream automatically.
    void set_notification_sink(NotificationSink sink);

    /// Signal a running run() loop to exit at the next message boundary.
    ///
    /// **Cancellation semantics**: ACP `session/cancel` flips a flag
    /// that the server consults *after* `engine->run()` naturally
    /// returns — it does NOT preempt an in-flight graph turn. The
    /// session/prompt response will carry `StopReason::Cancelled` if
    /// the flag was set by the time the graph finished, but a
    /// long-running LLM call inside a node still completes in full.
    /// Wire cancel into your nodes (or set a tight `max_steps` on
    /// `RunConfig`) for shorter cancel latency.
    ///
    /// **Destructor semantics**: ~ACPServer joins all in-flight
    /// session/prompt worker threads before returning. Each worker
    /// blocks until its `engine->run()` completes, so destroying the
    /// server during a 60-second LLM call blocks the destroying
    /// thread for the full duration. Drain or cancel before
    /// destruction if you need a bounded-latency teardown.
    void stop();

    /// True after at least one initialize has been processed.
    bool initialized() const;

    /// Handle for issuing agent→client requests (fs/*, etc.). On first
    /// call lazily creates a client bound to this server and caches it.
    /// Subsequent calls return the same instance. Nodes in the graph
    /// capture this handle to talk back to the editor.
    std::shared_ptr<ACPClient> client();

    /// Install a pre-constructed client and bind it to this server.
    /// Useful when the engine's nodes captured a client at compile()
    /// time before the server existed: pass that same shared_ptr here
    /// to make it usable. Replaces any previously cached client.
    void attach_client(std::shared_ptr<ACPClient> c);

    /// Issue a JSON-RPC request to the connected client and block
    /// until a response arrives. Used internally by @ref ACPClient;
    /// exposed publicly so user code can issue protocol extensions
    /// (e.g. session/request_permission once that surface is added).
    /// Throws on transport not yet connected, on `error` envelope, and
    /// on timeout.
    neograph::json call_client(std::string method,
                                neograph::json params,
                                std::chrono::milliseconds timeout
                                    = std::chrono::seconds(30));

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::acp
