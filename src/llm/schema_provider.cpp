#include <neograph/llm/schema_provider.h>
#include <neograph/async/conn_pool.h>
#include <neograph/async/curl_h2_pool.h>
#include <neograph/async/endpoint.h>
#include <neograph/async/http_client.h>
#include <neograph/async/run_sync.h>
#include <neograph/async/ws_client.h>
#include <neograph/graph/cancel.h>
#include <builtin_schemas.h>

#include "provider_error.h"

#include <asio/bind_cancellation_slot.hpp>
#include <asio/co_spawn.hpp>
#include <asio/dispatch.hpp>
#include <asio/post.hpp>
#include <asio/io_context.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <asio/this_coro.hpp>

#include <charconv>
#include <condition_variable>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <limits>

namespace neograph::llm {

struct SchemaProvider::StreamCancelControl {
    void attach(httplib::Client& target) {
        bool stop_now = false;
        {
            std::lock_guard lock(mutex);
            client = &target;
            if (cancelled) {
                ++stops_in_flight;
                stop_now = true;
            }
        }
        if (stop_now) {
            stop_target(target);
        }
    }

    void detach(httplib::Client& target) {
        std::unique_lock lock(mutex);
        if (client != &target) return;
        client = nullptr;
        stops_done.wait(lock, [this] { return stops_in_flight == 0; });
    }

    void cancel() {
        httplib::Client* target = nullptr;
        {
            std::lock_guard lock(mutex);
            if (cancelled) return;
            cancelled = true;
            if (client) {
                target = client;
                ++stops_in_flight;
            }
        }
        if (target) {
            // cpp-httplib v0.41.0 documents Client::stop() as its thread-safe
            // active-request abort path (vendored deps/httplib.h:14564).
            stop_target(*target);
        }
    }

    bool is_cancelled() const {
        std::lock_guard lock(mutex);
        return cancelled;
    }

  private:
    void stop_target(httplib::Client& target) {
        try {
            target.stop();
        } catch (...) {
            finish_stop();
            throw;
        }
        finish_stop();
    }

    void finish_stop() {
        std::lock_guard lock(mutex);
        --stops_in_flight;
        if (stops_in_flight == 0) stops_done.notify_all();
    }

    mutable std::mutex mutex;
    std::condition_variable stops_done;
    httplib::Client* client = nullptr;
    std::size_t stops_in_flight = 0;
    bool cancelled = false;
};

// ============================================================================
// Construction / Factory
// ============================================================================

SchemaProvider::SchemaProvider(Config config, json schema)
    : primitive_registry_(config.primitive_registry
                          ? *config.primitive_registry
                          : SchemaPrimitiveRegistry::standard())
    , strategy_registry_(config.strategy_registry
                         ? *config.strategy_registry
                         : SchemaStrategyRegistry::standard())
    , user_config_(std::move(config))
    , schema_(std::move(schema))
{
    parse_schema();

    if (user_config_.max_stream_line_bytes == 0 ||
        user_config_.max_stream_response_bytes == 0 ||
        user_config_.max_stream_line_bytes >
            user_config_.max_stream_response_bytes) {
        throw std::invalid_argument(
            "SchemaProvider stream limits must be nonzero and line <= response");
    }
    const bool credentialed = !conn_.auth_header.empty() ||
                              !conn_.auth_query_param.empty() ||
                              !conn_.extra_headers.empty();
    (void)async::validate_credential_endpoint(
        conn_.base_url, credentialed,
        user_config_.allow_insecure_loopback);

    // Stand up the long-lived HTTP loop + ConnPool. The pool is bound
    // to http_io_'s executor, so it survives the run_sync io_context
    // that drives any individual Provider::complete() call. Sync and
    // async completers both dispatch their HTTP work onto this loop;
    // successive calls to the same model host now amortise TCP+TLS.
    http_io_ = std::make_unique<asio::io_context>();
    http_work_.emplace(asio::make_work_guard(*http_io_));
    conn_pool_ = std::make_unique<async::ConnPool>(http_io_->get_executor());
    if (user_config_.prefer_libcurl) {
        curl_pool_ = std::make_unique<async::CurlH2Pool>();
    }

    // Long-lived sync-bridge thread for streaming HTTP/SSE (issue #16).
    // See header comment on `bridge_io_` for the rationale.
    bridge_io_ = std::make_unique<asio::io_context>();
    bridge_work_.emplace(asio::make_work_guard(*bridge_io_));

    // Threads come last so resource setup failures unwind normally. If the
    // second thread cannot start, explicitly stop and join the first one;
    // destroying a joinable std::thread would otherwise call std::terminate.
    try {
        http_thread_ = std::thread([io = http_io_.get()]{ io->run(); });
        bridge_thread_ = std::thread([io = bridge_io_.get()]{ io->run(); });
    } catch (...) {
        http_work_.reset();
        bridge_work_.reset();
        http_io_->stop();
        bridge_io_->stop();
        if (http_thread_.joinable()) http_thread_.join();
        if (bridge_thread_.joinable()) bridge_thread_.join();
        throw;
    }
}

SchemaProvider::~SchemaProvider()
{
    // Order: drop the work guards so each io_context.run() can return,
    // stop the io_contexts, then join the worker threads.
    if (http_work_) http_work_.reset();
    if (bridge_work_) bridge_work_.reset();
    if (http_io_) http_io_->stop();
    if (bridge_io_) bridge_io_->stop();
    if (http_thread_.joinable()) http_thread_.join();
    if (bridge_thread_.joinable()) bridge_thread_.join();
}

std::unique_ptr<SchemaProvider>
SchemaProvider::create(const Config& config)
{
    json schema;

    // Check builtin schemas first (e.g., "openai", "claude", "gemini")
    const auto& builtins = builtin::schemas();
    auto it = builtins.find(config.schema_path);
    if (it != builtins.end()) {
        schema = json::parse(it->second);
        return std::unique_ptr<SchemaProvider>(new SchemaProvider(config, std::move(schema)));
    }

    // Fall back to file path
    std::ifstream file(config.schema_path);
    if (!file.is_open()) {
        throw std::runtime_error("SchemaProvider: cannot open schema file: " + config.schema_path);
    }

    try {
        schema = json::parse(file);
    } catch (const json::parse_error& e) {
        throw std::runtime_error("SchemaProvider: invalid JSON in schema file: " + std::string(e.what()));
    }

    return std::unique_ptr<SchemaProvider>(new SchemaProvider(config, std::move(schema)));
}

std::string SchemaProvider::get_name() const {
    return provider_name_;
}

// ============================================================================
// Schema Parsing
// ============================================================================

// Extract the `Retry-After` header value from an httplib response, in
// seconds. Accepts either the numeric-seconds form ("30") or falls back
// to the `x-ratelimit-reset` / `anthropic-ratelimit-*` families. Returns
// -1 when no usable header is present.
//
// HTTP-date form (RFC 7231) is intentionally NOT parsed here — Anthropic
// emits the numeric-seconds form in practice, and HTTP-date parsing
// would drag locale-sensitive strptime into hot path. If you hit a
// server that only emits HTTP-date, add that branch.
static int retry_after_seconds(const httplib::Result& res) {
    if (!res) return -1;
    auto try_int = [](const std::string& v) -> int {
        if (v.empty()) return -1;
        try {
            long n = std::stol(v);
            if (n < 0) return -1;
            if (n > 600) return 600;  // clamp absurd values
            return static_cast<int>(n);
        } catch (...) { return -1; }
    };
    if (res->has_header("Retry-After")) {
        int s = try_int(res->get_header_value("Retry-After"));
        if (s >= 0) return s;
    }
    if (res->has_header("retry-after")) {
        int s = try_int(res->get_header_value("retry-after"));
        if (s >= 0) return s;
    }
    // Anthropic also emits `anthropic-ratelimit-input-tokens-reset` as
    // an ISO-8601 timestamp — skip that path for now; the standard
    // Retry-After header is always present on 429 per their docs.
    return -1;
}

// Parse base_url into host + path prefix
static std::pair<std::string, std::string> split_host_prefix(
    const std::string& base_url) {
    const auto endpoint = async::split_async_endpoint(base_url);
    std::string host = endpoint.tls ? "https://" : "http://";
    if (endpoint.host.find(':') != std::string::npos) {
        host += "[" + endpoint.host + "]";
    } else {
        host += endpoint.host;
    }
    const std::string default_port = endpoint.tls ? "443" : "80";
    if (endpoint.port != default_port) host += ":" + endpoint.port;
    return {std::move(host), endpoint.prefix};
}

// Vendor request id for support tickets: Anthropic sends `request-id`,
// OpenAI and OpenRouter `x-request-id`.
static std::string request_id_of(const async::HttpResponse& response) {
    for (const char* name : {"x-request-id", "request-id"}) {
        const auto value = response.get_header(name);
        if (!value.empty()) return std::string(value);
    }
    return {};
}

static std::string request_id_of(const httplib::Result& res) {
    if (!res) return {};
    for (const char* name : {"x-request-id", "request-id"}) {
        if (res->has_header(name)) return res->get_header_value(name);
    }
    return {};
}

void SchemaProvider::throw_http_failure(int status, const std::string& body,
                                        int retry_after_seconds,
                                        const std::string& request_id) const {
    detail::throw_http_error(status, body, retry_after_seconds, request_id,
                             detail::RetryPolicy{conn_.retryable_statuses,
                                                 conn_.retryable_codes});
}

// Parse Retry-After (seconds-integer shape only, matching the
// httplib-based retry_after_seconds() above). Returns -1 when missing
// or unparsable; clamps absurd values at 600s like the sync path.
static int parse_retry_after_string(std::string_view raw) {
    if (raw.empty()) return -1;
    int seconds = 0;
    auto begin = raw.data();
    auto end = raw.data() + raw.size();
    auto [ptr, ec] = std::from_chars(begin, end, seconds);
    if (ec != std::errc{} || ptr != end || seconds < 0) return -1;
    if (seconds > 600) return 600;
    return seconds;
}

// ============================================================================
// HTTP: complete()
// ============================================================================

// Shared HTTP transport strategy: select the owned pool once per operation,
// bind an operation-local cancellation slot, and surface wire-level failures.
asio::awaitable<async::HttpResponse> SchemaProvider::post_json(
    async::AsyncEndpoint endpoint, std::string path, std::string body,
    std::vector<std::pair<std::string, std::string>> headers,
    int timeout_seconds, std::shared_ptr<graph::CancelToken> cancel_token,
    const char* cancel_context, bool get)
{
    async::RequestOptions opts;
    const int effective_timeout = timeout_seconds > 0
        ? timeout_seconds : user_config_.timeout_seconds;
    if (effective_timeout > 0) {
        opts.timeout = std::chrono::seconds(effective_timeout);
    }
    std::optional<asio::awaitable<async::HttpResponse>> request;
    if (transport_factory_) {
        SchemaPrimitiveRequestContext context;
        context.endpoint = endpoint;
        context.path = path;
        context.body = std::move(body);
        context.headers = std::move(headers);
        context.timeout_seconds = effective_timeout;
        context.get = get;
        if (effective_timeout > 0) {
            context.deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(effective_timeout);
        }
        context.cancellation = cancel_token;
        context.trace_metadata = user_config_.trace_metadata;
        request.emplace(transport_factory_(std::move(context)));
    } else if (get) {
        auto executor = co_await asio::this_coro::executor;
        request.emplace(async::async_get(
            executor, endpoint.host, endpoint.port, path,
            std::move(headers), endpoint.tls, opts));
    } else if (curl_pool_) {
        const std::string default_port = endpoint.tls ? "443" : "80";
        const std::string url_host = endpoint.host.find(':') != std::string::npos
            ? "[" + endpoint.host + "]" : endpoint.host;
        std::string url = (endpoint.tls ? "https://" : "http://") + url_host
                        + (endpoint.port == default_port ? "" : ":" + endpoint.port)
                        + path;
        request.emplace(curl_pool_->async_post(
            std::move(url), std::move(body), std::move(headers), opts));
    } else {
        request.emplace(conn_pool_->async_post(
            endpoint.host, endpoint.port, path, std::move(body),
            std::move(headers), endpoint.tls, opts));
    }

    // Fork: concurrent callers must not replace one another's Asio slot.
    auto executor = co_await asio::this_coro::executor;
    auto operation = cancel_token ? cancel_token->fork()
        : std::shared_ptr<graph::CancelToken>{};
    async::HttpResponse response;
    try {
        if (operation) {
            const auto operation_executor = operation->bind_executor(executor);
            graph::CancelExecutorLease operation_lease(operation);
            co_await asio::post(operation_executor, asio::use_awaitable);
            operation->throw_if_cancelled(cancel_context);
            response = co_await asio::co_spawn(
                operation_executor, std::move(*request),
                asio::bind_cancellation_slot(operation->slot(), asio::use_awaitable));
        } else {
            response = co_await std::move(*request);
        }
    } catch (...) {
        // A transport may report a socket reset while cancellation is
        // concurrently closing the operation. Preserve the graph-level
        // cancellation contract instead of leaking that transport detail.
        if ((operation && operation->is_cancelled())
            || (cancel_token && cancel_token->is_cancelled())) {
            throw asio::system_error(
                asio::error::operation_aborted,
                "SchemaProvider operation cancelled");
        }
        throw;
    }

    if (response.status != 200) {
        throw_http_failure(response.status, response.body,
                           parse_retry_after_string(response.retry_after),
                           request_id_of(response));
    }
    co_return response;
}

asio::awaitable<ChatCompletion>
SchemaProvider::complete_async(const CompletionParams& params)
{
    // Build the request body under the schema lock so concurrent callers
    // don't race on shared yyjson_mut_doc templates. HTTP is issued OUTSIDE
    // the lock so parallel fan-out still overlaps on the wire.
    std::string body_str;
    std::string endpoint_path;
    std::vector<std::pair<std::string, std::string>> headers;
    async::AsyncEndpoint endpoint;
    {
        std::lock_guard<std::mutex> lock(schema_mutex_);
        const std::string api_key = get_api_key();
        endpoint = async::validate_credential_endpoint(
            conn_.base_url,
            !conn_.auth_header.empty() || !conn_.auth_query_param.empty() ||
                !conn_.extra_headers.empty() || !api_key.empty(),
            user_config_.allow_insecure_loopback);
        auto body = build_body(params);
        body_str = body.dump();
        std::string model = params.model.empty() ? user_config_.default_model : params.model;
        endpoint_path = endpoint.prefix + build_endpoint(model, false, api_key);
        for (const auto& [k, v] : build_headers(api_key)) {
            headers.emplace_back(k, v);
        }
        // async_post computes Content-Length from body but does not
        // default Content-Type; httplib used to set it for us.
        bool has_ct = false;
        for (const auto& [k, _] : headers) {
            if (k == "Content-Type" || k == "content-type") { has_ct = true; break; }
        }
        if (!has_ct) headers.emplace_back("Content-Type", "application/json");
    }
    SchemaPrimitiveRequestContext primitive_context;
    primitive_context.endpoint = endpoint;
    primitive_context.path = endpoint_path;
    primitive_context.body = body_str;
    primitive_context.headers = headers;
    primitive_context.timeout_seconds = params.timeout_seconds > 0
        ? params.timeout_seconds : user_config_.timeout_seconds;
    if (primitive_context.timeout_seconds > 0) {
        primitive_context.deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(primitive_context.timeout_seconds);
    }
    primitive_context.cancellation = params.cancel_token;
    primitive_context.trace_metadata = user_config_.trace_metadata;
    auto operation_token = params.cancel_token;
    asio::any_io_executor executor;
    std::chrono::steady_clock::time_point deadline;
    std::optional<asio::steady_timer> deadline_timer;
    const bool long_running = !operation_.id_path.empty();
    if (long_running) {
        const int operation_timeout = params.timeout_seconds > 0
            ? params.timeout_seconds : user_config_.timeout_seconds;
        if (operation_timeout <= 0) {
            throw std::invalid_argument("SchemaProvider: operation requires a positive deadline");
        }
        executor = co_await asio::this_coro::executor;
        deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(operation_timeout);
        operation_token = params.cancel_token ? params.cancel_token->fork()
                                              : std::make_shared<graph::CancelToken>();
        deadline_timer.emplace(executor);
        deadline_timer->expires_at(deadline);
        deadline_timer->async_wait(
            [weak = std::weak_ptr<graph::CancelToken>(operation_token)]
            (const asio::error_code& error) {
                if (!error) {
                    if (auto token = weak.lock()) token->cancel();
                }
            });
    }
    struct StopDeadline {
        std::optional<asio::steady_timer>& timer;
        ~StopDeadline() {
            if (timer) {
                timer->cancel();
            }
        }
    } stop_deadline{deadline_timer};
    auto check_operation = [&] {
        if (params.cancel_token) params.cancel_token->throw_if_cancelled("SchemaProvider operation");
        if (!operation_.id_path.empty() && std::chrono::steady_clock::now() >= deadline) {
            throw OperationTimeoutError("SchemaProvider: operation deadline exceeded");
        }
    };
    check_operation();

    async::HttpResponse res;
    if (execution_factory_) {
        SchemaExecutionContext execution_context;
        execution_context.mode = execution_primitive_name_;
        execution_context.request = primitive_context;
        execution_context.request.cancellation = operation_token;
        execution_context.transport =
            [this](SchemaPrimitiveRequestContext request)
                -> asio::awaitable<async::HttpResponse> {
                co_return co_await post_json(
                    std::move(request.endpoint), std::move(request.path),
                    std::move(request.body), std::move(request.headers),
                    request.timeout_seconds, std::move(request.cancellation),
                    "SchemaProvider custom execution", request.get);
            };
        const json result = co_await execution_factory_(std::move(execution_context));
        res.status = 200;
        res.body = result.dump();
    } else {
        try {
            res = co_await post_json(
                endpoint, endpoint_path, std::move(body_str), headers,
                params.timeout_seconds, operation_token, "SchemaProvider completion entry");
        } catch (const RateLimitError&) {
            check_operation();
            throw;
        } catch (const std::exception& error) {
            if (params.cancel_token && params.cancel_token->is_cancelled()) {
                throw asio::system_error(
                    asio::error::operation_aborted,
                    "SchemaProvider operation cancelled");
            }
            check_operation();
            if (!operation_.id_path.empty()) {
                throw OperationError(std::string("SchemaProvider: submission failed: ") + error.what());
            }
            throw;
        }
    }
    check_operation();
    json resp_json = json::parse(res.body);
    const auto submission_usage = operation_.id_path.empty()
        ? ChatCompletion::Usage{} : parse_usage(resp_json);
    if (!operation_.id_path.empty()) {
        if (!operation_.error_path.empty()) {
            const auto error = json_path::at_path(resp_json, operation_.error_path);
            if (error && !error->is_null()) {
                throw OperationError("SchemaProvider: submission failed: " + error->dump());
            }
        }
        const auto id_value = json_path::at_path(resp_json, operation_.id_path);
        if (!id_value || !id_value->is_string() ||
            id_value->get<std::string>().empty()) {
            throw OperationError("SchemaProvider: missing operation identifier");
        }
        const std::string operation_id = id_value->get<std::string>();
        const std::string api_key = get_api_key();
        auto read_state = [&]() -> bool {
            const auto error = operation_.error_path.empty()
                ? std::optional<json>{}
                : json_path::at_path(resp_json, operation_.error_path);
            if (error && !error->is_null()) {
                throw OperationError("SchemaProvider: operation failed: " + error->dump());
            }
            const auto done = json_path::at_path(resp_json, operation_.done_path);
            if (!done && resp_json.is_object() && operation_.absent_status_pending) {
                return false;
            }
            if (!done || !done->is_boolean()) {
                throw OperationError("SchemaProvider: missing boolean operation status");
            }
            return done->get<bool>();
        };
        while (!read_state()) {
            check_operation();
            auto pause = std::min(
                std::chrono::milliseconds(operation_.poll_interval_ms),
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()));
            if (pause.count() <= 0) throw OperationTimeoutError(
                "SchemaProvider: operation deadline exceeded");
            asio::steady_timer poll_timer(executor);
            poll_timer.expires_after(pause);
            // Short slices make cancellation observable even between HTTP requests.
            while (poll_timer.expiry() > std::chrono::steady_clock::now()) {
                check_operation();
                asio::steady_timer slice(executor);
                slice.expires_after(std::min(std::chrono::milliseconds(50),
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        poll_timer.expiry() - std::chrono::steady_clock::now())));
                co_await slice.async_wait(asio::use_awaitable);
            }
            check_operation();
            const auto poll_path = endpoint.prefix + operation_endpoint(
                operation_.poll_endpoint, operation_id, api_key);
            try {
                res = co_await post_json(endpoint, poll_path, "", headers,
                    std::max(1, static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                        deadline - std::chrono::steady_clock::now()).count())),
                    operation_token, "SchemaProvider operation poll",
                    operation_.poll_method == "GET");
            } catch (const RateLimitError&) {
                check_operation();
                throw;
            } catch (const std::exception& error) {
                check_operation();
                throw OperationError(std::string("SchemaProvider: poll failed: ") + error.what());
            }
            check_operation();
            resp_json = json::parse(res.body);
        }
        if (!operation_.finalize_endpoint.empty()) {
            check_operation();
            const auto final_path = endpoint.prefix + operation_endpoint(
                operation_.finalize_endpoint, operation_id, api_key);
            try {
                res = co_await post_json(endpoint, final_path, "", headers,
                    std::max(1, static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                        deadline - std::chrono::steady_clock::now()).count())),
                    operation_token, "SchemaProvider operation finalize", true);
            } catch (const RateLimitError&) {
                check_operation();
                throw;
            } catch (const std::exception& error) {
                check_operation();
                throw OperationError(std::string("SchemaProvider: finalize failed: ") + error.what());
            }
            check_operation();
            resp_json = json::parse(res.body);
        }
        if (!operation_.result_path.empty()) {
            auto result = json_path::at_path(resp_json, operation_.result_path);
            if (!result) throw OperationError("SchemaProvider: missing operation result");
            resp_json = std::move(*result);
        }
    }

    // parse_response / parse_usage read config strings + walk the freshly
    // parsed resp_json (thread-local). Still holding the lock is cheapest
    // correctness — they don't touch schema_ templates but they do read
    // resp_.*_field members (std::string) which are safe; lock kept for
    // symmetry + to guard against any future edits that introduce template
    // substitution during response parse.
    ChatCompletion completion;
    {
        std::lock_guard<std::mutex> lock(schema_mutex_);
        completion.message = parse_response(resp_json);
        if (!artifact_parser_factory_) {
            completion.artifacts = parse_artifacts(resp_json, &primitive_context);
        }
        completion.usage = operation_.id_path.empty()
            ? parse_usage(resp_json) : submission_usage;
        completion.stop_reason = parse_stop_reason(resp_json);
        if (completion.stop_reason.empty()) {
            completion.stop_reason = completion.message.tool_calls.empty()
                ? resp_.default_stop_reason
                : "tool_use";
        }
    }
    if (artifact_parser_factory_) {
        completion.artifacts = parse_artifacts(resp_json, &primitive_context);
    }
    if (!operation_.id_path.empty() && !resp_.artifacts.empty() &&
        completion.artifacts.empty()) {
        throw OperationError("SchemaProvider: operation returned no mapped artifacts");
    }

    co_return completion;
}

asio::awaitable<json>
SchemaProvider::request_json_async(
    const json& body,
    int timeout_seconds,
    std::shared_ptr<graph::CancelToken> cancel_token)
{
    // OpenRouter's alpha Decisions endpoint contract was verified against
    // the upstream API reference on 2026-09-22:
    // https://openrouter.ai/docs/api/api-reference/alphadecisions/submit-a-decisions-questions-and-answers-request
    // This path deliberately bypasses build_body()/parse_response(). A
    // schema-described endpoint is not necessarily a Chat Completions API;
    // it still gets the same schema-owned connection, authentication,
    // endpoint substitution, pooling, timeout, and cancellation semantics.
    std::string body_str = body.dump();
    std::string endpoint_path;
    std::vector<std::pair<std::string, std::string>> headers;
    async::AsyncEndpoint endpoint;
    {
        std::lock_guard<std::mutex> lock(schema_mutex_);
        const std::string api_key = get_api_key();
        endpoint = async::validate_credential_endpoint(
            conn_.base_url,
            !conn_.auth_header.empty() || !conn_.auth_query_param.empty() ||
                !conn_.extra_headers.empty() || !api_key.empty(),
            user_config_.allow_insecure_loopback);

        // If the schema endpoint contains $MODEL, prefer the model in the
        // caller-owned JSON body; otherwise use the provider default.
        std::string model = user_config_.default_model;
        if (!req_.model_field.empty() && body.is_object() &&
            body.contains(req_.model_field) &&
            body.at(req_.model_field).is_string()) {
            model = body.at(req_.model_field).get<std::string>();
        }
        endpoint_path = endpoint.prefix + build_endpoint(model, false, api_key);
        for (const auto& [k, v] : build_headers(api_key)) {
            headers.emplace_back(k, v);
        }

        bool has_ct = false;
        for (const auto& [k, _] : headers) {
            if (k == "Content-Type" || k == "content-type") {
                has_ct = true;
                break;
            }
        }
        if (!has_ct) headers.emplace_back("Content-Type", "application/json");
    }

    auto response = co_await post_json(
        std::move(endpoint), std::move(endpoint_path), std::move(body_str),
        std::move(headers), timeout_seconds, std::move(cancel_token),
        "SchemaProvider JSON request entry");

    co_return json::parse(response.body);
}

json SchemaProvider::request_json(const json& body, int timeout_seconds)
{
    return async::run_sync(request_json_async(body, timeout_seconds));
}

// ============================================================================
// HTTP: complete_stream()
// ============================================================================

ChatCompletion SchemaProvider::complete_stream(const CompletionParams& params,
                                               const StreamCallback& on_chunk)
{
    if (transport_factory_ || execution_factory_ || !operation_.id_path.empty()) {
        return async::run_sync(complete_stream_async(params, on_chunk));
    }
    // WebSocket mode dispatch — only the openai-responses schema is
    // supported (that's the one OpenAI's WS endpoint speaks). Other
    // providers fall through to the HTTP/SSE path below.
    if (user_config_.use_websocket) {
        if (provider_name_ != "openai-responses") {
            throw std::runtime_error(
                "SchemaProvider: use_websocket is only supported for "
                "the openai-responses schema (got: " + provider_name_ + ")");
        }
        return async::run_sync(
            complete_stream_ws_responses(params, on_chunk),
            params.cancel_token ? params.cancel_token.get() : nullptr);
    }

    return complete_stream_http(params, on_chunk, {});
}

ChatCompletion SchemaProvider::complete_stream_http(
    const CompletionParams& params,
    const StreamCallback& on_chunk,
    const std::shared_ptr<StreamCancelControl>& cancel_control)
{
    if (params.cancel_token) {
        params.cancel_token->throw_if_cancelled("SchemaProvider stream entry");
    }

    // See complete() for the locking rationale. Same pattern: build the
    // request under the schema lock, issue the streaming HTTP call outside.
    std::string body_str;
    std::string endpoint;
    httplib::Headers headers;
    std::string host, prefix;
    {
        std::lock_guard<std::mutex> lock(schema_mutex_);
        const std::string api_key = get_api_key();
        (void)async::validate_credential_endpoint(
            conn_.base_url,
            !conn_.auth_header.empty() || !conn_.auth_query_param.empty() ||
                !conn_.extra_headers.empty() || !api_key.empty(),
            user_config_.allow_insecure_loopback);
        body_str = build_sse_body(params).dump();
        std::string model = params.model.empty() ? user_config_.default_model : params.model;
        std::tie(host, prefix) = split_host_prefix(conn_.base_url);
        endpoint = prefix + build_endpoint(model, true, api_key);
        for (const auto& [k, v] : build_headers(api_key)) {
            headers.emplace(k, v);
        }
    }

    httplib::Client cli(host);
    if (cancel_control) cancel_control->attach(cli);
    struct DetachClient {
        std::shared_ptr<StreamCancelControl> control;
        httplib::Client& client;
        ~DetachClient() {
            if (control) control->detach(client);
        }
    } detach_client{cancel_control, cli};

    if ((cancel_control && cancel_control->is_cancelled()) ||
        (params.cancel_token && params.cancel_token->is_cancelled())) {
        throw neograph::graph::CancelledException("SchemaProvider stream entry");
    }
    const int timeout_seconds = params.timeout_seconds > 0
        ? params.timeout_seconds
        : user_config_.timeout_seconds;
    cli.set_read_timeout(timeout_seconds, 0);
    cli.set_connection_timeout(10, 0);

    StreamParseState state;
    state.completion.message.role = "assistant";
    if (artifact_parser_factory_) {
        auto& context = state.primitive_context;
        context.endpoint = async::split_async_endpoint(conn_.base_url);
        context.path = endpoint;
        context.body = body_str;
        context.headers.assign(headers.begin(), headers.end());
        context.timeout_seconds = timeout_seconds;
        if (timeout_seconds > 0) {
            context.deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(timeout_seconds);
        }
        context.cancellation = params.cancel_token;
        context.trace_metadata = user_config_.trace_metadata;
    }
    std::string line_buffer;
    bool& terminal_event_seen = state.terminal_event_seen;

    int response_status = 0;
    std::string error_body;
    std::size_t response_bytes = 0;
    std::exception_ptr stream_error;
    httplib::Request request;
    request.method = "POST";
    request.path = endpoint;
    request.headers = headers;
    request.body = body_str;
    request.set_header("Content-Type", "application/json");
    request.response_handler = [&](const httplib::Response& response) {
        response_status = response.status;
        return true;
    };
    request.content_receiver =
        [&](const char* data, size_t len, size_t, size_t) -> bool {
            if (len > user_config_.max_stream_response_bytes -
                    std::min(response_bytes,
                             user_config_.max_stream_response_bytes)) {
                stream_error = std::make_exception_ptr(std::length_error(
                    "SchemaProvider stream exceeds configured response limit"));
                return false;
            }
            response_bytes += len;
            if (response_status != 200) {
                constexpr std::size_t kMaxErrorBodyBytes = 64u * 1024u;
                if (len > kMaxErrorBodyBytes -
                        std::min(error_body.size(), kMaxErrorBodyBytes)) {
                    stream_error = std::make_exception_ptr(std::length_error(
                        "SchemaProvider error response exceeds configured limit"));
                    return false;
                }
                error_body.append(data, len);
                return true;
            }
            if ((cancel_control && cancel_control->is_cancelled()) ||
                (params.cancel_token && params.cancel_token->is_cancelled())) {
                return false;
            }
            line_buffer.append(data, len);

            size_t pos;
            while ((pos = line_buffer.find('\n')) != std::string::npos) {
                if ((cancel_control && cancel_control->is_cancelled()) ||
                    (params.cancel_token && params.cancel_token->is_cancelled())) {
                    return false;
                }
                if (pos > user_config_.max_stream_line_bytes) {
                    stream_error = std::make_exception_ptr(std::length_error(
                        "SchemaProvider SSE line exceeds configured limit"));
                    return false;
                }
                std::string line = line_buffer.substr(0, pos);
                line_buffer.erase(0, pos + 1);

                if (!line.empty() && line.back() == '\r') line.pop_back();
                try {
                    if (!consume_stream_line(state, line, on_chunk)) return false;
                } catch (...) {
                    stream_error = std::current_exception();
                    return false;
                }
            }
            if (line_buffer.size() > user_config_.max_stream_line_bytes) {
                stream_error = std::make_exception_ptr(std::length_error(
                    "SchemaProvider SSE line exceeds configured limit"));
                return false;
            }
            return true; // continue receiving
        };

    auto res = cli.send(request);

    if ((cancel_control && cancel_control->is_cancelled()) ||
        (params.cancel_token && params.cancel_token->is_cancelled())) {
        throw neograph::graph::CancelledException("SchemaProvider stream aborted");
    }
    if (stream_error) std::rethrow_exception(stream_error);
    if (response_status != 0 && response_status != 200) {
        throw_http_failure(response_status, error_body,
                           res ? retry_after_seconds(res) : -1, request_id_of(res));
    }
    if (!res && !(terminal_event_seen && response_status == 200 &&
                  res.error() == httplib::Error::Canceled)) {
        throw std::runtime_error("HTTP request failed: " + httplib::to_string(res.error()));
    }

    if (res && res->status != 200) {
        throw_http_failure(res->status, res->body, retry_after_seconds(res),
                           request_id_of(res));
    }

    return finish_stream(state);
}

// ============================================================================
// Async streaming bridge — native override (issue #4)
// ============================================================================

asio::awaitable<ChatCompletion>
SchemaProvider::complete_stream_async(const CompletionParams& params,
                                      const StreamCallback& on_chunk)
{
    // Extension primitives return one complete response, not a stream of wire
    // events. Preserve that selected execution contract and emit its completed
    // text once; a callback must never opt into an unrelated network transport.
    if (transport_factory_ || execution_factory_ || !operation_.id_path.empty()) {
        auto completion = co_await complete_async(params);
        if (params.cancel_token) {
            params.cancel_token->throw_if_cancelled("SchemaProvider callback delivery");
        }
        if (on_chunk && !completion.message.content.empty()) {
            on_chunk(completion.message.content);
        }
        co_return completion;
    }

    auto exec = co_await asio::this_coro::executor;

    // Native fast path for the WebSocket Responses transport: it's
    // already an async-native co_await, so we drop the bridge thread
    // + nested run_sync entirely. Fixes issue #4 for the WS branch.
    if (user_config_.use_websocket && provider_name_ == "openai-responses") {
        if (!params.cancel_token) {
            co_return co_await complete_stream_ws_responses(params, on_chunk);
        }

        auto operation = params.cancel_token->fork();
        const auto operation_executor = operation->bind_executor(exec);
        graph::CancelExecutorLease operation_lease(operation);
        co_await asio::post(operation_executor, asio::use_awaitable);
        operation->throw_if_cancelled("SchemaProvider WebSocket stream entry");
        auto operation_params = params;
        operation_params.cancel_token = operation;
        co_return co_await asio::co_spawn(
            operation_executor,
            complete_stream_ws_responses(operation_params, on_chunk),
            asio::bind_cancellation_slot(operation->slot(), asio::use_awaitable));
    }

    // HTTP/SSE branch (issue #16): dispatch the synchronous
    // `complete_stream` work onto our long-lived `bridge_thread_`
    // instead of letting Provider::complete_stream_async's base
    // default spawn a fresh `std::thread` per call.
    //
    // Why: a fresh thread starts with cold thread-local state in
    // glibc's resolver / NSS plugins / OpenSSL. The first
    // `getaddrinfo` on that thread can SEGV on `internal_strlen` when
    // the cold-init path races with the spawn pattern (observed on
    // some downstream Linux + glibc combinations under nested HTTP
    // server contexts; see #16). Routing through `bridge_thread_`
    // matches the working `complete_async` shape (which lives on
    // `http_thread_`): the thread is warm after the first call, all
    // subsequent calls reuse the warmed state.
    //
    // The bridge thread only writes to shared state. The awaiting
    // coroutine drains queued tokens on its own executor, preserving
    // the callback-thread invariant without letting late bridge work
    // retain or use an executor whose io_context may be gone.
    auto bridge_exec = bridge_io_->get_executor();

    auto cancel_control = std::make_shared<StreamCancelControl>();
    auto operation = params.cancel_token
        ? params.cancel_token->fork()
        : std::shared_ptr<neograph::graph::CancelToken>{};
    graph::CancelExecutorLease operation_lease(operation);
    auto operation_params = params;
    if (operation) {
        const auto operation_executor = operation->bind_executor(exec);
        co_await asio::post(operation_executor, asio::use_awaitable);
        exec = operation_executor;
        operation->throw_if_cancelled("SchemaProvider HTTP stream entry");
        operation_params.cancel_token = operation;
        operation->slot().assign(
            [cancel_control](asio::cancellation_type_t type) {
                if (!type) return;
                cancel_control->cancel();
            });
    }

    struct Shared {
        std::mutex mutex;
        bool abandoned = false;
        bool finished = false;
        std::vector<std::string> chunks;
        std::optional<ChatCompletion> result;
        std::exception_ptr err;
    };
    auto shared = std::make_shared<Shared>();

    StreamCallback wrapped = [shared](const std::string& chunk) {
        std::lock_guard lock(shared->mutex);
        if (!shared->abandoned) shared->chunks.push_back(chunk);
    };

    struct AbandonGuard {
        std::shared_ptr<Shared> shared;
        ~AbandonGuard() {
            std::lock_guard lock(shared->mutex);
            shared->abandoned = true;
            shared->chunks.clear();
        }
    } abandon_guard{shared};

    // params copied by value so the bridge thread's work item doesn't
    // outlive the caller's stack-allocated CompletionParams.
    asio::dispatch(bridge_exec,
        [this, params = std::move(operation_params), wrapped, shared,
         cancel_control]() mutable {
            std::optional<ChatCompletion> result;
            std::exception_ptr err;
            try {
                result = this->complete_stream_http(
                    params, wrapped, cancel_control);
            } catch (...) {
                err = std::current_exception();
            }
            {
                std::lock_guard lock(shared->mutex);
                shared->result = std::move(result);
                shared->err = err;
                shared->finished = true;
            }
        });

    asio::steady_timer poll(exec);
    for (;;) {
        if (operation && operation->is_cancelled()) {
            cancel_control->cancel();
            operation->throw_if_cancelled("SchemaProvider HTTP stream aborted");
        }
        std::vector<std::string> chunks;
        std::optional<ChatCompletion> result;
        std::exception_ptr err;
        bool finished = false;
        {
            std::lock_guard lock(shared->mutex);
            chunks.swap(shared->chunks);
            finished = shared->finished;
            if (finished) {
                result = std::move(shared->result);
                err = shared->err;
            }
        }

        if (on_chunk) {
            for (const auto& chunk : chunks) on_chunk(chunk);
        }
        if (finished) {
            if (operation && operation->is_cancelled()) {
                cancel_control->cancel();
                operation->throw_if_cancelled("SchemaProvider HTTP stream aborted");
            }
            if (err) std::rethrow_exception(err);
            co_return std::move(*result);
        }

        poll.expires_after(std::chrono::milliseconds(1));
        asio::error_code ec;
        co_await poll.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    }
}

// ============================================================================
// WebSocket: complete_stream_ws_responses()
// ============================================================================
//
// OpenAI's WebSocket mode for /v1/responses (per
// developers.openai.com/api/docs/guides/websocket-mode):
//
//   - Connect: wss://<host>/v1/responses with Authorization: Bearer header
//   - Send:    a JSON text frame `{"type":"response.create", ...body}`
//              where ...body mirrors the HTTP Responses request shape
//              (model, input, instructions, tools, ...).
//   - Recv:    the same SSE event payloads, but each one as a discrete
//              text frame (one event per frame). The `event:` SSE prefix
//              is replaced by an inline `"type"` field on the JSON
//              itself, so dispatch is `j["type"]` instead of parsing a
//              separate event line.
//
asio::awaitable<ChatCompletion>
SchemaProvider::complete_stream_ws_responses(const CompletionParams& params,
                                             const StreamCallback& on_chunk)
{
    // Build request body under the schema lock — same pattern as the
    // HTTP path. The lock is released before any I/O.
    json request_body;
    async::AsyncEndpoint endpoint;
    std::string api_key;
    {
        std::lock_guard<std::mutex> lock(schema_mutex_);
        request_body = build_ws_body(params);
        api_key = get_api_key();
        endpoint = async::validate_credential_endpoint(
            conn_.base_url, true,
            user_config_.allow_insecure_loopback);
    }

    std::vector<std::pair<std::string, std::string>> ws_headers = {
        {"Authorization", "Bearer " + api_key},
    };
    // Schema-declared extra headers (rarely set for OpenAI but the
    // contract is that build_headers() reflects them; we apply the
    // same set here, minus auth which we just put in).
    {
        std::lock_guard<std::mutex> lock(schema_mutex_);
        for (const auto& [k, v] : conn_.extra_headers) {
            ws_headers.emplace_back(k, v);
        }
    }
    StreamParseState state;
    state.completion.message.role = "assistant";
    auto dumped = request_body.dump();
    if (artifact_parser_factory_) {
        auto& context = state.primitive_context;
        context.endpoint = endpoint;
        context.path = endpoint.prefix + "/v1/responses";
        context.body = dumped;
        context.headers = ws_headers;
        context.timeout_seconds = params.timeout_seconds > 0
            ? params.timeout_seconds : user_config_.timeout_seconds;
        if (context.timeout_seconds > 0) {
            context.deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(context.timeout_seconds);
        }
        context.cancellation = params.cancel_token;
        context.trace_metadata = user_config_.trace_metadata;
    }

    auto ex = co_await asio::this_coro::executor;
    auto ws = co_await async::ws_connect(
        ex,
        endpoint.host,
        endpoint.port,
        endpoint.prefix.empty() ? std::string("/v1/responses")
                                : (endpoint.prefix + "/v1/responses"),
        std::move(ws_headers),
        endpoint.tls,
        user_config_.websocket_options);

    if (std::getenv("NEOGRAPH_WS_DEBUG")) {
        std::cerr << "[WS DEBUG] sending response.create bytes="
                  << dumped.size() << "\n";
    }
    co_await ws->send_text(dumped);

    bool ws_debug = std::getenv("NEOGRAPH_WS_DEBUG") != nullptr;
    std::size_t ws_response_bytes = 0;
    while (!state.terminal_event_seen) {
        auto msg = co_await ws->recv();
        if (msg.payload.size() >
            user_config_.max_stream_response_bytes -
                std::min(ws_response_bytes,
                         user_config_.max_stream_response_bytes)) {
            throw std::length_error(
                "SchemaProvider WebSocket response exceeds configured limit");
        }
        ws_response_bytes += msg.payload.size();
        if (ws_debug) {
            std::cerr << "[WS DEBUG] recv op=" << static_cast<int>(msg.op)
                      << " bytes=" << msg.payload.size() << "\n";
        }
        if (msg.op == async::WsOpcode::Close) {
            // Server closed before sending response.completed — surface
            // as an error rather than silently returning a partial
            // ChatCompletion (which would be hard to distinguish from
            // a successful empty completion). The Close frame payload
            // is `[uint16 status BE][optional UTF-8 reason]` per RFC
            // 6455 §5.5.1; lift both into the message so auth / quota
            // / model-not-found rejections are debuggable.
            std::string detail;
            int close_code = 0;
            if (msg.payload.size() >= 2) {
                std::uint16_t code =
                    (static_cast<std::uint8_t>(msg.payload[0]) << 8) |
                     static_cast<std::uint8_t>(msg.payload[1]);
                close_code = code;
                detail = " (close=" + std::to_string(code);
                if (msg.payload.size() > 2) {
                    detail += " reason=\"" + msg.payload.substr(2) + "\"";
                }
                detail += ")";
            }
            // A dropped connection (no close code, going away, abnormal,
            // internal error, service restart / overload) is transient; an
            // application-level rejection close code is not.
            const bool transient = close_code == 0 || close_code == 1001 ||
                                   close_code == 1006 || close_code == 1011 ||
                                   close_code == 1012 || close_code == 1013 ||
                                   close_code == 1014;
            throw ProviderError(
                "openai-responses ws: server closed before response.completed" + detail,
                0, transient, "stream_truncated");
        }

        json j;
        try {
            j = json::parse(msg.payload);
        } catch (const json::parse_error&) {
            // Skip malformed events to mirror the HTTP/SSE path's
            // tolerance — the server occasionally sends keep-alive
            // shaped frames that aren't application events.
            continue;
             }

        consume_ws_event(state, j, on_chunk);
    }

    // Polite close so the server doesn't log a transport reset. Drain
    // the close echo to keep the socket state clean.
    try {
        co_await ws->send_close(1000, "done");
        auto echo = co_await ws->recv();
        (void)echo;
    } catch (const std::exception&) {
        // Server may have already closed; ignore.
    }

    co_return finish_stream(state);
}

// Compatibility callback-selected override. It routes through the
// existing native overrides without changing their supported behavior.
// The streaming branch goes to
// `complete_stream_async` not the sync `complete_stream` so the
// WebSocket Responses native-async path + the HTTP/SSE worker-thread
// bridge stay in effect (issue #4 protection).
asio::awaitable<ChatCompletion>
SchemaProvider::invoke(const CompletionParams& params, StreamCallback on_chunk) {
    if (on_chunk) {
        co_return co_await complete_stream_async(params, on_chunk);
    }
    co_return co_await complete_async(params);
}

} // namespace neograph::llm
