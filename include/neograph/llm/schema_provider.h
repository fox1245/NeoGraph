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
#include <neograph/async/ws_client.h>
#include <neograph/provider.h>
#include <neograph/llm/json_path.h>
#include <neograph/llm/schema_strategy_registry.h>
#include <neograph/llm/schema_primitive_registry.h>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <cstddef>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <stdexcept>
#include <thread>
#include <map>
#include <vector>

namespace neograph::async {
class ConnPool;
class CurlH2Pool;
struct AsyncEndpoint;
struct HttpResponse;
}

namespace neograph::llm {

namespace test_access { class SchemaProviderTestAccess; }  // fwd-decl for friend

/// A failure of an operation-style (submit / poll / finalize) schema after the
/// job exists, or a malformed operation response. A transport or HTTP failure
/// while polling or finalizing carries its cause as a nested exception:
/// `std::rethrow_if_nested(error)` yields the `ProviderError` (status,
/// retryable, vendor code). It is deliberately not itself retryable, because
/// re-running the call would submit the job again. A failure of the submission
/// request itself (no job yet) is thrown as the `ProviderError` directly.
class NEOGRAPH_API OperationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class NEOGRAPH_API OperationTimeoutError : public OperationError {
public:
    using OperationError::OperationError;
};

/**
 * @brief LLM provider that adapts to any API via a JSON schema.
 *
 * The schema describes connection details, request/response formats,
 * tool call conventions, and streaming protocols for a given LLM vendor.
 * Built-in chat schemas include "openai", "openai_responses", "claude",
 * and "gemini"; media schemas include "openai_images" and "veo".
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
    /// Configuration for schema-based providers.
    struct Config {
        std::string schema_path;  ///< Path to schema file or built-in schema name.
        std::string api_key;      ///< API key (or empty for env lookup).
        std::string default_model = "gpt-4o-mini"; ///< Default model name.
        int timeout_seconds = 60; ///< HTTP timeout in seconds.
        std::string base_url_override; ///< If non-empty, overrides the schema's `connection.base_url`.
        /// Optional authentication overrides for adapters such as OpenRouter's
        /// Anthropic-compatible endpoint. Empty values preserve schema fields.
        std::string auth_header_override;
        std::string auth_prefix_override;
        /// Extra request headers set from code (for example
        /// `anthropic-workspace-id`, `anthropic-beta`). They are added after the
        /// schema's `connection.extra_headers` and replace a schema header of
        /// the same name (names compare case-insensitively). Values are sent
        /// literally; an unusable name or a value containing a line break is
        /// rejected when the provider is created. Adding headers does not
        /// relax the credential rule: credentials are still refused over a
        /// non-loopback `http://` endpoint.
        std::map<std::string, std::string> extra_headers;
        /// Drive `complete_stream` over a WebSocket instead of HTTP/SSE.
        /// Currently supported only for the "openai-responses" schema.
        /// This matches OpenAI's WebSocket mode at `/v1/responses`, which
        /// claims ~40% lower latency for long agentic tool calls.
        bool use_websocket = false;
        /// Default top-level OpenRouter provider-routing object stamped into
        /// every request. A per-call `CompletionParams::extra_fields.provider`
        /// object overrides this value.
        json provider_routing;
        /// Optional registry of reviewed aliases for schema strategy names.
        /// When unset, the standard registry is used.
        std::shared_ptr<const SchemaStrategyRegistry> strategy_registry;
        /// Prefer the libcurl HTTP/2 pool over the httplib transport.
        bool prefer_libcurl = false;
        /// Permit credentials over explicit http:// only for a literal
        /// loopback endpoint. Intended solely for local tests/development.
        bool allow_insecure_loopback = false;
        /// Maximum retained bytes for one unterminated HTTP/SSE line.
        std::size_t max_stream_line_bytes = 64u * 1024u;
        /// Maximum aggregate bytes accepted from one HTTP/SSE or WebSocket
        /// streaming response.
        std::size_t max_stream_response_bytes = 16u * 1024u * 1024u;
        /// WebSocket handshake, frame, and assembled-message limits.
        async::WsClientOptions websocket_options;
        /// Explicitly injected C++ factories for schema transport,
        /// execution-mode, and artifact-parser primitives. The registry is
        /// copied at provider creation; later mutations are not observed.
        std::shared_ptr<const SchemaPrimitiveRegistry> primitive_registry;
        /// Trace metadata copied into every primitive request context.
        std::map<std::string, std::string> trace_metadata;
    };
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

    /**
     * @brief POST an arbitrary JSON body using the schema connection contract.
     *
     * This is the non-Chat endpoint escape hatch for schema-described APIs.
     * The schema still owns the base URL, endpoint, authentication, and
     * transport policy; the caller owns the endpoint-specific JSON body and
     * receives the decoded JSON response without a ChatCompletion projection.
     *
     * For example, the built-in `openrouter_decisions` schema targets
     * OpenRouter's alpha Decisions endpoint for Typesafe/Jev (verified
     * against the upstream API reference on 2026-09-22):
     * https://openrouter.ai/docs/api/api-reference/alphadecisions/submit-a-decisions-questions-and-answers-request
     *
     * @param body JSON request body sent verbatim after serialization.
     * @param timeout_seconds Positive values override Config::timeout_seconds;
     *        -1 keeps the provider default.
     * @param cancel_token Optional operation token for async cancellation.
     * @return Decoded JSON response body.
     */
    asio::awaitable<json>
    request_json_async(
        const json& body,
        int timeout_seconds = -1,
        std::shared_ptr<graph::CancelToken> cancel_token = {});

    /// Synchronous bridge for request_json_async().
    json request_json(const json& body, int timeout_seconds = -1);

    /// Sync completion is inherited from `Provider::complete()`, which
    /// drives `complete_async` via `neograph::async::run_sync`.

    /// Streaming completion (HTTP/SSE httplib path; WS path dispatched
    /// to `complete_stream_ws_responses` when `use_websocket=true` and
    /// the schema is `openai-responses`).
    /// Custom transport/execution primitives and long-running operations keep
    /// their single-response contract: completion text is delivered once after
    /// success, without substituting a built-in streaming network request.
    ///
    /// **Locking contract for `on_chunk`**: callbacks run outside
    /// `schema_mutex_`, including buffered extension completions, httplib's
    /// content callback (HTTP/SSE), and the WebSocket receive loop.
    /// The lock is taken only during the per-call body-
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
    /// For the HTTP/SSE path (httplib synchronous) it dispatches
    /// `complete_stream` onto the provider's long-lived bridge thread.
    /// That thread writes tokens to a private queue; the awaiting
    /// coroutine drains the queue on its own executor. The engine's
    /// io_context stays responsive, user callbacks remain single-
    /// threaded with the awaiter, and abandoned calls never dispatch
    /// back into a destroyed outer io_context.
    asio::awaitable<ChatCompletion>
    complete_stream_async(const CompletionParams& params,
                          const StreamCallback& on_chunk) override;

    /// Callback-selected compatibility override. Engine
    /// `provider->invoke(...)` calls land here and route through the
    /// stable native overrides above without changing their behavior.
    asio::awaitable<ChatCompletion>
    invoke(const CompletionParams& params, StreamCallback on_chunk) override;

    /// @brief Get the provider name (from the schema's "name" field).
    /// @return Provider identifier string (e.g., "openai", "claude", "gemini").
    std::string get_name() const override;

  private:
    explicit SchemaProvider(Config config, json schema);
    struct StreamCancelControl;

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
        /// HTTP statuses this vendor documents as transient
        /// (`connection.retryable_statuses`). A failure with one of these
        /// becomes a retryable `ProviderError`; 429 is always a `RateLimitError`.
        std::set<int> retryable_statuses;
        /// Vendor error codes/types that mark an in-stream or HTTP-200 error
        /// as transient (`connection.retryable_codes`), e.g. `overloaded_error`.
        std::set<std::string> retryable_codes;
    };

    struct RequestConfig {
        std::string model_field;
        std::string messages_field;
        std::string tools_field;
        std::string temperature_path;
        /// `request.temperature_unsupported_models`: model names (exact, or
        /// prefix when ending in `*`) whose endpoint rejects `temperature`
        /// with HTTP 400. build_body omits the field for these models.
        std::vector<std::string> temperature_unsupported_models;
        std::string max_tokens_path;
        bool max_tokens_required = false;
        int max_tokens_default = -1;
        std::string stream_field;
        json extra_fields;
        std::string prompt_field; ///< Non-chat envelope field; empty selects messages.
        json prompt_template; ///< Structured prompt envelope with $PROMPT/$MODEL values.

        /// Issue #33: which body paths a schema declares as bindable
        /// per-call via `CompletionParams::extra_fields`. Schema:
        ///   "request.per_call_fields": ["reasoning.effort", "thinking.budget"]
        /// build_body iterates the caller's `params.extra_fields` map
        /// and only stamps keys present in this set; what happens to any
        /// other key is `unknown_knob_policy` (below). Empty set = no
        /// per-call bindings honoured (legacy default; back-compat
        /// for schemas that don't declare the key).
        std::set<std::string> per_call_fields;

        /// `request.unknown_knob_policy`: what build_body does with a
        /// `CompletionParams::extra_fields` key the schema does not declare in
        /// `per_call_fields`. `"drop"` (the default, for schemas written
        /// before this key existed) ignores it silently; `"error"` throws
        /// `std::invalid_argument` naming the declared keys, so a typo or an
        /// unsupported knob cannot look like success. The built-in schemas use
        /// `"error"`.
        bool unknown_knob_is_error = false;

        /// `request.rules`: vendor constraints on the finished body.
        ///   {"omit": "<path>", "when": {"path": "<p>", "in": ["a", "b"]}}
        ///       drops <path> when the body holds a string at <p> that is one
        ///       of the listed values (Anthropic rejects `temperature` != 1
        ///       while thinking is on, so it is left to the server default).
        ///   {"require_greater": {"path": "<a>", "than": "<b>"}}
        ///       throws std::invalid_argument when both are integers and
        ///       body[a] <= body[b] (Anthropic requires max_tokens >
        ///       thinking.budget_tokens); absent values leave the rule idle.
        struct Rule {
            enum class Kind { Omit, RequireGreater } kind = Kind::Omit;
            std::string path;
            std::string than;
            std::string when_path;
            std::set<std::string> when_in;
        };
        std::vector<Rule> rules;
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
        json url_item_template;
        bool remote_url_supported = false;
    };

    struct ResponseConfig {
        ResponseStrategy strategy;
        std::string message_path;
        std::string content_field;
        std::vector<std::string> reasoning_fields;
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
        /// Optional usage fields ADDED to the prompt count (Anthropic reports
        /// the cached prefix apart from `input_tokens`).
        std::vector<std::string> prompt_extra_fields;
        /// Optional path (inside the usage object) of the prompt-token subset
        /// served from cache -> `Usage::cached_prompt_tokens`.
        std::string cached_tokens_path;
        /// Optional path of the completion-token subset spent on reasoning
        /// -> `Usage::reasoning_tokens`.
        std::string reasoning_tokens_path;
        /// False when the vendor's completion counter excludes reasoning
        /// (Gemini); reasoning is then added so `completion_tokens` is
        /// everything the model produced.
        bool completion_includes_reasoning = true;
        std::string stop_reason_path;
        std::map<std::string, std::string> stop_reason_map;
        std::string stop_reason_status_path;
        std::map<std::string, std::string> stop_reason_status_map;
        std::string default_stop_reason = "unknown";
        /// Optional `response.error_path`: dotted path of an error object that
        /// marks a response as a failure even though the HTTP status was 200
        /// (gateways, `status: "failed"` bodies). Present -> `ProviderError`.
        std::string error_path;
        /// Optional `response.failure_status_path` + `failure_statuses`: a
        /// body whose status field holds one of these values (`failed`,
        /// `cancelled`) is a failure, never an empty successful completion.
        std::string failure_status_path;
        std::set<std::string> failure_statuses;
        /// Optional `response.block_reason_path`: present and non-empty means
        /// the prompt was blocked; the completion reports `block_stop_reason`
        /// instead of a normal end of turn.
        std::string block_reason_path;
        std::string block_stop_reason = "content_filter";
        /// Optional `response.error_finish_reasons`: raw vendor finish reasons
        /// that mean the model failed (not "stopped"); they throw a
        /// non-retryable `ProviderError` carrying the reason as its code.
        std::set<std::string> error_finish_reasons;
        struct ArtifactRule {
            std::string items_path, type_path, type, match_path, kind;
            std::string mime_type, mime_path, base64_path, url_path, file_id_path, metadata_path;
        };
        std::vector<ArtifactRule> artifacts;
    };

    /// Optional `reasoning` schema section: which provider reasoning items a
    /// response carries verbatim into `ChatMessage::reasoning_details` and
    /// replays on the assistant message that produced them. The interpreter
    /// knows no vendor names; every field below is data declared per schema.
    struct ReasoningConfig {
        /// Block/item `type` values (content[] / output[] entries) kept
        /// verbatim, in wire order. Replayed in front of the tool calls.
        std::set<std::string> carry_types;
        /// Field of a carried item holding readable text (a string, or an
        /// array of objects with a `text` string) for `ChatMessage::reasoning`.
        std::string text_field;
        /// Part-style: a part whose boolean field of this name is true is
        /// reasoning, not user-visible content.
        std::string thought_flag_field;
        /// Part-style: sibling field of a functionCall part carrying an opaque
        /// signature that must be echoed on the same part when replayed.
        std::string signature_field;
        /// Part-style, optional: value placed in `signature_field` on the first
        /// tool call of an assistant message that carries no captured
        /// signature -- a history that came from another vendor or model, or
        /// was built by hand. The API validates the signature and would answer
        /// HTTP 400 without one; the vendor documents this placeholder as a
        /// last resort that lowers quality for that turn. Absent = never invent
        /// a signature.
        std::string foreign_signature;
        /// Chat-style: assistant message field holding an opaque array that is
        /// captured verbatim and replayed on the assistant message.
        std::string message_field;
        /// Streaming, event style: maps a delta `type` to the field of the
        /// carried block it extends (string values are concatenated), for
        /// example `thinking_delta` -> `thinking`, `signature_delta` -> `signature`.
        std::map<std::string, std::string> delta_fields;
        /// Streaming, chat style: fields of `message_field` fragments that are
        /// concatenated when fragments of one item (same type + index) arrive.
        std::set<std::string> stream_concat_fields;
    };

    struct StreamConfig {
        StreamFormat format;
        std::string prefix;
        std::string done_signal;
        std::string delta_path;
        std::string content_field;
        std::vector<std::string> reasoning_fields;
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
        std::string stop_reason_path;
        std::string stop_reason_status_path;
        /// Optional `streaming.error_path`: dotted path checked in every
        /// data chunk (SSE_DATA); an object there is an in-stream error.
        std::string error_path;
        /// `streaming.require_terminal_event`: the stream is only complete if
        /// its terminal event (`done` action / done signal) arrived. EOF
        /// without one throws a retryable `ProviderError` instead of
        /// returning the partial output as a finished answer.
        bool require_terminal_event = false;
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
    // its purpose. So SchemaProvider owns a long-lived io_context and
    // worker thread for the default ConnPool path. post_json dispatches
    // non-streaming calls through that pool or the optional curl pool;
    // successive calls amortise TCP connect and TLS handshake.
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
    // Optional libcurl-backed HTTP/2 pool (prefer_libcurl=true). post_json
    // selects this only for non-streaming HTTP; SSE uses httplib and the
    // Responses WebSocket path owns its native async connection.
    std::unique_ptr<async::CurlH2Pool>  curl_pool_;

    // --- Parsed config ---
    SchemaPrimitiveRegistry primitive_registry_;
    std::string transport_primitive_name_;
    std::string execution_primitive_name_;
    std::string artifact_parser_primitive_name_;
    SchemaTransportFactory transport_factory_;
    SchemaExecutionFactory execution_factory_;
    SchemaArtifactParserFactory artifact_parser_factory_;
    SchemaStrategyRegistry strategy_registry_;
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
    ReasoningConfig reasoning_;
    StreamConfig stream_;
    struct OperationConfig {
        std::string id_path, done_path, error_path, result_path;
        std::string poll_endpoint, poll_method = "GET", finalize_endpoint;
        int poll_interval_ms = 1000;
        bool absent_status_pending = false;
    } operation_;

    // --- Internal methods ---
    void parse_schema();

    json build_body(const CompletionParams& params, bool websocket = false) const;
    json build_sse_body(const CompletionParams& params) const;
    json build_ws_body(const CompletionParams& params) const;
    json serialize_messages(const std::vector<ChatMessage>& messages) const;
    json serialize_tools(const std::vector<ChatTool>& tools) const;
    json serialize_single_message(const ChatMessage& msg) const;
    // Applies the schema's `request.rules` to a finished request body.
    void apply_request_rules(json& body) const;

    // Per-request value-level streaming decoder state. No sockets or
    // executors: callers can feed fixture lines independently of transport.
    struct StreamParseState {
        struct EventBlock {
            std::string type, id, name, args;
            int index = -1;
            /// Set for blocks whose type the schema carries verbatim; `raw`
            /// accumulates the block (start payload + deltas) until it stops.
            bool carried = false;
            json raw;
        };
        ChatCompletion completion;
        SchemaPrimitiveRequestContext primitive_context;
        std::string full_content;
        std::map<int, ToolCall> tc_map;
        std::vector<EventBlock> event_blocks;
        int event_block_index = -1;
        int gemini_tc_index = 0;
        std::string current_event_type;
        std::string observed_stop_reason;
        bool terminal_event_seen = false;
    };
    bool consume_stream_line(StreamParseState& state, const std::string& line,
                             const StreamCallback& on_chunk) const;
    void consume_ws_event(StreamParseState& state, const json& event,
                          const StreamCallback& on_chunk) const;
    ChatCompletion finish_stream(StreamParseState& state) const;

    // Throws a typed ProviderError when a non-stream body (HTTP 200) is a
    // failure per the schema's `response.error_path` / `failure_status_*`.
    void check_response_failure(const json& resp_json) const;
    // Throws the typed error described by a stream event whose schema action
    // is `error` or `fail` (SSE and WebSocket share it).
    [[noreturn]] void throw_stream_event_error(const std::string& event_type,
                                              const json& payload,
                                              const json& event_cfg) const;
    // Throws when `error_path` of a chunk holds an error object.
    void check_stream_chunk_error(const json& chunk) const;
    // Retry classification for HTTP failures of this provider's schema.
    [[noreturn]] void throw_http_failure(int status, const std::string& body,
                                         int retry_after_seconds,
                                         const std::string& request_id) const;

    // Owns HTTP/1.1 vs HTTP/2 selection and operation-local cancellation
    // for both chat completions and schema-described JSON endpoints.
    asio::awaitable<async::HttpResponse> post_json(
        async::AsyncEndpoint endpoint, std::string path, std::string body,
        std::vector<std::pair<std::string, std::string>> headers,
        int timeout_seconds, std::shared_ptr<graph::CancelToken> cancel_token,
        const char* cancel_context, bool get = false);

    /// WebSocket-mode streaming for OpenAI Responses. Async-native;
    /// `complete_stream` bridges via `neograph::async::run_sync`.
    /// Throws if `provider_name_ != "openai-responses"`.
    asio::awaitable<ChatCompletion>
    complete_stream_ws_responses(const CompletionParams& params,
                                 const StreamCallback& on_chunk);

    /// Blocking HTTP/SSE implementation shared by the synchronous API and the
    /// async bridge. The bridge supplies a control object so cancellation can
    /// interrupt cpp-httplib's active socket from its caller executor.
    ChatCompletion complete_stream_http(
        const CompletionParams& params,
        const StreamCallback& on_chunk,
        const std::shared_ptr<StreamCancelControl>& cancel_control);

    ChatMessage parse_response(const json& resp_json) const;
    ChatCompletion::Usage parse_usage(const json& resp_json) const;
    std::string parse_stop_reason(const json& resp_json) const;
    std::vector<GeneratedArtifact> parse_artifacts(
        const json& response,
        const SchemaPrimitiveRequestContext* request_context = nullptr) const;
    std::string operation_endpoint(const std::string& endpoint,
                                   const std::string& operation_id,
                                   std::string_view api_key) const;
    std::string parse_stream_stop_reason(const json& event_json) const;

    std::string build_endpoint(const std::string& model, bool streaming,
                               std::string_view api_key) const;
    // The schema's `connection.extra_headers` with `${ENV}` / `${ENV?}`
    // expanded (see header_template.h), then `Config::extra_headers` on top.
    std::map<std::string, std::string> resolved_extra_headers() const;
    std::map<std::string, std::string> build_headers(
        std::string_view api_key) const;
    std::string get_api_key() const;

    static std::pair<std::string, std::string> parse_data_url(const std::string& url);
    static json substitute(const json& tmpl, const std::map<std::string, json>& vars);
    static std::string generate_tool_call_id();

    // Test-only access to private internals. The helper class lives in a
    // separate `test_access` namespace to discourage accidental use; tests
    // that need to inspect build_body / serialize_messages output for
    // contract verification (e.g. issue #34, #35 regression coverage)
    // pull it in explicitly. NOT a public API surface — may change without
    // notice between versions.
    friend class neograph::llm::test_access::SchemaProviderTestAccess;
};

namespace test_access {

/// Test-only friend of `SchemaProvider`. See the `friend class` line
/// inside `SchemaProvider` for the rationale. Static methods only —
/// stateless wrapper.
class SchemaProviderTestAccess {
  public:
    static json build_body(const SchemaProvider& sp,
                           const CompletionParams& params) {
        return sp.build_body(params);
    }

    static std::map<std::string, std::string> build_headers(const SchemaProvider& sp,
                                                           std::string_view api_key) {
        return sp.build_headers(api_key);
    }

    static json build_sse_body(const SchemaProvider& sp,
                               const CompletionParams& params) {
        return sp.build_sse_body(params);
    }

    static json build_ws_body(const SchemaProvider& sp,
                              const CompletionParams& params) {
        return sp.build_ws_body(params);
    }

    static ChatMessage parse_response(const SchemaProvider& sp,
                                      const json& response) {
        return sp.parse_response(response);
    }

    static ChatCompletion parse_stream_lines(
        const SchemaProvider& sp, const std::vector<std::string>& lines,
        const StreamCallback& on_chunk = {}) {
        SchemaProvider::StreamParseState state;
        state.completion.message.role = "assistant";
        for (const auto& line : lines) {
            if (!sp.consume_stream_line(state, line, on_chunk)) break;
        }
        return sp.finish_stream(state);
    }

    static ChatCompletion parse_ws_events(
        const SchemaProvider& sp, const std::vector<json>& events,
        const StreamCallback& on_chunk = {}) {
        SchemaProvider::StreamParseState state;
        state.completion.message.role = "assistant";
        for (const auto& event : events) {
            sp.consume_ws_event(state, event, on_chunk);
            if (state.terminal_event_seen) break;
        }
        if (!state.terminal_event_seen) {
            throw ProviderError("openai-responses ws: server closed before response.completed",
                                0, true, "stream_truncated");
        }
        return sp.finish_stream(state);
    }
};

}  // namespace test_access

} // namespace neograph::llm
