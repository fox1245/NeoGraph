// Compile smoke for the asio integration. Resolves localhost, spins an
// io_context, runs a coroutine that just enqueues/awaits a timer. If
// this TU compiles and links (and the smoke binary runs to "ok"), asio
// is correctly wired into the build — header paths, ASIO_STANDALONE
// define, and std::thread/Threads link are all good.
//
// Not production code. The real async_http_client lives alongside this
// once the PoC bench confirms the approach.

#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/use_awaitable.hpp>

#include <chrono>
#include <iostream>

namespace neograph::async {

asio::awaitable<void> smoke() {
    auto ex = co_await asio::this_coro::executor;
    asio::steady_timer t{ex};
    t.expires_after(std::chrono::milliseconds(5));
    co_await t.async_wait(asio::use_awaitable);
    std::cout << "ok\n";
}

int run_smoke() {
    asio::io_context io;
    asio::co_spawn(io, smoke(), asio::detached);
    io.run();
    return 0;
}

} // namespace neograph::async
