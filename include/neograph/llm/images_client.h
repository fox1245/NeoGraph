#pragma once

#include <neograph/llm/endpoint_types.h>
#include <neograph/graph/cancel.h>
#include <asio/awaitable.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace neograph::llm {

// Generation only: neither provider result is a chat Completion or EndTurn.
class NEOGRAPH_API ImagesClient {
    struct State;
public:
    enum class ResponseFormat { Native, Base64, Url };
    enum class Thinking { Minimal, High };
    struct OpenAIControls {
        std::string size, quality;
        std::uint32_t n;
        ResponseFormat response_format;
    };
    struct GeminiControls {
        std::string aspect_ratio, image_size;
        Thinking thinking;
        std::uint32_t max_output_tokens;
    };
    using Controls = std::variant<OpenAIControls, GeminiControls>;
    class NEOGRAPH_API Config {
    public:
        // Closed version-1 operational JSON; admission failures never include input.
        static std::variant<Config, EndpointFailure> from_json(const json& document);
    private:
        explicit Config(std::shared_ptr<const State> state) : state_(std::move(state)) {}
        std::shared_ptr<const State> state_;
        friend class ImagesClient;
    };
    struct Request {
        std::string prompt;
        Controls controls;
        // Earlier of this absolute deadline and the configured operation timeout.
        std::optional<std::chrono::steady_clock::time_point> deadline;
        std::shared_ptr<graph::CancelToken> cancel_token;
    };
    struct TextOutput {
        std::string text;
        std::shared_ptr<const json> metadata;
    };
    using Output = std::variant<TextOutput, EndpointArtifact>;
    struct Generation {
        std::vector<Output> outputs;
        std::optional<EndpointUsage> usage;
        std::optional<std::uint64_t> created;
        std::string model, response_id;
    };
    using Result = std::variant<Generation, EndpointFailure>;

    ImagesClient(Config config, std::string api_key);
    // Named controls start at the admitted external defaults, not provider guesses.
    Request request(std::string prompt) const;
    // Inputs/client state are owned before the awaitable is returned. One POST only;
    // HTTPS certificate/hostname verification is mandatory, redirects/replay disabled.
    asio::awaitable<Result> async_generate(Request request) const;
private:
    static asio::awaitable<Result> generate_owned(
        std::shared_ptr<const State> state, std::shared_ptr<const std::string> key,
        Request request);
    std::shared_ptr<const State> state_;
    std::shared_ptr<const std::string> key_;
};

} // namespace neograph::llm
