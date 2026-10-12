#include <neograph/hook_runtime.h>

#include <neograph/graph/cancel.h>

#include <stdexcept>

namespace neograph {

HookRuntime::HookRuntime(std::shared_ptr<MandatoryHookRunner> runner,
                         std::chrono::milliseconds default_timeout)
    : runner_(std::move(runner)), default_timeout_(default_timeout) {
    if (!runner_ || default_timeout_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Hook runtime requires a runner and positive timeout");
    }
}

bool HookRuntime::executing_hook_target() noexcept { return false; }

asio::awaitable<void> HookRuntime::emit_async(
    HookPhase phase, std::string type, std::string owner_scope, std::string run_id, json data,
    std::shared_ptr<graph::CancelToken> cancellation,
    std::optional<std::chrono::system_clock::time_point> parent_deadline,
    std::optional<std::uint64_t> event_sequence) {
    if (cancellation) cancellation->throw_if_cancelled("before lifecycle hook");
    const auto deadline = std::min(parent_deadline.value_or(std::chrono::system_clock::time_point::max()),
                                   std::chrono::system_clock::now() + default_timeout_);
    // Durable callers supply their own sequence so reconnects and CAS retries
    // retain the same outbox identity.
    const auto sequence = event_sequence.value_or(
        next_sequence_.fetch_add(1, std::memory_order_relaxed) + 1);
    std::exception_ptr blocked;
    try {
        const auto event = RuntimeEvent::create({{}, sequence, phase, std::move(type),
                                                  std::move(owner_scope), std::move(run_id),
                                                  std::move(data)});
        if (deadline <= std::chrono::system_clock::now()) {
            // Only a mandatory Hook that was due and could not run in time
            // timed out. An event no mandatory Hook applies to passes.
            if (runner_->blocks(event))
                throw HookBoundaryBlocked({}, HookExecutionState::TimedOut,
                                          "lifecycle hook deadline elapsed");
        } else {
            co_await runner_->run_async(event, deadline);
        }
    } catch (const HookBoundaryBlocked&) {
        blocked = std::current_exception();
    } catch (const graph::CancelledException&) {
        blocked = std::current_exception();
    } catch (const std::exception& error) {
        // A mandatory boundary whose Hooks could not even run (no owner scope
        // to attribute the event to, an unusable journal) fails closed like
        // one whose Hook ran and failed, for every emitter alike: tool calls,
        // checkpoints, providers and Program lifecycle events.
        blocked = std::make_exception_ptr(HookBoundaryBlocked(
            {}, HookExecutionState::Failed,
            std::string("lifecycle hook could not run: ") + error.what()));
    }
    // Cancellation wins over a Hook block, as in a tool batch: a run that was
    // cancelled while its Hook ran stops as cancelled.
    if (cancellation) cancellation->throw_if_cancelled("after lifecycle hook");
    if (blocked) std::rethrow_exception(blocked);
}

} // namespace neograph
