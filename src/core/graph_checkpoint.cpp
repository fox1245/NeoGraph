#include <neograph/graph/checkpoint.h>
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace neograph::graph {

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

void InMemoryCheckpointStore::save(const Checkpoint& cp) {
    std::lock_guard lock(mutex_);
    by_id_[cp.id] = cp;
    by_thread_[cp.thread_id].push_back(cp);
}

std::optional<Checkpoint> InMemoryCheckpointStore::load_latest(
    const std::string& thread_id) {
    std::lock_guard lock(mutex_);
    auto it = by_thread_.find(thread_id);
    if (it == by_thread_.end() || it->second.empty()) return std::nullopt;
    return it->second.back();
}

std::optional<Checkpoint> InMemoryCheckpointStore::load_by_id(
    const std::string& id) {
    std::lock_guard lock(mutex_);
    auto it = by_id_.find(id);
    if (it == by_id_.end()) return std::nullopt;
    return it->second;
}

std::vector<Checkpoint> InMemoryCheckpointStore::list(
    const std::string& thread_id, int limit) {
    std::lock_guard lock(mutex_);
    auto it = by_thread_.find(thread_id);
    if (it == by_thread_.end()) return {};

    auto& vec = it->second;
    int count = std::min(limit, static_cast<int>(vec.size()));

    // Return most recent first
    std::vector<Checkpoint> result(vec.end() - count, vec.end());
    std::reverse(result.begin(), result.end());
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
}

size_t InMemoryCheckpointStore::size() const {
    std::lock_guard lock(mutex_);
    return by_id_.size();
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
