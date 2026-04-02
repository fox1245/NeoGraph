#pragma once

#include <neograph/graph/types.h>
#include <optional>
#include <mutex>
#include <map>
#include <vector>
#include <chrono>

namespace neograph::graph {

// =========================================================================
// Checkpoint: serialized snapshot of graph execution state
// =========================================================================
struct Checkpoint {
    std::string id;                // UUID v4
    std::string thread_id;         // conversation/session identifier
    json        channel_values;    // serialized channel data
    json        channel_versions;  // per-channel version counters
    std::string parent_id;         // previous checkpoint (for time-travel chain)
    std::string current_node;      // node that was active at checkpoint time
    std::string next_node;         // node to execute on resume
    std::string interrupt_phase;   // "before", "after", or "completed"
    json        metadata;          // user-defined metadata
    int64_t     step;              // super-step number
    int64_t     timestamp;         // unix epoch millis

    // Generate UUID v4
    static std::string generate_id();
};

// =========================================================================
// CheckpointStore: abstract interface for checkpoint persistence
// =========================================================================
class CheckpointStore {
public:
    virtual ~CheckpointStore() = default;

    virtual void save(const Checkpoint& cp) = 0;
    virtual std::optional<Checkpoint> load_latest(const std::string& thread_id) = 0;
    virtual std::optional<Checkpoint> load_by_id(const std::string& id) = 0;
    virtual std::vector<Checkpoint> list(const std::string& thread_id,
                                          int limit = 100) = 0;
    virtual void delete_thread(const std::string& thread_id) = 0;
};

// =========================================================================
// InMemoryCheckpointStore: for testing and single-process use
// =========================================================================
class InMemoryCheckpointStore : public CheckpointStore {
public:
    void save(const Checkpoint& cp) override;
    std::optional<Checkpoint> load_latest(const std::string& thread_id) override;
    std::optional<Checkpoint> load_by_id(const std::string& id) override;
    std::vector<Checkpoint> list(const std::string& thread_id,
                                  int limit = 100) override;
    void delete_thread(const std::string& thread_id) override;

    // Test helper: total checkpoint count
    size_t size() const;

private:
    mutable std::mutex mutex_;
    // thread_id -> checkpoints (ordered by timestamp)
    std::map<std::string, std::vector<Checkpoint>> by_thread_;
    // checkpoint_id -> checkpoint
    std::map<std::string, Checkpoint> by_id_;
};

} // namespace neograph::graph
