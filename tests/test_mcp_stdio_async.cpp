// End-to-end MCP protocol + stdio transport coverage. The protocol
// session owns JSON-RPC envelopes, initialization and tool adaptation.
// The stdio transport owns subprocess pipes, response-id demultiplexing,
// cancellation, and shutdown. Fixtures are real stdlib-only Python
// servers, with no third-party MCP dependency.

#include <gtest/gtest.h>
#include <neograph/mcp/client.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/cancel.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <string>
#include <cerrno>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

using namespace neograph;

namespace {

std::filesystem::path fixture_path() {
    // Tests run from the build dir; the source tree is two levels up
    // from build-nopg/. We can't rely on cwd so derive from __FILE__.
    std::filesystem::path here(__FILE__);
    return here.parent_path() / "fixtures" / "mcp_stdio_echo.py";
}

// Locate a usable Python interpreter — `python3` on POSIX, `python`
// on Windows where most installs don't expose the versioned name.
// Returned string is whatever `system()` will accept as argv[0].
const char* python_cmd() {
#ifdef _WIN32
    // `where python` on Windows shells; `where` returns 0 if found.
    if (std::system("where python >nul 2>&1") == 0)  return "python";
    if (std::system("where python3 >nul 2>&1") == 0) return "python3";
    return nullptr;
#else
    if (std::system("command -v python3 >/dev/null 2>&1") == 0) return "python3";
    if (std::system("command -v python  >/dev/null 2>&1") == 0) return "python";
    return nullptr;
#endif
}

bool python3_available() { return python_cmd() != nullptr; }

struct BoundaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path()
        / ("neograph-mcp-boundary-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    BoundaryDirectory() { std::filesystem::create_directory(path); }
    ~BoundaryDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

mcp::StdioClientConfig boundary_config(const std::string& mode) {
    mcp::StdioClientConfig config;
    config.argv = {python_cmd(),
        (fixture_path().parent_path() / "mcp_stdio_boundary.py").string(), mode};
    config.request_timeout = std::chrono::milliseconds(2000);
    return config;
}

} // namespace

TEST(MCPStdioAsync, RpcCallAsyncRoundTripsThroughSubprocess) {
    if (!python3_available()) {
        GTEST_SKIP() << "python3 not available";
    }
    auto fixture = fixture_path();
    ASSERT_TRUE(std::filesystem::exists(fixture))
        << "fixture missing: " << fixture;

    // The stdio transport owns its io_context independently of this caller.
    asio::io_context io;
    mcp::MCPClient client({python_cmd(), fixture.string()});

    mcp::InitializeResult initialize_result;
    mcp::ListToolsPage tools_result;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            co_await client.initialize_async("test");
            initialize_result = client.get_initialize_result();
            tools_result = co_await client.list_tools_async();
        },
        asio::detached);
    io.run();

    EXPECT_EQ(initialize_result.server_info.value("name", ""), "stdio-echo");
    ASSERT_EQ(tools_result.tools.size(), 1u);
    EXPECT_EQ(tools_result.tools[0].name, "echo");
}

TEST(MCPStdioAsync, ConcurrentAsyncCallsOnSameSessionCompleteSafely) {
    // The write semaphore protects frames only; one reader routes concurrent
    // replies by id without blocking the caller's single-threaded io_context.
    if (!python3_available()) {
        GTEST_SKIP() << "python3 not available";
    }
    auto fixture = fixture_path();
    ASSERT_TRUE(std::filesystem::exists(fixture));

    // Caller context and session context have independent lifetimes.
    asio::io_context io;
    mcp::MCPClient client({python_cmd(), fixture.string()});
    std::atomic<int> done{0};
    std::array<json, 3> results;

    // First: a sequential initialize so subsequent calls hit the
    // post-handshake state. (The echo fixture tolerates any order
    // but real MCP servers require initialize first.)
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            co_await client.initialize_async("test");
            // Now fan out three parallel calls on the same session.
            // Each nested co_spawn returns a deferred op for a
            // parallel_group... but we can also just co_await three
            // sequential — the point of the regression is that the
            // FIRST starts holding the lock and the SECOND/THIRD
            // suspend on it cooperatively. We verify via co_spawn of
            // siblings on the same io_context.
            for (int i = 0; i < 3; ++i) {
                asio::co_spawn(
                    io,
                    [&, i]() -> asio::awaitable<void> {
                        results[i] = co_await client.rpc_call_async(
                            "tools/list", json::object());
                        done.fetch_add(1, std::memory_order_relaxed);
                    },
                    asio::detached);
            }
        },
        asio::detached);
    io.run();

    EXPECT_EQ(done.load(), 3);
    for (const auto& r : results) {
        ASSERT_TRUE(r.is_object())
            << "one of the concurrent calls produced no result";
        EXPECT_TRUE(r.contains("tools"));
    }
}

TEST(MCPStdioAsync, SyncFacadeStillWorksAlongsideAsync) {
    // Sync calls bridge through the same session-owned async transport.
    if (!python3_available()) {
        GTEST_SKIP() << "python3 not available";
    }
    auto fixture = fixture_path();

    mcp::MCPClient client({python_cmd(), fixture.string()});
    ASSERT_TRUE(client.initialize());

    auto tools = client.get_tools();
    ASSERT_EQ(tools.size(), 1u);
    EXPECT_EQ(tools[0]->get_name(), "echo");

    auto out = client.call_tool("echo", json{{"msg", "hello"}});
    EXPECT_TRUE(out.is_object());
    ASSERT_TRUE(out.contains("content"));
}

TEST(MCPStdioAsync, SyncAndAsyncCallsShareOneCorrelationReader) {
    if (!python3_available()) GTEST_SKIP() << "python3 not available";
    auto fixture = fixture_path().parent_path() / "mcp_stdio_slow.py";
    ASSERT_TRUE(std::filesystem::exists(fixture));
    mcp::MCPClient client({python_cmd(), fixture.string()});
    ASSERT_TRUE(client.initialize());
    json slow{{"name", "echo"},
              {"arguments", {{"marker", "async"}, {"delay_ms", 75}}}};
    auto future = std::async(std::launch::async, [&] {
        return async::run_sync(client.rpc_call_async("tools/call", slow));
    });
    auto sync = client.call_tool("echo", json{{"marker", "sync"}});
    auto asynchronous = future.get();
    auto marker = [](const json& result) {
        return json::parse(result.at("content").at(0).at("text").get<std::string>())
            .at("args").at("marker").get<std::string>();
    };
    EXPECT_EQ(marker(sync), "sync");
    EXPECT_EQ(marker(asynchronous), "async");
}

TEST(MCPStdioAsync, ConcurrentStdioCallsOverlapIO) {
    // Demux-multiplexer proof: N tool calls on ONE stdio session, each
    // an I/O-bound 100 ms server-side wait, fanned out concurrently.
    // With the correlation-id demux the writes are pipelined and the
    // reads overlap, so wall ≈ one delay; the pre-demux round-trip lock
    // would serialise to ≈ N delays. The slow fixture handles each call
    // on its own thread so the server itself is NOT the bottleneck.
    if (!python3_available()) {
        GTEST_SKIP() << "python3 not available";
    }
    std::filesystem::path here(__FILE__);
    auto fixture = here.parent_path() / "fixtures" / "mcp_stdio_slow.py";
    ASSERT_TRUE(std::filesystem::exists(fixture))
        << "fixture missing: " << fixture;

    constexpr int kN = 5;
    constexpr int kDelayMs = 100;

    asio::io_context io;
    mcp::MCPClient client({python_cmd(), fixture.string()});

    // Pre-build each call's params OUTSIDE the coroutine (GCC 13 nested
    // brace-init-in-coroutine ICE). Each carries a distinct marker so we
    // can prove the demux routed each response to the RIGHT caller.
    std::array<json, kN> call_params;
    for (int i = 0; i < kN; ++i) {
        json args;
        args["delay_ms"] = kDelayMs;
        args["marker"] = i;
        json p;
        p["name"] = "echo";
        p["arguments"] = args;
        call_params[i] = p;
    }

    std::array<json, kN> results;
    std::atomic<int> done{0};
    long wall_ms = 0;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            co_await client.initialize_async("test");
            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < kN; ++i) {
                asio::co_spawn(
                    io,
                    [&, i]() -> asio::awaitable<void> {
                        results[i] = co_await client.rpc_call_async(
                            "tools/call", call_params[i]);
                        done.fetch_add(1, std::memory_order_relaxed);
                    },
                    asio::detached);
            }
            // Spin the same io_context cooperatively until all siblings
            // report in, then stamp the wall time.
            while (done.load(std::memory_order_relaxed) < kN) {
                asio::steady_timer t(co_await asio::this_coro::executor);
                t.expires_after(std::chrono::milliseconds(1));
                co_await t.async_wait(asio::use_awaitable);
            }
            wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
        },
        asio::detached);
    io.run();

    ASSERT_EQ(done.load(), kN);

    // Correctness: every response was routed to its own caller by id.
    for (int i = 0; i < kN; ++i) {
        ASSERT_TRUE(results[i].is_object()) << "call " << i << " no result";
        ASSERT_TRUE(results[i].contains("content"));
        const auto& content = results[i]["content"];
        ASSERT_TRUE(content.is_array() && !content.empty());
        auto text = content[0].value("text", std::string{});
        auto echoed = json::parse(text);
        EXPECT_EQ(echoed["args"].value("marker", -1), i)
            << "demux mis-routed: call " << i << " got " << text;
    }

    // Overlap: serial would be ≈ kN*kDelayMs (500 ms); demux ≈ kDelayMs
    // (100 ms). Assert well under the serial floor with generous slack.
    EXPECT_LT(wall_ms, kN * kDelayMs * 3 / 5)
        << "calls did not overlap: wall=" << wall_ms
        << "ms, serial floor=" << (kN * kDelayMs) << "ms";
}

TEST(MCPStdioAsync, TimeoutAndCancellationDoNotCorruptLaterCorrelation) {
    if (!python3_available()) GTEST_SKIP() << "python3 not available";
    auto fixture = fixture_path().parent_path() / "mcp_stdio_slow.py";
    ASSERT_TRUE(std::filesystem::exists(fixture));
    mcp::MCPClient client({python_cmd(), fixture.string()});
    ASSERT_TRUE(client.initialize());

    json slow{{"name", "echo"},
              {"arguments", {{"marker", "late"}, {"delay_ms", 200}}}};
    json fast{{"name", "echo"}, {"arguments", {{"marker", "next"}}}};
    try {
        async::run_sync(client.rpc_call_async(
            "tools/call", slow,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(25)));
        FAIL() << "expected stdio deadline";
    } catch (const mcp::MCPTransportError& e) {
        EXPECT_EQ(e.failure(), mcp::MCPFailure::timeout);
    }

    auto cancel = std::make_shared<graph::CancelToken>();
    std::jthread stop([cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        cancel->cancel();
    });
    try {
        async::run_sync(client.rpc_call_async(
            "tools/call", slow, std::chrono::steady_clock::time_point::max(), cancel));
        FAIL() << "expected stdio cancellation";
    } catch (const mcp::MCPTransportError& e) {
        EXPECT_EQ(e.failure(), mcp::MCPFailure::cancelled);
    }

    auto result = async::run_sync(client.rpc_call_async("tools/call", fast));
    auto payload = json::parse(result.at("content").at(0).at("text").get<std::string>());
    EXPECT_EQ(payload.at("args").at("marker"), "next");
    std::this_thread::sleep_for(std::chrono::milliseconds(230));
    auto again = async::run_sync(client.rpc_call_async("tools/call", fast));
    EXPECT_EQ(json::parse(again.at("content").at(0).at("text").get<std::string>())
                  .at("args").at("marker"), "next");
}

TEST(MCPStdioAsync, ToolRetainsProcessUntilLastOwnerReleasesIt) {
    if (!python3_available()) GTEST_SKIP() << "python3 not available";
    auto fixture = fixture_path().parent_path() / "mcp_stdio_slow.py";
    ASSERT_TRUE(std::filesystem::exists(fixture));
    std::vector<std::unique_ptr<Tool>> tools;
    int pid = -1;
    {
        mcp::MCPClient client({python_cmd(), fixture.string()});
        tools = client.get_tools();
        ASSERT_EQ(tools.size(), 1u);
        pid = client.call_tool("echo", json::object()).at("serverPid").get<int>();
    }
    auto* tool = dynamic_cast<mcp::MCPTool*>(tools[0].get());
    ASSERT_NE(tool, nullptr);
    auto result = tool->execute_result(json::object());
    EXPECT_EQ(result.raw.at("serverPid"), pid);
#ifndef _WIN32
    ASSERT_EQ(::kill(pid, 0), 0);
#endif
    tools.clear();
#ifndef _WIN32
    errno = 0;
    EXPECT_EQ(::kill(pid, 0), -1);
    EXPECT_EQ(errno, ESRCH);
#endif
}

TEST(MCPStdioAsync, DeadSubprocessIsAConnectionFailure) {
    if (!python3_available()) GTEST_SKIP() << "python3 not available";
    mcp::MCPClient client({python_cmd(), "-c", "import sys; sys.exit(0)"});
    try {
        client.initialize();
        FAIL() << "expected subprocess EOF";
    } catch (const mcp::MCPTransportError& e) {
        EXPECT_EQ(e.failure(), mcp::MCPFailure::connection);
    }
    EXPECT_FALSE(client.is_initialized());
}

TEST(MCPStdioAsync, MalformedSubprocessFrameIsAProtocolFailure) {
    if (!python3_available()) GTEST_SKIP() << "python3 not available";
    mcp::MCPClient client({
        python_cmd(), "-c",
        "import sys; sys.stdin.readline(); print('not-json', flush=True)",
    });
    try {
        client.initialize();
        FAIL() << "expected invalid stdio frame";
    } catch (const mcp::MCPTransportError& e) {
        EXPECT_EQ(e.failure(), mcp::MCPFailure::protocol);
    }
}

TEST(MCPStdioAsync, InvalidInitializeResultIsAProtocolFailure) {
    if (!python3_available()) GTEST_SKIP() << "python3 not available";
    mcp::MCPClient client({
        python_cmd(), "-c",
        "import sys,json; r=json.loads(sys.stdin.readline()); "
        "print(json.dumps({'jsonrpc':'2.0','id':r['id'],'result':{}}),flush=True)",
    });
    try {
        client.initialize();
        FAIL() << "expected missing initialize fields";
    } catch (const mcp::MCPTransportError& e) {
        EXPECT_EQ(e.failure(), mcp::MCPFailure::protocol);
    }
    EXPECT_FALSE(client.is_initialized());
}

#ifndef _WIN32
TEST(MCPStdioAsync, SpawnClosesInheritedHighFileDescriptor) {
    if (!python3_available()) GTEST_SKIP() << "python3 not available";
    int pipe_fds[2] = {-1, -1};
    ASSERT_EQ(::pipe(pipe_fds), 0);
    struct Fd {
        int value;
        ~Fd() { if (value >= 0) ::close(value); }
    } read_end{pipe_fds[0]}, write_end{pipe_fds[1]};
    Fd inherited{::fcntl(read_end.value, F_DUPFD, 512)};
    ASSERT_GE(inherited.value, 512);

    const char* script =
        "import json,os,sys\n"
        "fd=int(sys.argv[1])\n"
        "try:\n"
        "  os.fstat(fd)\n"
        "  clean=False\n"
        "except OSError:\n"
        "  clean=True\n"
        "r=json.loads(sys.stdin.readline())\n"
        "print(json.dumps({'jsonrpc':'2.0','id':r['id'],'result':"
        "{'protocolVersion':'2025-11-25','capabilities':{},"
        "'serverInfo':{'name':'fd-closed' if clean else 'fd-leaked',"
        "'version':'1'}}}),flush=True)\n"
        "sys.stdin.readline()\n";
    mcp::MCPClient client({python_cmd(), "-c", script,
                           std::to_string(inherited.value)});
    ASSERT_TRUE(client.initialize());
    EXPECT_EQ(client.get_initialize_result().server_info.value("name", ""),
              "fd-closed");
}
#endif

TEST(MCPStdioAsync, HardenedStartupUsesStartupRatherThanRequestTimeout) {
    if (!python3_available()) GTEST_SKIP() << "python not available";
    auto config = boundary_config("startup");
    config.startup_timeout = std::chrono::milliseconds(60);
    mcp::MCPClient client(std::move(config));
    try {
        client.initialize();
        FAIL() << "startup timeout was ignored";
    } catch (const mcp::MCPTransportError& error) {
        EXPECT_EQ(error.failure(), mcp::MCPFailure::timeout);
    }
    EXPECT_FALSE(client.is_initialized());
}

TEST(MCPStdioAsync, HardenedStderrOverflowTerminatesSession) {
    if (!python3_available()) GTEST_SKIP() << "python not available";
    auto config = boundary_config("stderr");
    config.max_stderr_bytes = 128;
    mcp::MCPClient client(std::move(config));
    EXPECT_THROW(client.initialize(), mcp::MCPTransportError);
    EXPECT_FALSE(client.is_initialized());
}

TEST(MCPStdioAsync, HardenedReplacementEnvironmentAndWorkingDirectory) {
    if (!python3_available()) GTEST_SKIP() << "python not available";
    BoundaryDirectory directory;
    constexpr auto key = "NEOGRAPH_MCP_SENTINEL_SECRET";
    struct RestoreEnvironment {
        std::optional<std::string> old;
        RestoreEnvironment() {
            if (const char* value = std::getenv("NEOGRAPH_MCP_SENTINEL_SECRET")) old = value;
#ifdef _WIN32
            _putenv_s("NEOGRAPH_MCP_SENTINEL_SECRET", "must-not-leak");
#else
            ::setenv("NEOGRAPH_MCP_SENTINEL_SECRET", "must-not-leak", 1);
#endif
        }
        ~RestoreEnvironment() {
#ifdef _WIN32
            _putenv_s("NEOGRAPH_MCP_SENTINEL_SECRET", old ? old->c_str() : "");
#else
            if (old) ::setenv("NEOGRAPH_MCP_SENTINEL_SECRET", old->c_str(), 1);
            else ::unsetenv("NEOGRAPH_MCP_SENTINEL_SECRET");
#endif
        }
    } restore;
    auto config = boundary_config("report");
    config.cwd = directory.path;
    config.replace_environment = true;
    config.environment = {{"NEOGRAPH_APPROVED_VALUE", "approved"}};
    // Use the same interpreter search path that python_cmd() checked above.
    if (const char* path = std::getenv("PATH"))
        config.environment.emplace_back("PATH", path);
#ifdef _WIN32
    if (const char* root = std::getenv("SystemRoot"))
        config.environment.emplace_back("SystemRoot", root);
#endif
    mcp::MCPClient client(std::move(config));
    auto result = client.call_tool("probe", json::object());
    EXPECT_TRUE(std::filesystem::equivalent(
        result.at("cwd").get<std::string>(), directory.path));
    EXPECT_FALSE(result.at("environment").contains(key));
    EXPECT_EQ(result.at("environment").value("NEOGRAPH_APPROVED_VALUE", ""), "approved");
}

TEST(MCPStdioAsync, HardenedRejectsAmbiguousReplacementEnvironment) {
    if (!python3_available()) GTEST_SKIP() << "python not available";
    for (const auto& environment : std::vector<mcp::HeaderList>{
             {{"BAD=KEY", "value"}}, {{"", "value"}},
             {{"KEY", std::string("value\0hidden", 12)}},
             {{"KEY", "one"}, {"KEY", "two"}}}) {
        auto config = boundary_config("report");
        config.replace_environment = true;
        config.environment = environment;
        EXPECT_THROW(mcp::MCPClient{std::move(config)}, std::invalid_argument);
    }
}

TEST(MCPStdioAsync, DescendantsAreTerminatedAfterLeaderExits) {
    if (!python3_available()) GTEST_SKIP() << "python not available";
    BoundaryDirectory directory;
    const auto marker = directory.path / "survived";
    {
        auto config = boundary_config("tree");
        config.argv.push_back(marker.string());
        mcp::MCPClient client(std::move(config));
        auto result = client.call_tool("probe", json::object());
        ASSERT_TRUE(result.at("childPid").is_number_integer());
        // The leader exits immediately after replying. Its child ignores TERM.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    EXPECT_FALSE(std::filesystem::exists(marker));
}

#ifdef _WIN32
TEST(MCPStdioAsync, HardenedSpawnDoesNotInheritUnrelatedWindowsHandles) {
    if (!python3_available()) GTEST_SKIP() << "python not available";
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE event = CreateEventW(&attributes, TRUE, FALSE, nullptr);
    ASSERT_NE(event, nullptr);
    struct CloseEvent {
        HANDLE value;
        ~CloseEvent() { CloseHandle(value); }
    } close{event};
    auto config = boundary_config("report");
    config.argv.push_back(std::to_string(reinterpret_cast<std::uintptr_t>(event)));
    config.argv.push_back("space and trailing slash\\");
    config.argv.push_back("backslash\\\"quote");
    config.argv.push_back("");
    mcp::MCPClient client(std::move(config));
    auto result = client.call_tool("probe", json::object());
    // A numeric handle can be reused in the child; observe the original object.
    EXPECT_EQ(WaitForSingleObject(event, 0), WAIT_TIMEOUT);
    EXPECT_EQ(result.at("argv"),
              json::array({"space and trailing slash\\", "backslash\\\"quote", ""}));
}
#endif

TEST(MCPStdioAsync, ExplicitShutdownAbortsRetainedCallsAndTools) {
    if (!python3_available()) GTEST_SKIP() << "python not available";
    BoundaryDirectory directory;
    auto config = boundary_config("blocked");
    const auto entered = directory.path / "entered";
    config.argv.push_back(entered.string());
    mcp::MCPClient client(std::move(config));
    auto tools = client.get_tools();
    ASSERT_EQ(tools.size(), 1u);
    auto pending = std::async(std::launch::async, [&]() -> std::optional<mcp::MCPFailure> {
        try { client.call_tool("probe", json::object()); }
        catch (const mcp::MCPTransportError& error) { return error.failure(); }
        return std::nullopt;
    });
    const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!std::filesystem::exists(entered) && std::chrono::steady_clock::now() < ready_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ASSERT_TRUE(std::filesystem::exists(entered));
    std::jthread concurrent_shutdown([&] { client.shutdown(); });
    client.shutdown();
    concurrent_shutdown.join();
    EXPECT_EQ(pending.get(), mcp::MCPFailure::shutdown);
    EXPECT_FALSE(client.is_initialized());
    auto* retained = dynamic_cast<mcp::MCPTool*>(tools.front().get());
    ASSERT_NE(retained, nullptr);
    try {
        retained->execute_result(json::object());
        FAIL() << "retained tool dispatched after shutdown";
    } catch (const mcp::MCPTransportError& error) {
        EXPECT_EQ(error.failure(), mcp::MCPFailure::shutdown);
    }
    try {
        client.initialize();
        FAIL() << "shutdown client reopened";
    } catch (const mcp::MCPTransportError& error) {
        EXPECT_EQ(error.failure(), mcp::MCPFailure::shutdown);
    }
}

TEST(MCPStdioAsync, ShutdownBeforeFirstRequestCannotRestartTransport) {
    if (!python3_available()) GTEST_SKIP() << "python not available";
    mcp::MCPClient client(boundary_config("report"));
    client.shutdown();
    client.shutdown();
    try {
        async::run_sync(client.rpc_call_async("tools/list"));
        FAIL() << "request restarted a shut-down transport";
    } catch (const mcp::MCPTransportError& error) {
        EXPECT_EQ(error.failure(), mcp::MCPFailure::shutdown);
    }
}

TEST(MCPStdioAsync, HardenedCancellationDrainsDescendantsAndClosesAdmission) {
    if (!python3_available()) GTEST_SKIP() << "python not available";
    BoundaryDirectory directory;
    const auto marker = directory.path / "survived";
    const auto ready = std::filesystem::path(marker.string() + ".ready");
    auto config = boundary_config("tree-blocked");
    config.argv.push_back(marker.string());
    mcp::MCPClient client(std::move(config));
    ASSERT_TRUE(client.initialize());
    auto cancel = std::make_shared<graph::CancelToken>();
    auto pending = std::async(std::launch::async, [&]() -> std::optional<mcp::MCPFailure> {
        try {
            async::run_sync(client.rpc_call_async("tools/call", json::object(),
                std::chrono::steady_clock::time_point::max(), cancel));
        } catch (const mcp::MCPTransportError& error) { return error.failure(); }
        return std::nullopt;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!std::filesystem::exists(ready) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ASSERT_TRUE(std::filesystem::exists(ready));
    cancel->cancel();
    EXPECT_EQ(pending.get(), mcp::MCPFailure::cancelled);
    try {
        client.call_tool("probe", json::object());
        FAIL() << "cancelled hardened process was reused";
    } catch (const mcp::MCPTransportError& error) {
        EXPECT_EQ(error.failure(), mcp::MCPFailure::shutdown);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    EXPECT_FALSE(std::filesystem::exists(marker));
}
