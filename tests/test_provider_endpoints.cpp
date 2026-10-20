// Provider's SchemaProvider endpoint calls (#531): they run on the provider's own client, the descriptor policy
// admits them, and the async form neither blocks a thread nor needs the provider to outlive it.
#include <gtest/gtest.h>
#include "fixtures/endpoint_peer.h"
#include "fixtures/unique_temp_dir.h"
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>
#include <filesystem>
#include <fstream>
#include <type_traits>

using namespace neograph;
namespace wire = neograph::test::wire;
using namespace std::chrono_literals;

namespace {
template <class Request> constexpr bool forwarded() {
    using Prepared = decltype(std::declval<sp::runtime::Client&>().prepare(
        std::declval<Request>(), std::declval<sp::runtime::RunOptions>()));
    return std::is_same_v<Prepared, sp::runtime::PreparedEndpoint<EndpointOutcomeOf<Request>>>;
}
// Every endpoint request names the outcome the SDK client prepares for it.
static_assert(forwarded<sp::images::Request>() && forwarded<sp::decisions::Request>() &&
              forwarded<sp::embeddings::Request>() && forwarded<sp::rerank::Request>() &&
              forwarded<sp::moderation::Request>() && forwarded<sp::speech::Request>() &&
              forwarded<sp::transcription::Request>());
static_assert(forwarded<sp::videos::Request>() && forwarded<sp::jobs::Poll>() && forwarded<sp::jobs::Download>());
static_assert(forwarded<sp::files::Upload>() && forwarded<sp::files::Get>() && forwarded<sp::files::List>() &&
              forwarded<sp::files::Delete>() && forwarded<sp::files::Download>() && forwarded<sp::files::Start>() &&
              forwarded<sp::files::Part>() && forwarded<sp::files::Complete>() && forwarded<sp::files::Query>() &&
              forwarded<sp::files::Cancel>());
static_assert(forwarded<sp::batches::Submit>() && forwarded<sp::batches::Get>() && forwarded<sp::batches::Cancel>() &&
              forwarded<sp::batches::Delete>() && forwarded<sp::batches::Results>());
static_assert(forwarded<sp::tokens::Count>() && forwarded<sp::generations::Get>());
// Chat goes through Provider::prepare.
static_assert(!EndpointRequest<sp::runtime::Request> && !EndpointRequest<sp::chat::Request>);

std::shared_ptr<llm::SchemaProvider> provider_at(const wire::Peer& peer, bool admitted = true) {
    return test::endpoints::provider_at(peer.origin(), admitted);
}
using test::endpoints::embedding_body;

sp::embeddings::Request embed(std::string text = "hello") {
    sp::embeddings::Request request;
    request.model = "fixture-embed";
    request.inputs.push_back({std::move(text)});
    return request;
}

void expect_vector(const sp::embeddings::Outcome& outcome) {
    const auto* result = std::get_if<sp::embeddings::Result>(&outcome);
    ASSERT_NE(result, nullptr) << std::get<sp::Failure>(outcome).error.safe_message;
    ASSERT_EQ(result->vectors.size(), 1u);
    EXPECT_EQ(result->vectors[0], (std::vector<float>{0.25f, -0.5f}));
    ASSERT_TRUE(result->usage.input_total);
    EXPECT_EQ(result->usage.input_total->value, 3u);
}

// Where an upload's files live; the directory goes with the object.
class Sources {
public:
    Sources() : dir_("neograph-upload-identity-") {}
    std::string write(const std::string& name, const std::string& bytes) const {
        const auto path = dir_.path() / name;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << bytes;
        return path.string();
    }
    std::string directory() const { return dir_.path().string(); }
private:
    graph::test_support::UniqueTempDir dir_;
};

constexpr auto kFixedOrigin = "http://127.0.0.1:1";

sp::files::Upload file_upload(std::string path, std::uint64_t offset, std::uint64_t size) {
    sp::files::Upload upload;
    upload.source.file = {std::move(path), offset, size};
    upload.filename = "data.bin";
    upload.mime = "application/octet-stream";
    upload.purpose = "user_data";
    return upload;
}

sp::files::Upload memory_upload(std::string bytes) {
    sp::files::Upload upload;
    upload.source.bytes = std::make_shared<const std::string>(std::move(bytes));
    upload.filename = "data.bin";
    upload.mime = "application/octet-stream";
    upload.purpose = "user_data";
    return upload;
}

sp::files::Part file_part(std::string path, std::uint64_t offset, std::uint64_t size) {
    sp::files::Part part;
    part.session.origin = kFixedOrigin;
    part.session.shape = "openai";
    part.session.id = "upload-1";
    part.session.size = 4096;
    part.source.file = {std::move(path), offset, size};
    return part;
}

// The dispatch identity of a call prepared on `provider`, or the error that refused it.
template <class Request>
std::string digest_of(Provider& provider, Request request) {
    auto prepared = provider.prepare(make_endpoint_request(std::move(request)));
    EXPECT_EQ(prepared.error(), nullptr);
    return prepared.valid() ? Provider::request_digest(prepared) : std::string{};
}

template <class Request>
std::string refusal_of(Provider& provider, Request request) {
    const auto prepared = provider.prepare(make_endpoint_request(std::move(request)));
    EXPECT_FALSE(prepared.valid());
    EXPECT_NE(prepared.error(), nullptr);
    if (!prepared.error()) return {};
    EXPECT_EQ(prepared.error()->kind, sp::ErrorKind::InvalidRequest);
    EXPECT_EQ(prepared.error()->retry_safety, sp::RetrySafety::NotSent);
    return prepared.error()->safe_message;
}

class NoEndpoints final : public Provider {
public:
    std::string get_name() const override { return "no-endpoints"; }
    std::string_view family() const noexcept override { return "openai.chat"; }
    PreparedProviderRequest prepare(ProviderRequest) override {
        return reject_preparation(sp::Error{});
    }
};
}  // namespace

TEST(ProviderEndpoints, CompletesOnTheProvidersClientWithItsCredential) {
    wire::Peer peer(embedding_body());
    auto provider = provider_at(peer);
    expect_vector(provider->complete_endpoint(embed()));
    std::lock_guard lock(peer.state->mutex);
    ASSERT_EQ(peer.state->paths.size(), 1u);
    EXPECT_EQ(peer.state->paths[0], "/v1/embeddings");
    EXPECT_EQ(peer.state->authorization[0], "Bearer fixture-key");
    EXPECT_EQ(peer.state->requests[0]["model"], "fixture-embed");
}

TEST(ProviderEndpoints, APreparedCallNamesItsEndpointAndModelBeforeItRuns) {
    wire::Peer peer(embedding_body());
    auto provider = provider_at(peer);
    auto prepared = provider->prepare_endpoint(embed());
    ASSERT_TRUE(prepared.valid());
    ASSERT_EQ(prepared.error(), nullptr);
    EXPECT_EQ(prepared.prepared().endpoint(), "embeddings");
    EXPECT_EQ(prepared.prepared().model(), "fixture-embed");
    EXPECT_EQ(peer.state->entered, 0u);
    expect_vector(provider->complete_endpoint(std::move(prepared)));
}

TEST(ProviderEndpoints, ThePolicyRefusesAnEndpointItDoesNotAdmitAtThisOrigin) {
    wire::Peer peer(embedding_body());
    auto provider = provider_at(peer, false);
    auto prepared = provider->prepare_endpoint(embed());
    ASSERT_NE(prepared.error(), nullptr);
    const auto outcome = provider->complete_endpoint(std::move(prepared));
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(outcome));
    EXPECT_EQ(peer.state->entered, 0u);
}

TEST(ProviderEndpoints, AProviderWithoutSchemaProviderEndpointsRefusesThem) {
    NoEndpoints provider;
    EXPECT_THROW(provider.prepare_endpoint(embed()), std::logic_error);
    EXPECT_THROW(provider.complete_endpoint(embed()), std::logic_error);
    EXPECT_THROW((void)provider.complete_endpoint_async(embed()), std::logic_error);
}

TEST(ProviderEndpoints, AsyncCompletionOwnsItsClient) {
    wire::Peer peer(embedding_body());
    auto provider = provider_at(peer);
    auto awaitable = provider->complete_endpoint_async(embed());
    provider.reset();  // the awaitable holds the client and the prepared call
    asio::io_context io;
    auto future = asio::co_spawn(io, std::move(awaitable), asio::use_future);
    io.run();
    ASSERT_EQ(future.wait_for(0s), std::future_status::ready);
    expect_vector(future.get());
}

TEST(ProviderEndpoints, AsyncCompletionOfAPreparedCall) {
    wire::Peer peer(embedding_body());
    auto provider = provider_at(peer);
    asio::io_context io;
    auto future = asio::co_spawn(io, provider->complete_endpoint_async(provider->prepare_endpoint(embed())),
                                 asio::use_future);
    io.run();
    expect_vector(future.get());
}

TEST(ProviderEndpoints, CancellingTheAwaitingCoroutineCancelsTheCall) {
    wire::Peer peer(embedding_body());
    peer.state->hold = true;
    auto provider = provider_at(peer);
    asio::io_context io;
    asio::cancellation_signal cancellation;
    auto future = asio::co_spawn(io, provider->complete_endpoint_async(embed()),
                                 asio::bind_cancellation_slot(cancellation.slot(), asio::use_future));
    std::thread runner([&io] { io.run(); });
    const bool entered = peer.await_requests(1);
    asio::post(io, [&] { cancellation.emit(asio::cancellation_type::terminal); });
    const auto status = future.wait_for(2s);
    peer.release();
    runner.join();
    ASSERT_TRUE(entered);
    ASSERT_EQ(status, std::future_status::ready);
    const auto outcome = future.get();
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(outcome));
    EXPECT_EQ(std::get<sp::Failure>(outcome).error.kind, sp::ErrorKind::Cancelled);
}

TEST(ProviderEndpoints, ImagesComeBackAsMedia) {
    wire::Peer peer(test::endpoints::image_body());
    auto provider = provider_at(peer);
    sp::images::Request request;
    request.model = "fixture-image";
    request.prompt = "a square";
    request.output_format = "png";
    const auto outcome = provider->complete_endpoint(std::move(request));
    const auto* result = std::get_if<sp::images::Result>(&outcome);
    ASSERT_NE(result, nullptr) << std::get<sp::Failure>(outcome).error.safe_message;
    ASSERT_EQ(result->images.size(), 1u);
    EXPECT_EQ(result->images[0].kind, sp::MediaKind::Image);
    std::lock_guard lock(peer.state->mutex);
    EXPECT_EQ(peer.state->paths[0], "/v1/images/generations");
    EXPECT_EQ(peer.state->requests[0]["prompt"], "a square");
}

TEST(ProviderEndpoints, AnEndpointRequestTakesTheChatPathAndItsOutcomeIsTyped) {
    wire::Peer peer(embedding_body());
    auto provider = provider_at(peer);
    auto prepared = provider->prepare(make_endpoint_request(embed()));
    ASSERT_TRUE(prepared.valid());
    ASSERT_EQ(prepared.error(), nullptr);
    EXPECT_EQ(prepared.model(), "fixture-embed");
    const auto result = provider->dispatch(std::move(prepared));
    ASSERT_TRUE(result);
    expect_vector(sp::embeddings::typed(*result));
}

TEST(ProviderEndpoints, TheDispatchIdentityNamesWhatAnEndpointCallSends) {
    wire::Peer peer(embedding_body());
    auto provider = provider_at(peer);
    const auto digest = [&](auto request) {
        auto prepared = provider->prepare(make_endpoint_request(std::move(request)));
        EXPECT_EQ(prepared.error(), nullptr);
        return Provider::request_digest(prepared);
    };
    // Same endpoint, no body: only the method and URL tell these apart.
    const auto first = digest(sp::files::Get{"file-a"});
    EXPECT_NE(first, digest(sp::files::Get{"file-b"}));
    EXPECT_NE(first, digest(sp::files::Delete{"file-a"}));
    EXPECT_EQ(first, digest(sp::files::Get{"file-a"}));
    EXPECT_EQ(peer.state->entered, 0u);
}

TEST(ProviderEndpoints, TheDispatchIdentityNamesTheBytesAnUploadSendsNotTheirPath) {
    Sources sources;
    const auto a = sources.write("a.bin", std::string(100, 'a'));
    const auto b = sources.write("b.bin", std::string(100, 'b'));
    const auto moved = sources.write("elsewhere/a.bin", std::string(100, 'a'));
    const auto both = sources.write("both.bin", std::string(100, 'a') + std::string(100, 'b'));
    auto provider = test::endpoints::provider_at(kFixedOrigin);
    const auto first = digest_of(*provider, file_upload(a, 0, 100));
    EXPECT_FALSE(first.empty());
    EXPECT_NE(first, digest_of(*provider, file_upload(b, 0, 100))) << "another file";
    EXPECT_EQ(first, digest_of(*provider, file_upload(moved, 0, 100))) << "the same bytes at another path";
    EXPECT_EQ(first, digest_of(*provider, file_upload(a, 0, 100))) << "prepared again";
    EXPECT_NE(first, digest_of(*provider, file_upload(a, 0, 50))) << "fewer bytes";
    EXPECT_NE(digest_of(*provider, file_upload(both, 0, 100)), digest_of(*provider, file_upload(both, 100, 100)))
        << "another part of the file";
    // What the file slice's prefix carries is named too.
    const auto named = [&](auto edit) {
        auto upload = file_upload(a, 0, 100);
        edit(upload);
        return digest_of(*provider, std::move(upload));
    };
    EXPECT_NE(first, named([](auto& upload) { upload.filename = "other.bin"; }));
    EXPECT_NE(first, named([](auto& upload) { upload.mime = "text/plain"; }));
    EXPECT_NE(first, named([](auto& upload) { upload.purpose = "batch"; }));
    EXPECT_NE(first, named([](auto& upload) { upload.expires_after = 3600; }));
    // A chunk of a chunked upload is named by its bytes in the same way.
    const auto part = digest_of(*provider, file_part(a, 0, 100));
    EXPECT_NE(part, digest_of(*provider, file_part(b, 0, 100)));
    EXPECT_EQ(part, digest_of(*provider, file_part(moved, 0, 100)));
    EXPECT_NE(part, first) << "a part is not a whole-file upload";
}

TEST(ProviderEndpoints, AnUploadFromMemoryHasOneIdentityNoMatterHowOftenItIsPrepared) {
    // The SDK's multipart boundary is random, and an upload from memory carries it in its encoded body.
    auto provider = test::endpoints::provider_at(kFixedOrigin);
    const auto first = digest_of(*provider, memory_upload(std::string(100, 'a')));
    EXPECT_FALSE(first.empty());
    EXPECT_EQ(first, digest_of(*provider, memory_upload(std::string(100, 'a'))));
    EXPECT_NE(first, digest_of(*provider, memory_upload(std::string(100, 'b'))));
}

TEST(ProviderEndpoints, AnUploadWhoseFileCannotBeReadAsDeclaredIsRefusedWhenItIsPrepared) {
    Sources sources;
    const auto a = sources.write("a.bin", std::string(100, 'a'));
    auto provider = test::endpoints::provider_at(kFixedOrigin);
    const auto expect_source = [&](const std::string& message) {
        EXPECT_NE(message.find("upload source"), std::string::npos) << message;
        EXPECT_EQ(message.find(sources.directory()), std::string::npos) << "the message holds no path: " << message;
    };
    expect_source(refusal_of(*provider, file_upload(sources.directory() + "/missing.bin", 0, 100)));
    expect_source(refusal_of(*provider, file_upload(sources.directory(), 0, 100)));
    expect_source(refusal_of(*provider, file_upload(a, 0, 101)));
    expect_source(refusal_of(*provider, file_upload(a, 60, 50)));
    expect_source(refusal_of(*provider, file_upload(a, 101, 1)));
    expect_source(refusal_of(*provider, file_part(a, 90, 20)));
    // A declared digest is checked against the bytes, and a matching one changes nothing.
    auto declared = file_upload(a, 0, 100);
    declared.source.file.sha256 = std::string(64, '0');
    expect_source(refusal_of(*provider, declared));
    // SHA-256 of one hundred 'a' bytes.
    declared.source.file.sha256 = "2816597888e4a0d3a36b82b83316ab32680eb8f00f8cd3b904d681246d285a0e";
    EXPECT_EQ(digest_of(*provider, declared), digest_of(*provider, file_upload(a, 0, 100)));
    // What the SDK refuses on its own terms stays its refusal.
    auto nothing = file_upload(a, 0, 0);
    EXPECT_FALSE(provider->prepare(make_endpoint_request(std::move(nothing))).valid());
}

TEST(ProviderEndpoints, AnUploadIsSentFromTheFileItsIdentityNames) {
    Sources sources;
    test::endpoints::UploadPeer peer;
    auto provider = test::endpoints::provider_at(peer.origin());
    const auto path = sources.write("a.bin", "first bytes");
    auto prepared = provider->prepare(make_endpoint_request(file_upload(path, 0, 11)));
    ASSERT_TRUE(prepared.valid());
    // The file changes after the call was named. Its new bytes are not sent under that name.
    sources.write("a.bin", "other bytes");
    const auto changed = provider->dispatch(std::move(prepared));
    ASSERT_TRUE(changed);
    const auto* failure = std::get_if<sp::Failure>(&*changed);
    ASSERT_NE(failure, nullptr);
    EXPECT_EQ(failure->error.kind, sp::ErrorKind::InvalidRequest);
    EXPECT_TRUE(peer.received().empty());
    // Prepared again, the call names the file as it is, and sends it.
    const auto sent = provider->dispatch(provider->prepare(make_endpoint_request(file_upload(path, 0, 11))));
    ASSERT_TRUE(sent);
    const auto outcome = sp::files::typed(*sent);
    const auto* file = std::get_if<sp::files::File>(&outcome);
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(file->id, "file-1");
    EXPECT_EQ(peer.received(), std::vector<std::string>{"other bytes"});
}

TEST(ProviderEndpoints, TheDispatchIdentityOfEverythingButAnUploadIsUnchanged) {
    // Pinned from before uploads were named (#541), so a journal written then still matches. The observer limits are
    // set because their defaults follow the size of the SDK's event type, which differs between standard libraries.
    // A change of the descriptor policy or of an SDK resource limit moves these digests too, and with them the
    // identity of every journaled call.
    auto provider = test::endpoints::provider_at(kFixedOrigin);
    constexpr ProviderObserverLimits limits{64, 65536};
    auto chat_request = test::wire::request();
    chat_request.observer_limits = limits;
    auto chat = provider->prepare(std::move(chat_request));
    ASSERT_TRUE(chat.valid());
    EXPECT_EQ(Provider::request_digest(chat), "sha256:7b05f3fac1c26a6be86fe6d8a6dc47df83fc3b5493fd4c84c12206c5e14f4305");
    auto get_request = make_endpoint_request(sp::files::Get{"file-a"});
    get_request.observer_limits = limits;
    auto get = provider->prepare(std::move(get_request));
    ASSERT_TRUE(get.valid());
    EXPECT_EQ(Provider::request_digest(get), "sha256:8a5fe0c5401985f8cdf4d412b2c305148a150a43e9c7a4992f655af2d11825a5");
}

TEST(ProviderEndpoints, AStreamedEndpointRequestIsRefusedBeforeItIsSent) {
    wire::Peer peer(embedding_body());
    auto provider = provider_at(peer);
    auto streamed = make_endpoint_request(embed());
    streamed.mode = ProviderMode::Stream;
    const auto refused = provider->prepare(std::move(streamed));
    ASSERT_NE(refused.error(), nullptr);
    EXPECT_EQ(refused.error()->kind, sp::ErrorKind::InvalidRequest);
    EXPECT_EQ(peer.state->entered, 0u);
}

TEST(ProviderEndpoints, ALocalProviderRefusesAnEndpointRequest) {
    // A local dispatch answers chat requests; it would hand an endpoint call a chat outcome.
    class Local final : public Provider {
    public:
        std::string get_name() const override { return "local"; }
        std::string_view family() const noexcept override { return "openai.chat"; }
        PreparedProviderRequest prepare(ProviderRequest request) override {
            return prepare_local(test::client(), std::move(request),
                [](const PreparedProviderRequest&, const std::function<void(const sp::Event&)>&)
                    -> asio::awaitable<sp::runtime::Result> { co_return nullptr; });
        }
    } local;
    const auto refused = local.prepare(make_endpoint_request(embed()));
    ASSERT_NE(refused.error(), nullptr);
    EXPECT_EQ(refused.error()->kind, sp::ErrorKind::Unsupported);
}
