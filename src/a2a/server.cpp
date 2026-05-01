#include <neograph/a2a/server.h>

#include <neograph/graph/types.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <list>
#include <map>
#include <unordered_map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

namespace neograph::a2a {

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// GraphAgentAdapter default
// ---------------------------------------------------------------------------
neograph::json
GraphAgentAdapter::build_initial_state(const std::string& user_text) const {
    neograph::json state = neograph::json::object();
    state[input_channel()] = user_text;
    return state;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {

std::string fresh_uuid_like() {
    static std::atomic<std::uint64_t> counter{0};
    auto n = counter.fetch_add(1, std::memory_order_relaxed);
    auto seed = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%08llx-%04llx-%04llx-%012llx",
                  static_cast<unsigned long long>((seed >> 32) & 0xFFFFFFFF),
                  static_cast<unsigned long long>((seed >> 16) & 0xFFFF),
                  static_cast<unsigned long long>(seed & 0xFFFF),
                  static_cast<unsigned long long>(n));
    return buf;
}

/// Pull the user text out of an inbound A2A Message — concatenate every
/// text part, ignoring file/data parts (those would need adapter support).
std::string extract_user_text(const Message& m) {
    std::string out;
    for (auto& p : m.parts) {
        if (p.kind == "text") {
            if (!out.empty()) out.push_back(' ');
            out.append(p.text);
        }
    }
    return out;
}

/// Read the agent's text reply out of the graph's final state. Looks
/// at `output_channel` first, falls back to `messages[-1].content` if
/// that channel is missing — matches the convention used by ReAct
/// graphs in the examples.
std::string extract_agent_text(const neograph::json& output,
                               const std::string& output_channel) {
    if (output.contains("channels")) {
        auto ch = output["channels"];
        if (ch.contains(output_channel) && ch[output_channel].contains("value")) {
            auto v = ch[output_channel]["value"];
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

Task build_response_task(const std::string& task_id,
                         const std::string& context_id,
                         const std::string& agent_text) {
    Message reply;
    reply.message_id = fresh_uuid_like();
    reply.role       = Role::Agent;
    reply.task_id    = task_id;
    reply.context_id = context_id;
    reply.parts.push_back(Part::text_part(agent_text));

    Task t;
    t.id              = task_id;
    t.context_id      = context_id;
    t.status.state    = TaskState::Completed;
    t.status.message  = reply;
    t.history.push_back(std::move(reply));
    return t;
}

Task build_failure_task(const std::string& task_id,
                        const std::string& context_id,
                        const std::string& reason) {
    Message reply;
    reply.message_id = fresh_uuid_like();
    reply.role       = Role::Agent;
    reply.task_id    = task_id;
    reply.context_id = context_id;
    reply.parts.push_back(Part::text_part(reason));

    Task t;
    t.id             = task_id;
    t.context_id     = context_id;
    t.status.state   = TaskState::Failed;
    t.status.message = reply;
    t.history.push_back(std::move(reply));
    return t;
}

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

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct A2AServer::Impl {
    std::shared_ptr<neograph::graph::GraphEngine> engine;
    AgentCard                                     card;
    std::shared_ptr<GraphAgentAdapter>            adapter;
    httplib::Server                               svr;
    std::thread                                   listener;
    std::atomic<bool>                             running{false};
    int                                           bound_port = 0;

    /// In-memory task store. tasks/get and tasks/cancel hit this; we
    /// don't persist across server restarts — A2A spec allows that.
    ///
    /// LRU bounded by `max_tasks` (default 1024) — without this a
    /// long-running A2A server unbounded-grows its task history,
    /// eventually OOMing. `task_lru` holds task_ids in
    /// least-recently-touched-first order; on insert/touch we move
    /// the id to the back. On insert that would exceed the cap, we
    /// evict from the front.
    ///
    /// `cancel_flags` holds atomics behind shared_ptrs so a worker
    /// thread can capture the pointer at dispatch time (under the
    /// lock) and read/write the flag thereafter without re-traversing
    /// the map — `std::map<…, std::atomic<bool>>` would otherwise
    /// invite concurrent-rebalance UB when a parallel insert from
    /// `tasks/cancel` rotates a tree node the worker is reading.
    std::mutex                                                tasks_mu;
    std::unordered_map<std::string, Task>                     tasks;
    std::unordered_map<std::string, std::shared_ptr<std::atomic<bool>>>
                                                              cancel_flags;
    std::list<std::string>                                    task_lru;
    std::unordered_map<std::string, std::list<std::string>::iterator>
                                                              task_lru_pos;
    std::size_t max_tasks = 1024;

    /// Move a task_id to the most-recently-used (back) position, or
    /// insert it if it's new. Caller must hold tasks_mu.
    void touch_task_unlocked(const std::string& tid) {
        auto it = task_lru_pos.find(tid);
        if (it != task_lru_pos.end()) {
            task_lru.erase(it->second);
        }
        task_lru.push_back(tid);
        task_lru_pos[tid] = std::prev(task_lru.end());
    }

    /// Evict LRU entries until tasks.size() <= max_tasks. Caller must
    /// hold tasks_mu.
    void evict_lru_unlocked() {
        while (tasks.size() > max_tasks && !task_lru.empty()) {
            auto victim = task_lru.front();
            task_lru.pop_front();
            task_lru_pos.erase(victim);
            tasks.erase(victim);
            cancel_flags.erase(victim);
        }
    }

    void register_routes();

    Task run_graph(const Message& inbound,
                   const std::string& task_id,
                   const std::string& context_id,
                   std::function<void(const TaskStatusUpdateEvent&)> on_event);

    void handle_jsonrpc(const httplib::Request& req, httplib::Response& res);
    void handle_message_send(const neograph::json& params, const neograph::json& id,
                             httplib::Response& res);
    void handle_message_stream(const neograph::json& params, const neograph::json& id,
                               httplib::Response& res);
    void handle_tasks_get(const neograph::json& params, const neograph::json& id,
                          httplib::Response& res);
    void handle_tasks_cancel(const neograph::json& params, const neograph::json& id,
                             httplib::Response& res);
};

void A2AServer::Impl::register_routes() {
    svr.Get("/.well-known/agent-card.json",
            [this](const httplib::Request&, httplib::Response& res) {
                neograph::json j;
                to_json(j, card);
                res.status = 200;
                res.set_content(j.dump(), "application/json");
            });

    svr.Post("/",
             [this](const httplib::Request& req, httplib::Response& res) {
                 handle_jsonrpc(req, res);
             });
}

void A2AServer::Impl::handle_jsonrpc(const httplib::Request& req,
                                     httplib::Response& res) {
    neograph::json req_json;
    try {
        req_json = neograph::json::parse(req.body);
    } catch (const std::exception&) {
        res.status = 200;
        res.set_content(
            jsonrpc_error(-32700, "Parse error", neograph::json()).dump(),
            "application/json");
        return;
    }
    bool is_notification = !req_json.contains("id");
    auto id     = req_json.contains("id") ? req_json["id"] : neograph::json();
    auto method = req_json.value("method", std::string());
    auto params = req_json.contains("params") ? req_json["params"]
                                              : neograph::json::object();

    // JSON-RPC 2.0 §4.1: "The Server MUST NOT reply to a Notification."
    // A2A's defined methods (message/send, message/stream, tasks/get,
    // tasks/cancel) are all request/response, so a notification of any
    // of them is malformed by spec — but we still must not reply with
    // a JSON envelope. Drop on the floor with HTTP 204 No Content.
    if (is_notification) {
        res.status = 204;
        return;
    }

    if (method == "message/send" || method == "SendMessage") {
        handle_message_send(params, id, res);
    } else if (method == "message/stream" || method == "SendStreamingMessage") {
        handle_message_stream(params, id, res);
    } else if (method == "tasks/get" || method == "GetTask") {
        handle_tasks_get(params, id, res);
    } else if (method == "tasks/cancel" || method == "CancelTask") {
        handle_tasks_cancel(params, id, res);
    } else {
        res.status = 200;
        res.set_content(jsonrpc_error(-32601, "Method not found", id).dump(),
                        "application/json");
    }
}

Task A2AServer::Impl::run_graph(
    const Message& inbound,
    const std::string& task_id,
    const std::string& context_id,
    std::function<void(const TaskStatusUpdateEvent&)> on_event) {

    auto user_text = extract_user_text(inbound);
    auto& a = *adapter;

    neograph::graph::RunConfig cfg;
    cfg.thread_id = task_id;
    cfg.input     = a.build_initial_state(user_text);

    // Capture the cancel-flag shared_ptr while holding the mutex so the
    // worker can read/write it later without re-traversing the map (see
    // tasks_mu / cancel_flags doc comment for the rationale).
    std::shared_ptr<std::atomic<bool>> my_cancel;
    {
        std::lock_guard lk(tasks_mu);
        Task working;
        working.id            = task_id;
        working.context_id    = context_id;
        working.status.state  = TaskState::Working;
        tasks[task_id]        = working;
        my_cancel             = std::make_shared<std::atomic<bool>>(false);
        cancel_flags[task_id] = my_cancel;
        touch_task_unlocked(task_id);
        evict_lru_unlocked();
    }

    if (on_event) {
        TaskStatusUpdateEvent ev;
        ev.task_id    = task_id;
        ev.context_id = context_id;
        ev.status.state = TaskState::Working;
        ev.final = false;
        on_event(ev);
    }

    Task result;
    try {
        auto rr = engine->run(cfg);
        if (my_cancel->load(std::memory_order_acquire)) {
            result = build_failure_task(task_id, context_id, "(canceled)");
            result.status.state = TaskState::Canceled;
        } else {
            auto agent_text = extract_agent_text(rr.output, a.output_channel());
            result = build_response_task(task_id, context_id, agent_text);
        }
    } catch (const std::exception& e) {
        result = build_failure_task(
            task_id, context_id,
            std::string("graph run failed: ") + e.what());
    }

    {
        std::lock_guard lk(tasks_mu);
        tasks[task_id] = result;
        touch_task_unlocked(task_id);
    }

    if (on_event) {
        TaskStatusUpdateEvent ev;
        ev.task_id    = task_id;
        ev.context_id = context_id;
        ev.status     = result.status;
        ev.final      = true;
        on_event(ev);
    }

    return result;
}

void A2AServer::Impl::handle_message_send(const neograph::json& params,
                                          const neograph::json& id,
                                          httplib::Response& res) {
    MessageSendParams mp;
    try {
        from_json(params, mp);
    } catch (const std::exception& e) {
        res.status = 200;
        res.set_content(jsonrpc_error(-32602,
                                      std::string("Invalid params: ") + e.what(),
                                      id).dump(),
                        "application/json");
        return;
    }

    auto task_id    = mp.message.task_id.value_or(fresh_uuid_like());
    auto context_id = mp.message.context_id.value_or(fresh_uuid_like());

    auto task = run_graph(mp.message, task_id, context_id, /*on_event=*/{});

    neograph::json tj;
    to_json(tj, task);
    res.status = 200;
    res.set_content(jsonrpc_result(std::move(tj), id).dump(),
                    "application/json");
}

void A2AServer::Impl::handle_message_stream(const neograph::json& params,
                                            const neograph::json& id,
                                            httplib::Response& res) {
    MessageSendParams mp;
    try {
        from_json(params, mp);
    } catch (const std::exception& e) {
        res.status = 200;
        res.set_content(jsonrpc_error(-32602,
                                      std::string("Invalid params: ") + e.what(),
                                      id).dump(),
                        "application/json");
        return;
    }

    auto task_id    = mp.message.task_id.value_or(fresh_uuid_like());
    auto context_id = mp.message.context_id.value_or(fresh_uuid_like());
    auto inbound    = mp.message;
    auto rpc_id     = id;
    auto self       = this;

    // Note on SSE resumability: this stream emits no `id:` field per
    // event and no `retry:` hints, so a client reconnecting with
    // `Last-Event-ID` cannot replay missed frames. A2A's documented
    // recovery path is `tasks/resubscribe` (issue a fresh
    // message/stream against the existing task_id), which works fine
    // here because the in-memory task store survives the dropped HTTP
    // connection. If you need finer-grained resumability for a
    // long-running task, run an external SSE proxy in front of this
    // server.
    res.set_chunked_content_provider(
        "text/event-stream",
        [self, inbound, task_id, context_id, rpc_id](
            size_t /*offset*/, httplib::DataSink& sink) {

            auto emit = [&](const TaskStatusUpdateEvent& ev) {
                neograph::json env_json;
                to_json(env_json, ev);
                auto env = jsonrpc_result(std::move(env_json), rpc_id);
                std::string frame = "data: " + env.dump() + "\n\n";
                sink.write(frame.data(), frame.size());
            };

            try {
                auto task = self->run_graph(inbound, task_id, context_id, emit);
                neograph::json tj;
                to_json(tj, task);
                auto env = jsonrpc_result(std::move(tj), rpc_id);
                std::string frame = "data: " + env.dump() + "\n\n";
                sink.write(frame.data(), frame.size());
            } catch (...) {
                // run_graph already wraps exceptions as Failed; fall through.
            }
            sink.done();
            return true;
        });
}

void A2AServer::Impl::handle_tasks_get(const neograph::json& params,
                                       const neograph::json& id,
                                       httplib::Response& res) {
    auto task_id = params.value("id", std::string());
    Task t;
    bool found = false;
    {
        std::lock_guard lk(tasks_mu);
        auto it = tasks.find(task_id);
        if (it != tasks.end()) {
            t = it->second;
            found = true;
            touch_task_unlocked(task_id);  // tasks/get is a recency signal
        }
    }
    if (!found) {
        res.status = 200;
        res.set_content(jsonrpc_error(-32001, "Task not found", id).dump(),
                        "application/json");
        return;
    }
    auto hl = params.value("historyLength", -1);
    if (hl >= 0 && static_cast<int>(t.history.size()) > hl) {
        t.history.erase(t.history.begin(),
                        t.history.begin() + (t.history.size() - hl));
    }
    neograph::json tj;
    to_json(tj, t);
    res.status = 200;
    res.set_content(jsonrpc_result(std::move(tj), id).dump(),
                    "application/json");
}

void A2AServer::Impl::handle_tasks_cancel(const neograph::json& params,
                                          const neograph::json& id,
                                          httplib::Response& res) {
    auto task_id = params.value("id", std::string());
    Task t;
    bool found = false;
    {
        std::lock_guard lk(tasks_mu);
        auto it = tasks.find(task_id);
        if (it != tasks.end()) {
            t = it->second;
            found = true;
            auto cf_it = cancel_flags.find(task_id);
            if (cf_it != cancel_flags.end() && cf_it->second) {
                cf_it->second->store(true, std::memory_order_release);
            }
            if (t.status.state == TaskState::Working
                || t.status.state == TaskState::Submitted) {
                t.status.state = TaskState::Canceled;
                tasks[task_id] = t;
                touch_task_unlocked(task_id);
            }
        }
    }
    if (!found) {
        res.status = 200;
        res.set_content(jsonrpc_error(-32001, "Task not found", id).dump(),
                        "application/json");
        return;
    }
    neograph::json tj;
    to_json(tj, t);
    res.status = 200;
    res.set_content(jsonrpc_result(std::move(tj), id).dump(),
                    "application/json");
}

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------
A2AServer::A2AServer(std::shared_ptr<neograph::graph::GraphEngine> engine,
                     AgentCard card,
                     std::shared_ptr<GraphAgentAdapter> adapter)
    : impl_(std::make_unique<Impl>()) {
    if (!engine) throw std::invalid_argument("A2AServer: engine is null");
    impl_->engine  = std::move(engine);
    impl_->card    = std::move(card);
    impl_->adapter = adapter ? adapter : std::make_shared<GraphAgentAdapter>();
    impl_->register_routes();
}

A2AServer::~A2AServer() { stop(); }

bool A2AServer::start(const std::string& host, int port) {
    if (port == 0) {
        impl_->bound_port = impl_->svr.bind_to_any_port(host);
        if (impl_->bound_port < 0) return false;
    } else {
        if (!impl_->svr.bind_to_port(host, port)) return false;
        impl_->bound_port = port;
    }
    impl_->running.store(true, std::memory_order_release);
    bool ok = impl_->svr.listen_after_bind();
    impl_->running.store(false, std::memory_order_release);
    return ok;
}

bool A2AServer::start_async(const std::string& host, int port) {
    if (port == 0) {
        impl_->bound_port = impl_->svr.bind_to_any_port(host);
        if (impl_->bound_port < 0) return false;
    } else {
        if (!impl_->svr.bind_to_port(host, port)) return false;
        impl_->bound_port = port;
    }
    impl_->running.store(true, std::memory_order_release);
    impl_->listener = std::thread([this] {
        impl_->svr.listen_after_bind();
        impl_->running.store(false, std::memory_order_release);
    });
    for (int i = 0; i < 200 && !impl_->svr.is_running(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    return impl_->svr.is_running();
}

void A2AServer::stop() {
    if (impl_->svr.is_running()) impl_->svr.stop();
    if (impl_->listener.joinable()) impl_->listener.join();
}

bool A2AServer::is_running() const { return impl_->running.load(std::memory_order_acquire); }

int A2AServer::port() const { return impl_->bound_port; }

}  // namespace neograph::a2a
