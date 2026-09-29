#include <gtest/gtest.h>

#include <neograph/llm/schema_provider.h>
#include <neograph/llm/schema_primitive_registry.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/cancel.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#include <atomic>
#include <thread>

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
    const neograph::json schema = {
        {"name", "synthetic"},
        {"connection", {{"base_url", "https://synthetic.invalid"},
                        {"endpoint", "/complete"}, {"transport", transport}}},
        {"execution", {{"mode", mode}}},
        {"request", {{"model_field", "model"}, {"messages_field", "messages"},
                     {"stream_field", "stream"}}},
        {"system_prompt", {{"strategy", "in_messages"}}},
        {"messages", {{"role_field", "role"}, {"content_field", "content"}}},
        {"tool_definition", {{"wrapper", "function"}}},
        {"tool_call_in_message", {{"strategy", "tool_calls_array"}}},
        {"tool_result", {{"strategy", "flat"}}},
        {"response", {{"strategy", "choices_message"},
                      {"message_path", "choices.0.message"},
                      {"artifact_parser", parser}}},
        {"streaming", {{"format", "sse_data"}}}
    };
    out << schema.dump(2);
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

TEST(SchemaPrimitiveRegistryTest, CallbackInvocationHonorsSelectedPrimitivesWithoutNetwork) {
    httplib::Server server;
    std::atomic<int> network_calls{0};
    server.Post("/complete", [&](const httplib::Request&, httplib::Response& response) {
        ++network_calls;
        response.set_content("data: [DONE]\n\n", "text/event-stream");
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    std::thread worker([&] { server.listen_after_bind(); });
    struct StopServer {
        httplib::Server& server;
        std::thread& worker;
        ~StopServer() { server.stop(); worker.join(); }
    } stop{server, worker};
    for (int i = 0; i != 200 && !server.is_running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    for (bool custom_execution : {false, true}) {
        int transports = 0, executions = 0;
        auto registry = std::make_shared<neograph::llm::SchemaPrimitiveRegistry>();
        registry->register_transport("synthetic",
            [&](neograph::llm::SchemaPrimitiveRequestContext request)
                -> asio::awaitable<neograph::async::HttpResponse> {
                ++transports;
                EXPECT_FALSE(neograph::json::parse(request.body).value("stream", false));
                co_return neograph::async::HttpResponse{200,
                    R"({"choices":[{"message":{"role":"assistant","content":"custom"}}]})"};
            });
        registry->register_execution_mode("synthetic_mode",
            [&](neograph::llm::SchemaExecutionContext context)
                -> asio::awaitable<neograph::json> {
                ++executions;
                const auto response = co_await context.transport(std::move(context.request));
                co_return neograph::json::parse(response.body);
            });
        const auto path = write_schema(custom_execution ? "callback_execution" : "callback_transport",
            "synthetic", custom_execution ? "synthetic_mode" : "standard", "rules");
        neograph::llm::SchemaProvider::Config config;
        config.schema_path = path.string();
        config.primitive_registry = registry;
        config.base_url_override = "http://127.0.0.1:" + std::to_string(port);
        config.allow_insecure_loopback = true;
        config.timeout_seconds = 2;
        auto provider = neograph::llm::SchemaProvider::create(config);
        neograph::CompletionParams params;
        params.messages.push_back({"user", "hello"});
        std::vector<std::string> chunks;
        const auto result = neograph::async::run_sync(provider->invoke(params,
            [&](const std::string& chunk) { chunks.push_back(chunk); }));
        EXPECT_EQ(result.message.content, "custom");
        EXPECT_EQ(chunks, std::vector<std::string>{"custom"});
        EXPECT_EQ(transports, 1);
        EXPECT_EQ(executions, custom_execution ? 1 : 0);
        EXPECT_EQ(network_calls, 0);
        std::filesystem::remove(path);
    }
}

TEST(SchemaPrimitiveRegistryTest, StreamRunsCustomArtifactParserAtTerminalBoundary) {
    httplib::Server server;
    server.Post("/complete", [](const httplib::Request&, httplib::Response& response) {
        const std::string events =
            "event: response.output_text.delta\ndata: {\"delta\":\"ready\"}\n\n"
            "event: response.completed\ndata: {\"response\":{\"id\":\"custom-image\",\"output\":[]}}\n\n";
        response.set_chunked_content_provider("text/event-stream",
            [events](size_t, httplib::DataSink& sink) {
                sink.write(events.data(), events.size());
                sink.done();
                return true;
            });
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    std::thread worker([&] { server.listen_after_bind(); });
    struct StopServer {
        httplib::Server& server;
        std::thread& worker;
        ~StopServer() { server.stop(); worker.join(); }
    } stop{server, worker};
    for (int i = 0; i != 200 && !server.is_running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto path = write_schema("stream_parser", "http", "standard", "typed");
    neograph::json schema;
    { std::ifstream input(path); schema = neograph::json::parse(input); }
    schema["streaming"] = {
        {"format", "sse_events"},
        {"events", {{"response.output_text.delta", {{"action", "text_delta"}, {"text_path", "delta"}}},
                    {"response.completed", {{"action", "done"}}}}}};
    { std::ofstream output(path); output << schema.dump(); }
    auto registry = std::make_shared<neograph::llm::SchemaPrimitiveRegistry>();
    int parses = 0;
    registry->register_artifact_parser("typed",
        [&](const neograph::json& response,
            const neograph::llm::SchemaPrimitiveRequestContext& request) {
            ++parses;
            EXPECT_EQ(request.path, "/complete");
            EXPECT_EQ(request.trace_metadata.at("test.trace"), "stream");
            EXPECT_EQ(neograph::json::parse(request.body).at("stream"), true);
            EXPECT_NE(request.cancellation, nullptr);
            EXPECT_GT(request.deadline, std::chrono::steady_clock::now());
            neograph::GeneratedArtifact artifact;
            artifact.kind = "image";
            artifact.file_id = response.at("id").get<std::string>();
            return std::vector<neograph::GeneratedArtifact>{std::move(artifact)};
        });
    neograph::llm::SchemaProvider::Config config;
    config.schema_path = path.string();
    config.primitive_registry = registry;
    config.trace_metadata.emplace("test.trace", "stream");
    config.base_url_override = "http://127.0.0.1:" + std::to_string(port);
    config.allow_insecure_loopback = true;
    config.timeout_seconds = 2;
    auto provider = neograph::llm::SchemaProvider::create(config);
    neograph::CompletionParams params;
    params.cancel_token = std::make_shared<neograph::graph::CancelToken>();
    params.messages.push_back({"user", "hello"});
    const auto result = provider->complete_stream(params, [](const std::string&) {});
    EXPECT_EQ(parses, 1);
    ASSERT_EQ(result.artifacts.size(), 1u);
    EXPECT_EQ(result.artifacts[0].file_id, "custom-image");
    std::filesystem::remove(path);
}
