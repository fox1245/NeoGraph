#include <neograph/llm/schema_provider.h>
#include <neograph/llm/openai_provider.h>

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

using neograph::llm::SchemaProvider;

TEST(ProviderConstructorExceptionSafety,
     NoLibcurlPreferencePropagatesOriginalError) {
    SchemaProvider::Config config;
    config.schema_path = "openai";
    config.api_key = "test-key";
    config.prefer_libcurl = true;

    try {
        auto provider = SchemaProvider::create(config);
        (void)provider;
        FAIL() << "expected the unavailable libcurl backend to throw";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("libcurl backend not compiled"),
                  std::string::npos);
    }
}

TEST(ProviderCredentialBoundary, SchemaProviderRejectsPlaintextByDefault) {
    SchemaProvider::Config config;
    config.schema_path = "openai";
    config.api_key = "sentinel-key";
    config.base_url_override = "http://127.0.0.1:8080";

    EXPECT_THROW(SchemaProvider::create(config), std::invalid_argument);
}

TEST(ProviderCredentialBoundary, SchemaProviderAllowsOnlyExplicitLoopbackOptIn) {
    SchemaProvider::Config loopback;
    loopback.schema_path = "openai";
    loopback.api_key = "sentinel-key";
    loopback.base_url_override = "http://127.1.2.3:8080";
    loopback.allow_insecure_loopback = true;
    EXPECT_NO_THROW({ auto provider = SchemaProvider::create(loopback); });

    auto remote = loopback;
    remote.base_url_override = "http://192.168.1.10:8080";
    EXPECT_THROW(SchemaProvider::create(remote), std::invalid_argument);

    auto schemeless = loopback;
    schemeless.base_url_override = "localhost:8080";
    EXPECT_THROW(SchemaProvider::create(schemeless), std::invalid_argument);
}

TEST(ProviderCredentialBoundary, OpenAIProviderRejectsPlaintextByDefault) {
    neograph::llm::OpenAIProvider::Config config;
    config.api_key = "sentinel-key";
    config.base_url = "http://127.0.0.1:8080";
    EXPECT_THROW(neograph::llm::OpenAIProvider::create(config),
                 std::invalid_argument);

    config.allow_insecure_loopback = true;
    EXPECT_NO_THROW({
        auto provider = neograph::llm::OpenAIProvider::create(config);
    });
}
