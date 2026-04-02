#include <neograph/graph/store.h>
#include <algorithm>
#include <set>

namespace neograph::graph {

std::string InMemoryStore::make_key(const Namespace& ns, const std::string& key) {
    return ns_to_string(ns) + "/" + key;
}

std::string InMemoryStore::ns_to_string(const Namespace& ns) {
    std::string result;
    for (size_t i = 0; i < ns.size(); ++i) {
        if (i > 0) result += "/";
        result += ns[i];
    }
    return result;
}

bool InMemoryStore::starts_with(const std::string& str, const std::string& prefix) {
    if (prefix.empty()) return true;
    return str.size() >= prefix.size() &&
           str.compare(0, prefix.size(), prefix) == 0;
}

void InMemoryStore::put(const Namespace& ns, const std::string& key, const json& value) {
    std::lock_guard lock(mutex_);
    auto composite = make_key(ns, key);

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    auto it = items_.find(composite);
    if (it != items_.end()) {
        it->second.value = value;
        it->second.updated_at = now;
    } else {
        items_[composite] = StoreItem{ns, key, value, now, now};
    }
}

std::optional<StoreItem> InMemoryStore::get(const Namespace& ns, const std::string& key) const {
    std::lock_guard lock(mutex_);
    auto it = items_.find(make_key(ns, key));
    if (it == items_.end()) return std::nullopt;
    return it->second;
}

std::vector<StoreItem> InMemoryStore::search(const Namespace& ns_prefix, int limit) const {
    std::lock_guard lock(mutex_);
    std::string prefix = ns_to_string(ns_prefix);

    std::vector<StoreItem> results;
    for (const auto& [composite, item] : items_) {
        if (starts_with(ns_to_string(item.ns), prefix)) {
            results.push_back(item);
            if (static_cast<int>(results.size()) >= limit) break;
        }
    }
    return results;
}

void InMemoryStore::delete_item(const Namespace& ns, const std::string& key) {
    std::lock_guard lock(mutex_);
    items_.erase(make_key(ns, key));
}

std::vector<Namespace> InMemoryStore::list_namespaces(const Namespace& prefix) const {
    std::lock_guard lock(mutex_);
    std::string prefix_str = ns_to_string(prefix);

    std::set<std::string> seen;
    std::vector<Namespace> results;

    for (const auto& [composite, item] : items_) {
        auto ns_str = ns_to_string(item.ns);
        if (starts_with(ns_str, prefix_str) && seen.find(ns_str) == seen.end()) {
            seen.insert(ns_str);
            results.push_back(item.ns);
        }
    }
    return results;
}

size_t InMemoryStore::size() const {
    std::lock_guard lock(mutex_);
    return items_.size();
}

} // namespace neograph::graph
