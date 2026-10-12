// The one mapping from a classified failure to its gRPC status
// (error_details.h). Built only with -DNEOGRAPH_BUILD_GRPC=ON.

#ifdef NEOGRAPH_HAVE_GRPC

#include "error_details.h"

#include "../core/text_safety.h"
#include "../core/tool_failure.h"

#include <neograph/hook_outbox.h>

#include <algorithm>
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

bool valid_reason(std::string_view reason) {
    // AIP-193: [A-Z][A-Z0-9_]+[A-Z0-9], at most 63 characters.
    if (reason.size() < 2 || reason.size() > 63) return false;
    if (reason.front() < 'A' || reason.front() > 'Z' || reason.back() == '_') return false;
    for (const char c : reason)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

// "HookBlocked" -> "HOOK_BLOCKED"; an uppercase letter that follows a
// lowercase letter or a digit starts a new word.
std::string upper_snake(std::string_view name) {
    std::string out;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        const bool upper = c >= 'A' && c <= 'Z';
        if (upper && i > 0) {
            const char previous = name[i - 1];
            if ((previous >= 'a' && previous <= 'z') || (previous >= '0' && previous <= '9'))
                out.push_back('_');
        }
        if (c >= 'a' && c <= 'z') out.push_back(static_cast<char>(c - 'a' + 'A'));
        else if (upper || (c >= '0' && c <= '9')) out.push_back(c);
        else out.push_back('_');
    }
    return out;
}

constexpr std::string_view kNgPrefix = "NG.";

}  // namespace

std::string error_reason(std::string_view code, graph::NodeFailureClass failure_class) {
    std::string_view source = code;
    if (source.substr(0, kNgPrefix.size()) != kNgPrefix)
        source = graph::node_failure_code(failure_class);  // an author code: its class
    auto reason = upper_snake(source.substr(std::min(kNgPrefix.size(), source.size())));
    return valid_reason(reason) ? reason : std::string("UNKNOWN_FAILURE");
}

std::string error_code_of(const ErrorInfo& info) {
    if (const auto found = info.metadata.find("code"); found != info.metadata.end())
        return found->second;
    if (info.reason.substr(0, kNgPrefix.size()) == kNgPrefix) return info.reason;
    return {};
}

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
    const std::string code = verdict.code.empty()
        ? std::string(graph::node_failure_code(verdict.failure_class)) : verdict.code;
    out.info.reason = error_reason(code, verdict.failure_class);
    out.info.domain = std::string(kErrorDomain);
    out.info.metadata["code"] = code;
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

WireFailure request_failure(::grpc::StatusCode code, std::string ng_code, std::string message) {
    WireFailure out;
    out.code = code;
    out.message = std::move(message);
    out.info.reason = error_reason(ng_code, graph::NodeFailureClass::ConfigError);
    out.info.domain = std::string(kErrorDomain);
    out.info.metadata["code"] = std::move(ng_code);
    out.info.metadata["retryable"] = "false";
    return out;
}

}  // namespace neograph::grpc::detail

#endif  // NEOGRAPH_HAVE_GRPC
