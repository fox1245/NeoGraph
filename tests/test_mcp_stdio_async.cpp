// End-to-end MCP protocol + stdio transport coverage. The protocol
// session owns JSON-RPC envelopes, initialization and tool adaptation.
// The stdio transport owns subprocess pipes, response-id demultiplexing,
// cancellation, and shutdown. Fixtures are real stdlib-only Python
// servers, with no third-party MCP dependency.

#include "fixtures/test_watchdog.h"
#include "fixtures/unique_temp_dir.h"

#include <gtest/gtest.h>
#include <neograph/mcp/client.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/cancel.h>
#include <neograph/tool_dispatch.h>
#include <neograph/tool_execution.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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

// These tests talk to a real subprocess; one that hangs ends the process with
// a message after 30 s instead of timing out CTest after 10 minutes.
[[maybe_unused]] const bool kWatched =
    neograph::graph::test_support::watch_tests({"MCPStdioAsync"});

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
    std::filesystem::path path = neograph::graph::test_support::unique_temp_path("neograph-mcp-boundary-");
    BoundaryDirectory() { std::filesystem::create_directory(path); }
    ~BoundaryDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

std::filesystem::path noise_fixture() {
    return std::filesystem::path(__FILE__).parent_path() / "fixtures" / "mcp_stdio_noise.py";
}

mcp::StdioClientConfig noise_config(const std::string& mode, int count = 0) {
    mcp::StdioClientConfig config;
    config.argv = {python_cmd(), noise_fixture().string(), mode, std::to_string(count)};
    config.request_timeout = std::chrono::milliseconds(3000);
    return config;
}

mcp::MCPFailure failure_of_initialize(mcp::MCPClient& client) {
    try {
        client.initialize();
    } catch (const mcp::MCPTransportError& e) {
        return e.failure();
    }
    ADD_FAILURE() << "initialize succeeded";
    return mcp::MCPFailure::connection;
}

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

TEST(MCPStdioAsync, ShutdownDoesNotWaitForAServerThatStaysAlive) {
    // After a call the session's reader stays suspended on the server's
    // stdout. Shutdown has to cancel that read itself: on Windows closing the
    // wrapper's handle did not, the I/O thread never ran out of work, and
    // shutdown() waited for a server that was never going to exit, so every
    // test with a live server timed out. This server answers initialize and
    // then neither exits nor writes again, even after its stdin is closed.
    if (!python3_available()) GTEST_SKIP() << "python3 not available";
    const char* script =
        "import json,sys,time\n"
        "for line in sys.stdin:\n"
        "  m=json.loads(line)\n"
        "  if m.get('method')=='initialize':\n"
        "    print(json.dumps({'jsonrpc':'2.0','id':m['id'],'result':{"
        "'protocolVersion':'2025-11-25','capabilities':{},"
        "'serverInfo':{'name':'idle','version':'1'}}}),flush=True)\n"
        "time.sleep(60)\n";
    mcp::MCPClient client({python_cmd(), "-c", script});
    ASSERT_TRUE(client.initialize("idle-server"));
    const auto started = std::chrono::steady_clock::now();
    client.shutdown();
    // The server gets about 2 s to exit before it is terminated.
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(10));
}

TEST(MCPStdioAsync, AReplyIsDeliveredWhenTheServerExitsRightAfterIt) {
    // A hardened session ends when the server's stdout closes. The reader sees
    // that as soon as it has routed the reply, and the caller's coroutine may
    // not have resumed yet (here its thread is busy while the reply comes):
    // the reply still belongs to the call. It used to fail with "shut down".
    if (!python3_available()) GTEST_SKIP() << "python3 not available";
    const char* script =
        "import json,sys,time\n"
        "def send(o): print(json.dumps(o),flush=True)\n"
        "for line in sys.stdin:\n"
        "  m=json.loads(line)\n"
        "  if 'id' not in m: continue\n"
        "  if m['method']=='initialize':\n"
        "    send({'jsonrpc':'2.0','id':m['id'],'result':{"
        "'protocolVersion':'2025-11-25','capabilities':{'tools':{}},"
        "'serverInfo':{'name':'once','version':'1'}}})\n"
        "  else:\n"
        "    time.sleep(0.3)\n"
        "    send({'jsonrpc':'2.0','id':m['id'],'result':{"
        "'content':[{'type':'text','text':'done'}]}})\n"
        "    sys.exit(0)\n";
    mcp::StdioClientConfig config;
    config.argv = {python_cmd(), "-c", script};
    mcp::MCPClient client(std::move(config));
    ASSERT_TRUE(client.initialize("exits-after-reply"));

    json call;
    call["name"] = "probe";
    call["arguments"] = json::object();
    asio::io_context io;
    json reply;
    std::string failure;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            asio::co_spawn(
                io,
                [&]() -> asio::awaitable<void> {
                    try {
                        reply = co_await client.rpc_call_async("tools/call", call);
                    } catch (const std::exception& error) {
                        failure = error.what();
                    }
                },
                asio::detached);
            // Let the request go out; then keep this thread busy while the
            // server (which waits 0.3 s) replies and exits.
            asio::steady_timer pause(io, std::chrono::milliseconds(100));
            co_await pause.async_wait(asio::use_awaitable);
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
        },
        asio::detached);
    io.run();

    EXPECT_EQ(failure, "");
    ASSERT_TRUE(reply.is_object()) << "no reply";
    EXPECT_EQ(reply.at("content").at(0).at("text"), "done");
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

#ifndef _WIN32
// SIGPIPE's default action for the scope of a test. cpp-httplib's Server
// sets SIG_IGN for the whole process, which would hide a SIGPIPE from every
// test that runs after one; a host does not have to ignore it.
class DefaultSigpipe {
public:
    DefaultSigpipe() : previous_(std::signal(SIGPIPE, SIG_DFL)) {}
    ~DefaultSigpipe() { std::signal(SIGPIPE, previous_); }
    DefaultSigpipe(const DefaultSigpipe&) = delete;
    DefaultSigpipe& operator=(const DefaultSigpipe&) = delete;

private:
    void (*previous_)(int);
};

TEST(MCPStdioAsync, AServerThatClosedItsInputFailsWithoutASignal) {
    // #495: the write of initialize fails with EPIPE. macOS raised SIGPIPE
    // for the process, and this thread, which does not block it, ended the
    // test.
    DefaultSigpipe default_action;
    mcp::MCPClient client({"/bin/sh", "-c", "exec 0<&-; sleep 2"});
    // The client starts the server here; let it close its stdin first, so
    // that the write of initialize finds no reader.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    try {
        client.initialize();
        FAIL() << "expected a broken pipe";
    } catch (const mcp::MCPTransportError& e) {
        EXPECT_EQ(e.failure(), mcp::MCPFailure::connection);
    }
    EXPECT_FALSE(client.is_initialized());
}
#endif

TEST(MCPStdioAsync, MalformedSubprocessFrameIsAProtocolFailure) {
    if (!python3_available()) GTEST_SKIP() << "python3 not available";
    mcp::MCPClient client({
        python_cmd(), "-c",
        "import sys; sys.stdin.readline(); print('{\"jsonrpc\": \"2.0\", \"id\": 1, \"res', flush=True)",
    });
    try {
        client.initialize();
        FAIL() << "expected invalid stdio frame";
    } catch (const mcp::MCPTransportError& e) {
        EXPECT_EQ(e.failure(), mcp::MCPFailure::protocol);
    }
}

// A server prints a banner or a log line to stdout although only messages
// belong there. Such text is skipped; anything that could be a damaged message
// still ends the session at once, since skipping it would leave the call it
// answers waiting for its timeout.
TEST(MCPStdioAsync, TextOnStdoutThatIsNotJsonIsSkipped) {
    if (!python3_available()) GTEST_SKIP() << "python3 not available";
    mcp::MCPClient client({python_cmd(), noise_fixture().string(), "banner"});
    ASSERT_TRUE(client.initialize());
    EXPECT_EQ(client.get_initialize_result().server_info.value("name", ""), "noise");
    const auto tools = client.get_tools();
    ASSERT_EQ(tools.size(), 1u);
    EXPECT_EQ(tools[0]->get_name(), "probe");
}

TEST(MCPStdioAsync, NonJsonTextBeforeAnEndOfFileIsAConnectionFailure) {
    if (!python3_available()) GTEST_SKIP() << "python3 not available";
    mcp::MCPClient client({
        python_cmd(), "-c",
        "import sys; sys.stdin.readline(); print('not-json', flush=True)",
    });
    EXPECT_EQ(failure_of_initialize(client), mcp::MCPFailure::connection);
}

TEST(MCPStdioAsync, ADamagedMessageOnStdoutIsNeverSkipped) {
    if (!python3_available()) GTEST_SKIP() << "python3 not available";
    for (const char* mode : {"cut", "split", "prefixed", "scalar"}) {
        SCOPED_TRACE(mode);
        mcp::MCPClient client({python_cmd(), noise_fixture().string(), mode});
        EXPECT_EQ(failure_of_initialize(client), mcp::MCPFailure::protocol);
    }
}

TEST(MCPStdioAsync, SkippedStdoutTextIsBoundedByLines) {
    if (!python3_available()) GTEST_SKIP() << "python3 not available";
    auto config = noise_config("flood", 5);
    config.max_stdout_text_lines = 4;
    mcp::MCPClient client(config);
    try {
        client.initialize();
        FAIL() << "five lines of text passed a limit of four";
    } catch (const mcp::MCPTransportError& e) {
        EXPECT_EQ(e.failure(), mcp::MCPFailure::protocol);
        EXPECT_NE(std::string(e.what()).find("not JSON-RPC"), std::string::npos) << e.what();
    }

    // Exactly the limit is still skipped.
    config.max_stdout_text_lines = 5;
    mcp::MCPClient within(config);
    EXPECT_TRUE(within.initialize());
}

TEST(MCPStdioAsync, SkippedStdoutTextIsBoundedByBytes) {
    if (!python3_available()) GTEST_SKIP() << "python3 not available";
    // "log line N\n" is 11 bytes.
    auto config = noise_config("flood", 5);
    config.max_stdout_text_bytes = 54;
    mcp::MCPClient client(config);
    EXPECT_EQ(failure_of_initialize(client), mcp::MCPFailure::protocol);

    config.max_stdout_text_bytes = 55;
    mcp::MCPClient within(config);
    EXPECT_TRUE(within.initialize());
}

TEST(MCPStdioAsync, ZeroLimitSkipsNoStdoutText) {
    if (!python3_available()) GTEST_SKIP() << "python3 not available";
    for (int which = 0; which < 2; ++which) {
        auto config = noise_config("banner");
        (which == 0 ? config.max_stdout_text_lines : config.max_stdout_text_bytes) = 0;
        mcp::MCPClient client(config);
        EXPECT_EQ(failure_of_initialize(client), mcp::MCPFailure::protocol);
    }
}

TEST(MCPStdioAsync, FirstSkippedStdoutLineIsReportedOnceWithoutQuotingIt) {
    if (!python3_available()) GTEST_SKIP() << "python3 not available";
    testing::internal::CaptureStderr();
    {
        mcp::MCPClient client(noise_config("flood", 5));
        ASSERT_TRUE(client.initialize());
        client.shutdown();
    }
    const auto err = testing::internal::GetCapturedStderr();
    const std::string warning = "an MCP stdio server wrote text that is not JSON-RPC";
    const auto first = err.find(warning);
    ASSERT_NE(first, std::string::npos) << err;
    EXPECT_EQ(err.find(warning, first + 1), std::string::npos) << "reported more than once";
    EXPECT_EQ(err.find("log line"), std::string::npos) << "the server's text was quoted";
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

TEST(MCPStdioAsync, AServerThatAnswers20241105IsUsable) {
    // MCP 2024-11-05 servers are still deployed, and the official SDK clients
    // still accept that revision; so does this client, over stdio.
    if (!python3_available()) GTEST_SKIP() << "python not available";
    const char* script =
        "import json,sys\n"
        "for line in sys.stdin:\n"
        "  m=json.loads(line)\n"
        "  if 'id' not in m or 'method' not in m: continue\n"
        "  if m['method']=='initialize':\n"
        "    r={'protocolVersion':'2024-11-05','capabilities':{'tools':{}},"
        "'serverInfo':{'name':'old','version':'1'}}\n"
        "  elif m['method']=='tools/list':\n"
        "    r={'tools':[{'name':'echo','description':'Echo',"
        "'inputSchema':{'type':'object','properties':{'text':{'type':'string'}}}}]}\n"
        "  elif m['method']=='tools/call':\n"
        "    r={'content':[{'type':'text','text':m['params']['arguments']['text']}]}\n"
        "  else:\n"
        "    print(json.dumps({'jsonrpc':'2.0','id':m['id'],'error':"
        "{'code':-32601,'message':'no'}}),flush=True); continue\n"
        "  print(json.dumps({'jsonrpc':'2.0','id':m['id'],'result':r}),flush=True)\n";
    mcp::MCPClient client({python_cmd(), "-c", script});
    ASSERT_TRUE(client.initialize("old-server"));
    EXPECT_EQ(client.get_initialize_result().protocol_version, "2024-11-05");
    const auto tools = client.get_tools();
    ASSERT_EQ(tools.size(), 1u);
    EXPECT_EQ(tools[0]->get_name(), "echo");
    EXPECT_EQ(tools[0]->execute(json{{"text", "hi"}}), "hi");
}

TEST(MCPStdioAsync, ABatchFromTheServerIsHandledMessageByMessage) {
    // MCP 2025-03-26 lets a peer send a JSON-RPC batch. Each message in it is
    // handled as if it came alone (a response completes its call, a
    // notification reaches its handler), and the replies to the requests in
    // it go back as one array.
    if (!python3_available()) GTEST_SKIP() << "python not available";
    const char* script =
        "import json,sys\n"
        "answered=False\n"
        "def send(o): print(json.dumps(o),flush=True)\n"
        "def note(m):\n"
        "  global answered\n"
        "  if isinstance(m,list):\n"
        "    if any(x.get('id')=='s1' and x.get('result')=={} for x in m): answered='array'\n"
        "  elif m.get('id')=='s1' and 'method' not in m: answered='single'\n"
        "for line in sys.stdin:\n"
        "  m=json.loads(line)\n"
        "  if isinstance(m,list) or 'method' not in m: note(m); continue\n"
        "  if 'id' not in m: continue\n"
        "  if m['method']=='initialize':\n"
        "    send({'jsonrpc':'2.0','id':m['id'],'result':{'protocolVersion':'2025-03-26',"
        "'capabilities':{'tools':{'listChanged':True}},'serverInfo':{'name':'batch','version':'1'}}})\n"
        "  elif m['method']=='tools/list':\n"
        "    send([{'jsonrpc':'2.0','id':'s1','method':'ping'},"
        "{'jsonrpc':'2.0','method':'notifications/tools/list_changed'},"
        "{'jsonrpc':'2.0','id':m['id'],'result':{'tools':[{'name':'echo',"
        "'inputSchema':{'type':'object'}}]}}])\n"
        "  elif m['method']=='tools/call':\n"
        "    while answered is False:\n"
        "      note(json.loads(sys.stdin.readline()))\n"
        "    send({'jsonrpc':'2.0','id':m['id'],'result':{'content':[{'type':'text','text':answered}]}})\n";
    mcp::MCPClient client({python_cmd(), "-c", script});
    std::atomic<int> changes{0};
    client.set_tool_list_changed_handler([&] { ++changes; });
    ASSERT_TRUE(client.initialize("batch"));
    const auto definitions = client.get_tool_definitions();
    ASSERT_EQ(definitions.size(), 1u);
    EXPECT_EQ(definitions[0].name, "echo");
    const auto result = client.call_tool_result("echo", json::object());
    EXPECT_EQ(result.content[0]["text"], "array") << "the ping in the batch was answered alone";
    for (int i = 0; i < 400 && changes.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_GE(changes.load(), 1);
}

TEST(MCPStdioAsync, ATimeoutWhileTheResponseArrivesIsATimeout) {
    // A call that runs out of time while its response line is still being
    // written fails as a timeout and tells the server; the line that ends
    // later is a late response and is dropped, and the session goes on.
    if (!python3_available()) GTEST_SKIP() << "python not available";
    const char* script =
        "import json,sys\n"
        "cancelled=[]\n"
        "def send(o): print(json.dumps(o),flush=True)\n"
        "for line in sys.stdin:\n"
        "  m=json.loads(line)\n"
        "  if 'id' not in m or 'method' not in m: continue\n"
        "  if m['method']=='initialize':\n"
        "    send({'jsonrpc':'2.0','id':m['id'],'result':{'protocolVersion':'2025-11-25',"
        "'capabilities':{'tools':{}},'serverInfo':{'name':'stall','version':'1'}}})\n"
        "  elif m['params']['name']=='slow':\n"
        "    full=json.dumps({'jsonrpc':'2.0','id':m['id'],'result':{'content':[{'type':'text','text':'late'}]}})\n"
        "    sys.stdout.write(full[:len(full)//2]); sys.stdout.flush()\n"
        // The rest of the line follows the client's next message, which a
        // client that gave up sends first: notifications/cancelled.
        "    note=json.loads(sys.stdin.readline())\n"
        "    if note.get('method')=='notifications/cancelled': cancelled.append(note['params']['requestId'])\n"
        "    sys.stdout.write(full[len(full)//2:]+'\\n'); sys.stdout.flush()\n"
        "  else:\n"
        "    send({'jsonrpc':'2.0','id':m['id'],'result':{'content':[{'type':'text','text':json.dumps(cancelled)}]}})\n";
    mcp::MCPClient client({python_cmd(), "-c", script});
    ASSERT_TRUE(client.initialize("stall"));
    const auto started = std::chrono::steady_clock::now();
    try {
        async::run_sync(client.call_tool_result_async(
            "slow", json::object(), std::chrono::steady_clock::now() + std::chrono::milliseconds(300),
            nullptr));
        ADD_FAILURE() << "the stalled call returned";
    } catch (const mcp::MCPTransportError& error) {
        EXPECT_EQ(error.failure(), mcp::MCPFailure::timeout) << error.what();
    } catch (const std::exception& error) {
        ADD_FAILURE() << "not a transport timeout: " << error.what();
    }
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(2500));
    // The late rest of the line did not disturb the session, and the server
    // heard of the abandoned request.
    const auto after = async::run_sync(client.call_tool_result_async(
        "echo", json::object(), std::chrono::steady_clock::now() + std::chrono::seconds(10),
        nullptr));
    const auto cancelled = json::parse(after.content[0]["text"].get<std::string>());
    ASSERT_EQ(cancelled.size(), 1u) << cancelled.dump();
}

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

TEST(MCPStdioAsync, HardenedStderrVolumeDoesNotEndTheSession) {
    // MCP-C-05: stderr is drained and its tail kept; volume never ends the
    // session (MCP: a client SHOULD NOT treat stderr as an error).
    if (!python3_available()) GTEST_SKIP() << "python not available";
    auto config = boundary_config("stderr");
    config.max_stderr_bytes = 128;
    mcp::MCPClient client(std::move(config));
    EXPECT_TRUE(client.initialize());
    EXPECT_NO_THROW(client.call_tool("probe", json::object()));
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
    // The server is told and gets this grace period before it is terminated.
    config.cancel_grace = std::chrono::milliseconds(100);
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

// A run's cancel token and deadline bound an in-flight MCP tool call made
// through any dispatcher (reference, "Tool failure contract"). Cancellation
// is classified as cancellation, not a tool error; the deadline as a
// timed-out call; the late response is dropped and never answers a later
// call.
TEST(MCPStdioAsync, DispatchBoundsAnInFlightToolCall) {
    if (!python3_available()) GTEST_SKIP() << "python3 not available";
    auto fixture = fixture_path().parent_path() / "mcp_stdio_slow.py";
    ASSERT_TRUE(std::filesystem::exists(fixture));
    mcp::MCPClient client({python_cmd(), fixture.string()});
    auto tools = client.get_tools();
    ASSERT_EQ(tools.size(), 1u);
    Tool& echo = *tools[0];
    const json slow{{"marker", "late"}, {"delay_ms", 3000}};
    auto controller = std::make_shared<ToolExecutionController>();

    // The controller's typed result: cancellation after the call started.
    {
        auto cancel = std::make_shared<graph::CancelToken>();
        ToolExecutionContext context;
        context.cancel_token = cancel;
        context.controller = controller;
        std::jthread stop([cancel] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            cancel->cancel();
        });
        const auto started = std::chrono::steady_clock::now();
        const auto result = async::run_sync(controller->execute_result_async(echo, slow, context));
        EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2))
            << "the call waited for the server instead of its cancel token";
        EXPECT_EQ(result.status, ToolTerminalStatus::CancellationRequested) << result.error;
    }

    // dispatch_tool_calls: the run stops as cancelled, one call or a batch.
    for (const bool batch : {false, true}) {
        auto cancel = std::make_shared<graph::CancelToken>();
        ToolExecutionContext context;
        context.cancel_token = cancel;
        context.controller = controller;
        std::vector<ToolCall> calls{{"c1", "echo", slow.dump()}};
        if (batch) calls.push_back({"c2", "echo", slow.dump()});
        std::jthread stop([cancel] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            cancel->cancel();
        });
        const auto started = std::chrono::steady_clock::now();
        EXPECT_THROW(async::run_sync(dispatch_tool_calls(calls, {&echo}, {}, {}, context)),
                     graph::CancelledException) << (batch ? "batch" : "single");
        EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
    }

    // The run's deadline: a timed-out call, not an ordinary failure.
    {
        ToolExecutionContext context;
        context.controller = controller;
        context.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        const auto started = std::chrono::steady_clock::now();
        const auto messages = async::run_sync(dispatch_tool_calls(
            {{"c3", "echo", slow.dump()}}, {&echo}, {}, {}, context));
        EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
        ASSERT_EQ(messages.size(), 1u);
        EXPECT_EQ(messages[0].tool_status, "timed_out") << messages[0].content;
    }

    // The late responses are discarded: a later call gets its own.
    const auto later = async::run_sync(dispatch_tool_calls(
        {{"c4", "echo", R"({"marker":"next"})"}}, {&echo}, {}, {},
        ToolExecutionContext{nullptr, controller}));
    ASSERT_EQ(later.size(), 1u);
    ASSERT_EQ(later[0].tool_status, "succeeded") << later[0].content;
    EXPECT_EQ(json::parse(later[0].content).at("args").at("marker"), "next");

    // Every call the client abandoned after sending it was announced with
    // notifications/cancelled: the controller's, one per dispatch (the
    // batch's second call never left its queue) and the timed-out one.
    const auto cancellations = client.call_tool_result("cancellations", json::object());
    const auto ids = json::parse(cancellations.content.at(0).at("text").get<std::string>());
    EXPECT_EQ(ids.size(), 4u) << ids.dump();
}

namespace {
std::filesystem::path conformance_fixture() {
    return fixture_path().parent_path() / "mcp_stdio_conformance.py";
}

std::string read_record(const std::filesystem::path& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool wait_for_record(const std::filesystem::path& path, const std::string& text,
                     std::chrono::milliseconds limit) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until) {
        if (read_record(path).find(text) != std::string::npos) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return read_record(path).find(text) != std::string::npos;
}
}  // namespace

// MCP-C-02, MCP-C-06: a server request is answered (ping with an empty result,
// anything undeclared with -32601), even when it reuses the id of the call in
// flight, which still gets its own response.
TEST(MCPStdioAsync, ServerRequestsAreAnsweredAndKeptApartFromResponses) {
    if (!python3_available()) GTEST_SKIP() << "python not available";
    BoundaryDirectory directory;
    mcp::MCPClient client(std::vector<std::string>{
        python_cmd(), conformance_fixture().string(), (directory.path / "record").string()});
    auto result = client.call_tool_result("ping_back", json::object());
    ASSERT_FALSE(result.is_error);
    const auto replies = json::parse(result.content[0]["text"].get<std::string>());
    ASSERT_EQ(replies.size(), 2u);
    bool ping = false, roots = false;
    for (std::size_t i = 0; i < replies.size(); ++i) {
        if (replies[i]["id"] == json("r-1")) {
            roots = replies[i]["error"]["code"] == json(-32601);
        } else {
            ping = replies[i]["result"] == json::object();
        }
    }
    EXPECT_TRUE(ping) << replies.dump();
    EXPECT_TRUE(roots) << replies.dump();
}

// MCP-C-12: shutting a stdio server down closes its stdin first; a server
// that exits on EOF never sees a signal.
TEST(MCPStdioAsync, ShutdownClosesStdinBeforeAnySignal) {
    if (!python3_available()) GTEST_SKIP() << "python not available";
    BoundaryDirectory directory;
    const auto record = directory.path / "record";
    const auto started = std::chrono::steady_clock::now();
    {
        mcp::MCPClient client(std::vector<std::string>{
            python_cmd(), conformance_fixture().string(), record.string()});
        ASSERT_TRUE(client.initialize());
    }
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(3));
    EXPECT_TRUE(wait_for_record(record, "eof", std::chrono::milliseconds(500)));
    EXPECT_EQ(read_record(record).find("sigterm"), std::string::npos) << read_record(record);
}

// The same for a hardened session, whose bound is its configured
// request_timeout: the server is told, then stopped.
TEST(MCPStdioAsync, HardenedTimeoutWhileTheResponseArrivesIsATimeout) {
    if (!python3_available()) GTEST_SKIP() << "python not available";
    BoundaryDirectory directory;
    const auto record = directory.path / "record";
    const char* script =
        "import json,sys\n"
        "def send(o): print(json.dumps(o),flush=True)\n"
        "for line in sys.stdin:\n"
        "  m=json.loads(line)\n"
        "  if 'id' not in m or 'method' not in m: continue\n"
        "  if m['method']=='initialize':\n"
        "    send({'jsonrpc':'2.0','id':m['id'],'result':{'protocolVersion':'2025-11-25',"
        "'capabilities':{'tools':{}},'serverInfo':{'name':'stall','version':'1'}}})\n"
        "  else:\n"
        "    full=json.dumps({'jsonrpc':'2.0','id':m['id'],'result':{'content':[{'type':'text','text':'late'}]}})\n"
        "    sys.stdout.write(full[:len(full)//2]); sys.stdout.flush()\n"
        "    note=sys.stdin.readline()\n"
        "    open(sys.argv[1],'a').write('cancelled\\n' if 'notifications/cancelled' in note else 'eof\\n')\n"
        "    sys.stdout.write(full[len(full)//2:]+'\\n'); sys.stdout.flush()\n";
    mcp::StdioClientConfig config;
    config.argv = {python_cmd(), "-c", script, record.string()};
    config.request_timeout = std::chrono::milliseconds(300);
    config.cancel_grace = std::chrono::milliseconds(300);
    mcp::MCPClient client(std::move(config));
    ASSERT_TRUE(client.initialize("stall"));
    const auto started = std::chrono::steady_clock::now();
    try {
        client.call_tool_result("slow", json::object());
        ADD_FAILURE() << "the stalled call returned";
    } catch (const mcp::MCPTransportError& error) {
        EXPECT_EQ(error.failure(), mcp::MCPFailure::timeout) << error.what();
    } catch (const std::exception& error) {
        ADD_FAILURE() << "not a transport timeout: " << error.what();
    }
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(2500));
    EXPECT_TRUE(wait_for_record(record, "cancelled", std::chrono::milliseconds(5000)))
        << read_record(record);
}

// Hardened stdio: a cancelled call sends notifications/cancelled first, waits
// a short grace period, then terminates the server; the session takes no more
// requests.
TEST(MCPStdioAsync, HardenedCancellationTellsTheServerBeforeTerminating) {
    if (!python3_available()) GTEST_SKIP() << "python not available";
    BoundaryDirectory directory;
    const auto record = directory.path / "record";
    mcp::StdioClientConfig config;
    config.argv = {python_cmd(), conformance_fixture().string(), record.string()};
    config.cancel_grace = std::chrono::milliseconds(300);
    mcp::MCPClient client(std::move(config));
    ASSERT_TRUE(client.initialize());
    auto cancel = std::make_shared<graph::CancelToken>();
    std::thread stop([cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        cancel->cancel();
    });
    try {
        async::run_sync(client.call_tool_result_async(
            "slow", json::object(), std::chrono::steady_clock::time_point::max(), cancel));
        FAIL() << "the call was not cancelled";
    } catch (const mcp::MCPTransportError& error) {
        EXPECT_EQ(error.failure(), mcp::MCPFailure::cancelled);
    }
    stop.join();
    // The server heard of the cancellation before it was stopped.
    EXPECT_TRUE(wait_for_record(record, "cancelled", std::chrono::milliseconds(2000)))
        << read_record(record);
    EXPECT_TRUE(wait_for_record(record, "eof", std::chrono::milliseconds(2000)) ||
                read_record(record).find("sigterm") != std::string::npos)
        << read_record(record);
    try {
        client.call_tool("echo", json::object());
        FAIL() << "a cancelled hardened session took another request";
    } catch (const mcp::MCPTransportError& error) {
        EXPECT_EQ(error.failure(), mcp::MCPFailure::shutdown);
    }
}

// Without replace_environment, configured entries add to or override the
// inherited environment (on Windows: the block built from GetEnvironmentStringsW).
TEST(MCPStdioAsync, ConfiguredEnvironmentEntriesOverrideTheInheritedOnes) {
    if (!python3_available()) GTEST_SKIP() << "python not available";
    const auto set = [](const char* name, const char* value) {
#ifdef _WIN32
        _putenv_s(name, value);
#else
        ::setenv(name, value, 1);
#endif
    };
    const auto unset = [](const char* name) {
#ifdef _WIN32
        _putenv_s(name, "");
#else
        ::unsetenv(name);
#endif
    };
    set("NG_MCP_INHERITED", "parent");
    set("NG_MCP_OVERRIDDEN", "parent");
    auto config = boundary_config("echo");
    config.environment = {{"NG_MCP_OVERRIDDEN", "child"}, {"NG_MCP_ADDED", "added"}};
    mcp::MCPClient client(std::move(config));
    auto result = client.call_tool("probe", json::object());
    const auto environment = result.at("environment");
    EXPECT_EQ(environment.value("NG_MCP_INHERITED", ""), "parent");
    EXPECT_EQ(environment.value("NG_MCP_OVERRIDDEN", ""), "child");
    EXPECT_EQ(environment.value("NG_MCP_ADDED", ""), "added");
    EXPECT_TRUE(environment.contains("PATH"));
#ifdef _WIN32
    // The child can start only with the system variables the merge kept.
    EXPECT_TRUE(environment.contains("SYSTEMROOT")) << environment.dump();
#endif
    unset("NG_MCP_INHERITED");
    unset("NG_MCP_OVERRIDDEN");
}
