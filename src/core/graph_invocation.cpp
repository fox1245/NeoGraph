#include <neograph/graph/invocation.h>

#include <neograph/async/run_sync.h>

#include <exception>
#include <stdexcept>
#include <utility>

namespace neograph::graph {

std::string_view to_string(InvocationStatus status) noexcept {
    switch (status) {
        case InvocationStatus::Completed: return "completed";
        case InvocationStatus::Interrupted: return "interrupted";
        case InvocationStatus::MaxSteps: return "max_steps";
        case InvocationStatus::Cancelled: return "cancelled";
        case InvocationStatus::Error: return "error";
    }
    return "error";
}

RunInvocation::RunInvocation(std::shared_ptr<GraphEngine> engine,
                             RunInvocationRequest request)
    : engine_(std::move(engine)), request_(std::move(request)) {
    validate_request();
    token_ = request_.config.cancel_token;
    if (!token_) token_ = std::make_shared<CancelToken>();
    request_.config.cancel_token = token_;
    if (request_.metadata.run_id.empty()) {
        // A protocol host may provide a durable identity.  The thread id is a
        // safe deterministic fallback for legacy hosts; it is only an
        // execution-local correlator and does not alter checkpoint identity.
        request_.metadata.run_id = request_.config.thread_id;
    }
}

void RunInvocation::validate_request() const {
    if (!engine_) throw std::invalid_argument("RunInvocation requires a GraphEngine");
    if (request_.config.thread_id.empty())
        throw std::invalid_argument("RunInvocation requires a non-empty thread_id");
    if (request_.config.max_steps <= 0)
        throw std::invalid_argument("RunInvocation max_steps must be positive");
}

bool RunInvocation::try_start() noexcept {
    bool expected = false;
    return started_.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel,
        std::memory_order_acquire);
}

InvocationResult RunInvocation::failure(std::string message) const {
    InvocationResult result;
    result.status = token_ && token_->is_cancelled()
        ? InvocationStatus::Cancelled
        : InvocationStatus::Error;
    result.error = std::move(message);
    return result;
}

InvocationResult RunInvocation::classify(RunResult result) {
    InvocationResult outcome;
    outcome.run_result = std::move(result);
    switch (outcome.run_result->status()) {
        case RunStatus::Completed:
            outcome.status = InvocationStatus::Completed;
            break;
        case RunStatus::Interrupted:
            outcome.status = InvocationStatus::Interrupted;
            break;
        case RunStatus::StepLimit:
            outcome.status = InvocationStatus::MaxSteps;
            break;
    }
    return outcome;
}

InvocationResult RunInvocation::classify_exception(std::exception_ptr error,
                                                   bool cancelled) {
    if (cancelled) {
        InvocationResult result;
        result.status = InvocationStatus::Cancelled;
        try {
            if (error) std::rethrow_exception(error);
        } catch (const std::exception& e) {
            result.error = e.what();
        } catch (...) {
            result.error = "run cancelled";
        }
        return result;
    }

    InvocationResult result;
    result.status = InvocationStatus::Error;
    try {
        if (error) std::rethrow_exception(error);
    } catch (const std::exception& e) {
        result.error = e.what();
    } catch (...) {
        result.error = "unknown graph invocation failure";
    }
    return result;
}

InvocationResult RunInvocation::run() {
    try {
        return neograph::async::run_sync(run_async(), token_.get());
    } catch (...) {
        return classify_exception(std::current_exception(),
                                  token_->is_cancelled());
    }
}

asio::awaitable<InvocationResult> RunInvocation::run_async() {
    if (!try_start()) {
        co_return failure("RunInvocation is single-use");
    }
    try {
        auto result = co_await engine_->run_stream_async(
            request_.config, request_.on_event, request_.metadata,
            request_.resources);
        co_return classify(std::move(result));
    } catch (...) {
        co_return classify_exception(std::current_exception(),
                                     token_->is_cancelled());
    }
}

InvocationResult RunInvocation::resume(json resume_value) {
    try {
        return neograph::async::run_sync(
            resume_async(std::move(resume_value)), token_.get());
    } catch (...) {
        return classify_exception(std::current_exception(),
                                  token_->is_cancelled());
    }
}

asio::awaitable<InvocationResult>
RunInvocation::resume_async(json resume_value) {
    if (!try_start()) {
        co_return failure("RunInvocation is single-use");
    }
    try {
        auto result = co_await engine_->resume_async(
            request_.config, std::move(resume_value), request_.on_event,
            request_.metadata, request_.resources);
        co_return classify(std::move(result));
    } catch (...) {
        co_return classify_exception(std::current_exception(),
                                     token_->is_cancelled());
    }
}

InvocationResult RunInvocation::resume_from(std::string checkpoint_id,
                                            json resume_value) {
    try {
        return neograph::async::run_sync(
            resume_from_async(std::move(checkpoint_id), std::move(resume_value)),
            token_.get());
    } catch (...) {
        return classify_exception(std::current_exception(),
                                  token_->is_cancelled());
    }
}

asio::awaitable<InvocationResult>
RunInvocation::resume_from_async(std::string checkpoint_id,
                                 json resume_value) {
    if (!try_start()) {
        co_return failure("RunInvocation is single-use");
    }
    try {
        auto result = co_await engine_->resume_from_async(
            request_.config, std::move(checkpoint_id), std::move(resume_value),
            request_.on_event, request_.metadata, request_.resources);
        co_return classify(std::move(result));
    } catch (...) {
        co_return classify_exception(std::current_exception(),
                                     token_->is_cancelled());
    }
}

}  // namespace neograph::graph
