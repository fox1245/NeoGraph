#include <gtest/gtest.h>

#include "../examples/cookbook/jarvis/src/provider_support.h"
#include <neograph/async/run_sync.h>
#include "fixtures/typed_provider.h"

#include <deque>
#include <mutex>
#include <vector>

using namespace neograph;

namespace {

class EnvValue {
public:
    EnvValue(const char* name, const char* value) : name_(name) {
        if (const char* previous = std::getenv(name)) previous_ = previous;
        set(value);
    }
    ~EnvValue() { set(previous_ ? previous_->c_str() : nullptr); }
private:
    void set(const char* value) {
#ifdef _WIN32
        _putenv_s(name_, value ? value : "");
#else
        if (value) setenv(name_, value, 1);
        else unsetenv(name_);
#endif
    }
    const char* name_;
    std::optional<std::string> previous_;
};

// Hands out scripted replies and records the caps/efforts it was asked with.
struct Script {
    std::mutex mutex;
    std::deque<sp::runtime::Result> replies;
    std::vector<std::optional<std::uint64_t>> caps;
    std::vector<std::optional<std::string>> efforts;
};

std::shared_ptr<test::LocalProvider> scripted(const std::shared_ptr<Script>& script) {
    return std::make_shared<test::LocalProvider>(
        [script](ProviderRequest request, const PreparedProviderRequest&,
                 const test::LocalProvider::EventCallback&) -> asio::awaitable<sp::runtime::Result> {
            std::lock_guard lock(script->mutex);
            const auto& chat = std::get<sp::chat::Request>(request.payload);
            script->caps.push_back(chat.max_output_tokens);
            script->efforts.push_back(chat.reasoning_effort);
            auto reply = std::move(script->replies.front());
            script->replies.pop_front();
            co_return reply;
        });
}

sp::runtime::Result completion(std::string text, sp::StopKind stop, std::uint64_t output_tokens) {
    sp::Completion value;
    if (!text.empty()) value.messages.push_back(test::message(std::move(text)));
    value.stop = {stop, stop == sp::StopKind::MaxTokens ? "length" : "stop"};
    value.usage = test::usage(1, output_tokens, 1 + output_tokens);
    return std::make_shared<const sp::Outcome>(std::move(value));
}

// What the synthesis and router nodes do: same request builder, node-owned ordinals.
struct Run {
    std::vector<unsigned> attempts;
    sp::runtime::Result reply;
    std::shared_ptr<UsageAccumulator> usage = std::make_shared<UsageAccumulator>();
    std::shared_ptr<graph::ProviderOutcomes> outcomes;
};

Run complete(const std::shared_ptr<Script>& script, std::uint64_t visible_tokens) {
    auto provider = scripted(script);
    graph::RunContext ctx;
    ctx.usage = std::make_shared<UsageAccumulator>();
    ctx.provider_outcomes = std::make_shared<graph::ProviderOutcomes>();
    Run run;
    run.usage = ctx.usage;
    run.outcomes = ctx.provider_outcomes;
    run.reply = async::run_sync(jarvis::providers::complete_with_visible_text(ctx,
        [&](unsigned attempt) {
            return jarvis::providers::request(*provider,
                {examples::message(sp::Role::User, "hi")}, 0.4,
                jarvis::providers::output_budget(visible_tokens, attempt));
        },
        [&](ProviderRequest request, unsigned attempt) {
            run.attempts.push_back(attempt);
            return provider->invoke_async(std::move(request));
        }));
    return run;
}

std::shared_ptr<Script> script_of(std::vector<sp::runtime::Result> replies) {
    auto script = std::make_shared<Script>();
    script->replies.assign(replies.begin(), replies.end());
    return script;
}

}  // namespace

TEST(JarvisProviderSupport, ReasoningExhaustedReplyIsRetriedOnceWithDoubledCap) {
    // The model spent the whole cap on hidden reasoning: stop=MaxTokens, no text.
    auto script = script_of({completion("", sp::StopKind::MaxTokens, 1244),
                             completion("Good morning.", sp::StopKind::EndTurn, 5)});
    const auto run = complete(script, 220);
    EXPECT_EQ(test::text(run.reply), "Good morning.");
    ASSERT_EQ(script->caps.size(), 2U);
    EXPECT_EQ(script->caps[0], std::optional<std::uint64_t>(1244));
    EXPECT_EQ(script->caps[1], std::optional<std::uint64_t>(2488));
    EXPECT_EQ(script->efforts, (std::vector<std::optional<std::string>>{"low", "low"}));
    // Each attempt is its own provider call (distinct broker ordinal) and both are accounted.
    EXPECT_EQ(run.attempts, (std::vector<unsigned>{0, 1}));
    const auto reports = run.usage->authority_snapshot().reports;
    ASSERT_TRUE(reports.output_total);
    EXPECT_EQ(reports.output_total->value, 1244U + 5U);
}

TEST(JarvisProviderSupport, StillEmptyAfterTheRetryThrowsWithoutAnotherRetry) {
    auto script = script_of({completion(" \n", sp::StopKind::MaxTokens, 1244),
                             completion("", sp::StopKind::MaxTokens, 2488),
                             completion("never asked for", sp::StopKind::EndTurn, 1)});
    EXPECT_THROW(complete(script, 220), ProviderOutcomeError);
    EXPECT_EQ(script->caps.size(), 2U);
}

TEST(JarvisProviderSupport, PartialVisibleTextAtTheLimitIsNotRetried) {
    auto script = script_of({completion("Half a sent", sp::StopKind::MaxTokens, 1244)});
    const auto run = complete(script, 220);
    EXPECT_EQ(script->caps.size(), 1U);
    EXPECT_EQ(test::text(run.reply), "Half a sent");
    EXPECT_FALSE(jarvis::providers::token_limit_without_text(*run.reply));
}

TEST(JarvisProviderSupport, ToolCallOutputAtTheLimitIsPreservedWithoutAnotherDispatch) {
    for (const sp::Part& part : std::vector<sp::Part>{
             sp::ToolCall{"call", "lookup", sp::ToolCallKind::ClientExecuted,
                          test::document(R"({"x":1})")},
             sp::InvalidToolCall{"call", "lookup", sp::ToolCallKind::ClientExecuted,
                                 R"({"x":)", sp::InvalidReason::Truncated}}) {
        sp::Completion value;
        sp::Message message;
        message.parts.push_back(part);
        value.messages.push_back(std::move(message));
        value.stop = {sp::StopKind::MaxTokens, "length"};
        const auto first = std::make_shared<const sp::Outcome>(std::move(value));
        auto script = script_of({first, completion("not dispatched", sp::StopKind::EndTurn, 1)});
        try {
            complete(script, 220);
            FAIL() << "tool-call output must not become a successful spoken reply";
        } catch (const ProviderOutcomeError& error) {
            EXPECT_EQ(error.outcome(), first);
            EXPECT_TRUE(jarvis::providers::has_tool_call_output(*error.outcome()));
        }
        EXPECT_EQ(script->caps.size(), 1U);
    }
}

TEST(JarvisProviderSupport, TruncatedRouterTextIsRejectedWithoutRetryOrFallback) {
    for (const std::string text : {std::string(R"({"mode":"direct","tool_calls":[)"),
                                   std::string(R"({"mode":"chat"})")}) {
        const auto first = completion(text, sp::StopKind::MaxTokens, 1324);
        auto script = script_of({first, completion("not dispatched", sp::StopKind::EndTurn, 1)});
        const auto run = complete(script, 300);
        EXPECT_EQ(run.reply, first);
        try {
            jarvis::providers::require_completed_router_output(run.reply);
            FAIL() << "even valid-looking JSON must not hide provider truncation";
        } catch (const ProviderOutcomeError& error) {
            EXPECT_EQ(error.outcome(), first);
            EXPECT_EQ(test::text(error.outcome()), text);
        }
        EXPECT_EQ(script->caps.size(), 1U);
    }
}

TEST(JarvisProviderSupport, EmptyReplyThatIsNotATokenLimitThrowsWithoutRetry) {
    auto script = script_of({completion(" \t\r\n", sp::StopKind::EndTurn, 0)});
    EXPECT_THROW(complete(script, 220), ProviderOutcomeError);
    EXPECT_EQ(script->caps.size(), 1U);
}

TEST(JarvisProviderSupport, ProviderFailureThrowsWithoutRetry) {
    auto script = script_of({test::failure(sp::ErrorKind::DeadlineExceeded)});
    EXPECT_THROW(complete(script, 220), ProviderFailure);
    EXPECT_EQ(script->caps.size(), 1U);
}

TEST(JarvisProviderSupport, CredentialedPlaintextEndpointsStayRejected) {
    EXPECT_THROW(jarvis::providers::live("key", "http://gateway.example:8080/openrouter/v1"),
                 std::invalid_argument);
    EXPECT_THROW(jarvis::providers::live("key", "http://127.0.0.1:8080/openrouter/v1"),
                 std::invalid_argument);
}

TEST(JarvisProviderSupport, TlsGatewayPreparationRetainsZdrAndExactOrigin) {
    // Admission/encoding only: a nonexistent CA file does not qualify TLS I/O.
    auto gateway = jarvis::providers::live(
        "key", "https://gateway.example:8443/openrouter/v1", "/certs/ca.pem");
    auto prepared = gateway->prepare(jarvis::providers::request(*gateway,
        {examples::message(sp::Role::User, "hi")}, 0.4,
        jarvis::providers::output_budget(220)));
    ASSERT_TRUE(prepared.valid());
    ASSERT_EQ(prepared.error(), nullptr);
    ASSERT_NE(prepared.admitted_descriptor(), nullptr);
    EXPECT_EQ(prepared.admitted_descriptor()->base_url(), "https://gateway.example:8443");
    EXPECT_EQ(neograph::json::parse(std::string(prepared.encoded_body())).at("provider").at("zdr"),
              neograph::json(true));
}

TEST(JarvisProviderSupport, CleanRouterDecisionRetainsTheOriginalCompletion) {
    for (const auto stop : {sp::StopKind::EndTurn, sp::StopKind::StopSequence}) {
        const auto first = completion(R"({"mode":"chat","tool_calls":[]})", stop, 10);
        auto script = script_of({first});
        const auto run = complete(script, 300);
        ASSERT_NO_THROW(jarvis::providers::require_completed_router_output(run.reply));
        EXPECT_EQ(run.reply, first);
        EXPECT_EQ(neograph::json::parse(test::text(run.reply)).at("mode"),
                  neograph::json("chat"));
        EXPECT_EQ(script->caps.size(), 1U);
    }
}

TEST(JarvisProviderSupport, ExplicitOutputCapIsNotDoubledOnANewSemanticCall) {
    EnvValue cap("NG_EXAMPLE_MAX_TOKENS", "1600");
    EnvValue reasks("NG_EXAMPLE_EMPTY_REASKS", "1");
    auto script = script_of({completion("", sp::StopKind::MaxTokens, 1600),
                             completion("Complete reply.", sp::StopKind::EndTurn, 4)});
    const auto run = complete(script, 220);
    EXPECT_EQ(test::text(run.reply), "Complete reply.");
    EXPECT_EQ(script->caps, (std::vector<std::optional<std::uint64_t>>{1600, 1600}));
    EXPECT_EQ(run.attempts, (std::vector<unsigned>{0, 1}));
    EXPECT_EQ(script->efforts, (std::vector<std::optional<std::string>>{"low", "low"}));
    const auto usage = run.usage->snapshot();
    ASSERT_TRUE(usage.output_total);
    EXPECT_EQ(usage.output_total->value, 1604U);
    const auto outcomes = run.outcomes->snapshot();
    ASSERT_EQ(outcomes.size(), 2U);
    EXPECT_EQ(std::get<sp::Completion>(*outcomes[0]).stop.kind, sp::StopKind::MaxTokens);
    EXPECT_EQ(outcomes[1], run.reply);
}

TEST(JarvisProviderSupport, CallerCanDisableEmptyReasksWithoutLosingTheOutcome) {
    EnvValue cap("NG_EXAMPLE_MAX_TOKENS", nullptr);
    EnvValue reasks("NG_EXAMPLE_EMPTY_REASKS", "0");
    const auto first = completion("", sp::StopKind::MaxTokens, 1244);
    auto script = script_of({first, completion("not dispatched", sp::StopKind::EndTurn, 4)});
    try {
        complete(script, 220);
        FAIL() << "empty completion must remain a failed spoken turn";
    } catch (const ProviderOutcomeError& error) {
        EXPECT_EQ(error.outcome(), first);
    }
    EXPECT_EQ(script->caps.size(), 1U);
}

TEST(JarvisProviderSupport, InvalidControlsFailBeforeProviderDispatch) {
    for (const auto* value : {"", "-1", "0", "1suffix", "18446744073709551616"}) {
        EnvValue cap("NG_EXAMPLE_MAX_TOKENS", value);
        EXPECT_THROW(jarvis::providers::output_budget(220), std::invalid_argument);
    }
    EnvValue cap("NG_EXAMPLE_MAX_TOKENS", nullptr);
    EnvValue reasks("NG_EXAMPLE_EMPTY_REASKS", "2");
    auto script = script_of({completion("", sp::StopKind::MaxTokens, 1244)});
    EXPECT_THROW(complete(script, 220), std::invalid_argument);
    EXPECT_TRUE(script->caps.empty());
}

TEST(JarvisProviderSupport, PartialOrRefusedTextCannotBecomeACompletedSpokenTurn) {
    for (const auto stop : {sp::StopKind::MaxTokens, sp::StopKind::Refusal,
                           sp::StopKind::ContentFilter, sp::StopKind::PauseTurn,
                           sp::StopKind::Unknown}) {
        const auto first = completion("Retained partial text", stop, 4);
        auto script = script_of({first});
        const auto run = complete(script, 220);
        EXPECT_EQ(run.reply, first);
        try {
            jarvis::providers::require_completed_spoken_output(run.reply);
            FAIL() << "non-terminal synthesis must not reach TTS";
        } catch (const ProviderOutcomeError& error) {
            EXPECT_EQ(error.outcome(), first);
            EXPECT_EQ(test::text(error.outcome()), "Retained partial text");
        }
        EXPECT_EQ(script->caps.size(), 1U);
    }
}

TEST(JarvisProviderSupport, CompletedSpokenTextRetainsItsOriginalOutcome) {
    for (const auto stop : {sp::StopKind::EndTurn, sp::StopKind::StopSequence}) {
        const auto first = completion("Hello.", stop, 4);
        auto script = script_of({first});
        const auto run = complete(script, 220);
        EXPECT_NO_THROW(jarvis::providers::require_completed_spoken_output(run.reply));
        EXPECT_EQ(run.reply, first);
        EXPECT_EQ(script->caps.size(), 1U);
    }
}
