#include <neograph/async/host_loop.h>

#include "host_timer.h"
#include "loop_handoff.h"
#include "runtime_clock.h"

#include <asio/basic_waitable_timer.hpp>
#include <asio/error.hpp>
#include <asio/error_code.hpp>
#include <asio/post.hpp>
#include <asio/execution_context.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace neograph::async {
namespace detail {

NEOGRAPH_API std::atomic<SimulatedClock*> simulated_clock{nullptr};

class HostLoopCore;

struct SimulatedHostTimer {
    explicit SimulatedHostTimer(std::shared_ptr<HostLoopCore> loop_core) noexcept
        : core(std::move(loop_core)) {}

    std::weak_ptr<HostLoopCore> core;
    RuntimeClock::time_point expiry{};
    std::optional<std::pair<RuntimeClock::time_point, std::uint64_t>> armed_deadline;
};

// The number of HostLoop turns the calling thread is running (they nest only when a handler runs another loop).
thread_local unsigned host_turn_depth = 0;
// The turns the calling thread is running, innermost first (they nest only when a handler runs another loop). Work
// posted from inside a loop's own turn is run by that turn, and nothing else is: a thread that posts from outside it is
// not this one. A loop whose turn is further out is still running it, so a post to it from a nested loop needs no turn.
struct HostTurnLink {
    const HostLoopCore* core;
    const HostTurnLink* outer;
};
thread_local const HostTurnLink* host_turn_top = nullptr;

bool runs_turn_of(const HostLoopCore* core) noexcept {
    for (const auto* link = host_turn_top; link; link = link->outer)
        if (link->core == core) return true;
    return false;
}

// While a pool thread posts the result of its work with the loop's gate held (HostLoopCore::post_handoff), a wake of
// that loop is noted and made after the gate is released, whole: the claim of the turn, the host executor's `post` and
// the re-asks after a refusal all happen without the gate, so no host code runs under it.
thread_local const HostLoopCore* wake_deferred_for = nullptr;
thread_local bool wake_deferred = false;

// Found through the io_context, so that note_deadline can reach the loop from any executor derived from it.
class HostLoopService final : public asio::execution_context::service {
public:
    static asio::execution_context::id id;
    // Only HostLoop creates one, with its core. The context-only form is for asio::use_service to compile.
    explicit HostLoopService(asio::execution_context& context, HostLoopCore* core = nullptr)
        : asio::execution_context::service(context), core(core) {}
    HostLoopCore* core;

private:
    void shutdown() override { core = nullptr; }
};
asio::execution_context::id HostLoopService::id;

class HostLoopCore : public std::enable_shared_from_this<HostLoopCore> {
public:
    HostLoopCore(std::shared_ptr<sp::runtime::Executor> host, HostLoop::Options options)
        : host_(std::move(host)), options_(std::move(options)) {
        if (!host_) throw std::invalid_argument("HostLoop needs a host executor");
        if (options_.turn_handlers == 0) options_.turn_handlers = 1;
        io_.emplace();
        asio::make_service<HostLoopService>(*io_, this);
    }

    asio::any_io_executor executor() {
        if (!io_) throw std::logic_error("HostLoop is closed");
        return asio::any_io_executor(PumpedExecutor<asio::io_context::executor_type>(io_->get_executor(), this));
    }

    void close() noexcept {
        closed_.store(true);
        disarm();
        steady_deadlines_.deadlines.clear();
        steady_deadlines_.sentinel.reset();
        steady_deadlines_.watching = false;
        runtime_deadlines_.deadlines.clear();
        runtime_deadlines_.sentinel.reset();
        runtime_deadlines_.watching = false;
        std::vector<ClaimedSimulatedWait> abandoned_waits;
        {
            std::lock_guard<std::mutex> lock(simulated_mutex_);
            for (auto& [key, owner] : simulated_deadlines_) {
                if (owner) owner->armed_deadline.reset();
            }
            simulated_deadlines_.clear();
            for (auto& [key, wait] : simulated_waits_) {
                abandoned_waits.push_back(
                    ClaimedSimulatedWait{std::move(wait.slot_state), std::move(wait.handler)});
            }
            simulated_waits_.clear();
        }
        for (auto& wait : abandoned_waits) {
            detach_slot(wait.slot_state);
        }
        abandoned_waits.clear();
        // The coroutines that await work on a pool of the process are held by that work, not by the io_context, and
        // go the same way, while the io_context they ran on still exists.
        take_handoffs();
        // Destroys the coroutines that have not finished. Their frames may wake the loop, which is closed.
        io_.reset();
        // Wait for any pool thread that has already posted a handoff and is still inside wake() to finish calling
        // host_->post, so no pool thread calls the host executor after the destructor returns, then release the loop's
        // reference to the host executor and on_error callback on the host thread.
        {
            std::unique_lock<std::mutex> lock(handoff_mutex_);
            wake_done_.wait(lock, [this] { return waking_ == 0; });
        }
        take_handoffs();
        host_.reset();
        options_.on_error = nullptr;
    }

    // ---- Work that a pool thread does for a coroutine of this loop (loop_handoff.h) ---------------------------------
    //
    // The gate is `handoff_mutex_`. A pool thread posts the continuation with it held, so the io_context is alive for
    // the whole post, and close() takes the continuations that are still waiting with it held, so a pool thread
    // either posted before that (the io_context destroys the handler as it does any queued one) or finds the
    // continuation taken and touches nothing of the loop. A post that succeeded notes an in-flight pool wake (`waking_`)
    // before releasing the gate, and close() waits for `waking_` to reach zero after setting `closed_`, so no pool
    // thread calls `host_->post` after close() returns.

    void track(LoopHandoff* handoff, std::shared_ptr<HostLoopCore> core) {
        std::lock_guard<std::mutex> lock(handoff_mutex_);
        const std::size_t needed = abandoned_.size() + handoffs_.size() + 1;
        abandoned_.reserve(needed);
        spare_.reserve(needed);
        handoffs_.insert(handoff);
        handoff->core_ = std::move(core);
    }

    void post_handoff(LoopHandoff& handoff) noexcept {
        bool wake_up = false;
        {
            std::lock_guard<std::mutex> lock(handoff_mutex_);
            if (handoff.state_ != LoopHandoff::State::Open) return;  // the loop took the continuation
            handoffs_.erase(&handoff);
            handoff.state_ = LoopHandoff::State::Posted;
            struct DeferWake {
                explicit DeferWake(const HostLoopCore* core) noexcept {
                    wake_deferred_for = core;
                    wake_deferred = false;
                }
                ~DeferWake() { wake_deferred_for = nullptr; }
            } defer(this);
            std::shared_ptr<Resume> holder;
            try {
                holder = std::make_shared<Resume>(std::move(handoff.resume_));
                asio::post(handoff.executor_, [holder]() mutable { (*holder)(); });
            } catch (...) {
                // Not posted (out of memory). The continuation is kept for the host thread to destroy in turn() or
                // take_handoffs(), never destroyed on the pool thread.
                handoff.state_ = LoopHandoff::State::Taken;
                Resume unposted = holder ? std::move(*holder) : std::move(handoff.resume_);
                abandoned_.emplace_back(std::move(unposted), std::move(handoff.executor_));
            }
            // Released with the gate held: a strand of the loop's executor touches its io_context when it dies.
            handoff.executor_ = asio::any_io_executor();
            // Requested unconditionally once the handoff is queued or set aside: a post to a strand that already has a
            // handler queued appends to the strand without calling PumpedExecutor::execute, so wake_deferred stays
            // false even when an earlier wake was refused by the host, and an abandoned continuation needs a turn to
            // destroy its coroutine frame on the host thread.
            if (!closed_.load()) {
                ++waking_;
                wake_up = true;
            }
        }
        if (wake_up) {
            wake();
            std::lock_guard<std::mutex> lock(handoff_mutex_);
            if (--waking_ == 0) wake_done_.notify_all();
        }
    }

    // A handoff that is destroyed unfinished: the pool discarded its work.
    void release_handoff(LoopHandoff& handoff) noexcept {
        bool wake_up = false;
        {
            std::lock_guard<std::mutex> lock(handoff_mutex_);
            if (handoff.state_ != LoopHandoff::State::Open) return;
            handoffs_.erase(&handoff);
            handoff.state_ = LoopHandoff::State::Taken;
            abandoned_.emplace_back(std::move(handoff.resume_), std::move(handoff.executor_));
            handoff.executor_ = asio::any_io_executor();
            if (!closed_.load()) {
                ++waking_;
                wake_up = true;
            }
        }
        if (wake_up) {
            wake();
            std::lock_guard<std::mutex> lock(handoff_mutex_);
            if (--waking_ == 0) wake_done_.notify_all();
        }
    }

    // The host's thread, once the loop is closed and before its io_context is destroyed.
    void take_handoffs() noexcept { (void)drain_handoffs(true); }

    bool drain_handoffs(bool take_open) noexcept {
        bool drained = false;
        std::vector<std::pair<Resume, asio::any_io_executor>> batch;
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(handoff_mutex_);
                if (take_open) {
                    for (LoopHandoff* handoff : handoffs_) {
                        handoff->state_ = LoopHandoff::State::Taken;
                        abandoned_.emplace_back(std::move(handoff->resume_), std::move(handoff->executor_));
                    }
                    handoffs_.clear();
                }
                if (abandoned_.empty()) {
                    if (spare_.capacity() < batch.capacity()) spare_.swap(batch);
                    return drained;
                }
                if (batch.capacity() < spare_.capacity()) batch.swap(spare_);
                abandoned_.swap(batch);
            }
            drained = true;
            // Destroyed here, without the gate: the coroutines that awaited the pool.
            batch.clear();
        }
    }

    // A turn is scheduled on the host, unless one already is. Safe from any thread (#602): it is what a post to the
    // executor does after it queued the work, and a completion that arrives on a thread of its own makes that post.
    //
    //   - No lock is held. `scheduled_` is claimed with one atomic exchange, so at most one turn of this loop is
    //     queued on the host at a time, and the winner alone calls `host->post`, on its own thread, with nothing of
    //     the loop's held. The host executor's `post` is therefore called from any thread that posts to the loop.
    //   - The work is queued before the wake. If the exchange finds a turn already scheduled, that turn has not
    //     cleared `scheduled_` yet, and it clears it before it polls, so it polls after this post. If the turn
    //     cleared it first, the exchange schedules the next one.
    //   - A post from inside this loop's own turn needs no turn: the turn polls on until it runs dry, and a turn
    //     that stops at its batch schedules the next. Only the thread that runs the turn is in that case; a post from
    //     another thread while the turn runs schedules one more turn, which finds nothing to do if the first
    //     already ran the work.
    //   - The host may refuse the post (it throws, as an executor that is shutting down may). A wake that found the
    //     flag set while the post was being asked for counted on a turn that then does not come, and it has returned.
    //     So that wake leaves a note (`relied_`) and looks at the flag again, and the wake whose post was refused hands
    //     the flag back first and then looks for a note, as in a handshake: one of the two sees the other. The one
    //     that sees it asks the host for the turn, so that the work of the other is not left without one. A refusal
    //     that no wake counted on, or one that repeats, leaves the work to the next wake: `HostLoop::wake()` or any
    //     post, which the host makes once it takes posts again.
    void wake() noexcept {
        if (closed_.load()) return;
        if (wake_deferred_for == this) {
            wake_deferred = true;
            return;
        }
        if (runs_turn_of(this) && running_) return;
        for (int asked = 0;;) {
            if (closed_.load()) return;
            if (scheduled_.exchange(true)) {
                relied_.store(true);
                if (scheduled_.load()) return;
                continue;  // the flag was handed back in the meantime
            }
            if (closed_.load()) {
                scheduled_.store(false);
                return;
            }
            try {
                host_->post([self = shared_from_this()] { self->turn(); });
                return;
            } catch (...) {
                // The host took nothing.
            }
            scheduled_.store(false);
            // A wake that has asked its last time returns without taking the note: another wake that counted on a
            // re-ask may still be in flight, and the note is the only thing that makes its refusal ask again.
            if (++asked == max_asks || !relied_.exchange(false)) return;
        }
    }

    // The deadline stays noted until Asio has dispatched the timers up to it in the same timer queue (see watch),
    // whatever the host's clock says.
    void wake_at(sp::runtime::SteadyTime at) noexcept { wake_at(steady_deadlines_, at); }
    void wake_at_runtime(sp::runtime::SteadyTime at) noexcept { wake_at(runtime_deadlines_, at); }

    // Takes back one noted deadline for `at`: the earliest note for that time in that timer queue. The loop then
    // waits on, and arms the host timer for, the next one, or for none.
    void forget_at(sp::runtime::SteadyTime at) noexcept { forget_at(steady_deadlines_, at); }
    void forget_at_runtime(sp::runtime::SteadyTime at) noexcept { forget_at(runtime_deadlines_, at); }

    bool idle() const noexcept { return closed_.load() || (!scheduled_.load() && !running_ && timer_ == 0); }

    void arm_simulated(SimulatedHostTimer& timer, RuntimeClock::time_point at) {
        std::vector<ClaimedSimulatedWait> cancelled;
        {
            std::lock_guard<std::mutex> lock(simulated_mutex_);
            has_simulated_.store(true, std::memory_order_release);
            if (timer.armed_deadline) {
                simulated_deadlines_.erase(*timer.armed_deadline);
                timer.armed_deadline.reset();
            }
            for (auto it = simulated_waits_.begin(); it != simulated_waits_.end();) {
                if (it->second.owner == &timer) {
                    cancelled.push_back(
                        ClaimedSimulatedWait{std::move(it->second.slot_state), std::move(it->second.handler)});
                    it = simulated_waits_.erase(it);
                } else {
                    ++it;
                }
            }
            timer.expiry = at;
            if (!closed_.load() && at < (RuntimeClock::time_point::max)()) {
                const auto key = std::make_pair(at, ++simulated_seq_);
                simulated_deadlines_.emplace(key, &timer);
                timer.armed_deadline = key;
            }
        }
        for (auto& wait : cancelled) {
            detach_slot(wait.slot_state);
            if (wait.handler) wait.handler->post(asio::error::operation_aborted);
        }
        rearm_simulated();
    }

    std::size_t cancel_simulated(SimulatedHostTimer& timer) noexcept {
        std::vector<ClaimedSimulatedWait> cancelled;
        {
            std::lock_guard<std::mutex> lock(simulated_mutex_);
            if (timer.armed_deadline) {
                simulated_deadlines_.erase(*timer.armed_deadline);
                timer.armed_deadline.reset();
            }
            for (auto it = simulated_waits_.begin(); it != simulated_waits_.end();) {
                if (it->second.owner == &timer) {
                    cancelled.push_back(
                        ClaimedSimulatedWait{std::move(it->second.slot_state), std::move(it->second.handler)});
                    it = simulated_waits_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        const std::size_t count = cancelled.size();
        for (auto& wait : cancelled) {
            detach_slot(wait.slot_state);
            if (wait.handler) {
                try {
                    wait.handler->post(asio::error::operation_aborted);
                } catch (...) {
                }
            }
        }
        rearm_simulated();
        return count;
    }

    void async_wait_simulated(const std::shared_ptr<SimulatedHostTimer>& timer,
                              asio::cancellation_slot slot,
                              std::unique_ptr<SimulatedWaitHandlerBase> handler) {
        if (closed_.load() || !host_) {
            if (handler) handler->post(asio::error::operation_aborted);
            return;
        }
        RuntimeClock::time_point deadline{};
        {
            std::lock_guard<std::mutex> lock(simulated_mutex_);
            if (closed_.load()) {
                if (handler) handler->post(asio::error::operation_aborted);
                return;
            }
            has_simulated_.store(true, std::memory_order_release);
            deadline = timer->expiry;
            const SimulatedWaitKey key{deadline, ++simulated_seq_};
            std::shared_ptr<SimulatedSlotState> slot_state;
            if (slot.is_connected()) {
                slot_state = std::make_shared<SimulatedSlotState>(slot, shared_from_this(), key);
                slot.assign([slot_state](asio::cancellation_type type) {
                    if ((type & (asio::cancellation_type::terminal |
                                 asio::cancellation_type::partial |
                                 asio::cancellation_type::total)) == asio::cancellation_type::none) {
                        return;
                    }
                    std::shared_ptr<HostLoopCore> locked_core;
                    SimulatedWaitKey wait_key{};
                    {
                        std::lock_guard<std::mutex> slot_lock(slot_state->mutex);
                        if (!slot_state->active) return;
                        slot_state->active = false;
                        locked_core = slot_state->core.lock();
                        wait_key = slot_state->key;
                    }
                    if (locked_core) locked_core->cancel_simulated_wait_from_slot(wait_key);
                });
            }
            simulated_waits_.emplace(
                key, PendingSimulatedWait{timer.get(), std::move(slot_state), std::move(handler)});
        }
        rearm_simulated();
    }

private:
    struct SimulatedWaitKey {
        RuntimeClock::time_point deadline;
        std::uint64_t sequence;

        bool operator<(const SimulatedWaitKey& other) const noexcept {
            if (deadline != other.deadline) return deadline < other.deadline;
            return sequence < other.sequence;
        }
    };

    struct SimulatedSlotState {
        SimulatedSlotState(asio::cancellation_slot s,
                           std::weak_ptr<HostLoopCore> c,
                           SimulatedWaitKey k) noexcept
            : slot(s), core(std::move(c)), key(k) {}

        std::mutex mutex;
        asio::cancellation_slot slot;
        std::weak_ptr<HostLoopCore> core;
        SimulatedWaitKey key;
        bool active = true;
    };

    struct PendingSimulatedWait {
        SimulatedHostTimer* owner = nullptr;
        std::shared_ptr<SimulatedSlotState> slot_state;
        std::unique_ptr<SimulatedWaitHandlerBase> handler;
    };

    struct ClaimedSimulatedWait {
        std::shared_ptr<SimulatedSlotState> slot_state;
        std::unique_ptr<SimulatedWaitHandlerBase> handler;
    };

    static void detach_slot(const std::shared_ptr<SimulatedSlotState>& slot_state) noexcept {
        if (!slot_state) return;
        std::lock_guard<std::mutex> lock(slot_state->mutex);
        if (!slot_state->active) return;
        slot_state->active = false;
        if (slot_state->slot.is_connected()) slot_state->slot.clear();
    }

    // A simulated wait can be registered or cancelled on any thread: a cancellation slot is emitted where the canceller
    // runs. Only the host thread touches the host timer, so a call outside a turn asks for a turn, which completes what
    // is due and, when it ends, arms the host timer for the earliest deadline (see turn). A call inside a turn needs
    // nothing: that turn does both.
    void rearm_simulated() noexcept {
        if (closed_.load() || !host_ || runs_turn_of(this)) return;
        wake();
    }

    void cancel_simulated_wait_from_slot(SimulatedWaitKey key) noexcept {
        std::unique_ptr<SimulatedWaitHandlerBase> handler;
        {
            std::lock_guard<std::mutex> lock(simulated_mutex_);
            const auto it = simulated_waits_.find(key);
            if (it == simulated_waits_.end()) return;
            SimulatedHostTimer* const owner = it->second.owner;
            handler = std::move(it->second.handler);
            simulated_waits_.erase(it);
            if (owner && owner->armed_deadline) {
                bool has_other_wait = false;
                for (const auto& [other_key, other_wait] : simulated_waits_) {
                    if (other_wait.owner == owner) {
                        has_other_wait = true;
                        break;
                    }
                }
                if (!has_other_wait) {
                    simulated_deadlines_.erase(*owner->armed_deadline);
                    owner->armed_deadline.reset();
                }
            }
        }
        if (handler) {
            try {
                handler->post(asio::error::operation_aborted);
            } catch (...) {
            }
        }
        rearm_simulated();
    }

    bool complete_due_simulated_waits(RuntimeClock::time_point now) {
        std::vector<ClaimedSimulatedWait> due;
        {
            std::lock_guard<std::mutex> lock(simulated_mutex_);
            for (auto it = simulated_deadlines_.begin();
                 it != simulated_deadlines_.end() && it->first.first <= now;) {
                if (it->second) it->second->armed_deadline.reset();
                it = simulated_deadlines_.erase(it);
            }
            for (auto it = simulated_waits_.begin();
                 it != simulated_waits_.end() &&
                 it->first.deadline < (RuntimeClock::time_point::max)() &&
                 it->first.deadline <= now;) {
                if (it->second.owner && it->second.owner->armed_deadline &&
                    it->second.owner->armed_deadline->first <= now) {
                    simulated_deadlines_.erase(*it->second.owner->armed_deadline);
                    it->second.owner->armed_deadline.reset();
                }
                due.push_back(
                    ClaimedSimulatedWait{std::move(it->second.slot_state), std::move(it->second.handler)});
                it = simulated_waits_.erase(it);
            }
        }
        for (auto& wait : due) {
            detach_slot(wait.slot_state);
            if (wait.handler) wait.handler->post(asio::error_code{});
        }
        return !due.empty();
    }

    void retire_due_simulated_deadlines(RuntimeClock::time_point now) noexcept {
        std::lock_guard<std::mutex> lock(simulated_mutex_);
        for (auto it = simulated_deadlines_.begin();
             it != simulated_deadlines_.end() && it->first.first <= now;) {
            if (it->second) it->second->armed_deadline.reset();
            it = simulated_deadlines_.erase(it);
        }
    }

    std::optional<RuntimeClock::time_point> earliest_simulated_deadline() const noexcept {
        if (!has_simulated_.load(std::memory_order_acquire)) return std::nullopt;
        std::lock_guard<std::mutex> lock(simulated_mutex_);
        std::optional<RuntimeClock::time_point> earliest;
        if (!simulated_deadlines_.empty()) {
            earliest = simulated_deadlines_.begin()->first.first;
        }
        if (!simulated_waits_.empty()) {
            const auto wait_deadline = simulated_waits_.begin()->first.deadline;
            if (wait_deadline < (RuntimeClock::time_point::max)()) {
                if (!earliest || wait_deadline < *earliest) earliest = wait_deadline;
            }
        }
        return earliest;
    }

    template <typename Clock>
    struct DeadlineQueue {
        std::optional<asio::basic_waitable_timer<Clock>> sentinel;
        // Deadlines whose timers Asio has not been seen to dispatch in this clock's timer queue,
        // each with the order it was noted in.
        std::set<std::pair<sp::runtime::SteadyTime, std::uint64_t>> deadlines;
        std::uint64_t noted = 0;
        sp::runtime::SteadyTime watched{};
        bool watching = false;
    };

    template <typename Clock>
    void wake_at(DeadlineQueue<Clock>& queue, sp::runtime::SteadyTime at) noexcept {
        if (closed_.load() || !host_) return;
        try {
            queue.deadlines.emplace(at, ++queue.noted);
            watch(queue);
            if (running_) return;  // the turn that is running arms the host timer when it ends
            if (at <= host_->now()) {
                wake();
            } else {
                arm();
            }
        } catch (...) {
        }
    }

    template <typename Clock>
    void forget_at(DeadlineQueue<Clock>& queue, sp::runtime::SteadyTime at) noexcept {
        if (closed_.load()) return;
        try {
            const auto noted = queue.deadlines.lower_bound({at, 0});
            if (noted == queue.deadlines.end() || noted->first != at) return;
            queue.deadlines.erase(noted);
            if (queue.deadlines.empty() && queue.sentinel) {
                queue.sentinel->cancel();
                queue.watching = false;
            } else {
                watch(queue);
            }
            if (!running_) arm();
        } catch (...) {
        }
    }

    void turn() {
        // Cleared before the poll, so that a post that lands after this point schedules the next turn (see wake).
        scheduled_.store(false);
        if (closed_.load() || !io_ || !host_ || running_) return;
        running_ = true;
        (void)drain_handoffs(false);
        const auto started = host_->now();
        bool more = false;
        std::exception_ptr failure;
        struct TurnScope {
            explicit TurnScope(const HostLoopCore* core) noexcept : link{core, host_turn_top} {
                ++host_turn_depth;
                host_turn_top = &link;
            }
            ~TurnScope() {
                --host_turn_depth;
                host_turn_top = link.outer;
            }
            HostTurnLink link;
        } in_turn(this);
        try {
            if (has_simulated_.load(std::memory_order_acquire)) {
                (void)complete_due_simulated_waits(RuntimeClock::now());
            }
            io_->restart();
            std::size_t handled = 0;
            for (;;) {
                if (io_->poll_one() == 0) {
                    const bool completed_simulated =
                        has_simulated_.load(std::memory_order_acquire) &&
                        complete_due_simulated_waits(RuntimeClock::now());
                    if (!drain_handoffs(false) && !completed_simulated) break;
                    io_->restart();
                    continue;
                }
                if (++handled == options_.turn_handlers) {
                    more = true;
                    break;
                }
            }
        } catch (...) {
            failure = std::current_exception();
            more = true;
        }
        running_ = false;
        if (!closed_.load()) {
            if (has_simulated_.load(std::memory_order_acquire)) retire_due_simulated_deadlines(RuntimeClock::now());
            if (more) wake();
            arm(started);
        }
        if (!closed_.load() && failure && options_.on_error) {
            try {
                options_.on_error(failure);
            } catch (...) {
            }
        }
    }

    // Asio decides when a timer is dispatched, and a poll at the host's deadline can find it not dispatched yet:
    // Asio's Windows reactor learns that a timer is due from a thread of its own, and a host's clock can run ahead
    // of Asio's. Asio also keeps one timer queue per clock type, and timer_queue_set::get_ready_timers reads each
    // queue's clock at a different instant, so each deadline queue (steady_clock for public note_deadline,
    // RuntimeClock for HostTimer) waits on a sentinel in its own Asio timer queue at the earliest deadline noted
    // there. Asio dispatches every timer that is due in a queue in one step, so when that queue's sentinel runs,
    // the timers in that queue that expire by then and were waiting before it was set have been dispatched, and
    // their deadlines are done.
    template <typename Clock>
    void watch(DeadlineQueue<Clock>& queue) noexcept {
        try {
            if (!io_ || queue.deadlines.empty()) return;
            if (!queue.sentinel) queue.sentinel.emplace(*io_);
            const auto next = queue.deadlines.begin()->first;
            if (queue.watching && queue.watched == next) return;
            queue.sentinel->expires_at(next);  // a wait that is pending ends with operation_aborted
            queue.watching = true;
            queue.watched = next;
            queue.sentinel->async_wait([this, &queue, next, covers = queue.noted](const asio::error_code& error) {
                if (!error) dispatched(queue, next, covers);
            });
        } catch (...) {
            queue.watching = false;
        }
    }

    template <typename Clock>
    void dispatched(DeadlineQueue<Clock>& queue, sp::runtime::SteadyTime up_to, std::uint64_t covers) {
        if (closed_.load()) return;
        if (queue.watched <= up_to) queue.watching = false;
        for (auto it = queue.deadlines.begin(); it != queue.deadlines.end() && it->first <= up_to;)
            it = it->second <= covers ? queue.deadlines.erase(it) : std::next(it);
        watch(queue);
    }

    // The host timer follows the earliest deadline across both timer queues that Asio has not been seen to dispatch,
    // and any simulated HostTimer wait or deadline. `polled` is when the turn that has just run started: an earlier
    // Asio deadline that is still noted was not dispatched by that turn, so look again soon instead of at a time that
    // has passed.
    void arm(std::optional<sp::runtime::SteadyTime> polled = std::nullopt) noexcept {
        if (closed_.load() || !host_) return;
        try {
            const auto next_for = [&](const auto& queue) {
                auto next = queue.deadlines.begin()->first;
                if (polled && next <= *polled) next = host_->now() + redispatch_delay;
                return next;
            };
            std::optional<sp::runtime::SteadyTime> next;
            if (!steady_deadlines_.deadlines.empty()) {
                next = next_for(steady_deadlines_);
            }
            if (!runtime_deadlines_.deadlines.empty()) {
                const auto candidate = next_for(runtime_deadlines_);
                next = next ? std::min(*next, candidate) : candidate;
            }
            if (const auto simulated = earliest_simulated_deadline()) {
                auto candidate = *simulated;
                if (polled && candidate <= *polled && candidate > RuntimeClock::now()) {
                    candidate = host_->now() + redispatch_delay;
                }
                next = next ? std::min(*next, candidate) : candidate;
            }
            if (!next) {
                disarm();
                return;
            }
            if (timer_ != 0 && armed_at_ == *next) return;
            disarm();
            timer_ = host_->schedule(*next, [self = shared_from_this()] { self->timer_fired(); });
            armed_at_ = *next;
        } catch (...) {
            timer_ = 0;
        }
    }

    void disarm() noexcept {
        if (timer_ == 0) return;
        const auto timer = timer_;
        timer_ = 0;
        if (host_) host_->cancel(timer);
    }

    void timer_fired() {
        timer_ = 0;
        wake();
    }

    // How soon a turn looks again for a timer that Asio has not dispatched at its deadline.
    static constexpr std::chrono::milliseconds redispatch_delay{1};
    // How many times one wake asks a host that refuses for a turn: the refusal of a wake that others counted on is
    // asked again, but a host that keeps refusing is not asked for ever.
    static constexpr int max_asks = 3;

    std::shared_ptr<sp::runtime::Executor> host_;
    HostLoop::Options options_;
    std::optional<asio::io_context> io_;
    DeadlineQueue<std::chrono::steady_clock> steady_deadlines_;
    DeadlineQueue<RuntimeClock> runtime_deadlines_;
    // Simulated HostTimer waits and deadlines (see HostTimer): registered, completed and cancelled from any thread.
    mutable std::mutex simulated_mutex_;
    std::atomic<bool> has_simulated_{false};
    std::uint64_t simulated_seq_ = 0;
    std::map<std::pair<RuntimeClock::time_point, std::uint64_t>, SimulatedHostTimer*> simulated_deadlines_;
    std::map<SimulatedWaitKey, PendingSimulatedWait> simulated_waits_;
    sp::runtime::Executor::Timer timer_ = 0;
    sp::runtime::SteadyTime armed_at_{};
    // What wake reads from any thread. Everything else here belongs to the host thread, except `host_`, which is only
    // read while the loop is open and is released on the host thread at the end of close().
    std::atomic<bool> scheduled_{false}, closed_{false};
    // Set by a wake that found a turn scheduled and returned; read and cleared by a wake whose post was refused.
    std::atomic<bool> relied_{false};
    // The gate and what it guards (see post_handoff): work that pool threads are doing for this loop's coroutines.
    std::mutex handoff_mutex_;
    std::condition_variable wake_done_;
    std::unordered_set<LoopHandoff*> handoffs_;
    std::vector<std::pair<Resume, asio::any_io_executor>> abandoned_;
    std::vector<std::pair<Resume, asio::any_io_executor>> spare_;
    std::size_t waking_ = 0;
    // Whether a turn is running.
    bool running_ = false;
};

void host_loop_wake(HostLoopCore* core) noexcept {
    if (core) core->wake();
}

std::shared_ptr<SimulatedHostTimer> make_simulated_host_timer(const asio::any_io_executor& executor) {
    try {
        auto& context = asio::query(executor, asio::execution::context);
        if (!asio::has_service<HostLoopService>(context)) return nullptr;
        if (auto* core = asio::use_service<HostLoopService>(context).core) {
            return std::make_shared<SimulatedHostTimer>(core->shared_from_this());
        }
    } catch (...) {
    }
    return nullptr;
}

void arm_simulated_host_timer(SimulatedHostTimer& timer, RuntimeClock::time_point at) {
    if (auto core = timer.core.lock()) {
        core->arm_simulated(timer, at);
    } else {
        timer.expiry = at;
    }
}

std::size_t cancel_simulated_host_timer(SimulatedHostTimer& timer) noexcept {
    if (auto core = timer.core.lock()) return core->cancel_simulated(timer);
    return 0;
}

void async_wait_simulated_host_timer(const std::shared_ptr<SimulatedHostTimer>& timer,
                                     asio::cancellation_slot slot,
                                     std::unique_ptr<SimulatedWaitHandlerBase> handler) {
    if (!timer) {
        if (handler) handler->post(asio::error::operation_aborted);
        return;
    }
    if (auto core = timer->core.lock()) {
        core->async_wait_simulated(timer, slot, std::move(handler));
    } else if (handler) {
        handler->post(asio::error::operation_aborted);
    }
}

namespace {
std::atomic<std::size_t> active_handoffs{0};
}  // namespace

LoopHandoff::LoopHandoff(asio::any_io_executor executor, Resume resume, std::shared_ptr<HostLoopCore> core)
    : core_(std::move(core)), executor_(std::move(executor)), resume_(std::move(resume)) {
    active_handoffs.fetch_add(1, std::memory_order_acq_rel);
}

std::shared_ptr<LoopHandoff> LoopHandoff::open(const asio::any_io_executor& executor, Resume resume) {
    std::shared_ptr<HostLoopCore> core;
    try {
        auto& context = asio::query(executor, asio::execution::context);
        if (asio::has_service<HostLoopService>(context)) {
            if (auto* found = asio::use_service<HostLoopService>(context).core) core = found->shared_from_this();
        }
    } catch (...) {
    }
    if (!core) throw std::logic_error("LoopHandoff requires a HostLoop executor");
    std::shared_ptr<LoopHandoff> handoff(new LoopHandoff(executor, std::move(resume), nullptr));
    auto* raw_core = core.get();
    raw_core->track(handoff.get(), std::move(core));
    return handoff;
}

LoopHandoff::~LoopHandoff() {
    if (auto core = std::move(core_)) core->release_handoff(*this);
    active_handoffs.fetch_sub(1, std::memory_order_acq_rel);
}

void LoopHandoff::finish() noexcept {
    if (auto core = std::move(core_)) core->post_handoff(*this);
}

std::size_t LoopHandoff::active_count() noexcept {
    return active_handoffs.load(std::memory_order_acquire);
}

void set_simulated_clock_for_testing(SimulatedClock* clock) noexcept {
    simulated_clock.store(clock, std::memory_order_release);
}

void note_deadline(const asio::any_io_executor& executor,
                   std::chrono::steady_clock::time_point at,
                   RuntimeClockTag) noexcept {
    try {
        auto& context = asio::query(executor, asio::execution::context);
        if (!asio::has_service<HostLoopService>(context)) return;
        if (auto* core = asio::use_service<HostLoopService>(context).core) core->wake_at_runtime(at);
    } catch (...) {
    }
}

void forget_deadline(const asio::any_io_executor& executor,
                     std::chrono::steady_clock::time_point at,
                     RuntimeClockTag) noexcept {
    try {
        auto& context = asio::query(executor, asio::execution::context);
        if (!asio::has_service<HostLoopService>(context)) return;
        if (auto* core = asio::use_service<HostLoopService>(context).core) core->forget_at_runtime(at);
    } catch (...) {
    }
}

}  // namespace detail

HostLoop::HostLoop(std::shared_ptr<sp::runtime::Executor> host, Options options)
    : core_(std::make_shared<detail::HostLoopCore>(std::move(host), std::move(options))) {}

HostLoop::~HostLoop() { core_->close(); }

asio::any_io_executor HostLoop::executor() { return core_->executor(); }

void HostLoop::wake() noexcept { core_->wake(); }

void HostLoop::wake_at(sp::runtime::SteadyTime at) noexcept { core_->wake_at(at); }

void HostLoop::forget_at(sp::runtime::SteadyTime at) noexcept { core_->forget_at(at); }

bool HostLoop::idle() const noexcept { return core_->idle(); }

void forget_deadline(const asio::any_io_executor& executor, std::chrono::steady_clock::time_point at) noexcept {
    try {
        auto& context = asio::query(executor, asio::execution::context);
        if (!asio::has_service<detail::HostLoopService>(context)) return;
        if (auto* core = asio::use_service<detail::HostLoopService>(context).core) core->forget_at(at);
    } catch (...) {
    }
}

bool in_host_turn() noexcept { return detail::host_turn_depth != 0; }

bool is_host_pumped(const asio::any_io_executor& executor) noexcept {
    try {
        auto& context = asio::query(executor, asio::execution::context);
        return asio::has_service<detail::HostLoopService>(context) &&
               asio::use_service<detail::HostLoopService>(context).core != nullptr;
    } catch (...) {
        return false;
    }
}

void note_deadline(const asio::any_io_executor& executor, std::chrono::steady_clock::time_point at) noexcept {
    try {
        auto& context = asio::query(executor, asio::execution::context);
        if (!asio::has_service<detail::HostLoopService>(context)) return;
        if (auto* core = asio::use_service<detail::HostLoopService>(context).core) core->wake_at(at);
    } catch (...) {
    }
}

}  // namespace neograph::async
