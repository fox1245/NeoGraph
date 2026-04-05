/**
 * @file provider.h
 * @brief Abstract LLM provider interface.
 *
 * Defines the Provider base class that all LLM backends must implement.
 * Supports both synchronous and streaming completions.
 */
#pragma once

#include <neograph/types.h>
#include <functional>
#include <memory>
#include <string>

namespace neograph {

/// Callback invoked per token during streaming completion.
/// @param chunk The token or text chunk received from the LLM.
using StreamCallback = std::function<void(const std::string& chunk)>;

/**
 * @brief Parameters for an LLM completion request.
 */
struct CompletionParams {
    std::string model;                  ///< Model name to use (e.g., "gpt-4o-mini").
    std::vector<ChatMessage> messages;  ///< Conversation history.
    std::vector<ChatTool> tools;        ///< Available tools the LLM may call.
    float temperature = 0.7f;           ///< Sampling temperature (0.0 = deterministic, 1.0 = creative).
    int max_tokens = -1;                ///< Maximum output tokens. -1 means provider default.
};

/**
 * @brief Abstract base class for LLM providers.
 *
 * Subclass this to integrate any LLM backend (OpenAI, Claude, Gemini, etc.).
 * Both synchronous and streaming completion must be implemented.
 *
 * @see neograph::llm::OpenAIProvider
 * @see neograph::llm::SchemaProvider
 */
class Provider {
  public:
    virtual ~Provider() = default;

    /**
     * @brief Perform a synchronous LLM completion.
     * @param params Completion parameters including model, messages, and tools.
     * @return The full completion response with message and usage statistics.
     */
    virtual ChatCompletion complete(const CompletionParams& params) = 0;

    /**
     * @brief Perform a streaming LLM completion.
     *
     * Calls @p on_chunk for each token as it arrives, then returns the
     * full assembled completion when done.
     *
     * @param params Completion parameters.
     * @param on_chunk Callback invoked per received token.
     * @return The full completion response after streaming is complete.
     */
    virtual ChatCompletion complete_stream(const CompletionParams& params,
                                           const StreamCallback& on_chunk) = 0;

    /**
     * @brief Get the provider name (e.g., "openai", "claude").
     * @return Provider identifier string.
     */
    virtual std::string get_name() const = 0;
};

} // namespace neograph
