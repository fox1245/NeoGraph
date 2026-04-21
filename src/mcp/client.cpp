#include <neograph/mcp/client.h>

#include <neograph/async/endpoint.h>
#include <neograph/async/http_client.h>
#include <neograph/async/run_sync.h>

#include <asio/experimental/channel.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#ifdef _WIN32
#  include <asio/windows/stream_handle.hpp>
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <asio/posix/stream_descriptor.hpp>
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <istream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace neograph::mcp {

// ===========================================================================
// detail::StdioSession — subprocess-backed JSON-RPC channel
// ===========================================================================
//
// Platform-split implementation:
//   POSIX: fork+execvp spawn, anonymous pipe fds, asio::posix::stream_descriptor.
//   Win32: CreateProcess spawn, named pipe with FILE_FLAG_OVERLAPPED,
//          asio::windows::stream_handle.
//
// The public surface (spawn / rpc_call / rpc_call_async / notify) is
// identical; members diverge where the handle type does. Members are
// commented by platform below.
namespace detail {

#ifdef _WIN32
using NativeHandle = HANDLE;
using AsyncHandle  = asio::windows::stream_handle;
#else
using NativeHandle = int;
using AsyncHandle  = asio::posix::stream_descriptor;
#endif

class StdioSession {
public:
    static std::shared_ptr<StdioSession> spawn(const std::vector<std::string>& argv);

    ~StdioSession();

    // Send a JSON-RPC request, block until the matching response arrives.
    json rpc_call(const std::string& method, const json& params);

    /// Async variant — wraps the subprocess pipes in
    /// asio::posix::stream_descriptor for non-blocking I/O. The
    /// awaitable serialises concurrent rpc_call_async() invocations
    /// on the same session via an asio::experimental::channel-backed
    /// lock (capacity 1, token-passing). A second coroutine's
    /// acquire suspends at its own `co_await async_receive` — no OS
    /// thread is held, so other coroutines on the io_context keep
    /// progressing and the first call's I/O completions fire normally.
    ///
    /// The lock is lazy-initialised on first call using the caller's
    /// executor. Subsequent calls on executors that can't interoperate
    /// with that executor would break the serialisation — in practice
    /// all callers of one session go through one engine and thus one
    /// io_context, so this isn't a concern.
    ///
    /// Sync rpc_call() continues to use the std::mutex mtx_ and must
    /// NOT be mixed with rpc_call_async on the same session (the two
    /// locks don't know about each other).
    asio::awaitable<json> rpc_call_async(
        const std::string& method, const json& params);

    // Send a JSON-RPC notification (no id, no response expected).
    void notify(const std::string& method, const json& params);

private:
    StdioSession() = default;

    std::string read_line_locked();       ///< caller holds mtx_
    void write_frame_locked(const json& j); ///< caller holds mtx_

    asio::awaitable<std::string> async_read_line_locked(AsyncHandle& out);
    asio::awaitable<void> async_write_frame_locked(AsyncHandle& in, const json& j);

#ifdef _WIN32
    HANDLE process_ = nullptr;   ///< child process handle (CloseHandle on dtor)
    HANDLE stdin_h_ = nullptr;   ///< parent → child (write end of a pipe)
    HANDLE stdout_h_ = nullptr;  ///< child → parent (read end of a pipe)
#else
    pid_t pid_ = -1;
    int   stdin_fd_ = -1;   // parent → child
    int   stdout_fd_ = -1;  // child  → parent
#endif

    std::mutex   mtx_;        ///< sync path serialisation
    std::string  buffer_;     ///< sync read buffer
    std::string  abuffer_;    ///< async read buffer (separate to avoid mixing)
    std::atomic<int> next_id_{0};

    // Awaitable lock for the async path (Sem 4 follow-up). Capacity-1
    // channel behaves as a binary semaphore: holder takes the token
    // via `async_receive`, releases via `try_send`. Second acquirer
    // suspends cooperatively rather than blocking the worker thread.
    using AsyncLock = asio::experimental::channel<void(asio::error_code)>;
    std::unique_ptr<AsyncLock> async_lock_;
    std::mutex async_lock_init_mtx_;
};

#ifdef _WIN32

namespace {
// Build the Windows command line from an argv vector. CreateProcess
// expects a single string; standard rules are quoting elements that
// contain whitespace or quotes. This is the minimal-sufficient escape
// for the cases the tests exercise (python3 script path).
std::string build_win_cmdline(const std::vector<std::string>& argv) {
    std::string out;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) out.push_back(' ');
        const auto& a = argv[i];
        bool need_quote = a.empty() ||
            a.find_first_of(" \t\"") != std::string::npos;
        if (need_quote) {
            out.push_back('"');
            for (char c : a) {
                if (c == '"') out.push_back('\\');
                out.push_back(c);
            }
            out.push_back('"');
        } else {
            out.append(a);
        }
    }
    return out;
}

// Create a named-pipe pair where the parent side supports
// FILE_FLAG_OVERLAPPED (needed for asio::windows::stream_handle) and
// the child side is a plain inheritable handle. CreatePipe's anonymous
// pipes don't support overlapped I/O, hence the named-pipe dance.
struct PipePair { HANDLE parent; HANDLE child; };
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
    SetHandleInformation(parent, HANDLE_FLAG_INHERIT, 0);
    return PipePair{parent, child};
}
} // namespace

std::shared_ptr<StdioSession> StdioSession::spawn(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        throw std::invalid_argument("StdioSession::spawn: argv is empty");
    }

    // Two pipes: in = parent writes (child reads on stdin),
    //            out = child writes (parent reads on stdout).
    PipePair in_p  = make_overlapped_pipe("in",  /*parent_reads=*/false);
    PipePair out_p;
    try {
        out_p = make_overlapped_pipe("out", /*parent_reads=*/true);
    } catch (...) {
        CloseHandle(in_p.parent);
        CloseHandle(in_p.child);
        throw;
    }

    std::string cmdline = build_win_cmdline(argv);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = in_p.child;
    si.hStdOutput = out_p.child;
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);  // inherit parent's stderr

    PROCESS_INFORMATION pi = {};
    // cmdline.data() is mutable per CreateProcess's contract.
    BOOL ok = CreateProcessA(
        /*application=*/nullptr,
        cmdline.data(),
        /*proc_sa=*/nullptr, /*thr_sa=*/nullptr,
        /*inherit=*/TRUE, /*flags=*/0,
        /*env=*/nullptr, /*cwd=*/nullptr,
        &si, &pi);
    // Child side handles belong to the child now (inherited) — we can
    // close our copies regardless of success.
    CloseHandle(in_p.child);
    CloseHandle(out_p.child);
    if (!ok) {
        DWORD err = GetLastError();
        CloseHandle(in_p.parent);
        CloseHandle(out_p.parent);
        throw std::system_error(static_cast<int>(err),
            std::system_category(), "CreateProcess");
    }
    CloseHandle(pi.hThread);  // thread handle unused

    auto sess = std::shared_ptr<StdioSession>(new StdioSession());
    sess->process_  = pi.hProcess;
    sess->stdin_h_  = in_p.parent;
    sess->stdout_h_ = out_p.parent;
    return sess;
}

StdioSession::~StdioSession() {
    if (stdin_h_)  { CloseHandle(stdin_h_); stdin_h_ = nullptr; }
    if (stdout_h_) { CloseHandle(stdout_h_); stdout_h_ = nullptr; }

    if (process_) {
        // Give the child a moment to exit cleanly on its own (closing
        // its stdin pipe above typically causes a well-behaved server
        // to exit). Fall back to TerminateProcess after ~500ms.
        DWORD wait = WaitForSingleObject(process_, 500);
        if (wait != WAIT_OBJECT_0) {
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 1000);
        }
        CloseHandle(process_);
        process_ = nullptr;
    }
}

#else  // !_WIN32

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

#endif  // _WIN32

void StdioSession::write_frame_locked(const json& j) {
    std::string line = j.dump();
    line.push_back('\n');

    const char* p = line.data();
    size_t      remaining = line.size();
    while (remaining > 0) {
#ifdef _WIN32
        DWORD written = 0;
        // With an overlapped named pipe, WriteFile(hEvent=nullptr) still
        // works synchronously on this thread — it just uses the pipe's
        // internal event. That's fine for the sync path.
        if (!WriteFile(stdin_h_, p, static_cast<DWORD>(remaining),
                       &written, nullptr)) {
            throw std::system_error(static_cast<int>(GetLastError()),
                std::system_category(), "StdioSession::WriteFile()");
        }
        p         += written;
        remaining -= static_cast<size_t>(written);
#else
        ssize_t n = ::write(stdin_fd_, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(),
                                    "StdioSession::write()");
        }
        p         += n;
        remaining -= static_cast<size_t>(n);
#endif
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
#ifdef _WIN32
        DWORD got = 0;
        if (!ReadFile(stdout_h_, tmp, static_cast<DWORD>(sizeof(tmp)),
                      &got, nullptr)) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) {
                throw std::runtime_error(
                    "StdioSession: child closed stdout");
            }
            throw std::system_error(static_cast<int>(err),
                std::system_category(), "StdioSession::ReadFile()");
        }
        if (got == 0) {
            throw std::runtime_error(
                "StdioSession: child closed stdout");
        }
        buffer_.append(tmp, static_cast<size_t>(got));
#else
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
#endif
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
        asio::streambuf sbuf;
        // Seed asio's streambuf with whatever we already have so
        // async_read_until doesn't re-read those bytes.
        if (!abuffer_.empty()) {
            std::ostream os(&sbuf);
            os.write(abuffer_.data(), static_cast<std::streamsize>(abuffer_.size()));
            abuffer_.clear();
        }
        std::size_t n = co_await asio::async_read_until(
            out, sbuf, '\n', asio::use_awaitable);
        // Re-merge into our string buffer so the rest of the trailing
        // bytes (after the newline) are kept for the next call.
        std::string drained(asio::buffers_begin(sbuf.data()),
                            asio::buffers_begin(sbuf.data()) + sbuf.size());
        abuffer_.append(drained);
        nl = abuffer_.find('\n');
        if (nl == std::string::npos) {
            // async_read_until promised a delim was found within `n`
            // bytes; this branch is defensive.
            throw std::runtime_error(
                "StdioSession::async_read_line: delimiter missing after "
                + std::to_string(n) + " bytes");
        }
    }
    std::string line = abuffer_.substr(0, nl);
    abuffer_.erase(0, nl + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    co_return line;
}

asio::awaitable<json>
StdioSession::rpc_call_async(const std::string& method, const json& params) {
    const int id = ++next_id_;

    json req = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", method},
        {"params", params}
    };

    auto ex = co_await asio::this_coro::executor;

    // Lazy-init the awaitable lock on first call. Bound to this
    // caller's executor; subsequent calls assume a compatible executor
    // (in practice: the engine's single io_context — no cross-executor
    // mix for one session). std::mutex guards only the one-time
    // init, not the lock itself.
    {
        std::lock_guard<std::mutex> g(async_lock_init_mtx_);
        if (!async_lock_) {
            async_lock_ = std::make_unique<AsyncLock>(ex, 1);
            // Seed with the initial token — the first acquirer can
            // take it without blocking. try_send returns false if
            // the channel is already full, which we never hit here
            // because we just created it with capacity 1 and empty.
            async_lock_->try_send(asio::error_code{});
        }
    }

    // Acquire the awaitable lock. Second coroutine on the same
    // session suspends here until the holder releases via try_send
    // below — cooperative, no OS-thread block.
    co_await async_lock_->async_receive(asio::use_awaitable);

    // RAII release: put the token back on every exit path, including
    // exception / cancellation. Destructor runs on coroutine frame
    // unwind so no co_await inside — safe under GCC 13's catch/await
    // ICE rules.
    struct LockReleaser {
        AsyncLock* ch;
        ~LockReleaser() {
            // try_send is noexcept-ish (no throw on a healthy
            // channel). We're on the hot unwind path; swallow any
            // residual error rather than abort the program.
            try { ch->try_send(asio::error_code{}); } catch (...) {}
        }
    } lock_rel{async_lock_.get()};

    // Wrap the raw pipe handles in an AsyncHandle (asio::posix::
    // stream_descriptor on POSIX, asio::windows::stream_handle on
    // Win32). Fresh wrappers per call avoid the executor-lifetime
    // issue that hits ConnPool when callers funnel through run_sync
    // (each run_sync uses a fresh io_context that dies on return).
    //
    // POSIX: release() before destruction so the session keeps the fd.
    // Win32: a HANDLE can be associated with at most one IOCP for its
    //        lifetime. Re-wrapping the same HANDLE on a second call
    //        crashes inside the handle_service. Duplicate the HANDLE
    //        per call instead and let the wrapper's destructor close
    //        the duplicate; the session keeps its original.
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
    AsyncHandle in_desc(ex, dup_in);
    AsyncHandle out_desc(ex, dup_out);
    // No release(): wrapper destructor closes dup_in / dup_out, which
    // is what we want — the session's stdin_h_ / stdout_h_ are
    // separate HANDLEs and unaffected.
#else
    AsyncHandle in_desc(ex, stdin_fd_);
    AsyncHandle out_desc(ex, stdout_fd_);
    struct ReleaseGuard {
        AsyncHandle& d;
        ~ReleaseGuard() { try { d.release(); } catch (...) {} }
    } in_guard{in_desc}, out_guard{out_desc};
#endif

    co_await async_write_frame_locked(in_desc, req);

    for (int guard = 0; guard < 1024; ++guard) {
        std::string line = co_await async_read_line_locked(out_desc);
        if (line.empty()) continue;

        json resp;
        try {
            resp = json::parse(line);
        } catch (const std::exception&) {
            continue;
        }

        if (!resp.contains("id")) continue;
        if (resp["id"] != id)     continue;

        if (resp.contains("error")) {
            auto err = resp["error"];
            throw std::runtime_error(
                "MCP stdio RPC error: " + err.value("message", "unknown"));
        }
        co_return resp.value("result", json::object());
    }
    throw std::runtime_error(
        "StdioSession::rpc_call_async: giving up after 1024 lines");
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
        co_return co_await stdio_session_->rpc_call_async(method, params);
    }

    json body;
    body["jsonrpc"] = "2.0";
    body["id"]      = ++request_id_;
    body["method"]  = method;
    body["params"]  = params;
    auto body_str = body.dump();

    auto endpoint = async::split_async_endpoint(server_url_);

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

    // Restore Mcp-Session-Id header tracking now that HttpResponse
    // exposes a generic headers map (see async/http_client.h). The
    // MCP spec (2025-03-26) sends this header on the initialize
    // response; subsequent rpc calls must echo it back so the server
    // routes to the same session. Sem 2.6 had to drop this when we
    // migrated off httplib; Sem 4 follow-up restores it.
    if (auto sid = res.get_header("Mcp-Session-Id"); !sid.empty()) {
        session_id_ = std::string(sid);
    }

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

    auto endpoint = async::split_async_endpoint(server_url_);
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
