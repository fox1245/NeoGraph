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

namespace {

asio::awaitable<std::pair<bool, json>>
try_rpc(A2AClient& self, const std::string& method, const json& params) {
    // co_await is forbidden inside catch blocks (g++14, clang
    // matches), so the try wraps a delegated awaitable and returns a
    // (ok, value-or-error-message) pair. Caller dispatches outside.
    try {
        json r = co_await self.rpc_call_async(method, params);
        co_return std::make_pair(true, std::move(r));
    } catch (const std::exception& e) {
        json err = std::string(e.what());
        co_return std::make_pair(false, std::move(err));
    }
}

}  // namespace

asio::awaitable<json> A2AClient::rpc_call_with_fallback(
    const std::string& v1_method,
    const std::string& v03_method,
    const json& params) {

    // Slash-form first — this is the JSON Schema spec form (a2a-js
    // canonical) and is also accepted by a2a-sdk Python ≥1.0.0 when
    // run with `enable_v0_3_compat=True`. PascalCase is the fallback
    // for v1-only deployments. The two protocol generations differ
    // not just in method name but in body shape (PascalCase form
    // doesn't accept the `kind` discriminator); slash-form keeps a
    // single body shape across both server generations.
    auto [ok, value] = co_await try_rpc(*this, v03_method, params);
    if (ok) co_return value;

    std::string err_msg = value.is_string() ? value.get<std::string>() : value.dump();
    bool method_not_found =
        err_msg.find("Method not found") != std::string::npos
        || err_msg.find("-32601")          != std::string::npos;
    if (!method_not_found) {
        throw std::runtime_error(err_msg);
    }
    co_return co_await rpc_call_async(v1_method, params);
}

namespace {
// Two A2A protocol generations are deployed in the wild (both under the
// a2aproject org):
//   - v1   : PascalCase method names ("SendMessage", "GetTask", ...)
//            used by a2a-sdk Python ≥1.0.0.
//   - v0.3 : slash-form method names ("message/send", "tasks/get", ...)
//            still used by a2a-js HEAD and pre-v1 deployments.
// We default to PascalCase and fall back to slash-form on "method not
// found", so a single client connects to either generation.
struct MethodPair { const char* v1; const char* v03; };
constexpr MethodPair k_send_message  = {"SendMessage",  "message/send"};
constexpr MethodPair k_get_task      = {"GetTask",      "tasks/get"};
constexpr MethodPair k_cancel_task   = {"CancelTask",   "tasks/cancel"};
}  // namespace

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
    auto result = co_await rpc_call_with_fallback(
        k_send_message.v1, k_send_message.v03, p);
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
    auto result = co_await rpc_call_with_fallback(
        k_get_task.v1, k_get_task.v03, params);
    co_return coerce_to_task(result);
}

Task A2AClient::get_task(const std::string& task_id, int history_length) {
    return async::run_sync(get_task_async(task_id, history_length));
}

asio::awaitable<Task>
A2AClient::cancel_task_async(const std::string& task_id) {
    json params = {{"id", task_id}};
    auto result = co_await rpc_call_with_fallback(
        k_cancel_task.v1, k_cancel_task.v03, params);
    co_return coerce_to_task(result);
}

Task A2AClient::cancel_task(const std::string& task_id) {
    return async::run_sync(cancel_task_async(task_id));
}

// ---------------------------------------------------------------------------
// message/stream — SSE consumer
// ---------------------------------------------------------------------------
namespace {

/// Carve `data: {...}` frames out of an SSE byte stream. Holds a tail
/// buffer across calls so a frame split across two chunks survives.
struct SseFrameSplitter {
    std::string carry;

    /// Append @p chunk and call @p on_frame for each complete `data:`
    /// line found. Returns when the stream tail does not contain a
    /// terminator yet.
    void feed(std::string_view chunk,
              const std::function<void(std::string_view)>& on_frame) {
        carry.append(chunk);
        std::size_t pos = 0;
        for (;;) {
            auto end = carry.find("\n\n", pos);
            if (end == std::string::npos) break;
            std::string_view frame(carry.data() + pos, end - pos);
            // Each SSE event may have multiple lines: "event: ...\ndata: {...}".
            // We only care about the data field.
            std::size_t line_start = 0;
            while (line_start < frame.size()) {
                auto line_end = frame.find('\n', line_start);
                std::string_view line = (line_end == std::string::npos)
                                          ? frame.substr(line_start)
                                          : frame.substr(line_start, line_end - line_start);
                if (line.rfind("data:", 0) == 0) {
                    auto payload = line.substr(5);
                    while (!payload.empty() && payload.front() == ' ')
                        payload.remove_prefix(1);
                    on_frame(payload);
                }
                if (line_end == std::string::npos) break;
                line_start = line_end + 1;
            }
            pos = end + 2;
        }
        carry.erase(0, pos);
    }
};

}  // namespace

Task A2AClient::send_message_stream(const MessageSendParams& params,
                                    EventCallback on_event) {
    json p;
    to_json(p, params);
    json body = {
        {"jsonrpc", "2.0"},
        {"id",      ++request_id_},
        {"method",  "message/stream"},
        {"params",  p},
    };
    auto body_str = body.dump();
    auto endpoint = async::split_async_endpoint(base_url_);
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"},
        {"Accept",       "text/event-stream"},
    };

    async::RequestOptions opts;
    opts.timeout = timeout_;

    Task last_task;
    SseFrameSplitter splitter;
    bool aborted = false;

    auto frame_handler = [&](std::string_view payload) {
        if (aborted) return;
        json frame_json;
        try {
            frame_json = json::parse(std::string(payload));
        } catch (...) {
            return;
        }
        json result = frame_json.contains("result")
                        ? frame_json["result"]
                        : frame_json;
        StreamEvent ev = parse_stream_event(result);
        if (ev.type == StreamEvent::Type::Task && ev.task) {
            last_task = *ev.task;
        }
        if (on_event && !on_event(ev)) aborted = true;
    };

    auto chunk_callback = [&](std::string_view chunk) {
        splitter.feed(chunk, frame_handler);
    };

    async::run_sync([&]() -> asio::awaitable<void> {
        auto ex = co_await asio::this_coro::executor;
        co_await async::async_post_stream(
            ex,
            endpoint.host,
            endpoint.port,
            endpoint.prefix.empty() ? "/" : endpoint.prefix,
            body_str,
            std::move(headers),
            endpoint.tls,
            chunk_callback,
            opts);
    }());

    return last_task;
}

Task A2AClient::send_message_stream(const std::string& text,
                                    EventCallback on_event,
                                    const std::string& task_id,
                                    const std::string& context_id) {
    MessageSendParams params;
    params.message.message_id = fresh_uuid_like();
    params.message.role       = Role::User;
    params.message.parts.push_back(Part::text_part(text));
    if (!task_id.empty())    params.message.task_id    = task_id;
    if (!context_id.empty()) params.message.context_id = context_id;
    return send_message_stream(params, std::move(on_event));
}

}  // namespace neograph::a2a
