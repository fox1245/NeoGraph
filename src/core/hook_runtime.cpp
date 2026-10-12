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

HookRuntime::HookRuntime(std::shared_ptr<HookRuntime> outer, std::shared_ptr<HookRuntime> inner)
    : default_timeout_(std::chrono::seconds(30)), outer_(std::move(outer)),
      inner_(std::move(inner)) {}

std::shared_ptr<HookRuntime> HookRuntime::compose(std::shared_ptr<HookRuntime> outer,
                                                  std::shared_ptr<HookRuntime> inner) {
    if (!outer) return inner;
    if (!inner || inner == outer) return outer;
    return std::shared_ptr<HookRuntime>(new HookRuntime(std::move(outer), std::move(inner)));
}

bool HookRuntime::executing_hook_target() noexcept { return false; }

asio::awaitable<void> HookRuntime::emit_async(
    HookPhase phase, std::string type, std::string owner_scope, std::string run_id, json data,
    std::shared_ptr<graph::CancelToken> cancellation,
    std::optional<std::chrono::system_clock::time_point> parent_deadline,
    std::optional<std::uint64_t> event_sequence) {
    if (!runner_) {
        // A nested run's runtimes: the outer run's Hooks first, then the
        // inner run's. The first block stops the event.
        auto outer = outer_->emit_async(phase, type, owner_scope, run_id, data, cancellation,
                                        parent_deadline, event_sequence);
        co_await std::move(outer);
        auto inner = inner_->emit_async(phase, std::move(type), std::move(owner_scope),
                                        std::move(run_id), std::move(data),
                                        std::move(cancellation), parent_deadline,
                                        event_sequence);
        co_await std::move(inner);
        co_return;
    }
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

asio::awaitable<void> HookRuntime::emit_once_async(
    HookPhase phase, std::string type, std::string owner_scope, std::string run_id, json data,
    std::shared_ptr<graph::CancelToken> cancellation,
    std::optional<std::chrono::system_clock::time_point> parent_deadline) {
    if (!runner_) {
        auto outer = outer_->emit_once_async(phase, type, owner_scope, run_id, data, cancellation,
                                             parent_deadline);
        co_await std::move(outer);
        auto inner = inner_->emit_once_async(phase, std::move(type), std::move(owner_scope),
                                             std::move(run_id), std::move(data),
                                             std::move(cancellation), parent_deadline);
        co_await std::move(inner);
        co_return;
    }
    if (cancellation) cancellation->throw_if_cancelled("before lifecycle hook");
    // The identity is the event's content; its sequence numbers the
    // deliveries of that content. The first sequence whose event is not an
    // earlier failed delivery is this one: delivered already (nothing to do),
    // in progress after a crash or awaiting reconciliation (the runner
    // continues it), or fresh.
    constexpr std::uint64_t kMaxDeliveries = 64;
    std::optional<std::uint64_t> sequence;
    std::exception_ptr invalid;
    try {
        for (std::uint64_t candidate = 1; candidate <= kMaxDeliveries; ++candidate) {
            const auto event = RuntimeEvent::create(
                {{}, candidate, phase, type, owner_scope, run_id, data});
            const auto state = runner_->delivery(event);
            if (state == MandatoryHookRunner::Delivery::Delivered) {
                if (cancellation) cancellation->throw_if_cancelled("after lifecycle hook");
                co_return;
            }
            if (state != MandatoryHookRunner::Delivery::NotDelivered) {
                sequence = candidate;
                break;
            }
        }
        if (!sequence)
            throw std::runtime_error("lifecycle hook event was not delivered after 64 emissions");
    } catch (const std::exception& error) {
        // The same fail-closed rule as a Hook that cannot run (emit_async).
        invalid = std::make_exception_ptr(HookBoundaryBlocked(
            {}, HookExecutionState::Failed,
            std::string("lifecycle hook could not run: ") + error.what()));
    }
    if (invalid) {
        if (cancellation) cancellation->throw_if_cancelled("after lifecycle hook");
        std::rethrow_exception(invalid);
    }
    auto emission = emit_async(phase, std::move(type), std::move(owner_scope), std::move(run_id),
                               std::move(data), std::move(cancellation), parent_deadline,
                               sequence);
    co_await std::move(emission);
}

} // namespace neograph
