#include <neograph/llm/decisions_client.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/steady_timer.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <istream>
#include <limits>
#include <string>
#include <stdexcept>
#include <thread>

namespace {
using namespace neograph;
using namespace neograph::llm;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// Private loopback HTTP fixture; the client admits it only with explicit
// config policy. Production never disables TLS certificate verification.
class TypedDecisionsPeer final {
public:
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor{io, {asio::ip::tcp::v4(), 0}};
    std::atomic<unsigned> dispatches{0};
    json response;
    int status = 200;
    std::chrono::milliseconds delay{0};
    std::string redirect;

    explicit TypedDecisionsPeer(json document, int code = 200,
        std::chrono::milliseconds wait = 0ms, std::string location = {})
        : response(std::move(document)), status(code), delay(wait), redirect(std::move(location)) {
        asio::co_spawn(io, accept_loop(), asio::detached);
        worker_ = std::thread([this] { io.run(); });
    }
    ~TypedDecisionsPeer() {
        io.stop();
        if (worker_.joinable()) worker_.join();
    }
    std::string endpoint() const {
        return "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/api/alpha/decisions";
    }
private:
    std::thread worker_;

    asio::awaitable<void> accept_loop() {
        try {
            for (;;) {
                auto socket = co_await acceptor.async_accept(asio::use_awaitable);
                asio::co_spawn(io, handle(std::move(socket)), asio::detached);
            }
        } catch (const std::exception&) { }
    }
    asio::awaitable<void> handle(asio::ip::tcp::socket socket) {
        try {
            asio::streambuf buffer;
            co_await asio::async_read_until(socket, buffer, "\r\n\r\n", asio::use_awaitable);
            std::istream input(&buffer);
            std::string line;
            std::getline(input, line);
            std::size_t length = 0;
            while (std::getline(input, line) && line != "\r") {
                if (line.starts_with("Content-Length:"))
                    length = static_cast<std::size_t>(std::stoull(line.substr(15)));
            }
            if (buffer.size() < length)
                co_await asio::async_read(socket, buffer,
                    asio::transfer_exactly(length - buffer.size()), asio::use_awaitable);
            std::string body(length, '\0');
            input.read(body.data(), static_cast<std::streamsize>(length));
            const auto request = json::parse(body);
            bool admitted = request.is_object() && !request.contains("criteria") &&
                request.contains("model") && request.contains("state") &&
                request.contains("questions") && request["questions"].is_object();
            if (admitted) {
                for (const auto& [name, question] : request["questions"].items()) {
                    admitted = admitted && question.is_object() && question.contains("instructions") &&
                        question.contains("type") && question["type"].is_string();
                    if (!admitted) break;
                    const auto kind = question["type"].get<std::string>();
                    if (kind == "noul") {
                        if (question.contains("criteria"))
                            admitted = question["criteria"].is_object() &&
                                question["criteria"].contains("false") && question["criteria"].contains("true");
                    } else if (kind == "choice")
                        admitted = question.contains("criteria") && question["criteria"].is_object();
                    else if (kind == "score")
                        admitted = question.contains("criteria") && question["criteria"].is_array() &&
                            !question["criteria"].empty();
                    else admitted = false;
                    if (!admitted) break;
                }
            }
            ++dispatches;
            if (delay.count() > 0) {
                asio::steady_timer timer(io, delay);
                co_await timer.async_wait(asio::use_awaitable);
            }
            auto text = admitted ? response.dump() : std::string("{\"error\":{\"code\":400}}");
            const auto code = admitted ? status : 400;
            std::string wire = "HTTP/1.1 " + std::to_string(code) + " Fixture\r\nContent-Type: application/json\r\n";
            if (!redirect.empty()) wire += "Location: " + redirect + "\r\n";
            wire += "Content-Length: " + std::to_string(text.size()) + "\r\nConnection: close\r\n\r\n" + text;
            co_await asio::async_write(socket, asio::buffer(wire), asio::use_awaitable);
        } catch (const std::exception&) { }
    }
};
json fixture_config(const std::string& endpoint) {
    return {
        {"version", 1}, {"endpoint", endpoint}, {"allow_insecure_loopback", true},
        {"default_model", "typesafe/jev-1.13"},
        {"models", json::array({{{"id", "typesafe/jev-1.13"},
            {"prompt_usd_per_million_tokens", 0.042},
            {"output_usd_per_million_tokens", 0}, {"implicit_caching", false}}})},
        {"limits", {{"timeout_ms", 2000}, {"max_questions", 8}, {"max_criteria", 8},
            {"max_request_bytes", 65536}, {"max_response_header_bytes", 4096},
            {"max_response_body_bytes", 65536}, {"max_response_chunk_bytes", 65536}}}
    };
}
DecisionsConfig admit(const json& document) {
    auto admitted = DecisionsConfig::from_json(document);
    if (auto* error = std::get_if<EndpointFailure>(&admitted))
        throw std::runtime_error(error->safe_message);
    return std::get<DecisionsConfig>(std::move(admitted));
}
DecisionsRequest tiny_request() {
    DecisionsRequest request;
    request.state = "The checkout page is blank.";
    request.questions.emplace("defect", DecisionsNoulQuestion{
        "Is this a defect?", DecisionsNoulCriteria{"feature request", "broken behavior"}});
    return request;
}
json tiny_response() {
    return {{"model", "typesafe/jev-1.13-20260917"},
        {"answers", {{"defect", {{"type", "noul"}, {"noul", 0.96}}}}},
        {"usage", {{"input_tokens", 31}, {"output_tokens", 6}, {"cost", 0.000001302}}}};
}
DecisionsResult run(asio::awaitable<DecisionsResult> pending) {
    asio::io_context io;
    auto future = asio::co_spawn(io, std::move(pending), asio::use_future);
    io.run();
    return future.get();
}

TEST(TypedDecisionsClient, AllThreeKindsRetainTypedEvidence) {
    auto document = tiny_response();
    document["id"] = "gen-local";
    document["provider"] = "TypeSafe";
    document["answers"]["team"] = {{"type", "choice"}, {"choice", "payments"},
        {"confidence", 0.75}, {"probabilities", {{"payments", 0.84}, {"frontend", 0.16}}}};
    document["answers"]["urgency"] = {{"type", "score"}, {"score", 1.99},
        {"confidence", 0.99}, {"probabilities", {{"0", 0.0}, {"1", 0.01}, {"2", 0.99}}},
        {"legend", {{"0", "next release"}, {"1", "this week"}, {"2", json{{"blocking", true}}}}}};
    TypedDecisionsPeer peer(std::move(document));
    auto request = tiny_request();
    request.questions.emplace("team", DecisionsChoiceQuestion{
        "Who owns this?", {{"payments", "billing"}, {"frontend", nullptr}}});
    request.questions.emplace("urgency", DecisionsScoreQuestion{
        json{{"question", "How urgent?"}}, {"next release", "this week", json{{"blocking", true}}}});
    DecisionsClient client(admit(fixture_config(peer.endpoint())), "fixture-key");
    auto result = run(client.submit_async(std::move(request)));
    ASSERT_TRUE(std::holds_alternative<DecisionsResponse>(result));
    const auto& value = std::get<DecisionsResponse>(result);
    EXPECT_DOUBLE_EQ(std::get<DecisionsNoulAnswer>(value.answers.at("defect")).probability, 0.96);
    const auto& choice = std::get<DecisionsChoiceAnswer>(value.answers.at("team"));
    EXPECT_EQ(choice.choice, "payments");
    ASSERT_TRUE(choice.confidence);
    EXPECT_DOUBLE_EQ(*choice.confidence, 0.75);
    ASSERT_TRUE(choice.probabilities);
    EXPECT_DOUBLE_EQ(choice.probabilities->at("frontend"), 0.16);
    const auto& score = std::get<DecisionsScoreAnswer>(value.answers.at("urgency"));
    EXPECT_DOUBLE_EQ(score.score, 1.99);
    ASSERT_TRUE(score.legend);
    EXPECT_EQ(score.legend->at("2"), (json{{"blocking", true}}));
    EXPECT_EQ(value.id, "gen-local");
    EXPECT_EQ(value.provider, "TypeSafe");
    EXPECT_EQ(value.model, "typesafe/jev-1.13-20260917");
    ASSERT_TRUE(value.cost_usd);
    EXPECT_DOUBLE_EQ(*value.cost_usd, 0.000001302);
    EXPECT_EQ(value.usage.input_tokens, 31u);
    EXPECT_EQ(peer.dispatches.load(), 1u);
}

TEST(TypedDecisionsClient, UnknownAndWideUsageAreNotZeroOrTruncated) {
    auto document = tiny_response();
    document["usage"] = {{"input_tokens", std::uint64_t{1} << 40},
        {"output_tokens", nullptr}, {"cost", nullptr}, {"future_metric", "preserved"}};
    TypedDecisionsPeer peer(std::move(document));
    DecisionsClient client(admit(fixture_config(peer.endpoint())), "fixture-key");
    auto result = run(client.submit_async(tiny_request()));
    ASSERT_TRUE(std::holds_alternative<DecisionsResponse>(result));
    const auto& value = std::get<DecisionsResponse>(result);
    EXPECT_EQ(value.usage.input_tokens, std::uint64_t{1} << 40);
    EXPECT_FALSE(value.usage.output_tokens);
    EXPECT_FALSE(value.usage.total_tokens);
    EXPECT_FALSE(value.cost_usd);
    ASSERT_TRUE(value.usage.reported);
    EXPECT_EQ(value.usage.reported->at("future_metric"), "preserved");
}

TEST(TypedDecisionsClient, MissingUsageRemainsUnknown) {
    auto document = tiny_response();
    auto without_usage = json::object();
    for (const auto& [name, value] : document.items())
        if (name != "usage") without_usage[name] = value;
    document = std::move(without_usage);
    TypedDecisionsPeer peer(std::move(document));
    DecisionsClient client(admit(fixture_config(peer.endpoint())), "fixture-key");
    auto result = run(client.submit_async(tiny_request()));
    ASSERT_TRUE(std::holds_alternative<DecisionsResponse>(result));
    const auto& value = std::get<DecisionsResponse>(result);
    EXPECT_FALSE(value.usage.input_tokens);
    EXPECT_FALSE(value.usage.output_tokens);
    EXPECT_FALSE(value.usage.reported);
    EXPECT_FALSE(value.cost_usd);
}

TEST(TypedDecisionsClient, RejectsMismatchedInvalidAndStaleAnswers) {
    for (int scenario = 0; scenario != 5; ++scenario) {
        auto document = tiny_response();
        if (scenario == 0) document["answers"]["defect"] = {{"type", "choice"}, {"choice", "true"}};
        if (scenario == 1) document["answers"]["defect"]["noul"] = 1.5;
        if (scenario == 2) document["answers"] = json::array({{{"noul", 0.9}}});
        if (scenario == 3) document["usage"]["input_tokens"] = -1;
        if (scenario == 4) document["answers"]["unrequested"] = {{"type", "noul"}, {"noul", 0.2}};
        TypedDecisionsPeer peer(std::move(document));
        DecisionsClient client(admit(fixture_config(peer.endpoint())), "fixture-key");
        auto result = run(client.submit_async(tiny_request()));
        ASSERT_TRUE(std::holds_alternative<EndpointFailure>(result)) << scenario;
        EXPECT_EQ(std::get<EndpointFailure>(result).kind, EndpointFailureKind::Protocol);
        EXPECT_TRUE(std::get<EndpointFailure>(result).request_may_have_left);
    }
}

TEST(TypedDecisionsClient, BadNamedControlsAndQuestionGrammarNeverDispatch) {
    TypedDecisionsPeer peer(tiny_response());
    DecisionsClient client(admit(fixture_config(peer.endpoint())), "fixture-key");
    for (int scenario = 0; scenario != 10; ++scenario) {
        auto request = tiny_request();
        if (scenario == 0) request.state = 3;
        if (scenario == 1) std::get<DecisionsNoulQuestion>(request.questions.at("defect")).instructions = false;
        if (scenario == 2) request.questions["defect"] = DecisionsScoreQuestion{"score", {}};
        if (scenario == 3) request.questions["defect"] = DecisionsScoreQuestion{"score", {nullptr}};
        if (scenario == 4) request.session_id = std::string(257, 's');
        if (scenario == 5) request.model = "chat/model";
        if (scenario == 6) request.trace = json{{"trace_id", 17}};
        if (scenario == 7) {
            request.provider.emplace();
            request.provider->preferred_max_latency = -1.0;
        }
        if (scenario == 8) {
            request.provider.emplace();
            request.provider->options["typesafe"] = json{{"max_tokens", 5}};
        }
        if (scenario == 9) request.state = json{{"number", std::numeric_limits<double>::infinity()}};
        auto result = run(client.submit_async(std::move(request)));
        ASSERT_TRUE(std::holds_alternative<EndpointFailure>(result)) << scenario;
        const auto& error = std::get<EndpointFailure>(result);
        EXPECT_EQ(error.kind, EndpointFailureKind::InvalidRequest);
        EXPECT_FALSE(error.request_may_have_left);
    }
    EXPECT_EQ(peer.dispatches.load(), 0u);
}

TEST(TypedDecisionsClient, ClosedConfigRejectsUnknownControlsAndCredentialLeakPaths) {
    auto base = fixture_config("https://openrouter.ai/api/alpha/decisions");
    for (int scenario = 0; scenario != 6; ++scenario) {
        auto document = base;
        if (scenario == 0) document["criteria"] = json::array();
        if (scenario == 1) document["version"] = 2;
        if (scenario == 2) document["limits"]["timeout_ms"] = 0;
        if (scenario == 3) document["endpoint"] = "http://example.com/api/alpha/decisions";
        if (scenario == 4) document["endpoint"] = "https://fixture-key@openrouter.ai/api/alpha/decisions";
        if (scenario == 5) document["limits"]["max_redirects"] = 1;
        auto result = DecisionsConfig::from_json(document);
        ASSERT_TRUE(std::holds_alternative<EndpointFailure>(result)) << scenario;
        const auto& error = std::get<EndpointFailure>(result);
        EXPECT_EQ(error.kind, EndpointFailureKind::InvalidConfig);
        EXPECT_EQ(error.safe_message.find("fixture-key"), std::string::npos);
    }
    EXPECT_THROW(DecisionsClient(admit(base), "key\r\nInjected: value"), std::invalid_argument);
}

TEST(TypedDecisionsClient, ProviderErrorDoesNotExposeSecretsOrRetry) {
    TypedDecisionsPeer peer({{"error", {{"code", 429}, {"message", "fixture-key private state"},
        {"metadata", {{"credential", "fixture-key"}}}}}}, 429);
    DecisionsClient client(admit(fixture_config(peer.endpoint())), "fixture-key");
    auto result = run(client.submit_async(tiny_request()));
    ASSERT_TRUE(std::holds_alternative<EndpointFailure>(result));
    const auto& error = std::get<EndpointFailure>(result);
    EXPECT_EQ(error.kind, EndpointFailureKind::Provider);
    EXPECT_EQ(error.http_status, 429);
    EXPECT_EQ(error.provider_code, "429");
    EXPECT_TRUE(error.request_may_have_left);
    EXPECT_EQ(error.safe_message.find("fixture-key"), std::string::npos);
    EXPECT_EQ(error.safe_message.find("private state"), std::string::npos);
    EXPECT_EQ(peer.dispatches.load(), 1u);
}

TEST(TypedDecisionsClient, RedirectIsReturnedWithoutCredentialReplay) {
    TypedDecisionsPeer peer(json::object(), 307, 0ms, "/api/alpha/decisions");
    DecisionsClient client(admit(fixture_config(peer.endpoint())), "fixture-key");
    auto result = run(client.submit_async(tiny_request()));
    ASSERT_TRUE(std::holds_alternative<EndpointFailure>(result));
    EXPECT_EQ(std::get<EndpointFailure>(result).http_status, 307);
    EXPECT_EQ(peer.dispatches.load(), 1u);
}

TEST(TypedDecisionsClient, PreCancelledAndExpiredNeverDispatch) {
    TypedDecisionsPeer peer(tiny_response());
    DecisionsClient client(admit(fixture_config(peer.endpoint())), "fixture-key");
    auto token = std::make_shared<graph::CancelToken>();
    token->cancel();
    auto cancelled = run(client.submit_async(tiny_request(), token));
    ASSERT_TRUE(std::holds_alternative<EndpointFailure>(cancelled));
    EXPECT_EQ(std::get<EndpointFailure>(cancelled).kind, EndpointFailureKind::Cancelled);
    auto expired = run(client.submit_async(tiny_request(), {}, Clock::now() - 1ms));
    ASSERT_TRUE(std::holds_alternative<EndpointFailure>(expired));
    EXPECT_EQ(std::get<EndpointFailure>(expired).kind, EndpointFailureKind::DeadlineExceeded);
    EXPECT_FALSE(std::get<EndpointFailure>(expired).request_may_have_left);
    EXPECT_EQ(peer.dispatches.load(), 0u);
}

TEST(TypedDecisionsClient, InFlightDeadlineHasOneDispatch) {
    TypedDecisionsPeer peer(tiny_response(), 200, 500ms);
    DecisionsClient client(admit(fixture_config(peer.endpoint())), "fixture-key");
    auto result = run(client.submit_async(tiny_request(), {}, Clock::now() + 100ms));
    ASSERT_TRUE(std::holds_alternative<EndpointFailure>(result));
    EXPECT_EQ(std::get<EndpointFailure>(result).kind, EndpointFailureKind::DeadlineExceeded);
    EXPECT_TRUE(std::get<EndpointFailure>(result).request_may_have_left);
    EXPECT_EQ(peer.dispatches.load(), 1u);
}

TEST(TypedDecisionsClient, InFlightCancellationStopsWaitingWithoutRetry) {
    TypedDecisionsPeer peer(tiny_response(), 200, 5s);
    DecisionsClient client(admit(fixture_config(peer.endpoint())), "fixture-key");
    auto token = std::make_shared<graph::CancelToken>();
    asio::io_context io;
    auto future = asio::co_spawn(io, client.submit_async(tiny_request(), token), asio::use_future);
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        asio::steady_timer timer(io);
        const auto limit = Clock::now() + 1s;
        while (peer.dispatches.load() == 0 && Clock::now() < limit) {
            timer.expires_after(1ms);
            co_await timer.async_wait(asio::use_awaitable);
        }
        token->cancel();
    }, asio::detached);
    io.run();
    auto result = future.get();
    ASSERT_TRUE(std::holds_alternative<EndpointFailure>(result));
    EXPECT_EQ(std::get<EndpointFailure>(result).kind, EndpointFailureKind::Cancelled);
    EXPECT_TRUE(std::get<EndpointFailure>(result).request_may_have_left);
    EXPECT_EQ(peer.dispatches.load(), 1u);
}

TEST(TypedDecisionsClient, AwaitableOwnsClientAndRequestBeforeScheduling) {
    TypedDecisionsPeer peer(tiny_response());
    auto pending = [&] {
        DecisionsClient client(admit(fixture_config(peer.endpoint())), "fixture-key");
        return client.submit_async(tiny_request());
    }();
    auto result = run(std::move(pending));
    ASSERT_TRUE(std::holds_alternative<DecisionsResponse>(result));
    EXPECT_DOUBLE_EQ(std::get<DecisionsNoulAnswer>(
        std::get<DecisionsResponse>(result).answers.at("defect")).probability, 0.96);
}

TEST(TypedDecisionsClient, RequestByteLimitHasNoDispatch) {
    TypedDecisionsPeer peer(tiny_response());
    auto config = fixture_config(peer.endpoint());
    config["limits"]["max_request_bytes"] = 16;
    DecisionsClient client(admit(config), "fixture-key");
    auto result = run(client.submit_async(tiny_request()));
    ASSERT_TRUE(std::holds_alternative<EndpointFailure>(result));
    EXPECT_EQ(std::get<EndpointFailure>(result).kind, EndpointFailureKind::ResourceLimit);
    EXPECT_FALSE(std::get<EndpointFailure>(result).request_may_have_left);
    EXPECT_EQ(peer.dispatches.load(), 0u);
}
TEST(TypedDecisionsClient, ChoiceAndScoreRejectUnrequestedOrOutOfRangeEvidence) {
    for (int scenario = 0; scenario != 5; ++scenario) {
        auto document = tiny_response();
        auto request = tiny_request();
        if (scenario < 2) {
            request.questions["defect"] = DecisionsChoiceQuestion{"owner", {{"payments", "billing"}}};
            document["answers"]["defect"] = {{"type", "choice"}, {"choice", "payments"}};
            if (scenario == 0) document["answers"]["defect"]["choice"] = "unrequested";
            else document["answers"]["defect"]["probabilities"] = {{"unrequested", 0.1}};
        } else {
            request.questions["defect"] = DecisionsScoreQuestion{"urgency", {"low", "high"}};
            document["answers"]["defect"] = {{"type", "score"}, {"score", 0.5}};
            if (scenario == 2) document["answers"]["defect"]["score"] = 2.0;
            if (scenario == 3) document["answers"]["defect"]["probabilities"] = {{"01", 0.5}};
            if (scenario == 4) document["answers"]["defect"]["legend"] = {{"0", nullptr}};
        }
        TypedDecisionsPeer peer(std::move(document));
        DecisionsClient client(admit(fixture_config(peer.endpoint())), "fixture-key");
        auto result = run(client.submit_async(std::move(request)));
        ASSERT_TRUE(std::holds_alternative<EndpointFailure>(result)) << scenario;
        EXPECT_EQ(std::get<EndpointFailure>(result).kind, EndpointFailureKind::Protocol);
    }
}

TEST(TypedDecisionsClient, MinimalChoiceAnswerDoesNotInventConfidenceOrDistribution) {
    auto document = tiny_response();
    document["answers"]["defect"] = {{"type", "choice"}, {"choice", "payments"}};
    TypedDecisionsPeer peer(std::move(document));
    auto request = tiny_request();
    request.questions["defect"] = DecisionsChoiceQuestion{"owner", {{"payments", "billing"}}};
    DecisionsClient client(admit(fixture_config(peer.endpoint())), "fixture-key");
    auto result = run(client.submit_async(std::move(request)));
    ASSERT_TRUE(std::holds_alternative<DecisionsResponse>(result));
    const auto& answer = std::get<DecisionsChoiceAnswer>(
        std::get<DecisionsResponse>(result).answers.at("defect"));
    EXPECT_EQ(answer.choice, "payments");
    EXPECT_FALSE(answer.confidence);
    EXPECT_FALSE(answer.probabilities);
}

TEST(TypedDecisionsClient, ConfiguredDeadlineIsNotRenewedAtCoroutineStart) {
    TypedDecisionsPeer peer(tiny_response());
    auto config = fixture_config(peer.endpoint());
    config["limits"]["timeout_ms"] = 10;
    DecisionsClient client(admit(config), "fixture-key");
    auto pending = client.submit_async(tiny_request());
    std::this_thread::sleep_for(30ms);
    auto result = run(std::move(pending));
    ASSERT_TRUE(std::holds_alternative<EndpointFailure>(result));
    EXPECT_EQ(std::get<EndpointFailure>(result).kind, EndpointFailureKind::DeadlineExceeded);
    EXPECT_FALSE(std::get<EndpointFailure>(result).request_may_have_left);
    EXPECT_EQ(peer.dispatches.load(), 0u);
}

} // namespace
