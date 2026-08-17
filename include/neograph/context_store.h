#pragma once

#include <neograph/api.h>
#include <neograph/runtime_context.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace neograph {

/** Owner-scoped identity of one append-only runtime-history feed. */
struct ContextStoreFeed {
    std::string owner_id;
    std::string feed_id;
};

/** The current durable head of a feed. An empty feed has sequence zero and no id. */
struct ContextStoreHead {
    std::uint64_t sequence = 0;
    std::optional<std::string> record_id;
};

enum class ContextStoreAppendResult : std::uint8_t {
    Appended,
    AlreadyPresent,
    Conflict,
};

enum class ContextArtifactPutResult : std::uint8_t {
    Stored,
    AlreadyPresent,
    Conflict,
};

/** Immutable reference to an exact contiguous history range, not a feed copy. */
struct ContextHistoryRange {
    std::string owner_id;
    std::string feed_id;
    std::uint64_t from_sequence = 0;
    std::uint64_t through_sequence = 0;
    std::string digest;
    std::string artifact_uri;
};

/**
 * Durable contract for owner-isolated runtime history and context artifacts.
 *
 * Implementations must make append_history atomic. A retry of the exact record
 * at its already committed sequence returns AlreadyPresent even after the feed
 * head has moved. Range snapshots are compact references; hydrate_history
 * materializes their exact canonical JSONL only when a caller asks for it.
 * Hydration must reject a range whose first predecessor does not match the
 * preceding persisted record when the range begins after sequence one.
 */
class NEOGRAPH_API ContextStore {
public:
    virtual ~ContextStore() = default;

    virtual ContextStoreAppendResult append_history(
        const ContextStoreFeed& feed,
        const RuntimeHistoryRecord& record,
        const std::optional<std::string>& expected_head_id) = 0;
    virtual ContextStoreHead history_head(const ContextStoreFeed& feed) const = 0;
    virtual ContextHistoryRange snapshot_history(
        const ContextStoreFeed& feed,
        std::uint64_t from_sequence,
        std::uint64_t through_sequence) const = 0;
    virtual std::string hydrate_history(const ContextHistoryRange& range) const = 0;

    virtual ContextArtifactPutResult put_artifact(
        std::string_view owner_id,
        const ContextArtifact& artifact) = 0;
    virtual std::optional<ContextArtifact> get_artifact(
        std::string_view owner_id,
        std::string_view artifact_id) const = 0;
};

/** Thread-safe in-memory implementation of the ContextStore durable contract. */
class NEOGRAPH_API InMemoryContextStore final : public ContextStore {
public:
    static constexpr std::size_t MAX_OWNER_ID_BYTES = 256;
    static constexpr std::size_t MAX_FEED_ID_BYTES = 256;
    static constexpr std::size_t MAX_RECORD_BYTES = 1024 * 1024;
    static constexpr std::size_t MAX_ARTIFACT_BYTES = 1024 * 1024;
    static constexpr std::size_t MAX_RANGE_RECORDS = 4096;
    static constexpr std::size_t MAX_RANGE_BYTES = 8 * 1024 * 1024;

    InMemoryContextStore();
    ~InMemoryContextStore() override;
    InMemoryContextStore(InMemoryContextStore&&) noexcept;
    InMemoryContextStore& operator=(InMemoryContextStore&&) noexcept;
    InMemoryContextStore(const InMemoryContextStore&) = delete;
    InMemoryContextStore& operator=(const InMemoryContextStore&) = delete;

    ContextStoreAppendResult append_history(
        const ContextStoreFeed& feed,
        const RuntimeHistoryRecord& record,
        const std::optional<std::string>& expected_head_id) override;
    ContextStoreHead history_head(const ContextStoreFeed& feed) const override;
    ContextHistoryRange snapshot_history(
        const ContextStoreFeed& feed,
        std::uint64_t from_sequence,
        std::uint64_t through_sequence) const override;
    std::string hydrate_history(const ContextHistoryRange& range) const override;
    ContextArtifactPutResult put_artifact(
        std::string_view owner_id,
        const ContextArtifact& artifact) override;
    std::optional<ContextArtifact> get_artifact(
        std::string_view owner_id,
        std::string_view artifact_id) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph
