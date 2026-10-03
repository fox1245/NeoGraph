/**
 * @file graph/state.h
 * @brief Thread-safe mutable graph state management.
 *
 * GraphState manages all state channels used during graph execution.
 * All read/write operations are thread-safe using a shared mutex, supporting
 * concurrent node execution on the caller's Asio executor or the engine's
 * opt-in worker pool.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/channel_key.h>
#include <neograph/graph/types.h>
#include <neograph/graph/run_context.h>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <vector>
#include <core/native_archive.h>

namespace neograph::graph {
namespace detail { class ManagedBudgetJournalAccess; }
class NativeGraphCheckpoint final {
private:
    struct History {
        std::vector<sp::Message> messages;
        json projection = json::array();
    };
    struct BudgetBinding {
        std::shared_ptr<UsageAccumulator> bank;
        std::uint64_t ceiling = 0;
        std::uint64_t original_ceiling = 0;
        bool managed = false;
        std::string owner_scope, thread_id, graph_identity;
        std::string original_thread_id, fork_source_thread_id;
    };
    std::map<std::string, History> histories_;
    std::map<std::string, ProviderLoopHistory::Entry> loops_;
    std::vector<sp::runtime::Result> outcomes_;
    std::optional<BudgetBinding> budget_;
    json projection_;
    NativeGraphCheckpoint() = default;
    friend class GraphState;
    friend class detail::ManagedBudgetJournalAccess;
};

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
     * @brief Initialize a new channel with reducer and lifecycle policy.
     *
     * Writes are combined by the reducer, normalized by retention, and then
     * projected to checkpoints according to persistence. Existing callers
     * that pass only an initial value retain the historical unbounded,
     * checkpointed behavior.
     */
    void init_channel(const std::string& name,
                      ReducerType type,
                      ReducerFn reducer,
                      const json& initial_value = json(),
                      ChannelLifecyclePolicy lifecycle = {});

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
    /// Full ordered provider history; JSON is only its portable projection.
    std::vector<sp::Message> get_provider_messages(const std::string& channel = "messages") const;
    /// Captured C++ history only; never interprets an arbitrary JSON channel.
    std::optional<std::vector<sp::Message>> captured_provider_messages(
        const std::string& channel = "messages") const;
    void set_native_history_archive(std::shared_ptr<sp::NativeArchive> archive);
    void copy_provider_history_from(const GraphState& source);
    void set_provider_run_history(std::shared_ptr<ProviderLoopHistory> loops,
                                  std::shared_ptr<ProviderOutcomes> outcomes);
    std::vector<sp::runtime::Result> provider_outcomes() const;
    void configure_budget_bank(std::shared_ptr<UsageAccumulator> bank, std::uint64_t ceiling,
        bool managed, std::string owner_scope, std::string thread_id, std::string graph_identity);
    std::shared_ptr<UsageAccumulator> budget_bank() const;
    std::uint64_t budget_ceiling() const;
    std::uint64_t budget_original_ceiling() const;
    std::string budget_original_thread_id() const;
    bool budget_managed() const;
    void defer_budget_authority_restore();
    void activate_budget_authority(bool observation_only = false);
    void rebind_fork_budget_thread(std::string thread_id, bool original_in_memory_custody);
    void validate_budget_context(const std::string& graph_identity, const std::string& thread_id) const;

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
     * @brief Serialize checkpointed channel values and versions to JSON.
     * @return JSON snapshot without ephemeral channel values.
     */
    json serialize() const;

    /**
     * @brief Restore durable channels from a JSON snapshot.
     * @param data JSON object previously produced by serialize().
     */
    void restore(const json& data);
    /// Isolated in-process Send workers need every live channel, including
    /// ephemeral values. Never pass this representation to a checkpoint store.
    json serialize_runtime() const;
    /// Cacheable channel/native inputs, excluding operational budget authority.
    json serialize_cache() const;
    void restore_runtime(const json& data);

    /// Record which non-checkpointed channels existed at this checkpoint.
    /// A written ephemeral value cannot be reconstructed on resume.
    json ephemeral_checkpoint_guard() const;

    /// Reject missing/incompatible guards and lost ephemeral values before
    /// restoring a durable checkpoint.
    void restore_checkpoint(const json& data, const json& guard,
                            std::shared_ptr<const NativeGraphCheckpoint> native = {});
    std::pair<json, std::shared_ptr<const NativeGraphCheckpoint>> checkpoint_snapshot() const;


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
    using ProviderHistory = NativeGraphCheckpoint::History;
    std::map<std::string, ProviderHistory> provider_histories_;
    std::shared_ptr<sp::NativeArchive> native_history_archive_;
    std::shared_ptr<ProviderLoopHistory> provider_loops_;
    std::shared_ptr<ProviderOutcomes> provider_outcomes_;
    std::optional<NativeGraphCheckpoint::BudgetBinding> provider_budget_;
    bool defer_budget_authority_ = false;
    std::optional<UsageAccumulator::AuthoritySnapshot> deferred_budget_authority_;
    std::shared_ptr<UsageAccumulator> deferred_budget_bank_;
    void admit_budget_binding(NativeGraphCheckpoint::BudgetBinding saved,
                              std::optional<UsageAccumulator::AuthoritySnapshot> authority);
    json managed_budget_projection_locked() const;
    void update_provider_history_locked(const std::string& channel, const json& before, const json& incoming,
        const json& after, const std::shared_ptr<const std::vector<sp::Message>>& native);
    void save_provider_history_locked(json& snapshot, bool durable = true, bool include_budget = true) const;
    json serialize_runtime_locked(bool include_budget) const;
    void restore_provider_history_locked(const json& snapshot, bool trusted_runtime_copy = false);
    mutable std::shared_mutex mutex_;
};

} // namespace neograph::graph
