#include <neograph/async/host_loop.h>

#include <asio/error_code.hpp>
#include <asio/execution_context.hpp>
#include <asio/steady_timer.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace neograph::async {
namespace detail {

class HostLoopCore;

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
        sentinel_.emplace(*io_);
        asio::make_service<HostLoopService>(*io_, this);
    }

    asio::any_io_executor executor() {
        if (!io_) throw std::logic_error("HostLoop is closed");
        return asio::any_io_executor(PumpedExecutor<asio::io_context::executor_type>(io_->get_executor(), this));
    }

    void close() noexcept {
        closed_.store(true);
        disarm();
        deadlines_.clear();
        sentinel_.reset();
        // Destroys the coroutines that have not finished. Their frames may wake the loop, which is closed.
        io_.reset();
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
        if (runs_turn_of(this) && running_) return;
        for (int asked = 0;;) {
            if (scheduled_.exchange(true)) {
                relied_.store(true);
                if (scheduled_.load()) return;
                continue;  // the flag was handed back in the meantime
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

    // The deadline stays noted until Asio has dispatched the timers up to it (see watch), whatever the host's clock
    // says.
    void wake_at(sp::runtime::SteadyTime at) noexcept {
        if (closed_.load()) return;
        try {
            deadlines_.emplace(at, ++noted_);
            watch();
            if (running_) return;  // the turn that is running arms the host timer when it ends
            if (at <= host_->now()) {
                wake();
            } else {
                arm();
            }
        } catch (...) {
        }
    }

    // Takes back one noted deadline for `at`: the earliest note for that time. The loop then waits on, and arms the host
    // timer for, the next one, or for none.
    void forget_at(sp::runtime::SteadyTime at) noexcept {
        if (closed_.load()) return;
        try {
            const auto noted = deadlines_.lower_bound({at, 0});
            if (noted == deadlines_.end() || noted->first != at) return;
            deadlines_.erase(noted);
            if (deadlines_.empty() && sentinel_) {
                sentinel_->cancel();
                watching_ = false;
            } else {
                watch();
            }
            if (!running_) arm();
        } catch (...) {
        }
    }

    bool idle() const noexcept { return closed_.load() || (!scheduled_.load() && !running_ && timer_ == 0); }

private:
    void turn() {
        // Cleared before the poll, so that a post that lands after this point schedules the next turn (see wake).
        scheduled_.store(false);
        if (closed_.load() || running_) return;
        running_ = true;
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
            io_->restart();
            std::size_t handled = 0;
            for (;;) {
                if (io_->poll_one() == 0) break;
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
        if (failure && options_.on_error) {
            try {
                options_.on_error(failure);
            } catch (...) {
            }
        }
        if (closed_.load()) return;
        if (more) wake();
        arm(started);
    }

    // Asio decides when a timer is dispatched, and a poll at the host's deadline can find it not dispatched yet:
    // Asio's Windows reactor learns that a timer is due from a thread of its own, and a host's clock can run ahead
    // of Asio's. So a poll that ran dry does not retire a deadline. The loop waits on a timer of its own at the
    // earliest one instead. Asio dispatches every timer that is due in one step, so when that timer's handler runs,
    // the timers that expire by then and were waiting before it was set have been dispatched, and their deadlines
    // are done.
    void watch() noexcept {
        try {
            if (!sentinel_ || deadlines_.empty()) return;
            const auto next = deadlines_.begin()->first;
            if (watching_ && watched_ == next) return;
            sentinel_->expires_at(next);  // a wait that is pending ends with operation_aborted
            watching_ = true;
            watched_ = next;
            sentinel_->async_wait([this, next, covers = noted_](const asio::error_code& error) {
                if (!error) dispatched(next, covers);
            });
        } catch (...) {
            watching_ = false;
        }
    }

    void dispatched(sp::runtime::SteadyTime up_to, std::uint64_t covers) {
        if (closed_.load()) return;
        if (watched_ <= up_to) watching_ = false;
        for (auto it = deadlines_.begin(); it != deadlines_.end() && it->first <= up_to;)
            it = it->second <= covers ? deadlines_.erase(it) : std::next(it);
        watch();
    }

    // The host timer follows the earliest deadline that Asio has not been seen to dispatch. `polled` is when the
    // turn that has just run started: an earlier deadline that is still noted was not dispatched by that turn, so
    // look again soon instead of at a time that has passed.
    void arm(sp::runtime::SteadyTime polled = {}) noexcept {
        try {
            if (deadlines_.empty()) {
                disarm();
                return;
            }
            auto next = deadlines_.begin()->first;
            if (next <= polled) next = host_->now() + redispatch_delay;
            if (timer_ != 0 && armed_at_ == next) return;
            disarm();
            timer_ = host_->schedule(next, [self = shared_from_this()] { self->timer_fired(); });
            armed_at_ = next;
        } catch (...) {
            timer_ = 0;
        }
    }

    void disarm() noexcept {
        if (timer_ == 0) return;
        const auto timer = timer_;
        timer_ = 0;
        host_->cancel(timer);
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
    std::optional<asio::steady_timer> sentinel_;
    // Deadlines whose timers Asio has not been seen to dispatch, each with the order it was noted in.
    std::set<std::pair<sp::runtime::SteadyTime, std::uint64_t>> deadlines_;
    std::uint64_t noted_ = 0;
    sp::runtime::SteadyTime watched_{};
    bool watching_ = false;
    sp::runtime::Executor::Timer timer_ = 0;
    sp::runtime::SteadyTime armed_at_{};
    // What wake reads from any thread. Everything else here belongs to the host thread, except `host_`, which is only
    // read after the constructor.
    std::atomic<bool> scheduled_{false}, closed_{false};
    // Set by a wake that found a turn scheduled and returned; read and cleared by a wake whose post was refused.
    std::atomic<bool> relied_{false};
    // Whether a turn is running. The host thread's: wake reads it only after finding that it is on that thread.
    bool running_ = false;
};

void host_loop_wake(HostLoopCore* core) noexcept {
    if (core) core->wake();
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
