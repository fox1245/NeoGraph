#pragma once

#include <neograph/program/child_synthesis.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace neograph::program::detail {

/**
 * The child-synthesis records of one run as a control last read them. The log grows only by publications that carry a
 * synthesis record, and each of those calls forget(); a reader that read the log before such a publication and stores
 * it afterwards is refused (its epoch is out of date). A control that was taken over does not use it (the caller reads).
 */
class SynthesisHeadsCache final {
public:
    std::optional<std::vector<ProgramChildSynthesisRecord>> get() const {
        std::lock_guard lock(mutex_);
        return heads_;
    }
    /// The epoch to pass to put() with a log read now.
    std::uint64_t epoch() const {
        std::lock_guard lock(mutex_);
        return epoch_;
    }
    /// Keeps `heads`, read while the epoch was `read_at`, unless a forget() ran since.
    void put(std::vector<ProgramChildSynthesisRecord> heads, std::uint64_t read_at) {
        std::lock_guard lock(mutex_);
        if (read_at == epoch_) heads_ = std::move(heads);
    }
    void forget() {
        std::lock_guard lock(mutex_);
        heads_.reset();
        ++epoch_;
    }

private:
    mutable std::mutex                                       mutex_;
    std::optional<std::vector<ProgramChildSynthesisRecord>> heads_;
    std::uint64_t                                            epoch_ = 0;
};

}  // namespace neograph::program::detail
