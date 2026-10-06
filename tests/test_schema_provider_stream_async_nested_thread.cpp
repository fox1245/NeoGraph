#include <gtest/gtest.h>
#include "fixtures/typed_wire_peer.h"
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509v3.h>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <random>

using namespace neograph;
namespace wire = neograph::test::wire;
using namespace std::chrono_literals;
namespace {
struct Certificate {
    std::filesystem::path directory;
    std::string cert, key;
    Certificate() {
        // create_directory is the atomic claim: it reports false when the name already exists.
        std::random_device entropy;
        for (int attempt = 0; attempt != 64 && directory.empty(); ++attempt) {
            const auto candidate = std::filesystem::temp_directory_path() /
                ("neograph-wire-tls-" + std::to_string(entropy()) + "-" + std::to_string(entropy()));
            std::error_code claimed;
            if (std::filesystem::create_directory(candidate, claimed) && !claimed) directory = candidate;
        }
        if (directory.empty()) throw std::runtime_error("TLS fixture directory failed");
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
        cert = (directory / "cert.pem").string(); key = (directory / "key.pem").string();
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
        if (!ctx || EVP_PKEY_keygen_init(ctx.get()) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), 2048) <= 0)
            throw std::runtime_error("TLS key initialization failed");
        EVP_PKEY* raw = nullptr;
        if (EVP_PKEY_keygen(ctx.get(), &raw) <= 0) throw std::runtime_error("TLS key generation failed");
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> pkey(raw, EVP_PKEY_free);
        std::unique_ptr<X509, decltype(&X509_free)> x509(X509_new(), X509_free);
        if (!x509) throw std::runtime_error("TLS certificate allocation failed");
        X509_set_version(x509.get(), 2); ASN1_INTEGER_set(X509_get_serialNumber(x509.get()), 1);
        X509_gmtime_adj(X509_get_notBefore(x509.get()), -60); X509_gmtime_adj(X509_get_notAfter(x509.get()), 3600);
        X509_set_pubkey(x509.get(), pkey.get());
        auto* subject = X509_get_subject_name(x509.get());
        X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
        X509_set_issuer_name(x509.get(), subject);
        X509V3_CTX extensions; X509V3_set_ctx(&extensions, x509.get(), x509.get(), nullptr, nullptr, 0);
        std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> san(
            X509V3_EXT_conf_nid(nullptr, &extensions, NID_subject_alt_name, const_cast<char*>("DNS:localhost")), X509_EXTENSION_free);
        if (!san || X509_add_ext(x509.get(), san.get(), -1) != 1 || !X509_sign(x509.get(), pkey.get(), EVP_sha256()))
            throw std::runtime_error("TLS certificate signing failed");
        // BIO files keep the CRT FILE inside OpenSSL; a vcpkg DLL cannot safely receive an application FILE*.
        std::unique_ptr<BIO, decltype(&BIO_free)> k(BIO_new_file(key.c_str(), "wb"), BIO_free);
        std::unique_ptr<BIO, decltype(&BIO_free)> c(BIO_new_file(cert.c_str(), "wb"), BIO_free);
        const bool written = k && c &&
            PEM_write_bio_PrivateKey(k.get(), pkey.get(), nullptr, nullptr, 0, nullptr, nullptr) &&
            PEM_write_bio_X509(c.get(), x509.get()) && BIO_flush(k.get()) == 1 && BIO_flush(c.get()) == 1;
        if (!written)
            throw std::runtime_error("TLS fixture write failed");
    }
    ~Certificate() { std::error_code ec; std::filesystem::remove_all(directory, ec); }
};
struct TlsPeer {
    Certificate certificate;
    std::shared_ptr<httplib::SSLServer> server;
    std::thread worker;
    int port;
    TlsPeer() : server(std::make_shared<httplib::SSLServer>(certificate.cert.c_str(), certificate.key.c_str())) {
        server->Post("/v1/responses", [body = wire::responses_sse("secure pong")](const httplib::Request&, httplib::Response& response) {
            response.set_content(body, "text/event-stream");
        });
        port = server->bind_to_any_port("127.0.0.1");
        if (port <= 0) throw std::runtime_error("TLS fixture bind failed");
        worker = std::thread([owned = server] { owned->listen_after_bind(); });
        for (int i = 0; i != 200 && !server->is_running(); ++i) std::this_thread::sleep_for(5ms);
    }
    ~TlsPeer() { server->stop(); if (worker.joinable()) worker.join(); }
    std::string origin() const { return "https://localhost:" + std::to_string(port); }
};
sp::runtime::Result drive(Provider& provider) {
    asio::io_context io;
    auto result = asio::co_spawn(io, provider.invoke_async(wire::request("openai.responses", ProviderMode::Stream)), asio::use_future);
    io.run(); return result.get();
}
void nested_request(std::shared_ptr<Provider> provider, const std::string& expected) {
    auto downstream = std::make_shared<httplib::Server>();
    downstream->Get("/stream", [owned = std::move(provider)](const httplib::Request&, httplib::Response& response) {
        response.set_chunked_content_provider("text/plain", [owned](std::size_t, httplib::DataSink& sink) {
            const auto result = drive(*owned);
            const auto text = test::text(result);
            sink.write(text.data(), text.size()); sink.done(); return true;
        });
    });
    const auto port = downstream->bind_to_any_port("127.0.0.1"); ASSERT_GT(port, 0);
    std::thread runner([owned = downstream] { owned->listen_after_bind(); });
    for (int i = 0; i != 200 && !downstream->is_running(); ++i) std::this_thread::sleep_for(5ms);
    httplib::Client client("127.0.0.1", port);
    const auto result = client.Get("/stream");
    downstream->stop(); runner.join();
    ASSERT_TRUE(result); EXPECT_EQ(result->status, 200); EXPECT_EQ(result->body, expected);
}
}

TEST(SchemaProviderStreamAsyncNestedThread, OuterIoRunOnStdThread) {
    wire::Peer peer(wire::responses_sse("thread pong"), true);
    auto provider = wire::provider("openai.responses", peer.origin());
    auto result = std::async(std::launch::async, [&] { return drive(*provider); });
    EXPECT_EQ(test::text(result.get()), "thread pong");
}
TEST(SchemaProviderStreamAsyncNestedThread, OuterIoRunOnStdThreadHttps) {
    TlsPeer peer; sp::runtime::Options options; options.ca_file = peer.certificate.cert;
    auto provider = wire::provider("openai.responses", peer.origin(), options);
    auto result = std::async(std::launch::async, [&] { return drive(*provider); });
    EXPECT_EQ(test::text(result.get()), "secure pong");
}
TEST(SchemaProviderStreamAsyncNestedThread, ConcurrentStdThreadDrivers) {
    wire::Peer peer(wire::responses_sse("parallel pong"), true);
    auto provider = wire::provider("openai.responses", peer.origin());
    std::vector<std::future<sp::runtime::Result>> results;
    for (int i = 0; i < 6; ++i) results.push_back(std::async(std::launch::async, [&] { return drive(*provider); }));
    for (auto& result : results) EXPECT_EQ(test::text(result.get()), "parallel pong");
    EXPECT_EQ(peer.state->entered, 6u);
}
TEST(SchemaProviderStreamAsyncNestedThread, OuterIoRunInsideHttplibChunkedProviderHttps) {
    TlsPeer peer; sp::runtime::Options options; options.ca_file = peer.certificate.cert;
    nested_request(wire::provider("openai.responses", peer.origin(), options), "secure pong");
}
TEST(SchemaProviderStreamAsyncNestedThread, OuterIoRunInsideHttplibChunkedProvider) {
    wire::Peer peer(wire::responses_sse("nested pong"), true);
    nested_request(wire::provider("openai.responses", peer.origin()), "nested pong");
}
