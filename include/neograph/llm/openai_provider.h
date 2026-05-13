/**
 * @file llm/openai_provider.h
 * @brief OpenAI API provider implementation.
 *
 * Supports any OpenAI-compatible API endpoint, including OpenAI, Groq,
 * Together, vLLM, and Ollama. Uses cpp-httplib internally (PRIVATE linkage).
 */
#pragma once

#include <neograph/api.h>
#include <neograph/provider.h>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace neograph::async { class ConnPool; }

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
class NEOGRAPH_API OpenAIProvider : public Provider {
  public:
    /// Configuration for OpenAI-compatible API connections.
    struct Config {
        std::string api_key;                              ///< API key for authentication.
        std::string base_url = "https://api.openai.com";  ///< Base URL of the API endpoint.
        std::string default_model = "gpt-4o-mini";        ///< Default model name.
        /// HTTP request timeout in seconds. Note: NeoGraph's public
        /// surface currently uses a mix of `int seconds` (here, in
        /// SchemaProvider, A2AClient default), `std::chrono::milliseconds`
        /// (ACPClient, ACPServer::call_client, async::HttpClient::Options),
        /// and dimensionless step counts (RunConfig::max_steps). A
        /// future v1.0 standardisation pass will move all timeouts to
        /// `std::chrono::milliseconds` on a process-level
        /// `NeoGraphConfig` defaults struct that individual configs
        /// inherit from. Until then, watch the unit when configuring
        /// across modules.
        int timeout_seconds = 60;
    };

    /**
     * @brief Create an OpenAI provider instance.
     * @param config Connection and authentication configuration.
     * @return A unique_ptr to the created provider.
     */
    static std::unique_ptr<OpenAIProvider> create(const Config& config);

    /**
     * @brief Same as @ref create but returns a `shared_ptr<Provider>`.
     *
     * Useful when one provider needs to be reused across multiple
     * NodeFactory closures, A2A servers, or threads — capturing a
     * `unique_ptr` into a copyable lambda is impossible without an
     * explicit `release()`. This peer drops the boilerplate.
     */
    static std::shared_ptr<Provider> create_shared(const Config& config);

    /// Destructor — shuts down the long-lived HTTP loop + worker
    /// thread held alongside the ConnPool.
    ~OpenAIProvider();

    /// Async completion — dispatches over the owned ConnPool so
    /// successive calls reuse a kept-alive TCP+TLS connection. The
    /// previous "fresh socket per call" comment has aged out: the
    /// pool is bound to a long-lived background io_context owned
    /// by this provider, so `run_sync` per-call destruction no
    /// longer affects pool lifetime.
    asio::awaitable<ChatCompletion>
    complete_async(const CompletionParams& params) override;

    /// Sync completion is inherited from `Provider::complete()`, which
    /// drives `complete_async` via `neograph::async::run_sync`.

    ChatCompletion complete_stream(const CompletionParams& params,
                                   const StreamCallback& on_chunk) override;

    /// v1.0 single-dispatch override (Candidate 6 PR6). Native invoke()
    /// that directly drives the ConnPool — no extra hop through
    /// `complete_stream_async`'s default worker-thread bridge for
    /// streaming, no extra hop through `complete_async` for non-
    /// streaming. The 4 legacy overrides above stay as thin adapters
    /// over invoke() through the v0.9 deprecation window; v1.0 deletes
    /// them along with the base-class virtuals.
    asio::awaitable<ChatCompletion>
    invoke(const CompletionParams& params, StreamCallback on_chunk) override;

    std::string get_name() const override { return "openai"; }

  private:
    explicit OpenAIProvider(Config config);
    json build_body(const CompletionParams& params) const;
    Config config_;

    // Long-lived HTTP loop + ConnPool. Same shape as SchemaProvider —
    // see commit 6da4810 / feedback_schema_provider_no_pool. ConnPool
    // can't live inside Provider::complete()'s run_sync io_context
    // (one-shot), so the provider owns its own io_context + worker.
    std::unique_ptr<asio::io_context> http_io_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> http_work_;
    std::thread http_thread_;
    std::unique_ptr<async::ConnPool> conn_pool_;
};

} // namespace neograph::llm
