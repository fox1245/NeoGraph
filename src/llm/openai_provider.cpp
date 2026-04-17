#include <neograph/llm/openai_provider.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <iostream>
#include <stdexcept>
#include <sstream>

namespace neograph::llm {

// --- OpenAIProvider ---

OpenAIProvider::OpenAIProvider(Config config)
  : config_(std::move(config))
{
}

std::unique_ptr<OpenAIProvider>
OpenAIProvider::create(const Config& config)
{
    return std::unique_ptr<OpenAIProvider>(new OpenAIProvider(config));
}

json
OpenAIProvider::build_body(const CompletionParams& params) const
{
    json body;
    body["model"] = params.model.empty() ? config_.default_model : params.model;
    body["messages"] = messages_to_json(params.messages);

    if (!params.tools.empty()) {
        body["tools"] = tools_to_json(params.tools);
        body["tool_choice"] = "auto";
    }

    if (params.temperature >= 0.0f) {
        body["temperature"] = params.temperature;
    }

    if (params.max_tokens > 0) {
        body["max_completion_tokens"] = params.max_tokens;
    }

    return body;
}

// Parse base_url into host + path prefix
static std::pair<std::string, std::string> parse_url(const std::string& base_url) {
    std::string host = base_url;
    std::string prefix;
    auto scheme_end = host.find("://");
    if (scheme_end != std::string::npos) {
        auto path_start = host.find('/', scheme_end + 3);
        if (path_start != std::string::npos) {
            prefix = host.substr(path_start);
            host = host.substr(0, path_start);
        }
    }
    return {host, prefix};
}

ChatCompletion
OpenAIProvider::complete(const CompletionParams& params)
{
    auto body = build_body(params);
    auto [host, prefix] = parse_url(config_.base_url);

    httplib::Client cli(host);
    cli.set_read_timeout(config_.timeout_seconds, 0);
    cli.set_connection_timeout(10, 0);

    httplib::Headers headers = {
        {"Authorization", "Bearer " + config_.api_key}
    };

    auto res = cli.Post(prefix + "/v1/chat/completions", headers, body.dump(), "application/json");

    if (!res) {
        throw std::runtime_error("HTTP request failed: " + httplib::to_string(res.error()));
    }

    if (res->status != 200) {
        throw std::runtime_error(
            "API error (HTTP " + std::to_string(res->status) + "): " + res->body);
    }

    auto resp_json = json::parse(res->body);
    auto choice = resp_json.at("choices").at(0);

    ChatCompletion completion;
    completion.message = parse_response_message(choice);

    if (resp_json.contains("usage")) {
        auto u = resp_json["usage"];
        completion.usage.prompt_tokens = u.value("prompt_tokens", 0);
        completion.usage.completion_tokens = u.value("completion_tokens", 0);
        completion.usage.total_tokens = u.value("total_tokens", 0);
    }

    return completion;
}

ChatCompletion
OpenAIProvider::complete_stream(const CompletionParams& params,
                                const StreamCallback& on_chunk)
{
    auto body = build_body(params);
    body["stream"] = true;

    auto [host, prefix] = parse_url(config_.base_url);

    httplib::Client cli(host);
    cli.set_read_timeout(config_.timeout_seconds, 0);
    cli.set_connection_timeout(10, 0);

    httplib::Headers headers = {
        {"Authorization", "Bearer " + config_.api_key}
    };

    // Accumulate the full response from streamed chunks
    ChatCompletion completion;
    completion.message.role = "assistant";
    std::string full_content;

    // tool_calls accumulator: id -> {name, arguments}
    std::map<int, ToolCall> tc_map;

    // Buffer for partial SSE lines across chunk boundaries
    std::string line_buffer;

    auto res = cli.Post(
        prefix + "/v1/chat/completions", headers, body.dump(), "application/json",
        [&](const char* data, size_t len) -> bool {
            line_buffer.append(data, len);

            // Process complete lines only
            size_t pos;
            while ((pos = line_buffer.find('\n')) != std::string::npos) {
                std::string line = line_buffer.substr(0, pos);
                line_buffer.erase(0, pos + 1);

                // Remove \r
                if (!line.empty() && line.back() == '\r') line.pop_back();

                if (line.rfind("data: ", 0) != 0) continue;
                std::string payload = line.substr(6);
                if (payload == "[DONE]") continue;

                try {
                    auto j = json::parse(payload);
                    auto delta = j["choices"][0]["delta"];

                    // Content token
                    if (delta.contains("content") && !delta["content"].is_null()) {
                        std::string token = delta["content"].get<std::string>();
                        full_content += token;
                        if (on_chunk) on_chunk(token);
                    }

                    // Tool calls (streamed incrementally)
                    if (delta.contains("tool_calls")) {
                        for (const auto& tc : delta["tool_calls"]) {
                            int idx = tc.value("index", 0);
                            if (tc.contains("id")) {
                                tc_map[idx].id = tc["id"].template get<std::string>();
                            }
                            if (tc.contains("function")) {
                                if (tc["function"].contains("name"))
                                    tc_map[idx].name += tc["function"]["name"].template get<std::string>();
                                if (tc["function"].contains("arguments"))
                                    tc_map[idx].arguments += tc["function"]["arguments"].template get<std::string>();
                            }
                        }
                    }
                } catch (...) {
                    // Skip malformed chunks
                }
            }
            return true; // continue receiving
        }
    );

    if (!res) {
        throw std::runtime_error("HTTP request failed: " + httplib::to_string(res.error()));
    }

    if (res->status != 200) {
        throw std::runtime_error(
            "API error (HTTP " + std::to_string(res->status) + "): " + res->body);
    }

    completion.message.content = full_content;
    for (auto& [idx, tc] : tc_map) {
        completion.message.tool_calls.push_back(tc);
    }

    return completion;
}

} // namespace neograph::llm
