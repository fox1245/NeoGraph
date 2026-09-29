// Issue #34 — RequestConfig::extra_fields applied unconditionally,
//              not only when params.tools is non-empty.
// Issue #35 — temperature_path: null in schema disables the temperature
//              field entirely; reasoning models don't get an unwanted
//              temperature: 0.7 stamped on every body.
//
// Both fixes live in src/llm/schema_provider.cpp::build_body. Tests
// drive build_body directly via the test_access friend helper.

// 본 테스트는 mkstemps + /tmp 같은 POSIX-only 경로를 쓴다. Windows
// 빌드에서는 통째로 skip — coverage 는 Linux/macOS CI 가 보장.
#ifndef _WIN32

#include <gtest/gtest.h>

#include <neograph/llm/schema_provider.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <fstream>
#include <string>
#include <unistd.h>

using neograph::CompletionParams;
using neograph::ChatMessage;
using neograph::json;
using neograph::llm::SchemaProvider;
using neograph::llm::SchemaStrategyFamily;
using neograph::llm::SchemaStrategyRegistry;
using neograph::llm::test_access::SchemaProviderTestAccess;

namespace {

// Write a JSON schema to a temp file so SchemaProvider::create can
// load it. Returns the absolute path; caller is responsible for the
// fixture's TearDown removing the file.
std::string write_temp_schema(const json& schema_doc) {
    char tmpl[] = "/tmp/neograph_schema_XXXXXX.json";
    int fd = mkstemps(tmpl, 5);
    if (fd < 0) {
        ADD_FAILURE() << "mkstemps failed";
        return "";
    }
    std::string path = tmpl;
    close(fd);
    std::ofstream out(path);
    out << schema_doc.dump();
    out.close();
    return path;
}

// Minimal valid OpenAI-Responses-shaped schema. Each test starts from
// this and patches the bits it cares about (extra_fields entries,
// temperature_path).
json base_schema() {
    return json{
        {"name", "test_schema"},
        {"connection", {
            {"base_url", "http://127.0.0.1:9999"},
            {"endpoint", "/v1/responses"},
            {"auth_header", "Authorization"},
            {"auth_prefix", "Bearer "},
            {"api_key_env", "TEST_API_KEY"},
        }},
        {"request", {
            {"model_field", "model"},
            {"messages_field", "input"},
            {"tools_field", "tools"},
            {"temperature_path", "temperature"},
            {"max_tokens_path", "max_output_tokens"},
            {"stream_field", "stream"},
        }},
        {"system_prompt", {
            {"strategy", "in_messages"},
        }},
        {"messages", {
            {"role_user", "user"},
            {"role_assistant", "assistant"},
            {"role_system", "system"},
        }},
        {"tool_call", {
            {"strategy", "tool_calls_array"},
        }},
        {"tool_result", {
            {"strategy", "flat"},
        }},
        {"image", {
            {"strategy", "none"},
        }},
        {"response", {
            {"strategy", "choices_message"},
            {"message_path", "choices.0.message"},
            {"content_path", "content"},
        }},
        {"stream", {
            {"format", "sse_data"},
        }},
    };
}

std::unique_ptr<SchemaProvider> make_provider_from(
    const json& schema,
    std::shared_ptr<const SchemaStrategyRegistry> strategy_registry = nullptr,
    json provider_routing = nullptr) {
    auto path = write_temp_schema(schema);
    if (path.empty()) return nullptr;

    SchemaProvider::Config cfg;
    cfg.schema_path = path;
    cfg.api_key = "test-key";
    cfg.default_model = "gpt-test";
    cfg.strategy_registry = std::move(strategy_registry);
    cfg.provider_routing = std::move(provider_routing);
    cfg.allow_insecure_loopback = true;

    auto sp = SchemaProvider::create(cfg);
    std::remove(path.c_str());
    return sp;
}

CompletionParams basic_params() {
    CompletionParams p;
    p.model = "gpt-test";
    p.messages.push_back({"user", "hello"});
    p.temperature = 0.7f;   // default-equivalent; let the schema decide
    return p;
}

}  // namespace

TEST(SchemaStrategyRegistry, InjectedAliasSelectsReviewedPrimitive) {
    auto schema = base_schema();
    schema["system_prompt"]["strategy"] = "claude_system";

    auto registry = std::make_shared<SchemaStrategyRegistry>();
    registry->register_alias(
        SchemaStrategyFamily::SystemPrompt, "claude_system", "top_level");

    auto sp = make_provider_from(schema, registry);
    ASSERT_NE(sp, nullptr);

    CompletionParams params = basic_params();
    params.messages.insert(params.messages.begin(), {"system", "be concise"});

    const json body = SchemaProviderTestAccess::build_body(*sp, params);
    ASSERT_TRUE(body.contains("system")) << body.dump();
    EXPECT_EQ(body["system"], "be concise");
    ASSERT_TRUE(body.contains("input")) << body.dump();
    ASSERT_EQ(body["input"].size(), 1U);
    EXPECT_EQ(body["input"][0]["role"], "user");
}

TEST(SchemaStrategyRegistry, RejectsUnknownPrimitiveAndDuplicateAlias) {
    SchemaStrategyRegistry registry;
    EXPECT_THROW(
        registry.register_alias(
            SchemaStrategyFamily::Response, "custom", "not_a_primitive"),
        std::invalid_argument);
    EXPECT_THROW(
        registry.register_alias(
            SchemaStrategyFamily::Response, "choices_message", "output_array"),
        std::invalid_argument);
    EXPECT_THROW(
        registry.resolve(SchemaStrategyFamily::Response, "missing"),
        std::invalid_argument);
}

// ─── #34: extra_fields applied without tools ───

TEST(SchemaExtraFields, AppliedWhenToolsAbsent) {
    auto schema = base_schema();
    schema["request"]["extra_fields"] = json{
        {"reasoning", {{"effort", "medium"}}},
        {"response_format", {{"type", "json_object"}}},
    };
    auto sp = make_provider_from(schema);
    ASSERT_NE(sp, nullptr);

    CompletionParams p = basic_params();
    ASSERT_TRUE(p.tools.empty());

    json body = SchemaProviderTestAccess::build_body(*sp, p);

    ASSERT_TRUE(body.contains("reasoning")) << body.dump();
    EXPECT_EQ(body["reasoning"]["effort"].get<std::string>(), "medium");
    ASSERT_TRUE(body.contains("response_format")) << body.dump();
    EXPECT_EQ(body["response_format"]["type"].get<std::string>(), "json_object");
}

TEST(SchemaExtraFields, AppliedWhenToolsPresent) {
    auto schema = base_schema();
    schema["request"]["extra_fields"] = json{
        {"reasoning", {{"effort", "high"}}},
    };
    auto sp = make_provider_from(schema);
    ASSERT_NE(sp, nullptr);

    CompletionParams p = basic_params();
    p.tools.push_back({"my_tool", "demo", json::object()});

    json body = SchemaProviderTestAccess::build_body(*sp, p);

    ASSERT_TRUE(body.contains("reasoning")) << body.dump();
    EXPECT_EQ(body["reasoning"]["effort"].get<std::string>(), "high");
    EXPECT_TRUE(body.contains("tools"));   // tools also there
}

TEST(SchemaExtraFields, EmptyExtraFieldsNoOp) {
    auto schema = base_schema();
    // request.extra_fields omitted entirely → defaults to {}; no body keys.
    auto sp = make_provider_from(schema);
    ASSERT_NE(sp, nullptr);

    json body = SchemaProviderTestAccess::build_body(*sp, basic_params());
    EXPECT_FALSE(body.contains("reasoning"));
    EXPECT_FALSE(body.contains("tools"));
}

// ─── #35: temperature_path opt-out via JSON null ───

TEST(SchemaTemperatureOptOut, NullPathSkipsField) {
    auto schema = base_schema();
    schema["request"]["temperature_path"] = nullptr;
    auto sp = make_provider_from(schema);
    ASSERT_NE(sp, nullptr);

    CompletionParams p = basic_params();
    p.temperature = 0.7f;   // would normally be written

    json body = SchemaProviderTestAccess::build_body(*sp, p);
    EXPECT_FALSE(body.contains("temperature")) << body.dump();
}

TEST(SchemaTemperatureOptOut, OmittedFieldDefaultsToTemperatureKey) {
    auto schema = base_schema();
    // Rebuild request block without the temperature_path key — schema
    // doesn't declare it. neograph::json doesn't expose erase(), so we
    // copy field-by-field via the structured-binding range loop.
    json req_no_temp;
    for (const auto& [k, v] : schema["request"].items()) {
        if (k != "temperature_path") req_no_temp[k] = v;
    }
    schema["request"] = req_no_temp;

    auto sp = make_provider_from(schema);
    ASSERT_NE(sp, nullptr);

    CompletionParams p = basic_params();
    p.temperature = 0.7f;

    json body = SchemaProviderTestAccess::build_body(*sp, p);
    ASSERT_TRUE(body.contains("temperature")) << body.dump();
    EXPECT_NEAR(body["temperature"].get<double>(), 0.7, 1e-5);
}

TEST(SchemaTemperatureOptOut, NullPathRespectsCallerSentinelToo) {
    // Combination case: schema opts out via null AND caller passes
    // negative sentinel. Should still be skipped (both opt-out paths
    // active), no surprises.
    auto schema = base_schema();
    schema["request"]["temperature_path"] = nullptr;
    auto sp = make_provider_from(schema);
    ASSERT_NE(sp, nullptr);

    CompletionParams p = basic_params();
    p.temperature = -1.0f;   // caller-side opt-out as well

    json body = SchemaProviderTestAccess::build_body(*sp, p);
    EXPECT_FALSE(body.contains("temperature"));
}

TEST(SchemaTemperatureOptOut, NestedTemperaturePathStillWorks) {
    // Sanity — the opt-out path doesn't break the existing nested-path
    // case (e.g. a future schema with `sampling.temperature` etc).
    auto schema = base_schema();
    schema["request"]["temperature_path"] = "sampling.temperature";
    auto sp = make_provider_from(schema);
    ASSERT_NE(sp, nullptr);

    CompletionParams p = basic_params();
    p.temperature = 0.5f;

    json body = SchemaProviderTestAccess::build_body(*sp, p);
    ASSERT_TRUE(body.contains("sampling")) << body.dump();
    ASSERT_TRUE(body["sampling"].contains("temperature")) << body.dump();
    EXPECT_NEAR(body["sampling"]["temperature"].get<double>(), 0.5, 1e-5);
}

// ─── #33: per-call extra_fields binding via schema's per_call_fields ───

TEST(SchemaPerCallFields, BoundPathLandsInBody) {
    auto schema = base_schema();
    // Schema declares: only reasoning.effort is bindable per-call.
    schema["request"]["per_call_fields"] = json::array({"reasoning.effort"});
    auto sp = make_provider_from(schema);
    ASSERT_NE(sp, nullptr);

    CompletionParams p = basic_params();
    p.extra_fields = json{{"reasoning.effort", "high"}};

    json body = SchemaProviderTestAccess::build_body(*sp, p);
    ASSERT_TRUE(body.contains("reasoning")) << body.dump();
    ASSERT_TRUE(body["reasoning"].contains("effort")) << body.dump();
    EXPECT_EQ(body["reasoning"]["effort"].get<std::string>(), "high");
}

TEST(SchemaPerCallFields, UnknownPathSilentlyDropped) {
    // Caller passes a path the schema didn't declare — should be a no-op,
    // not an error. Schema owns the contract; typo silently drops rather
    // than stamping a malformed key.
    auto schema = base_schema();
    schema["request"]["per_call_fields"] = json::array({"reasoning.effort"});
    auto sp = make_provider_from(schema);
    ASSERT_NE(sp, nullptr);

    CompletionParams p = basic_params();
    p.extra_fields = json{
        {"reasoning.effort", "low"},        // declared
        {"reasonin.effort",  "typo"},       // typo — not in allowlist
        {"random.knob",      "ignore me"},  // never declared
    };

    json body = SchemaProviderTestAccess::build_body(*sp, p);
    EXPECT_EQ(body["reasoning"]["effort"].get<std::string>(), "low");
    EXPECT_FALSE(body.contains("reasonin"));
    EXPECT_FALSE(body.contains("random"));
}

TEST(SchemaPerCallFields, PerCallOverridesSchemaStatic) {
    // Both schema-static (request.extra_fields) and per-call bind the
    // same path. Per-call wins — caller is overriding the default for
    // THIS call.
    auto schema = base_schema();
    schema["request"]["extra_fields"] = json{
        {"reasoning", {{"effort", "medium"}}},   // static default
    };
    schema["request"]["per_call_fields"] = json::array({"reasoning.effort"});
    auto sp = make_provider_from(schema);
    ASSERT_NE(sp, nullptr);

    CompletionParams p = basic_params();
    p.extra_fields = json{{"reasoning.effort", "high"}};

    json body = SchemaProviderTestAccess::build_body(*sp, p);
    EXPECT_EQ(body["reasoning"]["effort"].get<std::string>(), "high");
}

TEST(SchemaPerCallFields, OmittedPerCallFieldsNoOp) {
    // Caller passes extra_fields but schema didn't declare any per_call_fields
    // → all caller keys silently dropped (back-compat for schemas that
    // predate the feature).
    auto schema = base_schema();
    // No per_call_fields key in schema.
    auto sp = make_provider_from(schema);
    ASSERT_NE(sp, nullptr);

    CompletionParams p = basic_params();
    p.extra_fields = json{{"reasoning.effort", "high"}};

    json body = SchemaProviderTestAccess::build_body(*sp, p);
    EXPECT_FALSE(body.contains("reasoning"));
}

TEST(SchemaPerCallFields, EmptyExtraFieldsNoOpEvenWithDeclaration) {
    // Schema declares per_call_fields but caller doesn't bind anything
    // → no body change.
    auto schema = base_schema();
    schema["request"]["per_call_fields"] = json::array({"reasoning.effort"});
    auto sp = make_provider_from(schema);
    ASSERT_NE(sp, nullptr);

    CompletionParams p = basic_params();
    // p.extra_fields stays default-constructed (empty json).

    json body = SchemaProviderTestAccess::build_body(*sp, p);
    EXPECT_FALSE(body.contains("reasoning"));
}

TEST(SchemaPerCallFields, NestedPathBindsCorrectly) {
    // Sanity — per-call binding uses the same json_path::set_path
    // machinery, so deep paths (4+ segments) just work.
    auto schema = base_schema();
    schema["request"]["per_call_fields"] = json::array({"a.b.c.d"});
    auto sp = make_provider_from(schema);
    ASSERT_NE(sp, nullptr);

    CompletionParams p = basic_params();
    p.extra_fields = json{{"a.b.c.d", "deep"}};

    json body = SchemaProviderTestAccess::build_body(*sp, p);
    ASSERT_TRUE(body.contains("a")) << body.dump();
    EXPECT_EQ(body["a"]["b"]["c"]["d"].get<std::string>(), "deep");
}

TEST(SchemaProviderRouting, ConfigDefaultAndPerCallOverride) {
    auto schema = base_schema();
    schema["request"]["per_call_fields"] = json::array({"provider"});
    const json configured = {
        {"zdr", true},
        {"only", json::array({"morph"})},
        {"allow_fallbacks", false},
    };
    auto sp = make_provider_from(schema, nullptr, configured);
    ASSERT_NE(sp, nullptr);

    auto body = SchemaProviderTestAccess::build_body(*sp, basic_params());
    EXPECT_EQ(body.at("provider"), configured);

    CompletionParams params = basic_params();
    params.extra_fields = {
        {"provider", {{"only", json::array({"deepseek"})}}},
    };
    body = SchemaProviderTestAccess::build_body(*sp, params);
    const json expected_provider = {{"only", json::array({"deepseek"})}};
    EXPECT_EQ(body.at("provider"), expected_provider);
}

TEST(SchemaProviderRouting, RejectsNonObjectConfiguration) {
    auto sp = make_provider_from(base_schema(), nullptr, "morph");
    ASSERT_NE(sp, nullptr);
    EXPECT_THROW(
        (void)SchemaProviderTestAccess::build_body(*sp, basic_params()),
        std::invalid_argument);
}

// Built-in schemas must let callers cap reasoning per call. Without this a
// reasoning model can spend its whole output budget thinking (observed with
// OpenRouter deepseek: empty replies, finish_reason=length) and callers had no
// way to bound it through SchemaProvider.
TEST(SchemaBuiltinReasoningKnob, ResponsesSchemaBindsReasoningEffortPerCall) {
    SchemaProvider::Config cfg;
    cfg.schema_path = "openai_responses";
    cfg.api_key     = "test-key";
    auto sp         = SchemaProvider::create(cfg);
    ASSERT_NE(sp, nullptr);

    CompletionParams p = basic_params();
    p.extra_fields     = json{{"reasoning.effort", "low"}};
    const json body    = SchemaProviderTestAccess::build_body(*sp, p);
    ASSERT_TRUE(body.contains("reasoning")) << body.dump();
    EXPECT_EQ(body["reasoning"]["effort"], "low");
}

TEST(SchemaBuiltinReasoningKnob, ChatSchemaBindsReasoningEffortPerCall) {
    SchemaProvider::Config cfg;
    cfg.schema_path = "openai";
    cfg.api_key     = "test-key";
    auto sp         = SchemaProvider::create(cfg);
    ASSERT_NE(sp, nullptr);

    CompletionParams p = basic_params();
    p.extra_fields     = json{{"reasoning_effort", "low"}};
    const json body    = SchemaProviderTestAccess::build_body(*sp, p);
    EXPECT_EQ(body.value("reasoning_effort", ""), "low") << body.dump();
}

TEST(SchemaBuiltinReasoningKnob, NoKnobMeansNoReasoningKeyInBody) {
    SchemaProvider::Config cfg;
    cfg.schema_path = "openai_responses";
    cfg.api_key     = "test-key";
    auto sp         = SchemaProvider::create(cfg);
    ASSERT_NE(sp, nullptr);
    const json body = SchemaProviderTestAccess::build_body(*sp, basic_params());
    EXPECT_FALSE(body.contains("reasoning")) << body.dump();
}



// ─── request.temperature_unsupported_models ───
//
// Some endpoints answer HTTP 400 when `temperature` is present. Which models
// do is vendor data, so the schema declares it. Expectations below were
// measured against the live APIs (see the changelog entry).

namespace {

bool body_has_temperature(const std::string& schema_name, const std::string& model) {
    SchemaProvider::Config cfg;
    cfg.schema_path = schema_name;
    cfg.api_key     = "test-key";
    auto sp         = SchemaProvider::create(cfg);
    EXPECT_NE(sp, nullptr);
    CompletionParams p = basic_params();
    p.model            = model;
    return SchemaProviderTestAccess::build_body(*sp, p).contains("temperature");
}

}  // namespace

TEST(SchemaTemperaturePolicy, ClaudeModelsThatRejectTemperatureOmitIt) {
    for (const char* model : {"claude-sonnet-5-5", "claude-opus-5-5", "claude-fable-5-1",
                              "claude-opus-5", "claude-sonnet-5", "claude-fable-5",
                              "claude-opus-4-8", "claude-opus-4-7"}) {
        EXPECT_FALSE(body_has_temperature("claude", model)) << model;
    }
}

TEST(SchemaTemperaturePolicy, ClaudeModelsThatAcceptTemperatureKeepIt) {
    for (const char* model : {"claude-haiku-4-5-20251001", "claude-haiku-4-5",
                              "claude-sonnet-4-6", "claude-opus-4-6",
                              "claude-opus-4-5-20251101", "claude-sonnet-4-5-20250929"}) {
        EXPECT_TRUE(body_has_temperature("claude", model)) << model;
    }
}

TEST(SchemaTemperaturePolicy, OpenAiReasoningFamiliesOmitTemperatureOnBothSchemas) {
    for (const char* schema : {"openai_responses", "openai"}) {
        for (const char* model : {"gpt-5", "gpt-5-mini", "gpt-5.5", "gpt-5.6-luna",
                                  "gpt-6-luna", "gpt-6.1-sol", "o1", "o3", "o3-mini",
                                  "o4-mini"}) {
            EXPECT_FALSE(body_has_temperature(schema, model)) << schema << " " << model;
        }
    }
}

TEST(SchemaTemperaturePolicy, OpenAiSamplingModelsKeepTemperature) {
    for (const char* schema : {"openai_responses", "openai"}) {
        for (const char* model : {"gpt-4o", "gpt-4o-mini", "gpt-4.1", "gpt-4.1-nano",
                                  "gpt-4-turbo", "gpt-3.5-turbo"}) {
            EXPECT_TRUE(body_has_temperature(schema, model)) << schema << " " << model;
        }
    }
}

TEST(SchemaTemperaturePolicy, GatewayPrefixedModelIdsMatchTheirFamily) {
    EXPECT_FALSE(body_has_temperature("openai", "openai/o4-mini"));
    EXPECT_FALSE(body_has_temperature("openai", "openai/gpt-6-luna"));
    EXPECT_FALSE(body_has_temperature("claude", "anthropic/claude-sonnet-5-5"));
    EXPECT_TRUE(body_has_temperature("openai", "openai/gpt-4o-mini"));
    EXPECT_TRUE(body_has_temperature("claude", "anthropic/claude-haiku-4-5"));
}

TEST(SchemaTemperaturePolicy, MatchingIsCaseInsensitive) {
    EXPECT_FALSE(body_has_temperature("openai_responses", "GPT-6-Luna"));
    EXPECT_FALSE(body_has_temperature("claude", "Claude-Sonnet-5-5"));
}

TEST(SchemaTemperaturePolicy, CustomSchemaListIsHonouredExactAndPrefix) {
    auto schema = base_schema();
    schema["request"]["temperature_unsupported_models"] =
        json::array({"exact-model", "family-*"});
    auto sp = make_provider_from(schema);
    ASSERT_NE(sp, nullptr);
    const auto has_temp = [&](const char* model) {
        CompletionParams p = basic_params();
        p.model            = model;
        return SchemaProviderTestAccess::build_body(*sp, p).contains("temperature");
    };
    EXPECT_FALSE(has_temp("exact-model"));
    EXPECT_FALSE(has_temp("family-large"));
    EXPECT_TRUE(has_temp("exact-model-2"));   // no '*': exact match only
    EXPECT_TRUE(has_temp("other"));
}

TEST(SchemaTemperaturePolicy, SchemaWithoutTheKeySendsTemperatureForEveryModel) {
    // The schema owns the contract: no C++ vendor fallback for schemas that
    // do not declare the list.
    auto sp = make_provider_from(base_schema());
    ASSERT_NE(sp, nullptr);
    CompletionParams p = basic_params();
    p.model            = "gpt-5-mini";
    EXPECT_TRUE(SchemaProviderTestAccess::build_body(*sp, p).contains("temperature"));
}

TEST(SchemaTemperaturePolicy, MalformedListIsRejectedAtCreation) {
    for (const json& bad : {json("gpt-5*"), json::array({1}), json::array({""})}) {
        auto schema = base_schema();
        schema["request"]["temperature_unsupported_models"] = bad;
        EXPECT_THROW((void)make_provider_from(schema), std::invalid_argument) << bad.dump();
    }
}

#endif // !_WIN32
