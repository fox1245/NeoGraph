// Remote transports of the ACP server: Streamable HTTP and WebSocket on one
// endpoint. The mapping and the deviations from the draft are described in
// include/neograph/acp/http_server.h.
//
// One connection is one ACPServer, driven by ACPServer::run() over a
// queue-backed stream pair, so a connection behaves exactly like an stdio
// agent. The transport only (a) turns HTTP requests and WebSocket frames into
// lines for the input queue, and (b) routes the lines the agent writes to the
// stream or socket they belong to. Both directions are non-blocking for the
// agent: its sink only appends to a bounded queue, and a thread that belongs
// to the HTTP request or the socket does the writing.

#include <neograph/acp/http_server.h>

#include "../core/http_elastic_task_queue.h"
#include "../core/http_request_guard.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <future>
#include <istream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <random>
#include <set>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace neograph::acp {
namespace {

using json  = neograph::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using neograph::detail::ElasticTaskQueue;
using neograph::detail::HttpRequestGuard;

/// The most bytes an outbound queue holds for a stream that is not being
/// read. A peer that cannot keep up (or never opens its stream) loses its
/// connection instead of growing the server.
constexpr std::size_t kMaxQueuedBytes = 8 * 1024 * 1024;
/// Requests of one connection that have not been answered yet.
constexpr std::size_t kMaxOpenRequests = 1024;
/// How long a POST of `initialize` waits for the agent's answer.
constexpr auto kInitializeTimeout = 30s;
constexpr auto kReaperTick        = 250ms;
/// A stream wakes this often to notice that its peer is gone or that it was
/// replaced or closed.
constexpr auto kStreamSlice = 250ms;
/// A WebSocket handler wakes this often to notice that the server stops or
/// the connection is closed. A peer that stops in the middle of a frame for
/// longer than this is disconnected: the HTTP library cannot resume a frame.
constexpr auto kWebSocketSlice = 2s;
/// The largest frame the HTTP library accepts (CPPHTTPLIB_WEBSOCKET_MAX_PAYLOAD_LENGTH).
constexpr std::size_t kWebSocketFrameLimit = 16 * 1024 * 1024;
constexpr int kTryAgainLater = 1013;

json rpc_error(int code, std::string message, json id = nullptr) {
    json response = {
        {"jsonrpc", "2.0"},
        {"error", {{"code", code}, {"message", std::move(message)}}},
    };
    if (!id.is_null()) response["id"] = std::move(id);
    return response;
}

void http_error(httplib::Response& response, int status, int rpc_code, std::string message) {
    response.status = status;
    response.set_content(rpc_error(rpc_code, std::move(message)).dump(), "application/json");
}

bool media_type_contains(const std::string& header, const std::string& expected) {
    std::size_t start = 0;
    while (start <= header.size()) {
        auto end = header.find(',', start);
        if (end == std::string::npos) end = header.size();
        auto item = header.substr(start, end - start);
        item = item.substr(0, item.find(';'));
        const auto first = item.find_first_not_of(" \t");
        const auto last  = item.find_last_not_of(" \t");
        if (first != std::string::npos) {
            item = item.substr(first, last - first + 1);
            std::transform(item.begin(), item.end(), item.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (item == expected) return true;
        }
        start = end + 1;
    }
    return false;
}

bool is_websocket_upgrade(const httplib::Request& request) {
    if (request.method != "GET") return false;
    std::string upgrade = request.get_header_value("Upgrade");
    std::transform(upgrade.begin(), upgrade.end(), upgrade.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return upgrade.find("websocket") != std::string::npos;
}

std::string random_id(const char* prefix) {
    static std::mutex         mutex;
    static std::random_device device;
    std::uint32_t             words[4];
    {
        std::lock_guard lock(mutex);
        for (auto& word : words) word = device();
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s%08x%08x%08x%08x", prefix,
                  static_cast<unsigned>(words[0]), static_cast<unsigned>(words[1]),
                  static_cast<unsigned>(words[2]), static_cast<unsigned>(words[3]));
    return buffer;
}

std::string string_field(const json& object, const char* key) {
    if (object.is_object() && object.contains(key) && object[key].is_string()) {
        return object[key].get<std::string>();
    }
    return {};
}

/// The principal that pre_route authenticated for the upgrade request this
/// thread is serving. A WebSocket handler receives no Response to carry it,
/// and the handler runs on the thread that ran the pre-routing handler.
thread_local std::string t_upgrade_principal;

// --- The agent's stdin and stdout ------------------------------------------

/// The input of ACPServer::run(): one line per message, blocking until one
/// arrives or the source is closed.
class LineSource final : public std::streambuf {
public:
    bool push(std::string line) {
        {
            std::lock_guard lock(mutex_);
            if (closed_) return false;
            lines_.push_back(std::move(line));
        }
        ready_.notify_one();
        return true;
    }

    /// End of input. Lines not read yet are dropped: the connection is going.
    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
            lines_.clear();
        }
        ready_.notify_all();
    }

protected:
    int_type underflow() override {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return closed_ || !lines_.empty(); });
        if (lines_.empty()) return traits_type::eof();
        current_ = std::move(lines_.front());
        lines_.pop_front();
        current_.push_back('\n');
        setg(current_.data(), current_.data(), current_.data() + current_.size());
        return traits_type::to_int_type(current_.front());
    }

private:
    std::mutex              mutex_;
    std::condition_variable ready_;
    std::deque<std::string> lines_;
    std::string             current_;
    bool                    closed_ = false;
};

/// The output of ACPServer::run(): hands each complete line to `deliver`.
class LineSink final : public std::streambuf {
public:
    void set_deliver(std::function<void(std::string)> deliver) { deliver_ = std::move(deliver); }

protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        for (std::streamsize i = 0; i < count; ++i) put(data[i]);
        return count;
    }
    int_type overflow(int_type ch) override {
        if (!traits_type::eq_int_type(ch, traits_type::eof())) put(traits_type::to_char_type(ch));
        return traits_type::not_eof(ch);
    }

private:
    void put(char ch) {
        if (ch != '\n') {
            line_.push_back(ch);
            return;
        }
        std::string line;
        line.swap(line_);
        if (!line.empty() && deliver_) deliver_(std::move(line));
    }

    std::function<void(std::string)> deliver_;
    std::string                      line_;
};

// --- Outbound queue of one stream scope or WebSocket -------------------------

class Outbox {
public:
    enum class Next { Item, Idle, Closed, Superseded };

    /// False when the queue is full: the message is dropped and the caller
    /// closes the connection.
    bool push(std::string line) {
        {
            std::lock_guard lock(mutex_);
            if (closed_) return true;
            if (bytes_ + line.size() > kMaxQueuedBytes) return false;
            bytes_ += line.size();
            items_.push_back(std::move(line));
        }
        ready_.notify_all();
        return true;
    }

    /// A new reader takes over the queue; the previous one ends.
    std::uint64_t attach() {
        std::uint64_t generation;
        {
            std::lock_guard lock(mutex_);
            generation = ++generation_;
        }
        ready_.notify_all();
        return generation;
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        ready_.notify_all();
    }

    Next next(std::uint64_t generation, std::chrono::milliseconds wait, std::string& out) {
        std::unique_lock lock(mutex_);
        ready_.wait_for(lock, wait, [&] {
            return closed_ || generation != generation_ || !items_.empty();
        });
        if (closed_) return Next::Closed;
        if (generation != generation_) return Next::Superseded;
        if (items_.empty()) return Next::Idle;
        out = std::move(items_.front());
        items_.pop_front();
        bytes_ -= out.size();
        return Next::Item;
    }

private:
    std::mutex              mutex_;
    std::condition_variable ready_;
    std::deque<std::string> items_;
    std::size_t             bytes_      = 0;
    bool                    closed_     = false;
    std::uint64_t           generation_ = 0;
};

// --- One connection --------------------------------------------------------

struct Connection : std::enable_shared_from_this<Connection> {
    /// Where the response to a request goes, and what its method taught us.
    struct Route {
        std::string method;
        std::string session;
        bool        session_scoped = false;
        /// session/resume took the session over from nobody (so a refused
        /// resume gives it back).
        bool claimed_fresh = false;
    };

    Connection(std::string connection_id, std::string principal_in, bool websocket_in,
               ACPHttpConnection parts_in)
        : id(std::move(connection_id)),
          principal(std::move(principal_in)),
          websocket(websocket_in),
          parts(std::move(parts_in)),
          in(&source),
          out(&sink) {}

    ~Connection() {
        source.close();
        if (runner.joinable() && runner.get_id() != std::this_thread::get_id()) runner.join();
    }

    void touch() { last_active.store(Clock::now().time_since_epoch().count()); }
    Clock::time_point active_at() const {
        return Clock::time_point(Clock::duration(last_active.load()));
    }

    bool knows(const std::string& session) {
        std::lock_guard lock(mutex);
        return known_sessions.count(session) != 0;
    }
    void know(const std::string& session) {
        std::lock_guard lock(mutex);
        known_sessions.insert(session);
    }
    /// The session's own stream queue, created on first use.
    std::shared_ptr<Outbox> box_for(const std::string& session) {
        std::lock_guard lock(mutex);
        auto& box = session_boxes[session];
        if (!box) box = std::make_shared<Outbox>();
        return box;
    }
    /// The session's own stream queue; null for a session that has none.
    std::shared_ptr<Outbox> find_box(const std::string& session) {
        std::lock_guard lock(mutex);
        auto it = session_boxes.find(session);
        return it == session_boxes.end() ? nullptr : it->second;
    }
    void close_boxes() {
        std::vector<std::shared_ptr<Outbox>> boxes;
        {
            std::lock_guard lock(mutex);
            for (auto& entry : session_boxes) boxes.push_back(entry.second);
        }
        connection_box->close();
        for (auto& box : boxes) box->close();
    }

    const std::string id;
    const std::string principal;
    const bool        websocket;
    ACPHttpConnection parts;

    LineSource   source;
    std::istream in;
    LineSink     sink;
    std::ostream out;
    std::thread  runner;

    /// The connection stream; for a WebSocket connection, the socket.
    std::shared_ptr<Outbox> connection_box = std::make_shared<Outbox>();

    std::mutex                                     mutex;
    std::map<std::string, std::shared_ptr<Outbox>> session_boxes;
    std::set<std::string>                          known_sessions;
    std::map<std::string, Route>                   routes;  ///< by request id (dump)
    std::optional<std::string>                     initialize_id;
    std::shared_ptr<std::promise<json>>            initialize_promise;

    /// Serializes the end of the connection: one thread joins the agent.
    std::mutex                end_mutex;
    std::atomic<bool>         closing{false};
    /// The agent's queue overflowed or its run loop ended: close it.
    std::atomic<bool>         failed{false};
    std::atomic<int>          streams{0};
    std::atomic<Clock::rep>   last_active{Clock::now().time_since_epoch().count()};
};

/// Session ids by the connection that holds them: a session belongs to one
/// live connection (its engine thread id is the session id).
class Registry {
public:
    enum class Claim { Fresh, Held, Refused };

    Claim claim(const std::string& session, const std::shared_ptr<Connection>& connection) {
        std::lock_guard lock(mutex_);
        auto&           slot  = owners_[session];
        const auto      owner = slot.lock();
        if (owner == connection) return Claim::Held;
        if (owner && !owner->closing.load()) return Claim::Refused;
        slot = connection;
        return Claim::Fresh;
    }

    void release(const std::string& session, const Connection* connection) {
        std::lock_guard lock(mutex_);
        auto            it = owners_.find(session);
        if (it != owners_.end() && it->second.lock().get() == connection) owners_.erase(it);
    }

    void release_all(const Connection* connection) {
        std::lock_guard lock(mutex_);
        for (auto it = owners_.begin(); it != owners_.end();) {
            const auto owner = it->second.lock();
            if (!owner || owner.get() == connection) {
                it = owners_.erase(it);
            } else {
                ++it;
            }
        }
    }

    /// The ACPServer of the live connection that holds `session`, kept
    /// alive by the pointer it returns.
    std::shared_ptr<ACPServer> resolve(std::string_view session) {
        std::shared_ptr<Connection> owner;
        {
            std::lock_guard lock(mutex_);
            auto            it = owners_.find(std::string(session));
            if (it == owners_.end()) return nullptr;
            owner = it->second.lock();
        }
        if (!owner || owner->closing.load() || !owner->parts.server) return nullptr;
        return std::shared_ptr<ACPServer>(owner, owner->parts.server.get());
    }

private:
    std::mutex                                   mutex_;
    std::map<std::string, std::weak_ptr<Connection>> owners_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct ACPHttpServer::Impl {
    Impl(ConnectionFactory value_factory, ACPHttpConfig value_config)
        : factory(std::move(value_factory)), config(std::move(value_config)) {
        if (!factory) throw std::invalid_argument("ACPHttpServer factory is required");
        if (config.endpoint.empty() || config.endpoint.front() != '/' ||
            config.endpoint.find_first_of("\r\n?#") != std::string::npos) {
            throw std::invalid_argument("ACP HTTP endpoint must be an absolute path");
        }
        if (config.port < 0 || config.port > 65535 || config.max_connections == 0 ||
            config.max_payload_bytes == 0 || config.http_threads == 0 ||
            config.max_http_threads < config.http_threads || config.max_http_threads < 2 ||
            config.max_queued_requests == 0 || config.connection_idle_timeout.count() < 0 ||
            config.keepalive_interval.count() <= 0) {
            throw std::invalid_argument("ACP HTTP limits and port must be valid");
        }
        if (!config.streamable_http && !config.websocket) {
            throw std::invalid_argument("ACP HTTP server needs a transport: enable "
                                        "streamable_http or websocket");
        }
        if (config.websocket && config.max_payload_bytes > kWebSocketFrameLimit) {
            throw std::invalid_argument(
                "ACP HTTP max_payload_bytes may not exceed 16 MiB while websocket is on");
        }
        if (!HttpRequestGuard::is_loopback_bind(config.host) && !config.authenticator) {
            throw std::invalid_argument("remote ACP HTTP binds require an authenticator");
        }
        HttpRequestGuard::require_host_names(
            config.allowed_hosts,
            "ACP HTTP allowed_hosts entries are host names without a port, "
            "IPv6 addresses in brackets, or *");
        guard.allowed_hosts   = config.allowed_hosts;
        guard.allowed_origins = config.allowed_origins;
        max_streams           = std::max<std::size_t>(1, config.max_http_threads / 2);

        http.set_payload_max_length(config.max_payload_bytes);
        http.new_task_queue = [threads = config.http_threads, max = config.max_http_threads,
                               queued = config.max_queued_requests] {
            return new ElasticTaskQueue(threads, max, queued);
        };
        const auto ping_seconds = static_cast<time_t>(std::max<std::int64_t>(
            1, (config.keepalive_interval.count() + 999) / 1000));
        http.set_websocket_ping_interval(ping_seconds);
        http.set_websocket_max_missed_pongs(3);
        http.set_pre_routing_handler(
            [this](const httplib::Request& request, httplib::Response& response) {
                return pre_route(request, response);
            });
        register_routes();
    }

    ConnectionFactory factory;
    ACPHttpConfig     config;
    HttpRequestGuard  guard{"ACP HTTP server"};
    httplib::Server   http;
    std::thread       listener;
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};
    int               bound_port = 0;
    std::size_t       max_streams = 1;

    mutable std::mutex                          connections_mutex;
    std::map<std::string, std::shared_ptr<Connection>> connections;
    std::size_t                                 creating = 0;
    std::atomic<std::size_t>                    open_streams{0};
    std::shared_ptr<Registry>                   registry = std::make_shared<Registry>();

    std::mutex                 client_mutex;
    std::shared_ptr<ACPClient> client_handle;

    /// stop() may run on two threads (the caller's and the one blocked in
    /// start()); one at a time joins the threads.
    std::mutex lifecycle_mutex;

    std::mutex                                  reaper_mutex;
    std::condition_variable                     reaper_cv;
    bool                                        reaper_stop = false;
    std::vector<std::shared_ptr<Connection>>    retired;  ///< under reaper_mutex
    std::thread                                 reaper;

    // --- Guard, authentication -------------------------------------------

    /// A refusal that is decided from the headers: the body, if any, is not
    /// read, so the connection must not be reused.
    static void refuse(httplib::Response& response, int status, int rpc_code, std::string message) {
        response.set_header("Connection", "close");
        http_error(response, status, rpc_code, std::move(message));
    }

    httplib::Server::HandlerResponse pre_route(const httplib::Request& request,
                                               httplib::Response&      response) {
        using Verdict = HttpRequestGuard::Verdict;
        const auto host = request.has_header("Host")
                              ? std::optional<std::string>(request.get_header_value("Host"))
                              : std::nullopt;
        switch (guard.check_host(host)) {
            case Verdict::Allowed:
                break;
            case Verdict::HostMissingOrInvalid:
                refuse(response, 400, -32600, "Host header is missing or invalid");
                return httplib::Server::HandlerResponse::Handled;
            default:
                refuse(response, 403, -32003, "Host is not allowed");
                return httplib::Server::HandlerResponse::Handled;
        }
        const auto origin = request.has_header("Origin")
                                ? std::optional<std::string>(request.get_header_value("Origin"))
                                : std::nullopt;
        if (guard.check_origin(origin) != Verdict::Allowed) {
            refuse(response, 403, -32003, "Origin is not allowed");
            return httplib::Server::HandlerResponse::Handled;
        }

        const bool upgrade = is_websocket_upgrade(request);
        t_upgrade_principal.clear();
        if (config.authenticator) {
            std::optional<std::string> principal;
            try {
                principal = config.authenticator(request.get_header_value("Authorization"));
            } catch (...) {
                refuse(response, 503, -32000, "Authentication is unavailable");
                return httplib::Server::HandlerResponse::Handled;
            }
            if (!principal || principal->empty()) {
                response.set_header("WWW-Authenticate", "Bearer");
                refuse(response, 401, -32001, "Authentication is required");
                return httplib::Server::HandlerResponse::Handled;
            }
            response.user_data.set("acp.principal", *principal);
            if (upgrade) t_upgrade_principal = *principal;
        }
        // A body that is announced larger than the limit is never read.
        if (request.has_header("Content-Length")) {
            const auto announced = request.get_header_value("Content-Length");
            const bool digits    = !announced.empty() && announced.size() <= 19 &&
                                announced.find_first_not_of("0123456789") == std::string::npos;
            if (digits && std::stoull(announced) > config.max_payload_bytes) {
                refuse(response, 413, -32600, "The request body is too large");
                return httplib::Server::HandlerResponse::Handled;
            }
        }
        if (upgrade && config.websocket) {
            std::lock_guard lock(connections_mutex);
            if (stopping.load() || connections.size() + creating >= config.max_connections) {
                refuse(response, 503, -32000, "The server has no room for another connection");
                return httplib::Server::HandlerResponse::Handled;
            }
        }
        return httplib::Server::HandlerResponse::Unhandled;
    }

    static std::string principal_of(const httplib::Response& response) {
        const auto* principal = response.user_data.get<std::string>("acp.principal");
        return principal ? *principal : std::string();
    }

    // --- Connections -------------------------------------------------------

    /// Counts a connection being created against max_connections.
    struct Slot {
        explicit Slot(Impl& owner_in) : owner(owner_in) {}
        ~Slot() { release(); }
        Slot(const Slot&)            = delete;
        Slot& operator=(const Slot&) = delete;
        void release() {
            if (!held) return;
            held = false;
            std::lock_guard lock(owner.connections_mutex);
            --owner.creating;
        }
        Impl& owner;
        bool  held = false;
    };

    /// Build the connection and start its agent. `status` is the HTTP status
    /// to answer when it returns null.
    std::shared_ptr<Connection> create_connection(const std::string& principal, bool websocket,
                                                  int& status) {
        Slot slot(*this);
        {
            std::lock_guard lock(connections_mutex);
            if (stopping.load()) {
                status = 503;
                return nullptr;
            }
            if (connections.size() + creating >= config.max_connections) {
                status = 503;
                return nullptr;
            }
            ++creating;
            slot.held = true;
        }
        ACPHttpConnection parts;
        try {
            parts = factory(principal);
        } catch (...) {
            status = 500;
            return nullptr;
        }
        if (!parts.server) {
            status = 500;
            return nullptr;
        }
        auto connection = std::make_shared<Connection>(random_id("conn-"), principal, websocket,
                                                       std::move(parts));
        connection->sink.set_deliver(
            [this, raw = connection.get()](std::string line) { route(*raw, std::move(line)); });
        try {
            connection->runner = std::thread([this, raw = connection.get()] {
                try {
                    raw->parts.server->run(raw->in, raw->out);
                } catch (...) {
                    // The agent ended abnormally; the connection ends with it.
                }
                raw->failed.store(true);
                wake_reaper();
            });
        } catch (...) {
            status = 500;
            return nullptr;
        }
        {
            std::lock_guard lock(connections_mutex);
            if (!stopping.load()) {
                connections[connection->id] = connection;
                --creating;
                slot.held = false;
                return connection;
            }
        }
        status = 503;
        teardown(connection);
        return nullptr;
    }

    std::shared_ptr<Connection> find(const std::string& id, const std::string& principal) {
        std::lock_guard lock(connections_mutex);
        auto            it = connections.find(id);
        if (it == connections.end() || it->second->closing.load() ||
            it->second->principal != principal) {
            return nullptr;
        }
        return it->second;
    }

    /// Take the connection out of service now; the reaper thread ends it
    /// (cancelling its prompts), so no HTTP thread waits for the agent.
    void retire(const std::shared_ptr<Connection>& connection) {
        connection->closing.store(true);
        {
            std::lock_guard lock(connections_mutex);
            connections.erase(connection->id);
        }
        connection->close_boxes();
        {
            std::lock_guard lock(reaper_mutex);
            retired.push_back(connection);
        }
        reaper_cv.notify_all();
    }

    void wake_reaper() { reaper_cv.notify_all(); }

    /// End the connection: close its streams, end the agent's input (its
    /// prompts are cancelled at once) and wait for the agent to stop.
    void teardown(const std::shared_ptr<Connection>& connection) {
        connection->closing.store(true);
        {
            std::lock_guard lock(connections_mutex);
            connections.erase(connection->id);
        }
        registry->release_all(connection.get());
        connection->close_boxes();
        if (connection->parts.server) connection->parts.server->set_eof_grace(0ms);
        connection->source.close();
        std::lock_guard lock(connection->end_mutex);
        if (connection->runner.joinable() &&
            connection->runner.get_id() != std::this_thread::get_id()) {
            connection->runner.join();
        }
    }

    void reaper_loop() {
        for (;;) {
            std::vector<std::shared_ptr<Connection>> ending;
            {
                std::unique_lock lock(reaper_mutex);
                reaper_cv.wait_for(lock, kReaperTick, [this] {
                    return reaper_stop || !retired.empty();
                });
                if (reaper_stop) return;
                ending.swap(retired);
            }
            const auto now = Clock::now();
            {
                std::lock_guard lock(connections_mutex);
                for (const auto& entry : connections) {
                    const auto& connection = entry.second;
                    const bool  idle =
                        !connection->websocket && config.connection_idle_timeout.count() > 0 &&
                        connection->streams.load() == 0 &&
                        now - connection->active_at() >= config.connection_idle_timeout;
                    if (connection->failed.load() || idle) ending.push_back(connection);
                }
            }
            for (const auto& connection : ending) {
                if (connection->websocket && !connection->closing.load()) {
                    // The WebSocket handler owns its connection's end; it sees
                    // `failed` within one read slice.
                    continue;
                }
                teardown(connection);
            }
        }
    }

    // --- Agent -> client: routing -------------------------------------------

    void fail(Connection& connection) {
        connection.failed.store(true);
        wake_reaper();
    }

    void deliver(Connection& connection, const std::shared_ptr<Outbox>& box, std::string line) {
        if (!box->push(std::move(line))) fail(connection);
    }

    void route(Connection& connection, std::string line) {
        if (connection.closing.load()) return;
        json envelope;
        try {
            envelope = json::parse(line);
        } catch (...) {
            return;
        }
        if (!envelope.is_object()) return;

        std::shared_ptr<Outbox> box = connection.connection_box;
        const bool method = envelope.contains("method") && envelope["method"].is_string();
        if (method) {
            // A notification or a request of the agent: the session it names
            // decides the stream. A session without a stream of its own (one
            // that session/resume introduced, until the client opens its
            // stream) is served on the connection stream.
            if (!connection.websocket && envelope.contains("params")) {
                const auto session = string_field(envelope["params"], "sessionId");
                if (!session.empty()) {
                    if (auto own = connection.find_box(session)) box = std::move(own);
                }
            }
        } else if (envelope.contains("id")) {
            const auto                          key = envelope["id"].dump();
            Connection::Route                   route;
            bool                                found = false;
            std::shared_ptr<std::promise<json>> initialize;
            {
                std::lock_guard lock(connection.mutex);
                if (connection.initialize_id && *connection.initialize_id == key) {
                    initialize = std::move(connection.initialize_promise);
                    connection.initialize_promise.reset();
                    connection.initialize_id.reset();
                } else if (auto it = connection.routes.find(key); it != connection.routes.end()) {
                    route = std::move(it->second);
                    connection.routes.erase(it);
                    found = true;
                }
            }
            if (initialize) {
                initialize->set_value(std::move(envelope));
                return;
            }
            if (found) {
                const bool ok = envelope.contains("result");
                if (route.method == "session/new" && ok) {
                    const auto session = string_field(envelope["result"], "sessionId");
                    if (!session.empty()) {
                        connection.know(session);
                        connection.box_for(session);
                        registry->claim(session, connection.shared_from_this());
                    }
                } else if (route.method == "session/resume") {
                    if (ok) {
                        connection.know(route.session);
                    } else if (route.claimed_fresh) {
                        registry->release(route.session, &connection);
                    }
                }
                if (route.session_scoped && !connection.websocket) {
                    if (auto own = connection.find_box(route.session)) box = std::move(own);
                }
            }
        }
        deliver(connection, box, std::move(line));
    }

    // --- Client -> agent -----------------------------------------------------

    struct Admission {
        int         status = 0;  ///< non-zero: refuse with this HTTP status
        int         rpc_code = 0;
        std::string message;
        bool        forward = true;
    };

    /// Check one client message against the connection and record where its
    /// response goes. `session_header` is the Acp-Session-Id header of an
    /// HTTP request (empty when absent), null for a WebSocket message.
    Admission admit(Connection& connection, const json& envelope,
                    const std::string* session_header, std::string& line) {
        line = envelope.dump();
        const bool        has_method = envelope.contains("method") && envelope["method"].is_string();
        const bool        has_id     = envelope.contains("id");
        const std::string method     = has_method ? envelope["method"].get<std::string>() : "";
        const std::string session =
            has_method && envelope.contains("params") ? string_field(envelope["params"], "sessionId")
                                                       : std::string();
        const bool introduces = method == "session/resume";

        if (session_header) {
            if (!session.empty() && !introduces) {
                if (session_header->empty()) {
                    return {400, -32600, "Acp-Session-Id is required for a session request", false};
                }
                if (*session_header != session) {
                    return {400, -32600, "Acp-Session-Id does not match params.sessionId", false};
                }
            }
            if (!session_header->empty()) {
                if (introduces) {
                    if (*session_header != session) {
                        return {400, -32600, "Acp-Session-Id does not match params.sessionId",
                                false};
                    }
                } else if (!connection.knows(*session_header)) {
                    return {404, -32600, "Unknown Acp-Session-Id", false};
                }
            }
        }
        if (!has_method || !has_id) return {};  // a notification or a response

        Connection::Route route;
        route.method = method;
        if (introduces && !session.empty()) {
            route.session = session;
            switch (registry->claim(session, connection.shared_from_this())) {
                case Registry::Claim::Refused: {
                    // The answer an ACPServer gives for a busy session.
                    json refusal = rpc_error(-32603, "session " + session +
                                                         " is held by another connection",
                                             envelope["id"]);
                    refusal["error"]["data"] = json{{"reason", "session_busy"}};
                    deliver(connection, connection.connection_box, refusal.dump());
                    return {0, 0, "", false};
                }
                case Registry::Claim::Fresh:
                    route.claimed_fresh = true;
                    break;
                case Registry::Claim::Held:
                    break;
            }
        } else if (!session.empty() && !connection.websocket) {
            route.session        = session;
            route.session_scoped = true;
        }
        const auto key = envelope["id"].dump();
        bool       too_many = false;
        {
            std::lock_guard lock(connection.mutex);
            if (connection.routes.size() >= kMaxOpenRequests &&
                connection.routes.find(key) == connection.routes.end()) {
                too_many = true;
            } else {
                connection.routes[key] = route;
            }
        }
        if (too_many) {
            if (route.claimed_fresh) registry->release(route.session, &connection);
            return {429, -32000, "Too many requests are waiting for an answer", false};
        }
        return {};
    }

    // --- HTTP routes ---------------------------------------------------------

    void register_routes() {
        if (config.streamable_http) {
            http.Post(config.endpoint,
                      [this](const auto& request, auto& response) { handle_post(request, response); });
            http.Get(config.endpoint,
                     [this](const auto& request, auto& response) { handle_get(request, response); });
            http.Delete(config.endpoint, [this](const auto& request, auto& response) {
                handle_delete(request, response);
            });
        }
        if (config.websocket) {
            http.WebSocket(config.endpoint, [this](const httplib::Request& request,
                                                   httplib::ws::WebSocket&  socket) {
                handle_websocket(request, socket);
            });
        }
    }

    /// The connection a request names, or null after answering 400 or 404.
    std::shared_ptr<Connection> connection_of(const httplib::Request& request,
                                              httplib::Response&      response) {
        const auto id = request.get_header_value("Acp-Connection-Id");
        if (id.empty()) {
            http_error(response, 400, -32600, "Acp-Connection-Id is required");
            return nullptr;
        }
        auto connection = find(id, principal_of(response));
        if (!connection || connection->websocket) {
            http_error(response, 404, -32600, "Unknown Acp-Connection-Id");
            return nullptr;
        }
        return connection;
    }

    void handle_post(const httplib::Request& request, httplib::Response& response) {
        if (stopping.load()) {
            http_error(response, 503, -32000, "The server is stopping");
            return;
        }
        if (!media_type_contains(request.get_header_value("Content-Type"), "application/json")) {
            http_error(response, 415, -32600, "Content-Type must be application/json");
            return;
        }
        json envelope;
        try {
            envelope = json::parse(request.body);
        } catch (...) {
            http_error(response, 400, -32700, "Parse error");
            return;
        }
        if (envelope.is_array()) {
            http_error(response, 501, -32600, "Batch requests are not supported");
            return;
        }
        if (!envelope.is_object()) {
            http_error(response, 400, -32600, "Invalid Request");
            return;
        }

        const bool initialize = envelope.contains("method") && envelope["method"].is_string() &&
                                envelope["method"] == "initialize" && envelope.contains("id");
        if (!request.has_header("Acp-Connection-Id")) {
            if (!initialize) {
                http_error(response, 400, -32600,
                           "Acp-Connection-Id is required after initialize");
                return;
            }
            handle_initialize(response, envelope);
            return;
        }

        auto connection = connection_of(request, response);
        if (!connection) return;
        std::string line;
        const std::string session_header = request.get_header_value("Acp-Session-Id");
        const auto        admission = admit(*connection, envelope, &session_header, line);
        if (admission.status != 0) {
            http_error(response, admission.status, admission.rpc_code, admission.message);
            return;
        }
        connection->touch();
        if (admission.forward && !connection->source.push(std::move(line))) {
            http_error(response, 404, -32600, "Unknown Acp-Connection-Id");
            return;
        }
        response.status = 202;
        response.set_content("", "application/json");
    }

    void handle_initialize(httplib::Response& response, const json& envelope) {
        int  status     = 500;
        auto connection = create_connection(principal_of(response), false, status);
        if (!connection) {
            http_error(response, status, -32000,
                       status == 503 ? "The server has no room for another connection"
                                     : "The connection could not be created");
            return;
        }
        auto promise = std::make_shared<std::promise<json>>();
        auto future  = promise->get_future();
        {
            std::lock_guard lock(connection->mutex);
            connection->initialize_id      = envelope["id"].dump();
            connection->initialize_promise = promise;
        }
        connection->source.push(envelope.dump());
        if (future.wait_for(kInitializeTimeout) != std::future_status::ready) {
            retire(connection);
            http_error(response, 504, -32000, "The agent did not answer initialize");
            return;
        }
        json reply = future.get();
        response.status = 200;
        if (reply.contains("error")) {
            // A refused initialize leaves no connection behind.
            retire(connection);
            response.set_content(reply.dump(), "application/json");
            return;
        }
        connection->touch();
        response.set_header("Acp-Connection-Id", connection->id);
        response.set_content(reply.dump(), "application/json");
    }

    /// Holds a stream's place in the connection and the server until the
    /// stream ends, whether or not its provider ever ran.
    struct StreamLease {
        StreamLease(Impl& owner_in, std::shared_ptr<Connection> connection_in,
                    std::shared_ptr<Outbox> box_in, std::uint64_t generation_in)
            : owner(owner_in),
              connection(std::move(connection_in)),
              box(std::move(box_in)),
              generation(generation_in) {
            connection->streams.fetch_add(1);
        }
        ~StreamLease() { release(); }
        StreamLease(const StreamLease&)            = delete;
        StreamLease& operator=(const StreamLease&) = delete;
        void release() {
            if (released.exchange(true)) return;
            connection->streams.fetch_sub(1);
            connection->touch();
            owner.open_streams.fetch_sub(1);
        }
        Impl&                       owner;
        std::shared_ptr<Connection> connection;
        std::shared_ptr<Outbox>     box;
        std::uint64_t               generation;
        std::atomic<bool>           released{false};
    };

    void handle_get(const httplib::Request& request, httplib::Response& response) {
        if (stopping.load()) {
            http_error(response, 503, -32000, "The server is stopping");
            return;
        }
        if (!media_type_contains(request.get_header_value("Accept"), "text/event-stream")) {
            http_error(response, 406, -32600, "Accept must include text/event-stream");
            return;
        }
        auto connection = connection_of(request, response);
        if (!connection) return;
        std::shared_ptr<Outbox> box = connection->connection_box;
        if (request.has_header("Acp-Session-Id")) {
            const auto session = request.get_header_value("Acp-Session-Id");
            if (!connection->knows(session)) {
                http_error(response, 404, -32600, "Unknown Acp-Session-Id");
                return;
            }
            box = connection->box_for(session);
        }
        if (open_streams.fetch_add(1) >= max_streams) {
            open_streams.fetch_sub(1);
            http_error(response, 503, -32000, "The server has no room for another stream");
            return;
        }
        connection->touch();
        const auto lease =
            std::make_shared<StreamLease>(*this, connection, box, box->attach());
        const auto keepalive = std::chrono::duration_cast<Clock::duration>(config.keepalive_interval);
        response.set_header("Cache-Control", "no-store");
        response.set_header("X-Accel-Buffering", "no");
        response.set_chunked_content_provider(
            "text/event-stream",
            [this, lease, keepalive, first = true, last_sent = Clock::now()](
                std::size_t, httplib::DataSink& sink) mutable -> bool {
                // The opening comment tells the client that the stream is
                // attached before any message is due.
                static constexpr std::string_view kOpen      = ": open\n\n";
                static constexpr std::string_view kKeepAlive = ": keep-alive\n\n";
                if (first) {
                    first = false;
                    if (!sink.write(kOpen.data(), kOpen.size())) return false;
                    last_sent = Clock::now();
                }
                std::string message;
                switch (lease->box->next(lease->generation, kStreamSlice, message)) {
                    case Outbox::Next::Item: {
                        const auto frame = "data: " + message + "\n\n";
                        if (!sink.write(frame.data(), frame.size())) return false;
                        last_sent = Clock::now();
                        return true;
                    }
                    case Outbox::Next::Idle:
                        if (stopping.load()) {
                            sink.done();
                            return true;
                        }
                        if (Clock::now() - last_sent >= keepalive) {
                            if (!sink.write(kKeepAlive.data(), kKeepAlive.size())) return false;
                            last_sent = Clock::now();
                        }
                        return true;
                    case Outbox::Next::Closed:
                    case Outbox::Next::Superseded:
                        sink.done();
                        return true;
                }
                return false;
            },
            [lease](bool) { lease->release(); });
    }

    void handle_delete(const httplib::Request& request, httplib::Response& response) {
        auto connection = connection_of(request, response);
        if (!connection) return;
        retire(connection);
        response.status = 202;
        response.set_content("", "application/json");
    }

    // --- WebSocket -------------------------------------------------------------

    void send_error(Connection& connection, int code, const std::string& message,
                    const json& id = nullptr) {
        deliver(connection, connection.connection_box, rpc_error(code, message, id).dump());
    }

    void handle_websocket_message(Connection& connection, const std::string& text) {
        json envelope;
        try {
            envelope = json::parse(text);
        } catch (...) {
            send_error(connection, -32700, "Parse error");
            return;
        }
        if (envelope.is_array()) {
            send_error(connection, -32600, "Batch requests are not supported");
            return;
        }
        if (!envelope.is_object()) {
            send_error(connection, -32600, "Invalid Request");
            return;
        }
        std::string line;
        const auto  admission = admit(connection, envelope, nullptr, line);
        if (admission.status != 0) {
            send_error(connection, admission.rpc_code, admission.message,
                       envelope.contains("id") ? envelope["id"] : json());
            return;
        }
        connection.touch();
        if (admission.forward) connection.source.push(std::move(line));
    }

    void handle_websocket(const httplib::Request&, httplib::ws::WebSocket& socket) {
        const auto principal = std::exchange(t_upgrade_principal, std::string());
        if (open_streams.fetch_add(1) >= max_streams) {
            open_streams.fetch_sub(1);
            socket.close(static_cast<httplib::ws::CloseStatus>(kTryAgainLater),
                         "no room for another connection");
            return;
        }
        struct StreamCount {
            Impl& owner;
            ~StreamCount() { owner.open_streams.fetch_sub(1); }
        } stream_count{*this};

        int  status     = 500;
        auto connection = create_connection(principal, true, status);
        if (!connection) {
            socket.close(status == 503 ? static_cast<httplib::ws::CloseStatus>(kTryAgainLater)
                                       : httplib::ws::CloseStatus::InternalError,
                         "the connection could not be created");
            return;
        }
        socket.set_read_timeout(kWebSocketSlice);

        // How the connection ends when this side ends it. The writer sends the
        // Close frame while the reader below is still inside read(): a close()
        // from a thread that is not reading would wait for the peer's answer,
        // and one that is reading does not.
        std::mutex                 close_mutex;
        httplib::ws::CloseStatus   close_status = httplib::ws::CloseStatus::GoingAway;
        std::string                close_reason = "server stopping";
        const auto end_connection = [&](httplib::ws::CloseStatus status, std::string reason) {
            {
                std::lock_guard lock(close_mutex);
                close_status = status;
                close_reason = std::move(reason);
            }
            connection->connection_box->close();  // wakes the writer
        };

        // Messages the agent writes are sent from this thread, so the agent
        // never waits for a slow peer.
        std::thread writer([&] {
            const auto generation = connection->connection_box->attach();
            std::string message;
            for (;;) {
                switch (connection->connection_box->next(generation, 250ms, message)) {
                    case Outbox::Next::Item:
                        if (!socket.send(message)) {
                            connection->failed.store(true);
                            return;
                        }
                        break;
                    case Outbox::Next::Idle:
                        break;
                    case Outbox::Next::Closed:
                    case Outbox::Next::Superseded: {
                        std::lock_guard lock(close_mutex);
                        socket.close(close_status, close_reason);
                        return;
                    }
                }
            }
        });

        std::optional<Clock::time_point> ending;
        std::string                      message;
        for (;;) {
            if (!ending && (stopping.load() || connection->closing.load() || connection->failed.load())) {
                end_connection(httplib::ws::CloseStatus::GoingAway, "server stopping");
                ending = Clock::now();
            }
            // After this side has sent its Close, the peer answers within a
            // read slice or the connection is dropped.
            if (ending && Clock::now() - *ending >= kWebSocketSlice) break;
            const auto result = socket.read(message);
            if (result == httplib::ws::Timeout) continue;
            if (result == httplib::ws::Fail) break;
            if (ending || result == httplib::ws::Binary) continue;  // ignored, per the draft
            if (message.size() > config.max_payload_bytes) {
                end_connection(httplib::ws::CloseStatus::MessageTooBig, "message too large");
                ending = Clock::now();
                continue;
            }
            handle_websocket_message(*connection, message);
        }
        teardown(connection);  // closes the box, which ends the writer
        writer.join();
    }

    // --- Lifecycle ---------------------------------------------------------------

    bool bind() {
        if (config.port == 0) {
            bound_port = http.bind_to_any_port(config.host);
            if (bound_port < 0) return false;
        } else {
            if (!http.bind_to_port(config.host, config.port)) return false;
            bound_port = config.port;
        }
        guard.bind(config.host, bound_port);
        return true;
    }

    void start_reaper() {
        {
            std::lock_guard lock(reaper_mutex);
            reaper_stop = false;
        }
        reaper = std::thread([this] { reaper_loop(); });
    }

    void stop_reaper() {
        {
            std::lock_guard lock(reaper_mutex);
            reaper_stop = true;
        }
        reaper_cv.notify_all();
        if (reaper.joinable()) reaper.join();
    }

    void end_connections() {
        std::vector<std::shared_ptr<Connection>> all;
        {
            std::lock_guard lock(connections_mutex);
            for (auto& entry : connections) all.push_back(entry.second);
        }
        {
            std::lock_guard lock(reaper_mutex);
            for (auto& connection : retired) all.push_back(std::move(connection));
            retired.clear();
        }
        for (const auto& connection : all) {
            connection->closing.store(true);
            connection->close_boxes();
        }
        for (const auto& connection : all) teardown(connection);
    }
};

// ---------------------------------------------------------------------------
// ACPHttpServer
// ---------------------------------------------------------------------------
namespace {

void bind_to_registry(const std::shared_ptr<Registry>& registry,
                      const std::shared_ptr<ACPClient>& client) {
    client->bind(ACPClient::ServerResolver(
        [registry](std::string_view session) { return registry->resolve(session); }));
}

}  // namespace

ACPHttpServer::ACPHttpServer(ConnectionFactory factory, ACPHttpConfig config)
    : impl_(std::make_unique<Impl>(std::move(factory), std::move(config))) {}

ACPHttpServer::~ACPHttpServer() {
    stop();
}

bool ACPHttpServer::start() {
    if (!impl_->bind()) return false;
    impl_->stopping.store(false);
    impl_->running.store(true, std::memory_order_release);
    impl_->start_reaper();
    const bool ok = impl_->http.listen_after_bind();
    impl_->running.store(false, std::memory_order_release);
    stop();
    return ok;
}

bool ACPHttpServer::start_async() {
    if (!impl_->bind()) return false;
    impl_->stopping.store(false);
    impl_->running.store(true, std::memory_order_release);
    impl_->start_reaper();
    impl_->listener = std::thread([this] {
        impl_->http.listen_after_bind();
        impl_->running.store(false, std::memory_order_release);
    });
    impl_->http.wait_until_ready();
    return impl_->http.is_running();
}

void ACPHttpServer::stop() {
    std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->stopping.store(true);
    // End the open connections first, so that the streams and WebSocket
    // handlers the HTTP server waits for finish.
    impl_->end_connections();
    if (impl_->http.is_running()) impl_->http.stop();
    if (impl_->listener.joinable()) impl_->listener.join();
    impl_->running.store(false, std::memory_order_release);
    impl_->stop_reaper();
    impl_->end_connections();
}

bool ACPHttpServer::is_running() const {
    return impl_->running.load(std::memory_order_acquire);
}

int ACPHttpServer::port() const {
    return impl_->bound_port;
}

std::size_t ACPHttpServer::connection_count() const {
    std::lock_guard lock(impl_->connections_mutex);
    return impl_->connections.size();
}

std::shared_ptr<ACPClient> ACPHttpServer::client() {
    std::lock_guard lock(impl_->client_mutex);
    if (!impl_->client_handle) {
        impl_->client_handle = std::make_shared<ACPClient>();
        bind_to_registry(impl_->registry, impl_->client_handle);
    }
    return impl_->client_handle;
}

void ACPHttpServer::attach_client(std::shared_ptr<ACPClient> client) {
    if (!client) return;
    std::lock_guard lock(impl_->client_mutex);
    bind_to_registry(impl_->registry, client);
    impl_->client_handle = std::move(client);
}

}  // namespace neograph::acp
