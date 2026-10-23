/**
 * @file a2a/server.h
 * @brief A2A server — exposes a NeoGraph as an Agent-to-Agent endpoint.
 *
 * Speaks the JSON-RPC binding of A2A 1.0 (v1.0.1, the primary version:
 * PascalCase methods, ProtoJSON bodies, `A2A-Version: 1.0`) and keeps A2A 0.3
 * as a compatibility dialect (slash-form methods, `kind` discriminators, no
 * `A2A-Version` header). Each request is answered in the dialect its
 * `A2A-Version` names (header or query parameter); any other version gets
 * VersionNotSupportedError (-32009).
 *
 * Endpoints:
 *   - GET  /.well-known/agent-card.json   — one card per version: the pure
 *                                            1.0 card for `A2A-Version: 1.0`,
 *                                            the 0.3 card (also listing the
 *                                            1.0 interface) otherwise
 *   - POST /                                — JSON-RPC dispatcher
 *
 * Methods (1.0 / 0.3 spelling):
 *   - SendMessage / message/send — blocking, or `returnImmediately`
 *     (0.3 `blocking: false`) returns the SUBMITTED task at once
 *   - SendStreamingMessage / message/stream — SSE task lifecycle stream: the
 *     Task, status and artifact updates, closed on a terminal or
 *     interrupted state; a stream silent for 5 s writes the SSE comment
 *     `: keep-alive`
 *   - GetTask / tasks/get, CancelTask / tasks/cancel (a terminal task answers
 *     TaskNotCancelableError), ListTasks, SubscribeToTask / tasks/resubscribe
 *   - push-notification config methods answer
 *     PushNotificationNotSupportedError; GetExtendedAgentCard answers
 *     UnsupportedOperationError. Neither feature is claimed in the card.
 *
 * Requests are checked before they are routed: a loopback bind answers only
 * the loopback Host names and its own origin unless A2AHttpConfig lists
 * more, and a body over A2AHttpConfig::max_payload_bytes gets HTTP 413.
 *
 * Every run executes on its own worker thread; a stream or a blocking send
 * only observes it, so the task outlives a dropped connection. A failed run
 * reports only typed public fields (the failure's public message and
 * `metadata["neograph/failure"] = {code, retryable}`), never exception
 * text. A NodeInterrupt ends in INPUT_REQUIRED with the interrupt reason and
 * payload; a follow-up message to that task resumes the run from its
 * checkpoint (or reruns it when the engine has no checkpoint store).
 */
#pragma once

#include <neograph/api.h>
#include <neograph/a2a/types.h>
#include <neograph/graph/execution.h>

#ifdef NEOGRAPH_A2A_PROGRAM
#include <neograph/a2a/collaboration.h>
#include <neograph/a2a/program_adapter.h>
#endif

#include <chrono>
#include <functional>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef NEOGRAPH_A2A_PROGRAM
namespace neograph::program {
class ProgramRuntime;
class ProgramVersion;
}  // namespace neograph::program
#endif

namespace neograph::a2a {

#ifdef NEOGRAPH_A2A_PROGRAM
class ProgramAgentAdapter;
#endif

#ifdef NEOGRAPH_A2A_TESTING
namespace test { class A2AServerTestAccess; }
#endif

/**
 * @brief Adapt a NeoGraph run to the A2A request/response shape.
 *
 * Default: drop the inbound text into `prompt`, run the graph, return
 * the value of `response` as the agent's text. Subclass / override for
 * domain-specific input/output mapping.
 */
class NEOGRAPH_API GraphAgentAdapter {
  public:
    virtual ~GraphAgentAdapter() = default;

    /// Channel name to write the inbound user text into (default "prompt").
    virtual std::string input_channel() const { return "prompt"; }

    /// Channel name to read the agent's text response from (default "response").
    virtual std::string output_channel() const { return "response"; }

    /// Hook to populate the initial graph state. Default writes the
    /// inbound user text to @ref input_channel(). Override to add
    /// system prompts, conversation history, etc.
    virtual neograph::json build_initial_state(const std::string& user_text) const;

    /// Optionally promote a graph output into a typed A2A artifact.
    virtual std::optional<Artifact> build_output_artifact(
        const neograph::json& output,
        const std::string& task_id) const;

    /// The value a follow-up message to an INPUT_REQUIRED task resumes the
    /// interrupted run with (spec 3.4.3). Default: the message's text parts
    /// joined with spaces; without text, the first data part's value;
    /// otherwise null.
    virtual neograph::json build_resume_value(const Message& message) const;
};

/// Adapt one JSON-valued graph output channel into a versioned A2A DataPart.
class NEOGRAPH_API StructuredOutputAdapter final : public GraphAgentAdapter {
  public:
    StructuredOutputAdapter(std::string contract,
                            int version,
                            std::string data_channel,
                            std::string input_channel = "prompt");

    std::string input_channel() const override;
    std::string output_channel() const override;
    std::optional<Artifact> build_output_artifact(
        const neograph::json& output,
        const std::string& task_id) const override;

  private:
    std::string contract_;
    int         version_;
    std::string data_channel_;
    std::string input_channel_;
};

/**
 * @brief Which Host and Origin values an A2AServer answers, and how large a
 * request body it reads. Set with A2AServer::set_http_config() before start.
 *
 * A web page on another origin can reach a server bound to a loopback
 * address when its host name is re-pointed at this machine (DNS rebinding):
 * the browser then sends that name in `Host`, and the page's own origin in
 * `Origin` when it sends one. The checks cover every route of the server
 * (the agent card, the JSON-RPC endpoint and its event streams, and any path
 * it does not serve) and run before a request body is read. A refused
 * request gets HTTP 403, or 400 for a missing or malformed Host.
 */
struct NEOGRAPH_API A2AHttpConfig {
    /// Exact allowed Origin values. An Origin header is rejected unless
    /// listed, except that a loopback bind also admits its own loopback
    /// origin: `http://localhost:<port>`, `http://127.0.0.1:<port>` or
    /// `http://[::1]:<port>` at the port it is bound to (and writes a line to
    /// stderr the first time it admits each one). A request without an
    /// Origin header is not checked.
    std::vector<std::string> allowed_origins;

    /// Host header names the server answers: host names, IPv4 addresses and
    /// bracketed IPv6 addresses (`[::1]`), compared with the Host header's
    /// host without its port and ignoring case. `*` admits any Host. A
    /// loopback bind (`127.0.0.1`, `::1`, `localhost`) always admits
    /// `localhost`, `127.0.0.1` and `[::1]` and refuses every other Host
    /// unless it is listed, with or without an Origin. On a remote bind an
    /// empty list checks nothing, and a non-empty one admits only the listed
    /// names. Behind a reverse proxy or a tunnel that forwards the public
    /// Host to a loopback server, list that name (or `*`).
    std::vector<std::string> allowed_hosts;

    /// The largest request body the server reads; a larger one is answered
    /// with HTTP 413. The default, 4 MiB, fits the largest request an agent
    /// legitimately receives: a message whose parts carry long text or one
    /// inline file (base64 makes it a third larger than the file, so a file
    /// of a little under 3 MiB). A larger file travels as a `uri` part. A
    /// task keeps the messages it received in its history, and the server
    /// keeps up to 1024 tasks, so the cap also bounds what one request can
    /// add to memory; the HTTP library's own default is 100 MiB.
    std::size_t max_payload_bytes = 4 * 1024 * 1024;
};

/**
 * @brief HTTP server exposing a NeoGraph as an A2A agent.
 *
 * @code
 * a2a::AgentCard card;
 * card.name = "my-agent";
 * card.url  = "http://0.0.0.0:8080/";
 * a2a::A2AServer server(my_engine, card);
 * server.start("0.0.0.0", 8080);   // blocks until stop()
 * @endcode
 *
 * When NEOGRAPH_BUILD_PROGRAM is enabled, the ProgramAgentAdapter overload
 * projects an admitted ProgramVersion through ProgramRuntime while retaining
 * the same JSON-RPC methods and legacy GraphEngine constructor.
 */
class NEOGRAPH_API A2AServer {
  public:
    /// @param engine   The graph engine that handles incoming messages.
    /// @param card     Discovery payload returned at /.well-known/agent-card.json.
    /// @param adapter  Optional input/output mapping override.
    A2AServer(std::shared_ptr<neograph::graph::GraphEngine> engine,
              AgentCard card,
              std::shared_ptr<GraphAgentAdapter> adapter = {});
    /// Preferred constructor: retain only execution-facing capabilities.
    A2AServer(neograph::graph::GraphExecution execution,
              AgentCard card,
              std::shared_ptr<GraphAgentAdapter> adapter = {});

#ifdef NEOGRAPH_A2A_PROGRAM
    /// Expose one admitted ProgramVersion through the existing A2A surface.
    /// The overload keeps legacy GraphEngine construction source-compatible.
    /**
     * Resolve the HTTP Authorization header into an identity the application
     * has authenticated. The server never retains the header or credentials.
     * Collaboration envelopes require a non-empty, matching result. Setting
     * require_authenticated_requests additionally fail-closes ordinary
     * SendMessage, SendStreamingMessage, GetTask, CancelTask, ListTasks and
     * SubscribeToTask calls; its default is false for source-compatible
     * legacy behavior.
     *
     * With an authenticator the card declares an HTTP `Bearer` security
     * scheme (unless AgentCard::security_schemes names one), and a global
     * security requirement when requests must be authenticated. A request
     * without valid credentials where they are required fails with HTTP 401
     * and a `WWW-Authenticate` challenge (spec 7.4); an authenticated peer
     * that may not use a collaboration link gets HTTP 403.
     */
    using CollaborationAuthenticator = std::function<std::optional<CollaborationPeerIdentity>(
        std::string_view authorization_header)>;

    A2AServer(std::shared_ptr<ProgramAgentAdapter> adapter,
              AgentCard card,
              CollaborationAuthenticator collaboration_authenticator = {},
              bool require_authenticated_requests = false);

    A2AServer(std::shared_ptr<neograph::program::ProgramRuntime> runtime,
              neograph::program::ProgramVersion version,
              std::string owner_scope,
              AgentCard card,
              std::shared_ptr<CollaborationMailbox> mailbox = {},
              CollaborationAuthenticator collaboration_authenticator = {},
              bool require_authenticated_requests = false);
    A2AServer(std::shared_ptr<neograph::program::ProgramRuntime> runtime,
              neograph::program::ProgramVersion version,
              AgentCard card,
              std::string owner_scope,
              std::shared_ptr<CollaborationMailbox> mailbox = {},
              CollaborationAuthenticator collaboration_authenticator = {},
              bool require_authenticated_requests = false);
#endif

    ~A2AServer();

    /// Bind + listen. Blocks the calling thread.
    /// @return false if bind/listen fails.
    /// Empty discovery URLs use the bound host/port and follow later rebinds;
    /// an explicitly configured public URL is preserved.
    bool start(const std::string& host, int port);

    /// Bind + spawn a worker thread; returns when the server is ready
    /// to accept connections. Use stop() to shut down.
    /// Applies the same discovery URL rule as start(), including ephemeral ports.
    bool start_async(const std::string& host, int port);

    /// Signal the server to stop accepting connections and join. Requests
    /// already being handled are answered first, a stream with at least its
    /// opening Task; a request that arrives meanwhile gets JSON-RPC error
    /// -32603 (reason UNAVAILABLE). Open streams and blocking sends end when
    /// their runs publish a final state, and a stop never cancels a run.
    void stop();

    /// True once start_async() has the server listening.
    bool is_running() const;

    /// Bound port — useful when caller passed 0 to bind a free port.
    int port() const;

    /// Set the Host and Origin checks and the request size limit (see
    /// A2AHttpConfig). Without a call, a loopback bind answers only the
    /// loopback Host names and its own loopback origin, a remote bind checks
    /// no Host and admits a request that carries an Origin only when it is
    /// listed, and bodies are limited to the default of
    /// A2AHttpConfig::max_payload_bytes.
    /// @throws std::invalid_argument for an `allowed_hosts` entry that is not
    ///         a host name without a port, an IPv6 address in brackets or
    ///         `*`, or for a zero `max_payload_bytes`.
    /// @throws std::logic_error while the server is listening: call it before
    ///         start() or start_async().
    void set_http_config(A2AHttpConfig config);

    /// When the card has no configured `url`, the served card advertises the
    /// address the client used, with the `http` scheme. Behind a
    /// TLS-terminating reverse proxy, either configure `AgentCard::url` (the
    /// public URL, always preferred) or enable this to take the scheme and
    /// host from RFC 7239 `Forwarded`, else `X-Forwarded-Proto` /
    /// `X-Forwarded-Host`. Off by default: those headers are set by the
    /// client unless a proxy you control overwrites them.
    void set_trust_forwarded_headers(bool enabled);

  private:
#ifdef NEOGRAPH_A2A_TESTING
    friend class test::A2AServerTestAccess;
#endif
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::a2a

#ifdef NEOGRAPH_A2A_TESTING
namespace neograph::a2a::test {

class A2AServerTestAccess {
  public:
    static void set_max_inflight_runs(A2AServer& server, std::size_t cap);
    /// How long a task stream may be silent before it sends an SSE comment.
    static void set_stream_keepalive(A2AServer& server, std::chrono::milliseconds interval);
    /// Runs in a streaming request just before it hands its stream to the
    /// HTTP server (#493), so a test can hold the request there.
    static void set_stream_handoff_hook(A2AServer& server, std::function<void()> hook);
    /// Whether stop() is waiting for the requests it has (#493).
    static bool draining(const A2AServer& server);
};

}  // namespace neograph::a2a::test
#endif
