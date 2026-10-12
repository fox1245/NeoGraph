#include <neograph/mcp/client.h>
#include <neograph/mcp/json_schema.h>

#include <neograph/async/endpoint.h>
#include <neograph/async/http_client.h>
#include <neograph/async/run_sync.h>
#include <neograph/async/sse_parser.h>
#include <neograph/graph/cancel.h>
#include <neograph/graph/types.h>

#include <asio/co_spawn.hpp>
#include <asio/error.hpp>
#include <asio/multiple_exceptions.hpp>
#include <asio/system_error.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/detached.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/experimental/channel_error.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/read_until.hpp>
#include <asio/steady_timer.hpp>
#include <asio/streambuf.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <asio/associated_executor.hpp>
#include <asio/async_result.hpp>

#ifdef _WIN32
#  include <asio/windows/stream_handle.hpp>
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <asio/posix/stream_descriptor.hpp>
#  include <fcntl.h>
#  include <pthread.h>
#  include <signal.h>
#  include <sys/resource.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  if defined(__linux__)
#    include <sys/syscall.h>
#  endif
#  if defined(__APPLE__)
#    include <crt_externs.h>
#  endif
#endif

#include <atomic>
#include <algorithm>
#include <array>
#include <limits>
#include <cerrno>
#include <cstdlib>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <cwchar>
#include <functional>
#include <future>
#include <istream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <unordered_map>

namespace neograph::mcp {

namespace {
json extract_rpc_result(const json& response, int expected_id) {
    constexpr auto prefix = "MCP JSON-RPC";
    if (!response.is_object() || !response.contains("jsonrpc")
        || !response["jsonrpc"].is_string() || response["jsonrpc"] != "2.0") {
        throw MCPTransportError(MCPFailure::protocol,
                                "MCP JSON-RPC response is not a JSON-RPC 2.0 object");
    }
    if (!response.contains("id") || response["id"] != expected_id) {
        throw MCPTransportError(MCPFailure::protocol,
                                "MCP JSON-RPC response id does not match the request");
    }
    const bool has_result = response.contains("result");
    const bool has_error = response.contains("error");
    if (has_result == has_error) {
        throw MCPTransportError(MCPFailure::protocol,
                                "MCP JSON-RPC response must contain exactly one of result or error");
    }
    if (has_error) {
        const auto& error = response["error"];
        if (!error.is_object() || !error.value("code", json()).is_number_integer()
            || !error.value("message", json()).is_string()) {
            throw MCPTransportError(MCPFailure::protocol,
                                    "MCP JSON-RPC error must contain integer code and string message");
        }
        throw MCPError(error["code"].get<int>(),
                       std::string(prefix) + " error: " + error["message"].get<std::string>(),
                       error.value("data", json(nullptr)));
    }
    return response["result"];
}

asio::awaitable<void> wait_for_rpc_bound(
    std::chrono::steady_clock::time_point deadline,
    const std::shared_ptr<graph::CancelToken>& cancel_token,
    const std::shared_ptr<graph::CancelToken>& shutdown_token = {}) {
    while ((!cancel_token || !cancel_token->is_cancelled())
           && (!shutdown_token || !shutdown_token->is_cancelled())) {
        if (deadline <= std::chrono::steady_clock::now()) co_return;
        asio::steady_timer timer(co_await asio::this_coro::executor);
        timer.expires_at(std::min(deadline, std::chrono::steady_clock::now()
                                            + std::chrono::milliseconds(10)));
        co_await timer.async_wait(asio::use_awaitable);
    }
    co_return;
}

[[noreturn]] void throw_rpc_bound(
    std::chrono::steady_clock::time_point deadline,
    const std::shared_ptr<graph::CancelToken>& cancel_token,
    const std::shared_ptr<graph::CancelToken>& shutdown_token = {}) {
    if (shutdown_token && shutdown_token->is_cancelled())
        throw MCPTransportError(MCPFailure::shutdown, "MCP client has been shut down");
    if (cancel_token && cancel_token->is_cancelled()) {
        throw MCPTransportError(MCPFailure::cancelled, "MCP RPC cancelled");
    }
    if (deadline <= std::chrono::steady_clock::now()) {
        throw MCPTransportError(MCPFailure::timeout, "MCP RPC deadline elapsed");
    }
    throw MCPTransportError(MCPFailure::shutdown,
                            "MCP RPC bounds waiter completed unexpectedly");
}
template <typename T>
struct CapturedResult {
    std::optional<T> value;
    std::exception_ptr  error;
};

template <typename T, typename Executor>
asio::awaitable<CapturedResult<T>, Executor> capture_result(
    asio::awaitable<T, Executor> operation) {
    CapturedResult<T> result;
    try {
        result.value.emplace(co_await std::move(operation));
    } catch (...) {
        result.error = std::current_exception();
    }
    co_return result;
}

void validate_stdio_config(const StdioClientConfig& config) {
    if (config.argv.empty() || config.argv.front().empty())
        throw std::invalid_argument("MCP stdio executable is empty");
    for (const auto& arg : config.argv)
        if (arg.find('\0') != std::string::npos)
            throw std::invalid_argument("MCP stdio argv contains NUL");
    if (config.cwd.native().find(
            std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
        throw std::invalid_argument("MCP stdio cwd contains NUL");
    for (std::size_t i = 0; i < config.environment.size(); ++i) {
        const auto& [key, value] = config.environment[i];
        if (key.empty() || key.find('=') != std::string::npos
            || key.find('\0') != std::string::npos || value.find('\0') != std::string::npos)
            throw std::invalid_argument("MCP stdio environment contains an invalid entry");
        for (std::size_t j = 0; j < i; ++j)
            if (config.environment[j].first == key)
                throw std::invalid_argument("MCP stdio environment contains a duplicate key");
    }
    if (!config.max_frame_bytes || config.startup_timeout.count() <= 0
        || config.request_timeout.count() <= 0 || config.cancel_grace.count() < 0)
        throw std::invalid_argument("MCP stdio bounds must be positive");
}

[[noreturn]] void throw_shutdown() {
    throw MCPTransportError(MCPFailure::shutdown, "MCP client has been shut down");
}

// How an operation ends when its awaiting coroutine is cancelled through
// Asio's cancellation slots (a graph run's cancel token does that): a socket
// or timer reports operation_aborted, a channel channel_cancelled.
bool aborted_by_caller(const std::error_code& code) {
    return code == asio::error::operation_aborted ||
           code == asio::experimental::error::channel_cancelled;
}

// How long a stdio server has to exit after its input closes, and after
// SIGTERM, before the client escalates.
constexpr std::chrono::milliseconds kStdioExitGrace{1000};
constexpr std::chrono::milliseconds kStdioTermGrace{1000};
// After a cancellation's grace period: SIGTERM at once, SIGKILL soon after.
constexpr std::chrono::milliseconds kStdioPromptTermGrace{100};

asio::awaitable<json> notification_result(asio::awaitable<void> operation) {
    co_await std::move(operation);
    co_return json();
}

// A notification nobody waits for: its failure changes nothing.
asio::awaitable<void> best_effort(asio::awaitable<void> operation) {
    try {
        co_await std::move(operation);
    } catch (...) {
    }
}

json cancelled_notification(int request_id, std::string reason) {
    return json{{"jsonrpc", "2.0"},
                {"method", "notifications/cancelled"},
                {"params", {{"requestId", request_id}, {"reason", std::move(reason)}}}};
}
} // namespace

// HTTP and subprocess transports implement the same frame exchange contract.
// Only the protocol session interprets JSON-RPC messages and MCP tool results.
// POSIX resolves PATH before fork and uses execve with subprocess pipes;
// Windows uses CreateProcess and asio::windows::stream_handle.
namespace detail {

// Admission and co_spawn initiation share one lock. Shutdown cannot stop the
// executor between a caller checking admission and enqueueing its operation.
// Transport work drains here, independently of the caller's executor.
class TransportWorker {
public:
    asio::io_context io;
    std::atomic<bool> stopped{false};

    ~TransportWorker() { shutdown([] {}); }

    asio::awaitable<json> submit(asio::awaitable<json> operation) {
        return asio::async_initiate<decltype(asio::use_awaitable),
            void(std::exception_ptr, json)>(
            [this, operation = std::move(operation)](auto handler) mutable {
                std::lock_guard lk(admission_mu_);
                if (stopped.load()) {
                    auto ex = asio::get_associated_executor(handler);
                    asio::post(ex, [handler = std::move(handler)]() mutable {
                        handler(std::make_exception_ptr(MCPTransportError(
                            MCPFailure::shutdown, "MCP client has been shut down")), json());
                    });
                    return;
                }
                start_locked();
                asio::co_spawn(io, std::move(operation), std::move(handler));
            }, asio::use_awaitable);
    }

    template <typename Start>
    void start(Start start) {
        std::lock_guard lk(admission_mu_);
        if (stopped.load()) throw_shutdown();
        start_locked();
        asio::post(io, std::move(start));
    }

    template <typename Close>
    void shutdown(Close close) noexcept {
        const bool own_thread = io.get_executor().running_in_this_thread();
        {
            std::lock_guard lk(admission_mu_);
            if (!stopped.exchange(true)) {
                if (own_thread || !thread_.joinable()) {
                    close();
                } else {
                    try { asio::post(io, std::move(close)); }
                    catch (...) { io.stop(); }
                }
                guard_.reset();
            }
        }
        if (!own_thread) {
            std::lock_guard lk(join_mu_);
            if (thread_.joinable()) thread_.join();
        }
    }

private:
    void start_locked() {
        if (thread_.joinable()) return;
        thread_ = std::thread([this] {
#ifndef _WIN32
            sigset_t mask;
            sigemptyset(&mask);
            sigaddset(&mask, SIGPIPE);
            ::pthread_sigmask(SIG_BLOCK, &mask, nullptr);
#endif
            io.run();
        });
    }
    asio::executor_work_guard<asio::io_context::executor_type> guard_{
        asio::make_work_guard(io)};
    std::mutex admission_mu_;
    std::mutex join_mu_;
    std::thread thread_;
};

// A transport owns its connection-specific state. The protocol layer owns
// request envelopes, response validation, initialization, and tool semantics.
// Correlation is performed by each transport using the envelope's id.
struct TransportCapabilities {
    bool concurrent_requests;
    bool cancellation;
    bool deadlines;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual TransportCapabilities capabilities() const noexcept = 0;
    virtual asio::awaitable<json> exchange(
        json request, std::chrono::steady_clock::time_point deadline,
        std::shared_ptr<graph::CancelToken> cancel_token) = 0;
    virtual asio::awaitable<void> notify(json notification) = 0;
    /// Tell the server that request `id` is abandoned (MCP
    /// notifications/cancelled), without making the caller wait: the
    /// notification goes out on the transport's own worker, best effort.
    virtual void cancel_request(int /*id*/, std::string /*reason*/) noexcept {}
    virtual void negotiated_version(const std::string&) {}
    virtual void reset() {}
    /// Forget the session @p stale the server no longer knows, when it is
    /// still the current one. True when this call forgot it.
    virtual bool forget_session(const std::string& /*stale*/) { return false; }
    /// Where server notifications go; set once by the protocol session.
    void set_notification_handler(std::function<void(const json&)> handler) {
        std::lock_guard lk(notification_mu_);
        notification_handler_ = std::move(handler);
    }
    /// Open the transport's channel for server messages between calls, when
    /// it has one that is not always open (HTTP's optional GET stream).
    virtual void listen() noexcept {}
    virtual void shutdown() noexcept = 0;

protected:
    void deliver_notification(const json& notification) noexcept {
        std::function<void(const json&)> handler;
        {
            std::lock_guard lk(notification_mu_);
            handler = notification_handler_;
        }
        try {
            if (handler) handler(notification);
        } catch (...) {
            // A notification handler cannot fail the transport.
        }
    }

private:
    std::mutex notification_mu_;
    std::function<void(const json&)> notification_handler_;
};

// The HTTP server answered 404 for a request that carried Mcp-Session-Id:
// that session ended, and the server did not run the request. The protocol
// session starts a new session and sends the request again (MCP-C-03).
class SessionNotFound final : public MCPTransportError {
public:
    SessionNotFound(std::string stale, std::string message)
        : MCPTransportError(MCPFailure::http_status, std::move(message), 404),
          stale_(std::move(stale)) {}
    const std::string& session_id() const noexcept { return stale_; }

private:
    std::string stale_;
};

namespace {

// NeoGraph's client declares no client capability (roots, sampling,
// elicitation), so it answers a server's `ping` and refuses every other
// server request with -32601 (MCP-C-02, D8). Every request gets a response,
// as JSON-RPC requires.
json server_request_reply(const json& request) {
    const json id = request["id"];
    const auto method = request["method"].get<std::string>();
    if (method == "ping")
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", json::object()}};
    return json{{"jsonrpc", "2.0"},
                {"id", id},
                {"error", {{"code", -32601}, {"message", "Method not found: " + method}}}};
}

// What a JSON-RPC message is, by its members (MCP basic, "Messages"): a
// request has a method and an id, a notification a method and no id, and a
// response an id, no method and a result or an error. A server request that
// reuses the id of a pending client request is still a request (MCP-C-06).
enum class MessageKind { request, notification, response, invalid };

MessageKind message_kind(const json& message) {
    if (!message.is_object()) return MessageKind::invalid;
    if (message.contains("method")) {
        if (!message["method"].is_string()) return MessageKind::invalid;
        return message.contains("id") ? MessageKind::request : MessageKind::notification;
    }
    if (message.contains("id") && (message.contains("result") || message.contains("error")))
        return MessageKind::response;
    return MessageKind::invalid;
}

}  // namespace

// One HTTP exchange's response, as it streams in.
struct HttpExchange {
    explicit HttpExchange(int request_id) : id(request_id) {}

    int id;
    int status = 0;
    bool event_stream = false;
    bool saw_event = false;
    std::string body;               ///< a body that is not an event stream
    std::optional<json> response;   ///< this request's JSON-RPC response
    async::SseEventParser events{sse_limits()};

    static async::SseParserOptions sse_limits() {
        async::SseParserOptions options;
        // One event carries one JSON-RPC message, as large as a JSON body.
        options.max_partial_line_bytes = 16u * 1024u * 1024u;
        options.max_event_bytes = 16u * 1024u * 1024u;
        options.max_pending_bytes = 32u * 1024u * 1024u;
        return options;
    }
};

class HttpSession final : public Transport {
public:
    HttpSession(std::string url, MCPClientConfig client_config)
      : server_url(std::move(url))
      , config(std::move(client_config))
    {
        // MCP transports, "Streamable HTTP": the server provides one MCP
        // endpoint URL, and the client sends every message to it as given,
        // path and query included. The client used to append "/mcp", so a
        // server mounted anywhere else was unreachable (MCP-C-04).
        if (server_url.find('#') != std::string::npos)
            throw std::invalid_argument("MCP server URL must not contain a fragment");
        const auto query = server_url.find('?');
        endpoint = async::split_async_endpoint(server_url.substr(0, query));
        if (endpoint.host.empty()) {
            throw std::invalid_argument("MCP server URL has no host");
        }
        target = endpoint.prefix.empty() ? std::string("/") : endpoint.prefix;
        if (query != std::string::npos) target += server_url.substr(query);
        if (target.find_first_of(" \r\n") != std::string::npos)
            throw std::invalid_argument("MCP server URL contains whitespace");
    }

    TransportCapabilities capabilities() const noexcept override {
        return {true, true, true};
    }
    asio::awaitable<json> exchange(
        json request, std::chrono::steady_clock::time_point deadline,
        std::shared_ptr<graph::CancelToken> cancel_token) override;
    asio::awaitable<json> do_exchange(
        json request, std::chrono::steady_clock::time_point deadline,
        std::shared_ptr<graph::CancelToken> cancel_token);
    asio::awaitable<json> exchange_and_resume(json request, int id,
                                              std::chrono::steady_clock::time_point until);
    asio::awaitable<void> stream_once(std::string method, std::string body, HeaderList headers,
                                      std::shared_ptr<HttpExchange> exchange,
                                      std::chrono::milliseconds timeout);
    asio::awaitable<std::pair<int, std::string>> send(std::string method, std::string body,
                                                      HeaderList headers,
                                                      std::chrono::milliseconds timeout);
    asio::awaitable<void> notify(json notification) override;
    asio::awaitable<void> do_notify(json notification);
    asio::awaitable<void> reply(json message);
    asio::awaitable<void> delete_session(std::string session);
    asio::awaitable<void> listen_loop();
    ~HttpSession() override { shutdown(); }
    void shutdown() noexcept override {
        end_session();
        worker.shutdown([this] {
            shutdown_token->cancel();
            if (listen_stop) listen_stop->cancel();
        });
    }
    void listen() noexcept override;
    void cancel_request(int id, std::string reason) noexcept override {
        try {
            worker.start([this, notification = cancelled_notification(id, std::move(reason))]()
                             mutable {
                asio::co_spawn(worker.io, best_effort(do_notify(std::move(notification))),
                               asio::detached);
            });
        } catch (...) {
            // A shut-down client has nobody left to tell.
        }
    }
    TransportWorker worker;
    std::shared_ptr<graph::CancelToken> shutdown_token =
        std::make_shared<graph::CancelToken>();
    void negotiated_version(const std::string& version) override {
        std::lock_guard lk(mu);
        protocol_version = version;
    }
    void reset() override {
        std::lock_guard lk(mu);
        session_id.clear();
        protocol_version.clear();
    }
    bool forget_session(const std::string& stale) override {
        std::lock_guard lk(mu);
        if (stale.empty() || session_id != stale) return false;
        session_id.clear();
        protocol_version.clear();
        return true;
    }

    /// The headers of a request to the endpoint; @p sent_session receives the
    /// session id it carries.
    HeaderList request_headers(const char* accept, const std::string* last_event_id,
                               std::string* sent_session);
    /// Route one JSON-RPC message that arrived on a response stream.
    void on_stream_message(HttpExchange& exchange, json message);
    /// End the server's session with DELETE, bounded and best effort.
    void end_session() noexcept;
    std::chrono::milliseconds remaining(std::chrono::steady_clock::time_point until) const;
    [[noreturn]] void throw_http_failure(int status, const std::string& body) const;

    /// The GET stream for server messages between calls; touched only on
    /// the worker's thread.
    bool listening = false;
    std::unique_ptr<asio::steady_timer> listen_stop;

    std::string server_url;
    async::AsyncEndpoint endpoint;
    std::string target;  ///< request target: the URL's path and query
    MCPClientConfig config;
    std::mutex mu;
    std::string session_id;
    std::string protocol_version;
};

class ClientMetadata {
public:
    enum class Lifecycle { created, initializing, initialized, shutdown };

    mutable std::mutex mu;
    std::condition_variable changed;
    Lifecycle lifecycle = Lifecycle::created;
    /// The name the first initialize sent; a new session sends it again.
    std::string client_name = "neograph";
    /// Called for notifications/tools/list_changed.
    std::function<void()> tool_list_changed;
    InitializeResult initialize_result;
    std::unordered_map<std::string, json> output_schemas;
};
class ProtocolSession {
public:
    explicit ProtocolSession(std::shared_ptr<Transport> transport)
      : transport_(std::move(transport)) {
        // The transport outlives nothing it calls into: it holds only the
        // metadata, which the session shares.
        std::weak_ptr<ClientMetadata> metadata = metadata_;
        transport_->set_notification_handler([metadata](const json& notification) {
            if (notification.value("method", "") != "notifications/tools/list_changed") return;
            auto current = metadata.lock();
            if (!current) return;
            std::function<void()> handler;
            {
                std::lock_guard lk(current->mu);
                // Cached outputSchemas may be stale until the list is read again.
                current->output_schemas.clear();
                handler = current->tool_list_changed;
            }
            if (handler) handler();
        });
    }

    void set_tool_list_changed_handler(std::function<void()> handler);
    /// Listen for server messages between calls when a handler wants them
    /// and the server declares tools.listChanged.
    void listen_if_wanted();

    json rpc_call(const std::string& method, const json& params = json::object());
    asio::awaitable<json> rpc_call_async(
        const std::string& method, const json& params,
        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::time_point::max(),
        std::shared_ptr<graph::CancelToken> cancel_token = {});
    bool initialize(const std::string& client_name = "neograph");
    asio::awaitable<bool> initialize_async(
        const std::string& client_name = "neograph",
        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::time_point::max(),
        std::shared_ptr<graph::CancelToken> cancel_token = {});
    /// Start a new session after the server forgot @p stale (MCP-C-03).
    asio::awaitable<void> restart_session(std::string stale);
    void shutdown() noexcept;
    bool is_initialized() const noexcept;
    InitializeResult get_initialize_result() const;
    ListToolsPage list_tools(const std::optional<std::string>& cursor);
    asio::awaitable<ListToolsPage> list_tools_async(
        const std::optional<std::string>& cursor);
    std::vector<ToolDefinition> get_tool_definitions();
    json call_tool(const std::string& name, const json& arguments);
    asio::awaitable<json> call_tool_async(
        const std::string& name, const json& arguments,
        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::time_point::max(),
        std::shared_ptr<graph::CancelToken> cancel_token = {});
    CallToolResult call_tool_result(const std::string& name, const json& arguments);
    asio::awaitable<CallToolResult> call_tool_result_async(
        const std::string& name, const json& arguments,
        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::time_point::max(),
        std::shared_ptr<graph::CancelToken> cancel_token = {});

    std::shared_ptr<Transport> transport_;
    std::shared_ptr<ClientMetadata> metadata_ = std::make_shared<ClientMetadata>();
    std::atomic<int> next_id_{0};
    std::atomic<bool> stopped_{false};
};


#ifdef _WIN32
using AsyncHandle  = asio::windows::stream_handle;
#else
using AsyncHandle  = asio::posix::stream_descriptor;
#endif

class StdioSession final : public Transport {
public:
    static std::shared_ptr<StdioSession> spawn(const std::vector<std::string>& argv);
    static std::shared_ptr<StdioSession> spawn(StdioClientConfig config);
    ~StdioSession();

    TransportCapabilities capabilities() const noexcept override {
        return {true, true, true};
    }
    asio::awaitable<json> exchange(
        json request, std::chrono::steady_clock::time_point deadline,
        std::shared_ptr<graph::CancelToken> cancel_token) override;
    asio::awaitable<void> notify(json notification) override;
    void cancel_request(int id, std::string reason) noexcept override;

    void shutdown() noexcept override;
    void reset() override { if (hardened_) shutdown(); }
private:
    StdioSession() = default;
    /// A hardened session ends at a cancellation or timeout: tell the server
    /// (unless the request was initialize, id < 0), give it cancel_grace_ to
    /// stop, then terminate the process tree.
    void terminate_after_cancel(int id, std::string reason) noexcept;
    asio::awaitable<void> cancel_then_terminate(int id, std::string reason);
    static std::shared_ptr<StdioSession> spawn_impl(StdioClientConfig config, bool hardened);
    void start_stderr();
    asio::awaitable<void> read_stderr();
    void close_io() noexcept;
    void check_open() const;

    /// The actual exchange runs on the session-owned io_context.
    asio::awaitable<json> do_exchange(json request);
    asio::awaitable<void> do_notify(json notification);

    asio::awaitable<std::string> async_read_line_locked(AsyncHandle& out);
    asio::awaitable<void> async_write_frame_locked(AsyncHandle& in, const json& j);

    /// One reader routes responses by id. It starts when a call is in
    /// flight, stops after draining all responses, and may remain suspended
    /// on the pipe after cancellation until a late response or shutdown.
    asio::awaitable<void> run_reader();

#ifdef _WIN32
    HANDLE process_ = nullptr;   ///< child process handle (CloseHandle on dtor)
    HANDLE job_ = nullptr;       ///< owns descendants even after the leader exits
    HANDLE stderr_h_ = nullptr;
    HANDLE stdin_h_ = nullptr;   ///< parent → child (write end of a pipe)
    HANDLE stdout_h_ = nullptr;  ///< child → parent (read end of a pipe)
#else
    pid_t pid_ = -1;
    int   stdin_fd_ = -1;   // parent → child
    int   stdout_fd_ = -1;  // child  → parent
    int   stderr_fd_ = -1;
#endif

    // Outlives all executor-bound handles and channels below.
    TransportWorker worker_;
    std::mutex process_mu_;
    void terminate_process() noexcept;
    bool hardened_ = false;
    /// Set when a hardened session started to end after a cancellation.
    std::atomic<bool> closing_{false};
    /// The server already had its grace period: terminate without waiting
    /// for it to exit on its own.
    std::atomic<bool> prompt_termination_{false};
    std::chrono::milliseconds cancel_grace_{500};
    /// The last max_stderr_bytes_ bytes the server wrote to stderr.
    std::size_t max_stderr_bytes_ = 64 * 1024;
    std::string stderr_buffer_;
    std::chrono::steady_clock::time_point startup_deadline_ =
        std::chrono::steady_clock::time_point::max();
    std::atomic<bool> startup_complete_{false};
    std::string abuffer_; ///< Pending bytes on the single async reader.
    std::size_t max_frame_bytes_ = 16 * 1024 * 1024;
    std::chrono::milliseconds request_timeout_{30000};
    // Capacity-one channel serializes only frame writes. A waiting writer
    // suspends cooperatively; it never blocks the transport's I/O thread.
    using AsyncLock = asio::experimental::channel<void(asio::error_code)>;
    std::unique_ptr<AsyncLock> async_lock_;
    std::mutex async_lock_init_mtx_;

    // Cache wrappers bound to the transport executor. Windows associates
    // the pipe FILE_OBJECT with an IOCP once, so duplicating a handle into a
    // different context on each call is invalid. Wrappers own duplicates
    // of the native handles; the session retains the originals.
    std::unique_ptr<AsyncHandle> async_in_;
    std::unique_ptr<AsyncHandle> async_out_;
    std::unique_ptr<AsyncHandle> async_err_;
    std::mutex async_handles_init_mtx_;

    // One reader fans responses out to id-keyed waiters; calls overlap
    // their reads rather than serializing entire round trips.
    using RespChan =
        asio::experimental::channel<void(asio::error_code,
                                         std::shared_ptr<json>)>;
    std::mutex demux_mu_; ///< guards waiters, reader state, and reader failure
    std::map<int, std::shared_ptr<RespChan>> waiters_;
    bool reader_running_ = false;
    std::exception_ptr reader_failure_;
};

std::shared_ptr<StdioSession> StdioSession::spawn(const std::vector<std::string>& argv) {
    StdioClientConfig config;
    config.argv = argv;
    return spawn_impl(std::move(config), false);
}

std::shared_ptr<StdioSession> StdioSession::spawn(StdioClientConfig config) {
    return spawn_impl(std::move(config), true);
}

#ifdef _WIN32

namespace {
[[noreturn]] void windows_error(const char* operation) {
    throw std::system_error(static_cast<int>(GetLastError()),
                            std::system_category(), operation);
}

std::wstring windows_utf8(const std::string& text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("MCP Windows launch string is too long");
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count) windows_error("MultiByteToWideChar");
    std::wstring result(count, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), count))
        windows_error("MultiByteToWideChar");
    return result;
}

// Match CommandLineToArgvW / the CRT: backslashes preceding quotes and the
// closing quote must be doubled; other backslashes are literal.
std::wstring build_win_cmdline(const std::vector<std::string>& argv) {
    std::wstring out;
    for (const auto& argument : argv) {
        if (!out.empty()) out.push_back(L' ');
        const auto arg = windows_utf8(argument);
        out.push_back(L'"');
        std::size_t slashes = 0;
        for (wchar_t ch : arg) {
            if (ch == L'\\') { ++slashes; continue; }
            out.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            slashes = 0;
            out.push_back(ch);
        }
        out.append(slashes * 2, L'\\');
        out.push_back(L'"');
    }
    return out;
}

std::wstring windows_executable(const std::string& argument) {
    auto name = windows_utf8(argument);
    DWORD count = SearchPathW(nullptr, name.c_str(), L".exe", 0, nullptr, nullptr);
    if (!count) windows_error("SearchPathW");
    std::wstring path(count, L'\0');
    DWORD length = SearchPathW(nullptr, name.c_str(), L".exe",
                               count, path.data(), nullptr);
    if (!length || length >= count) windows_error("SearchPathW");
    path.resize(length);
    return path;
}

std::vector<wchar_t> windows_environment(const StdioClientConfig& config) {
    std::vector<std::pair<std::wstring, std::wstring>> entries;
    for (const auto& [key, value] : config.environment)
        entries.emplace_back(windows_utf8(key), windows_utf8(value));
    const auto compare = [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                    b.c_str(), static_cast<int>(b.size()), TRUE);
    };
    const std::size_t configured = entries.size();
    if (!config.replace_environment) {
        // Inherit the parent's environment; a configured entry overrides the
        // inherited one of the same (case-insensitive) name.
        if (LPWCH inherited = GetEnvironmentStringsW()) {
            for (const wchar_t* entry = inherited; *entry; entry += std::wcslen(entry) + 1) {
                const std::wstring_view text(entry);
                // Names such as "=C:" start with '='.
                const auto separator = text.find(L'=', 1);
                if (separator == std::wstring_view::npos) continue;
                std::wstring name(text.substr(0, separator));
                bool overridden = false;
                for (std::size_t i = 0; i < entries.size() && !overridden; ++i)
                    overridden = compare(entries[i].first, name) == CSTR_EQUAL;
                if (!overridden)
                    entries.emplace_back(std::move(name), std::wstring(text.substr(separator + 1)));
            }
            FreeEnvironmentStringsW(inherited);
        }
    }
    std::vector<std::size_t> order(entries.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return compare(entries[a].first, entries[b].first) == CSTR_LESS_THAN;
    });
    std::vector<std::pair<std::wstring, std::wstring>> sorted;
    sorted.reserve(order.size());
    for (std::size_t index : order) {
        if (!sorted.empty() && compare(sorted.back().first, entries[index].first) == CSTR_EQUAL) {
            if (index < configured)
                throw std::invalid_argument("MCP Windows environment contains a duplicate key");
            continue;
        }
        sorted.push_back(std::move(entries[index]));
    }
    entries = std::move(sorted);
    std::vector<wchar_t> block;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& [key, value] = entries[i];
        block.insert(block.end(), key.begin(), key.end());
        block.push_back(L'=');
        block.insert(block.end(), value.begin(), value.end());
        block.push_back(L'\0');
    }
    if (block.empty()) block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

// Create a named-pipe pair where the parent side supports
// FILE_FLAG_OVERLAPPED (needed for asio::windows::stream_handle) and
// the child side is a plain inheritable handle. CreatePipe's anonymous
// pipes don't support overlapped I/O, hence the named-pipe dance.
struct PipePair {
    HANDLE parent = nullptr;
    HANDLE child = nullptr;
    PipePair() = default;
    PipePair(HANDLE p, HANDLE c) : parent(p), child(c) {}
    PipePair(const PipePair&) = delete;
    PipePair& operator=(const PipePair&) = delete;
    PipePair(PipePair&& other) noexcept
      : parent(std::exchange(other.parent, nullptr))
      , child(std::exchange(other.child, nullptr)) {}
    PipePair& operator=(PipePair&& other) noexcept {
        if (this != &other) {
            close();
            parent = std::exchange(other.parent, nullptr);
            child = std::exchange(other.child, nullptr);
        }
        return *this;
    }
    void close_child() noexcept {
        if (child) CloseHandle(std::exchange(child, nullptr));
    }
    void close() noexcept {
        if (parent) CloseHandle(std::exchange(parent, nullptr));
        close_child();
    }
    ~PipePair() { close(); }
};
PipePair make_overlapped_pipe(const char* name_prefix, bool parent_reads) {
    static std::atomic<uint64_t> counter{0};
    char name[128];
    std::snprintf(name, sizeof(name),
        "\\\\.\\pipe\\neograph_mcp_%s_%lu_%llu",
        name_prefix,
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long long>(counter.fetch_add(1)));

    DWORD parent_mode = parent_reads
        ? (PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED)
        : (PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED);
    HANDLE parent = CreateNamedPipeA(
        name, parent_mode,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        /*instances=*/1, /*outbuf=*/64*1024, /*inbuf=*/64*1024,
        /*timeout=*/0, /*sa=*/nullptr);
    if (parent == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(GetLastError()),
            std::system_category(), "CreateNamedPipe");
    }

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };  // inheritable
    DWORD child_access = parent_reads ? GENERIC_WRITE : GENERIC_READ;
    HANDLE child = CreateFileA(name, child_access,
        0, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (child == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        CloseHandle(parent);
        throw std::system_error(static_cast<int>(err),
            std::system_category(), "CreateFile(child side)");
    }

    // Parent side must NOT be inherited by the child.
    if (!SetHandleInformation(parent, HANDLE_FLAG_INHERIT, 0)) {
        DWORD err = GetLastError();
        CloseHandle(parent);
        CloseHandle(child);
        throw std::system_error(static_cast<int>(err),
            std::system_category(), "SetHandleInformation(parent)");
    }
    return PipePair{parent, child};
}
} // namespace

std::shared_ptr<StdioSession> StdioSession::spawn_impl(
    StdioClientConfig config, bool hardened) {
    validate_stdio_config(config);
    auto executable = windows_executable(config.argv.front());
    auto extension = std::filesystem::path(executable).extension().wstring();
    if (CompareStringOrdinal(extension.c_str(), -1, L".bat", -1, TRUE) == CSTR_EQUAL
        || CompareStringOrdinal(extension.c_str(), -1, L".cmd", -1, TRUE) == CSTR_EQUAL)
        throw std::invalid_argument("MCP stdio refuses implicit batch-file execution");
    auto cmdline = build_win_cmdline(config.argv);
    auto environment = windows_environment(config);
    auto sess = std::shared_ptr<StdioSession>(new StdioSession());
    sess->hardened_ = hardened;
    sess->max_frame_bytes_ = config.max_frame_bytes;
    sess->max_stderr_bytes_ = config.max_stderr_bytes;
    sess->cancel_grace_ = config.cancel_grace;
    sess->request_timeout_ = config.request_timeout;
    if (hardened)
        sess->startup_deadline_ = std::chrono::steady_clock::now() + config.startup_timeout;
    PipePair in_p = make_overlapped_pipe("in", false);
    PipePair out_p = make_overlapped_pipe("out", true);
    PipePair err_p;
    if (hardened) {
        err_p = make_overlapped_pipe("err", true);
    } else {
        HANDLE parent_error = GetStdHandle(STD_ERROR_HANDLE);
        if (parent_error && parent_error != INVALID_HANDLE_VALUE) {
            if (!DuplicateHandle(GetCurrentProcess(), parent_error, GetCurrentProcess(),
                                 &err_p.child, 0, TRUE, DUPLICATE_SAME_ACCESS))
                windows_error("DuplicateHandle(stderr)");
        } else {
            SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
            err_p.child = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      &attributes, OPEN_EXISTING, 0, nullptr);
            if (err_p.child == INVALID_HANDLE_VALUE) {
                err_p.child = nullptr;
                windows_error("CreateFileW(NUL)");
            }
        }
    }

    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<unsigned char> attribute_storage(attribute_size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size))
        windows_error("InitializeProcThreadAttributeList");
    struct AttributeGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~AttributeGuard() { DeleteProcThreadAttributeList(value); }
    } attribute_guard{attributes};
    HANDLE inherited[] = {in_p.child, out_p.child, err_p.child};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  inherited, sizeof(inherited), nullptr, nullptr))
        windows_error("UpdateProcThreadAttribute");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = in_p.child;
    startup.StartupInfo.hStdOutput = out_p.child;
    startup.StartupInfo.hStdError = err_p.child;
    startup.lpAttributeList = attributes;

    sess->job_ = CreateJobObjectW(nullptr, nullptr);
    if (!sess->job_) windows_error("CreateJobObjectW");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(sess->job_, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits)))
        windows_error("SetInformationJobObject");
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), cmdline.data(), nullptr, nullptr, TRUE,
                         CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                         config.replace_environment || !config.environment.empty()
                             ? environment.data() : nullptr,
                         config.cwd.empty() ? nullptr : config.cwd.c_str(),
                         &startup.StartupInfo, &process))
        windows_error("CreateProcessW");
    sess->process_ = process.hProcess;
    struct ThreadGuard {
        HANDLE value;
        ~ThreadGuard() { CloseHandle(value); }
    } thread_guard{process.hThread};
    // No child code can run (or spawn outside the job) before assignment.
    if (!AssignProcessToJobObject(sess->job_, sess->process_))
        windows_error("AssignProcessToJobObject");
    sess->stdin_h_ = std::exchange(in_p.parent, nullptr);
    sess->stdout_h_ = std::exchange(out_p.parent, nullptr);
    sess->stderr_h_ = std::exchange(err_p.parent, nullptr);
    in_p.close_child();
    out_p.close_child();
    err_p.close_child();
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1))
        windows_error("ResumeThread");
    if (hardened) sess->start_stderr();
    return sess;
}

StdioSession::~StdioSession() { shutdown(); }

void StdioSession::terminate_process() noexcept {
    std::lock_guard lk(process_mu_);
    // MCP lifecycle, "Shutdown" (stdio): close the server's input and wait
    // for it to exit before terminating the process tree (MCP-C-12).
    if (stdin_h_) CloseHandle(std::exchange(stdin_h_, nullptr));
    if (process_) {
        const auto grace = prompt_termination_.load()
            ? static_cast<DWORD>(kStdioPromptTermGrace.count())
            : static_cast<DWORD>((kStdioExitGrace + kStdioTermGrace).count());
        WaitForSingleObject(process_, grace);
    }
    if (job_) {
        TerminateJobObject(job_, 1);
        CloseHandle(std::exchange(job_, nullptr));
    }
    if (process_) {
        // Also covers a suspended child whose job assignment failed.
        TerminateProcess(process_, 1);
        WaitForSingleObject(process_, INFINITE);
        CloseHandle(std::exchange(process_, nullptr));
    }
    if (stdout_h_) CloseHandle(std::exchange(stdout_h_, nullptr));
    if (stderr_h_) CloseHandle(std::exchange(stderr_h_, nullptr));
}

#else  // !_WIN32

std::shared_ptr<StdioSession> StdioSession::spawn_impl(
    StdioClientConfig config, bool hardened) {
    validate_stdio_config(config);
    const auto& argv = config.argv;
    // After fork the child may inherit libc locks held by another thread, so
    // it only uses async-signal-safe syscalls before execve or _Exit.
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& arg : argv) cargv.push_back(const_cast<char*>(arg.c_str()));
    cargv.push_back(nullptr);
    std::vector<char*> shell_argv;
    shell_argv.reserve(argv.size() + 2);
    shell_argv.push_back(const_cast<char*>("sh"));
    shell_argv.push_back(nullptr); // Selected script path, set in the child.
    for (size_t i = 1; i < argv.size(); ++i) {
        shell_argv.push_back(const_cast<char*>(argv[i].c_str()));
    }
    shell_argv.push_back(nullptr);

    std::vector<std::string> candidates;
    if (argv[0].find('/') != std::string::npos) {
        candidates.push_back(argv[0]);
    } else {
        const char* env_path = config.replace_environment ? nullptr : std::getenv("PATH");
        for (const auto& [key, value] : config.environment)
            if (key == "PATH") env_path = value.c_str();
        const std::string path = env_path ? env_path : "/bin:/usr/bin";
        for (size_t start = 0; start <= path.size();) {
            const auto end = path.find(':', start);
            const auto directory = path.substr(
                start, end == std::string::npos ? end : end - start);
            candidates.push_back(directory.empty() ? argv[0]
                                                    : directory + "/" + argv[0]);
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }

#if defined(__APPLE__)
    char** const inherited_environment = *_NSGetEnviron();
#else
    char** const inherited_environment = ::environ;
#endif
    // The child gets its own environment when one is configured: the
    // configured entries alone (replace_environment), or the inherited
    // environment with the configured entries added or overriding it.
    const bool own_environment = config.replace_environment || !config.environment.empty();
    std::vector<std::string> child_environment;
    std::vector<char*> child_envp;
    if (own_environment) {
        if (!config.replace_environment && inherited_environment) {
            for (char** entry = inherited_environment; *entry; ++entry) {
                const std::string_view text(*entry);
                const auto name = text.substr(0, text.find('='));
                const bool overridden = std::any_of(
                    config.environment.begin(), config.environment.end(),
                    [&](const auto& item) { return item.first == name; });
                if (!overridden) child_environment.emplace_back(text);
            }
        }
        for (const auto& [key, value] : config.environment)
            child_environment.push_back(key + "=" + value);
        child_envp.reserve(child_environment.size() + 1);
        for (auto& item : child_environment) child_envp.push_back(item.data());
        child_envp.push_back(nullptr);
    }
    struct rlimit descriptor_limit {};
    if (::getrlimit(RLIMIT_NOFILE, &descriptor_limit) != 0)
        throw std::system_error(errno, std::generic_category(), "getrlimit()");
    const long open_max = ::sysconf(_SC_OPEN_MAX);
    if (descriptor_limit.rlim_max == RLIM_INFINITY && open_max <= 0)
        throw std::runtime_error("MCP descriptor limit is unavailable");
    const auto max_fd = std::min<rlim_t>(
        descriptor_limit.rlim_max == RLIM_INFINITY
            ? static_cast<rlim_t>(open_max) : descriptor_limit.rlim_max,
        static_cast<rlim_t>(std::numeric_limits<int>::max()));

    int in_pipe[2]  = {-1, -1};  // parent writes → child stdin
    int out_pipe[2] = {-1, -1};  // child stdout → parent reads
    int err_pipe[2] = {-1, -1};
    const auto startup_deadline = std::chrono::steady_clock::now() + config.startup_timeout;

    auto close_all = [&]() {
        for (int* fd : {&in_pipe[0], &in_pipe[1], &out_pipe[0], &out_pipe[1],
                       &err_pipe[0], &err_pipe[1]}) {
            if (*fd >= 0) { ::close(*fd); *fd = -1; }
        }
    };

    if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0
        || (hardened && ::pipe(err_pipe) != 0)) {
        const int error = errno;
        close_all();
        throw std::system_error(error, std::generic_category(), "pipe()");
    }

    char* const* environment = own_environment ? child_envp.data() : inherited_environment;

    pid_t pid = ::fork();
    if (pid < 0) {
        const int error = errno;
        close_all();
        throw std::system_error(error, std::generic_category(), "fork()");
    }

    if (pid == 0) {
        if (::setpgid(0, 0) != 0) std::_Exit(127);
        // --- child ---
        if (::dup2(in_pipe[0], STDIN_FILENO) < 0
            || ::dup2(out_pipe[1], STDOUT_FILENO) < 0) {
            std::_Exit(127);
        }
        if (hardened && ::dup2(err_pipe[1], STDERR_FILENO) < 0) std::_Exit(127);
        if (!config.cwd.empty() && ::chdir(config.cwd.c_str()) != 0) std::_Exit(126);
        const int pipes[] = {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1],
                             err_pipe[0], err_pipe[1]};
        for (int fd : pipes)
            if (fd > STDERR_FILENO) ::close(fd);
#if defined(__linux__) && defined(SYS_close_range)
        if (::syscall(SYS_close_range, 3u, ~0u, 0u) != 0) {
#endif
            for (rlim_t fd = 3; fd < max_fd; ++fd) ::close(static_cast<int>(fd));
#if defined(__linux__) && defined(SYS_close_range)
        }
#endif
        for (const auto& candidate : candidates) {
            ::execve(candidate.c_str(), cargv.data(), environment);
            const int error = errno;
            if (error == ENOEXEC && !hardened) {
                shell_argv[1] = const_cast<char*>(candidate.c_str());
                ::execve("/bin/sh", shell_argv.data(), environment);
                break;
            }
            if (error == EACCES || error == ENOENT || error == ENOTDIR) continue;
            break;
        }
        constexpr char message[] = "MCP stdio exec failed\n";
        // Best effort: exit status 127 reports the failure if stderr is unusable.
        [[maybe_unused]] const auto written =
            ::write(STDERR_FILENO, message, sizeof(message) - 1);
        std::_Exit(127);
    }

    ::setpgid(pid, pid);
    // --- parent ---
    ::close(in_pipe[0]);
    in_pipe[0] = -1;
    ::close(out_pipe[1]);
    out_pipe[1] = -1;
    if (err_pipe[1] >= 0) ::close(std::exchange(err_pipe[1], -1));

    try {
        auto sess = std::shared_ptr<StdioSession>(new StdioSession());
        sess->hardened_ = hardened;
        sess->max_frame_bytes_ = config.max_frame_bytes;
        sess->max_stderr_bytes_ = config.max_stderr_bytes;
        sess->cancel_grace_ = config.cancel_grace;
        sess->request_timeout_ = config.request_timeout;
        if (hardened) sess->startup_deadline_ = startup_deadline;
        sess->pid_ = std::exchange(pid, -1);
        sess->stdin_fd_ = std::exchange(in_pipe[1], -1);
        sess->stdout_fd_ = std::exchange(out_pipe[0], -1);
        sess->stderr_fd_ = std::exchange(err_pipe[0], -1);
        if (hardened) sess->start_stderr();
        return sess;
    } catch (...) {
        close_all();
        if (pid > 0) {
            ::kill(-pid, SIGKILL);
            ::kill(pid, SIGKILL);
            int status;
            while (::waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
        }
        throw;
    }
}

StdioSession::~StdioSession() { shutdown(); }

namespace {
// True once the child @p pid exited (and was reaped) within @p wait.
bool exited_within(pid_t pid, std::chrono::milliseconds wait) {
    const auto until = std::chrono::steady_clock::now() + wait;
    for (;;) {
        int status = 0;
        const pid_t reaped = ::waitpid(pid, &status, WNOHANG);
        if (reaped == pid || (reaped < 0 && errno == ECHILD)) return true;
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
}  // namespace

void StdioSession::terminate_process() noexcept {
    std::lock_guard lk(process_mu_);
    // MCP lifecycle, "Shutdown" (stdio): close the server's input, wait for
    // it to exit, then SIGTERM, then SIGKILL. The client used to send SIGTERM
    // and SIGKILL 20 ms apart without closing stdin first (MCP-C-12). The
    // reader's duplicate of stdin is closed by close_io() before this runs.
    if (stdin_fd_ >= 0) ::close(std::exchange(stdin_fd_, -1));
    if (pid_ > 0) {
        const pid_t group = pid_;
        const bool prompt = prompt_termination_.load();
        bool reaped = exited_within(pid_, prompt ? std::chrono::milliseconds(0) : kStdioExitGrace);
        if (!reaped) {
            ::kill(-group, SIGTERM);
            ::kill(pid_, SIGTERM);
            reaped = exited_within(pid_, prompt ? kStdioPromptTermGrace : kStdioTermGrace);
        }
        // The leader can already be dead while a TERM-ignoring descendant
        // still owns this group. Do not return early after reaping the leader.
        ::kill(-group, SIGKILL);
        if (!reaped) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
        }
        pid_ = -1;
    }
    if (stdout_fd_ >= 0) ::close(std::exchange(stdout_fd_, -1));
    if (stderr_fd_ >= 0) ::close(std::exchange(stderr_fd_, -1));
}
#endif  // _WIN32

void StdioSession::check_open() const {
    if (worker_.stopped.load() || closing_.load()) throw_shutdown();
}

void StdioSession::close_io() noexcept {
    asio::error_code ec;
    // The reader stays suspended on the server's stdout between calls, and
    // shutdown() waits for the I/O thread to run out of work before it stops
    // the server. Closing a Windows handle does not cancel a read pending on
    // it while another handle to the same pipe end is open, and the session
    // keeps the original of each duplicate the wrappers own; so every
    // pending operation is cancelled first (POSIX closes them either way).
    const auto cancel_and_close = [&ec](AsyncHandle& handle) {
        handle.cancel(ec);
        handle.close(ec);
    };
    if (async_in_) cancel_and_close(*async_in_);
    if (async_out_) cancel_and_close(*async_out_);
    if (async_err_) cancel_and_close(*async_err_);
    if (async_lock_) {
        async_lock_->cancel();
        async_lock_->close();
    }
    for (auto& [id, channel] : waiters_) {
        channel->cancel();
        channel->close();
    }
    // Keep wrappers alive until suspended writes/reads have unwound.
}

void StdioSession::shutdown() noexcept {
    worker_.shutdown([this] { close_io(); });
    terminate_process();
}

void StdioSession::start_stderr() {
#ifdef _WIN32
    async_err_ = std::make_unique<AsyncHandle>(worker_.io, stderr_h_);
    stderr_h_ = nullptr;
#else
    async_err_ = std::make_unique<AsyncHandle>(worker_.io, stderr_fd_);
    stderr_fd_ = -1;
#endif
    worker_.start([this] {
        asio::co_spawn(worker_.io, read_stderr(), asio::detached);
    });
}

asio::awaitable<void> StdioSession::read_stderr() {
    // MCP transports, stdio: the client MAY capture or ignore the server's
    // stderr and SHOULD NOT treat it as an error. The session keeps draining
    // it and keeps its last max_stderr_bytes_ bytes; it used to end the
    // session once the total passed that limit, so a server that logs a line
    // per request died after about 1,500 calls (MCP-C-05, D8).
    std::array<char, 4096> bytes;
    try {
        for (;;) {
            const auto count = co_await async_err_->async_read_some(
                asio::buffer(bytes.data(), bytes.size()), asio::use_awaitable);
            stderr_buffer_.append(bytes.data(), count);
            if (stderr_buffer_.size() > max_stderr_bytes_)
                stderr_buffer_.erase(0, stderr_buffer_.size() - max_stderr_bytes_);
        }
    } catch (const std::system_error&) {
        // EOF, or the session closed the pipe: stderr says nothing about the
        // session's health.
    }
}

asio::awaitable<void>
StdioSession::async_write_frame_locked(AsyncHandle& in,
                                       const json& j) {
    std::string line = j.dump();
    line.push_back('\n');
    co_await asio::async_write(in, asio::buffer(line), asio::use_awaitable);
}

asio::awaitable<std::string>
StdioSession::async_read_line_locked(AsyncHandle& out) {
    auto nl = abuffer_.find('\n');
    if (nl == std::string::npos) {
        asio::streambuf sbuf(max_frame_bytes_);
        // Seed asio's streambuf with whatever we already have so
        // async_read_until doesn't re-read those bytes.
        if (!abuffer_.empty()) {
            std::ostream os(&sbuf);
            os.write(abuffer_.data(), static_cast<std::streamsize>(abuffer_.size()));
            abuffer_.clear();
        }
        std::size_t n = 0;
        try {
            n = co_await asio::async_read_until(
                out, sbuf, '\n', asio::use_awaitable);
        } catch (const std::system_error& e) {
            if (e.code() == asio::error::not_found) {
                throw MCPTransportError(
                    MCPFailure::protocol, "MCP stdio frame exceeds configured limit");
            }
            throw;
        }
        // Re-merge into our string buffer so the rest of the trailing
        // bytes (after the newline) are kept for the next call.
        std::string drained(asio::buffers_begin(sbuf.data()),
                            asio::buffers_begin(sbuf.data()) + sbuf.size());
        abuffer_.append(drained);
        nl = abuffer_.find('\n');
        if (nl == std::string::npos) {
            // async_read_until promised a delim was found within `n`
            // bytes; this branch is defensive.
            throw MCPTransportError(MCPFailure::protocol,
                "MCP stdio delimiter missing after "
                + std::to_string(n) + " bytes");
        }
    }
    std::string line = abuffer_.substr(0, nl);
    abuffer_.erase(0, nl + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    co_return line;
}

asio::awaitable<void> StdioSession::run_reader() {
    std::exception_ptr fail;
    try {
        for (;;) {
            std::string line = co_await async_read_line_locked(*async_out_);
            if (!line.empty()) {
                json resp;
                try {
                    resp = json::parse(line);
                } catch (const json::parse_error& e) {
                    throw MCPTransportError(
                        MCPFailure::protocol,
                        std::string("MCP stdio response JSON: ") + e.what());
                }
                if (!resp.is_object() && !resp.is_array()) {
                    throw MCPTransportError(MCPFailure::protocol,
                                            "MCP stdio frame must be a JSON object or a batch");
                }
                // A response completes its call, a notification reaches its
                // handler; unknown and late ids are dropped.
                const auto route = [this](json message) {
                    const auto kind = message_kind(message);
                    if (kind == MessageKind::response && message["id"].is_number_integer()) {
                        const int rid = message["id"].get<int>();
                        std::shared_ptr<RespChan> chan;
                        {
                            std::lock_guard<std::mutex> lk(demux_mu_);
                            auto it = waiters_.find(rid);
                            if (it != waiters_.end()) {
                                chan = it->second;
                                waiters_.erase(it);
                            }
                        }
                        // Capacity-1 sink is empty, so this send always lands
                        // for the matched id.
                        if (chan) {
                            auto p = std::make_shared<json>(std::move(message));
                            chan->try_send(asio::error_code{}, p);
                        }
                    } else if (kind == MessageKind::notification) {
                        deliver_notification(message);
                    }
                };
                if (resp.is_array()) {
                    // MCP 2025-03-26 lets a peer send a JSON-RPC batch. Each
                    // message is handled as if it came alone, and the replies
                    // to the requests in it go back as one array.
                    json replies = json::array();
                    for (std::size_t i = 0; i < resp.size(); ++i) {
                        json element = resp[i];
                        if (message_kind(element) == MessageKind::request)
                            replies.push_back(server_request_reply(element));
                        else
                            route(std::move(element));
                    }
                    if (!replies.empty()) {
                        asio::co_spawn(worker_.io, best_effort(do_notify(std::move(replies))),
                                       asio::detached);
                    }
                } else if (message_kind(resp) == MessageKind::request) {
                    // A server request, such as ping: answered at once, also
                    // between calls, and never taken for the response of a
                    // pending request with the same id (MCP-C-02, MCP-C-06).
                    asio::co_spawn(worker_.io,
                                   best_effort(do_notify(server_request_reply(resp))),
                                   asio::detached);
                } else {
                    route(std::move(resp));
                }
            }
            // The reader keeps reading between calls, on the session's own
            // io_context, so a server request is answered whenever it comes.
        }
    } catch (const std::system_error& error) {
        fail = std::make_exception_ptr(MCPTransportError(
            worker_.stopped.load() ? MCPFailure::shutdown : MCPFailure::connection,
            std::string("MCP stdio read failed: ") + error.what()));
    } catch (const std::exception&) {
        // Fail all callers on EOF, malformed frames, or broken pipes.
        fail = std::current_exception();
    }

    if (fail) {
        std::map<int, std::shared_ptr<RespChan>> remaining;
        {
            std::lock_guard<std::mutex> lk(demux_mu_);
            if (!reader_failure_) reader_failure_ = fail;
            remaining.swap(waiters_);
            reader_running_ = false;
        }
        for (auto& kv : remaining) {
            // Wake a pending receive explicitly before closing the sink.
            // Some Asio versions do not cancel a receive solely from close().
            kv.second->try_send(
                asio::error_code(asio::error::operation_aborted, asio::system_category()),
                std::shared_ptr<json>{});
            kv.second->close();
        }
        if (hardened_ && !worker_.stopped.load() && !closing_.load()) shutdown();
    }
    co_return;
}

// Public entry: hop onto the session's OWN io_context and do the exchange
// there.
//
// Everything asio in this session — the pipe descriptors, the write lock, the
// reader coroutine — is bound to an executor on first use, and used again on
// every later call. Binding that to the caller's executor was the bug: the graph
// engine's `run_sync` stands up an io_context for one call and destroys it on
// the way out, so the second run (or the client's destructor) touched asio state
// hanging off a destroyed io_context. In C++, with no sanitizer, that is a core
// dump; see MCPStdioInGraph.TheSameClientSurvivesASecondRun.
//
// The session owns the io_context now, so its lifetime is the session's, and the
// old precondition — "callers must ensure the io_context outlives the session" —
// is gone rather than merely documented. It was never a precondition an engine
// could honour.
asio::awaitable<json> StdioSession::exchange(
    json request, std::chrono::steady_clock::time_point deadline,
    std::shared_ptr<graph::CancelToken> cancel_token) {
    check_open();
    deadline = std::min(deadline, std::chrono::steady_clock::now() + request_timeout_);
    if (hardened_ && !startup_complete_.load())
        deadline = std::min(deadline, startup_deadline_);
    const int id = request.at("id").get<int>();
    const bool initialize = request.value("method", "") == "initialize";
    using asio::experimental::awaitable_operators::operator||;
    try {
        if ((cancel_token && cancel_token->is_cancelled())
            || deadline <= std::chrono::steady_clock::now())
            throw_rpc_bound(deadline, cancel_token);
        std::variant<CapturedResult<json>, std::monostate> result;
        try {
            result = co_await (
                capture_result(worker_.submit(do_exchange(std::move(request))))
                || wait_for_rpc_bound(deadline, cancel_token));
        } catch (const asio::multiple_exceptions& both) {
            // The caller cancelled the coroutine awaiting this call, and both
            // waits ended with it: the first failure names it.
            std::rethrow_exception(both.first_exception());
        }
        if (result.index() == 1) throw_rpc_bound(deadline, cancel_token);
        auto captured = std::get<0>(std::move(result));
        if (captured.error) std::rethrow_exception(captured.error);
        check_open();
        co_return std::move(*captured.value);
    } catch (const std::system_error& e) {
        const bool stopped = worker_.stopped.load() || closing_.load();
        // An aborted operation of a session still running was cancelled by
        // its caller (a cancelled run cancels the coroutine awaiting it).
        const auto failure = stopped ? MCPFailure::shutdown
            : aborted_by_caller(e.code()) ? MCPFailure::cancelled
                                          : MCPFailure::connection;
        if (hardened_ && failure == MCPFailure::cancelled)
            terminate_after_cancel(initialize ? -1 : id, "cancelled by the caller");
        else if (hardened_)
            shutdown();
        throw MCPTransportError(failure, std::string("MCP stdio transport: ") + e.what());
    } catch (const MCPTransportError& e) {
        if (hardened_ && (e.failure() == MCPFailure::cancelled ||
                          e.failure() == MCPFailure::timeout)) {
            // The owner's decision for hardened sessions: notifications/
            // cancelled first, a short grace period, then termination (it
            // used to kill the server at once and tell it nothing).
            terminate_after_cancel(initialize ? -1 : id,
                                   e.failure() == MCPFailure::cancelled ? "cancelled by the caller"
                                                                        : "request timed out");
        } else if (hardened_) {
            shutdown();
        }
        throw;
    } catch (...) {
        if (hardened_) shutdown();
        throw;
    }
}

void StdioSession::terminate_after_cancel(int id, std::string reason) noexcept {
    if (closing_.exchange(true)) return;
    try {
        worker_.start([this, id, reason = std::move(reason)]() mutable {
            asio::co_spawn(worker_.io, cancel_then_terminate(id, std::move(reason)),
                           asio::detached);
        });
    } catch (...) {
        shutdown();
    }
}

asio::awaitable<void> StdioSession::cancel_then_terminate(int id, std::string reason) {
    using asio::experimental::awaitable_operators::operator||;
    try {
        asio::steady_timer grace(worker_.io);
        grace.expires_after(cancel_grace_);
        if (id >= 0 && async_lock_ && async_in_) {
            co_await (best_effort(do_notify(cancelled_notification(id, std::move(reason))))
                      || grace.async_wait(asio::use_awaitable));
        }
        co_await grace.async_wait(asio::use_awaitable);
    } catch (...) {
        // Shut down by its owner meanwhile.
    }
    prompt_termination_.store(true);
    shutdown();
}

asio::awaitable<json> StdioSession::do_exchange(json request) {
    check_open();
    const int id = request.at("id").get<int>();
    auto ex = co_await asio::this_coro::executor;

    // Lazy-init the WRITE lock on first call (capacity-1 channel = binary
    // semaphore). Unlike the pre-demux design it is NOT held across the
    // round trip — only around the frame write below — so concurrent
    // calls overlap their reads. std::mutex guards only the one-time init.
    {
        std::lock_guard<std::mutex> g(async_lock_init_mtx_);
        if (!async_lock_) {
            async_lock_ = std::make_unique<AsyncLock>(ex, 1);
            // Seed with the initial token so the first writer takes it
            // without blocking.
            async_lock_->try_send(asio::error_code{});
        }
    }

    // Cached AsyncHandle wrappers. See member-field comments for why
    // we bind once per session instead of per call. Lazy-init under a
    // mutex so concurrent first-callers on the same session don't
    // double-bind; subsequent calls take the fast path with no lock.
    if (!async_in_ || !async_out_) {
        std::lock_guard<std::mutex> g(async_handles_init_mtx_);
        if (!async_in_ || !async_out_) {
#ifdef _WIN32
            HANDLE dup_in = nullptr, dup_out = nullptr;
            const HANDLE self = GetCurrentProcess();
            if (!DuplicateHandle(self, stdin_h_, self, &dup_in,
                                 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                throw std::system_error(static_cast<int>(GetLastError()),
                    std::system_category(), "DuplicateHandle(stdin)");
            }
            if (!DuplicateHandle(self, stdout_h_, self, &dup_out,
                                 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                DWORD err = GetLastError();
                CloseHandle(dup_in);
                throw std::system_error(static_cast<int>(err),
                    std::system_category(), "DuplicateHandle(stdout)");
            }
            try {
                async_in_  = std::make_unique<AsyncHandle>(ex, dup_in);
                async_out_ = std::make_unique<AsyncHandle>(ex, dup_out);
            } catch (...) {
                // On partial success, close whichever dup the failed
                // wrapper didn't take. Wrappers that succeeded already
                // own their dup and will close on reset/destruction.
                if (!async_in_)  CloseHandle(dup_in);
                if (!async_out_) CloseHandle(dup_out);
                async_in_.reset();
                async_out_.reset();
                throw;
            }
#else
            int dup_in  = ::dup(stdin_fd_);
            if (dup_in < 0) {
                throw std::system_error(errno, std::system_category(),
                    "dup(stdin_fd)");
            }
            int dup_out = ::dup(stdout_fd_);
            if (dup_out < 0) {
                int err = errno;
                ::close(dup_in);
                throw std::system_error(err, std::system_category(),
                    "dup(stdout_fd)");
            }
            try {
                async_in_  = std::make_unique<AsyncHandle>(ex, dup_in);
                async_out_ = std::make_unique<AsyncHandle>(ex, dup_out);
            } catch (...) {
                if (!async_in_)  ::close(dup_in);
                if (!async_out_) ::close(dup_out);
                async_in_.reset();
                async_out_.reset();
                throw;
            }
#endif
        }
    }

    // Register this call's response sink and make sure the single demux
    // reader is running. Registration happens AFTER the handles are
    // bound above, so the reader never dereferences a null async_out_.
    auto chan = std::make_shared<RespChan>(ex, 1);
    bool start_reader = false;
    {
        std::lock_guard<std::mutex> lk(demux_mu_);
        waiters_[id] = chan;
        if (!reader_running_) {
            reader_failure_ = nullptr;
            reader_running_ = true;
            start_reader = true;
        }
    }
    if (start_reader) {
        asio::co_spawn(ex, run_reader(), asio::detached);
    }

    // Drop our waiter on every exit path (delivered, threw, cancelled).
    // Erasing an already-served id is a harmless no-op.
    struct WaiterGuard {
        StdioSession* self;
        int           id;
        ~WaiterGuard() {
            std::lock_guard<std::mutex> lk(self->demux_mu_);
            self->waiters_.erase(id);
        }
    } wguard{this, id};

    // Serialise ONLY the frame write (held microseconds), releasing the
    // write lock before we await the response.
    co_await async_lock_->async_receive(asio::use_awaitable);
    {
        struct WriteReleaser {
            AsyncLock* ch;
            ~WriteReleaser() {
                try { ch->try_send(asio::error_code{}); } catch (...) {}
            }
        } wrel{async_lock_.get()};
        co_await async_write_frame_locked(*async_in_, request);
    }

    // Await our response. use_awaitable turns a closed sink (session torn
    // down / server gone) into a thrown system_error.
    std::shared_ptr<json> respptr;
    try {
        respptr = co_await chan->async_receive(asio::use_awaitable);
    } catch (const std::system_error&) {
        std::exception_ptr fail;
        {
            std::lock_guard<std::mutex> lk(demux_mu_);
            fail = reader_failure_;
        }
        if (fail) std::rethrow_exception(fail);
        throw;
    }

    json response = std::move(*respptr);
    co_return response;
}

asio::awaitable<void> StdioSession::notify(json notification) {
    check_open();
    using asio::experimental::awaitable_operators::operator||;
    const auto deadline = hardened_ && !startup_complete_.load()
        ? std::min(startup_deadline_, std::chrono::steady_clock::now() + request_timeout_)
        : std::chrono::steady_clock::now() + request_timeout_;
    try {
        auto result = co_await (
            capture_result(worker_.submit(notification_result(do_notify(std::move(notification)))))
            || wait_for_rpc_bound(deadline, {}));
        if (result.index() == 1) throw_rpc_bound(deadline, {});
        auto captured = std::get<0>(std::move(result));
        if (captured.error) std::rethrow_exception(captured.error);
        check_open();
        startup_complete_.store(true);
    } catch (const std::system_error& e) {
        const bool stopped = worker_.stopped.load();
        if (hardened_) shutdown();
        throw MCPTransportError(stopped ? MCPFailure::shutdown : MCPFailure::connection,
                                std::string("MCP stdio notification: ") + e.what());
    } catch (...) {
        if (hardened_) shutdown();
        throw;
    }
}

void StdioSession::cancel_request(int id, std::string reason) noexcept {
    // A hardened session told the server already, on its way to terminate.
    if (hardened_) return;
    try {
        worker_.start([this, notification = cancelled_notification(id, std::move(reason))]()
                          mutable {
            // The handles and the write lock exist once a request was sent.
            if (!async_lock_ || !async_in_) return;
            asio::co_spawn(worker_.io, best_effort(do_notify(std::move(notification))),
                           asio::detached);
        });
    } catch (...) {
        // A shut-down session (a hardened one ends at cancellation) has
        // nobody left to tell.
    }
}

asio::awaitable<void> StdioSession::do_notify(json notification) {
    if (worker_.stopped.load()) throw_shutdown();
    // initialize's request created the handles and write semaphore. Notifications
    // use that SAME write lock, so an adjacent tool call cannot interleave frames.
    co_await async_lock_->async_receive(asio::use_awaitable);
    struct Release {
        AsyncLock* lock;
        ~Release() { lock->try_send(asio::error_code{}); }
    } release{async_lock_.get()};
    co_await async_write_frame_locked(*async_in_, notification);
}

} // namespace detail

// ===========================================================================
// Shared helper — shape an MCP tools/call result into a string for the LLM.
// ===========================================================================
namespace {
// structuredContent is checked with the JSON Schema 2020-12 validator the
// server uses (MCP-C-16). A schema this client cannot check, such as another
// dialect, is no reason to fail a successful call: MCP asks implementations to
// handle unsupported dialects gracefully, and the check is a SHOULD.
void validate_tool_result(const CallToolResult& result,
                          const json& output_schema) {
    // outputSchema describes successful results. An error result may omit
    // structuredContent, and its content carries the tool's error message.
    if (output_schema.is_null() || result.is_error) return;
    try {
        validate_json_schema(output_schema, "outputSchema");
    } catch (const std::invalid_argument&) {
        return;
    }
    if (result.structured_content.is_null()) {
        throw std::runtime_error(
            "MCP tool advertised outputSchema but returned no structuredContent");
    }
    try {
        validate_json_value(result.structured_content, output_schema,
                            "MCP structuredContent", "$");
    } catch (const std::invalid_argument& error) {
        // The server's result is wrong, not this program: an ordinary tool
        // failure rather than a logic error.
        throw std::runtime_error(error.what());
    }
}

std::string format_tool_result(const CallToolResult& result) {
    if (result.content.is_array()) {
        std::string output;
        for (const auto& item : result.content) {
            if (item.value("type", "") == "text") {
                if (!output.empty()) output += "\n";
                output += item.value("text", "");
            }
        }
        if (!output.empty()) return output;
    }
    if (!result.structured_content.is_null()) {
        return result.structured_content.dump();
    }
    if (!result.content.empty()) return result.content.dump();
    return result.raw.dump();
}

ToolDefinition legacy_definition(const std::string& name,
                                 const std::string& description,
                                 const json& input_schema) {
    ToolDefinition definition;
    definition.name = name;
    definition.description = description;
    definition.input_schema = input_schema;
    definition.raw = {
        {"name", name},
        {"description", description},
        {"inputSchema", input_schema},
    };
    return definition;
}
} // namespace

// ===========================================================================
// MCPTool
// ===========================================================================

MCPTool::MCPTool(const std::string& server_url,
                 const std::string& name,
                 const std::string& description,
                 const json& input_schema)
  : server_url_(server_url)
  , definition_(legacy_definition(name, description, input_schema))
{
}

MCPTool::MCPTool(std::shared_ptr<detail::ProtocolSession> session,
                 ToolDefinition definition)
  : session_(std::move(session))
  , definition_(std::move(definition))
{
}

std::shared_ptr<detail::ProtocolSession> MCPTool::protocol_session() {
    // The URL constructor creates its protocol session on first use and keeps
    // it, so every call of this tool shares one MCP session, ended with DELETE
    // when the tool goes away. It used to open a new session for every call
    // and never end one, which exhausts a server's session limit (MCP-C-08).
    static std::mutex lazy_session;
    std::lock_guard lock(lazy_session);
    if (!session_) {
        session_ = std::make_shared<detail::ProtocolSession>(
            std::make_shared<detail::HttpSession>(server_url_, MCPClientConfig{}));
    }
    return session_;
}

ChatTool MCPTool::get_definition() const {
    return { definition_.name, definition_.description,
             definition_.input_schema };
}

CallToolResult MCPTool::execute_result(const json& arguments) {
    return async::run_sync(execute_result_async(arguments));
}

asio::awaitable<CallToolResult>
MCPTool::execute_result_async(const json& arguments) {
    co_return co_await execute_result_async(arguments, ToolExecutionContext{});
}

asio::awaitable<CallToolResult>
MCPTool::execute_result_async(const json& arguments, ToolExecutionContext context) {
    // The call's own bounds: the run's deadline and cancel token (a node
    // attempt's token, under an attempt timeout). Cancelling stops waiting for
    // the response; a late one is dropped (stdio routes responses by id, HTTP
    // abandons the request).
    const auto deadline = context.deadline.value_or(std::chrono::steady_clock::time_point::max());
    std::exception_ptr bounded;
    try {
        auto session = protocol_session();
        auto result = co_await session->call_tool_result_async(
            definition_.name, arguments, deadline, context.cancel_token);
        validate_tool_result(result, definition_.output_schema);
        co_return result;
    } catch (const MCPTransportError& error) {
        if (error.failure() != MCPFailure::cancelled && error.failure() != MCPFailure::timeout)
            throw;
        bounded = std::current_exception();
    } catch (const MCPError& error) {
        // A remote run control signal (reference, "Protocol failure mapping"):
        // the server's Hook blocked the call (-32010), or its run needs input
        // (-32011). It belongs to the remote run: an ordinary failure with the
        // remote code, which neither stops nor pauses this run.
        if (error.code() != -32010 && error.code() != -32011) throw;
        constexpr std::string_view prefix = "MCP JSON-RPC error: ";
        std::string_view message = error.what();
        if (message.substr(0, prefix.size()) == prefix) message.remove_prefix(prefix.size());
        throw graph::RemoteControlFailure(error.code() == -32010 ? "NG.HookBlocked"
                                                                 : "NG.Interrupted",
                                          std::string(message));
    }
    // A bounded call ends as what bounded it, the same as for any other tool
    // (reference, "Tool failure contract"): cancellation is
    // graph::CancelledException, and a deadline or request timeout is a
    // timed-out call, not an ordinary tool error.
    try {
        std::rethrow_exception(bounded);
    } catch (const MCPTransportError& error) {
        if (error.failure() == MCPFailure::cancelled) throw graph::CancelledException(error.what());
        throw asio::system_error(asio::error::timed_out, error.what());
    }
}

asio::awaitable<std::string> MCPTool::execute_async(const json& arguments) {
    co_return co_await execute_async(arguments, ToolExecutionContext{});
}

asio::awaitable<std::string> MCPTool::execute_async(const json& arguments,
                                                    ToolExecutionContext context) {
    auto result = co_await execute_result_async(arguments, std::move(context));
    std::string text = format_tool_result(result);
    if (result.is_error) {
        throw std::runtime_error("MCP tool execution error: " + text);
    }
    co_return text;
}

// The public client is only a facade over one protocol session. Tools retain
// that same session, so neither adaptation nor graph dispatch knows the
// transport type.
MCPClient::MCPClient(const std::string& server_url)
  : MCPClient(server_url, MCPClientConfig{}) {}

MCPClient::MCPClient(const std::string& server_url, MCPClientConfig config)
  : session_(std::make_shared<detail::ProtocolSession>(
        std::make_shared<detail::HttpSession>(server_url, std::move(config)))) {}

MCPClient::MCPClient(std::vector<std::string> argv)
  : session_(std::make_shared<detail::ProtocolSession>(
        detail::StdioSession::spawn(argv))) {}
MCPClient::MCPClient(StdioClientConfig config)
  : session_(std::make_shared<detail::ProtocolSession>(
        detail::StdioSession::spawn(std::move(config)))) {}

void MCPClient::shutdown() noexcept { session_->shutdown(); }

void detail::ProtocolSession::shutdown() noexcept {
    stopped_.store(true);
    {
        std::lock_guard lk(metadata_->mu);
        metadata_->lifecycle = ClientMetadata::Lifecycle::shutdown;
    }
    metadata_->changed.notify_all();
    transport_->shutdown();
}


asio::awaitable<json> MCPClient::rpc_call_async(
    const std::string& method, const json& params,
    std::chrono::steady_clock::time_point deadline,
    std::shared_ptr<graph::CancelToken> cancel_token) {
    auto session = session_;
    // MCP lifecycle: initialize is the first interaction. A request sent here
    // on a client that has not shaken hands yet does so first (MCP-C-15).
    if (method != "initialize" && !session->is_initialized())
        co_await session->initialize_async();
    co_return co_await session->rpc_call_async(
        method, params, deadline, std::move(cancel_token));
}

json detail::ProtocolSession::rpc_call(
    const std::string& method, const json& params) {
    return async::run_sync(rpc_call_async(method, params));
}

namespace {
void reset_initialization(const std::shared_ptr<detail::Transport>& transport,
                          const std::shared_ptr<detail::ClientMetadata>& metadata);
}  // namespace

asio::awaitable<json> detail::ProtocolSession::rpc_call_async(
    const std::string& method, const json& params,
    std::chrono::steady_clock::time_point deadline,
    std::shared_ptr<graph::CancelToken> cancel_token) {
    if (stopped_.load()) throw_shutdown();
    if ((cancel_token && cancel_token->is_cancelled())
        || deadline <= std::chrono::steady_clock::now()) {
        throw_rpc_bound(deadline, cancel_token);
    }
    auto transport = transport_;
    for (int attempt = 0;; ++attempt) {
        const int id = ++next_id_;
        json request{{"jsonrpc", "2.0"}, {"id", id},
                     {"method", method}, {"params", params}};
        json response;
        std::string expired_session;
        try {
            response = co_await transport->exchange(
                std::move(request), deadline, cancel_token);
        } catch (const SessionNotFound& error) {
            if (method == "initialize" || attempt > 0) throw;
            expired_session = error.session_id();
        } catch (const MCPTransportError& error) {
            // A request this client stops waiting for is abandoned: MCP asks the
            // client to tell the server (never for initialize), and its late
            // response is dropped.
            if (method != "initialize" && (error.failure() == MCPFailure::cancelled ||
                                           error.failure() == MCPFailure::timeout))
                transport->cancel_request(id, error.failure() == MCPFailure::cancelled
                                                  ? "cancelled by the caller"
                                                  : "request timed out");
            throw;
        }
        if (!expired_session.empty()) {
            // MCP transports, "Session Management": HTTP 404 for a request
            // that carried Mcp-Session-Id means the session ended, and the
            // client MUST start a new one. The server did not run the
            // request, so it goes again, once, in the new session (MCP-C-03).
            co_await restart_session(std::move(expired_session));
            continue;
        }
        if (stopped_.load()) throw_shutdown();
        try {
            co_return extract_rpc_result(response, id);
        } catch (const MCPTransportError&) {
            // A malformed response leaves the session unusable: the next
            // request starts a new one.
            reset_initialization(transport, metadata_);
            throw;
        }
    }
}

asio::awaitable<void> detail::ProtocolSession::restart_session(std::string stale) {
    if (transport_->forget_session(stale)) {
        {
            std::lock_guard lk(metadata_->mu);
            if (metadata_->lifecycle == ClientMetadata::Lifecycle::initialized) {
                metadata_->lifecycle = ClientMetadata::Lifecycle::created;
                metadata_->initialize_result = {};
            }
        }
        metadata_->changed.notify_all();
    }
    std::string client_name;
    {
        std::lock_guard lk(metadata_->mu);
        client_name = metadata_->client_name;
    }
    co_await initialize_async(client_name);
}

asio::awaitable<json> detail::HttpSession::exchange(
    json request, std::chrono::steady_clock::time_point deadline,
    std::shared_ptr<graph::CancelToken> cancel_token) {
    co_return co_await worker.submit(
        do_exchange(std::move(request), deadline, std::move(cancel_token)));
}

namespace {

std::string lowercase(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return out;
}

// The reconnection wait when a stream that ends early named none.
constexpr std::chrono::milliseconds kDefaultSseRetry{1000};
// Reconnections in a row that bring no event before the client gives up.
constexpr int kMaxIdleResumes = 3;
// A DELETE that ends a session never holds up a shutdown for longer.
constexpr std::chrono::milliseconds kSessionDeleteTimeout{2000};
// The largest JSON body read, as for an event.
constexpr std::size_t kMaxJsonBody = 16u * 1024u * 1024u;

}  // namespace

std::chrono::milliseconds detail::HttpSession::remaining(
    std::chrono::steady_clock::time_point until) const {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        until - std::chrono::steady_clock::now());
    if (left.count() <= 0)
        throw MCPTransportError(MCPFailure::timeout, "MCP HTTP request timed out");
    return left;
}

HeaderList detail::HttpSession::request_headers(const char* accept,
                                                const std::string* last_event_id,
                                                std::string* sent_session) {
    HeaderList headers;
    HeaderProvider provider;
    {
        std::lock_guard lk(mu);
        headers.emplace_back("Content-Type", "application/json");
        headers.emplace_back("Accept", accept);
        headers.insert(headers.end(), config.headers.begin(), config.headers.end());
        provider = config.header_provider;
        if (sent_session) *sent_session = session_id;
        if (!session_id.empty()) headers.emplace_back("Mcp-Session-Id", session_id);
        // MCP transports, "Protocol Version Header": every request after
        // initialize carries the negotiated revision.
        if (!protocol_version.empty()) headers.emplace_back("MCP-Protocol-Version", protocol_version);
    }
    if (last_event_id) headers.emplace_back("Last-Event-ID", *last_event_id);
    // User callbacks may refresh credentials or re-enter application code. Run
    // them after releasing the session mutex so they cannot deadlock the client.
    if (provider) {
        auto dynamic_headers = provider();
        headers.insert(headers.end(), dynamic_headers.begin(), dynamic_headers.end());
    }
    for (const auto& [name, value] : headers) {
        if (name.empty() || name.find_first_of("\r\n") != std::string::npos
            || value.find_first_of("\r\n") != std::string::npos) {
            throw std::invalid_argument("MCP header contains CR/LF or an empty name");
        }
    }
    return headers;
}

void detail::HttpSession::on_stream_message(HttpExchange& exchange, json message) {
    if (message.is_array()) {
        // MCP 2025-03-26 lets a peer send a JSON-RPC batch. Each message is
        // handled as if it came alone, and the replies to the requests in it
        // go back as one array.
        json replies = json::array();
        for (std::size_t i = 0; i < message.size(); ++i) {
            json element = message[i];
            if (message_kind(element) == MessageKind::request)
                replies.push_back(server_request_reply(element));
            else
                on_stream_message(exchange, std::move(element));
        }
        if (!replies.empty())
            asio::co_spawn(worker.io, best_effort(reply(std::move(replies))), asio::detached);
        return;
    }
    switch (message_kind(message)) {
    case MessageKind::response:
        // This request's response; a late response to an abandoned request
        // is dropped.
        if (!exchange.response && message["id"].is_number_integer() &&
            message["id"].get<long long>() == exchange.id)
            exchange.response = std::move(message);
        break;
    case MessageKind::request:
        // A server request on the response stream, such as a ping while a
        // tool runs: it is answered in its own POST while this stream stays
        // open (MCP-C-02).
        asio::co_spawn(worker.io, best_effort(reply(server_request_reply(message))),
                       asio::detached);
        break;
    case MessageKind::notification:
        deliver_notification(message);
        break;
    case MessageKind::invalid:
        break;
    }
}

void detail::HttpSession::listen() noexcept {
    try {
        worker.start([this] {
            if (listening || worker.stopped.load()) return;
            listening = true;
            listen_stop = std::make_unique<asio::steady_timer>(worker.io);
            listen_stop->expires_at(std::chrono::steady_clock::time_point::max());
            asio::co_spawn(worker.io, listen_loop(), asio::detached);
        });
    } catch (...) {
        // A shut-down client listens to nothing.
    }
}

asio::awaitable<void> detail::HttpSession::listen_loop() {
    // MCP transports, "Listening for Messages from the Server": the optional
    // GET stream carries notifications (such as tools/list_changed) and
    // requests between calls. A server that offers none answers 405; a
    // stream that ends is reopened with Last-Event-ID after `retry`.
    using asio::experimental::awaitable_operators::operator||;
    std::string last_event_id;
    std::optional<std::uint64_t> retry;
    int idle = 0;
    while (!worker.stopped.load()) {
        std::string sent_session;
        HeaderList headers;
        try {
            headers = request_headers("text/event-stream",
                                      last_event_id.empty() ? nullptr : &last_event_id,
                                      &sent_session);
        } catch (...) {
            break;
        }
        if (sent_session.empty()) break;  // no session to listen to
        auto stream = std::make_shared<HttpExchange>(-1);
        try {
            co_await (stream_once("GET", std::string(), std::move(headers), stream,
                                  std::chrono::milliseconds(0))
                      || listen_stop->async_wait(asio::use_awaitable));
        } catch (...) {
            if (worker.stopped.load()) break;
            stream->status = 0;
        }
        if (worker.stopped.load()) break;
        // 405: the server has no such stream; 404: the session ended (the
        // next request starts a new one, which listens again).
        if (stream->status >= 400 || (stream->status != 0 && !stream->event_stream)) break;
        if (!stream->events.last_event_id().empty()) last_event_id = stream->events.last_event_id();
        if (stream->events.retry_ms()) retry = stream->events.retry_ms();
        idle = stream->saw_event ? 0 : idle + 1;
        if (idle > kMaxIdleResumes) break;
        asio::steady_timer pause(worker.io);
        pause.expires_after(retry ? std::chrono::milliseconds(static_cast<std::int64_t>(
                                        std::min<std::uint64_t>(*retry, 3600000)))
                                  : kDefaultSseRetry);
        try {
            co_await (pause.async_wait(asio::use_awaitable)
                      || listen_stop->async_wait(asio::use_awaitable));
        } catch (...) {
            break;
        }
    }
    listening = false;
}

asio::awaitable<void> detail::HttpSession::stream_once(std::string method, std::string body,
                                                       HeaderList headers,
                                                       std::shared_ptr<HttpExchange> exchange,
                                                       std::chrono::milliseconds timeout) {
    struct StreamAnswered {};
    auto ex = co_await asio::this_coro::executor;
    async::RequestOptions options;
    options.timeout = timeout;
    // An event stream lasts as long as the call; its events are bounded one
    // by one, and a JSON body below.
    options.max_response_body_bytes = 0;
    async::HttpStreamResponse response;
    try {
    response = co_await async::async_request_stream(
        ex, method, endpoint.host, endpoint.port, target, body, std::move(headers),
        endpoint.tls,
        [this, exchange](const async::HttpResponseHead& head) {
            exchange->status = head.status;
            const bool success = head.status >= 200 && head.status < 300;
            exchange->event_stream =
                success && lowercase(head.get_header("Content-Type")).rfind("text/event-stream", 0) == 0;
            // The server may assign or rotate the session id on any response.
            if (const auto sid = head.get_header("Mcp-Session-Id"); success && !sid.empty()) {
                std::lock_guard lk(mu);
                session_id = std::string(sid);
            }
        },
        [this, exchange](std::string_view chunk) {
            if (!exchange->event_stream) {
                if (exchange->body.size() + chunk.size() > kMaxJsonBody)
                    throw MCPTransportError(MCPFailure::protocol, "MCP HTTP response body is too large");
                exchange->body.append(chunk);
                return;
            }
            // Server-Sent Events by the WHATWG rules: CRLF, LF or CR line
            // ends, multi-line data, id and retry. The client used to split
            // events only on "\n\n", so the official Python servers' CRLF
            // streams lost every response after a notification (MCP-C-01).
            exchange->events.feed(chunk);
            for (auto& event : exchange->events.drain()) {
                exchange->saw_event = true;
                if (event.data.empty()) continue;  // a priming event
                json message;
                try {
                    message = json::parse(event.data);
                } catch (const json::exception&) {
                    continue;
                }
                on_stream_message(*exchange, std::move(message));
            }
            // The request is answered: stop reading, whether or not the
            // server closes the stream (a resumed stream may stay open).
            if (exchange->response) throw StreamAnswered{};
        },
        options);
    } catch (const StreamAnswered&) {
        co_return;
    }
    exchange->status = response.status;
}

void detail::HttpSession::throw_http_failure(int status, const std::string& body) const {
    // A JSON-RPC error in the body keeps its code, message and data
    // (MCP-C-13), such as -32022 from a server that refuses the revision.
    std::optional<MCPError> rpc_error;
    try {
        const auto parsed = json::parse(body);
        if (parsed.is_object() && parsed.contains("error") && parsed["error"].is_object()) {
            const auto error = parsed["error"];
            if (error.contains("code") && error["code"].is_number_integer() &&
                error.contains("message") && error["message"].is_string()) {
                rpc_error.emplace(error["code"].get<int>(),
                                  "MCP JSON-RPC error (HTTP " + std::to_string(status) + "): " +
                                      error["message"].get<std::string>(),
                                  error.contains("data") ? error["data"] : json(nullptr), status);
            }
        }
    } catch (const json::exception&) {
    }
    if (rpc_error) throw *rpc_error;
    const std::string scheme = endpoint.tls ? "https://" : "http://";
    std::string message = "MCP error (HTTP " + std::to_string(status) + ") for " + scheme +
                          endpoint.host + ":" + endpoint.port + target + ": " +
                          body.substr(0, 512);
    if (status == 404) {
        message += " - the server has no MCP endpoint at this URL. The client uses the "
                   "configured endpoint URL as given, path and query included "
                   "(for example http://host:8000/mcp).";
    }
    throw MCPTransportError(MCPFailure::http_status, std::move(message), status);
}

asio::awaitable<json> detail::HttpSession::exchange_and_resume(
    json request, int id, std::chrono::steady_clock::time_point until) {
    std::string sent_session;
    auto headers = request_headers("application/json, text/event-stream", nullptr, &sent_session);
    auto exchange = std::make_shared<HttpExchange>(id);
    co_await stream_once("POST", request.dump(), std::move(headers), exchange, remaining(until));

    if (exchange->status == 404 && !sent_session.empty()) {
        throw SessionNotFound(sent_session, "MCP session " + sent_session +
                                                " is no longer known to the server (HTTP 404)");
    }
    if (exchange->status < 200 || exchange->status >= 300)
        throw_http_failure(exchange->status, exchange->body);
    if (!exchange->event_stream) {
        if (exchange->body.empty()) {
            throw MCPTransportError(MCPFailure::protocol,
                                    "MCP server answered a request without a JSON-RPC response (HTTP " +
                                        std::to_string(exchange->status) + ")");
        }
        json body;
        try {
            body = json::parse(exchange->body);
        } catch (const json::parse_error& e) {
            throw MCPTransportError(MCPFailure::protocol,
                                    std::string("MCP HTTP response JSON: ") + e.what());
        }
        if (!body.is_array()) co_return body;
        // A batch as the body: its messages are handled as on a stream, and
        // this request's response is the one with its id.
        on_stream_message(*exchange, std::move(body));
        if (!exchange->response) {
            throw MCPTransportError(MCPFailure::protocol,
                                    "MCP HTTP batch response holds no response for the request");
        }
        co_return std::move(*exchange->response);
    }

    // MCP transports, "Resumability" (2025-11-25): a server may close the
    // stream before the response, after it sent an event id. The client then
    // waits the stream's `retry` and asks for the rest with GET and
    // Last-Event-ID (MCP-C-07).
    std::string last_event_id = exchange->events.last_event_id();
    auto retry = exchange->events.retry_ms();
    int idle_resumes = 0;
    while (!exchange->response) {
        if (last_event_id.empty()) {
            throw MCPTransportError(
                MCPFailure::protocol,
                "MCP SSE response ended without a JSON-RPC response for the request");
        }
        if (++idle_resumes > kMaxIdleResumes) {
            throw MCPTransportError(MCPFailure::protocol,
                                    "MCP SSE stream ended repeatedly without the response");
        }
        const auto wait = std::min(
            retry ? std::chrono::milliseconds(static_cast<std::int64_t>(
                        std::min<std::uint64_t>(*retry, 3600000)))
                  : kDefaultSseRetry,
            remaining(until));
        asio::steady_timer timer(co_await asio::this_coro::executor);
        timer.expires_after(wait);
        co_await timer.async_wait(asio::use_awaitable);

        std::string resume_session;
        auto resume_headers = request_headers("text/event-stream", &last_event_id, &resume_session);
        auto resumed = std::make_shared<HttpExchange>(id);
        co_await stream_once("GET", std::string(), std::move(resume_headers), resumed,
                             remaining(until));
        // The request already reached the server, so a lost session cannot
        // be recovered by sending it again.
        if (resumed->status < 200 || resumed->status >= 300)
            throw_http_failure(resumed->status, resumed->body);
        if (!resumed->event_stream) {
            throw MCPTransportError(MCPFailure::protocol,
                                    "MCP server did not resume the event stream");
        }
        if (resumed->saw_event) idle_resumes = 0;
        if (!resumed->events.last_event_id().empty())
            last_event_id = resumed->events.last_event_id();
        if (resumed->events.retry_ms()) retry = resumed->events.retry_ms();
        exchange = std::move(resumed);
    }
    co_return std::move(*exchange->response);
}

asio::awaitable<json> detail::HttpSession::do_exchange(
    json request, std::chrono::steady_clock::time_point deadline,
    std::shared_ptr<graph::CancelToken> cancel_token) {
    if (worker.stopped.load()) throw_shutdown();
    using asio::experimental::awaitable_operators::operator||;
    const int id = request.at("id").get<int>();
    auto until = deadline;
    if (config.request_timeout.count() > 0)
        until = std::min(until, std::chrono::steady_clock::now() + config.request_timeout);
    if ((cancel_token && cancel_token->is_cancelled()) ||
        deadline <= std::chrono::steady_clock::now())
        throw_rpc_bound(deadline, cancel_token);
    try {
        std::variant<CapturedResult<json>, std::monostate> result;
        try {
            result = co_await (
                capture_result(exchange_and_resume(std::move(request), id, until))
                || wait_for_rpc_bound(deadline, cancel_token, shutdown_token));
        } catch (const asio::multiple_exceptions& both) {
            // The caller cancelled the coroutine awaiting this call, and both
            // waits ended with it: the first failure names it.
            std::rethrow_exception(both.first_exception());
        }
        if (result.index() == 1) throw_rpc_bound(deadline, cancel_token, shutdown_token);
        auto captured = std::get<0>(std::move(result));
        if (captured.error) std::rethrow_exception(captured.error);
        if (worker.stopped.load()) throw_shutdown();
        co_return std::move(*captured.value);
    } catch (const std::system_error& e) {
        // An aborted operation of a client still running was cancelled by
        // its caller, for example a graph run whose cancel token cancels
        // the coroutine that awaits the call: the server is told.
        const auto failure = e.code() == asio::error::timed_out ? MCPFailure::timeout
            : aborted_by_caller(e.code())
                ? (worker.stopped.load() ? MCPFailure::shutdown : MCPFailure::cancelled)
                : MCPFailure::connection;
        throw MCPTransportError(failure, std::string("MCP HTTP request failed: ") + e.what());
    } catch (const std::length_error& e) {
        throw MCPTransportError(MCPFailure::protocol, std::string("MCP HTTP response: ") + e.what());
    }
}

asio::awaitable<std::pair<int, std::string>> detail::HttpSession::send(
    std::string method, std::string body, HeaderList headers, std::chrono::milliseconds timeout) {
    using asio::experimental::awaitable_operators::operator||;
    auto exchange = std::make_shared<HttpExchange>(0);
    exchange->event_stream = false;
    auto result = co_await (
        capture_result(notification_result(
            stream_once(std::move(method), std::move(body), std::move(headers), exchange, timeout)))
        || wait_for_rpc_bound(std::chrono::steady_clock::time_point::max(), {}, shutdown_token));
    if (result.index() == 1) throw_shutdown();
    auto captured = std::get<0>(std::move(result));
    if (captured.error) std::rethrow_exception(captured.error);
    co_return std::make_pair(exchange->status, std::move(exchange->body));
}

asio::awaitable<void> detail::HttpSession::reply(json message) {
    // The response to a server request goes to the endpoint like any other
    // message; the server answers 202 with no body.
    auto headers = request_headers("application/json, text/event-stream", nullptr, nullptr);
    co_await send("POST", message.dump(), std::move(headers), config.request_timeout);
}

asio::awaitable<void> detail::HttpSession::delete_session(std::string session) {
    std::string sent;
    auto headers = request_headers("application/json, text/event-stream", nullptr, &sent);
    if (sent != session) co_return;
    const auto timeout = config.request_timeout.count() > 0
        ? std::min(config.request_timeout, kSessionDeleteTimeout)
        : kSessionDeleteTimeout;
    // 405 means the server does not let clients end sessions: nothing to do.
    co_await send("DELETE", std::string(), std::move(headers), timeout);
}

void detail::HttpSession::end_session() noexcept {
    // MCP transports, "Session Management": a client that no longer needs
    // its session SHOULD end it with DELETE. MCPClient::shutdown() and the
    // release of the last reference both do, once and bounded (MCP-C-10).
    std::string session;
    {
        std::lock_guard lk(mu);
        session = session_id;
    }
    if (session.empty() || worker.stopped.load() ||
        worker.io.get_executor().running_in_this_thread())
        return;
    try {
        auto done = std::make_shared<std::promise<void>>();
        auto finished = done->get_future();
        worker.start([this, done, session]() mutable {
            asio::co_spawn(worker.io, best_effort(delete_session(std::move(session))),
                           [done](std::exception_ptr) { done->set_value(); });
        });
        finished.wait_for(kSessionDeleteTimeout + std::chrono::milliseconds(500));
    } catch (...) {
        // A client that cannot reach its server leaves the session to expire.
    }
    std::lock_guard lk(mu);
    session_id.clear();
}

namespace {

template <typename T>
T decode_protocol_result(const json& raw) {
    try {
        return T::from_json(raw);
    } catch (const std::exception& e) {
        throw MCPTransportError(MCPFailure::protocol,
                                std::string("MCP invalid result: ") + e.what());
    }
}

#ifndef NEOGRAPH_VERSION_STR
#define NEOGRAPH_VERSION_STR "unknown"
#endif

json initialize_params(const std::string& client_name) {
    json params;
    params["protocolVersion"] = MCP_SUPPORTED_PROTOCOL_VERSIONS.front();
    params["capabilities"]    = json::object();
    // The library's version, not a placeholder (MCP-C-20).
    params["clientInfo"]      = {{"name", client_name}, {"version", NEOGRAPH_VERSION_STR}};
    return params;
}

void store_initialize_result(
    const std::shared_ptr<detail::ClientMetadata>& metadata,
    InitializeResult result) {
    {
        std::lock_guard lk(metadata->mu);
        if (metadata->lifecycle == detail::ClientMetadata::Lifecycle::shutdown)
            throw_shutdown();
        metadata->initialize_result = std::move(result);
        metadata->lifecycle = detail::ClientMetadata::Lifecycle::initialized;
    }
    metadata->changed.notify_all();
}

void reset_initialization(
    const std::shared_ptr<detail::Transport>& transport,
    const std::shared_ptr<detail::ClientMetadata>& metadata) {
    transport->reset();
    {
        std::lock_guard lk(metadata->mu);
        if (metadata->lifecycle != detail::ClientMetadata::Lifecycle::shutdown)
            metadata->lifecycle = detail::ClientMetadata::Lifecycle::created;
        metadata->initialize_result = {};
        metadata->output_schemas.clear();
    }
    metadata->changed.notify_all();
}

} // namespace

asio::awaitable<void> detail::HttpSession::notify(json notification) {
    co_await worker.submit(notification_result(do_notify(std::move(notification))));
}

asio::awaitable<void> detail::HttpSession::do_notify(json notification) {
    if (worker.stopped.load()) throw_shutdown();
    std::pair<int, std::string> answer;
    try {
        auto headers = request_headers("application/json, text/event-stream", nullptr, nullptr);
        if (worker.stopped.load()) throw_shutdown();
        answer = co_await send("POST", notification.dump(), std::move(headers),
                               config.request_timeout);
    } catch (const std::system_error& e) {
        throw MCPTransportError(e.code() == asio::error::timed_out
                                    ? MCPFailure::timeout : MCPFailure::connection,
                                std::string("MCP HTTP notification: ") + e.what());
    }
    if (answer.first < 200 || answer.first >= 300) {
        throw MCPTransportError(MCPFailure::http_status,
            "MCP notification returned HTTP " + std::to_string(answer.first) + ": " +
                answer.second.substr(0, 512), answer.first);
    }
}


bool MCPClient::initialize(const std::string& client_name) {
    return session_->initialize(client_name);
}

asio::awaitable<bool> MCPClient::initialize_async(const std::string& client_name) {
    auto session = session_;
    co_return co_await session->initialize_async(client_name);
}

bool MCPClient::initialize(const std::string& client_name,
                           std::chrono::steady_clock::time_point deadline,
                           std::shared_ptr<graph::CancelToken> cancel_token) {
    return async::run_sync(initialize_async(client_name, deadline, std::move(cancel_token)));
}

asio::awaitable<bool> MCPClient::initialize_async(
    const std::string& client_name, std::chrono::steady_clock::time_point deadline,
    std::shared_ptr<graph::CancelToken> cancel_token) {
    auto session = session_;
    co_return co_await session->initialize_async(client_name, deadline, std::move(cancel_token));
}

void MCPClient::set_tool_list_changed_handler(std::function<void()> handler) {
    session_->set_tool_list_changed_handler(std::move(handler));
}

bool detail::ProtocolSession::initialize(const std::string& client_name) {
    return async::run_sync(initialize_async(client_name));
}

asio::awaitable<bool> detail::ProtocolSession::initialize_async(
    const std::string& client_name, std::chrono::steady_clock::time_point deadline,
    std::shared_ptr<graph::CancelToken> cancel_token) {
    // Only one caller performs the handshake; concurrent callers wait for the
    // same negotiated session. A failure resets both protocol and transport.
    for (;;) {
        if (stopped_.load()) throw_shutdown();
        if ((cancel_token && cancel_token->is_cancelled()) ||
            deadline <= std::chrono::steady_clock::now())
            throw_rpc_bound(deadline, cancel_token);
        {
            std::lock_guard lk(metadata_->mu);
            if (metadata_->lifecycle
                == detail::ClientMetadata::Lifecycle::initialized) {
                co_return true;
            }
            if (metadata_->lifecycle
                == detail::ClientMetadata::Lifecycle::created) {
                metadata_->lifecycle =
                    detail::ClientMetadata::Lifecycle::initializing;
                metadata_->client_name = client_name;
                break;
            }
        }
        asio::steady_timer timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }

    bool unsupported_version = false;
    try {
        // The bounds stop waiting only: MCP never cancels initialize on the
        // wire, so no notifications/cancelled follows.
        auto raw = co_await rpc_call_async(
            "initialize", initialize_params(client_name), deadline, cancel_token);
        auto result = decode_protocol_result<InitializeResult>(raw);
        if (!is_supported_protocol_version(result.protocol_version)) {
            // MCP lifecycle, "Version Negotiation": a client that does not
            // support the server's revision SHOULD disconnect (MCP-C-11, D2).
            std::string supported;
            for (const auto version : MCP_SUPPORTED_PROTOCOL_VERSIONS) {
                if (!supported.empty()) supported += ", ";
                supported += version;
            }
            unsupported_version = true;
            throw MCPTransportError(
                MCPFailure::protocol,
                "MCP server answered protocolVersion " + json(result.protocol_version).dump() +
                    ", which this client does not support (supported: " + supported + ")");
        }
        transport_->negotiated_version(result.protocol_version);
        json notification = {
            {"jsonrpc", "2.0"}, {"method", "notifications/initialized"},
            {"params", json::object()},
        };
        co_await transport_->notify(std::move(notification));
        store_initialize_result(metadata_, std::move(result));
        listen_if_wanted();
        co_return true;
    } catch (...) {
        reset_initialization(transport_, metadata_);
        if (unsupported_version) shutdown();
        throw;
    }
}

void detail::ProtocolSession::set_tool_list_changed_handler(std::function<void()> handler) {
    {
        std::lock_guard lk(metadata_->mu);
        metadata_->tool_list_changed = std::move(handler);
    }
    listen_if_wanted();
}

void detail::ProtocolSession::listen_if_wanted() {
    // As the official clients do, an HTTP session opens the optional GET
    // stream once it is initialized: servers send requests and
    // notifications that belong to no call there (FastMCP's ping, for
    // instance, and tools/list_changed). A server without one answers 405.
    bool initialized = false;
    {
        std::lock_guard lk(metadata_->mu);
        initialized = metadata_->lifecycle == ClientMetadata::Lifecycle::initialized;
    }
    if (initialized) transport_->listen();
}

bool detail::ProtocolSession::is_initialized() const noexcept {
    std::lock_guard lk(metadata_->mu);
    return metadata_->lifecycle
        == detail::ClientMetadata::Lifecycle::initialized;
}

InitializeResult detail::ProtocolSession::get_initialize_result() const {
    std::lock_guard lk(metadata_->mu);
    if (metadata_->lifecycle
        != detail::ClientMetadata::Lifecycle::initialized) {
        throw std::logic_error("MCP client has not been initialized");
    }
    return metadata_->initialize_result;
}

std::vector<std::unique_ptr<Tool>> MCPClient::get_tools() {
    auto definitions = session_->get_tool_definitions();
    std::vector<std::unique_ptr<Tool>> tools;
    tools.reserve(definitions.size());
    for (auto& definition : definitions) {
        tools.push_back(std::unique_ptr<Tool>(
            new MCPTool(session_, std::move(definition))));
    }
    return tools;
}

ListToolsPage detail::ProtocolSession::list_tools(
    const std::optional<std::string>& cursor) {
    initialize();
    json params = json::object();
    if (cursor) params["cursor"] = *cursor;
    auto page = decode_protocol_result<ListToolsPage>(rpc_call("tools/list", params));
    {
        std::lock_guard lk(metadata_->mu);
        for (const auto& tool : page.tools) {
            metadata_->output_schemas[tool.name] = tool.output_schema;
        }
    }
    return page;
}

asio::awaitable<ListToolsPage> detail::ProtocolSession::list_tools_async(
    const std::optional<std::string>& cursor) {
    co_await initialize_async();
    json params = json::object();
    if (cursor) params["cursor"] = *cursor;
    auto raw = co_await rpc_call_async("tools/list", params);
    auto page = decode_protocol_result<ListToolsPage>(raw);
    {
        std::lock_guard lk(metadata_->mu);
        for (const auto& tool : page.tools) {
            metadata_->output_schemas[tool.name] = tool.output_schema;
        }
    }
    co_return page;
}

std::vector<ToolDefinition> detail::ProtocolSession::get_tool_definitions() {
    std::vector<ToolDefinition> definitions;
    std::optional<std::string> cursor;
    do {
        auto page = list_tools(cursor);
        definitions.insert(definitions.end(),
                           std::make_move_iterator(page.tools.begin()),
                           std::make_move_iterator(page.tools.end()));
        if (page.next_cursor == cursor && cursor) {
            throw std::runtime_error(
                "MCP tools/list returned the same nextCursor repeatedly");
        }
        cursor = std::move(page.next_cursor);
    } while (cursor);
    return definitions;
}

json detail::ProtocolSession::call_tool(const std::string& name, const json& arguments) {
    initialize();
    json params;
    params["name"]      = name;
    params["arguments"] = arguments;
    return rpc_call("tools/call", params);
}

CallToolResult detail::ProtocolSession::call_tool_result(
    const std::string& name, const json& arguments) {
    auto result = decode_protocol_result<CallToolResult>(call_tool(name, arguments));
    json output_schema;
    bool has_output_schema = false;
    {
        std::lock_guard lk(metadata_->mu);
        auto it = metadata_->output_schemas.find(name);
        if (it != metadata_->output_schemas.end()) {
            output_schema = it->second;
            has_output_schema = true;
        }
    }
    if (has_output_schema) validate_tool_result(result, output_schema);
    return result;
}

asio::awaitable<json>
detail::ProtocolSession::call_tool_async(const std::string& name, const json& arguments,
                                         std::chrono::steady_clock::time_point deadline,
                                         std::shared_ptr<graph::CancelToken> cancel_token) {
    // MCP never cancels initialization; the bounds apply to the call itself.
    co_await initialize_async();
    json params;
    params["name"]      = name;
    params["arguments"] = arguments;
    co_return co_await rpc_call_async("tools/call", params, deadline, std::move(cancel_token));
}

asio::awaitable<CallToolResult> detail::ProtocolSession::call_tool_result_async(
    const std::string& name, const json& arguments,
    std::chrono::steady_clock::time_point deadline,
    std::shared_ptr<graph::CancelToken> cancel_token) {
    auto raw = co_await call_tool_async(name, arguments, deadline, std::move(cancel_token));
    auto result = decode_protocol_result<CallToolResult>(raw);
    json output_schema;
    bool has_output_schema = false;
    {
        std::lock_guard lk(metadata_->mu);
        auto it = metadata_->output_schemas.find(name);
        if (it != metadata_->output_schemas.end()) {
            output_schema = it->second;
            has_output_schema = true;
        }
    }
    if (has_output_schema) validate_tool_result(result, output_schema);
    co_return result;
}

bool MCPClient::is_initialized() const noexcept {
    return session_->is_initialized();
}

InitializeResult MCPClient::get_initialize_result() const {
    return session_->get_initialize_result();
}

ListToolsPage MCPClient::list_tools(const std::optional<std::string>& cursor) {
    return session_->list_tools(cursor);
}

asio::awaitable<ListToolsPage> MCPClient::list_tools_async(
    const std::optional<std::string>& cursor) {
    auto session = session_;
    co_return co_await session->list_tools_async(cursor);
}

std::vector<ToolDefinition> MCPClient::get_tool_definitions() {
    return session_->get_tool_definitions();
}

json MCPClient::call_tool(const std::string& name, const json& arguments) {
    return session_->call_tool(name, arguments);
}

CallToolResult MCPClient::call_tool_result(
    const std::string& name, const json& arguments) {
    return session_->call_tool_result(name, arguments);
}

asio::awaitable<json> MCPClient::call_tool_async(
    const std::string& name, const json& arguments) {
    auto session = session_;
    co_return co_await session->call_tool_async(name, arguments);
}

asio::awaitable<CallToolResult> MCPClient::call_tool_result_async(
    const std::string& name, const json& arguments) {
    auto session = session_;
    co_return co_await session->call_tool_result_async(name, arguments);
}

asio::awaitable<CallToolResult> MCPClient::call_tool_result_async(
    const std::string& name, const json& arguments,
    std::chrono::steady_clock::time_point deadline,
    std::shared_ptr<graph::CancelToken> cancel_token) {
    auto session = session_;
    co_return co_await session->call_tool_result_async(name, arguments, deadline,
                                                       std::move(cancel_token));
}

} // namespace neograph::mcp
