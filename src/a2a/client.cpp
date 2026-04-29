#include <neograph/a2a/client.h>

#include <neograph/async/endpoint.h>
#include <neograph/async/http_client.h>
#include <neograph/async/run_sync.h>

#include <asio/awaitable.hpp>
#include <asio/this_coro.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

namespace neograph::a2a {

namespace {

constexpr auto kDiscoveryPath = "/.well-known/agent-card.json";

/// Strip a trailing "/.well-known/agent-card.json" if the user passed
/// the full discovery URL by accident — keeps `base_url` pointing at
/// the agent's primary endpoint so JSON-RPC POST lands on the right path.
std::string normalize_base_url(std::string url) {
    auto pos = url.find(kDiscoveryPath);
    if (pos != std::string::npos) url.erase(pos);
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

/// SSE-or-plain JSON parser. A2A Streamable HTTP can return either
/// the JSON-RPC envelope verbatim or wrapped in `data: {...}` SSE
/// frames; both round-trip through the same shape.
json parse_response_body(const std::string& body) {
    auto data_pos = body.find("data: ");
    if (data_pos == std::string::npos) {
        return json::parse(body);
    }
    auto json_start = data_pos + 6;
    auto json_end   = body.find('\n', json_start);
    auto json_str   = (json_end != std::string::npos)
                          ? body.substr(json_start, json_end - json_start)
                          : body.substr(json_start);
    return json::parse(json_str);
}

}  // namespace

A2AClient::A2AClient(std::string base_url)
    : base_url_(normalize_base_url(std::move(base_url))) {}

// ---------------------------------------------------------------------------
// JSON-RPC dispatch
// ---------------------------------------------------------------------------

asio::awaitable<json>
A2AClient::rpc_call_async(const std::string& method, const json& params) {
    json body = {
        {"jsonrpc", "2.0"},
        {"id",      ++request_id_},
        {"method",  method},
        {"params",  params},
    };
    auto body_str = body.dump();
    auto endpoint = async::split_async_endpoint(base_url_);

    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"},
        {"Accept",       "application/json, text/event-stream"},
    };

    async::RequestOptions opts;
    opts.timeout = timeout_;

    auto ex = co_await asio::this_coro::executor;
    async::HttpResponse res;
    try {
        res = co_await async::async_post(
            ex,
            endpoint.host,
            endpoint.port,
            endpoint.prefix.empty() ? "/" : endpoint.prefix,
            body_str,
            std::move(headers),
            endpoint.tls,
            opts);
    } catch (const std::system_error& e) {
        throw std::runtime_error(std::string("A2A request failed: ") + e.what());
    }

    if (res.status < 200 || res.status >= 300) {
        throw std::runtime_error(
            "A2A error (HTTP " + std::to_string(res.status) + "): " + res.body);
    }

    json resp;
    try {
        resp = parse_response_body(res.body);
    } catch (const json::exception& e) {
        throw std::runtime_error(
            std::string("A2A response not valid JSON: ") + e.what());
    }

    if (resp.contains("error") && !resp["error"].is_null()) {
        const auto& err = resp["error"];
        std::string msg = "A2A RPC error";
        if (err.is_object()) {
            if (err.contains("code"))    msg += " (code=" + err["code"].dump() + ")";
            if (err.contains("message")) msg += ": " + err.value("message", "");
        } else {
            msg += ": " + err.dump();
        }
        throw std::runtime_error(msg);
    }

    co_return resp.value("result", json::object());
}

json A2AClient::rpc_call(const std::string& method, const json& params) {
    return async::run_sync(rpc_call_async(method, params));
}

// ---------------------------------------------------------------------------
// AgentCard discovery
// ---------------------------------------------------------------------------

asio::awaitable<AgentCard>
A2AClient::fetch_agent_card_async(bool force) {
    if (!force && card_loaded_) co_return cached_card_;

    auto endpoint = async::split_async_endpoint(base_url_);
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Accept", "application/json"},
    };
    async::RequestOptions opts;
    opts.timeout = timeout_;

    auto ex = co_await asio::this_coro::executor;
    auto path = endpoint.prefix + kDiscoveryPath;

    async::HttpResponse res;
    try {
        res = co_await async::async_get(
            ex, endpoint.host, endpoint.port, path,
            std::move(headers), endpoint.tls, opts);
    } catch (const std::system_error& e) {
        throw std::runtime_error(
            std::string("A2A AgentCard discovery failed: ") + e.what());
    }

    if (res.status < 200 || res.status >= 300) {
        throw std::runtime_error(
            "A2A AgentCard discovery error (HTTP " + std::to_string(res.status)
            + "): " + res.body);
    }

    AgentCard card;
    try {
        from_json(json::parse(res.body), card);
    } catch (const json::exception& e) {
        throw std::runtime_error(
            std::string("A2A AgentCard not valid JSON: ") + e.what());
    }

    cached_card_ = card;
    card_loaded_ = true;
    co_return card;
}

AgentCard A2AClient::fetch_agent_card(bool force) {
    return async::run_sync(fetch_agent_card_async(force));
}

// ---------------------------------------------------------------------------
// message/send
// ---------------------------------------------------------------------------

namespace {
std::string fresh_uuid_like() {
    // Not cryptographic — just a unique ID per call. Spec only
    // requires uniqueness within the session.
    static std::atomic<std::uint64_t> counter{0};
    auto n = counter.fetch_add(1, std::memory_order_relaxed);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "ng-a2a-msg-%016llx",
                  static_cast<unsigned long long>(n));
    return buf;
}

/// Server may return either a Task object or a Message (for sync
/// completions that finish in a single round-trip). Coerce both into
/// a Task so callers see one shape.
Task coerce_to_task(const json& result) {
    if (result.is_null()) {
        Task t;
        t.status.state = TaskState::Failed;
        return t;
    }
    auto kind = result.value("kind", std::string());
    if (kind == "task") {
        Task t;
        from_json(result, t);
        return t;
    }

    if (kind == "message") {
        Task t;
        Message msg;
        from_json(result, msg);
        t.id          = msg.task_id.value_or("");
        t.context_id  = msg.context_id.value_or("");
        t.status.state   = TaskState::Completed;
        t.status.message = msg;
        t.history.push_back(std::move(msg));
        return t;
    }

    // Unknown — best effort: dump into a metadata-only Task.
    Task t;
    t.status.state = TaskState::Unknown;
    t.metadata     = result;
    return t;
}
}  // namespace

asio::awaitable<Task>
A2AClient::send_message_async(const MessageSendParams& params) {
    json p;
    to_json(p, params);
    auto result = co_await rpc_call_async("message/send", p);
    co_return coerce_to_task(result);
}

Task A2AClient::send_message_sync(const MessageSendParams& params) {
    return async::run_sync(send_message_async(params));
}

Task A2AClient::send_message_sync(const std::string& text,
                                  const std::string& task_id,
                                  const std::string& context_id) {
    MessageSendParams params;
    params.message.message_id = fresh_uuid_like();
    params.message.role       = Role::User;
    params.message.parts.push_back(Part::text_part(text));
    if (!task_id.empty())    params.message.task_id    = task_id;
    if (!context_id.empty()) params.message.context_id = context_id;
    return send_message_sync(params);
}

// ---------------------------------------------------------------------------
// tasks/get + tasks/cancel
// ---------------------------------------------------------------------------

asio::awaitable<Task>
A2AClient::get_task_async(const std::string& task_id, int history_length) {
    json params = {{"id", task_id}};
    if (history_length > 0) params["historyLength"] = history_length;
    auto result = co_await rpc_call_async("tasks/get", params);
    co_return coerce_to_task(result);
}

Task A2AClient::get_task(const std::string& task_id, int history_length) {
    return async::run_sync(get_task_async(task_id, history_length));
}

asio::awaitable<Task>
A2AClient::cancel_task_async(const std::string& task_id) {
    json params = {{"id", task_id}};
    auto result = co_await rpc_call_async("tasks/cancel", params);
    co_return coerce_to_task(result);
}

Task A2AClient::cancel_task(const std::string& task_id) {
    return async::run_sync(cancel_task_async(task_id));
}

}  // namespace neograph::a2a
