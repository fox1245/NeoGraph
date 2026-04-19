#include <neograph/graph/checkpoint.h>
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <stdexcept>

namespace neograph::graph {

// =========================================================================
// CheckpointPhase <-> string
// =========================================================================
const char* to_string(CheckpointPhase phase) {
    switch (phase) {
        case CheckpointPhase::Before:        return "before";
        case CheckpointPhase::After:         return "after";
        case CheckpointPhase::Completed:     return "completed";
        case CheckpointPhase::NodeInterrupt: return "node_interrupt";
        case CheckpointPhase::Updated:       return "updated";
    }
    return "unknown";  // unreachable — all enum values handled
}

CheckpointPhase parse_checkpoint_phase(std::string_view s) {
    if (s == "before")         return CheckpointPhase::Before;
    if (s == "after")          return CheckpointPhase::After;
    if (s == "completed")      return CheckpointPhase::Completed;
    if (s == "node_interrupt") return CheckpointPhase::NodeInterrupt;
    if (s == "updated")        return CheckpointPhase::Updated;
    throw std::invalid_argument(
        "parse_checkpoint_phase: unknown phase '" + std::string(s) + "'");
}

// =========================================================================
// UUID v4 generation
// =========================================================================
std::string Checkpoint::generate_id() {
    static thread_local std::mt19937 gen{std::random_device{}()};
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

    auto r = [&]() { return dist(gen); };

    uint32_t a = r(), b = r(), c = r(), d = r();
    // Set version (4) and variant (10xx)
    b = (b & 0xFFFF0FFF) | 0x00004000;
    c = (c & 0x3FFFFFFF) | 0x80000000;

    std::ostringstream ss;
    ss << std::hex << std::setfill('0')
       << std::setw(8) << a << '-'
       << std::setw(4) << (b >> 16) << '-'
       << std::setw(4) << (b & 0xFFFF) << '-'
       << std::setw(4) << (c >> 16) << '-'
       << std::setw(4) << (c & 0xFFFF)
       << std::setw(8) << d;
    return ss.str();
}

// =========================================================================
// InMemoryCheckpointStore
// =========================================================================
//
// Channel-blob deduplication
// ──────────────────────────
// On `save()` each channel value is moved out of the cp's inline
// `channel_values["channels"][n]["value"]` and into `blobs_`, keyed by
// (thread_id, channel_name, version). The cp itself is stored as a
// shell that retains only `version` per channel. On `load_*()` the
// shell is rehydrated by joining the pointers back with the blob map.
//
// Why this is safe: `Channel::version` is the per-channel monotonic
// counter (assigned from a global write counter, see graph_state.cpp),
// so the same `(thread, channel, version)` triple uniquely identifies
// one value. Duplicate puts at the same key are no-ops.
//
// Why this is helpful: a typical super-step touches only a handful of
// channels; the rest carry over unchanged at the same version. Without
// dedup, every cp would re-store every channel — `O(steps × channels)`
// blobs. With dedup it's `O(distinct (channel, version) pairs)`, which
// in steady state is `O(steps + channels)`.

// The neograph::json wrapper has no in-place erase and `items()` yields
// pairs by value, so split_/join_ rebuild a fresh `channels` object
// rather than mutating in place. The cost is one extra deep copy per cp
// transition; in exchange we keep blob dedup on the persistence path.

Checkpoint InMemoryCheckpointStore::split_blobs_locked(Checkpoint cp) {
    if (!cp.channel_values.is_object()) return cp;
    if (!cp.channel_values.contains("channels")) return cp;
    json channels_in = cp.channel_values["channels"];
    if (!channels_in.is_object()) return cp;

    json shell_channels = json::object();
    for (auto [name, ch] : channels_in.items()) {
        if (!ch.is_object() || !ch.contains("version")) {
            // Unknown shape — pass through verbatim so we don't lose data.
            shell_channels[name] = ch;
            continue;
        }
        uint64_t ver = ch["version"].get<uint64_t>();
        if (ch.contains("value")) {
            auto key = std::make_tuple(cp.thread_id, name, ver);
            // try_emplace: first writer wins, identical re-puts are no-ops.
            // Same (thread, channel, version) implies same value because
            // version is monotonic per write — see graph_state.cpp.
            blobs_.try_emplace(key, ch["value"]);
        }
        json entry = json::object();
        entry["version"] = ver;
        shell_channels[name] = entry;
    }

    json new_cv = json::object();
    new_cv["channels"] = shell_channels;
    if (cp.channel_values.contains("global_version")) {
        new_cv["global_version"] = cp.channel_values["global_version"];
    }
    cp.channel_values = new_cv;
    return cp;
}

Checkpoint InMemoryCheckpointStore::join_blobs_locked(Checkpoint cp) const {
    if (!cp.channel_values.is_object()) return cp;
    if (!cp.channel_values.contains("channels")) return cp;
    json channels_in = cp.channel_values["channels"];
    if (!channels_in.is_object()) return cp;

    json full_channels = json::object();
    for (auto [name, ch] : channels_in.items()) {
        if (!ch.is_object() || !ch.contains("version")) {
            full_channels[name] = ch;
            continue;
        }
        json entry = json::object();
        entry["version"] = ch["version"];
        if (ch.contains("value")) {
            // Already inline (legacy blob never went through split_, or a
            // shape we deliberately preserved) — keep as-is.
            entry["value"] = ch["value"];
        } else {
            uint64_t ver = ch["version"].get<uint64_t>();
            auto key = std::make_tuple(cp.thread_id, name, ver);
            auto it = blobs_.find(key);
            // Defensive: missing blob yields null, never throws. Indicates
            // store corruption (cp shell present, blob evicted) — caller
            // sees a deserializable but stale value rather than a crash.
            entry["value"] = (it != blobs_.end()) ? it->second : json();
        }
        full_channels[name] = entry;
    }

    json new_cv = json::object();
    new_cv["channels"] = full_channels;
    if (cp.channel_values.contains("global_version")) {
        new_cv["global_version"] = cp.channel_values["global_version"];
    }
    cp.channel_values = new_cv;
    return cp;
}

void InMemoryCheckpointStore::save(const Checkpoint& cp) {
    std::lock_guard lock(mutex_);
    Checkpoint shell = split_blobs_locked(cp);
    by_id_[shell.id] = shell;
    by_thread_[shell.thread_id].push_back(std::move(shell));
}

std::optional<Checkpoint> InMemoryCheckpointStore::load_latest(
    const std::string& thread_id) {
    std::lock_guard lock(mutex_);
    auto it = by_thread_.find(thread_id);
    if (it == by_thread_.end() || it->second.empty()) return std::nullopt;
    return join_blobs_locked(it->second.back());
}

std::optional<Checkpoint> InMemoryCheckpointStore::load_by_id(
    const std::string& id) {
    std::lock_guard lock(mutex_);
    auto it = by_id_.find(id);
    if (it == by_id_.end()) return std::nullopt;
    return join_blobs_locked(it->second);
}

std::vector<Checkpoint> InMemoryCheckpointStore::list(
    const std::string& thread_id, int limit) {
    std::lock_guard lock(mutex_);
    auto it = by_thread_.find(thread_id);
    if (it == by_thread_.end()) return {};

    auto& vec = it->second;
    int count = std::min(limit, static_cast<int>(vec.size()));

    // Materialize each shell on the way out so callers always see full
    // inline values, matching pre-dedup behavior.
    std::vector<Checkpoint> result;
    result.reserve(count);
    for (auto rit = vec.rbegin(); rit != vec.rbegin() + count; ++rit) {
        result.push_back(join_blobs_locked(*rit));
    }
    return result;
}

void InMemoryCheckpointStore::delete_thread(const std::string& thread_id) {
    std::lock_guard lock(mutex_);
    auto it = by_thread_.find(thread_id);
    if (it != by_thread_.end()) {
        for (const auto& cp : it->second) {
            by_id_.erase(cp.id);
        }
        by_thread_.erase(it);
    }
    // Drop blobs for the thread. Linear scan; acceptable because
    // delete_thread is administrative, not on the hot path.
    for (auto bit = blobs_.begin(); bit != blobs_.end(); ) {
        if (std::get<0>(bit->first) == thread_id) {
            bit = blobs_.erase(bit);
        } else {
            ++bit;
        }
    }
}

size_t InMemoryCheckpointStore::size() const {
    std::lock_guard lock(mutex_);
    return by_id_.size();
}

size_t InMemoryCheckpointStore::blob_count() const {
    std::lock_guard lock(mutex_);
    return blobs_.size();
}

// =========================================================================
// Pending writes (fine-grained progress log)
// =========================================================================

void InMemoryCheckpointStore::put_writes(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id,
    const PendingWrite& write) {
    std::lock_guard lock(mutex_);
    pending_[{thread_id, parent_checkpoint_id}].push_back(write);
}

std::vector<PendingWrite> InMemoryCheckpointStore::get_writes(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id) {
    std::lock_guard lock(mutex_);
    auto it = pending_.find({thread_id, parent_checkpoint_id});
    if (it == pending_.end()) return {};
    return it->second;
}

void InMemoryCheckpointStore::clear_writes(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id) {
    std::lock_guard lock(mutex_);
    pending_.erase({thread_id, parent_checkpoint_id});
}

size_t InMemoryCheckpointStore::pending_writes_count(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id) const {
    std::lock_guard lock(mutex_);
    auto it = pending_.find({thread_id, parent_checkpoint_id});
    if (it == pending_.end()) return 0;
    return it->second.size();
}

} // namespace neograph::graph
