#pragma once

// A checkpoint store whose saves all carry one timestamp, as on a host where a
// save returns inside the millisecond (SQLite on macOS, or on tmpfs). A store
// must then tell a thread's latest checkpoint by save order alone: the
// timestamps tie, a later run restarts at step 0, and two gates of one step
// save the same step.

#include <neograph/graph/checkpoint.h>

#include <asio/awaitable.hpp>

#include <cstdint>

namespace neograph::graph::test_support {

template <typename Store>
class CoarseClockStore final : public Store {
public:
    static constexpr std::int64_t kTimestamp = 1;

    using Store::Store;

    void save(const Checkpoint& cp) override { Store::save(pinned(cp)); }

    asio::awaitable<void> save_async(const Checkpoint& cp) override {
        co_await Store::save_async(pinned(cp));
    }

private:
    static Checkpoint pinned(Checkpoint cp) {
        cp.timestamp = kTimestamp;
        return cp;
    }
};

}  // namespace neograph::graph::test_support
