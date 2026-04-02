#pragma once

#include <neograph/graph/types.h>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace neograph::graph {

class GraphState {
public:
    // Initialize a channel with a reducer function
    void init_channel(const std::string& name,
                      ReducerType type,
                      ReducerFn reducer,
                      const json& initial_value = json());

    // Read channel value (thread-safe, shared lock)
    json get(const std::string& channel) const;

    // Convenience: read "messages" channel as vector<ChatMessage>
    std::vector<ChatMessage> get_messages() const;

    // Write to a single channel through its reducer (exclusive lock)
    void write(const std::string& channel, const json& value);

    // Apply a batch of writes atomically (exclusive lock)
    void apply_writes(const std::vector<ChannelWrite>& writes);

    // Version tracking
    uint64_t channel_version(const std::string& channel) const;
    uint64_t global_version() const;

    // Serialization (for checkpointing)
    json serialize() const;
    void restore(const json& data);

    // Channel enumeration
    std::vector<std::string> channel_names() const;

private:
    std::map<std::string, Channel> channels_;
    uint64_t global_version_ = 0;
    mutable std::shared_mutex mutex_;
};

} // namespace neograph::graph
