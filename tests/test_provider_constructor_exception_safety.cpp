#include <neograph/llm/schema_provider.h>

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
