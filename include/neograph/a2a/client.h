/**
 * @file a2a/client.h
 * @brief A2A (Agent-to-Agent) JSON-RPC client over Streamable HTTP.
 *
 * Implements the JSON-RPC 2.0 binding of the A2A protocol — see
 * https://a2a-protocol.org/latest/specification/. Methods supported:
 *   - send message (`SendMessage` / `message/send`)
 *   - streaming send (`SendStreamingMessage` / `message/stream`, SSE)
 *   - get task (`GetTask` / `tasks/get`)
 *   - cancel task (`CancelTask` / `tasks/cancel`)
 *   - AgentCard discovery via GET `/.well-known/agent-card.json`
 *
 * Two wire generations are spoken (see WireDialect in types.h): the A2A
 * 1.0 protobuf-JSON form (PascalCase methods, `A2A-Version: 1.0` header,
 * `ROLE_*` / `TASK_STATE_*` enums, flat Parts, no `kind`) — the primary
 * version — and the 0.3 JSON-Schema form (slash-form methods, `kind`
 * discriminators), kept for compatibility. The first call fetches the
 * AgentCard and selects an interface from it; only when no card can be
 * fetched does the client probe, 1.0 first.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/a2a/types.h>

#include <asio/awaitable.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace neograph::a2a {

/// JSON-RPC error object returned by the remote agent. `what()` carries
/// "A2A RPC error (code=<n>): <message>".
class NEOGRAPH_API A2ARpcError : public std::runtime_error {
  public:
    A2ARpcError(int code, const std::string& what)
        : std::runtime_error(what), code_(code) {}
    int code() const noexcept { return code_; }

  private:
    int code_;
};

/**
 * @brief A2A client — call a remote agent over JSON-RPC + HTTP.
 *
 * Thread-safe for concurrent use of one instance. `base_url` is immutable;
 * `set_timeout` and `set_connect_timeout` are synchronized and each request
 * snapshots them before network I/O, so an update cannot race or alter an
 * in-flight request.
 * JSON-RPC request IDs are process-local atomic counters. Agent-card cache
 * initialization and `force` refreshes are single-flight and synchronized;
 * waiting callers receive a copy, and no network I/O occurs under the state
 * lock. Stream callbacks still run on the network thread and must provide
 * their own synchronization for shared application state. Reuses
 * neograph::async::async_post for transport.
 *
 * Interface selection (spec 8.3.2): before its first RPC the client fetches
 * the AgentCard (unless @ref fetch_agent_card already did) and picks the
 * first `supportedInterfaces` entry with a `JSONRPC` binding and a `1.x`
 * (-> WireDialect::V1_0) or `0.x` (-> V0_3) protocol version, preferring an
 * entry whose URL equals the client's `base_url`; a 0.3 card without
 * `supportedInterfaces` selects V0_3 from its `protocolVersion` /
 * `preferredTransport` / `additionalInterfaces`. When the card offers no
 * compatible interface the call throws `std::runtime_error` naming what the
 * card offers. A selected interface `tenant` is copied into every request.
 *
 * Endpoint policy: RPCs go to the selected interface's URL when it has the
 * same origin (scheme, host, port) as `base_url`, for example an agent whose
 * JSON-RPC endpoint is at `/a2a/v1`. A card cannot redirect the client to
 * another origin unless @ref set_allow_cross_origin_interfaces opts in; a
 * cross-origin (or unusable) interface URL is otherwise ignored and the RPC
 * goes to `base_url`.
 *
 * When no card can be fetched, the client probes: 1.0 first, then 0.3 when
 * the agent answers -32601 (method not found), -32009 (version not
 * supported) or HTTP 400, and remembers the outcome.
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

    /// Bound the whole exchange of each request (connect, write, and reading
    /// the reply or the event stream to its end) to `t`; an exchange that is
    /// not done by then throws. The default is no bound: a blocking
    /// `SendMessage` runs until the agent answers and a stream until the
    /// agent ends it, since a task may take minutes. Zero removes a bound set
    /// earlier. Connecting is bounded separately, see
    /// @ref set_connect_timeout.
    void set_timeout(std::chrono::seconds t);

    /// Bound the time to resolve the host, connect and finish the TLS
    /// handshake of each request to `t` (default 30 s). Unlike
    /// @ref set_timeout it stops counting once the request can be written,
    /// so a slow reply or a long stream is not affected. Zero removes the
    /// bound.
    void set_connect_timeout(std::chrono::seconds t);

    /// Allow the AgentCard to direct RPCs to an interface URL on another
    /// origin than `base_url` (default false: such URLs are ignored and RPCs
    /// go to `base_url`). Takes effect at the next interface selection.
    void set_allow_cross_origin_interfaces(bool allow);

    /// Wire dialect in use: selected from the fetched AgentCard or learned by
    /// probing. `nullopt` until the first card-selected RPC / successful probe.
    std::optional<WireDialect> wire_dialect() const;

    /**
     * Set the explicit Authorization header for subsequent RPC requests.
     * Passing an empty string clears it. The client retains no separate token
     * representation and never copies this value into A2A payload metadata.
     */
    void set_authorization_header(std::string authorization_header);

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

    /// `SendStreamingMessage` / `message/stream` — send a message and receive
    /// SSE-framed status updates as the agent progresses, plus the final Task
    /// (assembled from the stream's Task / status / artifact events for 1.0
    /// agents, which have no terminal `final` frame).
    ///
    /// `on_event` is invoked synchronously on the network thread for
    /// each parsed StreamEvent. Return `true` to keep reading or
    /// `false` to abort early: the connection is closed at once and the
    /// call returns the task assembled so far. An error frame ends the
    /// call the same way and is thrown as A2ARpcError. The final Task (or
    /// a status-update with `final=true`) ends the stream regardless. The
    /// stream has no time limit unless @ref set_timeout sets one.
    using EventCallback = std::function<bool(const StreamEvent&)>;

    Task send_message_stream(const std::string& text,
                             EventCallback on_event,
                             const std::string& task_id    = "",
                             const std::string& context_id = "");

    Task send_message_stream(const MessageSendParams& params,
                             EventCallback on_event);

    /// Lower-level: arbitrary JSON-RPC method, sent verbatim. When
    /// @p dialect is V1_0 the `A2A-Version: 1.0` header is attached.
    /// Throws A2ARpcError for a JSON-RPC error reply.
    json rpc_call(const std::string& method, const json& params,
                  std::optional<WireDialect> dialect = std::nullopt);
    asio::awaitable<json> rpc_call_async(
        const std::string& method, const json& params,
        std::optional<WireDialect> dialect = std::nullopt);

    const std::string& base_url() const { return base_url_; }

    /// The JSON-RPC endpoint in use: the selected interface URL, or
    /// `base_url` before a selection or when the policy ignores the card URL.
    std::string endpoint_url() const;

  private:
    std::string base_url_;
    std::chrono::seconds timeout_ = std::chrono::seconds(0);          // 0 = no whole-exchange bound
    std::chrono::seconds connect_timeout_ = std::chrono::seconds(30); // 0 = no connect bound
    std::atomic<int> request_id_{0};

    mutable std::shared_ptr<std::mutex> state_mutex_ = std::make_shared<std::mutex>();
    bool card_loading_ = false;

    /// Cached agent card (populated by fetch_agent_card).
    AgentCard cached_card_;
    bool      card_loaded_ = false;
    /// The card could not be fetched; calls probe instead of refetching.
    bool      card_unavailable_ = false;
    bool      allow_cross_origin_ = false;
    std::string authorization_header_;

    /// Dialect/tenant/endpoint chosen from the card or learned by probing.
    struct Selection {
        WireDialect dialect = WireDialect::V0_3;
        std::string tenant;
        std::string url;   ///< JSON-RPC endpoint
    };
    std::optional<Selection> selection_;

    /// Card-derived selection (memoised), probe result, or nullopt when the
    /// dialect is still unknown. Throws when the card has no compatible
    /// interface.
    std::optional<Selection> resolve_selection();
    /// resolve_selection(), fetching the card first when none was fetched.
    asio::awaitable<std::optional<Selection>> select_async();
    void remember_probe(WireDialect dialect);

    asio::awaitable<json> post_rpc(const std::string& url, const std::string& method,
                                   const json& params, std::optional<WireDialect> dialect);

    /// One unary A2A call: picks the method name and request body for the
    /// resolved (or probed) dialect. @p build receives the dialect and the
    /// selected interface tenant.
    asio::awaitable<json> call_method(
        const char* v1_method, const char* v03_method,
        const std::function<json(WireDialect, const std::string& tenant)>& build);

    Task stream_once(WireDialect dialect, const std::string& tenant, const std::string& url,
                     const MessageSendParams& params, const EventCallback& on_event,
                     bool& saw_events);

    std::string request_authorization_header() const;

    std::chrono::seconds request_timeout() const;
    std::chrono::seconds request_connect_timeout() const;
};

} // namespace neograph::a2a
