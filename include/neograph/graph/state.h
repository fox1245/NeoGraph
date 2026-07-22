/**
 * @file graph/state.h
 * @brief Thread-safe mutable graph state management.
 *
 * GraphState manages all state channels used during graph execution.
 * All read/write operations are thread-safe using a shared mutex,
 * supporting concurrent node execution via Taskflow.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/channel_key.h>
#include <neograph/graph/types.h>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <vector>

namespace neograph::graph {

/**
 * @brief Thread-safe container for all graph state channels.
 *
 * Provides concurrent read access (shared lock) and exclusive write
 * access (unique lock) to state channels. Supports serialization
 * for checkpointing and version tracking for change detection.
 */
class NEOGRAPH_API GraphState {
public:
    /**
     * @brief Initialize a new channel with a reducer and optional initial value.
     * @param name Channel name (must be unique within the graph).
     * @param type Reducer strategy for merging writes.
     * @param reducer Custom reducer function (used when type == ReducerType::CUSTOM).
     * @param initial_value Initial channel value (default: null).
     */
    void init_channel(const std::string& name,
                      ReducerType type,
                      ReducerFn reducer,
                      const json& initial_value = json());

    /**
     * @brief Read a channel's current value (thread-safe, shared lock).
     * @param channel Channel name.
     * @return The current value, or null if the channel does not exist.
     */
    json get(const std::string& channel) const;

    /**
     * @brief Read and convert a channel using a reusable typed key.
     * @throws json::type_error If the current value cannot be converted to T.
     */
    template <typename T>
    T get(const ChannelKey<T>& channel) const {
        return get(channel.name()).template get<T>();
    }

    /**
     * @brief Read a typed channel when it is declared.
     *
     * Missing channels return std::nullopt. Conversion failures remain errors
     * so a schema/type mismatch is not mistaken for absence.
     */
    template <typename T>
    std::optional<T> try_get(const ChannelKey<T>& channel) const {
        if (!has_channel(channel.name())) return std::nullopt;
        return get(channel);
    }

    /**
     * @brief Convenience method to read the "messages" channel as a vector of ChatMessage.
     * @return Vector of ChatMessage objects from the "messages" channel.
     */
    std::vector<ChatMessage> get_messages() const;

    /**
     * @brief Write a value to a single channel through its reducer (exclusive lock).
     * @param channel Channel name.
     * @param value Value to merge via the channel's reducer.
     */
    void write(const std::string& channel, const json& value);

    /**
     * @brief Apply a batch of channel writes atomically (exclusive lock).
     *
     * All writes in the batch are applied under a single lock acquisition,
     * ensuring consistency when multiple channels must be updated together.
     *
     * @param writes Vector of ChannelWrite objects to apply.
     */
    void apply_writes(const std::vector<ChannelWrite>& writes);

    /**
     * @brief Get the version counter of a specific channel.
     * @param channel Channel name.
     * @return The channel's version counter (incremented on each write).
     */
    uint64_t channel_version(const std::string& channel) const;

    /**
     * @brief Get the global version counter across all channels.
     * @return The global version counter.
     */
    uint64_t global_version() const;

    /**
     * @brief Serialize the entire state to JSON (for checkpointing).
     * @return JSON object containing all channel values and versions.
     */
    json serialize() const;

    /**
     * @brief Restore state from a JSON snapshot (for time-travel).
     * @param data JSON object previously produced by serialize().
     */
    void restore(const json& data);

    /**
     * @brief List all channel names in this state.
     * @return Vector of channel name strings.
     */
    std::vector<std::string> channel_names() const;

    /**
     * @brief Whether the graph declared this channel.
     *
     * `write()` throws on an undeclared channel, so code that writes into a
     * channel it does not own (the engine seeding a resume value into
     * "messages", say) must ask first rather than assume the graph is
     * shaped like a chat.
     */
    bool has_channel(const std::string& channel) const;

    // v1.0 (9d): the v0.3 `run_cancel_token_` smuggling channel is gone.
    // Cancel propagation now flows exclusively through
    // `RunContext::cancel_token` on `NodeInput::ctx` (engine threads it
    // through every dispatch; multi-Send fan-out copies the context by
    // value, so the cancel token survives the isolated-state boundary
    // without a serialize/restore hop).

private:
    std::map<std::string, Channel> channels_;
    uint64_t global_version_ = 0;
    mutable std::shared_mutex mutex_;
};

} // namespace neograph::graph
