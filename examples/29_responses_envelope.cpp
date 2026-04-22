// NeoGraph Example 29: /v1/responses envelope dump (debug aid)
//
// Single-purpose: ask /v1/responses a question with one tool registered,
// dump the raw JSON envelope, and break out the output[] item types so
// you can see at a glance what a tool-calling response actually looks
// like before SchemaProvider flattens it into ChatCompletion.
//
// Useful for:
//   - debugging schema_provider parsing regressions
//   - learning the Responses API shape (function_call vs message vs
//     web_search_call vs reasoning items)
//   - confirming a model variant returns the items you expect
//
// Bypasses SchemaProvider on purpose — this is a wire-level peek.
//
// Usage:
//   echo 'OPENAI_API_KEY=sk-...' > .env
//   ./example_responses_envelope
//   ./example_responses_envelope "What's the weather in Tokyo?"
//   MODEL=gpt-5.4-mini ./example_responses_envelope "..."

#include <neograph/neograph.h>
#include <neograph/async/http_client.h>
#include <neograph/async/endpoint.h>
#include <neograph/async/run_sync.h>

#include <cppdotenv/dotenv.hpp>

#include <asio/this_coro.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace neograph;

static asio::awaitable<async::HttpResponse>
post_responses(async::AsyncEndpoint endpoint,
               std::string body,
               std::string auth_value) {
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Authorization", std::move(auth_value)},
        {"Content-Type",  "application/json"},
    };
    async::RequestOptions opts;
    opts.timeout = std::chrono::seconds(60);

    auto ex = co_await asio::this_coro::executor;
    co_return co_await async::async_post(
        ex,
        endpoint.host,
        endpoint.port,
        endpoint.prefix + "/v1/responses",
        std::move(body),
        std::move(headers),
        endpoint.tls,
        opts);
}

int main(int argc, char** argv) {
    cppdotenv::auto_load_dotenv();

    try {
        const char* api_key = std::getenv("OPENAI_API_KEY");
        if (!api_key) {
            std::cerr << "Set OPENAI_API_KEY environment variable "
                         "(or put it in .env beside the binary)\n";
            return 1;
        }
        const char* model_env = std::getenv("MODEL");
        std::string model = model_env ? model_env : "gpt-5.4-mini";

        std::string question = (argc >= 2)
            ? argv[1]
            : "What is the weather right now in Tokyo? Use the get_weather "
              "tool if you need a current value.";

        // One function tool so the model has a non-trivial choice between
        // {"answer directly"} and {"call the tool"}. Hand-shaped to match
        // OpenAI Responses' flat-function tool definition.
        json tools = json::array({
            json{
                {"type",        "function"},
                {"name",        "get_weather"},
                {"description", "Look up the current weather for a city."},
                {"parameters",  {
                    {"type",       "object"},
                    {"properties", {
                        {"city", {{"type", "string"},
                                  {"description", "City name"}}},
                        {"unit", {{"type", "string"},
                                  {"enum", json::array({"C", "F"})}}}
                    }},
                    {"required",   json::array({"city"})}
                }}
            }
        });

        json body;
        body["model"] = model;
        body["input"] = question;
        body["tools"] = tools;

        auto endpoint = async::split_async_endpoint("https://api.openai.com");
        auto resp = async::run_sync(post_responses(
            endpoint, body.dump(), "Bearer " + std::string(api_key)));

        if (resp.status != 200) {
            std::cerr << "HTTP " << resp.status << ": "
                      << resp.body.substr(0, 1000) << "\n";
            return 2;
        }

        auto envelope = json::parse(resp.body);

        std::cout << "=== model    : " << model << "\n"
                  << "=== question : " << question << "\n\n";

        std::cout << "─── Item-type summary ─────────────────────────────────\n";
        if (envelope.contains("output") && envelope["output"].is_array()) {
            int idx = 0;
            for (const auto& item : envelope["output"]) {
                std::string type = item.value("type", "?");
                std::cout << "  [" << idx++ << "] " << type;
                if (type == "function_call") {
                    std::cout << "  name=" << item.value("name", "?")
                              << "  args=" << item.value("arguments", "");
                } else if (type == "message") {
                    int parts = item.contains("content") &&
                                item["content"].is_array()
                        ? static_cast<int>(item["content"].size()) : 0;
                    std::cout << "  parts=" << parts;
                }
                std::cout << "\n";
            }
        } else {
            std::cout << "  (no output[] array)\n";
        }
        std::cout << "\n";

        std::cout << "─── Raw envelope ──────────────────────────────────────\n"
                  << envelope.dump(2) << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    }
}
