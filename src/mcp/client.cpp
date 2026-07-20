#include <neograph/mcp/client.h>

#include <neograph/async/endpoint.h>
#include <neograph/async/http_client.h>
#include <neograph/async/run_sync.h>

#include <asio/co_spawn.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/detached.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/read_until.hpp>
#include <asio/steady_timer.hpp>
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
#  include <dirent.h>
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  if defined(__linux__)
#    include <sys/syscall.h>
#  endif
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <istream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>

namespace neograph::mcp {

namespace {
json extract_rpc_result(const json& response, int expected_id,
                        std::string_view transport) {
    const std::string prefix = "MCP " + std::string(transport) + " RPC";
    if (!response.is_object() || response.value("jsonrpc", "") != "2.0") {
        throw std::runtime_error(prefix + " response is not a JSON-RPC 2.0 object");
    }
    if (!response.contains("id") || response["id"] != expected_id) {
        throw std::runtime_error(prefix + " response id does not match the request");
    }
    const bool has_result = response.contains("result");
    const bool has_error = response.contains("error");
    if (has_result == has_error) {
        throw std::runtime_error(
            prefix + " response must contain exactly one of result or error");
    }
    if (has_error) {
        const auto& error = response["error"];
        if (!error.is_object()) {
            throw std::runtime_error(prefix + " error must be an object");
        }
        throw MCPError(error.value("code", -32603),
                       prefix + " error: " + error.value("message", "unknown"),
                       error.value("data", json(nullptr)));
    }
    return response["result"];
}
} // namespace

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

class HttpSession {
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

    std::string server_url;
    async::AsyncEndpoint endpoint;
    std::string path;
    MCPClientConfig config;
    std::mutex mu;
    std::string session_id;
    std::string protocol_version;
    int request_id = 0;
};

class ClientMetadata {
public:
    enum class Lifecycle { created, initializing, initialized };

    mutable std::mutex mu;
    std::condition_variable changed;
    Lifecycle lifecycle = Lifecycle::created;
    InitializeResult initialize_result;
    std::unordered_map<std::string, json> output_schemas;
};

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
    /// asio::posix::stream_descriptor for non-blocking I/O. Concurrent
    /// rpc_call_async() calls on the same session OVERLAP their in-flight
    /// I/O via a correlation-id demultiplexer: each caller serialises
    /// only its frame write (a capacity-1 channel held for microseconds),
    /// registers a response sink keyed by its JSON-RPC id, then awaits
    /// that sink. A single reader coroutine owns the read side, routing
    /// each response line to the matching sink. Wall time for N siblings
    /// is therefore max(latency), not sum — provided the MCP server
    /// itself processes concurrently (a serial server is the Amdahl floor
    /// and gains nothing here).
    ///
    /// All callers of one session are assumed to share one io_context
    /// (in practice the engine's), so the reader and the writers run on
    /// the same executor.
    ///
    /// Sync rpc_call() continues to use the std::mutex mtx_ and must
    /// NOT be mixed with rpc_call_async on the same session (the two
    /// paths don't know about each other).
    asio::awaitable<json> rpc_call_async(
        const std::string& method, const json& params);

    // Send a JSON-RPC notification (no id, no response expected).
    void notify(const std::string& method, const json& params);

private:
    StdioSession() = default;

    /// The actual exchange, running on THIS session's io_context. Parameters by
    /// value: they live in the coroutine frame, not in the caller's.
    asio::awaitable<json> do_exchange(std::string method, json params);

    std::string read_line_locked();       ///< caller holds mtx_
    void write_frame_locked(const json& j); ///< caller holds mtx_

    asio::awaitable<std::string> async_read_line_locked(AsyncHandle& out);
    asio::awaitable<void> async_write_frame_locked(AsyncHandle& in, const json& j);

    /// Demux reader loop. Owns async_out_, reads every response line and
    /// routes it to the waiter keyed by its JSON-RPC id. Lazily spawned
    /// while ≥1 call is in flight; exits once no waiters remain so a
    /// private run_sync io_context can drain and return.
    asio::awaitable<void> run_reader();

#ifdef _WIN32
    HANDLE process_ = nullptr;   ///< child process handle (CloseHandle on dtor)
    HANDLE stdin_h_ = nullptr;   ///< parent → child (write end of a pipe)
    HANDLE stdout_h_ = nullptr;  ///< child → parent (read end of a pipe)
#else
    pid_t pid_ = -1;
    int   stdin_fd_ = -1;   // parent → child
    int   stdout_fd_ = -1;  // child  → parent
#endif

    // ── The session's OWN io_context ────────────────────────────────
    //
    // Every piece of asio state below (the pipe descriptors, the write
    // lock, the reader coroutine) is bound to an executor on first use
    // and reused on every later call. It used to be the CALLER's
    // executor, and the header said so:
    //
    //     "All callers of one session are assumed to share one
    //      io_context (in practice the engine's)"
    //
    // That was never true. `GraphEngine::run` goes through `run_sync`,
    // which stands up an io_context for ONE call and destroys it on the
    // way out. So the second run — or simply the client's destructor —
    // touched asio state hanging off a destroyed io_context. Without a
    // sanitizer, in plain C++, that is a core dump.
    //
    // Owning the context makes its lifetime the session's, and deletes
    // the old precondition ("callers must ensure the io_context outlives
    // the session") rather than restating it. No engine could have
    // honoured it.
    //
    // Declared FIRST so it is destroyed LAST — after the descriptors and
    // the lock that point into it.
    asio::io_context io_;
    asio::executor_work_guard<asio::io_context::executor_type> io_guard_{
        asio::make_work_guard(io_)};
    std::thread io_thread_;
    std::mutex  io_thread_mtx_;

    /// Stop the worker and drop everything bound to io_, in that order. Called
    /// from both platforms' destructors before the subprocess is reaped.
    void shutdown_async_io();

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

    // Cached AsyncHandle wrappers for the async path. Lazy-created on
    // first rpc_call_async using the caller's executor, then reused
    // for every subsequent call.
    //
    // Why caching, not per-call construction:
    //   Windows pins the IOCP association on the kernel FILE_OBJECT,
    //   not on the HANDLE. Once the first call registers a handle
    //   with the io_context's IOCP, the pipe's FILE_OBJECT is bound
    //   forever. DuplicateHandle produces a new HANDLE referring to
    //   the SAME FILE_OBJECT, so re-registering a duplicate in a
    //   second call returns ERROR_INVALID_PARAMETER (already bound),
    //   asio::windows::stream_handle's ctor throws, and the coroutine
    //   dies before reaching any I/O. Caching side-steps this: bind
    //   once, reuse forever.
    //
    //   POSIX is fine either way (epoll is per-fd, not per-file), but
    //   we cache there too for symmetry and to avoid the per-call
    //   wrapper churn.
    //
    //   We wrap DUPLICATES of the native handles so the session keeps
    //   ownership of stdin_h_/stdout_h_; the wrappers close the dups
    //   on their own destruction. Callers must ensure the io_context
    //   driving rpc_call_async outlives the session (reverse order of
    //   declaration in tests and call sites).
    std::unique_ptr<AsyncHandle> async_in_;
    std::unique_ptr<AsyncHandle> async_out_;
    std::mutex async_handles_init_mtx_;

    // ── Demux multiplexer ───────────────────────────────────────────
    // `async_lock_` above is repurposed as a WRITE-ONLY lock (held only
    // around the frame write). One reader coroutine owns async_out_ and
    // fans each response to the waiter registered under its JSON-RPC id,
    // so N in-flight calls overlap their reads instead of serialising
    // behind one round-trip lock.
    using RespChan =
        asio::experimental::channel<void(asio::error_code,
                                         std::shared_ptr<json>)>;
    std::mutex demux_mu_;                                ///< guards the two fields below
    std::map<int, std::shared_ptr<RespChan>> waiters_;  ///< id → response sink
    bool reader_running_ = false;                       ///< a run_reader() coroutine is live
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

    // Refuse to spawn `.bat` / `.cmd` targets via CreateProcess with
    // a null lpApplicationName. cmd.exe parses CommandLine for those
    // and `build_win_cmdline` only escapes the double-quote / quoted-
    // whitespace cases — `^`, `&`, `|`, `<`, `>`, parentheses are
    // passed through, which is a command-injection surface (CVE-2024-
    // 1874-class). Callers who genuinely need to launch a batch file
    // should resolve it to its interpreter first (e.g. cmd.exe /c).
    if (!argv.empty()) {
        const auto& exe = argv[0];
        if (exe.size() >= 4) {
            std::string ext = exe.substr(exe.size() - 4);
            for (auto& c : ext) c = static_cast<char>(::tolower(c));
            if (ext == ".bat" || ext == ".cmd") {
                throw std::runtime_error(
                    "StdioSession: refusing to spawn .bat/.cmd target via "
                    "CreateProcess (cmd.exe metacharacter injection risk). "
                    "Wrap the script in `cmd.exe /c <script>` and re-quote "
                    "the arguments yourself if you really need it.");
            }
        }
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
    shutdown_async_io();   // see the POSIX destructor

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

        // Close every other inherited file descriptor before exec.
        // Without this, the child inherits whatever the parent had
        // open: TLS sockets, sqlite/postgres connection fds, asio's
        // timerfd / eventfd, OpenSSL's RNG fd, etc. — both a resource
        // leak and a security issue (the spawned MCP server could
        // read or write the parent's TLS sessions or DB).
        //
        // Linux: prefer close_range(3, ~0u, 0) (kernel ≥ 5.9). Fall
        // back to walking /proc/self/fd. macOS / BSD: closefrom() is
        // the standard call. We don't enable closefrom on macOS at
        // build time without a feature probe, so the /proc fallback
        // is the portable belt-and-braces path.
#if defined(__linux__) && defined(SYS_close_range)
        if (::syscall(SYS_close_range, 3u, ~0u, 0u) != 0) {
#endif
        DIR* d = ::opendir("/proc/self/fd");
        if (d) {
            int dfd = ::dirfd(d);
            struct dirent* ent;
            while ((ent = ::readdir(d)) != nullptr) {
                if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;
                int fd = std::atoi(ent->d_name);
                if (fd > 2 && fd != dfd) ::close(fd);
            }
            ::closedir(d);
        } else {
            // Last-resort sweep — best effort, bounded so a high
            // RLIMIT_NOFILE doesn't make exec take seconds.
            int max_fd = static_cast<int>(::sysconf(_SC_OPEN_MAX));
            if (max_fd <= 0 || max_fd > 65536) max_fd = 65536;
            for (int fd = 3; fd < max_fd; ++fd) ::close(fd);
        }
#if defined(__linux__) && defined(SYS_close_range)
        }
#endif

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
    // First: stop the io_context and destroy everything bound to it. Doing this
    // after closing the fds (or not at all) is what left a destroyed
    // io_context's descriptors to be freed twice.
    shutdown_async_io();

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

// Tear down the async machinery on the thread that owns it, before anything it
// points at can go away. Order matters: close the descriptors (which cancels the
// reader's pending read, letting it finish), release the work guard so io_.run()
// can return, then join.
void StdioSession::shutdown_async_io() {
    if (!io_thread_.joinable()) {
        io_guard_.reset();
        return;
    }
    asio::post(io_, [this] {
        asio::error_code ec;
        if (async_in_)  async_in_->close(ec);
        if (async_out_) async_out_->close(ec);
        if (async_lock_) async_lock_->close();
        for (auto& kv : waiters_) {
            if (kv.second) kv.second->close();
        }
        async_in_.reset();
        async_out_.reset();
        async_lock_.reset();
    });
    io_guard_.reset();
    io_thread_.join();
}

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
    // Hard cap on a single line. Without this a malicious or buggy
    // server that never emits a newline would let `buffer_` grow until
    // the parent OOMs. 16 MB is the same order of magnitude as the
    // HTTP body limits in async/http_client.h and well above any
    // legitimate MCP message.
    constexpr size_t MAX_LINE_BYTES = 16 * 1024 * 1024;
    while (true) {
        auto nl = buffer_.find('\n');
        if (nl != std::string::npos) {
            std::string line = buffer_.substr(0, nl);
            buffer_.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return line;
        }
        if (buffer_.size() > MAX_LINE_BYTES) {
            throw std::runtime_error(
                "StdioSession: incoming line exceeded "
                + std::to_string(MAX_LINE_BYTES)
                + " bytes without newline (peer misbehaving)");
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

        return extract_rpc_result(resp, id, "stdio");
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

asio::awaitable<void> StdioSession::run_reader() {
    std::exception_ptr fail;
    try {
        for (;;) {
            std::string line = co_await async_read_line_locked(*async_out_);
            if (!line.empty()) {
                json resp;
                bool parsed = true;
                try { resp = json::parse(line); }
                catch (const std::exception&) { parsed = false; }
                if (parsed && resp.contains("id")
                        && resp["id"].is_number_integer()) {
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
    } catch (const std::exception&) {
        // Pipe EOF / read error (e.g. child died): fail every waiter so
        // awaiting callers throw instead of hanging on a dead server.
        fail = std::current_exception();
    }

    if (fail) {
        std::map<int, std::shared_ptr<RespChan>> remaining;
        {
            std::lock_guard<std::mutex> lk(demux_mu_);
            remaining.swap(waiters_);
            reader_running_ = false;
        }
        for (auto& kv : remaining) kv.second->close();
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
asio::awaitable<json>
StdioSession::rpc_call_async(const std::string& method, const json& params) {
    // Lazily start the worker. A session that only ever uses the sync path (or
    // never gets called at all) pays no thread.
    {
        std::lock_guard<std::mutex> g(io_thread_mtx_);
        if (!io_thread_.joinable()) {
            io_thread_ = std::thread([this] { io_.run(); });
        }
    }

    co_return co_await asio::co_spawn(
        io_.get_executor(),
        // BY VALUE, into the coroutine frame. A reference would dangle: this
        // function returns as soon as it has handed the awaitable to co_spawn.
        do_exchange(method, params),
        asio::use_awaitable);
}

asio::awaitable<json>
StdioSession::do_exchange(std::string method, json params) {
    const int id = ++next_id_;

    json req = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", method},
        {"params", params}
    };

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
        co_await async_write_frame_locked(*async_in_, req);
    }

    // Await our response. use_awaitable turns a closed sink (session torn
    // down / server gone) into a thrown system_error.
    std::shared_ptr<json> respptr =
        co_await chan->async_receive(asio::use_awaitable);

    json& resp = *respptr;
    // Bind to a named local before co_return — dodges the GCC 13
    // build_special_member_call ICE on co_return of a brace/temp.
    json result = extract_rpc_result(resp, id, "stdio");
    co_return result;
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
  , http_session_(nullptr)
  , metadata_(nullptr)
  , stdio_session_(nullptr)
  , definition_(legacy_definition(name, description, input_schema))
{
}

MCPTool::MCPTool(std::shared_ptr<detail::StdioSession> session,
                 const std::string& name,
                 const std::string& description,
                 const json& input_schema)
  : server_url_()
  , http_session_(nullptr)
  , metadata_(nullptr)
  , stdio_session_(std::move(session))
  , definition_(legacy_definition(name, description, input_schema))
{
}

MCPTool::MCPTool(std::shared_ptr<detail::HttpSession> session,
                 std::shared_ptr<detail::ClientMetadata> metadata,
                 ToolDefinition definition)
  : http_session_(std::move(session))
  , metadata_(std::move(metadata))
  , definition_(std::move(definition))
{
}

MCPTool::MCPTool(std::shared_ptr<detail::StdioSession> session,
                 ToolDefinition definition)
  : stdio_session_(std::move(session))
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
    if (stdio_session_) {
        json params{{"name", definition_.name}, {"arguments", arguments}};
        json result = co_await stdio_session_->rpc_call_async("tools/call", params);
        auto typed = CallToolResult::from_json(result);
        validate_tool_result(typed, definition_.output_schema);
        co_return typed;
    }

    if (http_session_) {
        auto client = std::unique_ptr<MCPClient>(
            new MCPClient(http_session_, metadata_));
        auto typed = co_await client->call_tool_result_async(
            definition_.name, arguments);
        co_return typed;
    }

    // HTTP — one ephemeral client per call, driven fully async. Unlike
    // the old sync path (which spun a private io_context via run_sync
    // per call and parked a worker thread), this awaits on the caller's
    // executor, so several sibling tool calls dispatched from one node
    // keep their HTTP round-trips in flight at the same time.
    //
    // Heap-allocate the client: MCPClient holds a std::mutex (non-move,
    // non-copy), and keeping such an object directly in the coroutine
    // frame across a co_await trips a GCC 13 codegen ICE
    // (build_special_member_call). A unique_ptr in the frame sidesteps
    // it — the object lives on the heap, the frame only owns a pointer.
    auto client = std::make_unique<MCPClient>(server_url_);
    co_await client->initialize_async();
    auto typed = co_await client->call_tool_result_async(
        definition_.name, arguments);
    validate_tool_result(typed, definition_.output_schema);
    co_return typed;
}

asio::awaitable<std::string> MCPTool::execute_async(const json& arguments) {
    auto result = co_await execute_result_async(arguments);
    std::string text = format_tool_result(result);
    if (result.is_error) {
        throw std::runtime_error("MCP tool execution error: " + text);
    }
    co_return text;
}

// ===========================================================================
// MCPClient — HTTP transport
// ===========================================================================

MCPClient::MCPClient(const std::string& server_url)
  : MCPClient(server_url, MCPClientConfig{})
{
}

MCPClient::MCPClient(const std::string& server_url, MCPClientConfig config)
  : http_session_(std::make_shared<detail::HttpSession>(
        server_url, std::move(config)))
  , metadata_(std::make_shared<detail::ClientMetadata>())
{
}

// ===========================================================================
// MCPClient — stdio transport
// ===========================================================================

MCPClient::MCPClient(std::vector<std::string> argv)
  : stdio_session_(detail::StdioSession::spawn(argv))
  , metadata_(std::make_shared<detail::ClientMetadata>())
{
}

MCPClient::MCPClient(std::shared_ptr<detail::HttpSession> session,
                     std::shared_ptr<detail::ClientMetadata> metadata)
  : http_session_(std::move(session))
  , metadata_(std::move(metadata))
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

    auto session = http_session_;
    if (!session) {
        throw std::logic_error("MCP client has no transport");
    }

    // Build the request envelope + headers under the session mutex — the
    // shared fields (request_id_, session_id_, negotiated_protocol_version_)
    // would otherwise race across concurrent rpc_call_async invocations.
    // We hold the lock only while reading/writing those fields, NOT
    // across the network call below.
    int                                              this_id;
    std::vector<std::pair<std::string, std::string>> headers;
    HeaderProvider header_provider;
    {
        std::lock_guard lk(session->mu);
        this_id = ++session->request_id;
        headers = {
            {"Content-Type", "application/json"},
            {"Accept",       "application/json, text/event-stream"},
        };
        headers.insert(headers.end(), session->config.headers.begin(),
                       session->config.headers.end());
        header_provider = session->config.header_provider;
        if (!session->session_id.empty()) {
            headers.emplace_back("Mcp-Session-Id", session->session_id);
        }
        // Spec MUST (transports / Streamable HTTP § "Protocol Version
        // Header"): include MCP-Protocol-Version on every HTTP request
        // after initialize. Strict 2025-11-25 servers respond 400 Bad
        // Request without it. Skip on the initialize call itself —
        // negotiated_protocol_version_ is empty until initialize returns.
        if (!session->protocol_version.empty()) {
            headers.emplace_back("MCP-Protocol-Version",
                                 session->protocol_version);
        }
    }
    // User callbacks may refresh credentials or re-enter application code. Run
    // them after releasing the session mutex so they cannot deadlock the client.
    if (header_provider) {
        auto dynamic_headers = header_provider();
        headers.insert(headers.end(), dynamic_headers.begin(),
                       dynamic_headers.end());
    }
    for (const auto& [name, value] : headers) {
        if (name.empty() || name.find_first_of("\r\n") != std::string::npos
            || value.find_first_of("\r\n") != std::string::npos) {
            throw std::invalid_argument("MCP header contains CR/LF or an empty name");
        }
    }

    json body;
    body["jsonrpc"] = "2.0";
    body["id"]      = this_id;
    body["method"]  = method;
    body["params"]  = params;
    auto body_str = body.dump();

    async::RequestOptions opts;
    opts.timeout = session->config.request_timeout;

    auto ex = co_await asio::this_coro::executor;
    async::HttpResponse res;
    try {
        res = co_await async::async_post(
            ex,
            session->endpoint.host,
            session->endpoint.port,
            session->path,
            body_str,
            std::move(headers),
            session->endpoint.tls,
            opts);
    } catch (const std::system_error& e) {
        throw std::runtime_error(std::string("MCP request failed: ") + e.what());
    }

    // Absorb response state under the mutex — `Mcp-Session-Id` may
    // change mid-conversation if the server rotates it. The MCP spec
    // sends this header on the initialize response; subsequent rpc
    // calls must echo it back so the server routes to the same session.
    if (auto sid = res.get_header("Mcp-Session-Id"); !sid.empty()) {
        std::lock_guard lk(session->mu);
        session->session_id = std::string(sid);
    }

    if (res.status != 200) {
        std::string scheme = session->endpoint.tls ? "https://" : "http://";
        std::string full_url =
            scheme + session->endpoint.host + ":" + session->endpoint.port
            + session->path;
        std::string hint;
        if (res.status == 404) {
            hint = " — the server has no MCP endpoint at this path. Check the "
                   "configured URL (a trailing '/mcp' is added automatically, "
                   "so pass the server base like 'http://host:8000' or the "
                   "full 'http://host:8000/mcp').";
        }
        throw std::runtime_error(
            "MCP error (HTTP " + std::to_string(res.status) + ") for " +
            full_url + ": " + res.body + hint);
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
            throw std::runtime_error(
                "MCP SSE response had no JSON-RPC response matching the request id");
        }
    } else {
        resp = json::parse(res.body);
    }

    co_return extract_rpc_result(resp, this_id, "HTTP");
}

namespace {

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
        metadata->initialize_result = std::move(result);
        metadata->lifecycle = detail::ClientMetadata::Lifecycle::initialized;
    }
    metadata->changed.notify_all();
}

void reset_initialization(
    const std::shared_ptr<detail::HttpSession>& http_session,
    const std::shared_ptr<detail::ClientMetadata>& metadata) {
    if (http_session) {
        std::lock_guard lk(http_session->mu);
        http_session->session_id.clear();
        http_session->protocol_version.clear();
    }
    {
        std::lock_guard lk(metadata->mu);
        metadata->lifecycle = detail::ClientMetadata::Lifecycle::created;
        metadata->initialize_result = {};
        metadata->output_schemas.clear();
    }
    metadata->changed.notify_all();
}

HeaderList notification_headers(const std::shared_ptr<detail::HttpSession>& session) {
    HeaderList headers = {
        {"Content-Type", "application/json"},
        {"Accept", "application/json, text/event-stream"},
    };
    HeaderProvider provider;
    {
        std::lock_guard lk(session->mu);
        headers.insert(headers.end(), session->config.headers.begin(),
                       session->config.headers.end());
        provider = session->config.header_provider;
        if (!session->session_id.empty()) {
            headers.emplace_back("Mcp-Session-Id", session->session_id);
        }
        if (!session->protocol_version.empty()) {
            headers.emplace_back("MCP-Protocol-Version", session->protocol_version);
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

asio::awaitable<void> send_initialized_notification(
    const std::shared_ptr<detail::HttpSession>& session) {
    json notify = {
        {"jsonrpc", "2.0"},
        {"method", "notifications/initialized"},
        {"params", json::object()},
    };
    async::RequestOptions opts;
    opts.timeout = session->config.request_timeout;
    auto ex = co_await asio::this_coro::executor;
    auto res = co_await async::async_post(
        ex, session->endpoint.host, session->endpoint.port, session->path,
        notify.dump(), notification_headers(session), session->endpoint.tls, opts);
    if (res.status < 200 || res.status >= 300) {
        throw std::runtime_error(
            "MCP initialize notification returned HTTP "
            + std::to_string(res.status) + ": " + res.body);
    }
    co_return;
}

} // namespace

bool MCPClient::initialize(const std::string& client_name) {
    {
        std::unique_lock lk(metadata_->mu);
        while (metadata_->lifecycle
               == detail::ClientMetadata::Lifecycle::initializing) {
            metadata_->changed.wait(lk);
        }
        if (metadata_->lifecycle
            == detail::ClientMetadata::Lifecycle::initialized) {
            return true;
        }
        metadata_->lifecycle = detail::ClientMetadata::Lifecycle::initializing;
    }

    try {
        auto raw = rpc_call("initialize", initialize_params(client_name));
        auto result = InitializeResult::from_json(raw);
        if (http_session_) {
            {
                std::lock_guard lk(http_session_->mu);
                http_session_->protocol_version = result.protocol_version;
            }
            async::run_sync(send_initialized_notification(http_session_));
        } else {
            stdio_session_->notify("notifications/initialized", json::object());
        }
        store_initialize_result(metadata_, std::move(result));
        return true;
    } catch (...) {
        reset_initialization(http_session_, metadata_);
        throw;
    }
}

asio::awaitable<bool> MCPClient::initialize_async(const std::string& client_name) {
    bool owner = false;
    for (;;) {
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
                owner = true;
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
        auto result = InitializeResult::from_json(raw);
        if (http_session_) {
            {
                std::lock_guard lk(http_session_->mu);
                http_session_->protocol_version = result.protocol_version;
            }
            co_await send_initialized_notification(http_session_);
        } else {
            stdio_session_->notify("notifications/initialized", json::object());
        }
        store_initialize_result(metadata_, std::move(result));
        co_return true;
    } catch (...) {
        reset_initialization(http_session_, metadata_);
        throw;
    }
}

bool MCPClient::is_initialized() const noexcept {
    std::lock_guard lk(metadata_->mu);
    return metadata_->lifecycle
        == detail::ClientMetadata::Lifecycle::initialized;
}

InitializeResult MCPClient::get_initialize_result() const {
    std::lock_guard lk(metadata_->mu);
    if (metadata_->lifecycle
        != detail::ClientMetadata::Lifecycle::initialized) {
        throw std::logic_error("MCP client has not been initialized");
    }
    return metadata_->initialize_result;
}

std::vector<std::unique_ptr<Tool>> MCPClient::get_tools() {
    auto definitions = get_tool_definitions();
    std::vector<std::unique_ptr<Tool>> tools;
    tools.reserve(definitions.size());
    for (auto& definition : definitions) {
        if (stdio_session_) {
            tools.push_back(std::unique_ptr<Tool>(
                new MCPTool(stdio_session_, std::move(definition))));
        } else {
            tools.push_back(std::unique_ptr<Tool>(
                new MCPTool(http_session_, metadata_, std::move(definition))));
        }
    }
    return tools;
}

ListToolsPage MCPClient::list_tools(
    const std::optional<std::string>& cursor) {
    initialize();
    json params = json::object();
    if (cursor) params["cursor"] = *cursor;
    auto page = ListToolsPage::from_json(rpc_call("tools/list", params));
    {
        std::lock_guard lk(metadata_->mu);
        for (const auto& tool : page.tools) {
            metadata_->output_schemas[tool.name] = tool.output_schema;
        }
    }
    return page;
}

asio::awaitable<ListToolsPage> MCPClient::list_tools_async(
    const std::optional<std::string>& cursor) {
    co_await initialize_async();
    json params = json::object();
    if (cursor) params["cursor"] = *cursor;
    auto raw = co_await rpc_call_async("tools/list", params);
    auto page = ListToolsPage::from_json(raw);
    {
        std::lock_guard lk(metadata_->mu);
        for (const auto& tool : page.tools) {
            metadata_->output_schemas[tool.name] = tool.output_schema;
        }
    }
    co_return page;
}

std::vector<ToolDefinition> MCPClient::get_tool_definitions() {
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

json MCPClient::call_tool(const std::string& name, const json& arguments) {
    initialize();
    json params;
    params["name"]      = name;
    params["arguments"] = arguments;
    return rpc_call("tools/call", params);
}

CallToolResult MCPClient::call_tool_result(
    const std::string& name, const json& arguments) {
    auto result = CallToolResult::from_json(call_tool(name, arguments));
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
MCPClient::call_tool_async(const std::string& name, const json& arguments) {
    co_await initialize_async();
    json params;
    params["name"]      = name;
    params["arguments"] = arguments;
    co_return co_await rpc_call_async("tools/call", params);
}

asio::awaitable<CallToolResult> MCPClient::call_tool_result_async(
    const std::string& name, const json& arguments) {
    auto raw = co_await call_tool_async(name, arguments);
    auto result = CallToolResult::from_json(raw);
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

} // namespace neograph::mcp
