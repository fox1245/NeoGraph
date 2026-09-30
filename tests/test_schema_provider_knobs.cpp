// Per-call request knobs (CompletionParams::extra_fields): every knob a
// built-in schema declares reaches the wire, an undeclared key is an error that
// names the declared ones (a typo must not look like success), and the vendor
// constraints that would otherwise come back as HTTP 400 are schema rules.
//
// Anthropic (checked live): with thinking enabled `temperature` may only be 1
// and `max_tokens` must exceed `thinking.budget_tokens`.

#include <gtest/gtest.h>

#include <neograph/llm/json_path.h>
#include <neograph/llm/schema_provider.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

using neograph::CompletionParams;
using neograph::json;
using neograph::llm::SchemaProvider;
using neograph::llm::test_access::SchemaProviderTestAccess;

namespace {

std::unique_ptr<SchemaProvider> builtin(const std::string& name) {
    SchemaProvider::Config cfg;
    cfg.schema_path = name;
    cfg.api_key = "test-key";
    auto sp = SchemaProvider::create(cfg);
    EXPECT_NE(sp, nullptr);
    return sp;
}

CompletionParams params_with(const json& extra, int max_tokens = -1, float temperature = 0.7f) {
    CompletionParams p;
    p.model = "test-model";
    p.messages.push_back({"user", "hi"});
    p.extra_fields = extra;
    p.max_tokens = max_tokens;
    p.temperature = temperature;
    return p;
}

json body_of(SchemaProvider& sp, const json& extra, int max_tokens = -1, float temperature = 0.7f) {
    return SchemaProviderTestAccess::build_body(sp, params_with(extra, max_tokens, temperature));
}

struct Knob {
    std::string path;
    json value;
};

}  // namespace

TEST(PerCallKnobs, EveryDeclaredKnobReachesTheWire) {
    const std::vector<std::pair<std::string, std::vector<Knob>>> declared = {
        {"claude",
         {{"provider", json{{"order", json::array({"anthropic"})}}},
          {"thinking", json{{"type", "enabled"}, {"budget_tokens", 1024}}},
          {"output_config.effort", "high"},
          {"cache_control", json{{"type", "ephemeral"}}},
          {"tool_choice", json{{"type", "auto"}}}}},
        {"gemini",
         {{"generationConfig.thinkingConfig.thinkingBudget", 512},
          {"generationConfig.thinkingConfig.thinkingLevel", "low"},
          {"generationConfig.thinkingConfig.includeThoughts", true},
          {"safetySettings",
           json::array({json{{"category", "HARM_CATEGORY_HARASSMENT"}, {"threshold", "BLOCK_NONE"}}})},
          {"toolConfig", json{{"functionCallingConfig", json{{"mode", "ANY"}}}}}}},
        {"openai_responses",
         {{"provider", json{{"order", json::array({"openai"})}}},
          {"reasoning.effort", "low"},
          {"reasoning.summary", "auto"},
          {"store", false},
          {"include", json::array({"reasoning.encrypted_content"})},
          {"previous_response_id", "resp_1"},
          {"parallel_tool_calls", false},
          {"text.verbosity", "low"},
          {"truncation", "auto"}}},
        {"openai",
         {{"provider", json{{"order", json::array({"openai"})}}},
          {"reasoning_effort", "low"},
          {"reasoning", json{{"effort", "low"}}},
          {"include_reasoning", true},
          {"usage", json{{"include", true}}},
          {"models", json::array({"openai/gpt-5-nano", "openai/gpt-4o-mini"})},
          {"response_format", json{{"type", "json_object"}}}}},
    };
    for (const auto& [schema, knobs] : declared) {
        auto sp = builtin(schema);
        for (const auto& knob : knobs) {
            const json body = body_of(*sp, json{{knob.path, knob.value}});
            const auto at = neograph::llm::json_path::at_path(body, knob.path);
            ASSERT_TRUE(at.has_value()) << schema << ": " << knob.path << " never reached the body";
            EXPECT_EQ(*at, knob.value) << schema << ": " << knob.path;
        }
    }
}

TEST(PerCallKnobs, AnUndeclaredKeyIsAnErrorNamingTheDeclaredOnes) {
    for (const char* schema : {"claude", "gemini", "openai_responses", "openai"}) {
        auto sp = builtin(schema);
        try {
            (void)body_of(*sp, json{{"reasonin.effort", "high"}});  // typo
            FAIL() << schema << ": an undeclared key must not be dropped silently";
        } catch (const std::invalid_argument& error) {
            const std::string what = error.what();
            EXPECT_NE(what.find("reasonin.effort"), std::string::npos) << what;
            EXPECT_NE(what.find("declared keys"), std::string::npos) << what;
        }
    }
    // The message lists the schema's own keys.
    auto claude = builtin("claude");
    try {
        (void)body_of(*claude, json{{"temperature_typo", 1}});
        FAIL();
    } catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string(error.what()).find("thinking"), std::string::npos) << error.what();
        EXPECT_NE(std::string(error.what()).find("output_config.effort"), std::string::npos);
    }
}

TEST(PerCallKnobs, TheDeclaredKnobsOfOneSchemaAreNotAcceptedByAnother) {
    auto gemini = builtin("gemini");
    EXPECT_THROW((void)body_of(*gemini, json{{"reasoning.effort", "low"}}), std::invalid_argument);
    auto chat = builtin("openai");
    EXPECT_THROW((void)body_of(*chat, json{{"thinking", json{{"type", "enabled"}}}}),
                 std::invalid_argument);
}

// ─── Anthropic rules ───

TEST(PerCallKnobsAnthropicRules, ThinkingOnLeavesTemperatureToTheServer) {
    auto sp = builtin("claude");
    for (const char* type : {"enabled", "adaptive"}) {
        const json body = body_of(
            *sp, json{{"thinking", json{{"type", type}, {"budget_tokens", 1024}}}});
        EXPECT_FALSE(body.contains("temperature")) << type << ": " << body.dump();
        EXPECT_EQ(body.at("thinking").at("type"), type);
    }
}

TEST(PerCallKnobsAnthropicRules, TemperatureIsKeptWhenThinkingIsOffOrDisabled) {
    auto sp = builtin("claude");
    const json plain = body_of(*sp, json::object());
    ASSERT_TRUE(plain.contains("temperature")) << plain.dump();
    EXPECT_NEAR(plain.at("temperature").get<double>(), 0.7, 1e-6);

    const json disabled = body_of(*sp, json{{"thinking", json{{"type", "disabled"}}}});
    EXPECT_TRUE(disabled.contains("temperature")) << disabled.dump();
}

TEST(PerCallKnobsAnthropicRules, MaxTokensMustExceedTheThinkingBudget) {
    auto sp = builtin("claude");
    const json thinking = json{{"thinking", json{{"type", "enabled"}, {"budget_tokens", 2048}}}};

    for (const int max_tokens : {512, 2048}) {
        try {
            (void)body_of(*sp, thinking, max_tokens);
            FAIL() << "max_tokens=" << max_tokens << " must be rejected before the request";
        } catch (const std::invalid_argument& error) {
            const std::string what = error.what();
            EXPECT_NE(what.find("max_tokens"), std::string::npos) << what;
            EXPECT_NE(what.find("thinking.budget_tokens"), std::string::npos) << what;
        }
    }
    const json ok = body_of(*sp, thinking, 4096);
    EXPECT_EQ(ok.at("max_tokens"), 4096);
    // The schema default (4096) also has to clear a large budget.
    EXPECT_THROW((void)body_of(*sp, json{{"thinking", json{{"type", "enabled"}, {"budget_tokens", 8192}}}}),
                 std::invalid_argument);
}

TEST(PerCallKnobsAnthropicRules, AdaptiveThinkingHasNoBudgetSoNothingToCompare) {
    auto sp = builtin("claude");
    const json body = body_of(*sp, json{{"thinking", json{{"type", "adaptive"}}}}, 100);
    EXPECT_EQ(body.at("max_tokens"), 100);
}

// ─── rule / policy mechanics on a custom schema ───

#ifndef _WIN32
namespace {
std::unique_ptr<SchemaProvider> custom(const std::string& request_extra) {
    const std::string schema = std::string(R"({"name":"knobs","connection":{"base_url":"http://127.0.0.1:9","endpoint":"/e"},)") +
        R"("request":{"model_field":"model","messages_field":"messages","temperature_path":"gen.temperature",)" +
        R"("max_tokens_path":"gen.max","per_call_fields":["gen.mode"])" + request_extra + R"(},)" +
        R"("system_prompt":{"strategy":"in_messages"},)" +
        R"("messages":{"role_field":"role","content_field":"content"},"tool_call_in_message":{"strategy":"tool_calls_array"},)" +
        R"("tool_result":{"strategy":"flat"},"image":{"strategy":"none"},)" +
        R"("response":{"strategy":"choices_message","message_path":"choices.0.message"},"streaming":{"format":"sse_data"}})";
    char path[] = "/tmp/neograph_knobs_XXXXXX.json";
    const int fd = mkstemps(path, 5);
    EXPECT_GE(fd, 0);
    close(fd);
    { std::ofstream(path) << schema; }
    SchemaProvider::Config cfg;
    cfg.schema_path = path;
    cfg.api_key = "k";
    cfg.allow_insecure_loopback = true;
    std::unique_ptr<SchemaProvider> sp;
    try {
        sp = SchemaProvider::create(cfg);
    } catch (...) {
        std::remove(path);
        throw;
    }
    std::remove(path);
    return sp;
}
}  // namespace

TEST(PerCallKnobsMechanics, ASchemaWithoutAPolicyStillDropsUnknownKeys) {
    auto sp = custom("");
    const json body = body_of(*sp, json{{"gen.mode", "fast"}, {"gen.unknown", 1}});
    EXPECT_EQ(body.at("gen").at("mode"), "fast");
    EXPECT_FALSE(body.at("gen").contains("unknown"));
}

TEST(PerCallKnobsMechanics, ThePolicyCanBeSwitchedOnPerSchema) {
    auto sp = custom(R"(,"unknown_knob_policy":"error")");
    EXPECT_THROW((void)body_of(*sp, json{{"gen.unknown", 1}}), std::invalid_argument);
}

TEST(PerCallKnobsMechanics, OmitRulesReachNestedPaths) {
    auto sp = custom(R"(,"rules":[{"omit":"gen.temperature","when":{"path":"gen.mode","in":["fast"]}}])");
    const json fast = body_of(*sp, json{{"gen.mode", "fast"}});
    EXPECT_FALSE(fast.at("gen").contains("temperature")) << fast.dump();
    EXPECT_EQ(fast.at("gen").at("mode"), "fast") << "siblings survive the rebuild";
    const json slow = body_of(*sp, json{{"gen.mode", "slow"}});
    EXPECT_TRUE(slow.at("gen").contains("temperature")) << slow.dump();
}

TEST(PerCallKnobsMechanics, AnUnconditionalOmitAlwaysDropsThePath) {
    auto sp = custom(R"(,"rules":[{"omit":"gen.temperature"}])");
    EXPECT_FALSE(body_of(*sp, json::object()).at("gen").contains("temperature"));
}

TEST(PerCallKnobsMechanics, MalformedPolicyOrRulesAreRejectedAtCreation) {
    for (const char* bad : {R"(,"unknown_knob_policy":"maybe")", R"(,"rules":"x")", R"(,"rules":[1])",
                            R"(,"rules":[{"nothing":true}])", R"(,"rules":[{"omit":""}])",
                            R"(,"rules":[{"omit":"a","when":{"path":"b"}}])",
                            R"(,"rules":[{"require_greater":{"path":"a"}}])"}) {
        EXPECT_THROW((void)custom(bad), std::invalid_argument) << bad;
    }
}
#endif  // !_WIN32
