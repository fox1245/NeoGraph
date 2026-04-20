#include <neograph/mcp/client.h>

#include <neograph/async/http_client.h>
#include <neograph/async/run_sync.h>

#include <asio/this_coro.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace neograph::mcp {

// ===========================================================================
// detail::StdioSession — subprocess-backed JSON-RPC channel
// ===========================================================================
namespace detail {

class StdioSession {
public:
    static std::shared_ptr<StdioSession> spawn(const std::vector<std::string>& argv);

    ~StdioSession();

    // Send a JSON-RPC request, block until the matching response arrives.
    // `id_out` receives the allocated request id for diagnostics / matching.
    json rpc_call(const std::string& method, const json& params);

    // Send a JSON-RPC notification (no id, no response expected).
    void notify(const std::string& method, const json& params);

    pid_t pid() const { return pid_; }

private:
    StdioSession() = default;

    std::string read_line_locked();       ///< caller holds mtx_
    void write_frame_locked(const json& j); ///< caller holds mtx_

    pid_t pid_ = -1;
    int   stdin_fd_ = -1;   // parent → child
    int   stdout_fd_ = -1;  // child  → parent

    std::mutex   mtx_;
    std::string  buffer_;
    std::atomic<int> next_id_{0};
};

std::shared_ptr<StdioSession> StdioSession::spawn(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        throw std::invalid_argument("StdioSession::spawn: argv is empty");
    }

    int in_pipe[2]  = {-1, -1};  // parent writes → child stdin
    int out_pipe[2] = {-1, -1};  // child stdout → parent reads

    auto close_all = [&]() {
        for (int* fd : {&in_pipe[0], &in_pipe[1], &out_pipe[0], &out_pipe[1]}) {
            if (*fd >= 0) { ::close(*fd); *fd = -1; }
        }
    };

    if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0) {
        close_all();
        throw std::system_error(errno, std::generic_category(), "pipe()");
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        close_all();
        throw std::system_error(errno, std::generic_category(), "fork()");
    }

    if (pid == 0) {
        // --- child ---
        ::dup2(in_pipe[0],  STDIN_FILENO);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        // Leave stderr alone so server logs remain visible.

        ::close(in_pipe[0]);  ::close(in_pipe[1]);
        ::close(out_pipe[0]); ::close(out_pipe[1]);

        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);

        ::execvp(cargv[0], cargv.data());
        // execvp only returns on error.
        std::fprintf(stderr, "execvp(%s) failed: %s\n", cargv[0], std::strerror(errno));
        std::_Exit(127);
    }

    // --- parent ---
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);

    auto sess = std::shared_ptr<StdioSession>(new StdioSession());
    sess->pid_       = pid;
    sess->stdin_fd_  = in_pipe[1];
    sess->stdout_fd_ = out_pipe[0];
    return sess;
}

StdioSession::~StdioSession() {
    if (stdin_fd_  >= 0) ::close(stdin_fd_);
    if (stdout_fd_ >= 0) ::close(stdout_fd_);

    if (pid_ > 0) {
        ::kill(pid_, SIGTERM);

        // Poll for exit up to ~500 ms, then SIGKILL.
        for (int i = 0; i < 50; ++i) {
            int status = 0;
            pid_t w = ::waitpid(pid_, &status, WNOHANG);
            if (w == pid_) { pid_ = -1; return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ::kill(pid_, SIGKILL);
        int status = 0;
        ::waitpid(pid_, &status, 0);
        pid_ = -1;
    }
}

void StdioSession::write_frame_locked(const json& j) {
    std::string line = j.dump();
    line.push_back('\n');

    const char* p = line.data();
    size_t      remaining = line.size();
    while (remaining > 0) {
        ssize_t n = ::write(stdin_fd_, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(),
                                    "StdioSession::write()");
        }
        p         += n;
        remaining -= static_cast<size_t>(n);
    }
}

std::string StdioSession::read_line_locked() {
    while (true) {
        auto nl = buffer_.find('\n');
        if (nl != std::string::npos) {
            std::string line = buffer_.substr(0, nl);
            buffer_.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return line;
        }

        char tmp[4096];
        ssize_t n = ::read(stdout_fd_, tmp, sizeof(tmp));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(),
                                    "StdioSession::read()");
        }
        if (n == 0) {
            throw std::runtime_error("StdioSession: child closed stdout");
        }
        buffer_.append(tmp, static_cast<size_t>(n));
    }
}

json StdioSession::rpc_call(const std::string& method, const json& params) {
    const int id = ++next_id_;

    json req = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", method},
        {"params", params}
    };

    std::lock_guard<std::mutex> lock(mtx_);
    write_frame_locked(req);

    // Loop: the server may emit server-initiated notifications / log messages
    // while we wait for the response with matching id. Filter by id.
    for (int guard = 0; guard < 1024; ++guard) {
        std::string line = read_line_locked();
        if (line.empty()) continue;

        json resp;
        try {
            resp = json::parse(line);
        } catch (const std::exception&) {
            // Non-JSON line (e.g., stray log). Skip.
            continue;
        }

        if (!resp.contains("id")) continue;          // notification from server
        if (resp["id"] != id)     continue;          // response to a prior call?

        if (resp.contains("error")) {
            auto err = resp["error"];
            throw std::runtime_error(
                "MCP stdio RPC error: " + err.value("message", "unknown"));
        }
        return resp.value("result", json::object());
    }
    throw std::runtime_error("StdioSession::rpc_call: giving up after 1024 lines");
}

void StdioSession::notify(const std::string& method, const json& params) {
    json n = {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params}
    };
    std::lock_guard<std::mutex> lock(mtx_);
    write_frame_locked(n);
}

} // namespace detail

// ===========================================================================
// Shared helper — shape an MCP tools/call result into a string for the LLM.
// ===========================================================================
namespace {
std::string format_tool_result(const json& result) {
    if (result.contains("content") && result["content"].is_array()) {
        std::string output;
        for (const auto& item : result["content"]) {
            if (item.value("type", "") == "text") {
                if (!output.empty()) output += "\n";
                output += item.value("text", "");
            }
        }
        return output;
    }
    return result.dump();
}
} // namespace

// ===========================================================================
// MCPTool
// ===========================================================================

MCPTool::MCPTool(const std::string& server_url,
                 const std::string& name,
                 const std::string& description,
                 const json& input_schema)
  : server_url_(server_url)
  , stdio_session_(nullptr)
  , name_(name)
  , description_(description)
  , input_schema_(input_schema)
{
}

MCPTool::MCPTool(std::shared_ptr<detail::StdioSession> session,
                 const std::string& name,
                 const std::string& description,
                 const json& input_schema)
  : server_url_()
  , stdio_session_(std::move(session))
  , name_(name)
  , description_(description)
  , input_schema_(input_schema)
{
}

ChatTool MCPTool::get_definition() const {
    return { name_, description_, input_schema_ };
}

std::string MCPTool::execute(const json& arguments) {
    if (stdio_session_) {
        json result = stdio_session_->rpc_call(
            "tools/call",
            json{{"name", name_}, {"arguments", arguments}});
        return format_tool_result(result);
    }

    // HTTP path (legacy) — one ephemeral client per call.
    MCPClient client(server_url_);
    client.initialize();
    return format_tool_result(client.call_tool(name_, arguments));
}

// ===========================================================================
// MCPClient — HTTP transport
// ===========================================================================

namespace {

// Decompose an MCP server_url into the (host, port, prefix, tls)
// shape that neograph::async::async_post expects. Mirrors the helpers
// in OpenAIProvider/SchemaProvider — three copies is a cleanup target
// for end-of-Semester-2 refactor, not for now.
struct AsyncEndpoint {
    std::string host;
    std::string port;
    std::string prefix;
    bool        tls = false;
};

AsyncEndpoint split_async_endpoint(const std::string& server_url) {
    AsyncEndpoint out;
    std::string rest = server_url;

    auto scheme_end = rest.find("://");
    if (scheme_end != std::string::npos) {
        std::string scheme = rest.substr(0, scheme_end);
        out.tls = (scheme == "https");
        rest = rest.substr(scheme_end + 3);
    }

    auto path_start = rest.find('/');
    std::string authority;
    if (path_start != std::string::npos) {
        authority = rest.substr(0, path_start);
        out.prefix = rest.substr(path_start);
    } else {
        authority = rest;
    }

    auto colon = authority.find(':');
    if (colon != std::string::npos) {
        out.host = authority.substr(0, colon);
        out.port = authority.substr(colon + 1);
    } else {
        out.host = authority;
        out.port = out.tls ? "443" : "80";
    }
    return out;
}

} // namespace

MCPClient::MCPClient(const std::string& server_url)
  : server_url_(server_url)
{
    host_ = server_url;
    auto scheme_end = host_.find("://");
    if (scheme_end != std::string::npos) {
        auto path_start = host_.find('/', scheme_end + 3);
        if (path_start != std::string::npos) {
            path_prefix_ = host_.substr(path_start);
            host_        = host_.substr(0, path_start);
        }
    }
}

// ===========================================================================
// MCPClient — stdio transport
// ===========================================================================

MCPClient::MCPClient(std::vector<std::string> argv)
  : stdio_session_(detail::StdioSession::spawn(argv))
{
}

// ===========================================================================
// MCPClient — RPC dispatch
// ===========================================================================

json MCPClient::rpc_call(const std::string& method, const json& params) {
    if (stdio_session_) {
        return stdio_session_->rpc_call(method, params);
    }
    return async::run_sync(rpc_call_async(method, params));
}

asio::awaitable<json>
MCPClient::rpc_call_async(const std::string& method, const json& params) {
    if (stdio_session_) {
        // stdio sessions still drive a blocking rpc_call internally
        // (Sem 2.7 migrates them to asio::posix::stream_descriptor).
        // Yield to the executor first so callers see consistent
        // suspend/resume shape, then run the sync call inline.
        co_return stdio_session_->rpc_call(method, params);
    }

    json body;
    body["jsonrpc"] = "2.0";
    body["id"]      = ++request_id_;
    body["method"]  = method;
    body["params"]  = params;
    auto body_str = body.dump();

    auto endpoint = split_async_endpoint(server_url_);

    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"},
        {"Accept",       "application/json, text/event-stream"},
        {"Host",         "localhost"},
    };
    if (!session_id_.empty()) {
        headers.emplace_back("Mcp-Session-Id", session_id_);
    }

    async::RequestOptions opts;
    opts.timeout = std::chrono::seconds(30);

    auto ex = co_await asio::this_coro::executor;
    async::HttpResponse res;
    try {
        res = co_await async::async_post(
            ex,
            endpoint.host,
            endpoint.port,
            endpoint.prefix + "/mcp",
            body_str,
            std::move(headers),
            endpoint.tls,
            opts);
    } catch (const std::system_error& e) {
        throw std::runtime_error(std::string("MCP request failed: ") + e.what());
    }

    // Note: the previous httplib code captured Mcp-Session-Id from
    // response headers. async::HttpResponse only surfaces a couple of
    // hot fields (status/body/retry_after/location) — extending it
    // for arbitrary response headers is a Sem 1 follow-up. For now
    // we still get the session id back via the RPC body when the
    // server includes it; pure-header sessioning would need that
    // extension. None of the existing MCP tests rely on the header
    // shape since they hit a stdio server.
    if (res.status != 200) {
        throw std::runtime_error(
            "MCP error (HTTP " + std::to_string(res.status) + "): " + res.body);
    }

    // Parse response — may be plain JSON or SSE (event: message\ndata: {...})
    json resp;
    auto data_pos = res.body.find("data: ");
    if (data_pos != std::string::npos) {
        auto json_start = data_pos + 6;
        auto json_end   = res.body.find('\n', json_start);
        std::string json_str = (json_end != std::string::npos)
            ? res.body.substr(json_start, json_end - json_start)
            : res.body.substr(json_start);
        resp = json::parse(json_str);
    } else {
        resp = json::parse(res.body);
    }

    if (resp.contains("error")) {
        auto err = resp["error"];
        throw std::runtime_error("MCP RPC error: " + err.value("message", "unknown"));
    }

    co_return resp.value("result", json::object());
}

bool MCPClient::initialize(const std::string& client_name) {
    json params;
    params["protocolVersion"] = "2025-03-26";
    params["capabilities"]    = json::object();
    params["clientInfo"]      = {{"name", client_name}, {"version", "0.1.0"}};

    rpc_call("initialize", params);

    // Send initialized notification.
    if (stdio_session_) {
        stdio_session_->notify("notifications/initialized", json::object());
        return true;
    }

    // HTTP notification path. Per the MCP spec this is a notification
    // (no id) but the server still returns a status code on the HTTP
    // envelope, and a 4xx/5xx here means the session is misconfigured —
    // silently swallowing it (as the pre-audit code did) would leave the
    // caller with an "initialized" MCPClient that actually isn't.
    json notify;
    notify["jsonrpc"] = "2.0";
    notify["method"]  = "notifications/initialized";
    notify["params"]  = json::object();

    auto endpoint = split_async_endpoint(server_url_);
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"},
        {"Accept",       "application/json, text/event-stream"},
        {"Host",         "localhost"},
    };
    if (!session_id_.empty()) {
        headers.emplace_back("Mcp-Session-Id", session_id_);
    }

    auto notify_body = notify.dump();
    auto res = async::run_sync([&]() -> asio::awaitable<async::HttpResponse> {
        auto ex = co_await asio::this_coro::executor;
        co_return co_await async::async_post(
            ex,
            endpoint.host,
            endpoint.port,
            endpoint.prefix + "/mcp",
            notify_body,
            headers,
            endpoint.tls);
    }());

    // 200 OK and 202 Accepted are both valid per MCP spec for
    // notifications. Anything else is an error.
    if (res.status != 200 && res.status != 202 && res.status != 204) {
        throw std::runtime_error(
            "MCP initialize notification returned HTTP "
            + std::to_string(res.status) + ": " + res.body);
    }

    return true;
}

std::vector<std::unique_ptr<Tool>> MCPClient::get_tools() {
    initialize();

    auto result = rpc_call("tools/list", json::object());
    std::vector<std::unique_ptr<Tool>> tools;

    if (result.contains("tools") && result["tools"].is_array()) {
        for (const auto& t : result["tools"]) {
            auto name   = t.value("name", "");
            auto desc   = t.value("description", "");
            auto schema = t.value("inputSchema", json::object());

            if (stdio_session_) {
                tools.push_back(std::make_unique<MCPTool>(
                    stdio_session_, name, desc, schema));
            } else {
                tools.push_back(std::make_unique<MCPTool>(
                    server_url_, name, desc, schema));
            }
        }
    }

    return tools;
}

json MCPClient::call_tool(const std::string& name, const json& arguments) {
    json params;
    params["name"]      = name;
    params["arguments"] = arguments;
    return rpc_call("tools/call", params);
}

} // namespace neograph::mcp
