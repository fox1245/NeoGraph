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
#include <fstream>
#include <string>
#include <unistd.h>

using neograph::CompletionParams;
using neograph::ChatMessage;
using neograph::json;
using neograph::llm::SchemaProvider;
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
            {"base_url", "http://localhost:9999"},
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

std::unique_ptr<SchemaProvider> make_provider_from(const json& schema) {
    auto path = write_temp_schema(schema);
    if (path.empty()) return nullptr;

    SchemaProvider::Config cfg;
    cfg.schema_path = path;
    cfg.api_key = "test-key";
    cfg.default_model = "gpt-test";

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

#endif // !_WIN32
