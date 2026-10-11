#include "tool_failure.h"

#include <neograph/graph/cancel.h>
#include <neograph/graph/types.h>
#include <neograph/hook_outbox.h>

#include <asio/error.hpp>
#include <asio/system_error.hpp>

namespace neograph::detail {

ToolFailureClass classify_tool_failure(const std::exception_ptr& error,
                                       const graph::CancelToken* run_token) noexcept {
    if (!error) return ToolFailureClass::ToolError;
    try {
        std::rethrow_exception(error);
    } catch (const graph::CancelledException&) {
        return ToolFailureClass::Cancelled;
    } catch (const HookBoundaryBlocked&) {
        return ToolFailureClass::HookBlocked;
    } catch (const graph::NodeInterrupt&) {
        return ToolFailureClass::Interrupted;
    } catch (const asio::system_error& failure) {
        // Socket and timer cancellation reach tool code as operation_aborted.
        // Only the run's own token makes that cancellation of the run.
        if (run_token && run_token->is_cancelled()
            && failure.code() == asio::error::operation_aborted) {
            return ToolFailureClass::Cancelled;
        }
        return ToolFailureClass::ToolError;
    } catch (...) {
        return ToolFailureClass::ToolError;
    }
}

void rethrow_run_stop(const std::exception_ptr& error, ToolFailureClass kind) {
    if (kind == ToolFailureClass::Cancelled) {
        try {
            std::rethrow_exception(error);
        } catch (const graph::CancelledException&) {
            throw;
        } catch (...) {
            throw graph::CancelledException("tool operation aborted");
        }
    }
    std::rethrow_exception(error);
}

std::string tool_failure_message(const std::exception_ptr& error) {
    if (!error) return "unknown tool failure";
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& failure) {
        return failure.what();
    } catch (...) {
        return "unknown tool failure";
    }
}

ToolExecutionResult ordinary_tool_failure(const ToolExecutionPolicy& policy, bool started,
                                          std::string error) {
    const auto uncertain = started
        && policy.effect != ToolEffectClass::ReadOnly
        && policy.effect != ToolEffectClass::ExternalRead
        && policy.idempotency != ToolIdempotency::Idempotent;
    return ToolExecutionResult{
        uncertain ? ToolTerminalStatus::ReconciliationRequired : ToolTerminalStatus::Failed,
        {}, std::move(error), false, uncertain};
}

}  // namespace neograph::detail
