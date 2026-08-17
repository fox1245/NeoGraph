#include <neograph/context_store.h>

#include "canonical_json.h"

#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace neograph {
namespace {

constexpr std::string_view RANGE_PREAMBLE = "NeoGraph Context history range v1";
constexpr std::string_view RANGE_DOMAIN = "context-history-range-jsonl/v1";
constexpr std::string_view RANGE_REFERENCE_DOMAIN = "context-history-range-reference/v1";

void validate_owner_id(std::string_view owner_id) {
    if (owner_id.empty() || owner_id.size() > InMemoryContextStore::MAX_OWNER_ID_BYTES) {
        throw std::invalid_argument("Context store owner_id is empty or exceeds its byte limit");
    }
    detail::validate_token(owner_id, "Context store owner_id");
}

void validate_feed(const ContextStoreFeed& feed) {
    validate_owner_id(feed.owner_id);
    if (feed.feed_id.empty() || feed.feed_id.size() > InMemoryContextStore::MAX_FEED_ID_BYTES) {
        throw std::invalid_argument("Context store feed_id is empty or exceeds its byte limit");
    }
    detail::validate_token(feed.feed_id, "Context store feed_id");
}

std::string range_digest(std::string_view jsonl) {
    return detail::sha256_identity(RANGE_PREAMBLE, RANGE_DOMAIN, jsonl);
}

std::string range_uri(const ContextHistoryRange& range) {
    const json reference{{"owner_id", range.owner_id},
                         {"feed_id", range.feed_id},
                         {"from_sequence", range.from_sequence},
                         {"through_sequence", range.through_sequence},
                         {"digest", range.digest}};
    const auto identity = detail::sha256_identity(
        RANGE_PREAMBLE, RANGE_REFERENCE_DOMAIN, detail::canonical_json_bytes(reference));
    return "neograph://context/history/" + identity.substr(7);
}

void validate_range(const ContextHistoryRange& range) {
    validate_owner_id(range.owner_id);
    validate_feed({range.owner_id, range.feed_id});
    if (range.from_sequence == 0 || range.through_sequence < range.from_sequence ||
        !detail::is_sha256_identity(range.digest) || range.artifact_uri != range_uri(range)) {
        throw std::invalid_argument("Context history range is invalid");
    }
}

}  // namespace

struct InMemoryContextStore::Impl {
    struct Feed {
        std::map<std::uint64_t, std::string> canonical_records;
        std::string head_id;
    };

    mutable std::mutex mutex;
    std::map<std::pair<std::string, std::string>, Feed> feeds;
    std::map<std::pair<std::string, std::string>, std::string> artifacts;
};

InMemoryContextStore::InMemoryContextStore() : impl_(std::make_unique<Impl>()) {}
InMemoryContextStore::~InMemoryContextStore() = default;
InMemoryContextStore::InMemoryContextStore(InMemoryContextStore&&) noexcept = default;
InMemoryContextStore& InMemoryContextStore::operator=(InMemoryContextStore&&) noexcept = default;

ContextStoreAppendResult InMemoryContextStore::append_history(
    const ContextStoreFeed& feed,
    const RuntimeHistoryRecord& record,
    const std::optional<std::string>& expected_head_id) {
    validate_feed(feed);
    if (record.feed_id() != feed.feed_id) {
        throw std::invalid_argument("Runtime history record does not belong to the target feed");
    }
    if (expected_head_id && !detail::is_sha256_identity(*expected_head_id)) {
        throw std::invalid_argument("Context store expected head id is invalid");
    }
    const auto canonical = record.serialize_canonical();
    if (canonical.size() > MAX_RECORD_BYTES) {
        throw std::invalid_argument("Runtime history record exceeds the byte limit");
    }

    std::lock_guard lock(impl_->mutex);
    const auto key = std::make_pair(feed.owner_id, feed.feed_id);
    const auto feed_it = impl_->feeds.find(key);
    if (feed_it != impl_->feeds.end()) {
        const auto existing = feed_it->second.canonical_records.find(record.sequence());
        if (existing != feed_it->second.canonical_records.end()) {
            if (existing->second == canonical) return ContextStoreAppendResult::AlreadyPresent;
            return ContextStoreAppendResult::Conflict;
        }
    }

    const auto stored_size = feed_it == impl_->feeds.end()
                                 ? std::size_t{0}
                                 : feed_it->second.canonical_records.size();
    const std::string stored_head =
        feed_it == impl_->feeds.end() ? std::string{} : feed_it->second.head_id;
    const std::uint64_t expected_sequence = stored_size + 1;
    if (record.sequence() != expected_sequence ||
        (expected_head_id ? *expected_head_id : std::string{}) != stored_head ||
        (record.sequence() == 1 ? record.predecessor_id().has_value()
                                : !record.predecessor_id() || *record.predecessor_id() != stored_head)) {
        return ContextStoreAppendResult::Conflict;
    }
    auto& stored = feed_it == impl_->feeds.end()
                       ? impl_->feeds.emplace(key, Impl::Feed{}).first->second
                       : feed_it->second;
    stored.canonical_records.emplace(record.sequence(), canonical);
    stored.head_id = record.id();
    return ContextStoreAppendResult::Appended;
}

ContextStoreHead InMemoryContextStore::history_head(const ContextStoreFeed& feed) const {
    validate_feed(feed);
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->feeds.find({feed.owner_id, feed.feed_id});
    if (found == impl_->feeds.end()) return {};
    return {static_cast<std::uint64_t>(found->second.canonical_records.size()),
            found->second.head_id.empty() ? std::nullopt
                                          : std::optional<std::string>(found->second.head_id)};
}

ContextHistoryRange InMemoryContextStore::snapshot_history(
    const ContextStoreFeed& feed,
    std::uint64_t from_sequence,
    std::uint64_t through_sequence) const {
    validate_feed(feed);
    if (from_sequence == 0 || through_sequence < from_sequence ||
        through_sequence - from_sequence >= MAX_RANGE_RECORDS) {
        throw std::invalid_argument("Context history range is invalid or exceeds the record limit");
    }
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->feeds.find({feed.owner_id, feed.feed_id});
    if (found == impl_->feeds.end() || through_sequence > found->second.canonical_records.size()) {
        throw std::out_of_range("Context history range is not present");
    }
    std::string jsonl;
    for (std::uint64_t sequence = from_sequence; sequence <= through_sequence; ++sequence) {
        const auto record = found->second.canonical_records.find(sequence);
        if (record == found->second.canonical_records.end() ||
            record->second.size() + (jsonl.empty() ? 0u : 1u) > MAX_RANGE_BYTES - jsonl.size()) {
            throw std::invalid_argument("Context history range exceeds the byte limit");
        }
        if (!jsonl.empty()) jsonl.push_back('\n');
        jsonl += record->second;
    }
    ContextHistoryRange range{feed.owner_id, feed.feed_id, from_sequence, through_sequence,
                              range_digest(jsonl), {}};
    range.artifact_uri = range_uri(range);
    return range;
}

std::string InMemoryContextStore::hydrate_history(const ContextHistoryRange& range) const {
    validate_range(range);
    if (range.through_sequence - range.from_sequence >= MAX_RANGE_RECORDS) {
        throw std::invalid_argument("Context history range exceeds the record limit");
    }
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->feeds.find({range.owner_id, range.feed_id});
    if (found == impl_->feeds.end()) throw std::out_of_range("Context history feed is not present");
    std::string jsonl;
    std::optional<std::string> predecessor;
    if (range.from_sequence > 1) {
        const auto previous = found->second.canonical_records.find(range.from_sequence - 1);
        if (previous == found->second.canonical_records.end()) {
            throw std::invalid_argument("Context history feed has a broken chain");
        }
        predecessor = RuntimeHistoryRecord::parse(previous->second).id();
    }
    for (std::uint64_t sequence = range.from_sequence; sequence <= range.through_sequence; ++sequence) {
        const auto stored = found->second.canonical_records.find(sequence);
        if (stored == found->second.canonical_records.end() ||
            stored->second.size() + (jsonl.empty() ? 0u : 1u) > MAX_RANGE_BYTES - jsonl.size()) {
            throw std::invalid_argument("Context history range cannot be hydrated exactly");
        }
        const auto record = RuntimeHistoryRecord::parse(stored->second);
        if (record.feed_id() != range.feed_id || record.sequence() != sequence ||
            (sequence == 1 && record.predecessor_id()) ||
            (sequence > 1 && (!record.predecessor_id() ||
                               !predecessor || *record.predecessor_id() != *predecessor))) {
            throw std::invalid_argument("Context history feed has a broken chain");
        }
        predecessor = record.id();
        if (!jsonl.empty()) jsonl.push_back('\n');
        jsonl += stored->second;
    }
    if (range_digest(jsonl) != range.digest) {
        throw std::invalid_argument("Context history range digest does not match stored records");
    }
    return jsonl;
}

ContextArtifactPutResult InMemoryContextStore::put_artifact(
    std::string_view owner_id,
    const ContextArtifact& artifact) {
    validate_owner_id(owner_id);
    const auto canonical = artifact.serialize_canonical();
    if (canonical.size() > MAX_ARTIFACT_BYTES) {
        throw std::invalid_argument("Context artifact exceeds the byte limit");
    }
    std::lock_guard lock(impl_->mutex);
    const auto key = std::make_pair(std::string(owner_id), artifact.id());
    const auto found = impl_->artifacts.find(key);
    if (found == impl_->artifacts.end()) {
        impl_->artifacts.emplace(key, canonical);
        return ContextArtifactPutResult::Stored;
    }
    return found->second == canonical ? ContextArtifactPutResult::AlreadyPresent
                                      : ContextArtifactPutResult::Conflict;
}

std::optional<ContextArtifact> InMemoryContextStore::get_artifact(
    std::string_view owner_id,
    std::string_view artifact_id) const {
    validate_owner_id(owner_id);
    if (!detail::is_sha256_identity(artifact_id)) {
        throw std::invalid_argument("Context artifact id is invalid");
    }
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->artifacts.find({std::string(owner_id), std::string(artifact_id)});
    if (found == impl_->artifacts.end()) return std::nullopt;
    return ContextArtifact::parse(found->second);
}

}  // namespace neograph
