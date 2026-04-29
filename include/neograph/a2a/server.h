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
 *   - message/send    — sync round-trip (graph runs, response wrapped as Task)
 *   - message/stream  — SSE-framed status updates while graph runs
 *   - tasks/get       — recall a Task from the in-memory store
 *   - tasks/cancel    — request cancellation (stops further super-steps)
 *
 * Built on httplib (already a dep) — one OS thread per connection,
 * fine for agent-style traffic where LLM call time dominates.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/a2a/types.h>
#include <neograph/graph/engine.h>

#include <functional>
#include <memory>
#include <string>

namespace neograph::a2a {

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
 */
class NEOGRAPH_API A2AServer {
  public:
    /// @param engine   The graph engine that handles incoming messages.
    /// @param card     Discovery payload returned at /.well-known/agent-card.json.
    /// @param adapter  Optional input/output mapping override.
    A2AServer(std::shared_ptr<neograph::graph::GraphEngine> engine,
              AgentCard card,
              std::shared_ptr<GraphAgentAdapter> adapter = {});

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
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::a2a
