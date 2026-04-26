#include <neograph/llm/openai_provider.h>

#include <neograph/async/conn_pool.h>
#include <neograph/async/endpoint.h>
#include <neograph/async/http_client.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <asio/this_coro.hpp>

#include <charconv>
#include <iostream>
#include <stdexcept>
#include <sstream>

namespace neograph::llm {

// --- OpenAIProvider ---

OpenAIProvider::OpenAIProvider(Config config)
  : config_(std::move(config))
{
    // Long-lived HTTP loop + ConnPool. Same pattern as SchemaProvider
    // (commit 6da4810): Provider::complete()'s run_sync builds a
    // throw-away io_context per call, so the pool can't live there.
    http_io_ = std::make_unique<asio::io_context>();
    http_work_.emplace(asio::make_work_guard(*http_io_));
    http_thread_ = std::thread([io = http_io_.get()]{ io->run(); });
    conn_pool_ = std::make_unique<async::ConnPool>(http_io_->get_executor());
}

OpenAIProvider::~OpenAIProvider()
{
    if (http_work_) http_work_.reset();
    if (http_io_) http_io_->stop();
    if (http_thread_.joinable()) http_thread_.join();
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

// Parse base_url into host + path prefix (used by complete_stream which
// still drives httplib synchronously). The httplib::Client constructor
// is happy taking the scheme + authority verbatim, so we strip only the
// path portion off the end.
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

namespace {

// Parse Retry-After. Supports the seconds-integer shape only (the
// HTTP-date shape is rare in practice for LLM endpoints; matching
// SchemaProvider's behavior). Returns -1 when missing or unparsable.
int parse_retry_after_seconds(std::string_view raw) {
    if (raw.empty()) return -1;
    int seconds = 0;
    auto begin = raw.data();
    auto end = raw.data() + raw.size();
    auto [ptr, ec] = std::from_chars(begin, end, seconds);
    if (ec != std::errc{} || ptr != end || seconds < 0) return -1;
    return seconds;
}

} // namespace

asio::awaitable<ChatCompletion>
OpenAIProvider::complete_async(const CompletionParams& params)
{
    auto body_json = build_body(params);
    auto body_str = body_json.dump();
    auto endpoint = async::split_async_endpoint(config_.base_url);

    std::vector<std::pair<std::string, std::string>> headers = {
        {"Authorization", "Bearer " + config_.api_key},
        {"Content-Type",  "application/json"},
    };

    async::RequestOptions opts;
    if (config_.timeout_seconds > 0) {
        opts.timeout = std::chrono::seconds(config_.timeout_seconds);
    }

    auto res = co_await conn_pool_->async_post(
        endpoint.host,
        endpoint.port,
        endpoint.prefix + "/v1/chat/completions",
        body_str,
        std::move(headers),
        endpoint.tls,
        opts);

    if (res.status == 429) {
        throw RateLimitError(
            "API error (HTTP 429): " + res.body,
            parse_retry_after_seconds(res.retry_after));
    }

    if (res.status != 200) {
        throw std::runtime_error(
            "API error (HTTP " + std::to_string(res.status) + "): " + res.body);
    }

    auto resp_json = json::parse(res.body);
    auto choice = resp_json.at("choices").at(0);

    ChatCompletion completion;
    completion.message = parse_response_message(choice);

    if (resp_json.contains("usage")) {
        auto u = resp_json["usage"];
        completion.usage.prompt_tokens = u.value("prompt_tokens", 0);
        completion.usage.completion_tokens = u.value("completion_tokens", 0);
        completion.usage.total_tokens = u.value("total_tokens", 0);
    }

    co_return completion;
}

ChatCompletion
OpenAIProvider::complete_stream(const CompletionParams& params,
                                const StreamCallback& on_chunk)
{
    auto body = build_body(params);
    body["stream"] = true;
    // OpenAI omits usage from streamed responses unless this flag is set
    // — without it, completion.usage stays zero after streaming. Users
    // tracking token cost lose data silently. Safe for any OpenAI-
    // compatible backend: endpoints that don't understand the field
    // ignore it rather than rejecting the request.
    body["stream_options"] = {{"include_usage", true}};

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

                    // The final chunk (after include_usage=true) has
                    // usage populated but an empty choices array. Capture
                    // it before falling through to per-choice delta
                    // handling.
                    if (j.contains("usage") && !j["usage"].is_null()) {
                        auto u = j["usage"];
                        completion.usage.prompt_tokens =
                            u.value("prompt_tokens", 0);
                        completion.usage.completion_tokens =
                            u.value("completion_tokens", 0);
                        completion.usage.total_tokens =
                            u.value("total_tokens",
                                    completion.usage.prompt_tokens +
                                    completion.usage.completion_tokens);
                    }

                    if (!j.contains("choices") || !j["choices"].is_array()
                        || j["choices"].empty()) {
                        continue;
                    }
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
