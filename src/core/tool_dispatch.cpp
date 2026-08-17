#include <neograph/tool_dispatch.h>
#include <neograph/hook_runtime.h>
#include <neograph/graph/cancel.h>
#include <neograph/graph/types.h>   // NodeInterrupt — the gate's Interrupt verdict

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
}

asio::awaitable<std::vector<ChatMessage>>
dispatch_tool_calls(std::vector<ToolCall> calls, std::vector<Tool*> tools,
                    ToolGate gate, ToolGateContext gctx,
                    ToolExecutionContext execution) {
    std::vector<ChatMessage> results;
    if (calls.empty()) co_return results;

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
    // One worker per call. Resolves the tool, awaits execute_async, and folds
    // every failure mode (unknown tool, bad arguments, throwing tool) into the
    // returned message so a worker never propagates an exception into the
    // parallel group.
    throw_if_cancelled("before tool dispatch");

    auto worker = [tools, execution](ToolCall tc, std::optional<ToolDecision> decision)
            -> asio::awaitable<ChatMessage> {
        const auto hook_deadline = [&execution]()
            -> std::optional<std::chrono::system_clock::time_point> {
            if (!execution.deadline) return std::nullopt;
            const auto remaining = *execution.deadline - std::chrono::steady_clock::now();
            return std::chrono::system_clock::now() +
                   std::chrono::duration_cast<std::chrono::system_clock::duration>(remaining);
        };
        if (execution.cancel_token) {
            execution.cancel_token->throw_if_cancelled("before tool execution");
        }
        ChatMessage tool_msg;
        tool_msg.role         = "tool";
        tool_msg.tool_call_id = tc.id;
        tool_msg.tool_name    = tc.name;
        const auto set_failure = [&tool_msg](ToolTerminalStatus status,
                                             std::string error,
                                             bool retryable = false,
                                             bool uncertain = false) {
            tool_msg.tool_status = std::string(to_string(status));
            tool_msg.tool_retryable = retryable;
            tool_msg.tool_effect_uncertain = uncertain;
            tool_msg.content = json{{"error", std::move(error)},
                                    {"status", tool_msg.tool_status},
                                    {"retryable", retryable},
                                    {"effect_uncertain", uncertain}}
                                   .dump();
        };

        // A denial is a result, not an error and not silence: the model has to
        // see it, or it will simply ask for the same tool again next turn.
        if (decision && decision->kind == ToolDecision::Kind::Deny) {
            set_failure(ToolTerminalStatus::Rejected, decision->reason);
            co_return tool_msg;
        }

        auto it = std::find_if(tools.begin(), tools.end(),
            [&](Tool* t) { return t->get_name() == tc.name; });
        if (it == tools.end()) {
            set_failure(ToolTerminalStatus::Rejected, "Tool not found: " + tc.name);
            co_return tool_msg;
        }
        try {
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
                co_await execution.hook_runtime->emit_async(
                    HookPhase::BeforeToolExecution, "tool_execution",
                    call_execution.identity.owner_scope, call_execution.identity.root_run_id,
                    json{{"tool_call_id", tc.id}, {"tool_name", tc.name}, {"arguments", args},
                         {"thread_id", call_execution.identity.thread_id},
                         {"request_id", call_execution.identity.request_id}},
                    execution.cancel_token, hook_deadline());
            }
            auto controller = call_execution.controller
                            ? call_execution.controller
                            : default_tool_execution_controller();
            const auto result = co_await controller->execute_result_async(
                **it, std::move(args), call_execution);
            tool_msg.tool_status = std::string(to_string(result.status));
            tool_msg.tool_retryable = result.retryable;
            tool_msg.tool_effect_uncertain = result.effect_uncertain;
            if (result.succeeded()) {
                tool_msg.content = result.output;
            } else {
                tool_msg.content = json{
                    {"error", result.error},
                    {"status", tool_msg.tool_status},
                    {"retryable", result.retryable},
                    {"effect_uncertain", result.effect_uncertain},
                    {"output", result.output}}
                    .dump();
            }
            if (execution.hook_runtime) {
                co_await execution.hook_runtime->emit_async(
                    HookPhase::AfterToolExecution, "tool_execution",
                    call_execution.identity.owner_scope, call_execution.identity.root_run_id,
                    json{{"tool_call_id", tc.id}, {"tool_name", tc.name}, {"status", tool_msg.tool_status},
                         {"result", tool_msg.content}, {"thread_id", call_execution.identity.thread_id},
                         {"request_id", call_execution.identity.request_id}},
                    execution.cancel_token, hook_deadline());
            }
            if (execution.cancel_token) {
                execution.cancel_token->throw_if_cancelled("after tool execution");
            }
        } catch (const HookBoundaryBlocked&) {
            throw;
        } catch (const graph::CancelledException&) {
            // Cancellation is graph control flow, not a tool result that the
            // model should consume before the run terminates.
            throw;
        } catch (const asio::system_error& error) {
            if (execution.cancel_token && execution.cancel_token->is_cancelled()
                && error.code() == asio::error::operation_aborted) {
                throw graph::CancelledException("tool operation aborted");
            }
            set_failure(ToolTerminalStatus::Failed, error.what());
        } catch (const std::exception& e) {
            set_failure(ToolTerminalStatus::Failed, e.what());
        }
        co_return tool_msg;
    };

    auto decision_for = [&decisions](std::size_t i) -> std::optional<ToolDecision> {
        if (i < decisions.size()) return decisions[i];
        return std::nullopt;
    };

    // Single call: run inline, skip the parallel-group machinery.
    if (calls.size() == 1) {
        results.push_back(co_await worker(calls.front(), decision_for(0)));
        co_return results;
    }

    // Multiple calls: fan them out via the same parallel-group idiom the engine
    // uses for independent nodes within a super-step.
    auto ex = co_await asio::this_coro::executor;
    using DeferredOp = decltype(asio::co_spawn(
        ex, worker(std::declval<ToolCall>(),
                   std::declval<std::optional<ToolDecision>>()),
        asio::deferred));
    std::vector<DeferredOp> ops;
    ops.reserve(calls.size());
    for (std::size_t i = 0; i < calls.size(); ++i) {
        ops.push_back(asio::co_spawn(ex, worker(calls[i], decision_for(i)),
                                     asio::deferred));
    }

    auto [order, excs, values] = co_await asio::experimental::make_parallel_group(
        std::move(ops))
        .async_wait(asio::experimental::wait_for_all(), asio::use_awaitable);
    (void)order;  // results are applied in call order, not completion order

    results.reserve(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (excs[i]) {
            // Workers catch their own exceptions, so this is a defensive
            // fallback (e.g. bad_alloc) keyed to the originating call.
            ChatMessage m;
            m.role         = "tool";
            m.tool_call_id = calls[i].id;
            m.tool_name    = calls[i].name;
            try {
                std::rethrow_exception(excs[i]);
            } catch (const graph::CancelledException&) {
                throw;
            } catch (const asio::system_error& error) {
                if (execution.cancel_token && execution.cancel_token->is_cancelled()
                    && error.code() == asio::error::operation_aborted) {
                    throw graph::CancelledException("tool operation aborted");
                }
                m.content = std::string(R"({"error": ")") + error.what() + "\"}";
            } catch (const std::exception& e) {
                m.content = std::string(R"({"error": ")") + e.what() + "\"}";
            } catch (...) {
                m.content = R"({"error": "unknown tool failure"})";
            }
            results.push_back(std::move(m));
        } else {
            results.push_back(std::move(values[i]));
        }
    }
    co_return results;
}

}  // namespace neograph
