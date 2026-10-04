#pragma once

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/error_code.hpp>
#include <asio/experimental/concurrent_channel.hpp>
#include <memory>
#include <mutex>

namespace neograph::detail {
using ProviderWakeChannel = asio::experimental::concurrent_channel<void(asio::error_code)>;
class ProviderWakePublications;
class ProviderWakeReader;

// SDK callbacks own this gated, non-owning endpoint, never the channel's
// executor/context. Context shutdown detaches it before channel-service teardown.
class ProviderWakeSignal final {
public:
    explicit ProviderWakeSignal(ProviderWakeChannel& channel) noexcept : channel_(&channel) {}
    bool notify() noexcept;
    void stop() noexcept;

private:
    friend class ProviderWakeReader;
    friend class ProviderWakePublications;
    void consume();
    bool prepare_wait() noexcept;
    std::mutex gate_;
    ProviderWakeChannel* channel_;
    bool pending_ = false, armed_ = false;
    // Intrusive, context-owned registration; SDK threads never access these.
    ProviderWakeSignal* previous_ = nullptr;
    ProviderWakeSignal* next_ = nullptr;
};

class ProviderWakeReader final {
public:
    explicit ProviderWakeReader(asio::any_io_executor executor);
    ~ProviderWakeReader();
    ProviderWakeReader(const ProviderWakeReader&) = delete;
    ProviderWakeReader& operator=(const ProviderWakeReader&) = delete;
    ProviderWakeReader(ProviderWakeReader&&) = delete;
    ProviderWakeReader& operator=(ProviderWakeReader&&) = delete;

    std::shared_ptr<ProviderWakeSignal> signal() const noexcept { return signal_; }
    // Under Bridge's data lock, enter active-drain state and snapshot its data.
    void consume();
    asio::awaitable<asio::error_code> wait(asio::cancellation_slot slot);

private:
    ProviderWakeChannel channel_;
    std::shared_ptr<ProviderWakeSignal> signal_;
    ProviderWakePublications* publications_ = nullptr;
};
} // namespace neograph::detail
