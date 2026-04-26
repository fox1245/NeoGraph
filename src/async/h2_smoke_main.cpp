// Live HTTP/2 smoke against api.openai.com — single request, verifies
// the nghttp2+asio integration end-to-end. Not in ctest (offline-safe
// by default). Run manually with OPENAI_API_KEY set.

#include <neograph/async/http2_client.h>
#include <neograph/async/run_sync.h>

#include <cstdlib>
#include <iostream>
#include <string>

int main() {
    // Two probes: httpbin (no auth, validates frame correctness) +
    // api.openai.com (real LLM, needs OPENAI_API_KEY).
    auto probe = [](const char* host, const char* path,
                    const std::string& body,
                    std::vector<std::pair<std::string, std::string>> hdrs) {
        try {
            auto resp = neograph::async::run_sync(
                [&]() -> asio::awaitable<neograph::async::HttpResponse> {
                    auto ex = co_await asio::this_coro::executor;
                    co_return co_await neograph::async::async_post_h2(
                        ex, host, "443", path, body, std::move(hdrs));
                }());
            std::cout << host << ": status " << resp.status << "\n";
            std::cout << "  body (first 200): "
                      << resp.body.substr(0, 200) << "\n";
            return resp.status;
        } catch (const std::exception& e) {
            std::cerr << host << ": ERROR " << e.what() << "\n";
            return -1;
        }
    };

    std::cout << "=== probe 0: google.com / (any 2xx/3xx = h2 works) ===\n";
    probe("www.google.com", "/", "ignored",
          {{"User-Agent", "neograph-h2-smoke/0.1"},
           {"Accept",     "*/*"}});

    std::cout << "\n=== probe 1: nghttp2.org/httpbin (no auth) ===\n";
    probe("nghttp2.org", "/httpbin/post",
          "{\"hello\":\"world\"}",
          {{"Content-Type", "application/json"},
           {"User-Agent",   "neograph-h2-smoke/0.1"},
           {"Accept",       "*/*"}});

    std::cout << "\n=== probe 2: api.openai.com ===\n";
    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key || !*key) {
        std::cerr << "OPENAI_API_KEY not set; skipping OpenAI probe.\n";
        return 0;
    }
    int status = probe("api.openai.com", "/v1/chat/completions",
        R"({"model":"gpt-5.4-mini","messages":[{"role":"user","content":"reply pong"}]})",
        {{"Authorization", std::string("Bearer ") + key},
         {"Content-Type",  "application/json"},
         {"User-Agent",    "neograph-h2-smoke/0.1"},
         {"Accept",        "*/*"}});
    return status == 200 ? 0 : 1;
}
