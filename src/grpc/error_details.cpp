// The one mapping from a classified failure to its gRPC status
// (error_details.h). Built only with -DNEOGRAPH_BUILD_GRPC=ON.

#ifdef NEOGRAPH_HAVE_GRPC

#include "error_details.h"

#include "../core/text_safety.h"
#include "../core/tool_failure.h"

#include <neograph/hook_outbox.h>

#include <exception>
#include <utility>

namespace neograph::grpc::detail {

namespace {

bool hook_timed_out(const graph::FailureVerdict& verdict) {
    try {
        if (verdict.signal) std::rethrow_exception(verdict.signal);
    } catch (const HookBoundaryBlocked& blocked) {
        return blocked.state() == HookExecutionState::TimedOut;
    } catch (...) {
    }
    return false;
}

}  // namespace

::grpc::StatusCode status_code_of(const graph::FailureVerdict& verdict) {
    using graph::NodeFailureClass;
    switch (verdict.failure_class) {
    case NodeFailureClass::Cancelled: return ::grpc::StatusCode::CANCELLED;
    case NodeFailureClass::HookBlocked:
        return hook_timed_out(verdict) ? ::grpc::StatusCode::DEADLINE_EXCEEDED
                                       : ::grpc::StatusCode::FAILED_PRECONDITION;
    case NodeFailureClass::Interrupted: return ::grpc::StatusCode::FAILED_PRECONDITION;
    case NodeFailureClass::Timeout: return ::grpc::StatusCode::DEADLINE_EXCEEDED;
    case NodeFailureClass::ProviderFailed: return ::grpc::StatusCode::UNAVAILABLE;
    case NodeFailureClass::BudgetExhausted: return ::grpc::StatusCode::RESOURCE_EXHAUSTED;
    case NodeFailureClass::SafetyBlocked: return ::grpc::StatusCode::FAILED_PRECONDITION;
    case NodeFailureClass::ConfigError: return ::grpc::StatusCode::FAILED_PRECONDITION;
    case NodeFailureClass::LogicError: return ::grpc::StatusCode::INTERNAL;
    case NodeFailureClass::EffectCommitted: return ::grpc::StatusCode::ABORTED;
    case NodeFailureClass::Ordinary:
    case NodeFailureClass::EffectUncertain:
    case NodeFailureClass::Unknown:
        return ::grpc::StatusCode::UNKNOWN;
    }
    return ::grpc::StatusCode::UNKNOWN;
}

WireFailure wire_failure(const graph::FailureVerdict& verdict) {
    WireFailure out;
    out.code = status_code_of(verdict);
    out.message = ::neograph::detail::public_failure_message(verdict);
    out.info.reason = verdict.code.empty() ? graph::node_failure_code(verdict.failure_class)
                                           : verdict.code;
    out.info.domain = std::string(kErrorDomain);
    out.info.metadata["retryable"] = verdict.retryable ? "true" : "false";
    try {
        if (verdict.signal) std::rethrow_exception(verdict.signal);
    } catch (const HookBoundaryBlocked& blocked) {
        out.info.metadata["invocation_id"] = blocked.invocation_id();
        out.info.metadata["state"] = std::string(to_string(blocked.state()));
    } catch (const graph::NodeInterrupt& interrupt) {
        // The interrupt's reason and payload are what the author meant a
        // human to read.
        out.info.metadata["reason"] = ::neograph::detail::valid_utf8(interrupt.reason());
        out.info.metadata["value"] = interrupt.value().dump();
    } catch (...) {
    }
    return out;
}

WireFailure request_failure(::grpc::StatusCode code, std::string reason, std::string message) {
    WireFailure out;
    out.code = code;
    out.message = std::move(message);
    out.info.reason = std::move(reason);
    out.info.domain = std::string(kErrorDomain);
    out.info.metadata["retryable"] = "false";
    return out;
}

}  // namespace neograph::grpc::detail

#endif  // NEOGRAPH_HAVE_GRPC
