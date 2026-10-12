#include <neograph/mcp/http_server.h>

// cpp-httplib v0.41.0 uses blocking Server handlers, bounded below with its
// runtime ThreadPool. Keep the TLS macro consistent with every NeoGraph TU that
// instantiates httplib types to avoid the macro-gated layout ODR trap.
// Source: https://github.com/yhirose/cpp-httplib/blob/v0.41.0/README.md
// Verified against the vendored header on 2026-07-21.
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <iomanip>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neograph::mcp {
namespace {

// An HTTP error body is a JSON-RPC error response with the request's id when
// it was read and no id otherwise, never a null id (MCP-S-07).
json rpc_error(int code, std::string message, json id = nullptr) {
    json response = {
        {"jsonrpc", "2.0"},
        {"error", {{"code", code}, {"message", std::move(message)}}},
    };
    if (!id.is_null()) response["id"] = std::move(id);
    return response;
}

bool is_loopback_bind(const std::string& host) {
    return host == "127.0.0.1" || host == "::1" || host == "localhost";
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool media_type_contains(const std::string& header, const std::string& expected) {
    std::size_t start = 0;
    while (start < header.size()) {
        auto end = header.find(',', start);
        if (end == std::string::npos) end = header.size();
        auto token = header.substr(start, end - start);
        if (const auto parameters = token.find(';'); parameters != std::string::npos) {
            token.erase(parameters);
        }
        const auto first = token.find_first_not_of(" \t");
        const auto last  = token.find_last_not_of(" \t");
        if (first != std::string::npos &&
            lower(token.substr(first, last - first + 1)) == lower(expected)) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

std::string request_key(const json& id) {
    if (id.is_string()) return "s:" + id.get<std::string>();
    return "n:" + id.dump();
}

std::string fresh_session_id() {
    std::random_device random;
    std::ostringstream out;
    out << "ng_" << std::hex << std::setfill('0');
    for (int i = 0; i < 8; ++i) {
        out << std::setw(8) << static_cast<std::uint32_t>(random());
    }
    return out.str();
}

void http_error(
    httplib::Response& response, int status, int rpc_code, std::string message, json id = nullptr) {
    response.status = status;
    response.set_content(rpc_error(rpc_code, std::move(message), std::move(id)).dump(),
                         "application/json");
}

// cpp-httplib's ThreadPool decides whether to start a thread from a count of
// idle threads that lags behind the jobs already handed to them: connections
// that arrive together can wait for one thread while it serves a tool call for
// minutes, so a notifications/cancelled sat behind the calls it should stop
// (MCP-S-10). This queue starts a thread whenever the jobs waiting and running
// outnumber the threads, up to `max` threads, which then stay.
class ElasticTaskQueue final : public httplib::TaskQueue {
public:
    ElasticTaskQueue(std::size_t base, std::size_t max, std::size_t max_queued)
        : max_(std::max(base, max)), max_queued_(max_queued) {
        std::lock_guard lock(mutex_);
        for (std::size_t i = 0; i < base; ++i) spawn_locked();
    }
    ~ElasticTaskQueue() override { shutdown(); }

    bool enqueue(std::function<void()> job) override {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return false;
            if (max_queued_ != 0 && jobs_.size() >= max_queued_) return false;
            jobs_.push_back(std::move(job));
            if (jobs_.size() + busy_ > threads_.size() && threads_.size() < max_) spawn_locked();
        }
        ready_.notify_one();
        return true;
    }

    void shutdown() override {
        std::vector<std::thread> threads;
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            threads.swap(threads_);
        }
        ready_.notify_all();
        for (auto& thread : threads) {
            if (thread.joinable()) thread.join();
        }
    }

private:
    void spawn_locked() {
        threads_.emplace_back([this] { work(); });
    }

    void work() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                if (jobs_.empty()) return;  // stopping, and every job ran
                job = std::move(jobs_.front());
                jobs_.pop_front();
                ++busy_;
            }
            job();
            std::lock_guard lock(mutex_);
            --busy_;
        }
    }

    const std::size_t                 max_;
    const std::size_t                 max_queued_;
    std::mutex                        mutex_;
    std::condition_variable           ready_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::thread>          threads_;
    std::size_t                       busy_     = 0;
    bool                              stopping_ = false;
};

}  // namespace

struct MCPHttpServer::Impl {
    using Clock = std::chrono::steady_clock;

    struct Session {
        std::string                                                          scope;
        std::shared_ptr<void>                                                owner;
        std::unique_ptr<MCPServer>                                           server;
        std::mutex                                                           dispatch_mutex;
        std::mutex                                                           pending_mutex;
        std::unordered_map<std::string, std::shared_ptr<std::promise<json>>> pending;
        /// When a request for this session last started or ended.
        std::atomic<Clock::rep> last_used{Clock::now().time_since_epoch().count()};
        /// Requests being handled now. A busy session never expires.
        std::atomic<int> in_flight{0};

        void touch() noexcept { last_used.store(Clock::now().time_since_epoch().count()); }
        Clock::time_point used_at() const noexcept {
            return Clock::time_point(Clock::duration(last_used.load()));
        }

        ~Session() {
            if (server) server->stop();
        }
    };

    // Marks a session busy for the duration of one request.
    struct SessionUse {
        explicit SessionUse(std::shared_ptr<Session> value) : session(std::move(value)) {
            if (session) {
                session->in_flight.fetch_add(1);
                session->touch();
            }
        }
        ~SessionUse() {
            if (session) {
                session->touch();
                session->in_flight.fetch_sub(1);
            }
        }
        SessionUse(const SessionUse&)            = delete;
        SessionUse& operator=(const SessionUse&) = delete;
        std::shared_ptr<Session> session;
    };

    Impl(ServerFactory value_factory, MCPHttpServerConfig value_config)
        : factory(std::move(value_factory)), config(std::move(value_config)) {
        if (!factory) throw std::invalid_argument("MCPHttpServer factory is required");
        if (config.endpoint.empty() || config.endpoint.front() != '/' ||
            config.endpoint.find_first_of("\r\n?#") != std::string::npos) {
            throw std::invalid_argument("MCP HTTP endpoint must be an absolute path");
        }
        if (config.port < 0 || config.port > 65535 || config.max_sessions == 0 ||
            config.max_payload_bytes == 0 || config.http_threads == 0 ||
            config.max_http_threads < config.http_threads ||
            config.max_queued_requests == 0 || config.response_timeout.count() <= 0 ||
            config.session_idle_timeout.count() < 0) {
            throw std::invalid_argument("MCP HTTP limits and port must be valid");
        }
        if (!is_loopback_bind(config.host) && !config.bearer_authorizer) {
            throw std::invalid_argument("remote MCP HTTP binds require a bearer authorizer");
        }

        http.set_payload_max_length(config.max_payload_bytes);
        // A request holds an HTTP thread until it is answered, and a tools/call
        // until its tool returns: http_threads start at once, and more start
        // on demand up to max_http_threads (MCP-S-10).
        http.new_task_queue = [threads = config.http_threads, max = config.max_http_threads,
                               queued = config.max_queued_requests] {
            return new ElasticTaskQueue(threads, max, queued);
        };
        register_routes();
    }

    ServerFactory       factory;
    MCPHttpServerConfig config;
    httplib::Server     http;
    std::thread         listener;
    std::atomic<bool>   running{false};
    int                 bound_port = 0;

    std::mutex                                      sessions_mutex;
    std::map<std::string, std::shared_ptr<Session>> sessions;
    std::size_t                                     creating_sessions = 0;

    // Closes idle sessions in the background (MCP-S-02).
    std::mutex              reaper_mutex;
    std::condition_variable reaper_cv;
    bool                    reaper_stop = false;
    std::thread             reaper;

    std::optional<std::string> authorize(const httplib::Request& request,
                                         httplib::Response&      response) const {
        if (!validate_origin(request, response)) return std::nullopt;

        if (!config.bearer_authorizer) return std::string{};
        const auto                 authorization = request.get_header_value("Authorization");
        constexpr std::string_view prefix        = "Bearer ";
        if (authorization.size() <= prefix.size() ||
            authorization.compare(0, prefix.size(), prefix) != 0) {
            response.set_header("WWW-Authenticate", "Bearer");
            http_error(response, 401, -32001, "Bearer authentication is required");
            return std::nullopt;
        }
        std::optional<std::string> scope;
        try {
            scope = config.bearer_authorizer(std::string_view(authorization).substr(prefix.size()));
        } catch (...) {
            http_error(response, 503, -32000, "Bearer authorization is unavailable");
            return std::nullopt;
        }
        if (!scope || scope->empty()) {
            response.set_header("WWW-Authenticate", "Bearer error=\"invalid_token\"");
            http_error(response, 401, -32001, "Bearer token is invalid");
            return std::nullopt;
        }
        return scope;
    }

    bool validate_origin(const httplib::Request& request, httplib::Response& response) const {
        if (request.has_header("Origin")) {
            const auto origin = request.get_header_value("Origin");
            if (std::find(config.allowed_origins.begin(), config.allowed_origins.end(), origin) ==
                config.allowed_origins.end()) {
                http_error(response, 403, -32003, "Origin is not allowed");
                return false;
            }
        }
        return true;
    }

    bool expired(const Session& session, Clock::time_point now) const {
        return config.session_idle_timeout.count() > 0 && session.in_flight.load() == 0 &&
               now - session.used_at() >= config.session_idle_timeout;
    }

    // A session the server closed answers 404, which tells the client to
    // initialize a new one (MCP transports, "Session Management").
    std::shared_ptr<Session> find_session(const std::string& id) {
        std::shared_ptr<Session> stale;
        {
            std::lock_guard lock(sessions_mutex);
            auto            it = sessions.find(id);
            if (it == sessions.end()) return nullptr;
            if (!expired(*it->second, Clock::now())) return it->second;
            stale = std::move(it->second);
            sessions.erase(it);
        }
        stale->server->stop();
        return nullptr;
    }

    // Remove the sessions idle past session_idle_timeout and, when `room` is
    // needed and none is left, the least recently used session with no
    // request in flight. Callers stop the returned sessions outside the lock.
    std::vector<std::shared_ptr<Session>> reclaim_locked(bool room) {
        std::vector<std::shared_ptr<Session>> removed;
        const auto now = Clock::now();
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (expired(*it->second, now)) {
                removed.push_back(std::move(it->second));
                it = sessions.erase(it);
            } else {
                ++it;
            }
        }
        if (room && sessions.size() + creating_sessions >= config.max_sessions) {
            auto oldest = sessions.end();
            for (auto it = sessions.begin(); it != sessions.end(); ++it) {
                if (it->second->in_flight.load() != 0) continue;
                if (oldest == sessions.end() ||
                    it->second->used_at() < oldest->second->used_at())
                    oldest = it;
            }
            if (oldest != sessions.end()) {
                removed.push_back(std::move(oldest->second));
                sessions.erase(oldest);
            }
        }
        return removed;
    }

    void reap_idle_sessions() {
        std::vector<std::shared_ptr<Session>> removed;
        {
            std::lock_guard lock(sessions_mutex);
            removed = reclaim_locked(false);
        }
        for (const auto& session : removed) session->server->stop();
    }

    void start_reaper() {
        if (config.session_idle_timeout.count() <= 0 || reaper.joinable()) return;
        {
            std::lock_guard lock(reaper_mutex);
            reaper_stop = false;
        }
        const auto period = std::clamp<std::chrono::milliseconds>(
            config.session_idle_timeout / 4, std::chrono::milliseconds(250),
            std::chrono::milliseconds(60000));
        reaper = std::thread([this, period] {
            std::unique_lock lock(reaper_mutex);
            while (!reaper_cv.wait_for(lock, period, [this] { return reaper_stop; })) {
                lock.unlock();
                reap_idle_sessions();
                lock.lock();
            }
        });
    }

    void stop_reaper() {
        {
            std::lock_guard lock(reaper_mutex);
            reaper_stop = true;
        }
        reaper_cv.notify_all();
        if (reaper.joinable()) reaper.join();
    }

    std::pair<std::string, std::shared_ptr<Session>> create_session(std::string scope) {
        std::vector<std::shared_ptr<Session>> reclaimed;
        {
            std::lock_guard lock(sessions_mutex);
            if (sessions.size() + creating_sessions >= config.max_sessions)
                reclaimed = reclaim_locked(true);
            if (sessions.size() + creating_sessions >= config.max_sessions) {
                throw std::runtime_error("MCP HTTP session capacity is exhausted");
            }
            ++creating_sessions;
        }
        for (const auto& session : reclaimed) session->server->stop();

        bool reserved = true;
        try {
            auto built      = factory(scope);
            auto session    = std::make_shared<Session>();
            // Busy until its initialize is answered, so it cannot be evicted
            // for another client before it was used.
            session->in_flight.store(1);
            session->scope  = std::move(scope);
            session->server = std::move(built.server);
            session->owner  = std::move(built.owner);
            if (!session->server) {
                throw std::runtime_error("MCP HTTP server factory returned null");
            }

            std::weak_ptr<Session> weak = session;
            session->server->set_response_sink([weak](const json& envelope) {
                auto current = weak.lock();
                if (!current || !envelope.contains("id")) return;
                std::shared_ptr<std::promise<json>> promise;
                {
                    std::lock_guard lock(current->pending_mutex);
                    auto            it = current->pending.find(request_key(envelope["id"]));
                    if (it == current->pending.end()) return;
                    promise = std::move(it->second);
                    current->pending.erase(it);
                }
                promise->set_value(envelope);
            });

            std::lock_guard lock(sessions_mutex);
            std::string     id;
            do {
                id = fresh_session_id();
            } while (sessions.find(id) != sessions.end());
            sessions.emplace(id, session);
            --creating_sessions;
            reserved = false;
            return {std::move(id), std::move(session)};
        } catch (...) {
            if (reserved) {
                std::lock_guard lock(sessions_mutex);
                --creating_sessions;
            }
            throw;
        }
    }

    bool erase_session(const std::string& id) {
        std::shared_ptr<Session> removed;
        {
            std::lock_guard lock(sessions_mutex);
            auto            it = sessions.find(id);
            if (it == sessions.end()) return false;
            removed = std::move(it->second);
            sessions.erase(it);
        }
        removed->server->stop();
        return true;
    }

    // MCP transports, "Protocol Version Header": a request after initialize
    // carries the negotiated revision. A missing header means the negotiated
    // one (MCP-S-08; 2025-03-26 clients never send it); any other value is
    // 400 Bad Request.
    bool validate_protocol_header(const httplib::Request& request, httplib::Response& response,
                                  const Session& session) const {
        if (!request.has_header("MCP-Protocol-Version")) return true;
        const auto negotiated = session.server->protocol_version();
        const auto header     = request.get_header_value("MCP-Protocol-Version");
        if (header != (negotiated.empty() ? std::string(MCP_PROTOCOL_VERSION) : negotiated)) {
            http_error(response, 400, -32600,
                       "MCP-Protocol-Version must be the negotiated revision " +
                           (negotiated.empty() ? std::string(MCP_PROTOCOL_VERSION) : negotiated));
            return false;
        }
        return true;
    }

    void handle_post(const httplib::Request& request, httplib::Response& response) {
        auto scope = authorize(request, response);
        if (!scope) return;
        if (!media_type_contains(request.get_header_value("Content-Type"), "application/json")) {
            http_error(response, 415, -32600, "Content-Type must be application/json");
            return;
        }
        const auto accept = request.get_header_value("Accept");
        if (!media_type_contains(accept, "application/json") ||
            !media_type_contains(accept, "text/event-stream")) {
            http_error(response, 406, -32600,
                       "Accept must include application/json and text/event-stream");
            return;
        }

        json envelope;
        try {
            envelope = json::parse(request.body);
        } catch (const std::exception&) {
            http_error(response, 400, -32700, "Parse error");
            return;
        }
        if (!envelope.is_object()) {
            http_error(response, 400, -32600, "JSON-RPC body must be an object");
            return;
        }

        const bool has_method = envelope.contains("method") && envelope["method"].is_string();
        const bool initialize = has_method && envelope["method"] == "initialize";
        const auto session_id = request.get_header_value("Mcp-Session-Id");
        std::shared_ptr<Session> session;
        std::string              response_session_id;

        if (initialize && !session_id.empty()) {
            http_error(response, 400, -32600, "initialize must not include Mcp-Session-Id");
            return;
        }
        if (session_id.empty()) {
            if (!initialize) {
                http_error(response, 400, -32600,
                           "Mcp-Session-Id is required after initialization");
                return;
            }
            try {
                auto created        = create_session(*scope);
                response_session_id = std::move(created.first);
                session             = std::move(created.second);
            } catch (const std::exception& error) {
                // Every session is busy with a request.
                response.set_header("Retry-After", "1");
                http_error(response, 503, -32000, error.what());
                return;
            }
        } else {
            session = find_session(session_id);
            if (!session) {
                http_error(response, 404, -32004, "MCP session was not found");
                return;
            }
            if (session->scope != *scope) {
                http_error(response, 403, -32003,
                           "MCP session belongs to a different authorization scope");
                return;
            }
            if (!validate_protocol_header(request, response, *session)) return;
        }

        const SessionUse use(session);
        if (!response_session_id.empty()) session->in_flight.fetch_sub(1);
        if (!has_method) {
            response.status = 202;
            return;
        }

        const bool                          has_id = envelope.contains("id");
        std::shared_ptr<std::promise<json>> promise;
        std::future<json>                   future;
        std::string                         key;
        bool                                owns_waiter = false;
        if (has_id) {
            key     = request_key(envelope["id"]);
            promise = std::make_shared<std::promise<json>>();
            future  = promise->get_future();
            std::lock_guard lock(session->pending_mutex);
            owns_waiter = session->pending.emplace(key, promise).second;
        }
        if (has_id && !owns_waiter) {
            response.status = 200;
            response.set_content(
                rpc_error(-32600, "MCP request ID is already in flight", envelope["id"]).dump(),
                "application/json");
            return;
        }

        json immediate;
        {
            std::lock_guard lock(session->dispatch_mutex);
            immediate = session->server->handle_message(envelope);
        }
        if (!response_session_id.empty()) {
            if (!immediate.contains("result")) {
                erase_session(response_session_id);
            } else {
                response.set_header("Mcp-Session-Id", response_session_id);
            }
        }

        if (!immediate.is_null()) {
            if (owns_waiter) {
                std::lock_guard lock(session->pending_mutex);
                session->pending.erase(key);
            }
            response.status = 200;
            response.set_content(immediate.dump(), "application/json");
            return;
        }
        if (!has_id) {
            response.status = 202;
            return;
        }
        if (future.wait_for(config.response_timeout) != std::future_status::ready) {
            {
                std::lock_guard lock(session->pending_mutex);
                session->pending.erase(key);
            }
            // Nobody waits for the answer any more: stop the tool too, as
            // the client would with notifications/cancelled (MCP-S-16).
            {
                std::lock_guard lock(session->dispatch_mutex);
                session->server->handle_message(
                    {{"jsonrpc", "2.0"},
                     {"method", "notifications/cancelled"},
                     {"params", {{"requestId", envelope["id"]}, {"reason", "response timed out"}}}});
            }
            http_error(response, 504, -32001, "MCP tool response timed out", envelope["id"]);
            return;
        }
        const json answer = future.get();
        if (answer.contains("error") && answer["error"].is_object() &&
            answer["error"].value("code", 0) == -32800) {
            // The client cancelled this call: MCP asks for no response to it,
            // and the official clients report one for an id they gave up as
            // an error. The POST still needs an answer, so it gets an event
            // stream that ends without one.
            response.status = 200;
            response.set_content(std::string(), "text/event-stream");
            return;
        }
        response.status = 200;
        response.set_content(answer.dump(), "application/json");
    }

    void handle_get(const httplib::Request& request, httplib::Response& response) const {
        if (!validate_origin(request, response)) return;
        response.status = 405;
        response.set_header("Allow", "POST, DELETE");
    }

    void handle_delete(const httplib::Request& request, httplib::Response& response) {
        auto scope = authorize(request, response);
        if (!scope) return;
        const auto session_id = request.get_header_value("Mcp-Session-Id");
        auto       session    = find_session(session_id);
        if (!session) {
            http_error(response, 404, -32004, "MCP session was not found");
            return;
        }
        if (session->scope != *scope) {
            http_error(response, 403, -32003,
                       "MCP session belongs to a different authorization scope");
            return;
        }
        if (!validate_protocol_header(request, response, *session)) return;
        erase_session(session_id);
        response.status = 204;
        // A DELETE may arrive with Content-Type but no body (the official
        // Python client sends one); closing keeps the next request from being
        // read as its body (MCP-S-17).
        response.set_header("Connection", "close");
    }

    void register_routes() {
        http.Post(config.endpoint,
                  [this](const auto& request, auto& response) { handle_post(request, response); });
        http.Get(config.endpoint,
                 [this](const auto& request, auto& response) { handle_get(request, response); });
        http.Delete(config.endpoint, [this](const auto& request, auto& response) {
            handle_delete(request, response);
        });
    }

    bool bind() {
        if (config.port == 0) {
            bound_port = http.bind_to_any_port(config.host);
            return bound_port >= 0;
        }
        if (!http.bind_to_port(config.host, config.port)) return false;
        bound_port = config.port;
        return true;
    }

    void stop_sessions() {
        std::vector<std::shared_ptr<Session>> active;
        {
            std::lock_guard lock(sessions_mutex);
            for (auto& [id, session] : sessions) {
                (void)id;
                active.push_back(std::move(session));
            }
            sessions.clear();
        }
        for (const auto& session : active)
            session->server->stop();
    }
};

MCPHttpServer::MCPHttpServer(ServerFactory factory, MCPHttpServerConfig config)
    : impl_(std::make_unique<Impl>(std::move(factory), std::move(config))) {}

MCPHttpServer::~MCPHttpServer() {
    stop();
}

bool MCPHttpServer::start() {
    if (!impl_->bind()) return false;
    impl_->running.store(true, std::memory_order_release);
    impl_->start_reaper();
    const bool ok = impl_->http.listen_after_bind();
    impl_->running.store(false, std::memory_order_release);
    impl_->stop_reaper();
    impl_->stop_sessions();
    return ok;
}

bool MCPHttpServer::start_async() {
    if (!impl_->bind()) return false;
    impl_->running.store(true, std::memory_order_release);
    impl_->start_reaper();
    impl_->listener = std::thread([this] {
        impl_->http.listen_after_bind();
        impl_->running.store(false, std::memory_order_release);
    });
    impl_->http.wait_until_ready();
    return impl_->http.is_running();
}

void MCPHttpServer::stop() {
    if (impl_->http.is_running()) impl_->http.stop();
    if (impl_->listener.joinable()) impl_->listener.join();
    impl_->running.store(false, std::memory_order_release);
    impl_->stop_reaper();
    impl_->stop_sessions();
}

bool MCPHttpServer::is_running() const {
    return impl_->running.load(std::memory_order_acquire);
}

int MCPHttpServer::port() const {
    return impl_->bound_port;
}

}  // namespace neograph::mcp
