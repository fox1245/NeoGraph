/**
 * @file tool_failure.h
 * @brief Internal: the one failure classification for nodes and tools.
 *
 * Every path that executes a model's tool calls (dispatch_tool_calls for one
 * call or a batch, the ToolExecutionController) and the node retry loop ask
 * this helper what a failure means instead of keeping their own lists of
 * exception types. Before it existed the paths kept separate lists and
 * drifted: a batch turned a HookBoundaryBlocked into a tool message the model
 * could read and route around, while a single call stopped the run; the retry
 * loop retried a subgraph failure whose root was a logic error.
 *
 * classify_node_failure walks the whole cause chain (NodeExecutionError
 * causes, std::nested_exception, subgraph failures) and returns the most
 * severe graph::NodeFailureClass found anywhere in it. It is an allowlist:
 * only the runtime_error family, typed author failures (NodeFailureError) and
 * transient provider failures are routable, and anything it does not know is
 * NG.UnknownFailure.
 *
 * The tool contract, documented for users in the reference ("Tool failure
 * contract"), is the same verdict:
 *
 *  - Run control signals stop the run and are never turned into a tool
 *    message: HookBoundaryBlocked (a mandatory lifecycle Hook blocked),
 *    graph::NodeInterrupt (a human has to answer) and cancellation of the
 *    run's CancelToken, wherever they appear in the cause chain.
 *  - Every other failure becomes a tool message with a typed status that the
 *    model sees, so it can react.
 */
#pragma once

#include <neograph/graph/types.h>
#include <neograph/tool_execution.h>

#include <exception>
#include <memory>
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

/// The classifier's verdict on one failure and its whole cause chain.
struct NodeFailureVerdict {
    graph::NodeFailureClass failure_class = graph::NodeFailureClass::Unknown;
    /// "NG.TaskFailed", "NG.SubgraphFailed", an author code, ...
    std::string code;
    /// Text from typed safe sources only (NodeFailureError::public_message).
    std::string public_message;
    bool retryable = false;
    bool routable = false;
};

/// Classify @p error. See NodeFailureClass for the classes and their order.
NodeFailureVerdict classify_node_failure(const std::exception_ptr& error,
                                         const graph::CancelToken* run_token) noexcept;

/// The NodeExecutionError the node retry loop throws: it carries the
/// NodeFailure record that graph::node_failure_of() returns. Not final, so
/// std::throw_with_nested can still nest a secondary failure under it.
class ClassifiedNodeExecutionError : public graph::NodeExecutionError {
public:
    ClassifiedNodeExecutionError(std::string node_name, int attempts, std::exception_ptr cause,
                                 std::shared_ptr<const graph::NodeFailure> failure)
        : NodeExecutionError(std::move(node_name), attempts, std::move(cause)),
          failure_(std::move(failure)) {}
    const std::shared_ptr<const graph::NodeFailure>& failure() const noexcept { return failure_; }

private:
    std::shared_ptr<const graph::NodeFailure> failure_;
};

}  // namespace neograph::detail
