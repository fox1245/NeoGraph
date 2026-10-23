#pragma once

// A checkpoint store that, once told to, no longer finds a checkpoint by its id, as a store does whose contents are gone
// when the attempt of a resume reads them: the stores of a restarted process do not have to agree.

#include <neograph/graph/checkpoint.h>

#include <atomic>
#include <optional>
#include <string>

namespace neograph::test {

class VanishingCheckpointStore final : public neograph::graph::InMemoryCheckpointStore {
public:
    void vanish() { vanished_.store(true); }
    std::optional<neograph::graph::Checkpoint> load_by_id(const std::string& id) override {
        if (vanished_.load()) return std::nullopt;
        return InMemoryCheckpointStore::load_by_id(id);
    }

private:
    std::atomic<bool> vanished_{false};
};

}  // namespace neograph::test
