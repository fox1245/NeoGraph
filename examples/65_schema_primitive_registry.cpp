// Synthetic, non-vendor SchemaProvider extension example.
#include <neograph/llm/schema_provider.h>
#include <neograph/llm/schema_primitive_registry.h>

#include <iostream>
#include <memory>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: example_schema_primitive_registry synthetic_schema.json\n";
        return 2;
    }

    auto registry = std::make_shared<neograph::llm::SchemaPrimitiveRegistry>();
    registry->register_transport(
        "synthetic_echo", [](neograph::llm::SchemaPrimitiveRequestContext request)
            -> asio::awaitable<neograph::async::HttpResponse> {
            neograph::async::HttpResponse response;
            response.status = 200;
            response.body = R"({"choices":[{"message":{"role":"assistant",
                "content":"typed synthetic response"}}]})";
            co_return response;
        });
    registry->register_execution_mode(
        "synthetic_execution", [](neograph::llm::SchemaExecutionContext context)
            -> asio::awaitable<neograph::json> {
            auto response = co_await context.transport(std::move(context.request));
            co_return neograph::json::parse(response.body);
        });
    registry->register_artifact_parser(
        "synthetic_artifacts", [](const neograph::json&, const neograph::llm::SchemaPrimitiveRequestContext&)
            -> std::vector<neograph::GeneratedArtifact> {
            neograph::GeneratedArtifact artifact;
            artifact.kind = "synthetic";
            artifact.mime_type = "application/x-neograph";
            artifact.file_id = "typed-example";
            return {std::move(artifact)};
        });

    neograph::llm::SchemaProvider::Config config;
    config.schema_path = argv[1];
    config.primitive_registry = std::move(registry);
    auto provider = neograph::llm::SchemaProvider::create(config);

    neograph::CompletionParams params;
    params.model = "synthetic-model";
    params.messages.push_back({"user", "hello"});
    const auto result = provider->complete(params);
    std::cout << result.message.content << " artifacts=" << result.artifacts.size() << '\n';
    return 0;
}
