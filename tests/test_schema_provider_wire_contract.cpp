// Offline wire-contract coverage for the bundled JSON SchemaProvider schemas.
//
// These tests deliberately stop at request-body construction and response
// parsing. They make provider-specific ToolCalling and vision differences
// executable without spending API credits or depending on a live endpoint.

#include <gtest/gtest.h>

#include <neograph/llm/schema_provider.h>

#include <array>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

using neograph::ChatMessage;
using neograph::ChatTool;
using neograph::CompletionParams;
using neograph::json;
using neograph::llm::SchemaProvider;
using neograph::llm::test_access::SchemaProviderTestAccess;

namespace {

std::unique_ptr<SchemaProvider> provider_for(const std::string& schema) {
    SchemaProvider::Config config;
    config.schema_path = schema;
    config.api_key = "wire-contract-test-key";
    config.default_model = "wire-contract-test-model";
    config.base_url_override = "http://127.0.0.1:1";
    config.allow_insecure_loopback = true;
    return SchemaProvider::create(config);
}

ChatTool weather_tool() {
    ChatTool tool;
    tool.name = "lookup_weather";
    tool.description = "Look up the current weather for a city.";
    tool.parameters = {
        {"type", "object"},
        {"properties", {
            {"city", {{"type", "string"}}}
        }},
        {"required", {"city"}}
    };
    return tool;
}

CompletionParams params_with_tool() {
    CompletionParams params;
    params.model = "wire-contract-test-model";
    ChatMessage message;
    message.role = "user";
    message.content = "What is the weather in Seoul?";
    params.messages.push_back(std::move(message));
    params.tools.push_back(weather_tool());
    return params;
}

CompletionParams params_with_image(const std::string& image_url) {
    CompletionParams params;
    params.model = "wire-contract-test-model";
    ChatMessage message;
    message.role = "user";
    message.content = "Describe this image.";
    message.image_urls.push_back(image_url);
    params.messages.push_back(std::move(message));
    return params;
}

json openai_tool_response() {
    json response;
    response["choices"] = json::array();
    json choice;
    choice["message"]["role"] = "assistant";
    choice["message"]["content"] = nullptr;
    choice["message"]["tool_calls"] = json::array();
    json tool_call;
    tool_call["id"] = "call_1";
    tool_call["type"] = "function";
    tool_call["function"]["name"] = "lookup_weather";
    tool_call["function"]["arguments"] = R"({"city":"Seoul"})";
    choice["message"]["tool_calls"].push_back(std::move(tool_call));
    choice["finish_reason"] = "tool_calls";
    response["choices"].push_back(std::move(choice));
    return response;
}

json claude_tool_response() {
    json response;
    response["role"] = "assistant";
    response["content"] = json::array();
    response["content"].push_back(
        {{"type", "text"}, {"text", "I will check that."}});
    json tool_use;
    tool_use["type"] = "tool_use";
    tool_use["id"] = "toolu_1";
    tool_use["name"] = "lookup_weather";
    tool_use["input"]["city"] = "Seoul";
    response["content"].push_back(std::move(tool_use));
    response["stop_reason"] = "tool_use";
    return response;
}

json gemini_tool_response() {
    json response;
    response["candidates"] = json::array();
    json candidate;
    candidate["content"]["parts"] = json::array();
    candidate["content"]["parts"].push_back(
        {{"text", "I will check that."}});
    json function_call;
    function_call["name"] = "lookup_weather";
    function_call["args"]["city"] = "Seoul";
    candidate["content"]["parts"].push_back(
        {{"functionCall", std::move(function_call)}});
    candidate["finishReason"] = "STOP";
    response["candidates"].push_back(std::move(candidate));
    return response;
}

}  // namespace

TEST(SchemaProviderWireContract, ToolDefinitionsMatchBundledProviderSchemas) {
    const std::array<std::string, 3> schemas = {"openai", "claude", "gemini"};

    for (const auto& schema : schemas) {
        SCOPED_TRACE(schema);
        auto provider = provider_for(schema);
        ASSERT_NE(provider, nullptr);

        const json body = SchemaProviderTestAccess::build_body(
            *provider, params_with_tool());
        ASSERT_TRUE(body.contains("tools")) << body.dump();
        ASSERT_EQ(body["tools"].size(), 1U) << body.dump();

        const auto& tool = body["tools"][0];
        if (schema == "openai") {
            EXPECT_EQ(tool.at("type"), "function");
            EXPECT_EQ(tool.at("function").at("name"), "lookup_weather");
            EXPECT_EQ(tool.at("function").at("description"),
                      "Look up the current weather for a city.");
            EXPECT_EQ(tool.at("function").at("parameters").at("type"),
                      "object");
        } else if (schema == "claude") {
            EXPECT_EQ(tool.at("name"), "lookup_weather");
            EXPECT_EQ(tool.at("description"),
                      "Look up the current weather for a city.");
            EXPECT_EQ(tool.at("input_schema").at("type"), "object");
        } else {
            ASSERT_TRUE(tool.contains("function_declarations"));
            ASSERT_EQ(tool["function_declarations"].size(), 1U);
            const auto& declaration = tool["function_declarations"][0];
            EXPECT_EQ(declaration.at("name"), "lookup_weather");
            EXPECT_EQ(declaration.at("description"),
                      "Look up the current weather for a city.");
            EXPECT_EQ(declaration.at("parameters").at("type"), "object");
        }
    }
}

TEST(SchemaProviderWireContract, ToolResponsesNormalizeAcrossProviders) {
    struct Case {
        const char* schema;
        json (*response)();
    };
    const std::array<Case, 3> cases = {{
        {"openai", &openai_tool_response},
        {"claude", &claude_tool_response},
        {"gemini", &gemini_tool_response},
    }};

    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.schema);
        auto provider = provider_for(test_case.schema);
        ASSERT_NE(provider, nullptr);

        const ChatMessage message = SchemaProviderTestAccess::parse_response(
            *provider, test_case.response());
        ASSERT_EQ(message.tool_calls.size(), 1U);
        EXPECT_EQ(message.tool_calls[0].name, "lookup_weather");
        ASSERT_FALSE(message.tool_calls[0].arguments.empty());
        EXPECT_EQ(json::parse(message.tool_calls[0].arguments).at("city"),
                  "Seoul");

        if (std::string(test_case.schema) == "openai") {
            EXPECT_EQ(message.tool_calls[0].id, "call_1");
        } else if (std::string(test_case.schema) == "claude") {
            EXPECT_EQ(message.tool_calls[0].id, "toolu_1");
        } else {
            EXPECT_FALSE(message.tool_calls[0].id.empty());
        }
    }
}

TEST(SchemaProviderWireContract, ChoicesMessagePreservesGlmReasoningWithToolCall) {
    auto provider = provider_for("openai");
    const json response = {
        {"choices", json::array({{
            {"message", {
                {"role", "assistant"},
                {"content", nullptr},
                {"reasoning_content", "Need the approved lookup."},
                {"tool_calls", json::array({{
                    {"id", "call_glm_1"},
                    {"type", "function"},
                    {"function", {{"name", "lookup_weather"},
                                  {"arguments", R"({"city":"Seoul"})"}}}
                }})}
            }},
            {"finish_reason", "tool_calls"}
        }})}
    };
    const auto message = SchemaProviderTestAccess::parse_response(*provider, response);
    EXPECT_EQ(message.role, "assistant");
    EXPECT_TRUE(message.content.empty());
    EXPECT_EQ(message.reasoning, "Need the approved lookup.");
    ASSERT_EQ(message.tool_calls.size(), 1U);
    EXPECT_EQ(message.tool_calls[0].id, "call_glm_1");
    EXPECT_EQ(message.tool_calls[0].name, "lookup_weather");
    EXPECT_EQ(json::parse(message.tool_calls[0].arguments).at("city"), "Seoul");
}

TEST(SchemaProviderWireContract, ReasoningFieldsFollowExternalSchema) {
    const auto path = std::filesystem::path(__FILE__).parent_path() /
                      "fixtures" / "schema_reasoning_alias.json";
    auto provider = provider_for(path.string());
    const json response = {
        {"choices", json::array({{
            {"message", {{"role", "assistant"},
                         {"content", "done"},
                         {"reasoning", "legacy value"},
                         {"reasoning_content", "legacy alias"},
                         {"private_thought", "schema-selected value"}}}
        }})}
    };
    const auto message = SchemaProviderTestAccess::parse_response(*provider, response);
    EXPECT_EQ(message.content, "done");
    EXPECT_EQ(message.reasoning, "schema-selected value");
}

TEST(SchemaProviderWireContract, DataUrlVisionUsesEachProviderWireShape) {
    const std::array<std::string, 3> schemas = {"openai", "claude", "gemini"};
    const std::string image_url = "data:image/png;base64,AA==";

    for (const auto& schema : schemas) {
        SCOPED_TRACE(schema);
        auto provider = provider_for(schema);
        ASSERT_NE(provider, nullptr);

        const json body = SchemaProviderTestAccess::build_body(
            *provider, params_with_image(image_url));
        const auto& messages = schema == "gemini"
            ? body.at("contents")
            : body.at("messages");
        const auto& parts = messages.at(0).at(
            schema == "gemini" ? "parts" : "content");
        ASSERT_EQ(parts.size(), 2U) << body.dump();

        if (schema == "openai") {
            EXPECT_EQ(parts[1].at("type"), "image_url");
            EXPECT_EQ(parts[1].at("image_url").at("url"), image_url);
        } else if (schema == "claude") {
            EXPECT_EQ(parts[1].at("type"), "image");
            EXPECT_EQ(parts[1].at("source").at("type"), "base64");
            EXPECT_EQ(parts[1].at("source").at("media_type"), "image/png");
            EXPECT_EQ(parts[1].at("source").at("data"), "AA==");
        } else {
            EXPECT_EQ(parts[1].at("inline_data").at("mime_type"), "image/png");
            EXPECT_EQ(parts[1].at("inline_data").at("data"), "AA==");
        }
    }
}

TEST(SchemaProviderWireContract, RemoteVisionUrlUsesDeclaredRepresentation) {
    const std::string remote_url = "https://example.invalid/image.png";

    {
        auto provider = provider_for("openai");
        ASSERT_NE(provider, nullptr);
        const json body = SchemaProviderTestAccess::build_body(
            *provider, params_with_image(remote_url));
        EXPECT_EQ(body.at("messages").at(0).at("content").at(1)
                      .at("image_url").at("url"),
                  remote_url);
    }

    {
        auto provider = provider_for("claude");
        ASSERT_NE(provider, nullptr);
        const json body = SchemaProviderTestAccess::build_body(
            *provider, params_with_image(remote_url));
        const auto& source = body.at("messages").at(0).at("content").at(1)
                                 .at("source");
        EXPECT_EQ(source.at("type"), "url");
        EXPECT_EQ(source.at("url"), remote_url);
    }

    {
        auto provider = provider_for("gemini");
        ASSERT_NE(provider, nullptr);
        EXPECT_THROW(
            SchemaProviderTestAccess::build_body(
                *provider, params_with_image(remote_url)),
            std::invalid_argument);
    }
}
