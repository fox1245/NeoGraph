#include <neograph/graph/node_cache.h>

#include <cstdint>
#include <string>
#include <utility>

namespace neograph::graph {

void NodeCache::set_enabled(const std::string& node_name, bool enabled) {
    std::lock_guard<std::mutex> lock(mu_);
    if (enabled) enabled_nodes_.insert(node_name);
    else         enabled_nodes_.erase(node_name);
}

bool NodeCache::is_enabled(const std::string& node_name) const {
    std::lock_guard<std::mutex> lock(mu_);
    return enabled_nodes_.count(node_name) != 0;
}

void NodeCache::set_policy(const std::string& node_name, CacheKeyPolicy policy) {
    std::lock_guard<std::mutex> lock(mu_);
    policies_[node_name] = std::move(policy);
}

CacheKeyPolicy NodeCache::policy_for(const std::string& node_name) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = policies_.find(node_name);
    return it == policies_.end() ? CacheKeyPolicy{} : it->second;
}

std::string NodeCache::make_key(const std::string& node_name,
                                 const std::string& state_hash,
                                 const std::string& scope_key) {
    std::string k;
    k.reserve(node_name.size() + state_hash.size() + scope_key.size() + 32);
    for (const auto* part : {&node_name, &scope_key, &state_hash}) {
        k.append(std::to_string(part->size()));
        k.push_back(':');
        k.append(*part);
    }
    return k;
}

std::optional<NodeResult> NodeCache::lookup(const std::string& node_name,
                                             const std::string& state_hash) const {
    return lookup(node_name, state_hash, {});
}

std::optional<NodeResult> NodeCache::lookup(const std::string& node_name,
                                             const std::string& state_hash,
                                             const std::string& scope_key) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (enabled_nodes_.count(node_name) == 0) return std::nullopt;
    auto it = entries_.find(make_key(node_name, state_hash, scope_key));
    if (it == entries_.end()) {
        ++misses_;
        return std::nullopt;
    }
    ++hits_;
    return it->second;
}

void NodeCache::store(const std::string& node_name,
                      const std::string& state_hash,
                      NodeResult result) {
    store(node_name, state_hash, {}, std::move(result));
}

void NodeCache::store(const std::string& node_name,
                      const std::string& state_hash,
                      const std::string& scope_key,
                      NodeResult result) {
    std::lock_guard<std::mutex> lock(mu_);
    if (enabled_nodes_.count(node_name) == 0) return;
    entries_[make_key(node_name, state_hash, scope_key)] = std::move(result);
}

void NodeCache::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    entries_.clear();
}

std::size_t NodeCache::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return entries_.size();
}

std::size_t NodeCache::hit_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return hits_;
}

std::size_t NodeCache::miss_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return misses_;
}

// FNV-1a 64-bit over the canonical JSON dump. nlohmann::json sorts
// object keys at dump time when given the default settings, so the
// same logical state produces the same string every time.
std::string hash_state_for_cache(const json& state_value) {
    const std::string canon = state_value.dump();
    std::uint64_t h = 0xcbf29ce484222325ULL;
    constexpr std::uint64_t prime = 0x100000001b3ULL;
    for (unsigned char c : canon) {
        h ^= static_cast<std::uint64_t>(c);
        h *= prime;
    }
    // Hex string — fixed-width, easy to debug.
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(h));
    return std::string(buf, 16);
}

} // namespace neograph::graph
