#include <neograph/async/host_loop.h>

#include <asio/execution_context.hpp>

#include <optional>
#include <set>
#include <stdexcept>

namespace neograph::async {
namespace detail {

class HostLoopCore;

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
        closed_ = true;
        disarm();
        deadlines_.clear();
        // Destroys the coroutines that have not finished. Their frames may wake the loop, which is closed.
        io_.reset();
    }

    // A turn is scheduled on the host. Work posted during a turn is run by that turn.
    void wake() noexcept {
        if (closed_ || scheduled_ || running_) return;
        scheduled_ = true;
        try {
            host_->post([self = shared_from_this()] { self->turn(); });
        } catch (...) {
            scheduled_ = false;
        }
    }

    void wake_at(sp::runtime::SteadyTime at) noexcept {
        if (closed_) return;
        try {
            if (at <= host_->now()) {
                wake();
                return;
            }
            deadlines_.insert(at);
            if (!running_) arm();
        } catch (...) {
        }
    }

    bool idle() const noexcept { return closed_ || (!scheduled_ && !running_ && timer_ == 0); }

private:
    void turn() {
        scheduled_ = false;
        if (closed_ || running_) return;
        running_ = true;
        const auto started = host_->now();
        bool more = false;
        bool drained = false;
        std::exception_ptr failure;
        try {
            io_->restart();
            std::size_t handled = 0;
            for (;;) {
                if (io_->poll_one() == 0) {
                    drained = true;
                    break;
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
        // A poll that ran dry ran the reactor after `started`, so every timer due by then has fired.
        if (drained) deadlines_.erase(deadlines_.begin(), deadlines_.upper_bound(started));
        if (failure && options_.on_error) {
            try {
                options_.on_error(failure);
            } catch (...) {
            }
        }
        if (closed_) return;
        if (more) wake();
        arm();
    }

    // The host timer follows the earliest noted deadline.
    void arm() noexcept {
        try {
            if (deadlines_.empty()) {
                disarm();
                return;
            }
            const auto next = *deadlines_.begin();
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

    std::shared_ptr<sp::runtime::Executor> host_;
    HostLoop::Options options_;
    std::optional<asio::io_context> io_;
    std::multiset<sp::runtime::SteadyTime> deadlines_;
    sp::runtime::Executor::Timer timer_ = 0;
    sp::runtime::SteadyTime armed_at_{};
    bool scheduled_ = false, running_ = false, closed_ = false;
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

bool HostLoop::idle() const noexcept { return core_->idle(); }

void note_deadline(const asio::any_io_executor& executor, std::chrono::steady_clock::time_point at) noexcept {
    try {
        auto& context = asio::query(executor, asio::execution::context);
        if (!asio::has_service<detail::HostLoopService>(context)) return;
        if (auto* core = asio::use_service<detail::HostLoopService>(context).core) core->wake_at(at);
    } catch (...) {
    }
}

}  // namespace neograph::async
