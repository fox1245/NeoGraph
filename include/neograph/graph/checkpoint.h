/**
 * @file graph/checkpoint.h
 * @brief Checkpoint system for graph execution state persistence and time-travel.
 *
 * Provides the Checkpoint data structure and the CheckpointStore interface
 * for saving and loading execution state snapshots. Used for HITL (Human-in-the-Loop)
 * interrupt/resume, time-travel debugging, and thread forking.
 */
#pragma once

#include <neograph/graph/types.h>
#include <optional>
#include <mutex>
#include <map>
#include <vector>
#include <chrono>

namespace neograph::graph {

/**
 * @brief Serialized snapshot of graph execution state at a single super-step.
 *
 * Each checkpoint captures the complete state of a graph execution,
 * including all channel values, the active node, and the next node
 * to execute. Checkpoints form a linked list via parent_id for
 * time-travel navigation.
 */
struct Checkpoint {
    std::string id;                ///< Unique checkpoint ID (UUID v4).
    std::string thread_id;         ///< Conversation/session identifier.
    json        channel_values;    ///< Serialized channel data.
    json        channel_versions;  ///< Per-channel version counters.
    std::string parent_id;         ///< Previous checkpoint ID (for time-travel chain).
    std::string current_node;      ///< Node that was active at checkpoint time.
    std::string next_node;         ///< Node to execute on resume.
    std::string interrupt_phase;   ///< Phase: "before", "after", or "completed".
    json        metadata;          ///< User-defined metadata.
    int64_t     step;              ///< Super-step number.
    int64_t     timestamp;         ///< Unix epoch milliseconds.

    /**
     * @brief Generate a new UUID v4 string.
     * @return A random UUID v4 string (e.g., "550e8400-e29b-41d4-a716-446655440000").
     */
    static std::string generate_id();
};

/**
 * @brief Abstract interface for checkpoint persistence backends.
 *
 * Implement this to store checkpoints in databases, files, or other
 * storage systems. The engine uses this interface for save/load operations.
 *
 * @see InMemoryCheckpointStore for a reference implementation.
 */
class CheckpointStore {
public:
    virtual ~CheckpointStore() = default;

    /**
     * @brief Save a checkpoint.
     * @param cp The checkpoint to persist.
     */
    virtual void save(const Checkpoint& cp) = 0;

    /**
     * @brief Load the most recent checkpoint for a thread.
     * @param thread_id Thread identifier.
     * @return The latest checkpoint, or std::nullopt if none exists.
     */
    virtual std::optional<Checkpoint> load_latest(const std::string& thread_id) = 0;

    /**
     * @brief Load a checkpoint by its unique ID.
     * @param id Checkpoint UUID.
     * @return The checkpoint, or std::nullopt if not found.
     */
    virtual std::optional<Checkpoint> load_by_id(const std::string& id) = 0;

    /**
     * @brief List checkpoints for a thread, ordered by timestamp (newest first).
     * @param thread_id Thread identifier.
     * @param limit Maximum number of checkpoints to return (default: 100).
     * @return Vector of checkpoints.
     */
    virtual std::vector<Checkpoint> list(const std::string& thread_id,
                                          int limit = 100) = 0;

    /**
     * @brief Delete all checkpoints for a thread.
     * @param thread_id Thread identifier to delete.
     */
    virtual void delete_thread(const std::string& thread_id) = 0;
};

/**
 * @brief In-memory checkpoint store for testing and single-process use.
 *
 * Stores checkpoints in memory using std::map. Thread-safe via mutex.
 * Not suitable for production use where persistence across restarts is needed.
 */
class InMemoryCheckpointStore : public CheckpointStore {
public:
    void save(const Checkpoint& cp) override;
    std::optional<Checkpoint> load_latest(const std::string& thread_id) override;
    std::optional<Checkpoint> load_by_id(const std::string& id) override;
    std::vector<Checkpoint> list(const std::string& thread_id,
                                  int limit = 100) override;
    void delete_thread(const std::string& thread_id) override;

    /**
     * @brief Get the total number of stored checkpoints (test helper).
     * @return Total checkpoint count across all threads.
     */
    size_t size() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<Checkpoint>> by_thread_;
    std::map<std::string, Checkpoint> by_id_;
};

} // namespace neograph::graph
