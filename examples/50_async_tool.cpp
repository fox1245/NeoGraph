// NeoGraph Example 45: AsyncTool — coroutine-shaped tool execution
//
// Tools that fetch over the network, RPC into MCP, or query an async
// DB are naturally awaitable. AsyncTool gives them a coroutine slot
// (`execute_async`) and adapts to the sync `Tool::execute` contract
// automatically — so the Agent / ToolDispatchNode that expects a
// blocking call still works without touching the tool surface.
//
// Demonstrates:
//   - subclass AsyncTool, override execute_async
//   - call sync execute() — driver fires up a private io_context
//   - and (optional) await execute_async directly from a coroutine
//
// No API key. Uses asio::steady_timer to fake "I/O latency" so the
// awaitable nature is observable in the timing.
//
// Usage: ./example_async_tool

#include <neograph/neograph.h>
#include <neograph/tool.h>
#include <neograph/async/run_sync.h>

#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>

#include <chrono>
#include <iostream>
#include <string>

using namespace neograph;

class FakeFetchTool : public AsyncTool {
public:
    ChatTool get_definition() const override {
        return {"fetch", "Fetch a URL (mocked)", json{{"type", "object"}}};
    }
    std::string get_name() const override { return "fetch"; }

    asio::awaitable<std::string> execute_async(const json& args) override {
        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer t(ex);
        t.expires_after(std::chrono::milliseconds(80));
        co_await t.async_wait(asio::use_awaitable);

        std::string url = args.value("url", "<missing>");
        co_return std::string("fetched: ") + url;
    }
};

int main() {
    FakeFetchTool tool;

    // ── A. Sync facade ─────────────────────────────────────────────
    // AsyncTool::execute(...) wraps execute_async in run_sync so the
    // Tool consumer (Agent / ToolDispatchNode) doesn't have to know
    // anything is async.
    auto t0 = std::chrono::steady_clock::now();
    std::string r1 = tool.execute({{"url", "https://example.com"}});
    auto el_sync = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::cout << "sync   result=\"" << r1 << "\" elapsed_ms=" << el_sync << "\n";

    // ── B. Native async ────────────────────────────────────────────
    // Drive execute_async on a private io_context via the public
    // run_sync helper — this is the same machinery AsyncTool::execute
    // uses internally, exposed so user code can mix the two styles.
    //
    // (A second co_await on top of execute_async via co_spawn was the
    // ideal shape here, but GCC 13's coroutine front-end ICEs on it —
    // bug filed in this repo's toolchain notes. run_sync gives the
    // same observable behaviour without the front-end hit.)
    auto t1 = std::chrono::steady_clock::now();
    std::string r2 = neograph::async::run_sync(
        tool.execute_async({{"url", "https://other.example"}}));
    auto el_async = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t1).count();
    std::cout << "async  result=\"" << r2 << "\" elapsed_ms=" << el_async << "\n";

    // Both should report the latency (~80 ms) and produce identical content.
    bool ok = true;
    if (r1.find("https://example.com") == std::string::npos) {
        std::cerr << "FAIL: sync path lost arg\n"; ok = false;
    }
    if (r2.find("https://other.example") == std::string::npos) {
        std::cerr << "FAIL: async path lost arg\n"; ok = false;
    }
    if (el_sync < 70 || el_async < 70) {
        std::cerr << "FAIL: I/O delay swallowed (sync=" << el_sync
                  << "ms async=" << el_async << "ms)\n"; ok = false;
    }
    // No upper-bound gate — CI noise can spike timer wakes.

    if (!ok) return 1;
    std::cout << "PASS\n";
    return 0;
}
