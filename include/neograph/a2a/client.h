/**
 * @file a2a/client.h
 * @brief A2A (Agent-to-Agent) JSON-RPC client over Streamable HTTP.
 *
 * Implements the JSON-RPC 2.0 binding of the A2A protocol — see
 * https://a2a-protocol.org/latest/specification/. Methods supported in
 * this version:
 *   - `message/send`        (sync)
 *   - `tasks/get`
 *   - `tasks/cancel`
 *   - AgentCard discovery via GET `/.well-known/agent-card.json`
 *
 * Streaming (`message/stream`, SSE-framed `tasks/resubscribe`) is not
 * implemented in v1; the awaitable returns once the server reports
 * a terminal TaskState.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/a2a/types.h>

#include <asio/awaitable.hpp>

#include <chrono>
#include <functional>
#include <string>

namespace neograph::a2a {

/**
 * @brief A2A client — call a remote agent over JSON-RPC + HTTP.
 *
 * Thread-safe: every public method acquires its own ephemeral HTTP
 * connection; no shared in-flight state. Reuses
 * neograph::async::async_post for transport.
 *
 * @code
 * a2a::A2AClient client("https://agent.example.com");
 * auto card = client.fetch_agent_card();           // discovery
 * auto task = client.send_message_sync("Hello!");  // round-trip
 * std::cout << task.status.state;                  // TaskState::Completed
 * @endcode
 */
class NEOGRAPH_API A2AClient {
  public:
    /// @param base_url Agent endpoint URL (e.g. "https://agent.example.com").
    ///                 The well-known card path is appended on discovery.
    explicit A2AClient(std::string base_url);

    /// Override the default 30 s request timeout.
    void set_timeout(std::chrono::seconds t) { timeout_ = t; }

    /**
     * @brief GET /.well-known/agent-card.json.
     *
     * Exposes the agent's identity, transports, and skills. Caches the
     * result in-instance — repeat calls are no-ops unless `force` is set.
     */
    AgentCard fetch_agent_card(bool force = false);

    /// Async variant of @ref fetch_agent_card.
    asio::awaitable<AgentCard> fetch_agent_card_async(bool force = false);

    /**
     * @brief `message/send` — convenience wrapper around send_message_sync().
     * @param text Plain text content for a single TextPart.
     * @param task_id Optional existing task to continue.
     * @param context_id Optional grouping id.
     */
    Task send_message_sync(
        const std::string& text,
        const std::string& task_id    = "",
        const std::string& context_id = "");

    /// Send an arbitrary Message (multipart, file, structured data).
    Task send_message_sync(const MessageSendParams& params);

    /// Async variant — server's response payload may be either a Message or
    /// a Task; we coerce both into a Task with the message in `history` so
    /// callers have one shape to handle.
    asio::awaitable<Task> send_message_async(const MessageSendParams& params);

    /// `tasks/get` — fetch the latest snapshot of a task.
    Task get_task(const std::string& task_id, int history_length = 0);
    asio::awaitable<Task> get_task_async(const std::string& task_id, int history_length = 0);

    /// `tasks/cancel` — request cancellation. Returns updated Task.
    Task cancel_task(const std::string& task_id);
    asio::awaitable<Task> cancel_task_async(const std::string& task_id);

    /// `message/stream` — send a message and receive SSE-framed status
    /// updates as the agent progresses, plus the final Task.
    ///
    /// `on_event` is invoked synchronously on the network thread for
    /// each parsed StreamEvent. Return `true` to keep reading or
    /// `false` to abort early. The final Task (or a status-update with
    /// `final=true`) ends the stream regardless.
    using EventCallback = std::function<bool(const StreamEvent&)>;
    /// @deprecated Renamed to `EventCallback` to avoid colliding with
    /// `neograph::StreamCallback` (provider.h: `void(string)`). The
    /// shapes are different (return type + argument), so a TU pulling
    /// in both `<neograph/provider.h>` and `<neograph/a2a/client.h>`
    /// would shadow one with the other. Old name kept as an alias
    /// for back-compat — prefer `EventCallback` in new code.
    using StreamCallback [[deprecated("use a2a::A2AClient::EventCallback")]]
        = EventCallback;

    Task send_message_stream(const std::string& text,
                             EventCallback on_event,
                             const std::string& task_id    = "",
                             const std::string& context_id = "");

    Task send_message_stream(const MessageSendParams& params,
                             EventCallback on_event);

    /// Lower-level: arbitrary JSON-RPC method.
    json rpc_call(const std::string& method, const json& params);
    asio::awaitable<json> rpc_call_async(const std::string& method, const json& params);

    /// Two A2A protocol generations are deployed in the wild:
    /// v1 (PascalCase, e.g. "SendMessage") used by a2a-sdk Python ≥1.0.0,
    /// and v0.3 (slash-form, e.g. "message/send") used by a2a-js HEAD and
    /// pre-v1 deployments. Try @p v1_method first; on a -32601
    /// "method not found" reply, retry with @p v03_method on the same
    /// params. Other JSON-RPC errors propagate.
    asio::awaitable<json> rpc_call_with_fallback(
        const std::string& v1_method,
        const std::string& v03_method,
        const json& params);

    const std::string& base_url() const { return base_url_; }

  private:
    std::string base_url_;
    std::chrono::seconds timeout_ = std::chrono::seconds(30);
    int request_id_ = 0;

    /// Cached agent card (populated by fetch_agent_card).
    AgentCard cached_card_;
    bool      card_loaded_ = false;
};

} // namespace neograph::a2a
