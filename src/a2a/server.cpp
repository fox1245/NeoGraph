#include <neograph/a2a/server.h>
#include <neograph/graph/invocation.h>
#include <neograph/graph/types.h>

#include "../core/tool_failure.h"
#include "ids.h"

#ifdef NEOGRAPH_A2A_PROGRAM
#include <neograph/a2a/program_adapter.h>
#endif

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#include <openssl/evp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

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

neograph::json GraphAgentAdapter::build_resume_value(const Message& message) const {
    std::string text;
    bool has_text = false;
    for (const auto& part : message.parts) {
        if (part.kind != "text") continue;
        if (has_text) text.push_back(' ');
        text.append(part.text);
        has_text = true;
    }
    if (has_text) return neograph::json(text);
    for (const auto& part : message.parts) {
        if (part.kind == "data") return part.data;
    }
    return neograph::json();
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

constexpr const char* kA2ADomain       = "a2a-protocol.org";
constexpr const char* kNeoGraphDomain  = "neograph";
constexpr const char* kErrorInfoType   = "type.googleapis.com/google.rpc.ErrorInfo";
constexpr const char* kBadRequestType  = "type.googleapis.com/google.rpc.BadRequest";
constexpr const char* kSupportedVersions = "1.0, 0.3";
constexpr int kDefaultPageSize = 50;
constexpr int kMaxPageSize     = 100;
/// How long a task stream stays silent before it sends an SSE comment. A run
/// can sit in one node far longer than a client or proxy waits for bytes
/// (httplib and httpx default to 5 s), and the stream otherwise says nothing
/// between the Task and the next status.
constexpr auto kStreamKeepAlive = 5s;
/// How often a stream looks at a durable run that has no channel to wake it.
constexpr auto kFollowPollInterval = 25ms;

/// A JSON-RPC error with its A2A typed details (spec 9.5: `error.data` is a
/// list of google.rpc details, the first one an ErrorInfo).
struct RpcError {
    int         code = -32603;
    std::string message;
    std::string reason;
    std::string domain = kA2ADomain;
    json        metadata;        ///< ErrorInfo metadata: string values only
    json        violations;      ///< BadRequest field violations
    int         http_status = 200;
    std::string www_authenticate;
};

const char* reason_for(int code) {
    switch (code) {
        case -32001: return "TASK_NOT_FOUND";
        case -32002: return "TASK_NOT_CANCELABLE";
        case -32003: return "PUSH_NOTIFICATION_NOT_SUPPORTED";
        case -32004: return "UNSUPPORTED_OPERATION";
        case -32005: return "CONTENT_TYPE_NOT_SUPPORTED";
        case -32007: return "EXTENDED_AGENT_CARD_NOT_CONFIGURED";
        case -32009: return "VERSION_NOT_SUPPORTED";
        case -32600: return "INVALID_REQUEST";
        case -32601: return "METHOD_NOT_FOUND";
        case -32602: return "INVALID_PARAMS";
        case -32700: return "PARSE_ERROR";
        default:     return "INTERNAL_ERROR";
    }
}

RpcError rpc_error(int code, std::string message) {
    RpcError e;
    e.code = code;
    e.message = std::move(message);
    e.reason = reason_for(code);
    return e;
}

json error_object(const RpcError& e) {
    json info = json::object();
    info["@type"]    = kErrorInfoType;
    info["reason"]   = e.reason.empty() ? std::string(reason_for(e.code)) : e.reason;
    info["domain"]   = e.domain;
    info["metadata"] = e.metadata.is_object() ? e.metadata : json::object();
    json details = json::array();
    details.push_back(std::move(info));
    if (e.violations.is_array() && !e.violations.empty()) {
        details.push_back(json{{"@type", kBadRequestType}, {"fieldViolations", e.violations}});
    }
    json err = json::object();
    err["code"]    = e.code;
    err["message"] = e.message;
    err["data"]    = std::move(details);
    return err;
}

json jsonrpc_error(const RpcError& e, const json& id) {
    json env = json::object();
    env["jsonrpc"] = "2.0";
    env["id"]      = id;
    env["error"]   = error_object(e);
    return env;
}

json jsonrpc_result(json result, const json& id) {
    json env = json::object();
    env["jsonrpc"] = "2.0";
    env["id"]      = id;
    env["result"]  = std::move(result);
    return env;
}

std::string sse_frame(const json& envelope) {
    return "data: " + envelope.dump() + "\n\n";
}

bool is_terminal(TaskState state) {
    return state == TaskState::Completed || state == TaskState::Canceled
        || state == TaskState::Failed || state == TaskState::Rejected;
}

/// Interrupted states (spec 4.1.3): the task waits for the client.
bool is_interrupted(TaskState state) {
    return state == TaskState::InputRequired || state == TaskState::AuthRequired;
}

/// A task lifecycle stream closes on a terminal or interrupted state.
bool closes_stream(TaskState state) { return is_terminal(state) || is_interrupted(state); }

void set_status(Task& task, TaskState state, std::optional<Message> message = std::nullopt) {
    task.status.state     = state;
    task.status.message   = std::move(message);
    task.status.timestamp = detail::utc_timestamp_now();
}

Message agent_message(const std::string& task_id, const std::string& context_id,
                      std::vector<Part> parts) {
    Message m;
    m.message_id = detail::random_uuid();
    m.role       = Role::Agent;
    m.task_id    = task_id;
    m.context_id = context_id;
    m.parts      = std::move(parts);
    return m;
}

Message agent_text(const std::string& task_id, const std::string& context_id,
                   std::string text) {
    std::vector<Part> parts;
    parts.push_back(Part::text_part(std::move(text)));
    return agent_message(task_id, context_id, std::move(parts));
}

/// `A2A-Version` -> dialect. Empty is 0.3 (spec 3.6.2). Only the exact
/// Major.Minor pairs 1.0 and 0.3 are spoken; a patch component is ignored.
std::optional<WireDialect> dialect_for_version(std::string_view v) {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
    if (v.empty()) return WireDialect::V0_3;
    auto number = [&](std::size_t& pos, int& out) {
        const auto start = pos;
        out = 0;
        while (pos < v.size() && v[pos] >= '0' && v[pos] <= '9' && pos - start < 6) {
            out = out * 10 + (v[pos] - '0');
            ++pos;
        }
        return pos > start;
    };
    std::size_t pos = 0;
    int major = 0, minor = 0;
    if (!number(pos, major)) return std::nullopt;
    if (pos < v.size()) {
        if (v[pos] != '.') return std::nullopt;
        ++pos;
        if (!number(pos, minor)) return std::nullopt;
        if (pos < v.size()) {
            if (v[pos] != '.') return std::nullopt;
            ++pos;
            int patch = 0;
            if (!number(pos, patch) || pos != v.size()) return std::nullopt;
        }
    } else if (major != 1) {
        return std::nullopt;  // "1" alone means 1.0; "0" names no version
    }
    if (major == 1 && minor == 0) return WireDialect::V1_0;
    if (major == 0 && minor == 3) return WireDialect::V0_3;
    return std::nullopt;
}

/// The requested protocol version: the `A2A-Version` header, else the
/// `A2A-Version` query parameter (spec 3.6.1 allows either).
std::string requested_version(const httplib::Request& req) {
    auto v = req.get_header_value("A2A-Version");
    if (v.empty() && req.has_param("A2A-Version")) v = req.get_param_value("A2A-Version");
    return v;
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

/// Copy of @p task as a client should see it: `historyLength` applied
/// (0 omits the history, spec 3.2.4) and artifacts dropped when not asked for.
json task_wire(Task task, WireDialect dialect, std::optional<int> history_length,
               bool include_artifacts = true) {
    if (history_length) {
        if (*history_length <= 0) {
            task.history.clear();
        } else if (task.history.size() > static_cast<std::size_t>(*history_length)) {
            task.history.erase(task.history.begin(),
                               task.history.end() - *history_length);
        }
    }
    if (!include_artifacts) task.artifacts.clear();
    json j;
    to_json(j, task, dialect);
    return j;
}

// --- request validation (spec 3.3.2: servers MUST validate input) --------

using Violations = std::vector<std::pair<std::string, std::string>>;

json violations_json(const Violations& v) {
    auto arr = json::array();
    for (const auto& [field, description] : v) {
        arr.push_back(json{{"field", field}, {"description", description}});
    }
    return arr;
}

bool is_string(const json& object, const char* key) {
    return object.contains(key) && object[key].is_string();
}

bool nonempty_string(const json& object, const char* key) {
    return is_string(object, key) && !object[key].get<std::string>().empty();
}

void validate_part(const json& part, const std::string& path, Violations& v) {
    if (!part.is_object()) {
        v.emplace_back(path, "must be an object");
        return;
    }
    if (part.contains("kind")) {  // 0.3 Part
        const auto kind = part["kind"].is_string() ? part["kind"].get<std::string>() : "";
        if (kind == "text") {
            if (!is_string(part, "text")) v.emplace_back(path + ".text", "is required and must be a string");
        } else if (kind == "file") {
            const auto file = part.contains("file") ? part["file"] : json();
            if (!file.is_object() || !(is_string(file, "bytes") || is_string(file, "uri"))) {
                v.emplace_back(path + ".file", "must be an object with bytes or uri");
            }
        } else if (kind == "data") {
            if (!part.contains("data") || !part["data"].is_object()) {
                v.emplace_back(path + ".data", "is required and must be an object");
            }
        } else {
            v.emplace_back(path + ".kind", "must be one of text, file, data");
        }
    } else {  // 1.0 Part: exactly one content member
        int members = 0;
        for (const char* key : {"text", "raw", "url", "data"}) {
            if (part.contains(key) && !part[key].is_null()) ++members;
        }
        if (members != 1) {
            v.emplace_back(path, "must set exactly one of text, raw, url, data");
        } else {
            for (const char* key : {"text", "raw", "url"}) {
                if (part.contains(key) && !part[key].is_null() && !part[key].is_string()) {
                    v.emplace_back(path + "." + key, "must be a string");
                }
            }
        }
    }
    if (part.contains("metadata") && !part["metadata"].is_null() && !part["metadata"].is_object()) {
        v.emplace_back(path + ".metadata", "must be an object");
    }
}

void validate_message(const json& m, Violations& v) {
    if (!m.is_object()) {
        v.emplace_back("message", "is required and must be an object");
        return;
    }
    if (!nonempty_string(m, "messageId")) {
        v.emplace_back("message.messageId", "is required and must be a non-empty string");
    }
    const auto role = is_string(m, "role") ? m["role"].get<std::string>() : std::string();
    if (role != "user" && role != "agent" && role != "ROLE_USER" && role != "ROLE_AGENT") {
        v.emplace_back("message.role", "is required and must be ROLE_USER or ROLE_AGENT");
    }
    if (!m.contains("parts") || !m["parts"].is_array() || m["parts"].empty()) {
        v.emplace_back("message.parts", "is required and must be a non-empty array");
    } else {
        std::size_t i = 0;
        for (auto part : m["parts"]) {
            validate_part(part, "message.parts[" + std::to_string(i++) + "]", v);
        }
    }
    for (const char* key : {"taskId", "contextId"}) {
        if (m.contains(key) && !m[key].is_null() && !nonempty_string(m, key)) {
            v.emplace_back(std::string("message.") + key, "must be a non-empty string");
        }
    }
    for (const char* key : {"referenceTaskIds", "extensions"}) {
        if (!m.contains(key) || m[key].is_null()) continue;
        bool ok = m[key].is_array();
        if (ok) for (auto item : m[key]) ok = ok && item.is_string();
        if (!ok) v.emplace_back(std::string("message.") + key, "must be an array of strings");
    }
    if (m.contains("metadata") && !m["metadata"].is_null() && !m["metadata"].is_object()) {
        v.emplace_back("message.metadata", "must be an object");
    }
    if (m.contains("kind") && !(m["kind"].is_string() && m["kind"].get<std::string>() == "message")) {
        v.emplace_back("message.kind", "must be \"message\"");
    }
}

void validate_history_length(const json& object, const std::string& path, Violations& v) {
    if (!object.contains("historyLength") || object["historyLength"].is_null()) return;
    const auto value = object["historyLength"];
    if (!value.is_number_integer() || value.get<long long>() < 0) {
        v.emplace_back(path, "must be a non-negative integer");
    }
}

std::optional<int> history_length_of(const json& object) {
    if (!object.is_object() || !object.contains("historyLength")
        || !object["historyLength"].is_number_integer()) {
        return std::nullopt;
    }
    return static_cast<int>(std::min<long long>(object["historyLength"].get<long long>(), 1'000'000));
}

void validate_task_id(const json& params, Violations& v) {
    if (!nonempty_string(params, "id")) {
        v.emplace_back("id", "is required and must be a non-empty string");
    }
}

// --- card ------------------------------------------------------------------

std::string sha256_hex(const std::string& text) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_Digest(text.data(), text.size(), digest, &length, EVP_sha256(), nullptr) != 1) {
        return {};
    }
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < length; ++i) {
        out.push_back(digits[digest[i] >> 4]);
        out.push_back(digits[digest[i] & 0x0F]);
    }
    return out;
}

/// A Host header that is safe to echo into the card's interface URL.
bool plausible_host(const std::string& host) {
    if (host.empty() || host.size() > 255) return false;
    return std::all_of(host.begin(), host.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '-' || c == ':' || c == '[' || c == ']'
            || c == '_';
    });
}

/// The first element of a comma-separated header list, trimmed.
std::string first_list_item(const std::string& value) {
    auto item = value.substr(0, value.find(','));
    const auto begin = item.find_first_not_of(" \t");
    if (begin == std::string::npos) return {};
    const auto end = item.find_last_not_of(" \t");
    return item.substr(begin, end - begin + 1);
}

/// Scheme and host a trusted reverse proxy reports for the client's request:
/// RFC 7239 `Forwarded` (its first element, the hop nearest the client),
/// else `X-Forwarded-Proto` / `X-Forwarded-Host`. Only `http` / `https` and
/// plausible hosts are taken; anything else keeps the direct value.
void apply_forwarded_origin(const httplib::Request& req, std::string& scheme, std::string& host) {
    std::string proto;
    std::string forwarded_host;
    const auto forwarded = first_list_item(req.get_header_value("Forwarded"));
    if (!forwarded.empty()) {
        std::size_t pos = 0;
        while (pos <= forwarded.size()) {
            const auto next = forwarded.find(';', pos);
            auto pair = forwarded.substr(pos, next == std::string::npos ? std::string::npos
                                                                        : next - pos);
            const auto eq = pair.find('=');
            if (eq != std::string::npos) {
                auto key = first_list_item(pair.substr(0, eq));
                auto value = first_list_item(pair.substr(eq + 1));
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                    value = value.substr(1, value.size() - 2);
                }
                std::transform(key.begin(), key.end(), key.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (key == "proto") proto = value;
                else if (key == "host") forwarded_host = value;
            }
            if (next == std::string::npos) break;
            pos = next + 1;
        }
    } else {
        proto = first_list_item(req.get_header_value("X-Forwarded-Proto"));
        forwarded_host = first_list_item(req.get_header_value("X-Forwarded-Host"));
    }
    std::transform(proto.begin(), proto.end(), proto.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (proto == "http" || proto == "https") scheme = proto;
    if (plausible_host(forwarded_host)) host = forwarded_host;
}

// --- page tokens ------------------------------------------------------------

std::string hex_encode(const std::string& text) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : text) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0x0F]);
    }
    return out;
}

std::optional<std::string> hex_decode(const std::string& text) {
    if (text.size() % 2 != 0) return std::nullopt;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int hi = nibble(text[i]), lo = nibble(text[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<char>(hi * 16 + lo));
    }
    return out;
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

using neograph::graph::FailureVerdict;
using neograph::graph::NodeFailureClass;

/// The classified failure that ended an invocation (classify_failure). Error
/// and Cancelled outcomes always carry one; the fallback is defensive.
FailureVerdict failure_of(const neograph::graph::InvocationResult& outcome) {
    if (outcome.failure) return *outcome.failure;
    FailureVerdict verdict;
    verdict.failure_class = outcome.cancelled() ? NodeFailureClass::Cancelled
                                                : NodeFailureClass::Unknown;
    verdict.code = neograph::graph::node_failure_code(verdict.failure_class);
    return verdict;
}

/// The only failure text a peer sees: the typed public message or the
/// class's fixed description, never what() (reference, "Protocol failure
/// mapping").
std::string public_text(const FailureVerdict& failure) {
    return ::neograph::detail::public_failure_message(failure);
}

/// @p object without @p key (the JSON wrapper has no erase).
json without_key(const json& object, const char* key) {
    json out = json::object();
    if (!object.is_object()) return out;
    for (auto [name, value] : object.items()) {
        if (name != key) out[name] = value;
    }
    return out;
}

/// The checkpoint an INPUT_REQUIRED task paused at, or empty when the run
/// saved none (an engine without a checkpoint store).
std::string interrupt_checkpoint(const Task& task) {
    if (!task.metadata.is_object() || !task.metadata.contains("neograph/interrupt")) return {};
    const auto interrupt = task.metadata["neograph/interrupt"];
    return interrupt.is_object() ? interrupt.value("checkpointId", std::string()) : std::string();
}

/// Failure fields a task may carry: `metadata["neograph/failure"]`.
json failure_metadata(const FailureVerdict& failure) {
    return json{{"code", failure.code}, {"retryable", failure.retryable}};
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct A2AServer::Impl {
    std::optional<neograph::graph::GraphExecution> execution;
    AgentCard                                     card;
    std::shared_ptr<GraphAgentAdapter>            adapter;
#ifdef NEOGRAPH_A2A_PROGRAM
    std::shared_ptr<ProgramAgentAdapter>           program_adapter;
    A2AServer::CollaborationAuthenticator          collaboration_authenticator;
    bool                                            require_authenticated_requests = false;
#endif
    httplib::Server                               svr;
    std::thread                                   listener;
    std::atomic<bool>                             running{false};
    std::atomic<bool>                             stopping{false};
    int                                           bound_port = 0;
    bool                                          generated_card_url = false;
    std::atomic<bool>                             trust_forwarded_headers{false};

    void bind_card_endpoint(const std::string& host) {
        if (!card.url.empty() && !generated_card_url) return;
        card.url.clear();
        card.url.reserve(host.size() + 16);
        card.url += "http://";
        const bool ipv6 = host.find(':') != std::string::npos;
        if (ipv6) card.url += '[';
        card.url += host;
        if (ipv6) card.url += ']';
        card.url += ':';
        card.url += std::to_string(bound_port);
        generated_card_url = true;
    }

    /// One look at a run that has no channel (`follow` of stream_task).
    enum class FollowStep {
        Working,      ///< not settled yet
        Settled,      ///< terminal or interrupted: the Task is the final status
        Unavailable,  ///< the run can no longer be followed
    };
    using FollowPoll = std::function<FollowStep(Task& settled)>;

    /// One event of a task lifecycle stream.
    struct StreamItem {
        bool                    is_artifact = false;
        TaskStatusUpdateEvent   status;
        TaskArtifactUpdateEvent artifact;
    };

    /// The events of one run, read by every stream of that task (the
    /// SendStreamingMessage that started it and any SubscribeToTask). Guarded
    /// by tasks_mu; `closed` once the run published its terminal or
    /// interrupted status.
    struct TaskChannel {
        std::vector<StreamItem>  items;
        bool                     closed = false;
        std::condition_variable  cv;
    };

    /// A run that owns a task until it publishes its final state.
    struct ActiveRun {
        std::shared_ptr<neograph::graph::CancelToken> cancel_token;
        std::uint64_t                                  generation = 0;
        std::shared_ptr<TaskChannel>                   channel;
    };

    /// In-memory task store, LRU-bounded by `max_tasks` (default 1024): a
    /// long-running server would otherwise grow its task history without
    /// bound. `task_lru` holds task ids least-recently-touched first. Tasks
    /// with an active run are never evicted. A2A allows a store that does
    /// not survive restarts.
    std::mutex                                                tasks_mu;
    std::unordered_map<std::string, Task>                     tasks;
    std::unordered_map<std::string, ActiveRun>                active_runs;
    std::unordered_map<std::string, std::shared_ptr<TaskChannel>> channels;
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
    /// Guarded by tasks_mu (read by streams while they wait on it).
    std::chrono::milliseconds stream_keepalive = kStreamKeepAlive;

    /// Run workers. Every run executes on its own thread, so a request can
    /// return before its run ends (returnImmediately, streams, cancellation).
    std::mutex                                                workers_mu;
    std::list<std::pair<std::thread, std::shared_ptr<std::atomic<bool>>>> workers;

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

    bool has_run_unlocked(const std::string& tid) const {
        if (active_runs.contains(tid)) return true;
#ifdef NEOGRAPH_A2A_PROGRAM
        if (program_runs.contains(tid)) return true;
#endif
        return false;
    }

    std::size_t inflight_unlocked() const {
        std::size_t n = active_runs.size();
#ifdef NEOGRAPH_A2A_PROGRAM
        for (const auto& [tid, _] : program_runs) {
            if (!active_runs.contains(tid)) ++n;
        }
#endif
        return n;
    }

    /// Evict LRU entries until tasks.size() <= max_tasks. Caller must
    /// hold tasks_mu.
    void evict_lru_unlocked() {
        auto remaining = task_lru.size();
        while (tasks.size() > max_tasks && remaining-- > 0 && !task_lru.empty()) {
            auto victim = task_lru.front();
            task_lru.pop_front();
            if (has_run_unlocked(victim)) {
                task_lru.push_back(victim);
                task_lru_pos[victim] = std::prev(task_lru.end());
                continue;
            }
            task_lru_pos.erase(victim);
            tasks.erase(victim);
            channels.erase(victim);
        }
    }

    void launch_worker(std::function<void()> body) {
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::thread worker([body = std::move(body), done] {
            try {
                body();
            } catch (...) {
                // A worker never lets an exception escape; finish_run already
                // published the failure it could classify.
            }
            done->store(true, std::memory_order_release);
        });
        std::lock_guard lk(workers_mu);
        for (auto it = workers.begin(); it != workers.end();) {
            if (it->second->load(std::memory_order_acquire)) {
                it->first.join();
                it = workers.erase(it);
            } else {
                ++it;
            }
        }
        workers.emplace_back(std::move(worker), std::move(done));
    }

    void join_workers() {
        std::list<std::pair<std::thread, std::shared_ptr<std::atomic<bool>>>> finished;
        {
            std::lock_guard lk(workers_mu);
            finished.swap(workers);
        }
        for (auto& worker : finished) {
            if (worker.first.joinable()) worker.first.join();
        }
    }

    void wake_streams() {
        std::lock_guard lk(tasks_mu);
        for (auto& [_, channel] : channels) channel->cv.notify_all();
    }

    // --- publishing (all under tasks_mu) ------------------------------------

    /// Append a non-final Working status from the run @p generation.
    void publish_working(const std::string& task_id, std::uint64_t generation,
                         const json& metadata = json()) {
        std::lock_guard lk(tasks_mu);
        auto run = active_runs.find(task_id);
        if (run == active_runs.end() || run->second.generation != generation
            || run->second.channel->closed) {
            return;
        }
        auto task_it = tasks.find(task_id);
        if (task_it == tasks.end()) return;
        auto& task = task_it->second;
        set_status(task, TaskState::Working);
        if (metadata.is_object()) {
            if (!task.metadata.is_object()) task.metadata = json::object();
            for (auto [key, value] : metadata.items()) task.metadata[key] = value;
        }
        StreamItem item;
        item.status.task_id    = task_id;
        item.status.context_id = task.context_id;
        item.status.status     = task.status;
        item.status.final      = false;
        if (metadata.is_object() && !metadata.empty()) item.status.metadata = metadata;
        run->second.channel->items.push_back(std::move(item));
        run->second.channel->cv.notify_all();
    }

    /// Commit the outcome of run @p generation: artifacts, then the final
    /// status, then close its stream. A cancel that committed first wins.
    void finish_run(const std::string& task_id, std::uint64_t generation, const Task& outcome) {
        std::lock_guard lk(tasks_mu);
        auto run = active_runs.find(task_id);
        if (run == active_runs.end() || run->second.generation != generation) return;
        auto channel = run->second.channel;
        auto task_it = tasks.find(task_id);
        if (!channel->closed && task_it != tasks.end()
            && task_it->second.status.state != TaskState::Canceled) {
            auto& task = task_it->second;
            for (const auto& artifact : outcome.artifacts) {
                auto same = std::find_if(task.artifacts.begin(), task.artifacts.end(),
                    [&](const Artifact& a) { return a.artifact_id == artifact.artifact_id; });
                if (same == task.artifacts.end()) task.artifacts.push_back(artifact);
                else *same = artifact;
                StreamItem item;
                item.is_artifact = true;
                item.artifact.task_id    = task_id;
                item.artifact.context_id = task.context_id;
                item.artifact.artifact   = artifact;
                item.artifact.last_chunk = true;
                channel->items.push_back(std::move(item));
            }
            task.status = outcome.status;
            if (!task.status.timestamp) task.status.timestamp = detail::utc_timestamp_now();
            if (outcome.status.message) task.history.push_back(*outcome.status.message);
            if (outcome.metadata.is_object()) {
                if (!task.metadata.is_object()) task.metadata = json::object();
                for (auto [key, value] : outcome.metadata.items()) task.metadata[key] = value;
            }
            StreamItem item;
            item.status.task_id    = task_id;
            item.status.context_id = task.context_id;
            item.status.status     = task.status;
            item.status.final      = true;
            if (outcome.metadata.is_object() && !outcome.metadata.empty())
                item.status.metadata = outcome.metadata;
            channel->items.push_back(std::move(item));
            touch_task_unlocked(task_id);
        }
        channel->closed = true;
        channel->cv.notify_all();
        active_runs.erase(run);
        evict_lru_unlocked();
    }

    void register_routes();

    // --- runs ----------------------------------------------------------------

    Task execute_graph(const Message& inbound, const std::string& task_id,
                       const std::string& context_id, const std::string& resume_checkpoint,
                       std::shared_ptr<neograph::graph::CancelToken> cancel_token,
                       std::uint64_t generation);
#ifdef NEOGRAPH_A2A_PROGRAM
    Task execute_program(const Message& inbound, const std::string& task_id,
                         const std::string& context_id, std::uint64_t generation);

    /// Resolve a request header to a non-secret collaboration principal.
    /// Failures intentionally collapse to no authenticated principal.
    std::optional<CollaborationPeerIdentity> authenticate_collaboration_request(
        const httplib::Request& request) const;

    CollaborationTaskAuthorization authorize_collaboration_task(
        std::string_view task_id,
        const std::optional<CollaborationPeerIdentity>& authenticated_peer,
        bool require_cancellation) const;

    /// Rebuild task projections for accepted mailbox records before the
    /// listener accepts new requests. The Program transition store remains
    /// authoritative; this only restores the HTTP-facing projection.
    void recover_program_tasks();

    /// Refresh the projection of a Program task from its durable run: the
    /// live handle when one is held, else a reconnect. False when the run is
    /// not visible to this adapter.
    bool refresh_program_task(const std::string& task_id, Task& out);

    /// One look at the durable run behind a task that has no channel: settled
    /// once the run has a result or a cancel committed, and unavailable when
    /// the task or its run cannot be read any more.
    FollowStep poll_program_task(const std::string& task_id,
                                 const program::ProgramHandle& handle, Task& out);
#endif

    /// The security scheme name and HTTP auth scheme the card declares.
    std::pair<std::string, std::string> declared_http_auth() const;
    RpcError unauthenticated() const;

    AgentCard served_card(const httplib::Request& req) const;

    // --- JSON-RPC --------------------------------------------------------------

    struct Call {
        WireDialect              dialect = WireDialect::V0_3;
        const httplib::Request*  req = nullptr;
        httplib::Response*       res = nullptr;
        json                     id;
        json                     params;
        bool                     streaming = false;
        /// Answer even an upfront error as an event stream (0.3
        /// `tasks/resubscribe`, whose 0.3 clients accept nothing else).
        bool                     sse_errors = false;
    };

    void reply_result(Call& call, json result) {
        call.res->status = 200;
        call.res->set_content(jsonrpc_result(std::move(result), call.id).dump(),
                              "application/json");
    }

    void reply_error(Call& call, const RpcError& e) {
        const auto env = jsonrpc_error(e, call.id);
        if (e.http_status != 200) {
            call.res->status = e.http_status;
            if (!e.www_authenticate.empty())
                call.res->set_header("WWW-Authenticate", e.www_authenticate);
            call.res->set_content(env.dump(), "application/json");
            return;
        }
        call.res->status = 200;
        if (call.sse_errors) {
            // 0.3 clients read tasks/resubscribe only as an event stream, so
            // even an upfront error is one SSE event (as @a2a-js does). Other
            // streaming methods answer an upfront error with a plain
            // JSON-RPC response, like both reference SDK servers.
            call.res->set_content(sse_frame(env), "text/event-stream");
        } else {
            call.res->set_content(env.dump(), "application/json");
        }
    }

    /// Serve a task lifecycle stream: the @p snapshot Task first, then the
    /// run's events from @p cursor until its final status. A task whose run
    /// has no channel (a Program task recovered after a restart) is followed
    /// through @p follow until it settles.
    void stream_task(Call& call, Task snapshot, std::shared_ptr<TaskChannel> channel,
                     std::size_t cursor, FollowPoll follow = {});

    void handle_jsonrpc(const httplib::Request& req, httplib::Response& res);
    void handle_send(Call& call);
    void handle_get(Call& call);
    void handle_cancel(Call& call);
    void handle_list(Call& call);
    void handle_subscribe(Call& call);
};

#ifdef NEOGRAPH_A2A_TESTING
void test::A2AServerTestAccess::set_max_inflight_runs(
    A2AServer& server, std::size_t cap) {
    std::lock_guard lk(server.impl_->tasks_mu);
    server.impl_->max_inflight_runs = cap;
}

void test::A2AServerTestAccess::set_stream_keepalive(
    A2AServer& server, std::chrono::milliseconds interval) {
    std::lock_guard lk(server.impl_->tasks_mu);
    server.impl_->stream_keepalive = std::max(interval, 1ms);
}
#endif

std::pair<std::string, std::string> A2AServer::Impl::declared_http_auth() const {
    if (card.security_schemes.is_object()) {
        for (auto [name, scheme] : card.security_schemes.items()) {
            if (scheme.is_object() && scheme.contains("httpAuthSecurityScheme")) {
                return {name, scheme["httpAuthSecurityScheme"].value("scheme", std::string("Bearer"))};
            }
        }
    }
    return {"bearer", "Bearer"};
}

RpcError A2AServer::Impl::unauthenticated() const {
    // Spec 7.4: a request without valid credentials fails with the binding's
    // authentication error, HTTP 401 with a challenge naming the scheme the
    // card declares.
    RpcError e = rpc_error(-32600, "Authentication required");
    e.reason = "UNAUTHENTICATED";
    e.domain = kNeoGraphDomain;
    e.http_status = 401;
    e.www_authenticate = declared_http_auth().second + " realm=\"a2a\"";
    return e;
}

AgentCard A2AServer::Impl::served_card(const httplib::Request& req) const {
    AgentCard c = card;
    if (!c.raw.is_null()) return c;
    std::string url = c.url;
    if (url.empty() || generated_card_url) {
        // No public URL was configured: advertise the address this client
        // used, so a client that keeps to the card's origin reaches us even
        // when the server is bound to a wildcard address. Behind a reverse
        // proxy the host may opt in to the proxy's forwarded scheme and host;
        // those headers are client-controlled otherwise and are ignored.
        std::string scheme = "http";
        auto host = req.get_header_value("Host");
        if (trust_forwarded_headers.load(std::memory_order_relaxed)) {
            apply_forwarded_origin(req, scheme, host);
        }
        if (plausible_host(host)) url = scheme + "://" + host;
    }
    if (c.supported_interfaces.empty()) {
        // 1.0 is the primary binding; 0.3 is the compatibility dialect.
        c.supported_interfaces = {AgentInterface{url, "JSONRPC", "1.0", {}},
                                  AgentInterface{url, "JSONRPC", "0.3", {}}};
    }
    c.url = url;
    c.protocol_version = "0.3.0";
    c.preferred_transport = "JSONRPC";
    // Not implemented, so never claimed: push notifications, the extended
    // card, and the HTTP+JSON / gRPC bindings (only JSONRPC interfaces).
    c.push_notifications = false;
    c.extended_card = false;
    c.supports_authenticated_extended = false;
#ifdef NEOGRAPH_A2A_PROGRAM
    if (collaboration_authenticator && !c.security_schemes.is_object()) {
        c.security_schemes = json{{"bearer", {{"httpAuthSecurityScheme", {
            {"scheme", "Bearer"},
            {"description", "The Authorization header the agent's authenticator accepts"},
        }}}}};
    }
    if (program_adapter && program_adapter->mailbox()) {
        // A2A-26: the collaboration envelope, whose sender names the receiver
        // task up front, is a declared extension rather than a silent one.
        bool declared = false;
        if (c.extensions.is_array()) {
            for (const auto& extension : c.extensions) {
                if (extension.is_object()
                    && extension.value("uri", std::string()) == COLLABORATION_EXTENSION_URI) {
                    declared = true;
                }
            }
        } else {
            c.extensions = json::array();
        }
        if (!declared) {
            c.extensions.push_back(json{
                {"uri", std::string(COLLABORATION_EXTENSION_URI)},
                {"description", "NeoGraph collaboration envelopes between linked, "
                                "authenticated peers: the sender names the receiver task "
                                "id, so a redelivery is idempotent."},
                {"required", false},
                {"params", {{"format", "neograph-a2a-collaboration-v1"},
                            {"metadataKey", "neograph_collaboration"}}},
            });
        }
    }
    if (collaboration_authenticator && require_authenticated_requests
        && !c.security_requirements.is_array()) {
        const auto [name, _] = declared_http_auth();
        json schemes = json::object();
        schemes[name] = json{{"list", json::array()}};
        c.security_requirements = json::array({json{{"schemes", std::move(schemes)}}});
    }
#endif
    return c;
}

void A2AServer::Impl::register_routes() {
    svr.Get("/.well-known/agent-card.json",
            [this](const httplib::Request& req, httplib::Response& res) {
                // One card per protocol version (as @a2a-js legacyCompat
                // does): A2A-Version 1.x receives the pure 1.0
                // card; no header (spec 3.6.2: 0.3) receives the 0.3 card,
                // which also lists the 1.0 interface.
                const auto version = requested_version(req);
                const auto dialect = dialect_for_version(version);
                res.set_header("Vary", "A2A-Version");
                if (!dialect) {
                    res.status = 400;
                    res.set_content(
                        json{{"error", "A2A version '" + version + "' is not supported; "
                                       "supported versions: " + kSupportedVersions}}.dump(),
                        "application/json");
                    return;
                }
                json j;
                to_json(j, served_card(req), *dialect);
                const auto body = j.dump();
                const auto etag = "W/\"" + sha256_hex(body).substr(0, 32) + "\"";
                res.set_header("ETag", etag);
                res.set_header("Cache-Control", "public, max-age=300");
                const auto if_none_match = req.get_header_value("If-None-Match");
                if (!if_none_match.empty()) {
                    std::stringstream candidates(if_none_match);
                    std::string candidate;
                    const auto bare = etag.substr(2);
                    while (std::getline(candidates, candidate, ',')) {
                        candidate.erase(0, candidate.find_first_not_of(" \t"));
                        candidate.erase(candidate.find_last_not_of(" \t") + 1);
                        if (candidate == "*" || candidate == etag || candidate == bare
                            || (candidate.rfind("W/", 0) == 0 && candidate.substr(2) == bare)) {
                            res.status = 304;
                            return;
                        }
                    }
                }
                res.status = 200;
                res.set_content(body, "application/json");
            });

    svr.Post("/",
             [this](const httplib::Request& req, httplib::Response& res) {
                 handle_jsonrpc(req, res);
             });
}

void A2AServer::Impl::handle_jsonrpc(const httplib::Request& req,
                                     httplib::Response& res) {
    Call call;
    call.req = &req;
    call.res = &res;

    json envelope;
    try {
        envelope = json::parse(req.body);
    } catch (const std::exception&) {
        reply_error(call, rpc_error(-32700, "Parse error"));
        return;
    }
    if (envelope.is_array()) {
        reply_error(call, rpc_error(-32600, "Batch requests are not supported"));
        return;
    }
    if (!envelope.is_object()) {
        reply_error(call, rpc_error(-32600, "Invalid Request: the body must be a JSON-RPC object"));
        return;
    }
    const bool has_id = envelope.contains("id");
    if (has_id) {
        const auto id = envelope["id"];
        if (!(id.is_string() || id.is_number() || id.is_null())) {
            reply_error(call, rpc_error(-32600, "Invalid Request: id must be a string, number or null"));
            return;
        }
        call.id = id;
    }
    if (!envelope.contains("jsonrpc") || !envelope["jsonrpc"].is_string()
        || envelope["jsonrpc"].get<std::string>() != "2.0") {
        reply_error(call, rpc_error(-32600, "Invalid Request: jsonrpc must be \"2.0\""));
        return;
    }
    if (!envelope.contains("method") || !envelope["method"].is_string()) {
        reply_error(call, rpc_error(-32600, "Invalid Request: method must be a string"));
        return;
    }
    const auto method = envelope["method"].get<std::string>();
    call.streaming = method == "SendStreamingMessage" || method == "message/stream"
                  || method == "SubscribeToTask" || method == "tasks/resubscribe";
    call.sse_errors = method == "tasks/resubscribe";

    // JSON-RPC 2.0 §4.1: "The Server MUST NOT reply to a Notification."
    // Every A2A method is a request, so a notification does nothing.
    if (!has_id) {
        res.status = 204;
        return;
    }

    // A2A-Version selects the wire generation of the *response* (spec
    // 3.6.2): empty means 0.3, 1.0 the ProtoJSON form. Both method
    // spellings are accepted with either version.
    const auto version = requested_version(req);
    const auto dialect = dialect_for_version(version);
    if (!dialect) {
        reply_error(call, rpc_error(-32009,
            "The requested A2A protocol version '" + version
            + "' is not supported. Supported versions: " + kSupportedVersions));
        return;
    }
    call.dialect = *dialect;

    if (envelope.contains("params") && !envelope["params"].is_null()) {
        call.params = envelope["params"];
        if (!call.params.is_object()) {
            reply_error(call, rpc_error(-32602, "Invalid params: params must be an object"));
            return;
        }
    } else {
        call.params = json::object();
    }

    if (method == "message/send" || method == "SendMessage"
        || method == "message/stream" || method == "SendStreamingMessage") {
        handle_send(call);
    } else if (method == "tasks/get" || method == "GetTask") {
        handle_get(call);
    } else if (method == "tasks/cancel" || method == "CancelTask") {
        handle_cancel(call);
    } else if (method == "ListTasks") {
        handle_list(call);
    } else if (method == "tasks/resubscribe" || method == "SubscribeToTask") {
        handle_subscribe(call);
    } else if (method == "CreateTaskPushNotificationConfig"
               || method == "GetTaskPushNotificationConfig"
               || method == "ListTaskPushNotificationConfigs"
               || method == "DeleteTaskPushNotificationConfig"
               || method == "tasks/pushNotificationConfig/set"
               || method == "tasks/pushNotificationConfig/get"
               || method == "tasks/pushNotificationConfig/list"
               || method == "tasks/pushNotificationConfig/delete") {
        reply_error(call, rpc_error(-32003, "Push notifications are not supported by this agent"));
    } else if (method == "GetExtendedAgentCard"
               || method == "agent/getAuthenticatedExtendedCard") {
        reply_error(call, rpc_error(-32004,
            "The agent does not support an authenticated extended card"));
    } else {
        reply_error(call, rpc_error(-32601, "Method not found"));
    }
}

// ---------------------------------------------------------------------------
// Runs
// ---------------------------------------------------------------------------

Task A2AServer::Impl::execute_graph(
    const Message& inbound, const std::string& task_id, const std::string& context_id,
    const std::string& resume_checkpoint,
    std::shared_ptr<neograph::graph::CancelToken> cancel_token, std::uint64_t generation) {
    auto& a = *adapter;
    publish_working(task_id, generation);

    Task result;
    result.id = task_id;
    result.context_id = context_id;
    // Whether any node finished: a hook that blocks before any work is a
    // rejection, after it a failure (spec 3.3.2 / §3 of the audit).
    auto work_done = std::make_shared<std::atomic<bool>>(false);
    try {
        neograph::graph::RunConfig cfg;
        cfg.thread_id    = task_id;
        cfg.cancel_token = cancel_token;
        // Reserve the generation before adapter work so CancelTask can signal
        // this run even while input preparation is still in progress. A
        // follow-up to an INPUT_REQUIRED task resumes from the checkpoint the
        // interrupt saved; without one (no checkpoint store) the run starts
        // again with the follow-up as its input.
        const bool resuming = !resume_checkpoint.empty();
        if (!resuming) cfg.input = a.build_initial_state(extract_user_text(inbound));
        neograph::graph::RunInvocationRequest request;
        request.config = std::move(cfg);
        request.on_event = [work_done](const neograph::graph::GraphEvent& event) {
            if (event.type == neograph::graph::GraphEvent::Type::NODE_END)
                work_done->store(true, std::memory_order_relaxed);
        };
        neograph::graph::RunInvocation invocation(*execution, std::move(request));
        const auto outcome = resuming
            ? invocation.resume_from(resume_checkpoint, a.build_resume_value(inbound))
            : invocation.run();
        json metadata = json::object();
        metadata["neograph/invocation_status"] =
            std::string(neograph::graph::to_string(outcome.status));
        if (outcome.cancelled()) {
            set_status(result, TaskState::Canceled);
        } else if (!outcome.succeeded() || !outcome.run_result) {
            // Only typed public fields reach the peer; outcome.error is
            // what() text and stays out of the task (A2A-15).
            const auto failure = failure_of(outcome);
            const bool rejected = failure.failure_class == neograph::graph::NodeFailureClass::HookBlocked
                                && !work_done->load(std::memory_order_relaxed);
            set_status(result, rejected ? TaskState::Rejected : TaskState::Failed,
                       agent_text(task_id, context_id, public_text(failure)));
            metadata["neograph/failure"] = failure_metadata(failure);
        } else if (outcome.status == neograph::graph::InvocationStatus::Interrupted) {
            // Spec 4.1.3: INPUT_REQUIRED says what is needed. The reason is
            // the node's question to the human; the payload travels as data.
            const auto& rr = *outcome.run_result;
            std::string reason;
            if (rr.interrupt_value.is_object()) {
                reason = rr.interrupt_value.value(
                    "reason", rr.interrupt_value.value("message", std::string()));
            }
            if (reason.empty()) {
                FailureVerdict interrupted;
                interrupted.failure_class = NodeFailureClass::Interrupted;
                reason = public_text(interrupted);
            }
            std::vector<Part> parts;
            parts.push_back(Part::text_part(reason));
            if (rr.interrupt_value.is_object() && rr.interrupt_value.contains("value")) {
                Part data;
                data.kind = "data";
                data.data = rr.interrupt_value["value"];
                parts.push_back(std::move(data));
            }
            set_status(result, TaskState::InputRequired,
                       agent_message(task_id, context_id, std::move(parts)));
            metadata["neograph/interrupt"] = json{{"node", rr.interrupt_node},
                                                  {"checkpointId", rr.checkpoint_id}};
        } else if (outcome.status == neograph::graph::InvocationStatus::MaxSteps) {
            set_status(result, TaskState::Failed,
                       agent_text(task_id, context_id, "The agent run reached its step limit."));
            metadata["neograph/failure"] = json{{"code", "NG.StepLimit"}, {"retryable", false}};
        } else {
            const auto& rr = *outcome.run_result;
            set_status(result, TaskState::Completed,
                       agent_text(task_id, context_id,
                                  extract_agent_text(rr.output, a.output_channel())));
            if (auto artifact = a.build_output_artifact(rr.output, task_id)) {
                result.artifacts.push_back(std::move(*artifact));
            }
        }
        result.metadata = std::move(metadata);
    } catch (...) {
        // The adapter (or the invocation setup) threw: report only the typed
        // projection of what it threw.
        const auto failure = neograph::graph::classify_failure(std::current_exception(),
                                                               cancel_token.get());
        if (failure.failure_class == neograph::graph::NodeFailureClass::Cancelled
            || cancel_token->is_cancelled()) {
            set_status(result, TaskState::Canceled);
            result.metadata = json{{"neograph/invocation_status", "cancelled"}};
        } else {
            set_status(result, TaskState::Failed,
                       agent_text(task_id, context_id, public_text(failure)));
            result.metadata = json{{"neograph/invocation_status", "error"},
                                   {"neograph/failure", failure_metadata(failure)}};
        }
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
        if (!task.status.timestamp) task.status.timestamp = detail::utc_timestamp_now();
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

bool A2AServer::Impl::refresh_program_task(const std::string& task_id, Task& out) {
    std::optional<program::ProgramHandle> live;
    std::string context;
    std::uint64_t generation = 0;
    {
        std::lock_guard lk(tasks_mu);
        if (const auto run = program_runs.find(task_id); run != program_runs.end()) {
            live = run->second.handle;
            generation = run->second.generation;
        }
        if (const auto task = tasks.find(task_id); task != tasks.end()) {
            if (!live) {
                // No durable run is attached: the projection is current.
                out = task->second;
                touch_task_unlocked(task_id);
                return true;
            }
            context = task->second.context_id;
        }
    }
    try {
        Task snapshot = live ? program_adapter->task_snapshot(
                                   *live, task_id,
                                   context.empty() ? std::string_view{task_id}
                                                   : std::string_view{context})
                             : program_adapter->reconnect_task(task_id, context);
        if (!snapshot.status.timestamp) snapshot.status.timestamp = detail::utc_timestamp_now();
        const bool terminal = is_terminal(snapshot.status.state);
        {
            std::lock_guard lk(tasks_mu);
            auto existing = tasks.find(task_id);
            if (existing != tasks.end()
                && existing->second.status.state == TaskState::Canceled) {
                snapshot = existing->second;  // a committed cancel wins
            } else if (!active_runs.contains(task_id)) {
                // A run this server still drives publishes its own state.
                tasks[task_id] = snapshot;
            } else if (existing != tasks.end()) {
                snapshot = existing->second;
            }
            touch_task_unlocked(task_id);
            if (terminal) {
                const auto run = program_runs.find(task_id);
                if (run != program_runs.end() && run->second.generation == generation
                    && !active_runs.contains(task_id)) {
                    program_runs.erase(run);
                }
            }
            evict_lru_unlocked();
        }
        if (terminal) program_adapter->acknowledge_task(task_id);
        out = std::move(snapshot);
        return true;
    } catch (const std::exception&) {
        // ProgramRuntime deliberately does not expose cross-owner existence
        // details: an invisible run is "not found".
        std::lock_guard lk(tasks_mu);
        if (const auto task = tasks.find(task_id); task != tasks.end()) {
            out = task->second;
            return true;
        }
        return false;
    }
}

A2AServer::Impl::FollowStep A2AServer::Impl::poll_program_task(
    const std::string& task_id, const program::ProgramHandle& handle, Task& out) {
    {
        std::lock_guard lk(tasks_mu);
        const auto task = tasks.find(task_id);
        if (task == tasks.end()) return FollowStep::Unavailable;
        // A cancel commits to the stored task before the run reports it.
        if (closes_stream(task->second.status.state)) {
            out = task->second;
            return FollowStep::Settled;
        }
    }
    // The handle answers from memory; the stores are read only once the run
    // has a result to project.
    if (!handle.try_result()) return FollowStep::Working;
    Task refreshed;
    if (!refresh_program_task(task_id, refreshed)) return FollowStep::Unavailable;
    if (!closes_stream(refreshed.status.state)) return FollowStep::Unavailable;
    out = std::move(refreshed);
    return FollowStep::Settled;
}

Task A2AServer::Impl::execute_program(const Message& inbound, const std::string& task_id,
                                      const std::string& context_id,
                                      std::uint64_t generation) {
    publish_working(task_id, generation,
                    json{{"program_backed", true}, {"reconnect_safe", true}});
    Task result;
    result.id = task_id;
    result.context_id = context_id;
    std::optional<program::ProgramHandle> handle;
    try {
        handle.emplace(program_adapter->start(inbound, task_id, context_id));
        bool cancel_before_publish = false;
        {
            std::lock_guard lk(tasks_mu);
            auto task_it = tasks.find(task_id);
            cancel_before_publish = task_it != tasks.end() &&
                                    task_it->second.status.state == TaskState::Canceled;
            program_runs.erase(task_id);
            program_runs.emplace(task_id, ProgramActiveRun{*handle, generation});
        }
        if (cancel_before_publish) handle->cancel();
        (void)handle->wait();
        result = program_adapter->task_snapshot(*handle, task_id, context_id);
        if (!result.status.timestamp) result.status.timestamp = detail::utc_timestamp_now();
    } catch (const ProgramA2ARequestError&) {
        // The adapter refused the request before any work: a rejection. Its
        // diagnostic text stays local.
        set_status(result, TaskState::Rejected,
                   agent_text(task_id, context_id,
                              "The agent's Program policy rejected the request."));
        result.metadata = json{{"neograph/failure", {{"code", "NG.Rejected"}, {"retryable", false}}}};
    } catch (...) {
        const auto failure = neograph::graph::classify_failure(std::current_exception());
        set_status(result, failure.failure_class == neograph::graph::NodeFailureClass::Cancelled
                               ? TaskState::Canceled : TaskState::Failed,
                   agent_text(task_id, context_id, public_text(failure)));
        result.metadata = json{{"neograph/failure", failure_metadata(failure)}};
    }
    {
        std::lock_guard lk(tasks_mu);
        auto run = program_runs.find(task_id);
        if (run != program_runs.end() && run->second.generation == generation
            && is_terminal(result.status.state)) {
            program_runs.erase(run);
        }
    }
    if (handle && is_terminal(result.status.state)) program_adapter->acknowledge_task(task_id);
    // finish_run appends status.message to the stored history; the snapshot's
    // own history is the Program's view and is not merged.
    result.history.clear();
    return result;
}
#endif

// ---------------------------------------------------------------------------
// SendMessage / SendStreamingMessage
// ---------------------------------------------------------------------------

void A2AServer::Impl::handle_send(Call& call) {
    if (call.streaming && !card.streaming) {
        // Spec 3.3.4: a card with streaming=false answers the streaming
        // methods with UnsupportedOperationError.
        reply_error(call, rpc_error(-32004, "Streaming is not supported by this agent"));
        return;
    }
    Violations violations;
    if (!call.params.contains("message")) {
        violations.emplace_back("message", "is required");
    } else {
        validate_message(call.params["message"], violations);
    }
    json configuration = call.params.contains("configuration") ? call.params["configuration"]
                                                               : json();
    if (!configuration.is_null()) {
        if (!configuration.is_object()) {
            violations.emplace_back("configuration", "must be an object");
        } else {
            validate_history_length(configuration, "configuration.historyLength", violations);
            for (const char* key : {"returnImmediately", "blocking"}) {
                if (configuration.contains(key) && !configuration[key].is_null()
                    && !configuration[key].is_boolean()) {
                    violations.emplace_back(std::string("configuration.") + key, "must be a boolean");
                }
            }
        }
    }
    if (call.params.contains("metadata") && !call.params["metadata"].is_null()
        && !call.params["metadata"].is_object()) {
        violations.emplace_back("metadata", "must be an object");
    }
    if (!violations.empty()) {
        auto e = rpc_error(-32602, "Invalid params");
        e.violations = violations_json(violations);
        reply_error(call, e);
        return;
    }
    if (configuration.is_object()) {
        for (const char* key : {"taskPushNotificationConfig", "pushNotificationConfig"}) {
            if (configuration.contains(key) && !configuration[key].is_null()) {
                reply_error(call, rpc_error(-32003,
                    "Push notifications are not supported by this agent"));
                return;
            }
        }
    }

    MessageSendParams mp;
    from_json(call.params, mp);
    const bool return_immediately = !call.streaming && mp.configuration
                                 && mp.configuration->blocking.has_value()
                                 && !*mp.configuration->blocking;
    const auto history_length = configuration.is_object() ? history_length_of(configuration)
                                                           : std::optional<int>{};

#ifdef NEOGRAPH_A2A_PROGRAM
    std::optional<CollaborationPeerIdentity> peer;
    bool collaboration = false;
    if (program_adapter) {
        peer = authenticate_collaboration_request(*call.req);
        collaboration = has_collaboration_marker(mp.message);
        if ((require_authenticated_requests || collaboration) && !peer) {
            reply_error(call, unauthenticated());
            return;
        }
        if (collaboration) {
            bool permitted = false;
            if (const auto& mailbox = program_adapter->mailbox()) {
                try {
                    const auto envelope = collaboration_from_message(mp.message);
                    permitted = mailbox->authenticates_sender(envelope.link_id, *peer);
                } catch (...) {
                    permitted = false;
                }
            }
            if (!permitted) {
                RpcError e = rpc_error(-32600,
                    "The authenticated peer may not send on this collaboration link");
                e.reason = "PERMISSION_DENIED";
                e.domain = kNeoGraphDomain;
                e.http_status = 403;
                reply_error(call, e);
                return;
            }
        }
        const auto host_task_id = mp.message.task_id.value_or(detail::random_uuid());
        const auto host_context_id = mp.message.context_id.value_or(detail::random_uuid());
        if (auto handled = program_adapter->handle_host_message(
                mp.message, host_task_id, host_context_id, peer)) {
            if (handled->id != host_task_id || handled->context_id != host_context_id) {
                // The host handler must answer for the task it was given.
                reply_error(call, rpc_error(-32603, "The agent's host handler failed"));
                return;
            }
            if (!handled->status.timestamp) handled->status.timestamp = detail::utc_timestamp_now();
            {
                std::lock_guard lk(tasks_mu);
                tasks[handled->id] = *handled;
                touch_task_unlocked(handled->id);
                evict_lru_unlocked();
            }
            if (call.streaming) {
                stream_task(call, *handled, nullptr, 0);
            } else {
                json tj = task_wire(*handled, call.dialect, history_length);
                if (call.dialect == WireDialect::V1_0) tj = json{{"task", std::move(tj)}};
                reply_result(call, std::move(tj));
            }
            return;
        }
    }
#endif

    // Resolve the task the message continues (spec 3.4.2 / 3.4.3). A new task
    // always gets a server-generated id.
    std::string task_id;
    std::string context_id;
    std::string resume_checkpoint;
    bool collaboration_new_task = false;
    // A Program run waiting for input cannot take its answer from an A2A
    // message yet. TODO(D3 phase 2): resume it with the message as the
    // input (ProgramRuntime::resume). Until then the message is refused and
    // the task keeps waiting, instead of reading as a busy task.
    const auto program_input_pending = [&](const Task& task) {
#ifdef NEOGRAPH_A2A_PROGRAM
        return program_adapter && !collaboration && task.status.state == TaskState::InputRequired;
#else
        (void)task;
        return false;
#endif
    };
#ifdef NEOGRAPH_A2A_PROGRAM
    if (program_adapter && mp.message.task_id) {
        Task refreshed;
        bool known = false;
        {
            std::lock_guard lk(tasks_mu);
            known = tasks.contains(*mp.message.task_id);
        }
        if (!known && !collaboration) {
            // A durable run that outlived this process is still the task.
            (void)refresh_program_task(*mp.message.task_id, refreshed);
        }
    }
#endif
    std::optional<RpcError> refusal;
    std::optional<Task> idempotent;
    std::shared_ptr<TaskChannel> channel;
    std::shared_ptr<neograph::graph::CancelToken> cancel_token;
    std::uint64_t generation = 0;
    Task snapshot;
    {
        std::lock_guard lk(tasks_mu);
        if (mp.message.task_id) {
            task_id = *mp.message.task_id;
            auto known = tasks.find(task_id);
            if (known == tasks.end()) {
#ifdef NEOGRAPH_A2A_PROGRAM
                // A NeoGraph collaboration envelope names its receiver task
                // up front, so delivery stays idempotent (an extension of the
                // A2A data model, used only between authenticated peers).
                if (collaboration) collaboration_new_task = true;
#endif
                if (!collaboration_new_task) {
                    refusal = rpc_error(-32001, "Task not found");
                }
            } else if (mp.message.context_id
                       && *mp.message.context_id != known->second.context_id) {
                refusal = rpc_error(-32602, "Invalid params: contextId does not match the task");
                refusal->violations = violations_json(
                    {{"message.contextId", "does not match the task's contextId"}});
            } else if (is_terminal(known->second.status.state)) {
#ifdef NEOGRAPH_A2A_PROGRAM
                if (collaboration) {
                    idempotent = known->second;  // duplicate delivery
                }
#endif
                if (!idempotent) {
                    refusal = rpc_error(-32004,
                        "Task " + task_id + " is in a terminal state and accepts no more messages");
                }
            } else if (program_input_pending(known->second)) {
                refusal = rpc_error(-32004, "Task " + task_id + " waits for Program input, "
                                            "which this agent does not accept over A2A yet");
            } else if (has_run_unlocked(task_id)) {
                refusal = rpc_error(-32603, "Task " + task_id + " is still running");
                refusal->reason = "TASK_BUSY";
                refusal->domain = kNeoGraphDomain;
            } else {
                context_id = known->second.context_id;
                if (is_interrupted(known->second.status.state)) {
                    resume_checkpoint = interrupt_checkpoint(known->second);
                }
            }
        }
        if (!refusal && !idempotent) {
            if (inflight_unlocked() >= max_inflight_runs) {
                refusal = rpc_error(-32603, "The agent is at its in-flight task limit; retry later");
                refusal->reason = "RESOURCE_EXHAUSTED";
                refusal->domain = kNeoGraphDomain;
                refusal->metadata = json{{"retryAfterSeconds", "1"}};
            }
        }
        if (!refusal && !idempotent) {
            if (task_id.empty()) task_id = detail::random_uuid();
            if (context_id.empty()) {
                context_id = mp.message.context_id ? *mp.message.context_id
                                                   : detail::random_uuid();
            }
            Message inbound = mp.message;
            inbound.task_id    = task_id;
            inbound.context_id = context_id;
            auto& task = tasks[task_id];
            if (task.id.empty()) {
                task.id         = task_id;
                task.context_id = context_id;
                set_status(task, TaskState::Submitted);
#ifdef NEOGRAPH_A2A_PROGRAM
                if (program_adapter) {
                    task.metadata = json{{"program_backed", true}, {"reconnect_safe", true}};
                }
#endif
            } else {
                // A follow-up resumes the interrupted task; the stream it
                // opens starts in the working state.
                set_status(task, TaskState::Working);
                task.metadata = without_key(task.metadata, "neograph/interrupt");
            }
            task.history.push_back(std::move(inbound));
            cancel_token = std::make_shared<neograph::graph::CancelToken>();
            channel = std::make_shared<TaskChannel>();
            generation = next_generation++;
            active_runs[task_id] = ActiveRun{cancel_token, generation, channel};
            channels[task_id] = channel;
            snapshot = task;
            touch_task_unlocked(task_id);
            evict_lru_unlocked();
        }
    }
    if (refusal) {
        reply_error(call, *refusal);
        return;
    }
    if (idempotent) {
        if (call.streaming) {
            stream_task(call, *idempotent, nullptr, 0);
        } else {
            json tj = task_wire(*idempotent, call.dialect, history_length);
            if (call.dialect == WireDialect::V1_0) tj = json{{"task", std::move(tj)}};
            reply_result(call, std::move(tj));
        }
        return;
    }

    Message inbound = mp.message;
    inbound.task_id    = task_id;
    inbound.context_id = context_id;
    try {
        launch_worker([this, inbound, task_id, context_id, resume_checkpoint, cancel_token,
                       generation] {
            Task outcome;
#ifdef NEOGRAPH_A2A_PROGRAM
            if (program_adapter) {
                outcome = execute_program(inbound, task_id, context_id, generation);
            } else
#endif
            {
                outcome = execute_graph(inbound, task_id, context_id, resume_checkpoint,
                                        cancel_token, generation);
            }
            finish_run(task_id, generation, outcome);
        });
    } catch (const std::exception&) {
        Task failed;
        set_status(failed, TaskState::Failed,
                   agent_text(task_id, context_id, "The agent could not start the run."));
        finish_run(task_id, generation, failed);
        auto e = rpc_error(-32603, "The agent could not start the run; retry later");
        e.reason = "RESOURCE_EXHAUSTED";
        e.domain = kNeoGraphDomain;
        reply_error(call, e);
        return;
    }

    if (call.streaming) {
        stream_task(call, std::move(snapshot), channel, 0);
        return;
    }
    if (!return_immediately) {
        // Blocking send (spec 3.2.2): wait for the terminal or interrupted
        // state; the run itself is independent of this connection.
        std::unique_lock lk(tasks_mu);
        channel->cv.wait(lk, [&] {
            return channel->closed || stopping.load(std::memory_order_acquire);
        });
        if (auto it = tasks.find(task_id); it != tasks.end()) snapshot = it->second;
    }
    json tj = task_wire(snapshot, call.dialect, history_length);
    if (call.dialect == WireDialect::V1_0) tj = json{{"task", std::move(tj)}};
    reply_result(call, std::move(tj));
}

void A2AServer::Impl::stream_task(Call& call, Task snapshot,
                                  std::shared_ptr<TaskChannel> channel, std::size_t cursor,
                                  FollowPoll follow) {
    const auto rpc_id  = call.id;
    const auto dialect = call.dialect;
    auto* self = this;
    call.res->set_chunked_content_provider(
        "text/event-stream",
        [self, rpc_id, dialect, snapshot = std::move(snapshot), channel, cursor,
         follow = std::move(follow)](size_t /*offset*/, httplib::DataSink& sink) mutable {
            using Clock = std::chrono::steady_clock;
            auto last_sent = Clock::now();
            auto write = [&](json result) {
                const auto frame = sse_frame(jsonrpc_result(std::move(result), rpc_id));
                const bool sent = sink.write(frame.data(), frame.size());
                last_sent = Clock::now();
                return sent;
            };
            // Spec 3.1.2: a task lifecycle stream opens with the Task. The 0.3
            // stream does the same (A2A-14) and ends on `final: true`.
            {
                json tj;
                to_json(tj, snapshot, dialect);
                if (dialect == WireDialect::V1_0) tj = json{{"task", std::move(tj)}};
                if (!write(std::move(tj))) return false;
            }
            if (channel) {
                std::unique_lock lk(self->tasks_mu);
                for (;;) {
                    while (cursor < channel->items.size()) {
                        auto item = channel->items[cursor++];
                        lk.unlock();
                        json ej;
                        if (item.is_artifact) {
                            to_json(ej, item.artifact, dialect);
                            if (dialect == WireDialect::V1_0)
                                ej = json{{"artifactUpdate", std::move(ej)}};
                        } else {
                            to_json(ej, item.status, dialect);
                            if (dialect == WireDialect::V1_0)
                                ej = json{{"statusUpdate", std::move(ej)}};
                        }
                        if (!write(std::move(ej))) return false;
                        lk.lock();
                    }
                    if (channel->closed || self->stopping.load(std::memory_order_acquire)) break;
                    // Wake for the next keep-alive too, not only every 250 ms.
                    const auto keepalive = self->stream_keepalive;
                    const auto until_due = std::max(
                        0ms, std::chrono::duration_cast<std::chrono::milliseconds>(
                                 last_sent + keepalive - Clock::now()));
                    channel->cv.wait_for(lk, std::min<std::chrono::milliseconds>(250ms, until_due));
                    if (cursor >= channel->items.size() && !channel->closed) {
                        const bool due = Clock::now() - last_sent >= keepalive;
                        lk.unlock();
                        // A quiet stream sends an SSE comment now and then, so
                        // a client or proxy that times out an idle connection
                        // does not cut a run that is still working. The write
                        // also finds a client that went away.
                        static constexpr std::string_view kComment = ": keep-alive\n\n";
                        bool alive = true;
                        if (due) {
                            alive = sink.write(kComment.data(), kComment.size());
                            last_sent = Clock::now();
                        } else {
                            alive = !sink.is_writable || sink.is_writable();
                        }
                        lk.lock();
                        if (!alive) return false;  // the client went away
                    }
                }
            } else if (follow) {
                // No channel carries this run's events: this server did not
                // start it (a Program task recovered after a restart). Follow
                // the durable run until it settles, then end with its final
                // status, or with an error when the run cannot be followed.
                std::vector<std::string> sent_artifacts;
                for (const auto& artifact : snapshot.artifacts)
                    sent_artifacts.push_back(artifact.artifact_id);
                for (;;) {
                    Task settled;
                    const auto step = follow(settled);
                    if (step == FollowStep::Settled) {
                        for (const auto& artifact : settled.artifacts) {
                            if (std::find(sent_artifacts.begin(), sent_artifacts.end(),
                                          artifact.artifact_id) != sent_artifacts.end())
                                continue;
                            TaskArtifactUpdateEvent update;
                            update.task_id    = settled.id;
                            update.context_id = settled.context_id;
                            update.artifact   = artifact;
                            update.last_chunk = true;
                            json ej;
                            to_json(ej, update, dialect);
                            if (dialect == WireDialect::V1_0)
                                ej = json{{"artifactUpdate", std::move(ej)}};
                            if (!write(std::move(ej))) return false;
                        }
                        TaskStatusUpdateEvent update;
                        update.task_id    = settled.id;
                        update.context_id = settled.context_id;
                        update.status     = settled.status;
                        update.final      = true;
                        if (settled.metadata.is_object() && !settled.metadata.empty())
                            update.metadata = settled.metadata;
                        json ej;
                        to_json(ej, update, dialect);
                        if (dialect == WireDialect::V1_0)
                            ej = json{{"statusUpdate", std::move(ej)}};
                        if (!write(std::move(ej))) return false;
                        break;
                    }
                    if (step == FollowStep::Unavailable) {
                        auto e = rpc_error(-32603, "The task's run can no longer be followed; "
                                                   "fetch the task with GetTask");
                        e.reason = "RUN_UNAVAILABLE";
                        e.domain = kNeoGraphDomain;
                        const auto frame = sse_frame(jsonrpc_error(e, rpc_id));
                        if (!sink.write(frame.data(), frame.size())) return false;
                        break;
                    }
                    if (self->stopping.load(std::memory_order_acquire)) break;
                    std::chrono::milliseconds keepalive;
                    {
                        std::lock_guard lk(self->tasks_mu);
                        keepalive = self->stream_keepalive;
                    }
                    const auto until_due = std::max(
                        0ms, std::chrono::duration_cast<std::chrono::milliseconds>(
                                 last_sent + keepalive - Clock::now()));
                    std::this_thread::sleep_for(std::min<std::chrono::milliseconds>(
                        kFollowPollInterval, until_due));
                    // As for a channel: a quiet stream says so with an SSE
                    // comment, and the write finds a client that went away.
                    bool alive = true;
                    if (Clock::now() - last_sent >= keepalive) {
                        static constexpr std::string_view kComment = ": keep-alive\n\n";
                        alive = sink.write(kComment.data(), kComment.size());
                        last_sent = Clock::now();
                    } else {
                        alive = !sink.is_writable || sink.is_writable();
                    }
                    if (!alive) return false;
                }
            }
            sink.done();
            return true;
        });
}

// ---------------------------------------------------------------------------
// GetTask / CancelTask / ListTasks / SubscribeToTask
// ---------------------------------------------------------------------------

void A2AServer::Impl::handle_get(Call& call) {
    Violations violations;
    validate_task_id(call.params, violations);
    validate_history_length(call.params, "historyLength", violations);
    if (!violations.empty()) {
        auto e = rpc_error(-32602, "Invalid params");
        e.violations = violations_json(violations);
        reply_error(call, e);
        return;
    }
    const auto task_id = call.params["id"].get<std::string>();
    Task t;
    bool found = false;
#ifdef NEOGRAPH_A2A_PROGRAM
    if (program_adapter) {
        const auto peer = authenticate_collaboration_request(*call.req);
        if (require_authenticated_requests && !peer) {
            reply_error(call, unauthenticated());
            return;
        }
        if (authorize_collaboration_task(task_id, peer, false)
            == CollaborationTaskAuthorization::Unauthorized) {
            reply_error(call, rpc_error(-32001, "Task not found"));
            return;
        }
        found = refresh_program_task(task_id, t);
    }
#endif
    if (!found) {
        std::lock_guard lk(tasks_mu);
        if (const auto it = tasks.find(task_id); it != tasks.end()) {
            t = it->second;
            found = true;
            touch_task_unlocked(task_id);  // GetTask is a recency signal
        }
    }
    if (!found) {
        reply_error(call, rpc_error(-32001, "Task not found"));
        return;
    }
    reply_result(call, task_wire(std::move(t), call.dialect, history_length_of(call.params)));
}

void A2AServer::Impl::handle_cancel(Call& call) {
    Violations violations;
    validate_task_id(call.params, violations);
    if (call.params.contains("metadata") && !call.params["metadata"].is_null()
        && !call.params["metadata"].is_object()) {
        violations.emplace_back("metadata", "must be an object");
    }
    if (!violations.empty()) {
        auto e = rpc_error(-32602, "Invalid params");
        e.violations = violations_json(violations);
        reply_error(call, e);
        return;
    }
    const auto task_id = call.params["id"].get<std::string>();
#ifdef NEOGRAPH_A2A_PROGRAM
    if (program_adapter) {
        const auto peer = authenticate_collaboration_request(*call.req);
        if (require_authenticated_requests && !peer) {
            reply_error(call, unauthenticated());
            return;
        }
        if (authorize_collaboration_task(task_id, peer, true)
            == CollaborationTaskAuthorization::Unauthorized) {
            reply_error(call, rpc_error(-32001, "Task not found"));
            return;
        }
        bool in_memory = false;
        {
            std::lock_guard lk(tasks_mu);
            in_memory = tasks.contains(task_id);
        }
        Task refreshed;
        if (!in_memory && !refresh_program_task(task_id, refreshed)) {
            reply_error(call, rpc_error(-32001, "Task not found"));
            return;
        }
    }
#endif
    Task t;
    std::shared_ptr<neograph::graph::CancelToken> cancel_token;
#ifdef NEOGRAPH_A2A_PROGRAM
    std::optional<program::ProgramHandle> program_handle;
#endif
    {
        std::lock_guard lk(tasks_mu);
        auto it = tasks.find(task_id);
        if (it == tasks.end()) {
            reply_error(call, rpc_error(-32001, "Task not found"));
            return;
        }
        if (is_terminal(it->second.status.state)) {
            // Spec 3.1.5: a terminal task is not cancelable; its state stays.
            reply_error(call, rpc_error(-32002,
                "Task cannot be canceled - current state: "
                + task_state_to_string(it->second.status.state, WireDialect::V1_0)));
            return;
        }
        set_status(it->second, TaskState::Canceled);
        touch_task_unlocked(task_id);
        if (auto run = active_runs.find(task_id); run != active_runs.end()) {
            cancel_token = run->second.cancel_token;
            auto& channel = run->second.channel;
            if (!channel->closed) {
                StreamItem item;
                item.status.task_id    = task_id;
                item.status.context_id = it->second.context_id;
                item.status.status     = it->second.status;
                item.status.final      = true;
                channel->items.push_back(std::move(item));
                channel->closed = true;
                channel->cv.notify_all();
            }
        }
#ifdef NEOGRAPH_A2A_PROGRAM
        if (auto run = program_runs.find(task_id); run != program_runs.end()) {
            program_handle = run->second.handle;
        }
#endif
        t = it->second;
    }
    if (cancel_token) cancel_token->cancel();
#ifdef NEOGRAPH_A2A_PROGRAM
    if (program_adapter && !program_handle) {
        try {
            program_handle.emplace(program_adapter->reconnect(task_id));
        } catch (const std::exception&) {
            // A task this server never ran durably has no run to signal.
        }
    }
    if (program_handle) program_handle->cancel();
#endif
    reply_result(call, task_wire(std::move(t), call.dialect, std::nullopt));
}

void A2AServer::Impl::handle_list(Call& call) {
    const auto& p = call.params;
    Violations violations;
    for (const char* key : {"tenant", "contextId", "pageToken"}) {
        if (p.contains(key) && !p[key].is_null() && !p[key].is_string())
            violations.emplace_back(key, "must be a string");
    }
    std::optional<TaskState> status_filter;
    if (p.contains("status") && !p["status"].is_null()) {
        if (p["status"].is_string()) {
            const auto name = p["status"].get<std::string>();
            const auto state = task_state_from_string(name);
            if (name == "TASK_STATE_UNSPECIFIED" || name.empty()) {
                // no filter
            } else if (state == TaskState::Unknown) {
                violations.emplace_back("status", "must be a TaskState name");
            } else {
                status_filter = state;
            }
        } else if (!(p["status"].is_number_integer() && p["status"].get<int>() == 0)) {
            violations.emplace_back("status", "must be a TaskState name");
        }
    }
    int page_size = kDefaultPageSize;
    if (p.contains("pageSize") && !p["pageSize"].is_null()) {
        if (!p["pageSize"].is_number_integer() || p["pageSize"].get<long long>() < 1
            || p["pageSize"].get<long long>() > kMaxPageSize) {
            violations.emplace_back("pageSize", "must be an integer from 1 to 100");
        } else {
            page_size = p["pageSize"].get<int>();
        }
    }
    validate_history_length(p, "historyLength", violations);
    std::optional<std::int64_t> after_ms;
    if (p.contains("statusTimestampAfter") && !p["statusTimestampAfter"].is_null()) {
        if (p["statusTimestampAfter"].is_string())
            after_ms = detail::parse_timestamp_ms(p["statusTimestampAfter"].get<std::string>());
        if (!after_ms) violations.emplace_back("statusTimestampAfter", "must be an RFC 3339 timestamp");
    }
    if (p.contains("includeArtifacts") && !p["includeArtifacts"].is_null()
        && !p["includeArtifacts"].is_boolean()) {
        violations.emplace_back("includeArtifacts", "must be a boolean");
    }
    std::optional<std::pair<std::string, std::string>> cursor;
    if (is_string(p, "pageToken") && !p["pageToken"].get<std::string>().empty()) {
        const auto decoded = hex_decode(p["pageToken"].get<std::string>());
        const auto sep = decoded ? decoded->find('\x1f') : std::string::npos;
        if (sep == std::string::npos) {
            violations.emplace_back("pageToken", "is not a token this agent issued");
        } else {
            cursor = std::make_pair(decoded->substr(0, sep), decoded->substr(sep + 1));
        }
    }
    if (!violations.empty()) {
        auto e = rpc_error(-32602, "Invalid params");
        e.violations = violations_json(violations);
        reply_error(call, e);
        return;
    }
    const bool include_artifacts = p.value("includeArtifacts", false);
    const auto history_length = history_length_of(p);
    const auto context_filter = p.value("contextId", std::string());

#ifdef NEOGRAPH_A2A_PROGRAM
    std::optional<CollaborationPeerIdentity> peer;
    if (program_adapter) {
        peer = authenticate_collaboration_request(*call.req);
        if (require_authenticated_requests && !peer) {
            reply_error(call, unauthenticated());
            return;
        }
    }
#endif
    std::vector<Task> listed;
    {
        std::lock_guard lk(tasks_mu);
        listed.reserve(tasks.size());
        for (const auto& [id, task] : tasks) {
            if (!context_filter.empty() && task.context_id != context_filter) continue;
            if (status_filter && task.status.state != *status_filter) continue;
            if (after_ms) {
                const auto ts = task.status.timestamp
                    ? detail::parse_timestamp_ms(*task.status.timestamp) : std::nullopt;
                if (!ts || *ts < *after_ms) continue;
            }
            listed.push_back(task);
        }
    }
#ifdef NEOGRAPH_A2A_PROGRAM
    if (program_adapter) {
        // Owner scope: a task linked to a collaboration is listed only for a
        // peer that may read it.
        listed.erase(std::remove_if(listed.begin(), listed.end(), [&](const Task& task) {
            return authorize_collaboration_task(task.id, peer, false)
                == CollaborationTaskAuthorization::Unauthorized;
        }), listed.end());
    }
#endif
    // Most recently updated first; the id breaks ties so pages are stable.
    auto key = [](const Task& task) {
        return std::make_pair(task.status.timestamp.value_or(std::string()), task.id);
    };
    std::sort(listed.begin(), listed.end(), [&](const Task& a, const Task& b) {
        return key(a) > key(b);
    });
    const auto total = listed.size();
    std::size_t start = 0;
    if (cursor) {
        while (start < listed.size() && key(listed[start]) >= *cursor) ++start;
    }
    const auto end = std::min(listed.size(), start + static_cast<std::size_t>(page_size));
    std::string next_token;
    if (end < listed.size()) {
        const auto last = key(listed[end - 1]);
        next_token = hex_encode(last.first + '\x1f' + last.second);
    }
    auto arr = json::array();
    for (std::size_t i = start; i < end; ++i) {
        arr.push_back(task_wire(listed[i], call.dialect, history_length, include_artifacts));
    }
    json result = json::object();
    result["tasks"]         = std::move(arr);
    result["nextPageToken"] = next_token;
    result["pageSize"]      = page_size;
    result["totalSize"]     = static_cast<long long>(total);
    reply_result(call, std::move(result));
}

void A2AServer::Impl::handle_subscribe(Call& call) {
    if (!card.streaming) {
        reply_error(call, rpc_error(-32004, "Streaming is not supported by this agent"));
        return;
    }
    Violations violations;
    validate_task_id(call.params, violations);
    if (!violations.empty()) {
        auto e = rpc_error(-32602, "Invalid params");
        e.violations = violations_json(violations);
        reply_error(call, e);
        return;
    }
    const auto task_id = call.params["id"].get<std::string>();
#ifdef NEOGRAPH_A2A_PROGRAM
    if (program_adapter) {
        const auto peer = authenticate_collaboration_request(*call.req);
        if (require_authenticated_requests && !peer) {
            reply_error(call, unauthenticated());
            return;
        }
        if (authorize_collaboration_task(task_id, peer, false)
            == CollaborationTaskAuthorization::Unauthorized) {
            reply_error(call, rpc_error(-32001, "Task not found"));
            return;
        }
        bool in_memory = false;
        bool driven = false;
        {
            std::lock_guard lk(tasks_mu);
            in_memory = tasks.contains(task_id);
            driven = active_runs.contains(task_id);
        }
        // A run this server drives publishes its own state. Any other Program
        // task is read from its durable run first, so a recovered run that
        // settled since the restart is answered as the terminal task it is
        // and not followed.
        Task refreshed;
        if (!in_memory || !driven) (void)refresh_program_task(task_id, refreshed);
    }
#endif
    Task snapshot;
    std::shared_ptr<TaskChannel> channel;
    std::size_t cursor = 0;
    FollowPoll follow;
    {
        std::lock_guard lk(tasks_mu);
        auto it = tasks.find(task_id);
        if (it == tasks.end()) {
            reply_error(call, rpc_error(-32001, "Task not found"));
            return;
        }
        if (is_terminal(it->second.status.state)) {
            // Spec 9.4.6: a terminal task has no stream to join.
            reply_error(call, rpc_error(-32004,
                "Task " + task_id + " is in a terminal state; there is nothing to subscribe to"));
            return;
        }
        snapshot = it->second;
        if (auto run = active_runs.find(task_id); run != active_runs.end()) {
            channel = run->second.channel;
            cursor  = channel->items.size();
        }
#ifdef NEOGRAPH_A2A_PROGRAM
        if (!channel && program_adapter && !closes_stream(snapshot.status.state)) {
            // A running task with no channel is a Program task this server did
            // not start (recovered after a restart): follow its durable run.
            if (const auto run = program_runs.find(task_id); run != program_runs.end()) {
                follow = [this, task_id, handle = run->second.handle](Task& settled) {
                    return poll_program_task(task_id, handle, settled);
                };
            } else {
                reply_error(call, rpc_error(-32603,
                    "Task " + task_id + " is not running here and cannot be followed; "
                    "fetch it with GetTask"));
                return;
            }
        }
#endif
    }
    stream_task(call, std::move(snapshot), channel, cursor, std::move(follow));
}

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------
A2AServer::A2AServer(std::shared_ptr<neograph::graph::GraphEngine> engine,
                     AgentCard card,
                     std::shared_ptr<GraphAgentAdapter> adapter)
    : A2AServer(neograph::graph::GraphExecution(std::move(engine)),
                std::move(card), std::move(adapter)) {}

A2AServer::A2AServer(neograph::graph::GraphExecution execution,
                     AgentCard card,
                     std::shared_ptr<GraphAgentAdapter> adapter)
    : impl_(std::make_unique<Impl>()) {
    impl_->execution = std::move(execution);
    impl_->card    = std::move(card);
    impl_->adapter = adapter ? adapter : std::make_shared<GraphAgentAdapter>();
    impl_->register_routes();
}

#ifdef NEOGRAPH_A2A_PROGRAM
A2AServer::A2AServer(std::shared_ptr<ProgramAgentAdapter> adapter,
                     AgentCard card,
                     CollaborationAuthenticator collaboration_authenticator,
                     bool require_authenticated_requests)
    : impl_(std::make_unique<Impl>()) {
    if (!adapter) throw std::invalid_argument("A2AServer: ProgramAgentAdapter is null");
    impl_->program_adapter = std::move(adapter);
    impl_->collaboration_authenticator = std::move(collaboration_authenticator);
    impl_->require_authenticated_requests = require_authenticated_requests;
    impl_->card = std::move(card);
    impl_->register_routes();
}

A2AServer::A2AServer(std::shared_ptr<neograph::program::ProgramRuntime> runtime,
                     neograph::program::ProgramVersion version,
                     std::string owner_scope,
                     AgentCard card,
                     std::shared_ptr<CollaborationMailbox> mailbox,
                     CollaborationAuthenticator collaboration_authenticator,
                     bool require_authenticated_requests)
    : A2AServer(std::make_shared<ProgramAgentAdapter>(
                    std::move(runtime), std::move(version), std::move(owner_scope),
                    std::move(mailbox)),
                std::move(card), std::move(collaboration_authenticator),
                require_authenticated_requests) {}

A2AServer::A2AServer(std::shared_ptr<neograph::program::ProgramRuntime> runtime,
                     neograph::program::ProgramVersion version,
                     AgentCard card,
                     std::string owner_scope,
                     std::shared_ptr<CollaborationMailbox> mailbox,
                     CollaborationAuthenticator collaboration_authenticator,
                     bool require_authenticated_requests)
    : A2AServer(std::move(runtime), std::move(version), std::move(owner_scope),
                std::move(card), std::move(mailbox),
                std::move(collaboration_authenticator),
                require_authenticated_requests) {}
#endif

A2AServer::~A2AServer() {
    stop();
    // Nothing can wait on a stream any more; release any that would.
    impl_->stopping.store(true, std::memory_order_release);
    impl_->wake_streams();
}

bool A2AServer::start(const std::string& host, int port) {
    if (port == 0) {
        impl_->bound_port = impl_->svr.bind_to_any_port(host);
        if (impl_->bound_port < 0) return false;
    } else {
        if (!impl_->svr.bind_to_port(host, port)) return false;
        impl_->bound_port = port;
    }
    impl_->bind_card_endpoint(host);
#ifdef NEOGRAPH_A2A_PROGRAM
    try {
        impl_->recover_program_tasks();
    } catch (...) {
        impl_->svr.stop();
        impl_->bound_port = 0;
        throw;
    }
#endif
    impl_->stopping.store(false, std::memory_order_release);
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
    impl_->bind_card_endpoint(host);
#ifdef NEOGRAPH_A2A_PROGRAM
    try {
        impl_->recover_program_tasks();
    } catch (...) {
        impl_->svr.stop();
        impl_->bound_port = 0;
        throw;
    }
#endif
    impl_->stopping.store(false, std::memory_order_release);
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
    // Drain: the listener stops accepting, open streams and blocking sends
    // end when their runs publish a final state, and the runs finish on their
    // workers. A server stop never cancels a run (a Program run is durable).
    if (impl_->svr.is_running()) impl_->svr.stop();
    if (impl_->listener.joinable()) impl_->listener.join();
    impl_->join_workers();
}

bool A2AServer::is_running() const { return impl_->running.load(std::memory_order_acquire); }

int A2AServer::port() const { return impl_->bound_port; }

void A2AServer::set_trust_forwarded_headers(bool enabled) {
    impl_->trust_forwarded_headers.store(enabled, std::memory_order_relaxed);
}

}  // namespace neograph::a2a
