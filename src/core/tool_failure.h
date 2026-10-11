/**
 * @file tool_failure.h
 * @brief Internal: the one failure classification for model-requested tools.
 *
 * Every path that executes a model's tool calls (dispatch_tool_calls for one
 * call or a batch, the ToolExecutionController, the node retry loop) asks this
 * helper what a failure means instead of keeping its own list of exception
 * types. Before it existed the paths kept separate lists and drifted: a batch
 * turned a HookBoundaryBlocked into a tool message the model could read and
 * route around, while a single call stopped the run.
 *
 * The contract, documented for users in the reference ("Tool failure
 * contract"):
 *
 *  - Run control signals stop the run and are never turned into a tool
 *    message: HookBoundaryBlocked (a mandatory lifecycle Hook blocked),
 *    graph::NodeInterrupt (a human has to answer) and cancellation of the
 *    run's CancelToken.
 *  - Every other failure becomes a tool message with a typed status that the
 *    model sees, so it can react.
 */
#pragma once

#include <neograph/tool_execution.h>

#include <exception>
#include <string>

namespace neograph::graph {
class CancelToken;
}

namespace neograph::detail {

/// What a tool-execution failure means for the run. Ordered by precedence:
/// when several calls of one batch stop the run, the highest value wins.
enum class ToolFailureClass : unsigned char {
    /// Ordinary failure. The call yields a tool message the model sees.
    ToolError = 0,
    /// graph::NodeInterrupt: pause the run for a human.
    Interrupted = 1,
    /// HookBoundaryBlocked: a mandatory lifecycle Hook blocked the boundary.
    HookBlocked = 2,
    /// The run was cancelled: graph::CancelledException, or an aborted
    /// operation while the run's CancelToken is cancelled.
    Cancelled = 3,
};

/// Classify @p error. @p run_token is the run's token, or null when the
/// caller has none; it only decides whether an aborted asio operation is
/// cancellation.
ToolFailureClass classify_tool_failure(const std::exception_ptr& error,
                                       const graph::CancelToken* run_token) noexcept;

/// True for every class that stops the run.
inline bool stops_run(ToolFailureClass kind) noexcept {
    return kind != ToolFailureClass::ToolError;
}

/// Rethrow a run-stopping failure. The original exception object is rethrown,
/// so its type and fields (invocation id, state, interrupt payload) survive;
/// an aborted asio operation under a cancelled run becomes
/// graph::CancelledException.
[[noreturn]] void rethrow_run_stop(const std::exception_ptr& error, ToolFailureClass kind);

/// The text a tool message carries for an ordinary failure.
std::string tool_failure_message(const std::exception_ptr& error);

/// The typed result of an ordinary exception raised by tool code. A call that
/// started under a policy that does not rule out an effect is uncertain.
ToolExecutionResult ordinary_tool_failure(const ToolExecutionPolicy& policy, bool started,
                                          std::string error);

}  // namespace neograph::detail
