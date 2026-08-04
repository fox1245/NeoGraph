/**
 * @file a2a/server.h
 * @brief A2A server — exposes a NeoGraph as an Agent-to-Agent endpoint.
 *
 * Speaks the JSON-RPC v0.3 dialect (slash-form method names, `kind`
 * discriminator) so clients written against either the a2a-js spec or
 * a2a-sdk Python in v0.3 compat mode can connect.
 *
 * Endpoints:
 *   - GET  /.well-known/agent-card.json   — AgentCard discovery
 *   - POST /                                — JSON-RPC dispatcher
 *
 * Methods:
 *   - message/send    — sync round-trip (GraphEngine or ProgramRuntime,
 *                        response wrapped as Task)
 *   - message/stream  — SSE-framed status updates while a run executes
 *   - tasks/get       — recall a Task, reconnecting Program-backed runs when
 *                        their durable transition record is available
 *   - tasks/cancel    — request cancellation (stops further super-steps)
 *
 * Built on httplib (already a dep) — one OS thread per connection,
 * fine for agent-style traffic where LLM call time dominates.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/a2a/types.h>
#include <neograph/graph/engine.h>

#ifdef NEOGRAPH_A2A_PROGRAM
#include <neograph/a2a/collaboration.h>
#include <neograph/a2a/program_adapter.h>
#endif

#include <functional>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

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

#ifdef NEOGRAPH_A2A_PROGRAM
    /// Expose one admitted ProgramVersion through the existing A2A surface.
    /// The overload keeps legacy GraphEngine construction source-compatible.
    /**
     * Resolve the HTTP Authorization header into an identity the application
     * has authenticated. The server never retains the header or credentials.
     * Collaboration envelopes require a non-empty, matching result; ordinary
     * legacy A2A messages retain their existing unauthenticated behavior.
     */
    using CollaborationAuthenticator = std::function<std::optional<CollaborationPeerIdentity>(
        std::string_view authorization_header)>;

    A2AServer(std::shared_ptr<ProgramAgentAdapter> adapter,
              AgentCard card,
              CollaborationAuthenticator collaboration_authenticator = {});

    A2AServer(std::shared_ptr<neograph::program::ProgramRuntime> runtime,
              neograph::program::ProgramVersion version,
              std::string owner_scope,
              AgentCard card,
              std::shared_ptr<CollaborationMailbox> mailbox = {},
              CollaborationAuthenticator collaboration_authenticator = {});
    A2AServer(std::shared_ptr<neograph::program::ProgramRuntime> runtime,
              neograph::program::ProgramVersion version,
              AgentCard card,
              std::string owner_scope,
              std::shared_ptr<CollaborationMailbox> mailbox = {},
              CollaborationAuthenticator collaboration_authenticator = {});
#endif

    ~A2AServer();

    /// Bind + listen. Blocks the calling thread.
    /// @return false if bind/listen fails.
    bool start(const std::string& host, int port);

    /// Bind + spawn a worker thread; returns when the server is ready
    /// to accept connections. Use stop() to shut down.
    bool start_async(const std::string& host, int port);

    /// Signal the server to stop accepting connections and join.
    void stop();

    /// True once start_async() has the server listening.
    bool is_running() const;

    /// Bound port — useful when caller passed 0 to bind a free port.
    int port() const;

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
};

}  // namespace neograph::a2a::test
#endif
