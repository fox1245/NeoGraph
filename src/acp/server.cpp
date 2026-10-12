#include <neograph/acp/server.h>

#include <neograph/graph/cancel.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/types.h>
#include <neograph/graph/invocation.h>
#include <neograph/tool.h>

#include "../core/tool_failure.h"

#ifdef NEOGRAPH_ACP_MCP
#include <neograph/mcp/client.h>
#include <neograph/mcp/types.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <set>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <crt_externs.h>
#elif !defined(_WIN32)
extern char** environ;
#endif

#ifndef NEOGRAPH_VERSION_STR
#define NEOGRAPH_VERSION_STR "unknown"
#endif

namespace neograph::acp {

// ---------------------------------------------------------------------------
// ACPGraphAdapter — defaults
// ---------------------------------------------------------------------------
std::string
ACPGraphAdapter::extract_user_text(const std::vector<ContentBlock>& blocks) const {
    std::string out;
    auto append = [&out](const std::string& piece) {
        if (piece.empty()) return;
        if (!out.empty()) out.push_back(' ');
        out.append(piece);
    };
    for (const auto& b : blocks) {
        if (b.type == "text") {
            append(b.text);
        } else if (b.type == "resource_link") {
            // ResourceLink is baseline prompt content (ACP initialization):
            // keep it visible to the graph as a Markdown link.
            append("[" + (b.name.empty() ? b.uri : b.name) + "](" + b.uri + ")");
        } else if (b.type == "resource" && b.resource.is_object()) {
            const auto resource = b.resource;
            if (resource.contains("text") && resource["text"].is_string()) {
                append(resource["text"].get<std::string>());
            } else if (resource.contains("uri") && resource["uri"].is_string()) {
                const auto uri = resource["uri"].get<std::string>();
                append("[" + uri + "](" + uri + ")");
            }
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

neograph::json
ACPGraphAdapter::build_session_state(
    const std::vector<ContentBlock>& blocks,
    const ACPSessionContext& session) const {
    auto state = build_initial_state(blocks, session.session_id);
    if (state.is_object()) {
        if (!state.contains("_acp_cwd")) state["_acp_cwd"] = session.cwd;
        if (!state.contains("_acp_additional_directories")) {
            auto dirs = neograph::json::array();
            for (const auto& dir : session.additional_directories) dirs.push_back(dir);
            state["_acp_additional_directories"] = std::move(dirs);
        }
        if (!state.contains("_acp_mcp_tools")) {
            state["_acp_mcp_tools"] = session.mcp_tools.is_array()
                ? session.mcp_tools : neograph::json::array();
        }
    }
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
            // The agent's reply is the last assistant message. A trailing tool
            // result is not a reply, and a failed one holds the tool's error
            // text, which must not reach the client.
            if (msgs.is_array()) {
                for (std::size_t i = msgs.size(); i-- > 0;) {
                    auto message = msgs[i];
                    if (!message.is_object()) continue;
                    const auto role = message.value("role", std::string("assistant"));
                    if (role != "assistant") continue;
                    if (!message.contains("content")) return "";
                    auto c = message["content"];
                    if (c.is_string()) return c.get<std::string>();
                    return c.dump();
                }
            }
        }
    }
    return "";
}

std::optional<ACPInterruptPermission>
ACPGraphAdapter::interrupt_permission(const ACPInterrupt& interrupt) const {
    ACPInterruptPermission permission;
    auto& call = permission.tool_call;
    call.tool_call_id = "neograph-interrupt-"
        + (interrupt.checkpoint_id.empty() ? interrupt.session_id : interrupt.checkpoint_id);
    call.title = interrupt.reason.empty()
        ? std::string("The agent needs your approval to continue")
        : interrupt.reason;
    call.kind = "other";
    call.status = "pending";
    neograph::json raw = neograph::json::object();
    if (!interrupt.payload.is_null()) raw["rawInput"] = interrupt.payload;
    // ACP reserves `_meta` for implementation metadata.
    raw["_meta"] = neograph::json{
        {"neograph/interrupt", {
            {"node", interrupt.node},
            {"reason", interrupt.reason},
            {"payload", interrupt.payload},
            {"checkpointId", interrupt.checkpoint_id},
        }},
    };
    call.raw = std::move(raw);
    permission.options = {
        {"approve", "Approve", "allow_once"},
        {"reject", "Reject", "reject_once"},
    };
    return permission;
}

neograph::json
ACPGraphAdapter::interrupt_resume_value(const ACPInterrupt&,
                                        const std::string& option_id) const {
    return neograph::json(option_id);
}

namespace server_internal {
/// One client-supplied stdio MCP server, validated.
struct McpServerSpec {
    std::string name;
    std::string command;
    std::vector<std::string> args;
    std::vector<std::pair<std::string, std::string>> env;
};
}  // namespace server_internal
using server_internal::McpServerSpec;

// ---------------------------------------------------------------------------
// JSON-RPC envelope helpers
// ---------------------------------------------------------------------------
namespace {

/// ACP ErrorCode values (schema/v1 ErrorCode) used by this server.
constexpr int kInvalidRequest   = -32600;
constexpr int kMethodNotFound   = -32601;
constexpr int kInvalidParams    = -32602;
constexpr int kInternalError    = -32603;
constexpr int kRequestCancelled = -32800;
constexpr int kResourceNotFound = -32002;

/// How long a cancelled turn waits for the client to answer a pending
/// agent->client request on its own before the server abandons it.
constexpr auto kClientRequestCancelGrace = std::chrono::milliseconds(500);

neograph::json jsonrpc_error(int code, std::string msg, const neograph::json& id,
                             neograph::json data = neograph::json()) {
    neograph::json env;
    env["jsonrpc"] = "2.0";
    env["id"]      = id;
    neograph::json e;
    e["code"]    = code;
    e["message"] = std::move(msg);
    if (!data.is_null()) e["data"] = std::move(data);
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

/// A refusal of a request the server cannot serve right now. ACP reserves
/// -32000 for "Authentication required", so capacity and single-flight
/// refusals are internal errors with a machine-readable reason.
neograph::json busy_error(std::string message, const char* reason,
                          const neograph::json& id) {
    return jsonrpc_error(kInternalError, std::move(message), id,
                         neograph::json{{"reason", reason}});
}

/// Session ids are unique across processes: a resumed session must never
/// collide with an id another agent process persisted.
std::string fresh_session_id() {
    static std::mutex mutex;
    static std::random_device device;
    std::array<std::uint32_t, 4> words{};
    {
        std::lock_guard lock(mutex);
        for (auto& word : words) word = device();
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), "sess-%08x%08x%08x%08x",
                  static_cast<unsigned>(words[0]), static_cast<unsigned>(words[1]),
                  static_cast<unsigned>(words[2]), static_cast<unsigned>(words[3]));
    return buf;
}

bool is_interrupt_phase(neograph::graph::CheckpointPhase phase) {
    return phase == neograph::graph::CheckpointPhase::Before ||
           phase == neograph::graph::CheckpointPhase::After ||
           phase == neograph::graph::CheckpointPhase::NodeInterrupt;
}


bool valid_request_id(const neograph::json& id) {
    return id.is_null() || id.is_string() || id.is_number_integer();
}

bool require_object(const neograph::json& value) {
    return value.is_object();
}

bool has_string(const neograph::json& object, const char* key,
                bool allow_empty = false) {
    if (!object.contains(key) || !object[key].is_string()) return false;
    return allow_empty || !object[key].get<std::string>().empty();
}

bool has_array(const neograph::json& object, const char* key) {
    return object.contains(key) && object[key].is_array();
}

// ACP workspace paths are absolute: POSIX (/work), or Windows with a drive
// letter and a separator (C:\work, C:/work) or a UNC share (\\host\share).
bool is_absolute_path(const std::string& path) {
    if (path.empty()) return false;
    if (path.front() == '/') return true;
    const auto letter = static_cast<unsigned char>(path.front());
    if (path.size() >= 3 && path[1] == ':' && (path[2] == '\\' || path[2] == '/') &&
        ((letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z')))
        return true;
    return path.size() >= 3 && path[0] == '\\' && path[1] == '\\';
}

/// agentInfo must be an ACP Implementation: string name and version, optional
/// string title and object _meta. Fill what is missing, drop the rest.
neograph::json normalize_agent_info(const neograph::json& info) {
    std::string name = "neograph-agent";
    std::string version = NEOGRAPH_VERSION_STR;
    neograph::json out = neograph::json::object();
    if (info.is_object()) {
        if (has_string(info, "name")) name = info["name"].get<std::string>();
        if (has_string(info, "version")) version = info["version"].get<std::string>();
    }
    out["name"] = name;
    out["version"] = version;
    if (info.is_object()) {
        if (info.contains("title") && info["title"].is_string()) out["title"] = info["title"];
        if (info.contains("_meta") && info["_meta"].is_object()) out["_meta"] = info["_meta"];
    }
    return out;
}

/// Validate a session/prompt ContentBlock array against the schema and the
/// advertised promptCapabilities. Returns the error message, or empty.
std::string validate_prompt_blocks(const neograph::json& prompt,
                                   const PromptCapabilities& caps) {
    for (const auto& block : prompt) {
        if (!block.is_object() || !has_string(block, "type")) {
            return "Invalid params: malformed content block";
        }
        const auto type = block["type"].get<std::string>();
        if (type == "text") {
            if (!has_string(block, "text", true))
                return "Invalid params: text content requires text";
        } else if (type == "image" || type == "audio") {
            if (!has_string(block, "data", true) || !has_string(block, "mimeType"))
                return "Invalid params: " + type + " content requires data and mimeType";
            if ((type == "image" && !caps.image) || (type == "audio" && !caps.audio))
                return "Invalid params: " + type + " content is not supported by this agent "
                       "(promptCapabilities." + type + " is false)";
        } else if (type == "resource_link") {
            if (!has_string(block, "uri") || !has_string(block, "name", true))
                return "Invalid params: resource_link content requires uri and name";
        } else if (type == "resource") {
            if (!block.contains("resource") || !block["resource"].is_object()
                || !has_string(block["resource"], "uri")
                || !(has_string(block["resource"], "text", true)
                     || has_string(block["resource"], "blob", true)))
                return "Invalid params: resource content requires a resource with uri and text or blob";
            if (!caps.embedded_context)
                return "Invalid params: embedded resource content is not supported by this agent "
                       "(promptCapabilities.embeddedContext is false)";
        } else {
            return "Invalid params: unknown content block type";
        }
    }
    return {};
}

/// Parse `mcpServers` (already known to be an array of objects). Returns the
/// error message, or empty.
std::string parse_mcp_servers(const neograph::json& servers,
                              std::vector<McpServerSpec>& out) {
    std::size_t index = 0;
    for (const auto& server : servers) {
        const auto where = "mcpServers[" + std::to_string(index++) + "]";
        if (!server.is_object()) return "Invalid params: " + where + " must be an object";
        if (server.contains("type") && !server["type"].is_null()) {
            if (!server["type"].is_string())
                return "Invalid params: " + where + ".type must be a string";
            const auto type = server["type"].get<std::string>();
            if (type == "http" || type == "sse") {
                return "Invalid params: " + where + " uses the " + type
                     + " transport, which this agent does not advertise "
                       "(agentCapabilities.mcpCapabilities." + type + " is false)";
            }
            if (type != "stdio")
                return "Invalid params: " + where + " has an unknown transport type";
        }
        McpServerSpec spec;
        if (!has_string(server, "name", true) || !has_string(server, "command"))
            return "Invalid params: " + where + " requires name and command";
        spec.name = server["name"].get<std::string>();
        spec.command = server["command"].get<std::string>();
        if (server.contains("args")) {
            if (!server["args"].is_array())
                return "Invalid params: " + where + ".args must be an array of strings";
            for (const auto& arg : server["args"]) {
                if (!arg.is_string())
                    return "Invalid params: " + where + ".args must be an array of strings";
                spec.args.push_back(arg.get<std::string>());
            }
        }
        if (server.contains("env")) {
            if (!server["env"].is_array())
                return "Invalid params: " + where + ".env must be an array";
            for (const auto& variable : server["env"]) {
                if (!variable.is_object() || !has_string(variable, "name")
                    || !has_string(variable, "value", true))
                    return "Invalid params: " + where + ".env entries require name and value";
                const auto name = variable["name"].get<std::string>();
                const auto value = variable["value"].get<std::string>();
                if (name.find('=') != std::string::npos || name.find('\0') != std::string::npos
                    || value.find('\0') != std::string::npos)
                    return "Invalid params: " + where + ".env contains an invalid entry";
                spec.env.emplace_back(variable["name"].get<std::string>(),
                                      variable["value"].get<std::string>());
            }
        }
        out.push_back(std::move(spec));
    }
    return {};
}

#ifdef NEOGRAPH_ACP_MCP
bool same_env_name(const std::string& a, const std::string& b) {
#if defined(_WIN32)
    return a.size() == b.size()
        && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x))
                   == std::tolower(static_cast<unsigned char>(y));
           });
#else
    return a == b;
#endif
}

/// The environment of a client-specified MCP server: the agent's PATH, so
/// the client's `command` resolves as it would in a shell, plus the client's
/// `env` entries. Nothing else of the agent's environment is passed on: it
/// may hold provider API keys, and the client names the server's command.
/// Windows also needs SystemRoot, without which a process cannot load its
/// system libraries; it holds a directory, never a secret.
std::vector<std::pair<std::string, std::string>>
mcp_server_environment(const std::vector<std::pair<std::string, std::string>>& overrides) {
    static const char* const kInherited[] = {
        "PATH",
#if defined(_WIN32)
        "SystemRoot",
#endif
    };
    std::vector<std::pair<std::string, std::string>> result;
#if defined(__APPLE__)
    char** entries = *_NSGetEnviron();
#elif defined(_WIN32)
    char** entries = _environ;
#else
    char** entries = environ;
#endif
    for (char** entry = entries; entry && *entry; ++entry) {
        std::string_view text(*entry);
        const auto eq = text.find('=');
        // Skip malformed entries and Windows' hidden "=C:" drive entries.
        if (eq == std::string_view::npos || eq == 0) continue;
        std::string key(text.substr(0, eq));
        const bool inherited = std::any_of(std::begin(kInherited), std::end(kInherited),
                                           [&](const char* name) { return same_env_name(name, key); });
        if (!inherited) continue;
        bool duplicate = false;
        for (const auto& [name, _] : result) {
            if (same_env_name(name, key)) { duplicate = true; break; }
        }
        if (!duplicate) result.emplace_back(std::move(key), std::string(text.substr(eq + 1)));
    }
    for (const auto& item : overrides) {
        bool duplicate = false;
        for (auto& [name, value] : result) {
            if (same_env_name(name, item.first)) {
                value = item.second;  // a client entry wins, a later one last
                duplicate = true;
                break;
            }
        }
        if (!duplicate) result.push_back(item);
    }
    return result;
}
#endif

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct ACPServer::Impl {
    neograph::graph::GraphExecution               execution;
    explicit Impl(neograph::graph::GraphExecution value)
        : execution(std::move(value)) {}
    neograph::json                                info;
    std::shared_ptr<ACPGraphAdapter>              adapter;
    AgentCapabilities                             caps;
    ClientCapabilities                            client_caps;
    std::atomic<bool>                             initialized{false};
    std::atomic<bool>                             initialize_seen{false};
    std::atomic<bool>                             stop_flag{false};
    /// Set by run() when its input reaches EOF: the client is gone, so a
    /// pending agent->client request can no longer be answered.
    std::atomic<bool>                             transport_closed{false};
    std::atomic<bool>                             pre_armed_cancel{false};
    std::atomic<std::int64_t>                     eof_grace_ms{1000};
    /// Toggled on by run() entry, off on exit. Mirrors
    /// A2AServer::is_running so a generic protocol-server supervisor
    /// can poll either uniformly.
    std::atomic<bool>                             running{false};

    /// Notification sink — published to atomically as a shared_ptr so
    /// readers (worker threads issuing emit()) can take a stable snapshot
    /// without holding a mutex. Writers (`run()`, `set_notification_sink`)
    /// store a fresh shared_ptr; the previous one is kept alive by any
    /// thread that already loaded it. Avoids the data race a bare
    /// `std::function` assignment would have during SBO reseating.
    std::shared_ptr<NotificationSink>             sink_;

    /// One client-supplied MCP server attached to a session.
    struct McpAttachment {
        std::string                                  server_name;
#ifdef NEOGRAPH_ACP_MCP
        std::shared_ptr<neograph::mcp::MCPClient>    client;
#endif
        std::vector<std::shared_ptr<neograph::Tool>> tools;
    };

    struct SessionState {
        std::string                cwd;
        std::vector<std::string>   additional_directories;
        std::vector<McpAttachment> mcp;
    };

    mutable std::mutex                            sessions_mu;
    std::map<std::string, SessionState>           sessions;
    /// Sessions restored by session/resume whose latest checkpoint is paused.
    /// The next prompt is consumed as the resume value exactly once instead of
    /// starting at entry.
    std::set<std::string>                         interrupted_sessions;
    /// An active prompt owns its token until its terminal response leaves the
    /// notification sink.
    std::map<std::string, std::shared_ptr<neograph::graph::CancelToken>>
                                                    active_cancel_tokens;
    /// Pre-armed cancels (opt-in, see set_pre_armed_cancel).
    std::set<std::string>                         pending_cancels;
    /// A terminal prompt response has been committed for these sessions. A
    /// cancel without an active prompt after this point is stale.
    std::set<std::string>                         terminal_prompt_sessions;

    /// Outbound JSON-RPC: id → promise that the run-loop reader fulfils
    /// when it sees a response with that id.
    std::atomic<std::int64_t>                     next_outbound_id{1};
    std::mutex                                    pending_mu;
    std::unordered_map<std::int64_t,
                       std::shared_ptr<std::promise<neograph::json>>>
                                                  pending;

    /// Worker bookkeeping for session/prompt dispatch.
    ///
    /// `max_inflight_prompts` caps total concurrent prompts across all
    /// sessions (default 32) — prevents a misbehaving / malicious peer
    /// that pipelines prompts faster than the engine completes from
    /// exhausting the host's thread quota and unbounded-growing the
    /// worker tracking. `inflight_sessions` enforces single-flight
    /// per session_id so two prompts on the same session don't race
    /// the engine on the same thread_id (which is what the engine's
    /// checkpoint store keys on).
    ///
    /// Workers are accounted via `inflight_count`
    /// (incremented before the worker launches, decremented + cv
    /// notified at the end of the worker body). Destructor waits on
    /// the cv until inflight_count drains to zero.
    std::mutex                                    workers_mu;
    std::condition_variable                       workers_cv;
    std::mutex                                    worker_threads_mu;
    using WorkerThread = std::pair<std::thread, std::future<void>>;
    std::vector<WorkerThread>                      worker_threads;
    int                                           inflight_count = 0;
    std::set<std::string>                         inflight_sessions;
    /// Sessions currently being restored from checkpoint. Prompt admission
    /// rejects these until the persisted interrupt state is installed.
    std::set<std::string>                         resuming_sessions;
    int                                           max_inflight_prompts = 32;
#if defined(NEOGRAPH_ACP_TESTING)
    std::atomic<bool>                             fail_next_worker_launch{false};
    std::atomic<bool>                             fail_next_handle_message{false};
#endif

    void retain_worker(std::thread& worker, std::future<void> exited) {
        // Reap finished threads on admission so a long-lived session retains
        // only its active workers. Use a separate lock: joining a thread may
        // still need workers_mu while its captured reservation is destroyed.
        std::lock_guard lock(worker_threads_mu);
        std::erase_if(worker_threads, [](auto& worker) {
            if (worker.second.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return false;
            worker.first.join();
            return true;
        });
        worker_threads.emplace_back(std::move(worker), std::move(exited));
    }

    void drain_workers() {
        {
            std::unique_lock lock(workers_mu);
            workers_cv.wait(lock, [this] { return inflight_count == 0; });
        }
        std::vector<WorkerThread> finished;
        {
            std::lock_guard lock(worker_threads_mu);
            finished.swap(worker_threads);
        }
        // Join before destroying the readiness futures: set_value_at_thread_exit
        // may still be releasing the promise's shared state in the thread tail.
        for (auto& worker : finished) worker.first.join();
    }

    void cancel_active_prompts() {
        std::vector<std::shared_ptr<neograph::graph::CancelToken>> tokens;
        {
            std::lock_guard lk(sessions_mu);
            for (const auto& [_, token] : active_cancel_tokens) tokens.push_back(token);
        }
        for (const auto& token : tokens) token->cancel();
    }

    struct WorkerReservation {
        Impl*       owner;
        std::string session_id;
        bool        active = false;
        bool        session_active = false;

        WorkerReservation(Impl* owner_in, std::string session_id_in)
            : owner(owner_in), session_id(std::move(session_id_in)) {}

        ~WorkerReservation() {
            if (!active) return;
            std::lock_guard lk(owner->workers_mu);
            --owner->inflight_count;
            if (session_active) owner->inflight_sessions.erase(session_id);
            // Keep notify under the lock. A waiter cannot destroy the cv
            // after seeing count==0 while this notify is still executing.
            owner->workers_cv.notify_all();
        }

        void release_session() {
            std::lock_guard lk(owner->workers_mu);
            if (!session_active) return;
            owner->inflight_sessions.erase(session_id);
            session_active = false;
        }
    };

    struct ResumeReservation {
        Impl*       owner;
        std::string session_id;
        bool        active = false;

        ResumeReservation(Impl* owner_in, std::string session_id_in)
            : owner(owner_in), session_id(std::move(session_id_in)) {}

        ~ResumeReservation() {
            if (!active) return;
            std::lock_guard lk(owner->workers_mu);
            owner->resuming_sessions.erase(session_id);
        }
    };

    /// Cached client handle returned by ACPServer::client().
    std::shared_ptr<ACPClient>                    client_handle;

    /// How one prompt turn ended.
    struct TurnResult {
        StopReason stop = StopReason::EndTurn;
        std::optional<neograph::graph::FailureVerdict> failure;
        /// The run stopped on a cancellation the client did not ask for
        /// (shutdown, a node that aborted): JSON-RPC -32800.
        bool        internal_cancel = false;
        std::string agent_text;
        std::string boundary_status = "completed";
        /// Model tokens already reached the client as agent_message_chunk
        /// updates, so the final text is not sent again.
        bool        streamed_text = false;
    };

    /// session/update notifications for one turn's graph events: model tokens
    /// as agent_message_chunk, tool calls as tool_call / tool_call_update.
    /// Runs inline on the engine's execution path, so it only formats and
    /// hands each update to the sink.
    struct TurnUpdates {
        Impl*                 owner;
        std::string           session_id;
        std::mutex            mutex;
        bool                  saw_token = false;
        std::set<std::string> announced_calls;

        void on_event(const neograph::graph::GraphEvent& event);
        void send(neograph::json update);
    };

    neograph::json handle_initialize(const neograph::json& params,
                                     const neograph::json& id);
    neograph::json handle_session_new(const neograph::json& params,
                                       const neograph::json& id);
    neograph::json handle_session_resume(const neograph::json& params,
                                          const neograph::json& id);
    void           handle_session_prompt(const neograph::json& params,
                                         const neograph::json& id,
                                         bool reply_expected);
    void           handle_session_cancel(const neograph::json& params);

    TurnResult run_turn(const PromptRequest& req,
                        const ACPSessionContext& session,
                        const std::shared_ptr<neograph::graph::CancelToken>& token,
                        bool resume_pending);

    /// Start the session's MCP servers. Returns an error envelope on failure
    /// (after shutting down any server it already started), else null.
    neograph::json attach_mcp_servers(const std::vector<McpServerSpec>& specs,
                                      const std::string& cwd,
                                      const neograph::json& id,
                                      std::vector<McpAttachment>& out);

    static void shutdown_mcp(std::vector<McpAttachment>& attachments) noexcept {
#ifdef NEOGRAPH_ACP_MCP
        for (auto& attachment : attachments) {
            if (attachment.client) attachment.client->shutdown();
        }
#endif
        attachments.clear();
    }

    neograph::json request_client(std::string method, neograph::json params,
                                  std::chrono::milliseconds timeout);

    void emit(const neograph::json& env) {
        // Take a stable snapshot of the current sink shared_ptr — even
        // if `set_notification_sink` swaps the pointer concurrently,
        // this thread keeps the old callable alive until it returns.
        auto s = std::atomic_load_explicit(&sink_, std::memory_order_acquire);
        if (s && *s) (*s)(env);
    }

    /// Report a host failure. The message is fixed: exception text from the
    /// host, the adapter or the transport never reaches the client.
    void emit_internal_error(const neograph::json& id) noexcept {
        try {
            emit(jsonrpc_error(kInternalError, "Internal error", id,
                               neograph::json{{"code", "NG.UnknownFailure"},
                                              {"retryable", false}}));
        } catch (...) {
            // A failing transport/sink cannot be used to report its own
            // failure, but it must never escape a prompt worker.
        }
    }
};

neograph::json
ACPServer::Impl::handle_initialize(const neograph::json& params,
                                   const neograph::json& id) {
    if (initialize_seen.load(std::memory_order_acquire)) {
        return jsonrpc_error(kInvalidRequest, "initialize may only be called once", id);
    }
    if (!require_object(params)
        || !params.contains("protocolVersion")
        || !params["protocolVersion"].is_number_integer()
        || params["protocolVersion"].get<std::int64_t>() < 0
        || params["protocolVersion"].get<std::int64_t>() > 65535
        || (params.contains("clientCapabilities")
            && !params["clientCapabilities"].is_object())
        || (params.contains("clientInfo")
            && !params["clientInfo"].is_object()
            && !params["clientInfo"].is_null())) {
        return jsonrpc_error(kInvalidParams, "Invalid params: malformed initialize request", id);
    }

    InitializeRequest req;
    try { from_json(params, req); }
    catch (const std::exception&) {
        return jsonrpc_error(kInvalidParams, "Invalid params: malformed initialize request", id);
    }
    bool expected = false;
    if (!initialize_seen.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return jsonrpc_error(kInvalidRequest, "initialize may only be called once", id);
    }
    client_caps = req.client_capabilities;

    InitializeResponse resp;
    // ACP v1 negotiation: return the client's version when supported,
    // otherwise return the latest version this agent supports (v1).
    resp.protocol_version = 1;
    resp.agent_capabilities = caps;
    resp.agent_info = info;

    initialized.store(true, std::memory_order_release);

    neograph::json rj;
    to_json(rj, resp);
    return jsonrpc_result(std::move(rj), id);
}

neograph::json
ACPServer::Impl::attach_mcp_servers(const std::vector<McpServerSpec>& specs,
                                    const std::string& cwd,
                                    const neograph::json& id,
                                    std::vector<McpAttachment>& out) {
    if (specs.empty()) return {};
#ifndef NEOGRAPH_ACP_MCP
    (void)cwd;
    (void)out;
    return jsonrpc_error(kInternalError,
                         "This agent was built without MCP client support", id,
                         neograph::json{{"reason", "mcp_unsupported"}});
#else
    for (std::size_t index = 0; index < specs.size(); ++index) {
        const auto& spec = specs[index];
        McpAttachment attachment;
        attachment.server_name = spec.name;
        bool connected = false;
        try {
            neograph::mcp::StdioClientConfig config;
            config.argv.push_back(spec.command);
            config.argv.insert(config.argv.end(), spec.args.begin(), spec.args.end());
            config.cwd = cwd;
            config.environment = mcp_server_environment(spec.env);
            config.replace_environment = true;
            // Do not treat server logging as an error (MCP stdio transport):
            // allow a generous stderr budget.
            config.max_stderr_bytes = 8u * 1024u * 1024u;
            config.startup_timeout = std::chrono::seconds(15);
            config.request_timeout = std::chrono::minutes(5);
            attachment.client = std::make_shared<neograph::mcp::MCPClient>(std::move(config));
            if (attachment.client->initialize("neograph-acp")) {
                for (auto& tool : attachment.client->get_tools()) {
                    attachment.tools.emplace_back(std::move(tool));
                }
                connected = true;
            }
        } catch (...) {
            connected = false;
        }
        if (!connected) {
            if (attachment.client) attachment.client->shutdown();
            shutdown_mcp(out);
            // The detail of the failure (the server's stderr, paths, the
            // transport error) stays local.
            return jsonrpc_error(kInternalError,
                                 "Could not start the MCP server mcpServers["
                                     + std::to_string(index) + "]",
                                 id,
                                 neograph::json{{"reason", "mcp_server_unavailable"},
                                                {"index", static_cast<int>(index)}});
        }
        out.push_back(std::move(attachment));
    }
    return {};
#endif
}

neograph::json
ACPServer::Impl::handle_session_new(const neograph::json& params,
                                    const neograph::json& id) {
    if (!initialized.load(std::memory_order_acquire)) {
        return jsonrpc_error(kInvalidRequest, "initialize must be called first", id);
    }
    if (!require_object(params)
        || !has_string(params, "cwd")
        || !has_array(params, "mcpServers")) {
        return jsonrpc_error(kInvalidParams, "Invalid params: session/new requires cwd and mcpServers", id);
    }
    if (params.contains("additionalDirectories")
        && !params["additionalDirectories"].is_array()) {
        return jsonrpc_error(kInvalidParams, "Invalid params: additionalDirectories must be an array", id);
    }
    for (const auto& path : params.value("additionalDirectories", neograph::json::array())) {
        if (!path.is_string() || !is_absolute_path(path.get<std::string>())) {
            return jsonrpc_error(kInvalidParams, "Invalid params: workspace paths must be absolute", id);
        }
    }
    const auto cwd = params["cwd"].get<std::string>();
    if (!is_absolute_path(cwd)) {
        return jsonrpc_error(kInvalidParams, "Invalid params: cwd must be absolute", id);
    }
    std::vector<McpServerSpec> specs;
    if (auto problem = parse_mcp_servers(params["mcpServers"], specs); !problem.empty()) {
        return jsonrpc_error(kInvalidParams, std::move(problem), id);
    }
    NewSessionRequest req;
    try { from_json(params, req); }
    catch (const std::exception&) {
        return jsonrpc_error(kInvalidParams, "Invalid params: malformed session/new request", id);
    }

    SessionState state;
    state.cwd = req.cwd;
    state.additional_directories = req.additional_directories;
    if (auto error = attach_mcp_servers(specs, state.cwd, id, state.mcp); !error.is_null()) {
        return error;
    }

    auto sid = fresh_session_id();
    {
        std::lock_guard lk(sessions_mu);
        sessions[sid] = std::move(state);
    }
    NewSessionResponse resp;
    resp.session_id = sid;
    neograph::json rj;
    to_json(rj, resp);
    return jsonrpc_result(std::move(rj), id);
}

neograph::json
ACPServer::Impl::handle_session_resume(const neograph::json& params,
                                       const neograph::json& id) {
    if (!initialized.load(std::memory_order_acquire)) {
        return jsonrpc_error(kInvalidRequest, "initialize must be called first", id);
    }
    if (!caps.session.resume) {
        return jsonrpc_error(kMethodNotFound, "session/resume is not supported", id);
    }
    if (!require_object(params)
        || !has_string(params, "sessionId")
        || !has_string(params, "cwd")
        || (params.contains("mcpServers") && !params["mcpServers"].is_array())
        || (params.contains("additionalDirectories")
            && !params["additionalDirectories"].is_array())) {
        return jsonrpc_error(kInvalidParams, "Invalid params: malformed session/resume request", id);
    }
    const auto cwd = params["cwd"].get<std::string>();
    if (!is_absolute_path(cwd)) {
        return jsonrpc_error(kInvalidParams, "Invalid params: cwd must be absolute", id);
    }
    for (const auto& path : params.value("additionalDirectories", neograph::json::array())) {
        if (!path.is_string() || !is_absolute_path(path.get<std::string>())) {
            return jsonrpc_error(kInvalidParams, "Invalid params: workspace paths must be absolute", id);
        }
    }
    std::vector<McpServerSpec> specs;
    if (params.contains("mcpServers")) {
        if (auto problem = parse_mcp_servers(params["mcpServers"], specs); !problem.empty()) {
            return jsonrpc_error(kInvalidParams, std::move(problem), id);
        }
    }
    ResumeSessionRequest req;
    try { from_json(params, req); }
    catch (const std::exception&) {
        return jsonrpc_error(kInvalidParams, "Invalid params: malformed session/resume request", id);
    }

    ResumeReservation reservation(this, req.session_id);
    {
        std::lock_guard lk(workers_mu);
        if (inflight_sessions.count(req.session_id) ||
            resuming_sessions.count(req.session_id)) {
            return busy_error("session " + req.session_id + " is busy with a prompt turn",
                              "session_busy", id);
        }
        resuming_sessions.insert(req.session_id);
        reservation.active = true;
    }

    bool known = false;
    {
        std::lock_guard lk(sessions_mu);
        auto existing = sessions.find(req.session_id);
        known = existing != sessions.end();
        if (known && existing->second.cwd != req.cwd) {
            return jsonrpc_error(
                kInvalidParams, "session/resume cwd does not match the existing session", id);
        }
    }
    // A session this process created never stays paused between turns (an
    // interrupt is answered inside its turn), so only a session from an
    // earlier process needs its persisted state. Reading it administers the
    // shared engine, which refuses while any other run is active.
    std::optional<neograph::graph::CheckpointPhase> phase;
    if (!known) {
        const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        for (;;) {
            try {
                phase = execution.latest_resume_phase(req.session_id);
                break;
            } catch (const std::logic_error&) {
                if (std::chrono::steady_clock::now() >= give_up) {
                    return busy_error("The agent is busy; retry session/resume shortly",
                                      "engine_busy", id);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            } catch (...) {
                const auto failure = graph::classify_failure(std::current_exception());
                return jsonrpc_error(kInternalError, "Failed to restore the session", id,
                                     neograph::json{{"code", failure.code},
                                                    {"retryable", failure.retryable}});
            }
        }
    }
    if (!phase && !known) {
        // ACP ErrorCode -32002 "Resource not found", as for session/prompt.
        return jsonrpc_error(kResourceNotFound, "Unknown session: " + req.session_id, id);
    }

    // session/resume carries the complete MCP server list for the session.
    SessionState state;
    state.cwd = req.cwd;
    state.additional_directories = req.additional_directories;
    if (auto error = attach_mcp_servers(specs, state.cwd, id, state.mcp); !error.is_null()) {
        return error;
    }

    std::vector<McpAttachment> replaced;
    {
        std::lock_guard lk(sessions_mu);
        auto& slot = sessions[req.session_id];
        replaced = std::move(slot.mcp);
        slot = std::move(state);
        if (phase && is_interrupt_phase(*phase)) {
            interrupted_sessions.insert(req.session_id);
        } else if (!known) {
            interrupted_sessions.erase(req.session_id);
        }
    }
    shutdown_mcp(replaced);

    // ACP v1 session/resume restores context without replaying session/update.
    return jsonrpc_result(neograph::json::object(), id);
}

ACPServer::Impl::TurnResult
ACPServer::Impl::run_turn(const PromptRequest& req,
                          const ACPSessionContext& session,
                          const std::shared_ptr<neograph::graph::CancelToken>& token,
                          bool resume_pending) {
    namespace graph = neograph::graph;
    TurnResult turn;
    auto& a = *adapter;
    auto budget_exhausted = std::make_shared<std::atomic_bool>(false);
    auto updates = std::make_shared<TurnUpdates>();
    updates->owner = this;
    updates->session_id = req.session_id;

    auto request_for = [&](neograph::json input, bool with_input) {
        graph::RunInvocationRequest request;
        request.config.thread_id = req.session_id;
        request.config.cancel_token = token;
        request.config.budget_exhausted = budget_exhausted;
        if (with_input) request.config.input = std::move(input);
        request.metadata.run_id = req.session_id;
        request.on_event = [updates](const graph::GraphEvent& event) {
            updates->on_event(event);
        };
        return request;
    };
    auto fail = [&](graph::FailureVerdict failure) {
        turn.failure = std::move(failure);
        turn.boundary_status = "error";
    };
    auto interrupted_failure = [] {
        graph::FailureVerdict failure;
        failure.failure_class = graph::NodeFailureClass::Interrupted;
        failure.code = graph::node_failure_code(failure.failure_class);
        return failure;
    };

    graph::InvocationResult outcome;
    try {
        if (resume_pending) {
            // A pause restored by session/resume: the prompt answers it.
            graph::RunInvocation invocation(execution, request_for({}, false));
            outcome = invocation.resume(neograph::json(a.extract_user_text(req.prompt)));
        } else {
            auto input = a.build_session_state(req.prompt, session);
            graph::RunInvocation invocation(execution, request_for(std::move(input), true));
            outcome = invocation.run();
        }
    } catch (...) {
        fail(graph::classify_failure(std::current_exception(), token.get()));
        return turn;
    }

    for (;;) {
        turn.boundary_status = std::string(graph::to_string(outcome.status));
        if (outcome.cancelled() && token->is_cancelled()) {
            turn.stop = StopReason::Cancelled;
            break;
        }
        if (!outcome.succeeded() || !outcome.run_result) {
            // An Error outcome, or a cancellation the client did not ask for
            // (a node that aborted), carries its classified failure.
            fail(outcome.failure.value_or(graph::classify_failure(nullptr)));
            break;
        }
        if (outcome.status == graph::InvocationStatus::MaxSteps) {
            // A node that crosses the run's model-token budget stops the run
            // at the next super-step boundary, like a step limit.
            turn.stop = budget_exhausted->load(std::memory_order_acquire)
                ? StopReason::MaxTokens : StopReason::MaxTurnRequests;
            break;
        }
        const auto& result = *outcome.run_result;
        if (!result.interrupted) {
            try {
                turn.agent_text = a.extract_agent_text(result.output);
            } catch (...) {
                fail(graph::classify_failure(std::current_exception(), token.get()));
            }
            break;
        }

        // A NodeInterrupt: ask the user through session/request_permission in
        // this same turn and resume the run with the answer.
        ACPInterrupt interrupt;
        interrupt.session_id = req.session_id;
        interrupt.node = result.interrupt_node;
        interrupt.checkpoint_id = result.checkpoint_id;
        const auto& value = result.interrupt_value;
        interrupt.reason = value.is_object()
            ? value.value("reason", value.value("message", std::string()))
            : std::string();
        if (value.is_object() && value.contains("value")) interrupt.payload = value["value"];

        std::optional<ACPInterruptPermission> permission;
        try {
            permission = a.interrupt_permission(interrupt);
        } catch (...) {
            fail(graph::classify_failure(std::current_exception(), token.get()));
            break;
        }
        if (!permission) {
            fail(interrupted_failure());
            break;
        }
        RequestPermissionRequest ask;
        ask.session_id = req.session_id;
        ask.tool_call = permission->tool_call;
        ask.options = permission->options;
        neograph::json ask_json;
        to_json(ask_json, ask);
        RequestPermissionResponse answer;
        bool answered = false;
        bool cancelled = false;
        try {
            auto reply = request_client("session/request_permission", std::move(ask_json),
                                        std::chrono::milliseconds::max());
            from_json(reply, answer);
            answered = true;
        } catch (const graph::CancelledException&) {
            cancelled = true;
        } catch (...) {
            cancelled = token->is_cancelled();
        }
        if (cancelled || (answered && answer.outcome.kind == PermissionOutcomeKind::Cancelled)) {
            turn.stop = StopReason::Cancelled;
            break;
        }
        if (!answered) {
            // The client could not answer (error reply, transport gone).
            fail(interrupted_failure());
            break;
        }
        try {
            auto resume_value = a.interrupt_resume_value(interrupt, answer.outcome.option_id);
            graph::RunInvocation invocation(execution, request_for({}, false));
            // Resume the exact checkpoint the run paused at.
            outcome = interrupt.checkpoint_id.empty()
                ? invocation.resume(std::move(resume_value))
                : invocation.resume_from(interrupt.checkpoint_id, std::move(resume_value));
        } catch (...) {
            fail(graph::classify_failure(std::current_exception(), token.get()));
            break;
        }
    }

    {
        std::lock_guard lock(updates->mutex);
        turn.streamed_text = updates->saw_token;
    }
    if (turn.failure) {
        using graph::NodeFailureClass;
        switch (turn.failure->failure_class) {
        case NodeFailureClass::SafetyBlocked:
            // A provider safety block is ACP's `refusal` stop reason.
            turn.stop = StopReason::Refusal;
            turn.failure.reset();
            break;
        case NodeFailureClass::BudgetExhausted:
            if (budget_exhausted->load(std::memory_order_acquire)) {
                // The run's model-token budget: ACP's `max_tokens`.
                turn.stop = StopReason::MaxTokens;
                turn.failure.reset();
            }
            break;
        case NodeFailureClass::Cancelled:
            if (!token->is_cancelled()) turn.internal_cancel = true;
            break;
        default:
            break;
        }
    }
    return turn;
}

void ACPServer::Impl::TurnUpdates::send(neograph::json update) {
    neograph::json params{{"sessionId", session_id}, {"update", std::move(update)}};
    owner->emit(jsonrpc_notify("session/update", std::move(params)));
}

void ACPServer::Impl::TurnUpdates::on_event(const neograph::graph::GraphEvent& event) {
    using Type = neograph::graph::GraphEvent::Type;
    try {
        if (event.type == Type::LLM_TOKEN) {
            if (!event.data.is_string()) return;
            auto token = event.data.get<std::string>();
            if (token.empty()) return;
            std::lock_guard lock(mutex);
            saw_token = true;
            send(neograph::json{{"sessionUpdate", "agent_message_chunk"},
                                {"content", {{"type", "text"}, {"text", std::move(token)}}}});
            return;
        }
        if (event.type != Type::CHANNEL_WRITE || event.node_name == "__state__") return;
        if (!event.data.is_object() || event.data.value("channel", std::string()) != "messages")
            return;
        auto value = event.data["value"];
        auto messages = neograph::json::array();
        if (value.is_array()) messages = value;
        else if (value.is_object()) messages.push_back(value);
        std::lock_guard lock(mutex);
        for (const auto message : messages) {
            if (!message.is_object()) continue;
            const auto role = message.value("role", std::string());
            if (role == "assistant" && message.contains("tool_calls")
                && message["tool_calls"].is_array()) {
                for (const auto call : message["tool_calls"]) {
                    if (!call.is_object()) continue;
                    const auto id = call.value("id", std::string());
                    if (id.empty() || !announced_calls.insert(id).second) continue;
                    neograph::json update{{"sessionUpdate", "tool_call"},
                                          {"toolCallId", id},
                                          {"title", call.value("name", std::string("tool"))},
                                          {"kind", "other"},
                                          {"status", "in_progress"}};
                    const auto arguments = call["arguments"];
                    if (arguments.is_string()) {
                        try {
                            update["rawInput"] =
                                neograph::json::parse(arguments.get<std::string>());
                        } catch (...) {
                            update["rawInput"] = arguments;
                        }
                    } else if (!arguments.is_null()) {
                        update["rawInput"] = arguments;
                    }
                    send(std::move(update));
                }
            } else if (role == "tool") {
                const auto id = message.value("tool_call_id", std::string());
                if (id.empty()) continue;
                const auto status = message.value("tool_status", std::string("succeeded"));
                const bool succeeded = status == "succeeded";
                neograph::json update{{"toolCallId", id},
                                      {"status", succeeded ? "completed" : "failed"}};
                if (announced_calls.insert(id).second) {
                    update["sessionUpdate"] = "tool_call";
                    update["title"] = message.value("tool_name", std::string("tool"));
                    update["kind"] = "other";
                } else {
                    update["sessionUpdate"] = "tool_call_update";
                }
                // Only a successful result is shown: a failed tool message
                // carries the tool's error text, which stays local.
                if (succeeded && message.contains("content")) {
                    const auto content = message["content"];
                    update["content"] = neograph::json::array({neograph::json{
                        {"type", "content"},
                        {"content", {{"type", "text"},
                                     {"text", content.is_string()
                                                  ? content.get<std::string>()
                                                  : content.dump()}}}}});
                }
                send(std::move(update));
            }
        }
    } catch (...) {
        // Progress updates are best effort; the terminal response reports
        // the outcome of the turn.
    }
}

void
ACPServer::Impl::handle_session_prompt(const neograph::json& params,
                                       const neograph::json& id,
                                       bool reply_expected) {
    if (!initialized.load(std::memory_order_acquire)) {
        if (reply_expected) emit(jsonrpc_error(kInvalidRequest, "initialize must be called first", id));
        return;
    }
    if (!require_object(params)
        || !has_string(params, "sessionId")
        || !has_array(params, "prompt")) {
        if (reply_expected) {
            emit(jsonrpc_error(kInvalidParams, "Invalid params: session/prompt requires sessionId and prompt", id));
        }
        return;
    }
    if (auto problem = validate_prompt_blocks(params["prompt"], caps.prompt); !problem.empty()) {
        if (reply_expected) emit(jsonrpc_error(kInvalidParams, std::move(problem), id));
        return;
    }
    PromptRequest req;
    try { from_json(params, req); }
    catch (const std::exception&) {
        if (reply_expected) {
            emit(jsonrpc_error(kInvalidParams, "Invalid params: malformed session/prompt request", id));
        }
        return;
    }
    {
        std::lock_guard lk(workers_mu);
        if (resuming_sessions.contains(req.session_id)) {
            if (reply_expected) {
                emit(busy_error("session " + req.session_id + " is being resumed",
                                "session_busy", id));
            }
            return;
        }
    }
    ACPSessionContext session;
    session.session_id = req.session_id;
    {
        std::lock_guard lk(sessions_mu);
        auto it = sessions.find(req.session_id);
        if (it == sessions.end()) {
            if (reply_expected) {
                emit(jsonrpc_error(kResourceNotFound, "Unknown session: " + req.session_id, id));
            }
            return;
        }
        session.cwd = it->second.cwd;
        session.additional_directories = it->second.additional_directories;
        for (const auto& attachment : it->second.mcp) {
            for (const auto& tool : attachment.tools) {
                session.mcp_tools.push_back(neograph::json{
                    {"server", attachment.server_name}, {"name", tool->get_name()}});
            }
        }
    }

    // Backpressure: refuse new prompts when the in-flight cap is hit
    // or the same session_id is already executing/restoring. Reserve the
    // session before reading its interrupt state so session/resume cannot
    // install that state between the read and worker admission. Without these
    // guards a misbehaving peer that pipelines prompts faster than
    // the engine completes would (a) exhaust the host's thread quota
    // and unbounded-grow the worker tracking, or (b) race two prompts
    // on the same session_id, which the engine's checkpoint store
    // (keyed by thread_id) cannot disambiguate.
    neograph::json rejection;
    auto reservation = std::make_shared<WorkerReservation>(
        this, req.session_id);
    {
        std::lock_guard lk(workers_mu);
        if (inflight_count >= max_inflight_prompts) {
            rejection = busy_error(
                "ACP agent is at its limit of " + std::to_string(max_inflight_prompts)
                    + " concurrent prompt turns; retry shortly",
                "overloaded", id);
        } else if (inflight_sessions.count(req.session_id) ||
                   resuming_sessions.count(req.session_id)) {
            rejection = busy_error(
                "session " + req.session_id
                    + " already has a prompt turn in progress",
                "session_busy", id);
        } else {
            inflight_sessions.insert(req.session_id);
            ++inflight_count;
            reservation->active = true;
            reservation->session_active = true;
        }
    }
    if (!rejection.is_null()) {
        if (reply_expected) emit(rejection);
        return;
    }

    std::shared_ptr<neograph::graph::CancelToken> task_cancel;
    bool pre_cancelled = false;
    bool resume_pending = false;
    {
        std::lock_guard lk(sessions_mu);
        task_cancel = std::make_shared<neograph::graph::CancelToken>();
        active_cancel_tokens[req.session_id] = task_cancel;
        pre_cancelled = pending_cancels.erase(req.session_id) != 0;
        terminal_prompt_sessions.erase(req.session_id);
        resume_pending = interrupted_sessions.count(req.session_id) != 0;
    }
    if (pre_cancelled) task_cancel->cancel();

    // Run the engine on a worker so the run-loop reader can
    // keep pumping inbound messages — including responses to fs/*
    // requests the engine issues mid-run via ACPClient::call_client.
    // Retain its thread handle so drain also joins thread-local cleanup.
    try {
#if defined(NEOGRAPH_ACP_TESTING)
        if (fail_next_worker_launch.exchange(false, std::memory_order_acq_rel)) {
            throw std::system_error(
                std::make_error_code(std::errc::resource_unavailable_try_again),
                "injected ACP worker launch failure");
        }
#endif
        std::promise<void> exited;
        auto exit_future = exited.get_future();
        std::thread worker(
            [this, req = std::move(req), session = std::move(session), id, task_cancel,
              worker_reservation = reservation, exited = std::move(exited), resume_pending,
              reply_expected]() mutable {
                exited.set_value_at_thread_exit();
                // Release accounting at body exit, before thread-local cleanup.
                auto reservation = std::move(worker_reservation);
                bool response_attempted = false;
                bool terminal_committed = false;
                auto cleanup = [&] {
                    reservation->release_session();
                    std::lock_guard lk(sessions_mu);
                    auto it = active_cancel_tokens.find(req.session_id);
                    if (it != active_cancel_tokens.end()
                        && it->second == task_cancel) {
                        active_cancel_tokens.erase(it);
                    }
                    if (!terminal_committed) {
                        // Error paths still complete this prompt turn. Mark it
                        // terminal so a late cancel cannot cancel the next one.
                        terminal_prompt_sessions.insert(req.session_id);
                        interrupted_sessions.erase(req.session_id);
                    }
                };
                auto commit_terminal = [&](StopReason& stop) {
                    std::lock_guard lk(sessions_mu);
                    // This lock is the cancel-vs-terminal-response
                    // linearization point. A cancel that wins here changes the
                    // sole terminal response; one that arrives after is stale.
                    if (task_cancel->is_cancelled()) {
                        stop = StopReason::Cancelled;
                    }
                    // A turn never leaves its session paused: an interrupt is
                    // answered inside the turn, and a turn that ends otherwise
                    // starts the next prompt afresh.
                    interrupted_sessions.erase(req.session_id);
                    auto it = active_cancel_tokens.find(req.session_id);
                    if (it != active_cancel_tokens.end()
                        && it->second == task_cancel) {
                        active_cancel_tokens.erase(it);
                    }
                    terminal_prompt_sessions.insert(req.session_id);
                    terminal_committed = true;
                };

                try {
                    auto turn = run_turn(req, session, task_cancel, resume_pending);
                    if (task_cancel->is_cancelled()) {
                        // Cancellation wins a concurrent failure so the client
                        // observes one terminal cancelled response.
                        turn.stop = StopReason::Cancelled;
                        turn.failure.reset();
                        turn.internal_cancel = false;
                    }

                    if (!turn.failure && turn.stop == StopReason::EndTurn
                        && !turn.agent_text.empty() && !turn.streamed_text) {
                        SessionNotification n;
                        n.session_id = req.session_id;
                        n.update.session_update = "agent_message_chunk";
                        n.update.content = ContentBlock::text_block(turn.agent_text);
                        neograph::json nj;
                        to_json(nj, n);
                        emit(jsonrpc_notify("session/update", std::move(nj)));
                    }

                    // Notifications may synchronously block in an embedder's
                    // sink. Recheck cancellation only after all pending updates
                    // have been emitted, then commit the one terminal response.
                    commit_terminal(turn.stop);
                    if (turn.stop == StopReason::Cancelled) {
                        turn.failure.reset();
                        turn.internal_cancel = false;
                        turn.boundary_status = "cancelled";
                    }

                    // The terminal state is now committed and the old cancel
                    // token is no longer reachable. Admit the next turn before
                    // publishing the response so a client awakened by the sink
                    // cannot observe a stale single-flight reservation.
                    reservation->release_session();

                    if (reply_expected) {
                        response_attempted = true;
                        if (turn.failure) {
                            // A failed turn is a JSON-RPC error: ACP's stop
                            // reasons all describe a turn that ended normally.
                            // Only typed public fields go on the wire
                            // (reference, "Protocol failure mapping").
                            const auto& failure = *turn.failure;
                            neograph::json data{{"code", failure.code},
                                                {"retryable", failure.retryable}};
                            if (turn.internal_cancel) {
                                emit(jsonrpc_error(kRequestCancelled, "Request cancelled", id,
                                                   std::move(data)));
                            } else {
                                emit(jsonrpc_error(kInternalError, ::neograph::detail::public_failure_message(failure), id,
                                                   std::move(data)));
                            }
                        } else {
                            PromptResponse resp;
                            resp.stop_reason = turn.stop;
                            neograph::json rj;
                            to_json(rj, resp);
                            rj["_meta"]["neograph/invocation_status"] = turn.boundary_status;
                            emit(jsonrpc_result(std::move(rj), id));
                        }
                    }
                    cleanup();
                } catch (...) {
                    cleanup();
                    if (reply_expected && !response_attempted) emit_internal_error(id);
                }
                // returns. Its destructor cleans every worker exit path.
            });

        try {
            retain_worker(worker, std::move(exit_future));
        } catch (...) {
            if (worker.joinable()) worker.join();
            throw;
        }
        reservation.reset();
    } catch (...) {
        // If worker launch fails, no worker owns the reservation.
        reservation.reset();
        {
            std::lock_guard lk(sessions_mu);
            auto it = active_cancel_tokens.find(req.session_id);
            if (it != active_cancel_tokens.end() && it->second == task_cancel) {
                active_cancel_tokens.erase(it);
            }
        }
        if (reply_expected) emit_internal_error(id);
    }
}

void
ACPServer::Impl::handle_session_cancel(const neograph::json& params) {
    CancelNotification n;
    try { from_json(params, n); }
    catch (...) { return; }

    std::shared_ptr<neograph::graph::CancelToken> token;
    {
        std::lock_guard lk(sessions_mu);
        auto it = active_cancel_tokens.find(n.session_id);
        if (it != active_cancel_tokens.end() && it->second) {
            token = it->second;
        } else if (pre_armed_cancel.load(std::memory_order_acquire)
                   && sessions.contains(n.session_id)
                   && !terminal_prompt_sessions.contains(n.session_id)) {
            // Opt-in only: a transport that may deliver a cancel ahead of the
            // prompt it targets.
            pending_cancels.insert(n.session_id);
        }
        // Otherwise there is no ongoing turn to cancel; the cancel is stale.
    }
    if (token) token->cancel();
}

neograph::json
ACPServer::Impl::request_client(std::string method, neograph::json params,
                                std::chrono::milliseconds timeout) {
    if (!initialized.load(std::memory_order_acquire)) {
        throw std::runtime_error(
            "ACPServer::call_client: initialize must complete before client requests");
    }
    if (method == "fs/read_text_file" && !client_caps.fs.read_text_file) {
        throw std::runtime_error(
            "ACPServer::call_client: client did not advertise fs.readTextFile");
    }
    if (method == "fs/write_text_file" && !client_caps.fs.write_text_file) {
        throw std::runtime_error(
            "ACPServer::call_client: client did not advertise fs.writeTextFile");
    }
    if (method.rfind("terminal/", 0) == 0 && !client_caps.terminal) {
        throw std::runtime_error(
            "ACPServer::call_client: client did not advertise terminal");
    }
    if (method.rfind("elicitation/", 0) == 0 && !client_caps.elicitation) {
        throw std::runtime_error(
            "ACPServer::call_client: client did not advertise elicitation");
    }
    auto s_snap = std::atomic_load_explicit(&sink_, std::memory_order_acquire);
    if (!s_snap || !*s_snap) {
        throw std::runtime_error(
            "ACPServer::call_client: no transport connected — call run() first "
            "or set_notification_sink() before issuing client requests");
    }

    // The prompt turn this request belongs to, when params name a session
    // with a turn in flight: cancelling the turn ends the wait.
    std::shared_ptr<neograph::graph::CancelToken> token;
    if (params.is_object() && params.contains("sessionId")
        && params["sessionId"].is_string()) {
        const auto session_id = params["sessionId"].get<std::string>();
        std::lock_guard lk(sessions_mu);
        if (auto it = active_cancel_tokens.find(session_id);
            it != active_cancel_tokens.end()) {
            token = it->second;
        }
    }

    auto id = next_outbound_id.fetch_add(1, std::memory_order_relaxed);
    neograph::json env;
    env["jsonrpc"] = "2.0";
    env["id"]      = id;
    env["method"]  = method;
    env["params"]  = std::move(params);

    auto promise = std::make_shared<std::promise<neograph::json>>();
    auto fut     = promise->get_future();
    {
        std::lock_guard lk(pending_mu);
        pending[id] = promise;
    }
    auto forget = [&] {
        std::lock_guard lk(pending_mu);
        pending.erase(id);
    };

    try {
        emit(env);
    } catch (...) {
        forget();
        throw;
    }

    using clock = std::chrono::steady_clock;
    const bool unbounded = timeout == std::chrono::milliseconds::max();
    const auto start = clock::now();
    std::optional<clock::time_point> abandon_at;
    for (;;) {
        auto slice = std::chrono::milliseconds(10);
        if (!unbounded) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                start + timeout - clock::now());
            slice = std::clamp(left, std::chrono::milliseconds(0), slice);
        }
        if (fut.wait_for(slice) == std::future_status::ready) break;
        const auto now = clock::now();
        const bool closing = stop_flag.load(std::memory_order_acquire)
                          || transport_closed.load(std::memory_order_acquire);
        if (closing || (token && token->is_cancelled())) {
            // ACP requires a client to answer pending permission requests with
            // `cancelled` once it cancels the turn; give it a moment to do so
            // before abandoning the request.
            if (!abandon_at) {
                abandon_at = now + (closing ? std::chrono::milliseconds(0)
                                            : kClientRequestCancelGrace);
            }
            if (now >= *abandon_at) {
                forget();
                if (!closing) {
                    try {
                        emit(jsonrpc_notify("$/cancel_request",
                                            neograph::json{{"requestId", id}}));
                    } catch (...) {
                    }
                }
                throw neograph::graph::CancelledException(
                    "ACP client request abandoned: the prompt turn was cancelled");
            }
            continue;
        }
        if (!unbounded && now - start >= timeout) {
            forget();
            throw std::runtime_error("ACPServer::call_client: timeout waiting for " + method);
        }
    }
    auto resp = fut.get();
    if (resp.contains("error")) {
        throw std::runtime_error(
            "ACPServer::call_client: error response: "
            + resp["error"].dump());
    }
    // A JSON null result (seen from some clients for fs/write_text_file) is
    // tolerated as an empty object.
    if (!resp.contains("result") || resp["result"].is_null()) return neograph::json::object();
    return resp["result"];
}

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------
ACPServer::ACPServer(std::shared_ptr<neograph::graph::GraphEngine> engine,
                     neograph::json info,
                     std::shared_ptr<ACPGraphAdapter> adapter)
    : ACPServer(neograph::graph::GraphExecution(std::move(engine)),
                std::move(info), std::move(adapter)) {}

ACPServer::ACPServer(neograph::graph::GraphExecution execution,
                     neograph::json info,
                     std::shared_ptr<ACPGraphAdapter> adapter)
    : impl_(std::make_unique<Impl>(std::move(execution))) {
    impl_->info    = normalize_agent_info(info);
    impl_->adapter = adapter ? adapter : std::make_shared<ACPGraphAdapter>();
    impl_->caps.session.resume = true;
}

ACPServer::~ACPServer() {
    // Make sure no in-flight worker is mid-engine-run when the server
    // dies — that would dereference a freed engine pointer through the
    // captured shared_ptr (which is safe) but would also try to write to
    // an output stream that's about to disappear. Drain reservations and
    // join the workers, including their captured and thread-local state.
    // Workers may finish while this destructor drains. Drop their
    // late notifications instead of invoking a sink owned by the caller.
    std::atomic_store_explicit(
        &impl_->sink_, std::shared_ptr<NotificationSink>{}, std::memory_order_release);
    impl_->stop_flag.store(true, std::memory_order_release);
    impl_->cancel_active_prompts();
    impl_->drain_workers();

    // Wake any pending outbound RPC waiters with an error so they don't
    // hang the caller forever.
    {
        std::lock_guard lk(impl_->pending_mu);
        for (auto& kv : impl_->pending) {
            try {
                neograph::json err;
                err["error"] = neograph::json{
                    {"code",    kInternalError},
                    {"message", "ACPServer torn down before response"}};
                kv.second->set_value(std::move(err));
            } catch (...) {}
        }
        impl_->pending.clear();
    }

    // Stop the client-supplied MCP servers.
    std::vector<Impl::McpAttachment> attachments;
    {
        std::lock_guard lk(impl_->sessions_mu);
        for (auto& [_, state] : impl_->sessions) {
            for (auto& attachment : state.mcp) attachments.push_back(std::move(attachment));
            state.mcp.clear();
        }
    }
    Impl::shutdown_mcp(attachments);
}

AgentCapabilities&       ACPServer::capabilities()       { return impl_->caps; }
const AgentCapabilities& ACPServer::capabilities() const { return impl_->caps; }

void ACPServer::set_notification_sink(NotificationSink sink) {
    auto s = std::make_shared<NotificationSink>(std::move(sink));
    std::atomic_store_explicit(&impl_->sink_, std::move(s),
                               std::memory_order_release);
}

#if defined(NEOGRAPH_ACP_TESTING)
void ACPServer::fail_next_worker_launch_for_testing() {
    impl_->fail_next_worker_launch.store(true, std::memory_order_release);
}

void ACPServer::fail_next_handle_message_for_testing() {
    impl_->fail_next_handle_message.store(true, std::memory_order_release);
}
#endif

void ACPServer::stop() {
    impl_->stop_flag.store(true, std::memory_order_release);
}
void ACPServer::set_max_inflight_prompts(std::size_t limit) {
    if (limit == 0 || limit > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("ACP prompt limit must be a positive int");
    std::lock_guard lock(impl_->workers_mu);
    impl_->max_inflight_prompts = static_cast<int>(limit);
}

void ACPServer::set_pre_armed_cancel(bool enabled) {
    impl_->pre_armed_cancel.store(enabled, std::memory_order_release);
}

void ACPServer::set_eof_grace(std::chrono::milliseconds grace) {
    if (grace.count() < 0) throw std::invalid_argument("ACP EOF grace must not be negative");
    impl_->eof_grace_ms.store(grace.count(), std::memory_order_release);
}

std::vector<std::shared_ptr<neograph::Tool>>
ACPServer::session_tools(std::string_view session_id) const {
    std::vector<std::shared_ptr<neograph::Tool>> tools;
    std::lock_guard lk(impl_->sessions_mu);
    auto it = impl_->sessions.find(std::string(session_id));
    if (it == impl_->sessions.end()) return tools;
    for (const auto& attachment : it->second.mcp) {
        tools.insert(tools.end(), attachment.tools.begin(), attachment.tools.end());
    }
    return tools;
}

bool ACPServer::is_running() const {
    return impl_->running.load(std::memory_order_acquire);
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
    return impl_->request_client(std::move(method), std::move(params), timeout);
}

neograph::json
ACPServer::handle_message(const neograph::json& env) {
#if defined(NEOGRAPH_ACP_TESTING)
    if (impl_->fail_next_handle_message.exchange(false,
                                                  std::memory_order_acq_rel)) {
        throw std::runtime_error("injected ACP handle_message failure");
    }
#endif
    const auto invalid_request_id =
        env.is_object() && env.contains("id") && valid_request_id(env["id"])
            ? env["id"] : neograph::json();
    if (!env.is_object()
        || !env.contains("jsonrpc")
        || env["jsonrpc"] != "2.0") {
        return jsonrpc_error(kInvalidRequest, "Invalid Request", invalid_request_id);
    }
    const bool has_method = env.contains("method");

    // Response to one of our outbound requests — fulfil the promise and emit
    // no reply. Outbound IDs are integers; inbound request IDs may also be
    // strings and are echoed by the normal request handlers below.
    if (!has_method) {
        if (!env.contains("id") || !valid_request_id(env["id"])
            || (env.contains("result") == env.contains("error"))
            || (env.contains("error") && !env["error"].is_object())) {
            return jsonrpc_error(kInvalidRequest, "Invalid Request", invalid_request_id);
        }
        if (env["id"].is_number_integer()) {
            const auto numeric_id = env["id"].get<std::int64_t>();
            std::shared_ptr<std::promise<neograph::json>> p;
            {
                std::lock_guard lk(impl_->pending_mu);
                auto it = impl_->pending.find(numeric_id);
                if (it != impl_->pending.end()) {
                    p = it->second;
                    impl_->pending.erase(it);
                }
            }
            if (p) p->set_value(env);
        }
        return {};
    }

    if (!env["method"].is_string()
        || (env.contains("id") && !valid_request_id(env["id"]))) {
        return jsonrpc_error(kInvalidRequest, "Invalid Request", invalid_request_id);
    }
    const auto method = env["method"].get<std::string>();
    const auto params = env.contains("params") ? env["params"] : neograph::json::object();
    const auto id = env.contains("id") ? env["id"] : neograph::json();
    const bool is_notification = !env.contains("id");

    // session/cancel is defined as a notification, never a request.
    if (method == "session/cancel" && !is_notification) {
        return jsonrpc_error(kInvalidRequest, "session/cancel must be a notification", id);
    }
    if (method == "initialize") {
        auto response = impl_->handle_initialize(params, id);
        return is_notification ? neograph::json() : response;
    }
    if (method == "session/new") {
        auto response = impl_->handle_session_new(params, id);
        return is_notification ? neograph::json() : response;
    }
    if (method == "session/resume") {
        auto response = impl_->handle_session_resume(params, id);
        return is_notification ? neograph::json() : response;
    }
    if (method == "session/prompt") {
        // Async dispatch — the worker emits a response only for a request.
        impl_->handle_session_prompt(params, id, !is_notification);
        return {};
    }
    if (method == "session/cancel") {
        impl_->handle_session_cancel(params);
        return {};
    }

    if (is_notification) return {};
    return jsonrpc_error(kMethodNotFound, "Method not found: " + method, id);
}

void ACPServer::run(std::istream& in, std::ostream& out) {
    impl_->running.store(true, std::memory_order_release);
    impl_->transport_closed.store(false, std::memory_order_release);
    // Always clear `running` on exit (including via exception), so
    // is_running() never reports a stale `true`.
    struct RunningGuard {
        std::atomic<bool>* flag;
        ~RunningGuard() {
            if (flag) flag->store(false, std::memory_order_release);
        }
    };
    RunningGuard running_guard{&impl_->running};

    // Established before allocating/installing the run sink so every exit,
    // including stream and dispatch exceptions, drains workers before the
    // sink's captured output pointer can become invalid.
    struct RunCleanupGuard {
        Impl* impl;
        ~RunCleanupGuard() {
            impl->drain_workers();
            std::atomic_store_explicit(
                &impl->sink_, std::shared_ptr<NotificationSink>{},
                std::memory_order_release);
        }
    };
    RunCleanupGuard cleanup_guard{impl_.get()};

    auto out_mu = std::make_shared<std::mutex>();
    auto out_ptr = &out;

    auto run_sink = std::make_shared<NotificationSink>(
        [out_ptr, out_mu](const neograph::json& env) {
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

        neograph::json resp;
        try {
            resp = handle_message(env);
        } catch (...) {
            // Host failures are reported without their exception text.
            auto request_id = env.is_object() && env.contains("id")
                                && valid_request_id(env["id"])
                                ? env["id"] : neograph::json();
            resp = jsonrpc_error(kInternalError, "Internal error", request_id);
        }
        if (!resp.is_null()) {
            std::lock_guard lk(*out_mu);
            out << resp.dump() << '\n';
            out.flush();
        }
    }

    if (!impl_->stop_flag.load(std::memory_order_acquire)) {
        // EOF: the client closed the connection, so nobody can answer an
        // agent->client request or read a response. Give in-flight turns a
        // short grace to finish, then cancel them so run() returns promptly.
        impl_->transport_closed.store(true, std::memory_order_release);
        {
            std::unique_lock lock(impl_->workers_mu);
            impl_->workers_cv.wait_for(
                lock, std::chrono::milliseconds(impl_->eof_grace_ms.load(std::memory_order_acquire)),
                [this] { return impl_->inflight_count == 0; });
        }
        impl_->cancel_active_prompts();
    }
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
void ACPClient::set_permission_timeout(std::optional<std::chrono::milliseconds> t) {
    permission_timeout_ = t;
}

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
                              const ToolCallUpdate& tool_call,
                              const std::vector<PermissionOption>& options) {
    if (!server_) throw std::runtime_error(
        "ACPClient::request_permission: not bound to a server (call ACPServer::attach_client)");

    RequestPermissionRequest req;
    req.session_id = std::string(session_id);
    req.tool_call  = tool_call;
    req.options    = options;

    neograph::json params; to_json(params, req);
    neograph::json result;
    try {
        result = server_->call_client(
            "session/request_permission", std::move(params),
            permission_timeout_.value_or(std::chrono::milliseconds::max()));
    } catch (const neograph::graph::CancelledException&) {
        // The prompt turn was cancelled while the user was being asked.
        return RequestPermissionOutcome{};
    }

    RequestPermissionResponse resp;
    from_json(result, resp);
    return std::move(resp.outcome);
}

std::vector<std::shared_ptr<neograph::Tool>>
ACPClient::session_tools(std::string_view session_id) const {
    if (!server_) return {};
    return server_->session_tools(session_id);
}

}  // namespace neograph::acp
