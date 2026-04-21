// Live-network smoke for the TLS path of neograph::async::async_post.
//
// Hits https://httpbin.org/post with a small JSON body, prints the
// status + first body bytes. Not part of ctest (the default test
// suite must stay offline-safe) — manually-run sanity check for the
// TLS implementation in http_client.cpp.
//
// Usage:   ./neograph_async_https_smoke [host] [path]
//   Defaults: httpbin.org /post
// Exit 0 on HTTP 200 with non-empty body, 1 otherwise.
//
// Coroutine body kept minimal. GCC 13 hits an internal compiler
// error at cp/call.cc:11096 on coroutines with slightly fancier
// control-flow shapes; keeping this shape close to the bench
// coroutine in bench_async_http.cpp stays safe.

#include <neograph/async/http_client.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/use_awaitable.hpp>

#include <cstdio>
#include <exception>
#include <string>

namespace {

asio::awaitable<void> smoke(asio::any_io_executor ex,
                            std::string host, std::string path, int* rc) {
    const std::string body = R"({"smoke":"tls"})";
    std::vector<std::pair<std::string, std::string>> hdrs;
    hdrs.emplace_back("User-Agent", "neograph-async-smoke/0.1");
    auto resp = co_await neograph::async::async_post(
        ex, host, "443", path, body, hdrs, true);
    std::printf("status=%d body_bytes=%zu\n",
                resp.status, resp.body.size());
    if (resp.status == 200 && !resp.body.empty()) {
        std::size_t n = resp.body.size() < 120 ? resp.body.size() : 120;
        std::printf("body[:%zu]=%.*s\n",
                    n, static_cast<int>(n), resp.body.data());
        *rc = 0;
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string host = argc > 1 ? argv[1] : "httpbin.org";
    std::string path = argc > 2 ? argv[2] : "/post";

    asio::io_context io;
    int rc = 1;
    asio::co_spawn(io, smoke(io.get_executor(), host, path, &rc),
                   asio::detached);
    try {
        io.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "io.run threw: %s\n", e.what());
        return 1;
    }
    return rc;
}
