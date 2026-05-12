/**
 * @file llm/schema_provider.h
 * @brief Multi-vendor LLM provider adapter driven by JSON schema configuration.
 *
 * SchemaProvider reads a JSON schema file that describes how to serialize
 * requests and parse responses for any LLM API (OpenAI, Claude, Gemini, etc.).
 * Built-in schemas are embedded at build time via embed_schemas.py.
 *
 * This allows supporting new LLM providers without writing C++ code --
 * just add a new JSON schema file.
 *
 * @warning **Downstream `httplib.h` macro consistency** (issue #16). The
 * SchemaProvider implementation TU defines `CPPHTTPLIB_OPENSSL_SUPPORT`
 * before including `<httplib.h>`. If your application **also** includes
 * `<httplib.h>` in any of its own TUs (e.g. to run an `httplib::Server`
 * SSE endpoint), every such include site MUST `#define CPPHTTPLIB_OPENSSL_SUPPORT`
 * before the include, OR you must set
 * `target_compile_definitions(your_target PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)`
 * globally. cpp-httplib is header-only and `ClientImpl`'s layout differs
 * between the two macro states; mismatched TUs produce a silent ODR
 * violation that SEGVs inside `getaddrinfo` on the first LLM call. The
 * audit recipe and a worked example live in
 * docs/troubleshooting.md under "C++ consumers — `httplib.h` macro
 * consistency".
 */
#pragma once

#include <neograph/api.h>
#include <neograph/provider.h>
#include <neograph/llm/json_path.h>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <map>

namespace neograph::async { class ConnPool; class CurlH2Pool; }

namespace neograph::llm {

/**
 * @brief LLM provider that adapts to any API via a JSON schema.
 *
 * The schema describes connection details, request/response formats,
 * tool call conventions, and streaming protocols for a given LLM vendor.
 * Built-in schemas: "openai", "claude", "gemini".
 *
 * @code
 * auto provider = SchemaProvider::create({
 *     .schema_path = "claude",          // built-in schema name
 *     .api_key = "sk-ant-...",
 *     .default_model = "claude-sonnet-4-20250514"
 * });
 * @endcode
 *
 * @see OpenAIProvider for a simpler OpenAI-only provider.
 */
class NEOGRAPH_API SchemaProvider : public Provider {
  public:
    /// Configuration for schema-based provider.
    struct Config {
        std::string schema_path;                    ///< Path to schema file, or built-in name ("openai", "claude", "gemini").
        std::string api_key;                        ///< API key (overrides env var if set).
        std::string default_model = "gpt-4o-mini";  ///< Default model name.
        int timeout_seconds = 60;                   ///< HTTP request timeout in seconds.
        std::string base_url_override;              ///< If non-empty, overrides the schema's `connection.base_url`. Useful for test doubles and self-hosted OpenAI-compatible endpoints.

        /// Drive `complete_stream` over `wss://` instead of HTTP/SSE.
        /// Currently supported only for the "openai-responses" schema —
        /// matches OpenAI's WebSocket mode at /v1/responses, which
        /// claims ~40% lower latency on agentic rollouts with 20+ tool
        /// calls (per developers.openai.com/api/docs/guides/websocket-mode).
        /// Throws on `complete_stream` for any other schema. Has no
        /// effect on `complete_async` / `complete()` (non-streaming
        /// path stays HTTP).
        bool use_websocket = false;

        /// Switch the non-streaming HTTP transport to libcurl (HTTP/2
        /// + multiplexing + Cloudflare-friendly fingerprint). Default
        /// off: empirical bench (dr_compare 2026-04-26) showed parity
        /// or slight regression on the LangGraph-equivalent
        /// deep-research workload vs the default HTTP/1.1 ConnPool;
        /// the multiplex win only materialises when fan-out width
        /// dominates per-call latency, and our default config doesn't
        /// hit that yet. Flip on if you have parallel-fan-out code
        /// hitting Cloudflare-WAF endpoints (where the default httplib
        /// path may get fingerprinted out).
        bool prefer_libcurl = false;
    };

    /**
     * @brief Create a schema-based provider instance.
     *
     * If schema_path matches a built-in name, the embedded schema is used.
     * Otherwise it is treated as a file path.
     *
     * @param config Provider configuration with schema path and API key.
     * @return A unique_ptr to the created provider.
     * @throws std::runtime_error If the schema cannot be loaded or parsed.
     */
    static std::unique_ptr<SchemaProvider> create(const Config& config);

    /// Destructor — shuts down the background HTTP loop + worker
    /// thread held alongside the ConnPool. Out-of-line so the
    /// ConnPool forward declaration is enough at this header.
    ~SchemaProvider();

    /// Async completion — single wire path implemented over the owned
    /// ConnPool (HTTP keep-alive). The schema_mutex_ still serializes
    /// the body-build and response-parse phases (yyjson_mut_doc traversal
    /// is not thread-safe even for reads), but the network I/O happens
    /// off-lock so concurrent fan-out still overlaps.
    asio::awaitable<ChatCompletion>
    complete_async(const CompletionParams& params) override;

    /// Sync completion is inherited from `Provider::complete()`, which
    /// drives `complete_async` via `neograph::async::run_sync`.

    /// Streaming completion (HTTP/SSE httplib path; WS path dispatched
    /// to `complete_stream_ws_responses` when `use_websocket=true` and
    /// the schema is `openai-responses`).
    ///
    /// **Locking contract for `on_chunk`**: the callback is invoked
    /// from inside httplib's content callback (HTTP/SSE) or the
    /// WebSocket recv loop (WS), in BOTH cases with `schema_mutex_`
    /// NOT held — the lock is taken only during the per-call body-
    /// build + per-call response-parse phases at the start of the
    /// request, then released before the network roundtrip begins.
    /// Parse state passed through `on_chunk` (accumulated `full_content`,
    /// tool-call map, etc.) is stack-local to this call and not shared
    /// with the `schema_mutex_`-protected schema templates. Callers
    /// can therefore (a) safely call other provider methods from
    /// inside `on_chunk` without deadlocking, and (b) assume tokens
    /// emitted by concurrent `complete_stream` calls on the same
    /// provider do not interleave their parse state.
    ChatCompletion complete_stream(const CompletionParams& params,
                                   const StreamCallback& on_chunk) override;

    /// Async streaming completion — native override (issue #4).
    ///
    /// For the WebSocket Responses path (use_websocket=true,
    /// openai-responses schema) this co_awaits the existing
    /// `complete_stream_ws_responses` directly — no bridge thread,
    /// no nested `run_sync`, no shared-state race against the
    /// awaiter's io_context.
    ///
    /// For the HTTP/SSE path (httplib synchronous) it defers to
    /// `Provider::complete_stream_async`'s base implementation, which
    /// post-#4 spawns a dedicated worker thread for `complete_stream`
    /// and dispatches tokens back onto the awaiter's executor — so
    /// the engine's io_context worker stays responsive and the user
    /// `on_chunk` runs single-threaded with the awaiting coroutine.
    asio::awaitable<ChatCompletion>
    complete_stream_async(const CompletionParams& params,
                          const StreamCallback& on_chunk) override;

    /// @brief Get the provider name (from the schema's "name" field).
    /// @return Provider identifier string (e.g., "openai", "claude", "gemini").
    std::string get_name() const override;

  private:
    explicit SchemaProvider(Config config, json schema);

    // --- Strategies (internal) ---
    enum class SystemPromptStrategy { IN_MESSAGES, TOP_LEVEL, TOP_LEVEL_PARTS };
    // FLAT_ITEMS: OpenAI Responses — tool calls are separate top-level items in input[] (not nested in a message).
    enum class ToolCallStrategy { TOOL_CALLS_ARRAY, CONTENT_ARRAY, PARTS_ARRAY, FLAT_ITEMS };
    // FLAT_ITEM: OpenAI Responses — {type:"function_call_output", call_id, output} as a top-level input[] item.
    enum class ToolResultStrategy { FLAT, CONTENT_ARRAY, PARTS_ARRAY, FLAT_ITEM };
    // FLAT_FUNCTION: OpenAI Responses — [{type:"function", name, description, parameters}] (no nesting under "function").
    enum class ToolDefWrapper { FUNCTION, NONE, FUNCTION_DECLARATIONS, FLAT_FUNCTION };
    // OUTPUT_ARRAY: OpenAI Responses — output[] with mixed message/function_call items.
    enum class ResponseStrategy { CHOICES_MESSAGE, CONTENT_ARRAY, CANDIDATES_PARTS, OUTPUT_ARRAY };
    enum class StreamFormat { SSE_DATA, SSE_EVENTS };

    // --- Internal config parsed from schema ---
    struct ConnectionConfig {
        std::string base_url;
        std::string endpoint;
        std::string stream_endpoint;
        std::string auth_header;
        std::string auth_prefix;
        std::string api_key_env;
        std::string auth_query_param;
        std::map<std::string, std::string> extra_headers;
    };

    struct RequestConfig {
        std::string model_field;
        std::string messages_field;
        std::string tools_field;
        std::string temperature_path;
        std::string max_tokens_path;
        bool max_tokens_required = false;
        int max_tokens_default = -1;
        std::string stream_field;
        json extra_fields;
    };

    struct SystemPromptConfig {
        SystemPromptStrategy strategy;
        std::string field;
        std::string role_name;
        std::string parts_field;
        std::string text_field;
    };

    struct MessagesConfig {
        std::string role_field;
        std::string content_field;
        std::map<std::string, std::string> role_map;
        bool content_is_parts = false;
        json text_part_template;
    };

    struct ToolDefConfig {
        ToolDefWrapper wrapper;
        std::string name_field;
        std::string description_field;
        std::string parameters_field;
    };

    struct ToolCallConfig {
        ToolCallStrategy strategy;
        std::string field;
        json item_template;
        json text_item_template;
    };

    struct ToolResultConfig {
        std::string role;
        ToolResultStrategy strategy;
        std::string id_field;
        std::string content_field;
        json item_template;
    };

    struct ImageConfig {
        std::string strategy;
        json item_template;
        json text_part_template;
    };

    struct ResponseConfig {
        ResponseStrategy strategy;
        std::string message_path;
        std::string content_field;
        std::string role_field;
        std::string tool_calls_field;
        std::string tool_call_id_field;
        std::string tool_call_name_path;
        std::string tool_call_args_path;
        bool tool_call_args_is_string = true;
        std::string content_path;
        std::string text_type;
        std::string text_field;
        std::string tool_use_type;
        std::string tool_call_name_field;
        std::string tool_call_args_field;
        std::string parts_path;
        std::string function_call_field;
        // OUTPUT_ARRAY (OpenAI Responses)
        std::string output_path;             ///< Path to output[] array (default: "output").
        std::string message_item_type;       ///< Discriminator value for message items (default: "message").
        std::string function_call_item_type; ///< Discriminator value for function_call items (default: "function_call").
        std::string message_content_field;   ///< Field holding message item's content[] (default: "content").
        std::string function_call_id_field;  ///< Field for tool call id inside function_call item (default: "call_id").
        std::string usage_path;
        std::string prompt_tokens_field;
        std::string completion_tokens_field;
        std::string total_tokens_field;
    };

    struct StreamConfig {
        StreamFormat format;
        std::string prefix;
        std::string done_signal;
        std::string delta_path;
        std::string content_field;
        std::string tool_calls_field;
        std::string tool_call_index_field;
        std::string tool_call_id_field;
        std::string tool_call_name_path;
        std::string tool_call_args_path;
        std::string delta_strategy;
        std::string delta_parts_path;
        std::string delta_text_field;
        std::string delta_function_call_field;
        std::string delta_tool_call_name_field;
        std::string delta_tool_call_args_field;
        json events_config;
    };

    // Serializes access to the schema-derived json templates (schema_,
    // tool_call_.item_template, tool_result_.item_template, req_.extra_fields,
    // etc.). These are backed by shared yyjson_mut_doc handles — even
    // read-only traversal of a yyjson_mut_val from multiple threads at once
    // trips internal iterator state that yyjson explicitly disclaims as
    // thread-unsafe for mutable docs. HTTP I/O is issued OUTSIDE this lock
    // so concurrent fan-out requests still overlap on the network.
    mutable std::mutex schema_mutex_;

    // --- Connection pool for HTTP keep-alive ---
    //
    // Each Provider::complete() goes through run_sync, which creates a
    // fresh asio::io_context per call. A ConnPool bound to that
    // throw-away executor would survive only one request — defeating
    // its purpose. So SchemaProvider owns its own long-lived
    // io_context + worker thread; the pool is bound to that, and
    // every complete_async dispatches through it. Successive calls to
    // the same host then amortise TCP connect + TLS handshake.
    std::unique_ptr<asio::io_context> http_io_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> http_work_;
    std::thread http_thread_;
    std::unique_ptr<async::ConnPool>    conn_pool_;

    // --- Long-lived "sync-bridge" thread for streaming (issue #16) ---
    //
    // The streaming HTTP/SSE path is implemented as a synchronous
    // httplib::Client::Post call inside `complete_stream`. The previous
    // `complete_stream_async` default ran that on a *fresh* `std::thread`
    // per call, which exposed cold thread-local resolver / NSS init in
    // glibc. The wild ptr in `internal_strlen` reported in #16 had this
    // shape: outer io.run() driven from an HTTP server worker thread →
    // fresh-spawn NeoGraph worker → first getaddrinfo on cold TLS.
    //
    // Fix: own one long-lived bridge thread (mirror of `http_thread_`
    // for ConnPool). `complete_stream_async` HTTP/SSE branch dispatches
    // each call onto this thread instead of spawning fresh. After the
    // first call warms the thread-local resolver state, every
    // subsequent call reuses the warm state — same robustness profile
    // as the working `complete_async` path.
    std::unique_ptr<asio::io_context> bridge_io_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> bridge_work_;
    std::thread bridge_thread_;
    // libcurl-backed HTTP/2 pool with multiplexing. Default transport
    // for SchemaProvider — passes Cloudflare/anti-bot WAFs (it IS curl)
    // and gives us native HTTP/2 stream multiplexing for parallel
    // fan-out workloads.
    std::unique_ptr<async::CurlH2Pool>  curl_pool_;

    // --- Parsed config ---
    Config user_config_;
    json schema_;
    std::string provider_name_;
    ConnectionConfig conn_;
    RequestConfig req_;
    SystemPromptConfig sys_;
    MessagesConfig msgs_;
    ToolDefConfig tool_def_;
    ToolCallConfig tool_call_;
    ToolResultConfig tool_result_;
    ImageConfig image_;
    ResponseConfig resp_;
    StreamConfig stream_;

    // --- Internal methods ---
    void parse_schema();

    json build_body(const CompletionParams& params) const;
    json serialize_messages(const std::vector<ChatMessage>& messages) const;
    json serialize_tools(const std::vector<ChatTool>& tools) const;
    json serialize_single_message(const ChatMessage& msg) const;

    /// WebSocket-mode streaming for OpenAI Responses. Async-native;
    /// `complete_stream` bridges via `neograph::async::run_sync`.
    /// Throws if `provider_name_ != "openai-responses"`.
    asio::awaitable<ChatCompletion>
    complete_stream_ws_responses(const CompletionParams& params,
                                 const StreamCallback& on_chunk);

    ChatMessage parse_response(const json& resp_json) const;
    ChatCompletion::Usage parse_usage(const json& resp_json) const;

    std::string build_endpoint(const std::string& model, bool streaming) const;
    std::map<std::string, std::string> build_headers() const;
    std::string get_api_key() const;

    static std::pair<std::string, std::string> parse_data_url(const std::string& url);
    static json substitute(const json& tmpl, const std::map<std::string, json>& vars);
    static std::string generate_tool_call_id();
};

} // namespace neograph::llm
