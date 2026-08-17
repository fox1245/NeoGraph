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
    if (deadline <= std::chrono::system_clock::now()) {
        throw HookBoundaryBlocked({}, HookExecutionState::TimedOut, "lifecycle hook deadline elapsed");
    }
    // Durable callers supply their own sequence so reconnects and CAS retries
    // retain the same outbox identity.
    const auto sequence = event_sequence.value_or(
        next_sequence_.fetch_add(1, std::memory_order_relaxed) + 1);
    const auto event = RuntimeEvent::create({{}, sequence, phase, std::move(type),
                                              std::move(owner_scope), std::move(run_id), std::move(data)});
    co_await runner_->run_async(event, deadline);
    if (cancellation) cancellation->throw_if_cancelled("after lifecycle hook");
}

} // namespace neograph
