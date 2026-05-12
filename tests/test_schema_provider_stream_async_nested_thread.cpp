// Issue #16 — repro for the SEGV not covered by #14's main-thread driver.
//
// Downstream (ProjectDatePop's cpp_backend) drives `io.run()` from inside
// an httplib `set_chunked_content_provider` lambda — i.e. a worker
// thread owned by the HTTP server. v0.7.0 fix verified by #14 only
// covers the case where `io.run()` runs on the main thread.
//
// This test strips the httplib server out and reproduces just the
// nesting depth: outer `std::thread` → `asio::io_context.run()` →
// `co_await provider->complete_stream_async(...)`. If the SEGV
// reproduces here, the corner is "outer io.run() on a non-main
// thread"; if it doesn't, the corner is httplib-specific (server
// thread state interfering).
//
// Plus a second variant that wraps the test in an httplib::Server
// chunked-provider lambda — closer to the literal downstream shape.
//
// Both variants use the same httplib SSE mock from
// `test_schema_provider_stream_async_outer_io.cpp` so the only
// variable is the thread driving the outer `io.run()`.

#include <gtest/gtest.h>

#include <neograph/llm/schema_provider.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace neograph;

namespace {

struct ResponsesMock {
    httplib::Server svr;
    std::thread     t;
    int             port = 0;

    explicit ResponsesMock(std::string sse_body) {
        svr.Post("/v1/responses",
            [body = std::move(sse_body)]
            (const httplib::Request&, httplib::Response& res) {
                res.set_header("Content-Type", "text/event-stream");
                res.set_content(body, "text/event-stream");
            });
        port = svr.bind_to_any_port("127.0.0.1");
        t = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 200 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    ~ResponsesMock() { svr.stop(); if (t.joinable()) t.join(); }
};

llm::SchemaProvider::Config cfg_for(int port) {
    llm::SchemaProvider::Config cfg;
    cfg.schema_path       = "openai_responses";
    cfg.api_key           = "test-key";
    cfg.default_model     = "gpt-test";
    cfg.timeout_seconds   = 10;
    // Use "localhost" (not 127.0.0.1) so httplib::Client triggers a
    // real `getaddrinfo` lookup. The downstream SEGV in #16 is on
    // `internal_strlen` inside `getaddrinfo` — IP-literal paths skip
    // resolution entirely and would silently miss the bug.
    cfg.base_url_override = "http://localhost:" + std::to_string(port);
    return cfg;
}

constexpr const char* kBody =
    "event: response.output_item.added\n"
    "data: {\"type\":\"response.output_item.added\",\"output_index\":0,"
          "\"item\":{\"type\":\"message\",\"id\":\"msg_1\","
          "\"role\":\"assistant\"}}\n"
    "\n"
    "event: response.output_text.delta\n"
    "data: {\"type\":\"response.output_text.delta\","
          "\"item_id\":\"msg_1\",\"delta\":\"hi\"}\n"
    "\n"
    "event: response.output_item.done\n"
    "data: {\"type\":\"response.output_item.done\",\"output_index\":0}\n"
    "\n"
    "event: response.completed\n"
    "data: {\"type\":\"response.completed\","
          "\"response\":{\"id\":\"resp_1\","
          "\"usage\":{\"input_tokens\":1,\"output_tokens\":1,"
                     "\"total_tokens\":2}}}\n"
    "\n";

CompletionParams params_with(std::string user_msg) {
    CompletionParams p;
    p.model = "gpt-test";
    ChatMessage u; u.role = "user"; u.content = std::move(user_msg);
    p.messages.push_back(u);
    return p;
}

// Generate a self-signed cert + key pair to disk, return paths.
// Caller deletes files in tear-down. Throws on any OpenSSL failure
// — test FAILs cleanly rather than running with a half-set-up TLS
// environment that would mask the real bug.
struct CertPaths {
    std::string cert_path;
    std::string key_path;
};

CertPaths make_self_signed_cert() {
    auto tmp_dir = std::filesystem::temp_directory_path();
    auto cert = (tmp_dir / "neograph_test_cert.pem").string();
    auto key  = (tmp_dir / "neograph_test_key.pem").string();

    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> pkey(
        EVP_PKEY_new(), &EVP_PKEY_free);
    if (!pkey) throw std::runtime_error("EVP_PKEY_new failed");

    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> kctx(
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), &EVP_PKEY_CTX_free);
    if (!kctx) throw std::runtime_error("EVP_PKEY_CTX_new_id failed");
    if (EVP_PKEY_keygen_init(kctx.get()) <= 0)
        throw std::runtime_error("EVP_PKEY_keygen_init failed");
    if (EVP_PKEY_CTX_set_rsa_keygen_bits(kctx.get(), 2048) <= 0)
        throw std::runtime_error("EVP_PKEY_CTX_set_rsa_keygen_bits failed");
    EVP_PKEY* raw_pkey = nullptr;
    if (EVP_PKEY_keygen(kctx.get(), &raw_pkey) <= 0)
        throw std::runtime_error("EVP_PKEY_keygen failed");
    pkey.reset(raw_pkey);

    std::unique_ptr<X509, decltype(&X509_free)> x509(X509_new(), &X509_free);
    if (!x509) throw std::runtime_error("X509_new failed");
    ASN1_INTEGER_set(X509_get_serialNumber(x509.get()), 1);
    X509_gmtime_adj(X509_get_notBefore(x509.get()), 0);
    X509_gmtime_adj(X509_get_notAfter(x509.get()), 60 * 60);  // 1 hour
    X509_set_pubkey(x509.get(), pkey.get());
    X509_NAME* name = X509_get_subject_name(x509.get());
    X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("US"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    X509_set_issuer_name(x509.get(), name);
    if (!X509_sign(x509.get(), pkey.get(), EVP_sha256()))
        throw std::runtime_error("X509_sign failed");

    std::unique_ptr<FILE, int(*)(FILE*)> kfp(
        std::fopen(key.c_str(), "wb"), std::fclose);
    if (!kfp) throw std::runtime_error("open key file failed");
    if (!PEM_write_PrivateKey(kfp.get(), pkey.get(),
                              nullptr, nullptr, 0, nullptr, nullptr))
        throw std::runtime_error("PEM_write_PrivateKey failed");

    std::unique_ptr<FILE, int(*)(FILE*)> cfp(
        std::fopen(cert.c_str(), "wb"), std::fclose);
    if (!cfp) throw std::runtime_error("open cert file failed");
    if (!PEM_write_X509(cfp.get(), x509.get()))
        throw std::runtime_error("PEM_write_X509 failed");

    return {cert, key};
}

struct ResponsesMockSSL {
    httplib::SSLServer svr;
    std::thread        t;
    int                port = 0;
    CertPaths          paths;

    explicit ResponsesMockSSL(CertPaths p, std::string sse_body)
        : svr(p.cert_path.c_str(), p.key_path.c_str()), paths(std::move(p)) {
        svr.Post("/v1/responses",
            [body = std::move(sse_body)]
            (const httplib::Request&, httplib::Response& res) {
                res.set_header("Content-Type", "text/event-stream");
                res.set_content(body, "text/event-stream");
            });
        port = svr.bind_to_any_port("127.0.0.1");
        t = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 200 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    ~ResponsesMockSSL() {
        svr.stop();
        if (t.joinable()) t.join();
        std::error_code ec;
        std::filesystem::remove(paths.cert_path, ec);
        std::filesystem::remove(paths.key_path, ec);
    }
};

llm::SchemaProvider::Config cfg_for_https(int port) {
    auto cfg = cfg_for(port);
    // SchemaProvider's underlying httplib::Client picks SSL based on
    // the URL scheme. "localhost" still triggers a real getaddrinfo.
    cfg.base_url_override = "https://localhost:" + std::to_string(port);
    return cfg;
}

} // namespace

// Variant A — outer io.run() on a fresh std::thread (no httplib server).
// If #16 reproduces here, the issue is "io_context driven from a
// non-main thread"; if not, the issue is httplib-server-specific.
TEST(SchemaProviderStreamAsyncNestedThread, OuterIoRunOnStdThread) {
    ResponsesMock mock{kBody};
    ASSERT_GT(mock.port, 0);

    auto provider = llm::SchemaProvider::create(cfg_for(mock.port));
    ASSERT_TRUE(provider);

    std::string final_content;
    std::exception_ptr caught;

    std::thread worker([&]() {
        asio::io_context io;
        asio::co_spawn(
            io,
            [&]() -> asio::awaitable<void> {
                try {
                    auto p = params_with("ping");
                    auto r = co_await provider->complete_stream_async(
                        p, [](const std::string&) {});
                    final_content = r.message.content;
                } catch (...) {
                    caught = std::current_exception();
                }
            },
            asio::detached);
        io.run();
    });
    worker.join();

    ASSERT_FALSE(caught) << "co_await complete_stream_async threw on "
                            "the std::thread driver";
    EXPECT_EQ(final_content, "hi");
}

// Variant A-HTTPS — same as Variant A but the upstream is HTTPS (forces
// SSL handshake + getaddrinfo on a real hostname). Closer to the
// downstream segfault env from #16. SchemaProvider's httplib::SSLClient
// picks SSL automatically from the "https://" prefix.
TEST(SchemaProviderStreamAsyncNestedThread, OuterIoRunOnStdThreadHttps) {
    CertPaths cert = make_self_signed_cert();
    ResponsesMockSSL mock{cert, kBody};
    ASSERT_GT(mock.port, 0);

    auto provider = llm::SchemaProvider::create(cfg_for_https(mock.port));
    ASSERT_TRUE(provider);

    std::string final_content;
    std::exception_ptr caught;

    std::thread worker([&]() {
        asio::io_context io;
        asio::co_spawn(
            io,
            [&]() -> asio::awaitable<void> {
                try {
                    auto p = params_with("ping");
                    auto r = co_await provider->complete_stream_async(
                        p, [](const std::string&) {});
                    final_content = r.message.content;
                } catch (...) {
                    caught = std::current_exception();
                }
            },
            asio::detached);
        io.run();
    });
    worker.join();

    // The connection itself may fail (self-signed cert + httplib's
    // default verifier); what matters is that we got a clean throw,
    // not a SEGV inside getaddrinfo. If #16's wild-ptr crash repros
    // here, the test process dies before reaching this line.
    if (caught) {
        try { std::rethrow_exception(caught); }
        catch (const std::exception& e) {
            std::cerr << "[expected — TLS may fail under self-signed cert] "
                      << e.what() << "\n";
        }
    }
    SUCCEED() << "no SEGV; final_content=\"" << final_content << "\"";
}

// Variant A-Concurrent — N concurrent fresh-thread drivers hammering one
// SchemaProvider. Tests the "concurrent request load triggers a race
// in the per-call worker thread spawn" hypothesis. If #16's wild-ptr
// is racy, this should hit it.
TEST(SchemaProviderStreamAsyncNestedThread, ConcurrentStdThreadDrivers) {
    ResponsesMock mock{kBody};
    ASSERT_GT(mock.port, 0);

    auto provider = llm::SchemaProvider::create(cfg_for(mock.port));
    ASSERT_TRUE(provider);

    constexpr int N = 16;
    std::vector<std::thread> drivers;
    std::atomic<int> ok{0};
    std::vector<std::exception_ptr> caughts(N);

    for (int i = 0; i < N; ++i) {
        drivers.emplace_back([&, i]() {
            asio::io_context io;
            asio::co_spawn(
                io,
                [&, i]() -> asio::awaitable<void> {
                    try {
                        auto p = params_with("ping " + std::to_string(i));
                        auto r = co_await provider->complete_stream_async(
                            p, [](const std::string&) {});
                        if (r.message.content == "hi") ++ok;
                    } catch (...) {
                        caughts[i] = std::current_exception();
                    }
                },
                asio::detached);
            io.run();
        });
    }
    for (auto& t : drivers) t.join();

    int err_count = 0;
    for (auto& e : caughts) if (e) ++err_count;
    EXPECT_EQ(err_count, 0)
        << "concurrent drivers produced " << err_count << " exceptions";
    EXPECT_EQ(ok.load(), N);
}

// Variant B-HTTPS — outer io.run() driven from inside an httplib::Server
// chunked-provider lambda + upstream is HTTPS. Literal downstream shape
// from #16 (just with a self-signed mock instead of api.openai.com).
TEST(SchemaProviderStreamAsyncNestedThread, OuterIoRunInsideHttplibChunkedProviderHttps) {
    CertPaths cert = make_self_signed_cert();
    ResponsesMockSSL mock{cert, kBody};
    ASSERT_GT(mock.port, 0);

    auto provider = llm::SchemaProvider::create(cfg_for_https(mock.port));
    ASSERT_TRUE(provider);

    httplib::Server proxy;
    std::exception_ptr proxy_caught;
    std::mutex proxy_mu;

    proxy.Get("/chat", [&](const httplib::Request&, httplib::Response& res) {
        res.set_chunked_content_provider(
            "text/event-stream",
            [&](size_t, httplib::DataSink& sink) -> bool {
                asio::io_context io;
                asio::co_spawn(
                    io,
                    [&]() -> asio::awaitable<void> {
                        try {
                            auto p = params_with("ping");
                            auto r = co_await provider->complete_stream_async(
                                p, [&sink](const std::string& tok) {
                                    sink.write(tok.data(), tok.size());
                                });
                            (void)r;
                        } catch (...) {
                            std::lock_guard<std::mutex> lock(proxy_mu);
                            proxy_caught = std::current_exception();
                        }
                    },
                    asio::detached);
                io.run();
                sink.done();
                return true;
            });
    });
    int proxy_port = proxy.bind_to_any_port("127.0.0.1");
    ASSERT_GT(proxy_port, 0);
    std::thread proxy_t([&]() { proxy.listen_after_bind(); });
    for (int i = 0; i < 200 && !proxy.is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    httplib::Client client("127.0.0.1", proxy_port);
    client.set_read_timeout(15, 0);
    std::string body;
    auto res = client.Get("/chat",
        [&body](const char* data, size_t len) {
            body.append(data, len);
            return true;
        });

    proxy.stop();
    if (proxy_t.joinable()) proxy_t.join();

    if (proxy_caught) {
        try { std::rethrow_exception(proxy_caught); }
        catch (const std::exception& e) {
            std::cerr << "[expected — TLS may fail under self-signed cert] "
                      << e.what() << "\n";
        }
    }
    SUCCEED() << "no SEGV; proxy body=\"" << body << "\"";
}

// Variant B — outer io.run() inside an httplib::Server chunked-provider
// lambda — the literal downstream ProjectDatePop shape from #16.
TEST(SchemaProviderStreamAsyncNestedThread, OuterIoRunInsideHttplibChunkedProvider) {
    ResponsesMock mock{kBody};
    ASSERT_GT(mock.port, 0);

    auto provider = llm::SchemaProvider::create(cfg_for(mock.port));
    ASSERT_TRUE(provider);

    // A proxy server: receives a GET /chat, drives the SchemaProvider
    // streaming via complete_stream_async from inside its
    // set_chunked_content_provider lambda, and writes accumulated
    // tokens into the SSE stream.
    httplib::Server proxy;
    std::string proxy_final_content;
    std::exception_ptr proxy_caught;
    std::mutex proxy_mu;

    proxy.Get("/chat", [&](const httplib::Request&, httplib::Response& res) {
        res.set_chunked_content_provider(
            "text/event-stream",
            [&](size_t, httplib::DataSink& sink) -> bool {
                // ← inside httplib worker thread (T_httplib)
                asio::io_context io;
                asio::co_spawn(
                    io,
                    [&]() -> asio::awaitable<void> {
                        try {
                            auto p = params_with("ping");
                            auto r = co_await provider->complete_stream_async(
                                p, [&sink](const std::string& tok) {
                                    sink.write(tok.data(), tok.size());
                                });
                            std::lock_guard<std::mutex> lock(proxy_mu);
                            proxy_final_content = r.message.content;
                        } catch (...) {
                            std::lock_guard<std::mutex> lock(proxy_mu);
                            proxy_caught = std::current_exception();
                        }
                    },
                    asio::detached);
                io.run();
                sink.done();
                return true;
            });
    });
    int proxy_port = proxy.bind_to_any_port("127.0.0.1");
    ASSERT_GT(proxy_port, 0);
    std::thread proxy_t([&]() { proxy.listen_after_bind(); });
    for (int i = 0; i < 200 && !proxy.is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    httplib::Client client("127.0.0.1", proxy_port);
    client.set_read_timeout(15, 0);
    std::string body;
    auto res = client.Get("/chat",
        [&body](const char* data, size_t len) {
            body.append(data, len);
            return true;
        });

    proxy.stop();
    if (proxy_t.joinable()) proxy_t.join();

    ASSERT_TRUE(res) << "proxy GET failed";
    {
        std::lock_guard<std::mutex> lock(proxy_mu);
        ASSERT_FALSE(proxy_caught)
            << "co_await complete_stream_async threw inside the "
               "httplib chunked-provider worker";
        EXPECT_EQ(proxy_final_content, "hi");
    }
    EXPECT_EQ(body, "hi");
}
