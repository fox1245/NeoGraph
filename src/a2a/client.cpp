#include <neograph/a2a/client.h>

#include "ids.h"

#include <neograph/async/endpoint.h>
#include <neograph/async/http_client.h>
#include <neograph/async/run_sync.h>

#include <asio/awaitable.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string_view>

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

/// True when a response declares a server-sent-event body: the media type of
/// its Content-Type, without parameters, is `text/event-stream`.
bool is_event_stream(const async::HttpResponse& response) {
    auto type = response.get_header("Content-Type");
    type = type.substr(0, type.find(';'));
    while (!type.empty() && (type.front() == ' ' || type.front() == '\t')) type.remove_prefix(1);
    while (!type.empty() && (type.back() == ' ' || type.back() == '\t')) type.remove_suffix(1);
    constexpr std::string_view kEventStream = "text/event-stream";
    return type.size() == kEventStream.size()
        && std::equal(type.begin(), type.end(), kEventStream.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == b;
           });
}

/// Carve `data:` events out of an SSE byte stream (WHATWG SSE framing).
/// Holds a tail buffer across calls so an event split across two chunks
/// survives. Line endings may be LF, CRLF or CR (sse-starlette, which backs
/// a2a-sdk, emits CRLF); they are normalised to LF before framing. The
/// `data:` lines of one event are joined with '\n'.
struct SseFrameSplitter {
    std::string carry;
    bool        prev_cr = false;

    void feed(std::string_view chunk,
              const std::function<void(std::string_view)>& on_frame) {
        for (char c : chunk) {
            if (c == '\r') {
                carry.push_back('\n');
                prev_cr = true;
            } else if (c == '\n' && prev_cr) {
                prev_cr = false;  // second half of a CRLF pair
            } else {
                carry.push_back(c);
                prev_cr = false;
            }
        }
        std::size_t pos = 0;
        for (;;) {
            auto end = carry.find("\n\n", pos);
            if (end == std::string::npos) break;
            emit(std::string_view(carry.data() + pos, end - pos), on_frame);
            pos = end + 2;
        }
        carry.erase(0, pos);
    }

    /// The stream ended: an unterminated last event is still an event.
    void finish(const std::function<void(std::string_view)>& on_frame) {
        if (!carry.empty()) emit(carry, on_frame);
        carry.clear();
    }

  private:
    static void emit(std::string_view event,
                     const std::function<void(std::string_view)>& on_frame) {
        std::string data;
        bool has_data = false;
        std::size_t line_start = 0;
        while (line_start <= event.size()) {
            auto line_end = event.find('\n', line_start);
            std::string_view line = (line_end == std::string_view::npos)
                                      ? event.substr(line_start)
                                      : event.substr(line_start, line_end - line_start);
            if (line.rfind("data:", 0) == 0) {
                auto payload = line.substr(5);
                if (!payload.empty() && payload.front() == ' ') payload.remove_prefix(1);
                if (has_data) data.push_back('\n');
                data.append(payload);
                has_data = true;
            }
            if (line_end == std::string_view::npos) break;
            line_start = line_end + 1;
        }
        if (has_data) on_frame(data);
    }
};

/// The JSON-RPC envelope of a unary reply. The body is JSON unless the
/// response says it is an event stream (`Content-Type: text/event-stream`),
/// in which case the envelope is the first `data:` event. What the body
/// contains never decides: an agent's text may well hold "data: ".
json parse_response_body(const async::HttpResponse& response) {
    if (!is_event_stream(response)) return json::parse(response.body);
    SseFrameSplitter splitter;
    std::optional<std::string> first;
    const auto take = [&first](std::string_view payload) {
        if (!first) first = std::string(payload);
    };
    splitter.feed(response.body, take);
    splitter.finish(take);
    if (!first) throw json::parse_error("event stream without a data event");
    return json::parse(*first);
}

constexpr int kMethodNotFound = -32601;
constexpr int kVersionNotSupported = -32009;
constexpr std::size_t kMaxStreamErrorBody = 1u << 20;

/// A non-2xx HTTP reply. what() keeps the historical "A2A error (HTTP n)"
/// text; the status lets the probe treat a 400 as "try the other dialect".
class HttpStatusError : public std::runtime_error {
  public:
    HttpStatusError(int status, const std::string& body)
        : std::runtime_error("A2A error (HTTP " + std::to_string(status) + "): " + body),
          status_(status) {}
    int status() const noexcept { return status_; }

  private:
    int status_;
};

/// Without a card the client probes 1.0 first. These replies mean "this
/// agent does not speak that dialect" (spec 3.6.2 / 9.5), so the other one
/// is tried: method not found, version not supported, or an HTTP 400.
bool probe_falls_back(int rpc_code, int http_status) {
    return rpc_code == kMethodNotFound || rpc_code == kVersionNotSupported
        || http_status == 400;
}

struct Origin {
    bool        tls = false;
    std::string host;
    std::string port;
    bool operator==(const Origin&) const = default;
};

std::optional<Origin> origin_of(const std::string& url) {
    try {
        auto endpoint = async::split_async_endpoint(url);
        std::transform(endpoint.host.begin(), endpoint.host.end(), endpoint.host.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return Origin{endpoint.tls, endpoint.host, endpoint.port};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// An origin for a log line. The host comes from a remote card, so anything
/// but printable ASCII is replaced.
std::string printable_origin(const Origin& origin) {
    std::string out = origin.tls ? "https://" : "http://";
    out += origin.host.find(':') != std::string::npos ? "[" + origin.host + "]" : origin.host;
    out += ":" + origin.port;
    for (auto& c : out) {
        if (c < 0x20 || c > 0x7e) c = '?';
    }
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size()
        && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x))
                   == std::tolower(static_cast<unsigned char>(y));
           });
}

/// "1.0" / "1.0.2" -> V1_0; "0.3" / "0.3.0" / "" (legacy card without a
/// version) -> V0_3; anything else is not spoken by this client.
std::optional<WireDialect> dialect_for_version(std::string_view v) {
    if (v.empty()) return WireDialect::V0_3;
    int major = 0;
    std::size_t i = 0;
    for (; i < v.size() && std::isdigit(static_cast<unsigned char>(v[i])); ++i) {
        major = major * 10 + (v[i] - '0');
        if (major > 1000) return std::nullopt;
    }
    if (i == 0) return std::nullopt;
    if (major == 1) return WireDialect::V1_0;
    if (major == 0) return WireDialect::V0_3;
    return std::nullopt;
}

std::string strip_trailing_slashes(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

struct Chosen {
    WireDialect dialect;
    std::string tenant;
    std::string url;
};

/// The endpoint for an interface URL under the client's origin policy: the
/// interface URL itself when it is usable and on `base_url`'s origin (or the
/// caller opted into cross-origin interfaces), else `base_url`.
std::string interface_endpoint(const std::string& interface_url, const std::string& base_url,
                               bool allow_cross_origin) {
    if (interface_url.empty()) return base_url;
    std::string candidate = interface_url;
    if (candidate.front() == '/') {
        // A path-only URL is relative to the card's origin.
        try {
            const auto base = async::split_async_endpoint(base_url);
            candidate = std::string(base.tls ? "https://" : "http://")
                      + (base.host.find(':') != std::string::npos ? "[" + base.host + "]"
                                                                  : base.host)
                      + ":" + base.port + candidate;
        } catch (const std::exception&) {
            return base_url;
        }
    } else if (!async::has_explicit_http_scheme(candidate)) {
        return base_url;
    }
    const auto target = origin_of(candidate);
    if (!target) return base_url;
    const auto base = origin_of(base_url);
    if (!allow_cross_origin && base != target) {
        // Keep the address the caller chose, but say why the card's
        // interface is not used. FILE* stderr: see graph_executor.cpp.
        const auto here = base ? printable_origin(*base) : std::string("the client's base_url");
        std::fprintf(stderr,
                     "[neograph] warning: the A2A AgentCard served at %s names an interface "
                     "on another origin (%s); sending to %s instead. Call "
                     "A2AClient::set_allow_cross_origin_interfaces(true) to follow it.\n",
                     here.c_str(), printable_origin(*target).c_str(), here.c_str());
        std::fflush(stderr);
        return base_url;
    }
    return candidate;
}

/// AgentCard -> wire dialect and endpoint (A2A spec §8.3.2): first supported
/// JSONRPC interface in card order, preferring one whose URL is the client's
/// `base_url` when the card lists several.
Chosen select_from_card(const AgentCard& card, const std::string& base_url,
                        bool allow_cross_origin) {
    const AgentInterface* first = nullptr;
    WireDialect first_dialect = WireDialect::V0_3;
    const AgentInterface* same_url = nullptr;
    WireDialect same_url_dialect = WireDialect::V0_3;
    std::string offered;
    for (const auto& iface : card.supported_interfaces) {
        if (!offered.empty()) offered += ", ";
        offered += (iface.protocol_binding.empty() ? "?" : iface.protocol_binding)
                 + " " + (iface.protocol_version.empty() ? "?" : iface.protocol_version)
                 + " @ " + iface.url;
        if (!iequals(iface.protocol_binding, "JSONRPC")) continue;
        auto dialect = dialect_for_version(iface.protocol_version);
        if (!dialect) continue;
        if (!first) { first = &iface; first_dialect = *dialect; }
        if (!same_url && strip_trailing_slashes(iface.url) == base_url) {
            same_url = &iface;
            same_url_dialect = *dialect;
        }
    }
    if (same_url) {
        return {same_url_dialect, same_url->tenant,
                interface_endpoint(same_url->url, base_url, allow_cross_origin)};
    }
    if (first) {
        return {first_dialect, first->tenant,
                interface_endpoint(first->url, base_url, allow_cross_origin)};
    }
    if (card.supported_interfaces.empty()) return {WireDialect::V0_3, {}, base_url};
    throw std::runtime_error(
        "A2A agent at " + base_url + " declares no compatible interface: this "
        "client speaks the JSONRPC binding at protocol version 1.x or 0.x, but "
        "the AgentCard offers [" + offered + "]");
}

const char* dialect_version_header(WireDialect d) {
    return d == WireDialect::V1_0 ? "1.0" : nullptr;
}

}  // namespace

A2AClient::A2AClient(std::string base_url)
    : base_url_(normalize_base_url(std::move(base_url))) {}

void A2AClient::set_timeout(std::chrono::seconds t) {
    std::lock_guard<std::mutex> lock(*state_mutex_);
    timeout_ = t;
}

void A2AClient::set_allow_cross_origin_interfaces(bool allow) {
    std::lock_guard<std::mutex> lock(*state_mutex_);
    allow_cross_origin_ = allow;
    if (card_loaded_) selection_.reset();  // re-select under the new policy
}

std::string A2AClient::endpoint_url() const {
    std::lock_guard<std::mutex> lock(*state_mutex_);
    return selection_ ? selection_->url : base_url_;
}

void A2AClient::set_authorization_header(std::string authorization_header) {
    std::lock_guard<std::mutex> lock(*state_mutex_);
    authorization_header_ = std::move(authorization_header);
}

std::string A2AClient::request_authorization_header() const {
    std::lock_guard<std::mutex> lock(*state_mutex_);
    return authorization_header_;
}

std::chrono::seconds A2AClient::request_timeout() const {
    std::lock_guard<std::mutex> lock(*state_mutex_);
    return timeout_;
}

std::optional<WireDialect> A2AClient::wire_dialect() const {
    std::lock_guard<std::mutex> lock(*state_mutex_);
    if (selection_) return selection_->dialect;
    return std::nullopt;
}

std::optional<A2AClient::Selection> A2AClient::resolve_selection() {
    std::lock_guard<std::mutex> lock(*state_mutex_);
    if (selection_) return selection_;
    if (!card_loaded_) return std::nullopt;
    // Selection and card refresh share the lock; an old card cannot install
    // its selection after a newer fetch invalidates it. No network I/O here.
    auto chosen = select_from_card(cached_card_, base_url_, allow_cross_origin_);
    selection_ = Selection{chosen.dialect, std::move(chosen.tenant), std::move(chosen.url)};
    return selection_;
}

asio::awaitable<std::optional<A2AClient::Selection>> A2AClient::select_async() {
    if (auto sel = resolve_selection()) co_return sel;
    bool need_card = false;
    {
        std::lock_guard<std::mutex> lock(*state_mutex_);
        need_card = !card_loaded_ && !card_unavailable_;
    }
    if (need_card) {
        // Fetch the card before the first call: the interface it selects
        // names both the dialect and the endpoint.
        bool failed = false;
        try {
            (void)co_await fetch_agent_card_async(false);
        } catch (const std::exception&) {
            failed = true;
        }
        if (failed) {
            std::lock_guard<std::mutex> lock(*state_mutex_);
            card_unavailable_ = true;
        }
    }
    co_return resolve_selection();
}

void A2AClient::remember_probe(WireDialect dialect) {
    std::lock_guard<std::mutex> lock(*state_mutex_);
    if (!card_loaded_ && !selection_) selection_ = Selection{dialect, {}, base_url_};
}

// ---------------------------------------------------------------------------
// JSON-RPC dispatch
// ---------------------------------------------------------------------------

asio::awaitable<json>
A2AClient::rpc_call_async(const std::string& method, const json& params,
                          std::optional<WireDialect> dialect) {
    co_return co_await post_rpc(endpoint_url(), method, params, dialect);
}

asio::awaitable<json>
A2AClient::post_rpc(const std::string& url, const std::string& method, const json& params,
                    std::optional<WireDialect> dialect) {
    json body = {
        {"jsonrpc", "2.0"},
        {"id",      request_id_.fetch_add(1, std::memory_order_relaxed) + 1},
        {"method",  method},
        {"params",  params},
    };
    auto body_str = body.dump();
    auto endpoint = async::split_async_endpoint(url);

    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"},
        {"Accept",       "application/json, text/event-stream"},
    };
    if (dialect) {
        if (auto v = dialect_version_header(*dialect)) headers.emplace_back("A2A-Version", v);
    }
    if (auto authorization = request_authorization_header(); !authorization.empty()) {
        headers.emplace_back("Authorization", std::move(authorization));
    }

    async::RequestOptions opts;
    opts.timeout = request_timeout();

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
        throw HttpStatusError(res.status, res.body);
    }

    json resp;
    try {
        resp = parse_response_body(res);
    } catch (const json::exception& e) {
        throw std::runtime_error(
            std::string("A2A response not valid JSON: ") + e.what());
    }

    if (resp.contains("error") && !resp["error"].is_null()) {
        const auto& err = resp["error"];
        std::string msg = "A2A RPC error";
        int code = 0;
        if (err.is_object()) {
            if (err.contains("code")) {
                msg += " (code=" + err["code"].dump() + ")";
                if (err["code"].is_number_integer()) code = err["code"].get<int>();
            }
            if (err.contains("message")) msg += ": " + err.value("message", "");
        } else {
            msg += ": " + err.dump();
        }
        throw A2ARpcError(code, msg);
    }

    co_return resp.value("result", json::object());
}

json A2AClient::rpc_call(const std::string& method, const json& params,
                         std::optional<WireDialect> dialect) {
    return async::run_sync(rpc_call_async(method, params, dialect));
}

asio::awaitable<json> A2AClient::call_method(
    const char* v1_method, const char* v03_method,
    const std::function<json(WireDialect, const std::string& tenant)>& build) {

    auto method_for = [&](WireDialect d) {
        return std::string(d == WireDialect::V1_0 ? v1_method : v03_method);
    };

    if (auto sel = co_await select_async()) {
        co_return co_await post_rpc(
            sel->url, method_for(sel->dialect), build(sel->dialect, sel->tenant), sel->dialect);
    }

    // No card could be fetched: probe. 1.0 (the primary version) first; the
    // two generations differ in body shape, not just the method name, so each
    // attempt builds its own body.
    json value;
    bool ok = false;
    int code = 0;
    int http_status = 0;
    std::string message;
    try {
        value = co_await post_rpc(base_url_, method_for(WireDialect::V1_0),
                                  build(WireDialect::V1_0, std::string()), WireDialect::V1_0);
        ok = true;
    } catch (const A2ARpcError& e) {
        code = e.code();
        message = e.what();
    } catch (const HttpStatusError& e) {
        http_status = e.status();
        message = e.what();
    }
    if (ok) {
        remember_probe(WireDialect::V1_0);
        co_return value;
    }
    if (!probe_falls_back(code, http_status)) {
        if (code != 0) throw A2ARpcError(code, message);
        throw std::runtime_error(message);
    }
    auto result = co_await post_rpc(base_url_, method_for(WireDialect::V0_3),
                                    build(WireDialect::V0_3, std::string()), WireDialect::V0_3);
    remember_probe(WireDialect::V0_3);
    co_return result;
}

namespace {
// Method names per wire generation: 1.0 is PascalCase (a2a-sdk >= 1.0,
// spec §9.1); 0.3 is slash-form (a2a-js, pre-1.0 deployments).
struct MethodPair { const char* v1; const char* v03; };
constexpr MethodPair k_send_message  = {"SendMessage",  "message/send"};
constexpr MethodPair k_get_task      = {"GetTask",      "tasks/get"};
constexpr MethodPair k_cancel_task   = {"CancelTask",   "tasks/cancel"};
constexpr MethodPair k_send_stream   = {"SendStreamingMessage", "message/stream"};
}  // namespace
// ---------------------------------------------------------------------------
// AgentCard discovery
// ---------------------------------------------------------------------------

asio::awaitable<AgentCard>
A2AClient::fetch_agent_card_async(bool force) {
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(*state_mutex_);
            if (!force && card_loaded_) co_return cached_card_;
            if (!card_loading_) {
                card_loading_ = true;
                break;
            }
        }
        auto timer = asio::steady_timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }

    std::function<void()> release_loading = [this] {
        std::lock_guard<std::mutex> lock(*state_mutex_);
        card_loading_ = false;
    };
    struct LoadingGuard {
        std::function<void()> release;
        ~LoadingGuard() { if (release) release(); }
    } guard{release_loading};

    auto endpoint = async::split_async_endpoint(base_url_);
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Accept", "application/json"},
    };
    async::RequestOptions opts;
    opts.timeout = request_timeout();

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

    {
        std::lock_guard<std::mutex> lock(*state_mutex_);
        cached_card_ = card;
        card_loaded_ = true;
        card_unavailable_ = false;
        card_loading_ = false;
        selection_.reset();  // re-select from the fresh card on the next RPC
    }
    guard.release = {};
    co_return card;
}

AgentCard A2AClient::fetch_agent_card(bool force) {
    return async::run_sync(fetch_agent_card_async(force));
}

// ---------------------------------------------------------------------------
// message/send
// ---------------------------------------------------------------------------

namespace {
/// messageId is the sender's unique id (spec 4.1.4); agents may de-duplicate
/// on it, so it must not repeat across processes: a random UUID.
std::string fresh_message_id() { return detail::random_uuid(); }
}  // namespace

asio::awaitable<Task>
A2AClient::send_message_async(const MessageSendParams& params) {
    auto result = co_await call_method(
        k_send_message.v1, k_send_message.v03,
        [&params](WireDialect d, const std::string& tenant) {
            json p;
            to_json(p, params, d);
            if (d == WireDialect::V1_0 && !tenant.empty()) p["tenant"] = tenant;
            return p;
        });
    co_return task_from_result(result);
}

Task A2AClient::send_message_sync(const MessageSendParams& params) {
    return async::run_sync(send_message_async(params));
}

Task A2AClient::send_message_sync(const std::string& text,
                                  const std::string& task_id,
                                  const std::string& context_id) {
    MessageSendParams params;
    params.message.message_id = fresh_message_id();
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
    auto result = co_await call_method(
        k_get_task.v1, k_get_task.v03,
        [&](WireDialect d, const std::string& tenant) {
            json params = {{"id", task_id}};
            if (history_length > 0) params["historyLength"] = history_length;
            if (d == WireDialect::V1_0 && !tenant.empty()) params["tenant"] = tenant;
            return params;
        });
    co_return task_from_result(result);
}

Task A2AClient::get_task(const std::string& task_id, int history_length) {
    return async::run_sync(get_task_async(task_id, history_length));
}

asio::awaitable<Task>
A2AClient::cancel_task_async(const std::string& task_id) {
    auto result = co_await call_method(
        k_cancel_task.v1, k_cancel_task.v03,
        [&](WireDialect d, const std::string& tenant) {
            json params = {{"id", task_id}};
            if (d == WireDialect::V1_0 && !tenant.empty()) params["tenant"] = tenant;
            return params;
        });
    co_return task_from_result(result);
}

Task A2AClient::cancel_task(const std::string& task_id) {
    return async::run_sync(cancel_task_async(task_id));
}

// ---------------------------------------------------------------------------
// message/stream — SSE consumer
// ---------------------------------------------------------------------------
namespace {

/// Fold one stream event into the Task the caller receives at the end.
/// 1.0 streams have no terminal Task frame: the initial Task plus status /
/// artifact updates *are* the result. 0.3 servers end with a full Task,
/// which simply replaces the accumulation.
void accumulate(Task& acc, const StreamEvent& ev) {
    switch (ev.type) {
        case StreamEvent::Type::Task:
            if (ev.task) acc = *ev.task;
            break;
        case StreamEvent::Type::StatusUpdate: {
            const auto& u = *ev.status_update;
            if (acc.id.empty())         acc.id = u.task_id;
            if (acc.context_id.empty()) acc.context_id = u.context_id;
            acc.status = u.status;
            // As the SDK task managers do: an update's metadata (a failure
            // code, the invocation status) becomes the task's.
            if (u.metadata.is_object() && !u.metadata.empty()) {
                if (!acc.metadata.is_object()) acc.metadata = json::object();
                for (auto [key, value] : u.metadata.items()) acc.metadata[key] = value;
            }
            if (u.status.message) {
                const auto& m = *u.status.message;
                if (acc.history.empty() || m.message_id.empty()
                    || acc.history.back().message_id != m.message_id) {
                    acc.history.push_back(m);
                }
            }
            break;
        }
        case StreamEvent::Type::ArtifactUpdate: {
            const auto& u = *ev.artifact_update;
            if (acc.id.empty())         acc.id = u.task_id;
            if (acc.context_id.empty()) acc.context_id = u.context_id;
            auto it = std::find_if(acc.artifacts.begin(), acc.artifacts.end(),
                [&](const Artifact& a) { return a.artifact_id == u.artifact.artifact_id; });
            if (it == acc.artifacts.end()) {
                acc.artifacts.push_back(u.artifact);
            } else if (u.append) {
                it->parts.insert(it->parts.end(), u.artifact.parts.begin(),
                                 u.artifact.parts.end());
            } else {
                *it = u.artifact;
            }
            break;
        }
    }
}

}  // namespace

Task A2AClient::stream_once(WireDialect dialect, const std::string& tenant,
                            const std::string& url, const MessageSendParams& params,
                            const EventCallback& on_event, bool& saw_events) {
    json p;
    to_json(p, params, dialect);
    if (dialect == WireDialect::V1_0 && !tenant.empty()) p["tenant"] = tenant;
    const auto* method = dialect == WireDialect::V1_0 ? k_send_stream.v1
                                                      : k_send_stream.v03;
    json body = {
        {"jsonrpc", "2.0"},
        {"id",      request_id_.fetch_add(1, std::memory_order_relaxed) + 1},
        {"method",  method},
        {"params",  p},
    };
    auto body_str = body.dump();
    auto endpoint = async::split_async_endpoint(url);
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"},
        {"Accept",       "text/event-stream"},
    };
    if (auto v = dialect_version_header(dialect)) headers.emplace_back("A2A-Version", v);
    if (auto authorization = request_authorization_header(); !authorization.empty()) {
        headers.emplace_back("Authorization", std::move(authorization));
    }

    async::RequestOptions opts;
    opts.timeout = request_timeout();

    Task acc;
    SseFrameSplitter splitter;
    bool aborted = false;
    std::optional<A2ARpcError> rpc_error;
    std::string raw_body;   // non-SSE reply (JSON-RPC error / plain result)
    int http_status = 0;

    auto frame_handler = [&](std::string_view payload) {
        if (aborted) return;
        json frame_json;
        try {
            frame_json = json::parse(std::string(payload));
        } catch (...) {
            return;
        }
        if (frame_json.is_object() && frame_json.contains("error")
            && !frame_json["error"].is_null()) {
            const auto& err = frame_json["error"];
            int code = 0;
            std::string msg = "A2A RPC error";
            if (err.is_object()) {
                if (err.contains("code")) {
                    msg += " (code=" + err["code"].dump() + ")";
                    if (err["code"].is_number_integer()) code = err["code"].get<int>();
                }
                if (err.contains("message")) msg += ": " + err.value("message", "");
            } else {
                msg += ": " + err.dump();
            }
            rpc_error.emplace(code, msg);
            aborted = true;
            return;
        }
        json result = frame_json.contains("result")
                        ? frame_json["result"]
                        : frame_json;
        StreamEvent ev = parse_stream_event(result);
        saw_events = true;
        accumulate(acc, ev);
        if (on_event && !on_event(ev)) aborted = true;
    };

    auto chunk_callback = [&](std::string_view chunk) {
        if (!saw_events && !rpc_error && raw_body.size() < kMaxStreamErrorBody) {
            raw_body.append(chunk.substr(0, kMaxStreamErrorBody - raw_body.size()));
        }
        splitter.feed(chunk, frame_handler);
    };

    async::run_sync([&]() -> asio::awaitable<void> {
        auto ex = co_await asio::this_coro::executor;
        auto res = co_await async::async_post_stream(
            ex,
            endpoint.host,
            endpoint.port,
            endpoint.prefix.empty() ? "/" : endpoint.prefix,
            body_str,
            std::move(headers),
            endpoint.tls,
            chunk_callback,
            opts);
        http_status = res.status;
    }());
    splitter.finish(frame_handler);

    if (http_status < 200 || http_status >= 300) {
        throw HttpStatusError(http_status, raw_body);
    }
    if (!saw_events && !rpc_error) {
        // A server that answers a streaming request with a plain JSON-RPC
        // body (typically an error such as -32601) never sends `data:`
        // frames; surface that instead of returning an empty Task.
        frame_handler(raw_body);
    }
    if (rpc_error) throw *rpc_error;
    return acc;
}

Task A2AClient::send_message_stream(const MessageSendParams& params,
                                    EventCallback on_event) {
    bool saw_events = false;
    if (auto sel = async::run_sync(select_async())) {
        return stream_once(sel->dialect, sel->tenant, sel->url, params, on_event, saw_events);
    }
    // No card could be fetched: the same probe as unary calls, 1.0 first.
    // Once an event was observed the dialect is settled and nothing is
    // re-sent.
    try {
        auto task = stream_once(WireDialect::V1_0, std::string(), base_url_, params,
                                on_event, saw_events);
        remember_probe(WireDialect::V1_0);
        return task;
    } catch (const A2ARpcError& e) {
        if (saw_events || !probe_falls_back(e.code(), 0)) throw;
    } catch (const HttpStatusError& e) {
        if (saw_events || !probe_falls_back(0, e.status())) throw;
    }
    auto task = stream_once(WireDialect::V0_3, std::string(), base_url_, params,
                            on_event, saw_events);
    remember_probe(WireDialect::V0_3);
    return task;
}

Task A2AClient::send_message_stream(const std::string& text,
                                    EventCallback on_event,
                                    const std::string& task_id,
                                    const std::string& context_id) {
    MessageSendParams params;
    params.message.message_id = fresh_message_id();
    params.message.role       = Role::User;
    params.message.parts.push_back(Part::text_part(text));
    if (!task_id.empty())    params.message.task_id    = task_id;
    if (!context_id.empty()) params.message.context_id = context_id;
    return send_message_stream(params, std::move(on_event));
}

}  // namespace neograph::a2a
