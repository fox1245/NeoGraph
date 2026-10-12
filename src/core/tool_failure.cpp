#include "tool_failure.h"

#include "text_safety.h"

#include <neograph/graph/cancel.h>
#include <neograph/graph/types.h>
#include <neograph/hook_outbox.h>
#include <neograph/provider.h>

#include <asio/error.hpp>
#include <asio/system_error.hpp>

#include <utility>
#include <variant>
#include <vector>

namespace neograph::detail {

ToolFailureClass classify_tool_failure(const std::exception_ptr& error,
                                       const graph::CancelToken* run_token) noexcept {
    // The tool failure contract is the node failure classifier: a run control
    // class anywhere in the cause chain stops the run, everything else is a
    // tool message the model sees.
    switch (classify_node_failure(error, run_token).failure_class) {
    case graph::NodeFailureClass::Cancelled: return ToolFailureClass::Cancelled;
    case graph::NodeFailureClass::HookBlocked: return ToolFailureClass::HookBlocked;
    case graph::NodeFailureClass::Interrupted: return ToolFailureClass::Interrupted;
    default: return ToolFailureClass::ToolError;
    }
}

namespace {
std::exception_ptr nested_of(const std::exception& error) noexcept;
}  // namespace

// The first exception of the signal's own type in @p error's cause chain
// (NodeExecutionError causes and std::nested_exception, depth first, outermost
// first), or null.
template <typename Signal>
std::exception_ptr find_signal(const std::exception_ptr& error) noexcept {
    std::vector<std::pair<std::exception_ptr, int>> pending;
    try {
        pending.emplace_back(error, 0);
        while (!pending.empty()) {
            auto [current, depth] = std::move(pending.back());
            pending.pop_back();
            if (!current || depth > 64) continue;
            std::exception_ptr cause;
            std::exception_ptr nested;
            try {
                std::rethrow_exception(current);
            } catch (const Signal&) {
                return current;
            } catch (const graph::NodeExecutionError& failure) {
                cause = failure.cause();
                nested = nested_of(failure);
            } catch (const std::exception& failure) {
                nested = nested_of(failure);
            } catch (...) {
            }
            if (nested) pending.emplace_back(std::move(nested), depth + 1);
            if (cause) pending.emplace_back(std::move(cause), depth + 1);
        }
    } catch (...) {
    }
    return {};
}

void rethrow_run_stop(const std::exception_ptr& error, ToolFailureClass kind) {
    // The run stops with the control signal itself, whatever wrapped it on
    // the way out (a subgraph's NodeExecutionError, a nested exception): one
    // type, with its fields, on every path (reference, "Tool failure
    // contract").
    switch (kind) {
    case ToolFailureClass::Cancelled:
        if (auto signal = find_signal<graph::CancelledException>(error))
            std::rethrow_exception(signal);
        // An aborted asio operation of a cancelled run.
        throw graph::CancelledException("tool operation aborted");
    case ToolFailureClass::HookBlocked:
        if (auto signal = find_signal<HookBoundaryBlocked>(error)) std::rethrow_exception(signal);
        break;
    case ToolFailureClass::Interrupted:
        if (auto signal = find_signal<graph::NodeInterrupt>(error)) std::rethrow_exception(signal);
        break;
    case ToolFailureClass::ToolError:
        break;
    }
    std::rethrow_exception(error);
}

std::string tool_failure_message(const std::exception_ptr& error) {
    if (!error) return "unknown tool failure";
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& failure) {
        return valid_utf8(failure.what());
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

std::string public_failure_message(const graph::FailureVerdict& verdict) {
    if (!verdict.public_message.empty()) return valid_utf8(verdict.public_message);
    switch (verdict.failure_class) {
    case graph::NodeFailureClass::Ordinary: return "the operation failed";
    case graph::NodeFailureClass::Timeout: return "the operation timed out";
    case graph::NodeFailureClass::ProviderFailed: return "a model provider call failed";
    case graph::NodeFailureClass::EffectCommitted:
        return "the operation failed after committing an effect";
    case graph::NodeFailureClass::LogicError: return "internal error";
    case graph::NodeFailureClass::ConfigError: return "the request or configuration is invalid";
    case graph::NodeFailureClass::SafetyBlocked: return "a provider safety filter stopped the run";
    case graph::NodeFailureClass::BudgetExhausted: return "a budget was exhausted";
    case graph::NodeFailureClass::EffectUncertain: return "the outcome of an effect is unknown";
    case graph::NodeFailureClass::Unknown: return "unknown failure";
    case graph::NodeFailureClass::Interrupted: return "the run needs input";
    case graph::NodeFailureClass::HookBlocked: return "a lifecycle Hook blocked the operation";
    case graph::NodeFailureClass::Cancelled: return "cancelled";
    }
    return "unknown failure";
}

namespace {

using graph::NodeFailureClass;

// A chain longer than this, or with more members than kMaxCauses, is not
// classified: it fails closed as Unknown.
constexpr int kMaxCauseDepth = 64;
constexpr int kMaxCauses = 256;

bool cancelled(const graph::CancelToken* run_token) noexcept {
    return run_token && run_token->is_cancelled();
}

NodeFailureClass provider_failure_class(const ProviderFailure& failure,
                                        const graph::CancelToken* run_token) noexcept {
    const auto& outcome = failure.outcome();
    const auto* failed = outcome ? std::get_if<sp::Failure>(outcome.get()) : nullptr;
    if (!failed) return NodeFailureClass::Unknown;
    // A safety terminal is a block whatever transport status came with it.
    if (const auto& stop = failed->partial.stop;
        stop && (stop->kind == sp::StopKind::ContentFilter || stop->kind == sp::StopKind::Refusal))
        return NodeFailureClass::SafetyBlocked;
    const auto& error = failed->error;
    switch (error.kind) {
    // Transient: the transport, a timeout, the server, overload, rate limits.
    // Truncated is how the transport reports a response that ended abnormally.
    case sp::ErrorKind::Transport:
    case sp::ErrorKind::Truncated:
    case sp::ErrorKind::DeadlineExceeded:
    case sp::ErrorKind::Overloaded:
    case sp::ErrorKind::RateLimited:
        return NodeFailureClass::ProviderFailed;
    case sp::ErrorKind::RemoteFailure:
        // Only a server error status proves a server failure. In-band remote
        // failures also carry safety and policy rejections.
        return error.http_status >= 500 && error.http_status <= 599
            ? NodeFailureClass::ProviderFailed : NodeFailureClass::Unknown;
    case sp::ErrorKind::QuotaExhausted:
    case sp::ErrorKind::ResourceLimit:
        return NodeFailureClass::BudgetExhausted;
    case sp::ErrorKind::InvalidConfig:
    case sp::ErrorKind::InvalidRequest:
    case sp::ErrorKind::Unsupported:
    case sp::ErrorKind::Misuse:
    case sp::ErrorKind::ReplayIneligible:
    case sp::ErrorKind::Authentication:
    case sp::ErrorKind::Permission:
    case sp::ErrorKind::NotFound:
    case sp::ErrorKind::LimitUnknown:
        return NodeFailureClass::ConfigError;
    case sp::ErrorKind::Cancelled:
        // Like an aborted socket: cancellation only when the run's own token
        // asked for it.
        return cancelled(run_token) ? NodeFailureClass::Cancelled : NodeFailureClass::Unknown;
    case sp::ErrorKind::ProtocolCorrupt:
        return NodeFailureClass::Unknown;
    }
    return NodeFailureClass::Unknown;
}

std::exception_ptr nested_of(const std::exception& error) noexcept {
    if (const auto* nested = dynamic_cast<const std::nested_exception*>(&error))
        return nested->nested_ptr();
    return {};
}

class CauseWalker {
public:
    explicit CauseWalker(const graph::CancelToken* run_token) : run_token_(run_token) {}

    NodeFailureVerdict walk(const std::exception_ptr& error) noexcept {
        try {
            std::vector<std::pair<std::exception_ptr, int>> pending;
            pending.emplace_back(error, 0);
            int visited = 0;
            while (!pending.empty()) {
                auto [current, depth] = std::move(pending.back());
                pending.pop_back();
                if (!current) continue;
                if (depth > kMaxCauseDepth || ++visited > kMaxCauses) {
                    found(NodeFailureClass::Unknown);
                    break;
                }
                // Visit the cause of an exception before what it nests, and
                // both after the exception itself: on a tie the outermost
                // finding names the failure.
                auto [cause, nested] = visit(current);
                if (nested) pending.emplace_back(std::move(nested), depth + 1);
                if (cause) pending.emplace_back(std::move(cause), depth + 1);
            }
        } catch (...) {
            found(NodeFailureClass::Unknown);
        }
        NodeFailureVerdict verdict;
        verdict.failure_class = have_ ? class_ : NodeFailureClass::Unknown;
        verdict.code = code_.empty() ? graph::node_failure_code(verdict.failure_class) : code_;
        verdict.public_message = std::move(public_message_);
        verdict.retryable = graph::node_failure_retryable(verdict.failure_class) && !not_retryable_;
        verdict.routable = graph::node_failure_routable(verdict.failure_class);
        return verdict;
    }

private:
    // Records one finding; the most severe class wins, the first one on a tie.
    void found(NodeFailureClass failure_class, std::string code = {}, std::string message = {}) {
        if (have_ && failure_class <= class_) return;
        have_ = true;
        class_ = failure_class;
        code_ = std::move(code);
        public_message_ = std::move(message);
    }

    // Classifies one exception and returns its cause and nested exception.
    std::pair<std::exception_ptr, std::exception_ptr> visit(const std::exception_ptr& error) {
        try {
            std::rethrow_exception(error);
        } catch (const graph::CancelledException& failure) {
            found(NodeFailureClass::Cancelled);
            return {{}, nested_of(failure)};
        } catch (const HookBoundaryBlocked&) {
            found(NodeFailureClass::HookBlocked);
        } catch (const graph::NodeInterrupt& failure) {
            found(NodeFailureClass::Interrupted);
            return {{}, nested_of(failure)};
        } catch (const graph::NodeAttemptTimeout& failure) {
            // An attempt that did not stop after its timeout may still be
            // performing effects.
            found(failure.stopped() ? NodeFailureClass::Timeout : NodeFailureClass::EffectUncertain);
            return {{}, nested_of(failure)};
        } catch (const graph::ConfigurationFailure& failure) {
            // A configuration or definition verdict, such as a Program
            // diagnostic: the same input reproduces it.
            found(NodeFailureClass::ConfigError);
            if (const auto* nested = dynamic_cast<const std::nested_exception*>(&failure))
                return {{}, nested->nested_ptr()};
        } catch (const graph::NodeFailureError& failure) {
            found(NodeFailureClass::Ordinary, failure.code(), failure.public_message());
            if (failure.retryable() == false) not_retryable_ = true;
            return {{}, nested_of(failure)};
        } catch (const graph::NodeExecutionError& failure) {
            // A node failure raised inside this node: a subgraph. Its record,
            // when it has one, includes what its executor saw outside the
            // exception (committed or uncertain effects).
            if (const auto* record = graph::node_failure_of(failure);
                record && record->failure_class != NodeFailureClass::Ordinary) {
                found(record->failure_class, record->code, record->public_message);
            } else {
                found(NodeFailureClass::Ordinary, "NG.SubgraphFailed");
                if (record && !record->retryable) not_retryable_ = true;
            }
            return {failure.cause(), nested_of(failure)};
        } catch (const ProviderOutcomeError& failure) {
            // The provider effect drained, but its outcome was not settled.
            found(NodeFailureClass::EffectUncertain);
            return {failure.cause(), nested_of(failure)};
        } catch (const ToolExecutionError& failure) {
            // The typed result of the throwing controller facade. A call whose
            // effect may have happened is never retried; one that ran out of
            // time is a timeout.
            const auto& result = failure.result();
            if (result.effect_uncertain || result.status == ToolTerminalStatus::ReconciliationRequired)
                found(NodeFailureClass::EffectUncertain);
            else if (result.status == ToolTerminalStatus::TimedOut ||
                     result.status == ToolTerminalStatus::Expired)
                found(NodeFailureClass::Timeout);
            else
                found(NodeFailureClass::Ordinary);
            return {{}, nested_of(failure)};
        } catch (const ProviderFailure& failure) {
            found(provider_failure_class(failure, run_token_));
        } catch (const asio::system_error& failure) {
            // Socket and timer cancellation reach node and tool code as
            // operation_aborted. Only the run's own token makes that
            // cancellation of the run.
            // A timed-out operation is a timeout, whoever's limit it was (a
            // socket, an MCP request, a tool's execution policy).
            found(cancelled(run_token_) && failure.code() == asio::error::operation_aborted
                      ? NodeFailureClass::Cancelled
                  : failure.code() == asio::error::timed_out ? NodeFailureClass::Timeout
                                                             : NodeFailureClass::Ordinary);
            return {{}, nested_of(failure)};
        } catch (const std::bad_alloc&) {
            found(NodeFailureClass::Unknown);
        } catch (const std::logic_error& failure) {
            found(NodeFailureClass::LogicError);
            return {{}, nested_of(failure)};
        } catch (const std::runtime_error& failure) {
            found(NodeFailureClass::Ordinary);
            return {{}, nested_of(failure)};
        } catch (const std::exception& failure) {
            found(NodeFailureClass::Unknown);
            return {{}, nested_of(failure)};
        } catch (...) {
            found(NodeFailureClass::Unknown);
        }
        return {};
    }

    const graph::CancelToken* run_token_;
    bool have_ = false;
    NodeFailureClass class_ = NodeFailureClass::Unknown;
    std::string code_;
    std::string public_message_;
    bool not_retryable_ = false;
};

}  // namespace

NodeFailureVerdict classify_node_failure(const std::exception_ptr& error,
                                         const graph::CancelToken* run_token) noexcept {
    return CauseWalker(run_token).walk(error);
}

}  // namespace neograph::detail

namespace neograph::graph {

ConfigurationFailure::~ConfigurationFailure() = default;

const char* node_failure_code(NodeFailureClass failure_class) noexcept {
    switch (failure_class) {
    case NodeFailureClass::Ordinary: return "NG.TaskFailed";
    case NodeFailureClass::Timeout: return "NG.Timeout";
    case NodeFailureClass::ProviderFailed: return "NG.ProviderFailed";
    case NodeFailureClass::EffectCommitted: return "NG.EffectCommitted";
    case NodeFailureClass::LogicError: return "NG.LogicError";
    case NodeFailureClass::ConfigError: return "NG.ConfigError";
    case NodeFailureClass::SafetyBlocked: return "NG.SafetyBlocked";
    case NodeFailureClass::BudgetExhausted: return "NG.BudgetExhausted";
    case NodeFailureClass::EffectUncertain: return "NG.EffectUncertain";
    case NodeFailureClass::Unknown: return "NG.UnknownFailure";
    case NodeFailureClass::Interrupted: return "NG.Interrupted";
    case NodeFailureClass::HookBlocked: return "NG.HookBlocked";
    case NodeFailureClass::Cancelled: return "NG.Cancelled";
    }
    return "NG.UnknownFailure";
}

bool node_failure_retryable(NodeFailureClass failure_class) noexcept {
    return failure_class == NodeFailureClass::Ordinary || failure_class == NodeFailureClass::Timeout;
}

bool node_failure_routable(NodeFailureClass failure_class) noexcept {
    // The routable classes are exactly the four least severe ones. A committed
    // effect is never retried, but a failure route may run its compensation.
    return failure_class <= NodeFailureClass::EffectCommitted;
}

const char* to_string(NodeFailureKind kind) noexcept {
    switch (kind) {
    case NodeFailureKind::Exhausted: return "exhausted";
    case NodeFailureKind::NonRetryable: return "non_retryable";
    case NodeFailureKind::EffectCommitted: return "effect_committed";
    case NodeFailureKind::Deadline: return "deadline";
    case NodeFailureKind::Ceiling: return "ceiling";
    case NodeFailureKind::VisitLimit: return "visit_limit";
    }
    return "non_retryable";
}

bool is_author_failure_code(std::string_view code) noexcept {
    if (code.empty() || code.size() > 128) return false;
    if (code.size() >= 3 && code.compare(0, 3, "ng.") == 0) return false;  // reserved
    const auto lower = [](char c) { return c >= 'a' && c <= 'z'; };
    const auto tail = [&](char c) { return lower(c) || (c >= '0' && c <= '9') || c == '_'; };
    if (!lower(code.front())) return false;
    std::size_t segments = 1;
    std::size_t length = 1;
    for (std::size_t i = 1; i < code.size(); ++i) {
        if (code[i] == '.') {
            if (length == 0) return false;
            ++segments;
            length = 0;
        } else if (tail(code[i])) {
            ++length;
        } else {
            return false;
        }
    }
    return segments >= 2 && length > 0;
}

FailureVerdict classify_failure(const std::exception_ptr& error,
                                const CancelToken* run_token) noexcept {
    // A run's failure is usually the NodeExecutionError the retry loop threw,
    // which already holds the node's own verdict. (Inside a node, the same
    // exception is a subgraph's failure, hence the classifier's
    // NG.SubgraphFailed.) One without a record is classified by its cause.
    std::exception_ptr classified = error;
    try {
        if (error) std::rethrow_exception(error);
    } catch (const NodeExecutionError& failure) {
        if (const auto* record = node_failure_of(failure)) {
            FailureVerdict out;
            out.failure_class = record->failure_class;
            out.code = record->code;
            out.public_message = record->public_message;
            out.retryable = record->retryable || out.failure_class == NodeFailureClass::ProviderFailed;
            out.routable = record->routable;
            return out;
        }
        if (failure.cause()) classified = failure.cause();
    } catch (...) {
    }
    auto verdict = ::neograph::detail::classify_node_failure(classified, run_token);
    FailureVerdict out;
    out.failure_class = verdict.failure_class;
    out.code = std::move(verdict.code);
    out.public_message = std::move(verdict.public_message);
    // A transient provider failure is not retried by the node retry loop (the
    // provider runtime owns those retries), but the same request sent again
    // may well succeed, which is what `retryable` tells a peer.
    out.retryable = verdict.retryable || out.failure_class == NodeFailureClass::ProviderFailed;
    out.routable = verdict.routable;
    switch (out.failure_class) {
    case NodeFailureClass::Cancelled:
        out.signal = ::neograph::detail::find_signal<CancelledException>(classified);
        break;
    case NodeFailureClass::HookBlocked:
        out.signal = ::neograph::detail::find_signal<HookBoundaryBlocked>(classified);
        break;
    case NodeFailureClass::Interrupted:
        out.signal = ::neograph::detail::find_signal<NodeInterrupt>(classified);
        break;
    default:
        break;
    }
    return out;
}

const NodeFailure* node_failure_of(const NodeExecutionError& error) noexcept {
    const auto* classified = dynamic_cast<const detail::ClassifiedNodeExecutionError*>(&error);
    return classified ? classified->failure().get() : nullptr;
}

std::optional<NodeFailure> node_failure_of(const std::exception_ptr& error) {
    if (!error) return std::nullopt;
    try {
        std::rethrow_exception(error);
    } catch (const NodeExecutionError& failure) {
        if (const auto* record = node_failure_of(failure)) return *record;
    } catch (...) {
    }
    return std::nullopt;
}

}  // namespace neograph::graph
