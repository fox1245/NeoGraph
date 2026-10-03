#include <gtest/gtest.h>
#include <neograph/llm/veo_client.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/cancel.h>
#include <asio/this_coro.hpp>
#include <httplib.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <mutex>
#include <thread>
#include <type_traits>

using namespace neograph;
using namespace neograph::llm;
using namespace std::chrono_literals;
namespace {
constexpr const char* model = "veo-3.1-lite-generate-preview";
constexpr const char* operation_name = "models/veo-3.1-lite-generate-preview/operations/local-1";

struct VeoLocalPeer {
    httplib::Server server;
    std::thread worker;
    int port;
    std::atomic<int> posts{0}, gets{0}, files{0};
    std::atomic<int> submit_status{200}, poll_status{200};
    std::atomic<bool> stall_poll{false}, stall_file{false}, release{false}, entered{false};
    std::mutex mutex;
    std::condition_variable condition;
    json submission = {{"name", operation_name}};
    json status = {{"name", operation_name}};
    std::string video_body = std::string("\0\0\0\x18", 4) + "ftypmp42" + std::string(12, '\0');
    std::string video_mime = "video/mp4";
    int file_status = 200;
    std::string file_location;

    VeoLocalPeer() {
        server.Post("/v1beta/models/veo-3.1-lite-generate-preview:predictLongRunning",
            [this](const httplib::Request&, httplib::Response& response) {
                ++posts;
                std::lock_guard<std::mutex> lock(mutex);
                response.status = submit_status;
                response.set_content(submission.dump(), "application/json");
            });
        server.Get("/v1beta/models/veo-3.1-lite-generate-preview/operations/local-1",
            [this](const httplib::Request&, httplib::Response& response) {
                ++gets;
                std::unique_lock<std::mutex> lock(mutex);
                if (stall_poll) {
                    entered = true;
                    condition.notify_all();
                    condition.wait(lock, [this] { return release.load(); });
                }
                response.status = poll_status;
                response.set_content(status.dump(), "application/json");
            });
        server.Get("/v1beta/files/local-video:download",
            [this](const httplib::Request& request, httplib::Response& response) {
                ++files;
                std::unique_lock<std::mutex> lock(mutex);
                if (stall_file) {
                    entered = true;
                    condition.notify_all();
                    condition.wait(lock, [this] { return release.load(); });
                }
                if (request.get_header_value("x-goog-api-key") != "private-local-key") {
                    response.status = 403; return;
                }
                response.status = file_status;
                if (!file_location.empty()) response.set_header("Location", file_location);
                response.set_content(video_body, video_mime);
            });
        port = server.bind_to_any_port("127.0.0.1");
        if (port <= 0) throw std::runtime_error("Cannot bind Veo local peer");
        worker = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i != 200 && !server.is_running(); ++i) std::this_thread::sleep_for(1ms);
        if (!server.is_running()) { server.stop(); worker.join(); throw std::runtime_error("Cannot start Veo local peer"); }
    }
    ~VeoLocalPeer() {
        release = true;
        condition.notify_all();
        server.stop();
        if (worker.joinable()) worker.join();
    }
    std::string origin() const { return "http://127.0.0.1:" + std::to_string(port); }
    void done() {
        std::lock_guard<std::mutex> lock(mutex);
        status = {{"name", operation_name}, {"done", true},
            {"response", {{"generateVideoResponse", {{"generatedSamples", json::array({
                {{"video", {{"uri", origin() + "/v1beta/files/local-video:download?alt=media"},
                             {"encoding", "video/mp4"}, {"fileId", "local-video"}}}}
            })}}}}}};
    }
    bool await_entry() {
        std::unique_lock<std::mutex> lock(mutex);
        return condition.wait_for(lock, 2s, [this] { return entered.load(); });
    }
};
json config(const VeoLocalPeer& peer) {
    return {{"version", 1}, {"endpoint", {{"origin", peer.origin()}, {"api_path", "/v1beta"},
        {"allow_insecure_loopback", true}}}, {"models", json::array({model})},
        {"defaults", {{"model", model}, {"aspect_ratio", "16:9"}, {"person_generation", "allow_all"},
            {"duration_seconds", 4}, {"resolution", "720p"}}},
        {"limits", {{"deadline_ms", 2000}, {"poll_interval_ms", 5}, {"max_prompt_bytes", 100},
            {"max_image_bytes", 100}, {"max_json_bytes", 4096}, {"max_video_bytes", 4096},
            {"max_status_queries", 4}}}, {"adult_only_region", false},
        {"download", {{"allowed_origins", json::array({peer.origin(), "https://storage.googleapis.com"})},
                      {"max_redirects", 3}}}};
}
VeoRequest request() { VeoRequest r; r.prompt = "A small blue paper kite in a clear sky."; return r; }
VeoSubmitResult submit(const VeoClient& client, VeoRequest r = request(),
                      std::optional<std::chrono::steady_clock::time_point> deadline = {}) {
    auto work = [](const VeoClient* c, VeoRequest owned,
                   std::optional<std::chrono::steady_clock::time_point> d) -> asio::awaitable<VeoSubmitResult> {
        co_return co_await c->submit(co_await asio::this_coro::executor, std::move(owned), d);
    };
    return async::run_sync(work(&client, std::move(r), deadline));
}
}

TEST(TypedVeoClient, MissingDoneRemainsPendingThenDownloadsOwnedMp4) {
    VeoLocalPeer peer;
    VeoClient client(config(peer), "private-local-key");
    const auto submitted = submit(client);
    ASSERT_TRUE(submitted.operation);
    EXPECT_FALSE(submitted.failure);
    EXPECT_EQ(submitted.operation->name(), operation_name);
    const auto pending = async::run_sync(client.poll(*submitted.operation));
    EXPECT_EQ(pending.status, VeoStatus::Pending);
    EXPECT_EQ(pending.generation_dispatches, 1u);
    EXPECT_EQ(pending.status_queries, 1u);
    EXPECT_FALSE(pending.usage.total_tokens);
    peer.done();
    const auto ready = async::run_sync(client.wait(*submitted.operation));
    ASSERT_EQ(ready.status, VeoStatus::Succeeded);
    ASSERT_EQ(ready.artifacts.size(), 1u);
    EXPECT_EQ(ready.artifacts[0].file_id, "local-video");
    const auto downloaded = async::run_sync(client.download(*submitted.operation));
    ASSERT_TRUE(downloaded.artifact);
    ASSERT_TRUE(downloaded.artifact->bytes);
    EXPECT_EQ(downloaded.artifact->bytes->size(), peer.video_body.size());
    EXPECT_EQ(std::to_integer<unsigned>((*downloaded.artifact->bytes)[4]), 'f');
    const auto again = async::run_sync(client.download(*submitted.operation));
    ASSERT_TRUE(again.artifact);
    EXPECT_EQ(again.artifact->bytes, downloaded.artifact->bytes);
    EXPECT_EQ(peer.files, 1);
    EXPECT_EQ(peer.posts, 1);
    EXPECT_EQ(peer.gets, 2);
}

TEST(TypedVeoClient, ProviderFailureIsStickyWithoutRegenerationOrPollingRetry) {
    VeoLocalPeer peer;
    peer.status = {{"error", {{"code", 7}, {"message", "private-local-key https://secret.invalid"}}}};
    VeoClient client(config(peer), "private-local-key");
    const auto submitted = submit(client);
    ASSERT_TRUE(submitted.operation);
    const auto failed = async::run_sync(client.poll(*submitted.operation));
    ASSERT_TRUE(failed.failure);
    EXPECT_EQ(failed.status, VeoStatus::Failed);
    EXPECT_EQ(failed.failure->kind, EndpointFailureKind::Provider);
    EXPECT_EQ(failed.failure->provider_code, "7");
    EXPECT_EQ(failed.failure->safe_message.find("private-local-key"), std::string::npos);
    peer.done();
    client.cancel(*submitted.operation);
    EXPECT_EQ(async::run_sync(client.wait(*submitted.operation)).status, VeoStatus::Failed);
    EXPECT_EQ(peer.posts, 1);
    EXPECT_EQ(peer.gets, 1);
}

TEST(TypedVeoClient, MissingVideoAndMalformedDoneAreNotSuccessful) {
    for (const json& bad : {json{{"done", true}}, json{{"done", nullptr}}, json{{"done", "false"}}}) {
        VeoLocalPeer peer;
        peer.status = bad;
        VeoClient client(config(peer), "private-local-key");
        const auto submitted = submit(client);
        ASSERT_TRUE(submitted.operation);
        const auto result = async::run_sync(client.poll(*submitted.operation));
        ASSERT_TRUE(result.failure);
        EXPECT_EQ(result.failure->kind, EndpointFailureKind::Protocol);
        EXPECT_EQ(result.status, VeoStatus::Failed);
        EXPECT_TRUE(result.artifacts.empty());
    }
}

TEST(TypedVeoClient, OriginAndModelForgeryCannotAuthorizeGetOrDownload) {
    static_assert(!std::is_default_constructible_v<VeoOperation>);
    static_assert(!std::is_constructible_v<VeoOperation, json>);
    VeoLocalPeer peer;
    VeoClient client(config(peer), "private-local-key");
    VeoClient other(config(peer), "other-key");
    const auto submitted = submit(client);
    ASSERT_TRUE(submitted.operation);
    const auto refused = async::run_sync(other.poll(*submitted.operation));
    ASSERT_TRUE(refused.failure);
    EXPECT_EQ(refused.failure->kind, EndpointFailureKind::InvalidRequest);
    EXPECT_EQ(peer.gets, 0);
    other.cancel(*submitted.operation); // A foreign client cannot cancel it either.
    peer.done();
    peer.status["response"]["generateVideoResponse"]["generatedSamples"][0]["video"]["uri"] =
        "https://attacker.invalid/v1beta/files/stolen:download?key=private-local-key";
    const auto invalid = async::run_sync(client.poll(*submitted.operation));
    ASSERT_TRUE(invalid.failure);
    EXPECT_EQ(invalid.failure->kind, EndpointFailureKind::Protocol);
    EXPECT_EQ(peer.files, 0);
    VeoLocalPeer forged;
    forged.submission["name"] = "models/veo-3.1-generate-preview/operations/local-1";
    VeoClient forged_client(config(forged), "private-local-key");
    const auto bad_submit = submit(forged_client);
    ASSERT_TRUE(bad_submit.failure);
    EXPECT_EQ(bad_submit.failure->kind, EndpointFailureKind::Protocol);
    EXPECT_FALSE(bad_submit.operation);
}

TEST(TypedVeoClient, QueryBudgetIsNeverRenewedAndHttpFailureIsNotRetried) {
    VeoLocalPeer peer;
    auto cfg = config(peer); cfg["limits"]["max_status_queries"] = 2;
    VeoClient client(cfg, "private-local-key");
    const auto submitted = submit(client);
    ASSERT_TRUE(submitted.operation);
    const auto exhausted = async::run_sync(client.wait(*submitted.operation));
    ASSERT_TRUE(exhausted.failure);
    EXPECT_EQ(exhausted.failure->kind, EndpointFailureKind::ResourceLimit);
    EXPECT_EQ(exhausted.status_queries, 2u);
    async::run_sync(client.wait(*submitted.operation));
    EXPECT_EQ(peer.gets, 2);
    EXPECT_EQ(peer.posts, 1);
    VeoLocalPeer unavailable;
    unavailable.submit_status = 503;
    VeoClient unavailable_client(config(unavailable), "private-local-key");
    const auto rejected = submit(unavailable_client);
    ASSERT_TRUE(rejected.failure);
    EXPECT_EQ(rejected.failure->http_status, 503);
    EXPECT_TRUE(rejected.failure->request_may_have_left);
    EXPECT_EQ(unavailable.posts, 1);
}

TEST(TypedVeoClient, DeadlineCoversStatusAndDownloadAndNeverStartsFreshBudget) {
    VeoLocalPeer peer;
    auto cfg = config(peer); cfg["limits"]["poll_interval_ms"] = 100;
    VeoClient client(cfg, "private-local-key");
    const auto deadline = std::chrono::steady_clock::now() + 40ms;
    const auto submitted = submit(client, request(), deadline);
    ASSERT_TRUE(submitted.operation);
    EXPECT_EQ(submitted.operation->deadline(), deadline);
    const auto result = async::run_sync(client.wait(*submitted.operation));
    ASSERT_TRUE(result.failure);
    EXPECT_EQ(result.failure->kind, EndpointFailureKind::DeadlineExceeded);
    EXPECT_EQ(peer.gets, 0);
    const auto download = async::run_sync(client.download(*submitted.operation));
    ASSERT_TRUE(download.failure);
    EXPECT_EQ(download.failure->kind, EndpointFailureKind::DeadlineExceeded);
    EXPECT_EQ(peer.files, 0);
    EXPECT_EQ(peer.posts, 1);
}

TEST(TypedVeoClient, CancelInterruptsStalledSocketWithoutPeerProgress) {
    VeoLocalPeer peer;
    peer.stall_poll = true;
    VeoClient client(config(peer), "private-local-key");
    const auto submitted = submit(client);
    ASSERT_TRUE(submitted.operation);
    auto pending = std::async(std::launch::async, [&] { return async::run_sync(client.poll(*submitted.operation)); });
    ASSERT_TRUE(peer.await_entry());
    client.cancel(*submitted.operation);
    const auto completion = pending.wait_for(500ms);
    EXPECT_EQ(completion, std::future_status::ready);
    if (completion != std::future_status::ready) { peer.release = true; peer.condition.notify_all(); }
    const auto result = pending.get();
    ASSERT_TRUE(result.failure);
    EXPECT_EQ(result.status, VeoStatus::Cancelled);
    EXPECT_EQ(result.failure->kind, EndpointFailureKind::Cancelled);
    async::run_sync(client.wait(*submitted.operation));
    EXPECT_EQ(peer.gets, 1);
    EXPECT_EQ(peer.posts, 1);
}

TEST(TypedVeoClient, CancelInterruptsWaitTimerAndDownloadSocket) {
    VeoLocalPeer peer;
    auto cfg = config(peer); cfg["limits"]["poll_interval_ms"] = 1000;
    VeoClient client(cfg, "private-local-key");
    const auto submitted = submit(client);
    ASSERT_TRUE(submitted.operation);
    auto pending = std::async(std::launch::async, [&] { return async::run_sync(client.wait(*submitted.operation)); });
    client.cancel(*submitted.operation);
    const auto result = pending.get();
    EXPECT_EQ(result.status, VeoStatus::Cancelled);
    EXPECT_EQ(peer.gets, 0);
    VeoLocalPeer video;
    video.done(); video.stall_file = true;
    VeoClient video_client(config(video), "private-local-key");
    const auto handle = submit(video_client);
    ASSERT_TRUE(handle.operation);
    ASSERT_EQ(async::run_sync(video_client.poll(*handle.operation)).status, VeoStatus::Succeeded);
    auto download = std::async(std::launch::async, [&] { return async::run_sync(video_client.download(*handle.operation)); });
    ASSERT_TRUE(video.await_entry());
    video_client.cancel(*handle.operation);
    const auto completion = download.wait_for(500ms);
    EXPECT_EQ(completion, std::future_status::ready);
    if (completion != std::future_status::ready) { video.release = true; video.condition.notify_all(); }
    const auto failed = download.get();
    ASSERT_TRUE(failed.failure);
    EXPECT_EQ(failed.failure->kind, EndpointFailureKind::Cancelled);
}

TEST(TypedVeoClient, UnsupportedLiteVariantsAndInvalidControlsDispatchNothing) {
    VeoLocalPeer peer;
    VeoClient client(config(peer), "private-local-key");
    for (int variant = 0; variant != 7; ++variant) {
        auto r = request();
        if (variant == 0) r.duration_seconds = 5;
        if (variant == 1) r.resolution = VeoResolution::P1080;
        if (variant == 2) { r.resolution = VeoResolution::P4K; r.duration_seconds = 8; }
        if (variant == 3) r.number_of_videos = 2;
        if (variant == 4) r.reference_images.push_back(VeoImage{});
        if (variant == 5) r.extension_video.emplace();
        if (variant == 6) r.person_generation = VeoPersonGeneration::DontAllow;
        const auto result = submit(client, std::move(r));
        ASSERT_TRUE(result.failure);
        EXPECT_EQ(result.failure->kind, variant < 2 ? EndpointFailureKind::InvalidRequest : EndpointFailureKind::Unsupported);
        EXPECT_FALSE(result.failure->request_may_have_left);
    }
    EXPECT_EQ(peer.posts, 0);
    auto cancelled = request(); cancelled.cancel_token = std::make_shared<graph::CancelToken>();
    cancelled.cancel_token->cancel();
    const auto result = submit(client, std::move(cancelled));
    ASSERT_TRUE(result.failure);
    EXPECT_EQ(result.failure->kind, EndpointFailureKind::Cancelled);
    EXPECT_EQ(peer.posts, 0);
}

TEST(TypedVeoClient, WrongDownloadMimeNeverBecomesAnArtifact) {
    for (const char* mime : {"application/json", "text/html"}) {
        VeoLocalPeer peer; peer.done(); peer.video_mime = mime;
        VeoClient client(config(peer), "private-local-key");
        const auto handle = submit(client);
        ASSERT_TRUE(handle.operation);
        ASSERT_EQ(async::run_sync(client.poll(*handle.operation)).status, VeoStatus::Succeeded);
        const auto downloaded = async::run_sync(client.download(*handle.operation));
        ASSERT_TRUE(downloaded.failure);
        EXPECT_EQ(downloaded.failure->kind, EndpointFailureKind::Protocol);
        EXPECT_FALSE(downloaded.artifact);
    }
}

TEST(TypedVeoClient, ClosedConfigurationRejectsGrammarAndUnsafeEndpoints) {
    VeoLocalPeer peer;
    auto cfg = config(peer); cfg["operation"] = {{"poll", "some grammar"}};
    EXPECT_THROW(VeoClient(cfg, "private-local-key"), std::invalid_argument);
    cfg = config(peer); cfg["endpoint"]["origin"] = "http://example.invalid";
    EXPECT_THROW(VeoClient(cfg, "private-local-key"), std::invalid_argument);
    cfg = config(peer); cfg["defaults"]["resolution"] = "1080p";
    EXPECT_THROW(VeoClient(cfg, "private-local-key"), std::invalid_argument);
}

TEST(TypedVeoClient, SignedHttpsRedirectDownloadsWithoutForwardingApiKey) {
    const char* origin = std::getenv("NEOGRAPH_VEO_SIGNED_ORIGIN");
    if (!origin) GTEST_SKIP() << "Run via tests/typed_veo_peer.py for isolated HTTPS proof";
    VeoLocalPeer peer; peer.done();
    peer.file_status = 302;
    peer.file_location = std::string(origin) + "/signed?X-Goog-Signature=local";
    auto cfg = config(peer); cfg["download"]["allowed_origins"].push_back(origin);
    VeoClient client(cfg, "private-local-key");
    const auto submitted = submit(client);
    ASSERT_TRUE(submitted.operation);
    ASSERT_EQ(async::run_sync(client.poll(*submitted.operation)).status, VeoStatus::Succeeded);
    const auto downloaded = async::run_sync(client.download(*submitted.operation));
    ASSERT_TRUE(downloaded.artifact);
    ASSERT_TRUE(downloaded.artifact->bytes);
    EXPECT_EQ(std::to_integer<unsigned>((*downloaded.artifact->bytes)[4]), 'f');
    const auto result = async::run_sync(client.poll(*submitted.operation));
    EXPECT_EQ(result.downloads, 2u);
    EXPECT_EQ(peer.posts, 1);
}

TEST(TypedVeoClient, RedirectsRejectDowngradeUnadmittedOriginLoopsAndUserinfo) {
    const char* origin = std::getenv("NEOGRAPH_VEO_SIGNED_ORIGIN");
    if (!origin) GTEST_SKIP() << "Run via tests/typed_veo_peer.py for isolated HTTPS proof";
    for (const std::string& location : {
        std::string(origin) + "/downgrade",
        std::string(origin) + "/loop",
        std::string("https://attacker.invalid/signed"),
        std::string("https://private-local-key@localhost/signed"),
        std::string(origin) + "/signed\r\nx-goog-api-key: leaked"
    }) {
        VeoLocalPeer peer; peer.done();
        peer.file_status = 302; peer.file_location = location;
        auto cfg = config(peer); cfg["download"]["allowed_origins"].push_back(origin);
        VeoClient client(cfg, "private-local-key");
        const auto submitted = submit(client);
        ASSERT_TRUE(submitted.operation);
        ASSERT_EQ(async::run_sync(client.poll(*submitted.operation)).status, VeoStatus::Succeeded);
        const auto downloaded = async::run_sync(client.download(*submitted.operation));
        EXPECT_FALSE(downloaded.artifact);
        ASSERT_TRUE(downloaded.failure);
        EXPECT_EQ(downloaded.failure->kind, EndpointFailureKind::Protocol);
        EXPECT_EQ(peer.posts, 1);
    }
}

TEST(TypedVeoClient, InFlightStatusDeadlineDoesNotRequirePeerProgress) {
    VeoLocalPeer peer; peer.stall_poll = true;
    VeoClient client(config(peer), "private-local-key");
    const auto submitted = submit(client, request(), std::chrono::steady_clock::now() + 150ms);
    ASSERT_TRUE(submitted.operation);
    const auto result = async::run_sync(client.poll(*submitted.operation));
    ASSERT_TRUE(result.failure);
    EXPECT_EQ(result.failure->kind, EndpointFailureKind::DeadlineExceeded);
    EXPECT_EQ(peer.posts, 1);
    EXPECT_EQ(peer.gets, 1);
    EXPECT_FALSE(peer.release);
}

TEST(TypedVeoClient, ConcurrentPollCannotReplaceActiveCancellationSlotOrDispatchTwice) {
    VeoLocalPeer peer; peer.stall_poll = true;
    VeoClient client(config(peer), "private-local-key");
    const auto submitted = submit(client);
    ASSERT_TRUE(submitted.operation);
    auto active = std::async(std::launch::async, [&] {
        return async::run_sync(client.poll(*submitted.operation));
    });
    ASSERT_TRUE(peer.await_entry());
    const auto duplicate = async::run_sync(client.poll(*submitted.operation));
    ASSERT_TRUE(duplicate.failure);
    EXPECT_EQ(duplicate.failure->kind, EndpointFailureKind::InvalidRequest);
    EXPECT_EQ(peer.gets, 1);
    client.cancel(*submitted.operation);
    const auto cancelled = active.get();
    EXPECT_EQ(cancelled.status, VeoStatus::Cancelled);
    EXPECT_EQ(peer.posts, 1);
}

TEST(TypedVeoClient, LookupResumesOnlyServerVerifiedAdmittedOperationWithoutGeneration) {
    VeoLocalPeer peer; peer.done();
    VeoClient client(config(peer), "private-local-key");
    auto lookup = [](const VeoClient* c, std::string name) -> asio::awaitable<VeoSubmitResult> {
        co_return co_await c->lookup(co_await asio::this_coro::executor, std::move(name));
    };
    const auto resumed = async::run_sync(lookup(&client, operation_name));
    ASSERT_TRUE(resumed.operation);
    const auto result = async::run_sync(client.poll(*resumed.operation));
    EXPECT_EQ(result.status, VeoStatus::Succeeded);
    EXPECT_EQ(result.generation_dispatches, 0u);
    EXPECT_EQ(result.status_queries, 1u);
    EXPECT_EQ(peer.posts, 0);
    EXPECT_EQ(peer.gets, 1);
    const auto invalid = async::run_sync(lookup(&client, "../models/forged/operations/id"));
    ASSERT_TRUE(invalid.failure);
    EXPECT_EQ(invalid.failure->kind, EndpointFailureKind::InvalidRequest);
    EXPECT_EQ(peer.gets, 1);
}

TEST(TypedVeoClient, InlineVideoOwnsDecodedBytesAndReportedUsageWithoutDownloadAuthority) {
    VeoLocalPeer peer; peer.done();
    auto video = peer.status["response"]["generateVideoResponse"]["generatedSamples"][0]["video"];
    auto inline_video = json::object();
    for (const auto& [name, value] : video.items())
        if (name != "uri") inline_video[name] = value;
    inline_video["encodedVideo"] = "AAAAGGZ0eXBtcDQyAAAAAAAAAAAAAAAA";
    peer.status["response"]["generateVideoResponse"]["generatedSamples"][0]["video"] = std::move(inline_video);
    peer.status["response"]["usageMetadata"] = {
        {"promptTokenCount", 3}, {"candidatesTokenCount", 4}, {"totalTokenCount", 7},
        {"cachedContentTokenCount", 0}
    };
    VeoClient client(config(peer), "private-local-key");
    const auto submitted = submit(client);
    ASSERT_TRUE(submitted.operation);
    const auto result = async::run_sync(client.poll(*submitted.operation));
    ASSERT_EQ(result.status, VeoStatus::Succeeded);
    ASSERT_EQ(result.artifacts.size(), 1u);
    ASSERT_TRUE(result.artifacts[0].bytes);
    EXPECT_TRUE(result.artifacts[0].uri.empty());
    EXPECT_EQ(result.usage.total_tokens, std::optional<std::uint64_t>(7));
    EXPECT_EQ(result.usage.cached_input_tokens, std::optional<std::uint64_t>(0));
    const auto downloaded = async::run_sync(client.download(*submitted.operation));
    ASSERT_TRUE(downloaded.artifact);
    EXPECT_EQ(downloaded.artifact->bytes, result.artifacts[0].bytes);
    EXPECT_EQ(peer.files, 0);
    EXPECT_EQ(peer.posts, 1);
}

TEST(TypedVeoClient, MalformedOrNonVideoBase64CannotCompleteOperation) {
    for (const char* encoded : {"AAAAGGZ0eXBtcDQyAAAAAAAAAAAAAAA!", "AAAAAAAAAAAAAAAA"}) {
        VeoLocalPeer peer; peer.done();
        auto video = peer.status["response"]["generateVideoResponse"]["generatedSamples"][0]["video"];
        auto inline_video = json::object();
        for (const auto& [name, value] : video.items())
            if (name != "uri") inline_video[name] = value;
        inline_video["encodedVideo"] = encoded;
        peer.status["response"]["generateVideoResponse"]["generatedSamples"][0]["video"] = std::move(inline_video);
        VeoClient client(config(peer), "private-local-key");
        const auto submitted = submit(client);
        ASSERT_TRUE(submitted.operation);
        const auto result = async::run_sync(client.poll(*submitted.operation));
        ASSERT_TRUE(result.failure);
        EXPECT_EQ(result.failure->kind, EndpointFailureKind::Protocol);
        EXPECT_EQ(result.status, VeoStatus::Failed);
        EXPECT_TRUE(result.artifacts.empty());
        EXPECT_EQ(peer.files, 0);
    }
}
