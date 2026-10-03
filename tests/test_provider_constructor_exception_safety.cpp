#include <gtest/gtest.h>
#include "fixtures/typed_wire_peer.h"

using namespace neograph;
namespace wire = neograph::test::wire;

TEST(ProviderConstructorExceptionSafety, InvalidRuntimeOptionsPropagateTypedAdmissionFailure) {
    wire::Peer peer(wire::chat_response());
    sp::runtime::Options options; options.workers = 0;
    EXPECT_THROW(llm::SchemaProvider::create(test::descriptor("openai.chat", peer.origin()), options), sp::descriptor::ConfigError);
    EXPECT_EQ(peer.state->entered, 0u);
    auto provider = wire::provider("openai.chat", peer.origin());
    EXPECT_EQ(test::text(provider->invoke(wire::request())), "pong");
    EXPECT_EQ(peer.state->entered, 1u);
}

TEST(ProviderCredentialBoundary, DescriptorRejectsNonLoopbackPlaintextOrigins) {
    for (const auto* origin : {"http://example.invalid", "http://192.0.2.1:8080", "http://127.0.0.1.example.invalid"}) {
        SCOPED_TRACE(origin);
        EXPECT_THROW(test::descriptor("openai.chat", origin), std::invalid_argument);
    }
}

TEST(ProviderCredentialBoundary, MalformedCredentialCannotCreateProviderOrEchoSecret) {
    wire::Peer peer(wire::chat_response());
    sp::runtime::Options options; options.api_key = "sentinel-secret\r\ninjected: bad";
    try {
        (void)llm::SchemaProvider::create(test::descriptor("openai.chat", peer.origin()), options);
        FAIL() << "credential header injection was admitted";
    } catch (const sp::descriptor::ConfigError& error) {
        EXPECT_EQ(error.message.find("sentinel-secret"), std::string::npos);
    }
    EXPECT_EQ(peer.state->entered, 0u);
}
