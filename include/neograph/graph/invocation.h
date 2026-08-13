/**
 * @file graph/invocation.h
 * @brief Owned protocol-neutral boundary for one graph execution attempt.
 *
 * RunInvocation is intentionally concrete.  It owns the request snapshot,
 * execution identity, cancellation token, ordered event callback, and the
 * terminal classification while leaving transport/session policy to its host.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/engine.h>

#include <asio/awaitable.hpp>

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace neograph::graph {

/** The five terminal outcomes visible to every protocol host. */
enum class InvocationStatus : std::uint8_t {
    Completed,
    Interrupted,
    MaxSteps,
    Cancelled,
    Error,
};

NEOGRAPH_API std::string_view to_string(InvocationStatus status) noexcept;

/**
 * @brief Immutable input snapshot for one RunInvocation.
 *
 * The callback executes inline on the engine's execution path in event order.
 * It MUST be non-blocking and MUST NOT destroy the invocation or re-enter the
 * same invocation.  Hosts that need transport backpressure must queue from the
 * callback and apply their own shutdown policy.
 */
struct RunInvocationRequest {
    RunConfig          config;
    RunMetadata        metadata;
    RunResources       resources;
    GraphStreamCallback on_event;
};

/** Result of one invocation, including a distinct terminal status. */
struct InvocationResult {
    InvocationStatus status = InvocationStatus::Error;
    std::optional<RunResult> run_result;
    std::string error;

    bool succeeded() const noexcept {
        return status == InvocationStatus::Completed ||
               status == InvocationStatus::Interrupted ||
               status == InvocationStatus::MaxSteps;
    }
    bool cancelled() const noexcept { return status == InvocationStatus::Cancelled; }
};

/**
 * @brief Owns one Core graph invocation and its cancellation lifetime.
 *
 * A RunInvocation is single-use: call exactly one of run/resume/resume_from
 * (sync or async).  The exposed token remains valid until the invocation is
 * destroyed, allowing protocol cancellation handlers to race the execution
 * safely.  RunConfig, metadata, resources, and callbacks are copied/moved into
 * the object before execution starts.
 */
class NEOGRAPH_API RunInvocation final {
public:
    RunInvocation(std::shared_ptr<GraphEngine> engine,
                  RunInvocationRequest request);

    RunInvocation(const RunInvocation&) = delete;
    RunInvocation& operator=(const RunInvocation&) = delete;
    RunInvocation(RunInvocation&&) = delete;
    RunInvocation& operator=(RunInvocation&&) = delete;
    ~RunInvocation() = default;

    const RunInvocationRequest& request() const noexcept { return request_; }
    std::shared_ptr<CancelToken> cancel_token() const noexcept { return token_; }
    void cancel() noexcept { token_->cancel(); }

    InvocationResult run();
    asio::awaitable<InvocationResult> run_async();

    InvocationResult resume(json resume_value = {});
    asio::awaitable<InvocationResult> resume_async(json resume_value = {});

    InvocationResult resume_from(std::string checkpoint_id,
                                 json resume_value = {});
    asio::awaitable<InvocationResult> resume_from_async(
        std::string checkpoint_id, json resume_value = {});

private:
    std::shared_ptr<GraphEngine> engine_;
    RunInvocationRequest request_;
    std::shared_ptr<CancelToken> token_;
    std::atomic_bool started_{false};

    void validate_request() const;
    bool try_start() noexcept;
    InvocationResult failure(std::string message) const;
    static InvocationResult classify(RunResult result);
    static InvocationResult classify_exception(std::exception_ptr error,
                                               bool cancelled);
};

}  // namespace neograph::graph
