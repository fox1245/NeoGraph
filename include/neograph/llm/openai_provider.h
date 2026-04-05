/**
 * @file llm/openai_provider.h
 * @brief OpenAI API provider implementation.
 *
 * Supports any OpenAI-compatible API endpoint, including OpenAI, Groq,
 * Together, vLLM, and Ollama. Uses cpp-httplib internally (PRIVATE linkage).
 */
#pragma once

#include <neograph/provider.h>
#include <memory>
#include <string>

namespace neograph::llm {

/**
 * @brief LLM provider for OpenAI-compatible APIs.
 *
 * Connects to any endpoint following the OpenAI chat completions API
 * format (`/v1/chat/completions`). Configure via Config struct.
 *
 * @code
 * auto provider = OpenAIProvider::create({
 *     .api_key = "sk-...",
 *     .default_model = "gpt-4o-mini"
 * });
 * @endcode
 *
 * @see SchemaProvider for multi-vendor support (Claude, Gemini).
 */
class OpenAIProvider : public Provider {
  public:
    /// Configuration for OpenAI-compatible API connections.
    struct Config {
        std::string api_key;                              ///< API key for authentication.
        std::string base_url = "https://api.openai.com";  ///< Base URL of the API endpoint.
        std::string default_model = "gpt-4o-mini";        ///< Default model name.
        int timeout_seconds = 60;                         ///< HTTP request timeout in seconds.
    };

    /**
     * @brief Create an OpenAI provider instance.
     * @param config Connection and authentication configuration.
     * @return A unique_ptr to the created provider.
     */
    static std::unique_ptr<OpenAIProvider> create(const Config& config);

    ChatCompletion complete(const CompletionParams& params) override;
    ChatCompletion complete_stream(const CompletionParams& params,
                                   const StreamCallback& on_chunk) override;
    std::string get_name() const override { return "openai"; }

  private:
    explicit OpenAIProvider(Config config);
    json build_body(const CompletionParams& params) const;
    Config config_;
};

} // namespace neograph::llm
