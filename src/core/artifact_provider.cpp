#include <neograph/artifact_provider.h>

#include <neograph/async/run_sync.h>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/co_spawn.hpp>
#include <asio/error.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace neograph {
namespace {

constexpr std::uint32_t kMaximumPollAttempts = 1'000'000;
constexpr auto kMaximumPollInterval = std::chrono::hours{24};

bool is_nonempty_text(const std::string_view value) noexcept {
    return !value.empty()
        && value.find('\0') == std::string_view::npos;
}

void require_text(const std::string_view value, const std::string_view field) {
    if (!is_nonempty_text(value)) {
        throw std::invalid_argument(std::string(field) + " must be non-empty and contain no NUL bytes");
    }
}

void validate_artifact(const Artifact& artifact) {
    require_text(artifact.identity, "Artifact identity");
    require_text(artifact.uri, "Artifact uri");
    require_text(artifact.media_type, "Artifact media_type");
    if (!artifact.metadata.is_object()) {
        throw std::invalid_argument("Artifact metadata must be an object");
    }
}

void validate_request(const ArtifactRequest& request) {
    require_text(request.operation, "Artifact request operation");
    if (!request.input.is_object()) {
        throw std::invalid_argument("Artifact request input must be an object");
    }
    if (!request.idempotency_key.empty()) {
        require_text(request.idempotency_key, "Artifact request idempotency_key");
    }
    if (request.mode != ArtifactExecutionMode::LongRunning) return;
    if (request.poll.max_attempts == 0 || request.poll.max_attempts > kMaximumPollAttempts) {
        throw std::invalid_argument("Artifact long-running max_attempts must be in [1, 1000000]");
    }
    if (request.poll.interval.count() < 0 || request.poll.interval > kMaximumPollInterval) {
        throw std::invalid_argument("Artifact long-running poll interval must be in [0ms, 24h]");
    }
}

void validate_nonterminal_operation(const ArtifactOperation& operation) {
    if (operation.terminal()) return;
    require_text(operation.id, "Non-terminal artifact operation id");
}

std::string required_string(const json& value, const std::string_view field) {
    if (!value.contains(std::string(field)) || !value.at(std::string(field)).is_string()) {
        throw std::invalid_argument("Artifact JSON field '" + std::string(field) + "' must be a string");
    }
    const auto result = value.at(std::string(field)).get<std::string>();
    require_text(result, "Artifact JSON string");
    return result;
}

std::uint64_t required_unsigned(const json& value, const std::string_view field) {
    if (!value.contains(std::string(field)) || !value.at(std::string(field)).is_number_unsigned()) {
        throw std::invalid_argument("Artifact JSON field '" + std::string(field) + "' must be an unsigned integer");
    }
    return value.at(std::string(field)).get<std::uint64_t>();
}

void reject_unknown_artifact_fields(const json& value) {
    constexpr std::array<std::string_view, 5> kAllowed = {
        "identity", "uri", "media_type", "size_bytes", "metadata"};
    for (const auto& [field, ignored] : value.items()) {
        (void)ignored;
        if (std::find(kAllowed.begin(), kAllowed.end(), field) == kAllowed.end()) {
            throw std::invalid_argument("Unknown Artifact JSON field '" + field + "'");
        }
    }
}

} // namespace

std::string_view to_string(const ArtifactExecutionMode mode) noexcept {
    switch (mode) {
        case ArtifactExecutionMode::OneShot: return "one_shot";
        case ArtifactExecutionMode::Stream: return "stream";
        case ArtifactExecutionMode::LongRunning: return "long_running";
    }
    return "unknown";
}

std::string_view to_string(const ArtifactOperationStatus status) noexcept {
    switch (status) {
        case ArtifactOperationStatus::Queued: return "queued";
        case ArtifactOperationStatus::Running: return "running";
        case ArtifactOperationStatus::Succeeded: return "succeeded";
        case ArtifactOperationStatus::Failed: return "failed";
        case ArtifactOperationStatus::Cancelled: return "cancelled";
        case ArtifactOperationStatus::TimedOut: return "timed_out";
    }
    return "unknown";
}

std::string_view to_string(const ArtifactEventKind kind) noexcept {
    switch (kind) {
        case ArtifactEventKind::Progress: return "progress";
        case ArtifactEventKind::Artifact: return "artifact";
        case ArtifactEventKind::State: return "state";
    }
    return "unknown";
}

bool ArtifactOperation::terminal() const noexcept {
    return status == ArtifactOperationStatus::Succeeded
        || status == ArtifactOperationStatus::Failed
        || status == ArtifactOperationStatus::Cancelled
        || status == ArtifactOperationStatus::TimedOut;
}

bool ArtifactOperation::succeeded() const noexcept {
    return status == ArtifactOperationStatus::Succeeded;
}

bool ArtifactOperation::failed() const noexcept {
    return terminal() && !succeeded();
}

void to_json(json& value, const Artifact& artifact) {
    validate_artifact(artifact);
    value = json{{"identity", artifact.identity},
                 {"uri", artifact.uri},
                 {"media_type", artifact.media_type},
                 {"size_bytes", artifact.size_bytes},
                 {"metadata", artifact.metadata}};
}

void from_json(const json& value, Artifact& artifact) {
    if (!value.is_object()) {
        throw std::invalid_argument("Artifact JSON must be an object");
    }
    reject_unknown_artifact_fields(value);
    if (!value.contains("metadata") || !value.at("metadata").is_object()) {
        throw std::invalid_argument("Artifact JSON field 'metadata' must be an object");
    }

    Artifact parsed{
        .identity = required_string(value, "identity"),
        .uri = required_string(value, "uri"),
        .media_type = required_string(value, "media_type"),
        .size_bytes = required_unsigned(value, "size_bytes"),
        .metadata = value.at("metadata"),
    };
    validate_artifact(parsed);
    artifact = std::move(parsed);
}

ArtifactProvider::~ArtifactProvider() = default;

ArtifactOperation ArtifactProvider::execute(ArtifactRequest request,
                                            ArtifactEventCallback on_event) {
    auto* cancel = request.cancel_token ? request.cancel_token.get() : nullptr;
    return async::run_sync(execute_async(std::move(request), std::move(on_event)), cancel);
}

asio::awaitable<ArtifactOperation>
ArtifactProvider::execute_async(ArtifactRequest request, ArtifactEventCallback on_event) {
    if (!request.cancel_token) {
        co_return co_await execute_unbound(std::move(request), std::move(on_event));
    }

    auto caller_executor = co_await asio::this_coro::executor;
    auto operation = request.cancel_token->fork();
    const auto operation_executor = operation->bind_executor(caller_executor);
    graph::CancelExecutorLease operation_lease(operation);

    co_await asio::post(operation_executor, asio::use_awaitable);
    operation->throw_if_cancelled("Artifact operation entry");
    request.cancel_token = operation;

    try {
        co_return co_await asio::co_spawn(
            operation_executor,
            execute_unbound(std::move(request), std::move(on_event)),
            asio::bind_cancellation_slot(operation->slot(), asio::use_awaitable));
    } catch (const asio::system_error& error) {
        if (operation->is_cancelled() && error.code() == asio::error::operation_aborted) {
            throw graph::CancelledException("Artifact operation cancelled");
        }
        throw;
    }
}

asio::awaitable<ArtifactOperation>
ArtifactProvider::execute_unbound(ArtifactRequest request, ArtifactEventCallback on_event) {
    validate_request(request);
    if (request.cancel_token) {
        request.cancel_token->throw_if_cancelled("Artifact operation entry");
    }

    // Retain only the generic lifecycle controls before moving the provider
    // request. This avoids copying an arbitrarily large structured input for
    // every poll while still making submit own the complete request envelope.
    const auto mode = request.mode;
    const auto poll = request.poll;
    auto cancel = request.cancel_token;
    auto operation = co_await do_start(std::move(request), std::move(on_event));

    if (mode != ArtifactExecutionMode::LongRunning) {
        if (!operation.terminal()) {
            throw std::logic_error(
                "One-shot and stream artifact providers must return a terminal operation");
        }
        co_return operation;
    }

    if (operation.terminal()) {
        co_return operation;  // Fast LRO completion; no needless poll.
    }
    validate_nonterminal_operation(operation);

    asio::steady_timer timer(co_await asio::this_coro::executor);
    for (std::uint32_t attempt = 0; attempt < poll.max_attempts; ++attempt) {
        if (cancel) cancel->throw_if_cancelled("Artifact operation poll wait");
        timer.expires_after(poll.interval);
        co_await timer.async_wait(asio::use_awaitable);
        if (cancel) cancel->throw_if_cancelled("Artifact operation poll");

        operation = co_await do_poll(std::move(operation));
        if (operation.terminal()) {
            co_return operation;
        }
        validate_nonterminal_operation(operation);
    }

    // A poll limit is a terminal local outcome, never a silent success. Keep
    // the provider's last operation payload so callers can inspect its ID and
    // reported progress before deciding whether to resume externally.
    operation.status = ArtifactOperationStatus::TimedOut;
    operation.error_code = "POLL_LIMIT_EXCEEDED";
    operation.error_message = "Artifact operation did not reach a terminal state within the configured poll bound";
    co_return operation;
}

} // namespace neograph
