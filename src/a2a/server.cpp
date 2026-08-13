#include <neograph/a2a/server.h>
#include <neograph/graph/invocation.h>
#include <neograph/graph/types.h>

#ifdef NEOGRAPH_A2A_PROGRAM
#include <neograph/a2a/program_adapter.h>
#endif

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
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

std::optional<Artifact> GraphAgentAdapter::build_output_artifact(
    const neograph::json&, const std::string&) const {
    return std::nullopt;
}

StructuredOutputAdapter::StructuredOutputAdapter(std::string contract,
                                                 int version,
                                                 std::string data_channel,
                                                 std::string input_channel)
    : contract_(std::move(contract)),
      version_(version),
      data_channel_(std::move(data_channel)),
      input_channel_(std::move(input_channel)) {
    if (contract_.find_first_not_of(" \t\n\r") == std::string::npos) {
        throw std::invalid_argument("structured output contract is required");
    }
    if (version_ < 1) throw std::invalid_argument("structured output version must be positive");
    if (data_channel_.empty()) throw std::invalid_argument("structured output data channel is required");
}

std::string StructuredOutputAdapter::input_channel() const { return input_channel_; }
std::string StructuredOutputAdapter::output_channel() const { return "response"; }

std::optional<Artifact> StructuredOutputAdapter::build_output_artifact(
    const neograph::json& output, const std::string& task_id) const {
    if (!output.contains("channels")) return std::nullopt;
    const auto& channels = output["channels"];
    if (!channels.contains(data_channel_) || !channels[data_channel_].contains("value")) {
        return std::nullopt;
    }
    Part part;
    part.kind = "data";
    part.data = {{"contract", contract_}, {"version", version_},
                 {"value", channels[data_channel_]["value"]}};
    Artifact artifact;
    artifact.artifact_id = task_id + "-artifact";
    artifact.name = contract_;
    artifact.parts.push_back(std::move(part));
    return artifact;
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
                          const std::string& agent_text,
                          std::optional<Artifact> artifact = std::nullopt) {
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
    if (artifact) t.artifacts.push_back(std::move(*artifact));
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

Task build_rejected_task(const std::string& task_id,
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
    t.status.state   = TaskState::Rejected;
    t.status.message = reply;
    t.history.push_back(std::move(reply));
    return t;
}

Task build_auth_required_task(const std::string& task_id,
                              const std::string& context_id) {
    Task task;
    task.id = task_id;
    task.context_id = context_id;
    task.status.state = TaskState::AuthRequired;
    return task;
}

#ifdef NEOGRAPH_A2A_PROGRAM
bool has_collaboration_marker(const Message& message) {
    if (message.metadata.is_object() &&
        message.metadata.value("neograph_collaboration", false)) {
        return true;
    }
    for (const auto& part : message.parts) {
        if (part.kind == "data" && part.data.is_object() &&
            part.data.value("format", std::string()) == "neograph-a2a-collaboration-v1") {
            return true;
        }
    }
    return false;
}
#endif

bool is_terminal(TaskState state) {
    return state == TaskState::Completed || state == TaskState::Canceled
        || state == TaskState::Failed || state == TaskState::Rejected
        || state == TaskState::AuthRequired;
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
#ifdef NEOGRAPH_A2A_PROGRAM
    std::shared_ptr<ProgramAgentAdapter>           program_adapter;
    A2AServer::CollaborationAuthenticator          collaboration_authenticator;
#endif
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
    struct ActiveRun {
        std::shared_ptr<neograph::graph::CancelToken> cancel_token;
        std::uint64_t                                  generation;
    };

    /// Active runs remain separately addressable until their owning
    /// generation publishes its terminal task state.
    std::mutex                                                tasks_mu;
    std::unordered_map<std::string, Task>                     tasks;
    std::unordered_map<std::string, ActiveRun>                active_runs;
#ifdef NEOGRAPH_A2A_PROGRAM
    struct ProgramActiveRun {
        program::ProgramHandle handle;
        std::uint64_t           generation;
    };
    std::unordered_map<std::string, ProgramActiveRun>          program_runs;
#endif
    bool                                                        program_recovery_complete = false;
    std::uint64_t                                              next_generation = 1;
    std::list<std::string>                                    task_lru;
    std::unordered_map<std::string, std::list<std::string>::iterator>
                                                              task_lru_pos;
    std::size_t max_tasks = 1024;
    std::size_t max_inflight_runs = 32;

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
        auto remaining = task_lru.size();
        while (tasks.size() > max_tasks && remaining-- > 0 && !task_lru.empty()) {
            auto victim = task_lru.front();
            task_lru.pop_front();
            if (active_runs.contains(victim)
#ifdef NEOGRAPH_A2A_PROGRAM
                || program_runs.contains(victim)
#endif
            ) {
                task_lru.push_back(victim);
                task_lru_pos[victim] = std::prev(task_lru.end());
                continue;
            }
            task_lru_pos.erase(victim);
            tasks.erase(victim);
        }
    }

    void register_routes();

    Task run_graph(const Message& inbound,
                   const std::string& task_id,
                   const std::string& context_id,
                   std::function<void(const TaskStatusUpdateEvent&)> on_event);
#ifdef NEOGRAPH_A2A_PROGRAM
    Task run_program(
        const Message& inbound,
        const std::string& task_id,
        const std::string& context_id,
        std::optional<CollaborationPeerIdentity> authenticated_peer,
        std::function<void(const TaskStatusUpdateEvent&)> on_event);

    /// Resolve a request header to a non-secret collaboration principal.
    /// Failures intentionally collapse to no authenticated principal.
    std::optional<CollaborationPeerIdentity> authenticate_collaboration_request(
        const httplib::Request& request) const;

    bool authorizes_collaboration_message(
        const Message& inbound,
        const std::optional<CollaborationPeerIdentity>& authenticated_peer) const;

    CollaborationTaskAuthorization authorize_collaboration_task(
        std::string_view task_id,
        const std::optional<CollaborationPeerIdentity>& authenticated_peer,
        bool require_cancellation) const;

    /// Rebuild task projections for accepted mailbox records before the
    /// listener accepts new requests. The Program transition store remains
    /// authoritative; this only restores the HTTP-facing projection.
    void recover_program_tasks();
#endif

    void handle_jsonrpc(const httplib::Request& req, httplib::Response& res);
    void handle_message_send(const httplib::Request& req,
                             const neograph::json& params, const neograph::json& id,
                             httplib::Response& res);
    void handle_message_stream(const httplib::Request& req,
                               const neograph::json& params, const neograph::json& id,
                               httplib::Response& res);
    void handle_tasks_get(const httplib::Request& req,
                          const neograph::json& params, const neograph::json& id,
                          httplib::Response& res);
    void handle_tasks_cancel(const httplib::Request& req,
                             const neograph::json& params, const neograph::json& id,
                             httplib::Response& res);
};

#ifdef NEOGRAPH_A2A_TESTING
void test::A2AServerTestAccess::set_max_inflight_runs(
    A2AServer& server, std::size_t cap) {
    std::lock_guard lk(server.impl_->tasks_mu);
    server.impl_->max_inflight_runs = cap;
}
#endif

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
        handle_message_send(req, params, id, res);
    } else if (method == "message/stream" || method == "SendStreamingMessage") {
        handle_message_stream(req, params, id, res);
    } else if (method == "tasks/get" || method == "GetTask") {
        handle_tasks_get(req, params, id, res);
    } else if (method == "tasks/cancel" || method == "CancelTask") {
        handle_tasks_cancel(req, params, id, res);
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

    auto& a = *adapter;

    neograph::graph::RunConfig cfg;
    cfg.thread_id = task_id;

    std::shared_ptr<neograph::graph::CancelToken> task_cancel;
    std::optional<Task> rejected;
    std::uint64_t generation = 0;
    {
        std::lock_guard lk(tasks_mu);
        if (active_runs.contains(task_id)) {
            rejected = build_rejected_task(
                task_id, context_id, "task_id is already running");
        } else if (active_runs.size()
#ifdef NEOGRAPH_A2A_PROGRAM
                   + program_runs.size()
#endif
                   >= max_inflight_runs) {
            rejected = build_rejected_task(
                task_id, context_id, "A2A server is at its in-flight task limit");
        } else {
            Task working;
            working.id             = task_id;
            working.context_id     = context_id;
            working.status.state   = TaskState::Working;
            tasks[task_id]         = working;
            task_cancel = std::make_shared<neograph::graph::CancelToken>();
            generation = next_generation++;
            active_runs[task_id] = ActiveRun{task_cancel, generation};
            touch_task_unlocked(task_id);
            evict_lru_unlocked();
        }
    }
    if (rejected) {
        if (on_event) {
            TaskStatusUpdateEvent ev;
            ev.task_id = task_id;
            ev.context_id = context_id;
            ev.status = rejected->status;
            ev.final = true;
            on_event(ev);
        }
        return *rejected;
    }
    cfg.cancel_token = task_cancel;

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
        // Reserve the generation before adapter work so tasks/cancel can
        // signal this run even while input preparation is still in progress.
        cfg.input = a.build_initial_state(extract_user_text(inbound));
        neograph::graph::RunInvocationRequest request;
        request.config = std::move(cfg);
        neograph::graph::RunInvocation invocation(engine, std::move(request));
        const auto outcome = invocation.run();
        if (outcome.cancelled()) {
            result = build_failure_task(task_id, context_id, "(canceled)");
            result.status.state = TaskState::Canceled;
        } else if (!outcome.run_result) {
            result = build_failure_task(
                task_id, context_id,
                std::string("graph run failed: ") + outcome.error);
        } else {
            const auto& rr = *outcome.run_result;
            auto agent_text = extract_agent_text(rr.output, a.output_channel());
            result = build_response_task(task_id, context_id, agent_text,
                                         a.build_output_artifact(rr.output, task_id));
        }
    } catch (const std::exception& e) {
        result = build_failure_task(
            task_id, context_id,
            std::string("graph run failed: ") + e.what());
    }

    {
        std::lock_guard lk(tasks_mu);
        auto run_it = active_runs.find(task_id);
        if (run_it != active_runs.end()
            && run_it->second.generation == generation
            && run_it->second.cancel_token == task_cancel) {
            // The task-map lock is the terminal-state linearization point.
            // A tasks/cancel that commits first must not be overwritten by a
            // graph completion that was already preparing its response.
            auto task_it = tasks.find(task_id);
            if (task_it != tasks.end()
                && task_it->second.status.state == TaskState::Canceled) {
                result = task_it->second;
            } else {
                tasks[task_id] = result;
            }
            touch_task_unlocked(task_id);
            active_runs.erase(run_it);
        }
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

#ifdef NEOGRAPH_A2A_PROGRAM

void A2AServer::Impl::recover_program_tasks() {
    if (!program_adapter) return;
    {
        std::lock_guard lk(tasks_mu);
        if (program_recovery_complete) return;
    }


    std::unordered_map<std::string, std::string> contexts;
    if (const auto& mailbox = program_adapter->mailbox()) {
        for (const auto& record : mailbox->snapshot()) {
            if (record.state != CollaborationRecordState::Accepted ||
                !record.program_request || !record.program_request->invocation) {
                continue;
            }
            const auto& run_id = record.program_request->invocation->run_id;
            if (!run_id.empty()) contexts.emplace(run_id, record.envelope.a2a_context_id);
        }
    }

    for (auto handle : program_adapter->recover_pending()) {
        const auto task_id = handle.run_id();
        const auto context_it = contexts.find(task_id);
        const auto context_id =
            context_it == contexts.end() || context_it->second.empty() ? task_id
                                                                        : context_it->second;
        auto task = program_adapter->task_snapshot(handle, task_id, context_id);
        const bool terminal = is_terminal(task.status.state);

        {
            std::lock_guard lk(tasks_mu);
            if (tasks.contains(task_id) || program_runs.contains(task_id)) {
                throw ProgramA2ARequestError(
                    "Recovered Program task collides with an existing A2A task identity");
            }
            tasks[task_id] = std::move(task);
            touch_task_unlocked(task_id);
            if (!terminal) {
                program_runs.emplace(task_id, ProgramActiveRun{std::move(handle), next_generation++});
            }
            evict_lru_unlocked();
        }
        if (terminal) program_adapter->acknowledge_task(task_id);
    }
    {
        std::lock_guard lk(tasks_mu);
        program_recovery_complete = true;
    }
}

std::optional<CollaborationPeerIdentity>
A2AServer::Impl::authenticate_collaboration_request(const httplib::Request& request) const {
    if (!collaboration_authenticator) return std::nullopt;
    const auto authorization = request.get_header_value("Authorization");
    if (authorization.empty()) return std::nullopt;
    try {
        auto peer = collaboration_authenticator(authorization);
        if (!peer || peer->owner_scope.empty() || peer->agent_id.empty()) {
            return std::nullopt;
        }
        return peer;
    } catch (...) {
        // Authentication failures are intentionally indistinguishable from
        // absent credentials at the collaboration boundary.
        return std::nullopt;
    }
}

bool A2AServer::Impl::authorizes_collaboration_message(
    const Message& inbound,
    const std::optional<CollaborationPeerIdentity>& authenticated_peer) const {
    if (!has_collaboration_marker(inbound)) return true;
    if (!authenticated_peer || !program_adapter) return false;
    const auto& mailbox = program_adapter->mailbox();
    if (!mailbox) return false;
    try {
        const auto envelope = collaboration_from_message(inbound);
        return mailbox->authenticates_sender(envelope.link_id, *authenticated_peer);
    } catch (...) {
        return false;
    }
}

CollaborationTaskAuthorization A2AServer::Impl::authorize_collaboration_task(
    std::string_view task_id,
    const std::optional<CollaborationPeerIdentity>& authenticated_peer,
    bool require_cancellation) const {
    if (!program_adapter) return CollaborationTaskAuthorization::NotLinked;
    const auto& mailbox = program_adapter->mailbox();
    if (!mailbox) return CollaborationTaskAuthorization::NotLinked;
    return mailbox->authorize_task(
        task_id,
        authenticated_peer.value_or(CollaborationPeerIdentity{}),
        require_cancellation);
}

Task A2AServer::Impl::run_program(
    const Message& inbound,
    const std::string& task_id,
    const std::string& context_id,
    std::optional<CollaborationPeerIdentity> authenticated_peer,
    std::function<void(const TaskStatusUpdateEvent&)> on_event) {
    if (!authorizes_collaboration_message(inbound, authenticated_peer)) {
        auto rejected = build_auth_required_task(task_id, context_id);
        if (on_event) {
            TaskStatusUpdateEvent event;
            event.task_id = task_id;
            event.context_id = context_id;
            event.status = rejected.status;
            event.final = true;
            on_event(event);
        }
        return rejected;
    }
    std::optional<Task> rejected;
    std::optional<Task> existing;
    std::uint64_t generation = 0;
    {
        std::lock_guard lk(tasks_mu);
        auto known = tasks.find(task_id);
        if (known != tasks.end() && is_terminal(known->second.status.state) &&
            !active_runs.contains(task_id) && !program_runs.contains(task_id)) {
            existing = known->second;
        } else if (active_runs.contains(task_id) || program_runs.contains(task_id)) {
            rejected = build_rejected_task(task_id, context_id, "task_id is already running");
        } else if (active_runs.size() + program_runs.size() >= max_inflight_runs) {
            rejected = build_rejected_task(
                task_id, context_id, "A2A server is at its in-flight task limit");
        } else {
            Task working;
            working.id = task_id;
            working.context_id = context_id;
            working.status.state = TaskState::Working;
            working.metadata = json{{"program_backed", true}, {"reconnect_safe", true}};
            tasks[task_id] = working;
            generation = next_generation++;
            touch_task_unlocked(task_id);
            evict_lru_unlocked();
        }
    }
    if (existing) return *existing;
    if (rejected) {
        if (on_event) {
            TaskStatusUpdateEvent event;
            event.task_id = task_id;
            event.context_id = context_id;
            event.status = rejected->status;
            event.final = true;
            on_event(event);
        }
        return *rejected;
    }

    if (on_event) {
        TaskStatusUpdateEvent event;
        event.task_id = task_id;
        event.context_id = context_id;
        event.status.state = TaskState::Working;
        event.metadata = json{{"program_backed", true}, {"reconnect_safe", true}};
        on_event(event);
    }

    Task result;
    std::optional<program::ProgramHandle> handle;
    try {
        handle.emplace(program_adapter->start(inbound, task_id, context_id));
        bool cancel_before_publish = false;
        {
            std::lock_guard lk(tasks_mu);
            auto task_it = tasks.find(task_id);
            cancel_before_publish = task_it != tasks.end() &&
                                    task_it->second.status.state == TaskState::Canceled;
            program_runs.emplace(task_id, ProgramActiveRun{*handle, generation});
        }
        if (cancel_before_publish) handle->cancel();
        const auto program_result = handle->wait();
        result = program_adapter->task_snapshot(*handle, task_id, context_id);
        (void)program_result;
    } catch (const ProgramA2ARequestError& error) {
        result = build_rejected_task(task_id, context_id, error.what());
    } catch (const std::exception& error) {
        result = build_failure_task(
            task_id, context_id, std::string("Program run failed: ") + error.what());
    }

    {
        std::lock_guard lk(tasks_mu);
        auto run_it = program_runs.find(task_id);
        if (run_it != program_runs.end() && run_it->second.generation == generation) {
            auto task_it = tasks.find(task_id);
            if (task_it != tasks.end() && task_it->second.status.state == TaskState::Canceled) {
                result = task_it->second;
            } else {
                tasks[task_id] = result;
            }
            touch_task_unlocked(task_id);
            program_runs.erase(run_it);
        } else {
            auto task_it = tasks.find(task_id);
            if (task_it == tasks.end() || task_it->second.status.state != TaskState::Canceled) {
                tasks[task_id] = result;
            } else {
                result = task_it->second;
            }
            touch_task_unlocked(task_id);
        }
    }
    if (handle && is_terminal(result.status.state)) program_adapter->acknowledge_task(task_id);
    if (on_event) {
        TaskStatusUpdateEvent event;
        event.task_id = task_id;
        event.context_id = context_id;
        event.status = result.status;
        event.metadata = result.metadata;
        event.final = true;
        on_event(event);
    }
    return result;
}
#endif

void A2AServer::Impl::handle_message_send(const httplib::Request& req,
                                          const neograph::json& params,
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

#ifdef NEOGRAPH_A2A_PROGRAM
    auto task = program_adapter
                    ? run_program(mp.message, task_id, context_id,
                                  authenticate_collaboration_request(req), /*on_event=*/{})
                    : run_graph(mp.message, task_id, context_id, /*on_event=*/{});
#else
    auto task = run_graph(mp.message, task_id, context_id, /*on_event=*/{});
#endif

    neograph::json tj;
    to_json(tj, task);
    res.status = 200;
    res.set_content(jsonrpc_result(std::move(tj), id).dump(),
                    "application/json");
}

void A2AServer::Impl::handle_message_stream(const httplib::Request& req,
                                            const neograph::json& params,
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
#ifdef NEOGRAPH_A2A_PROGRAM
    auto authenticated_peer = authenticate_collaboration_request(req);
#endif

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
        [self, inbound, task_id, context_id, rpc_id
#ifdef NEOGRAPH_A2A_PROGRAM
         , authenticated_peer
#endif
        ](size_t /*offset*/, httplib::DataSink& sink) {

            auto emit = [&](const TaskStatusUpdateEvent& ev) {
                neograph::json env_json;
                to_json(env_json, ev);
                auto env = jsonrpc_result(std::move(env_json), rpc_id);
                std::string frame = "data: " + env.dump() + "\n\n";
                sink.write(frame.data(), frame.size());
            };

            try {
#ifdef NEOGRAPH_A2A_PROGRAM
                auto task = self->program_adapter
                                ? self->run_program(
                                      inbound, task_id, context_id, authenticated_peer, emit)
                                : self->run_graph(inbound, task_id, context_id, emit);
#else
                auto task = self->run_graph(inbound, task_id, context_id, emit);
#endif
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

void A2AServer::Impl::handle_tasks_get(const httplib::Request& req,
                                       const neograph::json& params,
                                       const neograph::json& id,
                                       httplib::Response& res) {
    auto task_id = params.value("id", std::string());
#ifdef NEOGRAPH_A2A_PROGRAM
    if (authorize_collaboration_task(
            task_id, authenticate_collaboration_request(req), false) ==
        CollaborationTaskAuthorization::Unauthorized) {
        res.status = 200;
        res.set_content(jsonrpc_error(-32001, "Task not found", id).dump(),
                        "application/json");
        return;
    }
#endif
    Task t;
    bool found = false;
    bool live_program_active = false;
#ifdef NEOGRAPH_A2A_PROGRAM
    std::optional<program::ProgramHandle> live_program;
    std::string                            live_context;
    std::uint64_t                          live_generation = 0;
#endif
    {
        std::lock_guard lk(tasks_mu);
#ifdef NEOGRAPH_A2A_PROGRAM
        if (program_adapter) {
            if (const auto run_it = program_runs.find(task_id); run_it != program_runs.end()) {
                live_program        = run_it->second.handle;
                live_generation     = run_it->second.generation;
                live_program_active = true;
                if (const auto task_it = tasks.find(task_id); task_it != tasks.end()) {
                    live_context = task_it->second.context_id;
                }
            }
        }
#endif
        if (!live_program_active) {
            if (const auto it = tasks.find(task_id); it != tasks.end()) {
                t = it->second;
                found = true;
                touch_task_unlocked(task_id);  // tasks/get is a recency signal
            }
        }
    }
#ifdef NEOGRAPH_A2A_PROGRAM
    if (live_program) {
        try {
            t = program_adapter->task_snapshot(
                *live_program, task_id,
                live_context.empty() ? std::string_view{task_id} : std::string_view{live_context});
            const bool terminal = is_terminal(t.status.state);
            {
                std::lock_guard lk(tasks_mu);
                const auto run_it = program_runs.find(task_id);
                if (run_it != program_runs.end() && run_it->second.generation == live_generation) {
                    const auto task_it = tasks.find(task_id);
                    if (task_it != tasks.end() &&
                        task_it->second.status.state == TaskState::Canceled) {
                        t = task_it->second;
                    } else {
                        tasks[task_id] = t;
                    }
                    touch_task_unlocked(task_id);
                    if (terminal) program_runs.erase(run_it);
                    evict_lru_unlocked();
                }
            }
            if (terminal) program_adapter->acknowledge_task(task_id);
            found = true;
        } catch (const std::exception&) {
            // Preserve the stored task projection or owner-scoped not-found
            // result; ProgramRuntime owns detailed recovery diagnostics.
        }
    }
    if (!found && program_adapter) {
        try {
            t = program_adapter->reconnect_task(task_id);
            const bool terminal = is_terminal(t.status.state);
            {
                std::lock_guard lk(tasks_mu);
                tasks[task_id] = t;
                touch_task_unlocked(task_id);
                evict_lru_unlocked();
            }
            if (terminal) program_adapter->acknowledge_task(task_id);
            found = true;
        } catch (const std::exception&) {
            // Preserve the legacy not-found JSON-RPC contract. ProgramRuntime
            // deliberately does not expose cross-owner existence details.
        }
    }
#endif
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

void A2AServer::Impl::handle_tasks_cancel(const httplib::Request& req,
                                          const neograph::json& params,
                                          const neograph::json& id,
                                          httplib::Response& res) {
    auto task_id = params.value("id", std::string());
#ifdef NEOGRAPH_A2A_PROGRAM
    if (authorize_collaboration_task(
            task_id, authenticate_collaboration_request(req), true) ==
        CollaborationTaskAuthorization::Unauthorized) {
        res.status = 200;
        res.set_content(jsonrpc_error(-32001, "Task not found", id).dump(),
                        "application/json");
        return;
    }
#endif
    Task t;
    bool found = false;
    std::shared_ptr<neograph::graph::CancelToken> cancel_token;
#ifdef NEOGRAPH_A2A_PROGRAM
    std::optional<program::ProgramHandle> program_handle;
#endif
    {
        std::lock_guard lk(tasks_mu);
        auto it = tasks.find(task_id);
        if (it != tasks.end()) {
            t = it->second;
            found = true;
            if (!is_terminal(t.status.state)) {
                if (auto run_it = active_runs.find(task_id);
                    run_it != active_runs.end()) {
                    cancel_token = run_it->second.cancel_token;
                }
                t.status.state = TaskState::Canceled;
                tasks[task_id] = t;
                touch_task_unlocked(task_id);
            }
        }
    }
#ifdef NEOGRAPH_A2A_PROGRAM
    if (!found && program_adapter) {
        try {
            program_handle.emplace(program_adapter->reconnect(task_id));
            t = program_adapter->task_snapshot(*program_handle, task_id, task_id);
            found = true;
            std::lock_guard lk(tasks_mu);
            tasks[task_id] = t;
            touch_task_unlocked(task_id);
        } catch (const std::exception&) {
            // The not-found response remains deliberately owner-scoped.
        }
    }
#endif
    if (!found) {
        res.status = 200;
        res.set_content(jsonrpc_error(-32001, "Task not found", id).dump(),
                        "application/json");
        return;
    }
    if (cancel_token) cancel_token->cancel();
#ifdef NEOGRAPH_A2A_PROGRAM
    if (!cancel_token && program_adapter) {
        std::lock_guard lk(tasks_mu);
        if (const auto run_it = program_runs.find(task_id); run_it != program_runs.end()) {
            program_handle = run_it->second.handle;
        }
    }
    if (program_handle) {
        program_handle->cancel();
        t.status.state = TaskState::Canceled;
        std::lock_guard lk(tasks_mu);
        tasks[task_id] = t;
        touch_task_unlocked(task_id);
    }
#endif
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

#ifdef NEOGRAPH_A2A_PROGRAM
A2AServer::A2AServer(std::shared_ptr<ProgramAgentAdapter> adapter,
                     AgentCard card,
                     CollaborationAuthenticator collaboration_authenticator)
    : impl_(std::make_unique<Impl>()) {
    if (!adapter) throw std::invalid_argument("A2AServer: ProgramAgentAdapter is null");
    impl_->program_adapter = std::move(adapter);
    impl_->collaboration_authenticator = std::move(collaboration_authenticator);
    impl_->card = std::move(card);
    impl_->register_routes();
}

A2AServer::A2AServer(std::shared_ptr<neograph::program::ProgramRuntime> runtime,
                     neograph::program::ProgramVersion version,
                     std::string owner_scope,
                     AgentCard card,
                     std::shared_ptr<CollaborationMailbox> mailbox,
                     CollaborationAuthenticator collaboration_authenticator)
    : A2AServer(std::make_shared<ProgramAgentAdapter>(
                    std::move(runtime), std::move(version), std::move(owner_scope),
                    std::move(mailbox)),
                std::move(card), std::move(collaboration_authenticator)) {}

A2AServer::A2AServer(std::shared_ptr<neograph::program::ProgramRuntime> runtime,
                     neograph::program::ProgramVersion version,
                     AgentCard card,
                     std::string owner_scope,
                     std::shared_ptr<CollaborationMailbox> mailbox,
                     CollaborationAuthenticator collaboration_authenticator)
    : A2AServer(std::move(runtime), std::move(version), std::move(owner_scope),
                std::move(card), std::move(mailbox),
                std::move(collaboration_authenticator)) {}
#endif

A2AServer::~A2AServer() { stop(); }

bool A2AServer::start(const std::string& host, int port) {
    if (port == 0) {
        impl_->bound_port = impl_->svr.bind_to_any_port(host);
        if (impl_->bound_port < 0) return false;
    } else {
        if (!impl_->svr.bind_to_port(host, port)) return false;
        impl_->bound_port = port;
    }
#ifdef NEOGRAPH_A2A_PROGRAM
    try {
        impl_->recover_program_tasks();
    } catch (...) {
        impl_->svr.stop();
        impl_->bound_port = 0;
        throw;
    }
#endif
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
#ifdef NEOGRAPH_A2A_PROGRAM
    try {
        impl_->recover_program_tasks();
    } catch (...) {
        impl_->svr.stop();
        impl_->bound_port = 0;
        throw;
    }
#endif
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
