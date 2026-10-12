#include <neograph/tool_dispatch.h>
#include <neograph/tool_effect_broker.h>
#include <neograph/hook_outbox.h>
#include <neograph/hook_runtime.h>
#include <neograph/graph/cancel.h>
#include <neograph/graph/types.h>   // NodeInterrupt — the gate's Interrupt verdict

#include "text_safety.h"
#include "tool_failure.h"

#include <asio/co_spawn.hpp>
#include <asio/deferred.hpp>
#include <asio/experimental/parallel_group.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <algorithm>
#include <atomic>
#include <optional>
#include <utility>

namespace neograph {
namespace {
std::atomic<std::uint64_t> next_host_tool_request{0};

// The one shape of a failed call's tool message, wherever the failure was met.
ChatMessage failure_message(const ToolCall& call, ToolTerminalStatus status, std::string error,
                            bool retryable = false, bool uncertain = false) {
    ChatMessage message;
    message.role = "tool";
    message.tool_call_id = call.id;
    message.tool_name = call.name;
    message.tool_status = std::string(to_string(status));
    message.tool_retryable = retryable;
    message.tool_effect_uncertain = uncertain;
    message.content = json{{"error", detail::valid_utf8(std::move(error))},
                           {"status", message.tool_status},
                           {"retryable", retryable},
                           {"effect_uncertain", uncertain}}
                          .dump();
    return message;
}
}

asio::awaitable<std::vector<ChatMessage>>
dispatch_tool_calls(std::vector<ToolCall> calls, std::vector<Tool*> tools,
                    ToolGate gate, ToolGateContext gctx,
                    ToolExecutionContext execution) {
    std::vector<ChatMessage> results;
    if (calls.empty()) co_return results;
    if (execution.effect_broker &&
        (execution.identity.thread_id.empty() || execution.effect_task_id.empty())) {
        throw std::invalid_argument("Tool effect broker requires a thread-scoped Core task identity");
    }

    auto throw_if_cancelled = [&execution](const char* detail) {
        if (execution.cancel_token) {
            execution.cancel_token->throw_if_cancelled(detail);
        }
    };

    // ── Phase 1: decide, while nothing has happened yet (issue #89) ────────
    //
    // Every call is put to the gate BEFORE any tool runs, and an Interrupt
    // aborts the batch with zero side effects. See the header for why the
    // ordering is not merely tidier: a gate consulted after execution would let
    // the siblings of an approval-pending call take effect, and resume — which
    // re-enters the node from the top, since an interrupted node records no
    // writes — would then run them a second time.
    //
    // Gates are asked in call order, one at a time. They are policy checks, and
    // sequencing them keeps "which call interrupted" deterministic when more
    // than one wants to.
    std::vector<ToolDecision> decisions;
    if (gate) {
        // Only gated dispatch pays for the immutable batch snapshot. The gate
        // receives context by value and may suspend, so a borrowed view of
        // `calls` would not be a safe public API.
        gctx.batch_calls = std::make_shared<const std::vector<ToolCall>>(calls);
        decisions.reserve(calls.size());
        for (const auto& tc : calls) {
            throw_if_cancelled("before tool gate");
            decisions.push_back(co_await gate(tc, gctx));
        }
        for (const auto& d : decisions) {
            if (d.kind != ToolDecision::Kind::Interrupt) continue;
            // Rides the machinery #94 built rather than inventing a second
            // interrupt: the engine catches this, checkpoints, and hands the
            // caller a paused RunResult carrying the reason and the payload.
            if (d.payload) throw graph::NodeInterrupt(d.reason, *d.payload);
            throw graph::NodeInterrupt(d.reason);
        }
    }

    // ── Phase 2: act ──────────────────────────────────────────────────────
    //
    // One worker per call. Resolves the tool, awaits execute_async, and applies
    // the tool failure contract (tool_failure.h) to anything that goes wrong:
    // an ordinary failure (unknown tool, bad arguments, a throwing tool or
    // lookup) becomes the call's tool message; a run control signal (Hook
    // block, interrupt, cancellation) leaves the worker and stops the batch.
    throw_if_cancelled("before tool dispatch");

    auto worker = [tools, execution](std::size_t call_index, ToolCall tc,
                                     std::optional<ToolDecision> decision)
            -> asio::awaitable<ChatMessage> {
        const auto hook_deadline = [&execution]()
            -> std::optional<std::chrono::system_clock::time_point> {
            if (!execution.deadline) return std::nullopt;
            const auto remaining = *execution.deadline - std::chrono::steady_clock::now();
            return std::chrono::system_clock::now() +
                   std::chrono::duration_cast<std::chrono::system_clock::duration>(remaining);
        };

        // A denial is a result, not an error and not silence: the model has to
        // see it, or it will simply ask for the same tool again next turn.
        if (decision && decision->kind == ToolDecision::Kind::Deny) {
            co_return failure_message(tc, ToolTerminalStatus::Rejected, decision->reason);
        }

        ChatMessage tool_msg;
        tool_msg.role         = "tool";
        tool_msg.tool_call_id = tc.id;
        tool_msg.tool_name    = tc.name;
        bool broker_invoked = false;
        // Set while a lifecycle Hook is emitted for this call.
        bool at_hook_boundary = false;
        std::exception_ptr failure;
        try {
            if (execution.cancel_token) {
                execution.cancel_token->throw_if_cancelled("before tool execution");
            }
            // A call that would start after the run's deadline does not
            // start, and none of its Hooks run: it expired, like a call whose
            // queue wait ran out (the controller applies the same rule to
            // every caller). Cancellation, checked first, wins.
            if (execution.deadline && std::chrono::steady_clock::now() >= *execution.deadline) {
                co_return failure_message(tc, ToolTerminalStatus::Expired,
                                          std::string(detail::kRunDeadlineElapsed));
            }
            // Resolving the name runs tool code (get_name), so it is handled
            // like the call itself, not as a failure of the whole batch.
            Tool* tool = nullptr;
            for (Tool* candidate : tools) {
                if (candidate->get_name() == tc.name) {
                    tool = candidate;
                    break;
                }
            }
            if (!tool) {
                co_return failure_message(tc, ToolTerminalStatus::Rejected,
                                          "Tool not found: " + tc.name);
            }
            // The gate may rewrite the arguments — that is how ambient values
            // (tenant, thread, credentials) get injected without every tool
            // having to know about them. No rewrite means what the model sent.
            json args = (decision && decision->args)
                            ? *decision->args
                            : json::parse(tc.arguments);
            auto call_execution = execution;
            if (call_execution.identity.request_id.empty()) {
                call_execution.identity.request_id = "tool-request-" + std::to_string(
                    next_host_tool_request.fetch_add(1, std::memory_order_relaxed) + 1);
            }
            if (execution.hook_runtime) {
                at_hook_boundary = true;
                // GCC 13 ICEs when the returned awaitable is consumed as a temporary.
                auto emission = execution.hook_runtime->emit_async(
                    HookPhase::BeforeToolExecution, "tool_execution",
                    call_execution.identity.owner_scope, call_execution.identity.root_run_id,
                    json{{"tool_call_id", tc.id}, {"tool_name", tc.name}, {"arguments", args},
                         {"thread_id", call_execution.identity.thread_id},
                         {"request_id", call_execution.identity.request_id}},
                    execution.cancel_token, hook_deadline());
                co_await std::move(emission);
                at_hook_boundary = false;
            }
            auto controller = call_execution.controller
                            ? call_execution.controller
                            : default_tool_execution_controller();
            ToolExecutionResult result;
            if (call_execution.effect_broker) {
                ToolEffectIdentity identity;
                identity.owner_scope = call_execution.identity.owner_scope;
                identity.run_id = call_execution.identity.root_run_id;
                identity.program_version_id = call_execution.effect_grant.program_version_id;
                identity.operation_id = call_execution.effect_grant.operation_id;
                identity.grant_id = call_execution.effect_grant.grant_id;
                identity.attempt = call_execution.effect_grant.attempt;
                identity.binding_fingerprint = call_execution.effect_grant.binding_fingerprint;
                identity.thread_id = call_execution.identity.thread_id;
                identity.task_id = call_execution.effect_task_id;
                identity.call_ordinal = call_index;
                identity.tool_call_id = tc.id;
                call_execution.controller = controller;
                broker_invoked = true;
                result = co_await call_execution.effect_broker->execute(
                    std::move(identity), *tool, std::move(args), call_execution);
            } else {
                result = co_await controller->execute_result_async(
                    *tool, std::move(args), call_execution);
            }
            // What the model reads is well-formed UTF-8 whoever produced it
            // (a broker's replayed receipt, a controller result).
            result.output = detail::valid_utf8(std::move(result.output));
            result.error = detail::valid_utf8(std::move(result.error));
            tool_msg.tool_status = std::string(to_string(result.status));
            tool_msg.tool_retryable = result.retryable;
            tool_msg.tool_effect_uncertain = result.effect_uncertain ||
                result.status == ToolTerminalStatus::ReconciliationRequired;
            if (result.succeeded()) {
                tool_msg.content = result.output;
            } else {
                tool_msg.content = json{
                    {"error", result.error},
                    {"status", tool_msg.tool_status},
                    {"retryable", result.retryable},
                    {"effect_uncertain", tool_msg.tool_effect_uncertain},
                    {"output", result.output}}
                    .dump();
            }
            if (execution.hook_runtime) {
                at_hook_boundary = true;
                auto emission = execution.hook_runtime->emit_async(
                    HookPhase::AfterToolExecution, "tool_execution",
                    call_execution.identity.owner_scope, call_execution.identity.root_run_id,
                    json{{"tool_call_id", tc.id}, {"tool_name", tc.name}, {"status", tool_msg.tool_status},
                         {"result", tool_msg.content}, {"thread_id", call_execution.identity.thread_id},
                         {"request_id", call_execution.identity.request_id}},
                    execution.cancel_token, hook_deadline());
                co_await std::move(emission);
                at_hook_boundary = false;
            }
            // Cancellation is graph control flow, not a tool result that the
            // model should consume before the run terminates.
            if (execution.cancel_token) {
                execution.cancel_token->throw_if_cancelled("after tool execution");
            }
        } catch (...) {
            failure = std::current_exception();
        }
        if (failure) {
            const auto kind = detail::classify_tool_failure(failure, execution.cancel_token.get());
            if (detail::stops_run(kind)) detail::rethrow_run_stop(failure, kind);
            if (at_hook_boundary) {
                // A mandatory Hook that could not even run (say, the run has
                // no owner scope to attribute the event to) fails closed like
                // a Hook that ran and failed. Its error is not the tool's: the
                // model must not read it, and after the tool ran it must not
                // replace a result whose effect already happened.
                throw HookBoundaryBlocked({}, HookExecutionState::Failed,
                                          "lifecycle hook could not run: " +
                                              detail::tool_failure_message(failure));
            }
            // Once the broker has the call, its effect may have happened.
            co_return failure_message(tc,
                broker_invoked ? ToolTerminalStatus::ReconciliationRequired
                               : ToolTerminalStatus::Failed,
                detail::tool_failure_message(failure), false, broker_invoked);
        }
        co_return tool_msg;
    };

    auto decision_for = [&decisions](std::size_t i) -> std::optional<ToolDecision> {
        if (i < decisions.size()) return decisions[i];
        return std::nullopt;
    };
    const auto stop_on_uncertain_effect = [&execution](const std::vector<ChatMessage>& messages) {
        if (!execution.effect_broker) return;
        if (std::any_of(messages.begin(), messages.end(), [](const ChatMessage& message) {
                return message.tool_effect_uncertain;
            })) {
            throw graph::NodeInterrupt(
                "Mediated Tool effect requires reconciliation",
                json{{"kind", "tool_effect_reconciliation"},
                     {"thread_id", execution.identity.thread_id},
                     {"task_id", execution.effect_task_id}});
        }
    };

    // Every call's outcome is settled the same way, one call or many, in call
    // order whatever the completion order was. A run control signal from any
    // call stops the batch: cancellation first, then a Hook block, then an
    // interrupt, then the earliest call. Only then can an unresolved effect
    // pause the run for reconciliation, and only then do the messages return.
    const auto settle = [&](const std::vector<std::exception_ptr>& errors,
                            std::vector<ChatMessage> values) {
        std::exception_ptr stop;
        auto stop_kind = detail::ToolFailureClass::ToolError;
        for (const auto& error : errors) {
            if (!error) continue;
            const auto kind = detail::classify_tool_failure(error, execution.cancel_token.get());
            if (kind > stop_kind) {
                stop = error;
                stop_kind = kind;
            }
        }
        if (stop) detail::rethrow_run_stop(stop, stop_kind);

        std::vector<ChatMessage> settled;
        settled.reserve(values.size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (!errors[i]) {
                settled.push_back(std::move(values[i]));
                continue;
            }
            // A worker handles its own failures, so this is the defensive
            // case (e.g. bad_alloc while building a message). It still gets
            // the typed shape; under a broker the effect may have happened.
            const bool uncertain = static_cast<bool>(execution.effect_broker);
            settled.push_back(failure_message(
                calls[i],
                uncertain ? ToolTerminalStatus::ReconciliationRequired : ToolTerminalStatus::Failed,
                detail::tool_failure_message(errors[i]), false, uncertain));
        }
        stop_on_uncertain_effect(settled);
        return settled;
    };

    // Single call: run inline, skip the parallel-group machinery.
    if (calls.size() == 1) {
        std::vector<std::exception_ptr> errors(1);
        std::vector<ChatMessage> values(1);
        try {
            values[0] = co_await worker(0, calls.front(), decision_for(0));
        } catch (...) {
            errors[0] = std::current_exception();
        }
        co_return settle(errors, std::move(values));
    }

    // Multiple calls: fan them out via the same parallel-group idiom the engine
    // uses for independent nodes within a super-step.
    auto ex = co_await asio::this_coro::executor;
    using DeferredOp = decltype(asio::co_spawn(
        ex, worker(std::declval<std::size_t>(), std::declval<ToolCall>(),
                   std::declval<std::optional<ToolDecision>>()),
        asio::deferred));
    std::vector<DeferredOp> ops;
    ops.reserve(calls.size());
    for (std::size_t i = 0; i < calls.size(); ++i) {
        ops.push_back(asio::co_spawn(ex, worker(i, calls[i], decision_for(i)),
                                     asio::deferred));
    }

    auto [order, excs, values] = co_await asio::experimental::make_parallel_group(
        std::move(ops))
        .async_wait(asio::experimental::wait_for_all(), asio::use_awaitable);
    (void)order;  // results are applied in call order, not completion order
    co_return settle(excs, std::move(values));
}

}  // namespace neograph
