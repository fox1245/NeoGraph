#include <neograph/acp/server.h>

#include <neograph/graph/types.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

namespace neograph::acp {

// ---------------------------------------------------------------------------
// ACPGraphAdapter — defaults
// ---------------------------------------------------------------------------
std::string
ACPGraphAdapter::extract_user_text(const std::vector<ContentBlock>& blocks) const {
    std::string out;
    for (auto& b : blocks) {
        if (b.type == "text") {
            if (!out.empty()) out.push_back(' ');
            out.append(b.text);
        }
    }
    return out;
}

neograph::json
ACPGraphAdapter::build_initial_state(
    const std::vector<ContentBlock>& blocks,
    const std::string& session_id) const {
    neograph::json state = neograph::json::object();
    state[input_channel()] = extract_user_text(blocks);
    state["_acp_session_id"] = session_id;
    return state;
}

std::string
ACPGraphAdapter::extract_agent_text(const neograph::json& output) const {
    if (output.contains("channels")) {
        auto ch = output["channels"];
        if (ch.contains(output_channel())
            && ch[output_channel()].contains("value")) {
            auto v = ch[output_channel()]["value"];
            if (v.is_string()) return v.get<std::string>();
            return v.dump();
        }
        if (ch.contains("messages") && ch["messages"].contains("value")) {
            auto msgs = ch["messages"]["value"];
            if (msgs.is_array() && !msgs.empty()) {
                auto last = msgs[msgs.size() - 1];
                if (last.is_object() && last.contains("content")) {
                    auto c = last["content"];
                    if (c.is_string()) return c.get<std::string>();
                    return c.dump();
                }
            }
        }
    }
    return "";
}

// ---------------------------------------------------------------------------
// JSON-RPC envelope helpers
// ---------------------------------------------------------------------------
namespace {

neograph::json jsonrpc_error(int code, std::string msg, const neograph::json& id) {
    neograph::json env;
    env["jsonrpc"] = "2.0";
    env["id"]      = id;
    neograph::json e;
    e["code"]    = code;
    e["message"] = std::move(msg);
    env["error"] = std::move(e);
    return env;
}

neograph::json jsonrpc_result(neograph::json result, const neograph::json& id) {
    neograph::json env;
    env["jsonrpc"] = "2.0";
    env["id"]      = id;
    env["result"]  = std::move(result);
    return env;
}

neograph::json jsonrpc_notify(std::string method, neograph::json params) {
    neograph::json env;
    env["jsonrpc"] = "2.0";
    env["method"]  = std::move(method);
    env["params"]  = std::move(params);
    return env;
}

std::string fresh_session_id() {
    static std::atomic<std::uint64_t> counter{0};
    auto n = counter.fetch_add(1, std::memory_order_relaxed);
    auto seed = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    char buf[40];
    std::snprintf(buf, sizeof(buf), "sess-%08llx-%012llx",
                  static_cast<unsigned long long>(seed & 0xFFFFFFFF),
                  static_cast<unsigned long long>(n));
    return buf;
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct ACPServer::Impl {
    std::shared_ptr<neograph::graph::GraphEngine> engine;
    neograph::json                                info;
    std::shared_ptr<ACPGraphAdapter>              adapter;

    AgentCapabilities                             caps;
    std::atomic<bool>                             initialized{false};
    std::atomic<bool>                             stop_flag{false};

    /// Notification sink — published to atomically as a shared_ptr so
    /// readers (worker threads issuing emit()) can take a stable snapshot
    /// without holding a mutex. Writers (`run()`, `set_notification_sink`)
    /// store a fresh shared_ptr; the previous one is kept alive by any
    /// thread that already loaded it. Avoids the data race a bare
    /// `std::function` assignment would have during SBO reseating.
    std::shared_ptr<NotificationSink>             sink_;

    std::mutex                                    sessions_mu;
    std::map<std::string, std::string>            sessions;
    /// Per-session cancel flags held behind shared_ptrs so a worker
    /// thread can capture the pointer at dispatch time (under the
    /// lock) and read/write the flag thereafter without re-traversing
    /// the map — `std::map<…, std::atomic<bool>>` would otherwise
    /// invite concurrent-rebalance UB when a parallel session/cancel
    /// rotates a tree node the worker is reading.
    std::map<std::string, std::shared_ptr<std::atomic<bool>>>
                                                  cancel_flags;

    /// Outbound JSON-RPC: id → promise that the run-loop reader fulfils
    /// when it sees a response with that id.
    std::atomic<std::int64_t>                     next_outbound_id{1};
    std::mutex                                    pending_mu;
    std::unordered_map<std::int64_t,
                       std::shared_ptr<std::promise<neograph::json>>>
                                                  pending;

    /// In-flight session/prompt workers. Joined on destruction so the
    /// engine isn't called after the server is gone.
    std::mutex                                    workers_mu;
    std::vector<std::thread>                      workers;

    /// Cached client handle returned by ACPServer::client().
    std::shared_ptr<ACPClient>                    client_handle;

    std::shared_ptr<ACPGraphAdapter> adapter_ptr() { return adapter; }

    neograph::json handle_initialize(const neograph::json& params,
                                     const neograph::json& id);
    neograph::json handle_session_new(const neograph::json& params,
                                      const neograph::json& id);
    void           handle_session_prompt(ACPServer& owner,
                                         const neograph::json& params,
                                         const neograph::json& id);
    void           handle_session_cancel(const neograph::json& params);

    void emit(const neograph::json& env) {
        // Take a stable snapshot of the current sink shared_ptr — even
        // if `set_notification_sink` swaps the pointer concurrently,
        // this thread keeps the old callable alive until it returns.
        auto s = std::atomic_load_explicit(&sink_, std::memory_order_acquire);
        if (s && *s) (*s)(env);
    }
};

// ---------------------------------------------------------------------------
// Method handlers
// ---------------------------------------------------------------------------
neograph::json
ACPServer::Impl::handle_initialize(const neograph::json& params,
                                   const neograph::json& id) {
    InitializeRequest req;
    if (!params.is_null()) {
        try { from_json(params, req); }
        catch (const std::exception& e) {
            return jsonrpc_error(-32602, std::string("Invalid params: ") + e.what(), id);
        }
    }

    InitializeResponse resp;
    resp.protocol_version    = req.protocol_version > 0 ? req.protocol_version : 1;
    resp.agent_capabilities  = caps;
    resp.agent_info          = info;

    initialized.store(true, std::memory_order_release);

    neograph::json rj;
    to_json(rj, resp);
    return jsonrpc_result(std::move(rj), id);
}

neograph::json
ACPServer::Impl::handle_session_new(const neograph::json& params,
                                    const neograph::json& id) {
    NewSessionRequest req;
    if (!params.is_null()) {
        try { from_json(params, req); }
        catch (const std::exception& e) {
            return jsonrpc_error(-32602, std::string("Invalid params: ") + e.what(), id);
        }
    }

    auto sid = fresh_session_id();
    {
        std::lock_guard lk(sessions_mu);
        sessions[sid] = req.cwd;
        cancel_flags[sid] = std::make_shared<std::atomic<bool>>(false);
    }

    NewSessionResponse resp;
    resp.session_id = sid;

    neograph::json rj;
    to_json(rj, resp);
    return jsonrpc_result(std::move(rj), id);
}

void
ACPServer::Impl::handle_session_prompt(ACPServer& /*owner*/,
                                       const neograph::json& params,
                                       const neograph::json& id) {
    PromptRequest req;
    try { from_json(params, req); }
    catch (const std::exception& e) {
        emit(jsonrpc_error(-32602, std::string("Invalid params: ") + e.what(), id));
        return;
    }

    // Capture the cancel-flag shared_ptr while holding sessions_mu so the
    // worker can read/write it later without re-traversing the map.
    std::shared_ptr<std::atomic<bool>> my_cancel;
    {
        std::lock_guard lk(sessions_mu);
        if (sessions.count(req.session_id) == 0) {
            sessions[req.session_id] = "";
        }
        auto it = cancel_flags.find(req.session_id);
        if (it == cancel_flags.end() || !it->second) {
            my_cancel = std::make_shared<std::atomic<bool>>(false);
            cancel_flags[req.session_id] = my_cancel;
        } else {
            my_cancel = it->second;
        }
    }

    // Run the engine on a worker thread so the run-loop reader can keep
    // pumping inbound messages — including responses to fs/* requests
    // the engine issues mid-run via ACPClient::call_client.
    std::thread worker([this, req = std::move(req), id, my_cancel]() mutable {
        auto& a = *adapter;

        neograph::graph::RunConfig cfg;
        cfg.thread_id = req.session_id;
        cfg.input     = a.build_initial_state(req.prompt, req.session_id);

        StopReason  stop          = StopReason::EndTurn;
        bool        graph_failed  = false;
        std::string agent_text;

        try {
            auto rr = engine->run(cfg);
            agent_text = a.extract_agent_text(rr.output);
        } catch (const std::exception& e) {
            // ACP's StopReason vocabulary doesn't include a generic
            // "error" value — surface engine failures as a final
            // agent_message_chunk and end the turn normally. The chunk
            // text carries the diagnostic.
            SessionNotification n;
            n.session_id = req.session_id;
            n.update.session_update = "agent_message_chunk";
            n.update.content = ContentBlock::text_block(
                std::string("(graph error: ") + e.what() + ")");
            neograph::json nj; to_json(nj, n);
            emit(jsonrpc_notify("session/update", std::move(nj)));
            graph_failed = true;
        }

        if (!graph_failed) {
            bool was_cancelled = my_cancel->exchange(
                false, std::memory_order_acq_rel);
            if (was_cancelled) {
                stop = StopReason::Cancelled;
            } else {
                SessionNotification n;
                n.session_id = req.session_id;
                n.update.session_update = "agent_message_chunk";
                n.update.content = ContentBlock::text_block(agent_text);
                neograph::json nj; to_json(nj, n);
                emit(jsonrpc_notify("session/update", std::move(nj)));
            }
        }

        PromptResponse resp;
        resp.stop_reason = stop;
        neograph::json rj;
        to_json(rj, resp);
        emit(jsonrpc_result(std::move(rj), id));
    });

    {
        std::lock_guard lk(workers_mu);
        workers.push_back(std::move(worker));
    }
}

void
ACPServer::Impl::handle_session_cancel(const neograph::json& params) {
    CancelNotification n;
    try { from_json(params, n); }
    catch (...) { return; }

    std::shared_ptr<std::atomic<bool>> flag;
    {
        std::lock_guard lk(sessions_mu);
        auto it = cancel_flags.find(n.session_id);
        if (it != cancel_flags.end() && it->second) {
            flag = it->second;
        } else {
            // Cancel arriving for a session whose prompt hasn't been
            // dispatched yet — pre-create the flag so the eventual
            // handle_session_prompt picks it up. Aligns with the
            // documented semantic ("cancel-before-prompt sticks until
            // the next prompt observes and consumes it").
            flag = std::make_shared<std::atomic<bool>>(false);
            cancel_flags[n.session_id] = flag;
        }
    }
    flag->store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------
ACPServer::ACPServer(std::shared_ptr<neograph::graph::GraphEngine> engine,
                     neograph::json info,
                     std::shared_ptr<ACPGraphAdapter> adapter)
    : impl_(std::make_unique<Impl>()) {
    if (!engine) throw std::invalid_argument("ACPServer: engine is null");
    impl_->engine  = std::move(engine);
    impl_->info    = std::move(info);
    impl_->adapter = adapter ? adapter : std::make_shared<ACPGraphAdapter>();
}

ACPServer::~ACPServer() {
    // Make sure no in-flight worker is mid-engine-run when the server
    // dies — that would dereference a freed engine pointer through the
    // captured shared_ptr (which is safe) but would also try to write to
    // an output stream that's about to disappear.
    impl_->stop_flag.store(true, std::memory_order_release);
    std::vector<std::thread> drained;
    {
        std::lock_guard lk(impl_->workers_mu);
        drained = std::move(impl_->workers);
    }
    for (auto& t : drained) if (t.joinable()) t.join();

    // Wake any pending outbound RPC waiters with an error so they don't
    // hang the caller forever.
    std::lock_guard lk(impl_->pending_mu);
    for (auto& kv : impl_->pending) {
        try {
            neograph::json err;
            err["error"] = neograph::json{
                {"code",    -32603},
                {"message", "ACPServer torn down before response"}};
            kv.second->set_value(std::move(err));
        } catch (...) {}
    }
    impl_->pending.clear();
}

AgentCapabilities&       ACPServer::capabilities()       { return impl_->caps; }
const AgentCapabilities& ACPServer::capabilities() const { return impl_->caps; }

void ACPServer::set_notification_sink(NotificationSink sink) {
    auto s = std::make_shared<NotificationSink>(std::move(sink));
    std::atomic_store_explicit(&impl_->sink_, std::move(s),
                               std::memory_order_release);
}

void ACPServer::stop() {
    impl_->stop_flag.store(true, std::memory_order_release);
}

bool ACPServer::initialized() const {
    return impl_->initialized.load(std::memory_order_acquire);
}

std::shared_ptr<ACPClient> ACPServer::client() {
    if (!impl_->client_handle) {
        impl_->client_handle = std::make_shared<ACPClient>(this);
    } else if (!impl_->client_handle->bound()) {
        impl_->client_handle->bind(this);
    }
    return impl_->client_handle;
}

void ACPServer::attach_client(std::shared_ptr<ACPClient> c) {
    if (c) c->bind(this);
    impl_->client_handle = std::move(c);
}

neograph::json
ACPServer::call_client(std::string method,
                       neograph::json params,
                       std::chrono::milliseconds timeout) {
    auto s_snap = std::atomic_load_explicit(&impl_->sink_,
                                            std::memory_order_acquire);
    if (!s_snap || !*s_snap) {
        throw std::runtime_error(
            "ACPServer::call_client: no transport connected — call run() first "
            "or set_notification_sink() before issuing fs/* requests");
    }

    auto id = impl_->next_outbound_id.fetch_add(1, std::memory_order_relaxed);
    neograph::json env;
    env["jsonrpc"] = "2.0";
    env["id"]      = id;
    env["method"]  = std::move(method);
    env["params"]  = std::move(params);

    auto promise = std::make_shared<std::promise<neograph::json>>();
    auto fut     = promise->get_future();
    {
        std::lock_guard lk(impl_->pending_mu);
        impl_->pending[id] = promise;
    }

    impl_->emit(env);

    auto status = fut.wait_for(timeout);
    if (status != std::future_status::ready) {
        std::lock_guard lk(impl_->pending_mu);
        impl_->pending.erase(id);
        throw std::runtime_error("ACPServer::call_client: timeout waiting for "
                                 + env.value("method", std::string()));
    }
    auto resp = fut.get();
    if (resp.contains("error")) {
        throw std::runtime_error(
            "ACPServer::call_client: error response: "
            + resp["error"].dump());
    }
    return resp.contains("result") ? resp["result"] : neograph::json::object();
}

neograph::json
ACPServer::handle_message(const neograph::json& env) {
    bool has_method = env.contains("method") && !env["method"].is_null();

    // Response to one of OUR outbound requests — fulfil the promise
    // and emit no reply.
    if (!has_method) {
        if (env.contains("id")) {
            auto id_v = env["id"];
            std::int64_t id = id_v.is_number_integer() ? id_v.get<std::int64_t>() : -1;
            std::shared_ptr<std::promise<neograph::json>> p;
            {
                std::lock_guard lk(impl_->pending_mu);
                auto it = impl_->pending.find(id);
                if (it != impl_->pending.end()) {
                    p = it->second;
                    impl_->pending.erase(it);
                }
            }
            if (p) p->set_value(env);
        }
        return {};
    }

    auto method = env.value("method", std::string());
    auto params = env.contains("params") ? env["params"] : neograph::json::object();
    auto id     = env.contains("id") ? env["id"] : neograph::json();
    bool is_notification = !env.contains("id");

    if (method == "initialize") {
        return impl_->handle_initialize(params, id);
    }
    if (method == "session/new") {
        return impl_->handle_session_new(params, id);
    }
    if (method == "session/prompt") {
        // Async dispatch — the worker will emit the response via the sink.
        impl_->handle_session_prompt(*this, params, id);
        return {};
    }
    if (method == "session/cancel") {
        impl_->handle_session_cancel(params);
        return {};
    }

    if (is_notification) return {};
    return jsonrpc_error(-32601, "Method not found: " + method, id);
}

void ACPServer::run(std::istream& in, std::ostream& out) {
    auto out_mu = std::make_shared<std::mutex>();
    auto out_ptr = &out;

    auto run_sink = std::make_shared<NotificationSink>(
        [out_ptr, out_mu](const neograph::json& env) {
            // Strip trailing CR if a CRLF-emitting peer wrote one — the
            // other side's getline leaves it on the line; we never want
            // to emit one ourselves either.
            auto s = env.dump();
            std::lock_guard lk(*out_mu);
            (*out_ptr) << s << '\n';
            out_ptr->flush();
        });
    std::atomic_store_explicit(&impl_->sink_, run_sink,
                               std::memory_order_release);

    std::string line;
    while (!impl_->stop_flag.load(std::memory_order_acquire)
           && std::getline(in, line)) {
        // Tolerate CRLF input — strip a trailing \r left by getline().
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        neograph::json env;
        try {
            env = neograph::json::parse(line);
        } catch (const std::exception&) {
            auto err = jsonrpc_error(-32700, "Parse error", neograph::json());
            std::lock_guard lk(*out_mu);
            out << err.dump() << '\n';
            out.flush();
            continue;
        }

        auto resp = handle_message(env);
        if (!resp.is_null()) {
            std::lock_guard lk(*out_mu);
            out << resp.dump() << '\n';
            out.flush();
        }
    }

    // Drain workers before tearing the sink down — otherwise a worker
    // that finishes after run() returns would write through a dangling
    // stream pointer.
    std::vector<std::thread> drained;
    {
        std::lock_guard lk(impl_->workers_mu);
        drained = std::move(impl_->workers);
    }
    for (auto& t : drained) if (t.joinable()) t.join();

    std::atomic_store_explicit(&impl_->sink_,
                               std::shared_ptr<NotificationSink>{},
                               std::memory_order_release);
}

void ACPServer::run() {
    run(std::cin, std::cout);
}

// ---------------------------------------------------------------------------
// ACPClient
// ---------------------------------------------------------------------------
ACPClient::ACPClient() = default;
ACPClient::ACPClient(ACPServer* server) : server_(server) {}

void ACPClient::bind(ACPServer* server)             { server_ = server; }
bool ACPClient::bound() const noexcept              { return server_ != nullptr; }
void ACPClient::set_timeout(std::chrono::milliseconds t) { timeout_ = t; }

std::string
ACPClient::read_text_file(std::string_view session_id,
                          std::string_view path,
                          std::optional<int> line,
                          std::optional<int> limit) {
    if (!server_) throw std::runtime_error(
        "ACPClient::read_text_file: not bound to a server (call ACPServer::attach_client)");

    ReadTextFileRequest req;
    req.session_id = std::string(session_id);
    req.path       = std::string(path);
    req.line       = line;
    req.limit      = limit;

    neograph::json params; to_json(params, req);
    auto result = server_->call_client("fs/read_text_file",
                                       std::move(params), timeout_);
    ReadTextFileResponse resp;
    from_json(result, resp);
    return std::move(resp.content);
}

void
ACPClient::write_text_file(std::string_view session_id,
                           std::string_view path,
                           std::string_view content) {
    if (!server_) throw std::runtime_error(
        "ACPClient::write_text_file: not bound to a server (call ACPServer::attach_client)");

    WriteTextFileRequest req;
    req.session_id = std::string(session_id);
    req.path       = std::string(path);
    req.content    = std::string(content);

    neograph::json params; to_json(params, req);
    (void)server_->call_client("fs/write_text_file",
                               std::move(params), timeout_);
}

RequestPermissionOutcome
ACPClient::request_permission(std::string_view session_id,
                              const ToolCall& tool_call,
                              const std::vector<PermissionOption>& options) {
    if (!server_) throw std::runtime_error(
        "ACPClient::request_permission: not bound to a server (call ACPServer::attach_client)");

    RequestPermissionRequest req;
    req.session_id = std::string(session_id);
    req.tool_call  = tool_call;
    req.options    = options;

    neograph::json params; to_json(params, req);
    auto result = server_->call_client("session/request_permission",
                                       std::move(params), timeout_);

    RequestPermissionResponse resp;
    from_json(result, resp);
    return std::move(resp.outcome);
}

}  // namespace neograph::acp
