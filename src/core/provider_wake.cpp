#include "provider_wake.h"

#include <asio/bind_cancellation_slot.hpp>
#include <asio/error.hpp>
#include <asio/execution_context.hpp>
#include <asio/experimental/channel_error.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/redirect_error.hpp>
#include <asio/system_error.hpp>
#include <asio/use_awaitable.hpp>

namespace neograph::detail {
bool ProviderWakeSignal::notify() noexcept {
    std::lock_guard lock(gate_);
    if (!channel_) return false;
    pending_ = true;
    // An active consumer will resnapshot. Only a parked consumer needs an
    // executor submission; Asio posts completion and never resumes it inline.
    if (!armed_) return false;
    armed_ = false;
    return channel_->try_send(asio::error_code{});
}
void ProviderWakeSignal::stop() noexcept {
    std::lock_guard lock(gate_);
    channel_ = nullptr;
    armed_ = false;
}
void ProviderWakeSignal::consume() {
    std::lock_guard lock(gate_);
    pending_ = false;
    armed_ = false;
    while (channel_->try_receive([](asio::error_code) {})) {}
}
bool ProviderWakeSignal::prepare_wait() noexcept {
    std::lock_guard lock(gate_);
    if (pending_) return false;
    armed_ = true;
    return true;
}

// Registered after the channel service. Reverse-order context shutdown closes
// all publication gates while that service and its scheduler are still alive.
// Intrusive registration costs no allocation and contains no owning executor.
class ProviderWakePublications final : public asio::execution_context::service {
public:
    static asio::execution_context::id id;
    explicit ProviderWakePublications(asio::execution_context& context)
        : asio::execution_context::service(context) {}

    void attach(ProviderWakeSignal& signal) {
        std::lock_guard lock(gate_);
        if (closed_) throw asio::system_error(asio::error::operation_aborted);
        signal.next_ = head_;
        if (head_) head_->previous_ = &signal;
        head_ = &signal;
    }
    void detach(ProviderWakeSignal& signal) noexcept {
        std::lock_guard lock(gate_);
        if (signal.previous_) signal.previous_->next_ = signal.next_;
        else head_ = signal.next_;
        if (signal.next_) signal.next_->previous_ = signal.previous_;
    }

private:
    void shutdown() override {
        std::lock_guard lock(gate_);
        closed_ = true;
        // A frame detaches before dropping its signal owner. stop() only clears
        // a pointer; it never destroys handlers or invokes consumer code.
        for (auto* signal = head_; signal; signal = signal->next_) signal->stop();
    }
    std::mutex gate_;
    bool closed_ = false;
    ProviderWakeSignal* head_ = nullptr;
};
asio::execution_context::id ProviderWakePublications::id;

ProviderWakeReader::ProviderWakeReader(asio::any_io_executor executor)
    : channel_(executor, 1), signal_(std::make_shared<ProviderWakeSignal>(channel_)) {
    publications_ = &asio::use_service<ProviderWakePublications>(
        asio::query(executor, asio::execution::context));
    publications_->attach(*signal_);
}
ProviderWakeReader::~ProviderWakeReader() {
    signal_->stop();
    publications_->detach(*signal_);
}
void ProviderWakeReader::consume() { signal_->consume(); }
asio::awaitable<asio::error_code> ProviderWakeReader::wait(asio::cancellation_slot slot) {
    asio::error_code error;
    if (!signal_->prepare_wait()) {
        // Yield a useful publication epoch fairly; this is not an empty poll.
        co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
        co_return error;
    }
    co_await channel_.async_receive(asio::bind_cancellation_slot(slot,
        asio::redirect_error(asio::use_awaitable, error)));
    if (error == asio::experimental::error::channel_cancelled)
        error = asio::error::operation_aborted;
    co_return error;
}
} // namespace neograph::detail
