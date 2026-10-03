#include <neograph/llm/images_client.h>
#include <neograph/async/http_client.h>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/co_spawn.hpp>
#include <asio/post.hpp>
#include <asio/system_error.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace neograph::llm {
namespace {
using Kind = EndpointFailureKind;
using Clock = std::chrono::steady_clock;
EndpointFailure failure(Kind kind, const char* message, int status = 0, bool sent = false) {
    return {kind, message, status, {}, sent};
}
struct Invalid {};
void require(bool value) { if (!value) throw Invalid{}; }
void closed(const json& value, std::initializer_list<const char*> keys) {
    require(value.is_object() && value.size() == keys.size());
    for (const auto* key : keys) require(value.contains(key));
}
std::string string(const json& value) {
    require(value.is_string());
    return value.get<std::string>();
}
std::uint64_t integer(const json& value) {
    require(value.is_number_integer());
    if (!value.is_number_unsigned()) require(value.get<long long>() >= 0);
    return value.get<unsigned long long>();
}
std::optional<std::uint64_t> optional_integer(const json& object, const char* name) {
    if (!object.contains(name) || object[name].is_null()) return {};
    return integer(object[name]);
}
bool one_of(std::string_view value, std::initializer_list<std::string_view> allowed) {
    return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
}
std::size_t utf8_characters(std::string_view text) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size(); ++count) {
        const auto lead = static_cast<unsigned char>(text[i++]);
        if (lead < 0x80) continue;
        const unsigned continuation = lead >= 0xC2 && lead <= 0xDF ? 1 :
                                      lead >= 0xE0 && lead <= 0xEF ? 2 :
                                      lead >= 0xF0 && lead <= 0xF4 ? 3 : 0;
        require(continuation != 0 && continuation <= text.size() - i);
        const auto first = static_cast<unsigned char>(text[i]);
        require(!(lead == 0xE0 && first < 0xA0) && !(lead == 0xED && first >= 0xA0) &&
                !(lead == 0xF0 && first < 0x90) && !(lead == 0xF4 && first >= 0x90));
        for (unsigned j = 0; j < continuation; ++j)
            require((static_cast<unsigned char>(text[i++]) & 0xC0) == 0x80);
    }
    return count;
}
bool endpoint_atom(std::string_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '.' || c == '-';
    });
}
bool valid_uri(std::string_view uri) {
    if (!uri.starts_with("https://")) return false;
    const auto end = uri.find_first_of("/?#", 8);
    const auto authority = uri.substr(8, end == std::string_view::npos ? uri.size() - 8 : end - 8);
    return !authority.empty() && authority.find('@') == std::string_view::npos &&
           uri.find('#') == std::string_view::npos &&
           std::none_of(uri.begin(), uri.end(), [](unsigned char c) { return c <= 32 || c == 127; });
}
void controls_valid(const ImagesClient::Controls& controls, std::string_view profile) {
    if (profile == "nano_banana_2_lite") {
        const auto* c = std::get_if<ImagesClient::GeminiControls>(&controls);
        require(c && c->image_size == "1K" && c->max_output_tokens >= 1120 && c->max_output_tokens <= 4096);
        require(c->thinking == ImagesClient::Thinking::Minimal || c->thinking == ImagesClient::Thinking::High);
        require(one_of(c->aspect_ratio, {"1:1", "1:4", "4:1", "1:8", "8:1", "2:3", "3:2", "3:4", "4:3", "4:5", "5:4", "9:16", "16:9", "21:9"}));
        return;
    }
    const auto* c = std::get_if<ImagesClient::OpenAIControls>(&controls);
    require(c && c->n >= 1 && c->n <= 10);
    require(c->response_format == ImagesClient::ResponseFormat::Native ||
            c->response_format == ImagesClient::ResponseFormat::Base64 ||
            c->response_format == ImagesClient::ResponseFormat::Url);
    if (profile == "openai_gpt_image") {
        require(c->response_format == ImagesClient::ResponseFormat::Native);
        require(one_of(c->quality, {"auto", "low", "medium", "high"}));
        require(one_of(c->size, {"auto", "1024x1024", "1536x1024", "1024x1536"}));
    } else if (profile == "openai_dall_e_2") {
        require(c->response_format != ImagesClient::ResponseFormat::Native && c->quality == "standard");
        require(one_of(c->size, {"256x256", "512x512", "1024x1024"}));
    } else {
        require(profile == "openai_dall_e_3" && c->n == 1 && c->response_format != ImagesClient::ResponseFormat::Native);
        require(one_of(c->quality, {"standard", "hd"}));
        require(one_of(c->size, {"1024x1024", "1792x1024", "1024x1792"}));
    }
}
int sextet(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
std::vector<std::byte> decode(std::string_view encoded, std::size_t limit) {
    require(!encoded.empty() && encoded.size() % 4 == 0);
    const auto padding = encoded.ends_with("==") ? 2u : encoded.ends_with("=") ? 1u : 0u;
    const auto size = encoded.size() / 4 * 3 - padding;
    if (size > limit) throw Kind::ResourceLimit;
    std::vector<std::byte> bytes(size);
    std::size_t position = 0;
    for (std::size_t i = 0; i < encoded.size(); i += 4) {
        const int a = sextet(encoded[i]), b = sextet(encoded[i + 1]);
        const bool last = i + 4 == encoded.size();
        const int c = encoded[i + 2] == '=' && last ? 0 : sextet(encoded[i + 2]);
        const int d = encoded[i + 3] == '=' && last ? 0 : sextet(encoded[i + 3]);
        require(a >= 0 && b >= 0 && c >= 0 && d >= 0);
        require(encoded[i + 2] != '=' || (last && encoded[i + 3] == '='));
        if (last && padding == 2) require((b & 15) == 0);
        if (last && padding == 1) require((c & 3) == 0);
        const unsigned value = (a << 18) | (b << 12) | (c << 6) | d;
        if (position < size) bytes[position++] = static_cast<std::byte>(value >> 16);
        if (position < size) bytes[position++] = static_cast<std::byte>(value >> 8);
        if (position < size) bytes[position++] = static_cast<std::byte>(value);
    }
    return bytes;
}
std::string mime(const std::vector<std::byte>& bytes) {
    auto matches = [&](std::size_t offset, std::string_view signature) {
        if (offset > bytes.size() || signature.size() > bytes.size() - offset) return false;
        for (std::size_t i = 0; i < signature.size(); ++i)
            if (std::to_integer<unsigned char>(bytes[offset + i]) != static_cast<unsigned char>(signature[i])) return false;
        return true;
    };
    auto word = [&](std::size_t offset, bool little_endian) {
        require(offset <= bytes.size() && bytes.size() - offset >= 4);
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const auto index = little_endian ? 3 - i : i;
            value = (value << 8) | std::to_integer<unsigned char>(bytes[offset + index]);
        }
        return value;
    };
    if (matches(0, std::string_view("\x89PNG\r\n\x1a\n", 8))) {
        require(bytes.size() >= 45 && word(8, false) == 13 && matches(12, "IHDR"));
        require(word(16, false) != 0 && word(20, false) != 0);
        bool image_data = false, ended = false;
        std::size_t offset = 8;
        while (offset < bytes.size()) {
            require(bytes.size() - offset >= 12);
            const auto length = word(offset, false);
            require(length <= bytes.size() - offset - 12);
            if (matches(offset + 4, "IDAT") && length != 0) image_data = true;
            if (matches(offset + 4, "IEND")) {
                require(length == 0 && offset + 12 == bytes.size());
                ended = true;
            }
            offset += static_cast<std::size_t>(length) + 12;
        }
        require(image_data && ended);
        return "image/png";
    }
    if (matches(0, std::string_view("\xff\xd8\xff", 3))) {
        require(bytes.size() >= 5 && matches(bytes.size() - 2, std::string_view("\xff\xd9", 2)));
        return "image/jpeg";
    }
    if (matches(0, "RIFF") && matches(8, "WEBP")) {
        require(bytes.size() >= 20 && word(4, true) == bytes.size() - 8 &&
                (matches(12, "VP8 ") || matches(12, "VP8L") || matches(12, "VP8X")));
        return "image/webp";
    }
    throw Invalid{};
}
EndpointArtifact binary(std::string_view data, std::string declared_mime, json metadata, std::size_t limit) {
    auto bytes = decode(data, limit);
    auto actual_mime = mime(bytes);
    require(declared_mime.empty() || declared_mime == actual_mime);
    return {std::move(actual_mime), {}, {},
            std::make_shared<const std::vector<std::byte>>(std::move(bytes)),
            std::make_shared<const json>(std::move(metadata))};
}
std::optional<EndpointUsage> usage(const json& body, bool google) {
    const auto* field = google ? "usageMetadata" : "usage";
    if (!body.contains(field) || body[field].is_null()) return {};
    auto reported = body[field];
    require(reported.is_object());
    EndpointUsage result;
    result.input_tokens = optional_integer(reported, google ? "promptTokenCount" : "input_tokens");
    result.output_tokens = optional_integer(reported, google ? "candidatesTokenCount" : "output_tokens");
    result.total_tokens = optional_integer(reported, google ? "totalTokenCount" : "total_tokens");
    if (google) result.cached_input_tokens = optional_integer(reported, "cachedContentTokenCount");
    else if (reported.contains("input_tokens_details")) {
        require(reported["input_tokens_details"].is_object());
        result.cached_input_tokens = optional_integer(reported["input_tokens_details"], "cached_tokens");
    }
    result.reported = std::make_shared<const json>(std::move(reported));
    return result;
}
} // namespace

struct ImagesClient::State {
    std::string provider, profile, model, host, port, path;
    std::chrono::milliseconds timeout;
    std::size_t prompt_limit, artifact_limit, output_limit;
    async::RequestOptions options;
    Controls defaults;
};

std::variant<ImagesClient::Config, EndpointFailure> ImagesClient::Config::from_json(const json& document) {
    try {
        closed(document, {"version", "provider", "model_profile", "model", "endpoint", "limits", "defaults"});
        require(integer(document["version"]) == 1);
        auto state = std::make_shared<State>();
        state->provider = string(document["provider"]);
        state->profile = string(document["model_profile"]);
        state->model = string(document["model"]);
        if (state->provider == "gemini") {
            require(state->profile == "nano_banana_2_lite" && state->model == "gemini-3.1-flash-lite-image");
        } else {
            require(state->provider == "openai");
            if (state->profile == "openai_dall_e_2") require(state->model == "dall-e-2");
            else if (state->profile == "openai_dall_e_3") require(state->model == "dall-e-3");
            else require(state->profile == "openai_gpt_image" && one_of(state->model, {"gpt-image-1", "gpt-image-1-mini", "gpt-image-1.5"}));
        }
        const auto endpoint = document["endpoint"];
        closed(endpoint, {"host", "port", "path"});
        state->host = string(endpoint["host"]);
        state->port = string(endpoint["port"]);
        state->path = string(endpoint["path"]);
        require(endpoint_atom(state->host));
        require(!state->port.empty() && state->port.size() <= 5);
        unsigned port = 0;
        for (unsigned char c : state->port) { require(c >= '0' && c <= '9'); port = port * 10 + c - '0'; }
        require(port > 0 && port <= 65535);
        require(state->path == (state->provider == "gemini" ? "/v1beta/models/" + state->model + ":generateContent" : "/v1/images/generations"));
        const auto limits = document["limits"];
        closed(limits, {"timeout_ms", "max_prompt_bytes", "max_response_header_bytes", "max_response_body_bytes", "max_response_chunk_bytes", "max_artifact_bytes", "max_outputs"});
        auto bounded = [&](const char* name) {
            auto value = integer(limits[name]);
            require(value > 0 && value <= std::numeric_limits<std::uint32_t>::max());
            return static_cast<std::size_t>(value);
        };
        state->timeout = std::chrono::milliseconds(bounded("timeout_ms"));
        state->prompt_limit = bounded("max_prompt_bytes");
        state->artifact_limit = bounded("max_artifact_bytes");
        state->output_limit = bounded("max_outputs");
        state->options.max_response_header_bytes = bounded("max_response_header_bytes");
        state->options.max_response_body_bytes = bounded("max_response_body_bytes");
        state->options.max_response_chunk_bytes = bounded("max_response_chunk_bytes");
        state->options.allow_replay = false;
        state->options.max_redirects = 0;
        const auto defaults = document["defaults"];
        if (state->provider == "gemini") {
            closed(defaults, {"aspect_ratio", "image_size", "thinking", "max_output_tokens"});
            auto thinking = string(defaults["thinking"]);
            require(thinking == "minimal" || thinking == "high");
            auto tokens = integer(defaults["max_output_tokens"]);
            require(tokens <= 4096);
            state->defaults = GeminiControls{string(defaults["aspect_ratio"]), string(defaults["image_size"]), thinking == "minimal" ? Thinking::Minimal : Thinking::High, static_cast<std::uint32_t>(tokens)};
        } else {
            closed(defaults, {"size", "quality", "n", "response_format"});
            const auto format = string(defaults["response_format"]);
            require(one_of(format, {"native", "b64_json", "url"}));
            const auto n = integer(defaults["n"]);
            require(n <= 10);
            state->defaults = OpenAIControls{string(defaults["size"]), string(defaults["quality"]), static_cast<std::uint32_t>(n), format == "native" ? ResponseFormat::Native : format == "url" ? ResponseFormat::Url : ResponseFormat::Base64};
        }
        controls_valid(state->defaults, state->profile);
        return Config(std::move(state));
    } catch (...) {
        return failure(Kind::InvalidConfig, "Invalid images client configuration");
    }
}

ImagesClient::ImagesClient(Config config, std::string api_key)
    : state_(std::move(config.state_)), key_(std::make_shared<const std::string>(std::move(api_key))) {}
ImagesClient::Request ImagesClient::request(std::string prompt) const {
    return {std::move(prompt), state_->defaults, {}, {}};
}
asio::awaitable<ImagesClient::Result> ImagesClient::async_generate(Request request) const {
    return generate_owned(state_, key_, std::move(request));
}

asio::awaitable<ImagesClient::Result> ImagesClient::generate_owned(
    std::shared_ptr<const State> state, std::shared_ptr<const std::string> key, Request request) {
    const auto started = Clock::now();
    const auto deadline = request.deadline ? std::min(*request.deadline, started + state->timeout) : started + state->timeout;
    bool sent = false;
    try {
        if (request.cancel_token && request.cancel_token->is_cancelled())
            co_return failure(Kind::Cancelled, "Image generation cancelled");
        if (started >= deadline) co_return failure(Kind::DeadlineExceeded, "Image generation deadline exceeded");
        if (key->empty() || key->find_first_of("\r\n") != std::string::npos)
            co_return failure(Kind::InvalidConfig, "Invalid image credential");
        require(!request.prompt.empty() && request.prompt.size() <= state->prompt_limit);
        const auto prompt_characters = utf8_characters(request.prompt);
        controls_valid(request.controls, state->profile);
        const bool google = state->provider == "gemini";
        json body = json::object();
        if (google) {
            const auto& c = std::get<GeminiControls>(request.controls);
            body["contents"] = json::array({json{{"role", "user"}, {"parts", json::array({json{{"text", request.prompt}}})}}});
            body["generationConfig"] = json{{"responseModalities", json::array({"TEXT", "IMAGE"})},
                {"maxOutputTokens", c.max_output_tokens},
                {"imageConfig", json{{"aspectRatio", c.aspect_ratio}, {"imageSize", c.image_size}}},
                {"thinkingConfig", json{{"thinkingLevel", c.thinking == Thinking::Minimal ? "MINIMAL" : "HIGH"}, {"includeThoughts", false}}}};
        } else {
            const auto& c = std::get<OpenAIControls>(request.controls);
            require(prompt_characters <= (state->profile == "openai_dall_e_2" ? 1000u : state->profile == "openai_dall_e_3" ? 4000u : 32000u));
            require(c.n <= state->output_limit);
            body = json{{"model", state->model}, {"prompt", request.prompt}, {"size", c.size}, {"quality", c.quality}, {"n", c.n}};
            if (c.response_format != ResponseFormat::Native)
                body["response_format"] = c.response_format == ResponseFormat::Url ? "url" : "b64_json";
        }
        auto encoded = body.dump();
        auto ex = co_await asio::this_coro::executor;
        auto operation = request.cancel_token ? request.cancel_token->fork() : nullptr;
        auto operation_ex = operation ? operation->bind_executor(ex) : ex;
        graph::CancelExecutorLease binding(operation);
        if (operation) co_await asio::post(operation_ex, asio::use_awaitable);
        if (operation && operation->is_cancelled())
            co_return failure(Kind::Cancelled, "Image generation cancelled");
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining.count() <= 0) co_return failure(Kind::DeadlineExceeded, "Image generation deadline exceeded");
        auto options = state->options;
        options.timeout = remaining;
        std::vector<std::pair<std::string, std::string>> headers;
        headers.emplace_back("Content-Type", "application/json");
        headers.emplace_back(google ? "x-goog-api-key" : "Authorization", google ? *key : "Bearer " + *key);
        auto exchange = async::async_post(operation_ex, state->host, state->port, state->path, encoded, std::move(headers), true, options);
        sent = true;
        async::HttpResponse response;
        if (operation) response = co_await asio::co_spawn(operation_ex, std::move(exchange), asio::bind_cancellation_slot(operation->slot(), asio::use_awaitable));
        else response = co_await std::move(exchange);
        if (request.cancel_token && request.cancel_token->is_cancelled())
            co_return failure(Kind::Cancelled, "Image generation cancelled", 0, true);
        if (Clock::now() >= deadline) co_return failure(Kind::DeadlineExceeded, "Image generation deadline exceeded", 0, true);
        if (response.status < 200 || response.status >= 300)
            co_return failure(Kind::Provider, "Image provider rejected request", response.status, true);
        auto document = json::parse(response.body);
        require(document.is_object());
        if (document.contains("error")) co_return failure(Kind::Provider, "Image provider returned error", response.status, true);
        Generation result;
        result.model = state->model;
        result.usage = usage(document, google);
        auto append = [&](Output output) {
            if (result.outputs.size() >= state->output_limit) throw Kind::ResourceLimit;
            result.outputs.push_back(std::move(output));
        };
        if (google) {
            if (document.contains("responseId")) result.response_id = string(document["responseId"]);
            if (document.contains("modelVersion")) result.model = string(document["modelVersion"]);
            if (document.contains("promptFeedback") && document["promptFeedback"].contains("blockReason"))
                co_return failure(Kind::Provider, "Image provider blocked prompt", response.status, true);
            const auto candidates = document["candidates"];
            require(candidates.is_array() && candidates.size() == 1);
            const auto candidate = candidates[0];
            require(candidate.is_object());
            if (candidate.contains("finishReason") && string(candidate["finishReason"]) != "STOP")
                co_return failure(Kind::Provider, "Image provider did not complete generation", response.status, true);
            const auto parts = candidate["content"]["parts"];
            require(parts.is_array());
            std::size_t images = 0;
            for (const auto part : parts) {
                require(part.is_object());
                auto metadata = json::object();
                if (part.contains("thought")) { require(part["thought"].is_boolean()); metadata["thought"] = part["thought"]; }
                if (part.contains("thoughtSignature")) metadata["thought_signature"] = string(part["thoughtSignature"]);
                require(part.contains("text") != part.contains("inlineData"));
                if (part.contains("text")) append(TextOutput{string(part["text"]), std::make_shared<const json>(std::move(metadata))});
                else {
                    const auto inline_data = part["inlineData"];
                    require(inline_data.is_object());
                    auto data = string(inline_data["data"]);
                    append(binary(data, string(inline_data["mimeType"]), std::move(metadata), state->artifact_limit));
                    ++images;
                }
            }
            require(images > 0);
        } else {
            result.created = optional_integer(document, "created");
            const auto data = document["data"];
            const auto& c = std::get<OpenAIControls>(request.controls);
            require(data.is_array() && data.size() == c.n);
            for (const auto item : data) {
                require(item.is_object());
                auto metadata = json::object();
                if (item.contains("revised_prompt")) metadata["revised_prompt"] = string(item["revised_prompt"]);
                const bool has_binary = item.contains("b64_json") && !item["b64_json"].is_null();
                const bool has_uri = item.contains("url") && !item["url"].is_null();
                require(has_binary || has_uri);
                if (c.response_format == ResponseFormat::Url) require(has_uri);
                else require(has_binary);
                EndpointArtifact artifact;
                if (has_binary) {
                    auto data = string(item["b64_json"]);
                    std::string declared;
                    if (document.contains("output_format")) {
                        const auto format = string(document["output_format"]);
                        require(one_of(format, {"png", "jpeg", "webp"}));
                        declared = "image/" + format;
                    }
                    artifact = binary(data, std::move(declared), std::move(metadata), state->artifact_limit);
                } else artifact.metadata = std::make_shared<const json>(std::move(metadata));
                if (has_uri) { artifact.uri = string(item["url"]); require(valid_uri(artifact.uri)); }
                append(std::move(artifact));
            }
        }
        if (Clock::now() >= deadline) co_return failure(Kind::DeadlineExceeded, "Image generation deadline exceeded", 0, true);
        co_return result;
    } catch (const Invalid&) {
        co_return failure(sent ? Kind::Protocol : Kind::InvalidRequest, sent ? "Malformed image provider response" : "Invalid image generation controls", 0, sent);
    } catch (Kind kind) {
        co_return failure(kind, "Image response exceeded resource limit", 0, sent);
    } catch (const json::exception&) {
        co_return failure(sent ? Kind::Protocol : Kind::InvalidRequest, "Malformed image protocol data", 0, sent);
    } catch (const asio::system_error& error) {
        if (Clock::now() >= deadline || error.code() == asio::error::timed_out)
            co_return failure(Kind::DeadlineExceeded, "Image generation deadline exceeded", 0, sent);
        if ((request.cancel_token && request.cancel_token->is_cancelled()) || error.code() == asio::error::operation_aborted)
            co_return failure(Kind::Cancelled, "Image generation cancelled", 0, sent);
        co_return failure(Kind::Transport, "Image transport failed", 0, sent);
    } catch (...) {
        if (request.cancel_token && request.cancel_token->is_cancelled())
            co_return failure(Kind::Cancelled, "Image generation cancelled", 0, sent);
        if (Clock::now() >= deadline) co_return failure(Kind::DeadlineExceeded, "Image generation deadline exceeded", 0, sent);
        co_return failure(Kind::Transport, "Image transport failed", 0, sent);
    }
}
} // namespace neograph::llm
