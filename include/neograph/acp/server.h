/**
 * @file acp/server.h
 * @brief ACP server — exposes a NeoGraph as an Agent Client Protocol agent.
 *
 * Speaks JSON-RPC 2.0 over a transport. The default transport is
 * **newline-delimited JSON over stdio** (one message per line on stdin,
 * one per line on stdout) — the form used by Zed's agent-client-protocol
 * Rust crate and by every editor that launches an agent as a sub-process.
 * The implementation targets ACP protocol version 1 and is checked against
 * the official schema v1.25.0 (agent-client-protocol commit 4cf3dd85,
 * 2026-10-09) and the official Python and TypeScript SDKs.
 *
 * Methods served (client → agent):
 *   - initialize (exactly once, before all session methods)
 *   - session/new
 *   - session/resume
 *   - session/prompt
 *   - session/cancel  (notification — no response)
 *
 * Methods emitted (agent → client):
 *   - session/update  (streaming notification while a prompt is in flight)
 *   - session/request_permission, when a node asks for it or when the graph
 *     raises a NodeInterrupt (see ACPGraphAdapter::interrupt_permission)
 *   - fs/read_text_file, fs/write_text_file and terminal/&#42; only when the
 *     client advertised the matching capability in initialize.
 *
 * A failed turn is a JSON-RPC error on session/prompt (-32603) whose message
 * is a public message and whose `data` carries the NeoGraph failure code and
 * the retry hint; exception text never reaches the client. A provider
 * refusal ends the turn with stopReason `refusal`, a step limit with
 * `max_turn_requests`.
 *
 * Client-supplied stdio MCP servers (session/new `mcpServers`) are started
 * with NeoGraph's MCP client when the library is built with
 * NEOGRAPH_BUILD_MCP_CLIENT; their tools are available to nodes through
 * ACPClient::session_tools().
 *
 * Adapter pattern mirrors neograph::a2a — by default the inbound prompt
 * text is dropped into the `prompt` channel and the `response` channel
 * is sent back. Subclass @ref ACPGraphAdapter to plug in a richer
 * mapping (multi-modal blocks, system prompts, conversation history).
 */
#pragma once

#include <neograph/api.h>
#include <neograph/acp/types.h>
#include <neograph/graph/execution.h>

#include <chrono>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph {
class Tool;
}

namespace neograph::acp {

class ACPServer;
struct ACPServerTestAccess;

/**
 * @brief What a prompt turn knows about its session.
 *
 * Passed to ACPGraphAdapter::build_session_state. `cwd` and
 * `additional_directories` are the absolute paths the client sent in
 * session/new (or session/resume); relative paths a node receives from the
 * client resolve against `cwd`. `mcp_tools` names the tools of the session's
 * MCP servers (see ACPClient::session_tools).
 */
struct NEOGRAPH_API ACPSessionContext {
    std::string              session_id;
    std::string              cwd;
    std::vector<std::string> additional_directories;
    /// One object per tool: {"server": <mcpServers[].name>, "name": <tool>}.
    neograph::json           mcp_tools = neograph::json::array();
};

/**
 * @brief A NodeInterrupt raised during a prompt turn.
 *
 * `reason` and `payload` come from the interrupt; `checkpoint_id` is the
 * checkpoint the run paused at.
 */
struct NEOGRAPH_API ACPInterrupt {
    std::string    session_id;
    std::string    node;
    std::string    reason;
    neograph::json payload;
    std::string    checkpoint_id;
};

/// The session/request_permission request that asks the user about an
/// interrupt (see ACPGraphAdapter::interrupt_permission).
struct NEOGRAPH_API ACPInterruptPermission {
    ToolCallUpdate                tool_call;
    std::vector<PermissionOption> options;
};

/**
 * @brief Map between ACP `ContentBlock[]` and the engine's channels.
 *
 * The default concatenates the text of every block into the input channel —
 * text blocks verbatim, `resource_link` blocks as `[name](uri)`, embedded
 * text resources as their text — reads a string out of the output channel
 * and sends it as one `agent_message_chunk` before StopReason::EndTurn.
 *
 * Override to handle images/audio/embedded resources, to wire up a
 * conversation-history channel, or to change how interrupts are presented.
 */
class NEOGRAPH_API ACPGraphAdapter {
  public:
    virtual ~ACPGraphAdapter() = default;

    /// Channel name to write the inbound user text into (default "prompt").
    virtual std::string input_channel() const { return "prompt"; }

    /// Channel name to read the agent's text reply from (default "response").
    virtual std::string output_channel() const { return "response"; }

    /// Join the text of every block with single spaces: text blocks
    /// verbatim, `resource_link` as `[name](uri)`, embedded resources as their
    /// text (or as a link when they carry a blob). Image and audio blocks
    /// contribute nothing unless the adapter overrides this.
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

    /// What the server calls to build a turn's initial state. The default
    /// calls @ref build_initial_state (so existing overrides keep working) and
    /// then seeds, when the state is an object that does not set them already,
    /// `_acp_cwd`, `_acp_additional_directories` and `_acp_mcp_tools`. A graph
    /// that does not declare those channels ignores them.
    virtual neograph::json build_session_state(
        const std::vector<ContentBlock>& blocks,
        const ACPSessionContext& session) const;

    /// Pull the agent's text reply out of the graph's final state.
    /// Default looks at `channels[<output_channel()>].value`, then falls
    /// back to the content of the last assistant message in
    /// `channels.messages` (matches ReAct examples).
    virtual std::string extract_agent_text(
        const neograph::json& output) const;

    /**
     * Present a NodeInterrupt to the user. The server sends the returned
     * request as session/request_permission in the same prompt turn and
     * resumes the run with interrupt_resume_value() of the option the user
     * selects. The default is a tool call titled with the interrupt reason
     * (kind "other", status "pending", rawInput = the payload, and the
     * interrupt under `_meta["neograph/interrupt"]`) with the options
     * "approve" (allow_once) and "reject" (reject_once).
     *
     * Return std::nullopt when the interrupt cannot be answered by a choice;
     * the turn then fails with a JSON-RPC error whose data.code is
     * NG.Interrupted.
     */
    virtual std::optional<ACPInterruptPermission> interrupt_permission(
        const ACPInterrupt& interrupt) const;

    /// The resume value for the option the user selected. Default: the
    /// option id as a JSON string ("approve" or "reject").
    virtual neograph::json interrupt_resume_value(
        const ACPInterrupt& interrupt, const std::string& option_id) const;
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

    /// Per-call timeout for fs/* requests (default 30 s). Editors usually
    /// respond fast, but fs/* may be slow for large files.
    void set_timeout(std::chrono::milliseconds t);

    /// Timeout for session/request_permission. Default: none — a permission
    /// prompt waits for the user until the prompt turn is cancelled. Pass a
    /// duration to bound it; on expiry the call throws.
    void set_permission_timeout(std::optional<std::chrono::milliseconds> t);

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
    /// (kind == Cancelled — the prompt turn itself was cancelled, either
    /// because the client answered "cancelled" or because session/cancel
    /// arrived while the request was pending).
    /// Throws if not yet bound to a server.
    RequestPermissionOutcome request_permission(
        std::string_view session_id,
        const ToolCallUpdate&  tool_call,
        const std::vector<PermissionOption>& options);

    /// The tools of the session's client-supplied MCP servers (session/new
    /// `mcpServers`), in server order. Empty when the session has none, is
    /// unknown, or the library was built without the MCP client. A tool
    /// call goes to the MCP server process the client configured; the
    /// session owns those processes until it is replaced by session/resume
    /// or the server is destroyed.
    std::vector<std::shared_ptr<neograph::Tool>> session_tools(
        std::string_view session_id) const;

  private:
    ACPServer*               server_ = nullptr;
    std::chrono::milliseconds timeout_{std::chrono::seconds(30)};
    std::optional<std::chrono::milliseconds> permission_timeout_;
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
    /// @param info     agentInfo returned in initialize: an ACP Implementation
    ///                 `{"name":"my-agent","version":"0.1.0"}` with an
    ///                 optional string `title` and object `_meta`. A missing
    ///                 or non-string name/version is filled with
    ///                 "neograph-agent" / the NeoGraph version, and any other
    ///                 key is dropped, so the reply always validates.
    /// @param adapter  Optional input/output mapping override.
    ACPServer(std::shared_ptr<neograph::graph::GraphEngine> engine,
              neograph::json info,
              std::shared_ptr<ACPGraphAdapter> adapter = {});
    /// Preferred constructor: restrict the host to graph execution.
    ACPServer(neograph::graph::GraphExecution execution,
              neograph::json info,
              std::shared_ptr<ACPGraphAdapter> adapter = {});

    ~ACPServer();

    /// Mutable access to the capabilities advertised in initialize.
    /// Defaults: session.resume=true, load_session=false, no prompt extras,
    /// no MCP transports (stdio is required by spec — its presence is implicit).
    AgentCapabilities&       capabilities();
    const AgentCapabilities& capabilities() const;

    /// Drive the server loop reading newline-delimited JSON-RPC envelopes
    /// from `in` and writing them to `out`. Returns when `in` reaches EOF
    /// or @ref stop() is called from another thread.
    ///
    /// Lifecycle parallel with @ref neograph::a2a::A2AServer:
    /// `run()` here is the synchronous read-loop equivalent of A2A's
    /// `start()` (also synchronous, blocks until shutdown). Neither
    /// server exposes a built-in async-spawn helper because the host
    /// embedder usually picks the threading model; spawn `run()` on a
    /// `std::thread` if you want it to run in the background.
    void run(std::istream& in, std::ostream& out);

    /// Convenience overload — uses std::cin / std::cout.
    void run();

    /// True while `run()` is actively reading from its input stream.
    /// Set on entry, cleared on exit. Mirrors @ref
    /// neograph::a2a::A2AServer::is_running so a generic supervisor can
    /// poll either without protocol-specific casing.
    bool is_running() const;

    /// Process exactly one parsed JSON-RPC envelope. Returns:
    ///   - the response envelope for requests (`{ "jsonrpc","id","result"|"error" }`)
    ///   - a null json (no value) for notifications.
    /// Side-effect notifications go to the sink set via @ref
    /// set_notification_sink.
    neograph::json handle_message(const neograph::json& envelope);

    /// Where session/update notifications go. Default: dropped on the
    /// floor (test-friendly). When run() is driving, the sink is set to
    /// write to the output stream automatically. A sink retained by a
    /// shared_ptr-owned server must capture that server weakly to avoid an
    /// ownership cycle. Objects captured by reference must outlive the server
    /// (or clear the sink before they are destroyed); the server drops late
    /// worker notifications once its destructor begins.
    void set_notification_sink(NotificationSink sink);

    /// Signal a running run() loop to exit at the next message boundary.
    ///
    /// **Cancellation semantics**: ACP `session/cancel` signals the
    /// prompt's `CancelToken`, so engine checkpoints and cancellation-aware
    /// providers/tools stop cooperatively. A node that neither polls its token
    /// nor awaits a cancellation-aware operation still runs to its next engine
    /// checkpoint. The terminal session/prompt response then carries
    /// `StopReason::Cancelled`.
    ///
    /// session/cancel cancels the session's ongoing prompt turn; a cancel
    /// with no prompt running is ignored. An embedder whose transport can
    /// reorder messages may opt into @ref set_pre_armed_cancel, under which a
    /// cancel that arrives before a session's first prompt cancels it.
    ///
    /// While a turn waits for an agent→client request (fs/*, terminal/*,
    /// session/request_permission), session/cancel gives the client a short
    /// grace to answer on its own (ACP requires a client to answer pending
    /// permission requests with `cancelled`); after it the server sends
    /// `$/cancel_request` for the request and stops waiting.
    ///
    /// **Destructor semantics**: ~ACPServer joins all in-flight
    /// session/prompt worker threads before returning. Each worker
    /// blocks until its `engine->run()` completes, so destroying the
    /// server during a 60-second LLM call blocks the destroying
    /// thread for the full duration. Drain or cancel before
    /// destruction if you need a bounded-latency teardown.
    void stop();
    /// Bound concurrently executing sessions; reject excess prompts before
    /// any graph node runs (JSON-RPC -32603 with data.reason "overloaded").
    /// Defaults to 32.
    void set_max_inflight_prompts(std::size_t limit);
    bool initialized() const;

    /// Opt in to the pre-armed cancel: a session/cancel received after
    /// session/new but before the session's first prompt cancels that prompt.
    /// Off by default — on an ordered transport such as stdio a cancel with no
    /// prompt running is stale and must not cancel a later turn.
    void set_pre_armed_cancel(bool enabled);

    /// How long run() lets in-flight prompts finish after its input reaches
    /// EOF (the client closed the connection) before it cancels them.
    /// Defaults to one second.
    void set_eof_grace(std::chrono::milliseconds grace);

    /// The tools of a session's client-supplied MCP servers; see
    /// ACPClient::session_tools.
    std::vector<std::shared_ptr<neograph::Tool>> session_tools(
        std::string_view session_id) const;

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
    /// exposed publicly so user code can issue protocol extensions.
    /// fs/*, terminal/* and elicitation/* require the matching client
    /// capability. When `params.sessionId` names a session with a prompt
    /// turn in flight, cancelling that turn ends the wait (after the grace
    /// described at @ref stop) with neograph::graph::CancelledException.
    /// `timeout` of std::chrono::milliseconds::max() waits without a
    /// deadline. Throws std::runtime_error on transport not yet connected,
    /// on a missing capability, on an `error` envelope, and on timeout.
    neograph::json call_client(std::string method,
                                neograph::json params,
                                std::chrono::milliseconds timeout
                                    = std::chrono::seconds(30));

  private:
    friend struct ACPServerTestAccess;

    // Defined only in test builds. Kept private so the deterministic thread
    // failure seams are not part of the production ACP API.
    void fail_next_worker_launch_for_testing();
    void fail_next_handle_message_for_testing();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::acp
