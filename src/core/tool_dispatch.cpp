#include <neograph/tool_dispatch.h>
#include <neograph/graph/types.h>   // NodeInterrupt — the gate's Interrupt verdict

#include <asio/co_spawn.hpp>
#include <asio/deferred.hpp>
#include <asio/experimental/parallel_group.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <algorithm>
#include <optional>
#include <utility>

namespace neograph {

asio::awaitable<std::vector<ChatMessage>>
dispatch_tool_calls(std::vector<ToolCall> calls, std::vector<Tool*> tools,
                    ToolGate gate, ToolGateContext gctx) {
    std::vector<ChatMessage> results;
    if (calls.empty()) co_return results;

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
    auto worker = [tools](ToolCall tc, std::optional<ToolDecision> decision)
            -> asio::awaitable<ChatMessage> {
        ChatMessage tool_msg;
        tool_msg.role         = "tool";
        tool_msg.tool_call_id = tc.id;
        tool_msg.tool_name    = tc.name;

        // A denial is a result, not an error and not silence: the model has to
        // see it, or it will simply ask for the same tool again next turn.
        if (decision && decision->kind == ToolDecision::Kind::Deny) {
            json denied;
            denied["denied"] = decision->reason;
            tool_msg.content = denied.dump();
            co_return tool_msg;
        }

        auto it = std::find_if(tools.begin(), tools.end(),
            [&](Tool* t) { return t->get_name() == tc.name; });
        if (it == tools.end()) {
            tool_msg.content = R"({"error": "Tool not found: )" + tc.name + "\"}";
            co_return tool_msg;
        }
        try {
            // The gate may rewrite the arguments — that is how ambient values
            // (tenant, thread, credentials) get injected without every tool
            // having to know about them. No rewrite means what the model sent.
            json args = (decision && decision->args)
                            ? *decision->args
                            : json::parse(tc.arguments);
            tool_msg.content = co_await (*it)->execute_async(args);
        } catch (const std::exception& e) {
            tool_msg.content = std::string(R"({"error": ")") + e.what() + "\"}";
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
