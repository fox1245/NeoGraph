// Contract coverage for SchemaProvider's schema-owned arbitrary JSON endpoint
// path. The fixture mirrors OpenRouter's alpha Decisions route without making
// a network call to the public service.

#include <gtest/gtest.h>

#include <neograph/llm/schema_provider.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

using namespace neograph;

namespace {

struct DecisionsMock {
    httplib::Server server;
    std::thread thread;
    int port = 0;
    std::atomic<int> request_count{0};
    mutable std::mutex mutex;
    std::string path;
    std::string authorization;
    std::string request_body;

    DecisionsMock() {
        server.Post("/api/alpha/decisions",
                    [this](const httplib::Request& request,
                           httplib::Response& response) {
                        {
                            std::lock_guard lock(mutex);
                            path = request.path;
                            authorization = request.get_header_value("Authorization");
                            request_body = request.body;
                        }
                        request_count.fetch_add(1, std::memory_order_release);
                        response.set_content(
                            R"({
                                "answers": [{
                                    "question_id": "topology",
                                    "answer": "expand",
                                    "probability": 0.91,
                                    "confidence": 0.88
                                }],
                                "state": {"version": 1}
                            })",
                            "application/json");
                    });

        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i < 200 && !server.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    ~DecisionsMock() {
        server.stop();
        if (thread.joinable()) thread.join();
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port);
    }

    json last_request() const {
        std::lock_guard lock(mutex);
        return json::parse(request_body);
    }
};

}  // namespace

TEST(SchemaProviderJsonEndpoint, UsesSchemaEndpointAuthAndReturnsRawJson) {
    DecisionsMock mock;
    ASSERT_GT(mock.port, 0);

    llm::SchemaProvider::Config config;
    config.schema_path = "openrouter_decisions";
    config.api_key = "test-key";
    config.default_model = "~typesafe/jev-latest";
    config.base_url_override = mock.base_url();
    config.allow_insecure_loopback = true;
    config.timeout_seconds = 5;

    auto provider = llm::SchemaProvider::create(config);
    const json request = {
        {"model", "~typesafe/jev-latest"},
        {"questions", json::array({
            {{"id", "topology"}, {"question", "Should the retriever branch expand?"}}
        })},
        {"state", {{"topology_version", 3}}}
    };

    const json response = provider->request_json(request);

    ASSERT_EQ(mock.request_count.load(std::memory_order_acquire), 1);
    EXPECT_EQ(mock.path, "/api/alpha/decisions");
    EXPECT_EQ(mock.authorization, "Bearer test-key");
    EXPECT_EQ(mock.last_request(), request);
    ASSERT_TRUE(response.contains("answers"));
    ASSERT_EQ(response.at("answers").size(), 1u);
    EXPECT_EQ(response.at("answers").at(0).at("answer"), "expand");
    EXPECT_EQ(response.at("state").at("version"), 1);
}
