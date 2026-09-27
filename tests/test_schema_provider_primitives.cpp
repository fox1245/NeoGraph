#include <gtest/gtest.h>

#include <neograph/llm/schema_provider.h>
#include <neograph/llm/schema_primitive_registry.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <future>

namespace {

std::filesystem::path write_schema(const std::string& suffix,
                                   const std::string& transport = "synthetic",
                                   const std::string& mode = "synthetic_mode",
                                   const std::string& parser = "typed") {
    const auto path = std::filesystem::temp_directory_path() /
        ("neograph_schema_primitives_" + suffix + ".json");
    std::ofstream out(path);
    out << R"({
      "name":"synthetic",
      "connection":{"base_url":"https://synthetic.invalid","endpoint":"/complete","transport":")"
        << transport << R"("},
      "execution":{"mode":")" << mode << R"("},
      "request":{"model_field":"model","messages_field":"messages","stream_field":"stream"},
      "system_prompt":{"strategy":"in_messages"},
      "messages":{"role_field":"role","content_field":"content"},
      "tool_definition":{"wrapper":"function"},
      "tool_call_in_message":{"strategy":"tool_calls_array"},
      "tool_result":{"strategy":"flat"},
      "response":{"strategy":"choices_message","artifact_parser":")" << parser << R"("},
      "streaming":{"format":"sse_data"}
    })";
    return path;
}

} // namespace

TEST(SchemaPrimitiveRegistryTest, DuplicateAndReplacementAreDeterministic) {
    neograph::llm::SchemaPrimitiveRegistry registry;
    auto factory = [](neograph::llm::SchemaPrimitiveRequestContext)
        -> asio::awaitable<neograph::async::HttpResponse> {
        co_return neograph::async::HttpResponse{200, R"({})"};
    };
    registry.register_transport("local", factory);
    EXPECT_THROW(registry.register_transport("local", factory), std::invalid_argument);
    EXPECT_NO_THROW(registry.replace_transport("local", factory));
    EXPECT_TRUE(registry.contains(neograph::llm::SchemaPrimitiveCategory::Transport, "local"));
}

TEST(SchemaPrimitiveRegistryTest, ResolvesCustomTransportExecutionAndParserAtCreation) {
    auto registry = std::make_shared<neograph::llm::SchemaPrimitiveRegistry>();
    registry->register_transport(
        "synthetic", [](neograph::llm::SchemaPrimitiveRequestContext request)
            -> asio::awaitable<neograph::async::HttpResponse> {
            EXPECT_EQ(request.path, "/complete");
            EXPECT_EQ(request.trace_metadata.at("test.trace"), "present");
            neograph::async::HttpResponse response;
            response.status = 200;
            response.body = R"({"choices":[{"message":{"role":"assistant","content":"ok"}}]})";
            co_return response;
        });
    registry->register_execution_mode(
        "synthetic_mode", [](neograph::llm::SchemaExecutionContext context)
            -> asio::awaitable<neograph::json> {
            auto response = co_await context.transport(std::move(context.request));
            co_return neograph::json::parse(response.body);
        });
    registry->register_artifact_parser(
        "typed", [](const neograph::json& response,
                    const neograph::llm::SchemaPrimitiveRequestContext& request)
            -> std::vector<neograph::GeneratedArtifact> {
            EXPECT_EQ(request.trace_metadata.at("test.trace"), "present");
            EXPECT_EQ(response.at("choices").size(), 1u);
            neograph::GeneratedArtifact artifact;
            artifact.kind = "synthetic";
            artifact.mime_type = "application/x-neograph";
            artifact.file_id = "typed-result";
            return {std::move(artifact)};
        });

    const auto path = write_schema("end_to_end");
    neograph::llm::SchemaProvider::Config config;
    config.schema_path = path.string();
    config.primitive_registry = registry;
    config.trace_metadata.emplace("test.trace", "present");
    auto provider = neograph::llm::SchemaProvider::create(config);

    neograph::CompletionParams params;
    params.model = "synthetic-model";
    params.messages.push_back({"user", "hello"});
    auto first = std::async(std::launch::async, [&] { return provider->complete(params); });
    auto second = std::async(std::launch::async, [&] { return provider->complete(params); });
    const auto completion = first.get();
    const auto concurrent = second.get();
    EXPECT_EQ(completion.message.content, "ok");
    EXPECT_EQ(concurrent.message.content, "ok");
    ASSERT_EQ(concurrent.artifacts.size(), 1u);
    ASSERT_EQ(completion.artifacts.size(), 1u);
    EXPECT_EQ(completion.artifacts.front().file_id, "typed-result");
    std::filesystem::remove(path);
}

TEST(SchemaPrimitiveRegistryTest, UnknownPrimitiveReportsPathCategoryAndName) {
    const auto path = write_schema("unknown", "missing_transport");
    neograph::llm::SchemaProvider::Config config;
    config.schema_path = path.string();
    try {
        (void)neograph::llm::SchemaProvider::create(config);
        FAIL() << "expected provider creation to fail";
    } catch (const std::invalid_argument& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("connection.transport"), std::string::npos);
        EXPECT_NE(message.find("transport"), std::string::npos);
        EXPECT_NE(message.find("missing_transport"), std::string::npos);
    }
    std::filesystem::remove(path);
}
