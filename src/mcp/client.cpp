#include <neograph/mcp/client.h>

#include <neograph/async/endpoint.h>
#include <neograph/async/http_client.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/cancel.h>

#include <asio/co_spawn.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/detached.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/read_until.hpp>
#include <asio/steady_timer.hpp>
#include <asio/streambuf.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <asio/associated_executor.hpp>
#include <asio/async_result.hpp>

#ifdef _WIN32
#  include <asio/windows/stream_handle.hpp>
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <asio/posix/stream_descriptor.hpp>
#  include <fcntl.h>
#  include <pthread.h>
#  include <signal.h>
#  include <sys/resource.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  if defined(__linux__)
#    include <sys/syscall.h>
#  endif
#  if defined(__APPLE__)
#    include <crt_externs.h>
#  endif
#endif

#include <atomic>
#include <algorithm>
#include <array>
#include <limits>
#include <cerrno>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <istream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <unordered_map>

namespace neograph::mcp {

namespace {
json extract_rpc_result(const json& response, int expected_id) {
    constexpr auto prefix = "MCP JSON-RPC";
    if (!response.is_object() || !response.contains("jsonrpc")
        || !response["jsonrpc"].is_string() || response["jsonrpc"] != "2.0") {
        throw MCPTransportError(MCPFailure::protocol,
                                "MCP JSON-RPC response is not a JSON-RPC 2.0 object");
    }
    if (!response.contains("id") || response["id"] != expected_id) {
        throw MCPTransportError(MCPFailure::protocol,
                                "MCP JSON-RPC response id does not match the request");
    }
    const bool has_result = response.contains("result");
    const bool has_error = response.contains("error");
    if (has_result == has_error) {
        throw MCPTransportError(MCPFailure::protocol,
                                "MCP JSON-RPC response must contain exactly one of result or error");
    }
    if (has_error) {
        const auto& error = response["error"];
        if (!error.is_object() || !error.value("code", json()).is_number_integer()
            || !error.value("message", json()).is_string()) {
            throw MCPTransportError(MCPFailure::protocol,
                                    "MCP JSON-RPC error must contain integer code and string message");
        }
        throw MCPError(error["code"].get<int>(),
                       std::string(prefix) + " error: " + error["message"].get<std::string>(),
                       error.value("data", json(nullptr)));
    }
    return response["result"];
}

asio::awaitable<void> wait_for_rpc_bound(
    std::chrono::steady_clock::time_point deadline,
    const std::shared_ptr<graph::CancelToken>& cancel_token,
    const std::shared_ptr<graph::CancelToken>& shutdown_token = {}) {
    while ((!cancel_token || !cancel_token->is_cancelled())
           && (!shutdown_token || !shutdown_token->is_cancelled())) {
        if (deadline <= std::chrono::steady_clock::now()) co_return;
        asio::steady_timer timer(co_await asio::this_coro::executor);
        timer.expires_at(std::min(deadline, std::chrono::steady_clock::now()
                                            + std::chrono::milliseconds(10)));
        co_await timer.async_wait(asio::use_awaitable);
    }
    co_return;
}

[[noreturn]] void throw_rpc_bound(
    std::chrono::steady_clock::time_point deadline,
    const std::shared_ptr<graph::CancelToken>& cancel_token,
    const std::shared_ptr<graph::CancelToken>& shutdown_token = {}) {
    if (shutdown_token && shutdown_token->is_cancelled())
        throw MCPTransportError(MCPFailure::shutdown, "MCP client has been shut down");
    if (cancel_token && cancel_token->is_cancelled()) {
        throw MCPTransportError(MCPFailure::cancelled, "MCP RPC cancelled");
    }
    if (deadline <= std::chrono::steady_clock::now()) {
        throw MCPTransportError(MCPFailure::timeout, "MCP RPC deadline elapsed");
    }
    throw MCPTransportError(MCPFailure::shutdown,
                            "MCP RPC bounds waiter completed unexpectedly");
}
template <typename T>
struct CapturedResult {
    std::optional<T> value;
    std::exception_ptr  error;
};

template <typename T, typename Executor>
asio::awaitable<CapturedResult<T>, Executor> capture_result(
    asio::awaitable<T, Executor> operation) {
    CapturedResult<T> result;
    try {
        result.value.emplace(co_await std::move(operation));
    } catch (...) {
        result.error = std::current_exception();
    }
    co_return result;
}

void validate_stdio_config(const StdioClientConfig& config) {
    if (config.argv.empty() || config.argv.front().empty())
        throw std::invalid_argument("MCP stdio executable is empty");
    for (const auto& arg : config.argv)
        if (arg.find('\0') != std::string::npos)
            throw std::invalid_argument("MCP stdio argv contains NUL");
    if (config.cwd.native().find(
            std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
        throw std::invalid_argument("MCP stdio cwd contains NUL");
    if (!config.replace_environment && !config.environment.empty())
        throw std::invalid_argument("MCP stdio environment requires replacement mode");
    for (std::size_t i = 0; i < config.environment.size(); ++i) {
        const auto& [key, value] = config.environment[i];
        if (key.empty() || key.find('=') != std::string::npos
            || key.find('\0') != std::string::npos || value.find('\0') != std::string::npos)
            throw std::invalid_argument("MCP stdio environment contains an invalid entry");
        for (std::size_t j = 0; j < i; ++j)
            if (config.environment[j].first == key)
                throw std::invalid_argument("MCP stdio environment contains a duplicate key");
    }
    if (!config.max_frame_bytes || config.startup_timeout.count() <= 0
        || config.request_timeout.count() <= 0)
        throw std::invalid_argument("MCP stdio bounds must be positive");
}

[[noreturn]] void throw_shutdown() {
    throw MCPTransportError(MCPFailure::shutdown, "MCP client has been shut down");
}

asio::awaitable<json> notification_result(asio::awaitable<void> operation) {
    co_await std::move(operation);
    co_return json();
}
} // namespace

// HTTP and subprocess transports implement the same frame exchange contract.
// Only the protocol session interprets JSON-RPC messages and MCP tool results.
// POSIX resolves PATH before fork and uses execve with subprocess pipes;
// Windows uses CreateProcess and asio::windows::stream_handle.
namespace detail {

// Admission and co_spawn initiation share one lock. Shutdown cannot stop the
// executor between a caller checking admission and enqueueing its operation.
// Transport work drains here, independently of the caller's executor.
class TransportWorker {
public:
    asio::io_context io;
    std::atomic<bool> stopped{false};

    ~TransportWorker() { shutdown([] {}); }

    asio::awaitable<json> submit(asio::awaitable<json> operation) {
        return asio::async_initiate<decltype(asio::use_awaitable),
            void(std::exception_ptr, json)>(
            [this, operation = std::move(operation)](auto handler) mutable {
                std::lock_guard lk(admission_mu_);
                if (stopped.load()) {
                    auto ex = asio::get_associated_executor(handler);
                    asio::post(ex, [handler = std::move(handler)]() mutable {
                        handler(std::make_exception_ptr(MCPTransportError(
                            MCPFailure::shutdown, "MCP client has been shut down")), json());
                    });
                    return;
                }
                start_locked();
                asio::co_spawn(io, std::move(operation), std::move(handler));
            }, asio::use_awaitable);
    }

    template <typename Start>
    void start(Start start) {
        std::lock_guard lk(admission_mu_);
        if (stopped.load()) throw_shutdown();
        start_locked();
        asio::post(io, std::move(start));
    }

    template <typename Close>
    void shutdown(Close close) noexcept {
        const bool own_thread = io.get_executor().running_in_this_thread();
        {
            std::lock_guard lk(admission_mu_);
            if (!stopped.exchange(true)) {
                if (own_thread || !thread_.joinable()) {
                    close();
                } else {
                    try { asio::post(io, std::move(close)); }
                    catch (...) { io.stop(); }
                }
                guard_.reset();
            }
        }
        if (!own_thread) {
            std::lock_guard lk(join_mu_);
            if (thread_.joinable()) thread_.join();
        }
    }

private:
    void start_locked() {
        if (thread_.joinable()) return;
        thread_ = std::thread([this] {
#ifndef _WIN32
            sigset_t mask;
            sigemptyset(&mask);
            sigaddset(&mask, SIGPIPE);
            ::pthread_sigmask(SIG_BLOCK, &mask, nullptr);
#endif
            io.run();
        });
    }
    asio::executor_work_guard<asio::io_context::executor_type> guard_{
        asio::make_work_guard(io)};
    std::mutex admission_mu_;
    std::mutex join_mu_;
    std::thread thread_;
};

// A transport owns its connection-specific state. The protocol layer owns
// request envelopes, response validation, initialization, and tool semantics.
// Correlation is performed by each transport using the envelope's id.
struct TransportCapabilities {
    bool concurrent_requests;
    bool cancellation;
    bool deadlines;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual TransportCapabilities capabilities() const noexcept = 0;
    virtual asio::awaitable<json> exchange(
        json request, std::chrono::steady_clock::time_point deadline,
        std::shared_ptr<graph::CancelToken> cancel_token) = 0;
    virtual asio::awaitable<void> notify(json notification) = 0;
    virtual void negotiated_version(const std::string&) {}
    virtual void reset() {}
    virtual void shutdown() noexcept = 0;
};

class HttpSession final : public Transport {
public:
    HttpSession(std::string url, MCPClientConfig client_config)
      : server_url(std::move(url))
      , endpoint(async::split_async_endpoint(server_url))
      , config(std::move(client_config))
    {
        if (endpoint.host.empty()) {
            throw std::invalid_argument("MCP server URL has no host");
        }
        const auto& prefix = endpoint.prefix;
        if (prefix.empty() || prefix == "/") {
            path = "/mcp";
        } else if (prefix == "/mcp"
                   || (prefix.size() > 4
                       && prefix.compare(prefix.size() - 4, 4, "/mcp") == 0)) {
            path = prefix;
        } else {
            path = prefix + "/mcp";
        }
    }

    TransportCapabilities capabilities() const noexcept override {
        return {true, true, true};
    }
    asio::awaitable<json> exchange(
        json request, std::chrono::steady_clock::time_point deadline,
        std::shared_ptr<graph::CancelToken> cancel_token) override;
    asio::awaitable<json> do_exchange(
        json request, std::chrono::steady_clock::time_point deadline,
        std::shared_ptr<graph::CancelToken> cancel_token);
    asio::awaitable<void> notify(json notification) override;
    asio::awaitable<void> do_notify(json notification);
    ~HttpSession() override { shutdown(); }
    void shutdown() noexcept override {
        worker.shutdown([this] { shutdown_token->cancel(); });
    }
    TransportWorker worker;
    std::shared_ptr<graph::CancelToken> shutdown_token =
        std::make_shared<graph::CancelToken>();
    void negotiated_version(const std::string& version) override {
        std::lock_guard lk(mu);
        protocol_version = version;
    }
    void reset() override {
        std::lock_guard lk(mu);
        session_id.clear();
        protocol_version.clear();
    }

    std::string server_url;
    async::AsyncEndpoint endpoint;
    std::string path;
    MCPClientConfig config;
    std::mutex mu;
    std::string session_id;
    std::string protocol_version;
};

class ClientMetadata {
public:
    enum class Lifecycle { created, initializing, initialized, shutdown };

    mutable std::mutex mu;
    std::condition_variable changed;
    Lifecycle lifecycle = Lifecycle::created;
    InitializeResult initialize_result;
    std::unordered_map<std::string, json> output_schemas;
};
class ProtocolSession {
public:
    explicit ProtocolSession(std::shared_ptr<Transport> transport)
      : transport_(std::move(transport)) {}

    json rpc_call(const std::string& method, const json& params = json::object());
    asio::awaitable<json> rpc_call_async(
        const std::string& method, const json& params,
        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::time_point::max(),
        std::shared_ptr<graph::CancelToken> cancel_token = {});
    bool initialize(const std::string& client_name = "neograph");
    asio::awaitable<bool> initialize_async(const std::string& client_name = "neograph");
    void shutdown() noexcept;
    bool is_initialized() const noexcept;
    InitializeResult get_initialize_result() const;
    ListToolsPage list_tools(const std::optional<std::string>& cursor);
    asio::awaitable<ListToolsPage> list_tools_async(
        const std::optional<std::string>& cursor);
    std::vector<ToolDefinition> get_tool_definitions();
    json call_tool(const std::string& name, const json& arguments);
    asio::awaitable<json> call_tool_async(
        const std::string& name, const json& arguments);
    CallToolResult call_tool_result(const std::string& name, const json& arguments);
    asio::awaitable<CallToolResult> call_tool_result_async(
        const std::string& name, const json& arguments);

    std::shared_ptr<Transport> transport_;
    std::shared_ptr<ClientMetadata> metadata_ = std::make_shared<ClientMetadata>();
    std::atomic<int> next_id_{0};
    std::atomic<bool> stopped_{false};
};


#ifdef _WIN32
using AsyncHandle  = asio::windows::stream_handle;
#else
using AsyncHandle  = asio::posix::stream_descriptor;
#endif

class StdioSession final : public Transport {
public:
    static std::shared_ptr<StdioSession> spawn(const std::vector<std::string>& argv);
    static std::shared_ptr<StdioSession> spawn(StdioClientConfig config);
    ~StdioSession();

    TransportCapabilities capabilities() const noexcept override {
        return {true, true, true};
    }
    asio::awaitable<json> exchange(
        json request, std::chrono::steady_clock::time_point deadline,
        std::shared_ptr<graph::CancelToken> cancel_token) override;
    asio::awaitable<void> notify(json notification) override;

    void shutdown() noexcept override;
    void reset() override { if (hardened_) shutdown(); }
private:
    StdioSession() = default;
    static std::shared_ptr<StdioSession> spawn_impl(StdioClientConfig config, bool hardened);
    void start_stderr();
    asio::awaitable<void> read_stderr();
    void close_io() noexcept;
    void check_open() const;

    /// The actual exchange runs on the session-owned io_context.
    asio::awaitable<json> do_exchange(json request);
    asio::awaitable<void> do_notify(json notification);

    asio::awaitable<std::string> async_read_line_locked(AsyncHandle& out);
    asio::awaitable<void> async_write_frame_locked(AsyncHandle& in, const json& j);

    /// One reader routes responses by id. It starts when a call is in
    /// flight, stops after draining all responses, and may remain suspended
    /// on the pipe after cancellation until a late response or shutdown.
    asio::awaitable<void> run_reader();

#ifdef _WIN32
    HANDLE process_ = nullptr;   ///< child process handle (CloseHandle on dtor)
    HANDLE job_ = nullptr;       ///< owns descendants even after the leader exits
    HANDLE stderr_h_ = nullptr;
    HANDLE stdin_h_ = nullptr;   ///< parent → child (write end of a pipe)
    HANDLE stdout_h_ = nullptr;  ///< child → parent (read end of a pipe)
#else
    pid_t pid_ = -1;
    int   stdin_fd_ = -1;   // parent → child
    int   stdout_fd_ = -1;  // child  → parent
    int   stderr_fd_ = -1;
#endif

    // Outlives all executor-bound handles and channels below.
    TransportWorker worker_;
    std::mutex process_mu_;
    void terminate_process() noexcept;
    bool hardened_ = false;
    std::size_t max_stderr_bytes_ = 64 * 1024;
    std::string stderr_buffer_;
    std::chrono::steady_clock::time_point startup_deadline_ =
        std::chrono::steady_clock::time_point::max();
    std::atomic<bool> startup_complete_{false};
    std::string abuffer_; ///< Pending bytes on the single async reader.
    std::size_t max_frame_bytes_ = 16 * 1024 * 1024;
    std::chrono::milliseconds request_timeout_{30000};
    // Capacity-one channel serializes only frame writes. A waiting writer
    // suspends cooperatively; it never blocks the transport's I/O thread.
    using AsyncLock = asio::experimental::channel<void(asio::error_code)>;
    std::unique_ptr<AsyncLock> async_lock_;
    std::mutex async_lock_init_mtx_;

    // Cache wrappers bound to the transport executor. Windows associates
    // the pipe FILE_OBJECT with an IOCP once, so duplicating a handle into a
    // different context on each call is invalid. Wrappers own duplicates
    // of the native handles; the session retains the originals.
    std::unique_ptr<AsyncHandle> async_in_;
    std::unique_ptr<AsyncHandle> async_out_;
    std::unique_ptr<AsyncHandle> async_err_;
    std::mutex async_handles_init_mtx_;

    // One reader fans responses out to id-keyed waiters; calls overlap
    // their reads rather than serializing entire round trips.
    using RespChan =
        asio::experimental::channel<void(asio::error_code,
                                         std::shared_ptr<json>)>;
    std::mutex demux_mu_; ///< guards waiters, reader state, and reader failure
    std::map<int, std::shared_ptr<RespChan>> waiters_;
    bool reader_running_ = false;
    std::exception_ptr reader_failure_;
};

std::shared_ptr<StdioSession> StdioSession::spawn(const std::vector<std::string>& argv) {
    StdioClientConfig config;
    config.argv = argv;
    return spawn_impl(std::move(config), false);
}

std::shared_ptr<StdioSession> StdioSession::spawn(StdioClientConfig config) {
    return spawn_impl(std::move(config), true);
}

#ifdef _WIN32

namespace {
[[noreturn]] void windows_error(const char* operation) {
    throw std::system_error(static_cast<int>(GetLastError()),
                            std::system_category(), operation);
}

std::wstring windows_utf8(const std::string& text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("MCP Windows launch string is too long");
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count) windows_error("MultiByteToWideChar");
    std::wstring result(count, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), count))
        windows_error("MultiByteToWideChar");
    return result;
}

// Match CommandLineToArgvW / the CRT: backslashes preceding quotes and the
// closing quote must be doubled; other backslashes are literal.
std::wstring build_win_cmdline(const std::vector<std::string>& argv) {
    std::wstring out;
    for (const auto& argument : argv) {
        if (!out.empty()) out.push_back(L' ');
        const auto arg = windows_utf8(argument);
        out.push_back(L'"');
        std::size_t slashes = 0;
        for (wchar_t ch : arg) {
            if (ch == L'\\') { ++slashes; continue; }
            out.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            slashes = 0;
            out.push_back(ch);
        }
        out.append(slashes * 2, L'\\');
        out.push_back(L'"');
    }
    return out;
}

std::wstring windows_executable(const std::string& argument) {
    auto name = windows_utf8(argument);
    DWORD count = SearchPathW(nullptr, name.c_str(), L".exe", 0, nullptr, nullptr);
    if (!count) windows_error("SearchPathW");
    std::wstring path(count, L'\0');
    DWORD length = SearchPathW(nullptr, name.c_str(), L".exe",
                               count, path.data(), nullptr);
    if (!length || length >= count) windows_error("SearchPathW");
    path.resize(length);
    return path;
}

std::vector<wchar_t> windows_environment(const StdioClientConfig& config) {
    std::vector<std::pair<std::wstring, std::wstring>> entries;
    for (const auto& [key, value] : config.environment)
        entries.emplace_back(windows_utf8(key), windows_utf8(value));
    const auto compare = [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                    b.c_str(), static_cast<int>(b.size()), TRUE);
    };
    std::sort(entries.begin(), entries.end(), [&](const auto& a, const auto& b) {
        return compare(a.first, b.first) == CSTR_LESS_THAN;
    });
    std::vector<wchar_t> block;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i && compare(entries[i - 1].first, entries[i].first) == CSTR_EQUAL)
            throw std::invalid_argument("MCP Windows environment contains a duplicate key");
        const auto& [key, value] = entries[i];
        block.insert(block.end(), key.begin(), key.end());
        block.push_back(L'=');
        block.insert(block.end(), value.begin(), value.end());
        block.push_back(L'\0');
    }
    if (block.empty()) block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

// Create a named-pipe pair where the parent side supports
// FILE_FLAG_OVERLAPPED (needed for asio::windows::stream_handle) and
// the child side is a plain inheritable handle. CreatePipe's anonymous
// pipes don't support overlapped I/O, hence the named-pipe dance.
struct PipePair {
    HANDLE parent = nullptr;
    HANDLE child = nullptr;
    PipePair() = default;
    PipePair(HANDLE p, HANDLE c) : parent(p), child(c) {}
    PipePair(const PipePair&) = delete;
    PipePair& operator=(const PipePair&) = delete;
    PipePair(PipePair&& other) noexcept
      : parent(std::exchange(other.parent, nullptr))
      , child(std::exchange(other.child, nullptr)) {}
    PipePair& operator=(PipePair&& other) noexcept {
        if (this != &other) {
            close();
            parent = std::exchange(other.parent, nullptr);
            child = std::exchange(other.child, nullptr);
        }
        return *this;
    }
    void close_child() noexcept {
        if (child) CloseHandle(std::exchange(child, nullptr));
    }
    void close() noexcept {
        if (parent) CloseHandle(std::exchange(parent, nullptr));
        close_child();
    }
    ~PipePair() { close(); }
};
PipePair make_overlapped_pipe(const char* name_prefix, bool parent_reads) {
    static std::atomic<uint64_t> counter{0};
    char name[128];
    std::snprintf(name, sizeof(name),
        "\\\\.\\pipe\\neograph_mcp_%s_%lu_%llu",
        name_prefix,
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long long>(counter.fetch_add(1)));

    DWORD parent_mode = parent_reads
        ? (PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED)
        : (PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED);
    HANDLE parent = CreateNamedPipeA(
        name, parent_mode,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        /*instances=*/1, /*outbuf=*/64*1024, /*inbuf=*/64*1024,
        /*timeout=*/0, /*sa=*/nullptr);
    if (parent == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(GetLastError()),
            std::system_category(), "CreateNamedPipe");
    }

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };  // inheritable
    DWORD child_access = parent_reads ? GENERIC_WRITE : GENERIC_READ;
    HANDLE child = CreateFileA(name, child_access,
        0, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (child == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        CloseHandle(parent);
        throw std::system_error(static_cast<int>(err),
            std::system_category(), "CreateFile(child side)");
    }

    // Parent side must NOT be inherited by the child.
    if (!SetHandleInformation(parent, HANDLE_FLAG_INHERIT, 0)) {
        DWORD err = GetLastError();
        CloseHandle(parent);
        CloseHandle(child);
        throw std::system_error(static_cast<int>(err),
            std::system_category(), "SetHandleInformation(parent)");
    }
    return PipePair{parent, child};
}
} // namespace

std::shared_ptr<StdioSession> StdioSession::spawn_impl(
    StdioClientConfig config, bool hardened) {
    validate_stdio_config(config);
    auto executable = windows_executable(config.argv.front());
    auto extension = std::filesystem::path(executable).extension().wstring();
    if (CompareStringOrdinal(extension.c_str(), -1, L".bat", -1, TRUE) == CSTR_EQUAL
        || CompareStringOrdinal(extension.c_str(), -1, L".cmd", -1, TRUE) == CSTR_EQUAL)
        throw std::invalid_argument("MCP stdio refuses implicit batch-file execution");
    auto cmdline = build_win_cmdline(config.argv);
    auto environment = windows_environment(config);
    auto sess = std::shared_ptr<StdioSession>(new StdioSession());
    sess->hardened_ = hardened;
    sess->max_frame_bytes_ = config.max_frame_bytes;
    sess->max_stderr_bytes_ = config.max_stderr_bytes;
    sess->request_timeout_ = config.request_timeout;
    if (hardened)
        sess->startup_deadline_ = std::chrono::steady_clock::now() + config.startup_timeout;
    PipePair in_p = make_overlapped_pipe("in", false);
    PipePair out_p = make_overlapped_pipe("out", true);
    PipePair err_p;
    if (hardened) {
        err_p = make_overlapped_pipe("err", true);
    } else {
        HANDLE parent_error = GetStdHandle(STD_ERROR_HANDLE);
        if (parent_error && parent_error != INVALID_HANDLE_VALUE) {
            if (!DuplicateHandle(GetCurrentProcess(), parent_error, GetCurrentProcess(),
                                 &err_p.child, 0, TRUE, DUPLICATE_SAME_ACCESS))
                windows_error("DuplicateHandle(stderr)");
        } else {
            SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
            err_p.child = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      &attributes, OPEN_EXISTING, 0, nullptr);
            if (err_p.child == INVALID_HANDLE_VALUE) {
                err_p.child = nullptr;
                windows_error("CreateFileW(NUL)");
            }
        }
    }

    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<unsigned char> attribute_storage(attribute_size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size))
        windows_error("InitializeProcThreadAttributeList");
    struct AttributeGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~AttributeGuard() { DeleteProcThreadAttributeList(value); }
    } attribute_guard{attributes};
    HANDLE inherited[] = {in_p.child, out_p.child, err_p.child};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  inherited, sizeof(inherited), nullptr, nullptr))
        windows_error("UpdateProcThreadAttribute");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = in_p.child;
    startup.StartupInfo.hStdOutput = out_p.child;
    startup.StartupInfo.hStdError = err_p.child;
    startup.lpAttributeList = attributes;

    sess->job_ = CreateJobObjectW(nullptr, nullptr);
    if (!sess->job_) windows_error("CreateJobObjectW");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(sess->job_, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits)))
        windows_error("SetInformationJobObject");
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), cmdline.data(), nullptr, nullptr, TRUE,
                         CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                         config.replace_environment ? environment.data() : nullptr,
                         config.cwd.empty() ? nullptr : config.cwd.c_str(),
                         &startup.StartupInfo, &process))
        windows_error("CreateProcessW");
    sess->process_ = process.hProcess;
    struct ThreadGuard {
        HANDLE value;
        ~ThreadGuard() { CloseHandle(value); }
    } thread_guard{process.hThread};
    // No child code can run (or spawn outside the job) before assignment.
    if (!AssignProcessToJobObject(sess->job_, sess->process_))
        windows_error("AssignProcessToJobObject");
    sess->stdin_h_ = std::exchange(in_p.parent, nullptr);
    sess->stdout_h_ = std::exchange(out_p.parent, nullptr);
    sess->stderr_h_ = std::exchange(err_p.parent, nullptr);
    in_p.close_child();
    out_p.close_child();
    err_p.close_child();
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1))
        windows_error("ResumeThread");
    if (hardened) sess->start_stderr();
    return sess;
}

StdioSession::~StdioSession() { shutdown(); }

void StdioSession::terminate_process() noexcept {
    std::lock_guard lk(process_mu_);
    if (job_) {
        TerminateJobObject(job_, 1);
        CloseHandle(std::exchange(job_, nullptr));
    }
    if (process_) {
        // Also covers a suspended child whose job assignment failed.
        TerminateProcess(process_, 1);
        WaitForSingleObject(process_, INFINITE);
        CloseHandle(std::exchange(process_, nullptr));
    }
    if (stdin_h_) CloseHandle(std::exchange(stdin_h_, nullptr));
    if (stdout_h_) CloseHandle(std::exchange(stdout_h_, nullptr));
    if (stderr_h_) CloseHandle(std::exchange(stderr_h_, nullptr));
}

#else  // !_WIN32

std::shared_ptr<StdioSession> StdioSession::spawn_impl(
    StdioClientConfig config, bool hardened) {
    validate_stdio_config(config);
    const auto& argv = config.argv;
    // After fork the child may inherit libc locks held by another thread, so
    // it only uses async-signal-safe syscalls before execve or _Exit.
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& arg : argv) cargv.push_back(const_cast<char*>(arg.c_str()));
    cargv.push_back(nullptr);
    std::vector<char*> shell_argv;
    shell_argv.reserve(argv.size() + 2);
    shell_argv.push_back(const_cast<char*>("sh"));
    shell_argv.push_back(nullptr); // Selected script path, set in the child.
    for (size_t i = 1; i < argv.size(); ++i) {
        shell_argv.push_back(const_cast<char*>(argv[i].c_str()));
    }
    shell_argv.push_back(nullptr);

    std::vector<std::string> candidates;
    if (argv[0].find('/') != std::string::npos) {
        candidates.push_back(argv[0]);
    } else {
        const char* env_path = config.replace_environment ? nullptr : std::getenv("PATH");
        if (config.replace_environment)
            for (const auto& [key, value] : config.environment)
                if (key == "PATH") env_path = value.c_str();
        const std::string path = env_path ? env_path : "/bin:/usr/bin";
        for (size_t start = 0; start <= path.size();) {
            const auto end = path.find(':', start);
            const auto directory = path.substr(
                start, end == std::string::npos ? end : end - start);
            candidates.push_back(directory.empty() ? argv[0]
                                                    : directory + "/" + argv[0]);
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }

    std::vector<std::string> child_environment;
    std::vector<char*> child_envp;
    if (config.replace_environment) {
        child_environment.reserve(config.environment.size());
        for (const auto& [key, value] : config.environment)
            child_environment.push_back(key + "=" + value);
        child_envp.reserve(child_environment.size() + 1);
        for (auto& item : child_environment) child_envp.push_back(item.data());
        child_envp.push_back(nullptr);
    }
    struct rlimit descriptor_limit {};
    if (::getrlimit(RLIMIT_NOFILE, &descriptor_limit) != 0)
        throw std::system_error(errno, std::generic_category(), "getrlimit()");
    const long open_max = ::sysconf(_SC_OPEN_MAX);
    if (descriptor_limit.rlim_max == RLIM_INFINITY && open_max <= 0)
        throw std::runtime_error("MCP descriptor limit is unavailable");
    const auto max_fd = std::min<rlim_t>(
        descriptor_limit.rlim_max == RLIM_INFINITY
            ? static_cast<rlim_t>(open_max) : descriptor_limit.rlim_max,
        static_cast<rlim_t>(std::numeric_limits<int>::max()));

    int in_pipe[2]  = {-1, -1};  // parent writes → child stdin
    int out_pipe[2] = {-1, -1};  // child stdout → parent reads
    int err_pipe[2] = {-1, -1};
    const auto startup_deadline = std::chrono::steady_clock::now() + config.startup_timeout;

    auto close_all = [&]() {
        for (int* fd : {&in_pipe[0], &in_pipe[1], &out_pipe[0], &out_pipe[1],
                       &err_pipe[0], &err_pipe[1]}) {
            if (*fd >= 0) { ::close(*fd); *fd = -1; }
        }
    };

    if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0
        || (hardened && ::pipe(err_pipe) != 0)) {
        const int error = errno;
        close_all();
        throw std::system_error(error, std::generic_category(), "pipe()");
    }

    char* const* environment = child_envp.data();
    if (!config.replace_environment) {
#if defined(__APPLE__)
        environment = *_NSGetEnviron();
#else
        environment = ::environ;
#endif
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        const int error = errno;
        close_all();
        throw std::system_error(error, std::generic_category(), "fork()");
    }

    if (pid == 0) {
        if (::setpgid(0, 0) != 0) std::_Exit(127);
        // --- child ---
        if (::dup2(in_pipe[0], STDIN_FILENO) < 0
            || ::dup2(out_pipe[1], STDOUT_FILENO) < 0) {
            std::_Exit(127);
        }
        if (hardened && ::dup2(err_pipe[1], STDERR_FILENO) < 0) std::_Exit(127);
        if (!config.cwd.empty() && ::chdir(config.cwd.c_str()) != 0) std::_Exit(126);
        const int pipes[] = {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1],
                             err_pipe[0], err_pipe[1]};
        for (int fd : pipes)
            if (fd > STDERR_FILENO) ::close(fd);
#if defined(__linux__) && defined(SYS_close_range)
        if (::syscall(SYS_close_range, 3u, ~0u, 0u) != 0) {
#endif
            for (rlim_t fd = 3; fd < max_fd; ++fd) ::close(static_cast<int>(fd));
#if defined(__linux__) && defined(SYS_close_range)
        }
#endif
        for (const auto& candidate : candidates) {
            ::execve(candidate.c_str(), cargv.data(), environment);
            const int error = errno;
            if (error == ENOEXEC && !hardened) {
                shell_argv[1] = const_cast<char*>(candidate.c_str());
                ::execve("/bin/sh", shell_argv.data(), environment);
                break;
            }
            if (error == EACCES || error == ENOENT || error == ENOTDIR) continue;
            break;
        }
        constexpr char message[] = "MCP stdio exec failed\n";
        (void)::write(STDERR_FILENO, message, sizeof(message) - 1);
        std::_Exit(127);
    }

    ::setpgid(pid, pid);
    // --- parent ---
    ::close(in_pipe[0]);
    in_pipe[0] = -1;
    ::close(out_pipe[1]);
    out_pipe[1] = -1;
    if (err_pipe[1] >= 0) ::close(std::exchange(err_pipe[1], -1));

    try {
        auto sess = std::shared_ptr<StdioSession>(new StdioSession());
        sess->hardened_ = hardened;
        sess->max_frame_bytes_ = config.max_frame_bytes;
        sess->max_stderr_bytes_ = config.max_stderr_bytes;
        sess->request_timeout_ = config.request_timeout;
        if (hardened) sess->startup_deadline_ = startup_deadline;
        sess->pid_ = std::exchange(pid, -1);
        sess->stdin_fd_ = std::exchange(in_pipe[1], -1);
        sess->stdout_fd_ = std::exchange(out_pipe[0], -1);
        sess->stderr_fd_ = std::exchange(err_pipe[0], -1);
        if (hardened) sess->start_stderr();
        return sess;
    } catch (...) {
        close_all();
        if (pid > 0) {
            ::kill(-pid, SIGKILL);
            ::kill(pid, SIGKILL);
            int status;
            while (::waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
        }
        throw;
    }
}

StdioSession::~StdioSession() { shutdown(); }

void StdioSession::terminate_process() noexcept {
    std::lock_guard lk(process_mu_);
    if (pid_ > 0) {
        const pid_t group = pid_;
        ::kill(-group, SIGTERM);
        ::kill(pid_, SIGTERM);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        // The leader can already be dead while a TERM-ignoring descendant
        // still owns this group. Do not return early after reaping the leader.
        ::kill(-group, SIGKILL);
        ::kill(pid_, SIGKILL);
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
        pid_ = -1;
    }
    if (stdin_fd_ >= 0) ::close(std::exchange(stdin_fd_, -1));
    if (stdout_fd_ >= 0) ::close(std::exchange(stdout_fd_, -1));
    if (stderr_fd_ >= 0) ::close(std::exchange(stderr_fd_, -1));
}
#endif  // _WIN32

void StdioSession::check_open() const {
    if (worker_.stopped.load()) throw_shutdown();
}

void StdioSession::close_io() noexcept {
    asio::error_code ec;
    if (async_in_) async_in_->close(ec);
    if (async_out_) async_out_->close(ec);
    if (async_err_) async_err_->close(ec);
    if (async_lock_) {
        async_lock_->cancel();
        async_lock_->close();
    }
    for (auto& [id, channel] : waiters_) {
        channel->cancel();
        channel->close();
    }
    // Keep wrappers alive until suspended writes/reads have unwound.
}

void StdioSession::shutdown() noexcept {
    worker_.shutdown([this] { close_io(); });
    terminate_process();
}

void StdioSession::start_stderr() {
#ifdef _WIN32
    async_err_ = std::make_unique<AsyncHandle>(worker_.io, stderr_h_);
    stderr_h_ = nullptr;
#else
    async_err_ = std::make_unique<AsyncHandle>(worker_.io, stderr_fd_);
    stderr_fd_ = -1;
#endif
    worker_.start([this] {
        asio::co_spawn(worker_.io, read_stderr(), asio::detached);
    });
}

asio::awaitable<void> StdioSession::read_stderr() {
    std::array<char, 4096> bytes;
    try {
        for (;;) {
            const auto remaining = max_stderr_bytes_ - stderr_buffer_.size();
            const auto capacity = remaining >= bytes.size() ? bytes.size() : remaining + 1;
            const auto count = co_await async_err_->async_read_some(
                asio::buffer(bytes.data(), capacity), asio::use_awaitable);
            if (count > remaining) {
                {
                    std::lock_guard lk(demux_mu_);
                    reader_failure_ = std::make_exception_ptr(MCPTransportError(
                        MCPFailure::protocol, "MCP stdio stderr limit exceeded"));
                }
                shutdown();
                co_return;
            }
            stderr_buffer_.append(bytes.data(), count);
        }
    } catch (const std::system_error& error) {
        if (error.code() != asio::error::eof
            && error.code() != asio::error::operation_aborted)
            shutdown();
    }
}

asio::awaitable<void>
StdioSession::async_write_frame_locked(AsyncHandle& in,
                                       const json& j) {
    std::string line = j.dump();
    line.push_back('\n');
    co_await asio::async_write(in, asio::buffer(line), asio::use_awaitable);
}

asio::awaitable<std::string>
StdioSession::async_read_line_locked(AsyncHandle& out) {
    auto nl = abuffer_.find('\n');
    if (nl == std::string::npos) {
        asio::streambuf sbuf(max_frame_bytes_);
        // Seed asio's streambuf with whatever we already have so
        // async_read_until doesn't re-read those bytes.
        if (!abuffer_.empty()) {
            std::ostream os(&sbuf);
            os.write(abuffer_.data(), static_cast<std::streamsize>(abuffer_.size()));
            abuffer_.clear();
        }
        std::size_t n = 0;
        try {
            n = co_await asio::async_read_until(
                out, sbuf, '\n', asio::use_awaitable);
        } catch (const std::system_error& e) {
            if (e.code() == asio::error::not_found) {
                throw MCPTransportError(
                    MCPFailure::protocol, "MCP stdio frame exceeds configured limit");
            }
            throw;
        }
        // Re-merge into our string buffer so the rest of the trailing
        // bytes (after the newline) are kept for the next call.
        std::string drained(asio::buffers_begin(sbuf.data()),
                            asio::buffers_begin(sbuf.data()) + sbuf.size());
        abuffer_.append(drained);
        nl = abuffer_.find('\n');
        if (nl == std::string::npos) {
            // async_read_until promised a delim was found within `n`
            // bytes; this branch is defensive.
            throw MCPTransportError(MCPFailure::protocol,
                "MCP stdio delimiter missing after "
                + std::to_string(n) + " bytes");
        }
    }
    std::string line = abuffer_.substr(0, nl);
    abuffer_.erase(0, nl + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    co_return line;
}

asio::awaitable<void> StdioSession::run_reader() {
    std::exception_ptr fail;
    try {
        for (;;) {
            std::string line = co_await async_read_line_locked(*async_out_);
            if (!line.empty()) {
                json resp;
                try {
                    resp = json::parse(line);
                } catch (const json::parse_error& e) {
                    throw MCPTransportError(
                        MCPFailure::protocol,
                        std::string("MCP stdio response JSON: ") + e.what());
                }
                if (!resp.is_object()) {
                    throw MCPTransportError(MCPFailure::protocol,
                                            "MCP stdio frame must be a JSON object");
                }
                if (resp.contains("id") && resp["id"].is_number_integer()) {
                    const int rid = resp["id"].get<int>();
                    std::shared_ptr<RespChan> chan;
                    {
                        std::lock_guard<std::mutex> lk(demux_mu_);
                        auto it = waiters_.find(rid);
                        if (it != waiters_.end()) {
                            chan = it->second;
                            waiters_.erase(it);
                        }
                    }
                    // Unknown / late ids are dropped. Capacity-1 sink is
                    // empty, so this send always lands for the matched id.
                    if (chan) {
                        auto p = std::make_shared<json>(std::move(resp));
                        chan->try_send(asio::error_code{}, p);
                    }
                }
            }
            // Stop once nothing is outstanding so a private run_sync
            // io_context can drain and return; a later call lazily
            // restarts the reader.
            bool stop = false;
            {
                std::lock_guard<std::mutex> lk(demux_mu_);
                if (waiters_.empty()) {
                    reader_running_ = false;
                    stop = true;
                }
            }
            if (stop) break;
        }
    } catch (const std::system_error& error) {
        fail = std::make_exception_ptr(MCPTransportError(
            worker_.stopped.load() ? MCPFailure::shutdown : MCPFailure::connection,
            std::string("MCP stdio read failed: ") + error.what()));
    } catch (const std::exception&) {
        // Fail all callers on EOF, malformed frames, or broken pipes.
        fail = std::current_exception();
    }

    if (fail) {
        std::map<int, std::shared_ptr<RespChan>> remaining;
        {
            std::lock_guard<std::mutex> lk(demux_mu_);
            if (!reader_failure_) reader_failure_ = fail;
            remaining.swap(waiters_);
            reader_running_ = false;
        }
        for (auto& kv : remaining) {
            // Wake a pending receive explicitly before closing the sink.
            // Some Asio versions do not cancel a receive solely from close().
            kv.second->try_send(
                asio::error_code(asio::error::operation_aborted, asio::system_category()),
                std::shared_ptr<json>{});
            kv.second->close();
        }
        if (hardened_) shutdown();
    }
    co_return;
}

// Public entry: hop onto the session's OWN io_context and do the exchange
// there.
//
// Everything asio in this session — the pipe descriptors, the write lock, the
// reader coroutine — is bound to an executor on first use, and used again on
// every later call. Binding that to the caller's executor was the bug: the graph
// engine's `run_sync` stands up an io_context for one call and destroys it on
// the way out, so the second run (or the client's destructor) touched asio state
// hanging off a destroyed io_context. In C++, with no sanitizer, that is a core
// dump; see MCPStdioInGraph.TheSameClientSurvivesASecondRun.
//
// The session owns the io_context now, so its lifetime is the session's, and the
// old precondition — "callers must ensure the io_context outlives the session" —
// is gone rather than merely documented. It was never a precondition an engine
// could honour.
asio::awaitable<json> StdioSession::exchange(
    json request, std::chrono::steady_clock::time_point deadline,
    std::shared_ptr<graph::CancelToken> cancel_token) {
    check_open();
    deadline = std::min(deadline, std::chrono::steady_clock::now() + request_timeout_);
    if (hardened_ && !startup_complete_.load())
        deadline = std::min(deadline, startup_deadline_);
    using asio::experimental::awaitable_operators::operator||;
    try {
        if ((cancel_token && cancel_token->is_cancelled())
            || deadline <= std::chrono::steady_clock::now())
            throw_rpc_bound(deadline, cancel_token);
        auto result = co_await (
            capture_result(worker_.submit(do_exchange(std::move(request))))
            || wait_for_rpc_bound(deadline, cancel_token));
        if (result.index() == 1) throw_rpc_bound(deadline, cancel_token);
        auto captured = std::get<0>(std::move(result));
        if (captured.error) std::rethrow_exception(captured.error);
        check_open();
        co_return std::move(*captured.value);
    } catch (const std::system_error& e) {
        const bool stopped = worker_.stopped.load();
        if (hardened_) shutdown();
        throw MCPTransportError(stopped || e.code() == asio::error::operation_aborted
                                    ? MCPFailure::shutdown : MCPFailure::connection,
                                std::string("MCP stdio transport: ") + e.what());
    } catch (...) {
        if (hardened_) shutdown();
        throw;
    }
}

asio::awaitable<json> StdioSession::do_exchange(json request) {
    check_open();
    const int id = request.at("id").get<int>();
    auto ex = co_await asio::this_coro::executor;

    // Lazy-init the WRITE lock on first call (capacity-1 channel = binary
    // semaphore). Unlike the pre-demux design it is NOT held across the
    // round trip — only around the frame write below — so concurrent
    // calls overlap their reads. std::mutex guards only the one-time init.
    {
        std::lock_guard<std::mutex> g(async_lock_init_mtx_);
        if (!async_lock_) {
            async_lock_ = std::make_unique<AsyncLock>(ex, 1);
            // Seed with the initial token so the first writer takes it
            // without blocking.
            async_lock_->try_send(asio::error_code{});
        }
    }

    // Cached AsyncHandle wrappers. See member-field comments for why
    // we bind once per session instead of per call. Lazy-init under a
    // mutex so concurrent first-callers on the same session don't
    // double-bind; subsequent calls take the fast path with no lock.
    if (!async_in_ || !async_out_) {
        std::lock_guard<std::mutex> g(async_handles_init_mtx_);
        if (!async_in_ || !async_out_) {
#ifdef _WIN32
            HANDLE dup_in = nullptr, dup_out = nullptr;
            const HANDLE self = GetCurrentProcess();
            if (!DuplicateHandle(self, stdin_h_, self, &dup_in,
                                 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                throw std::system_error(static_cast<int>(GetLastError()),
                    std::system_category(), "DuplicateHandle(stdin)");
            }
            if (!DuplicateHandle(self, stdout_h_, self, &dup_out,
                                 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                DWORD err = GetLastError();
                CloseHandle(dup_in);
                throw std::system_error(static_cast<int>(err),
                    std::system_category(), "DuplicateHandle(stdout)");
            }
            try {
                async_in_  = std::make_unique<AsyncHandle>(ex, dup_in);
                async_out_ = std::make_unique<AsyncHandle>(ex, dup_out);
            } catch (...) {
                // On partial success, close whichever dup the failed
                // wrapper didn't take. Wrappers that succeeded already
                // own their dup and will close on reset/destruction.
                if (!async_in_)  CloseHandle(dup_in);
                if (!async_out_) CloseHandle(dup_out);
                async_in_.reset();
                async_out_.reset();
                throw;
            }
#else
            int dup_in  = ::dup(stdin_fd_);
            if (dup_in < 0) {
                throw std::system_error(errno, std::system_category(),
                    "dup(stdin_fd)");
            }
            int dup_out = ::dup(stdout_fd_);
            if (dup_out < 0) {
                int err = errno;
                ::close(dup_in);
                throw std::system_error(err, std::system_category(),
                    "dup(stdout_fd)");
            }
            try {
                async_in_  = std::make_unique<AsyncHandle>(ex, dup_in);
                async_out_ = std::make_unique<AsyncHandle>(ex, dup_out);
            } catch (...) {
                if (!async_in_)  ::close(dup_in);
                if (!async_out_) ::close(dup_out);
                async_in_.reset();
                async_out_.reset();
                throw;
            }
#endif
        }
    }

    // Register this call's response sink and make sure the single demux
    // reader is running. Registration happens AFTER the handles are
    // bound above, so the reader never dereferences a null async_out_.
    auto chan = std::make_shared<RespChan>(ex, 1);
    bool start_reader = false;
    {
        std::lock_guard<std::mutex> lk(demux_mu_);
        waiters_[id] = chan;
        if (!reader_running_) {
            reader_failure_ = nullptr;
            reader_running_ = true;
            start_reader = true;
        }
    }
    if (start_reader) {
        asio::co_spawn(ex, run_reader(), asio::detached);
    }

    // Drop our waiter on every exit path (delivered, threw, cancelled).
    // Erasing an already-served id is a harmless no-op.
    struct WaiterGuard {
        StdioSession* self;
        int           id;
        ~WaiterGuard() {
            std::lock_guard<std::mutex> lk(self->demux_mu_);
            self->waiters_.erase(id);
        }
    } wguard{this, id};

    // Serialise ONLY the frame write (held microseconds), releasing the
    // write lock before we await the response.
    co_await async_lock_->async_receive(asio::use_awaitable);
    {
        struct WriteReleaser {
            AsyncLock* ch;
            ~WriteReleaser() {
                try { ch->try_send(asio::error_code{}); } catch (...) {}
            }
        } wrel{async_lock_.get()};
        co_await async_write_frame_locked(*async_in_, request);
    }

    // Await our response. use_awaitable turns a closed sink (session torn
    // down / server gone) into a thrown system_error.
    std::shared_ptr<json> respptr;
    try {
        respptr = co_await chan->async_receive(asio::use_awaitable);
    } catch (const std::system_error&) {
        std::exception_ptr fail;
        {
            std::lock_guard<std::mutex> lk(demux_mu_);
            fail = reader_failure_;
        }
        if (fail) std::rethrow_exception(fail);
        throw;
    }

    json response = std::move(*respptr);
    co_return response;
}

asio::awaitable<void> StdioSession::notify(json notification) {
    check_open();
    using asio::experimental::awaitable_operators::operator||;
    const auto deadline = hardened_ && !startup_complete_.load()
        ? std::min(startup_deadline_, std::chrono::steady_clock::now() + request_timeout_)
        : std::chrono::steady_clock::now() + request_timeout_;
    try {
        auto result = co_await (
            capture_result(worker_.submit(notification_result(do_notify(std::move(notification)))))
            || wait_for_rpc_bound(deadline, {}));
        if (result.index() == 1) throw_rpc_bound(deadline, {});
        auto captured = std::get<0>(std::move(result));
        if (captured.error) std::rethrow_exception(captured.error);
        check_open();
        startup_complete_.store(true);
    } catch (const std::system_error& e) {
        const bool stopped = worker_.stopped.load();
        if (hardened_) shutdown();
        throw MCPTransportError(stopped ? MCPFailure::shutdown : MCPFailure::connection,
                                std::string("MCP stdio notification: ") + e.what());
    } catch (...) {
        if (hardened_) shutdown();
        throw;
    }
}

asio::awaitable<void> StdioSession::do_notify(json notification) {
    check_open();
    // initialize's request created the handles and write semaphore. Notifications
    // use that SAME write lock, so an adjacent tool call cannot interleave frames.
    co_await async_lock_->async_receive(asio::use_awaitable);
    struct Release {
        AsyncLock* lock;
        ~Release() { lock->try_send(asio::error_code{}); }
    } release{async_lock_.get()};
    co_await async_write_frame_locked(*async_in_, notification);
}

} // namespace detail

// ===========================================================================
// Shared helper — shape an MCP tools/call result into a string for the LLM.
// ===========================================================================
namespace {
bool schema_type_matches(const json& value, const std::string& type) {
    if (type == "null") return value.is_null();
    if (type == "boolean") return value.is_boolean();
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "number") return value.is_number();
    if (type == "integer") return value.is_number_integer();
    if (type == "string") return value.is_string();
    throw std::invalid_argument("unsupported JSON Schema type: " + type);
}

void validate_schema_value(const json& value, const json& schema,
                           const std::string& path) {
    if (!schema.is_object()) {
        throw std::runtime_error("MCP outputSchema at " + path
                                 + " must be an object");
    }
    if (schema.contains("const") && value != schema["const"]) {
        throw std::runtime_error("MCP structuredContent at " + path
                                 + " does not match const");
    }
    if (schema.contains("enum") && schema["enum"].is_array()) {
        bool matched = false;
        for (const auto& candidate : schema["enum"]) {
            if (value == candidate) { matched = true; break; }
        }
        if (!matched) {
            throw std::runtime_error("MCP structuredContent at " + path
                                     + " is not in enum");
        }
    }
    if (schema.contains("type")) {
        bool matched = false;
        if (schema["type"].is_string()) {
            matched = schema_type_matches(value, schema["type"].get<std::string>());
        } else if (schema["type"].is_array()) {
            for (const auto& type : schema["type"]) {
                if (type.is_string()
                    && schema_type_matches(value, type.get<std::string>())) {
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            throw std::runtime_error("MCP structuredContent at " + path
                                     + " has the wrong JSON type");
        }
    }
    if (value.is_object()) {
        if (schema.contains("required") && schema["required"].is_array()) {
            for (const auto& name : schema["required"]) {
                if (name.is_string() && !value.contains(name.get<std::string>())) {
                    throw std::runtime_error("MCP structuredContent at " + path
                                             + " is missing required property "
                                             + name.get<std::string>());
                }
            }
        }
        const json properties = schema.value("properties", json::object());
        if (properties.is_object()) {
            for (auto it = properties.begin(); it != properties.end(); ++it) {
                if (value.contains(it.key())) {
                    validate_schema_value(value[it.key()], it.value(),
                                          path + "/" + it.key());
                }
            }
        }
        if (schema.value("additionalProperties", true) == false
            && properties.is_object()) {
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (!properties.contains(it.key())) {
                    throw std::runtime_error("MCP structuredContent at " + path
                                             + " has unexpected property " + it.key());
                }
            }
        }
    }
    if (value.is_array() && schema.contains("items")
        && schema["items"].is_object()) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            validate_schema_value(value[i], schema["items"],
                                  path + "/" + std::to_string(i));
        }
    }
}

void validate_tool_result(const CallToolResult& result,
                          const json& output_schema) {
    if (output_schema.is_null()) return;
    if (result.structured_content.is_null()) {
        throw std::runtime_error(
            "MCP tool advertised outputSchema but returned no structuredContent");
    }
    validate_schema_value(result.structured_content, output_schema, "$ ");
}

std::string format_tool_result(const CallToolResult& result) {
    if (result.content.is_array()) {
        std::string output;
        for (const auto& item : result.content) {
            if (item.value("type", "") == "text") {
                if (!output.empty()) output += "\n";
                output += item.value("text", "");
            }
        }
        if (!output.empty()) return output;
    }
    if (!result.structured_content.is_null()) {
        return result.structured_content.dump();
    }
    if (!result.content.empty()) return result.content.dump();
    return result.raw.dump();
}

ToolDefinition legacy_definition(const std::string& name,
                                 const std::string& description,
                                 const json& input_schema) {
    ToolDefinition definition;
    definition.name = name;
    definition.description = description;
    definition.input_schema = input_schema;
    definition.raw = {
        {"name", name},
        {"description", description},
        {"inputSchema", input_schema},
    };
    return definition;
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
  , definition_(legacy_definition(name, description, input_schema))
{
}

MCPTool::MCPTool(std::shared_ptr<detail::ProtocolSession> session,
                 ToolDefinition definition)
  : session_(std::move(session))
  , definition_(std::move(definition))
{
}

ChatTool MCPTool::get_definition() const {
    return { definition_.name, definition_.description,
             definition_.input_schema };
}

CallToolResult MCPTool::execute_result(const json& arguments) {
    return async::run_sync(execute_result_async(arguments));
}

asio::awaitable<CallToolResult>
MCPTool::execute_result_async(const json& arguments) {
    if (auto session = session_) {
        auto result = co_await session->call_tool_result_async(
            definition_.name, arguments);
        validate_tool_result(result, definition_.output_schema);
        co_return result;
    }

    // The legacy direct-HTTP tool constructor has no originating client.
    // Create a fresh protocol session for this invocation.
    auto client = std::make_unique<MCPClient>(server_url_);
    auto result = co_await client->call_tool_result_async(
        definition_.name, arguments);
    validate_tool_result(result, definition_.output_schema);
    co_return result;
}

asio::awaitable<std::string> MCPTool::execute_async(const json& arguments) {
    auto result = co_await execute_result_async(arguments);
    std::string text = format_tool_result(result);
    if (result.is_error) {
        throw std::runtime_error("MCP tool execution error: " + text);
    }
    co_return text;
}

// The public client is only a facade over one protocol session. Tools retain
// that same session, so neither adaptation nor graph dispatch knows the
// transport type.
MCPClient::MCPClient(const std::string& server_url)
  : MCPClient(server_url, MCPClientConfig{}) {}

MCPClient::MCPClient(const std::string& server_url, MCPClientConfig config)
  : session_(std::make_shared<detail::ProtocolSession>(
        std::make_shared<detail::HttpSession>(server_url, std::move(config)))) {}

MCPClient::MCPClient(std::vector<std::string> argv)
  : session_(std::make_shared<detail::ProtocolSession>(
        detail::StdioSession::spawn(argv))) {}
MCPClient::MCPClient(StdioClientConfig config)
  : session_(std::make_shared<detail::ProtocolSession>(
        detail::StdioSession::spawn(std::move(config)))) {}

void MCPClient::shutdown() noexcept { session_->shutdown(); }

void detail::ProtocolSession::shutdown() noexcept {
    stopped_.store(true);
    {
        std::lock_guard lk(metadata_->mu);
        metadata_->lifecycle = ClientMetadata::Lifecycle::shutdown;
    }
    metadata_->changed.notify_all();
    transport_->shutdown();
}


asio::awaitable<json> MCPClient::rpc_call_async(
    const std::string& method, const json& params,
    std::chrono::steady_clock::time_point deadline,
    std::shared_ptr<graph::CancelToken> cancel_token) {
    auto session = session_;
    co_return co_await session->rpc_call_async(
        method, params, deadline, std::move(cancel_token));
}

json detail::ProtocolSession::rpc_call(
    const std::string& method, const json& params) {
    return async::run_sync(rpc_call_async(method, params));
}

asio::awaitable<json> detail::ProtocolSession::rpc_call_async(
    const std::string& method, const json& params,
    std::chrono::steady_clock::time_point deadline,
    std::shared_ptr<graph::CancelToken> cancel_token) {
    if (stopped_.load()) throw_shutdown();
    if ((cancel_token && cancel_token->is_cancelled())
        || deadline <= std::chrono::steady_clock::now()) {
        throw_rpc_bound(deadline, cancel_token);
    }
    const int id = ++next_id_;
    json request{{"jsonrpc", "2.0"}, {"id", id},
                 {"method", method}, {"params", params}};
    auto transport = transport_;
    auto response = co_await transport->exchange(
        std::move(request), deadline, std::move(cancel_token));
    if (stopped_.load()) throw_shutdown();
    try {
        co_return extract_rpc_result(response, id);
    } catch (const MCPTransportError&) {
        transport->reset();
        throw;
    }
}

asio::awaitable<json> detail::HttpSession::exchange(
    json request, std::chrono::steady_clock::time_point deadline,
    std::shared_ptr<graph::CancelToken> cancel_token) {
    co_return co_await worker.submit(
        do_exchange(std::move(request), deadline, std::move(cancel_token)));
}

asio::awaitable<json> detail::HttpSession::do_exchange(
    json request, std::chrono::steady_clock::time_point deadline,
    std::shared_ptr<graph::CancelToken> cancel_token) {
    if (worker.stopped.load()) throw_shutdown();
    using asio::experimental::awaitable_operators::operator||;
    const int this_id = request.at("id").get<int>();
    std::vector<std::pair<std::string, std::string>> headers;
    HeaderProvider header_provider;
    {
        std::lock_guard lk(mu);
        headers = {
            {"Content-Type", "application/json"},
            {"Accept",       "application/json, text/event-stream"},
        };
        headers.insert(headers.end(), config.headers.begin(),
                       config.headers.end());
        header_provider = config.header_provider;
        if (!session_id.empty()) {
            headers.emplace_back("Mcp-Session-Id", session_id);
        }
        // Spec MUST (transports / Streamable HTTP § "Protocol Version
        // Header"): include MCP-Protocol-Version on every HTTP request
        // after initialize. Strict 2025-11-25 servers respond 400 Bad
        // Request without it. Skip on the initialize call itself —
        // negotiated_protocol_version_ is empty until initialize returns.
        if (!protocol_version.empty()) {
            headers.emplace_back("MCP-Protocol-Version", protocol_version);
        }
    }
    // User callbacks may refresh credentials or re-enter application code. Run
    // them after releasing the session mutex so they cannot deadlock the client.
    if (header_provider) {
        auto dynamic_headers = header_provider();
        headers.insert(headers.end(), dynamic_headers.begin(),
                       dynamic_headers.end());
    }
    if (worker.stopped.load()) throw_shutdown();
    for (const auto& [name, value] : headers) {
        if (name.empty() || name.find_first_of("\r\n") != std::string::npos
            || value.find_first_of("\r\n") != std::string::npos) {
            throw std::invalid_argument("MCP header contains CR/LF or an empty name");
        }
    }

    auto body_str = request.dump();

    async::RequestOptions opts;
    opts.timeout = config.request_timeout;
    if (deadline != std::chrono::steady_clock::time_point::max()) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) throw_rpc_bound(deadline, cancel_token);
        if (opts.timeout.count() <= 0 || remaining < opts.timeout) opts.timeout = remaining;
    }

    auto ex = co_await asio::this_coro::executor;
    async::HttpResponse res;
    try {
        auto result = co_await (
            capture_result(async::async_post(ex, endpoint.host, endpoint.port,
                                             path, body_str, std::move(headers),
                                             endpoint.tls, opts))
            || wait_for_rpc_bound(deadline, cancel_token, shutdown_token));
        if (result.index() == 1) throw_rpc_bound(deadline, cancel_token, shutdown_token);
        auto captured = std::get<0>(std::move(result));
        if (captured.error) std::rethrow_exception(captured.error);
        if (worker.stopped.load()) throw_shutdown();
        res = std::move(*captured.value);
    } catch (const std::system_error& e) {
        const auto failure = e.code() == asio::error::timed_out
            ? MCPFailure::timeout : (e.code() == asio::error::operation_aborted
                ? MCPFailure::shutdown : MCPFailure::connection);
        throw MCPTransportError(failure, std::string("MCP HTTP request failed: ") + e.what());
    }

    // Absorb response state under the mutex — `Mcp-Session-Id` may
    // change mid-conversation if the server rotates it. The MCP spec
    // sends this header on the initialize response; subsequent rpc
    // calls must echo it back so the server routes to the same session.
    if (auto sid = res.get_header("Mcp-Session-Id"); !sid.empty()) {
        std::lock_guard lk(mu);
        session_id = std::string(sid);
    }

    if (res.status != 200) {
        std::string scheme = endpoint.tls ? "https://" : "http://";
        std::string full_url =
            scheme + endpoint.host + ":" + endpoint.port + path;
        std::string hint;
        if (res.status == 404) {
            hint = " — the server has no MCP endpoint at this path. Check the "
                   "configured URL (a trailing '/mcp' is added automatically, "
                   "so pass the server base like 'http://host:8000' or the "
                   "full 'http://host:8000/mcp').";
        }
        throw MCPTransportError(
            MCPFailure::http_status,
            "MCP error (HTTP " + std::to_string(res.status) + ") for " +
                full_url + ": " + res.body + hint, res.status);
    }

    // Parse response — may be plain JSON or SSE. Streamable HTTP can
    // pack multiple SSE events into one response (e.g. an in-stream
    // server→client request followed by the JSON-RPC response). Walk
    // the events and pick the first one whose payload's `id` matches
    // our request. Server requests and notifications remain unsupported,
    // but cannot be mistaken for this call's response.
    // This replaces a previous implementation that grabbed only the
    // first `data:` line and dropped any subsequent events.
    json resp;
    const auto response_content_type = std::string(res.get_header("Content-Type"));
    if (response_content_type.find("text/event-stream") != std::string::npos) {
        // Parse SSE event stream: events separated by "\n\n", lines
        // within an event starting with "data:" are concatenated with
        // newlines per the W3C SSE spec.
        json matched;
        size_t pos = 0;
        while (pos < res.body.size()) {
            auto event_end = res.body.find("\n\n", pos);
            std::string event = (event_end == std::string::npos)
                                ? res.body.substr(pos)
                                : res.body.substr(pos, event_end - pos);
            pos = (event_end == std::string::npos)
                  ? res.body.size()
                  : event_end + 2;

            std::string data;
            size_t lp = 0;
            while (lp < event.size()) {
                auto nl = event.find('\n', lp);
                std::string line = (nl == std::string::npos)
                                   ? event.substr(lp)
                                   : event.substr(lp, nl - lp);
                lp = (nl == std::string::npos) ? event.size() : nl + 1;
                if (line.rfind("data:", 0) == 0) {
                    auto value = line.substr(5);
                    if (!value.empty() && value.front() == ' ') value.erase(0, 1);
                    if (!data.empty()) data.push_back('\n');
                    data.append(value);
                }
            }
            if (data.empty()) continue;
            try {
                json frame = json::parse(data);
                if (frame.contains("id")
                    && frame["id"].is_number_integer()
                    && frame["id"].get<int>() == this_id) {
                    matched = std::move(frame);
                    break;
                }
            } catch (...) {
                // Tolerate keep-alive / comment events; skip.
            }
        }
        if (!matched.is_null()) {
            resp = std::move(matched);
        } else {
            throw MCPTransportError(MCPFailure::protocol,
                "MCP SSE response had no JSON-RPC response matching the request id");
        }
    } else {
        try {
            resp = json::parse(res.body);
        } catch (const json::parse_error& e) {
            throw MCPTransportError(MCPFailure::protocol,
                                    std::string("MCP HTTP response JSON: ") + e.what());
        }
    }

    co_return resp;
}

namespace {

template <typename T>
T decode_protocol_result(const json& raw) {
    try {
        return T::from_json(raw);
    } catch (const std::exception& e) {
        throw MCPTransportError(MCPFailure::protocol,
                                std::string("MCP invalid result: ") + e.what());
    }
}

json initialize_params(const std::string& client_name) {
    json params;
    params["protocolVersion"] = "2025-11-25";
    params["capabilities"]    = json::object();
    params["clientInfo"]      = {{"name", client_name}, {"version", "0.1.0"}};
    return params;
}

void store_initialize_result(
    const std::shared_ptr<detail::ClientMetadata>& metadata,
    InitializeResult result) {
    {
        std::lock_guard lk(metadata->mu);
        if (metadata->lifecycle == detail::ClientMetadata::Lifecycle::shutdown)
            throw_shutdown();
        metadata->initialize_result = std::move(result);
        metadata->lifecycle = detail::ClientMetadata::Lifecycle::initialized;
    }
    metadata->changed.notify_all();
}

void reset_initialization(
    const std::shared_ptr<detail::Transport>& transport,
    const std::shared_ptr<detail::ClientMetadata>& metadata) {
    transport->reset();
    {
        std::lock_guard lk(metadata->mu);
        if (metadata->lifecycle != detail::ClientMetadata::Lifecycle::shutdown)
            metadata->lifecycle = detail::ClientMetadata::Lifecycle::created;
        metadata->initialize_result = {};
        metadata->output_schemas.clear();
    }
    metadata->changed.notify_all();
}

HeaderList notification_headers(detail::HttpSession& session) {
    HeaderList headers = {
        {"Content-Type", "application/json"},
        {"Accept", "application/json, text/event-stream"},
    };
    HeaderProvider provider;
    {
        std::lock_guard lk(session.mu);
        headers.insert(headers.end(), session.config.headers.begin(),
                       session.config.headers.end());
        provider = session.config.header_provider;
        if (!session.session_id.empty()) {
            headers.emplace_back("Mcp-Session-Id", session.session_id);
        }
        if (!session.protocol_version.empty()) {
            headers.emplace_back("MCP-Protocol-Version", session.protocol_version);
        }
    }
    if (provider) {
        auto dynamic_headers = provider();
        headers.insert(headers.end(), dynamic_headers.begin(), dynamic_headers.end());
    }
    for (const auto& [name, value] : headers) {
        if (name.empty() || name.find_first_of("\r\n") != std::string::npos
            || value.find_first_of("\r\n") != std::string::npos) {
            throw std::invalid_argument("MCP header contains CR/LF or an empty name");
        }
    }
    return headers;
}

} // namespace

asio::awaitable<void> detail::HttpSession::notify(json notification) {
    co_await worker.submit(notification_result(do_notify(std::move(notification))));
}

asio::awaitable<void> detail::HttpSession::do_notify(json notification) {
    if (worker.stopped.load()) throw_shutdown();
    using asio::experimental::awaitable_operators::operator||;
    async::RequestOptions opts;
    opts.timeout = config.request_timeout;
    auto ex = co_await asio::this_coro::executor;
    async::HttpResponse res;
    try {
        auto headers = notification_headers(*this);
        if (worker.stopped.load()) throw_shutdown();
        auto result = co_await (
            capture_result(async::async_post(ex, endpoint.host, endpoint.port, path,
                                             notification.dump(), std::move(headers),
                                             endpoint.tls, opts))
            || wait_for_rpc_bound(std::chrono::steady_clock::time_point::max(),
                                  {}, shutdown_token));
        if (result.index() == 1) throw_shutdown();
        auto captured = std::get<0>(std::move(result));
        if (captured.error) std::rethrow_exception(captured.error);
        if (worker.stopped.load()) throw_shutdown();
        res = std::move(*captured.value);
    } catch (const std::system_error& e) {
        throw MCPTransportError(e.code() == asio::error::timed_out
                                    ? MCPFailure::timeout : MCPFailure::connection,
                                std::string("MCP HTTP notification: ") + e.what());
    }
    if (res.status < 200 || res.status >= 300) {
        throw MCPTransportError(MCPFailure::http_status,
            "MCP initialize notification returned HTTP "
            + std::to_string(res.status) + ": " + res.body, res.status);
    }
}


bool MCPClient::initialize(const std::string& client_name) {
    return session_->initialize(client_name);
}

asio::awaitable<bool> MCPClient::initialize_async(const std::string& client_name) {
    auto session = session_;
    co_return co_await session->initialize_async(client_name);
}

bool detail::ProtocolSession::initialize(const std::string& client_name) {
    return async::run_sync(initialize_async(client_name));
}

asio::awaitable<bool> detail::ProtocolSession::initialize_async(
    const std::string& client_name) {
    // Only one caller performs the handshake; concurrent callers wait for the
    // same negotiated session. A failure resets both protocol and transport.
    for (;;) {
        if (stopped_.load()) throw_shutdown();
        {
            std::lock_guard lk(metadata_->mu);
            if (metadata_->lifecycle
                == detail::ClientMetadata::Lifecycle::initialized) {
                co_return true;
            }
            if (metadata_->lifecycle
                == detail::ClientMetadata::Lifecycle::created) {
                metadata_->lifecycle =
                    detail::ClientMetadata::Lifecycle::initializing;
                break;
            }
        }
        asio::steady_timer timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }

    try {
        auto raw = co_await rpc_call_async(
            "initialize", initialize_params(client_name));
        auto result = decode_protocol_result<InitializeResult>(raw);
        transport_->negotiated_version(result.protocol_version);
        json notification = {
            {"jsonrpc", "2.0"}, {"method", "notifications/initialized"},
            {"params", json::object()},
        };
        co_await transport_->notify(std::move(notification));
        store_initialize_result(metadata_, std::move(result));
        co_return true;
    } catch (...) {
        reset_initialization(transport_, metadata_);
        throw;
    }
}

bool detail::ProtocolSession::is_initialized() const noexcept {
    std::lock_guard lk(metadata_->mu);
    return metadata_->lifecycle
        == detail::ClientMetadata::Lifecycle::initialized;
}

InitializeResult detail::ProtocolSession::get_initialize_result() const {
    std::lock_guard lk(metadata_->mu);
    if (metadata_->lifecycle
        != detail::ClientMetadata::Lifecycle::initialized) {
        throw std::logic_error("MCP client has not been initialized");
    }
    return metadata_->initialize_result;
}

std::vector<std::unique_ptr<Tool>> MCPClient::get_tools() {
    auto definitions = session_->get_tool_definitions();
    std::vector<std::unique_ptr<Tool>> tools;
    tools.reserve(definitions.size());
    for (auto& definition : definitions) {
        tools.push_back(std::unique_ptr<Tool>(
            new MCPTool(session_, std::move(definition))));
    }
    return tools;
}

ListToolsPage detail::ProtocolSession::list_tools(
    const std::optional<std::string>& cursor) {
    initialize();
    json params = json::object();
    if (cursor) params["cursor"] = *cursor;
    auto page = decode_protocol_result<ListToolsPage>(rpc_call("tools/list", params));
    {
        std::lock_guard lk(metadata_->mu);
        for (const auto& tool : page.tools) {
            metadata_->output_schemas[tool.name] = tool.output_schema;
        }
    }
    return page;
}

asio::awaitable<ListToolsPage> detail::ProtocolSession::list_tools_async(
    const std::optional<std::string>& cursor) {
    co_await initialize_async();
    json params = json::object();
    if (cursor) params["cursor"] = *cursor;
    auto raw = co_await rpc_call_async("tools/list", params);
    auto page = decode_protocol_result<ListToolsPage>(raw);
    {
        std::lock_guard lk(metadata_->mu);
        for (const auto& tool : page.tools) {
            metadata_->output_schemas[tool.name] = tool.output_schema;
        }
    }
    co_return page;
}

std::vector<ToolDefinition> detail::ProtocolSession::get_tool_definitions() {
    std::vector<ToolDefinition> definitions;
    std::optional<std::string> cursor;
    do {
        auto page = list_tools(cursor);
        definitions.insert(definitions.end(),
                           std::make_move_iterator(page.tools.begin()),
                           std::make_move_iterator(page.tools.end()));
        if (page.next_cursor == cursor && cursor) {
            throw std::runtime_error(
                "MCP tools/list returned the same nextCursor repeatedly");
        }
        cursor = std::move(page.next_cursor);
    } while (cursor);
    return definitions;
}

json detail::ProtocolSession::call_tool(const std::string& name, const json& arguments) {
    initialize();
    json params;
    params["name"]      = name;
    params["arguments"] = arguments;
    return rpc_call("tools/call", params);
}

CallToolResult detail::ProtocolSession::call_tool_result(
    const std::string& name, const json& arguments) {
    auto result = decode_protocol_result<CallToolResult>(call_tool(name, arguments));
    json output_schema;
    bool has_output_schema = false;
    {
        std::lock_guard lk(metadata_->mu);
        auto it = metadata_->output_schemas.find(name);
        if (it != metadata_->output_schemas.end()) {
            output_schema = it->second;
            has_output_schema = true;
        }
    }
    if (has_output_schema) validate_tool_result(result, output_schema);
    return result;
}

asio::awaitable<json>
detail::ProtocolSession::call_tool_async(const std::string& name, const json& arguments) {
    co_await initialize_async();
    json params;
    params["name"]      = name;
    params["arguments"] = arguments;
    co_return co_await rpc_call_async("tools/call", params);
}

asio::awaitable<CallToolResult> detail::ProtocolSession::call_tool_result_async(
    const std::string& name, const json& arguments) {
    auto raw = co_await call_tool_async(name, arguments);
    auto result = decode_protocol_result<CallToolResult>(raw);
    json output_schema;
    bool has_output_schema = false;
    {
        std::lock_guard lk(metadata_->mu);
        auto it = metadata_->output_schemas.find(name);
        if (it != metadata_->output_schemas.end()) {
            output_schema = it->second;
            has_output_schema = true;
        }
    }
    if (has_output_schema) validate_tool_result(result, output_schema);
    co_return result;
}

bool MCPClient::is_initialized() const noexcept {
    return session_->is_initialized();
}

InitializeResult MCPClient::get_initialize_result() const {
    return session_->get_initialize_result();
}

ListToolsPage MCPClient::list_tools(const std::optional<std::string>& cursor) {
    return session_->list_tools(cursor);
}

asio::awaitable<ListToolsPage> MCPClient::list_tools_async(
    const std::optional<std::string>& cursor) {
    auto session = session_;
    co_return co_await session->list_tools_async(cursor);
}

std::vector<ToolDefinition> MCPClient::get_tool_definitions() {
    return session_->get_tool_definitions();
}

json MCPClient::call_tool(const std::string& name, const json& arguments) {
    return session_->call_tool(name, arguments);
}

CallToolResult MCPClient::call_tool_result(
    const std::string& name, const json& arguments) {
    return session_->call_tool_result(name, arguments);
}

asio::awaitable<json> MCPClient::call_tool_async(
    const std::string& name, const json& arguments) {
    auto session = session_;
    co_return co_await session->call_tool_async(name, arguments);
}

asio::awaitable<CallToolResult> MCPClient::call_tool_result_async(
    const std::string& name, const json& arguments) {
    auto session = session_;
    co_return co_await session->call_tool_result_async(name, arguments);
}

} // namespace neograph::mcp
