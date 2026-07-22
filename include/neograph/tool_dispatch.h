/**
 * @file tool_dispatch.h
 * @brief The single implementation of "execute an assistant message's tool_calls".
 *
 * Before this existed the concept lived in two places — ToolDispatchNode (graph
 * path) and Agent (standalone path) — and they had drifted: the node fanned the
 * calls out concurrently, the agent ran them one at a time through the blocking
 * execute() facade. Measured on three 300 ms async tools: 300 ms via the node,
 * 900 ms via the agent (issue #87).
 *
 * The performance gap was the symptom. The defect was the duplication: every
 * capability added at this boundary — interception, per-call interrupt, token
 * accounting — would have landed in one path and silently missed the other,
 * exactly as concurrency did. Both callers now route through the function below
 * and nowhere else.
 *
 * **Concurrency contract.** Calls are launched together, so a tool that really
 * suspends (one that overrides `execute_async`) overlaps with its siblings and
 * may be entered concurrently with itself if the model asked for it twice. A
 * tool that implements only the sync `execute()` never overlaps: the default
 * `Tool::execute_async` body runs to completion before the coroutine's first
 * suspension, so the calls serialize even inside the parallel group. That is
 * why unifying the two paths cannot introduce a data race into an existing sync
 * tool — see ToolDispatchParity.SyncToolsDoNotOverlapOnToolNode, which pins the
 * behavior at 900 ms for three 300 ms sync tools.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/types.h>
#include <neograph/tool.h>

#include <asio/awaitable.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace neograph {

/**
 * @brief What a ToolGate is allowed to know about the run it is gating.
 *
 * Deliberately small and passed **by value**. A `graph::RunContext` would be
 * the obvious thing to hand over, but `dispatch_tool_calls` lives in the core
 * `neograph` layer while `RunContext` belongs to `neograph::graph` — and the
 * standalone `llm::Agent`, which shares this dispatcher, has no RunContext at
 * all. So the gate gets a struct of its own, which both callers can fill.
 *
 * Add fields here as gates need them; a by-value struct grows without breaking
 * anybody's gate signature.
 */
struct ToolGateContext {
    /// The human's answer from `GraphEngine::resume()`. Empty on a fresh run,
    /// which is how a gate distinguishes "nobody has been asked yet" from "the
    /// answer was no" — and therefore how it avoids interrupting forever.
    std::optional<json> resume_value;

    /// Mirrors `RunConfig::thread_id`. Empty on the Agent path.
    std::string thread_id;

    /// Super-step index. Zero on the Agent path.
    int step = 0;
};

/**
 * @brief A gate's verdict on one tool call.
 *
 * One gate with three verdicts rather than four hooks: permission, audit,
 * argument rewriting and the per-call interrupt are the same primitive —
 * observe and intervene at the tool-call boundary — wearing different hats.
 * Four hooks would be four things to keep in step, and the next capability
 * would land in some of them and not others.
 */
struct ToolDecision {
    enum class Kind {
        Allow,      ///< Run it, optionally with rewritten arguments.
        Deny,       ///< Do not run it. The reason goes back to the model as the
                    ///< tool's result, so it can react instead of asking again.
        Interrupt   ///< Do not run it, and pause the whole run for a human.
    };

    Kind kind = Kind::Allow;

    /// Allow: the arguments to actually pass. Empty means "what the model sent",
    /// unrewritten — and costs nothing, unlike a default-constructed json.
    std::optional<json> args;

    /// Deny / Interrupt: why. Reaches the model (Deny) or the caller (Interrupt).
    std::string reason;

    /// Interrupt: the structured payload, surfacing at
    /// `RunResult::interrupt_value["value"]` for the caller to render a prompt.
    std::optional<json> payload;

    static ToolDecision allow() { return ToolDecision{}; }

    static ToolDecision allow(json rewritten_args) {
        ToolDecision d;
        d.args = std::move(rewritten_args);
        return d;
    }

    static ToolDecision deny(std::string reason) {
        ToolDecision d;
        d.kind   = Kind::Deny;
        d.reason = std::move(reason);
        return d;
    }

    static ToolDecision interrupt(std::string reason) {
        ToolDecision d;
        d.kind   = Kind::Interrupt;
        d.reason = std::move(reason);
        return d;
    }

    static ToolDecision interrupt(std::string reason, json payload) {
        ToolDecision d;
        d.kind    = Kind::Interrupt;
        d.reason  = std::move(reason);
        d.payload = std::move(payload);
        return d;
    }
};

/**
 * @brief Called once per tool call, before any tool runs.
 *
 * Awaitable, so a gate may do real work — ask a policy service, hit a database,
 * prompt over a socket — without blocking the executor.
 *
 * Set it through `EngineConfig::tool_gate` or `GraphEngine::set_tool_gate`
 * (graph), or `Agent::set_tool_gate` (standalone).
 * Unset means every call runs, exactly as before this existed.
 */
using ToolGate =
    std::function<asio::awaitable<ToolDecision>(ToolCall, ToolGateContext)>;

/**
 * @brief Execute every tool call in @p calls and return one tool message each.
 *
 * Results come back in **call order**, not completion order, so the caller's
 * message history is deterministic regardless of which tool finished first.
 *
 * Never throws on a per-call failure. A call naming an unregistered tool, an
 * unparseable argument payload, or a tool that throws all yield a tool message
 * whose content is an `{"error": "..."}` object — the model sees the failure and
 * can react to it, which is the behavior both callers already had.
 *
 * @param calls Tool calls from the assistant message. Taken **by value**: this
 *        is a coroutine, and a reference parameter would dangle across the first
 *        suspension.
 * @param tools Non-owning tool pointers to resolve names against. Also by value,
 *        for the same reason. The pointees must outlive the returned awaitable.
 * @param gate Optional interception point (issue #89). Consulted for **every**
 *        call before **any** tool runs — see below. Default-constructed (empty)
 *        means no gating, and the behaviour is bit-for-bit what it was before.
 * @param gctx What the gate is told about the run.
 *
 * @throws NodeInterrupt when the gate returns Interrupt for any call. On the
 *         graph path the engine catches it, checkpoints, and hands the caller a
 *         paused `RunResult`. On the Agent path — which has no checkpoint
 *         machinery — it surfaces to the caller of `Agent::run`.
 *
 * **Gating happens before execution, and that ordering is the design.** A
 * permission gate that runs the command and then asks is not a permission gate.
 * Concretely: three calls, the middle one needs approval. Were the gate
 * consulted after execution, the siblings would already have had their effects;
 * the run pauses, the human approves, the engine resumes — and resume re-enters
 * the node from the top, because a node that interrupted recorded no writes. The
 * siblings run a second time. Approving an `rm -rf` would have double-committed
 * the `git commit` beside it. So every call is decided first, while nothing has
 * happened yet, and only then does anything run.
 */
NEOGRAPH_API asio::awaitable<std::vector<ChatMessage>>
dispatch_tool_calls(std::vector<ToolCall> calls, std::vector<Tool*> tools,
                    ToolGate gate = {}, ToolGateContext gctx = {});

}  // namespace neograph
